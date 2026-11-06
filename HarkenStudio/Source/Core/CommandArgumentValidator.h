#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

QString commandSchemaError(const QJsonObject& schema);
bool validateCommandArguments(const QJsonObject& arguments, const QJsonObject& schema,
                              QString* errorMessage = nullptr);

}
