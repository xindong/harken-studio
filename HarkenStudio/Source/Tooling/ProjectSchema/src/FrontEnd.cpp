#include "harken_project_schema/FrontEnd.h"

#include "harken_project_schema/FixedSlotLowerer.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace harkenprojectschema {
namespace {

// A diagnostic under construction. The free helpers below fill the fields a code
// needs and leave the rest at their defaults, so each emission site reads as the
// one fact it is reporting.
Diagnostic Make(DiagnosticCode code, const AuthoringGuid& primary) {
    Diagnostic d;
    d.code = code;
    d.primary = primary;
    return d;
}

// Orders diagnostics by a total key so the list is identical for identical
// input regardless of how the stages happened to encounter the failures. Code
// first groups by the rule broken; the GUIDs and the numeric detail break
// remaining ties. path is not part of the key — two diagnostics that agree on
// every keyed field keep their encounter order, which is itself deterministic.
bool DiagnosticLess(const Diagnostic& a, const Diagnostic& b) {
    if (a.code != b.code) {
        return static_cast<std::uint32_t>(a.code) <
               static_cast<std::uint32_t>(b.code);
    }
    if (a.primary != b.primary) {
        return a.primary < b.primary;
    }
    if (a.secondary != b.secondary) {
        return a.secondary < b.secondary;
    }
    return a.detail < b.detail;
}

// A distinguishing number for the NullGuid diagnostic: an object authored with
// the null GUID cannot be named by GUID, so the detail records what kind of
// thing carried it instead.
enum class NullGuidWhere : std::uint64_t {
    Node = 0,
    Clip = 1,
    Marker = 2,
};

bool IsValid(ContentKind kind) {
    switch (kind) {
        case ContentKind::Layer:
        case ContentKind::Sound:
        case ContentKind::Container:
        case ContentKind::Track:
        case ContentKind::Piece:
            return true;
    }
    return false;
}

bool IsValid(PieceTimeKind kind) {
    return kind == PieceTimeKind::Absolute || kind == PieceTimeKind::Musical;
}

bool IsValid(StartPointKind kind) {
    return kind == StartPointKind::Entry || kind == StartPointKind::Marker;
}

bool IsValid(MarkerKind kind) {
    return kind == MarkerKind::Custom || kind == MarkerKind::Exit;
}

using harkenplayableproperties::ChassisPresenceV1;
using harkenplayableproperties::PlayablePropertyIdV1;

std::uint32_t NonCanonicalAbsentField(
    const PlayablePropertyPayloadV1& payload,
    ChassisPresenceV1 presence) {
    const auto absent = [&](PlayablePropertyIdV1 id) {
        return !presence.Has(id);
    };
    if (absent(PlayablePropertyIdV1::Volume) &&
        std::bit_cast<std::uint32_t>(payload.volume) != 0) return 1;
    if (absent(PlayablePropertyIdV1::Pitch) &&
        std::bit_cast<std::uint32_t>(payload.pitch) != 0) return 2;
    if (absent(PlayablePropertyIdV1::MakeUpGain) &&
        std::bit_cast<std::uint32_t>(payload.makeUpGain) != 0) return 3;
    if (absent(PlayablePropertyIdV1::LoopCount) && payload.loopCount != 1) return 4;
    if (absent(PlayablePropertyIdV1::StreamingMode) &&
        payload.streamingMode != 0) return 5;
    if (absent(PlayablePropertyIdV1::OutputBusVolume) &&
        std::bit_cast<std::uint32_t>(payload.outputBusVolume) != 0) return 6;
    if (absent(PlayablePropertyIdV1::MainOutputBusRef) &&
        payload.mainOutputBusRef != harkenstudio::ObjectId{}) return 7;
    if (absent(PlayablePropertyIdV1::Sends) && !payload.sends.empty()) return 8;
    if (absent(PlayablePropertyIdV1::SpatialPanning) &&
        std::bit_cast<std::uint32_t>(payload.spatial.panning) != 0) return 9;
    if (absent(PlayablePropertyIdV1::SpatialSpread) &&
        std::bit_cast<std::uint32_t>(payload.spatial.spread) != 0) return 10;
    if (absent(PlayablePropertyIdV1::SpatialIs3DEnabled) &&
        payload.spatial.is3DEnabled) return 11;
    if (absent(PlayablePropertyIdV1::SpatialAttenuationDistanceScaling) &&
        std::bit_cast<std::uint32_t>(
            payload.spatial.attenuationDistanceScaling) != 0x3f800000u) return 12;
    if (absent(PlayablePropertyIdV1::SpatialAttenuationRef) &&
        payload.spatial.attenuationRef != harkenstudio::ObjectId{}) return 14;
    if (absent(PlayablePropertyIdV1::SignalGraph) &&
        (!payload.signalGraph.nodes.empty() ||
         !payload.signalGraph.edges.empty())) return 16;
    if (absent(PlayablePropertyIdV1::SpatializationMix) &&
        std::bit_cast<std::uint32_t>(payload.spatial.spatializationMix) !=
            0x3f800000u) return 17;
    return 0;
}

void ValidatePropertyPayload(const PlayablePropertyPayloadV1& payload,
                             ChassisPresenceV1 presence,
                             const AuthoringGuid& owner,
                             const AuthoringGuid& occurrence,
                             std::vector<Diagnostic>* out) {
    if (!presence.IsValid()) {
        Diagnostic d = Make(DiagnosticCode::UnknownPlayablePropertyBits, owner);
        d.secondary = occurrence;
        d.detail = presence.bits &
            ~harkenplayableproperties::kPlayablePropertyKnownMaskV1;
        out->push_back(d);
        return;
    }
    const std::uint32_t field = NonCanonicalAbsentField(payload, presence);
    if (field != 0) {
        Diagnostic d = Make(DiagnosticCode::NonCanonicalAbsentProperty, owner);
        d.secondary = occurrence;
        d.detail = field;
        out->push_back(d);
    }
}

// ---- Signal graph structural validation ---------------------------------
//
// Local to one object's own effect graph. All checks may fire together except
// the cycle search, which is only meaningful once node ids are unique and every
// edge endpoint resolves — an ill-formed id set or a dangling edge has no
// well-defined adjacency. The detail convention differs per code and is noted at
// each site; determinism does not depend on it, since the diagnostics are built
// in authored order and the final sort is stable over equal keys.

// Detects the first cycle, in deterministic traversal order, over a graph
// already known to have unique node ids and resolving edges. On a back edge it
// reports the node id where the cycle closes.
bool FindGraphCycle(const std::vector<EffectNode>& nodes,
                    const std::vector<GraphEdge>& edges, GraphNodeId* ringNode) {
    std::map<GraphNodeId, std::size_t> indexById;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        indexById.emplace(nodes[i].nodeId, i);
    }
    std::vector<std::vector<std::size_t>> adjacency(nodes.size());
    for (const GraphEdge& edge : edges) {
        adjacency[indexById.at(edge.fromNodeId)].push_back(
            indexById.at(edge.toNodeId));
    }

