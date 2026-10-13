#include "harken/param/VariableService.h"

#include "Engine.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/VariableExecutionFingerprint.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TransportEpochOwner.h"
#include "harken/param/BindingTable.h"
#include "harken/param/BusTargetStore.h"
#include "harken/param/EffectParamTargetStore.h"
#include "harken/param/EmitterTable.h"
#include "harken/param/ParamSystem.h"
#include "harken/dsp/GainRamp.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <utility>

namespace harken {

WorldInstallResult InstallContainerWorldV1(
    Engine& engine, const ContainerWorldInstallViewV1& install,
    CurrentWorldTraceOwnerV1 traceOwner) noexcept {
    try {
        const bool hasCurrentConsumers =
            HasCurrentWorldPlayableConsumers(engine);
        WorldInstallResult result =
            engine.variables.InstallWithReplacementAdmission(
                install, !hasCurrentConsumers);
        if (hasCurrentConsumers &&
            result.status == WorldInstallStatus::Backpressure) {
            AdvanceCurrentWorldReplacementDrain(engine);
            (void)CancelActionDeferredForWorldReplacement(
                engine, engine.variables.CurrentWorldGeneration());
            if (HasCurrentWorldPlayableConsumers(engine)) {
                engine.currentWorldReplacementAdmissionClosed = true;
                engine.variables.SetEngineAdmissionClosed(true);
            } else {
                result = engine.variables.InstallWithReplacementAdmission(
                    install, true);
                if (result.status == WorldInstallStatus::Ok &&
                    !result.idempotent) {
                    (void)CancelActionDeferredForWorldReplacement(
                        engine, result.priorGeneration);
                    engine.queueCheckpoints.ClearWorld(result.priorGeneration);
                }
                engine.currentWorldReplacementAdmissionClosed = false;
                engine.variables.SetEngineAdmissionClosed(false);
            }
        } else if (result.status == WorldInstallStatus::Ok &&
                   !result.idempotent) {
            (void)CancelActionDeferredForWorldReplacement(
                engine, result.priorGeneration);
            engine.queueCheckpoints.ClearWorld(result.priorGeneration);
            engine.currentWorldReplacementAdmissionClosed = false;
            engine.variables.SetEngineAdmissionClosed(false);
            ObserveMonitorCurrentWorld(result.resultGeneration);
        }
        engine.statefulCallSeen = true;
        if (engine.trace == nullptr || !engine.trace->Active()) {
            engine.traceOriginWorldProbeOnly =
                engine.traceOriginWorldProbeOnly &&
                result.status == WorldInstallStatus::Ok;
        }
        if (result.status == WorldInstallStatus::Ok && !result.idempotent) {
            ReclaimRetiredSelectionMemory(engine);
            engine.variables.ProjectCurrentWorld(
                engine.params, engine.emitters, engine.bindings,
                engine.busTargets);
        }
        if (traceOwner == CurrentWorldTraceOwnerV1::Internal &&
            engine.trace != nullptr && engine.trace->Active()) {
            const std::uint32_t encoding =
                result.status == WorldInstallStatus::Ok ? 0u : 1u;
            engine.trace->RecordCurrentWorldInstall(install, result, encoding);
        }
        return result;
    } catch (...) {
        WorldInstallResult result{};
        result.status = WorldInstallStatus::Capacity;
        result.priorGeneration = engine.variables.CurrentWorldGeneration();
        return result;
    }
}

WorldInstallResult InstallContainerWorldV2Replay(
    Engine& engine, const HistoricalContainerWorldViewV1& install) noexcept {
    try {
        return engine.variables.InstallReplayV2(install);
    } catch (...) {
        WorldInstallResult result{};
        result.status = WorldInstallStatus::Capacity;
        result.priorGeneration = engine.variables.CurrentWorldGeneration();
        return result;
    }
}

WorldInstallResult InstallContainerWorldV3Replay(
    Engine& engine, const HistoricalContainerWorldViewV1& install) noexcept {
    try {
        return engine.variables.InstallReplayV3(install);
    } catch (...) {
        WorldInstallResult result{};
        result.status = WorldInstallStatus::Capacity;
        result.priorGeneration = engine.variables.CurrentWorldGeneration();
        return result;
    }
}

WorldInstallResult InstallContainerWorldV4Replay(
    Engine& engine, const HistoricalContainerWorldViewV1& install) noexcept {
    try {
        return engine.variables.InstallReplayV4(install);
    } catch (...) {
        WorldInstallResult result{};
        result.status = WorldInstallStatus::Capacity;
        result.priorGeneration = engine.variables.CurrentWorldGeneration();
        return result;
    }
}

WorldInstallResult InstallContainerWorldV1(
    const ContainerWorldInstallViewV1& install) noexcept {
    Engine* engine = detail::ActiveEngine();
    if (engine == nullptr) {
        WorldInstallResult result{};
        result.status = WorldInstallStatus::Backpressure;
        return result;
    }
    return InstallContainerWorldV1(*engine, install);
}

VariableServiceV1::VariableServiceV1(
    std::uint32_t maxEmitters, std::uint32_t overridesPerEmitter,
    std::uint32_t maxPropertiesPerCurrentSlice,
    std::uint32_t maxActionDefinitions,
    LogicCommitCoordinator& commits, TransportEpochOwner& transport,
    VariableSliceTable& slices, ScheduledTransactionPool& timeline)
    : maxEmitters_(maxEmitters),
      overridesPerEmitter_(overridesPerEmitter),
      maxActionDefinitions_(maxActionDefinitions),
      censusScratch_(maxPropertiesPerCurrentSlice <=
                             kVariableSliceTargetHardLimit
                         ? maxPropertiesPerCurrentSlice
                         : 0),
      commits_(commits),
      transport_(transport), slices_(slices), timeline_(timeline) {
    definitions_.reserve(kVariableDefinitionCapacity);
    emitterBanks_.resize(maxEmitters_);
    emitterBankVersionGenerations_.resize(maxEmitters_, 1);
    for (EmitterBank& bank : emitterBanks_) {
        bank.overrides.resize(overridesPerEmitter_);
    }
    history_.resize(kVariableHistoryCapacity);
    freeSlots_.reserve(kVariableHistoryCapacity);
    for (std::uint32_t slot = kVariableHistoryCapacity; slot > 0; --slot) {
        freeSlots_.push_back(slot - 1);
    }
    outcomes_.reserve(kVariableHistoryCapacity);
    ticketQueryRecords_.assign(kVariableHistoryCapacity, TicketQueryRecord{});
    expiredTicketRecords_.assign(kVariableHistoryCapacity, VnVariableTicketV1{});
}

TransportEpoch VariableServiceV1::CurrentTransportEpoch() const noexcept {
    return transport_.Current();
}

bool VariableServiceV1::SameFormat(const WorldFormatIdentityV1& a,
                                   const WorldFormatIdentityV1& b) noexcept {
    return a.schema == b.schema && a.compiler == b.compiler &&
           a.selectionAlgorithm == b.selectionAlgorithm &&
           a.gridAlgorithm == b.gridAlgorithm &&
           a.transitionAlgorithm == b.transitionAlgorithm &&
           a.publicationProtocol == b.publicationProtocol;
}

bool VariableServiceV1::SameDefinition(const VariableDefinitionV1& a,
                                       const VariableDefinitionV1& b) noexcept {
    return a.variableId == b.variableId && a.kind == b.kind &&
           a.rangeMin == b.rangeMin && a.rangeMax == b.rangeMax &&
           a.defaultValue == b.defaultValue;
}

bool VariableServiceV1::SameDefinition(const DefinitionRow& a,
                                       const DefinitionRow& b) noexcept {
    return SameDefinition(a.definition, b.definition) && a.writers == b.writers &&
           a.executionFingerprint == b.executionFingerprint;
}

bool VariableServiceV1::ExactStepValue(float value) noexcept {
    return std::isfinite(value) && value >= -kStepExactIntegerLimit &&
           value <= kStepExactIntegerLimit && value == std::floor(value);
}

bool VariableServiceV1::ValidDefinition(
    const VariableDefinitionV1& definition) noexcept {
    if (definition.variableId == VnObjectId{} ||
        (definition.kind != VariableKindV1::Step &&
         definition.kind != VariableKindV1::Smooth) ||
        !std::isfinite(definition.rangeMin) ||
        !std::isfinite(definition.rangeMax) ||
        definition.rangeMin > definition.rangeMax ||
        !std::isfinite(definition.defaultValue) ||
        definition.defaultValue < definition.rangeMin ||
        definition.defaultValue > definition.rangeMax) {
        return false;
    }
    return definition.kind != VariableKindV1::Step ||
           ExactStepValue(definition.defaultValue);
}

bool VariableServiceV1::SemanticIdentical(
    const ContainerWorldInstallViewV1& install) const {
    if (!hasWorld_ ||
        install.identity.runtimeSemanticFingerprint !=
            identity_.runtimeSemanticFingerprint ||
        !SameFormat(install.identity.format, identity_.format) ||
        install.identity.capabilityMask != identity_.capabilityMask) {
        return false;
    }
    return true;
}

void VariableServiceV1::OwnedPayload::Refresh() noexcept {
    view.sendCount = static_cast<std::uint32_t>(sends.size());
    view.sends = sends.empty() ? nullptr : sends.data();
    view.effectNodeCount = static_cast<std::uint32_t>(effectNodes.size());
    view.effectNodes = effectNodes.empty() ? nullptr : effectNodes.data();
    for (std::size_t i = 0; i < effectNodes.size(); ++i) {
        effectNodes[i].paramCount =
            static_cast<std::uint32_t>(effectParams[i].size());
        effectNodes[i].params =
            effectParams[i].empty() ? nullptr : effectParams[i].data();
    }
    view.effectEdgeCount = static_cast<std::uint32_t>(effectEdges.size());
    view.effectEdges = effectEdges.empty() ? nullptr : effectEdges.data();
}

void VariableServiceV1::OwnedPropertyProgram::Refresh() noexcept {
    payload.Refresh();
    view.organizationLayerAncestorCount =
        static_cast<std::uint32_t>(layerAncestors.size());
    view.organizationLayerAncestors =
        layerAncestors.empty() ? nullptr : layerAncestors.data();
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

void VariableServiceV1::OwnedOccurrenceProgram::Refresh() noexcept {
    payload.Refresh();
    view.payload = payload.view;
}

void VariableServiceV1::OwnedTrackScheduleProgram::Refresh() noexcept {
    view.placementCount = static_cast<std::uint32_t>(placements.size());
    view.placements = placements.empty() ? nullptr : placements.data();
}

void VariableServiceV1::OwnedPieceScheduleProgram::Refresh() noexcept {
    view.trackRefCount = static_cast<std::uint32_t>(trackRefs.size());
    view.trackRefs = trackRefs.empty() ? nullptr : trackRefs.data();
    view.markerCount = static_cast<std::uint32_t>(markers.size());
    view.markers = markers.empty() ? nullptr : markers.data();
}

void VariableServiceV1::OwnedTransitionProgram::Refresh() noexcept {
    view.downwardPathStepCount =
        static_cast<std::uint32_t>(downwardPathSteps.size());
    view.downwardPathSteps = downwardPathSteps.empty()
        ? nullptr : downwardPathSteps.data();
    view.ruleCount = static_cast<std::uint32_t>(rules.size());
    view.rules = rules.empty() ? nullptr : rules.data();
}

void VariableServiceV1::OwnedActionProgram::Refresh() noexcept {
    view.stepCount = static_cast<std::uint32_t>(steps.size());
    view.steps = steps.empty() ? nullptr : steps.data();
}

void VariableServiceV1::OwnedContainerProgram::Refresh() noexcept {
    view.childCount = static_cast<std::uint32_t>(children.size());
    view.children = children.empty() ? nullptr : children.data();
    view.curveCount = static_cast<std::uint32_t>(curves.size());
    view.curves = curves.empty() ? nullptr : curves.data();

    view.discreteAxisCount = static_cast<std::uint32_t>(discreteAxes.size());
    view.discreteAxes = discreteAxes.empty() ? nullptr : discreteAxes.data();
    view.discreteRowCount = static_cast<std::uint32_t>(discreteRows.size());
    view.discreteRows = discreteRows.empty() ? nullptr : discreteRows.data();
    for (std::size_t i = 0; i < discreteRows.size(); ++i) {
        discreteRows[i].cellCount =
            static_cast<std::uint32_t>(discreteCells[i].size());
        discreteRows[i].cells =
            discreteCells[i].empty() ? nullptr : discreteCells[i].data();
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
        continuousRows[i].cells =
            continuousCells[i].empty() ? nullptr : continuousCells[i].data();
    }
}

WorldInstallResult VariableServiceV1::Install(
    const ContainerWorldInstallViewV1& install) {
    return InstallImpl(install, 0, true);
}

WorldInstallResult VariableServiceV1::InstallWithReplacementAdmission(
    const ContainerWorldInstallViewV1& install,
    bool replacementAdmitted) {
    return InstallImpl(install, 0, replacementAdmitted);
}

WorldInstallResult VariableServiceV1::InstallReplayV2(
    const HistoricalContainerWorldViewV1& install) {
    return InstallHistoricalReplay(install, 2);
}

WorldInstallResult VariableServiceV1::InstallReplayV3(
    const HistoricalContainerWorldViewV1& install) {
    return InstallHistoricalReplay(install, 3);
}

WorldInstallResult VariableServiceV1::InstallReplayV4(
    const HistoricalContainerWorldViewV1& install) {
    return InstallHistoricalReplay(install, 4);
}

WorldInstallResult VariableServiceV1::InstallHistoricalReplay(
    const HistoricalContainerWorldViewV1& install,
    std::uint32_t replayVintage) {
    WorldInstallResult result{};
    result.priorGeneration = worldGeneration_;
    result.resultGeneration = 0;
    const WorldInstallStatus worldStatus = replayVintage == 2
        ? ValidateCurrentWorldV2Replay(install)
        : replayVintage == 3 ? ValidateCurrentWorldV3Replay(install)
                             : ValidateCurrentWorldV4Replay(install);
    result.status = worldStatus;
    return result;
}

WorldInstallResult VariableServiceV1::InstallImpl(
    const ContainerWorldInstallViewV1& installInput,
    std::uint32_t replayVintage, bool replacementAdmitted) {
    WorldInstallResult result{};
    result.priorGeneration = worldGeneration_;
    result.resultGeneration = 0;

    const WorldInstallStatus worldStatus =
        ValidateCurrentWorldV1(installInput);
    if (worldStatus != WorldInstallStatus::Ok) {
        result.status = worldStatus;
        return result;
    }
    const ContainerWorldInstallViewV1& install = installInput;
    if (install.actionCount > maxActionDefinitions_) {
        result.status = WorldInstallStatus::Capacity;
        return result;
    }
    if (install.variableCount > kVariableDefinitionCapacity ||
        install.variableCount > install.capacity.maxVariableDefinitions ||
        install.capacity.maxVariableDefinitions > kVariableDefinitionCapacity) {
        result.status = WorldInstallStatus::Capacity;
        return result;
    }
    if (install.variableCount != 0 && install.variables == nullptr) {
        result.status = WorldInstallStatus::InvalidWorld;
        return result;
    }
    for (std::uint32_t i = 0; i < install.variableCount; ++i) {
        if (!ValidDefinition(install.variables[i]) ||
            (i != 0 && install.variables[i - 1].variableId >=
                           install.variables[i].variableId)) {
            result.status = WorldInstallStatus::InvalidWorld;
            return result;
        }
    }
    if (SemanticIdentical(install)) {
        result.status = WorldInstallStatus::Ok;
        result.resultGeneration = worldGeneration_;
        result.idempotent = true;
        return result;
    }
    if (!replacementAdmitted) {
        result.status = WorldInstallStatus::Backpressure;
        return result;
    }
    if (DrainingTransport()) {
        result.status = WorldInstallStatus::Backpressure;
        return result;
    }
    if (worldGeneration_ == UINT64_MAX) {
        result.status = WorldInstallStatus::GenerationExhausted;
        return result;
    }
    bool outstanding = slices_.HasUnreclaimedWorldRows(worldGeneration_);
    for (WriteRow& row : history_) {
        if (!row.used) {
            continue;
        }
        if ((row.state == RowState::Pending ||
             (row.state == RowState::WaitingFrontier &&
              row.noPhysicalTargets)) &&
            !row.slice.Valid()) {
            FinalizeRow(row, VariableOutcomeKind::WorldReplacement, {}, 0);
            continue;
        }
        if (row.state != RowState::Terminal) {
            outstanding = true;
        }
    }
    if (outstanding) {
        drainState_ = WorldDrainState::DrainingCurrent;
        result.status = WorldInstallStatus::Backpressure;
        return result;
    }
    drainState_ = WorldDrainState::Idle;

    std::vector<DefinitionRow> next;
    next.reserve(install.variableCount);
    for (std::uint32_t i = 0; i < install.variableCount; ++i) {
        DefinitionRow row{};
        row.definition = install.variables[i];
        row.definition.impact = StaticImpactSummary{};
        row.writers = 3;
        row.executionFingerprint = 0;
        row.currentGlobal = row.definition.defaultValue;
        row.scheduledGlobal = row.definition.defaultValue;
        const DefinitionRow* prior = FindDefinition(row.definition.variableId);
        if (prior != nullptr && SameDefinition(*prior, row)) {
            row.currentGlobal = prior->currentGlobal;
            row.scheduledGlobal = prior->currentGlobal;
            row.currentBankOrdinal = prior->currentBankOrdinal;
            row.scheduledBankOrdinal = prior->currentBankOrdinal;
        }
        next.push_back(row);
    }

    const auto copyPayload = [](const PlayablePropertyPayloadViewV1& source,
                                OwnedPayload& destination) {
        destination.view = source;
        if (source.sendCount != 0) {
            destination.sends.assign(source.sends,
                                     source.sends + source.sendCount);
        }
        if (source.effectNodeCount != 0) {
            destination.effectNodes.assign(
                source.effectNodes, source.effectNodes + source.effectNodeCount);
        }
        destination.effectParams.resize(source.effectNodeCount);
        for (std::uint32_t i = 0; i < source.effectNodeCount; ++i) {
            if (source.effectNodes[i].paramCount != 0) {
                destination.effectParams[i].assign(
                    source.effectNodes[i].params,
                    source.effectNodes[i].params +
                        source.effectNodes[i].paramCount);
            }
        }
        if (source.effectEdgeCount != 0) {
            destination.effectEdges.assign(
                source.effectEdges,
                source.effectEdges + source.effectEdgeCount);
        }
        destination.Refresh();
    };

    std::vector<OwnedPropertyProgram> nextProperties;
    nextProperties.resize(install.playablePropertyCount);
    for (std::uint32_t i = 0; i < install.playablePropertyCount; ++i) {
        auto& destination = nextProperties[i];
        const auto& source = install.playableProperties[i];
        destination.view = source;
        if (source.organizationLayerAncestorCount != 0) {
            destination.layerAncestors.assign(
                source.organizationLayerAncestors,
                source.organizationLayerAncestors +
                    source.organizationLayerAncestorCount);
        }
        if (source.structuralAncestorCount != 0) {
            destination.structuralAncestors.assign(
                source.structuralAncestors,
                source.structuralAncestors + source.structuralAncestorCount);
            destination.structuralPayloads.resize(source.structuralAncestorCount);
            for (std::uint32_t a = 0; a < source.structuralAncestorCount; ++a)
                copyPayload(source.structuralAncestors[a].payload,
                            destination.structuralPayloads[a]);
        }
        copyPayload(source.payload, destination.payload);
        destination.Refresh();
    }

    std::vector<OwnedOccurrenceProgram> nextOccurrences;
    nextOccurrences.resize(install.playableOccurrenceCount);
    for (std::uint32_t i = 0; i < install.playableOccurrenceCount; ++i) {
        auto& destination = nextOccurrences[i];
        destination.view = install.playableOccurrences[i];
        copyPayload(install.playableOccurrences[i].payload,
                    destination.payload);
        destination.Refresh();
    }

    std::vector<OwnedTrackScheduleProgram> nextTrackSchedules;
    nextTrackSchedules.resize(install.trackScheduleCount);
    for (std::uint32_t i = 0; i < install.trackScheduleCount; ++i) {
        auto& destination = nextTrackSchedules[i];
        const auto& source = install.trackSchedules[i];
        destination.view = source;
        if (source.placementCount != 0) {
            destination.placements.assign(
                source.placements, source.placements + source.placementCount);
        }
        destination.Refresh();
    }

    std::vector<OwnedPieceScheduleProgram> nextPieceSchedules;
    nextPieceSchedules.resize(install.pieceScheduleCount);
    for (std::uint32_t i = 0; i < install.pieceScheduleCount; ++i) {
        auto& destination = nextPieceSchedules[i];
        const auto& source = install.pieceSchedules[i];
        destination.view = source;
        if (source.trackRefCount != 0) {
            destination.trackRefs.assign(
                source.trackRefs, source.trackRefs + source.trackRefCount);
        }
        if (source.markerCount != 0) {
            destination.markers.assign(source.markers,
                                    source.markers + source.markerCount);
        }
        destination.Refresh();
    }

    std::vector<OwnedContainerProgram> nextContainers;
    nextContainers.resize(install.containerCount);
    for (std::uint32_t i = 0; i < install.containerCount; ++i) {
        auto& destination = nextContainers[i];
        const auto& source = install.containers[i];
        destination.view = source;
        if (source.childCount != 0) {
            destination.children.assign(source.children,
                                        source.children + source.childCount);
        }

        if (source.curveCount != 0) {
            destination.curves.assign(source.curves,
                                      source.curves + source.curveCount);
        }
        if (source.discreteAxisCount != 0) {
            destination.discreteAxes.assign(
                source.discreteAxes,
                source.discreteAxes + source.discreteAxisCount);
        }
        destination.discreteRows.resize(source.discreteRowCount);
        destination.discreteCells.resize(source.discreteRowCount);
        for (std::uint32_t row = 0; row < source.discreteRowCount; ++row) {
            destination.discreteRows[row] = source.discreteRows[row];
            if (source.discreteRows[row].cellCount != 0) {
                destination.discreteCells[row].assign(
                    source.discreteRows[row].cells,
                    source.discreteRows[row].cells +
                        source.discreteRows[row].cellCount);
            }
        }
        if (source.continuousAxisCount != 0) {
            destination.continuousAxes.assign(
                source.continuousAxes,
                source.continuousAxes + source.continuousAxisCount);
        }
        destination.continuousRows.resize(source.continuousRowCount);
        destination.continuousCells.resize(source.continuousRowCount);
        for (std::uint32_t row = 0; row < source.continuousRowCount; ++row) {
            destination.continuousRows[row] = source.continuousRows[row];
            if (source.continuousRows[row].cellCount != 0) {
                destination.continuousCells[row].assign(
                    source.continuousRows[row].cells,
                    source.continuousRows[row].cells +
                        source.continuousRows[row].cellCount);
            }
        }
        destination.Refresh();
    }

    std::vector<OwnedTransitionProgram> nextTransitions;
    nextTransitions.resize(install.transitionCount);
    for (std::uint32_t i = 0; i < install.transitionCount; ++i) {
        auto& destination = nextTransitions[i];
        const auto& source = install.transitions[i];
        destination.view = source;
        if (source.downwardPathStepCount != 0) {
            destination.downwardPathSteps.assign(
                source.downwardPathSteps,
                source.downwardPathSteps + source.downwardPathStepCount);
        }
        if (source.ruleCount != 0) {
            destination.rules.assign(source.rules,
                                     source.rules + source.ruleCount);
        }
        destination.Refresh();
    }

    std::vector<OwnedActionProgram> nextActions;
    nextActions.resize(install.actionCount);
    for (std::uint32_t i = 0; i < install.actionCount; ++i) {
        auto& destination = nextActions[i];
        const auto& source = install.actions[i];
        destination.view = source;
        if (source.stepCount != 0) {
            destination.steps.assign(source.steps,
                                     source.steps + source.stepCount);
        }
        destination.Refresh();
    }

    for (WriteRow& row : history_) {
        if (row.used && row.state != RowState::Terminal &&
            row.world == worldGeneration_) {
            FinalizeRow(row, VariableOutcomeKind::WorldReplacement, {}, 0);
        }
    }
    for (EmitterBank& bank : emitterBanks_) {
        for (OverrideRow& override : bank.overrides) {
            if (!override.used || override.currentBankOrdinal == 0) {
                override = OverrideRow{};
                continue;
            }
            const auto nextDef = std::find_if(
                next.begin(), next.end(), [&](const DefinitionRow& row) {
                    return row.definition.variableId == override.variableId;
                });
            const DefinitionRow* prior = FindDefinition(override.variableId);
            if (nextDef == next.end() || prior == nullptr ||
                !SameDefinition(*prior, *nextDef)) {
                override = OverrideRow{};
            } else {
                override.scheduledValue = override.currentValue;
                override.scheduledBankOrdinal = override.currentBankOrdinal;
            }
        }
    }

    definitions_ = std::move(next);
    sdkCatalogFingerprint_ = 0;
    labelCatalog_.clear();
    definitionViews_.clear();
    definitionViews_.reserve(definitions_.size());
    for (const DefinitionRow& row : definitions_) {
        definitionViews_.push_back(row.definition);
    }
    playableProperties_ = std::move(nextProperties);
    playableOccurrences_ = std::move(nextOccurrences);
    trackSchedules_ = std::move(nextTrackSchedules);
    pieceSchedules_ = std::move(nextPieceSchedules);
    containerPrograms_ = std::move(nextContainers);
    transitionPrograms_ = std::move(nextTransitions);
    actionPrograms_ = std::move(nextActions);
    playablePropertyViews_.clear();
    playablePropertyViews_.reserve(playableProperties_.size());
    for (auto& row : playableProperties_) {
        row.Refresh();
        playablePropertyViews_.push_back(row.view);
    }
    playableOccurrenceViews_.clear();
    playableOccurrenceViews_.reserve(playableOccurrences_.size());
    for (auto& row : playableOccurrences_) {
        row.Refresh();
        playableOccurrenceViews_.push_back(row.view);
    }
    trackScheduleViews_.clear();
    trackScheduleViews_.reserve(trackSchedules_.size());
    for (auto& row : trackSchedules_) {
        row.Refresh();
        trackScheduleViews_.push_back(row.view);
    }
    pieceScheduleViews_.clear();
    pieceScheduleViews_.reserve(pieceSchedules_.size());
    for (auto& row : pieceSchedules_) {
        row.Refresh();
        pieceScheduleViews_.push_back(row.view);
    }
    containerProgramViews_.clear();
    containerProgramViews_.reserve(containerPrograms_.size());
    for (auto& row : containerPrograms_) {
        row.Refresh();
        containerProgramViews_.push_back(row.view);
    }
    transitionProgramViews_.clear();
    transitionProgramViews_.reserve(transitionPrograms_.size());
    for (auto& row : transitionPrograms_) {
        row.Refresh();
        transitionProgramViews_.push_back(row.view);
    }
    actionProgramViews_.clear();
    actionProgramViews_.reserve(actionPrograms_.size());
    for (auto& row : actionPrograms_) {
        row.Refresh();
        actionProgramViews_.push_back(row.view);
    }
    identity_ = install.identity;
    capacity_ = install.capacity;
    ++worldGeneration_;
    globalBankVersionGeneration_ = 1;
    std::fill(emitterBankVersionGenerations_.begin(),
              emitterBankVersionGenerations_.end(), 1u);
    hasWorld_ = true;
    result.status = WorldInstallStatus::Ok;
    result.resultGeneration = worldGeneration_;
    return result;
}

WorldInstallStatus VariableServiceV1::PrepareCurrentWorldImageV1(
    const ContainerWorldInstallViewV1& installInput,
    PreparedCurrentWorldImageV1& out) {
    out.Clear();
    out.source_ = this;
    out.sourceWorld_ = worldGeneration_;
    out.sourceBankOrdinal_ = nextBankOrdinal_;
    const WorldInstallStatus worldStatus = ValidateCurrentWorldV1(installInput);
    if (worldStatus != WorldInstallStatus::Ok) {
        return worldStatus;
    }
    const ContainerWorldInstallViewV1& install = installInput;
    if (install.actionCount > maxActionDefinitions_) {
        return WorldInstallStatus::Capacity;
    }
    if (install.variableCount > kVariableDefinitionCapacity ||
        install.variableCount > install.capacity.maxVariableDefinitions ||
        install.capacity.maxVariableDefinitions > kVariableDefinitionCapacity) {
        return WorldInstallStatus::Capacity;
    }
    if (install.variableCount != 0 && install.variables == nullptr) {
        return WorldInstallStatus::InvalidWorld;
    }
    for (std::uint32_t i = 0; i < install.variableCount; ++i) {
        if (!ValidDefinition(install.variables[i]) ||
            (i != 0 && install.variables[i - 1].variableId >=
                           install.variables[i].variableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
    }
    if (SemanticIdentical(install)) {
        out.valid_ = true;
        out.idempotent_ = true;
        return WorldInstallStatus::Ok;
    }

    std::vector<DefinitionRow> next;
    next.reserve(install.variableCount);
    for (std::uint32_t i = 0; i < install.variableCount; ++i) {
        DefinitionRow row{};
        row.definition = install.variables[i];
        row.definition.impact = StaticImpactSummary{};
        row.writers = 3;
        row.executionFingerprint = 0;
        row.currentGlobal = row.definition.defaultValue;
        row.scheduledGlobal = row.definition.defaultValue;
        const DefinitionRow* prior = FindDefinition(row.definition.variableId);
        if (prior != nullptr && SameDefinition(*prior, row)) {
            row.currentGlobal = prior->currentGlobal;
            row.scheduledGlobal = prior->currentGlobal;
            row.currentBankOrdinal = prior->currentBankOrdinal;
            row.scheduledBankOrdinal = prior->currentBankOrdinal;
        }
        next.push_back(row);
    }

    const auto copyPayload = [](const PlayablePropertyPayloadViewV1& source,
                                OwnedPayload& destination) {
        destination.view = source;
        if (source.sendCount != 0) {
            destination.sends.assign(source.sends, source.sends + source.sendCount);
        }
        if (source.effectNodeCount != 0) {
            destination.effectNodes.assign(
                source.effectNodes, source.effectNodes + source.effectNodeCount);
        }
        destination.effectParams.resize(source.effectNodeCount);
        for (std::uint32_t i = 0; i < source.effectNodeCount; ++i) {
            if (source.effectNodes[i].paramCount != 0) {
                destination.effectParams[i].assign(
                    source.effectNodes[i].params,
                    source.effectNodes[i].params +
                        source.effectNodes[i].paramCount);
            }
        }
        if (source.effectEdgeCount != 0) {
            destination.effectEdges.assign(
                source.effectEdges, source.effectEdges + source.effectEdgeCount);
        }
        destination.Refresh();
    };

    std::vector<OwnedPropertyProgram> nextProperties;
    nextProperties.resize(install.playablePropertyCount);
    for (std::uint32_t i = 0; i < install.playablePropertyCount; ++i) {
        auto& destination = nextProperties[i];
        const auto& source = install.playableProperties[i];
        destination.view = source;
        if (source.organizationLayerAncestorCount != 0) {
            destination.layerAncestors.assign(
                source.organizationLayerAncestors,
                source.organizationLayerAncestors +
                    source.organizationLayerAncestorCount);
        }
        if (source.structuralAncestorCount != 0) {
            destination.structuralAncestors.assign(
                source.structuralAncestors,
                source.structuralAncestors + source.structuralAncestorCount);
            destination.structuralPayloads.resize(source.structuralAncestorCount);
            for (std::uint32_t a = 0; a < source.structuralAncestorCount; ++a) {
                copyPayload(source.structuralAncestors[a].payload,
                            destination.structuralPayloads[a]);
            }
        }
        copyPayload(source.payload, destination.payload);
        destination.Refresh();
    }

    std::vector<OwnedOccurrenceProgram> nextOccurrences;
    nextOccurrences.resize(install.playableOccurrenceCount);
    for (std::uint32_t i = 0; i < install.playableOccurrenceCount; ++i) {
        auto& destination = nextOccurrences[i];
        destination.view = install.playableOccurrences[i];
        copyPayload(install.playableOccurrences[i].payload, destination.payload);
        destination.Refresh();
    }

    std::vector<OwnedTrackScheduleProgram> nextTrackSchedules;
    nextTrackSchedules.resize(install.trackScheduleCount);
    for (std::uint32_t i = 0; i < install.trackScheduleCount; ++i) {
        auto& destination = nextTrackSchedules[i];
        const auto& source = install.trackSchedules[i];
        destination.view = source;
        if (source.placementCount != 0) {
            destination.placements.assign(
                source.placements, source.placements + source.placementCount);
        }
        destination.Refresh();
    }

    std::vector<OwnedPieceScheduleProgram> nextPieceSchedules;
    nextPieceSchedules.resize(install.pieceScheduleCount);
    for (std::uint32_t i = 0; i < install.pieceScheduleCount; ++i) {
        auto& destination = nextPieceSchedules[i];
        const auto& source = install.pieceSchedules[i];
        destination.view = source;
        if (source.trackRefCount != 0) {
            destination.trackRefs.assign(
                source.trackRefs, source.trackRefs + source.trackRefCount);
        }
        if (source.markerCount != 0) {
            destination.markers.assign(source.markers, source.markers + source.markerCount);
        }
        destination.Refresh();
    }

    std::vector<OwnedContainerProgram> nextContainers;
    nextContainers.resize(install.containerCount);
    for (std::uint32_t i = 0; i < install.containerCount; ++i) {
        auto& destination = nextContainers[i];
        const auto& source = install.containers[i];
        destination.view = source;
        if (source.childCount != 0) {
            destination.children.assign(source.children,
                                        source.children + source.childCount);
        }
        if (source.curveCount != 0) {
            destination.curves.assign(source.curves,
                                      source.curves + source.curveCount);
        }
        if (source.discreteAxisCount != 0) {
            destination.discreteAxes.assign(
                source.discreteAxes, source.discreteAxes + source.discreteAxisCount);
        }
        destination.discreteRows.resize(source.discreteRowCount);
        destination.discreteCells.resize(source.discreteRowCount);
        for (std::uint32_t row = 0; row < source.discreteRowCount; ++row) {
            destination.discreteRows[row] = source.discreteRows[row];
            if (source.discreteRows[row].cellCount != 0) {
                destination.discreteCells[row].assign(
                    source.discreteRows[row].cells,
                    source.discreteRows[row].cells +
                        source.discreteRows[row].cellCount);
            }
        }
        if (source.continuousAxisCount != 0) {
            destination.continuousAxes.assign(
                source.continuousAxes,
                source.continuousAxes + source.continuousAxisCount);
        }
        destination.continuousRows.resize(source.continuousRowCount);
        destination.continuousCells.resize(source.continuousRowCount);
        for (std::uint32_t row = 0; row < source.continuousRowCount; ++row) {
            destination.continuousRows[row] = source.continuousRows[row];
            if (source.continuousRows[row].cellCount != 0) {
                destination.continuousCells[row].assign(
                    source.continuousRows[row].cells,
                    source.continuousRows[row].cells +
                        source.continuousRows[row].cellCount);
            }
        }
        destination.Refresh();
    }

    std::vector<OwnedTransitionProgram> nextTransitions;
    nextTransitions.resize(install.transitionCount);
    for (std::uint32_t i = 0; i < install.transitionCount; ++i) {
        auto& destination = nextTransitions[i];
        const auto& source = install.transitions[i];
        destination.view = source;
        if (source.downwardPathStepCount != 0) {
            destination.downwardPathSteps.assign(
                source.downwardPathSteps,
                source.downwardPathSteps + source.downwardPathStepCount);
        }
        if (source.ruleCount != 0) {
            destination.rules.assign(source.rules, source.rules + source.ruleCount);
        }
        destination.Refresh();
    }

    std::vector<OwnedActionProgram> nextActions;
    nextActions.resize(install.actionCount);
    for (std::uint32_t i = 0; i < install.actionCount; ++i) {
        auto& destination = nextActions[i];
        const auto& source = install.actions[i];
        destination.view = source;
        if (source.stepCount != 0) {
            destination.steps.assign(source.steps, source.steps + source.stepCount);
        }
        destination.Refresh();
    }

    out.definitions_ = std::move(next);
    out.definitionViews_.reserve(out.definitions_.size());
    for (const DefinitionRow& row : out.definitions_) {
        out.definitionViews_.push_back(row.definition);
    }
    out.playableProperties_ = std::move(nextProperties);
    out.playableOccurrences_ = std::move(nextOccurrences);
    out.trackSchedules_ = std::move(nextTrackSchedules);
    out.pieceSchedules_ = std::move(nextPieceSchedules);
    out.containerPrograms_ = std::move(nextContainers);
    out.transitionPrograms_ = std::move(nextTransitions);
    out.actionPrograms_ = std::move(nextActions);
    out.playablePropertyViews_.reserve(out.playableProperties_.size());
    for (auto& row : out.playableProperties_) {
        row.Refresh();
        out.playablePropertyViews_.push_back(row.view);
    }
    out.playableOccurrenceViews_.reserve(out.playableOccurrences_.size());
    for (auto& row : out.playableOccurrences_) {
        row.Refresh();
        out.playableOccurrenceViews_.push_back(row.view);
    }
    out.trackScheduleViews_.reserve(out.trackSchedules_.size());
    for (auto& row : out.trackSchedules_) {
        row.Refresh();
        out.trackScheduleViews_.push_back(row.view);
    }
    out.pieceScheduleViews_.reserve(out.pieceSchedules_.size());
    for (auto& row : out.pieceSchedules_) {
        row.Refresh();
        out.pieceScheduleViews_.push_back(row.view);
    }
    out.containerProgramViews_.reserve(out.containerPrograms_.size());
    for (auto& row : out.containerPrograms_) {
        row.Refresh();
        out.containerProgramViews_.push_back(row.view);
    }
    out.transitionProgramViews_.reserve(out.transitionPrograms_.size());
    for (auto& row : out.transitionPrograms_) {
        row.Refresh();
        out.transitionProgramViews_.push_back(row.view);
    }
    out.actionProgramViews_.reserve(out.actionPrograms_.size());
    for (auto& row : out.actionPrograms_) {
        row.Refresh();
        out.actionProgramViews_.push_back(row.view);
    }
    out.identity_ = install.identity;
    out.capacity_ = install.capacity;
    out.valid_ = true;
    out.idempotent_ = false;
    return WorldInstallStatus::Ok;
}

bool VariableServiceV1::CanCommitPreparedCurrentWorldImageV1(
    const PreparedCurrentWorldImageV1& prepared) const noexcept {
    return prepared.valid_ && prepared.source_ == this &&
        prepared.sourceWorld_ == worldGeneration_ &&
        prepared.sourceBankOrdinal_ == nextBankOrdinal_ &&
        (prepared.idempotent_ || worldGeneration_ != UINT64_MAX);
}

ContainerWorldInstallViewV1 VariableServiceV1::PreparedWorldInstallViewV1(
    const PreparedCurrentWorldImageV1& prepared) const noexcept {
    if (!CanCommitPreparedCurrentWorldImageV1(prepared)) return {};
    if (prepared.idempotent_) return CurrentWorldInstallView();
    ContainerWorldInstallViewV1 view{};
    view.identity = prepared.identity_;
    view.capacity = prepared.capacity_;
    view.variables = prepared.definitionViews_.data();
    view.variableCount = static_cast<std::uint32_t>(prepared.definitionViews_.size());
    view.playableProperties = prepared.playablePropertyViews_.data();
    view.playablePropertyCount = static_cast<std::uint32_t>(prepared.playablePropertyViews_.size());
    view.playableOccurrences = prepared.playableOccurrenceViews_.data();
    view.playableOccurrenceCount = static_cast<std::uint32_t>(prepared.playableOccurrenceViews_.size());
    view.trackSchedules = prepared.trackScheduleViews_.data();
    view.trackScheduleCount = static_cast<std::uint32_t>(prepared.trackScheduleViews_.size());
    view.pieceSchedules = prepared.pieceScheduleViews_.data();
    view.pieceScheduleCount = static_cast<std::uint32_t>(prepared.pieceScheduleViews_.size());
    view.containers = prepared.containerProgramViews_.data();
    view.containerCount = static_cast<std::uint32_t>(prepared.containerProgramViews_.size());
    view.transitions = prepared.transitionProgramViews_.data();
    view.transitionCount = static_cast<std::uint32_t>(prepared.transitionProgramViews_.size());
    view.actions = prepared.actionProgramViews_.data();
    view.actionCount = static_cast<std::uint32_t>(prepared.actionProgramViews_.size());
    return view;
}

bool VariableServiceV1::PrepareWorldProjectionV1(
    const PreparedCurrentWorldImageV1& image, const EmitterTable& emitters,
    PreparedWorldProjectionV1* out) const {
    if (out == nullptr || out->IsPrepared()) return false;
    out->Clear();
    if (!CanCommitPreparedCurrentWorldImageV1(image)) return false;
    const auto& definitions = image.idempotent_ ? definitions_ : image.definitions_;
    if (definitions.size() > kMaxVariables) return false;
    detail::LogicAllocCheckpoint(detail::LogicAllocSite::ProjectGlobalProjection);
    out->globals_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        if (!std::isfinite(definition.currentGlobal)) return false;
        out->globals_.push_back({definition.definition.variableId, definition.currentGlobal});
    }
    const auto validRows = [](auto& rows) {
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!std::isfinite(rows[i].value) || (i != 0 && rows[i - 1].id == rows[i].id)) return false;
        }
        return true;
    };
    if (!validRows(out->globals_)) return false;
    detail::LogicAllocCheckpoint(detail::LogicAllocSite::ProjectEmitterProjection);
    out->emitters_.resize(emitterBanks_.size());
    for (std::size_t i = 0; i < emitterBanks_.size(); ++i) {
        const auto& bank = emitterBanks_[i];
        auto& batch = out->emitters_[i];
        batch.bankLive = bank.live;
        batch.generation = bank.generation;
        if (!bank.live) continue;
        batch.handle = (static_cast<std::uint64_t>(bank.generation) << 32) | i;
        batch.emitterLive = emitters.IsLive(batch.handle);
        if (!batch.emitterLive) continue;
        for (const auto& value : bank.overrides) {
            if (value.used && value.currentBankOrdinal != 0) {
                batch.values.push_back({value.variableId, value.currentValue});
            }
        }
        if (batch.values.size() > emitters.LocalValueCapacity(batch.handle) ||
            !validRows(batch.values)) return false;
    }
    out->sourceWorld_ = worldGeneration_;
    out->sourceBankOrdinal_ = nextBankOrdinal_;
    out->owner_ = this;
    return true;
}

bool VariableServiceV1::CanCommitWorldProjectionV1(
    const PreparedWorldProjectionV1& prepared, const EmitterTable& emitters) const noexcept {
    if (prepared.owner_ != this || prepared.sourceWorld_ != worldGeneration_ ||
        prepared.sourceBankOrdinal_ != nextBankOrdinal_ ||
        prepared.emitters_.size() != emitterBanks_.size()) return false;
    for (std::size_t i = 0; i < emitterBanks_.size(); ++i) {
        const auto& bank = emitterBanks_[i];
        const auto& batch = prepared.emitters_[i];
        if (bank.live != batch.bankLive || bank.generation != batch.generation) return false;
        if (batch.bankLive && (emitters.IsLive(batch.handle) != batch.emitterLive ||
            (batch.emitterLive && batch.values.size() > emitters.LocalValueCapacity(batch.handle)))) return false;
    }
    return true;
}

void VariableServiceV1::CommitWorldProjectionV1(
    PreparedWorldProjectionV1* prepared, ParamSystem& params, EmitterTable& emitters,
    BindingTable& bindings, BusTargetStore& busTargets) noexcept {
    assert(prepared != nullptr && prepared->owner_ == this);
    params.ClearInstalledValues();
    for (const auto& value : prepared->globals_) {
        const auto status = params.SetInstalledValue(value.id, value.value);
        assert(status == VN_OK);
        (void)status;
        bindings.NoteBusVariableWrite(value.id, busTargets);
    }
    for (std::size_t i = 0; i < prepared->emitters_.size(); ++i) {
        const auto& batch = prepared->emitters_[i];
        if (!batch.bankLive) continue;
        if (!batch.emitterLive) {
            emitterBanks_[i].live = false;
            continue;
        }
        const auto cleared = emitters.ClearLocalValues(batch.handle);
        assert(cleared == VN_OK);
        (void)cleared;
        for (const auto& value : batch.values) {
            const auto status = emitters.SetLocalValue(batch.handle, value.id, value.value);
            assert(status == VN_OK);
            (void)status;
        }
    }
    prepared->Clear();
}

WorldGeneration VariableServiceV1::CommitPreparedCurrentWorldImageV1(
    PreparedCurrentWorldImageV1& prepared) noexcept {
    if (!prepared.valid_) {
        return worldGeneration_;
    }
    if (prepared.idempotent_) {
        prepared.Clear();
        return worldGeneration_;
    }
    definitions_.swap(prepared.definitions_);
    definitionViews_.swap(prepared.definitionViews_);
    playableProperties_.swap(prepared.playableProperties_);
    playablePropertyViews_.swap(prepared.playablePropertyViews_);
    playableOccurrences_.swap(prepared.playableOccurrences_);
    playableOccurrenceViews_.swap(prepared.playableOccurrenceViews_);
    trackSchedules_.swap(prepared.trackSchedules_);
    trackScheduleViews_.swap(prepared.trackScheduleViews_);
    pieceSchedules_.swap(prepared.pieceSchedules_);
    pieceScheduleViews_.swap(prepared.pieceScheduleViews_);
    containerPrograms_.swap(prepared.containerPrograms_);
    containerProgramViews_.swap(prepared.containerProgramViews_);
    transitionPrograms_.swap(prepared.transitionPrograms_);
    transitionProgramViews_.swap(prepared.transitionProgramViews_);
    actionPrograms_.swap(prepared.actionPrograms_);
    actionProgramViews_.swap(prepared.actionProgramViews_);
    identity_ = prepared.identity_;
    capacity_ = prepared.capacity_;
    ++worldGeneration_;
    globalBankVersionGeneration_ = 1;
    std::fill(emitterBankVersionGenerations_.begin(),
              emitterBankVersionGenerations_.end(), 1u);
    hasWorld_ = true;
    prepared.Clear();
    return worldGeneration_;
}

const VariableDefinitionV1* VariableServiceV1::Definition(
    VnVariableId id) const noexcept {
    const DefinitionRow* row = FindDefinition(id);
    return row != nullptr ? &row->definition : nullptr;
}

VariableServiceV1::DefinitionRow* VariableServiceV1::FindDefinition(
    VnVariableId id) noexcept {
    for (DefinitionRow& row : definitions_) {
        if (row.definition.variableId == id) {
            return &row;
        }
    }
    return nullptr;
}

const VariableServiceV1::DefinitionRow* VariableServiceV1::FindDefinition(
    VnVariableId id) const noexcept {
    for (const DefinitionRow& row : definitions_) {
        if (row.definition.variableId == id) {
            return &row;
        }
    }
    return nullptr;
}

bool VariableServiceV1::ScopeEqual(const VariableScopeKey& a,
                                   const VariableScopeKey& b) noexcept {
    return a.scope == b.scope && a.emitterSlot == b.emitterSlot &&
           a.emitterGeneration == b.emitterGeneration;
}

bool VariableServiceV1::RowKeyEqual(const WriteRow& a,
                                    const WriteRow& b) noexcept {
    return a.world == b.world && a.transport == b.transport &&
           ScopeEqual(a.scope, b.scope) && a.variableId == b.variableId &&
           a.requestedFrame == b.requestedFrame;
}

bool VariableServiceV1::SameBankKey(const WriteRow& a,
                                    const WriteRow& b) noexcept {
    return a.world == b.world && a.transport == b.transport &&
           ScopeEqual(a.scope, b.scope) && a.variableId == b.variableId;
}

bool VariableServiceV1::SameServiceDependency(
    const WriteRow& a, const WriteRow& b) noexcept {
    if (SameBankKey(a, b)) {
        return true;
    }
    if (a.world != b.world || a.transport != b.transport ||
        a.variableId != b.variableId) {
        return false;
    }
    const bool aGlobal = a.scope.scope == VariableScopeV1::Global;
    const bool bGlobal = b.scope.scope == VariableScopeV1::Global;
    return aGlobal != bGlobal;
}

bool VariableServiceV1::BatchSupersedeKeyIntersect(const WriteRow& a,
                                                   const WriteRow& b) noexcept {
    if (a.variableId != b.variableId) {
        return false;
    }
    const bool aGlobal = a.scope.scope == VariableScopeV1::Global;
    const bool bGlobal = b.scope.scope == VariableScopeV1::Global;
    if (aGlobal && bGlobal) {
        return true;
    }
    if (!aGlobal && !bGlobal) {
        return ScopeEqual(a.scope, b.scope);
    }
    return false;
}

bool VariableServiceV1::SameTarget(const VariableTargetLocator& a,
                                   const VariableTargetLocator& b) noexcept {
    return a.scope == b.scope && a.object == b.object &&
           a.objectGeneration == b.objectGeneration &&
           a.targetId == b.targetId && a.effectSlot == b.effectSlot &&
           a.paramIndex == b.paramIndex && a.effectType == b.effectType;
}

VariableAcceptResult VariableServiceV1::Prepare(
    WriteOperation operation, const VariableScopeKey& scope, VnVariableId id,
    float value, const VariableCallObservation& observed,
    std::uint32_t executionOrdinal, PreparedVariableWriteToken* out,
    VariableWriterIdentityV1 writer) noexcept {
    VariableAcceptResult result{};
    result.observation = observed;
    result.observation.world = worldGeneration_;
    result.observation.transport = transport_.Current();
    if (out == nullptr || out->Valid()) {
        result.result = VN_INVALID_ARG;
        ++refusalCount_;
        return result;
    }
    if (!hasWorld_) {
        result.result = VN_NO_VARIABLE;
        ++refusalCount_;
        return result;
    }
    if (AdmissionClosed() || nextBankOrdinal_ == 0 ||
        preparedRows_ > UINT64_MAX - nextBankOrdinal_) {
        result.result = VN_BACKPRESSURE;
        ++refusalCount_;
        return result;
    }
    if (observed.world != worldGeneration_ ||
        observed.transport != transport_.Current()) {
        result.result = VN_BACKPRESSURE;
        ++refusalCount_;
        return result;
    }
    const DefinitionRow* definition = FindDefinition(id);
    if (definition == nullptr) {
        result.result = VN_NO_VARIABLE;
        ++refusalCount_;
        return result;
    }
    if (definition->writers == 0) {
        result.result = VN_VARIABLE_POLICY_INCOMPLETE;
        ++refusalCount_;
        return result;
    }
    if ((definition->writers & static_cast<std::uint8_t>(writer)) == 0) {
        result.result = VN_VARIABLE_WRITER_FORBIDDEN;
        ++refusalCount_;
        return result;
    }
    float storedValue = value;
    if (operation == WriteOperation::Reset) {
        if (scope.scope != VariableScopeV1::Global) {
            result.result = VN_INVALID_ARG;
            ++refusalCount_;
            return result;
        }
        storedValue = definition->definition.defaultValue;
    } else if (operation == WriteOperation::Set) {
        const bool valid = definition->definition.kind == VariableKindV1::Step
                               ? ExactStepValue(value)
                               : std::isfinite(value);
        if (!valid || value < definition->definition.rangeMin ||
            value > definition->definition.rangeMax) {
            result.result = VN_INVALID_ARG;
            ++refusalCount_;
            return result;
        }
    }
    const auto bankGenerationAvailable = [&]() noexcept {
        if (scope.scope == VariableScopeV1::Global) {
            if (globalBankVersionGeneration_ == 0 ||
                globalBankVersionGeneration_ == UINT32_MAX) {
                return false;
            }
            for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
                if (emitterBanks_[slot].generation != 0 &&
                    (emitterBankVersionGenerations_[slot] == 0 ||
                     emitterBankVersionGenerations_[slot] == UINT32_MAX)) {
                    return false;
                }
            }
            return true;
        }
        return scope.emitterSlot < emitterBankVersionGenerations_.size() &&
            emitterBankVersionGenerations_[scope.emitterSlot] != 0 &&
            emitterBankVersionGenerations_[scope.emitterSlot] != UINT32_MAX;
    };
    if (!bankGenerationAvailable()) {
        result.result = VN_BACKPRESSURE;
        ++refusalCount_;
        return result;
    }
    if (freeSlots_.empty() ||
        pendingRows_ + preparedRows_ >= kPendingVariableWriteCapacity) {
        result.result = VN_VARIABLE_TRANSACTION_EXHAUSTED;
        ++refusalCount_;
        return result;
    }
    std::size_t emitterOverrideSlot = overridesPerEmitter_;
    if (operation == WriteOperation::Set &&
        scope.scope == VariableScopeV1::Emitter) {
        const EmitterBank* bank = FindEmitterBank(scope);
        if (bank != nullptr) {
            for (std::size_t i = 0; i < bank->overrides.size(); ++i) {
                if (bank->overrides[i].used &&
                    bank->overrides[i].variableId == id) {
                    emitterOverrideSlot = i;
                    break;
                }
            }
        }
        if (emitterOverrideSlot == overridesPerEmitter_) {
            for (const WriteRow& prepared : history_) {
                if (!prepared.used &&
                    prepared.emitterOverrideSlot != UINT32_MAX &&
                    ScopeEqual(prepared.scope, scope) &&
                    prepared.variableId == id) {
                    emitterOverrideSlot = prepared.emitterOverrideSlot;
                    break;
                }
            }
        }
        if (emitterOverrideSlot == overridesPerEmitter_) {
            for (std::size_t i = 0; i < overridesPerEmitter_; ++i) {
                if (bank != nullptr && bank->overrides[i].used) {
                    continue;
                }
                bool reserved = false;
                for (const WriteRow& prepared : history_) {
                    if (!prepared.used &&
                        prepared.emitterOverrideSlot == i &&
                        ScopeEqual(prepared.scope, scope)) {
                        reserved = true;
                        break;
                    }
                }
                if (!reserved) {
                    emitterOverrideSlot = i;
                    break;
                }
            }
        }
        if (emitterOverrideSlot == overridesPerEmitter_) {
            result.result = VN_VARIABLE_EMITTER_SLOT_EXHAUSTED;
            ++refusalCount_;
            return result;
        }
    }
    WriteRow row{};
    row.operation = operation;
    row.world = worldGeneration_;
    row.transport = transport_.Current();
    row.scope = scope;
    row.variableId = id;
    row.value = storedValue;
    row.requestedFrame = observed.requestedEffectiveFrame;
    row.executionOrdinal = executionOrdinal;
    if (emitterOverrideSlot != overridesPerEmitter_) {
        row.emitterOverrideSlot =
            static_cast<std::uint32_t>(emitterOverrideSlot);
    }
    const std::uint32_t slot = freeSlots_.back();
    freeSlots_.pop_back();
    history_[slot] = row;
    ++preparedRows_;
    out->owner_ = this;
    out->slot_ = slot;
    out->prepared_ = true;
    result.result = VN_OK;
    return result;
}

