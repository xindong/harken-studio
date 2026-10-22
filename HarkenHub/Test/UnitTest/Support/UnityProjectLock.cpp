#include "UnityProjectLock.h"

#include <QFile>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace HarkenHubTest {

int runUnityProjectLock(const QStringList& arguments)
{
    const QString path = arguments.value(2);
#ifdef Q_OS_WIN
    const HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                   GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
#else
    const int file = open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT, 0600);
    if (file < 0) return 1;
    if (arguments.value(3) == QStringLiteral("record")) {
        struct flock lock {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (fcntl(file, F_SETLK, &lock) != 0) return 2;
    } else if (flock(file, LOCK_EX | LOCK_NB) != 0) {
        return 2;
    }
#endif
    std::fputs("locked\n", stdout);
    std::fflush(stdout);
    std::getchar();
#ifdef Q_OS_WIN
    CloseHandle(file);
#else
    close(file);
#endif
    return 0;
}

}
