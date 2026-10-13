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

TEST(RuntimeSyncCoordinatorTest,
     CurrentWorldMutationsInOneEventTurnSubmitOnlyTheGreatestRevision)
{
    auto application = ensureApplication();
    std::vector<VNX::RuntimeSyncCoordinator::AuthoringRevision> submittedRevisions;
    VNX::RuntimeSyncCoordinator coordinator(
        [&](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            submittedRevisions.push_back(token.authoringRevision);
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        });
    ASSERT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated,
              coordinator.beginProjectSession());
    ASSERT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated,
              coordinator.runtimeStarted());

    const auto first = coordinator.markCurrentWorldMutation();
    const auto second = coordinator.markCurrentWorldMutation();
    const auto third = coordinator.markCurrentWorldMutation();

    EXPECT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated, first.state);
    EXPECT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated, second.state);
    EXPECT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated, third.state);
    EXPECT_NE(0u, first.revision);
    EXPECT_LT(first.revision, second.revision);
    EXPECT_LT(second.revision, third.revision);
    EXPECT_EQ(third.revision, coordinator.currentAuthoringRevision());
    EXPECT_TRUE(submittedRevisions.empty());

    QCoreApplication::processEvents(QEventLoop::AllEvents);

    ASSERT_EQ(1u, submittedRevisions.size());
    EXPECT_EQ(third.revision, submittedRevisions.front());
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
     PendingDoesNotSatisfyBarrierAndMatchingTerminalSuccessPublishesReady)
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
    ASSERT_EQ(Coordinator::AllocationState::Allocated, mutation.state);
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    ASSERT_EQ(1u, submittedTokens.size());
    const auto pending = coordinator.barrier(mutation.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending, pending.state);

    HarkenEditor::Bridge::CurrentWorldSyncResult installed;
    installed.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    installed.token = submittedTokens.front();
    installed.worldGeneration = 81;
    coordinator.acceptCurrentWorldSyncResult(installed);

    const auto ready = coordinator.barrier(mutation.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Ready, ready.state);
    EXPECT_EQ(submittedTokens.front().projectSessionId, ready.projectSessionId);
    EXPECT_EQ(mutation.revision, ready.installedRevision);
    EXPECT_EQ(submittedTokens.front().runtimeEpoch, ready.runtimeEpoch);
    EXPECT_EQ(81u, ready.worldGeneration);
    EXPECT_EQ(submittedTokens.front().submissionSerial, ready.submissionSerial);
    ASSERT_TRUE(coordinator.hasInstalledWorld());
    const auto installedIdentity = coordinator.installedWorld();
    ASSERT_TRUE(installedIdentity.has_value());
    EXPECT_EQ(81u, installedIdentity->worldGeneration);
    EXPECT_EQ(mutation.revision, installedIdentity->authoringRevision);

    const auto replacement = coordinator.markCurrentWorldMutation();
    ASSERT_EQ(Coordinator::AllocationState::Allocated, replacement.state);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending,
              coordinator.barrier(replacement.revision).state);
    EXPECT_TRUE(coordinator.hasInstalledWorld());
    ASSERT_TRUE(coordinator.installedWorld().has_value());
    EXPECT_EQ(81u, coordinator.installedWorld()->worldGeneration);
}

TEST(RuntimeSyncCoordinatorTest,
     StaleSessionEpochOrSerialTerminalResultCannotPublishReady)
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
    const auto current = submittedTokens.front();

    const auto expectStaleIgnored = [&](auto token) {
        HarkenEditor::Bridge::CurrentWorldSyncResult installed;
        installed.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
        installed.token = token;
        installed.worldGeneration = 91;
        coordinator.acceptCurrentWorldSyncResult(installed);
        EXPECT_EQ(Coordinator::RevisionBarrierState::Pending,
                  coordinator.barrier(mutation.revision).state);
    };

    auto oldSession = current;
    --oldSession.projectSessionId;
    expectStaleIgnored(oldSession);
    auto oldEpoch = current;
    --oldEpoch.runtimeEpoch;
    expectStaleIgnored(oldEpoch);
    auto oldSerial = current;
    --oldSerial.submissionSerial;
    expectStaleIgnored(oldSerial);

    HarkenEditor::Bridge::CurrentWorldSyncResult installed;
    installed.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    installed.token = current;
    installed.worldGeneration = 92;
    coordinator.acceptCurrentWorldSyncResult(installed);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Ready,
              coordinator.barrier(mutation.revision).state);
}

TEST(RuntimeSyncCoordinatorTest,
     LatestInstalledRevisionSatisfiesAnEarlierRequiredRevision)
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

    const auto first = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    HarkenEditor::Bridge::CurrentWorldSyncResult installed;
    installed.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    installed.token = submittedTokens.back();
    installed.worldGeneration = 101;
    coordinator.acceptCurrentWorldSyncResult(installed);
    ASSERT_EQ(Coordinator::RevisionBarrierState::Ready,
              coordinator.barrier(first.revision).state);

    const auto second = coordinator.markCurrentWorldMutation();
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending,
              coordinator.barrier(first.revision).state);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    installed.token = submittedTokens.back();
    installed.worldGeneration = 102;
    coordinator.acceptCurrentWorldSyncResult(installed);

    const auto ready = coordinator.barrier(first.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Ready, ready.state);
    EXPECT_EQ(second.revision, ready.installedRevision);
    EXPECT_EQ(second.revision, coordinator.currentAuthoringRevision());
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
    EXPECT_EQ(Coordinator::RevisionBarrierState::RuntimeUnavailable,
              coordinator.barrier(mutation.revision).state);
    ASSERT_EQ(Coordinator::AllocationState::Allocated,
              coordinator.runtimeStarted());

    HarkenEditor::Bridge::CurrentWorldSyncResult oldCompletion;
    oldCompletion.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    oldCompletion.token = oldToken;
    oldCompletion.worldGeneration = 121;
    coordinator.acceptCurrentWorldSyncResult(oldCompletion);

    const auto barrier = coordinator.barrier(mutation.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending, barrier.state);
    EXPECT_GT(barrier.runtimeEpoch, oldToken.runtimeEpoch);
    EXPECT_EQ(0u, barrier.worldGeneration);
}

