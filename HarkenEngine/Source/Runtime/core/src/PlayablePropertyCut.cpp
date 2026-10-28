#include "harken/core/PlayablePropertyCut.h"

#include "harken/core/PackPlaybackView.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <type_traits>
#include <utility>

namespace harken {
namespace {

using PropertyId = playableproperties::PlayablePropertyIdV1;

constexpr std::size_t Slot(PropertyId id) noexcept {
    return static_cast<std::size_t>(id) - 1;
}

constexpr std::uint64_t Bit(PropertyId id) noexcept {
    return UINT64_C(1) << Slot(id);
}

bool Present(const PlayablePropertyPayloadViewV1& payload,
             PropertyId id) noexcept {
    return (payload.presentMask & Bit(id)) != 0;
}

float F32(const PlayablePropertyPayloadViewV1& payload,
          PropertyId id) noexcept {
    return std::bit_cast<float>(
        static_cast<std::uint32_t>(payload.values[Slot(id)].bits));
}

std::uint32_t U32(const PlayablePropertyPayloadViewV1& payload,
                  PropertyId id) noexcept {
    return static_cast<std::uint32_t>(payload.values[Slot(id)].bits);
}

VnObjectId ObjectId(const PlayablePropertyPayloadViewV1& payload,
                  PropertyId id) noexcept {
    return payload.values[Slot(id)].objectId;
}

playableproperties::ContentPropertyKindV1 ContentKind(
    PlayableKindV1 kind) noexcept {
    return static_cast<playableproperties::ContentPropertyKindV1>(
        static_cast<std::uint32_t>(kind) + 1);
}

bool SameRef(const PlayableRefV1& a, const PlayableRefV1& b) noexcept {
    return a.kind == b.kind && a.reserved == b.reserved &&
           a.stableId == b.stableId &&
           a.expectedProgramSemanticFingerprint ==
               b.expectedProgramSemanticFingerprint &&
           a.contentRevision == b.contentRevision &&
           a.routeStableId == b.routeStableId &&
           a.compatibilityFingerprint == b.compatibilityFingerprint;
}

bool Matches(const PlayablePropertyProgramViewV1& row,
             const PlayableRefV1& ref) noexcept {
    return row.contentPropertyKind == ContentKind(ref.kind) &&
           row.stableId == ref.stableId &&
           row.programSemanticFingerprint ==
               ref.expectedProgramSemanticFingerprint &&
           row.contentRevision == ref.contentRevision &&
           row.routeStableId == ref.routeStableId &&
           row.compatibilityFingerprint == ref.compatibilityFingerprint;
}

const PlayablePropertyProgramViewV1* FindProperty(
    const PlayableWorldViewV1& world, const PlayableRefV1& ref) noexcept {
    for (std::uint32_t i = 0; i < world.propertyCount; ++i) {
        if (Matches(world.properties[i], ref)) return &world.properties[i];
    }
    return nullptr;
}

const PlayablePropertyProgramViewV1* FindLayer(
    const PlayableWorldViewV1& world,
    const LayerAncestorRefV1& ref) noexcept {
    for (std::uint32_t i = 0; i < world.propertyCount; ++i) {
        const auto& row = world.properties[i];
        if (row.contentPropertyKind ==
                playableproperties::ContentPropertyKindV1::Layer &&
            row.stableId == ref.layerStableId &&
            row.programSemanticFingerprint ==
                ref.layerProgramSemanticFingerprint) {
            return &row;
        }
    }
    return nullptr;
}

ContainerProgramHandleV1 FindContainer(
    const PlayableWorldViewV1& world,
    const PlayableRefV1& ref) noexcept {
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        const auto& row = world.containers[i];
        if (row.containerStableId == ref.stableId &&
            row.programSemanticFingerprint ==
                ref.expectedProgramSemanticFingerprint) {
            return {ProgramKindV1::Container, ref.stableId, row.programSemanticFingerprint,
                    world.ProgramSource(), world.InitialClosureCapabilities(), &row};
        }
    }
    return {};
}

const ContainerChildProgramViewV2* FindChild(
    const ContainerProgramViewV3& program,
    std::uint32_t ordinal) noexcept {
    for (std::uint32_t i = 0; i < program.childCount; ++i) {
        if (program.children[i].stableChildOrdinal == ordinal) {
            return &program.children[i];
        }
    }
    return nullptr;
}

TrackProgramHandleV1 FindTrackSchedule(
    const PlayableWorldViewV1& world,
    const PlayableRefV1& ref) noexcept {
    for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
        const auto& row = world.trackSchedules[i];
        if (SameRef(row.rootDefinition, ref))
            return {ProgramKindV1::Track, ref.stableId, row.scheduleSemanticFingerprint,
                    world.ProgramSource(), world.InitialClosureCapabilities(), &row};
    }
    return {};
}

