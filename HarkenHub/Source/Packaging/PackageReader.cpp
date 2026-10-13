#include "Packaging/PackageReader.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

namespace VNH {

namespace {

constexpr qint64 kMaxTotalUncompressedBytes = 8LL * 1024 * 1024 * 1024;
constexpr int kMaxEntryCount = 100000;

bool isUnixHost(uint16_t versionMadeby)
{
    const uint8_t hostSystem = static_cast<uint8_t>(versionMadeby >> 8);
    return hostSystem == 3 || hostSystem == 19;
}

bool isSymlinkAttributes(uint16_t versionMadeby, uint32_t externalFa)
{
    if (!isUnixHost(versionMadeby))
        return false;
    return ((externalFa >> 16) & 0xF000) == 0xA000;
}

bool validateEntryPath(const QString& rawName, bool* isDirectory, QString* error)
{
    if (rawName.isEmpty()) {
        *error = QStringLiteral("Empty entry name");
        return false;
    }
    if (rawName.contains(QLatin1Char('\\'))) {
        *error = QStringLiteral("Backslash in entry name: %1").arg(rawName);
        return false;
    }
    if (rawName.startsWith(QLatin1Char('/'))) {
        *error = QStringLiteral("Absolute entry path: %1").arg(rawName);
        return false;
    }
    if (rawName.size() >= 2 && rawName.at(1) == QLatin1Char(':')) {
        *error = QStringLiteral("Drive-qualified entry path: %1").arg(rawName);
        return false;
    }

    *isDirectory = rawName.endsWith(QLatin1Char('/'));
    const QString trimmed =
        *isDirectory ? rawName.left(rawName.size() - 1) : rawName;
    if (trimmed.isEmpty()) {
        *error = QStringLiteral("Empty directory entry name");
        return false;
    }

    const QStringList components = trimmed.split(QLatin1Char('/'));
    for (const QString& component : components) {
        if (component.isEmpty()) {
            *error = QStringLiteral("Empty path component in: %1").arg(rawName);
            return false;
        }
        if (component == QStringLiteral(".") || component == QStringLiteral("..")) {
            *error = QStringLiteral("Path traversal component in: %1").arg(rawName);
            return false;
        }
    }
    return true;
}

class ZipReaderHandle
{
public:
    ZipReaderHandle() { handle_ = mz_zip_reader_create(); }
    ~ZipReaderHandle()
    {
        if (handle_) {
            mz_zip_reader_close(handle_);
            mz_zip_reader_delete(&handle_);
        }
    }
    void* get() const { return handle_; }

private:
    void* handle_ = nullptr;
};

}

bool PackageReader::isSafeEntryPath(const QString& rawName, QString* errorOut)
{
    bool isDirectory = false;
    QString error;
    const bool safe = validateEntryPath(rawName, &isDirectory, &error);
    if (!safe && errorOut)
        *errorOut = error;
    return safe;
}

QString PackageReader::fileSha256Hex(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

PackageListResult PackageReader::listEntries(const QString& archivePath)
{
    PackageListResult result;

    ZipReaderHandle reader;
    if (!reader.get()) {
        result.error = QStringLiteral("Cannot create archive reader");
        return result;
    }
    if (mz_zip_reader_open_file(reader.get(), archivePath.toUtf8().constData()) != MZ_OK) {
        result.error = QStringLiteral("Cannot open archive: %1").arg(archivePath);
        return result;
    }

    QSet<QString> seenPaths;
    qint64 totalDeclared = 0;

    int32_t status = mz_zip_reader_goto_first_entry(reader.get());
    while (status == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader.get(), &info) != MZ_OK || !info) {
            result.error = QStringLiteral("Cannot read entry metadata");
            return result;
        }

        const QString rawName = QString::fromUtf8(info->filename);
        bool isDirectory = false;
        QString pathError;
        if (!validateEntryPath(rawName, &isDirectory, &pathError)) {
            result.error = pathError;
            return result;
        }
        if (isSymlinkAttributes(info->version_madeby, info->external_fa)) {
            result.error = QStringLiteral("Symlink entry rejected: %1").arg(rawName);
            return result;
        }

        const QString folded = rawName.toCaseFolded();
        if (seenPaths.contains(folded)) {
            result.error = QStringLiteral("Duplicate entry: %1").arg(rawName);
            return result;
        }
        seenPaths.insert(folded);

        if (result.entries.size() >= kMaxEntryCount) {
            result.error = QStringLiteral("Too many entries in archive");
            return result;
        }
        totalDeclared += static_cast<qint64>(info->uncompressed_size);
        if (totalDeclared > kMaxTotalUncompressedBytes) {
            result.error = QStringLiteral("Archive exceeds the uncompressed size limit");
            return result;
        }

        PackageEntry entry;
        entry.path = rawName;
        entry.uncompressedSize = static_cast<qint64>(info->uncompressed_size);
        entry.isDirectory = isDirectory;
        result.entries.push_back(std::move(entry));

        status = mz_zip_reader_goto_next_entry(reader.get());
    }