void VariableServiceV1::CommitPreparedWrite(
    PreparedVariableWriteToken* prepared,
    LogicCommitSequence sequence) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || prepared->slot_ >= history_.size() ||
        sequence == 0) {
        return;
    }
    WriteRow& row = history_[prepared->slot_];
    if (row.used) {
        return;
    }
    const LogicCommitTicket ticket{sequence};
    row.used = true;
    row.ticket = ticket;
    row.bankOrdinal = nextBankOrdinal_++;
    for (WriteRow& prior : history_) {
        if (&prior != &row && prior.used &&
            prior.state == RowState::Pending && RowKeyEqual(prior, row) &&
            CanSupersede(prior, row, sequence)) {
            FinalizeBatch(prior, VariableOutcomeKind::Superseded, ticket, 0);
        }
    }
    if (row.emitterOverrideSlot != UINT32_MAX) {
        EmitterBank* bank = EnsureEmitterBank(row.scope);
        OverrideRow& override = bank->overrides[row.emitterOverrideSlot];
        if (!override.used) {
            override = OverrideRow{};
            override.used = true;
            override.variableId = row.variableId;
        }
        ++override.pendingSetCount;
    }
    --preparedRows_;
    ++pendingRows_;
    historyHighWater_ = std::max<std::uint64_t>(
        historyHighWater_, kVariableHistoryCapacity - freeSlots_.size());
    ++acceptedCount_;
    RememberAcceptedTicket(sequence, row.world, row.transport);
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->prepared_ = false;
}

