#include "Common/TestUtil.h"
#include "harken/core/VariableSliceTable.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

struct ApplyProbe {
    bool valid = true;
    std::uint32_t validationCount = 0;
    std::uint32_t applyCount = 0;
    std::uint16_t lastOpcode = 0;
};

struct ClaimInvalidationProbe {
    harken::VariableSliceTable* table = nullptr;
    harken::VariableSliceInvalidationResult result =
        harken::VariableSliceInvalidationResult::InvalidReference;
    std::uint32_t callCount = 0;
};

void InvalidateAfterAudioClaim(void* context,
                               harken::VariableSliceRef ref) noexcept {
    ClaimInvalidationProbe& probe =
        *static_cast<ClaimInvalidationProbe*>(context);
    ++probe.callCount;
    probe.result = probe.table->Invalidate(ref, 1u);
}

bool ValidateTarget(void* context,
                    const harken::VariableSliceTarget&) noexcept {
    ApplyProbe& probe = *static_cast<ApplyProbe*>(context);
    ++probe.validationCount;
    return probe.valid;
}

void ApplyCommand(void* context,
                  const harken::VariableSliceTarget& target) noexcept {
    ApplyProbe& probe = *static_cast<ApplyProbe*>(context);
    ++probe.applyCount;
    probe.lastOpcode = target.command.opcode;
}

harken::VariableSliceTarget MakeTarget(std::uint32_t id) {
    harken::VariableSliceTarget target{};
    target.locator.targetId = id;
    target.targetRevision = id + 1;
    target.command.opcode = static_cast<std::uint16_t>(id + 10);
    return target;
}

bool ReserveTargets(harken::VariableSliceTable& table,
                    std::uint32_t targetCount, std::uint64_t ticket,
                    harken::VariableSliceRef* out) {
    return table.Reserve(1, 1, ticket, ticket * 64, ticket, targetCount, out);
}

void FillTargets(harken::VariableSliceTable& table,
                 harken::VariableSliceRef ref, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        ASSERT_TRUE(table.AddTarget(ref, MakeTarget(i)));
    }
}

void PublishAndApply(harken::VariableSliceTable& table,
                     harken::VariableSliceRef ref,
                     std::uint64_t blockSequence) {
    ASSERT_TRUE(table.Publish(ref));
    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(ref, blockSequence, &probe, ValidateTarget,
                            ApplyCommand));
}

TEST(VariableSliceTable, ReservationProbeSeparatesProofPayloadAndFragmentedSpan) {
    using Status = harken::VariableSliceTable::ReservationStatus;
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(8, 4, 8, 8));
    EXPECT_EQ(0u, table.RowStorageBytes());
    EXPECT_EQ(0u, table.RowCommittedBytes());
    EXPECT_GT(table.RowReservedBytes(), 0u);
    table.SetRowCommitPageLimitForTests(0);
    harken::VariableSliceRef refused{};
    EXPECT_FALSE(ReserveTargets(table, 2, 1, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(8u, table.FreeRowCount());
    EXPECT_EQ(8u, table.FreePayloadCount());
    EXPECT_EQ(8u, table.FreeTargetCapacity());
    table.SetRowCommitPageLimitForTests(SIZE_MAX);
    std::array<harken::VariableSliceRef, 4> refs{};
    for (std::uint32_t i = 0; i < refs.size(); ++i) ASSERT_TRUE(ReserveTargets(table, 2, i + 1, &refs[i]));
    ASSERT_TRUE(table.AbandonPrepared(refs[0]));
    ASSERT_TRUE(table.AbandonPrepared(refs[2]));
    EXPECT_EQ(4u, table.FreeTargetCapacity());
    EXPECT_EQ(Status::TargetSpan, table.ReservationAvailability(4));
    EXPECT_EQ(Status::Ready, table.ReservationAvailability(2));
    EXPECT_EQ(Status::TargetLimit, table.ReservationAvailability(5));
    const auto freeProofs = table.FreeRowCount();
    const auto freePayloads = table.FreePayloadCount();
    for (unsigned i = 0; i < 10; ++i) EXPECT_EQ(Status::TargetSpan, table.ReservationAvailability(4));
    EXPECT_EQ(freeProofs, table.FreeRowCount());
    EXPECT_EQ(freePayloads, table.FreePayloadCount());
    ASSERT_TRUE(table.AbandonPrepared(refs[1]));
    EXPECT_EQ(Status::Ready, table.ReservationAvailability(4));
    ASSERT_TRUE(table.Reset(2, 0, 0, 1));
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &refs[0]));
    EXPECT_EQ(Status::PayloadCapacity, table.ReservationAvailability(0));
    ASSERT_TRUE(table.Reset(1, 0, 0, 1));
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &refs[0]));
    EXPECT_EQ(Status::ProofCapacity, table.ReservationAvailability(0));
}

