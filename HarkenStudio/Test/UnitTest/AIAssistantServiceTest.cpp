#include "AI/AIAssistantService.h"
#include "AI/AIDurableMemoryService.h"
#include "AI/AIDurableMemoryStore.h"
#include "ActionManager.h"
#include "UndoCommands.h"
#include "IPC/IPCServer.h"
#include "Support/AITestSupport.h"
#include "Support/MockLLMProvider.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <tuple>

using namespace VNX;

namespace {

QJsonArray enabledToolsPayload()
{
    QJsonArray tools;
    for (const AICommandSpec& spec : AICommandCatalog::enabled())
    {
        tools.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                {QStringLiteral("name"), spec.cmd},
                {QStringLiteral("description"), spec.description},
                {QStringLiteral("parameters"), spec.argumentSchema}
            }}
        });
    }
    return tools;
}

QJsonObject pingCommand(const QString& requestId)
{
    return QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("ping")},
        {QStringLiteral("args"), QJsonObject{}},
        {QStringLiteral("request_id"), requestId}
    };
}

QJsonObject setItemVolumeCommand(const QString& requestId, int objectId = 7,
                                 double value = -6.0)
{
    return QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("set_item_property")},
        {QStringLiteral("args"), QJsonObject{
            {QStringLiteral("object_id"), objectId},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), value}
        }},
        {QStringLiteral("request_id"), requestId}
    };
}

ILLMProvider::ToolCall toolCall(const QString& id, const QString& name,
                                const QJsonObject& arguments = {})
{
    return {id, name, arguments};
}

void registerBareSetItemPropertyAction()
{
    ActionManager::instance().registerAction({QStringLiteral("set_item_property"), {}, {}, {}, false,
        [](const QJsonObject& args) { return ActionResult{true, args}; }});
}

ILLMProvider::ToolCall setItemVolumeToolCall(const QString& id, double value = -3.0)
{
    return toolCall(id, QStringLiteral("set_item_property"),
                    QJsonObject{{QStringLiteral("object_id"), 7},
                                {QStringLiteral("property"), QStringLiteral("volume")},
                                {QStringLiteral("value"), value}});
}

AIDurableMemoryEntry storedMemoryEntry(
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
    entry.sourceReference = QStringLiteral("test-fixture");
    entry.createdAtMs = updatedAtMs;
    entry.updatedAtMs = updatedAtMs;
    entry.revision = 1;
    return entry;
}

class KeyRequiredMockProvider : public MockLLMProvider
{
public:
    using MockLLMProvider::MockLLMProvider;
    bool requiresApiKey() const override { return true; }
};

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

    static void configureValidUnrestrictedSettings(
        AIAssistantService& service)
    {
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
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        ActionManager::instance().clearAllForTesting();
        UndoRedoManager::instance().clear();
    }
};

void expectBoundedStepEvent(const AIStepEvent& event)
{
    EXPECT_TRUE(event.requestId.startsWith(QStringLiteral("ai-")));
    EXPECT_LE(event.requestId.size(), AIStepEvent::kMaximumRequestIdCharacters);
    EXPECT_LE(event.command.size(), AIStepEvent::kMaximumCommandCharacters);
    EXPECT_GE(event.modelRound, 0);
    EXPECT_LE(event.modelRound, AIStepEvent::kMaximumModelRounds);
    EXPECT_GE(event.toolCount, 0);
    EXPECT_LE(event.toolCount, AIStepEvent::kMaximumToolCount);
    EXPECT_GE(event.toolLimit, 0);
    EXPECT_LE(event.toolLimit, AIStepEvent::kMaximumToolLimit);
    EXPECT_GE(event.toolExecutionCount, 0);
    EXPECT_LE(event.toolExecutionCount, AIStepEvent::kMaximumToolLimit);
}

}

TEST_F(AIAssistantServiceTest,
       UnrestrictedModeAdvertisesExplicitMemoryTools)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("pending"));
    service.setProvider(provider);
    configureValidUnrestrictedSettings(service);

    service.submitUserMessage(QStringLiteral("Inspect available tools."));

    QStringList toolNames;
    for (const AICommandSpec& tool : provider->lastRequest().tools)
        toolNames.append(tool.cmd);
    EXPECT_TRUE(toolNames.contains(QStringLiteral("memory_list")));
    EXPECT_TRUE(toolNames.contains(QStringLiteral("memory_upsert")));
    EXPECT_TRUE(toolNames.contains(QStringLiteral("memory_remove")));
    EXPECT_TRUE(toolNames.contains(QStringLiteral("memory_clear")));
    EXPECT_TRUE(provider->lastRequest().systemPrompt.contains(
        QStringLiteral("Never infer or save a memory from an ordinary conversation.")));
    service.abortCurrentTurn();
}

