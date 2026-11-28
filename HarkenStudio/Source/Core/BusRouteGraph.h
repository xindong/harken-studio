
#pragma once

#include "ProjectTypes.h"
#include <QHash>
#include <QVector>

#include <cstdint>

namespace harken::studio {

struct BusSendState
{
    QUuid bus;
    std::vector<BusRoute> sends;
    std::vector<BusRoute> sidechains;
    bool operator==(const BusSendState&) const = default;
};

std::optional<BusSendState> captureBusSendState(const ProjectItem& bus);
std::optional<BusRoute> findBusRoute(const BusSendState& state, uint32_t targetBusID, SendType type, bool duckTarget);
bool restoreBusSendState(ProjectItem& bus, const BusSendState& state);
BusSendState withoutOutputConflicts(BusSendState state, uint32_t outputBusID);
std::optional<std::vector<Send>> effectiveBusSends(const ProjectItem& bus, PlatformScope scope = {});
std::vector<PlatformScope> busRoutingScopes(const ProjectData& project);
QString validateBusRouting(const ProjectData& project,
    const std::vector<BusSendState>& replacements = {},
    const QHash<QUuid, QUuid>& parentOverrides = {});

// Snapshot of the bus flow graph: tree edges (child -> parent, the dry signal
// path) plus send edges (from -> target, post-fader taps). Pure data, no
// project coupling — buildable from ProjectManager state or from a scan pass.
struct BusRouteSnapshot
{
    // child busID -> parent busID; the master/root bus maps to 0.
    QHash<uint32_t, uint32_t> parentOf;
    // from busID -> send target busIDs (dangling targets must be pre-filtered).
    QHash<uint32_t, QVector<uint32_t>> sendTargets;
};

// True if audio leaving startBusID can reach targetBusID by following tree
// edges upward and send edges. startBusID == targetBusID returns true.
bool busFlowReaches(const BusRouteSnapshot& g, uint32_t startBusID, uint32_t targetBusID);

// Full-graph cycle detection (Kahn peeling). Returns the set of bus IDs that
// sit on at least one cycle; empty means the graph is acyclic.
QVector<uint32_t> findBusRouteCycle(const BusRouteSnapshot& g);

}
