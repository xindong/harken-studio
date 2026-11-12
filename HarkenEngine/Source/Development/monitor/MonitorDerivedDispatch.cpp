#include "MonitorDerivedDispatch.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <algorithm>
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

bool DerivedDrainState::Prepare(const VnMonitorMeasurementRequest* requests,
                               std::uint32_t requestCount, std::uint32_t sampleRate,
                               const DerivedDrainState* previous) noexcept {
    if (memguard::IsAudioTagged() || requestCount > VN_MONITOR_HARD_MEASUREMENTS_PER_PLAN ||
        (requestCount != 0 && requests == nullptr)) return false;
    try {
        std::vector<Measurement> prepared;
        if (requestCount != 0)
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::MonitorDerivedStorage);
        prepared.reserve(requestCount);
        for (std::uint32_t i = 0; i < requestCount; ++i) {
            const auto index = SlotFor(requests[i].kind);
            if (index >= kHandlerSlots) return false;
            const auto handler = g_handlers[index];
            if (handler.consume == nullptr || handler.createState == nullptr ||
                handler.createScratch == nullptr) return false;
            Measurement measurement{requests[i].kind, handler, {}, {}};
            if (previous != nullptr && previous->sampleRate_ == sampleRate &&
                i < previous->measurements_.size()) {
                const auto& old = previous->measurements_[i];
                if (old.kind == measurement.kind && old.handler.createState == handler.createState)
                    measurement.state = old.state;
            }
            if (!measurement.state) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::MonitorDerivedStorage);
                measurement.state = handler.createState(requests[i], sampleRate);
            }
            const auto findScratch = [&](const auto& entries) {
                return std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                    return entry.handler.createScratch == handler.createScratch;
                });
            };
            const auto current = findScratch(prepared);
            if (current != prepared.end()) measurement.scratch = current->scratch;
            else if (previous != nullptr) {
                const auto old = findScratch(previous->measurements_);
                if (old != previous->measurements_.end()) measurement.scratch = old->scratch;
            }
            if (!measurement.scratch) {
                detail::LogicAllocCheckpoint(detail::LogicAllocSite::MonitorDerivedStorage);
                measurement.scratch = handler.createScratch();
            }
            if (!measurement.state || !measurement.scratch) return false;
            prepared.push_back(std::move(measurement));
        }
        measurements_.swap(prepared);
        sampleRate_ = sampleRate;
        return true;
    } catch (...) { return false; }
}

void DerivedDrainState::Dispatch(const VnMonitorMeasurementRequest* requests,
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
    if (memguard::IsAudioTagged() || requests == nullptr || requestCount == 0 ||
        requestCount > measurements_.size() || packedAudioChunk == nullptr ||
        packedBytes == 0 || taps == nullptr || tapCount == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < requestCount; ++i) {
        const VnMonitorMeasurementRequest& request = requests[i];
        if (request.tapIndex >= tapCount) {
            continue;
        }
        auto& measurement = measurements_[i];
        if (measurement.kind != request.kind) {
            continue;
        }
        measurement.handler.consume(measurement.state.get(), measurement.scratch.get(),
                                    request, taps[request.tapIndex], packedAudioChunk,
                                    packedBytes, emit, emitCtx);
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
