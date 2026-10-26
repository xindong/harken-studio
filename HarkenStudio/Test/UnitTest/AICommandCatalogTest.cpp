#include "AI/AICommandCatalog.h"
#include "IPC/IPCServer.h"
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
