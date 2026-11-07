#include "AI/AICommandCatalog.h"
#include "IPC/IPCServer.h"
#include "EditorCommandRegistration.h"
#include "CommandArgumentValidator.h"
#include "AI/AICommandValidator.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QJsonArray>

using namespace VNX;

TEST(AICommandCatalogTest, CatalogEntriesHaveUniqueResolvableIdentities)
{
    QSet<QString> names;
    for (const auto& entry : AICommandCatalog::all())
    {
        EXPECT_FALSE(names.contains(entry.cmd)) << entry.cmd.toStdString();
        names.insert(entry.cmd);
        EXPECT_EQ(AICommandCatalog::find(entry.cmd), &entry);
    }
}

TEST(AICommandCatalogTest, EnabledCommandsResolveTheirDeclaredHandlers)
{
    TestSupport::ensureApp();
    IPCServer server(nullptr);
    const QStringList commandNames = server.commandNames();
    const QSet<QString> handlerNames(commandNames.cbegin(), commandNames.cend());
    for (const auto& entry : AICommandCatalog::all())
    {
        if (entry.availability != AICommandSpec::Availability::Enabled) continue;
        if (entry.source == AICommandSpec::Source::CommandHandler)
            EXPECT_TRUE(handlerNames.contains(entry.cmd)) << entry.cmd.toStdString();
    }
}

TEST(AICommandCatalogTest, HostRegistriesContainOnlyTheirOwnCommands)
{
    TestSupport::ensureApp();
    auto& manager = ActionManager::instance();
    manager.clearAllForTesting();
    IPCServer desktop(nullptr);
    registerEditorCommands({nullptr, nullptr, &desktop});
    const auto desktopNames = desktop.allCommandNames();
    QMap<QString, QJsonObject> sharedDescriptions;
    for (const auto& name : desktopNames) {
        const auto description = describeEditorCommand(name, &desktop);
        EXPECT_TRUE(description.value("registered").toBool()) << name.toStdString();
        const auto schema = description.value("inputSchema").toObject();
        EXPECT_EQ(schema.value("type"), "object") << name.toStdString();
        EXPECT_EQ(schema.value("additionalProperties"), false) << name.toStdString();
        EXPECT_EQ(description.value("schemaSource"), "declared") << name.toStdString();
        EXPECT_TRUE(commandSchemaError(schema).isEmpty())
            << name.toStdString() << ": " << commandSchemaError(schema).toStdString();
        EXPECT_EQ(desktop.dispatchCommand(name, {{"__unexpected_contract_argument__", true}}).value("code"),
                  "INVALID_ARGUMENTS") << name.toStdString();
        if (description.value("scope") == "shared")
            sharedDescriptions.insert(name, description);
    }
    RecordProperty("desktop_command_count", desktopNames.size());
    QSet<QString> desktopOnly;
    for (const auto& name : desktopNames)
        if (desktop.commandRequiresDesktop(name)) desktopOnly.insert(name);
    EXPECT_FALSE(desktopOnly.isEmpty());
    RecordProperty("desktop_only_command_count", desktopOnly.size());

    manager.clearAllForTesting();
    IPCServer cli(nullptr, nullptr, CommandHost::Cli);
    registerEditorCommands({nullptr, nullptr, &cli});
    RecordProperty("cli_command_count", cli.allCommandNames().size());
    EXPECT_EQ(cli.allCommandNames(), sharedDescriptions.keys());
    for (const auto& name : cli.allCommandNames()) {
        EXPECT_FALSE(cli.commandRequiresDesktop(name)) << name.toStdString();
        EXPECT_TRUE(desktopNames.contains(name)) << name.toStdString();
        EXPECT_TRUE(manager.hasAction(name) || cli.hasCommandHandler(name));
        EXPECT_EQ(describeEditorCommand(name, &cli), sharedDescriptions.value(name))
            << name.toStdString();
        EXPECT_EQ(cli.dispatchCommand("get_command_schema", {{"command", name}}).value("result"),
                  sharedDescriptions.value(name)) << name.toStdString();
    }
    for (const auto& name : desktopOnly) {
        EXPECT_FALSE(manager.hasAction(name)) << name.toStdString();
        EXPECT_FALSE(cli.hasCommandHandler(name)) << name.toStdString();
        EXPECT_EQ(cli.dispatchCommand(name, {}).value("code"), "UNKNOWN_COMMAND")
            << name.toStdString();
    }
    manager.clearAllForTesting();
}

