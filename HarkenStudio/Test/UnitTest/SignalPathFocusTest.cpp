#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Blueprint/SignalPathFocus.h"

#include "Core/Components/VMixNode.h"
#include "Core/Components/VTrack.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <tuple>

namespace {

using namespace VNX;
using T = ProjectItemType;
using EdgeKey = std::tuple<QUuid, QUuid, SignalPathEdgeKind>;

class SignalPathFocusTest : public ::testing::Test
{
protected:
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows"});
    uint32_t nextID = 1;
    ProjectItem* master = create("Master", T::Bus);
    ProjectItem* b1 = create("B1", T::Bus, master);
    ProjectItem* b2 = create("B2", T::Bus, master);
    ProjectItem* b3 = create("B3", T::Bus, master);
    ProjectItem* layer = create("Layer", T::Layer);
    ProjectItem* s1 = create("S1", T::Sound, layer);
    ProjectItem* s2 = create("S2", T::Sound, layer);
    ProjectItem* otherLayer = create("Other Layer", T::Layer);
    ProjectItem* s3 = create("S3", T::Sound, otherLayer);

    void SetUp() override
    {
        output(s1, b1);
        output(s2, b2);
        output(s3, b3);
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

    static void output(ProjectItem* item, const ProjectItem* bus)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), 0.0f}};
    }

    SignalPathGraph build() const
    {
        return buildSignalPathGraph(project, {{s1->guid, {}}, {s2->guid, {}}, {s3->guid, {}}});
    }

    static size_t indexOf(const SignalPathGraph& graph, const ProjectItem* item)
    {
        const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto& node) { return node.key.objectGuid == item->guid; });
        EXPECT_NE(found, graph.nodes.end()) << item->name.toStdString();
        return static_cast<size_t>(found - graph.nodes.begin());
    }

    static EdgeKey edge(const ProjectItem* from, const ProjectItem* to, SignalPathEdgeKind kind)
    {
        return {from->guid, to->guid, kind};
    }

    static size_t edgeIndex(const SignalPathGraph& graph, const ProjectItem* from,
        const ProjectItem* to, SignalPathEdgeKind kind)
    {
        const auto found = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& entry) {
            return graph.nodes[entry.from.occurrenceIndex].key.objectGuid == from->guid
                && graph.nodes[entry.to.occurrenceIndex].key.objectGuid == to->guid
                && entry.kind == kind;
        });
        EXPECT_NE(found, graph.edges.end()) << from->name.toStdString() << " -> " << to->name.toStdString();
        return static_cast<size_t>(found - graph.edges.begin());
    }

    static void expectFocus(const SignalPathGraph& graph, const SignalPathFocus& focus,
        const std::set<QUuid>& nodes, const std::set<EdgeKey>& edges)
    {
        ASSERT_TRUE(focus.active);
        ASSERT_EQ(focus.nodes.size(), graph.nodes.size());
        ASSERT_EQ(focus.edges.size(), graph.edges.size());
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            SCOPED_TRACE(graph.nodes[index].name.toStdString());
            EXPECT_EQ(focus.nodes[index], nodes.contains(graph.nodes[index].key.objectGuid));
        }
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& entry = graph.edges[index];
            const auto& from = graph.nodes[entry.from.occurrenceIndex];
            const auto& to = graph.nodes[entry.to.occurrenceIndex];
            SCOPED_TRACE(from.name.toStdString() + " -> " + to.name.toStdString());
            EXPECT_EQ(focus.edges[index], edges.contains({from.key.objectGuid, to.key.objectGuid, entry.kind}));
        }
    }
};

TEST_F(SignalPathFocusTest, SoundFocusDoesNotExpandIntoOtherOutputsThroughSharedLayerOrMaster)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    const auto focus = projectSignalPathFocus(graph, {indexOf(graph, s1)});
    expectFocus(graph, focus, {s1->guid, layer->guid, b1->guid, master->guid}, {
        edge(s1, layer, SignalPathEdgeKind::Through),
        edge(layer, b1, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output)
    });
}

