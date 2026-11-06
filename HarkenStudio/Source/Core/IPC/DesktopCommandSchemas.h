#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

QJsonObject desktopCommandInputSchema(const QString& name);
bool desktopCommandIsReadOnly(const QString& name);

}
