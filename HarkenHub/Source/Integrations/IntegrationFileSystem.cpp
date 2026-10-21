#include "Integrations/IntegrationFileSystem.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#ifdef Q_OS_MACOS
#include <stdio.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace VNH {

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
