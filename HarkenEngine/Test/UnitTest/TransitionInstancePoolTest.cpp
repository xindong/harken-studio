#include "harken/core/TransitionInstancePool.h"
#include "harken/core/ImmediateActionLane.h"

#include <gtest/gtest.h>

namespace {
struct PrivateTimingLane {
    harken::ImmediateActionLane lane;
    harken::ImmediateLaneHandle handle{};
    harken::TimelineProgramView program{};
    bool Prepare() {
        if (!lane.Reset({1, 1, 1, 64}) || !lane.Reserve(71, 1, &handle)) return false;
        harken::ImmediateLaneEvent event{};
        event.event.program.program = &program;
        if (!lane.Prepare(handle, &event, 1)) return false;
        std::uint64_t anchor = 0;
        return lane.TryPlace(handle, 64, 0, &anchor);
    }
};
}

TEST(TransitionInstancePool, PrivateTimingBatchHoldsAllRowsBeforeChangingGeometry) {
    PrivateTimingLane privateLane;
    ASSERT_TRUE(privateLane.Prepare());
    harken::TransitionInstancePool pool(2);
    harken::TransitionInstanceToken first{}, second{};
    ASSERT_TRUE(pool.Acquire({1024, 16}, &first));
    ASSERT_TRUE(pool.Acquire({1280, 20}, &second));
    harken::TransitionInstancePool::PrivateTimingProof firstProof, secondProof;
    ASSERT_TRUE(pool.ProvePrivateTimingBeforePublication(first, privateLane.lane,
        privateLane.handle, 71, 1024, 16, 1088, 17, &firstProof));
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(second, privateLane.lane,
        privateLane.handle, 71, 1279, 20, 1344, 21, &secondProof));
    harken::TransitionInstanceSnapshot before{};
    ASSERT_TRUE(pool.Capture(first, &before));
    EXPECT_EQ(1024u, before.audibleFrame);
    EXPECT_EQ(16u, before.lockBlock);
    EXPECT_FALSE(pool.InvalidateReady(first));
    firstProof.Release();
    ASSERT_TRUE(pool.ProvePrivateTimingBeforePublication(first, privateLane.lane,
        privateLane.handle, 71, 1024, 16, 1088, 17, &firstProof));
    ASSERT_TRUE(pool.ProvePrivateTimingBeforePublication(second, privateLane.lane,
        privateLane.handle, 71, 1280, 20, 1344, 21, &secondProof));
    pool.ApplyPrivateTimingBeforePublication(firstProof);
    pool.ApplyPrivateTimingBeforePublication(secondProof);
    firstProof.Release();
    secondProof.Release();
    harken::TransitionInstanceSnapshot after{};
    ASSERT_TRUE(pool.Capture(first, &after));
    EXPECT_EQ(1088u, after.audibleFrame);
    EXPECT_EQ(17u, after.lockBlock);
    EXPECT_EQ(harken::TransitionAudibleGateState::Replaceable, after.state);
    ASSERT_TRUE(pool.Capture(second, &after));
    EXPECT_EQ(1344u, after.audibleFrame);
    EXPECT_EQ(21u, after.lockBlock);
    EXPECT_EQ(2u, pool.LiveCount());
}

TEST(TransitionInstancePool, PrivateTimingRejectsStaleLaneOwnerAndGateGeometry) {
    PrivateTimingLane privateLane;
    ASSERT_TRUE(privateLane.Prepare());
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({1024, 16}, &token));
    harken::TransitionInstancePool::PrivateTimingProof proof;
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(token, privateLane.lane,
        privateLane.handle, 72, 1024, 16, 1088, 17, &proof));
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(token, privateLane.lane,
        {privateLane.handle.slot, privateLane.handle.generation + 1u},
        71, 1024, 16, 1088, 17, &proof));
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(
        {token.slot, token.generation + 1u}, privateLane.lane,
        privateLane.handle, 71, 1024, 16, 1088, 17, &proof));
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(token, privateLane.lane,
        privateLane.handle, 71, 1024, 15, 1088, 17, &proof));
    harken::TransitionInstanceSnapshot unchanged{};
    ASSERT_TRUE(pool.Capture(token, &unchanged));
    EXPECT_EQ(1024u, unchanged.audibleFrame);
    EXPECT_EQ(16u, unchanged.lockBlock);
    EXPECT_EQ(harken::TransitionAudibleGateState::Replaceable, unchanged.state);
    EXPECT_FALSE(proof.Valid());
}

