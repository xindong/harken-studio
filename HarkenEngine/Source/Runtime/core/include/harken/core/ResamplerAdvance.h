#ifndef HARKEN_CORE_RESAMPLER_ADVANCE_H
#define HARKEN_CORE_RESAMPLER_ADVANCE_H

#include <cstdint>

namespace harken {

enum class ResamplerBoundaryKind : std::uint8_t {
    NaturalEnd = 0,
    CurrentLoopEnd = 1,
    ExitMarker = 2,
};

struct ResamplerAdvanceState {
    std::uint64_t sourceIntegerFrame = 0;
    double sourceFraction = 0.0;
    double currentRatio = 1.0;
    double targetRatio = 1.0;
    double rampStep = 0.0;
    std::uint32_t rampRemaining = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint64_t pcmFrameCount = 0;
    std::uint64_t sourceStartFrame = 0;
    std::uint32_t completedLoopPasses = 0;
    std::uint32_t loopsRemaining = 1;
    bool loopInfinite = false;
    bool hasExitMarker = false;
    std::uint64_t exitMarkerSourceFrame = 0;
    std::uint64_t logicalOutputFramesProduced = 0;
    std::uint64_t absoluteStartFrame = 0;
};

struct ResamplerBoundaryResult {
    bool available = false;
    std::uint64_t outputFrame = 0;
};

bool ResolveResamplerBoundary(ResamplerAdvanceState state,
                              ResamplerBoundaryKind kind,
                              std::uint64_t delayedFrame,
                              ResamplerBoundaryResult* out) noexcept;

bool ComputeNextResamplerBoundaries(const ResamplerAdvanceState& state,
                                    std::uint64_t* nextLoopEnd,
                                    std::uint64_t* nextExitMarker) noexcept;

} // namespace harken

#endif // HARKEN_CORE_RESAMPLER_ADVANCE_H
