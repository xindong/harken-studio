#ifndef HARKEN_RENDER_VOICE_TIMELINE_ELIGIBILITY_H
#define HARKEN_RENDER_VOICE_TIMELINE_ELIGIBILITY_H

#include "harken/render/Voice.h"

namespace harken {

inline bool TimelineEventGuardEligible(const VoiceEventSpan& span,
                                      std::uint32_t eventIndex) noexcept {
    return !span.events[eventIndex].firstOutputDeferred &&
        span.events[eventIndex].frontierProof.Eligible(span.quantumIndex, span.renderGeneration);
}

struct AuthoredRootStopCutoff {
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t eventIndex = UINT32_MAX;
    AuthoredStopOperationIdentity operation{};
};

inline AuthoredRootStopCutoff SelectAuthoredRootStop(
    const VoiceEventSpan& span, std::uint32_t blockFrames,
    std::uint64_t persistedFrame,
    AuthoredStopOperationIdentity persistedOperation = {}) noexcept {
    AuthoredRootStopCutoff cutoff{persistedFrame, UINT32_MAX, persistedOperation};
    for (std::uint32_t i = 0; i < span.eventCount; ++i) {
        if (!TimelineEventGuardEligible(span, i)) continue;
        const auto& event = span.events[i];
        if (event.offset >= blockFrames || event.Kind() != TimelineEventKind::Stop ||
            !event.authoredRootStop) continue;
        const auto frame = span.quantumIndex * blockFrames + event.offset;
        if (frame < cutoff.frame) cutoff = {frame, i, event.Stop().authoredStopOperation};
    }
    return cutoff;
}

inline bool TimelineEventPrecedesRootStop(
    const VoiceEventSpan& span, std::uint32_t eventIndex,
    std::uint32_t blockFrames, AuthoredRootStopCutoff cutoff,
    bool cutoffApplied) noexcept {
    if (!TimelineEventGuardEligible(span, eventIndex)) return false;
    const auto& event = span.events[eventIndex];
    const auto& proof = event.frontierProof;
    if (proof.state != FrontierEventState::Legacy) {
        return event.order.executeFrame < proof.cutoffFrame ||
            (event.Kind() == TimelineEventKind::Stop && event.authoredRootStop &&
             event.Stop().authoredStopOperation.Valid() &&
             event.Stop().authoredStopOperation == proof.cutoffOperation);
    }
    const auto frame = span.quantumIndex * blockFrames + span.events[eventIndex].offset;
    return frame < cutoff.frame || (!cutoffApplied && eventIndex == cutoff.eventIndex);
}

inline bool TimelineEventPrecedesRootStop(
    const VoiceRenderContext& context, std::uint32_t eventIndex,
    std::uint32_t blockFrames) noexcept {
    return TimelineEventPrecedesRootStop(context.timelineEvents, eventIndex,
        blockFrames, {context.authoredRootStopFrame, context.authoredRootStopEvent},
        context.authoredRootStopApplied);
}

} // namespace harken

#endif // HARKEN_RENDER_VOICE_TIMELINE_ELIGIBILITY_H
