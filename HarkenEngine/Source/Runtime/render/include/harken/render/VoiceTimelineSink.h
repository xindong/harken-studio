#ifndef HARKEN_RENDER_VOICE_TIMELINE_SINK_H
#define HARKEN_RENDER_VOICE_TIMELINE_SINK_H

#include "harken/core/ArmGroupTable.h"
#include "harken/core/TimelineExecutor.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/QueueContinuationLane.h"
#include "harken/core/TransitionInstancePool.h"
#include "harken/core/VariableControlRuntime.h"
#include "harken/render/VoicePool.h"
#include "harken/render/FrontierTimelineFinalizer.h"
#include "harken/render/ImmediateLaneTimelineFinalizer.h"

#include <algorithm>
#include <array>
#include <new>
#include <vector>

namespace harken {

namespace detail {
struct QueueCancellationRaceTestAccess;
struct VoiceTimelineSinkTestAccess;
}

class VoiceTimelineSink final : public TimelineExecutorSink {
public:
    VoiceTimelineSink(VoicePool& voices, TimelineArmTable& arms)
        : voices_(voices), arms_(arms), frontier_(voices, arms),
          laneFrontier_(voices, arms), immediate_(voices, arms),
          plainResults_(voices.Capacity() * 2u * kTimelineMaxEventsPerVoiceQuantum),
          retiringPlainArms_(arms.Capacity()), retiringPlainArmSlots_(arms.Capacity()) {
        if (!pendingVariableStarts_.Reserve(voices.Capacity()) ||
            !voices.AttachSlotStoragePreparation(slotStorageRegistration_, this,
                [](void* context, std::uint32_t count) noexcept {
                    return static_cast<VoiceTimelineSink*>(context)->pendingVariableStarts_.ConstructThrough(count);
                })) throw std::bad_alloc{};
    }

    ~VoiceTimelineSink() override { slotStorageRegistration_.Detach(); }

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    struct FrontierStageRecordForTest {
        TransactionId transactionId = 0;
        std::uint64_t rowSequence = 0;
        std::uint64_t executeFrame = 0;
        std::uint32_t txnSlot = UINT32_MAX;
        FrontierOperationKind kind = FrontierOperationKind::Invalid;
        TimelineEventKind payloadKind = TimelineEventKind::Start;
        std::uint8_t disposition = 0;
        bool payloadPresent = false;
        bool frozenTxnValid = false;
    };
    void WatchFrontierEntryForTest(FrontierEntryReceiptRef entry) noexcept {
        watchedFrontierEntryForTest_ = entry;
        watchedFrontierStageCountForTest_ = 0;
    }
    std::uint64_t WatchedFrontierStageCountForTest() const noexcept {
        return watchedFrontierStageCountForTest_;
    }
    FrontierStageRecordForTest WatchedFrontierStageForTest(
        std::size_t index) const noexcept {
        return index < watchedFrontierStagesForTest_.size()
            ? watchedFrontierStagesForTest_[index]
            : FrontierStageRecordForTest{};
    }
    struct CapacitySnapshotForTest {
        std::size_t pageGroups = 0;
        std::uint32_t frontierEvents = 0;
        std::uint32_t frontierGroups = 0;
        std::uint32_t laneEvents = 0;
        std::uint32_t laneGroups = 0;
    };
    CapacitySnapshotForTest CaptureCapacityForTest() const noexcept {
        return {pageGroups_.size(), frontier_.EventCapacityForTest(),
            frontier_.GroupCapacityForTest(),
            laneFrontier_.EventCapacityForTest(),
            laneFrontier_.GroupCapacityForTest()};
    }
#endif

    bool AttachImmediateLane(ImmediateActionLane& lane, std::uint32_t blockFrames) {
        if (!immediate_.Attach(lane, blockFrames) ||
            (pool_ != nullptr && !immediate_.AttachFrontier(laneFrontier_, *pool_))) return false;
        immediate_.AttachQueueSink(this, [](void* context, ImmediateActionLane& owner,
            std::uint32_t slot, const ImmediateLaneBucket* bucket, std::uint64_t quantum, bool* claimed) noexcept {
            return static_cast<VoiceTimelineSink*>(context)->ApplyQueueLaneBucket(owner, slot, bucket, quantum, claimed);
        });
        return true;
    }

    bool CaptureIndependentWork(std::uint64_t quantumIndex) noexcept override {
        arenaCapacityRefused_ = false;
        return immediate_.Capture(quantumIndex);
    }

    void DiscardIndependentWork() noexcept override {
        immediate_.Discard(arenaCapacityRefused_);
        arenaCapacityRefused_ = false;
        if (queues_ != nullptr) queues_->CompleteAudioConditionalReads();
    }

    void SetArenaAdmission(std::uint32_t availableBytes,
                           std::uint32_t blockFrames) noexcept {
        arenaAvailableBytes_ = availableBytes;
        blockFrames_ = blockFrames;
    }

    bool AttachPlan03ScratchAuthority(
        DecisionTable& decisions,
        const ScheduledTransactionPool& pool) {
        if (!decisions.Valid() || !pool.Valid() ||
            pool.MaxPlan03GroupHeadersPerPage() == 0) {
            return false;
        }
        decisions_ = &decisions;
        pool_ = &pool;
        pageGroups_.assign(pool.MaxPlan03GroupHeadersPerPage(),
                           Plan03PageGroupView{});
        currentSlices_.assign(decisions.Capacity(),
                              Plan03ScratchDecisionState{});
        authoredStopFences_.assign(pool.MaxScheduledTransactions(), nullptr);
        if (!frontier_.Attach(decisions, pool) ||
            !laneFrontier_.Attach(decisions, pool, true)) return false;
        return immediate_.AttachFrontier(laneFrontier_, pool);
    }

    bool AttachQueueAuthority(QueueInstanceTable& queues,
                              const ArmGroupTable& armGroups,
                              const OccurrenceStatePool& occurrences) {
        if (!queues.Valid() || !armGroups.Valid() || decisions_ == nullptr) {
            return false;
        }
        queues_ = &queues;
        queues.EnableConditionalReadReclamation();
        frontier_.AttachQueues(queues);
        laneFrontier_.AttachQueues(queues);
        armGroups_ = &armGroups;
        frontier_.AttachArmGroups(armGroups);
        laneFrontier_.AttachArmGroups(armGroups);
        occurrences_ = &occurrences;
        frontier_.AttachQueueCompletion(occurrences);
        laneFrontier_.AttachQueueCompletion(occurrences);
        composites_ = nullptr;
        participants_.assign(decisions_->MaxParticipantsPerDecision(),
                             DecisionParticipantRef{});
        return !participants_.empty();
    }

    bool AttachTransitionAuthority(
        TransitionInstancePool& transitions) noexcept {
        if (!transitions.Valid()) {
            return false;
        }
        transitions_ = &transitions;
        frontier_.AttachTransitions(transitions);
        laneFrontier_.AttachTransitions(transitions);
        return true;
    }

    bool AttachQueueAuthority(QueueInstanceTable& queues,
                              const ArmGroupTable& armGroups,
                              const OccurrenceStatePool& occurrences,
                              const CompositeCompletionOwnerPool& composites) {
        if (!AttachQueueAuthority(queues, armGroups, occurrences)) {
            return false;
        }
        composites_ = &composites;
        frontier_.AttachQueueCompletion(occurrences, &composites);
        laneFrontier_.AttachQueueCompletion(occurrences, &composites);
        return true;
    }

    bool AttachVariableControlAuthority(
        VariableControlInstanceTable& variableControls) noexcept {
        variableControls_ = &variableControls;
        frontier_.AttachVariableControls(variableControls);
        laneFrontier_.AttachVariableControls(variableControls);
        return true;
    }