SegmentProgramHandleV1 FindSegmentSchedule(
    const PlayableWorldViewV1& world,
    const PlayableRefV1& ref) noexcept {
    for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
        const auto& row = world.segmentSchedules[i];
        if (SameRef(row.rootDefinition, ref))
            return {ProgramKindV1::Segment, ref.stableId, row.scheduleSemanticFingerprint,
                    world.ProgramSource(), world.InitialClosureCapabilities(), &row};
    }
    return {};
}

const ScheduleClipPlacementV1* FindPlacement(
    const TrackScheduleProgramViewV1& track,
    std::uint32_t ordinal) noexcept {
    for (std::uint32_t i = 0; i < track.placementCount; ++i) {
        if (track.placements[i].stableOccurrenceOrdinal == ordinal) {
            return &track.placements[i];
        }
    }
    return nullptr;
}

const PlayableOccurrencePropertyViewV1* FindOccurrence(
    const PlayableWorldViewV1& world,
    const ContainerProgramViewV3& parent,
    const ContainerChildProgramViewV2& child) noexcept {
    for (std::uint32_t i = 0; i < world.occurrenceCount; ++i) {
        const auto& row = world.occurrences[i];
        if (row.parentKind ==
                playableproperties::ContentPropertyKindV1::Container &&
            row.parentStableId == parent.containerStableId &&
            row.parentProgramSemanticFingerprint ==
                parent.programSemanticFingerprint &&
            row.stableOccurrenceOrdinal == child.stableChildOrdinal &&
            SameRef(row.childPlayable, child.playable)) {
            return &row;
        }
    }
    return nullptr;
}

const PlayableOccurrencePropertyViewV1* FindTrackOccurrence(
    const PlayableWorldViewV1& world,
    const TrackScheduleProgramViewV1& parent,
    const ScheduleClipPlacementV1& placement) noexcept {
    for (std::uint32_t i = 0; i < world.occurrenceCount; ++i) {
        const auto& row = world.occurrences[i];
        if (row.parentKind ==
                playableproperties::ContentPropertyKindV1::Track &&
            row.parentStableId == parent.rootDefinition.stableId &&
            row.parentProgramSemanticFingerprint ==
                parent.rootDefinition.expectedProgramSemanticFingerprint &&
            row.stableOccurrenceOrdinal ==
                placement.stableOccurrenceOrdinal &&
            SameRef(row.childPlayable, placement.content)) {
            return &row;
        }
    }
    return nullptr;
}

const PlayablePropertyProgramViewV1* FindProperty(
    const PackPlaybackView& world, const PlayableRefV1& ref) noexcept {
    const PlayablePropertyProgramViewV1* row =
        world.ResolveProperty(ContentKind(ref.kind), ref.stableId);
    return row != nullptr && Matches(*row, ref) ? row : nullptr;
}

const PlayablePropertyProgramViewV1* FindLayer(
    const PackPlaybackView& world, const LayerAncestorRefV1& ref) noexcept {
    const PlayablePropertyProgramViewV1* row = world.ResolveProperty(
        playableproperties::ContentPropertyKindV1::Layer, ref.layerStableId);
    return row != nullptr &&
                   row->programSemanticFingerprint ==
                       ref.layerProgramSemanticFingerprint
               ? row
               : nullptr;
}

ContainerProgramHandleV1 FindContainer(
    const PackPlaybackView& world, const PlayableRefV1& ref) noexcept {
    return world.ResolveContainer(ref.stableId,
                                  ref.expectedProgramSemanticFingerprint);
}

TrackProgramHandleV1 FindTrackSchedule(
    const PackPlaybackView& world, const PlayableRefV1& ref) noexcept {
    const auto row =
        world.ResolveTrack(ref.stableId, 0);
    return row != nullptr && SameRef(row->rootDefinition, ref) ? row : TrackProgramHandleV1{};
}

