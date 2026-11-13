#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/detail/CurrentWorldValidation.h"
#include "harken/param/VariableService.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <type_traits>

#include <algorithm>
#include <harken/media/PackFormatV2.h>

namespace harken {
namespace {

struct Reader {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
    bool failed = false;

    std::uint64_t Remaining() const noexcept {
        return failed ? 0 : size - offset;
    }

    const std::uint8_t* Take(std::uint64_t count) noexcept {
        if (failed || count > size - offset) {
            failed = true;
            return nullptr;
        }
        const auto* result = bytes + offset;
        offset += static_cast<std::size_t>(count);
        return result;
    }
};

bool CountPointer(std::uint32_t count, const void* pointer,
                  std::uint32_t cap) noexcept {
    return count <= cap && ((count == 0) == (pointer == nullptr));
}

bool AddCount(std::uint32_t value, std::uint32_t cap,
              std::uint32_t& total) noexcept {
    if (value > cap || total > cap - value) {
        return false;
    }
    total += value;
    return true;
}

bool ValidStructuralAncestorKind(
    playableproperties::ContentPropertyKindV1 kind) noexcept {
    return kind == playableproperties::ContentPropertyKindV1::Layer ||
           kind == playableproperties::ContentPropertyKindV1::Container ||
           kind == playableproperties::ContentPropertyKindV1::Track ||
           kind == playableproperties::ContentPropertyKindV1::Segment;
}

bool CountPayload(const PlayablePropertyPayloadViewV1& payload,
                  std::array<std::uint32_t, 6>& totals) noexcept {
    if (!CountPointer(payload.sendCount, payload.sends,
                      kHardPlayablePropertyChildRowCapacity) ||
        !CountPointer(payload.effectNodeCount, payload.effectNodes,
                      kHardPlayablePropertyChildRowCapacity) ||
        !CountPointer(payload.effectEdgeCount, payload.effectEdges,
                      kHardPlayablePropertyEffectEdgeCapacity) ||
        !AddCount(payload.sendCount, kHardPlayablePropertyChildRowCapacity,
                  totals[0]) ||
        !AddCount(payload.effectNodeCount,
                  kHardPlayablePropertyChildRowCapacity, totals[3]) ||
        !AddCount(payload.effectEdgeCount,
                  kHardPlayablePropertyEffectEdgeCapacity, totals[5])) {
        return false;
    }
    for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
        const EffectNodeV1& node = payload.effectNodes[i];
        if ((node.effectType == kPackEffectPresetReferenceType
                 ? node.presetId == decltype(node.presetId){} || node.paramCount != 0
                 : node.presetId != decltype(node.presetId){}) ||
            !CountPointer(node.paramCount, node.params,
                          kHardPlayablePropertyEffectParamCapacity) ||
            !AddCount(node.paramCount,
                      kHardPlayablePropertyEffectParamCapacity, totals[4])) {
            return false;
        }
    }
    return true;
}

bool V9CapacityPrefixMatches(const WorldCapacitySummaryV1& prefix,
                             const WorldCapacitySummaryV1& complete) noexcept {
    return prefix.maxContainerDepth == complete.maxContainerDepth &&
           prefix.maxChildrenPerContainer == complete.maxChildrenPerContainer &&
           prefix.maxAvoidLastN == complete.maxAvoidLastN &&
           prefix.maxDiscreteAxes == complete.maxDiscreteAxes &&
           prefix.maxDiscreteRows == complete.maxDiscreteRows &&
           prefix.maxDiscreteTrieNodes == complete.maxDiscreteTrieNodes &&
           prefix.maxContinuousAxes == complete.maxContinuousAxes &&
           prefix.maxContinuousRows == complete.maxContinuousRows &&
           prefix.maxContinuousCandidates == complete.maxContinuousCandidates &&
           prefix.maxContinuousActiveRows ==
               complete.maxContinuousActiveRows &&
           prefix.maxContinuousLiveRows == complete.maxContinuousLiveRows &&
           prefix.maxTransitionRules == complete.maxTransitionRules &&
           prefix.maxCurvePoints == complete.maxCurvePoints &&
           prefix.maxKnownScheduleLeaves == complete.maxKnownScheduleLeaves &&
           prefix.maxTrackSchedulePrograms ==
               complete.maxTrackSchedulePrograms &&
           prefix.maxSegmentSchedulePrograms ==
               complete.maxSegmentSchedulePrograms &&
           prefix.maxScheduleTrackRefsPerSegment ==
               complete.maxScheduleTrackRefsPerSegment &&
           prefix.maxScheduleTrackRefsTotal ==
               complete.maxScheduleTrackRefsTotal &&
           prefix.maxSchedulePlacementsPerTrack ==
               complete.maxSchedulePlacementsPerTrack &&
           prefix.maxSchedulePlacementsTotal ==
               complete.maxSchedulePlacementsTotal &&
           prefix.maxScheduleMarkersPerSegment ==
               complete.maxScheduleMarkersPerSegment &&
           prefix.maxScheduleMarkersTotal == complete.maxScheduleMarkersTotal &&
           prefix.maxTrackSegmentScheduleDepth ==
               complete.maxTrackSegmentScheduleDepth &&
           prefix.maxActionDefinitions == complete.maxActionDefinitions &&
           prefix.maxActionStepsPerDefinition ==
               complete.maxActionStepsPerDefinition &&
           prefix.maxActionStepsTotal == complete.maxActionStepsTotal &&
           prefix.maxActionTriggerDepth == complete.maxActionTriggerDepth &&
           prefix.maxActionExpandedWork == complete.maxActionExpandedWork;
}

constexpr auto kTraceWorldChildCap = kHardPlayablePropertyChildRowCapacity;

constexpr auto kPackBodyActionStepCap = kHardActionStepCapacity;

}

struct CurrentWorldBodyImage::Impl {
    detail::PackDecodeCharge staging;
    struct OwnedPayload {
        PlayablePropertyPayloadViewV1 view{};
        std::vector<SendV1> sends;
        std::vector<EffectNodeV1> nodes;
        std::vector<std::vector<EffectParamV1>> params;
        std::vector<EffectEdgeV1> edges;

