#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"
#include "DeviceBackend.h"
#include "Common/TestUtil.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/TimelineExecutor.h"
#include "harken/abi/TimelineStopContract.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/TimelineRuntimeInstall.h"
#include "PlayableRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct DeviceBackendTestGuard {
    explicit DeviceBackendTestGuard(harken::DeviceBackendTestState& state) {
        harken::SetDeviceBackendTestState(&state);
    }
    ~DeviceBackendTestGuard() {
        harken::SetDeviceBackendTestState(nullptr);
    }
    DeviceBackendTestGuard(const DeviceBackendTestGuard&) = delete;
    DeviceBackendTestGuard& operator=(const DeviceBackendTestGuard&) = delete;
};

struct FreshPrepareDrainContext {
    harken::Engine* engine = nullptr;
    bool invoked = false;
};

void DrainFreshTimelineArm(void* context) {
    FreshPrepareDrainContext* drain =
        static_cast<FreshPrepareDrainContext*>(context);
    drain->invoked = true;
    drain->engine->mixer.DrainCommandsForTest();
}

class Sink final : public harken::TimelineExecutorSink {
public:
    bool BeginQuantum(std::uint64_t) noexcept override {
        ++beginCount;
        applied = false;
        count = 0;
        return begin;
    }
    bool AdmitQuantum(std::uint64_t) const noexcept override {
        if (requireDeadlineBeforeAdmit && deadlineFailureCount == 0) {
            admittedBeforeDeadline = true;
        }
        ++admitCount;
        return admit;
    }
    bool BindingValid(std::uint32_t, std::uint32_t) const noexcept override {
        return bindingValid;
    }
    bool ApplyTransaction(std::uint32_t, std::uint32_t,
                          std::uint64_t,
                          const harken::TimelineSinkEvent* events,
                          std::uint32_t eventCount,
                          harken::TimelineArmToken) noexcept override {
        if (!accept || events == nullptr || eventCount > 8) {
            return false;
        }
        applied = true;
        count = eventCount;
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            offsets[i] = events[i].effectiveOffset;
            orders[i] = events[i].order;
        }
        return true;
    }
    bool CancelBinding(std::uint32_t,
                       std::uint32_t) noexcept override {
        ++cancelCount;
        return true;
    }
    void FailPlan03Deadline(harken::DecisionToken decision,
                            std::uint64_t quantum) noexcept override {
        ++deadlineFailureCount;
        failedDecision = decision;
        failedAtQuantum = quantum;
        if (admitCount != 0) {
            deadlineAfterAdmit = true;
        }
    }
    void EndQuantum(std::uint64_t) noexcept override { ++endCount; }

    bool admit = true;
    bool begin = true;
    bool bindingValid = true;
    bool accept = true;
    bool applied = false;
    std::uint32_t count = 0;
    std::uint32_t cancelCount = 0;
    mutable std::uint32_t admitCount = 0;
    std::uint32_t deadlineFailureCount = 0;
    harken::DecisionToken failedDecision{};
    std::uint64_t failedAtQuantum = 0;
    bool requireDeadlineBeforeAdmit = false;
    mutable bool admittedBeforeDeadline = false;
    bool deadlineAfterAdmit = false;
    std::uint32_t beginCount = 0;
    std::uint32_t endCount = 0;
    std::array<std::uint32_t, 8> offsets{};
    std::array<harken::TimelineExecutionOrder, 8> orders{};
};

harken::ScheduledTransactionPool::Config PoolConfig(
    std::uint32_t events = 16,
    std::uint32_t transactions = 8) {
    harken::ScheduledTransactionPool::Config config{};
    config.quantumFrames = 256;
    config.scheduleHorizonFrames = 1024;
    config.publishLeadQuanta = 1;
    config.maxScheduledTransactions = transactions;
    config.maxTransactionSpanFrames = 512;
    config.maxIngressTransactionsPerQuantum = transactions;
    config.maxFrameBatchesPerQuantum = events;
    config.maxEventsPerQuantum = events;
    config.maxTransactionEvents = events;
    return config;
}

struct FiledTimeline {
    FiledTimeline() = default;
    FiledTimeline(const FiledTimeline&) = delete;
    FiledTimeline& operator=(const FiledTimeline&) = delete;
    harken::TimelineSemanticTarget target{};
    std::array<harken::TimelineEventPayload, 9> payloads{};
    std::array<harken::ScheduledEventInput, 9> events{};
    harken::TimelineProgramView program{};
    harken::TimelineArmToken arm{};
};

void FileTimeline(
    FiledTimeline& filed,
    harken::ScheduledTransactionPool& pool, harken::TimelineArmTable& arms,
    std::uint32_t eventCount, std::uint64_t frame = 1,
    harken::TimelineEventKind kind = harken::TimelineEventKind::Start,
    std::uint32_t phase = 20) {
    filed.target.programSemanticFingerprint = 91;
    filed.target.clipGuidHigh = 1;
    filed.target.clipGuidLow = 2;
    filed.target.subjectGuidHigh = 3;
    filed.target.subjectGuidLow = 4;
    filed.target.trackId = harkentest::ObjectId(5);
    filed.target.clipId = harkentest::ObjectId(6);
    filed.target.subjectId = harkentest::ObjectId(7);
    filed.target.occurrenceOrdinal = 8;
    for (std::uint32_t i = 0; i < eventCount; ++i) {
        filed.payloads[i].kind = kind;
        filed.payloads[i].target = filed.target;
    }
    filed.program = {1, filed.payloads.data(), eventCount};
    EXPECT_TRUE(arms.Reserve(0, 1, frame, 0, 1, filed.target, &filed.arm));
    EXPECT_TRUE(arms.AcknowledgeApplied(filed.arm));
    EXPECT_TRUE(arms.Publish(filed.arm));
    for (std::uint32_t i = 0; i < eventCount; ++i) {
        filed.events[i].executeFrame = frame;
        filed.events[i].eventIndex = i;
        filed.events[i].subeventPhase = phase;
        filed.events[i].payload = i;
    }
    harken::TransactionRequest request{};
    request.transactionId = 1;
    request.transportEpoch = 1;
    request.program = {1, &filed.program};
    request.armToken = filed.arm;
    request.events = filed.events.data();
    request.eventCount = eventCount;
    EXPECT_EQ(pool.Reserve(request,
                           harken::PrePublishLatePolicy::Refuse)
                  .status,
              harken::ReserveStatus::Committed);
    pool.Publish(256);
}

TEST(TimelineExecutor, DeadlinePrepassRunsBeforeAdmissionAndFailsOnce) {
    auto config = PoolConfig();
    config.maxPlan03GroupHeadersPerPage = 2;
    config.maxArmsPerGroup = 2;
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, 1));
    harken::TimelineArmTable arms(2);
    harken::DecisionTable decisions(1, 2);
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = 1;
    request.world = 3;
    request.transport = 1;
    request.expectedContinuationGeneration = 5;
    request.sliceCount = 2;
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(request, &decision));

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    const harken::ParentAudioGroupRef parent{decision};
    harken::TimelineArmToken members[2]{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 256, 13, target, parent, 0, &members[0]));
    ASSERT_TRUE(arms.ReserveGrouped(
        7, 11, 0, 256, 13, target, parent, 1, &members[1]));

    harken::DecisionParticipantRef participants[] = {
        {harken::DecisionParticipantKind::TimelineArm,
         members[0].slot, members[0].generation},
        {harken::DecisionParticipantKind::TimelineArm,
         members[1].slot, members[1].generation},
    };
    harken::DecisionCommitPayload payload{};
    payload.participants = participants;
    payload.participantCount = 2;
    payload.pages = {17, 19};
    payload.armGroup = {23, 29};
    ASSERT_TRUE(decisions.Commit(decision, payload));

    harken::Plan03GroupSliceInput slice{};
    slice.executeFrame = 0;
    slice.decision = decision;
    slice.members = members;
    slice.memberCount = 2;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(slice, arms));
    slice.sliceIndex = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(slice, arms));
    pool.Publish(256, nullptr, nullptr, &arms);

    Sink sink;
    sink.requireDeadlineBeforeAdmit = true;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink,
                               &decisions));
    harken::TimelineExecutionPlan plan{};
    plan.expectedFrontier = 0;
    plan.maxPages = 1;
    const auto result = executor.ExecuteQuantum(1, plan);

    EXPECT_EQ(1u, result.consumedPageCount);
    EXPECT_EQ(1u, sink.deadlineFailureCount);
    EXPECT_EQ(decision.slot, sink.failedDecision.slot);
    EXPECT_EQ(decision.generation, sink.failedDecision.generation);
    EXPECT_EQ(1u, sink.failedAtQuantum);
    EXPECT_FALSE(sink.deadlineAfterAdmit);
    EXPECT_FALSE(sink.admittedBeforeDeadline);
    EXPECT_EQ(1u, sink.admitCount);
    EXPECT_EQ(2u, sink.cancelCount);

    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::DeadlineInvalidated,
              snapshot.audioState);
    harken::Plan03PageGroupView groups[2]{};
    ASSERT_EQ(2u, pool.CapturePlan03PageGroupsRt(0, groups, 2));
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated,
              groups[0].lease->Capture().state);
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated,
              groups[1].lease->Capture().state);
}

TEST(TimelineArmTable, PreparedCommitProofHoldsRealMutationUntilMoveAndRelease) {
    using Proof = harken::TimelineArmTable::PreparedCommitProof;
    static_assert(!std::is_copy_constructible_v<Proof>);
    static_assert(!std::is_copy_assignable_v<Proof>);
    static_assert(std::is_nothrow_move_constructible_v<Proof>);
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.startFrame = 256;
    input.armDeadline = 192;
    input.programGeneration = 7;
    input.parentGroup = {{3, 4}};
    input.memberIndex = 1;
    ASSERT_TRUE(table.PrepareGrouped(0, 9, input.startFrame, input.armDeadline,
        input.programGeneration, input.target, input.parentGroup, input.memberIndex,
        &input.arm));
    Proof held;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &held));
    bool competingCommit = true;
    bool competingCancel = true;
    bool competingPin = true;
    std::thread competitor([&] {
        competingCommit = table.CommitPrepared(input.arm);
        competingCancel = table.CancelPrepared(input.arm);
        harken::TimelineArmReaderPin pin;
        competingPin = table.PinFrontierReader(input.arm, &pin);
    });
    competitor.join();
    EXPECT_FALSE(competingCommit);
    EXPECT_FALSE(competingCancel);
    EXPECT_FALSE(competingPin);
    EXPECT_FALSE(table.AcknowledgeApplied(input.arm, 1));
    EXPECT_FALSE(table.Publish(input.arm));
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(table.Observe(input.arm, &observed));
    EXPECT_EQ(harken::ArmState::Prepared, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.tombstoneAudioSequence);
    Proof moved(std::move(held));
    EXPECT_FALSE(held.Valid());
    EXPECT_TRUE(moved.Valid());
    held.Release();
    EXPECT_FALSE(table.CancelPrepared(input.arm));
    EXPECT_FALSE(table.ProvePreparedCommit(input, &moved));
    moved.Release();
    ASSERT_TRUE(table.CancelPrepared(input.arm));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(TimelineArmTable, PreparedCommitProofChecksCompleteIdentityAndOriginalGeometry) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.startFrame = 256;
    input.armDeadline = 192;
    input.programGeneration = 7;
    input.target.programSemanticFingerprint = 19;
    input.parentGroup = {{3, 4}};
    input.memberIndex = 1;
    ASSERT_TRUE(table.PrepareGrouped(0, 9, input.startFrame, input.armDeadline,
        input.programGeneration, input.target, input.parentGroup, input.memberIndex,
        &input.arm));
    for (std::uint32_t field = 0; field < 14; ++field) {
        SCOPED_TRACE(field);
        auto wrong = input;
        switch (field) {
        case 0: ++wrong.arm.slot; break;
        case 1: ++wrong.arm.generation; break;
        case 2: ++wrong.arm.renderSlot; break;
        case 3: ++wrong.arm.renderGeneration; break;
        case 4: ++wrong.startFrame; break;
        case 5: ++wrong.armDeadline; break;
        case 6: ++wrong.programGeneration; break;
        case 7: ++wrong.target.programSemanticFingerprint; break;
        case 8: ++wrong.target.occurrenceOrdinal; break;
        case 9: ++wrong.parentGroup.decision.slot; break;
        case 10: ++wrong.parentGroup.decision.generation; break;
        case 11: ++wrong.memberIndex; break;
        case 12: wrong.pendingImmediateStart = true; break;
        case 13: wrong.parentGroup = {}; break;
        }
        harken::TimelineArmTable::PreparedCommitProof rejected;
        EXPECT_FALSE(table.ProvePreparedCommit(wrong, &rejected));
        EXPECT_FALSE(rejected.Valid());
        ASSERT_NE(nullptr, table.Resolve(input.arm));
        EXPECT_EQ(harken::ArmState::Prepared, table.Resolve(input.arm)->state.load());
    }
    harken::TimelineArmTable::PreparedCommitProof valid;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &valid));
    valid.Release();
    ASSERT_TRUE(table.CancelPrepared(input.arm));
    input.pendingImmediateStart = true;
    input.startFrame = 0;
    input.armDeadline = 0;
    ASSERT_TRUE(table.PreparePendingImmediateGrouped(0, 10,
        input.programGeneration, input.target, input.parentGroup, input.memberIndex,
        &input.arm));
    auto wrongBound = input;
    wrongBound.startFrame = 256;
    wrongBound.armDeadline = 256;
    EXPECT_FALSE(table.ProvePreparedCommit(wrongBound, &valid));
    ASSERT_TRUE(table.ProvePreparedCommit(input, &valid));
    valid.Release();
    ASSERT_TRUE(table.CancelPrepared(input.arm));
}

