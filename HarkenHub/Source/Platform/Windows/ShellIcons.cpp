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

namespace harken::hub {

namespace {

bool hasHubIdentity(IPropertyStore* properties)
{
    PROPVARIANT identity{};
    const HRESULT result = properties->GetValue(PKEY_AppUserModel_ID, &identity);
    const bool matches = SUCCEEDED(result) && identity.vt == VT_LPWSTR &&
                         QString::fromWCharArray(identity.pwszVal) ==
                             QString::fromWCharArray(hubAppUserModelId);
    PropVariantClear(&identity);
    return matches;
}

QString missingHubTarget(IShellLinkW* link)
{
    wchar_t path[MAX_PATH]{};
    if (link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH) != S_OK)
        return {};
    const QString target = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    const QFileInfo info(target);
    if (!info.isAbsolute() ||
        (info.fileName().compare(QStringLiteral("HarkenHub.exe"), Qt::CaseInsensitive) != 0 &&
         info.fileName().compare(QStringLiteral("HarkenStudioHub.exe"), Qt::CaseInsensitive) != 0))
        return {};
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return {};
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        return {};
    wchar_t volume[MAX_PATH]{};
    return GetVolumePathNameW(path, volume, MAX_PATH) ? target : QString();
}

}

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
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const bool installedHub = normalized(registeredHubInstallDir()) == normalized(applicationDir);
    const std::wstring nativeDirectory = QDir::toNativeSeparators(applicationDir).toStdWString();
    const std::wstring nativeExecutable = QDir::toNativeSeparators(executable).toStdWString();
    for (const KNOWNFOLDERID* folder : {&FOLDERID_Desktop, &FOLDERID_PublicDesktop,
                                      &FOLDERID_Programs, &FOLDERID_CommonPrograms,
                                      &FOLDERID_RoamingAppData, &FOLDERID_ImplicitAppShortcuts}) {
        const bool taskbarShortcut = folder == &FOLDERID_RoamingAppData || folder == &FOLDERID_ImplicitAppShortcuts;
        if (!taskbarIconPath.isEmpty() && !taskbarShortcut)
            continue;
        PWSTR folderPath = nullptr;
        const DWORD flags = folder == &FOLDERID_ImplicitAppShortcuts ? KF_FLAG_DONT_VERIFY : KF_FLAG_DEFAULT;
        if (FAILED(SHGetKnownFolderPath(*folder, flags, nullptr, &folderPath)))
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
            if (folder == &FOLDERID_ImplicitAppShortcuts)
                root = testRoot + QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/ImplicitAppShortcuts");
        }
#endif
        if (folder == &FOLDERID_RoamingAppData)
            root += QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar");
        const bool recursive = folder == &FOLDERID_Programs || folder == &FOLDERID_CommonPrograms
                            || folder == &FOLDERID_ImplicitAppShortcuts;
        const QDir::Filters filters = taskbarShortcut && installedHub ? QDir::Files | QDir::System : QDir::Files;
        QDirIterator shortcuts(root, {QStringLiteral("*.lnk")}, filters,
                               recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (shortcuts.hasNext()) {
            const QFileInfo shortcut(shortcuts.next());
            const QString target = normalized(shortcut.symLinkTarget());
            const bool currentTarget = targets.contains(target);
            if (!currentTarget && !(taskbarShortcut && installedHub))
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
            Microsoft::WRL::ComPtr<IPropertyStore> properties;
            const bool matchesIdentity = SUCCEEDED(link.As(&properties)) && hasHubIdentity(properties.Get());
            QString oldTarget;
            if (!currentTarget) {
                if (!matchesIdentity)
                    continue;
                oldTarget = missingHubTarget(link.Get());
                if (oldTarget.isEmpty())
                    continue;
                wchar_t workingDirectory[MAX_PATH]{};
                if (FAILED(link->GetWorkingDirectory(workingDirectory, MAX_PATH)))
                    continue;
                if (normalized(QString::fromWCharArray(workingDirectory)) ==
                        normalized(QFileInfo(oldTarget).absolutePath()) &&
                    FAILED(link->SetWorkingDirectory(nativeDirectory.c_str())))
                    continue;
            }
            bool changed = false;
            if (target == normalized(legacy) || !oldTarget.isEmpty()) {
                if (FAILED(link->SetPath(nativeExecutable.c_str())))
                    continue;
                changed = true;
            }
            wchar_t iconPath[MAX_PATH]{};
            int iconIndex = 0;
            if (SUCCEEDED(link->GetIconLocation(iconPath, MAX_PATH, &iconIndex))) {
                const QString previousIcon = normalized(QString::fromWCharArray(iconPath));
                if (previousIcon == normalized(legacy) || (!oldTarget.isEmpty() && previousIcon == normalized(oldTarget))) {
                    if (FAILED(link->SetIconLocation(nativeExecutable.c_str(), iconIndex)))
                        continue;
                    changed = true;
                }
            }
            wchar_t description[MAX_PATH]{};
            if (SUCCEEDED(link->GetDescription(description, MAX_PATH)) &&
                QString::fromWCharArray(description) == QStringLiteral("Harken Studio Hub")) {
                if (FAILED(link->SetDescription(L"Harken Hub")))
                    continue;
                changed = true;
            }
            if (!taskbarIconPath.isEmpty() && properties && !matchesIdentity) {
                PROPVARIANT identity{};
                if (SUCCEEDED(InitPropVariantFromString(hubAppUserModelId, &identity))) {
                    const HRESULT written = properties->SetValue(PKEY_AppUserModel_ID, identity);
                    PropVariantClear(&identity);
                    if (SUCCEEDED(written) && SUCCEEDED(properties->Commit()))
                        changed = true;
                    else
                        qWarning() << "Cannot set the Hub shortcut identity:" << shortcut.filePath();
                }
            }
            if (taskbarShortcut && !taskbarIconPath.isEmpty() &&
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
            if (changed && (!taskbarIconPath.isEmpty() || !oldTarget.isEmpty()))
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                               path.c_str(), nullptr);
            properties.Reset();
            file.Reset();
            link.Reset();
            if (!taskbarShortcut &&
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
