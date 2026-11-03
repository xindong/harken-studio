#include "SourceFileState.h"

#include <QDataStream>
#include <QFile>

#if defined(Q_OS_WIN)
#include <QtCore/qt_windows.h>
#include <fcntl.h>
#include <io.h>
#elif defined(Q_OS_MACOS)
#include <sys/mount.h>
#include <sys/stat.h>
#elif defined(Q_OS_LINUX)
#include <sys/stat.h>
#include <sys/vfs.h>
#endif

namespace VNX {

struct FileState
{
    QByteArray identity;
    bool reusable = false;
};

bool openSourceFile(QFile& file, bool attributesOnly)
{
#if defined(Q_OS_WIN)
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(file.fileName().utf16()),
        attributesOnly ? FILE_READ_ATTRIBUTES : GENERIC_READ,
        FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(handle);
        return false;
    }
    if (file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) return true;
    _close(descriptor);
    return false;
#else
    Q_UNUSED(attributesOnly);
    return file.open(QIODevice::ReadOnly);
#endif
}

FileState fileState(const QFile& file, bool source)
{
    FileState result;
    QDataStream stream(&result.identity, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
#if defined(Q_OS_WIN)
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK
        || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
        || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))
        || standard.Directory)
        return {};
    stream << quint64(id.VolumeSerialNumber)
           << QByteArray(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier))
           << qint64(standard.EndOfFile.QuadPart) << qint64(basic.LastWriteTime.QuadPart);
    if (!source) return result;
    stream << qint64(basic.ChangeTime.QuadPart);
    wchar_t fileSystem[32]{};
    wchar_t resolved[512]{};
    const auto length = GetFinalPathNameByHandleW(handle, resolved, 512, VOLUME_NAME_DOS);
    const auto local = length > 0 && length < 512
        && QString::fromWCharArray(resolved, length).startsWith(QStringLiteral("\\\\?\\"))
        && !QString::fromWCharArray(resolved, length).startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive);
    if (local && GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr,
                                              fileSystem, 32)) {
        const auto type = QString::fromWCharArray(fileSystem);
        result.reusable = basic.ChangeTime.QuadPart != 0
            && (type == QStringLiteral("NTFS") || type == QStringLiteral("ReFS"));
    }
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    struct stat info{};
    if (fstat(file.handle(), &info) != 0 || !S_ISREG(info.st_mode)) return {};
#if defined(Q_OS_MACOS)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    stream << quint64(info.st_dev) << quint64(info.st_ino) << qint64(info.st_size)
           << qint64(modified.tv_sec) << qint64(modified.tv_nsec);
    if (!source) return result;
    stream << qint64(changed.tv_sec) << qint64(changed.tv_nsec);
    struct statfs volume{};
    if (fstatfs(file.handle(), &volume) == 0) {
#if defined(Q_OS_MACOS)
        result.reusable = (volume.f_flags & MNT_LOCAL) != 0
            && QByteArray(volume.f_fstypename) == QByteArrayLiteral("apfs");
#else
        const auto type = quint64(volume.f_type);
        result.reusable = changed.tv_nsec != 0
            && (type == 0xEF53 || type == 0x58465342 || type == 0x9123683E
                || type == 0x01021994 || type == 0x2FC12FC1);
#endif
    }
#endif
    return result;
}

QByteArray sourceFileState(const QFile& file, bool* reusable)
{
    const auto state = fileState(file, true);
    if (reusable) *reusable = state.reusable;
    return state.identity;
}

QByteArray outputFileState(const QFile& file)
{
    return fileState(file, false).identity;
}

QByteArray packFileState(const QString& path)
{
    QFile file(path);
    if (!openSourceFile(file, true)) return {};
    return outputFileState(file);
}

}
