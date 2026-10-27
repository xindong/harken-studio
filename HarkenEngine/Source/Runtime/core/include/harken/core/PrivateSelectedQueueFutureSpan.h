#ifndef HARKEN_CORE_PRIVATE_SELECTED_QUEUE_FUTURE_SPAN_H
#define HARKEN_CORE_PRIVATE_SELECTED_QUEUE_FUTURE_SPAN_H

#include "harken/core/QueueRuntime.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/ActionFiringTable.h"
#include "harken/core/PrivateOriginalQueueGroupClockProof.h"
#include "harken/core/NativeTimelineQuantumAdmissionProof.h"
#include <limits>

namespace harken {
namespace detail { struct PrivateSelectedQueueFutureSpanTestAccess; }
class PrivateSelectedQueueFutureCalendar;

// A short-lived view of an actual held issuer. Deferred replacement borrows
// its prepared child factory's inherited parent order instead of issuing again.
class PreparedQueueReplacementOrder {
public:
    PreparedQueueReplacementOrder(const LogicCommitCoordinator::PreparedIssue& issue) noexcept : issue_(&issue) {}
    PreparedQueueReplacementOrder(const PreparedActionFiring& child) noexcept : child_(&child) {}
    PreparedQueueReplacementOrder(LogicCommitCoordinator::PreparedIssue&&) = delete;
    PreparedQueueReplacementOrder(PreparedActionFiring&&) = delete;
    LogicCommitSequence Sequence() const noexcept {
        return issue_ != nullptr ? issue_->Sequence() : child_->InheritedSequence();
    }
    bool Valid() const noexcept { return Sequence() != 0; }
private:
    const LogicCommitCoordinator::PreparedIssue* issue_ = nullptr;
    const PreparedActionFiring* child_ = nullptr;
};

// Serialized logic-producer storage. Only the authentic matched replacement
// batch consumes a prepared Queue selection after its shared native winner.
// No method publishes a command head or derives firstS from an observation.
class PrivateSelectedQueueFutureSpanPool {
    struct Row;
public:
    enum class Phase : std::uint8_t { Free, Prepared, FrozenUnbound, Closed, Quarantined };
    struct Observation {
        QueueInstanceToken queue{};
        ConditionalQueueVersionToken oldVersion{};
        DecisionToken currentDecision{};
        OccurrenceToken current{};
        KnownCompletionPredicateV1 currentCompletion{};
        CompletionOwnerRef currentOwner{};
        EffectiveQueueState currentState{}, resetFirst{};
        DecisionToken newDecision{}, newCoordinator{};
        OccurrenceToken newSuccessor{};
        CompletionOwnerRef newSuccessorOwner{};
        std::array<Plan03GroupSliceIdentity, 2> newGroups{};
        std::array<std::uint64_t, 2> relativeFrames{};
        TimelineArmToken newMember{};
        std::uint16_t newMemberCount = 0;
        LogicCommitSequence reservedSequence = 0;
    };
    struct ConditionalBacking {
        WorldGeneration world = 0;
        PlayableInstanceHandle rootInstance = 0;
        std::uint64_t programSemanticFingerprint = 0;
        QueueInstanceToken queue{};
        ConditionalQueueVersionToken replacedVersion{}, version{};
        DecisionToken currentDecision{}, decision{}, coordinator{};
        OccurrenceToken current{}, successor{};
        KnownCompletionPredicateV1 completion{};
        CompletionOwnerRef completionOwner{};
        EffectiveQueueState currentState{}, resetInitial{}, next{};
        bool logicalFloor = false;
        std::array<Plan03GroupSliceIdentity, 2> groups{};
        std::array<std::uint64_t, 2> relativeFrames{};
        LogicCommitSequence sequence = 0;
    };
    // A new Source can complete a later Queue step under its own Decision.
    // These inputs are authenticated against the same reserved Action sequence;
    // constructing this view grants no publication or completion authority.
    struct SourceCompletionFactory {
        const PreparedDecisionReservation& decision;
        const OriginalDecisionLifetimePin& original;
        const DecisionTable::PreparedTicketCommitProof& ticket;
    };
    class Retention;
    class NativeConditionalRead;
    class NativeConditionalProof;
    class Owner {
    public:
        Owner() = default;
        Owner(const Owner&) = delete;
        Owner& operator=(const Owner&) = delete;
        Owner(Owner&& other) noexcept { Take(other); }
        Owner& operator=(Owner&& other) noexcept {
            if (this != &other) { Release(); Take(other); }
            return *this;
        }
        ~Owner() { Release(); }
        bool Valid() const noexcept { return pool_ != nullptr && pool_->Matches(*this); }
        bool Borrow(Retention* out) const noexcept;
        bool Capture(Observation* out) const noexcept;
        bool BorrowNativeConditional(NativeConditionalRead* out) const noexcept;
        Phase State() const noexcept;
        void Release() noexcept {
            if (pool_ != nullptr) pool_->Close(*this);
        }
    private:
        friend class PrivateSelectedQueueFutureSpanPool;
        void Take(Owner& other) noexcept {
            pool_ = std::exchange(other.pool_, nullptr);
            slot_ = std::exchange(other.slot_, UINT32_MAX);
            generation_ = std::exchange(other.generation_, 0);
            marker_ = std::exchange(other.marker_, 0);
        }
        PrivateSelectedQueueFutureSpanPool* pool_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX, generation_ = 0;
        std::uint64_t marker_ = 0;
    };
    class Retention {
    public:
        Retention() = default;
        Retention(const Retention&) = delete;
        Retention& operator=(const Retention&) = delete;
        Retention(Retention&& other) noexcept { Take(other); }
        Retention& operator=(Retention&& other) noexcept {
            if (this != &other) { Release(); Take(other); }
            return *this;
        }
        ~Retention() { Release(); }
        bool Valid() const noexcept { return pool_ != nullptr && pool_->Matches(*this); }
        bool Borrow(Retention* out) const noexcept;
        bool Capture(Observation* out) const noexcept;
        void Release() noexcept {
            if (pool_ != nullptr) pool_->Return(*this);
        }
    private:
        friend class PrivateSelectedQueueFutureSpanPool;
        void Take(Retention& other) noexcept {
            pool_ = std::exchange(other.pool_, nullptr);
            slot_ = std::exchange(other.slot_, UINT32_MAX);
            generation_ = std::exchange(other.generation_, 0);
            marker_ = std::exchange(other.marker_, 0);
        }
        PrivateSelectedQueueFutureSpanPool* pool_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX, generation_ = 0;
        std::uint64_t marker_ = 0;
    };

    // Producer-retained backing. Audio only borrows it through the surrounding
    // genuine Catalog activity; it never returns these native structural loans.
    class NativeConditionalRead {
    public:
        NativeConditionalRead() = default;
        NativeConditionalRead(const NativeConditionalRead&) = delete;
        NativeConditionalRead& operator=(const NativeConditionalRead&) = delete;
        NativeConditionalRead(NativeConditionalRead&& other) noexcept { Take(other); }
        NativeConditionalRead& operator=(NativeConditionalRead&& other) noexcept {
            if (this != &other) {
                const bool released = Release();
                assert(released);
                if (released) Take(other);
            }
            return *this;
        }
        ~NativeConditionalRead() { const bool released = Release(); assert(released); (void)released; }
        bool Valid() const noexcept { return pool_ != nullptr && pool_->Matches(*this); }
        bool Capture(ConditionalBacking* out) const noexcept;
        bool Release() noexcept;
    private:
        friend class PrivateSelectedQueueFutureSpanPool;
        void Take(NativeConditionalRead& other) noexcept {
            pool_ = std::exchange(other.pool_, nullptr);
            slot_ = std::exchange(other.slot_, UINT32_MAX);
            generation_ = std::exchange(other.generation_, 0);
            marker_ = std::exchange(other.marker_, 0);
        }
        PrivateSelectedQueueFutureSpanPool* pool_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX, generation_ = 0;
        std::uint64_t marker_ = 0;
    };
    class NativeConditionalProof {
    public:
        NativeConditionalProof() = default;
        NativeConditionalProof(const NativeConditionalProof&) = delete;
        NativeConditionalProof& operator=(const NativeConditionalProof&) = delete;
        NativeConditionalProof(NativeConditionalProof&& other) noexcept { Take(other); }
        NativeConditionalProof& operator=(NativeConditionalProof&& other) noexcept {
            if (this != &other) { Release(); Take(other); }
            return *this;
        }
        ~NativeConditionalProof() { Release(); }
        bool Valid() const noexcept { return row_ != nullptr && admission_.Valid(); }
        void Release() noexcept {
            if (row_ != nullptr) {
                gate_->store(false, std::memory_order_release);
                row_ = nullptr;
                gate_ = nullptr;
                lease_ = nullptr;
                admission_.Release();
            }
        }
    private:
        friend class PrivateSelectedQueueFutureSpanPool;
        void Take(NativeConditionalProof& other) noexcept {
            pool_ = std::exchange(other.pool_, nullptr);
            row_ = std::exchange(other.row_, nullptr);
            gate_ = std::exchange(other.gate_, nullptr);
            lease_ = std::exchange(other.lease_, nullptr);
            completion_ = other.completion_;
            admission_ = std::move(other.admission_);
            slice_ = other.slice_;
        }
        PrivateSelectedQueueFutureSpanPool* pool_ = nullptr;
        void* row_ = nullptr;
        std::atomic<bool>* gate_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
        KnownCompletionQuantumProof completion_{};
        NativeTimelineQuantumAdmissionProof admission_;
        QueueInstanceTable::FrontierConditionalSlice slice_ = QueueInstanceTable::FrontierConditionalSlice::Activation;
    };