void VariableServiceV1::CancelPreparedWrite(
    PreparedVariableWriteToken* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || prepared->slot_ >= history_.size()) {
        return;
    }
    WriteRow& row = history_[prepared->slot_];
    if (row.used) {
        return;
    }
    row = WriteRow{};
    freeSlots_.push_back(prepared->slot_);
    --preparedRows_;
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->prepared_ = false;
}

PreparedVariableBatchToken::PreparedVariableBatchToken(
    PreparedVariableBatchToken&& other) noexcept
    : owner_(other.owner_), count_(other.count_),
      committed_(other.committed_) {
    for (std::uint32_t i = 0; i < count_; ++i) {
        rows_[i] = std::move(other.rows_[i]);
    }
    other.owner_ = nullptr;
    other.count_ = 0;
    other.committed_ = false;
}

PreparedVariableBatchToken& PreparedVariableBatchToken::operator=(
    PreparedVariableBatchToken&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (owner_ != nullptr && !committed_) {
        for (std::uint32_t i = 0; i < count_; ++i) {
            owner_->CancelPreparedWrite(&rows_[i]);
        }
    }
    owner_ = other.owner_;
    count_ = other.count_;
    committed_ = other.committed_;
    for (std::uint32_t i = 0; i < count_; ++i) {
        rows_[i] = std::move(other.rows_[i]);
    }
    other.owner_ = nullptr;
    other.count_ = 0;
    other.committed_ = false;
    return *this;
}

