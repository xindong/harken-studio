#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"

#include "harken/core/MonitorSpectrumFft.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace harken {
namespace monitor {

namespace {

constexpr std::size_t kPackedSourceSequenceOffset = 40;
constexpr std::uint32_t kSpectrumPackCap =
    PackedSpectrumBytes(core::kMonitorSpectrumMaxBins);

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

struct SpectrumSlot {
    core::MonitorSpectrumState state{};
    std::uint64_t tapId = 0;
    std::uint64_t tapGeneration = 0;
    std::uint64_t capturePlanGeneration = 0;
    std::uint32_t fftSize = 0;
    std::uint32_t channelMode = 0;
    std::uint32_t channelIndex = 0;
    std::uint64_t nextSourceFrame = 0;
    bool occupied = false;
};

thread_local std::array<SpectrumSlot, VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN>
    g_slots{};
thread_local std::array<std::uint8_t, kSpectrumPackCap> g_pack{};
thread_local std::array<float, core::kMonitorSpectrumMaxFft> g_workTime{};
thread_local std::array<float, core::kMonitorSpectrumMaxFft * 2u> g_workFft{};

float MixMonoAverage(const float* interleaved, std::uint32_t frame,
                     std::uint32_t channels) {
    double sum = 0.0;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        sum += static_cast<double>(interleaved[frame * channels + ch]);
    }
    return static_cast<float>(sum / static_cast<double>(channels));
}

void EmitSpectrum(const VnMonitorMeasurementRequest& request,
                  const VnMonitorTapDescriptor& tap,
                  const core::MonitorSpectrumWindow& window,
                  std::uint32_t sampleRate, std::uint64_t sourceSequence,
                  std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
                  DerivedEmitFn emit, void* emitCtx) {
    if (emit == nullptr || !window.complete) {
        return;
    }
    VnMonitorSpectrumPayload payload{};
    payload.structSize = 48;
    payload.fftSize = request.fftSize;
    payload.windowKind = 0;
    payload.overlapNumerator = 1;
    payload.overlapDenominator = 2;
    payload.binCount = window.binCount;
    payload.sampleRate = sampleRate;
    payload.channelMode = request.channelMode;
    const std::uint32_t bytes = PackSpectrumRecord(
        g_pack.data(), static_cast<std::uint32_t>(g_pack.size()), sourceSequence,
        capturePlanGeneration, executeFrame, tap.tapDescriptorId,
        tap.tapDescriptorGeneration, payload, window.bins, window.binCount,
        VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC);
    if (bytes == 0) {
        return;
    }
    DerivedEmit emitRec{};
    emitRec.bytes = g_pack.data();
    emitRec.size = bytes;
    (void)emit(emitCtx, emitRec);
}

VnResult ValidateSpectrum(const VnMonitorMeasurementRequest& request,
                          const VnMonitorTapDescriptor&) {
    return core::ValidateSpectrumRequest(request);
}

void ConsumeSpectrum(const VnMonitorMeasurementRequest& request,
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
    if (core::ValidateSpectrumRequest(request) != VN_OK) {
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
    const std::uint32_t channelIndex = 0;
    if (request.channelMode == VN_MONITOR_CHANNEL_PER_CHANNEL &&
        channelIndex >= chunk.channelCount) {
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

    SpectrumSlot& slot = g_slots[measurementIndex];
    const std::uint64_t activeGeneration = DerivedDrainCapturePlanGeneration();
    if (activeGeneration != 0 && capturePlanGeneration != activeGeneration) {
        return;
    }
    if (slot.occupied && capturePlanGeneration < slot.capturePlanGeneration) {
        return;
    }
    const bool identityMismatch =
        !slot.occupied || slot.tapId != tap.tapDescriptorId ||
        slot.tapGeneration != tap.tapDescriptorGeneration ||
        slot.capturePlanGeneration != capturePlanGeneration ||
        slot.fftSize != request.fftSize ||
        slot.channelMode != request.channelMode ||
        slot.channelIndex != channelIndex;
    if (identityMismatch) {
        core::ResetSpectrumState(&slot.state, request.fftSize, segmentGeneration);
        slot.tapId = tap.tapDescriptorId;
        slot.tapGeneration = tap.tapDescriptorGeneration;
        slot.capturePlanGeneration = capturePlanGeneration;
        slot.fftSize = request.fftSize;
        slot.channelMode = request.channelMode;
        slot.channelIndex = channelIndex;
        slot.nextSourceFrame = 0;
        slot.occupied = true;
    } else if (slot.state.segmentGeneration != segmentGeneration) {
        core::ResetSpectrumState(&slot.state, request.fftSize, segmentGeneration);
        slot.nextSourceFrame = 0;
    }

    if (slot.nextSourceFrame != 0 &&
        chunk.sourceFrameBegin != slot.nextSourceFrame) {
        core::ResetSpectrumState(&slot.state, request.fftSize, segmentGeneration);
    }
    slot.nextSourceFrame = chunk.sourceFrameEnd;

    for (std::uint32_t i = 0; i < frames; ++i) {
        float sample = 0.0f;
        if (request.channelMode == VN_MONITOR_CHANNEL_MONO_AVERAGE) {
            sample = MixMonoAverage(samples, i, chunk.channelCount);
        } else {
            sample = samples[i * chunk.channelCount + channelIndex];
        }
        core::MonitorSpectrumWindow window{};
        if (core::PushSpectrumSample(&slot.state, sample,
                                     chunk.sourceFrameBegin + i,
                                     g_workTime.data(), g_workFft.data(),
                                     &window)) {
            EmitSpectrum(request, tap, window, chunk.sampleRate, sourceSequence,
                         capturePlanGeneration, executeFrame, emit, emitCtx);
        }
    }
}

struct SpectrumAutoRegister {
    SpectrumAutoRegister() {
        DerivedHandler handler{};
        handler.validate = &ValidateSpectrum;
        handler.consume = &ConsumeSpectrum;
        RegisterDerivedHandler(VN_MONITOR_MEASURE_SPECTRUM, handler);
    }
};

const SpectrumAutoRegister kRegister{};

void ResetSlots() {
    for (SpectrumSlot& slot : g_slots) {
        slot.occupied = false;
        slot.capturePlanGeneration = 0;
        slot.fftSize = 0;
        slot.nextSourceFrame = 0;
        slot.state.filled = 0;
        slot.state.fftSize = 0;
    }
}

} // namespace

void ForceLinkMonitorSpectrumHandler() {}

void ResetMonitorSpectrumSlotsForTests() { ResetSlots(); }

} // namespace monitor
} // namespace harken
