#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

class IPCServer;

enum class CommandHost { Desktop, Cli };

bool commandSupportedByHost(CommandHost host, const QString& name, bool requiresDesktop = false);
QJsonObject describeEditorCommand(const QString& name, const IPCServer* dispatcher);

}