TEST(VariableSliceTable, ResetAllowsDisabledTargetsAndRejectsHardLimits) {
    harken::VariableSliceTable table;
    EXPECT_FALSE(table.Reset(0, 1, 1));
    EXPECT_FALSE(table.Reset(1, harken::kVariableSliceTargetHardLimit + 1, 1));
    EXPECT_FALSE(
        table.Reset(1, 1, harken::kVariableSliceTargetPoolHardLimit + 1));
    EXPECT_EQ(0u, table.Capacity());
    ASSERT_TRUE(table.Reset(1, 0, 0));
    EXPECT_EQ(0u, table.MaxTargetsPerSlice());
    EXPECT_EQ(0u, table.TargetPoolCapacity());
    EXPECT_EQ(0u, table.FreeTargetCapacity());
    harken::VariableSliceRef disabled{};
    EXPECT_FALSE(ReserveTargets(table, 1, 1, &disabled));
    EXPECT_FALSE(disabled.Valid());
    ASSERT_TRUE(table.Reset(1, 0, 1));
    EXPECT_FALSE(ReserveTargets(table, 1, 2, &disabled));
    ASSERT_TRUE(table.Reset(1, 1, 0));
    EXPECT_FALSE(ReserveTargets(table, 1, 3, &disabled));
    EXPECT_TRUE(table.Reset(1, harken::kVariableSliceTargetHardLimit, 1));
    EXPECT_TRUE(
        table.Reset(1, 1, harken::kVariableSliceTargetPoolHardLimit));
    EXPECT_TRUE(table.Reset(1, harken::kVariableSliceTargetHardLimit,
                            harken::kVariableSliceTargetPoolHardLimit));
}

TEST(VariableSliceTable, SourcesAndStableProofSlotsCrossFormerFixedLimits) {
    constexpr std::uint32_t capacity = 5600;
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(capacity, 0, 0));
    std::vector<harken::VariableSliceRef> rows(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(ReserveTargets(table, 0, i + 1, &rows[i]));
    }
    for (std::uint32_t i = 0; i + 1 < capacity; ++i) {
        harken::VariableSliceSource source{};
        ASSERT_TRUE(table.CaptureSource(rows[i], &source));
        ASSERT_TRUE(table.AddSource(rows.back(), source));
        ASSERT_TRUE(table.Publish(rows[i]));
    }
    EXPECT_EQ(capacity - 1, table.SourceCount(rows.back()));
    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i + 1 < capacity; ++i) {
        const auto* source = table.NextSource(rows.back(), &cursor);
        ASSERT_NE(nullptr, source);
        EXPECT_EQ(rows[i].slot, source->slice.slot);
        EXPECT_EQ(rows[i].generation, source->slice.generation);
        EXPECT_EQ(i + 1, source->ticket);
        EXPECT_EQ(1u, table.SourceObserverCount(rows[i]));
    }
    EXPECT_EQ(nullptr, table.NextSource(rows.back(), &cursor));
    ASSERT_TRUE(table.ClearPreparedSources(rows.back()));
    for (std::uint32_t i = 0; i + 1 < capacity; ++i)
        EXPECT_EQ(0u, table.SourceObserverCount(rows[i]));
    EXPECT_EQ(0u, table.SourceCount(rows.back()));
}

TEST(VariableSliceTable, BankOnlySliceRequiresActualConsumptionAndDoesNotReserveTargets) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 0, 0));
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &ref));
    EXPECT_EQ(1u, table.FreeRowCount());
    EXPECT_EQ(0u, table.FreeTargetCapacity());
    ASSERT_TRUE(table.Publish(ref));
    EXPECT_EQ(harken::VariableSliceState::PageCommitted, table.State(ref));
    EXPECT_EQ(0u, table.AppliedBlockSequence(ref));
    EXPECT_FALSE(table.Reclaim(ref, 100));
    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(ref, 3, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, table.State(ref));
    EXPECT_EQ(3u, table.AppliedBlockSequence(ref));
    EXPECT_EQ(0u, probe.validationCount);
    EXPECT_EQ(0u, probe.applyCount);
    EXPECT_FALSE(table.Reclaim(ref, 3));
    EXPECT_TRUE(table.Reclaim(ref, 4));
    EXPECT_EQ(2u, table.FreeRowCount());
}