    bool AdmitQuantum(std::uint64_t quantumIndex) const noexcept override {
        arenaCapacityRefused_ = false;
        if (decisions_ == nullptr || pool_ == nullptr ||
            pageGroups_.empty() || currentSlices_.empty()) {
            const bool admitted = immediate_.AdmitQuantum(
                arenaAvailableBytes_, blockFrames_);
            arenaCapacityRefused_ = !admitted;
            return admitted;
        }
        std::fill(currentSlices_.begin(), currentSlices_.end(),
                  Plan03ScratchDecisionState{});
        const std::size_t count = pool_->CapturePlan03PageGroupsRt(
            quantumIndex, pageGroups_.data(), pageGroups_.size());
        if (count > pageGroups_.size()) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const Plan03PageGroupView& view = pageGroups_[index];
            if (!view.identity.decision.Valid() || view.lease == nullptr ||
                view.identity.decision.slot >= currentSlices_.size()) {
                return false;
            }
            Plan03ScratchDecisionState& state =
                currentSlices_[view.identity.decision.slot];
            if (state.generation != 0 &&
                state.generation != view.identity.decision.generation) {
                return false;
            }
            state.generation = view.identity.decision.generation;
            state.currentSliceClaimed =
                state.currentSliceClaimed ||
                view.lease->Capture().state == AudioSliceState::SliceClaimed;
        }
        const bool admitted = immediate_.AdmitQuantum(
            arenaAvailableBytes_, blockFrames_, decisions_,
            currentSlices_.data(),
            static_cast<std::uint32_t>(currentSlices_.size()));
        arenaCapacityRefused_ = !admitted;
        return admitted;
    }

    bool BeginQuantum(std::uint64_t quantumIndex) noexcept override {
        (void)immediate_.Capture(quantumIndex);
        plainResultCount_ = 0;
        plainQuantum_ = quantumIndex;
        authoredStopFenceCount_ = 0;
        frontier_.Begin(quantumIndex, blockFrames_);
        voices_.BeginTimelineQuantum(quantumIndex);
        RetryRetiringPlainArms();
        if (pendingVariableStartSeen_)
            for (auto& pending : pendingVariableStarts_)
                if (pending.count != 0 && !voices_.TimelineBindingValid(
                        pending.arm.renderSlot, pending.arm.renderGeneration))
                    RetirePendingVariableStart(pending);
        return true;
    }

    bool CollectFrontierGroups(std::uint64_t pageQuantum) noexcept override {
        if (pool_ == nullptr) return true;
        const auto count = pool_->CapturePlan03PageGroupsRt(pageQuantum,
            pageGroups_.data(), pageGroups_.size());
        if (count > pageGroups_.size()) return false;
        for (std::size_t i = 0; i < count; ++i)
            if (pageGroups_[i].frontier != nullptr && !frontier_.CollectGroup(pageGroups_[i])) return false;
        return true;
    }

    bool PendingFrontierBindingValid(const ConsumedEvent& event,
        const TimelineEventPayload& payload) const noexcept override {
        return frontier_.PendingBindingValid(event, payload);
    }

    bool StageFrontierEvent(const ConsumedEvent& event,
        const TimelineEventPayload* payload, std::uint8_t disposition) noexcept override {
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
        if (watchedFrontierEntryForTest_.Valid() &&
            event.frontier.entry == watchedFrontierEntryForTest_) {
            if (watchedFrontierStageCountForTest_ <
                watchedFrontierStagesForTest_.size())
                watchedFrontierStagesForTest_[watchedFrontierStageCountForTest_] =
                    {event.transactionId, event.rowSequence,
                     event.executeFrame, event.txnSlot, event.frontier.kind,
                     payload == nullptr ? TimelineEventKind::Start
                                        : payload->kind,
                     disposition, payload != nullptr,
                     event.frozenTxnValid};
            ++watchedFrontierStageCountForTest_;
        }
#endif
        return frontier_.CollectEvent(event, payload, disposition);
    }

    std::uint8_t FrontierEventDisposition(const ConsumedEvent& event) const noexcept override {
        return frontier_.EventDisposition(event);
    }

    std::uint8_t PlainEventDisposition(const ConsumedEvent& event,
        std::uint8_t disposition) const noexcept override {
        if (disposition != kTimelineDispositionApplied) return disposition;
        for (std::uint32_t i = 0; i < plainResultCount_; ++i)
            if (SamePlainArm(plainResults_[i].arm, event.armToken))
                return plainResults_[i].disposition;
        return disposition;
    }

    std::size_t VariableStartStorageBytes() const noexcept {
        return pendingVariableStarts_.RequestedBytes() +
            plainResults_.size() * sizeof(PlainTransactionResult) +
            retiringPlainArms_.size() * sizeof(TimelineArmToken) +
            retiringPlainArmSlots_.size() * sizeof(std::uint32_t);
    }

    bool CollectAuthoredStopFence(const AuthoredStopFenceDescriptor& descriptor,
        std::uint64_t quantumIndex, const TimelineExecutionOrder& order) noexcept override {
        return decisions_ != nullptr ? frontier_.CollectStop(descriptor, order) :
            ApplyAuthoredStopFence(descriptor, quantumIndex);
    }

    bool ApplyPlan03Groups(std::uint64_t quantumIndex) noexcept override {
        if (armGroups_ == nullptr || decisions_ == nullptr ||
            pool_ == nullptr) {
            return true;
        }
        const std::size_t count = pool_->CapturePlan03PageGroupsRt(
            quantumIndex, pageGroups_.data(), pageGroups_.size());
        if (count > pageGroups_.size()) return false;
        for (std::size_t index = 0; index < count; ++index)
            if (!ApplyNativeGroup(pageGroups_[index], quantumIndex)) return false;
        return true;
    }