    using BackingPreparation = StableSlotStorage<Row>::Preparation;
    using BackingUse = StableSlotStorage<Row>::PreparedUse;
    bool PrepareBacking(std::uint32_t count, BackingPreparation* out) noexcept {
        return rows_.PrepareBacking(count < capacity_ ? count : capacity_, 0,
            [](const Row& row) noexcept { return row.phase == Phase::Free; },
            [this](std::uint32_t slot) noexcept { return rows_.Emplace(slot); }, out);
    }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t ConstructedRowCount() const noexcept { return rows_.ConstructedCount(); }
    std::size_t RowStorageBytes() const noexcept { return rows_.RequestedBytes(); }
    std::size_t RowCommittedBytes() const noexcept { return rows_.CommittedBytes(); }
    std::size_t RowDirectoryBytes() const noexcept { return rows_.DirectoryBytes(); }
    void SetRowCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }

    explicit PrivateSelectedQueueFutureSpanPool(std::uint32_t capacity)
        : capacity_(capacity != 0 && rows_.Reserve(capacity) ? capacity : 0) {}
    PrivateSelectedQueueFutureSpanPool(const PrivateSelectedQueueFutureSpanPool&) = delete;
    PrivateSelectedQueueFutureSpanPool& operator=(const PrivateSelectedQueueFutureSpanPool&) = delete;
    PrivateSelectedQueueFutureSpanPool(PrivateSelectedQueueFutureSpanPool&&) = delete;
    PrivateSelectedQueueFutureSpanPool& operator=(PrivateSelectedQueueFutureSpanPool&&) = delete;
    ~PrivateSelectedQueueFutureSpanPool() {
        for (const auto& row : rows_)
            assert(row.phase == Phase::Free || row.phase == Phase::Quarantined);
    }

    // Only this retained factory can associate a plain Stop control with its
    // independently prepared grouped Source. Ordinary plain factories stay strict.
    bool PrepareSourceAuthoredControl(const Owner& owner, TimelineArmTable& arms,
        const TimelineArmTable::PendingAuthoredControlInput& input,
        TimelineArmToken* out) noexcept {
        if (!Matches(owner)) return false;
        const auto& row = rows_[owner.slot_];
        if (row.closed.load(std::memory_order_acquire) ||
            row.phase != Phase::FrozenUnbound || !row.sourceCompletion.Valid() ||
            !row.newSuccessor.Valid() || row.value.newMemberCount != 1 ||
            !SameArm(input.sourceStart, row.value.newMember) || row.arms != &arms)
            return false;
        return arms.PreparePendingAuthoredControlInternal(input,
            {row.value.newDecision}, out);
    }

    bool ProveSourceAuthoredControlBinding(const Owner& owner, TimelineArmTable& arms, TimelineArmToken control,
        const TimelineArmReaderPin& sourceReader, const TimelineArmTable::AuthoredControlInput& input,
        TimelineArmTable::PendingAuthoredControlBindingProof* out) noexcept {
        if (!Matches(owner)) return false;
        const auto& row = rows_[owner.slot_];
        if (row.closed.load(std::memory_order_acquire) ||
            !row.bound.load(std::memory_order_acquire) || !row.sourceCompletion.Valid() ||
            !row.newSuccessor.Valid() || row.value.newMemberCount != 1 || row.arms != &arms ||
            !row.oldFuture.owner_->IsSelectedOriginalConditionalCurrent(row.oldFuture)) return false;
        return arms.ProvePendingAuthoredControlBindingInternal(control, sourceReader,
            input, {row.value.newDecision}, out);
    }

    // Every input carrying authority is a live native factory object. The scope
    // only authenticates membership; this call does not freeze Old progression.
    bool Prepare(QueueInstanceTable& queues,
        const QueueInstanceTable::PreparedCatalogResetScope& matched,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const QueueInstanceTable::OriginalConditionalFutureReservation& oldFuture,
        const QueueInstanceTable::OriginalConditionalLaneClock& oldClock,
        const PreparedQueueReplacementOrder& sequence,
        const OriginalDecisionLifetimePin& currentDecisionOriginal,
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& currentOriginal,
        DecisionTable& decisions, const PreparedDecisionReservation& newDecision,
        const OriginalDecisionLifetimePin& newDecisionOriginal,
        const DecisionTable::PreparedTicketCommitProof& ticket,
        const PreparedDecisionReservation& newCoordinator,
        const OriginalDecisionLifetimePin& coordinatorOriginal,
        const DecisionTable::PreparedTicketCommitProof& coordinatorTicket,
        OccurrenceStatePool& occurrences,
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& newSuccessorOriginal,
        const OccurrenceStatePool::PreparedSingleCompletionCommitProof& successor,
        ScheduledTransactionPool& groups,
        const OriginalQueueFutureGroupRegistration& newPrimary,
        const OriginalQueueFutureGroupRegistration& newMember,
        Owner* out, const PrivateSelectedQueueFutureDomain* domain = nullptr,
        const SourceCompletionFactory* sourceCompletion = nullptr,
        const ResolvedQueueResetProjection* projection = nullptr) noexcept {
        return PrepareBody(queues, matched, parent, oldFuture, oldClock, sequence,
            currentDecisionOriginal, currentOriginal, decisions, newDecision,
            newDecisionOriginal, ticket, newCoordinator, coordinatorOriginal,
            coordinatorTicket, occurrences, newSuccessorOriginal, &successor,
            groups, newPrimary, newMember, out, domain, sourceCompletion, projection);
    }

