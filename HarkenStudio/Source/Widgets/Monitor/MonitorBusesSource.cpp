#include "Monitor/MonitorBusesSource.h"

#include "Monitor/MonitorBusesPanel.h"

namespace VNX {

void MonitorBusesSource::attach(MonitorBusesPanel* panel, MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorBusesSource::apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                               bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorBusesSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
