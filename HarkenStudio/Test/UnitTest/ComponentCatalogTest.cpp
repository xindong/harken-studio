#include "Core/ComponentCatalog.h"

#include <gtest/gtest.h>

namespace Catalog = VNX::ComponentCatalog;
using VNX::ComponentType;

namespace {

const ComponentType kUnknownComponent = static_cast<ComponentType>(99);

}

TEST(ComponentCatalog, EnumValuesAreDenselyPacked)
{
    EXPECT_EQ(int(ComponentType::MixNode), 0);
    EXPECT_EQ(int(ComponentType::Audio), 1);
    EXPECT_EQ(int(ComponentType::Selection), 2);
    EXPECT_EQ(int(ComponentType::Bus), 3);
    EXPECT_EQ(int(ComponentType::Action), 4);
    EXPECT_EQ(int(ComponentType::Variable), 5);
    EXPECT_EQ(int(ComponentType::Attenuation), 6);
    EXPECT_EQ(int(ComponentType::Spatial), 7);
    EXPECT_EQ(int(ComponentType::Track), 8);
    EXPECT_EQ(int(ComponentType::Piece), 9);
    EXPECT_EQ(int(ComponentType::Sound), 10);
    EXPECT_EQ(int(ComponentType::_Count), 11);
    EXPECT_EQ(VNX::kMaxComponents, 11u);
}

TEST(ComponentCatalog, AllComponentTypesCoversEverySlotOnce)
{
    const auto& all = Catalog::allComponentTypes();
    EXPECT_EQ(all.size(), 11u);
    for (size_t i = 0; i < all.size(); ++i)
        EXPECT_EQ(static_cast<size_t>(all[i]), i);
    for (ComponentType t : all)
        EXPECT_NE(Catalog::factsOrNull(t), nullptr);
    EXPECT_EQ(Catalog::factsOrNull(kUnknownComponent), nullptr);
}

TEST(ComponentCatalog, NameGoldenTable)
{
    EXPECT_EQ(Catalog::componentName(ComponentType::MixNode), "mixnode");
    EXPECT_EQ(Catalog::componentName(ComponentType::Audio), "audio");
    EXPECT_EQ(Catalog::componentName(ComponentType::Selection), "selection");
    EXPECT_EQ(Catalog::componentName(ComponentType::Bus), "bus");
    EXPECT_EQ(Catalog::componentName(ComponentType::Action), "action");
    EXPECT_EQ(Catalog::componentName(ComponentType::Variable), "variable");
    EXPECT_EQ(Catalog::componentName(ComponentType::Attenuation), "attenuation");
    EXPECT_EQ(Catalog::componentName(ComponentType::Spatial), "spatial");
    EXPECT_EQ(Catalog::componentName(ComponentType::Track), "tracktimeline");
    EXPECT_EQ(Catalog::componentName(ComponentType::Piece), "piecetimeline");
    EXPECT_EQ(Catalog::componentName(ComponentType::Sound), "sound");
    EXPECT_EQ(Catalog::componentName(kUnknownComponent), "unknown");
}

TEST(ComponentCatalog, ParseRoundTripIsCaseSensitive)
{
    for (ComponentType t : Catalog::allComponentTypes()) {
        const auto parsed = Catalog::parseComponentName(Catalog::componentName(t));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, t);
    }
    EXPECT_FALSE(Catalog::parseComponentName("unknown").has_value());
    EXPECT_FALSE(Catalog::parseComponentName("MixNode").has_value());
    EXPECT_FALSE(Catalog::parseComponentName(QString()).has_value());
}
