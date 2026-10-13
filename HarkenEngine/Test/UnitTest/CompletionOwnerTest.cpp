#include "Common/ObjectIdTestSupport.h"
#include "harken/core/CompletionOwner.h"
#include "harken/core/CompositeCompletionOwnerPool.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/LogicCommitCoordinator.h"

#include <atomic>
#include <cassert>
#include <array>
#include <gtest/gtest.h>
#include <thread>
#include <type_traits>
#include <utility>

namespace harken::detail {

struct CompletionOwnerTestAccess {
    static void SeedFreeOccurrenceGeneration(OccurrenceStatePool& pool, std::uint32_t slot, std::uint32_t generation) {
        assert(pool.m_entries[slot].stage == OccurrenceStage::Free && pool.m_entries[slot].originalSinglePinCount == 0);
        pool.m_entries[slot].generation = generation;
    }
    static void SeedNextOriginalMarker(OccurrenceStatePool& pool, std::uint64_t marker) {
        pool.m_nextOriginalSingleMarker = marker;
    }
    // Observations do not issue an origin or write its native marker/count.
    struct OriginalSingleObservation {
        std::uint64_t marker = 0;
        std::uint32_t count = 0;
        CompletionOwnerRef owner{};
        std::uint32_t renderGeneration = 0;
        std::uint64_t scheduleEpoch = 0;
        const CompletionProgressRow* progress = nullptr;
    };
    static bool ObserveOriginalSingle(const OccurrenceStatePool& pool,
        OccurrenceToken token, OriginalSingleObservation* out) {
        const auto* entry = pool.Find(token);
        if (entry == nullptr || out == nullptr) return false;
        *out = {entry->originalSingleMarker, entry->originalSinglePinCount,
            entry->originalSingleOwner, entry->originalSingleRenderGeneration,
            entry->originalSingleScheduleEpoch, &entry->completionProgress};
        return true;
    }
    static void SetAfterCompletionOwnerClearHook(
        OccurrenceStatePool& pool, void (*hook)(void*), void* context) {
        pool.m_afterCompletionOwnerClearHookForTest = hook;
        pool.m_afterCompletionOwnerClearHookContextForTest = context;
    }
};

struct CompositeCompletionOwnerPoolTestAccess {
    static void SeedGeneration(CompositeCompletionOwnerPool& pool,
                               std::uint32_t slot,
                               std::uint32_t generation) {
        pool.rows_[slot].generation = generation;
    }
    static void SeedNextOriginalMarker(CompositeCompletionOwnerPool& pool, std::uint64_t marker) {
        pool.nextOriginalMarker_ = marker;
    }
    static std::uint32_t OriginalCount(const CompositeCompletionOwnerPool& pool,
                                      CompositeCompletionOwnerToken token) {
        return pool.rows_[token.slot].originalPinCount;
    }
    static void SeedOriginalCount(CompositeCompletionOwnerPool& pool,
                                  CompositeCompletionOwnerToken token, std::uint32_t count) {
        pool.rows_[token.slot].originalPinCount = count;
    }
};

} // namespace harken::detail

namespace {

harken::KnownScheduleLeafV1 MakeLeaf(harken::OccurrenceToken occurrence,
                                    std::uint32_t renderGeneration,
                                    std::uint64_t factsRevision,
                                    std::uint32_t ordinal,
                                    std::uint64_t start,
                                    std::uint64_t complete) {
    harken::KnownScheduleLeafV1 leaf{};
    leaf.stableLeafOrdinal = ordinal;
    leaf.authoredOffset.denominator = 1;
    leaf.occurrence = occurrence;
    leaf.renderGeneration = renderGeneration;
    leaf.absoluteLeafStartFrame = start;
    leaf.predicate.occurrence = occurrence;
    leaf.predicate.renderGeneration = renderGeneration;
    leaf.predicate.absoluteLogicalStartFrame = start;
    leaf.predicate.absoluteLogicalCompleteFrame = complete;
    leaf.predicate.totalLogicalOutputFrames = complete - start;
    leaf.predicate.playbackFactsRevision = factsRevision;
    leaf.predicate.program.kind = harken::PlayableKindV1::Sound;
    leaf.predicate.program.stableId = harkentest::ObjectId(101);
    leaf.predicate.program.world = 13;
    leaf.predicate.program.runtimeSemanticFingerprint = 17;
    return leaf;
}

void BindProgress(harken::OccurrenceStatePool& occurrences,
                  harken::OccurrenceToken occurrence,
                  std::uint32_t renderGeneration,
                  std::uint64_t factsRevision) {
    ASSERT_TRUE(occurrences.BindCompletionProgress(
        occurrence, renderGeneration, factsRevision));
}

class OriginalCompositeStorageTest : public testing::Test {
protected:
    using Pin = harken::CompositeCompletionOwnerPool::OriginalLifetimePin;
    void SetUp() override {
        ASSERT_TRUE(occurrences.Acquire({}, &composed));
        ASSERT_TRUE(occurrences.Acquire({}, &early));
        ASSERT_TRUE(occurrences.Acquire({}, &late));
        ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, early, 31, 41));
        ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, late, 37, 43));
        leaves = {MakeLeaf(late, 37, 43, 9, 150, 430), MakeLeaf(early, 31, 41, 3, 100, 260)};
    }
    harken::CompositeCompletionAcquireStatus Prepare(Pin* cap, harken::CompositeCompletionOwnerToken* out) {
        return pool.PrepareAcquire(composed, 47, leaves.data(), leaves.size(), occurrences, out, cap);
    }
    harken::OccurrenceStatePool occurrences{3};
    harken::CompositeCompletionOwnerPool pool{1, 2};
    harken::OccurrenceToken composed{}, early{}, late{};
    std::array<harken::KnownScheduleLeafV1, 2> leaves{};
    harken::CompositeCompletionOwnerToken token{};
    Pin original;
};

TEST_F(OriginalCompositeStorageTest, SamePrepareLoanAllowsActualCommitAndPreservesEveryStrictFence) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    EXPECT_FALSE(pool.IsOriginalLifetimePinned(original));
    harken::CompositeCompletionOwnerInfo retained{};
    ASSERT_TRUE(pool.CaptureOriginal(original, &retained));
    EXPECT_EQ(47u, retained.aggregateRevision);
    EXPECT_EQ(430u, retained.absoluteLogicalCompleteFrame);
    EXPECT_FALSE(pool.Capture(token, &retained));
    Pin foreign;
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    EXPECT_TRUE(pool.IsOriginalLifetimePinned(foreign));
    for (const auto& leaf : leaves) {
        harken::CompletionProgressBlockUpdate update{};
        update.occurrence = leaf.occurrence;
        update.renderGeneration = leaf.renderGeneration;
        update.playbackFactsRevision = leaf.predicate.playbackFactsRevision;
        update.completedThroughBlockSequence = 9;
        ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
            harken::PublishCompletionProgressBlock(*occurrences.CompletionProgressOf(leaf.occurrence), update));
    }
    EXPECT_FALSE(pool.Release(token, occurrences, 9));
    EXPECT_EQ(1u, pool.OccupiedCount());
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(late, &owner));
    EXPECT_FALSE(pool.IsOriginalLifetimePinned(foreign));
    ASSERT_TRUE(pool.Release(token, occurrences, 10));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.FreeCount());
    EXPECT_FALSE(pool.Capture(token, &retained));
    ASSERT_TRUE(pool.CaptureOriginal(foreign, &retained));
    EXPECT_EQ(430u, retained.absoluteLogicalCompleteFrame);
    harken::KnownScheduleLeafV1 retainedLeaf{};
    ASSERT_TRUE(pool.CaptureOriginalLeaf(foreign, 0, &retainedLeaf));
    EXPECT_EQ(early.slot, retainedLeaf.occurrence.slot);
    EXPECT_EQ(early.generation, retainedLeaf.occurrence.generation);
    EXPECT_EQ(3u, retainedLeaf.stableLeafOrdinal);
    EXPECT_TRUE(harken::SameCompletionOwner(retainedLeaf.predicate.owner,
        harken::CompletionOwnerRef::Composite(token, 41)));
    Pin refused;
    harken::CompositeCompletionOwnerToken unchanged = token;
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Capacity, Prepare(&refused, &unchanged));
    EXPECT_EQ(token.generation, unchanged.generation);
    original.Release();
    EXPECT_TRUE(foreign.Valid());
    EXPECT_EQ(0u, pool.FreeCount());
    foreign.Release();
    EXPECT_EQ(1u, pool.FreeCount());
    Pin next;
    harken::CompositeCompletionOwnerToken nextToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&next, &nextToken));
    EXPECT_EQ(token.slot, nextToken.slot);
    EXPECT_EQ(token.generation + 1u, nextToken.generation);
    EXPECT_FALSE(pool.IsOriginalLifetimePinFor(next, token));
    ASSERT_TRUE(pool.CancelPrepared(nextToken));
}

TEST_F(OriginalCompositeStorageTest, ActualCancelRetainsFactoryStorageUntilLastMoveOnlyLoanReturns) {
    static_assert(!std::is_copy_constructible_v<Pin> && std::is_nothrow_move_constructible_v<Pin>);
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    Pin foreign, overwritten;
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_TRUE(original.Borrow(&overwritten));
    ASSERT_TRUE(pool.CancelPrepared(token));
    EXPECT_EQ(0u, pool.FreeCount());
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_FALSE(pool.IsOriginalLifetimePinned(foreign));
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, pool.CommitPrepared(token, occurrences));
    original.Release();
    overwritten = std::move(foreign);
    EXPECT_FALSE(foreign.Valid());
    EXPECT_TRUE(overwritten.Valid());
    EXPECT_EQ(1u, harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(pool, token));
    Pin last;
    ASSERT_TRUE(overwritten.Borrow(&last));
    overwritten.Release();
    EXPECT_EQ(0u, pool.FreeCount());
    last.Release();
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, ActualRollbackClearsOwnersBeforeRetainedStorageRecycles) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    Pin foreign;
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    ASSERT_TRUE(pool.RollbackBeforePublication(token, occurrences));
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(late, &owner));
    EXPECT_FALSE(pool.IsOriginalLifetimePinned(foreign));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.FreeCount());
    original.Release();
    ASSERT_TRUE(foreign.Valid());
    foreign.Release();
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, EqualTokensFromDifferentActualReceiversAndOccupiedOutputsNeverRegrant) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    harken::CompositeCompletionOwnerPool other{1, 2};
    Pin otherOriginal;
    harken::CompositeCompletionOwnerToken otherToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        other.PrepareAcquire(composed, 47, leaves.data(), leaves.size(), occurrences, &otherToken, &otherOriginal));
    ASSERT_EQ(token.slot, otherToken.slot);
    ASSERT_EQ(token.generation, otherToken.generation);
    EXPECT_FALSE(pool.IsOriginalLifetimePinFor(otherOriginal, token));
    EXPECT_FALSE(other.IsOriginalLifetimePinFor(original, otherToken));
    EXPECT_FALSE(original.Borrow(&otherOriginal));
    auto unchanged = token;
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, Prepare(&otherOriginal, &unchanged));
    EXPECT_TRUE(other.IsOriginalLifetimePinFor(otherOriginal, otherToken));
    EXPECT_EQ(1u, harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(pool, token));
    EXPECT_EQ(1u, harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(other, otherToken));
    ASSERT_TRUE(pool.CancelPrepared(token));
    ASSERT_TRUE(other.CancelPrepared(otherToken));
}

TEST_F(OriginalCompositeStorageTest, ReturningAllLoansEarlyDoesNotCancelOrCommitTheRealNativeOwner) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    original.Release();
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(pool.IsOriginalLifetimePinFor(original, token));
    EXPECT_EQ(0u, pool.FreeCount());
    harken::CompositeCompletionOwnerInfo info{};
    ASSERT_TRUE(pool.CapturePrepared(token, &info));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    ASSERT_TRUE(pool.Capture(token, &info));
    ASSERT_TRUE(pool.Release(token, occurrences, 1));
    EXPECT_EQ(1u, pool.FreeCount());
    Pin next;
    harken::CompositeCompletionOwnerToken nextToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&next, &nextToken));
    EXPECT_EQ(token.generation + 1u, nextToken.generation);
    EXPECT_FALSE(pool.IsOriginalLifetimePinFor(next, token));
    ASSERT_TRUE(pool.CancelPrepared(nextToken));
}

TEST_F(OriginalCompositeStorageTest, GenuineForeignLoanDoesNotBlockOriginalPrivateGeometryReanchor) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    Pin foreign;
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    std::array<harken::KnownScheduleLeafV1, 2> candidates{};
    for (std::uint32_t index = 0; index < candidates.size(); ++index) {
        ASSERT_TRUE(pool.CaptureLeaf(token, index, &candidates[index]));
        auto& leaf = candidates[index];
        harken::KnownCompletionPredicateV1 moved{};
        ASSERT_TRUE(harken::ReanchorKnownCompletionBeforePublication(leaf.predicate,
            leaf.absoluteLeafStartFrame + 64u, &moved));
        leaf.absoluteLeafStartFrame += 64u;
        leaf.predicate = moved;
    }
    harken::CompositeCompletionOwnerPool::PrivateReanchorProof proof;
    ASSERT_TRUE(pool.ProvePrivateReanchorBeforePublication(token, candidates.data(), candidates.size(), occurrences, &proof));
    pool.ApplyPrivateReanchorBeforePublication(proof);
    harken::CompositeCompletionOwnerInfo actual{};
    ASSERT_TRUE(pool.CaptureOriginal(foreign, &actual));
    EXPECT_EQ(494u, actual.absoluteLogicalCompleteFrame);
    EXPECT_EQ(47u, actual.aggregateRevision);
    harken::KnownScheduleLeafV1 nativeLeaf{};
    ASSERT_TRUE(pool.CaptureOriginalLeaf(foreign, 0, &nativeLeaf));
    EXPECT_EQ(164u, nativeLeaf.absoluteLeafStartFrame);
    EXPECT_EQ(early.generation, nativeLeaf.occurrence.generation);
    EXPECT_EQ(41u, nativeLeaf.predicate.playbackFactsRevision);
    ASSERT_TRUE(pool.RollbackBeforePublication(token, occurrences));
}

TEST_F(OriginalCompositeStorageTest, LastAuthenticMarkerIssuesOnceAndExhaustionLeavesLegacyPreparationHealthy) {
    harken::detail::CompositeCompletionOwnerPoolTestAccess::SeedNextOriginalMarker(pool, UINT64_MAX);
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    ASSERT_TRUE(original.Valid());
    ASSERT_TRUE(pool.CancelPrepared(token));
    original.Release();
    Pin refused;
    auto unchanged = token;
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, Prepare(&refused, &unchanged));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(token.generation, unchanged.generation);
    EXPECT_EQ(1u, pool.FreeCount());
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(nullptr, &unchanged));
    EXPECT_EQ(token.generation + 1u, unchanged.generation);
    ASSERT_TRUE(pool.CancelPrepared(unchanged));
}