TEST_F(AIAssistantServiceTest,
       OrdinaryConversationDoesNotCreateDurableMemory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString scope = QStringLiteral("project:C:/Projects/Ordinary");
    auto store = std::make_shared<LocalAIDurableMemoryStore>(
        directory.path());
    auto memoryService =
        std::make_shared<AIDurableMemoryService>(store);

    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("ordinary response"));
    service.setProvider(provider);
    service.setDurableMemoryService(memoryService);
    service.setConversationScope(scope);
    configureValidUnrestrictedSettings(service);

    int responses = 0;
    QObject::connect(
        &service, &AIAssistantService::responseReady, &service,
        [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Discuss this project."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));

    EXPECT_EQ(memoryService->list(scope).status,
              AIDurableMemoryLoadResult::Status::Missing);
}

TEST_F(AIAssistantServiceTest,
       RetrievesProjectAndUserMemoryWithTraceableIds)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectScope =
        QStringLiteral("project:C:/Projects/Retrieval");
    auto store = std::make_shared<LocalAIDurableMemoryStore>(
        directory.path());
    const QString projectId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString userId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    ASSERT_TRUE(store->save(
        projectScope,
        {storedMemoryEntry(
            projectId,
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("output.format"),
            QStringLiteral("VST3 and Standalone"),
            1000)}).success);
    ASSERT_TRUE(store->save(
        QStringLiteral("user:default"),
        {storedMemoryEntry(
            userId,
            AIDurableMemoryKind::UserPreference,
            QStringLiteral("response.language"),
            QStringLiteral("Chinese"),
            2000)}).success);

    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    service.setDurableMemoryService(
        std::make_shared<AIDurableMemoryService>(store));
    service.setConversationScope(projectScope);
    configureValidUnrestrictedSettings(service);

    int responses = 0;
    QObject::connect(
        &service, &AIAssistantService::responseReady, &service,
        [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(
        QStringLiteral("Confirm the output format and response language."));

    ASSERT_EQ(provider->submitCount(), 1);
    const QString systemPrompt = provider->lastRequest().systemPrompt;
    EXPECT_TRUE(systemPrompt.contains(
        QStringLiteral("<DURABLE_MEMORY_CONTEXT>")));
    EXPECT_TRUE(systemPrompt.contains(projectId));
    EXPECT_TRUE(systemPrompt.contains(userId));
    EXPECT_LT(systemPrompt.indexOf(projectId),
              systemPrompt.indexOf(userId));
    EXPECT_FALSE(systemPrompt.contains(projectScope));
    EXPECT_TRUE(service.lastUsedDurableMemoryIds().isEmpty());

    ASSERT_TRUE(provider->deliverNext());
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    EXPECT_EQ(
        service.lastUsedDurableMemoryIds(),
        (QStringList{projectId, userId}));
}

TEST_F(AIAssistantServiceTest,
       TightBudgetOmitsDurableMemoryBeforeConversationHistory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectScope =
        QStringLiteral("project:C:/Projects/TightBudget");
    auto store = std::make_shared<LocalAIDurableMemoryStore>(
        directory.path());
    const QString memoryId =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    ASSERT_TRUE(store->save(
        projectScope,
        {storedMemoryEntry(
            memoryId,
            AIDurableMemoryKind::ProjectFact,
            QStringLiteral("output.format"),
            QStringLiteral("VST3"),
            1000)}).success);

    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    const QString firstQuestion(2300, QLatin1Char('q'));
    const QString firstAnswer(2300, QLatin1Char('a'));
    provider->enqueueTextResponse(firstAnswer);
    provider->enqueueTextResponse(QStringLiteral("second answer"));
    service.setProvider(provider);
    service.setDurableMemoryService(
        std::make_shared<AIDurableMemoryService>(store));
    service.setConversationScope(projectScope);
    service.setUnrestrictedMode(false);
    const QJsonArray tools = enabledToolsPayload();
    const int contextWindow = 8192
        + QJsonDocument(tools).toJson(QJsonDocument::Compact).size() - 2;
    service.setSettingsValueLoaderForTesting(
        [contextWindow](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("small-context-model"));
        if (key == QLatin1String("context_window_tokens"))
            return QVariant(contextWindow);
        return defaultValue;
    });

    int responses = 0;
    QObject::connect(
        &service, &AIAssistantService::responseReady, &service,
        [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(firstQuestion);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    ASSERT_TRUE(provider->submittedRequests().constFirst()
                    .systemPrompt.contains(memoryId));
    service.submitUserMessage(QStringLiteral("Confirm the output format."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 2; }));

    const QList<ILLMProvider::Request> requests =
        provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    const ILLMProvider::Request& second = requests.constLast();
    EXPECT_FALSE(second.systemPrompt.contains(memoryId));
    EXPECT_FALSE(second.systemPrompt.contains(
        QStringLiteral("<DURABLE_MEMORY_CONTEXT>")));
    ASSERT_EQ(second.messages.size(), 3);
    EXPECT_EQ(
        second.messages.at(0).toObject()
            .value(QStringLiteral("content")).toString(),
        firstQuestion);
    EXPECT_EQ(
        second.messages.at(1).toObject()
            .value(QStringLiteral("content")).toString(),
        firstAnswer);

    QJsonArray wireMessages{
        QJsonObject{
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("content"), second.systemPrompt}
        }
    };
    for (const QJsonValue& message : second.messages)
        wireMessages.append(message);
    const int requestBytes = QJsonDocument(QJsonObject{
        {QStringLiteral("messages"), wireMessages},
        {QStringLiteral("tools"), tools}
    }).toJson(QJsonDocument::Compact).size();
    EXPECT_LE(requestBytes, contextWindow - 1024 - 2048);
}

TEST_F(AIAssistantServiceTest,
       ExplicitMemoryUpsertPersistsAndCanBeListedAfterRestart)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString scope = QStringLiteral("project:C:/Projects/Remembered");
    auto store = std::make_shared<LocalAIDurableMemoryStore>(
        directory.path());
    auto memoryService =
        std::make_shared<AIDurableMemoryService>(store);

    {
        AIAssistantService service(nullptr);
        auto* provider = new MockLLMProvider;
        provider->enqueueToolCallsResponse({
            toolCall(QStringLiteral("memory-write"),
                     QStringLiteral("memory_upsert"),
                     QJsonObject{
                         {QStringLiteral("scope"),
                          QStringLiteral("project")},
                         {QStringLiteral("key"),
                          QStringLiteral("output format")},
                         {QStringLiteral("value"),
                          QStringLiteral("VST3 and Standalone")}})});
        provider->enqueueTextResponse(QStringLiteral("remembered"));
        service.setProvider(provider);
        service.setDurableMemoryService(memoryService);
        service.setConversationScope(scope);
        configureValidUnrestrictedSettings(service);

        int responses = 0;
        QObject::connect(
            &service, &AIAssistantService::responseReady, &service,
            [&](const QString&, const QString&) { ++responses; });
        service.submitUserMessage(
            QStringLiteral("Remember the required output format."));
        ASSERT_TRUE(
            TestSupport::spinUntil([&]() { return responses == 1; }));
    }

    const auto stored = memoryService->list(scope);
    ASSERT_EQ(stored.status,
              AIDurableMemoryLoadResult::Status::Loaded);
    ASSERT_EQ(stored.entries.size(), 1);
    EXPECT_EQ(stored.entries.constFirst().kind,
              AIDurableMemoryKind::ProjectFact);
    EXPECT_EQ(stored.entries.constFirst().key,
              QStringLiteral("output format"));
    EXPECT_EQ(stored.entries.constFirst().value,
              QStringLiteral("VST3 and Standalone"));
    EXPECT_EQ(stored.entries.constFirst().source,
              AIDurableMemorySource::Model);
    EXPECT_EQ(stored.entries.constFirst().sourceReference,
              QStringLiteral("ai-0001"));

    AIAssistantService restarted(nullptr);
    auto* restartedProvider = new MockLLMProvider;
    restartedProvider->enqueueToolCallsResponse({
        toolCall(QStringLiteral("memory-read"),
                 QStringLiteral("memory_list"),
                 QJsonObject{
                     {QStringLiteral("scope"),
                      QStringLiteral("project")}})});
    restartedProvider->enqueueTextResponse(QStringLiteral("listed"));
    restarted.setProvider(restartedProvider);
    restarted.setDurableMemoryService(memoryService);
    restarted.setConversationScope(scope);
    configureValidUnrestrictedSettings(restarted);

    int responses = 0;
    QObject::connect(
        &restarted, &AIAssistantService::responseReady, &restarted,
        [&](const QString&, const QString&) { ++responses; });
    restarted.submitUserMessage(QStringLiteral("List saved project memory."));
    ASSERT_TRUE(
        TestSupport::spinUntil([&]() { return responses == 1; }));
    const QList<ILLMProvider::Request> requests =
        restartedProvider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    ASSERT_EQ(requests.constLast().messages.size(), 3);
    const QJsonArray listedMessages = requests.constLast().messages;
    const QString toolResult = listedMessages.at(listedMessages.size() - 1)
        .toObject().value(QStringLiteral("content")).toString();
    EXPECT_TRUE(toolResult.contains(QStringLiteral("project_fact")));
    EXPECT_TRUE(toolResult.contains(QStringLiteral("output format")));
    EXPECT_TRUE(toolResult.contains(QStringLiteral("ai-0001")));
}

TEST_F(AIAssistantServiceTest,
       FailedTurnDiscardsPendingDurableMemoryMutation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString scope =
        QStringLiteral("project:C:/Projects/FailedMemory");
    auto store = std::make_shared<LocalAIDurableMemoryStore>(
        directory.path());
    auto memoryService =
        std::make_shared<AIDurableMemoryService>(store);

    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({
        toolCall(QStringLiteral("memory-write"),
                 QStringLiteral("memory_upsert"),
                 QJsonObject{
                     {QStringLiteral("scope"),
                      QStringLiteral("project")},
                     {QStringLiteral("key"),
                      QStringLiteral("failed fact")},
                     {QStringLiteral("value"),
                      QStringLiteral("must not persist")}})});
    provider->enqueueFailure(
        QStringLiteral("NETWORK_ERROR"),
        QStringLiteral("Final response failed"));
    service.setProvider(provider);
    service.setDurableMemoryService(memoryService);
    service.setConversationScope(scope);
    configureValidUnrestrictedSettings(service);

    QString failureCode;
    QObject::connect(
        &service, &AIAssistantService::failed, &service,
        [&](const QString&, const QString& code, const QString&) {
            failureCode = code;
        });
    service.submitUserMessage(
        QStringLiteral("Remember this, then fail."));
    ASSERT_TRUE(TestSupport::spinUntil(
        [&]() { return !failureCode.isEmpty(); }));

    EXPECT_EQ(failureCode, QStringLiteral("NETWORK_ERROR"));
    EXPECT_EQ(
        memoryService->list(scope).status,
        AIDurableMemoryLoadResult::Status::Missing);
}

