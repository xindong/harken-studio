#pragma once

#include <QJsonObject>
#include <QString>

namespace harken::studio {

QJsonObject commandInputSchema(const QString& name);
bool commandIsReadOnly(const QString& name);

}
