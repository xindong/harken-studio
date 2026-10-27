#include "RuntimeSyncCoordinator.h"
#include <harken_playable_properties.g.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>

#include <limits>
#include <memory>
#include <vector>

namespace {

std::unique_ptr<QCoreApplication> ensureApplication()
{
    if (QCoreApplication::instance())
        return {};

    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    return std::make_unique<QCoreApplication>(argc, argv);
}

}

TEST(RuntimeSyncCoordinatorTest, AuthoringRevisionExhaustionDoesNotWrapToZero)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    const auto maximum = std::numeric_limits<Coordinator::AuthoringRevision>::max();
    std::vector<Coordinator::AuthoringRevision> submittedRevisions;
    Coordinator coordinator(
        [&](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            submittedRevisions.push_back(token.authoringRevision);
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        },
        maximum);

    const auto allocation = coordinator.markCurrentWorldMutation();

    EXPECT_EQ(Coordinator::AllocationState::IdentityExhausted, allocation.state);
    EXPECT_EQ(maximum, allocation.revision);
    EXPECT_EQ(maximum, coordinator.currentAuthoringRevision());

    QCoreApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_TRUE(submittedRevisions.empty());
}

TEST(RuntimeSyncCoordinatorTest,
     RuntimeRestartRejectsCompletionFromTheInvalidatedEpoch)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    std::vector<HarkenEditor::Bridge::CurrentWorldSyncToken> submittedTokens;
    Coordinator coordinator(
        [&](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            submittedTokens.push_back(token);
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        });
    ASSERT_EQ(Coordinator::AllocationState::Allocated,
              coordinator.beginProjectSession());
    ASSERT_EQ(Coordinator::AllocationState::Allocated,
              coordinator.runtimeStarted());
    const auto mutation = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(1u, submittedTokens.size());
    const auto oldToken = submittedTokens.front();

    coordinator.runtimeStopping();
    ASSERT_EQ(Coordinator::AllocationState::Allocated,
              coordinator.runtimeStarted());

    HarkenEditor::Bridge::CurrentWorldSyncResult oldCompletion;
    oldCompletion.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    oldCompletion.token = oldToken;
    oldCompletion.worldGeneration = 121;
    coordinator.acceptCurrentWorldSyncResult(oldCompletion);

    const auto barrier = coordinator.barrier(mutation.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending, barrier.state);
    EXPECT_NE(barrier.runtimeEpoch, oldToken.runtimeEpoch);
}

TEST(RuntimeSyncCoordinatorTest, RemainingIdentityCountersNeverWrapToZero)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();

    Coordinator projectSessionCoordinator(
        [](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        }, Coordinator::InitialIdentityForTesting{maximum, 0, 0, 0});
    EXPECT_EQ(Coordinator::AllocationState::IdentityExhausted,
              projectSessionCoordinator.beginProjectSession());
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::IdentityExhausted,
              projectSessionCoordinator.barrier(0).failure);

    Coordinator runtimeEpochCoordinator(
        [](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        }, Coordinator::InitialIdentityForTesting{0, 0, maximum, 0});
    EXPECT_EQ(Coordinator::AllocationState::IdentityExhausted,
              runtimeEpochCoordinator.runtimeStarted());

    int submissions = 0;
    Coordinator submissionCoordinator(
        [&submissions](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            ++submissions;
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        }, Coordinator::InitialIdentityForTesting{1, 0, 1, maximum});
    ASSERT_EQ(Coordinator::AllocationState::Allocated,
              submissionCoordinator.markCurrentWorldMutation().state);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(0, submissions);
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::IdentityExhausted,
              submissionCoordinator.barrier(1).failure);
}

TEST(RuntimeSyncCoordinatorTest, NewerEditsKeepPlaybackBarrierPendingUntilLatestWorldIsReady)
{
    auto application = ensureApplication();
    namespace Bridge = HarkenEditor::Bridge;
    using Coordinator = VNX::RuntimeSyncCoordinator;
    std::vector<Bridge::CurrentWorldSyncToken> tokens;
    Coordinator coordinator([&](Bridge::CurrentWorldSyncToken token) {
        tokens.push_back(token);
        Bridge::CurrentWorldSyncResult result;
        result.token = token;
        result.status = Bridge::CurrentWorldSyncStatus::Pending;
        return result;
    });
    coordinator.beginProjectSession();
    coordinator.runtimeStarted();
    coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_TRUE(coordinator.isCurrentSubmission(tokens.front()));
    coordinator.markCurrentWorldPropertyMutation({0, 1}, 1);
    EXPECT_FALSE(coordinator.isCurrentSubmission(tokens.front()));
    std::vector<Coordinator::RevisionBarrierState> observed;
    QObject::connect(&coordinator, &Coordinator::barrierChanged, [&] {
        observed.push_back(coordinator.barrier(coordinator.currentAuthoringRevision()).state);
    });
    Bridge::CurrentWorldSyncResult stale;
    stale.token = tokens.front();
    stale.status = Bridge::CurrentWorldSyncStatus::Superseded;
    coordinator.acceptCurrentWorldSyncResult(stale);
    EXPECT_EQ(coordinator.barrier(coordinator.currentAuthoringRevision()).state,
              Coordinator::RevisionBarrierState::Pending);
    EXPECT_TRUE(observed.empty());
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_TRUE(coordinator.isCurrentSubmission(tokens.back()));
    stale.status = Bridge::CurrentWorldSyncStatus::Ok;
    stale.worldGeneration = 51;
    coordinator.acceptCurrentWorldSyncResult(stale);
    EXPECT_FALSE(coordinator.hasInstalledWorld());
    Bridge::CurrentWorldSyncResult latest;
    latest.token = tokens.back();
    latest.status = Bridge::CurrentWorldSyncStatus::Ok;
    latest.worldGeneration = 52;
    coordinator.acceptCurrentWorldSyncResult(latest);
    const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
    EXPECT_EQ(barrier.state, Coordinator::RevisionBarrierState::Ready);
    EXPECT_EQ(barrier.worldGeneration, 52u);
    EXPECT_EQ(barrier.installedRevision, coordinator.currentAuthoringRevision());
}

