#include <harken/core/BindingTransition.h>
#include "harken/core/PackComposition.h"

#include "harken/core/EngineConfig.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/core/VariableExecutionFingerprint.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/param/VariableService.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace harken {

namespace {

constexpr std::uint64_t kIndexEntryBytes = 16 + 8 + 4 + 4;
std::atomic<std::uint64_t> nextOriginalPackIncarnation{1};
std::uint64_t IssueOriginalPackIncarnation() noexcept {
    auto next = nextOriginalPackIncarnation.load(std::memory_order_relaxed);
    while (next != 0) {
        const auto following = next == UINT64_MAX ? 0 : next + 1u;
        if (nextOriginalPackIncarnation.compare_exchange_weak(next, following,
                std::memory_order_relaxed)) return next;
    }
    return 0;
}

std::uint64_t ProviderClosureFingerprint(const PackComposition::PublishedOwner& owner) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> required{
        {owner.packStableId, owner.contentRevision}};
    for (const auto& edge : owner.dependencyEdges) {
        if (edge.required01 != 0)
            required.emplace_back(edge.prerequisitePackStableId,
                                  edge.expectedPrerequisiteContentRevision);
    }
    std::sort(required.begin(), required.end());
    required.erase(std::unique(required.begin(), required.end()), required.end());
    std::uint64_t hash = Fnv1a64(nullptr, 0);
    for (const auto& [id, revision] : required) {
        std::uint8_t bytes[16];
        StoreLE64(bytes, id);
        StoreLE64(bytes + 8, revision);
        hash = Fnv1a64(bytes, sizeof(bytes), hash);
    }
    return hash;
}

const PackComposition::IdentityIndexEntry* BinaryFind(
    const std::vector<PackComposition::IdentityIndexEntry>& index,
    VnObjectId stableId) noexcept {
    std::size_t lo = 0;
    std::size_t hi = index.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const VnObjectId midId = index[mid].stableId;
        if (midId < stableId) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < index.size() && index[lo].stableId == stableId
               ? &index[lo]
               : nullptr;
}

template <typename Predicate>
const PackComposition::IdentityIndexEntry* FindIdentityProvider(
    const PackComposition::PublishedView& published, VnObjectId stableId,
    Predicate acceptsSlot) noexcept {
    const auto* first = BinaryFind(published.index, stableId);
    if (first == nullptr) {
        return nullptr;
    }
    const auto* end = published.index.data() + published.index.size();
    for (const auto* hit = first; hit != end && hit->stableId == stableId; ++hit) {
        if (acceptsSlot(hit->packSlot)) {
            return hit;
        }
    }
    return nullptr;
}

const PackComposition::IdentityIndexEntry* FindIdentityInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    VnObjectId stableId) noexcept {
    return FindIdentityProvider(published, stableId, [&](std::uint32_t slot) {
        return slot < member.size() && member[slot] != 0;
    });
}

bool SameRef(const PlayableRefV1& a, const PlayableRefV1& b) noexcept {
    return a.kind == b.kind && a.stableId == b.stableId &&
           a.expectedProgramSemanticFingerprint ==
               b.expectedProgramSemanticFingerprint &&
           a.contentRevision == b.contentRevision &&
           a.routeStableId == b.routeStableId &&
           a.compatibilityFingerprint == b.compatibilityFingerprint;
}

bool PointerCountOk(std::uint32_t count, const void* pointer) noexcept {
    return count == 0 || pointer != nullptr;
}

const PlayablePropertyProgramViewV1* FindPlayableProperty(
    const ContainerWorldInstallViewV1& view,
    playableproperties::ContentPropertyKindV1 kind,
    VnObjectId stableId) noexcept {
    if (!PointerCountOk(view.playablePropertyCount, view.playableProperties) ||
        view.playablePropertyCount == 0) {
        return nullptr;
    }
    const auto key = std::pair(static_cast<std::uint32_t>(kind), stableId);
    const auto* begin = view.playableProperties;
    const auto* end = begin + view.playablePropertyCount;
    const auto* found = std::lower_bound(
        begin, end, key,
        [](const PlayablePropertyProgramViewV1& row,
           const std::pair<std::uint32_t, VnObjectId>& candidate) {
            return std::pair(static_cast<std::uint32_t>(row.contentPropertyKind),
                             row.stableId) < candidate;
        });
    if (found == end || found->contentPropertyKind != kind ||
        found->stableId != stableId) {
        return nullptr;
    }
    return found;
}

const ContainerProgramViewV3* FindContainerProgram(
    const ContainerWorldInstallViewV1& view, VnObjectId stableId) noexcept {
    if (!PointerCountOk(view.containerCount, view.containers) ||
        view.containerCount == 0) {
        return nullptr;
    }
    const auto* begin = view.containers;
    const auto* end = begin + view.containerCount;
    const auto* found = std::lower_bound(
        begin, end, stableId,
        [](const ContainerProgramViewV3& row, VnObjectId id) {
            return row.containerStableId < id;
        });
    if (found == end || found->containerStableId != stableId) {
        return nullptr;
    }
    return found;
}

const TrackScheduleProgramViewV1* FindTrackSchedule(
    const ContainerWorldInstallViewV1& view, VnObjectId stableId) noexcept {
    if (!PointerCountOk(view.trackScheduleCount, view.trackSchedules) ||
        view.trackScheduleCount == 0) {
        return nullptr;
    }
    const auto* begin = view.trackSchedules;
    const auto* end = begin + view.trackScheduleCount;
    const auto* found = std::lower_bound(
        begin, end, stableId,
        [](const TrackScheduleProgramViewV1& row, VnObjectId id) {
            return row.rootDefinition.stableId < id;
        });
    if (found == end || found->rootDefinition.stableId != stableId) {
        return nullptr;
    }
    return found;
}

const PieceScheduleProgramViewV1* FindPieceSchedule(
    const ContainerWorldInstallViewV1& view, VnObjectId stableId) noexcept {
    if (!PointerCountOk(view.pieceScheduleCount, view.pieceSchedules) ||
        view.pieceScheduleCount == 0) {
        return nullptr;
    }
    const auto* begin = view.pieceSchedules;
    const auto* end = begin + view.pieceScheduleCount;
    const auto* found = std::lower_bound(
        begin, end, stableId,
        [](const PieceScheduleProgramViewV1& row, VnObjectId id) {
            return row.rootDefinition.stableId < id;
        });
    if (found == end || found->rootDefinition.stableId != stableId) {
        return nullptr;
    }
    return found;
}

std::uint32_t CountPropertiesOfKind(
    const ContainerWorldInstallViewV1& view,
    playableproperties::ContentPropertyKindV1 kind) noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        if (view.playableProperties[i].contentPropertyKind == kind) {
            ++count;
        }
    }
    return count;
}

std::uint32_t CountOccurrencesForParent(
    const ContainerWorldInstallViewV1& view,
    playableproperties::ContentPropertyKindV1 parentKind,
    VnObjectId parentStableId) noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < view.playableOccurrenceCount; ++i) {
        const PlayableOccurrencePropertyViewV1& row = view.playableOccurrences[i];
        if (row.parentKind == parentKind && row.parentStableId == parentStableId) {
            ++count;
        }
    }
    return count;
}

const PlayableOccurrencePropertyViewV1* FindOccurrence(
    const ContainerWorldInstallViewV1& view,
    playableproperties::ContentPropertyKindV1 parentKind,
    VnObjectId parentStableId, std::uint32_t ordinal) noexcept {
    if (!PointerCountOk(view.playableOccurrenceCount, view.playableOccurrences) ||
        view.playableOccurrenceCount == 0) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < view.playableOccurrenceCount; ++i) {
        const PlayableOccurrencePropertyViewV1& row = view.playableOccurrences[i];
        if (row.parentKind == parentKind && row.parentStableId == parentStableId &&
            row.stableOccurrenceOrdinal == ordinal) {
            return &row;
        }
    }
    return nullptr;
}

bool ScheduleRootMatchesProperty(
    const PlayableRefV1& root,
    const PlayablePropertyProgramViewV1& property) noexcept {
    return root.stableId == property.stableId &&
           root.expectedProgramSemanticFingerprint ==
               property.programSemanticFingerprint &&
           root.contentRevision == property.contentRevision &&
           root.routeStableId == property.routeStableId &&
           root.compatibilityFingerprint == property.compatibilityFingerprint;
}

PlayableRefV1 RefFromProperty(
    const PlayablePropertyProgramViewV1& property) noexcept {
    PlayableRefV1 ref{};
    ref.kind = static_cast<PlayableKindV1>(
        static_cast<std::uint32_t>(property.contentPropertyKind) - 1u);
    ref.stableId = property.stableId;
    ref.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    ref.contentRevision = property.contentRevision;
    ref.routeStableId = property.routeStableId;
    ref.compatibilityFingerprint = property.compatibilityFingerprint;
    return ref;
}

bool CompanionTablesSorted(const ContainerWorldInstallViewV1& view) noexcept {
    if (!PointerCountOk(view.playablePropertyCount, view.playableProperties) ||
        !PointerCountOk(view.containerCount, view.containers) ||
        !PointerCountOk(view.trackScheduleCount, view.trackSchedules) ||
        !PointerCountOk(view.pieceScheduleCount, view.pieceSchedules) ||
        !PointerCountOk(view.playableOccurrenceCount, view.playableOccurrences) ||
        !PointerCountOk(view.transitionCount, view.transitions)) {
        return false;
    }
    for (std::uint32_t i = 1; i < view.playablePropertyCount; ++i) {
        const auto& prev = view.playableProperties[i - 1];
        const auto& cur = view.playableProperties[i];
        if (std::pair(static_cast<std::uint32_t>(prev.contentPropertyKind),
                      prev.stableId) >=
            std::pair(static_cast<std::uint32_t>(cur.contentPropertyKind),
                      cur.stableId)) {
            return false;
        }
    }
    for (std::uint32_t i = 1; i < view.containerCount; ++i) {
        if (view.containers[i - 1].containerStableId >=
            view.containers[i].containerStableId) {
            return false;
        }
    }
    for (std::uint32_t i = 1; i < view.trackScheduleCount; ++i) {
        if (view.trackSchedules[i - 1].rootDefinition.stableId >=
            view.trackSchedules[i].rootDefinition.stableId) {
            return false;
        }
    }
    for (std::uint32_t i = 1; i < view.pieceScheduleCount; ++i) {
        if (view.pieceSchedules[i - 1].rootDefinition.stableId >=
            view.pieceSchedules[i].rootDefinition.stableId) {
            return false;
        }
    }
    for (std::uint32_t i = 1; i < view.playableOccurrenceCount; ++i) {
        const auto& prev = view.playableOccurrences[i - 1];
        const auto& cur = view.playableOccurrences[i];
        if (std::pair(static_cast<std::uint32_t>(prev.parentKind),
                      prev.parentStableId) >
                std::pair(static_cast<std::uint32_t>(cur.parentKind),
                          cur.parentStableId) ||
            (prev.parentKind == cur.parentKind &&
             prev.parentStableId == cur.parentStableId &&
             prev.stableOccurrenceOrdinal >= cur.stableOccurrenceOrdinal)) {
            return false;
        }
    }
    for (std::uint32_t i = 1; i < view.transitionCount; ++i) {
        if (view.transitions[i - 1].ownerContainerStableId >=
            view.transitions[i].ownerContainerStableId) {
            return false;
        }
    }
    return true;
}

bool IdentityOwnedInSlot(const PackComposition::PublishedView& published,
                         std::uint32_t slot, VnObjectId stableId) noexcept {
    const PackComposition::IdentityIndexEntry* hit =
        FindIdentityProvider(published, stableId,
                             [slot](std::uint32_t candidate) { return candidate == slot; });
    return hit != nullptr;
}

bool TransitionEndpointIsDirectChild(const ContainerProgramViewV3& container,
                                     const PlayableRefV1& endpoint) noexcept {
    if (!PointerCountOk(container.childCount, container.children)) {
        return false;
    }
    for (std::uint32_t i = 0; i < container.childCount; ++i) {
        if (SameRef(container.children[i].playable, endpoint)) {
            return true;
        }
    }
    return false;
}

enum class BoundedTraversalStatus : std::uint8_t {
    Valid = 0,
    Invalid = 1,
    Exceeded = 2,
};

struct TraversalPathNode {
    const TraversalPathNode* parent;
    VnObjectId stableId;
};

const PackComposition::PublishedOwner* ResolvePlayableOwner(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const PlayableRefV1& endpoint) noexcept {
    if (static_cast<std::uint32_t>(endpoint.kind) >
        static_cast<std::uint32_t>(PlayableKindV1::Piece)) {
        return nullptr;
    }
    const PackComposition::IdentityIndexEntry* hit =
        FindIdentityInClosure(published, member, endpoint.stableId);
    if (hit == nullptr || hit->identityClass != kInitCatalogIdentityClassPlayable ||
        hit->packSlot >= member.size() || member[hit->packSlot] == 0 ||
        hit->packSlot >= published.owners.size()) {
        return nullptr;
    }
    const PackComposition::PublishedOwner& owner =
        published.owners[hit->packSlot];
    if (owner.readiness != PackOwnerReadiness::Active ||
        owner.view.playableProperties == nullptr ||
        hit->localRow >= owner.view.playablePropertyCount) {
        return nullptr;
    }
    const PlayablePropertyProgramViewV1& property =
        owner.view.playableProperties[hit->localRow];
    const auto expectedKind =
        static_cast<playableproperties::ContentPropertyKindV1>(
            static_cast<std::uint32_t>(endpoint.kind) + 1u);
    if (property.contentPropertyKind != expectedKind ||
        !SameRef(endpoint, RefFromProperty(property))) {
        return nullptr;
    }
    return &owner;
}

BoundedTraversalStatus AccumulateBoundedLeaves(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member, const PlayableRefV1& endpoint,
    std::uint32_t containerDepth, std::uint32_t scheduleDepth,
    std::uint32_t* leafCount,
    std::uint32_t maxKnownScheduleLeaves,
    std::uint32_t maxContainerDepth,
    std::uint32_t maxTrackPieceScheduleDepth,
    const TraversalPathNode* path) noexcept {
    if (leafCount == nullptr) {
        return BoundedTraversalStatus::Invalid;
    }
    for (const auto* ancestor = path; ancestor != nullptr;
         ancestor = ancestor->parent) {
        if (ancestor->stableId == endpoint.stableId) {
            return BoundedTraversalStatus::Invalid;
        }
    }
    if (maxKnownScheduleLeaves == 0 || *leafCount > maxKnownScheduleLeaves) {
        return BoundedTraversalStatus::Exceeded;
    }
    if (endpoint.kind == PlayableKindV1::Container &&
        (containerDepth == 0 || containerDepth > maxContainerDepth)) {
        return BoundedTraversalStatus::Exceeded;
    }
    if ((endpoint.kind == PlayableKindV1::Track ||
         endpoint.kind == PlayableKindV1::Piece) &&
        (scheduleDepth == 0 || scheduleDepth > maxTrackPieceScheduleDepth)) {
        return BoundedTraversalStatus::Exceeded;
    }
    // Each node lives in its depth-bounded call frame; cycle checks allocate
    // nothing even when Container and Track/Piece levels are interleaved.
    const TraversalPathNode currentPath{path, endpoint.stableId};
    const PackComposition::PublishedOwner* owner =
        ResolvePlayableOwner(published, member, endpoint);
    if (owner == nullptr) {
        return BoundedTraversalStatus::Invalid;
    }
    const ContainerWorldInstallViewV1& view = owner->view;
    if (endpoint.kind == PlayableKindV1::Sound) {
        if (*leafCount == maxKnownScheduleLeaves) {
            return BoundedTraversalStatus::Exceeded;
        }
        ++*leafCount;
        return BoundedTraversalStatus::Valid;
    }
    if (endpoint.kind == PlayableKindV1::Container) {
        const ContainerProgramViewV3* container =
            FindContainerProgram(view, endpoint.stableId);
        if (container == nullptr ||
            container->programSemanticFingerprint !=
                endpoint.expectedProgramSemanticFingerprint ||
            !PointerCountOk(container->childCount, container->children)) {
            return BoundedTraversalStatus::Invalid;
        }
        for (std::uint32_t i = 0; i < container->childCount; ++i) {
            const PlayableRefV1& childRef = container->children[i].playable;
            const std::uint32_t childContainerDepth =
                childRef.kind == PlayableKindV1::Container
                    ? (containerDepth == UINT32_MAX ? UINT32_MAX
                                                    : containerDepth + 1u)
                    : containerDepth;
            const std::uint32_t childScheduleDepth =
                (childRef.kind == PlayableKindV1::Track ||
                 childRef.kind == PlayableKindV1::Piece)
                    ? (scheduleDepth == UINT32_MAX ? UINT32_MAX
                                                   : scheduleDepth + 1u)
                    : scheduleDepth;
            const BoundedTraversalStatus child = AccumulateBoundedLeaves(
                published, member, childRef, childContainerDepth,
                childScheduleDepth, leafCount,
                maxKnownScheduleLeaves, maxContainerDepth,
                maxTrackPieceScheduleDepth, &currentPath);
            if (child != BoundedTraversalStatus::Valid) {
                return child;
            }
        }
        // Empty subgraphs are bounded zero work, matching runtime ZeroWork.
        return BoundedTraversalStatus::Valid;
    }
    if (endpoint.kind == PlayableKindV1::Track) {
        const TrackScheduleProgramViewV1* schedule =
            FindTrackSchedule(view, endpoint.stableId);
        if (schedule == nullptr ||
            !SameRef(schedule->rootDefinition, endpoint) ||
            !PointerCountOk(schedule->placementCount, schedule->placements)) {
            return BoundedTraversalStatus::Invalid;
        }
        for (std::uint32_t i = 0; i < schedule->placementCount; ++i) {
            const PlayableRefV1& childRef = schedule->placements[i].content;
            const std::uint32_t childContainerDepth =
                childRef.kind == PlayableKindV1::Container
                    ? (containerDepth == UINT32_MAX ? UINT32_MAX
                                                    : containerDepth + 1u)
                    : containerDepth;
            const std::uint32_t childScheduleDepth =
                (childRef.kind == PlayableKindV1::Track ||
                 childRef.kind == PlayableKindV1::Piece)
                    ? (scheduleDepth == UINT32_MAX ? UINT32_MAX
                                                   : scheduleDepth + 1u)
                    : scheduleDepth;
            const BoundedTraversalStatus child = AccumulateBoundedLeaves(
                published, member, childRef, childContainerDepth,
                childScheduleDepth, leafCount,
                maxKnownScheduleLeaves, maxContainerDepth,
                maxTrackPieceScheduleDepth, &currentPath);
            if (child != BoundedTraversalStatus::Valid) {
                return child;
            }
        }
        // Empty subgraphs are bounded zero work, matching runtime ZeroWork.
        return BoundedTraversalStatus::Valid;
    }
    const PieceScheduleProgramViewV1* schedule =
        FindPieceSchedule(view, endpoint.stableId);
    if (schedule == nullptr || !SameRef(schedule->rootDefinition, endpoint) ||
        !PointerCountOk(schedule->trackRefCount, schedule->trackRefs)) {
        return BoundedTraversalStatus::Invalid;
    }
    for (std::uint32_t i = 0; i < schedule->trackRefCount; ++i) {
        const PlayableRefV1& childRef = schedule->trackRefs[i].trackDefinition;
        const std::uint32_t childContainerDepth =
            childRef.kind == PlayableKindV1::Container
                ? (containerDepth == UINT32_MAX ? UINT32_MAX
                                                : containerDepth + 1u)
                : containerDepth;
        const std::uint32_t childScheduleDepth =
            (childRef.kind == PlayableKindV1::Track ||
             childRef.kind == PlayableKindV1::Piece)
                ? (scheduleDepth == UINT32_MAX ? UINT32_MAX
                                               : scheduleDepth + 1u)
                : scheduleDepth;
        const BoundedTraversalStatus child = AccumulateBoundedLeaves(
            published, member, childRef, childContainerDepth,
            childScheduleDepth, leafCount,
            maxKnownScheduleLeaves, maxContainerDepth,
            maxTrackPieceScheduleDepth, &currentPath);
        if (child != BoundedTraversalStatus::Valid) {
            return child;
        }
    }
    // Empty subgraphs are bounded zero work, matching runtime ZeroWork.
    return BoundedTraversalStatus::Valid;
}

