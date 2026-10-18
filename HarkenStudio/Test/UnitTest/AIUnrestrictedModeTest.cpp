#include "AI/AIAssistantService.h"
#include "ActionManager.h"
#include "IPC/IPCServer.h"
#include "ItemTypeCatalog.h"
#include "Support/AITestSupport.h"
#include "Support/MockLLMProvider.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>

using namespace VNX;

namespace {

void configureValidSettings(AIAssistantService& service)
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

void registerTestWriteAction(const QJsonObject& paramSchema = {})
{
    ActionDef action;
    action.id = QStringLiteral("test_write_command");
    action.displayName = QStringLiteral("Test Write Command");
    action.category = QStringLiteral("test");
    action.paramSchema = paramSchema;
    action.undoable = false;
    action.handler = [](const QJsonObject& args) { return ActionResult{true, args}; };
    ActionManager::instance().registerAction(action);
}

ILLMProvider::ToolCall dispatchIpcCall(const QString& id, const QString& command,
                                       const QJsonObject& args = {})
{
    return {id, QStringLiteral("dispatch_ipc"),
            QJsonObject{{QStringLiteral("command"), command},
                        {QStringLiteral("args"), args}}};
}

ILLMProvider::ToolCall describeCall(const QString& id, const QString& name)
{
    return {id, QStringLiteral("describe_command"),
            QJsonObject{{QStringLiteral("name"), name}}};
}

QJsonObject lastToolMessage(const QJsonArray& messages)
{
    for (int i = messages.size() - 1; i >= 0; --i)
    {
        const QJsonObject message = messages.at(i).toObject();
        if (message.value(QStringLiteral("role")).toString() == QLatin1String("tool"))
            return QJsonDocument::fromJson(
                message.value(QStringLiteral("content")).toString().toUtf8()).object();
    }
    return {};
}

QJsonObject lastToolMessageResult(const MockLLMProvider& provider)
{
    return lastToolMessage(provider.lastRequest().messages);
}

class AIUnrestrictedModeTest : public ::testing::Test
{
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

TEST_F(AIUnrestrictedModeTest, UnrestrictedModeDefaultsOn)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    EXPECT_TRUE(service.unrestrictedMode());
    service.setUnrestrictedMode(false);
    EXPECT_FALSE(service.unrestrictedMode());
}

TEST_F(AIUnrestrictedModeTest, UnrestrictedTurnOffersGenericToolsAndCommandVocabulary)
{
    IPCServer server(nullptr);
    registerTestWriteAction();
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("pending"));
    service.setProvider(provider);
    configureValidSettings(service);

    service.submitUserMessage(QStringLiteral("List your tools."));
    ASSERT_EQ(provider->submitCount(), 1);
    const auto tools = provider->lastRequest().tools;
    ASSERT_EQ(tools.size(), 6);
    EXPECT_EQ(tools.at(0).cmd, QStringLiteral("dispatch_ipc"));
    EXPECT_EQ(tools.at(1).cmd, QStringLiteral("describe_command"));
    EXPECT_EQ(tools.at(2).cmd, QStringLiteral("memory_list"));
    EXPECT_EQ(tools.at(3).cmd, QStringLiteral("memory_upsert"));
    EXPECT_EQ(tools.at(4).cmd, QStringLiteral("memory_remove"));
    EXPECT_EQ(tools.at(5).cmd, QStringLiteral("memory_clear"));
    for (const AICommandSpec& tool : tools)
        EXPECT_FALSE(tool.argumentSchema.isEmpty());

