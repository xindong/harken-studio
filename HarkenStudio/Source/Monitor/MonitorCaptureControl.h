#pragma once

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VNX {
namespace Monitor {

enum class MonitorLeaseProjection
{
    NoLease = 0,
    LeaseHeldByUnknown,
    LeaseHeldByThisSession,
    Unavailable,
};

struct MonitorCaptureControlState
{
    MonitorLeaseProjection projection = MonitorLeaseProjection::Unavailable;
    MonitorResult queryResult = kNotInitialized;
    uint32_t held = 0;
    uint64_t leaseGeneration = 0;
    uint64_t principalHash = 0;
    uint32_t lastChangeReason = 0;
    uint32_t permissions = 0;
};

struct MonitorMeasurementIntent
{
    std::vector<uint64_t> measurementIds;
    uint32_t subscriptionMask = 0;
};

void zeroCaptureControlStatus(MonitorCaptureControlStatus* outStatus);

MonitorCaptureControlState projectCaptureControlStatus(
    MonitorResult queryResult,
    const MonitorCaptureControlStatus* status,
    uint64_t thisSessionPrincipalHash = 0,
    uint64_t thisSessionLeaseGeneration = 0);

bool canApply(const MonitorCaptureControlState& state);
bool intentAllowed(const MonitorCaptureControlState& state);

// ModelOnly helper. Panel visibility is not wired in this pack; a null
// measurement id must copy the intent unchanged, including subscriptionMask.
MonitorMeasurementIntent hidePanelMeasurement(
    const MonitorMeasurementIntent& current,
    std::optional<uint64_t> measurementId);

std::string captureControlStatusText(const MonitorCaptureControlState& state);

} // namespace Monitor
} // namespace VNX
