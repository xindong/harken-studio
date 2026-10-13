#include "LineageProjection.h"

namespace VNX {
namespace Monitor {

bool lineageAdjacencyPublished()
{
    return false;
}

bool lineageLayerEntityPublished()
{
    return false;
}

LineageProjection projectLineageFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    LineageProjection projection;
    projection.face = LineageFace::UnsupportedAdjacency;
    projection.gapId = "P4-2";
    projection.layerGapId = "G21";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.adjacencyPublished = lineageAdjacencyPublished();
    projection.layerEntityPublished = lineageLayerEntityPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
