#ifndef HARKEN_API_MONITOR_LIFECYCLE_PROJECTION_H
#define HARKEN_API_MONITOR_LIFECYCLE_PROJECTION_H

#include "MonitorExecutionBuilder.h"
#include "harken/core/MonitorLifecycleSink.h"

namespace harken::monitor {

constexpr unsigned LifecycleFactKind(MonitorLifecyclePhase phase) noexcept {
    switch (phase) {
    case MonitorLifecyclePhase::PreparedCandidate: return VN_MONITOR_FACT_PLAYABLE_PREPARED;
    case MonitorLifecyclePhase::PlayableStart: return VN_MONITOR_FACT_PLAYABLE_ACTIVE;
    case MonitorLifecyclePhase::PlayableStarted: return VN_MONITOR_FACT_PLAYABLE_STARTED;
    case MonitorLifecyclePhase::PlayableFinished: return VN_MONITOR_FACT_PLAYABLE_FINISHED;
    case MonitorLifecyclePhase::PlayableLogicalComplete: return VN_MONITOR_FACT_PLAYABLE_LOGICAL_COMPLETE;
    case MonitorLifecyclePhase::PlayableAudibleRetiring: return VN_MONITOR_FACT_PLAYABLE_AUDIBLE_RETIRING;
    case MonitorLifecyclePhase::PlayableRetired: return VN_MONITOR_FACT_PLAYABLE_RETIRED;
    case MonitorLifecyclePhase::PlayableCancel: return VN_MONITOR_FACT_PLAYABLE_CANCELED;
    case MonitorLifecyclePhase::ActionAccept: return VN_MONITOR_FACT_ACTION_ACCEPTED;
    case MonitorLifecyclePhase::ActionStop: return VN_MONITOR_FACT_ACTION_STOPPED;
    case MonitorLifecyclePhase::ActionResult: return VN_MONITOR_FACT_ACTION_RESULT;
    case MonitorLifecyclePhase::ActionReject: return VN_MONITOR_FACT_ACTION_REJECTED;
    case MonitorLifecyclePhase::StepExecuted: return VN_MONITOR_FACT_STEP_EXECUTED;
    case MonitorLifecyclePhase::ActionTerminal: return VN_MONITOR_FACT_ACTION_TERMINAL;
    case MonitorLifecyclePhase::VoicePending: return VN_MONITOR_FACT_VOICE_PENDING;
    case MonitorLifecyclePhase::VoiceLive: return VN_MONITOR_FACT_VOICE_LIVE;
    case MonitorLifecyclePhase::VoicePark: return VN_MONITOR_FACT_VOICE_PARKED;
    case MonitorLifecyclePhase::VoiceEnd: return VN_MONITOR_FACT_VOICE_ENDED;
    case MonitorLifecyclePhase::SelectionCommitted: return VN_MONITOR_FACT_SELECTION_COMMITTED;
    case MonitorLifecyclePhase::QueueAdvance: return VN_MONITOR_FACT_QUEUE_ADVANCED;
    case MonitorLifecyclePhase::QueueFail: return VN_MONITOR_FACT_QUEUE_FAILED;
    case MonitorLifecyclePhase::TransitionStart: return VN_MONITOR_FACT_TRANSITION_STARTED;
    case MonitorLifecyclePhase::TransitionStop: return VN_MONITOR_FACT_TRANSITION_STOPPED;
    case MonitorLifecyclePhase::TransitionReject: return VN_MONITOR_FACT_TRANSITION_REJECTED;
    default: return 0;
    }
}

constexpr bool ExecutionReplacesLegacyEvent(unsigned category, unsigned code) noexcept {
    if (category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE)
        return LifecycleFactKind(static_cast<MonitorLifecyclePhase>(code)) != 0;
    if (category != VN_MONITOR_EVENT_CATEGORY_OPERATION) return false;
    switch (code) {
    case VN_MONITOR_OPERATION_POST_ACTION:
    case VN_MONITOR_OPERATION_POST_ACTION_BY_NAME:
    case VN_MONITOR_OPERATION_POST_ACTION_PINNED:
    case VN_MONITOR_OPERATION_POST_ACTION_RESOLVED:
    case VN_MONITOR_OPERATION_STOP_VOICE:
    case VN_MONITOR_OPERATION_STOP_ACTION:
    case VN_MONITOR_OPERATION_STOP_ALL:
    case VN_MONITOR_OPERATION_STOP_EMITTER:
    case VN_MONITOR_OPERATION_PLAY_PREVIEW:
    case VN_MONITOR_OPERATION_STOP_PREVIEW:
        return true;
    default: return false;
    }
}

inline MonitorExecutionFact CaptureEntityFact(unsigned kind, unsigned subject,
    std::uint64_t engine, std::uint64_t handle, std::uint64_t generation,
    std::uint64_t world, VnObjectId object, std::uint64_t action,
    std::uint32_t reason, std::uint8_t playableKind,
    const MonitorLifecycleFacts& source, const MonitorEmitterBinding& binding) noexcept {
    MonitorExecutionFact fact{};
    fact.engine = engine;
    fact.kind = static_cast<std::uint8_t>(kind);
    fact.subjectNamespace = static_cast<std::uint8_t>(subject);
    const bool actionSubject = subject == VN_MONITOR_SUBJECT_ACTION;
    const bool voiceSubject = subject == VN_MONITOR_SUBJECT_LOGICAL_VOICE;
    const bool queueSubject = subject == VN_MONITOR_SUBJECT_QUEUE;
    const bool transitionSubject = subject == VN_MONITOR_SUBJECT_TRANSITION;
    auto& body = fact.body.execution;
    if (ValidSubjectHandle(fact.subjectNamespace, handle) && SubjectGeneration(fact.subjectNamespace, handle) == generation) {
        body.identity.handle = handle;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_SUBJECT);
    } else {
        fact.subjectNamespace = VN_MONITOR_SUBJECT_NONE;
        fact.flags |= kMonitorFactIdentityIncomplete;
    }
    if (voiceSubject || transitionSubject) { world = source.contentWorld; object = source.contentObject; playableKind = source.contentKind; }
    if (queueSubject) playableKind = VN_MONITOR_CONTENT_CONTAINER;
    fact.contentKind = actionSubject ? VN_MONITOR_CONTENT_ACTION : playableKind;
    body.identity.project = source.projectId;
    body.identity.object = object;
    body.identity.world = world;
    body.identity.semantic = source.programSemanticFingerprint;
    body.identity.lineage = source.runtimeLineageFingerprint;
    body.identity.revision = source.contentRevision;
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_PROJECT, source.projectId != VnObjectId{});
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, object != VnObjectId{});
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_WORLD, world != 0);
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_SEMANTIC, source.programSemanticFingerprint != 0);
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_LINEAGE, source.runtimeLineageFingerprint != 0);
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_CONTENT_REVISION, source.contentRevision != 0);
    CaptureExecutionRequests(fact, source.playbackRequest, source.controlCause, source.selectionCause);
    CaptureFactEmitters(fact, source.emitterOrigin, binding);
    const auto relation = [&](unsigned field, std::uint64_t value, unsigned kind, std::uint64_t& out) {
        if (ValidSubjectHandle(kind, value)) { out = value; SetSourceField(fact, field); }
        else if (value != 0) fact.flags |= kMonitorFactIdentityIncomplete;
    };
    relation(actionSubject ? VN_MONITOR_FACT_FIELD_PARENT_ACTION : VN_MONITOR_FACT_FIELD_PARENT_PLAYABLE,
        actionSubject ? source.parentAction : source.parentPlayable,
        actionSubject ? VN_MONITOR_SUBJECT_ACTION : VN_MONITOR_SUBJECT_PLAYABLE, body.relations.parent);
    relation(VN_MONITOR_FACT_FIELD_ROOT, source.root,
        actionSubject ? VN_MONITOR_SUBJECT_ACTION : VN_MONITOR_SUBJECT_PLAYABLE, body.relations.root);
    relation(VN_MONITOR_FACT_FIELD_RELATED_ACTION, source.relatedAction != 0 ? source.relatedAction : action,
        VN_MONITOR_SUBJECT_ACTION, body.relations.action);
    relation(VN_MONITOR_FACT_FIELD_SERVICE_LEAF, source.serviceLeaf, VN_MONITOR_SUBJECT_PLAYABLE, body.relations.leaf);
    relation(VN_MONITOR_FACT_FIELD_RESOURCE_OWNER, source.resourceOwner, VN_MONITOR_SUBJECT_PLAYABLE, body.relations.owner);
    relation(VN_MONITOR_FACT_FIELD_LOGICAL_VOICE, source.logicalVoice, VN_MONITOR_SUBJECT_LOGICAL_VOICE, body.relations.logicalVoice);
    relation(VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION, source.renderActivation, VN_MONITOR_SUBJECT_RENDER_ACTIVATION, body.relations.renderActivation);
    body.position.logicCommit = source.logicCommitSequence;
    body.position.arranging = source.arrangingInstance;
    body.position.detail.ownerEpoch = source.ownerEpoch;
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_LOGIC_COMMIT, source.logicCommitSequence != 0);
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_ARRANGING, source.arrangingInstance != 0);
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_OWNER_EPOCH, source.ownerEpoch != 0);
    if (source.audioBlock != 0) {
        body.position.audioBlock = source.audioBlock;
        body.position.frame = source.executeFrame;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_AUDIO_BLOCK);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_EXECUTE_FRAME);
    }
    const auto& occurrence = source.occurrenceIdentity;
    if (occurrence.valid) {
        body.position.clip = occurrence.clipId;
        body.position.loop = occurrence.loopIteration;
        body.position.occurrence = occurrence.occurrenceOrdinal;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_CLIP, occurrence.clipId != VnObjectId{});
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_LOOP, occurrence.loopKnown);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_OCCURRENCE);
        if (occurrence.arrangingInstance != 0) {
            body.position.arranging = occurrence.arrangingInstance;
            SetSourceField(fact, VN_MONITOR_FACT_FIELD_ARRANGING);
        }
    }
    fact.reason = source.reasonKnown ? reason : 0;
    SetSourceField(fact, VN_MONITOR_FACT_FIELD_REASON, source.reasonKnown);
    if (source.terminalKind != 0) {
        fact.terminalKind = source.terminalKind;
        fact.result = source.terminalResult;
        fact.reason = source.terminalReason;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_RESULT);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_REASON);
    }
    if (source.controlObservationIncomplete) fact.flags |= kMonitorFactControlIncomplete;
    if (source.identityObservationIncomplete) fact.flags |= kMonitorFactIdentityIncomplete;
    return fact;
}

