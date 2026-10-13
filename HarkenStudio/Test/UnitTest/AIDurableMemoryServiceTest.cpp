#include "AI/AIDurableMemoryService.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <memory>

using namespace VNX;

class AIDurableMemoryServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(directory_.isValid());
        store_ =
            std::make_shared<LocalAIDurableMemoryStore>(directory_.path());
    }

    AIDurableMemoryService makeService(
        qint64* now,
        const QString& generatedId =
            QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"))
    {
        return AIDurableMemoryService(
            store_,
            [now]() { return *now; },
            [generatedId]() { return generatedId; });
    }

    QTemporaryDir directory_;
    std::shared_ptr<LocalAIDurableMemoryStore> store_;
};

TEST_F(AIDurableMemoryServiceTest, ExactRepeatIsIdempotentAndKeepsStableId)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    AIDurableMemoryService::WriteRequest request;
    request.kind = AIDurableMemoryKind::UserPreference;
    request.key = QStringLiteral(" Response.Language ");
    request.value = QStringLiteral(" Chinese ");
    request.source = AIDurableMemorySource::User;
    request.sourceReference = QStringLiteral(" settings-panel ");

    const auto created = service.upsert(QStringLiteral("user:default"), request);
    ASSERT_TRUE(created.success);
    EXPECT_TRUE(created.changed);
    EXPECT_EQ(created.entry.id,
              QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    EXPECT_EQ(created.entry.key, QStringLiteral("response.language"));
    EXPECT_EQ(created.entry.value, QStringLiteral("Chinese"));
    EXPECT_EQ(created.entry.createdAtMs, 1000);
    EXPECT_EQ(created.entry.revision, 1);

    now = 2000;
    request.source = AIDurableMemorySource::Model;
    request.sourceReference = QStringLiteral("request:test");
    const auto repeated =
        service.upsert(QStringLiteral("user:default"), request);
    ASSERT_TRUE(repeated.success);
    EXPECT_FALSE(repeated.changed);
    EXPECT_EQ(repeated.entry.id, created.entry.id);
    EXPECT_EQ(repeated.entry.updatedAtMs, 1000);
    EXPECT_EQ(repeated.entry.revision, 1);
    EXPECT_EQ(repeated.entry.source, AIDurableMemorySource::User);
    EXPECT_EQ(repeated.entry.sourceReference,
              QStringLiteral("settings-panel"));
}

TEST_F(AIDurableMemoryServiceTest,
       ChangedValuePreservesIdentityAndAdvancesRevision)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    AIDurableMemoryService::WriteRequest request;
    request.kind = AIDurableMemoryKind::ProjectFact;
    request.key = QStringLiteral("mix.rate");
    request.value = QStringLiteral("44100");
    request.source = AIDurableMemorySource::User;
    request.sourceReference = QStringLiteral("project-settings");

    const auto created =
        service.upsert(QStringLiteral("project:C:/Project"), request);
    ASSERT_TRUE(created.success);

    now = 2000;
    request.value = QStringLiteral("48000");
    request.source = AIDurableMemorySource::Model;
    request.sourceReference = QStringLiteral("request:42");
    const auto updated =
        service.upsert(QStringLiteral("project:C:/Project"), request);
    ASSERT_TRUE(updated.success);
    EXPECT_TRUE(updated.changed);
    EXPECT_EQ(updated.entry.id, created.entry.id);
    EXPECT_EQ(updated.entry.createdAtMs, 1000);
    EXPECT_EQ(updated.entry.updatedAtMs, 2000);
    EXPECT_EQ(updated.entry.revision, 2);
    EXPECT_EQ(updated.entry.source, AIDurableMemorySource::Model);
    EXPECT_EQ(updated.entry.sourceReference, QStringLiteral("request:42"));
}