    const QString prompt = provider->lastRequest().systemPrompt;
    EXPECT_TRUE(prompt.contains(QStringLiteral("dispatch_ipc")));
    EXPECT_TRUE(prompt.contains(QStringLiteral("describe_command")));
    // The prompt conveys the command surface as a fixed category map rather than
    // a flat list of every dispatchable name, so assert the map is present
    // instead of individual command names.
    EXPECT_TRUE(prompt.contains(QStringLiteral("DOMAIN MODEL")));
    for (const ProjectItem::Type type : ItemTypeCatalog::allTypes()) {
        const QString name = ItemTypeCatalog::canonicalName(type);
        if (ItemTypeCatalog::isUserFacingType(type))
            EXPECT_TRUE(prompt.contains(name));
        else
            EXPECT_FALSE(prompt.contains(name));
    }
    EXPECT_FALSE(prompt.contains(QStringLiteral("Container\" here means Layer")));
    EXPECT_FALSE(prompt.contains(QStringLiteral("a VCA is")));
    EXPECT_TRUE(prompt.contains(QStringLiteral("COMMAND MAP")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "pack metadata: set_action_pack_targeting")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "container (content selection and children")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "editor (derived Blueprint routing and overrides")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "Prefer create_item with type Action for canonical Action creation")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "create_action remains a permission-gated convenience command")));
    EXPECT_FALSE(prompt.contains(QStringLiteral(
        "set_item_property, create_pack)")));
    EXPECT_FALSE(prompt.contains(QStringLiteral("container (Layer children")));
    EXPECT_FALSE(prompt.contains(QStringLiteral("node graph & action creation")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "Never infer or save a memory from an ordinary conversation.")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "RESPONSE LANGUAGE IS MANDATORY")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "Object names, file paths, command names, code, and isolated identifiers "
        "do not determine the response language.")));
    EXPECT_TRUE(prompt.contains(QStringLiteral("APPLICATION LOCALE:")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "Treat tool results as evidence to explain, not as prose to copy.")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "synthesize the relevant structure, relationships, scope, and count")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "exactly one useful, context-aware next-step question")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "do not replace the question with a merely declarative suggestion")));
    EXPECT_TRUE(prompt.contains(QStringLiteral(
        "never append a generic")));

    // The union of both command sources (legacy handlers and registered actions)
    // is still assembled by the server and stays queryable; verify it at that
    // source rather than in the prompt text.
    const QStringList commandVocabulary = server.allCommandNames();
    EXPECT_TRUE(commandVocabulary.contains(QStringLiteral("ping")));
    EXPECT_TRUE(commandVocabulary.contains(QStringLiteral("test_write_command")));
}

TEST_F(AIUnrestrictedModeTest, DispatchIpcExecutesNonCatalogWriteWithoutGates)
{
    IPCServer server(nullptr);
    registerTestWriteAction();
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({dispatchIpcCall(QStringLiteral("call-1"),
        QStringLiteral("test_write_command"),
        QJsonObject{{QStringLiteral("value"), 5}})});
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);

    int dispatches = 0;
    ActionSource observedSource = ActionSource::GUI;
    bool confirmationShown = false;
    int responses = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject& args, const QJsonObject&,
                         ActionSource source) {
        if (id == QLatin1String("test_write_command"))
        {
            ++dispatches;
            observedSource = source;
            EXPECT_EQ(args.value(QStringLiteral("value")).toInt(), 5);
        }
    });
    QObject::connect(&service, &AIAssistantService::confirmationRequired, &service,
                     [&](const QString&, const QJsonObject&) { confirmationShown = true; });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Write without gates."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    EXPECT_EQ(dispatches, 1);
    EXPECT_EQ(observedSource, ActionSource::Internal);
    EXPECT_FALSE(confirmationShown);
}

TEST_F(AIUnrestrictedModeTest, DirectCommandNameCallDispatchesInUnrestrictedMode)
{
    IPCServer server(nullptr);
    registerTestWriteAction();
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({{QStringLiteral("call-1"),
        QStringLiteral("test_write_command"),
        QJsonObject{{QStringLiteral("value"), 7}}}});
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);

    int dispatches = 0;
    int responses = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("test_write_command")) ++dispatches;
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Call the command directly."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    EXPECT_EQ(dispatches, 1);
}

