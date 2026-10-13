#include "Update/BridgeDispatcher.h"

#include "Platform/UpdateBridgeTask.h"

#include <QProcess>

namespace VNH {

namespace {

QString overrideCommand()
{
    return qEnvironmentVariable("HARKEN_HUB_BRIDGE_COMMAND");
}

}

bool BridgeDispatcher::available()
{
    if (qEnvironmentVariable("HARKEN_HUB_BRIDGE_DISABLE") ==
        QStringLiteral("1"))
        return false;
    if (!overrideCommand().isEmpty())
        return true;
    return updateBridgeTaskAvailable();
}

bool BridgeDispatcher::dispatch(QString* error)
{
    if (!available()) {
        *error = QStringLiteral("The update bridge is not available");
        return false;
    }
    const QString command = overrideCommand();
    if (!command.isEmpty()) {
        if (command == QStringLiteral("noop"))
            return true;
        const QStringList parts = QProcess::splitCommand(command);
        if (parts.isEmpty()) {
            *error = QStringLiteral("Invalid bridge command override");
            return false;
        }
        if (!QProcess::startDetached(parts.first(), parts.mid(1))) {
            *error = QStringLiteral("Cannot start the bridge command override");
            return false;
        }
        return true;
    }
    return runUpdateBridgeTask(error);
}

}
