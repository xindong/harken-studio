// Coverage for effect-chain selection, linearization, lowering and fingerprinting.
//
// Selection chooses one declaration along the shared fold chain and never merges
// several: the walk is subject, then the organizational chain (nearest first),
// then the placement chain, and the first declaring object wins whole. Declaring
// is a separate fact from declaring something non-empty — an object with no
// declaration is transparent, while an object declaring an empty graph overrides
// an inherited chain with no effects. These tests pin that selection, the
// linearization refusals for a branching or disjoint graph, the compiled chain's
// entries and resource demand, the lowering ceiling, and that chain content moves
// the semantic fingerprint while its addressing does not.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/EffectChainSelection.h"
#include "harken_project_schema/FixedSlotLowerer.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/PieceIR.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;

ResourceInput StereoResource() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = true;
    return input;
}

// A declared graph of `count` effects in one straight line, node ids 1..count and
// effect types 10, 11, ... Each node carries one parameter so a payload exists to
// hash and to render.
SignalGraph LinearGraph(std::uint32_t count) {
    SignalGraph graph;
    for (std::uint32_t i = 0; i < count; ++i) {
        EffectNode node;
        node.nodeId = i + 1;
        node.effectType = 10 + i;
        node.params = {EffectParam{0, 0x3f800000u}};  // 1.0f as raw bits
        graph.nodes.push_back(node);
        if (i > 0) {
            graph.edges.push_back(GraphEdge{i, i + 1});
        }
    }
    return graph;
}

void Declare(InputNode* node, const SignalGraph& graph) {
    node->hasSignalGraph = true;
    node->signalGraph = graph;
}

ProjectInput ChainWorld() {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(2)};
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(5)};
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    InputNode piece = fixtures::Node(ContentKind::Piece, 5);
    piece.guid = harkentest::StudioObjectId(5);
    piece.ownedChildren = {G(4)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {layer, container, sound, piece, track};
    in.roots = {G(1)};
    return in;
}

constexpr std::size_t kLayer = 0;
constexpr std::size_t kContainer = 1;
constexpr std::size_t kSound = 2;
constexpr std::size_t kPiece = 3;
constexpr std::size_t kTrack = 4;

// The selection for clip 100, or nullopt when the compile failed.
std::optional<OccurrenceEffectChainSelection> SelectionForClip(
    const ProjectInput& input, StableId clipId) {
    const FrontEndResult front = CompileFrontEnd(input);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    if (!front.effectChains.has_value()) {
        return std::nullopt;
    }
    for (const OccurrenceEffectChainSelection& occ :
         front.effectChains->occurrences) {
        if (occ.clipId == clipId) {
            return occ;
        }
    }
    return std::nullopt;
}

std::optional<PieceIR> BuildChainIr(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    if (!front.ok() || !front.project.has_value() ||
        !front.facts.has_value() || !front.resolution.has_value() ||
        !front.sends.has_value() || !front.effectChains.has_value()) {
        return std::nullopt;
    }
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());
    return BuildPieceIR(*front.project, *front.facts, resources,
                        *front.resolution, *front.sends, *front.effectChains);
}

std::uint64_t SemanticFingerprint(const ProjectInput& input) {
    const std::optional<PieceIR> ir = BuildChainIr(input);
    if (!ir.has_value()) {
        return 0;
    }
    const std::optional<PieceProgram> program = BuildPieceProgram(*ir);
    if (!program.has_value()) {
        return 0;
    }
    return program->programSemanticFingerprint();
}

// ---- selection semantics -------------------------------------------------

// No object declares a graph: the occurrence selects nothing at all, which is
// distinct from selecting an empty chain.
TEST(ProjectSchemaEffectChain, NoDeclarationSelectsNoChain) {
    const std::optional<OccurrenceEffectChainSelection> occ =
        SelectionForClip(ChainWorld(), harkentest::StudioObjectId(100));
    ASSERT_TRUE(occ.has_value());
    EXPECT_FALSE(occ->declared);
    EXPECT_EQ(occ->owner, kUnmintedStableId);
    EXPECT_TRUE(occ->shadowedOwners.empty());
}

