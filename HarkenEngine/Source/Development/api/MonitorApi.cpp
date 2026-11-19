#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>

#include "Engine.h"
#include "MonitorCaptureHub.h"
#include "HarkenApiInternal.h"
#include "harken/core/MonitorVocabulary.h"

static_assert(VN_MONITOR_CONTENT_ACTION == harken::monitor::kContentAction);
static_assert(VN_MONITOR_CONTENT_MEDIA == harken::monitor::kContentMedia);
static_assert(VN_MONITOR_CONTENT_SOUND == harken::monitor::kContentSound);
static_assert(VN_MONITOR_ENTITY_ACTION == harken::monitor::kEntityAction);
static_assert(VN_MONITOR_ENTITY_NONE == harken::monitor::kEntityNone);
static_assert(VN_MONITOR_ENTITY_PLAYABLE == harken::monitor::kEntityPlayable);
static_assert(VN_MONITOR_ENTITY_VOICE == harken::monitor::kEntityVoice);
static_assert(VN_MONITOR_OPERATION_LOAD_PACK == harken::monitor::kOperationLoadPack);
static_assert(VN_MONITOR_OPERATION_PLAY_PREVIEW == harken::monitor::kOperationPlayPreview);
static_assert(VN_MONITOR_OPERATION_POST_ACTION == harken::monitor::kOperationPostAction);
static_assert(VN_MONITOR_OPERATION_RESET_VALUES == harken::monitor::kOperationResetValues);
static_assert(VN_MONITOR_OPERATION_SET_EMITTER_VALUE == harken::monitor::kOperationSetEmitterValue);
static_assert(VN_MONITOR_OPERATION_SET_EMITTER_VALUES == harken::monitor::kOperationSetEmitterValues);
static_assert(VN_MONITOR_OPERATION_SET_VALUE == harken::monitor::kOperationSetValue);
static_assert(VN_MONITOR_OPERATION_SET_VALUES == harken::monitor::kOperationSetValues);
static_assert(VN_MONITOR_OPERATION_STOP_ACTION == harken::monitor::kOperationStopAction);
static_assert(VN_MONITOR_OPERATION_STOP_ALL == harken::monitor::kOperationStopAll);
static_assert(VN_MONITOR_OPERATION_STOP_EMITTER == harken::monitor::kOperationStopEmitter);
static_assert(VN_MONITOR_OPERATION_STOP_PREVIEW == harken::monitor::kOperationStopPreview);
static_assert(VN_MONITOR_OPERATION_STOP_VOICE == harken::monitor::kOperationStopVoice);
static_assert(VN_MONITOR_OPERATION_UNLOAD_PACK == harken::monitor::kOperationUnloadPack);
static_assert(VN_MONITOR_PLAYABLE_PROVENANCE_KNOWN == harken::monitor::kPlayableProvenanceKnown);
static_assert(VN_MONITOR_SUBJECT_ACTION == harken::monitor::kSubjectAction);
static_assert(VN_MONITOR_SUBJECT_LOGICAL_VOICE == harken::monitor::kSubjectLogicalVoice);
static_assert(VN_MONITOR_SUBJECT_PLAYABLE == harken::monitor::kSubjectPlayable);
static_assert(VN_MONITOR_TARGET_ACTION == harken::monitor::kTargetAction);
static_assert(VN_MONITOR_TARGET_EMITTER == harken::monitor::kTargetEmitter);
static_assert(VN_MONITOR_TARGET_LOGICAL_VOICE == harken::monitor::kTargetLogicalVoice);

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
    return harken::monitor::Poll(handle, desc, outResult);
}

VnResult Vn_MonitorGetConsumerStatus(VnMonitorSessionHandle handle,
                                     VnMonitorDropSummary* outSummaries,
                                     uint32_t capacity,
                                     uint32_t* requiredCount) {
    return harken::monitor::GetConsumerStatus(handle, outSummaries, capacity,
                                             requiredCount);
}

} // extern "C"
