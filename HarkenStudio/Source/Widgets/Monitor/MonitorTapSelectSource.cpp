#include "Monitor/MonitorTapSelectSource.h"

#include "Monitor/MonitorTapSelectPanel.h"

namespace VNX {

void MonitorTapSelectSource::attach(MonitorTapSelectPanel* panel,
                                    MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorTapSelectSource::apply(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorTapSelectSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
