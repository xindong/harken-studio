#include "ProjectInstance.h"

#include <QApplication>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace VNX {

namespace {

QPointer<QWidget> activationWindow;

void focusWindow()
{
    QWidget* window = activationWindow;
    if (window && window->isMinimized())
        window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    if (QWidget* modal = QApplication::activeModalWidget())
        window = modal;
    if (!window)
        return;
    if (window->isMinimized())
        window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    window->raise();
    window->activateWindow();
    if (QWindow* handle = window->windowHandle())
        handle->requestActivate();
#ifdef Q_OS_WIN
    SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
#endif
}

}

void ProjectInstance::setActivationWindow(QWidget* window)
{
    activationWindow = window;
    setActivationHandler(&focusWindow, window != nullptr);
}

}
