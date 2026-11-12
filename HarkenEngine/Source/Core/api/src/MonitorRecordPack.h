#ifndef HARKEN_API_MONITOR_RECORD_PACK_H
#define HARKEN_API_MONITOR_RECORD_PACK_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harken {
namespace monitor {

inline constexpr std::uint32_t kPackedEnvelopeBytes =
    VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
inline constexpr std::uint64_t kSyntheticStreamId = 1;
inline constexpr std::uint64_t kSyntheticStateStreamId = 2;
inline constexpr std::uint64_t kTimeSeriesCallbackCpuStreamId = 101;
inline constexpr std::uint64_t kTimeSeriesStageDrainCpuStreamId = 102;
inline constexpr std::uint64_t kTimeSeriesStageVoicesCpuStreamId = 103;
inline constexpr std::uint64_t kTimeSeriesStageBusesCpuStreamId = 104;
inline constexpr std::uint64_t kTimeSeriesArenaHighWaterStreamId = 105;
inline constexpr std::uint32_t kTimeSeriesMetricCallbackCpu = 101;
inline constexpr std::uint32_t kTimeSeriesMetricStageDrainCpu = 102;
inline constexpr std::uint32_t kTimeSeriesMetricStageVoicesCpu = 103;
inline constexpr std::uint32_t kTimeSeriesMetricStageBusesCpu = 104;
inline constexpr std::uint32_t kTimeSeriesMetricArenaHighWater = 105;
inline constexpr std::uint32_t kTimeSeriesUnitMicroseconds = 1;
inline constexpr std::uint32_t kTimeSeriesUnitBytes = 2;
inline constexpr std::uint32_t kSyntheticStreamKind = VN_MONITOR_STREAM_TIME_SERIES;
inline constexpr std::uint32_t kStateCutRequiredRecords = 2;

constexpr std::uint32_t PackedRecordBytes(std::uint32_t payloadBytes) {
    const std::uint32_t total = kPackedEnvelopeBytes + payloadBytes;
    return (total + 7u) & ~7u;
}

inline std::uint32_t Crc32C(const void* data, std::size_t length) {
    static constexpr std::uint32_t kPoly = 0x82F63B78u;
    std::uint32_t crc = 0xFFFFFFFFu;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (kPoly & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

inline void WriteU32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}

inline void WriteU64(std::uint8_t* p, std::uint64_t value) {
    WriteU32(p, static_cast<std::uint32_t>(value));
    WriteU32(p + 4, static_cast<std::uint32_t>(value >> 32));
}

inline void WriteObjectId(std::uint8_t* p, VnObjectId value) {
    const auto bytes = ObjectIdBytes(value);
    std::memcpy(p, bytes.data(), bytes.size());
}

constexpr std::uint32_t PackedTimeSeriesBytes() {
    return PackedRecordBytes(static_cast<std::uint32_t>(
        sizeof(VnMonitorTimeSeriesSample)));
}

inline std::uint32_t PackedDropSummaryBytes() {
    return PackedRecordBytes(
        static_cast<std::uint32_t>(sizeof(VnMonitorDropSummary)));
}

inline std::uint32_t PackedControlRecordBytes() {
    return PackedRecordBytes(0);
}

inline std::uint32_t PackedStateCutBeginBytes() {
    return PackedRecordBytes(
        static_cast<std::uint32_t>(sizeof(VnMonitorStateCutBegin)));
}

inline std::uint32_t PackedStateCutEndBytes() {
    return PackedRecordBytes(
        static_cast<std::uint32_t>(sizeof(VnMonitorStateCutEnd)));
}

inline std::uint32_t PackedEventBytes() {
    return PackedRecordBytes(
        static_cast<std::uint32_t>(sizeof(VnMonitorEventPayload)));
}

inline std::uint32_t PackedStateUpsertBytes() {
    return PackedRecordBytes(
        static_cast<std::uint32_t>(sizeof(VnMonitorStateUpsert)));
}

inline std::uint32_t PackedStateTombstoneBytes() {
    return PackedRecordBytes(
        static_cast<std::uint32_t>(sizeof(VnMonitorStateTombstone)));
}

inline std::uint32_t PackedStreamBaselineBytes() {
    return PackedControlRecordBytes();
}

inline constexpr std::uint32_t kPackedAudioMaxChannels = 8;
inline constexpr std::uint64_t kAudioChunkStreamId = 8;
// Packed envelope offsets from PackHeader write order, not C offsetof.
inline constexpr std::size_t kPackedCapturePlanGenerationOffset = 80;
inline constexpr std::size_t kPackedClockAnchorGenerationOffset = 112;
inline constexpr std::size_t kPackedMeasurementSegmentGenerationOffset = 120;
inline constexpr std::size_t kPackedExecuteFrameOffset = 128;

inline bool AudioFormatChangesSegment(std::uint32_t lastRate, std::uint32_t lastChannels,
                                      std::uint32_t lastLayout, std::uint32_t sampleRate,
                                      std::uint32_t channels, std::uint32_t layout) {
    return lastRate != 0 &&
           (lastRate != sampleRate || lastChannels != channels || lastLayout != layout);
}

inline std::uint32_t PackedAudioChunkBytes(std::uint32_t frames,
                                           std::uint32_t channels) {
    const std::uint64_t sampleBytes =
        static_cast<std::uint64_t>(frames) * channels * sizeof(float);
    const std::uint64_t payload =
        static_cast<std::uint64_t>(sizeof(VnMonitorAudioChunk)) + sampleBytes;
    if (payload > 0xffffffffull) {
        return 0;
    }
    return PackedRecordBytes(static_cast<std::uint32_t>(payload));
}

inline std::uint32_t PackedAudioChunkMaxBytes() {
    return PackedAudioChunkBytes(VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES,
                                 kPackedAudioMaxChannels);
}

inline std::uint32_t PackedWaveformBucketBytes() {
    return PackedRecordBytes(88u + 8u);
}

inline std::uint32_t PackedScopeResultBytes() {
    return PackedRecordBytes(144u);
}

inline constexpr std::uint32_t kPackedSpectrumPayloadBytes = 48u;

static_assert(sizeof(VnMonitorSpectrumPayload) == kPackedSpectrumPayloadBytes,
              "spectrum payload stays 48 packed bytes");

inline constexpr std::uint32_t PackedSpectrumBytes(std::uint32_t binCount) {
    const std::uint64_t payload =
        static_cast<std::uint64_t>(kPackedSpectrumPayloadBytes) +
        static_cast<std::uint64_t>(binCount) * 4ull;
    if (payload > 0xffffffffull) {
        return 0;
    }
    return PackedRecordBytes(static_cast<std::uint32_t>(payload));
}

static_assert((PackedSpectrumBytes(257) & 7u) == 0u,
              "512-bin spectrum record stays 8-byte aligned");
static_assert((PackedSpectrumBytes(2049) & 7u) == 0u,
              "4096-bin spectrum record stays 8-byte aligned");

inline std::uint32_t PackedMeterBytes() { return PackedRecordBytes(64u); }

inline std::uint32_t PackedGainReductionBytes() {
    return PackedRecordBytes(48u);
}

inline std::uint32_t PackedAggregateBytes() { return PackedRecordBytes(40u); }

inline std::uint32_t PackedLogicProducerSlotBytes() {
    std::uint32_t bytes = PackedTimeSeriesBytes();
    bytes = bytes < PackedDropSummaryBytes() ? PackedDropSummaryBytes() : bytes;
    bytes = bytes < PackedControlRecordBytes() ? PackedControlRecordBytes()
                                               : bytes;
    bytes = bytes < PackedStateCutBeginBytes() ? PackedStateCutBeginBytes()
                                               : bytes;
    bytes = bytes < PackedStateCutEndBytes() ? PackedStateCutEndBytes() : bytes;
    bytes = bytes < PackedEventBytes() ? PackedEventBytes() : bytes;
    bytes = bytes < PackedStateUpsertBytes() ? PackedStateUpsertBytes() : bytes;
    bytes = bytes < PackedStateTombstoneBytes() ? PackedStateTombstoneBytes()
                                                : bytes;
    const auto detail = PackedRecordBytes(sizeof(VnMonitorEventDetailV2));
    return bytes < detail ? detail : bytes;
}

inline std::uint32_t PackedProducerSlotBytes() {
    const auto bytes = PackedLogicProducerSlotBytes();
    const std::uint32_t audio = PackedAudioChunkMaxBytes();
    return bytes < audio ? audio : bytes;
}

inline void PackHeader(std::uint8_t* out, const VnMonitorRecordHeader& header) {
    std::size_t o = 0;
    WriteU32(out + o, header.recordKind);
    o += 4;
    WriteU32(out + o, header.recordVersion);
    o += 4;
    WriteU32(out + o, header.flags);
    o += 4;
    WriteU32(out + o, header.streamKind);
    o += 4;
    WriteU64(out + o, header.recordBytes);
    o += 8;
    WriteU64(out + o, header.streamId);
    o += 8;
    WriteU64(out + o, header.producerId);
    o += 8;
    WriteU64(out + o, header.sourceSequence);
    o += 8;
    WriteU64(out + o, header.deliverySequence);
    o += 8;
    WriteU64(out + o, header.engineMonitorInstanceId);
    o += 8;
    WriteU64(out + o, header.monitorSessionEpoch);
    o += 8;
    WriteU64(out + o, header.worldGeneration);
    o += 8;
    WriteU64(out + o, header.capturePlanGeneration);
    o += 8;
    WriteU64(out + o, header.renderPlanGeneration);
    o += 8;
    WriteU64(out + o, header.transportEpoch);
    o += 8;
    WriteU64(out + o, header.deviceEpoch);
    o += 8;
    WriteU64(out + o, header.clockAnchorGeneration);
    o += 8;
    WriteU64(out + o, header.measurementSegmentGeneration);
    o += 8;
    WriteU64(out + o, header.executeFrame);
    o += 8;
    WriteU64(out + o, header.graphFrame);
    o += 8;
    WriteU64(out + o, header.devicePresentationEstimate);
    o += 8;
    WriteU32(out + o, header.deviceUncertaintyFrames);
    o += 4;
    WriteU32(out + o, header.entityKind);
    o += 4;
    WriteU64(out + o, header.programSemanticFingerprint);
    o += 8;
    WriteU64(out + o, header.runtimeLineageFingerprint);
    o += 8;
    WriteU64(out + o, header.optionalSymbolFingerprint);
    o += 8;
    WriteObjectId(out + o, header.stableId);
    o += 16;
    WriteU64(out + o, header.instanceHandle);
    o += 8;
    WriteU64(out + o, header.instanceGeneration);
    o += 8;
    WriteU64(out + o, header.actionInstanceHandle);
    o += 8;
    WriteU64(out + o, header.playableInstanceHandle);
    o += 8;
    WriteU64(out + o, header.parentActionInstanceHandle);
    o += 8;
    WriteU64(out + o, header.traceCorrelationId);
    o += 8;
    WriteU32(out + o, header.reason);
    o += 4;
    WriteU32(out + o, header.validityFlags);
    o += 4;
    WriteU64(out + o, header.payloadBytes);
    o += 8;
    WriteU32(out + o, header.telemetry.reserved0);
    o += 4;
    WriteU64(out + o, header.telemetry.arrangingInstance);
    o += 8;
    WriteObjectId(out + o, header.telemetry.clipId);
    o += 16;
    WriteU32(out + o, header.telemetry.loopIteration);
    o += 4;
    WriteU32(out + o, header.telemetry.occurrenceOrdinal);
    o += 4;
    WriteU64(out + o, header.telemetry.logicCommitSequence);
    o += 8;
    WriteU64(out + o, header.telemetry.executeFrame);
    o += 8;
}

inline void FillSyntheticHeader(VnMonitorRecordHeader* header,
                                std::uint64_t sourceSequence,
                                std::uint64_t recordBytes,
                                std::uint64_t payloadBytes,
                                std::uint32_t recordKind,
                                std::uint32_t validityFlags) {
    std::memset(header, 0, sizeof(*header));
    header->structSize = sizeof(*header);
    header->recordKind = recordKind;
    header->recordVersion = 1;
    header->flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
    header->streamKind = kSyntheticStreamKind;
    header->recordBytes = recordBytes;
    header->streamId = kSyntheticStreamId;
    header->producerId = 1;
    header->sourceSequence = sourceSequence;
    header->deliverySequence = 0;
    header->validityFlags = validityFlags;
    header->payloadBytes = payloadBytes;
    header->telemetry.structSize = sizeof(header->telemetry);
}

inline std::uint32_t PackSyntheticTimeSeries(std::uint8_t* out,
                                             std::uint32_t capacity,
                                             std::uint64_t sourceSequence) {
    const std::uint32_t bytes = PackedTimeSeriesBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes,
                        sizeof(VnMonitorTimeSeriesSample),
                        VN_MONITOR_RECORD_TIME_SERIES,
                        VN_MONITOR_VALID_UNSUPPORTED);
    PackHeader(out, header);
    VnMonitorTimeSeriesSample sample{};
    sample.structSize = sizeof(sample);
    std::memcpy(out + kPackedEnvelopeBytes, &sample, sizeof(sample));
    return bytes;
}

inline std::uint32_t PackTimeSeriesRecord(std::uint8_t* out, std::uint32_t capacity,
                                          std::uint64_t sourceSequence,
                                          std::uint64_t streamId,
                                          std::uint32_t metricId,
                                          std::uint32_t unitId,
                                          std::uint32_t cadenceHz,
                                          std::uint32_t valueBits,
                                          std::uint32_t validityFlags,
                                          std::uint64_t capturePlanGeneration,
                                          std::uint64_t executeFrame) {
    const std::uint32_t bytes = PackedTimeSeriesBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes,
                        sizeof(VnMonitorTimeSeriesSample),
                        VN_MONITOR_RECORD_TIME_SERIES, validityFlags);
    header.streamId = streamId;
    header.producerId = 2;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    PackHeader(out, header);
    VnMonitorTimeSeriesSample sample{};
    sample.structSize = sizeof(sample);
    sample.metricId = metricId;
    sample.cadenceHz = cadenceHz;
    sample.unitId = unitId;
    sample.valueBits = valueBits;
    std::memcpy(out + kPackedEnvelopeBytes, &sample, sizeof(sample));
    return bytes;
}

