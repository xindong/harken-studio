// Logic commit coordinator and bundle contract tests.
//
// The claim being pinned is atomicity: either every participating service holds
// room and publishes under one order stamp, or none of them was touched. So the
// cases that matter are the failing ones — a refusal in the middle of a
// reservation has to leave the services that already agreed exactly as they were,
// and it has to give back exactly what it took.

#include "harken/core/LogicCommitCoordinator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using harken::ActionCommitBundle;
using harken::BundleState;
using harken::CommitParticipant;
using harken::LogicCommitCoordinator;
using harken::LogicCommitSequence;

namespace {

// A service that records what was asked of it and can be told to refuse.
struct Service {
    bool refuse = false;
    std::uint32_t held = 0;
    std::vector<std::uint32_t> reserveCalls;
    std::vector<std::uint32_t> releaseCalls;
    std::vector<std::uint32_t> commitCalls;
    std::vector<LogicCommitSequence> commitSequences;
    // Order in which services were released, shared across a whole test.
    std::vector<int>* releaseOrder = nullptr;
    int id = 0;
};

bool Reserve(void* ctx, std::uint32_t count)
{
    Service* service = static_cast<Service*>(ctx);
    service->reserveCalls.push_back(count);
    if (service->refuse) {
        return false;
    }
    service->held += count;
    return true;
}

void Release(void* ctx, std::uint32_t count)
{
    Service* service = static_cast<Service*>(ctx);
    service->releaseCalls.push_back(count);
    service->held -= count;
    if (service->releaseOrder != nullptr) {
        service->releaseOrder->push_back(service->id);
    }
}

void Commit(void* ctx, std::uint32_t count, LogicCommitSequence sequence)
{
    Service* service = static_cast<Service*>(ctx);
    service->commitCalls.push_back(count);
    service->commitSequences.push_back(sequence);
}

CommitParticipant ParticipantFor(Service& service)
{
    CommitParticipant participant;
    participant.ctx = &service;
    participant.reserve = &Reserve;
    participant.release = &Release;
    participant.commit = &Commit;
    return participant;
}

} // namespace

// Per-service counters cannot be compared, so there is one sequence. Monotonic,
// never 0, never reissued: an unstamped record must not sort ahead of a real one.
TEST(LogicCommitCoordinator, IssuesAMonotonicSequenceThatNeverRepeatsOrHitsZero)
{
    LogicCommitCoordinator coordinator;
    EXPECT_EQ(coordinator.Current(), 0u);

    const LogicCommitSequence first = coordinator.Issue();
    const LogicCommitSequence second = coordinator.Issue();
    EXPECT_NE(first, 0u);
    EXPECT_GT(second, first);
    EXPECT_EQ(coordinator.Current(), second);
}

TEST(LogicCommitCoordinator, PreparedIssueKeepsCurrentUntilItsOriginalConsume)
{
    LogicCommitCoordinator coordinator;
    ASSERT_EQ(1u, coordinator.Issue());
    LogicCommitCoordinator::PreparedIssue proof;
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&proof));
    ASSERT_TRUE(proof.Valid());
    EXPECT_EQ(2u, proof.Sequence());
    EXPECT_EQ(1u, coordinator.Current());
    LogicCommitSequence untouched = 99;
    EXPECT_EQ(LogicCommitCoordinator::IssueStatus::Busy, coordinator.TryIssue(&untouched));
    EXPECT_EQ(99u, untouched);
    EXPECT_EQ(0u, coordinator.Issue());
    EXPECT_FALSE(coordinator.Exhausted());
    coordinator.ConsumePreparedIssue(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(0u, proof.Sequence());
    EXPECT_EQ(2u, coordinator.Current());
    EXPECT_EQ(3u, coordinator.Issue());
}

TEST(LogicCommitCoordinator, PreparedIssueAbortReturnsTheSameUnissuedSequence)
{
    LogicCommitCoordinator coordinator;
    {
        LogicCommitCoordinator::PreparedIssue abandoned;
        ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&abandoned));
        EXPECT_EQ(1u, abandoned.Sequence());
    }
    EXPECT_EQ(0u, coordinator.Current());
    LogicCommitCoordinator::PreparedIssue next;
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&next));
    EXPECT_EQ(1u, next.Sequence());
    next.Release();
    next.Release();
    EXPECT_EQ(0u, coordinator.Current());
    EXPECT_EQ(1u, coordinator.Issue());
}

