#include "harken_project_schema/EffectChainSelection.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace harkenprojectschema {
namespace {

// One clip's placement facts, gathered so occurrences can be enumerated in the
// normalized order the facts already fix.
struct ClipPlacement {
    StableId trackId = kUnmintedStableId;
    ContentRef content;
};

// One object's effect-graph declaration, as the walk sees it.
struct Declaration {
    bool declared = false;
    const SignalGraph* graph = nullptr;
    AuthoringGuid guid{};
};

// Orders the selected graph's nodes into their execution series.
//
// The series is the unique path through the graph. A node with two inbound edges
// merges paths and one with two outbound edges splits them; either makes the
// order ambiguous, so both are refused rather than resolved by a tiebreak. Once
// every node has at most one edge each way, a single path exists only if there is
// exactly one node with no inbound edge and the walk from it reaches every node,
// which is what the edge count and the visit count together prove.
//
// Returns true and fills `order` on success. On failure it raises exactly one
// diagnostic naming the offending point and returns false.
bool LinearizeGraph(const SignalGraph& graph, const AuthoringGuid& owner,
                    std::vector<const EffectNode*>* order,
                    std::vector<Diagnostic>* out) {
    order->clear();
    if (graph.nodes.empty()) {
        return true;
    }

    std::map<GraphNodeId, const EffectNode*> nodeById;
    for (const EffectNode& node : graph.nodes) {
        nodeById.emplace(node.nodeId, &node);
    }

    // Successor and in-degree per node. The front-end already proved every edge
    // endpoint resolves and node ids are unique, so every lookup below hits.
    std::map<GraphNodeId, GraphNodeId> successor;
    std::map<GraphNodeId, std::uint32_t> inDegree;
    std::map<GraphNodeId, std::uint32_t> outDegree;
    for (const EffectNode& node : graph.nodes) {
        inDegree.emplace(node.nodeId, 0u);
        outDegree.emplace(node.nodeId, 0u);
    }
    for (const GraphEdge& edge : graph.edges) {
        const std::uint32_t fromOut = ++outDegree[edge.fromNodeId];
        const std::uint32_t toIn = ++inDegree[edge.toNodeId];
        if (fromOut > 1 || toIn > 1) {
            Diagnostic d;
            d.code = DiagnosticCode::EffectChainNodeNotLinear;
            d.primary = owner;
            // The node that branches: the splitting source, or the merging target.
            d.detail = fromOut > 1 ? edge.fromNodeId : edge.toNodeId;
            out->push_back(d);
            return false;
        }
        successor.emplace(edge.fromNodeId, edge.toNodeId);
    }

    // A single path over n nodes has exactly n-1 edges. Fewer means disjoint
    // runs; more is impossible once every out-degree is at most one.
    if (graph.edges.size() + 1 != graph.nodes.size()) {
        Diagnostic d;
        d.code = DiagnosticCode::EffectChainNotConnected;
        d.primary = owner;
        d.detail = graph.nodes.size();
        out->push_back(d);
        return false;
    }

    // The head is the one node nothing feeds. Exactly one exists in a path; a
    // graph with none is a cycle, which the front-end already refused.
    const EffectNode* head = nullptr;
    for (const EffectNode& node : graph.nodes) {
        if (inDegree[node.nodeId] == 0) {
            if (head != nullptr) {
                Diagnostic d;
                d.code = DiagnosticCode::EffectChainNotConnected;
                d.primary = owner;
                d.detail = graph.nodes.size();
                out->push_back(d);
                return false;
            }
            head = &node;
        }
    }
    if (head == nullptr) {
        Diagnostic d;
        d.code = DiagnosticCode::EffectChainNotConnected;
        d.primary = owner;
        d.detail = graph.nodes.size();
        out->push_back(d);
        return false;
    }

    order->reserve(graph.nodes.size());
    const EffectNode* cursor = head;
    while (cursor != nullptr) {
        order->push_back(cursor);
        const auto next = successor.find(cursor->nodeId);
        if (next == successor.end()) {
            break;
        }
        cursor = nodeById.find(next->second)->second;
    }

    if (order->size() != graph.nodes.size()) {
        // The walk left nodes unvisited, so the edge count was spent on a
        // separate run rather than on one path through all of them.
        Diagnostic d;
        d.code = DiagnosticCode::EffectChainNotConnected;
        d.primary = owner;
        d.detail = graph.nodes.size();
        out->push_back(d);
        order->clear();
        return false;
    }
    return true;
}

// Compiles a linearized node order into a chain artifact: one entry and one
// parameter block per node, in series order, plus the aggregated demand. Entry i
// points at block i — a chain does not share blocks between entries at this
// stage, so recognizing two identical payloads as one is left to a later
// content-level pass.
CompiledEffectChain CompileChain(StableId owner,
                                const std::vector<const EffectNode*>& order) {
    CompiledEffectChain chain;
    chain.owner = owner;
    chain.entries.reserve(order.size());
    chain.paramBlocks.reserve(order.size());

    // Demand aggregated per effect type, kept in a map so the rows come out
    // ascending by effect type regardless of the order the chain names them.
    std::map<EffectTypeId, EffectTypeDemand> demandByType;
    for (const EffectNode* node : order) {
        CompiledEffectChainEntry entry;
        entry.stableNodeId = node->nodeId;
        entry.effectId = node->effectId;
        entry.effectType = node->effectType;
        entry.paramBlockRef = static_cast<std::uint32_t>(chain.paramBlocks.size());
        entry.flags = 0;
        chain.entries.push_back(entry);

        EffectParamBlock block;
        block.params = node->params;
        chain.paramBlocks.push_back(std::move(block));

        EffectTypeDemand& demand = demandByType[node->effectType];
        demand.effectType = node->effectType;
        ++demand.instanceCount;
        for (const EffectParam& param : node->params) {
            demand.requiredParameterCount = std::max(
                demand.requiredParameterCount,
                static_cast<std::uint64_t>(param.paramId) + 1u);
        }
    }

    chain.resources.maxChainLength = static_cast<std::uint32_t>(order.size());
    // A chain with no entries requires nothing of a host; a non-empty one must
    // run somewhere that hosts per-voice inserts.
    chain.resources.requiredCapabilities =
        order.empty() ? 0u : kEffectChainRequiresVoiceHost;
    chain.resources.perTypeDemand.reserve(demandByType.size());
    for (const auto& row : demandByType) {
        chain.resources.perTypeDemand.push_back(row.second);
    }
    return chain;
}

}  // namespace