PreparedVariableBatchToken::~PreparedVariableBatchToken() {
    if (owner_ == nullptr || committed_) {
        return;
    }
    for (std::uint32_t i = 0; i < count_; ++i) {
        owner_->CancelPreparedWrite(&rows_[i]);
    }
}

bool VariableServiceV1::CanSupersede(
    WriteRow& prior, const WriteRow& next,
    LogicCommitSequence sequence) noexcept {
    if (prior.world != next.world || prior.transport != next.transport ||
        prior.requestedFrame != next.requestedFrame ||
        prior.ticket.sequence >= sequence) {
        return false;
    }
    const auto members = BatchRows(prior);
    for (const WriteRow& other : history_) {
        if (other.used && other.state != RowState::Terminal &&
            other.requestedFrame == prior.requestedFrame &&
            other.ticket.sequence > prior.ticket.sequence &&
            other.ticket.sequence < sequence &&
            !ScopeEqual(prior.scope, other.scope)) {
            for (const WriteRow* member : members) {
                if (member != nullptr && SameServiceDependency(*member, other))
                    return false;
            }
        }
    }
    return true;
}

void VariableServiceV1::SupersedeIntersectingPendingBatches(
    const PreparedVariableBatchToken& batch,
    LogicCommitSequence sequence) noexcept {
    const LogicCommitTicket related{sequence};
    for (WriteRow& prior : history_) {
        if (!prior.used || prior.state != RowState::Pending) {
            continue;
        }
        bool hit = false;
        for (std::uint32_t i = 0; i < batch.count_; ++i) {
            const WriteRow& next = history_[batch.rows_[i].slot_];
            if (BatchSupersedeKeyIntersect(prior, next) &&
                CanSupersede(prior, next, sequence)) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            continue;
        }
        const std::uint64_t batchId = prior.batchId;
        if (batchId == 0) {
            FinalizeRow(prior, VariableOutcomeKind::Superseded, related, 0);
            continue;
        }
        for (WriteRow& row : history_) {
            if (row.used && row.state == RowState::Pending &&
                row.batchId == batchId) {
                FinalizeRow(row, VariableOutcomeKind::Superseded, related, 0);
            }
        }
    }
}

VariableAcceptResult VariableServiceV1::AcceptBatch(
    WriteOperation operation, const VariableScopeKey& scope,
    const VnVariableWriteV1* writes, std::uint32_t writeCount,
    const VariableCallObservation& observed,
    VariableWriterIdentityV1 writer, const EmitterTable* emitters,
    bool wholeBatchSupersede) {
    VariableAcceptResult result{};
    result.observation = observed;
    result.observation.world = worldGeneration_;
    result.observation.transport = transport_.Current();
    if (writes == nullptr || writeCount == 0 ||
        writeCount > kVariableBatchWriteCapacity) {
        result.result = VN_INVALID_ARG;
        ++refusalCount_;
        return result;
    }
    for (std::uint32_t i = 0; i < writeCount; ++i) {
        if (writes[i].reserved0 != 0) {
            result.result = VN_INVALID_ARG;
            ++refusalCount_;
            return result;
        }
        if (operation == WriteOperation::Reset &&
            writes[i].value != 0.0f) {
            result.result = VN_INVALID_ARG;
            ++refusalCount_;
            return result;
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (writes[i].variableId == writes[j].variableId) {
                result.result = VN_INVALID_ARG;
                ++refusalCount_;
                return result;
            }
        }
    }
    if (scope.scope == VariableScopeV1::Emitter) {
        if (emitters == nullptr) {
            result.result = VN_INVALID_ARG;
            ++refusalCount_;
            return result;
        }
        const VnEmitterHandle handle =
            (static_cast<std::uint64_t>(scope.emitterGeneration) << 32) |
            scope.emitterSlot;
        if (!emitters->IsLive(handle)) {
            result.result = VN_STALE_HANDLE;
            ++refusalCount_;
            return result;
        }
    }
    PreparedVariableBatchToken batch;
    batch.owner_ = this;
    for (std::uint32_t i = 0; i < writeCount; ++i) {
        VariableAcceptResult row = Prepare(
            operation, scope, writes[i].variableId, writes[i].value, observed,
            0, &batch.rows_[i], writer);
        if (row.result != VN_OK) {
            batch.count_ = i;
            result.result = row.result;
            result.observation = row.observation;
            return result;
        }
        batch.count_ = i + 1;
    }
    LogicCommitSequence sequence = 0;
    if (commits_.TryIssue(&sequence) !=
        LogicCommitCoordinator::IssueStatus::Issued) {
        result.result = VN_BACKPRESSURE;
        ++refusalCount_;
        return result;
    }
    for (std::uint32_t i = 0; i < batch.count_; ++i) {
        history_[batch.rows_[i].slot_].batchId = batch.count_ > 1 ? sequence : 0;
        history_[batch.rows_[i].slot_].batchLeader = i == 0;
    }
    if (wholeBatchSupersede) {
        SupersedeIntersectingPendingBatches(batch, sequence);
    }
    for (std::uint32_t i = 0; i < batch.count_; ++i) {
        CommitPreparedWrite(&batch.rows_[i], sequence);
    }
    batch.committed_ = true;
    batch.owner_ = nullptr;
    batch.count_ = 0;
    result.result = VN_OK;
    result.ticket.sequence = sequence;
    return result;
}

VariableAcceptResult VariableServiceV1::Accept(
    WriteOperation operation, const VariableScopeKey& scope, VnVariableId id,
    float value, const VariableCallObservation& observed,
    VariableWriterIdentityV1 writer) {
    VnVariableWriteV1 write{};
    write.variableId = id;
    write.value = value;
    return AcceptBatch(operation, scope, &write, 1, observed, writer, nullptr,
                       false);
}

VariableAcceptResult VariableServiceV1::PrepareGlobalWrite(
    VnVariableId id, float value, const VariableCallObservation& observed,
    std::uint32_t executionOrdinal, PreparedVariableWriteToken* out,
    VariableWriterIdentityV1 writer) noexcept {
    VariableScopeKey scope{};
    return Prepare(WriteOperation::Set, scope, id, value, observed,
                   executionOrdinal, out, writer);
}

VariableAcceptResult VariableServiceV1::PrepareEmitterWrite(
    VnEmitterHandle emitter, VnVariableId id, float value,
    const VariableCallObservation& observed,
    std::uint32_t executionOrdinal, const EmitterTable& emitters,
    PreparedVariableWriteToken* out, VariableWriterIdentityV1 writer) noexcept {
    if (emitter == 0 || !emitters.IsLive(emitter)) {
        VariableAcceptResult result{};
        result.result = emitter == 0 ? VN_INVALID_ARG : VN_STALE_HANDLE;
        result.observation = observed;
        ++refusalCount_;
        return result;
    }
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    return Prepare(WriteOperation::Set, scope, id, value, observed,
                   executionOrdinal, out, writer);
}

VariableAcceptResult VariableServiceV1::AcceptGlobal(
    VnVariableId id, float value, const VariableCallObservation& observed,
    ParamSystem& /*params*/, BindingTable& /*bindings*/,
    BusTargetStore& /*busTargets*/) {
    VnVariableWriteV1 write{};
    write.variableId = id;
    write.value = value;
    VariableScopeKey scope{};
    return AcceptBatch(WriteOperation::Set, scope, &write, 1, observed,
                       VariableWriterIdentityV1::Game, nullptr, false);
}

VariableAcceptResult VariableServiceV1::AcceptEmitter(
    VnEmitterHandle emitter, VnVariableId id, float value,
    const VariableCallObservation& observed, const EmitterTable& emitters,
    ParamSystem& /*params*/) {
    VnVariableWriteV1 write{};
    write.variableId = id;
    write.value = value;
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    return AcceptBatch(WriteOperation::Set, scope, &write, 1, observed,
                       VariableWriterIdentityV1::Game, &emitters, false);
}

VariableAcceptResult VariableServiceV1::AcceptGlobalBatch(
    const VnVariableWriteV1* writes, std::uint32_t writeCount,
    const VariableCallObservation& observed, ParamSystem& /*params*/,
    BindingTable& /*bindings*/, BusTargetStore& /*busTargets*/) {
    VariableScopeKey scope{};
    return AcceptBatch(WriteOperation::Set, scope, writes, writeCount, observed,
                       VariableWriterIdentityV1::Game, nullptr);
}

VariableAcceptResult VariableServiceV1::AcceptGlobalResetBatch(
    const VnVariableWriteV1* writes, std::uint32_t writeCount,
    const VariableCallObservation& observed, ParamSystem& /*params*/,
    BindingTable& /*bindings*/, BusTargetStore& /*busTargets*/) {
    VariableScopeKey scope{};
    return AcceptBatch(WriteOperation::Reset, scope, writes, writeCount,
                       observed, VariableWriterIdentityV1::Game, nullptr);
}

VariableAcceptResult VariableServiceV1::AcceptEmitterBatch(
    VnEmitterHandle emitter, const VnVariableWriteV1* writes,
    std::uint32_t writeCount, const VariableCallObservation& observed,
    const EmitterTable& emitters, ParamSystem& /*params*/) {
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    return AcceptBatch(WriteOperation::Set, scope, writes, writeCount, observed,
                       VariableWriterIdentityV1::Game, &emitters);
}

VariableServiceV1::EmitterBank* VariableServiceV1::FindEmitterBank(
    const VariableScopeKey& scope) noexcept {
    if (scope.scope != VariableScopeV1::Emitter ||
        scope.emitterSlot >= emitterBanks_.size()) {
        return nullptr;
    }
    EmitterBank& bank = emitterBanks_[scope.emitterSlot];
    return bank.live && bank.generation == scope.emitterGeneration ? &bank
                                                                   : nullptr;
}

const VariableServiceV1::EmitterBank* VariableServiceV1::FindEmitterBank(
    const VariableScopeKey& scope) const noexcept {
    if (scope.scope != VariableScopeV1::Emitter ||
        scope.emitterSlot >= emitterBanks_.size()) {
        return nullptr;
    }
    const EmitterBank& bank = emitterBanks_[scope.emitterSlot];
    return bank.live && bank.generation == scope.emitterGeneration ? &bank
                                                                   : nullptr;
}

VariableServiceV1::EmitterBank* VariableServiceV1::EnsureEmitterBank(
    const VariableScopeKey& scope) noexcept {
    if (scope.scope != VariableScopeV1::Emitter ||
        scope.emitterSlot >= emitterBanks_.size()) {
        return nullptr;
    }
    EmitterBank& bank = emitterBanks_[scope.emitterSlot];
    if (!bank.live || bank.generation != scope.emitterGeneration) {
        bank.live = true;
        bank.generation = scope.emitterGeneration;
        for (OverrideRow& override : bank.overrides) {
            override = OverrideRow{};
        }
    }
    return &bank;
}

VariableServiceV1::OverrideRow* VariableServiceV1::FindOverride(
    EmitterBank& bank, VnVariableId id) noexcept {
    for (OverrideRow& row : bank.overrides) {
        if (row.used && row.variableId == id) {
            return &row;
        }
    }
    return nullptr;
}

const VariableServiceV1::OverrideRow* VariableServiceV1::FindOverride(
    const EmitterBank& bank, VnVariableId id) const noexcept {
    for (const OverrideRow& row : bank.overrides) {
        if (row.used && row.variableId == id) {
            return &row;
        }
    }
    return nullptr;
}

const VariableServiceV1::OverrideRow*
VariableServiceV1::FindCurrentOverride(const EmitterBank& bank,
                                       VnVariableId id) const noexcept {
    const OverrideRow* row = FindOverride(bank, id);
    return row != nullptr && row->currentBankOrdinal != 0 ? row : nullptr;
}

VariableServiceV1::OverrideRow* VariableServiceV1::EnsureOverride(
    EmitterBank& bank, VnVariableId id) noexcept {
    if (OverrideRow* existing = FindOverride(bank, id)) {
        return existing;
    }
    for (OverrideRow& row : bank.overrides) {
        if (!row.used) {
            row = OverrideRow{};
            row.used = true;
            row.variableId = id;
            return &row;
        }
    }
    return nullptr;
}

std::array<VariableServiceV1::WriteRow*, kVariableBatchWriteCapacity>
VariableServiceV1::BatchRows(WriteRow& row) noexcept {
    std::array<WriteRow*, kVariableBatchWriteCapacity> rows{};
    if (row.batchId == 0) {
        rows[0] = &row;
        return rows;
    }
    std::size_t count = 0;
    for (WriteRow& member : history_) {
        if (member.used && member.state != RowState::Terminal &&
            member.batchId == row.batchId) {
            rows[count++] = &member;
        }
    }
    return rows;
}

void VariableServiceV1::SyncBatchState(WriteRow& leader) noexcept {
    // One slice owns the union of the batch's targets. Mirror its lifecycle
    // on every row so drain, pending queries and bank captures agree.
    for (WriteRow* member : BatchRows(leader)) {
        if (member == nullptr || member == &leader) continue;
        member->state = leader.state;
        member->slice = leader.slice;
        member->noPhysicalTargets = leader.noPhysicalTargets;
        member->frontierCaptured = leader.frontierCaptured;
        member->requiredCommandFrontier = leader.requiredCommandFrontier;
        member->targetQuantum = leader.targetQuantum;
        member->committedFrame = leader.committedFrame;
        member->emitterDestroyedBeforeApplied = leader.emitterDestroyedBeforeApplied;
    }
}

void VariableServiceV1::FinalizeBatch(WriteRow& leader,
    VariableOutcomeKind outcome, LogicCommitTicket related,
    std::uint64_t actualFrame) noexcept {
    for (WriteRow* member : BatchRows(leader)) {
        if (member != nullptr) {
            FinalizeRow(*member, outcome, related, actualFrame);
            member->slice = {};
        }
    }
}

void VariableServiceV1::FinalizeRow(WriteRow& row,
                                    VariableOutcomeKind outcome,
                                    LogicCommitTicket related,
                                    std::uint64_t actualFrame) noexcept {
    if (!row.used || row.state == RowState::Terminal) {
        return;
    }
    // Each used history slot finalizes once and is recycled only after delivery.
    // Queued outcomes plus unfinalized slots therefore fit the reserved storage.
    assert(outcomes_.size() < kVariableHistoryCapacity);
    assert(outcomes_.size() < outcomes_.capacity());
    ReleaseEmitterOverrideReservation(row);
    row.state = RowState::Terminal;
    row.outcomeGeneration = currentOutcomeGeneration_;
    if (pendingRows_ > 0) {
        --pendingRows_;
    }
    VariableOutcomeRecord record{};
    record.outcome = outcome;
    record.acceptedTicket = row.ticket;
    record.relatedTicket = related;
    record.actualFrame = actualFrame;
    record.bankVersionStableOrdinal =
        outcome == VariableOutcomeKind::Applied ? row.bankOrdinal : 0;
    record.world = row.world;
    record.transport = row.transport;
    record.variableId = row.variableId;
    record.scope = row.scope.scope;
    record.emitterGeneration = row.scope.emitterGeneration;
    outcomes_.push_back(record);
    RememberFinalizedTicket(row.ticket.sequence, outcome);
    if (outcome == VariableOutcomeKind::Applied) {
        ++appliedCount_;
    } else if (outcome == VariableOutcomeKind::Superseded) {
        ++supersededCount_;
    } else {
        ++terminationCount_;
    }
}

