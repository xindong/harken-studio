// Occurrence-state pool contract tests.
//
// The mechanisms here are the two that cannot be recovered by inspection later:
// that a record waiting on the event fence still owns its slot, and that a
// transaction is validated against the occurrence and its arranging instance
// together. Either check alone admits work that should have been dropped.

#include "Common/ObjectIdTestSupport.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PlayableInstanceTable.h"

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>
#include <utility>

using harken::OccurrenceKey;
using harken::OccurrenceStage;
using harken::OccurrenceStatePool;
using harken::OccurrenceToken;
using harken::PlayableInstanceHandle;
using harken::PlayableInstanceTable;
using harken::PlayableKind;
using harken::PlayableLifecycle;

namespace harken::detail {

// Test-only access reaches the terminal-generation cases without iterating
// through the full 32-bit generation space or exposing a production setter.
struct OccurrenceStatePoolTestAccess {
    static void SeedGeneration(OccurrenceStatePool& pool,
                               std::uint32_t slot,
                               std::uint32_t generation) {
        auto& entry = pool.m_entries.at(slot);
        entry.generation = generation;
        entry.quarantined = false;
        entry.stage = OccurrenceStage::Free;
        entry.key = OccurrenceKey{};
        entry.scheduleEpoch = 0;
    }

    static void SeedScheduleEpoch(OccurrenceStatePool& pool, std::uint64_t value) {
        pool.m_nextScheduleEpoch = value;
        pool.m_scheduleEpochExhausted = false;
    }

    // Negative-only overflow setup; no origin is issued by this helper.
    static void SeedStoragePinCount(OccurrenceStatePool& pool,
                                    OccurrenceToken token, std::uint32_t count) {
        pool.m_entries.at(token.slot).originalStoragePinCount = count;
    }

    static OccurrenceToken Token(std::uint32_t slot,
                                 std::uint32_t generation) {
        return OccurrenceToken{slot, generation};
    }
};

} // namespace harken::detail

namespace {

OccurrenceKey KeyFor(PlayableInstanceHandle arranging, VnObjectId clipId)
{
    OccurrenceKey key;
    key.arrangingInstance = arranging;
    key.clipId = clipId;
    key.loopIteration = 0;
    key.occurrenceOrdinal = 0;
    return key;
}

// Walks an occurrence from Preparing to the fence.
void RunToFence(OccurrenceStatePool& pool, const OccurrenceToken& token)
{
    pool.Advance(token, OccurrenceStage::Active);
    pool.Advance(token, OccurrenceStage::LogicalDone);
    pool.Advance(token, OccurrenceStage::AwaitingEventFence);
}

} // namespace

TEST(OccurrenceStatePool, AcquiredOccurrenceCarriesItsKeyAndAStamp)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(4);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(piece, 0u);

    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(77)), &token));
    EXPECT_NE(token.generation, 0u);
    EXPECT_EQ(pool.StageOf(token), OccurrenceStage::Preparing);

    const OccurrenceKey* key = pool.KeyOf(token);
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->arrangingInstance, piece);
    EXPECT_EQ(key->clipId, harkentest::ObjectId(77u));
    EXPECT_NE(pool.ScheduleEpochOf(token), 0u);
}

TEST(OccurrenceStatePool, CompletionProgressIsInlineAndResolvedByExactToken)
{
    PlayableInstanceTable instances(1);
    OccurrenceStatePool pool(1);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(piece, 0u);
    OccurrenceToken token{};
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(81)), &token));
    ASSERT_TRUE(pool.BindCompletionProgress(token, 17, 23));

    harken::CompletionProgressRow* row = pool.CompletionProgressOf(token);
    ASSERT_NE(nullptr, row);
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = token;
    update.renderGeneration = 17;
    update.sourceOutputFramesThisBlock = 4;
    update.playbackFactsRevision = 23;
    update.completedThroughBlockSequence = 9;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(*row, update));
    EXPECT_EQ(row, pool.CompletionProgressOf(token));

    harken::CompletionProgressSnapshot captured{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, 17, &captured));
    EXPECT_EQ(4u, captured.logicalOutputFramesProduced);
    RunToFence(pool, token);
    ASSERT_TRUE(pool.ReleaseAfterFence(token));
    EXPECT_EQ(nullptr, pool.CompletionProgressOf(token));
}

