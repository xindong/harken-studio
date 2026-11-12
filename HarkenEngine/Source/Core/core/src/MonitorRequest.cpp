#include "harken/core/MonitorRequest.h"

#include <random>

namespace harken {
namespace {

std::uint64_t NextSequence(std::atomic<std::uint64_t>& last) noexcept {
    auto value = last.load(std::memory_order_relaxed);
    while (value != UINT64_MAX) {
        if (last.compare_exchange_weak(value, value + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) return value + 1;
    }
    return 0;
}

std::uint64_t EngineSequenceSeed() noexcept {
    try {
        std::random_device entropy;
        // Randomize across processes/library reloads; retain at least half the
        // sequence space for strictly distinct incarnations in this library.
        return std::uniform_int_distribution<std::uint64_t>(1, UINT64_MAX / 2)(entropy);
    } catch (...) {
        return UINT64_MAX;
    }
}

} // namespace

std::uint64_t AllocateMonitorEngineInstance() noexcept {
    static std::atomic<std::uint64_t> last{EngineSequenceSeed()};
    return NextSequence(last);
}

MonitorRequestContext MonitorRequestSource::Begin(
    MonitorRequestOrigin origin, std::uint32_t operation) noexcept {
    MonitorRequestContext context{{}, origin, operation};
    const auto sequence = engineInstance_ != 0 ? NextSequence(lastSequence_) : 0;
    if (sequence != 0) context.key = {engineInstance_, sequence};
    else (void)NextSequence(unavailable_);
    return context;
}

} // namespace harken
