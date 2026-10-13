#pragma once

#include "SignalPathGraph.h"
#include "RuntimeBridge/DiagnosticsBridge.h"

namespace VNX {

struct SignalPathActivity
{
    std::vector<std::vector<bool>> portPairs;
    std::vector<bool> edges;
};

SignalPathActivity projectSignalPathActivity(
    const ProjectData& project, const SignalPathGraph& graph,
    const std::vector<HarkenEditor::Bridge::PlaybackPathObservation>& playback);

}
