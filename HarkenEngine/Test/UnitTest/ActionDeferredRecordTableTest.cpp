#include "Common/ObjectIdTestSupport.h"
#include "harken/core/ActionDeferredRecordTable.h"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <array>
#include <utility>

namespace {

harken::ActionDeferredRecord MakeRecord() {
    harken::ActionDeferredRecord record{};
    record.step.opcode = harken::ActionOpcodeV1::Play;
    record.firing = UINT64_C(0x0000000100000001);
    record.executionOrdinal = 7;
    record.ownerCancellationEpoch = 1;
    record.owner = harken::ActionDeferredOwner::DirectWorld(11);
    record.firstRowFrame = 100;
    record.lastRowFrame = 120;
    record.intendedBoundaryFrame = 100;
    record.deadlineFrame = 200;
    return record;
}

harken::DeferredIntent MakeIntent(
    const harken::ActionDeferredRecord& record,
    harken::ActionDeferredRecordToken recordToken) {
    harken::DeferredIntent intent{};
    intent.ownerDomain = harken::DeferredIntentOwnerDomain::Action;
    intent.ownerHandle = record.firing;
    intent.ownerEpoch = record.ownerCancellationEpoch;
    intent.dueFrame = record.intendedBoundaryFrame;
    intent.payload = recordToken.slot;
    intent.executionOrdinal = record.executionOrdinal;
    return intent;
}

harken::ActionDeferredRecord MakePublicationRecord() {
    auto record = MakeRecord();
    record.step.opcode = harken::ActionOpcodeV1::Stop;
    record.step.authoredEventIndex = 23;
    record.step.target = {harken::ActionTargetKindV1::Segment, 0, harkentest::ObjectId(51), 52};
    record.step.delay = {harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 1000000, 1};
    record.step.fade = {harken::AuthoredTimeDomainV1::MusicalTicks, 480, 1};
    record.step.quantizeReference = {harken::ActionTargetKindV1::Track, 0, harkentest::ObjectId(53), 54};
    record.step.quantizeTiming = harken::ActionQuantizeTimingV1::NextBar;
    record.step.cooldownScope = harken::ActionCooldownScopeV1::PerEmitter;
    record.step.ifSilent = harken::ActionIfSilentV1::Drop;
    record.step.scope = harken::ActionScopeV1::Emitter;
    record.step.cooldown = {harken::AuthoredTimeDomainV1::AbsoluteNanoseconds, 2000000, 1};
    record.step.valueBits = UINT32_C(0x3f800000);
    record.targetPackLease = {UINT64_C(0x0000000100000001), 31, 2, 3, 4, 5, 6, 7, 1, 8};
    return record;
}

static_assert(noexcept(std::declval<const harken::ActionDeferredRecordTable&>().CapturePublicationAt(
    0, nullptr, nullptr, nullptr)));

void ExpectPublicationRecord(const harken::ActionDeferredRecord& expected,
                             const harken::ActionDeferredRecord& actual) {
    EXPECT_EQ(expected.step.opcode, actual.step.opcode);
    EXPECT_EQ(expected.step.authoredEventIndex, actual.step.authoredEventIndex);
    EXPECT_EQ(expected.step.target.kind, actual.step.target.kind);
    EXPECT_EQ(expected.step.target.reserved, actual.step.target.reserved);
    EXPECT_EQ(expected.step.target.stableId, actual.step.target.stableId);
    EXPECT_EQ(expected.step.target.expectedProgramSemanticFingerprint, actual.step.target.expectedProgramSemanticFingerprint);
    EXPECT_EQ(expected.step.quantizeReference.kind, actual.step.quantizeReference.kind);
    EXPECT_EQ(expected.step.quantizeReference.reserved, actual.step.quantizeReference.reserved);
    EXPECT_EQ(expected.step.quantizeReference.stableId, actual.step.quantizeReference.stableId);
    EXPECT_EQ(expected.step.quantizeReference.expectedProgramSemanticFingerprint, actual.step.quantizeReference.expectedProgramSemanticFingerprint);
    EXPECT_EQ(expected.step.delay.domain, actual.step.delay.domain);
    EXPECT_EQ(expected.step.delay.numerator, actual.step.delay.numerator);
    EXPECT_EQ(expected.step.delay.denominator, actual.step.delay.denominator);
    EXPECT_EQ(expected.step.fade.domain, actual.step.fade.domain);
    EXPECT_EQ(expected.step.fade.numerator, actual.step.fade.numerator);
    EXPECT_EQ(expected.step.fade.denominator, actual.step.fade.denominator);
    EXPECT_EQ(expected.step.cooldown.domain, actual.step.cooldown.domain);
    EXPECT_EQ(expected.step.cooldown.numerator, actual.step.cooldown.numerator);
    EXPECT_EQ(expected.step.cooldown.denominator, actual.step.cooldown.denominator);
    EXPECT_EQ(expected.step.quantizeTiming, actual.step.quantizeTiming);
    EXPECT_EQ(expected.step.cooldownScope, actual.step.cooldownScope);
    EXPECT_EQ(expected.step.ifSilent, actual.step.ifSilent);
    EXPECT_EQ(expected.step.scope, actual.step.scope);
    EXPECT_EQ(expected.step.valueBits, actual.step.valueBits);
    EXPECT_EQ(expected.step.reserved0, actual.step.reserved0);
    EXPECT_EQ(expected.targetPackLease.handle, actual.targetPackLease.handle);
    EXPECT_EQ(expected.targetPackLease.packStableId, actual.targetPackLease.packStableId);
    EXPECT_EQ(expected.targetPackLease.contentRevision, actual.targetPackLease.contentRevision);
    EXPECT_EQ(expected.targetPackLease.partitionRevision, actual.targetPackLease.partitionRevision);
    EXPECT_EQ(expected.targetPackLease.resourceRevision, actual.targetPackLease.resourceRevision);
    EXPECT_EQ(expected.targetPackLease.resourceToken, actual.targetPackLease.resourceToken);
    EXPECT_EQ(expected.targetPackLease.resourceClosureToken, actual.targetPackLease.resourceClosureToken);
    EXPECT_EQ(expected.targetPackLease.compositeGeneration, actual.targetPackLease.compositeGeneration);
    EXPECT_EQ(expected.targetPackLease.slotGeneration, actual.targetPackLease.slotGeneration);
    EXPECT_EQ(expected.targetPackLease.leaseToken, actual.targetPackLease.leaseToken);
    EXPECT_EQ(expected.firing, actual.firing);
    EXPECT_EQ(expected.stepIndex, actual.stepIndex);
    EXPECT_EQ(expected.executionOrdinal, actual.executionOrdinal);
    EXPECT_EQ(expected.depth, actual.depth);
    EXPECT_EQ(expected.ownerActionStableId, actual.ownerActionStableId);
    EXPECT_EQ(expected.rootActionStableId, actual.rootActionStableId);
    EXPECT_EQ(expected.rootActionProgramSemanticFingerprint, actual.rootActionProgramSemanticFingerprint);
    EXPECT_EQ(expected.callerSeed, actual.callerSeed);
    EXPECT_EQ(expected.emitter, actual.emitter);
    EXPECT_EQ(expected.ownerCancellationEpoch, actual.ownerCancellationEpoch);
    EXPECT_EQ(expected.owner, actual.owner);
    EXPECT_EQ(expected.firstRowFrame, actual.firstRowFrame);
    EXPECT_EQ(expected.lastRowFrame, actual.lastRowFrame);
    EXPECT_EQ(expected.intendedBoundaryFrame, actual.intendedBoundaryFrame);
    EXPECT_EQ(expected.deadlineFrame, actual.deadlineFrame);
    EXPECT_EQ(expected.rootPlayable, actual.rootPlayable);
    EXPECT_EQ(expected.rootScheduleEpoch, actual.rootScheduleEpoch);
}

TEST(ActionDeferredRecordTable, HeldPairExcludesOtherPublishAbandonAndResetUntilConsume) {
    harken::ActionDeferredRecordTable table;
    harken::ActionDeferredRecordTable other;
    ASSERT_TRUE(table.Reset(1));
    ASSERT_TRUE(other.Reset(1));
    harken::DeferredIntentPool pool(1, 1);
    const auto record = MakeRecord();
    auto prepared = table.Prepare(record);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, prepared.status);
    const auto original = prepared.token;
    const auto added = pool.AddPrepared(MakeIntent(record, original));
    ASSERT_EQ(harken::DeferredIntentRefusal::None, added.status);
    harken::ActionDeferredRecordTable::PreparedPairCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedPair(original, pool, added.token, &proof));
    ASSERT_TRUE(proof.Valid());
    harken::ActionDeferredRecordTable::PreparedPairCommitProof duplicate;
    EXPECT_FALSE(table.ProvePreparedPair(original, pool, added.token, &duplicate));
    auto foreign = other.Prepare(record);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, foreign.status);
    EXPECT_FALSE(other.ProvePreparedPair(foreign.token, pool, added.token, &duplicate));
    EXPECT_FALSE(duplicate.Valid());
    EXPECT_FALSE(table.Abandon(&prepared.token));
    EXPECT_FALSE(table.Commit(&prepared.token, added.token));
    EXPECT_FALSE(table.CommitPreparedPair(&prepared.token, &pool, added.token));
    EXPECT_FALSE(pool.CommitPrepared(added.token));
    EXPECT_FALSE(pool.AbandonPrepared(added.token));
    EXPECT_FALSE(pool.CanResetForTransport());
    EXPECT_FALSE(pool.ResetForTransport());
    EXPECT_FALSE(table.Reset(2));
    EXPECT_EQ(harken::ActionDeferredRecordState::Prepared, table.State(original));
    EXPECT_EQ(1u, table.PreparedCount());
    EXPECT_EQ(1u, pool.PreparedCount());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, pool.Count());
    EXPECT_EQ(harken::DeferredIntentRefusal::OwnerCapReached,
        pool.AddPrepared(MakeIntent(record, original)).status);

    auto moved = std::move(proof);
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(moved.Valid());
    table.ConsumePreparedPair(std::move(moved));
    EXPECT_FALSE(moved.Valid());
    EXPECT_EQ(0u, table.PreparedCount());
    EXPECT_EQ(0u, pool.PreparedCount());
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_EQ(1u, pool.Count());
    EXPECT_EQ(harken::ActionDeferredRecordState::Held, table.State(original));
    ASSERT_TRUE(other.Abandon(&foreign.token));
    EXPECT_EQ(harken::ActionDeferredCancelOwnership::Canceller, table.RequestCancel(original));
    ASSERT_TRUE(pool.Cancel(added.token));
    ASSERT_TRUE(table.FinalizeHeld(original));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, pool.Count());

    auto replacement = table.Prepare(record);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, replacement.status);
    EXPECT_GT(replacement.token.generation, original.generation);
    auto next = pool.AddPrepared(MakeIntent(record, replacement.token));
    ASSERT_EQ(harken::DeferredIntentRefusal::None, next.status);
    EXPECT_GT(next.token.generation, added.token.generation);
    EXPECT_FALSE(table.ProvePreparedPair(original, pool, next.token, &duplicate));
    ASSERT_TRUE(pool.AbandonPrepared(next.token));
    ASSERT_TRUE(table.Abandon(&replacement.token));
}

