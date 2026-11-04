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

    EffectDefinition definition;

    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;
};

}
