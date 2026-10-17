#include "harken/param/VariableService.h"

#include "Engine.h"
#include "MonitorCaptureHub.h"
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
#include <stdexcept>
#include <utility>

namespace harken {

namespace {

class VariableProjectionGuard {
public:
    explicit VariableProjectionGuard(Engine& engine) noexcept : engine_(engine) {
        params_ = engine_.params.BeginVariableProjection();
        if (params_) emitters_ = engine_.emitters.BeginVariableProjection();
        if (emitters_) bindings_ = engine_.bindings.BeginVariableProjection();
        if (bindings_) buses_ = engine_.busTargets.BeginVariableProjection();
        if (buses_) effects_ = engine_.effectParamTargets.BeginVariableProjection();
    }
    VariableProjectionGuard(const VariableProjectionGuard&) = delete;
    VariableProjectionGuard& operator=(const VariableProjectionGuard&) = delete;
    ~VariableProjectionGuard() { Restore(); }
    bool Valid() const noexcept { return effects_; }
    void Restore() noexcept {
        if (effects_) engine_.effectParamTargets.RestoreVariableProjection();
        if (buses_) engine_.busTargets.RestoreVariableProjection();
        if (bindings_) engine_.bindings.RestoreVariableProjection();
        if (emitters_) engine_.emitters.RestoreVariableProjection();
        if (params_) engine_.params.RestoreVariableProjection();
        params_ = emitters_ = bindings_ = buses_ = effects_ = false;
    }
private:
    Engine& engine_;
    bool params_ = false, emitters_ = false, bindings_ = false;
    bool buses_ = false, effects_ = false;
};

} // namespace

WorldInstallResult InstallContainerWorldV1(
    Engine& engine, const ContainerWorldInstallViewV1& install,
    CurrentWorldTraceOwnerV1 traceOwner) noexcept {
    VariableOperationScope variableOperation(
        traceOwner == CurrentWorldTraceOwnerV1::Internal ? &engine : nullptr);
    harken::monitor::IdentityMutationScope identityMutation;
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
        if (traceOwner == CurrentWorldTraceOwnerV1::Internal &&
            engine.trace != nullptr && engine.trace->Active()) {
            engine.trace->RecordCurrentWorldInstall(install, result, 1);
        }
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
    VariableSliceTable& slices, ScheduledTransactionPool& timeline,
    std::uint64_t readCutCapacity, std::uint32_t timelineReadUseCapacity,
    std::uint32_t meterProbeCapacity)
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
    const auto cells = std::uint64_t{kVariableDefinitionCapacity} +
        std::uint64_t{maxEmitters_} * overridesPerEmitter_;
    if (cells > UINT32_MAX) throw std::length_error("variable cell capacity overflow");
    queryHistoryCapacity_ = static_cast<std::uint32_t>(std::max<std::uint64_t>(kVariableHistoryCapacity, cells));
    const auto revisions = cells + queryHistoryCapacity_;
    const auto storage = revisions + kVariableBatchWriteCapacity;
    const auto proofs = storage + 2ull * meterProbeCapacity;
    if (proofs > UINT32_MAX) throw std::length_error("variable revision capacity overflow");
    revisionCapacity_ = static_cast<std::uint32_t>(revisions);
    completionCapacity_ = static_cast<std::uint32_t>(proofs);
    applyPayloadCapacity_ = static_cast<std::uint32_t>(revisions + 2ull * meterProbeCapacity);
    history_.resize(static_cast<std::size_t>(storage));
    preparedWritePlacements_.resize(history_.size());
    historyGenerations_.resize(history_.size(), 1);
    sealRows_.resize(history_.size());
    commitRows_.resize(history_.size());
    prefixRows_.resize(history_.size());
    readCutRows_.resize(history_.size());
    actionReadCutRows_.resize(history_.size());
    preparationRows_.resize(history_.size());
    settledRows_.resize(history_.size());
    supersedeRows_.resize(history_.size());
    scheduledGroups_.resize(history_.size());
    serviceCandidates_.resize(history_.size());
    dependencyVisited_.resize(completionCapacity_);
    if (readCutCapacity > UINT32_MAX) throw std::length_error("variable read cut capacity overflow");
    readCutCapacity_ = static_cast<std::uint32_t>(readCutCapacity);
    readCuts_ = std::make_unique<ReadCutRow[]>(readCutCapacity_);
    const auto candidateWords = (storage + 63u) / 64u;
    if (candidateWords * readCutCapacity_ > SIZE_MAX / sizeof(std::uint64_t))
        throw std::length_error("variable candidate storage overflow");
    candidatePinStorage_.resize(static_cast<std::size_t>(candidateWords * readCutCapacity_));
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index)
        readCuts_[index].candidatePins.words = std::span(candidatePinStorage_).subspan(
            static_cast<std::size_t>(index * candidateWords), static_cast<std::size_t>(candidateWords));
    timelineReadUseCapacity_ = timelineReadUseCapacity;
    timelineReadUses_ = std::make_unique<TimelineReadUseRow[]>(timelineReadUseCapacity_);
    freeSlots_.reserve(history_.size());
    for (std::uint32_t slot = static_cast<std::uint32_t>(history_.size()); slot > 0; --slot) {
        freeSlots_.push_back(slot - 1);
    }
    outcomes_.reserve(history_.size());
    ticketQueryRecords_.assign(queryHistoryCapacity_, TicketQueryRecord{});
    expiredTicketRecords_.assign(queryHistoryCapacity_, VnVariableTicketV1{});
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
        if (row.state == RowState::Pending && !row.slice.Valid()) {
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

bool VariableServiceV1::Precedes(const WriteRow& a, const WriteRow& b) noexcept {
    if (a.requestedFrame != b.requestedFrame) return a.requestedFrame < b.requestedFrame;
    if (a.ticket.sequence != b.ticket.sequence) return a.ticket.sequence < b.ticket.sequence;
    if (a.executionOrdinal != b.executionOrdinal) return a.executionOrdinal < b.executionOrdinal;
    return a.bankOrdinal < b.bankOrdinal;
}

bool VariableServiceV1::PublicationPrecedes(const WriteRow& a, const WriteRow& b) noexcept {
    if (a.committedFrame != b.committedFrame) return a.committedFrame < b.committedFrame;
    if (a.ticket.sequence != b.ticket.sequence) return a.ticket.sequence < b.ticket.sequence;
    if (a.executionOrdinal != b.executionOrdinal) return a.executionOrdinal < b.executionOrdinal;
    return a.bankOrdinal < b.bankOrdinal;
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

bool VariableServiceV1::SamePhysicalTarget(const VariableTargetLocator& a,
                                          const VariableTargetLocator& b) noexcept {
    if (a.scope != b.scope || a.object != b.object ||
        a.objectGeneration != b.objectGeneration) return false;
    if (SameTarget(a, b)) return true;
    if (a.scope == VariableTargetScope::Voice) {
        const auto section = [](std::uint32_t target) {
            switch (target) {
            case kTargetVoiceLowpassCutoff:
            case kTargetVoiceLowpassQ: return 0u;
            case kTargetVoiceHighpassCutoff:
            case kTargetVoiceHighpassQ: return 1u;
            case kTargetVoiceLowShelfGainDb:
            case kTargetVoiceLowShelfFreqHz:
            case kTargetVoiceLowShelfQ: return 2u;
            case kTargetVoiceHighShelfGainDb:
            case kTargetVoiceHighShelfFreqHz:
            case kTargetVoiceHighShelfQ: return 3u;
            default: return UINT32_MAX;
            }
        };
        return section(a.targetId) != UINT32_MAX &&
               section(a.targetId) == section(b.targetId);
    }
    if (a.scope != VariableTargetScope::VoiceEffect &&
        a.scope != VariableTargetScope::BusEffect) return false;
    if (a.effectSlot != b.effectSlot || a.effectType != b.effectType) return false;
    if (a.effectType == VN_EFFECT_COMPRESSOR) return true;
    if (a.effectType == VN_EFFECT_THREE_BAND_TONE) {
        const auto section = [](std::uint32_t param) {
            if (param <= kToneParamLowShelfFreqHz) return 0u;
            if (param <= kToneParamPeakQ) return 1u;
            if (param <= kToneParamHighShelfFreqHz) return 2u;
            return UINT32_MAX;
        };
        return section(a.paramIndex) != UINT32_MAX &&
               section(a.paramIndex) == section(b.paramIndex);
    }
    return false;
}

VariableAcceptResult VariableServiceV1::Prepare(
    WriteOperation operation, const VariableScopeKey& scope, VnVariableId id,
    float value, const VariableCallObservation& observed,
    std::uint32_t executionOrdinal, PreparedVariableWriteToken* out,
    VariableWriterIdentityV1 writer, bool staging) noexcept {
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
    if (freeSlots_.empty() || (!staging && occupiedRevisions_ >= revisionCapacity_)) {
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
                if (prepared.reserved &&
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
                    if (prepared.reserved &&
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
    if (publicationWorld_ != worldGeneration_ || publicationTransport_ != transport_.Current()) {
        if (publicationOwner_.generation == UINT32_MAX) {
            result.result = VN_BACKPRESSURE;
            ++refusalCount_;
            return result;
        }
        publicationOwner_ = {0, publicationOwner_.generation + 1u,
                             TargetClaimKind::VariablePublication};
        publicationWorld_ = worldGeneration_;
        publicationTransport_ = transport_.Current();
    }
    WriteRow row{};
    row.reserved = true;
    row.publicationOwner = publicationOwner_;
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
    ++occupiedRevisions_;
    ++preparedRows_;
    out->owner_ = this;
    out->slot_ = slot;
    out->prepared_ = true;
    result.result = VN_OK;
    return result;
}

bool VariableServiceV1::CaptureActionOrder(const LogicCommitCoordinator::PreparedIssue& issue,
    ActionOrderIdentity* out) const noexcept {
    return out != nullptr && commits_.CapturePreparedIssueIdentity(issue, &out->ordinary);
}

bool VariableServiceV1::CaptureActionOrder(Engine& engine,
    const ActionFiringTable::PreparedChildIdentity& child, ActionOrderIdentity* out) const noexcept {
    if (out == nullptr || &engine.variables != this || &engine.logicCommits != &commits_ ||
        !engine.actionFirings.IsPreparedChildActive(child)) return false;
    out->deferred = child;
    out->deferredTable = &engine.actionFirings;
    return true;
}

bool VariableServiceV1::IsPreparedActionOrderActive(const ActionOrderIdentity& identity) const noexcept {
    return commits_.IsPreparedIssueActive(identity.ordinary) ||
        (identity.deferredTable != nullptr && identity.deferredTable->IsPreparedChildActive(identity.deferred));
}

bool VariableServiceV1::MatchesConsumedActionOrder(const ActionOrderIdentity& identity,
    const LogicCommitCoordinator::ConsumedIssue* consumed,
    const ActionFiringTable::ChildActivationReceipt* activated) const noexcept {
    return (consumed != nullptr && consumed->MatchesOriginalReservation(identity.ordinary)) ||
        (activated != nullptr && identity.deferredTable != nullptr &&
            activated->MatchesOriginalPreparation(identity.deferred) &&
            identity.deferredTable->IsChildActivationCurrent(*activated));
}

bool VariableServiceV1::BindPreparedActionWriteOrder(PreparedVariableWriteToken& prepared,
    const LogicCommitCoordinator::PreparedIssue& issue) noexcept {
    ActionOrderIdentity identity{};
    return CaptureActionOrder(issue, &identity) &&
        BindPreparedActionWriteOrderImpl(prepared, identity, issue.Sequence());
}

bool VariableServiceV1::BindPreparedActionWriteOrder(PreparedVariableWriteToken& prepared, Engine& engine,
    const ActionFiringTable::PreparedChildIdentity& child) noexcept {
    ActionOrderIdentity identity{};
    return CaptureActionOrder(engine, child, &identity) &&
        BindPreparedActionWriteOrderImpl(prepared, identity, child.Sequence());
}

bool VariableServiceV1::BindPreparedActionWriteOrderImpl(PreparedVariableWriteToken& prepared,
    const ActionOrderIdentity& identity, LogicCommitSequence sequence) noexcept {
    if (prepared.owner_ != this || !prepared.prepared_ || prepared.slot_ >= history_.size() ||
        !IsPreparedActionOrderActive(identity) || sequence == 0) return false;
    auto& row = history_[prepared.slot_];
    if (!row.reserved || row.used || row.state != RowState::Pending) return false;
    if (row.ticket.Valid()) return row.preparedIssue.SameReservation(identity);
    if (nextBankOrdinal_ == UINT64_MAX) return false;
    row.ticket = {sequence};
    row.preparedIssue = identity;
    row.bankOrdinal = nextBankOrdinal_++;
    InsertScheduledGroup(row);
    return true;
}

void VariableServiceV1::CommitPreparedWrite(
    PreparedVariableWriteToken* prepared,
    LogicCommitSequence sequence) noexcept {
    CommitPreparedWriteImpl(prepared, sequence, nullptr);
}

void VariableServiceV1::CommitPreparedActionWrite(PreparedVariableWriteToken* prepared,
    const LogicCommitCoordinator::ConsumedIssue& consumed) noexcept {
    CommitPreparedWriteImpl(prepared, consumed.Sequence(), &consumed);
}

void VariableServiceV1::CommitPreparedActionWrite(PreparedVariableWriteToken* prepared,
    const ActionFiringTable::ChildActivationReceipt& activated) noexcept {
    CommitPreparedWriteImpl(prepared, activated.Sequence(), nullptr, &activated);
}

void VariableServiceV1::CommitPreparedWriteImpl(PreparedVariableWriteToken* prepared,
    LogicCommitSequence sequence, const LogicCommitCoordinator::ConsumedIssue* consumed,
    const ActionFiringTable::ChildActivationReceipt* activated, bool supersede) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || prepared->slot_ >= history_.size() ||
        sequence == 0) {
        return;
    }
    WriteRow& row = history_[prepared->slot_];
    if (row.used) {
        return;
    }
    if (row.preparedIssue.HasReservation() &&
        !MatchesConsumedActionOrder(row.preparedIssue, consumed, activated)) return;
    if (row.preparedEngine != nullptr && !row.preparedSourcesSealed) return;
    const LogicCommitTicket ticket{sequence};
    assert(!row.ticket.Valid() || row.ticket.sequence == sequence);
    if (row.ticket.Valid() && row.ticket.sequence != sequence) return;
    row.used = true;
    row.reserved = false;
    row.ticket = ticket;
    if (row.bankOrdinal == 0) row.bankOrdinal = nextBankOrdinal_++;
    InsertScheduledGroup(row);
    RegisterCommittedPendingReadCutCandidate(prepared->slot_);
    if (supersede) for (WriteRow& prior : history_) {
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
        historyHighWater_, occupiedRevisions_);
    ++acceptedCount_;
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->prepared_ = false;
}

bool VariableServiceV1::SealPreparedActionSources(const LogicCommitCoordinator::PreparedIssue& issue) noexcept {
    ActionOrderIdentity identity{};
    return CaptureActionOrder(issue, &identity) && SealPreparedActionSourcesImpl(identity);
}

bool VariableServiceV1::SealPreparedActionSources(Engine& engine,
    const ActionFiringTable::PreparedChildIdentity& child) noexcept {
    ActionOrderIdentity identity{};
    return CaptureActionOrder(engine, child, &identity) && SealPreparedActionSourcesImpl(identity);
}

bool VariableServiceV1::SealPreparedActionSourcesImpl(const ActionOrderIdentity& identity) noexcept {
    if (!IsPreparedActionOrderActive(identity)) return false;
    auto& rows = sealRows_;
    std::size_t count = 0;
    for (auto& row : history_) {
        if (row.preparedEngine == nullptr || !row.preparedIssue.SameReservation(identity)) continue;
        row.preparedSourcesSealed = false;
        const auto& held = preparedWritePlacements_[static_cast<std::size_t>(&row - history_.data())];
        if (!row.reserved || row.used || row.state != RowState::SlicePrepared || !row.historySlicePin ||
            !row.placementHeld || !timeline_.CanReleaseHeldCurrentSlice(held) ||
            !slices_.CanPublishPrepared(row.slice, true)) return false;
        if (count == rows.size()) return false;
        rows[count++] = &row;
    }
    std::sort(rows.begin(), rows.begin() + count,
        [](const WriteRow* a, const WriteRow* b) { return PublicationPrecedes(*a, *b); });
    for (std::size_t index = 0; index < count; ++index) {
        std::uint32_t cursor = 0;
        while (const auto* dependency = slices_.NextSource(rows[index]->slice, &cursor)) {
            if (slices_.State(dependency->slice) != VariableSliceState::Prepared) continue;
            const bool earlier = std::any_of(rows.begin(), rows.begin() + index,
                [&](const auto* prior) { return prior->slice.slot == dependency->slice.slot &&
                    prior->slice.generation == dependency->slice.generation; });
            if (!earlier) return false;
        }
    }
    for (std::size_t index = 0; index < count; ++index) rows[index]->preparedSourcesSealed = true;
    return true;
}

void VariableServiceV1::CommitPreparedActionSources(const LogicCommitCoordinator::ConsumedIssue& consumed) noexcept {
    CommitPreparedActionSourcesImpl(&consumed, nullptr);
}

void VariableServiceV1::CommitPreparedActionSources(const ActionFiringTable::ChildActivationReceipt& activated) noexcept {
    CommitPreparedActionSourcesImpl(nullptr, &activated);
}

void VariableServiceV1::CommitPreparedActionSourcesImpl(const LogicCommitCoordinator::ConsumedIssue* consumed,
    const ActionFiringTable::ChildActivationReceipt* activated) noexcept {
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        if (cut.state.load(std::memory_order_acquire) == ReadCutState::Capturing &&
            cut.pendingPlacement && MatchesConsumedActionOrder(cut.preparedIssue, consumed, activated))
            cut.actionOrderConsumed = true;
    }
    auto& rows = commitRows_;
    std::size_t count = 0;
    for (auto& row : history_) {
        if (row.preparedEngine == nullptr || !MatchesConsumedActionOrder(row.preparedIssue, consumed, activated)) continue;
        assert(row.used && !row.reserved && row.state == RowState::SlicePrepared && row.preparedSourcesSealed);
        if (!row.used || row.reserved || row.state != RowState::SlicePrepared || !row.preparedSourcesSealed) return;
        rows[count++] = &row;
    }
    std::sort(rows.begin(), rows.begin() + count,
        [](const WriteRow* a, const WriteRow* b) { return PublicationPrecedes(*a, *b); });
    for (std::size_t index = 0; index < count; ++index) {
        auto& row = *rows[index];
        const bool published = slices_.Publish(row.slice);
        assert(published);
        if (!published) return;
        row.state = RowState::PageCommitted;
    }
    // Every source is committed before any of the held pages can become visible.
    for (std::size_t index = 0; index < count; ++index) {
        auto& row = *rows[index];
        auto& held = preparedWritePlacements_[static_cast<std::size_t>(&row - history_.data())];
        const bool released = timeline_.ReleaseHeldCurrentSlice(&held);
        assert(released);
        (void)released;
        row.placementHeld = false;
        row.preparedEngine = nullptr;
        pendingPlaybackPublicationThrough_ = std::max(pendingPlaybackPublicationThrough_,
            row.committedFrame + timeline_.QuantumFrames());
    }
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
    EraseScheduledGroup(row);
    for (auto& related : history_) {
        if (related.preparedIssue.SameReservation(row.preparedIssue)) related.preparedSourcesSealed = false;
    }
    if (row.preparedEngine != nullptr) {
        auto& held = preparedWritePlacements_[prepared->slot_];
        if (held.IsHeld()) {
            const bool cancelled = timeline_.CancelHeldCurrentSlice(&held);
            assert(cancelled);
            (void)cancelled;
        }
        row.placementHeld = false;
        ReleaseSliceClaims(*row.preparedEngine, row);
        row.preparedEngine = nullptr;
        row.cancelledBeforePublication = true;
    }
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        const auto state = cut.state.load(std::memory_order_acquire);
        if (state != ReadCutState::Capturing && state != ReadCutState::Published) continue;
        for (std::uint32_t source = 0; source < cut.sourceCount; ++source) {
            const auto& ref = cut.sources[source];
            const auto* observed = ResolveHistory(ref);
            if (observed == &row || (observed != nullptr && row.slice.Valid() &&
                PreparedSourceDependsOn(observed->slice, row.slice))) {
                cut.preparationFailed.store(true, std::memory_order_release);
                break;
            }
        }
    }
    row.reserved = false;
    row.emitterOverrideSlot = UINT32_MAX;
    if (row.cancelledBeforePublication) (void)slices_.ClearPreparedSources(row.slice);
    row.state = RowState::Terminal;
    row.outcomeDelivered = true;
    row.finalOutcome = VariableOutcomeKind::TargetRejected;
    ReclaimHistoryRows();
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
    for (const WriteRow* member : members) {
        if (member != nullptr && (member->observers != 0 || member->slice.Valid() ||
            member->placementHeld || member->state != RowState::Pending)) return false;
    }
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
            0, &batch.rows_[i], writer, true);
        if (row.result != VN_OK) {
            batch.count_ = i;
            result.result = row.result;
            result.observation = row.observation;
            return result;
        }
        batch.count_ = i + 1;
    }
    LogicCommitCoordinator::PreparedIssue issue;
    if (commits_.PrepareIssue(&issue) !=
        LogicCommitCoordinator::IssueStatus::Issued) {
        result.result = VN_BACKPRESSURE;
        ++refusalCount_;
        return result;
    }
    const auto sequence = issue.Sequence();
    std::size_t victimCount = 0;
    std::size_t reusable = 0;
    for (auto& prior : history_) {
        if (!prior.used || !prior.batchLeader || prior.state != RowState::Pending) continue;
        const auto members = BatchRows(prior);
        bool hit = false;
        for (auto* member : members) {
            if (member == nullptr) continue;
            for (std::uint32_t index = 0; index < batch.count_; ++index) {
                const auto& next = history_[batch.rows_[index].slot_];
                if ((wholeBatchSupersede ? BatchSupersedeKeyIntersect(*member, next) : RowKeyEqual(*member, next)) &&
                    CanSupersede(*member, next, sequence)) hit = true;
            }
        }
        if (!hit) continue;
        for (auto* member : members) {
            if (member == nullptr) continue;
            supersedeRows_[victimCount++] = member;
            const auto slot = static_cast<std::size_t>(member - history_.data());
            if (historyGenerations_[slot] != UINT32_MAX) ++reusable;
        }
    }
    if (occupiedRevisions_ - reusable > revisionCapacity_ ||
        victimCount > outcomes_.capacity() - outcomes_.size()) {
        result.result = VN_VARIABLE_TRANSACTION_EXHAUSTED;
        ++refusalCount_;
        return result;
    }
    const auto consumed = commits_.ConsumePreparedIssue(std::move(issue));
    assert(consumed.Sequence() == sequence);
    for (std::uint32_t i = 0; i < batch.count_; ++i) {
        auto& member = history_[batch.rows_[i].slot_];
        member.batchId = batch.count_ > 1 ? sequence : 0;
        member.batchLeader = i == 0;
        member.batchHead = batch.rows_[0].slot_;
        member.batchNext = i + 1 < batch.count_ ? batch.rows_[i + 1].slot_ : UINT32_MAX;
    }
    for (std::size_t index = 0; index < victimCount; ++index) {
        auto& victim = *supersedeRows_[index];
        FinalizeRow(victim, VariableOutcomeKind::Superseded, {sequence}, 0);
        const auto slot = static_cast<std::uint32_t>(&victim - history_.data());
        const bool recycled = RecycleHistoryRow(slot);
        assert(recycled);
        (void)recycled;
    }
    for (std::uint32_t i = 0; i < batch.count_; ++i) {
        CommitPreparedWriteImpl(&batch.rows_[i], sequence, nullptr, nullptr, false);
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
    auto slot = row.batchHead;
    while (slot != UINT32_MAX && count < rows.size()) {
        auto& member = history_[slot];
        if (member.used && member.state != RowState::Terminal &&
            member.batchId == row.batchId) {
            rows[count++] = &member;
        }
        slot = member.batchNext;
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
        member->frontierCaptured = leader.frontierCaptured;
        member->requiredCommandFrontier = leader.requiredCommandFrontier;
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
        }
    }
}

