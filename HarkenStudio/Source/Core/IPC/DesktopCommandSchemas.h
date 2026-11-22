#pragma once

#include <QJsonObject>
#include <QString>

namespace harken::studio {

QJsonObject desktopCommandInputSchema(const QString& name);
bool desktopCommandIsReadOnly(const QString& name);

}