TEST(TimelineArmTable, PreparedCommitProofConsumesOnceThenUsesRealAckAndRetirement) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.pendingImmediateStart = true;
    ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof held;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &held));
    // A caller changing its input after acquisition cannot redirect consumption.
    auto original = input;
    input.arm = {};
    table.ConsumePreparedCommit(std::move(held));
    EXPECT_FALSE(held.Valid());
    ASSERT_NE(nullptr, table.Resolve(original.arm));
    EXPECT_EQ(harken::ArmState::PendingApply, table.Resolve(original.arm)->state.load());
    EXPECT_FALSE(table.ProvePreparedCommit(original, &held));
    ASSERT_TRUE(table.AcknowledgeApplied(original.arm, 2));
    EXPECT_FALSE(table.ProvePreparedCommit(original, &held));
    ASSERT_TRUE(table.BindPendingImmediateStart(original.arm, 512, 512));
    ASSERT_TRUE(table.Publish(original.arm));
    EXPECT_FALSE(table.ProvePreparedCommit(original, &held));
    ASSERT_TRUE(table.Start(original.arm, 3));
    ASSERT_TRUE(table.Tombstone(original.arm, 4));
    EXPECT_FALSE(table.ProvePreparedCommit(original, &held));
    EXPECT_EQ(1u, table.ReclaimTombstoned(5));
    ASSERT_TRUE(table.PreparePendingImmediate(0, 10, 7, {}, &input.arm));
    EXPECT_FALSE(table.ProvePreparedCommit(original, &held));
    held.Release();
    ASSERT_TRUE(table.CancelPrepared(input.arm));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(TimelineArmTable, PreparedCommitProofReaderAndSecondRowContentionKeepPreparedRights) {
    harken::TimelineArmTable table(2);
    std::array<harken::TimelineArmTable::PreparedCommitInput, 2> inputs{};
    for (std::uint32_t index = 0; index < inputs.size(); ++index) {
        inputs[index].programGeneration = 7;
        inputs[index].pendingImmediateStart = true;
        ASSERT_TRUE(table.PreparePendingImmediate(index, 9, 7, {}, &inputs[index].arm));
    }
    harken::TimelineArmReaderPin reader;
    ASSERT_TRUE(table.PinFrontierReader(inputs[1].arm, &reader));
    harken::TimelineArmTable::PreparedCommitProof second;
    EXPECT_FALSE(table.ProvePreparedCommit(inputs[1], &second));
    ASSERT_TRUE(reader.Release(0));
    ASSERT_TRUE(table.ProvePreparedCommit(inputs[1], &second));
    {
        harken::TimelineArmTable::PreparedCommitProof first;
        harken::TimelineArmTable::PreparedCommitProof refused;
        ASSERT_TRUE(table.ProvePreparedCommit(inputs[0], &first));
        EXPECT_FALSE(table.ProvePreparedCommit(inputs[1], &refused));
        EXPECT_FALSE(refused.Valid());
    }
    // Acquiring a prefix and refusing the next row creates no PendingApply state.
    ASSERT_TRUE(table.CancelPrepared(inputs[0].arm));
    EXPECT_FALSE(table.CancelPrepared(inputs[1].arm));
    second.Release();
    ASSERT_TRUE(table.ProvePreparedCommit(inputs[1], &second));
    second.Release();
    ASSERT_TRUE(table.CancelPrepared(inputs[1].arm));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(TimelineArmTable, OriginalRowOwnerRequiresSameTableActualPreparedProof) {
    using Owner = harken::TimelineArmTable::OriginalArmRowOwner;
    static_assert(!std::is_copy_constructible_v<Owner>);
    static_assert(!std::is_copy_assignable_v<Owner>);
    static_assert(std::is_nothrow_move_constructible_v<Owner>);
    harken::TimelineArmTable table(1), foreign(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.pendingImmediateStart = true;
    ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
    auto otherInput = input;
    ASSERT_TRUE(foreign.PreparePendingImmediate(0, 9, 7, {}, &otherInput.arm));
    ASSERT_EQ(input.arm.slot, otherInput.arm.slot);
    ASSERT_EQ(input.arm.generation, otherInput.arm.generation);
    ASSERT_EQ(input.arm.renderSlot, otherInput.arm.renderSlot);
    ASSERT_EQ(input.arm.renderGeneration, otherInput.arm.renderGeneration);
    harken::TimelineArmTable::PreparedCommitProof proof, otherProof, empty;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
    ASSERT_TRUE(foreign.ProvePreparedCommit(otherInput, &otherProof));
    Owner owner, refused;
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(empty, &refused));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(otherProof, &refused));
    EXPECT_FALSE(foreign.IssueOriginalArmRowOwner(proof, &refused));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, nullptr));
    ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &owner));
    EXPECT_EQ(&table, owner.Owner());
    EXPECT_EQ(input.arm.slot, owner.Token().slot);
    EXPECT_EQ(input.arm.generation, owner.Token().generation);
    EXPECT_EQ(input.arm.renderSlot, owner.Token().renderSlot);
    EXPECT_EQ(input.arm.renderGeneration, owner.Token().renderGeneration);
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &owner));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &refused));
    EXPECT_TRUE(proof.Valid());
    owner.Release();
    proof.Release(); otherProof.Release();
    EXPECT_TRUE(table.CancelPrepared(input.arm));
    EXPECT_TRUE(foreign.CancelPrepared(otherInput.arm));
}

TEST(TimelineArmTable, OriginalRowOwnerDropAndProofAbortDoNotRemintOriginalGeneration) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.pendingImmediateStart = true;
    ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
    const auto original = input.arm;
    harken::TimelineArmTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
    harken::TimelineArmTable::OriginalArmRowOwner owner, next;
    ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &owner));
    proof.Release();
    EXPECT_FALSE(table.CancelPrepared(original));
    ASSERT_TRUE(table.Resolve(original)->originalLifetimePinned.load());
    owner.Release(); owner.Release();
    ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &next));
    proof.Release();
    ASSERT_TRUE(table.CancelPrepared(original));
    ASSERT_TRUE(table.PreparePendingImmediate(0, 10, 7, {}, &input.arm));
    ASSERT_EQ(original.generation + 1u, input.arm.generation);
    EXPECT_EQ(nullptr, table.Resolve(original));
    ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
    ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &next));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &owner));
    proof.Release(); next.Release();
    EXPECT_TRUE(table.CancelPrepared(input.arm));
}

TEST(TimelineArmTable, OriginalRowOwnerMoveAndDropRetainIndependentPreparedRows) {
    harken::TimelineArmTable table(2);
    std::array<harken::TimelineArmTable::PreparedCommitInput, 2> inputs{};
    std::array<harken::TimelineArmTable::OriginalArmRowOwner, 2> owners;
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
        inputs[i].programGeneration = 7;
        inputs[i].pendingImmediateStart = true;
        ASSERT_TRUE(table.PreparePendingImmediate(i, 9, 7, {}, &inputs[i].arm));
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(table.ProvePreparedCommit(inputs[i], &proof));
        ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &owners[i]));
    }
    harken::TimelineArmTable::OriginalArmRowOwner moved(std::move(owners[0]));
    EXPECT_FALSE(owners[0].Valid());
    moved = std::move(moved);
    EXPECT_TRUE(moved.Valid());
    moved = std::move(owners[1]);
    EXPECT_FALSE(owners[1].Valid());
    EXPECT_FALSE(table.Resolve(inputs[0].arm)->originalLifetimePinned.load());
    EXPECT_TRUE(table.Resolve(inputs[1].arm)->originalLifetimePinned.load());
    EXPECT_TRUE(table.CancelPrepared(inputs[0].arm));
    EXPECT_FALSE(table.CancelPrepared(inputs[1].arm));
    moved.Release();
    EXPECT_TRUE(table.CancelPrepared(inputs[1].arm));
    EXPECT_EQ(2u, table.FreeCount());

    ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &inputs[0].arm));
    {
        harken::TimelineArmTable::OriginalArmRowOwner scoped;
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(table.ProvePreparedCommit(inputs[0], &proof));
        ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &scoped));
        proof.Release();
        EXPECT_FALSE(table.CancelPrepared(inputs[0].arm));
        EXPECT_TRUE(table.Resolve(inputs[0].arm)->originalLifetimePinned.load());
    }
    EXPECT_FALSE(table.Resolve(inputs[0].arm)->originalLifetimePinned.load());
    EXPECT_TRUE(table.CancelPrepared(inputs[0].arm));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(TimelineArmTable, OriginalRowOwnerAllowsRealAckBindStartButBlocksPrematureReclaim) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.pendingImmediateStart = true;
    ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
    harken::TimelineArmTable::OriginalArmRowOwner owner;
    ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &owner));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &owner));
    harken::TimelineArmTable::SourceCommandRowProof ack;
    ASSERT_EQ(harken::TimelineArmTable::SourceCommandRowStatus::Ready,
        table.ProvePendingSourceCommand(input.arm, &ack));
    harken::TimelineArmTable::AcknowledgeSourceCommand(ack, 2);
    ack.Release();
    ASSERT_TRUE(table.BindPendingImmediateStart(input.arm, 512, 512));
    ASSERT_TRUE(table.Publish(input.arm));
    ASSERT_TRUE(table.Start(input.arm, 3));
    ASSERT_TRUE(table.Tombstone(input.arm, 4));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(input.arm, &observation));
    EXPECT_EQ(harken::ArmState::Tombstoned, observation.state);
    EXPECT_EQ(0u, table.RowAt(input.arm.slot)->frontierReaderPins.load());
    EXPECT_FALSE(table.Reclaim(input.arm, 100));
    ASSERT_TRUE(table.Observe(input.arm, &observation));
    EXPECT_EQ(harken::ArmState::Tombstoned, observation.state);
    owner.Release();
    EXPECT_FALSE(table.Reclaim(input.arm, 4));
    EXPECT_TRUE(table.Reclaim(input.arm, 5));
    EXPECT_EQ(nullptr, table.Resolve(input.arm));
}

TEST(TimelineArmTable, OriginalRowOwnerAllowsCompletePrivateUnboundStartBinding) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.pendingImmediateStart = true;
    ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof commit;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &commit));
    harken::TimelineArmTable::OriginalArmRowOwner owner;
    ASSERT_TRUE(table.IssueOriginalArmRowOwner(commit, &owner));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(commit));
    ASSERT_TRUE(table.AcknowledgeApplied(input.arm, 2));
    harken::TimelineArmReaderPin reader;
    harken::TimelineArmTable::PrivatePendingImmediateArmInput binding{
        input.arm, 512, 512, &reader, nullptr, nullptr};
    std::array<harken::TimelineArmTable::Row*, 1> held{};
    harken::TimelineArmTable::PrivatePendingImmediateArmProof proof;
    ASSERT_TRUE(table.ProvePrivatePendingImmediateArmBatch(&binding, 1,
        held.data(), held.size(), &proof));
    table.ApplyPrivatePendingImmediateArmBatch(proof);
    ASSERT_TRUE(reader.Valid());
    EXPECT_TRUE(owner.Valid());
    ASSERT_NE(nullptr, table.Resolve(input.arm));
    EXPECT_TRUE(table.Resolve(input.arm)->originalLifetimePinned.load());
    EXPECT_EQ(1u, table.Resolve(input.arm)->frontierReaderPins.load());
    EXPECT_EQ(512u, table.Resolve(input.arm)->startFrame);
    EXPECT_EQ(harken::ArmState::Published, table.Resolve(input.arm)->state.load());
    ASSERT_TRUE(table.RequestCancel(input.arm));
    ASSERT_TRUE(table.Tombstone(input.arm, 4));
    ASSERT_TRUE(reader.Release(5));
    EXPECT_FALSE(table.Reclaim(input.arm, 5));
    owner.Release();
    EXPECT_TRUE(table.Reclaim(input.arm, 5));
}

TEST(TimelineArmTable, OriginalRowOwnerGenericPreparedLifetimeDoesNotMintFrontierAdmission) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmTable::PreparedCommitInput input{};
    input.programGeneration = 7;
    input.startFrame = 300;
    input.armDeadline = 100;
    ASSERT_TRUE(table.Prepare(9, 11, 300, 100, 7, {}, &input.arm));
    harken::TimelineArmTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
    harken::TimelineArmTable::OriginalArmRowOwner owner;
    ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &owner));
    proof.Release();
    const auto* row = table.Resolve(input.arm);
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(0u, row->frontierReaderPins.load());
    EXPECT_EQ(nullptr, row->frontierAdmission);
    EXPECT_EQ(nullptr, row->frontierAdmissionLease);
    EXPECT_EQ(harken::TimelineArmTable::AuthoredControlStage::None, row->authoredControl.stage);
    EXPECT_EQ(300u, row->startFrame);
    EXPECT_FALSE(table.CancelPrepared(input.arm));
    owner.Release();
    EXPECT_TRUE(table.CancelPrepared(input.arm));
}

TEST(TimelineArmTable, OriginalRowOwnerMaximumGenerationQuarantinesOnlyAfterTrueDrop) {
    for (const bool committed : {false, true}) {
        harken::TimelineArmTable table(1);
        table.RowAt(0)->generation = UINT32_MAX;
        harken::TimelineArmTable::PreparedCommitInput input{};
        input.programGeneration = 7;
        input.pendingImmediateStart = true;
        ASSERT_TRUE(table.PreparePendingImmediate(0, 9, 7, {}, &input.arm));
        ASSERT_EQ(UINT32_MAX, input.arm.generation);
        harken::TimelineArmTable::PreparedCommitProof proof;
        ASSERT_TRUE(table.ProvePreparedCommit(input, &proof));
        harken::TimelineArmTable::OriginalArmRowOwner owner;
        ASSERT_TRUE(table.IssueOriginalArmRowOwner(proof, &owner));
        if (committed) {
            harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
            ASSERT_TRUE(table.RequestCancel(input.arm));
            ASSERT_TRUE(table.Tombstone(input.arm, 4));
            EXPECT_FALSE(table.Reclaim(input.arm, 5));
            EXPECT_EQ(harken::ArmState::Tombstoned, table.RowAt(0)->state.load());
        } else {
            proof.Release();
            EXPECT_FALSE(table.CancelPrepared(input.arm));
            EXPECT_EQ(harken::ArmState::Prepared, table.RowAt(0)->state.load());
        }
        owner.Release();
        if (committed) EXPECT_FALSE(table.Reclaim(input.arm, 5));
        else EXPECT_TRUE(table.CancelPrepared(input.arm));
        EXPECT_EQ(harken::ArmState::Quarantined, table.RowAt(0)->state.load());
        EXPECT_EQ(UINT32_MAX, table.RowAt(0)->generation);
        EXPECT_EQ(0u, table.FreeCount());
        harken::TimelineArmToken refused{};
        EXPECT_FALSE(table.PreparePendingImmediate(0, 10, 7, {}, &refused));
    }
}

TEST(TimelineArmTable, CapacityAt95_100_101) {
    constexpr std::uint32_t kCapacity = 20;
    harken::TimelineArmTable table(kCapacity);
    std::array<harken::TimelineArmToken, kCapacity> tokens{};
    for (std::uint32_t slot = 0; slot < 19; ++slot) {
        EXPECT_TRUE(table.Reserve(slot, 1, 256, 256, 1, {}, &tokens[slot]));
    }
    EXPECT_EQ(table.LiveCount(), 19u);
    EXPECT_TRUE(table.Reserve(19, 1, 256, 256, 1, {}, &tokens[19]));
    EXPECT_EQ(table.LiveCount(), kCapacity);
    harken::TimelineArmToken overflow{};
    EXPECT_FALSE(table.Reserve(20, 1, 256, 256, 1, {}, &overflow));
    EXPECT_FALSE(overflow.Valid());
    EXPECT_EQ(table.LiveCount(), kCapacity);
}

TEST(TimelineArmTable, PendingImmediateBindsOnlyAfterAckAndBeforePublication) {
    harken::TimelineArmTable table(2);
    harken::TimelineArmToken direct{};
    harken::TimelineArmToken grouped{};
    ASSERT_TRUE(table.ReservePendingImmediate(0, 1, 7, {}, &direct));
    ASSERT_TRUE(table.PreparePendingImmediateGrouped(
        1, 1, 7, {}, harken::ParentAudioGroupRef{{3, 4}}, 0, &grouped));
    ASSERT_TRUE(table.CommitPrepared(grouped));
    EXPECT_FALSE(table.Publish(direct));
    EXPECT_EQ(harken::ArmPublishStatus::Invalid,
              table.PublishExact(direct));
    EXPECT_FALSE(table.BindPendingImmediateStart(direct, 512, 512));
    ASSERT_TRUE(table.AcknowledgeApplied(direct));
    ASSERT_TRUE(table.AcknowledgeApplied(grouped));
    EXPECT_FALSE(table.Publish(grouped));
    EXPECT_EQ(harken::ArmPublishStatus::Invalid,
              table.PublishExact(grouped));
    ASSERT_TRUE(table.BindPendingImmediateStart(direct, 512, 512));
    ASSERT_TRUE(table.BindPendingImmediateStart(grouped, 768, 768));
    EXPECT_FALSE(table.BindPendingImmediateStart(direct, 1024, 1024));
    const auto* row = table.Resolve(grouped);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->startFrame, 768u);
    EXPECT_EQ(row->armDeadline, 768u);
    EXPECT_EQ(row->parentGroup.decision.slot, 3u);
    ASSERT_TRUE(table.Publish(direct));
    ASSERT_TRUE(table.Publish(grouped));
    EXPECT_FALSE(table.BindPendingImmediateStart(grouped, 1024, 1024));
    ASSERT_TRUE(table.RequestCancel(direct));
    EXPECT_FALSE(table.BindPendingImmediateStart(direct, 1024, 1024));
}

