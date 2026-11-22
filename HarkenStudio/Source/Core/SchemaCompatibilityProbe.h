#pragma once

#include <QJsonObject>

namespace harken::studio {

QJsonObject runSchemaCompatibilityProbe(const QJsonObject& request);

}