    enum Color { kWhite = 0, kGray = 1, kBlack = 2 };
    std::vector<int> color(nodes.size(), kWhite);
    struct Frame {
        std::size_t node = 0;
        std::size_t next = 0;
    };
    for (std::size_t root = 0; root < nodes.size(); ++root) {
        if (color[root] != kWhite) {
            continue;
        }
        std::vector<Frame> stack;
        color[root] = kGray;
        stack.push_back(Frame{root, 0});
        while (!stack.empty()) {
            Frame& frame = stack.back();
            if (frame.next == adjacency[frame.node].size()) {
                color[frame.node] = kBlack;
                stack.pop_back();
                continue;
            }
            const std::size_t child = adjacency[frame.node][frame.next++];
            if (color[child] == kGray) {
                *ringNode = nodes[child].nodeId;
                return true;
            }
            if (color[child] == kWhite) {
                color[child] = kGray;
                stack.push_back(Frame{child, 0});
            }
        }
    }
    return false;
}

// Proves the object's effect graph is a structurally legal DAG. Stores nothing;
// only raises refusals. The graph is authoring data this front-end does not
// interpret — it checks structure and leaves meaning to a later stage.
void ValidateSignalGraph(const InputNode& node, std::vector<Diagnostic>* out) {
    const SignalGraph& graph = node.signalGraph;

    bool idsClean = true;
    std::set<GraphNodeId> seenIds;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const EffectNode& n = graph.nodes[i];
        if (n.nodeId == kReservedGraphNodeId) {
            Diagnostic d = Make(DiagnosticCode::GraphNodeIdReserved, node.guid);
            d.detail = i;  // authored node index (the id itself is the reserved 0)
            out->push_back(d);
            idsClean = false;
        } else if (!seenIds.insert(n.nodeId).second) {
            Diagnostic d = Make(DiagnosticCode::GraphNodeIdDuplicate, node.guid);
            d.detail = n.nodeId;  // the colliding node id
            out->push_back(d);
            idsClean = false;
        }

        if (n.effectType == kReservedEffectType) {
            Diagnostic d =
                Make(DiagnosticCode::GraphEffectTypeReserved, node.guid);
            d.detail = i;  // authored node index
            out->push_back(d);
        }

        std::set<std::uint32_t> seenParams;
        for (const EffectParam& param : n.params) {
            if (!seenParams.insert(param.paramId).second) {
                Diagnostic d =
                    Make(DiagnosticCode::GraphParamIdDuplicate, node.guid);
                // High word: authored node index; low word: the repeated param id.
                d.detail =
                    (static_cast<std::uint64_t>(i) << 32) | param.paramId;
                out->push_back(d);
            }
        }
    }

    // An edge endpoint must name a node the graph declares — declared even if
    // that node was itself refused above. A dangling endpoint leaves the
    // adjacency ill-formed, so the cycle search is skipped when one is found.
    bool edgesResolve = true;
    std::set<GraphNodeId> declared;
    for (const EffectNode& n : graph.nodes) {
        declared.insert(n.nodeId);
    }
    for (const GraphEdge& edge : graph.edges) {
        if (declared.find(edge.fromNodeId) == declared.end()) {
            Diagnostic d =
                Make(DiagnosticCode::GraphEdgeEndpointMissing, node.guid);
            d.detail = edge.fromNodeId;  // the missing node id
            out->push_back(d);
            edgesResolve = false;
        }
        if (declared.find(edge.toNodeId) == declared.end()) {
            Diagnostic d =
                Make(DiagnosticCode::GraphEdgeEndpointMissing, node.guid);
            d.detail = edge.toNodeId;  // the missing node id
            out->push_back(d);
            edgesResolve = false;
        }
    }

    if (idsClean && edgesResolve) {
        GraphNodeId ringNode = kReservedGraphNodeId;
        if (FindGraphCycle(graph.nodes, graph.edges, &ringNode)) {
            Diagnostic d = Make(DiagnosticCode::GraphCycle, node.guid);
            d.detail = ringNode;  // a node id on the cycle
            out->push_back(d);
        }
    }
}

