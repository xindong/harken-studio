#pragma once

#include "Monitor/MonitorStore.h"

#include <memory>

namespace VNX {

class MonitorSpectrumPanel;
class MonitorCursorModel;

// Host-owned empty-dock shell. apply() forwards the store snapshot for
// generation display only. It is not a Spectrum decoder or FFT channel.
class MonitorSpectrumSource
{
public:
    void attach(MonitorSpectrumPanel* panel, MonitorCursorModel* sharedCursor);
    void apply(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
               bool stateStreamSubscribed = true);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot() const { return snapshot_; }
    MonitorSpectrumPanel* panel() const { return panel_; }

private:
    void pushToPanel() const;

    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    bool stateStreamSubscribed_ = true;
    MonitorSpectrumPanel* panel_ = nullptr;
    MonitorCursorModel* sharedCursor_ = nullptr;
};

} // namespace VNX