inline std::uint32_t PackNumericTimeSeries(std::uint8_t* out, std::uint32_t capacity,
                                           std::uint64_t sourceSequence,
                                           std::uint64_t streamId,
                                           std::uint32_t metricId,
                                           std::uint32_t unitId,
                                           std::uint32_t cadenceHz,
                                           std::uint32_t valueBits,
                                           std::uint64_t capturePlanGeneration,
                                           std::uint64_t executeFrame) {
    return PackTimeSeriesRecord(
        out, capacity, sourceSequence, streamId, metricId, unitId, cadenceHz,
        valueBits, VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC,
        capturePlanGeneration, executeFrame);
}

inline std::uint32_t PackUnknownRecord(std::uint8_t* out, std::uint32_t capacity,
                                       std::uint64_t sourceSequence,
                                       std::uint32_t flags,
                                       std::uint32_t recordKind = VN_MONITOR_RECORD_UNKNOWN,
                                       std::uint32_t recordVersion = 1) {
    const std::uint32_t bytes = PackedControlRecordBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 0, recordKind, 0);
    header.recordVersion = recordVersion;
    header.flags = flags;
    header.streamKind = VN_MONITOR_STREAM_TIME_SERIES;
    PackHeader(out, header);
    return bytes;
}

inline std::uint32_t PackedActivationBytes() {
    return (kPackedEnvelopeBytes + 7u) & ~7u;
}