TEST_F(OriginalCompositeStorageTest, BorrowCountOverflowRefusesWithoutChangingTheActualEpisode) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    const auto before = harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(pool, token);
    harken::detail::CompositeCompletionOwnerPoolTestAccess::SeedOriginalCount(pool, token, UINT32_MAX);
    Pin refused;
    EXPECT_FALSE(original.Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(UINT32_MAX, harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(pool, token));
    harken::detail::CompositeCompletionOwnerPoolTestAccess::SeedOriginalCount(pool, token, before);
    ASSERT_TRUE(pool.CancelPrepared(token));
    original.Release();
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, RealCommittedLeafLoanKeepsOriginalProgressUntilActualLastReturn) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin leafLoan;
    EXPECT_FALSE(pool.BorrowOriginalLeafCompletion(original, 0, &leafLoan));
    EXPECT_FALSE(leafLoan.Valid());
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    ASSERT_TRUE(pool.BorrowOriginalLeafCompletion(original, 0, &leafLoan));
    EXPECT_FALSE(pool.BorrowOriginalLeafCompletion(original, 1, &leafLoan));
    ASSERT_TRUE(occurrences.IsOriginalCompositeCompletionOwnerPinFor(leafLoan, early));
    EXPECT_FALSE(occurrences.IsOriginalCompositeCompletionOwnerPinFor(leafLoan, late));
    const auto owner = harken::CompletionOwnerRef::Composite(token, 41);
    const auto* progress = occurrences.CompletionProgressOf(early);
    ASSERT_NE(nullptr, progress);
    EXPECT_TRUE(occurrences.IsOriginalCompositeCompletionOwnerCurrent(
        leafLoan, owner, progress, 31, occurrences.ScheduleEpochOf(early)));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = early;
    update.renderGeneration = 31;
    update.playbackFactsRevision = 41;
    update.completedThroughBlockSequence = 9;
    update.sourceOutputFramesThisBlock = 32;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
        harken::PublishCompletionProgressBlock(*occurrences.CompletionProgressOf(early), update));
    EXPECT_FALSE(pool.Release(token, occurrences, 9));
    ASSERT_TRUE(pool.Release(token, occurrences, 10));
    EXPECT_TRUE(leafLoan.Valid());
    EXPECT_FALSE(occurrences.IsOriginalCompositeCompletionOwnerCurrent(
        leafLoan, owner, progress, 31, occurrences.ScheduleEpochOf(early)));
    EXPECT_FALSE(pool.BorrowOriginalLeafCompletion(original, 0, &leafLoan));
    harken::CompletionProgressSnapshot latest{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*progress, early, 31, &latest));
    EXPECT_EQ(9u, latest.completedThroughBlockSequence);
    EXPECT_EQ(32u, latest.logicalOutputFramesProduced);
    ASSERT_TRUE(occurrences.Advance(early, harken::OccurrenceStage::Active));
    ASSERT_TRUE(occurrences.Advance(early, harken::OccurrenceStage::LogicalDone));
    ASSERT_TRUE(occurrences.Advance(early, harken::OccurrenceStage::AwaitingEventFence));
    EXPECT_FALSE(occurrences.ReleaseAfterFence(early, 9));
    EXPECT_EQ(0u, occurrences.RetainedStorageCount());
    ASSERT_TRUE(occurrences.ReleaseAfterFence(early, 10));
    EXPECT_EQ(harken::OccurrenceStage::Free, occurrences.StageOf(early));
    EXPECT_EQ(2u, occurrences.OccupiedCount());
    EXPECT_EQ(0u, occurrences.AwaitingFenceCount());
    EXPECT_EQ(1u, occurrences.RetainedStorageCount());
    EXPECT_EQ(0u, occurrences.FreeCount());
    EXPECT_EQ(nullptr, occurrences.CompletionProgressOf(early));
    EXPECT_FALSE(occurrences.BindCompletionProgress(early, 31, 41));
    EXPECT_FALSE(occurrences.TryPinCompletionOwner(early, owner));
    EXPECT_EQ(harken::IssueStatus::Exhausted, occurrences.BumpScheduleEpochChecked(early).status);
    EXPECT_TRUE(occurrences.IsOriginalCompositeCompletionOwnerPinFor(leafLoan, early));
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*progress, early, 31, &latest));
    EXPECT_EQ(9u, latest.completedThroughBlockSequence);
    EXPECT_EQ(32u, latest.logicalOutputFramesProduced);
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin last;
    ASSERT_TRUE(leafLoan.Borrow(&last));
    leafLoan.Release();
    EXPECT_FALSE(occurrences.ReleaseAfterFence(early, 10));
    EXPECT_EQ(0u, occurrences.FreeCount());
    last.Release();
    EXPECT_EQ(0u, occurrences.RetainedStorageCount());
    EXPECT_EQ(1u, occurrences.FreeCount());
    harken::OccurrenceToken replacement{};
    ASSERT_TRUE(occurrences.Acquire({}, &replacement));
    EXPECT_EQ(early.slot, replacement.slot);
    EXPECT_GT(replacement.generation, early.generation);
}

TEST_F(OriginalCompositeStorageTest, RealPreparedLeafCancellationRetiresNativeOwnershipBeforeForeignStorage) {
    harken::OccurrenceStatePool prepared{2};
    harken::CompositeCompletionOwnerPool actual{1, 1};
    harken::OccurrenceToken aggregate{}, leaf{};
    ASSERT_TRUE(prepared.PrepareAcquire({}, &aggregate));
    ASSERT_TRUE(prepared.PrepareAcquire({}, &leaf));
    ASSERT_TRUE(prepared.BindCompletionProgress(leaf, 31, 41));
    const auto known = MakeLeaf(leaf, 31, 41, 1, 100, 260);
    Pin rowLoan;
    harken::CompositeCompletionOwnerToken rowToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        actual.PrepareAcquire(aggregate, 47, &known, 1, prepared, &rowToken, &rowLoan));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, actual.CommitPrepared(rowToken, prepared));
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin foreign;
    ASSERT_TRUE(actual.BorrowOriginalLeafCompletion(rowLoan, 0, &foreign));
    ASSERT_TRUE(actual.RollbackBeforePublication(rowToken, prepared));
    ASSERT_TRUE(prepared.UnbindCompletionProgress(leaf, 31));
    ASSERT_TRUE(prepared.CancelPreparedAcquire(leaf));
    EXPECT_EQ(1u, prepared.OccupiedCount());
    EXPECT_EQ(1u, prepared.RetainedStorageCount());
    EXPECT_EQ(0u, prepared.FreeCount());
    EXPECT_TRUE(prepared.IsOriginalCompositeCompletionOwnerPinFor(foreign, leaf));
    harken::OccurrenceToken refused{};
    EXPECT_FALSE(prepared.PrepareAcquire({}, &refused));
    EXPECT_EQ(0u, refused.generation);
    EXPECT_FALSE(prepared.CancelPreparedAcquire(leaf));
    foreign.Release();
    EXPECT_EQ(0u, prepared.RetainedStorageCount());
    EXPECT_EQ(1u, prepared.FreeCount());
    harken::OccurrenceToken next{};
    ASSERT_TRUE(prepared.PrepareAcquire({}, &next));
    EXPECT_EQ(leaf.slot, next.slot);
    EXPECT_EQ(leaf.generation, next.generation);
    ASSERT_TRUE(prepared.CancelPreparedAcquire(next));
    ASSERT_TRUE(prepared.CancelPreparedAcquire(aggregate));
    EXPECT_EQ(2u, prepared.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, EarlyForeignLeafReturnDoesNotRetireTheActualNativeOwner) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin foreign;
    ASSERT_TRUE(pool.BorrowOriginalLeafCompletion(original, 0, &foreign));
    foreign.Release();
    harken::CompletionOwnerRef owner{};
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_EQ(harken::CompletionOwnerKind::Composite, owner.kind);
    EXPECT_EQ(0u, occurrences.RetainedStorageCount());
    EXPECT_EQ(3u, occurrences.OccupiedCount());
    ASSERT_TRUE(occurrences.Advance(early, harken::OccurrenceStage::AwaitingEventFence));
    EXPECT_FALSE(occurrences.ReleaseAfterFence(early, 1));
    EXPECT_EQ(3u, occurrences.OccupiedCount());
    ASSERT_TRUE(pool.Release(token, occurrences, 1));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(early, 1));
    EXPECT_EQ(0u, occurrences.RetainedStorageCount());
    EXPECT_EQ(1u, occurrences.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, RealMaximumLeafGenerationRemainsRetainedThenQuarantinesWithoutWrap) {
    harken::OccurrenceStatePool actualOccurrences{2};
    harken::CompositeCompletionOwnerPool actualComposite{1, 1};
    harken::detail::CompletionOwnerTestAccess::SeedFreeOccurrenceGeneration(actualOccurrences, 0, UINT32_MAX);
    harken::OccurrenceToken aggregate{}, leaf{};
    ASSERT_TRUE(actualOccurrences.Acquire({}, &leaf));
    ASSERT_TRUE(actualOccurrences.Acquire({}, &aggregate));
    ASSERT_EQ(UINT32_MAX, leaf.generation);
    ASSERT_TRUE(actualOccurrences.BindCompletionProgress(leaf, 31, 41));
    const auto known = MakeLeaf(leaf, 31, 41, 1, 100, 260);
    Pin rowLoan;
    harken::CompositeCompletionOwnerToken rowToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        actualComposite.PrepareAcquire(aggregate, 47, &known, 1, actualOccurrences, &rowToken, &rowLoan));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, actualComposite.CommitPrepared(rowToken, actualOccurrences));
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin foreign;
    ASSERT_TRUE(actualComposite.BorrowOriginalLeafCompletion(rowLoan, 0, &foreign));
    ASSERT_TRUE(actualComposite.Release(rowToken, actualOccurrences, 1));
    ASSERT_TRUE(actualOccurrences.Advance(leaf, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(actualOccurrences.ReleaseAfterFence(leaf, 1));
    EXPECT_EQ(0u, actualOccurrences.QuarantinedCount());
    EXPECT_EQ(1u, actualOccurrences.RetainedStorageCount());
    EXPECT_TRUE(actualOccurrences.IsOriginalCompositeCompletionOwnerPinFor(foreign, leaf));
    foreign.Release();
    EXPECT_EQ(0u, actualOccurrences.RetainedStorageCount());
    EXPECT_EQ(1u, actualOccurrences.QuarantinedCount());
    EXPECT_EQ(0u, actualOccurrences.FreeCount());
    harken::OccurrenceToken refused{};
    EXPECT_FALSE(actualOccurrences.Acquire({}, &refused));
    EXPECT_EQ(0u, refused.generation);
}

TEST_F(OriginalCompositeStorageTest, CompositeFactoryLeafOutputCannotEnterTheSingleOriginInterface) {
    static_assert(!std::is_convertible_v<harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin,
        harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin>);
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    harken::KnownScheduleLeafV1 actual{};
    ASSERT_TRUE(pool.CaptureOriginalLeaf(original, 0, &actual));
    harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin refusedSingle;
    EXPECT_FALSE(occurrences.TryPinCompletionOwner(actual.occurrence, actual.predicate.owner, &refusedSingle));
    EXPECT_FALSE(refusedSingle.Valid());
    harken::CompletionOwnerRef notBound{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(actual.occurrence, &notBound));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin actualLeaf;
    ASSERT_TRUE(pool.BorrowOriginalLeafCompletion(original, 0, &actualLeaf));
    EXPECT_TRUE(occurrences.IsOriginalCompositeCompletionOwnerPinFor(actualLeaf, actual.occurrence));
    ASSERT_TRUE(pool.RollbackBeforePublication(token, occurrences));
}

TEST_F(OriginalCompositeStorageTest, RealSecondLeafMarkerRefusalRollsBackEveryAlreadyBoundOriginalPrefix) {
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    harken::detail::CompletionOwnerTestAccess::SeedNextOriginalMarker(occurrences, UINT64_MAX);
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::LeafOwned, pool.CommitPrepared(token, occurrences));
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(late, &owner));
    harken::detail::CompletionOwnerTestAccess::OriginalSingleObservation earlyState{}, lateState{};
    ASSERT_TRUE(harken::detail::CompletionOwnerTestAccess::ObserveOriginalSingle(occurrences, early, &earlyState));
    ASSERT_TRUE(harken::detail::CompletionOwnerTestAccess::ObserveOriginalSingle(occurrences, late, &lateState));
    EXPECT_EQ(0u, earlyState.count);
    EXPECT_EQ(0u, earlyState.marker);
    EXPECT_EQ(0u, lateState.count);
    EXPECT_EQ(0u, lateState.marker);
    harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin notIssued;
    EXPECT_FALSE(pool.BorrowOriginalLeafCompletion(original, 0, &notIssued));
    EXPECT_FALSE(notIssued.Valid());
    ASSERT_TRUE(pool.CancelPrepared(token));
    original.Release();
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, RegisteredActualBaselineReturnsOnNativeEndAndPreservesForeignStorage) {
    auto* actualBaseline = &original;
    ASSERT_TRUE(pool.RegisterOriginalLifetimeBaselineResolver(&actualBaseline,
        [](void* context, harken::CompositeCompletionOwnerPool&, harken::CompositeCompletionOwnerToken) noexcept {
            return *static_cast<Pin**>(context);
        }));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    Pin foreign;
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    EXPECT_FALSE(pool.Release(token, occurrences, 0));
    EXPECT_TRUE(original.Valid());
    ASSERT_TRUE(pool.Release(token, occurrences, 1));
    EXPECT_FALSE(original.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_EQ(0u, pool.FreeCount());
    EXPECT_EQ(0u, pool.OccupiedCount());
    foreign.Release();
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, ForeignLoanCannotBeConsumedAsTheRegisteredFactoryBaseline) {
    Pin foreign;
    auto* actualBaseline = &foreign;
    ASSERT_TRUE(pool.RegisterOriginalLifetimeBaselineResolver(&actualBaseline,
        [](void* context, harken::CompositeCompletionOwnerPool&, harken::CompositeCompletionOwnerToken) noexcept {
            return *static_cast<Pin**>(context);
        }));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    ASSERT_TRUE(original.Borrow(&foreign));
    EXPECT_FALSE(pool.CancelPrepared(token));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_EQ(2u, harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(pool, token));
    harken::CompositeCompletionOwnerInfo stillPrepared{};
    EXPECT_TRUE(pool.CapturePrepared(token, &stillPrepared));
    actualBaseline = &original;
    ASSERT_TRUE(pool.CancelPrepared(token));
    EXPECT_FALSE(original.Valid());
    EXPECT_TRUE(foreign.Valid());
    foreign.Release();
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalCompositeStorageTest, RefusedActualDuplicateLeafDoesNotConsumeTheLastMarkerOrIssueOutput) {
    harken::detail::CompositeCompletionOwnerPoolTestAccess::SeedNextOriginalMarker(pool, UINT64_MAX);
    const auto correct = leaves;
    leaves[1] = leaves[0];
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, Prepare(&original, &token));
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(1u, pool.FreeCount());
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(late, &owner));
    leaves = correct;
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    EXPECT_TRUE(original.Valid());
    ASSERT_TRUE(pool.CancelPrepared(token));
}