// ---- Stage 1: parse ------------------------------------------------------
//
// Per-node, purely local checks: a field authored where its kind forbids it, a
// Piece missing what a Piece requires, the reserved marker id used as a real marker,
// and a Piece's own start point pointing at a marker it does not list. Nothing here
// looks at another node, so the order is exactly the authored node order.
void ParseNode(const InputNode& node, std::vector<Diagnostic>* out) {
    if (!IsValid(node.kind)) {
        Diagnostic d = Make(DiagnosticCode::InvalidContentKind, node.guid);
        d.detail = static_cast<std::uint64_t>(node.kind);
        out->push_back(d);
    }
    if (IsNull(node.guid)) {
        Diagnostic d = Make(DiagnosticCode::NullGuid, node.guid);
        d.detail = static_cast<std::uint64_t>(NullGuidWhere::Node);
        out->push_back(d);
    }

    ChassisPresenceV1 definitionPresence = node.presence;
    if (node.hasSignalGraph) {
        definitionPresence.Set(PlayablePropertyIdV1::SignalGraph);
    }
    ValidatePropertyPayload(node, definitionPresence, node.guid, kNullGuid, out);
    if (node.containerChildOccurrences.compatibilityOrdinalOverflow) {
        out->push_back(Make(DiagnosticCode::OccurrenceOrdinalOverflow,
                            node.guid));
    }
    if (!node.containerChildOccurrences.empty() &&
        node.kind != ContentKind::Container) {
        out->push_back(Make(
            DiagnosticCode::ContainerChildOccurrencesOnNonContainer,
            node.guid));
    }

    // Placement ledger belongs to a Track and nowhere else.
    if (!node.clips.empty() && node.kind != ContentKind::Track) {
        out->push_back(Make(DiagnosticCode::ClipsOnNonTrack, node.guid));
    }

    // Time-side authoring belongs to a Piece and nowhere else. A Track carrying
    // a time spec is refused here — the one place the "no clock on a Track"
    // contract is enforced against an input that tried to spell it.
    if (node.timeSpec.has_value() && node.kind != ContentKind::Piece) {
        out->push_back(Make(DiagnosticCode::TimeSpecOnNonPiece, node.guid));
    }
    if (node.startPoint.has_value() && node.kind != ContentKind::Piece) {
        out->push_back(Make(DiagnosticCode::StartPointOnNonPiece, node.guid));
    }
    if (!node.markers.empty() && node.kind != ContentKind::Piece) {
        out->push_back(Make(DiagnosticCode::MarkersOnNonPiece, node.guid));
    }
    if (node.entryMarker && node.kind != ContentKind::Piece) {
        out->push_back(Make(DiagnosticCode::EntryMarkerOnNonPiece, node.guid));
    }

    if (node.timeSpec.has_value() && !IsValid(node.timeSpec->kind)) {
        Diagnostic d =
            Make(DiagnosticCode::InvalidPieceTimeKind, node.guid);
        d.detail = static_cast<std::uint64_t>(node.timeSpec->kind);
        out->push_back(d);
    }
    if (node.startPoint.has_value()) {
        const InputStartPoint& sp = *node.startPoint;
        if (!IsValid(sp.kind)) {
            Diagnostic d =
                Make(DiagnosticCode::InvalidStartPointKind, node.guid);
            d.detail = static_cast<std::uint64_t>(sp.kind);
            out->push_back(d);
        } else if (sp.kind == StartPointKind::Entry &&
                   sp.markerId != kReservedMarkerId) {
            Diagnostic d =
                Make(DiagnosticCode::EntryStartPointMarkerPresent, node.guid);
            d.detail = sp.markerId;
            out->push_back(d);
        }
    }

    // Markers carry authoring identity and may never use the reserved id, whatever
    // the node's kind — checked before the Piece-shape checks so a stray marker on
    // a non-Piece is still caught for the reserved id it used.
    std::set<MarkerId> markerIds;
    for (const InputMarker& marker : node.markers) {
        if (IsNull(marker.guid)) {
            Diagnostic d = Make(DiagnosticCode::NullGuid, node.guid);
            d.secondary = marker.guid;
            d.detail = static_cast<std::uint64_t>(NullGuidWhere::Marker);
            out->push_back(d);
        }
        if (marker.markerId == kReservedMarkerId) {
            Diagnostic d = Make(DiagnosticCode::ReservedMarkerId, node.guid);
            d.secondary = marker.guid;
            out->push_back(d);
        }
        if (!IsValid(marker.kind)) {
            Diagnostic d = Make(DiagnosticCode::InvalidMarkerKind, node.guid);
            d.secondary = marker.guid;
            d.detail = static_cast<std::uint64_t>(marker.kind);
            out->push_back(d);
        }
        if (node.kind == ContentKind::Piece &&
            marker.markerId != kReservedMarkerId &&
            !markerIds.insert(marker.markerId).second) {
            Diagnostic d = Make(DiagnosticCode::DuplicateMarkerId, node.guid);
            d.secondary = marker.guid;
            d.detail = marker.markerId;
            out->push_back(d);
        }
    }

    // A Piece must define its coordinate system and where it begins, and its
    // maps must match its kind. A non-Piece has already been refused for any of
    // these it carried, so this only runs the positive Piece rules.
    if (node.kind == ContentKind::Piece) {
        if (!node.timeSpec.has_value()) {
            out->push_back(Make(DiagnosticCode::PieceMissingTimeSpec, node.guid));
        } else {
            const InputPieceTimeSpec& spec = *node.timeSpec;
            if (spec.kind == PieceTimeKind::Musical) {
                if (spec.tempoMap.empty() || spec.meterMap.empty()) {
                    out->push_back(
                        Make(DiagnosticCode::MusicalMapEmpty, node.guid));
                }
            } else if (spec.kind == PieceTimeKind::Absolute) {
                // Absolute time carries no maps; maps present here would be
                // meaningless and are refused so the discriminant stays honest.
                if (!spec.tempoMap.empty() || !spec.meterMap.empty()) {
                    out->push_back(
                        Make(DiagnosticCode::NonMusicalMapPresent, node.guid));
                }
            }
        }

        if (!node.startPoint.has_value()) {
            out->push_back(
                Make(DiagnosticCode::PieceMissingStartPoint, node.guid));
        } else {
            const InputStartPoint& sp = *node.startPoint;
            if (sp.kind == StartPointKind::Marker) {
                if (sp.markerId == kReservedMarkerId) {
                    // The entry point is StartPointKind::Entry, never "the marker
                    // with id 0". A Marker start point at id 0 is the entry point
                    // wearing a marker's clothes, and is refused.
                    out->push_back(
                        Make(DiagnosticCode::StartPointMarkerReserved, node.guid));
                } else {
                    bool found = false;
                    for (const InputMarker& marker : node.markers) {
                        if (marker.markerId == sp.markerId) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        Diagnostic d = Make(DiagnosticCode::StartPointMarkerMissing,
                                            node.guid);
                        d.detail = sp.markerId;
                        out->push_back(d);
                    }
                }
            }
        }
    }

    // Clips carry authoring identity too; a null clip GUID is refused here so
    // stage 2 is not asked to mint one.
    std::map<std::uint32_t, AuthoringGuid> clipOccurrenceOrdinals;
    for (const InputClip& clip : node.clips) {
        if (IsNull(clip.guid)) {
            Diagnostic d = Make(DiagnosticCode::NullGuid, node.guid);
            d.secondary = clip.guid;
            d.detail = static_cast<std::uint64_t>(NullGuidWhere::Clip);
            out->push_back(d);
        }
        ValidatePropertyPayload(clip, clip.presence, node.guid, clip.guid, out);
        if (node.kind == ContentKind::Track) {
            const auto ordinal = clipOccurrenceOrdinals.find(
                clip.stableOccurrenceOrdinal);
            if (ordinal != clipOccurrenceOrdinals.end()) {
                Diagnostic d = Make(DiagnosticCode::DuplicateOccurrenceOrdinal,
                                    node.guid);
                d.secondary = clip.guid;
                d.detail = clip.stableOccurrenceOrdinal;
                out->push_back(d);
            } else {
                clipOccurrenceOrdinals.emplace(
                    clip.stableOccurrenceOrdinal, clip.guid);
            }
        }
    }

    std::map<std::uint32_t, AuthoringGuid> occurrenceOrdinals;
    for (const InputContainerChildOccurrence& edge :
         node.containerChildOccurrences) {
        ValidatePropertyPayload(edge, edge.presence, node.guid, edge.childGuid,
                                out);
        const auto ordinal = occurrenceOrdinals.find(
            edge.stableOccurrenceOrdinal);
        if (node.kind == ContentKind::Container &&
            ordinal != occurrenceOrdinals.end()) {
            Diagnostic d = Make(DiagnosticCode::DuplicateOccurrenceOrdinal,
                                node.guid);
            d.secondary = edge.childGuid;
            d.detail = edge.stableOccurrenceOrdinal;
            out->push_back(d);
        }
        occurrenceOrdinals.emplace(edge.stableOccurrenceOrdinal,
                                   edge.childGuid);
    }

    // The one rule the front-end enforces over the common chassis: a send entry
    // may not carry the reserved bus id 0, which means "unset" and can never name
    // a real bus. Each offending entry is reported separately, its index carried
    // in the detail so two bad sends on one node sort into a stable order.
    for (std::size_t i = 0; i < node.sends.size(); ++i) {
        if (node.sends[i].targetBusId == kReservedBusId) {
            Diagnostic d = Make(DiagnosticCode::SendTargetBusUnset, node.guid);
            d.detail = i;
            out->push_back(d);
        }
    }

    // The object's effect graph is validated structurally here too — it is
    // per-node authoring data with no cross-node references at this stage.
    ValidateSignalGraph(node, out);
}

