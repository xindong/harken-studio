#include "Monitor/MonitorScopeSource.h"

#include "Monitor/MonitorScopePanel.h"

namespace VNX {

void MonitorScopeSource::attach(MonitorScopePanel* panel,
                                MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorScopeSource::apply(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorScopeSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