TEST_F(OriginalCompositeStorageTest, MaximumRealGenerationRetainsOldStorageThenQuarantinesWithoutWrap) {
    harken::detail::CompositeCompletionOwnerPoolTestAccess::SeedGeneration(pool, 0, UINT32_MAX);
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, Prepare(&original, &token));
    ASSERT_EQ(UINT32_MAX, token.generation);
    Pin foreign;
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_TRUE(pool.CancelPrepared(token));
    EXPECT_EQ(0u, pool.QuarantinedCount());
    original.Release();
    EXPECT_TRUE(pool.IsOriginalLifetimePinFor(foreign, token));
    foreign.Release();
    EXPECT_EQ(1u, pool.QuarantinedCount());
    EXPECT_EQ(0u, pool.FreeCount());
    Pin refused;
    auto unchanged = token;
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Capacity, Prepare(&refused, &unchanged));
    EXPECT_EQ(UINT32_MAX, unchanged.generation);
}

TEST(CompletionOwner, SingleOwnerPinsInlineAndClearsOnlyExactIdentity) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire({}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 17, 77));

    const harken::CompletionOwnerRef owner =
        harken::CompletionOwnerRef::Single({41, 3}, 77);
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(occurrence, owner));
    harken::CompletionOwnerRef captured{};
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(occurrence, &captured));
    EXPECT_EQ(harken::CompletionOwnerKind::SingleQueue, captured.kind);
    EXPECT_EQ(41u, captured.singleDecision.slot);
    EXPECT_EQ(3u, captured.singleDecision.generation);
    EXPECT_EQ(77u, captured.factsRevision);

    EXPECT_FALSE(occurrences.TryPinCompletionOwner(
        occurrence, harken::CompletionOwnerRef::Single({42, 4}, 78)));
    EXPECT_FALSE(occurrences.ClearCompletionOwnerExact(
        occurrence, harken::CompletionOwnerRef::Single({41, 2}, 77)));
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(occurrence, &captured));
    EXPECT_EQ(3u, captured.singleDecision.generation);

    ASSERT_TRUE(occurrences.ClearCompletionOwnerExact(occurrence, owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(occurrence, &captured));
}

TEST(CompletionOwner, CaptureNeverAcceptsMixedOwnerAcrossClearAndRepin) {
    harken::CompletionOwnerBackref backref;
    const harken::CompletionOwnerRef first =
        harken::CompletionOwnerRef::Single({3, 5}, 7);
    const harken::CompletionOwnerRef second =
        harken::CompletionOwnerRef::Single({11, 13}, 17);
    ASSERT_TRUE(backref.TryPin(first));

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        harken::CompletionOwnerRef current = first;
        for (std::uint32_t i = 0; i < 100000; ++i) {
            const harken::CompletionOwnerRef next =
                harken::SameCompletionOwner(current, first) ? second : first;
            if (!backref.ClearExact(current) || !backref.TryPin(next)) {
                failed.store(true, std::memory_order_release);
                break;
            }
            current = next;
        }
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
        harken::CompletionOwnerRef captured{};
        if (backref.Capture(&captured) &&
            !harken::SameCompletionOwner(captured, first) &&
            !harken::SameCompletionOwner(captured, second)) {
            failed.store(true, std::memory_order_release);
            break;
        }
    }
    writer.join();
    EXPECT_FALSE(failed.load(std::memory_order_acquire));
}

TEST(CompletionOwner, PinnedOccurrenceCannotReleaseOrReuseBeforeExactClear) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire({}, &occurrence));
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 13, 11));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = occurrence;
    update.renderGeneration = 13;
    update.playbackFactsRevision = 11;
    update.completedThroughBlockSequence = 4;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(occurrence), update));
    const harken::CompletionOwnerRef owner =
        harken::CompletionOwnerRef::Single({5, 7}, 11);
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(occurrence, owner));
    ASSERT_TRUE(occurrences.Advance(
        occurrence, harken::OccurrenceStage::AwaitingEventFence));

    EXPECT_FALSE(occurrences.ReleaseAfterFence(occurrence, UINT64_MAX));
    harken::CompletionOwnerRef captured{};
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(occurrence, &captured));
    EXPECT_TRUE(harken::SameCompletionOwner(owner, captured));
    EXPECT_EQ(1u, occurrences.OccupiedCount());

    ASSERT_TRUE(occurrences.ClearCompletionOwnerExact(occurrence, owner));
    EXPECT_FALSE(occurrences.ReleaseAfterFence(occurrence, 4));
    EXPECT_EQ(harken::OccurrenceStage::AwaitingEventFence,
              occurrences.StageOf(occurrence));
    EXPECT_EQ(1u, occurrences.OccupiedCount());
    EXPECT_TRUE(occurrences.ReleaseAfterFence(occurrence, 5));
}

TEST(CompletionOwner, ReleaseFenceIncludesPublicationRacingAfterOwnerClear) {
    harken::OccurrenceStatePool occurrences(1);
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(occurrences.Acquire({}, &occurrence));
    BindProgress(occurrences, occurrence, 13, 11);
    const harken::CompletionOwnerRef owner =
        harken::CompletionOwnerRef::Single({5, 7}, 11);
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(occurrence, owner));
    ASSERT_TRUE(occurrences.Advance(
        occurrence, harken::OccurrenceStage::AwaitingEventFence));

    struct HookContext {
        harken::OccurrenceStatePool* pool;
        harken::OccurrenceToken occurrence;
    } context{&occurrences, occurrence};
    const auto publish = [](void* raw) {
        auto* context = static_cast<HookContext*>(raw);
        harken::CompletionProgressBlockUpdate update{};
        update.occurrence = context->occurrence;
        update.renderGeneration = 13;
        update.playbackFactsRevision = 11;
        update.completedThroughBlockSequence = 8;
        EXPECT_EQ(harken::CompletionProgressPublishStatus::Ok,
                  harken::PublishCompletionProgressBlock(
                      *context->pool->CompletionProgressOf(
                          context->occurrence),
                      update));
    };
    harken::detail::CompletionOwnerTestAccess::
        SetAfterCompletionOwnerClearHook(occurrences, publish, &context);

    ASSERT_TRUE(occurrences.ClearCompletionOwnerExact(occurrence, owner));
    harken::detail::CompletionOwnerTestAccess::
        SetAfterCompletionOwnerClearHook(occurrences, nullptr, nullptr);
    EXPECT_FALSE(occurrences.ReleaseAfterFence(occurrence, 8));
    EXPECT_TRUE(occurrences.ReleaseAfterFence(occurrence, 9));
}

TEST(CompletionOwner, CompositePinsStableLeavesAndUsesMaximumCompletion) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken early{};
    harken::OccurrenceToken late{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &early));
    ASSERT_TRUE(occurrences.Acquire({}, &late));
    ASSERT_TRUE(occurrences.BindCompletionProgress(early, 31, 41));
    ASSERT_TRUE(occurrences.BindCompletionProgress(late, 37, 43));

    harken::KnownScheduleLeafV1 leaves[2]{};
    leaves[0].stableLeafOrdinal = 9;
    leaves[0].authoredOffset.denominator = 1;
    leaves[0].occurrence = late;
    leaves[0].renderGeneration = 37;
    leaves[0].absoluteLeafStartFrame = 150;
    leaves[0].predicate.occurrence = late;
    leaves[0].predicate.renderGeneration = 37;
    leaves[0].predicate.absoluteLogicalStartFrame = 150;
    leaves[0].predicate.absoluteLogicalCompleteFrame = 430;
    leaves[0].predicate.totalLogicalOutputFrames = 280;
    leaves[0].predicate.playbackFactsRevision = 43;
    leaves[0].predicate.program.kind = harken::PlayableKindV1::Sound;
    leaves[0].predicate.program.stableId = harkentest::ObjectId(101);
    leaves[0].predicate.program.world = 13;
    leaves[0].predicate.program.runtimeSemanticFingerprint = 17;
    leaves[1].stableLeafOrdinal = 3;
    leaves[1].authoredOffset.denominator = 1;
    leaves[1].occurrence = early;
    leaves[1].renderGeneration = 31;
    leaves[1].absoluteLeafStartFrame = 100;
    leaves[1].predicate.occurrence = early;
    leaves[1].predicate.renderGeneration = 31;
    leaves[1].predicate.absoluteLogicalStartFrame = 100;
    leaves[1].predicate.absoluteLogicalCompleteFrame = 260;
    leaves[1].predicate.totalLogicalOutputFrames = 160;
    leaves[1].predicate.playbackFactsRevision = 41;
    leaves[1].predicate.program.kind = harken::PlayableKindV1::Sound;
    leaves[1].predicate.program.stableId = harkentest::ObjectId(103);
    leaves[1].predicate.program.world = 13;
    leaves[1].predicate.program.runtimeSemanticFingerprint = 19;

    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::CompositeCompletionOwnerToken token{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.Acquire(composed, 47, leaves, 2, occurrences, &token));
    harken::CompositeCompletionOwnerInfo info{};
    ASSERT_TRUE(pool.Capture(token, &info));
    EXPECT_EQ(composed.slot, info.composedOccurrence.slot);
    EXPECT_EQ(composed.generation, info.composedOccurrence.generation);
    EXPECT_EQ(47u, info.aggregateRevision);
    EXPECT_EQ(2u, info.leafCount);
    EXPECT_EQ(430u, info.absoluteLogicalCompleteFrame);

    harken::KnownScheduleLeafV1 captured{};
    ASSERT_TRUE(pool.CaptureLeaf(token, 0, &captured));
    EXPECT_EQ(3u, captured.stableLeafOrdinal);
    EXPECT_TRUE(harken::IsBoundKnownCompletionPredicate(captured.predicate));
    EXPECT_EQ(token.generation,
              captured.predicate.owner.composite.generation);
    ASSERT_TRUE(pool.CaptureLeaf(token, 1, &captured));
    EXPECT_EQ(9u, captured.stableLeafOrdinal);

    harken::CompletionOwnerRef owner{};
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_EQ(harken::CompletionOwnerKind::Composite, owner.kind);
    EXPECT_EQ(token.slot, owner.composite.slot);
    EXPECT_EQ(token.generation, owner.composite.generation);
    EXPECT_EQ(41u, owner.factsRevision);
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(late, &owner));
    EXPECT_EQ(token.generation, owner.composite.generation);
    EXPECT_EQ(43u, owner.factsRevision);
}

TEST(CompletionOwner, PrivateCompletionReanchorKeepsProgressIdentityAndBaseline) {
    harken::KnownScheduleLeafV1 leaf = MakeLeaf({3, 7}, 11, 19, 1, 256, 768);
    leaf.predicate.progressBaselineOutputFrames = 23;
    leaf.predicate.progressBaselineRevision = 5;
    leaf.predicate.progressBaselineCompletedThroughBlockSequence = 9;
    leaf.predicate.owner = harken::CompletionOwnerRef::Single({4, 6}, 19);
    harken::KnownCompletionPredicateV1 moved{};
    ASSERT_TRUE(harken::ReanchorKnownCompletionBeforePublication(
        leaf.predicate, 1024, &moved));
    EXPECT_EQ(1024u, moved.absoluteLogicalStartFrame);
    EXPECT_EQ(1536u, moved.absoluteLogicalCompleteFrame);
    EXPECT_EQ(23u, moved.progressBaselineOutputFrames);
    EXPECT_EQ(5u, moved.progressBaselineRevision);
    EXPECT_EQ(9u, moved.progressBaselineCompletedThroughBlockSequence);
    EXPECT_EQ(19u, moved.playbackFactsRevision);
    EXPECT_TRUE(harken::SameCompletionOwner(
        leaf.predicate.owner, moved.owner));
    EXPECT_FALSE(harken::ReanchorKnownCompletionBeforePublication(
        leaf.predicate, UINT64_MAX - 511u, &moved));
    EXPECT_EQ(1536u, moved.absoluteLogicalCompleteFrame);
}

TEST(CompletionOwner, PrivateCompositeReanchorRejectsChangedOwnerWithoutPartialMutation) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken first{};
    harken::OccurrenceToken second{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &first));
    ASSERT_TRUE(occurrences.Acquire({}, &second));
    BindProgress(occurrences, first, 11, 19);
    BindProgress(occurrences, second, 13, 23);
    const std::array<harken::KnownScheduleLeafV1, 2> leaves = {
        MakeLeaf(first, 11, 19, 1, 256, 768),
        MakeLeaf(second, 13, 23, 2, 384, 640)};
    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::CompositeCompletionOwnerToken token{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        pool.Acquire(composed, 29, leaves.data(), 2, occurrences, &token));
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        for (const auto leaf : {first, second}) {
            harken::CompletionProgressBlockUpdate silent{};
            silent.occurrence = leaf;
            silent.renderGeneration = leaf.slot == first.slot ? 11 : 13;
            silent.playbackFactsRevision = leaf.slot == first.slot ? 19 : 23;
            silent.completedThroughBlockSequence = sequence;
            ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
                harken::PublishCompletionProgressBlock(
                    *occurrences.CompletionProgressOf(leaf), silent));
        }
    }
    std::array<harken::KnownScheduleLeafV1, 2> candidates{};
    for (std::uint32_t index = 0; index < candidates.size(); ++index) {
        ASSERT_TRUE(pool.CaptureLeaf(token, index, &candidates[index]));
        harken::KnownCompletionPredicateV1 moved{};
        ASSERT_TRUE(harken::ReanchorKnownCompletionBeforePublication(
            candidates[index].predicate,
            candidates[index].absoluteLeafStartFrame + 1024,
            &moved));
        candidates[index].predicate = moved;
        candidates[index].absoluteLeafStartFrame =
            moved.absoluteLogicalStartFrame;
    }
    harken::CompositeCompletionOwnerPool::PrivateReanchorProof proof{};
    auto wrong = candidates;
    wrong[1].predicate.owner.factsRevision ^= 1;
    EXPECT_FALSE(pool.ProvePrivateReanchorBeforePublication(
        token, wrong.data(), 2, occurrences, &proof));
    harken::CompositeCompletionOwnerInfo info{};
    ASSERT_TRUE(pool.Capture(token, &info));
    EXPECT_EQ(768u, info.absoluteLogicalCompleteFrame);
    ASSERT_TRUE(pool.ProvePrivateReanchorBeforePublication(
        token, candidates.data(), 2, occurrences, &proof));
    pool.ApplyPrivateReanchorBeforePublication(proof);
    ASSERT_TRUE(pool.Capture(token, &info));
    EXPECT_EQ(1792u, info.absoluteLogicalCompleteFrame);
    harken::KnownScheduleLeafV1 actual{};
    ASSERT_TRUE(pool.CaptureLeaf(token, 1, &actual));
    EXPECT_EQ(1408u, actual.absoluteLeafStartFrame);
    EXPECT_EQ(1664u, actual.predicate.absoluteLogicalCompleteFrame);
    EXPECT_EQ(23u, actual.predicate.playbackFactsRevision);
    harken::CompletionProgressBlockUpdate audible{};
    audible.occurrence = first;
    audible.renderGeneration = 11;
    audible.playbackFactsRevision = 19;
    audible.completedThroughBlockSequence = 4;
    audible.sourceOutputFramesThisBlock = 1;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
        harken::PublishCompletionProgressBlock(
            *occurrences.CompletionProgressOf(first), audible));
    harken::CompositeCompletionOwnerPool::PrivateReanchorProof afterAudio{};
    EXPECT_FALSE(pool.ProvePrivateReanchorBeforePublication(
        token, candidates.data(), 2, occurrences, &afterAudio));
}

