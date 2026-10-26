#include "AI/AIDispatchPolicy.h"

#include <gtest/gtest.h>

#include <QJsonArray>

using namespace VNX;

namespace {

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