// Both lineage depths use the same key shape, so a directly triggered Track and
// a Track inside a Piece are not two code paths. The property under test is
// that occurrences of the two hosts do not collide even when the clip id and
// ordinal match exactly.
TEST(OccurrenceStatePool, KeepsBothLineageDepthsApartUnderIdenticalClipIds)
{
    PlayableInstanceTable instances(4);
    OccurrenceStatePool pool(4);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    const PlayableInstanceHandle track =
        instances.Acquire(PlayableKind::Track, 0);
    ASSERT_NE(piece, 0u);
    ASSERT_NE(track, 0u);

    OccurrenceToken inPiece;
    OccurrenceToken direct;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(5)), &inPiece));
    ASSERT_TRUE(pool.Acquire(KeyFor(track, harkentest::ObjectId(5)), &direct));

    EXPECT_NE(inPiece.slot, direct.slot);
    EXPECT_EQ(pool.KeyOf(inPiece)->arrangingInstance, piece);
    EXPECT_EQ(pool.KeyOf(direct)->arrangingInstance, track);
    // Cancelling one host's occurrence must not disturb the other's.
    EXPECT_NE(pool.BumpScheduleEpoch(inPiece), 0u);
    EXPECT_TRUE(pool.ValidateTransaction(
        direct, pool.ScheduleEpochOf(direct),
        instances.Classify(track).scheduleEpoch, instances));
}

// A fence-waiting record is not headroom. If acquire could take its slot, or a
// capacity reading counted it as free, the events it is still waiting on would
// be attributed to whatever moved in.
TEST(OccurrenceStatePool, FenceWaitingRecordStillOwnsItsSlot)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(1);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(piece, 0u);

    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &token));
    RunToFence(pool, token);

    EXPECT_EQ(pool.StageOf(token), OccurrenceStage::AwaitingEventFence);
    EXPECT_EQ(pool.AwaitingFenceCount(), 1u);
    EXPECT_EQ(pool.OccupiedCount(), 1u);

    OccurrenceToken refused;
    EXPECT_FALSE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(2)), &refused));

    ASSERT_TRUE(pool.ReleaseAfterFence(token));
    EXPECT_EQ(pool.AwaitingFenceCount(), 0u);
    EXPECT_EQ(pool.OccupiedCount(), 0u);
    EXPECT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(2)), &refused));
}

// Releasing before the fence would reuse a slot whose events have not been
// consumed, which is exactly the case the fence stage exists to prevent.
TEST(OccurrenceStatePool, RefusesReleaseBeforeTheFenceStage)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(2);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &token));

    EXPECT_FALSE(pool.ReleaseAfterFence(token));
    ASSERT_TRUE(pool.Advance(token, OccurrenceStage::Active));
    EXPECT_FALSE(pool.ReleaseAfterFence(token));
    ASSERT_TRUE(pool.Advance(token, OccurrenceStage::LogicalDone));
    EXPECT_FALSE(pool.ReleaseAfterFence(token));
    ASSERT_TRUE(pool.Advance(token, OccurrenceStage::AwaitingEventFence));
    EXPECT_TRUE(pool.ReleaseAfterFence(token));
}

TEST(OccurrenceStatePool, RefusesStageMovesThatAreNotStrictlyForward)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(2);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &token));

    ASSERT_TRUE(pool.Advance(token, OccurrenceStage::LogicalDone));
    EXPECT_FALSE(pool.Advance(token, OccurrenceStage::Active));
    EXPECT_FALSE(pool.Advance(token, OccurrenceStage::LogicalDone));
    EXPECT_FALSE(pool.Advance(token, OccurrenceStage::Free));
    EXPECT_EQ(pool.StageOf(token), OccurrenceStage::LogicalDone);
}

// A token to a released occurrence must not address the slot's next occupant.
TEST(OccurrenceStatePool, TokenGoesStaleWhenTheSlotIsReleasedAndReused)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(1);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceToken first;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &first));
    RunToFence(pool, first);
    ASSERT_TRUE(pool.ReleaseAfterFence(first));

    EXPECT_EQ(pool.StageOf(first), OccurrenceStage::Free);
    EXPECT_EQ(pool.KeyOf(first), nullptr);

    OccurrenceToken second;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(2)), &second));
    EXPECT_EQ(second.slot, first.slot);
    EXPECT_NE(second.generation, first.generation);
    // The reused slot answers only to the new token.
    EXPECT_EQ(pool.KeyOf(first), nullptr);
    ASSERT_NE(pool.KeyOf(second), nullptr);
    EXPECT_EQ(pool.KeyOf(second)->clipId, harkentest::ObjectId(2u));
}

