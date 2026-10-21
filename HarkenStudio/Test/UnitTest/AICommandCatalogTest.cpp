#include "AI/AICommandCatalog.h"
#include "AI/AICommandValidator.h"
#include "IPC/IPCServer.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QSet>

using namespace VNX;

namespace {

QStringList commandNames(const QList<AICommandSpec>& specs)
{
    QStringList names;
    names.reserve(specs.size());
    for (const auto& spec : specs)
        names.append(spec.cmd);
    return names;
}

}

TEST(AICommandCatalogTest, SelectionUsesAllEnabledCommandsAndPermissionMode)
{
    QSet<QString> names;
    QSet<QString> enabledNames;
    QSet<QString> queryNames;
    for (const auto& entry : AICommandCatalog::all())
    {
        EXPECT_FALSE(names.contains(entry.cmd)) << entry.cmd.toStdString();
        names.insert(entry.cmd);
        EXPECT_EQ(AICommandCatalog::find(entry.cmd), &entry);
        if (entry.availability == AICommandSpec::Availability::Enabled)
        {
            enabledNames.insert(entry.cmd);
            if (entry.effect == AICommandSpec::Effect::Query)
                queryNames.insert(entry.cmd);
        }
    }
    const auto selected = AICommandCatalog::selectTools();
    QSet<QString> selectedNames;
    for (const auto& entry : selected)
    {
        EXPECT_NE(AICommandCatalog::find(entry.cmd), nullptr);
        EXPECT_FALSE(selectedNames.contains(entry.cmd));
        selectedNames.insert(entry.cmd);
        EXPECT_EQ(entry.availability, AICommandSpec::Availability::Enabled);
        EXPECT_FALSE(entry.description.trimmed().isEmpty());
        EXPECT_GT(entry.contractVersion, 0);
    }
    EXPECT_EQ(selectedNames, enabledNames);
    EXPECT_EQ(commandNames(AICommandCatalog::enabled()), commandNames(selected));

    const auto readOnly = AICommandCatalog::selectTools(AIPermissionMode::ReadOnly);
    QSet<QString> readOnlyNames;
    for (const auto& entry : readOnly)
    {
        EXPECT_EQ(entry.effect, AICommandSpec::Effect::Query) << entry.cmd.toStdString();
        readOnlyNames.insert(entry.cmd);
    }
    EXPECT_EQ(readOnlyNames, queryNames);

    for (const QString& permission : {QStringLiteral("ask"), QStringLiteral("auto"),
                                      QStringLiteral("bypass"), QStringLiteral("unexpected")})
        EXPECT_EQ(commandNames(AICommandCatalog::selectTools(permission)), commandNames(selected));
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

TEST(AICommandCatalogTest, ReadOnlyQueryRejectsMutationArguments)
{
    const auto* execErrors = AICommandCatalog::find(QStringLiteral("get_exec_errors"));
    ASSERT_NE(execErrors, nullptr);
    EXPECT_FALSE(execErrors->argumentSchema.value(QStringLiteral("properties")).toObject()
                     .contains(QStringLiteral("clear")));

    AICommandValidator validator;
    QString errorCode;
    QString errorMessage;
    const auto normalized = validator.normalizeAndValidate(
        QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("get_exec_errors")},
            {QStringLiteral("args"), QJsonObject{{QStringLiteral("clear"), true}}},
            {QStringLiteral("request_id"), QStringLiteral("req-exec-errors")}
        },
        AICommandValidator::Policy(AIPermissionMode::ReadOnly, false),
        &errorCode, &errorMessage);
    EXPECT_FALSE(normalized.has_value());
    EXPECT_EQ(errorCode, QStringLiteral("INVALID_ARGUMENTS"));

    const auto accepted = validator.normalizeAndValidate(
        QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("get_exec_errors")},
            {QStringLiteral("args"), QJsonObject{}},
            {QStringLiteral("request_id"), QStringLiteral("req-exec-errors-clean")}
        },
        AICommandValidator::Policy(AIPermissionMode::ReadOnly, false),
        &errorCode, &errorMessage);
    EXPECT_TRUE(accepted.has_value());
}
