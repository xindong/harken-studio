#include "MonitorCaptureControlHost.h"
#include "Monitor/MonitorTypes.h"

#include <cstring>
#include <string>

namespace VNX {
namespace Monitor {

MonitorCaptureControlHost::MonitorCaptureControlHost(MonitorAdapter* adapter)
    : adapter_(adapter)
{
}

std::string MonitorCaptureControlHost::lastStatusText() const
{
    return captureControlStatusText(lastState_);
}

bool MonitorCaptureControlHost::tracesContain(const char* entry) const
{
    if (adapter_ == nullptr || entry == nullptr)
        return false;
    for (const auto& trace : adapter_->callTrace())
    {
        if (trace.entry != nullptr && std::string(trace.entry) == entry)
            return true;
    }
    return false;
}

MonitorRemoteStatusState MonitorCaptureControlHost::projectRemoteFromQueries()
{
    MonitorRemoteStatusInput input;
    input.queryResult = kMonitorUnsupported;
    lastRemoteState_ = projectRemoteStatus(input);
    return lastRemoteState_;
}

MonitorCaptureControlState MonitorCaptureControlHost::refresh()
{
    return openLayout();
}

MonitorCaptureControlState MonitorCaptureControlHost::openLayout()
{
    if (adapter_ == nullptr)
    {
        lastState_ = projectCaptureControlStatus(kNotInitialized, nullptr);
        projectRemoteFromQueries();
        return lastState_;
    }

    if (!adapter_->hasOpenSession() && boundHandle_ != 0)
    {
        markDisconnected();
        return lastState_;
    }

    if (!adapter_->hasOpenSession())
    {
        MonitorSessionDesc desc {};

        desc.requestedPermissionMask =
            kMonitorPermObserveState | kMonitorPermObserveEvents
            | kMonitorPermObserveMeasurements | kMonitorPermCaptureControl
            | kMonitorPermRecord;
        const MonitorOpenResult opened = adapter_->open(&desc);
        if (opened.result != kOk || opened.handle == 0)
        {
            lastState_ = projectCaptureControlStatus(opened.result, nullptr);
            projectRemoteFromQueries();
            return lastState_;
        }
        boundHandle_ = opened.handle;
        disconnecting_ = false;
        disconnected_ = false;
    }
    else if (boundHandle_ == 0)
    {
        boundHandle_ = adapter_->sessionHandle();
        disconnecting_ = false;
        disconnected_ = false;
    }

    MonitorCaptureControlStatus status {};

    const MonitorResult query = adapter_->getCaptureControlStatus(boundHandle_, &status);
    lastState_ = projectCaptureControlStatus(
        query,
        query == kOk ? &status : nullptr,
        adapter_->acquiredPrincipalHash(),
        adapter_->acquiredLeaseGeneration());
    if (query == kMonitorSequenceExhausted)
    {
        markDisconnected();
        return lastState_;
    }
    if (query == kMonitorStaleSession || query == kMonitorResyncRequired)
    {
        markDisconnecting();
        return lastState_;
    }

    boundHandle_ = adapter_->sessionHandle();
    projectRemoteFromQueries();
    return lastState_;
}

bool MonitorCaptureControlHost::hasOpenSession() const
{
    return adapter_ != nullptr && adapter_->hasOpenSession();
}

void MonitorCaptureControlHost::markDisconnecting()
{
    disconnecting_ = true;
    disconnected_ = false;
    lastState_ = {};
    projectRemoteFromQueries();
}

void MonitorCaptureControlHost::markDisconnected()
{
    disconnecting_ = false;
    disconnected_ = true;
    if (adapter_ != nullptr && adapter_->hasOpenSession())
        adapter_->close(adapter_->sessionHandle());
    boundHandle_ = 0;
    lastState_ = {};
    projectRemoteFromQueries();
}

MonitorResult MonitorCaptureControlHost::reconnect()
{
    if (adapter_ == nullptr)
        return kNotInitialized;
    if (adapter_->hasOpenSession())
        adapter_->close(adapter_->sessionHandle());
    boundHandle_ = 0;
    disconnecting_ = false;
    disconnected_ = false;
    openLayout();
    if (!adapter_->hasOpenSession())
        return lastState_.queryResult != kOk ? lastState_.queryResult
                                               : kMonitorUnsupported;
    return kOk;
}

void MonitorCaptureControlHost::closeSession()
{
    leaveLayout();
}

void MonitorCaptureControlHost::leaveLayout()
{
    boundHandle_ = 0;
    disconnecting_ = false;
    disconnected_ = false;
    if (adapter_ != nullptr && adapter_->hasOpenSession())
        adapter_->close(adapter_->sessionHandle());
}

} // namespace Monitor
} // namespace VNX
