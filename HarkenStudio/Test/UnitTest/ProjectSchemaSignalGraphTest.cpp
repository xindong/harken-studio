// Coverage for the object effect signal graph on the common chassis: the effect
// nodes, their params and the directed edges that all five content kinds share.
// The front-end stores the graph verbatim and
// proves it is a structurally legal DAG — unique non-zero node ids, non-zero
// effect types, no duplicate param ids within a node, edges whose endpoints
// resolve, and no cycle — but it does not compile the graph, judge its topology,
// or reach the engine. These tests pin the defaults, per-kind passthrough,
// deterministic serialization, each structural refusal, that content and the
// declaration bit must agree, and that declaring a graph renumbers no lineage.

#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace harkenprojectschema {
namespace {

using fixtures::G;

const ContentObject* FindByGuid(const CanonicalProject& project,
                                const AuthoringGuid& guid) {
    for (const ContentObject& obj : project.objects) {
        if (obj.chassis().guid == guid) {
            return &obj;
        }
    }
    return nullptr;
}

ProjectInput SingleNodeProject(ContentKind kind) {
    ProjectInput input;
    InputNode node = fixtures::Node(kind, 1);
    if (kind == ContentKind::Piece) {
        node.timeSpec = fixtures::AbsoluteTime();
        node.startPoint = fixtures::EntryStart();
    }
    input.nodes = {node};
    input.roots = {G(1)};
    return input;
}

// Declares the single object's graph and hands it back for population. Content
// and the declaration bit have to agree — an object holding nodes it never
// declares is refused — so a test that authors content declares it here rather
// than reaching for the field directly.
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

// A minimal seekable stereo resource, so a Sound placed on a Track yields a
// program the fingerprint test can build.
ResourceInput GraphTestResource() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = true;
    return input;
}

ResourceTable GraphTestResources(const CanonicalProject& project) {
    ResourceTable table;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Sound) {
            table.emplace(obj.chassis().id, GraphTestResource());
        }
    }
    return table;
}

std::optional<PieceProgram> BuildGraphTestProgram(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    if (!front.ok() || !front.project.has_value() || !front.facts.has_value()) {
        return std::nullopt;
    }
    const std::optional<PieceIR> ir = BuildPieceIRForStructureOnly(
        *front.project, *front.facts, GraphTestResources(*front.project));
    if (!ir.has_value()) {
        return std::nullopt;
    }
    return BuildPieceProgram(*ir);
}

void ExpectSingleRefusal(const ProjectInput& input, DiagnosticCode expected,
                         std::uint64_t expectedDetail) {
    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.facts.has_value());
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code, expected)
        << "got " << DiagnosticCodeName(result.diagnostics.front().code);
    EXPECT_EQ(result.diagnostics.front().detail, expectedDetail);
    EXPECT_EQ(result.diagnostics.front().detail >> 32, expectedDetail >> 32);
    EXPECT_EQ(static_cast<std::uint32_t>(result.diagnostics.front().detail),
              static_cast<std::uint32_t>(expectedDetail));
}

// A default-constructed signal graph is empty, and a default chassis declares no
// graph at all — the two facts a selection reads.
TEST(ProjectSchemaSignalGraph, DefaultsMatchContract) {
    SignalGraph g;
    EXPECT_TRUE(g.nodes.empty());
    EXPECT_TRUE(g.edges.empty());

    Chassis c;
    EXPECT_FALSE(c.presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::SignalGraph));
    EXPECT_TRUE(c.signalGraph.nodes.empty());
    EXPECT_TRUE(c.signalGraph.edges.empty());
}

// Equality is exact and, for a param value, compares raw bits — so the two
// float-zero bit patterns, equal as floats, are distinct params.
TEST(ProjectSchemaSignalGraph, ComponentEquality) {
    EXPECT_TRUE((EffectParam{5, 1u} == EffectParam{5, 1u}));
    EXPECT_FALSE((EffectParam{5, 1u} == EffectParam{6, 1u}));
    EXPECT_FALSE((EffectParam{5, 1u} == EffectParam{5, 2u}));
    EXPECT_FALSE((EffectParam{5, 0x00000000u} == EffectParam{5, 0x80000000u}));

    EXPECT_TRUE((GraphEdge{1, 2} == GraphEdge{1, 2}));
    EXPECT_FALSE((GraphEdge{1, 2} == GraphEdge{2, 1}));

    EffectNode a;
    a.nodeId = 1;
    a.effectType = 10;
    a.params = {EffectParam{5, 1u}};
    EffectNode same = a;
    EXPECT_TRUE(a == same);
    EffectNode diffParam = a;
    diffParam.params = {EffectParam{5, 2u}};
    EXPECT_FALSE(a == diffParam);
    EffectNode diffType = a;
    diffType.effectType = 11;
    EXPECT_FALSE(a == diffType);
    EffectNode diffId = a;
    diffId.nodeId = 2;
    EXPECT_FALSE(a == diffId);
}