inline std::uint32_t PackCapturePlanActivated(std::uint8_t* out,
                                              std::uint32_t capacity,
                                              std::uint64_t sourceSequence,
                                              std::uint64_t capturePlanGeneration,
                                              std::uint64_t executeFrame,
                                              std::uint32_t reason) {
    const std::uint32_t bytes = PackedActivationBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 0,
                        VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED, 0);
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.reason = reason;
    header.streamKind = VN_MONITOR_STREAM_EVENT;
    PackHeader(out, header);
    return bytes;
}

inline std::uint32_t PackAudioChunkRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t tapDescriptorId, std::uint64_t tapDescriptorGeneration,
    std::uint32_t sampleFormat, std::uint32_t sampleRate,
    std::uint32_t channelCount, std::uint32_t layout,
    std::uint64_t sourceFrameBegin, std::uint64_t sourceFrameEnd,
    const float* interleaved, std::uint32_t frames,
    std::uint64_t measurementSegmentGeneration = 0) {
    if (interleaved == nullptr || frames == 0 || channelCount == 0 ||
        frames > VN_MONITOR_HARD_AUDIO_CHUNK_FRAMES ||
        channelCount > VN_MONITOR_HARD_AUDIO_CHANNELS) {
        return 0;
    }
    const std::uint32_t bytes = PackedAudioChunkBytes(frames, channelCount);
    if (out == nullptr || bytes == 0 || capacity < bytes) {
        return 0;
    }
    const std::uint32_t sampleBytes = frames * channelCount * sizeof(float);
    const std::uint32_t payloadBytes =
        static_cast<std::uint32_t>(sizeof(VnMonitorAudioChunk)) + sampleBytes;
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, payloadBytes,
                        VN_MONITOR_RECORD_AUDIO_CHUNK,
                        VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.streamId = kAudioChunkStreamId;
    header.producerId = 4;
    header.capturePlanGeneration = capturePlanGeneration;
    header.measurementSegmentGeneration = measurementSegmentGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = tapDescriptorId;
    header.instanceGeneration = tapDescriptorGeneration;
    PackHeader(out, header);
    VnMonitorAudioChunk chunk{};
    chunk.structSize = sizeof(chunk);
    chunk.sampleFormat = sampleFormat;
    chunk.sourceFrameBegin = sourceFrameBegin;
    chunk.sourceFrameEnd = sourceFrameEnd;
    chunk.sourceSequenceFirst = sourceSequence;
    chunk.sourceSequenceLast = sourceSequence;
    chunk.sampleRate = sampleRate;
    chunk.channelCount = channelCount;
    chunk.channelOrder = 0;
    chunk.layout = layout;
    chunk.tapDescriptorId = tapDescriptorId;
    chunk.tapDescriptorGeneration = tapDescriptorGeneration;
    chunk.algorithmId = 0;
    chunk.algorithmVersion = 0;
    std::memcpy(out + kPackedEnvelopeBytes, &chunk, sizeof(chunk));
    std::memcpy(out + kPackedEnvelopeBytes + sizeof(chunk), interleaved,
                sampleBytes);
    return bytes;
}

