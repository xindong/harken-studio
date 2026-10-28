#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

namespace harkenprojectschema {
namespace {

using fixtures::G;

ProjectInput SingleNodeProject(ContentKind kind) {
    ProjectInput input;
    InputNode node = fixtures::Node(kind, 1);
    if (kind == ContentKind::Segment) {
        node.timeSpec = fixtures::AbsoluteTime();
        node.startPoint = fixtures::EntryStart();
    }
    input.nodes = {node};
    input.roots = {G(1)};
    return input;
}

SignalGraph& DeclareGraph(ProjectInput& input) {
    input.nodes.front().hasSignalGraph = true;
    return input.nodes.front().signalGraph;
}

EffectNode Effect(GraphNodeId nodeId, EffectTypeId effectType) {
    EffectNode node;
    node.nodeId = nodeId;
    node.effectType = effectType;
    return node;
}

void ExpectRefusal(const ProjectInput& input) {
    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.facts.has_value());
}

TEST(ProjectSchemaSignalGraph, DuplicateNodeIdRefused) {
    ExpectRefusal(fixtures::InvalidGraphDuplicateNodeId());
}

TEST(ProjectSchemaSignalGraph, DuplicateParamIdRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    EffectNode repeated = Effect(2, 11);
    repeated.params = {EffectParam{5, 0u}, EffectParam{5, 1u}};
    DeclareGraph(input).nodes = {Effect(1, 10), repeated};
    ExpectRefusal(input);
}

TEST(ProjectSchemaSignalGraph, EdgeEndpointMissingRefused) {
    ExpectRefusal(fixtures::InvalidGraphEdgeEndpointMissing());
}

TEST(ProjectSchemaSignalGraph, CycleRefused) {
    ExpectRefusal(fixtures::InvalidGraphCycle());
}

TEST(ProjectSchemaSignalGraph, SelfLoopIsCycle) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {Effect(7, 10)};
    input.nodes.front().signalGraph.edges = {GraphEdge{7, 7}};
    ExpectRefusal(input);
}

TEST(ProjectSchemaSignalGraph, CycleInSecondConnectedComponentIsRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {
        Effect(1, 10), Effect(2, 20), Effect(3, 30), Effect(4, 40), Effect(5, 50)};
    input.nodes.front().signalGraph.edges = {
        GraphEdge{1, 2}, GraphEdge{3, 4}, GraphEdge{4, 5}, GraphEdge{5, 3}};
    ExpectRefusal(input);
}

TEST(ProjectSchemaSignalGraph, DiamondDagIsAccepted) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {
        Effect(1, 10), Effect(2, 20), Effect(3, 30), Effect(4, 40)};
    input.nodes.front().signalGraph.edges = {
        GraphEdge{1, 2}, GraphEdge{1, 3}, GraphEdge{2, 4}, GraphEdge{3, 4}};

    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    EXPECT_TRUE(result.project.has_value());
    EXPECT_TRUE(result.facts.has_value());
}

}
}
