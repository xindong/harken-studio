#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Blueprint/SignalPathGraph.h"

#include "Core/Components/VMixNode.h"
#include "Core/Components/VTrack.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <set>

namespace {

using namespace VNX;
using T = ProjectItemType;

class SignalPathGraphTest : public ::testing::Test
{
protected:
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    uint32_t nextID = 1;
    ProjectItem* master = create("Master", T::Bus);
    ProjectItem* b1 = create("B1", T::Bus, master);
    ProjectItem* b2 = create("B2", T::Bus, master);
    ProjectItem* b3 = create("B3", T::Bus, master);
    ProjectItem* l3 = create("L3", T::Layer);
    ProjectItem* l1 = create("L1", T::Layer, l3);
    ProjectItem* p1 = create("P1", T::Piece, l1);
    ProjectItem* t1 = create("T1", T::Track, p1);
    ProjectItem* s1 = create("S1", T::Sound, t1);
    ProjectItem* s2 = create("S2", T::Sound, t1);
    ProjectItem* t2 = create("T2", T::Track, p1);
    ProjectItem* s3 = create("S3", T::Sound, t2);
    ProjectItem* c1 = create("C1", T::Container, l1);
    ProjectItem* s4 = create("S4", T::Sound, c1);
    ProjectItem* s5 = create("S5", T::Sound, c1);
    ProjectItem* l2 = create("L2", T::Layer, l3);
    ProjectItem* t3 = create("T3", T::Track, l2);
    ProjectItem* s6 = create("S6", T::Sound, t3);
    std::array<ProjectItem*, 6> sounds{s1, s2, s3, s4, s5, s6};

    void SetUp() override
    {
        output(p1, b1);
        output(c1, b1);
        output(s5, b3);
        output(l2, b1);
        send(c1, {{toFieldGroupGuid(b2->guid), -6.0f}});
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

    void output(ProjectItem* item, ProjectItem* bus)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), 0.0f}};
    }

    void send(ProjectItem* item, std::vector<SendFieldGroupRow> rows)
    {
        item->fieldGroups.send.shared = {FieldGroupMode::Local, SendFieldGroup{std::move(rows)}};
    }

    std::vector<SignalPathIdentity> inputs() const
    {
        std::vector<SignalPathIdentity> result;
        for (const auto* sound : sounds) result.push_back({sound->guid, {}});
        return result;
    }

    SignalPathGraph build(FieldGroupScope platform = {}) const
    {
        return buildSignalPathGraph(project, inputs(), platform);
    }

    PlayablePropertyLocatorV1 place(ProjectItem* track, ProjectItem* target)
    {
        Clip clip{QUuid::createUuid(), target->guid, 0, 1};
        auto& ledger = track->comp<VTrack>()->clips;
        clip.stableOccurrenceOrdinal = static_cast<uint32_t>(ledger.occurrenceOrdinalHighWater++);
        ledger.items.push_back(clip);
        return *PlayablePropertyAuthoringV1::trackClipLocator(*track, clip.clipGuid);
    }
};

std::vector<size_t> occurrences(const SignalPathGraph& graph, const ProjectItem* item)
{
    std::vector<size_t> result;
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index].key.objectGuid == item->guid) result.push_back(index);
    }
    return result;
}

QStringList computationNames(const SignalPathGraph& graph, size_t pathIndex)
{
    QStringList result;
    for (const auto endpoint : graph.paths[pathIndex].computation)
        result.push_back(graph.nodes[endpoint.occurrenceIndex].name);
    return result;
}

SignalPathEndpoint endpointFor(const SignalPathGraph& graph, size_t pathIndex,
                               const ProjectItem* item)
{
    const auto& computation = graph.paths.at(pathIndex).computation;
    const auto found = std::find_if(computation.begin(), computation.end(), [&](auto endpoint) {
        return graph.nodes.at(endpoint.occurrenceIndex).key.objectGuid == item->guid;
    });
    EXPECT_NE(found, computation.end());
    return found == computation.end() ? SignalPathEndpoint{} : *found;
}

std::vector<const SignalPathEdge*> edgesBetween(const SignalPathGraph& graph,
    const ProjectItem* from, const ProjectItem* to)
{
    std::vector<const SignalPathEdge*> result;
    for (const auto& edge : graph.edges)
        if (graph.nodes[edge.from.occurrenceIndex].key.objectGuid == from->guid
            && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == to->guid)
            result.push_back(&edge);
    return result;
}

