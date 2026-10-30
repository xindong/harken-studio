#include "ProjectTransaction.h"

#include "ProjectDataPolicy.h"
#include "ProjectSaveSnapshot.h"
#include "Utils/FileOperationRetry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace VNX {

namespace {

constexpr int kTransactionVersion = 2;
constexpr qsizetype kParallelVerificationMinimum = 64;
constexpr qsizetype kParallelFileOperationMinimum = 8;

bool validJournal(const QJsonObject& journal, QString* error);

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kPathCaseSensitivity = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCaseSensitivity = Qt::CaseSensitive;
#endif

QString projectDirectoryFor(const QString& input)
{
    const QFileInfo info(input);
    return QDir::cleanPath(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

QString journalPathFor(const QString& projectDirectory)
{
    return projectDirectory + QStringLiteral(".harken-project-transaction.json");
}

QString receiptPathFor(const QString& projectDirectory)
{
    return projectDirectory + QStringLiteral(".harken-upgrade-receipt.json");
}

QString lockPathFor(const QString& projectDirectory)
{
    return projectDirectory + QStringLiteral(".harken-project.lock");
}

QString managedRecoveryRoot()
{
    return QDir(QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("ProjectRecovery"));
}

bool isAdjacentBackup(const QString& projectDirectory,
                      const QString& backupDirectory,
                      const QString& transactionId)
{
    const QFileInfo projectInfo(projectDirectory);
    const QFileInfo backupInfo(backupDirectory);
    return projectInfo.absolutePath() == backupInfo.absolutePath()
        && backupInfo.fileName()
               == projectInfo.fileName() + QStringLiteral(".harken-backup-")
                      + transactionId;
}

bool isManagedBackup(const QString& projectDirectory,
                     const QString& backupDirectory,
                     const QString& transactionId)
{
    const QFileInfo projectInfo(projectDirectory);
    const QFileInfo backupInfo(backupDirectory);
    return QDir::cleanPath(backupInfo.absolutePath()).compare(
               QDir::cleanPath(managedRecoveryRoot()),
               kPathCaseSensitivity) == 0
        && backupInfo.fileName()
               == projectInfo.fileName() + QStringLiteral(".harken-backup-")
                      + transactionId;
}

bool isSha256(const QString& value)
{
    if (value.size() != 64)
        return false;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9')
              || (code >= 'a' && code <= 'f')))
            return false;
    }
    return true;
}

QString fileOperationError(const QString& operation, const QFileDevice& file)
{
    return QStringLiteral("%1 '%2': %3 (Qt file error %4)")
        .arg(operation, file.fileName(), file.errorString())
        .arg(static_cast<int>(file.error()));
}

QByteArray fileHash(const QString& path, QString* error = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot open file for verification"), file);
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[16 * 1024];
    qint64 length;
    while ((length = file.read(buffer, sizeof(buffer))) > 0)
        hash.addData(QByteArrayView(buffer, length));
    if (!file.atEnd()) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot read file for verification"), file);
        return {};
    }
    return hash.result();
}

#if defined(Q_OS_WIN)
QString windowsFilePath(const QString& path)
{
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}
#endif

bool installFileWithoutReplacement(const QString& source, const QString& target)
{
#if defined(Q_OS_WIN)
    const QString nativeSource = windowsFilePath(source);
    const QString nativeTarget = windowsFilePath(target);
    return MoveFileExW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
                       reinterpret_cast<LPCWSTR>(nativeTarget.utf16()),
                       MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    const QByteArray sourceBytes = QFile::encodeName(source);
    const QByteArray targetBytes = QFile::encodeName(target);
    return ::link(sourceBytes.constData(), targetBytes.constData()) == 0;
#endif
}

bool prepareFileDirectories(const QStringList& files, QString* error)
{
    QSet<QString> directories;
    for (const QString& path : files) directories.insert(QFileInfo(path).absolutePath());
    QStringList ordered = directories.values();
    ordered.sort();
    for (const QString& directory : ordered) {
        if (!QDir().mkpath(directory)) {
            if (error) *error = QStringLiteral("Cannot create transaction directory: %1").arg(directory);
            return false;
        }
    }
    return true;
}

bool copyFileVerified(const QString& source, const QString& destination,
                      QByteArray* hash, QString* error)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot read transaction source file"), input);
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create transaction directory: %1")
            .arg(QFileInfo(destination).absolutePath());
        return false;
    }
    QByteArray expected;
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    QString copyError;
    if (!retryFileOperation([&] {
        input.unsetError();
        if (!input.seek(0)) {
            copyError = fileOperationError(QStringLiteral("Cannot rewind transaction source file"), input);
            return false;
        }
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly)) {
            copyError = fileOperationError(QStringLiteral("Cannot open transaction file for writing"), output);
            return false;
        }
        QCryptographicHash sourceHash(QCryptographicHash::Sha256);
        while (!input.atEnd()) {
            const qint64 size = input.read(buffer.data(), buffer.size());
            if (size < 0) {
                copyError = fileOperationError(QStringLiteral("Cannot read transaction source file"), input);
                return false;
            }
            if (output.write(buffer.constData(), size) != size) {
                copyError = fileOperationError(QStringLiteral("Cannot write transaction file"), output);
                return false;
            }
            sourceHash.addData(buffer.constData(), size);
        }
        if (!output.commit()) {
            copyError = fileOperationError(QStringLiteral("Cannot commit transaction file"), output);
            return false;
        }
        expected = sourceHash.result();
        return true;
    })) {
        if (error) *error = copyError;
        return false;
    }
    QFile destinationFile(destination);
    if (!destinationFile.setPermissions(input.permissions())) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot preserve transaction file permissions"), destinationFile);
        return false;
    }
#if defined(Q_OS_WIN)
    const QString nativeSource = windowsFilePath(source);
    const QString nativeDestination = windowsFilePath(destination);
    const DWORD sourceAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativeSource.utf16()));
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD nativeError = GetLastError();
        if (error) *error = QStringLiteral("Cannot read transaction source file attributes '%1' (Win32 error %2)")
            .arg(source).arg(nativeError);
        return false;
    }
    const DWORD destinationAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativeDestination.utf16()));
    if (destinationAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD nativeError = GetLastError();
        if (error) *error = QStringLiteral("Cannot read transaction destination file attributes '%1' (Win32 error %2)")
            .arg(destination).arg(nativeError);
        return false;
    }
    constexpr DWORD preservedAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    if (!SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeDestination.utf16()),
            (destinationAttributes & ~preservedAttributes) | (sourceAttributes & preservedAttributes))) {
        const DWORD nativeError = GetLastError();
        if (error) *error = QStringLiteral("Cannot preserve transaction file attributes '%1' (Win32 error %2)")
            .arg(destination).arg(nativeError);
        return false;
    }
#endif
    const QByteArray actual = fileHash(destination, error);
    if (actual.isEmpty())
        return false;
    if (actual != expected) {
        if (error) *error = QStringLiteral("Transaction file copy failed verification: '%1' -> '%2'")
            .arg(source, destination);
        return false;
    }
    if (hash) *hash = expected;
    return true;
}

bool writeFileVerified(const QByteArray& bytes, const QByteArray& expected,
                       QFileDevice::Permissions permissions,
                       const QString& destination, QString* error)
{
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create transaction directory: %1")
            .arg(QFileInfo(destination).absolutePath());
        return false;
    }
    QString writeError;
    if (!retryFileOperation([&] {
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly)) {
            writeError = fileOperationError(QStringLiteral("Cannot open transaction file for writing"), output);
            return false;
        }
        if (output.write(bytes) != bytes.size()) {
            writeError = fileOperationError(QStringLiteral("Cannot write transaction file"), output);
            return false;
        }
        if (!output.commit()) {
            writeError = fileOperationError(QStringLiteral("Cannot commit transaction file"), output);
            return false;
        }
        return true;
    })) {
        if (error) *error = writeError;
        return false;
    }
#ifndef Q_OS_WIN
    QFile destinationFile(destination);
    if (permissions != QFileDevice::Permissions{} && !destinationFile.setPermissions(permissions)) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot preserve transaction file permissions"), destinationFile);
        return false;
    }
#else
    Q_UNUSED(permissions);
#endif
    const QByteArray actual = fileHash(destination, error);
    if (actual.isEmpty())
        return false;
    if (actual != expected) {
        if (error) *error = QStringLiteral("Transaction file write failed verification: '%1'")
            .arg(destination);
        return false;
    }
    return true;
}

QStringList discoverWriteSet(const QString& projectDirectory,
                             const ProjectDataPolicy& dataPolicy)
{
    QStringList result;
    QDirIterator iterator(projectDirectory,
                          QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    const QDir root(projectDirectory);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString relative = ProjectDataPolicy::normalizeRelativePath(
            root.relativeFilePath(path));
        if (dataPolicy.protectByDefault(relative))
            result.append(relative);
    }
    result.sort();
    result.removeDuplicates();
    return result;
}

QStringList normalizeWriteSet(const QStringList& writeSet)
{
    QStringList result;
    for (const QString& entry : writeSet) {
        const QString path = ProjectDataPolicy::normalizeRelativePath(entry);
        if (!ProjectDataPolicy::isSafeRelativePath(path))
            return {};
        result.append(path);
    }
    result.sort();
    result.removeDuplicates();
    return result;
}

QString retainedFileKey(const QString& path)
{
    return kPathCaseSensitivity == Qt::CaseInsensitive ? path.toCaseFolded() : path;
}

class DirectorySpellingSnapshot
{
public:
    QString resolve(const QDir& directory, const QString& segment)
    {
        auto found = directories_.find(directory.absolutePath());
        if (found == directories_.end()) {
            Listing listing;
            listing.entries = directory.entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            listing.names = QSet<QString>(listing.entries.cbegin(), listing.entries.cend());
            found = directories_.insert(directory.absolutePath(), std::move(listing));
        }
        QString spelling;
        if (found->names.contains(segment)) {
            spelling = segment;
        } else {
            const auto entry = std::find_if(found->entries.cbegin(), found->entries.cend(),
                [&](const QString& name) { return name.compare(segment, kPathCaseSensitivity) == 0; });
            if (entry == found->entries.cend()) return {};
            spelling = *entry;
        }
        found->used.insert(spelling);
        return spelling;
    }