// Every one of the five kinds — Layer included — carries the same graph, and the
// front-end passes the nodes, params and edges through unchanged and in authored
// order.
TEST(ProjectSchemaSignalGraph, GraphPassesThroughForEveryKind) {
    const FrontEndResult result = CompileFrontEnd(fixtures::SharedGraphAllKinds());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());

    for (std::uint64_t guid = 1; guid <= 5; ++guid) {
        const ContentObject* obj = FindByGuid(*result.project, G(guid));
        ASSERT_NE(obj, nullptr) << "missing object guid " << guid;
        const SignalGraph& g = obj->chassis().signalGraph;
        ASSERT_EQ(g.nodes.size(), 2u);
        EXPECT_EQ(g.nodes[0].nodeId, 1u);
        EXPECT_EQ(g.nodes[0].effectType, 10u);
        ASSERT_EQ(g.nodes[0].params.size(), 2u);
        EXPECT_TRUE((g.nodes[0].params[0] == EffectParam{100, 0x3f800000u}));
        EXPECT_TRUE((g.nodes[0].params[1] == EffectParam{101, 7u}));
        EXPECT_EQ(g.nodes[1].nodeId, 2u);
        EXPECT_EQ(g.nodes[1].effectType, 11u);
        EXPECT_TRUE(g.nodes[1].params.empty());
        ASSERT_EQ(g.edges.size(), 1u);
        EXPECT_TRUE((g.edges[0] == GraphEdge{1, 2}));
        EXPECT_TRUE(obj->chassis().presence.Has(
            harkenplayableproperties::PlayablePropertyIdV1::SignalGraph));
    }
}

// A default (empty) input graph reaches the canonical chassis unchanged.
TEST(ProjectSchemaSignalGraph, InputDefaultGraphPassesThrough) {
    const FrontEndResult result = CompileFrontEnd(SingleNodeProject(ContentKind::Sound));
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    const ContentObject* object = FindByGuid(*result.project, G(1));
    ASSERT_NE(object, nullptr);
    const SignalGraph& g = object->chassis().signalGraph;
    EXPECT_TRUE(g.nodes.empty());
    EXPECT_TRUE(g.edges.empty());
    EXPECT_FALSE(object->chassis().presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::SignalGraph));
}

// The whole graph section renders in a fixed order (nodes with their params,
// then edges) with every id as a decimal integer and every
// param value as its raw bit pattern.
TEST(ProjectSchemaSignalGraph, SerializationPinsGraphStructure) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    SignalGraph& g = DeclareGraph(input);
    EffectNode n1;
    n1.nodeId = 1;
    n1.effectId = G(11);
    n1.effectType = 10;
    n1.params = {EffectParam{100, 42u}};
    EffectNode n2;
    n2.nodeId = 2;
    n2.effectId = G(12);
    n2.effectType = 11;
    g.nodes = {n1, n2};
    g.edges = {GraphEdge{1, 2}};

    const FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    EXPECT_EQ(RenderProject(*result.project),
              "project\n"
              "object 3da91a64-b947-4560-0000-000000000001 kind=sound guid=4443111527285212512:1 editorOrder=0 volume=0 pitch=0 "
              "makeUpGain=0 loopCount=1 streamingMode=0 outputBusVolume=0\n"
              "  fxchain declared=1\n"
              "  fxnode id=1 object=3da91a64-b947-4560-0000-00000000000b type=10\n"
              "    param id=100 bits=42\n"
              "  fxnode id=2 object=3da91a64-b947-4560-0000-00000000000c type=11\n"
              "  fxedge from=1 to=2\n"
              "roots: 3da91a64-b947-4560-0000-000000000001\n");
}