void expectIntegrity(const SignalPathGraph& graph)
{
    ASSERT_TRUE(graph.ok());
    std::set<QUuid> objects;
    for (const auto& node : graph.nodes) {
        EXPECT_TRUE(objects.insert(node.key.objectGuid).second);
        ASSERT_EQ(node.portPairs.size(), 1u);
        const auto& pair = node.portPairs.front();
        if (node.type == T::Bus || pair.pathIndices.empty()) {
            EXPECT_FALSE(pair.routing);
        } else {
            const auto& first = graph.paths[pair.pathIndices.front()].routing;
            const bool same = std::all_of(pair.pathIndices.begin(), pair.pathIndices.end(),
                [&](size_t index) { return graph.paths[index].routing == first; });
            EXPECT_EQ(pair.routing, same ? std::make_optional(first) : std::nullopt);
        }
    }
    for (size_t index = 0; index < graph.paths.size(); ++index) {
        const auto& path = graph.paths[index];
        ASSERT_FALSE(path.computation.empty());
        EXPECT_EQ(graph.nodes.at(path.computation.front().occurrenceIndex).key.objectGuid,
                  path.identity.sourceGuid);
        for (auto endpoint : path.computation) {
            const auto& pair = graph.nodes.at(endpoint.occurrenceIndex).portPairs.at(endpoint.portPairIndex);
            EXPECT_EQ(endpoint.portPairIndex, 0u);
            if (pair.routing) EXPECT_EQ(pair.routing, path.routing);
            EXPECT_NE(std::find(pair.pathIndices.begin(), pair.pathIndices.end(), index),
                      pair.pathIndices.end());
        }
        for (size_t step = 1; step < path.computation.size(); ++step) {
            EXPECT_TRUE(std::any_of(path.edgeIndices.begin(), path.edgeIndices.end(), [&](size_t edgeIndex) {
                const auto& edge = graph.edges.at(edgeIndex);
                return edge.from == path.computation[step - 1] && edge.to == path.computation[step]
                    && edge.kind == SignalPathEdgeKind::Through;
            }));
        }
        for (size_t edgeIndex : path.edgeIndices) {
            const auto& edge = graph.edges.at(edgeIndex);
            EXPECT_NE(std::find(edge.pathIndices.begin(), edge.pathIndices.end(), index),
                      edge.pathIndices.end());
        }
    }
    std::set<std::pair<size_t, size_t>> hierarchy;
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        ASSERT_FALSE(edge.pathIndices.empty());
        EXPECT_EQ(edge.routingSource.has_value(), edge.kind != SignalPathEdgeKind::Through);
        if (edge.kind == SignalPathEdgeKind::Through)
            EXPECT_TRUE(hierarchy.emplace(edge.from.occurrenceIndex, edge.to.occurrenceIndex).second);
        if (edge.routingSource) {
            EXPECT_FALSE(edge.routingSource->objectGuid.isNull());
            if (graph.nodes[edge.from.occurrenceIndex].type == T::Bus) {
                EXPECT_EQ(edge.routingSource->objectGuid,
                          graph.nodes[edge.from.occurrenceIndex].key.objectGuid);
                EXPECT_FALSE(edge.routingSource->scope);
            }
        }
        for (size_t pathIndex : edge.pathIndices) {
            const auto& pathEdges = graph.paths.at(pathIndex).edgeIndices;
            EXPECT_NE(std::find(pathEdges.begin(), pathEdges.end(), index), pathEdges.end());
            for (const auto endpoint : {edge.from, edge.to}) {
                const auto& pair = graph.nodes.at(endpoint.occurrenceIndex).portPairs.at(endpoint.portPairIndex);
                EXPECT_NE(std::find(pair.pathIndices.begin(), pair.pathIndices.end(), pathIndex),
                          pair.pathIndices.end());
            }
        }
    }
}

