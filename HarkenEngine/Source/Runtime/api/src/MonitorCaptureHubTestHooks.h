#ifndef HARKEN_API_MONITOR_CAPTURE_HUB_TEST_HOOKS_H
#define HARKEN_API_MONITOR_CAPTURE_HUB_TEST_HOOKS_H

#include <harken/Harken.h>

#include <cstdint>
#include <string>

namespace harken {
namespace monitor {

void SetPollStallForTests(bool stall);
std::uint32_t PollEnteredForTests();
void ResetPollEnteredForTests();

bool PublishSyntheticTimeSeriesForTests(std::uint32_t count);
void DrainOnceForTests();
void DrainRoundForTests();
void PumpRecorderForTests();
VnMonitorSessionHandle RecordingReceiverForTests();
void SetRecordingQueueBudgetForTests(std::uint64_t bytes);
void DrainOneProducerForTests(bool audioProducer);
void DrainOneIdentityForTests();
void DrainOneTypedForTests();
std::uint64_t SymbolPublishedForTests();
std::uint32_t SymbolRingCapacityForTests();
std::uint64_t DeferredSymbolBytesForTests(VnMonitorSessionHandle handle);
std::uint64_t ConsumerUsedBytesForTests(VnMonitorSessionHandle handle);
void ExhaustSymbolSourcesForTests();
void ExhaustObservationRevisionForTests();
void DropIdentityRowForTests(std::int32_t row);
std::uint64_t IdentityRefreshesForTests();
std::uint32_t IdentityRingCapacityForTests();
void ExhaustIdentitySourcesForTests(bool baseline);
std::uint32_t InternalQueuedCountForTests(std::uint32_t index);
VnMonitorDropSummary InternalDropForTests(std::uint32_t index);
void SetFailInitAfterThreadForTests(bool fail);
void SetRecorderWriteFailureForTests(bool fail);
void SetBeforeRecorderFailureCleanupForTests(void (*callback)(void*), void* context);
void SetAfterRecordingReceiverOpenForTests(void (*callback)(void*), void* context);
void SetRecorderOptionalWriteFailureForTests(bool fail);
void SetRecorderOptionalRestoreFailureForTests(bool fail);
std::uint32_t PublishMutexHitsForTests();
void SetDrainHoldForTests(bool hold);
std::uint32_t DrainEnteredForTests();
void ResetDrainEnteredForTests();
void SetFanoutHoldForTests(bool hold);
std::uint32_t FanoutEnteredForTests();
void ResetFanoutEnteredForTests();
std::uint32_t PackedTimeSeriesBytesForTests();
bool PublishConformancePackedForTests(const std::uint8_t* bytes,
                                      std::uint32_t size);
VnMonitorDropSummary ProducerDropForTests();
std::uint32_t ProducerRingCapacityForTests();
void StopDrainForTests();
void StartDrainForTests();
bool DrainRunningForTests();
bool CaptureResourcesAllocatedForTests();
void SetFailCaptureResourcesForTests(bool fail);
bool RecorderConsumerEnabledForTests();
void EnableInternalConsumersForTests(std::uint32_t mask);
VnResult OpenMonitorEndpointSessionForTests(const VnMonitorSessionDesc* desc,
                                           VnMonitorSessionHandle* outHandle);
void SetHostPrincipalForTests(bool authenticated, std::uint32_t observeMask);
void ClearHostPrincipalForTests();
bool PublishStateCutBeginForTests();
bool PublishStateCutEndForTests();
void SetDropStateCutEndForTests(bool drop);
bool PublishEpochBoundaryForTests();
void AdvanceExecuteFrameForTests(std::uint64_t frames);
void SetLeaseExpiryExecuteFrameForTests(std::uint64_t frame);
void HoldPlanAckForTests(bool hold);
void SetAbandonedPlanWorkerForTests();
void DropNextActivationRecordForTests(bool drop);
std::uint64_t ExecuteFrameForTests();
void ResetLifecycleDedupeForTests();
void SetCurrentWorldGenerationForTests(std::uint64_t world);
std::uint32_t LogicRingCapacityForTests();
bool FillLogicRingForTests();
bool FillLogicRingLeavingFreeSlotsForTests(std::uint32_t freeSlots);
std::uint32_t LogicRingFreeSlotsForTests();
void ObserveDspGapForTests();
void FillProducerRingForTests();
void PublishPlanarAudioChunk(std::uint32_t tapKind, std::uint64_t ownerHandle,
                             std::uint64_t ownerGeneration, std::uint32_t slotId,
                             float* const* planes, std::uint32_t frames,
                             std::uint32_t channels, std::uint32_t offset,
                             std::uint32_t layout, std::uint32_t sampleRate,
                             std::uint64_t sourceFrameBegin);
void NoteExactInstanceRetired(std::uint32_t tapKind, std::uint64_t ownerHandle,
                              std::uint64_t ownerGeneration);
VnResult EnableMonitorEndpointForTests(std::uint32_t profile);
void DisableMonitorEndpointForTests();
std::string MonitorEndpointNameForTests();
int MonitorEndpointPortForTests();
VnResult ConnectMonitorEndpointForTests();
void DisconnectMonitorEndpointForTests();
std::uint64_t MonitorEndpointEpochForTests();
VnResult SendMonitorOpcodeForTests(std::uint16_t opcode);
VnResult SendMonitorOpcodeWithEpochForTests(std::uint16_t opcode, std::uint64_t epoch);
VnResult SendMonitorPollWithEpochForTests(std::uint64_t epoch);
VnResult SendMonitorLeaseWithEpochForTests(std::uint64_t epoch);
bool MonitorEndpointRejectsRemoteClientsForTests();
std::uint64_t MonitorEndpointLocalAclSidHashForTests();
std::uint32_t MonitorEndpointGrantedPermissionMaskForTests();
VnResult OfferSelfReportedPrincipalForTests(std::uint64_t claimedSidHash,
                                            std::uint32_t claimedPermissionMask);
VnResult FailMonitorHandshakeForTests();
VnResult GrantMonitorPermissionsForTests(std::uint32_t mask);
bool MonitorEndpointEmitsRawAudioForTests();

} // namespace monitor
} // namespace harken

#endif