TEST(VariableSliceTable, WaitingSourceKeepsDependentUnclaimedUntilSourceActuallyApplies) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 1, 1));
    harken::VariableSliceRef source{}, dependent{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &source));
    ASSERT_TRUE(ReserveTargets(table, 1, 2, &dependent));
    FillTargets(table, dependent, 1);
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    ASSERT_TRUE(table.Publish(source));
    ASSERT_TRUE(table.Publish(dependent));
    ApplyProbe probe{};
    EXPECT_FALSE(table.Apply(dependent, 4, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(harken::VariableSliceState::PageCommitted, table.State(dependent));
    EXPECT_EQ(0u, table.AppliedBlockSequence(dependent));
    EXPECT_EQ(0u, table.InvalidationReasons(dependent));
    EXPECT_EQ(0u, probe.validationCount);
    EXPECT_EQ(0u, probe.applyCount);
    ASSERT_TRUE(table.Apply(source, 5, &probe, ValidateTarget, ApplyCommand));
    EXPECT_TRUE(table.Apply(dependent, 5, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(1u, probe.applyCount);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, table.State(dependent));
    EXPECT_EQ(5u, table.AppliedBlockSequence(dependent));
}

TEST(VariableSliceTable, InstallationFrontiersBothSettleBeforeAnyTargetApplies) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 1, 1));
    for (const bool rawFirst : {false, true}) {
        SCOPED_TRACE(rawFirst);
        harken::VariableSliceRef ref{};
        ASSERT_TRUE(ReserveTargets(table, 1, rawFirst ? 2 : 1, &ref));
        FillTargets(table, ref, 1);
        ASSERT_TRUE(table.SetPreparedCommandFrontier(ref, 5, 9));
        ASSERT_TRUE(table.Publish(ref));
        ApplyProbe probe{};
        EXPECT_FALSE(table.Apply(ref, 10, &probe, ValidateTarget, ApplyCommand, 4, 8));
        EXPECT_FALSE(table.Apply(ref, 11, &probe, ValidateTarget, ApplyCommand,
                                 rawFirst ? 5 : 4, rawFirst ? 8 : 9));
        EXPECT_EQ(harken::VariableSliceState::PageCommitted, table.State(ref));
        EXPECT_EQ(0u, table.AppliedBlockSequence(ref));
        EXPECT_EQ(0u, table.InvalidationReasons(ref));
        EXPECT_EQ(0u, probe.validationCount);
        EXPECT_EQ(0u, probe.applyCount);
        ASSERT_TRUE(table.Apply(ref, 12, &probe, ValidateTarget, ApplyCommand, 5, 9));
        EXPECT_EQ(1u, probe.validationCount);
        EXPECT_EQ(1u, probe.applyCount);
        EXPECT_EQ(12u, table.AppliedBlockSequence(ref));
        EXPECT_FALSE(table.Apply(ref, 13, &probe, ValidateTarget, ApplyCommand, 5, 9));
        EXPECT_EQ(1u, probe.applyCount);
        ASSERT_TRUE(table.Reclaim(ref, 13));
    }
}

TEST(VariableSliceTable, FailedSourceRejectsWholeDependentSliceButIndependentSliceApplies) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(3, 2, 4));
    harken::VariableSliceRef source{}, dependent{}, independent{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &source));
    ASSERT_TRUE(ReserveTargets(table, 2, 2, &dependent));
    ASSERT_TRUE(ReserveTargets(table, 1, 3, &independent));
    FillTargets(table, dependent, 2);
    FillTargets(table, independent, 1);
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    EXPECT_EQ(1u, table.SourceObserverCount(source));
    ASSERT_TRUE(table.Publish(source));
    ASSERT_TRUE(table.Publish(dependent));
    ASSERT_TRUE(table.Publish(independent));
    EXPECT_FALSE(table.SourceApplied(proof));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim, table.Invalidate(source, 1));
    ApplyProbe probe{};
    EXPECT_FALSE(table.Apply(source, 4, &probe, ValidateTarget, ApplyCommand));
    EXPECT_FALSE(table.Apply(dependent, 4, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(0u, probe.validationCount);
    EXPECT_EQ(0u, probe.applyCount);
    EXPECT_EQ(harken::VariableSliceState::Invalidated, table.State(dependent));
    EXPECT_EQ(harken::kVariableSliceSourceRejected, table.InvalidationReasons(dependent));
    EXPECT_EQ(4u, table.AppliedBlockSequence(dependent));
    EXPECT_TRUE(table.Apply(independent, 4, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(1u, probe.applyCount);
    EXPECT_FALSE(table.Reclaim(source, 5));
    EXPECT_TRUE(table.Reclaim(dependent, 5));
    EXPECT_EQ(0u, table.SourceObserverCount(source));
    EXPECT_TRUE(table.Reclaim(source, 5));
    EXPECT_FALSE(table.SourceApplied(proof));
    EXPECT_EQ(4u, table.FreeTargetCapacity() + table.TargetCount(independent));
}

TEST(VariableSliceTable, AppliedSourceStaysPinnedUntilDependentReclaimAndRejectsWrongIdentity) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 1, 1));
    harken::VariableSliceRef source{}, dependent{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &source));
    ASSERT_TRUE(ReserveTargets(table, 1, 2, &dependent));
    FillTargets(table, dependent, 1);
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    auto wrong = proof;
    ++wrong.ticket;
    EXPECT_FALSE(table.AddSource(dependent, wrong));
    ++wrong.transport;
    EXPECT_FALSE(table.RetainSource(wrong));
    EXPECT_FALSE(table.AddSource(source, proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    ASSERT_TRUE(table.Publish(source));
    ASSERT_TRUE(table.Publish(dependent));
    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(source, 3, &probe, ValidateTarget, ApplyCommand));
    ASSERT_TRUE(table.SourceApplied(proof));
    EXPECT_FALSE(table.SourceApplied(wrong));
    EXPECT_FALSE(table.Reclaim(source, 4));
    ASSERT_TRUE(table.Apply(dependent, 4, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(1u, probe.applyCount);
    EXPECT_FALSE(table.Reclaim(dependent, 4));
    EXPECT_TRUE(table.Reclaim(dependent, 5));
    EXPECT_TRUE(table.Reclaim(source, 5));
    EXPECT_FALSE(table.ReleaseSource(proof));
    EXPECT_EQ(2u, table.FreeRowCount());
}

TEST(VariableSliceTable, FinalPlacementRechecksEverySourceBeforePublication) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 0, 0));
    harken::VariableSliceRef source{}, dependent{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &source));
    ASSERT_TRUE(ReserveTargets(table, 0, 2, &dependent));
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    EXPECT_FALSE(table.Publish(dependent));
    ASSERT_TRUE(table.SetPreparedEffectiveFrame(source, 256));
    ASSERT_TRUE(table.Publish(source));
    EXPECT_FALSE(table.SetPreparedEffectiveFrame(dependent, 192));
    EXPECT_FALSE(table.Publish(dependent));
    ASSERT_TRUE(table.SetPreparedEffectiveFrame(dependent, 256));
    ASSERT_TRUE(table.Publish(dependent));
    EXPECT_EQ(256u, table.EffectiveFrame(dependent));
    EXPECT_EQ(1u, table.SourceCount(dependent));
    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(source, 5, &probe, ValidateTarget, ApplyCommand));
    ASSERT_TRUE(table.Apply(dependent, 5, &probe, ValidateTarget, ApplyCommand));
    EXPECT_FALSE(table.Reclaim(source, 6));
    ASSERT_TRUE(table.Reclaim(dependent, 6));
    ASSERT_TRUE(table.Reclaim(source, 6));
}

