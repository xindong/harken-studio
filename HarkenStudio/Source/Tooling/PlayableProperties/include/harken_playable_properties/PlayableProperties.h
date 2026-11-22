#ifndef HARKEN_PLAYABLE_PROPERTIES_PLAYABLE_PROPERTIES_H
#define HARKEN_PLAYABLE_PROPERTIES_PLAYABLE_PROPERTIES_H

#include <harken_pack/PackProperties.h>

#include <cstdint>
#include <iterator>
#include <optional>

namespace harkenplayableproperties {

enum class ContentPropertyKindV1 : std::uint8_t {
    Layer = HK_PACK_CONTENT_LAYER,
    Sound = HK_PACK_CONTENT_SOUND,
    Container = HK_PACK_CONTENT_CONTAINER,
    Track = HK_PACK_CONTENT_TRACK,
    Segment = HK_PACK_CONTENT_SEGMENT,
};

enum class PlayablePropertyIdV1 : std::uint8_t {
    Volume = HK_PACK_PROPERTY_VOLUME,
    Pitch = HK_PACK_PROPERTY_PITCH,
    LoopCount = HK_PACK_PROPERTY_LOOP_COUNT,
    StreamingMode = HK_PACK_PROPERTY_STREAMING_MODE,
    OutputBusVolume = HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME,
    OutputBusRef = HK_PACK_PROPERTY_OUTPUT_BUS_REF,
    Sends = HK_PACK_PROPERTY_SENDS,
    SpatialPanning = HK_PACK_PROPERTY_SPATIAL_PANNING,
    SpatialSpread = HK_PACK_PROPERTY_SPATIAL_SPREAD,
    SpatialIs3DEnabled = HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED,
    SpatialAttenuationDistanceScaling = HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING,
    SpatialAttenuationRef = HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF,
    SignalGraph = HK_PACK_PROPERTY_SIGNAL_GRAPH,
    SpatializationMix = HK_PACK_PROPERTY_SPATIALIZATION_MIX,
};

enum class PlayablePropertyPresenceV1 : std::uint8_t {
    MaskBit = HK_PACK_PROPERTY_PRESENCE_MASK_BIT,
};

enum class PlayablePropertyWireV1 : std::uint8_t {
    F32 = HK_PACK_PROPERTY_WIRE_F32,
    U8 = HK_PACK_PROPERTY_WIRE_U8,
    U32 = HK_PACK_PROPERTY_WIRE_U32,
    ObjectId = HK_PACK_PROPERTY_WIRE_OBJECT_ID,
    RowSpan = HK_PACK_PROPERTY_WIRE_ROW_SPAN,
    EffectProgram = HK_PACK_PROPERTY_WIRE_EFFECT_PROGRAM,
};

enum class PlayablePropertyCombineRoleV1 : std::uint8_t {
    DbSum = 0,
    CentSumClamp = 1,
    ObjectLocal = 2,
    LeafMaterialization = 3,
    NearestExplicit = 4,
    SendResolution = 5,
    EffectSelection = 6,
};

enum class PlayablePropertyReferenceRoleV1 : std::uint8_t {
    None = 0,
    OutputBus = 1,
    Attenuation = 2,
};

enum class PlayablePropertyCapabilityRoleV1 : std::uint8_t {
    Common = 0,
    Playback = 1,
    Streaming = 2,
    Route = 3,
    Send = 4,
    Spatial = 5,
    Reserved = 6,
    Effect = 7,
};

enum class PlayablePropertyPhaseRoleV1 : std::uint8_t {
    FirstAudible = 0,
    PlaybackMaterialization = 1,
    LeafMaterialization = 2,
    Route = 3,
    SameCutInstall = 4,
};

enum class PlayablePropertyDefaultV1 : std::uint8_t {
    PositiveZero = HK_PACK_PROPERTY_DEFAULT_POSITIVE_ZERO,
    One = HK_PACK_PROPERTY_DEFAULT_ONE,
    Zero = HK_PACK_PROPERTY_DEFAULT_ZERO,
    Empty = HK_PACK_PROPERTY_DEFAULT_EMPTY,
    IntegerOne = HK_PACK_PROPERTY_DEFAULT_INTEGER_ONE,
};

inline constexpr std::uint64_t kPlayablePropertyKnownMaskV1 = HK_PACK_PROPERTY_KNOWN_MASK & ~HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_MAKE_UP_GAIN);