        void Refresh() noexcept {
            view.sendCount = static_cast<std::uint32_t>(sends.size());
            view.sends = sends.empty() ? nullptr : sends.data();
            view.effectNodeCount = static_cast<std::uint32_t>(nodes.size());
            view.effectNodes = nodes.empty() ? nullptr : nodes.data();
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                nodes[i].paramCount =
                    static_cast<std::uint32_t>(params[i].size());
                nodes[i].params = params[i].empty() ? nullptr : params[i].data();
            }
            view.effectEdgeCount = static_cast<std::uint32_t>(edges.size());
            view.effectEdges = edges.empty() ? nullptr : edges.data();
        }
    };

    struct OwnedProperty {
        PlayablePropertyProgramViewV1 view{};
        std::vector<LayerAncestorRefV1> ancestors;
        std::vector<StructuralAncestorRefV1> structuralAncestors;
        std::vector<OwnedPayload> structuralPayloads;
        OwnedPayload payload;
        void Refresh() noexcept {
            payload.Refresh();
            view.organizationLayerAncestorCount =
                static_cast<std::uint32_t>(ancestors.size());
            view.organizationLayerAncestors =
                ancestors.empty() ? nullptr : ancestors.data();
            view.structuralAncestorCount =
                static_cast<std::uint32_t>(structuralAncestors.size());
            view.structuralAncestors = structuralAncestors.empty()
                ? nullptr : structuralAncestors.data();
            for (std::size_t i = 0; i < structuralAncestors.size(); ++i) {
                structuralPayloads[i].Refresh();
                structuralAncestors[i].payload = structuralPayloads[i].view;
            }
            view.payload = payload.view;
        }
    };

    struct OwnedOccurrence {
        PlayableOccurrencePropertyViewV1 view{};
        OwnedPayload payload;
        void Refresh() noexcept {
            payload.Refresh();
            view.payload = payload.view;
        }
    };

    struct OwnedCurrentContainer {
        ContainerProgramViewV3 view{};
        std::vector<CurveV1> curves;
        std::vector<ContainerChildProgramViewV2> children;
        std::vector<GridAxisV1> discreteAxes;
        std::vector<DiscreteGridRowViewV1> discreteRows;
        std::vector<std::vector<DiscreteGridCellV1>> discreteCells;
        std::vector<GridAxisV1> continuousAxes;
        std::vector<ContinuousGridRowViewV1> continuousRows;
        std::vector<std::vector<ContinuousGridCellV1>> continuousCells;
        void Refresh() noexcept {
            view.curveCount = static_cast<std::uint32_t>(curves.size());
            view.curves = curves.empty() ? nullptr : curves.data();
            view.childCount = static_cast<std::uint32_t>(children.size());
            view.children = children.empty() ? nullptr : children.data();
            view.discreteAxisCount =
                static_cast<std::uint32_t>(discreteAxes.size());
            view.discreteAxes =
                discreteAxes.empty() ? nullptr : discreteAxes.data();
            view.discreteRowCount =
                static_cast<std::uint32_t>(discreteRows.size());
            view.discreteRows =
                discreteRows.empty() ? nullptr : discreteRows.data();
            for (std::size_t i = 0; i < discreteRows.size(); ++i) {
                discreteRows[i].cellCount =
                    static_cast<std::uint32_t>(discreteCells[i].size());
                discreteRows[i].cells = discreteCells[i].empty()
                    ? nullptr : discreteCells[i].data();
            }
            view.continuousAxisCount =
                static_cast<std::uint32_t>(continuousAxes.size());
            view.continuousAxes =
                continuousAxes.empty() ? nullptr : continuousAxes.data();
            view.continuousRowCount =
                static_cast<std::uint32_t>(continuousRows.size());
            view.continuousRows =
                continuousRows.empty() ? nullptr : continuousRows.data();
            for (std::size_t i = 0; i < continuousRows.size(); ++i) {
                continuousRows[i].cellCount =
                    static_cast<std::uint32_t>(continuousCells[i].size());
                continuousRows[i].cells = continuousCells[i].empty()
                    ? nullptr : continuousCells[i].data();
            }
        }
    };

    struct OwnedTrackSchedule {
        TrackScheduleProgramViewV1 view{};
        std::vector<ScheduleClipPlacementV1> placements;
        void Refresh() noexcept {
            view.placementCount =
                static_cast<std::uint32_t>(placements.size());
            view.placements = placements.empty() ? nullptr : placements.data();
        }
    };

    struct OwnedSegmentSchedule {
        SegmentScheduleProgramViewV1 view{};
        std::vector<ScheduleTrackRefV1> trackRefs;
        std::vector<ScheduleMarkerV1> markers;
        void Refresh() noexcept {
            view.trackRefCount = static_cast<std::uint32_t>(trackRefs.size());
            view.trackRefs = trackRefs.empty() ? nullptr : trackRefs.data();
            view.markerCount = static_cast<std::uint32_t>(markers.size());
            view.markers = markers.empty() ? nullptr : markers.data();
        }
    };

    struct OwnedTransition {
        TransitionProgramViewV1 view{};
        std::vector<StableAuthoredPathStepV1> downwardPathSteps;
        std::vector<TransitionRuleViewV1> rules;
        void Refresh() noexcept {
            view.downwardPathStepCount =
                static_cast<std::uint32_t>(downwardPathSteps.size());
            view.downwardPathSteps = downwardPathSteps.empty()
                ? nullptr : downwardPathSteps.data();
            view.ruleCount = static_cast<std::uint32_t>(rules.size());
            view.rules = rules.empty() ? nullptr : rules.data();
        }
    };

    struct OwnedAction {
        ActionProgramViewV1 view{};
        std::vector<ActionStepProgramViewV1> steps;
        void Refresh() noexcept {
            view.stepCount = static_cast<std::uint32_t>(steps.size());
            view.steps = steps.empty() ? nullptr : steps.data();
        }
    };

    ContainerWorldInstallViewV1 view{};
    std::vector<VariableDefinitionV1> variables;
    std::vector<OwnedProperty> properties;
    std::vector<PlayablePropertyProgramViewV1> propertyViews;
    std::vector<OwnedOccurrence> occurrences;
    std::vector<PlayableOccurrencePropertyViewV1> occurrenceViews;
    std::vector<OwnedTrackSchedule> trackSchedules;
    std::vector<TrackScheduleProgramViewV1> trackScheduleViews;
    std::vector<OwnedSegmentSchedule> segmentSchedules;
    std::vector<SegmentScheduleProgramViewV1> segmentScheduleViews;
    std::vector<OwnedCurrentContainer> currentContainers;
    std::vector<ContainerProgramViewV3> currentContainerViews;
    std::vector<OwnedTransition> transitions;
    std::vector<TransitionProgramViewV1> transitionViews;
    std::vector<OwnedAction> actions;
    std::vector<ActionProgramViewV1> actionViews;
    std::vector<SignalRelationV1> signalRelations;
    std::vector<SignalReceiverV1> signalReceivers;

    void Refresh() {
        propertyViews.clear();
        detail::DecodeReserve(propertyViews, properties.size(), staging);
        for (auto& row : properties) {
            row.Refresh();
            propertyViews.push_back(row.view);
        }
        occurrenceViews.clear();
        detail::DecodeReserve(occurrenceViews, occurrences.size(), staging);
        for (auto& row : occurrences) {
            row.Refresh();
            occurrenceViews.push_back(row.view);
        }
        trackScheduleViews.clear();
        detail::DecodeReserve(trackScheduleViews, trackSchedules.size(), staging);
        for (auto& row : trackSchedules) {
            row.Refresh();
            trackScheduleViews.push_back(row.view);
        }
        segmentScheduleViews.clear();
        detail::DecodeReserve(segmentScheduleViews, segmentSchedules.size(), staging);
        for (auto& row : segmentSchedules) {
            row.Refresh();
            segmentScheduleViews.push_back(row.view);
        }
        currentContainerViews.clear();
        detail::DecodeReserve(currentContainerViews, currentContainers.size(), staging);
        for (auto& row : currentContainers) {
            row.Refresh();
            currentContainerViews.push_back(row.view);
        }
        transitionViews.clear();
        detail::DecodeReserve(transitionViews, transitions.size(), staging);
        for (auto& row : transitions) {
            row.Refresh();
            transitionViews.push_back(row.view);
        }
        view.variables = variables.empty() ? nullptr : variables.data();
        view.variableCount = static_cast<std::uint32_t>(variables.size());
        view.playableProperties =
            propertyViews.empty() ? nullptr : propertyViews.data();
        view.playablePropertyCount =
            static_cast<std::uint32_t>(propertyViews.size());
        view.playableOccurrences =
            occurrenceViews.empty() ? nullptr : occurrenceViews.data();
        view.playableOccurrenceCount =
            static_cast<std::uint32_t>(occurrenceViews.size());
        view.trackSchedules = trackScheduleViews.empty()
            ? nullptr : trackScheduleViews.data();
        view.trackScheduleCount =
            static_cast<std::uint32_t>(trackScheduleViews.size());
        view.segmentSchedules = segmentScheduleViews.empty()
            ? nullptr : segmentScheduleViews.data();
        view.segmentScheduleCount =
            static_cast<std::uint32_t>(segmentScheduleViews.size());
        view.containers = currentContainerViews.empty()
            ? nullptr : currentContainerViews.data();
        view.containerCount =
            static_cast<std::uint32_t>(currentContainerViews.size());
        view.transitions = transitionViews.empty()
            ? nullptr : transitionViews.data();
        view.transitionCount = static_cast<std::uint32_t>(transitionViews.size());
        actionViews.clear();
        detail::DecodeReserve(actionViews, actions.size(), staging);
        for (auto& row : actions) {
            row.Refresh();
            actionViews.push_back(row.view);
        }
        view.actions = actionViews.empty() ? nullptr : actionViews.data();
        view.actionCount = static_cast<std::uint32_t>(actionViews.size());
        view.signalRelations = signalRelations.empty() ? nullptr : signalRelations.data();
        view.signalRelationCount = static_cast<std::uint32_t>(signalRelations.size());
        view.signalReceivers = signalReceivers.empty() ? nullptr : signalReceivers.data();
        view.signalReceiverCount = static_cast<std::uint32_t>(signalReceivers.size());
    }
};

namespace {

struct PackBodyWriteTrace {
    PackWireBuffer<std::uint8_t>* output = nullptr;

    bool Segment(const SegmentScheduleProgramViewV1& row) noexcept {
        if (output == nullptr) return true;
        auto* bytes = output->Take(kPackBodyWorldSegmentEntryTimelineBytes);
        if (bytes == nullptr) return false;
        SerializePackBodyWorldSegmentEntryTimeline(
            {row.entryMarker.stableMarkerId,
             std::bit_cast<std::uint64_t>(row.entryMarker.position),
             std::bit_cast<std::uint64_t>(row.bounds.contentStart),
             std::bit_cast<std::uint64_t>(row.bounds.contentEnd),
             std::bit_cast<std::uint64_t>(row.bounds.nominalStart),
             std::bit_cast<std::uint64_t>(row.bounds.nominalEnd)},
            bytes);
        return true;
    }
};

struct PackBodyVisitorEnd {
    bool Finish() noexcept { return true; }
};

template <class Row>
struct PackBodyWriteRow : PackBodyVisitorEnd {
    const Row& value;
    const Row& Value() noexcept { return value; }
    const Row& Header() noexcept { return value; }
};

struct PackBodyWritePayloadNode : PackBodyWriteRow<EffectNodeV1> {
    const auto& PresetReference() noexcept { return value; }
    bool PrepareParams(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.params,
                            kHardPlayablePropertyEffectParamCapacity);
    }
    const auto& Params(std::uint64_t i) noexcept { return value.params[i]; }
};

