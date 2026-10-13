#include "PlayableLifecycleProjection.h"
#include "Monitor/MonitorTypes.h"

namespace VNX {
namespace Monitor {

bool playableAsOfStatePublished()
{
    return false;
}

bool variableControlEntityKindPublished()
{
    return false;
}

PlayableLifecycleView projectPlayableLifecycle(const MonitorRecordHeader&)
{
    PlayableLifecycleView view;
    view.cell = PlayableLifecycleCell::UnsupportedAsOf;
    view.gapId = "P4-1";
    return view;
}

MonitorPlayablesProjection projectPlayablesFace(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    MonitorPlayablesProjection projection;
    projection.face = MonitorPlayablesFace::UnsupportedAsOf;
    projection.gapId = "P4-1";
    projection.generation = snapshot ? snapshot->generation : 0;
    projection.variableControlKindPublished = variableControlEntityKindPublished();
    return projection;
}

} // namespace Monitor
} // namespace VNX
