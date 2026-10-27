#include "harken/render/ImmediateLaneTimelineFinalizer.h"
#include "harken/core/BlockArena.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/QueueContinuationLane.h"
#include "harken/render/FrontierTimelineFinalizer.h"
#include "harken/render/VoicePool.h"

#include <cassert>
#include <algorithm>
#include <limits>

namespace harken {

struct ImmediateLaneTimelineFinalizer::Impl {
    struct EventScratch {
        VoiceTimelineReservation reservation{};
        VoiceTimelineReservationProof voiceProof{};
        TimelineArmTable::LaneAudioProof armProof{};
        VariableTimelineInitializerProof initializer{};
        std::uint64_t actualStartFrame = UINT64_MAX;
    };
    struct Receipt {
        std::uint32_t slot = UINT32_MAX;
        const ImmediateLaneBucket* bucket = nullptr;
        bool captured = false;
        bool claimed = false;
        bool applied = false;
        bool cancelledOnly = false;
        bool sourceWaiting = false;
        bool queueDeferred = false;
        std::uint32_t waitingEntryCount = 0;
    };
    struct ControlScratch {
        TimelineArmToken token{};
        ScheduledEventInput due[2]{};
        TimelineArmTable::AuthoredControlBucketProof proof{};
    };
    struct FenceScratch {
        const AuthoredStopFenceDescriptor* descriptor = nullptr;
        AuthoredStopFenceLaneProof proof{};
    };
    ImmediateActionLane* lane = nullptr;
    void* queueContext = nullptr;
    QueueBucketApply applyQueue = nullptr;
    FrontierTimelineFinalizer* frontier = nullptr;
    const ScheduledTransactionPool* pool = nullptr;
    std::uint32_t blockFrames = 0;
    std::uint32_t eventCapacity = 0;
    std::uint32_t receiptCount = 0;
    std::uint64_t capturedQuantum = 0;
    bool prepared = false;
    NativeTimelineQuantumAdmissionState admission;
    StableRowStorage<EventScratch> events;
    StableRowStorage<ControlScratch> controls;
    StableRowStorage<FenceScratch> fences;
    ImmediateActionLane::EventStorageRegistration eventStorageRegistration;
    std::unique_ptr<Receipt[]> receipts;
    std::unique_ptr<std::uint32_t[]> controlRenderGenerations;
    std::uint32_t renderCapacity = 0;
    static bool PrepareEventStorage(void* context, std::uint32_t demand) noexcept {
        auto& self = *static_cast<Impl*>(context);
        const auto count = std::min(self.eventCapacity, demand);
        return self.events.ConstructThrough(count) && self.controls.ConstructThrough(count) &&
            self.fences.ConstructThrough(count);
    }
};

ImmediateLaneTimelineFinalizer::ImmediateLaneTimelineFinalizer(
    VoicePool& voices, TimelineArmTable& arms) : voices_(voices), arms_(arms) {}
ImmediateLaneTimelineFinalizer::~ImmediateLaneTimelineFinalizer() {
    assert(impl_ == nullptr || impl_->admission.readers_.load(std::memory_order_acquire) == 0);
}

bool ImmediateLaneTimelineFinalizer::Attach(
    ImmediateActionLane& lane, std::uint32_t blockFrames) {
    if (!lane.Valid() || blockFrames == 0 ||
        (impl_ != nullptr && impl_->admission.readers_.load(std::memory_order_acquire) != 0)) return false;
    auto state = std::make_unique<Impl>();
    state->lane = &lane;
    state->blockFrames = blockFrames;
    state->eventCapacity = lane.MaxEventsPerQuantum();
    if (!state->events.Reserve(state->eventCapacity) ||
        !state->controls.Reserve(state->eventCapacity) ||
        !state->fences.Reserve(state->eventCapacity) ||
        !Impl::PrepareEventStorage(state.get(), lane.EventStorageHighWater())) return false;
    state->receipts = std::make_unique<Impl::Receipt[]>(lane.AttemptCapacity());
    state->renderCapacity = voices_.Capacity();
    state->controlRenderGenerations =
        std::make_unique<std::uint32_t[]>(state->renderCapacity);
    lane.AttachEventStorage(state->eventStorageRegistration, state.get(), Impl::PrepareEventStorage);
    impl_ = std::move(state);
    return true;
}

bool ImmediateLaneTimelineFinalizer::AttachFrontier(
    FrontierTimelineFinalizer& frontier,
    const ScheduledTransactionPool& pool) noexcept {
    if (impl_ == nullptr) return true;
    if (!impl_->eventStorageRegistration.IsFor(impl_->lane) ||
        !frontier.AttachLaneEventStorage(*impl_->lane)) return false;
    impl_->frontier = &frontier;
    impl_->pool = &pool;
    return true;
}

void ImmediateLaneTimelineFinalizer::AttachQueueSink(void* context, QueueBucketApply apply) noexcept {
    if (impl_ == nullptr) return;
    impl_->queueContext = context;
    impl_->applyQueue = apply;
}

std::uint64_t ImmediateLaneTimelineFinalizer::EventStorageBytes() const noexcept {
    return impl_ == nullptr ? 0u : impl_->events.RequestedBytes() +
        impl_->controls.RequestedBytes() + impl_->fences.RequestedBytes();
}

#if !defined(HARKEN_RT_QUALIFICATION)
void ImmediateLaneTimelineFinalizer::SetEventCommitPageLimitForTests(std::size_t pages) noexcept {
    if (impl_ != nullptr) impl_->events.SetCommitPageLimitForTests(pages);
}
#endif

bool ImmediateLaneTimelineFinalizer::Capture(std::uint64_t quantum) noexcept {
    if (impl_ == nullptr || !impl_->eventStorageRegistration.IsFor(impl_->lane)) return false;
    auto& state = *impl_;
    if (state.prepared) {
        assert(state.capturedQuantum == quantum);
        return state.capturedQuantum == quantum && state.receiptCount != 0;
    }
    state.admission.Invalidate();
    state.prepared = true;
    state.capturedQuantum = quantum;
    for (std::uint32_t slot = 0; slot < state.lane->AttemptCapacity(); ++slot) {
        const auto* bucket = state.lane->CaptureDue(slot, quantum);
        if (bucket != nullptr)
            state.receipts[state.receiptCount++] = {slot, bucket, true, false, false};
    }
    return state.receiptCount != 0;
}

bool ImmediateLaneTimelineFinalizer::AdmitQuantum(
    std::uint32_t availableBytes, std::uint32_t blockFrames,
    const DecisionTable* decisions,
    const Plan03ScratchDecisionState* currentSlices,
    std::uint32_t currentSliceCount) const noexcept {
    if (impl_ == nullptr || !impl_->prepared) {
        return voices_.AdmitTimelineQuantum(availableBytes, blockFrames,
            decisions, currentSlices, currentSliceCount);
    }
    auto& state = *impl_;
    state.admission.Invalidate();
    std::fill_n(state.controlRenderGenerations.get(), state.renderCapacity, 0u);
    for (std::uint32_t index = 0; index < state.receiptCount; ++index) {
        const auto& receipt = state.receipts[index];
        const auto* events = state.lane->Events(receipt.slot, receipt.bucket);
        if (events == nullptr) continue;
        for (std::uint32_t eventIndex = 0; eventIndex < receipt.bucket->eventCount; ++eventIndex) {
            const auto& event = events[eventIndex].event;
            if (!state.lane->IsEventAdmitted(receipt.slot, receipt.bucket,
                    receipt.bucket->firstEvent + eventIndex)) continue;
            const auto* program = event.program.program;
            if (program == nullptr || program->role != TimelineProgramRole::Voice ||
                program->generation != event.program.programGeneration ||
                program->payloads == nullptr || event.payload >= program->payloadCount) continue;
            const auto kind = program->payloads[event.payload].kind;
            if (kind != TimelineEventKind::Envelope && kind != TimelineEventKind::Stop) continue;
            if (!event.armToken.Valid() || event.armToken.renderSlot >= state.renderCapacity ||
                state.lane->CapturedArmReader({receipt.slot, receipt.bucket->attemptGeneration},
                    receipt.bucket, receipt.bucket->firstEvent + eventIndex) == nullptr) continue;
            const auto render = static_cast<VnVoiceHandle>(event.armToken.renderSlot) |
                (static_cast<VnVoiceHandle>(event.armToken.renderGeneration) << 32);
            if (voices_.Resolve(render) == nullptr) continue;
            state.controlRenderGenerations[event.armToken.renderSlot] = event.armToken.renderGeneration;
        }
    }
    return voices_.AdmitTimelineQuantum(availableBytes, blockFrames, decisions,
        currentSlices, currentSliceCount, state.controlRenderGenerations.get(), state.renderCapacity);
}

bool ImmediateLaneTimelineFinalizer::ProveQuantumAdmission(
    const BlockArena& arena, std::uint32_t blockFrames,
    NativeTimelineQuantumAdmissionProof* admitted,
    const DecisionTable* decisions, const Plan03ScratchDecisionState* currentSlices,
    std::uint32_t currentSliceCount) const noexcept {
    if (admitted == nullptr || admitted->state_ != nullptr || impl_ == nullptr ||
        !impl_->prepared || blockFrames == 0 || blockFrames != impl_->blockFrames ||
        impl_->capturedQuantum > UINT64_MAX / blockFrames ||
        impl_->capturedQuantum * blockFrames > UINT64_MAX - blockFrames ||
        arena.UsedBytes() > arena.CapacityBytes()) return false;
    // This calls the complete existing control/current scratch calculation.
    // A failed native calculation never issues an admission capability.
    if (!AdmitQuantum(arena.CapacityBytes() - arena.UsedBytes(), blockFrames,
            decisions, currentSlices, currentSliceCount)) return false;
    auto& state = *impl_;
    const auto epoch = state.admission.epoch_.load(std::memory_order_acquire);
    if (epoch == 0) return false;
    auto count = state.admission.readers_.load(std::memory_order_relaxed);
    do {
        if (count == UINT32_MAX) return false;
    } while (!state.admission.readers_.compare_exchange_weak(count, count + 1u,
        std::memory_order_acq_rel, std::memory_order_relaxed));
    admitted->state_ = &state.admission;
    admitted->arms_ = &arms_;
    admitted->voices_ = &voices_;
    admitted->arena_ = &arena;
    admitted->epoch_ = epoch;
    admitted->quantum_ = state.capturedQuantum;
    admitted->blockFrames_ = blockFrames;
    return true;
}

void ImmediateLaneTimelineFinalizer::Discard(bool arenaCapacityRefused) noexcept {
    if (impl_ == nullptr) return;
    auto& state = *impl_;
    for (std::uint32_t i = 0; i < state.receiptCount; ++i) {
        const auto& receipt = state.receipts[i];
        assert(!receipt.claimed);
        if (receipt.captured && !receipt.claimed) {
            if (arenaCapacityRefused)
                (void)state.lane->RecordAdmissionFailure(
                    receipt.slot, receipt.bucket,
                    ImmediateLaneAdmissionFailure::ArenaCapacity);
            if (state.lane->FinishDiscardedConditionalWithdrawal(receipt.slot, receipt.bucket) ||
                state.lane->FinishDiscardedPlainWithdrawals(receipt.slot, receipt.bucket))
                continue;
            (void)state.lane->ReleaseCapture(receipt.slot, receipt.bucket);
        }
    }
    state.receiptCount = 0;
    state.prepared = false;
    state.admission.Invalidate();
}

void ImmediateLaneTimelineFinalizer::Apply(std::uint64_t quantum) noexcept {
    if (impl_ == nullptr) return;
    auto& state = *impl_;
    if (!state.prepared || state.capturedQuantum != quantum ||
        quantum >= UINT64_MAX / state.blockFrames) return;
    const auto quantumStart = quantum * state.blockFrames;
    for (std::uint32_t receiptIndex = 0; receiptIndex < state.receiptCount; ++receiptIndex) {
        auto& receipt = state.receipts[receiptIndex];
        const auto slot = receipt.slot;
        const auto* bucket = receipt.bucket;
        if (const auto* continuation = state.lane->CapturedQueueContinuation(slot, bucket)) {
            std::uint64_t delay = 0;
            if (continuation->TimingForQuantum(quantumStart, quantumStart + state.blockFrames, &delay) ==
                    QueueConditionalProofStatus::Waiting) {
                receipt.queueDeferred = true;
                continue;
            }
            receipt.applied = state.applyQueue != nullptr &&
                state.applyQueue(state.queueContext, *state.lane, slot, bucket, quantum, &receipt.claimed);
            if (!receipt.applied && !receipt.claimed)
                (void)state.lane->RejectCapturedBucket({slot, bucket->attemptGeneration}, bucket);
            continue;
        }
        std::uint64_t resumeQuantum = 0;
        const bool resuming = state.lane->CapturedSourceWaitQuantum(slot, bucket, &resumeQuantum) &&
            resumeQuantum == quantum;
        const auto waiting = state.lane->CapturedSourceWaitScratch(slot, bucket);
        auto* waitingEntries = waiting.data();
        const auto waitingCapacity = static_cast<std::uint32_t>(waiting.size());
        const auto recordWaiting = [&](const FrontierEntryReceiptRef& entry) noexcept {
            if (!entry.Valid()) return true;
            for (std::uint32_t index = 0; index < receipt.waitingEntryCount; ++index)
                if (waitingEntries[index] == entry) return true;
            if (receipt.waitingEntryCount == waitingCapacity) return false;
            waitingEntries[receipt.waitingEntryCount++] = entry;
            return true;
        };
        const auto originalQuantumStart = resuming ? bucket->quantum * state.blockFrames : quantumStart;
        const auto* events = state.lane->Events(slot, bucket);
        bool valid = (events != nullptr || bucket->eventCount == 0) &&
                     bucket->eventCount <= state.events.ConstructedCount() &&
                     bucket->eventCount <= state.controls.ConstructedCount() &&
                     bucket->eventCount <= state.fences.ConstructedCount() &&
                     state.lane->CapturedEntryProjectionValid(slot, bucket);
        std::uint32_t admittedGroups = 0;
        std::uint32_t admittedEvents = 0;
        std::uint32_t originalFutureCount = 0;
        std::uint32_t futureCount = 0;
        for (std::uint32_t i = 0; i < bucket->groupCount; ++i)
            admittedGroups += state.lane->IsGroupAdmitted(slot, bucket,
                bucket->firstGroup + i) ? 1u : 0u;
        for (std::uint32_t i = 0; events != nullptr && i < bucket->eventCount; ++i) {
            originalFutureCount += events[i].futureGroupedStart ? 1u : 0u;
            if (!state.lane->IsEventAdmitted(slot, bucket, bucket->firstEvent + i)) continue;
            ++admittedEvents;
            futureCount += events[i].futureGroupedStart ? 1u : 0u;
        }
        const bool registered = state.lane->HasRegisteredEntries(slot, bucket->attemptGeneration);
        valid = bucket->memberCount == originalFutureCount && valid;
        if (valid && admittedGroups == 0 && admittedEvents == 0) {
            receipt.claimed = state.lane->ClaimCancelledOnly(
                {slot, bucket->attemptGeneration}, bucket);
            receipt.cancelledOnly = receipt.claimed;
            if (!receipt.claimed)
                (void)state.lane->RejectCapturedBucket({slot, bucket->attemptGeneration}, bucket);
            continue;
        }
        const bool grouped = admittedGroups != 0;
        bool continuation = false;
        const auto isInitialQueueCompletion = [](const Plan03PageGroupView& view) noexcept {
            return view.frontier != nullptr && view.frontier->role == FrontierGroupRole::Future &&
                view.frontier->queueInitial.Valid() && !view.frontier->queueConditional.Valid() &&
                !view.frontier->variableSide.Valid() && view.frontier->transition.Valid() &&
                view.identity.sliceIndex == 1u && view.memberCount == 2u && view.sourceControlCount == 1u;
        };
        const auto hasConditionalTargetSpans = [&](const Plan03PageGroupView& view) noexcept {
            if (view.frontier == nullptr || !view.frontier->queueConditional.Valid() ||
                view.frontier->queueInitial.Valid() || view.frontier->variableSide.Valid() ||
                !view.frontier->transition.Valid() || view.members == nullptr) return false;
            const auto& binding = *view.frontier;
            const bool initial = binding.role == FrontierGroupRole::EntryMember &&
                view.identity.sliceIndex == 0u && view.memberCount != 0u && view.sourceControlCount == 0u &&
                binding.operation.order.executeFrame == binding.transition.audibleFrame;
            const bool completion = binding.role == FrontierGroupRole::Future &&
                view.identity.sliceIndex == 1u && view.memberCount > 1u && view.sourceControlCount == 1u &&
                binding.operation.order.executeFrame > binding.transition.audibleFrame;
            if (!initial && !completion) return false;
            for (std::uint16_t member = view.sourceControlCount; member < view.memberCount; ++member) {
                const ImmediateLaneEvent* mapped = nullptr;
                std::uint16_t count = 0;
                if (!state.lane->FutureMember(slot, bucket->attemptGeneration, view.identity,
                        member, view.members[member], &mapped, &count) || (count != 1u && count != 2u)) return false;
            }
            return true;
        };
        const bool futureOnly = !grouped && futureCount != 0;
        if (state.frontier != nullptr)
            state.frontier->BeginLane(quantum, state.blockFrames, *state.lane, bucket, slot);
        if (futureOnly)
            valid = (registered || state.lane->EntryApplied(slot,
                bucket->attemptGeneration)) && futureCount == admittedEvents && valid;
        if (grouped) {
            valid = state.frontier != nullptr && state.pool != nullptr &&
                admittedGroups <= state.pool->MaxPlan03GroupHeadersPerPage() && valid;
            if (valid) {
                const auto* groups = state.lane->Groups(slot, bucket);
                valid = groups != nullptr;
                for (std::uint32_t i = 0; valid && i < bucket->groupCount; ++i) {
                    if (!state.lane->IsGroupAdmitted(slot, bucket, bucket->firstGroup + i)) continue;
                    Plan03PageGroupView view{};
                    valid = state.pool->ResolvePlan03GroupSliceRt(groups[i].identity, &view) &&
                        view.frontier != nullptr && state.frontier->CollectGroup(view);
                    if (valid && view.frontier->role == FrontierGroupRole::Coordinator &&
                        view.frontier->prerequisite.Valid()) continuation = true;
                    if (valid && view.frontier->role == FrontierGroupRole::Future &&
                        (isInitialQueueCompletion(view) || hasConditionalTargetSpans(view))) continuation = true;
                }
            }
        }
        auto sources = grouped && state.frontier != nullptr
            ? state.frontier->VariableSourcesStatus(waitingEntries, waitingCapacity,
                &receipt.waitingEntryCount) : VariableReadCutStatus::Applied;
        bool waitingShape = valid;
        bool publicationWaiting = false;
        bool relativePublication = !grouped && !futureOnly;
        for (std::uint32_t i = 0; events != nullptr && i < bucket->eventCount; ++i) {
            if (!state.lane->IsEventAdmitted(slot, bucket, bucket->firstEvent + i)) continue;
            relativePublication = relativePublication &&
                events[i].timeDomain == ImmediateLaneTimeDomain::RelativeToAnchor;
            const auto& event = events[i].event;
            const auto* program = event.program.program;
            const auto* payload = program != nullptr && program->payloads != nullptr &&
                event.payload < program->payloadCount ? &program->payloads[event.payload] : nullptr;
            waitingShape = payload != nullptr && program->generation == event.program.programGeneration &&
                event.executeFrame >= originalQuantumStart &&
                event.executeFrame - originalQuantumStart < state.blockFrames && waitingShape;
            if (payload == nullptr) continue;
            if (!grouped && !futureOnly && !registered && bucket->groupCount == 0 &&
                bucket->memberCount == 0 && !event.frontier.Present() &&
                !events[i].futureGroupedStart && program->role == TimelineProgramRole::Voice &&
                payload->kind == TimelineEventKind::Start) {
                TimelineArmTable::LaneAudioProof cancelled;
                const auto* reader = state.lane->CapturedArmReader(
                    {slot, bucket->attemptGeneration}, bucket, bucket->firstEvent + i);
                if (reader != nullptr && arms_.ProveLaneCancelledStart(*reader, event,
                        events[i].armProgramGeneration, quantum + 1u, &cancelled))
                    continue;
            }
            if (payload->kind == TimelineEventKind::ControllerActivation || payload->kind == TimelineEventKind::Start) {
                const auto timing = state.frontier == nullptr ? VariableReadCutStatus::Applied :
                    state.frontier->LaneEventTimingStatus(event);
                if (timing == VariableReadCutStatus::Failed) sources = timing;
                else if (timing == VariableReadCutStatus::Waiting && sources != VariableReadCutStatus::Failed)
                    sources = timing;
            }
            if (program->role != TimelineProgramRole::Voice || payload->kind != TimelineEventKind::Start) continue;
            bool waitForPublication = false;
            const auto status = voices_.TimelineStartSourcesStatus(arms_,
                event.armToken, &waitForPublication);
            publicationWaiting = publicationWaiting || waitForPublication;
            if (status == VariableReadCutStatus::Failed) sources = status;
            else if (status == VariableReadCutStatus::Waiting) {
                if (!recordWaiting(event.frontier.entry)) sources = VariableReadCutStatus::Failed;
                else if (sources != VariableReadCutStatus::Failed) sources = status;
            }
            const auto handle = static_cast<std::uint64_t>(event.armToken.renderSlot) |
                (static_cast<std::uint64_t>(event.armToken.renderGeneration) << 32);
            const auto* voice = voices_.Resolve(handle);
            std::uint16_t outputOffset = static_cast<std::uint16_t>(event.executeFrame - originalQuantumStart);
            if (state.frontier != nullptr && !state.frontier->LaneEventOffset(event, &outputOffset))
                sources = VariableReadCutStatus::Failed;
            const auto effective = quantumStart + outputOffset;
            if (voice == nullptr || effective >= voice->authoredRootStopFrame) sources = VariableReadCutStatus::Failed;
        }
        if (publicationWaiting && !relativePublication) {
            (void)state.lane->RejectCapturedBucket({slot, bucket->attemptGeneration}, bucket);
            continue;
        }
        if (waitingShape && sources == VariableReadCutStatus::Waiting) {
            receipt.sourceWaiting = true;
            continue;
        }
        valid = sources != VariableReadCutStatus::Failed && valid;
        std::uint32_t staged = 0;
        std::uint32_t controlCount = 0;
        std::uint32_t fenceCount = 0;
        bool sourcesWaiting = false;
        for (std::uint32_t i = 0; events != nullptr && i < bucket->eventCount; ++i) {
            if (!state.lane->IsEventAdmitted(slot, bucket, bucket->firstEvent + i)) continue;
            const auto& event = events[i].event;
            const auto* program = event.program.program;
            const auto* payload = program != nullptr && program->payloads != nullptr &&
                event.payload < program->payloadCount ? &program->payloads[event.payload] : nullptr;
            const bool basic = program != nullptr && payload != nullptr &&
                event.program.programGeneration != 0 &&
                program->generation == event.program.programGeneration &&
                event.executeFrame >= originalQuantumStart &&
                event.executeFrame - originalQuantumStart < state.blockFrames;
            std::uint16_t outputOffset = static_cast<std::uint16_t>(event.executeFrame - originalQuantumStart);
            const bool offsetValid = basic && (state.frontier == nullptr ||
                state.frontier->LaneEventOffset(event, &outputOffset));
            bool localInitialCompletion = false;
            bool localConditionalSpans = false;
            if (events[i].futureGroupedStart && grouped && state.pool != nullptr) {
                const auto* bucketGroups = state.lane->Groups(slot, bucket);
                for (std::uint32_t index = 0; bucketGroups != nullptr && index < bucket->groupCount; ++index) {
                    if (!state.lane->IsGroupAdmitted(slot, bucket, bucket->firstGroup + index) ||
                        !(bucketGroups[index].identity == event.frontier.group)) continue;
                    Plan03PageGroupView group{};
                    if (!state.pool->ResolvePlan03GroupSliceRt(bucketGroups[index].identity, &group) ||
                        group.frontier == nullptr) {
                        valid = false;
                        break;
                    }
                    localInitialCompletion = isInitialQueueCompletion(group);
                    localConditionalSpans = hasConditionalTargetSpans(group);
                }
            }
            if (event.frontier.Present() &&
                (!events[i].futureGroupedStart || localInitialCompletion || localConditionalSpans)) {
                if (localInitialCompletion && events[i].futureGroupedStart) {
                    const ImmediateLaneEvent* mapped = nullptr;
                    valid = basic && program->role == TimelineProgramRole::Voice &&
                        payload->kind == TimelineEventKind::Start &&
                        payload->reserved == kTimelineEventTransientStartArm &&
                        event.subeventPhase == 20u && events[i].armProgramGeneration != 0 &&
                        state.lane->FutureMember(slot, bucket->attemptGeneration,
                            event.frontier.group, 1u, event.armToken, &mapped) &&
                        mapped == &events[i] && valid;
                }
                if (grouped && state.frontier != nullptr &&
                    !state.frontier->CollectEvent(event, payload,
                        basic ? kTimelineDispositionApplied :
                            kTimelineDispositionProtocolViolation)) valid = false;
                else if (!grouped) valid = false;
                valid = basic && valid;
                continue;
            }
            if (!valid) continue;
            const bool plainControl = !grouped && !futureOnly && !registered &&
                bucket->groupCount == 0 && bucket->memberCount == 0 && !event.frontier.Present();
            if (program != nullptr && program->role == TimelineProgramRole::AuthoredStopFence) {
                auto& fence = state.fences[fenceCount++];
                fence.descriptor = state.lane->CapturedNoneFence(
                    {slot, bucket->attemptGeneration}, bucket, bucket->firstEvent + i);
                valid = basic && plainControl && fence.descriptor != nullptr &&
                    event.executeFrame == fence.descriptor->identity.stopFrame &&
                    event.subeventPhase == 50 && !event.authoredFadeOutCandidate &&
                    event.payload == 0 &&
                    fence.descriptor->ProveLaneApply(event.program, &fence.proof);
                continue;
            }
            const bool control = payload != nullptr &&
                (payload->kind == TimelineEventKind::Envelope || payload->kind == TimelineEventKind::Stop);
            if (control) {
                valid = basic && plainControl && program->role == TimelineProgramRole::Voice &&
                    payload->envelopeCurve == nullptr;
                if (!valid) break;
                std::uint32_t match = 0;
                for (; match < controlCount; ++match) {
                    const auto token = state.controls[match].token;
                    if (token.slot == event.armToken.slot && token.generation == event.armToken.generation &&
                        token.renderSlot == event.armToken.renderSlot &&
                        token.renderGeneration == event.armToken.renderGeneration) break;
                }
                if (match == controlCount) {
                    auto& scratch = state.controls[controlCount++];
                    scratch.token = event.armToken;
                    std::uint32_t dueCount = 0;
                    for (std::uint32_t due = 0; valid && due < bucket->eventCount; ++due) {
                        const auto& input = events[due].event;
                        if (input.armToken.slot != event.armToken.slot ||
                            input.armToken.generation != event.armToken.generation ||
                            input.armToken.renderSlot != event.armToken.renderSlot ||
                            input.armToken.renderGeneration != event.armToken.renderGeneration) continue;
                        valid = dueCount < 2 && input.program.program == event.program.program &&
                            input.program.programGeneration == event.program.programGeneration &&
                            !input.frontier.Present() && !events[due].futureGroupedStart &&
                            state.lane->IsEventAdmitted(slot, bucket, bucket->firstEvent + due);
                        if (valid) scratch.due[dueCount++] = {
                            input.executeFrame, input.logicCommitSequence, input.executionOrdinal,
                            input.eventIndex, input.transactionOrdinal, input.subeventPhase,
                            input.payload, input.authoredFadeOutCandidate, input.armToken, input.frontier};
                    }
                    const auto* reader = state.lane->CapturedArmReader(
                        {slot, bucket->attemptGeneration}, bucket, bucket->firstEvent + i);
                    valid = valid && reader != nullptr &&
                        arms_.ProveAuthoredControlBucket(event.armToken, *reader, event.program,
                            quantumStart, state.blockFrames, quantum + 1u,
                            scratch.due, dueCount, &scratch.proof);
                }
                if (!valid) break;
            }
            if (futureOnly && !events[i].futureGroupedStart) {
                valid = false;
                break;
            }
            valid = basic && offsetValid && events[i].armProgramGeneration != 0 &&
                program->role == TimelineProgramRole::Voice &&
                (control || (payload->kind == TimelineEventKind::Start &&
                    payload->reserved == (events[i].futureGroupedStart
                        ? kTimelineEventTransientStartArm : 0u) && event.subeventPhase == 20));
            if (!valid) break;
            if (events[i].futureGroupedStart) {
                if (registered && !state.lane->CanAdmitFutureEvent(
                        slot, bucket, bucket->firstEvent + i)) {
                    valid = false;
                    break;
                }
                const auto handle =
                    (static_cast<std::uint64_t>(
                        event.armToken.renderGeneration) << 32) |
                    event.armToken.renderSlot;
                const auto* voice = voices_.Resolve(handle);
                valid = voice != nullptr &&
                    quantumStart + outputOffset < voice->authoredRootStopFrame;
                if (valid) {
                    for (std::uint32_t stopIndex = 0;
                         stopIndex < voice->timelineEvents.eventCount;
                         ++stopIndex) {
                        const auto& stop =
                            voice->timelineEvents.events[stopIndex];
                        if (stop.kind == TimelineEventKind::Stop &&
                            stop.authoredRootStop &&
                            stop.frontierProof.Eligible(
                                quantum, event.armToken.renderGeneration) &&
                            stop.order.executeFrame <= quantumStart + outputOffset) {
                            valid = false;
                            break;
                        }
                    }
                }
                if (!valid) break;
            }
            auto& scratch = state.events[staged];
            scratch.actualStartFrame = quantumStart + outputOffset;
            scratch.initializer.Reset();
            scratch.reservation = {};
            if (plainControl && !control && !events[i].futureGroupedStart) {
                const auto* reader = state.lane->CapturedArmReader(
                    {slot, bucket->attemptGeneration}, bucket, bucket->firstEvent + i);
                if (reader != nullptr && arms_.ProveLaneCancelledStart(*reader, event,
                        events[i].armProgramGeneration, quantum + 1u, &scratch.armProof)) {
                    ++staged;
                    continue;
                }
            }
            if (!control) {
                const auto source = voices_.TimelineStartSourcesStatus(arms_, event.armToken);
                if (source != VariableReadCutStatus::Applied) {
                    sourcesWaiting = source == VariableReadCutStatus::Waiting;
                    valid = false;
                    break;
                }
            }
            valid = voices_.ReserveLaneTimelineEvents(event.armToken.renderSlot,
                event.armToken.renderGeneration, quantum, 1, &scratch.reservation);
            if (!valid) break;
            ++staged;
            VoiceTimelineEvent voice{};
            voice.kind = payload->kind;
            if (control) {
                voice.envelopeDirection = payload->envelopeDirection;
                voice.frame = payload->envelopeStartFrame;
                voice.endFrame = payload->envelopeEndFrame;
                voice.envelopeCurve = payload->envelopeCurve;
                voice.authoredFadeOutCandidate = payload->authoredFadeOutCandidate;
                voice.authoredRootStop = (payload->reserved & kTimelineEventAuthoredRootStop) != 0;
                voice.authoredStopOperation = payload->authoredStopOperation;
                voice.controlCause = payload->controlCause;
            }
            voice.offset = outputOffset;
            voice.order = {event.executeFrame, event.logicCommitSequence,
                event.executionOrdinal, event.eventIndex, event.transactionOrdinal,
                event.subeventPhase, event.rowSequence};
            voice.frontierProof.state = FrontierEventState::Pending;
            voice.frontierProof.quantum = quantum;
            voice.frontierProof.renderGeneration = event.armToken.renderGeneration;
            valid = voices_.StageReservedLaneTimelineEvent(scratch.reservation, 0, voice) &&
                voices_.ProveTimelineReservation(scratch.reservation, &scratch.voiceProof);
            if (valid && events[i].futureGroupedStart) {
                Plan03PageGroupView group{};
                const ImmediateLaneEvent* mapped = nullptr;
                std::uint16_t memberIndex = UINT16_MAX;
                valid = state.pool != nullptr &&
                    state.pool->ResolvePlan03GroupSliceRt(
                        event.frontier.group, &group) &&
                    group.frontier != nullptr && group.lease != nullptr &&
                    group.frontier->role == FrontierGroupRole::EntryMember &&
                    group.frontier->entry == event.frontier.entry &&
                    group.frontier->operation.owner == event.frontier.owner;
                bool sameBucketEntry = false;
                if (valid && grouped) {
                    const auto* bucketGroups = state.lane->Groups(slot, bucket);
                    valid = bucketGroups != nullptr &&
                        (event.executeFrame >
                             group.frontier->operation.order.executeFrame ||
                         (event.executeFrame ==
                              group.frontier->operation.order.executeFrame &&
                          group.frontier->operation.kind ==
                              FrontierOperationKind::Dependent &&
                          event.frontier.entry == group.frontier->entry &&
                          event.subeventPhase >
                              group.frontier->operation.order.subeventPhase));
                    for (std::uint32_t groupIndex = 0;
                         valid && groupIndex < bucket->groupCount;
                         ++groupIndex)
                        sameBucketEntry = sameBucketEntry ||
                            (state.lane->IsGroupAdmitted(slot, bucket,
                                 bucket->firstGroup + groupIndex) &&
                             bucketGroups[groupIndex].identity == event.frontier.group);
                    valid = valid && (sameBucketEntry ||
                        (group.frontier->operation.order.executeFrame / state.blockFrames <
                            bucket->quantum &&
                         group.lease->Capture().state == AudioSliceState::SliceApplied));
                }
                for (std::uint16_t member = 0; valid && member < group.memberCount;
                     ++member) {
                    if (group.members[member].slot == event.armToken.slot &&
                        group.members[member].generation == event.armToken.generation &&
                        group.members[member].renderSlot == event.armToken.renderSlot &&
                        group.members[member].renderGeneration ==
                            event.armToken.renderGeneration) {
                        if (memberIndex != UINT16_MAX) valid = false;
                        memberIndex = member;
                    }
                }
                valid = valid && memberIndex != UINT16_MAX &&
                    state.lane->FutureMember(slot,
                        bucket->attemptGeneration, event.frontier.group,
                        memberIndex, event.armToken, &mapped) &&
                    mapped == &events[i] &&
                    arms_.ProveLaneFutureStart(event.armToken,
                        *group.frontier, memberIndex, event.executeFrame,
                        quantum + 1u, &scratch.armProof,
                        events[i].armProgramGeneration, payload->target,
                        sameBucketEntry);
            } else if (valid && !control) {
                valid = arms_.ProveLaneStart(event.armToken,
                    event.executeFrame, quantum + 1u, &scratch.armProof,
                    events[i].armProgramGeneration, &payload->target);
            }
            if (valid && !control)
                valid = voices_.ProveTimelineVariableInitializer(event.armToken.renderSlot,
                    event.armToken.renderGeneration, scratch.armProof, &scratch.initializer);
            if (valid && grouped && !control)
                valid = state.frontier->CollectLaneStagedStart(events[i], scratch.armProof,
                    scratch.voiceProof, scratch.initializer);
        }
        bool claimed = false;
        if (grouped && state.frontier != nullptr && !sourcesWaiting) {
            struct Claim {
                ImmediateActionLane* lane;
                ImmediateLaneHandle handle;
                const ImmediateLaneBucket* bucket;
                bool allow;
                bool claimed = false;
            } claim{state.lane, {slot, bucket->attemptGeneration}, bucket, valid};
            const auto finish = continuation
                ? &FrontierTimelineFinalizer::FinishLaneContinuationBucket
                : &FrontierTimelineFinalizer::FinishLaneEntryBucket;
            const bool frontierApplied = (state.frontier->*finish)(
                [](void* context, ImmediateLaneBucketClaimReceipt* receipt) noexcept {
                    auto& claim = *static_cast<Claim*>(context);
                    if (!claim.allow) return false;
                    claim.claimed = claim.lane->Claim(
                        claim.handle, claim.bucket, receipt);
                    return claim.claimed;
                }, &claim, state.lane, bucket, slot,
                bucket->attemptGeneration);
            claimed = claim.claimed;
            valid = valid && frontierApplied && claimed;
            if (!claimed) {
                (void)state.frontier->RejectLaneBucket(
                    *state.lane, bucket, slot, bucket->attemptGeneration);
                (void)state.lane->RejectCapturedBucket(
                    {slot, bucket->attemptGeneration}, bucket);
            }
        } else {
            if (valid)
                claimed = state.lane->ClaimRetainingCaptureOnRefusal(
                    {slot, bucket->attemptGeneration}, bucket);
            else if (!sourcesWaiting)
                (void)state.lane->RejectCapturedBucket(
                    {slot, bucket->attemptGeneration}, bucket);
            valid = valid && claimed;
        }
        if (valid) {
            // All potentially failing work is complete. These transitions cannot
            // fail, and the worker sees spans only after this serial boundary.
            // Actual claimed Controls publish their original Source cutoff first.
            for (std::uint32_t i = 0; i < controlCount; ++i)
                TimelineArmTable::ApplyAuthoredControlBucket(state.controls[i].proof);
            for (std::uint32_t i = 0; i < staged; ++i) {
                auto& event = state.events[i];
                const bool cancelled = TimelineArmTable::LaneStartCancelled(event.armProof);
                if (!cancelled) voices_.ApplyTimelineVariableInitializer(event.initializer);
                if (event.voiceProof.Valid()) {
                    if (cancelled) VoicePool::RejectProvenTimelineReservation(event.voiceProof);
                    else VoicePool::ApplyProvenLaneTimelineReservation(event.voiceProof);
                }
                if (event.armProof.Valid()) TimelineArmTable::ApplyLaneStart(event.armProof, event.actualStartFrame);
            }
            for (std::uint32_t i = 0; i < controlCount; ++i) state.controls[i].proof.Release();
            for (std::uint32_t i = 0; i < fenceCount; ++i) {
                const auto* descriptor = state.fences[i].descriptor;
                descriptor->ApplyLaneProof(&state.fences[i].proof);
                descriptor->operation->PublishApplied(descriptor->identity, quantum + 1u);
                descriptor->FinishApply();
            }
        } else {
            for (std::uint32_t i = 0; i < staged; ++i) {
                auto& scratch = state.events[i];
                scratch.initializer.Reset();
                scratch.armProof.Release();
                VoicePool::ReleaseTimelineReservationProof(scratch.voiceProof);
                if (scratch.reservation.count != 0)
                    (void)voices_.RejectTimelineReservation(scratch.reservation);
            }
            for (std::uint32_t i = 0; i < controlCount; ++i) state.controls[i].proof.Release();
            for (std::uint32_t i = 0; i < fenceCount; ++i) state.fences[i].proof.Reset();
        }
        receipt.claimed = claimed;
        receipt.applied = valid;
    }
    // Slot reuse does not preserve causal order. Retry only deferred Queue
    // nodes after other captured buckets have published their actual Starts.
    for (std::uint32_t pass = 0; pass < state.receiptCount; ++pass) {
        bool progressed = false;
        for (std::uint32_t index = 0; index < state.receiptCount; ++index) {
            auto& receipt = state.receipts[index];
            if (!receipt.queueDeferred) continue;
            const auto* continuation = state.lane->CapturedQueueContinuation(receipt.slot, receipt.bucket);
            std::uint64_t delay = 0;
            if (continuation != nullptr && continuation->TimingForQuantum(
                    quantumStart, quantumStart + state.blockFrames, &delay) == QueueConditionalProofStatus::Waiting)
                continue;
            receipt.queueDeferred = false;
            receipt.applied = state.applyQueue != nullptr && state.applyQueue(state.queueContext,
                *state.lane, receipt.slot, receipt.bucket, quantum, &receipt.claimed);
            if (!receipt.applied && !receipt.claimed)
                (void)state.lane->RejectCapturedBucket(
                    {receipt.slot, receipt.bucket->attemptGeneration}, receipt.bucket);
            progressed = true;
        }
        if (!progressed) break;
    }
    for (std::uint32_t index = 0; index < state.receiptCount; ++index) {
        auto& receipt = state.receipts[index];
        if (receipt.queueDeferred) receipt.sourceWaiting = true;
    }
}

void ImmediateLaneTimelineFinalizer::Finish() noexcept {
    if (impl_ == nullptr) return;
    auto& state = *impl_;
    for (std::uint32_t i = 0; i < state.receiptCount; ++i) {
        const auto& receipt = state.receipts[i];
        const auto waiting = state.lane->CapturedSourceWaitScratch(receipt.slot, receipt.bucket);
        const auto* waitingEntries = waiting.data();
        if (receipt.sourceWaiting && state.lane->SuspendCaptureForSourceWait(receipt.slot, receipt.bucket,
                waitingEntries, receipt.waitingEntryCount))
            continue;
        if (receipt.cancelledOnly)
            (void)state.lane->FinishCancelledOnly(receipt.slot, receipt.bucket);
        else if (receipt.claimed)
            (void)state.lane->Finish(receipt.slot, receipt.bucket, receipt.applied);
        else if (receipt.captured)
            (void)state.lane->ReleaseCapture(receipt.slot, receipt.bucket);
    }
    state.receiptCount = 0;
    state.prepared = false;
    state.admission.Invalidate();
}

} // namespace harken
