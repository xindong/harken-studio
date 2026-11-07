#ifndef HARKEN_CORE_FRONTIER_SOURCE_EXIT_H
#define HARKEN_CORE_FRONTIER_SOURCE_EXIT_H

#include "harken/core/FrontierActivation.h"
#include "harken/core/SourceTerminalRecord.h"
#include "harken/param/Curve.h"

#include <span>

namespace harken {

struct FrontierSourceExit;

// Prepared by the source owner and retained through the voice's release fence.
// Only the serial audio stage changes linkage and freezes worker-visible state.
struct FrontierSourceExitVoice {
    const FrontierSourceExit* source = nullptr;
    std::uint32_t renderSlot = UINT32_MAX;
    std::uint32_t renderGeneration = 0;
    VoiceReleaseObserver release{};
    FrontierSourceExitVoice* next = nullptr;
    bool attached = false;
    bool active = false;
    std::atomic<std::uint64_t> detachedAudioSequence{0};
};

struct FrontierSourceExit {
    const DecisionTable* decisions = nullptr;
    FrontierSliceDependency dependency{};
    FrontierOwnerIdentity source{};
    std::uint64_t fadeStartFrame = 0;
    std::uint64_t fadeElapsedFrames = 0;
    std::uint64_t stopFrame = UINT64_MAX;
    bool fade = false;
    bool stop = false;
    CurveLUT curve{};
    // Frozen before publication; later voices borrow this descriptor separately.
    std::span<FrontierSourceExitVoice* const> initialVoices{};
};

} // namespace harken

#endif