void VariableServiceV1::InsertScheduledGroup(WriteRow& row) noexcept {
    if (!row.batchLeader || row.scheduled) return;
    const auto slot = static_cast<std::uint32_t>(&row - history_.data());
    const auto end = scheduledGroups_.begin() + scheduledGroupCount_;
    const auto position = std::lower_bound(scheduledGroups_.begin(), end, slot,
        [&](std::uint32_t left, std::uint32_t right) { return Precedes(history_[left], history_[right]); });
    assert(scheduledGroupCount_ < scheduledGroups_.size());
    std::move_backward(position, end, end + 1);
    *position = slot;
    ++scheduledGroupCount_;
    row.scheduled = true;
}

void VariableServiceV1::EraseScheduledGroup(WriteRow& row) noexcept {
    if (!row.scheduled) return;
    const auto slot = static_cast<std::uint32_t>(&row - history_.data());
    const auto end = scheduledGroups_.begin() + scheduledGroupCount_;
    const auto position = std::find(scheduledGroups_.begin(), end, slot);
    assert(position != end);
    if (position != end) {
        std::move(position + 1, end, position);
        --scheduledGroupCount_;
    }
    row.scheduled = false;
    row.poolBlocked = false;
    if (ordinaryDebt_.slot == slot) ordinaryDebt_ = {};
}

void VariableServiceV1::FinalizeRow(WriteRow& row,
                                    VariableOutcomeKind outcome,
                                    LogicCommitTicket related,
                                    std::uint64_t actualFrame) noexcept {
    if (!row.used || row.state == RowState::Terminal) {
        return;
    }
    if (row.ordinaryEngine != nullptr && row.state != RowState::PageCommitted)
        ResetPreparedRow(*row.ordinaryEngine, row);
    EraseScheduledGroup(row);
    // Each used history slot finalizes once and is recycled only after delivery.
    // Queued outcomes plus unfinalized slots therefore fit the reserved storage.
    assert(outcomes_.size() < history_.size());
    assert(outcomes_.size() < outcomes_.capacity());
    ReleaseEmitterOverrideReservation(row);
    row.state = RowState::Terminal;
    row.finalOutcome = outcome;
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
    RememberFinalizedTicket(row.ticket.sequence, outcome, row.world, row.transport);
    if (outcome == VariableOutcomeKind::Applied) {
        ++appliedCount_;
    } else if (outcome == VariableOutcomeKind::Superseded) {
        ++supersededCount_;
    } else {
        ++terminationCount_;
    }
}

void VariableServiceV1::CommitAppliedBank(const WriteRow& row) {
    ObserveAppliedWriteForReadCuts(row);
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

template <typename Visitor>
void VariableServiceV1::ForEachTarget(Engine& engine, WriteRow& batchRow,
                                      Visitor&& visitor) noexcept {
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
                visitor(locator, engine.params.TargetRevision(handle, targetId), row);
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
                    visitor(locator,
                        engine.busTargets.TargetRevision(
                            busIndex, locator.objectGeneration, targetId), row);
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
                visitor(locator, revision, row);
            });
    }
}

bool VariableServiceV1::SharesPhysicalTargets(Engine& engine, WriteRow& prior,
                                              const WriteRow& row) noexcept {
    bool overlaps = false;
    const VariableSliceTable& slices = slices_;
    ForEachTarget(engine, prior, [&](const VariableTargetLocator& locator,
                                     std::uint32_t, const WriteRow&) {
        if (overlaps) return;
        for (std::uint32_t i = 0; i < slices.TargetCount(row.slice); ++i) {
            const auto* target = slices.Target(row.slice, i);
            if (target != nullptr && SamePhysicalTarget(target->locator, locator)) {
                overlaps = true;
                break;
            }
        }
    });
    return overlaps;
}

bool VariableServiceV1::ScopeAppliesToTarget(const Engine& engine, const WriteRow& row,
                                            const VariableTargetLocator& target) noexcept {
    if (row.scope.scope == VariableScopeV1::Global) return true;
    if (target.scope != VariableTargetScope::Voice &&
        target.scope != VariableTargetScope::VoiceEffect) return false;
    const auto emitter = engine.params.EmitterHandleOf(target.object);
    return row.scope.emitterSlot == static_cast<std::uint32_t>(emitter) &&
           row.scope.emitterGeneration == static_cast<std::uint32_t>(emitter >> 32);
}

bool VariableServiceV1::CaptureCandidateSources(Engine& engine, WriteRow& row,
                                                WriteRow* const* prefix,
                                                std::size_t count) noexcept {
    const VariableSliceTable& slices = slices_;
    const auto ownMembers = BatchRows(row);
    for (std::size_t index = 0; index < count; ++index) {
        bool contributes = false;
        ForEachTarget(engine, *prefix[index], [&](const VariableTargetLocator& locator,
                                                  std::uint32_t, const WriteRow& input) {
            if (contributes) return;
            for (std::uint32_t targetIndex = 0; targetIndex < slices.TargetCount(row.slice); ++targetIndex) {
                const auto* target = slices.Target(row.slice, targetIndex);
                if (target == nullptr || !SamePhysicalTarget(target->locator, locator)) continue;
                const auto overwrites = [&](const WriteRow* later) {
                    return later != nullptr && later->variableId == input.variableId &&
                           ScopeAppliesToTarget(engine, *later, target->locator);
                };
                bool overwritten = std::any_of(ownMembers.begin(), ownMembers.end(), overwrites);
                for (std::size_t later = index + 1; later < count && !overwritten; ++later) {
                    const auto members = BatchRows(*prefix[later]);
                    overwritten = std::any_of(members.begin(), members.end(), overwrites);
                }
                if (!overwritten) {
                    contributes = true;
                    break;
                }
            }
        });
        if (!contributes) continue;
        VariableSliceSource source{};
        const auto sourceFrame = slices.EffectiveFrame(prefix[index]->slice);
        if (!slices_.SetPreparedEffectiveFrame(row.slice,
                std::max(slices.EffectiveFrame(row.slice), sourceFrame)) ||
            !slices_.CaptureSource(prefix[index]->slice, &source) ||
            !slices_.AddSource(row.slice, source)) return false;
    }
    return true;
}

bool VariableServiceV1::GatherTargets(Engine& engine, WriteRow& batchRow,
                                      std::uint32_t* outCount) noexcept {
    if (outCount == nullptr) return false;
    *outCount = 0;
    bool overflow = false;
    ForEachTarget(engine, batchRow, [&](VariableTargetLocator locator,
                                        std::uint32_t revision, const WriteRow&) {
        for (std::uint32_t i = 0; i < *outCount; ++i) {
            if (SameTarget(censusScratch_[i].locator, locator)) return;
        }
        if (*outCount >= censusScratch_.size()) {
            overflow = true;
            return;
        }
        censusScratch_[*outCount] = {locator, revision};
        ++*outCount;
    });
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
                target->locator.targetId, row.publicationOwner, &resolved, &rampMs,
                &revision);
            break;
        case VariableTargetScope::Bus:
            owned = engine.busTargets.SnapshotForClaim(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.publicationOwner, &resolved, &rampMs, &revision);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            owned = engine.effectParamTargets.SnapshotForClaim(
                target->locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object, target->locator.objectGeneration,
                target->locator.effectSlot, target->locator.paramIndex,
                row.publicationOwner, &resolved, &revision, &effectType);
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
VariableServiceV1::CaptureCandidateSlice(Engine& engine, WriteRow& row, WorkSpan work) {
    row.state = RowState::TargetsClaimed;
    RunVariableEvaluators(engine);
    if (!RecensusMatches(engine, row)) {
        return CandidateCaptureResult::Retry;
    }
    // Bank-only writes still await their slice; they have no physical snapshot or sources to project.
    if (slices_.TargetCount(row.slice) == 0) return CandidateCaptureResult::Ready;
    VariableProjectionGuard projection(engine);
    if (!projection.Valid()) {
        return CandidateCaptureResult::Retry;
    }
    auto& prefix = prefixRows_;
    std::size_t prefixCount = 0;
    for (WriteRow* priorPtr : work) {
        WriteRow& prior = *priorPtr;
        const bool privatePrefix = prior.reserved && row.reserved &&
            prior.preparedIssue.SameReservation(row.preparedIssue) &&
            IsPreparedActionOrderActive(prior.preparedIssue);
        if ((!prior.used && !privatePrefix) || !prior.batchLeader || prior.world != row.world ||
            prior.transport != row.transport || !PublicationPrecedes(prior, row) ||
            (prior.state != RowState::SlicePrepared && prior.state != RowState::PageCommitted)) continue;
        if (slices_.State(prior.slice) == VariableSliceState::Invalidated) continue;
        if (prefixCount == prefix.size()) return CandidateCaptureResult::Retry;
        prefix[prefixCount++] = &prior;
    }
    std::sort(prefix.begin(), prefix.begin() + prefixCount,
        [](const WriteRow* a, const WriteRow* b) { return PublicationPrecedes(*a, *b); });
    if (!CaptureCandidateSources(engine, row, prefix.data(), prefixCount)) {
        return CandidateCaptureResult::Retry;
    }
    SetSliceRampSuppressed(engine, row, true);
    for (std::size_t i = 0; i < prefixCount; ++i) {
        SetSliceRampSuppressed(engine, *prefix[i], true);
        for (const WriteRow* member : BatchRows(*prefix[i])) {
            if (member != nullptr) ProjectCandidate(engine, *member);
        }
        RunVariableEvaluators(engine);
    }
    SetSliceRampSuppressed(engine, row, false);
    for (const WriteRow* member : BatchRows(row)) {
        if (member != nullptr) ProjectCandidate(engine, *member);
    }
    RunVariableEvaluators(engine);
    bool valid = true;
    std::uint32_t rejectedTargets = 0;
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
        bool poisoned = false;
        switch (target.locator.scope) {
        case VariableTargetScope::Voice: {
            float pan2d = 0.0f;
            valid = engine.params.CaptureSnapshotForClaim(
                static_cast<VnVoiceHandle>(target.locator.object),
                target.locator.targetId, row.publicationOwner, &resolved, &rampMs,
                &revision, &pan2d);
            if (valid && target.locator.targetId == kTargetVoiceVolume &&
                !TryVolumeGainFromDb(resolved)) {
                poisoned = true;
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
                row.publicationOwner, &resolved, &rampMs, &revision);
            if (valid &&
                (target.locator.targetId == kBusTargetBusVolume ||
                 target.locator.targetId == kBusTargetBusOutputVolume) &&
                !TryVolumeGainFromDb(resolved)) {
                poisoned = true;
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
                target.locator.paramIndex, row.publicationOwner, &resolved, &revision,
                &effectType);
            valid = valid &&
                    MakeVariableEffectParamCommand(
                        engine, scope, target.locator.object,
                        target.locator.effectSlot, target.locator.paramIndex,
                        resolved, effectType, &target.command, 0, &target.toneBand);
            if (valid && scope == EffectParamTargetStore::Scope::Voice) {
                const auto voiceSlot = static_cast<std::uint32_t>(target.locator.object);
                if (voiceSlot >= engine.voiceEffectInstanceRefs.size() ||
                    target.locator.effectSlot >= engine.voiceEffectInstanceRefs[voiceSlot].size()) {
                    valid = false;
                } else {
                    const auto& effect = engine.voiceEffectInstanceRefs[voiceSlot][target.locator.effectSlot];
                    target.nativeEffect = {effect.type, effect.poolIndex, effect.generation};
                }
            }
            break;
        }
        }
        if (!valid || revision != target.targetRevision ||
            effectType != target.locator.effectType) {
            valid = false;
            break;
        }
        target.resolvedValue = resolved;
        target.rampMs = rampMs;
        *existing = target;
        if (poisoned) ++rejectedTargets;
    }
    projection.Restore();
    if (valid && rejectedTargets != 0) {
        // The census contains unique physical targets; retries must not count drops.
        engine.paramDropCount += rejectedTargets;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& target = *slices_.Target(row.slice, i);
            if (TryVolumeGainFromDb(target.resolvedValue)) continue;
            if (target.locator.scope == VariableTargetScope::Voice &&
                target.locator.targetId == kTargetVoiceVolume) {
                const auto handle = static_cast<VnVoiceHandle>(target.locator.object);
                if (!TryVolumeGainFromDb(engine.params.ResolvedValue(handle, target.locator.targetId)))
                    engine.params.DropClaimedBaseline(handle, target.locator.targetId, row.publicationOwner);
            } else if (target.locator.scope == VariableTargetScope::Bus &&
                (target.locator.targetId == kBusTargetBusVolume ||
                 target.locator.targetId == kBusTargetBusOutputVolume)) {
                const auto bus = static_cast<std::uint32_t>(target.locator.object);
                if (!TryVolumeGainFromDb(engine.busTargets.Reduce(bus, target.locator.targetId)))
                    engine.busTargets.DropClaimedBaseline(bus, target.locator.objectGeneration,
                        target.locator.targetId, row.publicationOwner);
            }
        }
        RejectPreparedRow(engine, row);
        return CandidateCaptureResult::Rejected;
    }
    if (valid) {
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* target = slices_.Target(row.slice, i);
            if (target->locator.scope == VariableTargetScope::Voice) {
                target->carriedRampMs = engine.params.TakePublicationRamp(
                    static_cast<VnVoiceHandle>(target->locator.object),
                    target->locator.targetId, row.publicationOwner);
            } else if (target->locator.scope == VariableTargetScope::Bus) {
                target->carriedRampMs = engine.busTargets.TakePublicationRamp(
                    static_cast<std::uint32_t>(target->locator.object),
                    target->locator.objectGeneration, target->locator.targetId,
                    row.publicationOwner);
            }
        }
    }
    return valid ? CandidateCaptureResult::Ready
                 : CandidateCaptureResult::Retry;
}

void VariableServiceV1::RejectPreparedRow(Engine& engine,
                                          WriteRow& row) noexcept {
    auto& held = preparedWritePlacements_[static_cast<std::size_t>(&row - history_.data())];
    if (held.IsHeld()) (void)timeline_.CancelHeldCurrentSlice(&held);
    row.placementHeld = false;
    const auto members = BatchRows(row);
    if (row.slice.Valid()) {
        assert(slices_.SourceObserverCount(row.slice) == (row.historySlicePin ? 1u : 0u));
        ReleaseSliceClaims(engine, row, ClaimReleaseReason::RejectCandidate);
        if (row.historySlicePin) {
            VariableSliceSource source{};
            if (slices_.CaptureSource(row.slice, &source)) (void)slices_.ReleaseSource(source);
            row.historySlicePin = false;
        }
        (void)slices_.AbandonPrepared(row.slice);
    }
    for (auto* member : members) {
        if (member != nullptr) member->slice = {};
    }
    FinalizeBatch(row, VariableOutcomeKind::TargetRejected, {}, 0);
}

void VariableServiceV1::ResetPreparedRow(Engine& engine,
                                         WriteRow& row) noexcept {
    auto& held = preparedWritePlacements_[static_cast<std::size_t>(&row - history_.data())];
    if (held.IsHeld()) (void)timeline_.CancelHeldCurrentSlice(&held);
    row.placementHeld = false;
    if (row.slice.Valid()) {
        assert(slices_.SourceObserverCount(row.slice) == (row.historySlicePin ? 1u : 0u));
        ReleaseSliceClaims(engine, row);
        if (row.historySlicePin) {
            VariableSliceSource source{};
            if (slices_.CaptureSource(row.slice, &source)) (void)slices_.ReleaseSource(source);
            row.historySlicePin = false;
        }
        (void)slices_.AbandonPrepared(row.slice);
    }
    row.slice = {};
    row.state = RowState::Pending;
    row.frontierCaptured = false;
    row.preparedEngine = nullptr;
    row.ordinaryEngine = nullptr;
    row.preparedSourcesSealed = false;
    row.poolBlocked = false;
    SyncBatchState(row);
}

bool VariableServiceV1::PrepareCurrentSlice(Engine& engine, WriteRow& row) {
    RefreshOrdinaryDebt(engine);
    std::uint32_t count = 0;
    if (!GatherTargets(engine, row, &count) ||
        count > slices_.MaxTargetsPerSlice() || count > slices_.TargetPoolCapacity()) {
        if (row.used) FinalizeBatch(row, VariableOutcomeKind::TargetRejected, {}, 0);
        return false;
    }
    if (YieldToOrdinaryDebt(engine, row, count)) return false;
    const auto availability = slices_.ReservationAvailability(count);
    if (availability != VariableSliceTable::ReservationStatus::Ready) {
        if (row.used && row.requestedFrame < timeline_.WindowEndFrame() &&
            CensusCanClaim(engine, row, count)) {
            // Freeze the first allocator wait; later commands cannot move this eligibility frontier.
            if (!row.poolBlocked && !row.frontierCaptured)
                row.requiredCommandFrontier = engine.commands.PublishedHead();
            row.poolBlocked = true;
            row.poolBlockReason = availability;
        }
        return false;
    }
    if (!slices_.Reserve(row.world, row.transport, row.ticket.sequence,
                         row.requestedFrame, row.bankOrdinal, count,
                         &row.slice)) {
        return false;
    }
    row.poolBlocked = false;
    if (ResolveHistory(ordinaryDebt_) == &row) ordinaryDebt_ = {};
    if (row.reserved) row.preparedEngine = &engine;
    else row.ordinaryEngine = &engine;
    row.state = RowState::TargetsClaimed;

    const auto releaseClaim = [&](const GatheredTarget& target) {
        switch (target.locator.scope) {
        case VariableTargetScope::Voice:
            engine.params.ReleaseClaim(
                static_cast<VnVoiceHandle>(target.locator.object),
                target.locator.targetId, row.publicationOwner);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.ReleaseClaim(
                static_cast<std::uint32_t>(target.locator.object),
                target.locator.objectGeneration, target.locator.targetId,
                row.publicationOwner);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.ReleaseClaim(
                target.locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target.locator.object, target.locator.objectGeneration,
                target.locator.effectSlot, target.locator.paramIndex,
                row.publicationOwner);
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
                target.locator.targetId, row.publicationOwner, target.revision);
            break;
        case VariableTargetScope::Bus:
            ok = engine.busTargets.TryClaim(
                static_cast<std::uint32_t>(target.locator.object),
                target.locator.objectGeneration, target.locator.targetId,
                row.publicationOwner, target.revision);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            ok = engine.effectParamTargets.TryClaim(
                target.locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target.locator.object, target.locator.objectGeneration,
                target.locator.effectSlot, target.locator.paramIndex,
                row.publicationOwner, target.revision);
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
    if (!row.frontierCaptured || count != 0)
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
            if (reason != ClaimReleaseReason::RejectCandidate) {
                engine.params.RestorePublicationRamp(
                    static_cast<VnVoiceHandle>(target->locator.object),
                    target->locator.targetId, row.publicationOwner, target->carriedRampMs);
            }
            engine.params.ReleaseClaim(
                static_cast<VnVoiceHandle>(target->locator.object),
                target->locator.targetId, row.publicationOwner, reason);
            break;
        case VariableTargetScope::Bus:
            if (reason != ClaimReleaseReason::RejectCandidate) {
                engine.busTargets.RestorePublicationRamp(
                    static_cast<std::uint32_t>(target->locator.object),
                    target->locator.objectGeneration, target->locator.targetId,
                    row.publicationOwner, target->carriedRampMs);
            }
            engine.busTargets.ReleaseClaim(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.publicationOwner, reason);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.ReleaseClaim(
                target->locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object, target->locator.objectGeneration,
                target->locator.effectSlot, target->locator.paramIndex,
                row.publicationOwner);
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
                target->locator.targetId, row.publicationOwner, target->resolvedValue,
                target->rampMs, target->locator.targetId == kTargetVoicePan
                    ? target->command.payload.setVoicePan.pan2d : 0.0f);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.MarkSliceCommitted(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.publicationOwner, target->resolvedValue, target->rampMs);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            engine.effectParamTargets.MarkSliceCommitted(
                target->locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice
                    : EffectParamTargetStore::Scope::Bus,
                target->locator.object, target->locator.objectGeneration,
                target->locator.effectSlot, target->locator.paramIndex,
                row.publicationOwner,
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
                target->locator.targetId, row.publicationOwner, suppressed);
            break;
        case VariableTargetScope::Bus:
            engine.busTargets.SetClaimRampSuppressed(
                static_cast<std::uint32_t>(target->locator.object),
                target->locator.objectGeneration, target->locator.targetId,
                row.publicationOwner, suppressed);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            break;
        }
    }
}