TEST(TimelineArmTable, NeverPublishedCancellationRejectsAudioVisibleArms) {
    harken::TimelineArmTable table(3);
    harken::TimelineArmToken pending{};
    harken::TimelineArmToken armed{};
    harken::TimelineArmToken published{};
    ASSERT_TRUE(table.ReservePendingImmediate(0, 1, 7, {}, &pending));
    ASSERT_TRUE(table.ReservePendingImmediate(1, 1, 7, {}, &armed));
    ASSERT_TRUE(table.ReservePendingImmediate(2, 1, 7, {}, &published));
    ASSERT_TRUE(table.AcknowledgeApplied(armed));
    ASSERT_TRUE(table.AcknowledgeApplied(published));

    EXPECT_TRUE(table.RequestCancelNeverPublished(pending));
    EXPECT_TRUE(table.RequestCancelNeverPublished(armed));
    EXPECT_FALSE(table.RequestCancelNeverPublished(pending));
    EXPECT_FALSE(table.ClaimAudioSlice(pending));
    EXPECT_FALSE(table.ClaimAudioSlice(armed));
    ASSERT_TRUE(table.Tombstone(pending, 4));
    ASSERT_TRUE(table.Reclaim(pending, 5));
    harken::TimelineArmToken replacement{};
    ASSERT_TRUE(table.ReservePendingImmediate(0, 2, 9, {}, &replacement));
    EXPECT_FALSE(table.RequestCancelNeverPublished(pending));
    EXPECT_NE(pending.generation, replacement.generation);

    ASSERT_TRUE(table.BindPendingImmediateStart(published, 512, 512));
    EXPECT_FALSE(table.RequestCancelNeverPublished(published));
    ASSERT_TRUE(table.Publish(published));
    EXPECT_FALSE(table.RequestCancelNeverPublished(published));
    ASSERT_TRUE(table.RequestCancel(published));
    EXPECT_FALSE(table.RequestCancelNeverPublished(published));
    EXPECT_TRUE(table.ClaimAudioSlice(published));
    EXPECT_FALSE(table.RequestCancelNeverPublished(published));
    ASSERT_TRUE(table.Start(published, 3));
    EXPECT_FALSE(table.RequestCancelNeverPublished(published));
}

TEST(TimelineArmTable, NeverPublishedCancellationAndPrivateApplyContestExactRow) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(table.ReservePendingImmediate(0, 1, 7, {}, &arm));
    ASSERT_TRUE(table.AcknowledgeApplied(arm));
    harken::TimelineArmReaderPin pin{};
    harken::TimelineArmTable::PrivatePendingImmediateArmInput input{};
    input.arm = arm;
    input.startFrame = 512;
    input.armDeadline = 512;
    input.pin = &pin;
    harken::TimelineArmTable::Row* reserved = nullptr;

    struct Gate {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool proceed = false;
    } gate;
    table.SetNeverPublishedCancelHookForTests(+[](void* context) {
        auto& state = *static_cast<Gate*>(context);
        std::unique_lock<std::mutex> lock(state.mutex);
        state.entered = true;
        state.condition.notify_all();
        state.condition.wait(lock, [&] { return state.proceed; });
    }, &gate);
    bool cancelled = false;
    std::thread cancellation([&] {
        cancelled = table.RequestCancelNeverPublished(arm);
    });
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        EXPECT_TRUE(gate.condition.wait_for(lock, std::chrono::seconds(5),
                                            [&] { return gate.entered; }));
    }
    harken::TimelineArmTable::PrivatePendingImmediateArmProof blocked;
    EXPECT_FALSE(table.ProvePrivatePendingImmediateArmBatch(
        &input, 1, &reserved, 1, &blocked));
    EXPECT_EQ(harken::ArmPublishStatus::DeferredReservation,
              table.PublishExact(arm));
    EXPECT_FALSE(table.ClaimAudioSlice(arm));
    EXPECT_FALSE(table.Start(arm, 2));
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.proceed = true;
    }
    gate.condition.notify_all();
    cancellation.join();
    table.SetNeverPublishedCancelHookForTests(nullptr, nullptr);
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(table.ClaimAudioSlice(arm));
    table.ApplyClaimedAudioSlice(arm);
    harken::ArmState cancelledState = harken::ArmState::Free;
    ASSERT_TRUE(table.State(arm, &cancelledState));
    EXPECT_EQ(harken::ArmState::CancelRequested, cancelledState);
    EXPECT_FALSE(table.Start(arm, 3));
    EXPECT_FALSE(table.Publish(arm));
    EXPECT_FALSE(table.ProvePrivatePendingImmediateArmBatch(
        &input, 1, &reserved, 1, &blocked));

    harken::TimelineArmTable applied(1);
    ASSERT_TRUE(applied.ReservePendingImmediate(0, 2, 9, {}, &arm));
    ASSERT_TRUE(applied.AcknowledgeApplied(arm));
    input.arm = arm;
    harken::TimelineArmTable::PrivatePendingImmediateArmProof proof;
    ASSERT_TRUE(applied.ProvePrivatePendingImmediateArmBatch(
        &input, 1, &reserved, 1, &proof));
    bool lateCancellation = true;
    std::thread late([&] {
        lateCancellation = applied.RequestCancelNeverPublished(arm);
    });
    late.join();
    EXPECT_FALSE(lateCancellation);
    applied.ApplyPrivatePendingImmediateArmBatch(proof);
    EXPECT_FALSE(applied.RequestCancelNeverPublished(arm));
    EXPECT_TRUE(applied.ClaimAudioSlice(arm));
    EXPECT_TRUE(applied.Start(arm, 3));
    EXPECT_FALSE(applied.RequestCancelNeverPublished(arm));
}

TEST(TimelineArmTable, PrivatePendingBatchReservesAllRowsBeforeBindingOrPublication) {
    harken::TimelineArmTable table(2);
    std::array<harken::TimelineArmToken, 2> arms{};
    for (std::uint32_t index = 0; index < arms.size(); ++index) {
        ASSERT_TRUE(table.ReservePendingImmediate(index, 1, 7, {},
                                                  &arms[index]));
        ASSERT_TRUE(table.AcknowledgeApplied(arms[index]));
    }
    std::array<harken::TimelineArmReaderPin, 2> pins{};
    std::array<harken::TimelineArmTable::PrivatePendingImmediateArmInput, 2>
        inputs{};
    std::array<harken::TimelineArmTable::Row*, 2> rows{};
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        inputs[index].arm = arms[index];
        inputs[index].startFrame = 512 + index * 256;
        inputs[index].armDeadline = inputs[index].startFrame;
        inputs[index].pin = &pins[index];
    }
    {
        harken::TimelineArmTable::PrivatePendingImmediateArmProof blocker;
        harken::TimelineArmTable::Row* blockedRow = nullptr;
        ASSERT_TRUE(table.ProvePrivatePendingImmediateArmBatch(
            &inputs[1], 1, &blockedRow, 1, &blocker));
        harken::TimelineArmTable::PrivatePendingImmediateArmProof rejected;
        EXPECT_FALSE(table.ProvePrivatePendingImmediateArmBatch(
            inputs.data(), inputs.size(), rows.data(), rows.size(),
            &rejected));
        EXPECT_FALSE(rejected.Valid());
        EXPECT_EQ(nullptr, rows[0]);
        EXPECT_FALSE(table.Publish(arms[0]));
        EXPECT_TRUE(table.RequestCancel(arms[0]));
        EXPECT_FALSE(table.RequestCancel(arms[1]));
    }
    EXPECT_TRUE(table.RequestCancel(arms[1]));
    harken::TimelineArmTable::PrivatePendingImmediateArmProof cancelled;
    EXPECT_FALSE(table.ProvePrivatePendingImmediateArmBatch(
        inputs.data(), inputs.size(), rows.data(), rows.size(),
        &cancelled));

    harken::TimelineArmTable successful(2);
    for (std::uint32_t index = 0; index < arms.size(); ++index) {
        ASSERT_TRUE(successful.ReservePendingImmediate(index, 2, 9, {},
                                                       &arms[index]));
        ASSERT_TRUE(successful.AcknowledgeApplied(arms[index]));
        inputs[index].arm = arms[index];
    }
    harken::TimelineArmTable::PrivatePendingImmediateArmProof proof;
    ASSERT_TRUE(successful.ProvePrivatePendingImmediateArmBatch(
        inputs.data(), inputs.size(), rows.data(), rows.size(), &proof));
    EXPECT_FALSE(successful.RequestCancel(arms[1]));
    EXPECT_FALSE(successful.RequestCancelNeverPublished(arms[1]));
    successful.ApplyPrivatePendingImmediateArmBatch(proof);
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(successful.RequestCancelNeverPublished(arms[1]));
    for (std::size_t index = 0; index < arms.size(); ++index) {
        const auto* row = successful.Resolve(arms[index]);
        ASSERT_NE(nullptr, row);
        EXPECT_EQ(harken::ArmState::Published, row->state.load());
        EXPECT_EQ(inputs[index].startFrame, row->startFrame);
        EXPECT_EQ(1u, row->frontierReaderPins.load());
        EXPECT_TRUE(pins[index].Valid());
        ASSERT_TRUE(pins[index].Release(0));
    }
}

TEST(TimelineArmTable, PrivatePendingBatchBindsGroupedAdmissionAfterReaderPin) {
    harken::TimelineArmTable table(1);
    harken::AudioSliceLease lease;
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::EntryMember;
    binding.entry = {{1, 1}, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
    binding.primary = {2, 1, {1, 1}, 0, 0, 1, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.group = {3, 1, {3, 4}, 0, 0, 1, 1};
    binding.operation.order = {512, 1};
    binding.operation.world = 1;
    binding.operation.transport = 1;
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(table.ReservePendingImmediateGrouped(
        0, 1, 7, {}, harken::ParentAudioGroupRef{{3, 4}}, 0, &arm));
    ASSERT_TRUE(table.AcknowledgeApplied(arm));
    harken::TimelineArmReaderPin pin;
    harken::TimelineArmTable::PrivatePendingImmediateArmInput input{};
    input.arm = arm;
    input.startFrame = 512;
    input.armDeadline = 512;
    input.pin = &pin;
    input.admission = &binding;
    input.lease = &lease;
    harken::TimelineArmTable::Row* rowScratch = nullptr;
    harken::TimelineArmTable::PrivatePendingImmediateArmProof proof;
    ASSERT_TRUE(table.ProvePrivatePendingImmediateArmBatch(
        &input, 1, &rowScratch, 1, &proof));
    EXPECT_EQ(0u, table.Resolve(arm)->frontierReaderPins.load());
    table.ApplyPrivatePendingImmediateArmBatch(proof);
    auto* row = table.Resolve(arm);
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(harken::ArmState::Published, row->state.load());
    EXPECT_EQ(1u, row->frontierReaderPins.load());
    EXPECT_EQ(&binding, row->frontierAdmission);
    EXPECT_EQ(&lease, row->frontierAdmissionLease);
    EXPECT_TRUE(pin.Valid());
    ASSERT_FALSE(row->reservation.test_and_set());
    EXPECT_FALSE(table.RequestCancel(arm));
    EXPECT_FALSE(table.Tombstone(arm, 1));
    EXPECT_EQ(harken::AudioSliceState::Ready, lease.Capture().state);
    row->reservation.clear();
    EXPECT_TRUE(table.RequestCancel(arm));
    EXPECT_NE(harken::AudioSliceState::Ready, lease.Capture().state);
}

TEST(TimelineArmTable, ZeroPinMutationsRespectHeldRowReservation) {
    harken::TimelineArmTable table(2);
    harken::TimelineArmToken prepared{};
    harken::TimelineArmToken ordinary{};
    ASSERT_TRUE(table.Prepare(0, 1, 256, 256, 7, {}, &prepared));
    ASSERT_TRUE(table.Reserve(1, 1, 256, 256, 7, {}, &ordinary));
    auto* first = table.Resolve(prepared);
    auto* second = table.Resolve(ordinary);
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_FALSE(first->reservation.test_and_set());
    EXPECT_FALSE(table.CommitPrepared(prepared));
    EXPECT_FALSE(table.CancelPrepared(prepared));
    first->reservation.clear();
    ASSERT_TRUE(table.CommitPrepared(prepared));
    ASSERT_TRUE(table.AcknowledgeApplied(prepared));

    ASSERT_FALSE(second->reservation.test_and_set());
    EXPECT_FALSE(table.AcknowledgeApplied(ordinary));
    EXPECT_FALSE(table.RequestCancel(ordinary));
    second->reservation.clear();
    ASSERT_TRUE(table.AcknowledgeApplied(ordinary));
    ASSERT_FALSE(second->reservation.test_and_set());
    EXPECT_FALSE(table.Publish(ordinary));
    EXPECT_EQ(harken::ArmPublishStatus::DeferredReservation,
              table.PublishExact(ordinary));
    EXPECT_FALSE(table.RequestCancel(ordinary));
    second->reservation.clear();
    ASSERT_EQ(harken::ArmPublishStatus::Published,
              table.PublishExact(ordinary));
    ASSERT_TRUE(table.RequestCancel(ordinary));
}

harken::FrontierDueOperationRef ArmDueOperation(harken::TimelineArmToken token,
    const harken::TimelineProgramView& program, std::uint64_t frame) {
    harken::FrontierDueOperationRef operation{};
    operation.kind = harken::FrontierOperationKind::Dependent;
    operation.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    operation.order.executeFrame = frame;
    operation.order.logicCommitSequence = 1;
    operation.transport = 1;
    operation.world = 1;
    operation.transactionId = 1;
    operation.transactionSlot = 0;
    operation.program = &program;
    operation.programGeneration = program.generation;
    operation.armSlot = token.slot;
    operation.armGeneration = token.generation;
    operation.renderSlot = token.renderSlot;
    operation.renderGeneration = token.renderGeneration;
    return operation;
}

TEST(TimelineArmTable, BoundMembersApplyThroughOneLeaseWithoutPerArmClaims) {
    harken::TimelineArmTable table(2);
    harken::AudioSliceLease lease;
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry = {{1, 1}, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
    binding.primary = {2, 1, {1, 1}, 0, 0, 1, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.group = binding.primary;
    binding.operation.order = {256, 1};
    binding.operation.world = 1;
    binding.operation.transport = 1;
    std::array<harken::TimelineArmToken, 2> tokens{};
    std::array<harken::TimelineArmReaderPin, 2> pins{};
    std::array<harken::TimelineArmTable::BoundAudioProof, 2> proofs{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(table.Reserve(i, 1, 256, 256, 1, {}, &tokens[i]));
        ASSERT_TRUE(table.PinFrontierReader(tokens[i], &pins[i]));
        ASSERT_TRUE(table.BindFrontierAdmission(tokens[i], binding, lease));
        ASSERT_TRUE(table.AcknowledgeApplied(tokens[i]));
        ASSERT_TRUE(table.Publish(tokens[i]));
        EXPECT_FALSE(table.ClaimAudioSlice(tokens[i]));
        EXPECT_FALSE(table.Start(tokens[i], 3));
        ASSERT_TRUE(table.ProveBoundFrontierArm(tokens[i], binding.entry, 3, true, &proofs[i]));
    }
    struct Context {
        harken::TimelineArmTable* table;
        decltype(tokens)* tokens;
        decltype(proofs)* proofs;
    } context{&table, &tokens, &proofs};
    harken::AudioSliceOperation operation{};
    operation.context = &context;
    operation.validate = [](void*) noexcept { return true; };
    operation.afterClaim = [](void* opaque) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        EXPECT_FALSE(context.table->RequestCancel((*context.tokens)[1]));
        EXPECT_FALSE(context.table->RequestCancelAppliedAudioSlice((*context.tokens)[0]));
    };
    operation.apply = [](void* opaque) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        for (auto& proof : *context.proofs) context.table->ApplyBoundFrontierArm(proof);
    };
    EXPECT_EQ(harken::AudioSliceApplyStatus::AppliedWithDeferredInvalidation,
        harken::ApplyAudioSliceWhole(lease, &operation, 1));
    for (std::uint32_t i = 0; i < 2; ++i) {
        proofs[i].Release();
        harken::TimelineArmObservation observation{};
        ASSERT_TRUE(table.Observe(tokens[i], &observation));
        EXPECT_EQ(harken::ArmState::Started, observation.state);
        EXPECT_EQ(3u, observation.startedBlockSequence);
        EXPECT_FALSE(pins[i].Release(3));
        ASSERT_TRUE(pins[i].Release(4));
    }
}

TEST(TimelineArmTable, CancellationDuringBoundValidationInvalidatesWholeEntryBeforeClaim) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    harken::TimelineArmReaderPin pin{};
    harken::AudioSliceLease lease;
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry = {{1, 1}, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
    binding.primary = {2, 1, {1, 1}, 0, 0, 1, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.group = binding.primary;
    binding.operation.order = {256, 1};
    binding.operation.world = 1;
    binding.operation.transport = 1;
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    ASSERT_TRUE(table.PinFrontierReader(token, &pin));
    ASSERT_TRUE(table.BindFrontierAdmission(token, binding, lease));
    ASSERT_TRUE(table.AcknowledgeApplied(token));
    ASSERT_TRUE(table.Publish(token));
    harken::TimelineArmTable::BoundAudioProof proof;
    ASSERT_TRUE(table.ProveBoundFrontierArm(token, binding.entry, 3, true, &proof));
    EXPECT_FALSE(table.RequestCancel(token));
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, lease.Capture().state);
    proof.Release();
    ASSERT_TRUE(table.RequestCancel(token));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(harken::ArmState::CancelRequested, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_TRUE(pin.Release(0));
}

TEST(TimelineArmTable, TombstonedFutureArmReleasesPinAfterAppliedEntryFence) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    harken::TimelineArmReaderPin pin{};
    harken::AudioSliceLease lease;
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry = {{1, 1}, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
    binding.primary = {2, 1, {1, 1}, 0, 0, 1, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.group = binding.primary;
    binding.operation.order = {256, 1};
    binding.operation.world = 1;
    binding.operation.transport = 1;
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    ASSERT_TRUE(table.PinFrontierReader(token, &pin));
    ASSERT_TRUE(table.BindFrontierAdmission(token, binding, lease));
    ASSERT_TRUE(table.AcknowledgeApplied(token));
    ASSERT_TRUE(table.Publish(token));
    auto* row = table.Resolve(token);
    ASSERT_NE(nullptr, row);

    ASSERT_TRUE(lease.TryClaim());
    EXPECT_FALSE(table.RequestCancel(token));
    EXPECT_FALSE(pin.Release(3));
    ASSERT_TRUE(lease.MarkApplied());
    ASSERT_TRUE(table.RequestCancel(token));
    ASSERT_TRUE(table.Tombstone(token, 3));
    row->frontierLastReadSequence.store(3, std::memory_order_release);
    EXPECT_FALSE(pin.Release(3));
    EXPECT_TRUE(pin.Release(4));
}

TEST(TimelineArmTable, StrictFrontierClaimExcludesCancellationAndPreservesControlStartFact) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    harken::TimelineProgramView program{};
    program.generation = 1;
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    harken::TimelineArmReaderPin pin{};
    ASSERT_TRUE(table.PinFrontierReader(token, &pin));
    ASSERT_TRUE(table.AcknowledgeApplied(token, 2));
    ASSERT_TRUE(table.Publish(token));
    auto operation = ArmDueOperation(token, program, 256);
    harken::TimelineArmTable::AudioOperationClaim claim{};
    ASSERT_EQ(harken::TimelineArmOperationDisposition::Claimed,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Start, 5, &claim));
    EXPECT_FALSE(table.RequestCancel(token));
    EXPECT_FALSE(table.RequestCancelAppliedAudioSlice(token));
    EXPECT_FALSE(table.RequestCancelStartedVoice(token));
    EXPECT_FALSE(table.Tombstone(token, 5));
    EXPECT_FALSE(table.Start(token, 8));
    EXPECT_FALSE(table.AcknowledgeLogicalComplete(token, 8));
    EXPECT_FALSE(pin.Release(100));
    table.CompleteClaimedAudioSlice(token, 8);
    table.ApplyFrontierOperation(claim);
    table.FinishFrontierOperation(claim);
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(harken::ArmState::Started, observation.state);
    EXPECT_EQ(5u, observation.startedBlockSequence);
    EXPECT_EQ(0u, observation.logicalCompletionSequence);
    EXPECT_EQ(0u, observation.tombstoneAudioSequence);
    EXPECT_FALSE(table.Start(token, 99));
    operation.order.executeFrame = 272;
    ASSERT_EQ(harken::TimelineArmOperationDisposition::Claimed,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Control, 5, &claim));
    EXPECT_FALSE(table.RequestCancelAppliedAudioSlice(token));
    table.ApplyFrontierOperation(claim);
    table.FinishFrontierOperation(claim);
    EXPECT_EQ(harken::TimelineArmOperationDisposition::Ineligible,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Control, 5, &claim));
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(5u, observation.startedBlockSequence);
    ASSERT_TRUE(table.RequestCancelStartedVoice(token));
    ASSERT_TRUE(table.Tombstone(token, 5));
    EXPECT_FALSE(table.Reclaim(token, 100));
    EXPECT_FALSE(pin.Release(5));
    ASSERT_TRUE(pin.Release(6));
    EXPECT_TRUE(table.Reclaim(token, 6));
}