    bool ApplyNativeGroup(const Plan03PageGroupView& group, std::uint64_t quantumIndex) noexcept {
        if (group.frontier != nullptr) return true;
        DecisionCommittedSnapshot decision{};
        if (!decisions_->CaptureCommitted(group.identity.decision,
                                          &decision)) {
            // Cancelled private Decisions may retire before their invalidated
            // page rows are consumed. They cannot claim audio or block other groups.
            if (group.lease != nullptr &&
                group.lease->Capture().state == AudioSliceState::SliceInvalidated &&
                (decisions_->IsOpen(group.identity.decision) ||
                 decisions_->IsCancelled(group.identity.decision) ||
                 !decisions_->Contains(group.identity.decision))) return true;
            return false;
        }
        if (decision.authoredStopInvalidated) {
            if (group.lease == nullptr) return false;
            (void)group.lease->Invalidate(AudioSliceInvalidateReason::Cancel);
            return true;
        }
        const AudioSliceLeaseSnapshot lease =
            group.lease != nullptr ? group.lease->Capture()
                                   : AudioSliceLeaseSnapshot{};
        if (group.lease != nullptr &&
            lease.state == AudioSliceState::SliceInvalidated &&
            (decision.audioState == AudioGroupState::Invalidated ||
             decision.audioState ==
                 AudioGroupState::DeadlineInvalidated ||
             decision.audioState ==
                 AudioGroupState::PostStartInvalidated ||
             decision.audioState == AudioGroupState::Retired)) {
            return true;
        }
        if (group.lease == nullptr) {
            return false;
        }
        Plan03PageGroupView checked{};
        if (!decision.pages.Valid() || group.identity.sliceIndex >= decision.sliceCount ||
            !pool_->ResolvePlan03GroupSliceRt(group.identity, &checked) ||
            checked.lease != group.lease || checked.members != group.members) return false;
        TransitionInstanceSnapshot transitionSnapshot{};
        const bool transitionCaptured =
            decision.transitionGate.Valid() &&
            transitions_ != nullptr &&
            transitions_->Capture(decision.transitionGate,
                                  &transitionSnapshot);
        const bool transitionAlreadyLocked =
            transitionCaptured &&
            transitionSnapshot.state ==
                TransitionAudibleGateState::Locked;
        const bool delayedTargetLockSlice =
            group.identity.sliceIndex == 1 &&
            decision.sliceCount == 2 && group.memberCount == 1 &&
            decision.audioState == AudioGroupState::Admitted &&
            transitionCaptured &&
            transitionSnapshot.state ==
                TransitionAudibleGateState::Replaceable;
        const bool naturalEndCompletionSlice =
            group.identity.sliceIndex == 1 &&
            decision.sliceCount == 2 &&
            transitionAlreadyLocked;
        const bool naturalEndPreludeSlice =
            group.identity.sliceIndex == 0 &&
            decision.sliceCount == 2 && transitionCaptured &&
            transitionSnapshot.state ==
                TransitionAudibleGateState::Replaceable &&
            group.sourceControlCount != 0 &&
            group.sourceControlCount < group.memberCount;
        if (decision.audioState == AudioGroupState::Admitted &&
            !delayedTargetLockSlice && !naturalEndCompletionSlice) {
            return true;
        }
        ArmGroupSnapshot armGroup{};
        const bool validArmGroupState =
            decision.armGroup.Valid() &&
            armGroups_->Capture(decision.armGroup, &armGroup) &&
            (armGroup.state == ArmGroupState::Ready ||
             (delayedTargetLockSlice &&
              armGroup.state == ArmGroupState::Retiring));
        if (!validArmGroupState ||
            armGroup.parent.slot != group.identity.decision.slot ||
            armGroup.parent.generation !=
                group.identity.decision.generation ||
            armGroup.memberCount != group.memberCount ||
            armGroup.ackedCount != group.memberCount) {
            return false;
        }
        const bool naturalEndCompletionDecision =
            naturalEndCompletionSlice &&
            (decision.audioState == AudioGroupState::AudioClaimed ||
             decision.audioState == AudioGroupState::Admitted);
        if (decision.audioState != AudioGroupState::Pending &&
            !naturalEndCompletionDecision &&
            !delayedTargetLockSlice) {
            return false;
        }
        for (std::uint16_t member = 0; member < group.memberCount;
             ++member) {
            TimelineArmObservation arm{};
            if (!arms_.Observe(group.members[member], &arm)) {
                return false;
            }
            const bool completedNaturalEndSource =
                naturalEndCompletionSlice &&
                member < group.sourceControlCount &&
                arm.memberIndex == member &&
                arm.state == ArmState::AudioClaimed;
            if ((arm.state != ArmState::Published &&
                 !completedNaturalEndSource) ||
                arm.parentGroup.decision.slot !=
                    group.identity.decision.slot ||
                arm.parentGroup.decision.generation !=
                    group.identity.decision.generation ||
                (!completedNaturalEndSource &&
                 !BindingValid(group.members[member].renderSlot,
                               group.members[member].renderGeneration))) {
                return false;
            }
        }
        std::uint32_t participantCount = 0;
        if (!decisions_->CopyCommittedParticipants(
                group.identity.decision, participants_.data(),
                static_cast<std::uint32_t>(participants_.size()),
                &participantCount)) {
            return false;
        }
        QueueInstanceToken queue{};
        VariableControlInstanceToken variableControl{};
        for (std::uint32_t participant = 0;
             participant < participantCount; ++participant) {
            if (participants_[participant].kind ==
                DecisionParticipantKind::QueueInstance) {
                if (queue.Valid()) return false;
                queue = {participants_[participant].slot,
                         participants_[participant].generation};
            } else if (participants_[participant].kind ==
                       DecisionParticipantKind::Variable) {
                if (variableControl.Valid()) return false;
                variableControl = {
                    participants_[participant].slot,
                    participants_[participant].generation};
            }
        }
        if (queue.Valid() == variableControl.Valid()) return false;
        if (variableControl.Valid()) {
            const bool variableNaturalEndCompletion =
                naturalEndCompletionSlice &&
                decision.audioState == AudioGroupState::AudioClaimed;
            if (decision.audioState != AudioGroupState::Pending &&
                !variableNaturalEndCompletion &&
                !delayedTargetLockSlice) {
                return false;
            }
            if (variableControls_ == nullptr || group.memberCount == 0) {
                return false;
            }
            VariableControlSliceContext context{};
            context.sink = this;
            context.lease = group.lease;
            context.instance = variableControl;
            context.decision = group.identity.decision;
            context.members = group.members;
            context.memberCount = group.memberCount;
            context.sourceControlCount = group.sourceControlCount;
            context.quantumIndex = quantumIndex;
            context.transition = decision.transitionGate;
            context.naturalEndPrelude = naturalEndPreludeSlice;
            context.naturalEndCompletion =
                variableNaturalEndCompletion;
            AudioSliceOperation operation{};
            operation.context = &context;
            operation.validate = &ValidateVariableControlSlice;
            operation.afterClaim = &AfterVariableControlSliceClaim;
            operation.apply = &ApplyVariableControlSlice;
            const AudioSliceApplyStatus applied =
                ApplyAudioSliceWhole(*group.lease, &operation, 1);
            if (applied == AudioSliceApplyStatus::ValidationFailed) {
                (void)decisions_->Invalidate(context.decision);
                (void)group.lease->Invalidate(
                    AudioSliceInvalidateReason::Identity);
                return true;
            }
            if ((applied != AudioSliceApplyStatus::Applied &&
                 applied != AudioSliceApplyStatus::
                                AppliedWithDeferredInvalidation) ||
                !context.applied) {
                if (applied == AudioSliceApplyStatus::SliceUnavailable &&
                    group.lease->Capture().state ==
                        AudioSliceState::SliceInvalidated) {
                    return true;
                }
                return false;
            }
            if (!decisions_->RecordAppliedSlice(context.decision, decision.pages,
                    group.identity.sliceIndex, quantumIndex + 1u)) return false;
            if (applied == AudioSliceApplyStatus::
                               AppliedWithDeferredInvalidation) {
                AudioSliceInvalidateReason reason =
                    AudioSliceInvalidateReason::Cancel;
                if (!group.lease->ConsumePostSliceInvalidation(
                        &reason)) {
                    return false;
                }
                if (reason == AudioSliceInvalidateReason::Deadline) {
                    (void)decisions_->InvalidateForDeadline(
                        context.decision);
                } else {
                    (void)decisions_->Invalidate(context.decision);
                }
            }
            return true;
        }
        const bool queueNaturalEndCompletion =
            naturalEndCompletionSlice &&
            decision.audioState == AudioGroupState::AudioClaimed;
        if (decision.audioState != AudioGroupState::Pending &&
            !queueNaturalEndCompletion && !delayedTargetLockSlice) {
            return false;
        }
        QueueInstanceSnapshot queueSnapshot{};
        ConditionalQueueVersionToken conditional{};
        if (queues_ == nullptr ||
            (!delayedTargetLockSlice &&
             (occurrences_ == nullptr ||
              !queues_->Capture(queue, &queueSnapshot) ||
              !queues_->ResolveConditionalVersion(queue, group.identity.decision, &conditional)))) {
            return false;
        }
        QueueSliceContext context{};
        context.sink = this;
        context.queue = queue;
        context.version = conditional;
        context.quantumIndex = quantumIndex;
        context.decision = group.identity.decision;
        context.transition = decision.transitionGate;
        context.members = group.members;
        context.memberCount = group.memberCount;
        context.sourceControlCount = group.sourceControlCount;
        context.sliceIndex = group.identity.sliceIndex;
        context.sliceCount = decision.sliceCount;
        context.delayedTargetLock = delayedTargetLockSlice;
        AudioSliceOperation operation{};
        operation.context = &context;
        operation.validate = &ValidateQueueSlice;
        operation.afterClaim = &AfterQueueSliceClaim;
        operation.apply = &ApplyQueueSlice;
        const AudioSliceApplyStatus applied =
            ApplyAudioSliceWhole(*group.lease, &operation, 1);
        if (applied == AudioSliceApplyStatus::ValidationFailed) {
            if (context.delayedTargetLock) return false;
            FailQueueSlice(context);
            return true;
        }
        if ((applied != AudioSliceApplyStatus::Applied &&
             applied != AudioSliceApplyStatus::
                            AppliedWithDeferredInvalidation) ||
            !context.applied) {
            if (applied == AudioSliceApplyStatus::SliceUnavailable &&
                group.lease->Capture().state ==
                    AudioSliceState::SliceInvalidated) {
                return true;
            }
            return false;
        }
        if (!decisions_->RecordAppliedSlice(context.decision, decision.pages,
                group.identity.sliceIndex, quantumIndex + 1u)) return false;
#if !defined(HARKEN_RT_QUALIFICATION)
        if (queueSliceAppliedHook_ != nullptr)
            queueSliceAppliedHook_(queueSliceAppliedHookContext_, group.identity.sliceIndex);
#endif
        return true;
    }

