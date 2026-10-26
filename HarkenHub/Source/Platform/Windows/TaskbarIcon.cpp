#include "Platform/Windows/TaskbarIcon.h"

#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <utility>

namespace VNH {

TaskbarIcon::TaskbarIcon(QObject* parent)
    : QObject(parent)
{
    qGuiApp->installEventFilter(this);
    qGuiApp->installNativeEventFilter(this);
}

TaskbarIcon::~TaskbarIcon()
{
    qGuiApp->removeEventFilter(this);
    qGuiApp->removeNativeEventFilter(this);
    for (HICON handle : handles_)
        DestroyIcon(handle);
}

void TaskbarIcon::setIcon(const QIcon& icon)
{
    icon_ = icon;
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
    const int size = GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(nativeWindow));
    const HICON handle = iconHandle(size);
    if (handle)
        SendMessageW(nativeWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(handle));
}

}
