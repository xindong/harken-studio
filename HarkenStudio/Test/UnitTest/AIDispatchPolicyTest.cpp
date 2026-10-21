#include "AI/AIDispatchPolicy.h"

#include <gtest/gtest.h>

#include <QJsonArray>

using namespace VNX;

namespace {

AIDispatchPolicy::Request pingRequest()
{
    return {
        QStringLiteral("ping"),
        QJsonObject{},
        QStringLiteral("ask"),
        QStringLiteral("ai-0001"),
        QString()
    };
}

AIDispatchPolicy::Request writeRequest()
{
    return {
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("object_id"), 7},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), -6.0}
        },
        QStringLiteral("ask"),
        QStringLiteral("ai-0001"),
        QString()
    };
}

AICommandSpec syntheticPolicyWriteSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("synthetic_policy_write");
    spec.effect = AICommandSpec::Effect::ProjectState;
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.confirmation = AICommandSpec::Confirmation::WhenPolicyRequires;
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false}
    };
    return spec;
}

}

TEST(AIDispatchPolicyTest, CanonicalBindingSortsObjectsAndPreservesArrayOrder)
{
    AIDispatchPolicy policy;
    auto first = writeRequest();
    first.args = QJsonObject{
        {QStringLiteral("object_id"), 7},
        {QStringLiteral("nested"), QJsonObject{
            {QStringLiteral("b"), 2}, {QStringLiteral("a"), 1}}},
        {QStringLiteral("array"), QJsonArray{1, 2}}
    };
    auto reordered = first;
    reordered.args = QJsonObject{
        {QStringLiteral("array"), QJsonArray{1, 2}},
        {QStringLiteral("nested"), QJsonObject{
            {QStringLiteral("a"), 1}, {QStringLiteral("b"), 2}}},
        {QStringLiteral("object_id"), 7}
    };
    EXPECT_EQ(policy.bindingFor(first), policy.bindingFor(reordered));

    reordered.args.insert(QStringLiteral("array"), QJsonArray{2, 1});
    EXPECT_NE(policy.bindingFor(first), policy.bindingFor(reordered));
}

TEST(AIDispatchPolicyTest, BindingIncludesCommandArgumentsAndPermission)
{
    AIDispatchPolicy policy;
    const auto baseline = writeRequest();
    const QByteArray binding = policy.bindingFor(baseline);

    auto changed = baseline;
    changed.cmd = QStringLiteral("ping");
    EXPECT_NE(binding, policy.bindingFor(changed));
    changed = baseline;
    changed.args.insert(QStringLiteral("value"), -3.0);
    EXPECT_NE(binding, policy.bindingFor(changed));
    changed = baseline;
    changed.permissionProfile = QStringLiteral("read_only");
    EXPECT_NE(binding, policy.bindingFor(changed));
}

TEST(AIDispatchPolicyTest, EnforcesAvailabilityPermissionSchemaAndConfirmation)
{
    AIDispatchPolicy policy;
    EXPECT_TRUE(policy.evaluate(pingRequest()).allowed);

    auto unavailable = pingRequest();
    unavailable.cmd = QStringLiteral("get_ui_tree");
    EXPECT_EQ(policy.evaluate(unavailable).code, QStringLiteral("AI_COMMAND_UNAVAILABLE"));
    unavailable.cmd = QStringLiteral("create_pack");
    EXPECT_EQ(policy.evaluate(unavailable).code, QStringLiteral("AI_COMMAND_UNAVAILABLE"));

    auto readOnly = writeRequest();
    readOnly.permissionProfile = QStringLiteral("read_only");
    EXPECT_EQ(policy.evaluate(readOnly).code, QStringLiteral("AI_PROFILE_DENIED"));

    auto invalid = writeRequest();
    invalid.args.remove(QStringLiteral("object_id"));
    EXPECT_EQ(policy.evaluate(invalid).code, QStringLiteral("INVALID_ARGUMENTS"));
    EXPECT_EQ(policy.evaluate(writeRequest()).code,
              QStringLiteral("AI_CONFIRMATION_REQUIRED"));
}

TEST(AIDispatchPolicyTest, GrantIsBoundOneShotExpiringAndRevocable)
{
    AIDispatchPolicy policy;
    auto request = writeRequest();
    request.grantId = policy.issueGrant(request);
    EXPECT_TRUE(policy.evaluate(request).allowed);
    EXPECT_EQ(policy.evaluate(request).code, QStringLiteral("AI_GRANT_REPLAYED"));

    auto mismatch = writeRequest();
    mismatch.grantId = policy.issueGrant(mismatch);
    mismatch.args.insert(QStringLiteral("value"), -3.0);
    EXPECT_EQ(policy.evaluate(mismatch).code, QStringLiteral("AI_GRANT_INVALID"));

    auto expired = writeRequest();
    expired.grantId = policy.issueGrant(expired, 0);
    EXPECT_EQ(policy.evaluate(expired).code, QStringLiteral("AI_GRANT_EXPIRED"));

    auto revoked = writeRequest();
    revoked.requestId = QStringLiteral("ai-revoked");
    revoked.grantId = policy.issueGrant(revoked);
    policy.revokeRequest(revoked.requestId);
    EXPECT_EQ(policy.evaluate(revoked).code, QStringLiteral("AI_GRANT_INVALID"));
}

TEST(AIDispatchPolicyTest, WhenPolicyRequiresUsesTypedPermissionModeAtTheDispatchGate)
{
    AIDispatchPolicy policy(QList<AICommandSpec>{syntheticPolicyWriteSpec()});
    auto request = writeRequest();
    request.cmd = QStringLiteral("synthetic_policy_write");
    request.args = QJsonObject{};
    request.permissionProfile = QStringLiteral("auto");
    EXPECT_TRUE(policy.evaluate(request).allowed);

    request.permissionProfile = QStringLiteral("ask");
    EXPECT_EQ(policy.evaluate(request).code, QStringLiteral("AI_CONFIRMATION_REQUIRED"));

    request.permissionProfile = QStringLiteral("read_only");
    EXPECT_EQ(policy.evaluate(request).code, QStringLiteral("AI_PROFILE_DENIED"));
}
