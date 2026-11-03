#ifndef HARKEN_RENDER_SIGNAL_FEEDBACK_PLAN_H
#define HARKEN_RENDER_SIGNAL_FEEDBACK_PLAN_H

#include "harken/render/DuckingReceiver.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <limits>
#include <cstdint>
#include <span>
#include <vector>

namespace harken {

// These are collapsed, absolute path bounds between receiving stages/ports.
// The owner includes ordinary sends, layout and effects, and publishes bounds
// covering the entire gain transition together with the actual trajectory.
struct SignalFeedbackEdge {
    std::uint32_t source = 0;
    std::uint32_t target = 0;
    double bound = 0;
    bool delayed = false;
};

struct SignalFeedbackNode {
    std::uint32_t component = 0;
    bool feedbackInput = false;
    double slope = kDuckingBaselineSlope;
};

// A borrowed compilation result. The workspace owner fences it through the
// complete quantum; no allocation, recursion, or container growth occurs here.
struct SignalFeedbackView {
    std::span<SignalFeedbackNode> nodes;
    std::span<std::uint32_t> order;
    std::span<std::size_t> offsets;
    std::span<std::uint32_t> outgoing;
    std::span<double> sensitivity, maximum;
    std::span<const SignalFeedbackEdge> edges;

    bool UpdateBounds(std::span<const double> bounds) noexcept {
        if (bounds.size() != edges.size()) return false;
        for (double bound : bounds)
            if (std::isnan(bound) || bound < 0) return false;
        std::fill(sensitivity.begin(), sensitivity.end(), 0.0);
        std::fill(maximum.begin(), maximum.end(), 0.0);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto& edge = edges[i];
            if (edge.delayed && nodes[edge.source].component == nodes[edge.target].component)
                sensitivity[edge.target] += bounds[i];
        }
        for (const auto node : order) {
            const auto component = nodes[node].component;
            maximum[component] = std::max(maximum[component], sensitivity[node]);
            for (auto p = offsets[node]; p < offsets[node + 1]; ++p) {
                const auto index = outgoing[p];
                const auto& edge = edges[index];
                if (!edge.delayed && nodes[edge.target].component == component &&
                    bounds[index] != 0 && sensitivity[node] != 0)
                    sensitivity[edge.target] += bounds[index] * sensitivity[node];
            }
        }
        for (auto& node : nodes)
            node.slope = node.feedbackInput
                ? 1.0 / (1.0 / kDuckingBaselineSlope + maximum[node.component])
                : kDuckingBaselineSlope;
        return true;
    }
};

// The same sparse compiler serves control-side prepared graphs and coordinator
// graphs whose workspace has already been admitted. Zero-weight edges retain
// structural identity; only the ordinary subgraph must be acyclic.
class SignalFeedbackPlan {
public:
    static std::size_t ScratchBytes(std::uint32_t count, std::size_t edgeCount) noexcept {
        if (edgeCount > UINT32_MAX) return 0;
        // Four offsets/cursors; six node-index arrays; two edge-index arrays;
        // visited, node results, and three numeric arrays. Include per-array
        // alignment so a float-aligned slab is a valid backing allocation.
        const std::uint64_t bytes = (static_cast<std::uint64_t>(count) + 1) *
            sizeof(std::size_t) * 4 + static_cast<std::uint64_t>(count) *
            (sizeof(std::uint32_t) * 4 + sizeof(std::uint8_t) +
             sizeof(SignalFeedbackNode) + sizeof(double) * 2) +
            static_cast<std::uint64_t>(edgeCount) * (sizeof(std::uint32_t) * 2 + sizeof(double)) +
            16 * alignof(std::max_align_t);
        return bytes <= std::numeric_limits<std::size_t>::max() ? static_cast<std::size_t>(bytes) : 0;
    }