    bool matches() const
    {
        for (auto directory = directories_.cbegin(); directory != directories_.cend(); ++directory) {
            const QStringList entries = QDir(directory.key()).entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            const QSet<QString> names(entries.cbegin(), entries.cend());
            for (const QString& used : directory->used)
                if (!names.contains(used)) return false;
        }
        return true;
    }

private:
    struct Listing {
        QStringList entries;
        QSet<QString> names;
        QSet<QString> used;
    };
    QHash<QString, Listing> directories_;
};

QString physicalPathSpelling(const QString& root, const QString& relative,
                             DirectorySpellingSnapshot* snapshot = nullptr)
{
    QString result;
    for (const QString& segment : relative.split(QLatin1Char('/'))) {
        const QDir directory(QDir(root).filePath(result));
        if (!QFileInfo::exists(directory.filePath(segment))) return {};
        if (snapshot) {
            const QString spelling = snapshot->resolve(directory, segment);
            if (spelling.isEmpty()) return {};
            if (!result.isEmpty()) result += QLatin1Char('/');
            result += spelling;
            continue;
        }
        const QStringList entries = directory.entryList(
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        auto found = std::find(entries.cbegin(), entries.cend(), segment);
        if (found == entries.cend())
            found = std::find_if(entries.cbegin(), entries.cend(), [&](const QString& entry) {
                return entry.compare(segment, kPathCaseSensitivity) == 0;
            });
        if (found == entries.cend()) return {};
        if (!result.isEmpty()) result += QLatin1Char('/');
        result += *found;
    }
    return result;
}

QJsonObject readJson(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot open project transaction record"), file);
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot read project transaction record"), file);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Project transaction record is invalid '%1': %2")
            .arg(path, parseError.errorString());
        return {};
    }
    const QJsonObject object = document.object();
    if (!validJournal(object, error)) {
        return {};
    }
    return object;
}

bool writeJson(const QString& path, const QJsonObject& object, QString* error)
{
    if (!validJournal(object, error)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 50;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (attempt == kMaxAttempts) {
                if (error)
                    *error = fileOperationError(QStringLiteral("Cannot open project transaction record for writing"), file);
                return false;
            }
            QThread::msleep(kRetryDelayMs);
            continue;
        }
        const bool written = file.write(bytes) == bytes.size();
        if (written && file.commit())
            return true;
        if (attempt == kMaxAttempts) {
            if (error) *error = fileOperationError(written
                ? QStringLiteral("Cannot commit project transaction record")
                : QStringLiteral("Cannot write project transaction record"), file);
            return false;
        }
        QThread::msleep(kRetryDelayMs);
    }
    return false;
}

QByteArray entryFingerprint(const QJsonArray& entries, bool committed,
                           const QJsonArray& directories = {})
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const QString path = !committed && entry.contains(QStringLiteral("oldPath"))
            ? entry.value(QStringLiteral("oldPath")).toString()
            : entry.value(QStringLiteral("path")).toString();
        const QString digest = committed
            ? entry.value(QStringLiteral("newHash")).toString()
            : entry.value(QStringLiteral("oldHash")).toString();
        hash.addData(path.toUtf8());
        hash.addData(separator);
        hash.addData(digest.toLatin1());
        hash.addData(separator);
    }
    for (const QJsonValue& value : directories) {
        const QJsonObject directory = value.toObject();
        hash.addData(QByteArrayLiteral("directory"));
        hash.addData(separator);
        hash.addData(directory.value(QStringLiteral("path")).toString().toUtf8());
        hash.addData(separator);
        hash.addData(directory.value(committed ? QStringLiteral("retained")
                                              : QStringLiteral("existed")).toBool()
            ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
        hash.addData(separator);
    }
    return hash.result();
}

bool validJournal(const QJsonObject& journal, QString* error)
{
    const QString transactionId = journal.value(
        QStringLiteral("transactionId")).toString();
    const QString journalState = journal.value(QStringLiteral("state")).toString();
    if (journal.value(QStringLiteral("version")).toInt()
            != kTransactionVersion
        || QUuid::fromString(transactionId).isNull()
        || !journal.value(QStringLiteral("entries")).isArray()
        || (journalState != QStringLiteral("prepared")
            && journalState != QStringLiteral("writing")
            && journalState != QStringLiteral("committed"))) {
        if (error) *error = QStringLiteral("Project transaction record is malformed");
        return false;
    }
    const QString projectDirectory = journal.value(QStringLiteral("projectDirectory")).toString();
    const QString backupDirectory = journal.value(QStringLiteral("backupDirectory")).toString();
    const QFileInfo projectInfo(projectDirectory);
    const QFileInfo backupInfo(backupDirectory);
    const bool adjacentBackup = isAdjacentBackup(
        projectDirectory, backupDirectory, transactionId);
    const bool managedBackup = isManagedBackup(
        projectDirectory, backupDirectory, transactionId);
    if (projectDirectory.isEmpty() || backupDirectory.isEmpty()
        || QDir::cleanPath(projectInfo.absoluteFilePath()) != projectDirectory
        || (!adjacentBackup && !managedBackup)
        || backupInfo.isSymLink()) {
        if (error) *error = QStringLiteral("Project transaction paths are invalid");
        return false;
    }
    const QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    QSet<QString> paths;
    for (const QJsonValue& value : entries) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("Project transaction record is malformed");
            return false;
        }
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        const QString state = entry.value(QStringLiteral("state")).toString();
        const QString operation = entry.value(QStringLiteral("operation")).toString();
        const bool existed = entry.value(QStringLiteral("existed")).toBool();
        const QString oldHash = entry.value(QStringLiteral("oldHash")).toString();
        const QString newHash = entry.value(QStringLiteral("newHash")).toString();
        const QString oldPath = entry.value(QStringLiteral("oldPath")).toString();
        if (!ProjectDataPolicy::isSafeRelativePath(path)
            || paths.contains(path)
            || !entry.value(QStringLiteral("existed")).isBool()
            || (entry.contains(QStringLiteral("oldPath"))
                && (!existed || !ProjectDataPolicy::isSafeRelativePath(oldPath)
                    || oldPath.compare(path, kPathCaseSensitivity) != 0))) {
            if (error) *error = QStringLiteral("Project transaction write set is invalid");
            return false;
        }
        paths.insert(path);
        if ((state != QStringLiteral("prepared")
             && state != QStringLiteral("planned")
             && state != QStringLiteral("written")
             && state != QStringLiteral("committed"))
            || (!operation.isEmpty()
                && operation != QStringLiteral("write")
                && operation != QStringLiteral("delete"))
            || (existed ? !isSha256(oldHash) : !oldHash.isEmpty())
            || (operation == QStringLiteral("write")
                    ? !isSha256(newHash)
                    : !newHash.isEmpty())) {
            if (error) *error = QStringLiteral("Project transaction record is malformed");
            return false;
        }
        const QString displacedPath = entry.value(QStringLiteral("displacedPath")).toString();
        if (entry.contains(QStringLiteral("displacedHash"))
            && (!isSha256(entry.value(QStringLiteral("displacedHash")).toString())
                || displacedPath.isEmpty())) {
            if (error) *error = QStringLiteral("Project transaction displacement hash is invalid");
            return false;
        }
        if (!displacedPath.isEmpty()) {
            const QString cleanDisplaced = QDir::cleanPath(
                QFileInfo(displacedPath).absoluteFilePath());
            const QString cleanBackup = QDir::cleanPath(backupInfo.absoluteFilePath());
            const QString relativeDisplaced = QDir::fromNativeSeparators(
                QDir(cleanBackup).relativeFilePath(cleanDisplaced));
            if (relativeDisplaced == QStringLiteral("..")
                || relativeDisplaced.startsWith(QStringLiteral("../"))
                || QDir::isAbsolutePath(relativeDisplaced)) {
                if (error) *error = QStringLiteral("Project transaction displaced path is invalid");
                return false;
            }
        }
    }
    const QJsonArray directories = journal.value(QStringLiteral("directories")).toArray();
    if (journal.contains(QStringLiteral("directories"))
        && !journal.value(QStringLiteral("directories")).isArray()) {
        if (error) *error = QStringLiteral("Project transaction directory set is invalid");
        return false;
    }
    QSet<QString> directoryPaths;
    for (const QJsonValue& value : directories) {
        const QJsonObject directory = value.toObject();
        const QString path = directory.value(QStringLiteral("path")).toString();
        if (!value.isObject() || !ProjectDataPolicy::isSafeRelativePath(path)
            || directoryPaths.contains(path)
            || !directory.value(QStringLiteral("existed")).isBool()
            || !directory.value(QStringLiteral("retained")).isBool()) {
            if (error) *error = QStringLiteral("Project transaction directory set is invalid");
            return false;
        }
        directoryPaths.insert(path);
    }
    const QString backupFingerprint = journal.value(
        QStringLiteral("backupFingerprint")).toString();
    if (!isSha256(backupFingerprint)
        || entryFingerprint(entries, false, directories).toHex() != backupFingerprint.toLatin1()) {
        if (error) *error = QStringLiteral("Project transaction recovery data does not match its record");
        return false;
    }
    const QString committedFingerprint = journal.value(
        QStringLiteral("committedFingerprint")).toString();
    if (journalState == QStringLiteral("committed")) {
        if (!isSha256(committedFingerprint)
            || entryFingerprint(entries, true, directories).toHex()
                   != committedFingerprint.toLatin1()) {
            if (error) *error = QStringLiteral("Project transaction completion data does not match its record");
            return false;
        }
    } else if (!committedFingerprint.isEmpty()) {
        if (error) *error = QStringLiteral("Project transaction record is malformed");
        return false;
    }
    return true;
}

bool currentMatchesKnownState(const QString& path, const QJsonObject& entry,
                              QString* error = nullptr)
{
    const bool exists = QFileInfo(path).isFile();
    const bool existed = entry.value(QStringLiteral("existed")).toBool();
    const QByteArray oldHash = QByteArray::fromHex(
        entry.value(QStringLiteral("oldHash")).toString().toLatin1());
    const QByteArray newHash = QByteArray::fromHex(
        entry.value(QStringLiteral("newHash")).toString().toLatin1());
    if (!exists) {
        if (!existed || entry.value(QStringLiteral("operation")).toString()
                == QStringLiteral("delete"))
            return true;
        const QString displacedPath = entry.value(QStringLiteral("displacedPath")).toString();
        const QByteArray displacedHash = QByteArray::fromHex(entry.value(
            QStringLiteral("displacedHash")).toString().toLatin1());
        return !displacedPath.isEmpty()
            && QFileInfo::exists(displacedPath)
            && !oldHash.isEmpty()
            && fileHash(displacedPath, error) == (displacedHash.isEmpty() ? oldHash : displacedHash);
    }
    const QByteArray current = fileHash(path, error);
    return (!oldHash.isEmpty() && current == oldHash)
        || (!newHash.isEmpty() && current == newHash);
}