// The single most important case in this file: the occurrence's own stamp is
// still current, and the transaction must still be refused because its
// arranging instance was cancelled. A validator that checked only the
// occurrence would let this work through.
TEST(OccurrenceStatePool, RefusesATransactionWhoseArrangingInstanceWasCancelled)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(2);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(piece, 0u);
    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &token));

    const std::uint64_t occurrenceStamp = pool.ScheduleEpochOf(token);
    const std::uint64_t instanceStamp =
        instances.Classify(piece).scheduleEpoch;
    ASSERT_TRUE(pool.ValidateTransaction(token, occurrenceStamp, instanceStamp,
                                         instances));

    // Cancel the whole host. The occurrence was not touched, so its own stamp
    // still matches.
    ASSERT_NE(instances.BumpScheduleEpoch(piece), 0u);
    EXPECT_EQ(pool.ScheduleEpochOf(token), occurrenceStamp);
    EXPECT_FALSE(pool.ValidateTransaction(token, occurrenceStamp, instanceStamp,
                                          instances));
}

// The mirror image: the host is untouched and the single occurrence was
// cancelled. A validator that checked only the instance would let this through.
TEST(OccurrenceStatePool, RefusesATransactionWhoseOccurrenceWasCancelled)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(2);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &token));

    const std::uint64_t occurrenceStamp = pool.ScheduleEpochOf(token);
    const std::uint64_t instanceStamp =
        instances.Classify(piece).scheduleEpoch;

    ASSERT_NE(pool.BumpScheduleEpoch(token), 0u);
    EXPECT_EQ(instances.Classify(piece).scheduleEpoch, instanceStamp);
    EXPECT_FALSE(pool.ValidateTransaction(token, occurrenceStamp, instanceStamp,
                                          instances));
    // Still valid under the occurrence's new stamp, so the cancel was scoped to
    // the record rather than to the host.
    EXPECT_TRUE(pool.ValidateTransaction(token, pool.ScheduleEpochOf(token),
                                         instanceStamp, instances));
}

// A retired host has no standing either, which is the case an epoch comparison
// alone would miss because the retired instance no longer publishes a stamp.
TEST(OccurrenceStatePool, RefusesATransactionWhoseArrangingInstanceRetired)
{
    PlayableInstanceTable instances(2);
    OccurrenceStatePool pool(2);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceToken token;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &token));
    const std::uint64_t occurrenceStamp = pool.ScheduleEpochOf(token);
    const std::uint64_t instanceStamp =
        instances.Classify(piece).scheduleEpoch;

    instances.AdvanceLifecycle(piece, PlayableLifecycle::LogicalComplete);
    instances.AdvanceLifecycle(piece, PlayableLifecycle::AudibleRetiring);
    instances.AdvanceLifecycle(piece, PlayableLifecycle::Retired);

    EXPECT_FALSE(pool.ValidateTransaction(token, occurrenceStamp, instanceStamp,
                                          instances));
}

TEST(OccurrenceStatePool,
     QuarantinesTheSlotWhenGenerationExhaustsInsteadOfWrapping)
{
    const std::uint32_t maximum =
        (std::numeric_limits<std::uint32_t>::max)();
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle piece =
        instances.Acquire(PlayableKind::Piece, 0);
    ASSERT_NE(piece, 0u);

    OccurrenceStatePool pool(1);
    harken::detail::OccurrenceStatePoolTestAccess::SeedGeneration(
        pool, 0, maximum - 1);

    OccurrenceToken penultimate;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(1)), &penultimate));
    EXPECT_EQ(penultimate.generation, maximum - 1);
    RunToFence(pool, penultimate);
    ASSERT_TRUE(pool.ReleaseAfterFence(penultimate));

    // The maximum generation is still available for the slot's final reuse.
    EXPECT_EQ(pool.FreeCount(), 1u);
    EXPECT_EQ(pool.QuarantinedCount(), 0u);
    OccurrenceToken maximumToken;
    ASSERT_TRUE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(2)), &maximumToken));
    EXPECT_EQ(maximumToken.generation, maximum);
    EXPECT_EQ(pool.StageOf(penultimate), OccurrenceStage::Free);
    RunToFence(pool, maximumToken);
    ASSERT_TRUE(pool.ReleaseAfterFence(maximumToken));

    // Exhaustion quarantines the slot rather than reviving generation one.
    EXPECT_EQ(pool.OccupiedCount(), 0u);
    EXPECT_EQ(pool.FreeCount(), 0u);
    EXPECT_EQ(pool.QuarantinedCount(), 1u);
    OccurrenceToken refused;
    EXPECT_FALSE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(3)), &refused));
    EXPECT_EQ(pool.StageOf(maximumToken), OccurrenceStage::Free);
    EXPECT_EQ(pool.StageOf(
                  harken::detail::OccurrenceStatePoolTestAccess::Token(0, 1)),
              OccurrenceStage::Free);
}