TEST(ProjectSchemaSignalGraph, SerializationPreservesReverseAuthoredOrder) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    SignalGraph& graph = DeclareGraph(input);
    EffectNode n4 = Effect(4, 40);
    n4.params = {EffectParam{9, 90u}, EffectParam{7, 70u}};
    EffectNode n3 = Effect(3, 30);
    n3.params = {EffectParam{6, 60u}, EffectParam{5, 50u}};
    graph.nodes = {n4, n3, Effect(2, 20), Effect(1, 10)};
    for (auto& node : graph.nodes) node.effectId = G(10 + node.nodeId);
    graph.edges = {GraphEdge{3, 4}, GraphEdge{2, 4}, GraphEdge{1, 3},
                   GraphEdge{1, 2}};

    const FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    EXPECT_EQ(RenderProject(*result.project),
              "project\n"
              "object 3da91a64-b947-4560-0000-000000000001 kind=sound guid=4443111527285212512:1 editorOrder=0 volume=0 pitch=0 "
              "makeUpGain=0 loopCount=1 streamingMode=0 outputBusVolume=0\n"
              "  fxchain declared=1\n"
              "  fxnode id=4 object=3da91a64-b947-4560-0000-00000000000e type=40\n"
              "    param id=9 bits=90\n"
              "    param id=7 bits=70\n"
              "  fxnode id=3 object=3da91a64-b947-4560-0000-00000000000d type=30\n"
              "    param id=6 bits=60\n"
              "    param id=5 bits=50\n"
              "  fxnode id=2 object=3da91a64-b947-4560-0000-00000000000c type=20\n"
              "  fxnode id=1 object=3da91a64-b947-4560-0000-00000000000b type=10\n"
              "  fxedge from=3 to=4\n"
              "  fxedge from=2 to=4\n"
              "  fxedge from=1 to=3\n"
              "  fxedge from=1 to=2\n"
              "roots: 3da91a64-b947-4560-0000-000000000001\n");
}

// An undeclared, empty graph costs nothing in the rendering: none of the graph
// lines appear, the declaration marker included.
TEST(ProjectSchemaSignalGraph, EmptyGraphRendersNothing) {
    const FrontEndResult result = CompileFrontEnd(SingleNodeProject(ContentKind::Sound));
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.project.has_value());
    const std::string s = RenderProject(*result.project);
    EXPECT_EQ(s.find("fxchain"), std::string::npos);
    EXPECT_EQ(s.find("fxnode"), std::string::npos);
    EXPECT_EQ(s.find("fxedge"), std::string::npos);
}

// Declaring a graph with no nodes renders the marker and nothing else: the
// declaration is the whole content, and it has to survive rendering because
// declaring emptiness and declaring nothing select differently.
TEST(ProjectSchemaSignalGraph, DeclaredEmptyGraphRendersItsMarker) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    input.nodes.front().hasSignalGraph = true;

    const FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    const std::string s = RenderProject(*result.project);
    EXPECT_NE(s.find("  fxchain declared=1\n"), std::string::npos);
    EXPECT_EQ(s.find("fxnode"), std::string::npos);
    EXPECT_EQ(s.find("fxedge"), std::string::npos);
}

// Graph content with field 16 absent violates canonical absent storage and is
// refused before any canonical model is published.
TEST(ProjectSchemaSignalGraph, GraphContentWithoutDeclarationIsRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    // Structurally clean content, but the declaration bit is left false.
    input.nodes.front().signalGraph.nodes = {Effect(1, 10), Effect(2, 20)};
    input.nodes.front().signalGraph.edges = {GraphEdge{1, 2}};

    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.effectChains.has_value());
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::NonCanonicalAbsentProperty);
    EXPECT_EQ(result.diagnostics.front().detail, 16u);
}

// An edge-only graph has no nodes that can name its endpoints and field 16 is
// absent, so the deterministic parse result reports both missing ids followed
// by the canonical-storage violation.
TEST(ProjectSchemaSignalGraph, GraphEdgesWithoutDeclarationAreRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    input.nodes.front().signalGraph.edges = {GraphEdge{1, 2}};

    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 3u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].code,
              DiagnosticCode::GraphEdgeEndpointMissing);
    EXPECT_EQ(result.diagnostics[1].code,
              DiagnosticCode::GraphEdgeEndpointMissing);
    EXPECT_EQ(result.diagnostics[0].detail, 1u);
    EXPECT_EQ(result.diagnostics[1].detail, 2u);
    EXPECT_EQ(result.diagnostics[2].code,
              DiagnosticCode::NonCanonicalAbsentProperty);
    EXPECT_EQ(result.diagnostics[2].detail, 16u);
}