TEST(ActionDeferredRecordTable, ReturnedPairLoanKeepsRealPreparationAndRollbackCapacity) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    harken::DeferredIntentPool pool(1, 1);
    const auto record = MakeRecord();
    table.SetCommitPageLimitForTests(0);
    const auto refused = table.Prepare(record);
    EXPECT_EQ(harken::ActionDeferredRecordPrepareStatus::Capacity, refused.status);
    EXPECT_FALSE(refused.token.Valid());
    EXPECT_EQ(1u, table.Capacity());
    EXPECT_EQ(0u, table.PreparedCount());
    table.SetCommitPageLimitForTests(SIZE_MAX);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        auto prepared = table.Prepare(record);
        ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, prepared.status);
        table.SetCommitPageLimitForTests(0);
        const auto original = prepared.token;
        const auto added = pool.AddPrepared(MakeIntent(record, original));
        ASSERT_EQ(harken::DeferredIntentRefusal::None, added.status);
        {
            harken::ActionDeferredRecordTable::PreparedPairCommitProof proof;
            ASSERT_TRUE(table.ProvePreparedPair(original, pool, added.token, &proof));
            auto moved = std::move(proof);
            ASSERT_TRUE(moved.Valid());
        }
        EXPECT_EQ(1u, table.PreparedCount());
        EXPECT_EQ(1u, pool.PreparedCount());
        EXPECT_EQ(0u, table.LiveCount());
        EXPECT_EQ(0u, pool.Count());
        ASSERT_TRUE(pool.AbandonPrepared(added.token));
        ASSERT_TRUE(table.Abandon(&prepared.token));
        EXPECT_EQ(0u, table.PreparedCount());
        EXPECT_EQ(0u, pool.PreparedCount());
    }
    ASSERT_TRUE(pool.ResetForTransport());
    ASSERT_TRUE(table.Reset(1));
}

TEST(ActionDeferredRecordTable, PairProofRejectsMismatchedFactoryOutputsWithoutHoldingEither) {
    for (unsigned mismatch = 0; mismatch < 6; ++mismatch) {
        SCOPED_TRACE(mismatch);
        harken::ActionDeferredRecordTable table;
        ASSERT_TRUE(table.Reset(1));
        harken::DeferredIntentPool pool(1, 1);
        const auto record = MakeRecord();
        auto prepared = table.Prepare(record);
        ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, prepared.status);
        auto intent = MakeIntent(record, prepared.token);
        switch (mismatch) {
        case 0: intent.ownerDomain = harken::DeferredIntentOwnerDomain::Playable; break;
        case 1: ++intent.ownerHandle; break;
        case 2: ++intent.ownerEpoch; break;
        case 3: ++intent.payload; break;
        case 4: ++intent.dueFrame; break;
        case 5: ++intent.executionOrdinal; break;
        }
        const auto added = pool.AddPrepared(intent);
        ASSERT_EQ(harken::DeferredIntentRefusal::None, added.status);
        harken::ActionDeferredRecordTable::PreparedPairCommitProof proof;
        EXPECT_FALSE(table.ProvePreparedPair(prepared.token, pool, added.token, &proof));
        EXPECT_FALSE(proof.Valid());
        EXPECT_EQ(1u, table.PreparedCount());
        EXPECT_EQ(1u, pool.PreparedCount());
        EXPECT_TRUE(pool.CanResetForTransport());
        ASSERT_TRUE(pool.AbandonPrepared(added.token));
        ASSERT_TRUE(table.Abandon(&prepared.token));
    }
}

