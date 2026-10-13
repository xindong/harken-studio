#include "AI/AICommandValidator.h"

#include <gtest/gtest.h>

#include <QJsonArray>

using namespace VNX;

namespace {

QJsonObject canonical(const QString& cmd = QStringLiteral("ping"),
                      const QJsonObject& args = {})
{
    return QJsonObject{
        {QStringLiteral("cmd"), cmd},
        {QStringLiteral("args"), args},
        {QStringLiteral("request_id"), QStringLiteral("ai-0001")}
    };
}

AICommandValidator::Policy readOnlyPolicy()
{
    return AICommandValidator::Policy{AIPermissionMode::ReadOnly, false};
}

void expectRejected(const AICommandValidator& validator, const QJsonObject& command,
                    const AICommandValidator::Policy& policy,
                    const QString& expectedCode)
{
    QString code;
    QString message;
    EXPECT_FALSE(validator.normalizeAndValidate(command, policy, &code, &message));
    EXPECT_EQ(code, expectedCode);
    EXPECT_FALSE(message.isEmpty());
}

AICommandValidator syntheticWriteValidator()
{
    AICommandSpec write;
    write.cmd = QStringLiteral("synthetic_write");
    write.effect = AICommandSpec::Effect::ProjectState;
    write.availability = AICommandSpec::Availability::Enabled;
    write.confirmation = AICommandSpec::Confirmation::Always;
    write.changeSupport = AICommandSpec::ChangeSupport::ChangeSet;
    write.source = AICommandSpec::Source::ActionManager;
    write.toolProfiles = QStringList{QStringLiteral("authoring")};
    write.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false}
    };
    return AICommandValidator(QList<AICommandSpec>{write});
}

AICommandValidator syntheticPolicyWriteValidator()
{
    AICommandSpec write;
    write.cmd = QStringLiteral("synthetic_policy_write");
    write.effect = AICommandSpec::Effect::ProjectState;
    write.availability = AICommandSpec::Availability::Enabled;
    write.confirmation = AICommandSpec::Confirmation::WhenPolicyRequires;
    write.toolProfiles = QStringList{QStringLiteral("authoring")};
    write.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false}
    };
    return AICommandValidator(QList<AICommandSpec>{write});
}

}

TEST(AICommandValidatorTest, AcceptsAndCanonicalizesTheExactEnvelope)
{
    AICommandValidator validator;
    QString code;
    QString message;
    const auto result = validator.normalizeAndValidate(
        canonical(), readOnlyPolicy(), &code, &message);

    ASSERT_TRUE(result);
    EXPECT_EQ(*result, canonical());
    EXPECT_TRUE(code.isEmpty());
    EXPECT_TRUE(message.isEmpty());
}

TEST(AICommandValidatorTest, RejectsAmbiguousOrIncompleteTopLevelShapes)
{
    AICommandValidator validator;
    const auto policy = readOnlyPolicy();

    expectRejected(validator, QJsonObject{
        {QStringLiteral("command"), QStringLiteral("ping")},
        {QStringLiteral("args"), QJsonObject{}},
        {QStringLiteral("request_id"), QStringLiteral("ai-0001")}
    }, policy, QStringLiteral("INVALID_ARGUMENTS"));

    QJsonObject both = canonical();
    both.insert(QStringLiteral("command"), QStringLiteral("ping"));
    expectRejected(validator, both, policy, QStringLiteral("INVALID_ARGUMENTS"));

    QJsonObject extra = canonical();
    extra.insert(QStringLiteral("extra"), true);
    expectRejected(validator, extra, policy, QStringLiteral("INVALID_ARGUMENTS"));

    QJsonObject missingArgs = canonical();
    missingArgs.remove(QStringLiteral("args"));
    expectRejected(validator, missingArgs, policy, QStringLiteral("INVALID_ARGUMENTS"));

    QJsonObject missingRequest = canonical();
    missingRequest.remove(QStringLiteral("request_id"));
    expectRejected(validator, missingRequest, policy, QStringLiteral("INVALID_ARGUMENTS"));
}

TEST(AICommandValidatorTest, RejectsCredentialKeysRecursivelyAndCaseInsensitively)
{
    AICommandValidator validator;
    const QJsonObject args{
        {QStringLiteral("nested"), QJsonArray{
            QJsonObject{{QStringLiteral("AuThOrIzAtIoN"), QStringLiteral("redacted")}}
        }}
    };
    expectRejected(validator, canonical(QStringLiteral("list_items"), args),
                   readOnlyPolicy(), QStringLiteral("INVALID_ARGUMENTS"));
}