TEST(ProjectSchemaEffectChain, AbsentDefinitionEffectsStayEmpty) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kContainer], LinearGraph(2));
    const auto occ = SelectionForClip(in, harkentest::StudioObjectId(100));
    ASSERT_TRUE(occ.has_value());
    EXPECT_FALSE(occ->declared);
    EXPECT_EQ(occ->owner, kUnmintedStableId);
    EXPECT_TRUE(occ->shadowedOwners.empty());
}

TEST(ProjectSchemaEffectChain, SubjectDeclarationDoesNotRecordAncestorEffects) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kLayer], LinearGraph(1));
    Declare(&in.nodes[kContainer], LinearGraph(2));
    Declare(&in.nodes[kSound], LinearGraph(3));

    const std::optional<OccurrenceEffectChainSelection> occ =
        SelectionForClip(in, harkentest::StudioObjectId(100));
    ASSERT_TRUE(occ.has_value());
    EXPECT_TRUE(occ->declared);
    EXPECT_EQ(occ->owner, harkentest::StudioObjectId(3u));  // the subject
    EXPECT_TRUE(occ->shadowedOwners.empty());
}

TEST(ProjectSchemaEffectChain, DeclaredEmptyDefinitionRunsNoEffects) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kContainer], LinearGraph(2));
    Declare(&in.nodes[kSound], SignalGraph{});  // declared, empty

    const std::optional<OccurrenceEffectChainSelection> occ =
        SelectionForClip(in, harkentest::StudioObjectId(100));
    ASSERT_TRUE(occ.has_value());
    EXPECT_TRUE(occ->declared);
    EXPECT_EQ(occ->owner, harkentest::StudioObjectId(3u));  // the subject's empty declaration won
    EXPECT_TRUE(occ->shadowedOwners.empty());

    const std::optional<PieceIR> ir = BuildChainIr(in);
    ASSERT_TRUE(ir.has_value());
    ASSERT_EQ(ir->tracks().size(), 1u);
    ASSERT_EQ(ir->tracks()[0].occurrences.size(), 1u);
    const IrOccurrence& irOcc = ir->tracks()[0].occurrences[0];
    ASSERT_TRUE(irOcc.hasEffectChain);
    // Selected, and the selected chain runs nothing.
    EXPECT_TRUE(ir->effectChains()[irOcc.effectChainIndex].entries.empty());
}

TEST(ProjectSchemaEffectChain, OwnershipAndPlacementEffectsDoNotSupplyTheSubject) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kContainer], LinearGraph(2));
    Declare(&in.nodes[kTrack], LinearGraph(3));
    Declare(&in.nodes[kPiece], LinearGraph(1));
    const auto occ = SelectionForClip(in, harkentest::StudioObjectId(100));
    ASSERT_TRUE(occ.has_value());
    EXPECT_FALSE(occ->declared);
    EXPECT_EQ(occ->owner, kUnmintedStableId);
    EXPECT_TRUE(occ->shadowedOwners.empty());
}

TEST(ProjectSchemaEffectChain, AncestorEffectEditsDoNotChangeSubjectVoiceProgram) {
    ProjectInput in = ChainWorld();
    const auto before = SemanticFingerprint(in);
    Declare(&in.nodes[kPiece], LinearGraph(2));
    Declare(&in.nodes[kContainer], LinearGraph(3));
    EXPECT_EQ(SemanticFingerprint(in), before);
}

TEST(ProjectSchemaEffectChain, OneChainStoredPerDeclaringOwner) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kSound], LinearGraph(2));
    InputClip second = fixtures::Clip(101, 3);
    second.stableOccurrenceOrdinal = 1;
    second.guid = harkentest::StudioObjectId(101);
    in.nodes[kTrack].clips.push_back(second);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    EXPECT_EQ(front.effectChains->occurrences.size(), 2u);
    ASSERT_EQ(front.effectChains->chains.size(), 1u);
    EXPECT_EQ(front.effectChains->chains[0].owner, harkentest::StudioObjectId(3u));
    for (const OccurrenceEffectChainSelection& occ :
         front.effectChains->occurrences) {
        EXPECT_TRUE(occ.declared);
        EXPECT_EQ(occ.chainIndex, 0u);
    }
}

// ---- linearization -------------------------------------------------------