    // An installation-free future keeps the genuine committed Current. It
    // needs neither a fabricated prepared Occurrence nor a second Source.
    bool PrepareRetainedCurrent(QueueInstanceTable& queues,
        const QueueInstanceTable::PreparedCatalogResetScope& matched,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const QueueInstanceTable::OriginalConditionalFutureReservation& oldFuture,
        const QueueInstanceTable::OriginalConditionalLaneClock& oldClock,
        const PreparedQueueReplacementOrder& sequence,
        const OriginalDecisionLifetimePin& currentDecisionOriginal,
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& currentOriginal,
        DecisionTable& decisions, const PreparedDecisionReservation& newDecision,
        const OriginalDecisionLifetimePin& newDecisionOriginal,
        const DecisionTable::PreparedTicketCommitProof& ticket,
        const PreparedDecisionReservation& newCoordinator,
        const OriginalDecisionLifetimePin& coordinatorOriginal,
        const DecisionTable::PreparedTicketCommitProof& coordinatorTicket,
        OccurrenceStatePool& occurrences, ScheduledTransactionPool& groups,
        const OriginalQueueFutureGroupRegistration& newPrimary,
        const OriginalQueueFutureGroupRegistration& newMember,
        Owner* out, const PrivateSelectedQueueFutureDomain* domain = nullptr,
        const ResolvedQueueResetProjection* projection = nullptr) noexcept {
        return PrepareBody(queues, matched, parent, oldFuture, oldClock, sequence,
            currentDecisionOriginal, currentOriginal, decisions, newDecision,
            newDecisionOriginal, ticket, newCoordinator, coordinatorOriginal,
            coordinatorTicket, occurrences, currentOriginal, nullptr, groups,
            newPrimary, newMember, out, domain, nullptr, projection);
    }

private:
    bool PrepareBody(QueueInstanceTable& queues,
        const QueueInstanceTable::PreparedCatalogResetScope& matched,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const QueueInstanceTable::OriginalConditionalFutureReservation& oldFuture,
        const QueueInstanceTable::OriginalConditionalLaneClock& oldClock,
        const PreparedQueueReplacementOrder& sequence,
        const OriginalDecisionLifetimePin& currentDecisionOriginal,
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& currentOriginal,
        DecisionTable& decisions, const PreparedDecisionReservation& newDecision,
        const OriginalDecisionLifetimePin& newDecisionOriginal,
        const DecisionTable::PreparedTicketCommitProof& ticket,
        const PreparedDecisionReservation& newCoordinator,
        const OriginalDecisionLifetimePin& coordinatorOriginal,
        const DecisionTable::PreparedTicketCommitProof& coordinatorTicket,
        OccurrenceStatePool& occurrences,
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& newSuccessorOriginal,
        const OccurrenceStatePool::PreparedSingleCompletionCommitProof* successor,
        ScheduledTransactionPool& groups,
        const OriginalQueueFutureGroupRegistration& newPrimary,
        const OriginalQueueFutureGroupRegistration& newMember,
        Owner* out, const PrivateSelectedQueueFutureDomain* domain,
        const SourceCompletionFactory* sourceCompletion,
        const ResolvedQueueResetProjection* projection) noexcept {
        if (out == nullptr || out->pool_ != nullptr || !sequence.Valid() ||
            nextMarker_ == UINT64_MAX || !queues.MatchesCatalogResetScope(matched) ||
            !queues.MatchesOriginalConditionalFutureReservation(oldFuture) ||
            !queues.IsOriginalConditionalLaneClockFor(oldClock, oldFuture, parent) ||
            !decisions.MatchesOriginalPreparedDecision(newDecisionOriginal, newDecision) ||
            !ticket.Associated(&decisions, &newDecision) || ticket.Sequence() != sequence.Sequence() ||
            !SameDecision(ticket.Token(), newDecision.Token()) ||
            !decisions.MatchesOriginalPreparedDecision(coordinatorOriginal, newCoordinator) ||
            !coordinatorTicket.Associated(&decisions, &newCoordinator) ||
            coordinatorTicket.Sequence() != sequence.Sequence() ||
            !SameDecision(coordinatorTicket.Token(), newCoordinator.Token()) ||
            SameDecision(newCoordinator.Token(), newDecision.Token()) ||
            !groups.IsOriginalQueueFutureGroupFamily(newPrimary, newMember)) return false;
        auto& old = queues.originalConditionalRows_[oldFuture.row_];
        if (sourceCompletion != nullptr &&
            (successor == nullptr ||
             !decisions.MatchesOriginalPreparedDecision(sourceCompletion->original,
                 sourceCompletion->decision) ||
             !sourceCompletion->ticket.Associated(&decisions, &sourceCompletion->decision) ||
             sourceCompletion->ticket.Sequence() != sequence.Sequence() ||
             !SameDecision(sourceCompletion->ticket.Token(), sourceCompletion->decision.Token()) ||
             sourceCompletion->decision.OriginalRequest().world != old.world ||
             sourceCompletion->decision.OriginalRequest().transport != newDecision.OriginalRequest().transport ||
             SameDecision(sourceCompletion->decision.Token(), newCoordinator.Token()))) return false;
        if (successor != nullptr &&
            (successor->owner_ != &occurrences || !occurrences.MatchesPreparedSingleProof(*successor) ||
             !occurrences.IsOriginalSingleCompletionOwnerPinFor(newSuccessorOriginal, successor->token_) ||
             successor->completionOwner_.kind != CompletionOwnerKind::SingleQueue ||
             !SameDecision(successor->completionOwner_.singleDecision,
                 sourceCompletion == nullptr ? newDecision.Token() : sourceCompletion->decision.Token())))
            return false;
        CompletionOwnerRef liveCurrentOwner{};
        if (!occurrences.CaptureCompletionOwner(old.current, &liveCurrentOwner) ||
            !SameCompletionOwner(liveCurrentOwner, old.completionOwner)) return false;
        if (!decisions.IsOriginalDecisionLifetimePinFor(currentDecisionOriginal, old.decision) ||
            !occurrences.IsOriginalSingleCompletionOwnerPinFor(currentOriginal, old.current) ||
            old.completionOwner.kind != CompletionOwnerKind::SingleQueue ||
            !SameDecision(old.completionOwner.singleDecision, old.decision) ||
            !SameCompletionOwner(occurrences.m_entries[old.current.slot].originalSingleOwner,
                old.completionOwner) || !IsBoundKnownCompletionPredicate(old.completion) ||
            old.logicalBoundary || old.logicalFloor ||
            old.completion.totalLogicalOutputFrames == 0 || !ValidPlayableProgramKey(old.completion.program) ||
            old.completion.absoluteLogicalCompleteFrame < old.completion.absoluteLogicalStartFrame ||
            old.completion.absoluteLogicalCompleteFrame - old.completion.absoluteLogicalStartFrame != old.completion.totalLogicalOutputFrames ||
            !SameOccurrence(old.completion.occurrence, old.current) ||
            !SameCompletionOwner(old.completion.owner, old.completionOwner)) return false;
        if ((old.clockState != QueueInstanceTable::OriginalConditionalClockState::Unbound &&
             old.clockState != QueueInstanceTable::OriginalConditionalClockState::Bound) ||
            old.clockPool != &groups || old.selectedFutureOwner != nullptr ||
            (old.state != QueueInstanceTable::OriginalConditionalState::Prepared &&
             old.state != QueueInstanceTable::OriginalConditionalState::Committed)) return false;
        auto& live = queues.rows_[old.queue.slot];
        const auto slotState = live.slotState.load(std::memory_order_acquire);
        if ((slotState != QueueInstanceTable::SlotState::Initializing &&
             slotState != QueueInstanceTable::SlotState::Live) ||
            live.catalogResetScopeMarker != matched.marker_ ||
            live.catalogResetScopeGeneration != old.queue.generation ||
            live.Pending().originalConditionalRow != oldFuture.row_ ||
            live.Pending().originalConditionalMarker != oldFuture.marker_ || live.generation != old.queue.generation ||
            live.failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
            live.Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
            live.world != old.world || live.rootInstance != old.rootInstance ||
            live.programSemanticFingerprint != old.programSemanticFingerprint ||
            !SameDecision(live.Pending().pendingDecision, old.decision) ||
            !SameOccurrence(live.Pending().pendingExpectedCurrent, old.current) ||
            !SameOccurrence(live.Pending().pendingSuccessor, old.successor) ||
            !SameCompletionOwner(live.Pending().pendingCompletionOwner, old.completionOwner) ||
            !SameDecision(newPrimary.Identity().decision, newCoordinator.Token()) ||
            !SameDecision(newMember.Identity().decision, newDecision.Token()) ||
            newPrimary.Identity().sliceIndex != 0 || newMember.Identity().sliceIndex != 0 ||
            newPrimary.memberCount_ != 0 || newMember.memberCount_ > 1 ||
            newPrimary.sourceControlCount_ != 0 ||
            (successor == nullptr && newMember.memberCount_ != 0) ||
            (sourceCompletion == nullptr
                ? newMember.sourceControlCount_ != newMember.memberCount_
                : (newMember.memberCount_ != 1 || newMember.sourceControlCount_ != 0)) ||
            newDecision.OriginalRequest().world != old.world ||
            newDecision.OriginalRequest().transport != newPrimary.Identity().transportEpoch ||
            newDecision.OriginalRequest().sliceCount != 1 ||
            newCoordinator.OriginalRequest().sliceCount != 1 ||
            newCoordinator.OriginalRequest().world != old.world ||
            newCoordinator.OriginalRequest().transport != newDecision.OriginalRequest().transport ||
            newPrimary.Identity().transportEpoch != old.clockGroups[1].Identity().transportEpoch)
            return false;
        if (old.state == QueueInstanceTable::OriginalConditionalState::Prepared) {
            if (!live.conditionalReserved.load(std::memory_order_acquire) ||
                live.Pending().pendingVersion.load(std::memory_order_acquire) != 0) return false;
        } else if (!queues.IsCurrentOriginalConditionalFutureReservation(oldFuture)) return false;
        EffectiveQueueState actual{}, projected{};
        if (!QueueInstanceTable::LoadMirror(live, &actual) || !SameState(actual, old.currentState) ||
            !QueueInstanceTable::ProjectReset(live, actual, &projected)) return false;
        auto final = projected;
        std::uint32_t stepCount = 1;
        if (projection != nullptr) {
            const auto& proof = *projection;
            if (proof.states_ == nullptr || proof.count_ == 0 ||
                proof.queue_.slot != old.queue.slot || proof.queue_.generation != old.queue.generation ||
                proof.world_ != old.world || proof.root_ != old.rootInstance ||
                proof.fingerprint_ != old.programSemanticFingerprint || !SameState(proof.current_, actual) ||
                !SameOccurrence(proof.completion_.occurrence, old.current) ||
                !SameCompletionOwner(proof.completion_.owner, old.completionOwner) ||
                !SamePlayableProgramKey(proof.completion_.program, old.completion.program) ||
                proof.completion_.renderGeneration != old.completion.renderGeneration) return false;
            ConditionalQueuePrepareRequest request{};
            request.expectedCurrentVersion = actual.version;
            request.next = proof.states_[proof.count_ - 1u];
            request.projectedStates = proof.states_;
            request.projectedStateCount = proof.count_;
            EffectiveQueueState first{};
            if (!QueueInstanceTable::ValidateProjection(request, actual, live.childCount, &first, &stepCount) ||
                !SameState(first, projected)) return false;
            final = request.next;
        }
        if (domain != nullptr &&
            (domain->owner_ != &groups || !domain->OwnsOriginal(newPrimary) ||
             !domain->OwnsOriginal(newMember))) return false;
        const auto& primaryRow = groups.immediateLaneGroups_[newPrimary.Identity().committedRow];
        const auto& memberRow = groups.immediateLaneGroups_[newMember.Identity().committedRow];
        if (!FreshPrivateGroup(groups, newPrimary, primaryRow) ||
            !FreshPrivateGroup(groups, newMember, memberRow) ||
            primaryRow.relativeFrame != memberRow.relativeFrame ||
            memberRow.relativeFrame != old.clockRelativeFrames[1] ||
            (newPrimary.cohortRoot_ == groups.immediateLaneGroups_[
                 old.clockGroups[1].Identity().committedRow].cohortRoot &&
             primaryRow.selectedFutureDomain == groups.immediateLaneGroups_[
                 old.clockGroups[1].Identity().committedRow].selectedFutureDomain)) return false;
        if (memberRow.memberCount != 0) {
            const auto actualMember = memberRow.MemberTokens()[0];
            Plan03GroupSliceInput expected{};
            expected.executeFrame = newMember.executeFrame_;
            expected.decision = newMember.Identity().decision;
            expected.sliceIndex = newMember.Identity().sliceIndex;
            expected.members = &actualMember;
            expected.memberCount = newMember.memberCount_;
            expected.sourceControlCount = newMember.sourceControlCount_;
            expected.immediateEntry = newMember.immediateEntry_;
            if (successor == nullptr || actualMember.renderGeneration != successor->renderGeneration_ ||
                !newMember.MatchesOriginalScope(newMember.cohortRoot_,
                    newMember.provisionalAnchorFrame_, expected, *newMember.armTable_)) return false;
        }
        std::uint32_t slot = UINT32_MAX;
        const auto available = [](const Row& row) noexcept { return row.phase == Phase::Free; };
        const auto construct = [this](std::uint32_t index) noexcept { return rows_.Emplace(index); };
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (!rows_.IsConstructed(i)) {
                if (rows_.PreparedUseActive()) continue;
                if (!construct(i)) return false;
            }
            if (!available(rows_[i])) continue;
            if (!rows_.ProtectBackingForAcquire(i, available, construct)) return false;
            slot = i;
            break;
        }
        if (slot == UINT32_MAX) return false;
        Row provisional;
        if (!oldFuture.Borrow(&provisional.oldFuture) || !oldClock.Borrow(&provisional.oldClock) ||
            !parent.Borrow(&provisional.parent) ||
            !currentDecisionOriginal.Borrow(&provisional.currentDecision) ||
            !currentOriginal.Borrow(&provisional.current) ||
            !newDecisionOriginal.Borrow(&provisional.newDecision) ||
            !coordinatorOriginal.Borrow(&provisional.coordinator) ||
            !newSuccessorOriginal.Borrow(&provisional.newSuccessor) ||
            !newPrimary.BorrowLifetime(&provisional.newGroups[0]) ||
            !newMember.BorrowLifetime(&provisional.newGroups[1]) ||
            (sourceCompletion != nullptr &&
             !sourceCompletion->original.Borrow(&provisional.sourceCompletion))) return false;
        auto& value = provisional.value;
        value.queue = old.queue;
        value.oldVersion = old.version;
        value.currentDecision = old.decision;
        value.current = old.current;
        value.currentCompletion = old.completion;
        value.currentOwner = old.completionOwner;
        value.currentState = old.currentState;
        value.resetFirst = projected;
        value.newDecision = newDecision.Token();
        value.newCoordinator = newCoordinator.Token();
        value.newSuccessor = successor == nullptr ? old.current : successor->token_;
        value.newSuccessorOwner = successor == nullptr ? old.completionOwner : successor->completionOwner_;
        value.newGroups = {newPrimary.Identity(), newMember.Identity()};
        value.relativeFrames = {primaryRow.relativeFrame, memberRow.relativeFrame};
        value.newMemberCount = memberRow.memberCount;
        if (memberRow.memberCount != 0)
            value.newMember = memberRow.MemberTokens()[0];
        value.reservedSequence = sequence.Sequence();
        Row& row = rows_[slot];
        const auto generation = row.generation;
        row.value = provisional.value;
        row.oldFuture = std::move(provisional.oldFuture);
        row.oldClock = std::move(provisional.oldClock);
        row.parent = std::move(provisional.parent);
        row.currentDecision = std::move(provisional.currentDecision);
        row.current = std::move(provisional.current);
        row.newDecision = std::move(provisional.newDecision);
        row.coordinator = std::move(provisional.coordinator);
        row.newSuccessor = std::move(provisional.newSuccessor);
        row.sourceCompletion = std::move(provisional.sourceCompletion);
        row.newGroups = std::move(provisional.newGroups);
        row.backing = {old.world, old.rootInstance, old.programSemanticFingerprint,
            old.queue, old.version, {final.version}, old.decision, newDecision.Token(),
            newCoordinator.Token(), old.current, value.newSuccessor, old.completion, old.completionOwner,
            old.currentState, live.resetInitial, final, old.logicalFloor,
            value.newGroups, value.relativeFrames, sequence.Sequence()};
        row.stepCount = stepCount;
        row.occurrences = &occurrences;
        row.decisions = &decisions;
        row.arms = groups.plan03ArmTable_;
        row.anchor = row.completionFloor = 0;
        row.closed.store(false, std::memory_order_relaxed);
        row.bound.store(false, std::memory_order_relaxed);
        row.activation.store(ConditionalQueueActivation::Waiting, std::memory_order_relaxed);
        row.nativeGate.store(false, std::memory_order_relaxed);
        row.generation = generation;
        row.marker = ++nextMarker_;
        row.loans = 1;
        row.phase = Phase::Prepared;
        old.selectedFutureOwner = this;
        old.selectedFutureMarker = row.marker;
        out->pool_ = this;
        out->slot_ = slot;
        out->generation_ = row.generation;
        out->marker_ = row.marker;
        return true;
    }

