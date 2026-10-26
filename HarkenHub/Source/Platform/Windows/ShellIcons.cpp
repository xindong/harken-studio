#include "Platform/Windows/ShellIcons.h"
#include "Platform/PlatformPaths.h"

#include <QBuffer>
#include <QDataStream>
#include <QSaveFile>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QScopeGuard>

#include <windows.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <wrl/client.h>

namespace VNH {

QString saveShellIcon(const QIcon& icon, const QString& root)
{
    const auto sizes = icon.availableSizes();
    if (sizes.isEmpty())
        return {};
    QByteArray data;
    QDataStream directory(&data, QIODevice::WriteOnly);
    directory.setByteOrder(QDataStream::LittleEndian);
    directory << quint16(0) << quint16(1) << quint16(sizes.size());
    QByteArray images;
    const quint32 headerSize = 6 + 16 * sizes.size();
    for (const QSize& size : sizes) {
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) ||
            !icon.pixmap(size, 1.0).save(&buffer, "PNG"))
            return {};
        directory << quint8(size.width() == 256 ? 0 : size.width())
                  << quint8(size.height() == 256 ? 0 : size.height())
                  << quint8(0) << quint8(0) << quint16(1) << quint16(32)
                  << quint32(png.size()) << quint32(headerSize + images.size());
        images += png;
    }
    data += images;
    const QString path = root + QLatin1Char('/') + QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".ico");
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data)
        return path;
    existing.close();
    if (!QDir().mkpath(root))
        return {};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return {};
    return path;
}

ShortcutMigrationResult migrateShellShortcuts(const QString& taskbarIconPath)
{
    const QString pathHash = QString::fromLatin1(QCryptographicHash::hash(
        QCoreApplication::applicationDirPath().toCaseFolded().toUtf8(),
        QCryptographicHash::Sha256).toHex());
    const std::wstring mutexName =
        (QStringLiteral("Local\\HarkenHubShortcutMigration-") + pathHash).toStdWString();
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex)
        return ShortcutMigrationResult::Failed;
    const auto closeMutex = qScopeGuard([mutex] { CloseHandle(mutex); });
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired == WAIT_TIMEOUT)
        return ShortcutMigrationResult::Busy;
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        return ShortcutMigrationResult::Failed;
    const auto releaseMutex = qScopeGuard([mutex] { ReleaseMutex(mutex); });

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return ShortcutMigrationResult::Failed;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    const QString executable = QCoreApplication::applicationDirPath() +
                               QStringLiteral("/HarkenHub.exe");
    const QString legacy = QCoreApplication::applicationDirPath() +
                           QStringLiteral("/HarkenStudioHub.exe");
    const auto normalized = [](const QString& path) {
        return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
    };
    const QSet<QString> targets{normalized(executable), normalized(legacy)};
    const std::wstring nativeExecutable = QDir::toNativeSeparators(executable).toStdWString();
    for (const KNOWNFOLDERID* folder : {&FOLDERID_Desktop, &FOLDERID_PublicDesktop,
                                      &FOLDERID_Programs, &FOLDERID_CommonPrograms,
                                      &FOLDERID_RoamingAppData}) {
        PWSTR folderPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(*folder, KF_FLAG_DEFAULT, nullptr, &folderPath)))
            continue;
        QString root = QString::fromWCharArray(folderPath);
        CoTaskMemFree(folderPath);
#ifdef HARKEN_HUB_TEST_OVERRIDES
        const QString testRoot = qEnvironmentVariable("HARKEN_HUB_SHELL_ROOT");
        if (!testRoot.isEmpty()) {
            root = testRoot + ((folder == &FOLDERID_Programs || folder == &FOLDERID_CommonPrograms)
                                 ? QStringLiteral("/Programs") : QStringLiteral("/Desktop"));
            if (folder == &FOLDERID_RoamingAppData)
                root = testRoot;
        }
