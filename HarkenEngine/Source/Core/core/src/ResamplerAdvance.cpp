#include "harken/core/ResamplerAdvance.h"

#include <algorithm>
#include <cmath>

namespace harken {
namespace {

bool AddU64(std::uint64_t a, std::uint64_t b, std::uint64_t* out) noexcept {
    if (a > UINT64_MAX - b) return false;
    *out = a + b;
    return true;
}

bool AddPasses(std::uint64_t first, std::uint64_t passes, std::uint64_t length,
               std::uint64_t* out) noexcept {
    if (passes != 0 && length > (UINT64_MAX - first) / passes) return false;
    *out = first + passes * length;
    return true;
}

bool CanWrap(const ResamplerAdvanceState& state) noexcept {
    return state.loopEndFrame > state.loopStartFrame &&
           (state.loopInfinite || state.loopsRemaining > 1u);
}

bool NaturalDistance(const ResamplerAdvanceState& state, std::uint64_t* out) noexcept {
    if (state.loopInfinite) return false;
    const auto passes = CanWrap(state) ? state.loopsRemaining - 1u : 0u;
    return AddPasses(state.pcmFrameCount - state.sourceIntegerFrame,
        passes, state.loopEndFrame - state.loopStartFrame, out);
}

struct BoundaryShape {
    std::uint64_t first = 0;
    std::uint64_t period = 0;
    std::uint64_t maxExtra = 0;
    bool naturalFallback = false;
    bool available = false;
};

bool DescribeBoundary(const ResamplerAdvanceState& state, ResamplerBoundaryKind kind,
                      BoundaryShape* out) noexcept {
    *out = {};
    if (kind == ResamplerBoundaryKind::NaturalEnd ||
        (kind == ResamplerBoundaryKind::CurrentLoopEnd && !CanWrap(state))) {
        if (state.loopInfinite) return true;
        if (!NaturalDistance(state, &out->first)) return false;
        out->available = out->first != 0;
        return true;
    }
    if (kind == ResamplerBoundaryKind::CurrentLoopEnd) {
        if (state.sourceIntegerFrame >= state.loopEndFrame) return true;
        out->first = state.loopEndFrame - state.sourceIntegerFrame;
        out->period = state.loopEndFrame - state.loopStartFrame;
        out->maxExtra = state.loopInfinite ? UINT64_MAX : state.loopsRemaining - 2u;
        out->naturalFallback = !state.loopInfinite;
        out->available = true;
        return true;
    }
    if (kind != ResamplerBoundaryKind::ExitMarker || !state.hasExitMarker) return true;
    const auto marker = state.exitMarkerSourceFrame;
    if (marker < state.loopStartFrame) {
        if (state.completedLoopPasses != 0 || state.sourceIntegerFrame >= marker) return true;
        out->first = marker - state.sourceIntegerFrame;
    } else if (marker >= state.loopEndFrame) {
        if (state.loopInfinite) return true;
        std::uint64_t remaining = 0;
        if (!NaturalDistance(state, &remaining)) return false;
        const auto afterMarker = state.pcmFrameCount - marker;
        if (remaining <= afterMarker) return true;
        out->first = remaining - afterMarker;
    } else {
        out->period = CanWrap(state) ? state.loopEndFrame - state.loopStartFrame : 0;
        out->maxExtra = state.loopInfinite ? UINT64_MAX : state.loopsRemaining > 0
            ? state.loopsRemaining - 1u : 0;
        if (state.sourceIntegerFrame < marker) {
            out->first = marker - state.sourceIntegerFrame;
        } else {
            if (!CanWrap(state) || state.sourceIntegerFrame >= state.loopEndFrame) return true;
            if (!AddU64(state.loopEndFrame - state.sourceIntegerFrame,
                    marker - state.loopStartFrame, &out->first)) return false;
            if (!state.loopInfinite) --out->maxExtra;
        }
    }
    out->available = out->first != 0;
    return true;
}

bool AdvanceOneOutput(ResamplerAdvanceState* state, std::uint64_t* whole) noexcept {
    if (state->rampRemaining != 0) {
        state->currentRatio += state->rampStep;
        if (--state->rampRemaining == 0) state->currentRatio = state->targetRatio;
    }
    SourcePhaseStep step{};
    if (!DecodeSourcePhaseStep(state->currentRatio, &step)) return false;
    *whole = StepSourcePhase(step, &state->sourcePhase);
    const auto completed = state->completedLoopPasses;
    SetResamplerConsumedPosition(state, state->sourceIntegerFrame,
        state->loopInfinite ? 0u : state->loopsRemaining, *whole);
    state->completedLoopPasses = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        UINT32_MAX, static_cast<std::uint64_t>(completed) + state->completedLoopPasses));
    return true;
}

} // namespace