void VariableServiceV1::CommitAppliedBank(const WriteRow& row) {
    const auto advanceGeneration = [](std::uint32_t& generation) noexcept {
        if (generation == UINT32_MAX || generation == 0) {
            generation = 0;
        } else {
            ++generation;
        }
    };
    if (row.scope.scope == VariableScopeV1::Global) {
        DefinitionRow* definition = FindDefinition(row.variableId);
        if (definition == nullptr) {
            return;
        }
        definition->currentGlobal = row.value;
        definition->scheduledGlobal = row.value;
        definition->currentBankOrdinal = row.bankOrdinal;
        definition->scheduledBankOrdinal = row.bankOrdinal;
        advanceGeneration(globalBankVersionGeneration_);
        for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
            EmitterBank& bank = emitterBanks_[slot];
            OverrideRow* override = FindOverride(bank, row.variableId);
            if (override != nullptr) {
                const std::uint32_t pendingSetCount = override->pendingSetCount;
                *override = OverrideRow{};
                if (pendingSetCount != 0) {
                    override->used = true;
                    override->variableId = row.variableId;
                    override->pendingSetCount = pendingSetCount;
                }
            }
            if (bank.generation != 0) {
                advanceGeneration(emitterBankVersionGenerations_[slot]);
            }
        }
        return;
    }
    EmitterBank* bank = FindEmitterBank(row.scope);
    if (bank == nullptr ||
        row.emitterOverrideSlot >= bank->overrides.size()) {
        return;
    }
    OverrideRow* override = &bank->overrides[row.emitterOverrideSlot];
    if (!override->used || override->variableId != row.variableId) {
        return;
    }
    override->currentValue = row.value;
    override->scheduledValue = row.value;
    override->currentBankOrdinal = row.bankOrdinal;
    override->scheduledBankOrdinal = row.bankOrdinal;
    if (row.scope.emitterSlot < emitterBankVersionGenerations_.size()) {
        advanceGeneration(
            emitterBankVersionGenerations_[row.scope.emitterSlot]);
    }
}

void VariableServiceV1::ReleaseEmitterOverrideReservation(
    WriteRow& row) noexcept {
    if (row.emitterOverrideSlot == UINT32_MAX) {
        return;
    }
    EmitterBank* bank = FindEmitterBank(row.scope);
    if (bank != nullptr && row.emitterOverrideSlot < bank->overrides.size()) {
        OverrideRow& override = bank->overrides[row.emitterOverrideSlot];
        if (override.used && override.variableId == row.variableId &&
            override.pendingSetCount != 0) {
            --override.pendingSetCount;
            if (override.pendingSetCount == 0 &&
                override.currentBankOrdinal == 0) {
                override = OverrideRow{};
            }
        }
    }
    row.emitterOverrideSlot = UINT32_MAX;
}

bool VariableServiceV1::GatherTargets(Engine& engine, WriteRow& batchRow,
                                      std::uint32_t* outCount) noexcept {
    if (outCount == nullptr) {
        return false;
    }
    *outCount = 0;
    bool overflow = false;
    const auto add = [&](VariableTargetLocator locator,
                         std::uint32_t revision) {
        for (std::uint32_t i = 0; i < *outCount; ++i) {
            if (SameTarget(censusScratch_[i].locator, locator)) {
                return;
            }
        }
        if (*outCount >= censusScratch_.size()) {
            overflow = true;
            return;
        }
        censusScratch_[*outCount] = {locator, revision};
        ++*outCount;
    };
    for (const WriteRow *member : BatchRows(batchRow)) {
        if (member == nullptr)
            continue;
        const WriteRow &row = *member;
        const VnEmitterHandle exactEmitter =
            (static_cast<std::uint64_t>(row.scope.emitterGeneration) << 32) |
            row.scope.emitterSlot;
        const auto voiceScopeMatches = [&](VnEmitterHandle emitter) {
            if (row.scope.scope == VariableScopeV1::Emitter) {
                return emitter == exactEmitter &&
                       engine.emitters.IsLive(emitter);
            }
            if (emitter == 0) {
                return true;
            }
            return engine.emitters.IsLive(emitter);
        };

        engine.params.ForEachVariableTarget(
            row.variableId, [&](VnVoiceHandle handle, VnEmitterHandle emitter,
                                std::uint32_t targetId) {
                if (!voiceScopeMatches(emitter) ||
                    engine.voices.Resolve(handle) == nullptr) {
                    return;
                }
                VariableTargetLocator locator{};
                locator.scope = VariableTargetScope::Voice;
                locator.object = handle;
                locator.objectGeneration =
                    static_cast<std::uint32_t>(handle >> 32);
                locator.targetId = targetId;
                add(locator, engine.params.TargetRevision(handle, targetId));
            });
        if (row.scope.scope == VariableScopeV1::Global) {
            engine.bindings.ForEachBusVariableTarget(
                row.variableId,
                [&](std::uint32_t busIndex, std::uint32_t targetId) {
                    VariableTargetLocator locator{};
                    locator.scope = VariableTargetScope::Bus;
                    locator.object = busIndex;
                    locator.objectGeneration =
                        engine.buses.GenerationAt(busIndex);
                    locator.targetId = targetId;
                    add(locator,
                        engine.busTargets.TargetRevision(
                            busIndex, locator.objectGeneration, targetId));
                });
        }
        engine.bindings.ForEachEffectVariableTarget(
            row.variableId,
            [&](EffectParamTargetStore::Scope scope, std::uint64_t object,
                std::uint32_t slot, std::uint32_t paramIndex,
                std::uint32_t effectType) {
                if (scope == EffectParamTargetStore::Scope::Voice) {
                    const VnVoiceHandle handle =
                        static_cast<VnVoiceHandle>(object);
                    if (engine.voices.Resolve(handle) == nullptr ||
                        !voiceScopeMatches(
                            engine.params.EmitterHandleOf(handle))) {
                        return;
                    }
                } else if (row.scope.scope != VariableScopeV1::Global) {
                    return;
                }
                std::uint32_t revision = 0;
                const std::uint32_t objectGeneration =
                    scope == EffectParamTargetStore::Scope::Voice
                        ? static_cast<std::uint32_t>(object >> 32)
                        : engine.buses.GenerationAt(
                              static_cast<std::uint32_t>(object));
                if (!engine.effectParamTargets.ExactTargetRevision(
                        scope, object, objectGeneration, slot, paramIndex,
                        effectType, &revision)) {
                    return;
                }
                VariableTargetLocator locator{};
                locator.scope = scope == EffectParamTargetStore::Scope::Voice
                                    ? VariableTargetScope::VoiceEffect
                                    : VariableTargetScope::BusEffect;
                locator.object = object;
                locator.objectGeneration = objectGeneration;
                locator.effectSlot = slot;
                locator.paramIndex = paramIndex;
                locator.effectType = effectType;
                add(locator, revision);
            });
    }
    if (overflow) {
        return false;
    }
    std::sort(censusScratch_.begin(), censusScratch_.begin() + *outCount,
              [](const GatheredTarget& left, const GatheredTarget& right) {
                  if (left.locator.scope != right.locator.scope) {
                      return left.locator.scope < right.locator.scope;
                  }
                  if (left.locator.object != right.locator.object) {
                      return left.locator.object < right.locator.object;
                  }
                  if (left.locator.objectGeneration !=
                      right.locator.objectGeneration) {
                      return left.locator.objectGeneration <
                             right.locator.objectGeneration;
                  }
                  if (left.locator.targetId != right.locator.targetId) {
                      return left.locator.targetId < right.locator.targetId;
                  }
                  if (left.locator.effectSlot != right.locator.effectSlot) {
                      return left.locator.effectSlot < right.locator.effectSlot;
                  }
                  if (left.locator.paramIndex != right.locator.paramIndex) {
                      return left.locator.paramIndex < right.locator.paramIndex;
                  }
                  return left.locator.effectType < right.locator.effectType;
              });
    return true;
}

bool VariableServiceV1::RecensusMatches(Engine& engine,
                                        WriteRow& row) noexcept {
    std::uint32_t count = 0;
    if (!GatherTargets(engine, row, &count) ||
        count != slices_.TargetCount(row.slice)) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target = slices_.Target(row.slice, i);
        if (target == nullptr ||
            !SameTarget(target->locator, censusScratch_[i].locator) ||
            target->targetRevision != censusScratch_[i].revision) {
            return false;
        }
        float resolved = 0.0f;
        std::uint32_t rampMs = 0;
        std::uint32_t revision = 0;
        std::uint32_t effectType = target->locator.effectType;
        bool owned = false;
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            owned = engine.params.SnapshotForClaim(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, row.slice, &resolved, &rampMs,
                &revision);
            break;
        case VariableTargetScope::Bus:
            owned = engine.busTargets.SnapshotForClaim(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.slice, &resolved, &rampMs, &revision);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            owned = engine.effectParamTargets.SnapshotForClaim(
                target->locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object, target->locator.objectGeneration,
                target->locator.effectSlot, target->locator.paramIndex,
                row.slice, &resolved, &revision, &effectType);
            break;
        }
        if (!owned || revision != target->targetRevision ||
            effectType != target->locator.effectType) {
            return false;
        }
    }
    return true;
}

void VariableServiceV1::ProjectCandidate(Engine& engine,
                                         const WriteRow& row) {
    if (row.scope.scope == VariableScopeV1::Global) {
        (void)engine.params.SetInstalledValue(row.variableId, row.value);
        for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
            const EmitterBank& bank = emitterBanks_[slot];
            if (!bank.live) continue;
            const VnEmitterHandle emitter =
                (static_cast<std::uint64_t>(bank.generation) << 32) | slot;
            (void)engine.emitters.ClearLocalValue(emitter, row.variableId);
        }
        engine.params.NoteVariableWrite(row.variableId);
        engine.bindings.NoteBusVariableWrite(row.variableId,
                                             engine.busTargets);
        return;
    }
    const VnEmitterHandle emitter =
        (static_cast<std::uint64_t>(row.scope.emitterGeneration) << 32) |
        row.scope.emitterSlot;
    (void)engine.emitters.SetLocalValue(emitter, row.variableId, row.value);
    engine.params.NoteVariableWrite(row.variableId);
}

void VariableServiceV1::ProjectCurrentApplied(Engine& engine,
                                              const WriteRow& row) {
    if (row.scope.scope == VariableScopeV1::Global) {
        const DefinitionRow* definition = FindDefinition(row.variableId);
        if (definition != nullptr) {
            (void)engine.params.SetInstalledValue(row.variableId,
                                                  definition->currentGlobal);
            for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
                const EmitterBank& bank = emitterBanks_[slot];
                if (!bank.live) continue;
                const VnEmitterHandle emitter =
                    (static_cast<std::uint64_t>(bank.generation) << 32) | slot;
                const OverrideRow* override = FindCurrentOverride(bank, row.variableId);
                if (override == nullptr) {
                    (void)engine.emitters.ClearLocalValue(emitter, row.variableId);
                } else {
                    (void)engine.emitters.SetLocalValue(emitter, row.variableId,
                                                       override->currentValue);
                }
            }
            engine.params.NoteVariableWrite(row.variableId);
            engine.bindings.NoteBusVariableWrite(row.variableId,
                                                 engine.busTargets);
        }
        return;
    }
    const VnEmitterHandle emitter =
        (static_cast<std::uint64_t>(row.scope.emitterGeneration) << 32) |
        row.scope.emitterSlot;
    const EmitterBank* bank = FindEmitterBank(row.scope);
    const OverrideRow* override =
        bank == nullptr ? nullptr
                        : FindCurrentOverride(*bank, row.variableId);
    if (override == nullptr) {
        (void)engine.emitters.ClearLocalValue(emitter, row.variableId);
    } else {
        (void)engine.emitters.SetLocalValue(emitter, row.variableId,
                                            override->currentValue);
    }
    engine.params.NoteVariableWrite(row.variableId);
}

void VariableServiceV1::RunVariableEvaluators(Engine& engine) {
    engine.bindings.EvaluateBusAttachments(engine.params, engine.busTargets);
    engine.bindings.EvaluateEffectParamBindings(
        engine.params, engine.effectParamTargets, &engine.emitters);
    engine.params.EvaluateDirtyBindings(&engine.emitters);
}

bool VariableServiceV1::SnapshotCandidateBaselines(Engine& engine,
                                                   WriteRow& row) noexcept {
    const std::uint32_t count = slices_.TargetCount(row.slice);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target = slices_.Target(row.slice, i);
        if (target == nullptr) {
            return false;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            if (!engine.params.CaptureCandidateBaseline(
                    static_cast<VnVoiceHandle>(target->locator.object),
                    target->locator.targetId, row.slice)) {
                return false;
            }
            break;
        case VariableTargetScope::Bus:
            if (!engine.busTargets.CaptureCandidateBaseline(
                    static_cast<std::uint32_t>(target->locator.object),
                    target->locator.objectGeneration,
                    target->locator.targetId, row.slice)) {
                return false;
            }
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            break;
        }
    }
    return true;
}

void VariableServiceV1::ClearCandidateBaselines(Engine& engine,
                                                WriteRow& row) noexcept {
    const std::uint32_t count = slices_.TargetCount(row.slice);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target = slices_.Target(row.slice, i);
        if (target == nullptr) {
            continue;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.ClearCandidateBaseline(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, row.slice);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.ClearCandidateBaseline(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.slice);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            break;
        }
    }
}

VariableServiceV1::CandidateCaptureResult
VariableServiceV1::CaptureCandidateSlice(Engine& engine, WriteRow& row) {
    row.state = RowState::TargetsClaimed;
    RunVariableEvaluators(engine);
    if (!RecensusMatches(engine, row)) {
        return CandidateCaptureResult::Retry;
    }
    if (!SnapshotCandidateBaselines(engine, row)) {
        ClearCandidateBaselines(engine, row);
        return CandidateCaptureResult::Retry;
    }
    for (const WriteRow* member : BatchRows(row)) {
        if (member != nullptr) ProjectCandidate(engine, *member);
    }
    RunVariableEvaluators(engine);
    bool valid = true;
    bool rejected = false;
    const std::uint32_t count = slices_.TargetCount(row.slice);
    for (std::uint32_t i = 0; i < count; ++i) {
        VariableSliceTarget* existing = slices_.Target(row.slice, i);
        if (existing == nullptr) {
            valid = false;
            break;
        }
        VariableSliceTarget target = *existing;
        float resolved = 0.0f;
        std::uint32_t rampMs = 0;
        std::uint32_t revision = 0;
        std::uint32_t effectType = target.locator.effectType;
        switch (target.locator.scope) {
        case VariableTargetScope::Voice: {
            float pan2d = 0.0f;
            valid = engine.params.CaptureSnapshotForClaim(
                static_cast<VnVoiceHandle>(target.locator.object),
                target.locator.targetId, row.slice, &resolved, &rampMs,
                &revision, &pan2d);
            if (valid && target.locator.targetId == kTargetVoiceVolume &&
                !TryVolumeGainFromDb(resolved)) {
                rejected = true;
                break;
            }
            ParamCommit commit{};
            commit.handle =
                static_cast<VnVoiceHandle>(target.locator.object);
            commit.targetId = target.locator.targetId;
            commit.resolved = resolved;
            commit.pan2d = pan2d;
            commit.rampMs = rampMs;
            valid = valid &&
                    MakeVariableParamCommand(engine, commit, &target.command);
            break;
        }
        case VariableTargetScope::Bus:
            valid = engine.busTargets.CaptureSnapshotForClaim(
                static_cast<std::uint32_t>(target.locator.object),
                target.locator.objectGeneration, target.locator.targetId,
                row.slice, &resolved, &rampMs, &revision);
            if (valid &&
                (target.locator.targetId == kBusTargetBusVolume ||
                 target.locator.targetId == kBusTargetBusOutputVolume) &&
                !TryVolumeGainFromDb(resolved)) {
                rejected = true;
                break;
            }
            if (valid) {
                const std::uint32_t busIndex =
                    static_cast<std::uint32_t>(target.locator.object);
                const bool outputOnly =
                    target.locator.targetId == kBusTargetBusOutputVolume;
                target.command.opcode = kOpcodeSetBusVolume;
                target.command.payload.setBusVolume.busIndex = busIndex;
                target.command.payload.setBusVolume.volumeDb =
                    VariableBusWireValue(engine, busIndex, resolved,
                                         target.locator.targetId);
                target.command.payload.setBusVolume.rampMs = rampMs;
                target.command.payload.setBusVolume.busGeneration =
                    target.locator.objectGeneration;
                target.command.payload.setBusVolume.outputOnly =
                    outputOnly ? 1u : 0u;
            }
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect: {
            const EffectParamTargetStore::Scope scope =
                target.locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus;
            valid = engine.effectParamTargets.SnapshotForClaim(
                scope, target.locator.object,
                target.locator.objectGeneration, target.locator.effectSlot,
                target.locator.paramIndex, row.slice, &resolved, &revision,
                &effectType);
            valid = valid &&
                    MakeVariableEffectParamCommand(
                        engine, scope, target.locator.object,
                        target.locator.effectSlot, target.locator.paramIndex,
                        resolved, effectType, &target.command);
            break;
        }
        }
        if (rejected) {
            break;
        }
        if (!valid || revision != target.targetRevision ||
            effectType != target.locator.effectType) {
            valid = false;
            break;
        }
        target.resolvedValue = resolved;
        target.rampMs = rampMs;
        *existing = target;
    }
    SetSliceRampSuppressed(engine, row, true);
    for (const WriteRow* member : BatchRows(row)) {
        if (member != nullptr) ProjectCurrentApplied(engine, *member);
    }
    RunVariableEvaluators(engine);
    if (rejected) {
        RejectPreparedRow(engine, row);
        return CandidateCaptureResult::Rejected;
    }
    SetSliceRampSuppressed(engine, row, false);
    return valid ? CandidateCaptureResult::Ready
                 : CandidateCaptureResult::Retry;
}

void VariableServiceV1::RejectPreparedRow(Engine& engine,
                                          WriteRow& row) noexcept {
    if (row.slice.Valid()) {
        ReleaseSliceClaims(engine, row, ClaimReleaseReason::RejectCandidate);
        (void)slices_.AbandonPrepared(row.slice);
    }
    row.slice = {};
    FinalizeBatch(row, VariableOutcomeKind::TargetRejected, {}, 0);
}