TEST_F(AIAssistantServiceTest,
       ProjectMemoryToolRejectsWhenNoSavedProjectIsOpen)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto store = std::make_shared<LocalAIDurableMemoryStore>(
        directory.path());
    auto memoryService =
        std::make_shared<AIDurableMemoryService>(store);

    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({
        toolCall(QStringLiteral("memory-write"),
                 QStringLiteral("memory_upsert"),
                 QJsonObject{
                     {QStringLiteral("scope"),
                      QStringLiteral("project")},
                     {QStringLiteral("key"), QStringLiteral("fact")},
                     {QStringLiteral("value"), QStringLiteral("value")}})});
    provider->enqueueTextResponse(QStringLiteral("not saved"));
    service.setProvider(provider);
    service.setDurableMemoryService(memoryService);
    service.setConversationScope(QStringLiteral("user:default"));
    configureValidUnrestrictedSettings(service);

    int responses = 0;
    QObject::connect(
        &service, &AIAssistantService::responseReady, &service,
        [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Remember a project fact."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));

    const QList<ILLMProvider::Request> requests =
        provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    const QJsonArray responseMessages = requests.constLast().messages;
    const QString toolResult =
        responseMessages.at(responseMessages.size() - 1)
            .toObject().value(QStringLiteral("content")).toString();
    EXPECT_TRUE(toolResult.contains(
        QStringLiteral("AI_DURABLE_MEMORY_SCOPE_UNAVAILABLE")));
    EXPECT_EQ(memoryService->list(QStringLiteral("user:default")).status,
              AIDurableMemoryLoadResult::Status::Missing);
}

TEST(AIStepEventTest, BoundsExternalStringsAndCounters)
{
    const QString longText(512, QLatin1Char('x'));
    const AIStepEvent step(AIStepPhase::ToolExecution, longText, longText,
                           -4, 999, -1, 999);
    EXPECT_EQ(step.requestId.size(), AIStepEvent::kMaximumRequestIdCharacters);
    EXPECT_EQ(step.command.size(), AIStepEvent::kMaximumCommandCharacters);
    EXPECT_EQ(step.modelRound, 0);
    EXPECT_EQ(step.toolCount, AIStepEvent::kMaximumToolCount);
    EXPECT_EQ(step.toolLimit, 0);
    EXPECT_EQ(step.toolExecutionCount, AIStepEvent::kMaximumToolLimit);

    const AIServiceDiagnostic diagnostic(longText, longText);
    EXPECT_EQ(diagnostic.code.size(), AIServiceDiagnostic::kMaximumCodeCharacters);
    EXPECT_EQ(diagnostic.message.size(), AIServiceDiagnostic::kMaximumMessageCharacters);
}

TEST_F(AIAssistantServiceTest, DispatchesValidatedReadOnlyCommandWithInternalContext)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    provider->enqueueCommandResponse(pingCommand(QStringLiteral("ai-0001")));

    ActionSource observedSource = ActionSource::GUI;
    int pingDispatches = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&,
                         ActionSource source) {
        if (id == QLatin1String("ping"))
        {
            ++pingDispatches;
            observedSource = source;
        }
    });

    QString responseId;
    QString responseText;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString& id, const QString& text) {
        responseId = id;
        responseText = text;
    });

    service.submitUserMessage(QStringLiteral("Check the Editor."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !responseId.isEmpty(); }));
    EXPECT_EQ(provider->submitCount(), 1);
    EXPECT_EQ(responseId, QStringLiteral("ai-0001"));
    EXPECT_EQ(pingDispatches, 1);
    EXPECT_EQ(observedSource, ActionSource::Internal);

    const QJsonObject envelope = QJsonDocument::fromJson(responseText.toUtf8()).object();
    EXPECT_EQ(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(envelope.value(QStringLiteral("request_id")).toString(),
              QStringLiteral("ai-0001"));
    EXPECT_TRUE(envelope.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("pong")).toBool());
}

TEST_F(AIAssistantServiceTest, SuccessfulTurnsCarryConversationIntoNextRequest)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    provider->enqueueTextResponse(QStringLiteral("First answer"));
    provider->enqueueTextResponse(QStringLiteral("Second answer"));

    int responseCount = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responseCount; });

    service.submitUserMessage(QStringLiteral("First question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 1; }));
    service.submitUserMessage(QStringLiteral("Follow-up question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 2; }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    ASSERT_EQ(requests[0].messages.size(), 1);
    ASSERT_EQ(requests[1].messages.size(), 3);
    EXPECT_EQ(requests[1].messages[0].toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("user"));
    EXPECT_EQ(requests[1].messages[0].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("First question"));
    EXPECT_EQ(requests[1].messages[1].toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("assistant"));
    EXPECT_EQ(requests[1].messages[1].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("First answer"));
    EXPECT_EQ(requests[1].messages[2].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("Follow-up question"));
}

TEST_F(AIAssistantServiceTest, HistoryBudgetEvictsOldestCompleteTurn)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    configureValidSettings(service);

    const QString firstQuestion =
        QStringLiteral("first-") + QString(20000, QLatin1Char('a'));
    const QString firstAnswer =
        QStringLiteral("first-") + QString(20000, QLatin1Char('b'));
    const QString secondQuestion =
        QStringLiteral("second-") + QString(20000, QLatin1Char('c'));
    const QString secondAnswer =
        QStringLiteral("second-") + QString(20000, QLatin1Char('d'));
    const QString thirdQuestion = QStringLiteral("third");
    provider->enqueueTextResponse(firstAnswer);
    provider->enqueueTextResponse(secondAnswer);
    provider->enqueueTextResponse(QStringLiteral("third answer"));

    int responseCount = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responseCount; });

    service.submitUserMessage(firstQuestion);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 1; }));
    service.submitUserMessage(secondQuestion);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 2; }));
    service.submitUserMessage(thirdQuestion);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 3; }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 3);
    ASSERT_EQ(requests[2].messages.size(), 3);
    EXPECT_EQ(requests[2].messages[0].toObject().value(QStringLiteral("content")).toString(),
              secondQuestion);
    EXPECT_EQ(requests[2].messages[1].toObject().value(QStringLiteral("content")).toString(),
              secondAnswer);
    EXPECT_EQ(requests[2].messages[2].toObject().value(QStringLiteral("content")).toString(),
              thirdQuestion);
    EXPECT_LT(QJsonDocument(requests[2].messages).toJson(QJsonDocument::Compact).size(),
              65536);
}

TEST_F(AIAssistantServiceTest, ContextBudgetEvictsHistoryBeforeNextProviderRequest)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    service.setUnrestrictedMode(false);
    const int contextWindow = 4096
        + QJsonDocument(enabledToolsPayload()).toJson(QJsonDocument::Compact).size() - 2;
    service.setSettingsValueLoaderForTesting(
        [contextWindow](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("small-context-model"));
        if (key == QLatin1String("context_window_tokens"))
            return QVariant(contextWindow);
        return defaultValue;
    });
    provider->enqueueTextResponse(QString(700, QLatin1Char('a')));
    provider->enqueueTextResponse(QStringLiteral("second answer"));

    int responseCount = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responseCount; });

    service.submitUserMessage(QString(700, QLatin1Char('q')));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 1; }));
    service.submitUserMessage(QStringLiteral("next"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 2; }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    ASSERT_EQ(requests[1].messages.size(), 1);
    EXPECT_EQ(requests[1].messages[0].toObject().value(QStringLiteral("content")),
              QStringLiteral("next"));
}