EffectChainSelectionResult BuildEffectChainSelection(
    const CanonicalProject& project, const NormalizedFacts& facts) {
    EffectChainSelectionResult result;

    std::map<StableId, Declaration> declarationById;
    std::map<StableId, ClipPlacement> clipPlacement;
    for (const ContentObject& obj : project.objects) {
        const Chassis& c = obj.chassis();
        // Graph content the object never declares is refused rather than read
        // either way. Treating it as declared would make the existence bit
        // derivable from the node list, which is exactly the ambiguity the bit
        // exists to remove; ignoring it would drop authored content silently. The
        // check covers every object, not only the ones some occurrence selects,
        // so content that is currently unplaced is still caught.
        const bool hasSignalGraph = c.presence.Has(
            harkenplayableproperties::PlayablePropertyIdV1::SignalGraph);
        if (!hasSignalGraph &&
            (!c.signalGraph.nodes.empty() || !c.signalGraph.edges.empty())) {
            Diagnostic d;
            d.code = DiagnosticCode::EffectGraphContentWithoutDeclaration;
            d.primary = c.guid;
            // High word: the node count; low word: the edge count.
            d.detail = (static_cast<std::uint64_t>(c.signalGraph.nodes.size())
                        << 32) |
                       static_cast<std::uint64_t>(c.signalGraph.edges.size());
            result.diagnostics.push_back(d);
        }
        declarationById.emplace(
            c.id, Declaration{hasSignalGraph, &c.signalGraph, c.guid});
        if (obj.kind == ContentKind::Track) {
            for (const Clip& clip : obj.track.clips) {
                clipPlacement.emplace(
                    clip.id, ClipPlacement{obj.track.chassis.id, clip.contentRef});
            }
        }
    }
    if (!result.ok()) {
        // Declaration consistency is settled before anything is selected: a
        // project holding content nobody declared has no well-defined selection.
        return result;
    }

    EffectChainSelectionFacts out;
    // Compiled chains are keyed by winning owner so two occurrences selecting the
    // same declaration share one artifact. The ordered map is what makes the
    // emitted table ascending by owner id, and the occurrence indices are assigned
    // from it after the walk.
    std::map<StableId, CompiledEffectChain> chainByOwner;

    for (const TrackNormalization& track : facts.tracks) {
        for (std::size_t ordinal = 0; ordinal < track.clipsByOrder.size();
             ++ordinal) {
            const StableId clipId = track.clipsByOrder[ordinal];
            const auto placementIt = clipPlacement.find(clipId);
            if (placementIt == clipPlacement.end()) {
                continue;  // a clip the model does not define is impossible here
            }
            const StableId subject = placementIt->second.content.target;

            OccurrenceEffectChainSelection occ;
            occ.trackId = track.track;
            occ.clipId = clipId;
            occ.occurrenceOrdinal = static_cast<std::uint32_t>(ordinal);
            occ.subjectId = subject;
            const SignalGraph* selected = nullptr;
            AuthoringGuid selectedGuid{};
            const auto declaration = declarationById.find(subject);
            if (declaration != declarationById.end() && declaration->second.declared) {
                occ.declared = true;
                occ.owner = subject;
                selected = declaration->second.graph;
                selectedGuid = declaration->second.guid;
            }

            if (selected != nullptr &&
                chainByOwner.find(occ.owner) == chainByOwner.end()) {
                // First occurrence to select this owner compiles its chain; a
                // graph that cannot be linearized refuses the whole build.
                std::vector<const EffectNode*> order;
                if (!LinearizeGraph(*selected, selectedGuid, &order,
                                    &result.diagnostics)) {
                    return result;
                }
                chainByOwner.emplace(occ.owner,
                                     CompileChain(occ.owner, order));
            }
            out.occurrences.push_back(std::move(occ));
        }
    }

    // Emit the table ascending by owner, then resolve each occurrence's index
    // against it. Doing this after the walk is what decouples the index from the
    // order occurrences happened to be visited in.
    std::map<StableId, std::uint32_t> chainIndexByOwner;
    out.chains.reserve(chainByOwner.size());
    for (auto& row : chainByOwner) {
        chainIndexByOwner.emplace(
            row.first, static_cast<std::uint32_t>(out.chains.size()));
        out.chains.push_back(std::move(row.second));
    }
    for (OccurrenceEffectChainSelection& occ : out.occurrences) {
        if (occ.declared) {
            occ.chainIndex = chainIndexByOwner.find(occ.owner)->second;
        }
    }

    result.facts = std::move(out);
    return result;
}

}  // namespace harkenprojectschema