TEST(CompletionOwner, CompositeCapacityPlusOneHasZeroPartialMutation) {
    harken::OccurrenceStatePool occurrences(4);
    harken::OccurrenceToken composedA{};
    harken::OccurrenceToken composedB{};
    harken::OccurrenceToken leafA{};
    harken::OccurrenceToken leafB{};
    ASSERT_TRUE(occurrences.Acquire({}, &composedA));
    ASSERT_TRUE(occurrences.Acquire({}, &composedB));
    ASSERT_TRUE(occurrences.Acquire({}, &leafA));
    ASSERT_TRUE(occurrences.Acquire({}, &leafB));
    BindProgress(occurrences, leafA, 3, 5);
    BindProgress(occurrences, leafB, 7, 11);
    const harken::KnownScheduleLeafV1 first =
        MakeLeaf(leafA, 3, 5, 1, 0, 20);
    const harken::KnownScheduleLeafV1 second =
        MakeLeaf(leafB, 7, 11, 1, 0, 30);
    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::CompositeCompletionOwnerToken firstToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.Acquire(composedA, 13, &first, 1, occurrences,
                           &firstToken));

    const harken::CompositeCompletionOwnerToken sentinel{99, 98};
    harken::CompositeCompletionOwnerToken refused = sentinel;
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Capacity,
              pool.Acquire(composedB, 17, &second, 1, occurrences,
                           &refused));
    EXPECT_EQ(sentinel.slot, refused.slot);
    EXPECT_EQ(sentinel.generation, refused.generation);
    EXPECT_EQ(1u, pool.OccupiedCount());
    EXPECT_EQ(1u, pool.HighWater());
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(leafB, &owner));
}

TEST(CompletionOwner,
     PreparedCompositePublishesOnlyAtCommitAndCancelKeepsHighWaterZero) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken leafToken{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &leafToken));
    BindProgress(occurrences, leafToken, 3, 5);
    const harken::KnownScheduleLeafV1 leaf =
        MakeLeaf(leafToken, 3, 5, 1, 0, 20);
    harken::CompositeCompletionOwnerPool pool(1, 1);

    harken::CompositeCompletionOwnerToken prepared{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.PrepareAcquire(composed, 13, &leaf, 1, occurrences,
                                  &prepared));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.HighWater());
    harken::CompositeCompletionOwnerInfo info{};
    EXPECT_FALSE(pool.Capture(prepared, &info));
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(leafToken, &owner));
    harken::KnownScheduleLeafV1 bound{};
    ASSERT_TRUE(pool.CapturePreparedLeaf(prepared, 0, &bound));
    EXPECT_EQ(harken::CompletionOwnerKind::Composite,
              bound.predicate.owner.kind);
    ASSERT_TRUE(pool.CancelPrepared(prepared));
    EXPECT_EQ(1u, pool.FreeCount());
    EXPECT_EQ(0u, pool.HighWater());

    harken::CompositeCompletionOwnerToken committed{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.PrepareAcquire(composed, 17, &leaf, 1, occurrences,
                                  &committed));
    EXPECT_EQ(prepared.generation + 1u, committed.generation);
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.CommitPrepared(committed, occurrences));
    EXPECT_EQ(1u, pool.OccupiedCount());
    EXPECT_EQ(1u, pool.HighWater());
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(leafToken, &owner));
    EXPECT_EQ(committed.slot, owner.composite.slot);
    EXPECT_EQ(committed.generation, owner.composite.generation);
}

TEST(CompletionOwner, CompositeRejectsOwnedOrMismatchedLeafAndRollsBackReverse) {
    harken::OccurrenceStatePool occurrences(4);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken first{};
    harken::OccurrenceToken second{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &first));
    ASSERT_TRUE(occurrences.Acquire({}, &second));
    BindProgress(occurrences, first, 3, 5);
    BindProgress(occurrences, second, 7, 11);
    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::CompositeCompletionOwnerToken out{99, 98};

    harken::KnownScheduleLeafV1 mismatch =
        MakeLeaf(first, 3, 5, 1, 0, 20);
    ++mismatch.predicate.renderGeneration;
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid,
              pool.Acquire(composed, 13, &mismatch, 1, occurrences, &out));
    EXPECT_EQ(99u, out.slot);

    const harken::CompletionOwnerRef prior =
        harken::CompletionOwnerRef::Single({17, 19}, 11);
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(second, prior));
    harken::KnownScheduleLeafV1 leaves[2]{
        MakeLeaf(first, 3, 5, 1, 0, 20),
        MakeLeaf(second, 7, 11, 2, 0, 30)};
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::LeafOwned,
              pool.Acquire(composed, 13, leaves, 2, occurrences, &out));
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(first, &owner));
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(second, &owner));
    EXPECT_TRUE(harken::SameCompletionOwner(prior, owner));
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(second, prior));

    leaves[1] = MakeLeaf(first, 3, 5, 2, 0, 20);
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid,
              pool.Acquire(composed, 13, leaves, 2, occurrences, &out));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(first, &owner));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(99u, out.slot);
}

TEST(CompletionOwner, CompositeReclaimsOnlyAfterEveryLatestProgressFence) {
    harken::OccurrenceStatePool occurrences(4);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken leafToken{};
    harken::OccurrenceToken nextLeafToken{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &leafToken));
    ASSERT_TRUE(occurrences.Acquire({}, &nextLeafToken));
    BindProgress(occurrences, leafToken, 3, 5);
    BindProgress(occurrences, nextLeafToken, 7, 11);
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = leafToken;
    update.renderGeneration = 3;
    update.playbackFactsRevision = 5;
    update.completedThroughBlockSequence = 4;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(
                  *occurrences.CompletionProgressOf(leafToken), update));

    harken::CompositeCompletionOwnerPool pool(1, 1);
    const harken::KnownScheduleLeafV1 leaf =
        MakeLeaf(leafToken, 3, 5, 1, 0, 20);
    harken::CompositeCompletionOwnerToken token{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.Acquire(composed, 13, &leaf, 1, occurrences, &token));
    EXPECT_FALSE(pool.Release(token, occurrences, 4));
    EXPECT_EQ(1u, pool.OccupiedCount());
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(leafToken, &owner));
    ASSERT_TRUE(pool.Release(token, occurrences, 5));
    EXPECT_EQ(0u, pool.OccupiedCount());

    const harken::KnownScheduleLeafV1 next =
        MakeLeaf(nextLeafToken, 7, 11, 1, 0, 30);
    harken::CompositeCompletionOwnerToken nextToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.Acquire(composed, 17, &next, 1, occurrences,
                           &nextToken));
    EXPECT_EQ(token.slot, nextToken.slot);
    EXPECT_EQ(token.generation + 1u, nextToken.generation);
}

TEST(CompletionOwner, CompositeMaximumGenerationQuarantinesInsteadOfWrapping) {
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken leafToken{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &leafToken));
    BindProgress(occurrences, leafToken, 3, 5);
    harken::CompositeCompletionOwnerPool pool(1, 1);
    harken::detail::CompositeCompletionOwnerPoolTestAccess::SeedGeneration(
        pool, 0, UINT32_MAX);
    const harken::KnownScheduleLeafV1 leaf =
        MakeLeaf(leafToken, 3, 5, 1, 0, 20);
    harken::CompositeCompletionOwnerToken token{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              pool.Acquire(composed, 13, &leaf, 1, occurrences, &token));
    EXPECT_EQ(UINT32_MAX, token.generation);
    ASSERT_TRUE(pool.Release(token, occurrences, 1));
    EXPECT_EQ(1u, pool.QuarantinedCount());
    harken::CompositeCompletionOwnerToken refused{99, 98};
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Capacity,
              pool.Acquire(composed, 17, &leaf, 1, occurrences,
                           &refused));
    EXPECT_EQ(99u, refused.slot);
}

TEST(CompletionOwner, CompositeWidthAndZeroCapacityRefuseWithoutLeafMutation) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken first{};
    harken::OccurrenceToken second{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &first));
    ASSERT_TRUE(occurrences.Acquire({}, &second));
    BindProgress(occurrences, first, 3, 5);
    BindProgress(occurrences, second, 7, 11);
    const harken::KnownScheduleLeafV1 leaves[2]{
        MakeLeaf(first, 3, 5, 1, 0, 20),
        MakeLeaf(second, 7, 11, 2, 0, 30)};
    const harken::CompositeCompletionOwnerToken sentinel{99, 98};
    harken::CompositeCompletionOwnerToken out = sentinel;

    harken::CompositeCompletionOwnerPool widthOne(1, 1);
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid,
              widthOne.Acquire(composed, 13, leaves, 2, occurrences,
                               &out));
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(first, &owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(second, &owner));
    EXPECT_EQ(0u, widthOne.OccupiedCount());
    EXPECT_EQ(sentinel.slot, out.slot);

    harken::CompositeCompletionOwnerPool zero(0, 1);
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Capacity,
              zero.Acquire(composed, 13, leaves, 1, occurrences, &out));
    EXPECT_EQ(0u, zero.Capacity());
    EXPECT_EQ(sentinel.slot, out.slot);
}

TEST(CompletionOwner,
     CompositeRejectsDuplicateOccurrenceBeforePinningAnyBackref) {
    harken::OccurrenceStatePool occurrences(2);
    harken::OccurrenceToken composed{};
    harken::OccurrenceToken leafOccurrence{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &leafOccurrence));
    BindProgress(occurrences, leafOccurrence, 7, 11);
    harken::KnownScheduleLeafV1 leaves[2]{
        MakeLeaf(leafOccurrence, 7, 11, 1, 0, 20),
        MakeLeaf(leafOccurrence, 7, 11, 2, 0, 20),
    };
    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::CompositeCompletionOwnerToken sentinel{19, 23};
    harken::CompositeCompletionOwnerToken out = sentinel;

    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid,
              pool.Acquire(composed, 13, leaves, 2, occurrences, &out));
    EXPECT_EQ(sentinel.slot, out.slot);
    EXPECT_EQ(sentinel.generation, out.generation);
    EXPECT_EQ(0u, pool.OccupiedCount());
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(
        occurrences.CaptureCompletionOwner(leafOccurrence, &owner));
}

TEST(CompletionOwner,
     CompositeReleasePreflightMismatchIsZeroMutationButPostClearFailureQuarantines) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composedA{};
    harken::OccurrenceToken composedB{};
    harken::OccurrenceToken leafA{};
    ASSERT_TRUE(occurrences.Acquire({}, &composedA));
    ASSERT_TRUE(occurrences.Acquire({}, &composedB));
    ASSERT_TRUE(occurrences.Acquire({}, &leafA));
    BindProgress(occurrences, leafA, 7, 11);
    const harken::KnownScheduleLeafV1 leaf =
        MakeLeaf(leafA, 7, 11, 1, 0, 20);

    harken::CompositeCompletionOwnerPool preflightPool(1, 1);
    harken::CompositeCompletionOwnerToken preflightToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              preflightPool.Acquire(composedA, 13, &leaf, 1, occurrences,
                                    &preflightToken));
    const harken::CompletionOwnerRef preflightOwner =
        harken::CompletionOwnerRef::Composite(preflightToken, 11);
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(
        leafA, preflightOwner));
    EXPECT_FALSE(preflightPool.Release(preflightToken, occurrences, 100));
    EXPECT_EQ(1u, preflightPool.OccupiedCount());
    EXPECT_EQ(0u, preflightPool.QuarantinedCount());
    harken::CompositeCompletionOwnerInfo info{};
    EXPECT_TRUE(preflightPool.Capture(preflightToken, &info));

    ASSERT_TRUE(occurrences.TryPinCompletionOwner(leafA, preflightOwner));
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(
        leafA, preflightOwner));
    harken::CompositeCompletionOwnerPool terminalPool(1, 1);
    harken::CompositeCompletionOwnerToken terminalToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
              terminalPool.Acquire(composedB, 17, &leaf, 1, occurrences,
                                   &terminalToken));
    struct HookContext {
        harken::OccurrenceStatePool* pool;
        harken::OccurrenceToken occurrence;
    } context{&occurrences, leafA};
    const auto invalidateFacts = [](void* raw) {
        auto* context = static_cast<HookContext*>(raw);
        harken::CompletionProgressBlockUpdate update{};
        update.occurrence = context->occurrence;
        update.renderGeneration = 7;
        update.playbackFactsRevision = 99;
        update.completedThroughBlockSequence = 8;
        EXPECT_EQ(harken::CompletionProgressPublishStatus::Ok,
                  harken::PublishCompletionProgressBlock(
                      *context->pool->CompletionProgressOf(
                          context->occurrence),
                      update));
    };
    harken::detail::CompletionOwnerTestAccess::
        SetAfterCompletionOwnerClearHook(occurrences, invalidateFacts,
                                         &context);
    EXPECT_FALSE(terminalPool.Release(terminalToken, occurrences, 100));
    harken::detail::CompletionOwnerTestAccess::
        SetAfterCompletionOwnerClearHook(occurrences, nullptr, nullptr);
    EXPECT_EQ(0u, terminalPool.OccupiedCount());
    EXPECT_EQ(1u, terminalPool.QuarantinedCount());
    EXPECT_FALSE(terminalPool.Capture(terminalToken, &info));
    EXPECT_FALSE(terminalPool.Release(terminalToken, occurrences, 100));
    EXPECT_EQ(1u, terminalPool.QuarantinedCount());
}

