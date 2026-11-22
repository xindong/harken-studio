#pragma once

#include <QJsonObject>
#include <QString>

namespace harken::studio {

QString commandSchemaError(const QJsonObject& schema);
bool validateCommandArguments(const QJsonObject& arguments, const QJsonObject& schema,
                              QString* errorMessage = nullptr);

}