TEST_F(AIAssistantServiceTest, CurrentTurnOverContextBudgetFailsBeforeProvider)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    service.setUnrestrictedMode(false);
    const int contextWindow = 4096
        + QJsonDocument(enabledToolsPayload()).toJson(QJsonDocument::Compact).size() - 2;
    service.setSettingsValueLoaderForTesting(
        [contextWindow](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("small-context-model"));
        if (key == QLatin1String("context_window_tokens"))
            return QVariant(contextWindow);
        return defaultValue;
    });
    provider->enqueueTextResponse(QStringLiteral("first answer"));
    provider->enqueueTextResponse(QStringLiteral("recovered answer"));

    QString failureCode;
    QString failureMessage;
    int responseCount = 0;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString& message) {
        failureCode = code;
        failureMessage = message;
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responseCount; });

    service.submitUserMessage(QStringLiteral("first"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 1; }));
    service.submitUserMessage(QString(2000, QLatin1Char('x')));

    EXPECT_EQ(failureCode, QStringLiteral("CONTEXT_LIMIT_EXCEEDED"));
    EXPECT_TRUE(failureMessage.contains(QStringLiteral("File > Preferences > AI Agent")));
    EXPECT_EQ(provider->submitCount(), 1);

    service.submitUserMessage(QStringLiteral("recovered"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 2; }));
    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    ASSERT_EQ(requests[1].messages.size(), 3);
    EXPECT_EQ(requests[1].messages[0].toObject().value(QStringLiteral("content")),
              QStringLiteral("first"));
}

TEST_F(AIAssistantServiceTest, ContextBudgetIncludesSystemPromptAndToolSchemas)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("small-context-model"));
        if (key == QLatin1String("context_window_tokens"))
            return QVariant(4096);
        return defaultValue;
    });

    QString failureCode;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });
    service.submitUserMessage(QStringLiteral("Short request."));

    EXPECT_EQ(failureCode, QStringLiteral("CONTEXT_LIMIT_EXCEEDED"));
    EXPECT_EQ(provider->submitCount(), 0);
}

TEST_F(AIAssistantServiceTest, ClearConversationDropsCompletedTurnHistory)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    provider->enqueueTextResponse(QStringLiteral("First answer"));
    provider->enqueueTextResponse(QStringLiteral("Fresh answer"));

    int responseCount = 0;
    int clearedCount = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responseCount; });
    QObject::connect(&service, &AIAssistantService::conversationCleared, &service,
                     [&]() { ++clearedCount; });

    service.submitUserMessage(QStringLiteral("First question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 1; }));
    service.clearConversation();
    EXPECT_EQ(clearedCount, 1);

    service.submitUserMessage(QStringLiteral("Fresh question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 2; }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    ASSERT_EQ(requests[1].messages.size(), 1);
    EXPECT_EQ(requests[1].messages[0].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("Fresh question"));
}

TEST_F(AIAssistantServiceTest, FailedTurnDoesNotPolluteCompletedConversation)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    provider->enqueueTextResponse(QStringLiteral("First answer"));
    provider->enqueueFailure(QStringLiteral("NETWORK_ERROR"), QStringLiteral("Offline"));
    provider->enqueueTextResponse(QStringLiteral("Recovered answer"));

    int responseCount = 0;
    int failureCount = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responseCount; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString&, const QString&) { ++failureCount; });

    service.submitUserMessage(QStringLiteral("First question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 1; }));
    service.submitUserMessage(QStringLiteral("Failed question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return failureCount == 1; }));
    service.submitUserMessage(QStringLiteral("Recovered question"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responseCount == 2; }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 3);
    ASSERT_EQ(requests[2].messages.size(), 3);
    EXPECT_EQ(requests[2].messages[0].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("First question"));
    EXPECT_EQ(requests[2].messages[1].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("First answer"));
    EXPECT_EQ(requests[2].messages[2].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("Recovered question"));
}

TEST_F(AIAssistantServiceTest, AbortSuppressesManualLateDelivery)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    service.setProvider(provider);
    configureValidSettings(service);
    provider->enqueueCommandResponse(pingCommand(QStringLiteral("ai-0001")));

    int dispatches = 0;
    int responses = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("ping")) ++dispatches;
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Start."));
    service.abortCurrentTurn();
    EXPECT_FALSE(provider->deliverNext());
    QCoreApplication::processEvents();
    EXPECT_EQ(dispatches, 0);
    EXPECT_EQ(responses, 0);
    EXPECT_EQ(provider->cancelledRequestIds(), QStringList{QStringLiteral("ai-0001")});
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

TEST_F(AIAssistantServiceTest, EmptyKeyFailsBeforeProviderSubmission)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new KeyRequiredMockProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    service.setSecretLoader([](const QString& serviceName, const QString& account) {
        EXPECT_EQ(serviceName, QStringLiteral("HarkenStudio.ai"));
        EXPECT_EQ(account, QStringLiteral("api_key"));
        return QString();
    });

    QString failedId;
    QString failedCode;
    QString failedMessage;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString& id, const QString& code, const QString& message) {
        failedId = id;
        failedCode = code;
        failedMessage = message;
    });

    service.submitUserMessage(QStringLiteral("Needs a provider key."));
    EXPECT_EQ(provider->submitCount(), 0);
    EXPECT_EQ(failedId, QStringLiteral("ai-0001"));
    EXPECT_EQ(failedCode, QStringLiteral("AUTH_MISSING"));
    EXPECT_TRUE(failedMessage.contains(QStringLiteral("File > Preferences > AI Agent")));
}

TEST_F(AIAssistantServiceTest, SettingsPathClampsLegacyBypassModeToAsk)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("pending"));
    service.setProvider(provider);
    configureValidSettings(service);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("permission_mode"))
            return QVariant(QStringLiteral("bypass"));
        return defaultValue;
    });

    service.submitUserMessage(QStringLiteral("Use conservative permissions."));
    EXPECT_EQ(provider->submitCount(), 1);
    const QList<AICommandSpec> enabledTools = AICommandCatalog::enabled();
    ASSERT_EQ(provider->lastRequest().tools.size(), enabledTools.size());
    for (int i = 0; i < enabledTools.size(); ++i)
    {
        EXPECT_EQ(provider->lastRequest().tools.at(i).cmd, enabledTools.at(i).cmd);
        EXPECT_EQ(provider->lastRequest().tools.at(i).argumentSchema,
                  enabledTools.at(i).argumentSchema);
    }
    EXPECT_TRUE(provider->lastRequest().systemPrompt.contains(
        QStringLiteral("permission profile is ask")));
    EXPECT_FALSE(provider->lastRequest().systemPrompt.contains(QStringLiteral("bypass")));
}

TEST_F(AIAssistantServiceTest, PermissionModeSelectsCompleteAllowedToolSet)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    service.setProvider(provider);
    configureValidSettings(service);

    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("permission_mode"))
            return QVariant(QStringLiteral("read_only"));
        return defaultValue;
    });
    provider->enqueueTextResponse(QStringLiteral("read-only"));
    service.submitUserMessage(QStringLiteral("Read status."));
    ASSERT_EQ(provider->submitCount(), 1);
    ASSERT_FALSE(provider->lastRequest().tools.isEmpty());
    for (const auto& spec : provider->lastRequest().tools)
    {
        EXPECT_EQ(spec.effect, AICommandSpec::Effect::Query) << spec.cmd.toStdString();
    }
    EXPECT_TRUE(provider->lastRequest().systemPrompt.contains(
        QStringLiteral("permission profile is read_only")));
    service.abortCurrentTurn();

    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("permission_mode"))
            return QVariant(QStringLiteral("ask"));
        return defaultValue;
    });
    provider->enqueueTextResponse(QStringLiteral("ready"));
    service.submitUserMessage(QStringLiteral("Use all available tools."));
    ASSERT_EQ(provider->submitCount(), 2);
    QSet<QString> expectedCommands;
    for (const auto& spec : AICommandCatalog::all())
    {
        if (spec.availability == AICommandSpec::Availability::Enabled)
            expectedCommands.insert(spec.cmd);
    }
    QSet<QString> actualCommands;
    for (const auto& spec : provider->lastRequest().tools)
        actualCommands.insert(spec.cmd);
    EXPECT_EQ(actualCommands, expectedCommands);
    EXPECT_TRUE(provider->lastRequest().systemPrompt.contains(
        QStringLiteral("permission profile is ask")));
}