BoundedTraversalStatus EndpointHasBoundedVoices(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const ContainerWorldInstallViewV1& rootView,
    const PlayableRefV1& endpoint) noexcept {
    std::uint32_t leafCount = 0;
    const std::uint32_t containerDepth =
        endpoint.kind == PlayableKindV1::Container ? 1u : 0u;
    const std::uint32_t scheduleDepth =
        (endpoint.kind == PlayableKindV1::Track ||
         endpoint.kind == PlayableKindV1::Piece)
            ? 1u
            : 0u;
    return AccumulateBoundedLeaves(
        published, member, endpoint, containerDepth, scheduleDepth, &leafCount,
        rootView.capacity.maxKnownScheduleLeaves,
        rootView.capacity.maxContainerDepth,
        rootView.capacity.maxTrackPieceScheduleDepth, nullptr);
}

VnResult ValidateNonAnyEndpoint(
    const ContainerProgramViewV3& container, std::uint8_t any,
    const PlayableRefV1& endpoint) noexcept {
    if (any != 0) {
        return VN_OK;
    }
    return TransitionEndpointIsDirectChild(container, endpoint) ? VN_OK
                                                                : VN_BAD_PACK;
}

VnResult ValidateContainerCompanions(
    const ContainerWorldInstallViewV1& view) noexcept {
    if (CountPropertiesOfKind(
            view, playableproperties::ContentPropertyKindV1::Container) !=
        view.containerCount) {
        return VN_BAD_PACK;
    }
    for (std::uint32_t i = 0; i < view.containerCount; ++i) {
        const ContainerProgramViewV3& program = view.containers[i];
        const PlayablePropertyProgramViewV1* property = FindPlayableProperty(
            view, playableproperties::ContentPropertyKindV1::Container,
            program.containerStableId);
        if (property == nullptr || property->programSemanticFingerprint !=
                                       program.programSemanticFingerprint) {
            return VN_BAD_PACK;
        }
    }
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        const PlayablePropertyProgramViewV1& property =
            view.playableProperties[i];
        if (property.contentPropertyKind !=
            playableproperties::ContentPropertyKindV1::Container) {
            continue;
        }
        const ContainerProgramViewV3* program =
            FindContainerProgram(view, property.stableId);
        if (program == nullptr || program->programSemanticFingerprint !=
                                      property.programSemanticFingerprint) {
            return VN_BAD_PACK;
        }
    }
    return VN_OK;
}

VnResult ValidateScheduleCompanions(
    const ContainerWorldInstallViewV1& view) noexcept {
    if (CountPropertiesOfKind(
            view, playableproperties::ContentPropertyKindV1::Track) !=
            view.trackScheduleCount ||
        CountPropertiesOfKind(
            view, playableproperties::ContentPropertyKindV1::Piece) !=
            view.pieceScheduleCount) {
        return VN_BAD_PACK;
    }
    for (std::uint32_t i = 0; i < view.trackScheduleCount; ++i) {
        const TrackScheduleProgramViewV1& schedule = view.trackSchedules[i];
        if (schedule.rootDefinition.kind != PlayableKindV1::Track) {
            return VN_BAD_PACK;
        }
        const PlayablePropertyProgramViewV1* property = FindPlayableProperty(
            view, playableproperties::ContentPropertyKindV1::Track,
            schedule.rootDefinition.stableId);
        if (property == nullptr ||
            !ScheduleRootMatchesProperty(schedule.rootDefinition, *property)) {
            return VN_BAD_PACK;
        }
    }
    for (std::uint32_t i = 0; i < view.pieceScheduleCount; ++i) {
        const PieceScheduleProgramViewV1& schedule = view.pieceSchedules[i];
        if (schedule.rootDefinition.kind != PlayableKindV1::Piece) {
            return VN_BAD_PACK;
        }
        const PlayablePropertyProgramViewV1* property = FindPlayableProperty(
            view, playableproperties::ContentPropertyKindV1::Piece,
            schedule.rootDefinition.stableId);
        if (property == nullptr ||
            !ScheduleRootMatchesProperty(schedule.rootDefinition, *property)) {
            return VN_BAD_PACK;
        }
    }
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        const PlayablePropertyProgramViewV1& property =
            view.playableProperties[i];
        if (property.contentPropertyKind ==
            playableproperties::ContentPropertyKindV1::Track) {
            const TrackScheduleProgramViewV1* schedule =
                FindTrackSchedule(view, property.stableId);
            if (schedule == nullptr ||
                !ScheduleRootMatchesProperty(schedule->rootDefinition,
                                             property)) {
                return VN_BAD_PACK;
            }
        } else if (property.contentPropertyKind ==
                   playableproperties::ContentPropertyKindV1::Piece) {
            const PieceScheduleProgramViewV1* schedule =
                FindPieceSchedule(view, property.stableId);
            if (schedule == nullptr ||
                !ScheduleRootMatchesProperty(schedule->rootDefinition,
                                             property)) {
                return VN_BAD_PACK;
            }
        }
    }
    return VN_OK;
}

VnResult ValidateOccurrences(
    const PackComposition::PublishedView& published, std::uint32_t slot,
    const ContainerWorldInstallViewV1& view) noexcept {
    for (std::uint32_t i = 0; i < view.playableOccurrenceCount; ++i) {
        const PlayableOccurrencePropertyViewV1& row =
            view.playableOccurrences[i];
        const PlayablePropertyProgramViewV1* parent =
            FindPlayableProperty(view, row.parentKind, row.parentStableId);
        if (parent == nullptr || parent->programSemanticFingerprint !=
                                     row.parentProgramSemanticFingerprint) {
            return VN_BAD_PACK;
        }
        if (!IdentityOwnedInSlot(published, slot, row.childPlayable.stableId)) {
            continue;
        }
        const PackComposition::IdentityIndexEntry* hit =
            FindIdentityProvider(published, row.childPlayable.stableId,
                [slot](std::uint32_t candidate) { return candidate == slot; });
        if (hit == nullptr ||
            hit->identityClass != kInitCatalogIdentityClassPlayable ||
            hit->localRow >= view.playablePropertyCount ||
            view.playableProperties == nullptr) {
            return VN_BAD_PACK;
        }
        const PlayablePropertyProgramViewV1& child =
            view.playableProperties[hit->localRow];
        if (child.stableId != row.childPlayable.stableId ||
            !SameRef(row.childPlayable, RefFromProperty(child))) {
            return VN_BAD_PACK;
        }
    }
    for (std::uint32_t i = 0; i < view.containerCount; ++i) {
        const ContainerProgramViewV3& program = view.containers[i];
        if (CountOccurrencesForParent(
                view, playableproperties::ContentPropertyKindV1::Container,
                program.containerStableId) != program.childCount) {
            return VN_BAD_PACK;
        }
        if (!PointerCountOk(program.childCount, program.children)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t c = 0; c < program.childCount; ++c) {
            const PlayableOccurrencePropertyViewV1* occurrence = FindOccurrence(
                view, playableproperties::ContentPropertyKindV1::Container,
                program.containerStableId, program.children[c].stableChildOrdinal);
            if (occurrence == nullptr ||
                !SameRef(occurrence->childPlayable, program.children[c].playable)) {
                return VN_BAD_PACK;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.trackScheduleCount; ++i) {
        const TrackScheduleProgramViewV1& schedule = view.trackSchedules[i];
        if (CountOccurrencesForParent(
                view, playableproperties::ContentPropertyKindV1::Track,
                schedule.rootDefinition.stableId) != schedule.placementCount) {
            return VN_BAD_PACK;
        }
        if (!PointerCountOk(schedule.placementCount, schedule.placements)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t p = 0; p < schedule.placementCount; ++p) {
            const PlayableOccurrencePropertyViewV1* occurrence = FindOccurrence(
                view, playableproperties::ContentPropertyKindV1::Track,
                schedule.rootDefinition.stableId,
                schedule.placements[p].stableOccurrenceOrdinal);
            if (occurrence == nullptr ||
                !SameRef(occurrence->childPlayable,
                         schedule.placements[p].content)) {
                return VN_BAD_PACK;
            }
        }
    }
    return VN_OK;
}

VnResult ValidateTransitions(
    const PackComposition::PublishedView& published, std::uint32_t slot,
    const ContainerWorldInstallViewV1& view) noexcept {
    for (std::uint32_t i = 0; i < view.transitionCount; ++i) {
        const TransitionProgramViewV1& transition = view.transitions[i];
        const ContainerProgramViewV3* container =
            FindContainerProgram(view, transition.ownerContainerStableId);
        if (container == nullptr || container->transitionProgramIndex != i) {
            return VN_BAD_PACK;
        }
        const TransitionProgramClassV1 classification =
            ClassifyTransitionProgramV1(transition);
        if (classification == TransitionProgramClassV1::Invalid) {
            return VN_BAD_PACK;
        }
        if (classification == TransitionProgramClassV1::Advanced) {
            return VN_CAPABILITY_UNAVAILABLE;
        }
        if (!PointerCountOk(transition.ruleCount, transition.rules)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t r = 0; r < transition.ruleCount; ++r) {
            const TransitionRuleViewV1& rule = transition.rules[r];
            const VnResult sourceOk =
                ValidateNonAnyEndpoint(*container, rule.sourceAny, rule.source);
            if (sourceOk != VN_OK) {
                return sourceOk;
            }
            const VnResult targetOk =
                ValidateNonAnyEndpoint(*container, rule.targetAny, rule.target);
            if (targetOk != VN_OK) {
                return targetOk;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.containerCount; ++i) {
        const ContainerProgramViewV3& container = view.containers[i];
        if (container.transitionProgramIndex == UINT32_MAX) {
            continue;
        }
        if (container.transitionProgramIndex >= view.transitionCount ||
            view.transitions[container.transitionProgramIndex]
                    .ownerContainerStableId != container.containerStableId) {
            return VN_BAD_PACK;
        }
    }
    return VN_OK;
}

const PackComposition::PublishedOwner* FindActiveOwnerByPack(
    const PackComposition::PublishedView& published, std::uint64_t packStableId,
    std::uint32_t* outSlot) noexcept;

VnResult BuildDeclaredClosure(
    const PackComposition::PublishedView& published, std::uint32_t rootSlot,
    std::size_t maxPacks, std::vector<std::uint8_t>* member);

VnResult ValidateContainerChildCapacity(
    const PackComposition::PublishedView& published,
    const ContainerWorldInstallViewV1& view,
    const std::vector<std::uint8_t>& member) noexcept {
    for (std::uint32_t i = 0; i < view.containerCount; ++i) {
        const ContainerProgramViewV3& container = view.containers[i];
        if (!PointerCountOk(container.childCount, container.children)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t c = 0; c < container.childCount; ++c) {
            const ContainerChildProgramViewV2& child = container.children[c];
            const BoundedTraversalStatus bounded = EndpointHasBoundedVoices(
                published, member, view, child.playable);
            if (bounded == BoundedTraversalStatus::Invalid) {
                return VN_BAD_PACK;
            }
            if (bounded == BoundedTraversalStatus::Exceeded) {
                return VN_PACK_RESIDENT_CAPACITY;
            }
        }
    }
    return VN_OK;
}

VnResult ValidateSameOwnerCompanions(
    const PackComposition::PublishedView& published,
    std::size_t maxPacks) {
    for (std::uint32_t slot = 0; slot < published.owners.size(); ++slot) {
        const PackComposition::PublishedOwner& owner = published.owners[slot];
        if (owner.readiness != PackOwnerReadiness::Active) {
            continue;
        }
        const ContainerWorldInstallViewV1& view = owner.view;
        if (!CompanionTablesSorted(view)) {
            return VN_BAD_PACK;
        }
        const VnResult containers = ValidateContainerCompanions(view);
        if (containers != VN_OK) {
            return containers;
        }
        const VnResult schedules = ValidateScheduleCompanions(view);
        if (schedules != VN_OK) {
            return schedules;
        }
        const VnResult occurrences = ValidateOccurrences(published, slot, view);
        if (occurrences != VN_OK) {
            return occurrences;
        }
        std::vector<std::uint8_t> member;
        const VnResult closure =
            BuildDeclaredClosure(published, slot, maxPacks, &member);
        if (closure != VN_OK) {
            return closure;
        }
        const VnResult childCapacity =
            ValidateContainerChildCapacity(published, view, member);
        if (childCapacity != VN_OK) {
            return childCapacity;
        }
        const VnResult transitions = ValidateTransitions(published, slot, view);
        if (transitions != VN_OK) {
            return transitions;
        }
    }
    return VN_OK;
}

const PackComposition::PublishedOwner* FindActiveOwnerByPack(
    const PackComposition::PublishedView& published, std::uint64_t packStableId,
    std::uint32_t* outSlot) noexcept {
    for (std::uint32_t slot = 0; slot < published.owners.size(); ++slot) {
        const PackComposition::PublishedOwner& owner = published.owners[slot];
        if (owner.readiness == PackOwnerReadiness::Active &&
            owner.packStableId == packStableId) {
            if (outSlot != nullptr) {
                *outSlot = slot;
            }
            return &owner;
        }
    }
    return nullptr;
}

bool CatalogKnowsIdentity(const PackComposition::PublishedView& published,
                          VnObjectId stableId) noexcept {
    return published.hasCatalog &&
           LookupInitCatalogIdentity(published.catalog, stableId) ==
               PackCatalogIdentityState::KnownUnloaded;
}

VnResult BuildDeclaredClosure(
    const PackComposition::PublishedView& published, std::uint32_t rootSlot,
    std::size_t maxPacks, std::vector<std::uint8_t>* member) {
    member->assign(published.owners.size(), 0);
    if (rootSlot >= published.owners.size()) {
        return VN_BAD_PACK;
    }
    if (maxPacks == 0) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
    std::vector<std::uint32_t> slots;
    slots.push_back(rootSlot);
    (*member)[rootSlot] = 1;
    for (std::size_t cursor = 0; cursor < slots.size(); ++cursor) {
        const PackComposition::PublishedOwner& owner =
            published.owners[slots[cursor]];
        for (const PackDependencyEdge& edge : owner.dependencyEdges) {
            const std::uint64_t providerId = edge.prerequisitePackStableId;
            std::uint32_t providerSlot = 0;
            const PackComposition::PublishedOwner* provider =
                FindActiveOwnerByPack(published, providerId, &providerSlot);
            if (provider == nullptr) {
                return VN_CONTENT_NOT_LOADED;
            }
            if (edge.expectedPrerequisiteContentRevision != 0 &&
                provider->contentRevision !=
                    edge.expectedPrerequisiteContentRevision) {
                return VN_PACK_STALE_REVISION;
            }
            if ((*member)[providerSlot] != 0) {
                continue;
            }
            if (slots.size() >= maxPacks) {
                return VN_PACK_RESIDENT_CAPACITY;
            }
            (*member)[providerSlot] = 1;
            slots.push_back(providerSlot);
        }
    }
    return VN_OK;
}

VnResult ResolveInDeclaredClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member, VnObjectId stableId,
    std::uint32_t expectedClass,
    const PackComposition::IdentityIndexEntry** outHit,
    const PackComposition::PublishedOwner** outOwner) noexcept {
    if (stableId == VnObjectId{}) {
        return VN_BAD_PACK;
    }
    const PackComposition::IdentityIndexEntry* hit =
        FindIdentityInClosure(published, member, stableId);
    if (hit == nullptr) {
        return CatalogKnowsIdentity(published, stableId) ? VN_CONTENT_NOT_LOADED
                                                         : VN_BAD_PACK;
    }
    if (hit->packSlot >= member.size() || member[hit->packSlot] == 0 ||
        hit->packSlot >= published.owners.size() ||
        hit->identityClass != expectedClass) {
        return VN_BAD_PACK;
    }
    const PackComposition::PublishedOwner& owner =
        published.owners[hit->packSlot];
    if (owner.readiness != PackOwnerReadiness::Active) {
        return VN_BAD_PACK;
    }
    *outHit = hit;
    *outOwner = &owner;
    return VN_OK;
}

const PlayablePropertyProgramViewV1* PropertyAtHit(
    const PackComposition::PublishedOwner& owner,
    const PackComposition::IdentityIndexEntry& hit) noexcept {
    if (owner.view.playableProperties == nullptr ||
        hit.localRow >= owner.view.playablePropertyCount) {
        return nullptr;
    }
    return &owner.view.playableProperties[hit.localRow];
}

const ActionProgramViewV1* ActionAtHit(
    const PackComposition::PublishedOwner& owner,
    const PackComposition::IdentityIndexEntry& hit) noexcept {
    if (owner.view.actions == nullptr || hit.localRow >= owner.view.actionCount) {
        return nullptr;
    }
    return &owner.view.actions[hit.localRow];
}

const VariableDefinitionV1* VariableAtHit(
    const PackComposition::PublishedOwner& owner,
    const PackComposition::IdentityIndexEntry& hit) noexcept {
    if (owner.view.variables == nullptr ||
        hit.localRow >= owner.view.variableCount) {
        return nullptr;
    }
    return &owner.view.variables[hit.localRow];
}

VnResult ResolveVariableDefinitionInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member, VnObjectId variableId,
    const VariableDefinitionV1** outVariable) noexcept {
    const PackComposition::IdentityIndexEntry* hit = nullptr;
    const PackComposition::PublishedOwner* owner = nullptr;
    const VnResult resolved = ResolveInDeclaredClosure(
        published, member, variableId, kInitCatalogIdentityClassVariable,
        &hit, &owner);
    if (resolved != VN_OK) {
        return resolved;
    }
    const VariableDefinitionV1* variable = VariableAtHit(*owner, *hit);
    if (variable == nullptr || variable->variableId != variableId) {
        return VN_BAD_PACK;
    }
    *outVariable = variable;
    return VN_OK;
}

bool DiscreteRangeCanRoundToInt32(
    const VariableDefinitionV1& definition) noexcept {
    const double roundedMin =
        std::floor(static_cast<double>(definition.rangeMin) + 0.5);
    const double roundedMax =
        std::floor(static_cast<double>(definition.rangeMax) + 0.5);
    return roundedMin >=
               static_cast<double>((std::numeric_limits<std::int32_t>::min)()) &&
           roundedMax <=
               static_cast<double>((std::numeric_limits<std::int32_t>::max)());
}

VnResult ValidateGridVariablesInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const ContainerProgramViewV3& container) noexcept {
    for (std::uint32_t i = 0; i < container.discreteAxisCount; ++i) {
        const VariableDefinitionV1* variable = nullptr;
        const VnResult resolved = ResolveVariableDefinitionInClosure(
            published, member, container.discreteAxes[i].variableId,
            &variable);
        if (resolved != VN_OK) {
            return resolved;
        }
        if (!DiscreteRangeCanRoundToInt32(*variable)) {
            return VN_BAD_PACK;
        }
    }
    for (std::uint32_t i = 0; i < container.continuousAxisCount; ++i) {
        const VariableDefinitionV1* variable = nullptr;
        const VnResult resolved = ResolveVariableDefinitionInClosure(
            published, member, container.continuousAxes[i].variableId,
            &variable);
        if (resolved != VN_OK) {
            return resolved;
        }
    }
    return VN_OK;
}

VnResult ValidateSetVariableInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const ActionStepProgramViewV1& step) noexcept {
    const VariableDefinitionV1* variable = nullptr;
    const VnResult resolved = ResolveVariableDefinitionInClosure(
        published, member, step.target.stableId, &variable);
    if (resolved != VN_OK) {
        return resolved;
    }
    const float value = std::bit_cast<float>(step.valueBits);
    if (!std::isfinite(value) || (value == 0.0f && std::signbit(value)) ||
        value < variable->rangeMin || value > variable->rangeMax ||
        (step.scope != ActionScopeV1::Emitter &&
         step.scope != ActionScopeV1::Global) ||
        (variable->kind == VariableKindV1::Step &&
         (value < -16777216.0f || value > 16777216.0f ||
          value != std::floor(value)))) {
        return VN_BAD_PACK;
    }
    return VN_OK;
}