bool currentMatchesOldState(const QString& path, const QJsonObject& entry,
                            QString* error = nullptr)
{
    const bool exists = QFileInfo(path).isFile();
    if (!entry.value(QStringLiteral("existed")).toBool())
        return !exists;
    if (!exists)
        return false;
    const QByteArray oldHash = QByteArray::fromHex(
        entry.value(QStringLiteral("oldHash")).toString().toLatin1());
    return !oldHash.isEmpty() && fileHash(path, error) == oldHash;
}

bool detachTarget(const QString& target, const QString& displaced,
                  const QByteArray& oldHash, bool moveToDisplaced,
                  QString* error)
{
    if (!QFileInfo::exists(target))
        return true;
    if (!moveToDisplaced) {
        const QByteArray currentHash = fileHash(target, error);
        if (currentHash.isEmpty())
            return false;
        if (currentHash != oldHash) {
            if (error) *error = QStringLiteral("Project changed before transaction write: %1").arg(target);
            return false;
        }
        QFile file(target);
        if (!file.remove()) {
            if (error) *error = fileOperationError(QStringLiteral("Cannot remove project file before transaction write"), file);
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(QFileInfo(displaced).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create transaction displacement directory: %1")
            .arg(QFileInfo(displaced).absolutePath());
        return false;
    }
    QFile file(target);
    if (!file.rename(displaced)) {
        if (error) *error = fileOperationError(
            QStringLiteral("Cannot move project file to '%1' before transaction write").arg(displaced), file);
        return false;
    }
    QString verificationError;
    if (fileHash(displaced, &verificationError) == oldHash)
        return true;
    if (!QFileInfo::exists(target))
        QFile::rename(displaced, target);
    if (error) *error = verificationError.isEmpty()
        ? QStringLiteral("Project changed before transaction write: %1").arg(displaced)
        : verificationError;
    return false;
}

bool currentMatchesPlannedState(const QString& path, const QJsonObject& entry,
                                QString* error = nullptr)
{
    const QString operation = entry.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("delete"))
        return !QFileInfo(path).isFile();
    if (operation != QStringLiteral("write"))
        return false;
    const QByteArray expected = QByteArray::fromHex(
        entry.value(QStringLiteral("newHash")).toString().toLatin1());
    return !expected.isEmpty() && QFileInfo(path).isFile() && fileHash(path, error) == expected;
}

enum class FileReadMode { Hash, OldState, PlannedState };

struct FileReadRequest {
    QString path;
    QJsonObject entry;
    FileReadMode mode;
};

struct FileReadResult {
    QByteArray hash;
    QString error;
    bool matches = true;
};

struct FileEntryResult {
    QJsonObject entry;
    QString error;
};

QThreadPool* transactionVerificationPool()
{
    static const auto pool = [] {
        auto result = std::make_unique<QThreadPool>();
        result->setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, 8));
        result->setExpiryTimeout(-1);
        return result;
    }();
    return pool.get();
}

QList<FileReadResult> readTransactionFiles(QThreadPool* pool,
                                          const QList<FileReadRequest>& requests)
{
    return QtConcurrent::blockingMapped<QList<FileReadResult>>(pool, requests,
        [](const FileReadRequest& request) {
            FileReadResult result;
            switch (request.mode) {
            case FileReadMode::Hash:
                result.hash = fileHash(request.path, &result.error);
                break;
            case FileReadMode::OldState:
                result.matches = currentMatchesOldState(request.path, request.entry, &result.error);
                break;
            case FileReadMode::PlannedState:
                result.matches = currentMatchesPlannedState(request.path, request.entry, &result.error);
                break;
            }
            return result;
        });
}

bool reconcileDirectories(const QString& projectDirectory, const QJsonArray& directories,
                          bool restoring, QString* error)
{
    QStringList remove;
    QStringList create;
    for (const QJsonValue& value : directories) {
        const QJsonObject directory = value.toObject();
        const QString path = QDir(projectDirectory).filePath(
            directory.value(QStringLiteral("path")).toString());
        if (directory.value(restoring ? QStringLiteral("existed")
                                      : QStringLiteral("retained")).toBool())
            create.append(path);
        else
            remove.append(path);
    }
    std::sort(remove.begin(), remove.end(), [](const QString& left, const QString& right) {
        return left.size() > right.size();
    });
    std::sort(create.begin(), create.end(), [](const QString& left, const QString& right) {
        return left.size() < right.size();
    });
    for (const QString& path : remove) {
        const QFileInfo info(path);
        const QString relative = QDir(projectDirectory).relativeFilePath(path);
        if (info.isSymLink() || (info.isDir()
            && physicalPathSpelling(projectDirectory, relative) == relative
            && !QDir().rmdir(path))) {
            if (error) *error = QStringLiteral("Cannot remove project directory: %1").arg(path);
            return false;
        }
    }
    for (const QString& path : create) {
        if (QFileInfo(path).isSymLink() || !QDir().mkpath(path)) {
            if (error) *error = QStringLiteral("Cannot create project directory: %1").arg(path);
            return false;
        }
    }
    return true;
}

bool restore(const QJsonObject& journal, QString* error,
             ProjectTransactionProgressCallback progress = {})
{
    if (!validJournal(journal, error))
        return false;
    const QString projectDirectory = journal.value(QStringLiteral("projectDirectory")).toString();
    const QString backupDirectory = journal.value(QStringLiteral("backupDirectory")).toString();
    const QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    qint64 totalBytes = 0;
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("existed")).toBool())
            totalBytes += QFileInfo(QDir(backupDirectory).filePath(
                entry.value(QStringLiteral("path")).toString())).size();
    }
    int completedFiles = 0;
    qint64 completedBytes = 0;
    if (progress)
        progress(0, entries.size(), 0, totalBytes);
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString target = QDir(projectDirectory).filePath(relative);
        QString verificationError;
        if (!currentMatchesKnownState(target, entry, &verificationError)) {
            if (error) *error = verificationError.isEmpty()
                ? QStringLiteral("Project changed after the transaction started: %1").arg(target)
                : verificationError;
            return false;
        }
    }
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString target = QDir(projectDirectory).filePath(relative);
        const QString oldPath = entry.value(QStringLiteral("oldPath")).toString();
        const bool changedSpelling = !oldPath.isEmpty()
            && physicalPathSpelling(projectDirectory, relative) != oldPath;
        if ((!entry.value(QStringLiteral("existed")).toBool() || changedSpelling)
            && QFileInfo(target).isFile()
            && !QFile::remove(target)) {
            if (error) *error = QStringLiteral("Cannot remove a file written by the transaction");
            return false;
        }
    }
    if (!reconcileDirectories(projectDirectory,
                              journal.value(QStringLiteral("directories")).toArray(),
                              true, error))
        return false;
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString target = QDir(projectDirectory).filePath(
            entry.value(QStringLiteral("oldPath")).toString(relative));
        if (entry.value(QStringLiteral("existed")).toBool()) {
            if (!currentMatchesOldState(target, entry)) {
                const QString backup = QDir(backupDirectory).filePath(relative);
                QByteArray restoredHash;
                if (!copyFileVerified(backup, target, &restoredHash, error)
                    || restoredHash.toHex()
                        != entry.value(QStringLiteral("oldHash")).toString().toLatin1())
                    return false;
            }
        }
        ++completedFiles;
        completedBytes += QFileInfo(QDir(backupDirectory).filePath(relative)).size();
        if (progress)
            progress(completedFiles, entries.size(), completedBytes, totalBytes);
    }
    return true;
}

bool removeTransactionFiles(const QJsonObject& journal, const QString& recordPath,
                            QString* error)
{
    const QString backup = journal.value(QStringLiteral("backupDirectory")).toString();
    QFile record(recordPath);
    if (record.exists() && !record.remove()) {
        if (error) *error = fileOperationError(QStringLiteral("Cannot remove project transaction record"), record);
        return false;
    }
    if (QDir(backup).exists() && !QDir(backup).removeRecursively()) {
        if (error) *error = QStringLiteral("Cannot remove project transaction recovery data: %1").arg(backup);
        return false;
    }
    return true;
}

}

std::unique_ptr<ProjectTransaction> ProjectTransaction::begin(
    const QString& projectFilePath, QString* error)
{
    return begin(projectFilePath,
                 discoverWriteSet(projectDirectoryFor(projectFilePath),
                                  ProjectDataPolicy::forProjectFile(projectFilePath)),
                 error);
}

std::unique_ptr<ProjectTransaction> ProjectTransaction::begin(
    const QString& projectFilePath, const QStringList& writeSet, QString* error)
{
    return begin(projectFilePath, writeSet, QString(), error);
}

std::unique_ptr<ProjectTransaction> ProjectTransaction::begin(
    const QString& projectFilePath, const QStringList& writeSet,
    const QString& recoveryRoot, QString* error,
    ProjectTransactionProgressCallback progress)
{
    return beginImpl(projectFilePath, writeSet, recoveryRoot, {}, nullptr, error, std::move(progress));
}

std::unique_ptr<ProjectTransaction> ProjectTransaction::beginReplacement(
    const QString& projectFilePath, const QStringList& writeSet,
    const QString& stagedDirectory, QString* error)
{
    if (!QFileInfo(stagedDirectory).isDir() || QFileInfo(stagedDirectory).isSymLink()) {
        if (error) *error = QStringLiteral("Project staging directory is invalid");
        return nullptr;
    }
    return beginImpl(projectFilePath, writeSet, {}, stagedDirectory, nullptr, error, {});
}

std::unique_ptr<ProjectTransaction> ProjectTransaction::beginReplacement(
    const QString& projectFilePath, const QStringList& writeSet,
    const QString& stagedDirectory, const ProjectSaveSnapshot& snapshot, QString* error)
{
    if (!QFileInfo(stagedDirectory).isDir() || QFileInfo(stagedDirectory).isSymLink()) {
        if (error) *error = QStringLiteral("Project staging directory is invalid");
        return nullptr;
    }
    return beginImpl(projectFilePath, writeSet, {}, stagedDirectory, &snapshot, error, {});
}

