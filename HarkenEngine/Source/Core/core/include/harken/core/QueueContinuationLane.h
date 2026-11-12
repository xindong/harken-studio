#ifndef HARKEN_CORE_QUEUE_CONTINUATION_LANE_H
#define HARKEN_CORE_QUEUE_CONTINUATION_LANE_H

#include "harken/core/ImmediateActionLane.h"
#include "harken/core/QueueRuntime.h"

namespace harken {

// One bounded continuation owns its native Queue, group and Lane lifetimes.
// The initial Action and selected-replacement domains retain their own rules.
class QueueContinuationLane {
    friend class OriginalConditionalReplacementBatch;
    friend class QueueInstanceTable;
public:
    QueueContinuationLane() = default;
    QueueContinuationLane(const QueueContinuationLane&) = delete;
    QueueContinuationLane& operator=(const QueueContinuationLane&) = delete;
    ~QueueContinuationLane() { assert(lane_ == nullptr); }

    bool Begin(QueueInstanceTable& queues, QueueInstanceToken queue,
        const QueueInstanceTable::OriginalLifetimePin& original,
        DecisionTable& decisions, DecisionToken decision, ImmediateActionLane& lane,
        ScheduledTransactionPool& pool, std::uint32_t events) noexcept {
        QueueInstanceSnapshot snapshot{};
        if (lane_ != nullptr || !queues.IsOriginalLifetimePinFor(original, queue) ||
            !queues.Capture(queue, &snapshot) || snapshot.rootInstance == 0 ||
            !decisions.IsOpen(decision) || pool.nextSelectedFutureDomain_ == UINT64_MAX ||
            events > UINT32_MAX - 2u || !original.Borrow(&queueLifetime_)) return false;
        root_ = snapshot.rootInstance;
        domain_ = ++pool.nextSelectedFutureDomain_;
        const ImmediateLaneDemand demand{events + 2u, 2u, 0u, events + 2u};
        if (!lane.Reserve(root_, demand, &handle_, &originalLane_)) {
            queueLifetime_.Release();
            return false;
        }
        lane_ = &lane;
        pool_ = &pool;
        queues_ = &queues;
        decisions_ = &decisions;
        queue_ = queue;
        decision_ = decision;
        admission_.store(0, std::memory_order_relaxed);
        lane.Resolve(handle_)->queueContinuation = this;
        return true;
    }

    bool Active() const noexcept { return lane_ != nullptr; }
    bool Published() const noexcept { return published_; }
    ImmediateLaneHandle Handle() const noexcept { return handle_; }
    DecisionToken Decision() const noexcept { return decision_; }
    bool SourceReady() const noexcept {
        return !Active() || queues_->ConditionalSourceReady(queue_, conditional_);
    }
    QueueConditionalProofStatus TimingForQuantum(std::uint64_t start, std::uint64_t end,
        std::uint64_t* delay) const noexcept {
        if (!Active()) return QueueConditionalProofStatus::FailedIdentity;
        const auto status = queues_->CaptureConditionalSourceDelay(queue_, conditional_, delay);
        if (status != QueueConditionalProofStatus::ReadyToActivate || *delay == 0) return status;
        const auto* attempt = lane_->Resolve(handle_);
        if (attempt == nullptr || attempt->eventCount != 1 || groupCount_ != 1)
            return QueueConditionalProofStatus::FailedIdentity;
        const auto frame = lane_->EventBase(handle_.slot)[0].event.executeFrame;
        if (frame > UINT64_MAX - *delay) return QueueConditionalProofStatus::FailedIdentity;
        if (frame + *delay >= end) return QueueConditionalProofStatus::Waiting;
        return frame + *delay < start ? QueueConditionalProofStatus::MissedDeadline
                                     : QueueConditionalProofStatus::ReadyToActivate;
    }
    Plan03GroupSliceIdentity GroupIdentity(std::uint32_t slice) const noexcept {
        for (std::uint32_t i = 0; i < groupCount_; ++i)
            if (groups_[i].sliceIndex == slice) return groups_[i];
        return {};
    }
    bool MatchesConditional(const QueueInstanceTable& queues, QueueInstanceToken queue,
        ConditionalQueueVersionToken conditional, DecisionToken decision) const noexcept {
        return Active() && queues_ == &queues && queueLifetime_.Valid() &&
            queue.slot == queue_.slot && queue.generation == queue_.generation &&
            conditional.version == conditional_.version && decision.slot == decision_.slot &&
            decision.generation == decision_.generation &&
            queues.ProvePendingConditionalIdentity(queue, conditional, decision);
    }
    bool Replaceable() const noexcept {
        if (!Active() || !MatchesConditional(*queues_, queue_, conditional_, decision_) ||
            (admission_.load(std::memory_order_acquire) & (kWritten | kWithdrawn)) != 0) return false;
        DecisionCommittedSnapshot decision{};
        if (decisions_->CaptureCommitted(decision_, &decision)) {
            if (decision.audioState != AudioGroupState::Pending || decision.authoredStopInvalidated ||
                decision.consumedSlices != 0) return false;
        } else if (!decisions_->IsOpen(decision_)) return false;
        for (std::uint32_t i = 0; i < groupCount_; ++i) {
            Plan03PageGroupView group{};
            if (!pool_->ResolvePlan03GroupSliceProducer(groups_[i], &group) || !Owns(group) ||
                group.lease->Capture().state != AudioSliceState::Ready) return false;
        }
        return true;
    }