    bool ApplyQueueLaneBucket(ImmediateActionLane& lane, std::uint32_t slot,
        const ImmediateLaneBucket* bucket, std::uint64_t quantum, bool* claimed) noexcept {
        const auto* owner = lane.CapturedQueueContinuation(slot, bucket);
        if (claimed == nullptr || owner == nullptr || !owner->Uses(pool_, queues_, decisions_) ||
            blockFrames_ == 0 || quantum >= UINT64_MAX / blockFrames_) return false;
        const auto start = quantum * blockFrames_;
        std::uint64_t delay = 0;
        if (owner->TimingForQuantum(start, start + blockFrames_, &delay) !=
            QueueConditionalProofStatus::ReadyToActivate) return false;
        const auto* events = lane.Events(slot, bucket);
        const auto* groups = lane.Groups(slot, bucket);
        const ImmediateLaneHandle handle{slot, bucket->attemptGeneration};
        if ((bucket->eventCount != 0 && events == nullptr) ||
            (bucket->groupCount != 0 && groups == nullptr)) return false;
        const auto sameArm = [](TimelineArmToken a, TimelineArmToken b) noexcept {
            return a.slot == b.slot && a.generation == b.generation &&
                a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
        };
        // Validate every member before the Queue's existing whole-slice claim.
        for (std::uint32_t index = 0; index < bucket->groupCount; ++index) {
            Plan03PageGroupView group{};
            if (!pool_->ResolvePlan03GroupSliceRt(groups[index].identity, &group) || !owner->Owns(group)) return false;
        }
        for (std::uint32_t index = 0; index < bucket->eventCount; ++index) {
            const auto& event = events[index].event;
            const auto* program = event.program.program;
            TimelineArmObservation arm{};
            TimelineSemanticTarget target{};
            ArmState state{};
            const auto decision = owner->Decision();
            if (!event.frozenTxnValid || event.frontier.Present() || events[index].futureGroupedStart ||
                event.executeFrame > UINT64_MAX - delay || event.executeFrame + delay < start ||
                event.executeFrame + delay - start >= blockFrames_ ||
                program == nullptr || program->role != TimelineProgramRole::Voice || program->payloads == nullptr ||
                program->generation != event.program.programGeneration || event.payload >= program->payloadCount ||
                lane.CapturedArmReader(handle, bucket, bucket->firstEvent + index) == nullptr ||
                !arms_.Observe(event.armToken, &arm) ||
                !arms_.Snapshot(event.armToken, &state, &target) ||
                arm.parentGroup.decision.slot != decision.slot || arm.parentGroup.decision.generation != decision.generation ||
                (state != ArmState::Published && state != ArmState::AudioClaimed && state != ArmState::Started)) return false;
            const auto& payload = program->payloads[event.payload];
            if (!(payload.target == target) ||
                (payload.kind != TimelineEventKind::Start && payload.kind != TimelineEventKind::Envelope &&
                 payload.kind != TimelineEventKind::Stop) ||
                event.subeventPhase != (payload.kind == TimelineEventKind::Start ? 20u :
                    payload.kind == TimelineEventKind::Envelope ? 30u : 50u)) return false;
            std::uint32_t count = 0;
            for (std::uint32_t other = 0; other < bucket->eventCount; ++other) {
                const auto token = events[other].event.armToken;
                count += token.renderSlot == event.armToken.renderSlot &&
                    token.renderGeneration == event.armToken.renderGeneration ? 1u : 0u;
            }
            if (!voices_.CanAppendTimelineEvents(event.armToken.renderSlot, event.armToken.renderGeneration, quantum, count))
                return false;
            if (payload.kind == TimelineEventKind::Start &&
                voices_.TimelineStartSourcesStatus(arms_, event.armToken) != VariableReadCutStatus::Applied) return false;
        }
        if (!owner->BeforeAudioWrite()) return false;
        *claimed = lane.ClaimRetainingCaptureOnRefusal(handle, bucket);
        if (!*claimed) return false;
        for (std::uint32_t index = 0; index < bucket->groupCount; ++index) {
            Plan03PageGroupView group{};
            if (!pool_->ResolvePlan03GroupSliceRt(groups[index].identity, &group) ||
                !owner->Owns(group) || !ApplyNativeGroup(group, quantum)) return false;
        }
        for (std::uint32_t index = 0; index < bucket->eventCount; ++index) {
            const auto& event = events[index].event;
            bool prior = false;
            for (std::uint32_t other = 0; other < index; ++other)
                prior = prior || sameArm(event.armToken, events[other].event.armToken);
            if (prior) continue;
            DecisionCommittedSnapshot decision{};
            TimelineArmObservation arm{};
            if (!decisions_->CaptureCommitted(owner->Decision(), &decision) || !arms_.Observe(event.armToken, &arm)) return false;
            if (decision.authoredStopInvalidated) continue;
            const bool admitted = decision.audioState == AudioGroupState::Admitted ||
                (arm.state == ArmState::AudioClaimed &&
                 (decision.audioState == AudioGroupState::AudioClaimed ||
                  decision.audioState == AudioGroupState::PostStartInvalidated ||
                  decision.audioState == AudioGroupState::PostStartDeadlineInvalidated));
            if (!admitted) continue;
            std::array<TimelineSinkEvent, kTimelineMaxEventsPerVoiceQuantum> due{};
            std::uint32_t count = 0;
            for (std::uint32_t other = index; other < bucket->eventCount; ++other) {
                const auto& input = events[other].event;
                if (!sameArm(event.armToken, input.armToken)) continue;
                if (count == due.size()) return false;
                const auto* program = input.program.program;
                const auto* payload = &program->payloads[input.payload];
                due[count++] = {static_cast<std::uint32_t>(input.executeFrame + delay - start), payload,
                    input.authoredFadeOutCandidate,
                    {input.executeFrame + delay, input.logicCommitSequence, input.executionOrdinal, input.eventIndex,
                        input.transactionOrdinal, input.subeventPhase, input.rowSequence}};
                if (program->readFence != nullptr) program->readFence->RecordRead(quantum);
            }
            if (!ApplyTransaction(event.armToken.renderSlot, event.armToken.renderGeneration,
                    quantum, due.data(), count, event.armToken)) return false;
            FinishPendingVariableStarts(quantum, event.armToken.renderSlot);
        }
        return true;
    }

    bool BindingValid(std::uint32_t renderSlot,
                      std::uint32_t renderGeneration) const noexcept override {
        return voices_.TimelineBindingValid(renderSlot, renderGeneration);
    }