TEST_F(SignalPathGraphTest, PrototypeResolvesSixCompleteSoundPaths)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 6u);
    EXPECT_EQ(graph.nodes.size(), 18u);
    EXPECT_EQ(graph.edges.size(), 21u);
    const std::array<QStringList, 6> expected{{
        {"S1", "T1", "P1", "L1", "L3"},
        {"S2", "T1", "P1", "L1", "L3"},
        {"S3", "T2", "P1", "L1", "L3"},
        {"S4", "C1", "L1", "L3"},
        {"S5", "C1", "L1", "L3"},
        {"S6", "T3", "L2", "L3"}
    }};
    for (size_t index = 0; index < sounds.size(); ++index) {
        EXPECT_EQ(computationNames(graph, index), expected[index]);
        const auto& routing = graph.paths[index].routing;
        EXPECT_EQ(routing.outputBusID, index == 4 ? b3->objectID : b1->objectID);
        if (index == 3 || index == 4) {
            ASSERT_EQ(routing.sends.size(), 1u);
            EXPECT_EQ(routing.sends[0].targetBusID, b2->objectID);
            EXPECT_FLOAT_EQ(routing.sends[0].sendLevel, VMixNode::dbToLinear(-6.0f));
        } else {
            EXPECT_TRUE(routing.sends.empty());
        }
    }
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, PrototypeKeepsOnePortPerObjectAndPreservesPathMembership)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(occurrences(graph, t1).size(), 1u);
    const auto t1Endpoint = endpointFor(graph, 0, t1);
    EXPECT_EQ(t1Endpoint, endpointFor(graph, 1, t1));
    EXPECT_EQ(graph.nodes[t1Endpoint.occurrenceIndex].portPairs.size(), 1u);
    const auto p1Endpoint = endpointFor(graph, 0, p1);
    EXPECT_EQ(p1Endpoint, endpointFor(graph, 1, p1));
    EXPECT_EQ(p1Endpoint, endpointFor(graph, 2, p1));
    ASSERT_EQ(occurrences(graph, c1).size(), 1u);
    const auto a = endpointFor(graph, 3, c1);
    const auto b = endpointFor(graph, 4, c1);
    EXPECT_EQ(a.occurrenceIndex, b.occurrenceIndex);
    EXPECT_EQ(a.portPairIndex, b.portPairIndex);
    const auto& pairs = graph.nodes[a.occurrenceIndex].portPairs;
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_FALSE(pairs.front().routing);
    EXPECT_EQ(pairs.front().pathIndices, (std::vector<size_t>{3, 4}));
    for (const auto& edge : graph.edges) {
        if (edge.from == a) EXPECT_EQ(edge.pathIndices, (std::vector<size_t>{3, 4}));
    }
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, PrototypeSharesLayerObjectsAcrossEveryContentBranch)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(occurrences(graph, l1).size(), 1u);
    EXPECT_EQ(occurrences(graph, l3).size(), 1u);
    EXPECT_EQ(endpointFor(graph, 0, l1), endpointFor(graph, 2, l1));
    EXPECT_EQ(endpointFor(graph, 0, l1), endpointFor(graph, 3, l1));
    EXPECT_EQ(endpointFor(graph, 3, l1), endpointFor(graph, 4, l1));
    for (const auto* layer : {l1, l3}) {
        for (size_t index : occurrences(graph, layer)) {
            EXPECT_EQ(graph.nodes[index].objectID, layer->objectID);
            EXPECT_EQ(graph.nodes[index].type, T::Layer);
            EXPECT_EQ(graph.nodes[index].portPairs.size(), 1u);
        }
    }
}

TEST_F(SignalPathGraphTest, OutputAndSendLeaveTheSoundComputationTerminal)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    for (size_t index = 0; index < graph.paths.size(); ++index) {
        const auto& path = graph.paths[index];
        size_t outputCount = 0;
        size_t sendCount = 0;
        for (size_t edgeIndex : path.edgeIndices) {
            const auto& edge = graph.edges[edgeIndex];
            if (edge.from != path.computation.back()) continue;
            const auto target = graph.nodes[edge.to.occurrenceIndex].objectID;
            if (edge.kind == SignalPathEdgeKind::Output) {
                EXPECT_EQ(target, path.routing.outputBusID);
                ++outputCount;
            } else if (edge.kind == SignalPathEdgeKind::Send) {
                EXPECT_EQ(target, b2->objectID);
                EXPECT_FLOAT_EQ(edge.sendLevel, path.routing.sends.front().sendLevel);
                ++sendCount;
            }
        }
        EXPECT_EQ(outputCount, 1u);
        EXPECT_EQ(sendCount, path.routing.sends.size());
    }
    for (const auto& edge : graph.edges) {
        if (edge.kind == SignalPathEdgeKind::Send)
            EXPECT_EQ(graph.nodes[edge.from.occurrenceIndex].type, T::Layer);
        if (graph.nodes[edge.from.occurrenceIndex].type == T::Bus) {
            EXPECT_EQ(edge.kind, SignalPathEdgeKind::Output);
            EXPECT_EQ(graph.nodes[edge.to.occurrenceIndex].objectID, master->objectID);
        }
    }
}

