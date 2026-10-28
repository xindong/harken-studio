// Deferred-intent pool contract tests.
//
// Two ceilings and one stamp are what make this pool safe. The per-owner cap has
// to refuse distinguishably from global exhaustion, because one owner starving
// the pool and the pool being genuinely full call for different fixes. And an
// owner that ended or cancelled has to invalidate its intents without the
// canceller walking them, which is what the stamp is for.

#include "harken/core/DeferredIntentPool.h"
#include "harken/core/PlayableInstanceTable.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using harken::DeferredIntent;
using harken::DeferredIntentPool;
using harken::DeferredIntentRefusal;
using harken::PlayableInstanceHandle;
using harken::PlayableInstanceTable;
using harken::PlayableKind;
using harken::PlayableLifecycle;

namespace {

DeferredIntent IntentFor(const PlayableInstanceTable& instances,
                         PlayableInstanceHandle owner, std::uint64_t dueFrame,
                         std::uint64_t payload)
{
    DeferredIntent intent;
    intent.ownerDomain = harken::DeferredIntentOwnerDomain::Playable;
    intent.ownerHandle = owner;
    intent.ownerEpoch = instances.Classify(owner).scheduleEpoch;
    intent.dueFrame = dueFrame;
    intent.payload = payload;
    return intent;
}

} // namespace

TEST(DeferredIntentPool, HoldsIntentsUntilTheWindowReachesThem)
{
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Segment, 0);
    DeferredIntentPool pool(4, 4);
    ASSERT_EQ(pool.Add(IntentFor(instances, owner, 1000, 1)),
              DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(IntentFor(instances, owner, 5000, 2)),
              DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Count(), 2u);

    std::vector<DeferredIntent> materialized;
    EXPECT_EQ(pool.Materialize(2000, instances, &materialized), 0u);
    ASSERT_EQ(materialized.size(), 1u);
    EXPECT_EQ(materialized[0].payload, 1u);
    // The far-future intent is still held, not released early.
    EXPECT_EQ(pool.Count(), 1u);
}

// One owner queueing thousands of far-future intents is perfectly legal
// individually, so the cap is what stops it from consuming everyone else's room.
// It reports as its own refusal because the fix is not "make the pool bigger".
TEST(DeferredIntentPool, PerOwnerCapRefusesDistinguishablyFromGlobalExhaustion)
{
    PlayableInstanceTable instances(4);
    const PlayableInstanceHandle greedy =
        instances.Acquire(PlayableKind::Track, 0);
    const PlayableInstanceHandle other =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(8, 2);

    ASSERT_EQ(pool.Add(IntentFor(instances, greedy, 1000, 1)),
              DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(IntentFor(instances, greedy, 1001, 2)),
              DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Add(IntentFor(instances, greedy, 1002, 3)),
              DeferredIntentRefusal::OwnerCapReached);
    EXPECT_EQ(pool.CountForOwner(greedy), 2u);

    // The pool itself still has room, and another owner gets it.
    EXPECT_EQ(pool.Add(IntentFor(instances, other, 1003, 4)),
              DeferredIntentRefusal::None);
}

TEST(DeferredIntentPool, ReportsGlobalExhaustionWhenTheCapIsNotTheLimit)
{
    PlayableInstanceTable instances(4);
    const PlayableInstanceHandle a = instances.Acquire(PlayableKind::Sound, 0);
    const PlayableInstanceHandle b = instances.Acquire(PlayableKind::Sound, 0);
    DeferredIntentPool pool(2, 4);

    ASSERT_EQ(pool.Add(IntentFor(instances, a, 1000, 1)),
              DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(IntentFor(instances, b, 1000, 2)),
              DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Add(IntentFor(instances, a, 1000, 3)),
              DeferredIntentRefusal::PoolFull);
    EXPECT_EQ(pool.HighWater(), 2u);
}

// Cancellation is constant-time: the canceller bumps one epoch and never touches
// the pool. The intents it queued are dropped when the window reaches them, and
// the drop is counted so the caller does not have to infer it from a gap.
TEST(DeferredIntentPool, DropsIntentsWhoseOwnerWasCancelledWhenTheyComeDue)
{
    PlayableInstanceTable instances(4);
    const PlayableInstanceHandle cancelled =
        instances.Acquire(PlayableKind::Track, 0);
    const PlayableInstanceHandle live =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(4, 4);
    ASSERT_EQ(pool.Add(IntentFor(instances, cancelled, 500, 1)),
              DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(IntentFor(instances, live, 500, 2)),
              DeferredIntentRefusal::None);

    ASSERT_NE(instances.BumpScheduleEpoch(cancelled), 0u);

    std::vector<DeferredIntent> materialized;
    std::vector<std::uint64_t> droppedPayloads;
    EXPECT_EQ(pool.Materialize(1000, instances, &materialized,
                               &droppedPayloads),
              1u);
    ASSERT_EQ(materialized.size(), 1u);
    EXPECT_EQ(materialized[0].payload, 2u);
    ASSERT_EQ(droppedPayloads.size(), 1u);
    EXPECT_EQ(droppedPayloads[0], 1u);
    EXPECT_EQ(pool.Count(), 0u);
}