struct ChassisPresenceV1 {
    std::uint64_t bits = 0;
    constexpr bool IsValid() const noexcept {
        return (bits & ~kPlayablePropertyKnownMaskV1) == 0;
    }
    constexpr bool Has(PlayablePropertyIdV1 id) const noexcept {
        const auto ordinal = static_cast<std::uint8_t>(id);
        return ordinal >= 1 && ordinal <= 17 && ordinal != 3 && ordinal != 13 && ordinal != 15 &&
               (bits & (UINT64_C(1) << (ordinal - 1))) != 0;
    }
    constexpr void Set(PlayablePropertyIdV1 id) noexcept {
        const auto ordinal = static_cast<std::uint8_t>(id);
        if (ordinal >= 1 && ordinal <= 17 && ordinal != 3 && ordinal != 13 && ordinal != 15) bits |= UINT64_C(1) << (ordinal - 1);
    }
    constexpr void Clear(PlayablePropertyIdV1 id) noexcept {
        const auto ordinal = static_cast<std::uint8_t>(id);
        if (ordinal >= 1 && ordinal <= 17 && ordinal != 3 && ordinal != 13 && ordinal != 15) bits &= ~(UINT64_C(1) << (ordinal - 1));
    }
};

struct PlayablePropertyMetaV1 {
    PlayablePropertyIdV1 id; std::uint64_t presentBit; const char* name; const char* jsonKey;
    PlayablePropertyWireV1 wire; PlayablePropertyPresenceV1 presence;
    PlayablePropertyCombineRoleV1 combineRole;
    PlayablePropertyReferenceRoleV1 referenceRole;
    PlayablePropertyCapabilityRoleV1 capabilityRole;
    PlayablePropertyPhaseRoleV1 phaseRole; PlayablePropertyDefaultV1 canonicalDefault;
};