VnResult ValidatePlayableRefInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member, const PlayableRefV1& ref) noexcept {
    const PackComposition::IdentityIndexEntry* hit = nullptr;
    const PackComposition::PublishedOwner* owner = nullptr;
    const VnResult resolved = ResolveInDeclaredClosure(
        published, member, ref.stableId, kInitCatalogIdentityClassPlayable,
        &hit, &owner);
    if (resolved != VN_OK) {
        return resolved;
    }
    const PlayablePropertyProgramViewV1* property = PropertyAtHit(*owner, *hit);
    if (property == nullptr ||
        !SameRef(ref, RefFromProperty(*property))) {
        return VN_BAD_PACK;
    }
    return VN_OK;
}

bool ActionKindIsPlayableOrAction(ActionTargetKindV1 kind) noexcept {
    return kind == ActionTargetKindV1::Sound ||
           kind == ActionTargetKindV1::Container ||
           kind == ActionTargetKindV1::Track ||
           kind == ActionTargetKindV1::Piece ||
           kind == ActionTargetKindV1::Action;
}

VnResult ValidateActionObjectRefInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const ActionObjectRefV1& ref) noexcept {
    if (!ActionKindIsPlayableOrAction(ref.kind)) {
        return VN_OK;
    }
    const bool isAction = ref.kind == ActionTargetKindV1::Action;
    const PackComposition::IdentityIndexEntry* hit = nullptr;
    const PackComposition::PublishedOwner* owner = nullptr;
    const VnResult resolved = ResolveInDeclaredClosure(
        published, member, ref.stableId,
        isAction ? kInitCatalogIdentityClassAction
                 : kInitCatalogIdentityClassPlayable,
        &hit, &owner);
    if (resolved != VN_OK) {
        return resolved;
    }
    if (isAction) {
        const ActionProgramViewV1* action = ActionAtHit(*owner, *hit);
        if (action == nullptr || action->actionStableId != ref.stableId ||
            action->programSemanticFingerprint !=
                ref.expectedProgramSemanticFingerprint) {
            return VN_BAD_PACK;
        }
        return VN_OK;
    }
    const PlayablePropertyProgramViewV1* property = PropertyAtHit(*owner, *hit);
    const auto expectedKind =
        static_cast<playableproperties::ContentPropertyKindV1>(
            static_cast<std::uint32_t>(ref.kind) + 1u);
    if (property == nullptr || property->contentPropertyKind != expectedKind ||
        property->programSemanticFingerprint !=
            ref.expectedProgramSemanticFingerprint) {
        return VN_BAD_PACK;
    }
    return VN_OK;
}

VnResult ValidateActionControlReference(
    const PackComposition::PublishedView& published,
    const ActionObjectRefV1& ref) noexcept {
    if (ref.reserved != 0 || ref.stableId == VnObjectId{} ||
        ref.expectedProgramSemanticFingerprint == 0 ||
        static_cast<std::uint32_t>(ref.kind) > static_cast<std::uint32_t>(ActionTargetKindV1::Piece))
        return VN_BAD_PACK;
    const auto* hit = BinaryFind(published.index, ref.stableId);
    if (hit == nullptr) return VN_OK;
    if (hit->identityClass != kInitCatalogIdentityClassPlayable ||
        hit->packSlot >= published.owners.size()) return VN_BAD_PACK;
    const auto* property = PropertyAtHit(published.owners[hit->packSlot], *hit);
    const auto kind = static_cast<playableproperties::ContentPropertyKindV1>(
        static_cast<std::uint32_t>(ref.kind) + 1u);
    return property != nullptr && property->contentPropertyKind == kind &&
        property->programSemanticFingerprint == ref.expectedProgramSemanticFingerprint
        ? VN_OK : VN_BAD_PACK;
}

VnResult ValidateScheduleKeyInClosure(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const TrackPieceScheduleKeyV1& key) noexcept {
    const PackComposition::IdentityIndexEntry* hit = nullptr;
    const PackComposition::PublishedOwner* owner = nullptr;
    const VnResult resolved = ResolveInDeclaredClosure(
        published, member, key.stableId, kInitCatalogIdentityClassPlayable,
        &hit, &owner);
    if (resolved != VN_OK) {
        return resolved;
    }
    const PlayablePropertyProgramViewV1* property = PropertyAtHit(*owner, *hit);
    if (property == nullptr) {
        return VN_BAD_PACK;
    }
    if (key.kind == PlayableKindV1::Track) {
        if (property->contentPropertyKind !=
            playableproperties::ContentPropertyKindV1::Track) {
            return VN_BAD_PACK;
        }
        const TrackScheduleProgramViewV1* schedule =
            FindTrackSchedule(owner->view, key.stableId);
        if (schedule == nullptr ||
            schedule->scheduleSemanticFingerprint !=
                key.scheduleSemanticFingerprint) {
            return VN_BAD_PACK;
        }
        return VN_OK;
    }
    if (key.kind != PlayableKindV1::Piece ||
        property->contentPropertyKind !=
            playableproperties::ContentPropertyKindV1::Piece) {
        return VN_BAD_PACK;
    }
    const PieceScheduleProgramViewV1* schedule =
        FindPieceSchedule(owner->view, key.stableId);
    if (schedule == nullptr || schedule->scheduleSemanticFingerprint !=
                                   key.scheduleSemanticFingerprint) {
        return VN_BAD_PACK;
    }
    return VN_OK;
}

