#include "Integrations/IntegrationFileSystem.h"
#include "Integrations/IntegrationProgress.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

#include <filesystem>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <stdio.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace VNH {

bool checkIntegrationDirectoryChanges(const QStringList& destinations, const QStringList& sources, QString* error)
{
    error->clear();
    QStringList failures;
    const auto check = [&](const QString& path, bool renameSource) {
        IntegrationProgress::checkpoint();
        const auto initialFailureCount = failures.size();
        QFileInfo directory(path);
        const auto inspect = [&](const QFileInfo& entry) {
            std::error_code code;
            const auto status = std::filesystem::status(std::filesystem::path(entry.absoluteFilePath().toStdU16String()), code);
            if (code && code != std::errc::no_such_file_or_directory)
                failures.append(QCoreApplication::translate("HarkenHub", "%1\nReason: %2")
                    .arg(QDir::toNativeSeparators(entry.absoluteFilePath()), QString::fromStdString(code.message())));
            return status;
        };
        const auto sourceStatus = inspect(directory);
        if (failures.size() != initialFailureCount) return;
        if (renameSource && !std::filesystem::exists(sourceStatus)) return;
#ifdef Q_OS_WIN
        const DWORD access = renameSource ? DELETE : FILE_ADD_SUBDIRECTORY;
        if (!renameSource) directory.setFile(directory.absolutePath());
#else
        directory.setFile(directory.absolutePath());
#endif
        auto status = inspect(directory);
        while (failures.size() == initialFailureCount && !std::filesystem::exists(status)
               && directory.absoluteFilePath() != directory.absolutePath()) {
            directory.setFile(directory.absolutePath());
            status = inspect(directory);
        }
        if (failures.size() != initialFailureCount) return;
        const QString nativePath = QDir::toNativeSeparators(directory.absoluteFilePath());
        if (!std::filesystem::is_directory(status)) {
            failures.append(QCoreApplication::translate("HarkenHub", "Not a directory: %1").arg(nativePath));
            return;
        }
#ifdef Q_OS_WIN
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return;
        }
        const DWORD code = GetLastError();
        wchar_t message[1024] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, message, 1024, nullptr);
        const QString reason = QString::fromWCharArray(message).trimmed();
#else
        if (::access(QFile::encodeName(directory.absoluteFilePath()).constData(), W_OK | X_OK) == 0) return;
        const QString reason = QString::fromLocal8Bit(std::strerror(errno));
#endif
        failures.append(QCoreApplication::translate("HarkenHub", "%1\nReason: %2").arg(nativePath, reason));
    };
    for (const QString& destination : destinations) check(destination, false);
    for (const QString& source : sources) check(source, true);
    if (failures.isEmpty()) return true;
    failures.removeDuplicates();
    *error = QCoreApplication::translate("HarkenHub",
        "Directories without required create or rename access:\n%1").arg(failures.join(QStringLiteral("\n\n")));
    return false;
}

bool checkIntegrationFilesWritable(const QStringList& paths, QString* error)
{
    error->clear();
    QStringList orderedPaths = paths;
    orderedPaths.removeDuplicates();
    orderedPaths.sort();
    QStringList readOnlyFiles;
    QStringList inaccessibleFiles;
    for (const QString& path : orderedPaths) {
        IntegrationProgress::checkpoint();
        const QFileInfo info(path);
        if (!info.exists()) continue;
        const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());
#ifdef Q_OS_WIN
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
        const bool readOnly = attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_READONLY) != 0 && info.isFile();
#else
        const bool readOnly = info.isFile() && !(info.permissions()
            & (QFileDevice::WriteOwner | QFileDevice::WriteGroup | QFileDevice::WriteOther));
#endif
        if (readOnly) {
            readOnlyFiles.append(nativePath);
            continue;
        }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly))
            inaccessibleFiles.append(QCoreApplication::translate("HarkenHub", "%1\nReason: %2")
                .arg(nativePath, file.errorString()));
    }
    if (readOnlyFiles.isEmpty() && inaccessibleFiles.isEmpty()) return true;
    QStringList sections{QCoreApplication::translate("HarkenHub",
        "Some files required for this SDK operation cannot be changed. No SDK files were changed.")};
    if (!readOnlyFiles.isEmpty())
        sections.append(QCoreApplication::translate("HarkenHub", "Read-only files:\n%1")
            .arg(readOnlyFiles.join(QLatin1Char('\n'))));
    if (!inaccessibleFiles.isEmpty())
        sections.append(QCoreApplication::translate("HarkenHub", "Files that could not be opened for writing:\n%1")
            .arg(inaccessibleFiles.join(QStringLiteral("\n\n"))));
    sections.append(QCoreApplication::translate("HarkenHub",
        "Check out or unlock the listed files if required by version control. Otherwise, check write protection, access permissions and programs using these files, then run the safety check again."));
    *error = sections.join(QStringLiteral("\n\n"));
    return false;
}

bool renameIntegrationDirectory(const QString& source, const QString& target, QString* error)
{
    if (error) error->clear();
#ifdef Q_OS_WIN
    const auto nativePath = [](const QString& path) {
        const QString absolute = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        if (absolute.startsWith(QStringLiteral("\\\\?\\"))) return absolute;
        return absolute.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + absolute.mid(2)
            : QStringLiteral("\\\\?\\") + absolute;
    };
    const QString from = nativePath(source);
    const QString to = nativePath(target);
#else
    const QByteArray from = QFile::encodeName(source);
    const QByteArray to = QFile::encodeName(target);
#endif
    constexpr int retries = 25;
    constexpr unsigned long retryDelayMs = 20;
    for (int attempt = 0; attempt <= retries; ++attempt) {
#ifdef Q_OS_WIN
        if (MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                        reinterpret_cast<LPCWSTR>(to.utf16()), 0)) return true;
        const DWORD code = GetLastError();
        wchar_t message[1024] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, message, 1024, nullptr);
        const QString reason = QString::fromWCharArray(message).trimmed();
#else
#ifdef Q_OS_MACOS
        if (renamex_np(from.constData(), to.constData(), RENAME_EXCL) == 0) return true;
#else
        if (syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD,
                    to.constData(), RENAME_NOREPLACE) == 0) return true;
#endif
        const int code = errno;
        const QString reason = QString::fromLocal8Bit(std::strerror(code));
#endif
        if (attempt == retries && error)
            *error = QStringLiteral("Cannot move directory from %1 to %2 (system error %3: %4).")
                .arg(source, target, QString::number(code), reason);
        if (attempt < retries) QThread::msleep(retryDelayMs);
    }
    return false;
}

bool writeIntegrationFile(const QString& path, const QByteArray& contents, QString* error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create directory for %1").arg(path);
        return false;
    }
    constexpr int retries = 25;
    constexpr unsigned long retryDelayMs = 20;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        {
            QSaveFile file(path);
            file.setDirectWriteFallback(false);
            if (file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit())
                return true;
            if (attempt == retries && error)
                *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        }
        if (attempt < retries) QThread::msleep(retryDelayMs);
    }
    return false;
}

}
