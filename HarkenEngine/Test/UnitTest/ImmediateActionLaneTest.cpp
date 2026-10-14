#include "harken/core/ImmediateActionLane.h"

#include <array>
#include <limits>
#include <thread>
#include <type_traits>
#include <gtest/gtest.h>

namespace {

harken::ImmediateLaneEvent Event(std::uint64_t relativeFrame,
                                const harken::TimelineProgramView* program,
                                std::uint32_t ordinal) {
    harken::ImmediateLaneEvent event{};
    event.relativeFrame = relativeFrame;
    event.event.program.program = program;
    event.event.executionOrdinal = ordinal;
    return event;
}

TEST(ImmediateActionLane, FutureBucketsAreDueWithoutAnotherLogicService) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 4, 2, 64}));
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(128, &program, 1)};
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 9 * 64, 4, &anchor));
    EXPECT_EQ(9u * 64u, anchor);
    ASSERT_TRUE(lane.Publish(handle));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 8));
    const auto* first = lane.CaptureDue(handle.slot, 9);
    ASSERT_NE(nullptr, first);
    ASSERT_EQ(1u, first->eventCount);
    ASSERT_TRUE(lane.Claim(handle, first));
    ASSERT_TRUE(lane.Finish(handle.slot, first, true));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 10));
    const auto* second = lane.CaptureDue(handle.slot, 11);
    ASSERT_NE(nullptr, second);
    ASSERT_EQ(1u, second->eventCount);
    ASSERT_NE(nullptr, lane.Events(handle.slot, second));
    EXPECT_EQ(11u * 64u, lane.Events(handle.slot, second)[0].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, second));
    ASSERT_TRUE(lane.Finish(handle.slot, second, true));
    EXPECT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, BucketClaimReceiptExistsOnlyWhileExactClaimIsOwned) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle first{};
    ASSERT_TRUE(lane.Reserve(1, 1, &first));
    ASSERT_TRUE(lane.Prepare(first, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(first, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(first));
    const auto* bucket = lane.CaptureDue(first.slot, 1);
    ASSERT_NE(nullptr, bucket);
    harken::ImmediateLaneBucketClaimReceipt receipt{};
    EXPECT_FALSE(lane.ValidateClaimReceipt(receipt, bucket));
    harken::ImmediateLaneBucketClaimReceipt rejected{};
    EXPECT_FALSE(lane.Claim({first.slot, first.generation + 1u},
                            bucket, &rejected));
    EXPECT_FALSE(rejected.Valid());
    ASSERT_TRUE(lane.Claim(first, bucket, &receipt));
    ASSERT_TRUE(receipt.Valid());
    EXPECT_EQ(first.slot, receipt.Slot());
    EXPECT_EQ(first.generation, receipt.Generation());
    EXPECT_EQ(1u, receipt.Quantum());
    EXPECT_TRUE(lane.ValidateClaimReceipt(receipt, bucket));
    ASSERT_TRUE(lane.Finish(first.slot, bucket, true));
    EXPECT_FALSE(lane.ValidateClaimReceipt(receipt, bucket));
    ASSERT_TRUE(lane.Retire(first));

    harken::ImmediateLaneHandle second{};
    ASSERT_TRUE(lane.Reserve(2, 1, &second));
    ASSERT_EQ(first.slot, second.slot);
    ASSERT_NE(first.generation, second.generation);
    ASSERT_TRUE(lane.Prepare(second, &event, 1));
    ASSERT_TRUE(lane.TryPlace(second, 128, 0, &anchor));
    ASSERT_TRUE(lane.Publish(second));
    const auto* reused = lane.CaptureDue(second.slot, 2);
    ASSERT_EQ(bucket, reused);
    EXPECT_FALSE(lane.ValidateClaimReceipt(receipt, reused));
    ASSERT_TRUE(lane.ReleaseCapture(second.slot, reused));
    ASSERT_TRUE(lane.Cancel(second));
    EXPECT_TRUE(lane.Retire(second));
}

TEST(ImmediateActionLane, SourceWaitSuspensionReturnsCaptureAndResumesExactBucket) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 1, &handle));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* original = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, original);
    std::uint64_t quantum = 0;
    EXPECT_FALSE(lane.CapturedSourceWaitQuantum(handle.slot, original, &quantum));
    ASSERT_TRUE(lane.SuspendCaptureForSourceWait(handle.slot, original));
    EXPECT_FALSE(lane.CapturedSourceWaitQuantum(handle.slot, original, &quantum));
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(lane.Observe(handle, &observation));
    EXPECT_FALSE(observation.complete);
    EXPECT_FALSE(observation.missed);
    const auto* resumed = lane.CaptureDue(handle.slot, 4);
    ASSERT_EQ(original, resumed);
    ASSERT_TRUE(lane.CapturedSourceWaitQuantum(handle.slot, resumed, &quantum));
    EXPECT_EQ(4u, quantum);
    EXPECT_EQ(1u, resumed->quantum);
    EXPECT_EQ(64u, lane.Events(handle.slot, resumed)[0].event.executeFrame);
    ASSERT_TRUE(lane.SuspendCaptureForSourceWait(handle.slot, resumed));
    resumed = lane.CaptureDue(handle.slot, 7);
    ASSERT_EQ(original, resumed);
    harken::ImmediateLaneBucketClaimReceipt receipt;
    ASSERT_TRUE(lane.Claim(handle, resumed, &receipt));
    EXPECT_EQ(1u, receipt.Quantum());
    EXPECT_FALSE(lane.SuspendCaptureForSourceWait(handle.slot, resumed));
    ASSERT_TRUE(lane.Finish(handle.slot, resumed, true));
    EXPECT_EQ(8u, resumed->completedAudioSequence.load());
    ASSERT_TRUE(lane.Observe(handle, &observation));
    EXPECT_TRUE(observation.applied);
    EXPECT_FALSE(observation.missed);
    EXPECT_TRUE(lane.Retire(handle, 8));
}

TEST(ImmediateActionLane, SuspendedSourceWaitAllowsCancellationAndRejectsReusedGeneration) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 1, &handle));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.SuspendCaptureForSourceWait(handle.slot, bucket));
    ASSERT_TRUE(lane.Cancel(handle));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 8));
    ASSERT_TRUE(lane.Retire(handle, 9));
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(2, 1, &reused));
    ASSERT_NE(handle.generation, reused.generation);
    ASSERT_TRUE(lane.Prepare(reused, &event, 1));
    ASSERT_TRUE(lane.TryPlace(reused, 10 * 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(reused));
    // This attempt was never captured or suspended at its due time.
    EXPECT_EQ(nullptr, lane.CaptureDue(reused.slot, 11));
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(lane.Observe(reused, &observation));
    EXPECT_TRUE(observation.missed);
    EXPECT_TRUE(lane.Retire(reused, 12));
}

TEST(ImmediateActionLane, SourceWaitDoesNotRetainUnrelatedFutureWithoutEntryIdentity) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {Event(0, &program, 0), Event(128, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* first = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, first);
    ASSERT_TRUE(lane.SuspendCaptureForSourceWait(handle.slot, first));
    first = lane.CaptureDue(handle.slot, 3);
    ASSERT_NE(nullptr, first);
    ASSERT_TRUE(lane.Claim(handle, first));
    ASSERT_TRUE(lane.Finish(handle.slot, first, true));
    const auto* successor = lane.CaptureDue(handle.slot, 4);
    EXPECT_EQ(nullptr, successor);
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(lane.Observe(handle, &observation));
    EXPECT_TRUE(observation.complete);
    EXPECT_TRUE(observation.missed);
    EXPECT_TRUE(lane.Retire(handle, 5));
}

TEST(ImmediateActionLane, SourceWaitValidationRefusalRequiresActualResumedAudioCompletion) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 1, &handle));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.SuspendCaptureForSourceWait(handle.slot, bucket));
    bucket = lane.CaptureDue(handle.slot, 7);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.RejectCapturedBucket(handle, bucket));
    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
    harken::ImmediateLaneValidationRefusalSnapshot refused{};
    EXPECT_FALSE(lane.CapturePublishedValidationRefusal(handle, 2, &refused));
    EXPECT_FALSE(lane.CapturePublishedValidationRefusal(handle, 7, &refused));
    ASSERT_TRUE(lane.CapturePublishedValidationRefusal(handle, 8, &refused));
    EXPECT_EQ(8u, refused.audioSequence);
    EXPECT_TRUE(lane.Retire(handle, 9));
}

TEST(ImmediateActionLane, ExactBucketCreditRejectsASecondDueQuantum) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 4, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(128, &program, 1)};
    harken::ImmediateLaneHandle undercredited{};
    ASSERT_TRUE(lane.Reserve(1, {2, 0, 0, 1}, &undercredited));
    ASSERT_TRUE(lane.Prepare(undercredited, events, 2));
    std::uint64_t anchor = 0;
    EXPECT_FALSE(lane.TryPlace(undercredited, 64, 0, &anchor));
    ASSERT_TRUE(lane.Cancel(undercredited));
    ASSERT_TRUE(lane.Retire(undercredited));

    harken::ImmediateLaneHandle exact{};
    ASSERT_TRUE(lane.Reserve(2, {2, 0, 0, 2}, &exact));
    ASSERT_TRUE(lane.Prepare(exact, events, 2));
    ASSERT_TRUE(lane.TryPlace(exact, 64, 0, &anchor));
    EXPECT_EQ(64u, anchor);
    ASSERT_TRUE(lane.Cancel(exact));
    EXPECT_TRUE(lane.Retire(exact));
}

TEST(ImmediateActionLane, GroupOnlyQuantumConsumesBucketCredit) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneDeclaredGroup group{};
    group.identity.absoluteQuantum = 3;
    group.identity.pageGeneration = 1;
    group.identity.decision = {0, 1};
    group.identity.committedRow = 0;
    group.identity.rowClaimGeneration = 1;
    group.identity.transportEpoch = 1;
    group.relativeFrame = 128;

    harken::ImmediateLaneHandle undercredited{};
    ASSERT_TRUE(lane.Reserve(1, {1, 1, 0, 1}, &undercredited));
    ASSERT_TRUE(lane.DeclareGroup(undercredited, group));
    ASSERT_TRUE(lane.Prepare(undercredited, &event, 1));
    std::uint64_t anchor = 0;
    EXPECT_FALSE(lane.TryPlace(undercredited, 64, 0, &anchor));
    ASSERT_TRUE(lane.Cancel(undercredited));
    ASSERT_TRUE(lane.Retire(undercredited));

    harken::ImmediateLaneHandle exact{};
    ASSERT_TRUE(lane.Reserve(2, {1, 1, 0, 2}, &exact));
    ASSERT_TRUE(lane.DeclareGroup(exact, group));
    ASSERT_TRUE(lane.Prepare(exact, &event, 1));
    // These are producer layout observations, not Pool or audio permissions.
    constexpr std::uint64_t unchangedQuantum = UINT64_MAX;
    std::uint64_t placedLastQuantum = unchangedQuantum;
    EXPECT_FALSE(lane.CapturePrivatePlacedLastQuantum(exact, &placedLastQuantum));
    EXPECT_EQ(unchangedQuantum, placedLastQuantum);
    ASSERT_TRUE(lane.TryPlace(exact, 64, 0, &anchor));
    EXPECT_EQ(64u, anchor);
    ASSERT_TRUE(lane.CapturePrivatePlacedLastQuantum(exact, &placedLastQuantum));
    EXPECT_EQ(3u, placedLastQuantum); // Event is due at 1; group is due at 3.
    ASSERT_TRUE(lane.Cancel(exact));
    ASSERT_TRUE(lane.Retire(exact));

    // Obtain the replacement generation from actual slot reuse, without
    // constructing or advancing a token in the test.
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(3, {1, 1, 0, 2}, &reused));
    ASSERT_EQ(exact.slot, reused.slot);
    ASSERT_NE(exact.generation, reused.generation);
    ASSERT_TRUE(lane.DeclareGroup(reused, group));
    const auto laterEvent = Event(256, &program, 0);
    ASSERT_TRUE(lane.Prepare(reused, &laterEvent, 1));
    placedLastQuantum = unchangedQuantum;
    EXPECT_FALSE(lane.CapturePrivatePlacedLastQuantum(reused, &placedLastQuantum));
    EXPECT_EQ(unchangedQuantum, placedLastQuantum);
    ASSERT_TRUE(lane.TryPlace(reused, 64, 0, &anchor));
    ASSERT_TRUE(lane.CapturePrivatePlacedLastQuantum(reused, &placedLastQuantum));
    EXPECT_EQ(5u, placedLastQuantum); // Group is due at 3; event is due at 5.
    placedLastQuantum = unchangedQuantum;
    EXPECT_FALSE(lane.CapturePrivatePlacedLastQuantum(exact, &placedLastQuantum));
    EXPECT_EQ(unchangedQuantum, placedLastQuantum);
    ASSERT_TRUE(lane.Cancel(reused));
    ASSERT_TRUE(lane.Retire(reused));
}