TEST_F(AIDurableMemoryServiceTest, EnforcesTypedScopeBoundaries)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    AIDurableMemoryService::WriteRequest preference;
    preference.kind = AIDurableMemoryKind::UserPreference;
    preference.key = QStringLiteral("response.language");
    preference.value = QStringLiteral("Chinese");
    preference.sourceReference = QStringLiteral("settings-panel");

    const auto wrongPreference =
        service.upsert(QStringLiteral("project:C:/Project"), preference);
    EXPECT_FALSE(wrongPreference.success);
    EXPECT_EQ(wrongPreference.code,
              QStringLiteral("AI_DURABLE_MEMORY_INVALID"));

    AIDurableMemoryService::WriteRequest fact = preference;
    fact.kind = AIDurableMemoryKind::ProjectFact;
    const auto wrongFact =
        service.upsert(QStringLiteral("user:default"), fact);
    EXPECT_FALSE(wrongFact.success);
    EXPECT_EQ(wrongFact.code,
              QStringLiteral("AI_DURABLE_MEMORY_INVALID"));
    EXPECT_EQ(service.list(QStringLiteral("user:default")).status,
              AIDurableMemoryLoadResult::Status::Missing);
}

TEST_F(AIDurableMemoryServiceTest, RemoveAndClearAreDeterministic)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    AIDurableMemoryService::WriteRequest request;
    request.kind = AIDurableMemoryKind::ProjectFact;
    request.key = QStringLiteral("mix.rate");
    request.value = QStringLiteral("48000");
    request.sourceReference = QStringLiteral("project-settings");
    const QString scope = QStringLiteral("project:C:/Project");

    const auto created = service.upsert(scope, request);
    ASSERT_TRUE(created.success);
    ASSERT_TRUE(service.remove(scope, created.entry.id).success);
    EXPECT_EQ(service.list(scope).status,
              AIDurableMemoryLoadResult::Status::Missing);

    const auto missing = service.remove(scope, created.entry.id);
    EXPECT_FALSE(missing.success);
    EXPECT_EQ(missing.code,
              QStringLiteral("AI_DURABLE_MEMORY_NOT_FOUND"));
    EXPECT_TRUE(service.clear(scope).success);
}

TEST_F(AIDurableMemoryServiceTest, ReadOnlyUseNeverCreatesStorage)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    const QString scope = QStringLiteral("user:default");

    EXPECT_EQ(service.list(scope).status,
              AIDurableMemoryLoadResult::Status::Missing);
    EXPECT_FALSE(QFile::exists(store_->storagePathForScope(scope)));
}

TEST_F(AIDurableMemoryServiceTest, RejectsInvalidProvenanceAndClockRollback)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    AIDurableMemoryService::WriteRequest request;
    request.kind = AIDurableMemoryKind::UserPreference;
    request.key = QStringLiteral("response.language");
    request.value = QStringLiteral("Chinese");

    EXPECT_FALSE(service.upsert(QStringLiteral("user:default"), request).success);

    request.sourceReference = QStringLiteral("settings-panel");
    const auto created =
        service.upsert(QStringLiteral("user:default"), request);
    ASSERT_TRUE(created.success);
    now = 999;
    request.value = QStringLiteral("English");
    const auto rollback =
        service.upsert(QStringLiteral("user:default"), request);
    EXPECT_FALSE(rollback.success);
    EXPECT_EQ(rollback.code,
              QStringLiteral("AI_DURABLE_MEMORY_CONFLICT"));
    EXPECT_EQ(service.list(QStringLiteral("user:default")).entries[0].value,
              QStringLiteral("Chinese"));
}

TEST_F(AIDurableMemoryServiceTest, GeneratedIdCollisionDoesNotReplaceMemory)
{
    qint64 now = 1000;
    AIDurableMemoryService service = makeService(&now);
    AIDurableMemoryService::WriteRequest first;
    first.kind = AIDurableMemoryKind::UserPreference;
    first.key = QStringLiteral("response.language");
    first.value = QStringLiteral("Chinese");
    first.sourceReference = QStringLiteral("settings-panel");
    ASSERT_TRUE(service.upsert(QStringLiteral("user:default"), first).success);

    ++now;
    AIDurableMemoryService::WriteRequest second = first;
    second.key = QStringLiteral("response.detail");
    second.value = QStringLiteral("concise");
    const auto collision =
        service.upsert(QStringLiteral("user:default"), second);
    EXPECT_FALSE(collision.success);
    EXPECT_EQ(collision.code,
              QStringLiteral("AI_DURABLE_MEMORY_CONFLICT"));

    const AIDurableMemoryLoadResult loaded =
        service.list(QStringLiteral("user:default"));
    ASSERT_EQ(loaded.entries.size(), 1);
    EXPECT_EQ(loaded.entries[0].key, QStringLiteral("response.language"));
}