// A retired owner has no standing either, and the intent must not materialize
// just because its due frame arrived.
TEST(DeferredIntentPool, DropsIntentsWhoseOwnerRetired)
{
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Segment, 0);
    DeferredIntentPool pool(2, 2);
    ASSERT_EQ(pool.Add(IntentFor(instances, owner, 500, 1)),
              DeferredIntentRefusal::None);

    instances.AdvanceLifecycle(owner, PlayableLifecycle::LogicalComplete);
    instances.AdvanceLifecycle(owner, PlayableLifecycle::AudibleRetiring);
    instances.AdvanceLifecycle(owner, PlayableLifecycle::Retired);

    std::vector<DeferredIntent> materialized;
    EXPECT_EQ(pool.Materialize(1000, instances, &materialized), 1u);
    EXPECT_TRUE(materialized.empty());
}

// Reclaiming early is optional for correctness and useful for occupancy: the
// room comes back before the due frame arrives.
TEST(DeferredIntentPool, ReclaimsStaleIntentsBeforeTheirDueFrame)
{
    PlayableInstanceTable instances(4);
    const PlayableInstanceHandle cancelled =
        instances.Acquire(PlayableKind::Track, 0);
    const PlayableInstanceHandle live =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(4, 4);
    ASSERT_EQ(pool.Add(IntentFor(instances, cancelled, 100000, 1)),
              DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(IntentFor(instances, live, 100000, 2)),
              DeferredIntentRefusal::None);
    ASSERT_NE(instances.BumpScheduleEpoch(cancelled), 0u);

    std::vector<std::uint64_t> droppedPayloads;
    EXPECT_EQ(pool.DropStale(instances, &droppedPayloads), 1u);
    ASSERT_EQ(droppedPayloads.size(), 1u);
    EXPECT_EQ(droppedPayloads[0], 1u);
    EXPECT_EQ(pool.Count(), 1u);
    EXPECT_EQ(pool.CountForOwner(cancelled), 0u);
    EXPECT_EQ(pool.CountForOwner(live), 1u);
    // Dropping again finds nothing further to drop.
    EXPECT_EQ(pool.DropStale(instances), 0u);
}

// A refused add must leave the pool as it was, so a caller that retries later is
// not working against state its refusal created.
TEST(DeferredIntentPool, GenerationalClaimRetryReplaceReleaseAndCancel) {
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(1, 1);
    const auto added = pool.AddClaimable(IntentFor(instances, owner, 100, 1));
    ASSERT_EQ(added.status, DeferredIntentRefusal::None);
    ASSERT_TRUE(added.token.Valid());

    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(pool.ClaimDue(200, instances, &claim, 1), 1u);
    EXPECT_EQ(claim.token.generation, added.token.generation);
    EXPECT_EQ(pool.Retry(claim.token), DeferredIntentRefusal::None);
    EXPECT_FALSE(pool.Release(claim.token));

    ASSERT_EQ(pool.ClaimDue(200, instances, &claim, 1), 1u);
    const auto replaced =
        pool.Replace(claim.token, IntentFor(instances, owner, 300, 2));
    ASSERT_EQ(replaced.status, DeferredIntentRefusal::None);
    EXPECT_NE(replaced.token.generation, claim.token.generation);
    EXPECT_FALSE(pool.Cancel(claim.token));
    EXPECT_TRUE(pool.Cancel(replaced.token));
    EXPECT_EQ(pool.Count(), 0u);

    const auto reused = pool.AddClaimable(IntentFor(instances, owner, 400, 3));
    ASSERT_EQ(reused.status, DeferredIntentRefusal::None);
    EXPECT_NE(reused.token.generation, replaced.token.generation);
}

TEST(DeferredIntentPool, ReplaceSucceedsAtFullOccupancy) {
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(1, 1);
    const auto added = pool.AddClaimable(IntentFor(instances, owner, 100, 1));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(pool.ClaimDue(200, instances, &claim, 1), 1u);
    const auto replaced =
        pool.Replace(claim.token, IntentFor(instances, owner, 300, 2));
    EXPECT_EQ(replaced.status, DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Count(), 1u);
}