TEST(TimelineArmTable, TransientFrontierStartKeepsOnsetAndReaderFenceUntilReclaim) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    harken::TimelineProgramView program{};
    program.generation = 1;
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    harken::TimelineArmReaderPin pin{};
    ASSERT_TRUE(table.PinFrontierReader(token, &pin));
    ASSERT_TRUE(table.AcknowledgeApplied(token, 2));
    ASSERT_TRUE(table.Publish(token));
    const auto operation = ArmDueOperation(token, program, 256);
    harken::TimelineArmTable::AudioOperationClaim claim{};
    ASSERT_EQ(harken::TimelineArmOperationDisposition::Claimed,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Start, 5, &claim));
    EXPECT_FALSE(table.RequestCancel(token));
    table.ApplyFrontierOperation(claim);
    table.FinishFrontierOperation(claim, true);
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(harken::ArmState::Tombstoned, observation.state);
    EXPECT_EQ(5u, observation.startedBlockSequence);
    EXPECT_EQ(5u, observation.logicalCompletionSequence);
    EXPECT_EQ(5u, observation.tombstoneAudioSequence);
    EXPECT_FALSE(table.Reclaim(token, 100));
    EXPECT_FALSE(pin.Release(5));
    ASSERT_TRUE(pin.Release(6));
    ASSERT_TRUE(table.Reclaim(token, 6));
    harken::TimelineArmToken next{};
    ASSERT_TRUE(table.Reserve(0, 2, 256, 256, 1, {}, &next));
    EXPECT_EQ(harken::TimelineArmOperationDisposition::Stale,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Start, 7, &claim));
}

TEST(TimelineArmTable, StrictFrontierStartNeverResurrectsCancelledOrReusedArm) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    harken::TimelineProgramView program{};
    program.generation = 1;
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    harken::TimelineArmReaderPin pin{};
    ASSERT_TRUE(table.PinFrontierReader(token, &pin));
    ASSERT_TRUE(table.AcknowledgeApplied(token));
    ASSERT_TRUE(table.Publish(token));
    ASSERT_TRUE(table.RequestCancel(token));
    const auto operation = ArmDueOperation(token, program, 256);
    harken::TimelineArmTable::AudioOperationClaim claim{};
    EXPECT_EQ(harken::TimelineArmOperationDisposition::Cancelled,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Start, 5, &claim));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(harken::ArmState::CancelRequested, observation.state);
    ASSERT_TRUE(table.Tombstone(token, 5));
    ASSERT_TRUE(pin.Release(0));
    ASSERT_TRUE(table.Reclaim(token, 6));
    harken::TimelineArmToken next{};
    ASSERT_TRUE(table.Reserve(0, 2, 256, 256, 1, {}, &next));
    EXPECT_EQ(harken::TimelineArmOperationDisposition::Stale,
        table.BeginFrontierOperation(token, operation, harken::TimelineArmOperationKind::Start, 7, &claim));
    EXPECT_TRUE(table.CancelPrepared(next) == false);
}

TEST(TimelineArmTable, FrontierObservationPinPreventsPreparedRollbackAndRequiresPrepublicationBinding) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    ASSERT_TRUE(table.Prepare(0, 1, 256, 256, 1, {}, &token));
    harken::TimelineArmReaderPin pin{};
    ASSERT_TRUE(table.PinFrontierReader(token, &pin));
    EXPECT_FALSE(table.CancelPrepared(token));
    ASSERT_TRUE(pin.Release(0));
    ASSERT_TRUE(table.CancelPrepared(token));
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    ASSERT_TRUE(table.AcknowledgeApplied(token));
    ASSERT_TRUE(table.Publish(token));
    EXPECT_FALSE(table.PinFrontierReader(token, &pin));
}

TEST(TimelineArmTable, ReclaimWaitsForAudioSequenceAfterTombstone) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    ASSERT_TRUE(table.AcknowledgeApplied(token));
    ASSERT_TRUE(table.Publish(token));
    ASSERT_TRUE(table.Start(token));
    ASSERT_TRUE(table.Tombstone(token, 7));
    harken::ArmState state{};
    ASSERT_TRUE(table.State(token, &state));
    EXPECT_EQ(state, harken::ArmState::Tombstoned);
    EXPECT_FALSE(table.Reclaim(token, 7));
    EXPECT_EQ(table.ReclaimTombstoned(7), 0u);
    EXPECT_EQ(table.ReclaimTombstoned(8), 1u);
    EXPECT_FALSE(table.State(token, &state));
}

TEST(TimelineArmTable, ObservationPublishesStartCompletionAndTombstoneFences) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));

    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(observation.state, harken::ArmState::PendingApply);
    EXPECT_EQ(observation.armedCompletionSequence, 0u);
    EXPECT_EQ(observation.startedBlockSequence, 0u);
    EXPECT_EQ(observation.logicalCompletionSequence, 0u);
    EXPECT_EQ(observation.tombstoneAudioSequence, 0u);

    ASSERT_TRUE(table.AcknowledgeApplied(token, 3));
    ASSERT_TRUE(table.Publish(token));
    ASSERT_TRUE(table.Start(token, 5));
    ASSERT_TRUE(table.AcknowledgeLogicalComplete(token, 7));
    ASSERT_TRUE(table.Tombstone(token, 8));
    ASSERT_TRUE(table.Observe(token, &observation));
    EXPECT_EQ(observation.state, harken::ArmState::Tombstoned);
    EXPECT_EQ(observation.armedCompletionSequence, 3u);
    EXPECT_EQ(observation.startedBlockSequence, 5u);
    EXPECT_EQ(observation.logicalCompletionSequence, 7u);
    EXPECT_EQ(observation.tombstoneAudioSequence, 8u);

    EXPECT_FALSE(table.Reclaim(token, 8));
    EXPECT_TRUE(table.Reclaim(token, 9));
    EXPECT_FALSE(table.Observe(token, &observation));
}

TEST(TimelineArmTable, ResolveIdentitySerializesWithReclaim) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &token));
    ASSERT_TRUE(table.AcknowledgeApplied(token));
    ASSERT_TRUE(table.Publish(token));
    ASSERT_TRUE(table.Start(token));
    ASSERT_TRUE(table.Tombstone(token, 0));
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;

    struct Barrier {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
    } barrier;
    table.SetResolveIdentityHookForTests(
        [](void* context) {
            auto* barrier = static_cast<Barrier*>(context);
            barrier->entered.store(true, std::memory_order_release);
            while (!barrier->release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        },
        &barrier);

    harken::TimelineArmToken resolved{};
    bool identityResolved = false;
    std::thread resolver(
        [&] { identityResolved = table.ResolveIdentity(packed, &resolved); });
    while (!barrier.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(table.Reclaim(token, 1));
    barrier.release.store(true, std::memory_order_release);
    resolver.join();
    table.SetResolveIdentityHookForTests(nullptr, nullptr);

    EXPECT_TRUE(identityResolved);
    EXPECT_EQ(resolved.slot, token.slot);
    EXPECT_EQ(resolved.generation, token.generation);
    EXPECT_EQ(resolved.renderSlot, token.renderSlot);
    EXPECT_EQ(resolved.renderGeneration, token.renderGeneration);
    EXPECT_TRUE(table.Reclaim(token, 1));
    harken::TimelineArmToken stale{};
    EXPECT_FALSE(table.ResolveIdentity(packed, &stale));
    EXPECT_FALSE(table.AcknowledgeApplied(packed, 1));
}

TEST(TimelineArmTable, PackedLateAckCannotAcknowledgeReusedRow) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken first{};
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &first));
    const std::uint64_t packedFirst =
        (static_cast<std::uint64_t>(first.generation) << 32) | first.slot;
    ASSERT_TRUE(table.AcknowledgeApplied(first));
    ASSERT_TRUE(table.Publish(first));
    ASSERT_TRUE(table.Start(first));
    ASSERT_TRUE(table.Tombstone(first, 0));
    ASSERT_TRUE(table.Reclaim(first, 1));

    harken::TimelineArmToken second{};
    ASSERT_TRUE(table.Reserve(0, 2, 512, 512, 1, {}, &second));
    EXPECT_FALSE(table.AcknowledgeApplied(packedFirst, 2));
    harken::TimelineArmToken stale{};
    EXPECT_FALSE(table.ResolveIdentity(packedFirst, &stale));
    EXPECT_TRUE(table.AcknowledgeApplied(second));
}

TEST(TimelineArmTable, OldTokenCannotWinWhileReclaimAndReuseProgress) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken first{};
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &first));
    ASSERT_TRUE(table.AcknowledgeApplied(first));
    ASSERT_TRUE(table.Publish(first));
    ASSERT_TRUE(table.Start(first));
    ASSERT_TRUE(table.Tombstone(first, 3));
    EXPECT_FALSE(table.Reclaim(first, 3));
    EXPECT_FALSE(table.AcknowledgeApplied(first));
    EXPECT_FALSE(table.RequestCancel(first));
    ASSERT_TRUE(table.Reclaim(first, 4));

    harken::TimelineArmToken second{};
    ASSERT_TRUE(table.Reserve(0, 2, 512, 512, 2, {}, &second));
    EXPECT_FALSE(table.AcknowledgeApplied(first));
    EXPECT_FALSE(table.Publish(first));
    EXPECT_FALSE(table.Start(first));
    EXPECT_FALSE(table.RequestCancel(first));
    EXPECT_FALSE(table.Reclaim(first, 5));
    EXPECT_TRUE(table.AcknowledgeApplied(second));
}

TEST(TimelineArmTable, OldGenerationCannotAcknowledgeReusedRow) {
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken first{};
    ASSERT_TRUE(table.Reserve(0, 1, 256, 256, 1, {}, &first));
    ASSERT_TRUE(table.AcknowledgeApplied(first));
    ASSERT_TRUE(table.Publish(first));
    ASSERT_TRUE(table.Start(first));
    ASSERT_TRUE(table.Tombstone(first, 0));
    ASSERT_TRUE(table.Reclaim(first, 1));

    harken::TimelineArmToken second{};
    ASSERT_TRUE(table.Reserve(0, 2, 512, 512, 1, {}, &second));
    EXPECT_FALSE(table.AcknowledgeApplied(first));
    EXPECT_TRUE(table.AcknowledgeApplied(second));
}

TEST(TimelineExecutor, DeferredBudgetDoesNotResetWithinSameQuantum) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);

    const harken::PlayableInstanceHandle owner =
        engine->playableInstances.Acquire(harken::PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch =
        engine->playableInstances.Classify(owner).scheduleEpoch;
    const std::uint64_t dueFrame = engine->timelinePool.WindowEndFrame();
    std::vector<harken::EventTemplate> events(33);
    std::vector<harken::ProgramView> programs(33);
    harken::PostInput input{};
    input.transportEpoch = engine->timelinePool.Epoch();
    for (std::uint32_t index = 0; index < programs.size(); ++index) {
        events[index].logicCommitSequence = index + 1u;
        events[index].payload = index + 1u;
        programs[index].ownerInstance = owner;
        programs[index].ownerInstanceScheduleEpoch = ownerEpoch;
        programs[index].transactionId = index + 1u;
        programs[index].entryAnchorFrame = dueFrame;
        programs[index].events = &events[index];
        programs[index].eventCount = 1;
        input.logicCommitSequence = index + 1u;
        ASSERT_EQ(engine->timelinePlanner.PostPlay(programs[index], input).status,
                  harken::PostStatus::Deferred);
    }
    ASSERT_EQ(engine->timelineDeferred.Count(), 33u);

    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    ASSERT_EQ(engine->timelineDeferred.Count(), 1u);
    EXPECT_EQ(engine->timelineService->MaterializationHighWater(), 32u);
    EXPECT_EQ(engine->timelineService->MaterializationsPacedByBudget(), 1u);

    engine->timelineService->Service();
    EXPECT_EQ(engine->timelineDeferred.Count(), 1u);
    EXPECT_EQ(engine->timelineService->MaterializationsPacedByBudget(), 1u);

    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    EXPECT_EQ(engine->timelineDeferred.Count(), 0u);
}