struct PackBodyWritePayload : PackBodyWriteRow<PlayablePropertyPayloadViewV1> {
    auto Header() noexcept {
        auto row = value;
        row.presentMask &= ~UINT64_C(4);
        return row;
    }
    auto& Body() noexcept { return *this; }
    bool PrepareSends(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.sends,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& Sends(std::uint64_t i) noexcept { return value.sends[i]; }
    bool PrepareEffectNodes(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.effectNodes,
                            kHardPlayablePropertyChildRowCapacity);
    }
    auto EffectNodes(std::uint64_t i) noexcept {
        return PackBodyWritePayloadNode{{{}, value.effectNodes[i]}};
    }
    bool PrepareEffectEdges(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.effectEdges,
                            kHardPlayablePropertyEffectEdgeCapacity);
    }
    const auto& EffectEdges(std::uint64_t i) noexcept { return value.effectEdges[i]; }
};

struct PackBodyWriteStructural : PackBodyWriteRow<StructuralAncestorRefV1> {
    auto Payload() noexcept { return PackBodyWritePayload{{{}, value.payload}}; }
};

struct PackBodyWriteProperty : PackBodyWriteRow<PlayablePropertyProgramViewV1> {
    auto PayloadHeader() noexcept {
        auto row = value.payload;
        row.presentMask &= ~UINT64_C(4);
        return row;
    }
    bool PrepareAncestors(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.organizationLayerAncestors,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& Ancestors(std::uint64_t i) noexcept { return value.organizationLayerAncestors[i]; }
    const auto& StructuralHeader() noexcept { return value; }
    bool PrepareStructuralAncestors(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.structuralAncestors,
                            kHardPlayablePropertyChildRowCapacity);
    }
    auto StructuralAncestors(std::uint64_t i) noexcept {
        return PackBodyWriteStructural{{{}, value.structuralAncestors[i]}};
    }
    auto PayloadBody() noexcept { return PackBodyWritePayload{{{}, value.payload}}; }
};

struct PackBodyWriteOccurrence : PackBodyWriteRow<PlayableOccurrencePropertyViewV1> {
    auto Payload() noexcept { return PackBodyWritePayload{{{}, value.payload}}; }
};

struct PackBodyWriteTrack : PackBodyWriteRow<TrackScheduleProgramViewV1> {
    bool PreparePlacements(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.placements,
                            kHardSchedulePlacementCapacity);
    }
    const auto& Placements(std::uint64_t i) noexcept { return value.placements[i]; }
};

struct PackBodyWriteSegment : PackBodyWriteRow<SegmentScheduleProgramViewV1> {
    PackBodyWriteTrace& trace;
    bool PrepareTrackRefs(std::uint64_t count) noexcept {
        return trace.Segment(value) && CountPointer(static_cast<std::uint32_t>(count), value.trackRefs,
                            kHardScheduleTrackRefCapacity);
    }
    const auto& TrackRefs(std::uint64_t i) noexcept { return value.trackRefs[i]; }
    bool PrepareMarkers(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.markers,
                            kHardScheduleMarkerCapacity);
    }
    const auto& Markers(std::uint64_t i) noexcept { return value.markers[i]; }
};

struct PackBodyWriteCurve : PackBodyWriteRow<CurveV1> {
    bool PreparePoints(std::uint64_t count) noexcept { return count <= kMaxCurvePoints; }
    const auto& Points(std::uint64_t i) noexcept { return value.curve.points[i]; }
    bool PrepareHandles(std::uint64_t count) noexcept { return count < kMaxCurvePoints; }
    const auto& Handles(std::uint64_t i) noexcept { return value.curve.handles[i]; }
};

struct PackBodyWriteChild : PackBodyWriteRow<ContainerChildProgramViewV2> {
    const auto& InlineSound() noexcept { return value.timingHint.payload.inlineSound; }
    const auto& Schedule() noexcept { return value.timingHint.payload.schedule; }
    bool Finish() noexcept {
        return value.timingHint.kind == ChildTimingHintKindV2::Unknown ||
               value.timingHint.kind == ChildTimingHintKindV2::InlineSound ||
               value.timingHint.kind == ChildTimingHintKindV2::ScheduleReference;
    }
};

template <class Row>
struct PackBodyWriteGrid : PackBodyWriteRow<Row> {
    bool PrepareCells(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), this->value.cells,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& Cells(std::uint64_t i) noexcept { return this->value.cells[i]; }
};

struct PackBodyWriteContainer : PackBodyWriteRow<ContainerProgramViewV3> {
    bool PrepareCurves(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.curves,
                            kHardPlayablePropertyChildRowCapacity);
    }
    auto Curves(std::uint64_t i) noexcept { return PackBodyWriteCurve{{{}, value.curves[i]}}; }
    bool PrepareChildren(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.children,
                            kHardPlayablePropertyChildRowCapacity);
    }
    auto Children(std::uint64_t i) noexcept { return PackBodyWriteChild{{{}, value.children[i]}}; }
    bool PrepareDiscreteAxes(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.discreteAxes,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& DiscreteAxes(std::uint64_t i) noexcept { return value.discreteAxes[i]; }
    bool PrepareDiscreteRows(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.discreteRows,
                            kHardPlayablePropertyChildRowCapacity);
    }
    auto DiscreteRows(std::uint64_t i) noexcept {
        return PackBodyWriteGrid<DiscreteGridRowViewV1>{{{}, value.discreteRows[i]}};
    }
    bool PrepareContinuousAxes(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.continuousAxes,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& ContinuousAxes(std::uint64_t i) noexcept { return value.continuousAxes[i]; }
    bool PrepareContinuousRows(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.continuousRows,
                            kHardPlayablePropertyChildRowCapacity);
    }
    auto ContinuousRows(std::uint64_t i) noexcept {
        return PackBodyWriteGrid<ContinuousGridRowViewV1>{{{}, value.continuousRows[i]}};
    }
};

struct PackBodyWriteTransition : PackBodyWriteRow<TransitionProgramViewV1> {
    bool PreparePath(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.downwardPathSteps,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& Path(std::uint64_t i) noexcept { return value.downwardPathSteps[i]; }
    bool PrepareRules(std::uint64_t count) noexcept {
        return CountPointer(static_cast<std::uint32_t>(count), value.rules,
                            kHardPlayablePropertyChildRowCapacity);
    }
    const auto& Rules(std::uint64_t i) noexcept { return value.rules[i]; }
};

struct PackBodyWriteAction : PackBodyWriteRow<ActionProgramViewV1> {
    bool PrepareSteps(std::uint64_t count) noexcept {
        return count != 0 && CountPointer(static_cast<std::uint32_t>(count), value.steps,
                                          kPackBodyActionStepCap);
    }
    const auto& Steps(std::uint64_t i) noexcept { return value.steps[i]; }
};

struct PackBodyWriteHeader : PackBodyWriteRow<ContainerWorldInstallViewV1> {
    const CurrentWorldV3PrefixValues& prefix;
    const std::uint32_t& structuralCount;
    const auto& Prefix() noexcept { return prefix; }
    const auto& ActionCapacity() noexcept { return value; }
    const auto& StructuralCount() noexcept { return structuralCount; }
    const auto& Capacity() noexcept { return value.capacity; }
    const auto& ProjectId() noexcept { return value.identity.projectId; }
};

struct PackBodyWriteVariable : PackBodyVisitorEnd {
    PackWorldVariableWire value;
    auto& Value() noexcept { return value; }
    auto& Header() noexcept { return value; }
};

struct PackBodyWriteWorld : PackBodyWriteRow<ContainerWorldInstallViewV1> {
    PackBodyWriteTrace& trace;
    const CurrentWorldV3PrefixValues& prefix;
    const std::uint32_t& structuralCount;
    auto Header() noexcept { return PackBodyWriteHeader{{{}, value}, prefix, structuralCount}; }
    bool PrepareVariables(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.variables, kHardVariableDefinitionCapacity); }
    auto Variables(std::uint64_t i) noexcept {
        const auto& row = value.variables[i];
        return PackBodyWriteVariable{{}, {row.variableId, static_cast<std::uint32_t>(row.kind), row.rangeMin, row.rangeMax, row.defaultValue}};
    }
    bool PrepareProperties(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.playableProperties, kHardPlayablePropertyProgramCapacity); }
    auto Properties(std::uint64_t i) noexcept { return PackBodyWriteProperty{{{}, value.playableProperties[i]}}; }
    bool PrepareOccurrences(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.playableOccurrences, kHardPlayableOccurrencePropertyCapacity); }
    auto Occurrences(std::uint64_t i) noexcept { return PackBodyWriteOccurrence{{{}, value.playableOccurrences[i]}}; }
    bool PrepareTrackSchedules(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.trackSchedules, kHardTrackScheduleProgramCapacity); }
    auto TrackSchedules(std::uint64_t i) noexcept { return PackBodyWriteTrack{{{}, value.trackSchedules[i]}}; }
    bool PrepareSegmentSchedules(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.segmentSchedules, kHardSegmentScheduleProgramCapacity); }
    auto SegmentSchedules(std::uint64_t i) noexcept { return PackBodyWriteSegment{{{}, value.segmentSchedules[i]}, trace}; }
    bool PrepareContainers(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.containers, kHardContainerProgramCapacity); }
    auto Containers(std::uint64_t i) noexcept { return PackBodyWriteContainer{{{}, value.containers[i]}}; }
    bool PrepareTransitions(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.transitions, kHardPlayablePropertyChildRowCapacity); }
    auto Transitions(std::uint64_t i) noexcept { return PackBodyWriteTransition{{{}, value.transitions[i]}}; }
    bool PrepareActions(std::uint64_t count) noexcept { return CountPointer(static_cast<std::uint32_t>(count), value.actions, kHardActionDefinitionCapacity); }
    auto Actions(std::uint64_t i) noexcept { return PackBodyWriteAction{{{}, value.actions[i]}}; }
};