VnResult ValidateOwnerExternalRefs(
    const PackComposition::PublishedView& published, std::uint32_t slot,
    const std::vector<std::uint8_t>& member) noexcept {
    const ContainerWorldInstallViewV1& view = published.owners[slot].view;
    if (const auto& bindings = published.owners[slot].voiceBindings) {
        for (const auto& binding : *bindings) {
            if (!IdentityOwnedInSlot(published, slot, binding.objectId)) {
                const auto* host = FindIdentityInClosure(
                    published, member, binding.objectId);
                if (host == nullptr ||
                    host->identityClass !=
                        kInitCatalogIdentityClassPlayable ||
                    host->packSlot >= published.owners.size() ||
                    published.owners[host->packSlot].packStableId !=
                        published.initPackStableId) {
                    return VN_BAD_PACK;
                }
            }
            const auto* variable = FindIdentityInClosure(
                published, member, binding.descriptor.variableId);
            if (variable == nullptr || variable->identityClass != kInitCatalogIdentityClassVariable) {
                return VN_BAD_PACK;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        const PlayablePropertyProgramViewV1& property =
            view.playableProperties[i];
        for (std::uint32_t a = 0;
             a < property.organizationLayerAncestorCount; ++a) {
            const LayerAncestorRefV1& ancestor =
                property.organizationLayerAncestors[a];
            const PackComposition::IdentityIndexEntry* hit = nullptr;
            const PackComposition::PublishedOwner* owner = nullptr;
            const VnResult resolved = ResolveInDeclaredClosure(
                published, member, ancestor.layerStableId,
                kInitCatalogIdentityClassPlayable, &hit, &owner);
            if (resolved != VN_OK) {
                return resolved;
            }
            const PlayablePropertyProgramViewV1* layer =
                PropertyAtHit(*owner, *hit);
            if (layer == nullptr ||
                layer->contentPropertyKind !=
                    playableproperties::ContentPropertyKindV1::Layer ||
                layer->programSemanticFingerprint !=
                    ancestor.layerProgramSemanticFingerprint) {
                return VN_BAD_PACK;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.playableOccurrenceCount; ++i) {
        const PlayableOccurrencePropertyViewV1& occurrence =
            view.playableOccurrences[i];
        const VnResult result = ValidatePlayableRefInClosure(
            published, member, occurrence.childPlayable);
        if (result != VN_OK) {
            return result;
        }
    }
    for (std::uint32_t i = 0; i < view.containerCount; ++i) {
        const ContainerProgramViewV3& program = view.containers[i];
        if (!PointerCountOk(program.childCount, program.children) ||
            !PointerCountOk(program.discreteAxisCount, program.discreteAxes) ||
            !PointerCountOk(program.continuousAxisCount,
                            program.continuousAxes)) {
            return VN_BAD_PACK;
        }
        const VnResult gridVariables =
            ValidateGridVariablesInClosure(published, member, program);
        if (gridVariables != VN_OK) {
            return gridVariables;
        }
        for (std::uint32_t c = 0; c < program.childCount; ++c) {
            const ContainerChildProgramViewV2& child = program.children[c];
            const VnResult playable = ValidatePlayableRefInClosure(
                published, member, child.playable);
            if (playable != VN_OK) {
                return playable;
            }
            if (child.timingHint.kind ==
                ChildTimingHintKindV2::ScheduleReference) {
                const VnResult schedule = ValidateScheduleKeyInClosure(
                    published, member, child.timingHint.payload.schedule);
                if (schedule != VN_OK) {
                    return schedule;
                }
                continue;
            }
            if (child.timingHint.kind != ChildTimingHintKindV2::Unknown &&
                child.timingHint.kind != ChildTimingHintKindV2::InlineSound) {
                return VN_BAD_PACK;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.trackScheduleCount; ++i) {
        const TrackScheduleProgramViewV1& schedule = view.trackSchedules[i];
        if (!PointerCountOk(schedule.placementCount, schedule.placements)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t p = 0; p < schedule.placementCount; ++p) {
            const VnResult result = ValidatePlayableRefInClosure(
                published, member, schedule.placements[p].content);
            if (result != VN_OK) {
                return result;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.pieceScheduleCount; ++i) {
        const PieceScheduleProgramViewV1& schedule = view.pieceSchedules[i];
        if (!PointerCountOk(schedule.trackRefCount, schedule.trackRefs)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t t = 0; t < schedule.trackRefCount; ++t) {
            const VnResult result = ValidatePlayableRefInClosure(
                published, member, schedule.trackRefs[t].trackDefinition);
            if (result != VN_OK) {
                return result;
            }
            const PackComposition::IdentityIndexEntry* hit = nullptr;
            const PackComposition::PublishedOwner* owner = nullptr;
            const VnResult resolved = ResolveInDeclaredClosure(
                published, member,
                schedule.trackRefs[t].trackDefinition.stableId,
                kInitCatalogIdentityClassPlayable, &hit, &owner);
            if (resolved != VN_OK) {
                return resolved;
            }
            const TrackScheduleProgramViewV1* track = FindTrackSchedule(
                owner->view,
                schedule.trackRefs[t].trackDefinition.stableId);
            if (track == nullptr ||
                track->scheduleSemanticFingerprint !=
                    schedule.trackRefs[t].trackScheduleSemanticFingerprint) {
                return VN_BAD_PACK;
            }
        }
    }
    for (std::uint32_t i = 0; i < view.transitionCount; ++i) {
        const TransitionProgramViewV1& transition = view.transitions[i];
        if (!PointerCountOk(transition.ruleCount, transition.rules) ||
            !PointerCountOk(transition.downwardPathStepCount,
                            transition.downwardPathSteps)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t r = 0; r < transition.ruleCount; ++r) {
            const TransitionRuleViewV1& rule = transition.rules[r];
            if (rule.sourceAny == 0) {
                const VnResult source = ValidatePlayableRefInClosure(
                    published, member, rule.source);
                if (source != VN_OK) {
                    return source;
                }
            }
            if (rule.targetAny == 0) {
                const VnResult target = ValidatePlayableRefInClosure(
                    published, member, rule.target);
                if (target != VN_OK) {
                    return target;
                }
            }
            if (rule.bridgePresent != 0) {
                const VnResult bridge = ValidatePlayableRefInClosure(
                    published, member, rule.bridge);
                if (bridge != VN_OK) {
                    return bridge;
                }
            }
        }
        for (std::uint32_t p = 0; p < transition.downwardPathStepCount; ++p) {
            const VnResult path = ValidatePlayableRefInClosure(
                published, member, transition.downwardPathSteps[p].playable);
            if (path != VN_OK) {
                return path;
            }
        }
    }
    if (!PointerCountOk(view.actionCount, view.actions)) {
        return VN_BAD_PACK;
    }
    for (std::uint32_t i = 0; i < view.actionCount; ++i) {
        const ActionProgramViewV1& action = view.actions[i];
        if (!PointerCountOk(action.stepCount, action.steps)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t s = 0; s < action.stepCount; ++s) {
            const ActionStepProgramViewV1& step = action.steps[s];
            if (step.opcode == ActionOpcodeV1::SetVariable) {
                const VnResult setValue =
                    ValidateSetVariableInClosure(published, member, step);
                if (setValue != VN_OK) {
                    return setValue;
                }
            }
            const VnResult target = step.opcode == ActionOpcodeV1::Stop
                ? ValidateActionControlReference(published, step.target)
                : ValidateActionObjectRefInClosure(published, member, step.target);
            if (target != VN_OK) {
                return target;
            }
            if (step.quantizeReference.stableId == VnObjectId{}) {
                continue;
            }
            const VnResult quantize = step.opcode == ActionOpcodeV1::Stop
                ? ValidateActionControlReference(published, step.quantizeReference)
                : ValidateActionObjectRefInClosure(published, member, step.quantizeReference);
            if (quantize != VN_OK) {
                return quantize;
            }
        }
    }
    return VN_OK;
}
bool SameFloatBits(float left, float right) noexcept {
    return std::bit_cast<std::uint32_t>(left) ==
           std::bit_cast<std::uint32_t>(right);
}

bool SamePayload(const PlayablePropertyPayloadViewV1& left,
                 const PlayablePropertyPayloadViewV1& right) noexcept {
    if (left.presentMask != right.presentMask ||
        left.sendCount != right.sendCount ||
        left.effectNodeCount != right.effectNodeCount ||
        left.effectEdgeCount != right.effectEdgeCount ||
        !PointerCountOk(left.sendCount, left.sends) ||
        !PointerCountOk(right.sendCount, right.sends) ||
        !PointerCountOk(left.effectNodeCount, left.effectNodes) ||
        !PointerCountOk(right.effectNodeCount, right.effectNodes) ||
        !PointerCountOk(left.effectEdgeCount, left.effectEdges) ||
        !PointerCountOk(right.effectEdgeCount, right.effectEdges)) {
        return false;
    }
    for (std::size_t i = 0; i < left.values.size(); ++i) {
        if (left.values[i].bits != right.values[i].bits) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < left.sendCount; ++i) {
        const SendV1& leftSend = left.sends[i];
        const SendV1& rightSend = right.sends[i];
        if (leftSend.stableSendOrdinal != rightSend.stableSendOrdinal ||
            leftSend.reserved != rightSend.reserved ||
            leftSend.targetBusId != rightSend.targetBusId ||
            !SameFloatBits(leftSend.sendLevel, rightSend.sendLevel) ||
            leftSend.reserved2 != rightSend.reserved2) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < left.effectNodeCount; ++i) {
        const EffectNodeV1& leftNode = left.effectNodes[i];
        const EffectNodeV1& rightNode = right.effectNodes[i];
        if (leftNode.nodeId != rightNode.nodeId ||
            leftNode.effectId != rightNode.effectId ||
            leftNode.effectType != rightNode.effectType ||
            leftNode.paramCount != rightNode.paramCount ||
            !PointerCountOk(leftNode.paramCount, leftNode.params) ||
            !PointerCountOk(rightNode.paramCount, rightNode.params)) {
            return false;
        }
        for (std::uint32_t p = 0; p < leftNode.paramCount; ++p) {
            if (leftNode.params[p].paramId != rightNode.params[p].paramId ||
                leftNode.params[p].valueBits !=
                    rightNode.params[p].valueBits) {
                return false;
            }
        }
    }
    for (std::uint32_t i = 0; i < left.effectEdgeCount; ++i) {
        if (left.effectEdges[i].fromNodeId !=
                right.effectEdges[i].fromNodeId ||
            left.effectEdges[i].toNodeId != right.effectEdges[i].toNodeId) {
            return false;
        }
    }
    return true;
}

VnResult ValidateStructuralAncestorRefs(
    const PackComposition::PublishedView& published,
    const std::vector<std::uint8_t>& member,
    const ContainerWorldInstallViewV1& view) noexcept {
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        const PlayablePropertyProgramViewV1& property =
            view.playableProperties[i];
        for (std::uint32_t a = 0; a < property.structuralAncestorCount; ++a) {
            const StructuralAncestorRefV1& ancestor =
                property.structuralAncestors[a];
            const PackComposition::IdentityIndexEntry* hit =
                FindIdentityInClosure(published, member, ancestor.stableId);
            if (hit == nullptr) {
                // The inline payload remains authoritative while the ancestor
                // owner is unloaded and therefore absent from the composite.
                continue;
            }
            if (hit->packSlot >= member.size() ||
                member[hit->packSlot] == 0) {
                continue;
            }
            if (hit->identityClass != kInitCatalogIdentityClassPlayable ||
                hit->packSlot >= published.owners.size()) {
                return VN_BAD_PACK;
            }
            const PackComposition::PublishedOwner& owner =
                published.owners[hit->packSlot];
            if (owner.readiness != PackOwnerReadiness::Active) {
                return VN_BAD_PACK;
            }
            const PlayablePropertyProgramViewV1* current = PropertyAtHit(
                owner, *hit);
            if (current == nullptr ||
                current->contentPropertyKind != ancestor.contentPropertyKind ||
                current->programSemanticFingerprint !=
                    ancestor.programSemanticFingerprint ||
                !SamePayload(MakeStructuralAncestorPayloadV1(current->payload),
                             ancestor.payload)) {
                return VN_BAD_PACK;
            }
        }
    }
    return VN_OK;
}

struct CompositeWorldScratch {
    ContainerWorldInstallViewV1 view{};
    std::vector<VariableDefinitionV1> variables;
    std::vector<PlayablePropertyProgramViewV1> properties;
    std::vector<PlayableOccurrencePropertyViewV1> occurrences;
    std::vector<TrackScheduleProgramViewV1> tracks;
    std::vector<PieceScheduleProgramViewV1> pieces;
    std::vector<ContainerProgramViewV3> containers;
    std::vector<TransitionProgramViewV1> transitions;
    std::vector<ActionProgramViewV1> actions;
};

bool EncodeIdentityDefinition(const PackComposition::PublishedOwner& owner,
                              const PackComposition::IdentityIndexEntry& entry,
                              std::vector<std::uint8_t>& bytes) {
    const auto& world = owner.view;
    ContainerWorldInstallViewV1 slice{};
    if (entry.identityClass == kInitCatalogIdentityClassVariable) {
        slice.variables = &world.variables[entry.localRow];
        slice.variableCount = 1;
    } else if (entry.identityClass == kInitCatalogIdentityClassAction) {
        slice.actions = &world.actions[entry.localRow];
        slice.actionCount = 1;
    } else {
        slice.playableProperties = &world.playableProperties[entry.localRow];
        slice.playablePropertyCount = 1;
    }
    std::vector<PlayableOccurrencePropertyViewV1> occurrences;
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
        if (world.playableOccurrences[i].parentStableId == entry.stableId) {
            occurrences.push_back(world.playableOccurrences[i]);
        }
    }
    slice.playableOccurrences = occurrences.data();
    slice.playableOccurrenceCount = static_cast<std::uint32_t>(occurrences.size());
    slice.trackSchedules = FindTrackSchedule(world, entry.stableId);
    slice.trackScheduleCount = slice.trackSchedules != nullptr ? 1 : 0;
    slice.pieceSchedules = FindPieceSchedule(world, entry.stableId);
    slice.pieceScheduleCount = slice.pieceSchedules != nullptr ? 1 : 0;
    for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
        if (world.transitions[i].ownerContainerStableId == entry.stableId) {
            slice.transitions = &world.transitions[i];
            slice.transitionCount = 1;
            break;
        }
    }
    ContainerProgramViewV3 container{};
    if (const auto* original = FindContainerProgram(world, entry.stableId)) {
        container = *original;
        container.transitionProgramIndex = slice.transitionCount == 0 ? UINT32_MAX : 0;
        slice.containers = &container;
        slice.containerCount = 1;
    }
    if (!EncodeCurrentWorldShippingBody(slice, bytes)) {
        return false;
    }
    if (owner.voiceBindings != nullptr) {
        for (const PackVoiceBinding& binding : *owner.voiceBindings) {
            if (binding.objectId != entry.stableId) {
                continue;
            }
            const std::size_t offset = bytes.size();
            bytes.resize(offset + kPackV2BindingDefinitionPayloadBytes);
            SerializePackV2BindingDefinition(binding.bindingId, binding.descriptor,
                                              bytes.data() + offset);
        }
    }
    return true;
}

template <typename T, typename Key>
void DeduplicateRows(std::vector<T>& rows, Key key) {
    rows.erase(std::unique(rows.begin(), rows.end(),
        [&](const T& left, const T& right) { return key(left) == key(right); }), rows.end());
}

template <typename T>
bool AppendRows(std::vector<T>* destination, const T* rows,
                std::uint32_t count) {
    if (count == 0) {
        return true;
    }
    if (!PointerCountOk(count, rows)) {
        return false;
    }
    destination->insert(destination->end(), rows, rows + count);
    return true;
}

bool SameCapacitySummary(const WorldCapacitySummaryV1& left,
                         const WorldCapacitySummaryV1& right) noexcept {
    static_assert(sizeof(WorldCapacitySummaryV1) ==
                  37 * sizeof(std::uint32_t));
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool CheckedCompositeCount(std::uint64_t* total, std::uint32_t add,
                           std::uint32_t limit) noexcept {
    if (*total > UINT32_MAX - static_cast<std::uint64_t>(add)) {
        return false;
    }
    *total += add;
    return *total <= limit;
}

enum class CompositeActionStatus : std::uint8_t {
    Valid,
    Invalid,
    Exceeded,
};

CompositeActionStatus ValidateCompositeActions(
    const ContainerWorldInstallViewV1& world) {
    std::vector<std::uint8_t> state(world.actionCount, 0);
    std::vector<std::uint32_t> depth(world.actionCount, 0);
    std::function<CompositeActionStatus(std::uint32_t, std::uint32_t)> visit =
        [&](std::uint32_t index,
            std::uint32_t traversalDepth) -> CompositeActionStatus {
        if (state[index] == 1) {
            return CompositeActionStatus::Invalid;
        }
        if (state[index] == 2) {
            return CompositeActionStatus::Valid;
        }
        state[index] = 1;
        const ActionProgramViewV1& action = world.actions[index];
        std::uint64_t expandedSteps = action.stepCount;
        std::uint64_t expandedFirings = 1;
        std::uint32_t transitiveMask = action.directFeatureMask;
        std::uint32_t actionDepth = 1;
        for (std::uint32_t stepIndex = 0; stepIndex < action.stepCount;
             ++stepIndex) {
            const ActionStepProgramViewV1& step = action.steps[stepIndex];
            if (step.opcode != ActionOpcodeV1::PostAction) {
                continue;
            }
            const ActionProgramViewV1* child = std::lower_bound(
                world.actions, world.actions + world.actionCount,
                step.target.stableId,
                [](const ActionProgramViewV1& candidate,
                   VnObjectId stableId) {
                    return candidate.actionStableId < stableId;
                });
            if (child == world.actions + world.actionCount ||
                child->actionStableId != step.target.stableId ||
                child->programSemanticFingerprint !=
                    step.target.expectedProgramSemanticFingerprint) {
                return CompositeActionStatus::Invalid;
            }
            const std::uint32_t childIndex =
                static_cast<std::uint32_t>(child - world.actions);
            if (traversalDepth >= world.capacity.maxActionTriggerDepth) {
                return CompositeActionStatus::Exceeded;
            }
            const CompositeActionStatus childStatus =
                visit(childIndex, traversalDepth + 1u);
            if (childStatus != CompositeActionStatus::Valid) {
                return childStatus;
            }
            expandedSteps += child->expandedStepOccurrenceCount;
            expandedFirings += child->expandedFiringCount;
            transitiveMask |= child->transitiveFeatureMask;
            actionDepth = std::max(actionDepth, depth[childIndex] + 1u);
            if (expandedSteps > UINT32_MAX || expandedFirings > UINT32_MAX) {
                return CompositeActionStatus::Exceeded;
            }
        }
        if (expandedSteps != action.expandedStepOccurrenceCount ||
            expandedFirings != action.expandedFiringCount ||
            transitiveMask != action.transitiveFeatureMask) {
            return CompositeActionStatus::Invalid;
        }
        if (action.expandedStepOccurrenceCount >
                world.capacity.maxActionExpandedWork ||
            actionDepth > world.capacity.maxActionTriggerDepth) {
            return CompositeActionStatus::Exceeded;
        }
        depth[index] = actionDepth;
        state[index] = 2;
        return CompositeActionStatus::Valid;
    };
    for (std::uint32_t index = 0; index < world.actionCount; ++index) {
        const CompositeActionStatus status = visit(index, 1);
        if (status != CompositeActionStatus::Valid) {
            return status;
        }
    }
    return CompositeActionStatus::Valid;
}

VnResult ValidateCompositeWorld(
    const PackComposition::PublishedView& published,
    std::uint64_t& worldSemanticFingerprint) {
    worldSemanticFingerprint = 0;
    const PackComposition::PublishedOwner* first = nullptr;
    std::uint64_t variableCount = 0;
    std::uint64_t propertyCount = 0;
    std::uint64_t occurrenceCount = 0;
    std::uint64_t trackCount = 0;
    std::uint64_t pieceCount = 0;
    std::uint64_t containerCount = 0;
    std::uint64_t transitionCount = 0;
    std::uint64_t actionCount = 0;
    for (const PackComposition::PublishedOwner& owner : published.owners) {
        if (owner.readiness != PackOwnerReadiness::Active) {
            continue;
        }
        if (first == nullptr) {
            first = &owner;
        } else if (first->view.identity.projectId != owner.view.identity.projectId ||
                   !SameCapacitySummary(first->view.capacity,
                                        owner.view.capacity)) {
            return VN_PACK_INCOMPATIBLE;
        }
        if (!CheckedCompositeCount(&variableCount, owner.view.variableCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&propertyCount,
                                   owner.view.playablePropertyCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&occurrenceCount,
                                   owner.view.playableOccurrenceCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&trackCount, owner.view.trackScheduleCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&pieceCount, owner.view.pieceScheduleCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&containerCount, owner.view.containerCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&transitionCount,
                                   owner.view.transitionCount,
                                   UINT32_MAX) ||
            !CheckedCompositeCount(&actionCount, owner.view.actionCount,
                                   UINT32_MAX)) {
            return VN_PACK_RESIDENT_CAPACITY;
        }
    }
    if (first == nullptr) {
        return VN_OK;
    }

    CompositeWorldScratch scratch;
    scratch.variables.reserve(static_cast<std::size_t>(variableCount));
    scratch.properties.reserve(static_cast<std::size_t>(propertyCount));
    scratch.occurrences.reserve(static_cast<std::size_t>(occurrenceCount));
    scratch.tracks.reserve(static_cast<std::size_t>(trackCount));
    scratch.pieces.reserve(static_cast<std::size_t>(pieceCount));
    scratch.containers.reserve(static_cast<std::size_t>(containerCount));
    scratch.transitions.reserve(static_cast<std::size_t>(transitionCount));
    scratch.actions.reserve(static_cast<std::size_t>(actionCount));
    scratch.view.identity.format = first->view.identity.format;
    scratch.view.identity.projectId = first->view.identity.projectId;
    scratch.view.capacity = first->view.capacity;
    for (const PackComposition::PublishedOwner& owner : published.owners) {
        if (owner.readiness != PackOwnerReadiness::Active) {
            continue;
        }
        scratch.view.identity.capabilityMask |=
            owner.view.identity.capabilityMask;
        if (!AppendRows(&scratch.variables, owner.view.variables,
                        owner.view.variableCount) ||
            !AppendRows(&scratch.properties, owner.view.playableProperties,
                        owner.view.playablePropertyCount) ||
            !AppendRows(&scratch.occurrences, owner.view.playableOccurrences,
                        owner.view.playableOccurrenceCount) ||
            !AppendRows(&scratch.tracks, owner.view.trackSchedules,
                        owner.view.trackScheduleCount) ||
            !AppendRows(&scratch.pieces, owner.view.pieceSchedules,
                        owner.view.pieceScheduleCount) ||
            !AppendRows(&scratch.containers, owner.view.containers,
                        owner.view.containerCount) ||
            !AppendRows(&scratch.transitions, owner.view.transitions,
                        owner.view.transitionCount) ||
            !AppendRows(&scratch.actions, owner.view.actions,
                        owner.view.actionCount)) {
            return VN_BAD_PACK;
        }
    }

    std::sort(scratch.variables.begin(), scratch.variables.end(),
              [](const VariableDefinitionV1& left,
                 const VariableDefinitionV1& right) {
                  return left.variableId < right.variableId;
              });
    std::sort(scratch.properties.begin(), scratch.properties.end(),
              [](const PlayablePropertyProgramViewV1& left,
                 const PlayablePropertyProgramViewV1& right) {
                  return std::pair(
                             static_cast<std::uint32_t>(
                                 left.contentPropertyKind),
                             left.stableId) <
                         std::pair(
                             static_cast<std::uint32_t>(
                                 right.contentPropertyKind),
                             right.stableId);
              });
    std::sort(scratch.occurrences.begin(), scratch.occurrences.end(),
              [](const PlayableOccurrencePropertyViewV1& left,
                 const PlayableOccurrencePropertyViewV1& right) {
                  return std::tuple(
                             static_cast<std::uint32_t>(left.parentKind),
                             left.parentStableId,
                             left.stableOccurrenceOrdinal) <
                         std::tuple(
                             static_cast<std::uint32_t>(right.parentKind),
                             right.parentStableId,
                             right.stableOccurrenceOrdinal);
              });
    std::sort(scratch.tracks.begin(), scratch.tracks.end(),
              [](const TrackScheduleProgramViewV1& left,
                 const TrackScheduleProgramViewV1& right) {
                  return left.rootDefinition.stableId <
                         right.rootDefinition.stableId;
              });
    std::sort(scratch.pieces.begin(), scratch.pieces.end(),
              [](const PieceScheduleProgramViewV1& left,
                 const PieceScheduleProgramViewV1& right) {
                  return left.rootDefinition.stableId <
                         right.rootDefinition.stableId;
              });
    std::sort(scratch.containers.begin(), scratch.containers.end(),
              [](const ContainerProgramViewV3& left,
                 const ContainerProgramViewV3& right) {
                  return left.containerStableId < right.containerStableId;
              });
    std::sort(scratch.transitions.begin(), scratch.transitions.end(),
              [](const TransitionProgramViewV1& left,
                 const TransitionProgramViewV1& right) {
                  return left.ownerContainerStableId <
                         right.ownerContainerStableId;
              });
    std::sort(scratch.actions.begin(), scratch.actions.end(),
              [](const ActionProgramViewV1& left,
                 const ActionProgramViewV1& right) {
                  return left.actionStableId < right.actionStableId;
              });

    DeduplicateRows(scratch.variables, [](const auto& row) { return row.variableId; });
    DeduplicateRows(scratch.properties, [](const auto& row) { return row.stableId; });
    DeduplicateRows(scratch.occurrences, [](const auto& row) {
        return std::tuple(row.parentKind, row.parentStableId, row.stableOccurrenceOrdinal);
    });
    DeduplicateRows(scratch.tracks, [](const auto& row) { return row.rootDefinition.stableId; });
    DeduplicateRows(scratch.pieces, [](const auto& row) { return row.rootDefinition.stableId; });
    DeduplicateRows(scratch.containers, [](const auto& row) { return row.containerStableId; });
    DeduplicateRows(scratch.transitions, [](const auto& row) { return row.ownerContainerStableId; });
    DeduplicateRows(scratch.actions, [](const auto& row) { return row.actionStableId; });
    const auto& capacity = scratch.view.capacity;
    if (scratch.variables.size() > capacity.maxVariableDefinitions ||
        scratch.properties.size() > capacity.maxPlayablePropertyPrograms ||
        scratch.occurrences.size() > capacity.maxPlayableOccurrencePropertyRows ||
        scratch.tracks.size() > capacity.maxTrackSchedulePrograms ||
        scratch.pieces.size() > capacity.maxPieceSchedulePrograms ||
        scratch.containers.size() > capacity.maxContainerPrograms ||
        scratch.actions.size() > capacity.maxActionDefinitions) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
    std::uint64_t actionStepTotal = 0;
    for (const auto& action : scratch.actions) {
        if (action.stepCount > capacity.maxActionStepsPerDefinition ||
            !CheckedCompositeCount(&actionStepTotal, action.stepCount,
                capacity.maxActionStepsTotal)) {
            return VN_PACK_RESIDENT_CAPACITY;
        }
    }

    for (ContainerProgramViewV3& container : scratch.containers) {
        if (container.transitionProgramIndex == UINT32_MAX) {
            continue;
        }
        const auto transition = std::lower_bound(
            scratch.transitions.begin(), scratch.transitions.end(),
            container.containerStableId,
            [](const TransitionProgramViewV1& candidate,
               VnObjectId stableId) {
                return candidate.ownerContainerStableId < stableId;
            });
        if (transition == scratch.transitions.end() ||
            transition->ownerContainerStableId !=
                container.containerStableId) {
            return VN_BAD_PACK;
        }
        container.transitionProgramIndex = static_cast<std::uint32_t>(
            transition - scratch.transitions.begin());
    }

    scratch.view.variables = scratch.variables.empty()
                                 ? nullptr
                                 : scratch.variables.data();
    scratch.view.variableCount =
        static_cast<std::uint32_t>(scratch.variables.size());
    scratch.view.playableProperties = scratch.properties.empty()
                                          ? nullptr
                                          : scratch.properties.data();
    scratch.view.playablePropertyCount =
        static_cast<std::uint32_t>(scratch.properties.size());
    scratch.view.playableOccurrences = scratch.occurrences.empty()
                                           ? nullptr
                                           : scratch.occurrences.data();
    scratch.view.playableOccurrenceCount =
        static_cast<std::uint32_t>(scratch.occurrences.size());
    scratch.view.trackSchedules =
        scratch.tracks.empty() ? nullptr : scratch.tracks.data();
    scratch.view.trackScheduleCount =
        static_cast<std::uint32_t>(scratch.tracks.size());
    scratch.view.pieceSchedules =
        scratch.pieces.empty() ? nullptr : scratch.pieces.data();
    scratch.view.pieceScheduleCount =
        static_cast<std::uint32_t>(scratch.pieces.size());
    scratch.view.containers =
        scratch.containers.empty() ? nullptr : scratch.containers.data();
    scratch.view.containerCount =
        static_cast<std::uint32_t>(scratch.containers.size());
    scratch.view.transitions =
        scratch.transitions.empty() ? nullptr : scratch.transitions.data();
    scratch.view.transitionCount =
        static_cast<std::uint32_t>(scratch.transitions.size());
    scratch.view.actions =
        scratch.actions.empty() ? nullptr : scratch.actions.data();
    scratch.view.actionCount =
        static_cast<std::uint32_t>(scratch.actions.size());
    scratch.view.identity.runtimeSemanticFingerprint =
        ComputeCurrentWorldFingerprintV5(scratch.view);

    const CompositeActionStatus actions =
        ValidateCompositeActions(scratch.view);
    if (actions == CompositeActionStatus::Invalid) {
        return VN_BAD_PACK;
    }
    if (actions == CompositeActionStatus::Exceeded) {
        return VN_PACK_RESIDENT_CAPACITY;
    }

    switch (ValidateCurrentWorldV1(scratch.view)) {
    case WorldInstallStatus::Ok:
        worldSemanticFingerprint =
            scratch.view.identity.runtimeSemanticFingerprint;
        return VN_OK;
    case WorldInstallStatus::InvalidFormat:
        return VN_PACK_INCOMPATIBLE;
    case WorldInstallStatus::Capacity:
        return VN_PACK_RESIDENT_CAPACITY;
    case WorldInstallStatus::CapabilityUnavailable:
        return VN_CAPABILITY_UNAVAILABLE;
    default:
        return VN_BAD_PACK;
    }
}

const PackCanonicalImage::VariablePolicyOverlay* FindVariablePolicyOverlay(
    const PackComposition::PublishedView& published, VnObjectId variableId) {
    for (const auto& overlay : published.catalog.variablePolicyOverlays) {
        if (overlay.variableId == variableId) {
            return &overlay;
        }
    }
    for (const auto& owner : published.owners) {
        if (!owner.hasVariablePolicyOverlay) {
            continue;
        }
        for (const auto& overlay : owner.variablePolicyOverlays) {
            if (overlay.variableId == variableId) {
                return &overlay;
            }
        }
    }
    return nullptr;
}

const VariableDefinitionV1* FindPublishedVariable(
    const PackComposition::PublishedView& published, VnObjectId variableId) {
    for (const auto& owner : published.owners) {
        if (owner.readiness != PackOwnerReadiness::Active ||
            owner.view.variables == nullptr) {
            continue;
        }
        for (std::uint32_t i = 0; i < owner.view.variableCount; ++i) {
            if (owner.view.variables[i].variableId == variableId) {
                return &owner.view.variables[i];
            }
        }
    }
    return nullptr;
}

const PackComposition::PublishedOwner* FindInitOwner(
    const PackComposition::PublishedView& published) {
    if (published.initPackStableId == 0) {
        return nullptr;
    }
    for (const auto& owner : published.owners) {
        if (owner.packStableId == published.initPackStableId) {
            return &owner;
        }
    }
    return nullptr;
}

VnResult ValidateInitVariablePolicy(
    const PackComposition::PublishedView& published) {
    const auto* initOwner = FindInitOwner(published);
    bool hasOverlay = published.catalog.hasVariablePolicyOverlay;
    std::vector<const PackCanonicalImage::VariablePolicyOverlay*> overlayRows;
    const auto addOverlays =
        [&overlayRows](
            const std::vector<PackCanonicalImage::VariablePolicyOverlay>&
                rows) {
            for (const auto& row : rows) {
                bool seen = false;
                for (const auto* existing : overlayRows) {
                    if (existing->variableId == row.variableId) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    overlayRows.push_back(&row);
                }
            }
        };
    addOverlays(published.catalog.variablePolicyOverlays);
    if (initOwner != nullptr && initOwner->hasVariablePolicyOverlay) {
        hasOverlay = true;
        addOverlays(initOwner->variablePolicyOverlays);
    }
    if (!hasOverlay) {
        return VN_OK;
    }

    std::vector<VnObjectId> initIds;
    if (initOwner != nullptr &&
        initOwner->readiness == PackOwnerReadiness::Active &&
        initOwner->view.variables != nullptr) {
        initIds.reserve(initOwner->view.variableCount);
        for (std::uint32_t i = 0; i < initOwner->view.variableCount; ++i) {
            initIds.push_back(initOwner->view.variables[i].variableId);
        }
    }
    std::sort(initIds.begin(), initIds.end());
    initIds.erase(std::unique(initIds.begin(), initIds.end()), initIds.end());

    std::vector<VnObjectId> overlayIds;
    overlayIds.reserve(overlayRows.size());
    for (const auto* overlay : overlayRows) {
        overlayIds.push_back(overlay->variableId);
    }
    std::sort(overlayIds.begin(), overlayIds.end());
    if (initIds != overlayIds) {
        return VN_VARIABLE_POLICY_INCOMPLETE;
    }

    for (const auto* overlay : overlayRows) {
        const auto* variable =
            FindPublishedVariable(published, overlay->variableId);
        if (variable == nullptr) {
            return VN_VARIABLE_POLICY_INCOMPLETE;
        }
        for (const auto& label : overlay->labels)
            if (label.anchor < variable->rangeMin || label.anchor > variable->rangeMax)
                return VN_BAD_PACK;
        const std::int32_t* anchors =
            overlay->discreteAnchors.empty()
                ? nullptr
                : overlay->discreteAnchors.data();
        std::uint64_t recomputed = 0;
        if (!ComputeVariableExecutionFingerprintV1(
                variable->variableId,
                static_cast<std::uint8_t>(variable->kind),
                1,
                overlay->writers, variable->rangeMin, variable->rangeMax,
                variable->defaultValue, anchors,
                static_cast<std::uint32_t>(overlay->discreteAnchors.size()),
                &recomputed) ||
            recomputed != overlay->executionFingerprint) {
            return VN_BAD_PACK;
        }
    }
    return VN_OK;
}

VnResult ValidateVariableRequirements(
    const PackComposition::PublishedView& published) {
    for (const auto& owner : published.owners) {
        if (!owner.hasVariableRequirements) {
            continue;
        }
        for (const auto& requirement : owner.variableRequirements) {
            if (requirement.identityClass != 1) {
                return VN_BAD_PACK;
            }
            const auto* overlay =
                FindVariablePolicyOverlay(published, requirement.stableId);
            const auto* variable =
                FindPublishedVariable(published, requirement.stableId);
            if (overlay == nullptr || variable == nullptr) {
                return VN_BAD_PACK;
            }
            const std::int32_t* anchors =
                overlay->discreteAnchors.empty()
                    ? nullptr
                    : overlay->discreteAnchors.data();
            std::uint64_t recomputed = 0;
            if (!ComputeVariableExecutionFingerprintV1(
                    variable->variableId,
                    static_cast<std::uint8_t>(variable->kind),
                    1,
                    overlay->writers, variable->rangeMin, variable->rangeMax,
                    variable->defaultValue, anchors,
                    static_cast<std::uint32_t>(overlay->discreteAnchors.size()),
                    &recomputed) ||
                recomputed != requirement.expectedExecutionFingerprint
                || (requirement.expectedLabelMappingFingerprint != 0
                    && requirement.expectedLabelMappingFingerprint != overlay->labelMappingFingerprint)) {
                return VN_BAD_PACK;
            }
        }
    }
    return VN_OK;
}

VnResult ValidateCrossOwnerReferences(
    PackComposition::PublishedView& published, std::size_t maxPacks) {
    for (std::uint32_t slot = 0; slot < published.owners.size(); ++slot) {
        if (published.owners[slot].readiness != PackOwnerReadiness::Active) {
            continue;
        }
        std::vector<std::uint8_t> member;
        const VnResult closure =
            BuildDeclaredClosure(published, slot, maxPacks, &member);
        if (closure != VN_OK) {
            return closure;
        }
        const VnResult refs =
            ValidateOwnerExternalRefs(published, slot, member);
        if (refs != VN_OK) {
            return refs;
        }
        const VnResult ancestors = ValidateStructuralAncestorRefs(
            published, member, published.owners[slot].view);
        if (ancestors != VN_OK) {
            return ancestors;
        }
    }
    return VN_OK;
}

VnResult BuildPublishedResourceClosure(
    const PackComposition::PublishedView* published, const PackPartitionLease& lease,
    PackResourceRevisionToken* outTokens, std::size_t tokenCapacity,
    std::size_t* outTokenCount) noexcept {
    using PublishedOwner = PackComposition::PublishedOwner;
    if (outTokenCount == nullptr ||
        (tokenCapacity != 0 && outTokens == nullptr)) {
        return VN_INVALID_ARG;
    }
    *outTokenCount = 0;
    if (published == nullptr || lease.resourceToken == 0) {
        return VN_CONTENT_NOT_LOADED;
    }
    const auto ownerForToken = [&](PackResourceRevisionToken token)
        -> const PublishedOwner* {
        for (const PublishedOwner& owner : published->owners) {
            if (owner.readiness == PackOwnerReadiness::Active &&
                owner.resourceToken == token) {
                return &owner;
            }
        }
        return nullptr;
    };
    const PublishedOwner* root = ownerForToken(lease.resourceToken);
    if (root == nullptr || root->handle != lease.handle ||
        root->packStableId != lease.packStableId) {
        return VN_CONTENT_NOT_LOADED;
    }
    if (tokenCapacity == 0) {
        return VN_BACKPRESSURE;
    }
    outTokens[0] = root->resourceToken;
    std::size_t count = 1;
    for (std::size_t cursor = 0; cursor < count; ++cursor) {
        const PublishedOwner* consumer = ownerForToken(outTokens[cursor]);
        if (consumer == nullptr) {
            return VN_CONTENT_NOT_LOADED;
        }
        for (const PackDependencyEdge& edge : consumer->dependencyEdges) {
            const std::uint64_t providerStableId =
                edge.prerequisitePackStableId;
            const PublishedOwner* provider = nullptr;
            for (const PublishedOwner& candidate : published->owners) {
                if (candidate.readiness == PackOwnerReadiness::Active &&
                    candidate.packStableId == providerStableId) {
                    provider = &candidate;
                    break;
                }
            }
            if (provider == nullptr || provider->resourceToken == 0) {
                return VN_CONTENT_NOT_LOADED;
            }
            bool duplicate = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (outTokens[i] == provider->resourceToken) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (count == tokenCapacity) {
                return VN_BACKPRESSURE;
            }
            outTokens[count++] = provider->resourceToken;
        }
    }
    *outTokenCount = count;
    return VN_OK;
}

VnResult BuildPlaybackOwnerStore(PackComposition::PublishedView& published,
                                 std::size_t maxPacks,
                                 std::uint64_t worldSemanticFingerprint,
                                 const PackAttenuationBindingMapV1& attenuationBindings) {
    auto store = std::make_shared<PackPlaybackOwnerStore>();
    store->worldSemanticFingerprint = worldSemanticFingerprint;
    store->attenuationBindings = attenuationBindings;
    store->owners.resize(published.owners.size());
    for (std::uint32_t slot = 0; slot < published.owners.size(); ++slot) {
        const PackComposition::PublishedOwner& owner = published.owners[slot];
        PackPlaybackOwnerStore::Owner& stored = store->owners[slot];
        stored.snapshot.packStableId = owner.packStableId;
        stored.snapshot.handle = owner.handle;
        stored.snapshot.contentRevision = owner.contentRevision;
        stored.snapshot.partitionRevision = owner.partitionRevision;
        stored.snapshot.resourceRevision = owner.resourceRevision;
        stored.snapshot.resourceToken = owner.resourceToken;
        stored.snapshot.definitionBody = owner.body;
        stored.snapshot.definitionView = owner.view;
        stored.snapshot.voiceBindings = owner.voiceBindings;
        if (owner.readiness != PackOwnerReadiness::Active) {
            continue;
        }
        std::vector<std::uint8_t> member;
        const VnResult closure =
            BuildDeclaredClosure(published, slot, maxPacks, &member);
        if (closure != VN_OK) {
            return closure;
        }
        for (std::uint32_t ownerSlot = 0; ownerSlot < member.size();
             ++ownerSlot) {
            if (member[ownerSlot] != 0) {
                stored.closureOwnerSlots.push_back(ownerSlot);
            }
        }
        if (owner.resourceToken != 0) {
            auto tokens = std::make_shared<std::vector<PackResourceRevisionToken>>(stored.closureOwnerSlots.size());
            PackPartitionLease lease{};
            lease.handle = owner.handle;
            lease.packStableId = owner.packStableId;
            lease.resourceToken = owner.resourceToken;
            std::size_t count = 0;
            const auto resources = BuildPublishedResourceClosure(&published, lease, tokens->data(), tokens->size(), &count);
            if (resources == VN_OK) {
                tokens->resize(count);
                stored.resourceTokens = std::move(tokens);
            } else if (resources != VN_CONTENT_NOT_LOADED) return resources;
        }
    }
    published.playbackStore = std::move(store);
    return VN_OK;
}

} // namespace

PackComposition::PackComposition(std::size_t sharedActionDeferredCapacity,
                                 std::size_t playableCapacity,
                                 std::size_t maxPacks,
                                 std::uint64_t generationFloor)
    : sharedCapacity_(sharedActionDeferredCapacity),
      playableCapacity_(playableCapacity),
      maxPacks_(maxPacks),
      generationFloor_(generationFloor) {
    const std::uint64_t leaseTotal =
        static_cast<std::uint64_t>(sharedActionDeferredCapacity) +
        playableCapacity;
    if (leaseTotal > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("pack lease pool overflow");
    }
    const std::size_t leaseCount = static_cast<std::size_t>(leaseTotal);
    leases_.resize(leaseCount);
    originalLeaseRecords_.resize(leaseCount);
    originalLeaseIncarnation_ = IssueOriginalPackIncarnation();
    frozenClosures_.resize(leaseCount);
}

PackComposition::~PackComposition() {
    for (auto& record : originalLeaseRecords_) {
        if (record.handle == nullptr) continue;
        record.handle->owner_ = nullptr;
        record.handle->lastPhase_ = OriginalLeasePhase::QuiescedDisposed;
        record.handle = nullptr;
    }
}

PackComposition::OriginalLeaseEpisode::~OriginalLeaseEpisode() { Detach(); }
PackComposition::OriginalLeaseEpisode::OriginalLeaseEpisode(OriginalLeaseEpisode&& other) noexcept {
    MoveFrom(other);
}
PackComposition::OriginalLeaseEpisode& PackComposition::OriginalLeaseEpisode::operator=(OriginalLeaseEpisode&& other) noexcept {
    if (this != &other) { Detach(); MoveFrom(other); }
    return *this;
}
void PackComposition::OriginalLeaseEpisode::MoveFrom(OriginalLeaseEpisode& other) noexcept {
    owner_ = other.owner_; index_ = other.index_; token_ = other.token_;
    incarnation_ = other.incarnation_; lastPhase_ = other.lastPhase_;
    if (owner_ != nullptr) {
        assert(owner_->MatchesOriginalLease(other));
        owner_->originalLeaseRecords_[index_].handle = this;
    }
    other.owner_ = nullptr; other.index_ = SIZE_MAX; other.token_ = 0;
    other.incarnation_ = 0; other.lastPhase_ = OriginalLeasePhase::Unissued;
}
void PackComposition::OriginalLeaseEpisode::Detach() noexcept {
    if (owner_ != nullptr && owner_->MatchesOriginalLease(*this)) {
        auto& record = owner_->originalLeaseRecords_[index_];
        record.handle = nullptr;
        record.phase = OriginalLeasePhase::Faulted;
        lastPhase_ = record.phase;
    }
    owner_ = nullptr; index_ = SIZE_MAX; token_ = 0; incarnation_ = 0;
}
bool PackComposition::OriginalLeaseEpisode::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalLease(*this);
}
PackComposition::OriginalLeasePhase PackComposition::OriginalLeaseEpisode::Phase() const noexcept {
    return Valid() ? owner_->originalLeaseRecords_[index_].phase : lastPhase_;
}
PackPartitionLease PackComposition::OriginalLeaseEpisode::OriginalLease() const noexcept {
    return Valid() ? owner_->originalLeaseRecords_[index_].lease : PackPartitionLease{};
}
bool PackComposition::MatchesOriginalLease(const OriginalLeaseEpisode& original) const noexcept {
    if (original.owner_ != this || original.incarnation_ != originalLeaseIncarnation_ ||
        original.index_ >= originalLeaseRecords_.size()) return false;
    const auto& record = originalLeaseRecords_[original.index_];
    return record.active && record.handle == &original && record.lease.leaseToken == original.token_;
}
bool PackComposition::SameFullOriginalLease(const PackPartitionLease& a, const PackPartitionLease& b) noexcept {
    return SameLease(a, b) && a.compositeGeneration == b.compositeGeneration;
}
bool PackComposition::HasOriginalLeaseEpisode(const PackPartitionLease& lease) const noexcept {
    for (const auto& record : originalLeaseRecords_)
        if (record.active && SameLeaseIdentity(record.lease, lease)) return true;
    return false;
}
bool PackComposition::BindOriginalResources(OriginalLeaseEpisode& original, void* receiver,
    PackResourceClosureToken resource) const noexcept {
    if (!MatchesOriginalLease(original) || receiver == nullptr || resource == 0) return false;
    auto& record = originalLeaseRecords_[original.index_];
    if (record.phase != OriginalLeasePhase::Preparing || record.resourceReceiver != nullptr) return false;
    record.resourceReceiver = receiver; record.acquiredResource = resource;
    return true;
}
VnResult PackComposition::EndOriginalDefinition(std::size_t index) const noexcept {
    if (index >= originalLeaseRecords_.size()) return VN_STALE_HANDLE;
    auto& record = originalLeaseRecords_[index];
    if (!record.active || record.definitionEnded) return VN_STALE_HANDLE;
    auto& row = leases_[index];
    const bool prepared = row.state == LeaseState::Prepared && row.refs == 0;
    const bool live = row.state == LeaseState::Live && row.refs == 1;
    if ((!prepared && !live) || !SameFullOriginalLease(row.lease, record.lease)) {
        record.definitionResult = VN_STALE_HANDLE;
        record.phase = OriginalLeasePhase::Faulted;
        return record.definitionResult;
    }
    ClearLeaseRow(row);
    record.definitionEnded = true; record.definitionResult = VN_OK;
    if (record.phase != OriginalLeasePhase::Faulted) record.phase = OriginalLeasePhase::DefinitionEnded;
    return VN_OK;
}
VnResult PackComposition::DischargeOriginalLease(OriginalLeaseEpisode& original) const noexcept {
    if (!MatchesOriginalLease(original)) return VN_STALE_HANDLE;
    auto& record = originalLeaseRecords_[original.index_];
    if (!record.definitionEnded || (record.resourceReceiver != nullptr && !record.resourceReturned) ||
        record.phase == OriginalLeasePhase::Faulted) return VN_RUNTIME_RECREATE_REQUIRED;
    --SliceOccupied(record.tag);
    record.active = false; record.phase = OriginalLeasePhase::Released; record.handle = nullptr;
    original.lastPhase_ = record.phase; original.owner_ = nullptr;
    original.index_ = SIZE_MAX; original.token_ = 0; original.incarnation_ = 0;
    return VN_OK;
}
VnResult PackComposition::AbortOriginalPreparedLease(OriginalLeaseEpisode& original) const noexcept {
    if (!MatchesOriginalLease(original)) return VN_STALE_HANDLE;
    const auto& record = originalLeaseRecords_[original.index_];
    if (record.phase != OriginalLeasePhase::Preparing || record.resourceReceiver != nullptr) return VN_IN_USE;
    const auto ended = EndOriginalDefinition(original.index_);
    return ended == VN_OK ? DischargeOriginalLease(original) : ended;
}
void PackComposition::QuiesceOriginalLease(std::size_t index) const noexcept {
    auto& record = originalLeaseRecords_[index];
    if (!record.active) return;
    if (!record.definitionEnded) (void)EndOriginalDefinition(index);
    if (record.definitionEnded) --SliceOccupied(record.tag);
    if (record.handle != nullptr) {
        record.handle->owner_ = nullptr;
        record.handle->lastPhase_ = OriginalLeasePhase::QuiescedDisposed;
    }
    record.handle = nullptr; record.active = false;
    record.phase = OriginalLeasePhase::QuiescedDisposed;
}

VnResult PackComposition::EnterPackAuthority() noexcept {
    if (authority_ == PackAuthorityMode::DirectWorld) {
        return VN_AUTHORITY_CONFLICT;
    }
    authority_ = PackAuthorityMode::Pack;
    return VN_OK;
}

VnResult PackComposition::EnterDirectWorldAuthority() noexcept {
    if (authority_ == PackAuthorityMode::Pack) {
        return VN_AUTHORITY_CONFLICT;
    }
    authority_ = PackAuthorityMode::DirectWorld;
    return VN_OK;
}

VnResult PackComposition::PrepareCandidate(const PackCanonicalImage& image,
                                           VnPackHandle handle,
                                           CandidateOwner* out,
                                           PackResourceRevisionToken resourceToken) const {
    if (out == nullptr || !image.hasRequirement || image.packStableId == 0) {
        return VN_INVALID_ARG;
    }
    CandidateOwner candidate;
    candidate.packStableId = image.packStableId;
    candidate.handle = handle;
    candidate.contentRevision = image.requirement.packContentRevision;
    candidate.partitionRevision = image.requirement.definitionPartitionRevision;
    candidate.resourceRevision = image.requirement.resourceClosureRevision;
    candidate.resourceToken = resourceToken;
    candidate.slotIndex = static_cast<std::uint32_t>(handle);
    candidate.hasPartition = image.hasPartition;
    if (image.hasPartition) {
        candidate.partitionBody = image.partitionBody;
    }
    auto bindings = std::make_shared<std::vector<PackVoiceBinding>>();
    for (const PackBindingAttachmentEntry& attachment : image.bindingAttachments) {
        if (attachment.scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
            return VN_BAD_PACK;
        }
        if (attachment.scopeKind != VN_TARGET_SCOPE_VOICE) {
            continue;
        }
        if (image.packKind == kPackKindInit) {
            return VN_BAD_PACK;
        }
        const auto definition = std::find_if(image.bindingDefinitions.begin(),
            image.bindingDefinitions.end(), [&](const auto& value) {
                return value.id == attachment.bindingId;
            });
        if (definition == image.bindingDefinitions.end() ||
            definition->desc.targetScopeKind != VN_TARGET_SCOPE_VOICE) {
            return VN_BAD_PACK;
        }
        PackVoiceBinding binding;
        binding.objectId = attachment.objectId;
        binding.bindingId = attachment.bindingId;
        binding.descriptor = definition->desc;
        const auto& curve = definition->desc.curve;
        const auto* target = FindTargetMeta(VN_TARGET_SCOPE_VOICE, definition->desc.targetId);
        if (target == nullptr ||
            !IsValidBindingTransitionMs(definition->desc.transitionMs) ||
            (definition->desc.transitionMs != 0 && !target->supportsTransition) ||
            curve.pointCount == 0 || curve.pointCount > kMaxCurvePoints) {
            return VN_BAD_PACK;
        }
        CurvePoint points[kMaxCurvePoints]{};
        CurveHandlePair handles[kMaxCurvePoints - 1]{};
        for (std::uint32_t i = 0; i < curve.pointCount; ++i) {
            points[i] = {curve.points[i].x, curve.points[i].y};
        }
        for (std::uint32_t i = 0; i + 1 < curve.pointCount; ++i) {
            handles[i] = {curve.handles[i].h1x, curve.handles[i].h1y,
                          curve.handles[i].h2x, curve.handles[i].h2y};
        }
        if (!binding.curve.SetPoints(points, handles, curve.pointCount)) {
            return VN_BAD_PACK;
        }
        bindings->push_back(std::move(binding));
    }
    std::sort(bindings->begin(), bindings->end(), [](const auto& a, const auto& b) {
        return std::pair(a.objectId, a.bindingId) < std::pair(b.objectId, b.bindingId);
    });
    if (std::adjacent_find(bindings->begin(), bindings->end(), [](const auto& a, const auto& b) {
            return a.objectId == b.objectId && a.bindingId == b.bindingId;
        }) != bindings->end()) {
        return VN_BAD_PACK;
    }
    for (const auto& definition : image.bindingDefinitions) {
        if (definition.desc.targetScopeKind != VN_TARGET_SCOPE_VOICE &&
            definition.desc.targetScopeKind != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
            continue;
        }
        if (image.packKind == kPackKindInit) {
            return VN_BAD_PACK;
        }
        if (definition.desc.targetScopeKind != VN_TARGET_SCOPE_VOICE) {
            continue;
        }
        const bool attached = std::any_of(
            image.bindingAttachments.begin(), image.bindingAttachments.end(),
            [&](const PackBindingAttachmentEntry& attachment) {
                return attachment.bindingId == definition.id &&
                       attachment.scopeKind == VN_TARGET_SCOPE_VOICE;
            });
        if (!attached) {
            return VN_BAD_PACK;
        }
    }
    candidate.voiceBindings = std::move(bindings);
    candidate.bindingManifest.definitionIds.clear();
    candidate.bindingManifest.attachments.clear();
    candidate.bindingManifest.definitionIds.reserve(image.bindingDefinitions.size());
    const auto definitionEntersTable = [&](std::uint32_t scopeKind) {
        return image.packKind == kPackKindInit &&
               (scopeKind == VN_TARGET_SCOPE_BUS ||
                scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM);
    };
    const auto attachmentEntersTable = [&](std::uint32_t scopeKind) {
        return image.packKind == kPackKindInit &&
               (scopeKind == VN_TARGET_SCOPE_BUS ||
                scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM);
    };
    for (const auto& definition : image.bindingDefinitions) {
        if (!definitionEntersTable(definition.desc.targetScopeKind)) {
            continue;
        }
        candidate.bindingManifest.definitionIds.push_back(definition.id);
    }
    for (const auto& attachment : image.bindingAttachments) {
        if (!attachmentEntersTable(attachment.scopeKind)) {
            continue;
        }
        candidate.bindingManifest.attachments.push_back(attachment);
    }
    candidate.definitionBytes = image.partitionBody.size() +
        candidate.voiceBindings->size() * sizeof(PackVoiceBinding);
    candidate.variablePolicyOverlays = image.variablePolicyOverlays;
    candidate.hasVariablePolicyOverlay = image.hasVariablePolicyOverlay;
    candidate.hasVariableNameCatalog = image.hasVariableNameCatalog;
    candidate.variableNameCatalog = image.variableNameCatalog;
    candidate.variableRequirements = image.variableRequirements;
    candidate.hasVariableRequirements = image.hasVariableRequirements;
    for (const PackDependencyEdge& edge : image.dependencyEdges) {
        if (edge.required01 != 0) {
            candidate.dependencyEdges.push_back(edge);
        }
    }
    if (image.packKind == kPackKindInit && image.hasGameplayCatalog) {
        candidate.hasGameplayCatalog = true;
        candidate.gameplayCatalog = image.gameplayCatalog;
    }
    candidate.hasBusDefinitionSnapshot = image.hasBusDefinitionSnapshot;
    candidate.busDefinitionSnapshotHeader = image.busDefinitionSnapshotHeader;
    candidate.requirementTriple = RequirementBusTriple(image.requirement);
    if (image.hasPartition) {
        candidate.partitionTriple = PartitionBusTriple(image.partitionHeader);
    }
    *out = std::move(candidate);
    return VN_OK;
}

VnResult PackComposition::DecodeOwner(CandidateOwner& owner) const {
    if (!owner.hasPartition) {
        return VN_OK;
    }
    return VN_OK;
}

VnResult PackComposition::BuildIndex(PublishedView& view,
                                     PackOwnerLocalFacts* facts) const {
    view.index.clear();
    std::uint64_t loadedCount = 0;
    std::uint64_t loadedBytes = 0;
    for (std::uint32_t slot = 0; slot < view.owners.size(); ++slot) {
        PublishedOwner& owner = view.owners[slot];
        if (owner.readiness != PackOwnerReadiness::Active) {
            continue;
        }
        if (!CompanionTablesSorted(owner.view) ||
            !PointerCountOk(owner.view.variableCount, owner.view.variables) ||
            !PointerCountOk(owner.view.actionCount, owner.view.actions)) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t prior = 0; prior < slot; ++prior) {
            if (view.owners[prior].packStableId == owner.packStableId) {
                return VN_BAD_PACK;
            }
        }
        if (owner.view.actionCount != 0 && owner.view.actions != nullptr) {
            for (std::uint32_t i = 0; i < owner.view.actionCount; ++i) {
                IdentityIndexEntry entry;
                entry.stableId = owner.view.actions[i].actionStableId;
                entry.packSlot = slot;
                entry.localRow = i;
                entry.identityClass = kInitCatalogIdentityClassAction;
                view.index.push_back(entry);
                ++loadedCount;
            }
        }
        if (owner.view.playablePropertyCount != 0 &&
            owner.view.playableProperties != nullptr) {
            for (std::uint32_t i = 0; i < owner.view.playablePropertyCount; ++i) {
                IdentityIndexEntry entry;
                entry.stableId = owner.view.playableProperties[i].stableId;
                entry.packSlot = slot;
                entry.localRow = i;
                entry.identityClass = kInitCatalogIdentityClassPlayable;
                view.index.push_back(entry);
                ++loadedCount;
            }
        }
        if (owner.view.variableCount != 0 && owner.view.variables != nullptr) {
            for (std::uint32_t i = 0; i < owner.view.variableCount; ++i) {
                IdentityIndexEntry entry;
                entry.stableId = owner.view.variables[i].variableId;
                entry.packSlot = slot;
                entry.localRow = i;
                entry.identityClass = kInitCatalogIdentityClassVariable;
                view.index.push_back(entry);
                ++loadedCount;
            }
        }
    }
    std::sort(view.index.begin(), view.index.end(),
              [&](const IdentityIndexEntry& a, const IdentityIndexEntry& b) {
                  return std::pair(a.stableId, view.owners[a.packSlot].packStableId) <
                         std::pair(b.stableId, view.owners[b.packSlot].packStableId);
              });
    std::vector<std::uint8_t> leftDefinition;
    std::vector<std::uint8_t> rightDefinition;
    std::vector<std::pair<const PackVoiceBinding*, std::uint64_t>> bindings;
    for (const auto& owner : view.owners) {
        if (owner.voiceBindings == nullptr) continue;
        for (const auto& binding : *owner.voiceBindings) {
            bindings.emplace_back(&binding, owner.packStableId);
        }
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
        return std::pair(a.first->bindingId, a.second) < std::pair(b.first->bindingId, b.second);
    });
    for (std::size_t i = 1; i < bindings.size(); ++i) {
        const auto& left = bindings[i - 1];
        const auto& right = bindings[i];
        if (left.first->bindingId != right.first->bindingId) continue;
        leftDefinition.resize(kPackV2BindingDefinitionPayloadBytes);
        rightDefinition.resize(kPackV2BindingDefinitionPayloadBytes);
        SerializePackV2BindingDefinition(left.first->bindingId, left.first->descriptor,
                                         leftDefinition.data());
        SerializePackV2BindingDefinition(right.first->bindingId, right.first->descriptor,
                                         rightDefinition.data());
        if (leftDefinition != rightDefinition) {
            facts->conflictingStableId = right.first->bindingId;
            facts->conflictingPackA = left.second;
            facts->conflictingPackB = right.second;
            return VN_BAD_PACK;
        }
    }
    for (std::size_t i = 1; i < view.index.size(); ++i) {
        const auto& left = view.index[i - 1];
        const auto& right = view.index[i];
        if (left.stableId == right.stableId) {
            if (left.packSlot == right.packSlot ||
                left.identityClass != right.identityClass ||
                !EncodeIdentityDefinition(view.owners[left.packSlot], left, leftDefinition) ||
                !EncodeIdentityDefinition(view.owners[right.packSlot], right, rightDefinition) ||
                leftDefinition != rightDefinition) {
                facts->conflictingStableId = right.stableId;
                facts->conflictingPackA = view.owners[left.packSlot].packStableId;
                facts->conflictingPackB = view.owners[right.packSlot].packStableId;
                return VN_BAD_PACK;
            }
            --loadedCount;
        }
    }
    loadedBytes = static_cast<std::uint64_t>(view.index.size()) * kIndexEntryBytes;
    facts->loadedCount = loadedCount;
    facts->loadedBytes = loadedBytes;
    if (facts->loadedCount > facts->highWaterLoadedCount) {
        facts->highWaterLoadedCount = facts->loadedCount;
    }
    if (facts->loadedBytes > facts->highWaterLoadedBytes) {
        facts->highWaterLoadedBytes = facts->loadedBytes;
    }
    return VN_OK;
}

VnResult PackComposition::Commit(std::vector<CandidateOwner> owners,
                                 const PackCanonicalImage* catalogOrNull,
                                 VnPackHandle initHandle,
                                 std::uint32_t maxActions,
                                 std::uint64_t serviceFrame) {
    PreparedCommit prepared;
    const VnResult result = PrepareCommit(
        std::move(owners), catalogOrNull, initHandle, maxActions,
        serviceFrame, &prepared);
    if (result == VN_OK) {
        CommitPrepared(&prepared);
    }
    return result;
}

VnResult PackComposition::PrepareCommit(
    std::vector<CandidateOwner> owners,
    const PackCanonicalImage* catalogOrNull, VnPackHandle initHandle,
    std::uint32_t maxActions, std::uint64_t serviceFrame,
    PreparedCommit* out,
    const PackAttenuationBindingMapV1* attenuationBindings,
    const CanonicalPackSetLedgerV1* ledger, bool clearInit) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedCommit{};
    if (authority_ == PackAuthorityMode::DirectWorld) {
        return VN_AUTHORITY_CONFLICT;
    }
    const std::uint64_t prior =
        published_ != nullptr ? published_->compositeGeneration : generationFloor_;
    if (prior == UINT64_MAX) {
        return VN_RUNTIME_RECREATE_REQUIRED;
    }
    PackAttenuationBindingMapV1 effectiveBindings;
    if (attenuationBindings != nullptr) {
        effectiveBindings = *attenuationBindings;
    } else if (published_ != nullptr && published_->playbackStore != nullptr) {
        effectiveBindings = published_->playbackStore->attenuationBindings;
    }
    if (!effectiveBindings.Valid()) return VN_BAD_PACK;
    PreparedCommit prepared;
    prepared.facts_ = facts_;
    prepared.facts_.conflictingStableId = {};
    prepared.facts_.conflictingProjectId = {};
    prepared.facts_.conflictingPackA = 0;
    prepared.facts_.conflictingPackB = 0;
    auto next = std::make_shared<PublishedView>();
    next->compositeGeneration = prior + 1;
    next->owners.reserve(owners.size());
    for (CandidateOwner& candidate : owners) {
        PublishedOwner owner;
        owner.packStableId = candidate.packStableId;
        owner.handle = candidate.handle;
        owner.contentRevision = candidate.contentRevision;
        owner.partitionRevision = candidate.partitionRevision;
        owner.resourceRevision = candidate.resourceRevision;
        owner.resourceToken = candidate.resourceToken;
        owner.slotIndex = candidate.slotIndex;
        owner.readiness = candidate.readiness;
        owner.dependencyEdges = std::move(candidate.dependencyEdges);
        owner.providerClosureFingerprint = ProviderClosureFingerprint(owner);
        owner.voiceBindings = std::move(candidate.voiceBindings);
        owner.definitionBytes = candidate.definitionBytes;
        owner.hasGameplayCatalog = candidate.hasGameplayCatalog;
        owner.gameplayCatalog = std::move(candidate.gameplayCatalog);
        owner.hasVariableNameCatalog = candidate.hasVariableNameCatalog;
        owner.variableNameCatalog = std::move(candidate.variableNameCatalog);
        if (candidate.body != nullptr) {
            if (candidate.body->ProductionView() == nullptr) {
                return VN_BAD_PACK;
            }
            owner.body = std::move(candidate.body);
            owner.view = *owner.body->ProductionView();
        } else if (candidate.hasPartition) {
            // ParsePackCanonicalImage already refused a non-current
            // bodyVintage. This decode is the current table only.
            auto decodedBody = std::make_shared<CurrentWorldBodyImage>();
            const CurrentWorldBodyDecodeResult decoded =
                decodedBody->DecodeWithResult(
                    candidate.partitionBody.data(),
                    candidate.partitionBody.size(),
                    kCanonicalDefinitionPartitionBodyVintage,
                    CurrentWorldDecodeMode::StrictArtifact);
            if (decoded != CurrentWorldBodyDecodeResult::Ok) {
                return decoded == CurrentWorldBodyDecodeResult::Capacity
                           ? VN_PACK_RESIDENT_CAPACITY
                           : VN_BAD_PACK;
            }
            owner.body = std::move(decodedBody);
            owner.view = *owner.body->ProductionView();
        }
        owner.variablePolicyOverlays =
            std::move(candidate.variablePolicyOverlays);
        owner.hasVariablePolicyOverlay = candidate.hasVariablePolicyOverlay;
        owner.variableRequirements = std::move(candidate.variableRequirements);
        owner.hasVariableRequirements = candidate.hasVariableRequirements;
        owner.hasBusDefinitionSnapshot = candidate.hasBusDefinitionSnapshot;
        owner.busDefinitionSnapshotHeader = candidate.busDefinitionSnapshotHeader;
        owner.requirementTriple = candidate.requirementTriple;
        owner.partitionTriple = candidate.partitionTriple;
        owner.bindingManifest = std::move(candidate.bindingManifest);
        next->owners.push_back(std::move(owner));
    }
    if (ledger != nullptr) {
        next->ledger = *ledger;
    }
    if (clearInit) {
        next->hasCatalog = false;
        next->catalog = {};
        next->initPackStableId = 0;
        next->initHandle = 0;
        next->attenuationBindings = {};
        prepared.facts_.catalogCount = 0;
        prepared.facts_.catalogBytes = 0;
    } else if (catalogOrNull != nullptr) {
        next->catalog = *catalogOrNull;
        next->hasCatalog = catalogOrNull->hasCatalog;
        next->initPackStableId = catalogOrNull->packStableId;
        next->initHandle = initHandle;
        prepared.facts_.catalogCount =
            catalogOrNull->catalogHeader.catalogEntryCount;
        prepared.facts_.catalogBytes =
            catalogOrNull->catalogHeader.catalogBytes;
    } else if (published_ != nullptr) {
        next->catalog = published_->catalog;
        next->hasCatalog = published_->hasCatalog;
        next->initPackStableId = published_->initPackStableId;
        next->initHandle = published_->initHandle;
    }
    if (!clearInit) {
        next->attenuationBindings = effectiveBindings;
    }
    for (const PublishedOwner& owner : next->owners) {
        if (!owner.hasGameplayCatalog) {
            continue;
        }
        if (next->initHandle != 0 && owner.handle != next->initHandle) {
            continue;
        }
        if (next->initPackStableId != 0 &&
            owner.packStableId != next->initPackStableId) {
            continue;
        }
        next->hasGameplayCatalog = true;
        next->gameplayCatalog = owner.gameplayCatalog;
        if (next->initPackStableId == 0) {
            next->initPackStableId = owner.packStableId;
        }
        if (!clearInit && next->initHandle == 0) {
            next->initHandle = owner.handle;
        }
        break;
    }
    for (const PublishedOwner& owner : next->owners) {
        if (owner.body == nullptr) continue;
        if (next->projectId != VnObjectId{} &&
            next->projectId != owner.view.identity.projectId) return VN_BAD_PACK;
        next->projectId = owner.view.identity.projectId;
    }
    if (next->hasGameplayCatalog &&
        next->projectId != ObjectIdFromBytes(next->gameplayCatalog.header.projectGuid)) {
        return VN_BAD_PACK;
    }
    for (const auto& owner : next->owners) {
        if (!owner.hasVariableNameCatalog) continue;
        if (clearInit || owner.readiness != PackOwnerReadiness::Active) continue;
        if (next->initHandle != 0 && owner.handle != next->initHandle) continue;
        if (next->hasVariableNameCatalog ||
            ObjectIdFromBytes(owner.variableNameCatalog.projectGuid) != next->projectId ||
            (next->initPackStableId != 0 && owner.packStableId != next->initPackStableId))
            return VN_BAD_PACK;
        std::set<VnObjectId> definitions;
        for (std::uint32_t i = 0; i < owner.view.variableCount; ++i)
            definitions.insert(owner.view.variables[i].variableId);
        for (const auto& row : owner.variableNameCatalog.rows) {
            if (definitions.erase(row.variableId) != 1) return VN_BAD_PACK;
        }
        if (!definitions.empty()) return VN_BAD_PACK;
        next->hasVariableNameCatalog = true;
        next->variableNameCatalog = owner.variableNameCatalog;
    }
    if (next->hasGameplayCatalog) {
        const bool sameCatalog = published_ != nullptr && published_->hasGameplayCatalog
            && published_->initHandle == next->initHandle
            && published_->gameplayCatalog.header.digest == next->gameplayCatalog.header.digest;
        for (std::size_t i = 0; i < next->gameplayCatalog.active.size(); ++i) {
            auto& row = next->gameplayCatalog.active[i];
            const auto owner = std::find_if(next->owners.begin(), next->owners.end(),
                [&](const auto& candidate) { return candidate.packStableId == row.ownerPackStableId; });
            if (sameCatalog) {
                row.providerClosureFingerprint = published_->gameplayCatalog.active[i].providerClosureFingerprint;
                // A validated live owner may adopt new content while the immutable Init stays installed.
                const auto prior = std::find_if(published_->owners.begin(), published_->owners.end(),
                    [&](const auto& candidate) { return candidate.packStableId == row.ownerPackStableId; });
                if (owner != next->owners.end() && prior != published_->owners.end()
                    && prior->providerClosureFingerprint == row.providerClosureFingerprint)
                    row.providerClosureFingerprint = owner->providerClosureFingerprint;
            }
            if (owner != next->owners.end()
                && owner->providerClosureFingerprint != row.providerClosureFingerprint)
                return VN_PACK_STALE_REVISION;
        }
    }
    const VnResult overlayPolicy = ValidateInitVariablePolicy(*next);
    if (overlayPolicy != VN_OK) {
        return overlayPolicy;
    }
    const VnResult requirements = ValidateVariableRequirements(*next);
    if (requirements != VN_OK) {
        return requirements;
    }
    const VnResult indexed = BuildIndex(*next, &prepared.facts_);
    if (indexed != VN_OK) {
        facts_.conflictingStableId = prepared.facts_.conflictingStableId;
        facts_.conflictingProjectId = next->projectId;
        facts_.conflictingPackA = prepared.facts_.conflictingPackA;
        facts_.conflictingPackB = prepared.facts_.conflictingPackB;
        return indexed;
    }
    std::uint64_t loadedActions = 0;
    VnObjectId lastActionId{};
    for (const auto& entry : next->index) {
        if (entry.identityClass == kInitCatalogIdentityClassAction &&
            entry.stableId != lastActionId) {
            ++loadedActions;
            lastActionId = entry.stableId;
        }
    }
    if (loadedActions > maxActions) {
        NoteRefusal(loadedActions, maxActions, VN_PACK_RESIDENT_CAPACITY);
        return VN_PACK_RESIDENT_CAPACITY;
    }
    const VnResult companions = ValidateSameOwnerCompanions(*next, maxPacks_);
    if (companions != VN_OK) {
        return companions;
    }
    const VnResult cross = ValidateCrossOwnerReferences(*next, maxPacks_);
    if (cross != VN_OK) {
        return cross;
    }
    std::uint64_t worldSemanticFingerprint = 0;
    const VnResult composite =
        ValidateCompositeWorld(*next, worldSemanticFingerprint);
    if (composite != VN_OK) {
        return composite;
    }
    const VnResult playbackStore =
        BuildPlaybackOwnerStore(*next, maxPacks_, worldSemanticFingerprint,
                                effectiveBindings);
    if (playbackStore != VN_OK) {
        return playbackStore;
    }
    prepared.facts_.compositeGeneration = next->compositeGeneration;
    prepared.facts_.activationFrame = serviceFrame;
    prepared.owner_ = this;
    prepared.published_ = std::move(next);
    *out = std::move(prepared);
    return VN_OK;
}

void PackComposition::CommitPrepared(PreparedCommit* prepared) noexcept {
    assert(prepared != nullptr && prepared->owner_ == this &&
           prepared->published_ != nullptr);
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->published_ == nullptr) {
        return;
    }
    published_ = std::move(prepared->published_);
    authority_ = PackAuthorityMode::Pack;
    facts_ = prepared->facts_;
    prepared->owner_ = nullptr;
}