TEST(ActionDeferredRecordTable, PublicationCaptureDoesNotChangeCancellationOrLiveState) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    auto expected = MakePublicationRecord();
    expected.stepIndex = 3;
    expected.depth = 2;
    expected.ownerActionStableId = harkentest::ObjectId(14);
    expected.rootActionStableId = harkentest::ObjectId(15);
    expected.rootActionProgramSemanticFingerprint = 16;
    expected.callerSeed = 17;
    expected.emitter = 18;
    expected.rootPlayable = 19;
    expected.rootScheduleEpoch = 20;
    auto prepared = table.Prepare(expected);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, prepared.status);
    const auto saved = prepared.token;
    harken::ActionDeferredRecordToken token{};
    harken::ActionDeferredRecord record{};
    harken::ActionDeferredRecordState state{};
    const auto verify = [&](harken::ActionDeferredRecordState expectedState) {
        const auto beforePrepared = table.PreparedCount();
        const auto beforeLive = table.LiveCount();
        const auto beforeHigh = table.HighWater();
        ASSERT_TRUE(table.CapturePublicationAt(saved.slot, &token, &record, &state));
        EXPECT_EQ(expectedState, state);
        EXPECT_EQ(saved.slot, token.slot);
        EXPECT_EQ(saved.generation, token.generation);
        ExpectPublicationRecord(expected, record);
        EXPECT_EQ(expectedState, table.State(saved));
        EXPECT_EQ(beforePrepared, table.PreparedCount());
        EXPECT_EQ(beforeLive, table.LiveCount());
        EXPECT_EQ(beforeHigh, table.HighWater());
        EXPECT_EQ(expectedState != harken::ActionDeferredRecordState::Prepared,
                  table.CaptureAt(saved.slot, &token, &record, &state));
    };
    EXPECT_EQ(harken::ActionDeferredCancelOwnership::None, table.RequestCancel(saved));
    verify(harken::ActionDeferredRecordState::Prepared);
    ASSERT_TRUE(table.Commit(&prepared.token, {3, 9}));
    EXPECT_FALSE(prepared.token.Valid());
    verify(harken::ActionDeferredRecordState::Held);
    harken::DeferredIntentClaim claim{};
    claim.token = {3, 9};
    claim.intent.ownerDomain = harken::DeferredIntentOwnerDomain::Action;
    claim.intent.payload = saved.slot;
    ASSERT_TRUE(table.Claim(claim, &token, &record));
    verify(harken::ActionDeferredRecordState::Claimed);
    EXPECT_EQ(harken::ActionDeferredCancelOwnership::Claimant, table.RequestCancel(saved));
    verify(harken::ActionDeferredRecordState::CancelRequested);
    ASSERT_TRUE(table.FinalizeClaim(saved));
    EXPECT_FALSE(table.CapturePublicationAt(saved.slot, &token, &record, &state));
}

TEST(ActionDeferredRecordTable, PublicationCaptureFailureLeavesEveryAvailableOutputUntouched) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    const harken::ActionDeferredRecordToken tokenSentinel{99, 123};
    auto recordSentinel = MakePublicationRecord();
    recordSentinel.executionOrdinal = 912;
    recordSentinel.owner = harken::ActionDeferredOwner::DirectWorld(77);
    const auto stateSentinel = harken::ActionDeferredRecordState::Claimed;
    const auto verify = [&](std::uint32_t slot, int nullOutput) {
        auto token = tokenSentinel;
        auto record = recordSentinel;
        auto state = stateSentinel;
        const auto beforePrepared = table.PreparedCount();
        const auto beforeLive = table.LiveCount();
        const auto beforeHigh = table.HighWater();
        const auto beforeQuarantined = table.QuarantinedCount();
        EXPECT_FALSE(table.CapturePublicationAt(slot,
            nullOutput == 1 ? nullptr : &token,
            nullOutput == 2 ? nullptr : &record,
            nullOutput == 3 ? nullptr : &state));
        EXPECT_EQ(tokenSentinel.slot, token.slot);
        EXPECT_EQ(tokenSentinel.generation, token.generation);
        ExpectPublicationRecord(recordSentinel, record);
        EXPECT_EQ(stateSentinel, state);
        EXPECT_EQ(beforePrepared, table.PreparedCount());
        EXPECT_EQ(beforeLive, table.LiveCount());
        EXPECT_EQ(beforeHigh, table.HighWater());
        EXPECT_EQ(beforeQuarantined, table.QuarantinedCount());
    };
    verify(0, 0);
    verify(1, 0);
    verify(UINT32_MAX, 0);
    ASSERT_TRUE(table.SetFreeGenerationForTests(0, UINT64_MAX));
    auto prepared = table.Prepare(MakePublicationRecord());
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, prepared.status);
    for (int nullOutput = 1; nullOutput <= 3; ++nullOutput) verify(0, nullOutput);
    const auto saved = prepared.token;
    ASSERT_TRUE(table.Commit(&prepared.token, {5, 11}));
    ASSERT_EQ(harken::ActionDeferredCancelOwnership::Canceller, table.RequestCancel(saved));
    ASSERT_TRUE(table.FinalizeHeld(saved));
    EXPECT_EQ(1u, table.QuarantinedCount());
    verify(0, 0);
    for (int nullOutput = 1; nullOutput <= 3; ++nullOutput) verify(0, nullOutput);
}

TEST(ActionDeferredRecordTable, PublicationCaptureAfterAbandonKeepsGenerationButReturnsNewRecord) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    auto first = table.Prepare(MakePublicationRecord());
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, first.status);
    const auto oldToken = first.token;
    ASSERT_TRUE(table.Abandon(&first.token));
    auto expected = MakePublicationRecord();
    expected.firing += 1;
    expected.executionOrdinal += 1;
    expected.owner = harken::ActionDeferredOwner::DirectWorld(22);
    auto next = table.Prepare(expected);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, next.status);
    EXPECT_EQ(oldToken.slot, next.token.slot);
    EXPECT_EQ(oldToken.generation, next.token.generation);
    harken::ActionDeferredRecordToken captured{};
    harken::ActionDeferredRecord actual{};
    harken::ActionDeferredRecordState state{};
    ASSERT_TRUE(table.CapturePublicationAt(oldToken.slot, &captured, &actual, &state));
    EXPECT_EQ(next.token.generation, captured.generation);
    EXPECT_EQ(harken::ActionDeferredRecordState::Prepared, state);
    ExpectPublicationRecord(expected, actual);
    EXPECT_EQ(1u, table.PreparedCount());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());
}

TEST(ActionDeferredRecordTable, PublicationCaptureAfterLiveFinalizationReturnsNewGeneration) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    auto first = table.Prepare(MakePublicationRecord());
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, first.status);
    const auto oldToken = first.token;
    ASSERT_TRUE(table.Commit(&first.token, {5, 11}));
    ASSERT_EQ(harken::ActionDeferredCancelOwnership::Canceller, table.RequestCancel(oldToken));
    ASSERT_TRUE(table.FinalizeHeld(oldToken));
    auto next = table.Prepare(MakePublicationRecord());
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, next.status);
    EXPECT_EQ(oldToken.slot, next.token.slot);
    EXPECT_EQ(oldToken.generation + 1, next.token.generation);
    harken::ActionDeferredRecordToken captured{};
    harken::ActionDeferredRecord actual{};
    harken::ActionDeferredRecordState state{};
    EXPECT_FALSE(table.Capture(oldToken, &actual));
    ASSERT_TRUE(table.CapturePublicationAt(next.token.slot, &captured, &actual, &state));
    EXPECT_EQ(next.token.generation, captured.generation);
    EXPECT_EQ(harken::ActionDeferredRecordState::Prepared, state);
    ExpectPublicationRecord(MakePublicationRecord(), actual);
}

TEST(ActionDeferredRecordTable, PublicationRecordComparatorDetectsMissingSemanticFields) {
    const auto expected = MakePublicationRecord();
    auto actual = expected;
    actual.step.target.stableId = harkentest::ObjectId(0);
    EXPECT_NONFATAL_FAILURE(ExpectPublicationRecord(expected, actual), "step.target.stableId");
    actual = expected;
    actual.step.delay.numerator = 0;
    EXPECT_NONFATAL_FAILURE(ExpectPublicationRecord(expected, actual), "step.delay.numerator");
    actual = expected;
    actual.step.quantizeTiming = harken::ActionQuantizeTimingV1::Immediate;
    EXPECT_NONFATAL_FAILURE(ExpectPublicationRecord(expected, actual), "step.quantizeTiming");
    actual = expected;
    actual.step.cooldown.numerator = 0;
    EXPECT_NONFATAL_FAILURE(ExpectPublicationRecord(expected, actual), "step.cooldown.numerator");
    actual = expected;
    actual.targetPackLease.resourceToken = 0;
    EXPECT_NONFATAL_FAILURE(ExpectPublicationRecord(expected, actual), "targetPackLease.resourceToken");
}