TEST(ImmediateActionLane, GroupOnlyAttemptPlacesWithoutSyntheticEvent) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    harken::ImmediateLaneHandle empty{};
    ASSERT_TRUE(lane.Reserve(1, {0, 1, 0, 1}, &empty));
    EXPECT_FALSE(lane.Prepare(empty, nullptr, 0));
    ASSERT_TRUE(lane.Cancel(empty));
    ASSERT_TRUE(lane.Retire(empty));

    harken::ImmediateLaneDeclaredGroup group{};
    group.identity.absoluteQuantum = 0;
    group.identity.pageGeneration = 1;
    group.identity.decision = {0, 1};
    group.identity.committedRow = 0;
    group.identity.rowClaimGeneration = 1;
    group.identity.transportEpoch = 1;
    group.relativeFrame = 128;
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(2, {0, 1, 0, 1}, &handle));
    ASSERT_TRUE(lane.DeclareGroup(handle, group));
    ASSERT_TRUE(lane.Prepare(handle, nullptr, 0));
    // The original group-only layout needs no synthetic event or audio proof.
    constexpr std::uint64_t unchangedQuantum = UINT64_MAX;
    std::uint64_t placedLastQuantum = unchangedQuantum;
    EXPECT_FALSE(lane.CapturePrivatePlacedLastQuantum(handle, &placedLastQuantum));
    EXPECT_EQ(unchangedQuantum, placedLastQuantum);
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    EXPECT_EQ(64u, anchor);
    ASSERT_TRUE(lane.CapturePrivatePlacedLastQuantum(handle, &placedLastQuantum));
    EXPECT_EQ(3u, placedLastQuantum);
    EXPECT_FALSE(lane.Publish(handle));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));

    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(3, {0, 1, 0, 1}, &reused));
    ASSERT_EQ(handle.slot, reused.slot);
    ASSERT_NE(handle.generation, reused.generation);
    ASSERT_TRUE(lane.DeclareGroup(reused, group));
    ASSERT_TRUE(lane.Prepare(reused, nullptr, 0));
    ASSERT_TRUE(lane.TryPlace(reused, 64, 0, &anchor));
    ASSERT_TRUE(lane.CapturePrivatePlacedLastQuantum(reused, &placedLastQuantum));
    EXPECT_EQ(3u, placedLastQuantum);
    placedLastQuantum = unchangedQuantum;
    EXPECT_FALSE(lane.CapturePrivatePlacedLastQuantum(handle, &placedLastQuantum));
    EXPECT_EQ(unchangedQuantum, placedLastQuantum);
    ASSERT_TRUE(lane.Cancel(reused));
    ASSERT_TRUE(lane.Retire(reused));
}

TEST(ImmediateActionLane, DeclaredGroupsSharePerQuantumCapacityAcrossAttempts) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 2, 64, false, 1}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneDeclaredGroup group{};
    group.identity.absoluteQuantum = 2;
    group.identity.pageGeneration = 1;
    group.identity.decision = {0, 1};
    group.identity.committedRow = 0;
    group.identity.rowClaimGeneration = 1;
    group.identity.transportEpoch = 1;
    group.relativeFrame = 64;
    harken::ImmediateLaneHandle first{}, second{};
    ASSERT_TRUE(lane.Reserve(1, {1, 1, 0, 2}, &first));
    ASSERT_TRUE(lane.Reserve(2, {1, 1, 0, 2}, &second));
    ASSERT_TRUE(lane.DeclareGroup(first, group));
    group.identity.decision = {1, 1};
    group.identity.committedRow = 1;
    ASSERT_TRUE(lane.DeclareGroup(second, group));
    ASSERT_TRUE(lane.Prepare(first, &event, 1));
    ASSERT_TRUE(lane.Prepare(second, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(first, 64, 0, &anchor));
    EXPECT_FALSE(lane.TryPlace(second, 64, 0, &anchor));
    ASSERT_TRUE(lane.TryPlace(second, 64, 1, &anchor));
    EXPECT_EQ(128u, anchor);
    ASSERT_TRUE(lane.Cancel(first));
    ASSERT_TRUE(lane.Cancel(second));
    EXPECT_TRUE(lane.Retire(first));
    EXPECT_TRUE(lane.Retire(second));
}

TEST(ImmediateActionLane, StaleGenerationCannotClaimReusedBucket) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle old{};
    ASSERT_TRUE(lane.Reserve(1, 1, &old));
    ASSERT_TRUE(lane.Prepare(old, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(old, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(old));
    const auto* oldBucket = lane.CaptureDue(old.slot, 1);
    ASSERT_NE(nullptr, oldBucket);
    ASSERT_TRUE(lane.Claim(old, oldBucket));
    ASSERT_TRUE(lane.Finish(old.slot, oldBucket, true));
    ASSERT_TRUE(lane.Retire(old));

    harken::ImmediateLaneHandle current{};
    ASSERT_TRUE(lane.Reserve(2, 1, &current));
    ASSERT_EQ(old.slot, current.slot);
    ASSERT_NE(old.generation, current.generation);
    ASSERT_TRUE(lane.Prepare(current, &event, 1));
    ASSERT_TRUE(lane.TryPlace(current, 128, 0, &anchor));
    ASSERT_TRUE(lane.Publish(current));
    const auto* currentBucket = lane.CaptureDue(current.slot, 2);
    ASSERT_NE(nullptr, currentBucket);
    EXPECT_EQ(oldBucket, currentBucket);
    EXPECT_FALSE(lane.Claim(old, oldBucket));
    ASSERT_TRUE(lane.Claim(current, currentBucket));
    ASSERT_TRUE(lane.Finish(current.slot, currentBucket, true));
    EXPECT_TRUE(lane.Retire(current));
}

TEST(ImmediateActionLane, RejectsAnchorAndFutureEventFrameOverflow) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(64, &program, 1)};
    ASSERT_TRUE(lane.Prepare(handle, events, 2));

    std::uint64_t anchor = 123;
    const auto lastAligned =
        (std::numeric_limits<std::uint64_t>::max() / 64) * 64;
    EXPECT_FALSE(lane.TryPlace(handle, lastAligned, 0, &anchor));
    EXPECT_EQ(123u, anchor);
    EXPECT_FALSE(lane.TryPlace(handle,
        std::numeric_limits<std::uint64_t>::max(), 0, &anchor));
    EXPECT_EQ(123u, anchor);

    ASSERT_TRUE(lane.TryPlace(handle, lastAligned - 64, 0, &anchor));
    EXPECT_EQ(lastAligned - 64, anchor);
    ASSERT_TRUE(lane.Publish(handle));
    const auto* first = lane.CaptureDue(handle.slot, anchor / 64);
    ASSERT_NE(nullptr, first);
    ASSERT_EQ(1u, first->eventCount);
    EXPECT_EQ(anchor, lane.Events(handle.slot, first)[0].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, first));
    ASSERT_TRUE(lane.Finish(handle.slot, first, true));
    const auto* second = lane.CaptureDue(handle.slot, lastAligned / 64);
    ASSERT_NE(nullptr, second);
    ASSERT_EQ(1u, second->eventCount);
    EXPECT_EQ(lastAligned, lane.Events(handle.slot, second)[0].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, second));
    ASSERT_TRUE(lane.Finish(handle.slot, second, true));
    EXPECT_TRUE(lane.Retire(handle));
}

#if !defined(HARKEN_RT_QUALIFICATION)
TEST(ImmediateActionLane, ReservingAnotherAttemptCannotBlockDueAudioCapture) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle active{};
    ASSERT_TRUE(lane.Reserve(1, 1, &active));
    ASSERT_TRUE(lane.Prepare(active, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(active, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(active));
    struct Probe {
        harken::ImmediateActionLane* lane;
        std::uint32_t activeSlot;
        std::uint32_t calls = 0;
        bool captured = true;
    } probe{&lane, active.slot};
    lane.SetReserveGateHookForTest([](void* opaque, std::uint32_t slot) {
        auto& state = *static_cast<Probe*>(opaque);
        ++state.calls;
        const auto* due = state.lane->CaptureDue(state.activeSlot, 1);
        state.captured = state.captured && due != nullptr && slot != state.activeSlot;
        if (due != nullptr)
            state.captured = state.lane->ReleaseCapture(state.activeSlot, due) &&
                             state.captured;
    }, &probe);
    harken::ImmediateLaneHandle other{};
    ASSERT_TRUE(lane.Reserve(2, 1, &other));
    EXPECT_EQ(1u, probe.calls);
    EXPECT_TRUE(probe.captured);
    lane.SetReserveGateHookForTest(nullptr, nullptr);
    const auto* due = lane.CaptureDue(active.slot, 1);
    ASSERT_NE(nullptr, due);
    ASSERT_TRUE(lane.Claim(active, due));
    ASSERT_TRUE(lane.Finish(active.slot, due, true));
    EXPECT_TRUE(lane.Retire(active));
    ASSERT_TRUE(lane.Cancel(other));
    EXPECT_TRUE(lane.Retire(other));
}
#endif

TEST(ImmediateActionLane, AnchorSkipsFullBucketAndRejectsFullWindow) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({3, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle first{}, second{}, third{};
    ASSERT_TRUE(lane.Reserve(1, 1, &first));
    ASSERT_TRUE(lane.Reserve(2, 1, &second));
    ASSERT_TRUE(lane.Reserve(3, 1, &third));
    ASSERT_TRUE(lane.Prepare(first, &event, 1));
    ASSERT_TRUE(lane.Prepare(second, &event, 1));
    ASSERT_TRUE(lane.Prepare(third, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(first, 4 * 64, 1, &anchor));
    ASSERT_TRUE(lane.Publish(first));
    ASSERT_TRUE(lane.TryPlace(second, 4 * 64, 1, &anchor));
    EXPECT_EQ(5u * 64u, anchor);
    ASSERT_TRUE(lane.Publish(second));
    EXPECT_FALSE(lane.TryPlace(third, 4 * 64, 1, &anchor));
    ASSERT_TRUE(lane.Cancel(third));
    EXPECT_TRUE(lane.Retire(third));
}

TEST(ImmediateActionLane, PostCreditCanOutliveACancelledCohortMember) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    const harken::TimelineProgramView program{};
    const auto remaining = Event(0, &program, 1);
    ASSERT_TRUE(lane.Prepare(handle, &remaining, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    EXPECT_EQ(1u, bucket->eventCount);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    EXPECT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, CancellationAndClaimShareOneGate) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 1, &handle));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.Cancel(handle));
    EXPECT_FALSE(lane.Retire(handle));
    EXPECT_FALSE(lane.Claim(handle, bucket));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 1));
    EXPECT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, CancellationAfterCaptureReleasesGateAndRetiresFutureBuckets) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 8, 1, 64}));
    const harken::TimelineProgramView program{};
    std::array<harken::ImmediateLaneEvent, 8> events{};
    for (std::uint32_t index = 0; index < events.size(); ++index)
        events[index] = Event(index * 64u, &program, index);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, events.size(), &handle));
    ASSERT_TRUE(lane.Prepare(handle, events.data(), events.size()));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(lane.Observe(handle, &observation));
    EXPECT_TRUE(observation.complete);
    EXPECT_FALSE(observation.applied);
    ASSERT_TRUE(lane.Retire(handle));
    EXPECT_FALSE(lane.Observe(handle, &observation));
}

#if !defined(HARKEN_RT_QUALIFICATION)
TEST(ImmediateActionLane, FinishCannotMoveTerminalHeadBackAfterCancellation) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(64, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    struct Probe {
        harken::ImmediateActionLane* lane;
        harken::ImmediateLaneHandle handle;
        bool cancelled = false;
    } probe{&lane, handle};
    lane.SetFinishHeadHookForTest([](void* context) {
        auto& state = *static_cast<Probe*>(context);
        state.cancelled = state.lane->Cancel(state.handle);
    }, &probe);
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    lane.SetFinishHeadHookForTest(nullptr, nullptr);
    ASSERT_TRUE(probe.cancelled);
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(lane.Observe(handle, &observation));
    EXPECT_TRUE(observation.applied);
    EXPECT_TRUE(observation.complete);
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 2));
    EXPECT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, ReservedAttemptRetirementRetriesAfterAudioReaderGate) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, 1, &handle));
    struct Probe {
        harken::ImmediateActionLane* lane;
        harken::ImmediateLaneHandle handle;
        bool cancelSucceeded = false;
        bool retiredWhilePinned = true;
    } probe{&lane, handle};
    lane.SetCaptureGateHookForTest([](void* context) {
        auto& state = *static_cast<Probe*>(context);
        state.cancelSucceeded = state.lane->Cancel(state.handle);
        state.retiredWhilePinned = state.lane->Retire(state.handle);
    }, &probe);
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 1));
    lane.SetCaptureGateHookForTest(nullptr, nullptr);
    EXPECT_FALSE(probe.cancelSucceeded);
    EXPECT_FALSE(probe.retiredWhilePinned);
    ASSERT_TRUE(lane.OwnsPrivate(handle, 17));
    ASSERT_TRUE(lane.Cancel(handle));
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(lane.Observe(handle, &observation));
    EXPECT_TRUE(observation.complete);
    ASSERT_TRUE(lane.Retire(handle));
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(18, 1, &reused));
    EXPECT_EQ(handle.slot, reused.slot);
    EXPECT_NE(handle.generation, reused.generation);
    ASSERT_TRUE(lane.Cancel(reused));
    EXPECT_TRUE(lane.Retire(reused));
}
#endif