std::uint64_t VariableServiceV1::ExpectedFrame(const WriteRow& row,
    std::uint64_t nextOpenFrame) const noexcept {
    return row.state == RowState::PageCommitted || row.placementHeld
        ? row.committedFrame : std::max(row.requestedFrame, nextOpenFrame);
}

bool VariableServiceV1::ExpectedPrecedes(const WriteRow& prior, const WriteRow& row,
    std::uint64_t nextOpenFrame) const noexcept {
    const auto left = ExpectedFrame(prior, nextOpenFrame);
    const auto right = ExpectedFrame(row, nextOpenFrame);
    if (left != right) return left < right;
    if (prior.ticket.sequence != row.ticket.sequence) return prior.ticket.sequence < row.ticket.sequence;
    if (prior.executionOrdinal != row.executionOrdinal) return prior.executionOrdinal < row.executionOrdinal;
    return prior.bankOrdinal < row.bankOrdinal;
}

bool VariableServiceV1::GroupsDepend(Engine& engine, WriteRow& prior, WriteRow& row) noexcept {
    if (&prior == &row || prior.world != row.world || prior.transport != row.transport) return false;
    const auto left = BatchRows(prior);
    const auto right = BatchRows(row);
    for (const auto* a : left) for (const auto* b : right)
        if (a != nullptr && b != nullptr && SameServiceDependency(*a, *b)) return true;
    if (prior.scope.scope == VariableScopeV1::Emitter && row.scope.scope == VariableScopeV1::Emitter &&
        !ScopeEqual(prior.scope, row.scope)) return false;
    if (row.slice.Valid()) return SharesPhysicalTargets(engine, prior, row);
    bool overlaps = false;
    ForEachTarget(engine, prior, [&](const VariableTargetLocator& a, std::uint32_t, const WriteRow&) {
        if (overlaps) return;
        ForEachTarget(engine, row, [&](const VariableTargetLocator& b, std::uint32_t, const WriteRow&) {
            overlaps = overlaps || SamePhysicalTarget(a, b);
        });
    });
    return overlaps;
}

bool VariableServiceV1::ReadCutNeedsGroup(const ReadCutRow& cut, WriteRow& row) noexcept {
    for (std::uint32_t i = 0; i < cut.sourceCount; ++i)
        if (ResolveHistory(cut.sources[i]) == &row) return true;
    if (!cut.pendingPlacement) return false;
    const VariableScopeKey scope{VariableScopeV1::Emitter,
        static_cast<std::uint32_t>(cut.emitter), static_cast<std::uint32_t>(cut.emitter >> 32)};
    for (const auto* member : BatchRows(row)) {
        if (member == nullptr || member->requestedFrame > cut.executeFrame ||
            member->ticket.sequence > cut.sequence ||
            (member->ticket.sequence == cut.sequence && member->executionOrdinal >= cut.executionOrdinalExclusive) ||
            (member->scope.scope != VariableScopeV1::Global &&
                (cut.emitter == 0 || !ScopeEqual(member->scope, scope))) ||
            !cut.candidatePins.test(static_cast<std::size_t>(member - history_.data()))) continue;
        for (std::uint32_t i = 0; i < cut.valueCount; ++i)
            if (cut.values[i].read && cut.values[i].variableId == member->variableId) return true;
    }
    return false;
}

VariableServiceV1::WorkSpan VariableServiceV1::BuildWorkSet(Engine& engine,
    const ReadCutRow* cut, const ActionOrderIdentity* privateIssue) {
    const auto nextOpenFrame = EarliestReadCutFrame(engine);
    const auto world = cut != nullptr ? cut->world : worldGeneration_;
    const auto transport = cut != nullptr ? cut->transport : transport_.Current();
    std::fill(serviceCandidates_.begin(), serviceCandidates_.end(), 0);
    std::size_t count = 0;
    for (std::uint32_t i = 0; i < scheduledGroupCount_; ++i) {
        auto& row = history_[scheduledGroups_[i]];
        const bool privateRow = row.reserved && privateIssue != nullptr &&
            row.preparedIssue.SameReservation(*privateIssue);
        if ((!row.used && !privateRow) || row.world != world || row.transport != transport ||
            row.state == RowState::Terminal) continue;
        actionReadCutRows_[count++] = &row;
        serviceCandidates_[static_cast<std::size_t>(&row - history_.data())] =
            cut != nullptr ? ReadCutNeedsGroup(*cut, row) :
                row.used && row.requestedFrame < timeline_.WindowEndFrame();
    }
    workCandidateCount_ = count;
    return CloseWorkSet(engine, nextOpenFrame);
}

VariableServiceV1::WorkSpan VariableServiceV1::CloseWorkSet(Engine& engine,
    std::uint64_t nextOpenFrame) {
    const auto count = workCandidateCount_;
    std::sort(actionReadCutRows_.begin(), actionReadCutRows_.begin() + count,
        [&](const WriteRow* a, const WriteRow* b) { return ExpectedPrecedes(*a, *b, nextOpenFrame); });
    // Ordered reverse closure promotes whole groups, including unobserved physical predecessors.
    for (std::size_t i = count; i-- > 0;) {
        auto& prior = *actionReadCutRows_[i];
        auto& selected = serviceCandidates_[static_cast<std::size_t>(&prior - history_.data())];
        if (selected != 0) continue;
        for (std::size_t j = i + 1; j < count; ++j) {
            auto& later = *actionReadCutRows_[j];
            if (serviceCandidates_[static_cast<std::size_t>(&later - history_.data())] != 0 &&
                GroupsDepend(engine, prior, later)) { selected = 1; break; }
        }
    }
    std::size_t selectedCount = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto* row = actionReadCutRows_[i];
        if (serviceCandidates_[static_cast<std::size_t>(row - history_.data())] != 0)
            readCutRows_[selectedCount++] = row;
    }
    return {readCutRows_.data(), selectedCount};
}

bool VariableServiceV1::CensusCanClaim(const Engine& engine, const WriteRow& row,
    std::uint32_t count) const noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& target = censusScratch_[i];
        const auto& locator = target.locator;
        bool available = false;
        switch (locator.scope) {
        case VariableTargetScope::Voice:
            available = engine.params.CanClaim(static_cast<VnVoiceHandle>(locator.object),
                locator.targetId, row.publicationOwner, target.revision);
            break;
        case VariableTargetScope::Bus:
            available = engine.busTargets.CanClaim(static_cast<std::uint32_t>(locator.object),
                locator.objectGeneration, locator.targetId, row.publicationOwner, target.revision);
            break;
        case VariableTargetScope::VoiceEffect:
        case VariableTargetScope::BusEffect:
            available = engine.effectParamTargets.CanClaim(
                locator.scope == VariableTargetScope::VoiceEffect
                    ? EffectParamTargetStore::Scope::Voice : EffectParamTargetStore::Scope::Bus,
                locator.object, locator.objectGeneration, locator.effectSlot, locator.paramIndex,
                row.publicationOwner, target.revision);
            break;
        }
        if (!available) return false;
    }
    return true;
}

void VariableServiceV1::RefreshOrdinaryDebt(Engine& engine) noexcept {
    ordinaryDebt_ = {};
    const auto nextOpenFrame = EarliestReadCutFrame(engine);
    WriteRow* earliest = nullptr;
    for (std::uint32_t i = 0; i < scheduledGroupCount_; ++i) {
        auto& row = history_[scheduledGroups_[i]];
        if (!row.poolBlocked || !row.used || row.state != RowState::Pending ||
            row.world != worldGeneration_ || row.transport != transport_.Current() ||
            engine.commands.SettledFrontier() < row.requiredCommandFrontier ||
            row.requestedFrame >= timeline_.WindowEndFrame() ||
            (earliest != nullptr && !ExpectedPrecedes(row, *earliest, nextOpenFrame))) continue;
        bool blocked = false;
        for (std::uint32_t j = 0; j < scheduledGroupCount_; ++j) {
            auto& prior = history_[scheduledGroups_[j]];
            if (!prior.used || prior.state == RowState::Terminal || prior.state == RowState::PageCommitted ||
                !ExpectedPrecedes(prior, row, nextOpenFrame)) continue;
            if (GroupsDepend(engine, prior, row)) { blocked = true; break; }
        }
        if (blocked) continue;
        ScheduledCurrentSliceInput input{};
        input.executeFrame = row.requestedFrame;
        input.logicCommitSequence = row.ticket.sequence;
        input.executionOrdinal = row.executionOrdinal;
        if (timeline_.PreflightHeldCurrentSlice(input,
                PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, nextOpenFrame).status !=
            ReserveStatus::Committed) continue;
        std::uint32_t count = 0;
        if (!GatherTargets(engine, row, &count) || count > slices_.MaxTargetsPerSlice() ||
            count > slices_.TargetPoolCapacity() || !CensusCanClaim(engine, row, count)) continue;
        // A recovered allocator remains owed to this identity until ordinary service gets its turn.
        earliest = &row;
    }
    if (earliest != nullptr) {
        const auto slot = static_cast<std::uint32_t>(earliest - history_.data());
        ordinaryDebt_ = {slot, historyGenerations_[slot], earliest->ticket.sequence};
    }
}

bool VariableServiceV1::YieldToOrdinaryDebt(Engine& engine, const WriteRow& row,
    std::uint32_t targetCount) noexcept {
    const auto* debt = ResolveHistory(ordinaryDebt_);
    if (debt == nullptr || debt == &row || ExpectedPrecedes(row, *debt, EarliestReadCutFrame(engine)))
        return false;
    // Bank-only work cannot consume the target span, and must leave one proof and payload for the debt.
    if (targetCount == 0 && debt->poolBlockReason == VariableSliceTable::ReservationStatus::TargetSpan &&
        slices_.FreeRowCount() > 1 && slices_.FreePayloadCount() > 1) return false;
    if (row.reserved) {
        for (std::uint32_t i = 0; i < scheduledGroupCount_; ++i) {
            const auto& prior = history_[scheduledGroups_[i]];
            if (prior.reserved && prior.slice.Valid() &&
                prior.preparedIssue.SameReservation(row.preparedIssue) &&
                (debt->poolBlockReason != VariableSliceTable::ReservationStatus::TargetSpan ||
                    slices_.TargetCount(prior.slice) != 0)) return false;
        }
    }
    return true;
}

void VariableServiceV1::ReserveOrdinaryWork(Engine& engine) {
    RefreshOrdinaryDebt(engine);
    auto work = BuildWorkSet(engine, nullptr, nullptr);
    PrepareClaimedSlicesImpl(engine, work, false, false, nullptr, true);
}

void VariableServiceV1::ReleaseOrdinaryReservations(Engine& engine) noexcept {
    for (std::uint32_t i = 0; i < scheduledGroupCount_; ++i) {
        auto& row = history_[scheduledGroups_[i]];
        if (row.used && row.placementHeld && row.state != RowState::PageCommitted) {
            const bool bankOnlyFrontier = row.frontierCaptured && slices_.TargetCount(row.slice) == 0;
            const auto frontier = row.requiredCommandFrontier;
            ResetPreparedRow(engine, row);
            if (bankOnlyFrontier) {
                row.requiredCommandFrontier = frontier;
                row.frontierCaptured = true;
                SyncBatchState(row);
            }
        }
    }
}

bool VariableServiceV1::HasUnpreparedPredecessor(Engine& engine, WriteRow& row,
    WorkSpan work, std::uint64_t nextOpenFrame) noexcept {
    for (auto* prior : work) {
        if (prior == &row || prior->state == RowState::Terminal || prior->state == RowState::PageCommitted ||
            (prior->reserved && prior->state == RowState::SlicePrepared && prior->placementHeld) ||
            !ExpectedPrecedes(*prior, row, nextOpenFrame)) continue;
        if (GroupsDepend(engine, *prior, row)) return true;
    }
    return false;
}

void VariableServiceV1::BeginService(Engine& engine,
                                     std::uint64_t blocksRendered,
                                     std::uint32_t blockFrames) {
    serviceAudioFrame_ =
        blockFrames == 0 || blocksRendered > UINT64_MAX / blockFrames
            ? (blockFrames == 0 ? blocksRendered : UINT64_MAX)
            : blocksRendered * blockFrames;
    ReleaseOrdinaryReservations(engine);
    ReserveOrdinaryWork(engine);
}

std::uint64_t VariableServiceV1::EarliestReadCutFrame(const Engine& engine) const noexcept {
    const auto quantum = timeline_.QuantumFrames();
    const auto blocks = engine.mixer.BlocksRendered();
    if (quantum == 0) return UINT64_MAX;
    const auto audioFrame = std::max(serviceAudioFrame_,
        blocks > UINT64_MAX / quantum ? UINT64_MAX : blocks * quantum);
    const auto frame = std::max(
        {timeline_.PublishCutoffFrame(),
         timeline_.WindowStartFrame() +
             static_cast<std::uint64_t>(timeline_.PublishLeadQuanta()) * timeline_.QuantumFrames(),
         (timeline_.ConsumerFrontier().load(std::memory_order_acquire) + 1u) * timeline_.QuantumFrames(),
         audioFrame > UINT64_MAX - quantum ? UINT64_MAX : audioFrame + quantum});
    return frame > UINT64_MAX - (quantum - 1u) ? UINT64_MAX :
        ((frame + quantum - 1u) / quantum) * quantum;
}

bool VariableServiceV1::PrepareReadCutSources(Engine& engine, VariableReadCutRef ref) {
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing) return false;
    auto work = BuildWorkSet(engine, cut, nullptr);
    PrepareClaimedSlicesImpl(engine, work, true, false);
    return true;
}

bool VariableServiceV1::PrepareActionReadCutSources(Engine& engine, VariableReadCutRef ref,
    const LogicCommitCoordinator::PreparedIssue& issue) {
    ActionOrderIdentity identity{};
    return CaptureActionOrder(issue, &identity) && PrepareActionReadCutSourcesImpl(engine, ref, identity);
}

bool VariableServiceV1::PrepareActionReadCutSources(Engine& engine, VariableReadCutRef ref,
    const ActionFiringTable::PreparedChildIdentity& child) {
    ActionOrderIdentity identity{};
    return CaptureActionOrder(engine, child, &identity) && PrepareActionReadCutSourcesImpl(engine, ref, identity);
}

bool VariableServiceV1::PrepareActionReadCutSourcesImpl(Engine& engine, VariableReadCutRef ref,
    const ActionOrderIdentity& identity) {
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
        &engine.variables != this || &engine.logicCommits != &commits_ || !IsPreparedActionOrderActive(identity) ||
        !cut->preparedIssue.SameReservation(identity)) return false;
    auto work = BuildWorkSet(engine, cut, &identity);
    PrepareClaimedSlicesImpl(engine, work, true, false, &identity);
    for (std::uint32_t index = 0; index < cut->sourceCount; ++index) {
        const auto* row = ResolveHistory(cut->sources[index]);
        if (row == nullptr || !row->slice.Valid()) return false;
        const bool privateReady = row->reserved && row->preparedIssue.SameReservation(identity) &&
            row->preparedEngine == &engine && row->placementHeld && row->state == RowState::SlicePrepared;
        const bool committedReady = row->used && (row->state == RowState::PageCommitted ||
            (row->state == RowState::Terminal && row->finalOutcome == VariableOutcomeKind::Applied));
        if (!privateReady && !committedReady) return false;
    }
    return true;
}

void VariableServiceV1::PrepareClaimedSlices(Engine& engine) {
    auto work = BuildWorkSet(engine, nullptr, nullptr);
    PrepareClaimedSlicesImpl(engine, work, false, true);
    ReleaseOrdinaryReservations(engine);
}

void VariableServiceV1::PrepareClaimedSlicesImpl(Engine& engine, WorkSpan work,
    bool allowUnsettledFrontier, bool publishPages,
    const ActionOrderIdentity* privateIssue, bool reserveOnly) {
    const std::uint64_t nextOpenFrame = EarliestReadCutFrame(engine);
    // Each selected identity is attempted once; actual placement can only grow the closure.
    for (;;) {
        for (auto* candidate : work) {
            auto& row = *candidate;
            auto& attempted = serviceCandidates_[static_cast<std::size_t>(&row - history_.data())];
            if (attempted == 2) continue;
            attempted = 2;
            const bool privateReady = row.reserved && privateIssue != nullptr &&
                row.preparedIssue.SameReservation(*privateIssue) && row.placementHeld &&
                row.state == RowState::SlicePrepared;
            if (row.state == RowState::Terminal || row.state == RowState::PageCommitted || privateReady) continue;
            auto& held = preparedWritePlacements_[static_cast<std::size_t>(&row - history_.data())];
            if (row.used && row.placementHeld && row.committedFrame < nextOpenFrame) {
                // Moving the calendar does not invalidate these claims or their captured FIFO frontier.
                const bool cancelled = timeline_.CancelHeldCurrentSlice(&held);
                assert(cancelled);
                if (!cancelled) return;
                row.placementHeld = false;
            }
            if (row.state == RowState::Pending && !PrepareCurrentSlice(engine, row)) continue;
            if (!row.slice.Valid() || (row.state != RowState::WaitingFrontier &&
                row.state != RowState::SlicePrepared)) continue;
            if (!allowUnsettledFrontier && engine.commands.SettledFrontier() < row.requiredCommandFrontier) continue;
            if (!RecensusMatches(engine, row)) { ResetPreparedRow(engine, row); continue; }
            (void)slices_.ClearPreparedSources(row.slice);
            if (!row.placementHeld) {
                ScheduledCurrentSliceInput input{};
                input.executeFrame = row.requestedFrame;
                input.logicCommitSequence = row.ticket.sequence;
                input.executionOrdinal = row.executionOrdinal;
                input.slice = {row.slice.slot, row.slice.generation};
                const auto reserved = timeline_.ReserveHeldCurrentSlice(input,
                    PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, nextOpenFrame, &held);
                if (reserved.status != ReserveStatus::Committed) {
                    ResetPreparedRow(engine, row);
                    continue;
                }
                row.committedFrame = reserved.firstFrame;
                row.placementHeld = true;
            }
            if (!slices_.SetPreparedEffectiveFrame(row.slice, row.committedFrame)) {
                ResetPreparedRow(engine, row);
                continue;
            }
            SyncBatchState(row);
        }
        const auto previousCount = work.size();
        work = CloseWorkSet(engine, nextOpenFrame);
        if (work.size() == previousCount) break;
    }
    RefreshOrdinaryDebt(engine);
    if (reserveOnly) return;
    auto& rows = preparationRows_;
    std::size_t rowCount = 0;
    for (auto* candidate : work) {
        auto& row = *candidate;
        const bool privateRow = row.reserved && privateIssue != nullptr &&
            row.preparedIssue.SameReservation(*privateIssue);
        if ((!row.used && !privateRow) || !row.placementHeld || !row.slice.Valid() ||
            (privateRow && row.state == RowState::SlicePrepared)) continue;
        if (row.state != RowState::WaitingFrontier && row.state != RowState::SlicePrepared) continue;
        assert(rowCount < rows.size());
        rows[rowCount++] = &row;
    }
    std::sort(rows.begin(), rows.begin() + rowCount,
        [](const WriteRow* a, const WriteRow* b) { return PublicationPrecedes(*a, *b); });
    std::uint64_t publishThrough = 0;
    for (std::size_t index = 0; index < rowCount; ++index) {
        WriteRow& row = *rows[index];
        auto& held = preparedWritePlacements_[static_cast<std::size_t>(&row - history_.data())];
        const auto cancelPlacement = [&] {
            if (held.IsHeld()) (void)timeline_.CancelHeldCurrentSlice(&held);
            row.placementHeld = false;
        };
        {
            if (HasUnpreparedPredecessor(engine, row, work, nextOpenFrame)) {
                cancelPlacement();
                ResetPreparedRow(engine, row);
                continue;
            }
            if (!RecensusMatches(engine, row)) {
                cancelPlacement();
                ResetPreparedRow(engine, row);
                continue;
            }
            const CandidateCaptureResult captured =
                CaptureCandidateSlice(engine, row, work);
            if (captured == CandidateCaptureResult::Retry) {
                cancelPlacement();
                ResetPreparedRow(engine, row);
                continue;
            }
            if (captured == CandidateCaptureResult::Rejected) {
                cancelPlacement();
                continue;
            }
            row.state = RowState::SlicePrepared;
            SyncBatchState(row);
        }
        if (!slices_.SetPreparedCommandFrontier(row.slice, row.requiredCommandFrontier) ||
            !PinHistorySlice(row) || (row.used && !slices_.Publish(row.slice))) {
            cancelPlacement();
            ResetPreparedRow(engine, row);
            continue;
        }
        if (row.reserved) {
            row.preparedEngine = &engine;
            continue;
        }
        const bool released = timeline_.ReleaseHeldCurrentSlice(&held);
        assert(released);
        (void)released;
        row.placementHeld = false;
        row.ordinaryEngine = nullptr;
        row.state = RowState::PageCommitted;
        SyncBatchState(row);
        publishThrough = std::max(publishThrough, row.committedFrame + timeline_.QuantumFrames());
    }
    pendingPlaybackPublicationThrough_ = std::max(pendingPlaybackPublicationThrough_, publishThrough);
    if (publishPages) PublishPreparedPlaybackPages();
}

