#include "Platform/Windows/TaskbarIcon.h"
#include "Platform/PlatformPaths.h"
#include "Platform/Windows/ShellIcons.h"

#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>
#include <QWindow>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <utility>

namespace VNH {

namespace {

bool setShellProperties(HWND window, const QString& iconPath)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return false;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(properties.GetAddressOf()))))
        return false;
    const QString resource = QDir::toNativeSeparators(iconPath) + QStringLiteral(",0");
    PROPVARIANT previous{};
    const HRESULT read = properties->GetValue(PKEY_AppUserModel_RelaunchIconResource, &previous);
    const bool changed = !iconPath.isEmpty() && (FAILED(read) || previous.vt != VT_LPWSTR ||
                         QString::fromWCharArray(previous.pwszVal) != resource);
    PropVariantClear(&previous);
    const auto set = [&](REFPROPERTYKEY key, const QString& value) {
        PROPVARIANT variant{};
        HRESULT result = S_OK;
        if (!iconPath.isEmpty())
            result = InitPropVariantFromString(value.toStdWString().c_str(), &variant);
        if (SUCCEEDED(result))
            result = properties->SetValue(key, variant);
        PropVariantClear(&variant);
        if (FAILED(result))
            qWarning() << "Cannot set the Hub taskbar property:" << key.pid << result;
    };
    set(PKEY_AppUserModel_RelaunchCommand,
        QLatin1Char('"') + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + QLatin1Char('"'));
    set(PKEY_AppUserModel_RelaunchDisplayNameResource, QStringLiteral("Harken Hub"));
    set(PKEY_AppUserModel_RelaunchIconResource, resource);
    set(PKEY_AppUserModel_ID, QString::fromWCharArray(hubAppUserModelId));
    return changed;
}

}

TaskbarIcon::TaskbarIcon(QObject* parent)
    : QObject(parent)
{
    const HRESULT result = SetCurrentProcessExplicitAppUserModelID(hubAppUserModelId);
    if (FAILED(result))
        qWarning() << "Cannot set the Hub taskbar identity:" << result;
    shortcutRetry_.setSingleShot(true);
    shortcutRetry_.setInterval(250);
    connect(&shortcutRetry_, &QTimer::timeout, this, &TaskbarIcon::updateShortcuts);
    qGuiApp->installEventFilter(this);
    qGuiApp->installNativeEventFilter(this);
}

TaskbarIcon::~TaskbarIcon()
{
    qGuiApp->removeEventFilter(this);
    qGuiApp->removeNativeEventFilter(this);
    for (QWindow* window : QGuiApplication::allWindows()) {
        if (window->isTopLevel() && window->handle())
            setShellProperties(reinterpret_cast<HWND>(window->winId()), {});
    }
    for (HICON handle : handles_)
        DestroyIcon(handle);
}

void TaskbarIcon::setIcon(const QIcon& icon)
{
    icon_ = icon;
    iconPath_ = saveShellIcon(icon, userDataDir() + QStringLiteral("/shell-icons"));
    if (iconPath_.isEmpty())
        qWarning() << "Cannot persist the Hub taskbar icon";
    shortcutRetry_.stop();
    const auto previous = std::move(handles_);
    handles_.clear();
    for (QWindow* window : QGuiApplication::allWindows())
        apply(window);
    for (HICON handle : previous)
        DestroyIcon(handle);
    if (!iconPath_.isEmpty())
        updateShortcuts();
}

void TaskbarIcon::updateShortcuts()
{
    const auto result = migrateShellShortcuts(iconPath_);
    if (result == ShortcutMigrationResult::Busy) {
        shortcutRetry_.start();
        return;
    }
    if (result == ShortcutMigrationResult::Failed)
        qWarning() << "Cannot migrate the Hub shell shortcuts";
    refreshTaskbar();
}

void TaskbarIcon::refreshTaskbar()
{
    QList<HWND> windows;
    for (QWindow* window : QGuiApplication::allWindows()) {
        if (!window->isTopLevel() || !window->isVisible() || !window->handle())
            continue;
        const HWND native = reinterpret_cast<HWND>(window->winId());
        const LONG_PTR style = GetWindowLongPtrW(native, GWL_EXSTYLE);
        if (IsWindowVisible(native) && !(style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) &&
            (!GetWindow(native, GW_OWNER) || (style & WS_EX_APPWINDOW)))
            windows.append(native);
    }
    if (windows.isEmpty())
        return;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    Microsoft::WRL::ComPtr<ITaskbarList> taskbar;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(taskbar.GetAddressOf()))) || FAILED(taskbar->HrInit()))
        return;
    for (const HWND window : windows) {
        if (FAILED(taskbar->DeleteTab(window)))
            qWarning() << "Cannot detach the Hub taskbar button for icon refresh";
    }
    for (const HWND window : windows) {
        if (FAILED(taskbar->AddTab(window)))
            qWarning() << "Cannot restore the Hub taskbar button after icon refresh";
    }
}

bool TaskbarIcon::eventFilter(QObject* watched, QEvent* event)
{
    auto* window = qobject_cast<QWindow*>(watched);
    if (window && window->isTopLevel() && window->handle() &&
        event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        setShellProperties(reinterpret_cast<HWND>(window->winId()), {});
    if (window && (event->type() == QEvent::Show ||
                   event->type() == QEvent::WindowIconChange ||
                   event->type() == QEvent::DevicePixelRatioChange ||
                   (event->type() == QEvent::PlatformSurface &&
                    static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
                        QPlatformSurfaceEvent::SurfaceCreated))) {
        QTimer::singleShot(0, this, [this, guard = QPointer<QWindow>(window)] {
            if (guard && apply(guard))
                refreshTaskbar();
        });
    }
    return QObject::eventFilter(watched, event);
}

bool TaskbarIcon::nativeEventFilter(const QByteArray&, void* message, qintptr* result)
{
    const auto* event = static_cast<MSG*>(message);
    if (event->message != WM_GETICON || icon_.isNull() ||
        (event->wParam != ICON_BIG && event->wParam != ICON_SMALL && event->wParam != ICON_SMALL2))
        return false;
    for (QWindow* window : QGuiApplication::allWindows()) {
        if (!window->isTopLevel() || !window->handle() ||
            reinterpret_cast<HWND>(window->winId()) != event->hwnd)
            continue;
        const UINT dpi = event->lParam ? static_cast<UINT>(event->lParam) : GetDpiForWindow(event->hwnd);
        const int metric = event->wParam == ICON_BIG ? SM_CXICON : SM_CXSMICON;
        const HICON handle = iconHandle(GetSystemMetricsForDpi(metric, dpi));
        if (!handle)
            return false;
        *result = reinterpret_cast<qintptr>(handle);
        return true;
    }
    return false;
}

HICON TaskbarIcon::iconHandle(int size)
{
    HICON& handle = handles_[size];
    if (!handle)
        handle = icon_.pixmap(QSize(size, size), 1.0).toImage().toHICON();
    return handle;
}

bool TaskbarIcon::apply(QWindow* window)
{
    if (!window->isTopLevel() || !window->handle() || icon_.isNull())
        return false;
    const HWND nativeWindow = reinterpret_cast<HWND>(window->winId());
    const bool changed = !iconPath_.isEmpty() && setShellProperties(nativeWindow, iconPath_);
    const int size = GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(nativeWindow));
    const HICON handle = iconHandle(size);
    if (handle)
        SendMessageW(nativeWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(handle));
    return changed;
}

}
