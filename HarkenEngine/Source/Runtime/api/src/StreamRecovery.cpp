#include "Engine.h"
#include "harken/core/ResamplerAdvance.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace harken {

struct EngineStreamRecoveryBinding {
    Engine* engine = nullptr;
    std::shared_ptr<EngineStreamRecoveryState> group;
    Engine::OriginalPlayableServiceLifetimePin owner;
    VnVoiceHandle render = 0;
    VoiceReleaseObserver release{};
    StreamManager::ReplacementRequest source{};
    std::uint64_t frameCount = 0;
    bool streamed = false;
    ~EngineStreamRecoveryBinding() {
        if (source.recoveryReservation) engine->streams.ReleaseRecoverySlot(source.recoveryReservation);
    }
};

struct EngineStreamRecoveryBatch {
    Engine* engine = nullptr;
    Engine::OriginalPlayableServiceLifetimePin owner;
    std::vector<std::shared_ptr<EngineStreamRecoveryBinding>> retained;
    std::vector<StreamRecoveryMember> members;
    std::vector<StreamManager::PreparedStream> streams;
    StreamRecoveryAttempt render;
    std::uint64_t ticket = 0;
    std::uint64_t capturedFrame = 0;
    std::uint64_t plannedFrame = 0;
    ~EngineStreamRecoveryBatch() {
        assert(ticket == 0);
        if (render.result != StreamRecoveryResult::Applied)
            for (const auto& stream : streams)
                (void)engine->streams.RetirePreparedSlot(stream.ring, stream.ringToken);
    }
};

struct EngineStreamRecoveryState {
    PlayableInstanceHandle instance = 0;
    std::uint64_t epoch = 0;
    StreamRecoveryGroup render;
    std::unique_ptr<EngineStreamRecoveryBatch> pending;
    std::uint64_t servedSerial = 0;
    std::uint64_t leadFrames = 0;
};

VnResult Engine::BindGroupRecoverySource(EnginePlayableServiceRow& service, PlayableInstanceHandle leaf, VnVoiceHandle render,
    VoiceReleaseObserver release, const TimelineVoiceInstallFacts& install,
    const StreamManager::StreamFacts& facts, std::uint64_t loopStart, std::uint64_t loopEnd) {
    EnginePlayableServiceRow* groupOwner = nullptr;
    PlayableInstanceHandle groupInstance = 0;
    std::uint64_t groupEpoch = 0;
    auto ancestor = leaf;
    auto epoch = playableInstances.SourceRetirementEpoch(leaf);
    for (std::size_t depth = 0; ancestor; ++depth) {
        if (depth >= playableInstances.Capacity()) return VN_NO_VOICE;
        PlayableKind kind{};
        PlayableInstanceHandle parent = 0;
        std::uint64_t parentEpoch = 0;
        if (!playableInstances.CaptureOwnerAncestry(ancestor, epoch, &kind, &parent, &parentEpoch))
            return VN_NO_VOICE;
        EnginePlayableServiceRow* owner = nullptr;
        for (auto& candidate : playableServices)
            if (candidate.ownerInstance == ancestor && candidate.ownerEpoch == epoch) {
                if (candidate.invocation != service.invocation ||
                    candidate.invocationGeneration != service.invocationGeneration) return VN_NO_VOICE;
                owner = &candidate;
                break;
            }
        if (kind == PlayableKind::Track || kind == PlayableKind::Segment ||
                (kind == PlayableKind::Container && owner && owner->variableControlInstance.Valid())) {
            groupInstance = ancestor;
            groupEpoch = epoch;
            groupOwner = owner;
        }
        if (groupInstance && !groupOwner && owner) groupOwner = owner;
        ancestor = parent;
        epoch = parentEpoch;
    }
    if (!groupInstance) return VN_OK;
    if (!groupOwner) return VN_NO_VOICE;
    const auto index = static_cast<std::uint32_t>(render);
    if (index >= streamResyncs.size() || groupOwner->identity.slot >= originalPlayableServiceLifetimeHolders.size())
        return VN_NO_VOICE;
    // One service can own several Track occurrences, including overlapping
    // Queue predecessor and successor trees. Their recovery clocks stay separate.
    auto found = std::find_if(groupOwner->streamRecoveries.begin(), groupOwner->streamRecoveries.end(),
        [=](const auto& group) { return group->instance == groupInstance && group->epoch == groupEpoch; });
    if (found == groupOwner->streamRecoveries.end()) {
        auto group = std::make_shared<EngineStreamRecoveryState>();
        group->instance = groupInstance;
        group->epoch = groupEpoch;
        groupOwner->streamRecoveries.push_back(std::move(group));
        found = std::prev(groupOwner->streamRecoveries.end());
    }
    auto binding = std::make_shared<EngineStreamRecoveryBinding>();
    binding->engine = this;
    binding->group = *found;
    binding->render = render;
    binding->release = release;
    if (!originalPlayableServiceLifetimeHolders[groupOwner->identity.slot].Borrow(&binding->owner))
        return VN_NO_VOICE;
    binding->streamed = install.streamed;
    if (binding->streamed) {
        binding->frameCount = facts.frameCount;
        auto& source = binding->source;
        source.fileIndex = install.streamFileIndex;
        source.sourceGeneration = install.streamSourceGeneration;
        source.contentRevision = install.streamContentRevision;
        source.loopStartFrame = loopStart;
        source.loopEndFrame = loopEnd;
        source.loopCount = install.loopCount;
        source.prebufferTargetFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX,
            (static_cast<std::uint64_t>(config.prebufferMs) * facts.sampleRate + 999u) / 1000u));
        std::uint64_t token = 0;
        if (!voices.CaptureStreamRingToken(render, &token)) return VN_NO_STREAM;
        const auto reserved = streams.ReserveRecoverySlot(token, pendingStreamReservations, &source.recoveryReservation);
        if (reserved != VN_OK) return reserved == VN_BACKPRESSURE ? VN_NO_STREAM : reserved;
    }
    if (!voices.BindStreamRecoveryGroup(render, &binding->group->render)) return VN_NO_VOICE;
    auto& row = streamResyncs[index];
    if (row.token) (void)streamSeekHandoffs.Abandon(row.token);
    if (row.ticket) streamSeekHandoffs.ReleaseTicket(row.ticket);
    row = {};
    row.handle = render;
    row.frameCount = binding->frameCount;
    row.group = std::move(binding);
    return VN_OK;
}