TEST(OccurrenceStatePool, OriginalPrepareStorageSurvivesRealCommitAndEndsOnlyAfterNativeFence)
{
    using Pin = OccurrenceStatePool::OriginalStoragePin;
    static_assert(!std::is_copy_constructible_v<Pin>);
    static_assert(!std::is_copy_assignable_v<Pin>);
    static_assert(std::is_nothrow_move_constructible_v<Pin>);
    PlayableInstanceTable instances(1);
    const auto piece = instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceStatePool pool(1);
    OccurrenceToken token{};
    Pin own;
    ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(101)), &token, &own));
    ASSERT_TRUE(pool.IsOriginalStoragePinFor(own, token));
    EXPECT_EQ(0u, pool.HighWater());
    EXPECT_FALSE(own.NativeEnded());
    EXPECT_EQ(nullptr, pool.CompletionProgressOf(token));
    harken::CompletionOwnerRef absent{};
    EXPECT_FALSE(pool.CaptureCompletionOwner(token, &absent));
    Pin foreign;
    ASSERT_TRUE(own.Borrow(&foreign));
    Pin moved(std::move(own));
    EXPECT_FALSE(own.Valid());
    const auto originalEpoch = pool.ScheduleEpochOf(token);
    ASSERT_GT(pool.BumpScheduleEpoch(token), originalEpoch);
    ASSERT_TRUE(pool.IsOriginalStoragePinFor(foreign, token));
    ASSERT_TRUE(pool.BindCompletionProgress(token, 17, 23));
    auto* progress = pool.CompletionProgressOf(token);
    ASSERT_NE(nullptr, progress);
    harken::CompletionProgressBlockUpdate update{};
    update.occurrence = token;
    update.renderGeneration = 17;
    update.playbackFactsRevision = 23;
    update.sourceOutputFramesThisBlock = 8;
    update.completedThroughBlockSequence = 9;
    ASSERT_EQ(harken::CompletionProgressPublishStatus::Ok,
              harken::PublishCompletionProgressBlock(*progress, update));
    ASSERT_TRUE(pool.CommitPreparedAcquire(token));
    EXPECT_EQ(1u, pool.HighWater());
    EXPECT_FALSE(pool.ReleaseAfterFence(token, 10, nullptr, &moved));
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(foreign.NativeEnded());
    RunToFence(pool, token);
    ASSERT_TRUE(pool.ReleaseAfterFence(token, 10, nullptr, &moved));
    EXPECT_FALSE(moved.Valid());
    EXPECT_TRUE(foreign.NativeEnded());
    EXPECT_EQ(nullptr, pool.KeyOf(token));
    EXPECT_EQ(nullptr, pool.CompletionProgressOf(token));
    EXPECT_EQ(OccurrenceStage::Free, pool.StageOf(token));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.AwaitingFenceCount());
    EXPECT_EQ(0u, pool.FreeCount());
    EXPECT_EQ(1u, pool.RetainedStorageCount());
    OccurrenceKey captured{};
    ASSERT_TRUE(foreign.CaptureOriginalKey(&captured));
    EXPECT_EQ(piece, captured.arrangingInstance);
    EXPECT_EQ(harkentest::ObjectId(101), captured.clipId);
    OccurrenceToken refused{37, 41};
    EXPECT_FALSE(pool.Acquire(KeyFor(piece, harkentest::ObjectId(102)), &refused));
    EXPECT_EQ(37u, refused.slot);
    EXPECT_EQ(41u, refused.generation);
    foreign.Release();
    EXPECT_EQ(0u, pool.RetainedStorageCount());
    EXPECT_EQ(1u, pool.FreeCount());
    OccurrenceToken next{};
    ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(102)), &next));
    EXPECT_EQ(token.slot, next.slot);
    EXPECT_GT(next.generation, token.generation);
    ASSERT_TRUE(pool.CancelPreparedAcquire(next));
}

