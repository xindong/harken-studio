// CPU-time measurement primitives for the per-stage profiling fields: a raw
// monotonic tick read (QueryPerformanceCounter) and the tick rate, plus the
// pure conversion from a tick interval to whole microseconds. The tick read
// is a single kernel-mapped counter read — no allocation, no locks — so it is
// legal on the audio thread in every build configuration. The tick rate is
// fetched by callers once on the logic thread (construction time) and passed
// into the conversion, keeping any first-call initialization off the audio
// thread.
#ifndef HARKEN_CORE_CPU_CLOCK_H
#define HARKEN_CORE_CPU_CLOCK_H

#include <cstdint>

namespace harken {

// Current value of the monotonic performance counter, in ticks.
std::uint64_t CpuClockTicks();

// Tick rate of the performance counter, in ticks per second. Constant for
// the life of the process; call once at setup and cache the value.
std::uint64_t CpuClockTicksPerSecond();

// Whole microseconds in the tick interval [begin, end], truncated. The
// truncation is load-bearing for the stage-sum invariant: stages measured
// from shared boundary reads satisfy floor(a) + floor(b) + floor(c) <=
// floor(a + b + c), so the per-stage sum can never exceed the truncated
// whole through rounding alone. Saturates at UINT32_MAX (an interval that
// long is not a per-block measurement).
inline std::uint32_t CpuTicksToMicros(std::uint64_t begin, std::uint64_t end,
                                      std::uint64_t ticksPerSecond) {
    if (end <= begin || ticksPerSecond == 0) {
        return 0;
    }
    const std::uint64_t ticks = end - begin;
    // Split the conversion so the multiply cannot overflow for any interval
    // shorter than ~584 years at a 1 GHz tick rate.
    const std::uint64_t seconds = ticks / ticksPerSecond;
    const std::uint64_t rem = ticks % ticksPerSecond;
    const std::uint64_t micros =
        seconds * 1000000ull + (rem * 1000000ull) / ticksPerSecond;
    if (micros > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;
    }
    return static_cast<std::uint32_t>(micros);
}

} // namespace harken

#endif // HARKEN_CORE_CPU_CLOCK_H