inline constexpr PlayablePropertyMetaV1 kPlayablePropertyRegistryV1[] = {
    {PlayablePropertyIdV1::Volume, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_VOLUME), "volume", "volume", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_VOLUME_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::DbSum, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Common, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_VOLUME_DEFAULT)},
    {PlayablePropertyIdV1::Pitch, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_PITCH), "pitch", "pitch", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_PITCH_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::CentSumClamp, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Common, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_PITCH_DEFAULT)},
    {PlayablePropertyIdV1::LoopCount, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_LOOP_COUNT), "loopCount", "loopCount", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_LOOP_COUNT_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::ObjectLocal, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Playback, PlayablePropertyPhaseRoleV1::PlaybackMaterialization, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_LOOP_COUNT_DEFAULT)},
    {PlayablePropertyIdV1::StreamingMode, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_STREAMING_MODE), "streamingMode", "streamingMode", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_STREAMING_MODE_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Streaming, PlayablePropertyPhaseRoleV1::LeafMaterialization, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_STREAMING_MODE_DEFAULT)},
    {PlayablePropertyIdV1::OutputBusVolume, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME), "outputBusVolume", "outputBusVolume", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Route, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_DEFAULT)},
    {PlayablePropertyIdV1::OutputBusRef, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_OUTPUT_BUS_REF), "outputBusRef", "outputBusRef", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_OUTPUT_BUS_REF_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::OutputBus, PlayablePropertyCapabilityRoleV1::Route, PlayablePropertyPhaseRoleV1::Route, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_OUTPUT_BUS_REF_DEFAULT)},
    {PlayablePropertyIdV1::Sends, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SENDS), "sends", "sends", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SENDS_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::SendResolution, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Send, PlayablePropertyPhaseRoleV1::SameCutInstall, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SENDS_DEFAULT)},
    {PlayablePropertyIdV1::SpatialPanning, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_PANNING), "spatialPanning", "spatial.panning", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_PANNING_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Spatial, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_PANNING_DEFAULT)},
    {PlayablePropertyIdV1::SpatialSpread, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_SPREAD), "spatialSpread", "spatial.spread", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_SPREAD_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Spatial, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_SPREAD_DEFAULT)},
    {PlayablePropertyIdV1::SpatialIs3DEnabled, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED), "spatialIs3DEnabled", "spatial.is3DEnabled", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Spatial, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_IS3D_ENABLED_DEFAULT)},
    {PlayablePropertyIdV1::SpatialAttenuationDistanceScaling, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING), "spatialAttenuationDistanceScaling", "spatial.attenuationDistanceScaling", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Spatial, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_DEFAULT)},
    {PlayablePropertyIdV1::SpatialAttenuationRef, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF), "spatialAttenuationRef", "spatial.attenuationRef", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::Attenuation, PlayablePropertyCapabilityRoleV1::Spatial, PlayablePropertyPhaseRoleV1::Route, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIAL_ATTENUATION_REF_DEFAULT)},
    {PlayablePropertyIdV1::SignalGraph, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SIGNAL_GRAPH), "signalGraph", "signalGraph", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SIGNAL_GRAPH_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::EffectSelection, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Effect, PlayablePropertyPhaseRoleV1::SameCutInstall, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SIGNAL_GRAPH_DEFAULT)},
    {PlayablePropertyIdV1::SpatializationMix, HK_PACK_PROPERTY_BIT(HK_PACK_PROPERTY_SPATIALIZATION_MIX), "spatializationMix", "spatial.spatializationMix", static_cast<PlayablePropertyWireV1>(HK_PACK_PROPERTY_SPATIALIZATION_MIX_WIRE), PlayablePropertyPresenceV1::MaskBit, PlayablePropertyCombineRoleV1::NearestExplicit, PlayablePropertyReferenceRoleV1::None, PlayablePropertyCapabilityRoleV1::Spatial, PlayablePropertyPhaseRoleV1::FirstAudible, static_cast<PlayablePropertyDefaultV1>(HK_PACK_PROPERTY_SPATIALIZATION_MIX_DEFAULT)},
};
static_assert(std::size(kPlayablePropertyRegistryV1) == 14);

struct PlayablePropertyRangeV1 {
    double minimum;
    double maximum;
};

constexpr std::optional<PlayablePropertyRangeV1> PublishedRangeV1(PlayablePropertyIdV1 id) noexcept {
    switch (id) {
    case PlayablePropertyIdV1::Volume:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_VOLUME_MIN, HK_PACK_PROPERTY_VOLUME_MAX};
    case PlayablePropertyIdV1::Pitch:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_PITCH_MIN, HK_PACK_PROPERTY_PITCH_MAX};
    case PlayablePropertyIdV1::LoopCount:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_LOOP_COUNT_MIN, HK_PACK_PROPERTY_LOOP_COUNT_MAX};
    case PlayablePropertyIdV1::OutputBusVolume:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_MIN, HK_PACK_PROPERTY_OUTPUT_BUS_VOLUME_MAX};
    case PlayablePropertyIdV1::SpatialPanning:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_SPATIAL_PANNING_MIN, HK_PACK_PROPERTY_SPATIAL_PANNING_MAX};
    case PlayablePropertyIdV1::SpatialSpread:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_SPATIAL_SPREAD_MIN, HK_PACK_PROPERTY_SPATIAL_SPREAD_MAX};
    case PlayablePropertyIdV1::SpatialAttenuationDistanceScaling:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_MIN,
                                       HK_PACK_PROPERTY_SPATIAL_ATTENUATION_DISTANCE_SCALING_MAX};
    case PlayablePropertyIdV1::SpatializationMix:
        return PlayablePropertyRangeV1{HK_PACK_PROPERTY_SPATIALIZATION_MIX_MIN, HK_PACK_PROPERTY_SPATIALIZATION_MIX_MAX};
    default:
        return std::nullopt;
    }
}
static_assert(sizeof(ChassisPresenceV1) == sizeof(std::uint64_t));

}

#endif
