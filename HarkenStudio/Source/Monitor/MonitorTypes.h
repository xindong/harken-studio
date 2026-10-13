#pragma once

#include <harken_studio/ObjectId.h>
#include <cstdint>

namespace VNX::Monitor {

using MonitorResult = std::int32_t;
inline constexpr auto kBackpressure = -301;
inline constexpr auto kContentNotLoaded = -220;
inline constexpr auto kInvalidArg = -100;
inline constexpr auto kInUse = -302;
inline constexpr auto kLogError = 2;
inline constexpr auto kLogInfo = 0;
inline constexpr auto kLogWarn = 1;
inline constexpr auto kMonitorBudgetExceeded = -343;
inline constexpr auto kMonitorChunkDropSummaries = 11;
inline constexpr auto kMonitorChunkEvents = 7;
inline constexpr auto kMonitorChunkOptional = 2;
inline constexpr auto kMonitorChunkOptionalAudio = 9;
inline constexpr auto kMonitorChunkOptionalSymbols = 5;
inline constexpr auto kMonitorChunkRecoveryIndex = 13;
inline constexpr auto kMonitorChunkRequired = 1;
inline constexpr auto kMonitorChunkSessionHeader = 1;
inline constexpr auto kMonitorChunkState = 6;
inline constexpr auto kMonitorChunkTimeSeries = 8;
inline constexpr auto kMonitorChunkTraceCorrelation = 10;
inline constexpr auto kMonitorCompareComparable = 0;
inline constexpr auto kMonitorCompareIncomparable = 2;
inline constexpr auto kMonitorCompareMissingData = 3;
inline constexpr auto kMonitorComparePartiallyComparable = 1;
inline constexpr auto kMonitorConcurrentPoll = -332;
inline constexpr auto kMonitorDropDerivedInput = 5;
inline constexpr auto kMonitorDropHubFanout = 2;
inline constexpr auto kMonitorDropRecorderQueue = 3;
inline constexpr auto kMonitorDropRemoteTransport = 4;
inline constexpr auto kMonitorDropSourceProducer = 1;
inline constexpr auto kMonitorEntityAction = 3;
inline constexpr auto kMonitorEntityBus = 5;
inline constexpr auto kMonitorEntityContainer = 7;
inline constexpr auto kMonitorEntityEffect = 6;
inline constexpr auto kMonitorEntityNone = 0;
inline constexpr auto kMonitorEntityPlayable = 2;
inline constexpr auto kMonitorEntityQueue = 8;
inline constexpr auto kMonitorEntitySession = 1;
inline constexpr auto kMonitorEntityTap = 10;
inline constexpr auto kMonitorEntityTransition = 9;
inline constexpr auto kMonitorEntityVoice = 4;
inline constexpr auto kMonitorEventAccept = 2;
inline constexpr auto kMonitorEventCategoryDiagnostic = 3;
inline constexpr auto kMonitorEventCategoryLifecycle = 2;
inline constexpr auto kMonitorEventCategoryOperation = 1;
inline constexpr auto kMonitorEventDrop = 13;
inline constexpr auto kMonitorEventPlan = 12;
inline constexpr auto kMonitorEventQueue = 8;
inline constexpr auto kMonitorEventReject = 3;
inline constexpr auto kMonitorEventRequest = 1;
inline constexpr auto kMonitorEventResource = 11;
inline constexpr auto kMonitorEventResult = 6;
inline constexpr auto kMonitorEventSelect = 7;
inline constexpr auto kMonitorEventSessionDrained = 2;
inline constexpr auto kMonitorEventStart = 4;
inline constexpr auto kMonitorEventStop = 5;
inline constexpr auto kMonitorEventTransition = 9;
inline constexpr auto kMonitorEventVoice = 10;
inline constexpr auto kMonitorHardChunkPayloadBytes = 268435456ull;
inline constexpr auto kMonitorHardSessionFileBytes = 68719476736ull;
inline constexpr auto kMonitorIncompatible = -344;
inline constexpr auto kMonitorLifecycleActionAccept = 6;
inline constexpr auto kMonitorLifecycleActionReject = 9;
inline constexpr auto kMonitorLifecycleActionResult = 8;
inline constexpr auto kMonitorLifecycleActionStop = 7;
inline constexpr auto kMonitorLifecycleEffectInstalled = 19;
inline constexpr auto kMonitorLifecycleEffectReleased = 20;
inline constexpr auto kMonitorLifecyclePlayableAudibleRetiring = 3;
inline constexpr auto kMonitorLifecyclePlayableCancel = 5;
inline constexpr auto kMonitorLifecyclePlayableLogicalComplete = 2;
inline constexpr auto kMonitorLifecyclePlayableRetired = 4;
inline constexpr auto kMonitorLifecyclePlayableStart = 1;
inline constexpr auto kMonitorLifecycleQueueAdvance = 10;
inline constexpr auto kMonitorLifecycleQueueFail = 11;
inline constexpr auto kMonitorLifecycleTransitionReject = 14;
inline constexpr auto kMonitorLifecycleTransitionStart = 12;
inline constexpr auto kMonitorLifecycleTransitionStop = 13;
inline constexpr auto kMonitorLifecycleVoiceEnd = 18;
inline constexpr auto kMonitorLifecycleVoiceLive = 16;
inline constexpr auto kMonitorLifecycleVoicePark = 17;
inline constexpr auto kMonitorLifecycleVoicePending = 15;
inline constexpr auto kMonitorMeasureLufsIntegrated = 11;
inline constexpr auto kMonitorMeasureLufsMomentary = 9;
inline constexpr auto kMonitorMeasureLufsShortTerm = 10;
inline constexpr auto kMonitorOperationLoadPack = 10;
inline constexpr auto kMonitorOperationPlayPreview = 3;
inline constexpr auto kMonitorOperationPostAction = 1;
inline constexpr auto kMonitorOperationPostActionByName = 2;
inline constexpr auto kMonitorOperationPostActionPinned = 14;
inline constexpr auto kMonitorOperationPostActionResolved = 15;
inline constexpr auto kMonitorOperationSetEmitterValue = 9;
inline constexpr auto kMonitorOperationSetEmitterValues = 13;
inline constexpr auto kMonitorOperationSetValue = 8;
inline constexpr auto kMonitorOperationSetValues = 12;
inline constexpr auto kMonitorOperationStopAction = 6;
inline constexpr auto kMonitorOperationStopAll = 7;
inline constexpr auto kMonitorOperationStopEmitter = 16;
inline constexpr auto kMonitorOperationStopPreview = 4;
inline constexpr auto kMonitorOperationStopVoice = 5;
inline constexpr auto kMonitorOperationUnloadPack = 11;
inline constexpr auto kMonitorPermissionDenied = -335;
inline constexpr auto kMonitorPermCaptureControl = 8;
inline constexpr auto kMonitorPermObserveEvents = 2;
inline constexpr auto kMonitorPermObserveMeasurements = 4;
inline constexpr auto kMonitorPermObserveState = 1;
inline constexpr auto kMonitorPermRawAudio = 32;
inline constexpr auto kMonitorPermRecord = 16;
inline constexpr auto kMonitorPermSymbols = 64;
inline constexpr auto kMonitorPlanPending = 0;
inline constexpr auto kMonitorRecordingStateActive = 1;
inline constexpr auto kMonitorRecordingStateFailed = 3;
inline constexpr auto kMonitorRecordingStateStopped = 0;
inline constexpr auto kMonitorRecordingStateStopping = 2;
inline constexpr auto kMonitorRecordCapturePlanActivated = 15;
inline constexpr auto kMonitorRecordDropSummary = 13;
inline constexpr auto kMonitorRecordEnvelopePackedBytes = 316u;
inline constexpr auto kMonitorRecordEvent = 5;
inline constexpr auto kMonitorRecordEventDetail = 23;
inline constexpr auto kMonitorRecordFlagOptional = 2;
inline constexpr auto kMonitorRecordFlagRequired = 1;
inline constexpr auto kMonitorRecordInvalid = 22;
inline constexpr auto kMonitorRecordStateCutBegin = 1;
inline constexpr auto kMonitorRecordStateCutEnd = 4;
inline constexpr auto kMonitorRecordStateTombstone = 3;
inline constexpr auto kMonitorRecordStateUpsert = 2;
inline constexpr auto kMonitorRecordStreamBaseline = 14;
inline constexpr auto kMonitorRecordTimeSeries = 6;
inline constexpr auto kMonitorRecordUnknown = 0;
inline constexpr auto kMonitorResyncRequired = -347;
inline constexpr auto kMonitorSequenceExhausted = -345;
inline constexpr auto kMonitorSessionCapacity = -340;
inline constexpr auto kMonitorSessionChunkHeaderBytes = 32u;
inline constexpr auto kMonitorSessionCommitFooterBytes = 24u;
inline constexpr auto kMonitorSessionCommitMagic = 0x3031544d434e56ull;
inline constexpr auto kMonitorSessionFileMagic = 0x30314e4f4d4e56ull;
inline constexpr auto kMonitorSessionFormatMajor = 1u;
inline constexpr auto kMonitorSessionFormatMinor = 0u;
inline constexpr auto kMonitorStaleLease = -336;
inline constexpr auto kMonitorStaleSession = -331;
inline constexpr auto kMonitorStaleTap = -338;
inline constexpr auto kMonitorStateCutAborted = 2;
inline constexpr auto kMonitorStateCutComplete = 0;
inline constexpr auto kMonitorStateCutTruncated = 1;
inline constexpr auto kMonitorStreamEvent = 2;
inline constexpr auto kMonitorStreamOptionalAudio = 8;
inline constexpr auto kMonitorStreamState = 1;
inline constexpr auto kMonitorStreamTimeSeries = 4;
inline constexpr auto kMonitorUnsupported = -334;
inline constexpr auto kMonitorValidArmed = 1;
inline constexpr auto kMonitorValidNumeric = 2;
inline constexpr auto kMonitorValidUnsupported = 8;
inline constexpr auto kMonitorValidWarmup = 4;
inline constexpr auto kMonitorWrongThread = -330;
inline constexpr auto kNotInitialized = -303;
inline constexpr auto kNoAction = -311;
inline constexpr auto kNoVoice = -300;
inline constexpr auto kOk = 0;
inline constexpr auto kStaleHandle = -201;
inline constexpr auto kUnknownId = -200;
inline constexpr auto kVariableScopeMismatch = -1200;
inline constexpr auto kVariableWriterForbidden = -1201;

struct MonitorRecordingStatus;
using MonitorSessionHandle = uint64_t;
struct MonitorCaptureControlStatus;
struct MonitorRecordingDesc;
struct MonitorEffectiveCapturePlan;
struct MonitorStateUpsert;
struct MonitorSessionDesc;
struct MonitorEventSessionStatus;
struct MonitorPollDesc;
struct MonitorPollResult;
struct MonitorEventDetailV2;
struct MonitorRecordHeader;
struct MonitorEventDetail;
struct MonitorSubscriptionStatus;
struct MonitorDropSummary;
struct MonitorCapturePlan;
struct MonitorCapabilities;
struct MonitorMeasurementCapability;
struct MonitorTapDescriptor;
struct MonitorMeasurementRequest;
struct MonitorStreamBaseline;
struct MonitorTimeSeriesSample;
struct MonitorSessionCompareView;
struct MonitorSessionCompareDesc;
struct MonitorCompareResult;
struct MonitorStateTombstone;
struct MonitorStateCutBegin;
struct MonitorStateCutEnd;
struct MonitorEventPayload;
struct MonitorTelemetryCorrelation;

struct MonitorRecordingStatus {
    uint32_t state{};
    uint64_t fileBytes{};
    uint64_t lastCommittedChunk{};
    uint64_t queueBytes{};
    uint32_t recoveryNeeded{};
    uint32_t errorReason{};
};

struct MonitorCaptureControlStatus {
    uint64_t leaseGeneration{};
    uint64_t principalHash{};
    uint32_t permissions{};
    uint32_t held{};
    uint64_t expiryExecuteFrame{};
    uint32_t lastChangeReason{};
};

struct MonitorRecordingDesc {
    const char * utf8Path{};
    uint64_t fileByteBudget{};
};

struct MonitorCapturePlan {
    uint32_t streamKindMask{};
    uint32_t tapCount{};
    uint32_t measurementCount{};
    uint32_t cadenceHz{};
    uint32_t downsample{};
    uint32_t channelMode{};
    uint64_t bytesPerSecondBudget{};
    uint64_t sessionByteBudget{};
    uint64_t maxDurationExecuteFrames{};
    uint32_t remotePermissionMask{};
    const MonitorTapDescriptor * taps{};
    const MonitorMeasurementRequest * measurements{};
};

struct MonitorEffectiveCapturePlan {
    uint32_t activationState{};
    uint64_t capturePlanGeneration{};
    uint64_t previousGeneration{};
    uint64_t effectiveFromExecuteFrame{};
    MonitorCapturePlan accepted{};
};

struct MonitorStateUpsert {
    uint32_t structSize{};
    uint32_t entityKind{};
    uint64_t stateCutId{};
    harkenstudio::ObjectId stableId{};
    uint64_t instanceHandle{};
    uint64_t instanceGeneration{};
    float volumeDb{};
    uint32_t mute01{};
    uint32_t solo01{};
    uint32_t bypass01{};
    uint32_t latencyFrames{};
    uint32_t tailFrames{};
    harkenstudio::ObjectId routeBusId{};
    uint32_t sendCount{};
    uint32_t occupancy{};
    uint32_t highWater{};
    uint32_t rejectCount{};
    uint32_t dropCount{};
    uint32_t reserved0{};
};

struct MonitorSessionDesc {
    uint32_t requestedStreamKindMask{};
    uint32_t requestRawAudio{};
    uint32_t requestSymbols{};
    uint64_t consumerQueueBytes{};
    uint32_t requestedPermissionMask{};
};

struct MonitorEventSessionStatus {
    uint32_t state{};
    uint64_t monitorSessionEpoch{};
    uint64_t startedTimestampNs{};
    uint64_t stoppedTimestampNs{};
    uint64_t sourceDroppedRecords{};
    uint64_t deliveryDroppedRecords{};
};

struct MonitorPollDesc {
    void * buffer{};
    uint64_t bufferBytes{};
};

struct MonitorPollResult {
    uint32_t recordsWritten{};
    uint64_t bytesWritten{};
    uint64_t nextRecordBytes{};
    uint64_t recordsRemaining{};
    uint64_t firstDeliverySequence{};
    uint64_t lastDeliverySequence{};
};

struct MonitorEventDetailV2 {
    uint32_t structSize{};
    uint32_t category{};
    uint32_t code{};
    uint32_t level{};
    int32_t result{};
    uint32_t reserved0{};
    uint64_t timestampNs{};
    uint64_t arg0{};
    uint64_t arg1{};
    uint32_t valueBits{};
    uint32_t textBytes{};
    char text[192]{};
    uint8_t projectGuid[16]{};
};

struct MonitorTelemetryCorrelation {
    uint32_t structSize{};
    uint32_t reserved0{};
    uint64_t arrangingInstance{};
    harkenstudio::ObjectId clipId{};
    uint32_t loopIteration{};
    uint32_t occurrenceOrdinal{};
    uint64_t logicCommitSequence{};
    uint64_t executeFrame{};
};

struct MonitorRecordHeader {
    uint32_t structSize{};
    uint32_t recordKind{};
    uint32_t recordVersion{};
    uint32_t flags{};
    uint32_t streamKind{};
    uint32_t reserved0{};
    uint64_t recordBytes{};
    uint64_t streamId{};
    uint64_t producerId{};
    uint64_t sourceSequence{};
    uint64_t deliverySequence{};
    uint64_t engineMonitorInstanceId{};
    uint64_t monitorSessionEpoch{};
    uint64_t worldGeneration{};
    uint64_t capturePlanGeneration{};
    uint64_t renderPlanGeneration{};
    uint64_t transportEpoch{};
    uint64_t deviceEpoch{};
    uint64_t clockAnchorGeneration{};
    uint64_t measurementSegmentGeneration{};
    uint64_t executeFrame{};
    uint64_t graphFrame{};
    uint64_t devicePresentationEstimate{};
    uint32_t deviceUncertaintyFrames{};
    uint32_t entityKind{};
    uint64_t programSemanticFingerprint{};
    uint64_t runtimeLineageFingerprint{};
    uint64_t optionalSymbolFingerprint{};
    harkenstudio::ObjectId stableId{};
    uint64_t instanceHandle{};
    uint64_t instanceGeneration{};
    uint64_t actionInstanceHandle{};
    uint64_t playableInstanceHandle{};
    uint64_t parentActionInstanceHandle{};
    uint64_t traceCorrelationId{};
    uint32_t reason{};
    uint32_t validityFlags{};
    uint64_t payloadBytes{};
    MonitorTelemetryCorrelation telemetry{};
};

struct MonitorEventDetail {
    uint32_t structSize{};
    uint32_t category{};
    uint32_t code{};
    uint32_t level{};
    int32_t result{};
    uint32_t reserved0{};
    uint64_t timestampNs{};
    uint64_t arg0{};
    uint64_t arg1{};
    uint32_t valueBits{};
    uint32_t textBytes{};
    char text[192]{};
};

struct MonitorSubscriptionStatus {
    uint32_t requestedStreamKindMask{};
    uint32_t resolvedStreamKindMask{};
    uint32_t subscriptionGeneration{};
    uint32_t requestedPermissionMask{};
    uint32_t acceptedPermissionMask{};
    uint32_t deniedPermissionMask{};
    uint32_t unsupportedPermissionMask{};
    uint64_t monitorSessionEpoch{};
    uint32_t rawAudioDecision{};
    uint32_t symbolsDecision{};
    uint32_t baselineCapacity{};
    uint32_t baselineCount{};
    MonitorStreamBaseline * baselines{};
};

struct MonitorDropSummary {
    uint32_t structSize{};
    uint32_t stage{};
    uint64_t streamId{};
    uint64_t firstSourceSequence{};
    uint64_t lastSourceSequence{};
    uint64_t recordCount{};
    uint64_t byteCount{};
    uint32_t reason{};
    uint32_t reserved0{};
};

struct MonitorCapabilities {
    bool consumerQueueBudgetAvailable = true;
    uint32_t monitorImplemented{};
    uint32_t remoteEnabled{};
    uint32_t securityProfile{};
    uint32_t keepPlanAfterLeaseTimeout{};
    uint32_t resolvedSessionsPerEngine{};
    uint32_t hardSessionsPerEngine{};
    uint32_t resolvedProcessSessionSlots{};
    uint32_t hardProcessSessionSlots{};
    uint64_t resolvedProcessQueueBytes{};
    uint64_t hardProcessQueueBytes{};
    uint32_t resolvedRemoteClients{};
    uint32_t hardRemoteClients{};
    uint32_t resolvedPlanSlots{};
    uint32_t hardPlanSlots{};
    uint32_t dedicatedStopSlots{};
    uint32_t lufsMomentarySupported{};
    uint32_t lufsShortTermSupported{};
    uint32_t lufsIntegratedSupported{};
    uint64_t capabilityFingerprint{};
    uint64_t resolvedProducerRingBytes{};
    uint64_t hardProducerRingBytes{};
    uint64_t resolvedConsumerQueueBytes{};
    uint64_t hardConsumerQueueBytes{};
    uint32_t resolvedDerivedWorkItems{};
    uint32_t hardDerivedWorkItems{};
    uint64_t resolvedRecorderQueueBytes{};
    uint64_t hardRecorderQueueBytes{};
    uint64_t resolvedRemoteQueueBytes{};
    uint64_t hardRemoteQueueBytes{};
    uint64_t resolvedCaptureByteRate{};
    uint64_t hardCaptureByteRate{};
    uint64_t resolvedRawAudioByteRate{};
    uint64_t hardRawAudioByteRate{};
};

struct MonitorMeasurementCapability {
    uint32_t kind{};
    uint32_t supported{};
    uint32_t algorithmVersion{};
    uint32_t unit{};
    float tolerance{};
};

struct MonitorTapDescriptor {
    uint32_t tapKind{};
    uint64_t tapRegistryGeneration{};
    uint64_t tapDescriptorId{};
    uint64_t tapDescriptorGeneration{};
    uint32_t ownerKind{};
    uint32_t selectorMode{};
    harkenstudio::ObjectId ownerStableId{};
    uint64_t ownerInstanceHandle{};
    uint64_t ownerGeneration{};
    uint32_t slotId{};
    uint32_t slotGeneration{};
    uint64_t expectedEffectGeneration{};
    uint32_t layout{};
    uint32_t sampleRate{};
    uint32_t channelOrder{};
    uint32_t capabilityState{};
    uint32_t availabilityState{};
    uint32_t reason{};
};

struct MonitorMeasurementRequest {
    uint32_t kind{};
    uint32_t tapIndex{};
    uint32_t channelMode{};
    uint32_t downsample{};
    uint32_t cadenceHz{};
    uint32_t bucketFrames{};
    uint32_t fftSize{};
    uint32_t windowKind{};
    uint32_t overlapNumerator{};
    uint32_t overlapDenominator{};
    uint64_t tapDescriptorId{};
    float triggerThreshold{};
    float hysteresis{};
    uint32_t triggerMode{};
};

struct MonitorStreamBaseline {
    uint32_t structSize{};
    uint32_t reserved0{};
    uint64_t streamId{};
    uint64_t firstExpectedSourceSequence{};
    uint32_t streamKind{};
    uint32_t reserved1{};
};

struct MonitorTimeSeriesSample {
    uint32_t structSize{};
    uint32_t metricId{};
    uint32_t cadenceHz{};
    uint32_t unitId{};
    uint32_t algorithmId{};
    uint32_t algorithmVersion{};
    uint32_t valueBits{};
    uint32_t toleranceBits{};
};

struct MonitorSessionCompareView {
    uint32_t status{};
    uint32_t recoveryNeeded{};
    uint32_t droppedRequired{};
    uint32_t requiredUnknown{};
    uint32_t crcFailedRequired{};
    uint32_t truncatedAfterCommit{};
    uint32_t hasCompleteFooter{};
    uint32_t hasTraceCorrelation{};
    uint32_t hasOptionalSymbols{};
    uint64_t lastCompleteChunkSequence{};
    uint64_t fileBytes{};
    uint64_t bytesConsumed{};
    uint64_t capabilityFingerprint{};
    uint64_t programSemanticFingerprint{};
    uint64_t runtimeLineageFingerprint{};
    uint64_t tapDescriptorId{};
    uint64_t measurementSegmentGeneration{};
    uint32_t sampleRate{};
    uint32_t algorithmId{};
    uint32_t algorithmVersion{};
    uint32_t unitId{};
    uint32_t targetStreamKindMask{};
    uint32_t presentStreamKindMask{};
    uint64_t rangeBegin{};
    uint64_t rangeEnd{};
    uint32_t gapInvalidWarmup{};
};

struct MonitorSessionCompareDesc {
    const MonitorSessionCompareView * left{};
    const MonitorSessionCompareView * right{};
    uint64_t executeFrameBegin{};
    uint64_t executeFrameEnd{};
};

struct MonitorCompareResult {
    uint32_t result{};
};

struct MonitorStateTombstone {
    uint32_t structSize{};
    uint32_t entityKind{};
    uint64_t stateCutId{};
    harkenstudio::ObjectId stableId{};
    uint64_t instanceHandle{};
    uint64_t instanceGeneration{};
    uint32_t reason{};
    uint32_t reserved0{};
};

struct MonitorStateCutBegin {
    uint32_t structSize{};
    uint32_t reserved0{};
    uint64_t stateCutId{};
    uint64_t worldGeneration{};
    uint64_t capturePlanGeneration{};
    uint64_t renderPlanGeneration{};
    uint64_t transportGeneration{};
    uint64_t capacityLimit{};
    uint64_t entityTrueCount[16]{};
};

struct MonitorStateCutEnd {
    uint32_t structSize{};
    uint32_t result{};
    uint64_t stateCutId{};
    uint32_t checksumAlgorithm{};
    uint32_t reserved0{};
    uint64_t checksum{};
    uint64_t emittedCount[16]{};
};

struct MonitorEventPayload {
    uint32_t structSize{};
    uint32_t eventClass{};
    uint32_t reason{};
    uint32_t reserved0{};
    uint64_t playableInstanceHandle{};
    uint64_t actionInstanceHandle{};
    uint64_t voiceInstanceHandle{};
    uint64_t parentActionInstanceHandle{};
};

}