SegmentProgramHandleV1 FindSegmentSchedule(
    const PackPlaybackView& world, const PlayableRefV1& ref) noexcept {
    const auto row =
        world.ResolveSegment(ref.stableId, 0);
    return row != nullptr && SameRef(row->rootDefinition, ref) ? row : SegmentProgramHandleV1{};
}

const PlayableOccurrencePropertyViewV1* FindOccurrence(
    const PackPlaybackView& world, const ContainerProgramViewV3& parent,
    const ContainerChildProgramViewV2& child) noexcept {
    const PlayableOccurrencePropertyViewV1* row = world.ResolveOccurrence(
        playableproperties::ContentPropertyKindV1::Container,
        parent.containerStableId, child.stableChildOrdinal);
    return row != nullptr &&
                   row->parentProgramSemanticFingerprint ==
                       parent.programSemanticFingerprint &&
                   SameRef(row->childPlayable, child.playable)
               ? row
               : nullptr;
}

const PlayableOccurrencePropertyViewV1* FindTrackOccurrence(
    const PackPlaybackView& world, const TrackScheduleProgramViewV1& parent,
    const ScheduleClipPlacementV1& placement) noexcept {
    const PlayableOccurrencePropertyViewV1* row = world.ResolveOccurrence(
        playableproperties::ContentPropertyKindV1::Track,
        parent.rootDefinition.stableId, placement.stableOccurrenceOrdinal);
    return row != nullptr &&
                   row->parentProgramSemanticFingerprint ==
                       parent.rootDefinition.expectedProgramSemanticFingerprint &&
                   SameRef(row->childPlayable, placement.content)
               ? row
               : nullptr;
}

bool WorldUsable(const PlayableWorldViewV1& world) noexcept {
    return world.generation != 0 &&
           world.identity.runtimeSemanticFingerprint != 0 &&
           (world.propertyCount == 0 || world.properties != nullptr) &&
           (world.occurrenceCount == 0 || world.occurrences != nullptr) &&
           (world.containerCount == 0 || world.containers != nullptr);
}

bool WorldUsable(const PackPlaybackView& world) noexcept {
    return world.CompositeGeneration() != 0 && world.OwnerCount() != 0;
}

void AssignCutWorld(ResolvedPlayablePropertyCutViewV1& view,
                    const PlayableWorldViewV1& world) noexcept {
    view.world = world.generation;
    view.worldSemanticFingerprint = world.identity.runtimeSemanticFingerprint;
}

void AssignCutWorld(ResolvedPlayablePropertyCutViewV1& view,
                    const PackPlaybackView& world) noexcept {
    view.world = world.CompositeGeneration();
    view.worldSemanticFingerprint = world.CompositeGeneration();
}

}

void ResolvedPlayablePropertyCutStorageV1::Buffer::Clear() noexcept {
    view = {};
    path.clear();
    payloads.clear();
    sends.clear();
    effectNodes.clear();
    effectParams.clear();
    effectParamOffsets.clear();
    effectEdges.clear();
}

void ResolvedPlayablePropertyCutStorageV1::Buffer::Refresh() noexcept {
    view.pathCount = static_cast<std::uint32_t>(path.size());
    view.path = path.empty() ? nullptr : path.data();
    view.sendCount = static_cast<std::uint32_t>(sends.size());
    view.sends = sends.empty() ? nullptr : sends.data();
    view.effectNodeCount = static_cast<std::uint32_t>(effectNodes.size());
    view.effectNodes = effectNodes.empty() ? nullptr : effectNodes.data();
    for (std::size_t i = 0; i < effectNodes.size(); ++i) {
        effectNodes[i].params = effectNodes[i].paramCount == 0
                                    ? nullptr
                                    : effectParams.data() +
                                          effectParamOffsets[i];
    }
    view.effectEdgeCount = static_cast<std::uint32_t>(effectEdges.size());
    view.effectEdges = effectEdges.empty() ? nullptr : effectEdges.data();
}

bool ResolvedPlayablePropertyCutStorageV1::Reserve(
    Buffer& buffer,
    const ResolvedPlayablePropertyCutConfigV1& config) {
    buffer.path.reserve(config.maxPathRows);
    buffer.payloads.reserve(config.maxPathRows);
    buffer.sends.reserve(config.maxSends);
    buffer.effectNodes.reserve(config.maxEffectNodes);
    buffer.effectParams.reserve(config.maxEffectParams);
    buffer.effectParamOffsets.reserve(config.maxEffectNodes);
    buffer.effectEdges.reserve(config.maxEffectEdges);
    return true;
}

