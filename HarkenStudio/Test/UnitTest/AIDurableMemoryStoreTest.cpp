#include "AI/AIDurableMemoryStore.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QUuid>

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

TEST_F(AIDurableMemoryStoreTest, RoundTripPreservesTypedEntryAndHidesRawScope)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const QString scope =
        QStringLiteral("project:C:/Users/Test/PrivateProject");
    const AIDurableMemoryEntry entry = makeEntry(
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        AIDurableMemoryKind::ProjectFact,
        QStringLiteral(" Mixer.SampleRate "),
        QStringLiteral("48000"));

    ASSERT_TRUE(store.save(scope, {entry}).success);
    const AIDurableMemoryLoadResult loaded = store.load(scope);
    ASSERT_EQ(loaded.status, AIDurableMemoryLoadResult::Status::Loaded);
    ASSERT_EQ(loaded.entries.size(), 1);
    EXPECT_EQ(loaded.entries[0].id, entry.id);
    EXPECT_EQ(loaded.entries[0].kind, AIDurableMemoryKind::ProjectFact);
    EXPECT_EQ(loaded.entries[0].key, QStringLiteral("mixer.samplerate"));
    EXPECT_EQ(loaded.entries[0].value, QStringLiteral("48000"));
    EXPECT_EQ(loaded.entries[0].sourceReference,
              QStringLiteral("settings-panel"));
    EXPECT_EQ(loaded.entries[0].revision, 1);

    QFile file(store.storagePathForScope(scope));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    EXPECT_FALSE(bytes.contains(scope.toUtf8()));
    EXPECT_FALSE(bytes.contains("PrivateProject"));
}

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
    EXPECT_NE(store.storagePathForScope(userScope),
              store.storagePathForScope(projectScope));
}

TEST_F(AIDurableMemoryStoreTest, RejectsScopeKindMismatchAndDuplicateIdentity)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const AIDurableMemoryEntry preference = makeEntry(
        QStringLiteral("44444444-4444-4444-8444-444444444444"),
        AIDurableMemoryKind::UserPreference,
        QStringLiteral("response.language"),
        QStringLiteral("Chinese"));
    EXPECT_FALSE(
        store.save(QStringLiteral("project:C:/Project"), {preference}).success);

    AIDurableMemoryEntry duplicate = preference;
    duplicate.id = QStringLiteral("55555555-5555-4555-8555-555555555555");
    duplicate.key = QStringLiteral(" RESPONSE.LANGUAGE ");
    EXPECT_FALSE(
        store.save(QStringLiteral("user:default"),
                   {preference, duplicate}).success);
}

TEST_F(AIDurableMemoryStoreTest, InvalidReplacementPreservesLastValidDocument)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const QString scope = QStringLiteral("user:default");
    const AIDurableMemoryEntry valid = makeEntry(
        QStringLiteral("66666666-6666-4666-8666-666666666666"),
        AIDurableMemoryKind::UserPreference,
        QStringLiteral("response.language"),
        QStringLiteral("Chinese"));
    ASSERT_TRUE(store.save(scope, {valid}).success);

    AIDurableMemoryEntry invalid = valid;
    invalid.value =
        QString(AIDurableMemoryLimits::kMaximumValueBytes + 1, QLatin1Char('x'));
    EXPECT_FALSE(store.save(scope, {invalid}).success);

    const AIDurableMemoryLoadResult loaded = store.load(scope);
    ASSERT_EQ(loaded.status, AIDurableMemoryLoadResult::Status::Loaded);
    ASSERT_EQ(loaded.entries.size(), 1);
    EXPECT_EQ(loaded.entries[0].value, QStringLiteral("Chinese"));
}

TEST_F(AIDurableMemoryStoreTest, RejectsUnsupportedSchemaAndMalformedMetadata)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const QString scope = QStringLiteral("user:default");
    const QString path = store.storagePathForScope(scope);

    const QJsonObject unsupported{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("scope_id"), QStringLiteral("wrong")},
        {QStringLiteral("entries"), QJsonArray{}}
    };
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QJsonDocument(unsupported).toJson(
                  QJsonDocument::Compact)),
              0);
    file.close();
    EXPECT_EQ(store.load(scope).status,
              AIDurableMemoryLoadResult::Status::Invalid);

    const AIDurableMemoryEntry valid = makeEntry(
        QStringLiteral("77777777-7777-4777-8777-777777777777"),
        AIDurableMemoryKind::UserPreference,
        QStringLiteral("response.language"),
        QStringLiteral("Chinese"));
    ASSERT_TRUE(store.save(scope, {valid}).success);

    QJsonDocument document;
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    document = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = document.object();
    QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    QJsonObject entry = entries[0].toObject();
    entry[QStringLiteral("revision")] = 1.5;
    entries[0] = entry;
    root[QStringLiteral("entries")] = entries;
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)),
              0);
    file.close();
    EXPECT_EQ(store.load(scope).status,
              AIDurableMemoryLoadResult::Status::Invalid);
}

TEST_F(AIDurableMemoryStoreTest, ClearIsIdempotent)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const QString scope = QStringLiteral("project:C:/Project");
    const AIDurableMemoryEntry fact = makeEntry(
        QStringLiteral("88888888-8888-4888-8888-888888888888"),
        AIDurableMemoryKind::ProjectFact,
        QStringLiteral("mix.rate"),
        QStringLiteral("48000"));
    ASSERT_TRUE(store.save(scope, {fact}).success);
    ASSERT_TRUE(store.clear(scope).success);
    EXPECT_EQ(store.load(scope).status,
              AIDurableMemoryLoadResult::Status::Missing);
    EXPECT_TRUE(store.clear(scope).success);
}

TEST_F(AIDurableMemoryStoreTest, MissingInvalidScopeIsRejected)
{
    LocalAIDurableMemoryStore store(directory_.path());
    EXPECT_EQ(store.load(QStringLiteral("unknown:scope")).status,
              AIDurableMemoryLoadResult::Status::Invalid);
    EXPECT_FALSE(store.clear(QStringLiteral("unknown:scope")).success);
    EXPECT_TRUE(
        store.storagePathForScope(QStringLiteral("unknown:scope")).isEmpty());
}

TEST_F(AIDurableMemoryStoreTest, OversizedDocumentIsRejected)
{
    LocalAIDurableMemoryStore store(directory_.path());
    const QString scope = QStringLiteral("user:default");
    QFile file(store.storagePathForScope(scope));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray oversized(
        AIDurableMemoryLimits::kMaximumDocumentBytes + 1, 'x');
    ASSERT_EQ(file.write(oversized), oversized.size());
    file.close();

    EXPECT_EQ(store.load(scope).status,
              AIDurableMemoryLoadResult::Status::Invalid);
}

TEST_F(AIDurableMemoryStoreTest, EntryCountLimitIsEnforced)
{
    LocalAIDurableMemoryStore store(directory_.path());
    QList<AIDurableMemoryEntry> entries;
    for (int index = 0;
         index <= AIDurableMemoryLimits::kMaximumEntries;
         ++index)
    {
        entries.append(makeEntry(
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            AIDurableMemoryKind::UserPreference,
            QStringLiteral("preference.%1").arg(index),
            QStringLiteral("value")));
    }

    EXPECT_FALSE(
        store.save(QStringLiteral("user:default"), entries).success);
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