TEST_F(AIAssistantServiceTest, DestructorSilentlyTearsDownActiveTurn)
{
    QList<AIStepEvent> steps;
    IPCServer server(nullptr);
    {
        AIAssistantService service(&server);
        auto* provider = new MockLLMProvider;
        provider->setAutoDelivery(false);
        service.setProvider(provider);
        configureValidSettings(service);
        QObject::connect(&service, &AIAssistantService::stepEventReady, &service,
                         [&](const AIStepEvent& value) { steps.append(value); });

        provider->enqueueTextResponse(QStringLiteral("pending"));
        service.submitUserMessage(QStringLiteral("Leave the turn active."));
        ASSERT_EQ(provider->submitCount(), 1);
        ASSERT_EQ(steps.size(), 1);
        EXPECT_EQ(steps.at(0).phase, AIStepPhase::ModelWaiting);
    }
    ASSERT_EQ(steps.size(), 1);
    EXPECT_EQ(steps.at(0).phase, AIStepPhase::ModelWaiting);
}

TEST_F(AIAssistantServiceTest, EmitsBoundedStepEventsForToolExecutionAndCompletion)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({toolCall(QStringLiteral("call-1"),
                                                 QStringLiteral("ping"))});
    provider->enqueueTextResponse(QStringLiteral("Tool completed."));
    service.setProvider(provider);
    configureValidSettings(service);

    QList<AIStepEvent> events;
    QString response;
    QObject::connect(&service, &AIAssistantService::stepEventReady, &service,
                     [&](const AIStepEvent& event) {
        events.append(event);
        expectBoundedStepEvent(event);
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString& text) { response = text; });

    service.submitUserMessage(QStringLiteral("Run a read-only tool."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !response.isEmpty(); }));

    QStringList phases;
    for (const auto& event : events)
        phases.append(QString::number(static_cast<int>(event.phase)));
    EXPECT_EQ(phases, (QStringList{QStringLiteral("0"), QStringLiteral("2"),
                                  QStringLiteral("1"), QStringLiteral("4")}));
    EXPECT_EQ(response, QStringLiteral("Tool completed."));
}

TEST_F(AIAssistantServiceTest, EmitsConfirmationWaitingAndCancelledStepEvents)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    service.setUnrestrictedMode(false);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({setItemVolumeToolCall(QStringLiteral("call-1"))});
    provider->setAutoDelivery(true);
    service.setProvider(provider);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("permission_mode"))
            return QVariant(QStringLiteral("ask"));
        return defaultValue;
    });

    QList<AIStepEvent> events;
    bool confirmationShown = false;
    QObject::connect(&service, &AIAssistantService::stepEventReady, &service,
                     [&](const AIStepEvent& event) {
        events.append(event);
        expectBoundedStepEvent(event);
    });
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString&, const QJsonObject&) { confirmationShown = true; });

    service.submitUserMessage(QStringLiteral("Ask before changing the volume."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return confirmationShown; }));
    service.abortCurrentTurn();

    ASSERT_GE(events.size(), 3);
    EXPECT_EQ(events.at(0).phase, AIStepPhase::ModelWaiting);
    EXPECT_EQ(events.at(1).phase, AIStepPhase::ConfirmationWaiting);
    EXPECT_EQ(events.constLast().phase, AIStepPhase::Cancelled);
}

TEST_F(AIAssistantServiceTest, EmitsBoundedFailedStepEventWithoutCredentials)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueFailure(QStringLiteral("MOCK_FAILURE"),
                             QStringLiteral("provider failed with secret-looking detail"));
    service.setProvider(provider);
    configureValidSettings(service);

    QList<AIStepEvent> events;
    QString failureCode;
    QObject::connect(&service, &AIAssistantService::stepEventReady, &service,
                     [&](const AIStepEvent& event) {
        events.append(event);
        expectBoundedStepEvent(event);
    });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });

    service.submitUserMessage(QStringLiteral("Fail this request."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !failureCode.isEmpty(); }));

    ASSERT_FALSE(events.isEmpty());
    EXPECT_EQ(events.constLast().phase, AIStepPhase::Failed);
    EXPECT_EQ(failureCode, QStringLiteral("MOCK_FAILURE"));
}

TEST_F(AIAssistantServiceTest, PropagatesConfiguredRequestLimitsAndFallsBackOutOfRange)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("first"));
    provider->enqueueTextResponse(QStringLiteral("second"));
    service.setProvider(provider);
    configureValidSettings(service);

    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("max_tokens"))
            return QVariant(32768);
        if (key == QLatin1String("request_timeout_ms"))
            return QVariant(1000);
        return defaultValue;
    });
    service.submitUserMessage(QStringLiteral("Configured limits."));
    ASSERT_EQ(provider->submitCount(), 1);
    EXPECT_EQ(provider->lastRequest().maxTokens, 32768);
    EXPECT_EQ(provider->lastRequest().timeoutMs, 1000);
    service.abortCurrentTurn();

    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("max_tokens"))
            return QVariant(32769);
        if (key == QLatin1String("request_timeout_ms"))
            return QVariant(999);
        return defaultValue;
    });
    service.submitUserMessage(QStringLiteral("Invalid configured limits."));
    ASSERT_EQ(provider->submitCount(), 2);
    EXPECT_EQ(provider->lastRequest().maxTokens, 1024);
    EXPECT_EQ(provider->lastRequest().timeoutMs, 30000);
}

class InvalidAIConfigurationTest : public AIAssistantServiceTest,
                                   public ::testing::WithParamInterface<std::tuple<QString, QString>>
{
};

TEST_P(InvalidAIConfigurationTest, RejectsBeforeSecretOrProviderAccess)
{
    const auto [baseUrl, model] = GetParam();
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new KeyRequiredMockProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    int secretLoads = 0;
    service.setSecretLoader([&](const QString&, const QString&) {
        ++secretLoads;
        return QStringLiteral("secret");
    });
    service.setSettingsValueLoaderForTesting(
        [&](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(baseUrl);
        if (key == QLatin1String("model"))
            return QVariant(model);
        return defaultValue;
    });

    QString code;
    QString message;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& value, const QString& detail) {
        code = value;
        message = detail;
    });
    service.submitUserMessage(QStringLiteral("Invalid configuration."));

    EXPECT_EQ(code, QStringLiteral("INVALID_ARGUMENTS"));
    EXPECT_TRUE(message.contains(QStringLiteral("File > Preferences > AI Agent")));
    EXPECT_LE(message.size(), 4096);
    EXPECT_EQ(secretLoads, 0);
    EXPECT_EQ(provider->submitCount(), 0);
}

INSTANTIATE_TEST_SUITE_P(InvalidConfigurations, InvalidAIConfigurationTest,
    ::testing::Values(
        std::make_tuple(QString(), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("not a url"), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("ftp://example.test/v1"), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("http://example.test/v1"), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("http://127.0.0.1:1234/v1"), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("https://user:password@example.test/v1"), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("https:///v1"), QStringLiteral("model")),
        std::make_tuple(QStringLiteral("https://example.test/v1"), QString())));


