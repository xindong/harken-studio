#include "Monitor/MonitorVoicesSource.h"

#include "Monitor/MonitorVoicesPanel.h"

namespace VNX {

void MonitorVoicesSource::attach(MonitorVoicesPanel* panel, MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorVoicesSource::apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                               bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorVoicesSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