std::unique_ptr<ProjectTransaction> ProjectTransaction::beginDelta(
    const QString& projectFilePath, const DeltaPlan& plan, QString* error)
{
    const QString projectDirectory = projectDirectoryFor(projectFilePath);
    const auto validPath = [&](const QString& path) {
        if (ProjectDataPolicy::normalizeRelativePath(path) != path
            || !ProjectDataPolicy::isSafeRelativePath(path))
            return false;
        QString ancestor = path;
        while (ancestor != QStringLiteral(".")) {
            if (QFileInfo(QDir(projectDirectory).filePath(ancestor)).isSymLink())
                return false;
            ancestor = QFileInfo(ancestor).path();
        }
        return true;
    };
    QStringList writeSet;
    QSet<QString> mutationKeys;
    const auto addMutation = [&](const QString& path) {
        const QString key = retainedFileKey(path);
        if (!validPath(path) || mutationKeys.contains(key)) return false;
        mutationKeys.insert(key);
        writeSet.append(path);
        return true;
    };
    bool valid = true;
    for (const FileWrite& write : plan.writes)
        valid = addMutation(write.path) && valid;
    for (const QString& path : plan.deletes)
        valid = addMutation(path) && valid;
    for (const FileRename& rename : plan.renames) {
        valid = validPath(rename.source) && rename.source != rename.target && valid;
        valid = addMutation(rename.target) && valid;
        if (rename.source.compare(rename.target, kPathCaseSensitivity) != 0)
            valid = addMutation(rename.source) && valid;
    }
    QSet<QString> readKeys;
    for (const ReadDependency& read : plan.readSet) {
        const QString key = retainedFileKey(read.path);
        valid = validPath(read.path) && !readKeys.contains(key)
            && !mutationKeys.contains(key)
            && (read.exists ? read.hash.size() == 32 : read.hash.isEmpty()) && valid;
        readKeys.insert(key);
    }
    QSet<QString> directoryPaths;
    for (const DirectoryChange& directory : plan.directories) {
        valid = validPath(directory.path) && !directoryPaths.contains(directory.path) && valid;
        directoryPaths.insert(directory.path);
    }
    if (!valid) {
        if (error) *error = QStringLiteral("Project transaction delta plan is invalid");
        return nullptr;
    }
    QSet<QString> removedDirectories;
    for (const DirectoryChange& directory : plan.directories)
        if (!directory.present) removedDirectories.insert(retainedFileKey(directory.path));
    QSet<QString> renamedDirectories;
    for (const DirectoryChange& directory : plan.directories)
        if (directory.present && removedDirectories.contains(retainedFileKey(directory.path)))
            renamedDirectories.insert(directory.path);
    DirectorySpellingSnapshot parentSpellings;
    const auto preserveParents = [&](const QString& path, bool directory) {
        QString result;
        QString requested;
        const QStringList segments = path.split(QLatin1Char('/'));
        for (qsizetype index = 0; index < segments.size(); ++index) {
            if (!result.isEmpty()) result += QLatin1Char('/');
            if (!requested.isEmpty()) requested += QLatin1Char('/');
            requested += segments.at(index);
            QString segment = segments.at(index);
            if ((directory || index + 1 < segments.size())
                && !renamedDirectories.contains(requested)) {
                const QString spelling = physicalPathSpelling(projectDirectory, result + segment, &parentSpellings);
                if (!spelling.isEmpty()) segment = QFileInfo(spelling).fileName();
            }
            result += segment;
        }
        return result;
    };
    DeltaPlan prepared = plan;
    writeSet.clear();
    for (FileWrite& write : prepared.writes) {
        write.path = preserveParents(write.path, false);
        writeSet.append(write.path);
    }
    for (QString& path : prepared.deletes) {
        path = preserveParents(path, false);
        writeSet.append(path);
    }
    for (FileRename& rename : prepared.renames) {
        rename.source = preserveParents(rename.source, false);
        rename.target = preserveParents(rename.target, false);
        writeSet.append(rename.target);
        if (rename.source.compare(rename.target, kPathCaseSensitivity) != 0)
            writeSet.append(rename.source);
    }
    directoryPaths.clear();
    for (DirectoryChange& directory : prepared.directories) {
        directory.path = preserveParents(directory.path, true);
        if (directoryPaths.contains(directory.path)) {
            if (error) *error = QStringLiteral("Project transaction directory plan is ambiguous");
            return nullptr;
        }
        directoryPaths.insert(directory.path);
    }
    for (ReadDependency& read : prepared.readSet)
        read.path = preserveParents(read.path, false);
    return beginImpl(projectFilePath, writeSet, {}, {}, nullptr, error, {}, &prepared);
}