void VariableServiceV1::PublishPreparedPlaybackPages(std::uint64_t requestedPublishThrough) noexcept {
    pendingPlaybackPublicationThrough_ = std::max(pendingPlaybackPublicationThrough_, requestedPublishThrough);
    if (pendingPlaybackPublicationThrough_ != 0) {
        auto publishThrough = pendingPlaybackPublicationThrough_;
        const auto quantum = timeline_.QuantumFrames();
        // A shared source page must not outrun its fixed voice installation.
        for (std::uint32_t index = 0; quantum != 0 && index < timelineReadUseCapacity_; ++index) {
            const auto& use = timelineReadUses_[index];
            if (use.state.load(std::memory_order_acquire) != TimelineReadUseState::Published ||
                use.initializer.load(std::memory_order_acquire) != TimelineInitializerState::None) continue;
            const auto* cut = ResolveReadCut(use.cut);
            TimelineArmObservation arm{};
            if (cut == nullptr || !use.lifetime.Owner()->Observe(use.lifetime.Token(), &arm)) continue;
            if (arm.state == ArmState::PendingApply || arm.state == ArmState::Armed)
                publishThrough = std::min(publishThrough, (cut->executeFrame / quantum) * quantum);
        }
        timeline_.Publish(publishThrough);
        if (timeline_.PublishCutoffFrame() >= pendingPlaybackPublicationThrough_)
            pendingPlaybackPublicationThrough_ = 0;
    }
}

void VariableServiceV1::EndService(Engine& engine) {
    const std::uint64_t blocksRendered = engine.mixer.BlocksRendered();
    auto& rows = settledRows_;
    std::size_t rowCount = 0;
    for (WriteRow& row : history_) {
        if (!row.used || !row.batchLeader) {
            continue;
        }
        if (row.state == RowState::Terminal) continue;
        assert(rowCount < rows.size());
        rows[rowCount++] = &row;
    }
    std::sort(rows.begin(), rows.begin() + rowCount,
        [](const WriteRow* a, const WriteRow* b) { return PublicationPrecedes(*a, *b); });
    for (std::size_t index = 0; index < rowCount; ++index) {
        WriteRow& row = *rows[index];
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
            const auto reasons = slices_.InvalidationReasons(row.slice);
            const auto outcome = (reasons & (kVariableSliceSourceRejected | kVariableSliceTargetRejected)) != 0
                ? VariableOutcomeKind::TargetRejected : VariableOutcomeKind::EmitterDestroyed;
            FinalizeBatch(row, outcome, {},
                        appliedBlock * engine.config.blockFrames);
            const bool acknowledged = slices_.Acknowledge(row.slice);
            assert(acknowledged);
            (void)acknowledged;
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
        const bool acknowledged = slices_.Acknowledge(row.slice);
        assert(acknowledged);
        (void)acknowledged;
    }
    observedBlock_ = blocksRendered;
    ReclaimHistoryRows();
    slices_.ReclaimSettled(blocksRendered);
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
        !row.slice.Valid() && row.state == RowState::Pending;
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
            if (unclaimedPending) {
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
        // Released native calendars must drain in the old epoch before reset.
        // Otherwise a source-free holder could lose its only consumption proof.
        if (HasNativeReadCutsForTransport(transportDrainEpoch_)) return false;
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
        if (row.used && row.state != RowState::Terminal && row.ticket.sequence == ticket) {
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

VariableServiceV1::ReadCutRow* VariableServiceV1::ResolveReadCut(
    VariableReadCutRef cut) noexcept {
    if (!cut.Valid() || cut.slot >= readCutCapacity_) return nullptr;
    auto& row = readCuts_[cut.slot];
    if (row.generation.load(std::memory_order_acquire) != cut.generation) return nullptr;
    const auto state = row.state.load(std::memory_order_acquire);
    return state == ReadCutState::Capturing || state == ReadCutState::Published ? &row : nullptr;
}

const VariableServiceV1::ReadCutRow* VariableServiceV1::ResolveReadCut(
    VariableReadCutRef cut) const noexcept {
    return const_cast<VariableServiceV1*>(this)->ResolveReadCut(cut);
}

VariableServiceV1::WriteRow* VariableServiceV1::ResolveHistory(HistoryRef ref) noexcept {
    if (!ref.Valid() || ref.slot >= history_.size() ||
        historyGenerations_[ref.slot] != ref.generation) return nullptr;
    auto& row = history_[ref.slot];
    if (row.ticket.Valid() && row.ticket.sequence != ref.sequence) return nullptr;
    return row.used || row.reserved || row.observers != 0 ? &row : nullptr;
}

bool VariableServiceV1::PreparedSourceDependsOn(VariableSliceRef source,
    VariableSliceRef cancelled) const noexcept {
    auto& visited = dependencyVisited_;
    std::size_t count = 0;
    if (!source.Valid()) return false;
    VariableSliceSource cancelledIdentity{};
    if (!slices_.CaptureSource(cancelled, &cancelledIdentity)) return false;
    visited[count++] = source;
    for (std::size_t index = 0; index < count; ++index) {
        const auto current = visited[index];
        if (current.slot == cancelled.slot && current.generation == cancelled.generation) return true;
        std::uint32_t cursor = 0;
        while (const auto* next = slices_.NextSource(current, &cursor)) {
            if (next->ticket < cancelledIdentity.ticket) continue;
            const bool seen = std::any_of(visited.begin(), visited.begin() + count,
                [&](const auto prior) { return prior.slot == next->slice.slot &&
                    prior.generation == next->slice.generation; });
            if (seen) continue;
            // The completion pool bounds the private dependency graph.
            if (count == visited.size()) return true;
            visited[count++] = next->slice;
        }
    }
    return false;
}

bool VariableServiceV1::PinHistorySlice(WriteRow& row) noexcept {
    if (!row.batchLeader || row.historySlicePin || !row.slice.Valid()) return true;
    VariableSliceSource source{};
    if (!slices_.CaptureSource(row.slice, &source) || !slices_.RetainSource(source)) return false;
    row.historySlicePin = true;
    return true;
}

bool VariableServiceV1::RecycleHistoryRow(std::uint32_t slot) noexcept {
    auto& row = history_[slot];
    assert(!row.scheduled && !preparedWritePlacements_[slot].IsHeld());
    if (row.observers != 0) return false;
    if (row.historySlicePin && slices_.SourceObserverCount(row.slice) > 1) return false;
    if (row.historySlicePin) {
        VariableSliceSource source{};
        if (!slices_.CaptureSource(row.slice, &source) || !slices_.ReleaseSource(source)) return false;
        row.historySlicePin = false;
    }
    if (row.cancelledBeforePublication && row.slice.Valid() && !slices_.AbandonPrepared(row.slice)) return false;
    row = WriteRow{};
    assert(occupiedRevisions_ != 0);
    --occupiedRevisions_;
    if (historyGenerations_[slot] != UINT32_MAX) {
        ++historyGenerations_[slot];
        freeSlots_.push_back(slot);
    }
    return true;
}

void VariableServiceV1::ReclaimHistoryRows() noexcept {
    slices_.ReclaimSettled(observedBlock_);
    for (std::uint32_t slot = 0; slot < history_.size(); ++slot) {
        auto& row = history_[slot];
        if (row.state != RowState::Terminal || row.observers != 0) continue;
        if (row.historySlicePin && slices_.SourceObserverCount(row.slice) > 1) continue;
        if (row.slice.Valid()) {
            if (row.cancelledBeforePublication) {
                (void)RecycleHistoryRow(slot);
                continue;
            }
            const auto fence = row.consumedFence != 0 ? row.consumedFence
                : slices_.AppliedBlockSequence(row.slice);
            if (fence == 0 || observedBlock_ <= fence) continue;
            row.consumedFence = fence;
            // Unobserved slices can retire while their value-only outcome awaits delivery.
            if (row.historySlicePin) {
                VariableSliceSource source{};
                if (!slices_.CaptureSource(row.slice, &source) || !slices_.ReleaseSource(source)) continue;
                row.historySlicePin = false;
            }
        }
        if (row.outcomeDelivered) (void)RecycleHistoryRow(slot);
    }
    slices_.ReclaimSettled(observedBlock_);
}

std::size_t VariableServiceV1::ReadCutStorageBytes() const noexcept {
    return static_cast<std::size_t>(readCutCapacity_) * sizeof(ReadCutRow) +
        static_cast<std::size_t>(timelineReadUseCapacity_) * sizeof(TimelineReadUseRow) +
        candidatePinStorage_.capacity() * sizeof(std::uint64_t) +
        historyGenerations_.capacity() * sizeof(std::uint32_t) +
        preparedWritePlacements_.capacity() * sizeof(HeldCurrentSliceToken);
}

std::size_t VariableServiceV1::StorageBytes() const noexcept {
    return ReadCutStorageBytes() + slices_.StorageBytes() +
        history_.capacity() * sizeof(WriteRow) + freeSlots_.capacity() * sizeof(std::uint32_t) +
        outcomes_.capacity() * sizeof(VariableOutcomeRecord) +
        ticketQueryRecords_.capacity() * sizeof(TicketQueryRecord) +
        expiredTicketRecords_.capacity() * sizeof(VnVariableTicketV1) +
        (sealRows_.capacity() + commitRows_.capacity() + prefixRows_.capacity() +
         readCutRows_.capacity() + actionReadCutRows_.capacity() + preparationRows_.capacity() +
         settledRows_.capacity() + supersedeRows_.capacity()) * sizeof(WriteRow*) +
        serviceCandidates_.capacity() * sizeof(std::size_t) +
        scheduledGroups_.capacity() * sizeof(std::uint32_t) +
        censusScratch_.capacity() * sizeof(GatheredTarget) +
        dependencyVisited_.capacity() * sizeof(VariableSliceRef);
}

bool VariableServiceV1::CaptureReadCut(
    VnEmitterHandle emitter, std::uint64_t executeFrame, LogicCommitSequence sequence,
    std::uint32_t executionOrdinalExclusive, VariableReadCutRef* out) noexcept {
    return CaptureReadCutImpl(emitter, executeFrame, sequence, executionOrdinalExclusive, out, nullptr);
}

bool VariableServiceV1::CaptureActionReadCut(VnEmitterHandle emitter, std::uint64_t executeFrame,
    const LogicCommitCoordinator::PreparedIssue& issue, std::uint32_t executionOrdinalExclusive,
    VariableReadCutRef* out, bool pendingPlacement) noexcept {
    ActionOrderIdentity identity{};
    if (!CaptureActionOrder(issue, &identity)) {
        if (out != nullptr) *out = {};
        return false;
    }
    return CaptureReadCutImpl(emitter, executeFrame, issue.Sequence(), executionOrdinalExclusive, out, &identity,
        pendingPlacement);
}

bool VariableServiceV1::CaptureActionReadCut(Engine& engine, VnEmitterHandle emitter, std::uint64_t executeFrame,
    const ActionFiringTable::PreparedChildIdentity& child, std::uint32_t executionOrdinalExclusive,
    VariableReadCutRef* out, bool pendingPlacement) noexcept {
    ActionOrderIdentity identity{};
    if (!CaptureActionOrder(engine, child, &identity)) {
        if (out != nullptr) *out = {};
        return false;
    }
    return CaptureReadCutImpl(emitter, executeFrame, child.Sequence(), executionOrdinalExclusive, out, &identity,
        pendingPlacement);
}

bool VariableServiceV1::CaptureReadCutImpl(
    VnEmitterHandle emitter, std::uint64_t executeFrame, LogicCommitSequence sequence,
    std::uint32_t executionOrdinalExclusive, VariableReadCutRef* out,
    const ActionOrderIdentity* privateIssue, bool pendingPlacement) noexcept {
    if (out == nullptr) return false;
    *out = {};
    if (sequence == 0 || definitions_.size() > kVariableDefinitionCapacity) return false;
    if (emitter != 0 && (static_cast<std::uint32_t>(emitter) >= emitterBanks_.size() ||
        static_cast<std::uint32_t>(emitter >> 32) == 0 ||
        emitterBanks_[static_cast<std::uint32_t>(emitter)].generation != static_cast<std::uint32_t>(emitter >> 32))) return false;
    std::uint32_t slot = 0;
    for (; slot < readCutCapacity_; ++slot) {
        if (readCuts_[slot].state.load(std::memory_order_acquire) == ReadCutState::Free) break;
    }
    if (slot == readCutCapacity_) return false;
    auto& cut = readCuts_[slot];
    cut.world = worldGeneration_;
    cut.transport = transport_.Current();
    cut.emitter = emitter;
    cut.executeFrame = executeFrame;
    cut.sequence = sequence;
    cut.executionOrdinalExclusive = executionOrdinalExclusive;
    cut.preparedIssue = privateIssue == nullptr ? ActionOrderIdentity{} : *privateIssue;
    cut.pendingPlacement = pendingPlacement;
    cut.finalPlacementCaptured = cut.actionOrderConsumed = false;
    cut.candidatePins.reset();
    cut.candidateAdmissionFailed = false;
    cut.valueCount = static_cast<std::uint32_t>(definitions_.size());
    cut.sourceCount = cut.publishedSourceCount = 0;
    cut.needsRebuild = false;
    cut.preparationFailed.store(false, std::memory_order_relaxed);
    cut.nativeHandle = 0;
    cut.timelineUseCount = 0;
    cut.timelineOwnershipTransferred = false;
    assert(!cut.entryPin.Valid());
    cut.entryDescriptor = {};
    cut.entryDecisions = nullptr;
    cut.entryPool = nullptr;
    cut.entryUse.store(EntryReadUseState::None, std::memory_order_relaxed);
    cut.nativeReceipt.store(VariableNativeStartReceipt::None, std::memory_order_relaxed);
    cut.nativeCalendarRequired = false;
    cut.nativeCalendarReservationSequence = 0;
    cut.nativeCalendarConsumed.store(false, std::memory_order_relaxed);
    const VariableScopeKey scope{VariableScopeV1::Emitter,
        static_cast<std::uint32_t>(emitter), static_cast<std::uint32_t>(emitter >> 32)};
    const auto* bank = emitter == 0 ? nullptr : FindEmitterBank(scope);
    const auto frameOf = [](const WriteRow& row) {
        return row.state == RowState::PageCommitted || row.placementHeld ? row.committedFrame : row.requestedFrame;
    };
    const auto sequenceOf = [](const WriteRow& row) { return row.ticket.sequence; };
    const auto included = [&](const WriteRow& row) {
        return row.used || (row.reserved && privateIssue != nullptr &&
            row.preparedIssue.SameReservation(*privateIssue) && IsPreparedActionOrderActive(row.preparedIssue));
    };
    if (pendingPlacement) {
        if (privateIssue == nullptr) return false;
        for (std::size_t index = 0; index < history_.size(); ++index) {
            const auto& row = history_[index];
            if (!included(row) || row.state == RowState::Terminal || row.world != cut.world ||
                row.transport != cut.transport || row.ticket.sequence > sequence ||
                (row.ticket.sequence == sequence && row.executionOrdinal >= executionOrdinalExclusive) ||
                (row.scope.scope != VariableScopeV1::Global &&
                    (emitter == 0 || !ScopeEqual(row.scope, scope)))) continue;
            if (row.observers == UINT32_MAX) return false;
            cut.candidatePins.set(index);
        }
        for (std::size_t index = 0; index < history_.size(); ++index)
            if (cut.candidatePins.test(index)) ++history_[index].observers;
    }
    for (std::uint32_t index = 0; index < cut.valueCount; ++index) {
        const auto& definition = definitions_[index];
        auto& value = cut.values[index];
        value = {};
        value.variableId = definition.definition.variableId;
        const auto* local = bank == nullptr ? nullptr : FindCurrentOverride(*bank, value.variableId);
        value.value = local == nullptr ? definition.currentGlobal : local->currentValue;
        cut.baselineValues[index] = value.value;
        value.baselineOrdinal = local == nullptr ? definition.currentBankOrdinal : local->currentBankOrdinal;
        const WriteRow* winner = nullptr;
        for (const auto& row : history_) {
            if (!included(row) || row.state == RowState::Terminal ||
                row.world != cut.world || row.transport != cut.transport || row.variableId != value.variableId ||
                row.requestedFrame > executeFrame || frameOf(row) > executeFrame ||
                (row.scope.scope != VariableScopeV1::Global && (emitter == 0 || !ScopeEqual(row.scope, scope)))) continue;
            const auto seq = sequenceOf(row);
            if (seq > sequence || (seq == sequence && row.executionOrdinal >= executionOrdinalExclusive)) continue;
            if (winner == nullptr || frameOf(row) > frameOf(*winner) ||
                (frameOf(row) == frameOf(*winner) && (seq > sequenceOf(*winner) ||
                    (seq == sequenceOf(*winner) && row.executionOrdinal > winner->executionOrdinal)))) winner = &row;
        }
        if (winner == nullptr) continue;
        value.value = winner->value;
        const auto* leader = winner;
        if (winner->batchId != 0) {
            for (const auto& member : history_) {
                if (included(member) && member.batchLeader && member.batchId == winner->batchId) {
                    leader = &member;
                    break;
                }
            }
        }
        const auto historySlot = static_cast<std::uint32_t>(leader - history_.data());
        value.source = {historySlot, historyGenerations_[historySlot], sequenceOf(*leader)};
    }
    cut.state.store(ReadCutState::Capturing, std::memory_order_release);
    *out = {slot, cut.generation.load(std::memory_order_relaxed)};
    return true;
}

bool VariableServiceV1::ReadCutValue(VariableReadCutRef ref, VnVariableId id, float* out) noexcept {
    return ReadCutInput(ref, id, out) == VariableReadCutValueStatus::Value;
}

void VariableServiceV1::RegisterCommittedPendingReadCutCandidate(std::uint32_t slot) noexcept {
    auto& row = history_[slot];
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        const VariableScopeKey scope{VariableScopeV1::Emitter,
            static_cast<std::uint32_t>(cut.emitter), static_cast<std::uint32_t>(cut.emitter >> 32)};
        if (cut.state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
            !cut.pendingPlacement || cut.candidateAdmissionFailed || !row.used ||
            row.world != cut.world || row.transport != cut.transport ||
            row.ticket.sequence > cut.sequence ||
            (row.ticket.sequence == cut.sequence && row.executionOrdinal >= cut.executionOrdinalExclusive) ||
            (row.scope.scope != VariableScopeV1::Global &&
                (cut.emitter == 0 || !ScopeEqual(row.scope, scope))) || cut.candidatePins.test(slot)) continue;
        if (row.observers == UINT32_MAX) {
            cut.candidateAdmissionFailed = true;
            cut.preparationFailed.store(true, std::memory_order_release);
            continue;
        }
        ++row.observers;
        cut.candidatePins.set(slot);
        if (cut.finalPlacementCaptured) cut.needsRebuild = true;
    }
}

bool VariableServiceV1::RebuildPendingReadCut(ReadCutRow& cut, std::uint64_t logicalFrame) noexcept {
    // The two Begin entries authenticate their distinct original owners before rebuilding.
    if (!cut.pendingPlacement || !cut.actionOrderConsumed || cut.candidateAdmissionFailed ||
        cut.state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
        cut.world != worldGeneration_ || cut.transport != transport_.Current()) return false;
    for (std::size_t slot = 0; slot < history_.size(); ++slot) {
        if (!cut.candidatePins.test(slot)) continue;
        const auto& row = history_[slot];
        if (row.world != cut.world || row.transport != cut.transport || row.observers == 0 ||
            row.ticket.sequence > cut.sequence ||
            (row.ticket.sequence == cut.sequence && row.executionOrdinal >= cut.executionOrdinalExclusive)) return false;
    }
    cut.needsRebuild = true;
    for (std::uint32_t index = 0; index < cut.sourceCount; ++index) {
        auto* row = ResolveHistory(cut.sources[index]);
        assert(row != nullptr && row->observers != 0);
        if (row == nullptr || row->observers == 0) return false;
    }
    for (std::uint32_t index = 0; index < cut.sourceCount; ++index)
        --ResolveHistory(cut.sources[index])->observers;
    cut.sourceCount = 0;
    cut.preparationFailed.store(false, std::memory_order_relaxed);
    cut.executeFrame = logicalFrame;
    const VariableScopeKey scope{VariableScopeV1::Emitter,
        static_cast<std::uint32_t>(cut.emitter), static_cast<std::uint32_t>(cut.emitter >> 32)};
    const auto frameOf = [](const WriteRow& row) {
        return row.state == RowState::PageCommitted || row.state == RowState::Terminal || row.placementHeld
            ? row.committedFrame : row.requestedFrame;
    };
    for (std::uint32_t index = 0; index < cut.valueCount; ++index) {
        auto& value = cut.values[index];
        value.value = cut.baselineValues[index];
        value.source = {};
        value.read = false;
        const WriteRow* winner = nullptr;
        for (std::size_t slot = 0; slot < history_.size(); ++slot) {
            if (!cut.candidatePins.test(slot)) continue;
            const auto& row = history_[slot];
            if (row.variableId != value.variableId || row.requestedFrame > logicalFrame ||
                frameOf(row) > logicalFrame ||
                (row.scope.scope != VariableScopeV1::Global && (cut.emitter == 0 || !ScopeEqual(row.scope, scope)))) continue;
            if (winner == nullptr || frameOf(row) > frameOf(*winner) ||
                (frameOf(row) == frameOf(*winner) && (row.ticket.sequence > winner->ticket.sequence ||
                    (row.ticket.sequence == winner->ticket.sequence && row.executionOrdinal > winner->executionOrdinal))))
                winner = &row;
        }
        if (winner == nullptr) continue;
        value.value = winner->value;
        const auto* leader = winner;
        if (winner->batchId != 0)
            for (std::size_t slot = 0; slot < history_.size(); ++slot)
                if (cut.candidatePins.test(slot) && history_[slot].batchLeader &&
                    history_[slot].batchId == winner->batchId) { leader = &history_[slot]; break; }
        const auto slot = static_cast<std::uint32_t>(leader - history_.data());
        value.source = {slot, historyGenerations_[slot], leader->ticket.sequence};
    }
    cut.finalPlacementCaptured = true;
    cut.needsRebuild = false;
    return true;
}

VariableReadCutReader VariableServiceV1::ReadCutReader(VariableReadCutRef ref) noexcept {
    return {this, ref, [](void* context, VariableReadCutRef cut, VnObjectId id, float* out) noexcept {
        return static_cast<VariableServiceV1*>(context)->ReadCutInput(cut, id, out);
    }};
}

bool VariableServiceV1::ReadCutHasSources(VariableReadCutRef ref) const noexcept {
    const auto* cut = ResolveReadCut(ref);
    return cut != nullptr && cut->sourceCount != 0;
}

VariableReadCutValueStatus VariableServiceV1::ReadCutInput(
    VariableReadCutRef ref, VnVariableId id, float* out) noexcept {
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || out == nullptr || cut->needsRebuild ||
        cut->preparationFailed.load(std::memory_order_acquire)) return VariableReadCutValueStatus::Failed;
    for (std::uint32_t index = 0; index < cut->valueCount; ++index) {
        auto& value = cut->values[index];
        if (value.variableId != id) continue;
        if (value.source.Valid()) {
            auto* row = ResolveHistory(value.source);
            if (row == nullptr || (row->reserved && !IsPreparedActionOrderActive(row->preparedIssue)) ||
                (row->state == RowState::Terminal &&
                (row->finalOutcome != VariableOutcomeKind::Applied || !row->historySlicePin))) {
                return VariableReadCutValueStatus::Failed;
            }
            if (row->slice.Valid()) {
                const auto state = slices_.State(row->slice);
                if (state == VariableSliceState::Invalidated || state == VariableSliceState::Free ||
                    state == VariableSliceState::Quarantined) return VariableReadCutValueStatus::Failed;
            }
            bool retained = false;
            for (std::uint32_t source = 0; source < cut->sourceCount; ++source) {
                const auto& prior = cut->sources[source];
                if (prior.slot == value.source.slot && prior.generation == value.source.generation) retained = true;
            }
            if (!retained && (!cut->pendingPlacement || cut->finalPlacementCaptured)) {
                if (cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
                    cut->sourceCount == cut->sources.size()) return VariableReadCutValueStatus::Failed;
                if (row->observers == UINT32_MAX) return VariableReadCutValueStatus::Failed;
                ++row->observers;
                cut->sources[cut->sourceCount++] = value.source;
            }
        }
        if (cut->state.load(std::memory_order_acquire) == ReadCutState::Capturing) value.read = true;
        *out = value.value;
        return VariableReadCutValueStatus::Value;
    }
    return VariableReadCutValueStatus::Missing;
}

bool VariableServiceV1::ValidateReadCutPublication(VariableReadCutRef ref,
    std::uint64_t finalFrame, bool* outWaiting) noexcept {
    if (outWaiting != nullptr) *outWaiting = false;
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->world != worldGeneration_ || cut->transport != transport_.Current() ||
        finalFrame != cut->executeFrame || cut->preparationFailed.load(std::memory_order_acquire)) return false;
    if (cut->emitter != 0 && (static_cast<std::uint32_t>(cut->emitter) >= emitterBanks_.size() ||
        emitterBanks_[static_cast<std::uint32_t>(cut->emitter)].generation != static_cast<std::uint32_t>(cut->emitter >> 32))) return false;
    if (cut->state.load(std::memory_order_acquire) == ReadCutState::Published) return true;
    if (cut->needsRebuild) return false;
    // Reordering private writes during placement must rebuild the captured image.
    const VariableScopeKey scope{VariableScopeV1::Emitter,
        static_cast<std::uint32_t>(cut->emitter), static_cast<std::uint32_t>(cut->emitter >> 32)};
    const auto frameOf = [](const WriteRow& row) {
        return row.state == RowState::PageCommitted || row.state == RowState::Terminal || row.placementHeld
            ? row.committedFrame : row.requestedFrame;
    };
    const auto newer = [&](const WriteRow& a, const WriteRow& b) {
        if (frameOf(a) != frameOf(b)) return frameOf(a) > frameOf(b);
        if (a.ticket.sequence != b.ticket.sequence) return a.ticket.sequence > b.ticket.sequence;
        return a.executionOrdinal > b.executionOrdinal;
    };
    for (std::uint32_t index = 0; index < cut->valueCount; ++index) {
        const auto& value = cut->values[index];
        if (!value.read) continue;
        const auto* original = value.source.Valid() ? ResolveHistory(value.source) : nullptr;
        const auto* winner = original;
        for (const auto& row : history_) {
            const bool privateRow = row.reserved && row.preparedIssue.SameReservation(cut->preparedIssue) &&
                IsPreparedActionOrderActive(row.preparedIssue);
            if (cut->pendingPlacement && !cut->candidatePins.test(static_cast<std::size_t>(&row - history_.data())))
                continue;
            if ((!row.used && !privateRow) || row.world != cut->world || row.transport != cut->transport ||
                row.variableId != value.variableId || row.requestedFrame > finalFrame || frameOf(row) > finalFrame ||
                (row.scope.scope != VariableScopeV1::Global && (cut->emitter == 0 || !ScopeEqual(row.scope, scope))) ||
                row.ticket.sequence > cut->sequence ||
                (row.ticket.sequence == cut->sequence && row.executionOrdinal >= cut->executionOrdinalExclusive)) continue;
            if (row.state == RowState::Terminal &&
                (row.finalOutcome != VariableOutcomeKind::Applied || row.bankOrdinal <= value.baselineOrdinal)) continue;
            if (winner == nullptr || newer(row, *winner)) winner = &row;
        }
        if (winner != original && (winner == nullptr || original == nullptr || winner->batchId == 0 ||
            winner->batchId != original->batchId)) return false;
    }
    for (std::uint32_t index = 0; index < cut->sourceCount; ++index) {
        auto* row = ResolveHistory(cut->sources[index]);
        const bool privateRow = row != nullptr && row->reserved && row->preparedEngine != nullptr &&
            row->placementHeld && row->state == RowState::SlicePrepared &&
            row->preparedIssue.SameReservation(cut->preparedIssue) && IsPreparedActionOrderActive(row->preparedIssue);
        if (row == nullptr || (!row->used && !privateRow) ||
            (row->state == RowState::Terminal && row->finalOutcome != VariableOutcomeKind::Applied)) return false;
        if (!row->slice.Valid() || (!privateRow && row->state != RowState::PageCommitted &&
            row->state != RowState::Terminal)) {
            if (outWaiting != nullptr) *outWaiting = true;
            return false;
        }
        if (slices_.EffectiveFrame(row->slice) > finalFrame ||
            (slices_.EffectiveFrame(row->slice) == finalFrame &&
                (row->ticket.sequence > cut->sequence ||
                    (row->ticket.sequence == cut->sequence && row->executionOrdinal >= cut->executionOrdinalExclusive))) ||
            slices_.State(row->slice) == VariableSliceState::Invalidated) return false;
    }
    return true;
}

