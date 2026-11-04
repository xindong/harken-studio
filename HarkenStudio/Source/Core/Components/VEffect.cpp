#include "VEffect.h"

namespace VNX {

const PropertySchema& VEffect::schema()
{
    static const PropertySchema schema;
    return schema;
}

QJsonObject VEffect::toJson() const
{
    return definition.toJson();
}

bool VEffect::fromJson(const QJsonObject& json)
{
    const auto parsed = EffectDefinition::fromJson(json);
    if (!parsed) return false;
    definition = *parsed;
    return true;
}

}
