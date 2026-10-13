#include "harken/core/MonitorScopeTrigger.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace harken {
namespace core {

namespace {

bool FiniteInRange(float value, float lo, float hi) {
    return std::isfinite(value) && value >= lo && value <= hi;
}

} // namespace

VnResult DecodeScopeOverlay(const VnMonitorMeasurementRequest& request,
                            MonitorScopeOverlay* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    if (request.structSize != sizeof(VnMonitorMeasurementRequest)) {
        return VN_INVALID_ARG;
    }
    if (request.kind != VN_MONITOR_MEASURE_SCOPE) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.downsample != 0 && request.downsample != 1) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.triggerMode != kMonitorScopeEdgeRising &&
        request.triggerMode != kMonitorScopeEdgeFalling) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (!FiniteInRange(request.triggerThreshold, -4.0f, 4.0f) ||
        !FiniteInRange(request.hysteresis, 0.0f, 4.0f)) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.channelMode != VN_MONITOR_CHANNEL_PER_CHANNEL &&
        request.channelMode != VN_MONITOR_CHANNEL_MONO_AVERAGE) {
        return VN_MONITOR_UNSUPPORTED;
    }
    if (request.overlapNumerator == 0 || request.overlapDenominator == 0) {
        return VN_INVALID_ARG;
    }
    MonitorScopeOverlay overlay{};
    overlay.edge = request.triggerMode;
    overlay.threshold = request.triggerThreshold;
    overlay.hysteresis = request.hysteresis;
    overlay.channelMode = request.channelMode;
    overlay.channelIndex = request.windowKind;
    overlay.preTriggerFrames = request.overlapNumerator;
    overlay.postTriggerFrames = request.overlapDenominator;
    const std::uint64_t window =
        static_cast<std::uint64_t>(overlay.preTriggerFrames) +
        overlay.postTriggerFrames;
    std::uint64_t maxSearch =
        request.cadenceHz == 0 ? window * 4ull : request.cadenceHz;
    const std::uint64_t lo = window;
    const std::uint64_t hi = window * 16ull;
    if (maxSearch < lo) {
        maxSearch = lo;
    }
    if (maxSearch > hi) {
        maxSearch = hi;
    }
    if (maxSearch == 0 || maxSearch > 0xffffffffull) {
        return VN_INVALID_ARG;
    }
    overlay.maxSearchFrames = static_cast<std::uint32_t>(maxSearch);
    if (overlay.channelMode == VN_MONITOR_CHANNEL_MONO_AVERAGE &&
        overlay.channelIndex != 0) {
        return VN_INVALID_ARG;
    }
    *out = overlay;
    return VN_OK;
}

void ResetScopeState(MonitorScopeState* state, std::uint64_t generation) {
    if (state == nullptr) {
        return;
    }
    *state = MonitorScopeState{};
    state->segmentGeneration = generation;
    state->armed = false;
}

bool ScopeRising(float previous, float current, float threshold) {
    return previous < threshold && current >= threshold;
}

bool ScopeFalling(float previous, float current, float threshold) {
    return previous > threshold && current <= threshold;
}

MonitorScopeResult FeedScopeSample(MonitorScopeState* state,
                                   const MonitorScopeOverlay& overlay,
                                   float sample, std::uint64_t frame) {
    MonitorScopeResult result{};
    if (state == nullptr) {
        result.noTrigger = true;
        return result;
    }
    if (!std::isfinite(sample)) {
        if (!state->noTriggerEmitted) {
            result.noTrigger = true;
            result.triggerFrame = 0;
            state->noTriggerEmitted = true;
        }
        state->hasPrevious = false;
        state->history = 0;
        state->searched = 0;
        return result;
    }
    if (!state->armed) {
        if (overlay.edge == kMonitorScopeEdgeRising) {
            state->armed = sample <= overlay.threshold - overlay.hysteresis;
        } else {
            state->armed = sample >= overlay.threshold + overlay.hysteresis;
        }
    }
    const bool ready = state->history >= overlay.preTriggerFrames;
    if (state->hasPrevious && state->armed && ready) {
        const bool hit =
            overlay.edge == kMonitorScopeEdgeRising
                ? ScopeRising(state->previous, sample, overlay.threshold)
                : ScopeFalling(state->previous, sample, overlay.threshold);
        if (hit) {
            result.triggered = true;
            result.triggerFrame = frame;
            result.windowBegin = frame >= overlay.preTriggerFrames
                                     ? frame - overlay.preTriggerFrames
                                     : 0;
            result.windowEnd = frame + overlay.postTriggerFrames;
            state->armed = false;
            state->searched = 0;
            state->noTriggerEmitted = false;
            state->previous = sample;
            state->hasPrevious = true;
            if (state->history < overlay.preTriggerFrames) {
                ++state->history;
            }
            return result;
        }
    }
    ++state->searched;
    state->previous = sample;
    state->hasPrevious = true;
    if (state->history < overlay.preTriggerFrames) {
        ++state->history;
        result.warmup = state->history < overlay.preTriggerFrames;
    }
    if (state->searched >= overlay.maxSearchFrames && !result.triggered &&
        !state->noTriggerEmitted) {
        result.noTrigger = true;
        result.windowBegin = frame >= overlay.preTriggerFrames
                                 ? frame - overlay.preTriggerFrames
                                 : 0;
        result.windowEnd = frame + overlay.postTriggerFrames;
        result.triggerFrame = 0;
        state->noTriggerEmitted = true;
        state->searched = 0;
    }
    return result;
}

} // namespace core
} // namespace harken