std::unique_ptr<ProjectTransaction> ProjectTransaction::beginImpl(
    const QString& projectFilePath, const QStringList& writeSet,
    const QString& recoveryRoot, const QString& stagedDirectory,
    const ProjectSaveSnapshot* snapshot, QString* error,
    ProjectTransactionProgressCallback progress, const DeltaPlan* delta)
{
    const QString projectDirectory = projectDirectoryFor(projectFilePath);
    if (!QDir(projectDirectory).exists() || QFileInfo(projectDirectory).isSymLink()) {
        if (error) *error = QStringLiteral("Project directory does not exist");
        return nullptr;
    }
    const QStringList normalized = normalizeWriteSet(writeSet);
    if ((!writeSet.isEmpty() && normalized.isEmpty()) || normalized.size() != writeSet.size()) {
        if (error) *error = QStringLiteral("Project transaction write set is invalid");
        return nullptr;
    }
    if (!recoveryRoot.isEmpty()
        && QDir::cleanPath(QFileInfo(recoveryRoot).absoluteFilePath()).compare(
               QDir::cleanPath(QFileInfo(managedRecoveryRoot()).absoluteFilePath()),
               kPathCaseSensitivity) != 0) {
        if (error) *error = QStringLiteral("Project recovery location is invalid");
        return nullptr;
    }

    auto transaction = std::unique_ptr<ProjectTransaction>(new ProjectTransaction);
    if (delta) {
        transaction->delta_ = true;
        transaction->readSet_ = delta->readSet;
        for (const FileWrite& write : delta->writes)
            transaction->stagedFiles_.insert(retainedFileKey(write.path), StagedFile{
                write.bytes, QCryptographicHash::hash(write.bytes, QCryptographicHash::Sha256),
                write.permissions});
        if (normalized.size() >= kParallelFileOperationMinimum
            || normalized.size() + delta->readSet.size() >= kParallelVerificationMinimum)
            transaction->verificationPool_ = transactionVerificationPool();
    }
    if (snapshot) {
        QSet<QString> writeKeys;
        for (const QString& relative : normalized)
            writeKeys.insert(retainedFileKey(relative));
        for (auto file = snapshot->files().cbegin(); file != snapshot->files().cend(); ++file) {
            const QString relative = ProjectDataPolicy::normalizeRelativePath(file.key());
            const QString key = retainedFileKey(relative);
            if (relative != file.key() || !ProjectDataPolicy::isSafeRelativePath(relative)
                || !writeKeys.contains(key) || transaction->stagedFiles_.contains(key)) {
                if (error) *error = QStringLiteral("Project transaction snapshot is invalid");
                return nullptr;
            }
            StagedFile staged{
                file.value(), QCryptographicHash::hash(file.value(), QCryptographicHash::Sha256), {}};
#ifndef Q_OS_WIN
            staged.permissions = snapshot->permissions(file.key());
#endif
            transaction->stagedFiles_.insert(key, std::move(staged));
        }
        const int threads = std::clamp(QThread::idealThreadCount(), 1, 8);
        if (!transaction->stagedFiles_.isEmpty()
            && normalized.size() >= kParallelVerificationMinimum && threads > 1)
            transaction->verificationPool_ = transactionVerificationPool();
    }
    transaction->projectDirectory_ = projectDirectory;
    transaction->writeSet_ = normalized;
    transaction->transactionPath_ = journalPathFor(projectDirectory);
    transaction->lock_ = std::make_unique<QLockFile>(lockPathFor(projectDirectory));
    transaction->lock_->setStaleLockTime(0);
    if (!transaction->lock_->tryLock(0)) {
        if (error) *error = QStringLiteral("PROJECT_IN_USE");
        return nullptr;
    }
    if (QFileInfo::exists(transaction->transactionPath_)) {
        if (error) *error = QStringLiteral("PROJECT_UPGRADE_RECOVERY_REQUIRED");
        return nullptr;
    }
    if (delta) {
        if (!transaction->verifyReadSet(error)) return nullptr;
        for (const FileRename& rename : delta->renames) {
            const QFileInfo source(QDir(projectDirectory).filePath(rename.source));
            const QFileInfo target(QDir(projectDirectory).filePath(rename.target));
            if (!source.isFile() || source.isSymLink()
                || (rename.source.compare(rename.target, kPathCaseSensitivity) != 0
                    && target.exists())) {
                if (error) *error = QStringLiteral("Project transaction rename is invalid: %1 -> %2")
                    .arg(rename.source, rename.target);
                return nullptr;
            }
        }
        QMap<QString, bool> directories;
        for (const DirectoryChange& change : delta->directories)
            directories.insert(change.path, change.present);
        QStringList destinations;
        for (const FileWrite& write : delta->writes) destinations.append(write.path);
        for (const FileRename& rename : delta->renames) destinations.append(rename.target);
        for (const DirectoryChange& change : delta->directories)
            if (change.present) destinations.append(change.path + QStringLiteral("/placeholder"));
        for (const QString& destination : destinations) {
            QString parent = QFileInfo(destination).path();
            while (parent != QStringLiteral(".")) {
                if (directories.contains(parent) && !directories.value(parent)) {
                    if (error) *error = QStringLiteral("Project transaction removes a required directory: %1")
                        .arg(parent);
                    return nullptr;
                }
                if (!directories.contains(parent)) {
                    const QString spelling = physicalPathSpelling(projectDirectory, parent);
                    if (spelling.isEmpty()) directories.insert(parent, true);
                    else if (spelling != parent) {
                        if (error) *error = QStringLiteral("Project transaction directory spelling is ambiguous: %1")
                            .arg(parent);
                        return nullptr;
                    }
                }
                parent = QFileInfo(parent).path();
            }
        }
        for (auto directory = directories.cbegin(); directory != directories.cend(); ++directory) {
            const QFileInfo info(QDir(projectDirectory).filePath(directory.key()));
            const bool existed = info.isDir()
                && physicalPathSpelling(projectDirectory, directory.key()) == directory.key();
            transaction->deltaDirectories_.append(QJsonObject{
                {QStringLiteral("path"), directory.key()},
                {QStringLiteral("existed"), existed},
                {QStringLiteral("retained"), directory.value()}});
        }
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    transaction->backupDirectory_ = recoveryRoot.isEmpty()
        ? projectDirectory + QStringLiteral(".harken-backup-") + token
        : QDir(recoveryRoot).filePath(
              QFileInfo(projectDirectory).fileName()
              + QStringLiteral(".harken-backup-") + token);
    if (!QDir().mkpath(transaction->backupDirectory_)) {
        if (error) *error = QStringLiteral("Cannot create transaction recovery directory");
        return nullptr;
    }

    DirectorySpellingSnapshot spellingSnapshot;
    struct PreparedFile {
        bool exists = false;
        QString oldPath;
        qsizetype readIndex = -1;
    };
    QList<PreparedFile> preparedFiles;
    QList<FileReadResult> preparedReads;
    QList<FileReadRequest> requests;
    QList<qsizetype> indices;
    for (const QString& relative : transaction->writeSet_) {
        const QString source = QDir(projectDirectory).filePath(relative);
        PreparedFile prepared;
        prepared.exists = QFileInfo(source).isFile();
        if (prepared.exists) {
            prepared.oldPath = physicalPathSpelling(projectDirectory, relative,
                stagedDirectory.isEmpty() && !delta ? nullptr : &spellingSnapshot);
            if (transaction->verificationPool_ && !progress && !delta
                && prepared.oldPath == relative
                && transaction->stagedFiles_.contains(retainedFileKey(relative))) {
                prepared.readIndex = requests.size();
                requests.append(FileReadRequest{source, {}, FileReadMode::Hash});
            }
        }
        indices.append(preparedFiles.size());
        preparedFiles.append(std::move(prepared));
    }
    if (!requests.isEmpty())
        preparedReads = readTransactionFiles(transaction->verificationPool_, requests);
    transaction->parallelFileOperations_ = delta && !progress
        && transaction->verificationPool_ && normalized.size() >= kParallelFileOperationMinimum
        && delta->renames.isEmpty() && transaction->deltaDirectories_.isEmpty();
    if (transaction->parallelFileOperations_) {
        QSet<QString> paths;
        for (const QString& path : normalized) paths.insert(retainedFileKey(path));
        for (qsizetype index : std::as_const(indices)) {
            const QString& path = normalized.at(index);
            const QString& oldPath = preparedFiles.at(index).oldPath;
            if (!oldPath.isEmpty() && oldPath != path)
                transaction->parallelFileOperations_ = false;
            QString parent = QFileInfo(path).path();
            while (parent != QStringLiteral(".")) {
                if (paths.contains(retainedFileKey(parent)))
                    transaction->parallelFileOperations_ = false;
                parent = QFileInfo(parent).path();
            }
            if (!transaction->parallelFileOperations_) break;
        }
    }
    QJsonArray entries;
    qint64 totalBytes = 0;
    if (progress) {
        for (const QString& relative : transaction->writeSet_) {
            const QFileInfo source(QDir(projectDirectory).filePath(relative));
            if (source.isFile())
                totalBytes += source.size();
        }
    }
    int completedFiles = 0;
    qint64 completedBytes = 0;
    if (progress)
        progress(0, transaction->writeSet_.size(), 0, totalBytes);
    struct BackupResult {
        QJsonObject entry;
        QByteArray retainedHash;
        QString error;
    };
    const auto prepareFile = [&](qsizetype index) {
        BackupResult result;
        QString* error = &result.error;
        const QString& relative = transaction->writeSet_.at(index);
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), relative);
        const QString source = QDir(projectDirectory).filePath(relative);
        const bool exists = preparedFiles.at(index).exists;
        entry.insert(QStringLiteral("existed"), exists);
        if (exists) {
            const QString oldPath = preparedFiles.at(index).oldPath;
            if (!oldPath.isEmpty() && oldPath != relative)
                entry.insert(QStringLiteral("oldPath"), oldPath);
            QByteArray hash;
            bool retained = false;
            const QString stagedPath = QDir(stagedDirectory).filePath(relative);
            const auto staged = transaction->stagedFiles_.constFind(retainedFileKey(relative));
            if (!stagedDirectory.isEmpty() && oldPath == relative
                && (staged != transaction->stagedFiles_.cend()
                    || (!delta && QFileInfo(stagedPath).isFile()))) {
                const qsizetype readIndex = preparedFiles.at(index).readIndex;
                if (readIndex < 0) {
                    hash = fileHash(source, error);
                } else {
                    const FileReadResult& result = preparedReads.at(readIndex);
                    hash = result.hash;
                    if (hash.isEmpty() && error)
                        *error = result.error;
                }
                const QByteArray stagedHash = staged != transaction->stagedFiles_.cend()
                    ? staged->hash : fileHash(stagedPath, error);
                if (hash.isEmpty() || stagedHash.isEmpty()) return result;
                retained = hash == stagedHash;
            }
            if (!retained && !copyFileVerified(source,
                                  QDir(transaction->backupDirectory_).filePath(relative),
                                  &hash, error)) return result;
            entry.insert(QStringLiteral("oldHash"), QString::fromLatin1(hash.toHex()));
            if (retained) {
                result.retainedHash = hash;
                entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
                entry.insert(QStringLiteral("newHash"), QString::fromLatin1(hash.toHex()));
            }
        }
        entry.insert(QStringLiteral("state"), QStringLiteral("prepared"));
        result.entry = std::move(entry);
        return result;
    };
    QList<BackupResult> backups;
    if (transaction->parallelFileOperations_) {
        QStringList destinations;
        for (qsizetype index : std::as_const(indices))
            if (preparedFiles.at(index).exists)
                destinations.append(QDir(transaction->backupDirectory_).filePath(normalized.at(index)));
        if (!prepareFileDirectories(destinations, error)) {
            QDir(transaction->backupDirectory_).removeRecursively();
            return nullptr;
        }
        backups = QtConcurrent::blockingMapped<QList<BackupResult>>(
            transaction->verificationPool_, indices, prepareFile);
    }
    for (qsizetype index : std::as_const(indices)) {
        const BackupResult result = backups.isEmpty() ? prepareFile(index) : backups.at(index);
        if (result.entry.isEmpty()) {
            if (error) *error = result.error;
            QDir(transaction->backupDirectory_).removeRecursively();
            return nullptr;
        }
        const QString& relative = transaction->writeSet_.at(index);
        if (!result.retainedHash.isEmpty())
            transaction->retainedFiles_.insert(retainedFileKey(relative), result.retainedHash);
        entries.append(result.entry);
        ++completedFiles;
        if (progress) {
            if (preparedFiles.at(index).exists)
                completedBytes += QFileInfo(QDir(projectDirectory).filePath(relative)).size();
            progress(completedFiles, transaction->writeSet_.size(),
                     completedBytes, totalBytes);
        }
    }
    if ((!stagedDirectory.isEmpty() || delta) && !spellingSnapshot.matches()) {
        if (error) *error = QStringLiteral("Project paths changed while preparing the transaction");
        QDir(transaction->backupDirectory_).removeRecursively();
        return nullptr;
    }
    if (delta) {
        for (const FileRename& rename : delta->renames) {
            const QString backupRelative = rename.source.compare(rename.target, kPathCaseSensitivity) == 0
                ? rename.target : rename.source;
            QFile backup(QDir(transaction->backupDirectory_).filePath(backupRelative));
            if (!backup.open(QIODevice::ReadOnly)) {
                if (error) *error = fileOperationError(QStringLiteral("Cannot read renamed project file"), backup);
                QDir(transaction->backupDirectory_).removeRecursively();
                return nullptr;
            }
            const QByteArray bytes = backup.readAll();
            if (backup.error() != QFileDevice::NoError) {
                if (error) *error = fileOperationError(QStringLiteral("Cannot read renamed project file"), backup);
                QDir(transaction->backupDirectory_).removeRecursively();
                return nullptr;
            }
            const QString key = retainedFileKey(rename.target);
            transaction->stagedFiles_.insert(key, StagedFile{
                bytes, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), backup.permissions()});
            transaction->renamedSources_.insert(key, backupRelative);
        }
    }
    transaction->backupFingerprint_ = entryFingerprint(entries, false, transaction->deltaDirectories_);
    QJsonObject journal{
        {QStringLiteral("version"), kTransactionVersion},
        {QStringLiteral("transactionId"), token},
        {QStringLiteral("projectDirectory"), projectDirectory},
        {QStringLiteral("backupDirectory"), transaction->backupDirectory_},
        {QStringLiteral("backupFingerprint"),
         QString::fromLatin1(transaction->backupFingerprint_.toHex())},
        {QStringLiteral("state"), QStringLiteral("prepared")},
        {QStringLiteral("entries"), entries},
    };
    if (delta) journal.insert(QStringLiteral("directories"), transaction->deltaDirectories_);
    if (!writeJson(transaction->transactionPath_, journal, error)) {
        QDir(transaction->backupDirectory_).removeRecursively();
        return nullptr;
    }
    transaction->active_ = true;
    return transaction;
}

ProjectTransaction::~ProjectTransaction()
{
    if (lock_ && lock_->isLocked())
        lock_->unlock();
}

QStringList ProjectTransaction::changedPaths() const
{
    QStringList paths;
    const QDir root(projectDirectory_);
    for (const QString& relative : writeSet_)
        if (!retainedFiles_.contains(retainedFileKey(relative)))
            paths.append(root.filePath(relative));
    for (const QJsonValue& value : deltaDirectories_) {
        const QJsonObject directory = value.toObject();
        if (directory.value(QStringLiteral("existed")) != directory.value(QStringLiteral("retained")))
            paths.append(root.filePath(directory.value(QStringLiteral("path")).toString()));
    }
    return paths;
}

bool ProjectTransaction::verifyReadSet(QString* error) const
{
    return verifyReadSet(projectDirectory_, readSet_, error);
}

