#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

QJsonObject commandInputSchema(const QString& name);
bool commandIsReadOnly(const QString& name);

}
