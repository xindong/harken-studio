#pragma once

#include "Monitor/MonitorStore.h"

#include <memory>

namespace VNX {

class MonitorDspTimelinePanel;
class MonitorCursorModel;

class MonitorDspTimelineSource
{
public:
    void attach(MonitorDspTimelinePanel* panel, MonitorCursorModel* sharedCursor);
    void apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
               bool stateStreamSubscribed = true);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot() const { return snapshot_; }
    MonitorDspTimelinePanel* panel() const { return panel_; }

private:
    void pushToPanel() const;

    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    bool stateStreamSubscribed_ = true;
    MonitorDspTimelinePanel* panel_ = nullptr;
    MonitorCursorModel* sharedCursor_ = nullptr;
};

} // namespace VNX
