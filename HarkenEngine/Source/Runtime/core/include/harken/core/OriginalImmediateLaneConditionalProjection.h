#pragma once

#include "harken/core/ImmediateActionLane.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/PrivateSelectedQueueFutureSpan.h"

namespace harken {
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace detail { struct OriginalConditionalWithdrawalNegativeTestAccess; }
#endif

// One real original C1 projection. Structural ownership does not grant a
// Catalog selector, Current mutation, physical cancellation or a whole Seal.
class OriginalImmediateLaneConditionalProjection {
public:
    OriginalImmediateLaneConditionalProjection() noexcept = default;
    OriginalImmediateLaneConditionalProjection(const OriginalImmediateLaneConditionalProjection&) = delete;
    OriginalImmediateLaneConditionalProjection& operator=(const OriginalImmediateLaneConditionalProjection&) = delete;
    OriginalImmediateLaneConditionalProjection(OriginalImmediateLaneConditionalProjection&&) noexcept;
    OriginalImmediateLaneConditionalProjection& operator=(OriginalImmediateLaneConditionalProjection&&) noexcept;
    ~OriginalImmediateLaneConditionalProjection();
    bool Valid() const noexcept;
    // Borrow retains only storage; it never duplicates the factory withdrawal right.
    bool Borrow(OriginalImmediateLaneConditionalProjection* out) const noexcept;
    void Return() noexcept;
    static bool PrepareBound(QueueInstanceTable& queues,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const QueueInstanceTable::OriginalConditionalFutureReservation& conditional,
        const QueueInstanceTable::OriginalConditionalLaneClock& clock,
        ScheduledTransactionPool& pool, const OriginalQueueFutureGroupRegistration& primary,
        const OriginalQueueFutureGroupRegistration& member,
        const OriginalImmediateLaneAttemptLifetimePin& lane,
        OriginalImmediateLaneConditionalProjection* out) noexcept;
    // Reserve the authentic C1 association before placement or Source ACK.
    // The native groups must already belong to this original Lane. Publication
    // stays closed until BindPrepared authenticates the complete placed manifest.
    static bool PrepareUnbound(QueueInstanceTable& queues,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const QueueInstanceTable::OriginalConditionalFutureReservation& conditional,
        const QueueInstanceTable::OriginalConditionalLaneClock& clock,
        ScheduledTransactionPool& pool, const OriginalQueueFutureGroupRegistration& primary,
        const OriginalQueueFutureGroupRegistration& member,
        const OriginalImmediateLaneAttemptLifetimePin& lane,
        OriginalImmediateLaneConditionalProjection* out) noexcept;
    bool BindPrepared() noexcept;
    bool OwnsUnpublishedGroup(const ImmediateActionLane& lane, ImmediateLaneHandle handle,
        const Plan03GroupSliceIdentity& group) const noexcept;
    // Return this producer's structural loans after the real C1 Applied receipt
    // and a later completed callback. Native Lane/group retirement stays separate.
    bool ReturnAfterApplied(const ImmediateActionLane& lane, ImmediateLaneHandle handle,
        const DecisionTable& decisions, std::uint64_t completedAudioSequence) noexcept;
    // A monotonic request; success is not proof that audio did not win first.
    bool RequestWithdrawal() noexcept;
    bool TryCompleteNativeWithdrawal(DecisionTable& decisions,
        std::uint64_t completedAudioSequence) noexcept;
    // Ends only this original Waiting C1 after its genuine cancelled disposition.
    bool TryRetireNativeConditional(DecisionTable& decisions,
        std::uint64_t completedAudioSequence) noexcept;
private:
    friend class FrontierTimelineFinalizer;
    friend class OriginalConditionalReplacementBatch;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    friend struct detail::OriginalConditionalWithdrawalNegativeTestAccess;
#endif
    // Reads only the actual captured original C1 withdrawal association. It
    // does not issue a cancellation, lifetime, publication or apply right.
    static bool CapturedWithdrawalExcludesPair(const ImmediateActionLane& lane,
        ImmediateLaneHandle handle, const ImmediateLaneBucket* bucket,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const ImmediateLaneGroupRef& primary,
        const ImmediateLaneGroupRef& member) noexcept;
    bool BindDeclared(bool commit = true) noexcept;
    static bool PrepareOriginal(QueueInstanceTable& queues,
        const QueueInstanceTable::OriginalLifetimePin& parent,
        const QueueInstanceTable::OriginalConditionalFutureReservation& conditional,
        const QueueInstanceTable::OriginalConditionalLaneClock& clock,
        ScheduledTransactionPool& pool, const OriginalQueueFutureGroupRegistration& primary,
        const OriginalQueueFutureGroupRegistration& member,
        const OriginalImmediateLaneAttemptLifetimePin& lane,
        bool bound, OriginalImmediateLaneConditionalProjection* out) noexcept;
    bool ReturnNativeReferences(std::uint64_t completedAudioSequence) noexcept;
    friend struct Engine;
    OriginalImmediateLaneAttemptLifetimePin lane_;
    QueueInstanceTable::OriginalConditionalFutureReservation conditional_;
    std::array<OriginalQueueFutureGroupLifetimePin, 2> groups_{};
    ScheduledTransactionPool* pool_ = nullptr;
    FrontierEntryReceiptRef entry_{};
    bool withdrawalOwner_ = false;
};

// One bounded Reset candidate, retained until every native attachment is gone.
// Acceptance selects only the exact Old C1 disposition. It does not publish New,
// consume any Source, mutate Current, or prove physical cancellation.
class OriginalConditionalReplacementBatch {
public:
    explicit OriginalConditionalReplacementBatch(std::uint32_t capacity);
    OriginalConditionalReplacementBatch(const OriginalConditionalReplacementBatch&) = delete;
    OriginalConditionalReplacementBatch& operator=(const OriginalConditionalReplacementBatch&) = delete;
    ~OriginalConditionalReplacementBatch();
    bool Prepare(QueueInstanceTable& queues,
        QueueInstanceTable::PreparedCatalogResetScope* scope,
        LogicCommitCoordinator::PreparedIssue* sequence) noexcept;
    bool PrepareDeferred(QueueInstanceTable& queues,
        QueueInstanceTable::PreparedCatalogResetScope* scope,
        const PreparedActionFiring& originalChild) noexcept;
    const QueueInstanceTable::PreparedCatalogResetScope& Scope() const noexcept { return scope_; }
    const LogicCommitCoordinator::PreparedIssue& SequenceIssue() const noexcept { return sequence_; }
    bool Attach(const OriginalImmediateLaneConditionalProjection& original,
        PrivateSelectedQueueFutureSpanPool& spans,
        const PrivateSelectedQueueFutureSpanPool::Owner& selected) noexcept;
    bool TryAccept(const ActionCommitBundle& reservedAction) noexcept;
    // Prepare only the Queue/reset participant. The caller must retain every
    // other Action participant and publication proof through TryAccept.
    bool PrepareQueueSelections(std::uint64_t executeFrame,
        std::uint32_t executionOrdinal = 0) noexcept;
    bool PrepareQueueSelection(const OriginalImmediateLaneConditionalProjection& original,
        std::uint64_t executeFrame, std::uint32_t executionOrdinal) noexcept;
    LogicCommitSequence ConsumeAcceptedOrder(
        LogicCommitCoordinator::ConsumedIssue* consumed = nullptr) noexcept;
    bool Accepted() const noexcept;
    // Abandonment is reversible before acceptance. Busy native readers retain
    // this object for a later close; the audio thread never waits for logic.
    bool Close() noexcept;
private:
    friend class ImmediateActionLane;
    bool PrepareScope(QueueInstanceTable& queues,
        QueueInstanceTable::PreparedCatalogResetScope* scope) noexcept;
    LogicCommitSequence Sequence() const noexcept {
        return sequence_.Valid() ? sequence_.Sequence() : deferredChild_.InheritedSequence();
    }
    struct Member {
        OriginalImmediateLaneConditionalProjection original;
        PrivateSelectedQueueFutureSpanPool::Retention selected;
        PrivateSelectedQueueFutureSpanPool* spans = nullptr;
        ImmediateLaneAttempt* attempt = nullptr;
        QueueExecutionOrderV1 order{};
        bool orderPrepared = false;
    };
    bool BorrowOldRead(std::uint64_t generation) noexcept;
    void BeforeOldWrite() noexcept;
    void ReturnOldRead() noexcept;
    static constexpr std::uint64_t kAborted = 1u, kAccepted = 2u, kRead = 4u;
    std::unique_ptr<Member[]> members_;
    std::uint32_t capacity_ = 0, count_ = 0;
    QueueInstanceTable* queues_ = nullptr;
    QueueInstanceTable::PreparedCatalogResetScope scope_;
    LogicCommitCoordinator::PreparedIssue sequence_;
    PreparedActionFiring deferredChild_;
    std::uint64_t generation_ = 0;
    std::uint32_t queueSelectionsPrepared_ = 0;
    bool orderConsumed_ = false;
    std::atomic<std::uint64_t> word_{0};
};

} // namespace harken