TEST(CompletionOwner, PreparedCompositeObservationKeepsActualReservedOwnerPrivate) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composed{}, early{}, late{};
    ASSERT_TRUE(occurrences.Acquire({}, &composed));
    ASSERT_TRUE(occurrences.Acquire({}, &early));
    ASSERT_TRUE(occurrences.Acquire({}, &late));
    ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, early, 31, 41));
    ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, late, 37, 43));
    const std::array leaves{MakeLeaf(late, 37, 43, 9, 150, 430),
                            MakeLeaf(early, 31, 41, 3, 100, 260)};
    harken::CompositeCompletionOwnerPool pool(1, 2);
    harken::CompositeCompletionOwnerToken original{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        pool.PrepareAcquire(composed, 47, leaves.data(), leaves.size(), occurrences, &original));
    harken::CompositeCompletionOwnerInfo info{};
    ASSERT_TRUE(pool.CapturePrepared(original, &info));
    EXPECT_EQ(composed.slot, info.composedOccurrence.slot);
    EXPECT_EQ(composed.generation, info.composedOccurrence.generation);
    EXPECT_EQ(47u, info.aggregateRevision);
    EXPECT_EQ(2u, info.leafCount);
    EXPECT_EQ(430u, info.absoluteLogicalCompleteFrame);
    // Ordinary audio-visible Capture must not expose the Reserved row.
    const auto before = info;
    EXPECT_FALSE(pool.Capture(original, &info));
    EXPECT_EQ(before.composedOccurrence.slot, info.composedOccurrence.slot);
    EXPECT_EQ(before.composedOccurrence.generation, info.composedOccurrence.generation);
    EXPECT_EQ(before.aggregateRevision, info.aggregateRevision);
    EXPECT_EQ(before.leafCount, info.leafCount);
    EXPECT_EQ(before.absoluteLogicalCompleteFrame, info.absoluteLogicalCompleteFrame);
    EXPECT_FALSE(pool.CapturePrepared(original, nullptr));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.HighWater());
    harken::CompletionOwnerRef owner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(late, &owner));
    harken::KnownScheduleLeafV1 bound{};
    ASSERT_TRUE(pool.CapturePreparedLeaf(original, 0, &bound));
    EXPECT_EQ(3u, bound.stableLeafOrdinal);
    EXPECT_TRUE(harken::SameCompletionOwner(bound.predicate.owner,
        harken::CompletionOwnerRef::Composite(original, 41)));
    ASSERT_TRUE(pool.CapturePreparedLeaf(original, 1, &bound));
    EXPECT_EQ(9u, bound.stableLeafOrdinal);
    EXPECT_TRUE(harken::SameCompletionOwner(bound.predicate.owner,
        harken::CompletionOwnerRef::Composite(original, 43)));
    ASSERT_TRUE(pool.CancelPrepared(original));
    EXPECT_EQ(1u, pool.FreeCount());
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.HighWater());
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(early, &owner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(late, &owner));
}

TEST(CompletionOwner, PreparedCompositeObservationRejectsPinnedAndActualOldReusedGenerationWithoutOutputChange) {
    harken::OccurrenceStatePool occurrences(3);
    harken::OccurrenceToken composedA{}, composedB{}, occurrence{};
    ASSERT_TRUE(occurrences.Acquire({}, &composedA));
    ASSERT_TRUE(occurrences.Acquire({}, &composedB));
    ASSERT_TRUE(occurrences.Acquire({}, &occurrence));
    ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, occurrence, 3, 5));
    const auto leaf = MakeLeaf(occurrence, 3, 5, 1, 10, 30);
    harken::CompositeCompletionOwnerPool pool(1, 1);
    harken::CompositeCompletionOwnerToken old{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        pool.PrepareAcquire(composedA, 13, &leaf, 1, occurrences, &old));
    harken::CompositeCompletionOwnerInfo observation{};
    ASSERT_TRUE(pool.CapturePrepared(old, &observation));
    ASSERT_TRUE(pool.CancelPrepared(old));
    const auto unchanged = observation;
    const auto expectUnchanged = [&] {
        EXPECT_EQ(unchanged.composedOccurrence.slot, observation.composedOccurrence.slot);
        EXPECT_EQ(unchanged.composedOccurrence.generation, observation.composedOccurrence.generation);
        EXPECT_EQ(unchanged.aggregateRevision, observation.aggregateRevision);
        EXPECT_EQ(unchanged.leafCount, observation.leafCount);
        EXPECT_EQ(unchanged.absoluteLogicalCompleteFrame, observation.absoluteLogicalCompleteFrame);
    };
    // The old identity is an actual cancelled factory output, not a forged token.
    EXPECT_FALSE(pool.CapturePrepared(old, &observation));
    expectUnchanged();
    harken::CompositeCompletionOwnerToken current{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        pool.PrepareAcquire(composedB, 17, &leaf, 1, occurrences, &current));
    EXPECT_EQ(old.slot, current.slot);
    EXPECT_NE(old.generation, current.generation);
    EXPECT_FALSE(pool.CapturePrepared(old, &observation));
    expectUnchanged();
    harken::CompositeCompletionOwnerInfo actual{};
    ASSERT_TRUE(pool.CapturePrepared(current, &actual));
    EXPECT_EQ(composedB.slot, actual.composedOccurrence.slot);
    EXPECT_EQ(composedB.generation, actual.composedOccurrence.generation);
    EXPECT_EQ(17u, actual.aggregateRevision);
    EXPECT_EQ(1u, actual.leafCount);
    EXPECT_EQ(30u, actual.absoluteLogicalCompleteFrame);
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        pool.CommitPrepared(current, occurrences));
    EXPECT_FALSE(pool.CapturePrepared(current, &observation));
    expectUnchanged();
    EXPECT_FALSE(pool.CapturePrepared(old, &observation));
    expectUnchanged();
    ASSERT_TRUE(pool.Capture(current, &actual));
    EXPECT_EQ(composedB.slot, actual.composedOccurrence.slot);
    EXPECT_EQ(composedB.generation, actual.composedOccurrence.generation);
    EXPECT_EQ(17u, actual.aggregateRevision);
    EXPECT_EQ(1u, actual.leafCount);
    EXPECT_EQ(30u, actual.absoluteLogicalCompleteFrame);
    harken::CompletionOwnerRef owner{};
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(occurrence, &owner));
    EXPECT_TRUE(harken::SameCompletionOwner(owner,
        harken::CompletionOwnerRef::Composite(current, 5)));
    // The true unpublished owning rollback returns the inline leaf pin.
    ASSERT_TRUE(pool.RollbackBeforePublication(current, occurrences));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(occurrence, &owner));
    EXPECT_EQ(1u, pool.FreeCount());
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_FALSE(pool.CapturePrepared(current, &observation));
    expectUnchanged();
}


// These component tests do not grant Decision lifetime or physical Current rights.
using OriginalSinglePin = harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin;
using OriginalSingleObservation =
    harken::detail::CompletionOwnerTestAccess::OriginalSingleObservation;
static_assert(!std::is_copy_constructible_v<OriginalSinglePin>);
static_assert(!std::is_copy_assignable_v<OriginalSinglePin>);
static_assert(std::is_move_constructible_v<OriginalSinglePin>);
static_assert(!std::is_move_constructible_v<harken::OccurrenceStatePool>);

struct GenuineSingleOrigin {
    harken::OccurrenceToken occurrence{};
    harken::DecisionToken decision{};
    harken::CompletionOwnerRef owner{};
    std::uint32_t renderGeneration = 0;
    std::uint64_t scheduleEpoch = 0;
    harken::CompletionProgressRow* progress = nullptr;
    OriginalSinglePin original;
};

bool ReserveSingleOriginDecision(harken::LogicCommitCoordinator& commits,
    harken::DecisionTable& decisions, harken::DecisionToken* out) {
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = commits.Issue();
    request.world = 7;
    request.transport = 9;
    request.expectedContinuationGeneration = 11;
    request.sliceCount = 1;
    return request.ticket.Valid() && decisions.Reserve(request, out);
}

bool PrepareGenuineSingleOrigin(harken::LogicCommitCoordinator& commits,
    harken::DecisionTable& decisions, harken::OccurrenceStatePool& occurrences,
    std::uint32_t renderGeneration, GenuineSingleOrigin* out) {
    if (out == nullptr || out->original.Valid() ||
        !ReserveSingleOriginDecision(commits, decisions, &out->decision) ||
        !occurrences.PrepareAcquire({}, &out->occurrence)) return false;
    out->renderGeneration = renderGeneration;
    out->scheduleEpoch = occurrences.ScheduleEpochOf(out->occurrence);
    out->owner = harken::CompletionOwnerRef::Single(out->decision, out->scheduleEpoch);
    if (!occurrences.BindCompletionProgress(out->occurrence, renderGeneration,
            out->owner.factsRevision) ||
        !occurrences.TryPinCompletionOwner(out->occurrence, out->owner, &out->original)) return false;
    out->progress = occurrences.CompletionProgressOf(out->occurrence);
    return out->progress != nullptr;
}

bool ObserveSingleOrigin(const harken::OccurrenceStatePool& occurrences,
    const GenuineSingleOrigin& origin, OriginalSingleObservation* out) {
    return harken::detail::CompletionOwnerTestAccess::ObserveOriginalSingle(
        occurrences, origin.occurrence, out);
}

void ExpectSameOriginalSingleObservation(const OriginalSingleObservation& before,
    const OriginalSingleObservation& after) {
    EXPECT_EQ(before.marker, after.marker);
    EXPECT_EQ(before.count, after.count);
    EXPECT_TRUE(harken::SameCompletionOwner(before.owner, after.owner));
    EXPECT_EQ(before.renderGeneration, after.renderGeneration);
    EXPECT_EQ(before.scheduleEpoch, after.scheduleEpoch);
    EXPECT_EQ(before.progress, after.progress);
}

TEST(CompletionOwner, OriginalSinglePreparedCommitBorrowKeepsGenuineOwnerAndProgress) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(1, 1);
    harken::OccurrenceStatePool occurrences(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &origin));
    ASSERT_TRUE(origin.original.Valid());
    ASSERT_TRUE(occurrences.IsOriginalSingleCompletionOwnerPinFor(
        origin.original, origin.occurrence));
    EXPECT_FALSE(occurrences.IsOriginalSingleCompletionOwnerCurrent(origin.original,
        origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    OriginalSinglePin loan;
    ASSERT_TRUE(origin.original.Borrow(&loan));
    OriginalSingleObservation before{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &before));
    EXPECT_NE(0u, before.marker);
    EXPECT_EQ(2u, before.count);
    EXPECT_TRUE(harken::SameCompletionOwner(origin.owner, before.owner));
    EXPECT_EQ(origin.progress, before.progress);
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(origin.occurrence));
    EXPECT_TRUE(occurrences.IsOriginalSingleCompletionOwnerCurrent(loan,
        origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    OriginalSingleObservation after{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    ExpectSameOriginalSingleObservation(before, after);
    loan.Release();
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    EXPECT_EQ(1u, after.count);
    EXPECT_EQ(before.marker, after.marker);
}

TEST(CompletionOwner, OriginalSingleReceiverAndFullScopeRefuseSameNumericForeignOrigin) {
    harken::LogicCommitCoordinator firstCommits, secondCommits;
    harken::DecisionTable firstDecisions(2, 1), secondDecisions(1, 1);
    harken::OccurrenceStatePool firstPool(1), secondPool(1);
    GenuineSingleOrigin first, second;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(firstCommits, firstDecisions, firstPool, 17, &first));
    ASSERT_TRUE(PrepareGenuineSingleOrigin(secondCommits, secondDecisions, secondPool, 17, &second));
    ASSERT_EQ(first.occurrence.slot, second.occurrence.slot);
    ASSERT_EQ(first.occurrence.generation, second.occurrence.generation);
    ASSERT_EQ(first.decision.slot, second.decision.slot);
    ASSERT_EQ(first.decision.generation, second.decision.generation);
    ASSERT_TRUE(firstPool.CommitPreparedAcquire(first.occurrence));
    ASSERT_TRUE(secondPool.CommitPreparedAcquire(second.occurrence));
    OriginalSingleObservation firstBefore{}, secondBefore{};
    ASSERT_TRUE(ObserveSingleOrigin(firstPool, first, &firstBefore));
    ASSERT_TRUE(ObserveSingleOrigin(secondPool, second, &secondBefore));
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerPinFor(second.original, first.occurrence));
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerCurrent(second.original,
        first.owner, first.progress, first.renderGeneration, first.scheduleEpoch));
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerCurrent(first.original,
        first.owner, second.progress, first.renderGeneration, first.scheduleEpoch));
    harken::DecisionToken otherDecision{};
    ASSERT_TRUE(ReserveSingleOriginDecision(firstCommits, firstDecisions, &otherDecision));
    const auto otherOwner = harken::CompletionOwnerRef::Single(otherDecision, first.owner.factsRevision);
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerCurrent(first.original,
        otherOwner, first.progress, first.renderGeneration, first.scheduleEpoch));
    auto wrongFacts = first.owner;
    ++wrongFacts.factsRevision;
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerCurrent(first.original,
        wrongFacts, first.progress, first.renderGeneration, first.scheduleEpoch));
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerCurrent(first.original,
        first.owner, first.progress, first.renderGeneration + 1u, first.scheduleEpoch));
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerCurrent(first.original,
        first.owner, first.progress, first.renderGeneration, first.scheduleEpoch + 1u));
    auto wrongToken = first.occurrence;
    ++wrongToken.generation;
    EXPECT_FALSE(firstPool.IsOriginalSingleCompletionOwnerPinFor(first.original, wrongToken));
    EXPECT_FALSE(firstPool.ReleaseAfterFence(first.occurrence, 1, &second.original));
    OriginalSingleObservation firstAfter{}, secondAfter{};
    ASSERT_TRUE(ObserveSingleOrigin(firstPool, first, &firstAfter));
    ASSERT_TRUE(ObserveSingleOrigin(secondPool, second, &secondAfter));
    ExpectSameOriginalSingleObservation(firstBefore, firstAfter);
    ExpectSameOriginalSingleObservation(secondBefore, secondAfter);
}

TEST(CompletionOwner, OriginalSingleIssuerPreflightKeepsActualBackrefAndOccupiedOutput) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(2, 1);
    harken::OccurrenceStatePool occurrences(2);
    GenuineSingleOrigin existing;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &existing));
    harken::DecisionToken decision{};
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(ReserveSingleOriginDecision(commits, decisions, &decision));
    ASSERT_TRUE(occurrences.PrepareAcquire({}, &occurrence));
    const auto facts = occurrences.ScheduleEpochOf(occurrence);
    const auto owner = harken::CompletionOwnerRef::Single(decision, facts);
    ASSERT_TRUE(occurrences.BindCompletionProgress(occurrence, 23, facts));
    OriginalSingleObservation before{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, existing, &before));
    EXPECT_FALSE(occurrences.TryPinCompletionOwner(occurrence, owner, &existing.original));
    harken::CompletionOwnerRef actualOwner{};
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(occurrence, &actualOwner));
    OriginalSinglePin output;
    auto wrongFacts = owner;
    ++wrongFacts.factsRevision;
    EXPECT_FALSE(occurrences.TryPinCompletionOwner(occurrence, wrongFacts, &output));
    EXPECT_FALSE(output.Valid());
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(occurrence, &actualOwner));
    EXPECT_FALSE(occurrences.TryPinCompletionOwner(occurrence, {}, &output));
    EXPECT_FALSE(output.Valid());
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(occurrence, &actualOwner));
    OriginalSingleObservation after{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, existing, &after));
    ExpectSameOriginalSingleObservation(before, after);
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(occurrence, owner, &output));
    EXPECT_FALSE(existing.original.Borrow(&output));
    EXPECT_FALSE(output.Borrow(&existing.original));
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(occurrence, &actualOwner));
    EXPECT_TRUE(harken::SameCompletionOwner(owner, actualOwner));
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, existing, &after));
    ExpectSameOriginalSingleObservation(before, after);
}