void PackComposition::RestorePublished(
    std::shared_ptr<const PublishedView> previous) noexcept {
    published_ = std::move(previous);
    facts_.compositeGeneration =
        published_ != nullptr ? published_->compositeGeneration : 0;
}

std::size_t PackComposition::LeaseIndex(const LeaseRow& row) const noexcept {
    if (leases_.empty()) {
        return 0;
    }
    const LeaseRow* data = leases_.data();
    if (&row < data || &row >= data + leases_.size()) {
        return leases_.size();
    }
    return static_cast<std::size_t>(&row - data);
}

PackFrozenClosure& PackComposition::FrozenClosure(LeaseRow& row) const noexcept {
    return frozenClosures_[LeaseIndex(row)];
}

const PackFrozenClosure& PackComposition::FrozenClosure(
    const LeaseRow& row) const noexcept {
    return frozenClosures_[LeaseIndex(row)];
}

const ContainerWorldInstallViewV1* PackComposition::DefinitionView(
    const LeaseRow& row) const noexcept {
    const auto& closure = FrozenClosure(row);
    if (closure.ownerStore == nullptr ||
        closure.rootOwnerIndex >= closure.ownerStore->owners.size()) return nullptr;
    const auto& snapshot = closure.ownerStore->owners[closure.rootOwnerIndex].snapshot;
    return snapshot.definitionBody != nullptr ? &snapshot.definitionView : nullptr;
}

