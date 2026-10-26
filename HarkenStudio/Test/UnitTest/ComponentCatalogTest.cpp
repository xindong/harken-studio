#include "Core/ComponentCatalog.h"

#include <gtest/gtest.h>

#include <set>

namespace Catalog = VNX::ComponentCatalog;
using VNX::ComponentType;

TEST(ComponentCatalog, AllComponentTypesCoversEverySlotOnce)
{
    const auto& all = Catalog::allComponentTypes();
    EXPECT_EQ(all.size(), VNX::kMaxComponents);
    const std::set<ComponentType> unique(all.begin(), all.end());
    EXPECT_EQ(unique.size(), all.size());
    for (ComponentType t : all)
        EXPECT_NE(Catalog::factsOrNull(t), nullptr);
    EXPECT_EQ(Catalog::factsOrNull(ComponentType::_Count), nullptr);
}

TEST(ComponentCatalog, ParseRoundTripPreservesComponentIdentity)
{
    for (ComponentType t : Catalog::allComponentTypes()) {
        const auto parsed = Catalog::parseComponentName(Catalog::componentName(t));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, t);
    }
}