    bool ReserveGroup(const Plan03GroupSliceInput& input,
        const TimelineArmTable& arms) noexcept {
        if (!Active() || placed_ || prepared_ || groupCount_ == groups_.size() ||
            input.decision.slot != decision_.slot || input.decision.generation != decision_.generation ||
            !pool_->ReserveImmediateLaneGroupInDomain(root_, 0, input, arms,
                &groups_[groupCount_], nullptr, domain_)) return false;
        const auto identity = groups_[groupCount_++];
        return pool_->BindImmediateLaneGroupOwnerInDomain(identity, *lane_, root_,
                   handle_.slot, handle_.generation, domain_) &&
            lane_->DeclareGroup(handle_, {identity, input.executeFrame});
    }

    bool Prepare(const ImmediateLaneEvent* events, std::uint32_t count,
        TimelineArmTable& arms, ConditionalQueueVersionToken conditional) noexcept {
        return Active() && queues_->ProvePendingConditionalIdentity(queue_, conditional, decision_) &&
            PrepareLayout(events, count, arms, conditional);
    }

    bool PrepareReplacement(const ImmediateLaneEvent* events, std::uint32_t count,
        TimelineArmTable& arms, const PreparedQueueConditionalReplacementV1& replacement) noexcept {
        return replacement.Valid() && replacement.owner_ == queues_ && replacement.stagedOwner_ != nullptr &&
            replacement.stagedOwner_->MatchesConditional(*queues_, replacement.queue_,
                replacement.expectedVersion_, replacement.expectedDecision_) &&
            replacement.queue_.slot == queue_.slot && replacement.queue_.generation == queue_.generation &&
            replacement.replacement_.rootDecision.slot == decision_.slot &&
            replacement.replacement_.rootDecision.generation == decision_.generation &&
            PrepareLayout(events, count, arms, {replacement.replacement_.next.version});
    }

    // Placement can refuse real calendar capacity while every owner is private.
    // Arm and group pins are acquired only after their owners have committed.
    bool PreparePlacement(const ImmediateLaneEvent* events, std::uint32_t count) noexcept {
        if (!Active() || placed_ || prepared_ || groupCount_ == 0 ||
            !lane_->Prepare(handle_, events, count) ||
            !lane_->TryPlaceExactPrivate(handle_, 0) ||
            !pool_->RebaseImmediateLaneGroupsInDomain(root_, 0, domain_)) return false;
        placed_ = true;
        return true;
    }

private:
    bool PrepareLayout(const ImmediateLaneEvent* events, std::uint32_t count,
        TimelineArmTable& arms, ConditionalQueueVersionToken conditional) noexcept {
        if (!Active() || prepared_ || groupCount_ == 0 ||
            (!placed_ && !PreparePlacement(events, count)) ||
            !lane_->PinArms(handle_, arms) ||
            !lane_->PinGroups(handle_, *pool_, *decisions_, pool_->MaxPlan03GroupHeadersPerPage())) return false;
        conditional_ = conditional;
        prepared_ = true;
        return true;
    }

public:
    bool BorrowPublicationLifetime(OriginalImmediateLaneAttemptLifetimePin* out) const noexcept {
        return Active() && prepared_ && !published_ && originalLane_.Borrow(out);
    }

