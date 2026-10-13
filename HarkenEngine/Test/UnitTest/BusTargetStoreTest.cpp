// Bus combine storage: per bus x bus target, {authored, API direct,
// Variable bindings, MeterProbe} reduce in that order onto the target's
// neutral base in the declared domain. These tests pin the neutral base,
// the API-slot-only equivalence with the direct absolute-dB set
// semantics, the commutative in-domain accumulation that fills the
// Variable slot, the reduce order, per-bus/per-slot independence, and
// the inert handling of out-of-range addresses. The store is exercised
// standalone here; the live bus-volume publication over it is covered by
// the meter/sidechain suites.
#include "harken/param/BusTargetStore.h"

#include <cmath>
#include <gtest/gtest.h>

namespace {

using harken::BusTargetStore;
using harken::CombineDomain;
using harken::CombineValue;

TEST(BusTargetStore, FreshStoreReducesToNeutralBaseOnEveryBus) {
    BusTargetStore store(4);
    for (std::uint32_t bus = 0; bus < 4; ++bus) {
        EXPECT_EQ(0.0f, store.Reduce(bus, harken::kBusTargetBusVolume))
            << "bus " << bus;
        EXPECT_FALSE(store.SlotActive(bus, harken::kBusTargetBusVolume,
                                      BusTargetStore::kSlotAuthoredIndex));
        EXPECT_FALSE(store.SlotActive(bus, harken::kBusTargetBusVolume,
                                      BusTargetStore::kSlotApiDirectIndex));
        EXPECT_FALSE(store.SlotActive(bus, harken::kBusTargetBusVolume,
                                      BusTargetStore::kSlotVariableIndex));
    }
}

TEST(BusTargetStore, ApiDirectSlotAloneMatchesDirectSetSemantics) {
    // With only the API-direct slot occupied, the reduced value IS the set
    // value: neutral 0 dB plus the direct contribution in add-in-dB. This
    // is the two-slot model's equivalence with the direct bus-volume set.
    BusTargetStore store(2);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -6.0f);
    EXPECT_FLOAT_EQ(-6.0f, store.Reduce(0, harken::kBusTargetBusVolume));

    // Retargeting replaces the slot, never accumulates into it.
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -2.0f);
    EXPECT_FLOAT_EQ(-2.0f, store.Reduce(0, harken::kBusTargetBusVolume));

    // Clearing returns the bus to the neutral base.
    store.ClearSlot(0, harken::kBusTargetBusVolume,
                    BusTargetStore::kSlotApiDirectIndex);
    EXPECT_EQ(0.0f, store.Reduce(0, harken::kBusTargetBusVolume));
}

TEST(BusTargetStore, VariableSlotAccumulatesInDbCommutatively) {
    // Multiple bindings on one bus target accumulate in-domain inside the
    // Variable slot: dB offsets sum, and the sum is order-free. The fold
    // is CombineValue; the store receives the accumulated slot value.
    const float contributions[] = {-4.0f, -6.0f, 2.0f};
    float forward = 0.0f;
    for (float c : contributions) {
        forward = CombineValue(CombineDomain::AddDb, forward, c);
    }
    float backward = 0.0f;
    for (int i = 2; i >= 0; --i) {
        backward =
            CombineValue(CombineDomain::AddDb, backward, contributions[i]);
    }
    EXPECT_FLOAT_EQ(forward, backward);
    EXPECT_FLOAT_EQ(-8.0f, forward);

    BusTargetStore store(2);
    store.SetSlot(1, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotVariableIndex, forward);
    EXPECT_FLOAT_EQ(-8.0f, store.Reduce(1, harken::kBusTargetBusVolume));
}

TEST(BusTargetStore, AuthoredSlotReducesBeforeApiDirectAndVariable) {
    BusTargetStore store(2);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotAuthoredIndex, -7.5f);
    EXPECT_FLOAT_EQ(-7.5f, store.Reduce(0, harken::kBusTargetBusVolume));

    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -6.0f);
    EXPECT_FLOAT_EQ(-13.5f, store.Reduce(0, harken::kBusTargetBusVolume));

    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotVariableIndex, -4.0f);
    EXPECT_FLOAT_EQ(-17.5f, store.Reduce(0, harken::kBusTargetBusVolume));

    store.ClearSlot(0, harken::kBusTargetBusVolume,
                    BusTargetStore::kSlotAuthoredIndex);
    EXPECT_FLOAT_EQ(-10.0f, store.Reduce(0, harken::kBusTargetBusVolume));
}

