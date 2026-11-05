#include "AI/AICommandCatalog.h"
#include "IPC/IPCServer.h"
#include "EditorCommandRegistration.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QSet>

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
    for (const auto& name : cli.allCommandNames()) {
        EXPECT_FALSE(cli.commandRequiresDesktop(name)) << name.toStdString();
        EXPECT_TRUE(desktopNames.contains(name)) << name.toStdString();
        EXPECT_TRUE(manager.hasAction(name) || cli.hasCommandHandler(name));
    }
    for (const auto& name : desktopOnly) {
        EXPECT_FALSE(manager.hasAction(name)) << name.toStdString();
        EXPECT_FALSE(cli.hasCommandHandler(name)) << name.toStdString();
        EXPECT_EQ(cli.dispatchCommand(name, {}).value("code"), "UNKNOWN_COMMAND")
            << name.toStdString();
    }
    manager.clearAllForTesting();
}
