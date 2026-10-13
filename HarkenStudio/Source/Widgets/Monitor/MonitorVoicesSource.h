#pragma once

#include "Monitor/MonitorStore.h"

#include <memory>

namespace VNX {

class MonitorVoicesPanel;
class MonitorCursorModel;

class MonitorVoicesSource
{
public:
    void attach(MonitorVoicesPanel* panel, MonitorCursorModel* sharedCursor);
    void apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
               bool stateStreamSubscribed = true);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot() const { return snapshot_; }
    MonitorVoicesPanel* panel() const { return panel_; }

private:
    void pushToPanel() const;

    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    bool stateStreamSubscribed_ = true;
    MonitorVoicesPanel* panel_ = nullptr;
    MonitorCursorModel* sharedCursor_ = nullptr;
};

} // namespace VNX