inline std::uint32_t PackTapBindingChanged(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t oldDescriptorId, std::uint64_t oldDescriptorGeneration,
    std::uint64_t newDescriptorId, std::uint64_t newDescriptorGeneration) {
    const std::uint32_t bytes = PackedControlRecordBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 0,
                        VN_MONITOR_RECORD_TAP_BINDING_CHANGED, 0);
    header.streamKind = VN_MONITOR_STREAM_EVENT;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = oldDescriptorId;
    header.instanceGeneration = oldDescriptorGeneration;
    header.actionInstanceHandle = newDescriptorId;
    header.playableInstanceHandle = newDescriptorGeneration;
    header.reason = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    PackHeader(out, header);
    return bytes;
}

inline std::uint32_t PackTapBindingChanged(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t oldDescriptorId, std::uint64_t oldDescriptorGeneration,
    std::uint64_t newDescriptorId, std::uint64_t newDescriptorGeneration,
    std::uint32_t reason) {
    const std::uint32_t bytes = PackedControlRecordBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 0,
                        VN_MONITOR_RECORD_TAP_BINDING_CHANGED, 0);
    header.streamKind = VN_MONITOR_STREAM_EVENT;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = oldDescriptorId;
    header.instanceGeneration = oldDescriptorGeneration;
    header.actionInstanceHandle = newDescriptorId;
    header.playableInstanceHandle = newDescriptorGeneration;
    header.reason = reason;
    PackHeader(out, header);
    return bytes;
}

