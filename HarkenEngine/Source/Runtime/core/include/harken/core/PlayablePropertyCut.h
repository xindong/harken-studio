#ifndef HARKEN_CORE_PLAYABLE_PROPERTY_CUT_H
#define HARKEN_CORE_PLAYABLE_PROPERTY_CUT_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/PlaybackPath.h"
#include "harken/core/ProgramHandle.h"

#include <cstdint>
#include <vector>

namespace harken {

class PackPlaybackView;

inline constexpr bool IsValidAuthoredSpreadPercent(float spread) noexcept {
    return spread >= 0.0f && spread <= 100.0f;
}

inline constexpr float AuthoredSpreadPercentToRuntimeWidth(
    float spread) noexcept {
    return spread * 0.01f;
}

struct PlayableWorldViewV1 {
    WorldGeneration generation = 0;
    WorldIdentityV1 identity{};
    const PlayablePropertyProgramViewV1* properties = nullptr;
    std::uint32_t propertyCount = 0;
    const PlayableOccurrencePropertyViewV1* occurrences = nullptr;
    std::uint32_t occurrenceCount = 0;
    const TrackScheduleProgramViewV1* trackSchedules = nullptr;
    std::uint32_t trackScheduleCount = 0;
    const PieceScheduleProgramViewV1* pieceSchedules = nullptr;
    std::uint32_t pieceScheduleCount = 0;
    std::uint32_t maxKnownScheduleLeaves = 16;
    std::uint32_t maxTrackPieceScheduleDepth = 8;
    const ContainerProgramViewV3* containers = nullptr;
    std::uint32_t containerCount = 0;
    const TransitionProgramViewV1* transitions = nullptr;
    std::uint32_t transitionCount = 0;
    std::uint32_t sourceBodyVintage = kCurrentWorldShippingBodyVintage;
    std::uint64_t ownerRevision = 0;
    std::uint64_t leaseIdentity = 0;
    std::uint64_t ownerStableId = 0;

    ProgramSourceIdentityV1 ProgramSource() const noexcept {
        return {generation, ownerStableId, ownerRevision, leaseIdentity, sourceBodyVintage};
    }
    std::uint32_t InitialClosureCapabilities() const noexcept {
        return sourceBodyVintage == kCurrentWorldBodyVintageV19
            ? kProgramCapabilityEntrySchedulingV1 : 0u;
    }
};

enum class ResolvedPlayablePathRowKindV1 : std::uint8_t {
    OrganizationLayer = 0,
    Definition = 1,
    Occurrence = 2,
};

struct ResolvedPlayablePathRowV1 {
    ResolvedPlayablePathRowKindV1 rowKind =
        ResolvedPlayablePathRowKindV1::Definition;
    playableproperties::ContentPropertyKindV1 contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Sound;
    VnObjectId stableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint32_t stableOccurrenceOrdinal = UINT32_MAX;
    VnObjectId clipId{};
    ProgramSourceIdentityV1 source{};
};

struct ResolvedPlayableSpatialV1 {
    float panning = 0.0f;
    // Authoring percent in [0, 100]. Convert only when installing Runtime Width.
    float spread = 0.0f;
    float spatializationMix = 1.0f;
    bool is3DEnabled = false;
    float attenuationDistanceScaling = 1.0f;
    VnObjectId attenuationRef{};
};

struct ResolvedPlayablePropertyCutViewV1 {
    WorldGeneration world = 0;
    std::uint64_t worldSemanticFingerprint = 0;
    PlayableRefV1 root{};
    PlayableRefV1 leaf{};
    float volumeDb = 0.0f;
    float pitchCents = 0.0f;
    float makeUpGainDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    std::uint32_t loopCount = 1;
    std::uint32_t streamingMode = 0;
    VnObjectId mainOutputBusRef{};
    ResolvedPlayableSpatialV1 spatial{};
    std::uint32_t pathCount = 0;
    const ResolvedPlayablePathRowV1* path = nullptr;
    std::uint32_t sendCount = 0;
    const SendV1* sends = nullptr;
    std::uint32_t effectNodeCount = 0;
    const EffectNodeV1* effectNodes = nullptr;
    std::uint32_t effectEdgeCount = 0;
    const EffectEdgeV1* effectEdges = nullptr;
};

struct ResolvedPlayablePropertyCutConfigV1 {
    std::uint32_t maxPathRows = kPlaybackPathRowCapacity;
    std::uint32_t maxSends = 16;
    std::uint32_t maxEffectNodes = 64;
    std::uint32_t maxEffectParams = 256;
    std::uint32_t maxEffectEdges = 64;
};

enum class ResolvedPlayablePropertyCutStatusV1 : std::uint8_t {
    Ok = 0,
    InvalidInput = 1,
    UnknownPlayable = 2,
    PathMismatch = 3,
    UnsupportedPlayable = 4,
    Capacity = 5,
};

class ResolvedPlayablePropertyCutStorageV1 {
public:
    bool Reset(const ResolvedPlayablePropertyCutConfigV1& config) noexcept;
    ResolvedPlayablePropertyCutStatusV1 Resolve(
        const PlayableWorldViewV1& world, const PlayableRefV1& root,
        const std::uint32_t* selectedChildOrdinals,
        std::uint32_t selectedChildCount,
        const VnObjectId* selectedTrackIds = nullptr) noexcept;
    ResolvedPlayablePropertyCutStatusV1 Resolve(
        const PackPlaybackView& world, const PlayableRefV1& root,
        const std::uint32_t* selectedChildOrdinals,
        std::uint32_t selectedChildCount,
        const VnObjectId* selectedTrackIds = nullptr) noexcept;
    const ResolvedPlayablePropertyCutViewV1& View() const noexcept {
        return active_.view;
    }

private:
    struct Buffer {
        ResolvedPlayablePropertyCutViewV1 view{};
        std::vector<ResolvedPlayablePathRowV1> path;
        std::vector<const PlayablePropertyPayloadViewV1*> payloads;
        std::vector<SendV1> sends;
        std::vector<EffectNodeV1> effectNodes;
        std::vector<EffectParamV1> effectParams;
        std::vector<std::uint32_t> effectParamOffsets;
        std::vector<EffectEdgeV1> effectEdges;

        void Clear() noexcept;
        void Refresh() noexcept;
        void Swap(Buffer& other) noexcept;
    };

    static bool Reserve(Buffer& buffer,
                        const ResolvedPlayablePropertyCutConfigV1& config);
    template<class World>
    ResolvedPlayablePropertyCutStatusV1 ResolveInto(
        Buffer& output, const World& world, const PlayableRefV1& root,
        const std::uint32_t* selectedChildOrdinals,
        std::uint32_t selectedChildCount,
        const VnObjectId* selectedTrackIds) noexcept;

    ResolvedPlayablePropertyCutConfigV1 config_{};
    Buffer active_{};
    Buffer candidate_{};
};

}

#endif
