#include "AI/AIConversationStore.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QTemporaryDir>

using namespace VNX;

namespace {

QJsonArray sampleConversation()
{
    return QJsonArray{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("Question")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), QStringLiteral("Answer")}}
    };
}

}

TEST(AIConversationStoreTest, ScopesAreIsolatedAndClearIsIdempotent)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    LocalAIConversationStore store(directory.path());
    const QString firstScope = QStringLiteral("project:first");
    const QString secondScope = QStringLiteral("project:second");

    ASSERT_TRUE(store.save(firstScope, sampleConversation()).success);
    EXPECT_EQ(store.load(secondScope).status,
              IAIConversationStore::LoadResult::Status::Missing);

    EXPECT_TRUE(store.clear(firstScope).success);
    EXPECT_TRUE(store.clear(firstScope).success);
    EXPECT_EQ(store.load(firstScope).status,
              IAIConversationStore::LoadResult::Status::Missing);
}
