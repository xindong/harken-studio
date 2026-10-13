#ifndef HARKEN_CORE_MONITOR_SCOPE_TRIGGER_H
#define HARKEN_CORE_MONITOR_SCOPE_TRIGGER_H

#include <harken/Harken.h>

#include <cstdint>

namespace harken {
namespace core {

enum {
    kMonitorScopeEdgeRising = 0,
    kMonitorScopeEdgeFalling = 1
};

struct MonitorScopeOverlay {
    std::uint32_t edge = 0;
    float threshold = 0.0f;
    float hysteresis = 0.0f;
    std::uint32_t channelMode = VN_MONITOR_CHANNEL_MONO_AVERAGE;
    std::uint32_t channelIndex = 0;
    std::uint32_t preTriggerFrames = 0;
    std::uint32_t postTriggerFrames = 0;
    std::uint32_t maxSearchFrames = 0;
};

struct MonitorScopeState {
    bool armed = false;
    bool hasPrevious = false;
    float previous = 0.0f;
    std::uint64_t searched = 0;
    std::uint64_t history = 0;
    std::uint64_t segmentGeneration = 0;
    bool noTriggerEmitted = false;
};

struct MonitorScopeResult {
    bool noTrigger = false;
    bool warmup = false;
    bool triggered = false;
    std::uint32_t discontinuityFlags = 0;
    std::uint64_t triggerFrame = 0;
    std::uint64_t windowBegin = 0;
    std::uint64_t windowEnd = 0;
};

VnResult DecodeScopeOverlay(const VnMonitorMeasurementRequest& request,
                            MonitorScopeOverlay* out);
void ResetScopeState(MonitorScopeState* state, std::uint64_t generation);
bool ScopeRising(float previous, float current, float threshold);
bool ScopeFalling(float previous, float current, float threshold);
MonitorScopeResult FeedScopeSample(MonitorScopeState* state,
                                   const MonitorScopeOverlay& overlay,
                                   float sample, std::uint64_t frame);

} // namespace core
} // namespace harken

#endif