    bool ProveInstallationPublication(TimelineArmToken arm, std::uint64_t frame,
        AudioSliceLease** out) const noexcept {
        if (out == nullptr || *out != nullptr || !Active() || !prepared_ || published_ ||
            groupCount_ != 1 || !queues_->ProvePendingConditionalIdentity(queue_, conditional_, decision_))
            return false;
        Plan03PageGroupView group{};
        const auto* attempt = lane_->Resolve(handle_);
        if (attempt == nullptr || attempt->eventCount != 1 ||
            !pool_->ResolvePlan03GroupSliceProducer(groups_[0], &group) || !Owns(group) ||
            group.memberCount != 1 || group.sourceControlCount != 0 || group.lease == nullptr ||
            group.lease->Capture().state != AudioSliceState::Ready) return false;
        const auto actual = group.members[0];
        if (actual.slot != arm.slot || actual.generation != arm.generation ||
            actual.renderSlot != arm.renderSlot || actual.renderGeneration != arm.renderGeneration) return false;
        const auto& event = lane_->EventBase(handle_.slot)[0].event;
        const auto* program = event.program.program;
        if (event.executeFrame != frame || event.armToken.slot != arm.slot ||
            event.armToken.generation != arm.generation || program == nullptr ||
            program->generation != event.program.programGeneration || event.payload >= program->payloadCount ||
            program->payloads[event.payload].kind != TimelineEventKind::Start) return false;
        *out = group.lease;
        return true;
    }

    bool Publish() noexcept {
        DecisionCommittedSnapshot decision{};
        if (!Active() || !prepared_ || published_ ||
            !queues_->ProvePendingConditionalIdentity(queue_, conditional_, decision_) ||
            !decisions_->CaptureCommitted(decision_, &decision) ||
            decision.pages.slot != handle_.slot + 1u || decision.pages.generation != handle_.generation ||
            !decisions_->HasExclusiveParticipant(decision_,
                {DecisionParticipantKind::QueueInstance, queue_.slot, queue_.generation})) return false;
        const auto* attempt = lane_->Resolve(handle_);
        if (attempt == nullptr) return false;
        auto* events = lane_->EventBase(handle_.slot);
        for (std::uint32_t i = 0; i < attempt->eventCount; ++i)
            events[i].event.logicCommitSequence = decision.ticket.sequence;
        if (!pool_->PublishImmediateLaneGroupsInDomain(root_, domain_) || !lane_->Publish(handle_)) return false;
        published_ = true;
        return true;
    }

    std::size_t CaptureGroups(const ImmediateActionLane& lane, ImmediateLaneHandle handle,
        const ScheduledTransactionPool& pool, Plan03ImmediateLaneGroupDescriptor* out,
        std::size_t capacity) const noexcept {
        if (&lane != lane_ || &pool != pool_ || handle.slot != handle_.slot ||
            handle.generation != handle_.generation || !queueLifetime_.Valid() ||
            !lane.OwnsPrivate(handle, root_)) return capacity + 1u;
        for (std::uint32_t index = 0; index < groupCount_; ++index) {
            const auto& identity = groups_[index];
            if (identity.committedRow >= pool.immediateLaneGroups_.ConstructedCount()) return capacity + 1u;
            const auto& native = pool.immediateLaneGroups_[identity.committedRow];
            if (native.state.load(std::memory_order_acquire) != ScheduledTransactionPool::LaneGroupState::Private ||
                native.cohortRoot != root_ || native.selectedFutureDomain != domain_ ||
                native.generation != identity.rowClaimGeneration || native.laneOwner != lane_ ||
                native.laneSlot != handle_.slot || native.laneGeneration != handle_.generation ||
                !native.frameRebased) return capacity + 1u;
            if (index < capacity) out[index] = {identity, native.dueQuantum, false, handle_.slot, handle_.generation};
        }
        return groupCount_;
    }