TEST(TimelineExecutor, TransientRefusalStopsItsDueClass) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);

    const harken::PlayableInstanceHandle owner =
        engine->playableInstances.Acquire(harken::PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch =
        engine->playableInstances.Classify(owner).scheduleEpoch;
    std::vector<harken::EventTemplate> events(258);
    std::vector<harken::ProgramView> programs(258);
    harken::PostInput input{};
    input.transportEpoch = engine->timelinePool.Epoch();
    for (std::uint32_t index = 0; index < 128; ++index) {
        events[index].logicCommitSequence = index + 1u;
        programs[index].ownerInstance = owner;
        programs[index].ownerInstanceScheduleEpoch = ownerEpoch;
        programs[index].transactionId = index + 1u;
        programs[index].entryAnchorFrame = 8192u;
        programs[index].events = &events[index];
        programs[index].eventCount = 1;
        input.logicCommitSequence = index + 1u;
        ASSERT_EQ(engine->timelinePlanner.PostPlay(programs[index], input).status,
                  harken::PostStatus::Reserved)
            << index;
    }
    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    for (std::uint32_t index = 128; index < 256; ++index) {
        events[index].logicCommitSequence = index + 1u;
        programs[index].ownerInstance = owner;
        programs[index].ownerInstanceScheduleEpoch = ownerEpoch;
        programs[index].transactionId = index + 1u;
        programs[index].entryAnchorFrame = 8448u;
        programs[index].events = &events[index];
        programs[index].eventCount = 1;
        input.logicCommitSequence = index + 1u;
        ASSERT_EQ(engine->timelinePlanner.PostPlay(programs[index], input).status,
                  harken::PostStatus::Reserved)
            << index;
    }
    ASSERT_EQ(engine->timelinePool.LiveTransactionCount(), 256u);

    const std::uint64_t dueFrame = engine->timelinePool.WindowEndFrame();
    for (std::uint32_t index = 256; index < programs.size(); ++index) {
        events[index].logicCommitSequence = index + 1u;
        programs[index].ownerInstance = owner;
        programs[index].ownerInstanceScheduleEpoch = ownerEpoch;
        programs[index].transactionId = index + 1u;
        programs[index].entryAnchorFrame = dueFrame;
        programs[index].events = &events[index];
        programs[index].eventCount = 1;
        input.logicCommitSequence = index + 1u;
        ASSERT_EQ(engine->timelinePlanner.PostPlay(programs[index], input).status,
                  harken::PostStatus::Deferred);
    }
    const std::uint64_t refusalsBefore =
        engine->timelinePool.RefusalCount(harken::ReserveRefusal::PoolFull);

    engine->nullBackend.Pump(1);
    engine->timelineService->Service();

    EXPECT_EQ(engine->timelinePool.RefusalCount(
                  harken::ReserveRefusal::PoolFull) -
                  refusalsBefore,
              1u);
    EXPECT_EQ(engine->timelineDeferred.Count(), 2u);
}

TEST(TimelineExecutor, SupersededRegistryWaitsForArmLeaseAndFence) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    auto installProgram = [&](std::uint64_t fingerprint,
                              std::int64_t startFrame,
                              std::uint32_t transaction) {
        static std::array<harken::TimelineEventPayload, 4> payloadStorage{};
        static std::array<harken::EventTemplate, 4> eventStorage{};
        const std::size_t slot = transaction - 1u;
        harken::TimelineSemanticTarget target{};
        target.programSemanticFingerprint = fingerprint;
        payloadStorage[slot * 2].kind = harken::TimelineEventKind::Start;
        payloadStorage[slot * 2].target = target;
        payloadStorage[slot * 2 + 1].kind = harken::TimelineEventKind::Stop;
        payloadStorage[slot * 2 + 1].target = target;
        eventStorage[slot * 2] = {startFrame, transaction, 0, 0, 20,
                                  slot * 2};
        eventStorage[slot * 2 + 1] = {startFrame + 512, transaction, 1, 0, 50,
                                      slot * 2 + 1};
        harken::ProgramView schedule{};
        schedule.transactionId = transaction;
        schedule.events = eventStorage.data() + slot * 2;
        schedule.eventCount = 2;
        schedule.rollingGroups = true;
        harken::TimelineProgramView program{
            1, payloadStorage.data(),
            static_cast<std::uint32_t>(payloadStorage.size())};
        harken::TimelineOccurrenceInstallView occurrence{};
        occurrence.schedule = &schedule;
        occurrence.voice.soundId = harkentest::ObjectId(1);
        occurrence.voice.outputBusId = VN_BUS_MASTER;
        occurrence.voice.target = target;
        const harken::TimelineProgramInstallView install{&program, &occurrence,
                                                        1};
        return harken::InstallTimelineProgram(install);
    };

    const harken::TimelineRuntimeInstallResult first =
        installProgram(103, 512, 1);
    ASSERT_EQ(first.status, harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& firstOccurrence =
        engine->timelineProgramSets[first.generation & 1u]->occurrences[0];
    ASSERT_TRUE(firstOccurrence.arm.Valid());
    const harken::TimelineRuntimeInstallResult second =
        installProgram(104, 1024, 2);
    ASSERT_EQ(second.status, harken::TimelineRuntimeInstallStatus::Installed);

    EXPECT_TRUE(engine->timelinePrograms.IsReadable(first.generation));
    EXPECT_NE(engine->timelineProgramSets[first.generation & 1u], nullptr);
    EXPECT_TRUE(engine->timelineArms.HasLiveGeneration(first.generation));
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_TRUE(engine->timelinePrograms.IsReadable(first.generation));
    EXPECT_NE(engine->timelineProgramSets[first.generation & 1u], nullptr);

    engine->mixer.DrainCommandsForTest();
    engine->timelineExecutor.ExecuteQuantum(0);
    engine->timelineArms.ReclaimTombstoned(1);
    ASSERT_EQ(engine->mixer.BlocksRendered(), 0u);
    engine->timelineService->Service();

    const std::unique_ptr<harken::EngineTimelineProgramSet>& retainedFirst =
        engine->timelineProgramSets[first.generation & 1u];
    ASSERT_NE(retainedFirst, nullptr);
    const bool allTerminal = std::all_of(
        retainedFirst->occurrences.begin(), retainedFirst->occurrences.end(),
        [](const harken::EngineTimelineOccurrence& occurrence) {
            return occurrence.phase ==
                   harken::EngineTimelineOccurrencePhase::Terminal;
        });
    EXPECT_TRUE(allTerminal);
    EXPECT_FALSE(engine->timelinePool.HasLiveProgramGeneration(first.generation));
    EXPECT_FALSE(
        engine->timelinePlanner.HasLiveProgramGeneration(first.generation));
    EXPECT_FALSE(engine->timelineArms.HasLiveGeneration(first.generation));
    EXPECT_EQ(engine->mixer.BlocksRendered(),
              retainedFirst->supersededAtAudioSequence);
    EXPECT_TRUE(engine->timelinePrograms.IsReadable(first.generation));

    engine->nullBackend.Pump(1);
    ASSERT_GT(engine->mixer.BlocksRendered(),
              retainedFirst->supersededAtAudioSequence);
    engine->timelineService->Service();
    EXPECT_EQ(engine->timelineProgramSets[first.generation & 1u], nullptr);
    EXPECT_FALSE(engine->timelinePrograms.IsReadable(first.generation));
}

TEST(TimelineExecutor, ProductionFrozenPageKeepsSupersededProgramAlive) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    auto installProgram = [&](std::uint64_t fingerprint,
                              std::uint32_t transaction) {
        static std::array<harken::TimelineEventPayload, 4> payloadStorage{};
        static std::array<harken::EventTemplate, 4> eventStorage{};
        const std::size_t slot = transaction - 1u;
        harken::TimelineSemanticTarget target{};
        target.programSemanticFingerprint = fingerprint;
        payloadStorage[slot * 2].kind = harken::TimelineEventKind::Start;
        payloadStorage[slot * 2].target = target;
        payloadStorage[slot * 2 + 1].kind = harken::TimelineEventKind::Stop;
        payloadStorage[slot * 2 + 1].target = target;
        eventStorage[slot * 2] = {256, transaction, 0, 0, 20,
                                  slot * 2};
        eventStorage[slot * 2 + 1] = {768, transaction, 1, 0, 50,
                                      slot * 2 + 1};
        harken::ProgramView schedule{};
        schedule.transactionId = transaction;
        schedule.events = eventStorage.data() + slot * 2;
        schedule.eventCount = 2;
        schedule.rollingGroups = true;
        harken::TimelineProgramView program{
            1, payloadStorage.data(),
            static_cast<std::uint32_t>(payloadStorage.size())};
        harken::TimelineOccurrenceInstallView occurrence{};
        occurrence.schedule = &schedule;
        occurrence.voice.soundId = harkentest::ObjectId(1);
        occurrence.voice.outputBusId = VN_BUS_MASTER;
        occurrence.voice.target = target;
        const harken::TimelineProgramInstallView install{&program, &occurrence,
                                                        1};
        return harken::InstallTimelineProgram(install);
    };

    const harken::TimelineRuntimeInstallResult first = installProgram(101, 1);
    ASSERT_EQ(first.status, harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    ASSERT_TRUE(engine->timelinePool.HasLiveProgramGeneration(first.generation));

    const harken::TimelineRuntimeInstallResult second = installProgram(102, 2);
    ASSERT_EQ(second.status, harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_NE(engine->timelineProgramSets[first.generation & 1u], nullptr);
    EXPECT_TRUE(engine->timelinePrograms.IsReadable(first.generation));
    EXPECT_TRUE(engine->timelinePool.HasLiveProgramGeneration(first.generation));

    for (std::uint32_t block = 0;
         block < 8 &&
         engine->timelineProgramSets[first.generation & 1u] != nullptr;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
    }
    EXPECT_EQ(engine->timelineProgramSets[first.generation & 1u], nullptr);
    EXPECT_FALSE(engine->timelinePrograms.IsReadable(first.generation));
}

TEST(TimelineExecutor, ReplaceAfterDeferredGenerationMaxCleansProductionState) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(32768, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 110;
    std::array<harken::TimelineEventPayload, 3> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Envelope;
    payloads[1].target = target;
    payloads[1].envelopeDirection =
        harken::TimelineEnvelopeDirection::FadeOut;
    payloads[1].envelopeStartFrame = 17408;
    payloads[1].envelopeEndFrame = 17920;
    payloads[2].kind = harken::TimelineEventKind::Stop;
    payloads[2].target = target;
    payloads[2].envelopeEndFrame = 17920;
    std::array<harken::EventTemplate, 3> events{{
        {256, 1, 0, 0, 20, 0},
        {17408, 1, 1, 0, 30, 1},
        {17920, 1, 2, 0, 50, 2},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult first =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(first.status, harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& firstOccurrence =
        engine->timelineProgramSets[first.generation & 1u]->occurrences[0];
    ASSERT_EQ(firstOccurrence.phase,
              harken::EngineTimelineOccurrencePhase::Started);
    ASSERT_GE(events[1].frameOffsetFromEntry,
              engine->timelinePool.WindowEndFrame());
    ASSERT_EQ(engine->timelineDeferred.Count(), 1u);
    ASSERT_TRUE(engine->timelinePlanner.SetRollingDeferredGenerationForTests(
        firstOccurrence.arm, UINT64_MAX));

    target.programSemanticFingerprint = 111;
    for (harken::TimelineEventPayload& payload : payloads) {
        payload.target = target;
    }
    occurrence.voice.target = target;
    const harken::TimelineRuntimeInstallResult replacement =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(replacement.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    constexpr std::uint32_t kMaxDrainBlocks = 96;
    std::uint32_t drainedBlocks = 0;
    for (; drainedBlocks < kMaxDrainBlocks &&
           (engine->timelineArms.LiveCount() != 0u ||
            engine->voices.ActiveCount() != 0u ||
            engine->timelineDeferred.Count() != 0u ||
            engine->timelinePlanner.LiveRollingCountForTests() != 0u ||
            engine->timelinePool.LiveTransactionCount() != 0u ||
            engine->timelinePrograms.IsReadable(first.generation));
         ++drainedBlocks) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        const std::uint64_t blocksBefore = engine->mixer.BlocksRendered();
        engine->nullBackend.Pump(1);
        ASSERT_EQ(engine->mixer.BlocksRendered(), blocksBefore + 1u);
    }
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_LT(drainedBlocks, kMaxDrainBlocks);

    const std::unique_ptr<harken::EngineTimelineProgramSet>& retainedFirst =
        engine->timelineProgramSets[first.generation & 1u];
    if (retainedFirst != nullptr) {
        ASSERT_EQ(retainedFirst->occurrences.size(), 1u);
        EXPECT_EQ(retainedFirst->occurrences[0].phase,
                  harken::EngineTimelineOccurrencePhase::Terminal);
    }
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
    std::vector<harken::SourceCursorMirror::Row> cursors(
        engine->sourceCursors.SlotCount());
    std::uint64_t cursorBlock = 0;
    ASSERT_TRUE(engine->sourceCursors.Capture(cursors.data(), &cursorBlock));
    EXPECT_TRUE(std::none_of(cursors.begin(), cursors.end(),
                             [](const harken::SourceCursorMirror::Row& row) {
                                 return row.active;
                             }));
    EXPECT_EQ(engine->timelineDeferred.Count(), 0u);
    EXPECT_EQ(engine->timelinePlanner.LiveRollingCountForTests(), 0u);
    EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
    EXPECT_EQ(engine->playableInstances.LiveCount(), 0u);
    EXPECT_EQ(engine->timelineOccurrences.OccupiedCount(), 0u);
    EXPECT_FALSE(engine->timelinePrograms.IsReadable(first.generation));
}

TEST(TimelineExecutor,
     TransportResetDrainsStartedLegacyRollingProgramAndDeferredTail) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint32_t voiceBaseline = engine->voices.ActiveCount();
    const std::uint32_t armBaseline = engine->timelineArms.LiveCount();
    const std::uint32_t occurrenceBaseline =
        engine->timelineOccurrences.OccupiedCount();
    const std::uint32_t instanceBaseline =
        engine->playableInstances.LiveCount();
    const std::vector<float> dc(32768, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 211;
    std::array<harken::TimelineEventPayload, 3> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Envelope;
    payloads[1].target = target;
    payloads[1].envelopeDirection =
        harken::TimelineEnvelopeDirection::FadeOut;
    payloads[1].envelopeStartFrame = 17408;
    payloads[1].envelopeEndFrame = 17920;
    payloads[2].kind = harken::TimelineEventKind::Stop;
    payloads[2].target = target;
    payloads[2].envelopeEndFrame = 17920;
    std::array<harken::EventTemplate, 3> events{{
        {256, 1, 0, 0, 20, 0},
        {17408, 1, 1, 0, 30, 1},
        {17920, 1, 2, 0, 50, 2},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{
        &program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    for (std::uint32_t block = 0; block < 4; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
    }
    ASSERT_NE(engine->timelineProgramSets[installed.generation & 1u],
              nullptr);
    ASSERT_EQ(engine->timelineProgramSets[installed.generation & 1u]
                  ->occurrences[0]
                  .phase,
              harken::EngineTimelineOccurrencePhase::Started);
    ASSERT_EQ(engine->timelineDeferred.Count(), 1u);

    const std::uint64_t resetFrame =
        engine->mixer.BlocksRendered() * engine->config.blockFrames;
    harken::RuntimeTransportResetStatus reset =
        harken::BeginRuntimeTransportReset(*engine, resetFrame);
    ASSERT_EQ(reset, harken::RuntimeTransportResetStatus::Backpressure);
    for (std::uint32_t block = 0;
         block < 128 &&
         reset == harken::RuntimeTransportResetStatus::Backpressure;
         ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
        reset = harken::BeginRuntimeTransportReset(*engine, resetFrame);
    }
    EXPECT_EQ(reset, harken::RuntimeTransportResetStatus::Complete);
    EXPECT_EQ(engine->voices.ActiveCount(), voiceBaseline);
    EXPECT_EQ(engine->timelineArms.LiveCount(), armBaseline);
    EXPECT_EQ(engine->timelineOccurrences.OccupiedCount(),
              occurrenceBaseline);
    EXPECT_EQ(engine->playableInstances.LiveCount(), instanceBaseline);
    EXPECT_EQ(engine->timelineDeferred.Count(), 0u);
    EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
    EXPECT_EQ(engine->timelineProgramSets[0], nullptr);
    EXPECT_EQ(engine->timelineProgramSets[1], nullptr);
    EXPECT_FALSE(engine->timelinePrograms.IsReadable(
        installed.generation));
    EXPECT_FALSE(engine->runtimeRecreateRequired);
}

TEST(TimelineExecutor, FrozenPageKeepsProgramGenerationLiveUntilReclaim) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::ScheduledEventInput event{};
    event.executeFrame = 1;
    harken::TransactionRequest request{};
    request.transactionId = 1;
    request.transportEpoch = 1;
    request.program = {7, nullptr};
    request.events = &event;
    request.eventCount = 1;
    ASSERT_EQ(pool.Reserve(request, harken::PrePublishLatePolicy::Refuse).status,
              harken::ReserveStatus::Committed);
    pool.Publish(256);
    EXPECT_TRUE(pool.HasLiveProgramGeneration(7));
    pool.ConsumerFrontier().store(1);
    EXPECT_EQ(pool.Advance(), 1u);
    EXPECT_FALSE(pool.HasLiveProgramGeneration(7));
}

TEST(TimelineExecutor, MissingFrontierPageDoesNotConsumeNewerReadyPage) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::TimelineArmTable arms(1);
    Sink sink;
    sink.admit = false;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));

    executor.ExecuteQuantum(1);
    EXPECT_EQ(executor.Stats().deadlineMissQuanta, 1u);
    EXPECT_EQ(pool.ConsumerFrontier().load(), 0u);
    EXPECT_FALSE(sink.applied);
    EXPECT_EQ(sink.admitCount, 0u);
    EXPECT_EQ(sink.beginCount, 1u);
    EXPECT_EQ(sink.endCount, 1u);
}

TEST(TimelineExecutor, FixedPrefixDoesNotExpandPastItsAdmittedPageCount) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    pool.Publish(512);
    harken::TimelineArmTable arms(1);
    Sink sink;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));

    const harken::TimelineExecutionPlan plan{
        0, 1, harken::TimelinePagePrefixStopReason::ScratchCapacity};
    const harken::TimelineExecutionResult result =
        executor.ExecuteQuantum(1, plan);
    EXPECT_EQ(result.consumedPageCount, 1u);
    EXPECT_EQ(result.frontier, 1u);
    EXPECT_EQ(pool.ConsumerFrontier().load(), 1u);
    EXPECT_EQ(sink.admitCount, 1u);
    EXPECT_EQ(sink.beginCount, 1u);
    EXPECT_EQ(sink.endCount, 1u);
}