TEST(ImmediateActionLane, PrivateCalendarReservationExcludesAnotherAnchor) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle first{}, second{};
    ASSERT_TRUE(lane.Reserve(1, 1, &first));
    ASSERT_TRUE(lane.Reserve(2, 1, &second));
    ASSERT_TRUE(lane.Prepare(first, &event, 1));
    ASSERT_TRUE(lane.Prepare(second, &event, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(first, 64, 1, &anchor));
    ASSERT_TRUE(lane.TryPlace(second, 64, 1, &anchor));
    EXPECT_EQ(128u, anchor);
    ASSERT_TRUE(lane.Cancel(first));
    EXPECT_TRUE(lane.Retire(first));
    EXPECT_TRUE(lane.TryPlace(second, 64, 0, &anchor));
    EXPECT_EQ(64u, anchor);
}

TEST(ImmediateActionLane, MissedFirstBucketCancelsFutureWithoutReplay) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(128, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 2));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 3));
    EXPECT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, FailedClaimCannotExposeLaterBucket) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(128, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* first = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, first);
    ASSERT_TRUE(lane.Claim(handle, first));
    ASSERT_TRUE(lane.Finish(handle.slot, first, false));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 3));
    EXPECT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PinnedArmClaimExcludesCancellationUntilStart) {
    harken::TimelineArmTable arms(1);
    harken::TimelineArmToken token{};
    const harken::TimelineSemanticTarget target{};
    ASSERT_TRUE(arms.PreparePendingImmediate(0, 1, 1, target, &token));
    ASSERT_TRUE(arms.CommitPrepared(token));
    ASSERT_TRUE(arms.AcknowledgeApplied(token, 1));
    ASSERT_TRUE(arms.BindPendingImmediateStart(token, 64, 64));
    harken::TimelineArmReaderPin pin{};
    ASSERT_TRUE(arms.PinFrontierReader(token, &pin));
    ASSERT_TRUE(arms.Publish(token));
    harken::TimelineArmTable::LaneAudioProof proof{};
    EXPECT_FALSE(arms.ProveLaneStart(token, 64, 2, &proof, 2, &target));
    auto staleTarget = target;
    staleTarget.occurrenceOrdinal = 42;
    EXPECT_FALSE(arms.ProveLaneStart(token, 64, 2, &proof, 1, &staleTarget));
    ASSERT_TRUE(arms.ProveLaneStart(token, 64, 2, &proof, 1, &target));
    EXPECT_FALSE(arms.RequestCancel(token));
    harken::TimelineArmTable::ApplyLaneStart(proof);
    harken::ArmState state = harken::ArmState::Free;
    ASSERT_TRUE(arms.State(token, &state));
    EXPECT_EQ(harken::ArmState::Started, state);
    EXPECT_TRUE(pin.Release(3));
}

TEST(ImmediateActionLane, CapturedValidationRefusalKeepsCaptureAndPublicationFences) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[] = {
        Event(0, &program, 0), Event(128, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    harken::ImmediateLaneBucket foreign{};
    foreign.attemptGeneration = handle.generation;
    EXPECT_FALSE(lane.RejectCapturedBucket(handle, &foreign));
    EXPECT_FALSE(lane.RejectCapturedBucket(
        {handle.slot, handle.generation + 1u}, bucket));
    ASSERT_EQ(harken::ImmediateLaneBucketState::Committed,
              bucket->state.load(std::memory_order_acquire));
    ASSERT_TRUE(lane.RejectCapturedBucket(handle, bucket));
    harken::ImmediateLaneObservation actual{};
    ASSERT_TRUE(lane.Observe(handle, &actual));
    EXPECT_TRUE(actual.complete);
    EXPECT_TRUE(actual.missed);
    EXPECT_TRUE(actual.protocolRefused);
    EXPECT_FALSE(actual.claimed);
    EXPECT_FALSE(actual.applied);
    EXPECT_FALSE(actual.cancelled);
    harken::ImmediateLaneValidationRefusalSnapshot refused{};
    EXPECT_FALSE(lane.CapturePublishedValidationRefusal(handle, 2, &refused));
    EXPECT_FALSE(lane.Retire(handle, 2));
    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
    EXPECT_FALSE(lane.CapturePublishedValidationRefusal(handle, 1, &refused));
    EXPECT_FALSE(lane.CapturePublishedValidationRefusal(
        {handle.slot, handle.generation + 1u}, 2, &refused));
    ASSERT_TRUE(lane.CapturePublishedValidationRefusal(handle, 2, &refused));
    EXPECT_EQ(1u, refused.root);
    EXPECT_EQ(0u, refused.bucketOrdinal);
    EXPECT_EQ(2u, refused.audioSequence);
    EXPECT_EQ(0u, refused.groupCount);
    ASSERT_TRUE(lane.Retire(handle, 2));
}

TEST(ImmediateActionLane, CapturedValidationRefusalPreservesCancellationClaimAndCapacity) {
    for (unsigned mode = 0; mode < 3u; ++mode) {
        SCOPED_TRACE(mode);
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
        const harken::TimelineProgramView program{};
        const auto event = Event(0, &program, 0);
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(1, 1, &handle));
        ASSERT_TRUE(lane.Prepare(handle, &event, 1));
        std::uint64_t anchor = 0;
        ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
        ASSERT_TRUE(lane.Publish(handle));
        const auto* bucket = lane.CaptureDue(handle.slot, 1);
        ASSERT_NE(nullptr, bucket);
        if (mode == 0u) {
            ASSERT_TRUE(lane.Cancel(handle));
            EXPECT_FALSE(lane.RejectCapturedBucket(handle, bucket));
            ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
        } else if (mode == 1u) {
            ASSERT_TRUE(lane.Claim(handle, bucket));
            EXPECT_FALSE(lane.RejectCapturedBucket(handle, bucket));
            ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
            EXPECT_FALSE(lane.RejectCapturedBucket(handle, bucket));
        } else {
            ASSERT_TRUE(lane.RecordAdmissionFailure(handle.slot, bucket,
                harken::ImmediateLaneAdmissionFailure::ArenaCapacity));
            ASSERT_TRUE(lane.RejectCapturedBucket(handle, bucket));
            ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
        }
        harken::ImmediateLaneObservation actual{};
        ASSERT_TRUE(lane.Observe(handle, &actual));
        EXPECT_TRUE(actual.complete);
        EXPECT_FALSE(actual.protocolRefused);
        EXPECT_EQ(mode == 0u, actual.cancelled);
        EXPECT_EQ(mode == 1u, actual.applied);
        EXPECT_EQ(mode == 2u, actual.missed);
        EXPECT_EQ(mode == 2u, actual.arenaCapacityMissed);
        ASSERT_TRUE(lane.Retire(handle, 2));
    }
}

TEST(ImmediateActionLane, ReservedMemberScopeRequiresExactActualDeclarationsAndOriginalCredit) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 4, 4, 64}));
    const harken::ImmediateLaneDemand demand{2, 2, 2, 1};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, demand, &handle));
    std::array<harken::ImmediateLaneMemberManifest, 2> members{};
    for (std::uint32_t index = 0; index < members.size(); ++index) {
        auto& member = members[index];
        member.group.absoluteQuantum = 1;
        member.group.pageGeneration = 1;
        member.group.decision = {index, 1};
        member.group.committedRow = index;
        member.group.rowClaimGeneration = 1;
        member.group.transportEpoch = 1;
        member.arm = {index, 1, index, 1};
        member.programGeneration = 1;
        member.ownerInstance = index + 1;
        member.ownerEpoch = 1;
    }
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    ASSERT_TRUE(lane.DeclareMember(handle, members[0]));
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    ASSERT_TRUE(lane.DeclareMember(handle, members[1]));
    EXPECT_TRUE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    EXPECT_FALSE(lane.DeclareMember(handle, members[1]));
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 18, demand, members.data(), 2));
    EXPECT_FALSE(lane.MatchesReservedMembers({handle.slot, handle.generation + 1}, 17,
        demand, members.data(), 2));
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, nullptr, 2));
    for (unsigned field = 0; field < 4; ++field) {
        auto wrong = demand;
        if (field == 0) ++wrong.events;
        if (field == 1) ++wrong.groups;
        if (field == 2) ++wrong.members;
        if (field == 3) ++wrong.buckets;
        EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, wrong, members.data(), 2));
    }
    for (unsigned field = 0; field < 18; ++field) {
        auto wrong = members;
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
        EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, wrong.data(), 2));
        EXPECT_TRUE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    }
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(17, demand, &reused));
    ASSERT_EQ(handle.slot, reused.slot);
    ASSERT_NE(handle.generation, reused.generation);
    EXPECT_FALSE(lane.MatchesReservedMembers(handle, 17, demand, members.data(), 2));
    EXPECT_FALSE(lane.MatchesReservedMembers(reused, 17, demand, members.data(), 2));
    ASSERT_TRUE(lane.Cancel(reused));
    EXPECT_TRUE(lane.Retire(reused));
}


harken::ImmediateLaneEvent FixedEvent(std::uint64_t frame,
    const harken::TimelineProgramView* program, std::uint32_t ordinal) {
    auto event = Event(0, program, ordinal);
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = frame;
    return event;
}

TEST(ImmediateActionLane, MixedTimeRealCapacitySlideReversesDueOrderWithoutMovingFixedFrame) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneHandle blocker{}, mixed{};
    ASSERT_TRUE(lane.Reserve(1, 1, &blocker));
    const auto pressure = Event(0, &program, 9);
    ASSERT_TRUE(lane.Prepare(blocker, &pressure, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(blocker, 192, 0, &anchor));
    ASSERT_TRUE(lane.Publish(blocker));
    ASSERT_TRUE(lane.Reserve(2, 2, &mixed));
    const harken::ImmediateLaneEvent events[]{Event(128, &program, 1), FixedEvent(256, &program, 0)};
    ASSERT_TRUE(lane.Prepare(mixed, events, 2));
    ASSERT_TRUE(lane.TryPlace(mixed, 64, 2, &anchor));
    EXPECT_EQ(192u, anchor);
    ASSERT_TRUE(lane.Publish(mixed));
    const auto* fixed = lane.CaptureDue(mixed.slot, 4);
    ASSERT_NE(nullptr, fixed);
    ASSERT_EQ(1u, fixed->eventCount);
    EXPECT_EQ(0u, lane.Events(mixed.slot, fixed)[0].event.executionOrdinal);
    EXPECT_EQ(256u, lane.Events(mixed.slot, fixed)[0].event.executeFrame);
    ASSERT_TRUE(lane.Claim(mixed, fixed));
    ASSERT_TRUE(lane.Finish(mixed.slot, fixed, true));
    const auto* relative = lane.CaptureDue(mixed.slot, 5);
    ASSERT_NE(nullptr, relative);
    EXPECT_EQ(1u, lane.Events(mixed.slot, relative)[0].event.executionOrdinal);
    EXPECT_EQ(320u, lane.Events(mixed.slot, relative)[0].event.executeFrame);
    ASSERT_TRUE(lane.Claim(mixed, relative));
    ASSERT_TRUE(lane.Finish(mixed.slot, relative, true));
    ASSERT_TRUE(lane.Retire(mixed));
    ASSERT_TRUE(lane.Cancel(blocker));
    ASSERT_TRUE(lane.Retire(blocker));
}

