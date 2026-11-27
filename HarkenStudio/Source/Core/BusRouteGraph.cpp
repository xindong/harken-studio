
#include "BusRouteGraph.h"
#include "Components/VMixNode.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <set>

namespace harken::studio {

std::optional<BusSendState> captureBusSendState(const ProjectItem& bus)
{
    if (bus.type != ProjectItemType::Bus) return {};
    const auto* mix = bus.comp<VMixNode>();
    if (!mix) return BusSendState{bus.guid, {}, {}};
    return BusSendState{bus.guid, mix->sends, mix->sidechains};
}

std::optional<BusRoute> findBusRoute(const BusSendState& state, uint32_t targetBusID, SendType type, bool duckTarget)
{
    const auto& rows = type == SendType::Sidechain ? state.sidechains : state.sends;
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const BusRoute& row) {
        return row.targetBusID == targetBusID && row.duckTarget == duckTarget;
    });
    return found == rows.end() ? std::nullopt : std::optional{*found};
}

bool restoreBusSendState(ProjectItem& bus, const BusSendState& state)
{
    if (bus.type != ProjectItemType::Bus || bus.guid != state.bus) return false;
    auto* mix = bus.compOrCreate<VMixNode>();
    mix->setHostItem(&bus);
    mix->sends = state.sends;
    mix->sidechains = state.sidechains;
    constexpr auto sends = harkenplayableproperties::PlayablePropertyIdV1::Sends;
    if (state.sends.empty())
        bus.playablePropertyPresence.Clear(sends);
    else
        bus.playablePropertyPresence.Set(sends);
    return true;
}

BusSendState withoutOutputConflicts(BusSendState state, uint32_t outputBusID)
{
    for (const bool sidechain : {false, true}) {
        const auto conflicts = [&](const BusRoute& route) {
            return route.targetBusID == outputBusID && (!sidechain || route.duckTarget);
        };
        auto& rows = sidechain ? state.sidechains : state.sends;
        std::erase_if(rows, [&](const BusRoute& row) { return conflicts(row.forPlatform({})); });
        for (auto& row : rows)
            std::erase_if(row.platforms, [&](const auto& cell) { return conflicts(row.forPlatform(cell.first)); });
    }
    return state;
}

std::optional<std::vector<Send>> effectiveBusSends(const ProjectItem& bus, PlatformScope scope)
{
    if (bus.type != ProjectItemType::Bus) return {};
    const auto* mix = bus.comp<VMixNode>();
    return mix ? mix->effectiveSends(scope) : std::vector<Send>{};
}

std::vector<PlatformScope> busRoutingScopes(const ProjectData& project)
{
    std::vector<PlatformScope> scopes{{}};
    for (const auto& platform : project.platforms) scopes.push_back(platform.id);
    return scopes;
}