TEST(ActionDeferredRecordTable,
     PairedCommitPublishesMatchingRecordAndIntentTogether) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    harken::DeferredIntentPool pool(1, 1);
    const auto record = MakeRecord();
    auto preparedRecord = table.Prepare(record);
    ASSERT_EQ(preparedRecord.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const auto recordToken = preparedRecord.token;
    const auto preparedIntent = pool.AddPrepared(
        MakeIntent(record, recordToken));
    ASSERT_EQ(preparedIntent.status, harken::DeferredIntentRefusal::None);

    ASSERT_TRUE(table.CommitPreparedPair(
        &preparedRecord.token, &pool, preparedIntent.token));
    EXPECT_FALSE(preparedRecord.token.Valid());
    EXPECT_EQ(table.PreparedCount(), 0u);
    EXPECT_EQ(table.LiveCount(), 1u);
    EXPECT_EQ(table.HighWater(), 1u);
    EXPECT_EQ(pool.PreparedCount(), 0u);
    EXPECT_EQ(pool.Count(), 1u);
    EXPECT_EQ(pool.HighWater(), 1u);
    harken::DeferredIntentToken storedPoolToken{};
    ASSERT_TRUE(table.PoolToken(recordToken, &storedPoolToken));
    EXPECT_EQ(storedPoolToken.slot, preparedIntent.token.slot);
    EXPECT_EQ(storedPoolToken.generation,
              preparedIntent.token.generation);
}

TEST(ActionDeferredRecordTable,
     FixedPlayClaimKeepsTheExactPairAndRetriesBothSides) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    harken::DeferredIntentPool pool(1, 1);
    auto record = MakeRecord();
    record.firstRowFrame = 512;
    record.lastRowFrame = 512;
    record.intendedBoundaryFrame = 512;
    record.deadlineFrame = 512;
    auto prepared = table.Prepare(record);
    ASSERT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const auto recordToken = prepared.token;
    const auto added = pool.AddPrepared(MakeIntent(record, recordToken));
    ASSERT_EQ(added.status, harken::DeferredIntentRefusal::None);
    ASSERT_TRUE(table.CommitPreparedPair(&prepared.token, &pool, added.token));

    harken::DeferredIntentClaim claim{};
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 1, 256,
                  1024, 256, &claim),
              harken::FixedPlayClaimStatus::Claimed);
    EXPECT_EQ(claim.token.slot, added.token.slot);
    EXPECT_EQ(claim.token.generation, added.token.generation);
    harken::ActionDeferredRecordToken claimedRecord{};
    harken::ActionDeferredRecord captured{};
    ASSERT_TRUE(table.Claim(claim, &claimedRecord, &captured));
    EXPECT_FALSE(table.RetryFixedPlayPair(
        &pool, {claim.token.slot, claim.token.generation + 1u},
        claimedRecord, 2));
    EXPECT_EQ(table.State(claimedRecord),
              harken::ActionDeferredRecordState::Claimed);
    ASSERT_TRUE(table.RetryFixedPlayPair(&pool, claim.token, claimedRecord, 2));
    EXPECT_EQ(table.State(claimedRecord),
              harken::ActionDeferredRecordState::Held);
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 1, 256,
                  1024, 256, &claim),
              harken::FixedPlayClaimStatus::None);
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 2, 256,
                  1024, 256, &claim),
              harken::FixedPlayClaimStatus::Claimed);
}

TEST(ActionDeferredRecordTable,
     RetryReservationBlocksBothClaimPathsAndPreservesDueOrder) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::DeferredIntentPool pool(2, 2);
    auto first = MakeRecord();
    first.firstRowFrame = 512;
    first.lastRowFrame = 512;
    first.intendedBoundaryFrame = 512;
    first.deadlineFrame = 512;
    first.executionOrdinal = 1;
    auto second = first;
    second.executionOrdinal = 2;
    auto firstPrepared = table.Prepare(first);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared,
              firstPrepared.status);
    const auto firstRecord = firstPrepared.token;
    const auto firstAdded = pool.AddPrepared(MakeIntent(first, firstRecord));
    ASSERT_EQ(harken::DeferredIntentRefusal::None, firstAdded.status);
    ASSERT_TRUE(table.CommitPreparedPair(
        &firstPrepared.token, &pool, firstAdded.token));
    auto secondPrepared = table.Prepare(second);
    ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared,
              secondPrepared.status);
    const auto secondAdded = pool.AddPrepared(
        MakeIntent(second, secondPrepared.token));
    ASSERT_EQ(harken::DeferredIntentRefusal::None, secondAdded.status);
    ASSERT_TRUE(table.CommitPreparedPair(
        &secondPrepared.token, &pool, secondAdded.token));

    harken::DeferredIntentClaim firstClaim{};
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed,
              table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 1,
                  256, 1024, 256, &firstClaim));
    harken::ActionDeferredRecordToken claimedRecord{};
    harken::ActionDeferredRecord captured{};
    ASSERT_TRUE(table.Claim(firstClaim, &claimedRecord, &captured));
    ASSERT_EQ(firstRecord.slot, claimedRecord.slot);
    ASSERT_TRUE(table.RetryFixedPlayPair(
        &pool, firstClaim.token, claimedRecord, 2, true));
    harken::ActionDeferredPairState pair{};
    ASSERT_TRUE(table.InspectPair(
        pool, firstClaim.token, claimedRecord, &pair));
    EXPECT_TRUE(pair.recordReservation.Valid());
    EXPECT_TRUE(pair.poolReservation.Valid());
    EXPECT_EQ(firstRecord.generation,
              pair.poolReservation.recordGeneration);

    harken::DeferredIntentClaim blocked{};
    EXPECT_EQ(harken::FixedPlayClaimStatus::None,
              table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 2,
                  256, 1024, 256, &blocked));
    harken::PlayableInstanceTable instances(1);
    EXPECT_EQ(0u, pool.ClaimDue(
        1024, instances, &blocked, 1, nullptr, nullptr, 2));
    EXPECT_FALSE(table.Claim(firstClaim, &claimedRecord, &captured));
    EXPECT_EQ(harken::ActionDeferredRecordState::Held,
              table.State(firstRecord));

    ASSERT_TRUE(table.ClearFixedPlayPairRetryReservation(
        &pool, firstClaim.token, firstRecord));
    ASSERT_TRUE(table.InspectPair(pool, firstClaim.token, firstRecord,
                                  &pair));
    EXPECT_FALSE(pair.recordReservation.Valid());
    EXPECT_FALSE(pair.poolReservation.Valid());
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed,
              table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 2,
                  256, 1024, 256, &blocked));
    EXPECT_EQ(firstClaim.token.slot, blocked.token.slot);
    EXPECT_EQ(firstClaim.token.generation, blocked.token.generation);
}