bool ProjectTransaction::verifyReadSet(const QString& projectFilePath,
                                     const QList<ReadDependency>& readSet,
                                     QString* error)
{
    if (readSet.isEmpty()) return true;
    const QDir root(projectDirectoryFor(projectFilePath));
    QList<FileReadRequest> requests;
    requests.reserve(readSet.size());
    for (const ReadDependency& read : readSet) {
        if (!ProjectDataPolicy::isSafeRelativePath(read.path)
            || ProjectDataPolicy::normalizeRelativePath(read.path) != read.path
            || (read.exists ? read.hash.size() != 32 : !read.hash.isEmpty())) {
            if (error) *error = QStringLiteral("Project transaction read set is invalid");
            return false;
        }
        const QString path = root.filePath(read.path);
        const QFileInfo info(path);
        if (info.isSymLink() || (read.exists ? !info.isFile() : info.exists())) {
            if (error) *error = QStringLiteral("Project transaction dependency changed: %1").arg(path);
            return false;
        }
        requests.append(FileReadRequest{path, QJsonObject{
            {QStringLiteral("existed"), read.exists},
            {QStringLiteral("oldHash"), QString::fromLatin1(read.hash.toHex())}},
            FileReadMode::OldState});
    }
    const auto results = readTransactionFiles(transactionVerificationPool(), requests);
    for (qsizetype index = 0; index < results.size(); ++index) {
        if (!results.at(index).matches) {
            if (error) *error = results.at(index).error.isEmpty()
                ? QStringLiteral("Project transaction dependency changed: %1").arg(requests.at(index).path)
                : results.at(index).error;
            return false;
        }
    }
    return true;
}

bool ProjectTransaction::applyDelta(QString* error, ProjectTransactionProgressCallback progress)
{
    if (!active_ || !delta_) {
        if (error) *error = QStringLiteral("Project delta transaction is not ready");
        return false;
    }
    QJsonObject journal = readJson(transactionPath_, error);
    if (journal.isEmpty()) return false;
    if (journal.value(QStringLiteral("state")) != QStringLiteral("prepared")) {
        if (error) *error = QStringLiteral("Project delta transaction has already started installation");
        return false;
    }
    for (const QJsonValue& value : deltaDirectories_) {
        const QJsonObject directory = value.toObject();
        const QString relative = directory.value(QStringLiteral("path")).toString();
        const QFileInfo info(QDir(projectDirectory_).filePath(relative));
        const bool exists = info.isDir()
            && physicalPathSpelling(projectDirectory_, relative) == relative;
        if (info.isSymLink() || exists != directory.value(QStringLiteral("existed")).toBool()) {
            if (error) *error = QStringLiteral("Project directory changed before transaction write: %1")
                .arg(info.absoluteFilePath());
            return false;
        }
    }
    return install(std::move(journal), {}, error, std::move(progress));
}

bool ProjectTransaction::replaceFromDirectory(const QString& stagedDirectory,
                                               QString* error,
                                               ProjectTransactionProgressCallback progress)
{
    return replaceFromDirectory(stagedDirectory, {}, error, std::move(progress));
}

bool ProjectTransaction::replaceFromDirectory(const QString& stagedDirectory,
                                               const QStringList& directoryRoots,
                                               QString* error,
                                               ProjectTransactionProgressCallback progress)
{
    if (!active_ || delta_ || !QDir(stagedDirectory).exists()) {
        if (error) *error = QStringLiteral("Project transaction is not ready");
        return false;
    }
    QJsonObject journal = readJson(transactionPath_, error);
    if (journal.isEmpty())
        return false;
    QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    QSet<QString> before;
    QSet<QString> after;
    QMap<QString, QString> preservedRoots;
    const auto collectDirectories = [&](const QString& root, const QString& sourceRelative,
                                        const QString& targetRelative,
                                        QSet<QString>& result) {
        const QString path = QDir(root).filePath(sourceRelative);
        if (QFileInfo(path).isSymLink()) return false;
        if (!QFileInfo(path).isDir()) return true;
        result.insert(targetRelative);
        QDirIterator iterator(path, QDir::Dirs | QDir::Hidden | QDir::System
            | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QFileInfo info(iterator.next());
            if (info.isSymLink()) return false;
            result.insert(targetRelative + QLatin1Char('/')
                + ProjectDataPolicy::normalizeRelativePath(
                    QDir(path).relativeFilePath(info.absoluteFilePath())));
        }
        return true;
    };
    const QStringList roots = normalizeWriteSet(directoryRoots);
    if (roots.size() != directoryRoots.size()) {
        if (error) *error = QStringLiteral("Project transaction directory roots are invalid");
        return false;
    }
    for (const QString& root : roots) {
        QString actualRoot = root;
        QString prefix = root;
        QString suffix;
        while (prefix != QStringLiteral(".")) {
            const QString spelling = physicalPathSpelling(projectDirectory_, prefix);
            if (!spelling.isEmpty()) {
                actualRoot = spelling + suffix;
                break;
            }
            suffix.prepend(QLatin1Char('/') + QFileInfo(prefix).fileName());
            prefix = QFileInfo(prefix).path();
        }
        preservedRoots.insert(root, actualRoot);
        if (!collectDirectories(projectDirectory_, root, actualRoot, before)
            || !collectDirectories(stagedDirectory, root, actualRoot, after)) {
            if (error) *error = QStringLiteral("Project transaction directory contains a symbolic link");
            return false;
        }
        QString parent = QFileInfo(actualRoot).path();
        while (parent != QStringLiteral(".")) {
            if (QFileInfo(QDir(projectDirectory_).filePath(parent)).isDir()) {
                before.insert(parent);
                after.insert(parent);
            }
            if (QFileInfo(QDir(stagedDirectory).filePath(parent)).isDir())
                after.insert(parent);
            parent = QFileInfo(parent).path();
        }
    }
    for (qsizetype index = 0; index < entries.size(); ++index) {
        QJsonObject entry = entries.at(index).toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        for (auto root = preservedRoots.cbegin(); root != preservedRoots.cend(); ++root) {
            if (path.startsWith(root.key() + QLatin1Char('/'), kPathCaseSensitivity)) {
                const QString preserved = root.value() + path.mid(root.key().size());
                entry.insert(QStringLiteral("path"), preserved);
                if (entry.value(QStringLiteral("oldPath")).toString() == preserved)
                    entry.remove(QStringLiteral("oldPath"));
                entries.replace(index, entry);
                break;
            }
        }
    }
    QJsonArray directories;
    QStringList paths = (before | after).values();
    paths.sort();
    for (const QString& path : paths) {
        directories.append(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("existed"), before.contains(path)},
            {QStringLiteral("retained"), after.contains(path)}});
    }
    if (!directories.isEmpty()) {
        journal.insert(QStringLiteral("directories"), directories);
        backupFingerprint_ = entryFingerprint(entries, false, directories);
        journal.insert(QStringLiteral("backupFingerprint"),
                       QString::fromLatin1(backupFingerprint_.toHex()));
    }
    journal.insert(QStringLiteral("entries"), entries);
    return install(std::move(journal), stagedDirectory, error, std::move(progress));
}

