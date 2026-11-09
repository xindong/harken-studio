#include "Engine.h"

#include <algorithm>
#include <vector>

namespace {

using namespace harken;

VnPlaybackReadinessMemberV1 EmptyMember() noexcept {
    VnPlaybackReadinessMemberV1 value{};
    value.structSize = sizeof(value);
    value.scheduledFrame = UINT64_MAX;
    value.appliedFrame = UINT64_MAX;
    return value;
}

void CaptureStream(Engine& engine, std::uint64_t token, VnPlaybackReadinessMemberV1& value) {
    value.ringToken = token;
    StreamManager::PlaybackObservation stream{};
    if (!engine.streams.CapturePlayback(token, &stream)) {
        value.state = VN_PLAYBACK_READINESS_UNKNOWN;
        return;
    }
    value.sourceGeneration = stream.sourceGeneration;
    value.contentRevision = stream.contentRevision;
    value.preparationGeneration = stream.preparationGeneration;
    value.bufferedFrames = stream.bufferedFrames;
    value.prebufferTargetFrames = stream.prebufferTargetFrames;
    value.result = stream.result;
    value.terminalReason = static_cast<std::uint32_t>(stream.terminal);
    value.state = stream.state == StreamManager::PreparationState::Ready ? VN_PLAYBACK_READINESS_READY :
        stream.state == StreamManager::PreparationState::Preparing ? VN_PLAYBACK_READINESS_PREPARING :
        stream.state == StreamManager::PreparationState::Cancelled ? VN_PLAYBACK_READINESS_CANCELLED :
        stream.state == StreamManager::PreparationState::Failed ? VN_PLAYBACK_READINESS_FAILED : VN_PLAYBACK_READINESS_UNKNOWN;
    value.appliedFrame = stream.appliedFrame;
    value.appliedAudioBlock = stream.appliedAudioBlock;
    value.replacementSerial = stream.replacementSerial;
}

bool BelongsToFiring(const Engine& engine, VnActionFiringHandle ancestor, VnActionFiringHandle member) noexcept {
    while (member != 0) {
        if (member == ancestor) return true;
        ActionFiringSnapshot snapshot{};
        if (!engine.actionFirings.Capture(member, &snapshot)) return false;
        member = snapshot.parent;
    }
    return false;
}

std::uint32_t DeferredCount(const Engine& engine, VnActionFiringHandle firing) noexcept {
    ActionFiringSnapshot snapshot{};
    if (!engine.actionFirings.Capture(firing, &snapshot)) return 0;
    auto count = snapshot.ownedDeferredCount;
    for (std::uint32_t i = 0; i < snapshot.directStepCount; ++i) {
        ActionFiringStepResult step{};
        if (engine.actionFirings.CaptureStep(firing, i, &step) && step.childFiring != 0)
            count += DeferredCount(engine, step.childFiring);
    }
    return count;
}

const EnginePlayableServiceRow* FindOwner(Engine& engine,
    const EngineTimelineVoiceInstallIdentity& installation, VnActionFiringHandle firing) noexcept {
    const EnginePlayableServiceRow* candidate = nullptr;
    for (std::uint32_t cursor = 0; auto* row = engine.NextOccupiedPlayableService(&cursor);) {
        if (!row->InvocationValid() || row->rootInstance != installation.rootInstance ||
            !BelongsToFiring(engine, firing, row->Invocation().actionFiring)) continue;
        if (row->renderHandle == installation.renderHandle) return row;
        if (candidate == nullptr || row->ownerInstance == installation.rootInstance) candidate = row;
    }
    return candidate;
}

void CaptureMembers(Engine& engine, VnActionFiringHandle firing,
    std::vector<VnPlaybackReadinessMemberV1>& members) {
    for (const auto& installation : engine.timelineVoiceInstallIdentities) {
        if (installation.renderHandle == 0) continue;
        const auto* row = FindOwner(engine, installation, firing);
        if (row == nullptr) continue;
        auto value = EmptyMember();
        value.firing = row->Invocation().actionFiring;
        value.playableInstance = row->ownerInstance;
        value.renderActivation = installation.renderHandle;
        value.preparationGeneration = installation.startArm.generation;
        value.logicCommitSequence = row->ticket;
        value.scheduledFrame = installation.onsetFrame;
        value.soundId = installation.target.subjectId;
        value.clipId = installation.target.clipId;
        value.occurrenceOrdinal = installation.target.occurrenceOrdinal;
        value.loopIteration = installation.target.loopIteration;
        value.state = VN_PLAYBACK_READINESS_READY;
        TimelineArmObservation arm{};
        const bool observed = engine.timelineArms.Observe(installation.startArm, &arm);
        std::uint64_t ringToken = installation.stream.ringToken;
        if (installation.streamed) {
            // Capture the currently installed generation after a recovery/seek.
            (void)engine.voices.CaptureStreamRingToken(installation.renderHandle, &ringToken);
            CaptureStream(engine, ringToken, value);
        }
        if (observed && arm.startedBlockSequence != 0 && value.state != VN_PLAYBACK_READINESS_FAILED &&
            value.state != VN_PLAYBACK_READINESS_UNKNOWN && value.state != VN_PLAYBACK_READINESS_CANCELLED) {
            value.state = VN_PLAYBACK_READINESS_APPLIED;
            if (installation.streamed && ringToken != installation.stream.ringToken)
                value.scheduledFrame = value.appliedFrame;
            if (!installation.streamed || ringToken == installation.stream.ringToken) {
                value.appliedFrame = arm.actualStartFrame;
                value.appliedAudioBlock = arm.startedBlockSequence;
            }
        } else if (!observed) {
            value.state = VN_PLAYBACK_READINESS_UNKNOWN;
        } else if (arm.state == ArmState::Tombstoned) {
            value.state = VN_PLAYBACK_READINESS_CANCELLED;
        }
        if (row->controlLifecycle == PlayableControlLifecycle::Cancelling ||
            row->phase == EnginePlayableStartPhase::Cancelling)
            value.state = VN_PLAYBACK_READINESS_CANCELLED;
        members.push_back(value);

        const auto slot = static_cast<std::uint32_t>(installation.renderHandle);
        if (!installation.streamed || slot >= engine.streamResyncs.size()) continue;
        const auto& recovery = engine.streamResyncs[slot];
        if (recovery.group) {
            auto replacement = value;
            if (engine.CaptureGroupRecovery(installation.renderHandle, replacement))
                members.push_back(replacement);
            continue;
        }
        VoicePool::StreamResyncView resync{};
        if (recovery.handle != installation.renderHandle ||
            !engine.voices.ReadStreamResync(recovery.handle, &resync) || !resync.latched) continue;
        auto replacement = value;
        replacement.replacementSerial = resync.serial;
        replacement.preparationGeneration = 0;
        replacement.logicCommitSequence = 0;
        replacement.sourceGeneration = 0;
        replacement.result = VN_OK;
        replacement.terminalReason = 0;
        replacement.ringToken = 0;
        replacement.bufferedFrames = 0;
        replacement.appliedAudioBlock = 0;
        replacement.appliedFrame = UINT64_MAX;
        replacement.scheduledFrame = UINT64_MAX;
        replacement.state = VN_PLAYBACK_READINESS_UNKNOWN;
        StreamSeekHandoffSnapshot handoff{};
        StreamSeekHandoffLifecycle lifecycle{};
        if (recovery.servedSerial == resync.serial && recovery.token != 0 &&
            engine.streamSeekHandoffs.Capture(recovery.token, recovery.ticket, &handoff, &lifecycle)) {
            CaptureStream(engine, handoff.replacementRingToken, replacement);
            replacement.replacementSerial = resync.serial;
            replacement.scheduledFrame = recovery.scheduledFrame;
            if (lifecycle == StreamSeekHandoffLifecycle::Applied)
                replacement.state = VN_PLAYBACK_READINESS_APPLIED;
            else if (lifecycle == StreamSeekHandoffLifecycle::Cancelled)
                replacement.state = VN_PLAYBACK_READINESS_CANCELLED;
        }
        members.push_back(replacement);
    }
    // Preserve unknown semantics for accepted work not yet materialized. This
    // inspects existing owners only and never expands a random/queue frontier.
    for (std::uint32_t cursor = 0; auto* row = engine.NextOccupiedPlayableService(&cursor);) {
        if (!row->InvocationValid() || row->phase == EnginePlayableStartPhase::Started ||
            !BelongsToFiring(engine, firing, row->Invocation().actionFiring)) continue;
        const bool represented = std::any_of(engine.timelineVoiceInstallIdentities.begin(),
            engine.timelineVoiceInstallIdentities.end(), [&](const auto& identity) {
                return identity.renderHandle != 0 && identity.rootInstance == row->rootInstance;
            });
        if (represented) continue;
        auto value = EmptyMember();
        value.firing = row->Invocation().actionFiring;
        value.playableInstance = row->ownerInstance;
        value.preparationGeneration = row->identity.generation;
        value.logicCommitSequence = row->ticket;
        value.state = row->phase == EnginePlayableStartPhase::Cancelling
            ? VN_PLAYBACK_READINESS_CANCELLED : VN_PLAYBACK_READINESS_UNKNOWN;
        members.push_back(value);
    }
}

} // namespace