VariableReadCutStatus VariableServiceV1::PublishReadCut(VariableReadCutRef ref,
                                                      std::uint64_t finalFrame) noexcept {
    bool waiting = false;
    if (!ValidateReadCutPublication(ref, finalFrame, &waiting))
        return waiting ? VariableReadCutStatus::Waiting : VariableReadCutStatus::Failed;
    auto* cut = ResolveReadCut(ref);
    if (cut->state.load(std::memory_order_acquire) == ReadCutState::Published) return ReadCutSourcesStatus(ref);
    for (std::uint32_t index = 0; index < cut->sourceCount; ++index) {
        auto* row = ResolveHistory(cut->sources[index]);
        VariableSliceSource source{};
        if (!slices_.CaptureSource(row->slice, &source) || !slices_.RetainSource(source)) {
            for (std::uint32_t prior = 0; prior < cut->publishedSourceCount; ++prior)
                (void)slices_.ReleaseSource(cut->publishedSources[prior]);
            cut->publishedSourceCount = 0;
            return VariableReadCutStatus::Failed;
        }
        cut->publishedSources[cut->publishedSourceCount++] = source;
    }
    cut->state.store(ReadCutState::Published, std::memory_order_release);
    return ReadCutSourcesStatus(ref);
}

VariableReadCutStatus VariableServiceV1::ReadCutSourcesStatus(VariableReadCutRef ref) const noexcept {
    const auto* cut = ResolveReadCut(ref);
    if (cut == nullptr) return VariableReadCutStatus::Failed;
    if (cut->preparationFailed.load(std::memory_order_acquire)) return VariableReadCutStatus::Failed;
    if (cut->state.load(std::memory_order_acquire) != ReadCutState::Published) return VariableReadCutStatus::Waiting;
    bool waiting = false;
    for (std::uint32_t index = 0; index < cut->publishedSourceCount; ++index) {
        const auto& source = cut->publishedSources[index];
        if (slices_.SourceApplied(source)) continue;
        const auto state = slices_.State(source.slice);
        if (state == VariableSliceState::Invalidated || state == VariableSliceState::Free ||
            state == VariableSliceState::Quarantined) return VariableReadCutStatus::Failed;
        waiting = true;
    }
    return waiting ? VariableReadCutStatus::Waiting : VariableReadCutStatus::Applied;
}

bool VariableServiceV1::CanReleaseReadCut(VariableReadCutRef ref) const noexcept {
    const auto* cut = ResolveReadCut(ref);
    return cut != nullptr && cut->timelineUseCount == 0 && !cut->entryPin.Valid() &&
        cut->entryUse.load(std::memory_order_acquire) == EntryReadUseState::None &&
        cut->nativeReceipt.load(std::memory_order_acquire) == VariableNativeStartReceipt::None &&
        (!cut->nativeCalendarRequired || cut->nativeCalendarConsumed.load(std::memory_order_acquire));
}

bool VariableServiceV1::ReleaseReadCut(VariableReadCutRef ref) noexcept {
    if (!CanReleaseReadCut(ref)) return false;
    auto* cut = ResolveReadCut(ref);
    for (std::uint32_t index = 0; index < cut->publishedSourceCount; ++index)
        (void)slices_.ReleaseSource(cut->publishedSources[index]);
    for (std::uint32_t index = 0; index < cut->sourceCount; ++index) {
        auto* row = ResolveHistory(cut->sources[index]);
        assert(row != nullptr && row->observers != 0);
        if (row != nullptr && row->observers != 0) --row->observers;
    }
    for (std::size_t index = 0; index < history_.size(); ++index) {
        if (!cut->candidatePins.test(index)) continue;
        assert(history_[index].observers != 0);
        --history_[index].observers;
    }
    cut->candidatePins.reset();
    cut->sourceCount = cut->publishedSourceCount = cut->valueCount = 0;
    if (ref.generation == UINT32_MAX) {
        cut->state.store(ReadCutState::Quarantined, std::memory_order_release);
    } else {
        cut->generation.store(ref.generation + 1u, std::memory_order_release);
        cut->state.store(ReadCutState::Free, std::memory_order_release);
    }
    ReclaimHistoryRows();
    return true;
}

const VariableServiceV1::ReadCutRow* VariableServiceV1::ResolveNativeReadCut(
    const VariableNativeStartIdentity& identity) const noexcept {
    const auto* cut = ResolveReadCut(identity.cut);
    if (cut == nullptr || cut->nativeReceipt.load(std::memory_order_acquire) ==
        VariableNativeStartReceipt::None || identity.nativeHandle == 0 ||
        cut->nativeHandle != identity.nativeHandle || cut->world != identity.world ||
        cut->transport != identity.transport || cut->executeFrame != identity.startFrame ||
        cut->sequence != identity.sequence || cut->executionOrdinalExclusive != identity.executionOrdinalExclusive) return nullptr;
    return cut;
}

bool VariableServiceV1::ReserveNativeReadCutCalendar(VariableReadCutRef ref,
    std::uint64_t publicationFrontier, HeldCurrentSliceToken* out) noexcept {
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || out == nullptr || out->IsHeld() || cut->nativeCalendarRequired ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None) return false;
    ScheduledCurrentSliceInput input{};
    input.executeFrame = cut->executeFrame;
    input.logicCommitSequence = cut->sequence;
    input.executionOrdinal = cut->executionOrdinalExclusive;
    input.kind = CurrentSliceKind::NativeStartCalendar;
    input.slice = {ref.slot, ref.generation};
    const auto reserved = timeline_.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse,
                                                           publicationFrontier, out);
    if (reserved.status != ReserveStatus::Committed) return false;
    cut->nativeCalendarRequired = true;
    cut->nativeCalendarReservationSequence = out->ReservationSequence();
    return true;
}

bool VariableServiceV1::CancelNativeReadCutCalendar(VariableReadCutRef ref,
    HeldCurrentSliceToken* held) noexcept {
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || held == nullptr || !held->IsHeld() ||
        held->Kind() != CurrentSliceKind::NativeStartCalendar ||
        held->Slice().slot != ref.slot || held->Slice().generation != ref.generation ||
        held->ExecuteFrame() != cut->executeFrame || !cut->nativeCalendarRequired ||
        held->ReservationSequence() != cut->nativeCalendarReservationSequence ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        !timeline_.CancelHeldCurrentSlice(held)) return false;
    cut->nativeCalendarRequired = false;
    cut->nativeCalendarReservationSequence = 0;
    return true;
}

bool VariableServiceV1::HasNativeReadCutsForTransport(TransportEpoch transport) const noexcept {
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        const auto& cut = readCuts_[index];
        const auto state = cut.state.load(std::memory_order_acquire);
        if ((state == ReadCutState::Capturing || state == ReadCutState::Published) &&
            cut.transport == transport && (cut.timelineUseCount != 0 || cut.nativeCalendarRequired ||
            cut.nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None)) return true;
    }
    return false;
}

bool VariableServiceV1::CaptureNativeReadCutIdentity(VariableReadCutRef ref,
    VnVoiceHandle nativeHandle, VariableNativeStartIdentity* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->nativeReceipt.load(std::memory_order_acquire) == VariableNativeStartReceipt::None ||
        nativeHandle == 0 || cut->nativeHandle != nativeHandle) return false;
    *out = {ref, nativeHandle, cut->world, cut->transport, cut->executeFrame,
            cut->sequence, cut->executionOrdinalExclusive};
    return true;
}

bool VariableServiceV1::ConsumeNativeReadCutCalendar(const ConsumedCurrentSlice& consumed) noexcept {
    if (consumed.kind != CurrentSliceKind::NativeStartCalendar) return false;
    auto* cut = ResolveReadCut({consumed.slice.slot, consumed.slice.generation});
    if (cut == nullptr || cut->state.load(std::memory_order_acquire) != ReadCutState::Published) return false;
    const auto receipt = cut->nativeReceipt.load(std::memory_order_acquire);
    if (receipt == VariableNativeStartReceipt::None || receipt == VariableNativeStartReceipt::Prepared ||
        !cut->nativeCalendarRequired || cut->executeFrame != consumed.executeFrame ||
        cut->nativeCalendarReservationSequence == 0 ||
        cut->nativeCalendarReservationSequence != consumed.reservationSequence ||
        cut->sequence != consumed.logicCommitSequence ||
        cut->executionOrdinalExclusive != consumed.executionOrdinal) return false;
    cut->nativeCalendarConsumed.store(true, std::memory_order_release);
    return true;
}

bool VariableServiceV1::NativeReadCutCalendarConsumed(const VariableNativeStartIdentity& identity) const noexcept {
    const auto* cut = ResolveNativeReadCut(identity);
    return cut != nullptr && (!cut->nativeCalendarRequired ||
        cut->nativeCalendarConsumed.load(std::memory_order_acquire));
}

bool VariableServiceV1::PrepareNativeReadCut(VariableReadCutRef ref, VnVoiceHandle nativeHandle,
    std::uint64_t finalFrame, VariableNativeStartIdentity* out) noexcept {
    if (out == nullptr) return false;
    *out = {};
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || nativeHandle == 0 || cut->timelineUseCount != 0 ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Published ||
        cut->executeFrame != finalFrame ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        ReadCutSourcesStatus(ref) == VariableReadCutStatus::Failed) return false;
    cut->nativeHandle = nativeHandle;
    *out = {ref, nativeHandle, cut->world, cut->transport, finalFrame,
            cut->sequence, cut->executionOrdinalExclusive};
    cut->nativeReceipt.store(VariableNativeStartReceipt::Prepared, std::memory_order_release);
    return true;
}

bool VariableServiceV1::CommitNativeReadCut(const VariableNativeStartIdentity& identity) noexcept {
    const auto* resolved = ResolveNativeReadCut(identity);
    if (resolved == nullptr) return false;
    auto& cut = *const_cast<ReadCutRow*>(resolved);
    auto expected = VariableNativeStartReceipt::Prepared;
    if (!cut.nativeReceipt.compare_exchange_strong(expected, VariableNativeStartReceipt::Pending,
        std::memory_order_release, std::memory_order_relaxed)) return false;
    if (cut.nativeCalendarRequired) {
        const auto quantum = timeline_.QuantumFrames();
        pendingPlaybackPublicationThrough_ = std::max(pendingPlaybackPublicationThrough_,
            cut.executeFrame > UINT64_MAX - quantum ? UINT64_MAX : cut.executeFrame + quantum);
    }
    return true;
}

bool VariableServiceV1::CancelPreparedNativeReadCut(const VariableNativeStartIdentity& identity) noexcept {
    const auto* resolved = ResolveNativeReadCut(identity);
    if (resolved == nullptr) return false;
    auto& cut = *const_cast<ReadCutRow*>(resolved);
    auto expected = VariableNativeStartReceipt::Prepared;
    return cut.nativeReceipt.compare_exchange_strong(expected, VariableNativeStartReceipt::None,
        std::memory_order_release, std::memory_order_relaxed);
}

VariableNativeStartReceipt VariableServiceV1::NativeReadCutStatus(
    const VariableNativeStartIdentity& identity) const noexcept {
    const auto* cut = ResolveNativeReadCut(identity);
    return cut == nullptr ? VariableNativeStartReceipt::None
        : cut->nativeReceipt.load(std::memory_order_acquire);
}

bool VariableServiceV1::MarkNativeReadCutInstalled(
    const VariableNativeStartIdentity& identity) noexcept {
    const auto* resolved = ResolveNativeReadCut(identity);
    if (resolved == nullptr) return false;
    auto& cut = *const_cast<ReadCutRow*>(resolved);
    auto expected = VariableNativeStartReceipt::Pending;
    return cut.nativeReceipt.compare_exchange_strong(expected, VariableNativeStartReceipt::Installed,
        std::memory_order_release, std::memory_order_relaxed);
}

VariableReadCutStatus VariableServiceV1::NativeReadCutSourcesStatus(
    const VariableNativeStartIdentity& identity) const noexcept {
    const auto* cut = ResolveNativeReadCut(identity);
    if (cut == nullptr || cut->nativeReceipt.load(std::memory_order_acquire) !=
        VariableNativeStartReceipt::Installed) return VariableReadCutStatus::Failed;
    return ReadCutSourcesStatus(identity.cut);
}

bool VariableServiceV1::FinishNativeReadCut(const VariableNativeStartIdentity& identity,
    bool activated) noexcept {
    const auto* resolved = ResolveNativeReadCut(identity);
    if (resolved == nullptr || (activated &&
        (!NativeReadCutCalendarConsumed(identity) ||
         NativeReadCutSourcesStatus(identity) != VariableReadCutStatus::Applied))) return false;
    auto& cut = *const_cast<ReadCutRow*>(resolved);
    auto expected = cut.nativeReceipt.load(std::memory_order_acquire);
    if (expected != VariableNativeStartReceipt::Installed &&
        (activated || expected != VariableNativeStartReceipt::Pending)) return false;
    return cut.nativeReceipt.compare_exchange_strong(expected,
        activated ? VariableNativeStartReceipt::Activated : VariableNativeStartReceipt::Failed,
        std::memory_order_release, std::memory_order_relaxed);
}