TEST(BusTargetStore, MarkDroppedKeepsLastCommittedOnPoisonedFlush) {
    BusTargetStore store(1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotAuthoredIndex, 3.0e38f);
    store.MarkCommitted(kBus, kTarget, 3.0e38f, 0);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, 3.0e38f);
    store.MarkVariableChanged(kBus, kTarget, 17);
    std::uint32_t visits = 0;
    float flushed = 0.0f;
    store.FlushDirty([&](std::uint32_t, std::uint32_t, float resolved,
                         std::uint32_t) {
        ++visits;
        flushed = resolved;
        return false;
    });
    EXPECT_EQ(1u, visits);
    EXPECT_FALSE(std::isfinite(flushed));
    store.MarkDropped(kBus, kTarget);
    EXPECT_FALSE(store.PendingPublication(kBus, kTarget));
    EXPECT_FLOAT_EQ(3.0e38f, store.LastCommitted(kBus, kTarget));
    EXPECT_TRUE(store.SlotActive(kBus, kTarget,
                                 BusTargetStore::kSlotVariableIndex));
}

TEST(BusTargetStore, MeterProbeOverflowDoesNotWriteSlot) {
    BusTargetStore store(1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kGeneration = 4;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.InitializeBusGeneration(kBus, kGeneration);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotAuthoredIndex, 3.0e38f);
    store.MarkCommitted(kBus, kTarget, 3.0e38f, 0);
    store.BeginMeterProbeSweep();
    store.AccumulateMeterProbe(kBus, kGeneration, kTarget, 3.0e38f, true, 17);
    store.EndMeterProbeSweep();
    EXPECT_FALSE(store.SlotActive(kBus, kTarget,
                                  BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FALSE(store.PendingPublication(kBus, kTarget));
    EXPECT_TRUE(std::isfinite(store.Reduce(kBus, kTarget)));
    EXPECT_FLOAT_EQ(3.0e38f, store.LastCommitted(kBus, kTarget));
}

TEST(BusTargetStore, ReduceWithSlotDoesNotMutateStoredSlots) {
    BusTargetStore store(2);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotAuthoredIndex, 3.0e38f);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -1.0f);
    EXPECT_FALSE(std::isfinite(store.ReduceWithSlot(
        0, harken::kBusTargetBusVolume, BusTargetStore::kSlotApiDirectIndex,
        3.0e38f)));
    EXPECT_TRUE(std::isfinite(store.Reduce(0, harken::kBusTargetBusVolume)));
    EXPECT_FLOAT_EQ(
        3.0e38f, store.SlotValue(0, harken::kBusTargetBusVolume,
                                 BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -1.0f, store.SlotValue(0, harken::kBusTargetBusVolume,
                               BusTargetStore::kSlotApiDirectIndex));
}

TEST(BusTargetStore, TwoSlotsReduceInDeclaredOrderOntoNeutral) {
    BusTargetStore store(2);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -6.0f);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotVariableIndex, -4.0f);
    // neutral 0, API direct -6, Variable bindings -4: add-in-dB gives -10.
    EXPECT_FLOAT_EQ(-10.0f, store.Reduce(0, harken::kBusTargetBusVolume));

    // Each slot detaches independently.
    store.ClearSlot(0, harken::kBusTargetBusVolume,
                    BusTargetStore::kSlotApiDirectIndex);
    EXPECT_FLOAT_EQ(-4.0f, store.Reduce(0, harken::kBusTargetBusVolume));
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -6.0f);
    store.ClearSlot(0, harken::kBusTargetBusVolume,
                    BusTargetStore::kSlotVariableIndex);
    EXPECT_FLOAT_EQ(-6.0f, store.Reduce(0, harken::kBusTargetBusVolume));
}

