#include <harken/Harken.h>

#include "Engine.h"
#include "MonitorCaptureHub.h"
#include "MonitorSessionCompare.h"
#include "HarkenApiInternal.h"

namespace {

VnResult MonitorEngineGate() {
    return harken::detail::ActiveEngine() == nullptr ? VN_NOT_INITIALIZED
                                                    : VN_OK;
}

} // namespace

extern "C" {

VnResult Vn_MonitorGetCapabilities(VnMonitorCapabilities* outCapabilities) {
    return harken::monitor::GetCapabilities(outCapabilities);
}

VnResult Vn_MonitorOpenSession(const VnMonitorSessionDesc* desc,
                               VnMonitorSessionHandle* outHandle) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::OpenSession(desc, outHandle);
}

VnResult Vn_MonitorCloseSession(VnMonitorSessionHandle handle) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::CloseSession(handle);
}

VnResult Vn_MonitorOpenEventSession(const VnMonitorSessionDesc* desc,
                                    VnMonitorSessionHandle* outHandle) {
    return harken::monitor::OpenEventSession(desc, outHandle);
}

VnResult Vn_MonitorSealEventSession(VnMonitorSessionHandle handle) {
    return harken::monitor::SealEventSession(handle);
}

VnResult Vn_MonitorGetEventSessionStatus(
    VnMonitorSessionHandle handle, VnMonitorEventSessionStatus* outStatus) {
    return harken::monitor::GetEventSessionStatus(handle, outStatus);
}

VnResult Vn_MonitorGetSubscriptionStatus(
    VnMonitorSessionHandle handle,
    VnMonitorSubscriptionStatus* outStatus) {
    return harken::monitor::GetSubscriptionStatus(handle, outStatus);
}

VnResult Vn_MonitorCaptureRegistry(uint32_t kind, void* out,
                                   uint32_t capacityBytes,
                                   uint32_t* requiredBytes,
                                   uint64_t* revision) {
    return harken::monitor::CaptureRegistry(kind, out, capacityBytes,
                                           requiredBytes, revision);
}

VnResult Vn_MonitorRequestStateCut(VnMonitorSessionHandle handle,
                                   const VnMonitorStateCutDesc* desc,
                                   uint64_t* outStateCutId) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::RequestStateCut(handle, desc, outStateCutId);
}

VnResult Vn_MonitorGetStateCutStatus(VnMonitorSessionHandle handle,
                                     uint64_t stateCutId,
                                     VnMonitorStateCutStatus* outStatus) {
    return harken::monitor::GetStateCutStatus(handle, stateCutId, outStatus);
}

VnResult Vn_MonitorAcquireCaptureControl(
    VnMonitorSessionHandle handle,
    VnMonitorCaptureControlStatus* outStatus) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::AcquireCaptureControl(handle, outStatus);
}

VnResult Vn_MonitorReleaseCaptureControl(VnMonitorSessionHandle handle,
                                         uint64_t leaseGeneration) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::ReleaseCaptureControl(handle, leaseGeneration);
}

VnResult Vn_MonitorGetCaptureControlStatus(
    VnMonitorSessionHandle handle,
    VnMonitorCaptureControlStatus* outStatus) {
    return harken::monitor::GetCaptureControlStatus(handle, outStatus);
}

VnResult Vn_MonitorValidateCapturePlan(
    VnMonitorSessionHandle handle,
    uint64_t leaseGeneration,
    const VnMonitorCapturePlan* plan,
    VnMonitorEffectiveCapturePlan* outAccepted) {
    return harken::monitor::ValidateCapturePlan(handle, leaseGeneration, plan,
                                               outAccepted);
}

VnResult Vn_MonitorApplyCapturePlan(
    VnMonitorSessionHandle handle,
    uint64_t leaseGeneration,
    const VnMonitorCapturePlan* plan,
    VnMonitorEffectiveCapturePlan* outAccepted) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::ApplyCapturePlan(handle, leaseGeneration, plan,
                                            outAccepted);
}

VnResult Vn_MonitorStopCapture(VnMonitorSessionHandle handle,
                               uint64_t leaseGeneration,
                               VnMonitorEffectiveCapturePlan* outResult) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::StopCapture(handle, leaseGeneration, outResult);
}

VnResult Vn_MonitorGetEffectiveCapturePlan(
    VnMonitorSessionHandle handle,
    uint64_t capturePlanGeneration,
    VnMonitorEffectiveCapturePlan* outPlan) {
    return harken::monitor::GetEffectiveCapturePlan(handle, capturePlanGeneration,
                                                   outPlan);
}

VnResult Vn_MonitorPoll(VnMonitorSessionHandle handle,
                        const VnMonitorPollDesc* desc,
                        VnMonitorPollResult* outResult) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    return harken::monitor::Poll(handle, desc, outResult);
}

VnResult Vn_MonitorGetConsumerStatus(VnMonitorSessionHandle handle,
                                     VnMonitorDropSummary* outSummaries,
                                     uint32_t capacity,
                                     uint32_t* requiredCount) {
    return harken::monitor::GetConsumerStatus(handle, outSummaries, capacity,
                                             requiredCount);
}

VnResult Vn_MonitorStartRecording(VnMonitorSessionHandle handle,
                                  uint64_t leaseGeneration,
                                  const VnMonitorRecordingDesc* desc) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    const VnResult gate = MonitorEngineGate();
    if (gate != VN_OK) {
        return gate;
    }
    return harken::monitor::StartRecording(handle, leaseGeneration, desc);
}

VnResult Vn_MonitorStopRecording(VnMonitorSessionHandle handle,
                                 uint64_t leaseGeneration) {
    if (harken::detail::CanonicalSetMutationBlocked()) { return VN_IN_USE; }

    const VnResult gate = MonitorEngineGate();
    if (gate != VN_OK) {
        return gate;
    }
    return harken::monitor::StopRecording(handle, leaseGeneration);
}

VnResult Vn_MonitorGetRecordingStatus(VnMonitorSessionHandle handle,
                                      VnMonitorRecordingStatus* outStatus) {
    const VnResult gate = MonitorEngineGate();
    if (gate != VN_OK) {
        return gate;
    }
    return harken::monitor::GetRecordingStatus(handle, outStatus);
}

VnResult Vn_MonitorCompareSessions(const VnMonitorSessionCompareDesc* desc,
                                   VnMonitorCompareResult* out) {
    return harken::monitor::CompareSessions(desc, out);
}

} // extern "C"