std::size_t PackComposition::SliceCapacity(PackLeaseSlice slice) const noexcept {
    return slice == PackLeaseSlice::Playable ? playableCapacity_
                                             : sharedCapacity_;
}

std::size_t& PackComposition::SliceOccupied(PackLeaseSlice slice) const noexcept {
    return slice == PackLeaseSlice::Playable ? playableOccupied_
                                             : sharedOccupied_;
}

void PackComposition::ClearLeaseRow(LeaseRow& row) const noexcept {
    const auto index = LeaseIndex(row);
    if (index < originalLeaseRecords_.size() && originalLeaseRecords_[index].active) {
        FrozenClosure(row) = PackFrozenClosure{};
        row = LeaseRow{};
        return;
    }
    if (row.state == LeaseState::Free) {
        row = LeaseRow{};
        return;
    }
    --SliceOccupied(row.tag);
    FrozenClosure(row) = PackFrozenClosure{};
    row = LeaseRow{};
}

VnResult PackComposition::SnapshotFrozenClosure(
    const PublishedOwner& root, LeaseRow& row) const noexcept {
    FrozenClosure(row) = PackFrozenClosure{};
    if (published_ == nullptr) {
        return VN_CONTENT_NOT_LOADED;
    }
    if (maxPacks_ == 0 || published_->playbackStore == nullptr) {
        return VN_BACKPRESSURE;
    }
    std::size_t rootIndex = published_->owners.size();
    for (std::size_t index = 0; index < published_->owners.size(); ++index) {
        if (&published_->owners[index] == &root) {
            rootIndex = index;
            break;
        }
    }
    if (rootIndex > std::numeric_limits<std::uint32_t>::max() ||
        rootIndex >= published_->owners.size() ||
        rootIndex >= published_->playbackStore->owners.size()) {
        return VN_STALE_HANDLE;
    }
    PackFrozenClosure& closure = FrozenClosure(row);
    closure.compositeGeneration = published_->compositeGeneration;
    closure.rootPackStableId = root.packStableId;
    closure.ownerStore = published_->playbackStore;
    closure.rootOwnerIndex = static_cast<std::uint32_t>(rootIndex);
    closure.ownerCount = static_cast<std::uint32_t>(
        published_->playbackStore->owners[rootIndex].closureOwnerSlots.size());
    return VN_OK;
}