TEST(ImmediateActionLane, MixedTimeFixedBucketCanPrecedeSlidAnchorAtOriginalSafeQuantum) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneHandle blocker{}, mixed{};
    ASSERT_TRUE(lane.Reserve(1, 2, &blocker));
    const harken::ImmediateLaneEvent pressure[]{Event(0, &program, 8), Event(128, &program, 9)};
    ASSERT_TRUE(lane.Prepare(blocker, pressure, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(blocker, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(blocker));
    ASSERT_TRUE(lane.Reserve(2, 2, &mixed));
    const harken::ImmediateLaneEvent events[]{Event(0, &program, 1), FixedEvent(128, &program, 0)};
    ASSERT_TRUE(lane.Prepare(mixed, events, 2));
    ASSERT_TRUE(lane.TryPlace(mixed, 64, 3, &anchor));
    EXPECT_EQ(256u, anchor);
    ASSERT_TRUE(lane.Publish(mixed));
    for (const auto quantum : {2u, 4u}) {
        const auto* bucket = lane.CaptureDue(mixed.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        EXPECT_EQ(quantum * 64u, lane.Events(mixed.slot, bucket)[0].event.executeFrame);
        ASSERT_TRUE(lane.Claim(mixed, bucket));
        ASSERT_TRUE(lane.Finish(mixed.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(mixed));
    ASSERT_TRUE(lane.Cancel(blocker));
    ASSERT_TRUE(lane.Retire(blocker));
}

TEST(ImmediateActionLane, MixedTimeEqualDueUsesAuthoredOrderAndPreservesSemanticOrdinal) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 3, 3, 64}));
    const harken::TimelineProgramView program{};
    auto relative = Event(72, &program, 1);
    relative.event.frontier.entryEventOrdinal = 11;
    auto fixed = FixedEvent(200, &program, 0);
    fixed.event.frontier.entryEventOrdinal = 12;
    const harken::ImmediateLaneEvent events[]{relative, fixed};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 128, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 3);
    ASSERT_NE(nullptr, bucket);
    ASSERT_EQ(2u, bucket->eventCount);
    const auto* actual = lane.Events(handle.slot, bucket);
    EXPECT_EQ(0u, actual[0].event.executionOrdinal);
    EXPECT_EQ(1u, actual[1].event.executionOrdinal);
    EXPECT_EQ(12u, actual[0].event.frontier.entryEventOrdinal);
    EXPECT_EQ(11u, actual[1].event.frontier.entryEventOrdinal);
    EXPECT_EQ(200u, actual[0].event.executeFrame);
    EXPECT_EQ(200u, actual[1].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, MixedTimeUnalignedSafeBoundaryRejectsOldFixedQuantum) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 100}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[]{Event(0, &program, 0), FixedEvent(150, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 777;
    EXPECT_FALSE(lane.TryPlace(handle, 130, 8, &anchor));
    EXPECT_EQ(777u, anchor);
    EXPECT_FALSE(lane.Publish(handle));
    ASSERT_TRUE(lane.TryPlace(handle, 100, 0, &anchor));
    EXPECT_EQ(100u, anchor);
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_EQ(2u, bucket->eventCount);
    EXPECT_EQ(150u, lane.Events(handle.slot, bucket)[1].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, MixedTimeBucketCreditRefusalPreservesPreviousPreparedLayout) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[]{Event(0, &program, 0), FixedEvent(128, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, {2, 0, 0, 1}, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 128, 0, &anchor));
    anchor = 777;
    EXPECT_FALSE(lane.TryPlace(handle, 64, 0, &anchor));
    EXPECT_EQ(777u, anchor);
    ASSERT_TRUE(lane.Publish(handle));
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 1));
    const auto* original = lane.CaptureDue(handle.slot, 2);
    ASSERT_NE(nullptr, original);
    ASSERT_EQ(2u, original->eventCount);
    EXPECT_EQ(128u, lane.Events(handle.slot, original)[0].event.executeFrame);
    EXPECT_EQ(128u, lane.Events(handle.slot, original)[1].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, original));
    ASSERT_TRUE(lane.Finish(handle.slot, original, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, MixedTimeOverflowAndInvalidDomainRefuseWithoutPlacement) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, 1, &handle));
    auto invalid = FixedEvent(UINT64_MAX, &program, 0);
    EXPECT_FALSE(lane.Prepare(handle, &invalid, 1));
    invalid = Event(0, &program, 0);
    invalid.timeDomain = static_cast<harken::ImmediateLaneTimeDomain>(255);
    EXPECT_FALSE(lane.Prepare(handle, &invalid, 1));
    invalid = FixedEvent(128, &program, 0);
    invalid.relativeFrame = 64;
    EXPECT_FALSE(lane.Prepare(handle, &invalid, 1));
    const auto relative = Event(64, &program, 0);
    ASSERT_TRUE(lane.Prepare(handle, &relative, 1));
    std::uint64_t anchor = 777;
    EXPECT_FALSE(lane.TryPlace(handle, UINT64_MAX - 63u, 0, &anchor));
    EXPECT_EQ(777u, anchor);
    EXPECT_FALSE(lane.Publish(handle));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, MixedTimeRepeatedPermutationKeepsRealArmPinsAndExactStarts) {
    harken::TimelineArmTable arms(2);
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 1, 64}));
    harken::TimelineSemanticTarget target{};
    harken::TimelineArmToken relativeArm{}, fixedArm{};
    ASSERT_TRUE(arms.Prepare(0, 1, 256, 256, 1, target, &relativeArm));
    ASSERT_TRUE(arms.Prepare(1, 1, 128, 128, 2, target, &fixedArm));
    ASSERT_TRUE(arms.CommitPrepared(relativeArm));
    ASSERT_TRUE(arms.CommitPrepared(fixedArm));
    ASSERT_TRUE(arms.AcknowledgeApplied(relativeArm, 1));
    ASSERT_TRUE(arms.AcknowledgeApplied(fixedArm, 1));
    harken::TimelineProgramView relativeProgram{}, fixedProgram{};
    relativeProgram.generation = 1;
    fixedProgram.generation = 2;
    auto relative = Event(0, &relativeProgram, 1);
    relative.event.program.programGeneration = 1;
    relative.event.armToken = relativeArm;
    auto fixed = FixedEvent(128, &fixedProgram, 0);
    fixed.event.program.programGeneration = 2;
    fixed.event.armToken = fixedArm;
    const harken::ImmediateLaneEvent events[]{relative, fixed};
    harken::ImmediateLaneHandle handle{}, blocker{};
    ASSERT_TRUE(lane.Reserve(1, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    EXPECT_EQ(1u, arms.Resolve(relativeArm)->frontierReaderPins.load());
    EXPECT_EQ(1u, arms.Resolve(fixedArm)->frontierReaderPins.load());
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 0, 0, &anchor));
    ASSERT_TRUE(lane.Reserve(2, 2, &blocker));
    const harken::ImmediateLaneEvent pressure[]{Event(0, &relativeProgram, 8), Event(128, &relativeProgram, 9)};
    ASSERT_TRUE(lane.Prepare(blocker, pressure, 2));
    ASSERT_TRUE(lane.TryPlace(blocker, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(blocker));
    ASSERT_TRUE(lane.TryPlace(handle, 64, 3, &anchor));
    EXPECT_EQ(256u, anchor);
    ASSERT_TRUE(lane.TryPlace(handle, 64, 3, &anchor));
    anchor = 777;
    EXPECT_FALSE(lane.TryPlace(handle, 129, 4, &anchor));
    EXPECT_EQ(777u, anchor);
    EXPECT_EQ(1u, arms.Resolve(relativeArm)->frontierReaderPins.load());
    EXPECT_EQ(1u, arms.Resolve(fixedArm)->frontierReaderPins.load());
    ASSERT_TRUE(arms.Publish(relativeArm));
    ASSERT_TRUE(arms.Publish(fixedArm));
    ASSERT_TRUE(lane.Publish(handle));
    for (const auto quantum : {2u, 4u}) {
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        const auto* actual = lane.Events(handle.slot, bucket);
        const auto expectedArm = quantum == 2u ? fixedArm : relativeArm;
        EXPECT_EQ(expectedArm.slot, actual[0].event.armToken.slot);
        EXPECT_EQ(expectedArm.generation, actual[0].event.armToken.generation);
        EXPECT_EQ(expectedArm.renderSlot, actual[0].event.armToken.renderSlot);
        EXPECT_EQ(expectedArm.renderGeneration, actual[0].event.armToken.renderGeneration);
        harken::TimelineArmTable::LaneAudioProof proof{};
        ASSERT_TRUE(arms.ProveLaneStart(actual[0].event.armToken, actual[0].event.executeFrame,
            quantum + 1u, &proof, actual[0].armProgramGeneration, &target));
        ASSERT_TRUE(lane.Claim(handle, bucket));
        harken::TimelineArmTable::ApplyLaneStart(proof);
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(handle, 8));
    EXPECT_EQ(0u, arms.Resolve(relativeArm)->frontierReaderPins.load());
    EXPECT_EQ(0u, arms.Resolve(fixedArm)->frontierReaderPins.load());
    ASSERT_TRUE(lane.Cancel(blocker));
    ASSERT_TRUE(lane.Retire(blocker));
}

TEST(ImmediateActionLane, PrivatePlainLayoutRepeatedShapesKeepLiteralFramesBeforeAnchor) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneHandle handle{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit;
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    EXPECT_FALSE(edit.SealCandidate(&ready));
    const harken::ImmediateLaneEvent two[]{FixedEvent(128, &program, 0), FixedEvent(192, &program, 1)};
    ASSERT_TRUE(edit.StageCandidate(two, 2, 64, 64));
    const auto one = FixedEvent(192, &program, 1);
    ASSERT_TRUE(edit.StageCandidate(&one, 1, 256, 64));
    ASSERT_TRUE(edit.StageCandidate(two, 2, 256, 64));
    ASSERT_TRUE(edit.SealCandidate(&ready));
    EXPECT_FALSE(edit.Valid());
    EXPECT_TRUE(ready.Valid());
    EXPECT_FALSE(lane.Publish(handle));
    ready.Consume();
    EXPECT_FALSE(ready.Valid());
    ready.Consume();
    ASSERT_TRUE(lane.Publish(handle));
    for (const auto quantum : {2u, 3u}) {
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        EXPECT_EQ(quantum * 64u, lane.Events(handle.slot, bucket)[0].event.executeFrame);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 4));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PrivatePlainLayoutAbortRestoresOriginalPlacedEventsAndCapacity) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    const harken::ImmediateLaneEvent original[]{FixedEvent(64, &program, 8), FixedEvent(128, &program, 9)};
    harken::ImmediateLaneHandle handle{}, competitor{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Prepare(handle, original, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Reserve(2, 1, &competitor));
    const auto pressure = FixedEvent(64, &program, 7);
    ASSERT_TRUE(lane.Prepare(competitor, &pressure, 1));
    {
        harken::PrivatePlainLayoutEdit first;
        ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &first));
        first.ClearCandidate();
        EXPECT_FALSE(lane.TryPlace(competitor, 64, 0, &anchor));
        harken::PrivatePlainLayoutEdit moved(std::move(first));
        EXPECT_FALSE(first.Valid());
        const auto changed = FixedEvent(256, &program, 0);
        ASSERT_TRUE(moved.StageCandidate(&changed, 1, 192, 64));
        EXPECT_FALSE(lane.TryPlace(competitor, 64, 0, &anchor));
    }
    ASSERT_TRUE(lane.Publish(handle));
    for (const auto quantum : {1u, 2u}) {
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        const auto* event = lane.Events(handle.slot, bucket);
        EXPECT_EQ(quantum * 64u, event->event.executeFrame);
        EXPECT_EQ(quantum + 7u, event->event.executionOrdinal);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(handle));
    ASSERT_TRUE(lane.TryPlace(competitor, 64, 0, &anchor));
    ASSERT_TRUE(lane.Cancel(competitor));
    ASSERT_TRUE(lane.Retire(competitor));
}

