#include "ScopeTriggerProjection.h"

namespace VNX {
namespace Monitor {

bool scopeWindowPublished()
{
    return false;
}

bool scopeTriggerPublished()
{
    return false;
}

ScopeTriggerProjection projectScopeFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    ScopeTriggerProjection projection;
    projection.face = ScopeFace::Unpublished;
    projection.gapId = "P5-3";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.windowPublished = scopeWindowPublished();
    projection.triggerPublished = scopeTriggerPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
