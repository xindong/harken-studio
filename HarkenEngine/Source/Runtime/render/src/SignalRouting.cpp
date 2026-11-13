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

void SignalRenderPlan::PrepareFinalOutputs(std::span<const SignalMemberImage> members) noexcept {
    for (auto& node : nodes_) node.deferOutput = node.outputGainOffset != SIZE_MAX;
    for (const auto& member : members) {
        const bool final = !member.nodes.empty() && member.voice != nullptr &&
            member.voice->voiceSendActiveCount == 0;
        for (const auto index : member.nodes)
            if (index < nodes_.size()) nodes_[index].deferOutput &= final && index == member.nodes.front();
    }
}

void SignalRenderPlan::SelectPreparedBusRouting(BusTree& buses) noexcept {
    if (preparedSoundRouting_ || !independentBuses_ || buses.AudioBusCount() > routingBusCount_) return;
    for (std::uint32_t i = 0; i < routingBusCount_; ++i)
        busReachability_[i] = (nodes_[i].source != UINT32_MAX ? 1u : 0u) |
                              (nodes_[i].target != UINT32_MAX ? 2u : 0u);
    preparedSoundRouting_ = buses.SignalReceiversCannotReachSources(busReachability_);
}

std::uint64_t SignalRenderPlan::RoutingScratchBytes(std::uint32_t nodes, std::uint32_t buses,
                                                  std::uint64_t edges, std::uint32_t frames) noexcept {
    if (edges > UINT32_MAX) return UINT64_MAX;
    const auto compiled = SignalFeedbackPlan::ScratchBytes(nodes, static_cast<std::size_t>(edges));
    if (compiled == 0) return UINT64_MAX;
    const auto fixed = static_cast<std::uint64_t>(compiled) + edges * sizeof(SignalFeedbackEdge) + 64;
    const auto perBus = sizeof(SignalBusRoutingInfo) +
        4ull * (frames / 16u + 2u) * sizeof(SignalBiquadSegment);
    if (buses > (UINT64_MAX - fixed) / perBus) return UINT64_MAX;
    return fixed + static_cast<std::uint64_t>(buses) * perBus;
}

bool SignalRenderPlan::ReserveRouting(BusTree& buses, BlockArena& arena,
                                     std::uint32_t members, std::size_t pathRows, std::uint64_t sends) noexcept {
    if (buses.AudioBusCount() > routingBusCount_) return false;
    const auto graphCount = static_cast<std::uint64_t>(NodeCount()) + buses.SignalRoutingExtraNodes();
    if (graphCount > UINT32_MAX) return false;
    const auto edges = static_cast<std::uint64_t>(buses.SignalRoutingEdgeCapacity()) +
        relations_.size() + pathRows + static_cast<std::uint64_t>(members) + sends;
    const auto bytes = RoutingScratchBytes(static_cast<std::uint32_t>(graphCount), buses.AudioBusCount(), edges, current_.frames);
    if (bytes > arena.CapacityBytes()) return false;
    auto* data = arena.ReserveFloats((static_cast<std::size_t>(bytes) + sizeof(float) - 1) / sizeof(float));
    if (data == nullptr) return false;
    routingStorage_ = {reinterpret_cast<std::byte*>(data), static_cast<std::size_t>(bytes)};
    return true;
}

bool SignalRenderPlan::BuildRouting(std::span<const SignalMemberImage> members,
                                    BusTree& buses) noexcept {
    if (!open_ || contentProcessed_) return false;
    const auto busCount = buses.AudioBusCount();
    if (busCount > routingBusCount_) return false;
    const auto graphCount = static_cast<std::uint64_t>(NodeCount()) + buses.SignalRoutingExtraNodes();
    if (graphCount > UINT32_MAX) return false;
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
    auto schedules = Reserve<SignalBiquadSegment>(storage,
        static_cast<std::size_t>(busCount) * 4 * (current_.frames / 16u + 2u));
    if (info.size() != busCount || edges.size() != capacity) return false;
    auto used = buses.SignalRoutingSnapshot(info, edges, current_.frames, schedules, NodeCount());
    if (used == UINT32_MAX) return false;
    const auto append = [&](std::uint32_t from, std::uint32_t to, double bound, bool delayed) {
        if (from >= graphCount || to >= graphCount || used == edges.size() ||
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
        const auto memberBegin = used;
        const auto route = [&](std::uint32_t destination, double bound) {
            if (destination >= busCount || info[destination].id == VnBusId{}) return false;
            return append(source, info[destination].inputNode, SignalBoundProduct(bound,
                info[destination].inputNode == destination ? info[destination].inputBound : 1), false);
        };
        if (!route(voice.busIndex, SignalOutputGainBound(voice.outputGain, member.frames) > 0 ? 1 : 0)) return false;
        for (const auto& send : voice.voiceSends) {
            if (!send.active || send.targetBusIndex >= busCount ||
                send.targetBusGeneration != info[send.targetBusIndex].generation) continue;
            if (!route(send.targetBusIndex, SendBound(send))) return false;
        }
        // Sum duplicate routes within this Voice before the existing maximum
        // across members sharing a sound node. No degree-squared rescans.
        auto memberEdges = edges.subspan(memberBegin, used - memberBegin);
        std::sort(memberEdges.begin(), memberEdges.end(), [](const auto& a, const auto& b) {
            return a.target < b.target;
        });
        auto merged = memberBegin;
        for (const auto& edge : memberEdges) {
            if (merged != memberBegin && edges[merged - 1].target == edge.target)
                edges[merged - 1].bound += edge.bound;
            else edges[merged++] = edge;
        }
        used = merged;
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
            if (edge.delayed || edge.source >= nodes_.size() || nodes_[edge.source].kind == SignalEndpointKindV1::Bus)
                prior.bound += edge.bound;
            else prior.bound = std::max(prior.bound, edge.bound);
        } else edges[merged++] = edge;
    }
    const auto bytes = SignalFeedbackPlan::ScratchBytes(static_cast<std::uint32_t>(graphCount), merged);
    if (storage.size() < bytes) return false;
    auto scratch = storage.first(bytes);
    SignalFeedbackView candidate;
    return SignalFeedbackPlan::Compile(static_cast<std::uint32_t>(graphCount), edges.first(merged), scratch, candidate) &&
        SetBlockRouting(candidate);
}
}