TEST(RuntimeSyncCoordinatorTest, CoalescedMutationsRetainAllPropertiesAndFullInvalidation)
{
    auto application = ensureApplication();
    namespace Bridge = HarkenEditor::Bridge;
    using Coordinator = VNX::RuntimeSyncCoordinator;
    std::vector<Coordinator::MutationSet> mutations;
    Coordinator* current = nullptr;
    Coordinator coordinator([&](Bridge::CurrentWorldSyncToken token) {
        mutations.push_back(current->submittedMutations());
        Bridge::CurrentWorldSyncResult result;
        result.token = token;
        result.status = Bridge::CurrentWorldSyncStatus::Ok;
        result.worldGeneration = token.submissionSerial;
        return result;
    });
    current = &coordinator;
    coordinator.beginProjectSession();
    coordinator.runtimeStarted();
    coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_TRUE(mutations.back().full);

    coordinator.beginSubmissionBatch();
    coordinator.markCurrentWorldPropertyMutation({0, 1}, 1);
    coordinator.markCurrentWorldPropertyMutation({0, 1}, 1);
    coordinator.markCurrentWorldPropertyMutation({0, 1}, 2);
    coordinator.markCurrentWorldPropertyMutation({0, 2}, 1);
    coordinator.endSubmissionBatch();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(mutations.size(), 2u);
    EXPECT_FALSE(mutations.back().full);
    EXPECT_EQ(mutations.back().properties.size(), 3u);

    coordinator.beginSubmissionBatch();
    coordinator.markCurrentWorldPropertyMutation({0, 1}, 1);
    coordinator.markCurrentWorldMutation();
    coordinator.markCurrentWorldPropertyMutation({0, 2}, 2);
    coordinator.endSubmissionBatch();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(mutations.size(), 3u);
    EXPECT_TRUE(mutations.back().full);
    EXPECT_TRUE(mutations.back().properties.empty());

    coordinator.markCurrentWorldPropertyMutation({0, 1}, 255);
    coordinator.flushPendingMutationNow();
    EXPECT_TRUE(mutations.back().full);
}

TEST(RuntimeSyncCoordinatorTest, RepeatedFlushDoesNotCreateWorkOrLoseEditsDuringSubmission)
{
    auto application = ensureApplication();
    namespace Bridge = HarkenEditor::Bridge;
    using Coordinator = VNX::RuntimeSyncCoordinator;
    std::vector<Bridge::CurrentWorldSyncToken> tokens;
    Coordinator coordinator([&](Bridge::CurrentWorldSyncToken token) {
        tokens.push_back(token);
        Bridge::CurrentWorldSyncResult result;
        result.token = token;
        result.status = Bridge::CurrentWorldSyncStatus::Pending;
        return result;
    });
    coordinator.beginProjectSession();
    coordinator.runtimeStarted();
    const auto first = coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(tokens.size(), 1u);
    for (int attempt = 0; attempt < 3; ++attempt)
        coordinator.flushPendingMutationNow();
    EXPECT_EQ(coordinator.currentAuthoringRevision(), first.revision);

    Bridge::CurrentWorldSyncResult completion;
    completion.token = tokens.front();
    completion.status = Bridge::CurrentWorldSyncStatus::Ok;
    completion.worldGeneration = 1;
    coordinator.acceptCurrentWorldSyncResult(completion);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(coordinator.barrier(first.revision).state,
              Coordinator::RevisionBarrierState::Ready);
    EXPECT_FALSE(coordinator.hasOutstandingSubmission());
    ASSERT_EQ(tokens.size(), 1u);

    coordinator.markCurrentWorldMutation();
    coordinator.flushPendingMutationNow();
    ASSERT_EQ(tokens.size(), 2u);
    const auto latest = coordinator.markCurrentWorldMutation();
    for (int attempt = 0; attempt < 3; ++attempt)
        coordinator.flushPendingMutationNow();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(tokens.size(), 2u);
    completion.token = tokens.back();
    completion.worldGeneration = 2;
    coordinator.acceptCurrentWorldSyncResult(completion);
    EXPECT_EQ(coordinator.barrier(latest.revision).state,
              Coordinator::RevisionBarrierState::Pending);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens.back().authoringRevision, latest.revision);
    EXPECT_TRUE(coordinator.hasOutstandingSubmission());
    completion.token = tokens.back();
    completion.worldGeneration = 3;
    coordinator.acceptCurrentWorldSyncResult(completion);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    coordinator.flushPendingMutationNow();
    EXPECT_EQ(tokens.size(), 3u);
    EXPECT_FALSE(coordinator.hasOutstandingSubmission());
    const auto barrier = coordinator.barrier(latest.revision);
    EXPECT_EQ(barrier.state, Coordinator::RevisionBarrierState::Ready);
    EXPECT_EQ(barrier.installedRevision, latest.revision);
    EXPECT_EQ(barrier.worldGeneration, 3u);
}
