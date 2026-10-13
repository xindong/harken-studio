#include "ProjectVolume.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

#ifdef Q_OS_MACOS
#include <sys/mount.h>
#endif

namespace VNX {

namespace {

bool isInside(const QString& path, const QString& root)
{
    if (root.isEmpty())
        return false;
    QString normalizedRoot = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(root).absoluteFilePath()));
    if (!normalizedRoot.endsWith(QLatin1Char('/')))
        normalizedRoot.append(QLatin1Char('/'));
    const QString normalizedPath = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    return normalizedPath.compare(normalizedRoot.left(normalizedRoot.size() - 1),
                                  Qt::CaseInsensitive) == 0
        || normalizedPath.startsWith(normalizedRoot, Qt::CaseInsensitive);
}

bool knownCloudPath(const QString& path)
{
    const QStringList roots{
        qEnvironmentVariable("OneDrive"),
        qEnvironmentVariable("OneDriveConsumer"),
        qEnvironmentVariable("OneDriveCommercial"),
        QDir::home().filePath(QStringLiteral("Library/Mobile Documents")),
        QDir::home().filePath(QStringLiteral("Library/CloudStorage")),
    };
    for (const QString& root : roots)
        if (isInside(path, root))
            return true;
    return false;
}

#ifdef Q_OS_WIN
bool windowsExternalBus(const QString& rootPath)
{
    if (rootPath.size() < 2 || rootPath.at(1) != QChar(':'))
        return true;
    const QString devicePath = QStringLiteral("\\\\.\\") + rootPath.left(2);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return true;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    QByteArray buffer(4096, 0);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
                                    &query, sizeof(query), buffer.data(),
                                    static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(handle);
    if (!ok || returned < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return true;
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(
        buffer.constData());
    return descriptor->BusType == BusTypeUsb
        || descriptor->BusType == BusType1394
        || descriptor->BusType == BusTypeSd
        || descriptor->BusType == BusTypeMmc
        || descriptor->BusType == BusTypeVirtual
        || descriptor->BusType == BusTypeFileBackedVirtual;
}

bool windowsCloudAttributes(const QString& path)
{
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;
    constexpr DWORD recallFlags = FILE_ATTRIBUTE_OFFLINE
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN;
    return (attributes & recallFlags) != 0;
}
#endif

}

ProjectVolumeInfo ProjectVolume::classify(const QString& path)
{
    ProjectVolumeInfo result;
    QStorageInfo storage(path);
    storage.refresh();
    result.rootPath = QDir::cleanPath(storage.rootPath());
    result.fileSystem = QString::fromLatin1(storage.fileSystemType()).toLower();
    result.availableBytes = storage.isValid() && storage.isReady()
        ? storage.bytesAvailable() : -1;
    if (!storage.isValid() || !storage.isReady() || knownCloudPath(path))
        return result;

#ifdef Q_OS_WIN
    wchar_t volumePath[MAX_PATH]{};
    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (!GetVolumePathNameW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                            volumePath, MAX_PATH))
        return result;
    result.rootPath = QDir::fromNativeSeparators(QString::fromWCharArray(volumePath));
    const UINT driveType = GetDriveTypeW(volumePath);
    if (driveType == DRIVE_REMOTE || windowsCloudAttributes(path))
        return result;
    if (driveType == DRIVE_REMOVABLE || driveType == DRIVE_CDROM
        || driveType == DRIVE_RAMDISK || windowsExternalBus(result.rootPath)) {
        result.kind = ProjectVolumeKind::RecoverableLocal;
        return result;
    }
    if (driveType == DRIVE_FIXED && result.fileSystem == QStringLiteral("ntfs"))
        result.kind = ProjectVolumeKind::CertifiedInternal;
    else if (driveType == DRIVE_FIXED)
        result.kind = ProjectVolumeKind::RecoverableLocal;
#elif defined(Q_OS_MACOS)
    struct statfs fileSystemInfo{};
    const QByteArray nativePath = QFile::encodeName(QFileInfo(path).absoluteFilePath());
    if (statfs(nativePath.constData(), &fileSystemInfo) != 0
        || (fileSystemInfo.f_flags & MNT_LOCAL) == 0)
        return result;
    if (isInside(path, QStringLiteral("/Volumes")))
        result.kind = ProjectVolumeKind::RecoverableLocal;
    else if (result.fileSystem == QStringLiteral("apfs"))
        result.kind = ProjectVolumeKind::CertifiedInternal;
    else
        result.kind = ProjectVolumeKind::RecoverableLocal;
#else
    result.kind = ProjectVolumeKind::RecoverableLocal;
#endif
    return result;
}

}
