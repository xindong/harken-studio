#ifndef HARKEN_RENDER_SIGNAL_FEEDBACK_PLAN_H
#define HARKEN_RENDER_SIGNAL_FEEDBACK_PLAN_H

#include "harken/render/DuckingReceiver.h"

#include <cmath>
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

// Control-side compilation only. Zero-weight edges retain structural identity;
// same-block cycles fail, but a cycle crossing a delayed edge is supported.
// Sparse storage and iterative traversals avoid quadratic memory and recursion
// proportional to the authored graph depth. Failure preserves the old plan.
class SignalFeedbackPlan {
public:
    bool Prepare(std::uint32_t count,
                 std::span<const SignalFeedbackEdge> edges) noexcept {
        if (edges.size() > UINT32_MAX) return false;
        for (const auto& e : edges)
            if (e.source >= count || e.target >= count ||
                !std::isfinite(e.bound) || e.bound < 0) return false;
        try {
            std::vector<std::size_t> offsets(static_cast<std::size_t>(count) + 1, 0);
            auto reverseOffsets = offsets;
            std::vector<std::uint32_t> indegree(count, 0);
            for (const auto& e : edges) {
                ++offsets[e.source + 1];
                ++reverseOffsets[e.target + 1];
                if (!e.delayed) ++indegree[e.target];
            }
            for (std::size_t i = 1; i < offsets.size(); ++i) {
                offsets[i] += offsets[i - 1];
                reverseOffsets[i] += reverseOffsets[i - 1];
            }
            std::vector<std::uint32_t> outgoing(edges.size()), incoming(edges.size());
            auto cursor = offsets;
            auto reverseCursor = reverseOffsets;
            for (std::size_t i = 0; i < edges.size(); ++i) {
                outgoing[cursor[edges[i].source]++] = static_cast<std::uint32_t>(i);
                incoming[reverseCursor[edges[i].target]++] = static_cast<std::uint32_t>(i);
            }

            std::vector<std::uint32_t> normalOrder;
            normalOrder.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                if (indegree[i] == 0) normalOrder.push_back(i);
            for (std::size_t i = 0; i < normalOrder.size(); ++i) {
                const auto node = normalOrder[i];
                for (auto p = offsets[node]; p < offsets[node + 1]; ++p) {
                    const auto& edge = edges[outgoing[p]];
                    if (!edge.delayed && --indegree[edge.target] == 0)
                        normalOrder.push_back(edge.target);
                }
            }
            if (normalOrder.size() != count) return false;

            std::vector<std::uint8_t> visited(count, 0);
            std::vector<std::uint32_t> stack, finished;
            stack.reserve(count);
            finished.reserve(count);
            cursor = offsets;
            for (std::uint32_t root = 0; root < count; ++root) {
                if (visited[root]) continue;
                stack.push_back(root);
                visited[root] = 1;
                while (!stack.empty()) {
                    const auto node = stack.back();
                    if (cursor[node] == offsets[node + 1]) {
                        finished.push_back(node);
                        stack.pop_back();
                        continue;
                    }
                    const auto target = edges[outgoing[cursor[node]++]].target;
                    if (!visited[target]) {
                        visited[target] = 1;
                        stack.push_back(target);
                    }
                }
            }
            std::vector<SignalFeedbackNode> nodes(count);
            std::fill(visited.begin(), visited.end(), 0);
            std::uint32_t components = 0;
            for (auto it = finished.rbegin(); it != finished.rend(); ++it) {
                if (visited[*it]) continue;
                stack.push_back(*it);
                visited[*it] = 1;
                while (!stack.empty()) {
                    const auto node = stack.back();
                    stack.pop_back();
                    nodes[node].component = components;
                    for (auto p = reverseOffsets[node]; p < reverseOffsets[node + 1]; ++p) {
                        const auto source = edges[incoming[p]].source;
                        if (!visited[source]) {
                            visited[source] = 1;
                            stack.push_back(source);
                        }
                    }
                }
                ++components;
            }

            // m = Q*1 + P*m in normal DAG order, within each structural SCC.
            // Parallel paths add. A stage traversed only by ordinary audio
            // retains its original external-input response.
            std::vector<double> sensitivity(count, 0), maximum(components, 0);
            for (const auto& edge : edges) {
                if (edge.delayed && nodes[edge.source].component == nodes[edge.target].component) {
                    nodes[edge.target].feedbackInput = true;
                    sensitivity[edge.target] += edge.bound;
                }
            }
            for (auto node : normalOrder) {
                if (!std::isfinite(sensitivity[node])) return false;
                const auto component = nodes[node].component;
                maximum[component] = std::max(maximum[component], sensitivity[node]);
                for (auto p = offsets[node]; p < offsets[node + 1]; ++p) {
                    const auto& edge = edges[outgoing[p]];
                    if (!edge.delayed && nodes[edge.target].component == component)
                        sensitivity[edge.target] += edge.bound * sensitivity[node];
                }
            }
            for (auto& node : nodes)
                if (node.feedbackInput)
                    node.slope = 1.0 / (1.0 / kDuckingBaselineSlope + maximum[node.component]);
            m_nodes.swap(nodes);
            m_normalOrder.swap(normalOrder);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::span<const SignalFeedbackNode> Nodes() const noexcept { return m_nodes; }
    std::span<const std::uint32_t> NormalOrder() const noexcept { return m_normalOrder; }

private:
    std::vector<SignalFeedbackNode> m_nodes;
    std::vector<std::uint32_t> m_normalOrder;
};

} // namespace harken

#endif // HARKEN_RENDER_SIGNAL_FEEDBACK_PLAN_H
