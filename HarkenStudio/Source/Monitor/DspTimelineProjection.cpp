#include "DspTimelineProjection.h"

namespace VNX {
namespace Monitor {

bool dspWallPublished()
{
    return false;
}

bool dspWorkPublished()
{
    return false;
}

bool dspCriticalPathPublished()
{
    return false;
}

DspTimelineProjection projectDspTimelineFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    DspTimelineProjection projection;
    projection.face = DspTimelineFace::UnsupportedG19;
    projection.gapId = "G19";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.wallPublished = dspWallPublished();
    projection.workPublished = dspWorkPublished();
    projection.criticalPathPublished = dspCriticalPathPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