TEST(AICommandValidatorTest, AppliesCatalogAndSchemaBeforeDispatch)
{
    AICommandValidator validator;
    expectRejected(validator, canonical(QStringLiteral("not_a_real_command")),
                   readOnlyPolicy(), QStringLiteral("INVALID_ARGUMENTS"));
    expectRejected(validator, canonical(QStringLiteral("get_3d_scene")),
                   readOnlyPolicy(), QStringLiteral("AI_COMMAND_DISABLED"));
    expectRejected(validator, canonical(QStringLiteral("get_item_info")),
                   readOnlyPolicy(), QStringLiteral("INVALID_ARGUMENTS"));
}

TEST(AICommandValidatorTest, RejectsMissingItemLocatorForReadAndWriteCommands)
{
    AICommandValidator validator;
    const QJsonObject emptyArgs;
    expectRejected(validator, canonical(QStringLiteral("get_item_info"), emptyArgs),
                   readOnlyPolicy(), QStringLiteral("INVALID_ARGUMENTS"));

    const AICommandValidator::Policy writePolicy{AIPermissionMode::Ask, true};
    expectRejected(validator, canonical(QStringLiteral("set_item_property"), QJsonObject{
                       {QStringLiteral("property"), QStringLiteral("volume")},
                       {QStringLiteral("value"), 0.5}
                   }), writePolicy, QStringLiteral("INVALID_ARGUMENTS"));
}
TEST(AICommandValidatorTest, EnforcesSyntheticWritePolicyWithoutEnablingProductionWrites)
{
    const AICommandValidator validator = syntheticWriteValidator();
    const QJsonObject command = canonical(QStringLiteral("synthetic_write"));

    expectRejected(validator, command,
                   AICommandValidator::Policy{AIPermissionMode::ReadOnly, false},
                   QStringLiteral("INVALID_ARGUMENTS"));
    expectRejected(validator, command,
                   AICommandValidator::Policy{AIPermissionMode::Ask, false},
                   QStringLiteral("AI_CONFIRMATION_REQUIRED"));

    expectRejected(validator, command,
                   AICommandValidator::Policy{AIPermissionMode::Auto, false},
                   QStringLiteral("AI_CONFIRMATION_REQUIRED"));

    QString code;
    QString message;
    EXPECT_TRUE(validator.normalizeAndValidate(
        command, AICommandValidator::Policy{AIPermissionMode::Ask, true},
        &code, &message));

    QJsonObject credentialCommand = command;
    credentialCommand.insert(QStringLiteral("args"), QJsonObject{
        {QStringLiteral("api_key"), QStringLiteral("forbidden")}
    });
    expectRejected(validator, credentialCommand,
                   AICommandValidator::Policy{AIPermissionMode::Ask, true},
                   QStringLiteral("INVALID_ARGUMENTS"));

    QJsonObject malformedCommand = command;
    malformedCommand.insert(QStringLiteral("args"), QStringLiteral("not-an-object"));
    expectRejected(validator, malformedCommand,
                   AICommandValidator::Policy{AIPermissionMode::Ask, true},
                   QStringLiteral("INVALID_ARGUMENTS"));
    EXPECT_NE(AICommandCatalog::find(QStringLiteral("create_project"))->availability,
              AICommandSpec::Availability::Enabled);
}


TEST(AICommandValidatorTest, UnsupportedAndLegacyPermissionModesClampToAsk)
{
    EXPECT_EQ(AICommandValidator::normalizePermissionProfile(QStringLiteral("bypass")),
              QStringLiteral("ask"));
    EXPECT_EQ(AICommandValidator::normalizePermissionProfile(QStringLiteral("unexpected")),
              QStringLiteral("ask"));
    EXPECT_EQ(AICommandValidator::normalizePermissionProfile(QStringLiteral(" AUTO ")),
              QStringLiteral("auto"));
}

TEST(AICommandValidatorTest, WhenPolicyRequiresUsesTypedPermissionModeAtTheGate)
{
    const AICommandValidator validator = syntheticPolicyWriteValidator();
    const auto command = canonical(QStringLiteral("synthetic_policy_write"));
    QString code;
    QString message;
    EXPECT_TRUE(validator.normalizeAndValidate(
        command, AICommandValidator::Policy{AIPermissionMode::Auto, false}, &code, &message));
    EXPECT_FALSE(validator.normalizeAndValidate(
        command, AICommandValidator::Policy{AIPermissionMode::Ask, false}, &code, &message));
    EXPECT_EQ(code, QStringLiteral("AI_CONFIRMATION_REQUIRED"));
    EXPECT_FALSE(validator.normalizeAndValidate(
        command, AICommandValidator::Policy{AIPermissionMode::ReadOnly, false}, &code, &message));
    EXPECT_EQ(code, QStringLiteral("INVALID_ARGUMENTS"));
}
