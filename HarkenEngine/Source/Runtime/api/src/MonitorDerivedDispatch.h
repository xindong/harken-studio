#ifndef HARKEN_API_MONITOR_DERIVED_DISPATCH_H
#define HARKEN_API_MONITOR_DERIVED_DISPATCH_H

#include <harken/Harken.h>

#include <cstdint>

namespace harken {
namespace monitor {

struct DerivedEmit {
    const std::uint8_t* bytes = nullptr;
    std::uint32_t size = 0;
};

using DerivedEmitFn = bool (*)(void* ctx, const DerivedEmit& emit);

struct DerivedHandler {
    using ValidateFn = VnResult (*)(const VnMonitorMeasurementRequest& request,
                                    const VnMonitorTapDescriptor& tap);
    using ConsumeFn = void (*)(const VnMonitorMeasurementRequest& request,
                               const VnMonitorTapDescriptor& tap,
                               const std::uint8_t* packedAudioChunk,
                               std::uint32_t packedBytes,
                               std::uint32_t measurementIndex, DerivedEmitFn emit,
                               void* emitCtx);
    ValidateFn validate = nullptr;
    ConsumeFn consume = nullptr;
};

void RegisterDerivedHandler(std::uint32_t kind, DerivedHandler handler);
VnResult ValidateDerivedRequest(const VnMonitorMeasurementRequest& request,
                                const VnMonitorTapDescriptor& tap);
void DispatchDerivedAudioChunk(const VnMonitorMeasurementRequest* requests,
                               std::uint32_t requestCount,
                               const VnMonitorTapDescriptor* taps,
                               std::uint32_t tapCount,
                               const std::uint8_t* packedAudioChunk,
                               std::uint32_t packedBytes, DerivedEmitFn emit,
                               void* emitCtx,
                               std::uint64_t activeCapturePlanGeneration = 0);
std::uint64_t DerivedDrainCapturePlanGeneration();
void ResetMonitorSpectrumSlotsForTests();
void ResetDerivedHandlersForTests();

} // namespace monitor
} // namespace harken

#endif