TEST(BusTargetStore, SlotsAreIndependentPerBus) {
    BusTargetStore store(3);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -3.0f);
    store.SetSlot(2, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotVariableIndex, -9.0f);

    EXPECT_FLOAT_EQ(-3.0f, store.Reduce(0, harken::kBusTargetBusVolume));
    EXPECT_EQ(0.0f, store.Reduce(1, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(-9.0f, store.Reduce(2, harken::kBusTargetBusVolume));
}

TEST(BusTargetStore, OutOfRangeAccessIsInertAndReducesNeutral) {
    BusTargetStore store(2);
    // Writes past the bus, target, or slot range change nothing.
    store.SetSlot(2, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotApiDirectIndex, -6.0f);
    store.SetSlot(0, harken::kBusTargetCount,
                  BusTargetStore::kSlotApiDirectIndex, -6.0f);
    store.SetSlot(0, harken::kBusTargetBusVolume,
                  BusTargetStore::kSlotCountPerTarget, -6.0f);
    EXPECT_EQ(0.0f, store.Reduce(0, harken::kBusTargetBusVolume));
    EXPECT_EQ(0.0f, store.Reduce(1, harken::kBusTargetBusVolume));
    // Reads past the range refuse quietly: inactive, neutral.
    EXPECT_FALSE(store.SlotActive(2, harken::kBusTargetBusVolume,
                                  BusTargetStore::kSlotApiDirectIndex));
    EXPECT_EQ(0.0f, store.Reduce(2, harken::kBusTargetBusVolume));
    EXPECT_EQ(0.0f, store.Reduce(0, harken::kBusTargetCount));
}

TEST(BusTargetStore,
     ClaimedEqualReductionKeepsOrdinaryFlushFromDroppingItsRamp) {
    BusTargetStore store(/*maxBuses=*/1, /*retiringTargetCapacity=*/1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kGeneration = 4;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.InitializeBusGeneration(kBus, kGeneration);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, -3.0f);
    store.MarkVariableChanged(kBus, kTarget, 17);
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision =
        store.TargetRevision(kBus, kGeneration, kTarget);
    ASSERT_TRUE(store.TryClaim(kBus, kGeneration, kTarget, owner, revision));
    float frozenResolved = 0.0f;
    std::uint32_t frozenRampMs = 0;
    std::uint32_t frozenRevision = 0;
    ASSERT_TRUE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, owner, &frozenResolved, &frozenRampMs,
        &frozenRevision));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(17u, frozenRampMs);

    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, 0.0f);
    store.MarkVariableChanged(kBus, kTarget, 73);
    std::uint32_t ordinaryFlushCalls = 0;
    store.FlushDirty([&ordinaryFlushCalls](std::uint32_t, std::uint32_t,
                                           float, std::uint32_t) {
        ++ordinaryFlushCalls;
        return true;
    });
    EXPECT_EQ(0u, ordinaryFlushCalls);
    EXPECT_TRUE(store.PendingPublication(kBus, kTarget));
    ASSERT_TRUE(store.SnapshotForClaim(
        kBus, kGeneration, kTarget, owner, &frozenResolved, &frozenRampMs,
        &frozenRevision));
    EXPECT_FLOAT_EQ(0.0f, frozenResolved);
    EXPECT_EQ(73u, frozenRampMs);

    store.MarkSliceCommitted(kBus, kGeneration, kTarget, owner, -3.0f, 17);
    EXPECT_FALSE(store.ForeignClaim(kBus, kGeneration, kTarget));
    EXPECT_TRUE(store.PendingPublication(kBus, kTarget));
    std::uint32_t staged = 0;
    float stagedResolved = 0.0f;
    std::uint32_t stagedRampMs = 0;
    store.FlushDirty([&](std::uint32_t, std::uint32_t, float resolved,
                         std::uint32_t rampMs) {
        ++staged;
        stagedResolved = resolved;
        stagedRampMs = rampMs;
        return true;
    });
    EXPECT_EQ(1u, staged);
    EXPECT_FLOAT_EQ(0.0f, stagedResolved);
    EXPECT_EQ(73u, stagedRampMs);
}

TEST(BusTargetStore,
     SliceSnapshotConsumesOnlyItsRampAndClaimReleaseRestoresIt) {
    BusTargetStore store(/*maxBuses=*/1, /*retiringTargetCapacity=*/1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kGeneration = 4;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.InitializeBusGeneration(kBus, kGeneration);
    const std::uint32_t revision =
        store.TargetRevision(kBus, kGeneration, kTarget);
    const harken::VariableSliceRef appliedOwner{0, 1};
    ASSERT_TRUE(
        store.TryClaim(kBus, kGeneration, kTarget, appliedOwner, revision));
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, -3.0f);
    store.MarkVariableChanged(kBus, kTarget, 73);
    float frozenResolved = 0.0f;
    std::uint32_t frozenRampMs = 0;
    std::uint32_t frozenRevision = 0;
    ASSERT_TRUE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, appliedOwner, &frozenResolved,
        &frozenRampMs, &frozenRevision));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(73u, frozenRampMs);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, 0.0f);
    store.MarkVariableChanged(kBus, kTarget, 17);
    store.MarkSliceCommitted(kBus, kGeneration, kTarget, appliedOwner,
                             frozenResolved, frozenRampMs);
    float stagedResolved = 0.0f;
    std::uint32_t stagedRampMs = 0;
    store.FlushDirty([&](std::uint32_t, std::uint32_t, float resolved,
                         std::uint32_t rampMs) {
        stagedResolved = resolved;
        stagedRampMs = rampMs;
        return true;
    });
    EXPECT_FLOAT_EQ(0.0f, stagedResolved);
    EXPECT_EQ(17u, stagedRampMs);
    store.MarkCommitted(kBus, kTarget, stagedResolved, stagedRampMs);

    const harken::VariableSliceRef abandonedOwner{1, 1};
    ASSERT_TRUE(store.TryClaim(kBus, kGeneration, kTarget, abandonedOwner,
                               revision));
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, -6.0f);
    store.MarkVariableChanged(kBus, kTarget, 73);
    ASSERT_TRUE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, abandonedOwner, &frozenResolved,
        &frozenRampMs, &frozenRevision));
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, -3.0f);
    store.MarkVariableChanged(kBus, kTarget, 17);
    store.ReleaseClaim(kBus, kGeneration, kTarget, abandonedOwner);
    stagedResolved = 0.0f;
    stagedRampMs = 0;
    store.FlushDirty([&](std::uint32_t, std::uint32_t, float resolved,
                         std::uint32_t rampMs) {
        stagedResolved = resolved;
        stagedRampMs = rampMs;
        return true;
    });
    EXPECT_FLOAT_EQ(-3.0f, stagedResolved);
    EXPECT_EQ(73u, stagedRampMs);
}