// The compiled entries follow the graph's path, not its authored node order.
TEST(ProjectSchemaEffectChain, EntriesFollowThePathNotTheAuthoredOrder) {
    ProjectInput in = ChainWorld();
    // Authored tail-first: node 2 feeds node 1, so execution is 2 then 1.
    SignalGraph graph;
    EffectNode first;
    first.nodeId = 1;
    first.effectType = 10;
    EffectNode second;
    second.nodeId = 2;
    second.effectType = 11;
    graph.nodes = {first, second};
    graph.edges = {GraphEdge{2, 1}};
    Declare(&in.nodes[kSound], graph);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    ASSERT_EQ(front.effectChains->chains.size(), 1u);
    const CompiledEffectChain& chain = front.effectChains->chains[0];
    ASSERT_EQ(chain.entries.size(), 2u);
    EXPECT_EQ(chain.entries[0].stableNodeId, 2u);
    EXPECT_EQ(chain.entries[0].effectType, 11u);
    EXPECT_EQ(chain.entries[1].stableNodeId, 1u);
    EXPECT_EQ(chain.entries[1].effectType, 10u);
}

// A node feeding two others splits the path; the order would be ambiguous, so the
// build refuses rather than picking one.
TEST(ProjectSchemaEffectChain, SplittingNodeIsRefused) {
    ProjectInput in = ChainWorld();
    SignalGraph graph = LinearGraph(3);
    graph.edges = {GraphEdge{1, 2}, GraphEdge{1, 3}};  // node 1 splits
    Declare(&in.nodes[kSound], graph);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_FALSE(front.ok());
    ASSERT_EQ(front.diagnostics.size(), 1u);
    EXPECT_EQ(front.diagnostics[0].code,
              DiagnosticCode::EffectChainNodeNotLinear);
    EXPECT_EQ(front.diagnostics[0].detail, 1u);  // the splitting node
    EXPECT_FALSE(front.effectChains.has_value());
}

// A node fed by two others merges paths, which the series artifact cannot express.
TEST(ProjectSchemaEffectChain, MergingNodeIsRefused) {
    ProjectInput in = ChainWorld();
    SignalGraph graph = LinearGraph(3);
    graph.edges = {GraphEdge{1, 3}, GraphEdge{2, 3}};  // node 3 merges
    Declare(&in.nodes[kSound], graph);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_FALSE(front.ok());
    ASSERT_EQ(front.diagnostics.size(), 1u);
    EXPECT_EQ(front.diagnostics[0].code,
              DiagnosticCode::EffectChainNodeNotLinear);
    EXPECT_EQ(front.diagnostics[0].detail, 3u);  // the merging node
}

// Two disjoint runs are each linear but are not one chain.
TEST(ProjectSchemaEffectChain, DisjointRunsAreRefused) {
    ProjectInput in = ChainWorld();
    SignalGraph graph = LinearGraph(4);
    graph.edges = {GraphEdge{1, 2}, GraphEdge{3, 4}};  // two separate pairs
    Declare(&in.nodes[kSound], graph);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_FALSE(front.ok());
    ASSERT_EQ(front.diagnostics.size(), 1u);
    EXPECT_EQ(front.diagnostics[0].code,
              DiagnosticCode::EffectChainNotConnected);
    EXPECT_EQ(front.diagnostics[0].detail, 4u);  // the node count
}

// Unconnected nodes with no edges at all are also not one chain.
TEST(ProjectSchemaEffectChain, EdgelessMultiNodeGraphIsRefused) {
    ProjectInput in = ChainWorld();
    SignalGraph graph = LinearGraph(2);
    graph.edges.clear();
    Declare(&in.nodes[kSound], graph);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_FALSE(front.ok());
    ASSERT_EQ(front.diagnostics.size(), 1u);
    EXPECT_EQ(front.diagnostics[0].code,
              DiagnosticCode::EffectChainNotConnected);
}

// A single node needs no edge to be a chain.
TEST(ProjectSchemaEffectChain, SingleNodeGraphLinearizes) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kSound], LinearGraph(1));

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    ASSERT_EQ(front.effectChains->chains.size(), 1u);
    EXPECT_EQ(front.effectChains->chains[0].entries.size(), 1u);
}

// ---- resource demand ------------------------------------------------------