TEST_F(SignalPathFocusTest, LayerFocusIncludesAllItsPathsAndExcludesUnrelatedLayer)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    const auto focus = projectSignalPathFocus(graph, {indexOf(graph, layer)});
    expectFocus(graph, focus, {s1->guid, s2->guid, layer->guid, b1->guid, b2->guid, master->guid}, {
        edge(s1, layer, SignalPathEdgeKind::Through),
        edge(s2, layer, SignalPathEdgeKind::Through),
        edge(layer, b1, SignalPathEdgeKind::Output),
        edge(layer, b2, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output),
        edge(b2, master, SignalPathEdgeKind::Output)
    });
}

TEST_F(SignalPathFocusTest, ThroughConnectionFocusIncludesItsCompleteInputPathWithoutSelectingEndpoints)
{
    s1->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b3->guid), -6.0f}}}};
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    const auto through = edgeIndex(graph, s1, layer, SignalPathEdgeKind::Through);
    const auto focus = projectSignalPathFocus(graph, {}, {through});
    expectFocus(graph, focus, {s1->guid, layer->guid, b1->guid, b3->guid, master->guid}, {
        edge(s1, layer, SignalPathEdgeKind::Through),
        edge(layer, b1, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output),
        edge(layer, b3, SignalPathEdgeKind::Send),
        edge(b3, master, SignalPathEdgeKind::Output)
    });
}

TEST_F(SignalPathFocusTest, ParallelOutputAndSendConnectionsOnlyFocusTheirActualPathMembers)
{
    output(s2, b1);
    for (auto* source : {s1, s2}) {
        source->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b3->guid), -6.0f}}}};
    }
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 3u);
    ASSERT_EQ(graph.paths.front().identity.sourceGuid, s1->guid);
    const std::set<QUuid> expectedNodes{s1->guid, layer->guid, b1->guid, b3->guid, master->guid};
    for (const auto kind : {SignalPathEdgeKind::Output, SignalPathEdgeKind::Send}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto* target = kind == SignalPathEdgeKind::Output ? b1 : b3;
        std::vector<size_t> parallel;
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& entry = graph.edges[index];
            if (entry.kind == kind && graph.nodes[entry.from.occurrenceIndex].key.objectGuid == layer->guid
                && graph.nodes[entry.to.occurrenceIndex].key.objectGuid == target->guid)
                parallel.push_back(index);
        }
        ASSERT_EQ(parallel.size(), 2u);
        const auto selected = std::find_if(parallel.begin(), parallel.end(), [&](size_t index) {
            return graph.edges[index].pathIndices == std::vector<size_t>{0};
        });
        ASSERT_NE(selected, parallel.end());
        const auto focus = projectSignalPathFocus(graph, {}, {*selected});
        ASSERT_TRUE(focus.active);
        ASSERT_EQ(focus.nodes.size(), graph.nodes.size());
        ASSERT_EQ(focus.edges.size(), graph.edges.size());
        for (size_t index = 0; index < graph.nodes.size(); ++index)
            EXPECT_EQ(focus.nodes[index], expectedNodes.contains(graph.nodes[index].key.objectGuid));
        const auto& expectedEdges = graph.paths.front().edgeIndices;
        for (size_t index = 0; index < graph.edges.size(); ++index)
            EXPECT_EQ(focus.edges[index], std::find(expectedEdges.begin(), expectedEdges.end(), index) != expectedEdges.end());
        for (const auto index : parallel) EXPECT_EQ(focus.edges[index], index == *selected);
    }
}

TEST_F(SignalPathFocusTest, MultipleSelectionUsesTheUnionAndClearingSelectionDisablesFocus)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    const auto first = projectSignalPathFocus(graph, {indexOf(graph, s1)});
    const auto third = projectSignalPathFocus(graph, {indexOf(graph, s3)});
    const auto combined = projectSignalPathFocus(graph, {indexOf(graph, s1), indexOf(graph, s3)});
    ASSERT_TRUE(combined.active);
    ASSERT_EQ(combined.nodes.size(), graph.nodes.size());
    ASSERT_EQ(combined.edges.size(), graph.edges.size());
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        EXPECT_EQ(combined.nodes[index], first.nodes[index] || third.nodes[index]);
    for (size_t index = 0; index < graph.edges.size(); ++index)
        EXPECT_EQ(combined.edges[index], first.edges[index] || third.edges[index]);
    EXPECT_TRUE(combined.nodes[indexOf(graph, s1)]);
    EXPECT_TRUE(combined.nodes[indexOf(graph, s3)]);
    EXPECT_FALSE(combined.nodes[indexOf(graph, s2)]);
    EXPECT_FALSE(combined.nodes[indexOf(graph, b2)]);
    const auto cleared = projectSignalPathFocus(graph, {});
    EXPECT_FALSE(cleared.active);
    EXPECT_EQ(cleared.nodes.size(), graph.nodes.size());
    EXPECT_EQ(cleared.edges.size(), graph.edges.size());
    EXPECT_TRUE(std::none_of(cleared.nodes.begin(), cleared.nodes.end(), [](bool focused) { return focused; }));
    EXPECT_TRUE(std::none_of(cleared.edges.begin(), cleared.edges.end(), [](bool focused) { return focused; }));
}

