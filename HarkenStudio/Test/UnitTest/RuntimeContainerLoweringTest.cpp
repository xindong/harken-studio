#include "UnitTest/StudioObjectIdTestSupport.h"
#include "gtest/gtest.h"

#include "Core/Components/VSelection.h"
#include "Core/Components/VVariable.h"
#include "Core/ContainerBehaviorSemantics.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/RuntimeContainerLowering.h"

#include <algorithm>

using namespace VNX;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, uint64_t id)
{
    auto item = std::make_shared<ProjectItem>(QStringLiteral("Item"), type);
    item->guid = QUuid::createUuid();
    item->objectID = static_cast<uint32_t>(id);
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(id));
    item->attachDefaultComponents();
    return item;
}

ProjectData projectWith(const std::shared_ptr<ProjectItem>& root)
{
    ProjectData project;
    ProjectFileBinding binding;
    binding.rootItem = root;
    project.layers.push_back(std::move(binding));
    return project;
}

}

TEST(RuntimeContainerLoweringTest, LowersRandomDrawWeights)
{
    auto container = makeItem(ProjectItem::Type::Container, 10);
    auto first = makeItem(ProjectItem::Type::Sound, 11);
    auto second = makeItem(ProjectItem::Type::Sound, 12);
    first->parent = container.get();
    second->parent = container.get();
    container->children = {first, second};
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, second->guid));
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Random;
    selection->selections.shared.find(0)->weight = 3;

    const auto result = RuntimeContainerLowering::lower(
        *container, projectWith(container));
    ASSERT_TRUE(result.ok()) << result.diagnostic.toStdString();
    EXPECT_TRUE(result.plan.weightedRandom);
    ASSERT_EQ(result.plan.children.size(), 2u);
    const auto loweredFirst = std::find_if(
        result.plan.children.begin(), result.plan.children.end(),
        [first](const RuntimeContainerChild& child) {
            return child.playableId == first->runtimeId();
        });
    ASSERT_NE(loweredFirst, result.plan.children.end());
    EXPECT_EQ(loweredFirst->weight,
              static_cast<uint32_t>(ContainerBehaviorSemantics::effectiveChildWeight(
                  *selection, 0)));
}

TEST(RuntimeContainerLoweringTest, LowersSingleAxisDiscreteGrid)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 9);
    auto container = makeItem(ProjectItem::Type::Container, 10);
    auto first = makeItem(ProjectItem::Type::Sound, 11);
    auto second = makeItem(ProjectItem::Type::Sound, 12);
    auto variable = makeItem(ProjectItem::Type::Variable, 13);
    container->children = {first, second};
    first->parent = container.get();
    second->parent = container.get();
    layer->children = {container, variable};
    container->parent = layer.get();
    variable->parent = layer.get();
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, first->guid));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, second->guid));
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Match;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(first->guid));
        ASSERT_TRUE(grid.addRow(second->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 1));
        ASSERT_TRUE(grid.setCell(1, 0, 2));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }

    const auto result = RuntimeContainerLowering::lower(
        *container, projectWith(layer));
    ASSERT_TRUE(result.ok()) << result.diagnostic.toStdString();
    EXPECT_EQ(result.plan.variableId, variable->runtimeId());
    EXPECT_EQ(result.plan.children[0].matchValue, 1u);
    EXPECT_EQ(result.plan.children[1].matchValue, 2u);
}

TEST(RuntimeContainerLoweringTest, RejectsUnsupportedShapesAtomically)
{
    auto container = makeItem(ProjectItem::Type::Container, 10);
    auto child = makeItem(ProjectItem::Type::Sound, 11);
    child->parent = container.get();
    container->children = {child};
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, child->guid));
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    EXPECT_FALSE(RuntimeContainerLowering::lower(
        *container, projectWith(container)).ok());

    selection->mode = SelectionMode::Random;
    selection->playType = SelectionPlayType::Single;
    selection->behavior.discipline = RandomDiscipline::Shuffle;
    EXPECT_FALSE(RuntimeContainerLowering::lower(
        *container, projectWith(container)).ok());
}
