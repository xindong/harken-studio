#include "ProjectTransaction.h"

#include "ProjectDataPolicy.h"

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

#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace VNX {

namespace {

constexpr int kTransactionVersion = 2;

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

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
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

bool copyFileVerified(const QString& source, const QString& destination,
                      QByteArray* hash, QString* error)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read transaction source file");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create transaction directory");
        return false;
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot write transaction file");
        return false;
    }
    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        const qint64 size = input.read(buffer.data(), buffer.size());
        if (size < 0 || output.write(buffer.constData(), size) != size) {
            if (error) *error = QStringLiteral("Cannot copy transaction file");
            return false;
        }
        sourceHash.addData(buffer.constData(), size);
    }
    if (!output.commit()) {
        if (error) *error = QStringLiteral("Cannot commit transaction file");
        return false;
    }
    if (!QFile::setPermissions(destination, QFile::permissions(source))) {
        if (error) *error = QStringLiteral("Cannot preserve transaction file permissions");
        return false;
    }
#if defined(Q_OS_WIN)
    const QString nativeSource = windowsFilePath(source);
    const QString nativeDestination = windowsFilePath(destination);
    const DWORD sourceAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativeSource.utf16()));
    const DWORD destinationAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativeDestination.utf16()));
    constexpr DWORD preservedAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES
        || destinationAttributes == INVALID_FILE_ATTRIBUTES
        || !SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeDestination.utf16()),
            (destinationAttributes & ~preservedAttributes) | (sourceAttributes & preservedAttributes))) {
        if (error) *error = QStringLiteral("Cannot preserve transaction file attributes");
        return false;
    }
#endif
    const QByteArray expected = sourceHash.result();
    if (fileHash(destination) != expected) {
        if (error) *error = QStringLiteral("Transaction file copy failed verification");
        return false;
    }
    if (hash) *hash = expected;
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

QString physicalPathSpelling(const QString& root, const QString& relative)
{
    QString result;
    for (const QString& segment : relative.split(QLatin1Char('/'))) {
        const QDir directory(QDir(root).filePath(result));
        if (!QFileInfo::exists(directory.filePath(segment))) return {};
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
        if (error) *error = QStringLiteral("Cannot read project transaction record");
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Project transaction record is invalid");
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
                    *error = QStringLiteral("Cannot write project transaction record");
                return false;
            }
            QThread::msleep(kRetryDelayMs);
            continue;
        }
        if (file.write(bytes) == bytes.size() && file.commit())
            return true;
        if (attempt == kMaxAttempts)
            break;
        QThread::msleep(kRetryDelayMs);
    }
    if (error) *error = QStringLiteral("Cannot commit project transaction record");
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

bool currentMatchesKnownState(const QString& path, const QJsonObject& entry)
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
        return !displacedPath.isEmpty()
            && QFileInfo::exists(displacedPath)
            && !oldHash.isEmpty()
            && fileHash(displacedPath) == oldHash;
    }
    const QByteArray current = fileHash(path);
    return (!oldHash.isEmpty() && current == oldHash)
        || (!newHash.isEmpty() && current == newHash);
}

bool currentMatchesOldState(const QString& path, const QJsonObject& entry)
{
    const bool exists = QFileInfo(path).isFile();
    if (!entry.value(QStringLiteral("existed")).toBool())
        return !exists;
    if (!exists)
        return false;
    const QByteArray oldHash = QByteArray::fromHex(
        entry.value(QStringLiteral("oldHash")).toString().toLatin1());
    return !oldHash.isEmpty() && fileHash(path) == oldHash;
}