public:
    // A one-shot local transfer after actual factory preflight. It consumes no
    // Queue/Decision/Occurrence proof and grants no audio acceptance or selector.
    bool FreezeUnbound(Owner& owner) noexcept {
        if (!Matches(owner) || rows_[owner.slot_].phase != Phase::Prepared) return false;
        rows_[owner.slot_].phase = Phase::FrozenUnbound;
        return true;
    }

    // Authenticates this exact prepared participant. It is not a health/epoch,
    // firstS, acceptance, native Claim or public Current predicate.
    bool IsFrozenFor(const Owner& owner,
        QueueInstanceTable& queues,
        const QueueInstanceTable::OriginalConditionalFutureReservation& original,
        ScheduledTransactionPool& groups,
        const OriginalQueueFutureGroupRegistration& primary,
        const OriginalQueueFutureGroupRegistration& member) const noexcept {
        if (!Matches(owner) || rows_[owner.slot_].phase != Phase::FrozenUnbound ||
            !queues.MatchesOriginalConditionalFutureReservation(original) ||
            !groups.IsOriginalQueueFutureGroupFamily(primary, member)) return false;
        const auto& row = rows_[owner.slot_];
        return row.oldFuture.owner_ == &queues && row.oldFuture.row_ == original.row_ &&
            row.oldFuture.generation_ == original.generation_ && row.oldFuture.marker_ == original.marker_ &&
            row.newGroups[0].Identity() == primary.Identity() &&
            row.newGroups[1].Identity() == member.Identity();
    }

    // This is a real once-binding from the existing original first-S issuer.
    // The historical CaptureInitialBinding DTO is never accepted as authority.
    bool BindOriginalFirstS(Owner& owner,
        const PrivateOriginalQueueGroupClockProof& actualClock,
        const TimelineArmTable& arms, const DecisionTable& decisions) noexcept {
        if (!Matches(owner) || rows_[owner.slot_].phase != Phase::FrozenUnbound) return false;
        auto& row = rows_[owner.slot_];
        if (row.bound.load(std::memory_order_acquire) || row.nativeGate.load(std::memory_order_acquire)) return false;
        auto& queues = *row.oldFuture.owner_;
        const auto& old = queues.originalConditionalRows_[row.oldFuture.row_];
        if (old.clockState != QueueInstanceTable::OriginalConditionalClockState::Bound ||
            old.clockPool == nullptr ||
            (!queues.IsCurrentOriginalConditionalFutureReservation(row.oldFuture) &&
             !queues.IsSelectedOriginalConditionalCurrent(row.oldFuture))) return false;
        std::array<std::uint64_t, 4> frames{};
        const FrontierGroupBinding *initial = nullptr, *conditional = nullptr;
        if (!old.clockPool->CapturePrivateOriginalQueueClockInitialSource(actualClock,
                old.clockGroups[0], old.clockGroups[1], arms, decisions, old.clockBinding.sourceArm,
                &frames, &initial, &conditional) ||
            frames[0] != old.clockBinding.anchorFrame || frames[1] != old.clockBinding.initialGroupFrame ||
            frames[2] != old.clockBinding.conditionalGroupFrame || frames[3] != old.clockBinding.sourceStartFrame ||
            initial->queueInitial.slot != row.backing.queue.slot ||
            initial->queueInitial.generation != row.backing.queue.generation ||
            !SameOccurrence(conditional->queueConditional.current, row.backing.current) ||
            !SameDecision(conditional->operation.group.decision, row.backing.currentDecision)) return false;
        return BindClockGeometry(row, old.clockBinding);
    }

    // This durable issuer retains the immutable original Source ACK and clock
    // after publication has returned the temporary placement proof.
    bool BindOriginalFirstS(Owner& owner,
        const QueueInstanceTable::OriginalConditionalLaneClock& actualClock) noexcept {
        if (!Matches(owner) || rows_[owner.slot_].phase != Phase::FrozenUnbound) return false;
        auto& row = rows_[owner.slot_];
        auto& queues = *row.oldFuture.owner_;
        if (!queues.IsOriginalConditionalLaneClockFor(actualClock, row.oldFuture, row.parent) ||
            (!queues.IsCurrentOriginalConditionalFutureReservation(row.oldFuture) &&
             !queues.IsSelectedOriginalConditionalCurrent(row.oldFuture)) ||
            row.bound.load(std::memory_order_acquire) || row.nativeGate.load(std::memory_order_acquire))
            return false;
        const auto& old = queues.originalConditionalRows_[row.oldFuture.row_];
        if (old.clockState != QueueInstanceTable::OriginalConditionalClockState::Bound ||
            old.clockBinding.installedAckSequence == 0 || !old.clockBinding.sourceArm.Valid() ||
            old.clockBinding.sourceArm.renderGeneration != row.backing.completion.renderGeneration)
            return false;
        return BindClockGeometry(row, old.clockBinding);
    }

    // This reads only the frozen participant's retained native New group family.
    // The hidden Lane remains unpublished; ordinary audio lookup still refuses it.
    // Returning its actual binding and lease is not a Catalog selector or claim.
    bool ResolveNativeConditionalGroup(const NativeConditionalRead& read,
        Plan03PageGroupView* out) const noexcept {
        if (out == nullptr || read.pool_ != this || read.slot_ >= capacity_ || read.marker_ == 0)
            return false;
        const auto& row = rows_[read.slot_];
        if (row.generation != read.generation_ || row.marker != read.marker_ ||
            row.closed.load(std::memory_order_acquire) ||
            !row.bound.load(std::memory_order_acquire)) return false;
        auto* pool = row.oldFuture.owner_->originalConditionalRows_[row.oldFuture.row_].clockPool;
        Plan03PageGroupView actual{};
        if (pool == nullptr || !pool->ResolveRetainedHiddenConditionalGroup(
                row.newGroups[0], row.newGroups[1], &actual) ||
            !(actual.identity == row.backing.groups[1])) return false;
        *out = actual;
        return true;
    }

    // This proves only the supplied genuine New C1 backing. Choosing New is a
    // separate Catalog right which the Engine must hold for the entire call.
    QueueConditionalProofStatus ProveNativeConditional(const NativeConditionalRead& read,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const OccurrenceStatePool& occurrences, std::uint64_t quantumStart,
        std::uint64_t quantumEnd, std::uint64_t completedSequence,
        const NativeTimelineQuantumAdmissionProof& admission,
        NativeConditionalProof* out,
        QueueInstanceTable::FrontierConditionalSlice slice = QueueInstanceTable::FrontierConditionalSlice::Activation) noexcept {
        if (read.pool_ != this || read.slot_ >= capacity_ || read.marker_ == 0 ||
            out == nullptr || out->row_ != nullptr ||
            slice != QueueInstanceTable::FrontierConditionalSlice::Activation) return QueueConditionalProofStatus::Stale;
        // The producer's genuine read loan keeps these immutable fields alive.
        auto& row = rows_[read.slot_];
        if (row.generation != read.generation_ || row.marker != read.marker_ ||
            row.closed.load(std::memory_order_acquire)) return QueueConditionalProofStatus::Stale;
        if (!row.bound.load(std::memory_order_acquire)) return QueueConditionalProofStatus::Waiting;
        if (!admission.Matches(row.arms, quantumStart, quantumEnd, completedSequence))
            return QueueConditionalProofStatus::Waiting;
        const auto& body = row.backing;
        const auto& id = binding.queueConditional;
        Plan03PageGroupView group{};
        DecisionCommittedSnapshot nativeDecision{};
        auto* pool = row.oldFuture.owner_->originalConditionalRows_[row.oldFuture.row_].clockPool;
        const auto ordinal = 1u;
        if (pool == nullptr || !binding.Valid() || !id.Valid() || binding.queueInitial.Valid() ||
            binding.variableSide.Valid() || !(binding.operation.group == body.groups[ordinal]) ||
            id.slot != body.queue.slot || id.generation != body.queue.generation ||
            id.currentVersion != body.currentState.version || id.pendingVersion != body.version.version ||
            !SameOccurrence(id.current, body.current) || !SameOccurrence(id.successor, body.successor) ||
            binding.operation.world != body.world || binding.operation.owner.child != body.rootInstance ||
            binding.operation.order.logicCommitSequence != body.sequence ||
            !row.decisions->CaptureCommitted(body.decision, &nativeDecision) ||
            nativeDecision.world != body.world || nativeDecision.transport != binding.operation.transport ||
            nativeDecision.ticket.sequence != body.sequence || nativeDecision.sliceCount <= binding.operation.group.sliceIndex ||
            nativeDecision.commit != DecisionCommitState::Committed || nativeDecision.authoredStopInvalidated ||
            (nativeDecision.audioState != AudioGroupState::Pending &&
             nativeDecision.audioState != AudioGroupState::AudioClaimed &&
             nativeDecision.audioState != AudioGroupState::Admitted) ||
            !ResolveNativeConditionalGroup(read, &group) ||
            group.frontier != &binding || group.lease != &lease ||
            lease.Capture().state != AudioSliceState::Ready ||
            body.relativeFrames[ordinal] > UINT64_MAX - row.anchor ||
            binding.operation.order.executeFrame != row.anchor + body.relativeFrames[ordinal])
            return QueueConditionalProofStatus::FailedIdentity;
        bool expected = false;
        if (!row.nativeGate.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) return QueueConditionalProofStatus::Waiting;
        out->pool_ = this;
        out->row_ = &row;
        out->gate_ = &row.nativeGate;
        out->lease_ = &lease;
        out->slice_ = slice;
        if (!admission.Borrow(&out->admission_)) { out->Release(); return QueueConditionalProofStatus::Waiting; }
        if (row.closed.load(std::memory_order_acquire)) { out->Release(); return QueueConditionalProofStatus::Stale; }
        if (row.activation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting) {
            out->Release(); return QueueConditionalProofStatus::Stale;
        }
        CompletionOwnerRef actualOwner{};
        CompletionProgressSnapshot snapshot{};
        const auto* progress = occurrences.CompletionProgressOf(body.current);
        if (&occurrences != row.occurrences || progress == nullptr ||
            !occurrences.CaptureCompletionOwner(body.current, &actualOwner) ||
            !SameCompletionOwner(actualOwner, body.completionOwner) ||
            !CaptureCompletionProgressExact(*progress, body.current, body.completion.renderGeneration, &snapshot)) {
            out->Release(); return QueueConditionalProofStatus::FailedIdentity;
        }
        const auto& predicate = body.completion;
        if (completedSequence < predicate.progressBaselineCompletedThroughBlockSequence ||
            completedSequence - predicate.progressBaselineCompletedThroughBlockSequence > UINT64_MAX - predicate.progressBaselineRevision) {
            out->Release(); return QueueConditionalProofStatus::FailedProgress;
        }
        const auto revision = predicate.progressBaselineRevision +
            completedSequence - predicate.progressBaselineCompletedThroughBlockSequence;
        const auto evaluated = EvaluateKnownCompletionForQuantum(predicate, snapshot,
            quantumStart, quantumEnd, completedSequence, revision, out->admission_.Valid(), &out->completion_);
        if (evaluated == CompletionProofStatus::CompletesAfterQuantum ||
            evaluated == CompletionProofStatus::CurrentQuantumNotAdmitted) {
            out->Release(); return QueueConditionalProofStatus::Waiting;
        }
        if (evaluated != CompletionProofStatus::CompletesInQuantum || quantumEnd <= quantumStart ||
            binding.operation.order.executeFrame < quantumStart ||
            binding.operation.order.executeFrame - quantumStart != out->completion_.activationOffset) {
            out->Release();
            return evaluated == CompletionProofStatus::CompletedBeforeQuantum
                ? QueueConditionalProofStatus::MissedDeadline : QueueConditionalProofStatus::FailedProgress;
        }
        return QueueConditionalProofStatus::ReadyToActivate;
    }
    void ApplyNativeConditional(const NativeConditionalProof& proof) noexcept {
        if (proof.pool_ != this || !proof.Valid() || proof.lease_ == nullptr) return;
        assert(proof.admission_.Valid());
        const auto state = proof.lease_->Capture().state;
        assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
        (void)state;
        auto& row = *static_cast<Row*>(proof.row_);
        assert(row.nativeGate.load(std::memory_order_acquire));
        if (proof.slice_ == QueueInstanceTable::FrontierConditionalSlice::Activation)
            row.activation.store(ConditionalQueueActivation::Activated, std::memory_order_release);
    }


    // Only this genuine, unselected body may undo its private Decision payloads.
    // The original Current and any selected/published native reader are excluded.
    bool RollbackUnselectedDecisionCommits(const Owner& owner,
        const PrivateSelectedQueueFutureDomain& domain) noexcept {
        if (!Matches(owner)) return false;
        auto& selected = rows_[owner.slot_];
        if (selected.phase != Phase::FrozenUnbound || selected.closed.load(std::memory_order_acquire) ||
            selected.nativeGate.load(std::memory_order_acquire) ||
            !MatchesCalendarDomain(selected, domain)) return false;
        const auto& original = selected.oldFuture.owner_->originalConditionalRows_[selected.oldFuture.row_];
        if (original.selectedFutureConsumed) return false;
        auto* pool = CalendarPool(selected);
        auto& decisions = *selected.decisions;
        const std::array<DecisionToken, 2> tokens{selected.backing.coordinator, selected.backing.decision};
        const std::array<const OriginalDecisionLifetimePin*, 2> lifetimes{&selected.coordinator, &selected.newDecision};
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto group = selected.newGroups[i].Identity();
            if (!pool->MatchesOriginalQueueFutureGroupLifetime(selected.newGroups[i])) return false;
            const auto& native = pool->immediateLaneGroups_[group.committedRow];
            const auto* decision = decisions.Resolve(tokens[i]);
            if (native.state.load(std::memory_order_acquire) != ScheduledTransactionPool::LaneGroupState::Private ||
                native.selectedFutureDomain != domain.marker_ ||
                native.lease.Capture().state != AudioSliceState::Ready ||
                native.frontier.Valid() || native.readers.pins.load(std::memory_order_acquire) != 0 ||
                native.readers.lastAudioSequence.load(std::memory_order_acquire) != 0 ||
                decision == nullptr || !decisions.IsOriginalDecisionLifetimePinFor(*lifetimes[i], tokens[i]) ||
                decision->ticket.sequence != selected.backing.sequence ||
                decision->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
                decision->consumedSlices.load(std::memory_order_acquire) != 0 ||
                decision->lastAudioSequence.load(std::memory_order_acquire) != 0 ||
                decision->authoredStopInvalidated.load(std::memory_order_acquire) ||
                decision->futurePinCount.load(std::memory_order_acquire) != 0 ||
                decisions.ResolveFrontierActivation(tokens[i]) != nullptr ||
                decisions.HasFrontierDecisionBinding(tokens[i])) return false;
            const auto commit = decision->commit.load(std::memory_order_acquire);
            if (commit != DecisionCommitState::Open && commit != DecisionCommitState::Committed) return false;
            if (commit == DecisionCommitState::Committed &&
                (decision->pages.slot != group.committedRow ||
                 decision->pages.generation != group.rowClaimGeneration || decision->participantCount != 1u)) return false;
        }
        // The serialized producer owns both private groups. No audio publication
        // or selected Queue can race these exact, jointly checked records.
        for (const auto token : tokens) {
            auto& decision = *decisions.Resolve(token);
            decision.participantCount = 0;
            decision.pages = {};
            decision.armGroup = {};
            decision.commit.store(DecisionCommitState::Open, std::memory_order_release);
        }
        return true;
    }

