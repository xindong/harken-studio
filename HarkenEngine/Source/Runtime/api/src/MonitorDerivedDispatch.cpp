#include "MonitorDerivedDispatch.h"

#include <array>
#include <cstdint>

namespace harken {
namespace monitor {

namespace {

constexpr std::uint32_t kHandlerSlots = 16;

std::array<DerivedHandler, kHandlerSlots> g_handlers{};
thread_local std::uint64_t g_activeCapturePlanGeneration = 0;

bool KnownKind(std::uint32_t kind) {
    return kind >= VN_MONITOR_MEASURE_SAMPLE_PEAK &&
           kind <= VN_MONITOR_MEASURE_LUFS_INTEGRATED;
}

std::uint32_t SlotFor(std::uint32_t kind) {
    if (!KnownKind(kind)) {
        return kHandlerSlots;
    }
    return kind;
}

} // namespace

void RegisterDerivedHandler(std::uint32_t kind, DerivedHandler handler) {
    const std::uint32_t slot = SlotFor(kind);
    if (slot >= kHandlerSlots) {
        return;
    }
    g_handlers[slot] = handler;
}

VnResult ValidateDerivedRequest(const VnMonitorMeasurementRequest& request,
                                const VnMonitorTapDescriptor& tap) {
    (void)tap;
    if (request.structSize != sizeof(VnMonitorMeasurementRequest)) {
        return VN_INVALID_ARG;
    }
    if (request.reserved0 != 0 || request.reserved1 != 0) {
        return VN_INVALID_ARG;
    }
    if (!KnownKind(request.kind)) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.kind == VN_MONITOR_MEASURE_LUFS_SHORT_TERM ||
        request.kind == VN_MONITOR_MEASURE_LUFS_INTEGRATED) {
        return VN_MONITOR_UNSUPPORTED;
    }
    const std::uint32_t slot = SlotFor(request.kind);
    if (slot >= kHandlerSlots) {
        return VN_MONITOR_UNSUPPORTED;
    }
    const DerivedHandler& handler = g_handlers[slot];
    if (handler.validate == nullptr) {
        return VN_MONITOR_UNSUPPORTED;
    }
    return handler.validate(request, tap);
}

void DispatchDerivedAudioChunk(const VnMonitorMeasurementRequest* requests,
                               std::uint32_t requestCount,
                               const VnMonitorTapDescriptor* taps,
                               std::uint32_t tapCount,
                               const std::uint8_t* packedAudioChunk,
                               std::uint32_t packedBytes, DerivedEmitFn emit,
                               void* emitCtx,
                               std::uint64_t activeCapturePlanGeneration) {
    struct ActiveGenerationScope {
        explicit ActiveGenerationScope(std::uint64_t generation) {
            g_activeCapturePlanGeneration = generation;
        }
        ~ActiveGenerationScope() { g_activeCapturePlanGeneration = 0; }
    } scope(activeCapturePlanGeneration);
    if (requests == nullptr || requestCount == 0 || packedAudioChunk == nullptr ||
        packedBytes == 0 || taps == nullptr || tapCount == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < requestCount; ++i) {
        const VnMonitorMeasurementRequest& request = requests[i];
        if (request.tapIndex >= tapCount) {
            continue;
        }
        const std::uint32_t slot = SlotFor(request.kind);
        if (slot >= kHandlerSlots) {
            continue;
        }
        const DerivedHandler& handler = g_handlers[slot];
        if (handler.consume == nullptr) {
            continue;
        }
        handler.consume(request, taps[request.tapIndex], packedAudioChunk,
                        packedBytes, i, emit, emitCtx);
    }
}

std::uint64_t DerivedDrainCapturePlanGeneration() {
    return g_activeCapturePlanGeneration;
}

void ResetDerivedHandlersForTests() { g_handlers = {}; }

void ForceLinkMonitorWaveformHandler();
void ForceLinkMonitorScopeHandler();
void ForceLinkMonitorSpectrumHandler();
void ForceLinkMonitorMeterHandler();

namespace {
const int kForceWaveformLink = (ForceLinkMonitorWaveformHandler(), 0);
const int kForceScopeLink = (ForceLinkMonitorScopeHandler(), 0);
const int kForceSpectrumLink = (ForceLinkMonitorSpectrumHandler(), 0);
const int kForceMeterLink = (ForceLinkMonitorMeterHandler(), 0);
}

} // namespace monitor
} // namespace harken
