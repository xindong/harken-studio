#include "harken/render/SignalRenderPlan.h"

namespace harken {

SignalRenderNodeInput SignalRenderPlan::NodeDefinition(std::uint32_t index) const noexcept {
    assert(index < nodes_.size());
    const auto& node = nodes_[index];
    SignalRenderNodeInput out;
    out.kind = node.kind;
    out.receiver = node.target != UINT32_MAX;
    out.attackMs = node.attackMs;
    out.releaseMs = node.releaseMs;
    const auto ordinal = busOrdinalByNode_[index];
    out.busIndex = ordinal == UINT32_MAX ? UINT32_MAX : busNodes_[ordinal].first;
    out.busId = node.busId;
    out.busGeneration = node.busGeneration;
    out.stableId = node.stableId;
    return out;
}

std::unique_ptr<SignalRenderPlan> SignalRenderPlan::PrepareProjection(
    std::span<const std::uint32_t> selected, std::uint32_t topologySlots) const noexcept {
    if (selected.size() > UINT32_MAX) return {};
    try {
        std::vector<std::uint32_t> remap(nodes_.size(), UINT32_MAX);
        std::vector<SignalRenderNodeInput> nodes;
        nodes.reserve(selected.size());
        for (auto index : selected) {
            if (index >= nodes_.size() || remap[index] != UINT32_MAX) return {};
            const auto input = NodeDefinition(index);
            if (input.kind == SignalEndpointKindV1::Bus && input.busIndex >= topologySlots) return {};
            remap[index] = static_cast<std::uint32_t>(nodes.size());
            nodes.push_back(input);
        }
        std::vector<SignalConnectionInput> relations;
        for (auto relation : relations_) {
            const auto source = remap[relation.source], target = remap[relation.target];
            // A connected zero source has different receiver recovery semantics
            // from a removed input. The activity closure must retain both ends.
            if ((source == UINT32_MAX) != (target == UINT32_MAX)) return {};
            if (source == UINT32_MAX) continue;
            relation.source = source;
            relation.target = target;
            relations.push_back(relation);
        }
        std::vector<SignalFeedbackEdge> paths;
        for (auto path : feedback_.Edges()) {
            const auto source = remap[path.source], target = remap[path.target];
            if (source != UINT32_MAX && target == UINT32_MAX) return {};
            if (source == UINT32_MAX || target == UINT32_MAX) continue;
            path.source = source;
            path.target = target;
            paths.push_back(path);
        }
        std::vector<SignalMemberProof> membership;
        for (auto proof : membership_) {
            if (remap[proof.node] == UINT32_MAX) continue;
            proof.node = remap[proof.node];
            membership.push_back(proof);
        }
        auto result = Prepare(format_, nodes, relations, paths, membership);
        if (result && actualRouting_) result->EnableActualRouting(topologySlots);
        // Continuity is configured against the actual activity predecessor,
        // separately from this immutable full-definition template.
        return result;
    } catch (...) { return {}; }
}

} // namespace harken
