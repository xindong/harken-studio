#pragma once

#include "Monitor/MonitorStore.h"

#include <memory>

namespace VNX {

class MonitorLineagePanel;
class MonitorCursorModel;

class MonitorLineageSource
{
public:
    void attach(MonitorLineagePanel* panel, MonitorCursorModel* sharedCursor);
    void apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
               bool stateStreamSubscribed = true);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot() const { return snapshot_; }
    MonitorLineagePanel* panel() const { return panel_; }

private:
    void pushToPanel() const;

    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    bool stateStreamSubscribed_ = true;
    MonitorLineagePanel* panel_ = nullptr;
    MonitorCursorModel* sharedCursor_ = nullptr;
};

} // namespace VNX