bool VariableServiceV1::AcknowledgeNativeReadCut(const VariableNativeStartIdentity& identity) noexcept {
    const auto* resolved = ResolveNativeReadCut(identity);
    if (resolved == nullptr) return false;
    auto& cut = *const_cast<ReadCutRow*>(resolved);
    const auto receipt = cut.nativeReceipt.load(std::memory_order_acquire);
    if (receipt != VariableNativeStartReceipt::Activated &&
        receipt != VariableNativeStartReceipt::Failed) return false;
    if (cut.nativeCalendarRequired && !cut.nativeCalendarConsumed.load(std::memory_order_acquire)) return false;
    cut.nativeReceipt.store(VariableNativeStartReceipt::None, std::memory_order_relaxed);
    return ReleaseReadCut(identity.cut);
}

void VariableServiceV1::AcknowledgeFinishedNativeReadCuts() noexcept {
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        const auto state = cut.state.load(std::memory_order_acquire);
        if (state != ReadCutState::Published) continue;
        const auto receipt = cut.nativeReceipt.load(std::memory_order_acquire);
        if (receipt != VariableNativeStartReceipt::Activated && receipt != VariableNativeStartReceipt::Failed)
            continue;
        const VariableNativeStartIdentity identity{{index, cut.generation.load(std::memory_order_relaxed)},
            cut.nativeHandle, cut.world, cut.transport, cut.executeFrame, cut.sequence, cut.executionOrdinalExclusive};
        (void)AcknowledgeNativeReadCut(identity);
    }
}

bool VariableServiceV1::HasNativePublicationHold(VnVoiceHandle nativeHandle) const noexcept {
    for (std::uint32_t index = 0; index < timelineReadUseCapacity_; ++index) {
        const auto& use = timelineReadUses_[index];
        const auto state = use.state.load(std::memory_order_acquire);
        if ((state == TimelineReadUseState::Prepared || state == TimelineReadUseState::Published) &&
            use.nativeHandle == nativeHandle) return true;
    }
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        const auto& cut = readCuts_[index];
        if (cut.state.load(std::memory_order_acquire) != ReadCutState::Published) continue;
        if (cut.nativeHandle == nativeHandle &&
            cut.nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None) return true;
    }
    return false;
}

void VariableServiceV1::ShutdownReadCutsAfterQuiescence() noexcept {
    for (std::uint32_t index = 0; index < timelineReadUseCapacity_; ++index) {
        auto& use = timelineReadUses_[index];
        const auto state = use.state.load(std::memory_order_relaxed);
        if (state == TimelineReadUseState::Prepared || state == TimelineReadUseState::Published)
            ReleaseTimelineReadUse(use);
    }
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        const auto state = cut.state.load(std::memory_order_relaxed);
        if (state != ReadCutState::Capturing && state != ReadCutState::Published) continue;
        cut.nativeReceipt.store(VariableNativeStartReceipt::None, std::memory_order_relaxed);
        // Joined teardown means no calendar or native consumer can read again.
        cut.nativeCalendarRequired = false;
        if (cut.entryPin.Valid()) {
            const bool released = cut.entryPin.Release();
            assert(released);
            (void)released;
        }
        cut.entryUse.store(EntryReadUseState::None, std::memory_order_relaxed);
        cut.entryDescriptor = {};
        cut.entryDecisions = nullptr;
        cut.entryPool = nullptr;
        (void)ReleaseReadCut({index, cut.generation.load(std::memory_order_relaxed)});
    }
}

const VariableServiceV1::TimelineReadUseRow* VariableServiceV1::ResolveTimelineReadUse(
    VariableTimelineReadUseRef ref) const noexcept {
    if (!ref.Valid() || ref.slot >= timelineReadUseCapacity_) return nullptr;
    const auto& use = timelineReadUses_[ref.slot];
    const auto state = use.state.load(std::memory_order_acquire);
    if ((state != TimelineReadUseState::Prepared && state != TimelineReadUseState::Published) ||
        use.generation.load(std::memory_order_acquire) != ref.generation) return nullptr;
    return &use;
}

VariableServiceV1::TimelineReadUseRow* VariableServiceV1::ResolveTimelineReadUse(
    VariableTimelineReadUseRef ref) noexcept {
    return const_cast<TimelineReadUseRow*>(std::as_const(*this).ResolveTimelineReadUse(ref));
}

bool VariableServiceV1::PrepareTimelineReadUse(VariableReadCutRef ref, VnVoiceHandle nativeHandle,
    TimelineArmTable& arms, const TimelineArmTable::PreparedCommitProof& proof,
    VariableTimelineReadUseRef* out, std::uint64_t authoredStartOffset) noexcept {
    if (out == nullptr || out->Valid()) return false;
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->timelineOwnershipTransferred || cut->nativeCalendarRequired ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        (cut->state.load(std::memory_order_acquire) != ReadCutState::Published &&
            cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing) ||
        cut->timelineUseCount == UINT32_MAX || nativeHandle == 0 ||
        cut->preparationFailed.load(std::memory_order_acquire) ||
        (!cut->pendingPlacement && authoredStartOffset != 0) ||
        authoredStartOffset > UINT64_MAX - cut->executeFrame) return false;
    std::uint32_t index = 0;
    for (; index < timelineReadUseCapacity_; ++index)
        if (timelineReadUses_[index].state.load(std::memory_order_acquire) == TimelineReadUseState::Free) break;
    if (index == timelineReadUseCapacity_) return false;
    auto& use = timelineReadUses_[index];
    if (!arms.PinPreparedVariableRead(proof, &use.lifetime)) return false;
    const auto token = use.lifetime.Token();
    const auto* arm = arms.Resolve(token);
    if (token.renderSlot != static_cast<std::uint32_t>(nativeHandle) ||
        token.renderGeneration != static_cast<std::uint32_t>(nativeHandle >> 32) ||
        arm == nullptr || (!arm->pendingImmediateStart && arm->startFrame < cut->executeFrame)) {
        use.lifetime.Release();
        return false;
    }
    use.cut = ref;
    use.nativeHandle = nativeHandle;
    use.sourceOwner = nullptr;
    use.logicalFrame = cut->executeFrame;
    use.onsetFrame = arm->pendingImmediateStart ? cut->executeFrame + authoredStartOffset : arm->startFrame;
    use.authoredStartOffset = authoredStartOffset;
    use.initialCommandCount = 0;
    use.initializer.store(cut->pendingPlacement ? TimelineInitializerState::PendingPlacement :
        TimelineInitializerState::None, std::memory_order_relaxed);
    ++cut->timelineUseCount;
    use.state.store(TimelineReadUseState::Prepared, std::memory_order_release);
    *out = {index, use.generation.load(std::memory_order_relaxed)};
    return true;
}

bool VariableServiceV1::BindPreparedTimelineReadUseSource(VariableTimelineReadUseRef ref,
    OriginalSourceSubmissionRegistry& sources, OriginalSourceLifetimePin&& original) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Prepared ||
        use->initializer.load(std::memory_order_relaxed) != TimelineInitializerState::PendingPlacement ||
        use->originalSource.Valid() || !sources.IsOriginalSourceLifetimePinFor(original, use->lifetime.Token()))
        return false;
    use->sourceOwner = &sources;
    use->originalSource = std::move(original);
    return true;
}

bool VariableServiceV1::BeginPendingTimelineReadUsePlacement(VariableTimelineReadUseRef ref,
    const ImmediateActionLane& lane, const PrivatePlainLayoutCommitProof& placement,
    VariableReadCutRef* cut) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    std::uint64_t logical = 0, onset = 0;
    return use != nullptr && lane.CapturePrivatePendingVariableStartGeometry(placement,
        *use->lifetime.Owner(), use->lifetime.Token(), use->authoredStartOffset, &logical, &onset) &&
        BeginPendingTimelineReadUsePlacement(*use, logical, onset, cut);
}

bool VariableServiceV1::BeginPendingTimelineReadUsePlacement(VariableTimelineReadUseRef ref,
    const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
    VariableReadCutRef* cut) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    std::uint64_t logical = 0, onset = 0;
    return use != nullptr && lane.CapturePrivatePendingVariableStartGeometry(placement,
        *use->lifetime.Owner(), use->lifetime.Token(), use->authoredStartOffset, &logical, &onset) &&
        BeginPendingTimelineReadUsePlacement(*use, logical, onset, cut);
}

bool VariableServiceV1::BeginPendingVariableGroupPlacement(Engine& engine,
    EngineActionPlayScratch& producer, EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, VariableReadCutReader* out) noexcept {
    if (out == nullptr) return false;
    *out = {};
    if (&engine.variables != this) return false;
    const auto* group = engine.OriginalPendingVariableGroupForPlacement(producer, attempt, placement);
    std::uint64_t anchor = 0;
    if (group == nullptr || !placement.CaptureAnchorFrame(&anchor)) return false;
    auto* cut = ResolveReadCut(group->cut);
    if (cut != nullptr && cut->timelineUseCount != 0) {
        if (!OriginalPendingVariableGroupUsesMatch(engine, producer, attempt, placement) ||
            cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
            !RebuildPendingReadCut(*cut, anchor)) return false;
        for (std::uint32_t index = 0; index < group->leafCount; ++index) {
            const auto arm = group->leafArms[index];
            auto* use = ResolveTimelineReadUse(engine.voices.TimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration));
            use->logicalFrame = anchor;
            use->onsetFrame = anchor + use->authoredStartOffset;
            use->initialCommandCount = 0;
            use->initializer.store(TimelineInitializerState::Building, std::memory_order_release);
        }
        *out = ReadCutReader(group->cut);
        return true;
    }
    return BeginUnborrowedPendingVariableGroupCut(engine, producer, group->cut, anchor, out);
}

bool VariableServiceV1::OriginalPendingVariableGroupUsesMatch(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement) const noexcept {
    const auto* group = &engine.variables == this
        ? engine.OriginalPendingVariableGroupForPlacement(producer, attempt, placement) : nullptr;
    const bool originalProducer = producer.row != nullptr && (attempt.privateVcProducerCount != 0
        ? engine.IsRegisteredPrivateVcProducerFor(producer, attempt)
        : attempt.producerSlot < engine.actionPlayScratch.size() &&
          &engine.actionPlayScratch[attempt.producerSlot] == &producer &&
          attempt.producerGeneration == producer.storage.generation &&
          attempt.initialService == producer.row->identity &&
          attempt.initialInvocationGeneration == producer.row->invocationGeneration);
    if (group == nullptr || !originalProducer ||
        group->leafCount == 0 || group->leafCount != producer.voiceStartCount ||
        group->leafCount != producer.row->scheduleLeafCount || group->leafCount > group->leafArms.size() ||
        group->leafRetirement != Engine::OriginalPendingVariableGroupHolder::LeafRetirement::None) return false;
    const auto* cut = ResolveReadCut(group->cut);
    const auto& row = *producer.row;
    if (cut == nullptr || !cut->pendingPlacement || !cut->actionOrderConsumed ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
        cut->timelineUseCount != group->leafCount || cut->timelineOwnershipTransferred ||
        cut->entryPin.Valid() || cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::None ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        cut->nativeCalendarRequired || cut->publishedSourceCount != 0 ||
        cut->world != worldGeneration_ ||
        cut->transport != transport_.Current() || cut->emitter != row.Invocation().invocationEmitter ||
        cut->sequence != row.ticket || cut->executionOrdinalExclusive != row.Invocation().actionExecutionOrdinal ||
        cut->preparationFailed.load(std::memory_order_acquire)) return false;
    for (std::uint32_t index = 0; index < group->leafCount; ++index) {
        if (!engine.MatchesPendingVariableGroupLeaf(*group, index)) return false;
        const auto arm = group->leafArms[index];
        const auto& source = engine.pendingVariableLeafSources[arm.slot];
        const auto ref = engine.voices.TimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration);
        const auto* use = ResolveTimelineReadUse(ref);
        std::uint64_t logical = 0, onset = 0;
        if (source.preparedStartFrame < row.startFrame || use == nullptr ||
            use->state.load(std::memory_order_acquire) != TimelineReadUseState::Published ||
            use->cut != group->cut || use->nativeHandle != source.sourceIdentity ||
            use->nativeHandle != ((static_cast<std::uint64_t>(arm.renderGeneration) << 32) | arm.renderSlot) ||
            use->authoredStartOffset != source.preparedStartFrame - row.startFrame ||
            use->lifetime.Owner() != &engine.timelineArms ||
            !OriginalSourceSubmissionRegistry::SameToken(use->lifetime.Token(), arm) ||
            use->sourceOwner != &engine.originalSourceSubmissions ||
            !engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(use->originalSource, arm) ||
            !engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(source.source, source.sourceIdentity) ||
            !engine.voices.IsOriginalSourceBodyCurrent(source.body)) return false;
        if (!engine.immediateActionLane.CapturePrivateUnboundVariableStartGeometry(placement,
                engine.timelineArms, arm, use->authoredStartOffset, &logical, &onset)) return false;
        const auto initializer = use->initializer.load(std::memory_order_acquire);
        if (initializer != TimelineInitializerState::PendingPlacement && initializer != TimelineInitializerState::Building &&
            initializer != TimelineInitializerState::Staged) return false;
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            const auto previous = group->leafArms[prior];
            const auto previousUse = engine.voices.TimelineVariableSourceRead(previous.renderSlot, previous.renderGeneration);
            if (previous.slot == arm.slot ||
                (previous.renderSlot == arm.renderSlot && previous.renderGeneration == arm.renderGeneration) ||
                (previousUse.slot == ref.slot && previousUse.generation == ref.generation)) return false;
        }
    }
    return true;
}

bool VariableServiceV1::StageOriginalPendingVariableGroupInitializer(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, std::uint32_t leaf,
    std::span<const CommandRecord> commands) noexcept {
    if (!OriginalPendingVariableGroupUsesMatch(engine, producer, attempt, placement)) return false;
    const auto* group = engine.OriginalPendingVariableGroupForPlacement(producer, attempt, placement);
    if (leaf >= group->leafCount) return false;
    const auto arm = group->leafArms[leaf];
    auto* use = ResolveTimelineReadUse(engine.voices.TimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration));
    if (use->initializer.load(std::memory_order_acquire) != TimelineInitializerState::Building ||
        !TimelineInitializerCommandsMatch(use->nativeHandle, commands)) return false;
    std::copy(commands.begin(), commands.end(), use->initialCommands.begin());
    use->initialCommandCount = static_cast<std::uint32_t>(commands.size());
    use->initializer.store(TimelineInitializerState::Staged, std::memory_order_release);
    return true;
}

bool VariableServiceV1::OriginalPendingVariableGroupReadyToFreeze(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, bool* outWaiting) noexcept {
    if (outWaiting != nullptr) *outWaiting = false;
    if (!OriginalPendingVariableGroupUsesMatch(engine, producer, attempt, placement)) return false;
    const auto* group = engine.OriginalPendingVariableGroupForPlacement(producer, attempt, placement);
    std::uint64_t anchor = 0;
    if (!placement.CaptureAnchorFrame(&anchor) || ResolveReadCut(group->cut)->executeFrame != anchor) return false;
    for (std::uint32_t index = 0; index < group->leafCount; ++index) {
        const auto arm = group->leafArms[index];
        const auto* use = ResolveTimelineReadUse(engine.voices.TimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration));
        if (use->initializer.load(std::memory_order_acquire) != TimelineInitializerState::Staged ||
            use->logicalFrame != anchor || use->onsetFrame != anchor + use->authoredStartOffset) return false;
    }
    return ValidateReadCutPublication(group->cut, anchor, outWaiting);
}

bool VariableServiceV1::FreezeOriginalPendingVariableGroup(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement) noexcept {
    bool waiting = false;
    if (!OriginalPendingVariableGroupReadyToFreeze(engine, producer, attempt, placement, &waiting)) return false;
    const auto* group = engine.OriginalPendingVariableGroupForPlacement(producer, attempt, placement);
    const auto* cut = ResolveReadCut(group->cut);
    if (PublishReadCut(group->cut, cut->executeFrame) == VariableReadCutStatus::Failed ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Published) return false;
    for (std::uint32_t index = 0; index < group->leafCount; ++index) {
        const auto arm = group->leafArms[index];
        ResolveTimelineReadUse(engine.voices.TimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration))->initializer.store(
            TimelineInitializerState::Frozen, std::memory_order_release);
    }
    return true;
}

bool VariableServiceV1::BeginPendingVariableGroupReprepare(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    std::uint64_t anchor, VariableReadCutReader* out) noexcept {
    if (out == nullptr) return false;
    *out = {};
    if (&engine.variables != this || attempt.lane.Valid() ||
        (attempt.privateVcProducerCount != 0 &&
         !engine.MatchesRegisteredPendingVariableCohortReprepare(producer, attempt))) return false;
    const auto* group = engine.OriginalPendingVariableGroupForReprepare(producer, attempt);
    if (group == nullptr || !group->ReprepareReady()) return false;
    return BeginUnborrowedPendingVariableGroupCut(engine, producer, group->cut, anchor, out);
}

bool VariableServiceV1::PreparePendingOrdinarySelectionReadCut(Engine& engine,
    EngineActionPlayScratch& producer, EngineImmediateEntryAttempt& attempt, std::uint64_t anchor) noexcept {
    auto* holder = engine.PendingOrdinarySelectionFor(producer.storage);
    auto* cut = ResolveReadCut(producer.storage.initialVariableCut);
    if (&engine.variables != this || producer.row == nullptr || holder == nullptr ||
        !holder->accepted || !holder->sourceRetired ||
        !engine.MatchesPendingOrdinarySelection(*holder, producer.storage, *producer.row) ||
        !engine.MatchesPendingSelectionCensus(attempt) ||
        producer.storage.initialVariableService != this || cut == nullptr ||
        !cut->pendingPlacement || !cut->actionOrderConsumed || cut->timelineUseCount != 0 ||
        cut->timelineOwnershipTransferred || cut->entryPin.Valid() ||
        cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::None ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        cut->nativeCalendarRequired) return false;
    for (std::uint32_t index = 0; index < cut->publishedSourceCount; ++index)
        (void)slices_.ReleaseSource(cut->publishedSources[index]);
    cut->publishedSourceCount = 0;
    cut->needsRebuild = true;
    cut->state.store(ReadCutState::Capturing, std::memory_order_release);
    VariableReadCutReader reader;
    return BeginUnborrowedPendingVariableGroupCut(engine, producer,
        producer.storage.initialVariableCut, anchor, &reader);
}

bool VariableServiceV1::BeginUnborrowedPendingVariableGroupCut(Engine& engine,
    EngineActionPlayScratch& producer, VariableReadCutRef ref,
    std::uint64_t anchor, VariableReadCutReader* out) noexcept {
    auto* cut = ResolveReadCut(ref);
    const auto& row = *producer.row;
    if (cut == nullptr || cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
        !cut->pendingPlacement || !cut->actionOrderConsumed ||
        cut->world != worldGeneration_ ||
        cut->transport != transport_.Current() || cut->emitter != row.Invocation().invocationEmitter ||
        cut->sequence != row.ticket || cut->executionOrdinalExclusive != row.Invocation().actionExecutionOrdinal ||
        cut->timelineUseCount != 0 || cut->timelineOwnershipTransferred || cut->entryPin.Valid() ||
        cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::None ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        cut->nativeCalendarRequired || cut->publishedSourceCount != 0) return false;
    if (cut->emitter != 0 && (static_cast<std::uint32_t>(cut->emitter) >= emitterBanks_.size() ||
        emitterBanks_[static_cast<std::uint32_t>(cut->emitter)].generation !=
            static_cast<std::uint32_t>(cut->emitter >> 32))) return false;
    if (!RebuildPendingReadCut(*cut, anchor)) return false;
    *out = ReadCutReader(ref);
    return true;
}

bool VariableServiceV1::MatchesPreparedTimelineReadUse(VariableTimelineReadUseRef ref,
    VariableReadCutRef expectedCut, VnVoiceHandle nativeHandle,
    const TimelineArmTable::PreparedCommitProof& arm,
    const OriginalSourceSubmissionRegistry& sources, std::uint64_t authoredOffset) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    const auto* cut = ResolveReadCut(expectedCut);
    const auto token = arm.OriginalPreparedToken();
    return arm.Valid() && use != nullptr && cut != nullptr &&
        cut->state.load(std::memory_order_acquire) == ReadCutState::Capturing &&
        use->state.load(std::memory_order_acquire) == TimelineReadUseState::Prepared &&
        use->initializer.load(std::memory_order_acquire) == TimelineInitializerState::PendingPlacement &&
        use->cut == expectedCut && use->nativeHandle == nativeHandle &&
        nativeHandle == ((static_cast<std::uint64_t>(token.renderGeneration) << 32) | token.renderSlot) &&
        use->authoredStartOffset == authoredOffset && use->lifetime.Owner() == arm.Owner() &&
        OriginalSourceSubmissionRegistry::SameToken(use->lifetime.Token(), token) &&
        use->sourceOwner == &sources && sources.IsOriginalSourceLifetimePinFor(use->originalSource, token);
}

