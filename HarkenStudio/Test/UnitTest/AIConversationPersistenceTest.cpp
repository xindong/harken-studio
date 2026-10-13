#include "AI/AIAssistantService.h"
#include "AI/AIConversationSessionStore.h"
#include "AI/AIConversationStore.h"
#include "Support/AITestSupport.h"
#include "Support/MockLLMProvider.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <utility>

using namespace VNX;

namespace {

void configureValidSettings(AIAssistantService& service)
{
    service.setUnrestrictedMode(false);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        return defaultValue;
    });
}

class FailingClearConversationStore final : public IAIConversationStore
{
public:
    explicit FailingClearConversationStore(QJsonArray messages)
        : messages_(std::move(messages))
    {
    }

    LoadResult load(const QString&) const override
    {
        LoadResult result;
        result.status = LoadResult::Status::Loaded;
        result.messages = messages_;
        return result;
    }

    OperationResult save(const QString&, const QJsonArray& messages) const override
    {
        messages_ = messages;
        return OperationResult{true, {}, {}};
    }

    OperationResult clear(const QString&) const override
    {
        return OperationResult{
            false,
            QStringLiteral("AI_MEMORY_CLEAR_FAILED"),
            QStringLiteral("Conversation storage could not be removed.")};
    }

private:
    mutable QJsonArray messages_;
};

}

class AIConversationPersistenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(directory_.isValid());
        store_ =
            std::make_shared<LocalAIConversationStore>(directory_.path());
    }

    QTemporaryDir directory_;
    std::shared_ptr<LocalAIConversationStore> store_;
    const QString scope_ = QStringLiteral("project:test");
};

TEST_F(AIConversationPersistenceTest, CompletedTurnRestoresIntoNewService)
{
    {
        AIAssistantService service(nullptr);
        auto* provider = new MockLLMProvider;
        provider->enqueueTextResponse(QStringLiteral("First answer"));
        service.setProvider(provider);
        service.setConversationStore(store_);
        service.setConversationScope(scope_);
        configureValidSettings(service);

        int responses = 0;
        QObject::connect(&service, &AIAssistantService::responseReady, &service,
                         [&](const QString&, const QString&) { ++responses; });
        service.submitUserMessage(QStringLiteral("First question"));
        ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    }

    AIAssistantService restored(nullptr);
    restored.setConversationStore(store_);
    restored.setConversationScope(scope_);
    ASSERT_EQ(restored.conversationHistory().size(), 2);

    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("Follow-up answer"));
    restored.setProvider(provider);
    configureValidSettings(restored);
    restored.submitUserMessage(QStringLiteral("Follow-up question"));

    ASSERT_EQ(provider->submitCount(), 1);
    const QJsonArray messages = provider->lastRequest().messages;
    ASSERT_EQ(messages.size(), 3);
    EXPECT_EQ(messages[0].toObject().value(QStringLiteral("content")),
              QStringLiteral("First question"));
    EXPECT_EQ(messages[1].toObject().value(QStringLiteral("content")),
              QStringLiteral("First answer"));
    EXPECT_EQ(messages[2].toObject().value(QStringLiteral("content")),
              QStringLiteral("Follow-up question"));
}

TEST_F(AIConversationPersistenceTest, FailedAndAbortedTurnsAreNotStored)
{
    {
        AIAssistantService service(nullptr);
        auto* provider = new MockLLMProvider;
        provider->enqueueFailure(QStringLiteral("NETWORK_ERROR"),
                                 QStringLiteral("Offline"));
        service.setProvider(provider);
        service.setConversationStore(store_);
        service.setConversationScope(scope_);
        configureValidSettings(service);
        service.submitUserMessage(QStringLiteral("Failed question"));
        EXPECT_EQ(store_->load(scope_).status,
                  IAIConversationStore::LoadResult::Status::Missing);
    }

    {
        AIAssistantService service(nullptr);
        auto* provider = new MockLLMProvider;
        provider->setAutoDelivery(false);
        provider->enqueueTextResponse(QStringLiteral("Late answer"));
        service.setProvider(provider);
        service.setConversationStore(store_);
        service.setConversationScope(scope_);
        configureValidSettings(service);
        service.submitUserMessage(QStringLiteral("Aborted question"));
        service.abortCurrentTurn();
        EXPECT_EQ(store_->load(scope_).status,
                  IAIConversationStore::LoadResult::Status::Missing);
    }
}

TEST_F(AIConversationPersistenceTest, NewConversationClearsDurableSnapshot)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("Answer"));
    service.setProvider(provider);
    service.setConversationStore(store_);
    service.setConversationScope(scope_);
    configureValidSettings(service);

    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    ASSERT_EQ(store_->load(scope_).status,
              IAIConversationStore::LoadResult::Status::Loaded);

    service.clearConversation();

    EXPECT_TRUE(service.conversationHistory().isEmpty());
    EXPECT_EQ(store_->load(scope_).status,
              IAIConversationStore::LoadResult::Status::Missing);
}

TEST_F(AIConversationPersistenceTest,
       FailedDurableClearKeepsConversationVisibleAndRestorable)
{
    const QJsonArray history{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("Question")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), QStringLiteral("Answer")}}
    };
    auto failingStore =
        std::make_shared<FailingClearConversationStore>(history);

    AIAssistantService service(nullptr);
    service.setConversationStore(failingStore);
    service.setConversationScope(scope_);

    int clearedCount = 0;
    QString diagnosticCode;
    QObject::connect(&service, &AIAssistantService::conversationCleared, &service,
                     [&]() { ++clearedCount; });
    QObject::connect(&service, &AIAssistantService::diagnosticReady, &service,
                     [&](const AIServiceDiagnostic& diagnostic) {
        diagnosticCode = diagnostic.code;
    });

    service.clearConversation();

    EXPECT_EQ(clearedCount, 0);
    EXPECT_EQ(diagnosticCode, QStringLiteral("AI_MEMORY_CLEAR_FAILED"));
    EXPECT_EQ(service.conversationHistory(), history);
    EXPECT_EQ(failingStore->load(scope_).messages, history);
}