TEST(LogicCommitCoordinator, PreparedIssueMoveAndOverwriteReturnOnlyTheirOriginalIssuer)
{
    LogicCommitCoordinator first;
    LogicCommitCoordinator second;
    LogicCommitCoordinator::PreparedIssue firstProof;
    LogicCommitCoordinator::PreparedIssue secondProof;
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, first.PrepareIssue(&firstProof));
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, second.PrepareIssue(&secondProof));
    EXPECT_EQ(LogicCommitCoordinator::IssueStatus::Busy, second.PrepareIssue(&firstProof));
    firstProof = std::move(secondProof);
    EXPECT_FALSE(secondProof.Valid());
    EXPECT_EQ(0u, first.Current());
    EXPECT_EQ(1u, first.Issue());
    LogicCommitSequence untouched = 99;
    EXPECT_EQ(LogicCommitCoordinator::IssueStatus::Busy, second.TryIssue(&untouched));
    LogicCommitCoordinator::PreparedIssue moved(std::move(firstProof));
    EXPECT_FALSE(firstProof.Valid());
    EXPECT_TRUE(moved.Valid());
    second.ConsumePreparedIssue(std::move(moved));
    EXPECT_EQ(1u, second.Current());
    EXPECT_EQ(2u, second.Issue());
}

TEST(LogicCommitCoordinator, PreparedIssueRejectsAnotherReservationWithoutExhausting)
{
    LogicCommitCoordinator coordinator;
    LogicCommitCoordinator::PreparedIssue held;
    LogicCommitCoordinator::PreparedIssue refused;
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&held));
    EXPECT_EQ(LogicCommitCoordinator::IssueStatus::Busy, coordinator.PrepareIssue(&held));
    EXPECT_EQ(LogicCommitCoordinator::IssueStatus::Busy, coordinator.PrepareIssue(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(held.Valid());
    EXPECT_FALSE(coordinator.Exhausted());
    held.Release();
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&refused));
    coordinator.ConsumePreparedIssue(std::move(refused));
    EXPECT_EQ(1u, coordinator.Current());
}

TEST(LogicCommitCoordinator, PreparedLastSequenceDoesNotWrapOrExhaustOnAbort)
{
    LogicCommitCoordinator coordinator;
    coordinator.SetCurrentForTests(UINT64_MAX - 1);
    LogicCommitCoordinator::PreparedIssue last;
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&last));
    EXPECT_EQ(UINT64_MAX, last.Sequence());
    last.Release();
    EXPECT_EQ(UINT64_MAX - 1, coordinator.Current());
    EXPECT_FALSE(coordinator.Exhausted());
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&last));
    coordinator.ConsumePreparedIssue(std::move(last));
    EXPECT_EQ(UINT64_MAX, coordinator.Current());
    LogicCommitCoordinator::PreparedIssue refused;
    EXPECT_EQ(LogicCommitCoordinator::IssueStatus::RuntimeRecreateRequired,
              coordinator.PrepareIssue(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(coordinator.Exhausted());
    EXPECT_EQ(0u, coordinator.Issue());
    EXPECT_EQ(UINT64_MAX, coordinator.Current());
}

TEST(ActionCommitBundle, PreparedSequenceBusyReleasesAllParticipantsWithoutPublishing)
{
    LogicCommitCoordinator coordinator;
    LogicCommitCoordinator::PreparedIssue sequence;
    ASSERT_EQ(LogicCommitCoordinator::IssueStatus::Issued, coordinator.PrepareIssue(&sequence));
    std::vector<int> releaseOrder;
    Service first;
    Service second;
    first.id = 1;
    second.id = 2;
    first.releaseOrder = &releaseOrder;
    second.releaseOrder = &releaseOrder;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(first), 2));
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(second), 3));
    ASSERT_TRUE(bundle.Reserve());
    EXPECT_FALSE(bundle.Commit(coordinator));
    EXPECT_EQ(BundleState::Abandoned, bundle.State());
    EXPECT_EQ(0u, coordinator.Current());
    EXPECT_FALSE(coordinator.Exhausted());
    EXPECT_TRUE(sequence.Valid());
    EXPECT_EQ(0u, first.held);
    EXPECT_EQ(0u, second.held);
    EXPECT_TRUE(first.commitCalls.empty());
    EXPECT_TRUE(second.commitCalls.empty());
    ASSERT_EQ(2u, releaseOrder.size());
    EXPECT_EQ(2, releaseOrder[0]);
    EXPECT_EQ(1, releaseOrder[1]);
    coordinator.ConsumePreparedIssue(std::move(sequence));
    EXPECT_EQ(1u, coordinator.Current());
}