void Engine::ReleaseGroupRecoverySource(VnVoiceHandle render) noexcept {
    const auto index = static_cast<std::uint32_t>(render);
    if (index >= streamResyncs.size()) return;
    auto& row = streamResyncs[index];
    if (row.handle == render && row.group && voices.ReleaseObserved(row.group->release)) row = {};
}

void Engine::ServiceGroupRecoveries(std::uint64_t preparationHorizon) {
    // A completed source can leave its index row. An outstanding batch keeps
    // its own binding references until audio acknowledges all member uses.
    for (auto& row : streamResyncs)
        if (row.group && voices.ReleaseObserved(row.group->release)) row = {};
    for (auto& service : playableServices) {
        bool stopOwner = false;
        std::erase_if(service.streamRecoveries, [](const auto& group) {
            return group.use_count() == 1 && !group->pending;
        });
        for (const auto& retainedGroup : service.streamRecoveries) {
            auto& group = *retainedGroup;
            const auto failure = group.render.failure.load(std::memory_order_acquire);
            if (failure != SourceTerminalKind::None && service.controlReason != PlayableControlReason::StopRequested &&
                service.controlLifecycle != PlayableControlLifecycle::Cancelling &&
                service.phase != EnginePlayableStartPhase::Cancelling) {
                service.sourceCohortFailureOwner = {};
                service.controlReason = PlayableControlReason::StartRefused;
                service.controlLifecycle = PlayableControlLifecycle::FailingSourceActive;
                if (service.Invocation().actionTerminalReason == ActionTerminalReason::None) {
                    service.Invocation().actionTerminalResult = failure == SourceTerminalKind::IntegrityAbort
                        ? VN_BAD_STREAM_FILE : VN_RUNTIME_RECREATE_REQUIRED;
                    service.Invocation().actionTerminalReason = failure == SourceTerminalKind::IntegrityAbort
                        ? ActionTerminalReason::MediaNotReady : ActionTerminalReason::RuntimeError;
                }
                stopOwner = true;
            }
            if (group.pending) {
                auto& batch = *group.pending;
                if (stopOwner || service.controlLifecycle == PlayableControlLifecycle::Cancelling ||
                    service.phase == EnginePlayableStartPhase::Cancelling)
                    batch.render.cancelRequested.store(true, std::memory_order_release);
                if (!voices.CollectGroupRecovery(batch.ticket, &batch.render)) continue;
                batch.ticket = 0;
                if (batch.render.result == StreamRecoveryResult::Applied || batch.render.result == StreamRecoveryResult::Failed)
                    group.servedSerial = batch.render.serial;
                if (batch.render.result == StreamRecoveryResult::Retry) {
                    const auto now = mixer.BlocksRendered() * config.blockFrames;
                    const auto elapsed = now >= batch.capturedFrame ? now - batch.capturedFrame : 0;
                    if (elapsed <= UINT64_MAX - 2ull * config.blockFrames)
                        group.leadFrames = std::max(group.leadFrames, elapsed + 2ull * config.blockFrames);
                }
                group.pending.reset();
            }
            const auto serial = group.render.recoverySerial.load(std::memory_order_acquire);
            if (stopOwner || failure != SourceTerminalKind::None || !serial || serial == group.servedSerial || preparationHorizon == UINT64_MAX ||
                service.controlLifecycle == PlayableControlLifecycle::Cancelling ||
                service.phase == EnginePlayableStartPhase::Cancelling) continue;
            std::vector<std::shared_ptr<EngineStreamRecoveryBinding>> bindings;
            std::vector<std::uint32_t> indices;
            for (const auto& row : streamResyncs) if (row.group && row.group->group.get() == &group) {
                bindings.push_back(row.group);
                indices.push_back(static_cast<std::uint32_t>(row.handle));
            }
            if (bindings.empty()) continue;
            std::vector<SourceCursorMirror::Row> cursors(indices.size());
            SourceCursorMirror::Publication publication{};
            if (!sourceCursors.CaptureRows(indices, cursors, &publication)) continue;
            if (publication.outputFrame > UINT64_MAX - group.leadFrames) continue;
            auto horizon = std::max(preparationHorizon, publication.outputFrame + group.leadFrames);
            const auto block = static_cast<std::uint64_t>(config.blockFrames);
            if (horizon > UINT64_MAX - (block - 1u)) continue;
            horizon = ((horizon + block - 1u) / block) * block;
            if (horizon < publication.outputFrame) continue;
            auto batch = std::make_unique<EngineStreamRecoveryBatch>();
            batch->engine = this;
            batch->capturedFrame = publication.outputFrame;
            batch->plannedFrame = horizon;
            batch->render.group = &group.render;
            batch->render.serial = serial;
            batch->retained = bindings;
            batch->members.reserve(bindings.size());
            batch->streams.reserve(bindings.size());
            if (!bindings.front()->owner.Borrow(&batch->owner)) continue;
            VnVoiceHandle representative = 0;
            bool prepared = true;
            for (std::size_t i = 0; i < bindings.size(); ++i) {
                const auto& binding = *bindings[i];
                const auto& cursor = cursors[i];
                if (!cursor.active || cursor.awaitingStart || !cursor.groupTimingValid ||
                    cursor.renderGeneration != binding.render >> 32u) continue;
                if (!representative) representative = binding.render;
                if (!binding.streamed) continue;
                const auto& progress = cursor.progress;
                ResamplerAdvanceState position;
                position.pcmFrameCount = binding.frameCount;
                position.loopStartFrame = binding.source.loopStartFrame;
                position.loopEndFrame = binding.source.loopEndFrame;
                position.loopInfinite = progress.activationLoopsRemaining == 0;
                SetResamplerConsumedPosition(&position, progress.sourceStartFrame, progress.activationLoopsRemaining,
                    progress.sourceReadFrame > kResampleHistoryPad ? progress.sourceReadFrame - kResampleHistoryPad : 0);
                position.sourcePhase = progress.sourcePhase;
                position.currentRatio = progress.currentRatio;
                position.targetRatio = progress.targetRatio;
                position.rampStep = progress.rampStep;
                position.rampRemaining = progress.rampRemaining;
                const auto before = position;
                if (!AdvanceResamplerPosition(&position, horizon - publication.outputFrame) ||
                    position.sourceIntegerFrame >= binding.frameCount) { prepared = false; break; }
                const auto passes = position.completedLoopPasses - before.completedLoopPasses;
                const auto span = position.loopEndFrame - position.loopStartFrame;
                if (passes && span > UINT64_MAX / passes) { prepared = false; break; }
                auto delta = passes * span;
                if (position.sourceIntegerFrame >= before.sourceIntegerFrame) {
                    const auto extra = position.sourceIntegerFrame - before.sourceIntegerFrame;
                    if (delta > UINT64_MAX - extra) { prepared = false; break; }
                    delta += extra;
                } else {
                    const auto wrapped = before.sourceIntegerFrame - position.sourceIntegerFrame;
                    if (delta < wrapped) { prepared = false; break; }
                    delta -= wrapped;
                }
                if (delta > UINT64_MAX - progress.sourceReadFrame) { prepared = false; break; }
                std::uint64_t old = 0;
                if (!voices.CaptureStreamRingToken(binding.render, &old)) { prepared = false; break; }
                auto request = binding.source;
                request.targetFrame = position.sourceIntegerFrame;
                request.loopCount = position.loopInfinite ? 0 : position.loopsRemaining;
                const auto demand = std::ceil(std::max(position.currentRatio, position.targetRatio) * block) + 2.0;
                if (demand > UINT32_MAX) { prepared = false; break; }
                request.prebufferTargetFrames = std::max(request.prebufferTargetFrames, static_cast<std::uint32_t>(demand));
                StreamManager::PreparedToken token;
                StreamManager::PreparedStream owned;
                if (streams.BeginReplacement(request, &token) != VN_OK) { prepared = false; break; }
                if (streams.TransferReplacement(token, &owned) != VN_OK) {
                    (void)streams.CancelReplacement(token);
                    prepared = false;
                    break;
                }
                batch->streams.push_back(owned);
                batch->members.push_back({binding.render, old, owned.ring, owned.ringToken,
                    progress.sourceReadFrame + delta, position.sourceIntegerFrame, request.loopCount});
            }
            if (!prepared || !representative) continue;
            batch->render.members = batch->members;
            if (!voices.PostGroupRecovery(representative, &batch->render, &batch->ticket)) continue;
            group.pending = std::move(batch);
        }
        // Cancellation may synchronously retire bindings and their host storage.
        if (stopOwner) (void)StopPlayable(*this, service.rootInstance, 0);
    }
}