    bool ApplyTransaction(std::uint32_t renderSlot,
                          std::uint32_t renderGeneration,
                          std::uint64_t quantumIndex,
                          const TimelineSinkEvent* events,
                          std::uint32_t eventCount,
                          TimelineArmToken armToken) noexcept override {
        if (events == nullptr || eventCount == 0 || renderSlot >= pendingVariableStarts_.size() ||
            eventCount > kTimelineMaxEventsPerVoiceQuantum ||
            !voices_.CanAppendTimelineEvents(renderSlot, renderGeneration,
                                             quantumIndex, eventCount)) {
            return false;
        }
        bool hasStart = false;
        for (std::uint32_t i = 0; i < eventCount; ++i)
            hasStart = hasStart || events[i].payload->kind == TimelineEventKind::Start;
        const bool publicationRefused = hasStart && !arms_.SourcePublicationAccepted(armToken);
        if (renderSlot < pendingVariableStarts_.size() &&
            (pendingVariableStarts_[renderSlot].count != 0 ||
             publicationRefused ||
             (hasStart && voices_.HasTimelineVariableSourceRead(renderSlot, renderGeneration)))) {
            auto& pending = pendingVariableStarts_[renderSlot];
            if (pending.count != 0 && pending.arm.renderGeneration != renderGeneration) return false;
            const auto pendingEvents = voices_.PendingTimelineVariableEvents(renderSlot);
            if (!pending.failed && !publicationRefused) {
                assert(pendingEvents.size() == kTimelineMaxEventsPerVoiceQuantum);
                if (pendingEvents.size() != kTimelineMaxEventsPerVoiceQuantum) return false;
            }
            if (pending.count == 0) {
                pending.arm = armToken;
                pending.source = voices_.TimelineVariableSourceRead(renderSlot, renderGeneration);
            }
            std::uint32_t transactionIndex = 0;
            while (transactionIndex < pending.transactionCount &&
                !SamePlainArm(pending.transactions[transactionIndex].arm, armToken)) ++transactionIndex;
            if (transactionIndex == pending.transactions.size()) return false;
            if (transactionIndex == pending.transactionCount) {
                pending.transactions[transactionIndex].arm = armToken;
                ++pending.transactionCount;
            }
            auto& transaction = pending.transactions[transactionIndex];
            transaction.starts = transaction.starts || hasStart;
            if (pending.failed || eventCount > kTimelineMaxEventsPerVoiceQuantum - pending.count) {
                pending.failed = true;
                RecordPlainResult(armToken, pending.failureDisposition);
                (void)CancelBinding(renderSlot, renderGeneration);
                return true;
            }
            for (std::uint32_t i = 0; i < eventCount; ++i) {
                const auto& payload = *events[i].payload;
                // Refused publication is terminal before any later payload reader.
                if (!publicationRefused) {
                    auto& stored = pendingEvents[pending.count + i];
                    stored = VoiceTimelineEvent{payload.kind};
                    stored.authoredRootStop = payload.kind == TimelineEventKind::Stop &&
                        (payload.reserved & kTimelineEventAuthoredRootStop) != 0;
                    if (payload.kind == TimelineEventKind::Stop) {
                        stored.Stop().authoredStopOperation = payload.authoredStopOperation;
                        stored.Stop().controlCause = payload.controlCause;
                    }
                    stored.offset = static_cast<std::uint16_t>(events[i].effectiveOffset);
                    stored.envelopeDirection = payload.envelopeDirection;
                    stored.authoredFadeOutCandidate = events[i].authoredFadeOutCandidate;
                    stored.frame = payload.envelopeStartFrame;
                    stored.endFrame = payload.envelopeEndFrame;
                    stored.value = payload.sourceStartFrame;
                    stored.envelopeCurve = payload.envelopeCurve;
                    if (payload.kind == TimelineEventKind::VariableWeight) {
                        stored.Weight().variableWeight = payload.variableWeight;
                        stored.Weight().variableWeightOwner = payload.variableWeightOwner;
                    }
                    stored.order = events[i].order;
                }
                transaction.transient = transaction.transient ||
                    payload.kind == TimelineEventKind::VariableWeight ||
                    (payload.kind == TimelineEventKind::Stop &&
                        (payload.reserved & kTimelineEventTransientControlArm) != 0) ||
                    (payload.kind == TimelineEventKind::Start &&
                        (payload.reserved & kTimelineEventTransientStartArm) != 0);
            }
            pendingVariableStartSeen_ = true;
            pending.count += eventCount;
            if (publicationRefused) {
                // Scheduled pages retain their absolute time; a missed permit
                // is a normal start failure, never a protocol error or re-anchor.
                pending.failed = true;
                RecordPlainResult(armToken, pending.failureDisposition);
                (void)CancelBinding(renderSlot, renderGeneration);
                return true;
            }
            RecordPlainResult(armToken, kTimelineDispositionArmPending);
            return true;
        }
        bool variableWeightControl = false;
        bool transientStartControl = false;
        bool transientControl = false;
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            const TimelineEventPayload& payload = *events[i].payload;
            VoiceTimelineEvent event{payload.kind};
            event.authoredRootStop = payload.kind == TimelineEventKind::Stop &&
                (payload.reserved & kTimelineEventAuthoredRootStop) != 0;
            event.offset = static_cast<std::uint16_t>(events[i].effectiveOffset);
            event.envelopeDirection = payload.envelopeDirection;
            event.authoredFadeOutCandidate =
                events[i].authoredFadeOutCandidate;
            event.frame = payload.envelopeStartFrame;
            event.endFrame = payload.envelopeEndFrame;
            event.value = payload.sourceStartFrame;
            event.envelopeCurve = payload.envelopeCurve;
            if (payload.kind == TimelineEventKind::VariableWeight) {
                event.Weight().variableWeight = payload.variableWeight;
                event.Weight().variableWeightOwner = payload.variableWeightOwner;
            }
            event.order = events[i].order;
            if (payload.kind == TimelineEventKind::Stop) {
                event.Stop().authoredStopOperation = payload.authoredStopOperation;
                event.Stop().controlCause = payload.controlCause;
            }
            if (!voices_.AppendTimelineEvent(renderSlot, renderGeneration,
                                             quantumIndex, event)) {
                return false;
            }
            if (payload.kind == TimelineEventKind::Start) {
                const std::uint64_t blockSequence =
                    quantumIndex == UINT64_MAX ? UINT64_MAX
                                               : quantumIndex + 1u;
                if (!arms_.Start(armToken, blockSequence, quantumIndex * blockFrames_ + events[i].effectiveOffset)) {
                    return false;
                }
                voices_.FinishTimelineVariableSourceRead(renderSlot, renderGeneration);
                transientStartControl = transientStartControl ||
                    (payload.reserved &
                     kTimelineEventTransientStartArm) != 0;
            } else if (payload.kind ==
                       TimelineEventKind::VariableWeight) {
                variableWeightControl = true;
            } else if (payload.kind == TimelineEventKind::Stop) {
                transientControl = transientControl ||
                    (payload.reserved &
                     kTimelineEventTransientControlArm) != 0;
            }
        }
        if (variableWeightControl || transientStartControl ||
            transientControl) {
            const std::uint64_t blockSequence =
                quantumIndex == UINT64_MAX ? UINT64_MAX
                                           : quantumIndex + 1u;
            if (((variableWeightControl || transientControl) &&
                 !transientStartControl &&
                 !arms_.Start(armToken, blockSequence)) ||
                !arms_.AcknowledgeLogicalComplete(
                    armToken, blockSequence) ||
                !arms_.Tombstone(armToken, blockSequence)) {
                return false;
            }
        }
        return true;
    }

    bool CancelBinding(std::uint32_t renderSlot,
                       std::uint32_t renderGeneration) noexcept override {
        const bool cancelled = voices_.CancelTimelineBindingFromAudio(renderSlot, renderGeneration);
        if (renderSlot < pendingVariableStarts_.size() &&
            pendingVariableStarts_[renderSlot].count != 0 &&
            pendingVariableStarts_[renderSlot].arm.renderGeneration == renderGeneration) {
            auto& pending = pendingVariableStarts_[renderSlot];
            pending.failed = true;
            if (cancelled) RetirePendingVariableStart(pending);
        }
        return cancelled;
    }

    void FailPlan03Deadline(DecisionToken decision,
                            std::uint64_t quantumIndex) noexcept override {
        (void)decision;
        (void)quantumIndex;
    }

    bool ApplyAuthoredStopFence(const AuthoredStopFenceDescriptor& descriptor,
                               std::uint64_t quantumIndex) noexcept override {
        if (blockFrames_ == 0) return false;
        if (!descriptor.Active() || descriptor.identity.stopFrame / blockFrames_ > quantumIndex) {
            return true;
        }
        if (!descriptor.UsesDecisionTable(decisions_)) return true;
        for (std::uint32_t i = 0; i < authoredStopFenceCount_; ++i) {
            const auto& prior = *authoredStopFences_[i];
            if (prior.operation == descriptor.operation && prior.identity == descriptor.identity &&
                prior.decision.slot == descriptor.decision.slot &&
                prior.decision.generation == descriptor.decision.generation) return true;
        }
        if (authoredStopFenceCount_ >= authoredStopFences_.size()) return false;
        authoredStopFences_[authoredStopFenceCount_++] = &descriptor;
        return true;
    }

    using CurrentSliceApplyCallback = void (*)(void*, std::uint64_t,
        const TimelineExecutionResult&) noexcept;
    void SetCurrentSliceApplyCallback(void* context, CurrentSliceApplyCallback callback) noexcept {
        currentSliceApplyContext_ = context;
        currentSliceApplyCallback_ = callback;
    }
    void BeforeEndQuantum(std::uint64_t quantumIndex,
                          const TimelineExecutionResult& result) noexcept override {
        if (currentSliceApplyCallback_ != nullptr)
            currentSliceApplyCallback_(currentSliceApplyContext_, quantumIndex, result);
        FinishPendingVariableStarts(quantumIndex);
    }
    void EndQuantum(std::uint64_t quantumIndex) noexcept override {
        immediate_.Apply(quantumIndex);
        frontier_.Finish();
        // All current voice spans are already copied; only future slices observe cancellation.
        for (std::uint32_t i = 0; i < authoredStopFenceCount_; ++i) {
            const auto& fence = *authoredStopFences_[i];
            if (fence.decision.Valid() && fence.UsesDecisionTable(decisions_) && fence.TryBeginApply()) {
                if (fence.operation->Active(fence.identity))
                    (void)decisions_->InvalidateForAuthoredStop(fence.decision);
                fence.FinishApply();
            }
        }
        for (std::uint32_t i = 0; i < authoredStopFenceCount_; ++i) {
            const auto& fence = *authoredStopFences_[i];
            if (!fence.decision.Valid() && fence.TryBeginApply()) {
                fence.operation->PublishApplied(fence.identity,
                    quantumIndex == UINT64_MAX ? UINT64_MAX : quantumIndex + 1u);
                fence.FinishApply();
            }
        }
        authoredStopFenceCount_ = 0;
        voices_.FinalizeTimelineQuantum(quantumIndex);
        immediate_.Finish();
        if (queues_ != nullptr) queues_->CompleteAudioConditionalReads();
    }

