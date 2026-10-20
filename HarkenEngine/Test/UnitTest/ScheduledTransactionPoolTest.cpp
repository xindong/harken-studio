// Scheduled-transaction pool: a no-audio, purely deterministic simulator of the
// logic-side scheduling core.
//
// The suite drives a planner's motions -- reserve, publish, consume, advance --
// with no audio thread and no clock but the one it borrows for the last case, and
// pins the four guarantees the rest of the runtime gets to assume: an atomic
// reservation that lands whole or not at all, a publication that freezes, a total
// consume order that does not depend on how work arrived, and a one-check stale
// no-op for cancelled work. It also re-pins, one case each, every contract the
// retired page ring held, so nothing that used to be guaranteed quietly lapsed.
//
// Capacity is asserted at 95%, 100% and 100%+1, the exit-gate figures: the pool
// admits to the ceiling, refuses the whole over-ceiling transaction, and never
// moves its high-water mark on a refusal.

#include "Common/ObjectIdTestSupport.h"
#include "harken/core/ClockKernel.h"
#include "harken/core/ArmGroupTable.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/render/VoicePool.h"
#include "harken/render/FrontierTimelineFinalizer.h"
#include "harken/render/ImmediateLaneTimelineFinalizer.h"
#include "PlayableInstanceTableTestAccess.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken::detail {
struct ScheduledTransactionPoolCancelTestAccess {
    static void SetCurrentSliceCommitPageLimit(ScheduledTransactionPool& pool, std::size_t pages) {
        for (std::uint32_t page = 0; page < pool.pageCount_; ++page) {
            auto& storage = pool.pageCurrentSliceStorage_[page];
            storage.rows.SetCommitPageLimitForTests(pages);
            storage.order.SetCommitPageLimitForTests(pages);
        }
    }
    static void SetCurrentSliceSerial(ScheduledTransactionPool& pool, std::uint64_t serial) {
        pool.nextCurrentSliceSequence_ = serial;
    }
    static bool SetEmptyPageGeneration(ScheduledTransactionPool& pool,
        std::uint64_t frame, std::uint64_t generation) {
        const auto ring = Ring(pool, frame);
        if (generation == 0 || ring == UINT32_MAX ||
            pool.pageEventCount_[ring] != 0 || pool.pagePreparedEventCount_[ring] != 0 ||
            pool.pagePublicationHolds_[ring] != 0 || pool.pagePreparedPlan03GroupCount_[ring] != 0 ||
            pool.pagePlan03GroupCount_[ring] != 0 || pool.pageHeaders_[ring].frozen) return false;
        pool.pageHeaders_[ring].pageGeneration = generation;
        return true;
    }
    static bool SetFreeBatchGenerations(ScheduledTransactionPool& pool, std::uint32_t generation) {
        if (generation == 0) return false;
        for (const auto& claim : pool.preparedTransactionClaims_)
            if (claim.used || claim.transactionCount != 0 || claim.transferOriginal != nullptr ||
                claim.transferProof != nullptr) return false;
        for (auto& claim : pool.preparedTransactionClaims_) claim.generation = generation;
        return true;
    }
    static bool SetFreePageGroupGeneration(ScheduledTransactionPool& pool,
        std::size_t index, std::uint32_t generation) {
        if (generation == 0 || index >= pool.pagePlan03Groups_.size() ||
            pool.pagePlan03GroupRowStates_[index] != ScheduledTransactionPool::Plan03GroupRowState::Free ||
            pool.pagePlan03Groups_[index].transferOriginal != nullptr ||
            pool.pagePlan03Groups_[index].transferProof != nullptr ||
            pool.pagePlan03GroupReaders_[index].pins.load(std::memory_order_acquire) != 0 ||
            pool.pagePlan03GroupLeases_[index].Capture().state != AudioSliceState::Ready) return false;
        pool.pagePlan03GroupClaimGenerations_[index] = generation;
        return true;
    }
    static AudioSliceLease* BorrowPageGroupLease(ScheduledTransactionPool& pool,
        const Plan03GroupSliceIdentity& identity) {
        if (identity.committedRow >= pool.pagePlan03Groups_.size()) return nullptr;
        const auto& row = pool.pagePlan03Groups_[identity.committedRow];
        if (!(row.transferProspectiveIdentity == identity)) return nullptr;
        return &pool.pagePlan03GroupLeases_[identity.committedRow];
    }
    static bool SetFreeImmediateLaneGeneration(ScheduledTransactionPool& pool,
        std::uint32_t rowIndex, std::uint32_t generation) {
        if (rowIndex >= pool.config_.maxImmediateLaneGroupRows ||
            !pool.immediateLaneGroups_.ConstructThrough(rowIndex + 1u)) return false;
        auto& row = pool.immediateLaneGroups_[rowIndex];
        if (row.state.load(std::memory_order_acquire) !=
                ScheduledTransactionPool::LaneGroupState::Free ||
            row.transferOriginal != nullptr || row.transferProof != nullptr ||
            row.readers.pins.load(std::memory_order_acquire) != 0 ||
            row.lease.Capture().state != harken::AudioSliceState::Ready) return false;
        row.generation = generation;
        return true;
    }
    static std::uint32_t Ring(ScheduledTransactionPool& pool,
                              std::uint64_t frame) {
        std::uint32_t page = 0;
        if (!pool.PageForFrame(frame, &page)) return UINT32_MAX;
        return pool.RingIndex(page);
    }
    static std::uint64_t SetStamp(ScheduledTransactionPool& pool,
                                  std::uint64_t frame, bool empty,
                                  std::uint64_t value) {
        const auto ring = Ring(pool, frame);
        if (ring == UINT32_MAX) return UINT64_MAX;
        auto& stamp = empty ? pool.pageEmptyQuantumStamp_[ring]
                            : pool.pageQuantumStamp_[ring];
        return stamp.exchange(value, std::memory_order_acq_rel);
    }
    static std::uint32_t SetCommittedCount(ScheduledTransactionPool& pool,
                                            std::uint64_t frame,
                                            std::uint32_t value) {
        const auto ring = Ring(pool, frame);
        if (ring == UINT32_MAX) return UINT32_MAX;
        auto& count = pool.pageHeaders_[ring].committedTransactionCount;
        const auto old = count;
        count = value;
        return old;
    }
    static std::uint32_t SetBatchLive(ScheduledTransactionPool& pool,
                                       std::uint64_t frame,
                                       std::uint32_t value) {
        const auto ring = Ring(pool, frame);
        if (ring == UINT32_MAX) return UINT32_MAX;
        const auto base = static_cast<std::size_t>(ring) *
            pool.config_.maxFrameBatchesPerQuantum;
        for (std::uint32_t index = 0;
             index < pool.pageBatchOccupiedCount_[ring]; ++index) {
            auto& batch = pool.pageBatchRows_[base + index];
            if (batch.frame != frame) continue;
            const auto old = batch.liveReferences;
            batch.liveReferences = value;
            return old;
        }
        return UINT32_MAX;
    }
    static std::uint32_t SetLiveBatchCount(ScheduledTransactionPool& pool,
                                            std::uint64_t frame,
                                            std::uint32_t value) {
        const auto ring = Ring(pool, frame);
        if (ring == UINT32_MAX) return UINT32_MAX;
        auto& count = pool.pageBatchCount_[ring];
        const auto old = count;
        count = value;
        return old;
    }
    static std::uint64_t SetRowSequence(ScheduledTransactionPool& pool,
                                         std::uint64_t frame,
                                         std::uint64_t value) {
        const auto ring = Ring(pool, frame);
        if (ring == UINT32_MAX) return UINT64_MAX;
        const auto base = static_cast<std::size_t>(ring) *
            pool.config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0; index < pool.pageEventCount_[ring]; ++index) {
            auto& event = pool.pageEvents_[base + index];
            if (event.executeFrame != frame) continue;
            const auto old = event.rowSequence;
            event.rowSequence = value;
            return old;
        }
        return UINT64_MAX;
    }
};
}
#endif

using harken::ClockKernel;
using harken::ConsumedEvent;
using harken::MusicalTick;
using harken::OccurrenceKey;
using harken::OccurrenceStatePool;
using harken::OccurrenceToken;
using harken::PlayableInstanceHandle;
using harken::PlayableInstanceTable;
using harken::PlayableKind;
using harken::PrePublishLatePolicy;
using harken::PreparedTransactionBatch;
using harken::ReserveRefusal;
using harken::ReserveResult;
using harken::ReserveStatus;
using harken::SampleFrame;
using harken::ScheduledEventInput;
using harken::ScheduledTransactionPool;
using harken::TempoSegment;
using harken::TransactionOccurrenceRef;
using harken::TransactionRequest;
using harken::TransportEpoch;

namespace {

constexpr TransportEpoch kEpoch = 7;

static_assert(!std::is_copy_constructible_v<PreparedTransactionBatch>);
static_assert(!std::is_copy_assignable_v<PreparedTransactionBatch>);
static_assert(std::is_nothrow_move_constructible_v<PreparedTransactionBatch>);
static_assert(std::is_nothrow_move_assignable_v<PreparedTransactionBatch>);
static_assert(!std::is_copy_constructible_v<harken::HeldCurrentSliceToken>);
static_assert(!std::is_copy_assignable_v<harken::HeldCurrentSliceToken>);
static_assert(std::is_nothrow_move_constructible_v<harken::HeldCurrentSliceToken>);
static_assert(std::is_nothrow_move_assignable_v<harken::HeldCurrentSliceToken>);

// A small pool geometry with everything overridable per case. Defaults: four
// 100-frame pages, generous per-item ceilings.
ScheduledTransactionPool::Config MakeConfig() {
    ScheduledTransactionPool::Config c;
    c.quantumFrames = 100;
    c.scheduleHorizonFrames = 400; // 4 pages
    c.publishLeadQuanta = 1;
    c.maxScheduledTransactions = 8;
    c.maxTransactionSpanFrames = 400;
    c.maxIngressTransactionsPerQuantum = 8;
    c.maxFrameBatchesPerQuantum = 8;
    c.maxEventsPerQuantum = 8;
    c.maxTransactionEvents = 8;
    return c;
}

// Owns the identity tables and one live owner instance, since every reserved
// transaction must name a live owner or it would consume as stale.
struct Harness {
    PlayableInstanceTable instances{16};
    OccurrenceStatePool occurrences{16};
    PlayableInstanceHandle owner = 0;
    std::uint64_t ownerEpoch = 0;

    Harness() {
        owner = instances.Acquire(PlayableKind::Piece, 0);
        ownerEpoch = instances.Classify(owner).scheduleEpoch;
    }
};

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
struct PrivateUnboundPageArmHarness {
    std::unique_ptr<ScheduledTransactionPool> pool = std::make_unique<ScheduledTransactionPool>();
    std::unique_ptr<harken::TimelineArmTable> arms = std::make_unique<harken::TimelineArmTable>(2);
    harken::DecisionTable decisions{4, 1};
    std::array<harken::DecisionToken, 2> decision{};
    std::array<harken::TimelineArmToken, 2> members{};
    std::array<std::unique_ptr<harken::CommittedPlan03GroupSlice>, 2> originals{};
    std::array<harken::Plan03GroupSliceIdentity, 2> identities{};
    std::unique_ptr<harken::PrivateUnboundPageArmRetirementProof> proof =
        std::make_unique<harken::PrivateUnboundPageArmRetirementProof>();
    std::uint64_t frame = 10;
    void Prepare(std::uint64_t base = 0, bool maximumArm = false,
        harken::TimelineArmTable::OriginalArmRowOwner* origin = nullptr,
        std::size_t originMember = 0) {
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = 3;
        config.maxArmsPerGroup = 1;
        ASSERT_TRUE(pool->Reset(config, base, kEpoch));
        frame = base + 10;
        if (maximumArm) ASSERT_TRUE(arms->SetFreeGenerationForTests(0, UINT32_MAX));
        const harken::PreparedPageGroupTransferScope scope{17, 51, 7, 3};
        for (std::size_t i = 0; i < 2; ++i) {
            harken::DecisionReserveRequest request{};
            request.ticket.sequence = scope.sequence;
            request.world = scope.world;
            request.transport = kEpoch;
            request.expectedContinuationGeneration = 1;
            request.sliceCount = 1;
            ASSERT_TRUE(decisions.Reserve(request, &decision[i]));
            ASSERT_TRUE(arms->PrepareGrouped(static_cast<std::uint32_t>(i + 1), 2,
                frame, frame, 7, {}, {decision[i]}, 0, &members[i]));
            harken::Plan03GroupSliceInput input{};
            input.executeFrame = frame;
            input.decision = decision[i];
            input.members = &members[i];
            input.memberCount = 1;
            harken::PreparedPlan03GroupSlice original;
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                pool->PreparePlan03GroupSlice(input, *arms, &original, &scope));
            harken::PreparedPageGroupTransferProof groupProof;
            ASSERT_TRUE(pool->ProvePreparedPageGroupTransfer(original, scope, input, *arms, &groupProof));
            auto consumed = ScheduledTransactionPool::ConsumePreparedPageGroupTransfer(std::move(groupProof));
            identities[i] = consumed.identity;
            originals[i] = std::make_unique<harken::CommittedPlan03GroupSlice>(std::move(consumed.owner));
            harken::TimelineArmTable::PreparedCommitInput expected{};
            expected.arm = members[i];
            expected.startFrame = expected.armDeadline = frame;
            expected.programGeneration = 7;
            expected.parentGroup = {decision[i]};
            expected.memberIndex = 0;
            harken::TimelineArmTable::PreparedCommitProof armProof;
            ASSERT_TRUE(arms->ProvePreparedCommit(expected, &armProof));
            if (origin != nullptr && i == originMember)
                ASSERT_TRUE(arms->IssueOriginalArmRowOwner(armProof, origin));
            harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
        }
    }
    bool Prove() {
        return pool->ProvePrivateUnboundPageArmRetirement(*originals[0], *originals[1],
            *arms, members, frame, proof.get());
    }
    harken::AudioSliceLease* Lease(std::size_t i) {
        return harken::detail::ScheduledTransactionPoolCancelTestAccess::BorrowPageGroupLease(*pool, identities[i]);
    }
    void ConsumeAndReclaim() {
        ASSERT_TRUE(proof->Valid());
        ScheduledTransactionPool::ConsumePrivateUnboundPageArmRetirement(std::move(*proof));
        EXPECT_FALSE(proof->Valid());
        for (std::size_t i = 0; i < 2; ++i) {
            EXPECT_FALSE(originals[i]->Held());
            EXPECT_TRUE(originals[i]->Valid());
            EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, Lease(i)->Capture().state);
            EXPECT_NE(0u, Lease(i)->Capture().pendingReasonBits);
            harken::ArmState state{};
            EXPECT_FALSE(arms->State(members[i], &state));
            EXPECT_TRUE(pool->RollbackCommittedPlan03GroupSlice(originals[i].get()));
            EXPECT_TRUE(decisions.Cancel(decision[i]));
            EXPECT_TRUE(decisions.ReclaimCancelled(decision[i]));
        }
        EXPECT_EQ(0u, pool->CaptureLivePlan03Groups(nullptr, 0));
        EXPECT_EQ(0u, pool->CapturePrivateMaterializationSnapshotForTest().publicationHolds);
        EXPECT_EQ(0u, arms->LiveCount());
        EXPECT_EQ(0u, decisions.LiveCount());
    }
};

TEST(ScheduledTransactionPool, OriginalRowOwnerBlocksBothRealPrivatePageRetirementMembers) {
    for (std::size_t selected = 0; selected < 2; ++selected) {
        PrivateUnboundPageArmHarness h;
        harken::TimelineArmTable::OriginalArmRowOwner owner;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(0, selected == 0, &owner, selected));
        ASSERT_TRUE(owner.Valid());
        EXPECT_FALSE(h.Prove());
        EXPECT_FALSE(h.proof->Valid());
        for (std::size_t i = 0; i < 2; ++i) {
            EXPECT_TRUE(h.originals[i]->Valid());
            EXPECT_FALSE(h.originals[i]->Held());
            EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(i)->Capture().state);
            const auto* row = h.arms->Resolve(h.members[i]);
            ASSERT_NE(nullptr, row);
            EXPECT_EQ(harken::ArmState::PendingApply, row->state.load());
            EXPECT_EQ(i == selected, row->originalLifetimePinned.load());
            EXPECT_FALSE(h.arms->RowAt(h.members[i].slot)->reservation.test_and_set());
            h.arms->RowAt(h.members[i].slot)->reservation.clear();
        }
        EXPECT_EQ(2u, h.pool->CapturePrivateMaterializationSnapshotForTest().publicationHolds);
        owner.Release();
        ASSERT_TRUE(h.Prove());
        ASSERT_NO_FATAL_FAILURE(h.ConsumeAndReclaim());
        if (selected == 0)
            EXPECT_EQ(harken::ArmState::Quarantined, h.arms->RowAt(0)->state.load());
    }
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementHoldsAllRightsReleaseReproveConsumeReclaims) {
    PrivateUnboundPageArmHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    for (const auto& original : h.originals)
        ASSERT_TRUE(h.pool->CanReleaseCommittedPlan03GroupSliceOwnership(*original));
    ASSERT_TRUE(h.Prove());
    ASSERT_TRUE(h.proof->Valid());
    EXPECT_FALSE(h.Prove());
    harken::PrivateUnboundPageArmRetirementProof duplicate;
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(*h.originals[0], *h.originals[1],
        *h.arms, h.members, h.frame, &duplicate));
    harken::CommittedPlan03GroupSlice destination;
    destination = std::move(*h.originals[0]);
    EXPECT_FALSE(destination.Valid());
    EXPECT_TRUE(h.originals[0]->Held());
    harken::CommittedPlan03GroupSlice moved(std::move(*h.originals[0]));
    EXPECT_FALSE(moved.Valid());
    const auto before = h.pool->CapturePrivateMaterializationSnapshotForTest();
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_FALSE(h.pool->RollbackCommittedPlan03GroupSlice(h.originals[i].get()));
        EXPECT_FALSE(h.pool->CanReleaseCommittedPlan03GroupSliceOwnership(*h.originals[i]));
        EXPECT_FALSE(h.pool->ReleaseCommittedPlan03GroupSliceOwnership(h.originals[i].get()));
        EXPECT_FALSE(h.Lease(i)->ResetForReuse());
        EXPECT_FALSE(h.Lease(i)->TryClaim());
        (void)h.Lease(i)->Invalidate(harken::AudioSliceInvalidateReason::Cancel);
        EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, h.Lease(i)->Capture().state);
        EXPECT_FALSE(h.arms->RequestCancelNeverPublished(h.members[i]));
        EXPECT_FALSE(h.arms->RequestCancel(h.members[i]));
        EXPECT_FALSE(h.arms->Start(h.members[i], 1));
        harken::TimelineArmReaderPin pin;
        EXPECT_FALSE(h.arms->PinFrontierReader(h.members[i], &pin));
        harken::Plan03GroupReaderPin groupPin;
        EXPECT_FALSE(h.pool->AcquirePlan03GroupReader(h.identities[i], &groupPin));
    }
    EXPECT_FALSE(h.pool->Reset(MakeConfig(), 0, kEpoch + 1));
    EXPECT_FALSE(h.pool->CanResetForEpoch(0, kEpoch + 1));
    h.pool->DropPreparedOccupancy();
    const auto base = h.pool->WindowStartFrame();
    EXPECT_EQ(0u, h.pool->Advance());
    EXPECT_EQ(base, h.pool->WindowStartFrame());
    EXPECT_EQ(before.publicationHolds, h.pool->CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    EXPECT_TRUE(h.proof->Valid());
    h.proof->Release();
    EXPECT_FALSE(h.proof->Valid());
    EXPECT_FALSE(h.originals[0]->Held());
    for (const auto& original : h.originals)
        EXPECT_TRUE(h.pool->CanReleaseCommittedPlan03GroupSliceOwnership(*original));
    EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(0)->Capture().state);
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.ConsumeAndReclaim());
    EXPECT_EQ(2u, h.arms->FreeCount());
    EXPECT_TRUE(h.pool->ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementRejectsWrongScopeAndUnwindsActualArmPrefix) {
    PrivateUnboundPageArmHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    auto wrong = h.members;
    ++wrong[1].renderGeneration;
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(*h.originals[0], *h.originals[1],
        *h.arms, wrong, h.frame, h.proof.get()));
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(*h.originals[0], *h.originals[1],
        *h.arms, h.members, h.frame + 1, h.proof.get()));
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(*h.originals[1], *h.originals[0],
        *h.arms, h.members, h.frame, h.proof.get()));
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(*h.originals[0], *h.originals[0],
        *h.arms, h.members, h.frame, h.proof.get()));
    harken::TimelineArmTable otherArms(2);
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(*h.originals[0], *h.originals[1],
        otherArms, h.members, h.frame, h.proof.get()));
    harken::CommittedPlan03GroupSlice absent;
    EXPECT_FALSE(h.pool->ProvePrivateUnboundPageArmRetirement(absent, *h.originals[1],
        *h.arms, h.members, h.frame, h.proof.get()));
    auto* blocked = h.arms->Resolve(h.members[1]);
    ASSERT_NE(nullptr, blocked);
    ASSERT_FALSE(blocked->reservation.test_and_set(std::memory_order_acquire));
    EXPECT_FALSE(h.Prove());
    EXPECT_FALSE(h.proof->Valid());
    EXPECT_FALSE(h.originals[0]->Held());
    EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(0)->Capture().state);
    EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(1)->Capture().state);
    auto* first = h.arms->Resolve(h.members[0]);
    ASSERT_NE(nullptr, first);
    EXPECT_FALSE(first->reservation.test_and_set(std::memory_order_acquire));
    first->reservation.clear(std::memory_order_release);
    EXPECT_TRUE(blocked->reservation.test(std::memory_order_acquire));
    blocked->reservation.clear(std::memory_order_release);
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.ConsumeAndReclaim());
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementActualPinAndStartedStateRefuse) {
    for (const bool armPin : {false, true}) {
        PrivateUnboundPageArmHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        harken::TimelineArmReaderPin pin;
        harken::Plan03GroupReaderPin groupPin;
        if (armPin) ASSERT_TRUE(h.arms->PinFrontierReader(h.members[1], &pin));
        else ASSERT_TRUE(h.pool->AcquirePlan03GroupReader(h.identities[1], &groupPin));
        EXPECT_FALSE(h.Prove());
        EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(0)->Capture().state);
        if (armPin) ASSERT_TRUE(pin.Release(0));
        else ASSERT_TRUE(groupPin.Release(0));
        ASSERT_TRUE(h.Prove());
        ASSERT_NO_FATAL_FAILURE(h.ConsumeAndReclaim());
    }
    PrivateUnboundPageArmHarness started;
    ASSERT_NO_FATAL_FAILURE(started.Prepare());
    ASSERT_TRUE(started.arms->AcknowledgeApplied(started.members[1], 1));
    EXPECT_FALSE(started.Prove());
    EXPECT_EQ(harken::AudioSliceState::Ready, started.Lease(0)->Capture().state);
    EXPECT_FALSE(started.originals[0]->Held());
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementEveryBackingDestructionReleasesActualGrant) {
    for (int first = 0; first < 4; ++first) {
        PrivateUnboundPageArmHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        ASSERT_TRUE(h.Prove());
        if (first == 0) h.originals[0].reset();
        if (first == 1) h.pool.reset();
        if (first == 2) h.arms.reset();
        if (first == 3) h.proof.reset();
        if (h.proof) EXPECT_FALSE(h.proof->Valid());
        EXPECT_FALSE(h.originals[1]->Held());
        if (h.pool) EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(1)->Capture().state);
        if (h.arms) {
            auto* row = h.arms->Resolve(h.members[1]);
            ASSERT_NE(nullptr, row);
            EXPECT_FALSE(row->reservation.test_and_set(std::memory_order_acquire));
            row->reservation.clear(std::memory_order_release);
        }
    }
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementMaximumGenerationQuarantinesWithoutWrap) {
    PrivateUnboundPageArmHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(0, true));
    ASSERT_EQ(UINT32_MAX, h.members[0].generation);
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.ConsumeAndReclaim());
    EXPECT_EQ(harken::ArmState::Quarantined, h.arms->RowAt(0)->state.load());
    EXPECT_EQ(UINT32_MAX, h.arms->RowAt(0)->generation);
    EXPECT_EQ(1u, h.arms->FreeCount());
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementHeldWindowOverflowCannotMutateBacking) {
    PrivateUnboundPageArmHarness h;
    const auto base = (UINT64_MAX - 400) / 100 * 100;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(base));
    ASSERT_TRUE(h.Prove());
    EXPECT_EQ(0u, h.pool->Advance());
    EXPECT_EQ(base, h.pool->WindowStartFrame());
    EXPECT_TRUE(h.pool->Valid());
    EXPECT_TRUE(h.proof->Valid());
    ASSERT_NO_FATAL_FAILURE(h.ConsumeAndReclaim());
    EXPECT_EQ(0u, h.pool->Advance());
    EXPECT_FALSE(h.pool->Valid());
}

TEST(ScheduledTransactionPool, PrivateUnboundPageArmRetirementReusedOriginalCannotGrantLegacyGroupPrivateRights) {
    for (const bool rollback : {false, true}) {
        PrivateUnboundPageArmHarness h;
        h.arms = std::make_unique<harken::TimelineArmTable>(3);
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        auto* const reusedOriginal = h.originals[0].get();
        ASSERT_TRUE(h.pool->CanReleaseCommittedPlan03GroupSliceOwnership(*reusedOriginal));
        ScheduledTransactionPool foreign;
        ASSERT_TRUE(foreign.Reset(MakeConfig(), 0, kEpoch));
        EXPECT_FALSE(foreign.CanReleaseCommittedPlan03GroupSliceOwnership(*reusedOriginal));
        if (rollback) ASSERT_TRUE(h.pool->RollbackCommittedPlan03GroupSlice(reusedOriginal));
        else ASSERT_TRUE(h.pool->ReleaseCommittedPlan03GroupSliceOwnership(reusedOriginal));
        ASSERT_FALSE(reusedOriginal->Valid());
        EXPECT_FALSE(h.pool->CanReleaseCommittedPlan03GroupSliceOwnership(*reusedOriginal));
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = h.frame;
        input.decision = h.decision[0];
        input.members = &h.members[0];
        input.memberCount = 1;
        if (!rollback) {
            harken::DecisionReserveRequest request{};
            request.ticket.sequence = 17;
            request.world = 51;
            request.transport = kEpoch;
            request.expectedContinuationGeneration = 1;
            request.sliceCount = 1;
            ASSERT_TRUE(h.decisions.Reserve(request, &input.decision));
            ASSERT_TRUE(h.arms->PrepareGrouped(3, 2, h.frame, h.frame, 7, {},
                {input.decision}, 0, &h.members[0]));
        }
        harken::PreparedPlan03GroupSlice prepared;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            h.pool->PreparePlan03GroupSlice(input, *h.arms, &prepared));
        ASSERT_TRUE(h.pool->CommitPreparedPlan03GroupSlice(&prepared, reusedOriginal, true));
        if (!rollback) {
            harken::TimelineArmTable::PreparedCommitInput expected{};
            expected.arm = h.members[0];
            expected.startFrame = expected.armDeadline = h.frame;
            expected.programGeneration = 7;
            expected.parentGroup = {input.decision};
            expected.memberIndex = 0;
            harken::TimelineArmTable::PreparedCommitProof armProof;
            ASSERT_TRUE(h.arms->ProvePreparedCommit(expected, &armProof));
            harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
        }
        EXPECT_EQ(reusedOriginal, h.originals[0].get());
        EXPECT_TRUE(reusedOriginal->Valid());
        EXPECT_FALSE(h.Prove());
        EXPECT_FALSE(h.proof->Valid());
        EXPECT_FALSE(h.originals[1]->Held());
        EXPECT_EQ(harken::AudioSliceState::Ready, h.Lease(1)->Capture().state);
        EXPECT_TRUE(h.pool->RollbackCommittedPlan03GroupSlice(reusedOriginal));
        EXPECT_FALSE(h.pool->CanReleaseCommittedPlan03GroupSliceOwnership(*reusedOriginal));
        EXPECT_TRUE(h.pool->RollbackCommittedPlan03GroupSlice(h.originals[1].get()));
    }
}

struct PrivatePageGroupTransferHarness {
    ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{2};
    harken::DecisionTable decisions{2, 1};
    harken::DecisionToken decision{};
    harken::TimelineArmToken member{};
    harken::Plan03GroupSliceInput input{};
    harken::PreparedPageGroupTransferScope scope{17, 51, 7, 3};
    harken::PreparedPlan03GroupSlice original{};
    harken::PreparedPageGroupTransferProof proof{};
    harken::Plan03GroupSliceIdentity prospective{};

    void Prepare(bool oneMember = true, std::uint32_t headers = 2,
        std::uint32_t freeGeneration = 1) {
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = headers;
        config.maxArmsPerGroup = 1;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = scope.sequence;
        request.world = scope.world;
        request.transport = kEpoch;
        request.expectedContinuationGeneration = 1;
        request.sliceCount = 1;
        ASSERT_TRUE(decisions.Reserve(request, &decision));
        input.executeFrame = 10;
        input.decision = decision;
        if (oneMember) {
            ASSERT_TRUE(arms.PreparePendingImmediateGrouped(1, 2, 7, {},
                harken::ParentAudioGroupRef{decision}, 0, &member));
            input.members = &member;
            input.memberCount = 1;
        }
        ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetFreePageGroupGeneration(
            pool, 0, freeGeneration));
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            pool.PreparePlan03GroupSlice(input, arms, &original, &scope));
        ASSERT_TRUE(original.Valid());
        ASSERT_TRUE(original.CaptureProspectiveIdentity(&prospective));
    }
    bool Prove() {
        return pool.ProvePreparedPageGroupTransfer(original, scope, input, arms, &proof);
    }
    harken::AudioSliceLease* Lease() {
        return harken::detail::ScheduledTransactionPoolCancelTestAccess::BorrowPageGroupLease(pool, prospective);
    }
    void Reclaim() {
        proof.Release();
        if (original.Valid()) ASSERT_TRUE(pool.CancelPreparedPlan03GroupSlice(&original));
        EXPECT_EQ(0u, pool.CaptureLivePlan03Groups(nullptr, 0));
        const auto state = pool.CapturePrivateMaterializationSnapshotForTest();
        EXPECT_EQ(0u, state.preparedGroups);
        EXPECT_EQ(0u, state.preparedEvents);
        EXPECT_EQ(0u, state.publicationHolds);
        if (member.Valid()) ASSERT_TRUE(arms.CancelPrepared(member));
        ASSERT_TRUE(decisions.Cancel(decision));
        ASSERT_TRUE(decisions.ReclaimCancelled(decision));
        EXPECT_EQ(0u, arms.LiveCount());
        EXPECT_EQ(0u, decisions.LiveCount());
        EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    }
};

TEST(ScheduledTransactionPool, PrivatePageGroupTransferRealZeroAndOneFactoryReleaseReproveConsumeRollback) {
    for (const bool one : {false, true}) {
        PrivatePageGroupTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(one));
        harken::Plan03PageGroupView observation{};
        EXPECT_FALSE(h.pool.ResolvePlan03GroupSliceProducer(h.prospective, &observation));
        ASSERT_TRUE(h.Prove());
        EXPECT_TRUE(h.original.Held());
        EXPECT_TRUE(h.proof.MatchesOriginalScope(h.original, h.scope, h.input, h.arms));
        h.proof.Release();
        ASSERT_TRUE(h.original.Valid());
        EXPECT_FALSE(h.original.Held());
        ASSERT_TRUE(h.Prove());
        auto consumed = ScheduledTransactionPool::ConsumePreparedPageGroupTransfer(std::move(h.proof));
        EXPECT_EQ(h.prospective, consumed.identity);
        EXPECT_TRUE(consumed.owner.Valid());
        EXPECT_TRUE(h.pool.MatchesCommittedPlan03GroupSlice(consumed.owner, consumed.identity));
        harken::CommittedPlan03GroupSlice absentOwner{};
        EXPECT_FALSE(h.pool.MatchesCommittedPlan03GroupSlice(absentOwner, consumed.identity));
        for (unsigned field = 0; field < 8; ++field) {
            auto wrong = consumed.identity;
            if (field == 0) ++wrong.absoluteQuantum;
            if (field == 1) ++wrong.pageGeneration;
            if (field == 2) ++wrong.decision.slot;
            if (field == 3) ++wrong.decision.generation;
            if (field == 4) ++wrong.sliceIndex;
            if (field == 5) ++wrong.committedRow;
            if (field == 6) ++wrong.rowClaimGeneration;
            if (field == 7) ++wrong.transportEpoch;
            EXPECT_FALSE(h.pool.MatchesCommittedPlan03GroupSlice(consumed.owner, wrong));
        }

        EXPECT_FALSE(h.original.Valid());
        EXPECT_FALSE(h.proof.Valid());
        EXPECT_FALSE(h.Prove());
        ASSERT_TRUE(h.pool.ResolvePlan03GroupSliceProducer(consumed.identity, &observation));
        EXPECT_EQ(one ? 1u : 0u, observation.memberCount);
        EXPECT_EQ(harken::AudioSliceState::Ready, observation.lease->Capture().state);
        const auto state = h.pool.CapturePrivateMaterializationSnapshotForTest();
        EXPECT_EQ(0u, state.preparedGroups);
        EXPECT_EQ(1u, state.publicationHolds);
        ASSERT_TRUE(h.pool.RollbackCommittedPlan03GroupSlice(&consumed.owner));
        EXPECT_FALSE(h.pool.MatchesCommittedPlan03GroupSlice(consumed.owner, consumed.identity));
        EXPECT_FALSE(h.pool.RollbackCommittedPlan03GroupSlice(&consumed.owner));
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferCommittedOwnerQueryRejectsRealFailedResetAndReuse) {
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms{1};
    harken::DecisionTable decisions{1, 1};
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = 1;
    request.world = 7;
    request.transport = kEpoch;
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(request, &decision));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 10;
    input.decision = decision;
    harken::PreparedPlan03GroupSlice prepared{};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSlice(input, arms, &prepared));
    harken::CommittedPlan03GroupSlice committed{};
    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&prepared, &committed, false));
    harken::Plan03PageGroupView actual{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&actual, 1));
    ASSERT_TRUE(pool.MatchesCommittedPlan03GroupSlice(committed, actual.identity));
    auto invalid = config;
    invalid.quantumFrames = 0;
    ASSERT_FALSE(pool.Reset(invalid, 0, kEpoch));
    EXPECT_FALSE(pool.Valid());
    EXPECT_TRUE(committed.Valid());
    EXPECT_FALSE(pool.MatchesCommittedPlan03GroupSlice(committed, actual.identity));
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch + 1));
    EXPECT_FALSE(pool.MatchesCommittedPlan03GroupSlice(committed, actual.identity));
    EXPECT_EQ(0u, pool.CaptureLivePlan03Groups(nullptr, 0));
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().preparedGroups);
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_TRUE(decisions.Cancel(decision));
    ASSERT_TRUE(decisions.ReclaimCancelled(decision));
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_EQ(0u, arms.LiveCount());
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferSamePageRealBatchHoldAndGlobalDropAtomicRefusal) {
    PrivatePageGroupTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    Harness owner;
    std::array<ScheduledEventInput, 5> events{};
    std::array<TransactionRequest, 5> requests{};
    std::array<harken::ConsumedEvent, 5> actual{};
    for (std::size_t index = 0; index < requests.size(); ++index) {
        events[index].executeFrame = h.input.executeFrame;
        requests[index].transactionId = 100 + index;
        requests[index].transportEpoch = kEpoch;
        requests[index].ownerInstanceHandle = owner.owner;
        requests[index].ownerInstanceScheduleEpoch = owner.ownerEpoch;
        requests[index].events = &events[index];
        requests[index].eventCount = 1;
    }
    PreparedTransactionBatch batch{};
    ASSERT_EQ(ReserveRefusal::None, h.pool.PrepareTransactionBatch(requests.data(), requests.size(), 0, &batch));
    ASSERT_TRUE(h.pool.FreezePreparedTransactionBatch(batch, h.scope.sequence));
    for (std::uint32_t index = 0; index < actual.size(); ++index)
        ASSERT_TRUE(h.pool.CapturePreparedTransactionEvent(batch, index, 0, &actual[index]));
    ASSERT_TRUE(h.Prove());
    h.pool.DropPreparedOccupancy();
    const auto refused = h.pool.CapturePrivateMaterializationSnapshotForTest();
    EXPECT_EQ(1u, refused.preparedGroups);
    EXPECT_EQ(5u, refused.preparedEvents);
    EXPECT_EQ(5u, refused.publicationHolds);
    EXPECT_TRUE(h.proof.Valid());
    EXPECT_TRUE(h.pool.MatchesFrozenPreparedTransactionEvents(batch, h.scope.sequence, kEpoch, actual.data(), actual.size()));
    h.pool.Publish(100);
    EXPECT_FALSE(h.pool.IsFrozen(0));
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_EQ(0u, h.pool.WindowStartFrame());
    auto consumed = ScheduledTransactionPool::ConsumePreparedPageGroupTransfer(std::move(h.proof));
    EXPECT_EQ(6u, h.pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_TRUE(h.pool.RollbackCommittedPlan03GroupSlice(&consumed.owner));
    EXPECT_EQ(5u, h.pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_TRUE(h.pool.MatchesFrozenPreparedTransactionEvents(batch, h.scope.sequence, kEpoch, actual.data(), actual.size()));
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&batch));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferHeldMovesLegacyMutatorsResetAndBorrowedLeaseRefuse) {
    PrivatePageGroupTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    auto* borrowed = h.Lease();
    ASSERT_NE(nullptr, borrowed);
    ASSERT_TRUE(h.Prove());
    harken::PreparedPageGroupTransferProof duplicate{};
    EXPECT_FALSE(h.pool.ProvePreparedPageGroupTransfer(h.original, h.scope, h.input, h.arms, &duplicate));
    harken::PreparedPlan03GroupSlice moved(std::move(h.original));
    EXPECT_FALSE(moved.Valid());
    EXPECT_TRUE(h.original.Held());
    moved = std::move(h.original);
    h.original = std::move(moved);
    EXPECT_FALSE(moved.Valid());
    EXPECT_TRUE(h.original.Held());
    harken::PreparedPageGroupTransferProof movedProof(std::move(h.proof));
    EXPECT_FALSE(movedProof.Valid());
    EXPECT_TRUE(h.proof.Valid());
    movedProof = std::move(h.proof);
    h.proof = std::move(movedProof);
    EXPECT_TRUE(h.proof.Valid());
    EXPECT_FALSE(h.pool.CancelPreparedPlan03GroupSlice(&h.original));
    harken::CommittedPlan03GroupSlice committed{};
    EXPECT_FALSE(h.pool.CommitPreparedPlan03GroupSlice(&h.original, &committed, true));
    EXPECT_FALSE(committed.Valid());
    EXPECT_FALSE(h.pool.ResetForEpoch(0, kEpoch + 1));
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 2;
    config.maxArmsPerGroup = 1;
    EXPECT_FALSE(h.pool.Reset(config, 0, kEpoch));
    EXPECT_FALSE(borrowed->TryClaim());
    EXPECT_FALSE(borrowed->ResetForReuse());
    EXPECT_FALSE(borrowed->PrepareReplacement());
    EXPECT_FALSE(borrowed->CommitPreparedReplacement());
    EXPECT_FALSE(borrowed->CancelPreparedReplacement());
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::PrivateTransferHeld,
        borrowed->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_EQ(0u, borrowed->Capture().pendingReasonBits);
    EXPECT_TRUE(h.proof.MatchesOriginalScope(h.original, h.scope, h.input, h.arms));
    h.proof.Release();
    EXPECT_EQ(harken::AudioSliceState::Ready, borrowed->Capture().state);
    harken::PreparedPlan03GroupSlice unheld(std::move(h.original));
    EXPECT_FALSE(h.original.Valid());
    EXPECT_TRUE(unheld.CaptureProspectiveIdentity(&h.prospective));
    ASSERT_TRUE(h.pool.ProvePreparedPageGroupTransfer(unheld, h.scope, h.input, h.arms, &h.proof));
    h.proof.Release();
    h.original = std::move(unheld);
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferWrongFrozenScopeFrameAndFullArmRefuse) {
    PrivatePageGroupTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    for (unsigned field = 0; field < 4; ++field) {
        auto wrong = h.scope;
        if (field == 0) ++wrong.cohortRoot;
        if (field == 1) ++wrong.sequence;
        if (field == 2) ++wrong.world;
        if (field == 3) ++wrong.attemptGeneration;
        EXPECT_FALSE(h.pool.ProvePreparedPageGroupTransfer(h.original, wrong, h.input, h.arms, &h.proof));
    }
    for (unsigned field = 0; field < 7; ++field) {
        auto wrong = h.input;
        if (field == 0) ++wrong.executeFrame;
        if (field == 1) ++wrong.decision.slot;
        if (field == 2) ++wrong.decision.generation;
        if (field == 3) ++wrong.sliceIndex;
        if (field == 4) wrong.memberCount = 0;
        if (field == 5) wrong.sourceControlCount = 1;
        if (field == 6) wrong.immediateEntry.generation = 1;
        EXPECT_FALSE(h.pool.ProvePreparedPageGroupTransfer(h.original, h.scope, wrong, h.arms, &h.proof));
    }
    for (unsigned field = 0; field < 4; ++field) {
        auto member = h.member;
        if (field == 0) ++member.slot;
        if (field == 1) ++member.generation;
        if (field == 2) ++member.renderSlot;
        if (field == 3) ++member.renderGeneration;
        auto wrong = h.input;
        wrong.members = &member;
        EXPECT_FALSE(h.pool.ProvePreparedPageGroupTransfer(h.original, h.scope, wrong, h.arms, &h.proof));
    }
    harken::TimelineArmTable otherArms{2};
    EXPECT_FALSE(h.pool.ProvePreparedPageGroupTransfer(h.original, h.scope, h.input, otherArms, &h.proof));
    ASSERT_TRUE(h.Prove());
    auto wrong = h.scope;
    ++wrong.world;
    EXPECT_FALSE(h.proof.MatchesOriginalScope(h.original, wrong, h.input, h.arms));
    EXPECT_TRUE(h.proof.Valid());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferOriginalIndexAndRenderSurviveRealArmCancelReprepare) {
    for (const unsigned changedField : {0u, 1u}) {
        PrivatePageGroupTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        ASSERT_TRUE(h.Prove());
        const auto original = h.member;
        ASSERT_TRUE(h.arms.CancelPrepared(original));
        ASSERT_TRUE(h.arms.PreparePendingImmediateGrouped(original.renderSlot,
            original.renderGeneration + (changedField == 1 ? 1u : 0u), 7, {},
            harken::ParentAudioGroupRef{h.decision}, changedField == 0 ? 7 : 0, &h.member));
        EXPECT_EQ(original.slot, h.member.slot);
        EXPECT_NE(original.generation, h.member.generation);
        EXPECT_FALSE(h.proof.MatchesOriginalScope(h.original, h.scope, h.input, h.arms));
        EXPECT_TRUE(h.original.Held());
        h.proof.Release();
        EXPECT_FALSE(h.Prove());
        EXPECT_TRUE(h.original.Valid());
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferRealCancelLegacyCommitAndGlobalDropSpendGrant) {
    for (const unsigned method : {0u, 1u, 2u}) {
        PrivatePageGroupTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(false));
        if (method == 0) ASSERT_TRUE(h.pool.CancelPreparedPlan03GroupSlice(&h.original));
        if (method == 1) {
            harken::CommittedPlan03GroupSlice committed{};
            ASSERT_TRUE(h.pool.CommitPreparedPlan03GroupSlice(&h.original, &committed, true));
            ASSERT_TRUE(h.pool.RollbackCommittedPlan03GroupSlice(&committed));
        }
        if (method == 2) h.pool.DropPreparedOccupancy();
        EXPECT_FALSE(h.original.Valid());
        EXPECT_FALSE(h.Prove());
        harken::Plan03GroupSliceIdentity stale{};
        EXPECT_FALSE(h.original.CaptureProspectiveIdentity(&stale));
        harken::PreparedPlan03GroupSlice next{};
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            h.pool.PreparePlan03GroupSlice(h.input, h.arms, &next, &h.scope));
        harken::Plan03GroupSliceIdentity newer{};
        ASSERT_TRUE(next.CaptureProspectiveIdentity(&newer));
        EXPECT_NE(h.prospective, newer);
        EXPECT_FALSE(h.Prove());
        ASSERT_TRUE(h.pool.CancelPreparedPlan03GroupSlice(&next));
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferProofFirstAndOriginalFirstDestructionReleaseRealHold) {
    for (const bool originalFirst : {false, true}) {
        PrivatePageGroupTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(false));
        auto selected = std::make_unique<harken::PreparedPlan03GroupSlice>(std::move(h.original));
        auto proof = std::make_unique<harken::PreparedPageGroupTransferProof>();
        ASSERT_TRUE(h.pool.ProvePreparedPageGroupTransfer(*selected, h.scope, h.input, h.arms, proof.get()));
        auto* lease = h.Lease();
        ASSERT_NE(nullptr, lease);
        if (originalFirst) {
            selected.reset();
            EXPECT_FALSE(proof->Valid());
            proof.reset();
        } else {
            proof.reset();
            EXPECT_TRUE(selected->Valid());
            EXPECT_FALSE(selected->Held());
            ASSERT_TRUE(h.pool.ProvePreparedPageGroupTransfer(*selected, h.scope, h.input, h.arms, &h.proof));
            h.proof.Release();
            selected.reset();
        }
        EXPECT_EQ(harken::AudioSliceState::Ready, lease->Capture().state);
        EXPECT_EQ(1u, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedGroups);
        h.pool.DropPreparedOccupancy();
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferPoolFirstDestructionDetachesHeldOriginalAndProof) {
    PrivatePageGroupTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(false));
    ASSERT_TRUE(h.pool.CancelPreparedPlan03GroupSlice(&h.original));
    auto pool = std::make_unique<ScheduledTransactionPool>();
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ASSERT_TRUE(pool->Reset(config, 0, kEpoch));
    for (const bool held : {false, true}) {
        harken::PreparedPlan03GroupSlice original{};
        harken::PreparedPageGroupTransferProof proof{};
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            pool->PreparePlan03GroupSlice(h.input, h.arms, &original, &h.scope));
        if (held) ASSERT_TRUE(pool->ProvePreparedPageGroupTransfer(original, h.scope, h.input, h.arms, &proof));
        pool.reset();
        EXPECT_FALSE(original.Valid());
        EXPECT_FALSE(original.Held());
        EXPECT_FALSE(proof.Valid());
        proof.Release();
        pool = std::make_unique<ScheduledTransactionPool>();
        ASSERT_TRUE(pool->Reset(config, 0, kEpoch + 1));
    }
    pool.reset();
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageGroupTransferRealMaximumGenerationSaturatesAndRowRetiresWithoutWrap) {
    for (const auto generation : {UINT32_MAX - 1u, UINT32_MAX}) {
        PrivatePageGroupTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(false, 1, generation));
        EXPECT_EQ(UINT32_MAX, h.prospective.rowClaimGeneration);
        ASSERT_TRUE(h.Prove());
        auto consumed = ScheduledTransactionPool::ConsumePreparedPageGroupTransfer(std::move(h.proof));
        EXPECT_EQ(h.prospective, consumed.identity);
        ASSERT_TRUE(h.pool.RollbackCommittedPlan03GroupSlice(&consumed.owner));
        harken::PreparedPlan03GroupSlice refused{};
        EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
            h.pool.PreparePlan03GroupSlice(h.input, h.arms, &refused, &h.scope));
        EXPECT_FALSE(refused.Valid());
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
}
#endif

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(ScheduledTransactionPool, PrivateLaneTransferProofReservedSpansRequireAllFourRealFactoryMembers) {
    Harness owners;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 4;
    config.maxArmsPerGroup = 1;
    config.maxImmediateLaneGroupRows = 4;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms{4};
    harken::DecisionTable decisions{4, 1};
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 10, 10, config.quantumFrames, false, 6}));
    const harken::ImmediateLaneDemand demand{10, 6, 4, 1};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, demand, &handle));
    std::array<harken::DecisionToken, 4> tokens{};
    std::array<harken::TimelineArmToken, 4> actualArms{};
    std::array<harken::PreparedPrivateImmediateLaneGroup, 4> originals{};
    std::array<harken::Plan03GroupSliceIdentity, 4> identities{};
    std::array<harken::TimelineProgramView, 4> programs{};
    std::array<harken::ImmediateLaneMemberManifest, 4> members{};
    std::array<harken::ImmediateLaneEvent, 4> events{};
    for (unsigned i = 0; i < members.size(); ++i) {
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = 9;
        request.world = 7;
        request.transport = kEpoch;
        request.expectedContinuationGeneration = 1;
        request.sliceCount = 1;
        ASSERT_TRUE(decisions.Reserve(request, &tokens[i]));
        ASSERT_TRUE(arms.PreparePendingImmediateGrouped(i + 1u, 3, 9, {},
            harken::ParentAudioGroupRef{tokens[i]}, 0, &actualArms[i]));
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = 100;
        input.decision = tokens[i];
        input.members = &actualArms[i];
        input.memberCount = 1;
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms,
            &identities[i], &originals[i]));
        programs[i].generation = i + 9u;
        auto& member = members[i];
        member.group = identities[i];
        member.arm = actualArms[i];
        member.programGeneration = programs[i].generation;
        member.ownerInstance = owners.owner;
        member.ownerEpoch = owners.ownerEpoch;
        member.target.programSemanticFingerprint = i + 51u;
        events[i].event.program = {programs[i].generation, &programs[i]};
        events[i].event.armToken = actualArms[i];
        events[i].event.ownerInstanceHandle = owners.owner;
        events[i].event.ownerInstanceScheduleEpoch = owners.ownerEpoch;
        std::uint32_t ordinal = UINT32_MAX;
        ASSERT_TRUE(lane.DeclareMember(handle, member, &ordinal));
        EXPECT_EQ(i, ordinal);
        if (i < 3) {
            EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 0, members.data(), 2));
            EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 2, members.data() + 2, 2));
        }
    }
    ASSERT_EQ(4u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    EXPECT_TRUE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 4));
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    for (const auto first : {0u, 2u}) {
        EXPECT_TRUE(lane.MatchesReservedMemberSpan(handle, 17, demand, first, members.data() + first, 2));
        EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 18, demand, first, members.data() + first, 2));
        EXPECT_FALSE(lane.MatchesReservedMemberSpan({handle.slot, handle.generation + 1}, 17,
            demand, first, members.data() + first, 2));
    }
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 0, nullptr, 2));
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 3, members.data() + 2, 2));
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, UINT32_MAX, members.data(), 2));
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 0, members.data(), UINT32_MAX));
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 1, members.data(), 2));
    std::uint32_t refusedOrdinal = 92;
    EXPECT_FALSE(lane.DeclareMember(handle, members[3], &refusedOrdinal));
    EXPECT_EQ(92u, refusedOrdinal);
    for (unsigned field = 0; field < 4; ++field) {
        auto wrong = demand;
        if (field == 0) ++wrong.events;
        if (field == 1) ++wrong.groups;
        if (field == 2) ++wrong.members;
        if (field == 3) ++wrong.buckets;
        EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, wrong, 2, members.data() + 2, 2));
    }
    for (unsigned field = 0; field < 18; ++field) {
        std::array<harken::ImmediateLaneMemberManifest, 2> wrong{members[2], members[3]};
        auto& member = wrong[1];
        if (field == 0) ++member.group.rowClaimGeneration;
        if (field == 1) member.relatedGroup = members[0].group;
        if (field == 2) ++member.arm.slot;
        if (field == 3) ++member.arm.generation;
        if (field == 4) ++member.arm.renderSlot;
        if (field == 5) ++member.arm.renderGeneration;
        if (field == 6) member.eventOrdinal = 0;
        if (field == 7) ++member.eventSpanCount;
        if (field == 8) member.memberIndex = 0;
        if (field == 9) member.kind = harken::ImmediateLaneMemberKind::CurrentEventSpan;
        if (field == 10) member.future = true;
        if (field == 11) ++member.relativeFrame;
        if (field == 12) ++member.programGeneration;
        if (field == 13) ++member.target.programSemanticFingerprint;
        if (field == 14) ++member.target.occurrenceOrdinal;
        if (field == 15) ++member.ownerInstance;
        if (field == 16) ++member.ownerEpoch;
        if (field == 17) std::swap(wrong[0], wrong[1]);
        EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 2, wrong.data(), 2));
        EXPECT_TRUE(lane.MatchesReservedMemberSpan(handle, 17, demand, 2, members.data() + 2, 2));
    }
    ASSERT_TRUE(lane.Prepare(handle, events.data(), events.size()));
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 0, members.data(), 2));
    refusedOrdinal = 92;
    EXPECT_FALSE(lane.DeclareMember(handle, members[0], &refusedOrdinal));
    EXPECT_EQ(92u, refusedOrdinal);
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(17, demand, &reused));
    ASSERT_EQ(handle.slot, reused.slot);
    ASSERT_NE(handle.generation, reused.generation);
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(handle, 17, demand, 0, members.data(), 2));
    EXPECT_FALSE(lane.MatchesReservedMemberSpan(reused, 17, demand, 0, members.data(), 2));
    ASSERT_TRUE(lane.Cancel(reused));
    ASSERT_TRUE(lane.Retire(reused));
    for (unsigned i = 0; i < originals.size(); ++i) {
        ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(&originals[i], &identities[i]));
        ASSERT_TRUE(arms.CancelPrepared(actualArms[i]));
        ASSERT_TRUE(decisions.Cancel(tokens[i]));
        ASSERT_TRUE(decisions.ReclaimCancelled(tokens[i]));
    }
    EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_EQ(0u, arms.LiveCount());
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, PrivateLaneEntriesRetainActualFactorySourcesAndOriginalSpansThroughReprove) {
    Harness owners;
    ScheduledTransactionPool pool, foreignPool;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 6;
    config.maxArmsPerGroup = 1;
    config.maxImmediateLaneGroupRows = 6;
    config.maxImmediateLaneOwnerGates = 6;
    config.maxImmediateLaneDecisionGates = 6;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    ASSERT_TRUE(foreignPool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms{4};
    harken::DecisionTable decisions{6, 1}, foreignDecisions{6, 1};
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 10, 10, config.quantumFrames, false, 6}));
    const harken::ImmediateLaneDemand demand{10, 6, 4, 1};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, demand, &handle));
    std::array<std::array<harken::DecisionToken, 3>, 2> tokens{}, foreignTokens{};
    std::array<harken::TimelineArmToken, 4> actualArms{};
    std::array<std::array<harken::PreparedPrivateImmediateLaneGroup, 3>, 2> originals{}, foreignOriginals{};
    std::array<std::array<harken::PrivateImmediateLaneGroupTransferProof, 3>, 2> proofs{}, foreignProofs{};
    std::array<std::array<harken::Plan03GroupSliceIdentity, 3>, 2> identities{}, foreignIdentities{};
    std::array<harken::ImmediateLaneMemberManifest, 4> members{};
    std::array<harken::ImmediateLanePrivateVariableControlEntryScope, 2> scopes{};
    for (unsigned entry = 0; entry < scopes.size(); ++entry) {
        auto& scope = scopes[entry];
        scope.provisionalFrame = 100;
        scope.firstMember = entry * 2;
        scope.memberCount = 2;
        scope.members = members.data() + scope.firstMember;
        auto& request = scope.coordinatorRequest;
        request.ticket.sequence = 9;
        request.world = 7;
        request.transport = kEpoch;
        request.expectedContinuationGeneration = entry + 1;
        request.sliceCount = 1;
        for (unsigned group = 0; group < 3; ++group) {
            ASSERT_TRUE(decisions.Reserve(request, &tokens[entry][group]));
            ASSERT_TRUE(foreignDecisions.Reserve(request, &foreignTokens[entry][group]));
            EXPECT_EQ(tokens[entry][group].slot, foreignTokens[entry][group].slot);
            EXPECT_EQ(tokens[entry][group].generation, foreignTokens[entry][group].generation);
            auto& input = scope.groups[group];
            input.executeFrame = 100;
            input.decision = tokens[entry][group];
            if (group < 2) {
                const auto member = entry * 2 + group;
                ASSERT_TRUE(arms.PreparePendingImmediateGrouped(member + 1, 3, 9, {},
                    harken::ParentAudioGroupRef{input.decision}, 0, &actualArms[member]));
                input.members = &actualArms[member];
                input.memberCount = 1;
            }
            ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms,
                &identities[entry][group], &originals[entry][group]));
            ASSERT_TRUE(foreignPool.ReserveImmediateLaneGroup(17, 100, input, arms,
                &foreignIdentities[entry][group], &foreignOriginals[entry][group]));
            EXPECT_EQ(identities[entry][group], foreignIdentities[entry][group]);
            ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(originals[entry][group],
                17, 100, input, arms, &proofs[entry][group]));
            ASSERT_TRUE(foreignPool.ProvePrivateImmediateLaneGroupTransfer(foreignOriginals[entry][group],
                17, 100, input, arms, &foreignProofs[entry][group]));
            if (group < 2) {
                const auto index = entry * 2 + group;
                auto& manifest = members[index];
                manifest.group = identities[entry][group];
                manifest.arm = actualArms[index];
                manifest.programGeneration = 9 + index;
                manifest.ownerInstance = owners.owner;
                manifest.ownerEpoch = owners.ownerEpoch;
                manifest.target.programSemanticFingerprint = index + 51;
                ASSERT_TRUE(lane.DeclareMember(handle, manifest));
            }
        }
        scope.entry = {tokens[entry][2], entry + 1u,
            {owners.owner, owners.ownerEpoch, 0, 0, owners.owner, owners.ownerEpoch, entry, entry + 1u}};
    }
    std::array<std::uint32_t, 2> ordinals{UINT32_MAX, UINT32_MAX};
    for (unsigned entry = 0; entry < scopes.size(); ++entry) {
        auto& scope = scopes[entry];
        auto wrong = scope;
        ++wrong.coordinatorRequest.expectedContinuationGeneration;
        std::uint32_t refused = 73;
        EXPECT_FALSE(lane.DeclarePrivateVariableControlEntry(handle, 17, demand, wrong,
            pool, arms, decisions, originals[entry], proofs[entry], &refused));
        EXPECT_EQ(73u, refused);
        proofs[entry][1].Release();
        EXPECT_FALSE(lane.DeclarePrivateVariableControlEntry(handle, 17, demand, scope,
            pool, arms, decisions, originals[entry], proofs[entry], &refused));
        EXPECT_EQ(73u, refused);
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(originals[entry][1],
            17, 100, scope.groups[1], arms, &proofs[entry][1]));
        for (unsigned group = 0; group < 3; ++group) {
            EXPECT_TRUE(foreignPool.MatchesHeldPrivateImmediateLaneGroupOriginal(
                foreignOriginals[entry][group], foreignProofs[entry][group], 17, 100,
                scope.groups[group], arms));
            EXPECT_FALSE(pool.MatchesHeldPrivateImmediateLaneGroupOriginal(
                foreignOriginals[entry][group], foreignProofs[entry][group], 17, 100,
                scope.groups[group], arms));
        }
        EXPECT_FALSE(lane.DeclarePrivateVariableControlEntry(handle, 17, demand, scope,
            pool, arms, decisions, foreignOriginals[entry], foreignProofs[entry], &refused));
        EXPECT_EQ(73u, refused);
        ASSERT_TRUE(lane.DeclarePrivateVariableControlEntry(handle, 17, demand, scope,
            pool, arms, decisions, originals[entry], proofs[entry], &ordinals[entry]));
        EXPECT_EQ(entry, ordinals[entry]);
        EXPECT_FALSE(lane.DeclarePrivateVariableControlEntry(handle, 17, demand, scope,
            pool, arms, decisions, originals[entry], proofs[entry], &refused));
        EXPECT_EQ(73u, refused);
        EXPECT_TRUE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry],
            scope, pool, arms, decisions, originals[entry], proofs[entry]));
        EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry],
            scope, foreignPool, arms, decisions, foreignOriginals[entry], foreignProofs[entry]));
        EXPECT_TRUE(foreignDecisions.MatchesOpenReservation(scope.entry.coordinator, scope.coordinatorRequest));
        EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry],
            scope, pool, arms, foreignDecisions, originals[entry], proofs[entry]));
        wrong = scope;
        ++wrong.entry.owner.serviceGeneration;
        EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry],
            wrong, pool, arms, decisions, originals[entry], proofs[entry]));
        EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry] + 1,
            scope, pool, arms, decisions, originals[entry], proofs[entry]));
        for (auto& proof : proofs[entry]) proof.Release();
        EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry],
            scope, pool, arms, decisions, originals[entry], proofs[entry]));
        for (unsigned group = 0; group < 3; ++group)
            ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(originals[entry][group],
                17, 100, scope.groups[group], arms, &proofs[entry][group]));
        EXPECT_TRUE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, ordinals[entry],
            scope, pool, arms, decisions, originals[entry], proofs[entry]));
    }
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(17, demand, &reused));
    EXPECT_NE(handle.generation, reused.generation);
    EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(handle, 17, demand, 0,
        scopes[0], pool, arms, decisions, originals[0], proofs[0]));
    for (const auto& manifest : members) ASSERT_TRUE(lane.DeclareMember(reused, manifest));
    EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(reused, 17, demand, 0,
        scopes[0], pool, arms, decisions, originals[0], proofs[0]));
    std::uint32_t newOrdinal = UINT32_MAX;
    ASSERT_TRUE(lane.DeclarePrivateVariableControlEntry(reused, 17, demand, scopes[1],
        pool, arms, decisions, originals[1], proofs[1], &newOrdinal));
    EXPECT_EQ(0u, newOrdinal);
    harken::TransitionInstancePool transitions{1};
    harken::TransitionInstanceToken actualGate{};
    ASSERT_TRUE(transitions.Acquire({200, 2}, &actualGate));
    ASSERT_TRUE(decisions.AttachTransitionGateBeforeCommit(
        scopes[1].entry.coordinator, actualGate, 2));
    auto changedScope = scopes[1];
    changedScope.coordinatorRequest.transitionGate = actualGate;
    changedScope.coordinatorRequest.sliceCount = 2;
    EXPECT_TRUE(decisions.MatchesOpenReservation(changedScope.entry.coordinator,
        changedScope.coordinatorRequest));
    EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(reused, 17, demand, newOrdinal,
        changedScope, pool, arms, decisions, originals[1], proofs[1]));
    EXPECT_FALSE(lane.MatchesPrivateVariableControlEntry(reused, 17, demand, newOrdinal,
        scopes[1], pool, arms, decisions, originals[1], proofs[1]));
    ASSERT_TRUE(lane.Cancel(reused));
    ASSERT_TRUE(lane.Retire(reused));
    for (unsigned entry = 0; entry < scopes.size(); ++entry) {
        for (unsigned group = 0; group < 3; ++group) {
            proofs[entry][group].Release();
            foreignProofs[entry][group].Release();
            ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(&originals[entry][group], &identities[entry][group]));
            ASSERT_TRUE(foreignPool.CancelPrivateImmediateLaneGroup(&foreignOriginals[entry][group], &foreignIdentities[entry][group]));
        }
    }
    for (const auto arm : actualArms) ASSERT_TRUE(arms.CancelPrepared(arm));
    for (unsigned entry = 0; entry < scopes.size(); ++entry) {
        for (unsigned group = 0; group < 3; ++group) {
            ASSERT_TRUE(decisions.Cancel(tokens[entry][group]));
            ASSERT_TRUE(decisions.ReclaimCancelled(tokens[entry][group]));
            ASSERT_TRUE(foreignDecisions.Cancel(foreignTokens[entry][group]));
            ASSERT_TRUE(foreignDecisions.ReclaimCancelled(foreignTokens[entry][group]));
        }
    }
    ASSERT_TRUE(transitions.CancelBeforePublication(actualGate));
    EXPECT_EQ(0u, transitions.LiveCount());
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_EQ(0u, foreignDecisions.LiveCount());
    EXPECT_EQ(0u, arms.LiveCount());
    EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    EXPECT_EQ(0u, foreignPool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    EXPECT_TRUE(foreignPool.ResetForEpoch(0, kEpoch + 1));
}

#endif

TEST(ScheduledTransactionPool, AcceptedPrivateEntryRequiresNestedControllersAndOriginalNaturalTargetKind) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        const bool replaceController = mode == 1;
        SCOPED_TRACE(mode);
        Harness owners;
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = 8;
        config.maxArmsPerGroup = 2;
        config.maxImmediateLaneGroupRows = 8;
        config.maxImmediateLaneOwnerGates = 5;
        config.maxImmediateLaneDecisionGates = 8;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms{4};
        harken::DecisionTable decisions{8, 4};
        harken::ArmGroupTable armGroups{2, 1};
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 11, 11, config.quantumFrames, true, 8}));
        const harken::ImmediateLaneDemand demand{11, 8, 6, 3};
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(17, demand, &handle));
        const harken::DecisionReserveRequest request{{9}, 7, kEpoch, 1, 1, {}};
        std::array<harken::DecisionToken, 8> tokens{};
        std::array<harken::Plan03GroupSliceIdentity, 8> identities{};
        std::array<harken::OriginalQueueFutureGroupRegistration, 8> originals{};
        std::array<harken::ImmediateLaneAcceptedPrivateGroup, 8> groups{};
        std::array<harken::ImmediateLaneMemberManifest, 6> members{};
        std::array<harken::TimelineArmToken, 4> actualArms{};
        std::array<harken::TimelineEventPayload, 5> payloads{};
        std::array<harken::TimelineProgramView, 4> programs{};
        std::array<const harken::TimelineProgramView*, 6> controls{};
        for (unsigned index = 0; index < tokens.size(); ++index) {
            auto groupRequest = request;
            if (index >= 5) groupRequest.expectedContinuationGeneration = 2;
            if (index >= 6) groupRequest.sliceCount = 2;
            if (index == 7) tokens[index] = tokens[6];
            else ASSERT_TRUE(decisions.Reserve(groupRequest, &tokens[index]));
            harken::Plan03GroupSliceInput input{};
            const auto relativeFrame = index < 5 ? 0u : index == 7 ? 200u : 100u;
            input.executeFrame = 100 + relativeFrame;
            input.decision = tokens[index];
            input.sliceIndex = index == 7 ? 1u : 0u;
            if (index < 2) {
                auto& member = members[index];
                member.programGeneration = 40 + index;
                member.ownerInstance = owners.owner;
                member.ownerEpoch = owners.ownerEpoch;
                member.target.programSemanticFingerprint = 50 + index;
                member.memberIndex = 0;
                ASSERT_TRUE(arms.PreparePendingImmediateGrouped(index + 1, 3,
                    member.programGeneration, member.target,
                    harken::ParentAudioGroupRef{input.decision}, 0, &actualArms[index]));
                member.arm = actualArms[index];
                input.members = &actualArms[index];
                input.memberCount = 1;
                payloads[index].kind = harken::TimelineEventKind::Start;
                payloads[index].target = member.target;
                programs[index] = {member.programGeneration, &payloads[index], 1};
            }
            if (index == 6) {
                for (unsigned side = 0; side < 2; ++side) {
                    auto& member = members[side + 2];
                    member.programGeneration = 42 + side;
                    member.ownerInstance = owners.owner;
                    member.ownerEpoch = owners.ownerEpoch;
                    member.target.programSemanticFingerprint = 52 + side;
                    member.memberIndex = side;
                    member.kind = side == 0 ? harken::ImmediateLaneMemberKind::CurrentEventSpan :
                        harken::ImmediateLaneMemberKind::FrozenFutureTarget;
                    member.relativeFrame = side == 0 ? 100 : 200;
                    member.eventSpanCount = side == 0 ? 1 : 2;
                    ASSERT_TRUE(arms.PreparePendingImmediateGrouped(side + 3, 3,
                        member.programGeneration, member.target, harken::ParentAudioGroupRef{input.decision},
                        side, &actualArms[side + 2]));
                    member.arm = actualArms[side + 2];
                }
                payloads[2].kind = harken::TimelineEventKind::Envelope;
                payloads[2].envelopeDirection = harken::TimelineEnvelopeDirection::FadeOut;
                payloads[2].target = members[2].target;
                payloads[3].kind = harken::TimelineEventKind::Start;
                payloads[3].target = members[3].target;
                payloads[4].kind = harken::TimelineEventKind::Envelope;
                payloads[4].envelopeDirection = harken::TimelineEnvelopeDirection::FadeIn;
                payloads[4].target = members[3].target;
                programs[2] = {42, &payloads[2], 1};
                programs[3] = {43, &payloads[3], 2};
            }
            if (index >= 6) {
                input.members = actualArms.data() + 2;
                input.memberCount = 2;
                input.sourceControlCount = 1;
            }
            harken::PreparedPrivateImmediateLaneGroup prepared;
            harken::PrivateImmediateLaneGroupTransferProof proof;
            ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &identities[index], &prepared));
            ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(prepared, 17, 100, input, arms, &proof));
            ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &originals[index]));
            groups[index].original = &originals[index];
            groups[index].schedule = {identities[index], relativeFrame};
            ASSERT_TRUE(originals[index].CaptureOriginalScope(pool, arms, &groups[index].scope));
            ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(identities[index], lane, 17, handle.slot, handle.generation));
            ASSERT_TRUE(lane.DeclareGroup(handle, groups[index].schedule));
            if (index < 2) {
                members[index].group = identities[index];
                ASSERT_TRUE(lane.DeclareMember(handle, members[index]));
            }
            if (index < controls.size()) {
                controls[index] = decisions.PrepareFrontierControlProgram(tokens[index], 99);
                ASSERT_NE(nullptr, controls[index]);
            }
        }
        members[2].group = members[3].group = identities[6];
        members[3].relatedGroup = identities[7];
        members[4] = members[2];
        members[4].kind = harken::ImmediateLaneMemberKind::HistoricalSourceCompletion;
        members[4].group = identities[7];
        members[4].relatedGroup = identities[6];
        members[5] = members[3];
        members[5].kind = harken::ImmediateLaneMemberKind::CurrentEventSpan;
        members[5].group = identities[7];
        members[5].relatedGroup = {};
        for (unsigned index = 2; index < members.size(); ++index)
            ASSERT_TRUE(lane.DeclareMember(handle, members[index]));
        harken::ImmediateLaneAcceptedPrivateEntryScope scope{};
        scope.entry = {tokens[2], 1, {owners.owner, owners.ownerEpoch, 0, 0,
            owners.owner, owners.ownerEpoch, 0, 1}};
        scope.coordinatorRequest = request;
        scope.provisionalFrame = 100;
        scope.firstMember = 0;
        scope.memberCount = members.size();
        scope.members = members.data();
        scope.outerGroups = {identities[0], identities[1], identities[2]};
        scope.groups = groups.data();
        scope.groupCount = groups.size();
        std::uint32_t ordinal = UINT32_MAX;
        ASSERT_TRUE(lane.DeclareAcceptedPrivateVariableControlEntry(handle, 17, demand,
            scope, pool, arms, decisions, &ordinal));
        const harken::FrontierEntryReceiptRef childEntry{tokens[5], 2, scope.entry.owner};
        std::array<harken::ImmediateLaneEvent, 11> events{};
        constexpr std::array<unsigned, 11> eventGroups{2, 0, 1, 3, 4, 0, 1, 5, 6, 7, 7};
        for (unsigned index = 0; index < events.size(); ++index) {
            auto& event = events[index].event;
            const auto group = eventGroups[index];
            const auto* program = index < 5 || index == 7 ? controls[group] :
                &programs[index < 7 ? group : index == 8 ? 2 : 3];
            event.program = {program->generation, program};
            if (index >= 5 && index != 7) event.armToken = actualArms[index < 7 ? group : index == 8 ? 2 : 3];
            event.logicCommitSequence = 9;
            event.executionOrdinal = 1;
            event.eventIndex = index;
            event.transactionOrdinal = index;
            event.subeventPhase = index < 5 || index == 7 ? 10u : index == 8 ? 30u : index == 10 ? 40u : 20u;
            event.rowSequence = index + 1;
            event.transactionId = 9;
            event.ownerInstanceHandle = owners.owner;
            event.ownerInstanceScheduleEpoch = owners.ownerEpoch;
            event.frozenTxnValid = true;
            event.frontier.kind = harken::FrontierOperationKind::Entry;
            event.frontier.entry = scope.entry;
            event.frontier.owner = scope.entry.owner;
            event.frontier.group = identities[group];
            event.frontier.transport = kEpoch;
            event.frontier.world = 7;
            event.frontier.operationEventCount = 1;
            event.frontier.entryEventOrdinal = index;
            if (index >= 7) {
                if (mode == 3 && index != 7) event.executionOrdinal = 0;
                events[index].relativeFrame = index < 9 ? 100u : 200u;
                event.frontier.entry = childEntry;
                event.frontier.entryEventOrdinal = index < 9 ? index - 7u : index - 9u;
                event.frontier.operationEventCount = index == 7 ? 1u : 2u;
                if (index != 7) event.frontier.kind = harken::FrontierOperationKind::Dependent;
                events[index].futureGroupedStart = index == 9;
                if (index == 10) event.payload = 1;
            }
        }
        const auto originalEvents = events;
        if (replaceController) {
            events[4] = events[3];
            events[4].event.frontier.entryEventOrdinal = 4;
        }
        if (mode == 2) {
            events[9].event.frontier.kind = harken::FrontierOperationKind::Genuine;
            events[10].event.frontier.kind = harken::FrontierOperationKind::Genuine;
        }
        if (mode == 4) events[9].event.executionOrdinal = 0;
        ASSERT_TRUE(lane.Prepare(handle, events.data(), events.size()));
        std::uint64_t anchor = 0;
        ASSERT_TRUE(lane.TryPlace(handle, 200, 0, &anchor));
        ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, anchor));
        for (const unsigned index : {2u, 0u, 1u, 3u, 4u}) {
            const auto eventIndex = index == 2 ? 0u : index < 2 ? index + 1u : index;
            const auto& event = originalEvents[eventIndex].event;
            harken::FrontierGroupBinding binding{};
            binding.role = index == 2 ? harken::FrontierGroupRole::Coordinator : harken::FrontierGroupRole::EntryMember;
            binding.entry = scope.entry;
            binding.primary = identities[2];
            binding.operation.kind = harken::FrontierOperationKind::Entry;
            binding.operation.owner = scope.entry.owner;
            binding.operation.group = identities[index];
            binding.operation.world = 7;
            binding.operation.transport = kEpoch;
            binding.operation.order = {anchor, 9, 1, eventIndex, eventIndex, 10, event.rowSequence};
            if (index != 2) {
                binding.operation.program = controls[index];
                binding.operation.programGeneration = 99;
                binding.variableSide = {index < 2 ? 0u : 1u, 1,
                    static_cast<std::uint8_t>(index < 2 ? index : index - 3), 1, 0, 1};
            }
            ASSERT_TRUE(pool.BindPlan03GroupFrontier(identities[index], binding));
        }
        for (const unsigned index : {5u, 6u, 7u}) {
            const auto eventIndex = index + 2u;
            const auto& event = originalEvents[eventIndex].event;
            harken::FrontierGroupBinding binding{};
            binding.role = index == 5 ? harken::FrontierGroupRole::Coordinator :
                index == 6 ? harken::FrontierGroupRole::EntryMember : harken::FrontierGroupRole::Future;
            binding.entry = childEntry;
            binding.primary = identities[5];
            binding.operation.kind = index == 5 ? harken::FrontierOperationKind::Entry : harken::FrontierOperationKind::Dependent;
            binding.operation.owner = scope.entry.owner;
            binding.operation.group = identities[index];
            binding.operation.world = 7;
            binding.operation.transport = kEpoch;
            binding.operation.order = {anchor + originalEvents[eventIndex].relativeFrame, 9, event.executionOrdinal,
                eventIndex, eventIndex, event.subeventPhase, event.rowSequence};
            if (index != 5) binding.queueInitial = {0, 1, 1, 77};
            ASSERT_TRUE(pool.BindPlan03GroupFrontier(identities[index], binding));
        }
        harken::FrontierActivationDescriptor descriptor{};
        descriptor.entry = scope.entry;
        descriptor.primary = identities[2];
        descriptor.order = {anchor, 9, 1, 0, 0, 10, 1};
        descriptor.worldGeneration = 7;
        descriptor.worldSemanticFingerprint = 77;
        descriptor.expectedMemberCount = 4;
        descriptor.expectedEntryEventCount = 7;
        descriptor.programGeneration = 99;
        ASSERT_TRUE(decisions.PrepareFrontierActivation(tokens[2], descriptor, pool));
        harken::ArmGroupToken coordinator{};
        ASSERT_TRUE(armGroups.Reserve(tokens[2], nullptr, 0, &coordinator));
        std::array<harken::DecisionParticipantRef, 4> participants{};
        constexpr std::array<unsigned, 4> participantGroups{0, 1, 3, 4};
        for (unsigned index = 0; index < participants.size(); ++index) {
            const auto token = tokens[participantGroups[index]];
            participants[index] = {harken::DecisionParticipantKind::Subdecision, token.slot, token.generation};
        }
        harken::DecisionCommitPayload commit{};
        commit.participants = participants.data();
        commit.participantCount = participants.size();
        commit.pages = {static_cast<std::uint32_t>(identities[2].committedRow), identities[2].rowClaimGeneration};
        commit.armGroup = coordinator;
        ASSERT_TRUE(decisions.Commit(tokens[2], commit));
        auto childDescriptor = descriptor;
        childDescriptor.entry = childEntry;
        childDescriptor.primary = identities[5];
        const auto& childControl = originalEvents[7].event;
        childDescriptor.order = {anchor + 100, 9, 1, 7, 7, 10, childControl.rowSequence};
        childDescriptor.expectedMemberCount = 1;
        childDescriptor.expectedEntryEventCount = 2;
        ASSERT_TRUE(decisions.PrepareFrontierActivation(tokens[5], childDescriptor, pool));
        harken::ArmGroupToken childCoordinator{};
        ASSERT_TRUE(armGroups.Reserve(tokens[5], nullptr, 0, &childCoordinator));
        const harken::DecisionParticipantRef childParticipant{harken::DecisionParticipantKind::Subdecision,
            tokens[6].slot, tokens[6].generation};
        commit.participants = &childParticipant;
        commit.participantCount = 1;
        commit.pages = {static_cast<std::uint32_t>(identities[5].committedRow), identities[5].rowClaimGeneration};
        commit.armGroup = childCoordinator;
        ASSERT_TRUE(decisions.Commit(tokens[5], commit));
        const bool accepted = mode == 0 || mode == 3;
        EXPECT_EQ(accepted, lane.PinGroups(handle, pool, decisions, 8));
        if (!accepted) EXPECT_FALSE(lane.Publish(handle));
        ASSERT_TRUE(lane.Cancel(handle));
        ASSERT_TRUE(lane.Retire(handle, 0));
        ASSERT_TRUE(decisions.RollbackPreparedFrontierDecision(tokens[2], tokens[2]));
        ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[2]));
        ASSERT_TRUE(decisions.RollbackPreparedFrontierDecision(tokens[5], tokens[5]));
        ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[5]));
        for (const unsigned index : {0u, 1u, 3u, 4u, 2u, 6u, 7u, 5u})
            ASSERT_TRUE(pool.DetachPlan03GroupFrontier(identities[index], 0));
        ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0, originals.data(), originals.size()));
        for (const auto arm : actualArms) ASSERT_TRUE(arms.CancelPrepared(arm));
        ASSERT_TRUE(armGroups.CancelBeforePublication(coordinator));
        ASSERT_TRUE(armGroups.CancelBeforePublication(childCoordinator));
        for (unsigned index = 0; index < 7; ++index) {
            const auto token = tokens[index];
            ASSERT_TRUE(decisions.Cancel(token));
            ASSERT_TRUE(decisions.ReclaimCancelled(token));
        }
        EXPECT_EQ(0u, arms.LiveCount());
        EXPECT_EQ(0u, decisions.LiveCount());
        EXPECT_EQ(0u, armGroups.LiveCount());
        EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
        EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    }
}

TEST(ScheduledTransactionPool, PrivateLaneEntryMappingsUseRealInterleavedFutureEventsAndRefusalRetryPins) {
    for (unsigned run = 0; run < 50; ++run) {
        const auto mode = run < 46 ? run % 23 : 0;
        const bool accepted = run >= 23;
        const auto badEvent = run < 46 ? 0u : run - 45;
        SCOPED_TRACE(run);
        Harness owners;
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = 6;
        config.maxArmsPerGroup = 1;
        config.maxImmediateLaneGroupRows = 6;
        config.maxImmediateLaneOwnerGates = 6;
        config.maxImmediateLaneDecisionGates = 6;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms{4}, foreignArms{4};
        harken::DecisionTable decisions{6, 2};
        harken::ArmGroupTable coordinatorGroups{2, 1};
        std::array<harken::ArmGroupToken, 2> coordinatorTokens{};
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 10, 10, config.quantumFrames, true, 6}));
        const harken::ImmediateLaneDemand demand{10, 6, 4, 4};
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(17, demand, &handle));
        std::array<harken::ImmediateLanePrivateVariableControlEntryScope, 2> scopes{};
        std::array<std::array<harken::PreparedPrivateImmediateLaneGroup, 3>, 2> originals{};
        std::array<std::array<harken::PrivateImmediateLaneGroupTransferProof, 3>, 2> proofs{};
        std::array<harken::OriginalQueueFutureGroupRegistration, 6> acceptedOriginals{};
        std::array<harken::ImmediateLaneAcceptedPrivateGroup, 6> acceptedGroups{};
        std::array<std::array<harken::Plan03GroupSliceIdentity, 3>, 2> groups{};
        std::array<std::array<harken::DecisionToken, 3>, 2> tokens{};
        std::array<harken::TimelineArmToken, 4> actualArms{}, foreignTokens{};
        std::array<harken::ImmediateLaneMemberManifest, 4> members{};
        std::array<harken::TimelineEventPayload, 4> payloads{};
        std::array<harken::TimelineProgramView, 4> programs{};
        std::array<harken::ImmediateLaneEvent, 10> events{};
        std::array<harken::FrontierActivationDescriptor, 2> descriptors{};
        std::array<std::uint32_t, 2> ordinals{UINT32_MAX, UINT32_MAX};
        for (unsigned entry = 0; entry < 2; ++entry) {
            auto& scope = scopes[entry];
            scope.provisionalFrame = 100;
            scope.firstMember = entry * 2;
            scope.memberCount = 2;
            scope.members = members.data() + scope.firstMember;
            scope.coordinatorRequest = {{9}, 7, kEpoch, entry + 1u, 1, {}};
            for (unsigned side = 0; side < 3; ++side) {
                ASSERT_TRUE(decisions.Reserve(scope.coordinatorRequest, &tokens[entry][side]));
                auto& input = scope.groups[side];
                input.executeFrame = 100;
                input.decision = tokens[entry][side];
                if (side < 2) {
                    const auto member = entry * 2 + side;
                    auto& manifest = members[member];
                    manifest.relativeFrame = mode == 1 || mode == 2 || mode == 5 || mode == 13 || mode >= 19 ?
                        (entry == 0 ? (side == 0 ? 0u : 200u) : (side == 0 ? 100u : 300u))
                        : 0u;
                    manifest.future = manifest.relativeFrame != 0;
                    if (mode == 22 && manifest.future) {
                        manifest.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
                        manifest.fixedFrame = 200 + manifest.relativeFrame;
                        manifest.relativeFrame = 0;
                    }
                    manifest.programGeneration = 40 + member;
                    manifest.ownerInstance = owners.owner;
                    manifest.ownerEpoch = owners.ownerEpoch;
                    manifest.target.programSemanticFingerprint = 50 + member;
                    ASSERT_TRUE(arms.PreparePendingImmediateGrouped(member + 1, 3,
                        manifest.programGeneration, manifest.target,
                        harken::ParentAudioGroupRef{input.decision}, 0, &actualArms[member]));
                    ASSERT_TRUE(foreignArms.PreparePendingImmediateGrouped(member + 1, 3,
                        manifest.programGeneration, manifest.target,
                        harken::ParentAudioGroupRef{input.decision}, 0, &foreignTokens[member]));
                    EXPECT_EQ(actualArms[member].slot, foreignTokens[member].slot);
                    EXPECT_EQ(actualArms[member].generation, foreignTokens[member].generation);
                    input.members = &actualArms[member];
                    input.memberCount = 1;
                    manifest.arm = actualArms[member];
                    manifest.memberIndex = 0;
                    payloads[member].kind = harken::TimelineEventKind::Start;
                    payloads[member].target = manifest.target;
                    payloads[member].reserved = manifest.future ? harken::kTimelineEventTransientStartArm : 0u;
                    programs[member] = {manifest.programGeneration, &payloads[member], 1};
                }
                ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms,
                    &groups[entry][side], &originals[entry][side]));
                ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(originals[entry][side],
                    17, 100, input, arms, &proofs[entry][side]));
                if (side < 2) {
                    auto& manifest = members[entry * 2 + side];
                    manifest.group = groups[entry][side];
                    ASSERT_TRUE(lane.DeclareMember(handle, manifest));
                }
            }
            scope.entry = {tokens[entry][2], entry + 1u,
                {owners.owner, owners.ownerEpoch, 0, 0, owners.owner, owners.ownerEpoch, entry, entry + 1u}};
        }
        for (unsigned entry = 0; entry < 2; ++entry) {
            if (!accepted)
                ASSERT_TRUE(lane.DeclarePrivateVariableControlEntry(handle, 17, demand, scopes[entry],
                    pool, arms, decisions, originals[entry], proofs[entry], &ordinals[entry]));
        }
        for (unsigned entry = 0; entry < 2; ++entry) {
            for (unsigned side = 0; side < 3; ++side) {
                if (accepted) {
                    const auto index = entry * 3 + side;
                    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(
                        std::move(proofs[entry][side]), &acceptedOriginals[index]));
                    EXPECT_EQ(groups[entry][side], acceptedOriginals[index].Identity());
                    auto& group = acceptedGroups[index];
                    group.original = &acceptedOriginals[index];
                    group.schedule = {groups[entry][side], 0};
                    ASSERT_TRUE(group.original->CaptureOriginalScope(pool, arms, &group.scope));
                } else {
                    EXPECT_EQ(groups[entry][side], ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(
                        std::move(proofs[entry][side])));
                }
                ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(groups[entry][side], lane,
                    17, handle.slot, handle.generation));
                ASSERT_TRUE(lane.DeclareGroup(handle, {groups[entry][side], 0}));
            }
            for (unsigned local = 0; local < 5; ++local) {
                auto& laneEvent = events[entry * 5 + local];
                const auto side = local == 0 ? 2u : (local < 3 ? local - 1u : local - 3u);
                auto& event = laneEvent.event;
                if (local < 3) {
                    const auto* control = decisions.PrepareFrontierControlProgram(tokens[entry][side], 99);
                    ASSERT_NE(nullptr, control);
                    event.program = {control->generation, control};
                } else {
                    const auto member = entry * 2 + side;
                    laneEvent.relativeFrame = members[member].relativeFrame;
                    laneEvent.timeDomain = members[member].timeDomain;
                    laneEvent.fixedFrame = members[member].fixedFrame;
                    laneEvent.futureGroupedStart = members[member].future;
                    event.program = {programs[member].generation, &programs[member]};
                    event.armToken = actualArms[member];
                }
                event.logicCommitSequence = 9;
                event.executionOrdinal = entry + 1;
                event.eventIndex = local;
                event.transactionOrdinal = local;
                event.subeventPhase = local < 3 ? 10u : 20u;
                event.rowSequence = entry * 5 + local + 1;
                event.transactionId = 9;
                event.ownerInstanceHandle = owners.owner;
                event.ownerInstanceScheduleEpoch = owners.ownerEpoch;
                event.frozenTxnValid = true;
                event.frontier.kind = laneEvent.futureGroupedStart ? harken::FrontierOperationKind::Dependent :
                    harken::FrontierOperationKind::Entry;
                event.frontier.entry = scopes[entry].entry;
                event.frontier.owner = scopes[entry].entry.owner;
                event.frontier.group = groups[entry][side];
                event.frontier.transport = kEpoch;
                event.frontier.world = 7;
                event.frontier.operationEventCount = 1;
                event.frontier.entryEventOrdinal = local;
            }
        }
        if (accepted) {
            for (unsigned entry = 0; entry < 2; ++entry) {
                harken::ImmediateLaneAcceptedPrivateEntryScope scope{};
                scope.entry = scopes[entry].entry;
                scope.coordinatorRequest = scopes[entry].coordinatorRequest;
                scope.provisionalFrame = scopes[entry].provisionalFrame;
                scope.firstMember = scopes[entry].firstMember;
                scope.memberCount = scopes[entry].memberCount;
                scope.members = scopes[entry].members;
                scope.outerGroups = groups[entry];
                scope.groups = acceptedGroups.data() + entry * 3;
                scope.groupCount = 3;
                auto wrong = scope;
                ++wrong.coordinatorRequest.ticket.sequence;
                EXPECT_FALSE(lane.DeclareAcceptedPrivateVariableControlEntry(handle, 17, demand, wrong,
                    pool, arms, decisions, &ordinals[entry]));
                ASSERT_TRUE(lane.DeclareAcceptedPrivateVariableControlEntry(handle, 17, demand, scope,
                    pool, arms, decisions, &ordinals[entry]));
                EXPECT_FALSE(lane.DeclareAcceptedPrivateVariableControlEntry(handle, 17, demand, scope,
                    pool, arms, decisions, &ordinals[entry]));
            }
        }
        if (mode >= 4) {
            auto wrong = scopes[0].entry;
            ++wrong.owner.serviceGeneration;
            EXPECT_FALSE(lane.RequestPrivateEntryStop(handle, 17, 0, wrong));
            EXPECT_FALSE(lane.RequestPrivateEntryStop(handle, 18, 0, scopes[0].entry));
            EXPECT_FALSE(lane.RequestPrivateEntryStop(handle, 17, 2, scopes[0].entry));
            if (mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 10 || mode == 11 || mode == 13 || (mode >= 14 && mode <= 18))
                ASSERT_TRUE(lane.RequestPrivateEntryStop(handle, 17, 0, scopes[0].entry));
            if (mode == 4 || mode == 5 || mode == 11 || mode == 13 || (mode >= 14 && mode <= 16))
                ASSERT_TRUE(lane.RequestPrivateEntryStop(handle, 17, 1, scopes[1].entry));
        }
        if (badEvent == 1) events[1].event.frontier.entryEventOrdinal = 0;
        if (badEvent == 2) events[1].event.frontier.entryEventOrdinal = 10;
        if (badEvent == 3) events[1].event.frontier.operationEventCount = 2;
        if (badEvent == 4) events[1].event.frontier.operationOrdinal = 1;
        ASSERT_TRUE(lane.Prepare(handle, events.data(), 10));
        std::uint64_t anchor = 0;
        harken::PrivateImmediateLanePlacementProof placement;
        ASSERT_TRUE(lane.TryPlace(handle, 200, 0, &anchor, &placement));
        std::array<std::uint32_t, 4> actualStartOrdinals{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
        for (std::uint32_t index = 0; index < events.size(); ++index) {
            harken::ImmediateLaneEvent placed{};
            ASSERT_TRUE(lane.CapturePrivatePlacedEvent(placement, index, &placed));
            for (std::uint32_t member = 0; member < actualArms.size(); ++member)
                if (placed.event.armToken.slot == actualArms[member].slot &&
                    placed.event.armToken.generation == actualArms[member].generation &&
                    placed.event.armToken.renderSlot == actualArms[member].renderSlot &&
                    placed.event.armToken.renderGeneration == actualArms[member].renderGeneration)
                    actualStartOrdinals[member] = index;
        }
        for (const auto index : actualStartOrdinals) ASSERT_NE(UINT32_MAX, index);
        placement.Release();
        const auto bind = [&](bool wrongLastDescriptor) {
            ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, anchor));
            for (unsigned entry = 0; entry < 2; ++entry) {
                const auto makeBinding = [&](unsigned side) {
                    const auto local = side == 2 ? 0u : side + 1u;
                    const auto& event = events[entry * 5 + local].event;
                    harken::FrontierGroupBinding binding{};
                    binding.role = side == 2 ? harken::FrontierGroupRole::Coordinator : harken::FrontierGroupRole::EntryMember;
                    binding.entry = scopes[entry].entry;
                    binding.primary = groups[entry][2];
                    binding.operation.kind = harken::FrontierOperationKind::Entry;
                    binding.operation.owner = binding.entry.owner;
                    binding.operation.group = groups[entry][side];
                    binding.operation.world = 7;
                    binding.operation.transport = kEpoch;
                    binding.operation.order = {anchor, 9, entry + 1u, local, local, 10, event.rowSequence};
                    if (side < 2) {
                        binding.operation.program = event.program.program;
                        binding.operation.programGeneration = 99;
                        binding.variableSide = {entry, entry + 1u, static_cast<std::uint8_t>(side), 1, 0, 1};
                    }
                    return binding;
                };
                const auto primary = makeBinding(2);
                ASSERT_TRUE(pool.BindPlan03GroupFrontier(groups[entry][2], primary));
                for (unsigned side = 0; side < 2; ++side)
                    ASSERT_TRUE(pool.BindPlan03GroupFrontier(groups[entry][side], makeBinding(side)));
                auto& descriptor = descriptors[entry];
                descriptor.entry = scopes[entry].entry;
                descriptor.primary = groups[entry][2];
                descriptor.order = primary.operation.order;
                descriptor.worldGeneration = 7;
                descriptor.worldSemanticFingerprint = 91;
                descriptor.expectedMemberCount = 2;
                descriptor.expectedEntryEventCount = 0;
                for (unsigned local = 0; local < 5; ++local)
                    if (!events[entry * 5 + local].futureGroupedStart) ++descriptor.expectedEntryEventCount;
                if (wrongLastDescriptor && entry == 1) ++descriptor.expectedEntryEventCount;
                descriptor.programGeneration = 99;
                ASSERT_TRUE(decisions.PrepareFrontierActivation(tokens[entry][2], descriptor, pool));
            }
        };
        ASSERT_NO_FATAL_FAILURE(bind(mode == 2 || mode == 13));
        for (unsigned entry = 0; entry < 2; ++entry) {
            ASSERT_TRUE(coordinatorGroups.Reserve(tokens[entry][2], nullptr, 0, &coordinatorTokens[entry]));
            std::array<harken::DecisionParticipantRef, 2> participants{};
            for (unsigned side = 0; side < 2; ++side)
                participants[side] = {harken::DecisionParticipantKind::Subdecision,
                    tokens[entry][side].slot, tokens[entry][side].generation};
            harken::DecisionCommitPayload commit{};
            commit.participants = participants.data();
            commit.participantCount = 2;
            commit.pages = {static_cast<std::uint32_t>(groups[entry][2].committedRow), groups[entry][2].rowClaimGeneration};
            commit.armGroup = coordinatorTokens[entry];
            ASSERT_TRUE(decisions.Commit(tokens[entry][2], commit));
        }
        harken::ImmediateLanePrivateVariableControlEntryMapping refused{};
        refused.coordinatorBucket = 4242;
        const auto sentinel = refused;
        if (badEvent != 0) {
            EXPECT_FALSE(lane.PinGroups(handle, pool, decisions, 6));
            EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, 0,
                scopes[0].entry, pool, arms, decisions, &refused));
            EXPECT_EQ(sentinel, refused);
            EXPECT_FALSE(lane.Publish(handle));
            ASSERT_TRUE(lane.Cancel(handle));
            ASSERT_TRUE(lane.Retire(handle, 0));
            for (unsigned entry = 0; entry < 2; ++entry) {
                ASSERT_TRUE(decisions.RollbackPreparedFrontierDecision(tokens[entry][2], tokens[entry][2]));
                ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[entry][2]));
                for (const auto& group : groups[entry]) ASSERT_TRUE(pool.DetachPlan03GroupFrontier(group, 0));
            }
            ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0, acceptedOriginals.data(), acceptedOriginals.size()));
            for (const auto arm : actualArms) ASSERT_TRUE(arms.CancelPrepared(arm));
            for (const auto arm : foreignTokens) ASSERT_TRUE(foreignArms.CancelPrepared(arm));
            for (const auto group : coordinatorTokens) ASSERT_TRUE(coordinatorGroups.CancelBeforePublication(group));
            for (const auto& entry : tokens) for (const auto token : entry) {
                ASSERT_TRUE(decisions.Cancel(token));
                ASSERT_TRUE(decisions.ReclaimCancelled(token));
            }
            EXPECT_EQ(0u, arms.LiveCount());
            EXPECT_EQ(0u, foreignArms.LiveCount());
            EXPECT_EQ(0u, decisions.LiveCount());
            EXPECT_EQ(0u, coordinatorGroups.LiveCount());
            EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
            EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
            continue;
        }
        if (mode == 2 || mode == 13) {
            EXPECT_FALSE(lane.PinGroups(handle, pool, decisions, 6));
            EXPECT_EQ(6u, lane.PrivatePinGroupCount(handle));
            for (unsigned entry = 0; entry < 2; ++entry) {
                harken::DecisionCommittedSnapshot actual{};
                ASSERT_TRUE(decisions.CaptureCommitted(tokens[entry][2], &actual));
                EXPECT_EQ(1u, actual.futurePinCount);
                EXPECT_EQ(0u, actual.lastAudioSequence);
                for (unsigned side = 0; side < 2; ++side)
                    EXPECT_TRUE(decisions.MatchesOpenReservation(tokens[entry][side], scopes[entry].coordinatorRequest));
                EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, ordinals[entry],
                    scopes[entry].entry, pool, arms, decisions, &refused));
                ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[entry][2]));
                for (unsigned side = 0; side < 2; ++side) ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[entry][side], 0));
                ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[entry][2], 0));
            }
            ASSERT_TRUE(lane.TryPlace(handle, 300, 0, &anchor));
            EXPECT_EQ(300u, anchor);
            ASSERT_NO_FATAL_FAILURE(bind(false));
        }
        ASSERT_TRUE(lane.PinGroups(handle, pool, decisions, 6));
        EXPECT_FALSE(lane.PinArms(handle, foreignArms));
        EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, 0,
            scopes[0].entry, pool, arms, decisions, &refused));
        EXPECT_FALSE(lane.Publish(handle));
        if (mode == 3) {
            ASSERT_TRUE(arms.CancelPrepared(actualArms[3]));
            EXPECT_FALSE(lane.PinArms(handle, arms));
            EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, 0,
                scopes[0].entry, pool, arms, decisions, &refused));
            EXPECT_FALSE(lane.Publish(handle));
            harken::TimelineArmToken replacement{};
            ASSERT_TRUE(arms.PreparePendingImmediateGrouped(actualArms[3].renderSlot,
                actualArms[3].renderGeneration, members[3].programGeneration, members[3].target,
                harken::ParentAudioGroupRef{tokens[1][1]}, 0, &replacement));
            EXPECT_EQ(actualArms[3].slot, replacement.slot);
            EXPECT_NE(actualArms[3].generation, replacement.generation);
            EXPECT_FALSE(lane.PinArms(handle, arms));
            EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, 1,
                scopes[1].entry, pool, arms, decisions, &refused));
            EXPECT_EQ(sentinel, refused);
            ASSERT_TRUE(lane.Cancel(handle));
            ASSERT_TRUE(lane.Retire(handle, 0));
            harken::ImmediateActionLane freshLane;
            ASSERT_TRUE(freshLane.Reset({1, 1, 1, config.quantumFrames, true}));
            harken::ImmediateLaneHandle freshHandle{};
            ASSERT_TRUE(freshLane.Reserve(18, {1, 0, 0, 1}, &freshHandle));
            harken::ImmediateLaneEvent freshEvent{};
            freshEvent.event.program = {programs[3].generation, &programs[3]};
            freshEvent.event.armToken = replacement;
            freshEvent.event.subeventPhase = 20;
            ASSERT_TRUE(freshLane.Prepare(freshHandle, &freshEvent, 1));
            ASSERT_TRUE(freshLane.TryPlace(freshHandle, 300, 0, &anchor));
            ASSERT_TRUE(freshLane.PinArms(freshHandle, arms));
            EXPECT_FALSE(freshLane.CapturePrivateVariableControlEntryMapping(freshHandle, 17, 1,
                scopes[1].entry, pool, arms, decisions, &refused));
            ASSERT_TRUE(freshLane.Cancel(freshHandle));
            ASSERT_TRUE(freshLane.Retire(freshHandle, 0));
            actualArms[3] = replacement;
            for (unsigned entry = 0; entry < 2; ++entry) {
                ASSERT_TRUE(decisions.RollbackPreparedFrontierDecision(tokens[entry][2], tokens[entry][2]));
                ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[entry][2]));
                for (unsigned side = 0; side < 3; ++side)
                    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[entry][side], 0));
            }
            ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0,
                accepted ? acceptedOriginals.data() : nullptr, accepted ? acceptedOriginals.size() : 0));
            for (const auto arm : actualArms) ASSERT_TRUE(arms.CancelPrepared(arm));
            for (const auto arm : foreignTokens) ASSERT_TRUE(foreignArms.CancelPrepared(arm));
            for (const auto group : coordinatorTokens) ASSERT_TRUE(coordinatorGroups.CancelBeforePublication(group));
            for (const auto& entry : tokens) for (const auto token : entry) {
                ASSERT_TRUE(decisions.Cancel(token));
                ASSERT_TRUE(decisions.ReclaimCancelled(token));
            }
            EXPECT_EQ(0u, arms.LiveCount());
            EXPECT_EQ(0u, decisions.LiveCount());
            continue;
        }
        EXPECT_EQ(sentinel, refused);
        ASSERT_TRUE(lane.PinArms(handle, arms));
        if (mode == 0) {
            harken::PrivateImmediateLanePublicationProof publication;
            ASSERT_TRUE(lane.ProvePrivatePublication(handle, 17, &publication));
            std::array<harken::DecisionTable::FrontierPublicationProof, 2> coordinators;
            std::array<harken::DecisionCommittedSnapshot, 2> before{};
            for (unsigned entry = 0; entry < 2; ++entry) {
                ASSERT_TRUE(decisions.CaptureCommitted(tokens[entry][2], &before[entry]));
                ASSERT_EQ(2u, before[entry].participantCount);
                harken::DecisionFuturePin reader;
                ASSERT_TRUE(decisions.AcquireFuturePin(tokens[entry][2], &reader));
                ASSERT_TRUE(decisions.ProveFrontierActivationPublication(tokens[entry][2],
                    scopes[entry].entry, pool, &coordinators[entry]));
                harken::DecisionFuturePin refusedReader;
                EXPECT_FALSE(decisions.AcquireFuturePin(tokens[entry][2], &refusedReader));
                EXPECT_FALSE(decisions.CloneFuturePin(reader, &refusedReader));
                EXPECT_FALSE(decisions.CancelPreparedFrontierActivation(tokens[entry][2]));
                EXPECT_FALSE(decisions.RollbackPreparedFrontierDecision(tokens[entry][2], tokens[entry][2]));
                if (entry == 1) reader = harken::DecisionFuturePin{};
            }
            for (unsigned entry = 0; entry < 2; ++entry) {
                EXPECT_FALSE(lane.RequestPrivateEntryStop(handle, 17, entry, scopes[entry].entry));
                EXPECT_FALSE(lane.CanCancelPrivateEntrySources(handle, 17, entry, scopes[entry].entry));
                harken::DecisionCommittedSnapshot held{};
                ASSERT_TRUE(decisions.CaptureCommitted(tokens[entry][2], &held));
                EXPECT_EQ(before[entry].futurePinCount, held.futurePinCount);
                EXPECT_EQ(before[entry].participantCount, held.participantCount);
                EXPECT_EQ(before[entry].ticket.sequence, held.ticket.sequence);
                EXPECT_EQ(harken::FrontierReceiptState::PublicationHeld,
                    decisions.ResolveFrontierActivation(tokens[entry][2])->State());
            }
            harken::PrivateImmediateLaneGroupsPublicationProof poolPublication;
            ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupsPublication(publication, &poolPublication));
            EXPECT_FALSE(pool.PublishImmediateLaneGroups(17));
            EXPECT_FALSE(pool.RebaseImmediateLaneGroups(17, anchor + 100));
            EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 0));
            EXPECT_FALSE(pool.Reset(config, 0, kEpoch + 1));
            EXPECT_FALSE(pool.ResetForEpoch(0, kEpoch + 1));
            for (unsigned entry = 0; entry < 2; ++entry) {
                EXPECT_FALSE(pool.InvalidateFrontierOwnerOperations(scopes[entry].entry.owner));
                for (unsigned side = 0; side < 3; ++side) {
                    harken::Plan03PageGroupView held{};
                    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(groups[entry][side], &held));
                    ASSERT_NE(nullptr, held.lease);
                    EXPECT_EQ(harken::AudioSliceState::PublicationReserved, held.lease->Capture().state);
                    EXPECT_FALSE(held.lease->TryClaim());
                    EXPECT_FALSE(held.lease->PrepareReplacement());
                    EXPECT_FALSE(held.lease->ResetForReuse());
                    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::PublicationHeld,
                        held.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
                    EXPECT_EQ(0u, held.lease->Capture().pendingReasonBits);
                    EXPECT_FALSE(pool.InvalidateFrontierOwnerOperations(scopes[entry].entry.owner, tokens[entry][side]));
                    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(groups[entry][side], 0));
                }
            }
            poolPublication.Release();
            for (unsigned entry = 0; entry < 2; ++entry) {
                coordinators[entry].Release();
                EXPECT_TRUE(decisions.CanPublishFrontierActivation(tokens[entry][2]));
            }
            publication.Release();
            for (unsigned entry = 0; entry < 2; ++entry) {
                EXPECT_FALSE(lane.CanCancelPrivateEntrySources(handle, 17, entry, scopes[entry].entry));
                ASSERT_TRUE(lane.RequestPrivateEntryStop(handle, 17, entry, scopes[entry].entry));
                EXPECT_TRUE(lane.CanCancelPrivateEntrySources(handle, 17, entry, scopes[entry].entry));
            }
        }
        std::array<harken::ImmediateLanePrivateVariableControlEntryMapping, 2> mappings{};
        for (unsigned entry = 0; entry < 2; ++entry) {
            ASSERT_TRUE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, ordinals[entry],
                scopes[entry].entry, pool, arms, decisions, &mappings[entry]));
            for (unsigned side = 0; side < 3; ++side) {
                harken::Plan03GroupSliceIdentity actual{};
                ASSERT_TRUE(lane.CapturePrivatePinGroup(handle, mappings[entry].groups[side], &actual));
                EXPECT_EQ(groups[entry][side], actual);
            }
            EXPECT_EQ(mode == 1 || mode == 2 || mode == 5 || mode == 13 || mode >= 19 ? (entry == 0 ? 4u : 3u) : 5u,
                decisions.ResolveFrontierActivation(tokens[entry][2])->Descriptor().expectedEntryEventCount);
        }
        if (mode == 1 || mode == 2 || mode == 5 || mode == 13 || mode >= 19) {
            EXPECT_LT(actualStartOrdinals[0], mappings[1].events[0]);
            EXPECT_LT(actualStartOrdinals[2], actualStartOrdinals[1]);
            EXPECT_LT(actualStartOrdinals[1], actualStartOrdinals[3]);
        }
        std::array<harken::ImmediateLanePrivateEntryBucketShape, 2> shapes{};
        for (unsigned entry = 0; entry < 2; ++entry) {
            ASSERT_TRUE(lane.CapturePrivateEntryBucketShape(handle, 17, entry, scopes[entry].entry,
                &shapes[entry]));
            EXPECT_EQ(mappings[entry].coordinatorBucket, shapes[entry].coordinator);
            EXPECT_GT(shapes[entry].count, 0u);
            EXPECT_LE(shapes[entry].count, 3u);
            auto wrong = scopes[entry].entry;
            ++wrong.owner.serviceGeneration;
            auto unchanged = shapes[entry];
            EXPECT_FALSE(lane.CapturePrivateEntryBucketShape(handle, 17, entry, wrong, &unchanged));
            EXPECT_EQ(shapes[entry], unchanged);
        }
        std::uint64_t completedSequence = 0;
        if (mode >= 4) {
            // This core fixture never publishes SideDecision or applies audio.
            // Its real lane capture exits prove admission and rejection only.
            if (mode == 14) ASSERT_TRUE(pool.PublishImmediateLaneGroups(17));
            ASSERT_TRUE(lane.Publish(handle));
            if (mode == 22) {
                for (unsigned entry = 0; entry < 2; ++entry) for (unsigned side = 0; side < 2; ++side) {
                    const auto member = entry * 2 + side;
                    const harken::ImmediateLaneEvent* actual = nullptr;
                    EXPECT_EQ(members[member].future, lane.FutureMember(handle.slot, handle.generation,
                        groups[entry][side], 0, actualArms[member], &actual));
                    if (members[member].future) {
                        ASSERT_NE(nullptr, actual);
                        EXPECT_EQ(members[member].fixedFrame, actual->event.executeFrame);
                        EXPECT_EQ(harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame, actual->timeDomain);
                    }
                }
            }
            const auto firstQuantum = anchor / config.quantumFrames;
            const auto lastQuantum = mode == 5 || mode == 13 || mode >= 19 ? (anchor + 300u) / config.quantumFrames : firstQuantum;
            std::uint32_t capturedOrdinal = 0;
            if (mode >= 19) {
                if (mode == 19) ASSERT_TRUE(lane.Cancel(handle));
                else if (mode == 20) {
                    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, firstQuantum + 1u));
                    completedSequence = firstQuantum + 2u;
                } else {
                    const auto* bucket = lane.CaptureDue(handle.slot, firstQuantum);
                    ASSERT_NE(nullptr, bucket);
                    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
                    completedSequence = firstQuantum + 1u;
                }
            } else if (mode >= 15) {
                harken::EngineConfig voiceConfig;
                harken::VoiceCapacityModel capacity;
                ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 0, &capacity));
                voiceConfig.voiceCapacity = capacity;
                harken::ResampleTables tables;
                tables.Init();
                harken::MultirateResampleTables multirateTables;
                multirateTables.Init();
                harken::BiquadTrigTable biquadTrig;
                harken::EffectPoolBank effects(voiceConfig);
                harken::DynamicRouteSystem routes(voiceConfig);
                harken::VoicePool voices(voiceConfig, &tables, &multirateTables, &biquadTrig,
                    effects, routes, &arms);
                harken::FrontierTimelineFinalizer frontier(voices, arms);
                ASSERT_TRUE(frontier.Attach(decisions, pool, true));
                frontier.AttachArmGroups(coordinatorGroups);
                harken::ImmediateLaneTimelineFinalizer finalizer(voices, arms);
                ASSERT_TRUE(finalizer.Attach(lane, config.quantumFrames));
                finalizer.AttachFrontier(frontier, pool);
                ASSERT_TRUE(finalizer.Capture(firstQuantum));
                if (mode == 16 || mode == 17) finalizer.Discard(true);
                else {
                    // Mode 18 exercises the real RT refusal of still-private groups.
                    finalizer.Apply(firstQuantum);
                    finalizer.Finish();
                }
                completedSequence = firstQuantum + 1u;
                EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, firstQuantum));
                for (unsigned entry = 0; entry < 2; ++entry) {
                    harken::ImmediateLaneEntryOutcomeSnapshot outcome{};
                    ASSERT_TRUE(lane.CapturePrivateEntryOutcome(handle, 17, entry, scopes[entry].entry,
                        mappings[entry].coordinatorBucket, completedSequence, &outcome));
                    EXPECT_EQ(entry == 0 || mode <= 16 ? harken::ImmediateLaneEntryOutcome::Withdrawn :
                        harken::ImmediateLaneEntryOutcome::NotApplied, outcome.outcome);
                    EXPECT_EQ(completedSequence, outcome.audioSequence);
                }
                harken::ImmediateLaneObservation observed{};
                ASSERT_TRUE(lane.Observe(handle, &observed));
                EXPECT_FALSE(observed.applied);
                EXPECT_EQ(mode <= 16, observed.cancelledOnly);
                EXPECT_EQ(mode == 17, observed.arenaCapacityMissed);
            } else for (auto quantum = firstQuantum; quantum <= lastQuantum; ++quantum) {
                const auto* bucket = lane.CaptureDue(handle.slot, quantum);
                ASSERT_NE(nullptr, bucket);
                ASSERT_TRUE(lane.CapturedEntryProjectionValid(handle.slot, bucket));
                const bool allWithdrawing = mode == 4 || mode == 5 || mode == 11 || mode == 13 || mode == 14;
                for (unsigned entry = 0; entry < 2; ++entry) {
                    const bool withdrew = allWithdrawing ||
                        (entry == 0 && (mode == 6 || mode == 7 || mode == 10));
                    for (const auto event : mappings[entry].events)
                        if (event >= bucket->firstEvent && event - bucket->firstEvent < bucket->eventCount)
                            EXPECT_EQ(!withdrew, lane.IsEventAdmitted(handle.slot, bucket, event));
                    for (const auto group : mappings[entry].groups)
                        if (group >= bucket->firstGroup && group - bucket->firstGroup < bucket->groupCount)
                            EXPECT_EQ(!withdrew, lane.IsGroupAdmitted(handle.slot, bucket, group));
                }
                harken::ImmediateLaneEntryOutcomeSnapshot outcome{};
                outcome.audioSequence = 4242;
                const auto outcomeSentinel = outcome;
                EXPECT_FALSE(lane.CapturePrivateEntryOutcome(handle, 17, 0, scopes[0].entry,
                    capturedOrdinal, quantum + 1u, &outcome));
                EXPECT_EQ(outcomeSentinel, outcome);
                if (mode == 6 || mode == 8) {
                    ASSERT_TRUE(lane.RequestPrivateEntryStop(handle, 17, 1, scopes[1].entry));
                    EXPECT_TRUE(lane.IsGroupAdmitted(handle.slot, bucket, mappings[1].groups[2]));
                    if (mode == 8) ASSERT_TRUE(lane.RequestPrivateEntryStop(handle, 17, 0, scopes[0].entry));
                }
                if (mode == 4 || mode == 5 || mode == 13 || mode == 14) {
                    ASSERT_TRUE(lane.ClaimCancelledOnly(handle, bucket));
                    EXPECT_FALSE(lane.Retire(handle, quantum + 1u));
                    ASSERT_TRUE(lane.FinishCancelledOnly(handle.slot, bucket));
                } else if (mode == 7 || mode == 10 || mode == 11) {
                    if (mode == 10 || mode == 11)
                        ASSERT_TRUE(lane.RecordAdmissionFailure(handle.slot, bucket,
                            harken::ImmediateLaneAdmissionFailure::ArenaCapacity));
                    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
                } else if (mode == 9) {
                    ASSERT_TRUE(lane.Cancel(handle));
                    EXPECT_FALSE(lane.Claim(handle, bucket));
                } else {
                    harken::ImmediateLaneBucketClaimReceipt receipt{};
                    EXPECT_FALSE(lane.ClaimCancelledOnly(handle, bucket));
                    ASSERT_TRUE(lane.Claim(handle, bucket, &receipt));
                    for (unsigned entry = 0; entry < 2; ++entry) for (unsigned side = 0; side < 3; ++side)
                        EXPECT_EQ(!(mode == 6 && entry == 0), lane.ValidateGroupClaimReceipt(receipt,
                            groups[entry][side], mappings[entry].coordinatorBucket, mappings[entry].groups[side]));
                    if (mode == 12) EXPECT_FALSE(lane.Cancel(handle));
                    ASSERT_TRUE(lane.Finish(handle.slot, bucket, false));
                }
                completedSequence = quantum + 1u;
                EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, quantum));
                EXPECT_FALSE(lane.Finish(handle.slot, bucket, false));
                for (unsigned entry = 0; entry < 2; ++entry) {
                    // Future-only buckets can contain one of the two entries.
                    std::uint32_t actualBucket = UINT32_MAX;
                    for (const auto event : mappings[entry].events) {
                        if (event >= bucket->firstEvent && event - bucket->firstEvent < bucket->eventCount) {
                            actualBucket = capturedOrdinal;
                            break;
                        }
                    }
                    if (actualBucket == UINT32_MAX) continue;
                    EXPECT_FALSE(lane.CapturePrivateEntryOutcome(handle, 17, entry, scopes[entry].entry,
                        actualBucket, quantum, &outcome));
                    EXPECT_EQ(outcomeSentinel, outcome);
                    ASSERT_TRUE(lane.CapturePrivateEntryOutcome(handle, 17, entry, scopes[entry].entry,
                        actualBucket, completedSequence, &outcome));
                    const bool withdrew = allWithdrawing ||
                        (entry == 0 && (mode == 6 || mode == 7 || mode == 10));
                    EXPECT_EQ(withdrew ? harken::ImmediateLaneEntryOutcome::Withdrawn :
                        harken::ImmediateLaneEntryOutcome::NotApplied, outcome.outcome);
                    EXPECT_EQ(completedSequence, outcome.audioSequence);
                    EXPECT_FALSE(lane.EntryApplied(handle.slot, handle.generation, scopes[entry].entry));
                    outcome = outcomeSentinel;
                }
                harken::ImmediateLaneObservation observed{};
                ASSERT_TRUE(lane.Observe(handle, &observed));
                EXPECT_FALSE(observed.applied);
                EXPECT_EQ(allWithdrawing, observed.cancelledOnly);
                if (mode == 10) {
                    EXPECT_TRUE(observed.arenaCapacityMissed);
                    EXPECT_FALSE(observed.protocolRefused);
                }
                if (quantum < lastQuantum) EXPECT_FALSE(lane.Retire(handle, completedSequence));
                ++capturedOrdinal;
            }
        }
        if (mode >= 4) {
            for (unsigned entry = 0; entry < 2; ++entry) {
                ASSERT_TRUE(lane.MatchesPrivateEntryAssociation(handle, 17, entry, scopes[entry].entry));
                harken::ImmediateLanePrivateEntryTerminalProjection projection{};
                projection.registeredEntryCount = 4242;
                const auto sentinelProjection = projection;
                ASSERT_TRUE(lane.CapturePrivateEntryTerminalProjection(handle, 17, entry,
                    scopes[entry].entry, completedSequence, &projection));
                std::uint64_t actualLastSequence = 0;
                for (unsigned index = 0; index < projection.shape.count; ++index)
                    actualLastSequence = std::max(actualLastSequence, projection.outcomes[index].audioSequence);
                if (actualLastSequence != 0) {
                    auto notReady = sentinelProjection;
                    EXPECT_FALSE(lane.CapturePrivateEntryTerminalProjection(handle, 17, entry,
                        scopes[entry].entry, actualLastSequence - 1u, &notReady));
                    EXPECT_EQ(sentinelProjection, notReady);
                }
                EXPECT_EQ(shapes[entry], projection.shape);
                EXPECT_EQ(2u, projection.registeredEntryCount);
                if (mode == 19 || mode == 20)
                    EXPECT_EQ(projection.shape.count, projection.omitted.count());
                else if (mode == 21 || mode == 22) EXPECT_TRUE(projection.omitted.any());
                else EXPECT_TRUE(projection.omitted.none());
                for (unsigned index = 0; index < projection.shape.count; ++index) {
                    EXPECT_EQ(projection.shape.ordinals[index], projection.outcomes[index].bucketOrdinal);
                    if (projection.omitted[index]) {
                        EXPECT_EQ(harken::ImmediateLaneEntryOutcome::Dormant, projection.outcomes[index].outcome);
                        EXPECT_EQ(0u, projection.outcomes[index].audioSequence);
                    }
                }
                auto wrong = scopes[entry].entry;
                ++wrong.owner.serviceGeneration;
                projection = sentinelProjection;
                EXPECT_FALSE(lane.CapturePrivateEntryTerminalProjection(handle, 17, entry, wrong,
                    completedSequence, &projection));
                EXPECT_EQ(sentinelProjection, projection);
            }
        }
        if (mode == 14) {
            // Published, nonterminal leases genuinely refuse lane reader release.
            EXPECT_FALSE(lane.Retire(handle, completedSequence));
            for (unsigned entry = 0; entry < 2; ++entry) {
                harken::ImmediateLaneEntryOutcomeSnapshot outcome{};
                ASSERT_TRUE(lane.CapturePrivateEntryOutcome(handle, 17, entry, scopes[entry].entry,
                    mappings[entry].coordinatorBucket, completedSequence, &outcome));
                EXPECT_EQ(harken::ImmediateLaneEntryOutcome::Withdrawn, outcome.outcome);
                ASSERT_TRUE(decisions.RollbackPreparedFrontierDecision(tokens[entry][2], tokens[entry][2]));
                harken::Plan03PageGroupView view{};
                ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(groups[entry][2], &view));
                ASSERT_NE(nullptr, view.lease);
                EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
                    view.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
                ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[entry][2]));
                for (const auto& group : groups[entry])
                    ASSERT_TRUE(pool.NotePlan03GroupReadRt(group, completedSequence));
            }
            // A real late metadata reader also refuses an equal completed fence.
            EXPECT_FALSE(lane.Retire(handle, completedSequence));
            for (unsigned entry = 0; entry < 2; ++entry) {
                harken::ImmediateLaneEntryOutcomeSnapshot outcome{};
                ASSERT_TRUE(lane.CapturePrivateEntryOutcome(handle, 17, entry, scopes[entry].entry,
                    mappings[entry].coordinatorBucket, completedSequence, &outcome));
                EXPECT_EQ(harken::ImmediateLaneEntryOutcome::Withdrawn, outcome.outcome);
            }
            ++completedSequence;
        }
        ASSERT_TRUE(lane.Cancel(handle));
        EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, 0,
            scopes[0].entry, pool, arms, decisions, &refused));
        ASSERT_TRUE(lane.Retire(handle, completedSequence));
        harken::ImmediateLaneHandle reused{};
        ASSERT_TRUE(lane.Reserve(17, demand, &reused));
        EXPECT_NE(handle.generation, reused.generation);
        if (mode >= 4) {
            EXPECT_FALSE(lane.RequestPrivateEntryStop(handle, 17, 0, scopes[0].entry));
            EXPECT_FALSE(lane.RequestPrivateEntryStop(reused, 17, 0, scopes[0].entry));
            harken::ImmediateLaneEntryOutcomeSnapshot old{};
            old.audioSequence = 4242;
            const auto oldSentinel = old;
            EXPECT_FALSE(lane.CapturePrivateEntryOutcome(handle, 17, 0, scopes[0].entry,
                mappings[0].coordinatorBucket, completedSequence, &old));
            EXPECT_EQ(oldSentinel, old);
        }
        EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(handle, 17, 0,
            scopes[0].entry, pool, arms, decisions, &refused));
        EXPECT_FALSE(lane.CapturePrivateVariableControlEntryMapping(reused, 17, 0,
            scopes[0].entry, pool, arms, decisions, &refused));
        EXPECT_EQ(sentinel, refused);
        ASSERT_TRUE(lane.Cancel(reused));
        ASSERT_TRUE(lane.Retire(reused));
        for (unsigned entry = 0; entry < 2; ++entry) {
            if (mode != 14) {
                ASSERT_TRUE(decisions.RollbackPreparedFrontierDecision(tokens[entry][2], tokens[entry][2]));
                ASSERT_TRUE(decisions.CancelPreparedFrontierActivation(tokens[entry][2]));
            }
            for (unsigned side = 0; side < 2; ++side) ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[entry][side], completedSequence));
            ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[entry][2], completedSequence));
        }
        ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, completedSequence,
            accepted ? acceptedOriginals.data() : nullptr, accepted ? acceptedOriginals.size() : 0));
        for (const auto arm : actualArms) ASSERT_TRUE(arms.CancelPrepared(arm));
        for (const auto arm : foreignTokens) ASSERT_TRUE(foreignArms.CancelPrepared(arm));
        for (const auto group : coordinatorTokens) ASSERT_TRUE(coordinatorGroups.CancelBeforePublication(group));
        for (const auto& entry : tokens) for (const auto token : entry) {
            ASSERT_TRUE(decisions.Cancel(token));
            ASSERT_TRUE(decisions.ReclaimCancelled(token));
        }
        EXPECT_EQ(0u, arms.LiveCount());
        EXPECT_EQ(0u, foreignArms.LiveCount());
        EXPECT_EQ(0u, decisions.LiveCount());
        EXPECT_EQ(0u, coordinatorGroups.LiveCount());
        EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
        EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    }
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
struct MixedTimeGroupHarness {
    Harness owners;
    ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{1};
    harken::DecisionTable decisions{4, 1};
    harken::ImmediateActionLane lane;
    std::array<harken::DecisionToken, 4> tokens{};
    std::array<harken::Plan03GroupSliceIdentity, 4> groups{};
    std::array<std::uint64_t, 4> roots{};
    std::array<bool, 4> bound{};
    unsigned count = 0;

    void Init(std::uint32_t maxGroups) {
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = 4;
        config.maxArmsPerGroup = 1;
        config.maxImmediateLaneGroupRows = 4;
        config.maxImmediateLaneOwnerGates = 4;
        config.maxImmediateLaneDecisionGates = 4;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        ASSERT_TRUE(lane.Reset({2, 2, 1, 100, false, maxGroups}));
    }

    void ReserveGroup(unsigned index, std::uint64_t root, std::uint64_t frame,
        harken::ImmediateLaneHandle handle, bool fixed, std::uint64_t relative) {
        harken::DecisionReserveRequest request{{index + 1u}, 7, kEpoch, 1, 1, {}};
        ASSERT_TRUE(decisions.Reserve(request, &tokens[index]));
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = frame;
        input.decision = tokens[index];
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(root, 0, input, arms, &groups[index]));
        ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(groups[index], lane, root, handle.slot, handle.generation));
        roots[index] = root;
        count = index + 1;
        harken::ImmediateLaneDeclaredGroup declaration{};
        declaration.identity = groups[index];
        if (fixed) {
            declaration.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
            declaration.fixedFrame = frame;
        } else declaration.relativeFrame = relative;
        ASSERT_TRUE(lane.DeclareGroup(handle, declaration));
    }

    void Bind(unsigned index, std::uint64_t frame) {
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Coordinator;
        binding.entry.coordinator = tokens[index];
        binding.entry.attempt = 1;
        binding.entry.owner = {owners.owner, owners.ownerEpoch, 0, 0,
            owners.owner, owners.ownerEpoch, 0, 1};
        binding.primary = groups[index];
        binding.operation.kind = harken::FrontierOperationKind::Entry;
        binding.operation.owner = binding.entry.owner;
        binding.operation.group = groups[index];
        binding.operation.world = 7;
        binding.operation.transport = kEpoch;
        binding.operation.order.executeFrame = frame;
        binding.operation.order.logicCommitSequence = index + 1;
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(groups[index], binding));
        bound[index] = true;
    }

    void Cleanup(harken::ImmediateLaneHandle handle, harken::ImmediateLaneHandle blocker = {}) {
        ASSERT_TRUE(lane.Cancel(handle));
        ASSERT_TRUE(lane.Retire(handle));
        if (blocker.Valid()) {
            ASSERT_TRUE(lane.Cancel(blocker));
            ASSERT_TRUE(lane.Retire(blocker));
        }
        for (unsigned index = 0; index < count; ++index)
            if (bound[index]) ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[index], 0));
        for (unsigned index = 0; index < count; ++index) {
            bool prior = false;
            for (unsigned previous = 0; previous < index; ++previous) prior = prior || roots[index] == roots[previous];
            if (!prior) ASSERT_TRUE(pool.CancelImmediateLaneGroups(roots[index], 0));
            ASSERT_TRUE(decisions.Cancel(tokens[index]));
            ASSERT_TRUE(decisions.ReclaimCancelled(tokens[index]));
        }
        EXPECT_EQ(0u, decisions.LiveCount());
        EXPECT_EQ(0u, arms.LiveCount());
        EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    }
};

struct PrivatePoolPublicationHarness : MixedTimeGroupHarness {
    harken::ImmediateLaneHandle handle{};
    harken::PrivateImmediateLanePublicationProof lanePublication;
    void Prepare(bool installGroupPins = true) {
        ASSERT_NO_FATAL_FAILURE(Init(2));
        ASSERT_TRUE(lane.Reserve(17, {0, 2, 0, 1}, &handle));
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(0, 17, 200, handle, false, 0));
        ASSERT_NO_FATAL_FAILURE(ReserveGroup(1, 17, 200, handle, false, 0));
        ASSERT_TRUE(lane.Prepare(handle, nullptr, 0));
        std::uint64_t anchor = 0;
        ASSERT_TRUE(lane.TryPlace(handle, 200, 0, &anchor));
        ASSERT_EQ(200u, anchor);
        ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 0));
        ASSERT_NO_FATAL_FAILURE(Bind(0, 200));
        ASSERT_NO_FATAL_FAILURE(Bind(1, 200));
        if (installGroupPins) ASSERT_TRUE(lane.PinGroups(handle, pool, decisions, 2));
        ASSERT_TRUE(lane.PinArms(handle, arms));
        if (installGroupPins) ASSERT_TRUE(lane.ProvePrivatePublication(handle, 17, &lanePublication));
    }
};

TEST(ScheduledTransactionPool, PrivateGroupPublicationHoldsCompleteCensusAndAppliesOriginalRows) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    harken::PrivateImmediateLanePublicationProof absent;
    EXPECT_FALSE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(absent, &publication));
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    harken::PrivateImmediateLaneGroupsPublicationProof duplicate;
    EXPECT_FALSE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &duplicate));
    harken::Plan03GroupSliceInput extra{};
    extra.executeFrame = 200;
    extra.decision = actual.tokens[0];
    extra.sliceIndex = 1;
    harken::Plan03GroupSliceIdentity refused{};
    EXPECT_FALSE(actual.pool.ReserveImmediateLaneGroup(17, 0, extra, actual.arms, &refused));
    EXPECT_FALSE(refused.Valid());
    for (unsigned index = 0; index < actual.count; ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[index], &view));
        ASSERT_NE(nullptr, view.lease);
        EXPECT_EQ(harken::AudioSliceState::PublicationReserved, view.lease->Capture().state);
        ASSERT_NE(nullptr, view.frontier);
        EXPECT_TRUE(actual.pool.IsPrivateImmediateLaneGroupsPublicationProofFor(
            publication, actual.groups[index], view.frontier->entry));
        EXPECT_FALSE(actual.pool.IsPrivateImmediateLaneGroupsPublicationProofFor(
            duplicate, actual.groups[index], view.frontier->entry));
        auto changedEntry = view.frontier->entry;
        ++changedEntry.owner.serviceGeneration;
        EXPECT_FALSE(actual.pool.IsPrivateImmediateLaneGroupsPublicationProofFor(
            publication, actual.groups[index], changedEntry));
        EXPECT_EQ(harken::AudioSliceInvalidateDisposition::PublicationHeld,
            view.lease->Invalidate(harken::AudioSliceInvalidateReason::WorldReplacement));
        EXPECT_EQ(0u, view.lease->Capture().pendingReasonBits);
        EXPECT_FALSE(actual.pool.ResolvePlan03GroupSliceRt(actual.groups[index], &view));
    }
    harken::ScheduledTransactionPool::ApplyPrivateImmediateLaneGroupsPublication(std::move(publication));
    EXPECT_FALSE(publication.Valid());
    for (unsigned index = 0; index < actual.count; ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[index], &view));
        EXPECT_EQ(actual.groups[index], view.identity);
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
        EXPECT_EQ(200u, view.frontier->operation.order.executeFrame);
        EXPECT_FALSE(actual.pool.IsPrivateImmediateLaneGroupsPublicationProofFor(
            publication, actual.groups[index], view.frontier->entry));
        EXPECT_FALSE(actual.pool.ResolvePlan03GroupSliceRt(actual.groups[index], &view));
    }
    EXPECT_EQ(nullptr, actual.lane.CaptureDue(actual.handle.slot, 2));
    harken::ImmediateActionLane::ApplyPrivatePublication(std::move(actual.lanePublication));
    for (unsigned index = 0; index < actual.count; ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceRt(actual.groups[index], &view));
        EXPECT_EQ(actual.groups[index], view.identity);
        ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
            view.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    }
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationRejectsActualTimingMismatchAndRecovers) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    for (const std::uint64_t offset : {1u, 100u}) {
        ASSERT_TRUE(actual.pool.RebaseImmediateLaneGroups(17, offset));
        harken::PrivateImmediateLaneGroupsPublicationProof publication;
        EXPECT_FALSE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
        publication.Release();
        for (unsigned index = 0; index < actual.count; ++index) {
            harken::Plan03PageGroupView view{};
            ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[index], &view));
            EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
            EXPECT_EQ(0u, view.lease->Capture().pendingReasonBits);
        }
        ASSERT_TRUE(actual.pool.RebaseImmediateLaneGroups(17, 0));
        ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
        publication.Release();
    }
    actual.lanePublication.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationRejectsForeignPoolAndRequiresRealGroupPins) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(false));
    EXPECT_FALSE(actual.lane.ProvePrivatePublication(actual.handle, 17, &actual.lanePublication));
    ScheduledTransactionPool empty;
    ASSERT_TRUE(empty.Reset(MakeConfig(), 0, kEpoch));
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    EXPECT_FALSE(empty.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    publication.Release();
    EXPECT_FALSE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    actual.lanePublication.Release();
    ASSERT_TRUE(actual.lane.PinGroups(actual.handle, actual.pool, actual.decisions, 2));
    ASSERT_TRUE(actual.lane.ProvePrivatePublication(actual.handle, 17, &actual.lanePublication));
    EXPECT_FALSE(empty.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    publication.Release();
    actual.lanePublication.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationLaneProofMoveAndEarlyReleaseReturnOriginalRows) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    harken::PrivateImmediateLanePublicationProof moved(std::move(actual.lanePublication));
    EXPECT_FALSE(actual.lanePublication.Valid());
    EXPECT_TRUE(publication.Valid());
    moved.Release();
    EXPECT_FALSE(publication.Valid());
    publication.Release();
    for (unsigned index = 0; index < actual.count; ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[index], &view));
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
        EXPECT_EQ(0u, view.lease->Capture().pendingReasonBits);
        EXPECT_FALSE(actual.pool.ResolvePlan03GroupSliceRt(actual.groups[index], &view));
    }
    ASSERT_TRUE(actual.lane.ProvePrivatePublication(actual.handle, 17, &actual.lanePublication));
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    actual.lanePublication = std::move(moved);
    EXPECT_FALSE(publication.Valid());
    publication.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationLaneOwnerDestructionRevokesActualEmptyCohort) {
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::PrivateImmediateLanePublicationProof lanePublication;
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    const harken::TimelineProgramView program{};
    {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 1, 1, 100}));
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(17, {1, 0, 0, 1}, &handle));
        harken::ImmediateLaneEvent event{};
        event.event.program.program = &program;
        ASSERT_TRUE(lane.Prepare(handle, &event, 1));
        std::uint64_t anchor = 0;
        ASSERT_TRUE(lane.TryPlace(handle, 200, 0, &anchor));
        ASSERT_TRUE(lane.ProvePrivatePublication(handle, 17, &lanePublication));
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupsPublication(lanePublication, &publication));
    }
    EXPECT_FALSE(lanePublication.Valid());
    EXPECT_FALSE(publication.Valid());
    publication.Release();
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationLaneCannotApplyBeforeOriginalRows) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    harken::ImmediateActionLane::ApplyPrivatePublication(std::move(actual.lanePublication));
    EXPECT_TRUE(actual.lanePublication.Valid());
    EXPECT_TRUE(publication.Valid());
    EXPECT_EQ(nullptr, actual.lane.CaptureDue(actual.handle.slot, 2));
    publication.Release();
    actual.lanePublication.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationLastLeaseRefusalKeepsEntireCohortReusable) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    harken::Plan03PageGroupView last{};
    ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[1], &last));
    ASSERT_TRUE(last.lease->PrepareReplacement());
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    EXPECT_FALSE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    EXPECT_FALSE(publication.Valid());
    harken::Plan03PageGroupView first{};
    ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[0], &first));
    EXPECT_EQ(harken::AudioSliceState::Ready, first.lease->Capture().state);
    EXPECT_EQ(0u, first.lease->Capture().pendingReasonBits);
    EXPECT_EQ(harken::AudioSliceState::ReplacementReserved, last.lease->Capture().state);
    ASSERT_TRUE(last.lease->CancelPreparedReplacement());
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    publication.Release();
    actual.lanePublication.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationRejectsDetachedLastFrontierAndRecovers) {
    PrivatePoolPublicationHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    ASSERT_TRUE(actual.pool.DetachPlan03GroupFrontier(actual.groups[1], 0));
    actual.bound[1] = false;
    harken::Plan03PageGroupView detached{};
    ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[1], &detached));
    ASSERT_EQ(nullptr, detached.frontier);
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    EXPECT_FALSE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    publication.Release();
    for (unsigned index = 0; index < actual.count; ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(actual.pool.ResolvePlan03GroupSliceProducer(actual.groups[index], &view));
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
        EXPECT_EQ(0u, view.lease->Capture().pendingReasonBits);
    }
    ASSERT_NO_FATAL_FAILURE(actual.Bind(1, 200));
    ASSERT_TRUE(actual.pool.ProvePrivateImmediateLaneGroupsPublication(actual.lanePublication, &publication));
    publication.Release();
    actual.lanePublication.Release();
    ASSERT_NO_FATAL_FAILURE(actual.Cleanup(actual.handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationPoolOwnerDestructionRetainsOriginalLaneProof) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 100}));
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, {1, 0, 0, 1}, &handle));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneEvent event{};
    event.event.program.program = &program;
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 200, 0, &anchor));
    harken::PrivateImmediateLanePublicationProof lanePublication;
    ASSERT_TRUE(lane.ProvePrivatePublication(handle, 17, &lanePublication));
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    {
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupsPublication(lanePublication, &publication));
    }
    EXPECT_FALSE(publication.Valid());
    EXPECT_TRUE(lanePublication.Valid());
    EXPECT_FALSE(lane.Cancel(handle));
    ScheduledTransactionPool replacement;
    ASSERT_TRUE(replacement.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_TRUE(replacement.ProvePrivateImmediateLaneGroupsPublication(lanePublication, &publication));
    harken::ScheduledTransactionPool::ApplyPrivateImmediateLaneGroupsPublication(std::move(publication));
    harken::ImmediateActionLane::ApplyPrivatePublication(std::move(lanePublication));
    const auto* bucket = lane.CaptureDue(handle.slot, 2);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationFreezesActualZeroGroupCohort) {
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 100}));
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, {1, 0, 0, 1}, &handle));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneEvent event{};
    event.event.program.program = &program;
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 200, 0, &anchor));
    harken::PrivateImmediateLanePublicationProof lanePublication;
    ASSERT_TRUE(lane.ProvePrivatePublication(handle, 17, &lanePublication));
    harken::PrivateImmediateLaneGroupsPublicationProof publication;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupsPublication(lanePublication, &publication));
    EXPECT_FALSE(pool.ResetForEpoch(0, kEpoch + 1));
    harken::TimelineArmTable arms{1};
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 200;
    input.decision = {0, 1};
    harken::Plan03GroupSliceIdentity refused{};
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(17, 0, input, arms, &refused));
    harken::ScheduledTransactionPool::ApplyPrivateImmediateLaneGroupsPublication(std::move(publication));
    harken::ImmediateActionLane::ApplyPrivatePublication(std::move(lanePublication));
    const auto* bucket = lane.CaptureDue(handle.slot, 2);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, PrivateGroupPublicationMoveAbandonAndOwnerDestructionReleaseExactCohort) {
    harken::PrivateImmediateLaneGroupsPublicationProof survivor;
    auto first = std::make_unique<PrivatePoolPublicationHarness>();
    auto second = std::make_unique<PrivatePoolPublicationHarness>();
    ASSERT_NO_FATAL_FAILURE(first->Prepare());
    ASSERT_NO_FATAL_FAILURE(second->Prepare());
    ASSERT_TRUE(first->pool.ProvePrivateImmediateLaneGroupsPublication(first->lanePublication, &survivor));
    harken::PrivateImmediateLaneGroupsPublicationProof other;
    ASSERT_TRUE(second->pool.ProvePrivateImmediateLaneGroupsPublication(second->lanePublication, &other));
    survivor = std::move(other);
    EXPECT_FALSE(other.Valid());
    for (unsigned index = 0; index < first->count; ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(first->pool.ResolvePlan03GroupSliceProducer(first->groups[index], &view));
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
    }
    harken::PrivateImmediateLaneGroupsPublicationProof moved(std::move(survivor));
    EXPECT_FALSE(survivor.Valid());
    moved.Release();
    ASSERT_TRUE(second->pool.ProvePrivateImmediateLaneGroupsPublication(second->lanePublication, &survivor));
    second->lanePublication.Release();
    second.reset();
    EXPECT_FALSE(survivor.Valid());
    survivor.Release();
    first->lanePublication.Release();
    ASSERT_NO_FATAL_FAILURE(first->Cleanup(first->handle));
}

TEST(ScheduledTransactionPool, MixedTimeRealGroupCapacitySlideReordersOriginalPinnedGroups) {
    MixedTimeGroupHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Init(1));
    harken::ImmediateLaneHandle mixed{}, blocker{};
    ASSERT_TRUE(h.lane.Reserve(17, {0, 2, 0, 2}, &mixed));
    ASSERT_TRUE(h.lane.Reserve(18, {0, 2, 0, 2}, &blocker));
    ASSERT_NO_FATAL_FAILURE(h.ReserveGroup(0, 17, 400, mixed, false, 0));
    ASSERT_NO_FATAL_FAILURE(h.ReserveGroup(1, 17, 200, mixed, true, 0));
    ASSERT_NO_FATAL_FAILURE(h.ReserveGroup(2, 18, 100, blocker, false, 0));
    ASSERT_NO_FATAL_FAILURE(h.ReserveGroup(3, 18, 300, blocker, false, 200));
    ASSERT_TRUE(h.lane.Prepare(blocker, nullptr, 0));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(h.lane.TryPlace(blocker, 100, 0, &anchor));
    ASSERT_TRUE(h.lane.Prepare(mixed, nullptr, 0));
    anchor = 777;
    EXPECT_FALSE(h.lane.TryPlace(mixed, 100, 2, &anchor));
    EXPECT_EQ(777u, anchor);
    EXPECT_FALSE(h.lane.Publish(mixed));
    ASSERT_TRUE(h.lane.TryPlace(mixed, 100, 3, &anchor));
    ASSERT_EQ(400u, anchor);
    ASSERT_TRUE(h.lane.TryPlace(mixed, 100, 3, &anchor));
    ASSERT_TRUE(h.pool.RebaseImmediateLaneGroups(17, 0));
    ASSERT_NO_FATAL_FAILURE(h.Bind(0, 400));
    ASSERT_NO_FATAL_FAILURE(h.Bind(1, 200));
    ASSERT_TRUE(h.lane.PinGroups(mixed, h.pool, h.decisions, 1));
    harken::Plan03GroupSliceIdentity actual{};
    ASSERT_TRUE(h.lane.CapturePrivatePinGroup(mixed, 0, &actual));
    EXPECT_EQ(h.groups[1], actual);
    ASSERT_TRUE(h.lane.CapturePrivatePinGroup(mixed, 1, &actual));
    EXPECT_EQ(h.groups[0], actual);
    ASSERT_TRUE(h.lane.Publish(mixed));
    harken::Plan03PageGroupView relativeView{};
    ASSERT_TRUE(h.pool.ResolvePlan03GroupSliceProducer(h.groups[0], &relativeView));
    ASSERT_NE(nullptr, relativeView.frontier);
    EXPECT_EQ(400u, relativeView.frontier->operation.order.executeFrame);
    const auto* bucket = h.lane.CaptureDue(mixed.slot, 2);
    ASSERT_NE(nullptr, bucket);
    ASSERT_EQ(0u, bucket->eventCount);
    ASSERT_EQ(1u, bucket->groupCount);
    const auto* groups = h.lane.Groups(mixed.slot, bucket);
    ASSERT_NE(nullptr, groups);
    EXPECT_EQ(h.groups[1], groups[0].identity);
    EXPECT_EQ(2u, groups[0].dueQuantum);
    // This mapping control exits by actual cancellation without audio apply.
    ASSERT_TRUE(h.lane.Cancel(mixed));
    ASSERT_TRUE(h.lane.ReleaseCapture(mixed.slot, bucket));
    ASSERT_NO_FATAL_FAILURE(h.Cleanup(mixed, blocker));
}

TEST(ScheduledTransactionPool, MixedTimeRealGroupUndercreditRefusalKeepsPriorGeometryAndPins) {
    MixedTimeGroupHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Init(2));
    harken::ImmediateLaneHandle mixed{};
    ASSERT_TRUE(h.lane.Reserve(17, {0, 2, 0, 1}, &mixed));
    ASSERT_NO_FATAL_FAILURE(h.ReserveGroup(0, 17, 200, mixed, false, 0));
    ASSERT_NO_FATAL_FAILURE(h.ReserveGroup(1, 17, 200, mixed, true, 0));
    ASSERT_TRUE(h.lane.Prepare(mixed, nullptr, 0));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(h.lane.TryPlace(mixed, 200, 0, &anchor));
    anchor = 777;
    EXPECT_FALSE(h.lane.TryPlace(mixed, 100, 0, &anchor));
    EXPECT_EQ(777u, anchor);
    ASSERT_TRUE(h.pool.RebaseImmediateLaneGroups(17, 0));
    ASSERT_NO_FATAL_FAILURE(h.Bind(0, 200));
    ASSERT_NO_FATAL_FAILURE(h.Bind(1, 200));
    ASSERT_TRUE(h.lane.PinGroups(mixed, h.pool, h.decisions, 2));
    ASSERT_TRUE(h.lane.Publish(mixed));
    EXPECT_EQ(nullptr, h.lane.CaptureDue(mixed.slot, 1));
    const auto* bucket = h.lane.CaptureDue(mixed.slot, 2);
    ASSERT_NE(nullptr, bucket);
    EXPECT_EQ(0u, bucket->eventCount);
    ASSERT_EQ(2u, bucket->groupCount);
    const auto* groups = h.lane.Groups(mixed.slot, bucket);
    ASSERT_NE(nullptr, groups);
    EXPECT_EQ(h.groups[0], groups[0].identity);
    EXPECT_EQ(h.groups[1], groups[1].identity);
    ASSERT_TRUE(h.lane.Cancel(mixed));
    ASSERT_TRUE(h.lane.ReleaseCapture(mixed.slot, bucket));
    ASSERT_NO_FATAL_FAILURE(h.Cleanup(mixed));
}

TEST(ScheduledTransactionPool, MixedTimePinnedCrossSliceFuturePrecedesSlidUnrelatedAnchor) {
    Harness owners;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 3;
    config.maxArmsPerGroup = 2;
    config.maxImmediateLaneGroupRows = 3;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms{3};
    harken::DecisionTable decisions{2, 2};
    harken::QueueInstanceTable queues{1};
    harken::QueueInstanceToken queue{};
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = owners.owner;
    create.initial.version = 1;
    create.containerStableId = harkentest::ObjectId(9);
    create.programSemanticFingerprint = 7;
    create.childCount = 2;
    ASSERT_TRUE(queues.Acquire(create, &queue));
    harken::TransitionInstancePool transitions{1};
    harken::TransitionInstanceToken transition{};
    ASSERT_TRUE(transitions.Acquire({200, 0}, &transition));
    harken::DecisionReserveRequest request{{9}, 7, kEpoch, 1, 1, {}};
    harken::DecisionToken primaryDecision{}, memberDecision{};
    ASSERT_TRUE(decisions.Reserve(request, &primaryDecision));
    request.sliceCount = 2;
    request.transitionGate = transition;
    ASSERT_TRUE(decisions.Reserve(request, &memberDecision));
    std::array<harken::TimelineSemanticTarget, 3> targets{};
    std::array<harken::TimelineArmToken, 3> tokens{};
    for (unsigned member = 0; member < 2; ++member) {
        targets[member].programSemanticFingerprint = 40 + member;
        ASSERT_TRUE(arms.PreparePendingImmediateGrouped(member, 1, member + 1,
            targets[member], harken::ParentAudioGroupRef{memberDecision}, member, &tokens[member]));
    }
    ASSERT_TRUE(arms.Prepare(2, 1, 300, 300, 3, targets[2], &tokens[2]));
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 5, 3, 100, true, 3}));
    harken::ImmediateLaneHandle handle{}, blocker{};
    ASSERT_TRUE(lane.Reserve(17, {5, 3, 4, 3}, &handle));
    std::array<harken::Plan03GroupSliceIdentity, 3> groups{};
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 100;
    input.decision = primaryDecision;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 0, input, arms, &groups[0]));
    input.decision = memberDecision;
    input.members = tokens.data();
    input.memberCount = 2;
    input.sourceControlCount = 1;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 0, input, arms, &groups[1]));
    input.executeFrame = 200;
    input.sliceIndex = 1;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 0, input, arms, &groups[2]));
    for (unsigned group = 0; group < groups.size(); ++group) {
        ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(groups[group], lane, 17, handle.slot, handle.generation));
        harken::ImmediateLaneDeclaredGroup declaration{};
        declaration.identity = groups[group];
        declaration.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
        declaration.fixedFrame = group == 2 ? 200 : 100;
        ASSERT_TRUE(lane.DeclareGroup(handle, declaration));
    }
    // Preserve the actual frozen pool frames while the unrelated relative event slides.
    ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 0));
    const harken::FrontierEntryReceiptRef entry{primaryDecision, 1,
        {owners.owner, owners.ownerEpoch, 0, 0, owners.owner, owners.ownerEpoch, 0, 1}};
    harken::FrontierGroupBinding binding{};
    binding.entry = entry;
    binding.primary = groups[0];
    binding.operation.owner = entry.owner;
    binding.operation.world = 7;
    binding.operation.transport = kEpoch;
    binding.operation.order.logicCommitSequence = 9;
    for (unsigned group = 0; group < groups.size(); ++group) {
        binding.role = group == 0 ? harken::FrontierGroupRole::Coordinator :
            (group == 1 ? harken::FrontierGroupRole::EntryMember : harken::FrontierGroupRole::Future);
        binding.operation.kind = group == 2 ? harken::FrontierOperationKind::Dependent : harken::FrontierOperationKind::Entry;
        binding.operation.group = groups[group];
        binding.operation.order.executeFrame = group == 2 ? 200 : 100;
        if (group != 0) binding.queueInitial = {queue.slot, queue.generation, create.initial.version, create.programSemanticFingerprint};
        if (group == 2) {
            binding.prerequisite = entry;
            binding.prerequisiteMember = groups[1];
            binding.transition = {transition.slot, transition.generation, 200};
        }
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(groups[group], binding));
    }
    std::array<harken::TimelineEventPayload, 4> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Envelope;
    payloads[0].target = targets[0];
    payloads[1].kind = harken::TimelineEventKind::Start;
    payloads[1].target = targets[1];
    payloads[1].reserved = harken::kTimelineEventTransientStartArm;
    payloads[2].kind = harken::TimelineEventKind::Envelope;
    payloads[2].target = targets[1];
    payloads[3].kind = harken::TimelineEventKind::Start;
    payloads[3].target = targets[2];
    std::array<harken::TimelineProgramView, 3> programs{
        harken::TimelineProgramView{1, &payloads[0], 1},
        harken::TimelineProgramView{2, &payloads[1], 2},
        harken::TimelineProgramView{3, &payloads[3], 1}};
    const auto* control = decisions.PrepareFrontierControlProgram(primaryDecision, 1);
    ASSERT_NE(nullptr, control);
    std::array<harken::ImmediateLaneEvent, 5> events{};
    for (unsigned ordinal = 0; ordinal < events.size(); ++ordinal) {
        auto& laneEvent = events[ordinal];
        auto& event = laneEvent.event;
        const auto member = ordinal == 1 ? 0u : (ordinal < 4 ? 1u : 2u);
        event.program = ordinal == 0 ? harken::TimelineTransactionProgramRef{control->generation, control} :
            harken::TimelineTransactionProgramRef{programs[member].generation, &programs[member]};
        event.payload = ordinal == 3 ? 1u : 0u;
        event.logicCommitSequence = 9;
        event.eventIndex = ordinal;
        event.rowSequence = ordinal + 1;
        event.ownerInstanceHandle = owners.owner;
        event.ownerInstanceScheduleEpoch = owners.ownerEpoch;
        event.frozenTxnValid = true;
        if (ordinal != 0) event.armToken = tokens[member];
        if (ordinal < 4) {
            laneEvent.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
            laneEvent.fixedFrame = ordinal < 2 ? 100 : 200;
            laneEvent.futureGroupedStart = ordinal == 2;
            event.frontier.kind = ordinal == 0 ? harken::FrontierOperationKind::Entry : harken::FrontierOperationKind::Dependent;
            event.frontier.entry = entry;
            event.frontier.owner = entry.owner;
            event.frontier.group = groups[ordinal == 0 ? 0 : (ordinal == 1 ? 1 : 2)];
            event.frontier.world = 7;
            event.frontier.transport = kEpoch;
            event.frontier.operationEventCount = ordinal == 0 ? 1u : 2u;
            event.frontier.entryEventOrdinal = ordinal == 3 ? 1u : 0u;
        }
    }
    const auto declare = [&](harken::ImmediateLaneMemberKind kind, unsigned group,
        unsigned related, unsigned member, unsigned span) {
        harken::ImmediateLaneMemberManifest declaration{};
        declaration.kind = kind;
        declaration.group = groups[group];
        if (related < groups.size()) declaration.relatedGroup = groups[related];
        declaration.arm = tokens[member];
        declaration.eventSpanCount = static_cast<std::uint16_t>(span);
        declaration.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
        declaration.fixedFrame = member == 0 ? 100 : 200;
        declaration.programGeneration = programs[member].generation;
        declaration.target = targets[member];
        declaration.ownerInstance = owners.owner;
        declaration.ownerEpoch = owners.ownerEpoch;
        ASSERT_TRUE(lane.DeclareMember(handle, declaration));
    };
    ASSERT_NO_FATAL_FAILURE(declare(harken::ImmediateLaneMemberKind::CurrentEventSpan, 1, 3, 0, 1));
    ASSERT_NO_FATAL_FAILURE(declare(harken::ImmediateLaneMemberKind::FrozenFutureTarget, 1, 2, 1, 2));
    ASSERT_NO_FATAL_FAILURE(declare(harken::ImmediateLaneMemberKind::HistoricalSourceCompletion, 2, 1, 0, 1));
    ASSERT_NO_FATAL_FAILURE(declare(harken::ImmediateLaneMemberKind::CurrentEventSpan, 2, 3, 1, 2));
    ASSERT_TRUE(lane.Prepare(handle, events.data(), events.size()));
    ASSERT_TRUE(lane.Reserve(18, 2, &blocker));
    const std::array pressure{events[4], events[4]};
    auto pressureEvents = pressure;
    pressureEvents[1].relativeFrame = 100;
    ASSERT_TRUE(lane.Prepare(blocker, pressureEvents.data(), pressureEvents.size()));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(blocker, 100, 0, &anchor));
    ASSERT_TRUE(lane.TryPlace(handle, 100, 2, &anchor));
    ASSERT_EQ(300u, anchor);
    ASSERT_TRUE(lane.PinGroups(handle, pool, decisions, 3));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    const harken::ImmediateLaneEvent* future = nullptr;
    EXPECT_FALSE(lane.FutureMember(handle.slot, handle.generation, groups[2], 1, tokens[1], &future));
    ASSERT_TRUE(lane.Publish(handle));
    EXPECT_TRUE(lane.FutureMember(handle.slot, handle.generation, groups[2], 1, tokens[1], &future));
    EXPECT_NE(nullptr, future);
    if (future != nullptr) {
        EXPECT_EQ(200u, future->event.executeFrame);
        EXPECT_EQ(harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame, future->timeDomain);
    }
    harken::ImmediateLaneMemberManifest frozen{};
    ASSERT_TRUE(lane.CaptureFrozenFutureTarget(handle, groups[1], groups[2], &frozen));
    EXPECT_EQ(groups[2], frozen.relatedGroup);
    EXPECT_EQ(tokens[1].generation, frozen.arm.generation);
    EXPECT_EQ(200u, frozen.fixedFrame);
    ASSERT_EQ(2u, frozen.eventSpanCount);
    ASSERT_NE(UINT32_MAX, frozen.secondEventOrdinal);
    EXPECT_GT(frozen.secondEventOrdinal, frozen.eventOrdinal);
    EXPECT_LT(frozen.secondEventOrdinal, events.size());
    const auto originalFrozen = frozen;
    payloads[2].target = targets[2];
    EXPECT_FALSE(lane.CaptureFrozenFutureTarget(handle, groups[1], groups[2], &frozen));
    EXPECT_EQ(originalFrozen.eventOrdinal, frozen.eventOrdinal);
    EXPECT_EQ(originalFrozen.secondEventOrdinal, frozen.secondEventOrdinal);
    EXPECT_EQ(originalFrozen.target, frozen.target);
    payloads[2].target = targets[1];
    ASSERT_TRUE(lane.CaptureFrozenFutureTarget(handle, groups[1], groups[2], &frozen));
    EXPECT_EQ(originalFrozen.eventOrdinal, frozen.eventOrdinal);
    EXPECT_EQ(originalFrozen.secondEventOrdinal, frozen.secondEventOrdinal);
    const auto* sentinel = future;
    EXPECT_FALSE(lane.FutureMember(handle.slot, handle.generation + 1, groups[2], 1, tokens[1], &future));
    EXPECT_FALSE(lane.FutureMember(handle.slot, handle.generation, groups[2], 0, tokens[0], &future));
    auto wrongArm = tokens[1];
    ++wrongArm.renderGeneration;
    EXPECT_FALSE(lane.FutureMember(handle.slot, handle.generation, groups[2], 1, wrongArm, &future));
    EXPECT_EQ(sentinel, future);
    // These queries grant no applied receipt, start fact, or audible result.
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    ASSERT_TRUE(lane.Cancel(blocker));
    ASSERT_TRUE(lane.Retire(blocker));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[1], 0));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[2], 0));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(groups[0], 0));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0));
    for (const auto token : tokens) ASSERT_TRUE(arms.CancelPrepared(token));
    for (const auto token : {primaryDecision, memberDecision}) {
        ASSERT_TRUE(decisions.Cancel(token));
        ASSERT_TRUE(decisions.ReclaimCancelled(token));
    }
    ASSERT_TRUE(transitions.CancelBeforePublication(transition));
    ASSERT_TRUE(queues.Release(queue));
    EXPECT_EQ(0u, arms.LiveCount());
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_EQ(0u, transitions.LiveCount());
    EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, NativeDisposalReportsPartialSuccessBeforeSlotReuse) {
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 2;
    config.maxArmsPerGroup = 1;
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 2;
    config.maxImmediateLaneDecisionGates = 2;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    harken::DecisionTable decisions(2, 2);
    std::array<harken::DecisionToken, 2> decisionTokens{};
    std::array<harken::Plan03GroupSliceInput, 2> inputs{};
    std::array<harken::Plan03GroupSliceIdentity, 2> identities{};
    std::array<harken::OriginalQueueFutureGroupRegistration, 2> baselines{};
    struct Observation {
        harken::Plan03GroupSliceIdentity identity{};
        std::uint64_t root = 0;
        std::uint64_t domain = UINT64_MAX;
        bool baselinePresent = false;
    };
    struct Observations {
        decltype(baselines)* originals = nullptr;
        std::array<Observation, 3> events{};
        std::size_t count = 0;
    } observed{&baselines};
    pool.SetImmediateLaneGroupDisposalObserver(&observed,
        [](void* opaque, harken::Plan03GroupSliceIdentity identity,
            std::uint64_t root, std::uint64_t domain) noexcept {
            auto& values = *static_cast<Observations*>(opaque);
            const auto index = values.count++;
            if (index >= values.events.size()) return;
            const auto& original = (*values.originals)[identity.committedRow];
            values.events[index] = {identity, root, domain,
                original.Valid() && original.Identity() == identity};
        });
    for (std::uint32_t index = 0; index < decisionTokens.size(); ++index) {
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = index + 1u;
        request.world = 7;
        request.transport = kEpoch;
        request.expectedContinuationGeneration = 1;
        request.sliceCount = 1;
        ASSERT_TRUE(decisions.Reserve(request, &decisionTokens[index]));
        inputs[index].executeFrame = 120 + 40u * index;
        inputs[index].decision = decisionTokens[index];
        harken::PreparedPrivateImmediateLaneGroup prepared;
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, inputs[index], arms, &identities[index], &prepared));
        harken::PrivateImmediateLaneGroupTransferProof proof;
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(prepared, 17, 100, inputs[index], arms, &proof));
        ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &baselines[identities[index].committedRow]));
        auto& baseline = baselines[identities[index].committedRow];
        harken::OriginalQueueFutureGroupScope scope{};
        ASSERT_TRUE(baseline.CaptureOriginalScope(pool, arms, &scope));
        EXPECT_EQ(17u, scope.cohortRoot);
        EXPECT_EQ(100u, scope.provisionalAnchorFrame);
        EXPECT_EQ(0u, scope.selectedFutureDomain);
        EXPECT_EQ(inputs[index].executeFrame, scope.input.executeFrame);
        EXPECT_EQ(inputs[index].decision.slot, scope.input.decision.slot);
        EXPECT_EQ(inputs[index].decision.generation, scope.input.decision.generation);
        EXPECT_EQ(0u, scope.input.memberCount);
        EXPECT_TRUE(baseline.MatchesOriginalScope(scope.cohortRoot,
            scope.provisionalAnchorFrame, scope.input, arms));
        harken::ScheduledTransactionPool foreignPool;
        harken::TimelineArmTable foreignArms(1);
        EXPECT_FALSE(baseline.CaptureOriginalScope(foreignPool, arms, &scope));
        EXPECT_FALSE(baseline.CaptureOriginalScope(pool, foreignArms, &scope));
        EXPECT_EQ(100u, scope.provisionalAnchorFrame);
    }
    harken::OriginalQueueFutureGroupLifetimePin busy;
    ASSERT_TRUE(baselines[identities[1].committedRow].BorrowLifetime(&busy));
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 0, baselines.data(), baselines.size()));
    ASSERT_EQ(1u, observed.count);
    EXPECT_EQ(identities[0], observed.events[0].identity);
    EXPECT_EQ(17u, observed.events[0].root);
    EXPECT_EQ(0u, observed.events[0].domain);
    EXPECT_TRUE(observed.events[0].baselinePresent);
    EXPECT_FALSE(baselines[identities[0].committedRow].Valid());
    harken::OriginalQueueFutureGroupScope refusedScope{};
    refusedScope.cohortRoot = 99;
    EXPECT_FALSE(baselines[identities[0].committedRow].CaptureOriginalScope(pool, arms, &refusedScope));
    EXPECT_EQ(99u, refusedScope.cohortRoot);
    EXPECT_TRUE(baselines[identities[1].committedRow].Valid());
    EXPECT_TRUE(busy.Valid());
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, identities[0]));
    EXPECT_EQ(1u, observed.count);
    busy.Release();
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0, baselines.data(), baselines.size()));
    ASSERT_EQ(2u, observed.count);
    EXPECT_EQ(identities[1], observed.events[1].identity);
    EXPECT_TRUE(observed.events[1].baselinePresent);
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0, baselines.data(), baselines.size()));
    EXPECT_EQ(2u, observed.count);

    harken::PreparedPrivateImmediateLaneGroup replacement;
    harken::Plan03GroupSliceIdentity next{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, inputs[0], arms, &next, &replacement));
    ASSERT_EQ(identities[0].committedRow, next.committedRow);
    EXPECT_NE(identities[0].rowClaimGeneration, next.rowClaimGeneration);
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(replacement, 17, 100, inputs[0], arms, &proof));
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &baselines[next.committedRow]));
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, identities[0], &baselines[next.committedRow]));
    EXPECT_EQ(2u, observed.count);
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, next, &baselines[next.committedRow]));
    ASSERT_EQ(3u, observed.count);
    EXPECT_EQ(next, observed.events[2].identity);
    EXPECT_TRUE(observed.events[2].baselinePresent);
    EXPECT_EQ(identities[0], observed.events[0].identity);
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, next));
    EXPECT_EQ(3u, observed.count);
    for (const auto token : decisionTokens) {
        ASSERT_TRUE(decisions.Cancel(token));
        ASSERT_TRUE(decisions.ReclaimCancelled(token));
    }
    pool.SetImmediateLaneGroupDisposalObserver(nullptr, nullptr);
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, OriginalQueueGroupScopePreservesIssuedInputsAfterRebase) {
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 2;
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 1;
    config.maxImmediateLaneDecisionGates = 1;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(2);
    harken::DecisionTable decisions(1, 1);
    harken::DecisionReserveRequest request{{9}, 7, kEpoch, 1, 1, {}};
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(request, &decision));
    std::array<harken::TimelineArmToken, 2> tokens{};
    for (std::uint16_t member = 0; member < tokens.size(); ++member) {
        harken::TimelineSemanticTarget target{};
        target.programSemanticFingerprint = 40 + member;
        ASSERT_TRUE(arms.PreparePendingImmediateGrouped(member, 1, member + 1, target,
            harken::ParentAudioGroupRef{decision}, member, &tokens[member]));
    }
    harken::ImmediateEntryAttemptGate gate;
    ASSERT_TRUE(gate.Prepare(4));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = decision;
    input.members = tokens.data();
    input.memberCount = 2;
    input.sourceControlCount = 1;
    input.immediateEntry = {&gate, 4};
    harken::Plan03GroupSliceIdentity identity{};
    harken::PreparedPrivateImmediateLaneGroup prepared;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &identity, &prepared));
    harken::PrivateImmediateLaneGroupTransferProof transfer;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(prepared, 17, 100, input, arms, &transfer));
    harken::OriginalQueueFutureGroupRegistration original;
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(transfer), &original));
    ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 300));
    harken::OriginalQueueFutureGroupScope scope{};
    ASSERT_TRUE(original.CaptureOriginalScope(pool, arms, &scope));
    EXPECT_EQ(17u, scope.cohortRoot);
    EXPECT_EQ(100u, scope.provisionalAnchorFrame);
    EXPECT_EQ(120u, scope.input.executeFrame);
    EXPECT_EQ(2u, scope.input.memberCount);
    EXPECT_EQ(1u, scope.input.sourceControlCount);
    EXPECT_EQ(&gate, scope.input.immediateEntry.gate);
    EXPECT_EQ(4u, scope.input.immediateEntry.generation);
    ASSERT_NE(nullptr, scope.input.members);
    for (std::uint16_t member = 0; member < tokens.size(); ++member) {
        EXPECT_EQ(tokens[member].slot, scope.input.members[member].slot);
        EXPECT_EQ(tokens[member].generation, scope.input.members[member].generation);
        EXPECT_EQ(tokens[member].renderSlot, scope.input.members[member].renderSlot);
        EXPECT_EQ(tokens[member].renderGeneration, scope.input.members[member].renderGeneration);
    }
    EXPECT_TRUE(original.MatchesOriginalScope(scope.cohortRoot,
        scope.provisionalAnchorFrame, scope.input, arms));
    auto rebased = scope.input;
    rebased.executeFrame = 320;
    EXPECT_FALSE(original.MatchesOriginalScope(17, 300, rebased, arms));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0, &original, 1));
    EXPECT_FALSE(original.CaptureOriginalScope(pool, arms, &scope));
    EXPECT_EQ(100u, scope.provisionalAnchorFrame);
    for (const auto token : tokens) ASSERT_TRUE(arms.CancelPrepared(token));
    ASSERT_TRUE(decisions.Cancel(decision));
    ASSERT_TRUE(decisions.ReclaimCancelled(decision));
}

TEST(ScheduledTransactionPool, PrivateLaneTransferProofMaximumGenerationIsRealAndExhaustionDoesNotWrap) {
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 2;
    config.maxImmediateLaneDecisionGates = 2;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    harken::DecisionTable decisions(1, 1);
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = 1;
    request.world = 7;
    request.transport = kEpoch;
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(request, &decision));
    ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetFreeImmediateLaneGeneration(
        pool, 0, UINT32_MAX - 1));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = decision;
    harken::Plan03GroupSliceIdentity raw{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &raw, &original));
    ASSERT_EQ(UINT32_MAX, raw.rowClaimGeneration);
    ASSERT_TRUE(original.Valid());
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    EXPECT_TRUE(pool.PrivateImmediateLaneGroupTransferHeld(raw));
    const auto consumed = ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(std::move(proof));
    EXPECT_EQ(raw, consumed);
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, consumed));
    harken::Plan03GroupSliceIdentity refused{};
    harken::PreparedPrivateImmediateLaneGroup refusedOriginal;
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &refused, &refusedOriginal));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(refusedOriginal.Valid());
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, consumed));
    EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    ASSERT_TRUE(decisions.Cancel(decision));
    ASSERT_TRUE(decisions.ReclaimCancelled(decision));
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_EQ(0u, arms.LiveCount());
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}
#endif


namespace {
void CheckFiveEventPrivateBatchCapacity(unsigned shortage) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = shortage == 0 ? 4u : 8u;
    config.maxIngressTransactionsPerQuantum = shortage == 1 ? 4u : 8u;
    config.maxFrameBatchesPerQuantum = shortage == 2 ? 4u : 8u;
    config.maxEventsPerQuantum = shortage == 3 ? 4u : 8u;
    if (shortage == 3) config.maxFrameBatchesPerQuantum = 4;

    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    std::array<ScheduledEventInput, 5> events{};
    std::array<TransactionRequest, 5> requests{};
    for (unsigned i = 0; i < 5; ++i) {
        events[i].executeFrame = 10 + i;
        requests[i].transportEpoch = kEpoch;
        requests[i].ownerInstanceHandle = h.owner;
        requests[i].ownerInstanceScheduleEpoch = h.ownerEpoch;
        requests[i].events = &events[i];
        requests[i].eventCount = 1;
    }
    PreparedTransactionBatch prepared;
    const auto expected = shortage == 0 ? ReserveRefusal::PoolFull : shortage == 1
        ? ReserveRefusal::IngressRateExceeded : shortage == 2 ? ReserveRefusal::PageBatchCapacity
        : ReserveRefusal::PageEventCapacity;
    EXPECT_EQ(expected, pool.PrepareTransactionBatch(requests.data(), requests.size(), 0, &prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_EQ(0u, pool.TransactionHighWater());
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(requests.data(), 4, 0, &prepared));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(prepared, 51));
    std::array<ConsumedEvent, 4> original{};
    for (unsigned i = 0; i < 4; ++i) ASSERT_TRUE(pool.CapturePreparedTransactionEvent(prepared, i, 0, &original[i]));
    EXPECT_TRUE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 51, kEpoch, original.data(), original.size()));
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}
}
TEST(ScheduledTransactionPool, PrivatePageBatchRealFiveRequestsTransactionShortage) { CheckFiveEventPrivateBatchCapacity(0); }
TEST(ScheduledTransactionPool, PrivatePageBatchRealFiveRequestsIngressShortage) { CheckFiveEventPrivateBatchCapacity(1); }
TEST(ScheduledTransactionPool, PrivatePageBatchRealFiveRequestsFrameBatchShortage) { CheckFiveEventPrivateBatchCapacity(2); }
TEST(ScheduledTransactionPool, PrivatePageBatchRealFiveRequestsEventShortage) { CheckFiveEventPrivateBatchCapacity(3); }

TEST(ScheduledTransactionPool, PrivatePageBatchFrozenOriginalKeysRejectChangesCommittedAndReuse) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    std::array<ScheduledEventInput, 5> events{};
    std::array<TransactionRequest, 5> requests{};
    for (unsigned i = 0; i < 5; ++i) {
        events[i].executeFrame = 10;
        events[i].eventIndex = i;
        events[i].transactionOrdinal = i;
        requests[i].transportEpoch = kEpoch;
        requests[i].ownerInstanceHandle = h.owner;
        requests[i].ownerInstanceScheduleEpoch = h.ownerEpoch;
        requests[i].events = &events[i];
        requests[i].eventCount = 1;
    }
    PreparedTransactionBatch prepared;
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(requests.data(), 5, 0, &prepared));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(prepared, 61));
    std::array<ConsumedEvent, 5> originals{};
    for (unsigned i = 0; i < 5; ++i) ASSERT_TRUE(pool.CapturePreparedTransactionEvent(prepared, i, 0, &originals[i]));
    EXPECT_TRUE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 5));
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 62, kEpoch, originals.data(), 5));
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch + 1, originals.data(), 5));
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 4));
    for (unsigned field = 0; field < 17; ++field) {
        auto wrong = originals;
        auto& e = wrong[3];
        switch (field) {
        case 0: ++e.rowSequence; break;
        case 1: ++e.txnSlot; break;
        case 2: ++e.executeFrame; break;
        case 3: ++e.logicCommitSequence; break;
        case 4: ++e.transactionId; break;
        case 5: ++e.executionOrdinal; break;
        case 6: ++e.eventIndex; break;
        case 7: ++e.transactionOrdinal; break;
        case 8: ++e.subeventPhase; break;
        case 9: ++e.payload; break;
        case 10: e.authoredFadeOutCandidate = true; break;
        case 11: ++e.armToken.generation; break;
        case 12: ++e.armToken.renderGeneration; break;
        case 13: ++e.program.programGeneration; break;
        case 14: ++e.ownerInstanceHandle; break;
        case 15: ++e.ownerInstanceScheduleEpoch; break;
        case 16: e.frozenTxnValid = true; break;
        }
        EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, wrong.data(), 5)) << field;
        EXPECT_TRUE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 5));
    }
    auto reordered = originals;
    std::swap(reordered[0], reordered[1]);
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, reordered.data(), 5));
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 5));
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(requests.data(), 5, 0, &prepared));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(prepared, 61));
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 5));
    for (unsigned i = 0; i < 5; ++i) ASSERT_TRUE(pool.CapturePreparedTransactionEvent(prepared, i, 0, &originals[i]));
    ASSERT_TRUE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 5));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, 61, true));
    EXPECT_FALSE(pool.MatchesFrozenPreparedTransactionEvents(prepared, 61, kEpoch, originals.data(), 5));
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(0u, pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelRejectsStaleReceiptAcrossSlotAndResetReuse) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineEventPayload payload{};
    harken::TimelineProgramView program{71, &payload, 1};
    const harken::TimelineTransactionProgramRef programRef{71, &program};
    const harken::TimelineArmToken arm{2, 3, 4, 5};
    ScheduledEventInput event{};
    event.executeFrame = 10;
    event.armToken = arm;
    TransactionRequest request{};
    request.transactionId = 11;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.program = programRef;
    request.armToken = arm;
    request.events = &event;
    request.eventCount = 1;
    harken::OrdinaryStartTransactionIdentity expected{
        request.transactionId, h.owner, h.ownerEpoch, programRef, arm, 10};
    const auto first = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, first.status);
    ASSERT_TRUE(first.committed.Valid());
    auto wrongSerial = first.committed;
    ++wrongSerial.commitSerial;
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Stale,
              pool.CancelUnpublishedOrdinaryStartExact(wrongSerial, expected));
    auto wrongIdentity = expected;
    ++wrongIdentity.transactionId;
    EXPECT_EQ(harken::OrdinaryStartCancelResult::ProtocolViolation,
              pool.CancelUnpublishedOrdinaryStartExact(first.committed, wrongIdentity));
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    EXPECT_TRUE(pool.HasProgramReference(programRef));
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
              pool.CancelUnpublishedOrdinaryStartExact(first.committed, expected));
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Stale,
              pool.CancelUnpublishedOrdinaryStartExact(first.committed, expected));

    request.transactionId = ++expected.transactionId;
    const auto second = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, second.status);
    EXPECT_EQ(first.committed.slot, second.committed.slot);
    EXPECT_GT(second.committed.commitSerial, first.committed.commitSerial);
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Stale,
              pool.CancelUnpublishedOrdinaryStartExact(first.committed, expected));
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    request.transactionId = ++expected.transactionId;
    const auto third = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, third.status);
    EXPECT_GT(third.committed.commitSerial, second.committed.commitSerial);
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Stale,
              pool.CancelUnpublishedOrdinaryStartExact(second.committed, expected));
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    ASSERT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    request.transportEpoch = kEpoch + 1;
    request.transactionId = ++expected.transactionId;
    const auto fourth = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, fourth.status);
    EXPECT_GT(fourth.committed.commitSerial, third.committed.commitSerial);
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Stale,
              pool.CancelUnpublishedOrdinaryStartExact(third.committed, expected));
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
              pool.CancelUnpublishedOrdinaryStartExact(fourth.committed, expected));
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelKeepsPackedNeighborsAndRejectsPublishedPage) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::TimelineEventPayload payload{};
    harken::TimelineProgramView targetProgram{71, &payload, 1};
    harken::TimelineProgramView neighborProgram{72, &payload, 1};
    const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
    const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
    const harken::TimelineArmToken arm{2, 3, 4, 5};
    const harken::TimelineArmToken neighborArm{6, 1, 6, 1};
    ScheduledEventInput event{};
    event.executeFrame = 10;
    event.armToken = neighborArm;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.armToken = neighborArm;
    request.events = &event;
    request.eventCount = 1;
    request.transactionId = 21;
    request.program = neighborRef;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    request.transactionId = 22;
    request.program = targetRef;
    request.armToken = arm;
    event.armToken = arm;
    const auto target = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, target.status);
    const harken::OrdinaryStartTransactionIdentity expected{
        22, h.owner, h.ownerEpoch, targetRef, arm, 10};
    event.executeFrame = 20;
    request.transactionId = 23;
    request.program = neighborRef;
    request.armToken = neighborArm;
    event.armToken = neighborArm;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    event.executeFrame = 30;
    request.transactionId = 24;
    PreparedTransactionBatch prepared;
    ASSERT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
              pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
    EXPECT_FALSE(pool.HasProgramReference(targetRef));
    EXPECT_TRUE(pool.HasProgramReference(neighborRef));
    EXPECT_EQ(2u, pool.LiveTransactionCount());
    ASSERT_NE(nullptr, pool.Page(0));
    EXPECT_EQ(2u, pool.Page(0)->committedTransactionCount);
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
    pool.Publish(100, &h.instances, &h.occurrences);
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Stale,
              pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
    std::array<ConsumedEvent, 4> consumed{};
    ASSERT_EQ(2u, pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                                   consumed.data(), consumed.size()));
    EXPECT_EQ(21u, consumed[0].transactionId);
    EXPECT_EQ(23u, consumed[1].transactionId);

    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    event.executeFrame = 10;
    request.transactionId = 25;
    request.program = targetRef;
    request.armToken = arm;
    event.armToken = arm;
    const auto published = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, published.status);
    const harken::OrdinaryStartTransactionIdentity publishedIdentity{
        25, h.owner, h.ownerEpoch, targetRef, arm, 10};
    pool.Publish(100, &h.instances, &h.occurrences);
    EXPECT_EQ(harken::OrdinaryStartCancelResult::NotEligible,
              pool.CancelUnpublishedOrdinaryStartExact(
                  published.committed, publishedIdentity));
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    EXPECT_TRUE(pool.HasProgramReference(targetRef));
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(ScheduledTransactionPool, OrdinaryStartExactCancelPreservesUnpublishedFrontierNeighbors) {
    Harness h;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 2;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    std::array<harken::TimelineEventPayload, 4> payloads{};
    std::array<harken::TimelineProgramView, 4> programs{};
    std::array<harken::TimelineTransactionProgramRef, 4> refs{};
    std::array<harken::TimelineArmToken, 4> armTokens{};
    std::array<harken::CommittedTransactionRef, 4> committed{};
    std::array<PlayableInstanceHandle, 4> owners{};
    std::array<std::uint64_t, 4> ownerEpochs{};
    for (std::uint32_t index = 0; index < 4; ++index) {
        owners[index] = index == 0 ? h.owner :
            h.instances.Acquire(PlayableKind::Piece, 0);
        ASSERT_NE(0u, owners[index]);
        ownerEpochs[index] = h.instances.Classify(owners[index]).scheduleEpoch;
        payloads[index].kind = harken::TimelineEventKind::Start;
        programs[index] = {71u + index, &payloads[index], 1};
        programs[index].role = harken::TimelineProgramRole::Voice;
        refs[index] = {programs[index].generation, &programs[index]};
        armTokens[index] = {index + 2u, 1, index + 2u, 1};
        ScheduledEventInput event{};
        event.executeFrame = 20;
        event.armToken = armTokens[index];
        TransactionRequest request{};
        request.transactionId = 21 + index;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = owners[index];
        request.ownerInstanceScheduleEpoch = ownerEpochs[index];
        request.program = refs[index];
        request.armToken = armTokens[index];
        request.events = &event;
        request.eventCount = 1;
        const auto result = pool.Reserve(request, PrePublishLatePolicy::Refuse);
        ASSERT_EQ(ReserveStatus::Committed, result.status);
        committed[index] = result.committed;
    }
    for (std::uint32_t index = 0; index < 2; ++index) {
        harken::Plan03GroupSliceInput group{};
        group.executeFrame = 20;
        group.decision = {index + 1u, 1};
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(group, arms));
    }
    std::array<harken::Plan03PageGroupView, 2> groups{};
    ASSERT_EQ(groups.size(), pool.CaptureLivePlan03Groups(groups.data(), groups.size()));
    const harken::OrdinaryStartTransactionIdentity expected{
        21, owners[0], ownerEpochs[0], refs[0], armTokens[0], 20, 7, 1};
    EXPECT_EQ(harken::OrdinaryStartCancelResult::NotEligible,
              pool.CancelUnpublishedOrdinaryStartExact(committed[0], expected));
    EXPECT_EQ(4u, pool.LiveTransactionCount());
    for (std::uint32_t index = 0; index < groups.size(); ++index) {
        const std::uint32_t neighbor = index + 1;
        const harken::FrontierOwnerIdentity owner{
            owners[neighbor], ownerEpochs[neighbor], 0, 0,
            owners[neighbor], ownerEpochs[neighbor], index + 8u, 1};
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Coordinator;
        binding.entry = {groups[index].identity.decision, 1, owner};
        binding.primary = groups[index].identity;
        binding.operation.kind = harken::FrontierOperationKind::Entry;
        binding.operation.owner = owner;
        binding.operation.order.executeFrame = 20;
        binding.operation.order.logicCommitSequence = 1;
        binding.operation.group = groups[index].identity;
        binding.operation.transport = kEpoch;
        binding.operation.world = 1;
        binding.operation.transactionSlot = committed[neighbor].slot;
        binding.operation.program = refs[neighbor].program;
        binding.operation.programGeneration = refs[neighbor].programGeneration;
        binding.operation.armSlot = armTokens[neighbor].slot;
        binding.operation.armGeneration = armTokens[neighbor].generation;
        binding.operation.renderSlot = armTokens[neighbor].renderSlot;
        binding.operation.renderGeneration = armTokens[neighbor].renderGeneration;
        ASSERT_TRUE(binding.Valid());
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(groups[index].identity, binding));
    }
    std::array<harken::Plan03ExactGroupSnapshot, 2> before{};
    for (std::uint32_t index = 0; index < groups.size(); ++index)
        ASSERT_TRUE(pool.CaptureExactPlan03Group(groups[index].identity, &before[index]));
    std::uint32_t live = 0, prepared = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(20, &live, &prepared));
    ASSERT_EQ(4u, live);
    ASSERT_EQ(0u, prepared);
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
              pool.CancelUnpublishedOrdinaryStartExact(committed[0], expected));
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(20, &live, &prepared));
    EXPECT_EQ(3u, live);
    EXPECT_EQ(0u, prepared);
    EXPECT_EQ(3u, pool.Page(0)->committedTransactionCount);
    EXPECT_EQ(2u, pool.CaptureLivePlan03Groups(nullptr, 0));
    for (std::uint32_t index = 0; index < groups.size(); ++index) {
        harken::Plan03ExactGroupSnapshot after{};
        ASSERT_TRUE(pool.CaptureExactPlan03Group(groups[index].identity, &after));
        EXPECT_EQ(before[index].identity, after.identity);
        EXPECT_EQ(before[index].readerPins, after.readerPins);
        EXPECT_EQ(before[index].lastAudioSequence, after.lastAudioSequence);
        EXPECT_EQ(before[index].publicationHeld, after.publicationHeld);
        EXPECT_EQ(before[index].frontier.operation.owner, after.frontier.operation.owner);
        EXPECT_EQ(before[index].frontier.operation.group, after.frontier.operation.group);
    }
    std::array<ConsumedEvent, 4> captured{};
    ASSERT_EQ(3u, pool.CapturePageEventsForTest(0, captured.data(), captured.size()));
    std::array<harken::TransactionId, 3> remaining{};
    for (std::uint32_t index = 0; index < remaining.size(); ++index)
        remaining[index] = captured[index].transactionId;
    std::sort(remaining.begin(), remaining.end());
    EXPECT_EQ((std::array<harken::TransactionId, 3>{22, 23, 24}), remaining);
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelReclaimsOnlyItsEmptyBatch) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::TimelineEventPayload payload{};
    payload.kind = harken::TimelineEventKind::Start;
    harken::TimelineProgramView targetProgram{71, &payload, 1};
    targetProgram.role = harken::TimelineProgramRole::Voice;
    harken::TimelineProgramView neighborProgram{72, &payload, 1};
    const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
    const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
    const harken::TimelineArmToken arm{2, 3, 4, 5};
    const harken::TimelineArmToken neighborArm{6, 1, 6, 1};
    ScheduledEventInput event{};
    event.armToken = neighborArm;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.armToken = neighborArm;
    request.events = &event;
    request.eventCount = 1;
    request.transactionId = 20;
    request.program = neighborRef;
    event.executeFrame = 10;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    request.transactionId = 21;
    request.program = targetRef;
    request.armToken = arm;
    event.armToken = arm;
    event.executeFrame = 20;
    const auto target = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, target.status);
    request.transactionId = 22;
    request.program = neighborRef;
    request.armToken = neighborArm;
    event.armToken = neighborArm;
    event.executeFrame = 30;
    PreparedTransactionBatch prepared;
    ASSERT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
    std::uint32_t live = 0, preparedCount = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(20, &live, &preparedCount));
    EXPECT_EQ(1u, live);
    EXPECT_EQ(0u, preparedCount);
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(30, &live, &preparedCount));
    EXPECT_EQ(0u, live);
    EXPECT_EQ(1u, preparedCount);
    const harken::OrdinaryStartTransactionIdentity expected{
        21, h.owner, h.ownerEpoch, targetRef, arm, 20};
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
              pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
    EXPECT_FALSE(pool.CaptureFrameBatchReferencesForTest(20, &live, &preparedCount));
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(10, &live, &preparedCount));
    EXPECT_EQ(1u, live);
    EXPECT_EQ(0u, preparedCount);
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(30, &live, &preparedCount));
    EXPECT_EQ(0u, live);
    EXPECT_EQ(1u, preparedCount);
    EXPECT_EQ(1u, pool.Page(0)->committedTransactionCount);
    EXPECT_TRUE(pool.HasProgramReference(neighborRef));
    EXPECT_FALSE(pool.HasProgramReference(targetRef));
    EXPECT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelRejectsFrontierOwnerAndArmAliasesWithoutMutation) {
    for (const bool serviceAlias : {true, false}) {
        SCOPED_TRACE(serviceAlias);
        Harness h;
        const auto neighborOwner = h.instances.Acquire(PlayableKind::Piece, 0);
        ASSERT_NE(0u, neighborOwner);
        const auto neighborEpoch = h.instances.Classify(neighborOwner).scheduleEpoch;
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = 1;
        config.maxArmsPerGroup = 1;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms(1);
        harken::TimelineEventPayload payload{};
        payload.kind = harken::TimelineEventKind::Start;
        harken::TimelineProgramView targetProgram{71, &payload, 1};
        targetProgram.role = harken::TimelineProgramRole::Voice;
        harken::TimelineProgramView neighborProgram{72, &payload, 1};
        const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
        const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
        const harken::TimelineArmToken targetArm{2, 3, 4, 5};
        const harken::TimelineArmToken neighborArm = serviceAlias
            ? harken::TimelineArmToken{6, 1, 6, 1} : targetArm;
        ScheduledEventInput event{};
        event.executeFrame = 20;
        event.armToken = targetArm;
        TransactionRequest request{};
        request.transactionId = 21;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.program = targetRef;
        request.armToken = targetArm;
        request.events = &event;
        request.eventCount = 1;
        const auto target = pool.Reserve(request, PrePublishLatePolicy::Refuse);
        ASSERT_EQ(ReserveStatus::Committed, target.status);
        event.armToken = neighborArm;
        request.transactionId = 22;
        request.ownerInstanceHandle = neighborOwner;
        request.ownerInstanceScheduleEpoch = neighborEpoch;
        request.program = neighborRef;
        request.armToken = neighborArm;
        ASSERT_EQ(ReserveStatus::Committed,
                  pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
        harken::Plan03GroupSliceInput group{};
        group.executeFrame = 20;
        group.decision = {1, 1};
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(group, arms));
        harken::Plan03PageGroupView groupView{};
        ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&groupView, 1));
        const harken::FrontierOwnerIdentity owner{
            neighborOwner, neighborEpoch, 0, 0,
            neighborOwner, neighborEpoch, serviceAlias ? 7u : 8u, 1};
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Coordinator;
        binding.entry = {groupView.identity.decision, 1, owner};
        binding.primary = groupView.identity;
        binding.operation.kind = harken::FrontierOperationKind::Entry;
        binding.operation.owner = owner;
        binding.operation.order.executeFrame = 20;
        binding.operation.order.logicCommitSequence = 1;
        binding.operation.group = groupView.identity;
        binding.operation.transport = kEpoch;
        binding.operation.world = 1;
        binding.operation.transactionSlot = UINT32_MAX - 1u;
        binding.operation.program = neighborRef.program;
        binding.operation.programGeneration = neighborRef.programGeneration;
        binding.operation.armSlot = 9;
        binding.operation.armGeneration = 1;
        binding.operation.renderSlot = 9;
        binding.operation.renderGeneration = 1;
        ASSERT_TRUE(binding.Valid());
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(groupView.identity, binding));
        harken::Plan03ExactGroupSnapshot beforeGroup{};
        ASSERT_TRUE(pool.CaptureExactPlan03Group(groupView.identity, &beforeGroup));
        std::array<ConsumedEvent, 2> beforeEvents{};
        ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, beforeEvents.data(), beforeEvents.size()));
        const harken::OrdinaryStartTransactionIdentity expected{
            21, h.owner, h.ownerEpoch, targetRef, targetArm, 20, 7, 1};
        EXPECT_EQ(harken::OrdinaryStartCancelResult::NotEligible,
                  pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
        EXPECT_EQ(2u, pool.LiveTransactionCount());
        EXPECT_EQ(2u, pool.Page(0)->committedTransactionCount);
        std::array<ConsumedEvent, 2> afterEvents{};
        ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, afterEvents.data(), afterEvents.size()));
        for (std::uint32_t index = 0; index < beforeEvents.size(); ++index) {
            EXPECT_EQ(beforeEvents[index].txnSlot, afterEvents[index].txnSlot);
            EXPECT_EQ(beforeEvents[index].rowSequence, afterEvents[index].rowSequence);
            EXPECT_EQ(beforeEvents[index].armToken.slot, afterEvents[index].armToken.slot);
            EXPECT_EQ(beforeEvents[index].armToken.generation, afterEvents[index].armToken.generation);
        }
        harken::Plan03ExactGroupSnapshot afterGroup{};
        ASSERT_TRUE(pool.CaptureExactPlan03Group(groupView.identity, &afterGroup));
        EXPECT_EQ(beforeGroup.identity, afterGroup.identity);
        EXPECT_EQ(beforeGroup.readerPins, afterGroup.readerPins);
        EXPECT_EQ(beforeGroup.lastAudioSequence, afterGroup.lastAudioSequence);
        EXPECT_EQ(beforeGroup.frontier.operation.owner, afterGroup.frontier.operation.owner);
    }
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelRejectsCorruptPageBeforeAnyMutation) {
    using Access = harken::detail::ScheduledTransactionPoolCancelTestAccess;
    for (std::uint32_t fault = 0; fault < 6; ++fault) {
        SCOPED_TRACE(fault);
        Harness h;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        harken::TimelineEventPayload payload{};
        payload.kind = harken::TimelineEventKind::Start;
        harken::TimelineProgramView targetProgram{71, &payload, 1};
        targetProgram.role = harken::TimelineProgramRole::Voice;
        harken::TimelineProgramView neighborProgram{72, &payload, 1};
        const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
        const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
        const harken::TimelineArmToken arm{2, 3, 4, 5};
        const harken::TimelineArmToken neighborArm{6, 1, 6, 1};
        ScheduledEventInput event{};
        event.executeFrame = 20;
        event.armToken = arm;
        TransactionRequest request{};
        request.transactionId = 21;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.program = targetRef;
        request.armToken = arm;
        request.events = &event;
        request.eventCount = 1;
        const auto target = pool.Reserve(request, PrePublishLatePolicy::Refuse);
        ASSERT_EQ(ReserveStatus::Committed, target.status);
        request.transactionId = 22;
        request.program = neighborRef;
        request.armToken = neighborArm;
        event.armToken = neighborArm;
        event.executeFrame = 30;
        ASSERT_EQ(ReserveStatus::Committed,
                  pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
        const harken::OrdinaryStartTransactionIdentity expected{
            21, h.owner, h.ownerEpoch, targetRef, arm, 20};
        std::uint64_t prior64 = 0;
        std::uint32_t prior32 = 0;
        switch (fault) {
        case 0: prior64 = Access::SetStamp(pool, 20, false, 0); break;
        case 1: prior64 = Access::SetStamp(pool, 20, true, 0); break;
        case 2: prior32 = Access::SetCommittedCount(pool, 20, 3); break;
        case 3:
            prior32 = Access::SetBatchLive(pool, 30, 0);
            EXPECT_EQ(2u, Access::SetLiveBatchCount(pool, 30, 1));
            break;
        case 4: prior64 = Access::SetRowSequence(pool, 20, 0); break;
        case 5: targetProgram.payloadCount = 2; break;
        }
        std::array<ConsumedEvent, 2> before{};
        ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, before.data(), before.size()));
        const auto beforeCount = pool.Page(0)->committedTransactionCount;
        const auto beforeStamp = pool.PageStamp(0);
        std::uint32_t neighborLive = 0, neighborPrepared = 0;
        ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
            30, &neighborLive, &neighborPrepared));
        EXPECT_EQ(fault <= 1 ? harken::OrdinaryStartCancelResult::NotEligible
                             : harken::OrdinaryStartCancelResult::ProtocolViolation,
                  pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
        EXPECT_EQ(beforeCount, pool.Page(0)->committedTransactionCount);
        EXPECT_EQ(beforeStamp, pool.PageStamp(0));
        EXPECT_EQ(2u, pool.LiveTransactionCount());
        EXPECT_TRUE(pool.HasProgramReference(targetRef));
        std::uint32_t afterLive = 0, afterPrepared = 0;
        ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
            30, &afterLive, &afterPrepared));
        EXPECT_EQ(neighborLive, afterLive);
        EXPECT_EQ(neighborPrepared, afterPrepared);
        std::array<ConsumedEvent, 2> after{};
        ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, after.data(), after.size()));
        for (std::uint32_t index = 0; index < before.size(); ++index) {
            EXPECT_EQ(before[index].txnSlot, after[index].txnSlot);
            EXPECT_EQ(before[index].rowSequence, after[index].rowSequence);
            EXPECT_EQ(before[index].armToken.slot, after[index].armToken.slot);
        }
        switch (fault) {
        case 0: EXPECT_EQ(0u, Access::SetStamp(pool, 20, false, prior64)); break;
        case 1: EXPECT_EQ(0u, Access::SetStamp(pool, 20, true, prior64)); break;
        case 2: EXPECT_EQ(3u, Access::SetCommittedCount(pool, 20, prior32)); break;
        case 3:
            EXPECT_EQ(0u, Access::SetBatchLive(pool, 30, prior32));
            EXPECT_EQ(1u, Access::SetLiveBatchCount(pool, 30, 2));
            break;
        case 4: EXPECT_EQ(0u, Access::SetRowSequence(pool, 20, prior64)); break;
        case 5: targetProgram.payloadCount = 1; break;
        }
        EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
                  pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
    }
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelRejectsOrdinaryPageAliases) {
    for (std::uint32_t alias = 0; alias < 3; ++alias) {
        SCOPED_TRACE(alias);
        Harness h;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        harken::TimelineEventPayload payload{};
        payload.kind = harken::TimelineEventKind::Start;
        harken::TimelineProgramView targetProgram{71, &payload, 1};
        targetProgram.role = harken::TimelineProgramRole::Voice;
        harken::TimelineProgramView neighborProgram{72, &payload, 1};
        neighborProgram.role = harken::TimelineProgramRole::Voice;
        const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
        const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
        const harken::TimelineArmToken targetArm{2, 3, 4, 5};
        const harken::TimelineArmToken neighborArm = alias == 0
            ? targetArm : alias == 1
                ? harken::TimelineArmToken{6, 1, 6, 1}
                : harken::TimelineArmToken{2, 4, 4, 6};
        ScheduledEventInput event{};
        event.executeFrame = 20;
        event.armToken = targetArm;
        TransactionRequest request{};
        request.transactionId = 21;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.program = targetRef;
        request.armToken = targetArm;
        request.events = &event;
        request.eventCount = 1;
        const auto target = pool.Reserve(request, PrePublishLatePolicy::Refuse);
        ASSERT_EQ(ReserveStatus::Committed, target.status);
        event.executeFrame = 30;
        event.armToken = neighborArm;
        request.transactionId = 22;
        request.program = alias == 1 ? targetRef : neighborRef;
        request.armToken = neighborArm;
        ASSERT_EQ(ReserveStatus::Committed,
                  pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
        std::array<ConsumedEvent, 2> before{};
        ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, before.data(), before.size()));
        const auto priorCommitted = pool.Page(0)->committedTransactionCount;
        const auto priorStamp = pool.PageStamp(0);
        std::uint32_t targetLive = 0, targetPrepared = 0;
        ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
            20, &targetLive, &targetPrepared));
        std::uint32_t priorLive = 0, priorPrepared = 0;
        ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
            30, &priorLive, &priorPrepared));
        const harken::OrdinaryStartTransactionIdentity expected{
            21, h.owner, h.ownerEpoch, targetRef, targetArm, 20};
        EXPECT_EQ(alias == 2 ? harken::OrdinaryStartCancelResult::Removed
                             : harken::OrdinaryStartCancelResult::NotEligible,
                  pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
        std::uint32_t afterLive = 0, afterPrepared = 0;
        ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
            30, &afterLive, &afterPrepared));
        EXPECT_EQ(priorLive, afterLive);
        EXPECT_EQ(priorPrepared, afterPrepared);
        EXPECT_EQ(priorStamp, pool.PageStamp(0));
        std::uint32_t afterTargetLive = 0, afterTargetPrepared = 0;
        const bool targetBatchPresent = pool.CaptureFrameBatchReferencesForTest(
            20, &afterTargetLive, &afterTargetPrepared);
        if (alias == 2) {
            EXPECT_FALSE(targetBatchPresent);
            EXPECT_EQ(1u, pool.LiveTransactionCount());
            EXPECT_EQ(priorCommitted - 1u,
                      pool.Page(0)->committedTransactionCount);
            std::array<ConsumedEvent, 2> after{};
            ASSERT_EQ(1u, pool.CapturePageEventsForTest(0, after.data(), after.size()));
            EXPECT_EQ(22u, after[0].transactionId);
            EXPECT_EQ(neighborArm.slot, after[0].armToken.slot);
            EXPECT_EQ(neighborArm.generation, after[0].armToken.generation);
            EXPECT_EQ(neighborArm.renderSlot, after[0].armToken.renderSlot);
            EXPECT_EQ(neighborArm.renderGeneration,
                      after[0].armToken.renderGeneration);
            EXPECT_EQ(request.program.program, after[0].program.program);
            EXPECT_EQ(request.program.programGeneration,
                      after[0].program.programGeneration);
        } else {
            ASSERT_TRUE(targetBatchPresent);
            EXPECT_EQ(targetLive, afterTargetLive);
            EXPECT_EQ(targetPrepared, afterTargetPrepared);
            EXPECT_EQ(2u, pool.LiveTransactionCount());
            EXPECT_EQ(priorCommitted, pool.Page(0)->committedTransactionCount);
            EXPECT_TRUE(pool.HasProgramReference(targetRef));
            std::array<ConsumedEvent, 2> after{};
            ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, after.data(), after.size()));
            for (std::uint32_t index = 0; index < before.size(); ++index) {
                EXPECT_EQ(before[index].transactionId, after[index].transactionId);
                EXPECT_EQ(before[index].txnSlot, after[index].txnSlot);
                EXPECT_EQ(before[index].executeFrame, after[index].executeFrame);
                EXPECT_EQ(before[index].payload, after[index].payload);
                EXPECT_EQ(before[index].rowSequence, after[index].rowSequence);
                EXPECT_EQ(before[index].program.program, after[index].program.program);
                EXPECT_EQ(before[index].program.programGeneration,
                          after[index].program.programGeneration);
                EXPECT_EQ(before[index].armToken.slot, after[index].armToken.slot);
                EXPECT_EQ(before[index].armToken.generation,
                          after[index].armToken.generation);
                EXPECT_EQ(before[index].armToken.renderSlot,
                          after[index].armToken.renderSlot);
                EXPECT_EQ(before[index].armToken.renderGeneration,
                          after[index].armToken.renderGeneration);
            }
        }
    }
}

TEST(ScheduledTransactionPool, OrdinaryStartExactCancelRejectsOtherFrameBatchMismatch) {
    using Access = harken::detail::ScheduledTransactionPoolCancelTestAccess;
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::TimelineEventPayload payload{};
    payload.kind = harken::TimelineEventKind::Start;
    harken::TimelineProgramView targetProgram{71, &payload, 1};
    targetProgram.role = harken::TimelineProgramRole::Voice;
    harken::TimelineProgramView neighborProgram{72, &payload, 1};
    const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
    const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
    const harken::TimelineArmToken targetArm{2, 3, 4, 5};
    const harken::TimelineArmToken neighborArm{6, 1, 6, 1};
    ScheduledEventInput event{};
    event.executeFrame = 20;
    event.armToken = targetArm;
    TransactionRequest request{};
    request.transactionId = 21;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.program = targetRef;
    request.armToken = targetArm;
    request.events = &event;
    request.eventCount = 1;
    const auto target = pool.Reserve(request, PrePublishLatePolicy::Refuse);
    ASSERT_EQ(ReserveStatus::Committed, target.status);
    event.executeFrame = 30;
    event.armToken = neighborArm;
    request.transactionId = 22;
    request.program = neighborRef;
    request.armToken = neighborArm;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    std::array<ConsumedEvent, 2> before{};
    ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, before.data(), before.size()));
    const auto priorCommitted = pool.Page(0)->committedTransactionCount;
    const auto priorStamp = pool.PageStamp(0);
    std::uint32_t targetLive = 0, targetPrepared = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
        20, &targetLive, &targetPrepared));
    EXPECT_EQ(1u, Access::SetBatchLive(pool, 30, 2));
    std::uint32_t priorLive = 0, priorPrepared = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
        30, &priorLive, &priorPrepared));
    ASSERT_EQ(2u, priorLive);
    const harken::OrdinaryStartTransactionIdentity expected{
        21, h.owner, h.ownerEpoch, targetRef, targetArm, 20};
    EXPECT_EQ(harken::OrdinaryStartCancelResult::ProtocolViolation,
              pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
    EXPECT_EQ(2u, pool.LiveTransactionCount());
    EXPECT_EQ(priorCommitted, pool.Page(0)->committedTransactionCount);
    EXPECT_EQ(priorStamp, pool.PageStamp(0));
    EXPECT_TRUE(pool.HasProgramReference(targetRef));
    EXPECT_TRUE(pool.HasProgramReference(neighborRef));
    std::uint32_t afterTargetLive = 0, afterTargetPrepared = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
        20, &afterTargetLive, &afterTargetPrepared));
    EXPECT_EQ(targetLive, afterTargetLive);
    EXPECT_EQ(targetPrepared, afterTargetPrepared);
    std::uint32_t afterLive = 0, afterPrepared = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(
        30, &afterLive, &afterPrepared));
    EXPECT_EQ(priorLive, afterLive);
    EXPECT_EQ(priorPrepared, afterPrepared);
    std::array<ConsumedEvent, 2> after{};
    ASSERT_EQ(2u, pool.CapturePageEventsForTest(0, after.data(), after.size()));
    for (std::uint32_t index = 0; index < before.size(); ++index) {
        EXPECT_EQ(before[index].transactionId, after[index].transactionId);
        EXPECT_EQ(before[index].txnSlot, after[index].txnSlot);
        EXPECT_EQ(before[index].executeFrame, after[index].executeFrame);
        EXPECT_EQ(before[index].payload, after[index].payload);
        EXPECT_EQ(before[index].rowSequence, after[index].rowSequence);
        EXPECT_EQ(before[index].program.program, after[index].program.program);
        EXPECT_EQ(before[index].program.programGeneration,
                  after[index].program.programGeneration);
        EXPECT_EQ(before[index].armToken.slot, after[index].armToken.slot);
        EXPECT_EQ(before[index].armToken.generation,
                  after[index].armToken.generation);
    }
    EXPECT_EQ(2u, Access::SetBatchLive(pool, 30, 1));
    EXPECT_EQ(harken::OrdinaryStartCancelResult::Removed,
              pool.CancelUnpublishedOrdinaryStartExact(target.committed, expected));
}

#endif

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(ScheduledTransactionPool,
     UnpublishedFrontierEntryCancelPreservesTransactionUsingReusedArmSlot) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::TimelineArmTable arms(1);
    harken::TimelineArmToken oldArm{};
    ASSERT_TRUE(arms.Reserve(4, 1, 20, 20, 71, {}, &oldArm));

    const auto* pageHeader = pool.Page(0);
    ASSERT_NE(nullptr, pageHeader);
    harken::Plan03GroupSliceIdentity primary{};
    primary.absoluteQuantum = 0;
    primary.pageGeneration = pageHeader->pageGeneration;
    primary.decision = {0, 1};
    primary.committedRow = 0;
    primary.rowClaimGeneration = 1;
    primary.transportEpoch = kEpoch;
    auto member = primary;
    member.decision = {1, 1};
    member.committedRow = 1;
    ASSERT_TRUE(primary.Exact());
    ASSERT_TRUE(member.Exact());
    const harken::FrontierOwnerIdentity coordinatorOwner{
        h.owner, h.ownerEpoch, 0, 0, h.owner, h.ownerEpoch, 0, 1};
    auto memberOwner = coordinatorOwner;
    memberOwner.serviceSlot = 1;
    const harken::FrontierEntryReceiptRef entry{{0, 1}, 1,
                                               coordinatorOwner};
    ASSERT_TRUE(entry.Valid());

    std::array<harken::TimelineEventPayload, 4> payloads{};
    std::array<harken::TimelineProgramView, 4> programs{};
    std::array<harken::TimelineTransactionProgramRef, 4> refs{};
    std::array<ScheduledEventInput, 3> oldEvents{};
    std::array<TransactionRequest, 3> oldRequests{};
    for (std::uint32_t index = 0; index < 4; ++index) {
        payloads[index].kind = index < 2
            ? harken::TimelineEventKind::ControllerActivation
            : harken::TimelineEventKind::Start;
        programs[index].generation = 71 + index;
        programs[index].payloads = &payloads[index];
        programs[index].payloadCount = 1;
        programs[index].role = index < 2
            ? harken::TimelineProgramRole::ControllerActivation
            : harken::TimelineProgramRole::Voice;
        refs[index] = {programs[index].generation, &programs[index]};
    }
    for (std::uint32_t index = 0; index < oldEvents.size(); ++index) {
        auto& event = oldEvents[index];
        event.executeFrame = 20;
        event.logicCommitSequence = 11;
        event.armToken = index == 2 ? oldArm : harken::TimelineArmToken{};
        event.frontier.kind = harken::FrontierOperationKind::Entry;
        event.frontier.entry = entry;
        event.frontier.owner = index == 0
            ? coordinatorOwner : memberOwner;
        event.frontier.group = index == 0 ? primary : member;
        event.frontier.transport = kEpoch;
        event.frontier.world = 1;
        event.frontier.operationEventCount = 1;
        event.frontier.entryEventOrdinal = index;
        ASSERT_TRUE(event.frontier.Valid());
        auto& request = oldRequests[index];
        request.transactionId = 11;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.program = refs[index];
        request.armToken = event.armToken;
        request.events = &event;
        request.eventCount = 1;
    }
    PreparedTransactionBatch oldBatch;
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(
        oldRequests.data(), oldRequests.size(), 0, &oldBatch));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&oldBatch, 11));

    ASSERT_TRUE(arms.AcknowledgeApplied(oldArm));
    ASSERT_TRUE(arms.Publish(oldArm));
    ASSERT_TRUE(arms.Start(oldArm));
    ASSERT_TRUE(arms.Tombstone(oldArm, 0));
    ASSERT_TRUE(arms.Reclaim(oldArm, 1));
    harken::TimelineArmToken newArm{};
    ASSERT_TRUE(arms.Reserve(5, 1, 20, 20, 74, {}, &newArm));
    ASSERT_EQ(oldArm.slot, newArm.slot);
    ASSERT_GT(newArm.generation, oldArm.generation);
    std::cerr << "reused arm old slot=" << oldArm.slot
              << " generation=" << oldArm.generation
              << " render=" << oldArm.renderSlot << ':'
              << oldArm.renderGeneration << " new slot=" << newArm.slot
              << " generation=" << newArm.generation
              << " render=" << newArm.renderSlot << ':'
              << newArm.renderGeneration << '\n';
    const auto neighborOwner = h.instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(0u, neighborOwner);
    const auto neighborEpoch =
        h.instances.Classify(neighborOwner).scheduleEpoch;
    ScheduledEventInput neighborEvent{};
    neighborEvent.executeFrame = 20;
    neighborEvent.logicCommitSequence = 12;
    neighborEvent.armToken = newArm;
    TransactionRequest neighbor{};
    neighbor.transactionId = 12;
    neighbor.transportEpoch = kEpoch;
    neighbor.ownerInstanceHandle = neighborOwner;
    neighbor.ownerInstanceScheduleEpoch = neighborEpoch;
    neighbor.program = refs[3];
    neighbor.armToken = newArm;
    neighbor.events = &neighborEvent;
    neighbor.eventCount = 1;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(neighbor, PrePublishLatePolicy::Refuse).status);
    std::array<ConsumedEvent, 4> before{};
    ASSERT_EQ(4u, pool.CapturePageEventsForTest(
        0, before.data(), before.size()));
    const auto neighborBefore = before[3];
    ASSERT_EQ(refs[3].program, neighborBefore.program.program);
    std::cerr << "reused arm neighbor before txn="
              << neighborBefore.txnSlot
              << " row=" << neighborBefore.rowSequence
              << " arm=" << neighborBefore.armToken.slot << ':'
              << neighborBefore.armToken.generation << '\n';
    std::uint32_t live = 0, prepared = 0;
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(20, &live, &prepared));
    ASSERT_EQ(4u, live);
    ASSERT_EQ(0u, prepared);
    harken::ArmState newStateBefore{};
    ASSERT_TRUE(arms.State(newArm, &newStateBefore));

    harken::UnpublishedFrontierEntryExpected expected{};
    expected.descriptor.entry = entry;
    expected.descriptor.primary = primary;
    expected.descriptor.order.executeFrame = 20;
    expected.descriptor.order.logicCommitSequence = 11;
    expected.descriptor.order.rowSequence = before[0].rowSequence;
    expected.descriptor.worldGeneration = 1;
    expected.descriptor.worldSemanticFingerprint = 1;
    expected.descriptor.expectedMemberCount = 1;
    expected.descriptor.expectedEntryEventCount = 3;
    expected.descriptor.programGeneration = 71;
    expected.member = member;
    expected.executeFrame = 20;
    for (std::uint32_t index = 0; index < expected.events.size(); ++index) {
        expected.events[index] = {refs[index], h.owner, h.ownerEpoch,
            h.owner, h.ownerEpoch, index == 0 ? primary : member,
            oldEvents[index].armToken, index == 0 ? 0u : 1u, 1};
    }
    ASSERT_TRUE(expected.descriptor.Valid());
    ASSERT_NE(0u, h.instances.BumpScheduleEpoch(h.owner));
    EXPECT_EQ(harken::CancelledFrontierStartResult::Removed,
        pool.CancelUnpublishedFrontierEntryExact(expected, h.instances));
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    EXPECT_EQ(1u, pool.Page(0)->committedTransactionCount);
    ASSERT_TRUE(pool.CaptureFrameBatchReferencesForTest(20, &live, &prepared));
    EXPECT_EQ(1u, live);
    EXPECT_EQ(0u, prepared);
    std::array<ConsumedEvent, 4> after{};
    ASSERT_EQ(1u, pool.CapturePageEventsForTest(
        0, after.data(), after.size()));
    EXPECT_EQ(neighborBefore.txnSlot, after[0].txnSlot);
    EXPECT_EQ(neighborBefore.rowSequence, after[0].rowSequence);
    EXPECT_EQ(neighborBefore.program.program, after[0].program.program);
    EXPECT_EQ(neighborBefore.armToken.slot, after[0].armToken.slot);
    EXPECT_EQ(neighborBefore.armToken.generation,
              after[0].armToken.generation);
    for (std::uint32_t index = 0; index < 3; ++index)
        EXPECT_FALSE(pool.HasProgramReference(refs[index]));
    EXPECT_TRUE(pool.HasProgramReference(refs[3]));
    harken::ArmState newStateAfter{};
    ASSERT_TRUE(arms.State(newArm, &newStateAfter));
    EXPECT_EQ(newStateBefore, newStateAfter);
    std::cerr << "reused arm after live=" << pool.LiveTransactionCount()
              << " batch=" << live << " neighbor txn=" << after[0].txnSlot
              << " row=" << after[0].rowSequence
              << " armState=" << static_cast<unsigned>(newStateBefore)
              << "->" << static_cast<unsigned>(newStateAfter) << '\n';
}
#endif

TEST(ScheduledTransactionPool,
     CancelledFrontierStartRejectsWrongRowSequenceWithoutTouchingSharedBatch) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    const auto* page = pool.Page(0);
    ASSERT_NE(nullptr, page);
    harken::Plan03GroupSliceIdentity member{};
    member.absoluteQuantum = 0;
    member.pageGeneration = page->pageGeneration;
    member.decision = {0, 1};
    member.committedRow = 1;
    member.rowClaimGeneration = 1;
    member.transportEpoch = kEpoch;
    ASSERT_TRUE(member.Exact());
    const harken::FrontierEntryReceiptRef entry{
        {0, 1}, 1,
        {h.owner, h.ownerEpoch, 0, 0, h.owner, h.ownerEpoch, 0, 1}};
    ASSERT_TRUE(entry.Valid());

    harken::TimelineEventPayload targetPayload{};
    harken::TimelineEventPayload neighborPayload{};
    harken::TimelineProgramView targetProgram{71, &targetPayload, 1};
    harken::TimelineProgramView neighborProgram{72, &neighborPayload, 1};
    const harken::TimelineTransactionProgramRef targetRef{71, &targetProgram};
    const harken::TimelineTransactionProgramRef neighborRef{72, &neighborProgram};
    const harken::TimelineArmToken arm{2, 3, 4, 5};
    ScheduledEventInput event{};
    event.executeFrame = 20;
    event.logicCommitSequence = 11;
    event.subeventPhase = 20;
    event.armToken = arm;
    event.frontier.kind = harken::FrontierOperationKind::Entry;
    event.frontier.entry = entry;
    event.frontier.owner = entry.owner;
    event.frontier.group = member;
    event.frontier.transport = kEpoch;
    event.frontier.world = 1;
    event.frontier.operationEventCount = 1;
    event.frontier.entryEventOrdinal = 0;
    ASSERT_TRUE(event.frontier.Valid());
    TransactionRequest target{};
    target.transactionId = 11;
    target.transportEpoch = kEpoch;
    target.ownerInstanceHandle = h.owner;
    target.ownerInstanceScheduleEpoch = h.ownerEpoch;
    target.program = targetRef;
    target.armToken = arm;
    target.events = &event;
    target.eventCount = 1;
    ASSERT_EQ(ReserveStatus::Committed,
        pool.Reserve(target, PrePublishLatePolicy::Refuse).status);

    ScheduledEventInput neighborEvent{};
    neighborEvent.executeFrame = event.executeFrame;
    neighborEvent.logicCommitSequence = 12;
    neighborEvent.subeventPhase = 20;
    neighborEvent.armToken = arm;
    TransactionRequest neighbor = target;
    neighbor.transactionId = 12;
    neighbor.program = neighborRef;
    neighbor.events = &neighborEvent;
    neighbor.ownerInstanceHandle = h.instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(0u, neighbor.ownerInstanceHandle);
    neighbor.ownerInstanceScheduleEpoch =
        h.instances.Classify(neighbor.ownerInstanceHandle).scheduleEpoch;
    ASSERT_EQ(ReserveStatus::Committed,
        pool.Reserve(neighbor, PrePublishLatePolicy::Refuse).status);
    std::uint64_t rowSequence = 0;
    ASSERT_TRUE(pool.CaptureUnpublishedFrontierStartRowSequence(
        target, entry, member, &rowSequence));
    ASSERT_NE(0u, rowSequence);
    ASSERT_NE(0u, h.instances.BumpScheduleEpoch(h.owner));
    const std::uint32_t beforeTransactions = pool.LiveTransactionCount();
    const std::uint32_t beforeCommitted = pool.Page(0)->committedTransactionCount;
    EXPECT_EQ(harken::CancelledFrontierStartResult::ProtocolViolation,
        pool.CancelUnpublishedFrontierStartExact(&targetRef, 1, target,
            entry, member, rowSequence + 1u, h.instances));
    EXPECT_EQ(beforeTransactions, pool.LiveTransactionCount());
    EXPECT_EQ(beforeCommitted, pool.Page(0)->committedTransactionCount);
    EXPECT_TRUE(pool.HasProgramReference(targetRef));
    EXPECT_TRUE(pool.HasProgramReference(neighborRef));
    std::uint64_t afterWrongSequence = 0;
    EXPECT_TRUE(pool.CaptureUnpublishedFrontierStartRowSequence(
        target, entry, member, &afterWrongSequence));
    EXPECT_EQ(rowSequence, afterWrongSequence);

    EXPECT_EQ(harken::CancelledFrontierStartResult::Removed,
        pool.CancelUnpublishedFrontierStartExact(&targetRef, 1, target,
            entry, member, rowSequence, h.instances));
    EXPECT_EQ(beforeTransactions - 1u, pool.LiveTransactionCount());
    EXPECT_EQ(beforeCommitted - 1u, pool.Page(0)->committedTransactionCount);
    EXPECT_FALSE(pool.HasProgramReference(targetRef));
    EXPECT_TRUE(pool.HasProgramReference(neighborRef));
    EXPECT_EQ(harken::CancelledFrontierStartResult::Stale,
        pool.CancelUnpublishedFrontierStartExact(&targetRef, 1, target,
            entry, member, rowSequence, h.instances));
    pool.Publish(100, &h.instances, &h.occurrences);
    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(1u, pool.ConsumePageRt(0, consumed.data(), consumed.size()));
    EXPECT_EQ(neighbor.transactionId, consumed[0].transactionId);
}

TEST(ScheduledTransactionPool, UnplacedCreditChargesSharedPoolAndReleasesOnCancel) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 3;
    config.maxEventsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));

    harken::UnplacedBatchCredit credit{};
    const harken::UnplacedBatchCreditRequest demand{1, 1, 1, 1, 0};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(demand, &credit));
    EXPECT_FALSE(pool.ReserveUnplacedBatchCredit(demand, &credit));
    ScheduledEventInput event{};
    event.executeFrame = 10;
    TransactionRequest request{};
    request.transactionId = 1;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
    EXPECT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    event.executeFrame = 110;
    EXPECT_EQ(ReserveRefusal::PoolFull, pool.PreflightTransaction(request, 0));

    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_FALSE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
    PreparedTransactionBatch prepared;
    EXPECT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
    EXPECT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
}

TEST(ScheduledTransactionPool, UnplacedCreditPersistsAcrossAdvanceAndOldHandleCannotCancel) {
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    config.maxIngressTransactionsPerQuantum = 1;
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::UnplacedBatchCredit credit{};
    const harken::UnplacedBatchCreditRequest demand{1, 1, 1, 1, 4};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(demand, &credit));
    auto old = credit;
    pool.Advance();
    EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
              pool.PreflightPlan03GroupSlices(200, 1));
    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreflightPlan03GroupSlices(200, 1));
    EXPECT_FALSE(pool.CancelUnplacedBatchCredit(&old));
    ASSERT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    EXPECT_FALSE(pool.CancelUnplacedBatchCredit(&old));
}

TEST(ScheduledTransactionPool, UnplacedEventAndBatchBudgetsRespectSharedFrames) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 4;
    config.maxEventsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::UnplacedBatchCredit credit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({1, 1, 2, 2, 0}, &credit));

    ScheduledEventInput event{};
    event.executeFrame = 10;
    TransactionRequest request{};
    request.transactionId = 1;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    event.executeFrame = 110;
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    event.executeFrame = 210;
    EXPECT_EQ(ReserveRefusal::PageEventCapacity,
              pool.PreflightTransaction(request, 0));
    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
}

TEST(ScheduledTransactionPool, UnplacedIngressChargeExpiresButCapacityDoesNot) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::UnplacedBatchCredit credit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({1, 1, 1, 1, 0}, &credit));

    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transactionId = 1;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    EXPECT_EQ(ReserveRefusal::IngressRateExceeded,
              pool.PreflightTransaction(request, 0));
    pool.Advance();
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_EQ(ReserveRefusal::IngressRateExceeded,
              pool.PreflightTransaction(request, 0));
}

TEST(ScheduledTransactionPool, UnplacedBatchBudgetCountsSharedFrameOnce) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 4;
    config.maxEventsPerQuantum = 2;
    config.maxFrameBatchesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::UnplacedBatchCredit credit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({1, 1, 3, 3, 0}, &credit));

    ScheduledEventInput event{};
    event.executeFrame = 10;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    request.transactionId = 1;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    request.transactionId = 2;
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
    ASSERT_EQ(ReserveStatus::Committed,
              pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    event.executeFrame = 110;
    request.transactionId = 3;
    EXPECT_EQ(ReserveRefusal::PageBatchCapacity,
              pool.PreflightTransaction(request, 0));
    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
}

TEST(ScheduledTransactionPool, UnplacedHolderPreflightsItsOwnPageBudgetWithoutSpendingCredit) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    config.maxIngressTransactionsPerQuantum = 1;
    config.maxEventsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    harken::UnplacedBatchCredit credit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({1, 1, 1, 1, 1}, &credit));

    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transactionId = 13;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    harken::Plan03GroupSliceInput group{};
    group.executeFrame = 210;
    group.decision = {1, 1};
    EXPECT_EQ(ReserveRefusal::IngressRateExceeded,
              pool.PreflightTransactionBatch(&request, 1, 0));
    EXPECT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreflightPlan03GroupSlices(210, 1));
    ReserveRefusal transactionRefusal = ReserveRefusal::EmptyTransaction;
    harken::Plan03GroupReserveStatus groupStatus =
        harken::Plan03GroupReserveStatus::Invalid;
    ASSERT_TRUE(pool.PreflightUnplacedBatchMaterialization(
        credit, &request, 1, &group, 1, arms, 0,
        &transactionRefusal, &groupStatus));
    EXPECT_EQ(ReserveRefusal::None, transactionRefusal);
    EXPECT_EQ(harken::Plan03GroupReserveStatus::Committed, groupStatus);
    EXPECT_EQ(ReserveRefusal::IngressRateExceeded,
              pool.PreflightTransactionBatch(&request, 1, 0));
    std::uint32_t page = 0;
    ASSERT_TRUE(pool.PageForFrame(210, &page));
    ASSERT_NE(nullptr, pool.Page(page));
    EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
    EXPECT_EQ(0u, pool.Page(page)->plan03GroupCount);
    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    EXPECT_EQ(ReserveRefusal::None,
              pool.PreflightTransactionBatch(&request, 1, 0));
}

TEST(ScheduledTransactionPool, UnplacedHolderMaterializesAndRollsBackItsExactPageClaims) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    config.maxIngressTransactionsPerQuantum = 1;
    config.maxEventsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    harken::UnplacedBatchCredit credit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({1, 1, 1, 1, 1}, &credit));
    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transactionId = 13;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    harken::Plan03GroupSliceInput group{};
    group.executeFrame = 210;
    group.decision = {1, 1};
    ReserveRefusal transactionRefusal = ReserveRefusal::None;
    harken::Plan03GroupReserveStatus groupStatus =
        harken::Plan03GroupReserveStatus::Committed;
    std::uint32_t page = 0;
    ASSERT_TRUE(pool.PageForFrame(210, &page));

    {
        ScheduledTransactionPool::PrivateMaterializationTransaction lease;
        ASSERT_TRUE(pool.TryMaterializeUnplacedBatch(
            &credit, &request, 1, &group, 1, arms, 0,
            &lease, &transactionRefusal, &groupStatus));
        ASSERT_TRUE(lease.Transactions()->Valid());
        ASSERT_NE(nullptr, lease.Group(0));
        EXPECT_TRUE(lease.Group(0)->Valid());
        EXPECT_FALSE(pool.CancelUnplacedBatchCredit(&credit));
        EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
        EXPECT_EQ(0u, pool.Page(page)->plan03GroupCount);
    }
    EXPECT_TRUE(credit.Valid());
    EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
    EXPECT_EQ(0u, pool.Page(page)->plan03GroupCount);
    EXPECT_TRUE(pool.Valid());

    {
        ScheduledTransactionPool::PrivateMaterializationTransaction lease;
        ASSERT_TRUE(pool.TryMaterializeUnplacedBatch(
            &credit, &request, 1, &group, 1, arms, 0,
            &lease, &transactionRefusal, &groupStatus));
        ASSERT_TRUE(lease.Commit(13));
        EXPECT_EQ(1u, pool.Page(page)->committedTransactionCount);
        EXPECT_EQ(1u, pool.CaptureLivePlan03Groups(nullptr, 0));
        ASSERT_NE(nullptr, lease.CommittedGroup(0));
        EXPECT_TRUE(lease.CommittedGroup(0)->Valid());
        ASSERT_TRUE(lease.ReleasePublication());
    }
    EXPECT_FALSE(credit.Valid());
    pool.Publish(300, &h.instances, &h.occurrences, &arms);
    std::array<ConsumedEvent, 1> consumed{};
    ASSERT_EQ(1u, pool.ConsumePageRt(2, consumed.data(), consumed.size()));
    EXPECT_EQ(13u, consumed[0].transactionId);
}

TEST(ScheduledTransactionPool, PrivateCommitRejectsLostGroupBeforeTransactionPublication) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    config.maxIngressTransactionsPerQuantum = 1;
    config.maxEventsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    harken::UnplacedBatchCredit credit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({1, 1, 1, 1, 1}, &credit));
    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transactionId = 13;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    harken::Plan03GroupSliceInput group{};
    group.executeFrame = 210;
    group.decision = {1, 1};
    ReserveRefusal refusal = ReserveRefusal::None;
    harken::Plan03GroupReserveStatus groupStatus =
        harken::Plan03GroupReserveStatus::Committed;
    std::uint32_t page = 0;
    ASSERT_TRUE(pool.PageForFrame(210, &page));
    {
        ScheduledTransactionPool::PrivateMaterializationTransaction lease;
        ASSERT_TRUE(pool.TryMaterializeUnplacedBatch(
            &credit, &request, 1, &group, 1, arms, 0,
            &lease, &refusal, &groupStatus));
        ASSERT_NE(nullptr, lease.Group(0));
        ASSERT_TRUE(pool.CancelPreparedPlan03GroupSlice(lease.Group(0)));
        EXPECT_FALSE(lease.Commit(13));
        EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
        EXPECT_EQ(0u, pool.Page(page)->plan03GroupCount);
    }
    EXPECT_TRUE(pool.Valid());
    EXPECT_TRUE(credit.Valid());
    EXPECT_EQ(0u, pool.CaptureLivePlan03Groups(nullptr, 0));
    EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
    EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
}

TEST(ScheduledTransactionPool, CohortMaterializationPreflightsSharedPageAndRollsBackAllCredits) {
    for (const bool enoughGroupHeaders : {false, true}) {
        SCOPED_TRACE(enoughGroupHeaders);
        Harness h;
        auto config = MakeConfig();
        config.maxScheduledTransactions = 2;
        config.maxIngressTransactionsPerQuantum = 2;
        config.maxEventsPerQuantum = 2;
        config.maxFrameBatchesPerQuantum = 2;
        config.maxPlan03GroupHeadersPerPage = enoughGroupHeaders ? 2 : 1;
        config.maxArmsPerGroup = 1;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms(1);
        std::array<harken::UnplacedBatchCredit, 2> credits{};
        std::array<ScheduledEventInput, 2> events{};
        std::array<TransactionRequest, 2> requests{};
        std::array<harken::Plan03GroupSliceInput, 2> groups{};
        std::array<harken::UnplacedBatchCohortMember, 2> members{};
        for (std::size_t index = 0; index < members.size(); ++index) {
            ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(
                {1, 1, 1, 1, 1}, &credits[index]));
            events[index].executeFrame = 210;
            requests[index].transactionId = 13u + index;
            requests[index].transportEpoch = kEpoch;
            requests[index].ownerInstanceHandle = h.owner;
            requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
            requests[index].events = &events[index];
            requests[index].eventCount = 1;
            groups[index].executeFrame = 210;
            groups[index].decision =
                {static_cast<std::uint32_t>(index + 1u), 1};
            members[index] = {&credits[index], &requests[index], 1,
                              &groups[index], 1};
        }
        std::uint32_t page = 0;
        ASSERT_TRUE(pool.PageForFrame(210, &page));
        ReserveRefusal refusal = ReserveRefusal::None;
        harken::Plan03GroupReserveStatus groupStatus =
            harken::Plan03GroupReserveStatus::Committed;
        if (enoughGroupHeaders)
            pool.SetPrivateMaterializationFaultStepForTest(3);
        {
            ScheduledTransactionPool::PrivateMaterializationTransaction lease;
            EXPECT_FALSE(pool.TryMaterializeUnplacedCohort(
                members.data(), members.size(), arms, 0,
                &lease, &refusal, &groupStatus));
            EXPECT_FALSE(lease.Valid());
        }
        EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
                  groupStatus);
        EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
        EXPECT_EQ(0u, pool.Page(page)->plan03GroupCount);
        EXPECT_EQ(0u, pool.CaptureLivePlan03Groups(nullptr, 0));
        EXPECT_TRUE(pool.Valid());
        EXPECT_TRUE(credits[0].Valid());
        EXPECT_TRUE(credits[1].Valid());
        if (enoughGroupHeaders) {
            ScheduledTransactionPool::PrivateMaterializationTransaction lease;
            ASSERT_TRUE(pool.TryMaterializeUnplacedCohort(
                members.data(), members.size(), arms, 0,
                &lease, &refusal, &groupStatus));
            EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
            EXPECT_EQ(0u, pool.Page(page)->plan03GroupCount);
            ASSERT_TRUE(lease.Commit(21));
            EXPECT_EQ(2u, pool.Page(page)->committedTransactionCount);
            EXPECT_EQ(2u, pool.CaptureLivePlan03Groups(nullptr, 0));
            ASSERT_TRUE(lease.ReleasePublication());
            EXPECT_FALSE(credits[0].Valid());
            EXPECT_FALSE(credits[1].Valid());
            pool.Publish(300, &h.instances, &h.occurrences, &arms);
            std::array<ConsumedEvent, 2> consumed{};
            ASSERT_EQ(2u, pool.ConsumePageRt(2, consumed.data(),
                                            consumed.size()));
            EXPECT_EQ(21u, consumed[0].transactionId);
            EXPECT_EQ(21u, consumed[1].transactionId);
        } else {
            EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credits[0]));
            EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credits[1]));
        }
    }
}

TEST(ScheduledTransactionPool, CohortMaterializationIncludesGroupOnlyHolder) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    config.maxIngressTransactionsPerQuantum = 1;
    config.maxEventsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    config.maxPlan03GroupHeadersPerPage = 2;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    std::array<harken::UnplacedBatchCredit, 2> credits{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(
        {1, 1, 1, 1, 1}, &credits[0]));
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(
        {0, 0, 0, 0, 1}, &credits[1]));
    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transactionId = 13;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    std::array<harken::Plan03GroupSliceInput, 2> groups{};
    for (std::size_t index = 0; index < groups.size(); ++index) {
        groups[index].executeFrame = 210;
        groups[index].decision =
            {static_cast<std::uint32_t>(index + 1u), 1};
    }
    std::array<harken::UnplacedBatchCohortMember, 2> members{{
        {&credits[0], &request, 1, &groups[0], 1},
        {&credits[1], nullptr, 0, &groups[1], 1},
    }};
    ReserveRefusal refusal = ReserveRefusal::None;
    harken::Plan03GroupReserveStatus groupStatus =
        harken::Plan03GroupReserveStatus::Committed;
    ScheduledTransactionPool::PrivateMaterializationTransaction lease;
    ASSERT_TRUE(pool.TryMaterializeUnplacedCohort(
        members.data(), members.size(), arms, 0,
        &lease, &refusal, &groupStatus));
    ASSERT_EQ(2u, lease.GroupCount());
    EXPECT_TRUE(lease.Group(0)->Valid());
    EXPECT_TRUE(lease.Group(1)->Valid());
    ASSERT_TRUE(lease.Commit(21));
    EXPECT_EQ(2u, pool.CaptureLivePlan03Groups(nullptr, 0));
    ASSERT_TRUE(lease.ReleasePublication());
    EXPECT_FALSE(credits[0].Valid());
    EXPECT_FALSE(credits[1].Valid());
    EXPECT_TRUE(pool.Valid());
}

TEST(ScheduledTransactionPool, CohortMaterializationUsesAggregatePageEventAndBatchGeometry) {
    enum class Geometry { EventOneShort, BatchOneShort, ExactFit };
    for (const auto geometry : {Geometry::EventOneShort,
                                Geometry::BatchOneShort,
                                Geometry::ExactFit}) {
        SCOPED_TRACE(static_cast<int>(geometry));
        Harness h;
        auto config = MakeConfig();
        config.maxScheduledTransactions = 2;
        config.maxIngressTransactionsPerQuantum = 2;
        config.maxEventsPerQuantum =
            geometry == Geometry::EventOneShort ? 1 : 2;
        config.maxFrameBatchesPerQuantum =
            geometry == Geometry::ExactFit ? 2 : 1;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms(1);
        std::array<harken::UnplacedBatchCredit, 2> credits{};
        std::array<ScheduledEventInput, 2> events{};
        std::array<TransactionRequest, 2> requests{};
        std::array<harken::UnplacedBatchCohortMember, 2> members{};
        for (std::size_t index = 0; index < members.size(); ++index) {
            ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(
                {1, 1, 1, 1, 0}, &credits[index]));
            events[index].executeFrame = 210 +
                ((geometry == Geometry::EventOneShort || index == 0)
                     ? 0 : 10);
            requests[index].transactionId = 13u + index;
            requests[index].transportEpoch = kEpoch;
            requests[index].ownerInstanceHandle = h.owner;
            requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
            requests[index].events = &events[index];
            requests[index].eventCount = 1;
            members[index] = {&credits[index], &requests[index], 1,
                              nullptr, 0};
        }
        ReserveRefusal refusal = ReserveRefusal::None;
        harken::Plan03GroupReserveStatus groupStatus =
            harken::Plan03GroupReserveStatus::Committed;
        std::uint32_t page = 0;
        ASSERT_TRUE(pool.PageForFrame(210, &page));
        {
            ScheduledTransactionPool::PrivateMaterializationTransaction lease;
            const bool admitted = pool.TryMaterializeUnplacedCohort(
                members.data(), members.size(), arms, 0,
                &lease, &refusal, &groupStatus);
            if (geometry == Geometry::ExactFit) {
                ASSERT_TRUE(admitted);
                ASSERT_TRUE(lease.Commit(21));
                ASSERT_TRUE(lease.ReleasePublication());
                EXPECT_FALSE(credits[0].Valid());
                EXPECT_FALSE(credits[1].Valid());
                EXPECT_EQ(2u, pool.Page(page)->committedTransactionCount);
            } else {
                EXPECT_FALSE(admitted);
                EXPECT_EQ(geometry == Geometry::EventOneShort
                              ? ReserveRefusal::PageEventCapacity
                              : ReserveRefusal::PageBatchCapacity,
                          refusal);
                EXPECT_FALSE(lease.Valid());
                EXPECT_EQ(0u, pool.Page(page)->committedTransactionCount);
                EXPECT_TRUE(credits[0].Valid());
                EXPECT_TRUE(credits[1].Valid());
            }
        }
        EXPECT_TRUE(pool.Valid());
        if (geometry != Geometry::ExactFit) {
            EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credits[0]));
            EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credits[1]));
        }
    }
}

TEST(ScheduledTransactionPool, CohortMaterializationRechecksCrossQuantumIngress) {
    Harness h;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::TimelineArmTable arms(1);
    std::array<harken::UnplacedBatchCredit, 2> credits{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(
        {1, 1, 1, 1, 0}, &credits[0]));
    ASSERT_EQ(0u, pool.Advance());
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(
        {1, 1, 1, 1, 0}, &credits[1]));
    std::array<ScheduledEventInput, 2> events{};
    std::array<TransactionRequest, 2> requests{};
    std::array<harken::UnplacedBatchCohortMember, 2> members{};
    for (std::size_t index = 0; index < members.size(); ++index) {
        events[index].executeFrame = 310;
        requests[index].transactionId = 13u + index;
        requests[index].transportEpoch = kEpoch;
        requests[index].ownerInstanceHandle = h.owner;
        requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
        requests[index].events = &events[index];
        requests[index].eventCount = 1;
        members[index] = {&credits[index], &requests[index], 1,
                          nullptr, 0};
    }
    ReserveRefusal refusal = ReserveRefusal::None;
    harken::Plan03GroupReserveStatus groupStatus =
        harken::Plan03GroupReserveStatus::Committed;
    ScheduledTransactionPool::PrivateMaterializationTransaction lease;
    EXPECT_FALSE(pool.TryMaterializeUnplacedCohort(
        members.data(), members.size(), arms, 0,
        &lease, &refusal, &groupStatus));
    EXPECT_EQ(ReserveRefusal::IngressRateExceeded, refusal);
    EXPECT_FALSE(lease.Valid());
    EXPECT_TRUE(credits[0].Valid());
    EXPECT_TRUE(credits[1].Valid());
    EXPECT_TRUE(pool.Valid());
    EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credits[0]));
    EXPECT_TRUE(pool.CancelUnplacedBatchCredit(&credits[1]));
}

TEST(ScheduledTransactionPool, PrivateMaterializationFaultsClearEveryPageBlockerBeforeReturn) {
    Harness h;
    for (std::uint32_t scenario = 0; scenario < 2; ++scenario) {
      for (std::uint32_t fault = 1; fault <= 3; ++fault) {
        SCOPED_TRACE(scenario);
        SCOPED_TRACE(fault);
        const bool sharedFrameBatch = scenario != 0;
        auto config = MakeConfig();
        config.maxScheduledTransactions = sharedFrameBatch ? 3 : 2;
        config.maxIngressTransactionsPerQuantum = sharedFrameBatch ? 3 : 2;
        config.maxEventsPerQuantum = sharedFrameBatch ? 2 : 1;
        config.maxFrameBatchesPerQuantum = 1;
        config.maxPlan03GroupHeadersPerPage = 1;
        config.maxArmsPerGroup = 1;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms(1);
        ScheduledEventInput independent{};
        independent.executeFrame = 110;
        TransactionRequest independentRequest{};
        independentRequest.transactionId = 17;
        independentRequest.transportEpoch = kEpoch;
        independentRequest.ownerInstanceHandle = h.owner;
        independentRequest.ownerInstanceScheduleEpoch = h.ownerEpoch;
        independentRequest.events = &independent;
        independentRequest.eventCount = 1;
        ASSERT_EQ(ReserveStatus::Committed,
                  pool.Reserve(independentRequest,
                               PrePublishLatePolicy::Refuse).status);

        harken::UnplacedBatchCredit credit{};
        const harken::UnplacedBatchCreditRequest demand{
            sharedFrameBatch ? 2u : 1u, 1, sharedFrameBatch ? 2u : 1u,
            1, 2};
        ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(demand, &credit));
        std::array<ScheduledEventInput, 2> queued{};
        queued[0].executeFrame = 210;
        queued[1].executeFrame = 210;
        std::array<TransactionRequest, 2> requests{};
        requests[0] = independentRequest;
        requests[0].transactionId = 19;
        requests[0].events = &queued[0];
        requests[1] = independentRequest;
        requests[1].transactionId = 20;
        requests[1].events = &queued[1];
        std::array<harken::Plan03GroupSliceInput, 2> groups{};
        groups[0].executeFrame = 210;
        groups[0].decision = {1, 1};
        groups[1].executeFrame = 310;
        groups[1].decision = {1, 1};
        groups[1].sliceIndex = 1;
        pool.SetPrivateMaterializationFaultStepForTest(fault);
        ReserveRefusal transactionRefusal = ReserveRefusal::None;
        harken::Plan03GroupReserveStatus groupStatus =
            harken::Plan03GroupReserveStatus::Committed;
        ScheduledTransactionPool::PrivateMaterializationTransaction lease;
        EXPECT_FALSE(pool.TryMaterializeUnplacedBatch(
            &credit, requests.data(), sharedFrameBatch ? 2 : 1,
            groups.data(), groups.size(), arms, 0,
            &lease, &transactionRefusal, &groupStatus));
        EXPECT_FALSE(lease.Valid());
        EXPECT_TRUE(credit.Valid());
        EXPECT_TRUE(pool.Valid());
        EXPECT_EQ(0u, pool.CaptureLivePlan03Groups(nullptr, 0));
        pool.Publish(400, &h.instances, &h.occurrences, &arms);
        EXPECT_EQ(2u, pool.PageStamp(2));
        EXPECT_EQ(3u, pool.PageStamp(3));
        std::array<ConsumedEvent, 1> consumed{};
        ASSERT_EQ(1u,
                  pool.ConsumePageRt(1, consumed.data(), consumed.size()));
        EXPECT_EQ(17u, consumed[0].transactionId);
        const auto priorGeneration = credit.generation;
        ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
        harken::UnplacedBatchCredit reused{};
        ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(demand, &reused));
        EXPECT_NE(priorGeneration, reused.generation);
        ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&reused));
      }
    }
}

TEST(ScheduledTransactionPool, UnplacedHolderMaterializesThreeGroupsWithConfiguredScratch) {
    Harness h;
    auto config = MakeConfig();
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 1;
    ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    harken::UnplacedBatchCredit credit{};
    const harken::UnplacedBatchCreditRequest demand{0, 0, 0, 0, 3};
    std::array<harken::Plan03GroupSliceInput, 3> groups{};
    for (std::size_t index = 0; index < groups.size(); ++index) {
        groups[index].executeFrame = 110 + index * 100;
        groups[index].decision = {1, 1};
        groups[index].sliceIndex = static_cast<std::uint16_t>(index);
    }
    ReserveRefusal transactionRefusal = ReserveRefusal::None;
    harken::Plan03GroupReserveStatus groupStatus =
        harken::Plan03GroupReserveStatus::Committed;
    for (std::uint32_t fault = 1; fault <= 4; ++fault) {
        SCOPED_TRACE(fault);
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(demand, &credit));
        ASSERT_TRUE(pool.PreflightUnplacedBatchMaterialization(
            credit, nullptr, 0, groups.data(), groups.size(), arms, 0,
            &transactionRefusal, &groupStatus));
        pool.SetPrivateMaterializationFaultStepForTest(fault);
        {
            ScheduledTransactionPool::PrivateMaterializationTransaction lease;
            EXPECT_FALSE(pool.TryMaterializeUnplacedBatch(
                &credit, nullptr, 0, groups.data(), groups.size(), arms, 0,
                &lease, &transactionRefusal, &groupStatus));
            EXPECT_FALSE(lease.Valid());
        }
        EXPECT_TRUE(pool.Valid());
        EXPECT_TRUE(credit.Valid());
        EXPECT_EQ(0u, pool.CaptureLivePlan03Groups(nullptr, 0));
        pool.Publish(400, &h.instances, &h.occurrences, &arms);
        for (std::uint32_t page = 1; page <= 3; ++page)
            EXPECT_EQ(page, pool.PageStamp(page));
        ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    }
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit(demand, &credit));

    {
        ScheduledTransactionPool::PrivateMaterializationTransaction lease;
        ASSERT_TRUE(pool.TryMaterializeUnplacedBatch(
            &credit, nullptr, 0, groups.data(), groups.size(), arms, 0,
            &lease, &transactionRefusal, &groupStatus));
        ASSERT_EQ(3u, lease.GroupCount());
        for (std::size_t index = 0; index < groups.size(); ++index)
            ASSERT_TRUE(lease.Group(index)->Valid());
        EXPECT_FALSE(pool.CanResetForEpoch(0, kEpoch + 1));
        EXPECT_FALSE(pool.Reset(config, 0, kEpoch + 1));
        ASSERT_TRUE(lease.Commit(21));
        ASSERT_TRUE(lease.ReleasePublication());
    }
    EXPECT_FALSE(credit.Valid());
    EXPECT_EQ(3u, pool.CaptureLivePlan03Groups(nullptr, 0));
}

TEST(ScheduledTransactionPool, PublishedEmptyPrefixRequiresExactPublishedUnreusedPages) {
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    EXPECT_EQ(0u, pool.PublishedEmptyPrefixEnd(0, 4));
    EXPECT_EQ(4u * sizeof(std::atomic<std::uint64_t>), pool.PublishedEmptyStampStorageBytes());
    pool.Publish(400);
    EXPECT_EQ(4u, pool.PublishedEmptyPrefixEnd(0, UINT64_MAX));
    EXPECT_EQ(2u, pool.PublishedEmptyPrefixEnd(0, 2));
    pool.ConsumerFrontier().store(2);
    EXPECT_FALSE(pool.TryAdvancePublishedEmptyPrefixRt(0, 4));
    EXPECT_EQ(2u, pool.ConsumerFrontier().load());
    EXPECT_TRUE(pool.TryAdvancePublishedEmptyPrefixRt(2, 4));
    EXPECT_EQ(4u, pool.ConsumerFrontier().load());
    pool.Advance();
    EXPECT_EQ(0u, pool.PublishedEmptyPrefixEnd(0, 4));
    EXPECT_EQ(4u, pool.PublishedEmptyPrefixEnd(1, 5));
    pool.Publish(500);
    EXPECT_EQ(5u, pool.PublishedEmptyPrefixEnd(1, 5));
    ASSERT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    EXPECT_EQ(0u, pool.PublishedEmptyPrefixEnd(0, 4));
    EXPECT_EQ(UINT64_MAX, pool.PublishedEmptyPrefixEnd(UINT64_MAX, UINT64_MAX));
}

TEST(ScheduledTransactionPool, CurrentSliceOrderPreservesWritesAcrossRecycledSliceSlots) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.executeFrame = 100;
    slice.logicCommitSequence = 7;
    slice.slice = {99, 1};
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0).status);
    slice.slice = {0, 2};
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0).status);
    pool.Publish(200);
    std::array<harken::ConsumedCurrentSlice, 2> consumed{};
    ASSERT_EQ(2u, pool.ConsumeCurrentSlicesRt(1, consumed.data(), consumed.size()));
    EXPECT_EQ(99u, consumed[0].slice.slot);
    EXPECT_EQ(0u, consumed[1].slice.slot);
}

TEST(ScheduledTransactionPool, NativeStartCalendarsHaveIndependentQuotaAndCancellationRecovery) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 2;
    config.maxNativeStartCalendarsPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    EXPECT_EQ(2u, pool.MaxVariableCurrentSlicesPerQuantum());
    EXPECT_EQ(3u, pool.MaxCurrentSlicesPerQuantum());
    harken::ScheduledCurrentSliceInput input{};
    input.executeFrame = 100;
    input.logicCommitSequence = 1;
    input.slice = {1, 1};
    ASSERT_EQ(ReserveStatus::Committed, pool.ReserveCurrentSlice(input, PrePublishLatePolicy::Refuse, 0).status);
    input.kind = harken::CurrentSliceKind::MeterProbeWithdrawal;
    input.slice = {2, 1};
    ASSERT_EQ(ReserveStatus::Committed, pool.ReserveCurrentSlice(input, PrePublishLatePolicy::Refuse, 0).status);
    input.slice = {3, 1};
    EXPECT_EQ(ReserveRefusal::PageEventCapacity, pool.ReserveCurrentSlice(input, PrePublishLatePolicy::Refuse, 0).refusal);
    input.kind = harken::CurrentSliceKind::NativeStartCalendar;
    input.executionOrdinal = 7;
    harken::HeldCurrentSliceToken first, replacement;
    ASSERT_EQ(ReserveStatus::Committed, pool.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse, 0, &first).status);
    input.slice = {4, 2};
    EXPECT_EQ(ReserveRefusal::PageEventCapacity,
        pool.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse, 0, &replacement).refusal);
    EXPECT_FALSE(replacement.IsHeld());
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&first));
    ASSERT_EQ(ReserveStatus::Committed, pool.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse, 0, &replacement).status);
    ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&replacement));
    pool.Publish(200);
    std::array<harken::ConsumedCurrentSlice, 3> consumed{};
    EXPECT_EQ(3u, pool.ConsumeCurrentSlicesRt(1, consumed.data(), 2));
    ASSERT_EQ(3u, pool.ConsumeCurrentSlicesRt(1, consumed.data(), consumed.size()));
    EXPECT_EQ(harken::CurrentSliceKind::VariableBankCut, consumed[0].kind);
    EXPECT_EQ(harken::CurrentSliceKind::MeterProbeWithdrawal, consumed[1].kind);
    EXPECT_EQ(harken::CurrentSliceKind::NativeStartCalendar, consumed[2].kind);
    EXPECT_EQ(4u, consumed[2].slice.slot);
    EXPECT_EQ(2u, consumed[2].slice.generation);
    EXPECT_EQ(7u, consumed[2].executionOrdinal);
}

TEST(ScheduledTransactionPool, NativeStartCalendarRejectsUnknownKindAndCapacityOverflow) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = UINT32_MAX;
    config.maxNativeStartCalendarsPerQuantum = 1;
    ScheduledTransactionPool pool;
    EXPECT_FALSE(pool.Reset(config, 0, kEpoch));
    config = MakeConfig();
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput input{};
    input.logicCommitSequence = 1;
    input.slice = {1, 1};
    input.kind = static_cast<harken::CurrentSliceKind>(255);
    harken::HeldCurrentSliceToken held;
    EXPECT_EQ(ReserveStatus::Refused, pool.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse, 0, &held).status);
    EXPECT_FALSE(held.IsHeld());
    input.kind = harken::CurrentSliceKind::NativeStartCalendar;
    EXPECT_EQ(ReserveRefusal::PageEventCapacity, pool.ReserveCurrentSlice(input, PrePublishLatePolicy::Refuse, 0).refusal);
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, PreparedNativeCalendarBackingSurvivesPageCommitRefusalAndEpochReuse) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    config.maxNativeStartCalendarsPerQuantum = 128;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    using Access = harken::detail::ScheduledTransactionPoolCancelTestAccess;
    Access::SetCurrentSliceCommitPageLimit(pool, 0);
    EXPECT_FALSE(pool.PrepareNativeStartCalendarBacking(128));
    ASSERT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    Access::SetCurrentSliceCommitPageLimit(pool, SIZE_MAX);
    ASSERT_TRUE(pool.PrepareNativeStartCalendarBacking(128));
    const auto committed = pool.CurrentSliceCommittedBytes();
    Access::SetCurrentSliceCommitPageLimit(pool, 0);
    for (std::uint32_t page = 0; page < pool.PageCount(); ++page) {
        harken::ScheduledCurrentSliceInput input{};
        input.executeFrame = static_cast<std::uint64_t>(page) * config.quantumFrames;
        input.logicCommitSequence = 1;
        input.slice = {0, 1};
        ASSERT_EQ(ReserveStatus::Committed,
            pool.ReserveCurrentSlice(input, PrePublishLatePolicy::Refuse, 0).status);
        input.kind = harken::CurrentSliceKind::NativeStartCalendar;
        for (std::uint32_t index = 0; index < 128; ++index) {
            input.slice.slot = index;
            harken::HeldCurrentSliceToken held;
            ASSERT_EQ(ReserveStatus::Committed,
                pool.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse, 0, &held).status);
            ASSERT_TRUE(pool.CancelHeldCurrentSlice(&held));
            ASSERT_EQ(ReserveStatus::Committed,
                pool.ReserveHeldCurrentSlice(input, PrePublishLatePolicy::Refuse, 0, &held).status);
            ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&held));
        }
        EXPECT_EQ(ReserveRefusal::PageEventCapacity,
            pool.ReserveCurrentSlice(input, PrePublishLatePolicy::Refuse, 0).refusal);
    }
    pool.Publish(config.scheduleHorizonFrames);
    std::array<harken::ConsumedCurrentSlice, 129> consumed{};
    for (std::uint32_t page = 0; page < pool.PageCount(); ++page) {
        ASSERT_EQ(consumed.size(), pool.ConsumeCurrentSlicesRt(page, consumed.data(), consumed.size()));
        EXPECT_EQ(harken::CurrentSliceKind::VariableBankCut, consumed.front().kind);
        EXPECT_EQ(127u, consumed.back().slice.slot);
    }
    EXPECT_EQ(committed, pool.CurrentSliceCommittedBytes());
    ASSERT_TRUE(pool.ResetForEpoch(0, kEpoch + 2));
    EXPECT_TRUE(pool.PrepareNativeStartCalendarBacking(128));
    EXPECT_EQ(committed, pool.CurrentSliceCommittedBytes());
}

TEST(ScheduledTransactionPool, HeldCurrentSliceCancelPreservesUnrelatedRowsAndHolds) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 3;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.executeFrame = 100;
    slice.logicCommitSequence = 7;
    slice.slice = {9, 1};
    harken::HeldCurrentSliceToken first, second;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &first).status);
    slice.logicCommitSequence = 8;
    slice.kind = harken::CurrentSliceKind::MeterProbeWithdrawal;
    slice.slice = {7, 1};
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0).status);
    slice.logicCommitSequence = 9;
    slice.kind = harken::CurrentSliceKind::VariableBankCut;
    slice.slice = {1, 2};
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &second).status);

    pool.Publish(400);
    EXPECT_EQ(100u, pool.PublishCutoffFrame());
    EXPECT_EQ(0u, pool.PageStamp(0));
    EXPECT_EQ(harken::kUnpublishedQuantumStamp, pool.PageStamp(1));
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&first));
    EXPECT_FALSE(first.IsHeld());
    EXPECT_FALSE(pool.CancelHeldCurrentSlice(&first));
    ASSERT_TRUE(pool.CanReleaseHeldCurrentSlice(second));
    pool.Publish(400);
    EXPECT_EQ(100u, pool.PublishCutoffFrame());
    ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&second));
    EXPECT_FALSE(pool.ReleaseHeldCurrentSlice(&second));
    pool.Publish(400);
    EXPECT_EQ(400u, pool.PublishCutoffFrame());
    std::array<harken::ConsumedCurrentSlice, 3> consumed{};
    ASSERT_EQ(2u, pool.ConsumeCurrentSlicesRt(1, consumed.data(), consumed.size()));
    EXPECT_EQ(7u, consumed[0].slice.slot);
    EXPECT_EQ(harken::CurrentSliceKind::MeterProbeWithdrawal, consumed[0].kind);
    EXPECT_EQ(1u, consumed[1].slice.slot);
}

TEST(ScheduledTransactionPool, HeldCurrentSliceCapacityRefusalLeavesNoReservationAndRecovers) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.executeFrame = 100;
    slice.logicCommitSequence = 7;
    slice.slice = {9, 1};
    harken::HeldCurrentSliceToken first, second;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &first).status);
    EXPECT_EQ(ReserveStatus::Refused,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &first).status);
    slice.slice = {1, 2};
    const auto refused = pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &second);
    EXPECT_EQ(ReserveStatus::Refused, refused.status);
    EXPECT_EQ(ReserveRefusal::PageEventCapacity, refused.refusal);
    EXPECT_FALSE(second.IsHeld());
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&first));
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &second).status);
    ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&second));
    pool.Publish(200);
    harken::ConsumedCurrentSlice consumed{};
    ASSERT_EQ(1u, pool.ConsumeCurrentSlicesRt(1, &consumed, 1));
    EXPECT_EQ(1u, consumed.slice.slot);
}

TEST(ScheduledTransactionPool, HeldCurrentSliceOwnershipBlocksResetDropAndPageReuseUntilCancelled) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken held;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &held).status);
    pool.Publish(100);
    pool.ConsumerFrontier().store(1, std::memory_order_release);
    EXPECT_FALSE(pool.Advance());
    EXPECT_FALSE(pool.ResetForEpoch(0, kEpoch + 1));
    EXPECT_FALSE(pool.Reset(config, 0, kEpoch));
    pool.DropPreparedOccupancy();
    ASSERT_TRUE(pool.CanReleaseHeldCurrentSlice(held));
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&held));
    ASSERT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    harken::HeldCurrentSliceToken next;
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &next).status);
    EXPECT_FALSE(pool.ReleaseHeldCurrentSlice(&held));
    ASSERT_TRUE(pool.CanReleaseHeldCurrentSlice(next));
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&next));
    EXPECT_TRUE(pool.Reset(config, 0, kEpoch));
}

TEST(ScheduledTransactionPool, HeldCurrentSliceMoveAndScopeExitCancelOnlyTheOwningPool) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool firstPool, secondPool;
    ASSERT_TRUE(firstPool.Reset(config, 0, kEpoch));
    ASSERT_TRUE(secondPool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken second;
    ASSERT_EQ(ReserveStatus::Committed,
              secondPool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &second).status);
    {
        harken::HeldCurrentSliceToken first;
        ASSERT_EQ(ReserveStatus::Committed,
                  firstPool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &first).status);
        EXPECT_FALSE(secondPool.ReleaseHeldCurrentSlice(&first));
        harken::HeldCurrentSliceToken moved(std::move(first));
        EXPECT_FALSE(first.IsHeld());
        EXPECT_TRUE(firstPool.CanReleaseHeldCurrentSlice(moved));
        EXPECT_TRUE(secondPool.CanReleaseHeldCurrentSlice(second));
    }
    EXPECT_TRUE(firstPool.ResetForEpoch(0, kEpoch + 1));
    EXPECT_FALSE(secondPool.ResetForEpoch(0, kEpoch + 1));
    ASSERT_TRUE(secondPool.CancelHeldCurrentSlice(&second));
}

TEST(ScheduledTransactionPool, CurrentSlicePreflightDoesNotConsumeQuotaOrChangeRefusalCounters) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput input{};
    input.executeFrame = 20;
    input.logicCommitSequence = 7;
    for (unsigned i = 0; i < 4; ++i) {
        const auto ready = pool.PreflightHeldCurrentSlice(input,
            PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, 100);
        EXPECT_EQ(ReserveStatus::Committed, ready.status);
        EXPECT_EQ(100u, ready.firstFrame);
        EXPECT_TRUE(ready.rescheduled);
        EXPECT_EQ(ReserveStatus::DroppedLate,
            pool.PreflightHeldCurrentSlice(input, PrePublishLatePolicy::Drop, 100).status);
    }
    EXPECT_EQ(0u, pool.DroppedLateCount());
    EXPECT_TRUE(pool.CanResetForEpoch(0, kEpoch + 1));
    input.slice = {0, 1};
    harken::HeldCurrentSliceToken held;
    const auto actual = pool.ReserveHeldCurrentSlice(input,
        PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, 100, &held);
    ASSERT_EQ(ReserveStatus::Committed, actual.status);
    EXPECT_EQ(100u, actual.firstFrame);
    const auto refused = pool.PreflightHeldCurrentSlice(input,
        PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, 100);
    EXPECT_EQ(ReserveStatus::Refused, refused.status);
    EXPECT_EQ(ReserveRefusal::PageEventCapacity, refused.refusal);
    EXPECT_EQ(0u, pool.RefusalCount(ReserveRefusal::PageEventCapacity));
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&held));
    EXPECT_EQ(ReserveStatus::Committed, pool.PreflightHeldCurrentSlice(input,
        PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, 100).status);
}

TEST(ScheduledTransactionPool, HeldCurrentSliceReschedulingRetainsSemanticActionOrder) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.executeFrame = 20;
    slice.logicCommitSequence = 7;
    slice.executionOrdinal = 2;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken later, earlier;
    const auto reserved = pool.ReserveHeldCurrentSlice(
        slice, PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, 100, &later);
    ASSERT_EQ(ReserveStatus::Committed, reserved.status);
    EXPECT_TRUE(reserved.rescheduled);
    EXPECT_EQ(100u, later.ExecuteFrame());
    slice.executeFrame = 100;
    slice.executionOrdinal = 1;
    slice.slice = {99, 1};
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &earlier).status);
    ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&later));
    ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&earlier));
    pool.Publish(200);
    std::array<harken::ConsumedCurrentSlice, 2> consumed{};
    ASSERT_EQ(2u, pool.ConsumeCurrentSlicesRt(1, consumed.data(), consumed.size()));
    EXPECT_EQ(1u, consumed[0].executionOrdinal);
    EXPECT_EQ(99u, consumed[0].slice.slot);
    EXPECT_EQ(2u, consumed[1].executionOrdinal);
    EXPECT_EQ(0u, consumed[1].slice.slot);
}

TEST(ScheduledTransactionPool, HeldCurrentSliceMoveAssignmentCancelsDestinationReservation) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken first, second;
    ASSERT_EQ(ReserveStatus::Committed,
        pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &first).status);
    slice.slice = {1, 1};
    ASSERT_EQ(ReserveStatus::Committed,
        pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &second).status);
    first = std::move(second);
    EXPECT_FALSE(second.IsHeld());
    ASSERT_TRUE(pool.ReleaseHeldCurrentSlice(&first));
    pool.Publish(100);
    std::array<harken::ConsumedCurrentSlice, 2> consumed{};
    ASSERT_EQ(1u, pool.ConsumeCurrentSlicesRt(0, consumed.data(), consumed.size()));
    EXPECT_EQ(1u, consumed[0].slice.slot);
}

TEST(ScheduledTransactionPool, HeldCurrentSliceCanCancelAfterWindowOverflowAtFirstOrLaterPage) {
    for (auto offset : {0u, 100u}) {
        SCOPED_TRACE(offset);
        auto config = MakeConfig();
        config.maxCurrentSlicesPerQuantum = 1;
        ScheduledTransactionPool pool;
        const auto start = (UINT64_MAX - 400u) / 100u * 100u;
        ASSERT_TRUE(pool.Reset(config, start, kEpoch));
        harken::ScheduledCurrentSliceInput slice{};
        slice.executeFrame = start + offset;
        slice.logicCommitSequence = 7;
        slice.slice = {0, 1};
        harken::HeldCurrentSliceToken held;
        ASSERT_EQ(ReserveStatus::Committed,
            pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, start, &held).status);
        EXPECT_EQ(0u, pool.Advance());
        EXPECT_FALSE(pool.Valid());
        EXPECT_FALSE(pool.CanReleaseHeldCurrentSlice(held));
        EXPECT_FALSE(pool.ReleaseHeldCurrentSlice(&held));
        ASSERT_TRUE(pool.CancelHeldCurrentSlice(&held));
        EXPECT_TRUE(pool.Reset(config, 0, kEpoch + 1));
    }
}

TEST(ScheduledTransactionPool, HeldCurrentSliceLaterPageAllowsUnrelatedPageToAdvance) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.executeFrame = 100;
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken held;
    ASSERT_EQ(ReserveStatus::Committed,
        pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &held).status);
    pool.Publish(100);
    pool.ConsumerFrontier().store(1, std::memory_order_release);
    pool.Advance();
    EXPECT_EQ(100u, pool.WindowStartFrame());
    ASSERT_TRUE(pool.CanReleaseHeldCurrentSlice(held));
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&held));
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(ScheduledTransactionPool, OrdinaryEventsCannotWrapExhaustedCurrentSliceIdentity) {
    Harness h;
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken first, second;
    harken::detail::ScheduledTransactionPoolCancelTestAccess::SetCurrentSliceSerial(pool, UINT64_MAX - 1);
    ASSERT_EQ(ReserveStatus::Committed,
        pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &first).status);
    ScheduledEventInput event{};
    event.executeFrame = 10;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    for (auto id : {11u, 12u}) {
        request.transactionId = id;
        ASSERT_EQ(ReserveStatus::Committed, pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    }
    const auto refused = pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &second);
    EXPECT_EQ(ReserveStatus::Refused, refused.status);
    EXPECT_EQ(ReserveRefusal::CommitSerialExhausted, refused.refusal);
    EXPECT_FALSE(second.IsHeld());
    ASSERT_TRUE(pool.CancelHeldCurrentSlice(&first));
    EXPECT_TRUE(pool.Reset(config, 0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, HeldCurrentSliceCanRollBackAfterUnrelatedPageGenerationExhaustion) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetEmptyPageGeneration(
        pool, 0, UINT64_MAX));
    harken::ScheduledCurrentSliceInput slice{};
    slice.executeFrame = 100;
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    {
        harken::HeldCurrentSliceToken held;
        ASSERT_EQ(ReserveStatus::Committed,
            pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &held).status);
        pool.Publish(100);
        pool.ConsumerFrontier().store(1, std::memory_order_release);
        EXPECT_EQ(0u, pool.Advance());
        EXPECT_FALSE(pool.Valid());
        EXPECT_FALSE(pool.Reset(config, 0, kEpoch + 1));
    }
    EXPECT_TRUE(pool.Reset(config, 0, kEpoch + 1));
}

TEST(ScheduledTransactionPool, CurrentSliceSerialExhaustionRefusesWithoutAbaOrPublicationHold) {
    auto config = MakeConfig();
    config.maxCurrentSlicesPerQuantum = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    harken::ScheduledCurrentSliceInput slice{};
    slice.logicCommitSequence = 7;
    slice.slice = {0, 1};
    harken::HeldCurrentSliceToken held;
    harken::detail::ScheduledTransactionPoolCancelTestAccess::SetCurrentSliceSerial(pool, UINT64_MAX);
    const auto refused = pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &held);
    EXPECT_EQ(ReserveStatus::Refused, refused.status);
    EXPECT_EQ(ReserveRefusal::CommitSerialExhausted, refused.refusal);
    EXPECT_FALSE(held.IsHeld());
    EXPECT_TRUE(pool.ResetForEpoch(0, kEpoch + 1));
    ASSERT_EQ(ReserveStatus::Committed,
              pool.ReserveHeldCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0, &held).status);
    EXPECT_TRUE(pool.ReleaseHeldCurrentSlice(&held));
    pool.Publish(100);
    EXPECT_EQ(0u, pool.PageStamp(0));
}
#endif

TEST(ScheduledTransactionPool, PublishedEmptyPrefixStopsBeforeEventsSlicesGroupsAndPreparation) {
    Harness h;
    for (unsigned kind = 0; kind < 4; ++kind) {
        SCOPED_TRACE(kind);
        auto config = MakeConfig();
        config.maxCurrentSlicesPerQuantum = 2;
        config.maxPlan03GroupHeadersPerPage = 1;
        config.maxArmsPerGroup = 2;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms(1);
        ScheduledEventInput event{};
        event.executeFrame = 200;
        TransactionRequest request{};
        request.transactionId = 1;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.events = &event;
        request.eventCount = 1;
        PreparedTransactionBatch prepared;
        if (kind == 0) {
            ASSERT_EQ(ReserveStatus::Committed, pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
        } else if (kind == 1) {
            harken::ScheduledCurrentSliceInput slice{};
            slice.executeFrame = 200;
            slice.logicCommitSequence = 1;
            slice.slice = {0, 1};
            ASSERT_EQ(ReserveStatus::Committed, pool.ReserveCurrentSlice(slice, PrePublishLatePolicy::Refuse, 0).status);
        } else if (kind == 2) {
            harken::Plan03GroupSliceInput group{};
            group.executeFrame = 200;
            group.decision = {0, 1};
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(group, arms));
        } else {
            ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
        }
        pool.Publish(400, &h.instances, &h.occurrences, &arms);
        EXPECT_EQ(2u, pool.PublishedEmptyPrefixEnd(0, 4));
        if (kind == 3) {
            ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
            EXPECT_EQ(2u, pool.PublishedEmptyPrefixEnd(0, 4));
            pool.Publish(400, &h.instances, &h.occurrences, &arms);
            EXPECT_EQ(4u, pool.PublishedEmptyPrefixEnd(0, 4));
        } else {
            pool.DropPreparedOccupancy();
            EXPECT_EQ(2u, pool.PublishedEmptyPrefixEnd(0, 4));
        }
    }
}

TEST(ScheduledTransactionPool, FrontierEventBindingIsFrozenForBothReservationAndConsumerPaths) {
    Harness h;
    for (const bool prepared : {false, true}) {
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        ScheduledEventInput event{};
        event.executeFrame = 20;
        event.logicCommitSequence = 11;
        event.frontier.kind = harken::FrontierOperationKind::Dependent;
        event.frontier.entry = {{1, 2}, 3, {h.owner, h.ownerEpoch, 0, 0, h.owner, h.ownerEpoch, 0, 1}};
        event.frontier.owner = event.frontier.entry.owner;
        event.frontier.world = 5;
        event.frontier.transport = kEpoch;
        event.frontier.operationOrdinal = 13;
        event.frontier.operationEventCount = 1;
        const auto expected = event.frontier;
        TransactionRequest request{};
        request.transactionId = 11;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.events = &event;
        request.eventCount = 1;
        if (prepared) {
            PreparedTransactionBatch batch;
            ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(&request, 1, 0, &batch));
            ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&batch, 11));
        } else {
            ASSERT_EQ(ReserveStatus::Committed, pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
        }
        event.frontier.world = 99;
        pool.Publish(100, &h.instances, &h.occurrences);
        ConsumedEvent logic{}, audio{};
        ASSERT_EQ(1u, pool.ConsumePage(0, kEpoch, h.instances, h.occurrences, &logic, 1));
        ASSERT_EQ(1u, pool.ConsumePageRt(0, &audio, 1));
        EXPECT_EQ(expected, logic.frontier);
        EXPECT_EQ(expected, audio.frontier);
        EXPECT_EQ(logic.rowSequence, audio.rowSequence);
        EXPECT_NE(0u, audio.rowSequence);
    }
}

TEST(ScheduledTransactionPool,
     ImmediateEntryGateKeepsUnfinishedCohortInertBesideUnrelatedWork) {
    Harness h;
    for (const bool audioWins : {false, true}) {
        SCOPED_TRACE(audioWins);
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        harken::ImmediateEntryAttemptGate gate;
        ASSERT_TRUE(gate.Prepare(1));
        const harken::ImmediateEntryAttemptRef entry{&gate, 1};

        std::array<ScheduledEventInput, 3> events{};
        std::array<TransactionRequest, 3> requests{};
        for (std::size_t index = 0; index < requests.size(); ++index) {
            events[index].executeFrame = 20 + 10 * index;
            events[index].logicCommitSequence = 11 + index;
            requests[index].transactionId = 11 + index;
            requests[index].transportEpoch = kEpoch;
            requests[index].ownerInstanceHandle = h.owner;
            requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
            requests[index].events = &events[index];
            requests[index].eventCount = 1;
            if (index < 2) requests[index].immediateEntry = entry;
        }
        PreparedTransactionBatch cohort;
        ASSERT_EQ(ReserveRefusal::None,
                  pool.PrepareTransactionBatch(requests.data(), 2, 0, &cohort));
        ASSERT_EQ(ReserveStatus::Committed,
                  pool.Reserve(requests[2], PrePublishLatePolicy::Refuse).status);
        ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&cohort, 11));
        ASSERT_TRUE(gate.Finalize(1));
        pool.Publish(100, &h.instances, &h.occurrences);
        ASSERT_EQ(0u, pool.PublishedEmptyPrefixEnd(0, 1));
        EXPECT_TRUE(pool.VerifyImmediateEntryPage(0, entry, 2, 0));
        EXPECT_FALSE(pool.VerifyImmediateEntryPage(0, entry, 3, 0));

        if (!audioWins) ASSERT_TRUE(gate.Commit(1));
        std::array<ConsumedEvent, 3> consumed{};
        const std::size_t count = pool.ConsumePageRt(0, consumed.data(), consumed.size());
        if (audioWins) {
            EXPECT_EQ(1u, count);
            EXPECT_EQ(40u, consumed[0].executeFrame);
            EXPECT_EQ(harken::ImmediateEntryAttemptState::Missed,
                      gate.Observe(1));
            EXPECT_FALSE(gate.Commit(1));
        } else {
            EXPECT_EQ(3u, count);
            EXPECT_EQ(20u, consumed[0].executeFrame);
            EXPECT_EQ(30u, consumed[1].executeFrame);
            EXPECT_EQ(40u, consumed[2].executeFrame);
            EXPECT_EQ(harken::ImmediateEntryAttemptState::Committed,
                      gate.Observe(1));
        }
    }
}

TEST(ScheduledTransactionPool, ImmediateEntryPublicationProofRefusesAudioAndCancelUntilCommitOrReturn) {
    using State = harken::ImmediateEntryAttemptState;
    using Gate = harken::ImmediateEntryAttemptGate;
    Gate gate;
    Gate::PublicationProof proof;
    ASSERT_TRUE(gate.Prepare(1));
    EXPECT_FALSE(gate.ProvePublication(2, &proof));
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(gate.ProvePublication(1, &proof));
    EXPECT_EQ(State::PublicationHeld, gate.Observe(1));
    EXPECT_FALSE(gate.AdmitAudio(1));
    EXPECT_EQ(State::PublicationHeld, gate.Observe(1));
    EXPECT_FALSE(gate.Abort(1));
    EXPECT_FALSE(gate.Finalize(1));
    EXPECT_FALSE(gate.Commit(1));
    EXPECT_FALSE(gate.Retire(1));
    EXPECT_FALSE(gate.Prepare(2));
    Gate::PublicationProof other;
    EXPECT_FALSE(gate.ProvePublication(1, &other));
    EXPECT_FALSE(other.Valid());
    proof.Release();
    EXPECT_EQ(State::Preparing, gate.Observe(1));
    ASSERT_TRUE(gate.ProvePublication(1, &proof));
    Gate::ApplyPublication(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(State::Committed, gate.Observe(1));
    EXPECT_TRUE(gate.AdmitAudio(1));
    EXPECT_FALSE(gate.ProvePublication(1, &proof));
    ASSERT_TRUE(gate.Retire(1));
    ASSERT_TRUE(gate.Prepare(2));
    EXPECT_FALSE(gate.ProvePublication(1, &proof));
    ASSERT_TRUE(gate.Abort(2));
    ASSERT_TRUE(gate.Retire(2));
}

TEST(ScheduledTransactionPool, ImmediateEntryPublicationProofMoveAbandonAndOwnerDestructionReturnOriginalGeneration) {
    using State = harken::ImmediateEntryAttemptState;
    using Gate = harken::ImmediateEntryAttemptGate;
    Gate::PublicationProof survivor;
    {
        Gate first, second;
        ASSERT_TRUE(first.Prepare(1));
        ASSERT_TRUE(second.Prepare(7));
        ASSERT_TRUE(first.ProvePublication(1, &survivor));
        Gate::PublicationProof other;
        EXPECT_FALSE(second.ProvePublication(7, &survivor));
        ASSERT_TRUE(second.ProvePublication(7, &other));
        survivor = std::move(other);
        EXPECT_FALSE(other.Valid());
        EXPECT_EQ(State::Preparing, first.Observe(1));
        ASSERT_TRUE(first.Abort(1));
        ASSERT_TRUE(first.Retire(1));
        Gate::PublicationProof moved(std::move(survivor));
        EXPECT_FALSE(survivor.Valid());
        EXPECT_EQ(State::PublicationHeld, second.Observe(7));
        moved.Release();
        EXPECT_EQ(State::Preparing, second.Observe(7));
        ASSERT_TRUE(second.ProvePublication(7, &survivor));
    }
    EXPECT_FALSE(survivor.Valid());
    survivor.Release();
}

TEST(ScheduledTransactionPool,
     ImmediateEntryGroupCannotClaimBeforeTheWholeAttemptCommits) {
    for (const bool audioWins : {false, true}) {
        SCOPED_TRACE(audioWins);
        auto config = MakeConfig();
        config.maxPlan03GroupHeadersPerPage = 2;
        config.maxArmsPerGroup = 2;
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
        harken::TimelineArmTable arms(1);
        harken::ImmediateEntryAttemptGate gate;
        ASSERT_TRUE(gate.Prepare(1));
        const harken::ImmediateEntryAttemptRef entry{&gate, 1};
        harken::Plan03GroupSliceInput group{};
        group.executeFrame = 20;
        group.decision = {0, 1};
        group.immediateEntry = entry;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(group, arms));
        ASSERT_TRUE(gate.Finalize(1));
        pool.Publish(100);
        EXPECT_TRUE(pool.VerifyImmediateEntryPage(0, entry, 0, 1));
        harken::Plan03PageGroupView live{};
        ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&live, 1));
        harken::Plan03PageGroupView producer{};
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(
            live.identity, &producer));
        EXPECT_EQ(harken::ImmediateEntryAttemptState::Finalizing,
                  gate.Observe(1));
        if (!audioWins) ASSERT_TRUE(gate.Commit(1));
        harken::Plan03PageGroupView audioResolved{};
        EXPECT_EQ(!audioWins, pool.ResolvePlan03GroupSliceRt(
            live.identity, &audioResolved));
        std::array<harken::Plan03PageGroupView, 2> captured{};
        const auto count = pool.CapturePlan03PageGroupsRt(
            0, captured.data(), captured.size());
        if (audioWins) {
            EXPECT_EQ(0u, count);
            EXPECT_EQ(harken::ImmediateEntryAttemptState::Missed,
                      gate.Observe(1));
            EXPECT_FALSE(gate.Commit(1));
        } else {
            ASSERT_EQ(1u, count);
            EXPECT_EQ(group.decision.slot, captured[0].identity.decision.slot);
            EXPECT_EQ(harken::ImmediateEntryAttemptState::Committed,
                      gate.Observe(1));
        }
    }
}

TEST(ScheduledTransactionPool, PreparedFrontierBindingPreservesFrozenKeyAndRefusesPublishedMutation) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ScheduledEventInput input{};
    input.executeFrame = 20;
    input.logicCommitSequence = 11;
    input.executionOrdinal = 3;
    input.eventIndex = 4;
    input.transactionOrdinal = 5;
    input.subeventPhase = 20;
    TransactionRequest request{};
    request.transactionId = 11;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &input;
    request.eventCount = 1;
    PreparedTransactionBatch batch;
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(&request, 1, 0, &batch));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(batch, 11));
    ConsumedEvent before{}, after{};
    ASSERT_TRUE(pool.CapturePreparedTransactionEvent(batch, 0, 0, &before));
    harken::FrontierEventBinding binding{};
    binding.kind = harken::FrontierOperationKind::Genuine;
    binding.entry = {{1, 2}, 3, {h.owner, h.ownerEpoch, 0, 0, h.owner, h.ownerEpoch, 0, 1}};
    binding.owner = binding.entry.owner;
    binding.world = 5;
    binding.transport = kEpoch;
    binding.operationEventCount = 1;
    EXPECT_FALSE(pool.BindPreparedTransactionEventFrontier(batch, 1, 0, binding));
    EXPECT_FALSE(pool.BindPreparedTransactionEventFrontier(batch, 0, 1, binding));
    auto wrong = binding;
    wrong.transport = kEpoch + 1;
    EXPECT_FALSE(pool.BindPreparedTransactionEventFrontier(batch, 0, 0, wrong));
    ASSERT_TRUE(pool.BindPreparedTransactionEventFrontier(batch, 0, 0, binding));
    ASSERT_TRUE(pool.BindPreparedTransactionEventFrontier(batch, 0, 0, binding));
    wrong = binding;
    ++wrong.world;
    EXPECT_FALSE(pool.BindPreparedTransactionEventFrontier(batch, 0, 0, wrong));
    ASSERT_TRUE(pool.CapturePreparedTransactionEvent(batch, 0, 0, &after));
    EXPECT_EQ(binding, after.frontier);
    EXPECT_EQ(before.executeFrame, after.executeFrame);
    EXPECT_EQ(before.logicCommitSequence, after.logicCommitSequence);
    EXPECT_EQ(before.executionOrdinal, after.executionOrdinal);
    EXPECT_EQ(before.eventIndex, after.eventIndex);
    EXPECT_EQ(before.transactionOrdinal, after.transactionOrdinal);
    EXPECT_EQ(before.subeventPhase, after.subeventPhase);
    EXPECT_EQ(before.rowSequence, after.rowSequence);
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&batch, 11, true));
    EXPECT_FALSE(pool.BindPreparedTransactionEventFrontier(batch, 0, 0, binding));
    ASSERT_TRUE(pool.ReleasePreparedTransactionPublication(&batch));
    pool.Publish(100, &h.instances, &h.occurrences);
    ASSERT_EQ(1u, pool.ConsumePageRt(0, &after, 1));
    EXPECT_EQ(binding, after.frontier);
    EXPECT_EQ(before.rowSequence, after.rowSequence);
    EXPECT_FALSE(pool.BindPreparedTransactionEventFrontier(batch, 0, 0, binding));
}

TEST(ScheduledTransactionPool, FrontierIdentityRefusalAndLateFrameNeverShiftAcceptedCoordinates) {
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    Harness h;
    ScheduledEventInput event{};
    event.executeFrame = 20;
    event.logicCommitSequence = 11;
    event.frontier.kind = harken::FrontierOperationKind::Entry;
    event.frontier.entry = {{1, 2}, 3, {h.owner, h.ownerEpoch, 0, 0, h.owner, h.ownerEpoch, 0, 1}};
    event.frontier.owner = event.frontier.entry.owner;
    event.frontier.world = 5;
    event.frontier.transport = kEpoch;
    event.frontier.operationEventCount = 1;
    TransactionRequest request{};
    request.transactionId = 11;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    EXPECT_EQ(ReserveRefusal::FrontierIdentity, pool.PreflightTransaction(request, 0));
    EXPECT_EQ(ReserveRefusal::FrontierIdentity,
        pool.Reserve(request, PrePublishLatePolicy::Refuse).refusal);
    event.frontier.entryEventOrdinal = 0;
    EXPECT_EQ(ReserveRefusal::None, pool.PreflightTransaction(request, 0));
    EXPECT_EQ(ReserveRefusal::Late,
        pool.Reserve(request, PrePublishLatePolicy::ExecuteAtEarliestSafeFrame, 30).refusal);
    EXPECT_EQ(20u, event.executeFrame);
    EXPECT_EQ(0u, pool.LiveTransactionCount());
}

// Builds a transaction of one event per frame (eventIndex ascending, payload
// derived from the frame so a digest can tell events apart) and reserves it.
ReserveResult ReserveFrames(ScheduledTransactionPool& pool, const Harness& h,
                            std::uint64_t txnId,
                            std::initializer_list<std::uint64_t> frames,
                            PrePublishLatePolicy policy =
                                PrePublishLatePolicy::Refuse) {
    std::vector<ScheduledEventInput> events;
    std::uint32_t index = 0;
    for (std::uint64_t f : frames) {
        ScheduledEventInput e;
        e.executeFrame = f;
        e.logicCommitSequence = txnId;
        e.eventIndex = index++;
        e.transactionOrdinal = 0;
        e.subeventPhase = 0;
        e.payload = 0x1000u + f;
        events.push_back(e);
    }
    TransactionRequest req;
    req.transactionId = txnId;
    req.transportEpoch = kEpoch;
    req.ownerInstanceHandle = h.owner;
    req.ownerInstanceScheduleEpoch = h.ownerEpoch;
    req.events = events.data();
    req.eventCount = events.size();
    return pool.Reserve(req, policy);
}

// FNV-1a over the ordered consume of one page: the fingerprint a determinism
// check compares. Independent of the pool's internals -- it only sees the public
// consumed events.
std::uint64_t ConsumeDigest(ScheduledTransactionPool& pool, std::uint32_t page,
                            TransportEpoch currentEpoch, const Harness& h,
                            std::size_t* outCount = nullptr) {
    std::array<ConsumedEvent, 256> buffer{};
    const std::size_t n = pool.ConsumePage(page, currentEpoch, h.instances,
                                           h.occurrences, buffer.data(),
                                           buffer.size());
    if (outCount != nullptr) {
        *outCount = n;
    }
    std::uint64_t hash = 14695981039346656037ull;
    auto fold = [&hash](std::uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            hash ^= (v >> (b * 8)) & 0xFFu;
            hash *= 1099511628211ull;
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        const ConsumedEvent& e = buffer[i];
        fold(e.executeFrame);
        fold(e.logicCommitSequence);
        fold(e.eventIndex);
        fold(e.transactionOrdinal);
        fold(e.subeventPhase);
        fold(e.payload);
        fold(e.transactionId);
    }
    return hash;
}

} // namespace

// ---- carried over from the retired HorizonPageRing tests -------------------

// The window a producer may file into is stated, and the page a frame lands in is
// arithmetic rather than a search.
TEST(ScheduledTransactionPool, WindowIsStatedAndPageIsArithmetic) {
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    EXPECT_EQ(pool.WindowStartFrame(), 0u);
    EXPECT_EQ(pool.WindowEndFrame(), 400u);
    EXPECT_EQ(pool.PageCount(), 4u);

    std::uint32_t page = 99;
    ASSERT_TRUE(pool.PageForFrame(0, &page));
    EXPECT_EQ(page, 0u);
    ASSERT_TRUE(pool.PageForFrame(99, &page));
    EXPECT_EQ(page, 0u);
    ASSERT_TRUE(pool.PageForFrame(100, &page));
    EXPECT_EQ(page, 1u);
    ASSERT_TRUE(pool.PageForFrame(399, &page));
    EXPECT_EQ(page, 3u);
    EXPECT_FALSE(pool.PageForFrame(400, &page));
}

// Asking and trying give the same answer: a frame beyond the window refuses the
// same way PageForFrame reports it.
TEST(ScheduledTransactionPool, RefusesOutsideWindowTheSameWayItReports) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    EXPECT_EQ(ReserveFrames(pool, h, 1, {399}).status, ReserveStatus::Committed);
    const ReserveResult beyond = ReserveFrames(pool, h, 2, {400});
    EXPECT_EQ(beyond.status, ReserveStatus::Refused);
    EXPECT_EQ(beyond.refusal, ReserveRefusal::OutsideWindow);
    EXPECT_FALSE(pool.PageForFrame(400, nullptr));
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
}

// A published page refuses a late arrival rather than sliding it to a later page;
// nothing lands in the following page instead.
TEST(ScheduledTransactionPool, PublishedPageRefusesLateInsteadOfSliding) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    pool.Publish(100); // freeze page 0 (frames 0..99)
    EXPECT_TRUE(pool.IsFrozen(0));
    EXPECT_FALSE(pool.IsFrozen(1));

    const ReserveResult late =
        ReserveFrames(pool, h, 1, {50}, PrePublishLatePolicy::Refuse);
    EXPECT_EQ(late.status, ReserveStatus::Refused);
    EXPECT_EQ(late.refusal, ReserveRefusal::Late);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
    std::size_t page1Count = 0;
    ConsumeDigest(pool, 1, kEpoch, h, &page1Count);
    EXPECT_EQ(page1Count, 0u); // did not slide into page 1

    // A frame at or past the frontier is still accepted.
    EXPECT_EQ(ReserveFrames(pool, h, 2, {150}).status, ReserveStatus::Committed);
}

// A cutoff past the far edge still freezes every page currently in the window.
// Recording that far cutoff without stamping those pages would leave the
// consumer frontier unpublished and block Advance forever.
TEST(ScheduledTransactionPool, PublishPastWindowEndStillFreezesCurrentPages) {
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    pool.Publish(10000);
    EXPECT_EQ(pool.PublishCutoffFrame(), 10000u);
    EXPECT_TRUE(pool.IsFrozen(0));
    EXPECT_TRUE(pool.IsFrozen(1));
    EXPECT_TRUE(pool.IsFrozen(2));
    EXPECT_TRUE(pool.IsFrozen(3));
    EXPECT_EQ(pool.PageStamp(0), 0u);
    EXPECT_EQ(pool.PageStamp(1), 1u);
    EXPECT_EQ(pool.PageStamp(2), 2u);
    EXPECT_EQ(pool.PageStamp(3), 3u);
}

// A full pool refuses rather than evicting: evicting accepted work would silence
// it.
TEST(ScheduledTransactionPool, FullPoolRefusesRatherThanEvicting) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxScheduledTransactions = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(pool, h, 2, {20}).status, ReserveStatus::Committed);

    const ReserveResult full = ReserveFrames(pool, h, 3, {30});
    EXPECT_EQ(full.status, ReserveStatus::Refused);
    EXPECT_EQ(full.refusal, ReserveRefusal::PoolFull);
    EXPECT_EQ(pool.LiveTransactionCount(), 2u);
    std::size_t count = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &count);
    EXPECT_EQ(count, 2u); // both survivors still filed
}

// Capacity bounds the whole pool, not each page: a burst spread across empty
// pages is bounded by the same number as one concentrated in a single page.
TEST(ScheduledTransactionPool, BoundsTheWholePoolNotEachPage) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxScheduledTransactions = 3;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(pool, h, 2, {110}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(pool, h, 3, {210}).status, ReserveStatus::Committed);
    // Room is gone even though the fourth page is empty.
    EXPECT_EQ(ReserveFrames(pool, h, 4, {310}).refusal, ReserveRefusal::PoolFull);
    EXPECT_EQ(pool.TransactionHighWater(), 3u);
}

// Advancing is the only way filed work goes away and it reports how much went
// with it; a survivor moves down a page rather than being re-filed.
TEST(ScheduledTransactionPool, AdvanceDropsOnlyTheLeavingPageAndReportsIt) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10, 20}).status,
              ReserveStatus::Committed); // two events in page 0
    ASSERT_EQ(ReserveFrames(pool, h, 2, {150}).status,
              ReserveStatus::Committed); // one event in page 1

    EXPECT_EQ(pool.Advance(), 2u); // page 0's two events fell out
    EXPECT_EQ(pool.WindowStartFrame(), 100u);
    EXPECT_EQ(pool.WindowEndFrame(), 500u);

    // The survivor is now on page 0 (moved down), consuming to one event.
    std::size_t count = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &count);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
}

// The page that leaves the window comes back reopened at the far end -- not
// frozen, a fresh generation, the next absolute quantum -- so a producer that
// publishes on time never meets a frozen page it did not publish.
TEST(ScheduledTransactionPool, ReopensThePageThatLeavesTheWindow) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    const std::uint64_t gen0 = pool.Page(0)->pageGeneration;
    pool.Publish(100); // freeze page 0
    ASSERT_TRUE(pool.IsFrozen(0));

    EXPECT_EQ(pool.Advance(), 0u);
    // Old page 0 is now the far page (logical page 3), reopened.
    EXPECT_FALSE(pool.IsFrozen(3));
    const harken::HorizonPageHeader* far = pool.Page(3);
    ASSERT_NE(far, nullptr);
    EXPECT_EQ(far->pageGeneration, gen0 + 1);
    EXPECT_EQ(far->absoluteQuantumIndex, 4u); // quanta 0..3 seen, next is 4
    EXPECT_FALSE(far->frozen);
    // It accepts work again (page 3 covers frames 400..499).
    EXPECT_EQ(ReserveFrames(pool, h, 1, {450}).status, ReserveStatus::Committed);
}

TEST(ScheduledTransactionPool, ConsumerFrontierBlocksUnconsumedPageReclamation) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10}).status,
              ReserveStatus::Committed);
    const std::uint64_t windowBefore = pool.WindowStartQuantum();
    const std::uint64_t frontierBefore = pool.ConsumerFrontier().load();

    EXPECT_EQ(pool.Advance(), 0u);
    EXPECT_EQ(pool.WindowStartQuantum(), windowBefore);
    EXPECT_EQ(pool.ConsumerFrontier().load(), frontierBefore);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
    EXPECT_EQ(pool.AdvanceBlockedByConsumer(), 1u);

    std::size_t count = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &count);
    EXPECT_EQ(count, 1u);
}

TEST(ScheduledTransactionPool, AdvancesOnlyThroughConsumedPages) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10}).status,
              ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(pool, h, 2, {110}).status,
              ReserveStatus::Committed);
    pool.ConsumerFrontier().store(1);

    EXPECT_EQ(pool.Advance(), 1u);
    EXPECT_EQ(pool.WindowStartQuantum(), 1u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);

    EXPECT_EQ(pool.Advance(), 0u);
    EXPECT_EQ(pool.WindowStartQuantum(), 1u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
    EXPECT_EQ(pool.AdvanceBlockedByConsumer(), 1u);
}

// The stamps a transaction carries are what a cancel invalidates: after the
// owner's schedule epoch is bumped, the filed transaction consumes as stale in
// one check, skipping every event it filed.
TEST(ScheduledTransactionPool, StaleTransactionNoOpsAfterOwnerCancel) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10, 20, 30}).status,
              ReserveStatus::Committed);

    std::size_t before = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &before);
    EXPECT_EQ(before, 3u);

    ASSERT_NE(h.instances.BumpScheduleEpoch(h.owner), 0u);
    std::size_t after = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &after);
    EXPECT_EQ(after, 0u); // whole transaction no-ops
}

// ---- Consume ordering and stale checks -------------------------------------

// The consume order is a total order on the ordering keys, independent of the
// order work was filed in: two pools fed the same events in opposite insertion
// orders consume to the same fingerprint and the same frame sequence.
TEST(ScheduledTransactionPool, ConsumeOrderIsDeterministicRegardlessOfArrival) {
    Harness ha;
    Harness hb;
    ScheduledTransactionPool a;
    ScheduledTransactionPool b;
    ASSERT_TRUE(a.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_TRUE(b.Reset(MakeConfig(), 0, kEpoch));

    // Same four page-0 events, filed earliest-first into a and latest-first into b.
    ASSERT_EQ(ReserveFrames(a, ha, 1, {10}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(a, ha, 2, {20}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(a, ha, 3, {30}).status, ReserveStatus::Committed);

    ASSERT_EQ(ReserveFrames(b, hb, 3, {30}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(b, hb, 1, {10}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(b, hb, 2, {20}).status, ReserveStatus::Committed);

    std::array<ConsumedEvent, 16> ea{};
    std::array<ConsumedEvent, 16> eb{};
    const std::size_t na =
        a.ConsumePage(0, kEpoch, ha.instances, ha.occurrences, ea.data(),
                      ea.size());
    const std::size_t nb =
        b.ConsumePage(0, kEpoch, hb.instances, hb.occurrences, eb.data(),
                      eb.size());
    ASSERT_EQ(na, 3u);
    ASSERT_EQ(nb, 3u);
    // Time order, not arrival order.
    EXPECT_EQ(ea[0].executeFrame, 10u);
    EXPECT_EQ(ea[1].executeFrame, 20u);
    EXPECT_EQ(ea[2].executeFrame, 30u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(ea[i].executeFrame, eb[i].executeFrame);
        EXPECT_EQ(ea[i].transactionId, eb[i].transactionId);
    }
    EXPECT_EQ(ConsumeDigest(a, 0, kEpoch, ha), ConsumeDigest(b, 0, kEpoch, hb));
}

TEST(ScheduledTransactionPool,
     AuthoredExecutionOrdinalWinsWhenEventIndexWouldInvert) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    std::array<ScheduledEventInput, 2> events{};
    events[0].executeFrame = 10;
    events[0].logicCommitSequence = 4;
    events[0].executionOrdinal = 1;
    events[0].eventIndex = 1;
    events[0].payload = 0xB;
    events[1].executeFrame = 10;
    events[1].logicCommitSequence = 4;
    events[1].executionOrdinal = 0;
    events[1].eventIndex = 5;
    events[1].payload = 0xA;

    TransactionRequest req;
    req.transactionId = 9;
    req.transportEpoch = kEpoch;
    req.ownerInstanceHandle = h.owner;
    req.ownerInstanceScheduleEpoch = h.ownerEpoch;
    req.events = events.data();
    req.eventCount = events.size();
    ASSERT_EQ(pool.Reserve(req, PrePublishLatePolicy::Refuse).status,
              ReserveStatus::Committed);

    std::array<ConsumedEvent, 4> consumed{};
    ASSERT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              2u);
    EXPECT_EQ(0u, consumed[0].executionOrdinal);
    EXPECT_EQ(5u, consumed[0].eventIndex);
    EXPECT_EQ(0xAu, consumed[0].payload);
    EXPECT_EQ(1u, consumed[1].executionOrdinal);
    EXPECT_EQ(1u, consumed[1].eventIndex);
    EXPECT_EQ(0xBu, consumed[1].payload);
}

// A far transaction filed before an earlier one still consumes in time order:
// insertion out of order does not disturb the result.
TEST(ScheduledTransactionPool, OutOfOrderInsertionStaysDeterministic) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {90, 10}).status,
              ReserveStatus::Committed); // filed late-then-early within page 0

    std::array<ConsumedEvent, 8> e{};
    const std::size_t n =
        pool.ConsumePage(0, kEpoch, h.instances, h.occurrences, e.data(),
                         e.size());
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(e[0].executeFrame, 10u);
    EXPECT_EQ(e[1].executeFrame, 90u);
}

TEST(ScheduledTransactionPool, EqualKeysPreserveOriginalRowSequenceInBothConsumers) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    std::array<ScheduledEventInput, 3> inputs{};
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
        inputs[i].executeFrame = 10;
        inputs[i].logicCommitSequence = 4;
        inputs[i].executionOrdinal = 2;
        inputs[i].eventIndex = 7;
        inputs[i].subeventPhase = 3;
        inputs[i].payload = 30 - i;
    }
    TransactionRequest request{};
    request.transactionId = 9;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = inputs.data();
    request.eventCount = static_cast<std::uint32_t>(inputs.size());
    ASSERT_EQ(ReserveStatus::Committed,
        pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
    std::array<ConsumedEvent, 3> logic{};
    std::array<ConsumedEvent, 3> audio{};
    ASSERT_EQ(3u, pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
        logic.data(), logic.size()));
    pool.Publish(100);
    ASSERT_EQ(3u, pool.ConsumePageRt(0, audio.data(), audio.size()));
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
        EXPECT_EQ(i + 1u, logic[i].rowSequence);
        EXPECT_EQ(logic[i].rowSequence, audio[i].rowSequence);
        EXPECT_EQ(inputs[i].payload, audio[i].payload);
    }
}

// All-or-nothing across pages: a transaction that would overflow one page's event
// ceiling is refused whole, and its events land on no page -- not even the pages
// that had room.
TEST(ScheduledTransactionPool, AllOrNothingAcrossPages) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxEventsPerQuantum = 4;
    c.maxFrameBatchesPerQuantum = 4; // cannot exceed the event ceiling
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));

    // Pre-fill page 0 with three events, leaving room for one.
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10, 20, 30}).status,
              ReserveStatus::Committed);

    // A transaction with two events in page 0 (would make five) and one in page 1.
    const ReserveResult spanning = ReserveFrames(pool, h, 2, {40, 50, 150});
    EXPECT_EQ(spanning.status, ReserveStatus::Refused);
    EXPECT_EQ(spanning.refusal, ReserveRefusal::PageEventCapacity);

    // Page 0 still holds only the original three; page 1 got nothing.
    std::size_t page0 = 0;
    std::size_t page1 = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &page0);
    ConsumeDigest(pool, 1, kEpoch, h, &page1);
    EXPECT_EQ(page0, 3u);
    EXPECT_EQ(page1, 0u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
}

// Late policy, reschedule: the whole transaction shifts so its earliest event
// lands on the frontier, its span preserved, and it commits at the new frames.
TEST(ScheduledTransactionPool, LateEarliestSafeShiftsWholeTransaction) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    pool.Publish(150); // frontier at 150; pages 0 (0..99) fully frozen

    // Earliest event at 40 is before the frontier; span is 60.
    const ReserveResult r =
        ReserveFrames(pool, h, 1, {40, 100},
                      PrePublishLatePolicy::ExecuteAtEarliestSafeFrame);
    EXPECT_EQ(r.status, ReserveStatus::Committed);
    EXPECT_TRUE(r.rescheduled);
    EXPECT_EQ(r.firstFrame, 150u);        // shifted up to the frontier
    EXPECT_EQ(r.lastFrame - r.firstFrame, 60u); // span unchanged
    EXPECT_EQ(r.lastFrame, 210u);
}

// Late policy, drop: nothing is written and the drop is counted.
TEST(ScheduledTransactionPool, LateDropCountsAndWritesNothing) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    pool.Publish(150);
    const ReserveResult r =
        ReserveFrames(pool, h, 1, {40}, PrePublishLatePolicy::Drop);
    EXPECT_EQ(r.status, ReserveStatus::DroppedLate);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
    EXPECT_EQ(pool.DroppedLateCount(), 1u);
}

// Late policy, refuse: nothing is written and the refusal is Late.
TEST(ScheduledTransactionPool, LateRefuseWritesNothing) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    pool.Publish(150);
    const ReserveResult r =
        ReserveFrames(pool, h, 1, {40}, PrePublishLatePolicy::Refuse);
    EXPECT_EQ(r.status, ReserveStatus::Refused);
    EXPECT_EQ(r.refusal, ReserveRefusal::Late);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
}

// Late policy, next-equivalent-boundary: the pool has no meter, so it hands the
// decision back to the planner rather than inventing a boundary.
TEST(ScheduledTransactionPool, NextEquivalentBoundaryDefersToPlanner) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    pool.Publish(150);
    const ReserveResult r =
        ReserveFrames(pool, h, 1, {40},
                      PrePublishLatePolicy::NextEquivalentBoundary);
    EXPECT_EQ(r.status, ReserveStatus::Refused);
    EXPECT_EQ(r.refusal, ReserveRefusal::NeedsBoundaryResolution);
}

// A published page is read-only: a reserve targeting it is refused and the page's
// contents and committed count are unchanged.
TEST(ScheduledTransactionPool, PublishedPageIsNotRewritten) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10, 20}).status,
              ReserveStatus::Committed);
    pool.Publish(100);
    const std::uint32_t committed = pool.Page(0)->committedTransactionCount;
    const std::uint64_t before = ConsumeDigest(pool, 0, kEpoch, h);

    const ReserveResult late =
        ReserveFrames(pool, h, 2, {30}, PrePublishLatePolicy::Refuse);
    EXPECT_EQ(late.refusal, ReserveRefusal::Late);
    EXPECT_EQ(pool.Page(0)->committedTransactionCount, committed);
    EXPECT_EQ(ConsumeDigest(pool, 0, kEpoch, h), before);
}

// A transport-epoch mismatch fails every transaction in one check: consuming with
// a different current epoch skips the whole page.
TEST(ScheduledTransactionPool, StaleNoOpOnEpochMismatch) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10, 20}).status,
              ReserveStatus::Committed);

    std::size_t sameEpoch = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &sameEpoch);
    EXPECT_EQ(sameEpoch, 2u);

    std::size_t otherEpoch = 0;
    ConsumeDigest(pool, 0, kEpoch + 1, h, &otherEpoch);
    EXPECT_EQ(otherEpoch, 0u);
}

// A reserve stamped with the reserved zero epoch, or with an epoch other than the
// pool's, is refused: the geometry it belongs to is not this one.
TEST(ScheduledTransactionPool, RejectsInvalidTransportEpochAtReserve) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    ScheduledEventInput e;
    e.executeFrame = 10;
    TransactionRequest req;
    req.transactionId = 1;
    req.ownerInstanceHandle = h.owner;
    req.ownerInstanceScheduleEpoch = h.ownerEpoch;
    req.events = &e;
    req.eventCount = 1;

    req.transportEpoch = harken::kInvalidTransportEpoch;
    EXPECT_EQ(pool.Reserve(req, PrePublishLatePolicy::Refuse).refusal,
              ReserveRefusal::InvalidTransportEpoch);
    req.transportEpoch = kEpoch + 5;
    EXPECT_EQ(pool.Reserve(req, PrePublishLatePolicy::Refuse).refusal,
              ReserveRefusal::InvalidTransportEpoch);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
}

// Cancelling one occurrence stops its transaction without touching a sibling
// occurrence bound to the same owner: the stale check validates the occurrence,
// not only the instance.
TEST(ScheduledTransactionPool, StaleNoOpOnOccurrenceCancelSparesSiblings) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    OccurrenceKey keyA{h.owner, /*clipId=*/harkentest::ObjectId(1), /*loop=*/0, /*ordinal=*/0};
    OccurrenceKey keyB{h.owner, /*clipId=*/harkentest::ObjectId(1), /*loop=*/0, /*ordinal=*/1};
    OccurrenceToken tokA{};
    OccurrenceToken tokB{};
    ASSERT_TRUE(h.occurrences.Acquire(keyA, &tokA));
    ASSERT_TRUE(h.occurrences.Acquire(keyB, &tokB));

    auto reserveOccurrence = [&](std::uint64_t id, const OccurrenceToken& tok,
                                 std::uint64_t frame) {
        ScheduledEventInput e;
        e.executeFrame = frame;
        e.eventIndex = 0;
        e.payload = id;
        TransactionRequest req;
        req.transactionId = id;
        req.transportEpoch = kEpoch;
        req.ownerInstanceHandle = h.owner;
        req.ownerInstanceScheduleEpoch = h.ownerEpoch;
        req.occurrence.present = true;
        req.occurrence.token = tok;
        req.occurrence.occurrenceScheduleEpoch =
            h.occurrences.ScheduleEpochOf(tok);
        req.events = &e;
        req.eventCount = 1;
        return pool.Reserve(req, PrePublishLatePolicy::Refuse);
    };

    ASSERT_EQ(reserveOccurrence(10, tokA, 10).status, ReserveStatus::Committed);
    ASSERT_EQ(reserveOccurrence(20, tokB, 20).status, ReserveStatus::Committed);

    std::size_t both = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &both);
    EXPECT_EQ(both, 2u);

    // Cancel occurrence A only.
    ASSERT_NE(h.occurrences.BumpScheduleEpoch(tokA), 0u);
    std::array<ConsumedEvent, 8> out{};
    const std::size_t n = pool.ConsumePage(0, kEpoch, h.instances,
                                           h.occurrences, out.data(),
                                           out.size());
    ASSERT_EQ(n, 1u); // sibling B survives
    EXPECT_EQ(out[0].payload, 20u);
}

TEST(ScheduledTransactionPool,
     OwnerCancelInvalidatesOccurrenceBoundTransaction) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    OccurrenceToken token{};
    ASSERT_TRUE(h.occurrences.Acquire(
        OccurrenceKey{h.owner, /*clipId=*/harkentest::ObjectId(1), /*loop=*/0, /*ordinal=*/0},
        &token));
    const std::uint64_t occurrenceEpoch =
        h.occurrences.ScheduleEpochOf(token);
    ASSERT_TRUE(h.occurrences.ValidateTransaction(
        token, occurrenceEpoch, h.ownerEpoch, h.instances));

    ScheduledEventInput event;
    event.executeFrame = 10;
    TransactionRequest request;
    request.transactionId = 1;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.occurrence.present = true;
    request.occurrence.token = token;
    request.occurrence.occurrenceScheduleEpoch = occurrenceEpoch;
    request.events = &event;
    request.eventCount = 1;
    ASSERT_EQ(pool.Reserve(request, PrePublishLatePolicy::Refuse).status,
              ReserveStatus::Committed);

    ASSERT_NE(h.instances.BumpScheduleEpoch(h.owner), 0u);
    std::array<ConsumedEvent, 4> out{};
    EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               out.data(), out.size()),
              0u);
}

TEST(ScheduledTransactionPool,
     OccurrenceBindingCannotMaskInvalidOwnerStamp) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    const PlayableInstanceHandle arranger =
        h.instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(arranger, 0u);
    const std::uint64_t arrangerEpoch =
        h.instances.Classify(arranger).scheduleEpoch;
    ASSERT_FALSE(h.instances.ValidateStamp(h.owner, arrangerEpoch));

    OccurrenceToken token{};
    ASSERT_TRUE(h.occurrences.Acquire(
        OccurrenceKey{arranger, /*clipId=*/harkentest::ObjectId(1), /*loop=*/0, /*ordinal=*/0},
        &token));
    const std::uint64_t occurrenceEpoch =
        h.occurrences.ScheduleEpochOf(token);
    ASSERT_TRUE(h.occurrences.ValidateTransaction(
        token, occurrenceEpoch, arrangerEpoch, h.instances));

    ScheduledEventInput event;
    event.executeFrame = 10;
    TransactionRequest request;
    request.transactionId = 1;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = arrangerEpoch;
    request.occurrence.present = true;
    request.occurrence.token = token;
    request.occurrence.occurrenceScheduleEpoch = occurrenceEpoch;
    request.events = &event;
    request.eventCount = 1;
    ASSERT_EQ(pool.Reserve(request, PrePublishLatePolicy::Refuse).status,
              ReserveStatus::Committed);

    std::array<ConsumedEvent, 4> out{};
    EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               out.data(), out.size()),
              0u);
}

TEST(ScheduledTransactionPool, SplitOccurrenceStampsSurviveBothReservationPaths) {
    for (const bool prepared : {false, true}) {
        SCOPED_TRACE(prepared);
        Harness h;
        const auto successor = h.instances.Acquire(PlayableKind::Sound, h.owner);
        const auto successorEpoch = h.instances.Classify(successor).scheduleEpoch;
        ASSERT_NE(successorEpoch, h.ownerEpoch);
        OccurrenceToken token{};
        ASSERT_TRUE(h.occurrences.Acquire({h.owner, harkentest::ObjectId(1), 0, 0}, &token));
        harken::TimelineArmTable arms(1);
        harken::TimelineArmToken arm{};
        ASSERT_TRUE(arms.Reserve(0, 1, 10, 10, 1, {}, &arm));
        ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        ScheduledEventInput event{};
        event.executeFrame = 10;
        event.armToken = arm;
        TransactionRequest request{};
        request.transactionId = 1;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = successor;
        request.ownerInstanceScheduleEpoch = successorEpoch;
        request.occurrence = {true, token, h.occurrences.ScheduleEpochOf(token),
                              h.ownerEpoch};
        request.events = &event;
        request.eventCount = 1;
        if (prepared) {
            PreparedTransactionBatch batch;
            ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &batch),
                      ReserveRefusal::None);
            ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&batch, 1));
        } else {
            ASSERT_EQ(pool.Reserve(request, PrePublishLatePolicy::Refuse).status,
                      ReserveStatus::Committed);
        }
        std::array<ConsumedEvent, 2> out{};
        EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                                  out.data(), out.size()), 1u);
        pool.Publish(100, &h.instances, &h.occurrences, &arms);
        ASSERT_EQ(pool.ConsumePageRt(0, out.data(), out.size()), 1u);
        EXPECT_TRUE(out[0].frozenTxnValid);
        harken::TimelineArmObservation observed{};
        ASSERT_TRUE(arms.Observe(arm, &observed));
        EXPECT_EQ(observed.state, harken::ArmState::Armed);
        // RT retains the publication decision; logic consume rechecks both owners.
        ASSERT_NE(h.instances.BumpScheduleEpoch(h.owner), 0u);
        ASSERT_EQ(pool.ConsumePageRt(0, out.data(), out.size()), 1u);
        EXPECT_TRUE(out[0].frozenTxnValid);
        EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                                  out.data(), out.size()), 0u);
    }
}

TEST(ScheduledTransactionPool, SplitOccurrenceRejectsIndependentInvalidationAndReuse) {
    for (const bool publishFirst : {false, true}) {
        for (const unsigned invalidation : {0u, 1u, 2u, 3u, 4u, 5u}) {
            SCOPED_TRACE(publishFirst);
            SCOPED_TRACE(invalidation);
            PlayableInstanceTable instances(2);
            OccurrenceStatePool occurrences(2);
            auto arranger = instances.Acquire(PlayableKind::Container, 0);
            auto successor = instances.Acquire(PlayableKind::Sound, 0);
            const auto arrangerEpoch = instances.Classify(arranger).scheduleEpoch;
            const auto successorEpoch = instances.Classify(successor).scheduleEpoch;
            OccurrenceToken token{};
            ASSERT_TRUE(occurrences.Acquire({arranger, harkentest::ObjectId(1), 0, 0}, &token));
            ScheduledTransactionPool pool;
            ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
            ScheduledEventInput event{};
            event.executeFrame = 10;
            TransactionRequest request{};
            request.transactionId = 1;
            request.transportEpoch = kEpoch;
            request.ownerInstanceHandle = successor;
            request.ownerInstanceScheduleEpoch = successorEpoch;
            request.occurrence = {true, token, occurrences.ScheduleEpochOf(token),
                                  arrangerEpoch};
            request.events = &event;
            request.eventCount = 1;
            ASSERT_EQ(pool.Reserve(request, PrePublishLatePolicy::Refuse).status,
                      ReserveStatus::Committed);
            if (publishFirst) pool.Publish(100, &instances, &occurrences);
            if (invalidation < 2) {
                ASSERT_NE(instances.BumpScheduleEpoch(
                    invalidation == 0 ? arranger : successor), 0u);
            } else if (invalidation == 2) {
                ASSERT_NE(occurrences.BumpScheduleEpoch(token), 0u);
            } else if (invalidation < 5) {
                auto& handle = invalidation == 3 ? arranger : successor;
                const auto stale = handle;
                ASSERT_TRUE(instances.AdvanceLifecycle(
                    stale, harken::PlayableLifecycle::Retired));
                handle = instances.Acquire(invalidation == 3
                    ? PlayableKind::Container : PlayableKind::Sound, 0);
                ASSERT_NE(handle, stale);
                EXPECT_EQ(static_cast<std::uint32_t>(handle),
                          static_cast<std::uint32_t>(stale));
            } else {
                const auto stale = token;
                ASSERT_TRUE(occurrences.Advance(
                    stale, harken::OccurrenceStage::AwaitingEventFence));
                ASSERT_TRUE(occurrences.ReleaseAfterFence(stale));
                ASSERT_TRUE(occurrences.Acquire({arranger, harkentest::ObjectId(1), 0, 0}, &token));
                EXPECT_EQ(token.slot, stale.slot);
                EXPECT_NE(token.generation, stale.generation);
            }
            std::array<ConsumedEvent, 4> out{};
            EXPECT_EQ(pool.ConsumePage(0, kEpoch, instances, occurrences,
                                      out.data(), out.size()), 0u);
            if (!publishFirst) {
                pool.Publish(100, &instances, &occurrences);
                ASSERT_EQ(pool.ConsumePageRt(0, out.data(), out.size()), 1u);
                EXPECT_FALSE(out[0].frozenTxnValid);
            }
            // A fresh pair passes beside the stale transaction in the same pool.
            OccurrenceToken fresh{};
            ASSERT_TRUE(occurrences.Acquire({arranger, harkentest::ObjectId(2), 0, 0}, &fresh));
            request.transactionId = 2;
            request.ownerInstanceHandle = successor;
            request.ownerInstanceScheduleEpoch =
                instances.Classify(successor).scheduleEpoch;
            request.occurrence = {true, fresh, occurrences.ScheduleEpochOf(fresh),
                                  instances.Classify(arranger).scheduleEpoch};
            event.executeFrame = 110;
            ASSERT_EQ(pool.Reserve(request, PrePublishLatePolicy::Refuse).status,
                      ReserveStatus::Committed);
            pool.Publish(200, &instances, &occurrences);
            EXPECT_EQ(pool.ConsumePage(1, kEpoch, instances, occurrences,
                                      out.data(), out.size()), 1u);
            ASSERT_EQ(pool.ConsumePageRt(1, out.data(), out.size()), 1u);
            EXPECT_TRUE(out[0].frozenTxnValid);
        }
    }
}

TEST(ScheduledTransactionPool, OccurrenceStampCompatibilityIsRestrictedToSameOwner) {
    for (const unsigned variant : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
        SCOPED_TRACE(variant);
        Harness h;
        auto arranger = h.owner;
        if (variant == 1 || variant == 2) {
            if (variant == 2) {
                harken::detail::PlayableInstanceTableTestAccess::SeedNextScheduleEpoch(
                    h.instances, h.ownerEpoch);
            }
            arranger = h.instances.Acquire(PlayableKind::Container, 0);
            ASSERT_NE(arranger, h.owner);
            if (variant == 2) {
                ASSERT_EQ(h.instances.Classify(arranger).scheduleEpoch, h.ownerEpoch);
            }
        }
        OccurrenceToken token{};
        ASSERT_TRUE(h.occurrences.Acquire({arranger, harkentest::ObjectId(1), 0, 0}, &token));
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
        ScheduledEventInput event{};
        event.executeFrame = 10;
        TransactionRequest request{};
        request.transactionId = 1;
        request.transportEpoch = kEpoch;
        request.ownerInstanceHandle = h.owner;
        request.ownerInstanceScheduleEpoch = h.ownerEpoch;
        request.occurrence = {true, token, h.occurrences.ScheduleEpochOf(token)};
        if (variant == 3) request.occurrence.arrangingInstanceScheduleEpoch = UINT64_MAX;
        if (variant == 4 || variant == 5) request.occurrence.token = {};
        if (variant == 5) request.occurrence.present = false;
        if (variant == 6) {
            request.occurrence.arrangingInstanceScheduleEpoch = h.ownerEpoch;
            ++request.occurrence.token.generation;
        }
        request.events = &event;
        request.eventCount = 1;
        ASSERT_EQ(pool.Reserve(request, PrePublishLatePolicy::Refuse).status,
                  ReserveStatus::Committed);
        const auto expected = variant == 0 || variant == 5 ? 1u : 0u;
        std::array<ConsumedEvent, 2> out{};
        EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                                  out.data(), out.size()), expected);
        pool.Publish(100, &h.instances, &h.occurrences);
        ASSERT_EQ(pool.ConsumePageRt(0, out.data(), out.size()), 1u);
        EXPECT_EQ(out[0].frozenTxnValid, expected != 0);
    }
}

// A transaction whose events exceed the per-transaction event ceiling is refused
// whole.
TEST(ScheduledTransactionPool, RefusesTransactionOverItsEventCeiling) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxTransactionEvents = 3;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));
    const ReserveResult r = ReserveFrames(pool, h, 1, {10, 20, 30, 40});
    EXPECT_EQ(r.refusal, ReserveRefusal::TransactionEventsExceeded);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
}

// A transaction spanning more frames than the ceiling allows is refused whole.
TEST(ScheduledTransactionPool, RefusesTransactionOverItsSpanCeiling) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxTransactionSpanFrames = 50;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));
    const ReserveResult r = ReserveFrames(pool, h, 1, {10, 90}); // span 80
    EXPECT_EQ(r.refusal, ReserveRefusal::SpanExceeded);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
}

// Admission per quantum is bounded and resets when the window advances: the
// ceiling turns away the over-rate transaction, and after an advance the budget is
// fresh.
TEST(ScheduledTransactionPool, IngressRateIsBoundedPerQuantumAndResetsOnAdvance) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxIngressTransactionsPerQuantum = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));

    ASSERT_EQ(ReserveFrames(pool, h, 1, {10}).status, ReserveStatus::Committed);
    ASSERT_EQ(ReserveFrames(pool, h, 2, {110}).status, ReserveStatus::Committed);
    const ReserveResult over = ReserveFrames(pool, h, 3, {210});
    EXPECT_EQ(over.refusal, ReserveRefusal::IngressRateExceeded);
    EXPECT_EQ(pool.IngressThisQuantum(), 2u);
    EXPECT_EQ(pool.IngressHighWater(), 2u);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::IngressRateExceeded), 1u);

    // Advancing the window opens a fresh per-quantum budget.
    pool.Advance();
    EXPECT_EQ(pool.IngressThisQuantum(), 0u);
    EXPECT_EQ(ReserveFrames(pool, h, 4, {210}).status, ReserveStatus::Committed);
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
struct PrivatePageBatchTransferHarness {
    Harness identities;
    ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{1};
    harken::DecisionTable decisions{3, 1};
    std::array<harken::DecisionToken, 3> tokens{};
    std::array<harken::Plan03GroupSliceInput, 3> groupInputs{};
    std::array<harken::PreparedPlan03GroupSlice, 3> groups{};
    std::array<harken::Plan03GroupSliceIdentity, 3> groupIds{};
    harken::PreparedPageGroupTransferScope scope{810, 53, 7, 11};
    harken::TimelineEventPayload payload{};
    harken::TimelineProgramView program{71, &payload, 1};
    std::array<ScheduledEventInput, 5> events{};
    std::array<TransactionRequest, 5> requests{};
    std::array<ConsumedEvent, 5> actual{};
    std::array<harken::FrontierEventBinding, 5> bindings{};
    PreparedTransactionBatch original;
    harken::PreparedPageBatchTransferProof proof;

    void Prepare(bool terminalLeavingPage = false, std::uint32_t ingress = 16,
        bool maximumBatchGeneration = false, std::uint64_t startFrame = 0) {
        auto config = MakeConfig();
        config.maxScheduledTransactions = maximumBatchGeneration ? 5 : 32;
        config.maxIngressTransactionsPerQuantum = ingress;
        config.maxEventsPerQuantum = 32;
        config.maxPlan03GroupHeadersPerPage = 3;
        config.maxArmsPerGroup = 1;
        ASSERT_TRUE(pool.Reset(config, startFrame, kEpoch));
        if (terminalLeavingPage)
            ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetEmptyPageGeneration(
                pool, 0, UINT64_MAX));
        if (maximumBatchGeneration)
            ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetFreeBatchGenerations(
                pool, UINT32_MAX));
        for (std::size_t index = 0; index < groups.size(); ++index) {
            harken::DecisionReserveRequest request{};
            request.ticket.sequence = scope.sequence;
            request.world = scope.world;
            request.transport = kEpoch;
            request.expectedContinuationGeneration = 1;
            request.sliceCount = 1;
            ASSERT_TRUE(decisions.Reserve(request, &tokens[index]));
            groupInputs[index].executeFrame = startFrame + 210;
            groupInputs[index].decision = tokens[index];
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                pool.PreparePlan03GroupSlice(groupInputs[index], arms, &groups[index], &scope));
            ASSERT_TRUE(groups[index].CaptureProspectiveIdentity(&groupIds[index]));
        }
        harken::FrontierOwnerIdentity owner{};
        owner.child = identities.owner;
        owner.childEpoch = identities.ownerEpoch;
        owner.invocation = identities.owner;
        owner.invocationEpoch = identities.ownerEpoch;
        owner.serviceSlot = 0;
        owner.serviceGeneration = 1;
        const std::array<std::size_t, 5> placement{2, 0, 1, 0, 1};
        for (std::uint32_t index = 0; index < requests.size(); ++index) {
            events[index].executeFrame = startFrame + 210;
            events[index].logicCommitSequence = scope.sequence;
            events[index].eventIndex = index;
            events[index].transactionOrdinal = index;
            events[index].payload = 0;
            requests[index].transactionId = scope.sequence;
            requests[index].transportEpoch = kEpoch;
            requests[index].ownerInstanceHandle = identities.owner;
            requests[index].ownerInstanceScheduleEpoch = identities.ownerEpoch;
            requests[index].program = {program.generation, &program};
            requests[index].events = &events[index];
            requests[index].eventCount = 1;
            auto& binding = bindings[index];
            binding.kind = harken::FrontierOperationKind::Entry;
            binding.entry = {tokens[2], scope.attemptGeneration, owner};
            binding.owner = owner;
            binding.group = groupIds[placement[index]];
            binding.transport = kEpoch;
            binding.world = scope.world;
            binding.operationEventCount = 1;
            binding.entryEventOrdinal = index;
        }
        ASSERT_NO_FATAL_FAILURE(PrepareBatch());
    }
    void PrepareBatch() {
        ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(
            requests.data(), requests.size(), 0, &original, &scope));
        ASSERT_TRUE(pool.FreezePreparedTransactionBatch(original, scope.sequence));
        for (std::uint32_t index = 0; index < actual.size(); ++index)
            ASSERT_TRUE(pool.CapturePreparedTransactionEvent(original, index, 0, &actual[index]));
    }
    bool Prove() {
        return pool.ProvePreparedPageBatchTransfer(original, scope, actual, bindings, &proof);
    }
    void CheckKeys(const PreparedTransactionBatch& batch, bool committed = false) {
        for (std::uint32_t index = 0; index < actual.size(); ++index) {
            ConsumedEvent row{};
            ASSERT_TRUE(pool.CapturePreparedTransactionEvent(batch, index, 0, &row));
            EXPECT_EQ(actual[index].txnSlot, row.txnSlot);
            EXPECT_EQ(actual[index].rowSequence, row.rowSequence);
            EXPECT_EQ(actual[index].logicCommitSequence, row.logicCommitSequence);
            EXPECT_EQ(actual[index].program.program, row.program.program);
            EXPECT_EQ(actual[index].program.programGeneration, row.program.programGeneration);
            if (committed) EXPECT_EQ(bindings[index], row.frontier);
            else EXPECT_FALSE(row.frontier.Present());
        }
    }
    void Reclaim() {
        proof.Release();
        if (original.Valid()) ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&original));
        for (std::size_t index = 0; index < groups.size(); ++index) {
            if (groups[index].Valid()) ASSERT_TRUE(pool.CancelPreparedPlan03GroupSlice(&groups[index]));
            ASSERT_TRUE(decisions.Cancel(tokens[index]));
            ASSERT_TRUE(decisions.ReclaimCancelled(tokens[index]));
        }
        const auto state = pool.CapturePrivateMaterializationSnapshotForTest();
        EXPECT_EQ(0u, state.preparedEvents);
        EXPECT_EQ(0u, state.preparedGroups);
        EXPECT_EQ(0u, state.publicationHolds);
        EXPECT_EQ(0u, pool.LiveTransactionCount());
        EXPECT_EQ(0u, decisions.LiveCount());
        EXPECT_TRUE(pool.ResetForEpoch(pool.WindowStartFrame(), kEpoch + 1));
    }
};

struct PrivatePageFrontierBindingHarness {
    struct Cohort {
        harken::PreparedPageGroupTransferScope scope{};
        std::array<harken::DecisionToken, 3> decisions{};
        std::array<harken::TimelineArmToken, 2> members{};
        std::array<harken::Plan03GroupSliceInput, 3> inputs{};
        std::array<harken::PreparedPlan03GroupSlice, 3> originals{};
        std::array<harken::PreparedPlan03GroupSlice*, 3> originalPointers{};
        std::array<harken::PreparedPageGroupTransferProof, 3> transfers{};
        std::array<harken::Plan03GroupSliceIdentity, 3> identities{};
        std::array<harken::CommittedPlan03GroupSlice, 3> outputs{};
        std::array<harken::CommittedPlan03GroupSlice*, 3> outputPointers{};
        std::array<harken::AudioSliceLease*, 3> leases{};
        harken::TimelineEventPayload payload{};
        std::array<harken::TimelineProgramView, 5> programs{};
        std::array<ScheduledEventInput, 5> events{};
        std::array<TransactionRequest, 5> requests{};
        std::array<ConsumedEvent, 5> actual{};
        std::array<harken::FrontierEventBinding, 5> eventBindings{};
        std::array<harken::FrontierGroupBinding, 3> groupBindings{};
        PreparedTransactionBatch batch;
        harken::PreparedPageBatchTransferProof batchProof;
        harken::PreparedPageFrontierBindingsProof bindingProof;
    };
    Harness owners;
    std::unique_ptr<ScheduledTransactionPool> pool = std::make_unique<ScheduledTransactionPool>();
    harken::TimelineArmTable arms{4};
    harken::DecisionTable decisions{6, 2};
    std::array<Cohort, 2> cohorts{};

    void Reset(bool leavingMaximumPage = false) {
        auto config = MakeConfig();
        config.maxScheduledTransactions = 32;
        config.maxIngressTransactionsPerQuantum = 16;
        config.maxEventsPerQuantum = 16;
        config.maxPlan03GroupHeadersPerPage = 6;
        config.maxArmsPerGroup = 1;
        ASSERT_TRUE(pool->Reset(config, 0, kEpoch));
        if (leavingMaximumPage)
            ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetEmptyPageGeneration(*pool, 0, UINT64_MAX));
    }
    void Prepare(unsigned cohortIndex = 0, bool maximumClaims = false) {
        auto& c = cohorts[cohortIndex];
        c.scope = {810u + cohortIndex, 53u + cohortIndex, 7u, 11u + cohortIndex};
        for (unsigned i = 0; i < 3; ++i) {
            harken::DecisionReserveRequest request{};
            request.ticket.sequence = c.scope.sequence;
            request.world = c.scope.world;
            request.transport = kEpoch;
            request.expectedContinuationGeneration = 1;
            request.sliceCount = 1;
            ASSERT_TRUE(decisions.Reserve(request, &c.decisions[i]));
            c.inputs[i] = {};
            c.inputs[i].executeFrame = 210;
            c.inputs[i].decision = c.decisions[i];
            if (i < 2) {
                ASSERT_TRUE(arms.PreparePendingImmediateGrouped(cohortIndex * 2u + i + 1u,
                    3, c.scope.attemptGeneration, {}, harken::ParentAudioGroupRef{c.decisions[i]}, 0, &c.members[i]));
                c.inputs[i].members = &c.members[i];
                c.inputs[i].memberCount = 1;
            }
            if (maximumClaims) {
                const auto row = harken::detail::ScheduledTransactionPoolCancelTestAccess::Ring(*pool, 210) * 6u + i;
                ASSERT_TRUE(harken::detail::ScheduledTransactionPoolCancelTestAccess::SetFreePageGroupGeneration(*pool, row, UINT32_MAX));
            }
            c.originalPointers[i] = &c.originals[i];
            c.outputPointers[i] = &c.outputs[i];
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                pool->PreparePlan03GroupSlice(c.inputs[i], arms, c.originalPointers[i], &c.scope));
            ASSERT_TRUE(c.originals[i].CaptureProspectiveIdentity(&c.identities[i]));
            c.leases[i] = harken::detail::ScheduledTransactionPoolCancelTestAccess::BorrowPageGroupLease(*pool, c.identities[i]);
            ASSERT_NE(nullptr, c.leases[i]);
        }
        harken::FrontierOwnerIdentity owner{};
        owner.child = owner.invocation = owners.owner;
        owner.childEpoch = owner.invocationEpoch = owners.ownerEpoch;
        owner.serviceSlot = cohortIndex;
        owner.serviceGeneration = 1;
        const std::array<unsigned, 5> placement{2, 0, 1, 0, 1};
        for (unsigned event = 0; event < 5; ++event) {
            c.programs[event] = {c.scope.attemptGeneration, &c.payload, 1};
            c.events[event] = {};
            c.events[event].executeFrame = 210;
            c.events[event].logicCommitSequence = c.scope.sequence;
            c.events[event].eventIndex = event;
            c.events[event].transactionOrdinal = event;
            if (event >= 3) c.events[event].armToken = c.members[event - 3u];
            c.requests[event] = {};
            c.requests[event].transactionId = c.scope.sequence;
            c.requests[event].transportEpoch = kEpoch;
            c.requests[event].ownerInstanceHandle = owners.owner;
            c.requests[event].ownerInstanceScheduleEpoch = owners.ownerEpoch;
            c.requests[event].program = {c.scope.attemptGeneration, &c.programs[event]};
            c.requests[event].armToken = c.events[event].armToken;
            c.requests[event].events = &c.events[event];
            c.requests[event].eventCount = 1;
            auto& binding = c.eventBindings[event];
            binding = {};
            binding.kind = harken::FrontierOperationKind::Entry;
            binding.entry = {c.decisions[2], c.scope.attemptGeneration, owner};
            binding.owner = owner;
            binding.group = c.identities[placement[event]];
            binding.world = c.scope.world;
            binding.transport = kEpoch;
            binding.operationEventCount = 1;
            binding.entryEventOrdinal = event;
        }
        ASSERT_EQ(ReserveRefusal::None, pool->PrepareTransactionBatch(c.requests.data(), 5, 0, &c.batch, &c.scope));
        ASSERT_TRUE(pool->FreezePreparedTransactionBatch(c.batch, c.scope.sequence));
        for (unsigned event = 0; event < 5; ++event)
            ASSERT_TRUE(pool->CapturePreparedTransactionEvent(c.batch, event, 0, &c.actual[event]));
        ASSERT_TRUE(pool->ProvePreparedPageBatchTransfer(c.batch, c.scope, c.actual, c.eventBindings, &c.batchProof));
        for (unsigned i = 0; i < 3; ++i) {
            const auto event = i == 2 ? 0u : i + 1u;
            const auto& actual = c.actual[event];
            auto& binding = c.groupBindings[i];
            binding = {};
            binding.role = i == 2 ? harken::FrontierGroupRole::Coordinator : harken::FrontierGroupRole::EntryMember;
            binding.entry = c.eventBindings[event].entry;
            binding.primary = c.identities[2];
            binding.operation.kind = harken::FrontierOperationKind::Entry;
            binding.operation.owner = owner;
            binding.operation.group = c.identities[i];
            binding.operation.transport = kEpoch;
            binding.operation.world = c.scope.world;
            binding.operation.order = {actual.executeFrame, actual.logicCommitSequence, actual.executionOrdinal,
                actual.eventIndex, actual.transactionOrdinal, actual.subeventPhase, actual.rowSequence};
            if (i < 2) {
                binding.operation.program = actual.program.program;
                binding.operation.programGeneration = actual.program.programGeneration;
            }
        }
        ASSERT_NO_FATAL_FAILURE(ProveTransfers(cohortIndex));
    }
    void ProveTransfers(unsigned index = 0) {
        auto& c = cohorts[index];
        for (unsigned i = 0; i < 3; ++i)
            if (!c.transfers[i].Valid())
                ASSERT_TRUE(pool->ProvePreparedPageGroupTransfer(*c.originalPointers[i], c.scope,
                    c.inputs[i], arms, &c.transfers[i]));
    }
    bool Prove(unsigned index = 0, harken::PreparedPageFrontierBindingsProof* output = nullptr) {
        auto& c = cohorts[index];
        return pool->ProvePreparedPageFrontierBindings({&c.transfers[0], &c.transfers[1], &c.transfers[2]},
            c.batchProof, c.groupBindings, c.outputPointers, output == nullptr ? &c.bindingProof : output);
    }
    void Mint(unsigned i, unsigned index = 0) {
        auto& c = cohorts[index];
        ScheduledTransactionPool::ConsumePreparedPageGroupIntoFrontierBindings(
            std::move(c.transfers[i]), c.bindingProof, *c.outputPointers[i]);
        EXPECT_TRUE(c.bindingProof.Valid());
        EXPECT_FALSE(c.originals[i].Valid());
        EXPECT_FALSE(c.transfers[i].Valid());
        EXPECT_TRUE(c.outputPointers[i]->Valid());
        EXPECT_TRUE(c.outputPointers[i]->Held());
        EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, c.leases[i]->Capture().state);
    }
    void Reclaim(unsigned index = 0, bool reset = true) {
        auto& c = cohorts[index];
        c.bindingProof.Release();
        c.batchProof.Release();
        if (c.batch.Valid()) ASSERT_TRUE(pool->CancelPreparedTransactionBatch(&c.batch));
        for (unsigned i = 0; i < 3; ++i) {
            if (c.outputs[i].Valid()) {
                harken::Plan03PageGroupView view{};
                ASSERT_TRUE(pool->ResolvePlan03GroupSliceProducer(c.identities[i], &view));
                if (view.frontier != nullptr) ASSERT_TRUE(pool->DetachPlan03GroupFrontier(c.identities[i], 0));
                ASSERT_TRUE(pool->RollbackCommittedPlan03GroupSlice(&c.outputs[i]));
            }
            c.transfers[i].Release();
            if (c.originals[i].Valid()) ASSERT_TRUE(pool->CancelPreparedPlan03GroupSlice(&c.originals[i]));
        }
        for (const auto& member : c.members) ASSERT_TRUE(arms.CancelPrepared(member));
        for (const auto& decision : c.decisions) {
            ASSERT_TRUE(decisions.Cancel(decision));
            ASSERT_TRUE(decisions.ReclaimCancelled(decision));
        }
        if (reset) {
            EXPECT_EQ(0u, pool->CaptureLivePlan03Groups(nullptr, 0));
            const auto snapshot = pool->CapturePrivateMaterializationSnapshotForTest();
            EXPECT_EQ(0u, snapshot.preparedGroups);
            EXPECT_EQ(0u, snapshot.preparedEvents);
            EXPECT_EQ(0u, snapshot.publicationHolds);
            EXPECT_TRUE(pool->ResetForEpoch(0, kEpoch + 1));
        }
    }
};

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsTrueMintAndUniqueBindingOwnExactReaders) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset());
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    auto& c = h.cohorts[0];
    ASSERT_TRUE(h.Prove());
    EXPECT_FALSE(h.Prove());
    EXPECT_TRUE(c.bindingProof.Valid());
    for (unsigned i = 0; i < 3; ++i) ASSERT_NO_FATAL_FAILURE(h.Mint(i));
    ScheduledTransactionPool::ConsumePreparedPageFrontierBindings(std::move(c.bindingProof));
    EXPECT_FALSE(c.bindingProof.Valid());
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_FALSE(c.outputs[i].Held());
        harken::Plan03ExactGroupSnapshot snapshot{};
        ASSERT_TRUE(h.pool->CaptureExactPlan03Group(c.identities[i], &snapshot));
        EXPECT_EQ(i == 2 ? 3u : 1u, snapshot.readerPins);
        EXPECT_EQ(i == 2 ? 2u : 0u, snapshot.frontierDependents);
        EXPECT_TRUE(snapshot.frontierPinsOwned);
        EXPECT_EQ(harken::AudioSliceState::Ready, c.leases[i]->Capture().state);
        EXPECT_EQ(c.groupBindings[i].role, snapshot.frontier.role);
        EXPECT_EQ(c.groupBindings[i].entry, snapshot.frontier.entry);
        EXPECT_EQ(c.groupBindings[i].operation.order, snapshot.frontier.operation.order);
        EXPECT_EQ(c.groupBindings[i].operation.program, snapshot.frontier.operation.program);
    }
    harken::FrontierActivationDescriptor descriptor{};
    descriptor.entry = c.groupBindings[2].entry;
    descriptor.primary = c.identities[2];
    descriptor.order = c.groupBindings[2].operation.order;
    descriptor.worldGeneration = c.scope.world;
    descriptor.worldSemanticFingerprint = 91;
    descriptor.expectedMemberCount = 2;
    descriptor.expectedEntryEventCount = 5;
    descriptor.programGeneration = c.scope.attemptGeneration;
    ASSERT_TRUE(h.decisions.PrepareFrontierActivation(c.decisions[2], descriptor, *h.pool));
    for (unsigned i = 0; i < 2; ++i) {
        harken::Plan03PageGroupView group{};
        ASSERT_TRUE(h.pool->ResolvePlan03GroupSliceProducer(c.identities[i], &group));
        ASSERT_NE(nullptr, group.frontier);
        ASSERT_TRUE(h.decisions.BindFrontierDecision(c.decisions[i], *group.frontier, *h.pool));
    }
    for (unsigned i = 0; i < 2; ++i) {
        ASSERT_TRUE(h.pool->DetachPlan03GroupFrontier(c.identities[i], 0));
        ASSERT_TRUE(h.decisions.ReleaseFrontierDecisionBinding(c.decisions[i]));
    }
    ASSERT_TRUE(h.decisions.CancelPreparedFrontierActivation(c.decisions[2]));
    c.batch = ScheduledTransactionPool::ConsumePreparedPageBatchTransfer(std::move(c.batchProof));
    EXPECT_TRUE(c.batch.Valid());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsPartialMintAbortKeepsRealOwnersAndUnmintedOriginal) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset());
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    auto& c = h.cohorts[0];
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.Mint(0));
    ASSERT_NO_FATAL_FAILURE(h.Mint(1));
    EXPECT_TRUE(c.originals[2].Valid());
    EXPECT_TRUE(c.originals[2].Held());
    EXPECT_FALSE(c.outputs[2].Valid());
    EXPECT_TRUE(c.outputs[2].Held());
    const auto snapshot = h.pool->CapturePrivateMaterializationSnapshotForTest();
    EXPECT_FALSE(h.pool->RollbackCommittedPlan03GroupSlice(&c.outputs[0]));
    EXPECT_FALSE(h.pool->ReleaseCommittedPlan03GroupSliceOwnership(&c.outputs[1]));
    EXPECT_FALSE(h.pool->CancelPreparedPlan03GroupSlice(&c.originals[2]));
    EXPECT_FALSE(h.pool->BindPlan03GroupFrontier(c.identities[0], c.groupBindings[0]));
    harken::Plan03GroupReaderPin pin;
    EXPECT_FALSE(h.pool->AcquirePlan03GroupReader(c.identities[0], &pin));
    h.pool->DropPreparedOccupancy();
    EXPECT_EQ(snapshot.preparedGroups, h.pool->CapturePrivateMaterializationSnapshotForTest().preparedGroups);
    EXPECT_EQ(snapshot.publicationHolds, h.pool->CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    EXPECT_FALSE(h.pool->CanResetForEpoch(0, kEpoch + 1));
    EXPECT_EQ(0u, h.pool->Advance());
    for (auto* lease : c.leases) {
        EXPECT_FALSE(lease->TryClaim());
        EXPECT_EQ(harken::AudioSliceInvalidateDisposition::PrivateTransferHeld,
            lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    }
    c.bindingProof.Release();
    EXPECT_TRUE(c.outputs[0].Valid());
    EXPECT_TRUE(c.outputs[1].Valid());
    EXPECT_TRUE(c.originals[2].Valid());
    EXPECT_FALSE(c.originals[2].Held());
    for (auto* lease : c.leases) EXPECT_EQ(harken::AudioSliceState::Ready, lease->Capture().state);
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsRefusalPreservesOriginalGrantsAndReproves) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset());
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    auto& c = h.cohorts[0];
    c.transfers[1].Release();
    EXPECT_FALSE(h.Prove());
    EXPECT_TRUE(c.transfers[0].Valid());
    EXPECT_TRUE(c.transfers[2].Valid());
    for (const auto& output : c.outputs) EXPECT_FALSE(output.Held());
    ASSERT_NO_FATAL_FAILURE(h.ProveTransfers());
    const auto originalBindings = c.groupBindings;
    for (unsigned mismatch = 0; mismatch < 8; ++mismatch) {
        c.groupBindings = originalBindings;
        switch (mismatch) {
        case 0: ++c.groupBindings[0].operation.order.rowSequence; break;
        case 1: ++c.groupBindings[1].operation.programGeneration; break;
        case 2: c.groupBindings[2].operation.program = c.actual[0].program.program; break;
        case 3: ++c.groupBindings[0].entry.attempt; break;
        case 4: ++c.groupBindings[1].operation.group.rowClaimGeneration; break;
        case 5: ++c.groupBindings[2].primary.pageGeneration; break;
        case 6: ++c.groupBindings[0].operation.owner.serviceGeneration; break;
        case 7: ++c.groupBindings[1].operation.transport; break;
        }
        EXPECT_FALSE(h.Prove()) << mismatch;
        for (const auto& transfer : c.transfers) EXPECT_TRUE(transfer.Valid());
        for (const auto& output : c.outputs) EXPECT_FALSE(output.Held());
    }
    c.groupBindings = originalBindings;
    c.outputPointers[1] = c.outputPointers[0];
    EXPECT_FALSE(h.Prove());
    c.outputPointers[1] = &c.outputs[1];
    ASSERT_TRUE(h.Prove());
    c.bindingProof.Release();
    ASSERT_NO_FATAL_FAILURE(h.ProveTransfers());
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsEmptyOutputHeldMovesAndLegacyOverwriteRefuse) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset());
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    auto& c = h.cohorts[0];
    ASSERT_TRUE(h.Prove());
    harken::CommittedPlan03GroupSlice moved(std::move(c.outputs[0]));
    EXPECT_FALSE(moved.Valid());
    EXPECT_FALSE(c.outputs[0].Valid());
    EXPECT_TRUE(c.outputs[0].Held());
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = 99;
    request.world = c.scope.world;
    request.transport = kEpoch;
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    harken::DecisionToken extraDecision{};
    ASSERT_TRUE(h.decisions.Reserve(request, &extraDecision));
    harken::Plan03GroupSliceInput extra{};
    extra.executeFrame = 210;
    extra.decision = extraDecision;
    harken::PreparedPlan03GroupSlice legacy;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, h.pool->PreparePlan03GroupSlice(extra, h.arms, &legacy));
    EXPECT_FALSE(h.pool->CommitPreparedPlan03GroupSlice(&legacy, &c.outputs[0], true));
    EXPECT_TRUE(legacy.Valid());
    harken::CommittedPlan03GroupSlice realOther;
    ASSERT_TRUE(h.pool->CommitPreparedPlan03GroupSlice(&legacy, &realOther, true));
    c.outputs[0] = std::move(realOther);
    EXPECT_TRUE(realOther.Valid());
    realOther = std::move(c.outputs[0]);
    EXPECT_TRUE(realOther.Valid());
    EXPECT_TRUE(c.outputs[0].Held());
    EXPECT_TRUE(c.bindingProof.Valid());
    ASSERT_TRUE(h.pool->RollbackCommittedPlan03GroupSlice(&realOther));
    ASSERT_TRUE(h.decisions.Cancel(extraDecision));
    ASSERT_TRUE(h.decisions.ReclaimCancelled(extraDecision));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsTwoTrueCohortsSharePageWithoutSharingRights) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset());
    ASSERT_NO_FATAL_FAILURE(h.Prepare(0));
    ASSERT_NO_FATAL_FAILURE(h.Prepare(1));
    ASSERT_TRUE(h.Prove(0));
    ASSERT_TRUE(h.Prove(1));
    for (unsigned i = 0; i < 3; ++i) ASSERT_NO_FATAL_FAILURE(h.Mint(i, 0));
    ScheduledTransactionPool::ConsumePreparedPageFrontierBindings(std::move(h.cohorts[0].bindingProof));
    EXPECT_TRUE(h.cohorts[1].bindingProof.Valid());
    for (unsigned i = 0; i < 3; ++i) ASSERT_NO_FATAL_FAILURE(h.Mint(i, 1));
    ScheduledTransactionPool::ConsumePreparedPageFrontierBindings(std::move(h.cohorts[1].bindingProof));
    EXPECT_EQ(6u, h.pool->CaptureLivePlan03Groups(nullptr, 0));
    EXPECT_EQ(10u, h.pool->CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(h.cohorts[0].identities[0].absoluteQuantum, h.cohorts[1].identities[0].absoluteQuantum);
    EXPECT_NE(h.cohorts[0].identities[0].committedRow, h.cohorts[1].identities[0].committedRow);
    ASSERT_NO_FATAL_FAILURE(h.Reclaim(0, false));
    EXPECT_EQ(3u, h.pool->CaptureLivePlan03Groups(nullptr, 0));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim(1));
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsOriginalProofOutputAndPoolFirstDetachRealHold) {
    for (unsigned first = 0; first < 4; ++first) {
        PrivatePageFrontierBindingHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Reset());
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        auto& c = h.cohorts[0];
        if (first == 0) {
            {
                harken::PreparedPageFrontierBindingsProof proof;
                ASSERT_TRUE(h.Prove(0, &proof));
            }
            ASSERT_NO_FATAL_FAILURE(h.ProveTransfers());
            ASSERT_TRUE(h.Prove());
            ASSERT_NO_FATAL_FAILURE(h.Reclaim());
        } else if (first == 1) {
            c.transfers[1].Release();
            auto original = std::make_unique<harken::PreparedPlan03GroupSlice>(std::move(c.originals[1]));
            c.originalPointers[1] = original.get();
            ASSERT_NO_FATAL_FAILURE(h.ProveTransfers());
            ASSERT_TRUE(h.Prove());
            original.reset();
            EXPECT_FALSE(c.bindingProof.Valid());
            for (const auto& output : c.outputs) EXPECT_FALSE(output.Held());
            h.pool->DropPreparedOccupancy();
        } else if (first == 2) {
            auto output = std::make_unique<harken::CommittedPlan03GroupSlice>();
            c.outputPointers[1] = output.get();
            ASSERT_TRUE(h.Prove());
            ASSERT_NO_FATAL_FAILURE(h.Mint(0));
            output.reset();
            EXPECT_FALSE(c.bindingProof.Valid());
            EXPECT_TRUE(c.outputs[0].Valid());
            EXPECT_FALSE(c.outputs[0].Held());
            ASSERT_NO_FATAL_FAILURE(h.Reclaim());
        } else {
            ASSERT_TRUE(h.Prove());
            ASSERT_NO_FATAL_FAILURE(h.Mint(0));
            h.pool.reset();
            EXPECT_FALSE(c.bindingProof.Valid());
            EXPECT_FALSE(c.outputs[0].Valid());
            for (const auto& transfer : c.transfers) EXPECT_FALSE(transfer.Valid());
            for (const auto& original : c.originals) EXPECT_FALSE(original.Valid());
            for (const auto& output : c.outputs) EXPECT_FALSE(output.Held());
            EXPECT_FALSE(c.batchProof.Valid());
        }
    }
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsActualMaximumClaimsRetireWithoutWrap) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset());
    ASSERT_NO_FATAL_FAILURE(h.Prepare(0, true));
    auto& c = h.cohorts[0];
    const auto identities = c.identities;
    ASSERT_TRUE(h.Prove());
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(UINT32_MAX, identities[i].rowClaimGeneration);
        ASSERT_NO_FATAL_FAILURE(h.Mint(i));
    }
    ScheduledTransactionPool::ConsumePreparedPageFrontierBindings(std::move(c.bindingProof));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim(0, false));
    ASSERT_NO_FATAL_FAILURE(h.Prepare(0));
    for (unsigned i = 0; i < 3; ++i) EXPECT_NE(identities[i].committedRow, c.identities[i].committedRow);
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageFrontierBindingsHoldPreventsRealLeavingPageTerminalMutation) {
    PrivatePageFrontierBindingHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Reset(true));
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.Mint(0));
    EXPECT_EQ(0u, h.pool->Advance());
    EXPECT_TRUE(h.pool->Valid());
    EXPECT_TRUE(h.cohorts[0].bindingProof.Valid());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim(0, false));
    EXPECT_EQ(0u, h.pool->Advance());
    EXPECT_FALSE(h.pool->Valid());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferReleaseReproveUniqueConsumeRealPublicationOwner) {
    for (bool releasePublication : {false, true}) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    ASSERT_TRUE(h.Prove());
    EXPECT_TRUE(h.original.Held());
    EXPECT_TRUE(h.proof.MatchesOriginalScope(h.original, h.scope, h.actual, h.bindings));
    h.proof.Release();
    EXPECT_TRUE(h.original.Valid());
    EXPECT_FALSE(h.original.Held());
    ASSERT_TRUE(h.Prove());
    std::array<harken::CommittedPlan03GroupSlice, 3> committedGroups{};
    for (std::size_t index = 0; index < h.groups.size(); ++index) {
        harken::PreparedPageGroupTransferProof groupProof;
        ASSERT_TRUE(h.pool.ProvePreparedPageGroupTransfer(h.groups[index], h.scope,
            h.groupInputs[index], h.arms, &groupProof));
        auto consumed = ScheduledTransactionPool::ConsumePreparedPageGroupTransfer(std::move(groupProof));
        EXPECT_EQ(h.groupIds[index], consumed.identity);
        committedGroups[index] = std::move(consumed.owner);
    }
    auto owner = ScheduledTransactionPool::ConsumePreparedPageBatchTransfer(std::move(h.proof));
    ASSERT_TRUE(owner.Valid());
    EXPECT_FALSE(owner.Held());
    EXPECT_FALSE(h.original.Valid());
    EXPECT_FALSE(h.proof.Valid());
    EXPECT_FALSE(h.Prove());
    EXPECT_EQ(5u, h.pool.LiveTransactionCount());
    EXPECT_EQ(5u, h.pool.IngressThisQuantum());
    EXPECT_EQ(0u, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(8u, h.pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(owner, true));
    EXPECT_TRUE(h.pool.CanReleasePreparedTransactionPublication(owner));
    if (releasePublication) {
        ASSERT_TRUE(h.pool.ReleasePreparedTransactionPublication(&owner));
        EXPECT_FALSE(owner.Valid());
        EXPECT_FALSE(h.pool.ReleasePreparedTransactionPublication(&owner));
    } else {
        ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&owner));
    }
    for (auto& group : committedGroups) ASSERT_TRUE(h.pool.RollbackCommittedPlan03GroupSlice(&group));
    if (releasePublication) {
        EXPECT_EQ(5u, h.pool.LiveTransactionCount());
        EXPECT_EQ(0u, h.pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
        for (unsigned page = 0; page < 4; ++page) h.pool.Advance();
        EXPECT_EQ(0u, h.pool.LiveTransactionCount());
    }
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferHeldRefusesLegacyMutationsMovesAndGlobalDrop) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    ASSERT_TRUE(h.Prove());
    const auto before = h.pool.CapturePrivateMaterializationSnapshotForTest();
    PreparedTransactionBatch moved(std::move(h.original));
    EXPECT_FALSE(moved.Valid());
    EXPECT_TRUE(h.original.Held());
    harken::PreparedPageBatchTransferProof movedProof(std::move(h.proof));
    EXPECT_FALSE(movedProof.Valid());
    EXPECT_TRUE(h.proof.Valid());
    h.original = std::move(moved);
    h.proof = std::move(movedProof);
    EXPECT_TRUE(h.proof.Valid());
    EXPECT_TRUE(h.original.Held());
    EXPECT_FALSE(h.pool.FreezePreparedTransactionBatch(h.original, h.scope.sequence));
    EXPECT_FALSE(h.pool.BindPreparedTransactionEventFrontier(h.original, 0, 0, h.bindings[0]));
    EXPECT_FALSE(h.pool.CommitPreparedTransactionBatch(&h.original, h.scope.sequence, true));
    EXPECT_FALSE(h.pool.CancelPreparedTransactionBatch(&h.original));
    EXPECT_FALSE(h.pool.CanReleasePreparedTransactionPublication(h.original));
    EXPECT_FALSE(h.pool.ReleasePreparedTransactionPublication(&h.original));
    EXPECT_FALSE(h.pool.ResetForEpoch(0, kEpoch + 1));
    h.pool.DropPreparedOccupancy();
    const auto after = h.pool.CapturePrivateMaterializationSnapshotForTest();
    EXPECT_EQ(before.preparedEvents, after.preparedEvents);
    EXPECT_EQ(before.preparedGroups, after.preparedGroups);
    EXPECT_EQ(before.publicationHolds, after.publicationHolds);
    EXPECT_EQ(0u, h.pool.LiveTransactionCount());
    EXPECT_EQ(0u, h.pool.IngressThisQuantum());
    EXPECT_TRUE(h.proof.Valid());
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(h.original));
    h.proof.Release();
    moved = std::move(h.original);
    EXPECT_FALSE(h.original.Valid());
    ASSERT_TRUE(moved.Valid());
    h.original = std::move(moved);
    ASSERT_TRUE(h.Prove());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferExactFrozenInputsAndBindingsRefuseAlterations) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    for (unsigned field = 0; field < 4; ++field) {
        auto scope = h.scope;
        if (field == 0) ++scope.cohortRoot;
        if (field == 1) ++scope.sequence;
        if (field == 2) ++scope.world;
        if (field == 3) ++scope.attemptGeneration;
        EXPECT_FALSE(h.pool.ProvePreparedPageBatchTransfer(h.original, scope, h.actual, h.bindings, &h.proof));
    }
    for (unsigned field = 0; field < 22; ++field) {
        auto inputs = h.actual;
        auto& input = inputs[3];
        if (field == 0) ++input.txnSlot;
        if (field == 1) ++input.rowSequence;
        if (field == 2) ++input.executeFrame;
        if (field == 3) ++input.logicCommitSequence;
        if (field == 4) ++input.transactionId;
        if (field == 5) ++input.executionOrdinal;
        if (field == 6) ++input.eventIndex;
        if (field == 7) ++input.transactionOrdinal;
        if (field == 8) ++input.payload;
        if (field == 9) input.authoredFadeOutCandidate = !input.authoredFadeOutCandidate;
        if (field == 10) ++input.program.programGeneration;
        if (field == 11) input.program.program = nullptr;
        if (field == 12) ++input.ownerInstanceScheduleEpoch;
        if (field == 13) ++input.armToken.renderGeneration;
        if (field == 14) input.frontier = h.bindings[3];
        if (field == 15) input.frozenTxnValid = true;
        if (field == 16) ++input.subeventPhase;
        if (field == 17) ++input.ownerInstanceHandle;
        if (field == 18) ++input.armToken.slot;
        if (field == 19) ++input.armToken.generation;
        if (field == 20) ++input.armToken.renderSlot;
        if (field == 21) input.immediateEntry.generation = 1;
        EXPECT_FALSE(h.pool.ProvePreparedPageBatchTransfer(h.original, h.scope, inputs, h.bindings, &h.proof));
        EXPECT_FALSE(h.original.Held());
    }
    for (unsigned field = 0; field < 9; ++field) {
        auto bindings = h.bindings;
        auto& binding = bindings[2];
        if (field == 0) ++binding.world;
        if (field == 1) ++binding.transport;
        if (field == 2) ++binding.entry.attempt;
        if (field == 3) ++binding.entry.coordinator.generation;
        if (field == 4) ++binding.owner.childEpoch;
        if (field == 5) ++binding.group.absoluteQuantum;
        if (field == 6) ++binding.operationOrdinal;
        if (field == 7) ++binding.operationEventCount;
        if (field == 8) ++binding.entryEventOrdinal;
        EXPECT_FALSE(h.pool.ProvePreparedPageBatchTransfer(h.original, h.scope, h.actual, bindings, &h.proof));
    }
    ASSERT_TRUE(h.Prove());
    auto changed = h.bindings;
    ++changed[1].group.pageGeneration;
    EXPECT_FALSE(h.proof.MatchesOriginalScope(h.original, h.scope, h.actual, changed));
    EXPECT_TRUE(h.proof.Valid());
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferTrueNeighborCommitMovesHeldRowsWithoutLosingIdentity) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare());
    ASSERT_TRUE(h.Prove());
    PreparedTransactionBatch neighbor;
    ASSERT_EQ(ReserveRefusal::None, h.pool.PrepareTransactionBatch(
        h.requests.data(), h.requests.size(), 0, &neighbor));
    ASSERT_TRUE(h.pool.CommitPreparedTransactionBatch(&neighbor, 54, true));
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(h.original));
    EXPECT_TRUE(h.proof.MatchesOriginalScope(h.original, h.scope, h.actual, h.bindings));
    auto owner = ScheduledTransactionPool::ConsumePreparedPageBatchTransfer(std::move(h.proof));
    EXPECT_EQ(10u, h.pool.LiveTransactionCount());
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(owner, true));
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&neighbor));
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(owner, true));
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&owner));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferEarlierAdvanceCarriesRealHeldIngressAndReleasesIt) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(false, 10));
    PreparedTransactionBatch older;
    ASSERT_EQ(ReserveRefusal::None, h.pool.PrepareTransactionBatch(
        h.requests.data(), h.requests.size(), 0, &older));
    ASSERT_TRUE(h.pool.FreezePreparedTransactionBatch(older, 54));
    ASSERT_TRUE(h.Prove());
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_EQ(100u, h.pool.WindowStartFrame());
    PreparedTransactionBatch current;
    ASSERT_EQ(ReserveRefusal::None, h.pool.PrepareTransactionBatch(
        h.requests.data(), h.requests.size(), 0, &current));
    EXPECT_FALSE(h.pool.CommitPreparedTransactionBatch(&older, 54, true));
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&current));
    ASSERT_TRUE(h.pool.CommitPreparedTransactionBatch(&older, 54, true));
    EXPECT_EQ(5u, h.pool.IngressThisQuantum());
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&older));
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_EQ(200u, h.pool.WindowStartFrame());
    EXPECT_TRUE(h.proof.Valid());
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(h.original));
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_EQ(200u, h.pool.WindowStartFrame());
    auto owner = ScheduledTransactionPool::ConsumePreparedPageBatchTransfer(std::move(h.proof));
    EXPECT_EQ(5u, h.pool.IngressThisQuantum());
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&owner));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferOlderProofMustAcquireActualCurrentCreditBeforeHolding) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(false, 10));
    EXPECT_EQ(0u, h.pool.Advance());
    ASSERT_EQ(100u, h.pool.WindowStartFrame());
    PreparedTransactionBatch current;
    ASSERT_EQ(ReserveRefusal::None, h.pool.PrepareTransactionBatch(
        h.requests.data(), h.requests.size(), 0, &current));
    harken::UnplacedBatchCredit credit;
    ASSERT_TRUE(h.pool.ReserveUnplacedBatchCredit({5, 1, 5, 1, 0}, &credit));
    EXPECT_FALSE(h.Prove());
    EXPECT_FALSE(h.original.Held());
    EXPECT_FALSE(h.proof.Valid());
    EXPECT_TRUE(current.Valid());
    EXPECT_TRUE(credit.Valid());
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(h.original));
    ASSERT_TRUE(h.pool.CancelUnplacedBatchCredit(&credit));
    ASSERT_TRUE(h.Prove());
    h.proof.Release();
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&current));
    EXPECT_EQ(0u, h.pool.Advance());
    ASSERT_EQ(200u, h.pool.WindowStartFrame());
    ASSERT_EQ(ReserveRefusal::None, h.pool.PrepareTransactionBatch(
        h.requests.data(), h.requests.size(), 0, &current));
    ASSERT_TRUE(h.pool.ReserveUnplacedBatchCredit({5, 1, 5, 1, 0}, &credit));
    EXPECT_FALSE(h.Prove());
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&current));
    ASSERT_TRUE(h.Prove());
    auto owner = ScheduledTransactionPool::ConsumePreparedPageBatchTransfer(std::move(h.proof));
    EXPECT_EQ(5u, h.pool.IngressThisQuantum());
    ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&owner));
    ASSERT_TRUE(h.pool.CancelUnplacedBatchCredit(&credit));
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferHeldProtectsTrueLeavingPageGenerationTerminalWrite) {
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(true));
    ASSERT_TRUE(h.Prove());
    const auto before = h.pool.CapturePrivateMaterializationSnapshotForTest();
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_TRUE(h.pool.Valid());
    EXPECT_EQ(0u, h.pool.WindowStartFrame());
    EXPECT_EQ(UINT64_MAX, h.pool.Page(0)->pageGeneration);
    EXPECT_TRUE(h.proof.Valid());
    EXPECT_TRUE(h.original.Held());
    EXPECT_EQ(0u, h.pool.IngressThisQuantum());
    EXPECT_EQ(before.preparedEvents, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(before.publicationHolds, h.pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(h.original));
    h.proof.Release();
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_FALSE(h.pool.Valid());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferHeldProtectsActualWindowOverflowTerminalWrite) {
    PrivatePageBatchTransferHarness h;
    const auto start = UINT64_MAX - UINT64_MAX % 100u - 400u;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(false, 16, false, start));
    ASSERT_TRUE(h.Prove());
    const auto before = h.pool.CapturePrivateMaterializationSnapshotForTest();
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_TRUE(h.pool.Valid());
    EXPECT_EQ(start, h.pool.WindowStartFrame());
    EXPECT_TRUE(h.original.Held());
    EXPECT_TRUE(h.proof.Valid());
    EXPECT_EQ(before.preparedEvents, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    EXPECT_EQ(before.publicationHolds, h.pool.CapturePrivateMaterializationSnapshotForTest().publicationHolds);
    ASSERT_NO_FATAL_FAILURE(h.CheckKeys(h.original));
    h.proof.Release();
    EXPECT_EQ(0u, h.pool.Advance());
    EXPECT_FALSE(h.pool.Valid());
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferOriginalProofAndPoolDestructionDetachGenuineAssociations) {
    {
        PrivatePageBatchTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        auto original = std::make_unique<PreparedTransactionBatch>(std::move(h.original));
        ASSERT_TRUE(h.pool.ProvePreparedPageBatchTransfer(*original, h.scope, h.actual, h.bindings, &h.proof));
        original.reset();
        EXPECT_FALSE(h.proof.Valid());
        EXPECT_EQ(5u, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
        h.pool.DropPreparedOccupancy();
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
    {
        PrivatePageBatchTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        {
            harken::PreparedPageBatchTransferProof proof;
            ASSERT_TRUE(h.pool.ProvePreparedPageBatchTransfer(h.original, h.scope, h.actual, h.bindings, &proof));
        }
        EXPECT_TRUE(h.original.Valid());
        EXPECT_FALSE(h.original.Held());
        EXPECT_EQ(5u, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
        ASSERT_TRUE(h.Prove());
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
    {
        PreparedTransactionBatch original;
        harken::PreparedPageBatchTransferProof proof;
        auto h = std::make_unique<PrivatePageBatchTransferHarness>();
        ASSERT_NO_FATAL_FAILURE(h->Prepare());
        original = std::move(h->original);
        ASSERT_TRUE(h->pool.ProvePreparedPageBatchTransfer(original, h->scope, h->actual, h->bindings, &proof));
        h.reset();
        EXPECT_FALSE(original.Valid());
        EXPECT_FALSE(proof.Valid());
    }
}

TEST(ScheduledTransactionPool, PrivatePageBatchTransferUnheldLegacyCompletionAndRealMaxGenerationRetireGrant) {
    for (bool commit : {false, true}) {
        PrivatePageBatchTransferHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare());
        ASSERT_TRUE(h.Prove());
        h.proof.Release();
        if (commit) {
            ASSERT_TRUE(h.pool.CommitPreparedTransactionBatch(&h.original, h.scope.sequence, true));
            EXPECT_FALSE(h.Prove());
        }
        ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&h.original));
        EXPECT_FALSE(h.Prove());
        ASSERT_NO_FATAL_FAILURE(h.Reclaim());
    }
    PrivatePageBatchTransferHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(false, 16, true));
    for (unsigned claim = 0; claim < 5; ++claim) {
        if (claim != 0) ASSERT_NO_FATAL_FAILURE(h.PrepareBatch());
        ASSERT_TRUE(h.Prove());
        h.proof.Release();
        ASSERT_TRUE(h.pool.CancelPreparedTransactionBatch(&h.original));
        EXPECT_FALSE(h.Prove());
    }
    EXPECT_NE(ReserveRefusal::None, h.pool.PrepareTransactionBatch(
        h.requests.data(), h.requests.size(), 0, &h.original, &h.scope));
    EXPECT_FALSE(h.original.Valid());
    EXPECT_EQ(0u, h.pool.LiveTransactionCount());
    EXPECT_EQ(0u, h.pool.CapturePrivateMaterializationSnapshotForTest().preparedEvents);
    ASSERT_NO_FATAL_FAILURE(h.Reclaim());
}
#endif


struct FivePreparedIngressInputs {
    std::array<ScheduledEventInput, 5> events{};
    std::array<TransactionRequest, 5> requests{};
    explicit FivePreparedIngressInputs(const Harness& h) {
        for (std::uint32_t index = 0; index < requests.size(); ++index) {
            events[index].executeFrame = 210;
            events[index].eventIndex = index;
            requests[index].transactionId = 51;
            requests[index].transportEpoch = kEpoch;
            requests[index].ownerInstanceHandle = h.owner;
            requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
            requests[index].events = &events[index];
            requests[index].eventCount = 1;
        }
    }
};

void CheckOlderPreparedIngressChargesCurrentReservations(bool unplaced) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 32;
    config.maxIngressTransactionsPerQuantum = 10;
    config.maxEventsPerQuantum = 20;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    FivePreparedIngressInputs inputs(h);
    PreparedTransactionBatch older;
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(
        inputs.requests.data(), inputs.requests.size(), 0, &older));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(older, 51));
    std::array<ConsumedEvent, 5> originals{};
    for (std::uint32_t index = 0; index < originals.size(); ++index)
        ASSERT_TRUE(pool.CapturePreparedTransactionEvent(older, index, 0, &originals[index]));
    EXPECT_EQ(0u, pool.Advance());
    ASSERT_EQ(100u, pool.WindowStartFrame());
    PreparedTransactionBatch current;
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(
        inputs.requests.data(), inputs.requests.size(), 0, &current));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(current, 52));
    harken::UnplacedBatchCredit credit{};
    if (unplaced) {
        ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({5, 1, 5, 1, 0}, &credit));
    } else {
        for (std::uint32_t index = 0; index < inputs.requests.size(); ++index) {
            auto request = inputs.requests[index];
            request.transactionId = 100 + index;
            ASSERT_EQ(ReserveStatus::Committed,
                pool.Reserve(request, PrePublishLatePolicy::Refuse).status);
        }
    }
    const auto liveBefore = pool.LiveTransactionCount();
    const auto ingressBefore = pool.IngressThisQuantum();
    EXPECT_FALSE(pool.CommitPreparedTransactionBatch(&older, 51, true));
    EXPECT_TRUE(older.Valid());
    EXPECT_TRUE(current.Valid());
    EXPECT_EQ(liveBefore, pool.LiveTransactionCount());
    EXPECT_EQ(ingressBefore, pool.IngressThisQuantum());
    for (std::uint32_t index = 0; index < originals.size(); ++index) {
        ConsumedEvent actual{};
        ASSERT_TRUE(pool.CapturePreparedTransactionEvent(older, index, 0, &actual));
        EXPECT_EQ(originals[index].rowSequence, actual.rowSequence);
        EXPECT_EQ(originals[index].txnSlot, actual.txnSlot);
        EXPECT_EQ(originals[index].logicCommitSequence, actual.logicCommitSequence);
    }
    // Already charged current credits transition without claiming capacity twice.
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&current, 52, true));
    EXPECT_FALSE(pool.CommitPreparedTransactionBatch(&older, 51, true));
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&current));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&older, 51, true));
    EXPECT_EQ(ingressBefore + 5u, pool.IngressThisQuantum());
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&older));
    if (unplaced) ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&credit));
    for (std::uint32_t page = 0; page < 4; ++page) pool.Advance();
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_EQ(0u, pool.IngressThisQuantum());
    EXPECT_TRUE(pool.ResetForEpoch(pool.WindowStartFrame(), kEpoch + 1));
}

TEST(ScheduledTransactionPool, PreparedIngressOlderBatchIncludesCurrentPreparedAndLiveReservations) {
    ASSERT_NO_FATAL_FAILURE(CheckOlderPreparedIngressChargesCurrentReservations(false));
}
TEST(ScheduledTransactionPool, PreparedIngressOlderBatchIncludesCurrentPreparedAndUnplacedReservations) {
    ASSERT_NO_FATAL_FAILURE(CheckOlderPreparedIngressChargesCurrentReservations(true));
}
TEST(ScheduledTransactionPool, PreparedIngressPrivateMaterializationRefusesOvercommitAndRetriesRealCredit) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 32;
    config.maxIngressTransactionsPerQuantum = 10;
    config.maxEventsPerQuantum = 20;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    FivePreparedIngressInputs inputs(h);
    harken::TimelineArmTable arms(1);
    harken::UnplacedBatchCredit originalCredit{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({5, 1, 5, 1, 0}, &originalCredit));
    ScheduledTransactionPool::PrivateMaterializationTransaction lease;
    ReserveRefusal refusal = ReserveRefusal::None;
    harken::Plan03GroupReserveStatus groupStatus = harken::Plan03GroupReserveStatus::Committed;
    ASSERT_TRUE(pool.TryMaterializeUnplacedBatch(&originalCredit,
        inputs.requests.data(), inputs.requests.size(), nullptr, 0, arms, 0,
        &lease, &refusal, &groupStatus));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(*lease.Transactions(), 51));
    EXPECT_EQ(0u, pool.Advance());
    ASSERT_EQ(100u, pool.WindowStartFrame());
    PreparedTransactionBatch current;
    ASSERT_EQ(ReserveRefusal::None, pool.PrepareTransactionBatch(
        inputs.requests.data(), inputs.requests.size(), 0, &current));
    harken::UnplacedBatchCredit neighbor{};
    ASSERT_TRUE(pool.ReserveUnplacedBatchCredit({5, 1, 5, 1, 0}, &neighbor));
    EXPECT_FALSE(lease.Commit(51));
    EXPECT_TRUE(pool.Valid());
    EXPECT_TRUE(lease.Valid());
    EXPECT_TRUE(lease.Transactions()->Valid());
    EXPECT_TRUE(current.Valid());
    EXPECT_TRUE(neighbor.Valid());
    EXPECT_EQ(0u, pool.IngressThisQuantum());
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    ASSERT_TRUE(pool.CancelUnplacedBatchCredit(&neighbor));
    ASSERT_TRUE(lease.Commit(51));
    EXPECT_EQ(5u, pool.IngressThisQuantum());
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&current, 52, true));
    EXPECT_EQ(10u, pool.IngressThisQuantum());
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&current));
    ASSERT_TRUE(lease.ReleasePublication());
    EXPECT_FALSE(originalCredit.Valid());
    for (std::uint32_t page = 0; page < 4; ++page) pool.Advance();
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_EQ(0u, pool.IngressThisQuantum());
    EXPECT_TRUE(pool.ResetForEpoch(pool.WindowStartFrame(), kEpoch + 1));
}

TEST(ScheduledTransactionPool,
     PreparedTransactionBatchStaysHiddenUntilSingleSequenceCommit) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 2;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));

    std::array<ScheduledEventInput, 2> events{};
    std::array<TransactionRequest, 2> requests{};
    for (std::uint32_t index = 0; index < requests.size(); ++index) {
        events[index].executeFrame = 10u + index * 10u;
        events[index].logicCommitSequence = 999u;
        events[index].eventIndex = index;
        events[index].transactionOrdinal = index;
        events[index].payload = 100u + index;
        requests[index].transactionId = 999u;
        requests[index].transportEpoch = kEpoch;
        requests[index].ownerInstanceHandle = h.owner;
        requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
        requests[index].events = &events[index];
        requests[index].eventCount = 1;
    }

    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(
                  requests.data(), requests.size(), 0, &prepared),
              ReserveRefusal::None);
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
    EXPECT_EQ(pool.TransactionHighWater(), 0u);
    EXPECT_EQ(pool.EventHighWater(), 0u);
    EXPECT_EQ(pool.FrameBatchHighWater(), 0u);
    EXPECT_EQ(pool.IngressThisQuantum(), 0u);
    ASSERT_NE(pool.Page(0), nullptr);
    EXPECT_EQ(pool.Page(0)->committedTransactionCount, 0u);

    std::array<ConsumedEvent, 2> consumed{};
    EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              0u);

    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, 41u));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(pool.LiveTransactionCount(), 2u);
    EXPECT_EQ(pool.TransactionHighWater(), 2u);
    EXPECT_EQ(pool.EventHighWater(), 2u);
    EXPECT_EQ(pool.FrameBatchHighWater(), 2u);
    EXPECT_EQ(pool.IngressThisQuantum(), 2u);
    ASSERT_NE(pool.Page(0), nullptr);
    EXPECT_EQ(pool.Page(0)->committedTransactionCount, 2u);
    ASSERT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              2u);
    EXPECT_EQ(consumed[0].logicCommitSequence, 41u);
    EXPECT_EQ(consumed[1].logicCommitSequence, 41u);
    EXPECT_EQ(consumed[0].transactionId, 41u);
    EXPECT_EQ(consumed[1].transactionId, 41u);
}

TEST(ScheduledTransactionPool,
     PreparedTransactionBatchOccupiesFinalAdmissionCapacity) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 2;
    config.maxFrameBatchesPerQuantum = 1;
    config.maxEventsPerQuantum = 3;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));

    ScheduledEventInput preparedEvent{};
    preparedEvent.executeFrame = 10;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &preparedEvent;
    request.eventCount = 1;
    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &prepared),
              ReserveRefusal::None);

    const ReserveResult distinctFrame = ReserveFrames(pool, h, 1, {20});
    EXPECT_EQ(distinctFrame.status, ReserveStatus::Refused);
    EXPECT_EQ(distinctFrame.refusal, ReserveRefusal::PageBatchCapacity);
    EXPECT_EQ(ReserveFrames(pool, h, 2, {10}).status,
              ReserveStatus::Committed);
    const ReserveResult fullIngress = ReserveFrames(pool, h, 3, {10});
    EXPECT_EQ(fullIngress.status, ReserveStatus::Refused);
    EXPECT_EQ(fullIngress.refusal, ReserveRefusal::IngressRateExceeded);
    EXPECT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
}

TEST(ScheduledTransactionPool,
     PreparedPublicationHoldProtectsPagesWhileEarlierPageAdvances) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 7, {10}).status, ReserveStatus::Committed);
    std::array<ScheduledEventInput, 2> events{};
    events[0].executeFrame = 110;
    events[1].executeFrame = 210;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = events.data();
    request.eventCount = events.size();
    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &prepared), ReserveRefusal::None);
    EXPECT_FALSE(pool.ReleasePreparedTransactionPublication(&prepared));
    pool.Publish(300, &h.instances, &h.occurrences);
    EXPECT_EQ(pool.PageStamp(0), 0u);
    EXPECT_EQ(pool.PageStamp(1), harken::kUnpublishedQuantumStamp);
    EXPECT_EQ(pool.PublishCutoffFrame(), 100u);
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, 41, true));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_FALSE(pool.CommitPreparedTransactionBatch(&prepared, 41, true));
    pool.Publish(300, &h.instances, &h.occurrences);
    EXPECT_EQ(pool.PageStamp(1), harken::kUnpublishedQuantumStamp);
    EXPECT_EQ(pool.PageStamp(2), harken::kUnpublishedQuantumStamp);
    EXPECT_EQ(pool.PublishCutoffFrame(), 100u);
    EXPECT_FALSE(pool.Reset(MakeConfig(), 0, kEpoch + 1));
    EXPECT_FALSE(pool.ResetForEpoch(0, kEpoch + 1));
    const auto frame = pool.WindowStartFrame();
    pool.Advance();
    EXPECT_EQ(pool.WindowStartFrame(), frame + 100u);
    pool.DropPreparedOccupancy();
    EXPECT_TRUE(prepared.Valid());
    ASSERT_TRUE(pool.ReleasePreparedTransactionPublication(&prepared));
    EXPECT_FALSE(pool.ReleasePreparedTransactionPublication(&prepared));
    pool.Publish(300, &h.instances, &h.occurrences);
    EXPECT_EQ(pool.PageStamp(1), 1u);
    EXPECT_EQ(pool.PageStamp(2), 2u);
    EXPECT_FALSE(pool.CancelPreparedTransactionBatch(&prepared));
    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(pool.ConsumePageRt(1, consumed.data(), consumed.size()), 1u);
    EXPECT_EQ(consumed[0].executeFrame, 110u);
    EXPECT_EQ(consumed[0].transactionId, 41u);
}

TEST(ScheduledTransactionPool,
     HeldFutureTransactionCanRollbackAfterIngressQuantumAdvances) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    PreparedTransactionBatch future;
    ASSERT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 0, &future));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&future, 41, true));
    pool.Publish(100, &h.instances, &h.occurrences);
    ASSERT_EQ(0u, pool.PageStamp(0));
    ASSERT_EQ(0u, pool.Advance());
    ASSERT_EQ(1u, pool.WindowStartQuantum());
    ASSERT_EQ(0u, pool.IngressThisQuantum());
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&future));
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_EQ(0u, pool.IngressThisQuantum());
    pool.Publish(300, &h.instances, &h.occurrences);
    EXPECT_EQ(2u, pool.PageStamp(2));
}

TEST(ScheduledTransactionPool,
     HeldFutureTransactionCanCommitAfterIngressQuantumAdvances) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    ScheduledEventInput event{};
    event.executeFrame = 210;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    PreparedTransactionBatch future;
    ASSERT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 0, &future));
    pool.Publish(100, &h.instances, &h.occurrences);
    ASSERT_EQ(0u, pool.Advance());
    ASSERT_EQ(1u, pool.WindowStartQuantum());
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&future, 41, true));
    EXPECT_EQ(1u, pool.IngressThisQuantum());
    ASSERT_TRUE(pool.ReleasePreparedTransactionPublication(&future));
    pool.Publish(300, &h.instances, &h.occurrences);
    EXPECT_EQ(2u, pool.PageStamp(2));
}

TEST(ScheduledTransactionPool, FrozenPreparedKeysSurviveOutOfOrderPromotionAndCompaction) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    std::array<ScheduledEventInput, 2> events{};
    for (auto& event : events) {
        event.executeFrame = 10;
        event.logicCommitSequence = 999;
        event.eventIndex = 23;
        event.executionOrdinal = 4;
        event.transactionOrdinal = 8;
        event.subeventPhase = 2;
    }
    events[0].payload = 11;
    events[1].payload = 22;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = events.data();
    request.eventCount = events.size();
    PreparedTransactionBatch first, second;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &first), ReserveRefusal::None);
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &second), ReserveRefusal::None);
    ConsumedEvent frozen{};
    EXPECT_FALSE(pool.CapturePreparedTransactionEvent(first, 0, 0, &frozen));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(first, 41));
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(first, 41));
    EXPECT_FALSE(pool.FreezePreparedTransactionBatch(first, 42));
    ASSERT_TRUE(pool.CapturePreparedTransactionEvent(first, 0, 1, &frozen));
    EXPECT_EQ(frozen.logicCommitSequence, 41u);
    EXPECT_EQ(frozen.transactionId, 41u);
    EXPECT_EQ(frozen.payload, 22u);
    EXPECT_FALSE(pool.CapturePreparedTransactionEvent(first, 1, 0, &frozen));
    EXPECT_FALSE(pool.CapturePreparedTransactionEvent(first, 0, 2, &frozen));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&second, 41, true));
    EXPECT_FALSE(pool.CommitPreparedTransactionBatch(&first, 42, true));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&first, 41, true));
    ConsumedEvent promoted{};
    ASSERT_TRUE(pool.CapturePreparedTransactionEvent(first, 0, 1, &promoted));
    EXPECT_EQ(promoted.rowSequence, frozen.rowSequence);
    EXPECT_EQ(promoted.txnSlot, frozen.txnSlot);
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&second));
    ASSERT_TRUE(pool.ReleasePreparedTransactionPublication(&first));
    pool.Publish(100, &h.instances, &h.occurrences);
    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(pool.ConsumePageRt(0, consumed.data(), consumed.size()), 2u);
    EXPECT_EQ(consumed[0].payload, 11u);
    EXPECT_EQ(consumed[1].payload, 22u);
    EXPECT_EQ(consumed[1].rowSequence, frozen.rowSequence);
    EXPECT_LT(consumed[0].rowSequence, consumed[1].rowSequence);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
    EXPECT_EQ(pool.IngressThisQuantum(), 1u);
}

TEST(ScheduledTransactionPool, PrivatePublicationRollbackPreservesLiveAndPreparedNeighbors) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 3;
    config.maxIngressTransactionsPerQuantum = 3;
    config.maxEventsPerQuantum = 3;
    config.maxFrameBatchesPerQuantum = 3;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));
    ASSERT_EQ(ReserveFrames(pool, h, 7, {10, 110}).status, ReserveStatus::Committed);
    std::array<ScheduledEventInput, 2> events{};
    events[0].executeFrame = 10;
    events[1].executeFrame = 110;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = events.data();
    request.eventCount = events.size();
    PreparedTransactionBatch cancelled, neighbor;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &cancelled), ReserveRefusal::None);
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &neighbor), ReserveRefusal::None);
    ASSERT_TRUE(pool.FreezePreparedTransactionBatch(neighbor, 43));
    ConsumedEvent frozen{};
    ASSERT_TRUE(pool.CapturePreparedTransactionEvent(neighbor, 0, 1, &frozen));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&cancelled, 41, true));
    pool.Publish(200, &h.instances, &h.occurrences);
    EXPECT_EQ(pool.PageStamp(0), harken::kUnpublishedQuantumStamp);
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&cancelled));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&neighbor, 43, true));
    ASSERT_TRUE(pool.ReleasePreparedTransactionPublication(&neighbor));
    EXPECT_EQ(ReserveFrames(pool, h, 44, {10, 110}).status, ReserveStatus::Committed);
    pool.Publish(200, &h.instances, &h.occurrences);
    for (std::uint32_t page = 0; page < 2; ++page) {
        std::array<ConsumedEvent, 3> consumed{};
        ASSERT_EQ(pool.ConsumePageRt(page, consumed.data(), consumed.size()), 3u);
        EXPECT_EQ(consumed[0].transactionId, 7u);
        EXPECT_EQ(consumed[1].transactionId, 43u);
        EXPECT_EQ(consumed[2].transactionId, 44u);
        if (page == 1) EXPECT_EQ(consumed[1].rowSequence, frozen.rowSequence);
        EXPECT_EQ(pool.Page(page)->committedTransactionCount, 3u);
    }
}

TEST(ScheduledTransactionPool, ExactProgramReferenceIncludesPreparedAndEveryUnreclaimedPage) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    harken::TimelineProgramView program{71};
    harken::TimelineProgramView other{71};
    const harken::TimelineTransactionProgramRef reference{71, &program};
    std::array<ScheduledEventInput, 2> events{};
    events[0].executeFrame = 10;
    events[1].executeFrame = 110;
    events[1].eventIndex = 1;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.program = reference;
    request.events = events.data();
    request.eventCount = events.size();
    PreparedTransactionBatch prepared;
    ASSERT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
    EXPECT_TRUE(pool.HasProgramReference(reference));
    EXPECT_FALSE(pool.HasProgramReference({71, &other}));
    EXPECT_FALSE(pool.HasProgramReference({72, &program}));
    EXPECT_FALSE(pool.HasProgramReference({}));
    EXPECT_FALSE(pool.HasLiveProgramGeneration(71));
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, 41));
    EXPECT_TRUE(pool.HasProgramReference(reference));
    std::size_t consumed = 0;
    ConsumeDigest(pool, 0, kEpoch, h, &consumed);
    ASSERT_EQ(1u, consumed);
    EXPECT_TRUE(pool.HasProgramReference(reference));
    pool.ConsumerFrontier().store(1);
    ASSERT_EQ(1u, pool.Advance());
    EXPECT_TRUE(pool.HasProgramReference(reference));
    ConsumeDigest(pool, 0, kEpoch, h, &consumed);
    ASSERT_EQ(1u, consumed);
    EXPECT_TRUE(pool.HasProgramReference(reference));
    pool.ConsumerFrontier().store(2);
    ASSERT_EQ(1u, pool.Advance());
    EXPECT_FALSE(pool.HasProgramReference(reference));

    events[0].executeFrame = 210;
    request.eventCount = 1;
    ASSERT_EQ(ReserveRefusal::None,
              pool.PrepareTransactionBatch(&request, 1, 200, &prepared));
    ASSERT_TRUE(pool.HasProgramReference(reference));
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
    EXPECT_FALSE(pool.HasProgramReference(reference));
}

TEST(ScheduledTransactionPool,
     CancelPreparedTransactionBatchRestoresCapacityWithoutHighWater) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 1;
    config.maxIngressTransactionsPerQuantum = 1;
    config.maxFrameBatchesPerQuantum = 1;
    config.maxEventsPerQuantum = 1;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));

    ScheduledEventInput event{};
    event.executeFrame = 10;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &prepared),
              ReserveRefusal::None);
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));

    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
    EXPECT_EQ(pool.TransactionHighWater(), 0u);
    EXPECT_EQ(pool.EventHighWater(), 0u);
    EXPECT_EQ(pool.FrameBatchHighWater(), 0u);
    EXPECT_EQ(pool.IngressThisQuantum(), 0u);
    EXPECT_EQ(ReserveFrames(pool, h, 1, {20}).status,
              ReserveStatus::Committed);
}

TEST(ScheduledTransactionPool,
     PreparedTransactionBatchBlocksAdvanceAndEpochReset) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    ScheduledEventInput event{};
    event.executeFrame = 10;
    TransactionRequest request{};
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &prepared),
              ReserveRefusal::None);
    EXPECT_EQ(pool.Advance(), 0u);
    EXPECT_EQ(pool.WindowStartFrame(), 0u);
    EXPECT_FALSE(pool.CanResetForEpoch(0, kEpoch + 1));

    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
    EXPECT_EQ(pool.Advance(), 0u);
    EXPECT_EQ(pool.WindowStartFrame(), 100u);
}

TEST(ScheduledTransactionPool,
     PreparedTransactionBatchesCommitAndCancelOutOfPrepareOrder) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 2;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));

    std::array<ScheduledEventInput, 2> events{};
    std::array<TransactionRequest, 2> requests{};
    for (std::uint32_t index = 0; index < requests.size(); ++index) {
        events[index].executeFrame = 10u + index * 10u;
        events[index].payload = 100u + index;
        requests[index].transportEpoch = kEpoch;
        requests[index].ownerInstanceHandle = h.owner;
        requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
        requests[index].events = &events[index];
        requests[index].eventCount = 1;
    }
    PreparedTransactionBatch first;
    PreparedTransactionBatch second;
    ASSERT_EQ(pool.PrepareTransactionBatch(&requests[0], 1, 0, &first),
              ReserveRefusal::None);
    ASSERT_EQ(pool.PrepareTransactionBatch(&requests[1], 1, 0, &second),
              ReserveRefusal::None);

    PreparedTransactionBatch movedSecond = std::move(second);
    EXPECT_FALSE(second.Valid());
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&movedSecond, 22u));
    EXPECT_FALSE(pool.CommitPreparedTransactionBatch(&movedSecond, 23u));
    EXPECT_FALSE(pool.CancelPreparedTransactionBatch(&movedSecond));
    ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&first));
    EXPECT_FALSE(pool.CancelPreparedTransactionBatch(&first));
    EXPECT_FALSE(pool.CommitPreparedTransactionBatch(&first, 24u));

    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].executeFrame, 20u);
    EXPECT_EQ(consumed[0].logicCommitSequence, 22u);
    EXPECT_EQ(consumed[0].payload, 101u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
    EXPECT_EQ(pool.IngressThisQuantum(), 1u);
}

TEST(ScheduledTransactionPool,
     PreparedTransactionBatchSpansPagesAndSharesFrameBatch) {
    Harness h;
    ScheduledTransactionPool pool;
    auto config = MakeConfig();
    config.maxScheduledTransactions = 2;
    config.maxIngressTransactionsPerQuantum = 2;
    config.maxTransactionEvents = 2;
    ASSERT_TRUE(pool.Reset(config, 0, kEpoch));

    std::array<std::array<ScheduledEventInput, 2>, 2> events{};
    events[0][0].executeFrame = 10;
    events[0][1].executeFrame = 110;
    events[1][0].executeFrame = 10;
    events[1][1].executeFrame = 210;
    std::array<TransactionRequest, 2> requests{};
    for (std::uint32_t index = 0; index < requests.size(); ++index) {
        for (std::uint32_t event = 0; event < events[index].size(); ++event) {
            events[index][event].eventIndex = event;
            events[index][event].transactionOrdinal = index;
        }
        requests[index].transportEpoch = kEpoch;
        requests[index].ownerInstanceHandle = h.owner;
        requests[index].ownerInstanceScheduleEpoch = h.ownerEpoch;
        requests[index].events = events[index].data();
        requests[index].eventCount = events[index].size();
    }

    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(
                  requests.data(), requests.size(), 0, &prepared),
              ReserveRefusal::None);
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, 77u));
    EXPECT_EQ(pool.LiveTransactionCount(), 2u);
    EXPECT_EQ(pool.EventHighWater(), 2u);
    EXPECT_EQ(pool.FrameBatchHighWater(), 1u);
    ASSERT_NE(pool.Page(0), nullptr);
    ASSERT_NE(pool.Page(1), nullptr);
    ASSERT_NE(pool.Page(2), nullptr);
    EXPECT_EQ(pool.Page(0)->committedTransactionCount, 2u);
    EXPECT_EQ(pool.Page(1)->committedTransactionCount, 1u);
    EXPECT_EQ(pool.Page(2)->committedTransactionCount, 1u);

    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              2u);
    EXPECT_EQ(consumed[0].logicCommitSequence, 77u);
    EXPECT_EQ(consumed[1].logicCommitSequence, 77u);
    EXPECT_EQ(pool.ConsumePage(1, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(pool.ConsumePage(2, kEpoch, h.instances, h.occurrences,
                               consumed.data(), consumed.size()),
              1u);
}

TEST(ScheduledTransactionPool,
     PreparedTransactionBatchAcceptsMaximumCommitSequence) {
    Harness h;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));

    ScheduledEventInput event{};
    event.executeFrame = 10;
    event.logicCommitSequence = 1;
    TransactionRequest request{};
    request.transactionId = 1;
    request.transportEpoch = kEpoch;
    request.ownerInstanceHandle = h.owner;
    request.ownerInstanceScheduleEpoch = h.ownerEpoch;
    request.events = &event;
    request.eventCount = 1;
    PreparedTransactionBatch prepared;
    ASSERT_EQ(pool.PrepareTransactionBatch(&request, 1, 0, &prepared),
              ReserveRefusal::None);
    ASSERT_TRUE(pool.CommitPreparedTransactionBatch(
        &prepared, std::numeric_limits<std::uint64_t>::max()));

    ConsumedEvent consumed{};
    ASSERT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               &consumed, 1),
              1u);
    EXPECT_EQ(consumed.logicCommitSequence,
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(consumed.transactionId,
              std::numeric_limits<std::uint64_t>::max());
}

TEST(ScheduledTransactionPool,
     SingleEventBatchPreflightRejectsWholeCrossPageIngressOverflow) {
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxScheduledTransactions = 2;
    c.maxIngressTransactionsPerQuantum = 2;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));
    const std::array<std::uint64_t, 3> frames{10, 110, 210};

    EXPECT_EQ(ReserveRefusal::IngressRateExceeded,
              pool.PreflightSingleEventTransactionBatch(
                  frames.data(), frames.size(), 0));
    EXPECT_EQ(0u, pool.LiveTransactionCount());
    EXPECT_EQ(0u, pool.IngressThisQuantum());
}

// The transaction pool admits to the ceiling at 95% and 100%, refuses the whole
// transaction at 100%+1, and never moves its high-water mark on the refusal.
TEST(ScheduledTransactionPool, TransactionCapacityAt95_100_101) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxScheduledTransactions = 20;
    c.maxIngressTransactionsPerQuantum = 64;
    c.maxEventsPerQuantum = 64;
    c.maxFrameBatchesPerQuantum = 64;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));

    for (std::uint32_t i = 0; i < 19; ++i) { // 95%
        ASSERT_EQ(ReserveFrames(pool, h, 100 + i, {10 + i}).status,
                  ReserveStatus::Committed);
    }
    EXPECT_EQ(pool.LiveTransactionCount(), 19u);
    EXPECT_EQ(pool.TransactionHighWater(), 19u);

    ASSERT_EQ(ReserveFrames(pool, h, 200, {90}).status, // 100%
              ReserveStatus::Committed);
    EXPECT_EQ(pool.TransactionHighWater(), 20u);

    const ReserveResult over = ReserveFrames(pool, h, 300, {95}); // 100%+1
    EXPECT_EQ(over.refusal, ReserveRefusal::PoolFull);
    EXPECT_EQ(pool.TransactionHighWater(), 20u); // unchanged by the refusal
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::PoolFull), 1u);
}

// The per-page event ceiling behaves the same at 95%/100%/100%+1: events admit to
// the ceiling and the whole over-ceiling transaction is refused.
TEST(ScheduledTransactionPool, PageEventCapacityAt95_100_101) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxEventsPerQuantum = 20;
    c.maxFrameBatchesPerQuantum = 20;
    c.maxTransactionEvents = 20;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));

    // Fill page 0 to 19 events (95%) with one transaction, then one more (100%).
    std::vector<std::uint64_t> frames;
    for (std::uint64_t f = 0; f < 19; ++f) {
        frames.push_back(f);
    }
    {
        std::vector<ScheduledEventInput> events;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            ScheduledEventInput e;
            e.executeFrame = frames[i];
            e.eventIndex = static_cast<std::uint32_t>(i);
            events.push_back(e);
        }
        TransactionRequest req;
        req.transactionId = 1;
        req.transportEpoch = kEpoch;
        req.ownerInstanceHandle = h.owner;
        req.ownerInstanceScheduleEpoch = h.ownerEpoch;
        req.events = events.data();
        req.eventCount = events.size();
        ASSERT_EQ(pool.Reserve(req, PrePublishLatePolicy::Refuse).status,
                  ReserveStatus::Committed);
    }
    EXPECT_EQ(pool.EventHighWater(), 19u);
    ASSERT_EQ(ReserveFrames(pool, h, 2, {19}).status, // 100%
              ReserveStatus::Committed);
    EXPECT_EQ(pool.EventHighWater(), 20u);

    const ReserveResult over = ReserveFrames(pool, h, 3, {20}); // 100%+1
    EXPECT_EQ(over.refusal, ReserveRefusal::PageEventCapacity);
    EXPECT_EQ(pool.EventHighWater(), 20u);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::PageEventCapacity), 1u);
}

// The per-page batch ceiling counts distinct execute frames: events sharing a
// frame are one batch, and a transaction introducing one distinct frame too many
// is refused whole.
TEST(ScheduledTransactionPool, PageBatchCeilingCountsDistinctFrames) {
    Harness h;
    ScheduledTransactionPool::Config c = MakeConfig();
    c.maxFrameBatchesPerQuantum = 2;
    c.maxEventsPerQuantum = 16;
    c.maxTransactionEvents = 16;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));

    // Four events across two distinct frames: two batches, admitted.
    ASSERT_EQ(ReserveFrames(pool, h, 1, {10, 10, 20, 20}).status,
              ReserveStatus::Committed);
    EXPECT_EQ(pool.FrameBatchHighWater(), 2u);
    // A third distinct frame would be a third batch: refused whole.
    const ReserveResult over = ReserveFrames(pool, h, 2, {30});
    EXPECT_EQ(over.refusal, ReserveRefusal::PageBatchCapacity);
    EXPECT_EQ(pool.FrameBatchHighWater(), 2u);
}

// Reset rejects a degenerate configuration and the reserved zero transport epoch.
TEST(ScheduledTransactionPool, ResetValidatesConfigAndEpoch) {
    ScheduledTransactionPool pool;
    EXPECT_FALSE(pool.Valid());

    EXPECT_FALSE(pool.Reset(MakeConfig(), 0, harken::kInvalidTransportEpoch));

    ScheduledTransactionPool::Config c = MakeConfig();
    c.scheduleHorizonFrames = 350; // not a whole number of 100-frame quanta
    EXPECT_FALSE(pool.Reset(c, 0, kEpoch));

    c = MakeConfig();
    EXPECT_FALSE(pool.Reset(c, 50, kEpoch)); // start not quantum-aligned

    c = MakeConfig();
    c.maxEventsPerQuantum = 0;
    EXPECT_FALSE(pool.Reset(c, 0, kEpoch));

    EXPECT_TRUE(pool.Reset(MakeConfig(), 0, kEpoch));
    EXPECT_TRUE(pool.Valid());
    EXPECT_EQ(pool.Epoch(), kEpoch);
}

// The clock and the pool compose: musical ticks convert to frames through the
// ClockKernel, file into the horizon, and consume back at exactly those frames,
// with the tick recoverable from each.
// This fixture uses an absolute entry anchor of zero. Production callers add the
// signed entry-relative frame to the instance's absolute anchor before filing and
// reject a negative result instead of casting it to the unsigned pool coordinate.
TEST(ScheduledTransactionPool, ClockKernelDrivesFrameScheduling) {
    Harness h;
    TempoSegment seg{MusicalTick{0}, 500'000}; // 120 BPM => 25 frames/tick at 48k
    ClockKernel clock;
    ASSERT_TRUE(clock.Reset(48000, &seg, 1));

    ScheduledTransactionPool::Config c = MakeConfig();
    c.quantumFrames = 100;
    c.scheduleHorizonFrames = 400; // frames 0..399
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));

    // Ticks 0/4/8/12 map to frames 0/100/200/300 (25 frames/tick), one per page.
    const std::int64_t ticks[] = {0, 4, 8, 12};
    std::vector<ScheduledEventInput> events;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::int64_t frame = clock.TickToFrame(MusicalTick{ticks[i]}).value;
        ASSERT_GE(frame, 0);
        ScheduledEventInput e;
        e.executeFrame = static_cast<std::uint64_t>(frame);
        e.eventIndex = static_cast<std::uint32_t>(i);
        e.payload = static_cast<std::uint64_t>(ticks[i]);
        events.push_back(e);
    }
    TransactionRequest req;
    req.transactionId = 1;
    req.transportEpoch = kEpoch;
    req.ownerInstanceHandle = h.owner;
    req.ownerInstanceScheduleEpoch = h.ownerEpoch;
    req.events = events.data();
    req.eventCount = events.size();
    ASSERT_EQ(pool.Reserve(req, PrePublishLatePolicy::Refuse).status,
              ReserveStatus::Committed);

    // Each page carries its one event at the expected frame, and the tick round
    // -trips from the consumed frame.
    const std::uint64_t expectedFrames[] = {0, 100, 200, 300};
    for (std::uint32_t page = 0; page < 4; ++page) {
        std::array<ConsumedEvent, 4> out{};
        const std::size_t n = pool.ConsumePage(page, kEpoch, h.instances,
                                               h.occurrences, out.data(),
                                               out.size());
        ASSERT_EQ(n, 1u) << "page=" << page;
        EXPECT_EQ(out[0].executeFrame, expectedFrames[page]);
        const MusicalTick back =
            clock.FrameToTick(SampleFrame{static_cast<std::int64_t>(
                out[0].executeFrame)});
        EXPECT_EQ(back.value, ticks[page]);
    }
}

// The window's far edge must fit in uint64: a start frame whose sum with the
// horizon would wrap is refused, so the page arithmetic never folds into the past.
TEST(ScheduledTransactionPool, RejectsStartFramePlusHorizonOverflow) {
    ScheduledTransactionPool pool;
    ScheduledTransactionPool::Config c = MakeConfig(); // 400-frame horizon
    // A quantum-aligned start frame so close to the top that start + horizon wraps.
    const std::uint64_t aligned = (UINT64_MAX / c.quantumFrames) * c.quantumFrames;
    EXPECT_FALSE(pool.Reset(c, aligned, kEpoch));
    // A start frame with room for the horizon is accepted.
    const std::uint64_t safe = aligned - c.scheduleHorizonFrames;
    EXPECT_TRUE(pool.Reset(c, safe, kEpoch));
}

TEST(ScheduledTransactionPool,
     ResetForEpochOverflowLeavesTheExistingWindowUntouched) {
    Harness h;
    ScheduledTransactionPool pool;
    const ScheduledTransactionPool::Config c = MakeConfig();
    ASSERT_TRUE(pool.Reset(c, 0, kEpoch));
    ASSERT_EQ(ReserveStatus::Committed,
              ReserveFrames(pool, h, 1, {150}).status);
    pool.Publish(100);

    const std::uint64_t aligned =
        (UINT64_MAX / c.quantumFrames) * c.quantumFrames;
    EXPECT_FALSE(pool.ResetForEpoch(aligned, kEpoch + 1));
    EXPECT_TRUE(pool.Valid());
    EXPECT_EQ(kEpoch, pool.Epoch());
    EXPECT_EQ(0u, pool.WindowStartFrame());
    EXPECT_EQ(c.scheduleHorizonFrames, pool.WindowEndFrame());
    EXPECT_EQ(1u, pool.LiveTransactionCount());
    EXPECT_EQ(100u, pool.PublishCutoffFrame());
    EXPECT_TRUE(pool.IsFrozen(0));
}

TEST(ScheduledTransactionPool, AdvanceFailsBeforeTheFarEdgeCanWrap) {
    ScheduledTransactionPool::Config c = MakeConfig();
    c.quantumFrames = 1;
    c.scheduleHorizonFrames = 4;
    c.publishLeadQuanta = 1;
    const std::uint64_t start = UINT64_MAX - c.scheduleHorizonFrames;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(c, start, kEpoch));
    pool.Publish(start + 1);
    ASSERT_EQ(pool.WindowEndFrame(), UINT64_MAX);
    ASSERT_EQ(pool.PublishCutoffFrame(), start + 1);

    EXPECT_EQ(pool.Advance(), 0u);
    EXPECT_FALSE(pool.Valid());
    EXPECT_EQ(pool.WindowStartFrame(), start);
    EXPECT_EQ(pool.WindowEndFrame(), UINT64_MAX);

    pool.Publish(UINT64_MAX);
    EXPECT_EQ(pool.PublishCutoffFrame(), start + 1);
    EXPECT_FALSE(pool.PageForFrame(start, nullptr));
    EXPECT_EQ(pool.Page(0), nullptr);
    EXPECT_FALSE(pool.IsFrozen(0));

    Harness h;
    EXPECT_EQ(ReserveFrames(pool, h, 1, {start}).status,
              ReserveStatus::Refused);
    std::array<ConsumedEvent, 1> out{};
    EXPECT_EQ(pool.ConsumePage(0, kEpoch, h.instances, h.occurrences,
                               out.data(), out.size()),
              0u);
    EXPECT_EQ(pool.Advance(), 0u);
    EXPECT_FALSE(pool.Valid());
    EXPECT_EQ(pool.WindowStartFrame(), start);
    EXPECT_EQ(pool.WindowEndFrame(), UINT64_MAX);
}

// The publish lead must leave at least one writable page. A lead equal to (or
// past) the page count would freeze the whole window; a lead of zero is legal (no
// default look-ahead publishing).
TEST(ScheduledTransactionPool, RejectsPublishLeadBeyondPageCount) {
    ScheduledTransactionPool::Config c = MakeConfig(); // 4 pages (400 / 100)
    c.publishLeadQuanta = 4; // == pageCount: the whole window would freeze
    ScheduledTransactionPool tooWide;
    EXPECT_FALSE(tooWide.Reset(c, 0, kEpoch));

    c.publishLeadQuanta = 3; // pageCount - 1: one writable page remains
    ScheduledTransactionPool ok;
    EXPECT_TRUE(ok.Reset(c, 0, kEpoch));

    c.publishLeadQuanta = 0; // no default look-ahead: legal
    ScheduledTransactionPool none;
    EXPECT_TRUE(none.Reset(c, 0, kEpoch));
    EXPECT_EQ(none.PublishLeadQuanta(), 0u);
}