TEST(CompletionOwner, OriginalSingleMovesAndDestructionReturnOnlyTheirActualAssociation) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(2, 1);
    harken::OccurrenceStatePool occurrences(2);
    GenuineSingleOrigin first, second;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &first));
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 23, &second));
    OriginalSinglePin firstLoan, secondLoan;
    ASSERT_TRUE(first.original.Borrow(&firstLoan));
    ASSERT_TRUE(second.original.Borrow(&secondLoan));
    OriginalSingleObservation firstState{}, secondState{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, first, &firstState));
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, second, &secondState));
    ASSERT_EQ(2u, firstState.count);
    ASSERT_EQ(2u, secondState.count);
    EXPECT_NE(firstState.marker, secondState.marker);
    OriginalSinglePin moved(std::move(firstLoan));
    EXPECT_FALSE(firstLoan.Valid());
    EXPECT_TRUE(occurrences.IsOriginalSingleCompletionOwnerPinFor(moved, first.occurrence));
    moved = std::move(secondLoan);
    EXPECT_FALSE(secondLoan.Valid());
    EXPECT_FALSE(occurrences.IsOriginalSingleCompletionOwnerPinFor(moved, first.occurrence));
    EXPECT_TRUE(occurrences.IsOriginalSingleCompletionOwnerPinFor(moved, second.occurrence));
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, first, &firstState));
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, second, &secondState));
    EXPECT_EQ(1u, firstState.count);
    EXPECT_EQ(2u, secondState.count);
    {
        OriginalSinglePin temporary;
        ASSERT_TRUE(moved.Borrow(&temporary));
        ASSERT_TRUE(ObserveSingleOrigin(occurrences, second, &secondState));
        EXPECT_EQ(3u, secondState.count);
    }
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, second, &secondState));
    EXPECT_EQ(2u, secondState.count);
    moved.Release();
    EXPECT_FALSE(moved.Valid());
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, second, &secondState));
    EXPECT_EQ(1u, secondState.count);
}

TEST(CompletionOwner, OriginalSingleForeignLoanAllowsProgressAndTerminalButKeepsOldBody) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(1, 1);
    harken::OccurrenceStatePool occurrences(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &origin));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(origin.occurrence));
    OriginalSinglePin foreign;
    ASSERT_TRUE(origin.original.Borrow(&foreign));
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = origin.occurrence;
    update.renderGeneration = origin.renderGeneration;
    update.playbackFactsRevision = origin.owner.factsRevision;
    update.sourceOutputFramesThisBlock = 32;
    update.completedThroughBlockSequence = 4;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
        harken::PublishCompletionProgressBlock(*origin.progress, update));
    harken::CompletionProgressSnapshot beforeProgress{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*origin.progress,
        origin.occurrence, origin.renderGeneration, &beforeProgress));
    EXPECT_EQ(32u, beforeProgress.logicalOutputFramesProduced);
    EXPECT_EQ(4u, beforeProgress.completedThroughBlockSequence);
    OriginalSingleObservation before{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &before));
    ASSERT_TRUE(occurrences.ClearCompletionOwnerExact(origin.occurrence, origin.owner));
    ASSERT_TRUE(occurrences.UnbindCompletionProgress(origin.occurrence, origin.renderGeneration));
    EXPECT_EQ(nullptr, occurrences.CompletionProgressOf(origin.occurrence));
    ASSERT_TRUE(occurrences.Advance(origin.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    EXPECT_FALSE(occurrences.BindCompletionProgress(origin.occurrence, 23,
        origin.owner.factsRevision + 1u, 5));
    EXPECT_FALSE(occurrences.IsOriginalSingleCompletionOwnerCurrent(foreign,
        origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_TRUE(occurrences.IsOriginalSingleCompletionOwnerPinFor(foreign, origin.occurrence));
    OriginalSingleObservation after{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    ExpectSameOriginalSingleObservation(before, after);
    harken::CompletionProgressSnapshot afterProgress{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*origin.progress,
        origin.occurrence, origin.renderGeneration, &afterProgress));
    EXPECT_EQ(beforeProgress.logicalOutputFramesProduced, afterProgress.logicalOutputFramesProduced);
    EXPECT_EQ(beforeProgress.playbackFactsRevision, afterProgress.playbackFactsRevision);
    EXPECT_EQ(beforeProgress.progressRevision, afterProgress.progressRevision);
    EXPECT_EQ(beforeProgress.completedThroughBlockSequence, afterProgress.completedThroughBlockSequence);
    EXPECT_FALSE(occurrences.ReleaseAfterFence(origin.occurrence, 5, &origin.original));
    EXPECT_TRUE(origin.original.Valid());
    EXPECT_TRUE(foreign.Valid());
    foreign.Release();
    EXPECT_FALSE(occurrences.ReleaseAfterFence(origin.occurrence, 4, &origin.original));
    EXPECT_TRUE(origin.original.Valid());
    ASSERT_TRUE(occurrences.ReleaseAfterFence(origin.occurrence, 5, &origin.original));
    EXPECT_FALSE(origin.original.Valid());
    EXPECT_EQ(0u, occurrences.OccupiedCount());
    EXPECT_FALSE(occurrences.IsOriginalSingleCompletionOwnerPinFor(foreign, origin.occurrence));
    harken::OccurrenceToken reused{};
    ASSERT_TRUE(occurrences.PrepareAcquire({}, &reused));
    EXPECT_EQ(origin.occurrence.slot, reused.slot);
    EXPECT_NE(origin.occurrence.generation, reused.generation);
    EXPECT_FALSE(occurrences.IsOriginalSingleCompletionOwnerPinFor(origin.original, reused));
    EXPECT_FALSE(occurrences.BindCompletionProgress(origin.occurrence, 23, 5));
    ASSERT_TRUE(occurrences.BindCompletionProgress(reused, 23,
        occurrences.ScheduleEpochOf(reused), 5));
    ASSERT_TRUE(occurrences.UnbindCompletionProgress(reused, 23));
    ASSERT_TRUE(occurrences.CancelPreparedAcquire(reused));
}

TEST(CompletionOwner, OriginalSingleHeldOriginRejectsDifferentOwnerWithoutStoppingRollback) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(2, 1);
    harken::OccurrenceStatePool occurrences(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &origin));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(origin.occurrence));
    harken::DecisionToken nextDecision{};
    ASSERT_TRUE(ReserveSingleOriginDecision(commits, decisions, &nextDecision));
    const auto nextOwner = harken::CompletionOwnerRef::Single(nextDecision, origin.owner.factsRevision);
    OriginalSinglePin foreign;
    ASSERT_TRUE(origin.original.Borrow(&foreign));
    OriginalSingleObservation before{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &before));
    EXPECT_FALSE(occurrences.ReplaceCompletionOwnerExact(origin.occurrence, origin.owner, nextOwner));
    harken::CompletionOwnerRef actual{};
    ASSERT_TRUE(occurrences.CaptureCompletionOwner(origin.occurrence, &actual));
    EXPECT_TRUE(harken::SameCompletionOwner(origin.owner, actual));
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(origin.occurrence, origin.owner));
    EXPECT_FALSE(occurrences.TryPinCompletionOwner(origin.occurrence, nextOwner));
    EXPECT_FALSE(occurrences.CaptureCompletionOwner(origin.occurrence, &actual));
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(origin.occurrence, origin.owner));
    EXPECT_TRUE(occurrences.IsOriginalSingleCompletionOwnerCurrent(foreign,
        origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    OriginalSingleObservation after{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    ExpectSameOriginalSingleObservation(before, after);
}

TEST(CompletionOwner, OriginalSinglePreparedRollbackConsumesOnlyTheActualReturnedBaseline) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(1, 1);
    harken::OccurrenceStatePool occurrences(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &origin));
    OriginalSinglePin foreign;
    ASSERT_TRUE(origin.original.Borrow(&foreign));
    OriginalSingleObservation before{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &before));
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(origin.occurrence, origin.owner));
    ASSERT_TRUE(occurrences.UnbindCompletionProgress(origin.occurrence, origin.renderGeneration));
    EXPECT_FALSE(occurrences.CancelPreparedAcquire(origin.occurrence));
    EXPECT_FALSE(occurrences.CancelPreparedAcquire(origin.occurrence, &origin.original));
    EXPECT_EQ(harken::OccurrenceStage::Preparing, occurrences.StageOf(origin.occurrence));
    EXPECT_EQ(1u, occurrences.OccupiedCount());
    EXPECT_TRUE(origin.original.Valid());
    EXPECT_TRUE(foreign.Valid());
    OriginalSingleObservation after{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    ExpectSameOriginalSingleObservation(before, after);
    foreign.Release();
    EXPECT_FALSE(occurrences.CancelPreparedAcquire(origin.occurrence));
    ASSERT_TRUE(occurrences.CancelPreparedAcquire(origin.occurrence, &origin.original));
    EXPECT_FALSE(origin.original.Valid());
    EXPECT_EQ(0u, occurrences.OccupiedCount());
    EXPECT_EQ(harken::OccurrenceStage::Free, occurrences.StageOf(origin.occurrence));
    harken::OccurrenceToken reused{};
    ASSERT_TRUE(occurrences.PrepareAcquire({}, &reused));
    EXPECT_EQ(origin.occurrence.slot, reused.slot);
    // Prepared cancellation preserves generation; the old private origin is gone.
    EXPECT_EQ(origin.occurrence.generation, reused.generation);
    EXPECT_FALSE(occurrences.IsOriginalSingleCompletionOwnerPinFor(origin.original, reused));
    ASSERT_TRUE(occurrences.CancelPreparedAcquire(reused));
}

} // namespace


// Genuine short native preparation exclusions; no audio or Current grant.
using PreparedSingleOccurrenceProof =
    harken::OccurrenceStatePool::PreparedSingleCompletionCommitProof;
static_assert(!std::is_copy_constructible_v<PreparedSingleOccurrenceProof>);
static_assert(std::is_nothrow_move_constructible_v<PreparedSingleOccurrenceProof>);

TEST(CompletionOwner, PreparedSingleNativeProofRejectsForeignAndBlocksOnlyItsHeldPreparation) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(2, 1);
    harken::OccurrenceStatePool occurrences(1), foreign(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 23, &origin));
    PreparedSingleOccurrenceProof proof, other;
    EXPECT_FALSE(foreign.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, 23, origin.scheduleEpoch, &other));
    auto wrong = origin.owner;
    ++wrong.factsRevision;
    EXPECT_FALSE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, wrong,
        origin.progress, 23, origin.scheduleEpoch, &other));
    EXPECT_FALSE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, 24, origin.scheduleEpoch, &other));
    EXPECT_FALSE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, 23, origin.scheduleEpoch + 1, &other));
    ASSERT_TRUE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, 23, origin.scheduleEpoch, &proof));
    EXPECT_FALSE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, 23, origin.scheduleEpoch, &other));
    EXPECT_TRUE(occurrences.MatchesPreparedSingleCompletionCommit(proof, origin.occurrence,
        origin.owner, origin.progress, 23, origin.scheduleEpoch, 0));
    EXPECT_FALSE(foreign.MatchesPreparedSingleCompletionCommit(proof, origin.occurrence,
        origin.owner, origin.progress, 23, origin.scheduleEpoch, 0));
    EXPECT_FALSE(occurrences.CommitPreparedAcquire(origin.occurrence));
    EXPECT_FALSE(occurrences.ClearCompletionOwnerExact(origin.occurrence, origin.owner));
    EXPECT_FALSE(occurrences.RollbackCompletionOwnerPinExact(origin.occurrence, origin.owner));
    EXPECT_FALSE(occurrences.UnbindCompletionProgress(origin.occurrence, 23));
    EXPECT_FALSE(occurrences.BindCompletionProgress(origin.occurrence, 23, origin.owner.factsRevision));
    EXPECT_FALSE(occurrences.ReplaceCompletionOwnerExact(origin.occurrence, origin.owner, origin.owner));
    EXPECT_FALSE(occurrences.Advance(origin.occurrence, harken::OccurrenceStage::Active));
    EXPECT_EQ(harken::IssueStatus::Exhausted, occurrences.BumpScheduleEpochChecked(origin.occurrence).status);
    EXPECT_FALSE(occurrences.CancelPreparedAcquire(origin.occurrence, &origin.original));
    OriginalSingleObservation held{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &held));
    EXPECT_EQ(1u, held.count);
    PreparedSingleOccurrenceProof moved(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_TRUE(moved.Valid());
    moved.Release();
    EXPECT_FALSE(moved.Valid());
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(origin.occurrence, origin.owner));
    ASSERT_TRUE(occurrences.UnbindCompletionProgress(origin.occurrence, 23));
    ASSERT_TRUE(occurrences.CancelPreparedAcquire(origin.occurrence, &origin.original));
    EXPECT_FALSE(origin.original.Valid());
    EXPECT_EQ(0u, occurrences.OccupiedCount());
    ASSERT_TRUE(decisions.Cancel(origin.decision));
    ASSERT_TRUE(decisions.ReclaimCancelled(origin.decision));
}

TEST(CompletionOwner, PreparedSingleProofOwnsNativeExclusionAfterLastStructuralReturnAndCommitsNormally) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(1, 1);
    harken::OccurrenceStatePool occurrences(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 23, &origin));
    PreparedSingleOccurrenceProof proof;
    ASSERT_TRUE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, 23, origin.scheduleEpoch, &proof));
    origin.original.Release();
    EXPECT_TRUE(occurrences.MatchesPreparedSingleCompletionCommit(proof, origin.occurrence,
        origin.owner, origin.progress, 23, origin.scheduleEpoch, 0));
    EXPECT_FALSE(occurrences.CancelPreparedAcquire(origin.occurrence));
    harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(occurrences.CommitPreparedAcquire(origin.occurrence));
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(origin.occurrence, origin.owner));
    ASSERT_TRUE(occurrences.UnbindCompletionProgress(origin.occurrence, 23));
    ASSERT_TRUE(occurrences.Advance(origin.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(origin.occurrence));
    EXPECT_EQ(1u, occurrences.FreeCount());
    EXPECT_FALSE(occurrences.MatchesPreparedSingleCompletionCommit(proof, origin.occurrence,
        origin.owner, origin.progress, 23, origin.scheduleEpoch, 0));
    ASSERT_TRUE(decisions.Cancel(origin.decision));
    ASSERT_TRUE(decisions.ReclaimCancelled(origin.decision));
}