VnResult Vn_QueryActionPlaybackReadiness(VnActionFiringHandle firing,
    VnActionPlaybackReadinessV1* outInfo, VnPlaybackReadinessMemberV1* output, uint32_t capacity) {
    if (outInfo == nullptr || outInfo->structSize != sizeof(*outInfo) ||
        (capacity != 0 && output == nullptr)) return VN_INVALID_ARG;
    *outInfo = {};
    outInfo->structSize = sizeof(*outInfo);
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr || engine->callbackFatalSticky) return VN_NOT_INITIALIZED;
    try {
        harken::ActionFiringSnapshot firingInfo{};
        if (!engine->actionFirings.Capture(firing, &firingInfo)) return VN_STALE_HANDLE;
        outInfo->engineInstance = engine->monitorRequests.EngineInstance();
        outInfo->worldGeneration = firingInfo.worldGeneration;
        outInfo->cancellationEpoch = firingInfo.cancellationEpoch;
        outInfo->acceptedLogicSequence = firingInfo.acceptedLogicSequence;
        outInfo->firingOutcome = static_cast<std::uint32_t>(firingInfo.outcome);
        outInfo->terminalReason = static_cast<std::uint32_t>(firingInfo.terminalReason);
        if (!engine->actionFirings.TreeOwnedDeferredCount(firing, &outInfo->deferredCount))
            outInfo->deferredCount = DeferredCount(*engine, firing);
        std::vector<VnPlaybackReadinessMemberV1> members;
        CaptureMembers(*engine, firing, members);
        outInfo->memberCount = static_cast<std::uint32_t>(members.size());
        for (const auto& member : members) {
            switch (member.state) {
            case VN_PLAYBACK_READINESS_PREPARING: ++outInfo->pendingCount; break;
            case VN_PLAYBACK_READINESS_READY: ++outInfo->readyCount; break;
            case VN_PLAYBACK_READINESS_APPLIED: ++outInfo->appliedCount; break;
            case VN_PLAYBACK_READINESS_FAILED: ++outInfo->failedCount; break;
            case VN_PLAYBACK_READINESS_CANCELLED: ++outInfo->cancelledCount; break;
            default: ++outInfo->unknownCount; break;
            }
        }
        outInfo->state = outInfo->failedCount != 0 ? VN_PLAYBACK_READINESS_FAILED :
            outInfo->cancelledCount != 0 ? VN_PLAYBACK_READINESS_CANCELLED :
            outInfo->pendingCount != 0 || outInfo->unknownCount != 0 || outInfo->deferredCount != 0 ? VN_PLAYBACK_READINESS_PREPARING :
            members.empty() ? VN_PLAYBACK_READINESS_UNKNOWN :
            outInfo->appliedCount == members.size() ? VN_PLAYBACK_READINESS_APPLIED : VN_PLAYBACK_READINESS_READY;
        if (firingInfo.state == harken::ActionFiringState::Stopping)
            outInfo->state = VN_PLAYBACK_READINESS_CANCELLED;
        if (firingInfo.state == harken::ActionFiringState::TerminalRetained)
            outInfo->state = firingInfo.outcome == harken::ActionFiringOutcome::Cancelled ? VN_PLAYBACK_READINESS_CANCELLED :
                firingInfo.outcome == harken::ActionFiringOutcome::Failed ||
                firingInfo.outcome == harken::ActionFiringOutcome::SucceededWithStepFailure
                ? VN_PLAYBACK_READINESS_FAILED : VN_PLAYBACK_READINESS_TERMINAL;
        if (capacity == 0) return VN_OK;
        if (capacity < members.size()) return VN_INVALID_ARG;
        std::copy(members.begin(), members.end(), output);
        return VN_OK;
    } catch (...) {
        *outInfo = {};
        outInfo->structSize = sizeof(*outInfo);
        return VN_DEVICE_ERROR;
    }
}