TEST(CommandArgumentValidatorTest, NestedContractsReportTheFailingArgumentPath)
{
    const QJsonObject schema{
        {"type", "object"}, {"properties", QJsonObject{
            {"rows", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 2},
                {"uniqueItems", true}, {"items", QJsonObject{
                    {"type", "object"}, {"properties", QJsonObject{
                        {"gain", QJsonObject{{"type", "number"}, {"minimum", -96}, {"maximum", 12}}},
                        {"name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 3}}}
                    }}, {"required", QJsonArray{"gain", "name"}}, {"additionalProperties", false}
                }}}}
        }}, {"required", QJsonArray{"rows"}}, {"additionalProperties", false}
    };
    const QJsonObject row{{"gain", -6}, {"name", "one"}};
    QString error;
    EXPECT_TRUE(validateCommandArguments({{"rows", QJsonArray{row}}}, schema, &error));
    EXPECT_TRUE(error.isEmpty());
    for (const auto& invalid : {
        QJsonObject{{"gain", "-6"}, {"name", "one"}},
        QJsonObject{{"gain", 13}, {"name", "one"}},
        QJsonObject{{"gain", -97}, {"name", "one"}}}) {
        EXPECT_FALSE(validateCommandArguments({{"rows", QJsonArray{invalid}}}, schema, &error));
        EXPECT_TRUE(error.startsWith("args.rows[0].gain:")) << error.toStdString();
    }
    EXPECT_FALSE(validateCommandArguments({{"rows", QJsonArray{row, row}}}, schema, &error));
    EXPECT_TRUE(error.startsWith("args.rows[1]:")) << error.toStdString();
    EXPECT_FALSE(validateCommandArguments({{"rows", QJsonArray{}}}, schema, &error));
    EXPECT_FALSE(validateCommandArguments({{"rows", QJsonArray{QJsonObject{{"gain", 1}}}}}, schema, &error));
    EXPECT_TRUE(error.startsWith("args.rows[0].name:")) << error.toStdString();
    EXPECT_FALSE(validateCommandArguments({{"rows", QJsonArray{QJsonObject{{"gain", 1}, {"name", "long"}}}}}, schema, &error));
}

TEST(CommandArgumentValidatorTest, AlternativesValidateValuesAndPreserveWideIdentifiers)
{
    const QJsonObject identifier{{"anyOf", QJsonArray{
        QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991.0}},
        QJsonObject{{"type", "string"}, {"format", "uint64"}}
    }}};
    const QJsonObject schema{{"type", "object"}, {"properties", QJsonObject{
        {"id", identifier}, {"optional", QJsonObject{{"type", QJsonArray{"null", "boolean"}}}}
    }}, {"required", QJsonArray{"id"}}, {"additionalProperties", false}};
    QString error;
    for (const auto& id : {QJsonValue(42), QJsonValue("18446744073709551615")}) {
        EXPECT_TRUE(validateCommandArguments({{"id", id}, {"optional", QJsonValue()}}, schema, &error));
        EXPECT_TRUE(AICommandValidator::validateArguments({{"id", id}}, schema, &error));
    }
    for (const auto& id : {QJsonValue(-1), QJsonValue(1.5), QJsonValue(9007199254740992.0),
                           QJsonValue("18446744073709551616"), QJsonValue("-1"), QJsonValue(true)}) {
        EXPECT_FALSE(validateCommandArguments({{"id", id}}, schema, &error));
        EXPECT_TRUE(error.contains("args.id")) << error.toStdString();
        QString aiError;
        EXPECT_FALSE(AICommandValidator::validateArguments({{"id", id}}, schema, &aiError));
        EXPECT_EQ(aiError, error);
    }
    const QJsonObject exclusive{{"oneOf", QJsonArray{
        QJsonObject{{"required", QJsonArray{"id"}}},
        QJsonObject{{"required", QJsonArray{"name"}}}
    }}};
    EXPECT_TRUE(validateCommandArguments({{"id", 1}}, exclusive, &error));
    EXPECT_FALSE(validateCommandArguments({{"id", 1}, {"name", "both"}}, exclusive, &error));
    EXPECT_FALSE(validateCommandArguments({}, exclusive, &error));
}

