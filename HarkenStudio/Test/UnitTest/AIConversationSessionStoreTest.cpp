#include "AI/AIConversationSessionStore.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QSemaphore>
#include <QTemporaryDir>

#include <atomic>
#include <future>

using namespace VNX;

namespace {

AIConversationSessionRecord makeSession(
    const QString& id,
    const QString& title,
    qint64 timestamp)
{
    AIConversationSessionRecord session;
    session.id = id;
    session.title = title;
    session.createdAtMs = timestamp;
    session.updatedAtMs = timestamp;
    return session;
}

AIConversationSessionOperationResult successfulOperation()
{
    AIConversationSessionOperationResult result;
    result.success = true;
    return result;
}

}

class AIConversationSessionStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(directory_.isValid());
    }

    QTemporaryDir directory_;
};

TEST_F(AIConversationSessionStoreTest,
       RejectsDuplicateAndMissingActiveSession)
{
    LocalAIConversationSessionStore store(directory_.path());
    const AIConversationSessionRecord first = makeSession(
        QStringLiteral(
            "33333333-3333-4333-8333-333333333333"),
        QStringLiteral("First"),
        1000);
    AIConversationSessionCatalog catalog;
    catalog.sessions = {first, first};
    catalog.activeSessionId = first.id;
    EXPECT_FALSE(
        store.save(QStringLiteral("project:test"), catalog)
            .success);

    catalog.sessions = {first};
    catalog.activeSessionId = QStringLiteral(
        "44444444-4444-4444-8444-444444444444");
    EXPECT_FALSE(
        store.save(QStringLiteral("project:test"), catalog)
            .success);
}

TEST_F(AIConversationSessionStoreTest,
       SerializesCatalogMutationsWithoutLosingSessions)
{
    LocalAIConversationSessionStore firstStore(
        directory_.path());
    LocalAIConversationSessionStore secondStore(
        directory_.path());
    const QString scope = QStringLiteral("project:test");
    const AIConversationSessionRecord firstSession = makeSession(
        QStringLiteral(
            "77777777-7777-4777-8777-777777777777"),
        QStringLiteral("First"),
        1000);
    const AIConversationSessionRecord secondSession = makeSession(
        QStringLiteral(
            "88888888-8888-4888-8888-888888888888"),
        QStringLiteral("Second"),
        2000);

    QSemaphore firstEntered;
    QSemaphore releaseFirst;
    QSemaphore secondEntered;
    std::atomic_bool firstSawMissing = false;
    std::atomic_bool secondSawFirst = false;

    auto first = std::async(std::launch::async, [&]() {
        return firstStore.mutate(
            scope,
            [&](const AIConversationSessionLoadResult& current,
                AIConversationSessionCatalog* replacement,
                bool* shouldPersist) {
                firstSawMissing =
                    current.status
                    == AIConversationSessionLoadResult::Status::Missing;
                firstEntered.release();
                releaseFirst.acquire();
                replacement->activeSessionId = firstSession.id;
                replacement->sessions = {firstSession};
                *shouldPersist = true;
                return successfulOperation();
            });
    });

    if (!firstEntered.tryAcquire(1, 1000))
    {
        releaseFirst.release();
        first.wait();
        FAIL() << "First catalog mutation did not enter.";
        return;
    }

    auto second = std::async(std::launch::async, [&]() {
        return secondStore.mutate(
            scope,
            [&](const AIConversationSessionLoadResult& current,
                AIConversationSessionCatalog* replacement,
                bool* shouldPersist) {
                secondSawFirst =
                    current.status
                        == AIConversationSessionLoadResult::Status::Loaded
                    && current.catalog.sessions.size() == 1
                    && current.catalog.sessions[0].id
                        == firstSession.id;
                secondEntered.release();
                *replacement = current.catalog;
                replacement->sessions.prepend(secondSession);
                replacement->activeSessionId = secondSession.id;
                *shouldPersist = true;
                return successfulOperation();
            });
    });

    EXPECT_FALSE(secondEntered.tryAcquire(1, 100));
    releaseFirst.release();
    const AIConversationSessionOperationResult firstResult =
        first.get();
    EXPECT_TRUE(firstResult.success);
    EXPECT_TRUE(firstSawMissing.load());

    if (!secondEntered.tryAcquire(1, 2000))
    {
        second.wait();
        FAIL() << "Second catalog mutation did not acquire the lock.";
        return;
    }
    const AIConversationSessionOperationResult secondResult =
        second.get();
    EXPECT_TRUE(secondResult.success);
    EXPECT_TRUE(secondSawFirst.load());

    const AIConversationSessionLoadResult loaded =
        firstStore.load(scope);
    ASSERT_EQ(
        loaded.status,
        AIConversationSessionLoadResult::Status::Loaded);
    ASSERT_EQ(loaded.catalog.sessions.size(), 2);
    EXPECT_EQ(
        loaded.catalog.sessions[0].id,
        secondSession.id);
    EXPECT_EQ(
        loaded.catalog.sessions[1].id,
        firstSession.id);
    EXPECT_EQ(
        loaded.catalog.activeSessionId,
        secondSession.id);
}