TEST(ActionDeferredRecordTable,
     CancelRequestedRetryReservationStillBlocksFixedDueClass) {
    const auto verify = [](harken::FixedPlayClaimMode mode,
                           std::uint64_t renderedQuantum) {
        harken::ActionDeferredRecordTable table;
        ASSERT_TRUE(table.Reset(2));
        harken::DeferredIntentPool pool(2, 2);
        auto first = MakeRecord();
        first.firstRowFrame = 512;
        first.lastRowFrame = 512;
        first.intendedBoundaryFrame = 512;
        first.deadlineFrame = 512;
        first.executionOrdinal = 1;
        auto second = first;
        second.firing = first.firing + 1;
        second.executionOrdinal = 2;
        auto firstPrepared = table.Prepare(first);
        ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared,
                  firstPrepared.status);
        const auto firstToken = firstPrepared.token;
        const auto firstAdded = pool.AddPrepared(
            MakeIntent(first, firstToken));
        ASSERT_EQ(harken::DeferredIntentRefusal::None, firstAdded.status);
        ASSERT_TRUE(table.CommitPreparedPair(
            &firstPrepared.token, &pool, firstAdded.token));
        auto secondPrepared = table.Prepare(second);
        ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared,
                  secondPrepared.status);
        const auto secondToken = secondPrepared.token;
        const auto secondAdded = pool.AddPrepared(
            MakeIntent(second, secondToken));
        ASSERT_EQ(harken::DeferredIntentRefusal::None, secondAdded.status);
        ASSERT_TRUE(table.CommitPreparedPair(
            &secondPrepared.token, &pool, secondAdded.token));

        harken::DeferredIntentClaim firstClaim{};
        ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed,
                  table.ClaimFixedPlayPair(
                      &pool, harken::FixedPlayClaimMode::BeforeDeadline,
                      1, 256, 1024, 256, &firstClaim));
        harken::ActionDeferredRecordToken claimedRecord{};
        harken::ActionDeferredRecord captured{};
        ASSERT_TRUE(table.Claim(firstClaim, &claimedRecord, &captured));
        ASSERT_EQ(firstToken.slot, claimedRecord.slot);
        ASSERT_TRUE(table.RetryFixedPlayPair(
            &pool, firstClaim.token, claimedRecord, 2, true));
        ASSERT_EQ(harken::ActionDeferredCancelOwnership::Claimant,
                  table.RequestCancel(firstToken));
        ASSERT_EQ(harken::ActionDeferredRecordState::CancelRequested,
                  table.State(firstToken));
        ASSERT_EQ(harken::ActionDeferredRecordState::Held,
                  table.State(secondToken));
        harken::ActionDeferredPairState pair{};
        ASSERT_TRUE(table.InspectPair(
            pool, firstClaim.token, firstToken, &pair));
        ASSERT_EQ(harken::DeferredIntentEntryState::Held, pair.poolState);
        ASSERT_TRUE(pair.recordReservation.Valid());
        ASSERT_TRUE(pair.poolReservation.Valid());

        harken::PlayableInstanceTable instances(1);
        harken::DeferredIntentClaim next{};
        EXPECT_EQ(0u, pool.ClaimDue(
            1024, instances, &next, 1, nullptr, nullptr,
            renderedQuantum));
        EXPECT_EQ(harken::FixedPlayClaimStatus::None,
                  table.ClaimFixedPlayPair(
                      &pool, mode, renderedQuantum, 256, 1024,
                      mode == harken::FixedPlayClaimMode::Expired ? 768 : 256,
                      &next));
        EXPECT_FALSE(next.token.Valid());
        EXPECT_EQ(harken::ActionDeferredRecordState::Held,
                  table.State(secondToken));
    };
    verify(harken::FixedPlayClaimMode::BeforeDeadline, 2);
    verify(harken::FixedPlayClaimMode::Expired, 3);
}

TEST(ActionDeferredRecordTable,
     FixedPlayClaimRejectsMismatchedHeldGenerationBeforeMutation) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    harken::DeferredIntentPool pool(1, 1);
    auto record = MakeRecord();
    record.firstRowFrame = 512;
    record.lastRowFrame = 512;
    record.intendedBoundaryFrame = 512;
    record.deadlineFrame = 512;
    auto prepared = table.Prepare(record);
    ASSERT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const auto recordToken = prepared.token;
    const auto added = pool.AddPrepared(MakeIntent(record, recordToken));
    ASSERT_EQ(added.status, harken::DeferredIntentRefusal::None);
    ASSERT_TRUE(table.CommitPreparedPair(&prepared.token, &pool, added.token));
    harken::DeferredIntentToken changed{};
    ASSERT_TRUE(pool.SetTokenGenerationForTests(
        added.token, added.token.generation + 1u, &changed));

    harken::DeferredIntentClaim claim{};
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 1, 256,
                  1024, 256, &claim),
              harken::FixedPlayClaimStatus::ProtocolViolation);
    EXPECT_FALSE(claim.token.Valid());
    EXPECT_EQ(table.State(recordToken), harken::ActionDeferredRecordState::Held);
    harken::ActionDeferredPairState state{};
    ASSERT_TRUE(table.InspectPair(pool, added.token, recordToken, &state));
    EXPECT_EQ(state.poolState, harken::DeferredIntentEntryState::Held);
    EXPECT_EQ(state.poolToken.generation, changed.generation);
    EXPECT_EQ(state.recordToken.generation, recordToken.generation);
    EXPECT_EQ(pool.Count(), 1u);
}

TEST(ActionDeferredRecordTable,
     FixedPlayClaimKeepsSameDueActionOrderBehindFutureRetry) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::DeferredIntentPool pool(2, 2);
    harken::PlayableInstanceTable instances(1);

    harken::ActionDeferredRecordToken tokens[2]{};
    harken::DeferredIntentToken poolTokens[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        auto record = MakeRecord();
        record.firstRowFrame = 512;
        record.lastRowFrame = 512;
        record.intendedBoundaryFrame = 512;
        record.deadlineFrame = 512;
        record.executionOrdinal = i + 1u;
        auto prepared = table.Prepare(record);
        ASSERT_EQ(prepared.status,
                  harken::ActionDeferredRecordPrepareStatus::Prepared);
        tokens[i] = prepared.token;
        auto intent = MakeIntent(record, prepared.token);
        intent.commitSequence = 2;
        const auto added = pool.AddPrepared(intent);
        ASSERT_EQ(added.status, harken::DeferredIntentRefusal::None);
        poolTokens[i] = added.token;
        ASSERT_TRUE(table.CommitPreparedPair(&prepared.token, &pool, added.token));
    }

    harken::DeferredIntentClaim ordinary{};
    ASSERT_EQ(pool.ClaimDue(1024, instances, &ordinary, 1), 1u);
    EXPECT_EQ(ordinary.token.slot, poolTokens[0].slot);
    ASSERT_EQ(pool.Retry(ordinary.token, 2), harken::DeferredIntentRefusal::None);

    harken::DeferredIntentClaim special{};
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 1, 256,
                  1024, 256, &special),
              harken::FixedPlayClaimStatus::None);
    EXPECT_FALSE(special.token.Valid());
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 2, 256,
                  1024, 256, &special),
              harken::FixedPlayClaimStatus::Claimed);
    EXPECT_EQ(special.token.slot, poolTokens[0].slot);
    EXPECT_EQ(pool.Count(), 2u);
    EXPECT_EQ(table.State(tokens[0]), harken::ActionDeferredRecordState::Held);
    EXPECT_EQ(table.State(tokens[1]), harken::ActionDeferredRecordState::Held);
}

TEST(ActionDeferredRecordTable,
     FixedPlayClaimHasExplicitSameDuePlayableDomainException) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    harken::DeferredIntentPool pool(2, 2);
    harken::PlayableInstanceTable instances(1);
    const auto owner = instances.Acquire(harken::PlayableKind::Segment, 0);
    ASSERT_NE(0u, owner);
    harken::DeferredIntent playable{};
    playable.ownerDomain = harken::DeferredIntentOwnerDomain::Playable;
    playable.ownerHandle = owner;
    playable.ownerEpoch = instances.Classify(owner).scheduleEpoch;
    playable.dueFrame = 512;
    playable.commitSequence = 1;
    playable.notBeforeQuantum = 2;
    ASSERT_EQ(pool.AddClaimable(playable).status,
              harken::DeferredIntentRefusal::None);

    auto record = MakeRecord();
    record.firstRowFrame = 512;
    record.lastRowFrame = 512;
    record.intendedBoundaryFrame = 512;
    record.deadlineFrame = 512;
    auto prepared = table.Prepare(record);
    ASSERT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    auto intent = MakeIntent(record, prepared.token);
    intent.commitSequence = 2;
    const auto added = pool.AddPrepared(intent);
    ASSERT_EQ(added.status, harken::DeferredIntentRefusal::None);
    ASSERT_TRUE(table.CommitPreparedPair(&prepared.token, &pool, added.token));

    harken::DeferredIntentClaim special{};
    EXPECT_EQ(table.ClaimFixedPlayPair(
                  &pool, harken::FixedPlayClaimMode::BeforeDeadline, 1, 256,
                  1024, 256, &special),
              harken::FixedPlayClaimStatus::Claimed);
    EXPECT_EQ(special.token.slot, added.token.slot);
    EXPECT_EQ(pool.EarliestHeldDueBefore(1024), 512u);
}