TEST_F(SignalPathGraphTest, SharedTerminalKeepsEachSourcesOutputAndSendMembershipSeparate)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    const auto main = edgesBetween(graph, l3, b1);
    const auto other = edgesBetween(graph, l3, b3);
    const auto sends = edgesBetween(graph, l3, b2);
    ASSERT_EQ(main.size(), 3u);
    ASSERT_EQ(other.size(), 1u);
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_EQ(main[0]->pathIndices, (std::vector<size_t>{0, 1, 2}));
    EXPECT_EQ(main[1]->pathIndices, (std::vector<size_t>{3}));
    EXPECT_EQ(main[2]->pathIndices, (std::vector<size_t>{5}));
    EXPECT_EQ(main[0]->routingSource, (SignalPathRoutingSource{p1->guid, {}}));
    EXPECT_EQ(main[1]->routingSource, (SignalPathRoutingSource{c1->guid, {}}));
    EXPECT_EQ(main[2]->routingSource, (SignalPathRoutingSource{l2->guid, {}}));
    EXPECT_EQ(other.front()->pathIndices, (std::vector<size_t>{4}));
    EXPECT_EQ(other.front()->routingSource, (SignalPathRoutingSource{s5->guid, {}}));
    EXPECT_EQ(sends.front()->pathIndices, (std::vector<size_t>{3, 4}));
    EXPECT_EQ(sends.front()->routingSource, (SignalPathRoutingSource{c1->guid, {}}));
    for (const auto index : graph.paths.front().edgeIndices) {
        EXPECT_NE(&graph.edges[index], main[1]);
        EXPECT_NE(&graph.edges[index], main[2]);
        EXPECT_NE(&graph.edges[index], other.front());
        EXPECT_NE(&graph.edges[index], sends.front());
    }
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, ReturningOutputToInheritRestoresCommonRouteWithoutReplacingNodes)
{
    s5->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Inherit;
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(graph.paths[4].routing, graph.paths[3].routing);
    EXPECT_EQ(endpointFor(graph, 3, c1), endpointFor(graph, 4, c1));
    EXPECT_EQ(endpointFor(graph, 3, l3), endpointFor(graph, 4, l3));
    EXPECT_EQ(occurrences(graph, l1).size(), 1u);
    EXPECT_EQ(occurrences(graph, l3).size(), 1u);
    EXPECT_EQ(graph.nodes[endpointFor(graph, 3, c1).occurrenceIndex].portPairs.front().routing,
              graph.paths[3].routing);
    const auto outputs = edgesBetween(graph, l3, b1);
    ASSERT_EQ(outputs.size(), 3u);
    EXPECT_EQ(outputs[1]->routingSource, (SignalPathRoutingSource{c1->guid, {}}));
    EXPECT_EQ(outputs[1]->pathIndices, (std::vector<size_t>{3, 4}));
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, LocalEmptySendRemovesOnlyThatSoundsBranch)
{
    send(s5, {});
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(graph.paths[4].routing.outputBusID, b3->objectID);
    EXPECT_TRUE(graph.paths[4].routing.sends.empty());
    ASSERT_EQ(graph.paths[3].routing.sends.size(), 1u);
    EXPECT_EQ(graph.paths[3].routing.sends.front().targetBusID, b2->objectID);
    EXPECT_EQ(std::count_if(graph.edges.begin(), graph.edges.end(), [](const auto& edge) {
        return edge.kind == SignalPathEdgeKind::Send;
    }), 1);
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, EqualOutputDoesNotEraseDifferentSendTargetsOrLevels)
{
    output(s5, b1);
    send(s5, {{toFieldGroupGuid(b3->guid), -6.0f}});
    auto graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(endpointFor(graph, 3, c1), endpointFor(graph, 4, c1));
    EXPECT_FALSE(graph.nodes[endpointFor(graph, 3, c1).occurrenceIndex].portPairs.front().routing);
    EXPECT_EQ(graph.paths[4].routing.sends.front().targetBusID, b3->objectID);
    send(s5, {{toFieldGroupGuid(b2->guid), -3.0f}});
    graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(endpointFor(graph, 3, c1), endpointFor(graph, 4, c1));
    EXPECT_FALSE(graph.nodes[endpointFor(graph, 3, c1).occurrenceIndex].portPairs.front().routing);
    EXPECT_FLOAT_EQ(graph.paths[4].routing.sends.front().sendLevel, VMixNode::dbToLinear(-3.0f));
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, IdenticalSendRowsFromDifferentProvidersStaySeparate)
{
    output(s5, b1);
    send(s4, {{toFieldGroupGuid(b2->guid), -6.0f}, {toFieldGroupGuid(b3->guid), -3.0f}});
    send(s5, {{toFieldGroupGuid(b3->guid), -3.0f}, {toFieldGroupGuid(b2->guid), -6.0f}});
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(endpointFor(graph, 3, c1), endpointFor(graph, 4, c1));
    for (const auto* target : {b2, b3}) {
        const auto sends = edgesBetween(graph, l3, target);
        ASSERT_EQ(sends.size(), 2u);
        EXPECT_EQ(sends[0]->routingSource, (SignalPathRoutingSource{s4->guid, {}}));
        EXPECT_EQ(sends[0]->pathIndices, (std::vector<size_t>{3}));
        EXPECT_EQ(sends[1]->routingSource, (SignalPathRoutingSource{s5->guid, {}}));
        EXPECT_EQ(sends[1]->pathIndices, (std::vector<size_t>{4}));
        EXPECT_FLOAT_EQ(sends[0]->sendLevel, sends[1]->sendLevel);
    }
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, ReturningIdenticalSendToInheritSharesOnlyItsProviderRelationship)
{
    send(s5, {{toFieldGroupGuid(b2->guid), -6.0f}});
    const auto local = build();
    ASSERT_TRUE(local.ok());
    ASSERT_EQ(edgesBetween(local, l3, b2).size(), 2u);
    s5->fieldGroups.send.shared.mode = FieldGroupMode::Inherit;
    const auto inherited = build();
    ASSERT_TRUE(inherited.ok());
    EXPECT_EQ(inherited.nodes.size(), local.nodes.size());
    const auto sends = edgesBetween(inherited, l3, b2);
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_EQ(sends.front()->routingSource, (SignalPathRoutingSource{c1->guid, {}}));
    EXPECT_EQ(sends.front()->pathIndices, (std::vector<size_t>{3, 4}));
    expectIntegrity(inherited);
}

