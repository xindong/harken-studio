#include "Platform/Windows/ShellIcons.h"

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
#include <shlobj.h>
#include <wrl/client.h>

namespace VNH {

void migrateShellShortcuts()
{
    const QString pathHash = QString::fromLatin1(QCryptographicHash::hash(
        QCoreApplication::applicationDirPath().toCaseFolded().toUtf8(),
        QCryptographicHash::Sha256).toHex());
    const std::wstring mutexName =
        (QStringLiteral("Local\\HarkenHubShortcutMigration-") + pathHash).toStdWString();
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex)
        return;
    const auto closeMutex = qScopeGuard([mutex] { CloseHandle(mutex); });
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        return;
    const auto releaseMutex = qScopeGuard([mutex] { ReleaseMutex(mutex); });

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return;
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
            if (changed && FAILED(file->Save(path.c_str(), TRUE))) {
                qWarning() << "Cannot update the Hub shortcut:" << shortcut.filePath();
                continue;
            }
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
