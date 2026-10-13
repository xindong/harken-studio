#pragma once

#include "Monitor/MonitorStore.h"

#include <memory>

namespace VNX {

class MonitorPlayablesPanel;
class MonitorCursorModel;

class MonitorPlayablesSource
{
public:
    void attach(MonitorPlayablesPanel* panel, MonitorCursorModel* sharedCursor);
    void apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
               bool stateStreamSubscribed = true);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot() const { return snapshot_; }
    MonitorPlayablesPanel* panel() const { return panel_; }

private:
    void pushToPanel() const;

    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    bool stateStreamSubscribed_ = true;
    MonitorPlayablesPanel* panel_ = nullptr;
    MonitorCursorModel* sharedCursor_ = nullptr;
};

} // namespace VNX
