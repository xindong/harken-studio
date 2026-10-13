#include "MonitorCaptureControl.h"
#include "Monitor/MonitorTypes.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace VNX {
namespace Monitor {

void zeroCaptureControlStatus(MonitorCaptureControlStatus* outStatus)
{
    if (outStatus == nullptr)
        return;

    std::memset(outStatus, 0, sizeof(*outStatus));

}

MonitorCaptureControlState projectCaptureControlStatus(
    MonitorResult queryResult,
    const MonitorCaptureControlStatus* status,
    uint64_t thisSessionPrincipalHash,
    uint64_t thisSessionLeaseGeneration)
{
    MonitorCaptureControlState state;
    state.queryResult = queryResult;
    if (queryResult != kOk || status == nullptr)
    {
        state.projection = MonitorLeaseProjection::Unavailable;
        return state;
    }

    state.held = status->held;
    state.leaseGeneration = status->leaseGeneration;
    state.principalHash = status->principalHash;
    state.lastChangeReason = status->lastChangeReason;
    state.permissions = status->permissions;
    if (status->held == 0)
        state.projection = MonitorLeaseProjection::NoLease;
    else if (thisSessionPrincipalHash != 0
             && thisSessionLeaseGeneration != 0
             && status->principalHash == thisSessionPrincipalHash
             && status->leaseGeneration == thisSessionLeaseGeneration)
        state.projection = MonitorLeaseProjection::LeaseHeldByThisSession;
    else
        state.projection = MonitorLeaseProjection::LeaseHeldByUnknown;
    return state;
}

bool canApply(const MonitorCaptureControlState& state)
{
    return state.projection == MonitorLeaseProjection::LeaseHeldByThisSession;
}

bool intentAllowed(const MonitorCaptureControlState& state)
{
    return canApply(state);
}

MonitorMeasurementIntent hidePanelMeasurement(
    const MonitorMeasurementIntent& current,
    std::optional<uint64_t> measurementId)
{
    MonitorMeasurementIntent next = current;
    if (!measurementId.has_value())
        return next;
    next.measurementIds.erase(
        std::remove(next.measurementIds.begin(), next.measurementIds.end(), *measurementId),
        next.measurementIds.end());
    return next;
}

std::string captureControlStatusText(const MonitorCaptureControlState& state)
{
    switch (state.projection)
    {
    case MonitorLeaseProjection::NoLease:
        return "NoLease";
    case MonitorLeaseProjection::LeaseHeldByUnknown:
        return "LeaseHeldByUnknown gen=" + std::to_string(state.leaseGeneration)
            + " hash=" + std::to_string(state.principalHash);
    case MonitorLeaseProjection::LeaseHeldByThisSession:
        return "LeaseHeld gen=" + std::to_string(state.leaseGeneration);
    case MonitorLeaseProjection::Unavailable:
        switch (state.queryResult)
        {
        case kMonitorStaleLease:
            return "Unavailable: StaleLease";
        case kMonitorPermissionDenied:
            return "Unavailable: PermissionDenied";
        case kMonitorStaleSession:
            return "Unavailable: StaleSession";
        case kNotInitialized:
            return "Unavailable: NotInitialized";
        case kMonitorWrongThread:
            return "Unavailable: WrongThread";
        default:
            return "Unavailable";
        }
    }
    return "Unavailable";
}

} // namespace Monitor
} // namespace VNX