inline std::uint32_t PackWaveformBucketRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t tapDescriptorId, std::uint64_t tapDescriptorGeneration,
    const VnMonitorWaveformBucket& payload, float minValue, float maxValue,
    std::uint32_t recordKind, std::uint32_t validityFlags) {
    const std::uint32_t bytes = PackedWaveformBucketBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 88u + 8u, recordKind,
                        validityFlags);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = tapDescriptorId;
    header.instanceGeneration = tapDescriptorGeneration;
    PackHeader(out, header);
    std::uint8_t* p = out + kPackedEnvelopeBytes;
    WriteU32(p + 0, payload.structSize);
    WriteU32(p + 4, payload.bucketFrames);
    WriteU32(p + 8, payload.channelMode);
    WriteU32(p + 12, payload.reductionMethod);
    WriteU64(p + 16, payload.sourceFrameOrigin);
    WriteU64(p + 24, payload.sourceFrameBegin);
    WriteU64(p + 32, payload.sourceFrameEnd);
    WriteU64(p + 40, payload.sourceSequenceFirst);
    WriteU64(p + 48, payload.sourceSequenceLast);
    WriteU32(p + 56, payload.algorithmId);
    WriteU32(p + 60, payload.algorithmVersion);
    WriteU32(p + 64, payload.unitId);
    WriteU32(p + 68, payload.toleranceBits);
    WriteU32(p + 72, payload.partialDiscard);
    WriteU32(p + 76, payload.reserved0);
    std::uint32_t minBits = 0;
    std::uint32_t maxBits = 0;
    std::memcpy(&minBits, &minValue, sizeof(minBits));
    std::memcpy(&maxBits, &maxValue, sizeof(maxBits));
    WriteU32(p + 80, minBits);
    WriteU32(p + 84, maxBits);
    return bytes;
}

inline std::uint32_t PackScopeResultRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t tapDescriptorId, std::uint64_t tapDescriptorGeneration,
    const VnMonitorScopeResult& payload, std::uint32_t validityFlags) {
    const std::uint32_t bytes = PackedScopeResultBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 144u,
                        VN_MONITOR_RECORD_SCOPE_RESULT, validityFlags);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = tapDescriptorId;
    header.instanceGeneration = tapDescriptorGeneration;
    header.measurementSegmentGeneration = payload.measurementSegmentGeneration;
    PackHeader(out, header);
    std::uint8_t* p = out + kPackedEnvelopeBytes;
    WriteU32(p + 0, payload.structSize);
    WriteU32(p + 4, payload.edge);
    WriteU32(p + 8, payload.channelMode);
    WriteU32(p + 12, payload.channelIndex);
    WriteU32(p + 16, payload.preTriggerFrames);
    WriteU32(p + 20, payload.postTriggerFrames);
    WriteU32(p + 24, payload.maxSearchFrames);
    WriteU32(p + 28, payload.thresholdBits);
    WriteU32(p + 32, payload.hysteresisBits);
    WriteU32(p + 36, payload.executedPreTriggerFrames);
    WriteU32(p + 40, payload.executedPostTriggerFrames);
    WriteU32(p + 44, payload.executedMaxSearchFrames);
    WriteU32(p + 48, payload.executedEdge);
    WriteU32(p + 52, payload.executedThresholdBits);
    WriteU32(p + 56, payload.executedHysteresisBits);
    WriteU32(p + 60, payload.executedChannelMode);
    WriteU32(p + 64, payload.noTrigger);
    WriteU32(p + 68, payload.discontinuityFlags);
    WriteU64(p + 72, payload.triggerFrame);
    WriteU64(p + 80, payload.windowBegin);
    WriteU64(p + 88, payload.windowEnd);
    WriteU64(p + 96, payload.engineMonitorInstanceId);
    WriteU64(p + 104, payload.streamId);
    WriteU64(p + 112, payload.measurementSegmentGeneration);
    WriteU64(p + 120, payload.triggerSourceSequence);
    WriteU64(p + 128, payload.capturePlanGeneration);
    WriteU64(p + 136, payload.tapDescriptorGeneration);
    return bytes;
}

inline std::uint32_t PackSpectrumRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t tapDescriptorId, std::uint64_t tapDescriptorGeneration,
    const VnMonitorSpectrumPayload& payload, const float* bins,
    std::uint32_t binCount, std::uint32_t validityFlags) {
    if (bins == nullptr || binCount == 0 || binCount != payload.binCount) {
        return 0;
    }
    const std::uint32_t bytes = PackedSpectrumBytes(binCount);
    if (out == nullptr || bytes == 0 || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    const std::uint32_t payloadBytes =
        kPackedSpectrumPayloadBytes + binCount * 4u;
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, payloadBytes,
                        VN_MONITOR_RECORD_SPECTRUM, validityFlags);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = tapDescriptorId;
    header.instanceGeneration = tapDescriptorGeneration;
    PackHeader(out, header);
    std::uint8_t* p = out + kPackedEnvelopeBytes;
    WriteU32(p + 0, payload.structSize);
    WriteU32(p + 4, payload.fftSize);
    WriteU32(p + 8, payload.windowKind);
    WriteU32(p + 12, payload.overlapNumerator);
    WriteU32(p + 16, payload.overlapDenominator);
    WriteU32(p + 20, payload.binCount);
    WriteU32(p + 24, payload.algorithmId);
    WriteU32(p + 28, payload.algorithmVersion);
    WriteU32(p + 32, payload.unitId);
    WriteU32(p + 36, payload.toleranceBits);
    WriteU32(p + 40, payload.sampleRate);
    WriteU32(p + 44, payload.channelMode);
    for (std::uint32_t i = 0; i < binCount; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &bins[i], sizeof(bits));
        WriteU32(p + kPackedSpectrumPayloadBytes + i * 4u, bits);
    }
    return bytes;
}