private:
    friend struct detail::PrivateSelectedQueueFutureSpanTestAccess;
    friend class OriginalConditionalReplacementBatch;
    bool CanConsumeQueueSelection(const Retention& retained,
        const QueueInstanceTable::OriginalConditionalFutureReservation& original,
        const QueueExecutionOrderV1& order) const noexcept {
        if (!IsFrozenReplacementOf(retained, original, order.sequence)) return false;
        const auto& selected = rows_[retained.slot_];
        auto& queues = *selected.oldFuture.owner_;
        if (!queues.IsCurrentOriginalConditionalFutureReservation(original)) return false;
        const auto& old = queues.originalConditionalRows_[original.row_];
        const auto& live = queues.rows_[old.queue.slot];
        if (old.selectedFutureConsumed || old.selectedFutureOwner != this ||
            old.selectedFutureMarker != selected.marker ||
            live.resetReserved.load(std::memory_order_acquire) ||
            live.resetPending.load(std::memory_order_acquire) ||
            live.resetIntentGeneration != 0 || live.resetIntentCounter == UINT64_MAX ||
            live.conditionalReplacementReserved.load(std::memory_order_acquire) ||
            QueueInstanceTable::SignalOwnsNext(live))
            return false;
        auto* groups = old.clockPool;
        if (groups == nullptr) return false;
        for (std::size_t i = 0; i < selected.newGroups.size(); ++i) {
            const auto& originalGroup = selected.newGroups[i];
            if (!groups->MatchesOriginalQueueFutureGroupLifetime(originalGroup)) return false;
            const auto& group = groups->immediateLaneGroups_[originalGroup.Identity().committedRow];
            DecisionCommittedSnapshot decision{};
            if (group.state.load(std::memory_order_acquire) != ScheduledTransactionPool::LaneGroupState::Private ||
                !selected.decisions->CaptureCommitted(originalGroup.Identity().decision, &decision) ||
                decision.commit != DecisionCommitState::Committed || decision.ticket.sequence != order.sequence ||
                decision.world != old.world || decision.transport != originalGroup.Identity().transportEpoch ||
                decision.audioState != AudioGroupState::Pending || decision.authoredStopInvalidated ||
                decision.consumedSlices != 0 || decision.lastAudioSequence != 0 ||
                decision.pages.slot != originalGroup.Identity().committedRow ||
                decision.pages.generation != originalGroup.Identity().rowClaimGeneration)
                return false;
        }
        DecisionParticipantRef queueParticipant{};
        queueParticipant.kind = DecisionParticipantKind::QueueInstance;
        queueParticipant.slot = old.queue.slot;
        queueParticipant.generation = old.queue.generation;
        DecisionParticipantRef memberParticipant{};
        memberParticipant.kind = DecisionParticipantKind::Subdecision;
        memberParticipant.slot = selected.backing.decision.slot;
        memberParticipant.generation = selected.backing.decision.generation;
        TimelineTransactionProgramRef controller{};
        CompletionOwnerRef actualSuccessor{};
        return selected.decisions->CaptureFrontierControlProgram(selected.backing.coordinator, &controller) &&
            controller.program != nullptr && controller.programGeneration != 0 &&
            controller.program->role == TimelineProgramRole::ControllerActivation &&
            selected.decisions->HasExclusiveParticipant(selected.backing.decision, queueParticipant) &&
            selected.decisions->HasExclusiveParticipant(selected.backing.coordinator, memberParticipant) &&
            selected.occurrences->CaptureCompletionOwner(selected.backing.successor, &actualSuccessor) &&
            SameCompletionOwner(actualSuccessor, selected.value.newSuccessorOwner);
    }

    void ConsumeQueueSelection(const Retention& retained,
        const QueueExecutionOrderV1& order) noexcept {
        auto& selected = rows_[retained.slot_];
        auto& queues = *selected.oldFuture.owner_;
        auto& old = queues.originalConditionalRows_[selected.oldFuture.row_];
        auto& live = queues.rows_[old.queue.slot];
        const auto& body = selected.backing;
        // The batch holds the exact old and new records. Its shared winner has
        // excluded every Old C1 writer; New groups remain genuinely private.
        auto* suffix = live.Pending().following.exchange(nullptr, std::memory_order_acq_rel);
        while (suffix != nullptr) {
            auto* next = suffix->following.load(std::memory_order_acquire);
            queues.RetireDetachedConditional(live, *suffix);
            suffix = next;
        }
        old.selectedFutureConsumed = true;
        old.selectedDecision = body.decision;
        old.selectedSuccessor = body.successor;
        old.selectedVersion = body.version;
        live.Pending().pendingDecision = body.decision;
        live.Pending().pendingExpectedCurrent = body.current;
        live.Pending().pendingSuccessor = body.successor;
        live.Pending().pendingCompletion = body.completion;
        live.Pending().pendingCompletionOwner = body.completionOwner;
        live.Pending().pendingCompletionIsLogicalBoundary = false;
        live.Pending().pendingCompletionHasLogicalFloor = body.logicalFloor;
        live.Pending().pendingLogicalCompletionFrame = body.logicalFloor ? selected.completionFloor : 0;
        live.Pending().pendingRetiredSource = {};
        live.Pending().pendingNext = body.next;
        live.Pending().pendingStepCount = selected.stepCount;
        live.resetNextVersion = body.next.version;
        live.resetOrder = order;
        live.resetExecuteFrame = order.executeFrame;
        live.resetIntentGeneration = ++live.resetIntentCounter;
        live.Pending().pendingResetIntent = live.resetIntentGeneration;
        live.resetPending.store(order.executeFrame <= queues.logicNow_, std::memory_order_release);
        live.Pending().pendingConsumesReset.store(true, std::memory_order_release);
        live.Pending().pendingVersion.store(body.version.version, std::memory_order_release);
    }
    bool IsFrozenReplacementOf(const Retention& retained,
        const QueueInstanceTable::OriginalConditionalFutureReservation& original,
        LogicCommitSequence sequence) const noexcept {
        if (!Matches(retained) || !original.Valid()) return false;
        const auto& row = rows_[retained.slot_];
        return row.phase == Phase::FrozenUnbound && !row.closed.load(std::memory_order_acquire) &&
            row.value.reservedSequence == sequence &&
            row.oldFuture.owner_ == original.owner_ && row.oldFuture.row_ == original.row_ &&
            row.oldFuture.generation_ == original.generation_ && row.oldFuture.marker_ == original.marker_;
    }
    struct Row {
        Phase phase = Phase::Free;
        std::uint32_t generation = 1, loans = 0;
        std::uint64_t marker = 0;
        Observation value{};
        ConditionalBacking backing{};
        std::uint32_t stepCount = 0;
        OccurrenceStatePool* occurrences = nullptr;
        DecisionTable* decisions = nullptr;
        const TimelineArmTable* arms = nullptr;
        std::uint64_t anchor = 0, completionFloor = 0;
        std::atomic<bool> closed{false}, bound{false}, nativeGate{false};
        std::atomic<ConditionalQueueActivation> activation{ConditionalQueueActivation::Waiting};
        QueueInstanceTable::OriginalConditionalFutureReservation oldFuture;
        QueueInstanceTable::OriginalConditionalLaneClock oldClock;
        QueueInstanceTable::OriginalLifetimePin parent;
        OriginalDecisionLifetimePin currentDecision;
        OccurrenceStatePool::OriginalSingleCompletionOwnerPin current;
        OriginalDecisionLifetimePin newDecision, coordinator, sourceCompletion;
        OccurrenceStatePool::OriginalSingleCompletionOwnerPin newSuccessor;
        std::array<OriginalQueueFutureGroupLifetimePin, 2> newGroups{};
    };
    static bool BindClockGeometry(Row& row,
        const QueueInstanceTable::OriginalConditionalInitialClockBinding& clock) noexcept {
        auto& queues = *row.oldFuture.owner_;
        const auto& old = queues.originalConditionalRows_[row.oldFuture.row_];
        if (old.selectedFutureConsumed &&
            !queues.IsSelectedOriginalConditionalCurrent(row.oldFuture)) return false;
        KnownCompletionPredicateV1 rebound{};
        if (!ReanchorKnownCompletionBeforePublication(row.backing.completion, clock.sourceStartFrame, &rebound) ||
            row.backing.relativeFrames[0] > UINT64_MAX - clock.anchorFrame ||
            row.backing.relativeFrames[1] > UINT64_MAX - clock.anchorFrame ||
            row.backing.relativeFrames[1] + clock.anchorFrame != clock.conditionalGroupFrame ||
            (!row.backing.logicalFloor && rebound.absoluteLogicalCompleteFrame != clock.conditionalGroupFrame) ||
            (row.backing.logicalFloor && rebound.absoluteLogicalCompleteFrame > clock.conditionalGroupFrame)) return false;
        row.backing.completion = rebound;
        row.anchor = clock.anchorFrame;
        row.completionFloor = clock.conditionalGroupFrame;
        if (old.selectedFutureConsumed) {
            auto& selected = queues.rows_[old.queue.slot];
            selected.Pending().pendingCompletion = rebound;
            selected.Pending().pendingLogicalCompletionFrame = row.backing.logicalFloor ? row.completionFloor : 0;
        }
        row.bound.store(true, std::memory_order_release);
        return true;
    }
    static bool SameArm(TimelineArmToken a, TimelineArmToken b) noexcept {
        return a.slot == b.slot && a.generation == b.generation &&
            a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
    }
    static bool SameDecision(DecisionToken a, DecisionToken b) noexcept {
        return a.slot == b.slot && a.generation == b.generation;
    }
    static bool SameOccurrence(OccurrenceToken a, OccurrenceToken b) noexcept {
        return a.slot == b.slot && a.generation == b.generation;
    }
    static bool SameState(const EffectiveQueueState& a, const EffectiveQueueState& b) noexcept {
        return a.version == b.version && a.cursor == b.cursor &&
            a.currentChildOrdinal == b.currentChildOrdinal && a.cycleOrdinal == b.cycleOrdinal &&
            a.selectionOrdinal == b.selectionOrdinal &&
            a.randomInitialRecentCount == b.randomInitialRecentCount &&
            a.randomInitialRecent == b.randomInitialRecent;
    }
    static bool FreshPrivateGroup(const ScheduledTransactionPool& pool,
        const OriginalQueueFutureGroupRegistration& original,
        const ScheduledTransactionPool::LaneGroupRow& row) noexcept {
        return pool.IsOriginalQueueFutureGroupRegistrationFor(original, original.Identity()) &&
            row.state.load(std::memory_order_acquire) == ScheduledTransactionPool::LaneGroupState::Private &&
            row.transferOriginal == nullptr && row.transferProof == nullptr && row.clockProof == nullptr &&
            row.cohortRoot == original.cohortRoot_ &&
            row.memberCount == original.memberCount_ &&
            row.sourceControlCount == original.sourceControlCount_ && row.relativeFrame ==
                original.executeFrame_ - original.provisionalAnchorFrame_ &&
            row.frontier.role == FrontierGroupRole::Legacy && !row.frontierPinsOwned &&
            row.readers.pins.load(std::memory_order_acquire) == 0 &&
            row.readers.lastAudioSequence.load(std::memory_order_acquire) == 0;
    }
    friend class PrivateSelectedQueueFutureCalendar;
    const Row* CalendarRow(const Owner& cap) const noexcept {
        return Matches(cap) ? &rows_[cap.slot_] : nullptr;
    }
    const Row* CalendarRow(const Retention& cap) const noexcept {
        return Matches(cap) ? &rows_[cap.slot_] : nullptr;
    }
    ScheduledTransactionPool* CalendarPool(const Row& row) const noexcept {
        return row.oldFuture.owner_->originalConditionalRows_[row.oldFuture.row_].clockPool;
    }
    bool HasSelectedCalendar(const Retention& retention, bool ended) const noexcept {
        const auto* selected = CalendarRow(retention);
        if (selected == nullptr || selected->closed.load(std::memory_order_acquire)) return false;
        const auto& queues = *selected->oldFuture.owner_;
        const auto& original = queues.originalConditionalRows_[selected->oldFuture.row_];
        return original.selectedFutureConsumed && original.selectedFutureOwner == this &&
            original.selectedFutureMarker == selected->marker &&
            SameDecision(original.selectedDecision, selected->backing.decision) &&
            original.selectedVersion.version == selected->backing.version.version &&
            (ended ? original.state == QueueInstanceTable::OriginalConditionalState::Retired
                   : queues.IsSelectedOriginalConditionalCurrent(selected->oldFuture));
    }
    bool MatchesCalendarDomain(const Row& row,
        const PrivateSelectedQueueFutureDomain& domain) const noexcept {
        const auto* pool = CalendarPool(row);
        const auto& old = row.oldFuture.owner_->originalConditionalRows_[row.oldFuture.row_];
        if (pool == nullptr || domain.owner_ != pool || !domain.oldC0_.Valid() ||
            !(domain.oldC0_.Identity() == old.clockGroups[0].Identity())) return false;
        for (const auto& original : row.newGroups) {
            if (!pool->MatchesOriginalQueueFutureGroupLifetime(original)) return false;
            const auto& group = pool->immediateLaneGroups_[original.Identity().committedRow];
            if (group.selectedFutureDomain != domain.marker_) return false;
        }
        return true;
    }
    bool CaptureCalendarFirstS(const Retention& retention,
        const QueueInstanceTable::OriginalConditionalLaneClock& actualClock,
        std::uint64_t* anchor) const noexcept {
        const auto* row = CalendarRow(retention);
        if (anchor == nullptr || row == nullptr ||
            !row->bound.load(std::memory_order_acquire) || row->closed.load(std::memory_order_acquire))
            return false;
        auto& queues = *row->oldFuture.owner_;
        if (!queues.IsOriginalConditionalLaneClockFor(actualClock, row->oldFuture, row->parent)) return false;
        const auto& old = queues.originalConditionalRows_[row->oldFuture.row_];
        if (old.clockState != QueueInstanceTable::OriginalConditionalClockState::Bound ||
            old.clockBinding.installedAckSequence == 0 ||
            old.clockBinding.anchorFrame != row->anchor ||
            old.clockBinding.conditionalGroupFrame != row->completionFloor) return false;
        *anchor = row->anchor;
        return true;
    }
    bool CaptureCalendarFirstS(const Retention& retention,
        const PrivateOriginalQueueGroupClockProof& actualClock,
        const TimelineArmTable& arms, const DecisionTable& decisions,
        std::uint64_t* anchor) const noexcept {
        const auto* row = CalendarRow(retention);
        if (anchor == nullptr || row == nullptr ||
            !row->bound.load(std::memory_order_acquire) ||
            row->closed.load(std::memory_order_acquire)) return false;
        const auto& old = row->oldFuture.owner_->originalConditionalRows_[row->oldFuture.row_];
        std::array<std::uint64_t, 4> frames{};
        const FrontierGroupBinding *initial = nullptr, *conditional = nullptr;
        if (old.clockPool == nullptr ||
            !old.clockPool->CapturePrivateOriginalQueueClockInitialSource(actualClock,
                old.clockGroups[0], old.clockGroups[1], arms, decisions,
                old.clockBinding.sourceArm, &frames, &initial, &conditional) ||
            frames[0] != row->anchor || frames[2] != row->completionFloor) return false;
        *anchor = frames[0];
        return true;
    }
    template<class Cap> bool Matches(const Cap& cap) const noexcept {
        if (cap.pool_ != this || !rows_.IsConstructed(cap.slot_) || cap.marker_ == 0) return false;
        const auto& row = rows_[cap.slot_];
        return row.phase != Phase::Free && row.phase != Phase::Quarantined &&
            row.generation == cap.generation_ && row.marker == cap.marker_ && row.loans != 0;
    }
    bool Matches(const Owner& cap) const noexcept {
        return Matches<Owner>(cap) && (rows_[cap.slot_].phase == Phase::Prepared ||
            rows_[cap.slot_].phase == Phase::FrozenUnbound);
    }
    template<class Cap> bool Borrow(const Cap& cap, Retention* out) noexcept {
        if (!Matches(cap) || out == nullptr || out->pool_ != nullptr) return false;
        auto& row = rows_[cap.slot_];
        if (row.loans == UINT32_MAX) return false;
        ++row.loans;
        out->pool_ = this;
        out->slot_ = cap.slot_;
        out->generation_ = cap.generation_;
        out->marker_ = cap.marker_;
        return true;
    }
    bool BorrowNative(const Owner& cap, NativeConditionalRead* out) noexcept {
        if (!Matches(cap) || rows_[cap.slot_].phase != Phase::FrozenUnbound ||
            out == nullptr || out->pool_ != nullptr) return false;
        auto& row = rows_[cap.slot_];
        if (row.loans == UINT32_MAX) return false;
        ++row.loans;
        out->pool_ = this;
        out->slot_ = cap.slot_;
        out->generation_ = cap.generation_;
        out->marker_ = cap.marker_;
        return true;
    }
    bool ReturnNative(NativeConditionalRead& cap) noexcept {
        if (!Matches(cap)) return false;
        auto& row = rows_[cap.slot_];
        if (row.nativeGate.load(std::memory_order_acquire)) return false;
        --row.loans;
        cap.pool_ = nullptr;
        Reclaim(row);
        return true;
    }
    void Close(Owner& cap) noexcept {
        const bool valid = Matches(cap);
        assert(valid);
        if (!valid) return;
        auto& row = rows_[cap.slot_];
        auto& old = row.oldFuture.owner_->originalConditionalRows_[row.oldFuture.row_];
        // A selected participant transfers lifetime to its continuing Queue.
        // The Engine retains this owner until that Queue's genuine native End.
        assert(!old.selectedFutureConsumed || old.state == QueueInstanceTable::OriginalConditionalState::Retired);
        if (old.selectedFutureConsumed && old.state != QueueInstanceTable::OriginalConditionalState::Retired) return;
        assert(old.selectedFutureOwner == this && old.selectedFutureMarker == row.marker);
        old.selectedFutureOwner = nullptr;
        old.selectedFutureMarker = 0;
        row.closed.store(true, std::memory_order_release);
        row.phase = Phase::Closed;
        --row.loans;
        cap.pool_ = nullptr;
        Reclaim(row);
    }
    void Return(Retention& cap) noexcept {
        const bool valid = Matches(cap);
        assert(valid);
        if (!valid) return;
        auto& row = rows_[cap.slot_];
        --row.loans;
        cap.pool_ = nullptr;
        Reclaim(row);
    }
    static void Reclaim(Row& row) noexcept {
        if (row.phase != Phase::Closed || row.loans != 0) return;
        assert(!row.nativeGate.load(std::memory_order_acquire));
        for (auto& group : row.newGroups) group.Release();
        row.newSuccessor.Release();
        row.sourceCompletion.Release();
        row.newDecision.Release();
        row.coordinator.Release();
        row.current.Release();
        row.currentDecision.Release();
        row.oldClock.Release();
        row.oldFuture.Release();
        row.parent.Release();
        row.value = {};
        row.backing = {};
        row.marker = 0;
        if (row.generation == UINT32_MAX) row.phase = Phase::Quarantined;
        else { ++row.generation; row.phase = Phase::Free; }
    }
    StableSlotStorage<Row> rows_;
    std::uint32_t capacity_ = 0;
    std::uint64_t nextMarker_ = 0;
};