TEST(TransitionInstancePool, PrivateTimingRefusesPublishedLaneAndLockedGate) {
    PrivateTimingLane privateLane;
    ASSERT_TRUE(privateLane.Prepare());
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({1024, 16}, &token));
    harken::TransitionInstancePool::PrivateTimingProof proof;
    ASSERT_TRUE(privateLane.lane.Publish(privateLane.handle));
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(token, privateLane.lane,
        privateLane.handle, 71, 1024, 16, 1088, 17, &proof));
    ASSERT_TRUE(pool.CanLockFromAudioClaim(token));
    pool.LockFromClaimedAudioSlice(token);
    PrivateTimingLane otherPrivateLane;
    ASSERT_TRUE(otherPrivateLane.Prepare());
    EXPECT_FALSE(pool.ProvePrivateTimingBeforePublication(token, otherPrivateLane.lane,
        otherPrivateLane.handle, 71, 1024, 16, 1088, 17, &proof));
    harken::TransitionInstanceSnapshot unchanged{};
    ASSERT_TRUE(pool.Capture(token, &unchanged));
    EXPECT_EQ(1024u, unchanged.audibleFrame);
    EXPECT_EQ(16u, unchanged.lockBlock);
    EXPECT_EQ(harken::TransitionAudibleGateState::Locked, unchanged.state);
}

TEST(TransitionInstancePool,
     ExactCapacityAndOneMoreRefusalLeaveNoPartialState) {
    harken::TransitionInstancePool pool(2);
    ASSERT_TRUE(pool.Valid());
    EXPECT_EQ(2u, pool.Capacity());
    EXPECT_EQ(2u, pool.FreeCount());

    harken::TransitionInstanceToken first{};
    harken::TransitionInstanceToken second{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &first));
    ASSERT_TRUE(pool.Acquire({256, 2}, &second));
    ASSERT_TRUE(first.Valid());
    ASSERT_TRUE(second.Valid());
    EXPECT_EQ(2u, pool.LiveCount());
    EXPECT_EQ(0u, pool.FreeCount());

    harken::TransitionInstanceToken unchanged{77, 88};
    EXPECT_FALSE(pool.Acquire({384, 3}, &unchanged));
    EXPECT_EQ(77u, unchanged.slot);
    EXPECT_EQ(88u, unchanged.generation);
    EXPECT_EQ(2u, pool.LiveCount());
    EXPECT_EQ(0u, pool.FreeCount());
}

TEST(TransitionInstancePool,
     ReadyInvalidationChangesOnlyReplaceableInstance) {
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));

    harken::TransitionInstanceSnapshot before{};
    ASSERT_TRUE(pool.Capture(token, &before));
    EXPECT_EQ(harken::TransitionAudibleGateState::Replaceable,
              before.state);
    EXPECT_EQ(128u, before.audibleFrame);
    EXPECT_EQ(1u, before.lockBlock);

    EXPECT_TRUE(pool.InvalidateReady(token));
    harken::TransitionInstanceSnapshot after{};
    ASSERT_TRUE(pool.Capture(token, &after));
    EXPECT_EQ(harken::TransitionAudibleGateState::Invalidated,
              after.state);
    EXPECT_FALSE(pool.InvalidateReady(token));
    ASSERT_TRUE(pool.Capture(token, &after));
    EXPECT_EQ(harken::TransitionAudibleGateState::Invalidated,
              after.state);
}

TEST(TransitionInstancePool,
     AudioClaimedLockPreventsReadyInvalidation) {
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    ASSERT_TRUE(pool.CanLockFromAudioClaim(token));

    pool.LockFromClaimedAudioSlice(token);

    harken::TransitionInstanceSnapshot snapshot{};
    ASSERT_TRUE(pool.Capture(token, &snapshot));
    EXPECT_EQ(harken::TransitionAudibleGateState::Locked,
              snapshot.state);
    EXPECT_FALSE(pool.CanLockFromAudioClaim(token));
    EXPECT_FALSE(pool.InvalidateReady(token));
    ASSERT_TRUE(pool.Capture(token, &snapshot));
    EXPECT_EQ(harken::TransitionAudibleGateState::Locked,
              snapshot.state);
}

