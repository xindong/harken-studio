#include "Integrations/Unity/UnityProjectAccess.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VNX {

bool checkUnityProjectClosed(const QString& project, QString* error)
{
    error->clear();
    const QString path = QDir(project).absoluteFilePath(QStringLiteral("Temp/UnityLockfile"));
    const auto inUse = [&]() {
        *error = QStringLiteral("This Unity project is open. Close it before changing the SDK, then run the safety check again.");
        return false;
    };
    const auto failed = [&](int code) {
        *error = QStringLiteral("Cannot check whether this Unity project is open: %1 (system error %2). Check project permissions and try again.")
                     .arg(QDir::toNativeSeparators(path), QString::number(code));
        return false;
    };
#ifdef Q_OS_WIN
    QString nativePath = QDir::toNativeSeparators(QDir::cleanPath(path));
    if (!nativePath.startsWith(QStringLiteral("\\\\?\\")))
        nativePath = nativePath.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + nativePath.mid(2)
            : QStringLiteral("\\\\?\\") + nativePath;
    const HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                                   GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) return inUse();
        return failed(static_cast<int>(code));
    }
    CloseHandle(file);
#else
    const int file = open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file < 0) {
        const int code = errno;
        return code == ENOENT ? true : failed(code);
    }
    const auto closeFile = qScopeGuard([&]() { close(file); });
    struct stat info {};
    if (fstat(file, &info) != 0) return failed(errno);
    if (!S_ISREG(info.st_mode)) return failed(EINVAL);
    struct flock lock {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(file, F_GETLK, &lock) != 0) return failed(errno);
    if (lock.l_type != F_UNLCK) return inUse();
    if (flock(file, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        return code == EWOULDBLOCK || code == EAGAIN ? inUse() : failed(code);
    }
#endif
    return true;
}

}