bool Engine::CaptureGroupRecovery(VnVoiceHandle render, VnPlaybackReadinessMemberV1& out) {
    const auto slot = static_cast<std::uint32_t>(render);
    if (slot >= streamResyncs.size()) return false;
    const auto& row = streamResyncs[slot];
    if (row.handle != render || !row.group) return false;
    const auto& group = *row.group->group;
    const auto serial = group.render.recoverySerial.load(std::memory_order_acquire);
    if (!serial || serial == group.servedSerial) return false;
    out.replacementSerial = serial;
    out.preparationGeneration = out.logicCommitSequence = out.sourceGeneration = 0;
    out.terminalReason = 0;
    out.ringToken = 0;
    out.state = VN_PLAYBACK_READINESS_PREPARING;
    out.result = VN_OK;
    out.bufferedFrames = 0;
    out.appliedAudioBlock = 0;
    out.appliedFrame = out.scheduledFrame = UINT64_MAX;
    if (!group.pending) return true;
    const auto& batch = *group.pending;
    for (const auto& member : batch.members) if (member.render == render) {
        StreamManager::PlaybackObservation observation{};
        if (!streams.CapturePlayback(member.ringToken, &observation)) return true;
        out.ringToken = member.ringToken;
        out.sourceGeneration = observation.sourceGeneration;
        out.contentRevision = observation.contentRevision;
        out.preparationGeneration = observation.preparationGeneration;
        out.bufferedFrames = observation.bufferedFrames;
        out.prebufferTargetFrames = observation.prebufferTargetFrames;
        out.terminalReason = static_cast<std::uint32_t>(observation.terminal);
        out.result = observation.result;
        out.scheduledFrame = batch.plannedFrame;
        out.appliedFrame = observation.appliedFrame;
        out.appliedAudioBlock = observation.appliedAudioBlock;
        out.state = observation.state == StreamManager::PreparationState::Ready ? VN_PLAYBACK_READINESS_READY :
            observation.state == StreamManager::PreparationState::Failed ? VN_PLAYBACK_READINESS_FAILED :
            VN_PLAYBACK_READINESS_PREPARING;
        if (out.appliedAudioBlock) {
            out.state = VN_PLAYBACK_READINESS_APPLIED;
            out.scheduledFrame = out.appliedFrame;
        }
        break;
    }
    return true;
}

void Engine::ShutdownGroupRecoveriesAfterQuiescence() noexcept {
    for (auto& service : playableServices) for (auto& group : service.streamRecoveries) if (group->pending) {
        auto& batch = *group->pending;
        const bool joined = voices.CollectGroupRecovery(batch.ticket, &batch.render);
        assert(joined);
        (void)joined;
        batch.ticket = 0;
        group->pending.reset();
    }
    for (auto& row : streamResyncs) if (row.group) row = {};
}

} // namespace harken