// The demand is the selected chain's own, never a sum down the chain: an inherited
// chain of two contributes two, not two plus the descendant's.
TEST(ProjectSchemaEffectChain, DemandIsTheSelectedChainNotAccumulated) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kLayer], LinearGraph(4));
    Declare(&in.nodes[kSound], LinearGraph(2));

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    // Only the winning declaration is compiled; the shadowed Layer chain is not.
    ASSERT_EQ(front.effectChains->chains.size(), 1u);
    const EffectChainResources& demand =
        front.effectChains->chains[0].resources;
    EXPECT_EQ(demand.maxChainLength, 2u);
    EXPECT_EQ(demand.requiredCapabilities, kEffectChainRequiresVoiceHost);
    // One row per distinct type, ascending by type, each with its instance count
    // and the parameter count needed to address its largest authored id.
    ASSERT_EQ(demand.perTypeDemand.size(), 2u);
    EXPECT_EQ(demand.perTypeDemand[0].effectType, 10u);
    EXPECT_EQ(demand.perTypeDemand[0].instanceCount, 1u);
    EXPECT_EQ(demand.perTypeDemand[0].requiredParameterCount, 1u);
    EXPECT_EQ(demand.perTypeDemand[1].effectType, 11u);
}

// Repeats of one type aggregate into a single row's instance count, and the
// count is one past the largest parameter id across those entries.
TEST(ProjectSchemaEffectChain, RepeatedTypeAggregatesIntoOneDemandRow) {
    ProjectInput in = ChainWorld();
    SignalGraph graph;
    EffectNode a;
    a.nodeId = 1;
    a.effectType = 10;
    a.params = {EffectParam{0, 0u}};
    EffectNode b;
    b.nodeId = 2;
    b.effectType = 10;  // the same type twice
    b.params = {EffectParam{3, 0u}};
    graph.nodes = {a, b};
    graph.edges = {GraphEdge{1, 2}};
    Declare(&in.nodes[kSound], graph);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    ASSERT_EQ(front.effectChains->chains.size(), 1u);
    const EffectChainResources& demand =
        front.effectChains->chains[0].resources;
    EXPECT_EQ(demand.maxChainLength, 2u);
    ASSERT_EQ(demand.perTypeDemand.size(), 1u);
    EXPECT_EQ(demand.perTypeDemand[0].effectType, 10u);
    EXPECT_EQ(demand.perTypeDemand[0].instanceCount, 2u);
    EXPECT_EQ(demand.perTypeDemand[0].requiredParameterCount, 4u);
}

// An empty declared chain demands nothing: overriding to no effects asks the host
// for no capability it would not otherwise need.
TEST(ProjectSchemaEffectChain, EmptyChainDemandsNothing) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kSound], SignalGraph{});

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    ASSERT_EQ(front.effectChains->chains.size(), 1u);
    const EffectChainResources& demand =
        front.effectChains->chains[0].resources;
    EXPECT_EQ(demand.maxChainLength, 0u);
    EXPECT_EQ(demand.requiredCapabilities, 0u);
    EXPECT_TRUE(demand.perTypeDemand.empty());
}

// ---- lowering to fixed slots ---------------------------------------------

// Entry i occupies slot i, the only mapping a fixed array admits.
TEST(ProjectSchemaEffectChain, LoweringMapsEntriesToAscendingSlots) {
    CompiledEffectChain chain;
    chain.entries.resize(3);
    const LowerEffectChainResult lowered =
        LowerEffectChainToFixedSlots(chain, AuthoringGuid{});
    ASSERT_TRUE(lowered.ok());
    ASSERT_TRUE(lowered.chain.has_value());
    ASSERT_EQ(lowered.chain->slots.size(), 3u);
    for (std::uint32_t i = 0; i < 3u; ++i) {
        EXPECT_EQ(lowered.chain->slots[i].slot, i);
        EXPECT_EQ(lowered.chain->slots[i].entryIndex, i);
    }
}

// A chain exactly filling the fixed array lowers; one entry longer is refused
// rather than truncated, because dropping the tail would change the sound.
TEST(ProjectSchemaEffectChain, LoweringRefusesPastTheFixedSlotCeiling) {
    CompiledEffectChain atCeiling;
    atCeiling.entries.resize(kFixedVoiceFxSlotCount);
    EXPECT_TRUE(LowerEffectChainToFixedSlots(atCeiling, AuthoringGuid{}).ok());

    CompiledEffectChain overCeiling;
    overCeiling.entries.resize(kFixedVoiceFxSlotCount + 1);
    const LowerEffectChainResult lowered =
        LowerEffectChainToFixedSlots(overCeiling, AuthoringGuid{});
    ASSERT_FALSE(lowered.ok());
    EXPECT_FALSE(lowered.chain.has_value());
    ASSERT_EQ(lowered.diagnostics.size(), 1u);
    EXPECT_EQ(lowered.diagnostics[0].code,
              DiagnosticCode::EffectChainExceedsSlotLimit);
    EXPECT_EQ(lowered.diagnostics[0].detail, kFixedVoiceFxSlotCount + 1);
}