TEST(VariableSliceTable, AbandonedDependentReleasesOnlyItsSourcePin) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(3, 0, 0));
    harken::VariableSliceRef source{}, a{}, b{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &source));
    ASSERT_TRUE(ReserveTargets(table, 0, 2, &a));
    ASSERT_TRUE(ReserveTargets(table, 0, 3, &b));
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.AddSource(a, proof));
    ASSERT_TRUE(table.AddSource(b, proof));
    EXPECT_FALSE(table.AbandonPrepared(source));
    ASSERT_TRUE(table.AbandonPrepared(a));
    EXPECT_EQ(1u, table.SourceObserverCount(source));
    ASSERT_TRUE(table.AbandonPrepared(b));
    EXPECT_EQ(0u, table.SourceObserverCount(source));
    EXPECT_TRUE(table.AbandonPrepared(source));
}

TEST(VariableSliceTable, QuarantinedDependentReleasesSourceAfterItsConsumptionFence) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 1, 1));
    ASSERT_TRUE(table.SetFreeRowGenerationForTest(1, UINT32_MAX));
    harken::VariableSliceRef source{}, dependent{};
    ASSERT_TRUE(ReserveTargets(table, 0, 1, &source));
    ASSERT_TRUE(ReserveTargets(table, 1, 2, &dependent));
    ASSERT_EQ(UINT32_MAX, dependent.generation);
    FillTargets(table, dependent, 1);
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    ASSERT_TRUE(table.Publish(source));
    ASSERT_TRUE(table.Publish(dependent));
    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(source, 3, &probe, ValidateTarget, ApplyCommand));
    ASSERT_TRUE(table.Apply(dependent, 3, &probe, ValidateTarget, ApplyCommand));
    EXPECT_FALSE(table.Reclaim(dependent, 3));
    EXPECT_EQ(1u, table.SourceObserverCount(source));
    EXPECT_FALSE(table.Reclaim(dependent, 4));
    EXPECT_EQ(harken::VariableSliceState::Quarantined, table.State(dependent));
    EXPECT_EQ(0u, table.SourceObserverCount(source));
    EXPECT_TRUE(table.Reclaim(source, 4));
    EXPECT_EQ(1u, table.FreeTargetCapacity());
    EXPECT_FALSE(table.HasUnreclaimedWorldRows(1));
    EXPECT_FALSE(table.HasUnreclaimedTransportRows(1));
}

TEST(VariableSliceTable, EngineUsesTheInternalDefaultSharedPool) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(1, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NE(nullptr, session.Engine());
    EXPECT_EQ(harken::kMaxVariableSliceTargetsTotalDefault,
              session.Engine()->variableSlices.TargetPoolCapacity());
    EXPECT_EQ(harken::kMaxPropertiesPerCurrentSliceDefault,
              session.Engine()->variableSlices.MaxTargetsPerSlice());
    EXPECT_GE(session.Engine()->mixer.CurrentSliceScratchCapacityForTest(),
              static_cast<std::size_t>(
                  session.Engine()->config.maxEventsPerQuantum) *
                  2u);
}

