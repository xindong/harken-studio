#include "RuntimeSyncCoordinator.h"

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