QString validateBusRouting(const ProjectData& project, const std::vector<BusSendState>& replacements,
                          const QHash<QUuid, QUuid>& parentOverrides)
{
    QHash<uint32_t, const ProjectItem*> buses;
    QHash<QUuid, const ProjectItem*> byGuid;
    QSet<const ProjectItem*> visited;
    std::function<void(const ProjectItem*)> collect = [&](const ProjectItem* item) {
        if (!item || visited.contains(item)) return;
        visited.insert(item);
        if (item->type == ProjectItemType::Bus) {
            buses.insert(item->objectID, item);
            byGuid.insert(item->guid, item);
        }
        for (const auto& child : item->children) collect(child.get());
    };
    collect(project.root.get());
    for (const auto* binding : project.allFileBindings())
        if (binding) collect(binding->rootItem.get());
    for (const auto& state : replacements)
        if (!byGuid.contains(state.bus)) return QStringLiteral("Bus Send owner is unavailable");
    const auto stateFor = [&](const ProjectItem& bus) -> std::optional<BusSendState> {
        const auto found = std::find_if(replacements.begin(), replacements.end(), [&](const auto& state) {
            return state.bus == bus.guid;
        });
        return found == replacements.end() ? captureBusSendState(bus) : std::optional{*found};
    };
    const auto rowsValid = [&](const std::vector<BusRoute>& rows) {
        QSet<QUuid> ids;
        for (const auto& row : rows) {
            if (row.id.isNull() || ids.contains(row.id)) return false;
            ids.insert(row.id);
            for (const auto& [platform, cells] : row.platforms)
                if (!ProjectPlatformSemantics::find(project.platforms, platform)) return false;
        }
        return true;
    };
    for (const auto* bus : buses) {
        const auto state = stateFor(*bus);
        if (!state || !rowsValid(state->sends) || !rowsValid(state->sidechains))
            return QStringLiteral("Bus Send rows are invalid");
    }
    for (const auto& scope : busRoutingScopes(project)) {
        BusRouteSnapshot routes;
        for (const auto* bus : buses) {
            const auto parentOverride = parentOverrides.constFind(bus->guid);
            const auto* parent = parentOverride == parentOverrides.constEnd() ? bus->parent
                : byGuid.value(parentOverride.value(), nullptr);
            const uint32_t output = parent && parent->type == ProjectItemType::Bus ? parent->objectID : 0u;
            routes.parentOf.insert(bus->objectID, output);
            const auto state = stateFor(*bus);
            const auto routeValid = [&](const BusRoute& route) {
                return buses.contains(route.targetBusID) && route.targetBusID != bus->objectID
                    && std::isfinite(route.sendLevelDb) && route.sendLevelDb >= -96.0f && route.sendLevelDb <= 24.0f;
            };
            std::set<uint32_t> sendTargets;
            for (const auto& row : state->sends) {
                const auto route = row.forPlatform(scope);
                if (!routeValid(route) || route.targetBusID == output || !sendTargets.insert(route.targetBusID).second)
                    return QStringLiteral("Bus Send has an invalid target, volume, or output conflict");
                routes.sendTargets[bus->objectID].push_back(route.targetBusID);
            }
            std::set<std::pair<uint32_t, bool>> sidechainTargets;
            for (const auto& row : state->sidechains) {
                const auto route = row.forPlatform(scope);
                if (!routeValid(route) || (route.duckTarget && route.targetBusID == output)
                    || !sidechainTargets.emplace(route.targetBusID, route.duckTarget).second)
                    return QStringLiteral("Bus Sidechain has an invalid target, Target Ducking setting, volume, or output conflict");
                routes.sendTargets[bus->objectID].push_back(route.targetBusID);
            }
        }
        if (!findBusRouteCycle(routes).isEmpty()) return QStringLiteral("Bus routing would create a cycle");
    }
    return {};
}

namespace {

void appendOutEdges(const BusRouteSnapshot& g, uint32_t busID, QVector<uint32_t>& out)
{
    const uint32_t parent = g.parentOf.value(busID, 0u);
    if (parent != 0u) out.push_back(parent);
    auto it = g.sendTargets.constFind(busID);
    if (it != g.sendTargets.constEnd()) out.append(it.value());
}

}

bool busFlowReaches(const BusRouteSnapshot& g, uint32_t startBusID, uint32_t targetBusID)
{
    if (startBusID == 0u || targetBusID == 0u) return false;
    if (startBusID == targetBusID) return true;

    QSet<uint32_t> visited;
    QVector<uint32_t> stack;
    stack.push_back(startBusID);
    visited.insert(startBusID);

    while (!stack.isEmpty()) {
        const uint32_t cur = stack.takeLast();
        QVector<uint32_t> next;
        appendOutEdges(g, cur, next);
        for (uint32_t n : next) {
            if (n == targetBusID) return true;
            if (visited.contains(n)) continue;
            visited.insert(n);
            stack.push_back(n);
        }
    }
    return false;
}

QVector<uint32_t> findBusRouteCycle(const BusRouteSnapshot& g)
{
    QSet<uint32_t> nodes;
    for (auto it = g.parentOf.constBegin(); it != g.parentOf.constEnd(); ++it)
        nodes.insert(it.key());
    for (auto it = g.sendTargets.constBegin(); it != g.sendTargets.constEnd(); ++it)
        nodes.insert(it.key());

    QHash<uint32_t, int> inDegree;
    for (uint32_t n : nodes) inDegree.insert(n, 0);
    for (uint32_t n : nodes) {
        QVector<uint32_t> next;
        appendOutEdges(g, n, next);
        for (uint32_t t : next) {
            if (nodes.contains(t)) ++inDegree[t];
        }
    }

    QVector<uint32_t> queue;
    for (auto it = inDegree.constBegin(); it != inDegree.constEnd(); ++it)
        if (it.value() == 0) queue.push_back(it.key());

    QSet<uint32_t> remaining = nodes;
    while (!queue.isEmpty()) {
        const uint32_t cur = queue.takeLast();
        remaining.remove(cur);
        QVector<uint32_t> next;
        appendOutEdges(g, cur, next);
        for (uint32_t t : next) {
            if (!remaining.contains(t)) continue;
            if (--inDegree[t] == 0) queue.push_back(t);
        }
    }

    QVector<uint32_t> cycle(remaining.begin(), remaining.end());
    std::sort(cycle.begin(), cycle.end());
    return cycle;
}

}
