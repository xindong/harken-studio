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

TEST(AICommandCatalogTest, SelectionUsesUniqueCommandsExplicitProfilesAndNormalizedFields)
{
    QSet<QString> names;
    for (const auto& entry : AICommandCatalog::all())
    {
        EXPECT_FALSE(names.contains(entry.cmd)) << entry.cmd.toStdString();
        names.insert(entry.cmd);
        EXPECT_EQ(AICommandCatalog::find(entry.cmd), &entry);
    }
    const auto authoring = AICommandCatalog::selectTools(QStringLiteral("authoring"),
                                                          QStringLiteral("ask"));
    ASSERT_TRUE(authoring.valid);
    for (int i = 0; i < authoring.tools.size(); ++i)
    {
        EXPECT_NE(AICommandCatalog::find(authoring.tools.at(i).cmd), nullptr);
        EXPECT_TRUE(authoring.tools.at(i).toolProfiles.contains(QStringLiteral("authoring")));
        EXPECT_EQ(authoring.tools.at(i).availability,
                  AICommandSpec::Availability::Enabled);
        EXPECT_FALSE(authoring.tools.at(i).description.trimmed().isEmpty());
        EXPECT_FALSE(authoring.tools.at(i).toolProfiles.isEmpty());
        EXPECT_GT(authoring.tools.at(i).contractVersion, 0);
    }

    const auto readOnly = AICommandCatalog::selectTools(QStringLiteral("read_only"),
                                                         QStringLiteral("read_only"));
    ASSERT_TRUE(readOnly.valid);
    for (const auto& entry : readOnly.tools)
        EXPECT_EQ(entry.effect, AICommandSpec::Effect::Query) << entry.cmd.toStdString();

    const auto pack = AICommandCatalog::selectTools(QStringLiteral("pack"), QStringLiteral("ask"));
    const auto live = AICommandCatalog::selectTools(QStringLiteral("live"), QStringLiteral("ask"));
    ASSERT_TRUE(pack.valid);
    ASSERT_TRUE(live.valid);
    EXPECT_NE(commandNames(pack.tools), commandNames(authoring.tools));
    EXPECT_NE(commandNames(live.tools), commandNames(authoring.tools));
    EXPECT_TRUE(AICommandCatalog::selectTools(QStringLiteral("pack"), QStringLiteral("read_only")).valid);

    const auto unknown = AICommandCatalog::selectTools(QStringLiteral("unrecognized"), QStringLiteral("ask"));
    EXPECT_FALSE(unknown.valid);
    EXPECT_TRUE(unknown.tools.isEmpty());
    EXPECT_EQ(unknown.statusCode, QStringLiteral("AI_TOOL_PROFILE_INVALID"));

    const auto zero = AICommandCatalog::selectTools(QStringLiteral("pack"), QStringLiteral("read_only"), 0);
    EXPECT_TRUE(zero.valid);
    EXPECT_TRUE(zero.tools.isEmpty());

    const auto bypass = AICommandCatalog::selectTools(QStringLiteral("authoring"), QStringLiteral("bypass"));
    const auto ask = AICommandCatalog::selectTools(QStringLiteral("authoring"), QStringLiteral("ask"));
    EXPECT_EQ(commandNames(bypass.tools), commandNames(ask.tools));

    const auto unknownPermission = AICommandCatalog::selectTools(
        QStringLiteral("authoring"), QStringLiteral("unexpected"));
    EXPECT_EQ(commandNames(unknownPermission.tools), commandNames(ask.tools));

    const auto capped = AICommandCatalog::selectTools(QStringLiteral("authoring"), QStringLiteral("ask"), 2);
    ASSERT_TRUE(capped.valid);
    ASSERT_EQ(capped.tools.size(), 2);
    EXPECT_EQ(capped.tools.at(0).cmd, ask.tools.at(0).cmd);
    EXPECT_EQ(capped.tools.at(1).cmd, ask.tools.at(1).cmd);
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