RuntimeIdentityKindV1 RuntimeKind(ContentKind kind) {
    switch (kind) {
        case ContentKind::Sound:
            return RuntimeIdentityKindV1::Sound;
        case ContentKind::Container:
            return RuntimeIdentityKindV1::Container;
        case ContentKind::Track:
            return RuntimeIdentityKindV1::Track;
        case ContentKind::Piece:
            return RuntimeIdentityKindV1::Piece;
        case ContentKind::Layer:
            return RuntimeIdentityKindV1::Layer;
    }
    return RuntimeIdentityKindV1::Invalid;
}

void ValidateCanonicalIdentities(const ProjectInput& input,
                                 std::vector<Diagnostic>* out) {
    std::set<AuthoringGuid> guids;
    const auto validate = [&](AuthoringGuid guid, AuthoringGuid owner,
                               RuntimeIdentityKindV1 kind) {
        if (IsNull(guid)) {
            out->push_back(Make(DiagnosticCode::NullGuid, owner));
        } else if (!guids.insert(guid).second) {
            out->push_back(Make(DiagnosticCode::DuplicateGuid, guid));
        } else if (input.identityIndex) {
            const auto* claim = input.identityIndex->find(guid);
            if (!claim || claim->kind != kind || claim->sourceKey.owningGuid != owner)
                out->push_back(Make(DiagnosticCode::DuplicateStableId, guid));
        }
    };
    for (const InputNode& node : input.nodes) {
        validate(node.guid, node.guid, RuntimeKind(node.kind));
        for (const InputClip& clip : node.clips) {
            validate(clip.guid, node.guid, RuntimeIdentityKindV1::TimelineClip);
        }
        for (const InputMarker& marker : node.markers) {
            validate(marker.guid, node.guid, RuntimeIdentityKindV1::TimelineMarker);
        }
        if (node.entryMarker) {
            validate(node.entryMarker->guid, node.guid,
                     RuntimeIdentityKindV1::TimelineMarker);
        }
    }
}

// ---- Stage 3: graph ------------------------------------------------------

// A resolved view of the nodes, keyed by GUID for the edge and reference
// lookups. Built only once identity is clean, so GUIDs are unique and the map
// is unambiguous. Ordered by GUID, but used only for lookup — never iterated to
// produce output.
struct NodeIndex {
    std::map<AuthoringGuid, std::size_t> byGuid;

    const InputNode* Find(const ProjectInput& input,
                          const AuthoringGuid& guid) const {
        const auto it = byGuid.find(guid);
        if (it == byGuid.end()) {
            return nullptr;
        }
        return &input.nodes[it->second];
    }
};

NodeIndex BuildNodeIndex(const ProjectInput& input) {
    NodeIndex index;
    for (std::size_t i = 0; i < input.nodes.size(); ++i) {
        index.byGuid.emplace(input.nodes[i].guid, i);
    }
    return index;
}

// Detects the first ownership cycle in authored traversal order and records its
// ring. The graph is the resolved ownership edges (a child that does not exist
// is not an edge); a three-colour DFS reports a back edge to a node still on the
// stack. Reporting one cycle is enough — any cycle fails the build — and taking
// the first in a fixed traversal keeps the report deterministic.
class CycleFinder {
public:
    CycleFinder(const ProjectInput& input, const NodeIndex& index,
                bool occurrenceGraph, DiagnosticCode code)
        : input_(input),
          index_(index),
          occurrenceGraph_(occurrenceGraph),
          code_(code),
          color_(input.nodes.size(), kWhite) {}