TEST(ImmediateActionLane, PrivatePlainLayoutEscrowCountsUnionWithoutDoubleCharging) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({3, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    harken::ImmediateLaneHandle handle{}, peer{}, third{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    const auto one = FixedEvent(64, &program, 0);
    ASSERT_TRUE(lane.Prepare(handle, &one, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Reserve(2, 1, &peer));
    ASSERT_TRUE(lane.Prepare(peer, &one, 1));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(&one, 1, 64, 64));
    ASSERT_TRUE(lane.TryPlace(peer, 64, 0, &anchor));
    const harken::ImmediateLaneEvent two[]{one, FixedEvent(65, &program, 1)};
    EXPECT_FALSE(edit.StageCandidate(two, 2, 64, 64));
    harken::PrivatePlainLayoutCommitProof refused;
    EXPECT_FALSE(edit.SealCandidate(&refused));
    ASSERT_TRUE(lane.Reserve(3, 1, &third));
    ASSERT_TRUE(lane.Prepare(third, &one, 1));
    EXPECT_FALSE(lane.TryPlace(third, 64, 0, &anchor));
    edit.Abort();
    EXPECT_FALSE(lane.TryPlace(third, 64, 0, &anchor));
    for (auto h : {handle, peer, third}) {
        ASSERT_TRUE(lane.Cancel(h));
        ASSERT_TRUE(lane.Retire(h));
    }
}

TEST(ImmediateActionLane, PrivatePlainLayoutLargerOriginalEscrowSurvivesSmallerTentative) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    const auto one = FixedEvent(64, &program, 0);
    const harken::ImmediateLaneEvent two[]{one, FixedEvent(65, &program, 1)};
    harken::ImmediateLaneHandle handle{}, peer{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Prepare(handle, two, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Reserve(2, 1, &peer));
    ASSERT_TRUE(lane.Prepare(peer, &one, 1));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(&one, 1, 64, 64));
    EXPECT_FALSE(lane.TryPlace(peer, 64, 0, &anchor));
    edit.ClearCandidate();
    EXPECT_FALSE(lane.TryPlace(peer, 64, 0, &anchor));
    ASSERT_TRUE(edit.StageCandidate(&one, 1, 64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.TryPlace(peer, 64, 0, &anchor));
    for (auto h : {handle, peer}) {
        ASSERT_TRUE(lane.Cancel(h));
        ASSERT_TRUE(lane.Retire(h));
    }
}

TEST(ImmediateActionLane, PrivatePlainLayoutFailedStageClearsReadyAndTrailingCapacity) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({3, 2, 1, 64}));
    const harken::TimelineProgramView program{};
    harken::ImmediateLaneHandle handle{}, blocker{}, follower{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Reserve(2, 1, &blocker));
    const auto busy = FixedEvent(256, &program, 0);
    ASSERT_TRUE(lane.Prepare(blocker, &busy, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(blocker, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(blocker));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    const harken::ImmediateLaneEvent two[]{FixedEvent(128, &program, 0), FixedEvent(192, &program, 1)};
    ASSERT_TRUE(edit.StageCandidate(two, 2, 64, 64));
    EXPECT_FALSE(edit.StageCandidate(&busy, 1, 64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    EXPECT_FALSE(edit.SealCandidate(&ready));
    ASSERT_TRUE(lane.Reserve(3, 1, &follower));
    ASSERT_TRUE(lane.Prepare(follower, &two[1], 1));
    ASSERT_TRUE(lane.TryPlace(follower, 64, 0, &anchor));
    auto invalid = FixedEvent(UINT64_MAX, &program, 0);
    EXPECT_FALSE(edit.StageCandidate(&invalid, 1, 64, 64));
    EXPECT_FALSE(edit.StageCandidate(nullptr, 1, 64, 64));
    EXPECT_FALSE(edit.StageCandidate(two, 0, 64, 64));
    EXPECT_FALSE(edit.StageCandidate(two, 2, UINT64_MAX, 64));
    EXPECT_FALSE(edit.StageCandidate(two, 2, 192, 65));
    invalid = Event(0, &program, 0);
    EXPECT_FALSE(edit.StageCandidate(&invalid, 1, 64, 64));
    EXPECT_FALSE(edit.SealCandidate(&ready));
    edit.Abort();
    ASSERT_TRUE(lane.Prepare(handle, &two[0], 1));
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    for (auto h : {handle, blocker, follower}) {
        ASSERT_TRUE(lane.Cancel(h));
        ASSERT_TRUE(lane.Retire(h));
    }
}

TEST(ImmediateActionLane, PrivatePlainLayoutHeldExcludesPrivateMutationAndAudioCapture) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto event = FixedEvent(64, &program, 0);
    harken::ImmediateLaneHandle handle{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(&event, 1, 64, 64));
    harken::TimelineArmTable arms(1);
    EXPECT_FALSE(lane.Prepare(handle, &event, 1));
    std::uint64_t anchor = 777;
    EXPECT_FALSE(lane.TryPlace(handle, 64, 0, &anchor));
    EXPECT_EQ(777u, anchor);
    EXPECT_FALSE(lane.PinArms(handle, arms));
    EXPECT_FALSE(lane.Publish(handle));
    EXPECT_FALSE(lane.Cancel(handle));
    EXPECT_FALSE(lane.Retire(handle));
    EXPECT_TRUE(lane.HasProgramReference(9));
    const harken::ImmediateLaneBucket* captured = nullptr;
    bool due = true;
    std::thread audio([&] { captured = lane.CaptureDue(handle.slot, 1); due = lane.HasDue(1); });
    audio.join();
    EXPECT_EQ(nullptr, captured);
    EXPECT_FALSE(due);
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(edit.SealCandidate(&ready));
    EXPECT_FALSE(lane.Cancel(handle));
    ready.Consume();
    ASSERT_TRUE(lane.Publish(handle));
    const auto* actual = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, actual);
    ASSERT_TRUE(lane.Claim(handle, actual));
    EXPECT_FALSE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Finish(handle.slot, actual, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PrivatePlainLayoutExactIssuedIdentityAndCreditCannotBeReplayed) {
    harken::ImmediateActionLane lane, other;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    ASSERT_TRUE(other.Reset({1, 2, 2, 64}));
    harken::ImmediateLaneHandle handle{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit, conflict;
    EXPECT_FALSE(other.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 2, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit({handle.slot, handle.generation + 1}, 1, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, {1, 0, 0, 2}, &edit));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, {2, 0, 0, 1}, &edit));
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &conflict));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    edit.Abort();
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    harken::ImmediateLaneHandle next{};
    ASSERT_TRUE(lane.Reserve(2, demand, &next));
    ASSERT_EQ(handle.slot, next.slot);
    ASSERT_NE(handle.generation, next.generation);
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(next, 2, demand, &edit));
    edit.Abort();
    ASSERT_TRUE(lane.Cancel(next));
    ASSERT_TRUE(lane.Retire(next));
}

TEST(ImmediateActionLane, PrivatePlainLayoutReadyMoveResetAndOwnerFirstDestructionAreSafe) {
    static_assert(!std::is_move_constructible_v<harken::ImmediateActionLane>);
    static_assert(!std::is_copy_constructible_v<harken::PrivatePlainLayoutEdit>);
    harken::PrivatePlainLayoutCommitProof survivor;
    const harken::TimelineProgramView program{};
    const auto event = FixedEvent(64, &program, 0);
    {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
        harken::ImmediateLaneHandle handle{};
        const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
        ASSERT_TRUE(lane.Reserve(1, demand, &handle));
        harken::PrivatePlainLayoutEdit edit;
        ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
        EXPECT_FALSE(lane.Reset({3, 1, 1, 128}));
        EXPECT_TRUE(lane.Valid());
        EXPECT_EQ(1u, lane.AttemptCapacity());
        ASSERT_TRUE(edit.StageCandidate(&event, 1, 64, 64));
        harken::PrivatePlainLayoutCommitProof ready;
        ASSERT_TRUE(edit.SealCandidate(&ready));
        survivor = std::move(ready);
        EXPECT_FALSE(ready.Valid());
        EXPECT_TRUE(survivor.Valid());
        EXPECT_FALSE(lane.Reset({}));
    }
    EXPECT_FALSE(survivor.Valid());
    survivor.Consume();
    harken::PrivatePlainLayoutEdit survivingEdit;
    {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(1, {2, 0, 0, 2}, &handle));
        ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, {2, 0, 0, 2}, &survivingEdit));
    }
    EXPECT_FALSE(survivingEdit.Valid());
    survivingEdit.Abort();
}

TEST(ImmediateActionLane, PrivatePlainLayoutRefusesRealArmPinsAndGroupedReservation) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 2, 64}));
    harken::TimelineArmTable arms(1);
    harken::TimelineArmToken arm{};
    const harken::TimelineSemanticTarget target{};
    ASSERT_TRUE(arms.Prepare(0, 1, 64, 64, 1, target, &arm));
    ASSERT_TRUE(arms.CommitPrepared(arm));
    ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
    harken::TimelineProgramView program{};
    program.generation = 1;
    auto event = FixedEvent(64, &program, 0);
    event.event.program.programGeneration = 1;
    event.event.armToken = arm;
    harken::ImmediateLaneHandle pinned{}, grouped{};
    ASSERT_TRUE(lane.Reserve(1, {2, 0, 0, 2}, &pinned));
    ASSERT_TRUE(lane.Prepare(pinned, &event, 1));
    ASSERT_TRUE(lane.PinArms(pinned, arms));
    harken::PrivatePlainLayoutEdit edit;
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(pinned, 1, {2, 0, 0, 2}, &edit));
    EXPECT_EQ(1u, arms.Resolve(arm)->frontierReaderPins.load());
    ASSERT_TRUE(lane.Reserve(2, {2, 1, 1, 2}, &grouped));
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(grouped, 2, {2, 1, 1, 2}, &edit));
    ASSERT_TRUE(lane.Cancel(pinned));
    ASSERT_TRUE(lane.Retire(pinned, 8));
    EXPECT_EQ(0u, arms.Resolve(arm)->frontierReaderPins.load());
    ASSERT_TRUE(lane.Cancel(grouped));
    ASSERT_TRUE(lane.Retire(grouped));
}

TEST(ImmediateActionLane, PrivatePlainLayoutMoveAssignmentAbortsPriorActualReservation) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto event = FixedEvent(64, &program, 0);
    harken::ImmediateLaneHandle first{}, second{};
    const harken::ImmediateLaneDemand demand{2, 0, 0, 2};
    ASSERT_TRUE(lane.Reserve(1, demand, &first));
    ASSERT_TRUE(lane.Reserve(2, demand, &second));
    harken::PrivatePlainLayoutEdit a, b;
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(first, 1, demand, &a));
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(second, 2, demand, &b));
    ASSERT_TRUE(a.StageCandidate(&event, 1, 64, 64));
    b = std::move(a);
    EXPECT_FALSE(a.Valid());
    EXPECT_TRUE(b.Valid());
    ASSERT_TRUE(lane.Prepare(second, &event, 1));
    b.Abort();
    ASSERT_TRUE(lane.Prepare(first, &event, 1));
    for (auto h : {first, second}) {
        ASSERT_TRUE(lane.Cancel(h));
        ASSERT_TRUE(lane.Retire(h));
    }
}

harken::PlainAuthoredControlLayoutInput AuthoredLayout(
    const harken::TimelineProgramView* program) {
    harken::PlainAuthoredControlLayoutInput input{};
    input.start = Event(0, program, 10);
    input.start.event.rowSequence = 71;
    input.envelope = FixedEvent(128, program, 20);
    input.envelope.event.subeventPhase = 30;
    input.envelope.event.payload = 72;
    input.envelope.event.authoredFadeOutCandidate = true;
    input.stop = FixedEvent(256, program, 30);
    input.stop.event.subeventPhase = 50;
    input.stop.event.payload = 73;
    input.stopFrame = 256;
    input.fadeStartFrame = 128;
    return input;
}

TEST(ImmediateActionLane, AuthoredLayoutRepeatedCompleteShapesUseCopiedOriginalExpression) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 3, 3, 64}));
    const harken::TimelineProgramView program{};
    auto input = AuthoredLayout(&program);
    harken::ImmediateLaneHandle handle{};
    const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
    input.start.relativeFrame = 4096;
    input.fadeStartFrame = 4096;
    input.stopFrame = 8192;
    for (const auto anchor : {64u, 192u, 256u, 320u, 192u})
        ASSERT_TRUE(edit.StageAuthoredControlCandidate(anchor, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.Publish(handle));
    const auto* first = lane.CaptureDue(handle.slot, 3);
    ASSERT_NE(nullptr, first);
    ASSERT_EQ(2u, first->eventCount);
    const auto* events = lane.Events(handle.slot, first);
    EXPECT_EQ(192u, events[0].event.executeFrame);
    EXPECT_EQ(10u, events[0].event.executionOrdinal);
    EXPECT_EQ(71u, events[0].event.rowSequence);
    EXPECT_EQ(192u, events[1].event.executeFrame);
    EXPECT_EQ(30u, events[1].event.subeventPhase);
    EXPECT_EQ(72u, events[1].event.payload);
    ASSERT_TRUE(lane.Claim(handle, first));
    ASSERT_TRUE(lane.Finish(handle.slot, first, true));
    const auto* last = lane.CaptureDue(handle.slot, 4);
    ASSERT_NE(nullptr, last);
    ASSERT_EQ(1u, last->eventCount);
    const auto& stop = lane.Events(handle.slot, last)[0].event;
    EXPECT_EQ(256u, stop.executeFrame);
    EXPECT_EQ(50u, stop.subeventPhase);
    EXPECT_EQ(73u, stop.payload);
    EXPECT_TRUE(stop.authoredFadeOutCandidate);
    ASSERT_TRUE(lane.Claim(handle, last));
    ASSERT_TRUE(lane.Finish(handle.slot, last, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, AuthoredLayoutStopBeforeShiftedStartOmitsEnvelopeAndKeepsLiteralStop) {
    for (const auto anchor : {256u, 320u}) {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 3, 3, 64}));
        const harken::TimelineProgramView program{};
        const auto input = AuthoredLayout(&program);
        harken::ImmediateLaneHandle handle{};
        const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
        ASSERT_TRUE(lane.Reserve(1, demand, &handle));
        harken::PrivatePlainLayoutEdit edit;
        ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
        ASSERT_TRUE(edit.StageAuthoredControlCandidate(anchor, 64));
        harken::PrivatePlainLayoutCommitProof ready;
        ASSERT_TRUE(edit.SealCandidate(&ready));
        ready.Consume();
        ASSERT_TRUE(lane.Publish(handle));
        std::uint32_t count = 0;
        for (std::uint64_t quantum = 4; quantum <= anchor / 64; ++quantum) {
            const auto* bucket = lane.CaptureDue(handle.slot, quantum);
            ASSERT_NE(nullptr, bucket);
            const auto* events = lane.Events(handle.slot, bucket);
            for (std::uint32_t i = 0; i < bucket->eventCount; ++i) {
                ++count;
                EXPECT_NE(30u, events[i].event.subeventPhase);
                if (events[i].event.subeventPhase == 50) {
                    EXPECT_EQ(256u, events[i].event.executeFrame);
                    EXPECT_FALSE(events[i].event.authoredFadeOutCandidate);
                } else EXPECT_EQ(anchor, events[i].event.executeFrame);
            }
            ASSERT_TRUE(lane.Claim(handle, bucket));
            ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
        }
        EXPECT_EQ(2u, count);
        ASSERT_TRUE(lane.Retire(handle));
    }
}

