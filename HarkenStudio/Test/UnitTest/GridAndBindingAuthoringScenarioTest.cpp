#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingSemantics.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "GridSemantics.h"
#include "ProjectTypes.h"
#include "ReferenceSlotResolution.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

QUuid id(int value)
{
    return QUuid(0xC1000000u + value, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}



}

TEST(GridAndBindingAuthoringScenario, FootstepMaterialAndMusicMatrixSelectExpectedRows)
{
    VNX::DiscreteGridDef footsteps;
    ASSERT_TRUE(footsteps.addAxis(id(1)));
    ASSERT_TRUE(footsteps.addRow(id(10)));
    ASSERT_TRUE(footsteps.addRow(id(11)));
    ASSERT_TRUE(footsteps.setCell(0, 0, 0));
    ASSERT_TRUE(footsteps.setCell(1, 0, 1));
    EXPECT_EQ(VNX::GridSemantics::evalDiscreteGrid(footsteps, {1.0f}).winner, 1);

    VNX::DiscreteGridDef music;
    ASSERT_TRUE(music.addAxis(id(2)));
    ASSERT_TRUE(music.addAxis(id(3)));
    ASSERT_TRUE(music.addRow(id(20)));
    ASSERT_TRUE(music.addRow(id(21)));
    ASSERT_TRUE(music.addRow(id(22)));
    ASSERT_TRUE(music.setCell(0, 0, 2));
    ASSERT_TRUE(music.setCell(0, 1, 1));
    ASSERT_TRUE(music.setCell(1, 0, 2));
    ASSERT_TRUE(music.setCell(2, 1, 1));

    const auto exact = VNX::GridSemantics::evalDiscreteGrid(music, {2.0f, 1.0f});
    EXPECT_EQ(exact.hits, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(exact.winner, 0);
    EXPECT_EQ(VNX::GridSemantics::evalDiscreteGrid(music, {2.0f, 3.0f}).winner, 1);
    EXPECT_EQ(VNX::GridSemantics::evalDiscreteGrid(music, {4.0f, 1.0f}).winner, 2);
}

TEST(GridAndBindingAuthoringScenario, RainBedBlendsOverlappingBandsWithNestedMatch)
{
    VNX::ProjectItem outerItem{QStringLiteral("RainBed"), VNX::ProjectItem::Type::Container};
    auto nestedItem = std::make_unique<VNX::ProjectItem>(
        QStringLiteral("ThunderLayers"), VNX::ProjectItem::Type::Container);
    nestedItem->guid = id(30);
    nestedItem->parent = &outerItem;
    VNX::ProjectItem* nestedTarget = nestedItem.get();
    outerItem.children.push_back(std::move(nestedItem));

    VNX::VSelection* outer = outerItem.compOrCreate<VNX::VSelection>();
    outer->mode = VNX::SelectionMode::Blend;
    outer->blendResponse = VNX::VariableResponse::Live;
    {
        auto mixing = outer->mixing();
        mixing.rows.push_back({id(2), {}});
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(id(31), 0.0f, 100.0f));
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(id(32), 0.0f, 100.0f));
        mixing.rows[0].children[0].points = {{0.0f, 0.0f}, {20.0f, 1.0f}, {40.0f, 1.0f}, {60.0f, 0.0f}, {100.0f, 0.0f}};
        mixing.rows[0].children[1].points = {{0.0f, 0.0f}, {40.0f, 0.0f}, {60.0f, 1.0f}, {80.0f, 1.0f}, {100.0f, 0.0f}};
        for (auto& child : mixing.rows[0].children) child.normalize();
        EXPECT_TRUE(outer->selections.shared.setMixing(mixing));
    }

    {
        auto mixing = outer->mixing();
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(nestedTarget->guid, 0.0f, 100.0f));
        EXPECT_TRUE(outer->selections.shared.setMixing(mixing));
    }

    VNX::VSelection* nested = nestedTarget->compOrCreate<VNX::VSelection>();
    nested->mode = VNX::SelectionMode::Match;
    {
        auto grid = nested->matchGrid();
        ASSERT_TRUE(grid.addAxis(id(3)));
        ASSERT_TRUE(grid.addRow(id(33)));
        ASSERT_TRUE(grid.setCell(0, 0, 4));
        EXPECT_TRUE(nested->selections.shared.setMatchGrid(grid));
    }

    EXPECT_EQ(outer->mixing().rows.front().children[2].target, nestedTarget->guid);
    const auto evaluated = VNX::MixingSemantics::evaluate(outer->mixing(),
        [](const QUuid&) -> std::optional<float> { return 50.0f; });
    ASSERT_TRUE(evaluated);
    const auto& rain = *evaluated;
    ASSERT_EQ(rain.size(), 3u);
    EXPECT_FLOAT_EQ(rain.value(nestedTarget->guid), 1.0f);
    EXPECT_GT(rain.value(id(31)), 0.0f);
    EXPECT_GT(rain.value(id(32)), 0.0f);
    EXPECT_EQ(VNX::GridSemantics::evalDiscreteGrid(
                  nested->matchGrid(), {4.0f}).winner, 0);

    const QJsonObject roundtrip = outer->toJson();
    VNX::VSelection loaded;
    loaded.fromJson(roundtrip);
    EXPECT_EQ(loaded.toJson(), roundtrip);
    EXPECT_TRUE(loaded.matchGrid().empty());
    EXPECT_FALSE(loaded.mixing().empty());
}

TEST(GridAndBindingAuthoringScenario, DescendantCombinesBindingContributionsFromSignalChain)
{
    VNX::ProjectItem alley{QStringLiteral("RainyAlley"), VNX::ProjectItem::Type::Layer};
    VNX::ProjectItem music{QStringLiteral("Music"), VNX::ProjectItem::Type::Container};
    VNX::ProjectItem marker{QStringLiteral("CombatMarker"), VNX::ProjectItem::Type::Sound};
    music.parent = &alley;
    marker.parent = &music;

    auto alleyBinding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), id(1));
    auto markerBinding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), id(2));
    alleyBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    markerBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(2));
    ASSERT_TRUE(alley.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(alleyBinding)));
    ASSERT_TRUE(marker.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(markerBinding)));

    const auto bindings = VNX::ReferenceSlotResolution::resolveParameterBindings(
        &marker, VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(bindings.size(), 2u);
    EXPECT_EQ(bindings[0].owner, &marker);
    EXPECT_EQ(bindings[1].owner, &alley);
}