    if (status != MZ_END_OF_LIST) {
        result.error = QStringLiteral("Archive iteration failed with code %1").arg(status);
        return result;
    }

    result.ok = true;
    return result;
}

bool PackageReader::readEntry(const QString& archivePath, const QString& entryPath,
                              QByteArray* out, QString* errorOut)
{
    constexpr qint64 kMaxInlineEntryBytes = 1 * 1024 * 1024;

    if (!out) {
        if (errorOut)
            *errorOut = QStringLiteral("No output buffer");
        return false;
    }
    out->clear();

    ZipReaderHandle reader;
    if (!reader.get() ||
        mz_zip_reader_open_file(reader.get(), archivePath.toUtf8().constData()) != MZ_OK) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot open archive: %1").arg(archivePath);
        return false;
    }

    if (mz_zip_reader_locate_entry(reader.get(), entryPath.toUtf8().constData(), 0) !=
        MZ_OK) {
        if (errorOut)
            *errorOut = QStringLiteral("Entry not found: %1").arg(entryPath);
        return false;
    }

    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader.get(), &info) != MZ_OK || !info) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot read entry metadata");
        return false;
    }
    if (static_cast<qint64>(info->uncompressed_size) > kMaxInlineEntryBytes) {
        if (errorOut)
            *errorOut = QStringLiteral("Entry too large to read inline: %1").arg(entryPath);
        return false;
    }

    if (mz_zip_reader_entry_open(reader.get()) != MZ_OK) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot open entry: %1").arg(entryPath);
        return false;
    }

    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    int32_t read = 0;
    while ((read = mz_zip_reader_entry_read(reader.get(), buffer.data(),
                                            buffer.size())) > 0) {
        out->append(buffer.constData(), read);
        if (out->size() > kMaxInlineEntryBytes) {
            mz_zip_reader_entry_close(reader.get());
            if (errorOut)
                *errorOut = QStringLiteral("Entry too large to read inline: %1")
                                .arg(entryPath);
            return false;
        }
    }
    mz_zip_reader_entry_close(reader.get());

    if (read < 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Read failed for entry: %1").arg(entryPath);
        return false;
    }
    return true;
}

bool PackageReader::extractAll(const QString& archivePath, const QString& targetDir,
                               QString* errorOut)
{
    return extractAllVerified(archivePath, targetDir, {}, errorOut);
}