TEST(BusTargetStore,
     ClaimInternalBankSyncDoesNotReintroduceTheFrozenRamp) {
    BusTargetStore store(/*maxBuses=*/1, /*retiringTargetCapacity=*/1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kGeneration = 4;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.InitializeBusGeneration(kBus, kGeneration);
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision =
        store.TargetRevision(kBus, kGeneration, kTarget);
    ASSERT_TRUE(
        store.TryClaim(kBus, kGeneration, kTarget, owner, revision));
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, -3.0f);
    store.MarkVariableChanged(kBus, kTarget, 73);
    float frozenResolved = 0.0f;
    std::uint32_t frozenRampMs = 0;
    std::uint32_t frozenRevision = 0;
    ASSERT_TRUE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, owner, &frozenResolved, &frozenRampMs,
        &frozenRevision));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(73u, frozenRampMs);
    EXPECT_FALSE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, owner, &frozenResolved, &frozenRampMs,
        &frozenRevision));

    store.SetClaimRampSuppressed(kBus, kGeneration, kTarget, owner, true);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, 0.0f);
    store.MarkVariableChanged(kBus, kTarget, 73);
    store.SetClaimRampSuppressed(kBus, kGeneration, kTarget, owner, false);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotApiDirectIndex, -1.0f);
    store.MarkVariableChanged(kBus, kTarget, 17);
    store.SetClaimRampSuppressed(kBus, kGeneration, kTarget, owner, true);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotVariableIndex, -3.0f);
    store.MarkVariableChanged(kBus, kTarget, 73);
    store.SetClaimRampSuppressed(kBus, kGeneration, kTarget, owner, false);

    EXPECT_FLOAT_EQ(-4.0f, store.Reduce(kBus, kTarget));
    store.MarkSliceCommitted(kBus, kGeneration, kTarget, owner,
                             frozenResolved, frozenRampMs);
    float stagedResolved = 0.0f;
    std::uint32_t stagedRampMs = 0;
    std::uint32_t staged = 0;
    store.FlushDirty([&](std::uint32_t, std::uint32_t, float resolved,
                         std::uint32_t rampMs) {
        ++staged;
        stagedResolved = resolved;
        stagedRampMs = rampMs;
        return true;
    });
    EXPECT_EQ(1u, staged);
    EXPECT_FLOAT_EQ(-4.0f, stagedResolved);
    EXPECT_EQ(17u, stagedRampMs);
}

