#include "MonitorBridge.h"
#include "NativeMonitorValues.h"

#include <algorithm>
#include <vector>

namespace HarkenEditor::Bridge::Monitoring {
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
int32_t submitPlan(const VNX::Monitor::MonitorCapturePlan* plan,
                    VNX::Monitor::MonitorEffectiveCapturePlan* out, Call call)
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

int32_t openSession(const VNX::Monitor::MonitorSessionDesc* desc, uint64_t* handle)
{
    if (!desc) return VN_INVALID_ARG;
    const auto native = toNative(*desc);
    return Vn_MonitorOpenSession(&native, handle);
}

int32_t openEventSession(const VNX::Monitor::MonitorSessionDesc* desc, uint64_t* handle)
{
    if (!desc) return VN_INVALID_ARG;
    const auto native = toNative(*desc);
    return Vn_MonitorOpenEventSession(&native, handle);
}

int32_t closeSession(uint64_t handle) { return Vn_MonitorCloseSession(handle); }
int32_t sealEventSession(uint64_t handle) { return Vn_MonitorSealEventSession(handle); }

int32_t getEventSessionStatus(uint64_t handle, VNX::Monitor::MonitorEventSessionStatus* out)
{
    return readValue<VnMonitorEventSessionStatus>(out, [&](auto* value) {
        return Vn_MonitorGetEventSessionStatus(handle, value);
    });
}

int32_t getCapabilities(VNX::Monitor::MonitorCapabilities* out)
{
    return readValue<VnMonitorCapabilities>(out, Vn_MonitorGetCapabilities);
}

int32_t poll(uint64_t handle, const VNX::Monitor::MonitorPollDesc* desc,
             VNX::Monitor::MonitorPollResult* out)
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

int32_t getSubscriptionStatus(uint64_t handle, VNX::Monitor::MonitorSubscriptionStatus* out)
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

int32_t getConsumerStatus(uint64_t handle, VNX::Monitor::MonitorDropSummary* out,
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

int32_t getCaptureControlStatus(uint64_t handle, VNX::Monitor::MonitorCaptureControlStatus* out)
{
    return readValue<VnMonitorCaptureControlStatus>(out, [&](auto* value) {
        return Vn_MonitorGetCaptureControlStatus(handle, value);
    });
}

int32_t acquireCaptureControl(uint64_t handle, VNX::Monitor::MonitorCaptureControlStatus* out)
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
                            const VNX::Monitor::MonitorCapturePlan* plan,
                            VNX::Monitor::MonitorEffectiveCapturePlan* out)
{
    return submitPlan(plan, out, [&](const auto* native, auto* value) {
        return Vn_MonitorValidateCapturePlan(handle, lease, native, value);
    });
}

int32_t applyCapturePlan(uint64_t handle, uint64_t lease,
                         const VNX::Monitor::MonitorCapturePlan* plan,
                         VNX::Monitor::MonitorEffectiveCapturePlan* out)
{
    return submitPlan(plan, out, [&](const auto* native, auto* value) {
        return Vn_MonitorApplyCapturePlan(handle, lease, native, value);
    });
}

int32_t stopCapture(uint64_t handle, uint64_t lease, VNX::Monitor::MonitorEffectiveCapturePlan* out)
{
    return readValue<VnMonitorEffectiveCapturePlan>(out, [&](auto* value) {
        return Vn_MonitorStopCapture(handle, lease, value);
    });
}

int32_t getEffectiveCapturePlan(uint64_t handle, uint64_t generation,
                                VNX::Monitor::MonitorEffectiveCapturePlan* out)
{
    return readValue<VnMonitorEffectiveCapturePlan>(out, [&](auto* value) {
        return Vn_MonitorGetEffectiveCapturePlan(handle, generation, value);
    });
}

int32_t startRecording(uint64_t handle, uint64_t lease, const VNX::Monitor::MonitorRecordingDesc* desc)
{
    if (!desc) return VN_INVALID_ARG;
    auto native = toNative(*desc);
    native.utf8Path = desc->utf8Path;
    return Vn_MonitorStartRecording(handle, lease, &native);
}

int32_t stopRecording(uint64_t handle, uint64_t lease) { return Vn_MonitorStopRecording(handle, lease); }

int32_t getRecordingStatus(uint64_t handle, VNX::Monitor::MonitorRecordingStatus* out)
{
    return readValue<VnMonitorRecordingStatus>(out, [&](auto* value) {
        return Vn_MonitorGetRecordingStatus(handle, value);
    });
}

int32_t compareSessions(const VNX::Monitor::MonitorSessionCompareDesc* desc,
                        VNX::Monitor::MonitorCompareResult* out)
{
    if (!desc || !desc->left || !desc->right || !out) return VN_INVALID_ARG;
    const auto left = toNative(*desc->left);
    const auto right = toNative(*desc->right);
    VnMonitorSessionCompareDesc native{};
    native.structSize = sizeof(native);
    native.left = &left;
    native.right = &right;
    native.executeFrameBegin = desc->executeFrameBegin;
    native.executeFrameEnd = desc->executeFrameEnd;
    return readValue<VnMonitorCompareResult>(out, [&](auto* value) {
        return Vn_MonitorCompareSessions(&native, value);
    });
}

}