bool detachTarget(const QString& target, const QString& displaced,
                  const QByteArray& oldHash, bool moveToDisplaced,
                  QString* error)
{
    if (!QFileInfo::exists(target))
        return true;
    if (!moveToDisplaced) {
        if (fileHash(target) != oldHash || !QFile::remove(target)) {
            if (error) *error = QStringLiteral("Project changed or is locked before transaction write");
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(QFileInfo(displaced).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create transaction displacement directory");
        return false;
    }
    if (!QFile::rename(target, displaced)) {
        if (error) *error = QStringLiteral("Project changed or is locked before transaction write");
        return false;
    }
    if (fileHash(displaced) == oldHash)
        return true;
    if (!QFileInfo::exists(target))
        QFile::rename(displaced, target);
    if (error) *error = QStringLiteral("Project changed before transaction write");
    return false;
}

bool currentMatchesPlannedState(const QString& path, const QJsonObject& entry)
{
    const QString operation = entry.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("delete"))
        return !QFileInfo(path).isFile();
    if (operation != QStringLiteral("write"))
        return false;
    const QByteArray expected = QByteArray::fromHex(
        entry.value(QStringLiteral("newHash")).toString().toLatin1());
    return !expected.isEmpty() && QFileInfo(path).isFile() && fileHash(path) == expected;
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
        if (!currentMatchesKnownState(target, entry)) {
            if (error) *error = QStringLiteral("Project changed after the transaction started");
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
    if (QFileInfo::exists(recordPath) && !QFile::remove(recordPath)) {
        if (error) *error = QStringLiteral("Cannot remove project transaction record");
        return false;
    }
    if (QDir(backup).exists() && !QDir(backup).removeRecursively()) {
        if (error) *error = QStringLiteral("Cannot remove project transaction recovery data");
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

    QJsonArray entries;
    qint64 totalBytes = 0;
    for (const QString& relative : transaction->writeSet_) {
        const QFileInfo source(QDir(projectDirectory).filePath(relative));
        if (source.isFile())
            totalBytes += source.size();
    }
    int completedFiles = 0;
    qint64 completedBytes = 0;
    if (progress)
        progress(0, transaction->writeSet_.size(), 0, totalBytes);
    for (const QString& relative : transaction->writeSet_) {
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), relative);
        const QString source = QDir(projectDirectory).filePath(relative);
        const bool exists = QFileInfo(source).isFile();
        entry.insert(QStringLiteral("existed"), exists);
        if (exists) {
            const QString oldPath = physicalPathSpelling(projectDirectory, relative);
            if (!oldPath.isEmpty() && oldPath != relative)
                entry.insert(QStringLiteral("oldPath"), oldPath);
            QByteArray hash;
            if (!copyFileVerified(source,
                                  QDir(transaction->backupDirectory_).filePath(relative),
                                  &hash, error)) {
                QDir(transaction->backupDirectory_).removeRecursively();
                return nullptr;
            }
            entry.insert(QStringLiteral("oldHash"), QString::fromLatin1(hash.toHex()));
            completedBytes += QFileInfo(source).size();
        }
        entry.insert(QStringLiteral("state"), QStringLiteral("prepared"));
        entries.append(entry);
        ++completedFiles;
        if (progress)
            progress(completedFiles, transaction->writeSet_.size(),
                     completedBytes, totalBytes);
    }
    transaction->backupFingerprint_ = entryFingerprint(entries, false);
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
    if (!active_ || !QDir(stagedDirectory).exists()) {
        if (error) *error = QStringLiteral("Project transaction is not ready");
        return false;
    }
    QJsonObject journal = readJson(transactionPath_, error);
    if (!validJournal(journal, error))
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
    qint64 totalBytes = 0;
    for (const QJsonValue& value : entries) {
        const QString relative = value.toObject().value(QStringLiteral("path")).toString();
        const QFileInfo source(QDir(stagedDirectory).filePath(relative));
        if (source.isFile())
            totalBytes += source.size();
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
    for (qsizetype index = 0; index < entries.size(); ++index) {
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
        if (!currentMatchesOldState(target, entry)) {
            if (error) *error = QStringLiteral("Project changed before transaction write");
            return false;
        }
        if (QFileInfo(source).isFile()) {
            const QString snapshot = QDir(backupDirectory_).filePath(
                QStringLiteral(".staged/") + relative);
            QByteArray expectedHash;
            if (!copyFileVerified(source, snapshot, &expectedHash, error))
                return false;
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
        entries.replace(index, entry);
    }
    journal.insert(QStringLiteral("entries"), entries);
    if (!writeJson(transactionPath_, journal, error))
        return false;
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const QString target = QDir(projectDirectory_).filePath(
            entry.value(QStringLiteral("path")).toString());
        if (!currentMatchesOldState(target, entry)) {
            if (error) *error = QStringLiteral("Project changed before transaction write");
            return false;
        }
        if (!entry.value(QStringLiteral("existed")).toBool()
            || (entry.value(QStringLiteral("operation")) == QStringLiteral("write")
                && !entry.contains(QStringLiteral("oldPath"))))
            continue;
        const QByteArray oldHash = QByteArray::fromHex(
            entry.value(QStringLiteral("oldHash")).toString().toLatin1());
        if (!detachTarget(target, entry.value(QStringLiteral("displacedPath")).toString(),
                          oldHash, adjacentBackup, error))
            return false;
    }
    if (!reconcileDirectories(projectDirectory_, directories, false, error))
        return false;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        QJsonObject entry = entries.at(index).toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString target = QDir(projectDirectory_).filePath(relative);
        if (entry.value(QStringLiteral("operation")) == QStringLiteral("write")
            && !currentMatchesPlannedState(target, entry)) {
            const bool replaceExisting = entry.value(QStringLiteral("existed")).toBool()
                && !entry.contains(QStringLiteral("oldPath"))
                && currentMatchesOldState(target, entry);
            if (QFileInfo::exists(target) && !replaceExisting) {
                if (error) *error = QStringLiteral("Project changed during transaction write");
                return false;
            }
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                if (error) *error = QStringLiteral("Cannot create project file directory");
                return false;
            }
            const QString snapshot = QDir(backupDirectory_).filePath(
                QStringLiteral(".staged/") + relative);
            QByteArray installedHash;
            const bool installed = adjacentBackup && !replaceExisting
                ? installFileWithoutReplacement(snapshot, target)
                    || copyFileVerified(snapshot, target, &installedHash, error)
                : copyFileVerified(snapshot, target, &installedHash, error);
            if (!installed || !currentMatchesPlannedState(target, entry)) {
                if (error) *error = QStringLiteral("Staged project file changed during transaction");
                return false;
            }
        }
        entry.insert(QStringLiteral("state"), QStringLiteral("written"));
        entries.replace(index, entry);
        ++completedFiles;
        completedBytes += QFileInfo(QDir(stagedDirectory).filePath(relative)).isFile()
            ? QFileInfo(QDir(stagedDirectory).filePath(relative)).size() : 0;
        if (progress)
            progress(completedFiles, entries.size(), completedBytes, totalBytes);
    }
    journal.insert(QStringLiteral("entries"), entries);
    return writeJson(transactionPath_, journal, error);
}

bool ProjectTransaction::commit(bool keepJournal, QString* error)
{
    if (!active_)
        return true;
    QJsonObject journal = readJson(transactionPath_, error);
    if (!validJournal(journal, error))
        return false;
    QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
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
            if (error) *error = QStringLiteral("Project directories changed before transaction commit");
            return false;
        }
    }
    for (qsizetype index = 0; index < entries.size(); ++index) {
        QJsonObject entry = entries.at(index).toObject();
        const QString target = QDir(projectDirectory_).filePath(
            entry.value(QStringLiteral("path")).toString());
        const QString state = entry.value(QStringLiteral("state")).toString();
        const QString operation = entry.value(QStringLiteral("operation")).toString();
        const bool hasPlannedState = state == QStringLiteral("written")
            && !operation.isEmpty();
        if (hasPlannedState && !currentMatchesPlannedState(target, entry)) {
            if (error) *error = QStringLiteral("Project changed before transaction commit");
            return false;
        }
        if (!hasPlannedState && QFileInfo(target).isFile()) {
            const QByteArray hash = fileHash(target);
            if (hash.isEmpty()) {
                if (error) *error = QStringLiteral("Cannot verify committed project file");
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
