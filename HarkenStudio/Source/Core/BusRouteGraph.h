
#pragma once

#include <QHash>
#include <QVector>

#include <cstdint>

namespace VNX {

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
