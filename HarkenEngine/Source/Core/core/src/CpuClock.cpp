#include "harken/core/CpuClock.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <chrono>
#endif

namespace harken {

std::uint64_t CpuClockTicks() {
#if defined(_WIN32)
    // Documented never to fail on XP and later; the counter is monotonic
    // and process-global. One read, no allocation, no locks.
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
#else
    // steady_clock is monotonic. Expressing it as nanoseconds gives callers a
    // stable, process-wide tick rate without any platform API dependencies.
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

std::uint64_t CpuClockTicksPerSecond() {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(frequency.QuadPart);
#else
    return 1000000000ull;
#endif
}

} // namespace harken