// A populated graph renders byte-identically on repeat.
TEST(ProjectSchemaSignalGraph, SerializationIsDeterministic) {
    const std::string a =
        RenderResult(CompileFrontEnd(fixtures::SharedGraphAllKinds()));
    const std::string b =
        RenderResult(CompileFrontEnd(fixtures::SharedGraphAllKinds()));
    EXPECT_EQ(a, b);
}

TEST(ProjectSchemaSignalGraph, ReservedNodeIdRefused) {
    ExpectSingleRefusal(fixtures::InvalidGraphReservedNodeId(),
                        DiagnosticCode::GraphNodeIdReserved, 0u);
}

TEST(ProjectSchemaSignalGraph, DuplicateNodeIdRefused) {
    ExpectSingleRefusal(fixtures::InvalidGraphDuplicateNodeId(),
                        DiagnosticCode::GraphNodeIdDuplicate, 5u);
}

TEST(ProjectSchemaSignalGraph, ReservedEffectTypeRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {
        Effect(1, 10), Effect(2, kReservedEffectType)};
    ExpectSingleRefusal(input, DiagnosticCode::GraphEffectTypeReserved, 1u);
}

// The sentinel sits at the top of the id space, not the bottom, because the
// bottom belongs to somebody else: the engine's effect registry owns its type
// ids from zero up, so a sentinel at zero would collide with the first
// registered type and refuse a chain naming it. The two facts are pinned
// together here — the reserved value is the maximum, and zero is an ordinary
// authorable type — so a migration back to zero fails as a contract break
// rather than as a puzzling refusal at the far end of the pipeline.
TEST(ProjectSchemaSignalGraph, ReservedEffectTypeIsTheTopOfTheIdSpace) {
    EXPECT_EQ(kReservedEffectType,
              std::numeric_limits<EffectTypeId>::max());
    EXPECT_NE(kReservedEffectType, EffectTypeId{0});
}

// A chain naming effect type zero is authored, compiled and accepted. Zero is
// the engine's first registered effect type, so refusing it here would make an
// entire built-in effect unreachable from an authored graph — the reserved
// value has to be a value the engine does not own.
TEST(ProjectSchemaSignalGraph, EffectTypeZeroIsAnOrdinaryAuthorableType) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {Effect(1, 0), Effect(2, 10)};

    const FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());

    const ContentObject* obj = FindByGuid(*result.project, G(1));
    ASSERT_NE(obj, nullptr);
    const SignalGraph& graph = obj->chassis().signalGraph;
    ASSERT_EQ(graph.nodes.size(), 2u);
    EXPECT_EQ(graph.nodes[0].effectType, 0u);
}

TEST(ProjectSchemaSignalGraph, DuplicateParamIdRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    EffectNode repeated = Effect(2, 11);
    repeated.params = {EffectParam{5, 0u}, EffectParam{5, 1u}};
    DeclareGraph(input).nodes = {Effect(1, 10), repeated};
    ExpectSingleRefusal(input, DiagnosticCode::GraphParamIdDuplicate,
                        (std::uint64_t{1} << 32) | 5u);
}

TEST(ProjectSchemaSignalGraph, EdgeEndpointMissingRefused) {
    ExpectSingleRefusal(fixtures::InvalidGraphEdgeEndpointMissing(),
                        DiagnosticCode::GraphEdgeEndpointMissing, 99u);
}

TEST(ProjectSchemaSignalGraph, CycleRefused) {
    ExpectSingleRefusal(fixtures::InvalidGraphCycle(),
                        DiagnosticCode::GraphCycle, 1u);
}

TEST(ProjectSchemaSignalGraph, SelfLoopIsCycle) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {Effect(7, 10)};
    input.nodes.front().signalGraph.edges = {GraphEdge{7, 7}};
    ExpectSingleRefusal(input, DiagnosticCode::GraphCycle, 7u);
}

TEST(ProjectSchemaSignalGraph, CycleInSecondConnectedComponentIsRefused) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {
        Effect(1, 10), Effect(2, 20), Effect(3, 30), Effect(4, 40), Effect(5, 50)};
    input.nodes.front().signalGraph.edges = {
        GraphEdge{1, 2}, GraphEdge{3, 4}, GraphEdge{4, 5}, GraphEdge{5, 3}};
    ExpectSingleRefusal(input, DiagnosticCode::GraphCycle, 3u);
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

