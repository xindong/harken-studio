#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"

#include "harken/core/MonitorMeterReduce.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace harken {
namespace monitor {

using GainLookupFn = bool (*)(const VnMonitorTapDescriptor& tap,
                              float* reductionDb, VnObjectId* ownerStableId,
                              std::uint64_t* ownerInstanceHandle,
                              std::uint64_t* ownerGeneration, void* ctx);
thread_local GainLookupFn g_grLookup = nullptr;
thread_local void* g_grLookupCtx = nullptr;

namespace {

constexpr std::size_t kPackedSourceSequenceOffset = 40;
static_assert(kPackedMeasurementSegmentGenerationOffset == 120);
static_assert(kPackedClockAnchorGenerationOffset == 112);
static_assert(kPackedExecuteFrameOffset == 128);

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

struct MeterSlot {
    core::MonitorMeterState state{};
    core::MonitorLoudnessState loudness{};
    std::uint64_t tapId = 0;
    std::uint64_t tapGeneration = 0;
    std::uint64_t capturePlanGeneration = 0;
    std::uint32_t channelMode = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t layout = 0;
    std::uint32_t kind = 0;
    std::uint64_t sourceFrameEnd = 0;
    bool occupied = false;
};

thread_local std::array<MeterSlot, VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN>
    g_slots{};
thread_local std::array<std::uint8_t, 512> g_pack{};

VnMonitorMeterPayload MakeMeterPayload(const core::MonitorMeterChunkResult& item,
                                       std::uint32_t kind) {
    VnMonitorMeterPayload payload{};
    payload.structSize = 64;
    payload.algorithmId = kind;
    payload.algorithmVersion = core::kMeterAlgorithmVersion;
    payload.lufsSupported = 0;
    if (kind == VN_MONITOR_MEASURE_SAMPLE_PEAK) {
        payload.samplePeakBits = core::MeterFloatBits(item.samplePeak);
        payload.samplePeakUnitId = core::kMeterUnitLinear;
    } else if (kind == VN_MONITOR_MEASURE_TRUE_PEAK) {
        payload.truePeakBits =
            core::MeterFloatBits(core::MeterLinearToDb(item.truePeak));
        payload.truePeakUnitId = core::kMeterUnitDbtp;
    } else if (kind == VN_MONITOR_MEASURE_RMS) {
        payload.rmsBits = core::MeterFloatBits(item.rmsLinear);
        payload.rmsUnitId = core::kMeterUnitLinear;
    } else if (kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY) {
        payload.lufsSupported = 1;
        payload.lufsMomentaryBits = core::MeterFloatBits(item.loudnessMomentary);
    } else if (kind == VN_MONITOR_MEASURE_PEAK_HOLD) {
        payload.holdBits =
            core::MeterFloatBits(core::MeterLinearToDb(item.holdLinear));
        payload.holdUnitId = core::kMeterUnitDbfs;
    }
    return payload;
}

std::uint32_t ValidityFor(const core::MonitorMeterChunkResult& item,
                          std::uint32_t kind) {
    if (item.invalid || !item.numeric) {
        return VN_MONITOR_VALID_ARMED;
    }
    if (kind == VN_MONITOR_MEASURE_RMS && item.rmsWarmup) {
        return VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_WARMUP;
    }
    if (kind == VN_MONITOR_MEASURE_TRUE_PEAK && item.truePeakWarmup) {
        return VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_WARMUP;
    }
    if (kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY && item.loudnessWarmup) {
        return VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_WARMUP;
    }
    return VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC;
}

void EmitMeter(std::uint32_t kind, const core::MonitorMeterChunkResult& item,
               const VnMonitorTapDescriptor& tap, std::uint64_t sourceSequence,
               std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
               DerivedEmitFn emit, void* emitCtx) {
    if (emit == nullptr) {
        return;
    }
    if ((item.invalid || !item.numeric) && kind != VN_MONITOR_MEASURE_LUFS_MOMENTARY) {
        return;
    }
    VnMonitorMeterPayload payload = MakeMeterPayload(item, kind);
    const std::uint32_t bytes = PackMeterRecord(
        g_pack.data(), static_cast<std::uint32_t>(g_pack.size()), sourceSequence,
        capturePlanGeneration, executeFrame, tap.tapDescriptorId,
        tap.tapDescriptorGeneration, payload, ValidityFor(item, kind));
    if (bytes == 0) {
        return;
    }
    DerivedEmit emitRec{};
    emitRec.bytes = g_pack.data();
    emitRec.size = bytes;
    (void)emit(emitCtx, emitRec);
}

void EmitGainReduction(const VnMonitorTapDescriptor& tap,
                       const core::MonitorMeterGainReductionSample& sample,
                       std::uint64_t sourceSequence,
                       std::uint64_t capturePlanGeneration,
                       std::uint64_t executeFrame, DerivedEmitFn emit,
                       void* emitCtx) {
    if (emit == nullptr || !sample.valid) {
        return;
    }
    VnMonitorGainReductionPayload payload{};
    payload.structSize = 40;
    payload.algorithmId = VN_MONITOR_MEASURE_GAIN_REDUCTION;
    payload.algorithmVersion = core::kMeterAlgorithmVersion;
    payload.reductionDbBits = core::MeterFloatBits(sample.reductionDb);
    payload.ownerStableId = sample.ownerStableId;
    payload.ownerInstanceHandle = sample.ownerInstanceHandle;
    payload.ownerGeneration = sample.ownerGeneration;
    const std::uint32_t bytes = PackGainReductionRecord(
        g_pack.data(), static_cast<std::uint32_t>(g_pack.size()), sourceSequence,
        capturePlanGeneration, executeFrame, tap.tapDescriptorId,
        tap.tapDescriptorGeneration, payload,
        VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC);
    if (bytes == 0) {
        return;
    }
    DerivedEmit emitRec{};
    emitRec.bytes = g_pack.data();
    emitRec.size = bytes;
    (void)emit(emitCtx, emitRec);
}

VnResult ValidateMeter(const VnMonitorMeasurementRequest& request,
                       const VnMonitorTapDescriptor&) {
    return core::ValidateMeterRequest(request);
}

void ConsumeMeter(const VnMonitorMeasurementRequest& request,
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
    if (core::ValidateMeterRequest(request) != VN_OK) {
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

    MeterSlot& slot = g_slots[measurementIndex];
    if (!slot.occupied || slot.tapId != tap.tapDescriptorId ||
        slot.tapGeneration != tap.tapDescriptorGeneration ||
        slot.capturePlanGeneration != capturePlanGeneration ||
        slot.channelMode != request.channelMode ||
        slot.kind != request.kind || slot.layout != chunk.layout ||
        slot.sourceFrameEnd != chunk.sourceFrameBegin ||
        slot.sampleRate != chunk.sampleRate) {
        core::ResetMeterState(&slot.state, chunk.sampleRate, segmentGeneration);
        if (request.kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY) {
            core::ResetLoudnessState(&slot.loudness, chunk.sampleRate, chunk.layout,
                                     segmentGeneration);
        }
        slot.tapId = tap.tapDescriptorId;
        slot.tapGeneration = tap.tapDescriptorGeneration;
        slot.capturePlanGeneration = capturePlanGeneration;
        slot.channelMode = request.channelMode;
        slot.sampleRate = chunk.sampleRate;
        slot.kind = request.kind;
        slot.layout = chunk.layout;
        slot.occupied = true;
    } else {
        core::NoteMeterSegment(&slot.state, segmentGeneration);
        if (request.kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY &&
            slot.loudness.segmentGeneration != segmentGeneration) {
            core::ResetLoudnessState(&slot.loudness, chunk.sampleRate, chunk.layout,
                                     segmentGeneration);
        }
    }
    slot.sourceFrameEnd = chunk.sourceFrameEnd;

    if (request.kind == VN_MONITOR_MEASURE_GAIN_REDUCTION) {
        core::MonitorMeterGainReductionSample sample{};
        if (g_grLookup != nullptr && core::MeterHasIdentity(tap)) {
            float reductionDb = 0.0f;
            VnObjectId ownerStableId{};
            std::uint64_t ownerInstanceHandle = 0;
            std::uint64_t ownerGeneration = 0;
            if (g_grLookup(tap, &reductionDb, &ownerStableId, &ownerInstanceHandle,
                           &ownerGeneration, g_grLookupCtx)) {
                VnMonitorTapDescriptor identity = tap;
                identity.ownerStableId = ownerStableId;
                identity.ownerInstanceHandle = ownerInstanceHandle;
                identity.ownerGeneration = ownerGeneration;
                sample = core::ResolveMeterGainReduction(
                    identity,
                    [](const VnMonitorTapDescriptor& resolved, void* raw) {
                        core::MonitorMeterGainReductionSample out{};
                        out.valid = true;
                        out.reductionDb = *static_cast<float*>(raw);
                        out.ownerStableId = resolved.ownerStableId;
                        out.ownerInstanceHandle = resolved.ownerInstanceHandle;
                        out.ownerGeneration = resolved.ownerGeneration;
                        return out;
                    },
                    &reductionDb);
            }
        }
        EmitGainReduction(tap, sample, sourceSequence, capturePlanGeneration,
                          executeFrame, emit, emitCtx);
        return;
    }

    const core::MonitorMeterChunkResult item = request.kind == VN_MONITOR_MEASURE_LUFS_MOMENTARY
        ? core::ReduceLoudnessChunk(&slot.loudness, samples, frames, chunk.channelCount)
        : core::ReduceMeterChunk(&slot.state, samples, frames, chunk.channelCount, request.channelMode);
    EmitMeter(request.kind, item, tap, sourceSequence, capturePlanGeneration,
              executeFrame, emit, emitCtx);
}

struct MeterAutoRegister {
    MeterAutoRegister() {
        DerivedHandler handler{};
        handler.validate = &ValidateMeter;
        handler.consume = &ConsumeMeter;
        RegisterDerivedHandler(VN_MONITOR_MEASURE_SAMPLE_PEAK, handler);
        RegisterDerivedHandler(VN_MONITOR_MEASURE_TRUE_PEAK, handler);
        RegisterDerivedHandler(VN_MONITOR_MEASURE_RMS, handler);
        RegisterDerivedHandler(VN_MONITOR_MEASURE_LUFS_MOMENTARY, handler);
        RegisterDerivedHandler(VN_MONITOR_MEASURE_PEAK_HOLD, handler);
        RegisterDerivedHandler(VN_MONITOR_MEASURE_GAIN_REDUCTION, handler);
    }
};

const MeterAutoRegister kRegister{};

} // namespace

void ForceLinkMonitorMeterHandler() {}

} // namespace monitor
} // namespace harken

namespace harken {
namespace monitor {
namespace meter_test {

void SetGainReductionLookupForTests(
    bool (*lookup)(const VnMonitorTapDescriptor& tap, float* reductionDb,
                   VnObjectId* ownerStableId,
                   std::uint64_t* ownerInstanceHandle,
                   std::uint64_t* ownerGeneration, void* ctx),
    void* ctx) {
    g_grLookup = lookup;
    g_grLookupCtx = ctx;
}

} // namespace meter_test

} // namespace monitor
} // namespace harken