TEST(ImmediateActionLane, AuthoredLayoutFixedStartDoesNotSlideWithLaterAnchor) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 3, 3, 64}));
    const harken::TimelineProgramView program{};
    auto input = AuthoredLayout(&program);
    input.start = FixedEvent(96, &program, 10);
    harken::ImmediateLaneHandle handle{};
    const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(320, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.Publish(handle));
    for (const auto frame : {96u, 128u, 256u}) {
        const auto* bucket = lane.CaptureDue(handle.slot, frame / 64);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        EXPECT_EQ(frame, lane.Events(handle.slot, bucket)[0].event.executeFrame);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, AuthoredLayoutChecksThirdBucketAndClampedSameQuantumCapacityTogether) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 3, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto input = AuthoredLayout(&program);
    harken::ImmediateLaneHandle handle{}, blocker{};
    const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Reserve(2, 1, &blocker));
    ASSERT_TRUE(lane.Prepare(blocker, &input.stop, 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(blocker, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(blocker));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
    EXPECT_FALSE(edit.StageAuthoredControlCandidate(64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    EXPECT_FALSE(edit.SealCandidate(&ready));
    ASSERT_TRUE(lane.Cancel(blocker));
    ASSERT_TRUE(lane.Retire(blocker));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
    EXPECT_FALSE(edit.StageAuthoredControlCandidate(192, 64));
    EXPECT_FALSE(edit.SealCandidate(&ready));
    EXPECT_FALSE(edit.StageAuthoredControlCandidate(320, 320));
    EXPECT_FALSE(edit.SealCandidate(&ready));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, AuthoredLayoutRequiresWorstCaseCreditBeforeCandidateSelection) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 3, 3, 64}));
    const harken::TimelineProgramView program{};
    auto input = AuthoredLayout(&program);
    harken::ImmediateLaneHandle small{}, full{};
    const harken::ImmediateLaneDemand smallDemand{2, 0, 0, 2};
    const harken::ImmediateLaneDemand fullDemand{3, 0, 0, 3};
    ASSERT_TRUE(lane.Reserve(1, smallDemand, &small));
    ASSERT_TRUE(lane.Reserve(2, fullDemand, &full));
    harken::PrivatePlainLayoutEdit edit;
    EXPECT_FALSE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(small, 1, smallDemand, input, &edit));
    EXPECT_FALSE(edit.Valid());
    EXPECT_FALSE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(full, 2, {3, 0, 0, 2}, input, &edit));
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(full, 2, fullDemand, input, &edit));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(320, 64));
    edit.Abort();
    input.fadeStartFrame = input.stopFrame;
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(small, 1, smallDemand, input, &edit));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.Publish(small));
    for (const auto quantum : {1u, 4u}) {
        const auto* bucket = lane.CaptureDue(small.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        EXPECT_FALSE(lane.Events(small.slot, bucket)[0].event.authoredFadeOutCandidate);
        ASSERT_TRUE(lane.Claim(small, bucket));
        ASSERT_TRUE(lane.Finish(small.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(small));
    ASSERT_TRUE(lane.Cancel(full));
    ASSERT_TRUE(lane.Retire(full));
}

TEST(ImmediateActionLane, AuthoredLayoutOverflowAndGenericBypassClearPreviouslyReadyShape) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 3, 3, 64}));
    const harken::TimelineProgramView program{};
    auto input = AuthoredLayout(&program);
    harken::ImmediateLaneHandle handle{};
    const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
    EXPECT_FALSE(edit.StageCandidate(&input.stop, 1, 64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    EXPECT_FALSE(edit.SealCandidate(&ready));
    edit.Abort();
    input.start.relativeFrame = UINT64_MAX - 64;
    ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
    ASSERT_TRUE(edit.StageAuthoredControlCandidate(0, 0));
    EXPECT_FALSE(edit.StageAuthoredControlCandidate(64, 0));
    EXPECT_FALSE(edit.SealCandidate(&ready));
    EXPECT_FALSE(edit.StageAuthoredControlCandidate(128, 0));
    edit.Abort();
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(&input.stop, 1, 64, 64));
    EXPECT_FALSE(edit.StageAuthoredControlCandidate(64, 64));
    EXPECT_FALSE(edit.SealCandidate(&ready));
    edit.Abort();
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, AuthoredLayoutAbortRestoresAllThreeOriginalBucketsAndEscrow) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 3, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto input = AuthoredLayout(&program);
    const harken::ImmediateLaneEvent originals[]{
        FixedEvent(64, &program, 8), FixedEvent(128, &program, 9), FixedEvent(192, &program, 10)};
    harken::ImmediateLaneHandle handle{}, peer{};
    const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Prepare(handle, originals, 3));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.Reserve(2, 1, &peer));
    ASSERT_TRUE(lane.Prepare(peer, &originals[2], 1));
    {
        harken::PrivatePlainLayoutEdit edit;
        ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
        ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
        EXPECT_FALSE(lane.TryPlace(peer, 64, 0, &anchor));
        harken::PrivatePlainLayoutEdit moved(std::move(edit));
        EXPECT_FALSE(edit.Valid());
        EXPECT_FALSE(lane.Reset({}));
    }
    ASSERT_TRUE(lane.Publish(handle));
    for (const auto quantum : {1u, 2u, 3u}) {
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        EXPECT_EQ(quantum * 64, lane.Events(handle.slot, bucket)[0].event.executeFrame);
        EXPECT_EQ(quantum + 7, lane.Events(handle.slot, bucket)[0].event.executionOrdinal);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(handle));
    ASSERT_TRUE(lane.TryPlace(peer, 64, 0, &anchor));
    ASSERT_TRUE(lane.Cancel(peer));
    ASSERT_TRUE(lane.Retire(peer));
}

TEST(ImmediateActionLane, AuthoredLayoutInvalidTemplatesAndOwnerFirstProofLifetimeAreSafe) {
    const harken::TimelineProgramView program{};
    auto input = AuthoredLayout(&program);
    harken::PrivatePlainLayoutCommitProof survivor;
    {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 3, 3, 64}));
        harken::ImmediateLaneHandle handle{};
        const harken::ImmediateLaneDemand demand{3, 0, 0, 3};
        ASSERT_TRUE(lane.Reserve(1, demand, &handle));
        harken::PrivatePlainLayoutEdit edit;
        for (int mutation = 0; mutation != 6; ++mutation) {
            auto wrong = input;
            if (mutation == 0) wrong.stopFrame = UINT64_MAX;
            if (mutation == 1) wrong.stop.fixedFrame = 128;
            if (mutation == 2) wrong.start.relativeFrame = UINT64_MAX;
            if (mutation == 3) wrong.envelope.event.program.program = nullptr;
            if (mutation == 4) wrong.stop.event.subeventPhase = 30;
            if (mutation == 5) wrong.start.futureGroupedStart = true;
            EXPECT_FALSE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, wrong, &edit));
            EXPECT_FALSE(edit.Valid());
        }
        ASSERT_TRUE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
        EXPECT_FALSE(lane.AcquirePrivatePlainAuthoredControlLayoutEdit(handle, 1, demand, input, &edit));
        ASSERT_TRUE(edit.StageAuthoredControlCandidate(64, 64));
        harken::PrivatePlainLayoutEdit moved(std::move(edit));
        ASSERT_TRUE(moved.StageAuthoredControlCandidate(192, 64));
        ASSERT_TRUE(moved.SealCandidate(&survivor));
        EXPECT_FALSE(lane.Publish(handle));
    }
    EXPECT_FALSE(survivor.Valid());
    survivor.Consume();
}

std::array<harken::ImmediateLaneEvent, 6> PlainBank(
    const harken::TimelineProgramView* program) {
    std::array<harken::ImmediateLaneEvent, 6> events{};
    for (std::uint32_t i = 0; i < events.size(); ++i)
        events[i] = FixedEvent((i + 1u) * 64u, program, i);
    return events;
}

TEST(ImmediateActionLane, PristineReservedPlainWholeBankConsumesActualSixBucketPlacement) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 8, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto events = PlainBank(&program);
    const harken::ImmediateLaneDemand demand{8, 0, 0, 8};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    harken::PrivatePlainLayoutEdit edit;
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(lane.AcquirePrivateReservedPlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(events.data(), static_cast<std::uint32_t>(events.size()), 64, 64));
    EXPECT_FALSE(lane.Prepare(handle, events.data(), 6));
    EXPECT_FALSE(lane.Publish(handle));
    EXPECT_FALSE(lane.Cancel(handle));
    EXPECT_FALSE(lane.Retire(handle));
    harken::PrivatePlainLayoutCommitProof ready;
    ASSERT_TRUE(edit.SealCandidate(&ready));
    EXPECT_FALSE(lane.Publish(handle));
    ready.Consume();
    ASSERT_TRUE(lane.Publish(handle));
    for (std::uint32_t quantum = 1; quantum <= 6; ++quantum) {
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        const auto& event = lane.Events(handle.slot, bucket)[0].event;
        EXPECT_EQ(quantum * 64u, event.executeFrame);
        EXPECT_EQ(quantum - 1u, event.executionOrdinal);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 7));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PristineReservedPlainLastBucketCollisionRefusesWholeCandidate) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 8, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto events = PlainBank(&program);
    const harken::ImmediateLaneDemand demand{8, 0, 0, 8};
    harken::ImmediateLaneHandle handle{}, peer{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Reserve(2, 1, &peer));
    ASSERT_TRUE(lane.Prepare(peer, &events.back(), 1));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(peer, 64, 0, &anchor));
    ASSERT_TRUE(lane.Publish(peer));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivateReservedPlainLayoutEdit(handle, 1, demand, &edit));
    EXPECT_FALSE(edit.StageCandidate(events.data(), 6, 64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    EXPECT_FALSE(edit.SealCandidate(&ready));
    ASSERT_TRUE(lane.Cancel(peer));
    ASSERT_TRUE(lane.Retire(peer));
    ASSERT_TRUE(edit.StageCandidate(events.data(), 6, 64, 64));
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PristineReservedPlainRepeatedLargeSmallShapesKeepOnlyActualTentativeCapacity) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({3, 8, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto events = PlainBank(&program);
    const harken::ImmediateLaneDemand demand{8, 0, 0, 8};
    harken::ImmediateLaneHandle handle{}, last{}, first{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Reserve(2, 1, &last));
    ASSERT_TRUE(lane.Prepare(last, &events.back(), 1));
    ASSERT_TRUE(lane.Reserve(3, 1, &first));
    ASSERT_TRUE(lane.Prepare(first, &events.front(), 1));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivateReservedPlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(events.data(), 6, 64, 64));
    std::uint64_t anchor = 0;
    EXPECT_FALSE(lane.TryPlace(last, 64, 0, &anchor));
    ASSERT_TRUE(edit.StageCandidate(events.data(), 2, 64, 64));
    ASSERT_TRUE(lane.TryPlace(last, 64, 0, &anchor));
    EXPECT_FALSE(edit.StageCandidate(events.data(), 6, 64, 64));
    harken::PrivatePlainLayoutCommitProof ready;
    EXPECT_FALSE(edit.SealCandidate(&ready));
    ASSERT_TRUE(lane.TryPlace(first, 64, 0, &anchor));
    ASSERT_TRUE(lane.Cancel(first));
    ASSERT_TRUE(lane.Retire(first));
    ASSERT_TRUE(lane.Cancel(last));
    ASSERT_TRUE(lane.Retire(last));
    ASSERT_TRUE(edit.StageCandidate(events.data(), 6, 64, 64));
    ASSERT_TRUE(edit.SealCandidate(&ready));
    ready.Consume();
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PristineReservedPlainAbortClearsAllCreditsAndReusedSmallEditRestoresSnapshots) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 8, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto events = PlainBank(&program);
    const harken::ImmediateLaneDemand demand{8, 0, 0, 8};
    harken::ImmediateLaneHandle handle{}, peer{};
    ASSERT_TRUE(lane.Reserve(1, demand, &handle));
    ASSERT_TRUE(lane.Reserve(2, 1, &peer));
    ASSERT_TRUE(lane.Prepare(peer, &events.back(), 1));
    harken::PrivatePlainLayoutEdit edit;
    ASSERT_TRUE(lane.AcquirePrivateReservedPlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(events.data(), 6, 64, 64));
    std::uint64_t anchor = 0;
    EXPECT_FALSE(lane.TryPlace(peer, 64, 0, &anchor));
    edit.Abort();
    ASSERT_TRUE(lane.TryPlace(peer, 64, 0, &anchor));
    ASSERT_TRUE(lane.Prepare(handle, events.data(), 2));
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit(handle, 1, demand, &edit));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    const harken::ImmediateLaneDemand small{3, 0, 0, 3};
    handle = {};
    ASSERT_TRUE(lane.Reserve(3, small, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events.data(), 3));
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    ASSERT_TRUE(lane.AcquirePrivatePlainLayoutEdit(handle, 3, small, &edit));
    const auto changed = FixedEvent(256, &program, 77);
    ASSERT_TRUE(edit.StageCandidate(&changed, 1, 64, 64));
    edit.Abort();
    ASSERT_TRUE(lane.Publish(handle));
    for (std::uint32_t quantum = 1; quantum <= 3; ++quantum) {
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_EQ(1u, bucket->eventCount);
        EXPECT_EQ(quantum * 64u, lane.Events(handle.slot, bucket)[0].event.executeFrame);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    }
    ASSERT_TRUE(lane.Retire(handle));
    ASSERT_TRUE(lane.Cancel(peer));
    ASSERT_TRUE(lane.Retire(peer));
}