TEST(TimelineExecutor, AdmitRefusalConsumesNoPartOfAFixedPrefix) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    pool.Publish(256);
    harken::TimelineArmTable arms(1);
    Sink sink;
    sink.admit = false;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));

    const harken::TimelineExecutionResult result = executor.ExecuteQuantum(
        0, {0, 1, harken::TimelinePagePrefixStopReason::ReachedCurrentQuantum});
    EXPECT_EQ(result.consumedPageCount, 0u);
    EXPECT_EQ(result.frontier, 0u);
    EXPECT_EQ(pool.ConsumerFrontier().load(), 0u);
    EXPECT_EQ(sink.admitCount, 1u);
    EXPECT_EQ(sink.beginCount, 0u);
    EXPECT_EQ(sink.endCount, 0u);
    EXPECT_EQ(executor.Stats().renderUnavailableDeferrals, 1u);
}

TEST(TimelineExecutor, BeginRefusalConsumesNoPartOfAFixedPrefix) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    pool.Publish(256);
    harken::TimelineArmTable arms(1);
    Sink sink;
    sink.begin = false;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));

    const harken::TimelineExecutionResult result = executor.ExecuteQuantum(
        0, {0, 1, harken::TimelinePagePrefixStopReason::ReachedCurrentQuantum});
    EXPECT_EQ(result.consumedPageCount, 0u);
    EXPECT_EQ(result.frontier, 0u);
    EXPECT_EQ(pool.ConsumerFrontier().load(), 0u);
    EXPECT_EQ(sink.admitCount, 1u);
    EXPECT_EQ(sink.beginCount, 1u);
    EXPECT_EQ(sink.endCount, 0u);
}

TEST(TimelineExecutor, ZeroPagePlanKeepsEmptyQuantumCallbacksWithoutAdmission) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::TimelineArmTable arms(1);
    Sink sink;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));

    const harken::TimelineExecutionResult result = executor.ExecuteQuantum(
        0, {0, 0, harken::TimelinePagePrefixStopReason::Unpublished});
    EXPECT_EQ(result.consumedPageCount, 0u);
    EXPECT_EQ(result.frontier, 0u);
    EXPECT_EQ(pool.ConsumerFrontier().load(), 0u);
    EXPECT_EQ(sink.admitCount, 0u);
    EXPECT_EQ(sink.beginCount, 1u);
    EXPECT_EQ(sink.endCount, 1u);
    EXPECT_EQ(executor.Stats().deadlineMissQuanta, 1u);
}

TEST(TimelineExecutor, AppliesImpulseOffsetsAtQuantumEdges) {
    for (const std::uint64_t frame : {0ull, 1ull, 255ull}) {
        harken::ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
        harken::TimelineArmTable arms(1);
        FiledTimeline filed;
        FileTimeline(filed, pool, arms, 1, frame);
        Sink sink;
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
        executor.ExecuteQuantum(0);
        ASSERT_TRUE(sink.applied);
        EXPECT_EQ(sink.offsets[0], frame);
        EXPECT_EQ(executor.Stats().protocolViolation, 0u);
        (void)filed;
    }
}

TEST(TimelineExecutor, DuplicateStartWritesNoPrefix) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::TimelineArmTable arms(1);
    FiledTimeline filed;
    FileTimeline(filed, pool, arms, 2);
    Sink sink;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
    executor.ExecuteQuantum(0);
    EXPECT_FALSE(sink.applied);
    EXPECT_EQ(sink.count, 0u);
    EXPECT_EQ(executor.Stats().protocolViolation, 1u);
    (void)filed;
}

TEST(TimelineExecutor, FrozenOrderingSurvivesDecodeAndLegacyLateDispatch) {
    for (const std::uint64_t quantum : {0ull, 1ull}) {
        harken::ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
        harken::TimelineArmTable arms(1);
        FiledTimeline filed;
        FileTimeline(filed, pool, arms, 2, 8,
            harken::TimelineEventKind::Envelope, 30);
        std::array<harken::ConsumedEvent, 2> consumed{};
        ASSERT_EQ(2u, pool.ConsumePageRt(0, consumed.data(), consumed.size()));
        Sink sink;
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
        executor.ExecuteQuantum(quantum);
        ASSERT_TRUE(sink.applied);
        ASSERT_EQ(2u, sink.count);
        for (std::uint32_t i = 0; i < sink.count; ++i) {
            const auto& event = consumed[i];
            const harken::TimelineExecutionOrder expected{
                event.executeFrame, event.logicCommitSequence,
                event.executionOrdinal, event.eventIndex,
                event.transactionOrdinal, event.subeventPhase, event.rowSequence};
            EXPECT_EQ(expected, sink.orders[i]);
            EXPECT_EQ(8u, sink.orders[i].executeFrame);
            EXPECT_EQ(i + 1u, sink.orders[i].rowSequence);
            EXPECT_EQ(quantum == 0 ? 8u : 0u, sink.offsets[i]);
        }
        EXPECT_LT(sink.orders[0], sink.orders[1]);
    }
}

TEST(TimelineExecutor, CancelledTransactionDoesNotValidateObsoletePayload) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::TimelineArmTable arms(1);
    FiledTimeline filed;
    FileTimeline(filed, pool, arms, 1);
    ASSERT_TRUE(arms.RequestCancel(filed.arm));
    filed.payloads[0].target = {};
    Sink sink;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
    executor.ExecuteQuantum(0);
    EXPECT_FALSE(sink.applied);
    EXPECT_EQ(0u, sink.count);
    EXPECT_EQ(0u, executor.Stats().protocolViolation);
}

TEST(TimelineExecutor, EightEventTransactionFitsExactly) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::TimelineArmTable arms(1);
    FiledTimeline filed;
    FileTimeline(filed, pool, arms, 8, 1, harken::TimelineEventKind::Envelope, 30);
    Sink sink;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
    executor.ExecuteQuantum(0);
    EXPECT_TRUE(sink.applied);
    EXPECT_EQ(sink.count, 8u);
    EXPECT_EQ(executor.Stats().spanOverflowDropped, 0u);
    (void)filed;
}

TEST(TimelineExecutor, NineEventTransactionWritesNoPrefix) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
    harken::TimelineArmTable arms(1);
    FiledTimeline filed;
    FileTimeline(filed, pool, arms, 9, 1, harken::TimelineEventKind::Envelope, 30);
    Sink sink;
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
    executor.ExecuteQuantum(0);
    EXPECT_FALSE(sink.applied);
    EXPECT_EQ(sink.count, 0u);
    EXPECT_EQ(executor.Stats().spanOverflowDropped, 1u);
    EXPECT_EQ(executor.Stats().protocolViolation, 1u);
    (void)filed;
}

TEST(TimelineExecutor, DigestIsIndependentOfPhysicalBinding) {
    std::uint64_t digests[2]{};
    for (std::uint32_t run = 0; run < 2; ++run) {
        harken::ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(), 0, 1));
        harken::TimelineArmTable arms(2);
        FiledTimeline filed;
        filed.target.programSemanticFingerprint = 91;
        filed.payloads[0].kind = harken::TimelineEventKind::Start;
        filed.payloads[0].target = filed.target;
        filed.program = {1, filed.payloads.data(), 1};
        ASSERT_TRUE(arms.Reserve(run, run + 1, 1, 0, 1, filed.target,
                                 &filed.arm));
        ASSERT_TRUE(arms.AcknowledgeApplied(filed.arm));
        ASSERT_TRUE(arms.Publish(filed.arm));
        filed.events[0].executeFrame = 1;
        filed.events[0].subeventPhase = 20;
        filed.events[0].payload = 0;
        harken::TransactionRequest request{};
        request.transactionId = 1;
        request.transportEpoch = 1;
        request.program = {1, &filed.program};
        request.armToken = filed.arm;
        request.events = filed.events.data();
        request.eventCount = 1;
        ASSERT_EQ(pool.Reserve(request,
                               harken::PrePublishLatePolicy::Refuse)
                      .status,
                  harken::ReserveStatus::Committed);
        pool.Publish(256);
        Sink sink;
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(256, 16, &pool, &arms, &sink));
        executor.ExecuteQuantum(0);
        ASSERT_TRUE(sink.applied);
        digests[run] = executor.Stats().digest;
    }
    EXPECT_EQ(digests[0], digests[1]);
}

TEST(TimelineExecutor, CallbackShapesKeepFixedQuantumCount) {
    constexpr std::uint32_t kBlockFrames = 256;
    constexpr std::array<std::uint32_t, 7> shapes = {
        64, 127, 128, 255, 256, 960, 1024};
    harken::DeviceBackendTestState deviceState{};
    DeviceBackendTestGuard seam(deviceState);
    VnConfig config =
        harkentest::MakeNullConfig(1, kBlockFrames, 48000);
    config.deviceMode = VN_DEVICE_REAL;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->device.Running());
    ASSERT_TRUE(engine->device.RenderAdmissionCommitted());

    const std::uint32_t channels = engine->config.deviceChannelCount;
    ASSERT_EQ(channels, 2u);
    constexpr std::size_t kGuardSamples = 8;
    constexpr float kGuardValue = 19.0f;
    std::vector<float> staging(
        static_cast<std::size_t>(kBlockFrames) * channels);
    std::uint64_t totalFrames = 0;
    for (const std::uint32_t shape : shapes) {
        const std::size_t outputSamples =
            static_cast<std::size_t>(shape) * channels;
        std::vector<float> guardedOutput(
            kGuardSamples + outputSamples + kGuardSamples, kGuardValue);
        ASSERT_TRUE(engine->device.PumpCallbackForTests(
            guardedOutput.data() + kGuardSamples, shape));

        totalFrames += shape;
        const std::uint64_t completeBlocks = totalFrames / kBlockFrames;
        const std::uint32_t partialFrames =
            static_cast<std::uint32_t>(totalFrames % kBlockFrames);
        const std::uint64_t expectedRenderedBlocks =
            completeBlocks + (partialFrames == 0 ? 0u : 1u);
        EXPECT_EQ(engine->mixer.BlocksRendered(), expectedRenderedBlocks)
            << "shape=" << shape << " totalFrames=" << totalFrames;

        std::uint32_t readFrames = 0;
        std::uint32_t availFrames = 0;
        ASSERT_TRUE(engine->device.GetRenderStateForTests(
            staging.data(), static_cast<std::uint32_t>(staging.size()),
            &readFrames, &availFrames));
        EXPECT_EQ(readFrames,
                  partialFrames == 0 ? kBlockFrames : partialFrames)
            << "shape=" << shape;
        EXPECT_EQ(availFrames,
                  partialFrames == 0 ? 0u : kBlockFrames - partialFrames)
            << "shape=" << shape;

        EXPECT_TRUE(std::all_of(
            guardedOutput.begin(),
            guardedOutput.begin() + kGuardSamples,
            [kGuardValue](float sample) { return sample == kGuardValue; }))
            << "shape=" << shape;
        EXPECT_TRUE(std::all_of(
            guardedOutput.begin() + kGuardSamples,
            guardedOutput.begin() + kGuardSamples + outputSamples,
            [](float sample) { return sample == 0.0f; }))
            << "shape=" << shape;
        EXPECT_TRUE(std::all_of(
            guardedOutput.begin() + kGuardSamples + outputSamples,
            guardedOutput.end(),
            [kGuardValue](float sample) { return sample == kGuardValue; }))
            << "shape=" << shape;
    }
}

TEST(TimelineExecutor, RepeatedUpdatePreparesOccurrenceExactlyOnce) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 80;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    ASSERT_EQ(Vn_Update(), VN_OK);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    EXPECT_EQ(live.prepareCount, 1u);
    EXPECT_EQ(live.phase,
              harken::EngineTimelineOccurrencePhase::AwaitingArmAck);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 1u);
}

TEST(TimelineExecutor, ProductionServiceFollowsThePerCallStateTable) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 89;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];

    EXPECT_EQ(engine->serviceFrameClock, 0u);
    EXPECT_EQ(engine->mixer.BlocksRendered(), 0u);
    EXPECT_EQ(engine->timelinePool.WindowStartQuantum(), 0u);
    EXPECT_EQ(engine->timelinePool.PublishCutoffFrame(), 256u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), 0u);
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Unprepared);
    EXPECT_EQ(engine->timelinePool.PageStamp(0), 0u);
    EXPECT_EQ(engine->timelinePool.PageStamp(1),
              harken::kUnpublishedQuantumStamp);

    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->serviceFrameClock, 0u);
    EXPECT_EQ(engine->mixer.BlocksRendered(), 0u);
    EXPECT_EQ(engine->timelinePool.WindowStartQuantum(), 0u);
    EXPECT_EQ(engine->timelinePool.PublishCutoffFrame(), 256u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), 0u);
    EXPECT_EQ(live.phase,
              harken::EngineTimelineOccurrencePhase::AwaitingArmAck);
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::PendingApply);

    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->mixer.BlocksRendered(), 1u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), 1u);
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::Armed);

    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->serviceFrameClock, 256u);
    EXPECT_EQ(engine->timelinePool.WindowStartQuantum(), 1u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), 1u);
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Filed);
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::Published);
    EXPECT_EQ(engine->timelinePool.PageStamp(1), 1u);
    EXPECT_EQ(engine->timelinePool.PublishCutoffFrame(), 1280u);

    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->mixer.BlocksRendered(), 2u);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), 2u);
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::Started);
}