void VariableServiceV1::ResetPreparedRow(Engine& engine,
                                         WriteRow& row) noexcept {
    if (row.slice.Valid()) {
        ReleaseSliceClaims(engine, row);
        (void)slices_.AbandonPrepared(row.slice);
    }
    row.slice = {};
    row.state = RowState::Pending;
    row.frontierCaptured = false;
    row.noPhysicalTargets = false;
    SyncBatchState(row);
}

bool VariableServiceV1::PrepareCurrentSlice(Engine& engine, WriteRow& row) {
    std::uint32_t count = 0;
    if (!GatherTargets(engine, row, &count)) {
        return false;
    }
    if (count == 0) {
        row.noPhysicalTargets = true;
        row.state = RowState::WaitingFrontier;
        return true;
    }

    if (!slices_.Reserve(row.world, row.transport, row.ticket.sequence,
                         row.requestedFrame, row.bankOrdinal, count,
                         &row.slice)) {
        return false;
    }
    row.state = RowState::TargetsClaimed;

    const auto releaseClaim = [&](const GatheredTarget& target) {
        switch (target.locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.ReleaseClaim(
                static_cast<VnVoiceHandle>(target.locator.object),
                target.locator.targetId, row.slice);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.ReleaseClaim(
                static_cast<std::uint32_t>(target.locator.object),
                target.locator.objectGeneration, target.locator.targetId,
                row.slice);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.ReleaseClaim(
                target.locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target.locator.object, target.locator.objectGeneration,
                target.locator.effectSlot, target.locator.paramIndex,
                row.slice);
            break;
        }
    };
    std::uint32_t claimed = 0;
    for (; claimed < count; ++claimed) {
        const GatheredTarget& target = censusScratch_[claimed];
        bool ok = false;
        switch (target.locator.scope) {
        case VariableTargetScope::Voice:
            ok = engine.params.TryClaim(
                static_cast<VnVoiceHandle>(target.locator.object),
                target.locator.targetId, row.slice, target.revision);
            break;
        case VariableTargetScope::Bus:
            ok = engine.busTargets.TryClaim(
                static_cast<std::uint32_t>(target.locator.object),
                target.locator.objectGeneration, target.locator.targetId,
                row.slice, target.revision);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            ok = engine.effectParamTargets.TryClaim(
                target.locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target.locator.object, target.locator.objectGeneration,
                target.locator.effectSlot, target.locator.paramIndex,
                row.slice, target.revision);
            break;
        }
        if (!ok) {
            break;
        }
        VariableSliceTarget sliceTarget{};
        sliceTarget.locator = target.locator;
        sliceTarget.targetRevision = target.revision;
        if (!slices_.AddTarget(row.slice, sliceTarget)) {
            releaseClaim(target);
            break;
        }
    }
    if (claimed != count) {
        ResetPreparedRow(engine, row);
        return false;
    }
    row.requiredCommandFrontier = engine.commands.PublishedHead();
    row.frontierCaptured = true;
    row.state = RowState::WaitingFrontier;
    return true;
}

void VariableServiceV1::ReleaseSliceClaims(
    Engine& engine, WriteRow& row, ClaimReleaseReason reason) noexcept {
    const VariableSliceTable& slices = slices_;
    const std::uint32_t count = slices_.TargetCount(row.slice);
    for (std::uint32_t i = count; i > 0; --i) {
        const VariableSliceTarget* target = slices.Target(row.slice, i - 1);
        if (target == nullptr) {
            continue;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.ReleaseClaim(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, row.slice, reason);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.ReleaseClaim(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.slice, reason);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.ReleaseClaim(
                target->locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object, target->locator.objectGeneration,
                target->locator.effectSlot, target->locator.paramIndex,
                row.slice);
            break;
        }
    }
}

void VariableServiceV1::CommitSliceTargets(Engine& engine,
                                           WriteRow& row) noexcept {
    const VariableSliceTable& slices = slices_;
    const std::uint32_t count = slices_.TargetCount(row.slice);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target = slices.Target(row.slice, i);
        if (target == nullptr) {
            continue;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.MarkSliceCommitted(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, row.slice, target->resolvedValue,
                target->rampMs, target->locator.targetId == kTargetVoicePan
                    ? target->command.payload.setVoicePan.pan2d : 0.0f);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.MarkSliceCommitted(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.slice, target->resolvedValue, target->rampMs);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.MarkSliceCommitted(
                target->locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object, target->locator.objectGeneration,
                target->locator.effectSlot, target->locator.paramIndex,
                row.slice,
                target->resolvedValue);
            break;
        }
    }
}

void VariableServiceV1::SetSliceRampSuppressed(
    Engine& engine, const WriteRow& row, bool suppressed) noexcept {
    const VariableSliceTable& slices = slices_;
    const std::uint32_t count = slices.TargetCount(row.slice);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VariableSliceTarget* target = slices.Target(row.slice, i);
        if (target == nullptr) {
            continue;
        }
        switch (target->locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.SetClaimRampSuppressed(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, row.slice, suppressed);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.SetClaimRampSuppressed(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.slice, suppressed);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            break;
        }
    }
}

void VariableServiceV1::BeginService(Engine& engine,
                                     std::uint64_t blocksRendered,
                                     std::uint32_t blockFrames) {
    serviceAudioFrame_ =
        blockFrames == 0 || blocksRendered > UINT64_MAX / blockFrames
            ? (blockFrames == 0 ? blocksRendered : UINT64_MAX)
            : blocksRendered * blockFrames;
    std::array<std::size_t, kVariableWritesPerService> candidates{};
    std::size_t candidateCount = 0;
    std::size_t candidateRows = 0;
    const std::size_t historySize = history_.size();
    if (historySize == 0) {
        return;
    }
    const std::size_t start = serviceCursor_ % historySize;
    for (std::size_t offset = 0; offset < historySize; ++offset) {
        const std::size_t i = (start + offset) % historySize;
        WriteRow& row = history_[i];
        if (!row.used || !row.batchLeader || row.state != RowState::Pending ||
            row.requestedFrame > serviceAudioFrame_) {
            continue;
        }
        bool earlierSameKey = false;
        const auto members = BatchRows(row);
        for (const WriteRow& other : history_) {
            if (&row != &other && other.used &&
                other.state != RowState::Terminal &&
                std::any_of(members.begin(), members.end(),
                    [&](const WriteRow* member) {
                        return member != nullptr && SameServiceDependency(other, *member);
                    }) &&
                (other.requestedFrame < row.requestedFrame ||
                 (other.requestedFrame == row.requestedFrame &&
                  (other.ticket.sequence < row.ticket.sequence ||
                   (other.ticket.sequence == row.ticket.sequence &&
                    other.executionOrdinal < row.executionOrdinal))))) {
                earlierSameKey = true;
                break;
            }
        }
        if (earlierSameKey) {
            continue;
        }
        const auto memberCount = static_cast<std::size_t>(std::count_if(
            members.begin(), members.end(), [](const WriteRow* member) {
                return member != nullptr;
            }));
        if (candidateRows + memberCount > kVariableWritesPerService) break;
        candidateRows += memberCount;
        candidates[candidateCount++] = i;
        serviceCursor_ = (i + 1) % historySize;
        if (candidateCount >= kVariableWritesPerService) {
            break;
        }
    }
    if (candidateCount == 0) {
        serviceCursor_ = (start + 1) % historySize;
    }
    for (std::size_t candidate = 0; candidate < candidateCount; ++candidate) {
        WriteRow& row = history_[candidates[candidate]];
        row.targetQuantum = blockFrames == 0 ? blocksRendered
                                             : row.requestedFrame / blockFrames;
        (void)PrepareCurrentSlice(engine, row);
        SyncBatchState(row);
    }
}

void VariableServiceV1::PrepareClaimedSlices(Engine& engine) {
    for (WriteRow& row : history_) {
        if (!row.used || !row.batchLeader || row.noPhysicalTargets || !row.slice.Valid()) {
            continue;
        }
        if (row.state == RowState::WaitingFrontier) {
            if (engine.commands.SettledFrontier() <
                row.requiredCommandFrontier) {
                continue;
            }
            if (!RecensusMatches(engine, row)) {
                ResetPreparedRow(engine, row);
                continue;
            }
            const CandidateCaptureResult captured =
                CaptureCandidateSlice(engine, row);
            if (captured == CandidateCaptureResult::Retry) {
                ResetPreparedRow(engine, row);
                continue;
            }
            if (captured == CandidateCaptureResult::Rejected) {
                continue;
            }
            row.state = RowState::SlicePrepared;
            SyncBatchState(row);
        }
        if (row.state != RowState::SlicePrepared) {
            continue;
        }
        ScheduledCurrentSliceInput input{};
        input.executeFrame = row.requestedFrame;
        input.logicCommitSequence = row.ticket.sequence;
        input.slice = {row.slice.slot, row.slice.generation};
        const std::uint64_t nextOpenFrame = std::max(
            {timeline_.PublishCutoffFrame(),
             timeline_.WindowStartFrame() +
                 static_cast<std::uint64_t>(timeline_.PublishLeadQuanta()) *
                     timeline_.QuantumFrames(),
             (timeline_.ConsumerFrontier().load(std::memory_order_acquire) +
              1u) * timeline_.QuantumFrames(),
             serviceAudioFrame_ >
                     UINT64_MAX - timeline_.QuantumFrames()
                 ? UINT64_MAX
                 : serviceAudioFrame_ + timeline_.QuantumFrames()});
        const ReserveResult reserved = timeline_.ReserveCurrentSlice(
            input, PrePublishLatePolicy::ExecuteAtEarliestSafeFrame,
            nextOpenFrame);
        if (reserved.status != ReserveStatus::Committed) {
            continue;
        }
        if (!slices_.Publish(row.slice)) {
            continue;
        }
        row.committedFrame = reserved.firstFrame;
        row.state = RowState::PageCommitted;
        SyncBatchState(row);
        timeline_.Publish(reserved.firstFrame + timeline_.QuantumFrames());
    }
}

void VariableServiceV1::EndService(Engine& engine) {
    const std::uint64_t blocksRendered = engine.mixer.BlocksRendered();
    for (WriteRow& row : history_) {
        if (!row.used || !row.batchLeader) {
            continue;
        }
        if (row.state == RowState::WaitingFrontier && row.noPhysicalTargets) {
            if (blocksRendered < row.targetQuantum + 1) {
                continue;
            }
            std::uint32_t count = 0;
            if (!GatherTargets(engine, row, &count) || count != 0) {
                row.state = RowState::Pending;
                row.noPhysicalTargets = false;
                SyncBatchState(row);
                continue;
            }
            for (WriteRow* member : BatchRows(row)) {
                if (member == nullptr) continue;
                CommitAppliedBank(*member);
                ProjectCurrentApplied(engine, *member);
            }
            RunVariableEvaluators(engine);
            FinalizeBatch(row, VariableOutcomeKind::Applied, {},
                        blocksRendered * engine.config.blockFrames);
            continue;
        }
        if (row.state != RowState::PageCommitted || !row.slice.Valid()) {
            continue;
        }
        const VariableSliceState state = slices_.State(row.slice);
        if (state == VariableSliceState::Invalidated) {
            const std::uint64_t appliedBlock =
                slices_.AppliedBlockSequence(row.slice);
            if (appliedBlock == 0) {
                continue;
            }
            ReleaseSliceClaims(engine, row);
            FinalizeBatch(row, VariableOutcomeKind::EmitterDestroyed, {},
                        appliedBlock * engine.config.blockFrames);
            row.slice = {};
            continue;
        }
        if (state != VariableSliceState::SliceApplied) {
            continue;
        }
        if (!row.emitterDestroyedBeforeApplied) {
            SetSliceRampSuppressed(engine, row, true);
            for (WriteRow* member : BatchRows(row)) {
                if (member == nullptr) continue;
                CommitAppliedBank(*member);
                ProjectCurrentApplied(engine, *member);
            }
            RunVariableEvaluators(engine);
            SetSliceRampSuppressed(engine, row, false);
            CommitSliceTargets(engine, row);
        } else {
            CommitSliceTargets(engine, row);
            for (const WriteRow* member : BatchRows(row)) {
                if (member != nullptr) ProjectCurrentApplied(engine, *member);
            }
            RunVariableEvaluators(engine);
        }
        const std::uint64_t appliedBlock =
            slices_.AppliedBlockSequence(row.slice);
        FinalizeBatch(row,
                    row.emitterDestroyedBeforeApplied
                        ? VariableOutcomeKind::EmitterDestroyed
                        : VariableOutcomeKind::Applied,
                    {},
                    appliedBlock * engine.config.blockFrames);
        row.slice = {};
    }
    slices_.ReclaimSettled(blocksRendered);
    observedBlock_ = blocksRendered;
}

VnResult VariableServiceV1::ReplacePackDefinitions(
    const VariableDefinitionV1* definitions, std::uint32_t count) {
    PreparedPackDefinitions prepared;
    const VnResult result =
        PreparePackDefinitions(definitions, count, &prepared);
    if (result == VN_OK) {
        CommitPreparedPackDefinitions(&prepared);
    }
    return result;
}

VariableServiceV1::PreparedPackDefinitions::PreparedPackDefinitions(
    PreparedPackDefinitions&& other) noexcept {
    *this = std::move(other);
}

VariableServiceV1::PreparedPackDefinitions&
VariableServiceV1::PreparedPackDefinitions::operator=(
    PreparedPackDefinitions&& other) noexcept {
    if (this != &other) {
        owner_ = std::exchange(other.owner_, nullptr);
        reason_ = other.reason_;
        labelCatalog_ = std::move(other.labelCatalog_);
        sdkCatalogFingerprint_ = other.sdkCatalogFingerprint_;
        priorWorld_ = other.priorWorld_;
        priorTransport_ = other.priorTransport_;
        actualFrame_ = other.actualFrame_;
        hasWorld_ = other.hasWorld_;
        worldGeneration_ = other.worldGeneration_;
        terminations_ = std::move(other.terminations_);
        definitions_ = std::move(other.definitions_);
        definitionViews_ = std::move(other.definitionViews_);
        emitterBanks_ = std::move(other.emitterBanks_);
    }
    return *this;
}

bool VariableServiceV1::CanTerminateForWorldReplacement(
    const WriteRow& row) const noexcept {
    return row.world == worldGeneration_ && row.transport == transport_.Current() &&
        !row.slice.Valid() && (row.state == RowState::Pending ||
            (row.state == RowState::WaitingFrontier && row.noPhysicalTargets));
}

VnResult VariableServiceV1::PreparePackDefinitions(
    const VariableDefinitionV1* definitions, std::uint32_t count,
    PreparedPackDefinitions* out) const {
    return PreparePackDefinitions(definitions, count,
        PackVariablePublicationReason::ContentMutation, 0, out);
}

VnResult VariableServiceV1::PreparePackDefinitions(
    const VariableDefinitionV1* definitions, std::uint32_t count,
    PackVariablePublicationReason reason, std::uint64_t actualFrame,
    PreparedPackDefinitions* out) const {
    return PreparePackDefinitions(definitions, count, nullptr, reason,
                                  actualFrame, out);
}

VnResult VariableServiceV1::PreparePackDefinitions(
    const VariableDefinitionV1* definitions, std::uint32_t count,
    const VariablePolicySidecarV1* overlays,
    PackVariablePublicationReason reason, std::uint64_t actualFrame,
    PreparedPackDefinitions* out) const {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedPackDefinitions{};
    if ((count != 0 && definitions == nullptr) ||
        (reason != PackVariablePublicationReason::ContentMutation &&
         reason != PackVariablePublicationReason::InitPublication &&
         reason != PackVariablePublicationReason::InitUnload) ||
        (reason == PackVariablePublicationReason::InitUnload && count != 0)) {
        return VN_INVALID_ARG;
    }
    if (count > kVariableDefinitionCapacity) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!ValidDefinition(definitions[i]) ||
            (i != 0 && definitions[i - 1].variableId >=
                           definitions[i].variableId)) {
            return VN_BAD_PACK;
        }
    }

    PreparedPackDefinitions prepared;
    const bool changesWorld = reason != PackVariablePublicationReason::ContentMutation;
    if (changesWorld) {
        if (worldGeneration_ == UINT64_MAX) {
            return VN_RUNTIME_RECREATE_REQUIRED;
        }
        if (AdmissionClosed() || preparedRows_ != 0 ||
            slices_.HasUnreclaimedWorldRows(worldGeneration_)) {
            return VN_BACKPRESSURE;
        }
        for (std::uint32_t slot = 0; slot < history_.size(); ++slot) {
            const WriteRow& row = history_[slot];
            if (!row.used || row.state == RowState::Terminal) continue;
            if (!CanTerminateForWorldReplacement(row)) return VN_BACKPRESSURE;
            detail::CheckedGrow(prepared.terminations_, prepared.terminations_.size() + 1,
                                detail::LogicAllocSite::PackVariablePublication);
            prepared.terminations_.push_back({slot, row.ticket});
        }
    }
    detail::CheckedGrow(prepared.definitions_, count, detail::LogicAllocSite::PackVariablePublication);
    prepared.definitions_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        DefinitionRow row{};
        row.definition = definitions[i];
        row.definition.impact = StaticImpactSummary{};
        const DefinitionRow* prior = FindDefinition(row.definition.variableId);
        if (overlays != nullptr) {
            row.writers = overlays[i].writers;
            row.executionFingerprint = overlays[i].executionFingerprint;
        } else if (prior != nullptr &&
                   SameDefinition(prior->definition, row.definition)) {
            row.writers = prior->writers;
            row.executionFingerprint = prior->executionFingerprint;
        } else {
            row.writers = 3;
            row.executionFingerprint = 0;
        }
        row.currentGlobal = row.definition.defaultValue;
        row.scheduledGlobal = row.definition.defaultValue;
        if (prior != nullptr && SameDefinition(*prior, row)) {
            row = *prior;
            row.definition = definitions[i];
            row.definition.impact = StaticImpactSummary{};
            if (overlays != nullptr) {
                row.writers = overlays[i].writers;
                row.executionFingerprint = overlays[i].executionFingerprint;
            }
        }
        if (changesWorld) {
            row.scheduledGlobal = row.currentGlobal;
            row.scheduledBankOrdinal = row.currentBankOrdinal;
        }
        prepared.definitions_.push_back(row);
    }

    std::vector<std::uint64_t> catalogFingerprints;
    bool completeCatalog = true;
    detail::CheckedGrow(prepared.labelCatalog_, count, detail::LogicAllocSite::PackVariablePublication);
    prepared.labelCatalog_.reserve(count);
    catalogFingerprints.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        VariableLabelCatalogV1 catalog;
        catalog.variableId = definitions[i].variableId;
        if (overlays) {
            if (overlays[i].labelCount != 0 && overlays[i].labels == nullptr) return VN_BAD_PACK;
            catalog.fingerprint = overlays[i].catalogFingerprint;
            if (overlays[i].labelCount != 0)
                catalog.labels.assign(overlays[i].labels, overlays[i].labels + overlays[i].labelCount);
        } else {
            const auto found = std::find_if(labelCatalog_.begin(), labelCatalog_.end(),
                [&](const auto& prior) { return prior.variableId == catalog.variableId; });
            const auto* prior = FindDefinition(catalog.variableId);
            if (found != labelCatalog_.end() && prior != nullptr &&
                SameDefinition(prior->definition, definitions[i])) catalog = *found;
        }
        completeCatalog = completeCatalog && catalog.fingerprint != 0;
        catalogFingerprints.push_back(catalog.fingerprint);
        prepared.labelCatalog_.push_back(std::move(catalog));
    }
    prepared.sdkCatalogFingerprint_ = completeCatalog
        ? ComputeVariableSdkCatalogFingerprintV1(catalogFingerprints.data(), count) : 0;

    detail::CheckedGrow(prepared.emitterBanks_, emitterBanks_.size(), detail::LogicAllocSite::PackVariablePublication);
    prepared.emitterBanks_ = emitterBanks_;
    for (EmitterBank& bank : prepared.emitterBanks_) {
        for (OverrideRow& override : bank.overrides) {
            if (!override.used) {
                continue;
            }
            const auto nextDef = std::find_if(
                prepared.definitions_.begin(), prepared.definitions_.end(),
                [&](const DefinitionRow& row) {
                    return row.definition.variableId == override.variableId;
                });
            const DefinitionRow* prior = FindDefinition(override.variableId);
            if (nextDef == prepared.definitions_.end() || prior == nullptr ||
                !SameDefinition(*prior, *nextDef) ||
                (changesWorld && override.currentBankOrdinal == 0)) {
                override = OverrideRow{};
            } else if (changesWorld) {
                override.scheduledValue = override.currentValue;
                override.scheduledBankOrdinal = override.currentBankOrdinal;
                override.pendingSetCount = 0;
            }
        }
    }

    detail::CheckedGrow(prepared.definitionViews_, prepared.definitions_.size(), detail::LogicAllocSite::PackVariablePublication);
    prepared.definitionViews_.reserve(prepared.definitions_.size());
    for (const DefinitionRow& row : prepared.definitions_) {
        prepared.definitionViews_.push_back(row.definition);
    }
    prepared.reason_ = reason;
    prepared.priorWorld_ = worldGeneration_;
    prepared.priorTransport_ = transport_.Current();
    prepared.actualFrame_ = actualFrame;
    prepared.worldGeneration_ = changesWorld ? worldGeneration_ + 1 :
        (worldGeneration_ == 0 ? 1 : worldGeneration_);
    prepared.hasWorld_ = reason != PackVariablePublicationReason::InitUnload;
    prepared.owner_ = this;
    *out = std::move(prepared);
    return VN_OK;
}