TEST(OccurrenceStatePool, OriginalPrepareStorageCancelConsumesOnlyItsActualBaseline)
{
    PlayableInstanceTable instances(1);
    const auto piece = instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceStatePool pool(1);
    OccurrenceToken token{};
    OccurrenceStatePool::OriginalStoragePin own;
    ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(201)), &token, &own));
    OccurrenceStatePool::OriginalStoragePin foreign;
    ASSERT_TRUE(own.Borrow(&foreign));
    const auto epoch = pool.ScheduleEpochOf(token);
    EXPECT_FALSE(pool.CancelPreparedAcquire(token, nullptr, &foreign));
    EXPECT_TRUE(own.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_FALSE(foreign.NativeEnded());
    EXPECT_EQ(OccurrenceStage::Preparing, pool.StageOf(token));
    EXPECT_EQ(epoch, pool.ScheduleEpochOf(token));
    ASSERT_TRUE(pool.BindCompletionProgress(token, 3, 7));
    EXPECT_FALSE(pool.CancelPreparedAcquire(token, nullptr, &own));
    EXPECT_TRUE(own.Valid());
    ASSERT_TRUE(pool.UnbindCompletionProgress(token, 3));
    ASSERT_TRUE(pool.CancelPreparedAcquire(token, nullptr, &own));
    EXPECT_FALSE(own.Valid());
    EXPECT_TRUE(foreign.NativeEnded());
    EXPECT_EQ(1u, pool.RetainedStorageCount());
    EXPECT_EQ(0u, pool.FreeCount());
    OccurrenceKey original{};
    ASSERT_TRUE(foreign.CaptureOriginalKey(&original));
    EXPECT_EQ(harkentest::ObjectId(201), original.clipId);
    foreign.Release();
    EXPECT_EQ(1u, pool.FreeCount());
    OccurrenceToken next{};
    OccurrenceStatePool::OriginalStoragePin nextOwn;
    ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(202)), &next, &nextOwn));
    // Native cancellation preserves the slot generation; the native epoch is new.
    EXPECT_EQ(token.slot, next.slot);
    EXPECT_EQ(token.generation, next.generation);
    EXPECT_GT(pool.ScheduleEpochOf(next), epoch);
    EXPECT_FALSE(foreign.CaptureOriginalKey(&original));
    ASSERT_TRUE(pool.CancelPreparedAcquire(next, nullptr, &nextOwn));
    EXPECT_EQ(0u, pool.RetainedStorageCount());
}

TEST(OccurrenceStatePool, OriginalPrepareStorageReceiverMoveAndDestructionKeepActualEpisodesSeparate)
{
    PlayableInstanceTable instances(1);
    const auto piece = instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceStatePool left(1), right(1);
    OccurrenceToken first{}, second{};
    OccurrenceStatePool::OriginalStoragePin leftOwn, rightOwn;
    ASSERT_TRUE(left.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(301)), &first, &leftOwn));
    ASSERT_TRUE(right.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(302)), &second, &rightOwn));
    ASSERT_EQ(first.slot, second.slot);
    ASSERT_EQ(first.generation, second.generation);
    ASSERT_EQ(left.ScheduleEpochOf(first), right.ScheduleEpochOf(second));
    EXPECT_FALSE(right.IsOriginalStoragePinFor(leftOwn, second));
    EXPECT_FALSE(left.IsOriginalStoragePinFor(rightOwn, first));
    EXPECT_FALSE(left.CancelPreparedAcquire(first, nullptr, &rightOwn));
    OccurrenceToken unchanged{13, 17};
    EXPECT_FALSE(left.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(303)), &unchanged, &rightOwn));
    EXPECT_EQ(13u, unchanged.slot);
    EXPECT_EQ(17u, unchanged.generation);
    EXPECT_EQ(1u, left.OccupiedCount());
    EXPECT_EQ(1u, right.OccupiedCount());
    {
        OccurrenceStatePool::OriginalStoragePin output;
        ASSERT_TRUE(leftOwn.Borrow(&output));
        OccurrenceStatePool::OriginalStoragePin rightForeign;
        ASSERT_TRUE(rightOwn.Borrow(&rightForeign));
        EXPECT_FALSE(leftOwn.Borrow(&rightForeign));
        ASSERT_TRUE(left.CancelPreparedAcquire(first, nullptr, &leftOwn));
        ASSERT_TRUE(right.CancelPreparedAcquire(second, nullptr, &rightOwn));
        EXPECT_EQ(1u, left.RetainedStorageCount());
        EXPECT_EQ(1u, right.RetainedStorageCount());
        output = std::move(rightForeign);
        EXPECT_FALSE(rightForeign.Valid());
        EXPECT_EQ(0u, left.RetainedStorageCount());
        EXPECT_EQ(1u, left.FreeCount());
        EXPECT_TRUE(right.IsOriginalStoragePinFor(output, second));
        EXPECT_TRUE(output.NativeEnded());
        OccurrenceKey captured{};
        ASSERT_TRUE(output.CaptureOriginalKey(&captured));
        EXPECT_EQ(harkentest::ObjectId(302), captured.clipId);
    }
    EXPECT_EQ(0u, right.RetainedStorageCount());
    EXPECT_EQ(1u, right.FreeCount());
}