// An empty chain lowers successfully to no steps: it is a real chain that runs
// nothing, not a failed lowering.
TEST(ProjectSchemaEffectChain, EmptyChainLowersToNoSlots) {
    const LowerEffectChainResult lowered =
        LowerEffectChainToFixedSlots(CompiledEffectChain{}, AuthoringGuid{});
    ASSERT_TRUE(lowered.ok());
    ASSERT_TRUE(lowered.chain.has_value());
    EXPECT_TRUE(lowered.chain->slots.empty());
}

// ---- fact bundle agreement ------------------------------------------------

// The chain facts must name the normalized occurrence set exactly, on the same
// terms as the routing bundles: a bundle that is short, doubled, padded, or
// disagrees about an occurrence's subject or ordinal produces no IR at all rather
// than an occurrence with silently defaulted effects. A chain index outside the
// table it came with is refused for the same reason.
TEST(ProjectSchemaEffectChain, ChainFactsMustMatchOccurrencesExactly) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kSound], LinearGraph(1));
    InputClip second = fixtures::Clip(101, 3);
    second.stableOccurrenceOrdinal = 1;
    second.guid = harkentest::StudioObjectId(101);
    in.nodes[kTrack].clips.push_back(second);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    ASSERT_EQ(front.effectChains->occurrences.size(), 2u);
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());

    std::vector<EffectChainSelectionFacts> invalid;
    EffectChainSelectionFacts missing = *front.effectChains;
    missing.occurrences.pop_back();
    invalid.push_back(missing);
    EffectChainSelectionFacts duplicate = *front.effectChains;
    duplicate.occurrences.push_back(duplicate.occurrences.front());
    invalid.push_back(duplicate);
    EffectChainSelectionFacts extra = *front.effectChains;
    OccurrenceEffectChainSelection extraOccurrence = extra.occurrences.front();
    extraOccurrence.trackId = harkentest::StudioObjectId(999);
    extraOccurrence.clipId = harkentest::StudioObjectId(998);
    extra.occurrences.push_back(extraOccurrence);
    invalid.push_back(extra);
    EffectChainSelectionFacts wrongSubject = *front.effectChains;
    wrongSubject.occurrences.front().subjectId = harkentest::OffsetObjectId(wrongSubject.occurrences.front().subjectId, 1);
    invalid.push_back(wrongSubject);
    EffectChainSelectionFacts wrongOrdinal = *front.effectChains;
    ++wrongOrdinal.occurrences.front().occurrenceOrdinal;
    invalid.push_back(wrongOrdinal);
    EffectChainSelectionFacts danglingIndex = *front.effectChains;
    danglingIndex.occurrences.front().chainIndex =
        static_cast<std::uint32_t>(danglingIndex.chains.size());
    invalid.push_back(danglingIndex);
    // An entry whose parameter block does not resolve inside its own chain is
    // refused too: the payload is read through that reference everywhere
    // downstream, so it may not dangle either.
    EffectChainSelectionFacts danglingBlock = *front.effectChains;
    ASSERT_FALSE(danglingBlock.chains.empty());
    ASSERT_FALSE(danglingBlock.chains.front().entries.empty());
    danglingBlock.chains.front().entries.front().paramBlockRef =
        static_cast<std::uint32_t>(
            danglingBlock.chains.front().paramBlocks.size());
    invalid.push_back(danglingBlock);

    for (const EffectChainSelectionFacts& facts : invalid) {
        EXPECT_FALSE(BuildPieceIR(*front.project, *front.facts, resources,
                                  *front.resolution, *front.sends, facts)
                         .has_value());
    }
}