TEST_F(SignalPathFocusTest, SharedSoundAndHierarchyConnectionFocusEveryClipPath)
{
    auto* track = create("Track", T::Track, layer);
    auto* clipped = create("Clipped", T::Sound, track);
    output(clipped, b1);
    std::vector<PlayablePropertyLocatorV1> uses;
    auto& ledger = track->comp<VTrack>()->clips;
    for (int index = 0; index < 2; ++index) {
        Clip clip{QUuid::createUuid(), clipped->guid, index, 1};
        clip.stableOccurrenceOrdinal = static_cast<uint32_t>(ledger.occurrenceOrdinalHighWater++);
        ledger.items.push_back(clip);
        const auto locator = PlayablePropertyAuthoringV1::trackClipLocator(*track, clip.clipGuid);
        ASSERT_TRUE(locator.has_value());
        uses.push_back(*locator);
    }
    const auto graph = buildSignalPathGraph(project,
        {{clipped->guid, {uses[0]}}, {clipped->guid, {uses[1]}}, {s2->guid, {}}});
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(std::count_if(graph.nodes.begin(), graph.nodes.end(),
        [&](const auto& node) { return node.key.objectGuid == clipped->guid; }), 1);
    const auto focus = projectSignalPathFocus(graph, {indexOf(graph, clipped)});
    expectFocus(graph, focus, {clipped->guid, track->guid, layer->guid, b1->guid, master->guid}, {
        edge(clipped, track, SignalPathEdgeKind::Through),
        edge(track, layer, SignalPathEdgeKind::Through),
        edge(layer, b1, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output)
    });
    const auto through = edgeIndex(graph, clipped, track, SignalPathEdgeKind::Through);
    ASSERT_LT(through, graph.edges.size());
    EXPECT_EQ(graph.edges[through].pathIndices, (std::vector<size_t>{0, 1}));
    const auto selectedConnection = projectSignalPathFocus(graph, {}, {through});
    ASSERT_TRUE(selectedConnection.active);
    EXPECT_TRUE(selectedConnection.nodes[indexOf(graph, clipped)]);
    EXPECT_TRUE(selectedConnection.nodes[indexOf(graph, track)]);
    EXPECT_TRUE(selectedConnection.nodes[indexOf(graph, layer)]);
    EXPECT_TRUE(selectedConnection.nodes[indexOf(graph, b1)]);
    EXPECT_TRUE(selectedConnection.nodes[indexOf(graph, master)]);
    EXPECT_FALSE(selectedConnection.nodes[indexOf(graph, s2)]);
    EXPECT_TRUE(selectedConnection.edges[through]);
    EXPECT_EQ(selectedConnection.nodes, focus.nodes);
    EXPECT_EQ(selectedConnection.edges, focus.edges);
}

