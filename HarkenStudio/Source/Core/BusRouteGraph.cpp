
#include "BusRouteGraph.h"
#include "Components/VMixNode.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <set>

namespace VNX {

std::optional<BusSendState> captureBusSendState(const ProjectItem& bus, PlatformScope scope)
{
    if (bus.type != ProjectItemType::Bus) return {};
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(bus);
    if (!locator) return {};
    constexpr auto id = static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Sends);
    const auto state = scope
        ? PlayablePropertyAuthoringV1::readPlatformOverlay(const_cast<ProjectItem&>(bus), *locator, id, *scope)
        : PlayablePropertyAuthoringV1::readProperty(const_cast<ProjectItem&>(bus), *locator, id, {});
    if (!state) return {};
    return BusSendState{bus.guid, scope, *state};
}

bool restoreBusSendState(ProjectItem& bus, const BusSendState& state)
{
    if (bus.type != ProjectItemType::Bus || bus.guid != state.bus) return false;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(bus);
    return locator && PlayablePropertyAuthoringV1::restoreProperty(bus, *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Sends), state.value, state.scope);
}

std::optional<QJsonArray> busSendRows(const ProjectItem& bus, PlatformScope scope)
{
    if (bus.type != ProjectItemType::Bus) return {};
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(bus);
    if (!locator) return {};
    const auto state = PlayablePropertyAuthoringV1::readProperty(const_cast<ProjectItem&>(bus), *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Sends), scope);
    if (!state || (state->present && !state->value.isArray())) return {};
    return state->present ? state->value.toArray() : QJsonArray{};
}

std::optional<std::vector<Send>> effectiveBusSends(const ProjectItem& bus, PlatformScope scope)
{
    const auto rows = busSendRows(bus, scope);
    if (!rows) return {};
    std::vector<Send> sends;
    sends.reserve(static_cast<size_t>(rows->size()));
    for (const auto& value : *rows) {
        auto row = value.toObject();
        row[QStringLiteral("sendLevel")] = VMixNode::dbToLinear(static_cast<float>(row.value(QStringLiteral("sendLevel")).toDouble()));
        const auto send = Send::fromJson(row);
        if (!send) return {};
        sends.push_back(*send);
    }
    return sends;
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
    for (const auto& state : replacements) {
        if (!byGuid.contains(state.bus)
            || (state.scope && !ProjectPlatformSemantics::find(project.platforms, *state.scope)))
            return QStringLiteral("Bus Send target or platform is unavailable");
    }
    const auto stateFor = [&](const ProjectItem& bus, PlatformScope scope) -> std::optional<BusSendState> {
        const auto found = std::find_if(replacements.begin(), replacements.end(), [&](const auto& state) {
            return state.bus == bus.guid && state.scope == scope;
        });
        return found == replacements.end() ? captureBusSendState(bus, scope) : std::optional{*found};
    };
    for (const auto& scope : busRoutingScopes(project)) {
        BusRouteSnapshot routes;
        for (const auto* bus : buses) {
            const auto parentOverride = parentOverrides.constFind(bus->guid);
            const auto* parent = parentOverride == parentOverrides.constEnd() ? bus->parent
                : byGuid.value(parentOverride.value(), nullptr);
            const uint32_t output = parent && parent->type == ProjectItemType::Bus ? parent->objectID : 0u;
            routes.parentOf.insert(bus->objectID, output);
            auto state = stateFor(*bus, scope);
            if (scope && state && !state->value.present) state = stateFor(*bus, {});
            if (!state || (state->value.present && !state->value.value.isArray()))
                return QStringLiteral("Bus Send configuration is unavailable");
            std::set<std::pair<uint32_t, SendMode>> identities;
            for (const auto& value : state->value.present ? state->value.value.toArray() : QJsonArray{}) {
                if (!value.isObject()) return QStringLiteral("Bus Send row is invalid");
                const auto row = value.toObject();
                const double rawTarget = row.value(QStringLiteral("targetBusID")).toDouble();
                const auto target = static_cast<uint32_t>(row.value(QStringLiteral("targetBusID")).toInteger());
                const auto rawMode = row.value(QStringLiteral("mode"));
                const int modeValue = rawMode.isUndefined() ? 0 : rawMode.toInt(-1);
                const auto mode = static_cast<SendMode>(modeValue);
                const auto rawGain = row.value(QStringLiteral("sendLevel"));
                const double gain = rawGain.toDouble();
                std::map<SendMode, float> volumes;
                if (!row.value(QStringLiteral("targetBusID")).isDouble() || rawTarget != target
                    || !buses.contains(target) || target == bus->objectID
                    || (!rawMode.isUndefined() && (!rawMode.isDouble() || rawMode.toDouble() != modeValue))
                    || modeValue < 0 || modeValue > 2 || !rawGain.isDouble() || !std::isfinite(gain)
                    || gain < -96.0 || gain > 24.0
                    || !readSendModeVolumes(row.value(QStringLiteral("modeVolumes")), mode, volumes)
                    || (mode != SendMode::Sidechain && target == output)
                    || !identities.emplace(target, mode).second)
                    return QStringLiteral("Bus Send has an invalid target, mode, volume, or output conflict");
                routes.sendTargets[bus->objectID].push_back(target);
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