VnResult PackComposition::PrepareLease(const PublishedOwner& owner,
                                       PackLeaseSlice slice,
                                       PackPartitionLease* out,
                                       OriginalLeaseEpisode* original) const noexcept {
    if (original != nullptr && (original->owner_ != nullptr || originalLeasesQuiesced_ ||
        originalLeaseIncarnation_ == 0)) return VN_IN_USE;
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = {};
    if (leaseTokensExhausted_) {
        return VN_RUNTIME_RECREATE_REQUIRED;
    }
    if (SliceOccupied(slice) >= SliceCapacity(slice)) {
        return VN_BACKPRESSURE;
    }
    std::size_t availableIndex = leases_.size();
    for (std::size_t i = 0; i < leases_.size(); ++i) {
        if (leases_[i].state == LeaseState::Free && !originalLeaseRecords_[i].active) {
            availableIndex = i;
            break;
        }
    }
    if (availableIndex == leases_.size()) {
        return VN_BACKPRESSURE;
    }
    LeaseRow& available = leases_[availableIndex];
    const VnResult snapshotted = SnapshotFrozenClosure(owner, available);
    if (snapshotted != VN_OK) {
        FrozenClosure(available) = PackFrozenClosure{};
        return snapshotted;
    }

    PackPartitionLease lease;
    lease.handle = owner.handle;
    lease.packStableId = owner.packStableId;
    lease.contentRevision = owner.contentRevision;
    lease.partitionRevision = owner.partitionRevision;
    lease.resourceRevision = owner.resourceRevision;
    lease.resourceToken = owner.resourceToken;
    lease.compositeGeneration =
        published_ != nullptr ? published_->compositeGeneration : 0;
    lease.slotGeneration = static_cast<std::uint32_t>(owner.handle >> 32);
    lease.leaseToken = nextLeaseToken_;
    if (nextLeaseToken_ == std::numeric_limits<std::uint64_t>::max()) {
        leaseTokensExhausted_ = true;
    } else {
        ++nextLeaseToken_;
    }
    available.state = LeaseState::Prepared;
    available.tag = slice;
    available.lease = lease;
    available.refs = 0;
    ++SliceOccupied(slice);
    if (original != nullptr) {
        auto& record = originalLeaseRecords_[availableIndex];
        record = OriginalLeaseRecord{};
        record.active = true; record.phase = OriginalLeasePhase::Preparing;
        record.tag = slice; record.lease = lease; record.handle = original;
        original->owner_ = this; original->index_ = availableIndex;
        original->token_ = lease.leaseToken; original->incarnation_ = originalLeaseIncarnation_;
        original->lastPhase_ = OriginalLeasePhase::Preparing;
    }
    *out = lease;
    return VN_OK;
}

bool PackComposition::SameIdentity(const IdentityIndexEntry& a,
                                   const IdentityIndexEntry& b) noexcept {
    return a.stableId == b.stableId && a.identityClass == b.identityClass &&
           a.packSlot == b.packSlot && a.localRow == b.localRow;
}

bool PackComposition::SameLeaseIdentity(const PackPartitionLease& a,
                                        const PackPartitionLease& b) noexcept {
    return a.leaseToken == b.leaseToken && a.handle == b.handle &&
           a.packStableId == b.packStableId &&
           a.slotGeneration == b.slotGeneration &&
           a.contentRevision == b.contentRevision &&
           a.partitionRevision == b.partitionRevision &&
           a.resourceRevision == b.resourceRevision &&
           a.resourceToken == b.resourceToken;
}

bool PackComposition::SameLease(const PackPartitionLease& a,
                                const PackPartitionLease& b) noexcept {
    return SameLeaseIdentity(a, b) &&
           a.resourceClosureToken == b.resourceClosureToken;
}

VnResult PackComposition::CommitPreparedLease(
    PackPartitionLease* lease,
    PackResourceClosureToken resourceClosureToken,
    const OriginalLeaseEpisode* original) const noexcept {
    if (original != nullptr && !MatchesOriginalLease(*original)) return VN_STALE_HANDLE;
    if (lease == nullptr) {
        return VN_INVALID_ARG;
    }
    for (LeaseRow& row : leases_) {
        if (row.state != LeaseState::Prepared ||
            !SameLeaseIdentity(row.lease, *lease)) {
            continue;
        }
        auto& episode = originalLeaseRecords_[LeaseIndex(row)];
        if (episode.active && (original == nullptr || episode.handle != original ||
            !SameFullOriginalLease(episode.lease, *lease) || episode.phase != OriginalLeasePhase::Preparing ||
            episode.resourceReceiver == nullptr || episode.acquiredResource != resourceClosureToken)) return VN_IN_USE;
        if (!episode.active && original != nullptr) return VN_STALE_HANDLE;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
        if (episode.active && std::exchange(refuseOriginalLeaseCommitOnceForTest, false)) return VN_BACKPRESSURE;
#endif
        row.lease.resourceClosureToken = resourceClosureToken;
        row.refs = 1;
        row.state = LeaseState::Live;
        lease->resourceClosureToken = resourceClosureToken;
        if (episode.active) { episode.lease = *lease; episode.phase = OriginalLeasePhase::Live; }
        return VN_OK;
    }
    return VN_STALE_HANDLE;
}

void PackComposition::AbandonPreparedLease(
    const PackPartitionLease& lease) const noexcept {
    for (LeaseRow& row : leases_) {
        if (row.state == LeaseState::Prepared &&
            SameLeaseIdentity(row.lease, lease)) {
            if (originalLeaseRecords_[LeaseIndex(row)].active) return;
            ClearLeaseRow(row);
            return;
        }
    }
}

VnResult PackComposition::ValidateLease(
    const PackPartitionLease& lease) const noexcept {
    for (const LeaseRow& row : leases_) {
        if (row.state == LeaseState::Live && SameLease(row.lease, lease)) {
            return row.refs != 0 ? VN_OK : VN_STALE_HANDLE;
        }
    }
    return VN_STALE_HANDLE;
}

VnResult PackComposition::ReleaseLease(const PackPartitionLease& lease) noexcept {
    for (LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0 ||
            !SameLease(row.lease, lease)) {
            continue;
        }
        if (originalLeaseRecords_[LeaseIndex(row)].active) return VN_IN_USE;
        --row.refs;
        if (row.refs == 0) {
            ClearLeaseRow(row);
        }
        return VN_OK;
    }
    return VN_STALE_HANDLE;
}

VnResult PackComposition::RetagLiveLease(const PackPartitionLease& lease,
                                         PackLeaseSlice to,
                                         const OriginalLeaseEpisode* original) noexcept {
    if (to != PackLeaseSlice::Playable) {
        return VN_INVALID_ARG;
    }
    for (LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || !SameLease(row.lease, lease)) {
            continue;
        }
        if (row.refs == 0) {
            return VN_STALE_HANDLE;
        }
        if (row.tag != PackLeaseSlice::SharedActionAndDeferred) {
            return VN_INVALID_ARG;
        }
        auto& episode = originalLeaseRecords_[LeaseIndex(row)];
        if (episode.active && (original == nullptr || !MatchesOriginalLease(*original) ||
            episode.handle != original || !SameFullOriginalLease(episode.lease, lease) ||
            episode.phase != OriginalLeasePhase::Live)) return VN_IN_USE;
        if (!episode.active && original != nullptr) return VN_STALE_HANDLE;
        if (SliceOccupied(PackLeaseSlice::Playable) >=
            SliceCapacity(PackLeaseSlice::Playable)) {
            ++retagBackpressureCountForTests_;
            return VN_BACKPRESSURE;
        }
        --SliceOccupied(row.tag);
        ++SliceOccupied(PackLeaseSlice::Playable);
        row.tag = PackLeaseSlice::Playable;
        if (episode.active) episode.tag = row.tag;
        return VN_OK;
    }
    return VN_STALE_HANDLE;
}

VnResult PackComposition::SetNextLeaseTokenForTests(
    std::uint64_t nextToken) noexcept {
    if (nextToken == 0 || nextToken < nextLeaseToken_ ||
        leaseTokensExhausted_) {
        return VN_INVALID_ARG;
    }
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Free || originalLeaseRecords_[LeaseIndex(row)].active) {
            return VN_IN_USE;
        }
    }
    nextLeaseToken_ = nextToken;
    return VN_OK;
}

const ActionProgramViewV1* PackComposition::FindActionAtLease(
    const PackPartitionLease& lease, VnObjectId actionId) const noexcept {
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0 ||
            !SameLease(row.lease, lease)) {
            continue;
        }
        const auto* definition = DefinitionView(row);
        if (definition == nullptr || definition->actions == nullptr) continue;
        for (std::uint32_t i = 0; i < definition->actionCount; ++i) {
            const ActionProgramViewV1& action = definition->actions[i];
            if (action.actionStableId == actionId) {
                return &action;
            }
        }
        return nullptr;
    }
    return nullptr;
}

const ContainerProgramViewV3* PackComposition::FindContainerAtLease(
    const PackPartitionLease& lease,
    VnObjectId containerStableId) const noexcept {
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0 ||
            !SameLease(row.lease, lease)) {
            continue;
        }
        const auto* definition = DefinitionView(row);
        if (definition == nullptr || definition->containers == nullptr) continue;
        for (std::uint32_t i = 0; i < definition->containerCount; ++i) {
            const ContainerProgramViewV3& container =
                definition->containers[i];
            if (container.containerStableId == containerStableId) {
                return &container;
            }
        }
        return nullptr;
    }
    return nullptr;
}

bool PackComposition::PlayableWorldAtLease(
    const PackPartitionLease& lease, PlayableWorldViewV1* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0 ||
            !SameLease(row.lease, lease)) {
            continue;
        }
        const auto* definition = DefinitionView(row);
        if (definition == nullptr) continue;
        out->generation = lease.compositeGeneration;
        out->identity = definition->identity;
        out->properties = definition->playableProperties;
        out->propertyCount = definition->playablePropertyCount;
        out->occurrences = definition->playableOccurrences;
        out->occurrenceCount = definition->playableOccurrenceCount;
        out->trackSchedules = definition->trackSchedules;
        out->trackScheduleCount = definition->trackScheduleCount;
        out->pieceSchedules = definition->pieceSchedules;
        out->pieceScheduleCount = definition->pieceScheduleCount;
        out->maxKnownScheduleLeaves =
            definition->capacity.maxKnownScheduleLeaves;
        out->maxTrackPieceScheduleDepth =
            definition->capacity.maxTrackPieceScheduleDepth;
        out->containers = definition->containers;
        out->containerCount = definition->containerCount;
        out->transitions = definition->transitions;
        out->transitionCount = definition->transitionCount;
        return true;
    }
    return false;
}

VnResult PackComposition::PrepareLookup(
    VnObjectId stableId, PackIdentityLookup* out) const noexcept {
    return PrepareLookup(stableId, PackLeaseSlice::SharedActionAndDeferred, out);
}

VnResult PackComposition::PrepareLookup(
    VnObjectId stableId, PackLeaseSlice slice,
    PackIdentityLookup* out, OriginalLeaseEpisode* original) const noexcept {
    if (original != nullptr && original->owner_ != nullptr) return VN_IN_USE;
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = {};
    if (published_ != nullptr) {
        const IdentityIndexEntry* hit = BinaryFind(published_->index, stableId);
        if (hit != nullptr && hit->packSlot < published_->owners.size()) {
            const PublishedOwner& owner = published_->owners[hit->packSlot];
            if (owner.readiness == PackOwnerReadiness::Active) {
                out->readiness = PackIdentityReadiness::Active;
                out->result = PrepareLease(owner, slice, &out->lease, original);
                if (out->result != VN_OK) {
                    return out->result;
                }
                if (hit->identityClass == kInitCatalogIdentityClassAction &&
                    hit->localRow < owner.view.actionCount) {
                    out->action = &owner.view.actions[hit->localRow];
                } else if (hit->identityClass ==
                               kInitCatalogIdentityClassPlayable &&
                           hit->localRow < owner.view.playablePropertyCount) {
                    out->playableRow =
                        &owner.view.playableProperties[hit->localRow];
                }
                return VN_OK;
            }
        }
        if (published_->hasCatalog) {
            const PackInitCatalogRow* row = nullptr;
            if (LookupInitCatalogIdentity(published_->catalog, stableId, &row) ==
                PackCatalogIdentityState::KnownUnloaded) {
                out->readiness = PackIdentityReadiness::KnownUnloaded;
                out->result = VN_CONTENT_NOT_LOADED;
                return out->result;
            }
        }
    }
    out->readiness = PackIdentityReadiness::Absent;
    out->result = VN_UNKNOWN_ID;
    return out->result;
}

VnResult PackComposition::BuildResourceClosure(
    const PackPartitionLease& lease,
    PackResourceRevisionToken* outTokens, std::size_t tokenCapacity,
    std::size_t* outTokenCount) const noexcept {
    return BuildPublishedResourceClosure(published_.get(), lease, outTokens, tokenCapacity, outTokenCount);
}

VnResult PackComposition::PreparedResourceClosureBacking(const PackPartitionLease& lease,
    std::shared_ptr<const std::vector<PackResourceRevisionToken>>* out) const noexcept {
    if (out == nullptr) return VN_INVALID_ARG;
    out->reset();
    for (const auto& row : leases_) {
        if (row.state != LeaseState::Prepared || !SameLeaseIdentity(row.lease, lease)) continue;
        const auto& closure = FrozenClosure(row);
        if (closure.ownerStore == nullptr || closure.rootOwnerIndex >= closure.ownerStore->owners.size() ||
            closure.compositeGeneration != lease.compositeGeneration) return VN_CONTENT_NOT_LOADED;
        const auto& owner = closure.ownerStore->owners[closure.rootOwnerIndex];
        if (owner.snapshot.handle != lease.handle || owner.snapshot.packStableId != lease.packStableId ||
            owner.snapshot.resourceToken != lease.resourceToken || owner.resourceTokens == nullptr ||
            owner.resourceTokens->empty()) return VN_CONTENT_NOT_LOADED;
        *out = owner.resourceTokens;
        return VN_OK;
    }
    return VN_STALE_HANDLE;
}