TEST_F(SignalPathGraphTest, PlatformResolutionRebuildsEffectivePaths)
{
    p1->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(b3->guid), 0.0f}};
    c1->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{}};
    const auto shared = build();
    const auto android = build(HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(shared.ok());
    ASSERT_TRUE(android.ok());
    EXPECT_EQ(shared.paths[0].routing.outputBusID, b1->objectID);
    EXPECT_EQ(android.paths[0].routing.outputBusID, b3->objectID);
    EXPECT_EQ(android.paths[4].routing.outputBusID, b3->objectID);
    EXPECT_FALSE(shared.paths[3].routing.sends.empty());
    EXPECT_TRUE(android.paths[3].routing.sends.empty());
    EXPECT_EQ(android.platform, HarkenTest::FieldGroupPlatformIds::Android);
    const auto sharedOutput = edgesBetween(shared, l3, b1);
    ASSERT_EQ(sharedOutput.size(), 3u);
    EXPECT_EQ(sharedOutput.front()->routingSource, (SignalPathRoutingSource{p1->guid, {}}));
    const auto androidOutput = edgesBetween(android, l3, b3);
    ASSERT_EQ(androidOutput.size(), 2u);
    EXPECT_EQ(androidOutput[0]->routingSource,
              (SignalPathRoutingSource{p1->guid, HarkenTest::FieldGroupPlatformIds::Android}));
    EXPECT_EQ(androidOutput[0]->pathIndices, (std::vector<size_t>{0, 1, 2}));
    EXPECT_EQ(androidOutput[1]->routingSource, (SignalPathRoutingSource{s5->guid, {}}));
    EXPECT_EQ(androidOutput[1]->pathIndices, (std::vector<size_t>{4}));
    expectIntegrity(android);
}

