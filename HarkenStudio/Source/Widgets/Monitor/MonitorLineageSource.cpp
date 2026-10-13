#include "Monitor/MonitorLineageSource.h"

#include "Monitor/MonitorLineagePanel.h"

namespace VNX {

void MonitorLineageSource::attach(MonitorLineagePanel* panel, MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorLineageSource::apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                                 bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorLineageSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