inline std::uint32_t PackMeterRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t tapDescriptorId, std::uint64_t tapDescriptorGeneration,
    const VnMonitorMeterPayload& payload, std::uint32_t validityFlags) {
    const std::uint32_t bytes = PackedMeterBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 64u,
                        VN_MONITOR_RECORD_METER, validityFlags);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = tapDescriptorId;
    header.instanceGeneration = tapDescriptorGeneration;
    PackHeader(out, header);
    std::uint8_t* p = out + kPackedEnvelopeBytes;
    WriteU32(p + 0, payload.structSize);
    WriteU32(p + 4, payload.algorithmId);
    WriteU32(p + 8, payload.algorithmVersion);
    WriteU32(p + 12, payload.lufsSupported);
    WriteU32(p + 16, payload.samplePeakBits);
    WriteU32(p + 20, payload.truePeakBits);
    WriteU32(p + 24, payload.rmsBits);
    WriteU32(p + 28, payload.holdBits);
    WriteU32(p + 32, payload.samplePeakUnitId);
    WriteU32(p + 36, payload.truePeakUnitId);
    WriteU32(p + 40, payload.rmsUnitId);
    WriteU32(p + 44, payload.holdUnitId);
    WriteU32(p + 48, payload.lufsMomentaryBits);
    WriteU32(p + 52, payload.lufsShortTermBits);
    WriteU32(p + 56, payload.lufsIntegratedBits);
    WriteU32(p + 60, payload.reserved0);
    return bytes;
}

inline std::uint32_t PackGainReductionRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    std::uint64_t tapDescriptorId, std::uint64_t tapDescriptorGeneration,
    const VnMonitorGainReductionPayload& payload, std::uint32_t validityFlags) {
    const std::uint32_t bytes = PackedGainReductionBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 48u,
                        VN_MONITOR_RECORD_GAIN_REDUCTION, validityFlags);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    header.entityKind = VN_MONITOR_ENTITY_TAP;
    header.stableId = {};
    header.instanceHandle = tapDescriptorId;
    header.instanceGeneration = tapDescriptorGeneration;
    PackHeader(out, header);
    std::uint8_t* p = out + kPackedEnvelopeBytes;
    WriteU32(p + 0, payload.structSize);
    WriteU32(p + 4, payload.algorithmId);
    WriteU32(p + 8, payload.algorithmVersion);
    WriteU32(p + 12, payload.reductionDbBits);
    WriteObjectId(p + 16, payload.ownerStableId);
    WriteU64(p + 32, payload.ownerInstanceHandle);
    WriteU64(p + 40, payload.ownerGeneration);
    return bytes;
}

inline std::uint32_t PackAggregateRecord(
    std::uint8_t* out, std::uint32_t capacity, std::uint64_t sourceSequence,
    std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
    const VnMonitorAggregatePayload& payload, std::uint32_t validityFlags) {
    const std::uint32_t bytes = PackedAggregateBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    if (payload.firstSourceSequence == 0 && payload.lastSourceSequence == 0) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 40u,
                        VN_MONITOR_RECORD_AGGREGATE, validityFlags);
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.capturePlanGeneration = capturePlanGeneration;
    header.executeFrame = executeFrame;
    PackHeader(out, header);
    std::uint8_t* p = out + kPackedEnvelopeBytes;
    WriteU32(p + 0, payload.structSize);
    WriteU32(p + 4, payload.reason);
    WriteU64(p + 8, payload.firstSourceSequence);
    WriteU64(p + 16, payload.lastSourceSequence);
    WriteU64(p + 24, payload.recordCount);
    WriteU64(p + 32, payload.byteCount);
    return bytes;
}

inline std::uint32_t PackDropSummaryRecord(std::uint8_t* out,
                                           std::uint32_t capacity,
                                           const VnMonitorDropSummary& summary,
                                           std::uint64_t sourceSequence) {
    const std::uint32_t bytes = PackedDropSummaryBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, sizeof(summary),
                        VN_MONITOR_RECORD_DROP_SUMMARY, 0);
    PackHeader(out, header);
    VnMonitorDropSummary packed = summary;
    packed.structSize = sizeof(packed);
    std::memcpy(out + kPackedEnvelopeBytes, &packed, sizeof(packed));
    return bytes;
}

inline std::uint32_t PackControlRecord(std::uint8_t* out, std::uint32_t capacity,
                                       std::uint64_t sourceSequence,
                                       std::uint32_t recordKind,
                                       std::uint32_t streamKind,
                                       std::uint64_t streamId,
                                       std::uint64_t sessionEpoch) {
    const std::uint32_t bytes = PackedControlRecordBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillSyntheticHeader(&header, sourceSequence, bytes, 0, recordKind, 0);
    header.flags = 0;
    header.streamKind = streamKind;
    header.streamId = streamId;
    header.monitorSessionEpoch = sessionEpoch;
    PackHeader(out, header);
    return bytes;
}

