#include "IPCDesktopInternal.h"

namespace VNX {

void IPCServer::setMainWindow(MainWindow* window)
{
    QObject::disconnect(desktopDestroyedConnection_);
    mainWindow_ = window;
    audition_ = window ? window->auditionManager() : nullptr;
    projectOpening_ = window ? std::function<bool()>([window] {
        return window->isProjectOpening();
    }) : std::function<bool()>{};
    projectOpener_ = window
        ? std::function<bool(const QString&, QString*, bool*)>(
            [window](const QString& path, QString* error, bool* forwarded) {
                return window->loadProject(path, {}, error, forwarded);
            })
        : std::function<bool(const QString&, QString*, bool*)>{};
    if (window) {
        desktopDestroyedConnection_ = connect(window, &QObject::destroyed, this, [this] {
            setMainWindow(nullptr);
        });
        registerDesktopSystemHandlers();
        registerDesktopEditorHandlers();
        registerDesktopAdvancedHandlers();
    }
}

}