TEST(TimelineExecutor, ProgramInstallRejectsMixedValidAndMalformedAudibleRows) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 86;
    harken::TimelineEventPayload payload{};
    payload.kind = harken::TimelineEventKind::Start;
    payload.target = target;
    harken::EventTemplate event{256, 1, 0, 0, 20, 0};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = &event;
    schedule.eventCount = 1;
    harken::TimelineProgramView program{1, &payload, 1};
    std::array<harken::TimelineOccurrenceInstallView, 2> rows{};
    rows[0].schedule = &schedule;
    rows[0].voice.soundId = harkentest::ObjectId(1);
    rows[0].voice.outputBusId = VN_BUS_MASTER;
    rows[0].voice.target = target;
    rows[1].schedule = nullptr;
    rows[1].voice.soundId = harkentest::ObjectId(1);
    rows[1].voice.outputBusId = VN_BUS_MASTER;
    const harken::TimelineProgramInstallView install{&program, rows.data(),
                                                    rows.size()};

    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    EXPECT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::InvalidProgram);
    EXPECT_EQ(engine->timelinePrograms.Current(), 0u);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
    EXPECT_EQ(engine->timelineProgramSets[0], nullptr);
    EXPECT_EQ(engine->timelineProgramSets[1], nullptr);
}

TEST(TimelineExecutor, TwoVoiceCapacityNeverCreatesAnOrphanSecondArm) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 90;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    const harken::TimelineArmToken first = live.arm;
    const std::uint64_t publishedHead = engine->commands.PublishedHead();
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(live.prepareCount, 1u);
    EXPECT_EQ(live.arm.slot, first.slot);
    EXPECT_EQ(live.arm.generation, first.generation);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 1u);
    EXPECT_EQ(engine->voices.ActiveCount(), 1u);
    EXPECT_EQ(engine->commands.PublishedHead(), publishedHead);

    for (std::uint32_t block = 0; block < 16 &&
         live.phase != harken::EngineTimelineOccurrencePhase::Terminal;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
        EXPECT_EQ(live.prepareCount, 1u);
        EXPECT_EQ(live.arm.slot, first.slot);
        EXPECT_EQ(live.arm.generation, first.generation);
        EXPECT_LE(engine->timelineArms.LiveCount(), 1u);
        EXPECT_LE(engine->voices.ActiveCount(), 1u);
    }
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Terminal);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
    EXPECT_EQ(engine->timelineDeferred.Count(), 0u);
}

TEST(TimelineExecutor, ArmAckWinsBeforeAudioSequenceDeadline) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 84;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Filed);
    EXPECT_EQ(engine->timelineArms.Resolve(live.arm)->state.load(),
              harken::ArmState::Published);
}

TEST(TimelineExecutor, TombstonedDelayedBatchIsABenignProtocolNoOp) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 98;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    ASSERT_TRUE(engine->timelineArms.RequestCancel(live.arm));
    engine->timelineExecutor.ExecuteQuantum(0);
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    ASSERT_EQ(state, harken::ArmState::Tombstoned);

    const std::uint64_t violations =
        engine->mixer.CommandProtocolViolationCount();
    engine->mixer.DrainCommandsForTest();
    EXPECT_EQ(engine->mixer.CommandProtocolViolationCount(), violations);
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::Tombstoned);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
}

TEST(TimelineExecutor, CancelWinsAgainstLateAckAndTombstonesSilently) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 91;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    ASSERT_TRUE(engine->timelineArms.RequestCancel(live.arm));
    EXPECT_FALSE(engine->timelineArms.AcknowledgeApplied(live.arm));
    engine->mixer.DrainCommandsForTest();
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::Tombstoned);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
}

TEST(TimelineExecutor, FreshPrepareAckAfterDeadlineSampleCannotWin) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 99;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(engine->mixer.BlocksRendered(), 1u);
    FreshPrepareDrainContext drain{engine};
    engine->timelineService->SetAfterFreshPrepareHookForTests(
        &DrainFreshTimelineArm, &drain);
    engine->timelineService->Service();
    engine->timelineService->SetAfterFreshPrepareHookForTests(nullptr,
                                                               nullptr);
    ASSERT_TRUE(drain.invoked);

    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    EXPECT_EQ(live.prepareCount, 1u);
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Cancelling);
    harken::ArmState state{};
    std::uint64_t completionSequence = 0;
    ASSERT_TRUE(engine->timelineArms.AckSnapshot(live.arm, &state,
                                                &completionSequence));
    EXPECT_EQ(state, harken::ArmState::CancelRequested);
    EXPECT_EQ(completionSequence, 2u);
    EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
}

TEST(TimelineExecutor, FreshPrepareAtDeadlineCancelsInTheSameService) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 96;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(engine->mixer.BlocksRendered(), 1u);
    engine->timelineService->Service();
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    EXPECT_EQ(live.prepareCount, 1u);
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Cancelling);
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::CancelRequested);
    EXPECT_FALSE(engine->timelineArms.AcknowledgeApplied(live.arm));
}

TEST(TimelineExecutor, PendingArmAckAfterDeadlineSampleCannotWin) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 100;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    ASSERT_EQ(live.phase,
              harken::EngineTimelineOccurrencePhase::AwaitingArmAck);

    engine->mixer.SetMaxCommandsPerQuantumForTest(0);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(engine->mixer.BlocksRendered(), 1u);
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    ASSERT_EQ(state, harken::ArmState::PendingApply);

    engine->mixer.SetMaxCommandsPerQuantumForTest(16);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(engine->mixer.BlocksRendered(), 2u);
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    ASSERT_EQ(state, harken::ArmState::Armed);
    std::uint64_t completionSequence = 0;
    ASSERT_TRUE(engine->timelineArms.AckSnapshot(live.arm, &state,
                                                &completionSequence));
    ASSERT_EQ(completionSequence, 2u);
    engine->timelineService->Service();

    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Cancelling);
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::CancelRequested);
    EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);

    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Terminal);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
}

TEST(TimelineExecutor, AudioSequenceDeadlineCancelsUnackedArm) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 85;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);

    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    engine->mixer.SetMaxCommandsPerQuantumForTest(0);
    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Cancelling);
    EXPECT_EQ(engine->timelineArms.Resolve(live.arm)->state.load(),
              harken::ArmState::CancelRequested);
    EXPECT_FALSE(engine->timelineArms.AcknowledgeApplied(live.arm));
}

TEST(TimelineExecutor, ReserveRetryPastDeadlineCancelsWithoutPostingAgain) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 87;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {1024, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    engine->nullBackend.Pump(1);
    live.forceReserveRetryForTest = true;
    engine->timelineService->Service();
    ASSERT_EQ(live.phase,
              harken::EngineTimelineOccurrencePhase::AwaitingReserveRetry);
    engine->mixer.DrainCommandsForTest();
    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    EXPECT_EQ(live.phase,
              harken::EngineTimelineOccurrencePhase::Cancelling);
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_EQ(state, harken::ArmState::CancelRequested);
    EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
}

TEST(TimelineExecutor, AckClosesBlockerAndRestoresDesiredLeadInOneService) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 88;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {1024, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    ASSERT_EQ(engine->timelinePool.PublishCutoffFrame(), 512u);
    engine->nullBackend.Pump(1);
    engine->timelineService->Service();
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Filed);
    const std::uint64_t desired =
        engine->timelinePool.WindowStartFrame() +
        static_cast<std::uint64_t>(engine->timelinePool.PublishLeadQuanta()) *
            engine->timelinePool.QuantumFrames();
    EXPECT_EQ(engine->timelinePool.PublishCutoffFrame(), desired);
}

TEST(TimelineExecutor, PendingArmBlocksItsPageButPublishesSafePrefix) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 83;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {1024, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    ASSERT_EQ(harken::InstallTimelineProgram(install).status,
              harken::TimelineRuntimeInstallStatus::Installed);

    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->timelinePool.PageStamp(0), 0u);
    EXPECT_EQ(engine->timelinePool.PageStamp(1), 1u);
    EXPECT_EQ(engine->timelinePool.PageStamp(2),
              harken::kUnpublishedQuantumStamp);
    EXPECT_EQ(engine->timelinePool.PageStamp(3),
              harken::kUnpublishedQuantumStamp);
}

TEST(TimelineExecutor, BootstrapBlocksStartPageUntilArmIsFiled) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->timelinePool.PageStamp(0), 0u);
    EXPECT_EQ(engine->timelinePool.PageStamp(1),
              harken::kUnpublishedQuantumStamp);

    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 82;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    ASSERT_EQ(harken::InstallTimelineProgram(install).status,
              harken::TimelineRuntimeInstallStatus::Installed);

    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->timelinePool.PageStamp(1),
              harken::kUnpublishedQuantumStamp);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->timelinePool.PageStamp(1), 1u);
}

TEST(TimelineExecutor, FreezeBeforeOwnerInvalidCancelsWithoutStarting) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(2048, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 92;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {1024, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]
            ->occurrences[0];
    ASSERT_NE(engine->playableInstances.BumpScheduleEpoch(live.ownerInstance),
              0u);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_NE(live.phase, harken::EngineTimelineOccurrencePhase::Started);
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(live.arm, &state));
    EXPECT_TRUE(state == harken::ArmState::CancelRequested ||
                state == harken::ArmState::Tombstoned);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
}

TEST(TimelineExecutor, FreezeAfterOwnerInvalidStartsThenDeclicksToTombstone) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(4096, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 81;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {2048, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::EngineTimelineOccurrence* livePtr =
        &engine->timelineProgramSets[installed.generation & 1u]
             ->occurrences[0];
    EXPECT_EQ(livePtr->phase,
              harken::EngineTimelineOccurrencePhase::AwaitingArmAck);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(livePtr->phase, harken::EngineTimelineOccurrencePhase::Filed);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    ASSERT_EQ(engine->timelineProgramSets[installed.generation & 1u]
                  ->occurrences.size(),
              1u);
    ASSERT_NE(livePtr, nullptr);
    harken::EngineTimelineOccurrence& live = *livePtr;
    const harken::TimelineArmTable::Row* startedRow =
        engine->timelineArms.Resolve(live.arm);
    ASSERT_NE(startedRow, nullptr);
    ASSERT_EQ(startedRow->state.load(), harken::ArmState::Started);
    ASSERT_NE(engine->playableInstances.BumpScheduleEpoch(live.ownerInstance),
              0u);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Stopping);
    EXPECT_EQ(engine->timelineArms.Resolve(live.arm)->state.load(),
              harken::ArmState::Started);
    for (std::uint32_t block = 0;
         block < 4 &&
         live.phase != harken::EngineTimelineOccurrencePhase::Terminal;
         ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
    }
    EXPECT_EQ(live.phase, harken::EngineTimelineOccurrencePhase::Terminal);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
}

TEST(TimelineExecutor, StopDeclickAt44100UsesCeilingFrames) {
    EXPECT_EQ((static_cast<std::uint64_t>(
                   harken::kTimelineStopDeclickMs) *
                   44100u +
               999u) /
                  1000u,
              221u);
}

TEST(TimelineExecutor, TimelineVoiceScratchShortfallPreservesPageAndSpanState) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    config.arenaBytes = 2560;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);

    const std::vector<float> dc(1024, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 1;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {768, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    ASSERT_EQ(harken::InstallTimelineProgram(install).status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    const std::uint64_t frontier =
        engine->timelinePool.ConsumerFrontier().load();
    const auto targetPage = engine->timelinePool.PublishedEmptyPrefixEnd(
        frontier, engine->mixer.BlocksRendered());
    ASSERT_EQ(1u, targetPage);
    harken::ConsumedEvent retained{};
    ASSERT_EQ(1u, engine->timelinePool.ConsumePageRt(targetPage, &retained, 1));
    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), targetPage);
    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), targetPage);
    EXPECT_EQ(targetPage, engine->timelinePool.PageStamp(targetPage));
    harken::TimelineArmObservation arm{};
    ASSERT_TRUE(engine->timelineArms.Observe(retained.armToken, &arm));
    EXPECT_EQ(0u, arm.startedBlockSequence);
    EXPECT_GT(engine->timelineExecutor.Stats().renderUnavailableDeferrals, 0u);
}

TEST(TimelineExecutor, TimelineArenaAdmissionUsesCascadeEstimator) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->voices.ScratchBytesBeforeTimeline(256), 0u);
    EXPECT_EQ(engine->voices.TimelineScratchBytes(256), 0u);
    EXPECT_TRUE(engine->voices.AdmitTimelineQuantum(0, 256));
}

TEST(TimelineExecutor, ArenaAdmissionIncludesLowerOrdinaryBlendAndSend) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    config.voiceSendSlots = 1;
    config.fxSlotsPerVoice = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(4096, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle ordinary = 0;
    ASSERT_EQ(harkentest::PlayRaw(&play, &ordinary), VN_OK);
    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    ASSERT_EQ(Vn_SetVoiceEffect(
                  ordinary, 0, VN_EFFECT_GAIN,
                  reinterpret_cast<const VnEffectDesc*>(&gain)),
              VN_OK);
    ASSERT_EQ(Vn_SetVoiceEffectBypass(ordinary, 0, 1), VN_OK);
    ASSERT_EQ(Vn_SetVoiceSend(ordinary, 0, VN_BUS_MASTER, 0.0f), VN_OK);
    engine->nullBackend.Pump(1);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 93;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {1024, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    ASSERT_EQ(harken::InstallTimelineProgram(install).status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);

    const std::uint64_t exact = engine->voices.ScratchBytesBeforeTimeline(256);
    ASSERT_GT(exact, engine->voices.TimelineScratchBytes(256));
    ASSERT_LE(exact, UINT32_MAX);
    EXPECT_TRUE(engine->voices.AdmitTimelineQuantum(
        static_cast<std::uint32_t>(exact), 256));
    EXPECT_FALSE(engine->voices.AdmitTimelineQuantum(
        static_cast<std::uint32_t>(exact - 1u), 256));
}

TEST(TimelineExecutor, MultipleTimelineVoicesReserveThroughTheLastTarget) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.voiceSendSlots = 1;
    config.fxSlotsPerVoice = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    std::array<float, 4096> samples{};
    samples.fill(0.25f);
    harken::PCMView view{};
    view.channels[0] = samples.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = samples.size();
    const harken::ResolvedPanGains pan{};

    std::array<VnVoiceHandle, 4> handles{};
    for (std::uint32_t slot = 0; slot < handles.size(); ++slot) {
        ASSERT_EQ(engine->voices.Play(
                      view, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f, pan, 0,
                      view.frames, 0,
                      harken::ResampleProfile::LegacyCompatibility,
                      &handles[slot]),
                  VN_OK);
        engine->voices.ApplyPlay(handles[slot], handles[slot], 0, 0);
    }
    harken::VoiceRenderContext* lowerOrdinary =
        engine->voices.Resolve(handles[0]);
    harken::VoiceRenderContext* timelineA = engine->voices.Resolve(handles[1]);
    harken::VoiceRenderContext* middleOrdinary =
        engine->voices.Resolve(handles[2]);
    harken::VoiceRenderContext* timelineB = engine->voices.Resolve(handles[3]);
    ASSERT_NE(lowerOrdinary, nullptr);
    ASSERT_NE(timelineA, nullptr);
    ASSERT_NE(middleOrdinary, nullptr);
    ASSERT_NE(timelineB, nullptr);
    lowerOrdinary->fxSlots[0].process =
        [](void*, float* const*, std::uint32_t, std::uint32_t,
           std::uint32_t) {};
    lowerOrdinary->fxBypassRamping[0] = true;
    lowerOrdinary->voiceSendActiveCount = 1;
    timelineA->timelineArmSlot = 0;
    timelineA->timelineArmGeneration = 1;
    timelineB->timelineArmSlot = 1;
    timelineB->timelineArmGeneration = 1;

    const std::uint64_t exact = engine->voices.ScratchBytesBeforeTimeline(256);
    const std::uint64_t expected =
        harken::VoiceCascadeScratchBytes(*lowerOrdinary, 256, 2) +
        harken::VoiceCascadeScratchBytes(*timelineA, 256, 2) +
        harken::VoiceCascadeScratchBytes(*middleOrdinary, 256, 2) +
        harken::VoiceCascadeScratchBytes(*timelineB, 256, 2);
    ASSERT_EQ(exact, expected);
    ASSERT_LE(exact, UINT32_MAX);
    EXPECT_TRUE(engine->voices.AdmitTimelineQuantum(
        static_cast<std::uint32_t>(exact), 256));
    EXPECT_FALSE(engine->voices.AdmitTimelineQuantum(
        static_cast<std::uint32_t>(exact - 1u), 256));
}