void VariableServiceV1::CommitPreparedPackDefinitions(
    PreparedPackDefinitions* prepared) noexcept {
    assert(prepared != nullptr && prepared->owner_ == this);
    if (prepared == nullptr || prepared->owner_ != this) {
        return;
    }
    assert(prepared->priorWorld_ == worldGeneration_);
    assert(prepared->priorTransport_ == transport_.Current());
    const bool changesWorld = prepared->reason_ != PackVariablePublicationReason::ContentMutation;
    if (changesWorld) {
        assert(!AdmissionClosed() && preparedRows_ == 0);
        assert(!slices_.HasUnreclaimedWorldRows(worldGeneration_));
#ifndef NDEBUG
        std::size_t captured = 0;
        for (std::uint32_t slot = 0; slot < history_.size(); ++slot) {
            const WriteRow& row = history_[slot];
            if (!row.used || row.state == RowState::Terminal) continue;
            assert(CanTerminateForWorldReplacement(row));
            assert(captured < prepared->terminations_.size());
            const auto& termination = prepared->terminations_[captured++];
            assert(termination.historySlot == slot && termination.ticket.sequence == row.ticket.sequence);
        }
        assert(captured == prepared->terminations_.size());
#endif
        assert(outcomes_.size() + prepared->terminations_.size() <= outcomes_.capacity());
        for (const auto& termination : prepared->terminations_) {
            FinalizeRow(history_[termination.historySlot],
                        VariableOutcomeKind::WorldReplacement, {}, prepared->actualFrame_);
        }
    }
    definitions_.swap(prepared->definitions_);
    labelCatalog_.swap(prepared->labelCatalog_);
    sdkCatalogFingerprint_ = prepared->sdkCatalogFingerprint_;
    definitionViews_.swap(prepared->definitionViews_);
    emitterBanks_.swap(prepared->emitterBanks_);
    worldGeneration_ = prepared->worldGeneration_;
    hasWorld_ = prepared->hasWorld_;
    if (changesWorld) {
        globalBankVersionGeneration_ = 1;
        std::fill(emitterBankVersionGenerations_.begin(), emitterBankVersionGenerations_.end(), 1u);
    }
    prepared->owner_ = nullptr;
}

void VariableServiceV1::CommitPreparedPackDefinitions(
    PreparedPackDefinitions* prepared, ParamSystem& params,
    EmitterTable& emitters, BindingTable& bindings,
    BusTargetStore& busTargets) noexcept {
    CommitPreparedPackDefinitions(prepared);
    ProjectCurrentWorld(params, emitters, bindings, busTargets);
}

void VariableServiceV1::ProjectCurrentWorld(ParamSystem& params,
                                            EmitterTable& emitters,
                                            BindingTable& bindings,
                                            BusTargetStore& busTargets) noexcept {
    params.ClearInstalledValues();
    for (const DefinitionRow& row : definitions_) {
        params.SetInstalledValue(row.definition.variableId, row.currentGlobal);
        bindings.NoteBusVariableWrite(row.definition.variableId, busTargets);
    }
    for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
        EmitterBank& bank = emitterBanks_[slot];
        if (!bank.live) {
            continue;
        }
        const VnEmitterHandle emitter =
            (static_cast<std::uint64_t>(bank.generation) << 32) | slot;
        if (!emitters.IsLive(emitter)) {
            bank.live = false;
            continue;
        }
        (void)emitters.ClearLocalValues(emitter);
        for (const OverrideRow& override : bank.overrides) {
            if (override.used && override.currentBankOrdinal != 0) {
                emitters.SetLocalValue(emitter, override.variableId,
                                       override.currentValue);
            }
        }
    }
}

bool VariableServiceV1::ResetTransport(std::uint64_t resetFrame) {
    if (transportDrainState_ == TransportDrainState::Idle) {
        if (DrainingWorld()) {
            return false;
        }
        transportDrainState_ = TransportDrainState::DrainingCurrent;
        transportDrainEpoch_ = transport_.Current();
        transportDrainFrame_ = resetFrame;
        for (WriteRow& row : history_) {
            const bool unclaimedPending =
                row.used && row.state == RowState::Pending &&
                !row.slice.Valid();
            const bool noPhysicalFallback =
                row.used && row.state == RowState::WaitingFrontier &&
                row.noPhysicalTargets && !row.slice.Valid();
            if (unclaimedPending || noPhysicalFallback) {
                FinalizeRow(row, VariableOutcomeKind::TransportReset, {},
                            transportDrainFrame_);
            }
        }
    }

    const std::uint64_t quantum = timeline_.QuantumFrames();
    const std::uint64_t alignedStart =
        quantum == 0
            ? transportDrainFrame_
            : transportDrainFrame_ - transportDrainFrame_ % quantum;

    if (transportDrainState_ == TransportDrainState::DrainingCurrent) {
        for (const WriteRow& row : history_) {
            if (row.used && row.state != RowState::Terminal) {
                return false;
            }
        }
        if (slices_.HasUnreclaimedTransportRows(transportDrainEpoch_)) {
            return false;
        }
        if (transport_.Exhausted() ||
            !CanMintTransportResetBankVersions()) {
            return false;
        }
        if (!timeline_.CanResetForEpoch(alignedStart,
                                        transport_.Current() + 1)) {
            return false;
        }
        const EpochRaiseResult raised = transport_.RaiseEpoch();
        if (raised.status != EpochRaiseStatus::Raised) {
            return false;
        }
        transportDrainState_ = TransportDrainState::EpochRaised;
    }

    if (!timeline_.ResetForEpoch(alignedStart, transport_.Current())) {
        return false;
    }
    for (DefinitionRow& definition : definitions_) {
        definition.scheduledGlobal = definition.currentGlobal;
        definition.scheduledBankOrdinal = definition.currentBankOrdinal;
    }
    for (EmitterBank& bank : emitterBanks_) {
        for (OverrideRow& override : bank.overrides) {
            if (override.used && override.currentBankOrdinal != 0) {
                override.scheduledValue = override.currentValue;
                override.scheduledBankOrdinal = override.currentBankOrdinal;
            }
        }
    }
    ++globalBankVersionGeneration_;
    for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
        if (emitterBanks_[slot].generation != 0) {
            ++emitterBankVersionGenerations_[slot];
        }
    }
    serviceAudioFrame_ = transportDrainFrame_;
    transportDrainState_ = TransportDrainState::Idle;
    transportDrainEpoch_ = 0;
    transportDrainFrame_ = 0;
    return true;
}

bool VariableServiceV1::CanMintTransportResetBankVersions() const noexcept {
    if (globalBankVersionGeneration_ == 0 ||
        globalBankVersionGeneration_ == UINT32_MAX) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < emitterBanks_.size(); ++slot) {
        if (emitterBanks_[slot].generation != 0 &&
            (emitterBankVersionGenerations_[slot] == 0 ||
             emitterBankVersionGenerations_[slot] == UINT32_MAX)) {
            return false;
        }
    }
    return true;
}

void VariableServiceV1::TerminateSession() {
    for (WriteRow& row : history_) {
        if (row.used && row.state != RowState::Terminal) {
            FinalizeRow(row, VariableOutcomeKind::Session, {}, 0);
        }
    }
}

bool VariableServiceV1::NoteEmitterCreated(
    VnEmitterHandle emitter) noexcept {
    if (emitter == 0) return false;
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    if (scope.emitterSlot >= emitterBanks_.size() ||
        scope.emitterGeneration == 0) {
        return false;
    }
    EmitterBank& bank = emitterBanks_[scope.emitterSlot];
    if (bank.live) return false;
    bank.generation = scope.emitterGeneration;
    for (OverrideRow& override : bank.overrides) {
        override = OverrideRow{};
    }
    return true;
}

void VariableServiceV1::NoteEmitterDestroyed(Engine& engine,
                                             VnEmitterHandle emitter) {
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    if (scope.emitterSlot < emitterBanks_.size()) {
        EmitterBank& bank = emitterBanks_[scope.emitterSlot];
        if (bank.generation == scope.emitterGeneration) {
            bank.live = false;
            bank.generation = 0;
        }
        for (OverrideRow& override : bank.overrides) {
            override = OverrideRow{};
        }
    }
    for (WriteRow& row : history_) {
        if (!row.used || !row.batchLeader || row.state == RowState::Terminal ||
            !ScopeEqual(row.scope, scope)) {
            continue;
        }
        if (row.state == RowState::PageCommitted && row.slice.Valid()) {
            const VariableSliceInvalidationResult invalidation =
                slices_.Invalidate(row.slice, 1u);
            if (invalidation ==
                    VariableSliceInvalidationResult::InvalidatedBeforeClaim ||
                invalidation ==
                    VariableSliceInvalidationResult::AudioClaimedApplyWhole ||
                invalidation ==
                    VariableSliceInvalidationResult::AlreadyInvalidated) {
                row.emitterDestroyedBeforeApplied = true;
                SyncBatchState(row);
            }
            continue;
        }
        if (row.slice.Valid()) {
            ResetPreparedRow(engine, row);
        }
        FinalizeBatch(row, VariableOutcomeKind::EmitterDestroyed, {},
                    serviceAudioFrame_);
    }
}

bool VariableServiceV1::HasOutstandingWork() const noexcept {
    return pendingRows_ != 0;
}

bool VariableServiceV1::HasBlockingWrite(VnVariableId id) const noexcept {
    for (const WriteRow& row : history_) {
        if (row.used && row.state != RowState::Terminal &&
            row.variableId == id) {
            return true;
        }
    }
    return false;
}

VariableSliceRef VariableServiceV1::SliceForTicketForTests(
    std::uint64_t ticket) const noexcept {
    for (const WriteRow& row : history_) {
        if (row.used && row.ticket.sequence == ticket) {
            return row.slice;
        }
    }
    return {};
}

bool VariableServiceV1::EmitterBankActiveForTests(
    VnEmitterHandle emitter) const noexcept {
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    return FindEmitterBank(scope) != nullptr;
}

bool VariableServiceV1::CurrentValue(VnVariableId id, float* out) const noexcept {
    const DefinitionRow* definition = FindDefinition(id);
    if (definition == nullptr || out == nullptr) {
        return false;
    }
    *out = definition->currentGlobal;
    return true;
}

bool VariableServiceV1::CurrentValue(VnEmitterHandle emitter,
                                     VnVariableId id, float* out) const noexcept {
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    const EmitterBank* bank = FindEmitterBank(scope);
    if (bank != nullptr) {
        const OverrideRow* override = FindCurrentOverride(*bank, id);
        if (override != nullptr) {
            *out = override->currentValue;
            return true;
        }
    }
    return CurrentValue(id, out);
}

bool VariableServiceV1::CaptureCurrentBank(
    VnEmitterHandle emitter, const VnVariableId* variableIds,
    std::uint32_t variableCount, float* outValues,
    BankVersionToken* outToken) const noexcept {
    if (!hasWorld_ || outToken == nullptr ||
        (variableCount != 0 && (variableIds == nullptr || outValues == nullptr))) {
        return false;
    }
    VariableScopeKey scope{};
    scope.scope = VariableScopeV1::Emitter;
    scope.emitterSlot = static_cast<std::uint32_t>(emitter);
    scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
    const EmitterBank* bank = emitter == 0 ? nullptr : FindEmitterBank(scope);
    for (std::uint32_t index = 0; index < variableCount; ++index) {
        if (FindDefinition(variableIds[index]) == nullptr) return false;
    }
    BankVersionToken token{};
    if (emitter == 0) {
        token = {0, globalBankVersionGeneration_};
    } else {
        if (scope.emitterSlot >= emitterBanks_.size() ||
            emitterBanks_[scope.emitterSlot].generation !=
                scope.emitterGeneration) {
            return false;
        }
        token = {scope.emitterSlot + 1u,
                 emitterBankVersionGenerations_[scope.emitterSlot]};
    }
    if (!token.Valid()) return false;
    for (std::uint32_t index = 0; index < variableCount; ++index) {
        const DefinitionRow* definition = FindDefinition(variableIds[index]);
        const OverrideRow* override =
            bank == nullptr
                ? nullptr
                : FindCurrentOverride(*bank, variableIds[index]);
        outValues[index] = override == nullptr
                               ? definition->currentGlobal
                               : override->currentValue;
    }
    *outToken = token;
    return true;
}

bool VariableServiceV1::CaptureCurrentBankWithPreparedOverlay(
    VnEmitterHandle emitter, const VnVariableId* variableIds,
    std::uint32_t variableCount,
    std::uint32_t executionOrdinalExclusive, float* outValues,
    BankVersionToken* outToken) const noexcept {
    return CaptureCurrentBankWithPreparedOverlay(
        emitter, variableIds, variableCount, UINT64_MAX, UINT64_MAX,
        executionOrdinalExclusive, outValues, outToken);
}

bool VariableServiceV1::CaptureCurrentBankWithPreparedOverlay(
    VnEmitterHandle emitter, const VnVariableId* variableIds,
    std::uint32_t variableCount, std::uint64_t executeFrame,
    LogicCommitSequence sequence,
    std::uint32_t executionOrdinalExclusive, float* outValues,
    BankVersionToken* outToken) const noexcept {
    if (!CaptureCurrentBank(emitter, variableIds, variableCount, outValues,
                            outToken)) {
        return false;
    }
    const auto writeSequence = [&](const WriteRow& row) noexcept {
        return row.used ? row.ticket.sequence : sequence;
    };
    const auto applies = [&](const WriteRow& row) noexcept {
        if (row.world != worldGeneration_ ||
            row.transport != transport_.Current() ||
            row.state == RowState::Terminal) {
            return false;
        }
        if (row.used && row.state == RowState::Terminal) {
            return false;
        }
        if (!row.used && sequence == 0) {
            return false;
        }
        if (row.requestedFrame > executeFrame) {
            return false;
        }
        const LogicCommitSequence writeSeq = writeSequence(row);
        if (writeSeq != sequence) {
            return false;
        }
        if (row.requestedFrame < executeFrame) {
            return true;
        }
        return row.executionOrdinal < executionOrdinalExclusive;
    };
    const auto newerThan = [&](const WriteRow* current,
                               const WriteRow& candidate) noexcept {
        if (current == nullptr) {
            return true;
        }
        if (candidate.requestedFrame != current->requestedFrame) {
            return candidate.requestedFrame > current->requestedFrame;
        }
        const LogicCommitSequence currentSeq = writeSequence(*current);
        const LogicCommitSequence candidateSeq = writeSequence(candidate);
        if (candidateSeq != currentSeq) {
            return candidateSeq > currentSeq;
        }
        return candidate.executionOrdinal > current->executionOrdinal;
    };
    VariableScopeKey emitterScope{};
    emitterScope.scope = VariableScopeV1::Emitter;
    emitterScope.emitterSlot = static_cast<std::uint32_t>(emitter);
    emitterScope.emitterGeneration =
        static_cast<std::uint32_t>(emitter >> 32);
    const EmitterBank* emitterBank =
        emitter == 0 ? nullptr : FindEmitterBank(emitterScope);
    for (std::uint32_t index = 0; index < variableCount; ++index) {
        const WriteRow* latestGlobal = nullptr;
        const WriteRow* latestEmitter = nullptr;
        for (const WriteRow& row : history_) {
            if (row.variableId != variableIds[index] || !applies(row)) {
                continue;
            }
            if (row.scope.scope == VariableScopeV1::Global &&
                newerThan(latestGlobal, row)) {
                latestGlobal = &row;
            } else if (emitter != 0 && ScopeEqual(row.scope, emitterScope) &&
                       newerThan(latestEmitter, row)) {
                latestEmitter = &row;
            }
        }
        const DefinitionRow* definition = FindDefinition(variableIds[index]);
        float globalValue = definition->currentGlobal;
        if (latestGlobal != nullptr) {
            globalValue = latestGlobal->value;
        }
        const OverrideRow* committedOverride =
            emitterBank == nullptr
                ? nullptr
                : FindCurrentOverride(*emitterBank, variableIds[index]);
        if (latestEmitter != nullptr &&
            (latestGlobal == nullptr || newerThan(latestGlobal, *latestEmitter))) {
            outValues[index] = latestEmitter->value;
        } else if (latestGlobal != nullptr || committedOverride == nullptr) {
            outValues[index] = globalValue;
        }
    }
    return true;
}