void MapLoopedSourceFrame(std::uint64_t baseFrame,
                      std::uint32_t baseLoopsRemaining,
                      std::uint64_t advance,
                      std::uint64_t frameCount,
                      std::uint64_t loopStart,
                      std::uint64_t loopEnd,
                      std::uint64_t* outFrame,
                      std::uint32_t* outLoopsRemaining) noexcept {
    std::uint64_t frame = baseFrame;
    std::uint32_t remaining = baseLoopsRemaining;

    if (remaining == 1 || loopEnd <= loopStart) {
        const std::uint64_t room = frame < frameCount ? frameCount - frame : 0;
        frame += std::min(advance, room);
    } else if (remaining == 0) {
        const std::uint64_t first =
            frame < loopEnd ? loopEnd - frame : 0;
        if (advance < first) {
            frame += advance;
        } else {
            const std::uint64_t span = loopEnd - loopStart;
            frame = loopStart + ((advance - first) % span);
        }
    } else {
        const std::uint64_t first =
            frame < loopEnd ? loopEnd - frame : 0;
        if (advance < first) {
            frame += advance;
        } else {
            std::uint64_t left = advance - first;
            --remaining;
            frame = loopStart;
            const std::uint64_t span = loopEnd - loopStart;
            if (remaining > 1 && left >= span) {
                const std::uint64_t crossed =
                    std::min<std::uint64_t>(remaining - 1,
                                            left / span);
                remaining -= static_cast<std::uint32_t>(crossed);
                left -= crossed * span;
            }
            if (remaining > 1) {
                frame = loopStart + left;
            } else {
                const std::uint64_t room =
                    loopStart < frameCount ? frameCount - loopStart : 0;
                frame = loopStart + std::min(left, room);
            }
        }
    }
    *outFrame = frame;
    *outLoopsRemaining = remaining;
}

void SetResamplerConsumedPosition(ResamplerAdvanceState* state,
                                  std::uint64_t baseFrame,
                                  std::uint32_t baseLoopsRemaining,
                                  std::uint64_t consumedSourceFrames) noexcept {
    if (state == nullptr) return;
    MapLoopedSourceFrame(baseFrame, baseLoopsRemaining, consumedSourceFrames,
        state->pcmFrameCount, state->loopStartFrame, state->loopEndFrame,
        &state->sourceIntegerFrame, &state->loopsRemaining);
    std::uint64_t passes = baseLoopsRemaining != 0
        ? baseLoopsRemaining - state->loopsRemaining : 0;
    if (baseLoopsRemaining == 0 && state->loopEndFrame > state->loopStartFrame) {
        const auto first = baseFrame < state->loopEndFrame ? state->loopEndFrame - baseFrame : 0;
        if (consumedSourceFrames >= first) {
            const auto extra = (consumedSourceFrames - first) /
                (state->loopEndFrame - state->loopStartFrame);
            passes = extra == UINT64_MAX ? UINT64_MAX : extra + 1u;
        }
    }
    state->completedLoopPasses = static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX, passes));
}