TEST_F(AIAssistantServiceTest, ConfirmationExpiryUsesCreationClockBoundary)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    ActionManager::instance().registerAction({
        QStringLiteral("set_item_property"), {}, {}, {}, false,
        [](const QJsonObject& args) { return ActionResult{true, args}; }
    });

    qint64 nowMs = 1000;
    AIAssistantService service(&server);
    service.setClockForTesting([&]() { return nowMs; });
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0001")));
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0002")));
    service.setProvider(provider);
    configureValidSettings(service);

    QString pendingId;
    QString failureCode;
    int dispatches = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pendingId = id; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("set_item_property"))
            ++dispatches;
    });

    service.submitUserMessage(QStringLiteral("Approve before expiry."));
    ASSERT_TRUE(TestSupport::spinUntil(
        [&]() { return pendingId == QLatin1String("ai-0001"); }));
    nowMs = 1000 + 300000 - 1;
    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(dispatches, 1);

    pendingId.clear();
    failureCode.clear();
    nowMs = 2000;
    service.submitUserMessage(QStringLiteral("Approve after expiry."));
    ASSERT_TRUE(TestSupport::spinUntil(
        [&]() { return pendingId == QLatin1String("ai-0002"); }));
    nowMs = 2000 + 300000;
    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(dispatches, 1);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
}

TEST_F(AIAssistantServiceTest, ConfirmationApproveDispatchesOnceWithStoredProfileAndRequestId)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    ActionDef action;
    action.id = QStringLiteral("set_item_property");
    action.displayName = QStringLiteral("Set Item Property");
    action.handler = [](const QJsonObject& args) {
        return ActionResult{true, args};
    };
    ActionManager::instance().registerAction(action);

    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0001")));
    service.setProvider(provider);
    configureValidSettings(service);

    QString pendingId;
    QJsonObject pendingCommand;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject& command) {
        pendingId = id;
        pendingCommand = command;
    });
    int dispatches = 0;
    ActionSource source = ActionSource::GUI;
    QString responseId;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource value) {
        if (id == QLatin1String("set_item_property"))
        {
            ++dispatches;
            source = value;
        }
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString& id, const QString&) { responseId = id; });

    service.submitUserMessage(QStringLiteral("Set volume."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !pendingId.isEmpty(); }));
    EXPECT_EQ(pendingId, QStringLiteral("ai-0001"));
    EXPECT_EQ(pendingCommand, setItemVolumeCommand(QStringLiteral("ai-0001")));

    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(dispatches, 1);
    EXPECT_EQ(source, ActionSource::Internal);
    EXPECT_EQ(responseId, QStringLiteral("ai-0001"));

    QString secondCode;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        secondCode = code;
    });
    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(dispatches, 1);
    EXPECT_EQ(secondCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
}

TEST_F(AIAssistantServiceTest, ApprovedWriteFlowsThroughActionManagerUndoStack)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    int value = 1;
    ActionDef action;
    action.id = QStringLiteral("set_item_property");
    action.displayName = QStringLiteral("Set Item Property");
    action.undoable = true;
    action.handler = [&](const QJsonObject& args) {
        const int oldValue = value;
        const int newValue = args.value(QStringLiteral("value")).toInt();
        value = newValue;
        ActionResult result;
        result.success = true;
        result.undoText = QStringLiteral("Set volume");
        result.undoFn = [&value, oldValue]() { value = oldValue; };
        result.redoFn = [&value, newValue]() { value = newValue; };
        return result;
    };
    ActionManager::instance().registerAction(action);

    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(
        setItemVolumeCommand(QStringLiteral("ai-0001"), 7, 5.0));
    service.setProvider(provider);
    configureValidSettings(service);

    QString pendingId;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pendingId = id; });
    service.submitUserMessage(QStringLiteral("Set volume."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !pendingId.isEmpty(); }));
    service.resolveConfirmation(pendingId, true);

    EXPECT_EQ(value, 5);
    ASSERT_TRUE(UndoRedoManager::instance().canUndo());
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(value, 1);
    ASSERT_TRUE(UndoRedoManager::instance().canRedo());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(value, 5);
}
TEST_F(AIAssistantServiceTest, ConfirmationRejectAndAbortExpireWithoutDispatch)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    ActionDef action;
    action.id = QStringLiteral("set_item_property");
    action.handler = [](const QJsonObject& args) { return ActionResult{true, args}; };
    ActionManager::instance().registerAction(action);

    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0001")));
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0002")));
    service.setProvider(provider);
    configureValidSettings(service);

    QString pendingId;
    QString failureCode;
    int dispatches = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pendingId = id; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("set_item_property")) ++dispatches;
    });

    service.submitUserMessage(QStringLiteral("First write."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return pendingId == QLatin1String("ai-0001"); }));
    service.resolveConfirmation(pendingId, false);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_REJECTED"));
    EXPECT_EQ(dispatches, 0);

    pendingId.clear();
    failureCode.clear();
    service.submitUserMessage(QStringLiteral("Second write."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return pendingId == QLatin1String("ai-0002"); }));
    service.abortCurrentTurn();
    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
    EXPECT_EQ(dispatches, 0);
}

TEST_F(AIAssistantServiceTest, NewTurnExpiresPendingAndBusyResolutionIsOneShot)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    ActionDef action;
    action.id = QStringLiteral("set_item_property");
    action.handler = [](const QJsonObject& args) { return ActionResult{true, args}; };
    ActionManager::instance().registerAction(action);

    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0001")));
    provider->enqueueTextResponse(QStringLiteral("active"));
    provider->setAutoDelivery(false);
    service.setProvider(provider);
    configureValidSettings(service);

    QString pendingId;
    QString failureCode;
    int dispatches = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pendingId = id; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("set_item_property")) ++dispatches;
    });

    service.submitUserMessage(QStringLiteral("Write."));
    ASSERT_TRUE(provider->deliverNext());
    ASSERT_EQ(pendingId, QStringLiteral("ai-0001"));

    service.submitUserMessage(QStringLiteral("New turn."));
    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
    EXPECT_EQ(dispatches, 0);
}

TEST_F(AIAssistantServiceTest, NewTurnExpiresPendingConfirmationWithoutDispatch)
{
    IPCServer server(nullptr);
    ActionDef action;
    action.id = QStringLiteral("set_item_property");
    action.handler = [](const QJsonObject& args) { return ActionResult{true, args}; };
    ActionManager::instance().registerAction(action);

    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("ai-0001")));
    provider->setAutoDelivery(false);
    service.setProvider(provider);
    configureValidSettings(service);

    QString pendingId;
    QString failureCode;
    int dispatches = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pendingId = id; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("set_item_property")) ++dispatches;
    });

    service.submitUserMessage(QStringLiteral("Write."));
    ASSERT_TRUE(provider->deliverNext());
    ASSERT_EQ(pendingId, QStringLiteral("ai-0001"));

    provider->enqueueTextResponse(QStringLiteral("new turn"));
    service.submitUserMessage(QStringLiteral("New turn."));
    service.resolveConfirmation(pendingId, true);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
    EXPECT_EQ(dispatches, 0);
}

TEST_F(AIAssistantServiceTest, RejectsMismatchedLegacyCommandAtProviderBoundary)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    ActionDef action;
    action.id = QStringLiteral("set_item_property");
    action.handler = [](const QJsonObject& args) { return ActionResult{true, args}; };
    ActionManager::instance().registerAction(action);

    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueCommandResponse(setItemVolumeCommand(QStringLiteral("wrong-request")));
    service.setProvider(provider);
    configureValidSettings(service);

    QString failureCode;
    bool confirmationShown = false;
    int dispatches = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString&, const QJsonObject&) { confirmationShown = true; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("set_item_property")) ++dispatches;
    });

    service.submitUserMessage(QStringLiteral("Mismatched write."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return failureCode == QLatin1String("INVALID_RESPONSE");
    }));
    EXPECT_EQ(failureCode, QStringLiteral("INVALID_RESPONSE"));
    EXPECT_FALSE(confirmationShown);
    EXPECT_EQ(dispatches, 0);
}

