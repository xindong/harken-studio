#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"

#include "harken/core/MonitorScopeTrigger.h"

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

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct ScopeSlot {
    core::MonitorScopeState state{};
    core::MonitorScopeOverlay overlay{};
    std::uint64_t tapId = 0;
    std::uint64_t tapGeneration = 0;
    std::uint64_t capturePlanGeneration = 0;
    std::uint64_t nextSourceFrame = 0;
    bool occupied = false;
};

struct ScopeScratch {
    std::array<std::uint8_t, 512> pack{};
};

bool SameOverlay(const core::MonitorScopeOverlay& a,
                 const core::MonitorScopeOverlay& b) {
    return a.edge == b.edge && a.threshold == b.threshold &&
           a.hysteresis == b.hysteresis && a.channelMode == b.channelMode &&
           a.channelIndex == b.channelIndex &&
           a.preTriggerFrames == b.preTriggerFrames &&
           a.postTriggerFrames == b.postTriggerFrames &&
           a.maxSearchFrames == b.maxSearchFrames;
}

void FillPayload(VnMonitorScopeResult* payload,
                 const core::MonitorScopeOverlay& overlay,
                 const core::MonitorScopeResult& item,
                 std::uint64_t sourceSequence,
                 std::uint64_t capturePlanGeneration,
                 std::uint64_t tapDescriptorGeneration,
                 std::uint64_t segmentGeneration) {
    std::memset(payload, 0, sizeof(*payload));
    payload->structSize = 144;
    payload->edge = overlay.edge;
    payload->channelMode = overlay.channelMode;
    payload->channelIndex = overlay.channelIndex;
    payload->preTriggerFrames = overlay.preTriggerFrames;
    payload->postTriggerFrames = overlay.postTriggerFrames;
    payload->maxSearchFrames = overlay.maxSearchFrames;
    payload->thresholdBits = FloatBits(overlay.threshold);
    payload->hysteresisBits = FloatBits(overlay.hysteresis);
    payload->executedPreTriggerFrames = overlay.preTriggerFrames;
    payload->executedPostTriggerFrames = overlay.postTriggerFrames;
    payload->executedMaxSearchFrames = overlay.maxSearchFrames;
    payload->executedEdge = overlay.edge;
    payload->executedThresholdBits = payload->thresholdBits;
    payload->executedHysteresisBits = payload->hysteresisBits;
    payload->executedChannelMode = overlay.channelMode;
    payload->noTrigger = item.noTrigger ? 1u : 0u;
    payload->discontinuityFlags = item.discontinuityFlags;
    payload->triggerFrame = item.triggerFrame;
    payload->windowBegin = item.windowBegin;
    payload->windowEnd = item.windowEnd;
    payload->measurementSegmentGeneration = segmentGeneration;
    payload->triggerSourceSequence = sourceSequence;
    payload->capturePlanGeneration = capturePlanGeneration;
    payload->tapDescriptorGeneration = tapDescriptorGeneration;
}

void EmitScope(ScopeScratch& scratch, const core::MonitorScopeOverlay& overlay,
               const core::MonitorScopeResult& item,
               const VnMonitorTapDescriptor& tap, std::uint64_t sourceSequence,
               std::uint64_t capturePlanGeneration, std::uint64_t executeFrame,
               std::uint64_t segmentGeneration, DerivedEmitFn emit,
               void* emitCtx) {
    if (emit == nullptr || (!item.triggered && !item.noTrigger)) {
        return;
    }
    VnMonitorScopeResult payload{};
    FillPayload(&payload, overlay, item, sourceSequence, capturePlanGeneration,
                tap.tapDescriptorGeneration, segmentGeneration);
    const std::uint32_t validity =
        item.triggered ? (VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC)
                       : VN_MONITOR_VALID_ARMED;
    const std::uint32_t bytes = PackScopeResultRecord(
        scratch.pack.data(), static_cast<std::uint32_t>(scratch.pack.size()), sourceSequence,
        capturePlanGeneration, executeFrame, tap.tapDescriptorId,
        tap.tapDescriptorGeneration, payload, validity);
    if (bytes == 0) {
        return;
    }
    DerivedEmit emitRec{};
    emitRec.bytes = scratch.pack.data();
    emitRec.size = bytes;
    (void)emit(emitCtx, emitRec);
}

