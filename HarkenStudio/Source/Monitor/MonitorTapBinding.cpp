#include "MonitorTapBinding.h"

namespace VNX {
namespace Monitor {

bool tapRegistryPublished()
{
    return false;
}

bool tapBindingPublished()
{
    return false;
}

TapBindingProjection projectTapSelectFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    TapBindingProjection projection;
    projection.face = TapSelectFace::MissingRegistry;
    projection.gapId = "P5-1";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.registryPublished = tapRegistryPublished();
    projection.bound = tapBindingPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
