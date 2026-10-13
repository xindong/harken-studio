#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"

#include "harken/core/MonitorWaveformReduce.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace harken {
namespace monitor {

namespace {

constexpr std::size_t kPackedSourceSequenceOffset = 40;

std::uint32_t ReadPackedU32(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadPackedU64(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(ReadPackedU32(bytes, offset)) |
           (static_cast<std::uint64_t>(ReadPackedU32(bytes, offset + 4)) << 32);
}

struct WaveformSlot {
    core::MonitorWaveformReducer reducer{};
    std::uint64_t tapId = 0;
    std::uint64_t tapGeneration = 0;
    std::uint64_t capturePlanGeneration = 0;
    std::uint32_t bucketFrames = 0;
    std::uint32_t channelMode = 0;
    bool occupied = false;
};

thread_local std::array<WaveformSlot, VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN>
    g_slots{};
thread_local std::array<std::uint8_t, 512> g_pack{};
thread_local std::array<core::MonitorWaveformBucketResult,
                        core::kWaveformBucketResultCap>
    g_results{};

void EmitBucket(const VnMonitorMeasurementRequest& request,
                const VnMonitorTapDescriptor& tap,
                const core::MonitorWaveformBucketResult& item,
                std::uint64_t sourceSequence,
                std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
                DerivedEmitFn emit, void* emitCtx) {
    if (emit == nullptr || (!item.complete && !item.discarded)) {
        return;
    }
    VnMonitorWaveformBucket payload{};
    payload.structSize = 88;
    payload.bucketFrames = request.bucketFrames;
    payload.channelMode = request.channelMode;
    payload.sourceFrameOrigin = item.sourceFrameBegin;
    payload.sourceFrameBegin = item.sourceFrameBegin;
    payload.sourceFrameEnd = item.sourceFrameEnd;
    payload.sourceSequenceFirst = item.sourceSequenceFirst;
    payload.sourceSequenceLast = item.sourceSequenceLast;
    payload.partialDiscard = item.discarded ? 1u : 0u;
    const std::uint32_t kind = item.complete
                                   ? VN_MONITOR_RECORD_WAVEFORM_BUCKET
                                   : VN_MONITOR_RECORD_PARTIAL_BUCKET_DISCARDED;
    const std::uint32_t validity =
        item.complete ? (VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC)
                      : VN_MONITOR_VALID_ARMED;
    const std::uint32_t bytes = PackWaveformBucketRecord(
        g_pack.data(), static_cast<std::uint32_t>(g_pack.size()), sourceSequence,
        capturePlanGeneration, executeFrame, tap.tapDescriptorId,
        tap.tapDescriptorGeneration, payload, item.minValue, item.maxValue, kind,
        validity);
    if (bytes == 0) {
        return;
    }
    DerivedEmit emitRec{};
    emitRec.bytes = g_pack.data();
    emitRec.size = bytes;
    (void)emit(emitCtx, emitRec);
}

VnResult ValidateWaveform(const VnMonitorMeasurementRequest& request,
                          const VnMonitorTapDescriptor&) {
    return core::ValidateWaveformRequest(request);
}

void ConsumeWaveform(const VnMonitorMeasurementRequest& request,
                     const VnMonitorTapDescriptor& tap,
                     const std::uint8_t* packedAudioChunk,
                     std::uint32_t packedBytes, std::uint32_t measurementIndex,
                     DerivedEmitFn emit, void* emitCtx) {
    if (emit == nullptr || packedAudioChunk == nullptr ||
        packedBytes < VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES +
                          sizeof(VnMonitorAudioChunk) ||
        measurementIndex >= g_slots.size()) {
        return;
    }
    VnMonitorAudioChunk chunk{};
    std::memcpy(&chunk, packedAudioChunk + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES,
                sizeof(chunk));
    if (chunk.structSize != sizeof(chunk) || chunk.channelCount == 0 ||
        chunk.sourceFrameEnd <= chunk.sourceFrameBegin) {
        return;
    }
    if (chunk.tapDescriptorId != tap.tapDescriptorId ||
        chunk.tapDescriptorGeneration != tap.tapDescriptorGeneration) {
        return;
    }
    const std::uint32_t frames = static_cast<std::uint32_t>(
        chunk.sourceFrameEnd - chunk.sourceFrameBegin);
    const float* samples = reinterpret_cast<const float*>(
        packedAudioChunk + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES +
        sizeof(VnMonitorAudioChunk));
    const std::uint64_t sourceSequence =
        ReadPackedU64(packedAudioChunk, kPackedSourceSequenceOffset);
    const std::uint64_t capturePlanGeneration =
        ReadPackedU64(packedAudioChunk, kPackedCapturePlanGenerationOffset);
    const std::uint64_t executeFrame =
        ReadPackedU64(packedAudioChunk, kPackedExecuteFrameOffset);
    const std::uint64_t segmentGeneration = ReadPackedU64(
        packedAudioChunk, kPackedMeasurementSegmentGenerationOffset);

    WaveformSlot& slot = g_slots[measurementIndex];
    if (!slot.occupied || slot.tapId != tap.tapDescriptorId ||
        slot.tapGeneration != tap.tapDescriptorGeneration ||
        slot.capturePlanGeneration != capturePlanGeneration ||
        slot.bucketFrames != request.bucketFrames ||
        slot.channelMode != request.channelMode) {
        if (slot.occupied && slot.reducer.filledCount != 0) {
            const core::MonitorWaveformBucketResult discarded =
                core::FlushWaveformPartial(&slot.reducer);
            EmitBucket(request, tap, discarded, discarded.sourceSequenceLast,
                       capturePlanGeneration, executeFrame, emit, emitCtx);
        }
        slot.reducer.Reset(request.bucketFrames, request.channelMode,
                           segmentGeneration);
        slot.tapId = tap.tapDescriptorId;
        slot.tapGeneration = tap.tapDescriptorGeneration;
        slot.capturePlanGeneration = capturePlanGeneration;
        slot.bucketFrames = request.bucketFrames;
        slot.channelMode = request.channelMode;
        slot.occupied = true;
    } else {
        core::MonitorWaveformBucketResult discarded{};
        core::NoteWaveformSegment(&slot.reducer, segmentGeneration, &discarded);
        if (discarded.discarded) {
            EmitBucket(request, tap, discarded, discarded.sourceSequenceLast,
                       capturePlanGeneration, executeFrame, emit, emitCtx);
        }
    }

    std::uint32_t count = 0;
    (void)core::ReduceWaveformChunk(
        &slot.reducer, samples, frames, chunk.channelCount, chunk.sourceFrameBegin,
        g_results.data(), static_cast<std::uint32_t>(g_results.size()), &count,
        sourceSequence);
    for (std::uint32_t i = 0; i < count; ++i) {
        EmitBucket(request, tap, g_results[i], sourceSequence,
                   capturePlanGeneration, executeFrame, emit, emitCtx);
    }
}

struct WaveformAutoRegister {
    WaveformAutoRegister() {
        DerivedHandler handler{};
        handler.validate = &ValidateWaveform;
        handler.consume = &ConsumeWaveform;
        RegisterDerivedHandler(VN_MONITOR_MEASURE_WAVEFORM, handler);
    }
};

const WaveformAutoRegister kRegister{};

} // namespace

void ForceLinkMonitorWaveformHandler() {}

} // namespace monitor
} // namespace harken