    bool Find(Diagnostic* out) {
        for (std::size_t i = 0; i < input_.nodes.size(); ++i) {
            if (color_[i] == kWhite) {
                if (Visit(i, out)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    static constexpr int kWhite = 0;
    static constexpr int kGray = 1;
    static constexpr int kBlack = 2;

    struct VisitFrame {
        std::size_t nodeIdx = 0;
        std::size_t nextChild = 0;
    };

    std::size_t ChildCount(const InputNode& node) const {
        return occurrenceGraph_ && node.kind == ContentKind::Container
                   ? node.containerChildOccurrences.size()
                   : occurrenceGraph_ ? 0 : node.ownedChildren.size();
    }

    const AuthoringGuid& ChildGuid(const InputNode& node,
                                   std::size_t index) const {
        return occurrenceGraph_
                   ? node.containerChildOccurrences[index].childGuid
                   : node.ownedChildren[index].childGuid;
    }

    bool Visit(std::size_t nodeIdx, Diagnostic* out) {
        color_[nodeIdx] = kGray;
        stack_.push_back(nodeIdx);
        std::vector<VisitFrame> visits;
        visits.push_back(VisitFrame{nodeIdx, 0});

        while (!visits.empty()) {
            VisitFrame& visit = visits.back();
            const InputNode& node = input_.nodes[visit.nodeIdx];
            if (visit.nextChild == ChildCount(node)) {
                color_[visit.nodeIdx] = kBlack;
                visits.pop_back();
                stack_.pop_back();
                continue;
            }

            const AuthoringGuid& childGuid =
                ChildGuid(node, visit.nextChild++);
            const auto it = index_.byGuid.find(childGuid);
            if (it == index_.byGuid.end()) {
                continue;  // dangling edge: reported elsewhere, not an edge here
            }
            const std::size_t childIdx = it->second;
            if (color_[childIdx] == kGray) {
                // Back edge: the ring runs from the child's first appearance on
                // the stack to the current node and closes back to the child.
                out->code = code_;
                out->primary = childGuid;
                std::size_t start = 0;
                for (; start < stack_.size(); ++start) {
                    if (stack_[start] == childIdx) {
                        break;
                    }
                }
                for (std::size_t k = start; k < stack_.size(); ++k) {
                    out->path.push_back(input_.nodes[stack_[k]].guid);
                }
                out->path.push_back(childGuid);
                return true;
            }
            if (color_[childIdx] == kWhite) {
                color_[childIdx] = kGray;
                stack_.push_back(childIdx);
                visits.push_back(VisitFrame{childIdx, 0});
            }
        }
        return false;
    }

    const ProjectInput& input_;
    const NodeIndex& index_;
    bool occurrenceGraph_ = false;
    DiagnosticCode code_ = DiagnosticCode::StructuralCycle;
    std::vector<int> color_;
    std::vector<std::size_t> stack_;
};

// Validates the structural ownership graph: every edge resolves and is legal by
// the kind rule, every object is owned exactly once or is a declared root, and
// there is no ownership cycle. Returns whether any diagnostic was raised, which
// gates the placement pass — placement facts on a malformed structure would be
// a cascade of unrelated noise.
bool ValidateStructure(const ProjectInput& input, const NodeIndex& index,
                       std::vector<Diagnostic>* out) {
    const std::size_t before = out->size();

    // Owner counts, from resolved edges only. An edge whose child exists counts
    // as ownership even if the kind pairing is illegal, so an object owned only
    // through an illegal edge is not also mis-reported as an orphan.
    std::map<AuthoringGuid, std::uint64_t> ownerCount;
    std::map<AuthoringGuid, AuthoringGuid> firstOwner;
    for (const InputNode& node : input.nodes) {
        std::set<AuthoringGuid> seenChildren;
        for (const InputOwnershipEdge& edge : node.ownedChildren) {
            const AuthoringGuid& childGuid = edge.childGuid;
            if (!seenChildren.insert(childGuid).second) {
                Diagnostic d =
                    Make(DiagnosticCode::DuplicateOwnershipEdge, node.guid);
                d.secondary = childGuid;
                out->push_back(d);
                continue;
            }
            const InputNode* child = index.Find(input, childGuid);
            if (child == nullptr) {
                Diagnostic d = Make(DiagnosticCode::DanglingOwnership, node.guid);
                d.secondary = childGuid;
                out->push_back(d);
                continue;
            }
            if (!MayOwn(node.kind, child->kind)) {
                Diagnostic d = Make(DiagnosticCode::IllegalOwnership, node.guid);
                d.secondary = childGuid;
                // Owner kind in the high nibble, child kind in the low: enough
                // to read the forbidden pairing back out of the diagnostic.
                d.detail = (static_cast<std::uint64_t>(node.kind) << 4) |
                           static_cast<std::uint64_t>(child->kind);
                out->push_back(d);
            }
            const std::uint64_t seen = ownerCount[childGuid];
            ownerCount[childGuid] = seen + 1;
            if (seen == 0) {
                firstOwner[childGuid] = node.guid;
            }
        }
    }

    // Roots: each must resolve, and a root may not also be owned.
    std::map<AuthoringGuid, bool> isRoot;
    std::set<AuthoringGuid> seenRoots;
    for (const AuthoringGuid& rootGuid : input.roots) {
        if (!seenRoots.insert(rootGuid).second) {
            out->push_back(Make(DiagnosticCode::DuplicateRoot, rootGuid));
            continue;
        }
        if (index.Find(input, rootGuid) == nullptr) {
            out->push_back(Make(DiagnosticCode::UnknownRoot, rootGuid));
            continue;
        }
        isRoot[rootGuid] = true;
        if (ownerCount.find(rootGuid) != ownerCount.end()) {
            out->push_back(Make(DiagnosticCode::RootAlsoOwned, rootGuid));
        }
    }

    // Every node must be reachable as either a root or an owned child, and owned
    // by at most one parent. Walked in authored order so the diagnostics come
    // out in a fixed order before the final sort.
    for (const InputNode& node : input.nodes) {
        const std::uint64_t owners = ownerCount.count(node.guid) != 0
                                         ? ownerCount[node.guid]
                                         : 0;
        const bool root = isRoot.count(node.guid) != 0;
        if (owners > 1) {
            Diagnostic d = Make(DiagnosticCode::MultipleOwners, node.guid);
            d.secondary = firstOwner[node.guid];
            d.detail = owners;
            out->push_back(d);
        }
        if (!root && owners == 0) {
            out->push_back(
                Make(DiagnosticCode::OwnedNotRootNorOwned, node.guid));
        }
    }

    // A cycle is only sought once the edges are otherwise well-formed enough to
    // trust; but the finder ignores dangling edges anyway, so running it here is
    // safe and keeps the whole structural verdict in one place.
    Diagnostic cycle;
    if (CycleFinder(input, index, false, DiagnosticCode::StructuralCycle)
            .Find(&cycle)) {
        out->push_back(std::move(cycle));
    }

    return out->size() != before;
}

void ValidatePlacement(const ProjectInput& input, const NodeIndex& index,
                       std::vector<Diagnostic>* out) {
    for (const InputNode& node : input.nodes) {
        for (const InputClip& clip : node.clips) {
            const InputNode* target = index.Find(input, clip.targetGuid);
            if (target == nullptr) {
                Diagnostic d = Make(DiagnosticCode::DanglingContentRef, clip.guid);
                d.secondary = clip.targetGuid;
                out->push_back(d);
                continue;
            }
            if (!IsPlayableRefKind(target->kind)) {
                Diagnostic d =
                    Make(DiagnosticCode::IllegalContentRefType, clip.guid);
                d.secondary = clip.targetGuid;
                d.detail = static_cast<std::uint64_t>(target->kind);
                out->push_back(d);
                continue;
            }
            if (std::none_of(node.ownedChildren.begin(), node.ownedChildren.end(),
                             [&](const InputOwnershipEdge& edge) {
                                 return edge.childGuid == clip.targetGuid;
                             })) {
                Diagnostic d = Make(DiagnosticCode::IllegalOwnership, node.guid);
                d.secondary = clip.targetGuid;
                d.detail = clip.stableOccurrenceOrdinal;
                out->push_back(d);
            }
        }
        for (const InputContainerChildOccurrence& occurrence :
             node.containerChildOccurrences) {
            const InputNode* target =
                index.Find(input, occurrence.childGuid);
            if (target == nullptr) {
                Diagnostic d =
                    Make(DiagnosticCode::DanglingContentRef, node.guid);
                d.secondary = occurrence.childGuid;
                d.detail = occurrence.stableOccurrenceOrdinal;
                out->push_back(d);
                continue;
            }
            if (!MayOwn(ContentKind::Container, target->kind)) {
                Diagnostic d =
                    Make(DiagnosticCode::IllegalContentRefType, node.guid);
                d.secondary = occurrence.childGuid;
                d.detail = static_cast<std::uint64_t>(target->kind);
                out->push_back(d);
                continue;
            }
            bool owned = false;
            for (const InputOwnershipEdge& edge : node.ownedChildren) {
                if (edge.childGuid == occurrence.childGuid) {
                    owned = true;
                    break;
                }
            }
            if (!owned) {
                Diagnostic d = Make(DiagnosticCode::IllegalOwnership, node.guid);
                d.secondary = occurrence.childGuid;
                d.detail = occurrence.stableOccurrenceOrdinal;
                out->push_back(d);
            }
        }
    }
    Diagnostic cycle;
    if (CycleFinder(input, index, true,
                    DiagnosticCode::ContainerOccurrenceCycle)
            .Find(&cycle)) {
        out->push_back(std::move(cycle));
    }
}

// ---- Model construction --------------------------------------------------
//
// Reached only when every stage was clean. Translates the authored input into
// the canonical model, mapping GUIDs to their minted ids. Authored order is kept
// inside each object (children, clips, markers) because it is what the author laid
// out; the top-level object list and the root list are sorted by stable id so
// the model has one canonical enumeration order independent of authored order.
Chassis BuildChassis(const InputNode& node) {
    Chassis c;
    static_cast<PlayablePropertyPayloadV1&>(c) =
        static_cast<const PlayablePropertyPayloadV1&>(node);
    if (node.hasSignalGraph) {
        c.presence.Set(PlayablePropertyIdV1::SignalGraph);
    }
    c.id = node.guid;
    c.guid = node.guid;
    c.editorOrder = node.editorOrder;
    // The common chassis scalars and sends are stored verbatim — no
    // interpretation, no reordering. Sends keep their authored order.
    return c;
}

std::vector<StableId> ResolveChildIds(const InputNode& node) {
    std::vector<StableId> ids;
    ids.reserve(node.ownedChildren.size());
    for (const InputOwnershipEdge& edge : node.ownedChildren) {
        ids.push_back(edge.childGuid);
    }
    return ids;
}

ContentObject BuildObject(const InputNode& node, const NodeIndex& index,
                          const ProjectInput& input) {
    ContentObject obj;
    obj.kind = node.kind;
    const Chassis chassis = BuildChassis(node);
    const std::vector<StableId> childIds = ResolveChildIds(node);
    switch (node.kind) {
        case ContentKind::Layer:
            obj.layer.chassis = chassis;
            obj.layer.ownedChildren = childIds;
            break;
        case ContentKind::Sound:
            obj.sound.chassis = chassis;
            break;
        case ContentKind::Container:
            obj.container.chassis = chassis;
            obj.container.ownedChildren = childIds;
            obj.container.children.reserve(
                node.containerChildOccurrences.size());
            for (const InputContainerChildOccurrence& edge :
                 node.containerChildOccurrences) {
                ContainerChildOccurrence occurrence;
                static_cast<PlayablePropertyPayloadV1&>(occurrence) =
                    static_cast<const PlayablePropertyPayloadV1&>(edge);
                occurrence.stableOccurrenceOrdinal =
                    edge.stableOccurrenceOrdinal;
                const InputNode* child = index.Find(input, edge.childGuid);
                occurrence.childPlayableRef.kind = child->kind;
                occurrence.childPlayableRef.target =
                    edge.childGuid;
                obj.container.children.push_back(std::move(occurrence));
            }
            break;
        case ContentKind::Track: {
            obj.track.chassis = chassis;
            obj.track.ownedChildren = childIds;
            for (const InputClip& clip : node.clips) {
                Clip out;
                static_cast<PlayablePropertyPayloadV1&>(out) =
                    static_cast<const PlayablePropertyPayloadV1&>(clip);
                out.id = clip.guid;
                out.guid = clip.guid;
                const InputNode* target = index.Find(input, clip.targetGuid);
                out.contentRef.kind = target->kind;  // resolved and legal by now
                out.contentRef.target = clip.targetGuid;
                out.stableOccurrenceOrdinal = clip.stableOccurrenceOrdinal;
                out.time = clip.time;
                out.duration = clip.duration;
                out.fades = clip.fades;
                out.semanticOrderKey = clip.semanticOrderKey;
                obj.track.clips.push_back(out);
            }
            break;
        }
        case ContentKind::Piece: {
            obj.piece.chassis = chassis;
            obj.piece.tracks = childIds;
            const InputPieceTimeSpec& spec = *node.timeSpec;
            obj.piece.timeSpec.kind = spec.kind;
            obj.piece.timeSpec.tempoMap = spec.tempoMap;
            obj.piece.timeSpec.meterMap = spec.meterMap;
            obj.piece.startPoint.kind = node.startPoint->kind;
            obj.piece.startPoint.markerId = node.startPoint->markerId;
            if (node.entryMarker) {
                obj.piece.entryMarker = EntryMarker{
                    node.entryMarker->guid, node.entryMarker->guid,
                    node.entryMarker->position};
            }
            for (const InputMarker& marker : node.markers) {
                Marker out;
                out.id = marker.guid;
                out.guid = marker.guid;
                out.markerId = marker.markerId;
                out.kind = marker.kind;
                out.position = marker.position;
                obj.piece.markers.push_back(out);
            }
            break;
        }
    }
    return obj;
}

CanonicalProject BuildProject(const ProjectInput& input, const NodeIndex& index) {
    CanonicalProject project;
    project.projectId = input.projectId;
    project.objects.reserve(input.nodes.size());
    for (const InputNode& node : input.nodes) {
        project.objects.push_back(BuildObject(node, index, input));
    }
    std::sort(project.objects.begin(), project.objects.end(),
              [](const ContentObject& a, const ContentObject& b) {
                  return a.chassis().id < b.chassis().id;
              });
    for (const AuthoringGuid& rootGuid : input.roots) {
        project.roots.push_back(rootGuid);
    }
    std::sort(project.roots.begin(), project.roots.end());
    return project;
}

// ---- Stage 4: time and marker normalization --------------------------------
//
// The frozen time rules that were deferred out of parse, plus the derivation of
// the deterministic order and start-coordinate facts. Validation runs on the
// input before anything is built, so a refusal here still yields no model and no
// facts.

// Which map failed the singleton-at-zero rule, carried in the diagnostic detail
// so tempo and meter are told apart.
enum class MusicalMapWhich : std::uint64_t {
    Tempo = 0,
    Meter = 1,
};

// A Musical map must be exactly one row, and that row must sit at tick 0 — the
// frozen initial-scope rule. An empty map was already refused at parse, so this
// catches a second row or a row placed off the origin.
template <typename Row>
bool IsSingletonAtZero(const std::vector<Row>& map) {
    return map.size() == 1 && map.front().tick == 0;
}

void ValidateTimeMarker(const ProjectInput& input, std::vector<Diagnostic>* out) {
    for (const InputNode& node : input.nodes) {
        for (const InputClip& clip : node.clips) {
            if (clip.duration < 0) {
                Diagnostic d = Make(DiagnosticCode::NegativeClipDuration,
                                    clip.guid);
                d.secondary = node.guid;
                out->push_back(d);
            } else if (clip.duration > 0 &&
                       clip.time > (std::numeric_limits<std::int64_t>::max)() -
                                       clip.duration) {
                Diagnostic d = Make(DiagnosticCode::ClipEndOverflow, clip.guid);
                d.secondary = node.guid;
                out->push_back(d);
            }
        }
        if (node.kind != ContentKind::Piece || !node.timeSpec.has_value()) {
            // Non-Pieces carry no time side, and a Piece missing its time spec
            // was already refused; either way there is nothing to normalize.
            continue;
        }
        const InputPieceTimeSpec& spec = *node.timeSpec;
        if (spec.kind == PieceTimeKind::Musical) {
            if (!IsSingletonAtZero(spec.tempoMap)) {
                Diagnostic d =
                    Make(DiagnosticCode::MusicalMapNotSingletonAtZero, node.guid);
                d.detail = static_cast<std::uint64_t>(MusicalMapWhich::Tempo);
                out->push_back(d);
            }
            if (!IsSingletonAtZero(spec.meterMap)) {
                Diagnostic d =
                    Make(DiagnosticCode::MusicalMapNotSingletonAtZero, node.guid);
                d.detail = static_cast<std::uint64_t>(MusicalMapWhich::Meter);
                out->push_back(d);
            }
        }

        // Exit is unique by kind: a Piece may declare at most one exit marker.
        // Custom markers are unbounded.
        std::uint64_t exitMarkers = 0;
        for (const InputMarker& marker : node.markers) {
            if (marker.kind == MarkerKind::Exit) {
                ++exitMarkers;
            }
        }
        if (exitMarkers > 1) {
            Diagnostic d = Make(DiagnosticCode::MultipleExitMarkers, node.guid);
            d.detail = exitMarkers;
            out->push_back(d);
        }
    }
}

// Derives the normalization facts from the already-built, already-valid model.
// The model's objects are in ascending stable-id order, so iterating them yields
// the per-Piece and per-Track facts in that same order without a further sort.
NormalizedFacts BuildNormalizedFacts(const CanonicalProject& project) {
    NormalizedFacts facts;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Piece) {
            const PieceDef& piece = obj.piece;
            PieceNormalization pn;
            pn.piece = piece.chassis.id;
            pn.bounds = *DerivePieceTimelineBounds(project, piece);
            pn.startKind = piece.startPoint.kind;
            if (piece.startPoint.kind == StartPointKind::Marker) {
                pn.startMarkerId = piece.startPoint.markerId;
                // The named marker exists (parse proved it), so its authored
                // position is the resolved start coordinate.
                for (const Marker& marker : piece.markers) {
                    if (marker.markerId == piece.startPoint.markerId) {
                        pn.startCoordinate = marker.position;
                        break;
                    }
                }
            } else {
                pn.startMarkerId = kReservedMarkerId;
                pn.startCoordinate = pn.bounds.nominalStart;
            }

            std::vector<const Marker*> byMarkerId;
            byMarkerId.reserve(piece.markers.size());
            for (const Marker& marker : piece.markers) {
                byMarkerId.push_back(&marker);
            }
            // MarkerIds are unique within a Piece (parse proved it), so this is a
            // total order and the sort is fully determined.
            std::stable_sort(byMarkerId.begin(), byMarkerId.end(),
                             [](const Marker* a, const Marker* b) {
                                 return a->markerId < b->markerId;
                             });
            for (const Marker* marker : byMarkerId) {
                pn.markersByMarkerId.push_back(marker->id);
            }
            facts.pieces.push_back(std::move(pn));
        } else if (obj.kind == ContentKind::Track) {
            const TrackDef& track = obj.track;
            TrackNormalization tn;
            tn.track = track.chassis.id;
            std::vector<const Clip*> byOrder;
            byOrder.reserve(track.clips.size());
            for (const Clip& clip : track.clips) {
                byOrder.push_back(&clip);
            }
            std::stable_sort(byOrder.begin(), byOrder.end(),
                             [](const Clip* a, const Clip* b) {
                                 if (a->time != b->time) {
                                     return a->time < b->time;
                                 }
                                 if (a->semanticOrderKey != b->semanticOrderKey) {
                                     return a->semanticOrderKey <
                                            b->semanticOrderKey;
                                 }
                                 return a->id < b->id;
                             });
            for (const Clip* clip : byOrder) {
                tn.clipsByOrder.push_back(clip->id);
            }
            facts.tracks.push_back(std::move(tn));
        }
    }
    return facts;
}

FrontEndResult CompileFrontEndImpl(
    const ProjectInput& input,
    const std::optional<std::uint32_t>& resolutionDepthCeiling) {
    FrontEndResult result;

    // Stage 1: parse. Runs fully; a failure here stops the pipeline, because
    // later stages assume well-shaped nodes.
    for (const InputNode& node : input.nodes) {
        ParseNode(node, &result.diagnostics);
    }
    if (!result.diagnostics.empty()) {
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    ValidateCanonicalIdentities(input, &result.diagnostics);
    if (!result.diagnostics.empty()) {
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    // Stage 3: graph. Structure first; placement only if the structure held.
    const NodeIndex index = BuildNodeIndex(input);
    const bool structureFailed =
        ValidateStructure(input, index, &result.diagnostics);
    if (!structureFailed) {
        ValidatePlacement(input, index, &result.diagnostics);
    }
    if (!result.diagnostics.empty()) {
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    // Stage 4: time and marker normalization. The frozen Musical-map and single-
    // exit rules are enforced before anything is built, so a refusal here leaves
    // no model and no facts.
    ValidateTimeMarker(input, &result.diagnostics);
    if (!result.diagnostics.empty()) {
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    // Every validation stage clean: build the canonical model, derive its
    // normalization facts, then resolve references before publishing any
    // product. This is the only path that produces output, which is what "no
    // partial output" means in code.
    CanonicalProject project = BuildProject(input, index);
    for (const auto& object : project.objects) {
        if (object.kind == ContentKind::Piece &&
            !DerivePieceTimelineBounds(project, object.piece)) {
            result.diagnostics.push_back(Make(
                DiagnosticCode::InvalidPieceTimelineBounds,
                object.chassis().guid));
        }
    }
    if (!result.diagnostics.empty()) return result;
    NormalizedFacts facts = BuildNormalizedFacts(project);
    ReferenceResolutionResult resolution =
        resolutionDepthCeiling.has_value()
            ? BuildReferenceResolution(project, facts,
                                       *resolutionDepthCeiling)
            : BuildReferenceResolution(project, facts);
    if (!resolution.ok()) {
        result.diagnostics = std::move(resolution.diagnostics);
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    SendResolutionResult sends = BuildSendResolution(project, facts);
    if (!sends.ok()) {
        result.diagnostics = std::move(sends.diagnostics);
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    EffectChainSelectionResult effectChains =
        BuildEffectChainSelection(project, facts);
    if (!effectChains.ok()) {
        result.diagnostics = std::move(effectChains.diagnostics);
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    // Execute the fixed-slot representability gate for every selected chain.
    // The physical mapping is backend-local and deliberately does not enter a
    // compile product or fingerprint.
    for (const CompiledEffectChain& chain : effectChains.facts->chains) {
        AuthoringGuid ownerGuid{};
        for (const ContentObject& object : project.objects) {
            if (object.chassis().id == chain.owner) {
                ownerGuid = object.chassis().guid;
                break;
            }
        }
        LowerEffectChainResult lowered =
            LowerEffectChainToFixedSlots(chain, ownerGuid);
        result.diagnostics.insert(result.diagnostics.end(),
                                  lowered.diagnostics.begin(),
                                  lowered.diagnostics.end());
    }
    if (!result.diagnostics.empty()) {
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         DiagnosticLess);
        return result;
    }

    result.resolution = std::move(resolution.facts);
    result.sends = std::move(sends.facts);
    result.effectChains = std::move(effectChains.facts);
    result.facts = std::move(facts);
    result.project = std::move(project);
    return result;
}

}  // namespace

std::optional<PieceTimelineBounds> DerivePieceTimelineBounds(
    const CanonicalProject& project, const PieceDef& piece) noexcept {
    PieceTimelineBounds bounds{};
    bool hasContent = false;
    for (const auto trackId : piece.tracks) {
        const auto track = std::lower_bound(
            project.objects.begin(), project.objects.end(), trackId,
            [](const ContentObject& object, StableId id) {
                return object.chassis().id < id;
            });
        if (track == project.objects.end() || track->chassis().id != trackId ||
            track->kind != ContentKind::Track) return std::nullopt;
        for (const auto& clip : track->track.clips) {
            if (clip.duration < 0 ||
                clip.time > (std::numeric_limits<std::int64_t>::max)() -
                                clip.duration) return std::nullopt;
            const auto end = clip.time + clip.duration;
            if (!hasContent) {
                bounds.contentStart = clip.time;
                bounds.contentEnd = end;
                hasContent = true;
            } else {
                bounds.contentStart = std::min(bounds.contentStart, clip.time);
                bounds.contentEnd = std::max(bounds.contentEnd, end);
            }
        }
    }
    bounds.nominalStart = piece.entryMarker
                              ? piece.entryMarker->position
                              : bounds.contentStart;
    bounds.nominalEnd = bounds.contentEnd;
    bool hasExit = false;
    for (const auto& marker : piece.markers) {
        if (marker.kind != MarkerKind::Exit) continue;
        if (hasExit) return std::nullopt;
        bounds.nominalEnd = marker.position;
        hasExit = true;
    }
    const auto validSpan = [](std::int64_t low, std::int64_t high) {
        return high >= low &&
            !(low < 0 && high > (std::numeric_limits<std::int64_t>::max)() + low);
    };
    if (!validSpan(bounds.contentStart, bounds.contentEnd) ||
        !validSpan(bounds.nominalStart, bounds.nominalEnd) ||
        (bounds.nominalStart > bounds.contentStart &&
         !validSpan(bounds.contentStart, bounds.nominalStart)) ||
        (bounds.contentEnd > bounds.nominalEnd &&
         !validSpan(bounds.nominalEnd, bounds.contentEnd))) return std::nullopt;
    return bounds;
}

FrontEndResult CompileFrontEnd(const ProjectInput& input) {
    return CompileFrontEndImpl(input, std::nullopt);
}

FrontEndResult CompileFrontEnd(const ProjectInput& input,
                               std::uint32_t resolutionDepthCeiling) {
    return CompileFrontEndImpl(input, resolutionDepthCeiling);
}

}  // namespace harkenprojectschema