TEST(OccurrenceStatePool, OriginalPrepareStorageMaximumGenerationAndActualEpochNeverWrap)
{
    using Access = harken::detail::OccurrenceStatePoolTestAccess;
    PlayableInstanceTable instances(1);
    const auto piece = instances.Acquire(PlayableKind::Piece, 0);
    {
        OccurrenceStatePool pool(1);
        Access::SeedGeneration(pool, 0, UINT32_MAX);
        OccurrenceToken token{};
        OccurrenceStatePool::OriginalStoragePin own, foreign;
        ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(401)), &token, &own));
        ASSERT_TRUE(own.Borrow(&foreign));
        ASSERT_TRUE(pool.CommitPreparedAcquire(token));
        RunToFence(pool, token);
        ASSERT_TRUE(pool.ReleaseAfterFence(token, 0, nullptr, &own));
        EXPECT_EQ(0u, pool.QuarantinedCount());
        EXPECT_TRUE(foreign.NativeEnded());
        EXPECT_EQ(UINT32_MAX, foreign.OriginalToken().generation);
        foreign.Release();
        EXPECT_EQ(1u, pool.QuarantinedCount());
        EXPECT_EQ(0u, pool.FreeCount());
        EXPECT_EQ(0u, pool.RetainedStorageCount());
        OccurrenceToken unchanged{11, 19};
        OccurrenceStatePool::OriginalStoragePin refused;
        EXPECT_FALSE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(402)), &unchanged, &refused));
        EXPECT_EQ(11u, unchanged.slot);
        EXPECT_EQ(19u, unchanged.generation);
        EXPECT_FALSE(refused.Valid());
    }
    {
        OccurrenceStatePool pool(1);
        Access::SeedScheduleEpoch(pool, UINT64_MAX);
        OccurrenceToken last{};
        OccurrenceStatePool::OriginalStoragePin own, foreign;
        ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(403)), &last, &own));
        ASSERT_TRUE(own.Borrow(&foreign));
        EXPECT_EQ(UINT64_MAX, pool.ScheduleEpochOf(last));
        EXPECT_TRUE(pool.ScheduleEpochExhausted());
        EXPECT_EQ(0u, pool.BumpScheduleEpoch(last));
        EXPECT_TRUE(own.Valid());
        ASSERT_TRUE(pool.CancelPreparedAcquire(last, nullptr, &own));
        EXPECT_TRUE(foreign.NativeEnded());
        foreign.Release();
        EXPECT_EQ(1u, pool.FreeCount());
        OccurrenceToken unchanged{23, 29};
        OccurrenceStatePool::OriginalStoragePin refused;
        EXPECT_FALSE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(404)), &unchanged, &refused));
        EXPECT_EQ(23u, unchanged.slot);
        EXPECT_EQ(29u, unchanged.generation);
        EXPECT_FALSE(refused.Valid());
        EXPECT_EQ(0u, pool.OccupiedCount());
        EXPECT_EQ(0u, pool.RetainedStorageCount());
    }
}