VnResult ValidateScope(const VnMonitorMeasurementRequest& request,
                       const VnMonitorTapDescriptor&) {
    core::MonitorScopeOverlay overlay{};
    return core::DecodeScopeOverlay(request, &overlay);
}

float MixMonoAverage(const float* interleaved, std::uint32_t frame,
                     std::uint32_t channels) {
    double sum = 0.0;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        sum += static_cast<double>(interleaved[frame * channels + ch]);
    }
    return static_cast<float>(sum / static_cast<double>(channels));
}

void ConsumeScope(void* state, void* workspace,
                     const VnMonitorMeasurementRequest& request,
                  const VnMonitorTapDescriptor& tap,
                  const std::uint8_t* packedAudioChunk,
                  std::uint32_t packedBytes,
                  DerivedEmitFn emit, void* emitCtx) {
    if (emit == nullptr || packedAudioChunk == nullptr ||
        packedBytes < VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES +
                          sizeof(VnMonitorAudioChunk)) {
        return;
    }
    core::MonitorScopeOverlay overlay{};
    if (core::DecodeScopeOverlay(request, &overlay) != VN_OK) {
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
    if (overlay.channelMode == VN_MONITOR_CHANNEL_PER_CHANNEL &&
        overlay.channelIndex >= chunk.channelCount) {
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

    auto& slot = *static_cast<ScopeSlot*>(state);
    auto& scratch = *static_cast<ScopeScratch*>(workspace);
    if (!slot.occupied || slot.tapId != tap.tapDescriptorId ||
        slot.tapGeneration != tap.tapDescriptorGeneration ||
        slot.capturePlanGeneration != capturePlanGeneration ||
        !SameOverlay(slot.overlay, overlay)) {
        core::ResetScopeState(&slot.state, segmentGeneration);
        slot.overlay = overlay;
        slot.tapId = tap.tapDescriptorId;
        slot.tapGeneration = tap.tapDescriptorGeneration;
        slot.capturePlanGeneration = capturePlanGeneration;
        slot.nextSourceFrame = 0;
        slot.occupied = true;
    } else if (slot.state.segmentGeneration != segmentGeneration) {
        core::ResetScopeState(&slot.state, segmentGeneration);
        slot.nextSourceFrame = 0;
    }

    if (slot.nextSourceFrame != 0 &&
        chunk.sourceFrameBegin != slot.nextSourceFrame) {
        core::ResetScopeState(&slot.state, segmentGeneration);
        core::MonitorScopeResult gap{};
        gap.noTrigger = true;
        gap.discontinuityFlags = 1u;
        gap.windowBegin = slot.nextSourceFrame;
        gap.windowEnd = chunk.sourceFrameBegin;
        EmitScope(scratch, overlay, gap, tap, sourceSequence, capturePlanGeneration,
                  executeFrame, segmentGeneration, emit, emitCtx);
    }
    slot.nextSourceFrame = chunk.sourceFrameEnd;

    for (std::uint32_t i = 0; i < frames; ++i) {
        float sample = 0.0f;
        if (overlay.channelMode == VN_MONITOR_CHANNEL_MONO_AVERAGE) {
            sample = MixMonoAverage(samples, i, chunk.channelCount);
        } else {
            sample = samples[i * chunk.channelCount + overlay.channelIndex];
        }
        const core::MonitorScopeResult item = core::FeedScopeSample(
            &slot.state, overlay, sample, chunk.sourceFrameBegin + i);
        EmitScope(scratch, overlay, item, tap, sourceSequence, capturePlanGeneration,
                  executeFrame, segmentGeneration, emit, emitCtx);
    }
}

struct ScopeAutoRegister {
    ScopeAutoRegister() {
        DerivedHandler handler{};
        handler.validate = &ValidateScope;
        handler.consume = &ConsumeScope;
        handler.createState = [](const VnMonitorMeasurementRequest&,
                                 std::uint32_t) -> std::shared_ptr<void> {
            return std::make_shared<ScopeSlot>();
        };
        handler.createScratch = []() -> std::shared_ptr<void> {
            return std::make_shared<ScopeScratch>();
        };
        RegisterDerivedHandler(VN_MONITOR_MEASURE_SCOPE, handler);
    }
};

const ScopeAutoRegister kRegister{};

} // namespace

void ForceLinkMonitorScopeHandler() {}

} // namespace monitor
} // namespace harken
