
#pragma once

#include "../Component.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"
#include <QJsonObject>

#include <limits>

namespace VNX {

class VAudio : public ReflectedComponent<VAudio>
{
public:
    static constexpr ComponentType kType = ComponentType::Audio;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    VAudio() = default;
    ~VAudio() override = default;

    VAudio(VAudio&&) noexcept = default;
    VAudio& operator=(VAudio&&) noexcept = default;

    VAudio(const VAudio&) = default;
    VAudio& operator=(const VAudio&) = default;

    uint32_t loopCount = 1;
    uint32_t streamingMode = 0;
    uint8_t priority = 50;
    uint16_t overrideBits = 0;

    void toJsonExtra(QJsonObject& json) const override
    {
        json[QStringLiteral("streamingMode")] = static_cast<qint64>(streamingMode);
        json[QStringLiteral("overrideBits")] = overrideBits;
        json[QStringLiteral("outputBusOverride")] = false;
    }

    void fromJsonExtra(const QJsonObject& json) override
    {
        const int raw = json[QStringLiteral("streamingMode")].toInt();
        streamingMode = raw >= 2 ? 1u : 0u;
        overrideBits = static_cast<uint16_t>(
            json[QStringLiteral("overrideBits")].toInt());
        if (!json.contains(QStringLiteral("outputBusOverride")))
            overrideBits &= ~0x1u;
    }
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VAudio::schema()
{
    static const PropertySchema s = withOwner<VAudio>({
        { .field="loopCount", .type=PropType::Int, .label="Loop", .section=PropertySection::Playback,
          .min=0.0, .max=static_cast<double>(std::numeric_limits<int>::max()), .def=1.0,
          .flags={ .segmented=true },
          .segments={
              { .value=1, .numeric=true, .numericMin=1,
                .numericMax=std::numeric_limits<int>::max() },
              { .text="Infinite", .value=0 },
          },
          .aliases={"loopcount"},
          .get=[](const VNX::IComponent* c) { return QVariant::fromValue(
              static_cast<qulonglong>(static_cast<const VAudio*>(c)->loopCount)); },
          .set=[](VNX::IComponent* c, const QVariant& value) {
              bool ok = false;
              const qulonglong raw = value.toULongLong(&ok);
              if (!ok || raw > std::numeric_limits<uint32_t>::max()) return false;
              static_cast<VAudio*>(c)->loopCount = static_cast<uint32_t>(raw);
              return true;
          } },

        { .field="streamingMode", .type=PropType::Enum, .label="Streaming", .section=PropertySection::Playback,
          .flags={.serialize=false}, .options={{"In Memory",0},{"Streaming",1}}, .aliases={"streamingmode"},
          .get=VNX_PROP_GET(VAudio, streamingMode), .set=VNX_PROP_SET_U32(VAudio, streamingMode) },

        { .field="priority", .jsonKey="Priority", .type=PropType::Int, .label="Priority",
          .section=PropertySection::PlaybackPriority, .min=0.0, .max=100.0, .def=50.0,
          .get=VNX_PROP_GETI(VAudio, priority), .set=VNX_PROP_SET_U8(VAudio, priority),
          .serializeWhen=[](const VNX::IComponent* c){ return static_cast<const VAudio*>(c)->priority != 50; } },

        { .field="overrideBits", .type=PropType::Int, .label="Override Bits", .section=PropertySection::Advanced,
          .flags={.serialize=false, .settableByName=false},
          .get=VNX_PROP_GETI(VAudio, overrideBits) },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