inline bool PrivateSelectedQueueFutureSpanPool::Owner::Borrow(Retention* out) const noexcept {
    return pool_ != nullptr && pool_->Borrow(*this, out);
}
inline bool PrivateSelectedQueueFutureSpanPool::Owner::Capture(Observation* out) const noexcept {
    if (!Valid() || out == nullptr) return false;
    *out = pool_->rows_[slot_].value;
    return true;
}
inline bool PrivateSelectedQueueFutureSpanPool::Owner::BorrowNativeConditional(NativeConditionalRead* out) const noexcept {
    return pool_ != nullptr && pool_->BorrowNative(*this, out);
}
inline bool PrivateSelectedQueueFutureSpanPool::NativeConditionalRead::Capture(ConditionalBacking* out) const noexcept {
    if (!Valid() || out == nullptr) return false;
    *out = pool_->rows_[slot_].backing;
    return true;
}
inline bool PrivateSelectedQueueFutureSpanPool::NativeConditionalRead::Release() noexcept {
    return pool_ == nullptr || pool_->ReturnNative(*this);
}
inline PrivateSelectedQueueFutureSpanPool::Phase
PrivateSelectedQueueFutureSpanPool::Owner::State() const noexcept {
    return Valid() ? pool_->rows_[slot_].phase : Phase::Free;
}
inline bool PrivateSelectedQueueFutureSpanPool::Retention::Borrow(Retention* out) const noexcept {
    return pool_ != nullptr && pool_->Borrow(*this, out);
}
inline bool PrivateSelectedQueueFutureSpanPool::Retention::Capture(Observation* out) const noexcept {
    if (!Valid() || out == nullptr) return false;
    *out = pool_->rows_[slot_].value;
    return true;
}
} // namespace harken
#endif