TEST(OccurrenceStatePool, OriginalPrepareStorageBorrowOverflowAndEarlyReturnNeverEndTheNativeOwner)
{
    PlayableInstanceTable instances(1);
    const auto piece = instances.Acquire(PlayableKind::Piece, 0);
    OccurrenceStatePool pool(1);
    OccurrenceToken token{};
    OccurrenceStatePool::OriginalStoragePin own, foreign;
    ASSERT_TRUE(pool.PrepareAcquire(KeyFor(piece, harkentest::ObjectId(501)), &token, &own));
    harken::detail::OccurrenceStatePoolTestAccess::SeedStoragePinCount(pool, token, UINT32_MAX);
    EXPECT_FALSE(own.Borrow(&foreign));
    EXPECT_TRUE(own.Valid());
    EXPECT_FALSE(foreign.Valid());
    EXPECT_EQ(OccurrenceStage::Preparing, pool.StageOf(token));
    harken::detail::OccurrenceStatePoolTestAccess::SeedStoragePinCount(pool, token, 1);
    ASSERT_TRUE(own.Borrow(&foreign));
    foreign.Release();
    EXPECT_FALSE(own.NativeEnded());
    EXPECT_EQ(1u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.RetainedStorageCount());
    own.Release();
    EXPECT_EQ(OccurrenceStage::Preparing, pool.StageOf(token));
    EXPECT_EQ(1u, pool.OccupiedCount());
    EXPECT_EQ(0u, pool.FreeCount());
    ASSERT_TRUE(pool.CommitPreparedAcquire(token));
    RunToFence(pool, token);
    ASSERT_TRUE(pool.ReleaseAfterFence(token));
    EXPECT_EQ(1u, pool.FreeCount());
    EXPECT_EQ(0u, pool.RetainedStorageCount());
}


TEST(OccurrenceStatePool, OriginalPrepareStorageRegisteredNativeEndRefusesForeignBaselineAndReturnsRealOwn)
{
    OccurrenceStatePool pool(1);
    struct NativeOwner {
        OccurrenceStatePool* receiver = nullptr;
        OccurrenceStatePool::OriginalStoragePin own, foreign;
        bool returnForeign = false;
    } native{&pool};
    const auto resolver = +[](void* context, OccurrenceStatePool& actual,
                              const OccurrenceToken& token) noexcept -> OccurrenceStatePool::OriginalStoragePin* {
        auto* owner = static_cast<NativeOwner*>(context);
        if (owner->receiver != &actual || !actual.IsOriginalStoragePinFor(owner->own, token)) return nullptr;
        return owner->returnForeign ? &owner->foreign : &owner->own;
    };
    ASSERT_TRUE(pool.RegisterOriginalStorageBaselineResolver(&native, resolver));
    EXPECT_FALSE(pool.RegisterOriginalStorageBaselineResolver(&native, resolver));
    OccurrenceToken token{};
    ASSERT_TRUE(pool.PrepareAcquire({}, &token, &native.own));
    ASSERT_TRUE(native.own.Borrow(&native.foreign));
    native.returnForeign = true;
    EXPECT_FALSE(pool.CancelPreparedAcquire(token));
    EXPECT_TRUE(native.own.Valid());
    EXPECT_FALSE(native.foreign.NativeEnded());
    EXPECT_EQ(OccurrenceStage::Preparing, pool.StageOf(token));
    native.returnForeign = false;
    ASSERT_TRUE(pool.CancelPreparedAcquire(token));
    EXPECT_FALSE(native.own.Valid());
    EXPECT_TRUE(native.foreign.NativeEnded());
    EXPECT_EQ(1u, pool.RetainedStorageCount());
    native.foreign.Release();
    EXPECT_EQ(1u, pool.FreeCount());
    ASSERT_TRUE(pool.PrepareAcquire({}, &token, &native.own));
    ASSERT_TRUE(native.own.Borrow(&native.foreign));
    ASSERT_TRUE(pool.CommitPreparedAcquire(token));
    RunToFence(pool, token);
    native.returnForeign = true;
    EXPECT_FALSE(pool.ReleaseAfterFence(token));
    EXPECT_EQ(1u, pool.AwaitingFenceCount());
    native.returnForeign = false;
    ASSERT_TRUE(pool.ReleaseAfterFence(token));
    EXPECT_FALSE(native.own.Valid());
    EXPECT_TRUE(native.foreign.NativeEnded());
    native.foreign.Release();
    EXPECT_EQ(0u, pool.RetainedStorageCount());
    EXPECT_EQ(1u, pool.FreeCount());
}
