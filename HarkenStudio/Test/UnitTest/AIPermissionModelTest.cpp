#include "AI/AIPermissionModel.h"

#include <gtest/gtest.h>

using namespace VNX;

TEST(AIPermissionModelTest, PermissionIdsRoundTripAndLegacyValuesClampToAsk)
{
    EXPECT_EQ(toId(parsePermissionMode(" READ_ONLY ")), QStringLiteral("read_only"));
    EXPECT_EQ(toId(parsePermissionMode("AsK")), QStringLiteral("ask"));
    EXPECT_EQ(toId(parsePermissionMode(" AUTO ")), QStringLiteral("auto"));
    for (const QString& value : {QString(), QStringLiteral("bypass"), QStringLiteral("unknown")})
        EXPECT_EQ(parsePermissionMode(value), AIPermissionMode::Ask);
}

TEST(AIPermissionModelTest, ToolProfileParserPreservesInvalidity)
{
    EXPECT_EQ(parseToolProfile(" AUTHORING "), AIToolProfile::Authoring);
    EXPECT_FALSE(parseToolProfile("invalid"));
    EXPECT_FALSE(parseToolProfile(QString()));
}

TEST(AIPermissionModelTest, AllowsWritesAndConfirmationTruthTable)
{
    EXPECT_FALSE(allowsWrites(AIPermissionMode::ReadOnly));
    EXPECT_TRUE(allowsWrites(AIPermissionMode::Ask));
    EXPECT_TRUE(allowsWrites(AIPermissionMode::Auto));
    const AIPermissionMode modes[] = {AIPermissionMode::ReadOnly, AIPermissionMode::Ask, AIPermissionMode::Auto};
    for (int mode = 0; mode < 3; ++mode) {
        EXPECT_FALSE(requiresConfirmation(modes[mode], AIConfirmationPolicy::None));
        EXPECT_EQ(requiresConfirmation(modes[mode], AIConfirmationPolicy::WhenPolicyRequires), mode != 2);
        EXPECT_TRUE(requiresConfirmation(modes[mode], AIConfirmationPolicy::Always));
    }
    EXPECT_FALSE(requiresConfirmation(AIPermissionMode::Auto, AIConfirmationPolicy::WhenPolicyRequires));
}

TEST(AIPermissionModelTest, UiMetadataUsesCanonicalIds)
{
    ASSERT_EQ(permissionMetadata().size(), 4);
    EXPECT_STREQ(permissionMetadata().at(2).id, "auto");
    EXPECT_STREQ(permissionMetadata().at(2).label, "Auto-approve eligible writes");
    EXPECT_EQ(toolProfileMetadata().size(), 4);
}