TEST(CommandArgumentValidatorTest, UnsupportedSchemaConstraintsCannotBeSilentlyIgnored)
{
    const QJsonObject schema{{"type", "object"}, {"properties", QJsonObject{
        {"unused", QJsonObject{{"type", "number"}, {"multipleOf", 3}}}
    }}};
    QString error;
    EXPECT_FALSE(validateCommandArguments({}, schema, &error));
    EXPECT_TRUE(error.contains("schema.properties.unused.multipleOf")) << error.toStdString();
    EXPECT_FALSE(commandSchemaError({{"type", "object"}, {"items", "invalid"}}).isEmpty());
}

TEST(CommandArgumentValidatorTest, InvalidExternalActionsNeverReachTheHandler)
{
    TestSupport::ensureApp();
    auto& manager = ActionManager::instance();
    manager.clearAllForTesting();
    bool called = false;
    ActionDef action;
    action.id = "contract_test_action";
    action.paramSchema = {{"type", "object"}, {"properties", QJsonObject{
        {"enabled", QJsonObject{{"type", "boolean"}}}
    }}, {"required", QJsonArray{"enabled"}}, {"additionalProperties", false}};
    action.handler = [&](const QJsonObject&) { called = true; return ActionResult{true}; };
    manager.registerAction(action);
    for (const auto source : {ActionSource::IPC, ActionSource::Remote}) {
        EXPECT_EQ(manager.execute(action.id, {{"enabled", "false"}}, source).errorCode, "INVALID_ARGUMENTS");
        EXPECT_FALSE(called);
    }
    EXPECT_TRUE(manager.execute(action.id, {{"enabled", false}}, ActionSource::IPC).success);
    EXPECT_TRUE(called);
    manager.clearAllForTesting();
}

TEST(CommandArgumentValidatorTest, BatchPreflightAndFailureCountsReflectActualExecution)
{
    TestSupport::ensureApp();
    auto& manager = ActionManager::instance();
    manager.clearAllForTesting();
    IPCServer server(nullptr);
    int executions = 0;
    ActionDef action;
    action.id = "contract_batch_step";
    action.paramSchema = {{"type", "object"}, {"properties", QJsonObject{
        {"fail", QJsonObject{{"type", "boolean"}}}
    }}, {"additionalProperties", false}};
    action.handler = [&](const QJsonObject& args) {
        ++executions;
        return args.value("fail").toBool() ? ActionResult{false, {}, "DOMAIN_ERROR", "Requested failure"} : ActionResult{true};
    };
    manager.registerAction(action);
    const QJsonObject step{{"cmd", action.id}};
    auto invalid = step;
    invalid.insert("args", QJsonObject{{"fail", "false"}});
    const auto preflight = server.dispatchCommand("batch", {{"commands", QJsonArray{step, invalid}}});
    EXPECT_EQ(preflight.value("code"), "INVALID_ARGUMENTS");
    EXPECT_EQ(executions, 0);
    const QJsonObject failure{{"cmd", action.id}, {"args", QJsonObject{{"fail", true}}}};
    for (const bool stop : {true, false}) {
        executions = 0;
        const auto response = server.dispatchCommand("batch", {
            {"commands", QJsonArray{step, failure, step}}, {"stop_on_error", stop}});
        EXPECT_EQ(response.value("status"), "error");
        EXPECT_EQ(response.value("code"), "BATCH_FAILED");
        const auto result = response.value("result").toObject();
        EXPECT_EQ(result.value("failed"), 1);
        EXPECT_EQ(result.value("succeeded"), stop ? 1 : 2);
        EXPECT_EQ(result.value("skipped"), stop ? 1 : 0);
        EXPECT_EQ(executions, stop ? 2 : 3);
    }
    manager.clearAllForTesting();
}