TEST(CompletionOwner, PreparedSingleProofMoveOverwriteAndDestructorReleaseOnlyTheActualShortGate) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(2, 1);
    harken::OccurrenceStatePool first(1), second(1);
    GenuineSingleOrigin a, b;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, first, 23, &a));
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, second, 29, &b));
    {
        PreparedSingleOccurrenceProof pa, pb;
        ASSERT_TRUE(first.ProvePreparedSingleCompletionCommit(a.original, a.owner,
            a.progress, 23, a.scheduleEpoch, &pa));
        ASSERT_TRUE(second.ProvePreparedSingleCompletionCommit(b.original, b.owner,
            b.progress, 29, b.scheduleEpoch, &pb));
        pa = std::move(pb);
        EXPECT_FALSE(pb.Valid());
        EXPECT_TRUE(second.MatchesPreparedSingleCompletionCommit(pa, b.occurrence,
            b.owner, b.progress, 29, b.scheduleEpoch, 0));
        ASSERT_TRUE(first.RollbackCompletionOwnerPinExact(a.occurrence, a.owner));
        ASSERT_TRUE(first.UnbindCompletionProgress(a.occurrence, 23));
        ASSERT_TRUE(first.CancelPreparedAcquire(a.occurrence, &a.original));
        EXPECT_FALSE(second.RollbackCompletionOwnerPinExact(b.occurrence, b.owner));
    }
    ASSERT_TRUE(second.RollbackCompletionOwnerPinExact(b.occurrence, b.owner));
    ASSERT_TRUE(second.UnbindCompletionProgress(b.occurrence, 29));
    ASSERT_TRUE(second.CancelPreparedAcquire(b.occurrence, &b.original));
    for (const auto token : {a.decision, b.decision}) {
        ASSERT_TRUE(decisions.Cancel(token));
        ASSERT_TRUE(decisions.ReclaimCancelled(token));
    }
}


TEST(CompletionOwner, OriginalSingleRebindReturnsOnlyWithdrawnEpisodeAndIssuesFreshMarker) {
    harken::LogicCommitCoordinator commits, otherCommits;
    harken::DecisionTable decisions(1, 1), otherDecisions(1, 1);
    harken::OccurrenceStatePool occurrences(1), otherPool(1);
    GenuineSingleOrigin origin, other;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &origin));
    ASSERT_TRUE(PrepareGenuineSingleOrigin(otherCommits, otherDecisions, otherPool, 17, &other));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(origin.occurrence));
    ASSERT_TRUE(otherPool.CommitPreparedAcquire(other.occurrence));
    OriginalSinglePin foreign;
    ASSERT_TRUE(origin.original.Borrow(&foreign));
    OriginalSingleObservation before{}, after{};
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &before));
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_FALSE(occurrences.ReturnWithdrawnOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    foreign.Release();
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &before));
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(other.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    auto stale = origin.occurrence;
    ++stale.generation;
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        stale, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, other.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration + 1u, origin.scheduleEpoch));
    auto wrongOwner = origin.owner;
    ++wrongOwner.factsRevision;
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, wrongOwner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    ExpectSameOriginalSingleObservation(before, after);
    ASSERT_TRUE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_FALSE(occurrences.ReturnWithdrawnOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    ASSERT_TRUE(occurrences.RollbackCompletionOwnerPinExact(origin.occurrence, origin.owner));
    EXPECT_FALSE(occurrences.ReturnWithdrawnOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    ASSERT_TRUE(occurrences.UnbindCompletionProgress(origin.occurrence, origin.renderGeneration));
    ASSERT_TRUE(occurrences.ReturnWithdrawnOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_FALSE(origin.original.Valid());
    EXPECT_FALSE(occurrences.ReturnWithdrawnOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_EQ(origin.scheduleEpoch, occurrences.ScheduleEpochOf(origin.occurrence));
    EXPECT_EQ(harken::OccurrenceStage::Preparing, occurrences.StageOf(origin.occurrence));
    EXPECT_EQ(1u, occurrences.OccupiedCount());
    ASSERT_TRUE(occurrences.BindCompletionProgress(origin.occurrence, 18, origin.owner.factsRevision));
    ASSERT_TRUE(occurrences.TryPinCompletionOwner(origin.occurrence, origin.owner, &origin.original));
    ASSERT_TRUE(ObserveSingleOrigin(occurrences, origin, &after));
    EXPECT_NE(before.marker, after.marker);
    EXPECT_EQ(1u, after.count);
    EXPECT_EQ(18u, after.renderGeneration);
}

TEST(CompletionOwner, OriginalSingleRebindCannotReturnARealPreparedCommitHold) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable decisions(1, 1);
    harken::OccurrenceStatePool occurrences(1);
    GenuineSingleOrigin origin;
    ASSERT_TRUE(PrepareGenuineSingleOrigin(commits, decisions, occurrences, 17, &origin));
    harken::OccurrenceStatePool::PreparedSingleCompletionCommitProof held;
    ASSERT_TRUE(occurrences.ProvePreparedSingleCompletionCommit(origin.original, origin.owner,
        origin.progress, origin.renderGeneration, origin.scheduleEpoch, &held));
    EXPECT_FALSE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_FALSE(occurrences.ReturnWithdrawnOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
    EXPECT_TRUE(held.Valid());
    EXPECT_TRUE(origin.original.Valid());
    harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(held));
    EXPECT_TRUE(occurrences.CanWithdrawOriginalSingleCompletionForRebind(origin.original,
        origin.occurrence, origin.owner, origin.progress, origin.renderGeneration, origin.scheduleEpoch));
}


TEST_F(OriginalCompositeStorageTest, ActualOccurrenceAndCompositeForeignLoansRecycleOnlyAfterBothReturn) {
    for (bool storageFirst : {false, true}) {
        harken::OccurrenceStatePool native{2};
        harken::CompositeCompletionOwnerPool composedPool{1, 1};
        harken::OccurrenceToken aggregate{}, leaf{};
        harken::OccurrenceStatePool::OriginalStoragePin own, storageForeign;
        ASSERT_TRUE(native.PrepareAcquire({}, &aggregate));
        ASSERT_TRUE(native.PrepareAcquire({}, &leaf, &own));
        ASSERT_TRUE(own.Borrow(&storageForeign));
        ASSERT_TRUE(native.BindCompletionProgress(leaf, 31, 41));
        const auto known = MakeLeaf(leaf, 31, 41, 1, 100, 260);
        Pin rowOriginal;
        harken::CompositeCompletionOwnerToken actual{};
        ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
            composedPool.PrepareAcquire(aggregate, 47, &known, 1, native, &actual, &rowOriginal));
        ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, composedPool.CommitPrepared(actual, native));
        harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin compositeForeign;
        ASSERT_TRUE(composedPool.BorrowOriginalLeafCompletion(rowOriginal, 0, &compositeForeign));
        ASSERT_TRUE(composedPool.RollbackBeforePublication(actual, native));
        ASSERT_TRUE(native.UnbindCompletionProgress(leaf, 31));
        ASSERT_TRUE(native.CancelPreparedAcquire(leaf, nullptr, &own));
        ASSERT_TRUE(storageForeign.NativeEnded());
        ASSERT_TRUE(native.IsOriginalCompositeCompletionOwnerPinFor(compositeForeign, leaf));
        EXPECT_EQ(1u, native.RetainedStorageCount());
        EXPECT_EQ(0u, native.FreeCount());
        if (storageFirst) {
            storageForeign.Release();
            EXPECT_TRUE(native.IsOriginalCompositeCompletionOwnerPinFor(compositeForeign, leaf));
        } else {
            compositeForeign.Release();
            EXPECT_TRUE(storageForeign.NativeEnded());
        }
        EXPECT_EQ(1u, native.RetainedStorageCount());
        EXPECT_EQ(0u, native.FreeCount());
        storageForeign.Release();
        compositeForeign.Release();
        EXPECT_EQ(0u, native.RetainedStorageCount());
        EXPECT_EQ(1u, native.FreeCount());
        ASSERT_TRUE(native.CancelPreparedAcquire(aggregate));
        EXPECT_EQ(2u, native.FreeCount());
        rowOriginal.Release();
        EXPECT_EQ(1u, composedPool.FreeCount());
    }
}


namespace {

class OriginalPreparedCompositeTest : public testing::Test {
protected:
    using Pin = harken::CompositeCompletionOwnerPool::OriginalLifetimePin;
    using LeafPin = harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin;
    using Proof = harken::OccurrenceStatePool::PreparedCompositeCompletionCommitProof;
    void SetUp() override {
        ASSERT_TRUE(occurrences.RegisterOriginalCompositePreparedCommitResolver(this,
            [](void* context, harken::OccurrenceStatePool& receiver, const harken::OccurrenceToken& occurrence) noexcept -> Proof* {
                auto& self = *static_cast<OriginalPreparedCompositeTest*>(context);
                if (&receiver != &self.occurrences || !self.resolveCommit) return nullptr;
                if (self.overrideCommit != nullptr) return self.overrideCommit;
                return self.pool.ResolveOriginalPreparedLeafCommit(receiver, occurrence);
            }));
        ASSERT_TRUE(pool.RegisterOriginalLifetimeBaselineResolver(this,
            [](void* context, harken::CompositeCompletionOwnerPool& receiver, harken::CompositeCompletionOwnerToken row) noexcept -> Pin* {
                auto& self = *static_cast<OriginalPreparedCompositeTest*>(context);
                if (&receiver != &self.pool || row.slot != self.token.slot || row.generation != self.token.generation) return nullptr;
                ++self.baselineCalls;
                return self.foreignBaseline ? &self.foreignRow : &self.original;
            }));
        ASSERT_TRUE(occurrences.PrepareAcquire({}, &aggregate));
        ASSERT_TRUE(occurrences.PrepareAcquire({}, &first));
        ASSERT_TRUE(occurrences.PrepareAcquire({}, &second));
        ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, first, 31, 41));
        ASSERT_NO_FATAL_FAILURE(BindProgress(occurrences, second, 37, 43));
        leaves = {MakeLeaf(first, 31, 41, 3, 100, 260), MakeLeaf(second, 37, 43, 9, 150, 430)};
        ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
            pool.PrepareAcquire(aggregate, 47, leaves.data(), leaves.size(), occurrences, &token, &original));
        for (std::uint32_t i = 0; i < leaves.size(); ++i)
            ASSERT_TRUE(pool.CapturePreparedLeaf(token, i, &leaves[i]));
    }
    void Stage() {
        ASSERT_TRUE(pool.PrepareOriginalLeafCompletionCommits(original, occurrences));
    }
    void Commit() {
        ASSERT_TRUE(occurrences.CommitPreparedAcquire(aggregate));
        for (const auto& leaf : leaves) ASSERT_TRUE(occurrences.CommitPreparedAcquire(leaf.occurrence));
        ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    }
    void Cancel() {
        ASSERT_TRUE(pool.CancelPrepared(token));
        for (const auto& leaf : leaves) {
            ASSERT_TRUE(occurrences.UnbindCompletionProgress(leaf.occurrence, leaf.renderGeneration));
            ASSERT_TRUE(occurrences.CancelPreparedAcquire(leaf.occurrence));
        }
        ASSERT_TRUE(occurrences.CancelPreparedAcquire(aggregate));
        EXPECT_EQ(0u, occurrences.OccupiedCount());
        EXPECT_EQ(0u, pool.OccupiedCount());
    }
    void FinishCommittedRollback() {
        ASSERT_TRUE(pool.RollbackBeforePublication(token, occurrences));
        for (const auto& leaf : leaves) {
            ASSERT_TRUE(occurrences.UnbindCompletionProgress(leaf.occurrence, leaf.renderGeneration));
            ASSERT_TRUE(occurrences.Advance(leaf.occurrence, harken::OccurrenceStage::AwaitingEventFence));
            ASSERT_TRUE(occurrences.ReleaseAfterFence(leaf.occurrence));
        }
        ASSERT_TRUE(occurrences.Advance(aggregate, harken::OccurrenceStage::AwaitingEventFence));
        ASSERT_TRUE(occurrences.ReleaseAfterFence(aggregate));
        EXPECT_EQ(0u, occurrences.OccupiedCount());
    }
    harken::OccurrenceStatePool occurrences{3};
    harken::CompositeCompletionOwnerPool pool{1, 2};
    Pin original, foreignRow;
    harken::OccurrenceToken aggregate{}, first{}, second{};
    harken::CompositeCompletionOwnerToken token{};
    std::array<harken::KnownScheduleLeafV1, 2> leaves{};
    bool foreignBaseline = false;
    bool resolveCommit = true;
    Proof* overrideCommit = nullptr;
    std::uint32_t baselineCalls = 0;
};

static_assert(!std::is_copy_constructible_v<harken::OccurrenceStatePool::PreparedCompositeCompletionCommitProof>);
static_assert(std::is_nothrow_move_constructible_v<harken::OccurrenceStatePool::PreparedCompositeCompletionCommitProof>);
static_assert(!std::is_convertible_v<harken::OccurrenceStatePool::PreparedCompositeCompletionCommitProof,
    harken::OccurrenceStatePool::PreparedSingleCompletionCommitProof>);
static_assert(!std::is_convertible_v<harken::OccurrenceStatePool::OriginalCompositeCompletionOwnerPin,
    harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin>);