// A structure-only build threads no facts at all, so its occurrences keep the
// neutral default: no chain selected, and no chain table on the IR.
TEST(ProjectSchemaEffectChain, StructureOnlyBuildSelectsNoChain) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kSound], LinearGraph(2));
    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());

    const std::optional<PieceIR> ir = BuildPieceIRForStructureOnly(
        *front.project, *front.facts, resources);
    ASSERT_TRUE(ir.has_value());
    EXPECT_TRUE(ir->effectChains().empty());
    ASSERT_EQ(ir->tracks().size(), 1u);
    ASSERT_EQ(ir->tracks()[0].occurrences.size(), 1u);
    EXPECT_FALSE(ir->tracks()[0].occurrences[0].hasEffectChain);
}

// ---- fingerprint ---------------------------------------------------------

// Selecting a chain, and what that chain holds, are audible facts.
TEST(ProjectSchemaEffectChain, ChainContentMovesTheSemanticFingerprint) {
    const ProjectInput none = ChainWorld();

    ProjectInput declared = ChainWorld();
    Declare(&declared.nodes[kSound], LinearGraph(1));

    ProjectInput longer = ChainWorld();
    Declare(&longer.nodes[kSound], LinearGraph(2));

    ProjectInput otherType = ChainWorld();
    SignalGraph retyped = LinearGraph(1);
    retyped.nodes[0].effectType = 99;
    Declare(&otherType.nodes[kSound], retyped);

    ProjectInput otherValue = ChainWorld();
    SignalGraph revalued = LinearGraph(1);
    revalued.nodes[0].params = {EffectParam{0, 0x40000000u}};  // 2.0f
    Declare(&otherValue.nodes[kSound], revalued);

    ProjectInput emptyDeclared = ChainWorld();
    Declare(&emptyDeclared.nodes[kSound], SignalGraph{});

    const std::uint64_t noneFp = SemanticFingerprint(none);
    ASSERT_NE(noneFp, 0u);
    const std::uint64_t declaredFp = SemanticFingerprint(declared);
    const std::uint64_t longerFp = SemanticFingerprint(longer);
    const std::uint64_t otherTypeFp = SemanticFingerprint(otherType);
    const std::uint64_t otherValueFp = SemanticFingerprint(otherValue);
    const std::uint64_t emptyDeclaredFp = SemanticFingerprint(emptyDeclared);

    EXPECT_NE(noneFp, declaredFp) << "selecting a chain must be audible";
    EXPECT_NE(declaredFp, longerFp) << "chain length must be audible";
    EXPECT_NE(declaredFp, otherTypeFp) << "effect type must be audible";
    EXPECT_NE(declaredFp, otherValueFp)
        << "a parameter value must be audible";
    EXPECT_NE(noneFp, emptyDeclaredFp)
        << "declaring an empty chain must be distinguishable from no declaration";
}

// The authored node id is lineage, not sound: renumbering nodes while keeping the
// same effects, order and values leaves the semantic fingerprint alone.
TEST(ProjectSchemaEffectChain, NodeRenumberingDoesNotMoveTheSemanticFingerprint) {
    ProjectInput low = ChainWorld();
    SignalGraph lowIds;
    EffectNode a;
    a.nodeId = 1;
    a.effectType = 10;
    a.params = {EffectParam{0, 0x3f800000u}};
    EffectNode b;
    b.nodeId = 2;
    b.effectType = 11;
    b.params = {EffectParam{0, 0x40000000u}};
    lowIds.nodes = {a, b};
    lowIds.edges = {GraphEdge{1, 2}};
    Declare(&low.nodes[kSound], lowIds);

    ProjectInput high = ChainWorld();
    SignalGraph highIds;
    a.nodeId = 71;
    b.nodeId = 72;
    highIds.nodes = {a, b};
    highIds.edges = {GraphEdge{71, 72}};
    Declare(&high.nodes[kSound], highIds);

    const std::uint64_t lowFp = SemanticFingerprint(low);
    ASSERT_NE(lowFp, 0u);
    EXPECT_EQ(lowFp, SemanticFingerprint(high));
}

// A golden the compiler's own output fills in: the value is pinned only so an
// unintended change to the chain fingerprinting shows up as a failure here.
TEST(ProjectSchemaEffectChain, SelectedChainFingerprintGolden) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kSound], LinearGraph(2));
    EXPECT_EQ(SemanticFingerprint(in), 7571979860121765373ull);
}

// ---- rendering -----------------------------------------------------------