TEST(VariableSliceTable, EngineAllowsAnExplicitDisabledInternalProfile) {
    harken::EngineConfig config{};
    config.maxPropertiesPerCurrentSlice = 0;
    config.maxVariableSliceTargetsTotal = 0;
    const std::unique_ptr<harken::Engine> engine =
        std::make_unique<harken::Engine>(config);
    EXPECT_EQ(0u, engine->variableSlices.MaxTargetsPerSlice());
    EXPECT_EQ(0u, engine->variableSlices.TargetPoolCapacity());

    harken::VariableSliceRef ref{};
    EXPECT_TRUE(engine->variableSlices.Reserve(1, 1, 1, 64, 1, 0, &ref));
    EXPECT_TRUE(ref.Valid());
    EXPECT_TRUE(engine->variableSlices.AbandonPrepared(ref));
}

TEST(VariableSliceTable, WorldQueryTracksPhysicalRowsAndIgnoresQuarantine) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 1, 2));
    harken::VariableSliceRef prepared{};
    ASSERT_TRUE(table.Reserve(41, 1, 1, 64, 1, 1, &prepared));
    EXPECT_TRUE(table.HasUnreclaimedWorldRows(41));
    EXPECT_FALSE(table.HasUnreclaimedWorldRows(42));
    ASSERT_TRUE(table.AbandonPrepared(prepared));
    EXPECT_FALSE(table.HasUnreclaimedWorldRows(41));

    harken::VariableSliceRef applied{};
    ASSERT_TRUE(table.Reserve(42, 1, 2, 128, 2, 1, &applied));
    FillTargets(table, applied, 1);
    PublishAndApply(table, applied, 9);
    EXPECT_TRUE(table.HasUnreclaimedWorldRows(42));
    EXPECT_FALSE(table.Reclaim(applied, 9));
    EXPECT_TRUE(table.HasUnreclaimedWorldRows(42));
    EXPECT_TRUE(table.Reclaim(applied, 10));
    EXPECT_FALSE(table.HasUnreclaimedWorldRows(42));

    ASSERT_TRUE(table.SetFreeRowGenerationForTest(0, UINT32_MAX));
    harken::VariableSliceRef quarantined{};
    ASSERT_TRUE(table.Reserve(43, 1, 3, 192, 3, 1, &quarantined));
    FillTargets(table, quarantined, 1);
    PublishAndApply(table, quarantined, 11);
    EXPECT_FALSE(table.Reclaim(quarantined, 12));
    EXPECT_EQ(harken::VariableSliceState::Quarantined,
              table.State(quarantined));
    EXPECT_FALSE(table.HasUnreclaimedWorldRows(43));
}

TEST(VariableSliceTable, AggregatePoolStopsAtExactCapacityWithoutSideEffects) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(4, 100, 100));
    EXPECT_EQ(100u, table.FreeTargetCapacity());

    harken::VariableSliceRef first{};
    ASSERT_TRUE(ReserveTargets(table, 95, 1, &first));
    EXPECT_EQ(5u, table.FreeTargetCapacity());

    harken::VariableSliceRef second{};
    ASSERT_TRUE(ReserveTargets(table, 5, 2, &second));
    EXPECT_EQ(0u, table.FreeTargetCapacity());
    const std::uint32_t rowsBefore = table.FreeRowCount();
    const std::size_t rangesBefore = table.FreeRangeCount();

    harken::VariableSliceRef refused{};
    EXPECT_FALSE(ReserveTargets(table, 1, 3, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(0u, table.FreeTargetCapacity());
    EXPECT_EQ(rowsBefore, table.FreeRowCount());
    EXPECT_EQ(rangesBefore, table.FreeRangeCount());
    EXPECT_EQ(harken::VariableSliceState::Prepared, table.State(first));
    EXPECT_EQ(harken::VariableSliceState::Prepared, table.State(second));

    EXPECT_TRUE(table.AbandonPrepared(first));
    EXPECT_TRUE(table.AbandonPrepared(second));
    EXPECT_EQ(100u, table.FreeTargetCapacity());
    EXPECT_EQ(1u, table.FreeRangeCount());
}

TEST(VariableSliceTable, RowShortageDoesNotConsumeAnAvailableSpan) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 1, 2));
    harken::VariableSliceRef first{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &first));
    ASSERT_EQ(1u, table.FreeTargetCapacity());

    harken::VariableSliceRef refused{};
    EXPECT_FALSE(ReserveTargets(table, 1, 2, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(1u, table.FreeTargetCapacity());
    EXPECT_EQ(0u, table.FreeRowCount());
    EXPECT_EQ(1u, table.FreeRangeCount());
}

TEST(VariableSliceTable, FragmentationBackpressuresUntilAdjacentRangesMerge) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(4, 6, 12));
    harken::VariableSliceRef first{};
    harken::VariableSliceRef middle{};
    harken::VariableSliceRef last{};
    ASSERT_TRUE(ReserveTargets(table, 4, 1, &first));
    ASSERT_TRUE(ReserveTargets(table, 4, 2, &middle));
    ASSERT_TRUE(ReserveTargets(table, 4, 3, &last));
    ASSERT_TRUE(table.AbandonPrepared(first));
    ASSERT_TRUE(table.AbandonPrepared(last));
    ASSERT_EQ(8u, table.FreeTargetCapacity());
    ASSERT_EQ(2u, table.FreeRangeCount());

    const std::uint32_t rowsBefore = table.FreeRowCount();
    harken::VariableSliceRef fragmented{};
    EXPECT_FALSE(ReserveTargets(table, 6, 4, &fragmented));
    EXPECT_FALSE(fragmented.Valid());
    EXPECT_EQ(8u, table.FreeTargetCapacity());
    EXPECT_EQ(rowsBefore, table.FreeRowCount());
    EXPECT_EQ(2u, table.FreeRangeCount());

    ASSERT_TRUE(table.AbandonPrepared(middle));
    ASSERT_EQ(12u, table.FreeTargetCapacity());
    ASSERT_EQ(1u, table.FreeRangeCount());
    EXPECT_TRUE(ReserveTargets(table, 6, 5, &fragmented));
    EXPECT_EQ(6u, table.FreeTargetCapacity());
}