bool VariableServiceV1::MatchesPrivateCommittedTimelineReadUse(VariableTimelineReadUseRef ref,
    VariableReadCutRef cutRef, VnVoiceHandle nativeHandle, const TimelineArmTable& arms, TimelineArmToken arm,
    const OriginalSourceSubmissionRegistry& sources, std::uint64_t anchor, std::uint64_t authoredOffset) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    const auto* cut = ResolveReadCut(cutRef);
    return use != nullptr && cut != nullptr && authoredOffset <= UINT64_MAX - anchor &&
        use->state.load(std::memory_order_acquire) == TimelineReadUseState::Prepared &&
        use->initializer.load(std::memory_order_acquire) == TimelineInitializerState::Frozen &&
        use->cut == cutRef && use->nativeHandle == nativeHandle &&
        nativeHandle == ((static_cast<std::uint64_t>(arm.renderGeneration) << 32) | arm.renderSlot) &&
        use->authoredStartOffset == authoredOffset && use->logicalFrame == anchor && use->onsetFrame == anchor + authoredOffset &&
        use->lifetime.Owner() == &arms && OriginalSourceSubmissionRegistry::SameToken(use->lifetime.Token(), arm) &&
        use->sourceOwner == &sources && sources.IsOriginalSourceLifetimePinFor(use->originalSource, arm) &&
        cut->state.load(std::memory_order_acquire) == ReadCutState::Published && cut->pendingPlacement &&
        cut->actionOrderConsumed && cut->executeFrame == anchor && cut->world == worldGeneration_ &&
        cut->transport == transport_.Current() && !cut->timelineOwnershipTransferred && !cut->entryPin.Valid() &&
        cut->entryUse.load(std::memory_order_acquire) == EntryReadUseState::None &&
        cut->nativeReceipt.load(std::memory_order_acquire) == VariableNativeStartReceipt::None &&
        !cut->nativeCalendarRequired && !cut->preparationFailed.load(std::memory_order_acquire);
}

bool VariableServiceV1::MatchesPublishedPendingVariableGroupReadCut(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const EngineActionPlayScratch& child, const PrivateImmediateLanePlacementProof& placement,
    bool entryBound) const noexcept {
    const auto* group = &engine.variables == this
        ? engine.OriginalPendingVariableGroupForReprepare(producer, attempt) : nullptr;
    if (group == nullptr || producer.row == nullptr || group->cut != child.pendingVariableReplacementCut ||
        !engine.MatchesPendingVariableReplacementProducer(child, producer, attempt) ||
        !engine.immediateActionLane.IsPrivatePlacementProofFor(placement, attempt.lane, attempt.root)) return false;
    const auto* cut = ResolveReadCut(group->cut);
    const auto& row = *producer.row;
    std::uint64_t anchor = 0;
    if (cut == nullptr || !placement.CaptureAnchorFrame(&anchor) ||
        anchor != child.pendingVariableReplacementAnchorFrame ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Published ||
        !cut->pendingPlacement || !cut->actionOrderConsumed || !cut->finalPlacementCaptured || cut->needsRebuild ||
        cut->preparationFailed.load(std::memory_order_acquire) || cut->candidateAdmissionFailed ||
        cut->world != worldGeneration_ ||
        cut->transport != transport_.Current() || cut->transport != engine.timelinePool.Epoch() ||
        cut->emitter != row.Invocation().invocationEmitter || cut->sequence != row.ticket ||
        cut->executionOrdinalExclusive != row.Invocation().actionExecutionOrdinal || cut->executeFrame != anchor ||
        cut->timelineUseCount != child.voiceStartCount || cut->timelineOwnershipTransferred ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        cut->nativeCalendarRequired) return false;
    if (!entryBound)
        return !cut->entryPin.Valid() && cut->entryUse.load(std::memory_order_acquire) == EntryReadUseState::None;
    const auto& descriptor = child.frontierDescriptor;
    const auto* activation = engine.decisions.ResolveFrontierActivation(descriptor.entry.coordinator);
    return descriptor.Valid() && descriptor.initialVariables == group->cut &&
        descriptor.order.executeFrame == anchor && descriptor.order.logicCommitSequence == row.ticket &&
        descriptor.order.executionOrdinal == row.Invocation().actionExecutionOrdinal &&
        cut->entryUse.load(std::memory_order_acquire) == EntryReadUseState::Prepared &&
        cut->entryDescriptor == descriptor && cut->entryDecisions == &engine.decisions &&
        cut->entryPool == &engine.timelinePool && cut->entryPin.Valid() &&
        cut->entryPin.Owner() == &engine.decisions &&
        cut->entryPin.Token().slot == descriptor.entry.coordinator.slot &&
        cut->entryPin.Token().generation == descriptor.entry.coordinator.generation &&
        activation != nullptr && activation->State() == FrontierReceiptState::Prepared &&
        activation->Descriptor() == descriptor &&
        engine.decisions.MatchesPreparedFrontierActivationPool(descriptor.entry, engine.timelinePool);
}

bool VariableServiceV1::MatchesEntryBoundTimelineReadUse(VariableTimelineReadUseRef ref,
    VariableReadCutRef cutRef, VnVoiceHandle nativeHandle, const TimelineArmTable& arms, TimelineArmToken arm,
    const OriginalSourceSubmissionRegistry& sources, const DecisionTable& decisions,
    const ScheduledTransactionPool& pool, const FrontierActivationDescriptor& descriptor,
    std::uint32_t leafCount, std::uint64_t authoredOffset) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    const auto* cut = ResolveReadCut(cutRef);
    const auto anchor = descriptor.order.executeFrame;
    const auto* activation = decisions.ResolveFrontierActivation(descriptor.entry.coordinator);
    return descriptor.Valid() && descriptor.initialVariables == cutRef &&
        use != nullptr && cut != nullptr && authoredOffset <= UINT64_MAX - anchor &&
        use->state.load(std::memory_order_acquire) == TimelineReadUseState::Prepared &&
        use->initializer.load(std::memory_order_acquire) == TimelineInitializerState::Frozen &&
        use->cut == cutRef && use->nativeHandle == nativeHandle &&
        nativeHandle == ((static_cast<std::uint64_t>(arm.renderGeneration) << 32) | arm.renderSlot) &&
        use->authoredStartOffset == authoredOffset && use->logicalFrame == anchor &&
        use->onsetFrame == anchor + authoredOffset && use->lifetime.Owner() == &arms &&
        OriginalSourceSubmissionRegistry::SameToken(use->lifetime.Token(), arm) &&
        use->sourceOwner == &sources && sources.IsOriginalSourceLifetimePinFor(use->originalSource, arm) &&
        cut->state.load(std::memory_order_acquire) == ReadCutState::Published && cut->pendingPlacement &&
        cut->actionOrderConsumed && cut->executeFrame == anchor && cut->world == worldGeneration_ &&
        cut->transport == transport_.Current() &&
        cut->transport == descriptor.primary.transportEpoch && cut->sequence == descriptor.order.logicCommitSequence &&
        cut->executionOrdinalExclusive == descriptor.order.executionOrdinal && cut->timelineUseCount == leafCount &&
        !cut->timelineOwnershipTransferred && cut->entryUse.load(std::memory_order_acquire) == EntryReadUseState::Prepared &&
        cut->entryDescriptor == descriptor && cut->entryDecisions == &decisions && cut->entryPool == &pool &&
        cut->entryPin.Valid() && cut->entryPin.Owner() == &decisions &&
        cut->entryPin.Token().slot == descriptor.entry.coordinator.slot &&
        cut->entryPin.Token().generation == descriptor.entry.coordinator.generation &&
        activation != nullptr && activation->State() == FrontierReceiptState::Prepared &&
        activation->Descriptor() == descriptor && decisions.MatchesPreparedFrontierActivationPool(descriptor.entry, pool) &&
        cut->nativeReceipt.load(std::memory_order_acquire) == VariableNativeStartReceipt::None &&
        !cut->nativeCalendarRequired && !cut->preparationFailed.load(std::memory_order_acquire);
}

bool VariableServiceV1::PreparedPendingVariableGroupUsesMatch(const EngineActionPlayScratch& child,
    const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement) const noexcept {
    const auto* cut = ResolveReadCut(child.pendingVariableReplacementCut);
    std::uint64_t anchor = 0;
    if (cut == nullptr || !placement.CaptureAnchorFrame(&anchor) ||
        anchor != child.pendingVariableReplacementAnchorFrame || !cut->pendingPlacement ||
        !cut->actionOrderConsumed || cut->executeFrame != anchor ||
        cut->world != worldGeneration_ || cut->transport != transport_.Current() ||
        cut->timelineUseCount != child.voiceStartCount || cut->timelineOwnershipTransferred ||
        cut->entryPin.Valid() || cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::None ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        cut->nativeCalendarRequired || cut->preparationFailed.load(std::memory_order_acquire)) return false;
    for (std::uint32_t index = 0; index < child.voiceStartCount; ++index) {
        const auto& start = child.voiceStarts[index];
        const auto* use = ResolveTimelineReadUse(start.initialVariableReadUse);
        const auto& proof = child.voiceOwnerContexts[index].armCommitProof;
        const auto token = proof.OriginalPreparedToken();
        std::uint64_t logical = 0, onset = 0;
        if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Prepared ||
            use->cut != child.pendingVariableReplacementCut || use->nativeHandle != start.renderHandle ||
            start.renderHandle != ((static_cast<std::uint64_t>(token.renderGeneration) << 32) | token.renderSlot) ||
            use->authoredStartOffset != child.pendingVariableReplacementLeaves[index].authoredOffset ||
            use->lifetime.Owner() != proof.Owner() ||
            !OriginalSourceSubmissionRegistry::SameToken(use->lifetime.Token(), token) ||
            use->sourceOwner == nullptr || !use->sourceOwner->IsOriginalSourceLifetimePinFor(use->originalSource, token) ||
            !lane.CapturePrivatePreparedVariableStartGeometry(placement, proof, use->authoredStartOffset,
                &logical, &onset) || use->logicalFrame != logical || use->onsetFrame != onset) return false;
        const auto initializer = use->initializer.load(std::memory_order_acquire);
        if (initializer != TimelineInitializerState::PendingPlacement && initializer != TimelineInitializerState::Building &&
            initializer != TimelineInitializerState::Staged && initializer != TimelineInitializerState::Frozen) return false;
    }
    return true;
}

bool VariableServiceV1::BeginPreparedPendingVariableGroupPlacement(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, VariableReadCutReader* out) noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* child = &engine.variables == this
        ? engine.OriginalPreparedPendingVariableGroupChildren(producer, attempt, placement) : nullptr;
    if (child == nullptr || !PreparedPendingVariableGroupUsesMatch(*child, engine.immediateActionLane, placement))
        return false;
    const auto* cut = ResolveReadCut(child->pendingVariableReplacementCut);
    if (child->voiceStartCount == 0) {
        const auto state = cut->state.load(std::memory_order_acquire);
        if (state != ReadCutState::Capturing && state != ReadCutState::Published) return false;
        *out = ReadCutReader(child->pendingVariableReplacementCut);
        return true;
    }
    const auto first = ResolveTimelineReadUse(child->voiceStarts[0].initialVariableReadUse)->initializer.load(std::memory_order_acquire);
    for (std::uint32_t index = 0; index < child->voiceStartCount; ++index) {
        const auto initializer = ResolveTimelineReadUse(child->voiceStarts[index].initialVariableReadUse)->initializer.load(std::memory_order_acquire);
        if ((first == TimelineInitializerState::PendingPlacement && initializer != first) ||
            ((first == TimelineInitializerState::Frozen) != (initializer == TimelineInitializerState::Frozen)) ||
            (initializer == TimelineInitializerState::PendingPlacement && first != initializer)) return false;
    }
    if (first == TimelineInitializerState::Frozen) {
        if (cut->state.load(std::memory_order_acquire) != ReadCutState::Published) return false;
    } else {
        if (cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing) return false;
        if (first == TimelineInitializerState::PendingPlacement)
            for (std::uint32_t index = 0; index < child->voiceStartCount; ++index)
                ResolveTimelineReadUse(child->voiceStarts[index].initialVariableReadUse)->initializer.store(
                    TimelineInitializerState::Building, std::memory_order_release);
    }
    *out = ReadCutReader(child->pendingVariableReplacementCut);
    return true;
}

bool VariableServiceV1::TimelineInitializerCommandsMatch(VnVoiceHandle nativeHandle,
    std::span<const CommandRecord> commands) noexcept {
    if (commands.size() > kTimelineInitialCommandCapacity) return false;
    for (const auto& command : commands) {
        VnVoiceHandle handle = 0;
        switch (command.opcode) {
        case kOpcodeSetVoiceVolume: handle = command.payload.setVoiceVolume.handle; break;
        case kOpcodeSetVoicePitch: handle = command.payload.setVoicePitch.handle; break;
        case kOpcodeSetVoiceFilter: handle = command.payload.setVoiceFilter.handle; break;
        case kOpcodeSetVoiceLowShelf: handle = command.payload.setVoiceShelf.handle; break;
        case kOpcodeSetVoicePan: handle = command.payload.setVoicePan.handle; break;
        case kOpcodeSetVoiceSpatialScalar: handle = command.payload.setVoiceSpatialScalar.handle; break;
        default: return false;
        }
        if (handle != nativeHandle) return false;
    }
    return true;
}

bool VariableServiceV1::StagePreparedPendingVariableGroupInitializer(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, std::uint32_t leaf,
    std::span<const CommandRecord> commands) noexcept {
    const auto* child = &engine.variables == this
        ? engine.OriginalPreparedPendingVariableGroupChildren(producer, attempt, placement) : nullptr;
    if (child == nullptr || leaf >= child->voiceStartCount ||
        !PreparedPendingVariableGroupUsesMatch(*child, engine.immediateActionLane, placement)) return false;
    auto* use = ResolveTimelineReadUse(child->voiceStarts[leaf].initialVariableReadUse);
    const auto* cut = ResolveReadCut(child->pendingVariableReplacementCut);
    if (cut->state.load(std::memory_order_acquire) != ReadCutState::Capturing ||
        use->initializer.load(std::memory_order_acquire) != TimelineInitializerState::Building ||
        !TimelineInitializerCommandsMatch(use->nativeHandle, commands)) return false;
    std::copy(commands.begin(), commands.end(), use->initialCommands.begin());
    use->initialCommandCount = static_cast<std::uint32_t>(commands.size());
    use->initializer.store(TimelineInitializerState::Staged, std::memory_order_release);
    return true;
}

bool VariableServiceV1::PreparedPendingVariableGroupReadyToFreeze(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, bool* outWaiting) noexcept {
    if (outWaiting != nullptr) *outWaiting = false;
    const auto* child = &engine.variables == this
        ? engine.OriginalPreparedPendingVariableGroupChildren(producer, attempt, placement) : nullptr;
    if (child == nullptr || !PreparedPendingVariableGroupUsesMatch(*child, engine.immediateActionLane, placement))
        return false;
    if (child->voiceStartCount == 0) {
        const auto* cut = ResolveReadCut(child->pendingVariableReplacementCut);
        return ValidateReadCutPublication(child->pendingVariableReplacementCut, cut->executeFrame, outWaiting);
    }
    const auto first = ResolveTimelineReadUse(child->voiceStarts[0].initialVariableReadUse)->initializer.load(std::memory_order_acquire);
    if (first != TimelineInitializerState::Staged && first != TimelineInitializerState::Frozen) return false;
    for (std::uint32_t index = 0; index < child->voiceStartCount; ++index)
        if (ResolveTimelineReadUse(child->voiceStarts[index].initialVariableReadUse)->initializer.load(std::memory_order_acquire) != first)
            return false;
    const auto* cut = ResolveReadCut(child->pendingVariableReplacementCut);
    return ValidateReadCutPublication(child->pendingVariableReplacementCut, cut->executeFrame, outWaiting);
}

bool VariableServiceV1::FreezePreparedPendingVariableGroup(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt,
    const PrivateImmediateLanePlacementProof& placement, bool* outWaiting) noexcept {
    if (outWaiting != nullptr) *outWaiting = false;
    if (!PreparedPendingVariableGroupReadyToFreeze(engine, producer, attempt, placement, outWaiting)) return false;
    const auto* child = engine.OriginalPreparedPendingVariableGroupChildren(producer, attempt, placement);
    auto* cut = ResolveReadCut(child->pendingVariableReplacementCut);
    const auto first = child->voiceStartCount == 0
        ? (cut->state.load(std::memory_order_acquire) == ReadCutState::Published
            ? TimelineInitializerState::Frozen : TimelineInitializerState::Staged)
        : ResolveTimelineReadUse(child->voiceStarts[0].initialVariableReadUse)->initializer.load(std::memory_order_acquire);
    if (first != TimelineInitializerState::Staged && first != TimelineInitializerState::Frozen) return false;
    for (std::uint32_t index = 0; index < child->voiceStartCount; ++index)
        if (ResolveTimelineReadUse(child->voiceStarts[index].initialVariableReadUse)->initializer.load(std::memory_order_acquire) != first)
            return false;
    if (first == TimelineInitializerState::Frozen)
        return cut->state.load(std::memory_order_acquire) == ReadCutState::Published;
    const auto published = PublishReadCut(child->pendingVariableReplacementCut, cut->executeFrame);
    if (published == VariableReadCutStatus::Waiting &&
        cut->state.load(std::memory_order_acquire) == ReadCutState::Capturing) {
        if (outWaiting != nullptr) *outWaiting = true;
        return false;
    }
    if (published == VariableReadCutStatus::Failed || cut->state.load(std::memory_order_acquire) != ReadCutState::Published)
        return false;
    for (std::uint32_t index = 0; index < child->voiceStartCount; ++index)
        ResolveTimelineReadUse(child->voiceStarts[index].initialVariableReadUse)->initializer.store(
            TimelineInitializerState::Frozen, std::memory_order_release);
    return true;
}

bool VariableServiceV1::ResetPrivatePendingVariableGroupCut(Engine& engine,
    EngineActionPlayScratch& producer, const EngineImmediateEntryAttempt& attempt) noexcept {
    const auto* child = &engine.variables == this
        ? engine.OriginalCancelledPendingVariableGroupChildren(producer, attempt) : nullptr;
    if (child == nullptr) return false;
    auto* cut = ResolveReadCut(child->pendingVariableReplacementCut);
    if (cut == nullptr || !cut->pendingPlacement || !cut->actionOrderConsumed || cut->timelineUseCount != 0 ||
        cut->timelineOwnershipTransferred || cut->entryPin.Valid() ||
        cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::None ||
        cut->nativeReceipt.load(std::memory_order_acquire) != VariableNativeStartReceipt::None ||
        cut->nativeCalendarRequired) return false;
    for (std::uint32_t index = 0; index < cut->publishedSourceCount; ++index)
        (void)slices_.ReleaseSource(cut->publishedSources[index]);
    cut->publishedSourceCount = 0;
    cut->needsRebuild = true;
    cut->state.store(ReadCutState::Capturing, std::memory_order_release);
    return true;
}

bool VariableServiceV1::BeginPendingTimelineReadUsePlacement(TimelineReadUseRow& use,
    std::uint64_t logicalFrame, std::uint64_t onsetFrame, VariableReadCutRef* out) noexcept {
    auto* cut = ResolveReadCut(use.cut);
    const auto initializer = use.initializer.load(std::memory_order_acquire);
    if (out != nullptr && cut != nullptr && initializer == TimelineInitializerState::Frozen &&
        use.logicalFrame == logicalFrame && use.onsetFrame == onsetFrame) {
        *out = use.cut;
        return true;
    }
    if (out == nullptr || cut == nullptr || use.state.load(std::memory_order_acquire) != TimelineReadUseState::Published ||
        (initializer != TimelineInitializerState::PendingPlacement && initializer != TimelineInitializerState::Building) ||
        cut->timelineUseCount != 1 || use.sourceOwner == nullptr ||
        !use.sourceOwner->IsOriginalInstalledSourceCurrent(use.originalSource, use.nativeHandle) ||
        !RebuildPendingReadCut(*cut, logicalFrame)) return false;
    use.logicalFrame = logicalFrame;
    use.onsetFrame = onsetFrame;
    use.initializer.store(TimelineInitializerState::Building, std::memory_order_release);
    *out = use.cut;
    return true;
}

bool VariableServiceV1::FreezePendingTimelineReadUse(VariableTimelineReadUseRef ref,
    std::span<const CommandRecord> commands) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->initializer.load(std::memory_order_acquire) != TimelineInitializerState::Building ||
        commands.size() > use->initialCommands.size()) return false;
    // This first stage supports builtin tuples only; effect identities need their own sidecars.
    if (!TimelineInitializerCommandsMatch(use->nativeHandle, commands)) return false;
    const auto published = PublishReadCut(use->cut, use->logicalFrame);
    const auto* cut = ResolveReadCut(use->cut);
    if (published == VariableReadCutStatus::Failed || cut == nullptr ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Published) return false;
    std::copy(commands.begin(), commands.end(), use->initialCommands.begin());
    use->initialCommandCount = static_cast<std::uint32_t>(commands.size());
    use->initializer.store(TimelineInitializerState::Frozen, std::memory_order_release);
    return true;
}

bool VariableServiceV1::PendingTimelineReadUseFrozen(VariableTimelineReadUseRef ref) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    return use != nullptr && use->initializer.load(std::memory_order_acquire) == TimelineInitializerState::Frozen;
}

bool VariableServiceV1::PendingTimelineReadUseStaged(VariableTimelineReadUseRef ref) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    return use != nullptr && use->initializer.load(std::memory_order_acquire) == TimelineInitializerState::Staged;
}

bool VariableServiceV1::ProveTimelineReadUseInitializer(VariableTimelineReadUseRef ref,
    VnVoiceHandle nativeHandle, const TimelineArmTable::PlainAudioProof& proof,
    VariableTimelineInitializerProof* out) noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    return use != nullptr && use->lifetime.Owner()->MatchesVariableInitializerStart(
        proof, use->lifetime.Token(), use->onsetFrame) && CaptureTimelineInitializerProof(ref, nativeHandle, out);
}