TEST(ImmediateActionLane, PristineReservedPlainIssuesFromActualLocalReservationAndRejectsNonemptyForeignHolder) {
    harken::ImmediateActionLane lane, other;
    ASSERT_TRUE(lane.Reset({1, 8, 1, 64}));
    ASSERT_TRUE(other.Reset({1, 8, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto events = PlainBank(&program);
    const harken::ImmediateLaneDemand demand{8, 0, 0, 8};
    harken::ImmediateLaneHandle local{}, equal{};
    ASSERT_TRUE(lane.Reserve(1, demand, &local));
    harken::PrivatePlainLayoutEdit edit;
    EXPECT_FALSE(other.AcquirePrivateReservedPlainLayoutEdit(local, 1, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit(local, 2, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit({local.slot, local.generation + 1}, 1, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit(local, 1, {7, 0, 0, 8}, &edit));
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit(local, 1, {8, 0, 0, 7}, &edit));
    ASSERT_TRUE(other.Reserve(1, demand, &equal));
    ASSERT_EQ(local.slot, equal.slot);
    ASSERT_EQ(local.generation, equal.generation);
    // The DTO names a genuine receiving-table reservation, not its provenance.
    ASSERT_TRUE(other.AcquirePrivateReservedPlainLayoutEdit(local, 1, demand, &edit));
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit(local, 1, demand, &edit));
    ASSERT_TRUE(edit.StageCandidate(events.data(), 6, 64, 64));
    EXPECT_FALSE(other.Prepare(equal, events.data(), 6));
    ASSERT_TRUE(lane.Prepare(local, events.data(), 6));
    harken::PrivatePlainLayoutEdit refused;
    EXPECT_FALSE(lane.AcquirePrivateReservedPlainLayoutEdit(local, 1, demand, &refused));
    edit.Abort();
    ASSERT_TRUE(other.Prepare(equal, events.data(), 6));
    for (auto* table : {&lane, &other}) {
        ASSERT_TRUE(table->Cancel(local));
        ASSERT_TRUE(table->Retire(local));
    }
}

TEST(ImmediateActionLane, PristineReservedPlainMoveAssignmentAndOwnerFirstDestructionReleaseWholeHold) {
    const harken::TimelineProgramView program{};
    const auto events = PlainBank(&program);
    const harken::ImmediateLaneDemand demand{8, 0, 0, 8};
    harken::PrivatePlainLayoutCommitProof survivor;
    {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({2, 8, 2, 64}));
        harken::ImmediateLaneHandle first{}, second{};
        ASSERT_TRUE(lane.Reserve(1, demand, &first));
        ASSERT_TRUE(lane.Reserve(2, demand, &second));
        harken::PrivatePlainLayoutEdit a, b;
        ASSERT_TRUE(lane.AcquirePrivateReservedPlainLayoutEdit(first, 1, demand, &a));
        ASSERT_TRUE(lane.AcquirePrivateReservedPlainLayoutEdit(second, 2, demand, &b));
        ASSERT_TRUE(a.StageCandidate(events.data(), 6, 64, 64));
        ASSERT_TRUE(b.StageCandidate(events.data(), 6, 64, 64));
        b = std::move(a);
        EXPECT_FALSE(a.Valid());
        ASSERT_TRUE(lane.Prepare(second, events.data(), 6));
        EXPECT_FALSE(lane.Reset({}));
        EXPECT_EQ(nullptr, lane.CaptureDue(first.slot, 1));
        ASSERT_TRUE(b.SealCandidate(&survivor));
    }
    EXPECT_FALSE(survivor.Valid());
    survivor.Consume();
}


// These actual Lane factory cases certify a private placement only. The local
// Event inputs do not stand in for physical Core/Source/Queue capabilities.
TEST(ImmediateActionLane, ActualPlacementProofIsIssuedOnlyOnTruePlaceAndPreservesFailureOutputs) {
    using Proof = harken::PrivateImmediateLanePlacementProof;
    static_assert(!std::is_copy_constructible_v<Proof>);
    static_assert(!std::is_copy_assignable_v<Proof>);
    static_assert(std::is_nothrow_move_constructible_v<Proof>);
    static_assert(!std::is_constructible_v<Proof, harken::ImmediateLaneHandle, std::uint64_t>);
    harken::ImmediateActionLane lane, foreign;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    ASSERT_TRUE(foreign.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(17, &program, 0);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(73, 1, &handle));
    Proof proof, output;
    std::uint64_t anchor = UINT64_MAX, observed = 91;
    EXPECT_FALSE(proof.CaptureAnchorFrame(&observed));
    EXPECT_EQ(91u, observed);
    EXPECT_FALSE(lane.TryPlace(handle, 65, 0, &anchor, &proof));
    EXPECT_EQ(UINT64_MAX, anchor);
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    ASSERT_TRUE(lane.TryPlace(handle, 65, 0, &anchor, &proof));
    EXPECT_EQ(128u, anchor);
    ASSERT_TRUE(proof.CaptureAnchorFrame(&observed));
    EXPECT_EQ(anchor, observed);
    EXPECT_FALSE(proof.CaptureAnchorFrame(nullptr));
    EXPECT_TRUE(lane.IsPrivatePlacementProofFor(proof, handle, 73));
    EXPECT_FALSE(foreign.IsPrivatePlacementProofFor(proof, handle, 73));
    EXPECT_FALSE(lane.IsPrivatePlacementProofFor(proof, handle, 74));
    EXPECT_FALSE(lane.IsPrivatePlacementProofFor(proof, {handle.slot, handle.generation + 1u}, 73));
    anchor = 93;
    EXPECT_FALSE(lane.TryPlace(handle, 256, 0, &anchor, &proof));
    EXPECT_FALSE(lane.TryPlace(handle, 256, 0, &anchor, &output));
    EXPECT_EQ(93u, anchor);
    EXPECT_TRUE(proof.Valid());
    EXPECT_FALSE(output.Valid());
    proof.Release();
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    harken::ImmediateLaneHandle reused{};
    ASSERT_TRUE(lane.Reserve(75, 1, &reused));
    ASSERT_EQ(handle.slot, reused.slot);
    ASSERT_NE(handle.generation, reused.generation);
    ASSERT_TRUE(lane.Prepare(reused, &event, 1));
    EXPECT_FALSE(lane.TryPlace(handle, 256, 0, &anchor, &output));
    EXPECT_EQ(93u, anchor);
    EXPECT_FALSE(output.Valid());
    ASSERT_TRUE(lane.TryPlace(reused, 256, 0, &anchor, nullptr));
    EXPECT_EQ(256u, anchor);
    ASSERT_TRUE(lane.Cancel(reused));
    ASSERT_TRUE(lane.Retire(reused));
}

TEST(ImmediateActionLane, PublicationProofHoldsActualLayoutAndAppliesWithoutReacquiringReaderGate) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[]{Event(0, &program, 0), Event(17, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(73, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    std::uint64_t anchor = 0;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor));
    harken::PrivateImmediateLanePublicationProof proof;
    ASSERT_TRUE(lane.ProvePrivatePublication(handle, 73, &proof));
    EXPECT_FALSE(lane.Publish(handle));
    EXPECT_FALSE(lane.Cancel(handle));
    EXPECT_FALSE(lane.Retire(handle));
    EXPECT_FALSE(lane.Reset({1, 2, 2, 64}));
    std::uint64_t refusedAnchor = 99;
    EXPECT_FALSE(lane.TryPlace(handle, 128, 0, &refusedAnchor));
    EXPECT_EQ(99u, refusedAnchor);
    const harken::ImmediateLaneEvent changed[]{Event(0, &program, 3)};
    EXPECT_FALSE(lane.Prepare(handle, changed, 1));
    std::thread audio([&] { EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 1)); });
    audio.join();
    harken::ImmediateActionLane::ApplyPrivatePublication(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_EQ(2u, bucket->eventCount);
    const auto* actual = lane.Events(handle.slot, bucket);
    ASSERT_NE(nullptr, actual);
    EXPECT_EQ(64u, actual[0].event.executeFrame);
    EXPECT_EQ(81u, actual[1].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, PublicationProofRefusalAbandonMoveAndOwnerDestructionReturnExactGate) {
    harken::PrivateImmediateLanePublicationProof survivor;
    {
        harken::ImmediateActionLane lane, foreign;
        ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
        ASSERT_TRUE(foreign.Reset({1, 1, 1, 64}));
        const harken::TimelineProgramView program{};
        const auto event = Event(0, &program, 0);
        harken::ImmediateLaneHandle a{}, b{};
        ASSERT_TRUE(lane.Reserve(73, 1, &a));
        ASSERT_TRUE(foreign.Reserve(74, 1, &b));
        EXPECT_FALSE(lane.ProvePrivatePublication(a, 73, &survivor));
        EXPECT_FALSE(survivor.Valid());
        ASSERT_TRUE(lane.Prepare(a, &event, 1));
        ASSERT_TRUE(foreign.Prepare(b, &event, 1));
        std::uint64_t anchor = 0;
        ASSERT_TRUE(lane.TryPlace(a, 64, 0, &anchor));
        ASSERT_TRUE(foreign.TryPlace(b, 128, 0, &anchor));
        EXPECT_FALSE(lane.ProvePrivatePublication(a, 74, &survivor));
        EXPECT_FALSE(lane.ProvePrivatePublication({a.slot, a.generation + 1}, 73, &survivor));
        bool refusedWhileBusy = false;
        struct BusyContext {
            harken::ImmediateActionLane& lane;
            harken::ImmediateLaneHandle handle;
            harken::PrivateImmediateLanePublicationProof& proof;
            bool& refused;
        } context{lane, a, survivor, refusedWhileBusy};
        lane.SetCaptureGateHookForTest([](void* opaque) {
            auto& busy = *static_cast<BusyContext*>(opaque);
            busy.refused = !busy.lane.ProvePrivatePublication(busy.handle, 73, &busy.proof);
        }, &context);
        EXPECT_EQ(nullptr, lane.CaptureDue(a.slot, 1));
        lane.SetCaptureGateHookForTest(nullptr, nullptr);
        EXPECT_TRUE(refusedWhileBusy);
        EXPECT_FALSE(survivor.Valid());
        ASSERT_TRUE(lane.ProvePrivatePublication(a, 73, &survivor));
        harken::PrivateImmediateLanePublicationProof other;
        ASSERT_TRUE(foreign.ProvePrivatePublication(b, 74, &other));
        survivor = std::move(other);
        EXPECT_FALSE(other.Valid());
        ASSERT_TRUE(lane.Cancel(a));
        ASSERT_TRUE(lane.Retire(a));
        harken::PrivateImmediateLanePublicationProof moved(std::move(survivor));
        EXPECT_FALSE(survivor.Valid());
        EXPECT_FALSE(foreign.Cancel(b));
        survivor = std::move(moved);
        survivor.Release();
        ASSERT_TRUE(foreign.TryPlace(b, 192, 0, &anchor));
        ASSERT_TRUE(foreign.ProvePrivatePublication(b, 74, &survivor));
    }
    EXPECT_FALSE(survivor.Valid());
    survivor.Release();
}

TEST(ImmediateActionLane, ActualPlacementProofBrieflyHoldsPrivateAnchorThenAllowsRealRetryPublishAndApply) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const harken::ImmediateLaneEvent events[]{Event(0, &program, 0), Event(17, &program, 1)};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(73, {2, 0, 0, 1}, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    harken::PrivateImmediateLanePlacementProof proof;
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(lane.TryPlace(handle, 64, 0, &anchor, &proof));
    EXPECT_FALSE(lane.Publish(handle));
    EXPECT_FALSE(lane.Cancel(handle));
    EXPECT_FALSE(lane.Retire(handle));
    EXPECT_FALSE(lane.Reset({1, 2, 2, 64}));
    harken::PrivatePlainLayoutEdit edit;
    EXPECT_FALSE(lane.AcquirePrivatePlainLayoutEdit(handle, 73, {2, 0, 0, 1}, &edit));
    EXPECT_FALSE(edit.Valid());
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 1));
    EXPECT_TRUE(proof.Valid());
    proof.Release();
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(lane.TryPlace(handle, 128, 0, &anchor));
    EXPECT_EQ(128u, anchor);
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 2);
    ASSERT_NE(nullptr, bucket);
    ASSERT_EQ(2u, bucket->eventCount);
    const auto* actual = lane.Events(handle.slot, bucket);
    ASSERT_NE(nullptr, actual);
    EXPECT_EQ(128u, actual[0].event.executeFrame);
    EXPECT_EQ(145u, actual[1].event.executeFrame);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    ASSERT_TRUE(lane.Retire(handle));
}