TEST_F(SignalPathGraphTest, RepeatedClipUsesKeepPathIdentityAndShareOneHierarchyConnection)
{
    const auto first = place(t1, s1);
    const auto second = place(t1, s1);
    output(s1, b3);
    send(t1, {{toFieldGroupGuid(b2->guid), -6.0f}});
    const auto graph = buildSignalPathGraph(project, {
        {s1->guid, {first}}, {s1->guid, {second}}});
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 2u);
    EXPECT_NE(graph.paths[0].identity, graph.paths[1].identity);
    EXPECT_EQ(graph.paths[0].identity.occurrences, (std::vector<PlayablePropertyLocatorV1>{first}));
    EXPECT_EQ(graph.paths[1].identity.occurrences, (std::vector<PlayablePropertyLocatorV1>{second}));
    EXPECT_EQ(occurrences(graph, s1).size(), 1u);
    EXPECT_EQ(graph.paths[0].computation.front(), graph.paths[1].computation.front());
    EXPECT_EQ(endpointFor(graph, 0, t1), endpointFor(graph, 1, t1));
    const auto relationships = edgesBetween(graph, s1, t1);
    ASSERT_EQ(relationships.size(), 1u);
    EXPECT_EQ(relationships.front()->kind, SignalPathEdgeKind::Through);
    EXPECT_EQ(relationships.front()->pathIndices, (std::vector<size_t>{0, 1}));
    EXPECT_EQ(graph.paths[0].edgeIndices, graph.paths[1].edgeIndices);
    EXPECT_EQ(computationNames(graph, 0), (QStringList{"S1", "T1", "P1", "L1", "L3"}));
    for (const auto& path : graph.paths) {
        EXPECT_EQ(path.routing.outputBusID, b3->objectID);
        ASSERT_EQ(path.routing.sends.size(), 1u);
        EXPECT_EQ(path.routing.sends.front().targetBusID, b2->objectID);
    }
    const auto outputs = edgesBetween(graph, l3, b3);
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front()->routingSource, (SignalPathRoutingSource{s1->guid, {}}));
    EXPECT_EQ(outputs.front()->pathIndices, (std::vector<size_t>{0, 1}));
    const auto sends = edgesBetween(graph, l3, b2);
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_EQ(sends.front()->routingSource, (SignalPathRoutingSource{t1->guid, {}}));
    EXPECT_EQ(sends.front()->pathIndices, (std::vector<size_t>{0, 1}));
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, NestedPlacementSharesEveryHierarchyConnectionAcrossOuterClips)
{
    auto* piece = create("InnerPiece", T::Piece, t1);
    auto* track = create("InnerTrack", T::Track, piece);
    auto* sound = create("InnerSound", T::Sound, track);
    const auto inner = place(track, sound);
    const auto outerA = place(t1, piece);
    const auto outerB = place(t1, piece);
    const auto graph = buildSignalPathGraph(project, {
        {sound->guid, {inner, outerA}}, {sound->guid, {inner, outerB}}});
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 2u);
    EXPECT_EQ(graph.paths[0].identity.occurrences, (std::vector<PlayablePropertyLocatorV1>{inner, outerA}));
    EXPECT_EQ(graph.paths[1].identity.occurrences, (std::vector<PlayablePropertyLocatorV1>{inner, outerB}));
    EXPECT_EQ(occurrences(graph, sound).size(), 1u);
    EXPECT_EQ(occurrences(graph, track).size(), 1u);
    EXPECT_EQ(occurrences(graph, piece).size(), 1u);
    EXPECT_EQ(occurrences(graph, t1).size(), 1u);
    EXPECT_EQ(computationNames(graph, 0),
              (QStringList{"InnerSound", "InnerTrack", "InnerPiece", "T1", "P1", "L1", "L3"}));
    EXPECT_EQ(graph.paths[0].routing.outputBusID, b1->objectID);
    const auto internal = edgesBetween(graph, sound, track);
    ASSERT_EQ(internal.size(), 1u);
    EXPECT_EQ(internal.front()->pathIndices, (std::vector<size_t>{0, 1}));
    const auto outer = edgesBetween(graph, piece, t1);
    ASSERT_EQ(outer.size(), 1u);
    EXPECT_EQ(outer.front()->pathIndices, (std::vector<size_t>{0, 1}));
    EXPECT_EQ(graph.paths[0].edgeIndices, graph.paths[1].edgeIndices);
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, PlacedContainerPreservesItsDescendantSoundsAndInheritedSends)
{
    auto* container = create("PlacedContainer", T::Container, t1);
    auto* a = create("A", T::Sound, container);
    auto* b = create("B", T::Sound, container);
    send(container, {{toFieldGroupGuid(b2->guid), -6.0f}});
    output(b, b3);
    const auto first = place(t1, container);
    const auto second = place(t1, container);
    const auto graph = buildSignalPathGraph(project, {
        {a->guid, {first}}, {b->guid, {first}},
        {a->guid, {second}}, {b->guid, {second}}});
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 4u);
    ASSERT_EQ(occurrences(graph, container).size(), 1u);
    for (size_t index : occurrences(graph, container))
        EXPECT_EQ(graph.nodes[index].portPairs.size(), 1u);
    EXPECT_EQ(occurrences(graph, t1).size(), 1u);
    EXPECT_EQ(endpointFor(graph, 0, t1), endpointFor(graph, 2, t1));
    EXPECT_EQ(endpointFor(graph, 1, t1), endpointFor(graph, 3, t1));
    EXPECT_EQ(graph.paths[0].routing.outputBusID, b1->objectID);
    EXPECT_EQ(graph.paths[1].routing.outputBusID, b3->objectID);
    EXPECT_EQ(graph.paths[0].routing.sends, graph.paths[1].routing.sends);
    const auto firstChild = edgesBetween(graph, a, container);
    const auto secondChild = edgesBetween(graph, b, container);
    ASSERT_EQ(firstChild.size(), 1u);
    ASSERT_EQ(secondChild.size(), 1u);
    EXPECT_EQ(firstChild.front()->pathIndices, (std::vector<size_t>{0, 2}));
    EXPECT_EQ(secondChild.front()->pathIndices, (std::vector<size_t>{1, 3}));
    const auto relationships = edgesBetween(graph, container, t1);
    ASSERT_EQ(relationships.size(), 1u);
    EXPECT_EQ(relationships.front()->pathIndices, (std::vector<size_t>{0, 1, 2, 3}));
    EXPECT_EQ(graph.paths[0].edgeIndices, graph.paths[2].edgeIndices);
    EXPECT_EQ(graph.paths[1].edgeIndices, graph.paths[3].edgeIndices);
    EXPECT_EQ(computationNames(graph, 0), (QStringList{"A", "PlacedContainer", "T1", "P1", "L1", "L3"}));
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, RepeatedContainerChildrenKeepPathOrdinalsAndShareOneHierarchyConnection)
{
    SelectionEntry first;
    SelectionEntry second;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*c1, s4->guid, {}, &first));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*c1, s4->guid, {}, &second));
    const auto a = PlayablePropertyAuthoringV1::containerChildLocator(*c1, first.stableOccurrenceOrdinal);
    const auto b = PlayablePropertyAuthoringV1::containerChildLocator(*c1, second.stableOccurrenceOrdinal);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    const auto graph = buildSignalPathGraph(project, {{s4->guid, {*a}}, {s4->guid, {*b}}});
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 2u);
    EXPECT_NE(graph.paths[0].identity, graph.paths[1].identity);
    EXPECT_EQ(graph.paths[0].identity.occurrences, (std::vector<PlayablePropertyLocatorV1>{*a}));
    EXPECT_EQ(graph.paths[1].identity.occurrences, (std::vector<PlayablePropertyLocatorV1>{*b}));
    EXPECT_EQ(occurrences(graph, s4).size(), 1u);
    EXPECT_EQ(occurrences(graph, c1).size(), 1u);
    EXPECT_EQ(endpointFor(graph, 0, c1), endpointFor(graph, 1, c1));
    const auto relationships = edgesBetween(graph, s4, c1);
    ASSERT_EQ(relationships.size(), 1u);
    EXPECT_EQ(relationships.front()->kind, SignalPathEdgeKind::Through);
    EXPECT_EQ(relationships.front()->pathIndices, (std::vector<size_t>{0, 1}));
    EXPECT_EQ(graph.paths[0].edgeIndices, graph.paths[1].edgeIndices);
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, BusSendsContinueToMasterWithPerPathMembership)
{
    b1->comp<VMixNode>()->sends.push_back({b3->objectID, 0.25f});
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(occurrences(graph, b3).size(), 1u);
    ASSERT_EQ(occurrences(graph, master).size(), 1u);
    size_t busSendCount = 0;
    for (const auto& edge : graph.edges) {
        if (edge.kind != SignalPathEdgeKind::Send
            || graph.nodes[edge.from.occurrenceIndex].objectID != b1->objectID)
            continue;
        ++busSendCount;
        EXPECT_EQ(graph.nodes[edge.to.occurrenceIndex].objectID, b3->objectID);
        EXPECT_FLOAT_EQ(edge.sendLevel, 0.25f);
        EXPECT_EQ(edge.pathIndices, (std::vector<size_t>{0, 1, 2, 3, 5}));
        EXPECT_EQ(edge.routingSource, (SignalPathRoutingSource{b1->guid, {}}));
    }
    EXPECT_EQ(busSendCount, 1u);
    expectIntegrity(graph);
}

