#include "AI/AIConversationStore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
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

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

}

TEST(AIConversationStoreTest, RoundTripUsesHashedScopeAndSanitizesMessages)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    LocalAIConversationStore store(directory.path());
    const QString scope = QStringLiteral("project:C:/Secret/Project.vproj");
    QJsonArray messages = sampleConversation();
    QJsonObject first = messages[0].toObject();
    first.insert(QStringLiteral("ignored"), QStringLiteral("value"));
    messages[0] = first;

    const auto saved = store.save(scope, messages);
    ASSERT_TRUE(saved.success);
    const QString path = store.storagePathForScope(scope);
    EXPECT_TRUE(QFile::exists(path));
    EXPECT_FALSE(path.contains(QStringLiteral("Secret")));

    const auto loaded = store.load(scope);
    ASSERT_EQ(loaded.status,
              IAIConversationStore::LoadResult::Status::Loaded);
    EXPECT_EQ(loaded.messages, sampleConversation());
}

TEST(AIConversationStoreTest, InvalidSaveDoesNotReplaceLastValidSnapshot)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    LocalAIConversationStore store(directory.path());
    const QString scope = QStringLiteral("user:default");
    ASSERT_TRUE(store.save(scope, sampleConversation()).success);

    QJsonArray incomplete = sampleConversation();
    incomplete.removeLast();
    const auto rejected = store.save(scope, incomplete);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.code, QStringLiteral("AI_MEMORY_INVALID"));

    const auto loaded = store.load(scope);
    ASSERT_EQ(loaded.status,
              IAIConversationStore::LoadResult::Status::Loaded);
    EXPECT_EQ(loaded.messages, sampleConversation());
}

TEST(AIConversationStoreTest, RejectsUnsupportedMalformedAndOversizedDocuments)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    LocalAIConversationStore store(directory.path());
    const QString scope = QStringLiteral("user:default");
    const QString path = store.storagePathForScope(scope);

    QDir().mkpath(directory.path());
    writeBytes(path, QByteArrayLiteral("{not-json"));
    EXPECT_EQ(store.load(scope).status,
              IAIConversationStore::LoadResult::Status::Invalid);

    const QJsonObject unsupported{
        {QStringLiteral("schema_version"), 99},
        {QStringLiteral("scope_id"), QStringLiteral("unused")},
        {QStringLiteral("messages"), QJsonArray{}}
    };
    writeBytes(path, QJsonDocument(unsupported).toJson(QJsonDocument::Compact));
    EXPECT_EQ(store.load(scope).status,
              IAIConversationStore::LoadResult::Status::Invalid);

    writeBytes(path, QByteArray(
        AIConversationLimits::kMaximumDocumentBytes + 1, 'x'));
    EXPECT_EQ(store.load(scope).status,
              IAIConversationStore::LoadResult::Status::Invalid);
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
    EXPECT_NE(store.storagePathForScope(firstScope),
              store.storagePathForScope(secondScope));

    EXPECT_TRUE(store.clear(firstScope).success);
    EXPECT_TRUE(store.clear(firstScope).success);
    EXPECT_EQ(store.load(firstScope).status,
              IAIConversationStore::LoadResult::Status::Missing);
}

TEST(AIConversationStoreTest, RejectsInvalidRolesAndOversizedMessages)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    LocalAIConversationStore store(directory.path());

    QJsonArray wrongRoles = sampleConversation();
    QJsonObject first = wrongRoles[0].toObject();
    first.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    wrongRoles[0] = first;
    EXPECT_FALSE(store.save(QStringLiteral("scope"), wrongRoles).success);

    QJsonArray oversized = sampleConversation();
    QJsonObject user = oversized[0].toObject();
    user.insert(QStringLiteral("content"),
                QString(AIConversationLimits::kMaximumMessageBytes + 1,
                        QLatin1Char('x')));
    oversized[0] = user;
    EXPECT_FALSE(store.save(QStringLiteral("scope"), oversized).success);

    QJsonArray tooMany;
    for (int index = 0;
         index < AIConversationLimits::kMaximumMessages + 2;
         ++index)
    {
        tooMany.append(QJsonObject{
            {QStringLiteral("role"),
             index % 2 == 0 ? QStringLiteral("user")
                            : QStringLiteral("assistant")},
            {QStringLiteral("content"), QString()}
        });
    }
    EXPECT_FALSE(store.save(QStringLiteral("scope"), tooMany).success);
}