inline void FillStateCutHeader(VnMonitorRecordHeader* header,
                               std::uint64_t sourceSequence,
                               std::uint64_t recordBytes,
                               std::uint64_t payloadBytes,
                               std::uint32_t recordKind,
                               std::uint64_t sessionEpoch) {
    FillSyntheticHeader(header, sourceSequence, recordBytes, payloadBytes,
                        recordKind, 0);
    header->flags = 0;
    header->streamKind = VN_MONITOR_STREAM_STATE;
    header->streamId = kSyntheticStateStreamId;
    header->monitorSessionEpoch = sessionEpoch;
}

inline std::uint32_t PackStateCutBegin(std::uint8_t* out, std::uint32_t capacity,
                                       std::uint64_t sourceSequence,
                                       std::uint64_t stateCutId,
                                       std::uint64_t sessionEpoch) {
    const std::uint32_t bytes = PackedStateCutBeginBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillStateCutHeader(&header, sourceSequence, bytes, sizeof(VnMonitorStateCutBegin),
                       VN_MONITOR_RECORD_STATE_CUT_BEGIN, sessionEpoch);
    PackHeader(out, header);
    VnMonitorStateCutBegin begin{};
    begin.structSize = sizeof(begin);
    begin.stateCutId = stateCutId;
    std::memcpy(out + kPackedEnvelopeBytes, &begin, sizeof(begin));
    return bytes;
}

inline std::uint32_t PackStateCutEnd(std::uint8_t* out, std::uint32_t capacity,
                                     std::uint64_t sourceSequence,
                                     std::uint64_t stateCutId,
                                     std::uint64_t sessionEpoch,
                                     std::uint32_t result,
                                     std::uint64_t checksum) {
    const std::uint32_t bytes = PackedStateCutEndBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillStateCutHeader(&header, sourceSequence, bytes, sizeof(VnMonitorStateCutEnd),
                       VN_MONITOR_RECORD_STATE_CUT_END, sessionEpoch);
    PackHeader(out, header);
    VnMonitorStateCutEnd end{};
    end.structSize = sizeof(end);
    end.result = result;
    end.stateCutId = stateCutId;
    end.checksumAlgorithm = VN_MONITOR_CHECKSUM_CRC32C;
    end.checksum = checksum;
    std::memcpy(out + kPackedEnvelopeBytes, &end, sizeof(end));
    return bytes;
}

inline std::uint32_t PackStreamBaseline(std::uint8_t* out, std::uint32_t capacity,
                                        std::uint64_t sourceSequence,
                                        std::uint32_t streamKind,
                                        std::uint64_t streamId,
                                        std::uint64_t sessionEpoch) {
    return PackControlRecord(out, capacity, sourceSequence,
                             VN_MONITOR_RECORD_STREAM_BASELINE, streamKind,
                             streamId, sessionEpoch);
}

inline std::uint64_t EmptyStateCutChecksum(std::uint64_t stateCutId) {
    return Crc32C(&stateCutId, sizeof(stateCutId));
}

struct LifecycleEnvelope {
    std::uint64_t sourceSequence = 0;
    std::uint64_t worldGeneration = 0;
    std::uint64_t executeFrame = 0;
    VnObjectId stableId{};
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint64_t playableInstanceHandle = 0;
    std::uint64_t actionInstanceHandle = 0;
    std::uint32_t entityKind = VN_MONITOR_ENTITY_NONE;
    std::uint32_t reason = 0;
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t runtimeLineageFingerprint = 0;
    std::uint64_t optionalSymbolFingerprint = 0;
    std::uint64_t renderPlanGeneration = 0;
    std::uint64_t clockAnchorGeneration = 0;
    std::uint64_t arrangingInstance = 0;
    VnObjectId clipId{};
    std::uint32_t loopIteration = 0;
    std::uint32_t occurrenceOrdinal = 0;
    std::uint64_t logicCommitSequence = 0;
    float volumeDb = 0.0f;
    bool volumeKnown = false;
    std::uint32_t notifyType = 0;
    std::uint32_t mute01 = 0;
    std::uint32_t solo01 = 0;
    std::uint32_t bypass01 = 0;
    std::uint32_t latencyFrames = 0;
    std::uint32_t tailFrames = 0;
    VnObjectId routeBusId{};
    std::uint32_t validityFlags = 0;
};

