#include "SignalPathFocus.h"

#include <algorithm>

namespace VNX {

SignalPathFocus projectSignalPathFocus(const SignalPathGraph& graph,
                                      const std::vector<size_t>& selectedNodes,
                                      const std::vector<size_t>& selectedEdges)
{
    SignalPathFocus result;
    result.nodes.resize(graph.nodes.size());
    result.edges.resize(graph.edges.size());
    if (!graph.ok()) return result;
    std::vector<bool> selected(graph.nodes.size());
    for (const auto index : selectedNodes) {
        if (index >= graph.nodes.size()) continue;
        selected[index] = true;
        result.nodes[index] = true;
        result.active = true;
    }
    const auto includeEdge = [&](size_t index) {
        const auto& edge = graph.edges[index];
        result.edges[index] = true;
        result.nodes[edge.from.occurrenceIndex] = true;
        result.nodes[edge.to.occurrenceIndex] = true;
    };
    std::vector<bool> selectedPaths(graph.paths.size());
    for (const auto index : selectedEdges) {
        if (index >= graph.edges.size()) continue;
        result.active = true;
        includeEdge(index);
        for (const auto pathIndex : graph.edges[index].pathIndices)
            selectedPaths[pathIndex] = true;
    }
    if (!result.active) return result;

    for (size_t pathIndex = 0; pathIndex < graph.paths.size(); ++pathIndex) {
        const auto& path = graph.paths[pathIndex];
        const bool related = selectedPaths[pathIndex]
            || std::any_of(path.computation.begin(), path.computation.end(),
            [&](const auto& endpoint) { return selected[endpoint.occurrenceIndex]; })
            || std::any_of(path.edgeIndices.begin(), path.edgeIndices.end(), [&](size_t index) {
                const auto& edge = graph.edges[index];
                return selected[edge.from.occurrenceIndex] || selected[edge.to.occurrenceIndex];
            });
        if (!related) continue;
        for (const auto& endpoint : path.computation)
            result.nodes[endpoint.occurrenceIndex] = true;
        for (const auto index : path.edgeIndices) includeEdge(index);
    }

    for (const bool upstream : {false, true}) {
        std::vector<size_t> pending;
        std::vector<bool> visited(graph.nodes.size());
        const auto enqueue = [&](size_t index) {
            if (!visited[index]) {
                pending.push_back(index);
                visited[index] = true;
            }
        };
        for (const auto index : selectedNodes) {
            if (index < graph.nodes.size() && graph.nodes[index].type == ProjectItemType::Bus)
                enqueue(index);
        }
        for (const auto index : selectedEdges) {
            if (index >= graph.edges.size()) continue;
            const auto& edge = graph.edges[index];
            if (!edge.pathIndices.empty()
                || graph.nodes[edge.from.occurrenceIndex].type != ProjectItemType::Bus
                || graph.nodes[edge.to.occurrenceIndex].type != ProjectItemType::Bus) continue;
            enqueue(upstream ? edge.from.occurrenceIndex : edge.to.occurrenceIndex);
        }
        for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (size_t index = 0; index < graph.edges.size(); ++index) {
                const auto& edge = graph.edges[index];
                const auto from = edge.from.occurrenceIndex;
                const auto to = edge.to.occurrenceIndex;
                if (graph.nodes[from].type != ProjectItemType::Bus
                    || graph.nodes[to].type != ProjectItemType::Bus
                    || (upstream ? to : from) != pending[cursor]) continue;
                includeEdge(index);
                const auto next = upstream ? from : to;
                enqueue(next);
            }
        }
    }
    return result;
}

}
