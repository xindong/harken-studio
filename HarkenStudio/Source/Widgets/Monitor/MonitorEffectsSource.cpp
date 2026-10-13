#include "Monitor/MonitorEffectsSource.h"

#include "Monitor/MonitorEffectsPanel.h"

namespace VNX {

void MonitorEffectsSource::attach(MonitorEffectsPanel* panel, MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorEffectsSource::apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                                 bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorEffectsSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
