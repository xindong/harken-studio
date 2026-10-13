
#include "BusRouteGraph.h"

#include <QSet>

#include <algorithm>

namespace VNX {

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
