#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class LineageFace
{
    UnsupportedAdjacency = 0,
};

struct LineageProjection
{
    LineageFace face = LineageFace::UnsupportedAdjacency;
    const char* gapId = "P4-2";
    const char* layerGapId = "G21";
    std::uint64_t generation = 0;
    bool adjacencyPublished = false;
    bool layerEntityPublished = false;
};

bool lineageAdjacencyPublished();
bool lineageLayerEntityPublished();
LineageProjection projectLineageFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot);

} // namespace Monitor
} // namespace VNX