TEST_F(SignalPathGraphTest, BusOnlyRelationshipsKeepTheirOwnProviders)
{
    b1->comp<VMixNode>()->sends.push_back({b3->objectID, 0.25f});
    const auto graph = buildSignalPathGraph(project, {}, {}, {master->objectID});
    ASSERT_TRUE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
    EXPECT_EQ(graph.nodes.size(), 4u);
    EXPECT_EQ(graph.edges.size(), 4u);
    for (const auto& edge : graph.edges) {
        EXPECT_TRUE(edge.pathIndices.empty());
        EXPECT_EQ(edge.routingSource,
                  (SignalPathRoutingSource{graph.nodes[edge.from.occurrenceIndex].key.objectGuid, {}}));
    }
    const auto sends = edgesBetween(graph, b1, b3);
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_EQ(sends.front()->kind, SignalPathEdgeKind::Send);
    const auto outputs = edgesBetween(graph, b1, master);
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front()->kind, SignalPathEdgeKind::Output);
}

TEST_F(SignalPathGraphTest, IdentitySurvivesInputOrderAndDuplicateRequests)
{
    const auto original = build();
    auto reordered = inputs();
    std::reverse(reordered.begin(), reordered.end());
    reordered.push_back(reordered.front());
    const auto rebuilt = buildSignalPathGraph(project, reordered);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(rebuilt.ok());
    ASSERT_EQ(rebuilt.paths.size(), 6u);
    ASSERT_EQ(rebuilt.nodes.size(), original.nodes.size());
    for (const auto& node : original.nodes) {
        EXPECT_EQ(std::count_if(rebuilt.nodes.begin(), rebuilt.nodes.end(), [&](const auto& candidate) {
            return candidate.key == node.key;
        }), 1);
    }
    for (const auto& path : original.paths) {
        const auto found = std::find_if(rebuilt.paths.begin(), rebuilt.paths.end(), [&](const auto& candidate) {
            return candidate.identity == path.identity;
        });
        ASSERT_NE(found, rebuilt.paths.end());
        EXPECT_EQ(found->routing, path.routing);
    }
    expectIntegrity(rebuilt);
}

