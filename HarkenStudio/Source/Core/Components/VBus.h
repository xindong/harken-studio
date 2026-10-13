
#pragma once

#include "../Component.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"

#include <QJsonObject>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <functional>

namespace VNX {

class VBus : public ReflectedComponent<VBus>
{
public:
    static constexpr ComponentType kType = ComponentType::Bus;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    VBus() = default;
    ~VBus() override = default;

    VBus(VBus&&) noexcept = default;
    VBus& operator=(VBus&&) noexcept = default;

    VBus(const VBus&) = default;
    VBus& operator=(const VBus&) = default;

    float pan = 0.0f;

    uint8_t  channelConfig            = 0;
    bool     listenerRelativeRouting  = false;

    void toJsonExtra(QJsonObject& obj) const override
    {
        obj[QStringLiteral("pan")]         = pan;

        if (channelConfig != 0)            obj[QStringLiteral("channelConfig")] = channelConfig;
        if (listenerRelativeRouting)       obj[QStringLiteral("listenerRelativeRouting")] = true;
    }

    void fromJsonExtra(const QJsonObject& obj) override
    {
        pan = static_cast<float>(obj[QStringLiteral("pan")].toDouble(0.0));

        channelConfig           = static_cast<uint8_t>(obj[QStringLiteral("channelConfig")].toInt(0));
        listenerRelativeRouting = obj[QStringLiteral("listenerRelativeRouting")].toBool(false);
    }
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VBus::schema()
{
    static const PropertySchema s = withOwner<VBus>({
        { .field="pan", .type=PropType::Float, .label="Pan", .section=PropertySection::Playback,
          .min=-1.0, .max=1.0, .decimals=2, .flags={.serialize=false},
          .get=VNX_PROP_GET(VBus, pan), .set=VNX_PROP_SET_F(VBus, pan) },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