inline MonitorExecutionFact CaptureLifecycleFact(MonitorLifecyclePhase phase,
    std::uint64_t engine, std::uint64_t handle, std::uint64_t generation,
    std::uint64_t world, VnObjectId object, std::uint64_t actionHandle,
    std::uint32_t reason, std::uint8_t playableKind,
    const MonitorLifecycleFacts& source, const MonitorEmitterBinding& binding) noexcept {
    const bool action = (phase >= MonitorLifecyclePhase::ActionAccept && phase <= MonitorLifecyclePhase::ActionReject) ||
        phase == MonitorLifecyclePhase::StepExecuted || phase == MonitorLifecyclePhase::ActionTerminal;
    const bool voice = (phase >= MonitorLifecyclePhase::VoicePending && phase <= MonitorLifecyclePhase::VoiceEnd) ||
        phase == MonitorLifecyclePhase::SelectionCommitted;
    const bool queue = phase == MonitorLifecyclePhase::QueueAdvance || phase == MonitorLifecyclePhase::QueueFail;
    const bool transition = phase >= MonitorLifecyclePhase::TransitionStart && phase <= MonitorLifecyclePhase::TransitionReject;
    const auto subject = action ? VN_MONITOR_SUBJECT_ACTION : voice ? VN_MONITOR_SUBJECT_LOGICAL_VOICE :
        queue ? VN_MONITOR_SUBJECT_QUEUE : transition ? VN_MONITOR_SUBJECT_TRANSITION : VN_MONITOR_SUBJECT_PLAYABLE;
    auto fact = CaptureEntityFact(LifecycleFactKind(phase), subject, engine, handle, generation,
        world, object, actionHandle, reason, playableKind, source, binding);
    if ((phase == MonitorLifecyclePhase::ActionResult || phase == MonitorLifecyclePhase::ActionReject) && source.actionOutcome != 0) {
        fact.terminalKind = 0;
        fact.result = 0;
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND, false);
        SetSourceField(fact, VN_MONITOR_FACT_FIELD_RESULT, false);
    }
    if (IsActivityFactKind(fact.kind)) {
        auto& activity = fact.body.execution.activity;
        activity = {};
        activity.logicCommit = source.logicCommitSequence;
        if (phase == MonitorLifecyclePhase::StepExecuted) {
            activity.stepIndex = source.stepIndex;
            activity.stepExecutionOrdinal = source.stepExecutionOrdinal;
            activity.stepOpcode = source.stepOpcode;
            activity.stepTargetKind = source.stepTargetKind;
            activity.stepTargetId = source.stepTargetId;
            fact.result = source.terminalResult;
            SetSourceField(fact, VN_MONITOR_FACT_FIELD_RESULT);
            SetSourceField(fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND, false);
            fact.terminalKind = 0;
            SetSourceField(fact, VN_MONITOR_FACT_FIELD_ACTIVITY_STEP);
        } else {
            activity.actionOutcome = source.actionOutcome;
            SetSourceField(fact, VN_MONITOR_FACT_FIELD_ACTION_OUTCOME);
        }
    }
    return fact;
}

} // namespace harken::monitor

#endif
