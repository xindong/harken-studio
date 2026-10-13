#pragma once

#include "MonitorAdapter.h"
#include "Monitor/MonitorTypes.h"
#include "MonitorCaptureControl.h"
#include "MonitorRemoteStatus.h"

#include <string>

namespace VNX {
namespace Monitor {

// Studio-owned query host for P3-7. Opens a session only to read
// capture-control status. Never Acquire / Release / Validate / Apply / Stop.
class MonitorCaptureControlHost
{
public:
    explicit MonitorCaptureControlHost(MonitorAdapter* adapter);

    MonitorCaptureControlState refresh();
    MonitorCaptureControlState openLayout();
    MonitorRemoteStatusState lastRemoteState() const { return lastRemoteState_; }
    bool hasOpenSession() const;
    void markDisconnecting();
    void markDisconnected();
    MonitorResult reconnect();
    void closeSession();
    void leaveLayout();

    const MonitorCaptureControlState& lastState() const { return lastState_; }
    std::string lastStatusText() const;
    bool tracesContain(const char* entry) const;

private:
    MonitorRemoteStatusState projectRemoteFromQueries();

    MonitorAdapter* adapter_ = nullptr;
    MonitorSessionHandle boundHandle_ = 0;
    MonitorCaptureControlState lastState_;
    MonitorRemoteStatusState lastRemoteState_;
    bool disconnecting_ = false;
    bool disconnected_ = false;
};

} // namespace Monitor
} // namespace VNX