TEST(ActionCommitBundle, CommitsEveryParticipantUnderOneSequence)
{
    LogicCommitCoordinator coordinator;
    Service variables;
    Service instances;
    ActionCommitBundle bundle(4);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(variables), 2));
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(instances), 3));

    ASSERT_TRUE(bundle.Reserve());
    EXPECT_EQ(bundle.State(), BundleState::Reserved);
    ASSERT_TRUE(bundle.Commit(coordinator));
    EXPECT_EQ(bundle.State(), BundleState::Committed);

    ASSERT_EQ(variables.commitSequences.size(), 1u);
    ASSERT_EQ(instances.commitSequences.size(), 1u);
    // One stamp across the bundle is what stops anything downstream from
    // interleaving the two changes.
    EXPECT_EQ(variables.commitSequences[0], instances.commitSequences[0]);
    EXPECT_EQ(variables.commitSequences[0], bundle.Sequence());
    EXPECT_EQ(variables.commitCalls[0], 2u);
    EXPECT_EQ(instances.commitCalls[0], 3u);
}

TEST(ActionCommitBundle, CommitReservedPublishesTheCallerSequence) {
    LogicCommitCoordinator coordinator;
    ASSERT_NE(coordinator.Issue(), 0u);
    const LogicCommitSequence reserved = coordinator.Current();
    Service variables;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(variables), 1));
    ASSERT_TRUE(bundle.Reserve());
    ASSERT_TRUE(bundle.CommitReserved(reserved));
    EXPECT_EQ(bundle.State(), BundleState::Committed);
    ASSERT_EQ(variables.commitSequences.size(), 1u);
    EXPECT_EQ(variables.commitSequences[0], reserved);
    EXPECT_EQ(coordinator.Current(), reserved);
}

TEST(ActionCommitBundle,
     SequenceExhaustionReleasesEveryReservationWithoutPublishing)
{
    std::vector<int> releaseOrder;
    Service first;
    Service second;
    first.id = 1;
    second.id = 2;
    first.releaseOrder = &releaseOrder;
    second.releaseOrder = &releaseOrder;
    LogicCommitCoordinator coordinator;
    coordinator.SetCurrentForTests(
        std::numeric_limits<LogicCommitSequence>::max());
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(first), 2));
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(second), 3));
    ASSERT_TRUE(bundle.Reserve());

    EXPECT_FALSE(bundle.Commit(coordinator));
    EXPECT_EQ(bundle.State(), BundleState::Abandoned);
    EXPECT_EQ(bundle.Sequence(), 0u);
    EXPECT_TRUE(coordinator.Exhausted());
    EXPECT_EQ(first.held, 0u);
    EXPECT_EQ(second.held, 0u);
    EXPECT_TRUE(first.commitCalls.empty());
    EXPECT_TRUE(second.commitCalls.empty());
    ASSERT_EQ(releaseOrder.size(), 2u);
    EXPECT_EQ(releaseOrder[0], 2);
    EXPECT_EQ(releaseOrder[1], 1);

    EXPECT_FALSE(bundle.Commit(coordinator));
    bundle.Abandon();
    EXPECT_EQ(first.releaseCalls.size(), 1u);
    EXPECT_EQ(second.releaseCalls.size(), 1u);
}

// The case the class exists for. The third service refuses, and the two that
// already agreed must end up holding nothing — a planner that saw one of them
// changed and the other not would decide against a state that never legally
// existed.
TEST(ActionCommitBundle, ARefusalMidReservationLeavesEveryServiceUntouched)
{
    std::vector<int> releaseOrder;
    Service first;
    Service second;
    Service third;
    first.id = 1;
    second.id = 2;
    third.id = 3;
    first.releaseOrder = &releaseOrder;
    second.releaseOrder = &releaseOrder;
    third.releaseOrder = &releaseOrder;
    third.refuse = true;

    ActionCommitBundle bundle(4);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(first), 2));
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(second), 5));
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(third), 1));

    EXPECT_FALSE(bundle.Reserve());
    EXPECT_EQ(bundle.State(), BundleState::Abandoned);

    // Exactly what each took, given back.
    EXPECT_EQ(first.held, 0u);
    EXPECT_EQ(second.held, 0u);
    ASSERT_EQ(first.releaseCalls.size(), 1u);
    ASSERT_EQ(second.releaseCalls.size(), 1u);
    EXPECT_EQ(first.releaseCalls[0], 2u);
    EXPECT_EQ(second.releaseCalls[0], 5u);
    // Reverse order: the service that reserved last gives its room back first.
    ASSERT_EQ(releaseOrder.size(), 2u);
    EXPECT_EQ(releaseOrder[0], 2);
    EXPECT_EQ(releaseOrder[1], 1);
    // The refusing service was never asked to release something it never held.
    EXPECT_TRUE(third.releaseCalls.empty());
    // And nothing published.
    EXPECT_TRUE(first.commitCalls.empty());
    EXPECT_TRUE(second.commitCalls.empty());
}