TEST_F(OriginalPreparedCompositeTest, GenuineFactoryStagesEveryLeafBeforeCommitWithoutSingleOrCurrentGrant) {
    LeafPin loan;
    EXPECT_FALSE(pool.BorrowOriginalPreparedLeafCompletion(original, 0, &loan));
    EXPECT_FALSE(pool.BorrowOriginalLeafCompletion(original, 0, &loan));
    ASSERT_TRUE(original.Borrow(&foreignRow));
    EXPECT_FALSE(pool.PrepareOriginalLeafCompletionCommits(foreignRow, occurrences));
    harken::OccurrenceStatePool equalNumericReceiver(3);
    EXPECT_FALSE(pool.PrepareOriginalLeafCompletionCommits(original, equalNumericReceiver));
    ASSERT_NO_FATAL_FAILURE(Stage());
    EXPECT_FALSE(pool.PrepareOriginalLeafCompletionCommits(original, occurrences));
    ASSERT_TRUE(pool.BorrowOriginalPreparedLeafCompletion(foreignRow, 0, &loan));
    EXPECT_FALSE(pool.BorrowOriginalPreparedLeafCompletion(original, 1, &loan));
    EXPECT_FALSE(pool.BorrowOriginalLeafCompletion(original, 0, &loan));
    EXPECT_TRUE(occurrences.IsOriginalCompositeCompletionOwnerPinFor(loan, first));
    EXPECT_FALSE(occurrences.IsOriginalCompositeCompletionOwnerCurrent(loan, leaves[0].predicate.owner,
        occurrences.CompletionProgressOf(first), 31, occurrences.ScheduleEpochOf(first)));
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, pool.CommitPrepared(token, occurrences));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, occurrences.HighWater());
    for (const auto& leaf : leaves) {
        auto* held = pool.ResolveOriginalPreparedLeafCommit(occurrences, leaf.occurrence);
        ASSERT_NE(nullptr, held);
        EXPECT_TRUE(occurrences.IsPreparedCompositeCompletionCommitFor(*held, leaf.occurrence));
        EXPECT_FALSE(equalNumericReceiver.IsPreparedCompositeCompletionCommitFor(*held, leaf.occurrence));
        EXPECT_FALSE(occurrences.ClearCompletionOwnerExact(leaf.occurrence, leaf.predicate.owner));
        EXPECT_FALSE(occurrences.RollbackCompletionOwnerPinExact(leaf.occurrence, leaf.predicate.owner));
        EXPECT_FALSE(occurrences.UnbindCompletionProgress(leaf.occurrence, leaf.renderGeneration));
        EXPECT_FALSE(occurrences.Advance(leaf.occurrence, harken::OccurrenceStage::Active));
        EXPECT_EQ(harken::IssueStatus::Exhausted, occurrences.BumpScheduleEpochChecked(leaf.occurrence).status);
    }
    ASSERT_NO_FATAL_FAILURE(Cancel());
    EXPECT_TRUE(loan.Valid());
    EXPECT_EQ(1u, occurrences.RetainedStorageCount());
    loan.Release();
    foreignRow.Release();
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, NativeOwnerOrderConsumesPreparedGatesAndKeepsStrictPhysicalFence) {
    ASSERT_NO_FATAL_FAILURE(Stage());
    LeafPin loan;
    ASSERT_TRUE(pool.BorrowOriginalPreparedLeafCompletion(original, 0, &loan));
    ASSERT_TRUE(original.Borrow(&foreignRow));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(aggregate));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(first));
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, pool.CommitPrepared(token, occurrences));
    EXPECT_EQ(nullptr, pool.ResolveOriginalPreparedLeafCommit(occurrences, first));
    EXPECT_NE(nullptr, pool.ResolveOriginalPreparedLeafCommit(occurrences, second));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(second));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    EXPECT_TRUE(occurrences.IsOriginalCompositeCompletionOwnerCurrent(loan, leaves[0].predicate.owner,
        occurrences.CompletionProgressOf(first), 31, occurrences.ScheduleEpochOf(first)));
    for (const auto& leaf : leaves) {
        harken::CompletionProgressBlockUpdate update{};
        update.occurrence = leaf.occurrence;
        update.renderGeneration = leaf.renderGeneration;
        update.playbackFactsRevision = leaf.predicate.playbackFactsRevision;
        update.completedThroughBlockSequence = 9;
        ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
            harken::PublishCompletionProgressBlock(*occurrences.CompletionProgressOf(leaf.occurrence), update));
        ASSERT_TRUE(occurrences.Advance(leaf.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    }
    EXPECT_FALSE(pool.Release(token, occurrences, 9));
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(occurrences.ReleaseAfterFence(first, 9));
    ASSERT_TRUE(pool.Release(token, occurrences, 10));
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(pool.IsOriginalLifetimePinned(foreignRow));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(first, 10));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(second, 10));
    ASSERT_TRUE(occurrences.Advance(aggregate, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(aggregate));
    EXPECT_TRUE(loan.Valid());
    EXPECT_EQ(1u, occurrences.RetainedStorageCount());
    EXPECT_FALSE(occurrences.IsOriginalCompositeCompletionOwnerCurrent(loan, leaves[0].predicate.owner,
        nullptr, 31, 1));
    loan.Release();
    foreignRow.Release();
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, ForeignRegisteredBaselineRefusesBeforeAnyLeafGateOrOwnerIsChanged) {
    ASSERT_NO_FATAL_FAILURE(Stage());
    ASSERT_TRUE(original.Borrow(&foreignRow));
    foreignBaseline = true;
    std::array<harken::detail::CompletionOwnerTestAccess::OriginalSingleObservation, 2> before{};
    std::array<Proof*, 2> gates{};
    for (std::uint32_t i = 0; i < leaves.size(); ++i) {
        ASSERT_TRUE(harken::detail::CompletionOwnerTestAccess::ObserveOriginalSingle(occurrences, leaves[i].occurrence, &before[i]));
        gates[i] = pool.ResolveOriginalPreparedLeafCommit(occurrences, leaves[i].occurrence);
        ASSERT_NE(nullptr, gates[i]);
    }
    EXPECT_FALSE(pool.CancelPrepared(token));
    EXPECT_EQ(1u, baselineCalls);
    EXPECT_EQ(2u, harken::detail::CompositeCompletionOwnerPoolTestAccess::OriginalCount(pool, token));
    harken::CompositeCompletionOwnerInfo row{};
    ASSERT_TRUE(pool.CapturePrepared(token, &row));
    for (std::uint32_t i = 0; i < leaves.size(); ++i) {
        harken::detail::CompletionOwnerTestAccess::OriginalSingleObservation after{};
        ASSERT_TRUE(harken::detail::CompletionOwnerTestAccess::ObserveOriginalSingle(occurrences, leaves[i].occurrence, &after));
        EXPECT_EQ(before[i].marker, after.marker);
        EXPECT_EQ(before[i].count, after.count);
        EXPECT_EQ(before[i].progress, after.progress);
        EXPECT_EQ(before[i].scheduleEpoch, after.scheduleEpoch);
        EXPECT_TRUE(harken::SameCompletionOwner(before[i].owner, after.owner));
        EXPECT_EQ(gates[i], pool.ResolveOriginalPreparedLeafCommit(occurrences, leaves[i].occurrence));
        EXPECT_TRUE(gates[i]->Valid());
    }
    foreignBaseline = false;
    ASSERT_NO_FATAL_FAILURE(Cancel());
    EXPECT_EQ(2u, baselineCalls);
    foreignRow.Release();
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, ActualCancelAfterOneLeafCommitReturnsBothKindsOfNativePreparation) {
    ASSERT_NO_FATAL_FAILURE(Stage());
    LeafPin foreignFirst, foreignSecond;
    ASSERT_TRUE(pool.BorrowOriginalPreparedLeafCompletion(original, 0, &foreignFirst));
    ASSERT_TRUE(pool.BorrowOriginalPreparedLeafCompletion(original, 1, &foreignSecond));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(aggregate));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(first));
    ASSERT_TRUE(pool.CancelPrepared(token));
    for (const auto& leaf : leaves) ASSERT_TRUE(occurrences.UnbindCompletionProgress(leaf.occurrence, leaf.renderGeneration));
    ASSERT_TRUE(occurrences.Advance(first, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(first));
    ASSERT_TRUE(occurrences.CancelPreparedAcquire(second));
    ASSERT_TRUE(occurrences.Advance(aggregate, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(occurrences.ReleaseAfterFence(aggregate));
    EXPECT_EQ(0u, occurrences.OccupiedCount());
    EXPECT_EQ(2u, occurrences.RetainedStorageCount());
    EXPECT_EQ(1u, pool.FreeCount());
    foreignFirst.Release();
    foreignSecond.Release();
    EXPECT_EQ(3u, occurrences.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, MissingOrEqualNumericForeignProofCannotConsumeTheGenuineHeldGate) {
    harken::OccurrenceStatePool otherOccurrences(3);
    harken::CompositeCompletionOwnerPool otherPool(1, 2);
    Pin otherOriginal;
    std::array<harken::OccurrenceToken, 3> others{};
    for (auto& occurrence : others) ASSERT_TRUE(otherOccurrences.PrepareAcquire({}, &occurrence));
    ASSERT_NO_FATAL_FAILURE(BindProgress(otherOccurrences, others[1], 31, 41));
    ASSERT_NO_FATAL_FAILURE(BindProgress(otherOccurrences, others[2], 37, 43));
    auto otherLeaves = std::array<harken::KnownScheduleLeafV1, 2>{
        MakeLeaf(others[1], 31, 41, 3, 100, 260), MakeLeaf(others[2], 37, 43, 9, 150, 430)};
    harken::CompositeCompletionOwnerToken otherToken{};
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok,
        otherPool.PrepareAcquire(others[0], 47, otherLeaves.data(), otherLeaves.size(), otherOccurrences, &otherToken, &otherOriginal));
    ASSERT_TRUE(otherPool.PrepareOriginalLeafCompletionCommits(otherOriginal, otherOccurrences));
    ASSERT_NO_FATAL_FAILURE(Stage());
    resolveCommit = false;
    EXPECT_FALSE(occurrences.CommitPreparedAcquire(first));
    resolveCommit = true;
    overrideCommit = otherPool.ResolveOriginalPreparedLeafCommit(otherOccurrences, others[1]);
    ASSERT_NE(nullptr, overrideCommit);
    EXPECT_EQ(first.slot, others[1].slot);
    EXPECT_EQ(first.generation, others[1].generation);
    EXPECT_FALSE(occurrences.CommitPreparedAcquire(first));
    EXPECT_TRUE(overrideCommit->Valid());
    EXPECT_NE(nullptr, pool.ResolveOriginalPreparedLeafCommit(occurrences, first));
    overrideCommit = nullptr;
    ASSERT_NO_FATAL_FAILURE(Commit());
    ASSERT_NO_FATAL_FAILURE(FinishCommittedRollback());
    ASSERT_TRUE(otherPool.CancelPrepared(otherToken));
    otherOriginal.Release();
    for (const auto& leaf : otherLeaves) {
        ASSERT_TRUE(otherOccurrences.UnbindCompletionProgress(leaf.occurrence, leaf.renderGeneration));
        ASSERT_TRUE(otherOccurrences.CancelPreparedAcquire(leaf.occurrence));
    }
    ASSERT_TRUE(otherOccurrences.CancelPreparedAcquire(others[0]));
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(3u, otherOccurrences.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, SecondLeafNativeMarkerExhaustionReturnsTheCompleteGenuinePrefix) {
    harken::detail::CompletionOwnerTestAccess::SeedNextOriginalMarker(occurrences, UINT64_MAX);
    EXPECT_FALSE(pool.PrepareOriginalLeafCompletionCommits(original, occurrences));
    for (const auto& leaf : leaves) {
        harken::detail::CompletionOwnerTestAccess::OriginalSingleObservation actual{};
        ASSERT_TRUE(harken::detail::CompletionOwnerTestAccess::ObserveOriginalSingle(occurrences, leaf.occurrence, &actual));
        EXPECT_EQ(0u, actual.count);
        EXPECT_EQ(0u, actual.marker);
        EXPECT_EQ(nullptr, pool.ResolveOriginalPreparedLeafCommit(occurrences, leaf.occurrence));
        harken::CompletionOwnerRef owner{};
        EXPECT_FALSE(occurrences.CaptureCompletionOwner(leaf.occurrence, &owner));
    }
    EXPECT_EQ(0u, occurrences.HighWater());
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_FALSE(pool.PrepareOriginalLeafCompletionCommits(original, occurrences));
    ASSERT_NO_FATAL_FAILURE(Cancel());
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, GenuineProofMoveKeepsItsNativeGateUntilReturnedToActualOwner) {
    ASSERT_NO_FATAL_FAILURE(Stage());
    auto* actual = pool.ResolveOriginalPreparedLeafCommit(occurrences, first);
    ASSERT_NE(nullptr, actual);
    Proof moved(std::move(*actual));
    EXPECT_FALSE(actual->Valid());
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(occurrences.CommitPreparedAcquire(first));
    EXPECT_FALSE(pool.CancelPrepared(token));
    EXPECT_NE(nullptr, pool.ResolveOriginalPreparedLeafCommit(occurrences, second));
    EXPECT_FALSE(occurrences.UnbindCompletionProgress(first, 31));
    EXPECT_FALSE(occurrences.Advance(first, harken::OccurrenceStage::Active));
    *actual = std::move(moved);
    EXPECT_FALSE(moved.Valid());
    EXPECT_EQ(actual, pool.ResolveOriginalPreparedLeafCommit(occurrences, first));
    for (const auto& leaf : leaves) ASSERT_TRUE(occurrences.CommitPreparedAcquire(leaf.occurrence));
    EXPECT_EQ(harken::CompositeCompletionAcquireStatus::Invalid, pool.CommitPrepared(token, occurrences));
    ASSERT_TRUE(occurrences.CommitPreparedAcquire(aggregate));
    ASSERT_EQ(harken::CompositeCompletionAcquireStatus::Ok, pool.CommitPrepared(token, occurrences));
    ASSERT_NO_FATAL_FAILURE(FinishCommittedRollback());
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(1u, pool.FreeCount());
}

TEST_F(OriginalPreparedCompositeTest, ForeignOccurrenceReceiverAndBaselineCannotMutateThePinnedOriginalRow) {
    ASSERT_NO_FATAL_FAILURE(Stage());
    ASSERT_NO_FATAL_FAILURE(Commit());
    harken::OccurrenceStatePool foreign(3);
    std::array<harken::OccurrenceToken, 3> equal{};
    for (auto& occurrence : equal) ASSERT_TRUE(foreign.Acquire({}, &occurrence));
    for (std::uint32_t i = 0; i < leaves.size(); ++i) {
        const auto& leaf = leaves[i];
        ASSERT_EQ(leaf.occurrence.slot, equal[i + 1u].slot);
        ASSERT_EQ(leaf.occurrence.generation, equal[i + 1u].generation);
        ASSERT_NO_FATAL_FAILURE(BindProgress(foreign, leaf.occurrence, leaf.renderGeneration, leaf.predicate.playbackFactsRevision));
        ASSERT_TRUE(foreign.TryPinCompletionOwner(leaf.occurrence, leaf.predicate.owner));
    }
    harken::CompositeCompletionOwnerPool::PrivateReanchorProof noProof;
    EXPECT_FALSE(pool.ProvePrivateReanchorBeforePublication(token, leaves.data(), leaves.size(), foreign, &noProof));
    EXPECT_FALSE(pool.RollbackBeforePublication(token, foreign));
    EXPECT_FALSE(pool.Release(token, foreign, 1));
    ASSERT_TRUE(original.Borrow(&foreignRow));
    foreignBaseline = true;
    EXPECT_FALSE(pool.RollbackBeforePublication(token, occurrences));
    EXPECT_FALSE(pool.Release(token, occurrences, 1));
    EXPECT_TRUE(pool.IsOriginalLifetimePinned(original));
    EXPECT_EQ(1u, pool.OccupiedCount());
    for (const auto& leaf : leaves) {
        harken::CompletionOwnerRef actual{}, other{};
        ASSERT_TRUE(occurrences.CaptureCompletionOwner(leaf.occurrence, &actual));
        ASSERT_TRUE(foreign.CaptureCompletionOwner(leaf.occurrence, &other));
        EXPECT_TRUE(harken::SameCompletionOwner(actual, leaf.predicate.owner));
        EXPECT_TRUE(harken::SameCompletionOwner(other, leaf.predicate.owner));
    }
    foreignBaseline = false;
    ASSERT_NO_FATAL_FAILURE(FinishCommittedRollback());
    foreignRow.Release();
    EXPECT_EQ(3u, occurrences.FreeCount());
    EXPECT_EQ(1u, pool.FreeCount());
}

} // namespace