TEST_F(SignalPathFocusTest, NodeAndConnectionMultiSelectionUsesUnionAndClearsCompletely)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    const auto first = edgeIndex(graph, s1, layer, SignalPathEdgeKind::Through);
    const auto third = edgeIndex(graph, s3, otherLayer, SignalPathEdgeKind::Through);
    const auto combined = projectSignalPathFocus(graph, {indexOf(graph, s1)}, {first, third});
    expectFocus(graph, combined, {s1->guid, s3->guid, layer->guid, otherLayer->guid,
        b1->guid, b3->guid, master->guid}, {
        edge(s1, layer, SignalPathEdgeKind::Through),
        edge(s3, otherLayer, SignalPathEdgeKind::Through),
        edge(layer, b1, SignalPathEdgeKind::Output),
        edge(otherLayer, b3, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output),
        edge(b3, master, SignalPathEdgeKind::Output)
    });
    const auto onlyConnections = projectSignalPathFocus(graph, {}, {first, third});
    EXPECT_TRUE(onlyConnections.active);
    EXPECT_EQ(onlyConnections.nodes, combined.nodes);
    EXPECT_EQ(onlyConnections.edges, combined.edges);
    const auto cleared = projectSignalPathFocus(graph, {}, {});
    EXPECT_FALSE(cleared.active);
    EXPECT_TRUE(std::none_of(cleared.nodes.begin(), cleared.nodes.end(), [](bool focused) { return focused; }));
    EXPECT_TRUE(std::none_of(cleared.edges.begin(), cleared.edges.end(), [](bool focused) { return focused; }));
    const auto invalidSelection = projectSignalPathFocus(graph, {graph.nodes.size()}, {graph.edges.size()});
    EXPECT_FALSE(invalidSelection.active);
    EXPECT_EQ(invalidSelection.nodes, cleared.nodes);
    EXPECT_EQ(invalidSelection.edges, cleared.edges);
}

TEST_F(SignalPathFocusTest, BusOnlyFocusIncludesUpstreamAndDownstreamWithoutCrossingIntoMasterSiblings)
{
    auto* child = create("B1 Child", T::Bus, b1);
    b1->comp<VMixNode>()->sends.push_back({b3->objectID, 0.5f});
    const auto graph = buildSignalPathGraph(project, {}, {}, {master->objectID});
    ASSERT_TRUE(graph.ok());
    ASSERT_TRUE(graph.paths.empty());
    EXPECT_TRUE(std::all_of(graph.nodes.begin(), graph.nodes.end(),
        [](const auto& node) { return node.type == T::Bus; }));
    const auto focus = projectSignalPathFocus(graph, {indexOf(graph, b1)});
    expectFocus(graph, focus, {child->guid, b1->guid, b3->guid, master->guid}, {
        edge(child, b1, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output),
        edge(b1, b3, SignalPathEdgeKind::Send),
        edge(b3, master, SignalPathEdgeKind::Output)
    });
    EXPECT_FALSE(focus.nodes[indexOf(graph, b2)]);
}

TEST_F(SignalPathFocusTest, BusOnlyConnectionFocusExpandsInEachDirectionWithoutOtherEndpointBranches)
{
    auto* child = create("B1 Child", T::Bus, b1);
    b1->comp<VMixNode>()->sends.push_back({b3->objectID, 0.5f});
    b2->comp<VMixNode>()->sends.push_back({b3->objectID, 0.25f});
    const auto graph = buildSignalPathGraph(project, {}, {}, {master->objectID});
    ASSERT_TRUE(graph.ok());
    ASSERT_TRUE(graph.paths.empty());
    const auto output = edgeIndex(graph, b1, master, SignalPathEdgeKind::Output);
    const auto send = edgeIndex(graph, b1, b3, SignalPathEdgeKind::Send);
    const auto outputFocus = projectSignalPathFocus(graph, {}, {output});
    expectFocus(graph, outputFocus, {child->guid, b1->guid, master->guid}, {
        edge(child, b1, SignalPathEdgeKind::Output),
        edge(b1, master, SignalPathEdgeKind::Output)
    });
    const auto sendFocus = projectSignalPathFocus(graph, {}, {send});
    expectFocus(graph, sendFocus, {child->guid, b1->guid, b3->guid, master->guid}, {
        edge(child, b1, SignalPathEdgeKind::Output),
        edge(b1, b3, SignalPathEdgeKind::Send),
        edge(b3, master, SignalPathEdgeKind::Output)
    });
    const auto combined = projectSignalPathFocus(graph, {}, {output, send});
    EXPECT_TRUE(combined.active);
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        EXPECT_EQ(combined.nodes[index], outputFocus.nodes[index] || sendFocus.nodes[index]);
    for (size_t index = 0; index < graph.edges.size(); ++index)
        EXPECT_EQ(combined.edges[index], outputFocus.edges[index] || sendFocus.edges[index]);
}

}
