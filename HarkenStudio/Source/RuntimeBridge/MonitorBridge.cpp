#include <harken/HarkenDevelopment.h>
#include "MonitorBridge.h"
#include "NativeMonitorValues.h"

#include <algorithm>
#include <vector>

namespace harken::studio::bridge::Monitoring {
namespace {

template<class Native, class Model, class Call>
int32_t readValue(Model* out, Call call)
{
    if (!out) return VN_INVALID_ARG;
    Native value{};
    value.structSize = sizeof(value);
    const auto result = call(&value);
    if (result == VN_OK) *out = fromNative(value);
    return result;
}

template<class Call>
int32_t submitPlan(const harken::studio::Monitor::MonitorCapturePlan* plan,
                    harken::studio::Monitor::MonitorEffectiveCapturePlan* out, Call call)
{
    if (!plan || !out || (plan->tapCount && !plan->taps) ||
        (plan->measurementCount && !plan->measurements)) return VN_INVALID_ARG;
    if (plan->tapCount > VN_MONITOR_HARD_TAPS_PER_PLAN ||
        plan->measurementCount > VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN) return VN_MONITOR_UNSUPPORTED;
    std::vector<VnMonitorTapDescriptor> taps;
    std::vector<VnMonitorMeasurementRequest> measurements;
    taps.reserve(plan->tapCount);
    measurements.reserve(plan->measurementCount);
    for (uint32_t i = 0; i < plan->tapCount; ++i) taps.push_back(toNative(plan->taps[i]));
    for (uint32_t i = 0; i < plan->measurementCount; ++i)
        measurements.push_back(toNative(plan->measurements[i]));
    auto native = toNative(*plan);
    native.taps = taps.empty() ? nullptr : taps.data();
    native.measurements = measurements.empty() ? nullptr : measurements.data();
    return readValue<VnMonitorEffectiveCapturePlan>(out, [&](auto* value) { return call(&native, value); });
}

}

int32_t openSession(const harken::studio::Monitor::MonitorSessionDesc* desc, uint64_t* handle)
{
    if (!desc) return VN_INVALID_ARG;
    const auto native = toNative(*desc);
    return Vn_MonitorOpenSession(reinterpret_cast<const VnMonitorSessionDesc*>(&native), handle);
}

int32_t openEventSession(const harken::studio::Monitor::MonitorSessionDesc* desc, uint64_t* handle)
{
    if (!desc) return VN_INVALID_ARG;
    const auto native = toNative(*desc);
    return Vn_MonitorOpenEventSession(reinterpret_cast<const VnMonitorSessionDesc*>(&native), handle);
}

int32_t closeSession(uint64_t handle) { return Vn_MonitorCloseSession(handle); }
int32_t sealEventSession(uint64_t handle) { return Vn_MonitorSealEventSession(handle); }

int32_t getEventSessionStatus(uint64_t handle, harken::studio::Monitor::MonitorEventSessionStatus* out)
{
    return readValue<VnMonitorEventSessionStatusV2>(out, [&](auto* value) {
        return Vn_MonitorGetEventSessionStatus(handle, reinterpret_cast<VnMonitorEventSessionStatus*>(value));
    });
}

int32_t getCapabilities(harken::studio::Monitor::MonitorCapabilities* out)
{
    return readValue<VnMonitorCapabilities>(out, Vn_MonitorGetCapabilities);
}

int32_t poll(uint64_t handle, const harken::studio::Monitor::MonitorPollDesc* desc,
             harken::studio::Monitor::MonitorPollResult* out)
{
    if (!desc || !out) return VN_INVALID_ARG;
    auto native = toNative(*desc);
    native.buffer = desc->buffer;
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    const auto status = Vn_MonitorPoll(handle, &native, &result);
    if (status == VN_OK || status == VN_MONITOR_BUFFER_TOO_SMALL) *out = fromNative(result);
    return status;
}

int32_t getSubscriptionStatus(uint64_t handle, harken::studio::Monitor::MonitorSubscriptionStatus* out)
{
    if (!out || (out->baselineCapacity && !out->baselines)) return VN_INVALID_ARG;
    std::vector<VnMonitorStreamBaseline> baselines(out->baselineCapacity);
    for (auto& entry : baselines) entry.structSize = sizeof(entry);
    VnMonitorSubscriptionStatus native{};
    native.structSize = sizeof(native);
    native.baselineCapacity = out->baselineCapacity;
    native.baselines = baselines.empty() ? nullptr : baselines.data();
    const auto result = Vn_MonitorGetSubscriptionStatus(handle, &native);
    if (result == VN_MONITOR_BUFFER_TOO_SMALL) {
        out->baselineCount = native.baselineCount;
    } else if (result == VN_OK) {
        auto* storage = out->baselines;
        const auto count = std::min(out->baselineCapacity, native.baselineCount);
        for (uint32_t i = 0; i < count; ++i) storage[i] = fromNative(baselines[i]);
        *out = fromNative(native);
        out->baselines = storage;
    }
    return result;
}

int32_t getConsumerStatus(uint64_t handle, harken::studio::Monitor::MonitorDropSummary* out,
                          uint32_t capacity, uint32_t* required)
{
    if (!required || (capacity && !out)) return VN_INVALID_ARG;
    std::vector<VnMonitorDropSummary> native(capacity);
    for (auto& entry : native) entry.structSize = sizeof(entry);
    const auto result = Vn_MonitorGetConsumerStatus(handle, native.empty() ? nullptr : native.data(), capacity, required);
    if (result == VN_OK)
        for (uint32_t i = 0; i < std::min(capacity, *required); ++i) out[i] = fromNative(native[i]);
    return result;
}

int32_t getCaptureControlStatus(uint64_t handle, harken::studio::Monitor::MonitorCaptureControlStatus* out)
{
    return readValue<VnMonitorCaptureControlStatus>(out, [&](auto* value) {
        return Vn_MonitorGetCaptureControlStatus(handle, value);
    });
}

int32_t acquireCaptureControl(uint64_t handle, harken::studio::Monitor::MonitorCaptureControlStatus* out)
{
    return readValue<VnMonitorCaptureControlStatus>(out, [&](auto* value) {
        return Vn_MonitorAcquireCaptureControl(handle, value);
    });
}

int32_t releaseCaptureControl(uint64_t handle, uint64_t lease)
{
    return Vn_MonitorReleaseCaptureControl(handle, lease);
}

int32_t validateCapturePlan(uint64_t handle, uint64_t lease,
                            const harken::studio::Monitor::MonitorCapturePlan* plan,
                            harken::studio::Monitor::MonitorEffectiveCapturePlan* out)
{
    return submitPlan(plan, out, [&](const auto* native, auto* value) {
        return Vn_MonitorValidateCapturePlan(handle, lease, native, value);
    });
}

int32_t applyCapturePlan(uint64_t handle, uint64_t lease,
                         const harken::studio::Monitor::MonitorCapturePlan* plan,
                         harken::studio::Monitor::MonitorEffectiveCapturePlan* out)
{
    return submitPlan(plan, out, [&](const auto* native, auto* value) {
        return Vn_MonitorApplyCapturePlan(handle, lease, native, value);
    });
}

int32_t stopCapture(uint64_t handle, uint64_t lease, harken::studio::Monitor::MonitorEffectiveCapturePlan* out)
{
    return readValue<VnMonitorEffectiveCapturePlan>(out, [&](auto* value) {
        return Vn_MonitorStopCapture(handle, lease, value);
    });
}

int32_t getEffectiveCapturePlan(uint64_t handle, uint64_t generation,
                                harken::studio::Monitor::MonitorEffectiveCapturePlan* out)
{
    return readValue<VnMonitorEffectiveCapturePlan>(out, [&](auto* value) {
        return Vn_MonitorGetEffectiveCapturePlan(handle, generation, value);
    });
}

}
