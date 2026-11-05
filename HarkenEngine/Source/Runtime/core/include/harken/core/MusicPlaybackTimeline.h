#ifndef HARKEN_CORE_MUSIC_PLAYBACK_TIMELINE_H
#define HARKEN_CORE_MUSIC_PLAYBACK_TIMELINE_H

#include "harken/core/MarkerIndex.h"

namespace harken {

// Native coordinates remain unrounded until the unfolded position is converted.
// This is a value describing one occurrence, not another transport or scheduler.
struct MusicPlaybackTimeline {
    SegmentTimeType timeType = SegmentTimeType::Musical;
    std::int64_t entry = 0;
    std::int64_t exit = 0;
    std::int64_t contentEnd = 0;
    std::uint32_t playCount = 1; // Total body passes; zero means unlimited.
    bool fromEntry = false;
    bool hasExitCue = false;
};

enum class MusicTimelineStatus : std::uint8_t {
    Ok, Invalid, Unrepresentable, NoCandidate, Missed
};

enum class MusicPlaybackPhase : std::uint8_t { Intro, Loop, Outro, Ended };

struct MusicPlaybackPosition {
    MusicTimelineStatus status = MusicTimelineStatus::Invalid;
    MusicPlaybackPhase phase = MusicPlaybackPhase::Intro;
    std::uint64_t iteration = 0; // Zero-based; retained through the outro.
    std::int64_t position = 0;  // Local authored coordinate, not unfolded time.
};

enum class MusicBoundaryKind : std::uint8_t { NaturalEnd, NextBeat, NextBar, ExitCue };

struct MusicBoundaryResult {
    MusicTimelineStatus status = MusicTimelineStatus::Invalid;
    std::uint64_t frame = 0;
    std::uint64_t iteration = 0;
    std::int64_t position = 0;
};

// The caller validates occurrence/world/transport identity before querying. All
// frames belong to that occurrence's audio transport; wall time is never input.
// Clock and index must remain immutable, valid and refer to the same fixed clock.
MusicPlaybackPosition ResolveMusicPlaybackPosition(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, std::uint64_t elapsedFrames) noexcept;

// A candidate is strictly after currentFrame and at or after publicationFloor.
// Missing opportunities, missed finite opportunities and arithmetic failure are
// distinct. These functions neither allocate nor change playback ownership.
MusicBoundaryResult ResolveMusicPlaybackBoundary(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, const MarkerIndex& markers, MusicBoundaryKind kind,
    std::uint64_t anchorFrame, std::uint64_t currentFrame,
    std::uint64_t publicationFloor) noexcept;

MusicTimelineStatus MusicPlaybackFrame(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, std::int64_t position, std::uint64_t iteration,
    std::uint64_t* elapsedFrames) noexcept;

// Find the first repeated clip with nonzero PCM width without visiting empty
// rounded passes. The range uses the occurrence's existing 32-bit iteration domain.
MusicTimelineStatus FindMusicPlaybackClipIteration(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, std::int64_t clipStart, std::int64_t clipEnd,
    std::uint32_t first, std::uint32_t last, std::uint64_t* iteration) noexcept;

// A clipped source may begin before the occurrence's playback origin. Measure
// both native endpoints on the same unfolded pass before subtracting frames.
MusicTimelineStatus MusicPlaybackFrameDistance(const MusicPlaybackTimeline& timeline,
    const ClockKernel& clock, std::int64_t from, std::int64_t to,
    std::uint64_t iteration, std::uint64_t* frames) noexcept;

} // namespace harken

#endif