TEST_F(AIAssistantServiceTest, TypedReadReadFinalPreservesToolTranscript)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({toolCall("call-1", "ping" )});
    provider->enqueueToolCallsResponse({toolCall("call-2", "get_version" )});
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);
    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Read twice."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    ASSERT_EQ(provider->submitCount(), 3);
    const auto submitted = provider->submittedRequests();
    ASSERT_EQ(submitted.size(), 3);
    const QJsonArray secondMessages = submitted.at(1).messages;
    ASSERT_GE(secondMessages.size(), 3);
    EXPECT_EQ(secondMessages.at(1).toObject().value(QStringLiteral("role")), QStringLiteral("assistant"));
    EXPECT_EQ(secondMessages.at(2).toObject().value(QStringLiteral("tool_call_id")), QStringLiteral("call-1"));
    const QJsonArray thirdMessages = submitted.at(2).messages;
    ASSERT_GE(thirdMessages.size(), 5);
    EXPECT_EQ(thirdMessages.at(4).toObject().value(QStringLiteral("tool_call_id")), QStringLiteral("call-2"));
}

TEST_F(AIAssistantServiceTest, CompletedHistoryExcludesPriorToolProtocolMessages)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({toolCall("call-1", "ping")});
    provider->enqueueTextResponse(QStringLiteral("first done"));
    provider->enqueueTextResponse(QStringLiteral("second done"));
    service.setProvider(provider);
    configureValidSettings(service);

    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Run a tool."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    service.submitUserMessage(QStringLiteral("Follow up."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 2; }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 3);
    ASSERT_EQ(requests[2].messages.size(), 3);
    EXPECT_EQ(requests[2].messages[0].toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("user"));
    EXPECT_EQ(requests[2].messages[1].toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("assistant"));
    EXPECT_EQ(requests[2].messages[1].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("first done"));
    EXPECT_EQ(requests[2].messages[2].toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("user"));
}

TEST_F(AIAssistantServiceTest, TypedWriteApprovalResumesAndFinalizes)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    registerBareSetItemPropertyAction();
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({setItemVolumeToolCall(QStringLiteral("write-1"))});
    provider->enqueueTextResponse(QStringLiteral("finished"));
    service.setProvider(provider);
    configureValidSettings(service);
    QString pending;
    int responses = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pending = id; });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Write."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !pending.isEmpty(); }));
    EXPECT_EQ(provider->submitCount(), 1);
    service.resolveConfirmation(pending, true);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    EXPECT_EQ(provider->submitCount(), 2);
}

TEST_F(AIAssistantServiceTest, TypedWriteRejectAppendsToolErrorAndResumes)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    registerBareSetItemPropertyAction();
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({setItemVolumeToolCall(QStringLiteral("write-1"))});
    provider->enqueueTextResponse(QStringLiteral("rejected"));
    service.setProvider(provider);
    configureValidSettings(service);
    QString pending;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pending = id; });

    service.submitUserMessage(QStringLiteral("Reject."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !pending.isEmpty(); }));
    service.resolveConfirmation(pending, false);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return provider->submitCount() == 2; }));
    const QJsonArray messages = provider->lastRequest().messages;
    ASSERT_GE(messages.size(), 3);
    const QString content = messages.at(messages.size() - 1).toObject().value(QStringLiteral("content")).toString();
    EXPECT_TRUE(content.contains(QStringLiteral("AI_CONFIRMATION_REJECTED")));
}

TEST_F(AIAssistantServiceTest, TypedWriteExpiryAppendsToolErrorAndResumes)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    registerBareSetItemPropertyAction();
    qint64 now = 1000;
    AIAssistantService service(&server);
    service.setClockForTesting([&]() { return now; });
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({setItemVolumeToolCall(QStringLiteral("write-1"))});
    provider->enqueueTextResponse(QStringLiteral("expired"));
    service.setProvider(provider);
    configureValidSettings(service);
    QString pending;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pending = id; });

    service.submitUserMessage(QStringLiteral("Expire."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !pending.isEmpty(); }));
    now += 300000;
    service.resolveConfirmation(pending, true);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return provider->submitCount() == 2; }));
    const QJsonArray messages = provider->lastRequest().messages;
    ASSERT_GE(messages.size(), 3);
    EXPECT_TRUE(messages.last().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("AI_CONFIRMATION_EXPIRED")));
}

TEST_F(AIAssistantServiceTest, TypedMultipleCallsExecuteInReturnedOrder)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({toolCall("call-1", "ping"), toolCall("call-2", "get_version")});
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);
    QStringList executed;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) { executed.append(id); });
    service.submitUserMessage(QStringLiteral("Two tools."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return provider->submitCount() == 2; }));
    ASSERT_EQ(executed.size(), 2);
    EXPECT_EQ(executed.at(0), QStringLiteral("ping"));
    EXPECT_EQ(executed.at(1), QStringLiteral("get_version"));
}

TEST_F(AIAssistantServiceTest, TypedEightCallsExecuteInReturnedOrder)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    ActionManager::instance().registerAction({QStringLiteral("get_item_info"), {}, {}, {}, false,
        [](const QJsonObject& args) { return ActionResult{true, args}; }});
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    QList<ILLMProvider::ToolCall> calls;
    for (int i = 0; i < 8; ++i)
        calls.append(toolCall(QStringLiteral("call-%1").arg(i + 1),
                              QStringLiteral("get_item_info"),
                              QJsonObject{{QStringLiteral("object_id"), i + 1}}));
    provider->enqueueToolCallsResponse(calls);
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);
    QList<int> executedObjectIds;
    int responses = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString&, const QJsonObject& args, const QJsonObject&, ActionSource) {
        executedObjectIds.append(args.value(QStringLiteral("object_id")).toInt());
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Eight tools."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    ASSERT_EQ(executedObjectIds.size(), 8);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(executedObjectIds.at(i), i + 1);
    EXPECT_EQ(provider->submitCount(), 2);
}

TEST_F(AIAssistantServiceTest, NineCallsInOneResponseFailInvalidResponse)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    QList<ILLMProvider::ToolCall> calls;
    for (int i = 0; i < 9; ++i)
        calls.append(toolCall(QStringLiteral("call-%1").arg(i + 1), QStringLiteral("ping")));
    provider->enqueueToolCallsResponse(calls);
    service.setProvider(provider);
    configureValidSettings(service);
    int executions = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString&, const QJsonObject&, const QJsonObject&, ActionSource) { ++executions; });
    QString code;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& value, const QString&) { code = value; });
    service.submitUserMessage(QStringLiteral("Nine tools."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !code.isEmpty(); }));
    EXPECT_EQ(code, QStringLiteral("INVALID_RESPONSE"));
    EXPECT_EQ(executions, 0);
    EXPECT_EQ(provider->submitCount(), 1);
}

TEST_F(AIAssistantServiceTest, ConfirmationWaitExtendsWholeTurnDeadline)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    registerBareSetItemPropertyAction();
    qint64 now = 1000;
    AIAssistantService service(&server);
    service.setClockForTesting([&]() { return now; });
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({setItemVolumeToolCall(QStringLiteral("write-1"))});
    provider->enqueueTextResponse(QStringLiteral("after wait"));
    service.setProvider(provider);
    configureValidSettings(service);
    QString pending;
    int responses = 0;
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString& id, const QJsonObject&) { pending = id; });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });
    service.submitUserMessage(QStringLiteral("Wait."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !pending.isEmpty(); }));
    now += 120001;
    service.resolveConfirmation(pending, true);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
}

