#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

class IPCServer;

QJsonObject describeEditorCommand(const QString& name, const IPCServer* dispatcher);

}