bool ProjectTransaction::install(QJsonObject journal, const QString& stagedDirectory,
                                QString* error, ProjectTransactionProgressCallback progress)
{
    QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    const QJsonArray directories = journal.value(QStringLiteral("directories")).toArray();
    qint64 totalBytes = 0;
    if (progress) {
        for (const QJsonValue& value : entries) {
            const QString relative = value.toObject().value(QStringLiteral("path")).toString();
            const auto staged = stagedFiles_.constFind(retainedFileKey(relative));
            if (staged != stagedFiles_.cend()) {
                totalBytes += staged->bytes.size();
            } else {
                const QFileInfo source(QDir(stagedDirectory).filePath(relative));
                if (source.isFile())
                    totalBytes += source.size();
            }
        }
    }
    int completedFiles = 0;
    qint64 completedBytes = 0;
    if (progress)
        progress(0, entries.size(), 0, totalBytes);
    journal.insert(QStringLiteral("state"), QStringLiteral("writing"));
    if (!writeJson(transactionPath_, journal, error))
        return false;
    const bool adjacentBackup = isAdjacentBackup(
        projectDirectory_, backupDirectory_,
        journal.value(QStringLiteral("transactionId")).toString());
    QList<FileReadResult> plannedReads;
    if (verificationPool_ && !progress && !delta_) {
        QList<FileReadRequest> requests;
        for (const QJsonValue& value : std::as_const(entries)) {
            const QJsonObject entry = value.toObject();
            requests.append(FileReadRequest{
                QDir(projectDirectory_).filePath(entry.value(QStringLiteral("path")).toString()),
                entry, FileReadMode::OldState});
        }
        plannedReads = readTransactionFiles(verificationPool_, requests);
    }
    QList<qsizetype> indices;
    for (qsizetype index = 0; index < entries.size(); ++index) indices.append(index);
    const bool parallel = parallelFileOperations_ && !progress;
    const auto stageEntry = [&](qsizetype index) {
        FileEntryResult result;
        QString* error = &result.error;
        QJsonObject entry = entries.at(index).toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString source = QDir(stagedDirectory).filePath(relative);
        const QString target = QDir(projectDirectory_).filePath(relative);
        const bool existed = entry.value(QStringLiteral("existed")).toBool();
        const QString displaced = existed
            ? adjacentBackup
                ? QDir(backupDirectory_).filePath(QStringLiteral(".displaced/") + relative)
                : QDir(backupDirectory_).filePath(relative)
            : QString();
        QString verificationError;
        const bool matches = delta_ || (plannedReads.isEmpty()
            ? currentMatchesOldState(target, entry, &verificationError)
            : plannedReads.at(index).matches);
        if (!plannedReads.isEmpty())
            verificationError = plannedReads.at(index).error;
        if (!matches) {
            if (error) *error = verificationError.isEmpty()
                ? QStringLiteral("Project changed before transaction write: %1").arg(target)
                : verificationError;
            return result;
        }
        const auto retained = retainedFiles_.constFind(retainedFileKey(relative));
        const auto staged = stagedFiles_.constFind(retainedFileKey(relative));
        if (retained != retainedFiles_.cend()) {
            const bool matches = staged != stagedFiles_.cend()
                ? staged->hash == retained.value()
                : QFileInfo(source).isFile() && fileHash(source, error) == retained.value();
            if (!matches) {
                if (error && error->isEmpty())
                    *error = QStringLiteral("Staged project file changed before transaction write: %1").arg(source);
                return result;
            }
            entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
            entry.insert(QStringLiteral("newHash"), QString::fromLatin1(retained.value().toHex()));
        } else if (renamedSources_.contains(retainedFileKey(relative))) {
            const QString snapshot = QDir(backupDirectory_).filePath(
                QStringLiteral(".staged/") + relative);
            QByteArray expectedHash;
            if (!copyFileVerified(QDir(backupDirectory_).filePath(
                    renamedSources_.value(retainedFileKey(relative))), snapshot, &expectedHash, error)
                || expectedHash != staged->hash)
                return result;
            entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
            entry.insert(QStringLiteral("newHash"), QString::fromLatin1(expectedHash.toHex()));
        } else if (staged != stagedFiles_.cend()) {
            const QString snapshot = QDir(backupDirectory_).filePath(
                QStringLiteral(".staged/") + relative);
            if (!writeFileVerified(staged->bytes, staged->hash, staged->permissions, snapshot, error))
                return result;
            entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
            entry.insert(QStringLiteral("newHash"),
                         QString::fromLatin1(staged->hash.toHex()));
        } else if (!delta_ && QFileInfo(source).isFile()) {
            const QString snapshot = QDir(backupDirectory_).filePath(
                QStringLiteral(".staged/") + relative);
            QByteArray expectedHash;
            if (!copyFileVerified(source, snapshot, &expectedHash, error))
                return result;
            entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
            entry.insert(QStringLiteral("newHash"),
                         QString::fromLatin1(expectedHash.toHex()));
        } else {
            entry.insert(QStringLiteral("operation"), QStringLiteral("delete"));
            entry.remove(QStringLiteral("newHash"));
        }
        if (existed)
            entry.insert(QStringLiteral("displacedPath"), displaced);
        else
            entry.remove(QStringLiteral("displacedPath"));
        entry.insert(QStringLiteral("state"), QStringLiteral("planned"));
        result.entry = std::move(entry);
        return result;
    };
    QList<FileEntryResult> stagedEntries;
    if (parallel) {
        QStringList destinations;
        for (const QJsonValue& value : std::as_const(entries)) {
            const QString relative = value.toObject().value(QStringLiteral("path")).toString();
            if (stagedFiles_.contains(retainedFileKey(relative)))
                destinations.append(QDir(backupDirectory_).filePath(QStringLiteral(".staged/") + relative));
        }
        if (!prepareFileDirectories(destinations, error)) return false;
        stagedEntries = QtConcurrent::blockingMapped<QList<FileEntryResult>>(
            verificationPool_, indices, stageEntry);
    }
    for (qsizetype index : std::as_const(indices)) {
        const FileEntryResult result = stagedEntries.isEmpty() ? stageEntry(index) : stagedEntries.at(index);
        if (result.entry.isEmpty()) {
            if (error) *error = result.error;
            return false;
        }
        entries.replace(index, result.entry);
    }
    journal.insert(QStringLiteral("entries"), entries);
    if (!writeJson(transactionPath_, journal, error))
        return false;
    const auto requiresDetach = [](const QJsonObject& entry) {
        return entry.value(QStringLiteral("existed")).toBool()
            && (entry.value(QStringLiteral("operation")) != QStringLiteral("write")
                || entry.contains(QStringLiteral("oldPath")));
    };
    QList<FileReadResult> detachReads;
    qsizetype detachReadBegin = 0;
    qsizetype detachReadEnd = 0;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        if (verificationPool_ && !progress && !delta_ && index >= detachReadEnd) {
            detachReads.clear();
            detachReadBegin = index;
            detachReadEnd = index;
            while (detachReadEnd < entries.size()
                   && !requiresDetach(entries.at(detachReadEnd).toObject()))
                ++detachReadEnd;
            if (detachReadEnd - detachReadBegin >= kParallelVerificationMinimum) {
                QList<FileReadRequest> requests;
                requests.reserve(detachReadEnd - detachReadBegin);
                for (qsizetype read = detachReadBegin; read < detachReadEnd; ++read) {
                    const QJsonObject entry = entries.at(read).toObject();
                    requests.append(FileReadRequest{QDir(projectDirectory_).filePath(
                        entry.value(QStringLiteral("path")).toString()), entry, FileReadMode::OldState});
                }
                detachReads = readTransactionFiles(verificationPool_, requests);
            }
        }
        QJsonObject entry = entries.at(index).toObject();
        const QString target = QDir(projectDirectory_).filePath(
            entry.value(QStringLiteral("path")).toString());
        QString verificationError;
        const bool matches = delta_ || (detachReads.isEmpty()
            ? currentMatchesOldState(target, entry, &verificationError)
            : detachReads.at(index - detachReadBegin).matches);
        if (!detachReads.isEmpty())
            verificationError = detachReads.at(index - detachReadBegin).error;
        if (!matches) {
            if (error) *error = verificationError.isEmpty()
                ? QStringLiteral("Project changed before transaction write: %1").arg(target)
                : verificationError;
            return false;
        }
        const bool detach = delta_
            ? entry.value(QStringLiteral("operation")) != QStringLiteral("write")
                || entry.contains(QStringLiteral("oldPath"))
            : requiresDetach(entry);
        if (!detach)
            continue;
        QByteArray oldHash = QByteArray::fromHex(
            entry.value(QStringLiteral("oldHash")).toString().toLatin1());
        if (delta_ && QFileInfo::exists(target)) {
            if (!QFileInfo(target).isFile() || QFileInfo(target).isSymLink()) {
                if (error) *error = QStringLiteral("Project transaction target changed kind: %1").arg(target);
                return false;
            }
            oldHash = fileHash(target, error);
            if (oldHash.isEmpty()) return false;
            entry.insert(QStringLiteral("displacedPath"),
                QDir(backupDirectory_).filePath(QStringLiteral(".displaced/")
                    + entry.value(QStringLiteral("path")).toString()));
            entry.insert(QStringLiteral("displacedHash"), QString::fromLatin1(oldHash.toHex()));
            entries.replace(index, entry);
            journal.insert(QStringLiteral("entries"), entries);
            if (!writeJson(transactionPath_, journal, error)) return false;
        }
        if (!detachTarget(target, entry.value(QStringLiteral("displacedPath")).toString(),
                          oldHash, adjacentBackup, error))
            return false;
    }
    if (!reconcileDirectories(projectDirectory_, directories, false, error))
        return false;
    const auto installEntry = [&](QJsonObject entry, bool plannedMatches) {
        FileEntryResult result;
        QString* error = &result.error;
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString target = QDir(projectDirectory_).filePath(relative);
        if (entry.value(QStringLiteral("operation")) == QStringLiteral("write") && !plannedMatches) {
            const bool replaceExisting = delta_
                ? QFileInfo(target).isFile() && !QFileInfo(target).isSymLink()
                : entry.value(QStringLiteral("existed")).toBool()
                    && !entry.contains(QStringLiteral("oldPath"))
                    && currentMatchesOldState(target, entry);
            if (QFileInfo::exists(target) && !replaceExisting) {
                *error = QStringLiteral("Project changed during transaction write: %1").arg(target);
                return result;
            }
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                *error = QStringLiteral("Cannot create project file directory: %1")
                    .arg(QFileInfo(target).absolutePath());
                return result;
            }
            const QString snapshot = QDir(backupDirectory_).filePath(
                QStringLiteral(".staged/") + relative);
            QByteArray installedHash;
            const bool installed = adjacentBackup && !replaceExisting
                ? installFileWithoutReplacement(snapshot, target)
                    || copyFileVerified(snapshot, target, &installedHash, error)
                : copyFileVerified(snapshot, target, &installedHash, error);
            if (!installed) return result;
            QString verificationError;
            if (!currentMatchesPlannedState(target, entry, &verificationError)) {
                *error = verificationError.isEmpty()
                    ? QStringLiteral("Staged project file changed during transaction: %1").arg(target)
                    : verificationError;
                return result;
            }
        }
        entry.insert(QStringLiteral("state"), QStringLiteral("written"));
        result.entry = std::move(entry);
        return result;
    };
    QList<FileEntryResult> installedEntries;
    if (parallel)
        installedEntries = QtConcurrent::blockingMapped<QList<FileEntryResult>>(
            verificationPool_, indices, [&](qsizetype index) {
                const QJsonObject entry = entries.at(index).toObject();
                const QString target = QDir(projectDirectory_).filePath(
                    entry.value(QStringLiteral("path")).toString());
                const bool matches = entry.value(QStringLiteral("operation")) != QStringLiteral("write")
                    || currentMatchesPlannedState(target, entry);
                return installEntry(entry, matches);
            });
    QList<FileReadResult> installedReads;
    qsizetype installedReadBegin = 0;
    qsizetype installedReadEnd = 0;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        if (!parallel && verificationPool_ && !progress && index >= installedReadEnd) {
            installedReads.clear();
            installedReadBegin = index;
            installedReadEnd = index;
            while (installedReadEnd < entries.size()
                   && retainedFiles_.contains(retainedFileKey(entries.at(installedReadEnd)
                       .toObject().value(QStringLiteral("path")).toString())))
                ++installedReadEnd;
            if (installedReadEnd - installedReadBegin >= kParallelVerificationMinimum) {
                QList<FileReadRequest> requests;
                requests.reserve(installedReadEnd - installedReadBegin);
                for (qsizetype read = installedReadBegin; read < installedReadEnd; ++read) {
                    const QJsonObject entry = entries.at(read).toObject();
                    requests.append(FileReadRequest{QDir(projectDirectory_).filePath(
                        entry.value(QStringLiteral("path")).toString()), entry, FileReadMode::PlannedState});
                }
                installedReads = readTransactionFiles(verificationPool_, requests);
            }
        }
        const QJsonObject entry = entries.at(index).toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        FileEntryResult result;
        if (!installedEntries.isEmpty()) {
            result = installedEntries.at(index);
        } else {
            const QString target = QDir(projectDirectory_).filePath(relative);
            const bool matches = entry.value(QStringLiteral("operation")) != QStringLiteral("write")
                || (installedReads.isEmpty() ? currentMatchesPlannedState(target, entry)
                    : installedReads.at(index - installedReadBegin).matches);
            if (!matches) {
                installedReads.clear();
                installedReadEnd = index + 1;
            }
            result = installEntry(entry, matches);
        }
        if (result.entry.isEmpty()) {
            if (error) *error = result.error;
            return false;
        }
        entries.replace(index, result.entry);
        ++completedFiles;
        if (progress) {
            const auto staged = stagedFiles_.constFind(retainedFileKey(relative));
            if (staged != stagedFiles_.cend()) {
                completedBytes += staged->bytes.size();
            } else {
                const QFileInfo source(QDir(stagedDirectory).filePath(relative));
                if (source.isFile())
                    completedBytes += source.size();
            }
            progress(completedFiles, entries.size(), completedBytes, totalBytes);
        }
    }
    journal.insert(QStringLiteral("entries"), entries);
    return writeJson(transactionPath_, journal, error);
}

