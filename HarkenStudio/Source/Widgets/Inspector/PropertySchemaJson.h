
#pragma once

#include "Core/PropertyDescriptor.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace VNX {

QJsonObject propertySchemaToJson(const PropertySchema& schema,
                                 const QString& component,
                                 const QStringList& itemTypes);

}