TEST(ProjectSchemaSignalGraph, DuplicateNodeIdsGateCycleDetection) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    DeclareGraph(input).nodes = {
        Effect(1, 10), Effect(1, 11), Effect(2, 20)};
    input.nodes.front().signalGraph.edges = {GraphEdge{1, 2}, GraphEdge{2, 1}};

    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.facts.has_value());
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::GraphNodeIdDuplicate);
    EXPECT_NE(result.diagnostics.front().code, DiagnosticCode::GraphCycle);
    EXPECT_EQ(result.diagnostics.front().detail, 1u);
}

// Refused for every kind that carries the chassis: the graph rule is not
// Track-specific.
TEST(ProjectSchemaSignalGraph, CycleRefusedForEveryKind) {
    constexpr ContentKind kinds[] = {ContentKind::Layer, ContentKind::Sound,
                                     ContentKind::Container, ContentKind::Track,
                                     ContentKind::Piece};
    for (const ContentKind kind : kinds) {
        SCOPED_TRACE(static_cast<std::uint32_t>(kind));
        ProjectInput input = SingleNodeProject(kind);
        EffectNode a;
        a.nodeId = 1;
        a.effectType = 10;
        EffectNode b;
        b.nodeId = 2;
        b.effectType = 11;
        DeclareGraph(input).nodes = {a, b};
        input.nodes.front().signalGraph.edges = {GraphEdge{1, 2}, GraphEdge{2, 1}};

        const FrontEndResult result = CompileFrontEnd(input);
        EXPECT_FALSE(result.ok());
        EXPECT_FALSE(result.project.has_value());
        ASSERT_EQ(result.diagnostics.size(), 1u)
            << RenderDiagnostics(result.diagnostics);
        EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::GraphCycle);
    }
}

// Two reserved node ids on one graph produce two refusals, ordered by the
// authored node index carried in the detail, and the rendering is deterministic.
TEST(ProjectSchemaSignalGraph, MultipleReservedNodeIdsDeterministicByIndex) {
    const FrontEndResult result =
        CompileFrontEnd(fixtures::InvalidGraphTwoReservedNodeIds());
    ASSERT_EQ(result.diagnostics.size(), 2u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].code, DiagnosticCode::GraphNodeIdReserved);
    EXPECT_EQ(result.diagnostics[1].code, DiagnosticCode::GraphNodeIdReserved);
    EXPECT_EQ(result.diagnostics[0].detail, 0u);
    EXPECT_EQ(result.diagnostics[1].detail, 1u);

    const std::string a =
        RenderResult(CompileFrontEnd(fixtures::InvalidGraphTwoReservedNodeIds()));
    const std::string b =
        RenderResult(CompileFrontEnd(fixtures::InvalidGraphTwoReservedNodeIds()));
    EXPECT_EQ(a, b);
}

// A declared graph is no longer authoring-only: it is selected and compiled into
// the chain an occurrence runs, so declaring one on every object changes what the
// program sounds like and must move the semantic fingerprint. Lineage is untouched
// — the objects, tracks and clips are the same — so that fingerprint must not move.
//
// This build routes no facts (structure only), which is what keeps the comparison
// about the graph alone; the selected chain reaches the fingerprint through the
// fact-threaded path the effect-chain tests exercise.
TEST(ProjectSchemaSignalGraph, DeclaredGraphLeavesLineageFingerprintAlone) {
    const ProjectInput defaults = fixtures::IrLineageMix();
    ProjectInput populated = defaults;
    for (InputNode& node : populated.nodes) {
        fixtures::ApplyGraphProbe(&node);
    }

    const std::optional<PieceProgram> defaultProgram =
        BuildGraphTestProgram(defaults);
    const std::optional<PieceProgram> populatedProgram =
        BuildGraphTestProgram(populated);
    ASSERT_TRUE(defaultProgram.has_value());
    ASSERT_TRUE(populatedProgram.has_value());
    EXPECT_EQ(defaultProgram->runtimeLineageFingerprint(),
              populatedProgram->runtimeLineageFingerprint())
        << "declaring a graph renumbered nothing, so lineage must be unchanged";
}

}  // namespace
}  // namespace harkenprojectschema