TEST(DeferredIntentPool, ARefusedAddChangesNothing)
{
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(1, 1);
    ASSERT_EQ(pool.Add(IntentFor(instances, owner, 500, 1)),
              DeferredIntentRefusal::None);

    EXPECT_EQ(pool.Add(IntentFor(instances, owner, 600, 2)),
              DeferredIntentRefusal::OwnerCapReached);
    EXPECT_EQ(pool.Count(), 1u);
    EXPECT_EQ(pool.CountForOwner(owner), 1u);

    std::vector<DeferredIntent> materialized;
    EXPECT_EQ(pool.Materialize(1000, instances, &materialized), 0u);
    ASSERT_EQ(materialized.size(), 1u);
    EXPECT_EQ(materialized[0].payload, 1u);
}

TEST(DeferredIntentPool,
     PreparedAbandonPreservesGenerationAndVisibleFacts) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(1, 1);
    ASSERT_TRUE(pool.SetGenerationForTests(0, UINT64_MAX));

    auto first = pool.AddPrepared(IntentFor(instances, owner, 100, 1));
    ASSERT_EQ(first.status, DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Count(), 0u);
    EXPECT_EQ(pool.HighWater(), 0u);
    ASSERT_TRUE(pool.AbandonPrepared(first.token));

    auto second = pool.AddPrepared(IntentFor(instances, owner, 100, 1));
    ASSERT_EQ(second.status, DeferredIntentRefusal::None);
    EXPECT_EQ(second.token.generation, UINT64_MAX);
    ASSERT_TRUE(pool.CommitPrepared(second.token));
    EXPECT_EQ(pool.Count(), 1u);
    EXPECT_EQ(pool.HighWater(), 1u);
}

TEST(DeferredIntentPool,
     ExecutionOrdinalPrecedesPayloadWithinOneDueClass) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(2, 2);

    DeferredIntent later = IntentFor(instances, owner, 100, 1);
    later.commitSequence = 7;
    later.executionOrdinal = 2;
    DeferredIntent earlier = IntentFor(instances, owner, 100, 2);
    earlier.commitSequence = 7;
    earlier.executionOrdinal = 1;
    ASSERT_EQ(pool.Add(later), DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(earlier), DeferredIntentRefusal::None);

    harken::DeferredIntentClaim claims[2]{};
    ASSERT_EQ(pool.ClaimDue(101, instances, claims, 2), 2u);
    EXPECT_EQ(claims[0].intent.payload, 2u);
    EXPECT_EQ(claims[1].intent.payload, 1u);
}

TEST(DeferredIntentPool, OwnerCapSeparatesPlayableAndActionDomains) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(2, 1);

    DeferredIntent playable = IntentFor(instances, owner, 100, 1);
    DeferredIntent action = playable;
    action.ownerDomain = harken::DeferredIntentOwnerDomain::Action;
    action.ownerHandle = owner;
    action.ownerEpoch = 9;
    action.payload = 2;

    EXPECT_EQ(pool.Add(playable), DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Add(action), DeferredIntentRefusal::None);
    EXPECT_EQ(pool.Count(), 2u);
}

TEST(DeferredIntentPool,
     NotBeforeBlocksItsDueClassWithoutBlockingLaterClasses) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Track, 0);
    DeferredIntentPool pool(3, 3);

    DeferredIntent blocked = IntentFor(instances, owner, 100, 1);
    blocked.commitSequence = 1;
    blocked.notBeforeQuantum = 5;
    DeferredIntent sameDue = IntentFor(instances, owner, 100, 2);
    sameDue.commitSequence = 2;
    DeferredIntent laterDue = IntentFor(instances, owner, 200, 3);
    ASSERT_EQ(pool.Add(blocked), DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(sameDue), DeferredIntentRefusal::None);
    ASSERT_EQ(pool.Add(laterDue), DeferredIntentRefusal::None);

    harken::DeferredIntentClaim claims[3]{};
    ASSERT_EQ(pool.ClaimDue(300, instances, claims, 3, nullptr, nullptr, 4),
              1u);
    EXPECT_EQ(claims[0].intent.payload, 3u);
    ASSERT_TRUE(pool.Release(claims[0].token));

    ASSERT_EQ(pool.ClaimDue(300, instances, claims, 3, nullptr, nullptr, 5),
              2u);
    EXPECT_EQ(claims[0].intent.payload, 1u);
    EXPECT_EQ(claims[1].intent.payload, 2u);
}