class ActionDeferredPrerequisite : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(table.Reset(3)); }

    void Publish(std::uint32_t index, harken::ActionOpcodeV1 opcode, std::uint64_t frame,
                 std::uint32_t ordinal, std::uint64_t ticket, std::uint64_t firingOffset = 0) {
        auto record = MakeRecord();
        record.step.opcode = opcode;
        record.firing += firingOffset;
        record.firstRowFrame = record.lastRowFrame = record.intendedBoundaryFrame = record.deadlineFrame = frame;
        record.executionOrdinal = ordinal;
        auto prepared = table.Prepare(record);
        ASSERT_EQ(harken::ActionDeferredRecordPrepareStatus::Prepared, prepared.status);
        records[index] = prepared.token;
        auto intent = MakeIntent(record, prepared.token);
        intent.commitSequence = ticket;
        const auto added = pool.AddPrepared(intent);
        ASSERT_EQ(harken::DeferredIntentRefusal::None, added.status);
        intents[index] = added.token;
        ASSERT_TRUE(table.CommitPreparedPair(&prepared.token, &pool, added.token));
    }

    harken::FixedPlayClaimStatus Claim(harken::DeferredIntentClaim* out,
        harken::FixedPlayClaimMode mode = harken::FixedPlayClaimMode::BeforeDeadline,
        std::uint64_t quantum = 1, std::uint64_t cutoff = 256,
        harken::ActionDeferredPairState* fault = nullptr) {
        return table.ClaimFixedPlayPair(&pool, mode, quantum, 256, 1024, cutoff, out, fault);
    }

    void ExpectBlockedThenConsumed(harken::ActionOpcodeV1 opcode) {
        ASSERT_NO_FATAL_FAILURE(Publish(0, opcode, 512, 1, 7, 1));
        ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 2, 7));
        harken::DeferredIntentClaim claim{};
        ASSERT_EQ(harken::FixedPlayClaimStatus::None, Claim(&claim));
        EXPECT_FALSE(claim.token.Valid());
        EXPECT_EQ(2u, pool.Count());
        ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1));
        EXPECT_EQ(intents[0].slot, claim.token.slot);
        harken::ActionDeferredRecordToken record{};
        harken::ActionDeferredRecord captured{};
        ASSERT_TRUE(table.Claim(claim, &record, &captured));
        EXPECT_EQ(opcode, captured.step.opcode);
        ASSERT_TRUE(pool.Release(claim.token));
        ASSERT_TRUE(table.FinalizeClaim(record));
        ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed, Claim(&claim));
        EXPECT_EQ(intents[1].slot, claim.token.slot);
        EXPECT_EQ(harken::ActionDeferredRecordState::Held, table.State(records[1]));
    }

    void ExpectDifferentFrameOrdinaryRetry(harken::ActionOpcodeV1 opcode) {
        ASSERT_NO_FATAL_FAILURE(Publish(0, opcode, 512, 0, 7, 1));
        ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 768, 1, 7));
        harken::DeferredIntentClaim claim{};
        harken::ActionDeferredRecordToken record{};
        harken::ActionDeferredRecord captured{};
        ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1));
        ASSERT_EQ(intents[0].slot, claim.token.slot);
        ASSERT_TRUE(table.Claim(claim, &record, &captured));
        ASSERT_EQ(harken::DeferredIntentRefusal::None, pool.Retry(claim.token, 2));
        ASSERT_TRUE(table.RetryClaim(record));
        claim = {};
        EXPECT_EQ(harken::FixedPlayClaimStatus::None, Claim(&claim));
        ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1, nullptr, nullptr, 1));
        ASSERT_EQ(intents[1].slot, claim.token.slot);
        ASSERT_TRUE(table.Claim(claim, &record, &captured));
        bool held = false;
        ASSERT_TRUE(table.InspectClaimedPlayPrerequisites(&pool, claim.token, record, &held));
        ASSERT_TRUE(held);
        ASSERT_TRUE(table.RetryFixedPlayPair(&pool, claim.token, record, 2));
        ASSERT_TRUE(table.Capture(records[1], &captured));
        EXPECT_EQ(768u, captured.intendedBoundaryFrame);
        EXPECT_EQ(768u, captured.deadlineFrame);
        EXPECT_EQ(harken::ActionDeferredRecordState::Held, table.State(records[1]));

        ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1, nullptr, nullptr, 2));
        ASSERT_EQ(intents[0].slot, claim.token.slot);
        ASSERT_TRUE(table.Claim(claim, &record, &captured));
        ASSERT_TRUE(pool.Release(claim.token));
        ASSERT_TRUE(table.FinalizeClaim(record));
        ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1, nullptr, nullptr, 2));
        EXPECT_EQ(intents[1].slot, claim.token.slot);
        EXPECT_EQ(intents[1].generation, claim.token.generation);
        EXPECT_EQ(768u, claim.intent.dueFrame);
        EXPECT_EQ(7u, claim.intent.commitSequence);
        ASSERT_TRUE(table.Claim(claim, &record, &captured));
        ASSERT_TRUE(table.InspectClaimedPlayPrerequisites(&pool, claim.token, record, &held));
        EXPECT_FALSE(held);
        ASSERT_TRUE(pool.Release(claim.token));
        ASSERT_TRUE(table.FinalizeClaim(record));
        EXPECT_EQ(0u, table.LiveCount());
        EXPECT_EQ(0u, pool.Count());
    }

    harken::ActionDeferredRecordTable table;
    harken::DeferredIntentPool pool{3, 3};
    harken::PlayableInstanceTable instances{1};
    std::array<harken::ActionDeferredRecordToken, 3> records{};
    std::array<harken::DeferredIntentToken, 3> intents{};
};

TEST_F(ActionDeferredPrerequisite, ResetInAnotherFiringOfTheSameAcceptedTicketRunsBeforePlay) {
    ExpectBlockedThenConsumed(harken::ActionOpcodeV1::Reset);
}

TEST_F(ActionDeferredPrerequisite, SetVariableInAnotherFiringOfTheSameAcceptedTicketRunsBeforePlay) {
    ExpectBlockedThenConsumed(harken::ActionOpcodeV1::SetVariable);
}

TEST_F(ActionDeferredPrerequisite, BlockedTicketDoesNotPreventAnotherAcceptedPlay) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 512, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    ASSERT_NO_FATAL_FAILURE(Publish(2, harken::ActionOpcodeV1::Play, 512, 0, 8, 2));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed, Claim(&claim));
    EXPECT_EQ(intents[2].slot, claim.token.slot);
    EXPECT_EQ(8u, claim.intent.commitSequence);
    EXPECT_EQ(harken::ActionDeferredRecordState::Held, table.State(records[1]));
}

TEST_F(ActionDeferredPrerequisite, FutureResetDoesNotBlockEarlierFramePlay) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 768, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed, Claim(&claim));
    EXPECT_EQ(intents[1].slot, claim.token.slot);
}

TEST_F(ActionDeferredPrerequisite, LaterSameFrameResetDoesNotBlockEarlierEventPlay) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 512, 2, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed, Claim(&claim));
    EXPECT_EQ(intents[1].slot, claim.token.slot);
}