TEST(TransitionInstancePool,
     ClaimedAudioLockOverridesLateReadyInvalidation) {
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    ASSERT_TRUE(pool.CanLockFromAudioClaim(token));

    ASSERT_TRUE(pool.InvalidateReady(token));
    pool.LockFromClaimedAudioSlice(token);

    harken::TransitionInstanceSnapshot snapshot{};
    ASSERT_TRUE(pool.Capture(token, &snapshot));
    EXPECT_EQ(harken::TransitionAudibleGateState::Locked,
              snapshot.state);
}

TEST(TransitionInstancePool,
     RetiredTransitionCannotBeRelockedByLateClaimCompletion) {
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    ASSERT_TRUE(pool.InvalidateReady(token));
    ASSERT_TRUE(pool.BeginRetire(token, 41));

    pool.LockFromClaimedAudioSlice(token);

    harken::TransitionInstanceSnapshot snapshot{};
    ASSERT_TRUE(pool.Capture(token, &snapshot));
    EXPECT_EQ(harken::TransitionAudibleGateState::Retired,
              snapshot.state);
    EXPECT_TRUE(pool.Reclaim(token, 42));
}

TEST(TransitionInstancePool,
     RetiredInstanceRequiresStrictAudioFenceBeforeRelease) {
    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    ASSERT_TRUE(pool.InvalidateReady(token));

    ASSERT_TRUE(pool.BeginRetire(token, 41));
    harken::TransitionInstanceSnapshot snapshot{};
    ASSERT_TRUE(pool.Capture(token, &snapshot));
    EXPECT_EQ(harken::TransitionAudibleGateState::Retired,
              snapshot.state);
    EXPECT_EQ(41u, snapshot.retiredAtAudioSequence);
    EXPECT_FALSE(pool.Reclaim(token, 41));
    EXPECT_EQ(1u, pool.LiveCount());
    EXPECT_EQ(0u, pool.FreeCount());

    EXPECT_TRUE(pool.Reclaim(token, 42));
    EXPECT_EQ(0u, pool.LiveCount());
    EXPECT_EQ(1u, pool.FreeCount());
    EXPECT_FALSE(pool.Capture(token, &snapshot));

    harken::TransitionInstanceToken reused{};
    ASSERT_TRUE(pool.Acquire({256, 2}, &reused));
    EXPECT_EQ(token.slot, reused.slot);
    EXPECT_EQ(token.generation + 1u, reused.generation);
    EXPECT_FALSE(pool.Capture(token, &snapshot));
    EXPECT_TRUE(pool.Capture(reused, &snapshot));
}

TEST(TransitionInstancePool,
     GenerationMaxQuarantinesSlotWithoutWrappingOrPartialAcquire) {
    harken::TransitionInstancePool pool(1);
    ASSERT_TRUE(pool.SetGenerationForTests(
        0, harken::kMaxTransitionInstanceGeneration));

    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    ASSERT_EQ(harken::kMaxTransitionInstanceGeneration, token.generation);
    ASSERT_TRUE(pool.InvalidateReady(token));
    ASSERT_TRUE(pool.BeginRetire(token, 41));
    EXPECT_FALSE(pool.Reclaim(token, 42));
    EXPECT_EQ(0u, pool.LiveCount());
    EXPECT_EQ(0u, pool.FreeCount());
    EXPECT_EQ(1u, pool.QuarantinedCount());

    harken::TransitionInstanceSnapshot snapshot{};
    EXPECT_FALSE(pool.Capture(token, &snapshot));
    harken::TransitionInstanceToken unchanged{77, 88};
    EXPECT_FALSE(pool.Acquire({256, 2}, &unchanged));
    EXPECT_EQ(77u, unchanged.slot);
    EXPECT_EQ(88u, unchanged.generation);
}
