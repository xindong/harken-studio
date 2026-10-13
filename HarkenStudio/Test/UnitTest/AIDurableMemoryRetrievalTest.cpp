#include "AI/AIDurableMemoryRetrieval.h"

#include <gtest/gtest.h>

#include <QStringList>

using namespace VNX;

namespace {

AIDurableMemoryEntry memoryEntry(
    const QString& id,
    AIDurableMemoryKind kind,
    const QString& key,
    const QString& value,
    qint64 updatedAtMs)
{
    AIDurableMemoryEntry entry;
    entry.id = id;
    entry.kind = kind;
    entry.key = key;
    entry.value = value;
    entry.source = AIDurableMemorySource::User;
    entry.sourceReference = QStringLiteral("test");
    entry.createdAtMs = updatedAtMs;
    entry.updatedAtMs = updatedAtMs;
    entry.revision = 1;
    return entry;
}

QStringList resultIds(const AIDurableMemoryRetrievalResult& result)
{
    QStringList ids;
    for (const AIDurableMemoryRetrievalItem& item : result.items)
        ids.append(item.entry.id);
    return ids;
}

}

TEST(AIDurableMemoryRetrievalTest,
     ProjectScopeOutranksConflictingUserPreference)
{
    DeterministicAIDurableMemoryRetriever retriever;
    const QList<AIDurableMemoryCandidate> candidates{
        {
            QStringLiteral("user"),
            memoryEntry(
                QStringLiteral("11111111-1111-4111-8111-111111111111"),
                AIDurableMemoryKind::UserPreference,
                QStringLiteral("output.format"),
                QStringLiteral("WAV"),
                2000)
        },
        {
            QStringLiteral("project"),
            memoryEntry(
                QStringLiteral("22222222-2222-4222-8222-222222222222"),
                AIDurableMemoryKind::ProjectFact,
                QStringLiteral("output.format"),
                QStringLiteral("VST3 and Standalone"),
                1000)
        }
    };

    const auto result = retriever.retrieve(
        {QStringLiteral("What output format should this project use?"),
         16384, 8},
        candidates);

    ASSERT_EQ(result.items.size(), 2);
    EXPECT_EQ(result.items.at(0).scope, QStringLiteral("project"));
    EXPECT_EQ(result.items.at(1).scope, QStringLiteral("user"));
    EXPECT_LT(
        result.contextBlock.indexOf(
            QStringLiteral("22222222-2222-4222-8222-222222222222")),
        result.contextBlock.indexOf(
            QStringLiteral("11111111-1111-4111-8111-111111111111")));
}

TEST(AIDurableMemoryRetrievalTest,
     LexicalRelevanceThenRecencyProduceStableOrder)
{
    DeterministicAIDurableMemoryRetriever retriever;
    const AIDurableMemoryCandidate newestUnrelated{
        QStringLiteral("project"),
        memoryEntry(
            QStringLiteral("33333333-3333-4333-8333-333333333333"),
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("sample.rate"),
            QStringLiteral("48000"),
            3000)
    };
    const AIDurableMemoryCandidate olderRelevant{
        QStringLiteral("project"),
        memoryEntry(
            QStringLiteral("44444444-4444-4444-8444-444444444444"),
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("output.format"),
            QStringLiteral("VST3"),
            1000)
    };
    const AIDurableMemoryCandidate olderUnrelated{
        QStringLiteral("project"),
        memoryEntry(
            QStringLiteral("55555555-5555-4555-8555-555555555555"),
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("channel.layout"),
            QStringLiteral("stereo"),
            2000)
    };

    const auto forward = retriever.retrieve(
        {QStringLiteral("Confirm the output format."), 16384, 8},
        {newestUnrelated, olderRelevant, olderUnrelated});
    const auto reversed = retriever.retrieve(
        {QStringLiteral("Confirm the output format."), 16384, 8},
        {olderUnrelated, olderRelevant, newestUnrelated});

    EXPECT_EQ(
        resultIds(forward),
        (QStringList{
            QStringLiteral("44444444-4444-4444-8444-444444444444"),
            QStringLiteral("33333333-3333-4333-8333-333333333333"),
            QStringLiteral("55555555-5555-4555-8555-555555555555")
        }));
    EXPECT_EQ(resultIds(reversed), resultIds(forward));
    EXPECT_EQ(reversed.contextBlock, forward.contextBlock);
}

TEST(AIDurableMemoryRetrievalTest,
     TightBudgetOmitsLowerPriorityWholeEntries)
{
    DeterministicAIDurableMemoryRetriever retriever;
    const AIDurableMemoryCandidate relevant{
        QStringLiteral("project"),
        memoryEntry(
            QStringLiteral("66666666-6666-4666-8666-666666666666"),
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("output.format"),
            QStringLiteral("VST3"),
            1000)
    };
    const AIDurableMemoryCandidate unrelated{
        QStringLiteral("project"),
        memoryEntry(
            QStringLiteral("77777777-7777-4777-8777-777777777777"),
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("sample.rate"),
            QStringLiteral("48000"),
            2000)
    };
    const auto one = retriever.retrieve(
        {QStringLiteral("output format"), 16384, 8},
        {relevant});
    ASSERT_EQ(one.items.size(), 1);
    const int oneEntryBytes = one.contextBlock.toUtf8().size();

    const auto constrained = retriever.retrieve(
        {QStringLiteral("output format"), oneEntryBytes, 8},
        {unrelated, relevant});

    ASSERT_EQ(constrained.items.size(), 1);
    EXPECT_EQ(
        constrained.items.constFirst().entry.id,
        QStringLiteral("66666666-6666-4666-8666-666666666666"));
    EXPECT_EQ(constrained.omittedCount, 1);
    EXPECT_LE(
        constrained.contextBlock.toUtf8().size(),
        oneEntryBytes);
    EXPECT_FALSE(constrained.contextBlock.contains(
        QStringLiteral("77777777-7777-4777-8777-777777777777")));
}

TEST(AIDurableMemoryRetrievalTest,
     ContextBlockPreservesTraceableSourceMetadata)
{
    DeterministicAIDurableMemoryRetriever retriever;
    const auto result = retriever.retrieve(
        {QStringLiteral("language"), 16384, 8},
        {{
            QStringLiteral("user"),
            memoryEntry(
                QStringLiteral("88888888-8888-4888-8888-888888888888"),
                AIDurableMemoryKind::UserPreference,
                QStringLiteral("response.language"),
                QStringLiteral("Chinese"),
                1000)
        }});

    ASSERT_EQ(result.items.size(), 1);
    EXPECT_TRUE(result.contextBlock.startsWith(
        QStringLiteral("<DURABLE_MEMORY_CONTEXT>")));
    EXPECT_TRUE(result.contextBlock.contains(
        QStringLiteral("\"id\":\"88888888-8888-4888-8888-888888888888\"")));
    EXPECT_TRUE(result.contextBlock.contains(
        QStringLiteral("\"scope\":\"user\"")));
    EXPECT_TRUE(result.contextBlock.contains(
        QStringLiteral("\"source_reference\":\"test\"")));
    EXPECT_TRUE(result.contextBlock.endsWith(
        QStringLiteral("</DURABLE_MEMORY_CONTEXT>")));
}