struct PackBodyReadState {
    Reader& reader;
    CurrentWorldBodyDecodeResult result = CurrentWorldBodyDecodeResult::Invalid;
    CurrentWorldV3PrefixValues prefix{};
    WorldCapacitySummaryV1 completeCapacity{};
    std::uint32_t structuralCount = 0;
    std::array<std::uint32_t, 6> payloadTotals{};
    std::uint32_t ancestors = 0;
    std::uint32_t structuralAncestors = 0;
    std::uint32_t trackRefs = 0;
    std::uint32_t placements = 0;
    std::uint32_t markers = 0;
    std::uint32_t transitionRules = 0;
    std::uint32_t transitionPath = 0;
    std::uint64_t actionSteps = 0;
    std::uint32_t bodyVintage = kCurrentWorldShippingBodyVintage;
    detail::PackDecodeCharge* staging = nullptr;

    template <class Row>
    bool Resize(std::vector<Row>& rows, std::uint64_t count, std::uint64_t cap) noexcept {
        if (count > cap) return false;
        try {
            detail::DecodeResize(rows, static_cast<std::size_t>(count), *staging);
            return true;
        } catch (const std::bad_alloc&) {
            result = CurrentWorldBodyDecodeResult::Capacity;
            return false;
        } catch (...) {
            return false;
        }
    }
};

bool PackBodyPayloadHeaderValid(const PlayablePropertyPayloadViewV1& row) noexcept {
    return (row.presentMask & UINT64_C(4)) == 0 && row.values[14].bits == 0;
}

template <class Row>
struct PackBodyReadRow : PackBodyVisitorEnd {
    Row& value;
    Row& Value() noexcept { return value; }
    Row& Header() noexcept { return value; }
};