PackIdentityLookup PackComposition::Lookup(VnObjectId stableId) const noexcept {
    PackIdentityLookup result;
    const VnResult prepared = PrepareLookup(stableId, &result);
    if (prepared != VN_OK) {
        return result;
    }
    result.result = CommitPreparedLease(&result.lease, 0);
    if (result.result != VN_OK) {
        AbandonPreparedLease(result.lease);
        result.action = nullptr;
        result.playableRow = nullptr;
    }
    return result;
}

const ActionProgramViewV1* PackComposition::FindActionIn(
    const PublishedView& view, VnObjectId actionId) const noexcept {
    const IdentityIndexEntry* hit = BinaryFind(view.index, actionId);
    if (hit == nullptr || hit->packSlot >= view.owners.size() ||
        hit->identityClass != kInitCatalogIdentityClassAction) {
        return nullptr;
    }
    const PublishedOwner& owner = view.owners[hit->packSlot];
    if (owner.readiness != PackOwnerReadiness::Active ||
        hit->localRow >= owner.view.actionCount ||
        owner.view.actions == nullptr) {
        return nullptr;
    }
    return &owner.view.actions[hit->localRow];
}

const ActionProgramViewV1* PackComposition::FindAction(
    VnObjectId actionId) const noexcept {
    if (published_ == nullptr) {
        return nullptr;
    }
    return FindActionIn(*published_, actionId);
}

const ContainerProgramViewV3* PackComposition::FindContainer(
    VnObjectId containerStableId) const noexcept {
    if (published_ == nullptr) {
        return nullptr;
    }
    const IdentityIndexEntry* hit =
        BinaryFind(published_->index, containerStableId);
    if (hit == nullptr ||
        hit->identityClass != kInitCatalogIdentityClassPlayable ||
        hit->packSlot >= published_->owners.size()) {
        return nullptr;
    }
    const PublishedOwner& owner = published_->owners[hit->packSlot];
    if (owner.readiness != PackOwnerReadiness::Active ||
        owner.view.playableProperties == nullptr ||
        hit->localRow >= owner.view.playablePropertyCount) {
        return nullptr;
    }
    const PlayablePropertyProgramViewV1& property =
        owner.view.playableProperties[hit->localRow];
    if (property.contentPropertyKind !=
            playableproperties::ContentPropertyKindV1::Container ||
        property.stableId != containerStableId) {
        return nullptr;
    }
    return FindContainerProgram(owner.view, containerStableId);
}

bool PackComposition::RequiredConsumerExists(std::uint64_t providerStableId,
                                             std::uint64_t* outConsumer) const
    noexcept {
    if (published_ == nullptr || providerStableId == 0) {
        return false;
    }
    for (const PublishedOwner& owner : published_->owners) {
        if (owner.readiness != PackOwnerReadiness::Active) {
            continue;
        }
        for (const PackDependencyEdge& edge : owner.dependencyEdges) {
            if (edge.prerequisitePackStableId == providerStableId) {
                if (outConsumer != nullptr) {
                    *outConsumer = owner.packStableId;
                }
                return true;
            }
        }
    }
    return false;
}

bool PackComposition::HasLiveLease(std::uint64_t packStableId,
                                   std::uint64_t partitionRevision,
                                   std::uint64_t resourceRevision) const
    noexcept {
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0) {
            continue;
        }
        if (row.lease.packStableId == packStableId &&
            row.lease.partitionRevision == partitionRevision &&
            row.lease.resourceRevision == resourceRevision) {
            return true;
        }
        const PackFrozenClosure& closure = FrozenClosure(row);
        if (closure.ownerStore == nullptr ||
            closure.rootOwnerIndex >= closure.ownerStore->owners.size()) {
            continue;
        }
        const auto& rootOwner =
            closure.ownerStore->owners[closure.rootOwnerIndex];
        const std::uint32_t ownerCount = closure.ownerCount;
        for (std::uint32_t i = 0; i < ownerCount; ++i) {
            if (i >= rootOwner.closureOwnerSlots.size()) {
                break;
            }
            const auto ownerSlot = rootOwner.closureOwnerSlots[i];
            if (ownerSlot >= closure.ownerStore->owners.size()) {
                continue;
            }
            const auto& owner = closure.ownerStore->owners[ownerSlot].snapshot;
            if (owner.packStableId == packStableId &&
                owner.partitionRevision == partitionRevision &&
                owner.resourceRevision == resourceRevision) {
                return true;
            }
        }
    }
    return false;
}

bool PackComposition::HasLiveAttenuationGeneration(
    VnAttenuationId id, std::uint64_t generation) const noexcept {
    if (id == VnObjectId{} || generation == 0) return false;
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0) continue;
        const auto& closure = FrozenClosure(row);
        if (closure.ownerStore != nullptr &&
            closure.ownerStore->attenuationBindings.GenerationFor(id) == generation) {
            return true;
        }
    }
    return false;
}

bool PackComposition::HasLiveResourceToken(
    PackResourceRevisionToken resourceToken) const noexcept {
    if (resourceToken == 0) {
        return false;
    }
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0) {
            continue;
        }
        if (row.lease.resourceToken == resourceToken) {
            return true;
        }
        const PackFrozenClosure& closure = FrozenClosure(row);
        if (closure.ownerStore == nullptr ||
            closure.rootOwnerIndex >= closure.ownerStore->owners.size()) {
            continue;
        }
        const auto& rootOwner =
            closure.ownerStore->owners[closure.rootOwnerIndex];
        const std::uint32_t ownerCount = closure.ownerCount;
        for (std::uint32_t i = 0; i < ownerCount; ++i) {
            if (i >= rootOwner.closureOwnerSlots.size()) {
                break;
            }
            const auto ownerSlot = rootOwner.closureOwnerSlots[i];
            if (ownerSlot >= closure.ownerStore->owners.size()) {
                continue;
            }
            if (closure.ownerStore->owners[ownerSlot]
                    .snapshot.resourceToken == resourceToken) {
                return true;
            }
        }
    }
    return false;
}

void PackComposition::NoteRefusal(std::uint64_t required,
                                  std::uint64_t configured,
                                  VnResult result) noexcept {
    facts_.lastRefusalRequired = required;
    facts_.lastRefusalConfigured = configured;
    facts_.lastRefusalResult = result;
}

bool PackComposition::PlaybackViewAtLease(const PackPartitionLease& lease,
                                          PackPlaybackView* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = PackPlaybackView{};
    for (const LeaseRow& row : leases_) {
        if (row.state != LeaseState::Live || row.refs == 0 ||
            !SameLease(row.lease, lease)) {
            continue;
        }
        out->closure_ = &FrozenClosure(row);
        out->ownerStore_ = FrozenClosure(row).ownerStore.get();
        return FrozenClosure(row).ownerCount == 0 ||
               (out->ownerStore_ != nullptr &&
                FrozenClosure(row).rootOwnerIndex <
                    out->ownerStore_->owners.size());
    }
    return false;
}

VnObjectId PackComposition::ProjectIdAtLease(const PackPartitionLease& lease) const noexcept {
    PackPlaybackView view;
    return PlaybackViewAtLease(lease, &view) ? view.ProjectId() : VnObjectId{};
}

std::uint64_t PackPlaybackView::AttenuationGeneration(VnAttenuationId id) const noexcept {
    return ownerStore_ != nullptr
        ? ownerStore_->attenuationBindings.GenerationFor(id) : 0;
}

VnObjectId PackPlaybackView::ProjectId() const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr ||
        closure_->rootOwnerIndex >= ownerStore_->owners.size()) return {};
    return ownerStore_->owners[closure_->rootOwnerIndex].snapshot.definitionView.identity.projectId;
}

std::uint64_t PackPlaybackView::CompositeGeneration() const noexcept {
    return closure_ != nullptr ? closure_->compositeGeneration : 0;
}

std::uint64_t PackPlaybackView::WorldSemanticFingerprint() const noexcept {
    return ownerStore_ != nullptr ? ownerStore_->worldSemanticFingerprint : 0;
}

std::uint64_t PackPlaybackView::RootPackStableId() const noexcept {
    return closure_ != nullptr ? closure_->rootPackStableId : 0;
}

std::uint32_t PackPlaybackView::OwnerCount() const noexcept {
    return closure_ != nullptr ? closure_->ownerCount : 0;
}

const PackPlaybackOwnerStore::Owner* PackPlaybackView::OwnerAt(
    std::uint32_t closureIndex) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr ||
        closure_->rootOwnerIndex >= ownerStore_->owners.size()) {
        return nullptr;
    }
    const auto& root = ownerStore_->owners[closure_->rootOwnerIndex];
    if (closureIndex >= root.closureOwnerSlots.size()) {
        return nullptr;
    }
    const std::uint32_t ownerSlot = root.closureOwnerSlots[closureIndex];
    if (ownerSlot >= ownerStore_->owners.size()) {
        return nullptr;
    }
    return &ownerStore_->owners[ownerSlot];
}

namespace {

bool FingerprintMatches(std::uint64_t expected, std::uint64_t actual) noexcept {
    return expected == 0 || expected == actual;
}

const TransitionProgramViewV1* FindTransitionProgram(
    const ContainerWorldInstallViewV1& view, VnObjectId ownerId) noexcept {
    if (!PointerCountOk(view.transitionCount, view.transitions) ||
        view.transitionCount == 0) {
        return nullptr;
    }
    const auto* begin = view.transitions;
    const auto* end = begin + view.transitionCount;
    const auto* found = std::lower_bound(
        begin, end, ownerId,
        [](const TransitionProgramViewV1& row, VnObjectId id) {
            return row.ownerContainerStableId < id;
        });
    if (found == end || found->ownerContainerStableId != ownerId) {
        return nullptr;
    }
    return found;
}

const PlayableOccurrencePropertyViewV1* FindOccurrenceKey(
    const ContainerWorldInstallViewV1& view,
    playableproperties::ContentPropertyKindV1 parentKind,
    VnObjectId parentStableId, std::uint32_t ordinal) noexcept {
    if (!PointerCountOk(view.playableOccurrenceCount, view.playableOccurrences) ||
        view.playableOccurrenceCount == 0) {
        return nullptr;
    }
    const auto key = std::tuple(static_cast<std::uint32_t>(parentKind),
                                parentStableId, ordinal);
    const auto* begin = view.playableOccurrences;
    const auto* end = begin + view.playableOccurrenceCount;
    const auto* found = std::lower_bound(
        begin, end, key,
        [](const PlayableOccurrencePropertyViewV1& row, const auto& candidate) {
            return std::tuple(static_cast<std::uint32_t>(row.parentKind),
                              row.parentStableId,
                              row.stableOccurrenceOrdinal) < candidate;
        });
    if (found == end || found->parentKind != parentKind ||
        found->parentStableId != parentStableId ||
        found->stableOccurrenceOrdinal != ordinal) {
        return nullptr;
    }
    return found;
}

} // namespace

const PlayablePropertyProgramViewV1* PackPlaybackView::ResolveProperty(
    playableproperties::ContentPropertyKindV1 kind,
    VnObjectId stableId) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return nullptr;
        }
        const PlayablePropertyProgramViewV1* property =
            FindPlayableProperty(owner->snapshot.definitionView, kind, stableId);
        if (property != nullptr) {
            return property;
        }
    }
    return nullptr;
}

const ContainerProgramViewV3* PackPlaybackView::ResolveContainer(
    VnObjectId stableId, std::uint64_t expectedFingerprint) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return nullptr;
        }
        const PlayablePropertyProgramViewV1* property = FindPlayableProperty(
            owner->snapshot.definitionView,
            playableproperties::ContentPropertyKindV1::Container, stableId);
        if (property == nullptr) {
            continue;
        }
        const ContainerProgramViewV3* program =
            FindContainerProgram(owner->snapshot.definitionView, stableId);
        if (program != nullptr &&
            FingerprintMatches(expectedFingerprint,
                               program->programSemanticFingerprint)) {
            return program;
        }
        return nullptr;
    }
    return nullptr;
}

const TrackScheduleProgramViewV1* PackPlaybackView::ResolveTrack(
    VnObjectId stableId,
    std::uint64_t expectedScheduleFingerprint) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return nullptr;
        }
        const TrackScheduleProgramViewV1* schedule =
            FindTrackSchedule(owner->snapshot.definitionView, stableId);
        if (schedule != nullptr &&
            FingerprintMatches(expectedScheduleFingerprint,
                               schedule->scheduleSemanticFingerprint)) {
            return schedule;
        }
    }
    return nullptr;
}

const PieceScheduleProgramViewV1* PackPlaybackView::ResolvePiece(
    VnObjectId stableId,
    std::uint64_t expectedScheduleFingerprint) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return nullptr;
        }
        const PieceScheduleProgramViewV1* schedule =
            FindPieceSchedule(owner->snapshot.definitionView, stableId);
        if (schedule != nullptr &&
            FingerprintMatches(expectedScheduleFingerprint,
                               schedule->scheduleSemanticFingerprint)) {
            return schedule;
        }
    }
    return nullptr;
}

const PlayableOccurrencePropertyViewV1* PackPlaybackView::ResolveOccurrence(
    playableproperties::ContentPropertyKindV1 parentKind,
    VnObjectId parentStableId, std::uint32_t ordinal) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return nullptr;
        }
        const PlayableOccurrencePropertyViewV1* occurrence = FindOccurrenceKey(
            owner->snapshot.definitionView, parentKind, parentStableId, ordinal);
        if (occurrence != nullptr) {
            return occurrence;
        }
    }
    return nullptr;
}

const TransitionProgramViewV1* PackPlaybackView::ResolveTransition(
    VnObjectId ownerContainerStableId) const noexcept {
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return nullptr;
        }
        const TransitionProgramViewV1* transition = FindTransitionProgram(
            owner->snapshot.definitionView, ownerContainerStableId);
        if (transition != nullptr) {
            return transition;
        }
    }
    return nullptr;
}

bool PackPlaybackView::ResolveSoundResource(
    VnObjectId stableId, std::uint64_t expectedRevision,
    const PackClosureOwnerSnapshot** outOwner) const noexcept {
    if (outOwner != nullptr) {
        *outOwner = nullptr;
    }
    if (closure_ == nullptr || ownerStore_ == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < closure_->ownerCount; ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr) {
            return false;
        }
        const PlayablePropertyProgramViewV1* property = FindPlayableProperty(
            owner->snapshot.definitionView,
            playableproperties::ContentPropertyKindV1::Sound, stableId);
        if (property == nullptr) {
            continue;
        }
        if (!FingerprintMatches(expectedRevision,
                                 owner->snapshot.resourceRevision)) {
            return false;
        }
        if (outOwner != nullptr) {
            *outOwner = &owner->snapshot;
        }
        return true;
    }
    return false;
}

std::uint32_t PackPlaybackView::ResolveVoiceBindings(
    VnObjectId objectId, const PackVoiceBinding** out,
    std::uint32_t capacity) const noexcept {
    for (std::uint32_t i = 0; i < OwnerCount(); ++i) {
        const auto* owner = OwnerAt(i);
        if (owner == nullptr || owner->snapshot.voiceBindings == nullptr) {
            continue;
        }
        std::uint32_t count = 0;
        for (const PackVoiceBinding& binding : *owner->snapshot.voiceBindings) {
            if (binding.objectId == objectId) {
                if (count < capacity) out[count] = &binding;
                ++count;
            }
        }
        if (count != 0) {
            return count;
        }
    }
    return 0;
}

namespace {

using VoiceBindingLookup = std::uint32_t (*)(
    const void*, VnObjectId, const PackVoiceBinding**, std::uint32_t);

std::uint32_t LookupCatalogVoiceBindings(
    const void* ctx, VnObjectId objectId, const PackVoiceBinding** out,
    std::uint32_t capacity) noexcept {
    const auto* catalog = static_cast<const std::pair<const PackVoiceBinding*,
                                                      std::uint32_t>*>(ctx);
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < catalog->second; ++i) {
        if (catalog->first[i].objectId != objectId) {
            continue;
        }
        if (count < capacity) {
            out[count] = &catalog->first[i];
        }
        ++count;
    }
    return count;
}

std::uint32_t LookupViewVoiceBindings(
    const void* ctx, VnObjectId objectId, const PackVoiceBinding** out,
    std::uint32_t capacity) noexcept {
    return static_cast<const PackPlaybackView*>(ctx)->ResolveVoiceBindings(
        objectId, out, capacity);
}

std::uint32_t CollectCutVoiceBindingsWithLookup(
    const ResolvedPlayablePathRowV1* path, std::uint32_t pathCount,
    VoiceBindingLookup lookup, const void* ctx, const PackVoiceBinding** out,
    std::uint32_t capacity) noexcept {
    if (lookup == nullptr || (pathCount != 0 && path == nullptr)) {
        return 0;
    }
    const PackVoiceBinding* chosen[kMaxVoiceBindingInstanceSlots]{};
    std::uint32_t chosenCount = 0;
    bool overflow = false;
    const PackVoiceBinding* found[kMaxVoiceBindingInstanceSlots]{};
    for (std::uint32_t i = 0; i < pathCount; ++i) {
        const std::uint32_t n =
            lookup(ctx, path[i].stableId, found, kMaxVoiceBindingInstanceSlots);
        if (n > kMaxVoiceBindingInstanceSlots) {
            overflow = true;
        }
        if (n == 0) {
            continue;
        }
        const std::uint32_t visible =
            n < kMaxVoiceBindingInstanceSlots ? n : kMaxVoiceBindingInstanceSlots;
        bool hasReal = false;
        for (std::uint32_t j = 0; j < visible; ++j) {
            if (found[j] != nullptr && found[j]->bindingId != VnObjectId{}) {
                hasReal = true;
                break;
            }
        }
        if (!hasReal) {
            chosenCount = 0;
            continue;
        }
        chosenCount = 0;
        for (std::uint32_t j = 0; j < visible; ++j) {
            if (found[j] == nullptr || found[j]->bindingId == VnObjectId{}) {
                continue;
            }
            bool duplicate = false;
            for (std::uint32_t k = 0; k < chosenCount; ++k) {
                duplicate = duplicate || chosen[k]->bindingId == found[j]->bindingId;
            }
            if (duplicate) {
                continue;
            }
            if (chosenCount < kMaxVoiceBindingInstanceSlots) {
                chosen[chosenCount] = found[j];
            }
            ++chosenCount;
        }
        if (chosenCount > kMaxVoiceBindingInstanceSlots) {
            overflow = true;
        }
    }
    const std::uint32_t written =
        out == nullptr ? 0
                       : (chosenCount < capacity ? chosenCount : capacity);
    for (std::uint32_t i = 0; i < written; ++i) {
        out[i] = chosen[i];
    }
    if (overflow && chosenCount <= kMaxVoiceBindingInstanceSlots) {
        return kMaxVoiceBindingInstanceSlots + 1;
    }
    return chosenCount;
}

} // namespace

std::uint32_t CollectCutVoiceBindings(
    const ResolvedPlayablePathRowV1* path, std::uint32_t pathCount,
    const PackVoiceBinding* catalog, std::uint32_t catalogCount,
    const PackVoiceBinding** out, std::uint32_t capacity) noexcept {
    std::pair<const PackVoiceBinding*, std::uint32_t> ctx{catalog, catalogCount};
    return CollectCutVoiceBindingsWithLookup(
        path, pathCount, LookupCatalogVoiceBindings, &ctx, out, capacity);
}

std::uint32_t PackPlaybackView::CollectCutVoiceBindings(
    const ResolvedPlayablePathRowV1* path, std::uint32_t pathCount,
    const PackVoiceBinding** out, std::uint32_t capacity) const noexcept {
    return CollectCutVoiceBindingsWithLookup(
        path, pathCount, LookupViewVoiceBindings, this, out, capacity);
}

} // namespace harken