    static bool Compile(std::uint32_t count, std::span<const SignalFeedbackEdge> edges,
                        std::span<std::byte> storage, SignalFeedbackView& out) noexcept {
        const auto required = ScratchBytes(count, edges.size());
        if (required == 0 || storage.size() < required) return false;
        for (const auto& edge : edges)
            if (edge.source >= count || edge.target >= count ||
                std::isnan(edge.bound) || edge.bound < 0) return false;
        Scratch cursor(storage);
        auto offsets = cursor.Take<std::size_t>(static_cast<std::size_t>(count) + 1);
        auto reverseOffsets = cursor.Take<std::size_t>(static_cast<std::size_t>(count) + 1);
        auto forwardCursor = cursor.Take<std::size_t>(static_cast<std::size_t>(count) + 1);
        auto reverseCursor = cursor.Take<std::size_t>(static_cast<std::size_t>(count) + 1);
        auto indegree = cursor.Take<std::uint32_t>(count);
        auto order = cursor.Take<std::uint32_t>(count);
        auto stack = cursor.Take<std::uint32_t>(count);
        auto finished = cursor.Take<std::uint32_t>(count);
        auto outgoing = cursor.Take<std::uint32_t>(edges.size());
        auto incoming = cursor.Take<std::uint32_t>(edges.size());
        auto visited = cursor.Take<std::uint8_t>(count);
        auto nodes = cursor.Take<SignalFeedbackNode>(count);
        auto sensitivity = cursor.Take<double>(count);
        auto maximum = cursor.Take<double>(count);
        auto bounds = cursor.Take<double>(edges.size());
        if (!cursor.Valid()) return false;
        for (const auto& edge : edges) {
            ++offsets[edge.source + 1];
            ++reverseOffsets[edge.target + 1];
            if (!edge.delayed) ++indegree[edge.target];
        }
        for (std::size_t i = 1; i < offsets.size(); ++i) {
            offsets[i] += offsets[i - 1];
            reverseOffsets[i] += reverseOffsets[i - 1];
        }
        std::copy(offsets.begin(), offsets.end(), forwardCursor.begin());
        std::copy(reverseOffsets.begin(), reverseOffsets.end(), reverseCursor.begin());
        for (std::size_t i = 0; i < edges.size(); ++i) {
            outgoing[forwardCursor[edges[i].source]++] = static_cast<std::uint32_t>(i);
            incoming[reverseCursor[edges[i].target]++] = static_cast<std::uint32_t>(i);
            bounds[i] = edges[i].bound;
        }
        std::size_t ordered = 0;
        for (std::uint32_t i = 0; i < count; ++i)
            if (indegree[i] == 0) order[ordered++] = i;
        for (std::size_t i = 0; i < ordered; ++i) {
            const auto node = order[i];
            for (auto p = offsets[node]; p < offsets[node + 1]; ++p) {
                const auto& edge = edges[outgoing[p]];
                if (!edge.delayed && --indegree[edge.target] == 0) order[ordered++] = edge.target;
            }
        }
        if (ordered != count) return false;
        std::copy(offsets.begin(), offsets.end(), forwardCursor.begin());
        std::size_t depth = 0, finishedCount = 0;
        for (std::uint32_t root = 0; root < count; ++root) {
            if (visited[root]) continue;
            stack[depth++] = root;
            visited[root] = 1;
            while (depth != 0) {
                const auto node = stack[depth - 1];
                if (forwardCursor[node] == offsets[node + 1]) {
                    finished[finishedCount++] = node;
                    --depth;
                    continue;
                }
                const auto target = edges[outgoing[forwardCursor[node]++]].target;
                if (!visited[target]) { visited[target] = 1; stack[depth++] = target; }
            }
        }
        std::fill(visited.begin(), visited.end(), 0);
        std::uint32_t components = 0;
        for (auto position = finishedCount; position != 0;) {
            const auto root = finished[--position];
            if (visited[root]) continue;
            stack[depth++] = root;
            visited[root] = 1;
            while (depth != 0) {
                const auto node = stack[--depth];
                nodes[node].component = components;
                for (auto p = reverseOffsets[node]; p < reverseOffsets[node + 1]; ++p) {
                    const auto source = edges[incoming[p]].source;
                    if (!visited[source]) { visited[source] = 1; stack[depth++] = source; }
                }
            }
            ++components;
        }
        for (const auto& edge : edges)
            if (edge.delayed && nodes[edge.source].component == nodes[edge.target].component)
                nodes[edge.target].feedbackInput = true;
        SignalFeedbackView result{nodes, order, offsets, outgoing, sensitivity,
            maximum.first(components), edges};
        if (!result.UpdateBounds(bounds)) return false;
        out = result;
        return true;
    }

    bool Prepare(std::uint32_t count, std::span<const SignalFeedbackEdge> edges) noexcept {
        for (const auto& edge : edges) if (!std::isfinite(edge.bound)) return false;
        try {
            const auto size = ScratchBytes(count, edges.size());
            if (size == 0) return false;
            std::vector<std::byte> scratch(size);
            SignalFeedbackView compiled;
            if (!Compile(count, edges, scratch, compiled)) return false;
            for (const auto bound : compiled.maximum) if (!std::isfinite(bound)) return false;
            SignalFeedbackPlan next;
            next.m_edges.assign(edges.begin(), edges.end());
            next.m_nodes.assign(compiled.nodes.begin(), compiled.nodes.end());
            next.m_normalOrder.assign(compiled.order.begin(), compiled.order.end());
            next.m_offsets.assign(compiled.offsets.begin(), compiled.offsets.end());
            next.m_outgoing.assign(compiled.outgoing.begin(), compiled.outgoing.end());
            next.m_sensitivity.assign(compiled.sensitivity.begin(), compiled.sensitivity.end());
            next.m_maximum.assign(compiled.maximum.begin(), compiled.maximum.end());
            *this = std::move(next);
            return true;
        } catch (...) { return false; }
    }
    bool UpdateBounds(std::span<const double> bounds) noexcept {
        return SignalFeedbackView{m_nodes, m_normalOrder, m_offsets, m_outgoing,
            m_sensitivity, m_maximum, m_edges}.UpdateBounds(bounds);
    }
    std::span<const SignalFeedbackNode> Nodes() const noexcept { return m_nodes; }
    std::span<const std::uint32_t> NormalOrder() const noexcept { return m_normalOrder; }
private:
    class Scratch {
    public:
        explicit Scratch(std::span<std::byte> bytes) : data_(bytes.data()), size_(bytes.size()) {}
        template<class T> std::span<T> Take(std::size_t count) noexcept {
            if (!valid_ || count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
                valid_ = false; return {};
            }
            const auto bytes = count * sizeof(T);
            if (std::align(alignof(T), bytes, data_, size_) == nullptr) { valid_ = false; return {}; }
            auto* result = static_cast<T*>(data_);
            for (std::size_t i = 0; i < count; ++i) std::construct_at(result + i);
            data_ = static_cast<std::byte*>(data_) + bytes;
            size_ -= bytes;
            return {result, count};
        }
        bool Valid() const noexcept { return valid_; }
    private:
        void* data_;
        std::size_t size_;
        bool valid_ = true;
    };
    std::vector<SignalFeedbackEdge> m_edges;
    std::vector<std::size_t> m_offsets;
    std::vector<std::uint32_t> m_outgoing;
    std::vector<double> m_sensitivity, m_maximum;
    std::vector<SignalFeedbackNode> m_nodes;
    std::vector<std::uint32_t> m_normalOrder;
};

} // namespace harken
#endif