TEST_F(AIUnrestrictedModeTest, RestrictedModeStillDeniesNonCatalogCommand)
{
    IPCServer server(nullptr);
    registerTestWriteAction();
    AIAssistantService service(&server);
    service.setUnrestrictedMode(false);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({dispatchIpcCall(QStringLiteral("call-1"),
        QStringLiteral("test_write_command"))});
    provider->enqueueTextResponse(QStringLiteral("denied"));
    service.setProvider(provider);
    configureValidSettings(service);

    int dispatches = 0;
    int responses = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &service,
                     [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
        if (id == QLatin1String("test_write_command")) ++dispatches;
    });
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Try the generic tool while restricted."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));
    EXPECT_EQ(dispatches, 0);
    const QJsonObject toolResult = lastToolMessageResult(*provider);
    EXPECT_EQ(toolResult.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(toolResult.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_ARGUMENTS"));
}

TEST_F(AIUnrestrictedModeTest, DescribeCommandReturnsCatalogSchema)
{
    IPCServer server(nullptr);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({describeCall(QStringLiteral("call-1"),
                                                     QStringLiteral("set_item_property"))});
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);

    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Describe a catalog command."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));

    const QJsonObject toolResult = lastToolMessageResult(*provider);
    EXPECT_EQ(toolResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject described = toolResult.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(described.value(QStringLiteral("source")).toString(), QStringLiteral("catalog"));
    EXPECT_EQ(described.value(QStringLiteral("command")).toString(),
              QStringLiteral("set_item_property"));
    EXPECT_FALSE(described.value(QStringLiteral("parameters")).toObject().isEmpty());
    EXPECT_FALSE(described.value(QStringLiteral("description")).toString().isEmpty());
}

TEST_F(AIUnrestrictedModeTest, DescribeCommandFallsBackToActionInfoAndUnknownNote)
{
    IPCServer server(nullptr);
    const QJsonObject paramSchema{{QStringLiteral("value"), QStringLiteral("number")}};
    registerTestWriteAction(paramSchema);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({describeCall(QStringLiteral("call-1"),
                                                     QStringLiteral("test_write_command"))});
    provider->enqueueToolCallsResponse({describeCall(QStringLiteral("call-2"),
                                                     QStringLiteral("no_such_command"))});
    provider->enqueueTextResponse(QStringLiteral("done"));
    service.setProvider(provider);
    configureValidSettings(service);

    int responses = 0;
    QObject::connect(&service, &AIAssistantService::responseReady, &service,
                     [&](const QString&, const QString&) { ++responses; });

    service.submitUserMessage(QStringLiteral("Describe fallback sources."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses == 1; }));

    const auto submitted = provider->submittedRequests();
    ASSERT_EQ(submitted.size(), 3);
    const QJsonObject actionResult = lastToolMessage(submitted.at(1).messages);
    const QJsonObject actionDescribed = actionResult.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(actionDescribed.value(QStringLiteral("source")).toString(),
              QStringLiteral("action"));
    EXPECT_EQ(actionDescribed.value(QStringLiteral("parameters")).toObject(), paramSchema);

    const QJsonObject unknownResult = lastToolMessageResult(*provider);
    const QJsonObject unknownDescribed = unknownResult.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(unknownDescribed.value(QStringLiteral("source")).toString(),
              QStringLiteral("unknown"));
    EXPECT_FALSE(unknownDescribed.value(QStringLiteral("note")).toString().isEmpty());
}

TEST_F(AIUnrestrictedModeTest, RepeatedIdenticalReadsThroughDispatchIpcHitLoopLimit)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);
    AIAssistantService service(&server);
    auto* provider = new MockLLMProvider;
    provider->enqueueToolCallsResponse({dispatchIpcCall(QStringLiteral("call-1"),
                                                        QStringLiteral("ping"))});
    provider->enqueueToolCallsResponse({dispatchIpcCall(QStringLiteral("call-2"),
                                                        QStringLiteral("ping"))});
    provider->enqueueToolCallsResponse({dispatchIpcCall(QStringLiteral("call-3"),
                                                        QStringLiteral("ping"))});
    service.setProvider(provider);
    configureValidSettings(service);

    QString code;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& value, const QString&) { code = value; });

    service.submitUserMessage(QStringLiteral("Repeat the same read."));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return !code.isEmpty(); }));
    EXPECT_EQ(code, QStringLiteral("AI_LOOP_LIMIT"));
    EXPECT_EQ(provider->submitCount(), 3);
}

TEST_F(AIUnrestrictedModeTest, UnrestrictedContextBypassesPolicyGatesAtDispatchLevel)
{
    IPCServer server(nullptr);
    registerTestWriteAction();

    IPCServer::IPCDispatchContext context;
    context.source = ActionSource::Internal;
    context.origin = IPCServer::DispatchOrigin::AI;
    context.requestId = QStringLiteral("ai-test");
    context.permissionProfile = QStringLiteral("ask");
    context.toolProfile = QStringLiteral("authoring");

    context.unrestricted = false;
    const QJsonObject denied = server.dispatchCommand(
        QStringLiteral("test_write_command"), QJsonObject{}, context);
    EXPECT_EQ(denied.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(denied.value(QStringLiteral("code")).toString(),
              QStringLiteral("AI_COMMAND_UNAVAILABLE"));

    context.unrestricted = true;
    const QJsonObject allowed = server.dispatchCommand(
        QStringLiteral("test_write_command"), QJsonObject{}, context);
    EXPECT_EQ(allowed.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
}
