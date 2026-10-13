#pragma once

#include "SignalPathGraph.h"

namespace VNX {

struct SignalPathFocus
{
    bool active = false;
    std::vector<bool> nodes;
    std::vector<bool> edges;
};

SignalPathFocus projectSignalPathFocus(const SignalPathGraph& graph,
                                      const std::vector<size_t>& selectedNodes,
                                      const std::vector<size_t>& selectedEdges = {});

}
