#ifndef HARKEN_CORE_PACK_PLAYBACK_VIEW_H
#define HARKEN_CORE_PACK_PLAYBACK_VIEW_H

#include <harken/ObjectId.h>
#include "harken/abi/PackRevisionIdentity.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/ProgramHandle.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

struct ResolvedPlayablePathRowV1;

struct PackAttenuationBindingMapV1 {
    struct Binding {
        VnAttenuationId id{};
        std::uint64_t generation = 0;
    };
    std::vector<Binding> entries;
    std::uint32_t count = 0;

    bool Valid() const noexcept {
        if (count > entries.size()) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (entries[i].id == VnObjectId{} || entries[i].generation == 0 ||
                (i != 0 && entries[i - 1].id >= entries[i].id)) {
                return false;
            }
        }
        return true;
    }

    std::uint64_t GenerationFor(VnAttenuationId id) const noexcept {
        for (std::uint32_t i = 0; i < count && i < entries.size(); ++i) {
            if (entries[i].id == id) return entries[i].generation;
        }
        return 0;
    }
};

struct PackVoiceBinding {
    VnObjectId objectId = {};
    VnBindingId bindingId{};
    VnBindingDesc descriptor{};
    Curve curve;
    std::uint32_t revision = 1;
};

struct PackClosureOwnerSnapshot {
    std::uint64_t packStableId = 0;
    VnPackHandle handle = 0;
    std::uint64_t contentRevision = 0;
    std::uint64_t partitionRevision = 0;
    std::uint64_t resourceRevision = 0;
    PackResourceRevisionToken resourceToken = 0;
    std::shared_ptr<const CurrentWorldBodyImage> definitionBody;
    ContainerWorldInstallViewV1 definitionView{};
    std::shared_ptr<const std::vector<PackVoiceBinding>> voiceBindings;
};

struct PackPlaybackOwnerStore {
    struct Owner {
        PackClosureOwnerSnapshot snapshot{};
        std::vector<std::uint32_t> closureOwnerSlots;
        std::shared_ptr<const std::vector<PackResourceRevisionToken>> resourceTokens;
    };

    std::vector<Owner> owners;
    std::vector<ProgramCapabilitySummaryV1> programCapabilities;
    std::uint64_t worldSemanticFingerprint = 0;
    PackAttenuationBindingMapV1 attenuationBindings;
};

struct PackFrozenClosure {
    std::uint64_t compositeGeneration = 0;
    std::uint64_t rootPackStableId = 0;
    std::shared_ptr<const PackPlaybackOwnerStore> ownerStore;
    std::uint32_t rootOwnerIndex = UINT32_MAX;
    std::uint32_t ownerCount = 0;
    std::uint64_t leaseIdentity = 0;
};

class PackPlaybackView {
public:
    VnObjectId ProjectId() const noexcept;
    std::uint64_t CompositeGeneration() const noexcept;
    std::uint64_t WorldSemanticFingerprint() const noexcept;
    std::uint64_t RootPackStableId() const noexcept;
    std::uint32_t OwnerCount() const noexcept;
    std::uint64_t AttenuationGeneration(VnAttenuationId id) const noexcept;

    const PlayablePropertyProgramViewV1* ResolveProperty(
        playableproperties::ContentPropertyKindV1 kind,
        VnObjectId stableId) const noexcept;
    ContainerProgramHandleV1 ResolveContainer(
        VnObjectId stableId, std::uint64_t expectedFingerprint) const noexcept;
    TrackProgramHandleV1 ResolveTrack(
        VnObjectId stableId,
        std::uint64_t expectedScheduleFingerprint) const noexcept;
    SegmentProgramHandleV1 ResolveSegment(
        VnObjectId stableId,
        std::uint64_t expectedScheduleFingerprint) const noexcept;
    ProgramSourceIdentityV1 ResolveProgramSource(
        playableproperties::ContentPropertyKindV1 kind, VnObjectId stableId) const noexcept;
    std::uint32_t InitialClosureCapabilities(
        PlayableKindV1 kind, VnObjectId stableId) const noexcept;
    const PlayableOccurrencePropertyViewV1* ResolveOccurrence(
        playableproperties::ContentPropertyKindV1 parentKind,
        VnObjectId parentStableId, std::uint32_t ordinal) const noexcept;
    const TransitionProgramViewV1* ResolveTransition(
        VnObjectId ownerContainerStableId) const noexcept;
    bool ResolveSoundResource(
        VnObjectId stableId, std::uint64_t expectedRevision,
        const PackClosureOwnerSnapshot** outOwner) const noexcept;
    std::uint32_t ResolveVoiceBindings(
        VnObjectId objectId, const PackVoiceBinding** out,
        std::uint32_t capacity) const noexcept;
    std::uint32_t CollectCutVoiceBindings(
        const ResolvedPlayablePathRowV1* path, std::uint32_t pathCount,
        const PackVoiceBinding** out, std::uint32_t capacity) const noexcept;

private:
    friend class PackComposition;
    const PackPlaybackOwnerStore::Owner* OwnerAt(
        std::uint32_t closureIndex) const noexcept;
    ProgramSourceIdentityV1 SourceOf(const PackClosureOwnerSnapshot& owner) const noexcept;
    const PackFrozenClosure* closure_ = nullptr;
    const PackPlaybackOwnerStore* ownerStore_ = nullptr;
};

// Installation-only traversal over actual definition references, including providers.
bool BuildProgramCapabilitySummariesV1(
    const ContainerWorldInstallViewV1* worlds, std::uint32_t worldCount,
    std::vector<ProgramCapabilitySummaryV1>& out);

std::uint32_t CollectCutVoiceBindings(
    const ResolvedPlayablePathRowV1* path, std::uint32_t pathCount,
    const PackVoiceBinding* catalog, std::uint32_t catalogCount,
    const PackVoiceBinding** out, std::uint32_t capacity) noexcept;

} // namespace harken

#endif