    bool Owns(const Plan03PageGroupView& group) const noexcept {
        if (!Active() || group.frontier != nullptr || group.lease == nullptr ||
            group.identity.decision.slot != decision_.slot ||
            group.identity.decision.generation != decision_.generation) return false;
        for (std::uint32_t index = 0; index < groupCount_; ++index)
            if (group.identity == groups_[index]) return true;
        return false;
    }

    bool Uses(const ScheduledTransactionPool* pool, const QueueInstanceTable* queues,
        const DecisionTable* decisions) const noexcept {
        return Active() && pool_ == pool && queues_ == queues && decisions_ == decisions;
    }

    bool Retire(std::uint64_t completed) noexcept {
        if (!Active()) return true;
        if (handle_.Valid()) {
            if (!lane_->Cancel(handle_) || !lane_->Retire(handle_, completed, &originalLane_)) return false;
            handle_ = {};
        }
        if (!pool_->CancelImmediateLaneGroupsInDomain(root_, completed, nullptr, 0, domain_)) return false;
        queueLifetime_.Release();
        lane_ = nullptr;
        pool_ = nullptr;
        groups_ = {};
        groupCount_ = 0;
        placed_ = prepared_ = published_ = false;
        return true;
    }

    // The caller holds the actual Lane capture gate. A rejected Reset leaves
    // Old executable; the batch's single selector arbitrates every old write.
    bool BeginAudio() const noexcept;
    bool BeforeAudioWrite() const noexcept;
    void EndAudio() const noexcept;

    void Collect(std::uint64_t completed) noexcept {
        if (!Active()) return;
        if (!handle_.Valid()) { (void)Retire(completed); return; }
        ImmediateLaneObservation observation{};
        if (!lane_->Observe(handle_, &observation)) return;
        DecisionCommittedSnapshot decision{};
        const bool committed = decisions_->CaptureCommitted(decision_, &decision);
        const bool cancelled = (admission_.load(std::memory_order_acquire) & kWithdrawn) != 0 ||
            decisions_->IsCancelled(decision_) ||
            (committed && (decision.authoredStopInvalidated || decision.audioState == AudioGroupState::Invalidated ||
                decision.audioState == AudioGroupState::PostStartInvalidated ||
                decision.audioState == AudioGroupState::DeadlineInvalidated ||
                decision.audioState == AudioGroupState::PostStartDeadlineInvalidated));
        if (observation.missed || observation.protocolRefused || observation.arenaCapacityMissed)
            (void)decisions_->InvalidateForDeadline(decision_);
        if (cancelled || observation.complete || observation.missed || observation.protocolRefused ||
            observation.arenaCapacityMissed || observation.cancelled) (void)Retire(completed);
    }

    void ReturnAfterQuiescence() noexcept {
        if (!Active()) return;
        originalLane_.Return();
        queueLifetime_.Release();
        lane_ = nullptr;
    }

private:
    static constexpr std::uint64_t kReading = 1u, kChanging = 2u, kAttached = 4u,
        kWritten = 8u, kWithdrawn = 16u;
    mutable std::atomic<std::uint64_t> admission_{0};
    OriginalConditionalReplacementBatch* replacement_ = nullptr;
    std::uint64_t replacementGeneration_ = 0;
    mutable bool audioRead_ = false, replacementRead_ = false;
    ImmediateActionLane* lane_ = nullptr;
    ScheduledTransactionPool* pool_ = nullptr;
    QueueInstanceTable* queues_ = nullptr;
    DecisionTable* decisions_ = nullptr;
    QueueInstanceTable::OriginalLifetimePin queueLifetime_{};
    OriginalImmediateLaneAttemptLifetimePin originalLane_{};
    ImmediateLaneHandle handle_{};
    QueueInstanceToken queue_{};
    ConditionalQueueVersionToken conditional_{};
    DecisionToken decision_{};
    std::array<Plan03GroupSliceIdentity, 2> groups_{};
    std::uint32_t groupCount_ = 0;
    std::uint64_t root_ = 0, domain_ = 0;
    bool placed_ = false, prepared_ = false, published_ = false;
};

} // namespace harken
#endif