TEST_F(AIConversationPersistenceTest, InvalidSnapshotEmitsDiagnosticAndStartsFresh)
{
    const QString path = store_->storagePathForScope(scope_);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(QByteArrayLiteral("{invalid")),
              QByteArrayLiteral("{invalid").size());
    file.close();

    AIAssistantService service(nullptr);
    QString diagnosticCode;
    QObject::connect(&service, &AIAssistantService::diagnosticReady, &service,
                     [&](const AIServiceDiagnostic& diagnostic) {
        diagnosticCode = diagnostic.code;
    });
    service.setConversationStore(store_);
    service.setConversationScope(scope_);

    EXPECT_EQ(diagnosticCode, QStringLiteral("AI_MEMORY_INVALID"));
    EXPECT_TRUE(service.conversationHistory().isEmpty());
}

TEST_F(AIConversationPersistenceTest, SwitchingScopeRestoresIndependentHistory)
{
    const QJsonArray first{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("First question")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), QStringLiteral("First answer")}}
    };
    const QJsonArray second{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("Second question")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), QStringLiteral("Second answer")}}
    };
    ASSERT_TRUE(store_->save(QStringLiteral("project:first"), first).success);
    ASSERT_TRUE(store_->save(QStringLiteral("project:second"), second).success);

    AIAssistantService service(nullptr);
    service.setConversationStore(store_);
    service.setConversationScope(QStringLiteral("project:first"));
    EXPECT_EQ(service.conversationHistory(), first);
    service.setConversationScope(QStringLiteral("project:second"));
    EXPECT_EQ(service.conversationHistory(), second);
}

TEST_F(AIConversationPersistenceTest,
       LegacyConversationBecomesDefaultSessionWithoutDataMigration)
{
    const QJsonArray legacy{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("Legacy question")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), QStringLiteral("Legacy answer")}}
    };
    ASSERT_TRUE(store_->save(scope_, legacy).success);
    auto sessionStore =
        std::make_shared<LocalAIConversationSessionStore>(
            directory_.path());

    AIAssistantService service(nullptr);
    service.setConversationStore(store_);
    service.setConversationSessionStore(sessionStore);
    service.setConversationScope(scope_);

    EXPECT_EQ(service.conversationHistory(), legacy);
    ASSERT_EQ(service.conversationSessions().size(), 1);
    EXPECT_EQ(
        service.activeConversationSessionId(),
        QStringLiteral(
            "00000000-0000-4000-8000-000000000001"));
    EXPECT_EQ(
        sessionStore->load(scope_).status,
        AIConversationSessionLoadResult::Status::Loaded);
}

TEST_F(AIConversationPersistenceTest,
       CreatesSwitchesAndRestoresIndependentSessions)
{
    auto sessionStore =
        std::make_shared<LocalAIConversationSessionStore>(
            directory_.path());
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("First answer"));
    provider->enqueueTextResponse(QStringLiteral("Second answer"));
    service.setProvider(provider);
    service.setConversationStore(store_);
    service.setConversationSessionStore(sessionStore);
    service.setConversationScope(scope_);
    configureValidSettings(service);

    int responses = 0;
    QObject::connect(
        &service, &AIAssistantService::responseReady, &service,
        [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("First topic"));
    ASSERT_TRUE(TestSupport::spinUntil(
        [&]() { return responses == 1; }));
    const QString firstId =
        service.activeConversationSessionId();

    service.createConversationSession();
    const QString secondId =
        service.activeConversationSessionId();
    EXPECT_NE(secondId, firstId);
    EXPECT_TRUE(service.conversationHistory().isEmpty());
    service.submitUserMessage(QStringLiteral("Second topic"));
    ASSERT_TRUE(TestSupport::spinUntil(
        [&]() { return responses == 2; }));
    ASSERT_EQ(service.conversationSessions().size(), 2);

    service.switchConversationSession(firstId);
    ASSERT_EQ(service.conversationHistory().size(), 2);
    EXPECT_EQ(
        service.conversationHistory()[0].toObject()
            .value(QStringLiteral("content")).toString(),
        QStringLiteral("First topic"));
    service.switchConversationSession(secondId);
    EXPECT_EQ(
        service.conversationHistory()[0].toObject()
            .value(QStringLiteral("content")).toString(),
        QStringLiteral("Second topic"));

    AIAssistantService restored(nullptr);
    restored.setConversationStore(store_);
    restored.setConversationSessionStore(sessionStore);
    restored.setConversationScope(scope_);
    EXPECT_EQ(
        restored.activeConversationSessionId(),
        secondId);
    ASSERT_EQ(restored.conversationHistory().size(), 2);
    EXPECT_EQ(
        restored.conversationHistory()[0].toObject()
            .value(QStringLiteral("content")).toString(),
        QStringLiteral("Second topic"));

    const QJsonArray sessions = restored.conversationSessions();
    QStringList titles;
    for (const QJsonValue& value : sessions)
    {
        titles.append(
            value.toObject().value(
                QStringLiteral("title")).toString());
    }
    EXPECT_TRUE(titles.contains(QStringLiteral("First topic")));
    EXPECT_TRUE(titles.contains(QStringLiteral("Second topic")));
}
