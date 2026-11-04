#include "VEffect.h"

namespace VNX {

const PropertySchema& VEffect::schema()
{
    static const PropertySchema schema;
    return schema;
}

QJsonObject VEffect::toJson() const
{
    QJsonArray values;
    for (const float parameter : parameters) values.append(parameter);
    return {{QStringLiteral("type"), EffectChainSemantics::typeName(type)},
            {QStringLiteral("parameters"), values}};
}

bool VEffect::fromJson(const QJsonObject& json)
{
    const auto parsedType = EffectChainSemantics::parseType(
        json.value(QStringLiteral("type")).toString());
    const auto values = json.value(QStringLiteral("parameters"));
    if (!parsedType || !values.isArray()) return false;
    VEffect candidate;
    candidate.type = *parsedType;
    candidate.parameters.clear();
    for (const auto& value : values.toArray()) {
        if (!value.isDouble()) return false;
        candidate.parameters.push_back(static_cast<float>(value.toDouble()));
    }
    if (!EffectChainSemantics::validate(candidate)) return false;
    type = candidate.type;
    parameters = std::move(candidate.parameters);
    return true;
}

}
