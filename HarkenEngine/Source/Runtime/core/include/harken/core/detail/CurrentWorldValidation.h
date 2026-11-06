#ifndef HARKEN_CORE_DETAIL_CURRENT_WORLD_VALIDATION_H
#define HARKEN_CORE_DETAIL_CURRENT_WORLD_VALIDATION_H

#include "harken/core/CurrentWorld.h"
#include "harken/core/detail/PackDecodeBudget.h"
#include <span>

namespace harken::detail {

#if defined(__GNUC__) || defined(__clang__)
#define HARKEN_CORE_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define HARKEN_CORE_DETAIL_HIDDEN
#endif

// Sequence selection follows the next cursor or wraps; Reset returns to the
// authored initial cursor. Other directed pairs cannot select a transition.
inline bool SequenceQueueTransitionPairReachableV1(const ContainerProgramViewV3& owner,
    std::uint32_t sourceIndex, std::uint32_t targetIndex) noexcept {
    if (owner.mode != ContainerModeV1::SequenceQueue) return true;
    if (sourceIndex >= owner.childCount || targetIndex >= owner.childCount) return false;
    return targetIndex == owner.initialSequenceCursor || targetIndex == sourceIndex + 1u ||
        (sourceIndex + 1u == owner.childCount && targetIndex == 0 &&
         owner.queueCycle == QueueCycleV1::Loop && owner.queueCycleCount != 1);
}

// Composite rows retain the contract of their owning partition. This context
// is used only while validating Runtime Pack composition, never as a wire format.
struct CompositeWorldVintagesV1 {
    std::span<const std::uint32_t> segments;
    std::span<const std::uint32_t> actions;
    bool hasEntryScheduling = false;
};

struct MusicSourceValidationOwnerV1 {
    const ContainerWorldInstallViewV1* world = nullptr;
    std::span<const std::uint32_t> segmentVintages;
};

struct MusicSourceNodeV1 {
    const SegmentScheduleProgramViewV1* segment = nullptr;
    const TrackScheduleProgramViewV1* track = nullptr;
    const ContainerProgramViewV3* container = nullptr;
    std::uint64_t loopCount = 1;
    std::uint32_t vintage = 0;
    std::uint32_t maxDepth = 0;
    std::uint32_t maxChildren = 0;
};

// Installation views and prepared program handles share the same shape rule.
// The resolver authenticates each node in its own partition without retaining it.
template <class ResolveNode>
bool TransparentMusicSourceShapeV1(const PlayableRefV1& source,
    const TransitionRuleViewV1& rule, const ResolveNode& resolve,
    std::uint32_t depth = 1) noexcept {
    const auto node = resolve(source);
    if (depth == 0 || depth > node.maxDepth) return false;
    if (source.kind == PlayableKindV1::Segment) {
        if (node.segment == nullptr || node.vintage != kCurrentWorldBodyVintageV19) return false;
        const auto& segment = *node.segment;
        return rule.sourceExit <= TransitionSourceExitV1::NaturalEnd
            ? segment.bounds.nominalEnd > segment.bounds.nominalStart && segment.entryMarker.stableMarkerId != VnObjectId{}
            : NativeMusicTransitionSourceCompatibleV1(rule, segment);
    }
    if (source.kind == PlayableKindV1::Track) return node.track != nullptr &&
        node.track->placementCount == 1 && node.track->placements != nullptr &&
        TransparentMusicSourceShapeV1(node.track->placements[0].content, rule, resolve, depth + 1u);
    if (node.loopCount != 1) return false;
    if (source.kind != PlayableKindV1::Container || node.container == nullptr) return false;
    const auto& container = *node.container;
    if ((container.mode != ContainerModeV1::Random && container.mode != ContainerModeV1::Sequence) ||
        container.childCount == 0 || container.children == nullptr || container.childCount > node.maxChildren) return false;
    for (std::uint32_t child = 0; child < container.childCount; ++child)
        if (!TransparentMusicSourceShapeV1(container.children[child].playable, rule, resolve, depth + 1u)) return false;
    return true;
}

inline MusicSourceNodeV1 ResolveInstalledMusicSourceNodeV1(const PlayableRefV1& source,
    const MusicSourceValidationOwnerV1& owner) noexcept {
    MusicSourceNodeV1 node{};
    if (owner.world == nullptr) return node;
    const auto& world = *owner.world;
    node.maxDepth = world.capacity.maxContainerDepth;
    node.maxChildren = world.capacity.maxChildrenPerContainer;
    const auto same = [&](const PlayableRefV1& ref) noexcept {
        return ref.kind == source.kind && ref.stableId == source.stableId &&
            ref.expectedProgramSemanticFingerprint == source.expectedProgramSemanticFingerprint &&
            ref.contentRevision == source.contentRevision && ref.routeStableId == source.routeStableId &&
            ref.compatibilityFingerprint == source.compatibilityFingerprint;
    };
    if (source.kind == PlayableKindV1::Segment) {
        if (world.segmentSchedules == nullptr) return node;
        for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
            const auto& segment = world.segmentSchedules[i];
            if (!same(segment.rootDefinition)) continue;
            node.vintage = owner.segmentVintages.empty() ? world.sourceBodyVintage
                : i < owner.segmentVintages.size() ? owner.segmentVintages[i] : 0u;
            node.segment = &segment;
            return node;
        }
        return node;
    }
    if (source.kind != PlayableKindV1::Track && source.kind != PlayableKindV1::Container) return node;
    const auto kind = source.kind == PlayableKindV1::Track
        ? playableproperties::ContentPropertyKindV1::Track : playableproperties::ContentPropertyKindV1::Container;
    constexpr auto loopSlot = static_cast<std::size_t>(playableproperties::PlayablePropertyIdV1::LoopCount) - 1u;
    if (world.playablePropertyCount != 0 && world.playableProperties == nullptr) return {};
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto& property = world.playableProperties[i];
        if (property.contentPropertyKind == kind && property.stableId == source.stableId &&
            (property.payload.presentMask & (UINT64_C(1) << loopSlot)) != 0)
            node.loopCount = property.payload.values[loopSlot].bits;
    }
    if (source.kind == PlayableKindV1::Track) {
        if (world.trackSchedules == nullptr) return node;
        for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
            const auto& track = world.trackSchedules[i];
            if (same(track.rootDefinition)) { node.track = &track; return node; }
        }
        return node;
    }
    if (world.containers == nullptr) return node;
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        const auto& container = world.containers[i];
        if (container.containerStableId != source.stableId ||
            container.programSemanticFingerprint != source.expectedProgramSemanticFingerprint) continue;
        node.container = &container;
        return node;
    }
    return node;
}

