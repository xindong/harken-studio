#include "AI/AIDurableMemoryStore.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QSemaphore>
#include <QTemporaryDir>

#include <atomic>
#include <future>

using namespace VNX;

namespace {

AIDurableMemoryEntry makeEntry(
    const QString& id,
    AIDurableMemoryKind kind,
    const QString& key,
    const QString& value,
    qint64 revision = 1)
{
    AIDurableMemoryEntry entry;
    entry.id = id;
    entry.kind = kind;
    entry.key = key;
    entry.value = value;
    entry.source = AIDurableMemorySource::User;
    entry.sourceReference = QStringLiteral("settings-panel");
    entry.createdAtMs = 1000;
    entry.updatedAtMs = 1000 + revision - 1;
    entry.revision = revision;
    return entry;
}

AIDurableMemoryOperationResult successfulOperation()
{
    AIDurableMemoryOperationResult result;
    result.success = true;
    return result;
}

}

class AIDurableMemoryStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(directory_.isValid());
    }

    QTemporaryDir directory_;
};

TEST_F(AIDurableMemoryStoreTest, UserAndProjectScopesRemainIndependent)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const QString userScope = QStringLiteral("user:default");
    const QString projectScope = QStringLiteral("project:C:/Project");
    const AIDurableMemoryEntry preference = makeEntry(
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        AIDurableMemoryKind::UserPreference,
        QStringLiteral("response.language"),
        QStringLiteral("Chinese"));
    const AIDurableMemoryEntry fact = makeEntry(
        QStringLiteral("33333333-3333-4333-8333-333333333333"),
        AIDurableMemoryKind::ProjectFact,
        QStringLiteral("mix.rate"),
        QStringLiteral("48000"));

    ASSERT_TRUE(store.save(userScope, {preference}).success);
    ASSERT_TRUE(store.save(projectScope, {fact}).success);
    ASSERT_EQ(store.load(userScope).entries.size(), 1);
    ASSERT_EQ(store.load(projectScope).entries.size(), 1);
    EXPECT_EQ(store.load(userScope).entries[0].value,
              QStringLiteral("Chinese"));
    EXPECT_EQ(store.load(projectScope).entries[0].value,
              QStringLiteral("48000"));
}

TEST_F(AIDurableMemoryStoreTest,
       SerializesScopedMutationsWithoutLosingUpdates)
{
    LocalAIDurableMemoryStore firstStore(directory_.path());
    LocalAIDurableMemoryStore secondStore(directory_.path());
    const QString scope = QStringLiteral("user:default");
    const AIDurableMemoryEntry firstEntry = makeEntry(
        QStringLiteral("99999999-9999-4999-8999-999999999999"),
        AIDurableMemoryKind::UserPreference,
        QStringLiteral("response.language"),
        QStringLiteral("Chinese"));
    const AIDurableMemoryEntry secondEntry = makeEntry(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        AIDurableMemoryKind::UserPreference,
        QStringLiteral("response.detail"),
        QStringLiteral("expanded"));

    QSemaphore firstEntered;
    QSemaphore releaseFirst;
    QSemaphore secondEntered;
    std::atomic_bool firstSawMissing = false;
    std::atomic_bool secondSawFirst = false;

    auto first = std::async(std::launch::async, [&]() {
        return firstStore.mutate(
            scope,
            [&](const AIDurableMemoryLoadResult& current,
                QList<AIDurableMemoryEntry>* replacement,
                bool* shouldPersist) {
                firstSawMissing =
                    current.status
                    == AIDurableMemoryLoadResult::Status::Missing;
                firstEntered.release();
                releaseFirst.acquire();
                *replacement = {firstEntry};
                *shouldPersist = true;
                return successfulOperation();
            });
    });

    if (!firstEntered.tryAcquire(1, 1000))
    {
        releaseFirst.release();
        first.wait();
        FAIL() << "First mutation did not enter the transaction.";
        return;
    }

    auto second = std::async(std::launch::async, [&]() {
        return secondStore.mutate(
            scope,
            [&](const AIDurableMemoryLoadResult& current,
                QList<AIDurableMemoryEntry>* replacement,
                bool* shouldPersist) {
                secondSawFirst =
                    current.status
                        == AIDurableMemoryLoadResult::Status::Loaded
                    && current.entries.size() == 1
                    && current.entries[0].id == firstEntry.id;
                secondEntered.release();
                *replacement = current.entries;
                replacement->append(secondEntry);
                *shouldPersist = true;
                return successfulOperation();
            });
    });

    EXPECT_FALSE(secondEntered.tryAcquire(1, 100));
    releaseFirst.release();
    const AIDurableMemoryOperationResult firstResult = first.get();
    EXPECT_TRUE(firstResult.success);
    EXPECT_TRUE(firstSawMissing.load());

    if (!secondEntered.tryAcquire(1, 2000))
    {
        second.wait();
        FAIL() << "Second mutation did not acquire the scope lock.";
        return;
    }
    const AIDurableMemoryOperationResult secondResult = second.get();
    EXPECT_TRUE(secondResult.success);
    EXPECT_TRUE(secondSawFirst.load());

    const AIDurableMemoryLoadResult loaded = firstStore.load(scope);
    ASSERT_EQ(loaded.status, AIDurableMemoryLoadResult::Status::Loaded);
    ASSERT_EQ(loaded.entries.size(), 2);
    EXPECT_EQ(loaded.entries[0].id, firstEntry.id);
    EXPECT_EQ(loaded.entries[1].id, secondEntry.id);
}
