#ifndef HARKEN_PLAYABLE_PROPERTIES_PLAYABLE_PROPERTY_STUDIO_H
#define HARKEN_PLAYABLE_PROPERTIES_PLAYABLE_PROPERTY_STUDIO_H

#include "harken_playable_properties/PlayableProperties.h"

#include <cstdint>
#include <iterator>

namespace harkenplayableproperties { namespace studio {

inline constexpr char kEnvelopeJsonKeyV1[] = "playableProperties";
inline constexpr char kPresenceJsonKeyV1[] = "presence";

struct JsonFieldV1 { std::uint8_t id; std::uint64_t presentBit; PlayablePropertyWireV1 wire; PlayablePropertyDefaultV1 canonicalDefault; PlayablePropertyReferenceRoleV1 referenceRole; const char* component; const char* storageField; const char* fieldName; const char* jsonKey; };

inline constexpr JsonFieldV1 kJsonFieldsV1[] = {
    {HK_PACK_PROPERTY_VOLUME, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_VOLUME), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_VOLUME_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_VOLUME_DEFAULT), PlayablePropertyReferenceRoleV1::None, "mixnode", "volume", "volume", "volume"},
    {HK_PACK_PROPERTY_PITCH, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_PITCH), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_PITCH_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_PITCH_DEFAULT), PlayablePropertyReferenceRoleV1::None, "mixnode", "pitch", "pitch", "pitch"},
    {HK_PACK_PROPERTY_LOOP_COUNT, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_LOOP_COUNT), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_LOOP_COUNT_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_LOOP_COUNT_DEFAULT), PlayablePropertyReferenceRoleV1::None, "audio", "loopCount", "loopCount", "loopCount"},
    {HK_PACK_PROPERTY_STREAMING_MODE, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_STREAMING_MODE), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_STREAMING_MODE_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_STREAMING_MODE_DEFAULT), PlayablePropertyReferenceRoleV1::None, "audio", "streamingMode", "streamingMode", "streamingMode"},
    {HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_DEFAULT), PlayablePropertyReferenceRoleV1::None, "mixnode", "outputBusVolume", "outputBusVolume", "outputBusVolume"},
    {HK_PACK_PROPERTY_OUTPUT_BUS_REF, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_OUTPUT_BUS_REF), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_OUTPUT_BUS_REF_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_OUTPUT_BUS_REF_DEFAULT), PlayablePropertyReferenceRoleV1::OutputBus, "mixnode", "outputBusRef", "outputBusRef", "outputBusRef"},
    {HK_PACK_PROPERTY_SENDS, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SENDS), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SENDS_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SENDS_DEFAULT), PlayablePropertyReferenceRoleV1::None, "mixnode", "sends", "sends", "sends"},
    {HK_PACK_PROPERTY_SPATIAL_PANNING, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_PANNING), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_PANNING_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_PANNING_DEFAULT), PlayablePropertyReferenceRoleV1::None, "spatial", "panning", "spatialPanning", "spatial.panning"},
    {HK_PACK_PROPERTY_SPATIAL_SPREAD, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_SPREAD), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_SPREAD_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_SPREAD_DEFAULT), PlayablePropertyReferenceRoleV1::None, "spatial", "spread", "spatialSpread", "spatial.spread"},
    {HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED_DEFAULT), PlayablePropertyReferenceRoleV1::None, "spatial", "is3DEnabled", "spatialIs3DEnabled", "spatial.is3DEnabled"},
    {HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_DEFAULT), PlayablePropertyReferenceRoleV1::None, "spatial", "attenuationDistanceScaling", "spatialAttenuationDistanceScaling", "spatial.attenuationDistanceScaling"},
    {HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF_DEFAULT), PlayablePropertyReferenceRoleV1::Attenuation, "spatial", "attenuationRef", "spatialAttenuationRef", "spatial.attenuationRef"},
    {HK_PACK_PROPERTY_SIGNAL_GRAPH, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SIGNAL_GRAPH), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SIGNAL_GRAPH_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SIGNAL_GRAPH_DEFAULT), PlayablePropertyReferenceRoleV1::None, "mixnode", "signalGraph", "signalGraph", "signalGraph"},
    {HK_PACK_PROPERTY_SPATIALIZATION_MIX, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIALIZATION_MIX), static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIALIZATION_MIX_WIRE), static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIALIZATION_MIX_DEFAULT), PlayablePropertyReferenceRoleV1::None, "spatial", "spatializationMix", "spatializationMix", "spatial.spatializationMix"},
};
static_assert(std::size(kJsonFieldsV1) == 14);

} }

#endif
