#pragma once

#include "NativeValues.h"
#include "Monitor/MonitorTypes.h"
#include <harken/Harken.h>
#include <algorithm>

namespace HarkenEditor::Bridge {

inline VnMonitorSessionDescV2 toNative(const VNX::Monitor::MonitorSessionDesc& value)
{
    VnMonitorSessionDescV2 out{};
    out.structSize = value.requestedExecutionFactVersion ? sizeof(out) : sizeof(VnMonitorSessionDesc);
    out.requestedExecutionFactVersion = value.requestedExecutionFactVersion;
    out.requestedStreamKindMask = value.requestedStreamKindMask;
    out.requestRawAudio = value.requestRawAudio;
    out.requestSymbols = value.requestSymbols;
    out.consumerQueueBytes = value.consumerQueueBytes;
    out.requestedPermissionMask = value.requestedPermissionMask;
    return out;
}

inline VnMonitorPollDesc toNative(const VNX::Monitor::MonitorPollDesc& value)
{
    VnMonitorPollDesc out{};
    out.structSize = sizeof(out);
    out.bufferBytes = value.bufferBytes;
    return out;
}

inline VnMonitorRecordingDesc toNative(const VNX::Monitor::MonitorRecordingDesc& value)
{
    VnMonitorRecordingDesc out{};
    out.structSize = sizeof(out);
    out.fileByteBudget = value.fileByteBudget;
    return out;
}

inline VnMonitorSessionCompareView toNative(const VNX::Monitor::MonitorSessionCompareView& value)
{
    VnMonitorSessionCompareView out{};
    out.structSize = sizeof(out);
    out.status = value.status;
    out.recoveryNeeded = value.recoveryNeeded;
    out.droppedRequired = value.droppedRequired;
    out.requiredUnknown = value.requiredUnknown;
    out.crcFailedRequired = value.crcFailedRequired;
    out.truncatedAfterCommit = value.truncatedAfterCommit;
    out.hasCompleteFooter = value.hasCompleteFooter;
    out.hasTraceCorrelation = value.hasTraceCorrelation;
    out.hasOptionalSymbols = value.hasOptionalSymbols;
    out.lastCompleteChunkSequence = value.lastCompleteChunkSequence;
    out.fileBytes = value.fileBytes;
    out.bytesConsumed = value.bytesConsumed;
    out.capabilityFingerprint = value.capabilityFingerprint;
    out.programSemanticFingerprint = value.programSemanticFingerprint;
    out.runtimeLineageFingerprint = value.runtimeLineageFingerprint;
    out.tapDescriptorId = value.tapDescriptorId;
    out.measurementSegmentGeneration = value.measurementSegmentGeneration;
    out.sampleRate = value.sampleRate;
    out.algorithmId = value.algorithmId;
    out.algorithmVersion = value.algorithmVersion;
    out.unitId = value.unitId;
    out.targetStreamKindMask = value.targetStreamKindMask;
    out.presentStreamKindMask = value.presentStreamKindMask;
    out.rangeBegin = value.rangeBegin;
    out.rangeEnd = value.rangeEnd;
    out.gapInvalidWarmup = value.gapInvalidWarmup;
    return out;
}

inline VnMonitorTapDescriptor toNative(const VNX::Monitor::MonitorTapDescriptor& value)
{
    VnMonitorTapDescriptor out{};
    out.structSize = sizeof(out);
    out.tapKind = value.tapKind;
    out.tapRegistryGeneration = value.tapRegistryGeneration;
    out.tapDescriptorId = value.tapDescriptorId;
    out.tapDescriptorGeneration = value.tapDescriptorGeneration;
    out.ownerKind = value.ownerKind;
    out.selectorMode = value.selectorMode;
    out.ownerStableId = toNative(value.ownerStableId);
    out.ownerInstanceHandle = value.ownerInstanceHandle;
    out.ownerGeneration = value.ownerGeneration;
    out.slotId = value.slotId;
    out.slotGeneration = value.slotGeneration;
    out.expectedEffectGeneration = value.expectedEffectGeneration;
    out.layout = value.layout;
    out.sampleRate = value.sampleRate;
    out.channelOrder = value.channelOrder;
    out.capabilityState = value.capabilityState;
    out.availabilityState = value.availabilityState;
    out.reason = value.reason;
    return out;
}

inline VnMonitorMeasurementRequest toNative(const VNX::Monitor::MonitorMeasurementRequest& value)
{
    VnMonitorMeasurementRequest out{};
    out.structSize = sizeof(out);
    out.kind = value.kind;
    out.tapIndex = value.tapIndex;
    out.channelMode = value.channelMode;
    out.downsample = value.downsample;
    out.cadenceHz = value.cadenceHz;
    out.bucketFrames = value.bucketFrames;
    out.fftSize = value.fftSize;
    out.windowKind = value.windowKind;
    out.overlapNumerator = value.overlapNumerator;
    out.overlapDenominator = value.overlapDenominator;
    out.tapDescriptorId = value.tapDescriptorId;
    out.triggerThreshold = value.triggerThreshold;
    out.hysteresis = value.hysteresis;
    out.triggerMode = value.triggerMode;
    return out;
}

inline VnMonitorCapturePlan toNative(const VNX::Monitor::MonitorCapturePlan& value)
{
    VnMonitorCapturePlan out{};
    out.structSize = sizeof(out);
    out.streamKindMask = value.streamKindMask;
    out.tapCount = value.tapCount;
    out.measurementCount = value.measurementCount;
    out.cadenceHz = value.cadenceHz;
    out.downsample = value.downsample;
    out.channelMode = value.channelMode;
    out.bytesPerSecondBudget = value.bytesPerSecondBudget;
    out.sessionByteBudget = value.sessionByteBudget;
    out.maxDurationExecuteFrames = value.maxDurationExecuteFrames;
    out.remotePermissionMask = value.remotePermissionMask;
    return out;
}

inline VNX::Monitor::MonitorEventSessionStatus fromNative(const VnMonitorEventSessionStatusV2& value)
{
    VNX::Monitor::MonitorEventSessionStatus out{};
    out.state = value.state;
    out.monitorSessionEpoch = value.monitorSessionEpoch;
    out.startedTimestampNs = value.startedTimestampNs;
    out.stoppedTimestampNs = value.stoppedTimestampNs;
    out.sourceDroppedRecords = value.sourceDroppedRecords;
    out.deliveryDroppedRecords = value.deliveryDroppedRecords;
    out.acceptedExecutionFactVersion = value.acceptedExecutionFactVersion;
    out.baselineAvailability = value.baselineAvailability;
    out.requestedPermissionMask = value.requestedPermissionMask;
    out.acceptedPermissionMask = value.acceptedPermissionMask;
    out.deniedPermissionMask = value.deniedPermissionMask;
    out.unsupportedPermissionMask = value.unsupportedPermissionMask;
    out.acceptedStreamKindMask = value.acceptedStreamKindMask;
    out.symbolsDecision = value.symbolsDecision;
    out.symbolAvailability = value.symbolAvailability;
    out.identityUnavailableCount = value.identityUnavailableCount;
    out.baselineStateCutId = value.baselineStateCutId;
    std::copy_n(value.supportedFactKinds, 2, out.supportedFactKinds);
    return out;
}

inline VNX::Monitor::MonitorCapabilities fromNative(const VnMonitorCapabilities& value)
{
    VNX::Monitor::MonitorCapabilities out{};
    out.consumerQueueBudgetAvailable = value.structSize >=
        offsetof(VnMonitorCapabilities, hardConsumerQueueBytes) + sizeof(value.hardConsumerQueueBytes);
    out.monitorImplemented = value.monitorImplemented;
    out.remoteEnabled = value.remoteEnabled;
    out.securityProfile = value.securityProfile;
    out.keepPlanAfterLeaseTimeout = value.keepPlanAfterLeaseTimeout;
    out.resolvedSessionsPerEngine = value.resolvedSessionsPerEngine;
    out.hardSessionsPerEngine = value.hardSessionsPerEngine;
    out.resolvedProcessSessionSlots = value.resolvedProcessSessionSlots;
    out.hardProcessSessionSlots = value.hardProcessSessionSlots;
    out.resolvedProcessQueueBytes = value.resolvedProcessQueueBytes;
    out.hardProcessQueueBytes = value.hardProcessQueueBytes;
    out.resolvedRemoteClients = value.resolvedRemoteClients;
    out.hardRemoteClients = value.hardRemoteClients;
    out.resolvedPlanSlots = value.resolvedPlanSlots;
    out.hardPlanSlots = value.hardPlanSlots;
    out.dedicatedStopSlots = value.dedicatedStopSlots;
    out.lufsMomentarySupported = value.lufsMomentarySupported;
    out.lufsShortTermSupported = value.lufsShortTermSupported;
    out.lufsIntegratedSupported = value.lufsIntegratedSupported;
    out.capabilityFingerprint = value.capabilityFingerprint;
    out.resolvedProducerRingBytes = value.resolvedProducerRingBytes;
    out.hardProducerRingBytes = value.hardProducerRingBytes;
    out.resolvedConsumerQueueBytes = value.resolvedConsumerQueueBytes;
    out.hardConsumerQueueBytes = value.hardConsumerQueueBytes;
    out.resolvedDerivedWorkItems = value.resolvedDerivedWorkItems;
    out.hardDerivedWorkItems = value.hardDerivedWorkItems;
    out.resolvedRecorderQueueBytes = value.resolvedRecorderQueueBytes;
    out.hardRecorderQueueBytes = value.hardRecorderQueueBytes;
    out.resolvedRemoteQueueBytes = value.resolvedRemoteQueueBytes;
    out.hardRemoteQueueBytes = value.hardRemoteQueueBytes;
    out.resolvedCaptureByteRate = value.resolvedCaptureByteRate;
    out.hardCaptureByteRate = value.hardCaptureByteRate;
    out.resolvedRawAudioByteRate = value.resolvedRawAudioByteRate;
    out.hardRawAudioByteRate = value.hardRawAudioByteRate;
    return out;
}

inline VNX::Monitor::MonitorCaptureControlStatus fromNative(const VnMonitorCaptureControlStatus& value)
{
    VNX::Monitor::MonitorCaptureControlStatus out{};
    out.leaseGeneration = value.leaseGeneration;
    out.principalHash = value.principalHash;
    out.permissions = value.permissions;
    out.held = value.held;
    out.expiryExecuteFrame = value.expiryExecuteFrame;
    out.lastChangeReason = value.lastChangeReason;
    return out;
}

inline VNX::Monitor::MonitorPollResult fromNative(const VnMonitorPollResult& value)
{
    VNX::Monitor::MonitorPollResult out{};
    out.recordsWritten = value.recordsWritten;
    out.bytesWritten = value.bytesWritten;
    out.nextRecordBytes = value.nextRecordBytes;
    out.recordsRemaining = value.recordsRemaining;
    out.firstDeliverySequence = value.firstDeliverySequence;
    out.lastDeliverySequence = value.lastDeliverySequence;
    return out;
}

inline VNX::Monitor::MonitorRecordingStatus fromNative(const VnMonitorRecordingStatus& value)
{
    VNX::Monitor::MonitorRecordingStatus out{};
    out.state = value.state;
    out.fileBytes = value.fileBytes;
    out.lastCommittedChunk = value.lastCommittedChunk;
    out.queueBytes = value.queueBytes;
    out.recoveryNeeded = value.recoveryNeeded;
    out.errorReason = value.errorReason;
    return out;
}

inline VNX::Monitor::MonitorStreamBaseline fromNative(const VnMonitorStreamBaseline& value)
{
    VNX::Monitor::MonitorStreamBaseline out{};
    out.structSize = sizeof(out);
    out.streamId = value.streamId;
    out.firstExpectedSourceSequence = value.firstExpectedSourceSequence;
    out.streamKind = value.streamKind;
    return out;
}

inline VNX::Monitor::MonitorDropSummary fromNative(const VnMonitorDropSummary& value)
{
    VNX::Monitor::MonitorDropSummary out{};
    out.structSize = sizeof(out);
    out.stage = value.stage;
    out.streamId = value.streamId;
    out.firstSourceSequence = value.firstSourceSequence;
    out.lastSourceSequence = value.lastSourceSequence;
    out.recordCount = value.recordCount;
    out.byteCount = value.byteCount;
    out.reason = value.reason;
    return out;
}

inline VNX::Monitor::MonitorSubscriptionStatus fromNative(const VnMonitorSubscriptionStatus& value)
{
    VNX::Monitor::MonitorSubscriptionStatus out{};
    out.requestedStreamKindMask = value.requestedStreamKindMask;
    out.resolvedStreamKindMask = value.resolvedStreamKindMask;
    out.subscriptionGeneration = value.subscriptionGeneration;
    out.requestedPermissionMask = value.requestedPermissionMask;
    out.acceptedPermissionMask = value.acceptedPermissionMask;
    out.deniedPermissionMask = value.deniedPermissionMask;
    out.unsupportedPermissionMask = value.unsupportedPermissionMask;
    out.monitorSessionEpoch = value.monitorSessionEpoch;
    out.rawAudioDecision = value.rawAudioDecision;
    out.symbolsDecision = value.symbolsDecision;
    out.baselineCapacity = value.baselineCapacity;
    out.baselineCount = value.baselineCount;
    return out;
}

inline VNX::Monitor::MonitorCapturePlan fromNative(const VnMonitorCapturePlan& value)
{
    VNX::Monitor::MonitorCapturePlan out{};
    out.streamKindMask = value.streamKindMask;
    out.tapCount = value.tapCount;
    out.measurementCount = value.measurementCount;
    out.cadenceHz = value.cadenceHz;
    out.downsample = value.downsample;
    out.channelMode = value.channelMode;
    out.bytesPerSecondBudget = value.bytesPerSecondBudget;
    out.sessionByteBudget = value.sessionByteBudget;
    out.maxDurationExecuteFrames = value.maxDurationExecuteFrames;
    out.remotePermissionMask = value.remotePermissionMask;
    return out;
}

inline VNX::Monitor::MonitorEffectiveCapturePlan fromNative(const VnMonitorEffectiveCapturePlan& value)
{
    VNX::Monitor::MonitorEffectiveCapturePlan out{};
    out.activationState = value.activationState;
    out.capturePlanGeneration = value.capturePlanGeneration;
    out.previousGeneration = value.previousGeneration;
    out.effectiveFromExecuteFrame = value.effectiveFromExecuteFrame;
    out.accepted = fromNative(value.accepted);
    return out;
}

inline VNX::Monitor::MonitorCompareResult fromNative(const VnMonitorCompareResult& value)
{
    VNX::Monitor::MonitorCompareResult out{};
    out.result = value.result;
    return out;
}

}
