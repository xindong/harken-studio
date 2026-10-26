#include "Core/ThemePackageParser.h"

#include <gtest/gtest.h>

TEST(ThemePackageParser, RejectsRecipeSizeOutsideIntegerRange)
{
    const auto result = VNX::parseThemeRecipes(R"json({
        "recipes": [{
            "host": "track.header",
            "variant": "track.compact",
            "slots": [{"id": "identity", "preferred": 1e30}]
        }]
    })json");

    ASSERT_FALSE(result.ok());
}