TEST_F(SignalPathGraphTest, StalePlacementDoesNotFallBackToAnOrganizationPath)
{
    const auto placement = place(t1, s1);
    t1->comp<VTrack>()->clips.items.clear();
    const auto graph = buildSignalPathGraph(project, {{s1->guid, {placement}}});
    EXPECT_FALSE(graph.ok());
    ASSERT_EQ(graph.diagnostics.size(), 1u);
    EXPECT_EQ(graph.diagnostics.front().path->sourceGuid, s1->guid);
    EXPECT_TRUE(graph.paths.empty());
    EXPECT_TRUE(graph.nodes.empty());
    EXPECT_TRUE(graph.edges.empty());
}

TEST_F(SignalPathGraphTest, RejectsNonChildAndUnrelatedPlacementTargets)
{
    const auto nonChild = place(t1, s4);
    auto graph = buildSignalPathGraph(project, {{s4->guid, {nonChild}}});
    EXPECT_FALSE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
    ASSERT_FALSE(graph.diagnostics.empty());
    EXPECT_TRUE(graph.diagnostics.front().message.contains(QStringLiteral("direct child")));
    t1->comp<VTrack>()->clips.items.clear();
    const auto unrelated = place(t1, s1);
    graph = buildSignalPathGraph(project, {{s4->guid, {unrelated}}});
    EXPECT_FALSE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
}

TEST_F(SignalPathGraphTest, RejectsReorderedOrRetargetedNestedOccurrences)
{
    auto* piece = create("InnerPiece", T::Piece, t1);
    auto* track = create("InnerTrack", T::Track, piece);
    auto* sound = create("InnerSound", T::Sound, track);
    const auto inner = place(track, sound);
    const auto outer = place(t1, piece);
    auto graph = buildSignalPathGraph(project, {{sound->guid, {outer, inner}}});
    EXPECT_FALSE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
    track->comp<VTrack>()->clips.items.front().targetGuid = s1->guid;
    graph = buildSignalPathGraph(project, {{sound->guid, {inner, outer}}});
    EXPECT_FALSE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
}

TEST_F(SignalPathGraphTest, InvalidRoutingAndBusCyclesDoNotProducePartialGraphs)
{
    b1->comp<VMixNode>()->sends.push_back({9999, 0.25f});
    auto graph = build();
    EXPECT_FALSE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
    EXPECT_TRUE(graph.nodes.empty());
    EXPECT_TRUE(graph.edges.empty());
    b1->comp<VMixNode>()->sends = {{b2->objectID, 0.25f}};
    b2->comp<VMixNode>()->sends = {{b1->objectID, 0.5f}};
    graph = build();
    EXPECT_FALSE(graph.ok());
    EXPECT_TRUE(graph.paths.empty());
    ASSERT_FALSE(graph.diagnostics.empty());
    EXPECT_TRUE(graph.diagnostics.front().message.contains(QStringLiteral("cycle")));
}

TEST_F(SignalPathGraphTest, DerivationDoesNotWriteInheritedConfiguration)
{
    const auto beforeOutput = s5->fieldGroups.mainOutput;
    const auto beforeSend = s5->fieldGroups.send;
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(s5->fieldGroups.mainOutput, beforeOutput);
    EXPECT_EQ(s5->fieldGroups.send, beforeSend);
    EXPECT_EQ(s5->fieldGroups.send.shared.mode, FieldGroupMode::Inherit);
}

}