// The occurrence line renders the chain it will run, and the two "no effects"
// cases stay distinguishable in the text.
TEST(ProjectSchemaEffectChain, RenderingDistinguishesNoneFromDeclaredEmpty) {
    const std::optional<PieceIR> noneIr = BuildChainIr(ChainWorld());
    ASSERT_TRUE(noneIr.has_value());
    EXPECT_NE(RenderPieceIR(*noneIr).find("/chainnone"), std::string::npos);

    ProjectInput emptyDeclared = ChainWorld();
    Declare(&emptyDeclared.nodes[kSound], SignalGraph{});
    const std::optional<PieceIR> emptyIr = BuildChainIr(emptyDeclared);
    ASSERT_TRUE(emptyIr.has_value());
    const std::string emptyText = RenderPieceIR(*emptyIr);
    EXPECT_EQ(emptyText.find("/chainnone"), std::string::npos);
    EXPECT_NE(emptyText.find("/chain3da91a64-b947-4560-0000-000000000003#0"), std::string::npos);

    ProjectInput populated = ChainWorld();
    Declare(&populated.nodes[kSound], LinearGraph(2));
    const std::optional<PieceIR> populatedIr = BuildChainIr(populated);
    ASSERT_TRUE(populatedIr.has_value());
    EXPECT_NE(RenderPieceIR(*populatedIr).find("/chain3da91a64-b947-4560-0000-000000000003#2"),
              std::string::npos);
}

// The exact text freezes entries, parameter blocks, resource demand and every
// selection field instead of accepting a renderer that silently drops one.
TEST(ProjectSchemaEffectChain, ChainFactRenderingIsByteExact) {
    ProjectInput in = ChainWorld();
    Declare(&in.nodes[kContainer], LinearGraph(1));
    Declare(&in.nodes[kSound], LinearGraph(2));
    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.effectChains.has_value());
    const std::string expected =
        "chains 1\n"
        "chain owner=3da91a64-b947-4560-0000-000000000003 entries=2 1,10,0,b0 2,11,0,b1 demand len=2 "
        "caps=1 types: 10x1/p1 11x1/p1\n"
        "  block 0: 0=1065353216\n"
        "  block 1: 0=1065353216\n"
        "chainsel 1\n"
        "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=3da91a64-b947-4560-0000-000000000003 owner=3da91a64-b947-4560-0000-000000000003 chain=0 shadowed:\n";
    EXPECT_EQ(RenderEffectChainSelection(*front.effectChains), expected);
}

// The program requires the per-voice effect capability exactly when some
// occurrence selected a chain that has effects to run.
TEST(ProjectSchemaEffectChain, VoiceEffectCapabilityTracksSelectedEntries) {
    const std::optional<PieceIR> noneIr = BuildChainIr(ChainWorld());
    ASSERT_TRUE(noneIr.has_value());
    const std::optional<PieceProgram> noneProgram = BuildPieceProgram(*noneIr);
    ASSERT_TRUE(noneProgram.has_value());
    EXPECT_FALSE(noneProgram->capabilities().Requires(
        ProgramCapability::VoiceEffectChain));

    ProjectInput emptyDeclared = ChainWorld();
    Declare(&emptyDeclared.nodes[kSound], SignalGraph{});
    const std::optional<PieceIR> emptyIr = BuildChainIr(emptyDeclared);
    ASSERT_TRUE(emptyIr.has_value());
    const std::optional<PieceProgram> emptyProgram =
        BuildPieceProgram(*emptyIr);
    ASSERT_TRUE(emptyProgram.has_value());
    EXPECT_FALSE(emptyProgram->capabilities().Requires(
        ProgramCapability::VoiceEffectChain))
        << "an empty chain needs no effect host";

    ProjectInput populated = ChainWorld();
    Declare(&populated.nodes[kSound], LinearGraph(1));
    const std::optional<PieceIR> populatedIr = BuildChainIr(populated);
    ASSERT_TRUE(populatedIr.has_value());
    const std::optional<PieceProgram> populatedProgram =
        BuildPieceProgram(*populatedIr);
    ASSERT_TRUE(populatedProgram.has_value());
    EXPECT_TRUE(populatedProgram->capabilities().Requires(
        ProgramCapability::VoiceEffectChain));
}

}  // namespace
}  // namespace harkenprojectschema
