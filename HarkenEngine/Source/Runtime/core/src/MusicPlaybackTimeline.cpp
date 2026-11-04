#include "harken/core/MusicPlaybackTimeline.h"

#include <algorithm>
#include <limits>

namespace harken {
namespace {

constexpr auto kMax = (std::numeric_limits<std::int64_t>::max)();
constexpr auto kMin = (std::numeric_limits<std::int64_t>::min)();

bool Add(std::int64_t a, std::int64_t b, std::int64_t& result) noexcept {
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) return false;
    result = a + b;
    return true;
}

bool Difference(std::int64_t a, std::int64_t b, std::int64_t& result) noexcept {
    if ((b < 0 && a > kMax + b) || (b > 0 && a < kMin + b)) return false;
    result = a - b;
    return true;
}

std::uint64_t Magnitude(std::int64_t value) noexcept {
    return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1u
                     : static_cast<std::uint64_t>(value);
}

bool AddUnsigned(std::int64_t value, std::uint64_t offset, std::int64_t& result) noexcept {
    const auto room = static_cast<std::uint64_t>(kMax) - static_cast<std::uint64_t>(value);
    if (offset > room) return false;
    if (value >= 0) result = value + static_cast<std::int64_t>(offset);
    else if (offset >= Magnitude(value)) result = static_cast<std::int64_t>(offset - Magnitude(value));
    else {
        const auto magnitude = Magnitude(value) - offset;
        result = magnitude == (UINT64_C(1) << 63) ? kMin : -static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool SubtractUnsigned(std::int64_t value, std::uint64_t offset, std::int64_t& result) noexcept {
    const auto room = static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(kMin);
    if (offset > room) return false;
    if (value >= 0 && offset <= static_cast<std::uint64_t>(value))
        result = value - static_cast<std::int64_t>(offset);
    else {
        const auto magnitude = value < 0 ? Magnitude(value) + offset
                                         : offset - static_cast<std::uint64_t>(value);
        result = magnitude == (UINT64_C(1) << 63) ? kMin : -static_cast<std::int64_t>(magnitude);
    }
    return true;
}

struct Mapping {
    const MusicPlaybackTimeline& timeline;
    const ClockKernel& clock;
    std::int64_t span = 0;
    std::int64_t originFrame = 0;
    MusicTimelineStatus status = MusicTimelineStatus::Invalid;

    std::int64_t Convert(std::int64_t position) const noexcept {
        return timeline.timeType == SegmentTimeType::Musical
            ? clock.TickToFrame({position}).value : clock.TimeToFrame({position}).value;
    }

    Mapping(const MusicPlaybackTimeline& t, const ClockKernel& c) noexcept
        : timeline(t), clock(c) {
        if (!clock.Valid() || clock.SegmentCount() != 1 ||
            (t.timeType != SegmentTimeType::Musical && t.timeType != SegmentTimeType::Absolute) ||
            t.exit <= t.entry || !Difference(t.exit, t.entry, span) ||
            (!t.fromEntry && t.exit <= 0)) return;
        originFrame = Convert(t.fromEntry ? t.entry : 0);
        const auto in = Convert(t.entry);
        const auto out = Convert(t.exit);
        // ClockKernel saturates. Extremal values are reserved as an explicit
        // failure here; they must never become a plausible future event.
        if (originFrame == kMin || originFrame == kMax || in == kMin ||
            in == kMax || out == kMin || out == kMax) {
            status = MusicTimelineStatus::Unrepresentable;
            return;
        }
        if (out <= in) return;
        status = MusicTimelineStatus::Ok;
    }

    MusicTimelineStatus Frame(std::int64_t position, std::uint64_t iteration,
                              std::uint64_t& result) const noexcept {
        if (status != MusicTimelineStatus::Ok) return status;
        const auto room = static_cast<std::uint64_t>(kMax) - static_cast<std::uint64_t>(position);
        if (iteration > room / static_cast<std::uint64_t>(span))
            return MusicTimelineStatus::Unrepresentable;
        std::int64_t unfolded = 0;
        if (!AddUnsigned(position, iteration * static_cast<std::uint64_t>(span), unfolded))
            return MusicTimelineStatus::Unrepresentable;
        const auto converted = Convert(unfolded);
        if (converted == kMin || converted == kMax)
            return MusicTimelineStatus::Unrepresentable;
        if (converted < originFrame) return MusicTimelineStatus::Invalid;
        // Unsigned subtraction represents a nonnegative signed difference even
        // when the two coordinates straddle zero.
        result = static_cast<std::uint64_t>(converted) - static_cast<std::uint64_t>(originFrame);
        return MusicTimelineStatus::Ok;
    }

    std::uint64_t LastIteration() const noexcept {
        return timeline.playCount == 0 ?
            (static_cast<std::uint64_t>(kMax) - static_cast<std::uint64_t>(timeline.exit)) /
                static_cast<std::uint64_t>(span)
                                      : timeline.playCount - 1u;
    }

    // First Out at or beyond lower. Binary search also bounds work for infinite
    // playback after a very large transport jump; no per-pass walk is needed.
    MusicTimelineStatus ExitAtOrAfter(std::uint64_t lower, std::uint64_t& iteration,
                                     std::uint64_t& frame) const noexcept {
        if (status != MusicTimelineStatus::Ok) return status;
        std::uint64_t lo = 0, hi = LastIteration();
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            std::uint64_t candidate = 0;
            const auto mapped = Frame(timeline.exit, mid, candidate);
            if (mapped != MusicTimelineStatus::Ok || candidate >= lower) hi = mid;
            else lo = mid + 1;
        }
        iteration = lo;
        const auto mapped = Frame(timeline.exit, lo, frame);
        if (mapped != MusicTimelineStatus::Ok) return mapped;
        return frame >= lower ? MusicTimelineStatus::Ok : timeline.playCount == 0
            ? MusicTimelineStatus::Unrepresentable : MusicTimelineStatus::Missed;
    }

    MusicPlaybackPosition Position(std::uint64_t elapsed) const noexcept {
        MusicPlaybackPosition result;
        result.status = status;
        if (status != MusicTimelineStatus::Ok) return result;
        if (elapsed == UINT64_MAX) {
            result.status = MusicTimelineStatus::Unrepresentable;
            return result;
        }
        std::uint64_t outFrame = 0;
        result.status = ExitAtOrAfter(elapsed + 1, result.iteration, outFrame);
        if (result.status == MusicTimelineStatus::Missed && timeline.playCount != 0) {
            result.iteration = timeline.playCount - 1u;
            std::uint64_t end = 0;
            result.status = Frame(std::max(timeline.exit, timeline.contentEnd), result.iteration, end);
            if (result.status != MusicTimelineStatus::Ok) return result;
            result.phase = elapsed >= end ? MusicPlaybackPhase::Ended : MusicPlaybackPhase::Outro;
            if (result.phase == MusicPlaybackPhase::Ended) {
                result.position = std::max(timeline.exit, timeline.contentEnd);
                return result;
            }
        } else if (result.status == MusicTimelineStatus::Ok) {
            std::uint64_t entryFrame = 0;
            const auto entryStatus = Frame(timeline.entry, result.iteration, entryFrame);
            // An entry before the direct origin has already been crossed.
            result.phase = entryStatus == MusicTimelineStatus::Ok && elapsed < entryFrame
                ? MusicPlaybackPhase::Intro : MusicPlaybackPhase::Loop;
        } else return result;

        const auto positiveRoom = static_cast<std::uint64_t>(kMax) -
            static_cast<std::uint64_t>(originFrame);
        if (elapsed >= positiveRoom) {
            result.status = MusicTimelineStatus::Unrepresentable;
            return result;
        }
        std::int64_t absolute = 0;
        if (!AddUnsigned(originFrame, elapsed, absolute)) {
            result.status = MusicTimelineStatus::Unrepresentable;
            return result;
        }
        auto unfolded = timeline.timeType == SegmentTimeType::Musical
            ? clock.FrameToTick({absolute}).value
            : clock.FrameToTime({absolute + 1}).value;
        if (unfolded == kMin || unfolded == kMax) {
            result.status = MusicTimelineStatus::Unrepresentable;
            return result;
        }
        if (timeline.timeType == SegmentTimeType::Absolute) --unfolded;
        if (unfolded == kMin ||
            !SubtractUnsigned(unfolded, result.iteration * static_cast<std::uint64_t>(span), result.position))
            result.status = MusicTimelineStatus::Unrepresentable;
        return result;
    }
};

} // namespace

MusicTimelineStatus MusicPlaybackFrame(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, std::int64_t position, std::uint64_t iteration,
    std::uint64_t* elapsedFrames) noexcept {
    if (elapsedFrames == nullptr) return MusicTimelineStatus::Invalid;
    *elapsedFrames = 0;
    return Mapping(timeline, clock).Frame(position, iteration, *elapsedFrames);
}

MusicPlaybackPosition ResolveMusicPlaybackPosition(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, std::uint64_t elapsedFrames) noexcept {
    return Mapping(timeline, clock).Position(elapsedFrames);
}

MusicBoundaryResult ResolveMusicPlaybackBoundary(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, const MarkerIndex& markers, MusicBoundaryKind kind,
    std::uint64_t anchorFrame, std::uint64_t currentFrame,
    std::uint64_t publicationFloor) noexcept {
    MusicBoundaryResult result;
    const Mapping mapping(timeline, clock);
    result.status = mapping.status;
    if (mapping.status != MusicTimelineStatus::Ok) return result;
    if (currentFrame < anchorFrame) { result.status = MusicTimelineStatus::Invalid; return result; }
    if (currentFrame == UINT64_MAX) { result.status = MusicTimelineStatus::Unrepresentable; return result; }
    const auto lower = std::max(currentFrame + 1u, publicationFloor) - anchorFrame;
    std::uint64_t elapsed = 0;
    if (kind == MusicBoundaryKind::NaturalEnd) {
        if (timeline.playCount == 0) { result.status = MusicTimelineStatus::NoCandidate; return result; }
        result.iteration = timeline.playCount - 1u;
        result.position = std::max(timeline.exit, timeline.contentEnd);
        result.status = mapping.Frame(result.position, result.iteration, elapsed);
    } else if (kind == MusicBoundaryKind::ExitCue) {
        if (!timeline.hasExitCue) { result.status = MusicTimelineStatus::NoCandidate; return result; }
        result.position = timeline.exit;
        result.status = mapping.ExitAtOrAfter(lower, result.iteration, elapsed);
    } else if (kind == MusicBoundaryKind::NextBeat || kind == MusicBoundaryKind::NextBar) {
        if (!markers.Valid() || markers.TimeType() != timeline.timeType) {
            result.status = MusicTimelineStatus::Invalid; return result;
        }
        if (timeline.timeType != SegmentTimeType::Musical) {
            result.status = MusicTimelineStatus::NoCandidate; return result;
        }
        const auto position = mapping.Position(lower);
        result.status = position.status;
        if (result.status != MusicTimelineStatus::Ok) return result;
        if (position.phase == MusicPlaybackPhase::Ended) {
            result.status = MusicTimelineStatus::Missed; return result;
        }
        result.iteration = position.iteration;
        auto local = position.position;
        const auto boundaryType = kind == MusicBoundaryKind::NextBeat ? BoundaryType::Beat : BoundaryType::Bar;
        // At most the current region, the following loop and the final outro.
        for (unsigned attempt = 0; attempt != 3; ++attempt) {
            std::int64_t relative = 0;
            if (!Difference(local, timeline.entry, relative) || relative == kMin) {
                result.status = MusicTimelineStatus::Unrepresentable; return result;
            }
            auto candidate = markers.NextBoundary(relative - 1, boundaryType);
            if (!candidate.hasCandidate || !Add(timeline.entry, candidate.position, result.position)) {
                result.status = MusicTimelineStatus::Unrepresentable; return result;
            }
            result.status = mapping.Frame(result.position, result.iteration, elapsed);
            if (result.status != MusicTimelineStatus::Ok) return result;
            if (elapsed < lower) {
                candidate = markers.NextBoundary(candidate.position, boundaryType);
                if (!candidate.hasCandidate || !Add(timeline.entry, candidate.position, result.position)) {
                    result.status = MusicTimelineStatus::Unrepresentable; return result;
                }
                result.status = mapping.Frame(result.position, result.iteration, elapsed);
                if (result.status != MusicTimelineStatus::Ok) return result;
            }
            const bool moreLoops = timeline.playCount == 0 || result.iteration < timeline.playCount - 1u;
            if (moreLoops && result.position >= timeline.exit) {
                ++result.iteration;
                local = timeline.entry;
                continue;
            }
            if (!moreLoops && result.position >= std::max(timeline.exit, timeline.contentEnd)) {
                result.status = MusicTimelineStatus::NoCandidate; return result;
            }
            break;
        }
    } else { result.status = MusicTimelineStatus::Invalid; return result; }
    if (result.status != MusicTimelineStatus::Ok) return result;
    if (elapsed < lower) { result.status = MusicTimelineStatus::Missed; return result; }
    if (elapsed > UINT64_MAX - anchorFrame) { result.status = MusicTimelineStatus::Unrepresentable; return result; }
    result.frame = anchorFrame + elapsed;
    return result;
}

} // namespace harken