inline void FillLifecycleHeader(VnMonitorRecordHeader* header,
                                const LifecycleEnvelope& envelope,
                                std::uint64_t recordBytes,
                                std::uint64_t payloadBytes,
                                std::uint32_t recordKind,
                                std::uint32_t streamKind) {
    FillSyntheticHeader(header, envelope.sourceSequence, recordBytes,
                        payloadBytes, recordKind, 0);
    header->flags = VN_MONITOR_RECORD_FLAG_REQUIRED;
    header->streamKind = streamKind;
    header->streamId = 1;
    header->producerId = 3;
    header->worldGeneration = envelope.worldGeneration;
    header->executeFrame = envelope.executeFrame;
    header->entityKind = envelope.entityKind;
    header->programSemanticFingerprint = envelope.programSemanticFingerprint;
    header->runtimeLineageFingerprint = envelope.runtimeLineageFingerprint;
    header->optionalSymbolFingerprint = envelope.optionalSymbolFingerprint;
    header->renderPlanGeneration = envelope.renderPlanGeneration;
    header->clockAnchorGeneration = envelope.clockAnchorGeneration;
    header->stableId = envelope.stableId;
    header->instanceHandle = envelope.instanceHandle;
    header->instanceGeneration = envelope.instanceGeneration;
    header->playableInstanceHandle = envelope.playableInstanceHandle;
    header->actionInstanceHandle = envelope.actionInstanceHandle;
    header->reason = envelope.reason;
    if (envelope.entityKind == VN_MONITOR_ENTITY_VOICE) {
        header->validityFlags = envelope.volumeKnown
                                    ? VN_MONITOR_VALID_NUMERIC
                                    : VN_MONITOR_VALID_UNSUPPORTED;
    } else {
        header->validityFlags = envelope.validityFlags;
    }
    header->telemetry.arrangingInstance = envelope.arrangingInstance;
    header->telemetry.clipId = envelope.clipId;
    header->telemetry.loopIteration = envelope.loopIteration;
    header->telemetry.occurrenceOrdinal = envelope.occurrenceOrdinal;
    header->telemetry.logicCommitSequence = envelope.logicCommitSequence;
    header->telemetry.executeFrame = envelope.executeFrame;
}

inline std::uint32_t PackLifecycleEvent(std::uint8_t* out, std::uint32_t capacity,
                                        const LifecycleEnvelope& envelope,
                                        std::uint32_t eventClass) {
    const std::uint32_t bytes = PackedEventBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillLifecycleHeader(&header, envelope, bytes, sizeof(VnMonitorEventPayload),
                        VN_MONITOR_RECORD_EVENT, VN_MONITOR_STREAM_EVENT);
    PackHeader(out, header);
    VnMonitorEventPayload payload{};
    payload.structSize = sizeof(payload);
    payload.eventClass = eventClass;
    payload.reason = envelope.reason;
    payload.playableInstanceHandle = envelope.playableInstanceHandle;
    payload.actionInstanceHandle = envelope.actionInstanceHandle;
    if (envelope.entityKind == VN_MONITOR_ENTITY_VOICE) {
        payload.voiceInstanceHandle = envelope.instanceHandle;
    }
    std::memcpy(out + kPackedEnvelopeBytes, &payload, sizeof(payload));
    return bytes;
}

inline std::uint32_t PackLifecycleUpsert(std::uint8_t* out, std::uint32_t capacity,
                                         const LifecycleEnvelope& envelope) {
    const std::uint32_t bytes = PackedStateUpsertBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillLifecycleHeader(&header, envelope, bytes, sizeof(VnMonitorStateUpsert),
                        VN_MONITOR_RECORD_STATE_UPSERT, VN_MONITOR_STREAM_STATE);
    PackHeader(out, header);
    VnMonitorStateUpsert payload{};
    payload.structSize = sizeof(payload);
    payload.entityKind = envelope.entityKind;
    payload.stableId = envelope.stableId;
    payload.instanceHandle = envelope.instanceHandle;
    payload.instanceGeneration = envelope.instanceGeneration;
    payload.volumeDb = envelope.volumeKnown ? envelope.volumeDb : 0.0f;
    payload.mute01 = envelope.mute01;
    payload.solo01 = envelope.solo01;
    payload.bypass01 = envelope.bypass01;
    payload.latencyFrames = envelope.latencyFrames;
    payload.tailFrames = envelope.tailFrames;
    payload.routeBusId = envelope.routeBusId;
    std::memcpy(out + kPackedEnvelopeBytes, &payload, sizeof(payload));
    return bytes;
}

inline std::uint32_t PackLifecycleTombstone(std::uint8_t* out,
                                            std::uint32_t capacity,
                                            const LifecycleEnvelope& envelope) {
    const std::uint32_t bytes = PackedStateTombstoneBytes();
    if (out == nullptr || capacity < bytes) {
        return 0;
    }
    std::memset(out, 0, bytes);
    VnMonitorRecordHeader header{};
    FillLifecycleHeader(&header, envelope, bytes,
                        sizeof(VnMonitorStateTombstone),
                        VN_MONITOR_RECORD_STATE_TOMBSTONE,
                        VN_MONITOR_STREAM_STATE);
    PackHeader(out, header);
    VnMonitorStateTombstone payload{};
    payload.structSize = sizeof(payload);
    payload.entityKind = envelope.entityKind;
    payload.stableId = envelope.stableId;
    payload.instanceHandle = envelope.instanceHandle;
    payload.instanceGeneration = envelope.instanceGeneration;
    payload.reason = envelope.reason;
    std::memcpy(out + kPackedEnvelopeBytes, &payload, sizeof(payload));
    return bytes;
}

} // namespace monitor
} // namespace harken

#endif
