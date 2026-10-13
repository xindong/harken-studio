
#pragma once

#include "../Component.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"

#include <QJsonObject>

namespace VNX {

class VAttenuation : public ReflectedComponent<VAttenuation>
{
public:
    static constexpr ComponentType kType = ComponentType::Attenuation;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    float maxDistance = 100.0f;
    QVector<AttenuationDimensionDef> dimensions;

    const AttenuationDimensionDef* dimension(AttenuationDimension type) const;
    AttenuationSnapshot snapshot() const;
    bool setDimensions(const QJsonArray& value);
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;
};

}
