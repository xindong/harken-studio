
#pragma once

#include "Core/PropertyDescriptor.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace harken::studio {

QJsonObject inspectorSchemaToJson(const QString& schemaId);

QJsonObject propertySchemaToJson(const PropertySchema& schema,
                                 const QString& component,
                                 const QStringList& itemTypes);

}
