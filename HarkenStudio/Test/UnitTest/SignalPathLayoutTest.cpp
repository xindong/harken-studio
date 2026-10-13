#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Blueprint/SignalPathSource.h"

#include "Core/Components/VTrack.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <QApplication>

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <map>

namespace {

using namespace VNX;
using T = ProjectItemType;
constexpr std::array kDirections{CanvasFlowDirection::LR, CanvasFlowDirection::RL,
    CanvasFlowDirection::TB, CanvasFlowDirection::BT};

class SignalPathLayoutTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (qApp) return;
        static int argc = 1;
        static char name[] = "harken_editor_tests";
        static char* argv[] = {name, nullptr};
        static QApplication app(argc, argv);
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto item = std::make_shared<ProjectItem>(QString::fromLatin1(name), type);
        item->guid = QUuid::createUuid();
        item->objectID = nextID++;
        item->guid = VNX::authoringUuid(harkentest::StudioObjectId(item->objectID));
        item->attachDefaultComponents();
        auto* result = item.get();
        if (parent) {
            item->parent = parent;
            item->parentRef = {type == T::Bus ? ParentRef::Kind::Bus : ParentRef::Kind::Item,
                parent->guid};
            parent->children.push_back(std::move(item));
        } else {
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type))
                item->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
            auto& bucket = type == T::Bus ? project.busses : project.layers;
            bucket.push_back({std::move(item), {}});
        }
        return result;
    }

    PlayablePropertyLocatorV1 place(ProjectItem* owner, const ProjectItem* target)
    {
        if (owner->type == T::Container) {
            SelectionEntry row;
            EXPECT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                *owner, target->guid, {}, &row));
            return *PlayablePropertyAuthoringV1::containerChildLocator(*owner, row.stableOccurrenceOrdinal);
        }
        Clip clip{QUuid::createUuid(), target->guid, 0, 1};
        auto& ledger = owner->comp<VTrack>()->clips;
        clip.stableOccurrenceOrdinal = static_cast<uint32_t>(ledger.occurrenceOrdinalHighWater++);
        ledger.items.push_back(clip);
        return *PlayablePropertyAuthoringV1::trackClipLocator(*owner, clip.clipGuid);
    }

    std::map<QUuid, QPointF> positions(const SignalPathGraph& graph, CanvasFlowDirection direction)
    {
        SignalPathSource source;
        source.loadGraph(project, graph);
        std::vector<QSizeF> sizes;
        for (const auto& node : graph.nodes)
            sizes.emplace_back(220.0 + 10.0 * (node.objectID % 3),
                               100.0 + 20.0 * (node.objectID % 2));
        const auto layout = source.layout(sizes, direction);
        EXPECT_EQ(layout.nodePositions.size(), graph.nodes.size());
        std::map<QUuid, QPointF> result;
        for (size_t index = 0; index < layout.nodePositions.size(); ++index)
            result.emplace(graph.nodes[index].key.objectGuid, layout.nodePositions[index]);
        return result;
    }

    void expectOccurrenceCountDoesNotChangeLayout(T ownerType)
    {
        auto* owner = create("Owner", ownerType, layer);
        auto* first = create("First", T::Sound, owner);
        auto* second = create("Second", T::Sound, owner);
        const auto firstUse = place(owner, first);
        const auto secondUse = place(owner, second);
        const auto initial = buildSignalPathGraph(project,
            {{first->guid, {firstUse}}, {second->guid, {secondUse}}});
        ASSERT_TRUE(initial.ok());
        std::vector<SignalPathIdentity> uses{{first->guid, {firstUse}}};
        for (int index = 1; index < 20; ++index)
            uses.push_back({first->guid, {place(owner, first)}});
        uses.push_back({second->guid, {secondUse}});
        const auto repeated = buildSignalPathGraph(project, uses);
        ASSERT_TRUE(repeated.ok());
        ASSERT_EQ(initial.nodes.size(), repeated.nodes.size());
        EXPECT_EQ(initial.paths.size(), 2u);
        EXPECT_EQ(repeated.paths.size(), 21u);
        for (const auto direction : kDirections) {
            SCOPED_TRACE(static_cast<int>(direction));
            EXPECT_EQ(positions(initial, direction), positions(repeated, direction));
        }
    }

    ProjectData project = HarkenTest::projectWithPlatforms({"Windows"});
    uint32_t nextID = 1;
    ProjectItem* master = create("Master", T::Bus);
    ProjectItem* layer = create("Layer", T::Layer);
};

TEST_F(SignalPathLayoutTest, RepeatedTrackClipsDoNotAllocateAdditionalObjectRows)
{
    expectOccurrenceCountDoesNotChangeLayout(T::Track);
}

TEST_F(SignalPathLayoutTest, RepeatedContainerChildrenDoNotBiasSharedNodePositions)
{
    expectOccurrenceCountDoesNotChangeLayout(T::Container);
}

