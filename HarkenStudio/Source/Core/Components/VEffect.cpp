#include "VEffect.h"

#include <cmath>

namespace VNX {

const PropertySchema& VEffect::schema()
{
    static const PropertySchema schema = [] {
        const auto& ranges = EffectChainSemantics::parameterRanges(EffectType::Limiter);
        const auto defaults = EffectChainSemantics::defaultParameters(EffectType::Limiter);
        PropertySchema fields;
        for (size_t index = 0; index < ranges.size(); ++index) {
            PropertyDescriptor field;
            field.field = index == 0 ? QStringLiteral("ceilingDb") : QStringLiteral("releaseMs");
            field.ownerComponent = ComponentType::Effect;
            field.label = index == 0 ? QStringLiteral("Ceiling") : QStringLiteral("Release");
            field.section = PropertySection::Effect;
            field.unit = index == 0 ? QStringLiteral(" dB") : QStringLiteral(" ms");
            field.min = ranges[index].first;
            field.max = ranges[index].second;
            field.def = defaults[index];
            field.decimals = 2;
            field.flags.serialize = false;
            field.get = [index](const IComponent* component) -> QVariant {
                return static_cast<const VEffect*>(component)->definition.parameters[index];
            };
            field.set = [index, range = ranges[index]](IComponent* component, const QVariant& value) {
                bool valid = false;
                const double parameter = value.toDouble(&valid);
                if (!valid || !std::isfinite(parameter)
                    || parameter < range.first || parameter > range.second) return false;
                static_cast<VEffect*>(component)->definition.parameters[index] = static_cast<float>(parameter);
                return true;
            };
            fields.push_back(std::move(field));
        }
        return fields;
    }();
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