bool VariableServiceV1::HasPendingBankWrite(
    VnEmitterHandle emitter, const VnVariableId* variableIds,
    std::uint32_t variableCount) const noexcept {
    if (variableIds == nullptr || variableCount == 0) return false;
    const std::uint32_t emitterSlot = static_cast<std::uint32_t>(emitter);
    const std::uint32_t emitterGeneration =
        static_cast<std::uint32_t>(emitter >> 32);
    for (const WriteRow& row : history_) {
        if (!row.used || row.state == RowState::Terminal ||
            row.world != worldGeneration_ ||
            row.transport != transport_.Current()) {
            continue;
        }
        const bool scopeMatches =
            row.scope.scope == VariableScopeV1::Global ||
            (emitter != 0 &&
             row.scope.scope == VariableScopeV1::Emitter &&
             row.scope.emitterSlot == emitterSlot &&
             row.scope.emitterGeneration == emitterGeneration);
        if (!scopeMatches) continue;
        for (std::uint32_t index = 0; index < variableCount; ++index) {
            if (row.variableId == variableIds[index]) return true;
        }
    }
    return false;
}

bool VariableServiceV1::HasPendingWriteForVoiceTarget(
    const ParamSystem& params, VnVoiceHandle handle,
    std::uint32_t targetId) const noexcept {
    for (const WriteRow& row : history_) {
        if (!row.used || row.state == RowState::Terminal) {
            continue;
        }
        bool matched = false;
        params.ForEachVariableTarget(
            row.variableId,
            [&](VnVoiceHandle candidate, VnEmitterHandle emitter,
                std::uint32_t candidateTargetId) {
                if (candidate != handle || candidateTargetId != targetId) {
                    return;
                }
                if (row.scope.scope == VariableScopeV1::Global) {
                    matched = true;
                    return;
                }
                const VnEmitterHandle exactEmitter =
                    (static_cast<std::uint64_t>(row.scope.emitterGeneration)
                     << 32) |
                    row.scope.emitterSlot;
                matched = emitter == exactEmitter;
            });
        if (matched) {
            return true;
        }
    }
    return false;
}

bool VariableServiceV1::HasPendingWriteForBusTarget(
    const BindingTable& bindings, std::uint32_t busIndex,
    std::uint32_t targetId) const noexcept {
    for (const WriteRow& row : history_) {
        if (!row.used || row.state == RowState::Terminal ||
            row.scope.scope != VariableScopeV1::Global) {
            continue;
        }
        bool matched = false;
        bindings.ForEachBusVariableTarget(
            row.variableId,
            [&](std::uint32_t candidateBus, std::uint32_t candidateTargetId) {
                matched = matched || (candidateBus == busIndex &&
                                      candidateTargetId == targetId);
            });
        if (matched) {
            return true;
        }
    }
    return false;
}

bool VariableServiceV1::ScheduledValue(VnEmitterHandle emitter,
                                       VnVariableId id, std::uint64_t frame,
                                       float* out,
                                       std::uint64_t* bankOrdinal,
                                       std::uint64_t* requestedFrame) const noexcept {
    if (out == nullptr) {
        return false;
    }
    if (bankOrdinal != nullptr) {
        *bankOrdinal = 0;
    }
    if (requestedFrame != nullptr) {
        *requestedFrame = 0;
    }
    // Every accepted nonterminal row owns an immutable candidate. Applied
    // values already live in the current banks and Terminal slots may recycle.
    const WriteRow* winner = nullptr;
    for (const WriteRow& row : history_) {
        if (row.used && row.state != RowState::Terminal &&
            row.world == worldGeneration_ && row.transport == transport_.Current() &&
            row.variableId == id && row.requestedFrame <= frame) {
            const bool emitterMatch =
                row.scope.scope == VariableScopeV1::Emitter &&
                row.scope.emitterSlot == static_cast<std::uint32_t>(emitter) &&
                row.scope.emitterGeneration ==
                    static_cast<std::uint32_t>(emitter >> 32);
            const bool global = row.scope.scope == VariableScopeV1::Global;
            if (!emitterMatch && !global) {
                continue;
            }
            if (winner == nullptr ||
                row.requestedFrame > winner->requestedFrame ||
                (row.requestedFrame == winner->requestedFrame &&
                 (row.ticket.sequence > winner->ticket.sequence ||
                  (row.ticket.sequence == winner->ticket.sequence &&
                   row.executionOrdinal > winner->executionOrdinal)))) {
                winner = &row;
            }
        }
    }
    if (winner != nullptr) {
        *out = winner->value;
        if (bankOrdinal != nullptr) {
            *bankOrdinal = winner->bankOrdinal;
        }
        if (requestedFrame != nullptr) {
            *requestedFrame = winner->requestedFrame;
        }
        return true;
    }
    return CurrentValue(emitter, id, out);
}

bool VariableServiceV1::PopOutcome(VariableOutcomeRecord* out) noexcept {
    if (out == nullptr || outcomeRead_ >= outcomes_.size()) {
        return false;
    }
    *out = outcomes_[outcomeRead_++];
    return true;
}

void VariableServiceV1::MarkOutcomeDeliveryComplete() noexcept {
    if (outcomes_.empty()) {
        return;
    }
    deliveredOutcomeCount_ = outcomes_.size();
    deliveredOutcomeGeneration_ = currentOutcomeGeneration_;
    if (currentOutcomeGeneration_ != UINT64_MAX) {
        ++currentOutcomeGeneration_;
    }
}

void VariableServiceV1::FinishOutcomeDelivery() noexcept {
    if (deliveredOutcomeGeneration_ != 0) {
        for (std::uint32_t slot = 0; slot < history_.size(); ++slot) {
            WriteRow& row = history_[slot];
            if (row.used && row.state == RowState::Terminal &&
                row.outcomeGeneration <= deliveredOutcomeGeneration_) {
                row = WriteRow{};
                freeSlots_.push_back(slot);
            }
        }
        outcomes_.erase(
            outcomes_.begin(),
            outcomes_.begin() +
                static_cast<std::ptrdiff_t>(deliveredOutcomeCount_));
        deliveredOutcomeCount_ = 0;
        deliveredOutcomeGeneration_ = 0;
    }
    outcomeRead_ = 0;
}

std::uint64_t VariableServiceV1::PendingCount() const noexcept {
    return pendingRows_;
}

std::uint32_t VariableServiceV1::MapPublicOutcome(
    VariableOutcomeKind outcome) noexcept {
    switch (outcome) {
    case VariableOutcomeKind::Applied:
        return VN_VARIABLE_OUTCOME_APPLIED;
    case VariableOutcomeKind::Superseded:
        return VN_VARIABLE_OUTCOME_SUPERSEDED;
    case VariableOutcomeKind::WorldReplacement:
        return VN_VARIABLE_OUTCOME_TERMINATED_WORLD;
    case VariableOutcomeKind::TransportReset:
        return VN_VARIABLE_OUTCOME_TERMINATED_TRANSPORT;
    case VariableOutcomeKind::EmitterDestroyed:
        return VN_VARIABLE_OUTCOME_TERMINATED_EMITTER;
    case VariableOutcomeKind::Session:
        return VN_VARIABLE_OUTCOME_TERMINATED_SESSION;
    case VariableOutcomeKind::TargetRejected:
        return VN_VARIABLE_OUTCOME_REJECTED_TARGET;
    }
    return VN_VARIABLE_OUTCOME_PENDING;
}

void VariableServiceV1::PushExpiredTicket(
    const TicketQueryRecord& record) noexcept {
    if (record.sequence == 0) {
        return;
    }
    if (expiredTicketCount_ == kVariableHistoryCapacity) {
        expiredTicketBegin_ =
            (expiredTicketBegin_ + 1u) % kVariableHistoryCapacity;
        --expiredTicketCount_;
    }
    const std::uint32_t index =
        (expiredTicketBegin_ + expiredTicketCount_) % kVariableHistoryCapacity;
    expiredTicketRecords_[index] = {record.sequence, record.world, record.transport};
    ++expiredTicketCount_;
}

bool VariableServiceV1::TicketExpired(
    const VnVariableTicketV1& ticket) const noexcept {
    for (std::uint32_t i = 0; i < expiredTicketCount_; ++i) {
        const std::uint32_t index =
            (expiredTicketBegin_ + i) % kVariableHistoryCapacity;
        const auto& record = expiredTicketRecords_[index];
        if (record.sequence == ticket.sequence &&
            record.worldGeneration == ticket.worldGeneration &&
            record.transportGeneration == ticket.transportGeneration) {
            return true;
        }
    }
    return false;
}

const VariableServiceV1::TicketQueryRecord*
VariableServiceV1::FindTicketQuery(LogicCommitSequence sequence,
                                   WorldGeneration world,
                                   TransportEpoch transport) const noexcept {
    for (std::uint32_t i = 0; i < ticketQueryCount_; ++i) {
        const std::uint32_t index =
            (ticketQueryBegin_ + i) % kVariableHistoryCapacity;
        const TicketQueryRecord& row = ticketQueryRecords_[index];
        if (row.sequence == sequence && row.world == world &&
            row.transport == transport) {
            return &row;
        }
    }
    return nullptr;
}

void VariableServiceV1::RememberAcceptedTicket(
    LogicCommitSequence sequence, WorldGeneration world,
    TransportEpoch transport) noexcept {
    if (sequence == 0) {
        return;
    }
    if (FindTicketQuery(sequence, world, transport) != nullptr) {
        return;
    }
    if (ticketQueryCount_ == kVariableHistoryCapacity) {
        const TicketQueryRecord& old =
            ticketQueryRecords_[ticketQueryBegin_];
        PushExpiredTicket(old);
        ticketQueryBegin_ =
            (ticketQueryBegin_ + 1u) % kVariableHistoryCapacity;
        --ticketQueryCount_;
    }
    const std::uint32_t index =
        (ticketQueryBegin_ + ticketQueryCount_) % kVariableHistoryCapacity;
    TicketQueryRecord& row = ticketQueryRecords_[index];
    row.sequence = sequence;
    row.world = world;
    row.transport = transport;
    row.outcome = VariableOutcomeKind::Applied;
    row.pending = true;
    ++ticketQueryCount_;
}

void VariableServiceV1::RememberFinalizedTicket(
    LogicCommitSequence sequence, VariableOutcomeKind outcome) noexcept {
    if (sequence == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < ticketQueryCount_; ++i) {
        const std::uint32_t index =
            (ticketQueryBegin_ + i) % kVariableHistoryCapacity;
        TicketQueryRecord& row = ticketQueryRecords_[index];
        if (row.sequence == sequence) {
            row.pending = false;
            row.outcome = outcome;
            return;
        }
    }
    RememberAcceptedTicket(sequence, worldGeneration_, transport_.Current());
    RememberFinalizedTicket(sequence, outcome);
}

void VariableServiceV1::QueryVariableTicket(
    const VnVariableTicketV1& ticket, std::uint32_t* status,
    std::uint32_t* outcome) const noexcept {
    if (status == nullptr || outcome == nullptr) {
        return;
    }
    *status = VN_VARIABLE_TICKET_UNKNOWN;
    *outcome = VN_VARIABLE_OUTCOME_PENDING;
    if (ticket.sequence == 0) {
        return;
    }
    bool pending = false;
    for (const WriteRow& row : history_) {
        if (!row.used || row.ticket.sequence != ticket.sequence) {
            continue;
        }
        if (row.world != ticket.worldGeneration ||
            row.transport != ticket.transportGeneration) {
            continue;
        }
        if (row.state != RowState::Terminal) {
            pending = true;
            break;
        }
    }
    if (pending) {
        *status = VN_VARIABLE_TICKET_FOUND;
        *outcome = VN_VARIABLE_OUTCOME_PENDING;
        return;
    }
    const TicketQueryRecord* record = FindTicketQuery(
        ticket.sequence, ticket.worldGeneration, ticket.transportGeneration);
    if (record != nullptr) {
        *status = VN_VARIABLE_TICKET_FOUND;
        *outcome = record->pending ? VN_VARIABLE_OUTCOME_PENDING
                                   : MapPublicOutcome(record->outcome);
        return;
    }
    if (TicketExpired(ticket)) {
        *status = VN_VARIABLE_TICKET_OUTCOME_EXPIRED;
    }
}

VnResult VariableServiceV1::QueryVariable(
    VnVariableId id, VnEmitterHandle emitter, const EmitterTable& emitters,
    VnVariableQueryResultV1* out) const noexcept {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    const std::uint32_t structSize = out->structSize;
    *out = VnVariableQueryResultV1{};
    out->structSize = structSize;
    const DefinitionRow* definition = FindDefinition(id);
    if (definition == nullptr) {
        return VN_NO_VARIABLE;
    }
    if (emitter != 0 && !emitters.IsLive(emitter)) {
        return VN_STALE_HANDLE;
    }
    float current = definition->currentGlobal;
    std::uint32_t hasOverride = 0;
    if (emitter != 0) {
        VariableScopeKey scope{};
        scope.scope = VariableScopeV1::Emitter;
        scope.emitterSlot = static_cast<std::uint32_t>(emitter);
        scope.emitterGeneration = static_cast<std::uint32_t>(emitter >> 32);
        const EmitterBank* bank = FindEmitterBank(scope);
        const OverrideRow* override =
            bank == nullptr ? nullptr : FindCurrentOverride(*bank, id);
        if (override != nullptr) {
            current = override->currentValue;
            hasOverride = 1;
        }
    }
    std::uint32_t pending = 0;
    for (const WriteRow& row : history_) {
        if (!row.used || row.state == RowState::Terminal ||
            row.variableId != id) {
            continue;
        }
        if (emitter == 0) {
            if (row.scope.scope == VariableScopeV1::Global) {
                ++pending;
            }
        } else if (row.scope.scope == VariableScopeV1::Global ||
                  (row.scope.scope == VariableScopeV1::Emitter &&
                   row.scope.emitterSlot ==
                       static_cast<std::uint32_t>(emitter) &&
                   row.scope.emitterGeneration ==
                       static_cast<std::uint32_t>(emitter >> 32))) {
            ++pending;
        }
    }
    out->kind = static_cast<std::uint32_t>(definition->definition.kind);
    out->scope = static_cast<std::uint32_t>(hasOverride != 0
        ? VariableScopeV1::Emitter : VariableScopeV1::Global);
    out->writers = definition->writers;
    out->rangeMin = definition->definition.rangeMin;
    out->rangeMax = definition->definition.rangeMax;
    out->defaultValue = definition->definition.defaultValue;
    out->currentValue = current;
    out->hasEmitterOverride = hasOverride;
    out->pendingCount = pending;
    out->executionFingerprint = definition->executionFingerprint;
    return VN_OK;
}

std::uint64_t VariableServiceV1::ExecutionCatalogFingerprint() const noexcept {
    VariableCatalogFingerprintRowV1 rows[kVariableDefinitionCapacity];
    const std::uint32_t count = std::min(
        static_cast<std::uint32_t>(definitions_.size()),
        kVariableDefinitionCapacity);
    for (std::uint32_t i = 0; i < count; ++i) {
        rows[i].variableId = definitions_[i].definition.variableId;
        rows[i].fingerprint = definitions_[i].executionFingerprint;
    }
    std::sort(rows, rows + count,
              [](const VariableCatalogFingerprintRowV1& a,
                 const VariableCatalogFingerprintRowV1& b) {
                  return a.variableId < b.variableId;
              });
    return ComputeVariableExecutionCatalogFingerprintV1(rows, count);
}

VnResult VariableServiceV1::ResolveLabel(
    VnVariableId variable, VnObjectId label, float* value) const noexcept {
    if (value == nullptr || label == VnObjectId{}) return VN_INVALID_ARG;
    const auto* definition = FindDefinition(variable);
    if (!definition) return VN_NO_VARIABLE;
    for (const auto& catalog : labelCatalog_) {
        if (catalog.variableId != variable) continue;
        for (const auto& candidate : catalog.labels) {
            if (candidate.labelId != label) continue;
            if (candidate.anchor < definition->definition.rangeMin
                || candidate.anchor > definition->definition.rangeMax) return VN_NO_VARIABLE_LABEL;
            *value = static_cast<float>(candidate.anchor);
            return VN_OK;
        }
    }
    return VN_NO_VARIABLE_LABEL;
}

VnResult VariableServiceV1::ResolveLabel(
    VnVariableId variable, std::string_view name, float* value) const noexcept {
    if (value == nullptr || name.empty()) return VN_INVALID_ARG;
    const auto* definition = FindDefinition(variable);
    if (!definition) return VN_NO_VARIABLE;
    for (const auto& catalog : labelCatalog_) {
        if (catalog.variableId != variable) continue;
        for (const auto& candidate : catalog.labels) {
            if (candidate.name != name) continue;
            if (candidate.anchor < definition->definition.rangeMin
                || candidate.anchor > definition->definition.rangeMax) return VN_NO_VARIABLE_LABEL;
            *value = static_cast<float>(candidate.anchor);
            return VN_OK;
        }
    }
    return VN_NO_VARIABLE_LABEL;
}

namespace test {

void SetSdkCatalogFingerprint(VariableServiceV1& service,
                              std::uint64_t fingerprint) noexcept {
    service.sdkCatalogFingerprint_ = fingerprint;
}

bool SetDefinitionExecutionFingerprint(VariableServiceV1& service,
                                       VnVariableId id,
                                       std::uint64_t fingerprint) noexcept {
    VariableServiceV1::DefinitionRow* row = service.FindDefinition(id);
    if (row == nullptr) {
        return false;
    }
    row->executionFingerprint = fingerprint;
    return true;
}

} // namespace test

} // namespace harken
