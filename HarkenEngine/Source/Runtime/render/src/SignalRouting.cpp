#include "harken/render/SignalRenderPlan.h"
#include "harken/render/SignalRoutingTypes.h"
#include "harken/render/SignalPathBounds.h"
#include "harken/render/BusTree.h"
#include "harken/render/Voice.h"
#include "harken/core/BlockArena.h"
#include <tuple>

namespace harken {
namespace {
template<class T> std::span<T> Reserve(std::span<std::byte>& storage, std::size_t count) noexcept {
    if (count > (SIZE_MAX - alignof(T)) / sizeof(T)) return {};
    const auto bytes = count * sizeof(T);
    auto size = storage.size();
    void* raw = storage.data();
    if (raw == nullptr || std::align(alignof(T), bytes, raw, size) == nullptr) return {};
    auto* pointer = static_cast<T*>(raw);
    for (std::size_t i = 0; i < count; ++i) std::construct_at(pointer + i);
    storage = {static_cast<std::byte*>(raw) + bytes, size - bytes};
    return {pointer, count};
}
double SendBound(const VoiceSendSlot& send) noexcept {
    auto gain = send.gain;
    if (send.ramping) gain.SetBlockTarget(send.rampStartGain, send.rampEndGain,
        send.rampBlocksDone, send.rampBlocksTotal);
    // GainRamp is block-linear, with finite prepared endpoints.
    return std::max(std::abs(static_cast<double>(gain.current)), std::abs(static_cast<double>(gain.target)));
}
}

std::uint64_t SignalRenderPlan::RoutingScratchBytes(std::uint32_t nodes, std::uint32_t buses,
                                                  std::uint64_t edges) noexcept {
    if (edges > UINT32_MAX) return UINT64_MAX;
    const auto compiled = SignalFeedbackPlan::ScratchBytes(nodes, static_cast<std::size_t>(edges));
    if (compiled == 0) return UINT64_MAX;
    return compiled + edges * sizeof(SignalFeedbackEdge) +
        static_cast<std::uint64_t>(buses) * sizeof(SignalBusRoutingInfo) + 64;
}

bool SignalRenderPlan::ReserveRouting(BusTree& buses, BlockArena& arena,
                                     std::uint32_t members, std::size_t pathRows) noexcept {
    const auto edges = static_cast<std::uint64_t>(buses.SignalRoutingEdgeCapacity()) +
        relations_.size() + pathRows + static_cast<std::uint64_t>(members) * (kMaxVoiceSendSlots + 1ull);
    const auto bytes = RoutingScratchBytes(NodeCount(), buses.AudioBusCount(), edges);
    if (bytes > arena.CapacityBytes()) return false;
    auto* data = arena.AllocFloats((static_cast<std::size_t>(bytes) + sizeof(float) - 1) / sizeof(float));
    if (data == nullptr) return false;
    routingStorage_ = {reinterpret_cast<std::byte*>(data), static_cast<std::size_t>(bytes)};
    return true;
}

bool SignalRenderPlan::BuildRouting(std::span<const SignalMemberImage> members,
                                    BusTree& buses) noexcept {
    if (!open_ || contentProcessed_) return false;
    const auto busCount = buses.AudioBusCount();
    if (busCount > nodes_.size()) return false;
    std::size_t capacity = buses.SignalRoutingEdgeCapacity() + relations_.size();
    for (const auto& member : members) {
        if (member.voice == nullptr || member.nodes.empty()) return false;
        const auto extra = member.nodes.size() + member.voice->voiceSendActiveCount + 1;
        if (capacity > UINT32_MAX - extra) return false;
        capacity += extra;
    }
    if (capacity > UINT32_MAX) return false;
    auto storage = routingStorage_;
    auto info = Reserve<SignalBusRoutingInfo>(storage, busCount);
    auto edges = Reserve<SignalFeedbackEdge>(storage, capacity);
    if (info.size() != busCount || edges.size() != capacity) return false;
    auto used = buses.SignalRoutingSnapshot(info, edges, current_.frames);
    if (used == UINT32_MAX) return false;
    const auto append = [&](std::uint32_t from, std::uint32_t to, double bound, bool delayed) {
        if (from >= nodes_.size() || to >= nodes_.size() || used == edges.size() ||
            std::isnan(bound) || bound < 0) return false;
        edges[used++] = {from, to, bound, delayed};
        return true;
    };
    for (const auto& member : members) {
        // Prepared paths retain root-to-leaf order, including selected actual
        // ancestors. No per-voice gain state is invented for a shared stage.
        for (std::size_t i = 1; i < member.nodes.size(); ++i)
            if (!append(member.nodes[i], member.nodes[i - 1], 1, false)) return false;
        const auto source = member.nodes.front();
        const auto& voice = *member.voice;
        const auto route = [&](std::uint32_t destination) {
            if (destination >= busCount || info[destination].id == VnBusId{}) return false;
            double bound = destination == voice.busIndex &&
                SignalOutputGainBound(voice.outputGain, member.frames) > 0 ? 1 : 0;
            for (const auto& send : voice.voiceSends)
                if (send.active && send.targetBusIndex == destination &&
                    send.targetBusGeneration == info[destination].generation)
                    bound += SendBound(send);
            return append(source, destination, SignalBoundProduct(bound, info[destination].inputBound), false);
        };
        if (!route(voice.busIndex)) return false;
        for (std::size_t i = 0; i < std::size(voice.voiceSends); ++i) {
            const auto& send = voice.voiceSends[i];
            if (!send.active || send.targetBusIndex == voice.busIndex) continue;
            bool earlier = false;
            for (std::size_t j = 0; j < i; ++j)
                earlier |= voice.voiceSends[j].active && voice.voiceSends[j].targetBusIndex == send.targetBusIndex;
            if (!earlier && !route(send.targetBusIndex)) return false;
        }
    }
    for (std::size_t i = 0; i < relations_.size(); ++i) {
        const auto& relation = relations_[i];
        double bound = levelBounds_[i];
        for (auto node : {relation.source, relation.target}) {
            if (nodes_[node].kind != SignalEndpointKindV1::Bus) continue;
            if (node >= busCount || NodeForBus(node, info[node].id, info[node].generation) != node)
                bound = 0;
        }
        if (nodes_[relation.source].kind == SignalEndpointKindV1::Bus && relation.source < busCount)
            bound = SignalBoundProduct(bound, info[relation.source].outputFilterBound);
        if (!append(relation.source, relation.target, bound, true)) return false;
    }
    auto active = edges.first(used);
    std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) {
        return std::tuple(a.source, a.target, a.delayed) < std::tuple(b.source, b.target, b.delayed);
    });
    std::size_t merged = 0;
    for (const auto& edge : active) {
        if (merged != 0 && edges[merged - 1].source == edge.source &&
            edges[merged - 1].target == edge.target && edges[merged - 1].delayed == edge.delayed) {
            auto& prior = edges[merged - 1];
            if (edge.delayed || nodes_[edge.source].kind == SignalEndpointKindV1::Bus)
                prior.bound += edge.bound;
            else prior.bound = std::max(prior.bound, edge.bound);
        } else edges[merged++] = edge;
    }
    const auto bytes = SignalFeedbackPlan::ScratchBytes(NodeCount(), merged);
    auto scratch = Reserve<std::byte>(storage, bytes);
    if (scratch.size() != bytes) return false;
    SignalFeedbackView candidate;
    return SignalFeedbackPlan::Compile(NodeCount(), edges.first(merged), scratch, candidate) &&
        SetBlockRouting(candidate);
}
}
