#include "Monitor/MonitorDspTimelineSource.h"

#include "Monitor/MonitorDspTimelinePanel.h"

namespace VNX {

void MonitorDspTimelineSource::attach(MonitorDspTimelinePanel* panel,
                                      MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorDspTimelineSource::apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                                     bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorDspTimelineSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