TEST_F(ActionDeferredPrerequisite, RetriedVariableWriteStillPrecedesPlayUntilMaterialized) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::SetVariable, 512, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1));
    ASSERT_EQ(intents[0].slot, claim.token.slot);
    harken::ActionDeferredRecordToken record{};
    harken::ActionDeferredRecord captured{};
    ASSERT_TRUE(table.Claim(claim, &record, &captured));
    ASSERT_EQ(harken::DeferredIntentRefusal::None, pool.Retry(claim.token, 2));
    ASSERT_TRUE(table.RetryClaim(record));
    claim = {};
    EXPECT_EQ(harken::FixedPlayClaimStatus::None, Claim(&claim));
    EXPECT_EQ(harken::FixedPlayClaimStatus::None,
              Claim(&claim, harken::FixedPlayClaimMode::BeforeDeadline, 2, 512));
    EXPECT_FALSE(claim.token.Valid());
    ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1, nullptr, nullptr, 2));
    EXPECT_EQ(intents[0].slot, claim.token.slot);
    ASSERT_TRUE(table.Claim(claim, &record, &captured));
    ASSERT_TRUE(pool.Release(claim.token));
    ASSERT_TRUE(table.FinalizeClaim(record));
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed,
              Claim(&claim, harken::FixedPlayClaimMode::BeforeDeadline, 2, 512));
    EXPECT_EQ(intents[1].slot, claim.token.slot);
    EXPECT_EQ(intents[1].generation, claim.token.generation);
}

TEST_F(ActionDeferredPrerequisite, CancellerOwnedResetDoesNotBlockPlay) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 512, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    ASSERT_EQ(harken::ActionDeferredCancelOwnership::Canceller, table.RequestCancel(records[0]));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed, Claim(&claim));
    EXPECT_EQ(intents[1].slot, claim.token.slot);
    ASSERT_TRUE(pool.Cancel(intents[0]));
    ASSERT_TRUE(table.FinalizeHeld(records[0]));
    EXPECT_EQ(1u, table.LiveCount());
}

TEST_F(ActionDeferredPrerequisite, ExpiredPlayKeepsItsOriginalDeadlineDespiteHeldReset) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 512, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(harken::FixedPlayClaimStatus::Claimed,
              Claim(&claim, harken::FixedPlayClaimMode::Expired, 3, 768));
    EXPECT_EQ(intents[1].slot, claim.token.slot);
    EXPECT_EQ(512u, claim.intent.dueFrame);
    EXPECT_EQ(harken::ActionDeferredRecordState::Held, table.State(records[0]));
}

TEST_F(ActionDeferredPrerequisite, BrokenPrerequisitePairReportsProtocolFaultWithoutClaimingPlay) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 512, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 512, 1, 7));
    harken::DeferredIntentToken changed{};
    ASSERT_TRUE(pool.SetTokenGenerationForTests(intents[0], intents[0].generation + 1u, &changed));
    harken::DeferredIntentClaim claim{};
    harken::ActionDeferredPairState fault{};
    EXPECT_EQ(harken::FixedPlayClaimStatus::ProtocolViolation,
              Claim(&claim, harken::FixedPlayClaimMode::BeforeDeadline, 1, 256, &fault));
    EXPECT_EQ(records[0].slot, fault.recordToken.slot);
    EXPECT_FALSE(claim.token.Valid());
    EXPECT_EQ(2u, pool.Count());
    EXPECT_EQ(harken::ActionDeferredRecordState::Held, table.State(records[1]));
}

TEST_F(ActionDeferredPrerequisite, OrdinaryClaimCannotMaterializePastEarlierFrameRetriedReset) {
    ExpectDifferentFrameOrdinaryRetry(harken::ActionOpcodeV1::Reset);
}

TEST_F(ActionDeferredPrerequisite, OrdinaryClaimCannotMaterializePastEarlierFrameRetriedVariableWrite) {
    ExpectDifferentFrameOrdinaryRetry(harken::ActionOpcodeV1::SetVariable);
}

TEST_F(ActionDeferredPrerequisite, ClaimedInspectionRejectsHeldPairWithoutMutation) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Play, 512, 0, 7));
    bool held = false;
    harken::ActionDeferredPairState fault{};
    EXPECT_FALSE(table.InspectClaimedPlayPrerequisites(&pool, intents[0], records[0], &held, &fault));
    EXPECT_EQ(harken::ActionDeferredRecordState::Held, fault.recordState);
    EXPECT_EQ(harken::DeferredIntentEntryState::Held, fault.poolState);
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_EQ(1u, pool.Count());
}

TEST_F(ActionDeferredPrerequisite, ClaimedInspectionReportsBrokenEarlierPairAndPreservesCandidate) {
    ASSERT_NO_FATAL_FAILURE(Publish(0, harken::ActionOpcodeV1::Reset, 512, 0, 7, 1));
    ASSERT_NO_FATAL_FAILURE(Publish(1, harken::ActionOpcodeV1::Play, 768, 1, 7));
    harken::DeferredIntentClaim claim{};
    ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1));
    ASSERT_EQ(harken::DeferredIntentRefusal::None, pool.Retry(claim.token, 2));
    ASSERT_EQ(1u, pool.ClaimDue(1024, instances, &claim, 1, nullptr, nullptr, 1));
    ASSERT_EQ(intents[1].slot, claim.token.slot);
    harken::ActionDeferredRecordToken record{};
    harken::ActionDeferredRecord captured{};
    ASSERT_TRUE(table.Claim(claim, &record, &captured));
    harken::DeferredIntentToken changed{};
    ASSERT_TRUE(pool.SetTokenGenerationForTests(intents[0], intents[0].generation + 1u, &changed));
    bool held = false;
    harken::ActionDeferredPairState fault{};
    EXPECT_FALSE(table.InspectClaimedPlayPrerequisites(&pool, claim.token, record, &held, &fault));
    EXPECT_EQ(records[0].slot, fault.recordToken.slot);
    EXPECT_EQ(harken::ActionDeferredRecordState::Claimed, table.State(records[1]));
    EXPECT_EQ(2u, table.LiveCount());
    EXPECT_EQ(2u, pool.Count());
}

TEST(ActionDeferredRecordTable,
     PairedCommitRejectsWrongPoolTokenWithoutPublishingEitherSide) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    harken::DeferredIntentPool pool(1, 1);
    const auto record = MakeRecord();
    auto preparedRecord = table.Prepare(record);
    ASSERT_EQ(preparedRecord.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const auto recordToken = preparedRecord.token;
    const auto preparedIntent = pool.AddPrepared(
        MakeIntent(record, recordToken));
    ASSERT_EQ(preparedIntent.status, harken::DeferredIntentRefusal::None);
    const harken::DeferredIntentToken wrongPoolToken{
        preparedIntent.token.slot, preparedIntent.token.generation + 1u};

    EXPECT_FALSE(table.CommitPreparedPair(
        &preparedRecord.token, &pool, wrongPoolToken));
    EXPECT_EQ(preparedRecord.token.slot, recordToken.slot);
    EXPECT_EQ(preparedRecord.token.generation, recordToken.generation);
    EXPECT_EQ(table.State(recordToken),
              harken::ActionDeferredRecordState::Prepared);
    EXPECT_EQ(table.PreparedCount(), 1u);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);
    EXPECT_EQ(pool.PreparedCount(), 1u);
    EXPECT_EQ(pool.Count(), 0u);
    EXPECT_EQ(pool.HighWater(), 0u);
    harken::DeferredIntentToken storedPoolToken{};
    EXPECT_FALSE(table.PoolToken(recordToken, &storedPoolToken));
}