struct PackBodyReadPayloadNode : PackBodyReadRow<EffectNodeV1> {
    auto& PresetReference() noexcept { return value; }
    std::vector<EffectParamV1>& params;
    PackBodyReadState& state;
    std::uint32_t& paramTotal;
    bool PrepareParams(std::uint64_t count) noexcept {
        return AddCount(static_cast<std::uint32_t>(count), kHardPlayablePropertyEffectParamCapacity, paramTotal) &&
               state.Resize(params, count, kHardPlayablePropertyEffectParamCapacity);
    }
    auto& Params(std::uint64_t i) noexcept { return params[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept {
        value.params = params.empty() ? nullptr : params.data();
        return value.effectType == kPackEffectPresetReferenceType
            ? value.presetId != decltype(value.presetId){} && value.paramCount == 0
            : value.presetId == decltype(value.presetId){};
    }
};

template <class Owned>
struct PackBodyReadPayload : PackBodyReadRow<PlayablePropertyPayloadViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    std::uint32_t paramTotal = 0;
    auto& Body() noexcept { return *this; }
    bool PrepareSends(std::uint64_t count) noexcept {
        return PackBodyPayloadHeaderValid(this->value) &&
               state.Resize(owned.sends, count, kHardPlayablePropertyChildRowCapacity);
    }
    auto& Sends(std::uint64_t i) noexcept { return owned.sends[static_cast<std::size_t>(i)]; }
    bool PrepareEffectNodes(std::uint64_t count) noexcept {
        return state.Resize(owned.nodes, count, kHardPlayablePropertyChildRowCapacity) &&
               state.Resize(owned.params, count, kHardPlayablePropertyChildRowCapacity);
    }
    auto EffectNodes(std::uint64_t i) noexcept {
        const auto index = static_cast<std::size_t>(i);
        return PackBodyReadPayloadNode{{{}, owned.nodes[index]}, owned.params[index], state, paramTotal};
    }
    bool PrepareEffectEdges(std::uint64_t count) noexcept {
        return state.Resize(owned.edges, count, kHardPlayablePropertyEffectEdgeCapacity);
    }
    auto& EffectEdges(std::uint64_t i) noexcept { return owned.edges[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept {
        this->value.values[2] = {};
        owned.Refresh();
        return PackBodyPayloadHeaderValid(this->value);
    }
};

template <class Owned>
auto PackBodyPayloadReader(Owned& owned, PackBodyReadState& state) noexcept {
    return PackBodyReadPayload<Owned>{{{}, owned.view}, owned, state};
}

template <class PayloadStorage>
struct PackBodyReadStructural : PackBodyReadRow<StructuralAncestorRefV1> {
    PayloadStorage& payload;
    PackBodyReadState& state;
    auto PayloadValue() noexcept { return PackBodyPayloadReader(payload, state); }
    auto Payload() noexcept { return PayloadValue(); }
    bool Finish() noexcept {
        this->value.payload = payload.view;
        return ValidStructuralAncestorKind(this->value.contentPropertyKind) &&
               this->value.reserved == 0 && this->value.stableId != decltype(this->value.stableId){} &&
               this->value.programSemanticFingerprint != 0;
    }
};

template <class Owned>
struct PackBodyReadProperty : PackBodyReadRow<PlayablePropertyProgramViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    auto& PayloadHeader() noexcept { return owned.payload.view; }
    bool PrepareAncestors(std::uint64_t count) noexcept {
        return PackBodyPayloadHeaderValid(owned.payload.view) &&
               AddCount(static_cast<std::uint32_t>(count), kHardPlayablePropertyChildRowCapacity, state.ancestors) &&
               state.Resize(owned.ancestors, count, kHardPlayablePropertyChildRowCapacity);
    }
    auto& Ancestors(std::uint64_t i) noexcept { return owned.ancestors[static_cast<std::size_t>(i)]; }
    auto& StructuralHeader() noexcept { return this->value; }
    bool PrepareStructuralAncestors(std::uint64_t count) noexcept {
        return AddCount(static_cast<std::uint32_t>(count), kHardPlayablePropertyChildRowCapacity, state.structuralAncestors) &&
               state.Resize(owned.structuralAncestors, count, kHardPlayablePropertyChildRowCapacity) &&
               state.Resize(owned.structuralPayloads, count, kHardPlayablePropertyChildRowCapacity);
    }
    auto StructuralAncestors(std::uint64_t i) noexcept {
        const auto index = static_cast<std::size_t>(i);
        using Payload = std::remove_reference_t<decltype(owned.structuralPayloads[index])>;
        return PackBodyReadStructural<Payload>{{{}, owned.structuralAncestors[index]}, owned.structuralPayloads[index], state};
    }
    auto PayloadBody() noexcept { return PackBodyPayloadReader(owned.payload, state); }
    bool Finish() noexcept {
        owned.Refresh();
        for (std::size_t i = 1; i < owned.structuralAncestors.size(); ++i) {
            const auto& prior = owned.structuralAncestors[i - 1];
            const auto& current = owned.structuralAncestors[i];
            if (prior.contentPropertyKind == current.contentPropertyKind && prior.stableId == current.stableId) return false;
        }
        return CountPayload(this->value.payload, state.payloadTotals);
    }
};

template <class Owned>
struct PackBodyReadOccurrence : PackBodyReadRow<PlayableOccurrencePropertyViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    auto Payload() noexcept { return PackBodyPayloadReader(owned.payload, state); }
    bool Finish() noexcept { owned.Refresh(); return CountPayload(this->value.payload, state.payloadTotals); }
};

template <class Owned>
struct PackBodyReadTrack : PackBodyReadRow<TrackScheduleProgramViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    bool PreparePlacements(std::uint64_t count) noexcept {
        return AddCount(static_cast<std::uint32_t>(count), kHardSchedulePlacementCapacity, state.placements) &&
               state.Resize(owned.placements, count, kHardSchedulePlacementCapacity);
    }
    auto& Placements(std::uint64_t i) noexcept { return owned.placements[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { owned.Refresh(); return true; }
};

template <class Owned>
struct PackBodyReadSegment : PackBodyReadRow<SegmentScheduleProgramViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    bool PrepareTrackRefs(std::uint64_t count) noexcept {
        if (state.bodyVintage == kCurrentWorldBodyVintageV19) {
            const auto* bytes = state.reader.Take(kPackBodyWorldSegmentEntryTimelineBytes);
            PackSegmentEntryTimeline timeline{};
            if (bytes == nullptr ||
                !ParsePackBodyWorldSegmentEntryTimeline(
                    bytes, kPackBodyWorldSegmentEntryTimelineBytes, &timeline)) return false;
            this->value.entryMarker.stableMarkerId = timeline.stableMarkerId;
            this->value.entryMarker.position = std::bit_cast<std::int64_t>(timeline.position);
            this->value.bounds.contentStart = std::bit_cast<std::int64_t>(timeline.contentStart);
            this->value.bounds.contentEnd = std::bit_cast<std::int64_t>(timeline.contentEnd);
            this->value.bounds.nominalStart = std::bit_cast<std::int64_t>(timeline.nominalStart);
            this->value.bounds.nominalEnd = std::bit_cast<std::int64_t>(timeline.nominalEnd);
        }
        return AddCount(static_cast<std::uint32_t>(count), kHardScheduleTrackRefCapacity, state.trackRefs) &&
               state.Resize(owned.trackRefs, count, kHardScheduleTrackRefCapacity);
    }
    auto& TrackRefs(std::uint64_t i) noexcept { return owned.trackRefs[static_cast<std::size_t>(i)]; }
    bool PrepareMarkers(std::uint64_t count) noexcept {
        return AddCount(static_cast<std::uint32_t>(count), kHardScheduleMarkerCapacity, state.markers) &&
               state.Resize(owned.markers, count, kHardScheduleMarkerCapacity);
    }
    auto& Markers(std::uint64_t i) noexcept { return owned.markers[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { owned.Refresh(); return true; }
};

struct PackBodyReadCurve : PackBodyReadRow<CurveV1> {
    bool PreparePoints(std::uint64_t count) noexcept { return count <= kMaxCurvePoints; }
    auto& Points(std::uint64_t i) noexcept { return value.curve.points[i]; }
    bool PrepareHandles(std::uint64_t count) noexcept { return count < kMaxCurvePoints; }
    auto& Handles(std::uint64_t i) noexcept { return value.curve.handles[i]; }
    bool Finish() noexcept {
        std::array<CurvePoint, kMaxCurvePoints> points{};
        std::array<CurveHandlePair, kMaxCurvePoints - 1> handles{};
        const auto count = value.curve.pointCount;
        std::copy_n(value.curve.points, count, points.data());
        if (count != 0) std::copy_n(value.curve.handles, count - 1, handles.data());
        return count != 0 && value.curve.SetPoints(points.data(), handles.data(), count);
    }
};

struct PackBodyReadChild : PackBodyReadRow<ContainerChildProgramViewV2> {
    auto& InlineSound() noexcept { return value.timingHint.payload.inlineSound; }
    auto& Schedule() noexcept { return value.timingHint.payload.schedule; }
    bool Finish() noexcept {
        return value.timingHint.kind == ChildTimingHintKindV2::Unknown ||
               value.timingHint.kind == ChildTimingHintKindV2::InlineSound ||
               value.timingHint.kind == ChildTimingHintKindV2::ScheduleReference;
    }
};

template <class Row, class CellStorage>
struct PackBodyReadGrid : PackBodyReadRow<Row> {
    CellStorage& cells;
    PackBodyReadState& state;
    bool PrepareCells(std::uint64_t count) noexcept { return state.Resize(cells, count, kHardPlayablePropertyChildRowCapacity); }
    auto& CellsValue(std::uint64_t i) noexcept { return cells[static_cast<std::size_t>(i)]; }
    auto& Cells(std::uint64_t i) noexcept { return CellsValue(i); }
    bool Finish() noexcept { this->value.cells = cells.empty() ? nullptr : cells.data(); return true; }
};

template <class Owned>
struct PackBodyReadContainer : PackBodyReadRow<ContainerProgramViewV3> {
    Owned& owned;
    PackBodyReadState& state;
    bool PrepareCurves(std::uint64_t count) noexcept { return state.Resize(owned.curves, count, kHardPlayablePropertyChildRowCapacity); }
    auto Curves(std::uint64_t i) noexcept { return PackBodyReadCurve{{{}, owned.curves[static_cast<std::size_t>(i)]}}; }
    bool PrepareChildren(std::uint64_t count) noexcept { return state.Resize(owned.children, count, kHardPlayablePropertyChildRowCapacity); }
    auto Children(std::uint64_t i) noexcept { return PackBodyReadChild{{{}, owned.children[static_cast<std::size_t>(i)]}}; }
    bool PrepareDiscreteAxes(std::uint64_t count) noexcept { return state.Resize(owned.discreteAxes, count, kHardPlayablePropertyChildRowCapacity); }
    auto& DiscreteAxes(std::uint64_t i) noexcept { return owned.discreteAxes[static_cast<std::size_t>(i)]; }
    bool PrepareDiscreteRows(std::uint64_t count) noexcept { return state.Resize(owned.discreteRows, count, kHardPlayablePropertyChildRowCapacity) && state.Resize(owned.discreteCells, count, kHardPlayablePropertyChildRowCapacity); }
    auto DiscreteRows(std::uint64_t i) noexcept {
        const auto index = static_cast<std::size_t>(i);
        using Cells = std::remove_reference_t<decltype(owned.discreteCells[index])>;
        return PackBodyReadGrid<DiscreteGridRowViewV1, Cells>{{{}, owned.discreteRows[index]}, owned.discreteCells[index], state};
    }
    bool PrepareContinuousAxes(std::uint64_t count) noexcept { return state.Resize(owned.continuousAxes, count, kHardPlayablePropertyChildRowCapacity); }
    auto& ContinuousAxes(std::uint64_t i) noexcept { return owned.continuousAxes[static_cast<std::size_t>(i)]; }
    bool PrepareContinuousRows(std::uint64_t count) noexcept { return state.Resize(owned.continuousRows, count, kHardPlayablePropertyChildRowCapacity) && state.Resize(owned.continuousCells, count, kHardPlayablePropertyChildRowCapacity); }
    auto ContinuousRows(std::uint64_t i) noexcept {
        const auto index = static_cast<std::size_t>(i);
        using Cells = std::remove_reference_t<decltype(owned.continuousCells[index])>;
        return PackBodyReadGrid<ContinuousGridRowViewV1, Cells>{{{}, owned.continuousRows[index]}, owned.continuousCells[index], state};
    }
    bool Finish() noexcept { owned.Refresh(); return true; }
};

template <class Owned>
struct PackBodyReadTransition : PackBodyReadRow<TransitionProgramViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    bool PreparePath(std::uint64_t count) noexcept {
        return AddCount(static_cast<std::uint32_t>(count), kHardPlayablePropertyChildRowCapacity, state.transitionPath) &&
               state.Resize(owned.downwardPathSteps, count, kHardPlayablePropertyChildRowCapacity);
    }
    auto& Path(std::uint64_t i) noexcept { return owned.downwardPathSteps[static_cast<std::size_t>(i)]; }
    bool PrepareRules(std::uint64_t count) noexcept {
        return AddCount(static_cast<std::uint32_t>(count), kHardPlayablePropertyChildRowCapacity, state.transitionRules) &&
               state.Resize(owned.rules, count, kHardPlayablePropertyChildRowCapacity);
    }
    auto& Rules(std::uint64_t i) noexcept { return owned.rules[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { owned.Refresh(); return true; }
};

template <class Owned>
struct PackBodyReadAction : PackBodyReadRow<ActionProgramViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    bool PrepareSteps(std::uint64_t count) noexcept {
        state.actionSteps += count;
        return count != 0 && count <= kPackBodyActionStepCap && state.actionSteps <= kHardActionStepCapacity &&
               count <= (state.reader.size - state.reader.offset) / kPackBodyWorldActionStepBytes &&
               state.Resize(owned.steps, count, kPackBodyActionStepCap);
    }
    auto& Steps(std::uint64_t i) noexcept { return owned.steps[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { owned.Refresh(); return true; }
};

struct PackBodyReadHeader : PackBodyReadRow<ContainerWorldInstallViewV1> {
    PackBodyReadState& state;
    auto& Prefix() noexcept { return state.prefix; }
    auto& ActionCapacity() noexcept {
        const auto& prefix = state.prefix;
        value.identity.format.schema = prefix.schema;
        value.identity.format.compiler = prefix.compiler;
        value.identity.format.selectionAlgorithm = prefix.selectionAlgorithm;
        value.identity.format.gridAlgorithm = prefix.gridAlgorithm;
        value.identity.format.transitionAlgorithm = prefix.transitionAlgorithm;
        value.identity.format.publicationProtocol = prefix.publicationProtocol;
        value.identity.runtimeSemanticFingerprint = prefix.runtimeSemanticFingerprint;
        value.identity.capabilityMask = prefix.capabilityMask;
        value.variableCount = prefix.variableCount;
        value.playablePropertyCount = prefix.playablePropertyProgramCount;
        value.playableOccurrenceCount = prefix.playableOccurrencePropertyCount;
        value.trackScheduleCount = prefix.trackScheduleCount;
        value.segmentScheduleCount = prefix.segmentScheduleCount;
        value.containerCount = prefix.containerCount;
        value.transitionCount = prefix.transitionProgramCount;
        value.capacity.maxContainerDepth = prefix.maxContainerDepth;
        value.capacity.maxChildrenPerContainer = prefix.maxChildrenPerContainer;
        value.capacity.maxAvoidLastN = prefix.maxAvoidLastN;
        value.capacity.maxDiscreteAxes = prefix.maxDiscreteAxes;
        value.capacity.maxDiscreteRows = prefix.maxDiscreteRows;
        value.capacity.maxDiscreteTrieNodes = prefix.maxDiscreteTrieNodes;
        value.capacity.maxContinuousAxes = prefix.maxContinuousAxes;
        value.capacity.maxContinuousRows = prefix.maxContinuousRows;
        value.capacity.maxContinuousCandidates = prefix.maxContinuousCandidates;
        value.capacity.maxContinuousActiveRows = prefix.maxContinuousActiveRows;
        value.capacity.maxContinuousLiveRows = prefix.maxContinuousLiveRows;
        value.capacity.maxTransitionRules = prefix.maxTransitionRules;
        value.capacity.maxCurvePoints = prefix.maxCurvePoints;
        value.capacity.maxKnownScheduleLeaves = prefix.maxKnownScheduleLeaves;
        value.capacity.maxTrackSchedulePrograms = prefix.maxTrackSchedulePrograms;
        value.capacity.maxSegmentSchedulePrograms = prefix.maxSegmentSchedulePrograms;
        value.capacity.maxScheduleTrackRefsPerSegment = prefix.maxScheduleTrackRefsPerSegment;
        value.capacity.maxScheduleTrackRefsTotal = prefix.maxScheduleTrackRefsTotal;
        value.capacity.maxSchedulePlacementsPerTrack = prefix.maxSchedulePlacementsPerTrack;
        value.capacity.maxSchedulePlacementsTotal = prefix.maxSchedulePlacementsTotal;
        value.capacity.maxScheduleMarkersPerSegment = prefix.maxScheduleMarkersPerSegment;
        value.capacity.maxScheduleMarkersTotal = prefix.maxScheduleMarkersTotal;
        value.capacity.maxTrackSegmentScheduleDepth = prefix.maxTrackSegmentScheduleDepth;
        return value;
    }
    auto& StructuralCount() noexcept { return state.structuralCount; }
    auto& Capacity() noexcept { return state.completeCapacity; }
    auto& ProjectId() noexcept { return value.identity.projectId; }
    bool Finish() noexcept {
        const auto& prefix = state.prefix;
        if (!V9CapacityPrefixMatches(value.capacity, state.completeCapacity)) return false;
        value.capacity = state.completeCapacity;
        return value.variableCount <= kHardVariableDefinitionCapacity &&
               value.playablePropertyCount <= kHardPlayablePropertyProgramCapacity &&
               value.playableOccurrenceCount <= kHardPlayableOccurrencePropertyCapacity &&
               value.trackScheduleCount <= kHardTrackScheduleProgramCapacity &&
               value.segmentScheduleCount <= kHardSegmentScheduleProgramCapacity &&
               value.containerCount <= kHardContainerProgramCapacity &&
               value.transitionCount <= kHardPlayablePropertyChildRowCapacity &&
               value.actionCount <= kHardActionDefinitionCapacity &&
               value.actionCount <= (state.reader.size - state.reader.offset) / kPackBodyWorldActionHeaderBytes &&
               prefix.playablePropertySendCount <= kHardPlayablePropertyChildRowCapacity &&
               prefix.reservedChildCount1 <= kHardPlayablePropertyChildRowCapacity &&
               prefix.reservedChildCount2 <= kHardPlayablePropertyChildRowCapacity &&
               prefix.playablePropertyLayerAncestorRefCount <= kHardPlayablePropertyChildRowCapacity &&
               prefix.playablePropertyEffectNodeCount <= kHardPlayablePropertyChildRowCapacity &&
               prefix.playablePropertyEffectParamCount <= kHardPlayablePropertyEffectParamCapacity &&
               prefix.playablePropertyEffectEdgeCount <= kHardPlayablePropertyEffectEdgeCapacity &&
               state.structuralCount <= kHardPlayablePropertyChildRowCapacity &&
               prefix.scheduleTrackRefCount <= kHardScheduleTrackRefCapacity &&
               prefix.schedulePlacementCount <= kHardSchedulePlacementCapacity &&
               prefix.scheduleMarkerCount <= kHardScheduleMarkerCapacity &&
               value.capacity.maxTrackSchedulePrograms <= kHardTrackScheduleProgramCapacity &&
               value.capacity.maxSegmentSchedulePrograms <= kHardSegmentScheduleProgramCapacity &&
               value.capacity.maxScheduleTrackRefsPerSegment <= 1024 &&
               value.capacity.maxScheduleTrackRefsTotal <= kHardScheduleTrackRefCapacity &&
               value.capacity.maxSchedulePlacementsPerTrack <= 65536 &&
               value.capacity.maxSchedulePlacementsTotal <= kHardSchedulePlacementCapacity &&
               value.capacity.maxScheduleMarkersPerSegment <= 65536 &&
               value.capacity.maxScheduleMarkersTotal <= kHardScheduleMarkerCapacity &&
               value.capacity.maxTrackSegmentScheduleDepth <= 64;
    }
};

struct PackBodyReadVariable {
    VariableDefinitionV1& target;
    PackWorldVariableWire value{};
    auto& Value() noexcept { return value; }
    auto& Header() noexcept { return value; }
    bool Finish() noexcept {
        target.variableId = value.variableId;
        target.kind = static_cast<VariableKindV1>(value.kind);
        target.rangeMin = value.rangeMin;
        target.rangeMax = value.rangeMax;
        target.defaultValue = value.defaultValue;
        return true;
    }
};

template <class Owned>
struct PackBodyReadWorld : PackBodyReadRow<ContainerWorldInstallViewV1> {
    Owned& owned;
    PackBodyReadState& state;
    auto Header() noexcept { return PackBodyReadHeader{{{}, this->value}, state}; }
    bool PrepareVariables(std::uint64_t count) noexcept { return state.Resize(owned.variables, count, kHardVariableDefinitionCapacity); }
    auto Variables(std::uint64_t i) noexcept { return PackBodyReadVariable{owned.variables[static_cast<std::size_t>(i)]}; }
    bool PrepareProperties(std::uint64_t count) noexcept { return state.Resize(owned.properties, count, kHardPlayablePropertyProgramCapacity); }
    auto Properties(std::uint64_t i) noexcept {
        auto& row = owned.properties[static_cast<std::size_t>(i)];
        return PackBodyReadProperty<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool PrepareOccurrences(std::uint64_t count) noexcept { return state.structuralAncestors == state.structuralCount && state.Resize(owned.occurrences, count, kHardPlayableOccurrencePropertyCapacity); }
    auto Occurrences(std::uint64_t i) noexcept {
        auto& row = owned.occurrences[static_cast<std::size_t>(i)];
        return PackBodyReadOccurrence<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool PrepareTrackSchedules(std::uint64_t count) noexcept { return state.Resize(owned.trackSchedules, count, kHardTrackScheduleProgramCapacity); }
    auto TrackSchedules(std::uint64_t i) noexcept {
        auto& row = owned.trackSchedules[static_cast<std::size_t>(i)];
        return PackBodyReadTrack<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool PrepareSegmentSchedules(std::uint64_t count) noexcept { return state.Resize(owned.segmentSchedules, count, kHardSegmentScheduleProgramCapacity); }
    auto SegmentSchedules(std::uint64_t i) noexcept {
        auto& row = owned.segmentSchedules[static_cast<std::size_t>(i)];
        return PackBodyReadSegment<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool PrepareContainers(std::uint64_t count) noexcept { return state.Resize(owned.currentContainers, count, kHardContainerProgramCapacity); }
    auto Containers(std::uint64_t i) noexcept {
        auto& row = owned.currentContainers[static_cast<std::size_t>(i)];
        return PackBodyReadContainer<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool PrepareTransitions(std::uint64_t count) noexcept { return state.Resize(owned.transitions, count, kHardPlayablePropertyChildRowCapacity); }
    auto Transitions(std::uint64_t i) noexcept {
        auto& row = owned.transitions[static_cast<std::size_t>(i)];
        return PackBodyReadTransition<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool PrepareActions(std::uint64_t count) noexcept { return state.Resize(owned.actions, count, kHardActionDefinitionCapacity); }
    auto Actions(std::uint64_t i) noexcept {
        auto& row = owned.actions[static_cast<std::size_t>(i)];
        return PackBodyReadAction<std::remove_reference_t<decltype(row)>>{{{}, row.view}, row, state};
    }
    bool Finish() noexcept {
        const auto& prefix = state.prefix;
        const std::array<std::uint32_t, 6> expected = {
            prefix.playablePropertySendCount, prefix.reservedChildCount1,
            prefix.reservedChildCount2, prefix.playablePropertyEffectNodeCount,
            prefix.playablePropertyEffectParamCount, prefix.playablePropertyEffectEdgeCount};
        return state.payloadTotals == expected && state.ancestors == prefix.playablePropertyLayerAncestorRefCount &&
               state.structuralAncestors == state.structuralCount && state.trackRefs == prefix.scheduleTrackRefCount &&
               state.placements == prefix.schedulePlacementCount && state.markers == prefix.scheduleMarkerCount;
    }
};

struct PackBodyEncodeBudget {
    detail::PackDecodeCharge& charge;
    std::size_t& requestedSlots;
    bool capacity = false;
    void Reserve(std::vector<std::uint8_t>& out, std::size_t size) {
        if (size <= requestedSlots) return;
        detail::PackDecodeCharge replacement(charge.Budget());
        replacement.AddRows<std::uint8_t>(size);
        out.reserve(size);
        charge.Release(detail::DecodeBytes(requestedSlots, sizeof(std::uint8_t)));
        charge.Merge(std::move(replacement));
        requestedSlots = size;
    }
};

}

bool EncodeCurrentWorldBodyImpl(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out,
    bool includeEntryTimeline = false, bool includeSignals = false,
    PackBodyEncodeBudget* budget = nullptr) noexcept {
    try {
        out.clear();
        {
            if (!CountPointer(world.signalRelationCount, world.signalRelations, UINT32_MAX) ||
                !CountPointer(world.signalReceiverCount, world.signalReceivers, UINT32_MAX) ||
                (!includeSignals && (world.signalRelationCount || world.signalReceiverCount)))
                return false;
        }
        if (!CountPointer(world.variableCount, world.variables,
                          kHardVariableDefinitionCapacity) ||
            !CountPointer(world.playablePropertyCount,
                          world.playableProperties,
                          kHardPlayablePropertyProgramCapacity) ||
            !CountPointer(world.playableOccurrenceCount,
                          world.playableOccurrences,
                          kHardPlayableOccurrencePropertyCapacity) ||
            !CountPointer(world.trackScheduleCount, world.trackSchedules,
                          kHardTrackScheduleProgramCapacity) ||
            !CountPointer(world.segmentScheduleCount, world.segmentSchedules,
                          kHardSegmentScheduleProgramCapacity) ||
            !CountPointer(world.containerCount, world.containers,
                           kHardContainerProgramCapacity) ||
            !CountPointer(world.transitionCount, world.transitions,
                           kTraceWorldChildCap) ||
            !CountPointer(world.actionCount, world.actions,
                          kHardActionDefinitionCapacity)) {
            return false;
        }
        std::array<std::uint32_t, 6> totals{};
        std::uint32_t ancestorTotal = 0;
        std::uint32_t structuralAncestorTotal = 0;
        std::uint32_t trackRefTotal = 0;
        std::uint32_t placementTotal = 0;
        std::uint32_t markerTotal = 0;
        for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const auto& row = world.playableProperties[i];
            if (!CountPointer(row.organizationLayerAncestorCount,
                              row.organizationLayerAncestors,
                              kHardPlayablePropertyChildRowCapacity) ||
                !AddCount(row.organizationLayerAncestorCount,
                          kHardPlayablePropertyChildRowCapacity,
                          ancestorTotal) ||
                !CountPointer(row.structuralAncestorCount,
                              row.structuralAncestors,
                              kHardPlayablePropertyChildRowCapacity) ||
                !AddCount(row.structuralAncestorCount,
                          kHardPlayablePropertyChildRowCapacity,
                          structuralAncestorTotal) ||
                !CountPayload(row.payload, totals)) {
                return false;
            }
            {
                for (std::uint32_t a = 0; a < row.structuralAncestorCount; ++a) {
                    std::array<std::uint32_t, 6> ignoredTotals{};
                    if (!CountPayload(row.structuralAncestors[a].payload,
                                      ignoredTotals))
                        return false;
                }
            }
        }
        for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
            if (!CountPayload(world.playableOccurrences[i].payload, totals)) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
            const auto& row = world.trackSchedules[i];
            if (!CountPointer(row.placementCount, row.placements,
                              kHardSchedulePlacementCapacity) ||
                !AddCount(row.placementCount,
                          kHardSchedulePlacementCapacity,
                          placementTotal)) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
            const auto& row = world.segmentSchedules[i];
            if (!includeEntryTimeline &&
                (row.entryMarker.stableMarkerId != VnObjectId{} ||
                 row.entryMarker.position != 0 || row.bounds.contentStart != 0 ||
                 row.bounds.contentEnd != 0 || row.bounds.nominalStart != 0 ||
                 row.bounds.nominalEnd != 0)) return false;
            if (!CountPointer(row.trackRefCount, row.trackRefs,
                              kHardScheduleTrackRefCapacity) ||
                !CountPointer(row.markerCount, row.markers,
                              kHardScheduleMarkerCapacity) ||
                !AddCount(row.trackRefCount, kHardScheduleTrackRefCapacity,
                          trackRefTotal) ||
                !AddCount(row.markerCount, kHardScheduleMarkerCapacity, markerTotal)) {
                return false;
            }
        }
        std::uint32_t transitionRuleTotal = 0;
        std::uint32_t transitionPathTotal = 0;
        for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
            const auto& row = world.transitions[i];
            if (!CountPointer(row.ruleCount, row.rules,
                              kTraceWorldChildCap) ||
                !CountPointer(row.downwardPathStepCount,
                              row.downwardPathSteps,
                              kTraceWorldChildCap) ||
                !AddCount(row.ruleCount, kTraceWorldChildCap,
                          transitionRuleTotal) ||
                !AddCount(row.downwardPathStepCount, kTraceWorldChildCap,
                          transitionPathTotal)) {
                return false;
            }
        }
        std::uint64_t stepTotal = 0;
        for (std::uint32_t i = 0; i < world.actionCount; ++i) {
            const auto& action = world.actions[i];
            if (!CountPointer(action.stepCount, action.steps, kPackBodyActionStepCap) ||
                action.stepCount == 0) {
                return false;
            }
            stepTotal += action.stepCount;
            if (stepTotal > kHardActionStepCapacity) return false;
            for (std::uint32_t s = 0; !includeEntryTimeline && s < action.stepCount; ++s) {
                const auto timing = action.steps[s].quantizeTiming;
                if (timing == ActionQuantizeTimingV1::EntryMarker ||
                    timing == ActionQuantizeTimingV1::ReferenceExitMarker ||
                    timing == ActionQuantizeTimingV1::TargetNextMarker) {
                    return false;
                }
            }
        }
        CurrentWorldV3PrefixValues prefix{};
        if (!TryBuildCurrentWorldV3Prefix(world, false, &prefix) ||
            prefix.playablePropertySendCount != totals[0] ||
            prefix.reservedChildCount1 != totals[1] ||
            prefix.reservedChildCount2 != totals[2] ||
            prefix.playablePropertyLayerAncestorRefCount != ancestorTotal ||
            prefix.playablePropertyEffectNodeCount != totals[3] ||
            prefix.playablePropertyEffectParamCount != totals[4] ||
            prefix.playablePropertyEffectEdgeCount != totals[5] ||
            prefix.scheduleTrackRefCount != trackRefTotal ||
            prefix.schedulePlacementCount != placementTotal ||
            prefix.scheduleMarkerCount != markerTotal) {
            return false;
        }
        PackBodyWriteTrace trace{};
        PackBodyWriteWorld visitor{{{}, world}, trace, prefix, structuralAncestorTotal};
        std::uint64_t byteCount = 0;
        if (!MeasurePackBodyWorldBody(visitor, byteCount)) return false;
        if (includeEntryTimeline) {
            const auto extensionBytes = static_cast<std::uint64_t>(world.segmentScheduleCount) * kPackBodyWorldSegmentEntryTimelineBytes;
            if (extensionBytes > UINT64_MAX - byteCount) return false;
            byteCount += extensionBytes;
        }
        if (byteCount > out.max_size()) return false;
        if (budget != nullptr) budget->Reserve(out, static_cast<std::size_t>(byteCount));
        out.resize(static_cast<std::size_t>(byteCount));
        PackWireBuffer<std::uint8_t> output{out.data(), out.size()};
        if (includeEntryTimeline) trace.output = &output;
        if (!AppendPackBodyWorldBody(output, visitor) || output.offset != out.size()) {
            out.clear();
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        if (budget != nullptr) budget->capacity = true;
        out.clear();
        return false;
    } catch (...) {
        out.clear();
        return false;
    }
}

bool EncodeCurrentWorldShippingBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage ||
        !ValidateCurrentWorldSectionGroupsV1(world)) {
        out.clear();
        return false;
    }
    return EncodeCurrentWorldBodyImpl(world, out);
}

WorldInstallStatus detail::EncodeCurrentWorldIdentityBodyWithBudget(
    const ContainerWorldInstallViewV1& world, std::vector<std::uint8_t>& out,
    PackDecodeCharge& charge, std::size_t& requestedSlots) noexcept {
    out.clear();
    const bool entryScheduling = world.sourceBodyVintage == kCurrentWorldBodyVintageV19;
    if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage && !entryScheduling)
        return WorldInstallStatus::InvalidFormat;
    const auto validated = ValidateCurrentWorldSectionGroupsWithBudget(world, charge.Budget());
    if (validated != WorldInstallStatus::Ok) return validated;
    PackBodyEncodeBudget budget{charge, requestedSlots};
    if (EncodeCurrentWorldBodyImpl(world, out, entryScheduling, false, &budget))
        return WorldInstallStatus::Ok;
    return budget.capacity ? WorldInstallStatus::Capacity : WorldInstallStatus::InvalidWorld;
}

bool EncodeCurrentWorldShippingBodyRawAttempt(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage) {
        out.clear();
        return false;
    }
    return EncodeCurrentWorldBodyImpl(world, out);
}

bool EncodeCurrentWorldBodyV19(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldBodyVintageV19 ||
        !ValidateCurrentWorldSectionGroupsV1(world)) {
        out.clear();
        return false;
    }
    return EncodeCurrentWorldBodyImpl(world, out, true);
}

bool EncodeCurrentWorldBodyV20(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldBodyVintageV20 ||
        !ValidateCurrentWorldSectionGroupsV1(world) ||
        !EncodeCurrentWorldBodyImpl(world, out, false, true)) {
        out.clear();
        return false;
    }
    try {
        const auto offset = out.size();
        const auto relations = std::uint64_t{world.signalRelationCount};
        const auto receivers = std::uint64_t{world.signalReceiverCount};
        out.resize(offset + kPackBodyWorldSignalCountsBytes +
                   relations * kPackBodyWorldSignalRelationBytes +
                   receivers * kPackBodyWorldSignalReceiverBytes);
        auto* cursor = out.data() + offset;
        SerializePackBodyWorldSignalCounts(world, cursor);
        cursor += kPackBodyWorldSignalCountsBytes;
        for (std::uint64_t i = 0; i < relations; ++i, cursor += kPackBodyWorldSignalRelationBytes)
            SerializePackBodyWorldSignalRelation(world.signalRelations[i], cursor);
        for (std::uint64_t i = 0; i < receivers; ++i, cursor += kPackBodyWorldSignalReceiverBytes)
            SerializePackBodyWorldSignalReceiver(world.signalReceivers[i], cursor);
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

CurrentWorldBodyImage::CurrentWorldBodyImage() noexcept = default;

CurrentWorldBodyImage::~CurrentWorldBodyImage() = default;

CurrentWorldBodyDecodeResult CurrentWorldBodyImage::DecodeWithResult(
    const std::uint8_t* bytes, std::size_t byteCount,
    std::uint32_t bodyVintage, CurrentWorldDecodeMode mode) noexcept {
    return DecodeWithBudget(bytes, byteCount, bodyVintage, mode, {});
}

CurrentWorldBodyDecodeResult CurrentWorldBodyImage::DecodeWithBudget(
    const std::uint8_t* bytes, std::size_t byteCount,
    std::uint32_t worldTraceVintage, CurrentWorldDecodeMode mode,
    const detail::PackDecodeBudget& budget) noexcept {
    const bool strictArtifact =
        mode == CurrentWorldDecodeMode::StrictArtifact;
    const bool replayRawAttempt =
        mode == CurrentWorldDecodeMode::ReplayRawAttempt;
    const bool replayCurrentAttempt =
        mode == CurrentWorldDecodeMode::ReplayCurrentAttempt;
    if (!strictArtifact && !replayRawAttempt && !replayCurrentAttempt) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
    if (!IsReadableCurrentWorldBodyVintage(worldTraceVintage) || replayRawAttempt ||
        (replayCurrentAttempt && worldTraceVintage != kCurrentWorldShippingBodyVintage &&
         worldTraceVintage != kCurrentWorldBodyVintageV19 &&
         worldTraceVintage != kCurrentWorldBodyVintageV20)) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
    if (bytes == nullptr && byteCount != 0) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
    try {
        detail::LogicAllocCheckpoint(
            detail::LogicAllocSite::CurrentWorldDecodedImage);
        detail::PackDecodeCharge allocation(budget);
        allocation.AddRows<Impl>(1);
        auto decoded = std::make_unique<Impl>();
        decoded->staging = std::move(allocation);
        Reader reader{bytes, byteCount};
        auto& world = decoded->view;
        world.sourceBodyVintage = worldTraceVintage;
        PackBodyReadState state{reader};
        state.bodyVintage = worldTraceVintage;
        state.staging = &decoded->staging;
        PackBodyReadWorld<Impl> visitor{{{}, world}, *decoded, state};
        if (!ReadPackBodyWorldBody(reader, visitor)) return state.result;
        if (worldTraceVintage == kCurrentWorldBodyVintageV20) {
            const auto* counts = reader.Take(kPackBodyWorldSignalCountsBytes);
            if (counts == nullptr ||
                !ParsePackBodyWorldSignalCounts(counts, kPackBodyWorldSignalCountsBytes, &world))
                return CurrentWorldBodyDecodeResult::Invalid;
            const auto relations = world.signalRelationCount;
            const auto receivers = world.signalReceiverCount;
            const auto tableBytes = std::uint64_t{relations} * kPackBodyWorldSignalRelationBytes +
                std::uint64_t{receivers} * kPackBodyWorldSignalReceiverBytes;
            if (tableBytes != reader.size - reader.offset ||
                relations > decoded->signalRelations.max_size() ||
                receivers > decoded->signalReceivers.max_size())
                return CurrentWorldBodyDecodeResult::Invalid;
            detail::DecodeResize(decoded->signalRelations, relations, decoded->staging);
            detail::DecodeResize(decoded->signalReceivers, receivers, decoded->staging);
            for (auto& row : decoded->signalRelations) {
                const auto* bytes = reader.Take(kPackBodyWorldSignalRelationBytes);
                if (bytes == nullptr ||
                    !ParsePackBodyWorldSignalRelation(bytes, kPackBodyWorldSignalRelationBytes, &row))
                    return CurrentWorldBodyDecodeResult::Invalid;
            }
            for (auto& row : decoded->signalReceivers) {
                const auto* bytes = reader.Take(kPackBodyWorldSignalReceiverBytes);
                if (bytes == nullptr ||
                    !ParsePackBodyWorldSignalReceiver(bytes, kPackBodyWorldSignalReceiverBytes, &row))
                    return CurrentWorldBodyDecodeResult::Invalid;
            }
        }
        if (reader.failed || reader.offset != reader.size) {
            return CurrentWorldBodyDecodeResult::Invalid;
        }
        decoded->Refresh();
        if (strictArtifact) {
            const auto sections = detail::ValidateCurrentWorldSectionGroupsWithBudget(decoded->view, budget);
            if (sections != WorldInstallStatus::Ok)
                return sections == WorldInstallStatus::Capacity
                    ? CurrentWorldBodyDecodeResult::Capacity : CurrentWorldBodyDecodeResult::Invalid;
        }
        impl_ = std::move(decoded);
        return CurrentWorldBodyDecodeResult::Ok;
    } catch (const std::bad_alloc&) {
        return CurrentWorldBodyDecodeResult::Capacity;
    } catch (...) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
}

void CurrentWorldBodyImage::ReleaseDecodeStaging() noexcept {
    if (impl_) impl_->staging.Detach();
}

const ContainerWorldInstallViewV1* CurrentWorldBodyImage::ProductionView() const noexcept {
    return impl_ != nullptr ? &impl_->view : nullptr;
}

const HistoricalContainerWorldViewV1* CurrentWorldBodyImage::ReplayView() const noexcept {
    return nullptr;
}

}