bool ResolvedPlayablePropertyCutStorageV1::Reset(
    const ResolvedPlayablePropertyCutConfigV1& config) noexcept {
    if (config.maxPathRows == 0 || config.maxSends == 0 ||
        config.maxEffectNodes == 0 || config.maxEffectParams == 0 ||
        config.maxEffectEdges == 0) {
        return false;
    }
    try {
        Buffer first;
        Buffer second;
        Reserve(first, config);
        Reserve(second, config);
        active_ = std::move(first);
        candidate_ = std::move(second);
        config_ = config;
        return true;
    } catch (...) {
        return false;
    }
}

void ResolvedPlayablePropertyCutStorageV1::Buffer::Swap(Buffer& other) noexcept {
    std::swap(view, other.view);
    path.swap(other.path);
    payloads.swap(other.payloads);
    sends.swap(other.sends);
    effectNodes.swap(other.effectNodes);
    effectParams.swap(other.effectParams);
    effectParamOffsets.swap(other.effectParamOffsets);
    effectEdges.swap(other.effectEdges);
}

ResolvedPlayablePropertyCutStatusV1
ResolvedPlayablePropertyCutStorageV1::Resolve(
    const PlayableWorldViewV1& world, const PlayableRefV1& root,
    const std::uint32_t* selectedChildOrdinals,
    std::uint32_t selectedChildCount,
    const VnObjectId* selectedTrackIds) noexcept {
    const auto status = ResolveInto(candidate_, world, root,
                                    selectedChildOrdinals,
                                    selectedChildCount, selectedTrackIds);
    if (status != ResolvedPlayablePropertyCutStatusV1::Ok) return status;
    active_.Swap(candidate_);
    active_.Refresh();
    return status;
}

ResolvedPlayablePropertyCutStatusV1
ResolvedPlayablePropertyCutStorageV1::Resolve(
    const PackPlaybackView& world, const PlayableRefV1& root,
    const std::uint32_t* selectedChildOrdinals,
    std::uint32_t selectedChildCount,
    const VnObjectId* selectedTrackIds) noexcept {
    const auto status = ResolveInto(candidate_, world, root,
                                    selectedChildOrdinals,
                                    selectedChildCount, selectedTrackIds);
    if (status != ResolvedPlayablePropertyCutStatusV1::Ok) return status;
    active_.Swap(candidate_);
    active_.Refresh();
    return status;
}