TEST(TimelineExecutor, MultipleTimelinePrepareCommitsOnlyAfterFullArenaAdmission) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.voiceSendSlots = 1;
    config.fxSlotsPerVoice = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> dc(4096, 0.25f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &desc), VN_OK);
    const harken::PCMView view = engine->registry.Lookup(harkentest::ObjectId(1));
    const harken::ResolvedPanGains pan{};

    auto startOrdinary = [&](std::uint32_t requestedSlot) {
        VnVoiceHandle handle = 0;
        EXPECT_EQ(engine->voices.Play(
                      view, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f, pan, 0,
                      view.frames, 0,
                      harken::ResampleProfile::LegacyCompatibility, &handle, 1,
                      requestedSlot),
                  VN_OK);
        engine->voices.ApplyPlay(handle, handle, 0, 0);
        return handle;
    };
    const VnVoiceHandle lowerOrdinary = startOrdinary(0);
    const VnVoiceHandle middleOrdinary = startOrdinary(2);
    harken::VoiceRenderContext* lower = engine->voices.Resolve(lowerOrdinary);
    ASSERT_NE(lower, nullptr);
    lower->fxSlots[0].process =
        [](void*, float* const*, std::uint32_t, std::uint32_t,
           std::uint32_t) {};
    lower->fxBypassRamping[0] = true;
    lower->voiceSendActiveCount = 1;
    ASSERT_NE(engine->voices.Resolve(middleOrdinary), nullptr);

    std::array<harken::TimelineSemanticTarget, 2> targets{};
    targets[0].programSemanticFingerprint = 97;
    targets[0].occurrenceOrdinal = 1;
    targets[1].programSemanticFingerprint = 97;
    targets[1].occurrenceOrdinal = 2;
    std::array<harken::TimelineEventPayload, 4> payloads{};
    std::array<std::array<harken::EventTemplate, 2>, 2> eventRows{};
    std::array<harken::ProgramView, 2> schedules{};
    std::array<harken::TimelineOccurrenceInstallView, 2> occurrences{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        payloads[i * 2].kind = harken::TimelineEventKind::Start;
        payloads[i * 2].target = targets[i];
        payloads[i * 2 + 1].kind = harken::TimelineEventKind::Stop;
        payloads[i * 2 + 1].target = targets[i];
        eventRows[i][0] = {256, i + 1u, 0, 0, 20, i * 2u};
        eventRows[i][1] = {768, i + 1u, 1, 0, 50, i * 2u + 1u};
        schedules[i].transactionId = i + 1u;
        schedules[i].events = eventRows[i].data();
        schedules[i].eventCount = eventRows[i].size();
        schedules[i].rollingGroups = true;
        occurrences[i].schedule = &schedules[i];
        occurrences[i].voice.soundId = harkentest::ObjectId(1);
        occurrences[i].voice.outputBusId = VN_BUS_MASTER;
        occurrences[i].voice.target = targets[i];
    }
    harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    const harken::TimelineProgramInstallView install{
        &program, occurrences.data(), occurrences.size()};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    const std::unique_ptr<harken::EngineTimelineProgramSet>& set =
        engine->timelineProgramSets[installed.generation & 1u];
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->occurrences.size(), 2u);
    ASSERT_EQ(set->occurrences[0].arm.renderSlot, 1u);
    ASSERT_EQ(set->occurrences[1].arm.renderSlot, 3u);
    ASSERT_EQ(set->occurrences[0].phase,
              harken::EngineTimelineOccurrencePhase::Filed);
    ASSERT_EQ(set->occurrences[1].phase,
              harken::EngineTimelineOccurrencePhase::Filed);

    const std::uint64_t exact = engine->voices.ScratchBytesBeforeTimeline(256);
    ASSERT_GT(exact, engine->voices.TimelineScratchBytes(256));
    ASSERT_LE(exact, UINT32_MAX);
    const std::uint32_t busPlaneBytes = 2u * 256u * sizeof(float);
    const std::uint64_t exactCapacity =
        static_cast<std::uint64_t>(busPlaneBytes) + exact;
    ASSERT_LE(exactCapacity, UINT32_MAX);
    const std::uint64_t frontier = engine->timelinePool.ConsumerFrontier().load();
    const std::uint64_t overflows = engine->mixer.Arena().OverflowCount();
    engine->mixer.ArenaForTests().SetCapacityForTests(
        static_cast<std::uint32_t>(exactCapacity - 1u));
    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), frontier);
    EXPECT_EQ(engine->mixer.Arena().OverflowCount(), overflows);
    const VnVoiceHandle timelineHandleA =
        (static_cast<std::uint64_t>(
             set->occurrences[0].arm.renderGeneration)
         << 32) |
        set->occurrences[0].arm.renderSlot;
    const VnVoiceHandle timelineHandleB =
        (static_cast<std::uint64_t>(
             set->occurrences[1].arm.renderGeneration)
         << 32) |
        set->occurrences[1].arm.renderSlot;
    const harken::VoiceRenderContext* timelineContextA =
        engine->voices.Resolve(timelineHandleA);
    const harken::VoiceRenderContext* timelineContextB =
        engine->voices.Resolve(timelineHandleB);
    ASSERT_NE(timelineContextA, nullptr);
    ASSERT_NE(timelineContextB, nullptr);
    EXPECT_EQ(timelineContextA->timelineEvents.eventCount, 0u);
    EXPECT_EQ(timelineContextB->timelineEvents.eventCount, 0u);

    engine->mixer.ArenaForTests().SetCapacityForTests(
        static_cast<std::uint32_t>(exactCapacity));
    engine->nullBackend.Pump(1);
    EXPECT_GT(engine->timelinePool.ConsumerFrontier().load(), frontier);
    EXPECT_EQ(engine->mixer.Arena().OverflowCount(), overflows);
    EXPECT_TRUE(std::any_of(
        engine->nullBackend.LastBlock(),
        engine->nullBackend.LastBlock() + 2u * 256u,
        [](float sample) { return sample != 0.0f; }));
    harken::ArmState state{};
    ASSERT_TRUE(engine->timelineArms.State(set->occurrences[0].arm, &state));
    EXPECT_EQ(state, harken::ArmState::Started);
    ASSERT_TRUE(engine->timelineArms.State(set->occurrences[1].arm, &state));
    EXPECT_EQ(state, harken::ArmState::Started);
    engine->mixer.ArenaForTests().RestoreCapacityForTests();
}

TEST(TimelineExecutor, ArenaShortfallPreservesFrontierAndPublishesCounters) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    config.arenaBytes = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint64_t frontier = engine->timelinePool.ConsumerFrontier().load();
    engine->nullBackend.Pump(1);
    EXPECT_EQ(engine->timelinePool.ConsumerFrontier().load(), frontier);
    harken::SnapshotPayload snapshot{};
    ASSERT_TRUE(engine->mixer.Snapshots().Capture(snapshot));
    EXPECT_EQ(snapshot.deadlineMissQuanta, 0u);
    EXPECT_EQ(engine->timelineExecutor.Stats().renderUnavailableDeferrals, 1u);
}

TEST(TimelineExecutor, UnpublishedCancellationWaitsForReadersAndRejectsReusedArm) {
    harken::TimelineArmTable arms(1);
    const harken::ParentAudioGroupRef parent{{0, 1}};
    harken::TimelineSemanticTarget target{};
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.ReserveGrouped(3, 5, 0, 0, 13, target, parent, 0, &arm));
    harken::TimelineArmReaderPin reader{};
    ASSERT_TRUE(arms.PinFrontierReader(arm, &reader));
    ASSERT_TRUE(arms.RequestCancel(arm));
    ASSERT_TRUE(arms.Tombstone(arm, 0));
    EXPECT_EQ(nullptr, arms.Resolve(arm));
    EXPECT_FALSE(arms.HasUnpublishedTombstoneExact(arm, parent, 0));
    ASSERT_TRUE(reader.Release(0));
    ASSERT_TRUE(arms.HasUnpublishedTombstoneExact(arm, parent, 0));
    EXPECT_FALSE(arms.HasUnpublishedTombstoneExact(arm, {{0, 2}}, 0));
    EXPECT_FALSE(arms.HasUnpublishedTombstoneExact(arm, parent, 1));
    auto foreignRender = arm;
    ++foreignRender.renderGeneration;
    EXPECT_FALSE(arms.HasUnpublishedTombstoneExact(foreignRender, parent, 0));
    ASSERT_TRUE(arms.Reclaim(arm, 1));
    harken::TimelineArmToken replacement{};
    ASSERT_TRUE(arms.ReserveGrouped(3, 5, 0, 0, 13, target, parent, 0, &replacement));
    ASSERT_TRUE(arms.RequestCancel(replacement));
    ASSERT_TRUE(arms.Tombstone(replacement, 0));
    EXPECT_EQ(arm.slot, replacement.slot);
    EXPECT_NE(arm.generation, replacement.generation);
    EXPECT_FALSE(arms.HasUnpublishedTombstoneExact(arm, parent, 0));
    EXPECT_TRUE(arms.HasUnpublishedTombstoneExact(replacement, parent, 0));
}

TEST(TimelineExecutor, UnobservedTombstoneRequiresRealReaderReturnAndRejectsAcknowledgedArm) {
    const harken::ParentAudioGroupRef parent{{0, 1}};
    for (const bool acknowledged : {false, true}) {
        SCOPED_TRACE(acknowledged);
        harken::TimelineArmTable arms(1);
        harken::TimelineArmToken arm{};
        ASSERT_TRUE(arms.ReserveGrouped(3, 5, 0, 0, 13, {}, parent, 0, &arm));
        harken::TimelineArmReaderPin reader{};
        ASSERT_TRUE(arms.PinFrontierReader(arm, &reader));
        if (acknowledged) ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
        ASSERT_TRUE(arms.RequestCancel(arm));
        ASSERT_TRUE(arms.Tombstone(arm, 1));
        EXPECT_FALSE(arms.HasUnobservedTombstoneExact(arm, parent, 0));
        ASSERT_TRUE(reader.Release(0));
        EXPECT_EQ(!acknowledged, arms.HasUnobservedTombstoneExact(arm, parent, 0));
        EXPECT_FALSE(arms.HasUnobservedTombstoneExact(arm, {{0, 2}}, 0));
        EXPECT_FALSE(arms.HasUnobservedTombstoneExact(arm, parent, 1));
        auto foreign = arm;
        ++foreign.renderGeneration;
        EXPECT_FALSE(arms.HasUnobservedTombstoneExact(foreign, parent, 0));
        ASSERT_TRUE(arms.Reclaim(arm, 2));
        harken::TimelineArmToken replacement{};
        ASSERT_TRUE(arms.ReserveGrouped(3, 5, 0, 0, 13, {}, parent, 0, &replacement));
        ASSERT_TRUE(arms.RequestCancel(replacement));
        ASSERT_TRUE(arms.Tombstone(replacement, 1));
        EXPECT_EQ(arm.slot, replacement.slot);
        EXPECT_NE(arm.generation, replacement.generation);
        EXPECT_FALSE(arms.HasUnobservedTombstoneExact(arm, parent, 0));
        EXPECT_TRUE(arms.HasUnobservedTombstoneExact(replacement, parent, 0));
    }
}

TEST(TimelineExecutor, AcknowledgedCancellationCannotUseUnpublishedGroupExit) {
    harken::TimelineArmTable arms(1);
    const harken::ParentAudioGroupRef parent{{0, 1}};
    harken::TimelineSemanticTarget target{};
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.ReserveGrouped(3, 5, 0, 0, 13, target, parent, 0, &arm));
    ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
    ASSERT_TRUE(arms.RequestCancel(arm));
    ASSERT_TRUE(arms.Tombstone(arm, 0));
    EXPECT_FALSE(arms.HasUnpublishedTombstoneExact(arm, parent, 0));
}

} // namespace

TEST(TimelineArmTable, CancelledPreparedTokenAckCannotConfirmGenuinelyReissuedRowAfterResolve) {
    for (const bool pending : {false, true}) {
        harken::TimelineArmTable table(1);
        harken::TimelineArmToken old{}, replacement{};
        if (pending) ASSERT_TRUE(table.PreparePendingImmediate(9, 11, 7, {}, &old));
        else ASSERT_TRUE(table.Prepare(9, 11, 256, 128, 7, {}, &old));
        struct Probe {
            harken::TimelineArmTable* table = nullptr;
            harken::TimelineArmToken old{};
            harken::TimelineArmToken* replacement = nullptr;
            bool pending = false;
            bool cancelled = false;
            bool prepared = false;
            bool committed = false;
            unsigned calls = 0;
        } probe{&table, old, &replacement, pending};
        table.SetTokenAcknowledgeHookForTests([](void* context) {
            auto& p = *static_cast<Probe*>(context);
            ++p.calls;
            p.cancelled = p.table->CancelPrepared(p.old);
            p.prepared = p.pending
                ? p.table->PreparePendingImmediate(9, 11, 7, {}, p.replacement)
                : p.table->Prepare(9, 11, 256, 128, 7, {}, p.replacement);
            p.committed = p.table->CommitPrepared(*p.replacement);
        }, &probe);
        EXPECT_FALSE(table.AcknowledgeApplied(old, 99));
        table.SetTokenAcknowledgeHookForTests(nullptr, nullptr);
        ASSERT_EQ(1u, probe.calls);
        ASSERT_TRUE(probe.cancelled);
        ASSERT_TRUE(probe.prepared);
        ASSERT_TRUE(probe.committed);
        ASSERT_EQ(old.slot, replacement.slot);
        ASSERT_NE(old.generation, replacement.generation);
        ASSERT_EQ(old.renderSlot, replacement.renderSlot);
        ASSERT_EQ(old.renderGeneration, replacement.renderGeneration);
        harken::TimelineArmObservation actual{};
        ASSERT_TRUE(table.Observe(replacement, &actual));
        EXPECT_EQ(harken::ArmState::PendingApply, actual.state);
        EXPECT_EQ(0u, actual.armedCompletionSequence);
        EXPECT_EQ(0u, actual.startedBlockSequence);
        EXPECT_FALSE(table.AcknowledgeApplied(
            (static_cast<std::uint64_t>(old.generation) << 32) | old.slot, 99));
        ASSERT_TRUE(table.AcknowledgeApplied(replacement, 7));
        ASSERT_TRUE(table.Observe(replacement, &actual));
        EXPECT_EQ(7u, actual.armedCompletionSequence);
        if (pending) ASSERT_TRUE(table.BindPendingImmediateStart(replacement, 256, 128));
        ASSERT_TRUE(table.RequestCancel(replacement));
        ASSERT_TRUE(table.Tombstone(replacement, 9));
        EXPECT_FALSE(table.Reclaim(replacement, 9));
        ASSERT_TRUE(table.Reclaim(replacement, 10));
        EXPECT_EQ(1u, table.FreeCount());
    }
    RecordProperty("arm_table_inline_bytes", static_cast<int>(sizeof(harken::TimelineArmTable)));
    RecordProperty("control_metadata_bytes", static_cast<int>(sizeof(harken::TimelineArmTable::AuthoredControlMetadata)));
    RecordProperty("arm_row_bytes", static_cast<int>(sizeof(harken::TimelineArmTable::Row)));
}