TEST(VariableSliceTable, PublishRequiresExactFillAndAddNeverCrossesTheSpan) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 3, 3));
    const std::uint32_t rowsBefore = table.FreeRowCount();
    const std::uint32_t targetsBefore = table.FreeTargetCapacity();
    harken::VariableSliceRef refused{};
    EXPECT_FALSE(ReserveTargets(table, 4, 1, &refused));
    EXPECT_EQ(rowsBefore, table.FreeRowCount());
    EXPECT_EQ(targetsBefore, table.FreeTargetCapacity());

    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 3, 2, &ref));
    ASSERT_TRUE(table.AddTarget(ref, MakeTarget(0)));
    ASSERT_TRUE(table.AddTarget(ref, MakeTarget(1)));
    EXPECT_FALSE(table.Publish(ref));
    ASSERT_TRUE(table.AddTarget(ref, MakeTarget(2)));
    EXPECT_FALSE(table.AddTarget(ref, MakeTarget(3)));
    EXPECT_EQ(3u, table.TargetCount(ref));
    ASSERT_TRUE(table.Publish(ref));
    EXPECT_EQ(nullptr, table.Target(ref, 0));
    const harken::VariableSliceTable& immutable = table;
    EXPECT_NE(nullptr, immutable.Target(ref, 0));
    EXPECT_EQ(nullptr, immutable.Target(ref, 3));
}

TEST(VariableSliceTable, AppliedFenceRequiresStrictlyLaterObservation) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 1, 1));
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &ref));
    FillTargets(table, ref, 1);
    PublishAndApply(table, ref, 10);
    EXPECT_EQ(10u, table.AppliedBlockSequence(ref));
    EXPECT_FALSE(table.Reclaim(ref, 10));
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, table.State(ref));
    EXPECT_TRUE(table.Reclaim(ref, 11));
    EXPECT_EQ(1u, table.FreeTargetCapacity());
}

TEST(VariableSliceTable, ValidationFailureUsesTheSameStrictTombstoneFence) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 1, 1));
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &ref));
    FillTargets(table, ref, 1);
    ASSERT_TRUE(table.Publish(ref));
    ApplyProbe probe{};
    probe.valid = false;
    EXPECT_FALSE(table.Apply(ref, 20, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(1u, probe.validationCount);
    EXPECT_EQ(0u, probe.applyCount);
    EXPECT_EQ(20u, table.AppliedBlockSequence(ref));
    EXPECT_EQ(harken::VariableSliceState::Invalidated, table.State(ref));
    EXPECT_FALSE(table.Reclaim(ref, 20));
    EXPECT_TRUE(table.Reclaim(ref, 21));
}

TEST(VariableSliceTable, InvalidatedPageWaitsForAudioTombstoneAck) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 1, 1));
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &ref));
    FillTargets(table, ref, 1);
    ASSERT_TRUE(table.Publish(ref));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim,
              table.Invalidate(ref, 1));
    EXPECT_EQ(harken::VariableSliceInvalidationResult::AlreadyInvalidated,
              table.Invalidate(ref, 2));
    EXPECT_EQ(harken::VariableSliceState::Invalidated, table.State(ref));
    EXPECT_EQ(0u, table.AppliedBlockSequence(ref));
    EXPECT_FALSE(table.Reclaim(ref, UINT64_MAX));

    ApplyProbe probe{};
    EXPECT_FALSE(table.Apply(ref, 30, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(0u, probe.validationCount);
    EXPECT_EQ(0u, probe.applyCount);
    EXPECT_EQ(30u, table.AppliedBlockSequence(ref));
    EXPECT_FALSE(table.Reclaim(ref, 30));
    EXPECT_TRUE(table.Reclaim(ref, 31));
}

TEST(VariableSliceTable, InvalidationAfterAudioClaimAppliesTheWholeSlice) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 2, 2));
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 2, 1, &ref));
    FillTargets(table, ref, 2);
    ASSERT_TRUE(table.Publish(ref));

    ClaimInvalidationProbe invalidation{&table};
    table.SetAfterAudioClaimHookForTest(InvalidateAfterAudioClaim,
                                        &invalidation);
    ApplyProbe apply{};
    ASSERT_TRUE(table.Apply(ref, 40, &apply, ValidateTarget, ApplyCommand));
    table.SetAfterAudioClaimHookForTest(nullptr, nullptr);

    EXPECT_EQ(1u, invalidation.callCount);
    EXPECT_EQ(harken::VariableSliceInvalidationResult::AudioClaimedApplyWhole,
              invalidation.result);
    EXPECT_EQ(2u, apply.validationCount);
    EXPECT_EQ(2u, apply.applyCount);
    EXPECT_EQ(harken::VariableSliceState::SliceApplied, table.State(ref));
    EXPECT_EQ(harken::VariableSliceInvalidationResult::AlreadyApplied,
              table.Invalidate(ref, 2u));
    EXPECT_FALSE(table.Reclaim(ref, 40));
    EXPECT_TRUE(table.Reclaim(ref, 41));
}