bool PackageReader::extractAllVerified(const QString& archivePath,
                                       const QString& targetDir,
                                       const QHash<QString, QString>& expectedSha256ByPath,
                                       QString* errorOut)
{
    const PackageListResult listing = listEntries(archivePath);
    if (!listing.ok) {
        if (errorOut)
            *errorOut = listing.error;
        return false;
    }

    if (!QDir().mkpath(targetDir)) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot create target directory: %1").arg(targetDir);
        return false;
    }

    ZipReaderHandle reader;
    if (!reader.get() ||
        mz_zip_reader_open_file(reader.get(), archivePath.toUtf8().constData()) != MZ_OK) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot open archive: %1").arg(archivePath);
        return false;
    }

    const QDir target(targetDir);
    qint64 totalWritten = 0;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);

    int32_t status = mz_zip_reader_goto_first_entry(reader.get());
    while (status == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader.get(), &info) != MZ_OK || !info) {
            if (errorOut)
                *errorOut = QStringLiteral("Cannot read entry metadata");
            return false;
        }

        const QString rawName = QString::fromUtf8(info->filename);
        const QString destPath = target.filePath(rawName);

        if (rawName.endsWith(QLatin1Char('/'))) {
            if (!QDir().mkpath(destPath)) {
                if (errorOut)
                    *errorOut = QStringLiteral("Cannot create directory: %1").arg(destPath);
                return false;
            }
        } else {
            if (!QDir().mkpath(QFileInfo(destPath).absolutePath())) {
                if (errorOut)
                    *errorOut = QStringLiteral("Cannot create parent directory for: %1")
                                    .arg(rawName);
                return false;
            }
            if (mz_zip_reader_entry_open(reader.get()) != MZ_OK) {
                if (errorOut)
                    *errorOut = QStringLiteral("Cannot open entry: %1").arg(rawName);
                return false;
            }

            QFile out(destPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                mz_zip_reader_entry_close(reader.get());
                if (errorOut)
                    *errorOut = QStringLiteral("Cannot write file: %1").arg(destPath);
                return false;
            }

            QCryptographicHash entryHash(QCryptographicHash::Sha256);
            qint64 entryWritten = 0;
            int32_t read = 0;
            while ((read = mz_zip_reader_entry_read(reader.get(), buffer.data(),
                                                    buffer.size())) > 0) {
                if (out.write(buffer.constData(), read) != read) {
                    mz_zip_reader_entry_close(reader.get());
                    if (errorOut)
                        *errorOut = QStringLiteral("Write failed: %1").arg(destPath);
                    return false;
                }
                entryHash.addData(QByteArrayView(buffer.constData(), read));
                entryWritten += read;
                totalWritten += read;
                if (totalWritten > kMaxTotalUncompressedBytes) {
                    mz_zip_reader_entry_close(reader.get());
                    if (errorOut)
                        *errorOut =
                            QStringLiteral("Archive exceeds the uncompressed size limit");
                    return false;
                }
            }
            mz_zip_reader_entry_close(reader.get());
            out.close();

#ifndef Q_OS_WIN
            if (isUnixHost(info->version_madeby)) {
                const mode_t recorded =
                    static_cast<mode_t>((info->external_fa >> 16) & 0777);
                const mode_t permissions =
                    recorded == 0 ? recorded : (recorded | 0644);
                if (permissions != 0 &&
                    chmod(QFile::encodeName(destPath).constData(),
                          permissions) != 0) {
                    if (errorOut)
                        *errorOut =
                            QStringLiteral("Cannot restore permissions for: %1")
                                .arg(rawName);
                    return false;
                }
            }
#endif

            if (read < 0) {
                if (errorOut)
                    *errorOut = QStringLiteral("Read failed for entry: %1").arg(rawName);
                return false;
            }
            if (entryWritten != static_cast<qint64>(info->uncompressed_size)) {
                if (errorOut)
                    *errorOut = QStringLiteral(
                                    "Entry size mismatch for %1: declared %2, wrote %3")
                                    .arg(rawName)
                                    .arg(static_cast<qint64>(info->uncompressed_size))
                                    .arg(entryWritten);
                return false;
            }

            if (!expectedSha256ByPath.isEmpty()) {
                const auto expectedIt = expectedSha256ByPath.constFind(rawName);
                if (expectedIt == expectedSha256ByPath.constEnd()) {
                    if (errorOut)
                        *errorOut =
                            QStringLiteral("No expected hash for entry: %1").arg(rawName);
                    return false;
                }
                const QString actual =
                    QString::fromLatin1(entryHash.result().toHex());
                if (actual.compare(expectedIt.value(), Qt::CaseInsensitive) != 0) {
                    if (errorOut)
                        *errorOut =
                            QStringLiteral("Entry SHA-256 mismatch: %1").arg(rawName);
                    return false;
                }
            }
        }

        status = mz_zip_reader_goto_next_entry(reader.get());
    }

    if (status != MZ_END_OF_LIST) {
        if (errorOut)
            *errorOut = QStringLiteral("Archive iteration failed with code %1").arg(status);
        return false;
    }
    return true;
}

}
