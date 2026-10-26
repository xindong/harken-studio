#include "SignalPathGraph.h"

#include "Core/PlaybackReferenceValidation.h"
#include "Core/ReferenceSlotResolution.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace VNX {
namespace {

namespace Slots = ReferenceSlotResolution;

void appendUnique(std::vector<size_t>& values, size_t value)
{
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

class Builder
{
public:
    Builder(const ProjectData& project, FieldGroupScope platform,
            std::shared_ptr<const FieldGroupEffectiveSnapshot> groups)
        : groups_(groups ? std::move(groups) : std::make_shared<FieldGroupEffectiveSnapshot>(
              buildFieldGroupEffectiveSnapshot(project, platform)))
    {
        graph_.platform = platform;
        for (const auto& diagnostic : groups_->diagnostics)
            graph_.diagnostics.push_back({{}, diagnostic.message});
        for (const auto& [guid, item] : groups_->objects) {
            if (item->type == ProjectItemType::Bus)
                buses_.emplace(item->objectID, item);
        }
    }

    SignalPathGraph build(const std::vector<SignalPathIdentity>& sources,
                          const std::vector<uint32_t>& busRoots)
    {
        if (!graph_.ok()) return std::move(graph_);
        for (const auto& source : sources) {
            if (std::any_of(graph_.paths.begin(), graph_.paths.end(),
                    [&](const auto& path) { return path.identity == source; }))
                continue;
            appendPath(source);
        }
        for (uint32_t busRoot : busRoots) {
            if (graph_.ok()) appendBusHierarchy(busRoot);
        }
        if (!graph_.ok()) {
            graph_.nodes.clear();
            graph_.edges.clear();
            graph_.paths.clear();
        }
        return std::move(graph_);
    }

private:
    void appendBusHierarchy(uint32_t id)
    {
        std::set<uint32_t> active;
        if (!validateBus(id, {}, active)) return;
        std::set<uint32_t> visited;
        appendBus(id, {}, visited);
        for (const auto& child : buses_.at(id)->children) {
            if (child->type == ProjectItemType::Bus) appendBusHierarchy(child->objectID);
        }
    }

    const ProjectItem* findItem(const QUuid& guid) const
    {
        const auto found = groups_->objects.find(toFieldGroupGuid(guid));
        return found == groups_->objects.end() ? nullptr : found->second;
    }

    bool validateOccurrences(const SignalPathIdentity& identity,
                             const Slots::ResolutionScope& scope)
    {
        auto next = scope.candidates.begin();
        for (const auto& occurrence : identity.occurrences) {
            const auto* owner = findItem(occurrence.ownerGuid);
            std::optional<PlayablePropertyLocatorV1> current;
            if (owner) {
                if (occurrence.kind == PlayablePropertyTargetKindV1::TrackClip)
                    current = PlayablePropertyAuthoringV1::trackClipLocator(*owner, occurrence.occurrenceGuid);
                else if (occurrence.kind == PlayablePropertyTargetKindV1::ContainerChild)
                    current = PlayablePropertyAuthoringV1::containerChildLocator(*owner, occurrence.stableOccurrenceOrdinal,
                        graph_.platform ? PlatformScope{fromFieldGroupGuid(*graph_.platform)} : std::nullopt);
            }
            if (!current || *current != occurrence) {
                graph_.diagnostics.push_back({identity,
                    QStringLiteral("Signal path occurrence is unavailable or has changed.")});
                return false;
            }
            QString error;
            const auto* target = findItem(occurrence.targetGuid);
            if (!validateDirectPlaybackTarget(*owner, occurrence.targetGuid, target, &error)) {
                graph_.diagnostics.push_back({identity, error});
                return false;
            }
            next = std::find_if(next, scope.candidates.end(), [&](const auto& candidate) {
                return candidate.item == target;
            });
            if (next == scope.candidates.end()) {
                graph_.diagnostics.push_back({identity,
                    QStringLiteral("Signal path occurrences must follow the source's ancestry, nearest first.")});
                return false;
            }
            ++next;
        }
        return true;
    }

    size_t node(const ProjectItem& item)
    {
        const auto [entry, inserted] = nodes_.try_emplace(item.guid, graph_.nodes.size());
        if (inserted)
            graph_.nodes.push_back({{item.guid}, item.objectID, item.type, item.name, {{{}, {}}}});
        return entry->second;
    }

    SignalPathEndpoint port(size_t occurrenceIndex,
                            std::optional<SignalPathRouting> routing,
                            std::optional<size_t> pathIndex)
    {
        auto& pair = graph_.nodes[occurrenceIndex].portPairs.front();
        if (pair.pathIndices.empty()) pair.routing = std::move(routing);
        else if (pair.routing != routing) pair.routing.reset();
        if (pathIndex) appendUnique(pair.pathIndices, *pathIndex);
        return {occurrenceIndex, 0};
    }

    void edge(SignalPathEndpoint from, SignalPathEndpoint to,
              SignalPathEdgeKind kind, std::optional<size_t> pathIndex, float sendLevel = 0.0f,
              std::optional<SignalPathRoutingSource> routingSource = {})
    {
        size_t index = 0;
        for (; index < graph_.edges.size(); ++index) {
            const auto& candidate = graph_.edges[index];
            if (candidate.from == from && candidate.to == to
                && candidate.kind == kind && candidate.sendLevel == sendLevel
                && candidate.routingSource == routingSource)
                break;
        }
        if (index == graph_.edges.size())
            graph_.edges.push_back({from, to, kind, sendLevel, {}, std::move(routingSource)});
        if (pathIndex) {
            appendUnique(graph_.edges[index].pathIndices, *pathIndex);
            appendUnique(graph_.paths[*pathIndex].edgeIndices, index);
        }
    }

    bool validateBus(uint32_t id, const std::optional<SignalPathIdentity>& path,
                     std::set<uint32_t>& active)
    {
        if (validatedBuses_.contains(id)) return true;
        const auto found = buses_.find(id);
        if (found == buses_.end()) {
            graph_.diagnostics.push_back({path,
                QStringLiteral("Signal path Bus %1 is unavailable.").arg(id)});
            return false;
        }
        if (!active.insert(id).second) {
            graph_.diagnostics.push_back({path,
                QStringLiteral("Signal path contains a Bus cycle at %1.").arg(id)});
            return false;
        }
        const auto* bus = found->second;
        if (bus->parent && bus->parent->type == ProjectItemType::Bus
            && !validateBus(bus->parent->objectID, path, active))
            return false;
        const auto sends = Slots::resolveEffectiveSends(
            Slots::buildResolutionScope(bus, groups_));
        for (const auto& send : sends.sends) {
            if (!validateBus(send.targetBusID, path, active)) return false;
        }
        active.erase(id);
        validatedBuses_.insert(id);
        return true;
    }

    SignalPathEndpoint busEndpoint(uint32_t id, std::optional<size_t> pathIndex)
    {
        const auto& bus = *buses_.at(id);
        return port(node(bus), {}, pathIndex);
    }

    void appendBus(uint32_t id, std::optional<size_t> pathIndex, std::set<uint32_t>& visited)
    {
        const auto from = busEndpoint(id, pathIndex);
        if (!visited.insert(id).second) return;
        const auto* bus = buses_.at(id);
        const SignalPathRoutingSource routingSource{bus->guid, {}};
        if (bus->parent && bus->parent->type == ProjectItemType::Bus) {
            const auto parentID = bus->parent->objectID;
            edge(from, busEndpoint(parentID, pathIndex), SignalPathEdgeKind::Output,
                 pathIndex, 0.0f, routingSource);
            appendBus(parentID, pathIndex, visited);
        }
        const auto sends = Slots::resolveEffectiveSends(
            Slots::buildResolutionScope(bus, groups_));
        for (const auto& send : sends.sends) {
            edge(from, busEndpoint(send.targetBusID, pathIndex),
                 SignalPathEdgeKind::Send, pathIndex, send.sendLevel, routingSource);
            appendBus(send.targetBusID, pathIndex, visited);
        }
    }

    void appendPath(const SignalPathIdentity& identity)
    {
        const auto* source = findItem(identity.sourceGuid);
        if (!source || !PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(source->type)) {
            graph_.diagnostics.push_back({identity,
                QStringLiteral("Signal path source is not an available content object.")});
            return;
        }
        const auto scope = Slots::buildResolutionScope(source, groups_);
        if (!validateOccurrences(identity, scope)) return;
        const auto sends = Slots::resolveEffectiveSends(scope);
        if (!sends.valid) {
            graph_.diagnostics.push_back({identity, sends.error});
            return;
        }
        SignalPathRouting routing;
        routing.outputBusID = Slots::resolveEffectiveOutputBusId(scope);
        for (const auto& send : sends.sends)
            routing.sends.push_back({send.targetBusID, send.sendLevel});
        std::sort(routing.sends.begin(), routing.sends.end(), [](const auto& a, const auto& b) {
            return a.targetBusID < b.targetBusID;
        });
        std::set<uint32_t> active;
        if (!validateBus(routing.outputBusID, identity, active)) return;
        for (const auto& send : routing.sends) {
            if (!validateBus(send.targetBusID, identity, active)) return;
        }

        const size_t pathIndex = graph_.paths.size();
        graph_.paths.push_back({identity, routing, {}, {}});
        for (const auto& candidate : scope.candidates) {
            const auto& item = *candidate.item;
            const auto endpoint = port(node(item), routing, pathIndex);
            auto& computation = graph_.paths[pathIndex].computation;
            if (!computation.empty())
                edge(computation.back(), endpoint, SignalPathEdgeKind::Through, pathIndex);
            computation.push_back(endpoint);
        }

        const auto terminal = graph_.paths[pathIndex].computation.back();
        const auto& effective = groups_->definitions.at(toFieldGroupGuid(source->guid));
        const SignalPathRoutingSource outputSource{
            fromFieldGroupGuid(effective.mainOutputSource.definition), effective.mainOutputSource.scope};
        const SignalPathRoutingSource sendSource{
            fromFieldGroupGuid(effective.sendSource.definition), effective.sendSource.scope};
        edge(terminal, busEndpoint(routing.outputBusID, pathIndex),
             SignalPathEdgeKind::Output, pathIndex, 0.0f, outputSource);
        std::set<uint32_t> visited;
        appendBus(routing.outputBusID, pathIndex, visited);
        for (const auto& send : routing.sends) {
            edge(terminal, busEndpoint(send.targetBusID, pathIndex),
                 SignalPathEdgeKind::Send, pathIndex, send.sendLevel, sendSource);
            appendBus(send.targetBusID, pathIndex, visited);
        }
    }

    std::shared_ptr<const FieldGroupEffectiveSnapshot> groups_;
    SignalPathGraph graph_;
    std::map<uint32_t, const ProjectItem*> buses_;
    std::set<uint32_t> validatedBuses_;
    std::map<QUuid, size_t> nodes_;
};

}

SignalPathGraph buildSignalPathGraph(
    const ProjectData& project, const std::vector<SignalPathIdentity>& sources,
    FieldGroupScope platform, const std::vector<uint32_t>& busRoots,
    std::shared_ptr<const FieldGroupEffectiveSnapshot> groups)
{
    return Builder(project, platform, std::move(groups)).build(sources, busRoots);
}

}