template <class ResolveOwner>
bool TransparentMusicSourceCompatibleV1(const PlayableRefV1& source,
    const TransitionRuleViewV1& rule, const ResolveOwner& resolve) noexcept {
    return TransparentMusicSourceShapeV1(source, rule, [&](const auto& ref) noexcept {
        return ResolveInstalledMusicSourceNodeV1(ref, resolve(ref));
    });
}

HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldCompositeV1(
    const ContainerWorldInstallViewV1& world,
    const CompositeWorldVintagesV1& vintages, const PackDecodeBudget& budget = {}) noexcept;

HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldPartitionLocalV1(
    const ContainerWorldInstallViewV1& world) noexcept;

HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldWithBudget(
    const ContainerWorldInstallViewV1& world, const PackDecodeBudget& budget) noexcept;

HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldPartitionLocalWithBudget(
    const ContainerWorldInstallViewV1& world, const PackDecodeBudget& budget) noexcept;
HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldSectionGroupsWithBudget(
    const ContainerWorldInstallViewV1& world, const PackDecodeBudget& budget) noexcept;

HARKEN_CORE_DETAIL_HIDDEN std::uint64_t ComputeCurrentWorldLocalCapabilityMaskV1(
    const ContainerWorldInstallViewV1& world) noexcept;
HARKEN_CORE_DETAIL_HIDDEN std::uint64_t ComputeCurrentWorldLocalCapabilityMaskV1(
    const HistoricalContainerWorldViewV1& world) noexcept;

#undef HARKEN_CORE_DETAIL_HIDDEN

} // namespace harken::detail

#endif // HARKEN_CORE_DETAIL_CURRENT_WORLD_VALIDATION_H