TEST(ActionDeferredRecordTable,
     PairedCommitRejectsWrongStateAndOwnerWithoutPublishingPeer) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::DeferredIntentPool pool(2, 2);
    const auto record = MakeRecord();

    auto wrongOwnerRecord = table.Prepare(record);
    ASSERT_EQ(wrongOwnerRecord.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    auto wrongOwnerIntent = MakeIntent(record, wrongOwnerRecord.token);
    ++wrongOwnerIntent.ownerEpoch;
    const auto preparedWrongOwner = pool.AddPrepared(wrongOwnerIntent);
    ASSERT_EQ(preparedWrongOwner.status,
              harken::DeferredIntentRefusal::None);
    EXPECT_FALSE(table.CommitPreparedPair(
        &wrongOwnerRecord.token, &pool, preparedWrongOwner.token));
    EXPECT_EQ(table.PreparedCount(), 1u);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(pool.PreparedCount(), 1u);
    EXPECT_EQ(pool.Count(), 0u);

    ASSERT_TRUE(pool.AbandonPrepared(preparedWrongOwner.token));
    ASSERT_TRUE(table.Abandon(&wrongOwnerRecord.token));
    auto wrongStateRecord = table.Prepare(record);
    ASSERT_EQ(wrongStateRecord.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const auto wrongStateToken = wrongStateRecord.token;
    const auto preparedWrongState = pool.AddPrepared(
        MakeIntent(record, wrongStateToken));
    ASSERT_EQ(preparedWrongState.status,
              harken::DeferredIntentRefusal::None);
    ASSERT_TRUE(table.Abandon(&wrongStateRecord.token));

    auto staleRecordToken = wrongStateToken;
    EXPECT_FALSE(table.CommitPreparedPair(
        &staleRecordToken, &pool, preparedWrongState.token));
    EXPECT_EQ(table.PreparedCount(), 0u);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);
    EXPECT_EQ(pool.PreparedCount(), 1u);
    EXPECT_EQ(pool.Count(), 0u);
    EXPECT_EQ(pool.HighWater(), 0u);
}

TEST(ActionDeferredRecordTable, PackAnchorRoundTripsThroughPreparedCommitClaim) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));

    auto record = MakeRecord();
    record.owner = harken::ActionDeferredOwner::Pack({
        UINT64_C(0x0000000d00000003), 90, 12, 34, 56, 78, 91, 92, 13, 17});
    const auto expectedOwner = record.owner;
    auto prepared = table.Prepare(record);
    ASSERT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    EXPECT_EQ(table.PreparedCount(), 1u);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);

    const harken::DeferredIntentToken poolToken{3, 9};
    ASSERT_TRUE(table.Commit(&prepared.token, poolToken));

    harken::DeferredIntentClaim claim{};
    claim.token = poolToken;
    claim.intent.ownerDomain = harken::DeferredIntentOwnerDomain::Action;
    claim.intent.payload = 0;
    harken::ActionDeferredRecordToken claimedToken{};
    harken::ActionDeferredRecord captured{};
    ASSERT_TRUE(table.Claim(claim, &claimedToken, &captured));
    EXPECT_EQ(captured.owner, expectedOwner);
    EXPECT_EQ(captured.owner.pack.leaseToken, 17u);
    EXPECT_EQ(captured.owner.pack.resourceClosureToken, 91u);
    ASSERT_TRUE(table.FinalizeClaim(claimedToken));
    EXPECT_EQ(table.LiveCount(), 0u);
}

TEST(ActionDeferredRecordTable, InvalidPackAnchorDoesNotReserveARecord) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));

    auto record = MakeRecord();
    record.owner = harken::ActionDeferredOwner::Pack({
        UINT64_C(0x0000000d00000003), 90, 12, 34, 56, 78, 91, 0, 13, 17});
    const auto prepared = table.Prepare(record);
    EXPECT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Invalid);
    EXPECT_EQ(table.PreparedCount(), 0u);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);
}

TEST(ActionDeferredRecordTable,
     ClaimedCancellationIsFinalizedExactlyOnceByTheClaimant) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));

    auto prepared = table.Prepare(MakeRecord());
    ASSERT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const std::uint64_t payload = prepared.token.slot;
    EXPECT_EQ(table.PreparedCount(), 1u);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);

    const harken::DeferredIntentToken poolToken{3, 9};
    ASSERT_TRUE(table.Commit(&prepared.token, poolToken));
    EXPECT_EQ(table.PreparedCount(), 0u);
    EXPECT_EQ(table.LiveCount(), 1u);
    EXPECT_EQ(table.HighWater(), 1u);

    harken::DeferredIntentClaim claim{};
    claim.token = poolToken;
    claim.intent.ownerDomain = harken::DeferredIntentOwnerDomain::Action;
    claim.intent.payload = payload;
    harken::ActionDeferredRecordToken claimedToken{};
    harken::ActionDeferredRecord captured{};
    ASSERT_TRUE(table.Claim(claim, &claimedToken, &captured));
    EXPECT_EQ(captured.executionOrdinal, 7u);
    EXPECT_EQ(table.State(claimedToken),
              harken::ActionDeferredRecordState::Claimed);

    EXPECT_EQ(table.RequestCancel(claimedToken),
              harken::ActionDeferredCancelOwnership::Claimant);
    EXPECT_EQ(table.State(claimedToken),
              harken::ActionDeferredRecordState::CancelRequested);
    EXPECT_TRUE(table.FinalizeClaim(claimedToken));
    EXPECT_FALSE(table.FinalizeClaim(claimedToken));
    EXPECT_EQ(table.LiveCount(), 0u);
}

TEST(ActionDeferredRecordTable, HeldCancellationHasOneCancellerFinalizer) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    auto prepared = table.Prepare(MakeRecord());
    ASSERT_EQ(prepared.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    const harken::ActionDeferredRecordToken heldToken = prepared.token;
    ASSERT_TRUE(table.Commit(&prepared.token, {4, 10}));

    EXPECT_EQ(table.RequestCancel(heldToken),
              harken::ActionDeferredCancelOwnership::Canceller);
    EXPECT_EQ(table.RequestCancel(heldToken),
              harken::ActionDeferredCancelOwnership::None);
    EXPECT_TRUE(table.FinalizeHeld(heldToken));
    EXPECT_FALSE(table.FinalizeHeld(heldToken));
    EXPECT_EQ(table.RequestCancel(heldToken),
              harken::ActionDeferredCancelOwnership::None);
    EXPECT_EQ(table.LiveCount(), 0u);
}

TEST(ActionDeferredRecordTable,
     PreparedAbandonPreservesGenerationUntilLiveFinalization) {
    harken::ActionDeferredRecordTable table;
    ASSERT_TRUE(table.Reset(1));
    ASSERT_TRUE(table.SetFreeGenerationForTests(0, UINT64_MAX));

    auto first = table.Prepare(MakeRecord());
    ASSERT_EQ(first.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    EXPECT_EQ(first.token.generation, UINT64_MAX);
    EXPECT_EQ(table.HighWater(), 0u);
    ASSERT_TRUE(table.Abandon(&first.token));
    EXPECT_EQ(table.PreparedCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);

    auto second = table.Prepare(MakeRecord());
    ASSERT_EQ(second.status,
              harken::ActionDeferredRecordPrepareStatus::Prepared);
    EXPECT_EQ(second.token.generation, UINT64_MAX);
    const harken::ActionDeferredRecordToken heldToken = second.token;
    ASSERT_TRUE(table.Commit(&second.token, {5, 11}));
    ASSERT_EQ(table.RequestCancel(heldToken),
              harken::ActionDeferredCancelOwnership::Canceller);
    ASSERT_TRUE(table.FinalizeHeld(heldToken));
    EXPECT_EQ(table.QuarantinedCount(), 1u);
    EXPECT_EQ(table.Prepare(MakeRecord()).status,
              harken::ActionDeferredRecordPrepareStatus::Capacity);
}

} // namespace