TEST(VariableSliceTable, StaleReferenceCannotReachAReusedSpan) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(1, 1, 1));
    harken::VariableSliceRef stale{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &stale));
    FillTargets(table, stale, 1);
    PublishAndApply(table, stale, 5);
    ASSERT_TRUE(table.Reclaim(stale, 6));

    harken::VariableSliceRef current{};
    ASSERT_TRUE(ReserveTargets(table, 1, 2, &current));
    ASSERT_EQ(stale.slot, current.slot);
    ASSERT_NE(stale.generation, current.generation);
    FillTargets(table, current, 1);
    EXPECT_EQ(nullptr, table.Target(stale, 0));
    const harken::VariableSliceTable& immutable = table;
    EXPECT_EQ(nullptr, immutable.Target(stale, 0));
    EXPECT_NE(nullptr, immutable.Target(current, 0));
}

TEST(VariableSliceTable, MaximumGenerationQuarantinesProofAndReleasesItsPayload) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 2, 2));
    ASSERT_TRUE(table.SetFreeRowGenerationForTest(0, UINT32_MAX));
    harken::VariableSliceRef pinned{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &pinned));
    ASSERT_EQ(UINT32_MAX, pinned.generation);
    FillTargets(table, pinned, 1);
    PublishAndApply(table, pinned, 7);
    EXPECT_FALSE(table.Reclaim(pinned, 8));
    EXPECT_EQ(harken::VariableSliceState::Quarantined, table.State(pinned));
    EXPECT_EQ(2u, table.FreeTargetCapacity());
    EXPECT_EQ(1u, table.FreeRowCount());

    harken::VariableSliceRef tooWide{};
    ASSERT_TRUE(ReserveTargets(table, 2, 2, &tooWide));
    ASSERT_TRUE(table.AbandonPrepared(tooWide));
    harken::VariableSliceRef remaining{};
    ASSERT_TRUE(ReserveTargets(table, 1, 3, &remaining));
    EXPECT_TRUE(table.AbandonPrepared(remaining));
    EXPECT_EQ(2u, table.FreeTargetCapacity());
    EXPECT_EQ(harken::VariableSliceState::Quarantined, table.State(pinned));
}

TEST(VariableSliceTable, AcknowledgementAndStrictFenceReleasePayloadWhileProofRemainsObserved) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(3, 1, 1, 1));
    harken::VariableSliceRef source{}, next{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &source));
    FillTargets(table, source, 1);
    const auto* originalTarget = static_cast<const harken::VariableSliceTable&>(table).Target(source, 0);
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.RetainSource(proof));
    PublishAndApply(table, source, 1);
    table.ReclaimSettled(2);
    EXPECT_EQ(0u, table.FreePayloadCount());
    EXPECT_EQ(1u, table.TargetCount(source));
    EXPECT_FALSE(ReserveTargets(table, 1, 2, &next));
    ASSERT_TRUE(table.Acknowledge(source));
    table.ReclaimSettled(1);
    EXPECT_EQ(0u, table.FreeTargetCapacity());
    table.ReclaimSettled(2);
    EXPECT_EQ(1u, table.FreePayloadCount());
    EXPECT_EQ(1u, table.FreeTargetCapacity());
    EXPECT_EQ(0u, table.TargetCount(source));
    ASSERT_TRUE(table.SourceApplied(proof));
    ASSERT_TRUE(ReserveTargets(table, 1, 2, &next));
    ASSERT_TRUE(table.AddTarget(next, MakeTarget(99)));
    EXPECT_EQ(originalTarget, static_cast<const harken::VariableSliceTable&>(table).Target(next, 0));
    EXPECT_NE(source.slot, next.slot);
    ASSERT_TRUE(table.AddSource(next, proof));
    PublishAndApply(table, next, 3);
    ASSERT_TRUE(table.Acknowledge(next));
    table.ReclaimSettled(4);
    EXPECT_EQ(1u, table.SourceObserverCount(source));
    EXPECT_TRUE(table.SourceApplied(proof));
    EXPECT_EQ(1u, table.FreePayloadCount());
    ASSERT_TRUE(table.ReleaseSource(proof));
    table.ReclaimSettled(5);
    EXPECT_FALSE(table.SourceApplied(proof));
    EXPECT_EQ(3u, table.FreeRowCount());
    harken::VariableSliceRef reused{};
    ASSERT_TRUE(ReserveTargets(table, 1, 3, &reused));
    EXPECT_EQ(source.slot, reused.slot);
    EXPECT_NE(source.generation, reused.generation);
    EXPECT_FALSE(table.RetainSource(proof));
}