TEST(AICommandCatalogTest, QueryAdaptersPreserveStructuredFailures)
{
    TestSupport::ensureApp();
    auto& manager = ActionManager::instance();
    manager.clearAllForTesting();
    IPCServer server(nullptr, nullptr, CommandHost::Cli);
    registerEditorCommands({nullptr, nullptr, &server});
    QMap<QString, QJsonObject> requests{{"get_bus_properties", {{"bus_id", 1}}}};
    for (const auto& name : server.allCommandNames()) {
        const auto schema = describeEditorCommand(name, &server).value("inputSchema").toObject();
        if (server.isReadOnlyCommand(name) && server.hasCommandHandler(name)
            && validateCommandArguments({}, schema)) requests.insert(name, {});
    }
    manager.clearAllForTesting();
    QMap<QString, QJsonObject> directResponses;
    for (auto it = requests.begin(); it != requests.end(); ++it)
        directResponses.insert(it.key(), server.dispatchCommand(it.key(), it.value()));
    registerEditorCommands({nullptr, nullptr, &server});
    for (auto it = requests.begin(); it != requests.end(); ++it) {
        const auto direct = directResponses.value(it.key());
        if (direct.value("status") != "error") continue;
        const auto wrapped = server.dispatchCommand(it.key(), it.value());
        EXPECT_EQ(wrapped.value("status"), "error") << it.key().toStdString();
        EXPECT_FALSE(wrapped.value("code").toString().isEmpty()) << it.key().toStdString();
        EXPECT_FALSE(wrapped.value("message").toString().isEmpty()) << it.key().toStdString();
        if (!direct.value("result").toObject().isEmpty())
            EXPECT_EQ(wrapped.value("result"), direct.value("result")) << it.key().toStdString();
    }
    manager.clearAllForTesting();
}

TEST(AICommandCatalogTest, DiscoveryRemainsAvailableWithoutAnOpenProject)
{
    TestSupport::ensureApp();
    auto& manager = ActionManager::instance();
    for (const auto host : {CommandHost::Desktop, CommandHost::Cli}) {
        manager.clearAllForTesting();
        IPCServer server(nullptr, nullptr, host);
        registerEditorCommands({nullptr, nullptr, &server});
        server.setAppState(IPCServer::AppState::ProjectPicker);
        const auto listed = server.dispatchCommand("list_commands", {});
        ASSERT_EQ(listed.value("status"), "ok");
        const auto entries = listed.value("result").toObject().value("commands").toArray();
        EXPECT_EQ(entries.size(), server.allCommandNames().size());
        for (const auto& entry : entries) {
            const auto name = entry.toObject().value("command");
            const auto described = server.dispatchCommand("get_command_schema", {{"command", name}});
            EXPECT_EQ(described.value("status"), "ok");
            EXPECT_EQ(described.value("result"), entry);
        }
        EXPECT_EQ(server.dispatchCommand("not_a_registered_command", {}).value("code"), "UNKNOWN_COMMAND");
        EXPECT_EQ(server.dispatchCommand("get_command_schema", {{"command", "not_a_registered_command"}})
                      .value("code"), "UNKNOWN_COMMAND");
    }
    manager.clearAllForTesting();
}