TEST(BusTargetStore, RetiringOwnerIsExactAcrossDenseSlotGenerations) {
    BusTargetStore store(/*maxBuses=*/1, /*retiringTargetCapacity=*/1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    constexpr std::uint32_t kOldGeneration = 7;
    constexpr std::uint32_t kNewGeneration = 8;
    const harken::VariableSliceRef oldOwner{0, 1};
    const harken::VariableSliceRef newOwner{1, 1};

    store.InitializeBusGeneration(kBus, kOldGeneration);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotApiDirectIndex, -6.0f);
    const std::uint32_t oldRevision =
        store.TargetRevision(kBus, kOldGeneration, kTarget);
    ASSERT_NE(0u, oldRevision);
    ASSERT_TRUE(store.TryClaim(kBus, kOldGeneration, kTarget, oldOwner,
                               oldRevision));

    store.RetireBusGeneration(kBus, kOldGeneration);
    ASSERT_EQ(1u, store.RetiringTargetCountForTests());
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t revision = 0;
    ASSERT_TRUE(store.SnapshotForClaim(kBus, kOldGeneration, kTarget,
                                       oldOwner, &resolved, &rampMs,
                                       &revision));
    EXPECT_FLOAT_EQ(-6.0f, resolved);
    EXPECT_EQ(oldRevision, revision);

    store.InitializeBusGeneration(kBus, kNewGeneration);
    store.SetSlot(kBus, kTarget, BusTargetStore::kSlotApiDirectIndex, -3.0f);
    const std::uint32_t newRevision =
        store.TargetRevision(kBus, kNewGeneration, kTarget);
    ASSERT_TRUE(store.TryClaim(kBus, kNewGeneration, kTarget, newOwner,
                               newRevision));
    store.ReleaseClaim(kBus, kOldGeneration, kTarget, oldOwner);
    EXPECT_EQ(0u, store.RetiringTargetCountForTests());
    EXPECT_TRUE(store.ForeignClaim(kBus, kNewGeneration, kTarget));
    EXPECT_FLOAT_EQ(-3.0f, store.Reduce(kBus, kTarget));

    store.RetireBusGeneration(kBus, kNewGeneration);
    ASSERT_EQ(1u, store.RetiringTargetCountForTests());
    constexpr std::uint32_t kNewestGeneration = 9;
    const harken::VariableSliceRef newestOwner{2, 1};
    store.InitializeBusGeneration(kBus, kNewestGeneration);
    const std::uint32_t newestRevision =
        store.TargetRevision(kBus, kNewestGeneration, kTarget);
    ASSERT_TRUE(store.TryClaim(kBus, kNewestGeneration, kTarget, newestOwner,
                               newestRevision));
    store.MarkSliceCommitted(kBus, kNewGeneration, kTarget, newOwner, -3.0f,
                             0);
    EXPECT_EQ(0u, store.RetiringTargetCountForTests());
    EXPECT_TRUE(store.ForeignClaim(kBus, kNewestGeneration, kTarget));
    store.ReleaseClaim(kBus, kNewestGeneration, kTarget, newestOwner);
}

TEST(BusTargetStore, RejectCandidateRestoresBaselineInsteadOfMergingPreparedRamp) {
    BusTargetStore store(/*maxBuses=*/1, /*retiringTargetCapacity=*/1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kGeneration = 4;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.InitializeBusGeneration(kBus, kGeneration);
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision =
        store.TargetRevision(kBus, kGeneration, kTarget);
    ASSERT_TRUE(store.TryClaim(kBus, kGeneration, kTarget, owner, revision));
    store.MarkVariableChanged(kBus, kTarget, 20);
    ASSERT_TRUE(store.CaptureCandidateBaseline(kBus, kGeneration, kTarget,
                                               owner));
    store.MarkVariableChanged(kBus, kTarget, 500);
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t capturedRevision = 0;
    ASSERT_TRUE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, owner, &resolved, &rampMs,
        &capturedRevision));
    EXPECT_EQ(500u, rampMs);
    store.ReleaseClaim(kBus, kGeneration, kTarget, owner,
                       harken::ClaimReleaseReason::RejectCandidate);
    EXPECT_EQ(20u, store.PendingRampMs(kBus, kTarget));
}

TEST(BusTargetStore, RetryPreserveStillMergesPreparedRamp) {
    BusTargetStore store(/*maxBuses=*/1, /*retiringTargetCapacity=*/1);
    constexpr std::uint32_t kBus = 0;
    constexpr std::uint32_t kGeneration = 4;
    constexpr std::uint32_t kTarget = harken::kBusTargetBusVolume;
    store.InitializeBusGeneration(kBus, kGeneration);
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision =
        store.TargetRevision(kBus, kGeneration, kTarget);
    ASSERT_TRUE(store.TryClaim(kBus, kGeneration, kTarget, owner, revision));
    store.MarkVariableChanged(kBus, kTarget, 20);
    ASSERT_TRUE(store.CaptureCandidateBaseline(kBus, kGeneration, kTarget,
                                               owner));
    store.MarkVariableChanged(kBus, kTarget, 500);
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t capturedRevision = 0;
    ASSERT_TRUE(store.CaptureSnapshotForClaim(
        kBus, kGeneration, kTarget, owner, &resolved, &rampMs,
        &capturedRevision));
    store.ReleaseClaim(kBus, kGeneration, kTarget, owner,
                       harken::ClaimReleaseReason::RetryPreserve);
    EXPECT_EQ(500u, store.PendingRampMs(kBus, kTarget));
}

} // namespace
