#include "Monitor/MonitorWaveformSource.h"

#include "Monitor/MonitorWaveformPanel.h"

namespace VNX {

void MonitorWaveformSource::attach(MonitorWaveformPanel* panel,
                                   MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    sharedCursor_ = sharedCursor;
    if (panel_)
        panel_->setSharedCursor(sharedCursor_);
    pushToPanel();
}

void MonitorWaveformSource::apply(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    pushToPanel();
}

void MonitorWaveformSource::pushToPanel() const
{
    if (panel_)
        panel_->bindSnapshot(snapshot_, stateStreamSubscribed_);
}

} // namespace VNX