TEST_F(AIAssistantServiceTest, AbortSuppressesPostToolStaleCompletion)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueToolCallsResponse({toolCall("call-1", "ping")});
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

TEST_F(AIAssistantServiceTest, ToolResultAggregateBudgetFailsSafely)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    ActionManager::instance().registerAction({QStringLiteral("get_item_info"), {}, {}, {}, false,
        [](const QJsonObject&) {
            return ActionResult{true, QJsonObject{{"payload", QString(15800, QLatin1Char('x'))}}};
        }});
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    for (int round = 0; round < 2; ++round)
    {
        QList<ILLMProvider::ToolCall> calls;
        for (int i = 0; i < 4; ++i)
        {
            const int index = round * 4 + i;
            calls.append(toolCall(QStringLiteral("call-%1").arg(index),
                                  QStringLiteral("get_item_info"),
                                  QJsonObject{{QStringLiteral("object_id"), index + 1}}));
        }
        provider->enqueueToolCallsResponse(calls);
    }
    service.setProvider(provider);
    configureValidSettings(service);
    QString code;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& value, const QString&) { code = value; });
    service.submitUserMessage(QStringLiteral("Large result."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !code.isEmpty(); }));
    EXPECT_EQ(code, QStringLiteral("AI_LOOP_LIMIT"));
}

TEST_F(AIAssistantServiceTest, ThirdIdenticalTypedReadHitsLoopLimit)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({toolCall("call-1", "ping")});
    provider->enqueueToolCallsResponse({toolCall("call-2", "ping")});
    provider->enqueueToolCallsResponse({toolCall("call-3", "ping")});
    service.setProvider(provider);
    configureValidSettings(service);
    QString code;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& value, const QString&) { code = value; });
    service.submitUserMessage(QStringLiteral("Repeat."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !code.isEmpty(); }));
    EXPECT_EQ(code, QStringLiteral("AI_LOOP_LIMIT"));
    EXPECT_EQ(provider->submitCount(), 3);
}

TEST_F(AIAssistantServiceTest, ProgressExhaustionForcesSummaryRoundInsteadOfBareFailure)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    ActionManager::instance().registerAction({QStringLiteral("get_item_info"), {}, {}, {}, false,
        [](const QJsonObject& args) { return ActionResult{true, args}; }});
    AIAssistantService service(&server);
    // Inject a small work budget so the exhaustion path is exercised without
    // running the full production limit of rounds.
    constexpr int kTestRoundLimit = 3;
    service.setExecutionLimitsForTesting(kTestRoundLimit, kTestRoundLimit);
    auto* provider = new MockLLMProvider;
    // One tool call per round with distinct arguments avoids the repeated-read
    // safety limit, so the turn exhausts its model-round budget instead.
    for (int i = 0; i < kTestRoundLimit; ++i)
        provider->enqueueToolCallsResponse({toolCall(QStringLiteral("call-%1").arg(i),
            QStringLiteral("get_item_info"),
            QJsonObject{{QStringLiteral("object_id"), i + 1}})});
    provider->enqueueTextResponse(QStringLiteral("Summary after the step limit."));
    service.setProvider(provider);
    configureValidSettings(service);

    QString responseText;
    int failures = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString& text) { responseText = text; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString&, const QString&) { ++failures; });

    service.submitUserMessage(QStringLiteral("Keep calling tools."));
    ASSERT_TRUE(TestSupport::spinUntil(
        [&]() { return !responseText.isEmpty() || failures > 0; }));
    EXPECT_EQ(failures, 0);
    EXPECT_EQ(responseText, QStringLiteral("Summary after the step limit."));
    // The closing summary round is submitted with tools disabled.
    EXPECT_TRUE(provider->lastRequest().tools.isEmpty());
}

TEST_F(AIAssistantServiceTest, ForcedSummaryContextOverflowGuidesUserToAIAgentSettings)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    ActionManager::instance().registerAction({
        QStringLiteral("get_item_info"), {}, {}, {}, false,
        [](const QJsonObject&) {
            return ActionResult{
                true,
                QJsonObject{{QStringLiteral("payload"),
                             QString(15800, QLatin1Char('x'))}}
            };
        }
    });

    AIAssistantService service(&server);
    service.setExecutionLimitsForTesting(1, 1);
    service.setUnrestrictedMode(false);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("small-context-model"));
        if (key == QLatin1String("permission_mode"))
            return QVariant(QStringLiteral("read_only"));
        if (key == QLatin1String("context_window_tokens"))
            return QVariant(16384);
        return defaultValue;
    });

    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({
        toolCall(QStringLiteral("call-1"), QStringLiteral("get_item_info"),
                 QJsonObject{{QStringLiteral("object_id"), 1}})
    });
    service.setProvider(provider);

    QString failureCode;
    QString failureMessage;
    QObject::connect(
        &service, &AIAssistantService::failed, &service,
        [&](const QString&, const QString& code, const QString& message) {
        failureCode = code;
        failureMessage = message;
    });

    service.submitUserMessage(QStringLiteral("Read the item and summarize it."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !failureCode.isEmpty(); }));
    EXPECT_EQ(failureCode, QStringLiteral("CONTEXT_LIMIT_EXCEEDED"));
    EXPECT_TRUE(failureMessage.contains(QStringLiteral("File > Preferences > AI Agent")));
    EXPECT_EQ(provider->submitCount(), 1);
}

TEST_F(AIAssistantServiceTest, WholeTurnTimeoutForcedSummaryKeepsBoundedNonZeroTimeout)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    qint64 now = 1000;
    AIAssistantService service(&server);
    service.setClockForTesting([&]() { return now; });
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueToolCallsResponse({toolCall(QStringLiteral("call-1"),
                                                 QStringLiteral("ping"))});
    provider->enqueueTextResponse(QStringLiteral("Summary after the deadline."));
    service.setProvider(provider);
    configureValidSettings(service);

    QString responseText;
    int failures = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString& text) { responseText = text; });
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString&, const QString&) { ++failures; });

    service.submitUserMessage(QStringLiteral("Start a turn."));
    ASSERT_EQ(provider->submitCount(), 1);

    // Advance past the whole-turn deadline (kWholeTurnTimeoutMs = 120000), then
    // deliver the tool call so the queue's deadline check forces a summary round.
    now = 1000 + 120000 + 1;
    ASSERT_TRUE(provider->deliverNext());
    ASSERT_EQ(provider->submitCount(), 2);

    // The summary round must keep a bounded, non-zero window even though the turn
    // deadline is already spent; it must never be clamped to the exhausted
    // remaining time (which would leave the model no time to reply).
    const int summaryTimeout = provider->lastRequest().timeoutMs;
    EXPECT_GT(summaryTimeout, 0);
    EXPECT_LE(summaryTimeout, 15000);
    EXPECT_TRUE(provider->lastRequest().tools.isEmpty());

    ASSERT_TRUE(provider->deliverNext());
    EXPECT_EQ(failures, 0);
    EXPECT_EQ(responseText, QStringLiteral("Summary after the deadline."));
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


TEST_F(AIAssistantServiceTest, DoubleSubmitDoesNotReachProviderTwice)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("pending"));
    service.setProvider(provider);
    configureValidSettings(service);

    QString failureCode;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        failureCode = code;
    });

    service.submitUserMessage(QStringLiteral("First."));
    service.submitUserMessage(QStringLiteral("Second."));
    EXPECT_EQ(provider->submitCount(), 1);
    EXPECT_EQ(failureCode, QStringLiteral("INVALID_ARGUMENTS"));
}