bool ProjectTransaction::commit(bool keepJournal, QString* error)
{
    if (!active_)
        return true;
    QJsonObject journal = readJson(transactionPath_, error);
    if (journal.isEmpty())
        return false;
    if (!verifyReadSet(error)) return false;
    QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    if (delta_ && (journal.value(QStringLiteral("state")) != QStringLiteral("writing")
        || std::any_of(entries.cbegin(), entries.cend(), [](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("state")) != QStringLiteral("written");
        }))) {
        if (error) *error = QStringLiteral("Project delta transaction has not completed installation");
        return false;
    }
    const QJsonArray directories = journal.value(QStringLiteral("directories")).toArray();
    for (const QJsonValue& value : directories) {
        const QJsonObject directory = value.toObject();
        const QFileInfo info(QDir(projectDirectory_).filePath(
            directory.value(QStringLiteral("path")).toString()));
        const QString relative = directory.value(QStringLiteral("path")).toString();
        const bool exists = info.isDir()
            && physicalPathSpelling(projectDirectory_, relative) == relative;
        if (info.isSymLink()
            || exists != directory.value(QStringLiteral("retained")).toBool()) {
            if (error) *error = QStringLiteral("Project directory changed before transaction commit: %1")
                .arg(info.absoluteFilePath());
            return false;
        }
    }
    QList<FileReadResult> committedReads;
    if (verificationPool_) {
        QList<FileReadRequest> requests;
        bool allWritten = true;
        for (const QJsonValue& value : std::as_const(entries)) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("state")).toString() != QStringLiteral("written")
                || entry.value(QStringLiteral("operation")).toString().isEmpty()) {
                allWritten = false;
                break;
            }
            const QString relative = entry.value(QStringLiteral("path")).toString();
            requests.append(FileReadRequest{QDir(projectDirectory_).filePath(relative), entry,
                retainedFiles_.contains(retainedFileKey(relative))
                    ? FileReadMode::OldState : FileReadMode::PlannedState});
        }
        if (allWritten)
            committedReads = readTransactionFiles(verificationPool_, requests);
    }
    for (qsizetype index = 0; index < entries.size(); ++index) {
        QJsonObject entry = entries.at(index).toObject();
        const QString target = QDir(projectDirectory_).filePath(
            entry.value(QStringLiteral("path")).toString());
        const QString state = entry.value(QStringLiteral("state")).toString();
        const QString operation = entry.value(QStringLiteral("operation")).toString();
        const bool hasPlannedState = state == QStringLiteral("written")
            && !operation.isEmpty();
        const auto retained = retainedFiles_.constFind(
            retainedFileKey(entry.value(QStringLiteral("path")).toString()));
        QString verificationError;
        bool matches = true;
        if (!committedReads.isEmpty()) {
            matches = committedReads.at(index).matches;
            verificationError = committedReads.at(index).error;
        } else if (retained != retainedFiles_.cend()) {
            matches = currentMatchesOldState(target, entry, &verificationError);
        } else if (hasPlannedState) {
            matches = currentMatchesPlannedState(target, entry, &verificationError);
        }
        if (!matches) {
            if (error) *error = verificationError.isEmpty()
                ? QStringLiteral("Project changed before transaction commit: %1").arg(target)
                : verificationError;
            return false;
        }
        if (!hasPlannedState && QFileInfo(target).isFile()) {
            const QByteArray hash = retained == retainedFiles_.cend()
                ? fileHash(target, error) : retained.value();
            if (hash.isEmpty()) {
                return false;
            }
            entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
            entry.insert(QStringLiteral("newHash"), QString::fromLatin1(hash.toHex()));
        } else if (!hasPlannedState) {
            entry.insert(QStringLiteral("operation"), QStringLiteral("delete"));
            entry.remove(QStringLiteral("newHash"));
        }
        entry.insert(QStringLiteral("state"), QStringLiteral("committed"));
        entries.replace(index, entry);
    }
    committedFingerprint_ = entryFingerprint(entries, true, directories);
    journal.insert(QStringLiteral("entries"), entries);
    journal.insert(QStringLiteral("committedFingerprint"),
                   QString::fromLatin1(committedFingerprint_.toHex()));
    journal.insert(QStringLiteral("state"), QStringLiteral("committed"));
    if (!writeJson(transactionPath_, journal, error))
        return false;
    if (!keepJournal && !removeTransactionFiles(journal, transactionPath_, error))
        return false;
    active_ = false;
    return true;
}

bool ProjectTransaction::finalizeToReceipt(const QString& receiptPath, QString* error)
{
    if (!active_ || !commit(true, error))
        return false;
    return finalizeJournal(transactionPath_, receiptPath, error);
}

bool ProjectTransaction::rollback(QString* error,
                                  ProjectTransactionProgressCallback progress)
{
    if (!active_)
        return true;
    const QJsonObject journal = readJson(transactionPath_, error);
    if (journal.isEmpty() || !restore(journal, error, std::move(progress))
        || !removeTransactionFiles(journal, transactionPath_, error))
        return false;
    active_ = false;
    return true;
}

bool ProjectTransaction::recover(const QString& projectFilePath, QString* error,
                                 ProjectTransactionProgressCallback progress)
{
    const QString projectDirectory = projectDirectoryFor(projectFilePath);
    const QString transactionPath = journalPathFor(projectDirectory);
    if (!QFileInfo::exists(transactionPath))
        return true;
    QLockFile lock(lockPathFor(projectDirectory));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (error) *error = QStringLiteral("PROJECT_IN_USE");
        return false;
    }
    const QJsonObject journal = readJson(transactionPath, error);
    if (journal.isEmpty() || !validJournal(journal, error))
        return false;
    if (QDir::cleanPath(journal.value(QStringLiteral("projectDirectory")).toString())
        != projectDirectory) {
        if (error) *error = QStringLiteral("Project transaction targets another project");
        return false;
    }
    if (journal.value(QStringLiteral("state")).toString() == QStringLiteral("committed")) {
        const QString receipt = receiptPathFor(projectDirectory);
        if (QFileInfo::exists(receipt)) {
            const QJsonObject savedReceipt = readJson(receipt, error);
            if (savedReceipt != journal) {
                if (error) *error = QStringLiteral("Project completion receipt does not match");
                return false;
            }
            return QFile::remove(transactionPath) || !QFileInfo::exists(transactionPath);
        }
    }
    return restore(journal, error, std::move(progress))
        && removeTransactionFiles(journal, transactionPath, error);
}

bool ProjectTransaction::recoveryRequired(const QString& projectFilePath)
{
    return QFileInfo::exists(journalPathFor(projectDirectoryFor(projectFilePath)));
}

bool ProjectTransaction::rollbackJournal(const QString& transactionPath, QString* error)
{
    const QJsonObject journal = readJson(transactionPath, error);
    if (journal.isEmpty() || !validJournal(journal, error))
        return false;
    const QString projectDirectory = journal.value(QStringLiteral("projectDirectory")).toString();
    if (QDir::cleanPath(transactionPath) != QDir::cleanPath(journalPathFor(projectDirectory))
        && QDir::cleanPath(transactionPath) != QDir::cleanPath(receiptPathFor(projectDirectory))) {
        if (error) *error = QStringLiteral("Project transaction record path is invalid");
        return false;
    }
    QLockFile lock(lockPathFor(projectDirectory));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (error) *error = QStringLiteral("PROJECT_IN_USE");
        return false;
    }
    return restore(journal, error)
        && removeTransactionFiles(journal, transactionPath, error);
}

bool ProjectTransaction::finalizeJournal(const QString& transactionPath,
                                         const QString& receiptPath,
                                         QString* error)
{
    const QJsonObject journal = readJson(transactionPath, error);
    if (journal.isEmpty() || !validJournal(journal, error)
        || journal.value(QStringLiteral("state")).toString() != QStringLiteral("committed")) {
        if (error && error->isEmpty()) *error = QStringLiteral("Project transaction is not complete");
        return false;
    }
    const QString projectDirectory = journal.value(QStringLiteral("projectDirectory")).toString();
    if (QDir::cleanPath(transactionPath) != QDir::cleanPath(journalPathFor(projectDirectory))
        || QDir::cleanPath(receiptPath) != QDir::cleanPath(receiptPathFor(projectDirectory))) {
        if (error) *error = QStringLiteral("Project completion receipt path is invalid");
        return false;
    }
    QLockFile lock(lockPathFor(projectDirectory));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (error) *error = QStringLiteral("PROJECT_IN_USE");
        return false;
    }
    if (!writeJson(receiptPath, journal, error))
        return false;
    if (!QFile::remove(transactionPath) && QFileInfo::exists(transactionPath)) {
        QFile::remove(receiptPath);
        if (error) *error = QStringLiteral("Cannot finalize project transaction");
        return false;
    }
    return true;
}

bool ProjectTransaction::discardReceipt(const QString& receiptPath, QString* error)
{
    const QJsonObject receipt = readJson(receiptPath, error);
    if (receipt.isEmpty() || !validJournal(receipt, error)
        || receipt.value(QStringLiteral("state")).toString() != QStringLiteral("committed"))
        return false;
    const QString projectDirectory = receipt.value(QStringLiteral("projectDirectory")).toString();
    if (QDir::cleanPath(receiptPath) != QDir::cleanPath(receiptPathFor(projectDirectory))) {
        if (error) *error = QStringLiteral("Project completion receipt path is invalid");
        return false;
    }
    QLockFile lock(lockPathFor(projectDirectory));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (error) *error = QStringLiteral("PROJECT_IN_USE");
        return false;
    }
    return removeTransactionFiles(receipt, receiptPath, error);
}

}