#endif
        if (folder == &FOLDERID_RoamingAppData)
            root += QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar");
        const bool recursive = folder == &FOLDERID_Programs || folder == &FOLDERID_CommonPrograms;
        QDirIterator shortcuts(root, {QStringLiteral("*.lnk")}, QDir::Files,
                               recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (shortcuts.hasNext()) {
            const QFileInfo shortcut(shortcuts.next());
            if (!targets.contains(normalized(shortcut.symLinkTarget())))
                continue;
            Microsoft::WRL::ComPtr<IShellLinkW> link;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(link.GetAddressOf()))))
                continue;
            Microsoft::WRL::ComPtr<IPersistFile> file;
            if (FAILED(link.As(&file)))
                continue;
            const std::wstring path = QDir::toNativeSeparators(shortcut.filePath()).toStdWString();
            if (FAILED(file->Load(path.c_str(), STGM_READWRITE)))
                continue;
            bool changed = false;
            if (normalized(shortcut.symLinkTarget()) == normalized(legacy)) {
                if (FAILED(link->SetPath(nativeExecutable.c_str())))
                    continue;
                changed = true;
            }
            wchar_t iconPath[MAX_PATH]{};
            int iconIndex = 0;
            if (SUCCEEDED(link->GetIconLocation(iconPath, MAX_PATH, &iconIndex)) &&
                normalized(QString::fromWCharArray(iconPath)) == normalized(legacy)) {
                if (FAILED(link->SetIconLocation(nativeExecutable.c_str(), iconIndex)))
                    continue;
                changed = true;
            }
            wchar_t description[MAX_PATH]{};
            if (SUCCEEDED(link->GetDescription(description, MAX_PATH)) &&
                QString::fromWCharArray(description) == QStringLiteral("Harken Studio Hub")) {
                if (FAILED(link->SetDescription(L"Harken Hub")))
                    continue;
                changed = true;
            }
            Microsoft::WRL::ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(link.As(&properties))) {
                PROPVARIANT identity{};
                const HRESULT read = properties->GetValue(PKEY_AppUserModel_ID, &identity);
                const bool matches = SUCCEEDED(read) && identity.vt == VT_LPWSTR &&
                                     QString::fromWCharArray(identity.pwszVal) ==
                                         QString::fromWCharArray(hubAppUserModelId);
                PropVariantClear(&identity);
                if (!matches && SUCCEEDED(InitPropVariantFromString(hubAppUserModelId, &identity))) {
                    const HRESULT written = properties->SetValue(PKEY_AppUserModel_ID, identity);
                    PropVariantClear(&identity);
                    if (SUCCEEDED(written) && SUCCEEDED(properties->Commit()))
                        changed = true;
                    else
                        qWarning() << "Cannot set the Hub shortcut identity:" << shortcut.filePath();
                }
            }
            if (folder == &FOLDERID_RoamingAppData && !taskbarIconPath.isEmpty() &&
                SUCCEEDED(link->GetIconLocation(iconPath, MAX_PATH, &iconIndex))) {
                const QString currentIcon = normalized(QString::fromWCharArray(iconPath));
                const QString managedRoot = normalized(userDataDir() + QStringLiteral("/shell-icons"));
                if ((currentIcon.isEmpty() || targets.contains(currentIcon) ||
                     normalized(QFileInfo(currentIcon).absolutePath()) == managedRoot) &&
                    currentIcon != normalized(taskbarIconPath)) {
                    const std::wstring nativeIcon = QDir::toNativeSeparators(taskbarIconPath).toStdWString();
                    if (SUCCEEDED(link->SetIconLocation(nativeIcon.c_str(), 0)))
                        changed = true;
                    else
                        qWarning() << "Cannot set the Hub pinned shortcut icon:" << shortcut.filePath();
                }
            }
            if (changed && FAILED(file->Save(path.c_str(), TRUE))) {
                qWarning() << "Cannot update the Hub shortcut:" << shortcut.filePath();
                continue;
            }
            if (changed)
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                               path.c_str(), nullptr);
            properties.Reset();
            file.Reset();
            link.Reset();
            if (folder != &FOLDERID_RoamingAppData &&
                shortcut.fileName() == QStringLiteral("Harken Studio Hub.lnk")) {
                const QString renamed = shortcut.dir().filePath(QStringLiteral("Harken Hub.lnk"));
                if (!QFileInfo::exists(renamed)) {
                    if (!QFile::rename(shortcut.filePath(), renamed))
                        qWarning() << "Cannot rename the Hub shortcut:" << shortcut.filePath();
                }
            }
        }
    }
    return ShortcutMigrationResult::Complete;
}

void refreshShellIconsIfChanged()
{
    if (!GetShellWindow())
        return;

    migrateShellShortcuts();

    QFile icon(QStringLiteral(":/Resources/icons/HubIcon.ico"));
    if (!icon.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read the Hub icon for Windows shell refresh";
        return;
    }
    const QByteArray fingerprint =
        QCryptographicHash::hash(icon.readAll() + QByteArrayLiteral("HarkenHub.exe"),
                                QCryptographicHash::Sha256);
    const QString executable = QCoreApplication::applicationFilePath();
    const QString pathHash = QString::fromLatin1(QCryptographicHash::hash(
        executable.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex());
    const std::wstring mutexName =
        (QStringLiteral("Local\\HarkenHubShellIcons-") + pathHash).toStdWString();
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex)
        return;
    const auto closeMutex = qScopeGuard([mutex] { CloseHandle(mutex); });
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        return;
    const auto releaseMutex = qScopeGuard([mutex] { ReleaseMutex(mutex); });

    const QString key = QStringLiteral("windowsShellIcons/") + pathHash;
    QSettings settings;
    if (settings.value(key).toByteArray() == fingerprint)
        return;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    const auto notify = [](const QString& path) {
        const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                       nativePath.c_str(), nullptr);
    };
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH,
                   nullptr, nullptr);
    QSet<QString> executables;
    for (const QString& path : {executable, QCoreApplication::applicationDirPath() +
                                              QStringLiteral("/HarkenStudioHub.exe")}) {
        if (!QFileInfo::exists(path))
            continue;
        executables.insert(QDir::cleanPath(path).toCaseFolded());
        notify(path);
    }

    for (const KNOWNFOLDERID* folder : {&FOLDERID_Desktop, &FOLDERID_PublicDesktop,
                                      &FOLDERID_Programs, &FOLDERID_CommonPrograms,
                                      &FOLDERID_RoamingAppData}) {
        PWSTR path = nullptr;
        if (FAILED(SHGetKnownFolderPath(*folder, KF_FLAG_DEFAULT, nullptr, &path)))
            continue;
        QString root = QString::fromWCharArray(path);
        CoTaskMemFree(path);
        if (folder == &FOLDERID_RoamingAppData)
            root += QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar");
        const bool recursive = folder == &FOLDERID_Programs || folder == &FOLDERID_CommonPrograms;
        QDirIterator shortcuts(root, {QStringLiteral("*.lnk")}, QDir::Files,
                               recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (shortcuts.hasNext()) {
            const QFileInfo shortcut(shortcuts.next());
            const QString target = QDir::cleanPath(
                QDir::fromNativeSeparators(shortcut.symLinkTarget())).toCaseFolded();
            if (executables.contains(target))
                notify(shortcut.filePath());
        }
    }
    settings.setValue(key, fingerprint);
    settings.sync();
    qInfo() << "Requested Windows shell icon refresh for" << executable;
}

}
