#ifndef HARKEN_API_MONITOR_IDENTITY_SNAPSHOT_H
#define HARKEN_API_MONITOR_IDENTITY_SNAPSHOT_H

#include "Engine.h"
#include "MonitorLifecycleProjection.h"

namespace harken::monitor {

// Stable diagnostic slot order. These are value copies, not native owners.
inline std::size_t IdentitySlotCapacity(const Engine& engine) noexcept {
    return static_cast<std::size_t>(engine.playableInstances.Capacity()) + engine.actionFirings.Capacity() +
        engine.ledger.Capacity() + engine.monitorEmitters.Capacity() + 1;
}

inline bool CaptureIdentitySlot(const Engine& engine, std::size_t slot,
    MonitorExecutionFact& out) noexcept {
    out = {};
    MonitorLifecycleFacts values{};
    values.reasonKnown = false;
    std::uint64_t handle = 0;
    unsigned subject = 0;
    if (slot < engine.playableInstances.Capacity()) {
        handle = engine.playableInstances.CaptureMonitorIdentityAt(static_cast<std::uint32_t>(slot), &values);
        subject = VN_MONITOR_SUBJECT_PLAYABLE;
    } else if ((slot -= engine.playableInstances.Capacity()) < engine.actionFirings.Capacity()) {
        ActionFiringSnapshot action{};
        handle = engine.actionFirings.CaptureMonitorIdentityAt(static_cast<std::uint32_t>(slot), &action);
        subject = VN_MONITOR_SUBJECT_ACTION;
        values.playbackRequest = action.playbackRequest;
        values.controlCause = action.controlCause;
        values.emitterOrigin = action.emitterOrigin;
        values.parentAction = action.parent;
        values.root = action.root;
        values.relatedAction = handle;
        values.contentObject = action.actionId;
        values.contentWorld = action.worldGeneration;
        values.contentKind = VN_MONITOR_CONTENT_ACTION;
        values.programSemanticFingerprint = action.programSemanticFingerprint;
        values.projectId = action.owner.projectId;
        values.logicCommitSequence = action.acceptedLogicSequence;
    } else if ((slot -= engine.actionFirings.Capacity()) < engine.ledger.Capacity()) {
        handle = engine.ledger.CaptureMonitorIdentityAt(static_cast<std::uint32_t>(slot), &values);
        subject = VN_MONITOR_SUBJECT_LOGICAL_VOICE;
    } else {
        slot -= engine.ledger.Capacity();
        subject = VN_MONITOR_SUBJECT_EMITTER;
        if (slot < engine.monitorEmitters.Capacity()) {
            if (!engine.monitorEmitters.CaptureAt(static_cast<std::uint32_t>(slot), &values.emitterOrigin)) return false;
        } else if (slot == engine.monitorEmitters.Capacity()) {
            values.emitterOrigin = engine.monitorEmitters.CaptureOrigin(engine.emitters.DefaultState());
        } else return false;
        handle = values.emitterOrigin.emitter;
    }
    if (handle == 0) return false;
    const auto context = engine.emitters.Context(values.emitterOrigin.emitter);
    const auto binding = engine.monitorEmitters.CaptureBinding(values.emitterOrigin,
        context.stateEmitter, context.spatialEmitter);
    out = CaptureEntityFact(VN_MONITOR_FACT_IDENTITY_UPSERT, subject,
        engine.monitorRequests.EngineInstance(), handle, SubjectGeneration(subject, handle),
        values.contentWorld, values.contentObject, values.relatedAction, 0, values.contentKind, values, binding);
    return true;
}

} // namespace harken::monitor

#endif
