#ifndef HARKEN_API_MONITOR_CAPTURE_HUB_H
#define HARKEN_API_MONITOR_CAPTURE_HUB_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include <cstdint>

namespace harken {

struct Engine;
struct MonitorExecutionFact;

namespace monitor {

// Declare before rollback/cleanup guards. Nested scopes publish only after the
// outermost logical operation has fully committed or rolled back.
class IdentityMutationScope {
public:
    IdentityMutationScope() noexcept;
    ~IdentityMutationScope();
    IdentityMutationScope(const IdentityMutationScope&) = delete;
    IdentityMutationScope& operator=(const IdentityMutationScope&) = delete;
private:
    std::uint64_t engine_ = 0;
};
void RefreshIdentityBoundary() noexcept;

VnResult OnEngineInit(Engine* engine);
void OnEngineTeardown(Engine* engine);
bool InitWouldExceedProcessCap();

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
bool EventCatalogEnabled();
bool ExecutionObservationEnabled() noexcept;
std::uint64_t ExecutionEngineInstance() noexcept;
bool PublishExecutionFact(MonitorExecutionFact fact, bool audioProducer = false) noexcept;
bool PublishEventDetail(const VnMonitorEventDetail& detail,
                         std::uint32_t entityKind, VnObjectId stableId,
                         VnObjectId projectId,
                         std::uint64_t worldGeneration,
                         std::uint64_t instanceHandle = 0,
                         std::uint64_t instanceGeneration = 0,
                         std::uint64_t programFingerprint = 0,
                         std::uint32_t logSource = 0, std::uint64_t logTicket = 0);
void ObserveOperation(std::uint32_t operation, VnResult result,
                       std::uint32_t entityKind, VnObjectId stableId,
                       std::uint64_t handle = 0, std::uint64_t arg0 = 0,
                       std::uint64_t arg1 = 0, float value = 0,
                       const char* text = nullptr);
void ObserveDiagnostic(std::uint32_t level, std::uint32_t code,
                        std::uint64_t arg0, std::uint64_t arg1,
                        std::uint64_t timestampNs, VnObjectId objectId, VnObjectId projectId,
                        const char* text, std::uint32_t logSource = 0,
                        std::uint64_t logTicket = 0);
void CompleteLogTranscription(std::uint32_t source, std::uint64_t consumed);
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
VnResult StartRecording(VnMonitorSessionHandle handle, uint64_t leaseGeneration,
                        const VnMonitorRecordingDesc* desc);
VnResult StopRecording(VnMonitorSessionHandle handle, uint64_t leaseGeneration);
VnResult GetRecordingStatus(VnMonitorSessionHandle handle,
                            VnMonitorRecordingStatus* outStatus);
void OnAudioQuantum();
bool TimeSeriesLiveArmed();
bool AudioLiveArmed();
void PublishTimeSeriesQuantum(std::uint32_t callbackCpuUs,
                              std::uint32_t drainCpuUs,
                              std::uint32_t voicesCpuUs,
                              std::uint32_t busesCpuUs,
                              std::uint32_t arenaHighWaterBytes);
void PublishMasterAudioChunk(const float* interleaved, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t layout,
                             std::uint32_t sampleRate,
                             std::uint32_t masterGeneration,
                             std::uint64_t sourceFrameBegin);
void PublishPlanarAudioChunk(std::uint32_t tapKind, std::uint64_t ownerHandle,
                             std::uint64_t ownerGeneration, std::uint32_t slotId,
                             float* const* planes, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t offset,
                             std::uint32_t layout, std::uint32_t sampleRate,
                             std::uint64_t sourceFrameBegin);
void NoteExactInstanceRetired(std::uint32_t tapKind, std::uint64_t ownerHandle,
                              std::uint64_t ownerGeneration);
bool PublishLogicPacked(const std::uint8_t* bytes, std::uint32_t size);
std::uint64_t NextLogicSourceSequence();
std::uint64_t CurrentExecuteFrame();
bool LogicThreadReady();
VnResult GateControlEntry();

} // namespace monitor
} // namespace harken

#endif