template<class World>
ResolvedPlayablePropertyCutStatusV1
ResolvedPlayablePropertyCutStorageV1::ResolveInto(
    Buffer& output, const World& world, const PlayableRefV1& root,
    const std::uint32_t* selectedChildOrdinals,
    std::uint32_t selectedChildCount,
    const VnObjectId* selectedTrackIds) noexcept {
    output.Clear();
    if (!WorldUsable(world) || root.stableId == VnObjectId{} || root.reserved != 0 ||
        root.expectedProgramSemanticFingerprint == 0 ||
        (selectedChildCount != 0 && selectedChildOrdinals == nullptr)) {
        return ResolvedPlayablePropertyCutStatusV1::InvalidInput;
    }

    const auto sourceFor = [&](playableproperties::ContentPropertyKindV1 kind,
                               VnObjectId stableId) {
        if constexpr (std::is_same_v<World, PackPlaybackView>) {
            return world.ResolveProgramSource(kind, stableId);
        } else {
            return world.ProgramSource();
        }
    };
    const auto addPayload = [&](const PlayablePropertyPayloadViewV1& payload,
                                const ResolvedPlayablePathRowV1& pathRow,
                                const ProgramSourceIdentityV1* payloadSource = nullptr) {
        if (output.path.size() >= config_.maxPathRows) return false;
        auto sourcedRow = pathRow;
        sourcedRow.source = payloadSource != nullptr ? *payloadSource
            : sourceFor(pathRow.contentPropertyKind, pathRow.stableId);
        output.path.push_back(sourcedRow);
        output.payloads.push_back(&payload);
        return true;
    };

    const PlayablePropertyProgramViewV1* definition =
        FindProperty(world, root);
    if (definition == nullptr) {
        return ResolvedPlayablePropertyCutStatusV1::UnknownPlayable;
    }
    if (definition->structuralAncestorCount != 0) {
        // Inline ancestor payloads belong to the definition carrying the snapshot.
        const auto payloadSource = sourceFor(
            definition->contentPropertyKind, definition->stableId);
        for (std::uint32_t i = 0; i < definition->structuralAncestorCount; ++i) {
            const auto& ref = definition->structuralAncestors[i];
            if (!addPayload(
                    ref.payload,
                    {ref.contentPropertyKind ==
                             playableproperties::ContentPropertyKindV1::Layer
                         ? ResolvedPlayablePathRowKindV1::OrganizationLayer
                         : ResolvedPlayablePathRowKindV1::Definition,
                     ref.contentPropertyKind, ref.stableId,
                     ref.programSemanticFingerprint, UINT32_MAX}, &payloadSource)) {
                return ResolvedPlayablePropertyCutStatusV1::Capacity;
            }
        }
    } else {
        for (std::uint32_t i = 0;
             i < definition->organizationLayerAncestorCount; ++i) {
            const auto* layer =
                FindLayer(world, definition->organizationLayerAncestors[i]);
            if (layer == nullptr ||
                !addPayload(layer->payload,
                            {ResolvedPlayablePathRowKindV1::OrganizationLayer,
                             playableproperties::ContentPropertyKindV1::Layer,
                             layer->stableId,
                             layer->programSemanticFingerprint, UINT32_MAX})) {
                return layer == nullptr
                           ? ResolvedPlayablePropertyCutStatusV1::PathMismatch
                           : ResolvedPlayablePropertyCutStatusV1::Capacity;
            }
        }
    }
    if (!addPayload(definition->payload,
                    {ResolvedPlayablePathRowKindV1::Definition,
                     ContentKind(root.kind),
                     root.stableId, root.expectedProgramSemanticFingerprint,
                     UINT32_MAX})) {
        return ResolvedPlayablePropertyCutStatusV1::Capacity;
    }

    PlayableRefV1 cursor = root;
    std::uint32_t selectedIndex = 0;
    while (cursor.kind == PlayableKindV1::Container ||
           cursor.kind == PlayableKindV1::Track ||
           cursor.kind == PlayableKindV1::Segment) {
        if (cursor.kind == PlayableKindV1::Segment) {
            const auto segment = FindSegmentSchedule(world, cursor);
            if (segment == nullptr || segment->trackRefCount == 0 ||
                segment->trackRefs == nullptr) {
                return ResolvedPlayablePropertyCutStatusV1::
                    UnknownPlayable;
            }
            const ScheduleTrackRefV1* selectedTrack = nullptr;
            if (selectedTrackIds == nullptr && segment->trackRefCount == 1) {
                selectedTrack = &segment->trackRefs[0];
            } else if (selectedTrackIds != nullptr && selectedIndex < selectedChildCount) {
                for (std::uint32_t i = 0; i < segment->trackRefCount; ++i) {
                    if (segment->trackRefs[i].trackDefinition.stableId !=
                        selectedTrackIds[selectedIndex]) continue;
                    if (selectedTrack != nullptr)
                        return ResolvedPlayablePropertyCutStatusV1::PathMismatch;
                    selectedTrack = &segment->trackRefs[i];
                }
            }
            if (selectedTrack == nullptr)
                return ResolvedPlayablePropertyCutStatusV1::PathMismatch;
            cursor = selectedTrack->trackDefinition;
            definition = FindProperty(world, cursor);
            if (definition == nullptr) {
                return ResolvedPlayablePropertyCutStatusV1::UnknownPlayable;
            }
            if (!addPayload(
                    definition->payload,
                    {ResolvedPlayablePathRowKindV1::Definition,
                     ContentKind(cursor.kind), cursor.stableId,
                     cursor.expectedProgramSemanticFingerprint,
                     UINT32_MAX})) {
                return ResolvedPlayablePropertyCutStatusV1::Capacity;
            }
            continue;
        }
        if (selectedIndex >= selectedChildCount) {
            return ResolvedPlayablePropertyCutStatusV1::PathMismatch;
        }
        if (cursor.kind == PlayableKindV1::Track) {
            if (selectedTrackIds != nullptr &&
                selectedTrackIds[selectedIndex] != cursor.stableId)
                return ResolvedPlayablePropertyCutStatusV1::PathMismatch;
            const auto track = FindTrackSchedule(world, cursor);
            const auto* placement =
                track == nullptr
                    ? nullptr
                    : FindPlacement(*track,
                                    selectedChildOrdinals[selectedIndex++]);
            const auto* occurrence =
                placement == nullptr
                    ? nullptr
                    : FindTrackOccurrence(world, *track, *placement);
            if (occurrence != nullptr &&
                (occurrence->payload.presentMask & kResolvedSectionGroupPropertyMaskV1) != 0)
                return ResolvedPlayablePropertyCutStatusV1::InvalidInput;
            if (track == nullptr || placement == nullptr ||
                occurrence == nullptr ||
                !addPayload(
                    occurrence->payload,
                    {ResolvedPlayablePathRowKindV1::Occurrence,
                     occurrence->parentKind,
                     occurrence->parentStableId,
                     occurrence->occurrenceSemanticFingerprint,
                     occurrence->stableOccurrenceOrdinal, placement->clipId})) {
                return track == nullptr
                           ? ResolvedPlayablePropertyCutStatusV1::
                                 UnknownPlayable
                           : occurrence == nullptr
                                 ? ResolvedPlayablePropertyCutStatusV1::
                                       PathMismatch
                                 : ResolvedPlayablePropertyCutStatusV1::
                                       Capacity;
            }
            cursor = placement->content;
            definition = FindProperty(world, cursor);
            if (definition == nullptr) {
                return ResolvedPlayablePropertyCutStatusV1::UnknownPlayable;
            }
            if (!addPayload(
                    definition->payload,
                    {ResolvedPlayablePathRowKindV1::Definition,
                     ContentKind(cursor.kind), cursor.stableId,
                     cursor.expectedProgramSemanticFingerprint,
                     UINT32_MAX})) {
                return ResolvedPlayablePropertyCutStatusV1::Capacity;
            }
            continue;
        }
        if (selectedTrackIds != nullptr && selectedTrackIds[selectedIndex] != VnObjectId{})
            return ResolvedPlayablePropertyCutStatusV1::PathMismatch;
        const auto container = FindContainer(world, cursor);
        if (container == nullptr) {
            return ResolvedPlayablePropertyCutStatusV1::UnknownPlayable;
        }
        const auto* child =
            FindChild(*container, selectedChildOrdinals[selectedIndex++]);
        if (child == nullptr) {
            return ResolvedPlayablePropertyCutStatusV1::PathMismatch;
        }
        const auto* occurrence = FindOccurrence(world, *container, *child);
        if (occurrence != nullptr &&
            (occurrence->payload.presentMask & kResolvedSectionGroupPropertyMaskV1) != 0)
            return ResolvedPlayablePropertyCutStatusV1::InvalidInput;
        if (occurrence == nullptr ||
            !addPayload(
                occurrence->payload,
                {ResolvedPlayablePathRowKindV1::Occurrence,
                 occurrence->parentKind,
                 occurrence->parentStableId,
                 occurrence->occurrenceSemanticFingerprint,
                 occurrence->stableOccurrenceOrdinal})) {
            return occurrence == nullptr
                       ? ResolvedPlayablePropertyCutStatusV1::PathMismatch
                       : ResolvedPlayablePropertyCutStatusV1::Capacity;
        }
        cursor = child->playable;
        definition = FindProperty(world, cursor);
        if (definition == nullptr) {
            return ResolvedPlayablePropertyCutStatusV1::UnknownPlayable;
        }
        if (!addPayload(
                definition->payload,
                {ResolvedPlayablePathRowKindV1::Definition,
                 ContentKind(cursor.kind),
                 cursor.stableId,
                 cursor.expectedProgramSemanticFingerprint, UINT32_MAX})) {
            return ResolvedPlayablePropertyCutStatusV1::Capacity;
        }
    }
    if (cursor.kind != PlayableKindV1::Sound ||
        selectedIndex != selectedChildCount) {
        return cursor.kind == PlayableKindV1::Sound
                   ? ResolvedPlayablePropertyCutStatusV1::PathMismatch
                   : ResolvedPlayablePropertyCutStatusV1::UnsupportedPlayable;
    }

    AssignCutWorld(output.view, world);
    output.view.root = root;
    output.view.leaf = cursor;
    for (const auto* payload : output.payloads) {
        if (Present(*payload, PropertyId::Volume)) {
            output.view.volumeDb += F32(*payload, PropertyId::Volume);
        }
        if (Present(*payload, PropertyId::Pitch)) {
            output.view.pitchCents += F32(*payload, PropertyId::Pitch);
        }
        if (Present(*payload, PropertyId::MakeUpGain)) {
            output.view.makeUpGainDb += F32(*payload, PropertyId::MakeUpGain);
        }
    }
    output.view.pitchCents =
        std::clamp(output.view.pitchCents, -7200.0f, 7200.0f);

    const auto& payload = definition->payload;
    if (payload.sendCount > config_.maxSends)
        return ResolvedPlayablePropertyCutStatusV1::Capacity;
    if (!ValidateResolvedSectionGroupPayloadV1(payload))
        return ResolvedPlayablePropertyCutStatusV1::InvalidInput;
    output.view.mainOutputBusRef = ObjectId(payload, PropertyId::MainOutputBusRef);
    output.view.outputBusVolumeDb = F32(payload, PropertyId::OutputBusVolume);
    output.view.spatial.panning = F32(payload, PropertyId::SpatialPanning);
    output.view.spatial.spread = F32(payload, PropertyId::SpatialSpread);
    output.view.spatial.spatializationMix =
        F32(payload, PropertyId::SpatializationMix);
    output.view.spatial.is3DEnabled = U32(payload, PropertyId::SpatialIs3DEnabled) != 0;
    output.view.spatial.attenuationDistanceScaling =
        F32(payload, PropertyId::SpatialAttenuationDistanceScaling);
    output.view.spatial.attenuationRef = ObjectId(payload, PropertyId::SpatialAttenuationRef);
    if (Present(payload, PropertyId::LoopCount))
        output.view.loopCount = U32(payload, PropertyId::LoopCount);
    if (Present(payload, PropertyId::StreamingMode))
        output.view.streamingMode = U32(payload, PropertyId::StreamingMode);
    if (payload.sendCount != 0)
        output.sends.assign(payload.sends, payload.sends + payload.sendCount);
    if (Present(payload, PropertyId::SignalGraph)) {
        if ((payload.effectNodeCount != 0 && payload.effectNodes == nullptr) ||
            (payload.effectEdgeCount != 0 && payload.effectEdges == nullptr))
            return ResolvedPlayablePropertyCutStatusV1::InvalidInput;
        if (payload.effectNodeCount > config_.maxEffectNodes ||
            payload.effectEdgeCount > config_.maxEffectEdges)
            return ResolvedPlayablePropertyCutStatusV1::Capacity;
        std::uint64_t paramCount = 0;
        for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
            paramCount += payload.effectNodes[i].paramCount;
            if (payload.effectNodes[i].paramCount != 0 && payload.effectNodes[i].params == nullptr)
                return ResolvedPlayablePropertyCutStatusV1::InvalidInput;
        }
        if (paramCount > config_.maxEffectParams)
            return ResolvedPlayablePropertyCutStatusV1::Capacity;
        if (payload.effectNodeCount != 0)
            output.effectNodes.assign(payload.effectNodes, payload.effectNodes + payload.effectNodeCount);
        for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
            output.effectParamOffsets.push_back(static_cast<std::uint32_t>(output.effectParams.size()));
            if (payload.effectNodes[i].paramCount != 0)
                output.effectParams.insert(output.effectParams.end(), payload.effectNodes[i].params,
                    payload.effectNodes[i].params + payload.effectNodes[i].paramCount);
        }
        if (payload.effectEdgeCount != 0)
            output.effectEdges.assign(payload.effectEdges, payload.effectEdges + payload.effectEdgeCount);
    }
    output.Refresh();
    return ResolvedPlayablePropertyCutStatusV1::Ok;
}

template ResolvedPlayablePropertyCutStatusV1
ResolvedPlayablePropertyCutStorageV1::ResolveInto<PlayableWorldViewV1>(
    Buffer& output, const PlayableWorldViewV1& world, const PlayableRefV1& root,
    const std::uint32_t* selectedChildOrdinals,
    std::uint32_t selectedChildCount,
    const VnObjectId* selectedTrackIds) noexcept;
template ResolvedPlayablePropertyCutStatusV1
ResolvedPlayablePropertyCutStorageV1::ResolveInto<PackPlaybackView>(
    Buffer& output, const PackPlaybackView& world, const PlayableRefV1& root,
    const std::uint32_t* selectedChildOrdinals,
    std::uint32_t selectedChildCount,
    const VnObjectId* selectedTrackIds) noexcept;

}
