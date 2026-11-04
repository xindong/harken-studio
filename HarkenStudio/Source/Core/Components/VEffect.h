#pragma once

#include "../EffectChainSemantics.h"
#include "../PropertyDescriptor.h"

namespace VNX {

class VEffect : public ReflectedComponent<VEffect>
{
public:
    static constexpr ComponentType kType = ComponentType::Effect;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    EffectType type = EffectType::Limiter;
    std::vector<float> parameters = EffectChainSemantics::defaultParameters(EffectType::Limiter);

    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;
};

}
