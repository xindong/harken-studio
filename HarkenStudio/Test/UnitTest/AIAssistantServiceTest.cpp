#include "AI/AIAssistantService.h"
#include "ActionManager.h"
#include "IPC/IPCServer.h"
#include "Support/AITestSupport.h"
#include "Support/MockLLMProvider.h"

#include <gtest/gtest.h>

#include <QCoreApplication>

using namespace VNX;

namespace {

QJsonObject pingCommand(const QString& requestId)
{
    return QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("ping")},
        {QStringLiteral("args"), QJsonObject{}},
        {QStringLiteral("request_id"), requestId}
    };
}

class AIAssistantServiceTest : public ::testing::Test
{
public:
    static void configureValidSettings(AIAssistantService& service)
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

protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ActionManager::instance().clearAllForTesting();
    }

    void TearDown() override
    {
        ActionManager::instance().clearAllForTesting();
    }
};

}

TEST_F(AIAssistantServiceTest, DropsStalePreviousTurnWhileNewTurnIsActive)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    service.setProvider(provider);
    configureValidSettings(service);

    provider->enqueueCommandResponse(pingCommand(QStringLiteral("ai-0001")));
    service.submitUserMessage(QStringLiteral("First."));
    service.abortCurrentTurn();

    provider->enqueueCommandResponse(pingCommand(QStringLiteral("ai-0002")));
    service.submitUserMessage(QStringLiteral("Second."));

    int dispatches = 0;
    int responses = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("ping")) ++dispatches;
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    ILLMProvider::Response stale;
    stale.requestId = QStringLiteral("ai-0001");
    stale.structuredCommand = pingCommand(QStringLiteral("ai-0001"));
    stale.hasCommand = true;
    stale.finished = true;
    emit provider->completed(stale);
    EXPECT_EQ(dispatches, 0);
    EXPECT_EQ(responses, 0);

    EXPECT_TRUE(provider->deliverNext());
    EXPECT_EQ(dispatches, 1);
    EXPECT_EQ(responses, 1);
}

TEST_F(AIAssistantServiceTest, AbortSuppressesPostToolStaleCompletion)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueToolCallsResponse({
        {QStringLiteral("call-1"), QStringLiteral("ping"), {}}});
    service.setProvider(provider);
    configureValidSettings(service);
    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Abort."));
    ASSERT_TRUE(provider->deliverNext());
    service.abortCurrentTurn();
    ILLMProvider::Response stale;
    stale.requestId = QStringLiteral("ai-0001");
    stale.text = QStringLiteral("stale");
    emit provider->completed(stale);
    EXPECT_EQ(responses, 0);
}

TEST_F(AIAssistantServiceTest, TypedFinalResponseEmitsOnceForDuplicateCompletion)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("final"));
    service.setProvider(provider);
    configureValidSettings(service);
    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Finish."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    ILLMProvider::Response duplicate;
    duplicate.requestId = QStringLiteral("ai-0001");
    duplicate.text = QStringLiteral("duplicate");
    emit provider->completed(duplicate);
    QCoreApplication::processEvents();
    EXPECT_EQ(responses, 1);
}
