#include "SignalPathPlayback.h"

#include <QHash>

#include <algorithm>
#include <functional>

namespace VNX {

SignalPathActivity projectSignalPathActivity(
    const ProjectData& project, const SignalPathGraph& graph,
    const std::vector<HarkenEditor::Bridge::PlaybackPathObservation>& playback)
{
    SignalPathActivity result;
    result.edges.resize(graph.edges.size());
    for (const auto& node : graph.nodes) result.portPairs.emplace_back(node.portPairs.size(), false);
    if (!graph.ok() || playback.empty()) return result;

    QHash<QUuid, const ProjectItem*> items;
    const std::function<void(const ProjectItem&)> collect = [&](const ProjectItem& item) {
        items.insert(item.guid, &item);
        for (const auto& child : item.children) collect(*child);
    };
    for (const auto* binding : project.allFileBindings())
        if (binding->rootItem) collect(*binding->rootItem);
    const auto stableId = [&](const QUuid& guid) {
        const auto* item = items.value(guid);
        return item ? item->runtimeId() : harkenstudio::ObjectId{};
    };
    const auto nodeStableId = [&](size_t index) { return stableId(graph.nodes[index].key.objectGuid); };
    const auto activate = [&](const SignalPathEndpoint& endpoint) {
        result.portPairs[endpoint.occurrenceIndex][endpoint.portPairIndex] = true;
    };

    for (const auto& voice : playback) {
        std::vector<harkenstudio::ObjectId> definitions;
        std::vector<HarkenEditor::Bridge::PlaybackPathRow> uses;
        for (auto row = voice.rows.rbegin(); row != voice.rows.rend(); ++row) {
            if (row->occurrenceOrdinal == UINT32_MAX) definitions.push_back(row->stableId);
            else uses.push_back(*row);
        }
        std::vector<size_t> matches;
        for (size_t index = 0; index < graph.paths.size(); ++index) {
            const auto& path = graph.paths[index];
            if (voice.soundId == harkenstudio::ObjectId{} || stableId(path.identity.sourceGuid) != voice.soundId
                || definitions.size() != path.computation.size()) continue;
            if (!std::equal(definitions.begin(), definitions.end(), path.computation.begin(),
                    [&](harkenstudio::ObjectId id, const auto& endpoint) { return id == nodeStableId(endpoint.occurrenceIndex); }))
                continue;
            size_t observed = 0;
            for (const auto& use : path.identity.occurrences) {
                if (observed < uses.size() && uses[observed].stableId == stableId(use.ownerGuid)
                    && uses[observed].occurrenceOrdinal == use.stableOccurrenceOrdinal)
                    ++observed;
            }
            if (observed == uses.size()) matches.push_back(index);
        }
        if (matches.empty()) continue;
        const auto pathIndex = matches.front();
        const auto& path = graph.paths[pathIndex];
        if (std::any_of(matches.begin(), matches.end(), [&](size_t index) {
                const auto& edges = graph.paths[index].edgeIndices;
                return !std::is_permutation(path.edgeIndices.begin(), path.edgeIndices.end(),
                                            edges.begin(), edges.end());
            })) continue;
        for (const auto& endpoint : path.computation) activate(endpoint);
        std::vector<SignalPathEndpoint> pending{path.computation.back()};
        std::vector<bool> visited(graph.edges.size());
        for (size_t edgeIndex : path.edgeIndices) {
            const auto& edge = graph.edges[edgeIndex];
            if (edge.kind == SignalPathEdgeKind::Through) result.edges[edgeIndex] = true;
        }
        for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (size_t edgeIndex : path.edgeIndices) {
                const auto& edge = graph.edges[edgeIndex];
                if (visited[edgeIndex] || edge.from != pending[cursor]) continue;
                if (cursor == 0) {
                    const auto target = nodeStableId(edge.to.occurrenceIndex);
                    if (edge.kind == SignalPathEdgeKind::Output && target != voice.outputBusId) continue;
                    if (edge.kind == SignalPathEdgeKind::Send
                        && std::none_of(voice.sends.begin(), voice.sends.end(),
                            [&](const auto& send) { return send.targetBusId == target; })) continue;
                }
                visited[edgeIndex] = true;
                result.edges[edgeIndex] = true;
                activate(edge.from);
                activate(edge.to);
                pending.push_back(edge.to);
            }
        }
    }
    return result;
}

}