TEST(ImmediateActionLane, ActualPlacementProofMoveOverwriteAndDropReturnExactPriorPrivateReceiver) {
    harken::ImmediateActionLane first, second;
    ASSERT_TRUE(first.Reset({1, 1, 1, 64}));
    ASSERT_TRUE(second.Reset({1, 1, 1, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle a{}, b{};
    ASSERT_TRUE(first.Reserve(73, 1, &a));
    ASSERT_TRUE(second.Reserve(74, 1, &b));
    ASSERT_TRUE(first.Prepare(a, &event, 1));
    ASSERT_TRUE(second.Prepare(b, &event, 1));
    std::uint64_t anchor = 0;
    {
        harken::PrivateImmediateLanePlacementProof x, y;
        ASSERT_TRUE(first.TryPlace(a, 64, 0, &anchor, &x));
        ASSERT_TRUE(second.TryPlace(b, 128, 0, &anchor, &y));
        x = std::move(y);
        EXPECT_FALSE(y.Valid());
        EXPECT_TRUE(second.IsPrivatePlacementProofFor(x, b, 74));
        EXPECT_FALSE(first.IsPrivatePlacementProofFor(x, a, 73));
        ASSERT_TRUE(first.Cancel(a));
        ASSERT_TRUE(first.Retire(a));
        harken::PrivateImmediateLanePlacementProof moved(std::move(x));
        EXPECT_FALSE(x.Valid());
        EXPECT_TRUE(moved.Valid());
        EXPECT_FALSE(second.Cancel(b));
    }
    ASSERT_TRUE(second.Cancel(b));
    ASSERT_TRUE(second.Retire(b));
}

TEST(ImmediateActionLane, ActualPlacementProofRefusalDoesNotGrantAnchorAndNativeOwnerDestructionReturnsShortProof) {
    harken::PrivateImmediateLanePlacementProof survivor;
    {
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
        const harken::TimelineProgramView program{};
        const harken::ImmediateLaneEvent events[]{Event(0, &program, 0), Event(128, &program, 1)};
        harken::ImmediateLaneHandle under{};
        ASSERT_TRUE(lane.Reserve(73, {2, 0, 0, 1}, &under));
        ASSERT_TRUE(lane.Prepare(under, events, 2));
        std::uint64_t anchor = 97;
        EXPECT_FALSE(lane.TryPlace(under, 64, 0, &anchor, &survivor));
        EXPECT_EQ(97u, anchor);
        EXPECT_FALSE(survivor.Valid());
        ASSERT_TRUE(lane.Cancel(under));
        ASSERT_TRUE(lane.Retire(under));
        harken::ImmediateLaneHandle exact{};
        ASSERT_TRUE(lane.Reserve(74, {2, 0, 0, 2}, &exact));
        ASSERT_TRUE(lane.Prepare(exact, events, 2));
        ASSERT_TRUE(lane.TryPlace(exact, 64, 0, &anchor, &survivor));
        EXPECT_TRUE(survivor.Valid());
        // This proof is only an unpublished Lane-native hold, not a Voice end.
    }
    EXPECT_FALSE(survivor.Valid());
    std::uint64_t observed = 101;
    EXPECT_FALSE(survivor.CaptureAnchorFrame(&observed));
    EXPECT_EQ(101u, observed);
    survivor.Release();
}

} // namespace


TEST(ImmediateActionLane, OriginalUnpublishedStorageRetentionRequiresExactNativeOwner) {
    harken::ImmediateActionLane lane, foreignReceiver;
    ASSERT_TRUE(lane.Reset({2, 2, 2, 64}));
    ASSERT_TRUE(foreignReceiver.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle handle{}, other{};
    harken::OriginalImmediateLaneAttemptLifetimePin own, foreign, otherOwn;
    ASSERT_TRUE(lane.Reserve(41, {1, 0, 0, 1}, &handle, &own));
    ASSERT_TRUE(lane.Reserve(41, {1, 0, 0, 1}, &other, &otherOwn));
    ASSERT_TRUE(own.Borrow(&foreign));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    harken::PrivateImmediateLanePlacementProof placement;
    ASSERT_TRUE(lane.TryPlaceExactPrivate(handle, 64, &placement));
    EXPECT_FALSE(lane.Cancel(handle));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    placement.Release();
    ASSERT_TRUE(lane.Cancel(handle));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(otherOwn, handle, 41, 0));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, other, 41, 0));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 42, 0));
    EXPECT_FALSE(foreignReceiver.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    EXPECT_TRUE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    EXPECT_FALSE(lane.Retire(handle, 0, &own));
    EXPECT_TRUE(own.Valid());
    EXPECT_TRUE(foreign.Valid());
    foreign.Return();
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    ASSERT_TRUE(lane.Retire(handle, 0, &own));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    harken::ImmediateLaneHandle replacement{};
    harken::OriginalImmediateLaneAttemptLifetimePin replacementOwn;
    ASSERT_TRUE(lane.Reserve(43, {1, 0, 0, 1}, &replacement, &replacementOwn));
    EXPECT_EQ(handle.slot, replacement.slot);
    EXPECT_NE(handle.generation, replacement.generation);
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(replacementOwn, handle, 41, 0));
    ASSERT_TRUE(lane.Cancel(replacement));
    ASSERT_TRUE(lane.Retire(replacement, 0, &replacementOwn));
    ASSERT_TRUE(lane.Cancel(other));
    ASSERT_TRUE(lane.Retire(other, 0, &otherOwn));
}


TEST(ImmediateActionLane, OriginalUnpublishedStorageRetentionRefusesInstalledNativeArmPin) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    harken::TimelineArmTable arms(1);
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.Prepare(0, 1, 64, 64, 1, {}, &arm));
    ASSERT_TRUE(arms.CommitPrepared(arm));
    ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
    harken::TimelineProgramView program{};
    program.generation = 1;
    auto event = Event(0, &program, 0);
    event.event.program.programGeneration = 1;
    event.event.armToken = arm;
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin own, foreign;
    ASSERT_TRUE(lane.Reserve(41, {1, 0, 0, 1}, &handle, &own));
    ASSERT_TRUE(own.Borrow(&foreign));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    ASSERT_EQ(1u, arms.Resolve(arm)->frontierReaderPins.load());
    ASSERT_TRUE(lane.Cancel(handle));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 8));
    EXPECT_FALSE(lane.Retire(handle, 8, &own));
    EXPECT_EQ(1u, arms.Resolve(arm)->frontierReaderPins.load());
    foreign.Return();
    ASSERT_TRUE(lane.Retire(handle, 8, &own));
    EXPECT_EQ(0u, arms.Resolve(arm)->frontierReaderPins.load());
}

#if !defined(HARKEN_RT_QUALIFICATION)
TEST(ImmediateActionLane, OriginalUnpublishedStorageRetentionRefusesActualCancelledReaderGate) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 64}));
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin own, foreign;
    ASSERT_TRUE(lane.Reserve(41, {1, 0, 0, 1}, &handle, &own));
    ASSERT_TRUE(own.Borrow(&foreign));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    struct Probe {
        harken::ImmediateActionLane* lane;
        const harken::OriginalImmediateLaneAttemptLifetimePin* own;
        harken::ImmediateLaneHandle handle;
        bool called = false;
        bool storageRetained = true;
    } probe{&lane, &own, handle};
    lane.SetCaptureGateHookForTest([](void* context) {
        auto& state = *static_cast<Probe*>(context);
        state.called = true;
        state.storageRetained = state.lane->ObserveOriginalUnpublishedStorageOnlyRetention(
            *state.own, state.handle, 41, 0);
    }, &probe);
    EXPECT_EQ(nullptr, lane.CaptureDue(handle.slot, 1));
    lane.SetCaptureGateHookForTest(nullptr, nullptr);
    ASSERT_TRUE(probe.called);
    EXPECT_FALSE(probe.storageRetained);
    EXPECT_TRUE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 0));
    foreign.Return();
    ASSERT_TRUE(lane.Retire(handle, 0, &own));
}
#endif

TEST(ImmediateActionLane, OriginalAttemptLoanProtectsReuseWithoutBlockingActualAudio) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin own, foreign;
    ASSERT_TRUE(lane.Reserve(41, {1, 0, 0, 1}, &handle, &own));
    ASSERT_TRUE(own.Valid());
    ASSERT_TRUE(own.Borrow(&foreign));
    ASSERT_TRUE(lane.IsOriginalAttemptLifetimePinFor(foreign, handle, 41));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    ASSERT_TRUE(lane.TryPlaceExactPrivate(handle, 64));
    ASSERT_TRUE(lane.Publish(handle));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 2));
    const auto* bucket = lane.CaptureDue(handle.slot, 1);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(lane.Claim(handle, bucket));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 2));
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(own, handle, 41, 2));
    EXPECT_FALSE(lane.Retire(handle));
    EXPECT_FALSE(lane.Retire(handle, 2, &own));
    EXPECT_FALSE(lane.Reset({1, 2, 2, 64}));
    EXPECT_TRUE(own.Valid());
    EXPECT_TRUE(foreign.Valid());
    foreign.Return();
    ASSERT_TRUE(lane.Retire(handle, 2, &own));
    EXPECT_FALSE(own.Valid());
    harken::ImmediateLaneObservation retired{};
    EXPECT_FALSE(lane.Observe(handle, &retired));
    ASSERT_TRUE(lane.Reset({1, 2, 2, 64}));
}

TEST(ImmediateActionLane, OriginalAttemptReceiverAndNonemptyOutputRefusalsPreserveActualOwner) {
    harken::ImmediateActionLane first, second;
    ASSERT_TRUE(first.Reset({1, 1, 1, 64}));
    ASSERT_TRUE(second.Reset({1, 1, 1, 64}));
    harken::ImmediateLaneHandle a{}, b{};
    harken::OriginalImmediateLaneAttemptLifetimePin original, other;
    ASSERT_TRUE(first.Reserve(7, {1, 0, 0, 1}, &a, &original));
    ASSERT_TRUE(second.Reserve(7, {1, 0, 0, 1}, &b, &other));
    ASSERT_EQ(a.slot, b.slot);
    ASSERT_EQ(a.generation, b.generation);
    EXPECT_FALSE(second.IsOriginalAttemptLifetimePinFor(original, b, 7));
    EXPECT_FALSE(first.IsOriginalAttemptLifetimePinFor(original, a, 8));
    EXPECT_FALSE(original.Borrow(&other));
    ASSERT_TRUE(first.Cancel(a));
    ASSERT_TRUE(second.Cancel(b));
    EXPECT_FALSE(second.Retire(b, 0, &original));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(other.Valid());
    ASSERT_TRUE(first.Retire(a, 0, &original));
    ASSERT_TRUE(second.Retire(b, 0, &other));
    a = {};
    ASSERT_TRUE(first.Reserve(7, {1, 0, 0, 1}, &a));
    EXPECT_FALSE(first.IsOriginalAttemptLifetimePinFor(original, a, 7));
    ASSERT_TRUE(first.Cancel(a));
    ASSERT_TRUE(first.Retire(a));
}

TEST(ImmediateActionLane, OriginalAttemptMovesAndDropReturnOnlyTheirGenuineBorrow) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 1, 1, 64}));
    harken::ImmediateLaneHandle a{}, b{};
    harken::OriginalImmediateLaneAttemptLifetimePin first, second;
    ASSERT_TRUE(lane.Reserve(1, {1, 0, 0, 1}, &a, &first));
    ASSERT_TRUE(lane.Reserve(2, {1, 0, 0, 1}, &b, &second));
    {
        harken::OriginalImmediateLaneAttemptLifetimePin x, y;
        ASSERT_TRUE(first.Borrow(&x));
        ASSERT_TRUE(second.Borrow(&y));
        harken::OriginalImmediateLaneAttemptLifetimePin moved(std::move(x));
        EXPECT_FALSE(x.Valid());
        moved = std::move(y);
        EXPECT_FALSE(y.Valid());
        EXPECT_TRUE(lane.IsOriginalAttemptLifetimePinFor(moved, b, 2));
        ASSERT_TRUE(lane.Cancel(a));
        ASSERT_TRUE(lane.Retire(a, 0, &first));
        ASSERT_TRUE(lane.Cancel(b));
        EXPECT_FALSE(lane.Retire(b, 0, &second));
    }
    ASSERT_TRUE(lane.Retire(b, 0, &second));
}

TEST(ImmediateActionLane, ExactPrivatePlacementNeverSlidesOrDestroysThePriorPrivateCandidate) {
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({2, 1, 1, 64}));
    harken::ImmediateLaneHandle first{}, second{};
    const harken::TimelineProgramView program{};
    const auto event = Event(0, &program, 0);
    ASSERT_TRUE(lane.Reserve(1, 1, &first));
    ASSERT_TRUE(lane.Reserve(2, 1, &second));
    ASSERT_TRUE(lane.Prepare(first, &event, 1));
    ASSERT_TRUE(lane.Prepare(second, &event, 1));
    ASSERT_TRUE(lane.TryPlaceExactPrivate(first, 64));
    ASSERT_TRUE(lane.TryPlaceExactPrivate(second, 128));
    EXPECT_FALSE(lane.TryPlaceExactPrivate(second, 64));
    std::uint64_t actualLastQuantum = UINT64_MAX;
    ASSERT_TRUE(lane.CapturePrivatePlacedLastQuantum(second, &actualLastQuantum));
    EXPECT_EQ(2u, actualLastQuantum);
    ASSERT_TRUE(lane.Publish(first));
    ASSERT_TRUE(lane.Publish(second));
    for (const auto handle : {first, second}) {
        const auto quantum = handle.slot == first.slot ? 1u : 2u;
        const auto* bucket = lane.CaptureDue(handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_NE(nullptr, lane.Events(handle.slot, bucket));
        EXPECT_EQ(quantum * 64u, lane.Events(handle.slot, bucket)[0].event.executeFrame);
        ASSERT_TRUE(lane.Claim(handle, bucket));
        ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
        ASSERT_TRUE(lane.Retire(handle));
    }
}