bool ResolveResamplerBoundary(ResamplerAdvanceState state,
                              ResamplerBoundaryKind kind,
                              std::uint64_t delayedFrame,
                              ResamplerBoundaryResult* out) noexcept {
    if (out == nullptr) return false;
    *out = {};
    SourcePhaseStep step{};
    if (state.pcmFrameCount == 0 || state.sourceIntegerFrame > state.pcmFrameCount ||
        state.sourceStartFrame > state.pcmFrameCount ||
        state.loopStartFrame > state.loopEndFrame || state.loopEndFrame > state.pcmFrameCount ||
        (state.hasExitMarker && state.exitMarkerSourceFrame >= state.pcmFrameCount) ||
        !DecodeSourcePhaseStep(state.currentRatio, &step) ||
        !DecodeSourcePhaseStep(state.targetRatio, &step) || !std::isfinite(state.rampStep)) return true;
    std::uint64_t produced = 0;
    if (!AddU64(state.absoluteStartFrame, state.logicalOutputFramesProduced, &produced)) return false;

    // Only an in-flight ramp needs per-frame work, using the DSP's exact Step.
    while (state.rampRemaining != 0) {
        BoundaryShape boundary{};
        if (!DescribeBoundary(state, kind, &boundary)) return false;
        if (!boundary.available) return true;
        std::uint64_t whole = 0;
        if (!AdvanceOneOutput(&state, &whole) || !AddU64(produced, 1, &produced)) return false;
        if (whole >= boundary.first && produced > delayedFrame) {
            out->available = true;
            out->outputFrame = produced;
            return true;
        }
    }
    BoundaryShape boundary{};
    if (!DescribeBoundary(state, kind, &boundary)) return false;
    if (!boundary.available || !DecodeSourcePhaseStep(state.currentRatio, &step)) return true;

    // A boundary at integer source distance D occurs after output n exactly
    // when Jump(n - 1) < D <= Jump(n). Select the first boundary after delay.
    const auto before = delayedFrame >= produced ? delayedFrame - produced : 0;
    const auto consumed = JumpSourcePhase(step, state.sourcePhase, before);
    std::uint64_t distance = boundary.first;
    if (consumed >= distance) {
        if (boundary.period == 0) return true;
        const auto quotient = (consumed - distance) / boundary.period;
        if (quotient == UINT64_MAX) return true;
        const auto extra = quotient + 1u;
        if (extra > boundary.maxExtra) {
            if (!boundary.naturalFallback) return true;
            if (!NaturalDistance(state, &distance)) return false;
            if (distance <= consumed) return true;
        } else if (!AddPasses(distance, extra, boundary.period, &distance)) return false;
    }
    std::uint64_t frames = 0;
    if (!FixedSourceOutputFrames(distance, state.currentRatio, &frames, state.sourcePhase) ||
        !AddU64(produced, frames, &out->outputFrame)) return false;
    out->available = frames != 0 && out->outputFrame > delayedFrame;
    return true;
}

bool ComputeNextResamplerBoundaries(const ResamplerAdvanceState& state,
                                    std::uint64_t* nextLoopEnd,
                                    std::uint64_t* nextExitMarker) noexcept {
    if (nextLoopEnd == nullptr || nextExitMarker == nullptr) return false;
    *nextLoopEnd = *nextExitMarker = UINT64_MAX;
    std::uint64_t delayed = 0;
    if (!AddU64(state.absoluteStartFrame, state.logicalOutputFramesProduced, &delayed)) return false;
    if (delayed != 0) --delayed;
    ResamplerBoundaryResult loop{}, marker{};
    if (!ResolveResamplerBoundary(state, ResamplerBoundaryKind::CurrentLoopEnd, delayed, &loop) ||
        !ResolveResamplerBoundary(state, ResamplerBoundaryKind::ExitMarker, delayed, &marker)) return false;
    if (loop.available) *nextLoopEnd = loop.outputFrame;
    if (marker.available) *nextExitMarker = marker.outputFrame;
    return true;
}

} // namespace harken
