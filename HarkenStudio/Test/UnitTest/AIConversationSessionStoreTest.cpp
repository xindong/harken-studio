#include "AI/AIConversationSessionStore.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
       RoundTripPreservesCatalogAndHidesRawScope)
{
    LocalAIConversationSessionStore store(directory_.path());
    const QString scope =
        QStringLiteral("project:C:/Users/Test/PrivateProject");
    AIConversationSessionCatalog catalog;
    catalog.sessions = {
        makeSession(
            QStringLiteral(
                "11111111-1111-4111-8111-111111111111"),
            QStringLiteral("First conversation"),
            1000),
        makeSession(
            QStringLiteral(
                "22222222-2222-4222-8222-222222222222"),
            QStringLiteral("Second conversation"),
            2000)
    };
    catalog.activeSessionId = catalog.sessions[1].id;

    ASSERT_TRUE(store.save(scope, catalog).success);
    const AIConversationSessionLoadResult loaded =
        store.load(scope);
    ASSERT_EQ(
        loaded.status,
        AIConversationSessionLoadResult::Status::Loaded);
    ASSERT_EQ(loaded.catalog.sessions.size(), 2);
    EXPECT_EQ(
        loaded.catalog.activeSessionId,
        catalog.activeSessionId);
    EXPECT_EQ(
        loaded.catalog.sessions[0].title,
        QStringLiteral("First conversation"));

    QFile file(store.storagePathForScope(scope));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    EXPECT_FALSE(bytes.contains(scope.toUtf8()));
    EXPECT_FALSE(bytes.contains("PrivateProject"));
}

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
       InvalidReplacementPreservesLastValidCatalog)
{
    LocalAIConversationSessionStore store(directory_.path());
    const QString scope = QStringLiteral("project:test");
    const AIConversationSessionRecord first = makeSession(
        QStringLiteral(
            "55555555-5555-4555-8555-555555555555"),
        QStringLiteral("First"),
        1000);
    AIConversationSessionCatalog catalog;
    catalog.sessions = {first};
    catalog.activeSessionId = first.id;
    ASSERT_TRUE(store.save(scope, catalog).success);

    catalog.sessions[0].title = QString(
        AIConversationSessionLimits::kMaximumTitleBytes + 1,
        QLatin1Char('x'));
    EXPECT_FALSE(store.save(scope, catalog).success);

    const auto loaded = store.load(scope);
    ASSERT_EQ(
        loaded.status,
        AIConversationSessionLoadResult::Status::Loaded);
    ASSERT_EQ(loaded.catalog.sessions.size(), 1);
    EXPECT_EQ(
        loaded.catalog.sessions[0].title,
        QStringLiteral("First"));
}

TEST_F(AIConversationSessionStoreTest,
       CorruptAndOversizedCatalogsAreRejected)
{
    LocalAIConversationSessionStore store(directory_.path());
    const QString scope = QStringLiteral("project:test");
    QFile file(store.storagePathForScope(scope));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QByteArrayLiteral("{invalid")), 0);
    file.close();
    EXPECT_EQ(
        store.load(scope).status,
        AIConversationSessionLoadResult::Status::Invalid);

    const QJsonObject fractionalSchema{
        {QStringLiteral("schema_version"), 1.5},
        {QStringLiteral("scope_id"), QStringLiteral("invalid")},
        {QStringLiteral("active_session_id"),
         QStringLiteral(
             "66666666-6666-4666-8666-666666666666")},
        {QStringLiteral("sessions"), QJsonArray{}}
    };
    ASSERT_TRUE(
        file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(
        file.write(
            QJsonDocument(fractionalSchema)
                .toJson(QJsonDocument::Compact)),
        0);
    file.close();
    EXPECT_EQ(
        store.load(scope).status,
        AIConversationSessionLoadResult::Status::Invalid);

    ASSERT_TRUE(
        file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray oversized(
        AIConversationSessionLimits::kMaximumDocumentBytes + 1,
        'x');
    ASSERT_EQ(file.write(oversized), oversized.size());
    file.close();
    EXPECT_EQ(
        store.load(scope).status,
        AIConversationSessionLoadResult::Status::Invalid);
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