private:
    friend struct detail::VoiceTimelineSinkTestAccess;
    friend struct detail::QueueCancellationRaceTestAccess;

    struct PendingVariableTransaction {
        TimelineArmToken arm{};
        bool starts = false;
        bool transient = false;
    };
    struct PlainTransactionResult {
        TimelineArmToken arm{};
        std::uint8_t disposition = kTimelineDispositionArmPending;
    };
    static bool SamePlainArm(TimelineArmToken a, TimelineArmToken b) noexcept {
        return a.slot == b.slot && a.generation == b.generation &&
            a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
    }
    void RecordPlainResult(TimelineArmToken arm, std::uint8_t disposition) noexcept {
        for (std::uint32_t i = 0; i < plainResultCount_; ++i) {
            if (!SamePlainArm(plainResults_[i].arm, arm)) continue;
            plainResults_[i].disposition = disposition;
            return;
        }
        assert(plainResultCount_ < plainResults_.size());
        if (plainResultCount_ < plainResults_.size()) plainResults_[plainResultCount_++] = {arm, disposition};
    }
    struct PendingVariableStart {
        TimelineArmToken arm{};
        VariableTimelineReadUseRef source{};
        std::uint32_t count = 0;
        std::uint32_t transactionCount = 0;
        bool failed = false;
        std::uint8_t failureDisposition = kTimelineDispositionArmFailed;
        std::array<PendingVariableTransaction, kTimelineMaxEventsPerVoiceQuantum + 1u> transactions{};
    };

    void RetirePendingVariableStart(PendingVariableStart& pending) noexcept {
        pending.failed = true;
        // A busy Arm owns its retirement independently of the reusable native slot.
        for (std::uint32_t i = 0; i < pending.transactionCount; ++i) {
            const auto arm = pending.transactions[i].arm;
            if (arm.slot >= retiringPlainArms_.size()) return;
            const auto prior = retiringPlainArms_[arm.slot];
            if (prior.Valid() && !SamePlainArm(prior, arm)) return;
        }
        for (std::uint32_t i = 0; i < pending.transactionCount; ++i) {
            const auto arm = pending.transactions[i].arm;
            RecordPlainResult(arm, pending.failureDisposition);
            if (TryRetirePlainArm(arm)) continue;
            if (!retiringPlainArms_[arm.slot].Valid()) {
                assert(retiringPlainArmCount_ < retiringPlainArmSlots_.size());
                retiringPlainArmSlots_[retiringPlainArmCount_++] = arm.slot;
                retiringPlainArms_[arm.slot] = arm;
            }
        }
        pending = {};
    }

    bool TryRetirePlainArm(TimelineArmToken arm) noexcept {
        TimelineArmObservation observation{};
        if (!arms_.Observe(arm, &observation) || observation.state == ArmState::Tombstoned) return true;
        const bool cancelled = observation.state == ArmState::Started
            ? arms_.RequestCancelStartedVoice(arm)
            : observation.state == ArmState::AudioClaimed
                ? arms_.RequestCancelAppliedAudioSlice(arm) : arms_.RequestCancel(arm);
        const auto sequence = plainQuantum_ == UINT64_MAX ? UINT64_MAX : plainQuantum_ + 1u;
        return cancelled && arms_.Tombstone(arm, sequence);
    }

    void RetryRetiringPlainArms() noexcept {
        for (std::uint32_t i = 0; i < retiringPlainArmCount_;) {
            const auto slot = retiringPlainArmSlots_[i];
            if (!TryRetirePlainArm(retiringPlainArms_[slot])) { ++i; continue; }
            retiringPlainArms_[slot] = {};
            retiringPlainArmSlots_[i] = retiringPlainArmSlots_[--retiringPlainArmCount_];
        }
    }

    void FinishPendingVariableStarts(std::uint64_t quantumIndex, std::uint32_t onlySlot = UINT32_MAX) noexcept {
        if (!pendingVariableStartSeen_) return;
        const auto sequence = quantumIndex == UINT64_MAX ? UINT64_MAX : quantumIndex + 1u;
        const auto begin = onlySlot == UINT32_MAX ? 0u : onlySlot;
        const auto end = onlySlot == UINT32_MAX ? pendingVariableStarts_.size() : std::min<std::size_t>(onlySlot + 1u, pendingVariableStarts_.size());
        for (std::size_t slot = begin; slot < end; ++slot) {
            auto& pending = pendingVariableStarts_[slot];
            if (pending.count == 0) continue;
            const auto arm = pending.arm;
            if (!voices_.TimelineBindingValid(arm.renderSlot, arm.renderGeneration)) {
                // A released or reused generation has already detached every audio reader.
                RetirePendingVariableStart(pending);
                continue;
            }
            if (pending.failed) {
                (void)CancelBinding(arm.renderSlot, arm.renderGeneration);
                continue;
            }
            const auto pendingEvents = voices_.PendingTimelineVariableEvents(arm.renderSlot);
            assert(pendingEvents.size() == kTimelineMaxEventsPerVoiceQuantum);
            if (pendingEvents.size() != kTimelineMaxEventsPerVoiceQuantum) {
                pending.failed = true;
                (void)CancelBinding(arm.renderSlot, arm.renderGeneration);
                continue;
            }
            TimelineArmObservation observation{};
            const bool observed = arms_.Observe(arm, &observation);
            const auto source = voices_.TimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration);
            const auto status = source.Valid() && source.slot == pending.source.slot &&
                source.generation == pending.source.generation
                ? voices_.TimelineStartSourcesStatus(arms_, arm)
                : VariableReadCutStatus::Failed;
            const bool canStart = observed && (observation.state == ArmState::Published ||
                observation.state == ArmState::AudioClaimed);
            std::uint64_t startFrame = UINT64_MAX;
            std::uint64_t stopFrame = UINT64_MAX;
            for (std::uint32_t i = 0; i < pending.count; ++i) {
                const auto& event = pendingEvents[i];
                if (event.Kind() == TimelineEventKind::Start) startFrame = std::min(startFrame, event.order.executeFrame);
                if (event.Kind() == TimelineEventKind::Stop) stopFrame = std::min(stopFrame, event.order.executeFrame);
            }
            const bool stopDominates = status != VariableReadCutStatus::Failed &&
                stopFrame != UINT64_MAX && (status == VariableReadCutStatus::Waiting ||
                    stopFrame <= std::max(startFrame, quantumIndex * blockFrames_));
            if (stopDominates) {
                pending.failureDisposition = kTimelineDispositionStopDominated;
                (void)CancelBinding(arm.renderSlot, arm.renderGeneration);
                continue;
            }
            if (canStart &&
                status == VariableReadCutStatus::Waiting) continue;
            const bool ready = canStart &&
                status == VariableReadCutStatus::Applied &&
                voices_.CanAppendTimelineEvents(arm.renderSlot, arm.renderGeneration,
                    quantumIndex, pending.count);
            if (!ready) {
                (void)CancelBinding(arm.renderSlot, arm.renderGeneration);
                continue;
            }
            std::array<TimelineArmTable::PlainAudioProof, kTimelineMaxEventsPerVoiceQuantum + 1u> proofs{};
            bool proved = true;
            bool deferred = false;
            for (std::uint32_t i = 0; i < pending.transactionCount; ++i) {
                const auto& transaction = pending.transactions[i];
                const auto disposition = arms_.ProvePlainAudioApply(transaction.arm,
                    transaction.starts, transaction.transient, sequence, &proofs[i]);
                if (disposition == TimelineArmOperationDisposition::Claimed) continue;
                proved = false;
                deferred = disposition == TimelineArmOperationDisposition::Deferred;
                break;
            }
            if (!proved) {
                for (auto& proof : proofs) proof.Release();
                if (!deferred) (void)CancelBinding(arm.renderSlot, arm.renderGeneration);
                continue;
            }
            VariableTimelineInitializerProof initializer;
            bool initialized = true;
            for (std::uint32_t i = 0; i < pending.transactionCount; ++i) {
                if (!pending.transactions[i].starts) continue;
                initialized = voices_.ProveTimelineVariableInitializer(arm.renderSlot,
                    arm.renderGeneration, proofs[i], &initializer);
                break;
            }
            if (!initialized) {
                for (auto& proof : proofs) proof.Release();
                (void)CancelBinding(arm.renderSlot, arm.renderGeneration);
                continue;
            }
            voices_.ApplyTimelineVariableInitializer(initializer);
            for (std::uint32_t i = 0; i < pending.count; ++i) {
                auto event = pendingEvents[i];
                const auto quantumStart = quantumIndex * blockFrames_;
                event.offset = static_cast<std::uint16_t>(event.order.executeFrame < quantumStart
                    ? 0 : event.order.executeFrame - quantumStart);
                const bool appended = voices_.AppendTimelineEvent(arm.renderSlot, arm.renderGeneration,
                    quantumIndex, event);
                assert(appended);
                (void)appended;
            }
            voices_.FinishTimelineVariableSourceRead(arm.renderSlot, arm.renderGeneration);
            for (std::uint32_t i = 0; i < pending.transactionCount; ++i) {
                const auto& transaction = pending.transactions[i];
                arms_.ApplyPlainAudio(proofs[i], std::max(startFrame, quantumIndex * blockFrames_));
                RecordPlainResult(transaction.arm, kTimelineDispositionApplied);
            }
            pending = {};
        }
    }

    struct VariableControlSliceContext {
        VoiceTimelineSink* sink = nullptr;
        AudioSliceLease* lease = nullptr;
        VariableControlInstanceToken instance{};
        DecisionToken decision{};
        const TimelineArmToken* members = nullptr;
        std::uint16_t memberCount = 0;
        std::uint16_t sourceControlCount = 0;
        std::uint64_t quantumIndex = 0;
        VariableControlActivationProof proof{};
        TransitionInstanceToken transition{};
        bool naturalEndPrelude = false;
        bool naturalEndCompletion = false;
        bool applied = false;
    };

    static bool ValidateVariableControlSlice(void* opaque) noexcept {
        auto& context =
            *static_cast<VariableControlSliceContext*>(opaque);
        if (context.naturalEndCompletion) {
            TransitionInstanceSnapshot transition{};
            if (context.members == nullptr ||
                context.sourceControlCount == 0 ||
                context.sourceControlCount >= context.memberCount ||
                !context.transition.Valid() ||
                context.sink->transitions_ == nullptr ||
                !context.sink->transitions_->Capture(
                    context.transition, &transition) ||
                transition.state != TransitionAudibleGateState::Locked) {
                return false;
            }
            return context.sink->variableControls_->ProveActivation(
                context.instance, context.decision, &context.proof);
        }
        return context.sink->variableControls_->ProveActivation(
                   context.instance, context.decision, &context.proof) &&
               (!context.transition.Valid() ||
                (context.sink->transitions_ != nullptr &&
                 context.sink->transitions_->CanLockFromAudioClaim(
                     context.transition)));
    }

    static void AfterVariableControlSliceClaim(void* opaque) noexcept {
#if !defined(HARKEN_RT_QUALIFICATION)
        auto& context =
            *static_cast<VariableControlSliceContext*>(opaque);
        if (context.sink->variableControlSliceClaimHook_ != nullptr) {
            context.sink->variableControlSliceClaimHook_(
                context.sink->variableControlSliceClaimHookContext_);
        }
#else
        (void)opaque;
#endif
    }

    static void ApplyVariableControlSlice(void* opaque) noexcept {
        auto& context =
            *static_cast<VariableControlSliceContext*>(opaque);
        if (context.naturalEndPrelude) {
            if (!context.sink->decisions_->TryClaimAudio(
                    context.decision)) {
                return;
            }
            context.sink->transitions_->LockFromClaimedAudioSlice(
                context.transition);
            for (std::uint16_t member = 0;
                 member < context.sourceControlCount; ++member) {
                context.sink->arms_.ApplyClaimedAudioSlice(
                    context.members[member]);
            }
            context.applied = true;
            return;
        }
        if (context.naturalEndCompletion) {
            context.sink->variableControls_->ApplyClaimedActivation(
                context.proof);
            context.applied = context.sink->decisions_->MarkAudioAdmitted(
                context.decision);
            if (!context.applied) {
                return;
            }
            for (std::uint16_t member = 0;
                 member < context.sourceControlCount; ++member) {
                context.sink->arms_.CompleteClaimedAudioSlice(
                    context.members[member], context.quantumIndex + 1u);
            }
            return;
        }
        if (context.transition.Valid()) {
            context.sink->transitions_->LockFromClaimedAudioSlice(
                context.transition);
        }
        for (std::uint16_t index = 0; index < context.memberCount;
             ++index) {
            context.sink->arms_.ApplyClaimedAudioSlice(
                context.members[index]);
        }
        context.sink->variableControls_->ApplyClaimedActivation(
            context.proof);
        context.sink->decisions_->ApplyClaimedAudioSlice(
            context.decision);
        context.applied = true;
    }

    struct QueueSliceContext {
        VoiceTimelineSink* sink = nullptr;
        QueueInstanceToken queue{};
        ConditionalQueueVersionToken version{};
        DecisionToken decision{};
        TransitionInstanceToken transition{};
        QueueConditionalProof proof{};
        QueueConditionalProofStatus proofStatus =
            QueueConditionalProofStatus::Stale;
        const TimelineArmToken* members = nullptr;
        std::uint16_t memberCount = 0;
        std::uint16_t sourceControlCount = 0;
        std::uint16_t sliceIndex = 0;
        std::uint16_t sliceCount = 0;
        std::uint64_t quantumIndex = 0;
        bool prelude = false;
        bool delayedTargetActivation = false;
        bool delayedTargetLock = false;
        bool transitionAlreadyLocked = false;
        bool naturalEndCompletion = false;
        bool applied = false;
    };

    static bool ValidateQueueSlice(void* opaque) noexcept {
        const bool valid = ValidateQueueSliceProof(opaque);
#if !defined(HARKEN_RT_QUALIFICATION)
        auto& context = *static_cast<QueueSliceContext*>(opaque);
        if (valid && context.sink->queueSliceValidatedHook_ != nullptr)
            context.sink->queueSliceValidatedHook_(context.sink->queueSliceValidatedHookContext_);
#endif
        return valid;
    }

    static bool ValidateQueueSliceProof(void* opaque) noexcept {
        auto& context = *static_cast<QueueSliceContext*>(opaque);
        const std::uint64_t blockFrames = context.sink->blockFrames_;
        if (blockFrames == 0 ||
            context.quantumIndex > UINT64_MAX / blockFrames) {
            context.proofStatus =
                QueueConditionalProofStatus::FailedProgress;
            return false;
        }
        const std::uint64_t start = context.quantumIndex * blockFrames;
        if (start > UINT64_MAX - blockFrames) {
            context.proofStatus =
                QueueConditionalProofStatus::FailedProgress;
            return false;
        }
        TransitionInstanceSnapshot transition{};
        if (context.transition.Valid()) {
            if (context.sink->transitions_ == nullptr ||
                !context.sink->transitions_->Capture(
                    context.transition, &transition)) {
                return false;
            }
            context.transitionAlreadyLocked =
                transition.state == TransitionAudibleGateState::Locked;
        }
        if (context.delayedTargetLock) {
            return context.sliceCount == 2 && context.sliceIndex == 1 &&
                context.members != nullptr && context.memberCount == 1 &&
                transition.state ==
                    TransitionAudibleGateState::Replaceable &&
                transition.audibleFrame >= start &&
                transition.audibleFrame < start + blockFrames &&
                context.sink->transitions_->CanLockFromAudioClaim(
                    context.transition);
        }
        context.proofStatus = context.sink->composites_ == nullptr
            ? context.sink->queues_->ProveConditionalForQuantum(
                context.queue, context.version,
                *context.sink->occurrences_, start,
                start + blockFrames, context.quantumIndex, true,
                &context.proof)
            : context.sink->queues_->ProveConditionalForQuantum(
                context.queue, context.version,
                *context.sink->occurrences_,
                *context.sink->composites_, start,
                start + blockFrames, context.quantumIndex, true,
                &context.proof);
        context.naturalEndCompletion =
            context.sliceCount == 2 && context.sliceIndex == 1 &&
            context.transitionAlreadyLocked &&
            context.members != nullptr &&
            context.sourceControlCount != 0 &&
            context.sourceControlCount < context.memberCount;
        const bool validPreludeProof =
            context.proofStatus == QueueConditionalProofStatus::Waiting ||
            context.proofStatus ==
                QueueConditionalProofStatus::ReadyToActivate;
        context.prelude =
            context.sliceCount == 2 && context.sliceIndex == 0 &&
            validPreludeProof && context.transition.Valid() &&
            context.members != nullptr &&
            context.sourceControlCount != 0 &&
            context.sourceControlCount < context.memberCount &&
            transition.state == TransitionAudibleGateState::Replaceable &&
            transition.audibleFrame >= start &&
            transition.audibleFrame < start + blockFrames;
        context.delayedTargetActivation =
            context.sliceCount == 2 && context.sliceIndex == 0 &&
            context.transition.Valid() && context.members != nullptr &&
            context.memberCount == 1 &&
            transition.state == TransitionAudibleGateState::Replaceable &&
            transition.audibleFrame >= start;
        if (context.prelude) {
            return context.sink->transitions_->CanLockFromAudioClaim(
                context.transition);
        }
        const bool readyInQuantum =
            context.proofStatus ==
            QueueConditionalProofStatus::ReadyToActivate;
        const bool readyRetiredSource =
            context.proofStatus ==
            QueueConditionalProofStatus::ReadyToActivateRetiredSource;
        bool retiredSourceIdentityValid = readyRetiredSource &&
            context.proof.completionMode ==
                QueueConditionalCompletionMode::RetiredSource &&
            context.proof.successorOccurrence.generation != 0 &&
            context.proof.retiredSourceRenderHandle != 0 &&
            context.proof.retiredSourceLogicalCompletionSequence != 0 &&
            context.proof.retiredSourceTombstoneAudioSequence <=
                context.proof.retiredSourceLogicalCompletionSequence &&
            context.proof.retiredSourceTombstoneAudioSequence <=
                context.quantumIndex &&
            context.proof.retiredSourceArm.renderSlot ==
                static_cast<std::uint32_t>(
                    context.proof.retiredSourceRenderHandle) &&
            context.proof.retiredSourceArm.renderGeneration ==
                static_cast<std::uint32_t>(
                    context.proof.retiredSourceRenderHandle >> 32) &&
            context.sink->occurrences_->StageOf(
                context.proof.currentOccurrence) ==
                OccurrenceStage::AwaitingEventFence &&
            context.sink->occurrences_->StageOf(
                context.proof.successorOccurrence) ==
                OccurrenceStage::Preparing &&
            !context.sink->BindingValid(
                context.proof.retiredSourceArm.renderSlot,
                context.proof.retiredSourceArm.renderGeneration) &&
            context.members != nullptr && context.sourceControlCount == 0 &&
            context.memberCount != 0;
        for (std::uint32_t index = 0; retiredSourceIdentityValid && index < context.memberCount; ++index) {
            TimelineArmObservation successorArm{};
            const auto member = context.members[index];
            retiredSourceIdentityValid =
                (context.proof.retiredSourceArm.slot != member.slot ||
                 context.proof.retiredSourceArm.generation != member.generation) &&
                context.sink->arms_.Observe(member, &successorArm) &&
                successorArm.state == ArmState::Published &&
                successorArm.parentGroup.decision.slot == context.decision.slot &&
                successorArm.parentGroup.decision.generation == context.decision.generation &&
                successorArm.memberIndex == index &&
                context.sink->BindingValid(member.renderSlot, member.renderGeneration);
        }
        return (readyInQuantum || retiredSourceIdentityValid) &&
               (!context.transition.Valid() ||
                context.transitionAlreadyLocked ||
                context.delayedTargetActivation ||
                context.sink->transitions_->CanLockFromAudioClaim(
                    context.transition));
    }

    static void ApplyQueueSlice(void* opaque) noexcept {
        auto& context = *static_cast<QueueSliceContext*>(opaque);
        if (context.delayedTargetLock) {
            context.sink->transitions_->LockFromClaimedAudioSlice(
                context.transition);
            context.applied = true;
            return;
        }
        if (context.prelude) {
            if (!context.sink->decisions_->TryClaimAudio(
                    context.decision)) {
                context.applied = false;
                return;
            }
            context.sink->transitions_->LockFromClaimedAudioSlice(
                context.transition);
            for (std::uint16_t member = 0;
                 member < context.sourceControlCount; ++member) {
                context.sink->arms_.ApplyClaimedAudioSlice(
                    context.members[member]);
            }
            context.applied = true;
            return;
        }
        context.applied =
            (context.naturalEndCompletion ||
             context.sink->decisions_->TryClaimAudio(context.decision)) &&
            context.sink->queues_->ActivateProvenConditional(
                context.proof) &&
            context.sink->decisions_->MarkAudioAdmitted(
                context.decision);
        if (context.applied && context.transition.Valid() &&
            !context.transitionAlreadyLocked &&
            !context.delayedTargetActivation) {
            context.sink->transitions_->LockFromClaimedAudioSlice(
                context.transition);
        }
        if (context.applied && context.naturalEndCompletion) {
            for (std::uint16_t member = 0;
                 member < context.sourceControlCount; ++member) {
                context.sink->arms_.CompleteClaimedAudioSlice(
                    context.members[member], context.quantumIndex + 1u);
            }
        }
    }

    static void AfterQueueSliceClaim(void* opaque) noexcept {
#if !defined(HARKEN_RT_QUALIFICATION)
        auto& context = *static_cast<QueueSliceContext*>(opaque);
        if (context.sink->queueSliceClaimHook_ != nullptr) {
            context.sink->queueSliceClaimHook_(
                context.sink->queueSliceClaimHookContext_);
        }
#else
        (void)opaque;
#endif
    }

    static void FailQueueSlice(QueueSliceContext& context) noexcept {
        const std::uint64_t frame =
            context.quantumIndex * context.sink->blockFrames_;
        if (context.proofStatus ==
            QueueConditionalProofStatus::FailedIdentity) {
            (void)context.sink->queues_->FailContainer(
                context.queue, context.version,
                ConditionalQueueActivation::InvalidatedIdentity,
                QueueFailureReason::CompletionOwnerLost, frame);
        } else if (context.proofStatus ==
                   QueueConditionalProofStatus::MissedDeadline) {
            (void)context.sink->queues_->FailContainer(
                context.queue, context.version,
                ConditionalQueueActivation::InvalidatedDeadline,
                QueueFailureReason::Deadline, frame);
        } else {
            (void)context.sink->queues_->FailContainer(
                context.queue, context.version,
                ConditionalQueueActivation::InvalidatedProgress,
                QueueFailureReason::RenderProgressLost, frame);
        }
    }

    VoicePool& voices_;
    TimelineArmTable& arms_;
    FrontierTimelineFinalizer frontier_;
    void* currentSliceApplyContext_ = nullptr;
    CurrentSliceApplyCallback currentSliceApplyCallback_ = nullptr;
    FrontierTimelineFinalizer laneFrontier_;
    ImmediateLaneTimelineFinalizer immediate_;
    StableRowStorage<PendingVariableStart> pendingVariableStarts_;
    bool pendingVariableStartSeen_ = false;
    VoicePool::SlotStorageRegistration slotStorageRegistration_;
    std::vector<PlainTransactionResult> plainResults_;
    std::vector<TimelineArmToken> retiringPlainArms_;
    std::vector<std::uint32_t> retiringPlainArmSlots_;
    std::uint32_t retiringPlainArmCount_ = 0;
    std::uint32_t plainResultCount_ = 0;
    std::uint64_t plainQuantum_ = 0;
    DecisionTable* decisions_ = nullptr;
    const ScheduledTransactionPool* pool_ = nullptr;
    mutable std::vector<Plan03PageGroupView> pageGroups_;
    mutable std::vector<Plan03ScratchDecisionState> currentSlices_;
    QueueInstanceTable* queues_ = nullptr;
    VariableControlInstanceTable* variableControls_ = nullptr;
    TransitionInstancePool* transitions_ = nullptr;
    const ArmGroupTable* armGroups_ = nullptr;
    const OccurrenceStatePool* occurrences_ = nullptr;
    const CompositeCompletionOwnerPool* composites_ = nullptr;
    std::vector<DecisionParticipantRef> participants_;
    std::vector<const AuthoredStopFenceDescriptor*> authoredStopFences_;
    std::uint32_t authoredStopFenceCount_ = 0;
    void (*variableControlSliceClaimHook_)(void*) = nullptr;
    void* variableControlSliceClaimHookContext_ = nullptr;
    void (*queueSliceClaimHook_)(void*) = nullptr;
    void (*queueSliceValidatedHook_)(void*) = nullptr;
    void (*queueSliceAppliedHook_)(void*, std::uint16_t) = nullptr;
    void* queueSliceAppliedHookContext_ = nullptr;
    void* queueSliceValidatedHookContext_ = nullptr;
    void* queueSliceClaimHookContext_ = nullptr;
    std::uint32_t arenaAvailableBytes_ = UINT32_MAX;
    mutable bool arenaCapacityRefused_ = false;
    std::uint32_t blockFrames_ = 1;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    FrontierEntryReceiptRef watchedFrontierEntryForTest_{};
    std::uint64_t watchedFrontierStageCountForTest_ = 0;
    std::array<FrontierStageRecordForTest, 8>
        watchedFrontierStagesForTest_{};
#endif
};

} // namespace harken

#endif // HARKEN_RENDER_VOICE_TIMELINE_SINK_H
