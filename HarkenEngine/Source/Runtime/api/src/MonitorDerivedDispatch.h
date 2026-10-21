#ifndef HARKEN_API_MONITOR_DERIVED_DISPATCH_H
#define HARKEN_API_MONITOR_DERIVED_DISPATCH_H

#include <harken/Harken.h>

#include <cstdint>
#include <memory>
#include <vector>

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
    using CreateStateFn = std::shared_ptr<void> (*)(
        const VnMonitorMeasurementRequest& request, std::uint32_t sampleRate);
    using CreateScratchFn = std::shared_ptr<void> (*)();
    using ConsumeFn = void (*)(void* state, void* scratch,
                               const VnMonitorMeasurementRequest& request,
                               const VnMonitorTapDescriptor& tap,
                               const std::uint8_t* packedAudioChunk,
                               std::uint32_t packedBytes,
                               DerivedEmitFn emit, void* emitCtx);
    ValidateFn validate = nullptr;
    ConsumeFn consume = nullptr;
    CreateStateFn createState = nullptr;
    CreateScratchFn createScratch = nullptr;
};

void RegisterDerivedHandler(std::uint32_t kind, DerivedHandler handler);
VnResult ValidateDerivedRequest(const VnMonitorMeasurementRequest& request,
                                const VnMonitorTapDescriptor& tap);
class DerivedDrainState {
public:
    bool Prepare(const VnMonitorMeasurementRequest* requests, std::uint32_t requestCount,
                 std::uint32_t sampleRate, const DerivedDrainState* previous = nullptr) noexcept;
    void Dispatch(const VnMonitorMeasurementRequest* requests,
                  std::uint32_t requestCount,
                  const VnMonitorTapDescriptor* taps,
                  std::uint32_t tapCount,
                  const std::uint8_t* packedAudioChunk,
                  std::uint32_t packedBytes, DerivedEmitFn emit,
                  void* emitCtx,
                  std::uint64_t activeCapturePlanGeneration = 0);
private:
    struct Measurement {
        std::uint32_t kind = 0;
        DerivedHandler handler{};
        std::shared_ptr<void> state;
        std::shared_ptr<void> scratch;
    };
    std::vector<Measurement> measurements_;
    std::uint32_t sampleRate_ = 0;
};

std::uint64_t DerivedDrainCapturePlanGeneration();
void ResetDerivedHandlersForTests();

} // namespace monitor
} // namespace harken

#endif