TEST_F(SignalPathLayoutTest, GainNodesSeparateOutputAndSendRoutesWithoutOverlapsInEveryDirection)
{
    auto* output = create("Output", T::Bus, master);
    auto* send = create("Send", T::Bus, master);
    auto* owner = create("Container", T::Container, layer);
    auto* first = create("First", T::Sound, owner);
    auto* second = create("Second", T::Sound, owner);
    layer->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(output->guid), -3.0f}};
    owner->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(send->guid), -6.0f}}}};
    const auto graph = buildSignalPathGraph(project,
        {{first->guid, {place(owner, first)}}, {second->guid, {place(owner, second)}}});
    ASSERT_TRUE(graph.ok());
    SignalPathSource source;
    source.loadGraph(project, graph);
    std::vector<QSizeF> sizes(graph.nodes.size(), QSizeF(240, 110));
    std::vector<QSizeF> gainSizes(graph.edges.size());
    for (size_t index = 0; index < graph.edges.size(); ++index)
        if (graph.edges[index].kind != SignalPathEdgeKind::Through) gainSizes[index] = QSizeF(190, 36);
    for (const auto direction : kDirections) {
        SCOPED_TRACE(static_cast<int>(direction));
        const auto layout = source.layout(sizes, direction, {}, gainSizes);
        ASSERT_EQ(layout.nodePositions.size(), graph.nodes.size());
        ASSERT_EQ(layout.gainPositions.size(), graph.edges.size());
        std::vector<QRectF> rectangles;
        for (size_t index = 0; index < graph.nodes.size(); ++index)
            rectangles.emplace_back(layout.nodePositions[index], sizes[index]);
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            EXPECT_EQ(layout.gainPositions[index].has_value(), edge.kind != SignalPathEdgeKind::Through);
            if (!layout.gainPositions[index]) continue;
            const QRectF gain(*layout.gainPositions[index], gainSizes[index]);
            rectangles.push_back(gain);
            const auto& from = rectangles[edge.from.occurrenceIndex];
            const auto& to = rectangles[edge.to.occurrenceIndex];
            switch (direction) {
            case CanvasFlowDirection::LR:
                EXPECT_LT(from.right(), gain.left()); EXPECT_LT(gain.right(), to.left()); break;
            case CanvasFlowDirection::RL:
                EXPECT_GT(from.left(), gain.right()); EXPECT_GT(gain.left(), to.right()); break;
            case CanvasFlowDirection::TB:
                EXPECT_LT(from.bottom(), gain.top()); EXPECT_LT(gain.bottom(), to.top()); break;
            case CanvasFlowDirection::BT:
                EXPECT_GT(from.top(), gain.bottom()); EXPECT_GT(gain.top(), to.bottom()); break;
            }
        }
        for (size_t index = 0; index < rectangles.size(); ++index)
            for (size_t other = index + 1; other < rectangles.size(); ++other)
                EXPECT_FALSE(rectangles[index].adjusted(-20, -20, 20, 20).intersects(rectangles[other]));
        const auto repeated = source.layout(sizes, direction, {}, gainSizes);
        EXPECT_EQ(repeated.nodePositions, layout.nodePositions);
        EXPECT_EQ(repeated.gainPositions, layout.gainPositions);
    }
}

TEST_F(SignalPathLayoutTest, AutomaticNodesAndGainsAvoidPersistedGainAndObjectPositions)
{
    auto* owner = create("Container", T::Container, layer);
    auto* sound = create("Sound", T::Sound, owner);
    const auto graph = buildSignalPathGraph(project, {{sound->guid, {place(owner, sound)}}});
    ASSERT_TRUE(graph.ok());
    SignalPathSource source;
    source.loadGraph(project, graph);
    const std::vector<QSizeF> sizes(graph.nodes.size(), QSizeF(240, 110));
    std::vector<QSizeF> gainSizes(graph.edges.size());
    const auto found = std::find_if(graph.edges.begin(), graph.edges.end(),
        [](const auto& edge) { return edge.kind == SignalPathEdgeKind::Output; });
    ASSERT_NE(found, graph.edges.end());
    const auto gainIndex = static_cast<size_t>(found - graph.edges.begin());
    for (size_t index = 0; index < graph.edges.size(); ++index)
        if (graph.edges[index].kind != SignalPathEdgeKind::Through) gainSizes[index] = QSizeF(190, 36);
    for (const auto direction : kDirections) {
        SCOPED_TRACE(static_cast<int>(direction));
        const auto automatic = source.layout(sizes, direction, {}, gainSizes);
        std::vector<std::optional<QPointF>> fixedNodes(graph.nodes.size());
        std::vector<std::optional<QPointF>> fixedGains(graph.edges.size());
        fixedNodes.front() = QPointF(-500, -500);
        fixedGains[gainIndex] = automatic.nodePositions.back();
        const auto layout = source.layout(sizes, direction, fixedNodes, gainSizes, fixedGains);
        EXPECT_EQ(layout.nodePositions.front(), *fixedNodes.front());
        EXPECT_EQ(layout.gainPositions[gainIndex], fixedGains[gainIndex]);
        std::vector<QRectF> rectangles;
        for (size_t index = 0; index < graph.nodes.size(); ++index)
            rectangles.emplace_back(layout.nodePositions[index], sizes[index]);
        for (size_t index = 0; index < graph.edges.size(); ++index)
            if (layout.gainPositions[index]) rectangles.emplace_back(*layout.gainPositions[index], gainSizes[index]);
        for (size_t index = 0; index < rectangles.size(); ++index)
            for (size_t other = index + 1; other < rectangles.size(); ++other)
                EXPECT_FALSE(rectangles[index].adjusted(-20, -20, 20, 20).intersects(rectangles[other]));
    }
}

}
