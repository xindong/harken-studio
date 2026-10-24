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

namespace VNH {

void refreshShellIconsIfChanged()
{
    if (!GetShellWindow())
        return;

    QFile icon(QStringLiteral(":/Resources/icons/HubIcon.ico"));
    if (!icon.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read the Hub icon for Windows shell refresh";
        return;
    }
    const QByteArray fingerprint =
        QCryptographicHash::hash(icon.readAll(), QCryptographicHash::Sha256);
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
                                              QStringLiteral("/HarkenHub.exe")}) {
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
