#ifndef HARKEN_API_MONITOR_CAPTURE_HUB_H
#define HARKEN_API_MONITOR_CAPTURE_HUB_H

#include <harken/HarkenDevelopment.h>
#include "MonitorSeam.h"

#include <cstdint>

namespace harken {

namespace monitor {

void RefreshIdentityBoundary() noexcept;

VnResult GetCapabilities(VnMonitorCapabilities* outCapabilities);
VnResult CaptureRegistry(uint32_t kind, void* out, uint32_t capacityBytes,
                         uint32_t* requiredBytes, uint64_t* revision);
VnResult OpenSession(const VnMonitorSessionDesc* desc,
                     VnMonitorSessionHandle* outHandle);
VnResult OpenRemoteSession(const VnMonitorSessionDesc* desc,
                           std::uint32_t permissionMask,
                           std::uint64_t principalHash,
                           VnMonitorSessionHandle* outHandle);
VnResult CloseSession(VnMonitorSessionHandle handle);
VnResult OpenEventSession(const VnMonitorSessionDesc* desc,
                          VnMonitorSessionHandle* outHandle,
                          bool remote = false, std::uint64_t principalHash = 0,
                          std::uint32_t remoteGrant = VN_MONITOR_PERM_OBSERVE_EVENTS);
VnResult SealEventSession(VnMonitorSessionHandle handle);
VnResult GetEventSessionStatus(VnMonitorSessionHandle handle,
                               VnMonitorEventSessionStatus* outStatus);
bool PublishEventDetail(const VnMonitorEventDetail& detail,
                         std::uint32_t entityKind, VnObjectId stableId,
                         VnObjectId projectId,
                         std::uint64_t worldGeneration,
                         std::uint64_t instanceHandle = 0,
                         std::uint64_t instanceGeneration = 0,
                         std::uint64_t programFingerprint = 0,
                         std::uint32_t logSource = 0, std::uint64_t logTicket = 0);
VnResult GetSubscriptionStatus(VnMonitorSessionHandle handle,
                               VnMonitorSubscriptionStatus* outStatus);
VnResult Poll(VnMonitorSessionHandle handle, const VnMonitorPollDesc* desc,
              VnMonitorPollResult* outResult);
VnResult GetConsumerStatus(VnMonitorSessionHandle handle,
                           VnMonitorDropSummary* outSummaries,
                           uint32_t capacity, uint32_t* requiredCount);
VnResult RequestStateCut(VnMonitorSessionHandle handle,
                         const VnMonitorStateCutDesc* desc,
                         uint64_t* outStateCutId);
VnResult GetStateCutStatus(VnMonitorSessionHandle handle, uint64_t stateCutId,
                           VnMonitorStateCutStatus* outStatus);
VnResult AcquireCaptureControl(VnMonitorSessionHandle handle,
                               VnMonitorCaptureControlStatus* outStatus);
VnResult ReleaseCaptureControl(VnMonitorSessionHandle handle,
                               uint64_t leaseGeneration);
VnResult GetCaptureControlStatus(VnMonitorSessionHandle handle,
                                 VnMonitorCaptureControlStatus* outStatus);
VnResult ValidateCapturePlan(VnMonitorSessionHandle handle,
                             uint64_t leaseGeneration,
                             const VnMonitorCapturePlan* plan,
                             VnMonitorEffectiveCapturePlan* outAccepted);
VnResult ApplyCapturePlan(VnMonitorSessionHandle handle,
                          uint64_t leaseGeneration,
                          const VnMonitorCapturePlan* plan,
                          VnMonitorEffectiveCapturePlan* outAccepted);
VnResult StopCapture(VnMonitorSessionHandle handle, uint64_t leaseGeneration,
                     VnMonitorEffectiveCapturePlan* outResult);
VnResult GetEffectiveCapturePlan(VnMonitorSessionHandle handle,
                                 uint64_t capturePlanGeneration,
                                 VnMonitorEffectiveCapturePlan* outPlan);
bool PublishLogicPacked(const std::uint8_t* bytes, std::uint32_t size);
std::uint64_t NextLogicSourceSequence();
std::uint64_t CurrentExecuteFrame();

} // namespace monitor
} // namespace harken

#endif