TEST(RuntimeSyncCoordinatorTest,
     SubmissionBatchDefersFlushAndKeepsLatestRevision)
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

    coordinator.beginSubmissionBatch();
    const auto first = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_TRUE(submittedTokens.empty());
    const auto latest = coordinator.markCurrentWorldMutation();
    coordinator.endSubmissionBatch();
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    ASSERT_EQ(submittedTokens.size(), 1u);
    EXPECT_EQ(latest.revision, submittedTokens.front().authoringRevision);
    EXPECT_GT(latest.revision, first.revision);
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

TEST(RuntimeSyncCoordinatorTest, MutationWithoutRuntimeRemainsUnavailable)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    int submissions = 0;
    Coordinator coordinator(
        [&submissions](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            ++submissions;
            HarkenEditor::Bridge::CurrentWorldSyncResult result;
            result.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Pending;
            result.token = token;
            return result;
        });
    ASSERT_EQ(Coordinator::AllocationState::Allocated,
              coordinator.beginProjectSession());
    const auto mutation = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_EQ(0, submissions);
    EXPECT_EQ(Coordinator::RevisionBarrierState::RuntimeUnavailable,
              coordinator.barrier(mutation.revision).state);
}

TEST(RuntimeSyncCoordinatorTest, CurrentTokenSupersessionIsExplicit)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    HarkenEditor::Bridge::CurrentWorldSyncToken submittedToken;
    Coordinator coordinator(
        [&submittedToken](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
            submittedToken = token;
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

    HarkenEditor::Bridge::CurrentWorldSyncResult superseded;
    superseded.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Superseded;
    superseded.token = submittedToken;
    coordinator.acceptCurrentWorldSyncResult(superseded);

    const auto barrier = coordinator.barrier(mutation.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Superseded, barrier.state);
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::Superseded,
              barrier.failure);
}

TEST(RuntimeSyncCoordinatorTest, CurrentFailureIsTerminalAndNextRevisionCanRecover)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    std::vector<HarkenEditor::Bridge::CurrentWorldSyncToken> submittedTokens;
    Coordinator coordinator(
        [&submittedTokens](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
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

    const auto failedRevision = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(submittedTokens.size(), 1u);
    HarkenEditor::Bridge::CurrentWorldSyncResult failed;
    failed.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::InstallFailed;
    failed.token = submittedTokens.back();
    coordinator.acceptCurrentWorldSyncResult(failed);

    const auto failure = coordinator.barrier(failedRevision.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Failed, failure.state);
    EXPECT_EQ(HarkenEditor::Bridge::CurrentWorldSyncStatus::InstallFailed,
              failure.failure);

    const auto recoveryRevision = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(submittedTokens.size(), 2u);
    HarkenEditor::Bridge::CurrentWorldSyncResult recovered;
    recovered.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    recovered.token = submittedTokens.back();
    recovered.worldGeneration = 404;
    coordinator.acceptCurrentWorldSyncResult(recovered);

    const auto ready = coordinator.barrier(recoveryRevision.revision);
    EXPECT_EQ(Coordinator::RevisionBarrierState::Ready, ready.state);
    EXPECT_EQ(404u, ready.worldGeneration);
}

TEST(RuntimeSyncCoordinatorTest,
     OutstandingSubmissionBlocksSecondFlushUntilOpenThenCoalescesLatest)
{
    auto application = ensureApplication();
    using Coordinator = VNX::RuntimeSyncCoordinator;
    std::vector<HarkenEditor::Bridge::CurrentWorldSyncToken> submittedTokens;
    Coordinator coordinator(
        [&submittedTokens](HarkenEditor::Bridge::CurrentWorldSyncToken token) {
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

    const auto first = coordinator.markCurrentWorldMutation();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(1u, submittedTokens.size());
    EXPECT_TRUE(coordinator.hasOutstandingSubmission());
    EXPECT_EQ(first.revision, submittedTokens.front().authoringRevision);

    const auto second = coordinator.markCurrentWorldMutation();
    EXPECT_GT(second.revision, first.revision);
    coordinator.flushPendingMutationNow();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(1u, submittedTokens.size());
    EXPECT_TRUE(coordinator.hasOutstandingSubmission());
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending,
              coordinator.barrier(second.revision).state);

    HarkenEditor::Bridge::CurrentWorldSyncResult installed;
    installed.status = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    installed.token = submittedTokens.front();
    installed.worldGeneration = 501;
    coordinator.acceptCurrentWorldSyncResult(installed);
    EXPECT_FALSE(coordinator.hasOutstandingSubmission());

    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_EQ(2u, submittedTokens.size());
    EXPECT_EQ(second.revision, submittedTokens.back().authoringRevision);
    EXPECT_TRUE(coordinator.hasOutstandingSubmission());
    EXPECT_EQ(Coordinator::RevisionBarrierState::Pending,
              coordinator.barrier(second.revision).state);
}
