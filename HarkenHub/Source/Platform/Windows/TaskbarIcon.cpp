#include "Platform/Windows/TaskbarIcon.h"
#include "Platform/PlatformPaths.h"
#include "Platform/Windows/ShellIcons.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QSaveFile>
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

QString saveShellIcon(const QIcon& icon)
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
    const QString root = userDataDir() + QStringLiteral("/shell-icons");
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

void setShellProperties(HWND window, const QString& iconPath)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return;
    const auto uninitialize = qScopeGuard([initialized] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(properties.GetAddressOf()))))
        return;
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
    set(PKEY_AppUserModel_RelaunchIconResource, QDir::toNativeSeparators(iconPath) + QStringLiteral(",0"));
    set(PKEY_AppUserModel_ID, QString::fromWCharArray(hubAppUserModelId));
}

}

TaskbarIcon::TaskbarIcon(QObject* parent)
    : QObject(parent)
{
    const HRESULT result = SetCurrentProcessExplicitAppUserModelID(hubAppUserModelId);
    if (FAILED(result))
        qWarning() << "Cannot set the Hub taskbar identity:" << result;
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
    iconPath_ = saveShellIcon(icon);
    if (iconPath_.isEmpty())
        qWarning() << "Cannot persist the Hub taskbar icon";
    migrateShellShortcuts(iconPath_);
    const auto previous = std::move(handles_);
    handles_.clear();
    for (QWindow* window : QGuiApplication::allWindows())
        apply(window);
    for (HICON handle : previous)
        DestroyIcon(handle);
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
            if (guard)
                apply(guard);
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

void TaskbarIcon::apply(QWindow* window)
{
    if (!window->isTopLevel() || !window->handle() || icon_.isNull())
        return;
    const HWND nativeWindow = reinterpret_cast<HWND>(window->winId());
    if (!iconPath_.isEmpty())
        setShellProperties(nativeWindow, iconPath_);
    const int size = GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(nativeWindow));
    const HICON handle = iconHandle(size);
    if (handle)
        SendMessageW(nativeWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(handle));
}

}
