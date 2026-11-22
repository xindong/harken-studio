
#pragma once

#include "../Component.h"
#include "../PropertyDescriptor.h"
#include "../ProjectTypes.h"
#include <harken_pack/PackProperties.h>

#include <cstdint>
#include <cmath>

namespace harken::studio {

class VSpatial : public ReflectedComponent<VSpatial>
{
public:
    static constexpr ComponentType kType = ComponentType::Spatial;
    ComponentType getType() const override { return kType; }

    static const PropertySchema& schema();

    VSpatial() = default;
    ~VSpatial() override = default;

    VSpatial(VSpatial&&) noexcept = default;
    VSpatial& operator=(VSpatial&&) noexcept = default;

    VSpatial(const VSpatial&) = default;
    VSpatial& operator=(const VSpatial&) = default;

    bool fromJson(const QJsonObject& json) override
    {
        const auto value = json.value(QStringLiteral("spatializationMix"));
        if (!value.isUndefined() && (!value.isDouble()
            || !std::isfinite(value.toDouble())
            || value.toDouble() < 0.0 || value.toDouble() > 1.0))
            return false;
        if (!ReflectedComponent<VSpatial>::fromJson(json)) return false;
        spatializationMixPresent = !value.isUndefined();
        if (!spatializationMixPresent) spatializationMix = 1.0f;
        return true;
    }

    bool  is3DEnabled       = false;
    float panning           = 0.0f;
    float spatializationMix = 1.0f;
    bool spatializationMixPresent = true;
    float attenuationDistanceScaling = 1.0f;
    harken::studio::types::ObjectId attenuationRef{};
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline const PropertySchema& VSpatial::schema()
{
    static const PropertySchema s = withOwner<VSpatial>({
        { .field="is3DEnabled", .type=PropType::Bool, .label="Enable 3D Spatialization", .section=PropertySection::Rating,
          .aliases={"is3d"},
          .get=VNX_PROP_GET(VSpatial, is3DEnabled), .set=[](IComponent* c, const QVariant& v) {
              auto* s = static_cast<VSpatial*>(c);
              if (s->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(s->hostItem()->type)) return false;
              s->is3DEnabled = v.toBool();
              return true;
          } },
        { .field="panning", .type=PropType::Float, .label="Panning", .section=PropertySection::Rating,
          .min=HK_PACK_PROPERTY_SPATIAL_PANNING_MIN, .max=HK_PACK_PROPERTY_SPATIAL_PANNING_MAX, .def=0.0,
          .decimals=2, .flags={.serialize=false},
          .get=VNX_PROP_GET(VSpatial, panning), .set=[](IComponent* c, const QVariant& v) {
              auto* s = static_cast<VSpatial*>(c);
              if (s->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(s->hostItem()->type)) return false;
              s->panning = v.toFloat();
              return true;
          } },
        { .field="spatializationMix", .type=PropType::Float, .label="Spatialization Mix", .section=PropertySection::Rating,
          .min=HK_PACK_PROPERTY_SPATIALIZATION_MIX_MIN, .max=HK_PACK_PROPERTY_SPATIALIZATION_MIX_MAX, .def=1.0,
          .decimals=2,
          .get=VNX_PROP_GET(VSpatial, spatializationMix), .set=[](IComponent* c, const QVariant& v) {
              bool ok = false;
              const double value = v.toDouble(&ok);
              if (!ok || !std::isfinite(value) || value < 0.0 || value > 1.0) return false;
              auto* s = static_cast<VSpatial*>(c);
              s->spatializationMix = static_cast<float>(value);
              s->spatializationMixPresent = true;
              return true;
          },
          .serializeWhen=[](const IComponent* c) {
              return static_cast<const VSpatial*>(c)->spatializationMixPresent;
          } },
        { .field="attenuationDistanceScaling", .type=PropType::Float, .label="Distance Scale", .section=PropertySection::Rating,
          .unit="%", .min=HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_MIN,
          .max=HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_MAX, .def=1.0, .decimals=0,
          .displayScale=100.0,
          .get=VNX_PROP_GET(VSpatial, attenuationDistanceScaling), .set=[](IComponent* c, const QVariant& v) {
              auto* s = static_cast<VSpatial*>(c);
              if (s->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(s->hostItem()->type)) return false;
              s->attenuationDistanceScaling = v.toFloat();
              return true;
          },
          .coerce=[](const QVariant& v){ return QVariant(qBound(0.1f, v.toFloat(), 10.0f)); },
          .serializeWhen=[](const harken::studio::IComponent* c){ return static_cast<const VSpatial*>(c)->attenuationDistanceScaling != 1.0f; } },
        { .field="attenuationRef", .type=PropType::ObjectRef, .label="Attenuation", .section=PropertySection::Rating,
          .flags={.serialize=false},
          .get=[](const harken::studio::IComponent* c) { return QVariant::fromValue(
              static_cast<const VSpatial*>(c)->attenuationRef); },
          .set=[](IComponent* c, const QVariant& v) {
              auto* s = static_cast<VSpatial*>(c);
              if (s->hostItem() && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(s->hostItem()->type)) return false;
              if (!v.canConvert<harken::studio::types::ObjectId>()) return false;
              s->attenuationRef = v.value<harken::studio::types::ObjectId>();
              return true;
          } },
    });
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