TEST(VariableSliceTable, DependencyAloneRetainsProofAfterAcknowledgedPayloadIsReused) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(3, 1, 1, 2));
    harken::VariableSliceRef source{}, dependent{}, replacement{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &source));
    FillTargets(table, source, 1);
    ASSERT_TRUE(ReserveTargets(table, 0, 2, &dependent));
    harken::VariableSliceSource proof{};
    ASSERT_TRUE(table.CaptureSource(source, &proof));
    ASSERT_TRUE(table.AddSource(dependent, proof));
    ASSERT_TRUE(table.Publish(dependent) == false);
    PublishAndApply(table, source, 1);
    ASSERT_TRUE(table.Publish(dependent));
    ASSERT_TRUE(table.Acknowledge(source));
    table.ReclaimSettled(2);
    ASSERT_TRUE(table.SourceApplied(proof));
    EXPECT_EQ(1u, table.SourceObserverCount(source));
    ASSERT_TRUE(ReserveTargets(table, 1, 3, &replacement));
    FillTargets(table, replacement, 1);
    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(dependent, 3, &probe, ValidateTarget, ApplyCommand));
    ASSERT_TRUE(table.Acknowledge(dependent));
    table.ReclaimSettled(4);
    table.ReclaimSettled(5);
    EXPECT_FALSE(table.SourceApplied(proof));
    EXPECT_TRUE(table.AbandonPrepared(replacement));
    EXPECT_EQ(3u, table.FreeRowCount());
    EXPECT_EQ(2u, table.FreePayloadCount());
    EXPECT_EQ(1u, table.FreeTargetCapacity());
}

TEST(VariableSliceTable, InvalidatedPayloadStillWaitsForActualConsumptionAndFenceAfterAcknowledgement) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 1, 1, 1));
    harken::VariableSliceRef source{}, next{};
    ASSERT_TRUE(ReserveTargets(table, 1, 1, &source));
    FillTargets(table, source, 1);
    ASSERT_TRUE(table.Publish(source));
    ASSERT_EQ(harken::VariableSliceInvalidationResult::InvalidatedBeforeClaim, table.Invalidate(source, 1u));
    ASSERT_TRUE(table.Acknowledge(source));
    table.ReclaimSettled(100);
    EXPECT_FALSE(ReserveTargets(table, 1, 2, &next));
    EXPECT_EQ(0u, table.FreePayloadCount());
    ApplyProbe probe{};
    EXPECT_FALSE(table.Apply(source, 101, &probe, ValidateTarget, ApplyCommand));
    table.ReclaimSettled(101);
    EXPECT_EQ(0u, table.FreePayloadCount());
    table.ReclaimSettled(102);
    EXPECT_EQ(1u, table.FreePayloadCount());
    EXPECT_EQ(1u, table.FreeTargetCapacity());
    EXPECT_EQ(2u, table.FreeRowCount());
}

TEST(VariableSliceTable, AudioApplyDoesNotMutatePoolOrFreeRangeState) {
    harken::VariableSliceTable table;
    ASSERT_TRUE(table.Reset(2, 2, 4));
    harken::VariableSliceRef ref{};
    ASSERT_TRUE(ReserveTargets(table, 2, 1, &ref));
    FillTargets(table, ref, 2);
    ASSERT_TRUE(table.Publish(ref));
    const harken::VariableSliceTable& immutable = table;
    const harken::VariableSliceTarget* firstBefore = immutable.Target(ref, 0);
    ASSERT_NE(nullptr, firstBefore);
    const std::uint32_t freeTargetsBefore = table.FreeTargetCapacity();
    const std::uint32_t freeRowsBefore = table.FreeRowCount();
    const std::size_t freeRangesBefore = table.FreeRangeCount();

    ApplyProbe probe{};
    ASSERT_TRUE(table.Apply(ref, 9, &probe, ValidateTarget, ApplyCommand));
    EXPECT_EQ(2u, probe.validationCount);
    EXPECT_EQ(2u, probe.applyCount);
    EXPECT_EQ(11u, probe.lastOpcode);
    EXPECT_EQ(freeTargetsBefore, table.FreeTargetCapacity());
    EXPECT_EQ(freeRowsBefore, table.FreeRowCount());
    EXPECT_EQ(freeRangesBefore, table.FreeRangeCount());
    EXPECT_EQ(firstBefore, immutable.Target(ref, 0));
}

} // namespace