// Reserving and committing are separate so that the failure lives entirely in
// the first step. Committing without holding room is the half-state this class
// rules out, so it is refused rather than attempted.
TEST(ActionCommitBundle, RefusesToCommitWithoutAHeldReservation)
{
    LogicCommitCoordinator coordinator;
    Service service;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(service), 1));

    EXPECT_FALSE(bundle.Commit(coordinator));
    EXPECT_EQ(bundle.State(), BundleState::Open);
    EXPECT_TRUE(service.commitCalls.empty());
    // The coordinator must not have been advanced by a refused commit, or the
    // sequence would have a gap that no commit explains.
    EXPECT_EQ(coordinator.Current(), 0u);
}

// A bundle must not grow after its room was proven sufficient, or the proof
// stops covering it.
TEST(ActionCommitBundle, RefusesNewStepsOnceReserved)
{
    Service first;
    Service second;
    ActionCommitBundle bundle(4);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(first), 1));
    ASSERT_TRUE(bundle.Reserve());

    EXPECT_FALSE(bundle.AddStep(ParticipantFor(second), 1));
    EXPECT_EQ(bundle.StepCount(), 1u);
    EXPECT_TRUE(second.reserveCalls.empty());
}

TEST(ActionCommitBundle, RefusesMoreStepsThanItWasSizedFor)
{
    Service a;
    Service b;
    ActionCommitBundle bundle(1);
    EXPECT_TRUE(bundle.AddStep(ParticipantFor(a), 1));
    EXPECT_FALSE(bundle.AddStep(ParticipantFor(b), 1));
    EXPECT_EQ(bundle.StepCount(), 1u);
}

// Abandoning gives back exactly one reservation. Calling it twice must not
// double-release, because the second release would take room from whoever
// acquired it in between.
TEST(ActionCommitBundle, AbandonReleasesOnceAndIsSafeToRepeat)
{
    Service service;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(service), 4));
    ASSERT_TRUE(bundle.Reserve());
    EXPECT_EQ(service.held, 4u);

    bundle.Abandon();
    EXPECT_EQ(bundle.State(), BundleState::Abandoned);
    EXPECT_EQ(service.held, 0u);
    EXPECT_EQ(service.releaseCalls.size(), 1u);

    bundle.Abandon();
    EXPECT_EQ(service.releaseCalls.size(), 1u);
}

// A participant that cannot promise room is not a participant that needs none.
// Treating a missing callback as success would let a partly wired service commit
// into room nobody held.
TEST(ActionCommitBundle, TreatsAParticipantWithNoReserveCallbackAsARefusal)
{
    Service service;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(service), 1));
    CommitParticipant silent;
    ASSERT_TRUE(bundle.AddStep(silent, 1));

    EXPECT_FALSE(bundle.Reserve());
    EXPECT_EQ(service.held, 0u);
    EXPECT_EQ(service.releaseCalls.size(), 1u);
}

TEST(ActionCommitBundle, TreatsAParticipantWithNoCommitCallbackAsARefusal)
{
    Service first;
    Service incomplete;
    CommitParticipant missingCommit = ParticipantFor(incomplete);
    missingCommit.commit = nullptr;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(first), 2));
    ASSERT_TRUE(bundle.AddStep(missingCommit, 3));

    EXPECT_FALSE(bundle.Reserve());
    EXPECT_EQ(bundle.State(), BundleState::Abandoned);
    EXPECT_EQ(first.held, 0u);
    EXPECT_EQ(first.releaseCalls.size(), 1u);
    EXPECT_TRUE(incomplete.reserveCalls.empty());
    EXPECT_TRUE(first.commitCalls.empty());
    EXPECT_TRUE(incomplete.commitCalls.empty());
}

TEST(ActionCommitBundle, TreatsAParticipantWithNoReleaseCallbackAsARefusal)
{
    Service first;
    Service incomplete;
    CommitParticipant missingRelease = ParticipantFor(incomplete);
    missingRelease.release = nullptr;
    ActionCommitBundle bundle(2);
    ASSERT_TRUE(bundle.AddStep(ParticipantFor(first), 2));
    ASSERT_TRUE(bundle.AddStep(missingRelease, 3));

    EXPECT_FALSE(bundle.Reserve());
    EXPECT_EQ(bundle.State(), BundleState::Abandoned);
    EXPECT_EQ(first.held, 0u);
    EXPECT_EQ(first.releaseCalls.size(), 1u);
    EXPECT_TRUE(incomplete.reserveCalls.empty());
    EXPECT_TRUE(first.commitCalls.empty());
    EXPECT_TRUE(incomplete.commitCalls.empty());
}
