#include "harken/core/ResamplerAdvance.h"

#include <cmath>
#include <limits>

namespace harken {
namespace {

constexpr double kExactIntegerLimit = 9007199254740992.0; // 2^53

bool FinitePositive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

bool FiniteFraction(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value < 1.0;
}

bool AddU64(std::uint64_t left, std::uint64_t right,
            std::uint64_t* out) noexcept {
    if (out == nullptr || left > UINT64_MAX - right) {
        return false;
    }
    *out = left + right;
    return true;
}

bool ValidRegion(const ResamplerAdvanceState& state) noexcept {
    if (state.pcmFrameCount == 0 ||
        state.sourceStartFrame > state.pcmFrameCount ||
        state.sourceIntegerFrame > state.pcmFrameCount ||
        !FiniteFraction(state.sourceFraction) ||
        !FinitePositive(state.currentRatio) ||
        !FinitePositive(state.targetRatio) ||
        !std::isfinite(state.rampStep)) {
        return false;
    }
    if (state.loopEndFrame < state.loopStartFrame ||
        state.loopEndFrame > state.pcmFrameCount) {
        return false;
    }
    if (state.hasExitMarker &&
        state.exitMarkerSourceFrame >= state.pcmFrameCount) {
        return false;
    }
    return true;
}

bool CanWrap(const ResamplerAdvanceState& state) noexcept {
    return state.loopEndFrame > state.loopStartFrame &&
           (state.loopInfinite || state.loopsRemaining > 1u);
}

std::uint64_t LoopLength(const ResamplerAdvanceState& state) noexcept {
    return state.loopEndFrame - state.loopStartFrame;
}

bool LastPass(const ResamplerAdvanceState& state) noexcept {
    return !state.loopInfinite && state.loopsRemaining <= 1u;
}

double SourcePosition(const ResamplerAdvanceState& state) noexcept {
    return static_cast<double>(state.sourceIntegerFrame) +
           state.sourceFraction;
}

bool CeilOutputFrames(double sourceRemaining, double ratio,
                      std::uint64_t* out) noexcept {
    if (out == nullptr || !FinitePositive(ratio) ||
        !std::isfinite(sourceRemaining)) {
        return false;
    }
    if (sourceRemaining <= 0.0) {
        *out = 0;
        return true;
    }
    const double raw = std::ceil(sourceRemaining / ratio);
    if (!std::isfinite(raw) || raw < 0.0 || raw >= kExactIntegerLimit) {
        return false;
    }
    auto frames = static_cast<std::uint64_t>(raw);
    while (frames < UINT64_MAX &&
           static_cast<double>(frames) * ratio < sourceRemaining) {
        ++frames;
    }
    while (frames > 0u &&
           static_cast<double>(frames - 1u) * ratio >= sourceRemaining) {
        --frames;
    }
    *out = frames;
    return true;
}

bool AdvanceOneOutput(ResamplerAdvanceState* state) noexcept {
    if (state == nullptr) {
        return false;
    }
    double frameRatio = state->currentRatio;
    if (state->rampRemaining != 0) {
        frameRatio += state->rampStep;
        if (state->rampRemaining == 1u) {
            frameRatio = state->targetRatio;
        }
        if (!FinitePositive(frameRatio)) {
            return false;
        }
        state->currentRatio = frameRatio;
        --state->rampRemaining;
    }
    const double advanced = state->sourceFraction + frameRatio;
    if (!std::isfinite(advanced) || advanced < 0.0) {
        return false;
    }
    const double whole = std::floor(advanced);
    if (whole >= kExactIntegerLimit) {
        return false;
    }
    const auto wholeFrames = static_cast<std::uint64_t>(whole);
    if (!AddU64(state->sourceIntegerFrame, wholeFrames,
                &state->sourceIntegerFrame)) {
        return false;
    }
    state->sourceFraction = advanced - whole;
    if (CanWrap(*state)) {
        const std::uint64_t length = LoopLength(*state);
        while (state->sourceIntegerFrame >= state->loopEndFrame) {
            state->sourceIntegerFrame =
                state->sourceIntegerFrame - state->loopEndFrame +
                state->loopStartFrame;
            if (state->completedLoopPasses < UINT32_MAX) {
                ++state->completedLoopPasses;
            }
            if (!state->loopInfinite) {
                --state->loopsRemaining;
            }
            if (!CanWrap(*state)) {
                break;
            }
            if (length == 0) {
                return false;
            }
        }
    }
    return true;
}

bool AbsoluteNow(const ResamplerAdvanceState& state,
                 std::uint64_t outputOffset, std::uint64_t* out) noexcept {
    std::uint64_t produced = 0;
    return AddU64(state.absoluteStartFrame,
                  state.logicalOutputFramesProduced, &produced) &&
           AddU64(produced, outputOffset, out);
}

bool NaturalEndSourceRemaining(const ResamplerAdvanceState& state,
                               double* out) noexcept {
    if (out == nullptr || state.loopInfinite) {
        return false;
    }
    const double position = SourcePosition(state);
    if (LastPass(state)) {
        const double remaining =
            static_cast<double>(state.pcmFrameCount) - position;
        if (!std::isfinite(remaining)) {
            return false;
        }
        *out = remaining;
        return true;
    }
    if (state.loopEndFrame <= state.loopStartFrame) {
        const double remaining =
            static_cast<double>(state.pcmFrameCount) - position;
        if (!std::isfinite(remaining)) {
            return false;
        }
        *out = remaining;
        return true;
    }
    const double toWrap =
        static_cast<double>(state.loopEndFrame) - position;
    const std::uint32_t extraPasses =
        state.loopsRemaining > 1u ? state.loopsRemaining - 2u : 0u;
    const double middle =
        static_cast<double>(extraPasses) *
        static_cast<double>(LoopLength(state));
    const double last =
        static_cast<double>(state.pcmFrameCount) -
        static_cast<double>(state.loopStartFrame);
    const double remaining = toWrap + middle + last;
    if (!std::isfinite(toWrap) || !std::isfinite(middle) ||
        !std::isfinite(last) || !std::isfinite(remaining)) {
        return false;
    }
    *out = remaining;
    return true;
}

bool MarkerInLoopRegion(const ResamplerAdvanceState& state) noexcept {
    return state.hasExitMarker &&
           state.exitMarkerSourceFrame >= state.loopStartFrame &&
           state.exitMarkerSourceFrame < state.loopEndFrame;
}

bool MarkerInIntro(const ResamplerAdvanceState& state) noexcept {
    return state.hasExitMarker &&
           state.exitMarkerSourceFrame < state.loopStartFrame;
}

bool MarkerInOutro(const ResamplerAdvanceState& state) noexcept {
    return state.hasExitMarker &&
           state.exitMarkerSourceFrame >= state.loopEndFrame;
}

} // namespace

bool ResolveResamplerBoundary(ResamplerAdvanceState state,
                              ResamplerBoundaryKind kind,
                              std::uint64_t delayedFrame,
                              ResamplerBoundaryResult* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    if (!ValidRegion(state)) {
        return true;
    }
    if (kind == ResamplerBoundaryKind::ExitMarker && !state.hasExitMarker) {
        return true;
    }
    if (kind == ResamplerBoundaryKind::NaturalEnd && state.loopInfinite) {
        return true;
    }

    std::uint64_t outputOffset = 0;
    auto crossed = [&](std::uint32_t wrapsBefore, std::uint64_t integerBefore,
                       bool lastBefore) -> bool {
        if (kind == ResamplerBoundaryKind::NaturalEnd) {
            return !state.loopInfinite &&
                   state.sourceIntegerFrame >= state.pcmFrameCount;
        }
        if (kind == ResamplerBoundaryKind::CurrentLoopEnd) {
            if (state.completedLoopPasses > wrapsBefore) {
                return true;
            }
            return lastBefore &&
                   state.sourceIntegerFrame >= state.pcmFrameCount;
        }
        if (kind != ResamplerBoundaryKind::ExitMarker || !state.hasExitMarker) {
            return false;
        }
        if (integerBefore < state.exitMarkerSourceFrame &&
            state.sourceIntegerFrame >= state.exitMarkerSourceFrame) {
            return true;
        }
        if (state.completedLoopPasses > wrapsBefore && MarkerInLoopRegion(state) &&
            state.sourceIntegerFrame >= state.exitMarkerSourceFrame) {
            return true;
        }
        return false;
    };

    while (state.rampRemaining != 0) {
        const std::uint32_t wrapsBefore = state.completedLoopPasses;
        const std::uint64_t integerBefore = state.sourceIntegerFrame;
        const bool lastBefore = LastPass(state);
        if (!AdvanceOneOutput(&state)) {
            return false;
        }
        if (!AddU64(outputOffset, 1u, &outputOffset)) {
            return false;
        }
        if (crossed(wrapsBefore, integerBefore, lastBefore)) {
            std::uint64_t absolute = 0;
            if (!AbsoluteNow(state, outputOffset, &absolute)) {
                return false;
            }
            if (absolute > delayedFrame) {
                out->available = true;
                out->outputFrame = absolute;
                return true;
            }
            if (kind == ResamplerBoundaryKind::NaturalEnd) {
                return true;
            }
        }
        if (!state.loopInfinite &&
            state.sourceIntegerFrame >= state.pcmFrameCount) {
            return true;
        }
    }

    const double ratio = state.currentRatio;
    const double position = SourcePosition(state);
    std::uint64_t produced = 0;
    if (!AddU64(state.absoluteStartFrame, state.logicalOutputFramesProduced,
                &produced) ||
        !AddU64(produced, outputOffset, &produced)) {
        return false;
    }
    const std::uint64_t needN =
        delayedFrame < produced ? 0 : delayedFrame - produced + 1u;

    if (kind == ResamplerBoundaryKind::NaturalEnd) {
        double remaining = 0.0;
        if (!NaturalEndSourceRemaining(state, &remaining)) {
            return true;
        }
        std::uint64_t frames = 0;
        if (!CeilOutputFrames(remaining, ratio, &frames)) {
            return false;
        }
        std::uint64_t absolute = 0;
        if (!AddU64(produced, frames, &absolute)) {
            return false;
        }
        if (absolute > delayedFrame && frames != 0) {
            out->available = true;
            out->outputFrame = absolute;
        }
        return true;
    }

    if (kind == ResamplerBoundaryKind::CurrentLoopEnd) {
        if (LastPass(state) || state.loopEndFrame <= state.loopStartFrame) {
            double remaining =
                static_cast<double>(state.pcmFrameCount) - position;
            std::uint64_t frames = 0;
            if (!std::isfinite(remaining) ||
                !CeilOutputFrames(remaining, ratio, &frames)) {
                return false;
            }
            std::uint64_t absolute = 0;
            if (!AddU64(produced, frames, &absolute)) {
                return false;
            }
            if (absolute > delayedFrame && frames != 0) {
                out->available = true;
                out->outputFrame = absolute;
            }
            return true;
        }

        const double first =
            static_cast<double>(state.loopEndFrame) - position;
        const double length = static_cast<double>(LoopLength(state));
        if (!std::isfinite(first) || !std::isfinite(length) || length <= 0.0) {
            return false;
        }
        std::uint64_t firstFrames = 0;
        if (!CeilOutputFrames(first, ratio, &firstFrames)) {
            return false;
        }
        if (firstFrames >= needN) {
            std::uint64_t absolute = 0;
            if (!AddU64(produced, firstFrames, &absolute)) {
                return false;
            }
            out->available = true;
            out->outputFrame = absolute;
            return true;
        }

        const double needSource =
            static_cast<double>(needN) * ratio - first;
        double extraPasses = 0.0;
        if (needSource > 0.0) {
            extraPasses = std::ceil(needSource / length);
        }
        if (!std::isfinite(extraPasses) || extraPasses < 0.0 ||
            extraPasses >= kExactIntegerLimit) {
            return false;
        }
        auto extra = static_cast<std::uint64_t>(extraPasses);
        if (!state.loopInfinite) {
            const std::uint32_t maxExtra =
                state.loopsRemaining > 1u ? state.loopsRemaining - 1u : 0u;
            if (extra >= maxExtra) {
                const double last =
                    static_cast<double>(state.pcmFrameCount) -
                    static_cast<double>(state.loopStartFrame);
                const double fromNow =
                    first +
                    static_cast<double>(maxExtra > 0u ? maxExtra - 1u : 0u) *
                        length +
                    last;
                std::uint64_t frames = 0;
                if (!std::isfinite(fromNow) ||
                    !CeilOutputFrames(fromNow, ratio, &frames)) {
                    return false;
                }
                std::uint64_t absolute = 0;
                if (!AddU64(produced, frames, &absolute)) {
                    return false;
                }
                if (absolute > delayedFrame && frames != 0) {
                    out->available = true;
                    out->outputFrame = absolute;
                }
                return true;
            }
        }
        const double source = first + static_cast<double>(extra) * length;
        std::uint64_t frames = 0;
        if (!CeilOutputFrames(source, ratio, &frames)) {
            return false;
        }
        while (frames < needN) {
            if (extra == UINT64_MAX) {
                return false;
            }
            ++extra;
            if (!CeilOutputFrames(
                    first + static_cast<double>(extra) * length, ratio,
                    &frames)) {
                return false;
            }
        }
        std::uint64_t absolute = 0;
        if (!AddU64(produced, frames, &absolute)) {
            return false;
        }
        out->available = true;
        out->outputFrame = absolute;
        return true;
    }

    if (!state.hasExitMarker) {
        return true;
    }
    const double marker = static_cast<double>(state.exitMarkerSourceFrame);
    if (MarkerInIntro(state)) {
        if (state.completedLoopPasses != 0 || position >= marker) {
            return true;
        }
        std::uint64_t frames = 0;
        if (!CeilOutputFrames(marker - position, ratio, &frames)) {
            return false;
        }
        std::uint64_t absolute = 0;
        if (!AddU64(produced, frames, &absolute)) {
            return false;
        }
        if (absolute > delayedFrame && frames != 0) {
            out->available = true;
            out->outputFrame = absolute;
        }
        return true;
    }
    if (MarkerInOutro(state)) {
        if (state.loopInfinite) {
            return true;
        }
        double remaining = 0.0;
        if (!NaturalEndSourceRemaining(state, &remaining)) {
            return true;
        }
        const double toMarker =
            static_cast<double>(state.pcmFrameCount) - marker;
        const double source = remaining - toMarker;
        if (source <= 0.0) {
            return true;
        }
        std::uint64_t frames = 0;
        if (!CeilOutputFrames(source, ratio, &frames)) {
            return false;
        }
        std::uint64_t absolute = 0;
        if (!AddU64(produced, frames, &absolute)) {
            return false;
        }
        if (absolute > delayedFrame && frames != 0) {
            out->available = true;
            out->outputFrame = absolute;
        }
        return true;
    }
    if (!MarkerInLoopRegion(state)) {
        return true;
    }

    double first = marker - position;
    if (first <= 0.0) {
        if (LastPass(state) || !CanWrap(state)) {
            return true;
        }
        first = static_cast<double>(state.loopEndFrame) - position +
                (marker - static_cast<double>(state.loopStartFrame));
    }
    const double length = static_cast<double>(LoopLength(state));
    if (!std::isfinite(first) || !std::isfinite(length) || length <= 0.0) {
        return false;
    }
    std::uint64_t firstFrames = 0;
    if (!CeilOutputFrames(first, ratio, &firstFrames)) {
        return false;
    }
    if (firstFrames >= needN) {
        std::uint64_t absolute = 0;
        if (!AddU64(produced, firstFrames, &absolute)) {
            return false;
        }
        out->available = true;
        out->outputFrame = absolute;
        return true;
    }
    if (LastPass(state) || !CanWrap(state)) {
        return true;
    }
    const double needSource = static_cast<double>(needN) * ratio - first;
    double extraPasses = 0.0;
    if (needSource > 0.0) {
        extraPasses = std::ceil(needSource / length);
    }
    if (!std::isfinite(extraPasses) || extraPasses < 0.0 ||
        extraPasses >= kExactIntegerLimit) {
        return false;
    }
    auto extra = static_cast<std::uint64_t>(extraPasses);
    if (!state.loopInfinite) {
        const std::uint32_t maxExtra =
            state.loopsRemaining > 1u ? state.loopsRemaining - 1u : 0u;
        if (extra > maxExtra) {
            return true;
        }
    }
    const double source = first + static_cast<double>(extra) * length;
    std::uint64_t frames = 0;
    if (!CeilOutputFrames(source, ratio, &frames)) {
        return false;
    }
    std::uint64_t absolute = 0;
    if (!AddU64(produced, frames, &absolute)) {
        return false;
    }
    if (absolute > delayedFrame && frames != 0) {
        out->available = true;
        out->outputFrame = absolute;
    }
    return true;
}

bool ComputeNextResamplerBoundaries(const ResamplerAdvanceState& state,
                                    std::uint64_t* nextLoopEnd,
                                    std::uint64_t* nextExitMarker) noexcept {
    if (nextLoopEnd == nullptr || nextExitMarker == nullptr) {
        return false;
    }
    *nextLoopEnd = UINT64_MAX;
    *nextExitMarker = UINT64_MAX;
    std::uint64_t delayed = 0;
    if (!AddU64(state.absoluteStartFrame, state.logicalOutputFramesProduced,
                &delayed)) {
        return false;
    }
    if (delayed > 0) {
        --delayed;
    }
    ResamplerBoundaryResult loop{};
    if (!ResolveResamplerBoundary(state, ResamplerBoundaryKind::CurrentLoopEnd,
                                  delayed, &loop)) {
        return false;
    }
    if (loop.available) {
        *nextLoopEnd = loop.outputFrame;
    }
    ResamplerBoundaryResult marker{};
    if (!ResolveResamplerBoundary(state, ResamplerBoundaryKind::ExitMarker,
                                  delayed, &marker)) {
        return false;
    }
    if (marker.available) {
        *nextExitMarker = marker.outputFrame;
    }
    return true;
}

} // namespace harken
