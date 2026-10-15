#include "Common/ObjectIdTestSupport.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/TransitionInstancePool.h"
#include "harken/core/AudioSliceLease.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/PrivateSelectedQueueFutureCalendar.h"
#include "harken/core/ArmGroupTable.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/PrivateOriginalQueueGroupClockProof.h"
#include "harken/core/CommandQueue.h"
#include "harken/render/VoicePool.h"
#include "harken/render/VoiceTimelineSink.h"

#include <atomic>
#include <barrier>
#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <limits>

namespace harken::detail {
struct OriginalGroupMemberTestAccess {
    using Original = TimelineArmTable::OriginalGroupMember;
    static const Original* Member(const ScheduledTransactionPool& pool, const Plan03GroupSliceIdentity& group) {
        const auto offset = group.committedRow * pool.config_.maxArmsPerGroup;
        return group.backing == Plan03GroupBacking::ImmediateLane ?
            &pool.immediateGroupMemberOriginals_[offset] : &pool.pageGroupMemberOriginals_[offset];
    }
    static std::uint64_t Count(const TimelineArmTable& table, TimelineArmToken arm) {
        const auto& scope = table.groupMemberScopes_[arm.slot];
        return scope.arm.slot == arm.slot && scope.arm.generation == arm.generation &&
            scope.arm.renderSlot == arm.renderSlot && scope.arm.renderGeneration == arm.renderGeneration
            ? scope.activeCount : 0;
    }
    template<std::size_t N> static bool Covers(const TimelineArmTable& table,
        TimelineArmToken arm, const std::array<const Original*, N>& originals) {
        return table.CompleteOriginalGroupMemberCoverage(arm, originals.data(), N);
    }
    static Plan03GroupSliceIdentity ObservePreparedOrigin(const ScheduledTransactionPool& pool,
        DecisionToken decision, std::uint32_t slice) {
        for (std::size_t row = 0; row < pool.pagePlan03Groups_.size(); ++row) {
            const auto& original = pool.pageGroupMemberOriginals_[row * pool.config_.maxArmsPerGroup];
            if (original.serial_ != 0 && original.Group().decision.slot == decision.slot &&
                original.Group().decision.generation == decision.generation && original.Group().sliceIndex == slice)
                return original.Group();
        }
        return {};
    }
    static std::uint64_t Serial(const Original* original) { return original->serial_; }
    static Plan03GroupSliceIdentity Group(const Original* original) { return original->Group(); }
    static bool ReceiverAttached(const ScheduledTransactionPool& pool) { return pool.groupMemberReceiver_.owner_ != nullptr; }
    static void ExhaustSerial(TimelineArmTable& table) { table.nextGroupMemberSerial_ = UINT64_MAX; }
};
}

namespace {

harken::DecisionReserveRequest MakeRequest(std::uint64_t ticket = 1) {
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = ticket;
    request.world = 7;
    request.transport = 9;
    request.expectedContinuationGeneration = 11;
    request.sliceCount = 2;
    return request;
}

TEST(DecisionGroup, CancelledReservationIdentityWaitsForRealReaderAndRejectsReusedRow) {
    harken::DecisionTable table(1, 1);
    const auto request = MakeRequest();
    harken::DecisionToken token{};
    ASSERT_TRUE(table.Reserve(request, &token));
    harken::DecisionFuturePin reader{};
    ASSERT_TRUE(table.AcquireFuturePin(token, &reader));
    ASSERT_TRUE(table.Cancel(token));
    EXPECT_FALSE(table.MatchesCancelledReservation(token, request));
    EXPECT_FALSE(table.ReclaimCancelled(token));
    ASSERT_TRUE(reader.Release());
    ASSERT_TRUE(table.MatchesCancelledReservation(token, request));
    auto wrong = request;
    ++wrong.ticket.sequence;
    EXPECT_FALSE(table.MatchesCancelledReservation(token, wrong));
    wrong = request;
    ++wrong.world;
    EXPECT_FALSE(table.MatchesCancelledReservation(token, wrong));
    wrong = request;
    ++wrong.transport;
    EXPECT_FALSE(table.MatchesCancelledReservation(token, wrong));
    wrong = request;
    ++wrong.expectedContinuationGeneration;
    EXPECT_FALSE(table.MatchesCancelledReservation(token, wrong));
    wrong = request;
    --wrong.sliceCount;
    EXPECT_FALSE(table.MatchesCancelledReservation(token, wrong));
    wrong = request;
    wrong.transitionGate = {0, 1};
    EXPECT_FALSE(table.MatchesCancelledReservation(token, wrong));
    ASSERT_TRUE(table.ReclaimCancelled(token));
    harken::DecisionToken reused{};
    ASSERT_TRUE(table.Reserve(request, &reused));
    EXPECT_EQ(token.slot, reused.slot);
    EXPECT_NE(token.generation, reused.generation);
    EXPECT_FALSE(table.MatchesCancelledReservation(reused, request));
    ASSERT_TRUE(table.Cancel(reused));
    EXPECT_FALSE(table.MatchesCancelledReservation(token, request));
    EXPECT_TRUE(table.MatchesCancelledReservation(reused, request));
    ASSERT_TRUE(table.ReclaimCancelled(reused));
}

harken::ScheduledTransactionPool::Config MakePoolConfig() {
    harken::ScheduledTransactionPool::Config config{};
    config.quantumFrames = 100;
    config.scheduleHorizonFrames = 400;
    config.publishLeadQuanta = 1;
    config.maxScheduledTransactions = 4;
    config.maxTransactionSpanFrames = 400;
    config.maxIngressTransactionsPerQuantum = 4;
    config.maxFrameBatchesPerQuantum = 4;
    config.maxEventsPerQuantum = 8;
    config.maxTransactionEvents = 8;
    config.maxCurrentSlicesPerQuantum = 4;
    config.maxPlan03GroupHeadersPerPage = 1;
    config.maxArmsPerGroup = 2;
    return config;
}

TEST(DecisionGroup, PreparedTicketHoldBlocksRealMutatorsAbortAndConsumeRetainAudioProtocol) {
    harken::DecisionTable table(1, 2);
    const auto request = MakeRequest(0);
    harken::PreparedDecisionReservation original;
    ASSERT_TRUE(table.PrepareReservation(request, &original));
    const auto token = original.Token();
    ASSERT_NE(nullptr, table.PrepareFrontierControlProgram(token, 21));
    harken::DecisionTable::PreparedTicketCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedReservationCommit(original, request, 37, &proof));
    EXPECT_TRUE(table.PreparedTicketHeld(token));
    EXPECT_FALSE(table.IsOpen(token));
    harken::DecisionToken unchanged{19, 23};
    EXPECT_FALSE(table.CommitPreparedReservation(&original, 38, &unchanged));
    EXPECT_EQ(19u, unchanged.slot);
    EXPECT_EQ(23u, unchanged.generation);
    EXPECT_FALSE(table.CancelPreparedReservation(&original));
    EXPECT_FALSE(table.Cancel(token));
    EXPECT_FALSE(table.ReclaimCancelled(token));
    EXPECT_FALSE(table.AttachTransitionGateBeforeCommit(token, {0, 1}, 3));
    EXPECT_EQ(nullptr, table.PrepareFrontierControlProgram(token, 21));
    EXPECT_EQ(nullptr, table.PrepareFrontierControlProgram(token, 22));
    harken::DecisionFuturePin pin;
    EXPECT_FALSE(table.AcquireFuturePin(token, &pin));
    EXPECT_FALSE(pin.Valid());
    EXPECT_FALSE(table.SetFuturePinCountForTests(token, 1));
    EXPECT_FALSE(table.TryClaimAudio(token));
    table.ApplyClaimedAudioSlice(token);
    EXPECT_FALSE(table.MarkAudioAdmitted(token));
    EXPECT_FALSE(table.RecordAppliedSlice(token, {}, 0, 3));
    EXPECT_FALSE(table.Retire(token, 3));
    EXPECT_FALSE(table.TryInvalidatePending(token));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NotEligible, table.Invalidate(token));
    EXPECT_EQ(harken::DecisionDeadlineDisposition::NotEligible, table.InvalidateForDeadline(token));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NotEligible, table.InvalidateForAuthoredStop(token));
    harken::TimelineTransactionProgramRef control{};
    ASSERT_TRUE(table.CaptureFrontierControlProgram(token, &control));
    EXPECT_EQ(21u, control.programGeneration);
    proof.Release();
    ASSERT_TRUE(original.Valid());
    EXPECT_TRUE(table.MatchesOpenReservation(token, request));
    // Re-proving checks the actual unchanged pin/audio/layout prerequisites.
    ASSERT_TRUE(table.ProvePreparedReservationCommit(original, request, 37, &proof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(original.Valid());
    auto issued = request;
    issued.ticket.sequence = 37;
    EXPECT_TRUE(table.MatchesOpenReservation(token, issued));
    ASSERT_TRUE(table.AcquireFuturePin(token, &pin));
    ASSERT_TRUE(pin.Release());
    harken::ArmGroupTable groups(1, 2);
    harken::ArmGroupToken group{};
    ASSERT_TRUE(groups.Reserve(token, nullptr, 0, &group));
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, request.transport));
    harken::TimelineArmTable arms(1);
    harken::Plan03GroupSliceInput input{};
    input.decision = token;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(input, arms));
    harken::Plan03PageGroupView realGroup{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&realGroup, 1));
    EXPECT_EQ(token.slot, realGroup.identity.decision.slot);
    EXPECT_EQ(token.generation, realGroup.identity.decision.generation);
    harken::DecisionCommitPayload payload{};
    // This legacy marker matches the production Plan03 carrier; the actual
    // group identity and lifetime are held by the pool independently.
    payload.pages = {1, 1};
    payload.armGroup = group;
    ASSERT_TRUE(table.Commit(token, payload));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCommitted(token, &snapshot));
    EXPECT_EQ(37u, snapshot.ticket.sequence);
    EXPECT_EQ(harken::AudioGroupState::Pending, snapshot.audioState);
    EXPECT_EQ(0u, snapshot.consumedSlices);
    EXPECT_EQ(0u, snapshot.lastAudioSequence);
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    ASSERT_TRUE(table.TryClaimAudio(token));
    ASSERT_TRUE(table.RecordAppliedSlice(token, payload.pages, 0, 3));
    ASSERT_TRUE(table.RecordAppliedSlice(token, payload.pages, 1, 4));
    ASSERT_TRUE(table.MarkAudioAdmitted(token));
    ASSERT_TRUE(table.AcquireFuturePin(token, &pin));
    ASSERT_TRUE(table.Retire(token, 4));
    EXPECT_FALSE(table.ReclaimRetired(token));
    ASSERT_TRUE(pin.Release());
    ASSERT_TRUE(table.ReclaimRetired(token));
    ASSERT_TRUE(groups.BeginRetire(group));
    ASSERT_TRUE(groups.Reclaim(group, 4));
    EXPECT_EQ(0u, groups.LiveCount());
}

TEST(DecisionGroup, PreparedTicketProofRejectsOriginalInputMutationAndStaleReuse) {
    harken::DecisionTable table(1, 1);
    const auto request = MakeRequest(0);
    harken::PreparedDecisionReservation original;
    ASSERT_TRUE(table.PrepareReservation(request, &original));
    const auto token = original.Token();
    for (unsigned field = 0; field < 6; ++field) {
        auto wrong = request;
        if (field == 0) ++wrong.world;
        if (field == 1) ++wrong.transport;
        if (field == 2) ++wrong.expectedContinuationGeneration;
        if (field == 3) ++wrong.sliceCount;
        if (field == 4) wrong.transitionGate = {0, 1};
        if (field == 5) wrong.transitionGate.slot = 7;
        harken::DecisionTable::PreparedTicketCommitProof proof;
        EXPECT_FALSE(table.ProvePreparedReservationCommit(original, wrong, 17, &proof));
        EXPECT_FALSE(proof.Valid());
        EXPECT_TRUE(table.MatchesOpenReservation(token, request));
    }
    harken::DecisionTable::PreparedTicketCommitProof proof;
    EXPECT_FALSE(table.ProvePreparedReservationCommit(original, request, 0, &proof));
    ASSERT_TRUE(table.Cancel(token));
    EXPECT_FALSE(table.ProvePreparedReservationCommit(original, request, 17, &proof));
    ASSERT_TRUE(table.CancelPreparedReservation(&original));
    ASSERT_TRUE(table.PrepareReservation(request, &original));
    EXPECT_EQ(token.slot, original.Token().slot);
    EXPECT_GT(original.Token().generation, token.generation);
    EXPECT_FALSE(table.Cancel(token));
    ASSERT_TRUE(table.ProvePreparedReservationCommit(original, request, 17, &proof));
    proof.Release();
    EXPECT_TRUE(table.CancelPreparedReservation(&original));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(DecisionGroup, PreparedTicketProofFreezesCapAddressAndMoveAssignmentReleasesOriginalRow) {
    harken::DecisionTable table(2, 1);
    const auto request = MakeRequest(0);
    harken::PreparedDecisionReservation first, second;
    ASSERT_TRUE(table.PrepareReservation(request, &first));
    ASSERT_TRUE(table.PrepareReservation(request, &second));
    const auto firstToken = first.Token();
    const auto secondToken = second.Token();
    harken::DecisionTable::PreparedTicketCommitProof a, b;
    ASSERT_TRUE(table.ProvePreparedReservationCommit(first, request, 17, &a));
    auto refusedMove = std::move(first);
    EXPECT_FALSE(refusedMove.Valid());
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(a.Associated(&table, &first));
    first = std::move(second);
    EXPECT_EQ(firstToken.slot, first.Token().slot);
    EXPECT_EQ(secondToken.slot, second.Token().slot);
    second = std::move(first);
    EXPECT_EQ(firstToken.slot, first.Token().slot);
    EXPECT_EQ(secondToken.slot, second.Token().slot);
    ASSERT_TRUE(table.ProvePreparedReservationCommit(second, request, 18, &b));
    a = std::move(b);
    EXPECT_FALSE(b.Valid());
    EXPECT_TRUE(a.Associated(&table, &second));
    ASSERT_TRUE(table.CancelPreparedReservation(&first));
    EXPECT_FALSE(table.CancelPreparedReservation(&second));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(a));
    auto issued = request;
    issued.ticket.sequence = 18;
    EXPECT_TRUE(table.MatchesOpenReservation(secondToken, issued));
    ASSERT_TRUE(table.Cancel(secondToken));
    ASSERT_TRUE(table.ReclaimCancelled(secondToken));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(DecisionGroup, PreparedTicketProofMaximumOwnedCancellationAndConsumedDisposalNeverWrap) {
    for (const bool consume : {false, true}) {
        harken::DecisionTable table(1, 1);
        ASSERT_TRUE(table.SetGenerationForTests(0, std::numeric_limits<std::uint32_t>::max()));
        const auto request = MakeRequest(0);
        harken::PreparedDecisionReservation original;
        ASSERT_TRUE(table.PrepareReservation(request, &original));
        const auto token = original.Token();
        harken::DecisionTable::PreparedTicketCommitProof proof;
        ASSERT_TRUE(table.ProvePreparedReservationCommit(original, request, 17, &proof));
        if (consume) {
            harken::DecisionTable::ConsumePreparedReservationCommit(std::move(proof));
            ASSERT_TRUE(table.Cancel(token));
            EXPECT_FALSE(table.ReclaimCancelled(token));
        } else {
            proof.Release();
            ASSERT_TRUE(table.CancelPreparedReservation(&original));
        }
        EXPECT_FALSE(original.Valid());
        EXPECT_EQ(0u, table.LiveCount());
        EXPECT_EQ(1u, table.QuarantinedCount());
        EXPECT_FALSE(table.PrepareReservation(request, &original));
        EXPECT_FALSE(table.ProvePreparedReservationCommit(original, request, 18, &proof));
    }
}

struct SliceStep {
    harken::AudioSliceLease* lease = nullptr;
    std::uint32_t* applied = nullptr;
    bool valid = true;
    bool invalidateOnApply = false;
    harken::DecisionTable* decisions = nullptr;
    const harken::FrontierDecisionApplyProof* frontierProof = nullptr;
};

bool ValidateSliceStep(void* context) noexcept {
    return static_cast<SliceStep*>(context)->valid;
}

void ApplySliceStep(void* context) noexcept {
    auto* step = static_cast<SliceStep*>(context);
    ++*step->applied;
    if (step->decisions != nullptr && step->frontierProof != nullptr) {
        step->decisions->ApplyClaimedFrontierDecision(*step->frontierProof);
    }
    if (step->invalidateOnApply) {
        (void)step->lease->Invalidate(
            harken::AudioSliceInvalidateReason::WorldReplacement);
    }
}

struct FrontierReceiptHarness {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{1};
    harken::DecisionTable decisions{1, 2};
    harken::DecisionToken token{};
    harken::Plan03PageGroupView group{};
    harken::FrontierActivationDescriptor descriptor{};

    void Prepare() {
        ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 9));
        auto request = MakeRequest();
        request.sliceCount = 1;
        ASSERT_TRUE(decisions.Reserve(request, &token));
        harken::Plan03GroupSliceInput input{};
        input.decision = token;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            pool.ReservePlan03GroupSlice(input, arms));
        ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&group, 1));
        descriptor.entry.coordinator = token;
        descriptor.entry.attempt = 1;
        descriptor.entry.owner = {1, 2, 0, 0, 3, 4, 0, 5};
        descriptor.primary = group.identity;
        descriptor.order.logicCommitSequence = request.ticket.sequence;
        descriptor.worldGeneration = request.world;
        descriptor.worldSemanticFingerprint = 42;
        descriptor.expectedEntryEventCount = 1;
        descriptor.programGeneration = 3;
        ASSERT_TRUE(decisions.PrepareFrontierActivation(token, descriptor, pool));
    }
    void Commit() {
        harken::DecisionCommitPayload payload{};
        payload.pages = {1, 1};
        payload.armGroup = {1, 1};
        ASSERT_TRUE(decisions.Commit(token, payload));
    }
    void Publish() {
        ASSERT_NO_FATAL_FAILURE(Commit());
        ASSERT_TRUE(decisions.CanPublishFrontierActivation(token));
        ASSERT_TRUE(decisions.PublishFrontierActivation(token));
        EXPECT_FALSE(decisions.CanPublishFrontierActivation(token));
        EXPECT_FALSE(decisions.CanRollbackPreparedFrontierDecision(token, token));
        EXPECT_FALSE(decisions.RollbackPreparedFrontierDecision(token, token));
        pool.Publish(100, nullptr, nullptr, &arms);
        EXPECT_FALSE(decisions.RollbackPreparedFrontierDecision(token, token));
    }
};

TEST(DecisionGroup, FrontierPublicationHoldsActualTicketAndLoansUntilNoFailureApply) {
    FrontierReceiptHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    ASSERT_NO_FATAL_FAILURE(actual.Commit());
    auto* activation = actual.decisions.ResolveFrontierActivation(actual.token);
    ASSERT_NE(nullptr, activation);
    harken::DecisionFuturePin reader;
    ASSERT_TRUE(actual.decisions.AcquireFuturePin(actual.token, &reader));
    harken::DecisionCommittedSnapshot before{};
    ASSERT_TRUE(actual.decisions.CaptureCommitted(actual.token, &before));
    harken::DecisionTable::FrontierPublicationProof publication;
    auto wrong = actual.descriptor.entry;
    ++wrong.owner.serviceGeneration;
    EXPECT_FALSE(actual.decisions.ProveFrontierActivationPublication(actual.token, wrong, actual.pool, &publication));
    EXPECT_FALSE(publication.Valid());
    ASSERT_TRUE(actual.decisions.ProveFrontierActivationPublication(actual.token,
        actual.descriptor.entry, actual.pool, &publication));
    EXPECT_EQ(harken::FrontierReceiptState::PublicationHeld, activation->State());
    harken::DecisionTable::FrontierPublicationProof duplicate;
    EXPECT_TRUE(actual.decisions.IsFrontierPublicationProofFor(publication, actual.descriptor));
    EXPECT_FALSE(actual.decisions.IsFrontierPublicationProofFor(duplicate, actual.descriptor));
    auto changedDescriptor = actual.descriptor;
    ++changedDescriptor.order.executeFrame;
    EXPECT_FALSE(actual.decisions.IsFrontierPublicationProofFor(publication, changedDescriptor));
    harken::DecisionTable foreign(2, 2);
    EXPECT_FALSE(foreign.IsFrontierPublicationProofFor(publication, actual.descriptor));
    EXPECT_FALSE(actual.decisions.ProveFrontierActivationPublication(actual.token,
        actual.descriptor.entry, actual.pool, &duplicate));
    EXPECT_FALSE(actual.decisions.PublishFrontierActivation(actual.token));
    EXPECT_FALSE(actual.decisions.CancelPreparedFrontierActivation(actual.token));
    EXPECT_FALSE(actual.decisions.RollbackPreparedFrontierDecision(actual.token, actual.token));
    EXPECT_FALSE(activation->TryBeginAudio(actual.descriptor.entry));
    EXPECT_EQ(harken::FrontierCancelDisposition::Stale,
        activation->Cancel(actual.descriptor.entry, harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_EQ(harken::DecisionDeadlineDisposition::DeferredByFrontier,
        actual.decisions.InvalidateForDeadline(actual.token));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::DeferredByFrontier,
        actual.decisions.InvalidateForAuthoredStop(actual.token));
    harken::DecisionFuturePin extra;
    EXPECT_FALSE(actual.decisions.AcquireFuturePin(actual.token, &extra));
    EXPECT_FALSE(actual.decisions.CloneFuturePin(reader, &extra));
    EXPECT_TRUE(reader.Release());
    harken::DecisionCommittedSnapshot held{};
    ASSERT_TRUE(actual.decisions.CaptureCommitted(actual.token, &held));
    EXPECT_EQ(before.ticket.sequence, held.ticket.sequence);
    EXPECT_EQ(before.participantCount, held.participantCount);
    EXPECT_EQ(before.futurePinCount - 1u, held.futurePinCount);
    EXPECT_EQ(harken::AudioGroupState::Pending, held.audioState);
    EXPECT_FALSE(held.authoredStopInvalidated);
    EXPECT_EQ(harken::AudioSliceState::Ready, actual.group.lease->Capture().state);
    harken::DecisionTable::ApplyFrontierActivationPublication(std::move(publication));
    EXPECT_FALSE(publication.Valid());
    EXPECT_FALSE(actual.decisions.IsFrontierPublicationProofFor(publication, actual.descriptor));
    EXPECT_EQ(harken::FrontierReceiptState::Pending, activation->State());
    ASSERT_TRUE(reader.Release());
    EXPECT_EQ(harken::FrontierCancelDisposition::CancelledBeforeAudio,
        activation->Cancel(actual.descriptor.entry, harken::AudioSliceInvalidateReason::Cancel));
    harken::FrontierActivationReceipt receipt{};
    ASSERT_TRUE(actual.decisions.ReconcileFrontierActivation(actual.token, actual.descriptor.entry, 1, &receipt));
    EXPECT_EQ(harken::FrontierReceiptReason::Cancelled, receipt.reason);
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, actual.decisions.Invalidate(actual.token));
    ASSERT_TRUE(actual.decisions.Retire(actual.token, 1));
    ASSERT_TRUE(actual.decisions.ReclaimRetired(actual.token));
    EXPECT_EQ(0u, actual.decisions.LiveCount());
}

TEST(DecisionGroup, FrontierPublicationMoveAbandonAndOwnerDestructionReturnExactReservation) {
    FrontierReceiptHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare());
    ASSERT_NO_FATAL_FAILURE(first.Commit());
    ASSERT_NO_FATAL_FAILURE(second.Prepare());
    ASSERT_NO_FATAL_FAILURE(second.Commit());
    harken::DecisionTable::FrontierPublicationProof a, b;
    ASSERT_TRUE(first.decisions.ProveFrontierActivationPublication(first.token,
        first.descriptor.entry, first.pool, &a));
    ASSERT_TRUE(second.decisions.ProveFrontierActivationPublication(second.token,
        second.descriptor.entry, second.pool, &b));
    a = std::move(b);
    EXPECT_FALSE(b.Valid());
    EXPECT_TRUE(first.decisions.CanPublishFrontierActivation(first.token));
    EXPECT_FALSE(second.decisions.CanPublishFrontierActivation(second.token));
    a.Release();
    EXPECT_TRUE(second.decisions.CanPublishFrontierActivation(second.token));
    ASSERT_TRUE(first.decisions.ProveFrontierActivationPublication(first.token,
        first.descriptor.entry, first.pool, &a));
    a.Release();
    ASSERT_TRUE(first.decisions.RollbackPreparedFrontierDecision(first.token, first.token));
    ASSERT_TRUE(first.decisions.CancelPreparedFrontierActivation(first.token));
    ASSERT_TRUE(first.decisions.Cancel(first.token));
    ASSERT_TRUE(first.decisions.ReclaimCancelled(first.token));
    EXPECT_FALSE(first.decisions.ProveFrontierActivationPublication(first.token,
        first.descriptor.entry, first.pool, &a));
    auto owner = std::make_unique<FrontierReceiptHarness>();
    ASSERT_NO_FATAL_FAILURE(owner->Prepare());
    ASSERT_NO_FATAL_FAILURE(owner->Commit());
    ASSERT_TRUE(owner->decisions.ProveFrontierActivationPublication(owner->token,
        owner->descriptor.entry, owner->pool, &a));
    owner.reset();
    EXPECT_FALSE(a.Valid());
    a.Release();
}

TEST(DecisionGroup, FrontierPublicationReaderDestructionReturnsLoanBeforeAbandonAndReclaim) {
    FrontierReceiptHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    ASSERT_NO_FATAL_FAILURE(actual.Commit());
    harken::DecisionCommittedSnapshot baseline{};
    ASSERT_TRUE(actual.decisions.CaptureCommitted(actual.token, &baseline));
    ASSERT_EQ(1u, baseline.futurePinCount);
    harken::DecisionTable::FrontierPublicationProof publication;
    {
        harken::DecisionFuturePin reader;
        ASSERT_TRUE(actual.decisions.AcquireFuturePin(actual.token, &reader));
        ASSERT_TRUE(actual.decisions.ProveFrontierActivationPublication(actual.token,
            actual.descriptor.entry, actual.pool, &publication));
    }
    harken::DecisionCommittedSnapshot after{};
    ASSERT_TRUE(actual.decisions.CaptureCommitted(actual.token, &after));
    EXPECT_EQ(baseline.futurePinCount, after.futurePinCount);
    EXPECT_EQ(harken::FrontierReceiptState::PublicationHeld,
        actual.decisions.ResolveFrontierActivation(actual.token)->State());
    publication.Release();
    ASSERT_TRUE(actual.decisions.RollbackPreparedFrontierDecision(actual.token, actual.token));
    ASSERT_TRUE(actual.decisions.CancelPreparedFrontierActivation(actual.token));
    ASSERT_TRUE(actual.decisions.Cancel(actual.token));
    EXPECT_TRUE(actual.decisions.ReclaimCancelled(actual.token));
    EXPECT_EQ(0u, actual.decisions.LiveCount());
}

TEST(DecisionGroup, FrontierPublicationReaderMoveToEmptyReturnsLoanBeforeApplyAndReclaim) {
    FrontierReceiptHarness actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare());
    ASSERT_NO_FATAL_FAILURE(actual.Commit());
    harken::DecisionCommittedSnapshot baseline{};
    ASSERT_TRUE(actual.decisions.CaptureCommitted(actual.token, &baseline));
    ASSERT_EQ(1u, baseline.futurePinCount);
    harken::DecisionFuturePin reader;
    ASSERT_TRUE(actual.decisions.AcquireFuturePin(actual.token, &reader));
    harken::DecisionTable::FrontierPublicationProof publication;
    ASSERT_TRUE(actual.decisions.ProveFrontierActivationPublication(actual.token,
        actual.descriptor.entry, actual.pool, &publication));
    reader = harken::DecisionFuturePin{};
    EXPECT_FALSE(reader.Valid());
    harken::DecisionCommittedSnapshot after{};
    ASSERT_TRUE(actual.decisions.CaptureCommitted(actual.token, &after));
    EXPECT_EQ(baseline.futurePinCount, after.futurePinCount);
    harken::DecisionTable::ApplyFrontierActivationPublication(std::move(publication));
    auto* activation = actual.decisions.ResolveFrontierActivation(actual.token);
    ASSERT_NE(nullptr, activation);
    EXPECT_EQ(harken::FrontierCancelDisposition::CancelledBeforeAudio,
        activation->Cancel(actual.descriptor.entry, harken::AudioSliceInvalidateReason::Cancel));
    harken::FrontierActivationReceipt receipt{};
    ASSERT_TRUE(actual.decisions.ReconcileFrontierActivation(actual.token, actual.descriptor.entry, 1, &receipt));
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated,
        actual.decisions.Invalidate(actual.token));
    ASSERT_TRUE(actual.decisions.Retire(actual.token, 1));
    EXPECT_TRUE(actual.decisions.ReclaimRetired(actual.token));
    EXPECT_EQ(0u, actual.decisions.LiveCount());
}

struct DecisionLaneHarness {
    harken::DecisionTable decisions{2, 2};
    harken::ImmediateActionLane lane;
    harken::ImmediateLaneHandle handle{};
    std::array<harken::DecisionToken, 2> tokens{};

    void ReserveDecisions(std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index)
            ASSERT_TRUE(decisions.Reserve(MakeRequest(index + 1), &tokens[index]));
    }

    void Bind(harken::ScheduledTransactionPool& pool,
              const harken::Plan03GroupSliceIdentity* groups,
              std::uint32_t count) {
        ASSERT_TRUE(lane.Reset({1, 2, 2, 100, false, 2}));
        ASSERT_TRUE(lane.Reserve(17, {0, count, 0, 1}, &handle));
        for (std::uint32_t index = 0; index < count; ++index)
            ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(
                groups[index], lane, 17, handle.slot, handle.generation));
        ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 400));
    }

    void Publish(harken::ScheduledTransactionPool& pool,
                 harken::TimelineArmTable& arms,
                 const harken::Plan03GroupSliceIdentity* groups,
                 std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index)
            ASSERT_TRUE(lane.DeclareGroup(handle, {groups[index], 20}));
        ASSERT_TRUE(lane.Prepare(handle, nullptr, 0));
        std::uint64_t anchor = UINT64_MAX;
        ASSERT_TRUE(lane.TryPlace(handle, 400, 0, &anchor));
        ASSERT_EQ(400u, anchor);
        ASSERT_TRUE(lane.PinGroups(handle, pool, decisions, 2));
        ASSERT_TRUE(lane.PinArms(handle, arms));
        ASSERT_TRUE(pool.PublishImmediateLaneGroups(17));
        ASSERT_TRUE(lane.Publish(handle));
    }
};

TEST(DecisionGroup, PrivateImmediateCohortCapacityRefusalReclaimsUnpublishedPrefix) {
    harken::ScheduledTransactionPool pool;
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxArmsPerGroup = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::TimelineArmTable arms(2);
    harken::DecisionTable decisions(3, 2);
    harken::CommandQueue queue(4);
    std::array<harken::DecisionToken, 3> decisionsByGroup{};
    for (std::uint32_t index = 0; index < decisionsByGroup.size(); ++index)
        ASSERT_TRUE(decisions.Reserve(MakeRequest(index + 1),
                                      &decisionsByGroup[index]));
    ASSERT_EQ(3u, decisions.LiveCount());

    std::array<harken::TimelineArmToken, 2> members{};
    for (std::uint32_t index = 0; index < members.size(); ++index) {
        ASSERT_TRUE(arms.PreparePendingImmediateGrouped(
            index + 1, 1, 7, {},
            harken::ParentAudioGroupRef{decisionsByGroup[index + 1]}, 0,
            &members[index]));
        harken::TimelineArmObservation observation{};
        ASSERT_TRUE(arms.Observe(members[index], &observation));
        EXPECT_EQ(harken::ArmState::Prepared, observation.state);
        EXPECT_EQ(0u, observation.memberIndex);
    }
    ASSERT_EQ(2u, arms.LiveCount());
    harken::CommandQueue::BatchReservation batch{};
    ASSERT_TRUE(queue.ReserveBatch(2, &batch));
    harken::CommandRecord command{};
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_FALSE(queue.TryPop(command));

    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = decisionsByGroup[0];
    harken::Plan03GroupSliceIdentity primary{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &primary));
    input.decision = decisionsByGroup[1];
    input.members = &members[0];
    input.memberCount = 1;
    harken::Plan03GroupSliceIdentity first{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &first));
    ASSERT_EQ(harken::Plan03GroupBacking::ImmediateLane, primary.backing);
    ASSERT_EQ(harken::Plan03GroupBacking::ImmediateLane, first.backing);
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(first, &reader));
    ASSERT_TRUE(reader.Valid());

    input.decision = decisionsByGroup[2];
    input.members = &members[1];
    harken::Plan03GroupSliceIdentity refused{};
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &refused));
    EXPECT_FALSE(refused.Valid());
    std::array<harken::Plan03ImmediateLaneGroupDescriptor, 2> cohort{};
    ASSERT_EQ(2u, pool.CaptureImmediateLaneCohortGroups(
                      17, cohort.data(), cohort.size()));
    EXPECT_EQ(primary, cohort[0].identity);
    EXPECT_EQ(first, cohort[1].identity);
    for (const auto& identity : {primary, first}) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(identity, &view));
        EXPECT_EQ(identity, view.identity);
        EXPECT_EQ(nullptr, view.frontier);
        ASSERT_NE(nullptr, view.lease);
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
    }
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_FALSE(queue.TryPop(command));
    EXPECT_EQ(3u, decisions.LiveCount());
    EXPECT_EQ(2u, arms.LiveCount());

    ASSERT_TRUE(queue.CancelReservedBatchExact(&batch));
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_FALSE(queue.TryPop(command));
    ASSERT_TRUE(reader.Release(0));
    EXPECT_FALSE(reader.Valid());
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0));
    EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    harken::Plan03PageGroupView retired{};
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(primary, &retired));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(first, &retired));
    for (const auto& member : members) {
        ASSERT_TRUE(arms.CancelPrepared(member));
        harken::TimelineArmObservation observation{};
        EXPECT_FALSE(arms.Observe(member, &observation));
        const auto* row = arms.RowAt(member.slot);
        ASSERT_NE(nullptr, row);
        EXPECT_EQ(harken::ArmState::Free,
                  row->state.load(std::memory_order_acquire));
        EXPECT_EQ(0u, row->frontierReaderPins.load(std::memory_order_acquire));
    }
    EXPECT_EQ(0u, arms.LiveCount());
    for (const auto& token : decisionsByGroup) {
        ASSERT_TRUE(decisions.Cancel(token));
        ASSERT_TRUE(decisions.ReclaimCancelled(token));
        EXPECT_FALSE(decisions.Contains(token));
    }
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_EQ(0u, queue.PublishedHead());
    EXPECT_FALSE(queue.TryPop(command));
}

TEST(DecisionGroup, FrontierControlProgramUsesDecisionStorageUntilOriginalReaderPinReleases) {
    harken::DecisionTable decisions(1, 1);
    harken::DecisionToken token{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(), &token));
    const auto* program = decisions.PrepareFrontierControlProgram(token, 37);
    ASSERT_NE(nullptr, program);
    const auto* payload = program->payloads;
    EXPECT_EQ(harken::TimelineProgramRole::ControllerActivation, program->role);
    EXPECT_EQ(harken::TimelineEventKind::ControllerActivation, payload->kind);
    EXPECT_EQ(program, decisions.PrepareFrontierControlProgram(token, 37));
    EXPECT_EQ(nullptr, decisions.PrepareFrontierControlProgram(token, 38));
    harken::DecisionFuturePin pin;
    ASSERT_TRUE(decisions.AcquireFuturePin(token, &pin));
    ASSERT_TRUE(decisions.Cancel(token));
    EXPECT_FALSE(decisions.ReclaimCancelled(token));
    EXPECT_EQ(37u, program->generation);
    EXPECT_EQ(payload, program->payloads);
    EXPECT_EQ(nullptr, decisions.PrepareFrontierControlProgram(token, 38));
    ASSERT_TRUE(pin.Release());
    ASSERT_TRUE(decisions.ReclaimCancelled(token));
    harken::DecisionToken reused{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(2), &reused));
    EXPECT_NE(token.generation, reused.generation);
    EXPECT_EQ(nullptr, decisions.PrepareFrontierControlProgram(token, 39));
    const auto* next = decisions.PrepareFrontierControlProgram(reused, 39);
    ASSERT_NE(nullptr, next);
    EXPECT_EQ(program, next);
    EXPECT_EQ(39u, next->generation);
}

TEST(DecisionGroup, FrontierMembersBorrowOnlyEntryClaimAndKeepExactPageRetirement) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    auto config = MakePoolConfig();
    config.maxPlan03GroupHeadersPerPage = 3;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.decision = {1, 1};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(input, arms));
    input.decision = {2, 1};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(input, arms));
    input.executeFrame = 100;
    input.sliceIndex = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(input, arms));
    std::array<harken::Plan03PageGroupView, 3> groups{};
    ASSERT_EQ(3u, pool.CaptureLivePlan03Groups(groups.data(), groups.size()));
    const auto primary = groups[0].identity;
    const auto member = groups[1].identity;
    const auto future = groups[2].identity;
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = primary.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.group = primary;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = primary;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(primary, binding));
    binding.role = harken::FrontierGroupRole::EntryMember;
    binding.operation.group = member;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(member, binding));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(primary, 0));
    binding.role = harken::FrontierGroupRole::Future;
    binding.operation.kind = harken::FrontierOperationKind::Genuine;
    binding.operation.group = future;
    binding.operation.order.executeFrame = 100;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(future, binding));
    ASSERT_EQ(3u, pool.CaptureLivePlan03Groups(groups.data(), groups.size()));
    EXPECT_EQ(primary, groups[0].identity);
    EXPECT_EQ(member, groups[1].identity);
    EXPECT_EQ(groups[0].lease, groups[1].lease);
    EXPECT_NE(groups[0].lease, groups[2].lease);
    ASSERT_NE(nullptr, groups[1].frontier);
    EXPECT_EQ(harken::FrontierGroupRole::EntryMember, groups[1].frontier->role);
    EXPECT_FALSE(pool.Reset(config, 0, 10));
    pool.Publish(200, nullptr, nullptr, &arms);
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(primary, 1));
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(member, 1));
    std::uint32_t applied = 0;
    SliceStep step{groups[0].lease, &applied};
    harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
        harken::ApplyAudioSliceWhole(*groups[0].lease, &operation, 1));
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, groups[1].lease->Capture().state);
    EXPECT_EQ(harken::AudioSliceState::Ready, groups[2].lease->Capture().state);
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(member, 1));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(member, 2));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(primary, 2));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(primary, &groups[0]));
    ASSERT_NE(nullptr, groups[0].frontier);
    EXPECT_EQ(harken::FrontierGroupRole::Coordinator, groups[0].frontier->role);
    EXPECT_FALSE(pool.FrontierCoordinatorDetached(primary, binding.entry, 1));
    EXPECT_TRUE(pool.FrontierCoordinatorDetached(primary, binding.entry, 2));
    pool.ConsumerFrontier().store(1, std::memory_order_release);
    pool.Advance();
    EXPECT_EQ(100u, pool.WindowStartFrame());
    EXPECT_FALSE(pool.FrontierCoordinatorDetached(primary, binding.entry, 2));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(future, &groups[2]));
    EXPECT_EQ(harken::AudioSliceState::Ready, groups[2].lease->Capture().state);
    EXPECT_EQ(harken::FrontierGroupRole::Future, groups[2].frontier->role);
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(future, 2));
    ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
        groups[2].lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(future, 2));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(future, 3));
    pool.ConsumerFrontier().store(2, std::memory_order_release);
    pool.Advance();
    EXPECT_EQ(200u, pool.WindowStartFrame());
}

TEST(DecisionGroup, ImmediateLaneGroupIdentitySurvivesAnchorRebaseAndSharesEntryLease) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    laneHarness.ReserveDecisions(2);
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity primary{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &primary));
    input.decision = laneHarness.tokens[1];
    harken::Plan03GroupSliceIdentity member{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &member));
    const std::array laneGroups{primary, member};
    laneHarness.Bind(pool, laneGroups.data(), 2);

    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = primary.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = primary;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = primary;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(primary, binding));
    binding.role = harken::FrontierGroupRole::EntryMember;
    binding.operation.group = member;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(member, binding));

    harken::ImmediateLaneFrontierClaim privateClaim{};
    EXPECT_FALSE(pool.ClaimImmediateLaneFrontier(primary, &privateClaim));
    ASSERT_TRUE(pool.ClaimPrivateImmediateLaneFrontier(primary, &privateClaim));
    EXPECT_FALSE(pool.ClaimPrivateImmediateLaneFrontier(primary, &privateClaim));
    ASSERT_TRUE(pool.ReleaseImmediateLaneFrontier(&privateClaim));

    std::array<harken::Plan03PageGroupView, 2> live{};
    ASSERT_EQ(2u, pool.CaptureLivePlan03Groups(live.data(), live.size()));
    EXPECT_EQ(primary, live[0].identity);
    EXPECT_EQ(member, live[1].identity);

    laneHarness.Publish(pool, arms, laneGroups.data(), 2);
    EXPECT_FALSE(pool.ClaimPrivateImmediateLaneFrontier(primary, &privateClaim));
    std::array<harken::Plan03PageGroupView, 2> due{};
    EXPECT_EQ(0u, pool.CaptureImmediateLaneGroupsRt(1, due.data(), due.size()));
    ASSERT_EQ(2u, pool.CaptureImmediateLaneGroupsRt(4, due.data(), due.size()));
    harken::Plan03PageGroupView primaryView{}, memberView{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(primary, &primaryView));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(member, &memberView));
    EXPECT_EQ(primary, primaryView.identity);
    EXPECT_EQ(member, memberView.identity);
    EXPECT_EQ(primaryView.lease, memberView.lease);
    ASSERT_NE(nullptr, memberView.frontier);
    EXPECT_EQ(harken::FrontierGroupRole::EntryMember, memberView.frontier->role);
    const auto owner = binding.operation.owner;
    EXPECT_TRUE(pool.InvalidateFrontierOwnerOperations(owner, member.decision));
    harken::ImmediateLaneFrontierClaim primaryClaim{};
    harken::ImmediateLaneFrontierClaim memberClaim{};
    EXPECT_FALSE(pool.ClaimImmediateLaneFrontier(member, &memberClaim));
    ASSERT_TRUE(pool.ClaimImmediateLaneFrontier(primary, &primaryClaim));
    EXPECT_FALSE(pool.InvalidateFrontierOwnerOperations(owner));
    ASSERT_TRUE(pool.ReleaseImmediateLaneFrontier(&primaryClaim));
    EXPECT_FALSE(pool.ClaimImmediateLaneFrontier(primary, &primaryClaim));
    EXPECT_FALSE(pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, ImmediateLaneFutureMemberBlocksAppliedEntryDetachUntilRetired) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    laneHarness.ReserveDecisions(2);
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity primary{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms,
        &primary));
    input.decision = laneHarness.tokens[1];
    harken::Plan03GroupSliceIdentity member{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms,
        &member));
    const std::array laneGroups{primary, member};
    laneHarness.Bind(pool, laneGroups.data(), 2);

    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = primary.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = primary;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = primary;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(primary, binding));
    binding.role = harken::FrontierGroupRole::EntryMember;
    binding.operation.group = member;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(member, binding));
    laneHarness.Publish(pool, arms, laneGroups.data(), 2);
    ASSERT_TRUE(pool.AcquireImmediateLaneFutureDependent(member));

    harken::Plan03PageGroupView primaryView{}, memberView{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(primary, &primaryView));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(member, &memberView));
    ASSERT_EQ(primaryView.lease, memberView.lease);
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(primary, 1));
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(member, 1));
    std::uint32_t applied = 0;
    SliceStep step{primaryView.lease, &applied};
    harken::AudioSliceOperation operation{
        &step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
        harken::ApplyAudioSliceWhole(*primaryView.lease, &operation, 1));
    EXPECT_EQ(harken::AudioSliceState::SliceApplied,
        memberView.lease->Capture().state);
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(member, 2));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(primary, 2));
    ASSERT_TRUE(pool.ReleaseImmediateLaneFutureDependent(member));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(member, 2));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(primary, 2));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(primary, &primaryView));
    ASSERT_NE(nullptr, primaryView.frontier);
    EXPECT_EQ(harken::FrontierGroupRole::Coordinator, primaryView.frontier->role);
    EXPECT_FALSE(pool.FrontierCoordinatorDetached(primary, binding.entry, 1));
    EXPECT_FALSE(pool.FrontierCoordinatorDetached(primary, binding.entry, 2));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    EXPECT_TRUE(pool.FrontierCoordinatorDetached(primary, binding.entry, 2));
}

TEST(DecisionGroup, ImmediateLaneFrontierCancellationRetainsScopeReasonAndClaimOrder) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    laneHarness.ReserveDecisions(2);
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity first{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &first));
    input.decision = laneHarness.tokens[1];
    harken::Plan03GroupSliceIdentity second{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &second));
    const std::array laneGroups{first, second};
    laneHarness.Bind(pool, laneGroups.data(), 2);
    const harken::FrontierOwnerIdentity owner{1, 1, 0, 0, 1, 1, 0, 1};
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Future;
    binding.entry.coordinator = first.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = owner;
    binding.operation.kind = harken::FrontierOperationKind::Dependent;
    binding.operation.owner = owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.operation.group = first;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(first, binding));
    binding.operation.group = second;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(second, binding));
    laneHarness.Publish(pool, arms, laneGroups.data(), 2);
    harken::Plan03PageGroupView firstView{}, secondView{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(first, &firstView));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(second, &secondView));
    ASSERT_NE(firstView.lease, secondView.lease);

    ASSERT_TRUE(pool.InvalidateFrontierOwnerOperations(
        owner, first.decision, harken::AudioSliceInvalidateReason::Deadline));
    pool.SettleInvalidatedImmediateLaneFrontiers();
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated,
              firstView.lease->Capture().state);
    EXPECT_EQ(harken::AudioSliceReasonDeadline,
              firstView.lease->Capture().pendingReasonBits);
    EXPECT_EQ(harken::AudioSliceState::Ready, secondView.lease->Capture().state);
    harken::ImmediateLaneFrontierClaim claim{};
    EXPECT_FALSE(pool.ClaimImmediateLaneFrontier(first, &claim));
    ASSERT_TRUE(pool.ClaimImmediateLaneFrontier(second, &claim));
    EXPECT_FALSE(pool.InvalidateFrontierOwnerOperations(
        owner, {}, harken::AudioSliceInvalidateReason::WorldReplacement));
    pool.SettleInvalidatedImmediateLaneFrontiers();
    EXPECT_EQ(harken::AudioSliceState::Ready, secondView.lease->Capture().state);
    ASSERT_TRUE(pool.ReleaseImmediateLaneFrontier(&claim));
    EXPECT_FALSE(claim.Valid());
    pool.SettleInvalidatedImmediateLaneFrontiers();
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated,
              secondView.lease->Capture().state);
    EXPECT_EQ(harken::AudioSliceReasonWorldReplacement,
              secondView.lease->Capture().pendingReasonBits);
    EXPECT_FALSE(pool.ClaimImmediateLaneFrontier(second, &claim));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(first, 0));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(second, 0));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0));
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
}

struct PrivateLaneTransferHarness {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{2};
    harken::DecisionTable decisions{3, 2};
    std::array<harken::DecisionToken, 3> decisionsByGroup{};
    std::array<harken::TimelineArmToken, 2> members{};
    std::array<harken::Plan03GroupSliceInput, 3> inputs{};
    std::array<harken::Plan03GroupSliceIdentity, 3> raw{};
    std::array<harken::PreparedPrivateImmediateLaneGroup, 3> originals{};
    std::array<harken::PrivateImmediateLaneGroupTransferProof, 3> proofs{};

    void Prepare(std::uint32_t rows = 3) {
        auto config = MakePoolConfig();
        config.maxImmediateLaneGroupRows = rows;
        config.maxImmediateLaneOwnerGates = 4;
        config.maxImmediateLaneDecisionGates = 4;
        ASSERT_TRUE(pool.Reset(config, 0, 9));
        for (std::uint32_t index = 0; index < decisionsByGroup.size(); ++index)
            ASSERT_TRUE(decisions.Reserve(MakeRequest(index + 1), &decisionsByGroup[index]));
        for (std::uint32_t index = 0; index < members.size(); ++index)
            ASSERT_TRUE(arms.PreparePendingImmediateGrouped(index + 1, 1, 7, {},
                harken::ParentAudioGroupRef{decisionsByGroup[index + 1]}, 0, &members[index]));
        for (std::uint32_t index = 0; index < raw.size(); ++index) {
            auto& input = inputs[index];
            input.executeFrame = 120;
            input.decision = decisionsByGroup[index];
            input.members = index == 0 ? nullptr : &members[index - 1];
            input.memberCount = index == 0 ? 0 : 1;
            input.sourceControlCount = input.memberCount;
            ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms,
                &raw[index], &originals[index]));
        }
    }

    bool Prove(std::uint32_t index) {
        return pool.ProvePrivateImmediateLaneGroupTransfer(originals[index], 17,
            100, inputs[index], arms, &proofs[index]);
    }

    void ReclaimOtherParticipants() {
        ASSERT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
        for (const auto& member : members) ASSERT_TRUE(arms.CancelPrepared(member));
        for (const auto& token : decisionsByGroup) {
            ASSERT_TRUE(decisions.Cancel(token));
            ASSERT_TRUE(decisions.ReclaimCancelled(token));
        }
        EXPECT_EQ(0u, arms.LiveCount());
        EXPECT_EQ(0u, decisions.LiveCount());
        EXPECT_TRUE(pool.ResetForEpoch(0, 10));
    }
};

TEST(DecisionGroup, PrivateLaneTransferProofHoldsActualRowsReleaseReproveAndConsumeOnce) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    const auto identities = state.raw;
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 3, 2, 100, false, 3}));
    harken::ImmediateLaneHandle laneHandle{};
    ASSERT_TRUE(lane.Reserve(17, {0, 3, 0, 1}, &laneHandle));
    for (std::uint32_t index = 0; index < state.raw.size(); ++index) {
        ASSERT_TRUE(state.Prove(index));
        EXPECT_TRUE(state.originals[index].Held());
        EXPECT_TRUE(state.proofs[index].MatchesOriginalScope(state.originals[index],
            17, 100, state.inputs[index], state.arms));
        EXPECT_TRUE(state.pool.PrivateImmediateLaneGroupTransferHeld(state.raw[index]));
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[index]));
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
        EXPECT_EQ(identities[index], state.raw[index]);
        harken::Plan03GroupReaderPin reader;
        EXPECT_FALSE(state.pool.AcquirePlan03GroupReader(state.raw[index], &reader));
        EXPECT_FALSE(reader.Valid());
        EXPECT_FALSE(state.pool.NotePlan03GroupReadRt(state.raw[index], 1));
        EXPECT_FALSE(state.pool.PrivatePlan03GroupBindable(state.raw[index]));
        EXPECT_FALSE(state.pool.PrivateImmediateLaneGroupRetirementReady(state.raw[index], 0));
        EXPECT_FALSE(state.pool.DetachPlan03GroupFrontier(state.raw[index], 0));
        EXPECT_FALSE(state.pool.AcquireImmediateLaneFutureDependent(state.raw[index]));
        EXPECT_FALSE(state.pool.ReleaseImmediateLaneFutureDependent(state.raw[index]));
        EXPECT_FALSE(state.pool.BindImmediateLaneGroupOwner(state.raw[index], lane,
            17, laneHandle.slot, laneHandle.generation));
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Coordinator;
        binding.entry.coordinator = state.raw[index].decision;
        binding.entry.attempt = 1;
        binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
        binding.operation.kind = harken::FrontierOperationKind::Entry;
        binding.operation.owner = binding.entry.owner;
        binding.operation.order.logicCommitSequence = 1;
        binding.operation.order.executeFrame = 120;
        binding.operation.group = state.raw[index];
        binding.operation.transport = 9;
        binding.operation.world = 7;
        binding.primary = state.raw[index];
        ASSERT_TRUE(binding.Valid());
        EXPECT_FALSE(state.pool.BindPlan03GroupFrontier(state.raw[index], binding));
    }
    EXPECT_FALSE(state.pool.CancelImmediateLaneGroups(17, 0));
    EXPECT_FALSE(state.pool.RebaseImmediateLaneGroups(17, 400));
    EXPECT_FALSE(state.pool.PublishImmediateLaneGroups(17));
    EXPECT_FALSE(state.pool.ResetForEpoch(0, 10));
    auto config = MakePoolConfig();
    EXPECT_FALSE(state.pool.Reset(config, 0, 10));
    std::array<harken::Plan03ImmediateLaneGroupDescriptor, 3> actual{};
    ASSERT_EQ(3u, state.pool.CaptureImmediateLaneCohortGroups(17, actual.data(), actual.size()));
    for (std::uint32_t index = 0; index < actual.size(); ++index) {
        EXPECT_EQ(identities[index], actual[index].identity);
        EXPECT_EQ(UINT64_MAX, actual[index].dueQuantum);
        EXPECT_EQ(UINT32_MAX, actual[index].laneSlot);
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(identities[index], &view));
        EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, view.lease->Capture().state);
        EXPECT_EQ(state.inputs[index].memberCount, view.memberCount);
        EXPECT_EQ(state.inputs[index].sourceControlCount, view.sourceControlCount);
        if (index != 0) {
            EXPECT_EQ(state.members[index - 1].slot, view.members[0].slot);
            EXPECT_EQ(state.members[index - 1].generation, view.members[0].generation);
        }
    }
    state.proofs[1].Release();
    EXPECT_FALSE(state.originals[1].Held());
    EXPECT_TRUE(state.originals[1].Valid());
    EXPECT_FALSE(state.proofs[1].MatchesOriginalScope(state.originals[1], 17,
        100, state.inputs[1], state.arms));
    ASSERT_TRUE(state.Prove(1));
    for (std::uint32_t index = 0; index < state.raw.size(); ++index) {
        const auto consumed = harken::ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(
            std::move(state.proofs[index]));
        EXPECT_EQ(identities[index], consumed);
        EXPECT_FALSE(state.proofs[index].Valid());
        EXPECT_FALSE(state.originals[index].Valid());
        EXPECT_FALSE(state.Prove(index));
        EXPECT_FALSE(state.pool.PrivateImmediateLaneGroupTransferHeld(consumed));
        EXPECT_TRUE(state.pool.PrivatePlan03GroupBindable(consumed));
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(consumed, &view));
        EXPECT_FALSE(state.pool.ResolvePlan03GroupSliceRt(consumed, &view));
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, consumed));
        state.raw[index] = {};
    }
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofRejectsForeignFactoryScopesAndDuplicateDestinations) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(4));
    harken::PrivateImmediateLaneGroupTransferProof refused;
    harken::PreparedPrivateImmediateLaneGroup empty;
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(empty, 17, 100,
        state.inputs[1], state.arms, &refused));
    harken::ScheduledTransactionPool foreign;
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 4;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(foreign.Reset(config, 0, 9));
    EXPECT_FALSE(foreign.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
        17, 100, state.inputs[1], state.arms, &refused));
    harken::TimelineArmTable foreignArms(2);
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
        17, 100, state.inputs[1], foreignArms, &refused));
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
        18, 100, state.inputs[1], state.arms, &refused));
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
        17, 101, state.inputs[1], state.arms, &refused));
    for (std::uint32_t mode = 0; mode < 10; ++mode) {
        auto wrong = state.inputs[1];
        auto wrongMember = state.members[0];
        switch (mode) {
        case 0: ++wrong.executeFrame; break;
        case 1: ++wrong.decision.generation; break;
        case 2: ++wrong.sliceIndex; break;
        case 3: wrong.memberCount = 0; break;
        case 4: wrong.sourceControlCount = 0; break;
        case 5: wrong.members = &state.members[1]; break;
        case 6: wrong.members = nullptr; break;
        case 7: ++wrong.immediateEntry.generation; break;
        case 8: ++wrongMember.renderSlot; wrong.members = &wrongMember; break;
        case 9: ++wrongMember.renderGeneration; wrong.members = &wrongMember; break;
        }
        EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
            17, 100, wrong, state.arms, &refused));
        EXPECT_FALSE(refused.Valid());
        EXPECT_FALSE(state.originals[1].Held());
        refused.Release();
    }
    harken::Plan03GroupSliceIdentity unchanged{};
    auto newInput = state.inputs[0];
    ++newInput.sliceIndex;
    EXPECT_FALSE(state.pool.ReserveImmediateLaneGroup(17, 100, newInput, state.arms,
        &unchanged, &state.originals[0]));
    EXPECT_FALSE(unchanged.Valid());
    EXPECT_EQ(3u, state.pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    ASSERT_TRUE(state.Prove(1));
    for (std::uint32_t field = 0; field < 2; ++field) {
        auto wrong = state.inputs[1];
        auto member = state.members[0];
        if (field == 0) ++member.renderSlot;
        else ++member.renderGeneration;
        wrong.members = &member;
        EXPECT_FALSE(state.proofs[1].MatchesOriginalScope(state.originals[1],
            17, 100, wrong, state.arms));
        EXPECT_TRUE(state.proofs[1].Valid());
    }
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
        17, 100, state.inputs[1], state.arms, &refused));
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[0],
        17, 100, state.inputs[0], state.arms, &state.proofs[1]));
    EXPECT_TRUE(state.proofs[1].Valid());
    EXPECT_FALSE(state.originals[0].Held());
    state.proofs[1].Release();
    for (std::uint32_t index = 0; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofLaterOwnedCancelRefusalReleasesPrefixAndRetainsNeighbor) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(4));
    harken::Plan03GroupSliceIdentity neighbor{};
    harken::PreparedPrivateImmediateLaneGroup neighborOriginal;
    ASSERT_TRUE(state.pool.ReserveImmediateLaneGroup(18, 100, state.inputs[0], state.arms,
        &neighbor, &neighborOriginal));
    const auto cancelled = state.raw[1];
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[1], &state.raw[1]));
    EXPECT_FALSE(state.originals[1].Valid());
    EXPECT_FALSE(state.raw[1].Valid());
    harken::Plan03PageGroupView view{};
    EXPECT_FALSE(state.pool.ResolvePlan03GroupSliceProducer(cancelled, &view));
    ASSERT_TRUE(state.Prove(0));
    EXPECT_TRUE(state.originals[0].Held());
    EXPECT_FALSE(state.Prove(1));
    EXPECT_TRUE(state.originals[0].Held());
    EXPECT_FALSE(state.proofs[1].Valid());
    EXPECT_EQ(2u, state.pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    state.proofs[0].Release();
    EXPECT_FALSE(state.originals[0].Held());
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[0], &state.raw[0]));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[2], &state.raw[2]));
    EXPECT_EQ(1u, state.pool.CaptureImmediateLaneCohortGroups(18, nullptr, 0));
    ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(neighbor, &view));
    EXPECT_TRUE(neighborOriginal.Valid());
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&neighborOriginal, &neighbor));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofMovesCannotOverwriteEitherHeldEndpoint) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    const auto first = state.raw[0];
    const auto second = state.raw[1];
    ASSERT_TRUE(state.Prove(0));
    harken::PreparedPrivateImmediateLaneGroup empty;
    empty = std::move(state.originals[0]);
    EXPECT_FALSE(empty.Valid());
    EXPECT_TRUE(state.originals[0].Held());
    state.originals[1] = std::move(state.originals[0]);
    EXPECT_EQ(second, state.originals[1].Identity());
    state.originals[0] = std::move(state.originals[1]);
    EXPECT_EQ(first, state.originals[0].Identity());
    EXPECT_EQ(second, state.originals[1].Identity());
    harken::PrivateImmediateLaneGroupTransferProof movedProof(std::move(state.proofs[0]));
    EXPECT_FALSE(movedProof.Valid());
    EXPECT_TRUE(state.proofs[0].Valid());
    state.proofs[0] = std::move(movedProof);
    EXPECT_TRUE(state.proofs[0].Valid());
    state.proofs[0].Release();
    empty = std::move(state.originals[0]);
    EXPECT_FALSE(state.originals[0].Valid());
    ASSERT_TRUE(empty.Valid());
    {
        harken::PrivateImmediateLaneGroupTransferProof temporary;
        ASSERT_TRUE(state.pool.ProvePrivateImmediateLaneGroupTransfer(empty, 17,
            100, state.inputs[0], state.arms, &temporary));
        EXPECT_TRUE(empty.Held());
    }
    EXPECT_FALSE(empty.Held());
    EXPECT_TRUE(empty.Valid());
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&empty, &state.raw[0]));
    for (std::uint32_t index = 1; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofOriginalDestructionReleasesHoldWithoutCancellingRow) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    {
        auto original = std::move(state.originals[0]);
        ASSERT_TRUE(original.Valid());
        ASSERT_TRUE(state.pool.ProvePrivateImmediateLaneGroupTransfer(original, 17,
            100, state.inputs[0], state.arms, &state.proofs[0]));
        EXPECT_TRUE(state.proofs[0].Valid());
    }
    EXPECT_FALSE(state.proofs[0].Valid());
    EXPECT_FALSE(state.pool.PrivateImmediateLaneGroupTransferHeld(state.raw[0]));
    harken::Plan03PageGroupView actual{};
    ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(state.raw[0], &actual));
    EXPECT_EQ(harken::AudioSliceState::Ready, actual.lease->Capture().state);
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0]));
    state.raw[0] = {};
    for (std::uint32_t index = 1; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofRawCancellationAndReuseCannotRecreateOriginalGrant) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    const auto stale = state.raw[0];
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, stale));
    EXPECT_FALSE(state.originals[0].Valid());
    EXPECT_FALSE(state.Prove(0));
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[0], &state.raw[0]));
    EXPECT_EQ(stale, state.raw[0]);
    harken::Plan03GroupSliceIdentity reused{};
    harken::PreparedPrivateImmediateLaneGroup reusedOriginal;
    ASSERT_TRUE(state.pool.ReserveImmediateLaneGroup(17, 100, state.inputs[0], state.arms,
        &reused, &reusedOriginal));
    EXPECT_EQ(stale.committedRow, reused.committedRow);
    EXPECT_NE(stale.rowClaimGeneration, reused.rowClaimGeneration);
    EXPECT_FALSE(state.Prove(0));
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, stale));
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(state.pool.ProvePrivateImmediateLaneGroupTransfer(reusedOriginal,
        17, 100, state.inputs[0], state.arms, &proof));
    auto wrongEpoch = reused;
    ++wrongEpoch.transportEpoch;
    EXPECT_FALSE(state.pool.PrivateImmediateLaneGroupTransferHeld(wrongEpoch));
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, wrongEpoch));
    proof.Release();
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&reusedOriginal, &reused));
    state.raw[0] = {};
    for (std::uint32_t index = 1; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofEarlierBorrowCannotMutateHeldRealLease) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    harken::Plan03PageGroupView before{};
    ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(state.raw[0], &before));
    auto* borrowed = before.lease;
    ASSERT_NE(nullptr, borrowed);
    ASSERT_TRUE(state.Prove(0));
    harken::Plan03PageGroupView after{};
    ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(state.raw[0], &after));
    ASSERT_EQ(borrowed, after.lease);
    const auto held = borrowed->Capture();
    EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, held.state);
    for (const auto reason : {harken::AudioSliceInvalidateReason::Cancel,
            harken::AudioSliceInvalidateReason::Supersede,
            harken::AudioSliceInvalidateReason::Deadline,
            harken::AudioSliceInvalidateReason::Identity,
            harken::AudioSliceInvalidateReason::WorldReplacement,
            harken::AudioSliceInvalidateReason::TransportReset}) {
        EXPECT_EQ(harken::AudioSliceInvalidateDisposition::PrivateTransferHeld,
            borrowed->Invalidate(reason));
        EXPECT_EQ(harken::AudioSliceInvalidateDisposition::PrivateTransferHeld,
            after.lease->Invalidate(reason));
    }
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidReason,
        borrowed->Invalidate(static_cast<harken::AudioSliceInvalidateReason>(UINT8_MAX)));
    EXPECT_FALSE(borrowed->TryClaim());
    EXPECT_FALSE(borrowed->MarkApplied());
    EXPECT_FALSE(borrowed->AbortClaimedBeforeApply(harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_FALSE(borrowed->PrepareReplacement());
    EXPECT_FALSE(borrowed->CommitPreparedReplacement());
    EXPECT_FALSE(borrowed->CancelPreparedReplacement());
    EXPECT_FALSE(borrowed->ResetForReuse());
    auto outputReason = harken::AudioSliceInvalidateReason::TransportReset;
    EXPECT_FALSE(borrowed->ConsumePostSliceInvalidation(&outputReason));
    EXPECT_EQ(harken::AudioSliceInvalidateReason::TransportReset, outputReason);
    const auto unchanged = after.lease->Capture();
    EXPECT_EQ(held.state, unchanged.state);
    EXPECT_EQ(0u, unchanged.pendingReasonBits);
    EXPECT_FALSE(unchanged.invalidateAfterSlice);
    EXPECT_TRUE(state.proofs[0].Valid());
    state.proofs[0].Release();
    EXPECT_EQ(harken::AudioSliceState::Ready, borrowed->Capture().state);
    ASSERT_TRUE(state.Prove(0));
    EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, borrowed->Capture().state);
    state.proofs[0].Release();
    EXPECT_EQ(harken::AudioSliceState::Ready, borrowed->Capture().state);
    for (std::uint32_t index = 0; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofConsumeLeavesRealPrivateGroupForNormalLanePublication) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(1));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity raw{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &raw, &original));
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    const auto consumed = harken::ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(std::move(proof));
    EXPECT_EQ(raw, consumed);
    ASSERT_TRUE(pool.PrivatePlan03GroupBindable(consumed));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Bind(pool, &consumed, 1));
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = consumed.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = consumed;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = consumed;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(consumed, binding));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Publish(pool, arms, &consumed, 1));
    harken::Plan03PageGroupView view{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(consumed, &view));
    EXPECT_EQ(consumed, view.identity);
    std::array<harken::Plan03PageGroupView, 1> due{};
    EXPECT_EQ(1u, pool.CaptureImmediateLaneGroupsRt(4, due.data(), due.size()));
    EXPECT_FALSE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
        view.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(consumed, 2));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2));
    ASSERT_TRUE(laneHarness.decisions.Cancel(laneHarness.tokens[0]));
    ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(laneHarness.tokens[0]));
    EXPECT_EQ(0u, laneHarness.decisions.LiveCount());
    EXPECT_TRUE(pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, PrivateLaneTransferProofOriginalMemberIndexSurvivesRealCancelReprepare) {
    for (const bool initiallyHeld : {false, true}) {
        PrivateLaneTransferHarness state;
        ASSERT_NO_FATAL_FAILURE(state.Prepare());
        const auto originalMember = state.members[0];
        if (initiallyHeld) ASSERT_TRUE(state.Prove(1));
        ASSERT_TRUE(state.arms.CancelPrepared(originalMember));
        harken::TimelineArmToken changed{};
        ASSERT_TRUE(state.arms.PreparePendingImmediateGrouped(originalMember.renderSlot,
            originalMember.renderGeneration, 7, {},
            harken::ParentAudioGroupRef{state.decisionsByGroup[1]}, 7, &changed));
        ASSERT_EQ(originalMember.slot, changed.slot);
        ASSERT_NE(originalMember.generation, changed.generation);
        ASSERT_EQ(originalMember.renderSlot, changed.renderSlot);
        ASSERT_EQ(originalMember.renderGeneration, changed.renderGeneration);
        harken::TimelineArmObservation actual{};
        ASSERT_TRUE(state.arms.Observe(changed, &actual));
        ASSERT_EQ(7u, actual.memberIndex);
        ASSERT_EQ(nullptr, state.arms.Resolve(originalMember));
        if (initiallyHeld) {
            EXPECT_FALSE(state.proofs[1].MatchesOriginalScope(state.originals[1],
                17, 100, state.inputs[1], state.arms));
            EXPECT_TRUE(state.proofs[1].Valid());
            EXPECT_TRUE(state.originals[1].Held());
            state.proofs[1].Release();
        }
        EXPECT_FALSE(state.Prove(1));
        state.proofs[1].Release();
        EXPECT_TRUE(state.originals[1].Valid());
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(state.raw[1], &view));
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
        ASSERT_TRUE(state.arms.CancelPrepared(changed));
        harken::TimelineArmToken restored{};
        ASSERT_TRUE(state.arms.PreparePendingImmediateGrouped(originalMember.renderSlot,
            originalMember.renderGeneration, 7, {},
            harken::ParentAudioGroupRef{state.decisionsByGroup[1]}, 0, &restored));
        ASSERT_EQ(originalMember.slot, restored.slot);
        ASSERT_NE(originalMember.generation, restored.generation);
        ASSERT_EQ(originalMember.renderSlot, restored.renderSlot);
        ASSERT_EQ(originalMember.renderGeneration, restored.renderGeneration);
        EXPECT_FALSE(state.Prove(1));
        state.proofs[1].Release();
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[1], &state.raw[1]));
        state.members[0] = restored;
        ASSERT_TRUE(state.pool.ReserveImmediateLaneGroup(17, 100, state.inputs[1], state.arms,
            &state.raw[1], &state.originals[1]));
        ASSERT_TRUE(state.Prove(1));
        EXPECT_TRUE(state.proofs[1].MatchesOriginalScope(state.originals[1],
            17, 100, state.inputs[1], state.arms));
        state.proofs[1].Release();
        for (std::uint32_t index = 0; index < state.raw.size(); ++index)
            ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
        ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
    }
}

TEST(DecisionGroup, PrivateLaneTransferProofCurrentRealRenderMustMatchFactoryOriginal) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(4));
    const auto originalMember = state.members[0];
    ASSERT_TRUE(state.arms.CancelPrepared(originalMember));
    harken::TimelineArmToken changed{};
    ASSERT_TRUE(state.arms.PreparePendingImmediateGrouped(originalMember.renderSlot + 20,
        originalMember.renderGeneration + 10, 7, {},
        harken::ParentAudioGroupRef{state.decisionsByGroup[1]}, 0, &changed));
    ASSERT_EQ(originalMember.slot, changed.slot);
    ASSERT_NE(originalMember.generation, changed.generation);
    harken::TimelineArmObservation actual{};
    EXPECT_FALSE(state.arms.Observe(originalMember, &actual));
    ASSERT_TRUE(state.arms.Observe(changed, &actual));
    ASSERT_EQ(nullptr, state.arms.Resolve(originalMember));
    EXPECT_FALSE(state.Prove(1));
    state.proofs[1].Release();
    auto currentInput = state.inputs[1];
    currentInput.members = &changed;
    EXPECT_FALSE(state.pool.ProvePrivateImmediateLaneGroupTransfer(state.originals[1],
        17, 100, currentInput, state.arms, &state.proofs[1]));
    EXPECT_TRUE(state.originals[1].Valid());
    auto staleFactoryInput = state.inputs[1];
    ++staleFactoryInput.sliceIndex;
    harken::Plan03GroupSliceIdentity refusedRaw{};
    harken::PreparedPrivateImmediateLaneGroup refusedOriginal;
    EXPECT_FALSE(state.pool.ReserveImmediateLaneGroup(17, 100, staleFactoryInput,
        state.arms, &refusedRaw, &refusedOriginal));
    if (refusedOriginal.Valid())
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&refusedOriginal, &refusedRaw));
    EXPECT_FALSE(refusedRaw.Valid());
    ASSERT_TRUE(state.arms.CancelPrepared(changed));
    harken::TimelineArmToken restored{};
    ASSERT_TRUE(state.arms.PreparePendingImmediateGrouped(originalMember.renderSlot,
        originalMember.renderGeneration, 7, {},
        harken::ParentAudioGroupRef{state.decisionsByGroup[1]}, 0, &restored));
    EXPECT_FALSE(state.Prove(1));
    state.proofs[1].Release();
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[1], &state.raw[1]));
    state.members[0] = restored;
    ASSERT_TRUE(state.pool.ReserveImmediateLaneGroup(17, 100, state.inputs[1], state.arms,
        &state.raw[1], &state.originals[1]));
    ASSERT_TRUE(state.Prove(1));
    EXPECT_TRUE(state.proofs[1].MatchesOriginalScope(state.originals[1], 17, 100,
        state.inputs[1], state.arms));
    state.proofs[1].Release();
    for (std::uint32_t index = 0; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofFactoryMemberGeometryAllowsReorderedInputSet) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(2);
    harken::DecisionTable decisions(1, 2);
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(), &decision));
    std::array<harken::TimelineArmToken, 2> members{};
    ASSERT_TRUE(arms.PreparePendingImmediateGrouped(1, 1, 7, {},
        harken::ParentAudioGroupRef{decision}, 7, &members[0]));
    ASSERT_TRUE(arms.PreparePendingImmediateGrouped(2, 1, 7, {},
        harken::ParentAudioGroupRef{decision}, 3, &members[1]));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = decision;
    input.members = members.data();
    input.memberCount = 2;
    input.sourceControlCount = 2;
    harken::Plan03GroupSliceIdentity raw{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &raw, &original));
    harken::Plan03PageGroupView view{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(raw, &view));
    EXPECT_EQ(members[1].slot, view.members[0].slot);
    EXPECT_EQ(members[0].slot, view.members[1].slot);
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    const std::array reversed{members[1], members[0]};
    auto reordered = input;
    reordered.members = reversed.data();
    EXPECT_TRUE(proof.MatchesOriginalScope(original, 17, 100, reordered, arms));
    proof.Release();
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, reordered, arms, &proof));
    const auto consumed = harken::ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(std::move(proof));
    EXPECT_EQ(raw, consumed);
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, consumed));
    for (const auto& member : members) ASSERT_TRUE(arms.CancelPrepared(member));
    ASSERT_TRUE(decisions.Cancel(decision));
    ASSERT_TRUE(decisions.ReclaimCancelled(decision));
    EXPECT_EQ(0u, arms.LiveCount());
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_TRUE(pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, PrivateLaneTransferProofRealReaderRefusalRetainsAcquiredPrefixUntilRetry) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    ASSERT_TRUE(state.Prove(0));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(state.pool.AcquirePlan03GroupReader(state.raw[1], &reader));
    EXPECT_FALSE(state.Prove(1));
    EXPECT_TRUE(state.originals[0].Held());
    EXPECT_TRUE(state.proofs[0].Valid());
    EXPECT_TRUE(state.originals[1].Valid());
    EXPECT_FALSE(state.originals[1].Held());
    EXPECT_TRUE(reader.Valid());
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[1], &state.raw[1]));
    ASSERT_TRUE(reader.Release(0));
    ASSERT_TRUE(state.Prove(1));
    EXPECT_TRUE(state.originals[0].Held());
    EXPECT_TRUE(state.originals[1].Held());
    state.proofs[0].Release();
    state.proofs[1].Release();
    for (std::uint32_t index = 0; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[index], &state.raw[index]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateLaneTransferProofConsumedSideDoesNotReleaseHeldCoordinatorOrOtherSide) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    for (std::uint32_t index = 0; index < state.raw.size(); ++index)
        ASSERT_TRUE(state.Prove(index));
    const auto consumed = harken::ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(
        std::move(state.proofs[1]));
    EXPECT_EQ(state.raw[1], consumed);
    EXPECT_FALSE(state.originals[1].Valid());
    EXPECT_TRUE(state.originals[0].Held());
    EXPECT_TRUE(state.originals[2].Held());
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, consumed));
    state.raw[1] = {};
    EXPECT_FALSE(state.pool.CancelImmediateLaneGroups(17, 0));
    EXPECT_EQ(2u, state.pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    EXPECT_TRUE(state.proofs[0].Valid());
    EXPECT_TRUE(state.proofs[2].Valid());
    EXPECT_TRUE(state.proofs[0].MatchesOriginalScope(state.originals[0],
        17, 100, state.inputs[0], state.arms));
    EXPECT_TRUE(state.proofs[2].MatchesOriginalScope(state.originals[2],
        17, 100, state.inputs[2], state.arms));
    state.proofs[0].Release();
    state.proofs[2].Release();
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[0], &state.raw[0]));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[2], &state.raw[2]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, PrivateImmediateLaneExactCancelRetainsReaderAndRetriesRemainingIdentity) {
    harken::ScheduledTransactionPool pool;
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 3;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::TimelineArmTable arms(2);
    harken::DecisionTable decisions(3, 2);
    std::array<harken::DecisionToken, 3> tokens{};
    for (std::uint32_t index = 0; index < tokens.size(); ++index)
        ASSERT_TRUE(decisions.Reserve(MakeRequest(index + 1), &tokens[index]));
    std::array<harken::TimelineArmToken, 2> members{};
    for (std::uint32_t index = 0; index < members.size(); ++index)
        ASSERT_TRUE(arms.PreparePendingImmediateGrouped(index + 1, 1, 7, {},
            harken::ParentAudioGroupRef{tokens[index + 1]}, 0, &members[index]));
    std::array<harken::Plan03GroupSliceIdentity, 3> held{};
    for (std::uint32_t index = 0; index < held.size(); ++index) {
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = 120;
        input.decision = tokens[index];
        input.members = index == 0 ? nullptr : &members[index - 1];
        input.memberCount = index == 0 ? 0 : 1;
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &held[index]));
    }
    const auto first = held[0];
    auto foreign = held[0];
    ++foreign.transportEpoch;
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, foreign));
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(18, held[0]));
    ASSERT_EQ(3u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(held[2], &reader));
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, held[0]));
    held[0] = {};
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, held[2]));
    ASSERT_TRUE(reader.Valid());
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, held[1]));
    held[1] = {};
    EXPECT_EQ(1u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    harken::Plan03PageGroupView remaining{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(held[2], &remaining));
    EXPECT_EQ(held[2], remaining.identity);
    ASSERT_TRUE(reader.Release(0));
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, held[2]));
    held[2] = {};
    EXPECT_EQ(0u, pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    harken::Plan03GroupSliceInput retry{};
    retry.executeFrame = 120;
    retry.decision = tokens[0];
    harken::Plan03GroupSliceIdentity reused{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, retry, arms, &reused));
    ASSERT_EQ(first.committedRow, reused.committedRow);
    EXPECT_NE(first.rowClaimGeneration, reused.rowClaimGeneration);
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, first));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(reused, &remaining));
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, reused));
    for (const auto& member : members) ASSERT_TRUE(arms.CancelPrepared(member));
    for (const auto& token : tokens) {
        ASSERT_TRUE(decisions.Cancel(token));
        ASSERT_TRUE(decisions.ReclaimCancelled(token));
    }
    EXPECT_EQ(0u, arms.LiveCount());
    EXPECT_EQ(0u, decisions.LiveCount());
    EXPECT_TRUE(pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, ImmediateLaneGroupReaderFencePreventsEarlyReuse) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    laneHarness.ReserveDecisions(1);
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity first{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &first));
    laneHarness.Bind(pool, &first, 1);
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Future;
    binding.entry.coordinator = first.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Dependent;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = first;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(first, binding));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(first, &reader));
    laneHarness.Publish(pool, arms, &first, 1);
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(first, 1));
    harken::Plan03PageGroupView view{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(first, &view));
    ASSERT_NE(nullptr, view.lease);
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
              view.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(first, 2));
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 2));
    EXPECT_FALSE(reader.Release(1));
    EXPECT_FALSE(pool.Reset(config, 0, 10));
    EXPECT_FALSE(pool.ResetForEpoch(0, 10));
    ASSERT_TRUE(reader.Release(2));
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 1));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(first, &view));
    harken::Plan03GroupSliceIdentity second{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(18, 100, input, arms, &second));
    EXPECT_NE(first.rowClaimGeneration, second.rowClaimGeneration);
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(first, &view));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(18, 0));
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(second, &view));
}

TEST(DecisionGroup, ImmediateLaneGroupRejectsDuplicateMemberIndexWithoutUsingRow) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(3);
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 16;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    const harken::ParentAudioGroupRef parent{{13, 17}};
    std::array<harken::TimelineArmToken, 3> tokens{};
    ASSERT_TRUE(arms.ReserveGrouped(1, 1, 120, 200, 29, target,
                                    parent, 0, &tokens[0]));
    ASSERT_TRUE(arms.ReserveGrouped(2, 1, 120, 200, 29, target,
                                    parent, 0, &tokens[1]));
    ASSERT_TRUE(arms.ReserveGrouped(3, 1, 120, 200, 29, target,
                                    parent, 1, &tokens[2]));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = parent.decision;
    input.members = tokens.data();
    input.memberCount = 2;
    harken::Plan03GroupSliceIdentity identity{};
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &identity));
    EXPECT_FALSE(identity.Valid());
    tokens[1] = tokens[2];
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &identity));
    ASSERT_TRUE(identity.Exact());
}

TEST(DecisionGroup, FrontierReceiptUsesRealLeaseAndCompletedBlockBeforeReconciliation) {
    FrontierReceiptHarness h;
    h.Prepare();
    h.Publish();
    auto* activation = h.decisions.ResolveFrontierActivation(h.token);
    ASSERT_NE(nullptr, activation);
    harken::DecisionFuturePin dependency;
    ASSERT_TRUE(h.decisions.AcquireFuturePin(h.token, &dependency));
    harken::FrontierActivationReceipt receipt{};
    EXPECT_EQ(harken::FrontierReceiptRead::InFlight, activation->Read(h.descriptor.entry, 100, &receipt));
    ASSERT_TRUE(activation->TryBeginAudio(h.descriptor.entry));
    ASSERT_TRUE(h.pool.NotePlan03GroupReadRt(h.group.identity, 1));
    std::uint32_t applied = 0;
    harken::FrontierDecisionApplyProof proof;
    ASSERT_TRUE(h.decisions.ValidateFrontierDecisionApply(h.token, 1, &proof));
    SliceStep step{h.group.lease, &applied, true, false, &h.decisions, &proof};
    harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
        harken::ApplyAudioSliceWhole(*h.group.lease, &operation, 1));
    ASSERT_TRUE(activation->FinishAudio(h.descriptor.entry, harken::FrontierReceiptOutcome::Applied,
        harken::FrontierReceiptReason::None, 1));
    EXPECT_EQ(1u, applied);
    EXPECT_FALSE(activation->TryBeginAudio(h.descriptor.entry));
    EXPECT_EQ(harken::FrontierReceiptRead::InFlight, activation->Read(h.descriptor.entry, 0, &receipt));
    ASSERT_EQ(harken::FrontierReceiptRead::Ready, activation->Read(h.descriptor.entry, 1, &receipt));
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    EXPECT_EQ(h.descriptor.entry, receipt.entry);
    EXPECT_EQ(h.descriptor.order, receipt.order);
    EXPECT_EQ(harken::FrontierCancelDisposition::AppliedNeedsLifecycle,
        activation->Cancel(h.descriptor.entry, harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_FALSE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 1, &receipt));
    ASSERT_TRUE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 2, &receipt));
    h.pool.ConsumerFrontier().store(1, std::memory_order_release);
    h.pool.Advance();
    EXPECT_EQ(100u, h.pool.WindowStartFrame());
    ASSERT_EQ(harken::FrontierReceiptRead::Ready, activation->Read(h.descriptor.entry, 2, &receipt));
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, h.decisions.Invalidate(h.token));
    ASSERT_TRUE(h.decisions.Retire(h.token, 2));
    EXPECT_FALSE(h.decisions.ReclaimRetired(h.token));
    harken::DecisionFuturePin childDependency;
    EXPECT_FALSE(h.decisions.AcquireFuturePin(h.token, &childDependency));
    ASSERT_TRUE(h.decisions.CloneFuturePin(dependency, &childDependency));
    EXPECT_EQ(dependency.Token().slot, childDependency.Token().slot);
    EXPECT_EQ(dependency.Token().generation, childDependency.Token().generation);
    harken::DecisionCommittedSnapshot retired{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(h.token, &retired));
    EXPECT_EQ(harken::AudioGroupState::Retired, retired.audioState);
    EXPECT_EQ(harken::FrontierReceiptRead::Ready, activation->Read(h.descriptor.entry, 2, &receipt));
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    EXPECT_FALSE(h.decisions.CloneFuturePin(dependency, &childDependency));
    harken::DecisionTable other(1, 1);
    harken::DecisionFuturePin refused;
    EXPECT_FALSE(h.decisions.CloneFuturePin(dependency, nullptr));
    EXPECT_FALSE(h.decisions.CloneFuturePin(dependency, &dependency));
    ASSERT_TRUE(h.decisions.SetFuturePinCountForTests(h.token, 0));
    EXPECT_FALSE(h.decisions.CloneFuturePin(dependency, &refused));
    ASSERT_TRUE(h.decisions.SetFuturePinCountForTests(h.token, UINT32_MAX));
    EXPECT_FALSE(h.decisions.CloneFuturePin(dependency, &refused));
    ASSERT_TRUE(h.decisions.SetFuturePinCountForTests(h.token, 2));
    EXPECT_FALSE(other.CloneFuturePin(dependency, &refused));
    ASSERT_TRUE(dependency.Release());
    EXPECT_FALSE(h.decisions.CloneFuturePin(dependency, &refused));
    EXPECT_FALSE(h.decisions.ReclaimRetired(h.token));
    harken::DecisionFuturePin moved = std::move(childDependency);
    EXPECT_FALSE(h.decisions.CloneFuturePin(childDependency, &refused));
    ASSERT_TRUE(moved.Release());
    ASSERT_TRUE(h.decisions.ReclaimRetired(h.token));
    harken::DecisionToken reused{};
    ASSERT_TRUE(h.decisions.Reserve(MakeRequest(2), &reused));
    EXPECT_NE(h.token.generation, reused.generation);
    EXPECT_EQ(nullptr, h.decisions.ResolveFrontierActivation(h.token));
    EXPECT_EQ(nullptr, h.decisions.ResolveFrontierActivation(reused));
    harken::DecisionFuturePin withoutActivation;
    ASSERT_TRUE(h.decisions.AcquireFuturePin(reused, &withoutActivation));
    EXPECT_FALSE(h.decisions.CloneFuturePin(withoutActivation, &refused));
    ASSERT_TRUE(h.decisions.Cancel(reused));
    EXPECT_FALSE(h.decisions.CloneFuturePin(withoutActivation, &refused));
    ASSERT_TRUE(withoutActivation.Release());
    ASSERT_TRUE(h.decisions.ReclaimCancelled(reused));
}

TEST(DecisionGroup, FrontierLogicCancellationNeverInventsAudioCompletion) {
    FrontierReceiptHarness h;
    h.Prepare();
    h.Publish();
    auto* activation = h.decisions.ResolveFrontierActivation(h.token);
    ASSERT_NE(nullptr, activation);
    harken::FrontierActivationSidecarSnapshot sidecar{};
    ASSERT_TRUE(h.decisions.CaptureFrontierActivationSidecar(
        h.token, h.descriptor.entry, 0, &sidecar));
    EXPECT_EQ(h.group.identity, sidecar.primary);
    EXPECT_TRUE(sidecar.pagePinHeld);
    EXPECT_TRUE(sidecar.observerPinHeld);
    EXPECT_EQ(harken::FrontierReceiptRead::InFlight, sidecar.receiptRead);
    EXPECT_EQ(harken::FrontierCancelDisposition::CancelledBeforeAudio,
        activation->Cancel(h.descriptor.entry, harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_FALSE(activation->TryBeginAudio(h.descriptor.entry));
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, h.group.lease->Capture().state);
    harken::FrontierActivationReceipt receipt{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready, activation->Read(h.descriptor.entry, 0, &receipt));
    EXPECT_EQ(harken::FrontierReceiptOrigin::LogicBeforeAudio, receipt.origin);
    EXPECT_EQ(0u, receipt.audioSequence);
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, receipt.outcome);
    auto stale = h.descriptor.entry;
    ++stale.owner.serviceGeneration;
    EXPECT_EQ(harken::FrontierReceiptRead::Stale, activation->Read(stale, 0, &receipt));
    EXPECT_FALSE(h.decisions.CaptureFrontierActivationSidecar(h.token, stale, 0, &sidecar));
    auto staleToken = h.token;
    ++staleToken.generation;
    EXPECT_FALSE(h.decisions.CaptureFrontierActivationSidecar(
        staleToken, h.descriptor.entry, 0, &sidecar));
    ASSERT_TRUE(h.decisions.CaptureFrontierActivationSidecar(
        h.token, h.descriptor.entry, 0, &sidecar));
    EXPECT_EQ(harken::FrontierReceiptRead::Ready, sidecar.receiptRead);
    EXPECT_EQ(h.descriptor.entry, sidecar.receipt.entry);
    EXPECT_FALSE(h.decisions.ReconcileFrontierActivation(h.token, stale, 0, &receipt));
    ASSERT_TRUE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 0, &receipt));
    ASSERT_TRUE(h.decisions.CaptureFrontierActivationSidecar(
        h.token, h.descriptor.entry, 0, &sidecar));
    EXPECT_FALSE(sidecar.pagePinHeld);
    EXPECT_FALSE(sidecar.observerPinHeld);
    EXPECT_EQ(harken::FrontierReceiptRead::Ready, sidecar.receiptRead);
}

TEST(DecisionGroup, FrontierCancellationDuringStagingLeavesTheAudioWriterAndPin) {
    FrontierReceiptHarness h;
    h.Prepare();
    h.Publish();
    auto* activation = h.decisions.ResolveFrontierActivation(h.token);
    ASSERT_NE(nullptr, activation);
    ASSERT_TRUE(activation->TryBeginAudio(h.descriptor.entry));
    ASSERT_TRUE(h.pool.NotePlan03GroupReadRt(h.group.identity, 1));
    EXPECT_EQ(harken::FrontierCancelDisposition::DeferredToAudio,
        activation->Cancel(h.descriptor.entry, harken::AudioSliceInvalidateReason::Cancel));
    harken::FrontierActivationReceipt receipt{};
    EXPECT_EQ(harken::FrontierReceiptRead::InFlight, activation->Read(h.descriptor.entry, 100, &receipt));
    EXPECT_FALSE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 100, &receipt));
    EXPECT_FALSE(h.group.lease->TryClaim());
    ASSERT_TRUE(activation->FinishAudio(h.descriptor.entry,
        harken::FrontierReceiptOutcome::DefinitivelyNotApplied, harken::FrontierReceiptReason::Cancelled, 1));
    EXPECT_FALSE(activation->FinishAudio(h.descriptor.entry,
        harken::FrontierReceiptOutcome::DefinitivelyNotApplied, harken::FrontierReceiptReason::Cancelled, 1));
    EXPECT_FALSE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 1, &receipt));
    ASSERT_TRUE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 2, &receipt));
    EXPECT_EQ(harken::FrontierReceiptOrigin::Audio, receipt.origin);
}

TEST(DecisionGroup, FrontierPrivateCancellationReleasesRealPinsWithoutPublishing) {
    FrontierReceiptHarness h;
    h.Prepare();
    const auto bytes = h.decisions.FrontierStorageBytes();
    EXPECT_GT(bytes, 0u);
    harken::DecisionCommitPayload payload{};
    payload.pages = {1, 1};
    payload.armGroup = {1, 1};
    ASSERT_TRUE(h.decisions.Commit(h.token, payload));
    EXPECT_FALSE(h.decisions.Cancel(h.token));
    ASSERT_TRUE(h.decisions.CanRollbackPreparedFrontierDecision(h.token, h.token));
    ASSERT_TRUE(h.decisions.RollbackPreparedFrontierDecision(h.token, h.token));
    EXPECT_TRUE(h.decisions.IsOpen(h.token));
    ASSERT_TRUE(h.decisions.CancelPreparedFrontierActivation(h.token));
    ASSERT_TRUE(h.decisions.Cancel(h.token));
    ASSERT_TRUE(h.decisions.ReclaimCancelled(h.token));
    EXPECT_EQ(bytes, h.decisions.FrontierStorageBytes());
}

TEST(DecisionGroup, PreparedFrontierActivationRejectsForeignPoolWithSameIdentity) {
    FrontierReceiptHarness original, foreign;
    ASSERT_NO_FATAL_FAILURE(original.Prepare());
    ASSERT_NO_FATAL_FAILURE(foreign.Prepare());
    ASSERT_EQ(original.descriptor, foreign.descriptor);
    EXPECT_TRUE(original.decisions.MatchesPreparedFrontierActivationPool(original.descriptor.entry, original.pool));
    EXPECT_FALSE(original.decisions.MatchesPreparedFrontierActivationPool(original.descriptor.entry, foreign.pool));
    auto stale = original.descriptor.entry;
    ++stale.attempt;
    EXPECT_FALSE(original.decisions.MatchesPreparedFrontierActivationPool(stale, original.pool));
    ASSERT_TRUE(original.decisions.CancelPreparedFrontierActivation(original.token));
    EXPECT_FALSE(original.decisions.MatchesPreparedFrontierActivationPool(original.descriptor.entry, original.pool));
    ASSERT_TRUE(original.decisions.Cancel(original.token));
    ASSERT_TRUE(original.decisions.ReclaimCancelled(original.token));
    ASSERT_TRUE(foreign.decisions.CancelPreparedFrontierActivation(foreign.token));
    ASSERT_TRUE(foreign.decisions.Cancel(foreign.token));
    ASSERT_TRUE(foreign.decisions.ReclaimCancelled(foreign.token));
}

struct PrivatePendingArmRetirementHarness {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{2};
    harken::DecisionTable decisions{2, 2};
    std::array<harken::TimelineArmToken, 2> tokens{};
    std::array<harken::TimelineArmReaderPin, 2> pins{};
    std::array<harken::TimelineArmTable::PendingImmediateRetirementInput, 2> inputs{};
    std::array<harken::TimelineArmTable::Row*, 2> held{};
    harken::TimelineArmTable::UnpublishedPendingRetirementProof proof{};
    harken::AudioSliceLease* lease = nullptr;

    void Prepare(harken::TimelineArmTable::OriginalArmRowOwner* origin = nullptr,
        std::size_t originMember = 0, std::uint64_t armDeadline = 100) {
        auto config = MakePoolConfig();
        config.maxPlan03GroupHeadersPerPage = 2;
        ASSERT_TRUE(pool.Reset(config, 0, 9));
        harken::DecisionToken coordinator{}, member{};
        ASSERT_TRUE(decisions.Reserve(MakeRequest(1), &coordinator));
        ASSERT_TRUE(decisions.Reserve(MakeRequest(2), &member));
        harken::Plan03GroupSliceInput input{};
        input.decision = coordinator;
        input.executeFrame = 100;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(input, arms));
        harken::TimelineSemanticTarget target{};
        target.programSemanticFingerprint = 7;
        target.subjectId = harkentest::ObjectId(11);
        for (std::uint16_t index = 0; index < tokens.size(); ++index) {
            ASSERT_TRUE(arms.PrepareGrouped(index, 1, 100, armDeadline, 29, target,
                harken::ParentAudioGroupRef{member}, index, &tokens[index]));
            harken::TimelineArmTable::PreparedCommitInput expected{};
            expected.arm = tokens[index];
            expected.startFrame = 100;
            expected.armDeadline = armDeadline;
            expected.programGeneration = 29;
            expected.target = target;
            expected.parentGroup = {member};
            expected.memberIndex = index;
            harken::TimelineArmTable::PreparedCommitProof proof;
            ASSERT_TRUE(arms.ProvePreparedCommit(expected, &proof));
            if (origin != nullptr && index == originMember)
                ASSERT_TRUE(arms.IssueOriginalArmRowOwner(proof, origin));
            harken::TimelineArmTable::ConsumePreparedCommit(std::move(proof));
        }
        input.decision = member;
        input.members = tokens.data();
        input.memberCount = static_cast<std::uint16_t>(tokens.size());
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(input, arms));
        std::array<harken::Plan03PageGroupView, 2> groups{};
        ASSERT_EQ(2u, pool.CaptureLivePlan03Groups(groups.data(), groups.size()));
        const auto primary = groups[0].identity;
        const auto memberGroup = groups[1].identity;
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Coordinator;
        binding.entry.coordinator = coordinator;
        binding.entry.attempt = 1;
        binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
        binding.operation.kind = harken::FrontierOperationKind::Entry;
        binding.operation.owner = binding.entry.owner;
        binding.operation.order.logicCommitSequence = 1;
        binding.operation.group = primary;
        binding.operation.transport = 9;
        binding.operation.world = 7;
        binding.primary = primary;
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(primary, binding));
        binding.role = harken::FrontierGroupRole::EntryMember;
        binding.operation.group = memberGroup;
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(memberGroup, binding));
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(memberGroup, &groups[1]));
        ASSERT_NE(nullptr, groups[1].frontier);
        ASSERT_NE(nullptr, groups[1].lease);
        lease = groups[1].lease;
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(primary, &groups[0]));
        ASSERT_EQ(groups[0].lease, lease);
        for (std::uint16_t index = 0; index < tokens.size(); ++index) {
            ASSERT_TRUE(arms.PinFrontierReader(tokens[index], &pins[index]));
            ASSERT_TRUE(arms.BindFrontierAdmission(tokens[index], *groups[1].frontier, *lease));
            inputs[index] = {tokens[index], harken::ParentAudioGroupRef{member}, index,
                100, groups[1].frontier, lease, &pins[index]};
        }
    }
    bool Begin() {
        return arms.BeginUnpublishedPendingRetirement(
            inputs.data(), inputs.size(), held.data(), held.size(), &proof);
    }
    void CheckUntouched() {
        EXPECT_FALSE(proof.Valid());
        ASSERT_NE(nullptr, lease);
        EXPECT_EQ(harken::AudioSliceState::Ready, lease->Capture().state);
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            EXPECT_TRUE(pins[index].Valid());
            auto* row = arms.Resolve(tokens[index]);
            ASSERT_NE(nullptr, row);
            EXPECT_EQ(harken::ArmState::PendingApply, row->state.load());
            EXPECT_EQ(1u, row->frontierReaderPins.load());
            EXPECT_FALSE(row->reservation.test_and_set());
            row->reservation.clear();
        }
    }
};

TEST(DecisionGroup, OriginalRowOwnerBlocksUnpublishedRetirementBeforeAnySharedLeaseMutation) {
    for (std::size_t selected = 0; selected < 2; ++selected) {
        PrivatePendingArmRetirementHarness h;
        harken::TimelineArmTable::OriginalArmRowOwner owner;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(&owner, selected));
        ASSERT_TRUE(owner.Valid());
        EXPECT_FALSE(h.Begin());
        h.CheckUntouched();
        EXPECT_TRUE(h.arms.Resolve(h.tokens[selected])->originalLifetimePinned.load());
        owner.Release();
        ASSERT_TRUE(h.Begin());
        ASSERT_TRUE(h.arms.FinishUnpublishedPendingRetirement(&h.proof));
        EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, h.lease->Capture().state);
        EXPECT_EQ(0u, h.arms.LiveCount());
        for (const auto& pin : h.pins) EXPECT_FALSE(pin.Valid());
    }
}

TEST(DecisionGroup, OriginalRowOwnerBlocksExactAlreadyCancelledPendingRetirement) {
    for (std::size_t selected = 0; selected < 2; ++selected) {
        PrivatePendingArmRetirementHarness h;
        harken::TimelineArmTable::OriginalArmRowOwner owner;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(&owner, selected));
        ASSERT_TRUE(owner.Valid());
        for (auto& pin : h.pins) ASSERT_TRUE(pin.Release(0));
        ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
            h.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
        EXPECT_FALSE(h.arms.RetirePendingImmediateBatchExact(h.inputs.data(), h.inputs.size(),
            h.held.data(), h.held.size()));
        EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, h.lease->Capture().state);
        for (const auto token : h.tokens) {
            ASSERT_NE(nullptr, h.arms.Resolve(token));
            EXPECT_EQ(harken::ArmState::PendingApply, h.arms.Resolve(token)->state.load());
            EXPECT_FALSE(h.arms.Resolve(token)->reservation.test_and_set());
            h.arms.Resolve(token)->reservation.clear();
        }
        owner.Release();
        ASSERT_TRUE(h.arms.RetirePendingImmediateBatchExact(h.inputs.data(), h.inputs.size(),
            h.held.data(), h.held.size()));
        EXPECT_EQ(0u, h.arms.LiveCount());
    }
}

TEST(DecisionGroup, PrivatePendingArmRetirementAllowsEarlierArmDeadline) {
    for (const auto deadline : {0u, 96u, 99u, 100u}) {
        PrivatePendingArmRetirementHarness h;
        ASSERT_NO_FATAL_FAILURE(h.Prepare(nullptr, 0, deadline));
        for (const auto token : h.tokens) {
            const auto* row = h.arms.Resolve(token);
            ASSERT_NE(nullptr, row);
            EXPECT_EQ(100u, row->startFrame);
            EXPECT_EQ(deadline, row->armDeadline);
        }
        ASSERT_TRUE(h.Begin());
        ASSERT_TRUE(h.arms.FinishUnpublishedPendingRetirement(&h.proof));
        EXPECT_EQ(0u, h.arms.LiveCount());
        EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, h.lease->Capture().state);
    }
}

TEST(DecisionGroup, PrivatePendingArmRetirementEarlierDeadlinePreservesReaderAndStartGuards) {
    PrivatePendingArmRetirementHarness h;
    ASSERT_NO_FATAL_FAILURE(h.Prepare(nullptr, 0, 96));
    --h.inputs[1].startFrame;
    EXPECT_FALSE(h.Begin());
    h.CheckUntouched();
    ++h.inputs[1].startFrame;
    harken::TimelineArmReaderPin reader;
    ASSERT_TRUE(h.arms.PinFrontierReader(h.tokens[1], &reader));
    EXPECT_FALSE(h.Begin());
    EXPECT_TRUE(reader.Valid());
    ASSERT_EQ(harken::AudioSliceState::Ready, h.lease->Capture().state);
    ASSERT_TRUE(reader.Release(0));
    h.CheckUntouched();
    ASSERT_TRUE(h.Begin());
    ASSERT_TRUE(h.arms.FinishUnpublishedPendingRetirement(&h.proof));
    EXPECT_EQ(0u, h.arms.LiveCount());
}

TEST(DecisionGroup, PrivatePendingArmRetirementReservesAllRowsBeforeSharedLeaseMutation) {
    PrivatePendingArmRetirementHarness h;
    h.Prepare();
    ASSERT_NE(nullptr, h.lease);
    auto* second = h.arms.Resolve(h.tokens[1]);
    ASSERT_NE(nullptr, second);
    ASSERT_FALSE(second->reservation.test_and_set());
    EXPECT_FALSE(h.Begin());
    second->reservation.clear();
    h.CheckUntouched();
    ASSERT_TRUE(h.Begin());
    ASSERT_TRUE(h.proof.Valid());
    EXPECT_EQ(2u, h.proof.Count());
    ASSERT_TRUE(h.arms.AbortUnpublishedPendingRetirement(&h.proof));
    h.CheckUntouched();
    ASSERT_TRUE(h.Begin());
    ASSERT_TRUE(h.arms.FinishUnpublishedPendingRetirement(&h.proof));
    EXPECT_FALSE(h.proof.Valid());
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, h.lease->Capture().state);
    EXPECT_EQ(0u, h.arms.LiveCount());
    for (std::size_t index = 0; index < h.tokens.size(); ++index) {
        EXPECT_FALSE(h.pins[index].Valid());
        EXPECT_EQ(nullptr, h.held[index]);
        EXPECT_EQ(nullptr, h.arms.Resolve(h.tokens[index]));
    }
    EXPECT_FALSE(h.arms.FinishUnpublishedPendingRetirement(&h.proof));
}

TEST(DecisionGroup, PrivatePendingArmRetirementRejectsWrongRenderAndAdditionalReader) {
    PrivatePendingArmRetirementHarness h;
    h.Prepare();
    ASSERT_NE(nullptr, h.lease);
    ++h.inputs[1].arm.renderGeneration;
    EXPECT_FALSE(h.Begin());
    h.inputs[1].arm = h.tokens[1];
    h.CheckUntouched();
    harken::TimelineArmReaderPin extra;
    ASSERT_TRUE(h.arms.PinFrontierReader(h.tokens[1], &extra));
    EXPECT_FALSE(h.Begin());
    EXPECT_TRUE(extra.Valid());
    ASSERT_TRUE(extra.Release(0));
    h.CheckUntouched();
    ASSERT_TRUE(h.Begin());
    ASSERT_TRUE(h.arms.FinishUnpublishedPendingRetirement(&h.proof));
}

TEST(DecisionGroup, PrivatePendingArmRetirementRejectsAcknowledgedArm) {
    PrivatePendingArmRetirementHarness h;
    h.Prepare();
    ASSERT_NE(nullptr, h.lease);
    ASSERT_TRUE(h.arms.AcknowledgeApplied(h.tokens[1], 1));
    EXPECT_FALSE(h.Begin());
    EXPECT_FALSE(h.proof.Valid());
    EXPECT_EQ(harken::AudioSliceState::Ready, h.lease->Capture().state);
    EXPECT_EQ(harken::ArmState::Armed, h.arms.Resolve(h.tokens[1])->state.load());
    for (auto& pin : h.pins) ASSERT_TRUE(pin.Release(0));
}

TEST(DecisionGroup, PrivatePendingArmRetirementRejectsClaimedSharedLease) {
    PrivatePendingArmRetirementHarness h;
    h.Prepare();
    ASSERT_NE(nullptr, h.lease);
    ASSERT_TRUE(h.lease->TryClaim());
    EXPECT_FALSE(h.Begin());
    EXPECT_FALSE(h.proof.Valid());
    EXPECT_EQ(harken::AudioSliceState::SliceClaimed, h.lease->Capture().state);
    EXPECT_FALSE(h.lease->Capture().invalidateAfterSlice);
    for (auto& pin : h.pins) ASSERT_TRUE(pin.Release(0));
    ASSERT_TRUE(h.lease->AbortClaimedBeforeApply(harken::AudioSliceInvalidateReason::Cancel));
}

enum class UnpublishedRetirementArmEnd {
    Reclaimed,
    Reused,
    RebuiltTable,
    Quarantined,
    RollbackMember,
    RollbackPrimary,
    ReplaceMember,
    Stage3Primary,
    UnrelatedGroup,
};

void CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd armEnd) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    if (armEnd == UnpublishedRetirementArmEnd::Quarantined)
        arms.RowAt(0)->generation = UINT32_MAX;
    harken::DecisionTable decisions(2, 2);
    auto config = MakePoolConfig();
    config.maxPlan03GroupHeadersPerPage =
        armEnd == UnpublishedRetirementArmEnd::UnrelatedGroup ? 3 : 2;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::DecisionToken coordinatorDecision{}, memberDecision{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(1), &coordinatorDecision));
    ASSERT_TRUE(decisions.Reserve(MakeRequest(2), &memberDecision));
    harken::Plan03GroupSliceInput input{};
    input.decision = coordinatorDecision;
    harken::CommittedPlan03GroupSlice committedPrimary;
    if (armEnd == UnpublishedRetirementArmEnd::RollbackPrimary ||
        armEnd == UnpublishedRetirementArmEnd::Stage3Primary) {
        harken::PreparedPlan03GroupSlice prepared;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.PreparePlan03GroupSlice(input, arms, &prepared));
        ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(
            &prepared, &committedPrimary, false));
    } else {
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(input, arms));
    }
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.ReserveGrouped(1, 1, 0, 100, 29, target,
        harken::ParentAudioGroupRef{memberDecision}, 0, &arm));
    input.decision = memberDecision;
    input.members = &arm;
    input.memberCount = 1;
    harken::CommittedPlan03GroupSlice committedMember;
    if (armEnd == UnpublishedRetirementArmEnd::RollbackMember) {
        harken::PreparedPlan03GroupSlice prepared;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.PreparePlan03GroupSlice(input, arms, &prepared));
        ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(
            &prepared, &committedMember, false));
    } else {
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.ReservePlan03GroupSlice(input, arms));
    }
    std::array<harken::Plan03PageGroupView, 2> groups{};
    ASSERT_EQ(2u, pool.CaptureLivePlan03Groups(groups.data(), groups.size()));
    const auto primary = groups[0].identity;
    const auto member = groups[1].identity;
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = coordinatorDecision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.group = primary;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = primary;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(primary, binding));
    binding.role = harken::FrontierGroupRole::EntryMember;
    binding.operation.group = member;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(member, binding));
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
    ASSERT_NE(nullptr, groups[1].frontier);
    ASSERT_NE(nullptr, groups[1].lease);
    harken::TimelineArmReaderPin armReader;
    ASSERT_TRUE(arms.PinFrontierReader(arm, &armReader));
    ASSERT_TRUE(arms.BindFrontierAdmission(
        arm, *groups[1].frontier, *groups[1].lease));
    auto wrongRender = arm;
    ++wrongRender.renderGeneration;
    EXPECT_FALSE(arms.WitnessUnpublishedFrontierRetirement(
        wrongRender, *groups[1].frontier, *groups[1].lease));
    harken::FrontierGroupBinding wrongBinding = *groups[1].frontier;
    harken::AudioSliceLease wrongLease;
    EXPECT_FALSE(arms.WitnessUnpublishedFrontierRetirement(
        arm, wrongBinding, *groups[1].lease));
    EXPECT_FALSE(arms.WitnessUnpublishedFrontierRetirement(
        arm, *groups[1].frontier, wrongLease));
    harken::FrontierActivationDescriptor descriptor{};
    descriptor.entry = binding.entry;
    descriptor.primary = primary;
    descriptor.order.logicCommitSequence = 1;
    descriptor.worldGeneration = 7;
    descriptor.worldSemanticFingerprint = 42;
    descriptor.expectedEntryEventCount = 1;
    descriptor.programGeneration = 3;
    ASSERT_TRUE(decisions.PrepareFrontierActivation(
        coordinatorDecision, descriptor, pool));
    harken::DecisionCommitPayload payload{};
    payload.pages = {1, 1};
    payload.armGroup = {1, 1};
    ASSERT_TRUE(decisions.Commit(coordinatorDecision, payload));
    ASSERT_TRUE(decisions.PublishFrontierActivation(coordinatorDecision));
    harken::Plan03GroupReaderPin externalReader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(member, &externalReader));
    harken::Plan03GroupReaderPin primaryExternalReader;
    if (armEnd == UnpublishedRetirementArmEnd::Stage3Primary)
        ASSERT_TRUE(pool.AcquirePlan03GroupReader(primary, &primaryExternalReader));
    auto* activation = decisions.ResolveFrontierActivation(coordinatorDecision);
    ASSERT_NE(nullptr, activation);
    ASSERT_EQ(harken::FrontierCancelDisposition::CancelledBeforeAudio,
              activation->Cancel(binding.entry, harken::AudioSliceInvalidateReason::Cancel));
    harken::FrontierActivationReceipt receipt{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready,
              activation->Read(binding.entry, 0, &receipt));
    harken::Plan03UnpublishedRetirementToken retirement{};
    harken::Plan03ExactGroupSnapshot memberBefore{}, primaryBefore{};
    ASSERT_TRUE(pool.CaptureExactPlan03Group(member, &memberBefore));
    ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryBefore));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::NotEligible,
        pool.BeginUnpublishedFrontierRetirement(
            member, primary, binding.entry, receipt, &retirement));
    EXPECT_FALSE(retirement.Valid());
    harken::Plan03ExactGroupSnapshot memberAfter{}, primaryAfter{};
    ASSERT_TRUE(pool.CaptureExactPlan03Group(member, &memberAfter));
    ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
    EXPECT_EQ(memberBefore.readerPins, memberAfter.readerPins);
    EXPECT_EQ(primaryBefore.readerPins, primaryAfter.readerPins);
    EXPECT_EQ(primaryBefore.frontierDependents, primaryAfter.frontierDependents);
    EXPECT_EQ(0u, primaryAfter.pagePublicationHolds);
    ASSERT_TRUE(arms.RequestCancel(arm));
    EXPECT_FALSE(arms.WitnessUnpublishedFrontierRetirement(
        wrongRender, *groups[1].frontier, *groups[1].lease));
    EXPECT_FALSE(arms.WitnessUnpublishedFrontierRetirement(
        arm, wrongBinding, *groups[1].lease));
    EXPECT_FALSE(arms.WitnessUnpublishedFrontierRetirement(
        arm, *groups[1].frontier, wrongLease));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
        pool.BeginUnpublishedFrontierRetirement(
            member, primary, binding.entry, receipt, &retirement));
    ASSERT_TRUE(retirement.Valid());
    ASSERT_TRUE(pool.CaptureExactPlan03Group(member, &memberAfter));
    ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
    EXPECT_EQ(1u, memberAfter.readerPins);
    EXPECT_EQ(armEnd == UnpublishedRetirementArmEnd::Stage3Primary ? 3u : 2u,
              primaryAfter.readerPins);
    EXPECT_EQ(0u, primaryAfter.frontierDependents);
    EXPECT_EQ(1u, primaryAfter.pagePublicationHolds);
    pool.Publish(200, nullptr, nullptr, &arms);
    EXPECT_EQ(0u, pool.PublishCutoffFrame());
    EXPECT_EQ(0u, pool.Advance());
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    EXPECT_FALSE(decisions.FrontierActivationBindingsDetached(binding.entry, pool, 0));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Ready,
        pool.BeginUnpublishedFrontierRetirement(
            member, primary, binding.entry, receipt, &retirement));
    ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
    EXPECT_EQ(2u, primaryAfter.unpublishedRetirementStage);
    EXPECT_EQ(harken::FrontierGroupRole::Coordinator, primaryAfter.frontier.role);
    EXPECT_EQ(armEnd == UnpublishedRetirementArmEnd::Stage3Primary ? 2u : 1u,
              primaryAfter.readerPins);
    EXPECT_EQ(armEnd != UnpublishedRetirementArmEnd::Stage3Primary,
              decisions.FrontierActivationBindingsDetached(binding.entry, pool, 0));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    ASSERT_TRUE(externalReader.Release(0));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    EXPECT_TRUE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
    EXPECT_TRUE(pool.ResolvePlan03GroupSliceProducer(primary, &groups[0]));
    harken::FrontierActivationReceipt reconciled{};
    ASSERT_TRUE(decisions.ReconcileFrontierActivation(
        coordinatorDecision, binding.entry, 0, &reconciled));
    EXPECT_EQ(armEnd != UnpublishedRetirementArmEnd::Stage3Primary,
              decisions.FrontierActivationBindingsDetached(binding.entry, pool, 0));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    ASSERT_TRUE(pool.CaptureExactPlan03Group(member, &memberAfter));
    EXPECT_EQ(1u, memberAfter.pagePublicationHolds);
    EXPECT_EQ(0u, memberAfter.readerPins);
    if (armEnd == UnpublishedRetirementArmEnd::RollbackMember ||
        armEnd == UnpublishedRetirementArmEnd::RollbackPrimary) {
        auto* committed = armEnd == UnpublishedRetirementArmEnd::RollbackMember
            ? &committedMember : &committedPrimary;
        ASSERT_TRUE(committed->Valid());
        const bool rolledBack = pool.RollbackCommittedPlan03GroupSlice(committed);
        EXPECT_FALSE(rolledBack);
        EXPECT_TRUE(committed->Valid());
        EXPECT_TRUE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
        EXPECT_TRUE(pool.ResolvePlan03GroupSliceProducer(primary, &groups[0]));
        ASSERT_EQ(1u, groups[1].memberCount);
        EXPECT_EQ(arm.slot, groups[1].members[0].slot);
        EXPECT_EQ(arm.generation, groups[1].members[0].generation);
        EXPECT_TRUE(armReader.Valid());
        ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
        EXPECT_EQ(1u, primaryAfter.pagePublicationHolds);
        if (rolledBack) return;
        EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
                  pool.FinishUnpublishedFrontierRetirement(retirement));
    }
    if (armEnd == UnpublishedRetirementArmEnd::ReplaceMember) {
        harken::Plan03GroupSliceInput replacement = input;
        replacement.members = nullptr;
        replacement.memberCount = 0;
        replacement.sourceControlCount = 0;
        harken::PreparedPlan03GroupSliceReplacement prepared;
        EXPECT_EQ(harken::Plan03GroupReserveStatus::PageFrozen,
            pool.PreparePlan03GroupSliceReplacement(
                member, replacement, arms, &prepared));
        EXPECT_FALSE(prepared.Valid());
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
        ASSERT_EQ(1u, groups[1].memberCount);
        EXPECT_EQ(arm.slot, groups[1].members[0].slot);
        EXPECT_EQ(arm.generation, groups[1].members[0].generation);
        EXPECT_TRUE(armReader.Valid());
        ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
        EXPECT_EQ(1u, primaryAfter.pagePublicationHolds);
        EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
                  pool.FinishUnpublishedFrontierRetirement(retirement));
    }
    if (armEnd == UnpublishedRetirementArmEnd::UnrelatedGroup) {
        harken::Plan03GroupSliceInput unrelated{};
        unrelated.decision = {77, 1};
        harken::PreparedPlan03GroupSlice prepared;
        harken::CommittedPlan03GroupSlice committed;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.PreparePlan03GroupSlice(unrelated, arms, &prepared));
        ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(
            &prepared, &committed, false));
        std::array<harken::Plan03PageGroupView, 3> currentGroups{};
        ASSERT_EQ(3u, pool.CaptureLivePlan03Groups(
            currentGroups.data(), currentGroups.size()));
        harken::Plan03GroupSliceIdentity unrelatedIdentity{};
        for (const auto& group : currentGroups)
            if (group.identity.decision.slot == unrelated.decision.slot)
                unrelatedIdentity = group.identity;
        ASSERT_TRUE(unrelatedIdentity.Exact());
        harken::Plan03GroupSliceInput replacement = unrelated;
        replacement.decision = {78, 1};
        harken::PreparedPlan03GroupSliceReplacement preparedReplacement;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            pool.PreparePlan03GroupSliceReplacement(
                unrelatedIdentity, replacement, arms, &preparedReplacement));
        ASSERT_TRUE(pool.CommitPreparedPlan03GroupSliceReplacement(
            &preparedReplacement));
        ASSERT_TRUE(pool.RollbackCommittedPlan03GroupSlice(&committed));
        ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
        EXPECT_EQ(1u, primaryAfter.pagePublicationHolds);
    }
    ASSERT_TRUE(arms.Tombstone(arm, 0));
    EXPECT_FALSE(arms.Reclaim(arm, 1));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    EXPECT_TRUE(armReader.Release(0));
    if (armEnd == UnpublishedRetirementArmEnd::Quarantined) {
        EXPECT_FALSE(arms.Reclaim(arm, 1));
        ASSERT_EQ(harken::ArmState::Quarantined,
                  arms.RowAt(arm.slot)->state.load());
    } else {
        ASSERT_TRUE(arms.Reclaim(arm, 1));
    }
    if (armEnd == UnpublishedRetirementArmEnd::Stage3Primary) {
        EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Pending,
                  pool.FinishUnpublishedFrontierRetirement(retirement));
        EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
        ASSERT_TRUE(pool.CaptureExactPlan03Group(primary, &primaryAfter));
        EXPECT_EQ(3u, primaryAfter.unpublishedRetirementStage);
        EXPECT_EQ(1u, primaryAfter.pagePublicationHolds);
        ASSERT_TRUE(primaryExternalReader.Release(0));
        EXPECT_TRUE(decisions.FrontierActivationBindingsDetached(binding.entry, pool, 0));
        EXPECT_FALSE(pool.RollbackCommittedPlan03GroupSlice(&committedPrimary));
        EXPECT_TRUE(committedPrimary.Valid());
        harken::Plan03GroupSliceInput reusedGroup{};
        reusedGroup.decision = {77, 1};
        harken::PreparedPlan03GroupSlice prepared;
        harken::CommittedPlan03GroupSlice committed;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                  pool.PreparePlan03GroupSlice(reusedGroup, arms, &prepared));
        ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(
            &prepared, &committed, false));
        std::array<harken::Plan03PageGroupView, 2> currentGroups{};
        ASSERT_EQ(2u, pool.CaptureLivePlan03Groups(
            currentGroups.data(), currentGroups.size()));
        const auto newGroup = currentGroups[0].identity.committedRow == member.committedRow
            ? currentGroups[0].identity : currentGroups[1].identity;
        EXPECT_EQ(member.committedRow, newGroup.committedRow);
        EXPECT_NE(member.rowClaimGeneration, newGroup.rowClaimGeneration);
        harken::Plan03GroupSliceInput replacement = reusedGroup;
        replacement.decision = {78, 1};
        harken::PreparedPlan03GroupSliceReplacement preparedReplacement;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            pool.PreparePlan03GroupSliceReplacement(
                newGroup, replacement, arms, &preparedReplacement));
        ASSERT_TRUE(pool.CommitPreparedPlan03GroupSliceReplacement(
            &preparedReplacement));
        EXPECT_TRUE(pool.RollbackCommittedPlan03GroupSlice(&committed));
        EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Ready,
                  pool.FinishUnpublishedFrontierRetirement(retirement));
        return;
    }
    if (armEnd == UnpublishedRetirementArmEnd::RebuiltTable) {
        const auto oldIncarnation = arms.Incarnation();
        std::destroy_at(&arms);
        std::construct_at(&arms, 1);
        ASSERT_NE(oldIncarnation, arms.Incarnation());
        EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Stale,
                  pool.FinishUnpublishedFrontierRetirement(retirement));
        ASSERT_TRUE(pool.CaptureExactPlan03Group(member, &memberAfter));
        EXPECT_EQ(1u, memberAfter.pagePublicationHolds);
        return;
    }
    harken::TimelineArmToken nextArm{};
    if (armEnd == UnpublishedRetirementArmEnd::Reused) {
        ASSERT_TRUE(arms.Reserve(1, 2, 0, 100, 29, target, &nextArm));
        ASSERT_EQ(arm.slot, nextArm.slot);
        ASSERT_GT(nextArm.generation, arm.generation);
        const auto* nextRow = arms.RowAt(nextArm.slot);
        ASSERT_NE(nullptr, nextRow);
        harken::ArmState nextState{};
        ASSERT_TRUE(arms.State(nextArm, &nextState));
        EXPECT_EQ(harken::ArmState::PendingApply, nextState);
        EXPECT_EQ(0u, nextRow->frontierReaderPins.load());
        EXPECT_EQ(nullptr, nextRow->frontierAdmission);
        EXPECT_EQ(nullptr, nextRow->frontierAdmissionLease);
        EXPECT_EQ(1u, nextRow->renderSlot);
        EXPECT_EQ(2u, nextRow->renderGeneration);
    }
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Ready,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    if (armEnd == UnpublishedRetirementArmEnd::Reused) {
        const auto* nextRow = arms.RowAt(nextArm.slot);
        harken::ArmState nextState{};
        ASSERT_TRUE(arms.State(nextArm, &nextState));
        EXPECT_EQ(harken::ArmState::PendingApply, nextState);
        EXPECT_EQ(0u, nextRow->frontierReaderPins.load());
        EXPECT_EQ(nullptr, nextRow->frontierAdmission);
        EXPECT_EQ(nullptr, nextRow->frontierAdmissionLease);
        EXPECT_EQ(1u, nextRow->renderSlot);
        EXPECT_EQ(2u, nextRow->renderGeneration);
        EXPECT_TRUE(arms.AcknowledgeApplied(nextArm));
    }
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(member, &groups[1]));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(primary, &groups[0]));
    EXPECT_EQ(harken::Plan03UnpublishedRetirementResult::Stale,
              pool.FinishUnpublishedFrontierRetirement(retirement));
    pool.Publish(200, nullptr, nullptr, &arms);
    EXPECT_GE(pool.PublishCutoffFrame(), 100u);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementWaitsForRealReadersAndActivation) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::Reused);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementAfterReclaimWithoutReuse) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::Reclaimed);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementRejectsRebuiltArmTable) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::RebuiltTable);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementAcceptsQuarantinedArm) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::Quarantined);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementRejectsMemberRollback) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::RollbackMember);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementRejectsPrimaryRollback) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::RollbackPrimary);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementRejectsMemberReplacement) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::ReplaceMember);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementProtectsStage3Primary) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::Stage3Primary);
}

TEST(DecisionGroup, UnpublishedFrontierRetirementAllowsUnrelatedSamePageGroup) {
    CheckUnpublishedFrontierRetirement(UnpublishedRetirementArmEnd::UnrelatedGroup);
}

TEST(DecisionGroup, FrontierClaimDefersEveryDecisionMutationUntilItsNoFailApply) {
    FrontierReceiptHarness h;
    h.Prepare();
    h.Publish();
    auto* activation = h.decisions.ResolveFrontierActivation(h.token);
    ASSERT_NE(nullptr, activation);
    ASSERT_TRUE(activation->TryBeginAudio(h.descriptor.entry));
    ASSERT_TRUE(h.pool.NotePlan03GroupReadRt(h.group.identity, 1));
    harken::FrontierDecisionApplyProof proof;
    ASSERT_TRUE(h.decisions.ValidateFrontierDecisionApply(h.token, 1, &proof));
    ASSERT_TRUE(h.group.lease->TryClaim());
    EXPECT_EQ(harken::DecisionInvalidationDisposition::DeferredByFrontier, h.decisions.Invalidate(h.token));
    EXPECT_EQ(harken::DecisionDeadlineDisposition::DeferredByFrontier, h.decisions.InvalidateForDeadline(h.token));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::DeferredByFrontier, h.decisions.InvalidateForAuthoredStop(h.token));
    EXPECT_FALSE(h.decisions.TryInvalidatePending(h.token));
    EXPECT_FALSE(h.decisions.TryClaimAudio(h.token));
    h.decisions.ApplyClaimedAudioSlice(h.token);
    EXPECT_FALSE(h.decisions.MarkAudioAdmitted(h.token));
    EXPECT_FALSE(h.decisions.Retire(h.token, 1));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(h.token, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::Pending, snapshot.audioState);
    EXPECT_EQ(0u, snapshot.consumedSlices);
    h.decisions.ApplyClaimedFrontierDecision(proof);
    ASSERT_TRUE(h.group.lease->MarkApplied());
    ASSERT_TRUE(activation->FinishAudio(h.descriptor.entry, harken::FrontierReceiptOutcome::Applied,
        harken::FrontierReceiptReason::None, 1));
    ASSERT_TRUE(h.decisions.CaptureCommitted(h.token, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::Admitted, snapshot.audioState);
    EXPECT_EQ(1u, snapshot.consumedSlices);
    EXPECT_EQ(1u, snapshot.lastAudioSequence);
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, h.decisions.Invalidate(h.token));
    harken::FrontierActivationReceipt receipt{};
    ASSERT_TRUE(h.decisions.ReconcileFrontierActivation(h.token, h.descriptor.entry, 2, &receipt));
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
}

TEST(DecisionGroup, CommitPublishesOneCompleteImmutableDecision) {
    harken::DecisionTable table(1, 2);
    harken::DecisionToken token{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &token));
    ASSERT_TRUE(token.Valid());

    harken::DecisionCommittedSnapshot unavailable{};
    unavailable.ticket.sequence = 99;
    EXPECT_FALSE(table.CaptureCommitted(token, &unavailable));
    EXPECT_EQ(99u, unavailable.ticket.sequence);

    const harken::DecisionParticipantRef participants[] = {
        {harken::DecisionParticipantKind::Occurrence, 3, 5},
        {harken::DecisionParticipantKind::TimelineArm, 7, 13},
    };
    harken::DecisionCommitPayload payload{};
    payload.participants = participants;
    payload.participantCount = 2;
    payload.pages = {17, 19};
    payload.armGroup = {23, 29};
    ASSERT_TRUE(table.Commit(token, payload));
    EXPECT_FALSE(table.Cancel(token));
    EXPECT_FALSE(table.IsCancelled(token));

    harken::DecisionCommittedSnapshot committed{};
    ASSERT_TRUE(table.CaptureCommitted(token, &committed));
    EXPECT_EQ(harken::DecisionCommitState::Committed, committed.commit);
    EXPECT_EQ(1u, committed.ticket.sequence);
    EXPECT_EQ(7u, committed.world);
    EXPECT_EQ(9u, committed.transport);
    EXPECT_EQ(11u, committed.continuationGeneration);
    EXPECT_EQ(harken::AudioGroupState::Pending, committed.audioState);
    EXPECT_EQ(2u, committed.sliceCount);
    EXPECT_EQ(0u, committed.consumedSlices);
    EXPECT_EQ(2u, committed.participantCount);
    EXPECT_EQ(17u, committed.pages.slot);
    EXPECT_EQ(19u, committed.pages.generation);
    EXPECT_EQ(23u, committed.armGroup.slot);
    EXPECT_EQ(29u, committed.armGroup.generation);

    harken::DecisionParticipantRef copied[2]{};
    std::uint32_t copiedCount = 0;
    ASSERT_TRUE(table.CopyCommittedParticipants(
        token, copied, 2, &copiedCount));
    ASSERT_EQ(2u, copiedCount);
    EXPECT_EQ(harken::DecisionParticipantKind::Occurrence, copied[0].kind);
    EXPECT_EQ(3u, copied[0].slot);
    EXPECT_EQ(5u, copied[0].generation);
    EXPECT_EQ(harken::DecisionParticipantKind::TimelineArm, copied[1].kind);
    EXPECT_EQ(7u, copied[1].slot);
    EXPECT_EQ(13u, copied[1].generation);
}

TEST(DecisionGroup, CapacityRefusalAndInvalidCommitLeaveNoPartialState) {
    harken::DecisionTable table(1, 2);
    harken::DecisionToken token{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &token));

    harken::DecisionToken refused{41, 43};
    EXPECT_FALSE(table.Reserve(MakeRequest(2), &refused));
    EXPECT_EQ(41u, refused.slot);
    EXPECT_EQ(43u, refused.generation);

    const harken::DecisionParticipantRef tooMany[] = {
        {harken::DecisionParticipantKind::Occurrence, 1, 1},
        {harken::DecisionParticipantKind::TimelineArm, 2, 2},
        {harken::DecisionParticipantKind::Property, 3, 3},
    };
    harken::DecisionCommitPayload invalid{};
    invalid.participants = tooMany;
    invalid.participantCount = 3;
    invalid.pages = {5, 7};
    invalid.armGroup = {11, 13};
    EXPECT_FALSE(table.Commit(token, invalid));

    harken::DecisionCommittedSnapshot snapshot{};
    EXPECT_FALSE(table.CaptureCommitted(token, &snapshot));
    EXPECT_TRUE(table.Cancel(token));
    EXPECT_FALSE(table.Commit(token, {}));
}

TEST(DecisionGroup, AppliedSliceReceiptRequiresExactPagesOrderAndGeneration) {
    harken::DecisionTable table(1, 1);
    harken::DecisionToken first{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &first));
    harken::DecisionCommitPayload payload{};
    payload.pages = {17, 19};
    payload.armGroup = {23, 29};
    ASSERT_TRUE(table.Commit(first, payload));
    EXPECT_FALSE(table.RecordAppliedSlice(first, payload.pages, 0, 1));
    ASSERT_TRUE(table.TryClaimAudio(first));
    EXPECT_FALSE(table.RecordAppliedSlice(first, {17, 20}, 0, 1));
    EXPECT_FALSE(table.RecordAppliedSlice(first, {18, 19}, 0, 1));
    EXPECT_FALSE(table.RecordAppliedSlice(first, payload.pages, 1, 1));
    ASSERT_TRUE(table.RecordAppliedSlice(first, payload.pages, 0, 2));
    ASSERT_TRUE(table.RecordAppliedSlice(first, payload.pages, 0, 9));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCommitted(first, &snapshot));
    EXPECT_EQ(1u, snapshot.consumedSlices);
    EXPECT_EQ(2u, snapshot.lastAudioSequence);
    EXPECT_FALSE(table.RecordAppliedSlice(first, payload.pages, 1, 1));
    ASSERT_TRUE(table.MarkAudioAdmitted(first));
    ASSERT_TRUE(table.RecordAppliedSlice(first, payload.pages, 1, 3));
    EXPECT_FALSE(table.RecordAppliedSlice(first, payload.pages, 2, 4));
    ASSERT_TRUE(table.CaptureCommitted(first, &snapshot));
    EXPECT_EQ(2u, snapshot.consumedSlices);
    EXPECT_EQ(3u, snapshot.lastAudioSequence);
    ASSERT_TRUE(table.Retire(first, 4));
    ASSERT_TRUE(table.ReclaimRetired(first));
    harken::DecisionToken second{};
    ASSERT_TRUE(table.Reserve(MakeRequest(2), &second));
    ASSERT_TRUE(table.Commit(second, payload));
    ASSERT_TRUE(table.TryClaimAudio(second));
    EXPECT_FALSE(table.RecordAppliedSlice(first, payload.pages, 0, 5));
    ASSERT_TRUE(table.CaptureCommitted(second, &snapshot));
    EXPECT_EQ(0u, snapshot.consumedSlices);
    EXPECT_EQ(0u, snapshot.lastAudioSequence);
}

TEST(DecisionGroup, ReuseRejectsOldGenerationAndNeverWrapsIdentity) {
    harken::DecisionTable table(1, 1);
    harken::DecisionToken first{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &first));
    EXPECT_FALSE(table.IsCancelled(first));
    ASSERT_TRUE(table.Cancel(first));
    EXPECT_TRUE(table.IsCancelled(first));
    ASSERT_TRUE(table.ReclaimCancelled(first));
    EXPECT_FALSE(table.IsCancelled(first));

    harken::DecisionToken second{};
    ASSERT_TRUE(table.Reserve(MakeRequest(2), &second));
    EXPECT_EQ(first.slot, second.slot);
    EXPECT_EQ(first.generation + 1, second.generation);
    EXPECT_FALSE(table.Cancel(first));
    EXPECT_TRUE(table.Cancel(second));
    EXPECT_FALSE(table.IsCancelled(first));
    EXPECT_TRUE(table.IsCancelled(second));

    ASSERT_TRUE(table.ReclaimCancelled(second));
    ASSERT_TRUE(table.SetGenerationForTests(0, UINT32_MAX));
    harken::DecisionToken last{};
    ASSERT_TRUE(table.Reserve(MakeRequest(3), &last));
    EXPECT_EQ(UINT32_MAX, last.generation);
    ASSERT_TRUE(table.Cancel(last));
    EXPECT_FALSE(table.ReclaimCancelled(last));
    EXPECT_EQ(1u, table.QuarantinedCount());

    harken::DecisionToken unchanged{31, 37};
    EXPECT_FALSE(table.Reserve(MakeRequest(4), &unchanged));
    EXPECT_EQ(31u, unchanged.slot);
    EXPECT_EQ(37u, unchanged.generation);
}

TEST(DecisionGroup, PreparedCancellationResumesAfterActualCancelAndReaderExit) {
    harken::DecisionTable table(1, 1);
    auto request = MakeRequest();
    request.ticket = {};
    harken::PreparedDecisionReservation prepared{};
    ASSERT_TRUE(table.PrepareReservation(request, &prepared));
    const auto token = prepared.Token();
    harken::DecisionFuturePin reader{};
    ASSERT_TRUE(table.AcquireFuturePin(token, &reader));
    ASSERT_TRUE(table.Cancel(token));
    EXPECT_FALSE(table.CancelPreparedReservation(&prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(token.generation, prepared.Token().generation);
    EXPECT_TRUE(table.IsCancelled(token));
    ASSERT_TRUE(reader.Release());
    EXPECT_TRUE(table.CancelPreparedReservation(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(0u, table.LiveCount());
    harken::DecisionToken next{};
    ASSERT_TRUE(table.Reserve(MakeRequest(2), &next));
    EXPECT_EQ(token.generation + 1u, next.generation);
    EXPECT_FALSE(table.Cancel(token));
}

TEST(DecisionGroup, PreparedCancellationDisposesFinalGenerationWithoutReusingSlot) {
    harken::DecisionTable table(1, 1);
    ASSERT_TRUE(table.SetGenerationForTests(0, UINT32_MAX));
    auto request = MakeRequest();
    request.ticket = {};
    harken::PreparedDecisionReservation prepared{};
    ASSERT_TRUE(table.PrepareReservation(request, &prepared));
    const auto token = prepared.Token();
    EXPECT_EQ(UINT32_MAX, token.generation);
    EXPECT_TRUE(table.CancelPreparedReservation(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(table.Contains(token));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(1u, table.QuarantinedCount());
    EXPECT_FALSE(table.CancelPreparedReservation(&prepared));
    harken::DecisionToken next{};
    EXPECT_FALSE(table.Reserve(MakeRequest(2), &next));
}

TEST(DecisionGroup, PreparedCancellationRejectsForeignTableAndKeepsOwner) {
    harken::DecisionTable owner(1, 1), foreign(1, 1);
    auto request = MakeRequest();
    request.ticket = {};
    harken::PreparedDecisionReservation prepared{};
    ASSERT_TRUE(owner.PrepareReservation(request, &prepared));
    const auto token = prepared.Token();
    EXPECT_FALSE(foreign.CancelPreparedReservation(&prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_TRUE(owner.IsOpen(token));
    ASSERT_TRUE(owner.CancelPreparedReservation(&prepared));
}

TEST(DecisionGroup, InvalidIdentityInputsDoNotConsumeCapacity) {
    harken::DecisionTable table(1, 1);
    harken::DecisionToken unchanged{31, 37};

    auto invalid = MakeRequest();
    invalid.ticket.sequence = 0;
    EXPECT_FALSE(table.Reserve(invalid, &unchanged));
    invalid = MakeRequest();
    invalid.world = 0;
    EXPECT_FALSE(table.Reserve(invalid, &unchanged));
    invalid = MakeRequest();
    invalid.transport = 0;
    EXPECT_FALSE(table.Reserve(invalid, &unchanged));
    invalid = MakeRequest();
    invalid.expectedContinuationGeneration = 0;
    EXPECT_FALSE(table.Reserve(invalid, &unchanged));
    invalid = MakeRequest();
    invalid.sliceCount = 0;
    EXPECT_FALSE(table.Reserve(invalid, &unchanged));
    EXPECT_EQ(31u, unchanged.slot);
    EXPECT_EQ(37u, unchanged.generation);

    harken::DecisionToken valid{};
    EXPECT_TRUE(table.Reserve(MakeRequest(), &valid));
}

TEST(DecisionGroup, HardCapacityProfilesFailClosedBeforeAllocation) {
    EXPECT_FALSE(harken::DecisionTable(0, 1).Valid());
    EXPECT_FALSE(harken::DecisionTable(1, 0).Valid());
    EXPECT_FALSE(harken::DecisionTable(
                     harken::kMaxDecisionRowsHard + 1, 1)
                     .Valid());
    EXPECT_FALSE(harken::DecisionTable(
                     1, harken::kMaxParticipantsPerDecisionHard + 1)
                     .Valid());
    EXPECT_FALSE(harken::ArmGroupTable(0, 1).Valid());
    EXPECT_FALSE(harken::ArmGroupTable(1, 0).Valid());
    EXPECT_FALSE(harken::ArmGroupTable(
                     harken::kMaxArmGroupsHard + 1, 1)
                     .Valid());

    auto config = MakePoolConfig();
    config.maxPlan03GroupHeadersPerPage =
        harken::kMaxPlan03GroupHeadersPerPageHard + 1;
    harken::ScheduledTransactionPool pool;
    EXPECT_FALSE(pool.Reset(config, 0, 3));
    EXPECT_FALSE(pool.Valid());
}

TEST(DecisionGroup, DeadlineTransitionDistinguishesPrestartAndPoststart) {
    harken::DecisionTable table(2, 1);
    harken::DecisionCommitPayload payload{};
    payload.pages = {3, 5};
    payload.armGroup = {7, 11};

    harken::DecisionToken pending{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &pending));
    ASSERT_TRUE(table.Commit(pending, payload));
    EXPECT_EQ(harken::DecisionDeadlineDisposition::NewlyInvalidated,
              table.InvalidateForDeadline(pending));
    EXPECT_EQ(harken::DecisionDeadlineDisposition::AlreadyInvalidated,
              table.InvalidateForDeadline(pending));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCommitted(pending, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::DeadlineInvalidated,
              snapshot.audioState);

    harken::DecisionToken admitted{};
    ASSERT_TRUE(table.Reserve(MakeRequest(2), &admitted));
    ASSERT_TRUE(table.Commit(admitted, payload));
    ASSERT_TRUE(table.TryClaimAudio(admitted));
    ASSERT_TRUE(table.MarkAudioAdmitted(admitted));
    EXPECT_EQ(harken::DecisionDeadlineDisposition::NewlyInvalidated,
              table.InvalidateForDeadline(admitted));
    ASSERT_TRUE(table.CaptureCommitted(admitted, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::PostStartDeadlineInvalidated,
              snapshot.audioState);
}

TEST(DecisionGroup, ExplicitInvalidationDistinguishesPrestartAndPoststart) {
    harken::DecisionTable table(2, 1);
    harken::DecisionCommitPayload payload{};
    payload.pages = {3, 5};
    payload.armGroup = {7, 11};

    harken::DecisionToken pending{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &pending));
    ASSERT_TRUE(table.Commit(pending, payload));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated,
              table.Invalidate(pending));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::AlreadyInvalidated,
              table.Invalidate(pending));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCommitted(pending, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::Invalidated, snapshot.audioState);

    harken::DecisionToken admitted{};
    ASSERT_TRUE(table.Reserve(MakeRequest(2), &admitted));
    ASSERT_TRUE(table.Commit(admitted, payload));
    ASSERT_TRUE(table.TryClaimAudio(admitted));
    ASSERT_TRUE(table.MarkAudioAdmitted(admitted));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated,
              table.Invalidate(admitted));
    ASSERT_TRUE(table.CaptureCommitted(admitted, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::PostStartInvalidated,
              snapshot.audioState);
}

TEST(DecisionGroup, DeadlineAfterAudioClaimDefersUntilWholeSliceCompletes) {
    harken::DecisionTable table(1, 1);
    harken::DecisionToken decision{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &decision));
    harken::DecisionCommitPayload payload{};
    payload.pages = {3, 5};
    payload.armGroup = {7, 11};
    ASSERT_TRUE(table.Commit(decision, payload));
    ASSERT_TRUE(table.TryClaimAudio(decision));

    EXPECT_EQ(harken::DecisionDeadlineDisposition::NewlyInvalidated,
              table.InvalidateForDeadline(decision));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::PostStartDeadlineInvalidated,
              snapshot.audioState);
    EXPECT_TRUE(table.MarkAudioAdmitted(decision));
    ASSERT_TRUE(table.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::PostStartDeadlineInvalidated,
              snapshot.audioState);
}

TEST(DecisionGroup, RetiredAudioDecisionInvalidatesOldTokenBeforeReuse) {
    harken::DecisionTable table(1, 1);
    harken::DecisionToken first{};
    ASSERT_TRUE(table.Reserve(MakeRequest(), &first));
    harken::DecisionCommitPayload payload{};
    payload.pages = {3, 5};
    payload.armGroup = {7, 11};
    ASSERT_TRUE(table.Commit(first, payload));
    ASSERT_TRUE(table.TryClaimAudio(first));
    ASSERT_TRUE(table.MarkAudioAdmitted(first));
    ASSERT_TRUE(table.Retire(first, 13));
    harken::DecisionCommittedSnapshot retired{};
    ASSERT_TRUE(table.CaptureCommitted(first, &retired));
    EXPECT_EQ(harken::AudioGroupState::Retired, retired.audioState);
    ASSERT_TRUE(table.ReclaimRetired(first));
    EXPECT_FALSE(table.CaptureCommitted(first, &retired));

    harken::DecisionToken reused{};
    ASSERT_TRUE(table.Reserve(MakeRequest(2), &reused));
    EXPECT_EQ(first.slot, reused.slot);
    EXPECT_EQ(first.generation + 1u, reused.generation);
}

TEST(AudioSliceLease, InvalidationBeforeClaimMakesWholeSliceUnavailable) {
    harken::AudioSliceLease lease;
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
              lease.Invalidate(harken::AudioSliceInvalidateReason::Deadline));
    EXPECT_FALSE(lease.TryClaim());
    EXPECT_FALSE(lease.MarkApplied());

    const auto snapshot = lease.Capture();
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, snapshot.state);
    EXPECT_EQ(harken::AudioSliceReasonDeadline, snapshot.pendingReasonBits);
    EXPECT_FALSE(snapshot.invalidateAfterSlice);
}

TEST(AudioSliceLease, InvalidationAfterClaimDefersUntilWholeSliceApplied) {
    harken::AudioSliceLease lease;
    ASSERT_TRUE(lease.TryClaim());
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::DeferredAfterClaim,
              lease.Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::DeferredAfterClaim,
              lease.Invalidate(harken::AudioSliceInvalidateReason::Identity));
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::DeferredAfterClaim,
              lease.Invalidate(
                  harken::AudioSliceInvalidateReason::TransportReset));
    EXPECT_EQ(harken::AudioSliceState::SliceClaimed, lease.Capture().state);

    ASSERT_TRUE(lease.MarkApplied());
    harken::AudioSliceInvalidateReason selected =
        harken::AudioSliceInvalidateReason::Cancel;
    ASSERT_TRUE(lease.ConsumePostSliceInvalidation(&selected));
    EXPECT_EQ(harken::AudioSliceInvalidateReason::TransportReset, selected);
    EXPECT_FALSE(lease.ConsumePostSliceInvalidation(&selected));

    const auto snapshot = lease.Capture();
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, snapshot.state);
    EXPECT_FALSE(snapshot.invalidateAfterSlice);
    EXPECT_EQ(harken::AudioSliceReasonCancel |
                  harken::AudioSliceReasonIdentity |
                  harken::AudioSliceReasonTransportReset,
              snapshot.pendingReasonBits);
}

TEST(AudioSliceLease, AbortClaimedBeforeApplyInvalidatesWithoutExposingWork) {
    harken::AudioSliceLease lease;
    EXPECT_FALSE(lease.AbortClaimedBeforeApply(
        harken::AudioSliceInvalidateReason::Identity));
    EXPECT_EQ(0u, lease.Capture().pendingReasonBits);
    ASSERT_TRUE(lease.TryClaim());
    ASSERT_EQ(harken::AudioSliceInvalidateDisposition::DeferredAfterClaim,
        lease.Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    ASSERT_TRUE(lease.AbortClaimedBeforeApply(
        harken::AudioSliceInvalidateReason::Identity));
    const auto snapshot = lease.Capture();
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, snapshot.state);
    EXPECT_EQ(harken::AudioSliceReasonCancel | harken::AudioSliceReasonIdentity,
        snapshot.pendingReasonBits);
    EXPECT_FALSE(snapshot.invalidateAfterSlice);
    EXPECT_FALSE(lease.MarkApplied());
    EXPECT_FALSE(lease.TryClaim());
    harken::AudioSliceInvalidateReason selected =
        harken::AudioSliceInvalidateReason::Cancel;
    EXPECT_FALSE(lease.ConsumePostSliceInvalidation(&selected));
    ASSERT_TRUE(lease.ResetForReuse());
    EXPECT_TRUE(lease.TryClaim());
}

TEST(AudioSliceLease, AbortClaimedBeforeApplyPreservesConcurrentInvalidation) {
    for (std::uint32_t iteration = 0; iteration < 1000; ++iteration) {
        harken::AudioSliceLease lease;
        ASSERT_TRUE(lease.TryClaim());
        std::atomic<bool> start{false};
        std::thread logic([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            (void)lease.Invalidate(
                harken::AudioSliceInvalidateReason::TransportReset);
        });
        start.store(true, std::memory_order_release);
        EXPECT_TRUE(lease.AbortClaimedBeforeApply(
            harken::AudioSliceInvalidateReason::Identity));
        logic.join();
        const auto snapshot = lease.Capture();
        EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, snapshot.state);
        EXPECT_EQ(harken::AudioSliceReasonIdentity |
                      harken::AudioSliceReasonTransportReset,
                  snapshot.pendingReasonBits);
        EXPECT_FALSE(snapshot.invalidateAfterSlice);
    }
}

TEST(AudioSliceLease,
     InvalidationAfterAppliedCheckBelongsToLogicLifecycle) {
    harken::AudioSliceLease lease;
    ASSERT_TRUE(lease.TryClaim());
    ASSERT_TRUE(lease.MarkApplied());
    harken::AudioSliceInvalidateReason selected =
        harken::AudioSliceInvalidateReason::Cancel;
    EXPECT_FALSE(lease.ConsumePostSliceInvalidation(&selected));

    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::AppliedNeedsLifecycle,
              lease.Invalidate(
                  harken::AudioSliceInvalidateReason::Supersede));
    const auto snapshot = lease.Capture();
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, snapshot.state);
    EXPECT_EQ(harken::AudioSliceReasonSupersede,
              snapshot.pendingReasonBits);
    EXPECT_FALSE(snapshot.invalidateAfterSlice);
    EXPECT_FALSE(lease.ConsumePostSliceInvalidation(&selected));
}

TEST(AudioSliceLease, ResetForReuseClearsInvalidationResidue) {
    harken::AudioSliceLease lease;
    ASSERT_TRUE(lease.TryClaim());
    ASSERT_TRUE(lease.MarkApplied());
    ASSERT_EQ(harken::AudioSliceInvalidateDisposition::AppliedNeedsLifecycle,
              lease.Invalidate(
                  harken::AudioSliceInvalidateReason::WorldReplacement));
    ASSERT_TRUE(lease.ResetForReuse());

    const auto snapshot = lease.Capture();
    EXPECT_EQ(harken::AudioSliceState::Ready, snapshot.state);
    EXPECT_EQ(0u, snapshot.pendingReasonBits);
    EXPECT_FALSE(snapshot.invalidateAfterSlice);
}

TEST(AudioSliceLease, ClaimAndInvalidateRaceHasOneWholeOutcome) {
    constexpr std::uint32_t kIterations = 10000;
    std::optional<harken::AudioSliceLease> lease;
    bool claimed = false;
    bool applied = false;
    harken::AudioSliceInvalidateDisposition disposition{};
    std::barrier start(3);
    std::barrier finish(3);
    std::thread audio([&] {
        for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
            start.arrive_and_wait();
            claimed = lease->TryClaim();
            applied = claimed && lease->MarkApplied();
            finish.arrive_and_wait();
        }
    });
    std::thread logic([&] {
        for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
            start.arrive_and_wait();
            disposition = lease->Invalidate(
                harken::AudioSliceInvalidateReason::WorldReplacement);
            finish.arrive_and_wait();
        }
    });
    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        lease.emplace();
        start.arrive_and_wait();
        finish.arrive_and_wait();
        EXPECT_EQ(claimed, applied);
        const auto snapshot = lease->Capture();
        if (!claimed) {
            EXPECT_EQ(harken::AudioSliceState::SliceInvalidated,
                      snapshot.state);
            EXPECT_EQ(
                harken::AudioSliceInvalidateDisposition::InvalidatedReady,
                disposition);
        } else {
            EXPECT_EQ(harken::AudioSliceState::SliceApplied, snapshot.state);
            harken::AudioSliceInvalidateReason selected =
                harken::AudioSliceInvalidateReason::Cancel;
            if (disposition ==
                harken::AudioSliceInvalidateDisposition::DeferredAfterClaim) {
                EXPECT_TRUE(lease->ConsumePostSliceInvalidation(&selected));
                EXPECT_EQ(
                    harken::AudioSliceInvalidateReason::WorldReplacement,
                    selected);
            } else {
                EXPECT_EQ(
                    harken::AudioSliceInvalidateDisposition::
                        AppliedNeedsLifecycle,
                    disposition);
                EXPECT_FALSE(lease->ConsumePostSliceInvalidation(&selected));
            }
            EXPECT_FALSE(lease->ConsumePostSliceInvalidation(&selected));
        }
    }
    audio.join();
    logic.join();
}

TEST(AudioSliceLease, PassOneFailureAppliesNoPrefix) {
    harken::AudioSliceLease lease;
    std::uint32_t applied = 0;
    SliceStep steps[] = {
        {&lease, &applied, true, false},
        {&lease, &applied, false, false},
        {&lease, &applied, true, false},
    };
    const harken::AudioSliceOperation operations[] = {
        {&steps[0], ValidateSliceStep, nullptr, ApplySliceStep},
        {&steps[1], ValidateSliceStep, nullptr, ApplySliceStep},
        {&steps[2], ValidateSliceStep, nullptr, ApplySliceStep},
    };
    EXPECT_EQ(harken::AudioSliceApplyStatus::ValidationFailed,
              harken::ApplyAudioSliceWhole(lease, operations, 3));
    EXPECT_EQ(0u, applied);
    EXPECT_EQ(harken::AudioSliceState::Ready, lease.Capture().state);
}

TEST(AudioSliceLease, InvalidationAfterClaimCannotSplitPassTwo) {
    harken::AudioSliceLease lease;
    std::uint32_t applied = 0;
    SliceStep steps[] = {
        {&lease, &applied, true, true},
        {&lease, &applied, true, false},
    };
    const harken::AudioSliceOperation operations[] = {
        {&steps[0], ValidateSliceStep, nullptr, ApplySliceStep},
        {&steps[1], ValidateSliceStep, nullptr, ApplySliceStep},
    };
    EXPECT_EQ(harken::AudioSliceApplyStatus::AppliedWithDeferredInvalidation,
              harken::ApplyAudioSliceWhole(lease, operations, 2));
    EXPECT_EQ(2u, applied);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied,
              lease.Capture().state);
    harken::AudioSliceInvalidateReason reason =
        harken::AudioSliceInvalidateReason::Cancel;
    ASSERT_TRUE(lease.ConsumePostSliceInvalidation(&reason));
    EXPECT_EQ(harken::AudioSliceInvalidateReason::WorldReplacement, reason);
}

TEST(AudioSliceLease, AlreadyInvalidatedSliceAppliesNoPrefix) {
    harken::AudioSliceLease lease;
    ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
              lease.Invalidate(
                  harken::AudioSliceInvalidateReason::TransportReset));
    std::uint32_t applied = 0;
    SliceStep step{&lease, &applied, true, false};
    const harken::AudioSliceOperation operation{
        &step, ValidateSliceStep, nullptr, ApplySliceStep};
    EXPECT_EQ(harken::AudioSliceApplyStatus::SliceUnavailable,
              harken::ApplyAudioSliceWhole(lease, &operation, 1));
    EXPECT_EQ(0u, applied);
}

TEST(DecisionGroup, TimelineArmPublishesImmutableExactParentBeforePending) {
    harken::TimelineArmTable arms(2);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 17;
    target.subjectId = harkentest::ObjectId(19);

    harken::TimelineArmToken grouped{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 1024, 2048, 7, target, {{11, 13}}, 1, &grouped));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(arms.Observe(grouped, &observation));
    ASSERT_TRUE(observation.parentGroup.Valid());
    EXPECT_EQ(11u, observation.parentGroup.decision.slot);
    EXPECT_EQ(13u, observation.parentGroup.decision.generation);
    EXPECT_EQ(1u, observation.memberIndex);

    harken::TimelineArmToken standalone{};
    ASSERT_TRUE(arms.Reserve(7, 9, 2048, 4096, 11, target, &standalone));
    ASSERT_TRUE(arms.Observe(standalone, &observation));
    EXPECT_FALSE(observation.parentGroup.Valid());
    EXPECT_EQ(UINT16_MAX, observation.memberIndex);

    ASSERT_TRUE(arms.AcknowledgeApplied(grouped));
    ASSERT_TRUE(arms.RequestCancel(grouped));
    ASSERT_TRUE(arms.Tombstone(grouped, 20));
    ASSERT_TRUE(arms.Reclaim(grouped, 21));
    EXPECT_FALSE(arms.Observe(grouped, &observation));
}

TEST(DecisionGroup, GroupedTimelineArmRejectsInvalidParentOrMember) {
    harken::TimelineArmTable arms(1);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 1;
    target.subjectId = harkentest::ObjectId(3);
    harken::TimelineArmToken unchanged{31, 37, 41, 43};

    EXPECT_FALSE(arms.ReserveGrouped(
        3, 5, 0, 10, 7, target, {}, 0, &unchanged));
    EXPECT_FALSE(arms.ReserveGrouped(
        3, 5, 0, 10, 7, target, {{11, 13}}, UINT16_MAX, &unchanged));
    EXPECT_EQ(31u, unchanged.slot);
    EXPECT_EQ(37u, unchanged.generation);
    EXPECT_EQ(1u, arms.FreeCount());
}

TEST(DecisionGroup, VoiceBindingCopiesExactParentBeforeAudioVisibility) {
    harken::EngineConfig config;
    harken::VoiceCapacityModel capacity;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 0, &capacity));
    config.voiceCapacity = capacity;
    harken::ResampleTables tables;
    tables.Init();
    harken::MultirateResampleTables multirateTables;
    multirateTables.Init();
    harken::BiquadTrigTable biquadTrig;
    harken::EffectPoolBank effects(config);
    harken::DynamicRouteSystem routes(config);
    harken::TimelineArmTable arms(1);
    harken::VoicePool voices(config, &tables, &multirateTables, &biquadTrig,
                            effects, routes, &arms);

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.ReserveGrouped(
        0, 1, 0, 100, 13, target, {{17, 19}}, 0, &arm));
    VnVoiceHandle render = 0;
    float sample = 0.0f;
    harken::PCMView pcm{};
    pcm.channels[0] = &sample;
    pcm.channelCount = 1;
    pcm.stride = 1;
    pcm.frames = 1;
    pcm.sampleRate = 48000;
    harken::ResolvedPanGains pan{};
    ASSERT_EQ(VN_OK, voices.Play(pcm, 0.0f, 1.0, 0, false, 0.0f, 0.0f,
                                 0.0f, pan, 0, 1, 1,
                                 harken::ResampleProfile::LegacyCompatibility,
                                 &render));
    voices.ApplyTimelineGatedPlay(
        render, 23, 0,
        (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot, 0);
    const auto* context = voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->timelineParentGroup.Valid());
    EXPECT_EQ(17u, context->timelineParentGroup.decision.slot);
    EXPECT_EQ(19u, context->timelineParentGroup.decision.generation);
    EXPECT_EQ(0u, context->timelineGroupMemberIndex);
}

TEST(DecisionGroup, InvalidatedUnstartedMemberIsExcludedUnlessSliceClaimed) {
    harken::EngineConfig config;
    harken::VoiceCapacityModel capacity;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 0, &capacity));
    config.voiceCapacity = capacity;
    harken::ResampleTables tables;
    tables.Init();
    harken::MultirateResampleTables multirateTables;
    multirateTables.Init();
    harken::BiquadTrigTable biquadTrig;
    harken::EffectPoolBank effects(config);
    harken::DynamicRouteSystem routes(config);
    harken::TimelineArmTable arms(1);
    harken::VoicePool voices(config, &tables, &multirateTables, &biquadTrig,
                            effects, routes, &arms);
    harken::DecisionTable decisions(1, 1);
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(), &decision));
    harken::DecisionCommitPayload payload{};
    payload.pages = {1, 1};
    payload.armGroup = {1, 1};
    ASSERT_TRUE(decisions.Commit(decision, payload));

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.ReserveGrouped(
        0, 1, 0, 100, 13, target, {decision}, 0, &arm));
    VnVoiceHandle render = 0;
    float sample = 0.0f;
    harken::PCMView pcm{};
    pcm.channels[0] = &sample;
    pcm.channelCount = 1;
    pcm.stride = 1;
    pcm.frames = 1;
    pcm.sampleRate = 48000;
    harken::ResolvedPanGains pan{};
    ASSERT_EQ(VN_OK, voices.Play(pcm, 0.0f, 1.0, 0, false, 0.0f, 0.0f,
                                 0.0f, pan, 0, 1, 1,
                                 harken::ResampleProfile::LegacyCompatibility,
                                 &render));
    voices.ApplyTimelineGatedPlay(
        render, 23, 0,
        (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot, 0);

    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = decision;
    input.members = &arm;
    input.memberCount = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));
    pool.Publish(100, nullptr, nullptr, &arms);

    harken::VoiceTimelineSink sink(voices, arms);
    ASSERT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
    const auto required = voices.TimelineScratchBytes(1);
    ASSERT_GT(required, 0u);
    ASSERT_LE(required, UINT32_MAX);
    sink.SetArenaAdmission(static_cast<std::uint32_t>(required - 1), 1);
    EXPECT_FALSE(sink.AdmitQuantum(0));

    EXPECT_EQ(harken::DecisionDeadlineDisposition::NewlyInvalidated,
              decisions.InvalidateForDeadline(decision));
    ASSERT_TRUE(arms.RequestCancel(arm));
    EXPECT_TRUE(sink.AdmitQuantum(0));

    const harken::Plan03ScratchDecisionState unclaimed{decision.generation, false};
    std::vector<std::uint32_t> controlTargets(voices.Capacity());
    const auto renderSlot = static_cast<std::uint32_t>(render);
    const auto renderGeneration = static_cast<std::uint32_t>(render >> 32);
    controlTargets[renderSlot] = renderGeneration + 1;
    EXPECT_TRUE(voices.AdmitTimelineQuantum(static_cast<std::uint32_t>(required - 1),
        1, &decisions, &unclaimed, 1, controlTargets.data(),
        static_cast<std::uint32_t>(controlTargets.size())));
    controlTargets[renderSlot] = renderGeneration;
    EXPECT_FALSE(voices.AdmitTimelineQuantum(static_cast<std::uint32_t>(required - 1),
        1, &decisions, &unclaimed, 1, controlTargets.data(),
        static_cast<std::uint32_t>(controlTargets.size())));
    EXPECT_TRUE(voices.AdmitTimelineQuantum(static_cast<std::uint32_t>(required),
        1, &decisions, &unclaimed, 1, controlTargets.data(),
        static_cast<std::uint32_t>(controlTargets.size())));

    harken::Plan03PageGroupView view{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &view, 1));
    ASSERT_NE(nullptr, view.lease);
    ASSERT_TRUE(view.lease->TryClaim());
    EXPECT_FALSE(sink.AdmitQuantum(0));
}

TEST(DecisionGroup, FrozenPagePublishesExactDeduplicatedGroupSummary) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(2);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    const harken::ParentAudioGroupRef parent{{13, 17}};

    harken::TimelineArmToken first{};
    harken::TimelineArmToken second{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 100, 19, target, parent, 0, &first));
    ASSERT_TRUE(arms.ReserveGrouped(
        7, 11, 0, 100, 19, target, parent, 1, &second));

    const harken::TimelineArmToken reverseMembers[] = {second, first};
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = parent.decision;
    input.sliceIndex = 0;
    input.members = reverseMembers;
    input.memberCount = 2;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));

    harken::Plan03PageGroupView unavailable{};
    unavailable.identity.absoluteQuantum = 99;
    EXPECT_EQ(0u, pool.CapturePlan03PageGroupsRt(0, &unavailable, 1));
    EXPECT_EQ(99u, unavailable.identity.absoluteQuantum);

    pool.Publish(100, nullptr, nullptr, &arms);
    harken::Plan03PageGroupView view{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &view, 1));
    EXPECT_EQ(0u, view.identity.absoluteQuantum);
    EXPECT_EQ(1u, view.identity.pageGeneration);
    EXPECT_EQ(13u, view.identity.decision.slot);
    EXPECT_EQ(17u, view.identity.decision.generation);
    EXPECT_EQ(0u, view.identity.sliceIndex);
    ASSERT_EQ(2u, view.memberCount);
    ASSERT_NE(nullptr, view.members);
    EXPECT_EQ(first.slot, view.members[0].slot);
    EXPECT_EQ(second.slot, view.members[1].slot);
    ASSERT_NE(nullptr, view.lease);
    EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
}

TEST(DecisionGroup, MemberlessQueueSliceRetainsIdentityAndLease) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = {13, 17};

    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));
    pool.Publish(100, nullptr, nullptr, &arms);

    harken::Plan03PageGroupView view{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &view, 1));
    EXPECT_EQ(13u, view.identity.decision.slot);
    EXPECT_EQ(17u, view.identity.decision.generation);
    EXPECT_EQ(0u, view.memberCount);
    EXPECT_EQ(nullptr, view.members);
    ASSERT_NE(nullptr, view.lease);
    EXPECT_EQ(harken::AudioSliceState::Ready,
              view.lease->Capture().state);
}

TEST(DecisionGroup, MemberlessReaderPinSurvivesConsumerAdvanceUntilCompletedBlockFence) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    harken::Plan03GroupSliceInput input{};
    input.decision = {13, 17};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.ReservePlan03GroupSlice(input, arms));
    harken::Plan03PageGroupView view{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&view, 1));
    ASSERT_TRUE(view.identity.Exact());
    EXPECT_EQ(3u, view.identity.transportEpoch);
    harken::Plan03GroupReaderPin pin;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(view.identity, &pin));
    const auto storageBytes = pool.Plan03GroupReaderStorageBytes();
    EXPECT_GT(storageBytes, 0u);
    pool.Publish(100, nullptr, nullptr, &arms);
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(view.identity, 7));
    pool.ConsumerFrontier().store(1, std::memory_order_release);
    EXPECT_FALSE(pin.Release(8));
    ASSERT_TRUE(view.lease->TryClaim());
    EXPECT_FALSE(pin.Release(8));
    ASSERT_TRUE(view.lease->MarkApplied());
    EXPECT_FALSE(pin.Release(7));
    EXPECT_FALSE(pool.CanResetForEpoch(0, 5));
    EXPECT_FALSE(pool.Reset(MakePoolConfig(), 0, 5));
    pool.DropPreparedOccupancy();
    pool.Advance();
    EXPECT_EQ(0u, pool.WindowStartFrame());
    harken::Plan03PageGroupView retained{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(view.identity, &retained));
    EXPECT_EQ(view.lease, retained.lease);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, retained.lease->Capture().state);
    ASSERT_TRUE(pin.Release(8));
    EXPECT_FALSE(pin.Valid());
    EXPECT_TRUE(pin.Release(8));
    pool.Advance();
    EXPECT_EQ(100u, pool.WindowStartFrame());
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(view.identity, &retained));
    EXPECT_FALSE(pool.NotePlan03GroupReadRt(view.identity, 8));
    EXPECT_EQ(storageBytes, pool.Plan03GroupReaderStorageBytes());
}

TEST(DecisionGroup, ReaderPinRejectsRollbackAndReplacementWithoutLosingExactRow) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    harken::Plan03GroupSliceInput input{};
    input.decision = {13, 17};
    harken::PreparedPlan03GroupSlice prepared;
    harken::CommittedPlan03GroupSlice committed;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSlice(input, arms, &prepared));
    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&prepared, &committed));
    harken::Plan03PageGroupView original{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&original, 1));
    harken::Plan03GroupReaderPin pin;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(original.identity, &pin));
    EXPECT_FALSE(pool.RollbackCommittedPlan03GroupSlice(&committed));
    EXPECT_TRUE(committed.Valid());
    harken::PreparedPlan03GroupSliceReplacement replacement;
    EXPECT_NE(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSliceReplacement(original.identity, input, arms, &replacement));
    EXPECT_FALSE(replacement.Valid());
    auto moved = std::move(pin);
    EXPECT_FALSE(pin.Valid());
    ASSERT_TRUE(moved.Release(0));
    ASSERT_TRUE(pool.RollbackCommittedPlan03GroupSlice(&committed));
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.ReservePlan03GroupSlice(input, arms));
    harken::Plan03PageGroupView current{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&current, 1));
    EXPECT_EQ(original.identity.committedRow, current.identity.committedRow);
    EXPECT_NE(original.identity.rowClaimGeneration, current.identity.rowClaimGeneration);
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(original.identity, &current));
    EXPECT_FALSE(pool.AcquirePlan03GroupReader(original.identity, &pin));
}

TEST(DecisionGroup, ReusedGroupRowDoesNotInheritPriorAudioReadSequence) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    harken::Plan03GroupSliceInput input{};
    input.decision = {13, 17};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));
    harken::Plan03PageGroupView oldGroup{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&oldGroup, 1));
    harken::Plan03GroupReaderPin pin;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(oldGroup.identity, &pin));
    pool.Publish(100, nullptr, nullptr, &arms);
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(oldGroup.identity, 5));
    ASSERT_TRUE(oldGroup.lease->TryClaim());
    ASSERT_TRUE(oldGroup.lease->MarkApplied());
    ASSERT_TRUE(pin.Release(6));
    pool.ConsumerFrontier().store(1, std::memory_order_release);
    pool.Advance();
    ASSERT_EQ(100u, pool.WindowStartFrame());

    input.executeFrame = 400;
    input.decision = {19, 23};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));
    std::array<harken::Plan03PageGroupView, 4> groups{};
    const auto count = pool.CaptureLivePlan03Groups(groups.data(), groups.size());
    ASSERT_EQ(1u, count);
    const auto& reused = groups[0];
    EXPECT_EQ(oldGroup.identity.committedRow, reused.identity.committedRow);
    EXPECT_NE(oldGroup.identity.pageGeneration,
              reused.identity.pageGeneration);

    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = reused.identity.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.group = reused.identity;
    binding.operation.transport = 3;
    binding.operation.world = 7;
    binding.primary = reused.identity;
    EXPECT_TRUE(pool.BindPlan03GroupFrontier(reused.identity, binding));
}

TEST(DecisionGroup,
     PreparedSliceOwnsFinalRowAndCopiesMembersBeforeCommit) {
    static_assert(!std::is_copy_constructible_v<
                  harken::PreparedPlan03GroupSlice>);
    static_assert(!std::is_copy_assignable_v<
                  harken::PreparedPlan03GroupSlice>);
    static_assert(std::is_move_constructible_v<
                  harken::PreparedPlan03GroupSlice>);

    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    const harken::DecisionToken decision{13, 17};
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    harken::TimelineArmToken member{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 100, 19, target, {decision}, 0, &member));
    const harken::TimelineArmToken expectedMember = member;

    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = decision;
    input.members = &member;
    input.memberCount = 1;
    harken::PreparedPlan03GroupSlice prepared{};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreparePlan03GroupSlice(input, arms, &prepared));
    ASSERT_TRUE(prepared.Valid());

    const harken::Plan03GroupSliceIdentity identity{0, 1, decision, 0};
    harken::Plan03PageGroupView hidden{};
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(identity, &hidden));
    EXPECT_FALSE(pool.CanResetForEpoch(0, 5));
    member = {};

    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(pool.CommitPreparedPlan03GroupSlice(&prepared));
    EXPECT_FALSE(pool.CancelPreparedPlan03GroupSlice(&prepared));
    harken::Plan03PageGroupView committed{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(identity, &committed));
    ASSERT_EQ(1u, committed.memberCount);
    ASSERT_NE(nullptr, committed.members);
    EXPECT_EQ(expectedMember.slot, committed.members[0].slot);
    EXPECT_EQ(expectedMember.generation, committed.members[0].generation);
}

TEST(DecisionGroup,
     PreparedSliceCancelRestoresCapacityAndWindowProgress) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable firstArms(1);
    harken::Plan03GroupSliceInput first{};
    first.executeFrame = 0;
    first.decision = {13, 17};
    harken::PreparedPlan03GroupSlice prepared{};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreparePlan03GroupSlice(
                  first, firstArms, &prepared));

    harken::Plan03GroupSliceInput refused = first;
    refused.decision = {19, 23};
    EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
              pool.PreflightPlan03GroupSlices(0, 1));
    EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
              pool.ReservePlan03GroupSlice(refused, firstArms));
    EXPECT_EQ(0u, pool.WindowStartQuantum());
    EXPECT_EQ(0u, pool.Advance());
    EXPECT_EQ(0u, pool.WindowStartQuantum());

    harken::PreparedPlan03GroupSlice moved(std::move(prepared));
    EXPECT_FALSE(prepared.Valid());
    ASSERT_TRUE(moved.Valid());
    ASSERT_TRUE(pool.CancelPreparedPlan03GroupSlice(&moved));
    EXPECT_FALSE(moved.Valid());
    EXPECT_FALSE(pool.CancelPreparedPlan03GroupSlice(&moved));
    EXPECT_TRUE(pool.CanResetForEpoch(0, 5));

    harken::TimelineArmTable secondArms(1);
    EXPECT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(refused, secondArms));
}

TEST(DecisionGroup,
     PreparedSlicesCommitOutOfOrderWithoutReacquiringCapacity) {
    auto config = MakePoolConfig();
    config.maxPlan03GroupHeadersPerPage = 2;
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(config, 0, 3));
    harken::TimelineArmTable arms(1);

    harken::Plan03GroupSliceInput first{};
    first.executeFrame = 0;
    first.decision = {13, 17};
    harken::Plan03GroupSliceInput second = first;
    second.decision = {19, 23};
    second.sliceIndex = 1;
    harken::PreparedPlan03GroupSlice firstPrepared{};
    harken::PreparedPlan03GroupSlice secondPrepared{};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreparePlan03GroupSlice(
                  first, arms, &firstPrepared));
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreparePlan03GroupSlice(
                  second, arms, &secondPrepared));
    EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
              pool.PreflightPlan03GroupSlices(0, 1));

    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&secondPrepared));
    harken::Plan03PageGroupView secondView{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(
        {0, 1, second.decision, second.sliceIndex}, &secondView));
    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&firstPrepared));
    harken::Plan03PageGroupView firstView{};
    EXPECT_TRUE(pool.ResolvePlan03GroupSliceProducer(
        {0, 1, first.decision, first.sliceIndex}, &firstView));
}

TEST(DecisionGroup, EmptyGroupPublicationHoldSurvivesPromotionAndReleasesOnlyExactOwner) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    harken::Plan03GroupSliceInput input{};
    input.decision = {13, 17};
    harken::PreparedPlan03GroupSlice first, second;
    harken::CommittedPlan03GroupSlice firstCommitted, secondCommitted;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSlice(input, arms, &first));
    input.executeFrame = 100;
    input.decision = {19, 23};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSlice(input, arms, &second));
    pool.Publish(200, nullptr, nullptr, &arms);
    EXPECT_EQ(0u, pool.PublishCutoffFrame());
    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&first, &firstCommitted, true));
    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&second, &secondCommitted, true));
    pool.Publish(200, nullptr, nullptr, &arms);
    EXPECT_EQ(harken::kUnpublishedQuantumStamp, pool.PageStamp(0));
    EXPECT_EQ(harken::kUnpublishedQuantumStamp, pool.PageStamp(1));
    EXPECT_FALSE(pool.Reset(MakePoolConfig(), 0, 4));
    EXPECT_FALSE(pool.ResetForEpoch(0, 4));
    pool.ConsumerFrontier().store(2);
    pool.Advance();
    EXPECT_EQ(0u, pool.WindowStartFrame());
    pool.DropPreparedOccupancy();
    std::array<harken::Plan03PageGroupView, 2> views{};
    ASSERT_EQ(2u, pool.CaptureLivePlan03Groups(views.data(), views.size()));
    harken::PreparedPlan03GroupSliceReplacement replacement;
    EXPECT_NE(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSliceReplacement(views[1].identity, input, arms, &replacement));
    ASSERT_TRUE(pool.ReleaseCommittedPlan03GroupSliceOwnership(&firstCommitted));
    EXPECT_FALSE(pool.RollbackCommittedPlan03GroupSlice(&firstCommitted));
    pool.Publish(200, nullptr, nullptr, &arms);
    EXPECT_EQ(0u, pool.PageStamp(0));
    EXPECT_EQ(harken::kUnpublishedQuantumStamp, pool.PageStamp(1));
    EXPECT_EQ(100u, pool.PublishCutoffFrame());
    auto moved = std::move(secondCommitted);
    EXPECT_FALSE(secondCommitted.Valid());
    ASSERT_TRUE(pool.RollbackCommittedPlan03GroupSlice(&moved));
    pool.Publish(200, nullptr, nullptr, &arms);
    EXPECT_EQ(1u, pool.PageStamp(1));
    EXPECT_EQ(0u, pool.CapturePlan03PageGroupsRt(1, views.data(), views.size()));
    EXPECT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, views.data(), views.size()));
}

TEST(DecisionGroup,
     UnpublishedProducerSliceCanBeReplacedWithoutConsumerStamp) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    const harken::DecisionToken original{13, 17};
    const harken::DecisionToken replacementDecision{23, 29};
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = original;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));

    const harken::Plan03GroupSliceIdentity originalIdentity{
        0, 1, original, 0};
    harken::Plan03PageGroupView originalView{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(originalIdentity,
                                                     &originalView));
    ASSERT_NE(nullptr, originalView.lease);
    EXPECT_EQ(harken::AudioSliceState::Ready,
              originalView.lease->Capture().state);

    harken::Plan03GroupSliceInput replacement = input;
    replacement.decision = replacementDecision;
    harken::PreparedPlan03GroupSliceReplacement prepared{};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.PreparePlan03GroupSliceReplacement(
                  originalIdentity, replacement, arms, &prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(harken::AudioSliceState::ReplacementReserved,
              originalView.lease->Capture().state);
    ASSERT_TRUE(pool.CommitPreparedPlan03GroupSliceReplacement(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(originalIdentity,
                                                      &originalView));

    const harken::Plan03GroupSliceIdentity replacementIdentity{
        0, 1, replacementDecision, 0};
    harken::Plan03PageGroupView replacementView{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(replacementIdentity,
                                                     &replacementView));
    ASSERT_NE(nullptr, replacementView.lease);
    EXPECT_EQ(harken::AudioSliceState::Ready,
              replacementView.lease->Capture().state);
    pool.Publish(100, nullptr, nullptr, &arms);
    harken::Plan03PageGroupView published{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &published, 1));
    EXPECT_EQ(replacementDecision.slot,
              published.identity.decision.slot);
    EXPECT_EQ(replacementDecision.generation,
              published.identity.decision.generation);
}

TEST(DecisionGroup,
     ProducerResolveRejectsMixedIdentityWithoutTouchingClaimedSibling) {
    harken::ScheduledTransactionPool pool;
    harken::ScheduledTransactionPool::Config config = MakePoolConfig();
    config.maxPlan03GroupHeadersPerPage = 2;
    ASSERT_TRUE(pool.Reset(config, 0, 3));
    harken::TimelineArmTable arms(2);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    const harken::ParentAudioGroupRef firstParent{{13, 17}};
    const harken::ParentAudioGroupRef secondParent{{19, 23}};

    harken::TimelineArmToken firstArm{};
    harken::TimelineArmToken secondArm{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 100, 29, target, firstParent, 0, &firstArm));
    ASSERT_TRUE(arms.ReserveGrouped(
        7, 11, 0, 100, 31, target, secondParent, 0, &secondArm));
    harken::Plan03GroupSliceInput first{};
    first.executeFrame = 0;
    first.decision = firstParent.decision;
    first.sliceIndex = 0;
    first.members = &firstArm;
    first.memberCount = 1;
    harken::Plan03GroupSliceInput second = first;
    second.decision = secondParent.decision;
    second.sliceIndex = 1;
    second.members = &secondArm;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(first, arms));
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(second, arms));

    const harken::Plan03GroupSliceIdentity firstIdentity{
        0, 1, firstParent.decision, 0};
    const harken::Plan03GroupSliceIdentity secondIdentity{
        0, 1, secondParent.decision, 1};
    harken::Plan03PageGroupView exact{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(firstIdentity, &exact));
    ASSERT_NE(nullptr, exact.lease);
    const harken::AudioSliceLeaseSnapshot firstBefore =
        exact.lease->Capture();
    harken::Plan03PageGroupView sibling{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(secondIdentity,
                                                     &sibling));
    ASSERT_NE(nullptr, sibling.lease);
    ASSERT_TRUE(sibling.lease->TryClaim());
    const harken::AudioSliceLeaseSnapshot secondBefore =
        sibling.lease->Capture();
    ASSERT_EQ(harken::AudioSliceState::SliceClaimed, secondBefore.state);

    harken::Plan03GroupSliceIdentity mixed = firstIdentity;
    mixed.sliceIndex = secondIdentity.sliceIndex;
    harken::Plan03PageGroupView rejected{};
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(mixed, &rejected));
    EXPECT_EQ(firstBefore.state, exact.lease->Capture().state);
    EXPECT_EQ(firstBefore.pendingReasonBits,
              exact.lease->Capture().pendingReasonBits);
    EXPECT_EQ(secondBefore.state, sibling.lease->Capture().state);
    EXPECT_EQ(secondBefore.pendingReasonBits,
              sibling.lease->Capture().pendingReasonBits);
}

TEST(DecisionGroup, PageSummaryAcceptsSparseGlobalMemberIndices) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(2);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    const harken::ParentAudioGroupRef parent{{13, 17}};

    harken::TimelineArmToken first{};
    harken::TimelineArmToken second{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 100, 19, target, parent, 0, &first));
    ASSERT_TRUE(arms.ReserveGrouped(
        7, 11, 0, 100, 19, target, parent, 1, &second));

    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = parent.decision;
    input.sliceIndex = 0;
    input.members = &second;
    input.memberCount = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));

    pool.Publish(100, nullptr, nullptr, &arms);
    harken::Plan03PageGroupView view{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &view, 1));
    ASSERT_EQ(1u, view.memberCount);
    EXPECT_EQ(second.slot, view.members[0].slot);
    EXPECT_EQ(second.generation, view.members[0].generation);
}

TEST(DecisionGroup, PageGroupCapacityAndIdentityFailuresAreZeroPartial) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(2);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);

    harken::TimelineArmToken member{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 100, 19, target, {{13, 17}}, 0, &member));
    harken::Plan03GroupSliceInput valid{};
    valid.executeFrame = 0;
    valid.decision = {13, 17};
    valid.sliceIndex = 0;
    valid.members = &member;
    valid.memberCount = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(valid, arms));

    EXPECT_EQ(harken::Plan03GroupReserveStatus::DuplicateSlice,
              pool.ReservePlan03GroupSlice(valid, arms));

    auto second = valid;
    second.sliceIndex = 1;
    EXPECT_EQ(harken::Plan03GroupReserveStatus::PageGroupCapacity,
              pool.ReservePlan03GroupSlice(second, arms));
    auto wrongParent = valid;
    wrongParent.decision = {23, 29};
    EXPECT_EQ(harken::Plan03GroupReserveStatus::ArmIdentityMismatch,
              pool.ReservePlan03GroupSlice(wrongParent, arms));

    pool.Publish(100, nullptr, nullptr, &arms);
    harken::Plan03PageGroupView view{};
    EXPECT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &view, 1));
    EXPECT_EQ(0u, view.identity.sliceIndex);
}

TEST(DecisionGroup, PageCannotReclaimReadyOrClaimedSliceAndRejectsStaleView) {
    harken::ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, 3));
    harken::TimelineArmTable arms(1);
    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7;
    target.subjectId = harkentest::ObjectId(11);
    harken::TimelineArmToken member{};
    ASSERT_TRUE(arms.ReserveGrouped(
        3, 5, 0, 100, 19, target, {{13, 17}}, 0, &member));

    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 0;
    input.decision = {13, 17};
    input.members = &member;
    input.memberCount = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
              pool.ReservePlan03GroupSlice(input, arms));
    pool.Publish(100, nullptr, nullptr, &arms);
    harken::Plan03PageGroupView view{};
    ASSERT_EQ(1u, pool.CapturePlan03PageGroupsRt(0, &view, 1));
    const auto oldIdentity = view.identity;

    EXPECT_EQ(0u, pool.Advance());
    EXPECT_EQ(0u, pool.WindowStartQuantum());
    ASSERT_TRUE(view.lease->TryClaim());
    EXPECT_EQ(0u, pool.Advance());
    EXPECT_EQ(0u, pool.WindowStartQuantum());
    ASSERT_TRUE(view.lease->MarkApplied());
    EXPECT_EQ(0u, pool.Advance());
    EXPECT_EQ(0u, pool.WindowStartQuantum());
    ASSERT_TRUE(arms.RequestCancel(member));
    ASSERT_TRUE(arms.Tombstone(member, 20));
    ASSERT_TRUE(arms.Reclaim(member, 21));
    EXPECT_EQ(0u, pool.Advance());
    EXPECT_EQ(1u, pool.WindowStartQuantum());

    harken::Plan03PageGroupView stale{};
    stale.identity.absoluteQuantum = 71;
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceRt(oldIdentity, &stale));
    EXPECT_EQ(71u, stale.identity.absoluteQuantum);
}

TEST(ArmGroup, PreparedMembersRequireExactIdentityAndCompleteOrderedCensus) {
    harken::ArmGroupTable groups(1, 2);
    const harken::DecisionToken parent{29, 31};
    const std::array<harken::TimelineArmToken, 2> members{{{3, 5, 7, 11}, {13, 17, 19, 23}}};
    harken::ArmGroupToken token{};
    ASSERT_TRUE(groups.Prepare(parent, members.data(), 2, &token));
    const auto& view = groups;
    ASSERT_TRUE(view.MatchesPreparedMembers(token, parent, members.data(), 2));
    EXPECT_EQ(0u, groups.LiveCount());
    harken::ArmGroupSnapshot snapshot{};
    EXPECT_FALSE(view.Capture(token, &snapshot));
    EXPECT_FALSE(view.MatchesPreparedMembers({token.slot, token.generation + 1u}, parent, members.data(), 2));
    EXPECT_FALSE(view.MatchesPreparedMembers(token, {parent.slot, parent.generation + 1u}, members.data(), 2));
    EXPECT_FALSE(view.MatchesPreparedMembers(token, parent, members.data(), 1));
    EXPECT_FALSE(view.MatchesPreparedMembers(token, parent, nullptr, 2));
    auto wrong = members;
    std::swap(wrong[0], wrong[1]);
    EXPECT_FALSE(view.MatchesPreparedMembers(token, parent, wrong.data(), 2));
    wrong = members;
    ++wrong[1].renderGeneration;
    EXPECT_FALSE(view.MatchesPreparedMembers(token, parent, wrong.data(), 2));
    EXPECT_TRUE(view.MatchesPreparedMembers(token, parent, members.data(), 2));
    ASSERT_TRUE(groups.CancelPrepared(token));
    EXPECT_FALSE(view.MatchesPreparedMembers(token, parent, members.data(), 2));
    harken::ArmGroupToken reused{};
    ASSERT_TRUE(groups.Prepare(parent, members.data(), 2, &reused));
    EXPECT_EQ(token.slot, reused.slot);
    EXPECT_NE(token.generation, reused.generation);
    EXPECT_FALSE(view.MatchesPreparedMembers(token, parent, members.data(), 2));
    EXPECT_TRUE(view.MatchesPreparedMembers(reused, parent, members.data(), 2));
    ASSERT_TRUE(groups.CommitPrepared(reused));
    EXPECT_FALSE(view.MatchesPreparedMembers(reused, parent, members.data(), 2));
    EXPECT_TRUE(view.Capture(reused, &snapshot));

    harken::ArmGroupTable empty(1, 1);
    ASSERT_TRUE(empty.Prepare(parent, nullptr, 0, &token));
    EXPECT_TRUE(empty.MatchesPreparedMembers(token, parent, nullptr, 0));
    EXPECT_FALSE(empty.MatchesPreparedMembers(token, parent, members.data(), 1));
    EXPECT_TRUE(empty.CancelPrepared(token));
}

TEST(ArmGroup, ExactMembersBecomeReadyOnlyAfterEveryUniqueAck) {
    harken::ArmGroupTable groups(1, 2);
    const harken::TimelineArmToken members[] = {
        {3, 5, 7, 11},
        {13, 17, 19, 23},
    };
    harken::ArmGroupToken token{};
    ASSERT_TRUE(groups.Reserve({29, 31}, members, 2, &token));

    harken::ArmGroupSnapshot snapshot{};
    ASSERT_TRUE(groups.Capture(token, &snapshot));
    EXPECT_EQ(harken::ArmGroupState::AwaitingAcks, snapshot.state);
    EXPECT_EQ(0u, snapshot.ackedCount);
    EXPECT_EQ(2u, snapshot.memberCount);
    EXPECT_FALSE(groups.Acknowledge(token, {3, 4, 7, 11}, 37));
    EXPECT_TRUE(groups.Acknowledge(token, members[1], 41));
    EXPECT_FALSE(groups.Acknowledge(token, members[1], 43));
    ASSERT_TRUE(groups.Capture(token, &snapshot));
    EXPECT_EQ(1u, snapshot.ackedCount);
    EXPECT_EQ(harken::ArmGroupState::AwaitingAcks, snapshot.state);
    EXPECT_TRUE(groups.Acknowledge(token, members[0], 47));
    ASSERT_TRUE(groups.Capture(token, &snapshot));
    EXPECT_EQ(2u, snapshot.ackedCount);
    EXPECT_EQ(harken::ArmGroupState::Ready, snapshot.state);
}

TEST(ArmGroup, EmptyGroupIsReadyAndReclaimsAfterAudioFence) {
    harken::ArmGroupTable groups(1, 2);
    harken::ArmGroupToken token{};
    ASSERT_TRUE(groups.Reserve({29, 31}, nullptr, 0, &token));

    harken::ArmGroupSnapshot snapshot{};
    ASSERT_TRUE(groups.Capture(token, &snapshot));
    EXPECT_EQ(harken::ArmGroupState::Ready, snapshot.state);
    EXPECT_EQ(0u, snapshot.memberCount);
    EXPECT_EQ(0u, snapshot.ackedCount);
    EXPECT_EQ(0u, snapshot.tombstonedCount);

    ASSERT_TRUE(groups.BeginRetire(token));
    EXPECT_FALSE(groups.Reclaim(token, 0));
    EXPECT_TRUE(groups.Reclaim(token, 1));
    EXPECT_EQ(0u, groups.LiveCount());
}

TEST(ArmGroup, MemberObservationRetainsExactTombstoneAcrossArmAndGroupReuse) {
    harken::ArmGroupTable groups(1, 2);
    const harken::TimelineArmToken members[] = {{3, 5, 7, 11}, {13, 17, 19, 23}};
    harken::ArmGroupToken first{};
    ASSERT_TRUE(groups.Reserve({29, 31}, members, 2, &first));
    harken::ArmGroupMemberSnapshot snapshot{};
    ASSERT_TRUE(groups.CaptureMember(first, 0, &snapshot));
    EXPECT_EQ(members[0].generation, snapshot.token.generation);
    EXPECT_EQ(members[0].renderGeneration, snapshot.token.renderGeneration);
    EXPECT_FALSE(snapshot.acked);
    EXPECT_FALSE(snapshot.tombstoned);
    ASSERT_TRUE(groups.Acknowledge(first, members[0], 37));
    ASSERT_TRUE(groups.Acknowledge(first, members[1], 41));
    ASSERT_TRUE(groups.BeginRetire(first));
    ASSERT_TRUE(groups.Tombstone(first, members[0], 43));
    auto reusedArm = members[0];
    ++reusedArm.generation;
    ++reusedArm.renderGeneration;
    EXPECT_FALSE(groups.Tombstone(first, reusedArm, 47));
    ASSERT_TRUE(groups.CaptureMember(first, 0, &snapshot));
    EXPECT_TRUE(snapshot.acked);
    EXPECT_TRUE(snapshot.tombstoned);
    EXPECT_EQ(members[0].generation, snapshot.token.generation);
    EXPECT_FALSE(groups.CaptureMember(first, 2, &snapshot));
    EXPECT_TRUE(snapshot.tombstoned);
    ASSERT_TRUE(groups.Tombstone(first, members[1], 47));
    ASSERT_TRUE(groups.Reclaim(first, 48));
    harken::ArmGroupToken second{};
    ASSERT_TRUE(groups.Reserve({53, 59}, &reusedArm, 1, &second));
    EXPECT_EQ(first.slot, second.slot);
    EXPECT_FALSE(groups.CaptureMember(first, 0, &snapshot));
    EXPECT_TRUE(snapshot.tombstoned);
    ASSERT_TRUE(groups.CaptureMember(second, 0, &snapshot));
    EXPECT_EQ(reusedArm.generation, snapshot.token.generation);
    EXPECT_FALSE(snapshot.acked);
    EXPECT_FALSE(snapshot.tombstoned);
}

TEST(ArmGroup, PublishedMemberTombstoneRequiresOriginalIdentityAndCompletedBlock) {
    harken::ArmGroupTable groups(1, 2);
    const harken::DecisionToken decision{3, 7};
    const harken::TimelineArmToken members[] = {{4, 9, 1, 2}, {5, 8, 2, 3}};
    harken::ArmGroupToken group{};
    ASSERT_TRUE(groups.Reserve(decision, members, 2, &group));
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, members[0], 30));
    ASSERT_TRUE(groups.BeginRetire(group));
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, members[0], 30));
    ASSERT_TRUE(groups.Tombstone(group, members[0], 30));
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, members[0], 29));
    EXPECT_TRUE(groups.HasPublishedMemberTombstone(group, decision, members[0], 30));
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, members[1], 30));
    auto wrongGroup = group;
    ++wrongGroup.generation;
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(wrongGroup, decision, members[0], 30));
    auto wrongDecision = decision;
    ++wrongDecision.generation;
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, wrongDecision, members[0], 30));
    auto wrongArm = members[0];
    ++wrongArm.generation;
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, wrongArm, 30));
    wrongArm = members[0];
    ++wrongArm.renderGeneration;
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, wrongArm, 30));
    ASSERT_TRUE(groups.Tombstone(group, members[1], 31));
    ASSERT_TRUE(groups.Reclaim(group, 32));
    EXPECT_FALSE(groups.HasPublishedMemberTombstone(group, decision, members[0], 32));
}

TEST(ArmGroup, ReclaimRequiresAllTombstonesAndStrictAudioFence) {
    harken::ArmGroupTable groups(1, 2);
    const harken::TimelineArmToken members[] = {
        {3, 5, 7, 11},
        {13, 17, 19, 23},
    };
    harken::ArmGroupToken first{};
    ASSERT_TRUE(groups.Reserve({29, 31}, members, 2, &first));
    ASSERT_TRUE(groups.Acknowledge(first, members[0], 37));
    ASSERT_TRUE(groups.Acknowledge(first, members[1], 41));
    ASSERT_TRUE(groups.BeginRetire(first));
    ASSERT_TRUE(groups.Tombstone(first, members[0], 43));
    EXPECT_FALSE(groups.Reclaim(first, 100));
    ASSERT_TRUE(groups.Tombstone(first, members[1], 47));
    EXPECT_FALSE(groups.Reclaim(first, 47));
    EXPECT_TRUE(groups.Reclaim(first, 48));

    harken::ArmGroupToken second{};
    ASSERT_TRUE(groups.Reserve({53, 59}, members, 2, &second));
    EXPECT_EQ(first.slot, second.slot);
    EXPECT_EQ(first.generation + 1, second.generation);
    EXPECT_FALSE(groups.BeginRetire(first));
}

TEST(ArmGroup, CapacityAndDuplicateRefusalsLeaveNoPartialGroup) {
    harken::ArmGroupTable groups(1, 2);
    const harken::TimelineArmToken duplicate[] = {
        {3, 5, 7, 11},
        {3, 5, 7, 11},
    };
    harken::ArmGroupToken unchanged{31, 37};
    EXPECT_FALSE(groups.Reserve({29, 31}, duplicate, 2, &unchanged));
    EXPECT_EQ(31u, unchanged.slot);
    EXPECT_EQ(37u, unchanged.generation);
    EXPECT_EQ(0u, groups.LiveCount());

    const harken::TimelineArmToken members[] = {
        {3, 5, 7, 11},
        {13, 17, 19, 23},
    };
    ASSERT_TRUE(groups.Reserve({29, 31}, members, 2, &unchanged));
    harken::ArmGroupToken refused{41, 43};
    EXPECT_FALSE(groups.Reserve({47, 53}, members, 2, &refused));
    EXPECT_EQ(41u, refused.slot);
    EXPECT_EQ(43u, refused.generation);
    EXPECT_EQ(1u, groups.LiveCount());
}

} // namespace

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken::detail {

struct ScheduledTransactionPoolOriginalTestAccess {
    static bool OriginalAssociationIs(const ScheduledTransactionPool& pool,
        const Plan03GroupSliceIdentity& identity,
        const PreparedPrivateImmediateLaneGroup* expected) noexcept {
        Plan03PageGroupView actual{};
        if (!pool.ResolvePlan03GroupSliceProducer(identity, &actual)) return false;
        return pool.immediateLaneGroups_[identity.committedRow].transferOriginal == expected;
    }

    static bool SeedFreeGeneration(ScheduledTransactionPool& pool,
                                  std::uint32_t rowIndex,
                                  std::uint32_t generation) noexcept {
        if (pool.immediateLaneGroups_ == nullptr ||
            rowIndex >= pool.config_.maxImmediateLaneGroupRows ||
            generation != UINT32_MAX - 1u) return false;
        auto& row = pool.immediateLaneGroups_[rowIndex];
        if (row.state.load(std::memory_order_acquire) !=
                ScheduledTransactionPool::LaneGroupState::Free ||
            row.originalRegistrationPinCount != 0 || row.originalFactoryMarker != 0 ||
            row.transferOriginal != nullptr || row.transferProof != nullptr ||
            row.readers.pins.load(std::memory_order_acquire) != 0 ||
            row.lease.Capture().state != AudioSliceState::Ready) return false;
        row.generation = generation;
        return true;
    }

    static bool SeedLastMarkerPrerequisite(ScheduledTransactionPool& pool) noexcept {
        if (pool.nextOriginalLaneGroupFactoryMarker_ != 0 ||
            pool.immediateLaneGroups_ == nullptr) return false;
        for (std::uint32_t index = 0; index < pool.config_.maxImmediateLaneGroupRows; ++index) {
            const auto& row = pool.immediateLaneGroups_[index];
            if (row.state.load(std::memory_order_acquire) !=
                    ScheduledTransactionPool::LaneGroupState::Free ||
                row.originalFactoryMarker != 0 || row.originalRegistrationPinCount != 0 ||
                row.transferOriginal != nullptr || row.transferProof != nullptr) return false;
        }
        pool.nextOriginalLaneGroupFactoryMarker_ = UINT64_MAX - 1u;
        return true;
    }

    static std::uint32_t Generation(const ScheduledTransactionPool& pool,
                                    std::uint32_t index) noexcept {
        return pool.immediateLaneGroups_[index].generation;
    }
    static std::uint64_t MarkerBudget(const ScheduledTransactionPool& pool) noexcept {
        return pool.nextOriginalLaneGroupFactoryMarker_;
    }
    static std::uint64_t RowMarker(const ScheduledTransactionPool& pool,
                                  std::uint32_t index) noexcept {
        return pool.immediateLaneGroups_[index].originalFactoryMarker;
    }
    static std::uint32_t PinCount(const ScheduledTransactionPool& pool,
                                 std::uint32_t index) noexcept {
        return pool.immediateLaneGroups_[index].originalRegistrationPinCount;
    }

    static bool EnterCountLimit(ScheduledTransactionPool& pool,
        const OriginalQueueFutureGroupRegistration& actual,
        std::uint32_t* genuineCount) noexcept {
        const auto identity = actual.Identity();
        if (genuineCount == nullptr ||
            !pool.IsOriginalQueueFutureGroupRegistrationFor(actual, identity)) return false;
        auto& row = pool.immediateLaneGroups_[identity.committedRow];
        // Only the existing real holder count is temporarily exhausted.
        if (row.originalRegistrationPinCount == 0 ||
            row.originalRegistrationPinCount == UINT32_MAX) return false;
        *genuineCount = row.originalRegistrationPinCount;
        row.originalRegistrationPinCount = UINT32_MAX;
        return true;
    }

    static bool LeaveCountLimit(ScheduledTransactionPool& pool,
        const OriginalQueueFutureGroupRegistration& actual,
        std::uint32_t genuineCount) noexcept {
        const auto identity = actual.Identity();
        if (genuineCount == 0 || genuineCount == UINT32_MAX ||
            !pool.IsOriginalQueueFutureGroupRegistrationFor(actual, identity)) return false;
        auto& row = pool.immediateLaneGroups_[identity.committedRow];
        if (row.originalRegistrationPinCount != UINT32_MAX) return false;
        row.originalRegistrationPinCount = genuineCount;
        return true;
    }
};

} // namespace harken::detail
#endif

namespace {

static_assert(!std::is_copy_constructible_v<harken::OriginalQueueFutureGroupRegistration>);
static_assert(!std::is_copy_constructible_v<harken::OriginalQueueFutureGroupLifetimePin>);
static_assert(!std::is_constructible_v<harken::OriginalQueueFutureGroupRegistration,
                                      harken::Plan03GroupSliceIdentity>);
static_assert(!std::is_constructible_v<harken::OriginalQueueFutureGroupLifetimePin,
                                      harken::Plan03GroupSliceIdentity>);

bool ConsumeOriginalQueueFuture(PrivateLaneTransferHarness& state, std::uint32_t index,
                        harken::OriginalQueueFutureGroupRegistration* out) {
    return state.Prove(index) &&
        state.pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(state.proofs[index]), out);
}

TEST(DecisionGroup, OriginalQueueFutureRegistrationConsumesActualFactoryAndProtectsOnlyStorage) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    const auto identities = state.raw;
    std::array<harken::OriginalQueueFutureGroupRegistration, 3> registrations;
    for (std::uint32_t index = 0; index < registrations.size(); ++index) {
        ASSERT_TRUE(ConsumeOriginalQueueFuture(state, index, &registrations[index]));
        EXPECT_FALSE(state.originals[index].Valid());
        EXPECT_FALSE(state.proofs[index].Valid());
        EXPECT_EQ(identities[index], registrations[index].Identity());
        EXPECT_TRUE(state.pool.IsOriginalQueueFutureGroupRegistrationFor(
            registrations[index], identities[index]));
        EXPECT_TRUE(registrations[index].MatchesOriginalScope(
            17, 100, state.inputs[index], state.arms));
        EXPECT_FALSE(state.pool.PrivateImmediateLaneGroupTransferHeld(identities[index]));
        EXPECT_TRUE(state.pool.PrivatePlan03GroupBindable(identities[index]));
        // A lifetime pin cannot make its own real tail-readiness predicate Busy.
        EXPECT_TRUE(state.pool.PrivateImmediateLaneGroupRetirementReady(identities[index], 0));
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, identities[index]));
    }
    EXPECT_FALSE(state.pool.CancelImmediateLaneGroups(17, 0));
    EXPECT_EQ(3u, state.pool.CaptureImmediateLaneCohortGroups(17, nullptr, 0));
    for (std::uint32_t index = 0; index < registrations.size(); ++index) {
        harken::Plan03PageGroupView view{};
        ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(identities[index], &view));
        EXPECT_EQ(identities[index], view.identity);
        EXPECT_EQ(state.inputs[index].memberCount, view.memberCount);
        EXPECT_EQ(state.inputs[index].sourceControlCount, view.sourceControlCount);
        EXPECT_EQ(harken::AudioSliceState::Ready, view.lease->Capture().state);
        if (index != 0) {
            EXPECT_EQ(state.members[index - 1].slot, view.members[0].slot);
            EXPECT_EQ(state.members[index - 1].generation, view.members[0].generation);
            EXPECT_EQ(state.members[index - 1].renderSlot, view.members[0].renderSlot);
            EXPECT_EQ(state.members[index - 1].renderGeneration, view.members[0].renderGeneration);
        }
        registrations[index].Release();
        registrations[index].Release();
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, identities[index]));
    }
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, OriginalQueueFutureIndependentLoansMovesAndDestructorsReturnTheirActualRows) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    std::array<harken::OriginalQueueFutureGroupRegistration, 3> registrations;
    for (std::uint32_t index = 0; index < registrations.size(); ++index)
        ASSERT_TRUE(ConsumeOriginalQueueFuture(state, index, &registrations[index]));
    harken::OriginalQueueFutureGroupLifetimePin first, second, descendant;
    ASSERT_TRUE(registrations[0].BorrowLifetime(&first));
    ASSERT_TRUE(registrations[0].BorrowLifetime(&second));
    registrations[0].Release();
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0]));
    first.Release();
    first.Release();
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0]));
    ASSERT_TRUE(second.Borrow(&descendant));
    harken::OriginalQueueFutureGroupLifetimePin moved(std::move(second));
    EXPECT_FALSE(second.Valid());
    EXPECT_EQ(state.raw[0], moved.Identity());
    {
        harken::OriginalQueueFutureGroupLifetimePin overwrite;
        ASSERT_TRUE(registrations[1].BorrowLifetime(&overwrite));
        registrations[1].Release();
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1]));
        overwrite = std::move(moved);
        EXPECT_FALSE(moved.Valid());
        EXPECT_EQ(state.raw[0], overwrite.Identity());
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1]));
        descendant.Release();
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0]));
        // Nonempty destruction returns the last actual row0 storage owner.
    }
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[0]));
    {
        harken::OriginalQueueFutureGroupRegistration movedRegistration(std::move(registrations[2]));
        EXPECT_FALSE(registrations[2].Valid());
        EXPECT_EQ(state.raw[2], movedRegistration.Identity());
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[2]));
    }
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[2]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, OriginalQueueFuturePinPermitsRealOldPublicationApplyAndGreaterReadFenceRetirement) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(1));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity raw{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &raw, &original));
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    harken::OriginalQueueFutureGroupLifetimePin lifetime;
    ASSERT_TRUE(registration.BorrowLifetime(&lifetime));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Bind(pool, &raw, 1));
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = raw.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = raw;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = raw;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw, binding));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(raw, &reader));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Publish(pool, arms, &raw, 1));
    harken::Plan03PageGroupView view{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(raw, &view));
    std::array<harken::Plan03PageGroupView, 1> due{};
    ASSERT_EQ(1u, pool.CaptureImmediateLaneGroupsRt(4, due.data(), due.size()));
    EXPECT_EQ(raw, due[0].identity);
    harken::ImmediateLaneFrontierClaim frontierClaim{};
    ASSERT_TRUE(pool.ClaimImmediateLaneFrontier(raw, &frontierClaim));
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(raw, 1));
    std::uint32_t applied = 0;
    SliceStep step{view.lease, &applied};
    harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
              harken::ApplyAudioSliceWhole(*view.lease, &operation, 1));
    EXPECT_EQ(1u, applied);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, view.lease->Capture().state);
    EXPECT_TRUE(registration.Valid());
    EXPECT_TRUE(lifetime.Valid());
    ASSERT_TRUE(pool.ReleaseImmediateLaneFrontier(&frontierClaim));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(raw, 1));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw, 2));
    EXPECT_FALSE(reader.Release(1));
    ASSERT_TRUE(reader.Release(2));
    registration.Release();
    EXPECT_TRUE(lifetime.Valid());
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 2));
    lifetime.Release();
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 1));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(raw, &view));
    ASSERT_TRUE(laneHarness.decisions.Cancel(laneHarness.tokens[0]));
    ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(laneHarness.tokens[0]));
    EXPECT_EQ(0u, laneHarness.decisions.LiveCount());
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, OriginalQueueFutureForeignEmptyRawAndNonemptyRefusalPreserveRealProofOwners) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    harken::OriginalQueueFutureGroupRegistration occupied, empty;
    ASSERT_TRUE(ConsumeOriginalQueueFuture(state, 0, &occupied));
    ASSERT_TRUE(state.Prove(1));
    const auto occupiedIdentity = occupied.Identity();
    harken::ScheduledTransactionPool foreign;
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 3;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(foreign.Reset(config, 0, 9));
    EXPECT_FALSE(foreign.ConsumePrivateImmediateLaneGroupTransfer(std::move(state.proofs[1]), &empty));
    EXPECT_FALSE(foreign.IsOriginalQueueFutureGroupRegistrationFor(occupied, occupiedIdentity));
    EXPECT_FALSE(state.pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(state.proofs[1]), &occupied));
    EXPECT_FALSE(state.pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(state.proofs[1]), nullptr));
    EXPECT_EQ(occupiedIdentity, occupied.Identity());
    EXPECT_FALSE(empty.Valid());
    EXPECT_TRUE(state.originals[1].Held());
    EXPECT_TRUE(state.proofs[1].Valid());
    EXPECT_TRUE(state.proofs[1].MatchesOriginalScope(
        state.originals[1], 17, 100, state.inputs[1], state.arms));
    harken::PrivateImmediateLaneGroupTransferProof noProof;
    EXPECT_FALSE(state.pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(noProof), &empty));
    ASSERT_TRUE(state.Prove(2));
    const auto rawOnly = harken::ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(
        std::move(state.proofs[2]));
    EXPECT_EQ(state.raw[2], rawOnly);
    EXPECT_FALSE(state.Prove(2));
    EXPECT_FALSE(state.pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(state.proofs[2]), &empty));
    EXPECT_FALSE(empty.Valid());
    ASSERT_TRUE(state.pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(state.proofs[1]), &empty));
    occupied.Release();
    empty.Release();
    for (const auto& identity : state.raw)
        ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, identity));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}

TEST(DecisionGroup, OriginalQueueFutureLifetimeSurvivesParentReturnAndRefusedPoolResetEpoch) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(ConsumeOriginalQueueFuture(state, 1, &registration));
    harken::OriginalQueueFutureGroupLifetimePin first, second;
    ASSERT_TRUE(registration.BorrowLifetime(&first));
    registration.Release();
    EXPECT_FALSE(registration.Valid());
    EXPECT_TRUE(first.Valid());
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 3;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    EXPECT_FALSE(state.pool.Reset(config, 0, 10));
    EXPECT_FALSE(state.pool.CanResetForEpoch(0, 10));
    EXPECT_FALSE(state.pool.ResetForEpoch(0, 10));
    EXPECT_EQ(state.raw[1], first.Identity());
    ASSERT_TRUE(first.Borrow(&second));
    first.Release();
    EXPECT_TRUE(second.Valid());
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1]));
    second.Release();
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[1]));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[0], &state.raw[0]));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[2], &state.raw[2]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
    ASSERT_TRUE(state.pool.Reset(config, 0, 11));
    EXPECT_TRUE(state.pool.CanResetForEpoch(0, 12));
    ASSERT_TRUE(state.pool.ResetForEpoch(0, 12));
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
using OriginalGroupAccess = harken::detail::ScheduledTransactionPoolOriginalTestAccess;

struct ActualOriginalGroupCountLimit {
    harken::ScheduledTransactionPool& pool;
    harken::OriginalQueueFutureGroupRegistration& registration;
    std::uint32_t genuineCount = 0;
    bool entered = false;
    ActualOriginalGroupCountLimit(harken::ScheduledTransactionPool& actualPool,
                         harken::OriginalQueueFutureGroupRegistration& actualRegistration)
        : pool(actualPool), registration(actualRegistration) {
        entered = OriginalGroupAccess::EnterCountLimit(pool, registration, &genuineCount);
    }
    ~ActualOriginalGroupCountLimit() {
        if (entered) EXPECT_TRUE(OriginalGroupAccess::LeaveCountLimit(pool, registration, genuineCount));
    }
};

TEST(DecisionGroup, OriginalQueueFutureLastNativeGenerationReturnsThenUsesSecondActualRow) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(2));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    ASSERT_TRUE(OriginalGroupAccess::SeedFreeGeneration(pool, 0, UINT32_MAX - 1u));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity first{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &first, &original));
    EXPECT_EQ(0u, first.committedRow);
    EXPECT_EQ(UINT32_MAX, first.rowClaimGeneration);
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    harken::OriginalQueueFutureGroupLifetimePin lifetime;
    ASSERT_TRUE(registration.BorrowLifetime(&lifetime));
    registration.Release();
    EXPECT_TRUE(lifetime.Valid());
    EXPECT_FALSE(pool.CancelPrivateImmediateLaneGroup(17, first));
    lifetime.Release();
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, first));
    const auto markerAfterFirst = OriginalGroupAccess::MarkerBudget(pool);
    EXPECT_EQ(UINT32_MAX, OriginalGroupAccess::Generation(pool, 0));
    EXPECT_EQ(0u, OriginalGroupAccess::RowMarker(pool, 0));
    EXPECT_EQ(0u, OriginalGroupAccess::PinCount(pool, 0));
    input.decision = laneHarness.tokens[1];
    harken::Plan03GroupSliceIdentity second{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(18, 100, input, arms, &second, &original));
    EXPECT_EQ(1u, second.committedRow);
    EXPECT_EQ(1u, second.rowClaimGeneration);
    EXPECT_EQ(UINT32_MAX, OriginalGroupAccess::Generation(pool, 0));
    EXPECT_EQ(markerAfterFirst + 1u, OriginalGroupAccess::MarkerBudget(pool));
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 18, 100, input, arms, &proof));
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    registration.Release();
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(18, second));
    ASSERT_TRUE(OriginalGroupAccess::SeedFreeGeneration(pool, 1, UINT32_MAX - 1u));
    harken::Plan03GroupSliceIdentity lastSecond{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(19, 100, input, arms, &lastSecond, &original));
    EXPECT_EQ(1u, lastSecond.committedRow);
    EXPECT_EQ(UINT32_MAX, lastSecond.rowClaimGeneration);
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 19, 100, input, arms, &proof));
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    registration.Release();
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(19, lastSecond));
    const auto exhaustedMarkerBudget = OriginalGroupAccess::MarkerBudget(pool);
    harken::Plan03GroupSliceIdentity refused{};
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(20, 100, input, arms, &refused, &original));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(UINT32_MAX, OriginalGroupAccess::Generation(pool, 0));
    EXPECT_EQ(UINT32_MAX, OriginalGroupAccess::Generation(pool, 1));
    EXPECT_EQ(exhaustedMarkerBudget, OriginalGroupAccess::MarkerBudget(pool));
    for (const auto& token : laneHarness.tokens) {
        ASSERT_TRUE(laneHarness.decisions.Cancel(token));
        ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(token));
    }
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, OriginalQueueFutureLastMarkerConsumesReturnsAndDoesNotPoisonRawReserveOrReset) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(2));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    ASSERT_TRUE(OriginalGroupAccess::SeedLastMarkerPrerequisite(pool));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity last{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &last, &original));
    EXPECT_EQ(UINT64_MAX, OriginalGroupAccess::MarkerBudget(pool));
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    harken::OriginalQueueFutureGroupLifetimePin lifetime;
    ASSERT_TRUE(registration.BorrowLifetime(&lifetime));
    registration.Release();
    input.decision = laneHarness.tokens[1];
    const auto generation0 = OriginalGroupAccess::Generation(pool, 0);
    const auto generation1 = OriginalGroupAccess::Generation(pool, 1);
    harken::Plan03GroupSliceIdentity refused{};
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(18, 100, input, arms, &refused, &original));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(generation0, OriginalGroupAccess::Generation(pool, 0));
    EXPECT_EQ(generation1, OriginalGroupAccess::Generation(pool, 1));
    EXPECT_EQ(UINT64_MAX, OriginalGroupAccess::MarkerBudget(pool));
    EXPECT_TRUE(lifetime.Valid());
    harken::Plan03GroupSliceIdentity rawOnly{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(18, 100, input, arms, &rawOnly));
    EXPECT_EQ(1u, rawOnly.committedRow);
    EXPECT_EQ(0u, OriginalGroupAccess::RowMarker(pool, rawOnly.committedRow));
    EXPECT_EQ(0u, OriginalGroupAccess::PinCount(pool, rawOnly.committedRow));
    lifetime.Release();
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(17, last));
    ASSERT_TRUE(pool.CancelPrivateImmediateLaneGroup(18, rawOnly));
    ASSERT_TRUE(pool.Reset(config, 0, 10));
    EXPECT_EQ(UINT64_MAX, OriginalGroupAccess::MarkerBudget(pool));
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(19, 100, input, arms, &refused, &original));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(0u, OriginalGroupAccess::Generation(pool, 0));
    EXPECT_EQ(0u, OriginalGroupAccess::Generation(pool, 1));
    ASSERT_TRUE(pool.ResetForEpoch(0, 11));
    EXPECT_EQ(UINT64_MAX, OriginalGroupAccess::MarkerBudget(pool));
    EXPECT_FALSE(pool.ReserveImmediateLaneGroup(19, 100, input, arms, &refused, &original));
    EXPECT_FALSE(refused.Valid());
    for (const auto& token : laneHarness.tokens) {
        ASSERT_TRUE(laneHarness.decisions.Cancel(token));
        ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(token));
    }
}

TEST(DecisionGroup, OriginalQueueFutureCountLimitAndExactScopeNegativesKeepGenuineOwnersReturnable) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(ConsumeOriginalQueueFuture(state, 1, &registration));
    harken::OriginalQueueFutureGroupLifetimePin existing, refused;
    ASSERT_TRUE(registration.BorrowLifetime(&existing));
    const auto identity = registration.Identity();
    EXPECT_FALSE(registration.BorrowLifetime(&existing));
    EXPECT_EQ(identity, existing.Identity());
    EXPECT_EQ(2u, OriginalGroupAccess::PinCount(state.pool, identity.committedRow));
    {
        ActualOriginalGroupCountLimit exhausted(state.pool, registration);
        ASSERT_TRUE(exhausted.entered);
        EXPECT_FALSE(registration.BorrowLifetime(&refused));
        EXPECT_FALSE(existing.Borrow(&refused));
        EXPECT_FALSE(refused.Valid());
        EXPECT_TRUE(registration.Valid());
        EXPECT_TRUE(existing.Valid());
        EXPECT_EQ(UINT32_MAX, OriginalGroupAccess::PinCount(state.pool, identity.committedRow));
    }
    EXPECT_EQ(2u, OriginalGroupAccess::PinCount(state.pool, identity.committedRow));
    ASSERT_TRUE(existing.Borrow(&refused));
    EXPECT_EQ(3u, OriginalGroupAccess::PinCount(state.pool, identity.committedRow));
    refused.Release();
    auto wrongIdentity = identity;
    ++wrongIdentity.rowClaimGeneration;
    EXPECT_FALSE(state.pool.IsOriginalQueueFutureGroupRegistrationFor(registration, wrongIdentity));
    wrongIdentity = identity;
    ++wrongIdentity.decision.generation;
    EXPECT_FALSE(state.pool.IsOriginalQueueFutureGroupRegistrationFor(registration, wrongIdentity));
    wrongIdentity = identity;
    ++wrongIdentity.transportEpoch;
    EXPECT_FALSE(state.pool.IsOriginalQueueFutureGroupRegistrationFor(registration, wrongIdentity));
    EXPECT_FALSE(registration.MatchesOriginalScope(18, 100, state.inputs[1], state.arms));
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 101, state.inputs[1], state.arms));
    auto wrongInput = state.inputs[1];
    ++wrongInput.executeFrame;
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 100, wrongInput, state.arms));
    wrongInput = state.inputs[1];
    wrongInput.sourceControlCount = 0;
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 100, wrongInput, state.arms));
    wrongInput = state.inputs[1];
    wrongInput.memberCount = 0;
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 100, wrongInput, state.arms));
    auto wrongMember = state.members[0];
    ++wrongMember.generation;
    wrongInput = state.inputs[1];
    wrongInput.members = &wrongMember;
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 100, wrongInput, state.arms));
    harken::ImmediateEntryAttemptGate foreignEntryGate;
    wrongInput = state.inputs[1];
    wrongInput.immediateEntry = {&foreignEntryGate, 1};
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 100, wrongInput, state.arms));
    harken::TimelineArmTable foreignArms(2);
    EXPECT_NE(state.arms.Incarnation(), foreignArms.Incarnation());
    EXPECT_FALSE(registration.MatchesOriginalScope(17, 100, state.inputs[1], foreignArms));
    EXPECT_TRUE(registration.MatchesOriginalScope(17, 100, state.inputs[1], state.arms));
    EXPECT_TRUE(registration.Valid());
    registration.Release();
    EXPECT_TRUE(existing.Valid());
    existing.Release();
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, identity));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[0], &state.raw[0]));
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(&state.originals[2], &state.raw[2]));
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}
#endif

} // namespace

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace {

TEST(DecisionGroup, OriginalQueueFutureUnconsumedPublishedMoveOverwriteAndDestructionReturnActualAssociation) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(2));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 2;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    std::array<harken::Plan03GroupSliceInput, 2> inputs{};
    std::array<harken::Plan03GroupSliceIdentity, 2> raw{};
    std::array<harken::PreparedPrivateImmediateLaneGroup, 2> originals{};
    for (std::uint32_t index = 0; index < raw.size(); ++index) {
        inputs[index].executeFrame = 120;
        inputs[index].decision = laneHarness.tokens[index];
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, inputs[index], arms,
            &raw[index], &originals[index]));
        EXPECT_TRUE(originals[index].Valid());
        EXPECT_FALSE(originals[index].Held());
    }
    using Access = harken::detail::ScheduledTransactionPoolOriginalTestAccess;
    const std::array markers{Access::RowMarker(pool, raw[0].committedRow),
                             Access::RowMarker(pool, raw[1].committedRow)};
    const auto markerBudget = Access::MarkerBudget(pool);
    ASSERT_NE(0u, markers[0]);
    ASSERT_NE(0u, markers[1]);
    ASSERT_NO_FATAL_FAILURE(laneHarness.Bind(pool, raw.data(), raw.size()));
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = raw[0].decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = raw[0];
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = raw[0];
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw[0], binding));
    binding.role = harken::FrontierGroupRole::EntryMember;
    binding.operation.group = raw[1];
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw[1], binding));
    std::array<harken::Plan03GroupReaderPin, 2> readers;
    for (std::uint32_t index = 0; index < raw.size(); ++index)
        ASSERT_TRUE(pool.AcquirePlan03GroupReader(raw[index], &readers[index]));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Publish(pool, arms, raw.data(), raw.size()));
    std::array<harken::Plan03PageGroupView, 2> views{};
    for (std::uint32_t index = 0; index < raw.size(); ++index) {
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(raw[index], &views[index]));
        EXPECT_FALSE(originals[index].Valid());
        EXPECT_FALSE(originals[index].Held());
        EXPECT_TRUE(Access::OriginalAssociationIs(pool, raw[index], &originals[index]));
        EXPECT_EQ(markers[index], Access::RowMarker(pool, raw[index].committedRow));
        EXPECT_EQ(0u, Access::PinCount(pool, raw[index].committedRow));
    }
    {
        harken::PreparedPrivateImmediateLaneGroup moved(std::move(originals[0]));
        EXPECT_TRUE(Access::OriginalAssociationIs(pool, raw[0], &moved));
        EXPECT_FALSE(Access::OriginalAssociationIs(pool, raw[0], &originals[0]));
        EXPECT_FALSE(moved.Valid());
        EXPECT_FALSE(moved.Held());
        EXPECT_EQ(raw[0], moved.Identity());
        // Negative qualification cannot grant a post-publication transfer.
        harken::PrivateImmediateLaneGroupTransferProof refused;
        EXPECT_FALSE(pool.ProvePrivateImmediateLaneGroupTransfer(
            moved, 17, 100, inputs[0], arms, &refused));
        EXPECT_FALSE(refused.Valid());
        originals[1] = std::move(moved);
        EXPECT_FALSE(moved.Valid());
        EXPECT_TRUE(Access::OriginalAssociationIs(pool, raw[1], nullptr));
        EXPECT_EQ(0u, Access::RowMarker(pool, raw[1].committedRow));
        EXPECT_TRUE(Access::OriginalAssociationIs(pool, raw[0], &originals[1]));
        EXPECT_EQ(markers[0], Access::RowMarker(pool, raw[0].committedRow));
        {
            harken::PreparedPrivateImmediateLaneGroup finalOwner(std::move(originals[1]));
            EXPECT_TRUE(Access::OriginalAssociationIs(pool, raw[0], &finalOwner));
            EXPECT_FALSE(finalOwner.Valid());
            EXPECT_FALSE(finalOwner.Held());
            EXPECT_FALSE(pool.ProvePrivateImmediateLaneGroupTransfer(
                finalOwner, 17, 100, inputs[0], arms, &refused));
            EXPECT_FALSE(refused.Valid());
        }
        EXPECT_TRUE(Access::OriginalAssociationIs(pool, raw[0], nullptr));
        EXPECT_EQ(0u, Access::RowMarker(pool, raw[0].committedRow));
    }
    EXPECT_EQ(markerBudget, Access::MarkerBudget(pool));
    // Original cleanup returned only its association, not the published rows.
    for (std::uint32_t index = 0; index < raw.size(); ++index) {
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(raw[index], &views[index]));
        EXPECT_EQ(harken::AudioSliceState::Ready, views[index].lease->Capture().state);
        ASSERT_TRUE(pool.NotePlan03GroupReadRt(raw[index], 1));
    }
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 2));
    ASSERT_EQ(views[0].lease, views[1].lease);
    EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
              views[0].lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(raw[1], 1));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw[1], 2));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(raw[0], 1));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw[0], 2));
    for (auto& reader : readers) {
        EXPECT_FALSE(reader.Release(1));
        ASSERT_TRUE(reader.Release(2));
    }
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 1));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2));
    for (const auto& token : laneHarness.tokens) {
        ASSERT_TRUE(laneHarness.decisions.Cancel(token));
        ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(token));
    }
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));

    // The surviving original outlives its receiver, but actual quiescent Pool
    // destruction must return its still-associated owner before arrays die.
    harken::PreparedPrivateImmediateLaneGroup survivesReceiver;
    harken::TimelineArmTable teardownArms(1);
    DecisionLaneHarness teardownLane;
    ASSERT_NO_FATAL_FAILURE(teardownLane.ReserveDecisions(1));
    {
        harken::ScheduledTransactionPool teardownPool;
        auto teardownConfig = config;
        teardownConfig.maxImmediateLaneGroupRows = 1;
        ASSERT_TRUE(teardownPool.Reset(teardownConfig, 0, 9));
        harken::Plan03GroupSliceInput teardownInput{};
        teardownInput.executeFrame = 120;
        teardownInput.decision = teardownLane.tokens[0];
        harken::Plan03GroupSliceIdentity teardownRaw{};
        {
            harken::PreparedPrivateImmediateLaneGroup factoryOwner;
            ASSERT_TRUE(teardownPool.ReserveImmediateLaneGroup(17, 100,
                teardownInput, teardownArms, &teardownRaw, &factoryOwner));
            ASSERT_NO_FATAL_FAILURE(teardownLane.Bind(teardownPool, &teardownRaw, 1));
            auto teardownBinding = binding;
            teardownBinding.role = harken::FrontierGroupRole::Coordinator;
            teardownBinding.entry.coordinator = teardownRaw.decision;
            teardownBinding.operation.group = teardownRaw;
            teardownBinding.primary = teardownRaw;
            ASSERT_TRUE(teardownPool.BindPlan03GroupFrontier(teardownRaw, teardownBinding));
            ASSERT_NO_FATAL_FAILURE(teardownLane.Publish(teardownPool, teardownArms, &teardownRaw, 1));
            survivesReceiver = std::move(factoryOwner);
            EXPECT_TRUE(Access::OriginalAssociationIs(teardownPool, teardownRaw, &survivesReceiver));
            EXPECT_FALSE(survivesReceiver.Valid());
        }
        // The old factory object's lifetime is now over. Only the genuine
        // relocated original remains in the Pool's cleanup association.
        EXPECT_TRUE(Access::OriginalAssociationIs(teardownPool, teardownRaw, &survivesReceiver));
        harken::Plan03PageGroupView teardownView{};
        ASSERT_TRUE(teardownPool.ResolvePlan03GroupSliceRt(teardownRaw, &teardownView));
        ASSERT_TRUE(teardownPool.NotePlan03GroupReadRt(teardownRaw, 1));
        EXPECT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
                  teardownView.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
        ASSERT_TRUE(teardownLane.lane.Cancel(teardownLane.handle));
        ASSERT_TRUE(teardownLane.lane.Retire(teardownLane.handle, 2));
        EXPECT_FALSE(teardownPool.DetachPlan03GroupFrontier(teardownRaw, 1));
        ASSERT_TRUE(teardownPool.DetachPlan03GroupFrontier(teardownRaw, 2));
        EXPECT_TRUE(Access::OriginalAssociationIs(teardownPool, teardownRaw, &survivesReceiver));
        EXPECT_NE(0u, Access::RowMarker(teardownPool, teardownRaw.committedRow));
        EXPECT_EQ(0u, Access::PinCount(teardownPool, teardownRaw.committedRow));
        ASSERT_TRUE(teardownLane.decisions.Cancel(teardownLane.tokens[0]));
        ASSERT_TRUE(teardownLane.decisions.ReclaimCancelled(teardownLane.tokens[0]));
        // No native reader/frontier/lane/Decision duty remains; actual Pool
        // destructor owns its original association return. No count is forced.
    }
    // No Valid() call may dereference a dead receiver here. Identity is the
    // original's plain observer and must have been emptied by the real Drop.
    EXPECT_FALSE(survivesReceiver.Identity().Valid());
}

} // namespace
#endif

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace {

TEST(DecisionGroup, NativeOriginalBaselineRetirementWaitsForOldApplyAndEveryTrueLoan) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(1));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity raw{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &raw, &original));
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    harken::OriginalQueueFutureGroupLifetimePin lifetime;
    ASSERT_TRUE(registration.BorrowLifetime(&lifetime));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Bind(pool, &raw, 1));
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = raw.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = raw;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = raw;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw, binding));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(raw, &reader));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Publish(pool, arms, &raw, 1));
    harken::Plan03PageGroupView view{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(raw, &view));
    std::array<harken::Plan03PageGroupView, 1> due{};
    ASSERT_EQ(1u, pool.CaptureImmediateLaneGroupsRt(4, due.data(), due.size()));
    EXPECT_EQ(raw, due[0].identity);
    harken::ImmediateLaneFrontierClaim frontierClaim{};
    ASSERT_TRUE(pool.ClaimImmediateLaneFrontier(raw, &frontierClaim));
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(raw, 1));
    std::uint32_t applied = 0;
    SliceStep step{view.lease, &applied};
    harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
              harken::ApplyAudioSliceWhole(*view.lease, &operation, 1));
    EXPECT_EQ(1u, applied);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, view.lease->Capture().state);
    EXPECT_TRUE(registration.Valid());
    EXPECT_TRUE(lifetime.Valid());
    ASSERT_TRUE(pool.ReleaseImmediateLaneFrontier(&frontierClaim));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(raw, 1));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw, 2));
    EXPECT_FALSE(reader.Release(1));
    ASSERT_TRUE(reader.Release(2));
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 2, &registration, 1));
    EXPECT_TRUE(registration.Valid());
    EXPECT_TRUE(lifetime.Valid());
    lifetime.Release();
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 1, &registration, 1));
    EXPECT_TRUE(registration.Valid());
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2, &registration, 1));
    EXPECT_FALSE(registration.Valid());
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(raw, &view));
    ASSERT_TRUE(laneHarness.decisions.Cancel(laneHarness.tokens[0]));
    ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(laneHarness.tokens[0]));
    EXPECT_EQ(0u, laneHarness.decisions.LiveCount());
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
}



TEST(DecisionGroup, NativePrivateOriginalBaselineCancellationRefusesForeignAndEveryDescendant) {
    PrivateLaneTransferHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    std::array<harken::OriginalQueueFutureGroupRegistration, 3> registrations;
    for (std::uint32_t index = 0; index < registrations.size(); ++index)
        ASSERT_TRUE(ConsumeOriginalQueueFuture(state, index, &registrations[index]));
    const auto identity = state.raw[0];
    harken::OriginalQueueFutureGroupLifetimePin loan, descendant;
    ASSERT_TRUE(registrations[0].BorrowLifetime(&loan));
    ASSERT_TRUE(loan.Borrow(&descendant));
    EXPECT_TRUE(state.pool.IsOriginalQueueFutureGroupRegistrationFor(registrations[0], identity, 17));
    EXPECT_FALSE(state.pool.IsOriginalQueueFutureGroupRegistrationFor(registrations[0], identity, 18));
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, identity, &registrations[0]));
    EXPECT_TRUE(registrations[0].Valid());
    loan.Release();
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, identity, &registrations[0]));
    descendant.Release();
    EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, identity, &registrations[1]));
    EXPECT_TRUE(registrations[0].Valid());
    EXPECT_TRUE(registrations[1].Valid());
    ASSERT_TRUE(state.pool.CancelPrivateImmediateLaneGroup(17, identity, &registrations[0]));
    EXPECT_FALSE(registrations[0].Valid());
    ASSERT_TRUE(state.pool.CancelImmediateLaneGroups(17, 0,
        registrations.data(), registrations.size()));
    for (const auto& registration : registrations) EXPECT_FALSE(registration.Valid());
    ASSERT_NO_FATAL_FAILURE(state.ReclaimOtherParticipants());
}


TEST(DecisionGroup, NativeOriginalMemberBlocksPrimaryRetirementUntilItsTrueLoanOrReaderReturns) {
    for (const bool crossBucket : {false, true}) {
        for (const bool lifetimeBlocker : {false, true}) {
            for (const bool primaryFirst : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "crossBucket=" << crossBucket
                    << " lifetimeBlocker=" << lifetimeBlocker << " primaryFirst=" << primaryFirst);
                harken::ScheduledTransactionPool pool;
                harken::TimelineArmTable arms(1);
                DecisionLaneHarness laneHarness;
                ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(2));
                auto config = MakePoolConfig();
                config.maxImmediateLaneGroupRows = 2;
                config.maxImmediateLaneOwnerGates = 4;
                config.maxImmediateLaneDecisionGates = 4;
                ASSERT_TRUE(pool.Reset(config, 0, 9));
                const std::uint32_t primaryIndex = primaryFirst ? 0u : 1u;
                const std::uint32_t memberIndex = 1u - primaryIndex;
                std::array<harken::Plan03GroupSliceInput, 2> inputs{};
                std::array<harken::Plan03GroupSliceIdentity, 2> raw{};
                std::array<harken::PreparedPrivateImmediateLaneGroup, 2> originals;
                std::array<harken::OriginalQueueFutureGroupRegistration, 2> registrations;
                for (std::uint32_t index = 0; index < raw.size(); ++index) {
                    inputs[index].executeFrame = 120 + (crossBucket && index == memberIndex ? 100 : 0);
                    inputs[index].decision = laneHarness.tokens[index];
                    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, inputs[index], arms,
                        &raw[index], &originals[index]));
                    harken::PrivateImmediateLaneGroupTransferProof proof;
                    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(
                        originals[index], 17, 100, inputs[index], arms, &proof));
                    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(
                        std::move(proof), &registrations[index]));
                }
                harken::OriginalQueueFutureGroupLifetimePin loan;
                if (lifetimeBlocker) ASSERT_TRUE(registrations[memberIndex].BorrowLifetime(&loan));
                ASSERT_TRUE(laneHarness.lane.Reset({1, 2, 2, 100, false, 2}));
                ASSERT_TRUE(laneHarness.lane.Reserve(17,
                    {0, 2, 0, crossBucket ? 2u : 1u}, &laneHarness.handle));
                for (const auto& identity : raw) ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(
                    identity, laneHarness.lane, 17, laneHarness.handle.slot, laneHarness.handle.generation));
                ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 400));
                harken::FrontierGroupBinding binding{};
                binding.role = harken::FrontierGroupRole::Coordinator;
                binding.entry.coordinator = raw[primaryIndex].decision;
                binding.entry.attempt = 1;
                binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
                binding.operation.kind = harken::FrontierOperationKind::Entry;
                binding.operation.owner = binding.entry.owner;
                binding.operation.order.logicCommitSequence = 1;
                binding.operation.order.executeFrame = 420;
                binding.operation.group = raw[primaryIndex];
                binding.operation.transport = 9;
                binding.operation.world = 7;
                binding.primary = raw[primaryIndex];
                ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw[primaryIndex], binding));
                binding.role = harken::FrontierGroupRole::EntryMember;
                binding.operation.group = raw[memberIndex];
                binding.operation.order.executeFrame = inputs[memberIndex].executeFrame + 300;
                ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw[memberIndex], binding));
                std::array<harken::Plan03GroupReaderPin, 2> readers;
                for (std::uint32_t index = 0; index < raw.size(); ++index) {
                    ASSERT_TRUE(pool.AcquirePlan03GroupReader(raw[index], &readers[index]));
                    ASSERT_TRUE(laneHarness.lane.DeclareGroup(laneHarness.handle,
                        {raw[index], inputs[index].executeFrame - 100}));
                }
                ASSERT_TRUE(laneHarness.lane.Prepare(laneHarness.handle, nullptr, 0));
                std::uint64_t anchor = UINT64_MAX;
                ASSERT_TRUE(laneHarness.lane.TryPlace(laneHarness.handle, 400, 0, &anchor));
                ASSERT_EQ(400u, anchor);
                ASSERT_TRUE(laneHarness.lane.PinGroups(laneHarness.handle, pool, laneHarness.decisions, 2));
                ASSERT_TRUE(laneHarness.lane.PinArms(laneHarness.handle, arms));
                ASSERT_TRUE(pool.PublishImmediateLaneGroups(17));
                ASSERT_TRUE(laneHarness.lane.Publish(laneHarness.handle));
                std::array<harken::Plan03PageGroupView, 2> views{};
                for (std::uint32_t index = 0; index < raw.size(); ++index) {
                    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(raw[index], &views[index]));
                    ASSERT_TRUE(pool.NotePlan03GroupReadRt(raw[index], 1));
                }
                EXPECT_EQ(!crossBucket, views[primaryIndex].lease == views[memberIndex].lease);
                ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
                    views[primaryIndex].lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
                if (crossBucket) ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
                    views[memberIndex].lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
                ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
                ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
                ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw[memberIndex], 2));
                ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw[primaryIndex], 2));
                ASSERT_TRUE(readers[primaryIndex].Release(2));
                if (lifetimeBlocker) ASSERT_TRUE(readers[memberIndex].Release(2));
                // A blocked member still needs its original primary's live backing.
                EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 2, registrations.data(), registrations.size()));
                for (std::uint32_t index = 0; index < raw.size(); ++index) {
                    ASSERT_TRUE(registrations[index].Valid());
                    ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(raw[index], &views[index]));
                    EXPECT_EQ(raw[index], views[index].identity);
                    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, views[index].lease->Capture().state);
                }
                if (lifetimeBlocker) {
                    ASSERT_TRUE(loan.Valid());
                    loan.Release();
                } else {
                    EXPECT_FALSE(readers[memberIndex].Release(1));
                    ASSERT_TRUE(readers[memberIndex].Release(2));
                }
                EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 1, registrations.data(), registrations.size()));
                for (const auto& registration : registrations) ASSERT_TRUE(registration.Valid());
                ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2, registrations.data(), registrations.size()));
                for (std::uint32_t index = 0; index < raw.size(); ++index) {
                    EXPECT_FALSE(registrations[index].Valid());
                    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(raw[index], &views[index]));
                    ASSERT_TRUE(laneHarness.decisions.Cancel(laneHarness.tokens[index]));
                    ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(laneHarness.tokens[index]));
                }
                EXPECT_EQ(0u, laneHarness.decisions.LiveCount());
                ASSERT_TRUE(pool.ResetForEpoch(0, 10));
            }
        }
    }
}

} // namespace
#endif

namespace harken::detail {

struct DecisionOriginalLifetimeTestAccess {
    struct Observation {
        std::uint64_t marker = 0;
        std::uint32_t count = 0;
        DecisionReserveRequest request{};
        std::uint32_t futureCount = 0;
    };
    static bool Observe(const DecisionTable& table, DecisionToken token, Observation* out) noexcept {
        const auto* row = table.Resolve(token);
        if (row == nullptr || out == nullptr) return false;
        *out = {row->originalLifetimeMarker, row->originalLifetimePinCount,
            row->originalLifetimeRequest, row->futurePinCount.load(std::memory_order_acquire)};
        return true;
    }

    // Negative-only, scoped count limit on an already genuine origin.
    struct BorrowLimit {
        DecisionTable* table = nullptr;
        DecisionToken token{};
        std::uint32_t genuineCount = 0;
        BorrowLimit(DecisionTable& owner, const OriginalDecisionLifetimePin& actual) noexcept {
            if (!owner.IsOriginalDecisionLifetimePinFor(actual, actual.Token())) return;
            auto* row = owner.Resolve(actual.Token());
            if (row == nullptr || row->originalLifetimePinCount == UINT32_MAX) return;
            table = &owner;
            token = actual.Token();
            genuineCount = row->originalLifetimePinCount;
            row->originalLifetimePinCount = UINT32_MAX;
        }
        BorrowLimit(const BorrowLimit&) = delete;
        BorrowLimit& operator=(const BorrowLimit&) = delete;
        ~BorrowLimit() {
            if (table == nullptr) return;
            auto* row = table->Resolve(token);
            if (row != nullptr && row->originalLifetimePinCount == UINT32_MAX)
                row->originalLifetimePinCount = genuineCount;
        }
        bool Ready() const noexcept { return table != nullptr; }
    };

    // No origin can issue inside this scope; raw old factories may still progress.
    struct IssuerLimit {
        DecisionTable* table = nullptr;
        std::uint64_t originalNext = 0;
        bool originalExhausted = false;
        IssuerLimit(DecisionTable& owner, bool zeroBudget) noexcept {
            if (owner.LiveCount() != 0 || owner.originalLifetimeMarkerExhausted_ ||
                owner.nextOriginalLifetimeMarker_ == 0) return;
            for (std::uint32_t index = 0; index < owner.capacity_; ++index)
                if (owner.rows_[index].originalLifetimePinCount != 0) return;
            table = &owner;
            originalNext = owner.nextOriginalLifetimeMarker_;
            originalExhausted = owner.originalLifetimeMarkerExhausted_;
            if (zeroBudget) owner.nextOriginalLifetimeMarker_ = 0;
            else owner.originalLifetimeMarkerExhausted_ = true;
        }
        IssuerLimit(const IssuerLimit&) = delete;
        IssuerLimit& operator=(const IssuerLimit&) = delete;
        ~IssuerLimit() {
            if (table == nullptr) return;
            table->nextOriginalLifetimeMarker_ = originalNext;
            table->originalLifetimeMarkerExhausted_ = originalExhausted;
        }
        bool Ready() const noexcept { return table != nullptr; }
    };
};

} // namespace harken::detail

namespace {

using OriginalDecisionPin = harken::OriginalDecisionLifetimePin;
using OriginalDecisionObservation = harken::detail::DecisionOriginalLifetimeTestAccess::Observation;
static_assert(!std::is_copy_constructible_v<OriginalDecisionPin>);
static_assert(!std::is_copy_assignable_v<OriginalDecisionPin>);
static_assert(std::is_move_constructible_v<OriginalDecisionPin>);
static_assert(!std::is_move_constructible_v<harken::DecisionTable>);
static_assert(!std::is_constructible_v<OriginalDecisionPin, harken::DecisionToken>);
static_assert(!std::is_constructible_v<OriginalDecisionPin, const harken::DecisionFuturePin&>);

harken::DecisionReserveRequest MakeGenuineOriginalDecisionRequest(
    harken::LogicCommitCoordinator& commits, bool prepared = false) {
    auto request = MakeRequest(0);
    if (!prepared) request.ticket.sequence = commits.Issue();
    return request;
}

bool SameOriginalDecisionInput(const harken::DecisionReserveRequest& a,
    const harken::DecisionReserveRequest& b) {
    return a.ticket.sequence == b.ticket.sequence && a.world == b.world &&
        a.transport == b.transport &&
        a.expectedContinuationGeneration == b.expectedContinuationGeneration &&
        a.sliceCount == b.sliceCount &&
        a.transitionGate.slot == b.transitionGate.slot &&
        a.transitionGate.generation == b.transitionGate.generation;
}

void ExpectOriginalDecisionObservation(const OriginalDecisionObservation& before,
    const OriginalDecisionObservation& after) {
    EXPECT_EQ(before.marker, after.marker);
    EXPECT_EQ(before.count, after.count);
    EXPECT_TRUE(SameOriginalDecisionInput(before.request, after.request));
    EXPECT_EQ(before.futureCount, after.futureCount);
}

bool ObserveOriginalDecision(const harken::DecisionTable& table, harken::DecisionToken token,
    OriginalDecisionObservation* out) {
    return harken::detail::DecisionOriginalLifetimeTestAccess::Observe(table, token, out);
}

TEST(DecisionGroup, OriginalDecisionReserveLoansAllowTrueAudioProgressAndRetire) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(1, 1);
    harken::ArmGroupTable groups(1, 1);
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    const auto request = MakeGenuineOriginalDecisionRequest(commits);
    harken::DecisionToken token{};
    OriginalDecisionPin original, loan;
    ASSERT_TRUE(table.Reserve(request, &token, &original));
    ASSERT_TRUE(table.IsOriginalDecisionLifetimePinFor(original, token));
    ASSERT_TRUE(table.MatchesOriginalDecisionFactory(original, request));
    ASSERT_TRUE(original.Borrow(&loan));
    OriginalDecisionObservation before{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &before));
    EXPECT_NE(0u, before.marker);
    EXPECT_EQ(2u, before.count);
    EXPECT_EQ(0u, before.futureCount);
    harken::ArmGroupToken group{};
    ASSERT_TRUE(groups.Reserve(token, nullptr, 0, &group));
    ASSERT_TRUE(pool.Reset(MakePoolConfig(), 0, request.transport));
    harken::Plan03GroupSliceInput input{};
    input.decision = token;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool.ReservePlan03GroupSlice(input, arms));
    harken::Plan03PageGroupView nativeGroup{};
    ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&nativeGroup, 1));
    ASSERT_EQ(token.slot, nativeGroup.identity.decision.slot);
    ASSERT_EQ(token.generation, nativeGroup.identity.decision.generation);
    harken::DecisionCommitPayload payload{};
    // The real native group above backs the existing Plan03 marker carrier.
    payload.pages = {1, 1};
    payload.armGroup = group;
    ASSERT_TRUE(table.Commit(token, payload));
    ASSERT_TRUE(table.TryClaimAudio(token));
    ASSERT_TRUE(table.RecordAppliedSlice(token, payload.pages, 0, 3));
    ASSERT_TRUE(table.MarkAudioAdmitted(token));
    ASSERT_TRUE(table.RecordAppliedSlice(token, payload.pages, 1, 4));
    harken::DecisionFuturePin reader;
    ASSERT_TRUE(table.AcquireFuturePin(token, &reader));
    ASSERT_TRUE(table.Retire(token, 4));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(table.CaptureCommitted(token, &snapshot));
    EXPECT_EQ(harken::AudioGroupState::Retired, snapshot.audioState);
    EXPECT_EQ(2u, snapshot.consumedSlices);
    EXPECT_EQ(4u, snapshot.lastAudioSequence);
    EXPECT_EQ(1u, snapshot.futurePinCount);
    EXPECT_FALSE(table.ReclaimRetired(token, &original));
    ASSERT_TRUE(reader.Release());
    EXPECT_FALSE(table.ReclaimRetired(token));
    EXPECT_FALSE(table.ReclaimRetired(token, &original));
    OriginalDecisionObservation after{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &after));
    ExpectOriginalDecisionObservation(before, after);
    EXPECT_TRUE(table.MatchesOriginalDecisionFactory(loan, request));
    ASSERT_TRUE(loan.Release());
    ASSERT_TRUE(table.ReclaimRetired(token, &original));
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(table.Contains(token));
    ASSERT_TRUE(groups.BeginRetire(group));
    ASSERT_TRUE(groups.Reclaim(group, 4));
    EXPECT_EQ(0u, groups.LiveCount());
}

TEST(DecisionGroup, OriginalDecisionPreparedProofKeepsFutureZeroAndNativeFactoryInput) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(1, 1);
    const auto request = MakeGenuineOriginalDecisionRequest(commits, true);
    harken::PreparedDecisionReservation prepared;
    OriginalDecisionPin original, foreign;
    ASSERT_TRUE(table.PrepareReservation(request, &prepared, &original));
    const auto token = prepared.Token();
    ASSERT_TRUE(original.Borrow(&foreign));
    OriginalDecisionObservation before{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &before));
    ASSERT_EQ(2u, before.count);
    ASSERT_EQ(0u, before.futureCount);
    const auto sequence = commits.Issue();
    ASSERT_NE(0u, sequence);
    harken::DecisionFuturePin legacy;
    ASSERT_TRUE(table.AcquireFuturePin(token, &legacy));
    harken::DecisionTable::PreparedTicketCommitProof proof;
    EXPECT_FALSE(table.ProvePreparedReservationCommit(prepared, request, sequence, &proof));
    EXPECT_FALSE(proof.Valid());
    ASSERT_TRUE(legacy.Release());
    auto wrong = request;
    ++wrong.world;
    EXPECT_FALSE(table.ProvePreparedReservationCommit(prepared, wrong, sequence, &proof));
    EXPECT_TRUE(prepared.Valid());
    ASSERT_TRUE(table.ProvePreparedReservationCommit(prepared, request, sequence, &proof));
    EXPECT_TRUE(table.PreparedTicketHeld(token));
    OriginalDecisionPin heldLoan;
    ASSERT_TRUE(foreign.Borrow(&heldLoan));
    ASSERT_TRUE(heldLoan.Release());
    EXPECT_FALSE(table.AcquireFuturePin(token, &legacy));
    proof.Release();
    ASSERT_TRUE(prepared.Valid());
    ASSERT_TRUE(table.ProvePreparedReservationCommit(prepared, request, sequence, &proof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(prepared.Valid());
    auto ticketed = request;
    ticketed.ticket.sequence = sequence;
    EXPECT_TRUE(table.MatchesOpenReservation(token, ticketed));
    EXPECT_TRUE(table.MatchesOriginalDecisionFactory(original, request));
    EXPECT_FALSE(table.MatchesOriginalDecisionFactory(original, ticketed));
    OriginalDecisionObservation after{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &after));
    ExpectOriginalDecisionObservation(before, after);
    ASSERT_TRUE(table.Cancel(token));
    EXPECT_FALSE(table.ReclaimCancelled(token, &original));
    ASSERT_TRUE(foreign.Release());
    ASSERT_TRUE(table.ReclaimCancelled(token, &original));
}

TEST(DecisionGroup, PreparedTransitionAttachmentPreservesOriginalFactoryAndTicketProof) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(1, 1);
    harken::TransitionInstancePool transitions(1);
    harken::TransitionInstanceToken gate{};
    ASSERT_TRUE(transitions.Acquire({300, 3}, &gate));
    const auto request = MakeGenuineOriginalDecisionRequest(commits, true);
    harken::PreparedDecisionReservation prepared;
    OriginalDecisionPin original;
    ASSERT_TRUE(table.PrepareReservation(request, &prepared, &original));
    const auto token = prepared.Token();
    OriginalDecisionObservation before{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &before));
    ASSERT_TRUE(table.AttachPreparedTransitionGateBeforeCommit(prepared, gate, 2));
    EXPECT_FALSE(table.AttachPreparedTransitionGateBeforeCommit(prepared, gate, 2));
    EXPECT_TRUE(SameOriginalDecisionInput(request, prepared.OriginalRequest()));
    EXPECT_TRUE(table.MatchesOriginalPreparedDecision(original, prepared));
    auto runtime = request;
    runtime.transitionGate = gate;
    runtime.sliceCount = 2;
    EXPECT_TRUE(table.MatchesOpenReservation(token, runtime));
    harken::DecisionTable::PreparedTicketCommitProof proof;
    EXPECT_FALSE(table.ProvePreparedReservationCommit(prepared, runtime, 17, &proof));
    harken::PreparedDecisionReservation moved(std::move(prepared));
    ASSERT_TRUE(table.ProvePreparedReservationCommit(moved, request, 17, &proof));
    EXPECT_FALSE(table.AttachPreparedTransitionGateBeforeCommit(moved, gate, 1));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(proof));
    runtime.ticket.sequence = 17;
    EXPECT_TRUE(table.MatchesOpenReservation(token, runtime));
    OriginalDecisionObservation after{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &after));
    ExpectOriginalDecisionObservation(before, after);
    ASSERT_TRUE(table.Cancel(token));
    ASSERT_TRUE(table.ReclaimCancelled(token, &original));
}

TEST(DecisionGroup, PreparedTransitionAttachmentRejectsForeignLoanAndHeldTicket) {
    harken::DecisionTable table(1, 1), foreign(1, 1);
    harken::TransitionInstancePool transitions(1);
    harken::TransitionInstanceToken gate{};
    ASSERT_TRUE(transitions.Acquire({300, 3}, &gate));
    const auto request = MakeRequest(0);
    harken::PreparedDecisionReservation prepared;
    OriginalDecisionPin original, loan;
    ASSERT_TRUE(table.PrepareReservation(request, &prepared, &original));
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(table.AttachPreparedTransitionGateBeforeCommit(prepared, gate, 2));
    EXPECT_FALSE(foreign.AttachPreparedTransitionGateBeforeCommit(prepared, gate, 2));
    EXPECT_TRUE(table.MatchesOpenReservation(prepared.Token(), request));
    ASSERT_TRUE(loan.Release());
    harken::DecisionTable::PreparedTicketCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedReservationCommit(prepared, request, 17, &proof));
    EXPECT_FALSE(table.AttachPreparedTransitionGateBeforeCommit(prepared, gate, 2));
    proof.Release();
    ASSERT_TRUE(table.AttachPreparedTransitionGateBeforeCommit(prepared, gate, 2));
    ASSERT_TRUE(table.CancelPreparedReservation(&prepared, &original));
    ASSERT_TRUE(table.PrepareReservation(request, &prepared, &original));
    ASSERT_TRUE(table.ProvePreparedReservationCommit(prepared, request, 18, &proof));
    proof.Release();
    ASSERT_TRUE(table.CancelPreparedReservation(&prepared, &original));
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(DecisionGroup, OriginalDecisionPreparedCancelKeepsNativeOwnerAcrossForeignLoanRetry) {
    harken::LogicCommitCoordinator firstCommits, secondCommits;
    harken::DecisionTable owner(1, 1), foreignTable(1, 1);
    const auto request = MakeGenuineOriginalDecisionRequest(firstCommits, true);
    const auto foreignRequest = MakeGenuineOriginalDecisionRequest(secondCommits);
    harken::PreparedDecisionReservation prepared;
    harken::DecisionToken foreignToken{};
    OriginalDecisionPin original, loan, foreignOriginal;
    ASSERT_TRUE(owner.PrepareReservation(request, &prepared, &original));
    const auto token = prepared.Token();
    ASSERT_TRUE(foreignTable.Reserve(foreignRequest, &foreignToken, &foreignOriginal));
    ASSERT_EQ(token.slot, foreignToken.slot);
    ASSERT_EQ(token.generation, foreignToken.generation);
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(owner.IsOriginalDecisionLifetimePinFor(foreignOriginal, token));
    EXPECT_FALSE(owner.CancelPreparedReservation(&prepared, &foreignOriginal));
    EXPECT_TRUE(owner.IsOpen(token));
    EXPECT_TRUE(foreignTable.IsOpen(foreignToken));
    OriginalDecisionObservation before{};
    ASSERT_TRUE(ObserveOriginalDecision(owner, token, &before));
    EXPECT_FALSE(owner.CancelPreparedReservation(&prepared, &original));
    EXPECT_TRUE(owner.IsCancelled(token));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(loan.Valid());
    EXPECT_TRUE(owner.MatchesOriginalDecisionFactory(loan, request));
    OriginalDecisionObservation after{};
    ASSERT_TRUE(ObserveOriginalDecision(owner, token, &after));
    ExpectOriginalDecisionObservation(before, after);
    EXPECT_FALSE(owner.CancelPreparedReservation(&prepared));
    ASSERT_TRUE(loan.Release());
    ASSERT_TRUE(owner.CancelPreparedReservation(&prepared, &original));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(owner.Contains(token));
    ASSERT_TRUE(foreignTable.Cancel(foreignToken));
    ASSERT_TRUE(foreignTable.ReclaimCancelled(foreignToken, &foreignOriginal));
}

TEST(DecisionGroup, OriginalDecisionFactoryPreflightRefusesOccupiedAndExactScopeMismatch) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(2, 1);
    const auto request = MakeGenuineOriginalDecisionRequest(commits);
    const auto nextRequest = MakeGenuineOriginalDecisionRequest(commits);
    auto preparedRequest = MakeGenuineOriginalDecisionRequest(commits, true);
    harken::DecisionToken token{}, unchanged{31, 37};
    OriginalDecisionPin original, second, loan;
    ASSERT_TRUE(table.Reserve(request, &token, &original));
    OriginalDecisionObservation before{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &before));
    EXPECT_FALSE(table.Reserve(nextRequest, &unchanged, &original));
    EXPECT_EQ(31u, unchanged.slot);
    EXPECT_EQ(37u, unchanged.generation);
    harken::PreparedDecisionReservation prepared;
    EXPECT_FALSE(table.PrepareReservation(preparedRequest, &prepared, &original));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    auto invalid = nextRequest;
    invalid.world = 0;
    EXPECT_FALSE(table.Reserve(invalid, &unchanged, &second));
    EXPECT_FALSE(second.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    for (int field = 0; field < 7; ++field) {
        auto wrong = request;
        switch (field) {
        case 0: ++wrong.ticket.sequence; break;
        case 1: ++wrong.world; break;
        case 2: ++wrong.transport; break;
        case 3: ++wrong.expectedContinuationGeneration; break;
        case 4: ++wrong.sliceCount; break;
        case 5: wrong.transitionGate = {0, 1}; break;
        default: wrong.transitionGate = {0, 2}; break;
        }
        EXPECT_FALSE(table.MatchesOriginalDecisionFactory(original, wrong));
    }
    auto wrongToken = token;
    ++wrongToken.generation;
    EXPECT_FALSE(table.IsOriginalDecisionLifetimePinFor(original, wrongToken));
    EXPECT_FALSE(original.Borrow(&original));
    OriginalDecisionObservation after{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &after));
    ExpectOriginalDecisionObservation(before, after);
    harken::DecisionToken next{};
    ASSERT_TRUE(table.Reserve(nextRequest, &next, &second));
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&second));
    ASSERT_TRUE(table.Cancel(token));
    EXPECT_FALSE(table.ReclaimCancelled(token, &second));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(second.Valid());
    ASSERT_TRUE(loan.Release());
    ASSERT_TRUE(table.ReclaimCancelled(token, &original));
    ASSERT_TRUE(table.Cancel(next));
    ASSERT_TRUE(table.ReclaimCancelled(next, &second));
}

TEST(DecisionGroup, OriginalDecisionMovesAndDestructionReturnTheirExactNativeReceiver) {
    harken::LogicCommitCoordinator firstCommits, secondCommits;
    harken::DecisionTable first(1, 1), second(1, 1);
    const auto firstInput = MakeGenuineOriginalDecisionRequest(firstCommits);
    const auto secondInput = MakeGenuineOriginalDecisionRequest(secondCommits);
    harken::DecisionToken firstToken{}, secondToken{};
    OriginalDecisionPin firstOriginal, secondOriginal, firstLoan, secondLoan;
    ASSERT_TRUE(first.Reserve(firstInput, &firstToken, &firstOriginal));
    ASSERT_TRUE(second.Reserve(secondInput, &secondToken, &secondOriginal));
    ASSERT_TRUE(firstOriginal.Borrow(&firstLoan));
    ASSERT_TRUE(secondOriginal.Borrow(&secondLoan));
    OriginalDecisionPin moved(std::move(firstLoan));
    EXPECT_FALSE(firstLoan.Valid());
    EXPECT_TRUE(first.IsOriginalDecisionLifetimePinFor(moved, firstToken));
    moved = std::move(secondLoan);
    EXPECT_FALSE(secondLoan.Valid());
    EXPECT_FALSE(first.IsOriginalDecisionLifetimePinFor(moved, firstToken));
    EXPECT_TRUE(second.IsOriginalDecisionLifetimePinFor(moved, secondToken));
    OriginalDecisionObservation a{}, b{};
    ASSERT_TRUE(ObserveOriginalDecision(first, firstToken, &a));
    ASSERT_TRUE(ObserveOriginalDecision(second, secondToken, &b));
    EXPECT_EQ(1u, a.count);
    EXPECT_EQ(2u, b.count);
    {
        OriginalDecisionPin scoped;
        ASSERT_TRUE(moved.Borrow(&scoped));
        ASSERT_TRUE(ObserveOriginalDecision(second, secondToken, &b));
        EXPECT_EQ(3u, b.count);
    }
    ASSERT_TRUE(ObserveOriginalDecision(second, secondToken, &b));
    EXPECT_EQ(2u, b.count);
    ASSERT_TRUE(moved.Release());
    OriginalDecisionPin refused;
    EXPECT_FALSE(moved.Borrow(&refused));
    ASSERT_TRUE(first.Cancel(firstToken));
    ASSERT_TRUE(first.ReclaimCancelled(firstToken, &firstOriginal));
    ASSERT_TRUE(second.Cancel(secondToken));
    ASSERT_TRUE(second.ReclaimCancelled(secondToken, &secondOriginal));
}

TEST(DecisionGroup, OriginalDecisionReturnedLoanKeepsTruePreparedIssuerAndLegacyCannotUpgrade) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(1, 1);
    const auto input = MakeGenuineOriginalDecisionRequest(commits, true);
    harken::PreparedDecisionReservation prepared;
    OriginalDecisionPin original, absent;
    ASSERT_TRUE(table.PrepareReservation(input, &prepared, &original));
    const auto token = prepared.Token();
    OriginalDecisionObservation before{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &before));
    ASSERT_TRUE(original.Release());
    EXPECT_FALSE(original.Borrow(&absent));
    OriginalDecisionObservation after{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &after));
    EXPECT_EQ(before.marker, after.marker);
    EXPECT_EQ(0u, after.count);
    EXPECT_TRUE(SameOriginalDecisionInput(before.request, after.request));
    const auto sequence = commits.Issue();
    harken::DecisionToken ticketed{};
    ASSERT_TRUE(table.CommitPreparedReservation(&prepared, sequence, &ticketed));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_EQ(token.slot, ticketed.slot);
    EXPECT_EQ(token.generation, ticketed.generation);
    ASSERT_TRUE(table.Cancel(token));
    ASSERT_TRUE(table.ReclaimCancelled(token));
    harken::DecisionToken reused{};
    const auto fresh = MakeGenuineOriginalDecisionRequest(commits);
    ASSERT_TRUE(table.Reserve(fresh, &reused));
    EXPECT_NE(token.generation, reused.generation);
    EXPECT_FALSE(table.IsOriginalDecisionLifetimePinFor(original, reused));
    EXPECT_FALSE(table.MatchesOriginalDecisionFactory(original, fresh));
    harken::DecisionFuturePin legacy;
    ASSERT_TRUE(table.AcquireFuturePin(reused, &legacy));
    EXPECT_FALSE(absent.Valid());
    EXPECT_FALSE(absent.Borrow(&original));
    ASSERT_TRUE(ObserveOriginalDecision(table, reused, &after));
    EXPECT_EQ(0u, after.marker);
    EXPECT_EQ(0u, after.count);
    EXPECT_EQ(1u, after.futureCount);
    ASSERT_TRUE(legacy.Release());
    ASSERT_TRUE(table.Cancel(reused));
    ASSERT_TRUE(table.ReclaimCancelled(reused));
}

TEST(DecisionGroup, OriginalDecisionLimitRefusalsRestoreOnlyGenuineNegativePrerequisites) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(1, 1);
    const auto request = MakeGenuineOriginalDecisionRequest(commits);
    harken::DecisionToken token{};
    OriginalDecisionPin original, refused;
    ASSERT_TRUE(table.Reserve(request, &token, &original));
    {
        harken::detail::DecisionOriginalLifetimeTestAccess::BorrowLimit limit(table, original);
        ASSERT_TRUE(limit.Ready());
        EXPECT_FALSE(original.Borrow(&refused));
        EXPECT_FALSE(refused.Valid());
    }
    OriginalDecisionObservation restored{};
    ASSERT_TRUE(ObserveOriginalDecision(table, token, &restored));
    EXPECT_EQ(1u, restored.count);
    ASSERT_TRUE(original.Borrow(&refused));
    ASSERT_TRUE(refused.Release());
    ASSERT_TRUE(table.Cancel(token));
    ASSERT_TRUE(table.ReclaimCancelled(token, &original));
    for (const bool zeroBudget : {false, true}) {
        {
            harken::detail::DecisionOriginalLifetimeTestAccess::IssuerLimit limit(table, zeroBudget);
            ASSERT_TRUE(limit.Ready());
            harken::DecisionToken unchanged{31, 37};
            const auto input = MakeGenuineOriginalDecisionRequest(commits);
            EXPECT_FALSE(table.Reserve(input, &unchanged, &original));
            EXPECT_EQ(31u, unchanged.slot);
            EXPECT_EQ(37u, unchanged.generation);
            harken::PreparedDecisionReservation prepared;
            auto preparing = input;
            preparing.ticket = {};
            EXPECT_FALSE(table.PrepareReservation(preparing, &prepared, &original));
            EXPECT_FALSE(prepared.Valid());
            EXPECT_FALSE(original.Valid());
            harken::DecisionToken raw{};
            ASSERT_TRUE(table.Reserve(input, &raw));
            ASSERT_TRUE(table.Cancel(raw));
            ASSERT_TRUE(table.ReclaimCancelled(raw));
        }
        const auto restoredInput = MakeGenuineOriginalDecisionRequest(commits);
        ASSERT_TRUE(table.Reserve(restoredInput, &token, &original));
        ASSERT_TRUE(table.MatchesOriginalDecisionFactory(original, restoredInput));
        ASSERT_TRUE(table.Cancel(token));
        ASSERT_TRUE(table.ReclaimCancelled(token, &original));
    }
}

TEST(DecisionGroup, OriginalDecisionFinalGenerationDisposesAuthenticBaselineWithoutWrap) {
    harken::LogicCommitCoordinator commits;
    harken::DecisionTable table(2, 1);
    ASSERT_TRUE(table.SetGenerationForTests(0, UINT32_MAX));
    const auto request = MakeGenuineOriginalDecisionRequest(commits);
    harken::DecisionToken last{};
    OriginalDecisionPin original, foreign;
    ASSERT_TRUE(table.Reserve(request, &last, &original));
    ASSERT_EQ(UINT32_MAX, last.generation);
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_TRUE(table.Cancel(last));
    EXPECT_FALSE(table.ReclaimCancelled(last, &original));
    EXPECT_TRUE(table.Contains(last));
    EXPECT_EQ(0u, table.QuarantinedCount());
    ASSERT_TRUE(foreign.Release());
    // True disposal succeeded, but this native slot can never be reused.
    ASSERT_TRUE(table.ReclaimCancelled(last, &original));
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(table.Contains(last));
    EXPECT_EQ(1u, table.QuarantinedCount());
    harken::DecisionToken next{};
    const auto nextInput = MakeGenuineOriginalDecisionRequest(commits);
    ASSERT_TRUE(table.Reserve(nextInput, &next, &original));
    EXPECT_NE(last.slot, next.slot);
    EXPECT_EQ(1u, next.generation);
    EXPECT_FALSE(table.IsOriginalDecisionLifetimePinFor(original, last));
    EXPECT_FALSE(table.Cancel(last));
    ASSERT_TRUE(table.Cancel(next));
    ASSERT_TRUE(table.ReclaimCancelled(next, &original));
    harken::DecisionTable rawTable(1, 1);
    ASSERT_TRUE(rawTable.SetGenerationForTests(0, UINT32_MAX));
    harken::DecisionToken raw{};
    ASSERT_TRUE(rawTable.Reserve(MakeGenuineOriginalDecisionRequest(commits), &raw));
    ASSERT_TRUE(rawTable.Cancel(raw));
    EXPECT_FALSE(rawTable.ReclaimCancelled(raw));
    EXPECT_EQ(1u, rawTable.QuarantinedCount());
}

} // namespace

// Real native Group/Decision factories and Apply/fence cleanup; no Queue clock grant.
TEST(DecisionGroup, OriginalQueueForeignLoanAllowsGenuinePlacementProofReanchorAndTrueOldApply) {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms(1);
    DecisionLaneHarness laneHarness;
    ASSERT_NO_FATAL_FAILURE(laneHarness.ReserveDecisions(1));
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = laneHarness.tokens[0];
    harken::Plan03GroupSliceIdentity raw{};
    harken::PreparedPrivateImmediateLaneGroup original;
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &raw, &original));
    harken::PrivateImmediateLaneGroupTransferProof proof;
    ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, 17, 100, input, arms, &proof));
    harken::OriginalQueueFutureGroupRegistration registration;
    ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(std::move(proof), &registration));
    harken::OriginalQueueFutureGroupLifetimePin lifetime;
    ASSERT_TRUE(registration.BorrowLifetime(&lifetime));
    ASSERT_NO_FATAL_FAILURE(laneHarness.Bind(pool, &raw, 1));
    ASSERT_TRUE(laneHarness.lane.DeclareGroup(laneHarness.handle, {raw, 20}));
    ASSERT_TRUE(laneHarness.lane.Prepare(laneHarness.handle, nullptr, 0));
    harken::PrivateImmediateLanePlacementProof placement;
    std::uint64_t actualAnchor = UINT64_MAX;
    ASSERT_TRUE(laneHarness.lane.TryPlace(laneHarness.handle, 500, 0, &actualAnchor, &placement));
    ASSERT_EQ(500u, actualAnchor);
    ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, actualAnchor));
    EXPECT_TRUE(registration.Valid());
    EXPECT_TRUE(lifetime.Valid());
    EXPECT_TRUE(registration.MatchesOriginalScope(17, 100, input, arms));
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = raw.decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 520;
    binding.operation.group = raw;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = raw;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw, binding));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(pool.AcquirePlan03GroupReader(raw, &reader));
    ASSERT_TRUE(laneHarness.lane.PinGroups(laneHarness.handle, pool, laneHarness.decisions, 2));
    ASSERT_TRUE(laneHarness.lane.PinArms(laneHarness.handle, arms));
    EXPECT_FALSE(laneHarness.lane.Publish(laneHarness.handle));
    placement.Release();
    ASSERT_TRUE(pool.PublishImmediateLaneGroups(17));
    ASSERT_TRUE(laneHarness.lane.Publish(laneHarness.handle));
    harken::Plan03PageGroupView view{};
    ASSERT_TRUE(pool.ResolvePlan03GroupSliceRt(raw, &view));
    std::array<harken::Plan03PageGroupView, 1> due{};
    ASSERT_EQ(1u, pool.CaptureImmediateLaneGroupsRt(5, due.data(), due.size()));
    EXPECT_EQ(raw, due[0].identity);
    harken::ImmediateLaneFrontierClaim frontierClaim{};
    ASSERT_TRUE(pool.ClaimImmediateLaneFrontier(raw, &frontierClaim));
    ASSERT_TRUE(pool.NotePlan03GroupReadRt(raw, 1));
    std::uint32_t applied = 0;
    SliceStep step{view.lease, &applied};
    harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
              harken::ApplyAudioSliceWhole(*view.lease, &operation, 1));
    EXPECT_EQ(1u, applied);
    EXPECT_EQ(harken::AudioSliceState::SliceApplied, view.lease->Capture().state);
    EXPECT_TRUE(registration.Valid());
    EXPECT_TRUE(lifetime.Valid());
    ASSERT_TRUE(pool.ReleaseImmediateLaneFrontier(&frontierClaim));
    ASSERT_TRUE(laneHarness.lane.Cancel(laneHarness.handle));
    ASSERT_TRUE(laneHarness.lane.Retire(laneHarness.handle, 2));
    EXPECT_FALSE(pool.DetachPlan03GroupFrontier(raw, 1));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw, 2));
    EXPECT_FALSE(reader.Release(1));
    ASSERT_TRUE(reader.Release(2));
    registration.Release();
    EXPECT_TRUE(lifetime.Valid());
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 2));
    lifetime.Release();
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 1));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 2));
    EXPECT_FALSE(pool.ResolvePlan03GroupSliceProducer(raw, &view));
    ASSERT_TRUE(laneHarness.decisions.Cancel(laneHarness.tokens[0]));
    ASSERT_TRUE(laneHarness.decisions.ReclaimCancelled(laneHarness.tokens[0]));
    EXPECT_EQ(0u, laneHarness.decisions.LiveCount());
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
}


namespace {

using OriginalGroupClockProof = harken::PrivateOriginalQueueGroupClockProof;

// Empty real native groups exercise only this Pool/Lane structural unit.
// They are not physical Current/Source/Queue readiness or ACK fixtures.
struct GenuineOriginalGroupClockHarness {
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{1};
    harken::DecisionTable decisions{6, 2};
    harken::ImmediateActionLane lane;
    harken::ImmediateLaneHandle handle{};
    std::array<harken::DecisionToken, 6> tokens{};
    std::array<harken::Plan03GroupSliceInput, 6> inputs{};
    std::array<harken::Plan03GroupSliceIdentity, 6> raw{};
    std::array<harken::OriginalQueueFutureGroupRegistration, 6> registrations{};
    std::array<harken::FrontierGroupBinding, 4> bindings{};
    harken::PrivateImmediateLanePlacementProof placement;
    std::uint32_t count = 0;
    std::uint32_t second = 0;
    std::uint64_t anchor = UINT64_MAX;

    void ReserveOriginal(std::uint32_t index, std::uint64_t root,
                         std::uint64_t relative) {
        ASSERT_TRUE(decisions.Reserve(MakeRequest(index + 1), &tokens[index]));
        inputs[index].executeFrame = 100 + relative;
        inputs[index].decision = tokens[index];
        harken::PreparedPrivateImmediateLaneGroup original;
        ASSERT_TRUE(pool.ReserveImmediateLaneGroup(root, 100, inputs[index], arms,
                                                   &raw[index], &original));
        ASSERT_EQ(index, raw[index].committedRow);
        harken::PrivateImmediateLaneGroupTransferProof transfer;
        ASSERT_TRUE(pool.ProvePrivateImmediateLaneGroupTransfer(original, root, 100,
            inputs[index], arms, &transfer));
        ASSERT_TRUE(pool.ConsumePrivateImmediateLaneGroupTransfer(
            std::move(transfer), &registrations[index]));
        ASSERT_FALSE(original.Valid());
        ASSERT_TRUE(registrations[index].MatchesOriginalScope(root, 100, inputs[index], arms));
    }

    void Prepare(bool sharedPrimary, bool pinGroups = true) {
        auto config = MakePoolConfig();
        config.maxImmediateLaneGroupRows = 6;
        config.maxImmediateLaneOwnerGates = 8;
        config.maxImmediateLaneDecisionGates = 8;
        ASSERT_TRUE(pool.Reset(config, 0, 9));
        count = sharedPrimary ? 3u : 4u;
        second = sharedPrimary ? 2u : 3u;
        for (std::uint32_t i = 0; i < count; ++i)
            ASSERT_NO_FATAL_FAILURE(ReserveOriginal(i, 17, sharedPrimary || i < 2 ? 20 : 120));
        ASSERT_TRUE(lane.Reset({1, 4, 4, 100, false, 4}));
        ASSERT_TRUE(lane.Reserve(17, {0, count, 0, sharedPrimary ? 1u : 2u}, &handle));
        for (std::uint32_t i = 0; i < count; ++i) {
            ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(raw[i], lane, 17,
                handle.slot, handle.generation));
            ASSERT_TRUE(lane.DeclareGroup(handle, {raw[i], inputs[i].executeFrame - 100}));
        }
        ASSERT_TRUE(lane.Prepare(handle, nullptr, 0));
        ASSERT_TRUE(lane.TryPlace(handle, 400, 0, &anchor, &placement));
        ASSERT_EQ(400u, anchor);
        ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, anchor));
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto primary = sharedPrimary ? 0u : (i < 2 ? 0u : 2u);
            auto& binding = bindings[i];
            binding.role = i == primary ? harken::FrontierGroupRole::Coordinator :
                harken::FrontierGroupRole::EntryMember;
            binding.entry.coordinator = tokens[primary];
            binding.entry.attempt = primary + 1;
            binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
            binding.operation.kind = harken::FrontierOperationKind::Entry;
            binding.operation.owner = binding.entry.owner;
            binding.operation.order.logicCommitSequence = 1;
            binding.operation.order.executeFrame = anchor + inputs[i].executeFrame - 100;
            binding.operation.group = raw[i];
            binding.operation.transport = 9;
            binding.operation.world = 7;
            binding.primary = raw[primary];
            ASSERT_TRUE(pool.BindPlan03GroupFrontier(raw[i], binding));
        }
        if (pinGroups) ASSERT_NO_FATAL_FAILURE(Pin());
    }

    void Pin() {
        ASSERT_TRUE(lane.PinGroups(handle, pool, decisions, 4));
        ASSERT_TRUE(lane.PinArms(handle, arms));
    }

    bool Prove(OriginalGroupClockProof* out) {
        return pool.ProvePrivateOriginalQueueGroupClock(registrations[1],
            registrations[second], &placement, out);
    }

    harken::AudioSliceLease* Lease(std::uint32_t index) {
        harken::Plan03PageGroupView actual{};
        EXPECT_TRUE(pool.ResolvePlan03GroupSliceProducer(raw[index], &actual));
        return actual.lease;
    }

    void Cleanup(std::uint64_t completed = 0) {
        placement.Release();
        ASSERT_TRUE(lane.Cancel(handle));
        ASSERT_TRUE(lane.Retire(handle, completed));
        for (std::uint32_t i = count; i != 0; --i)
            ASSERT_TRUE(pool.DetachPlan03GroupFrontier(raw[i - 1], completed));
        ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, completed,
            registrations.data(), registrations.size()));
        for (std::uint32_t i = 0; i < count; ++i) {
            EXPECT_FALSE(registrations[i].Valid());
            ASSERT_TRUE(decisions.Cancel(tokens[i]));
            ASSERT_TRUE(decisions.ReclaimCancelled(tokens[i]));
        }
        EXPECT_EQ(0u, decisions.LiveCount());
        ASSERT_TRUE(pool.ResetForEpoch(0, 10));
    }
};

TEST(DecisionGroup, OriginalGroupClockSharedActualLeaseReturnReproveAndAllPrivateGates) {
    static_assert(!std::is_copy_constructible_v<OriginalGroupClockProof>);
    static_assert(!std::is_copy_assignable_v<OriginalGroupClockProof>);
    static_assert(!std::is_constructible_v<OriginalGroupClockProof, harken::Plan03GroupSliceIdentity>);
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true));
    auto* const primaryLease = state.Lease(0);
    ASSERT_EQ(primaryLease, state.Lease(1));
    ASSERT_EQ(primaryLease, state.Lease(2));
    harken::OriginalQueueFutureGroupLifetimePin foreign;
    ASSERT_TRUE(state.registrations[1].BorrowLifetime(&foreign));
    OriginalGroupClockProof proof;
    ASSERT_TRUE(state.Prove(&proof));
    EXPECT_FALSE(state.placement.Valid());
    ASSERT_TRUE(proof.Valid());
    EXPECT_TRUE(state.pool.IsPrivateOriginalQueueGroupClockProofFor(proof,
        state.registrations[1], state.registrations[2]));
    EXPECT_FALSE(state.pool.IsPrivateOriginalQueueGroupClockProofFor(proof,
        state.registrations[2], state.registrations[1]));
    EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, primaryLease->Capture().state);
    std::uint64_t captured = 71;
    ASSERT_TRUE(proof.CaptureAnchorFrame(&captured));
    EXPECT_EQ(400u, captured);
    for (std::uint32_t i = 0; i < state.count; ++i) {
        EXPECT_FALSE(state.pool.PrivatePlan03GroupBindable(state.raw[i]));
        EXPECT_FALSE(state.pool.PrivateImmediateLaneFrontierRollbackReady(state.raw[i]));
        EXPECT_FALSE(state.pool.PrivateImmediateLaneGroupRetirementReady(state.raw[i], 0));
        EXPECT_FALSE(state.pool.BindPlan03GroupFrontier(state.raw[i], state.bindings[i]));
        EXPECT_FALSE(state.pool.DetachPlan03GroupFrontier(state.raw[i], 0));
        EXPECT_FALSE(state.pool.BindImmediateLaneGroupOwner(state.raw[i], state.lane,
            17, state.handle.slot, state.handle.generation));
        EXPECT_FALSE(state.pool.CancelPrivateImmediateLaneGroup(17, state.raw[i],
            &state.registrations[i]));
        harken::Plan03GroupReaderPin reader;
        EXPECT_FALSE(state.pool.AcquirePlan03GroupReader(state.raw[i], &reader));
        harken::ImmediateLaneFrontierClaim claim{};
        EXPECT_FALSE(state.pool.ClaimPrivateImmediateLaneFrontier(state.raw[i], &claim));
        EXPECT_FALSE(claim.Valid());
    }
    EXPECT_FALSE(state.pool.AcquireImmediateLaneFutureDependent(state.raw[1]));
    EXPECT_FALSE(state.pool.ReleaseImmediateLaneFutureDependent(state.raw[1]));
    EXPECT_FALSE(state.pool.RebaseImmediateLaneGroups(17, 800));
    EXPECT_FALSE(state.pool.PublishImmediateLaneGroups(17));
    EXPECT_FALSE(state.pool.CancelImmediateLaneGroups(17, 0,
        state.registrations.data(), state.registrations.size()));
    EXPECT_FALSE(state.pool.ResetForEpoch(0, 10));
    EXPECT_FALSE(state.pool.Reset(MakePoolConfig(), 0, 10));
    EXPECT_FALSE(state.lane.Publish(state.handle));
    EXPECT_FALSE(state.lane.Cancel(state.handle));
    EXPECT_FALSE(proof.ReturnPlacement(nullptr));
    ASSERT_TRUE(proof.ReturnPlacement(&state.placement));
    EXPECT_FALSE(proof.Valid());
    EXPECT_TRUE(state.placement.Valid());
    EXPECT_EQ(harken::AudioSliceState::Ready, primaryLease->Capture().state);
    EXPECT_TRUE(foreign.Valid());
    ASSERT_TRUE(state.Prove(&proof));
    proof.Release();
    captured = 73;
    EXPECT_FALSE(proof.CaptureAnchorFrame(&captured));
    EXPECT_EQ(73u, captured);
    foreign.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(DecisionGroup, OriginalGroupClockDistinctActualLeasesPreserveForeignLoanOldApplyAndStrictFence) {
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false));
    EXPECT_EQ(state.Lease(0), state.Lease(1));
    EXPECT_EQ(state.Lease(2), state.Lease(3));
    EXPECT_NE(state.Lease(1), state.Lease(3));
    harken::OriginalQueueFutureGroupLifetimePin foreign;
    ASSERT_TRUE(state.registrations[3].BorrowLifetime(&foreign));
    harken::Plan03GroupReaderPin actualReader;
    ASSERT_TRUE(state.pool.AcquirePlan03GroupReader(state.raw[3], &actualReader));
    OriginalGroupClockProof proof;
    ASSERT_TRUE(state.Prove(&proof));
    for (std::uint32_t i = 0; i < state.count; ++i)
        EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, state.Lease(i)->Capture().state);
    proof.Release();
    ASSERT_TRUE(state.pool.PublishImmediateLaneGroups(17));
    ASSERT_TRUE(state.lane.Publish(state.handle));
    for (std::uint32_t primary : {0u, 2u}) {
        const auto quantum = primary == 0 ? 4u : 5u;
        const auto* bucket = state.lane.CaptureDue(state.handle.slot, quantum);
        ASSERT_NE(nullptr, bucket);
        ASSERT_TRUE(state.lane.Claim(state.handle, bucket));
        harken::ImmediateLaneFrontierClaim claim{};
        ASSERT_TRUE(state.pool.ClaimImmediateLaneFrontier(state.raw[primary], &claim));
        for (const auto i : {primary, primary + 1})
            ASSERT_TRUE(state.pool.NotePlan03GroupReadRt(state.raw[i], 1));
        std::uint32_t applied = 0;
        SliceStep step{state.Lease(primary), &applied};
        harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
        ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
            harken::ApplyAudioSliceWhole(*step.lease, &operation, 1));
        EXPECT_EQ(1u, applied);
        ASSERT_TRUE(state.pool.ReleaseImmediateLaneFrontier(&claim));
        ASSERT_TRUE(state.lane.Finish(state.handle.slot, bucket, true));
    }
    ASSERT_TRUE(state.lane.Cancel(state.handle));
    EXPECT_FALSE(state.lane.Retire(state.handle, 1));
    ASSERT_TRUE(state.lane.Retire(state.handle, 2));
    EXPECT_FALSE(state.pool.DetachPlan03GroupFrontier(state.raw[3], 1));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[3], 2));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[2], 2));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[1], 2));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[0], 2));
    EXPECT_FALSE(actualReader.Release(1));
    ASSERT_TRUE(actualReader.Release(2));
    EXPECT_FALSE(state.pool.CancelImmediateLaneGroups(17, 2,
        state.registrations.data(), state.registrations.size()));
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(state.registrations[3].Valid());
    EXPECT_TRUE(state.registrations[2].Valid());
    foreign.Release();
    EXPECT_FALSE(state.pool.CancelImmediateLaneGroups(17, 1,
        state.registrations.data(), state.registrations.size()));
    ASSERT_TRUE(state.pool.CancelImmediateLaneGroups(17, 2,
        state.registrations.data(), state.registrations.size()));
    for (std::uint32_t i = 0; i < state.count; ++i) {
        ASSERT_TRUE(state.decisions.Cancel(state.tokens[i]));
        ASSERT_TRUE(state.decisions.ReclaimCancelled(state.tokens[i]));
    }
    ASSERT_TRUE(state.pool.ResetForEpoch(0, 10));
}

TEST(DecisionGroup, OriginalGroupClockRequiresActualPinnedPlacementReceiverOriginalsAndEmptyOutput) {
    GenuineOriginalGroupClockHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare(true, false));
    OriginalGroupClockProof refused;
    EXPECT_FALSE(first.Prove(&refused));
    EXPECT_TRUE(first.placement.Valid());
    ASSERT_NO_FATAL_FAILURE(first.Pin());
    ASSERT_NO_FATAL_FAILURE(second.Prepare(true));
    harken::OriginalQueueFutureGroupRegistration empty;
    EXPECT_FALSE(first.pool.ProvePrivateOriginalQueueGroupClock(empty, first.registrations[2],
        &first.placement, &refused));
    EXPECT_FALSE(first.pool.ProvePrivateOriginalQueueGroupClock(first.registrations[1],
        first.registrations[1], &first.placement, &refused));
    EXPECT_FALSE(second.pool.ProvePrivateOriginalQueueGroupClock(first.registrations[1],
        second.registrations[2], &second.placement, &refused));
    EXPECT_FALSE(first.pool.ProvePrivateOriginalQueueGroupClock(first.registrations[1],
        first.registrations[2], &second.placement, &refused));
    EXPECT_FALSE(first.pool.ProvePrivateOriginalQueueGroupClock(first.registrations[1],
        first.registrations[2], nullptr, &refused));
    EXPECT_FALSE(first.Prove(nullptr));
    std::uint64_t sentinel = 79;
    EXPECT_FALSE(refused.CaptureAnchorFrame(&sentinel));
    EXPECT_EQ(79u, sentinel);
    EXPECT_TRUE(first.placement.Valid());
    EXPECT_TRUE(second.placement.Valid());
    ASSERT_TRUE(first.Prove(&refused));
    EXPECT_FALSE(second.Prove(&refused));
    EXPECT_TRUE(second.placement.Valid());
    EXPECT_TRUE(first.pool.IsPrivateOriginalQueueGroupClockProofFor(refused,
        first.registrations[1], first.registrations[2]));
    EXPECT_FALSE(second.pool.IsPrivateOriginalQueueGroupClockProofFor(refused,
        second.registrations[1], second.registrations[2]));
    EXPECT_FALSE(refused.ReturnPlacement(&second.placement));
    EXPECT_TRUE(refused.Valid());
    EXPECT_TRUE(second.placement.Valid());
    ASSERT_TRUE(refused.ReturnPlacement(&first.placement));
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

TEST(DecisionGroup, OriginalGroupClockSecondActualLeaseRefusalReturnsHeldPrefixAndKeepsPlacement) {
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false));
    auto* const first = state.Lease(1);
    auto* const second = state.Lease(3);
    ASSERT_TRUE(second->TryClaim());
    OriginalGroupClockProof refused;
    EXPECT_FALSE(state.Prove(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(state.placement.Valid());
    EXPECT_EQ(harken::AudioSliceState::Ready, first->Capture().state);
    EXPECT_EQ(harken::AudioSliceState::SliceClaimed, second->Capture().state);
    // End the actual native winning private lease, without inventing an audio receipt.
    ASSERT_TRUE(second->MarkApplied());
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(DecisionGroup, OriginalGroupClockSecondGenuineBorrowLimitReturnsFirstBorrowAndRetries) {
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true));
    using Access = harken::detail::ScheduledTransactionPoolOriginalTestAccess;
    const auto firstCount = Access::PinCount(state.pool, state.raw[1].committedRow);
    OriginalGroupClockProof proof;
    {
        ActualOriginalGroupCountLimit limit(state.pool, state.registrations[2]);
        ASSERT_TRUE(limit.entered);
        EXPECT_FALSE(state.Prove(&proof));
        EXPECT_FALSE(proof.Valid());
        EXPECT_TRUE(state.placement.Valid());
        EXPECT_EQ(firstCount, Access::PinCount(state.pool, state.raw[1].committedRow));
        EXPECT_EQ(harken::AudioSliceState::Ready, state.Lease(0)->Capture().state);
    }
    ASSERT_TRUE(state.Prove(&proof));
    ASSERT_TRUE(proof.ReturnPlacement(&state.placement));
    EXPECT_EQ(firstCount, Access::PinCount(state.pool, state.raw[1].committedRow));
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}
#endif

TEST(DecisionGroup, OriginalGroupClockMoveOverwriteAndDestructorReturnExactNativeAssociations) {
    GenuineOriginalGroupClockHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare(true));
    ASSERT_NO_FATAL_FAILURE(second.Prepare(false));
    OriginalGroupClockProof firstProof, secondProof;
    ASSERT_TRUE(first.Prove(&firstProof));
    ASSERT_TRUE(second.Prove(&secondProof));
    {
        OriginalGroupClockProof moved(std::move(firstProof));
        EXPECT_FALSE(firstProof.Valid());
        EXPECT_TRUE(first.pool.IsPrivateOriginalQueueGroupClockProofFor(moved,
            first.registrations[1], first.registrations[2]));
        moved = std::move(secondProof);
        EXPECT_FALSE(secondProof.Valid());
        EXPECT_EQ(harken::AudioSliceState::Ready, first.Lease(0)->Capture().state);
        EXPECT_TRUE(first.pool.PrivatePlan03GroupBindable(first.raw[0]));
        EXPECT_TRUE(second.pool.IsPrivateOriginalQueueGroupClockProofFor(moved,
            second.registrations[1], second.registrations[3]));
        EXPECT_FALSE(second.pool.DetachPlan03GroupFrontier(second.raw[2], 0));
        EXPECT_FALSE(second.pool.PublishImmediateLaneGroups(17));
        EXPECT_FALSE(second.lane.Cancel(second.handle));
    }
    EXPECT_EQ(harken::AudioSliceState::Ready, second.Lease(0)->Capture().state);
    EXPECT_EQ(harken::AudioSliceState::Ready, second.Lease(2)->Capture().state);
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

TEST(DecisionGroup, OriginalGroupClockHeldDoesNotBlockActualOldApplyOrOwnerStopAndDoesNotGrantHealth) {
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(true));
    ASSERT_NO_FATAL_FAILURE(state.ReserveOriginal(3, 19, 20));
    harken::ImmediateActionLane oldLane;
    harken::ImmediateLaneHandle oldHandle{};
    ASSERT_TRUE(oldLane.Reset({1, 1, 1, 100, false, 1}));
    ASSERT_TRUE(oldLane.Reserve(19, {0, 1, 0, 1}, &oldHandle));
    ASSERT_TRUE(state.pool.BindImmediateLaneGroupOwner(state.raw[3], oldLane, 19,
        oldHandle.slot, oldHandle.generation));
    ASSERT_TRUE(oldLane.DeclareGroup(oldHandle, {state.raw[3], 20}));
    ASSERT_TRUE(oldLane.Prepare(oldHandle, nullptr, 0));
    std::uint64_t oldAnchor = UINT64_MAX;
    ASSERT_TRUE(oldLane.TryPlace(oldHandle, 300, 0, &oldAnchor));
    ASSERT_EQ(300u, oldAnchor);
    ASSERT_TRUE(state.pool.RebaseImmediateLaneGroups(19, oldAnchor));
    auto binding = state.bindings[0];
    binding.entry.coordinator = state.tokens[3];
    binding.entry.attempt = 9;
    binding.primary = state.raw[3];
    binding.operation.group = state.raw[3];
    binding.operation.order.executeFrame = 320;
    ASSERT_TRUE(state.pool.BindPlan03GroupFrontier(state.raw[3], binding));
    ASSERT_TRUE(oldLane.PinGroups(oldHandle, state.pool, state.decisions, 1));
    ASSERT_TRUE(oldLane.PinArms(oldHandle, state.arms));
    ASSERT_TRUE(state.pool.PublishImmediateLaneGroups(19));
    ASSERT_TRUE(oldLane.Publish(oldHandle));
    OriginalGroupClockProof proof;
    ASSERT_TRUE(state.Prove(&proof));
    const auto* bucket = oldLane.CaptureDue(oldHandle.slot, 3);
    ASSERT_NE(nullptr, bucket);
    ASSERT_TRUE(oldLane.Claim(oldHandle, bucket));
    harken::ImmediateLaneFrontierClaim claim{};
    ASSERT_TRUE(state.pool.ClaimImmediateLaneFrontier(state.raw[3], &claim));
    ASSERT_TRUE(state.pool.NotePlan03GroupReadRt(state.raw[3], 1));
    std::uint32_t applied = 0;
    SliceStep step{state.Lease(3), &applied};
    harken::AudioSliceOperation operation{&step, ValidateSliceStep, nullptr, ApplySliceStep};
    ASSERT_EQ(harken::AudioSliceApplyStatus::Applied,
        harken::ApplyAudioSliceWhole(*step.lease, &operation, 1));
    EXPECT_EQ(1u, applied);
    EXPECT_FALSE(state.pool.InvalidateFrontierOwnerOperations(binding.entry.owner));
    EXPECT_TRUE(proof.Valid());
    state.pool.SettleInvalidatedImmediateLaneFrontiers();
    EXPECT_EQ(harken::AudioSliceState::PrivateTransferReserved, state.Lease(1)->Capture().state);
    ASSERT_TRUE(state.pool.ReleaseImmediateLaneFrontier(&claim));
    ASSERT_TRUE(oldLane.Finish(oldHandle.slot, bucket, true));
    ASSERT_TRUE(oldLane.Cancel(oldHandle));
    EXPECT_FALSE(oldLane.Retire(oldHandle, 1));
    ASSERT_TRUE(oldLane.Retire(oldHandle, 2));
    EXPECT_FALSE(state.pool.DetachPlan03GroupFrontier(state.raw[3], 1));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[3], 2));
    ASSERT_TRUE(state.pool.CancelImmediateLaneGroups(19, 2,
        state.registrations.data(), state.registrations.size()));
    ASSERT_TRUE(state.decisions.Cancel(state.tokens[3]));
    ASSERT_TRUE(state.decisions.ReclaimCancelled(state.tokens[3]));
    ASSERT_TRUE(proof.ReturnPlacement(&state.placement));
    // The real gate was invalidated by normal owner Stop. No clock observation
    // restores it, and a new proof issuance still requires actual healthy gates.
    EXPECT_FALSE(state.Prove(&proof));
    EXPECT_TRUE(state.placement.Valid());
    EXPECT_FALSE(proof.Valid());
    state.pool.SettleInvalidatedImmediateLaneFrontiers();
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, state.Lease(1)->Capture().state);
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

} // namespace


namespace {
void ReserveSelectedDomainOriginal(GenuineOriginalGroupClockHarness& state,
    harken::PrivateSelectedQueueFutureDomain& domain, std::uint32_t index) {
    ASSERT_TRUE(state.decisions.Reserve(MakeRequest(index + 1), &state.tokens[index]));
    state.inputs[index].executeFrame = 220;
    state.inputs[index].decision = state.tokens[index];
    harken::PreparedPrivateImmediateLaneGroup prepared;
    ASSERT_TRUE(domain.ReserveGroup(100, state.inputs[index], state.arms,
        &state.raw[index], &prepared));
    harken::PrivateImmediateLaneGroupTransferProof transfer;
    ASSERT_TRUE(state.pool.ProvePrivateImmediateLaneGroupTransfer(prepared,
        domain.RootObservation(), 100, state.inputs[index], state.arms, &transfer));
    ASSERT_TRUE(state.pool.ConsumePrivateImmediateLaneGroupTransfer(
        std::move(transfer), &state.registrations[index]));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_TRUE(domain.OwnsOriginal(state.registrations[index]));
}
void DisposeSelectedTestDecision(GenuineOriginalGroupClockHarness& state, std::uint32_t i) {
    ASSERT_TRUE(state.decisions.Cancel(state.tokens[i]));
    ASSERT_TRUE(state.decisions.ReclaimCancelled(state.tokens[i]));
}
}

TEST(DecisionGroup, SelectedFutureDomainSeparatesARealSameNumericOrdinaryCohort) {
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false, false));
    harken::ImmediateActionLane newLane;
    ASSERT_TRUE(newLane.Reset({1, 2, 2, 100, false, 2}));
    harken::LogicCommitCoordinator commits;
    harken::LogicCommitCoordinator::PreparedIssue sequence;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, commits.PrepareIssue(&sequence));
    harken::PrivateSelectedQueueFutureDomain domain;
    ASSERT_TRUE(state.pool.PreparePrivateSelectedQueueFutureDomain(state.registrations[1],
        state.registrations[3], sequence, newLane, &domain));
    ASSERT_NO_FATAL_FAILURE(state.ReserveOriginal(4, domain.RootObservation(), 120));
    ASSERT_NO_FATAL_FAILURE(ReserveSelectedDomainOriginal(state, domain, 5));
    harken::ImmediateActionLane ordinaryLane;
    harken::ImmediateLaneHandle ordinaryHandle{};
    ASSERT_TRUE(ordinaryLane.Reset({1, 1, 1, 100, false, 1}));
    ASSERT_TRUE(ordinaryLane.Reserve(domain.RootObservation(), {0, 1, 0, 1}, &ordinaryHandle));
    EXPECT_FALSE(state.pool.BindImmediateLaneGroupOwner(state.raw[5], ordinaryLane,
        domain.RootObservation(), ordinaryHandle.slot, ordinaryHandle.generation));
    ASSERT_TRUE(state.pool.BindImmediateLaneGroupOwner(state.raw[4], ordinaryLane,
        domain.RootObservation(), ordinaryHandle.slot, ordinaryHandle.generation));
    ASSERT_TRUE(ordinaryLane.DeclareGroup(ordinaryHandle, {state.raw[4], 120}));
    ASSERT_TRUE(ordinaryLane.Prepare(ordinaryHandle, nullptr, 0));
    ASSERT_TRUE(ordinaryLane.TryPlaceExactPrivate(ordinaryHandle, 400));
    ASSERT_TRUE(state.pool.RebaseImmediateLaneGroups(domain.RootObservation(), 400));
    auto binding = state.bindings[0];
    binding.entry.coordinator = state.tokens[4];
    binding.entry.attempt = 5;
    binding.operation.group = state.raw[4];
    binding.operation.order.executeFrame = 520;
    binding.primary = state.raw[4];
    ASSERT_TRUE(state.pool.BindPlan03GroupFrontier(state.raw[4], binding));
    ASSERT_TRUE(ordinaryLane.PinGroups(ordinaryHandle, state.pool, state.decisions, 1));
    ASSERT_TRUE(ordinaryLane.PinArms(ordinaryHandle, state.arms));
    ASSERT_TRUE(ordinaryLane.Cancel(ordinaryHandle));
    ASSERT_TRUE(ordinaryLane.Retire(ordinaryHandle, 0));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[4], 0));
    EXPECT_FALSE(state.pool.IsOriginalQueueFutureGroupFamily(state.registrations[4],
        state.registrations[5]));
    ASSERT_TRUE(state.pool.CancelImmediateLaneGroups(domain.RootObservation(), 0,
        state.registrations.data(), state.registrations.size()));
    EXPECT_FALSE(state.registrations[4].Valid());
    EXPECT_TRUE(state.registrations[5].Valid());
    EXPECT_FALSE(domain.Return());
    ASSERT_TRUE(domain.CancelGroups(0, state.registrations.data(), state.registrations.size()));
    ASSERT_TRUE(domain.Return());
    ASSERT_NO_FATAL_FAILURE(DisposeSelectedTestDecision(state, 4));
    ASSERT_NO_FATAL_FAILURE(DisposeSelectedTestDecision(state, 5));
    sequence.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(DecisionGroup, SelectedFutureDomainOwnCalendarCapacityAndForeignAttemptTailAreNative) {
    GenuineOriginalGroupClockHarness state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(false, false));
    harken::ImmediateActionLane newLane;
    ASSERT_TRUE(newLane.Reset({1, 2, 2, 100, false, 2}));
    harken::LogicCommitCoordinator commits;
    harken::LogicCommitCoordinator::PreparedIssue sequence;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, commits.PrepareIssue(&sequence));
    harken::PrivateSelectedQueueFutureDomain domain;
    ASSERT_TRUE(state.pool.PreparePrivateSelectedQueueFutureDomain(state.registrations[1],
        state.registrations[3], sequence, newLane, &domain));
    ASSERT_NO_FATAL_FAILURE(ReserveSelectedDomainOriginal(state, domain, 4));
    ASSERT_NO_FATAL_FAILURE(ReserveSelectedDomainOriginal(state, domain, 5));
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin baseline, foreign;
    ASSERT_TRUE(domain.ReserveCalendar({0, 2, 0, 1}, &handle, &baseline));
    ASSERT_TRUE(baseline.Borrow(&foreign));
    for (const auto index : {4u, 5u}) {
        ASSERT_TRUE(domain.BindGroup(state.raw[index], handle.slot, handle.generation));
        ASSERT_TRUE(newLane.DeclareGroup(handle, {state.raw[index], 120}));
    }
    ASSERT_TRUE(newLane.Prepare(handle, nullptr, 0));
    ASSERT_TRUE(newLane.TryPlaceExactPrivate(handle, 400));
    ASSERT_TRUE(domain.Rebase(400));
    for (const auto index : {4u, 5u}) {
        auto binding = state.bindings[2];
        binding.role = index == 4u ? harken::FrontierGroupRole::Coordinator :
            harken::FrontierGroupRole::EntryMember;
        binding.entry.coordinator = state.tokens[4];
        binding.entry.attempt = 5;
        binding.operation.group = state.raw[index];
        binding.operation.order.executeFrame = 520;
        binding.primary = state.raw[4];
        ASSERT_TRUE(state.pool.BindPlan03GroupFrontier(state.raw[index], binding));
    }
    std::array<harken::ImmediateLaneGroupRef, 2> ordinary{};
    EXPECT_EQ(0u, state.pool.CaptureImmediateLaneCohortGroups(
        domain.RootObservation(), ordinary.data(), ordinary.size()));
    EXPECT_FALSE(newLane.PinGroups(handle, state.pool, state.decisions, 1));
    ASSERT_TRUE(newLane.PinGroups(handle, state.pool, state.decisions, 2));
    ASSERT_TRUE(newLane.PinArms(handle, state.arms));
    ASSERT_TRUE(domain.PublishGroups());
    harken::Plan03PageGroupView hidden{};
    EXPECT_FALSE(state.pool.ResolvePlan03GroupSliceRt(state.raw[4], &hidden));
    ASSERT_TRUE(newLane.Cancel(handle));
    harken::Plan03PageGroupView ownPrimary{};
    ASSERT_TRUE(state.pool.ResolvePlan03GroupSliceProducer(state.raw[4], &ownPrimary));
    ASSERT_NE(nullptr, ownPrimary.lease);
    ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
        ownPrimary.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_FALSE(newLane.Retire(handle, 0, &baseline));
    EXPECT_TRUE(baseline.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_FALSE(domain.Return());
    foreign.Return();
    ASSERT_TRUE(newLane.Retire(handle, 0, &baseline));
    EXPECT_FALSE(domain.Return());
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[5], 0));
    ASSERT_TRUE(state.pool.DetachPlan03GroupFrontier(state.raw[4], 0));
    ASSERT_TRUE(domain.CancelGroups(0, state.registrations.data(), state.registrations.size()));
    ASSERT_TRUE(domain.Return());
    ASSERT_NO_FATAL_FAILURE(DisposeSelectedTestDecision(state, 4));
    ASSERT_NO_FATAL_FAILURE(DisposeSelectedTestDecision(state, 5));
    sequence.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(DecisionGroup, SelectedFutureDomainRequiresActualOldFamilyAndDistinctLaneReceiver) {
    GenuineOriginalGroupClockHarness first, second;
    ASSERT_NO_FATAL_FAILURE(first.Prepare(false, false));
    ASSERT_NO_FATAL_FAILURE(second.Prepare(false, false));
    harken::ImmediateActionLane newLane;
    ASSERT_TRUE(newLane.Reset({1, 2, 2, 100, false, 2}));
    harken::LogicCommitCoordinator commits;
    harken::LogicCommitCoordinator::PreparedIssue sequence;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, commits.PrepareIssue(&sequence));
    harken::PrivateSelectedQueueFutureDomain rejected, domain;
    EXPECT_FALSE(first.pool.PreparePrivateSelectedQueueFutureDomain(first.registrations[1],
        first.registrations[3], sequence, first.lane, &rejected));
    EXPECT_FALSE(first.pool.PreparePrivateSelectedQueueFutureDomain(first.registrations[1],
        second.registrations[3], sequence, newLane, &rejected));
    EXPECT_FALSE(rejected.Valid());
    ASSERT_TRUE(first.pool.PreparePrivateSelectedQueueFutureDomain(first.registrations[1],
        first.registrations[3], sequence, newLane, &domain));
    EXPECT_FALSE(first.pool.PreparePrivateSelectedQueueFutureDomain(first.registrations[1],
        first.registrations[3], sequence, newLane, &domain));
    harken::PrivateSelectedQueueFutureDomain moved(std::move(domain));
    EXPECT_FALSE(domain.Valid());
    ASSERT_TRUE(moved.Return());
    sequence.Release();
    ASSERT_NO_FATAL_FAILURE(first.Cleanup());
    ASSERT_NO_FATAL_FAILURE(second.Cleanup());
}

namespace {
using MemberOriginAccess = harken::detail::OriginalGroupMemberTestAccess;
struct NativeGroupOriginFixture {
    harken::TimelineArmTable arms{2};
    harken::DecisionTable decisions{2, 2};
    harken::ScheduledTransactionPool pool;
    harken::DecisionToken decision{};
    harken::TimelineArmToken arm{};
    harken::ScheduledTransactionPool::Config Config() {
        auto config = MakePoolConfig();
        config.maxPlan03GroupHeadersPerPage = 3;
        config.maxImmediateLaneGroupRows = 2;
        config.maxImmediateLaneOwnerGates = 4;
        config.maxImmediateLaneDecisionGates = 4;
        return config;
    }
    void Init() {
        ASSERT_TRUE(pool.Reset(Config(), 0, 9));
        ASSERT_TRUE(decisions.Reserve(MakeRequest(), &decision));
        ASSERT_TRUE(arms.PreparePendingImmediateGrouped(10, 1, 7, {}, {decision}, 0, &arm));
    }
    harken::Plan03GroupSliceInput Input(std::uint32_t slice = 0) {
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = 200;
        input.decision = decision;
        input.sliceIndex = slice;
        input.members = &arm;
        input.memberCount = 1;
        return input;
    }
    void Prepare(harken::ScheduledTransactionPool& target, std::uint32_t slice,
        harken::PreparedPlan03GroupSlice& prepared, harken::Plan03GroupSliceIdentity& identity) {
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
            target.PreparePlan03GroupSlice(Input(slice), arms, &prepared));
        identity = MemberOriginAccess::ObservePreparedOrigin(target, decision, slice);
        ASSERT_TRUE(identity.Exact());
    }
};
}

TEST(DecisionGroup, NativeMemberOriginsCoverActualPageImmediateAndForeignPoolTogether) {
    NativeGroupOriginFixture f;
    ASSERT_NO_FATAL_FAILURE(f.Init());
    harken::ScheduledTransactionPool other;
    ASSERT_TRUE(other.Reset(f.Config(), 0, 9));
    harken::PreparedPlan03GroupSlice first, foreign;
    harken::Plan03GroupSliceIdentity firstId{}, foreignId{}, immediateId{};
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 0, first, firstId));
    ASSERT_NO_FATAL_FAILURE(f.Prepare(other, 1, foreign, foreignId));
    ASSERT_TRUE(f.pool.ReserveImmediateLaneGroup(17, 100, f.Input(2), f.arms, &immediateId));
    const auto* a = MemberOriginAccess::Member(f.pool, firstId);
    const auto* b = MemberOriginAccess::Member(other, foreignId);
    const auto* c = MemberOriginAccess::Member(f.pool, immediateId);
    EXPECT_EQ(3u, MemberOriginAccess::Count(f.arms, f.arm));
    EXPECT_FALSE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{a, b}));
    EXPECT_FALSE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{a, b, b}));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{a, b, c}));
    harken::TimelineArmTable foreignReceiver(2);
    EXPECT_FALSE(MemberOriginAccess::Covers(foreignReceiver, f.arm, std::array{a, b, c}));
    ASSERT_TRUE(other.CancelPreparedPlan03GroupSlice(&foreign));
    EXPECT_EQ(2u, MemberOriginAccess::Count(f.arms, f.arm));
    EXPECT_FALSE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{a, b}));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(f.pool.AcquirePlan03GroupReader(immediateId, &reader));
    EXPECT_FALSE(f.pool.CancelPrivateImmediateLaneGroup(17, immediateId));
    EXPECT_EQ(2u, MemberOriginAccess::Count(f.arms, f.arm));
    ASSERT_TRUE(reader.Release(0));
    ASSERT_TRUE(f.pool.CancelPrivateImmediateLaneGroup(17, immediateId));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{a}));
    ASSERT_TRUE(f.pool.CancelPreparedPlan03GroupSlice(&first));
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, f.arm));
}

TEST(DecisionGroup, NativeMemberCommitKeepsOriginalSerialAndOwnershipReleaseKeepsRealObligation) {
    NativeGroupOriginFixture f;
    ASSERT_NO_FATAL_FAILURE(f.Init());
    harken::PreparedPlan03GroupSlice prepared;
    harken::Plan03GroupSliceIdentity identity{};
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 0, prepared, identity));
    const auto* original = MemberOriginAccess::Member(f.pool, identity);
    const auto serial = MemberOriginAccess::Serial(original);
    harken::CommittedPlan03GroupSlice committed;
    ASSERT_TRUE(f.pool.CommitPreparedPlan03GroupSlice(&prepared, &committed));
    EXPECT_EQ(serial, MemberOriginAccess::Serial(original));
    identity = MemberOriginAccess::Group(original);
    harken::Plan03ExactGroupSnapshot actual{};
    ASSERT_TRUE(f.pool.CaptureExactPlan03Group(identity, &actual));
    EXPECT_EQ(identity, actual.identity);
    EXPECT_EQ(1u, MemberOriginAccess::Count(f.arms, f.arm));
    ASSERT_TRUE(f.pool.ReleaseCommittedPlan03GroupSliceOwnership(&committed));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{original}));
    f.pool.DropPreparedOccupancy();
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, f.arm));
    EXPECT_EQ(0u, MemberOriginAccess::Serial(original));
}

TEST(DecisionGroup, NativeOldMemberReturnAfterStrictArmReclaimNeverDecrementsReusedGeneration) {
    NativeGroupOriginFixture f;
    ASSERT_NO_FATAL_FAILURE(f.Init());
    harken::PreparedPlan03GroupSlice oldGroup, newGroup;
    harken::Plan03GroupSliceIdentity oldId{}, newId{};
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 0, oldGroup, oldId));
    const auto oldArm = f.arm;
    ASSERT_TRUE(f.arms.CommitPrepared(oldArm));
    ASSERT_TRUE(f.arms.AcknowledgeApplied(oldArm, 1));
    ASSERT_TRUE(f.arms.BindPendingImmediateStart(oldArm, 200, 100));
    ASSERT_TRUE(f.arms.Publish(oldArm));
    ASSERT_TRUE(f.arms.RequestCancel(oldArm));
    ASSERT_TRUE(f.arms.Tombstone(oldArm, 1));
    EXPECT_FALSE(f.arms.Reclaim(oldArm, 1));
    ASSERT_TRUE(f.arms.Reclaim(oldArm, 2));
    ASSERT_TRUE(f.arms.PreparePendingImmediateGrouped(10, 2, 7, {}, {f.decision}, 0, &f.arm));
    ASSERT_EQ(oldArm.slot, f.arm.slot);
    ASSERT_NE(oldArm.generation, f.arm.generation);
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 1, newGroup, newId));
    const auto* current = MemberOriginAccess::Member(f.pool, newId);
    ASSERT_EQ(1u, MemberOriginAccess::Count(f.arms, f.arm));
    ASSERT_TRUE(f.pool.CancelPreparedPlan03GroupSlice(&oldGroup));
    EXPECT_EQ(1u, MemberOriginAccess::Count(f.arms, f.arm));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{current}));
    ASSERT_TRUE(f.pool.CancelPreparedPlan03GroupSlice(&newGroup));
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, f.arm));
}

TEST(DecisionGroup, NativeMemberReplacementFailurePreservesOldOriginAndSuccessfulSwapReturnsIt) {
    NativeGroupOriginFixture f;
    ASSERT_NO_FATAL_FAILURE(f.Init());
    harken::PreparedPlan03GroupSlice prepared;
    harken::Plan03GroupSliceIdentity identity{};
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 0, prepared, identity));
    harken::CommittedPlan03GroupSlice committed;
    ASSERT_TRUE(f.pool.CommitPreparedPlan03GroupSlice(&prepared, &committed));
    const auto* original = MemberOriginAccess::Member(f.pool, identity);
    identity = MemberOriginAccess::Group(original);
    harken::Plan03ExactGroupSnapshot actual{};
    ASSERT_TRUE(f.pool.CaptureExactPlan03Group(identity, &actual));
    const auto serial = MemberOriginAccess::Serial(original);
    auto replacement = f.Input();
    harken::TimelineArmToken wrongRender = f.arm;
    ++wrongRender.renderGeneration;
    replacement.members = &wrongRender;
    harken::PreparedPlan03GroupSliceReplacement refused;
    const auto status = f.pool.PreparePlan03GroupSliceReplacement(identity, replacement, f.arms, &refused);
    if (status == harken::Plan03GroupReserveStatus::Committed) {
        EXPECT_FALSE(f.pool.CommitPreparedPlan03GroupSliceReplacement(&refused));
        ASSERT_TRUE(f.pool.CancelPreparedPlan03GroupSliceReplacement(&refused));
    } else EXPECT_EQ(harken::Plan03GroupReserveStatus::ArmIdentityMismatch, status);
    EXPECT_EQ(serial, MemberOriginAccess::Serial(original));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{original}));
    harken::DecisionToken successorDecision{};
    ASSERT_TRUE(f.decisions.Reserve(MakeRequest(2), &successorDecision));
    ASSERT_NE(f.decision.slot, successorDecision.slot);
    harken::TimelineArmToken successor{};
    ASSERT_TRUE(f.arms.PreparePendingImmediateGrouped(11, 1, 7, {}, {successorDecision}, 1, &successor));
    replacement.decision = successorDecision;
    replacement.members = &successor;
    harken::PreparedPlan03GroupSliceReplacement accepted;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        f.pool.PreparePlan03GroupSliceReplacement(identity, replacement, f.arms, &accepted));
    ASSERT_TRUE(f.pool.CommitPreparedPlan03GroupSliceReplacement(&accepted));
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, f.arm));
    EXPECT_EQ(1u, MemberOriginAccess::Count(f.arms, successor));
    EXPECT_NE(serial, MemberOriginAccess::Serial(original));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, successor, std::array{original}));
    EXPECT_EQ(successorDecision.slot, MemberOriginAccess::Group(original).decision.slot);
    EXPECT_EQ(successorDecision.generation, MemberOriginAccess::Group(original).decision.generation);

    // Reusing replacement scratch for another row must preserve the first origin.
    const auto successorOrigin = MemberOriginAccess::Group(original);
    harken::PreparedPlan03GroupSlice otherPrepared;
    harken::Plan03GroupSliceIdentity otherIdentity{};
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 1, otherPrepared, otherIdentity));
    harken::CommittedPlan03GroupSlice otherCommitted;
    ASSERT_TRUE(f.pool.CommitPreparedPlan03GroupSlice(&otherPrepared, &otherCommitted));
    const auto* otherOriginal = MemberOriginAccess::Member(f.pool, otherIdentity);
    otherIdentity = MemberOriginAccess::Group(otherOriginal);
    harken::PreparedPlan03GroupSliceReplacement otherReplacement;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        f.pool.PreparePlan03GroupSliceReplacement(otherIdentity, f.Input(1), f.arms, &otherReplacement));
    ASSERT_TRUE(f.pool.CommitPreparedPlan03GroupSliceReplacement(&otherReplacement));
    EXPECT_EQ(successorOrigin, MemberOriginAccess::Group(original));
    EXPECT_EQ(f.decision.slot, MemberOriginAccess::Group(otherOriginal).decision.slot);
    EXPECT_EQ(1u, MemberOriginAccess::Group(otherOriginal).sliceIndex);
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, successor, std::array{original}));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{otherOriginal}));
    ASSERT_TRUE(f.pool.RollbackCommittedPlan03GroupSlice(&otherCommitted));
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, f.arm));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, successor, std::array{original}));
    ASSERT_TRUE(f.pool.RollbackCommittedPlan03GroupSlice(&committed));
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, successor));
}

TEST(DecisionGroup, NativeMemberFactoryAcceptsActuallyHeldCommitProofAndRefusesExhaustedSerial) {
    NativeGroupOriginFixture f;
    ASSERT_NO_FATAL_FAILURE(f.Init());
    harken::TimelineArmTable::PreparedCommitProof armProof;
    const harken::TimelineArmTable::PreparedCommitInput expected{f.arm, 0, 0, 7, {}, {f.decision}, 0, true};
    ASSERT_TRUE(f.arms.ProvePreparedCommit(expected, &armProof));
    harken::PreparedPlan03GroupSlice prepared;
    harken::Plan03GroupSliceIdentity identity{};
    ASSERT_NO_FATAL_FAILURE(f.Prepare(f.pool, 0, prepared, identity));
    EXPECT_TRUE(armProof.Valid());
    const auto* original = MemberOriginAccess::Member(f.pool, identity);
    const auto serial = MemberOriginAccess::Serial(original);
    MemberOriginAccess::ExhaustSerial(f.arms);
    harken::PreparedPlan03GroupSlice refused;
    EXPECT_EQ(harken::Plan03GroupReserveStatus::ArmIdentityMismatch,
        f.pool.PreparePlan03GroupSlice(f.Input(1), f.arms, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(serial, MemberOriginAccess::Serial(original));
    EXPECT_TRUE(MemberOriginAccess::Covers(f.arms, f.arm, std::array{original}));
    ASSERT_TRUE(f.pool.CancelPreparedPlan03GroupSlice(&prepared));
    EXPECT_EQ(0u, MemberOriginAccess::Count(f.arms, f.arm));
    armProof.Release();
}

TEST(DecisionGroup, NativeMemberQuiescentReceiverTeardownSupportsBothRealDestructionOrders) {
    for (const bool tableFirst : {false, true}) {
        SCOPED_TRACE(tableFirst);
        auto pool = std::make_unique<harken::ScheduledTransactionPool>();
        auto arms = std::make_unique<harken::TimelineArmTable>(1);
        harken::DecisionTable decisions(1, 2);
        harken::DecisionToken decision{};
        ASSERT_TRUE(decisions.Reserve(MakeRequest(), &decision));
        ASSERT_TRUE(pool->Reset(MakePoolConfig(), 0, 9));
        harken::TimelineArmToken arm{};
        ASSERT_TRUE(arms->PreparePendingImmediateGrouped(10, 1, 7, {}, {decision}, 0, &arm));
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = 200;
        input.decision = decision;
        input.members = &arm;
        input.memberCount = 1;
        ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed, pool->ReservePlan03GroupSlice(input, *arms));
        ASSERT_TRUE(MemberOriginAccess::ReceiverAttached(*pool));
        ASSERT_EQ(1u, MemberOriginAccess::Count(*arms, arm));
        if (tableFirst) {
            arms.reset();
            EXPECT_FALSE(MemberOriginAccess::ReceiverAttached(*pool));
            pool.reset();
        } else {
            pool.reset();
            EXPECT_EQ(0u, MemberOriginAccess::Count(*arms, arm));
            arms.reset();
        }
    }
}

TEST(DecisionGroup, NativeDetachedMemberEpisodeReturnsWithoutTouchingRealReconstructedReceiver) {
    harken::ScheduledTransactionPool pool;
    auto config = MakePoolConfig();
    config.maxPlan03GroupHeadersPerPage = 3;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    alignas(harken::TimelineArmTable) std::byte backing[sizeof(harken::TimelineArmTable)];
    auto destroy = [](harken::TimelineArmTable* table) { std::destroy_at(table); };
    std::unique_ptr<harken::TimelineArmTable, decltype(destroy)> arms(
        std::construct_at(reinterpret_cast<harken::TimelineArmTable*>(backing), 1), destroy);
    harken::DecisionTable decisions(1, 2);
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(), &decision));
    harken::TimelineArmToken oldArm{}, newArm{};
    ASSERT_TRUE(arms->PreparePendingImmediateGrouped(10, 1, 7, {}, {decision}, 0, &oldArm));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 200;
    input.decision = decision;
    input.members = &oldArm;
    input.memberCount = 1;
    harken::PreparedPlan03GroupSlice oldGroup, newGroup;
    harken::Plan03GroupSliceIdentity oldId{}, newId{};
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSlice(input, *arms, &oldGroup));
    oldId = MemberOriginAccess::ObservePreparedOrigin(pool, decision, 0);
    ASSERT_TRUE(oldId.Exact());
    const auto oldIncarnation = arms->Incarnation();
    arms.reset();
    ASSERT_FALSE(MemberOriginAccess::ReceiverAttached(pool));
    arms.reset(std::construct_at(reinterpret_cast<harken::TimelineArmTable*>(backing), 1));
    ASSERT_NE(oldIncarnation, arms->Incarnation());
    ASSERT_TRUE(arms->PreparePendingImmediateGrouped(11, 1, 7, {}, {decision}, 0, &newArm));
    input.members = &newArm;
    input.sliceIndex = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        pool.PreparePlan03GroupSlice(input, *arms, &newGroup));
    newId = MemberOriginAccess::ObservePreparedOrigin(pool, decision, 1);
    ASSERT_TRUE(newId.Exact());
    const auto* current = MemberOriginAccess::Member(pool, newId);
    const auto* obsolete = MemberOriginAccess::Member(pool, oldId);
    EXPECT_FALSE(MemberOriginAccess::Covers(*arms, newArm, std::array{obsolete}));
    ASSERT_TRUE(pool.CancelPreparedPlan03GroupSlice(&oldGroup));
    EXPECT_EQ(1u, MemberOriginAccess::Count(*arms, newArm));
    EXPECT_TRUE(MemberOriginAccess::Covers(*arms, newArm, std::array{current}));
    ASSERT_TRUE(pool.CancelPreparedPlan03GroupSlice(&newGroup));
    EXPECT_EQ(0u, MemberOriginAccess::Count(*arms, newArm));
}

TEST(DecisionGroup, UnpublishedStorageRetentionRefusesActualGroupAndDecisionReaders) {
    harken::ScheduledTransactionPool pool;
    auto config = MakePoolConfig();
    config.maxImmediateLaneGroupRows = 1;
    config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    ASSERT_TRUE(pool.Reset(config, 0, 9));
    harken::TimelineArmTable arms(1);
    harken::DecisionTable decisions(1, 1);
    harken::DecisionToken decision{};
    ASSERT_TRUE(decisions.Reserve(MakeRequest(1), &decision));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = 120;
    input.decision = decision;
    harken::Plan03GroupSliceIdentity group{};
    ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 100, input, arms, &group));
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, 100, false, 1}));
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin original, foreign;
    ASSERT_TRUE(lane.Reserve(17, {0, 1, 0, 1}, &handle, &original));
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(group, lane, 17, handle.slot, handle.generation));
    ASSERT_TRUE(lane.DeclareGroup(handle, {group, 20}));
    ASSERT_TRUE(lane.Prepare(handle, nullptr, 0));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(lane.TryPlace(handle, 400, 0, &anchor));
    ASSERT_EQ(400u, anchor);
    ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, anchor));
    harken::FrontierGroupBinding binding{};
    binding.role = harken::FrontierGroupRole::Coordinator;
    binding.entry.coordinator = decision;
    binding.entry.attempt = 1;
    binding.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    binding.operation.kind = harken::FrontierOperationKind::Entry;
    binding.operation.owner = binding.entry.owner;
    binding.operation.order.logicCommitSequence = 1;
    binding.operation.order.executeFrame = 420;
    binding.operation.group = group;
    binding.operation.transport = 9;
    binding.operation.world = 7;
    binding.primary = group;
    ASSERT_TRUE(pool.BindPlan03GroupFrontier(group, binding));
    ASSERT_TRUE(lane.PinGroups(handle, pool, decisions, 1));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    ASSERT_TRUE(lane.Cancel(handle));
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(original, handle, 17, 0));
    EXPECT_FALSE(lane.Retire(handle, 0, &original));
    ASSERT_TRUE(decisions.Cancel(decision));
    EXPECT_FALSE(decisions.ReclaimCancelled(decision));
    EXPECT_FALSE(pool.CancelImmediateLaneGroups(17, 0));
    EXPECT_TRUE(foreign.Valid());
    foreign.Return();
    ASSERT_TRUE(lane.Retire(handle, 0, &original));
    ASSERT_TRUE(pool.DetachPlan03GroupFrontier(group, 0));
    ASSERT_TRUE(pool.CancelImmediateLaneGroups(17, 0));
    ASSERT_TRUE(decisions.ReclaimCancelled(decision));
    EXPECT_EQ(0u, decisions.LiveCount());
    ASSERT_TRUE(pool.ResetForEpoch(0, 10));
}