bool VariableServiceV1::ProveTimelineReadUseInitializer(VariableTimelineReadUseRef ref,
    VnVoiceHandle nativeHandle, const TimelineArmTable::LaneAudioProof& proof,
    VariableTimelineInitializerProof* out) noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    return use != nullptr && use->lifetime.Owner()->MatchesVariableInitializerStart(
        proof, use->lifetime.Token(), use->onsetFrame) && CaptureTimelineInitializerProof(ref, nativeHandle, out);
}

bool VariableServiceV1::ProveTimelineReadUseInitializer(VariableTimelineReadUseRef ref,
    VnVoiceHandle nativeHandle, const TimelineArmTable::BoundAudioProof& proof,
    VariableTimelineInitializerProof* out) noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    return use != nullptr && use->lifetime.Owner()->MatchesVariableInitializerStart(
        proof, use->lifetime.Token(), use->onsetFrame) && CaptureTimelineInitializerProof(ref, nativeHandle, out);
}

bool VariableServiceV1::CaptureTimelineInitializerProof(VariableTimelineReadUseRef ref,
    VnVoiceHandle nativeHandle, VariableTimelineInitializerProof* out) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->nativeHandle != nativeHandle || out == nullptr || out->Valid() ||
        TimelineReadUseStartStatus(ref, nativeHandle) != VariableReadCutStatus::Applied) return false;
    const auto initializer = use->initializer.load(std::memory_order_acquire);
    if (initializer != TimelineInitializerState::None && initializer != TimelineInitializerState::Frozen) return false;
    const auto* cut = ResolveReadCut(use->cut);
    out->row_ = use;
    out->nativeHandle_ = nativeHandle;
    out->order_ = {use->logicalFrame, cut->sequence, cut->executionOrdinalExclusive};
    out->commands_ = use->initialCommands.data();
    out->commandCount_ = initializer == TimelineInitializerState::Frozen ? use->initialCommandCount : 0;
    return true;
}

void VariableServiceV1::ApplyTimelineReadUseInitializer(VariableTimelineInitializerProof& proof,
    void* context, void (*apply)(void*, const CommandRecord&) noexcept) noexcept {
    assert(proof.Valid() && apply != nullptr);
    auto& use = *static_cast<TimelineReadUseRow*>(proof.row_);
    for (std::uint32_t index = 0; index < proof.commandCount_; ++index) apply(context, proof.commands_[index]);
    use.initializer.store(TimelineInitializerState::Consumed, std::memory_order_release);
    proof.Reset();
}

bool VariableServiceV1::ProveTimelineReadUseInitializer(VariableTimelineReadUseRef ref,
    VnVoiceHandle nativeHandle, const TimelineArmTable::AudioOperationClaim& proof,
    VariableTimelineInitializerProof* out) noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    return use != nullptr && use->lifetime.Owner()->MatchesVariableInitializerStart(
        proof, use->lifetime.Token(), use->onsetFrame) && CaptureTimelineInitializerProof(ref, nativeHandle, out);
}

bool VariableServiceV1::CommitTimelineReadUse(VariableTimelineReadUseRef ref) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Prepared)
        return false;
    use->state.store(TimelineReadUseState::Published, std::memory_order_release);
    return true;
}

bool VariableServiceV1::CanTransferReadCutToTimelineUses(VariableReadCutRef ref) const noexcept {
    const auto* cut = ResolveReadCut(ref);
    return cut != nullptr && (cut->timelineUseCount != 0 ||
        cut->entryUse.load(std::memory_order_acquire) == EntryReadUseState::Published) &&
        !cut->timelineOwnershipTransferred;
}

bool VariableServiceV1::TransferReadCutToTimelineUses(VariableReadCutRef ref) noexcept {
    if (!CanTransferReadCutToTimelineUses(ref)) return false;
    ResolveReadCut(ref)->timelineOwnershipTransferred = true;
    return true;
}

bool VariableServiceV1::PrepareFrontierEntryReadUse(VariableReadCutRef ref,
    DecisionTable& decisions, ScheduledTransactionPool& pool,
    const FrontierActivationDescriptor& descriptor) noexcept {
    auto* cut = ResolveReadCut(ref);
    const auto* activation = decisions.ResolveFrontierActivation(descriptor.entry.coordinator);
    DecisionCommittedSnapshot coordinator{};
    Plan03PageGroupView group{};
    if (cut == nullptr || !descriptor.Valid() || !(descriptor.initialVariables == ref) ||
        cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::None || cut->entryPin.Valid() ||
        cut->state.load(std::memory_order_acquire) != ReadCutState::Published || cut->timelineOwnershipTransferred ||
        cut->world != worldGeneration_ || cut->transport != transport_.Current() ||
        cut->transport != descriptor.primary.transportEpoch ||
        cut->executeFrame != descriptor.order.executeFrame || cut->sequence != descriptor.order.logicCommitSequence ||
        activation == nullptr || activation->State() != FrontierReceiptState::Prepared ||
        !(activation->Descriptor() == descriptor) ||
        !decisions.CaptureCommitted(descriptor.entry.coordinator, &coordinator) ||
        coordinator.ticket.sequence != cut->sequence || coordinator.world != descriptor.worldGeneration ||
        coordinator.transport != cut->transport || coordinator.audioState != AudioGroupState::Pending ||
        coordinator.participantCount != descriptor.expectedMemberCount || coordinator.consumedSlices != 0 ||
        !pool.PrivatePlan03GroupBindable(descriptor.primary) ||
        !pool.ResolvePlan03GroupSliceProducer(descriptor.primary, &group) || group.frontier == nullptr ||
        !group.frontier->Valid() || group.frontier->role != FrontierGroupRole::Coordinator ||
        !(group.frontier->entry == descriptor.entry) || !(group.frontier->primary == descriptor.primary) ||
        !(group.frontier->operation.group == descriptor.primary) ||
        !(group.frontier->operation.owner == descriptor.entry.owner) ||
        group.frontier->operation.order != descriptor.order ||
        !(group.frontier->initialVariables == ref) || group.lease == nullptr ||
        group.lease->Capture().state != AudioSliceState::Ready ||
        !decisions.AcquireFuturePin(descriptor.entry.coordinator, &cut->entryPin)) return false;
    cut->entryDescriptor = descriptor;
    cut->entryDecisions = &decisions;
    cut->entryPool = &pool;
    cut->entryUse.store(EntryReadUseState::Prepared, std::memory_order_release);
    return true;
}

bool VariableServiceV1::CommitFrontierEntryReadUse(VariableReadCutRef ref) noexcept {
    auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::Prepared ||
        !cut->entryPin.Valid() || cut->entryDecisions == nullptr || cut->entryPool == nullptr) return false;
    const auto* activation = cut->entryDecisions->ResolveFrontierActivation(cut->entryDescriptor.entry.coordinator);
    Plan03PageGroupView group{};
    if (activation == nullptr || activation->State() != FrontierReceiptState::Prepared ||
        !(activation->Descriptor() == cut->entryDescriptor) ||
        !cut->entryPool->ResolvePlan03GroupSliceProducer(cut->entryDescriptor.primary, &group) ||
        group.frontier == nullptr || !(group.frontier->initialVariables == ref) ||
        !(group.frontier->entry == cut->entryDescriptor.entry) || group.lease == nullptr ||
        group.lease->Capture().state != AudioSliceState::Ready) return false;
    cut->entryUse.store(EntryReadUseState::Published, std::memory_order_release);
    return true;
}

bool VariableServiceV1::CanCommitFrontierEntryReadUse(VariableReadCutRef ref,
    const DecisionTable::FrontierPublicationProof& activation,
    const PrivateImmediateLaneGroupsPublicationProof& groups) const noexcept {
    const auto* cut = ResolveReadCut(ref);
    if (cut == nullptr || cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::Prepared ||
        !cut->entryPin.Valid() || cut->entryDecisions == nullptr || cut->entryPool == nullptr ||
        cut->entryDescriptor.initialVariables != ref ||
        !cut->entryDecisions->IsFrontierPublicationProofFor(activation, cut->entryDescriptor) ||
        !cut->entryPool->IsPrivateImmediateLaneGroupsPublicationProofFor(
            groups, cut->entryDescriptor.primary, cut->entryDescriptor.entry)) return false;
    Plan03PageGroupView group{};
    return cut->entryPool->ResolvePlan03GroupSliceProducer(cut->entryDescriptor.primary, &group) &&
        group.frontier != nullptr && group.frontier->initialVariables == ref &&
        group.frontier->primary == cut->entryDescriptor.primary;
}

bool VariableServiceV1::CommitFrontierEntryReadUse(VariableReadCutRef ref,
    const DecisionTable::FrontierPublicationProof& activation,
    const PrivateImmediateLaneGroupsPublicationProof& groups) noexcept {
    if (!CanCommitFrontierEntryReadUse(ref, activation, groups)) return false;
    ResolveReadCut(ref)->entryUse.store(EntryReadUseState::Published, std::memory_order_release);
    return true;
}

VariableReadCutStatus VariableServiceV1::FrontierEntrySourcesStatus(
    const FrontierActivationDescriptor& descriptor) const noexcept {
    const auto* cut = ResolveReadCut(descriptor.initialVariables);
    if (cut == nullptr || cut->entryUse.load(std::memory_order_acquire) != EntryReadUseState::Published ||
        !(cut->entryDescriptor == descriptor)) return VariableReadCutStatus::Failed;
    return ReadCutSourcesStatus(descriptor.initialVariables);
}

bool VariableServiceV1::FrontierEntryReadUseDetached(const ReadCutRow& cut,
    std::uint64_t completedAudioSequence) const noexcept {
    return cut.entryPool != nullptr && cut.entryDecisions != nullptr &&
        cut.entryDecisions->FrontierActivationBindingsDetached(cut.entryDescriptor.entry,
            *cut.entryPool, completedAudioSequence);
}

bool VariableServiceV1::ReleaseFrontierEntryReadUse(ReadCutRow& cut) noexcept {
    if (!cut.entryPin.Release()) return false;
    const auto ref = cut.entryDescriptor.initialVariables;
    cut.entryUse.store(EntryReadUseState::None, std::memory_order_release);
    cut.entryDescriptor = {};
    cut.entryDecisions = nullptr;
    cut.entryPool = nullptr;
    if (cut.timelineUseCount == 0 && cut.timelineOwnershipTransferred) return ReleaseReadCut(ref);
    return true;
}

bool VariableServiceV1::FinishFrontierEntryReadUse(const FrontierEntryReceiptRef& entry,
    std::uint64_t completedAudioSequence) noexcept {
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        if (cut.entryUse.load(std::memory_order_acquire) == EntryReadUseState::None ||
            !(cut.entryDescriptor.entry == entry)) continue;
        const auto* activation = cut.entryDecisions->ResolveFrontierActivation(entry.coordinator);
        FrontierActivationReceipt receipt{};
        // LogicFinal alone does not fence a controller already read by audio.
        if (activation == nullptr || !(activation->Descriptor() == cut.entryDescriptor) ||
            activation->Read(entry, completedAudioSequence, &receipt) != FrontierReceiptRead::Ready ||
            !FrontierEntryReadUseDetached(cut, completedAudioSequence) || !ReleaseFrontierEntryReadUse(cut)) return false;
    }
    return true;
}

bool VariableServiceV1::CancelPreparedFrontierEntryReadUse(const FrontierEntryReceiptRef& entry) noexcept {
    for (std::uint32_t index = 0; index < readCutCapacity_; ++index) {
        auto& cut = readCuts_[index];
        if (cut.entryUse.load(std::memory_order_acquire) == EntryReadUseState::None ||
            !(cut.entryDescriptor.entry == entry)) continue;
        const auto* activation = cut.entryDecisions->ResolveFrontierActivation(entry.coordinator);
        if (activation == nullptr || activation->State() != FrontierReceiptState::Prepared ||
            !(activation->Descriptor() == cut.entryDescriptor) || !FrontierEntryReadUseDetached(cut, 0) ||
            !ReleaseFrontierEntryReadUse(cut)) return false;
    }
    return true;
}

void VariableServiceV1::ReleaseTimelineReadUse(TimelineReadUseRow& use) noexcept {
    const auto ref = use.cut;
    auto* cut = ResolveReadCut(ref);
    assert(cut != nullptr && cut->timelineUseCount != 0);
    use.lifetime.Release();
    use.originalSource.Release();
    use.sourceOwner = nullptr;
    use.initializer.store(TimelineInitializerState::None, std::memory_order_relaxed);
    use.initialCommandCount = 0;
    use.cut = {};
    use.nativeHandle = 0;
    use.logicalFrame = 0;
    use.onsetFrame = 0;
    use.authoredStartOffset = 0;
    const auto generation = use.generation.load(std::memory_order_relaxed);
    if (generation == UINT32_MAX) {
        use.state.store(TimelineReadUseState::Quarantined, std::memory_order_release);
    } else {
        use.generation.store(generation + 1u, std::memory_order_release);
        use.state.store(TimelineReadUseState::Free, std::memory_order_release);
    }
    if (cut != nullptr && cut->timelineUseCount != 0) {
        --cut->timelineUseCount;
        if (cut->timelineUseCount == 0 && cut->timelineOwnershipTransferred) (void)ReleaseReadCut(ref);
    }
}

bool VariableServiceV1::CancelPreparedTimelineReadUse(VariableTimelineReadUseRef ref) noexcept {
    auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Prepared)
        return false;
    // The original command run is still private, even if its Arm was consumed.
    ReleaseTimelineReadUse(*use);
    return true;
}

VariableReadCutStatus VariableServiceV1::TimelineReadUseSourcesStatus(
    VariableTimelineReadUseRef ref, VnVoiceHandle nativeHandle) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Published ||
        use->nativeHandle != nativeHandle) return VariableReadCutStatus::Failed;
    return ReadCutSourcesStatus(use->cut);
}

VariableReadCutStatus VariableServiceV1::TimelineReadUseStartStatus(
    VariableTimelineReadUseRef ref, VnVoiceHandle nativeHandle) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Published ||
        use->nativeHandle != nativeHandle) return VariableReadCutStatus::Failed;
    const auto* cut = ResolveReadCut(use->cut);
    const auto initializer = use->initializer.load(std::memory_order_acquire);
    if (initializer == TimelineInitializerState::PendingPlacement || initializer == TimelineInitializerState::Building)
        return VariableReadCutStatus::Waiting;
    const auto* arm = use->lifetime.Owner()->Resolve(use->lifetime.Token());
    if (cut == nullptr || arm == nullptr) return VariableReadCutStatus::Failed;
    const auto state = arm->state.load(std::memory_order_acquire);
    if (state != ArmState::Published && state != ArmState::AudioClaimed)
        return state == ArmState::Armed || state == ArmState::PendingApply
            ? VariableReadCutStatus::Waiting : VariableReadCutStatus::Failed;
    if ((arm->pendingImmediateStart && !arm->startBound) ||
        arm->startFrame != use->onsetFrame)
        return VariableReadCutStatus::Failed;
    return ReadCutSourcesStatus(use->cut);
}

bool VariableServiceV1::PendingTimelineReadUseMatchesArm(VariableTimelineReadUseRef ref,
    VnVoiceHandle nativeHandle, TimelineArmToken arm) const noexcept {
    const auto* use = ResolveTimelineReadUse(ref);
    if (use == nullptr || use->state.load(std::memory_order_acquire) != TimelineReadUseState::Published ||
        use->nativeHandle != nativeHandle || !use->lifetime.Valid()) return false;
    const auto original = use->lifetime.Token();
    return original.slot == arm.slot && original.generation == arm.generation &&
        original.renderSlot == arm.renderSlot && original.renderGeneration == arm.renderGeneration &&
        TimelineReadUseStartStatus(ref, nativeHandle) == VariableReadCutStatus::Waiting &&
        TimelineReadUseSourcesStatus(ref, nativeHandle) != VariableReadCutStatus::Failed;
}

void VariableServiceV1::AcknowledgeFinishedTimelineReadUses(std::uint64_t completedAudioSequence) noexcept {
    for (std::uint32_t index = 0; index < timelineReadUseCapacity_; ++index) {
        auto& use = timelineReadUses_[index];
        if (use.state.load(std::memory_order_acquire) != TimelineReadUseState::Published) continue;
        TimelineArmObservation arm{};
        const bool observed = use.lifetime.Owner()->Observe(use.lifetime.Token(), &arm);
        assert(observed);
        if (!observed) continue;
        if ((arm.startedBlockSequence != 0 && completedAudioSequence > arm.startedBlockSequence) ||
            (arm.state == ArmState::Tombstoned && completedAudioSequence >
                arm.tombstoneAudioSequence)) ReleaseTimelineReadUse(use);
    }
}

void VariableServiceV1::ObserveAppliedWriteForReadCuts(const WriteRow& applied) noexcept {
    for (std::uint32_t slot = 0; slot < readCutCapacity_; ++slot) {
        auto& cut = readCuts_[slot];
        if (cut.state.load(std::memory_order_acquire) != ReadCutState::Capturing || cut.needsRebuild ||
            (cut.pendingPlacement && !cut.finalPlacementCaptured) ||
            cut.world != applied.world || cut.transport != applied.transport ||
            applied.requestedFrame > cut.executeFrame || applied.committedFrame > cut.executeFrame ||
            applied.ticket.sequence > cut.sequence ||
            (applied.ticket.sequence == cut.sequence && applied.executionOrdinal >= cut.executionOrdinalExclusive)) continue;
        if (applied.scope.scope == VariableScopeV1::Emitter &&
            (cut.emitter == 0 || applied.scope.emitterSlot != static_cast<std::uint32_t>(cut.emitter) ||
                applied.scope.emitterGeneration != static_cast<std::uint32_t>(cut.emitter >> 32))) continue;
        for (std::uint32_t index = 0; index < cut.valueCount; ++index) {
            const auto& value = cut.values[index];
            if (!value.read || value.variableId != applied.variableId) continue;
            const auto* original = value.source.Valid() ? ResolveHistory(value.source) : nullptr;
            if (original != nullptr && (&applied == original ||
                (applied.batchId != 0 && applied.batchId == original->batchId))) continue;
            if (original == nullptr) {
                if (applied.bankOrdinal > value.baselineOrdinal) cut.needsRebuild = true;
                continue;
            }
            const auto originalFrame = original->state == RowState::PageCommitted || original->state == RowState::Terminal
                ? original->committedFrame : original->requestedFrame;
            if (applied.committedFrame > originalFrame || (applied.committedFrame == originalFrame &&
                (applied.ticket.sequence > original->ticket.sequence ||
                    (applied.ticket.sequence == original->ticket.sequence && applied.executionOrdinal > original->executionOrdinal)))) {
                cut.needsRebuild = true;
            }
        }
    }
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
                row.outcomeDelivered = true;
            }
        }
        outcomes_.erase(
            outcomes_.begin(),
            outcomes_.begin() +
                static_cast<std::ptrdiff_t>(deliveredOutcomeCount_));
        deliveredOutcomeCount_ = 0;
        deliveredOutcomeGeneration_ = 0;
    }
    ReclaimHistoryRows();
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
    if (expiredTicketCount_ == queryHistoryCapacity_) {
        expiredTicketBegin_ =
            (expiredTicketBegin_ + 1u) % queryHistoryCapacity_;
        --expiredTicketCount_;
    }
    const std::uint32_t index =
        (expiredTicketBegin_ + expiredTicketCount_) % queryHistoryCapacity_;
    expiredTicketRecords_[index] = {record.sequence, record.world, record.transport};
    ++expiredTicketCount_;
}

bool VariableServiceV1::TicketExpired(
    const VnVariableTicketV1& ticket) const noexcept {
    for (std::uint32_t i = 0; i < expiredTicketCount_; ++i) {
        const std::uint32_t index =
            (expiredTicketBegin_ + i) % queryHistoryCapacity_;
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
            (ticketQueryBegin_ + i) % queryHistoryCapacity_;
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
    if (ticketQueryCount_ == queryHistoryCapacity_) {
        const TicketQueryRecord& old =
            ticketQueryRecords_[ticketQueryBegin_];
        PushExpiredTicket(old);
        ticketQueryBegin_ =
            (ticketQueryBegin_ + 1u) % queryHistoryCapacity_;
        --ticketQueryCount_;
    }
    const std::uint32_t index =
        (ticketQueryBegin_ + ticketQueryCount_) % queryHistoryCapacity_;
    TicketQueryRecord& row = ticketQueryRecords_[index];
    row.sequence = sequence;
    row.world = world;
    row.transport = transport;
    row.outcome = VariableOutcomeKind::Applied;
    row.pending = true;
    ++ticketQueryCount_;
}

void VariableServiceV1::RememberFinalizedTicket(
    LogicCommitSequence sequence, VariableOutcomeKind outcome,
    WorldGeneration world, TransportEpoch transport) noexcept {
    if (sequence == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < ticketQueryCount_; ++i) {
        const std::uint32_t index =
            (ticketQueryBegin_ + i) % queryHistoryCapacity_;
        TicketQueryRecord& row = ticketQueryRecords_[index];
        if (row.sequence == sequence && row.world == world && row.transport == transport) {
            row.pending = false;
            row.outcome = outcome;
            return;
        }
    }
    RememberAcceptedTicket(sequence, world, transport);
    const auto index = (ticketQueryBegin_ + ticketQueryCount_ - 1u) % queryHistoryCapacity_;
    ticketQueryRecords_[index].pending = false;
    ticketQueryRecords_[index].outcome = outcome;
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
