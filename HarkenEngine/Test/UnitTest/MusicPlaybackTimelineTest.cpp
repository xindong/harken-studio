#include "harken/core/MusicPlaybackTimeline.h"

#include <gtest/gtest.h>

namespace {
using namespace harken;

struct MusicClock {
    TempoSegment tempo{{0}, 500000};
    ClockKernel clock;
    MarkerIndex markers;
    explicit MusicClock(MeterSignature meter = {4, 4}, std::int64_t micros = 500000,
                        std::int64_t rate = 48000) {
        tempo.microsPerQuarter = micros;
        EXPECT_TRUE(clock.Reset(rate, &tempo, 1));
        EXPECT_TRUE(markers.Reset(SegmentTimeType::Musical, &clock, meter, nullptr, 0, 1));
    }
};

TEST(MusicPlaybackTimeline, IntroBodyFivePassesAndOutroHaveOneFinalBoundary) {
    MusicClock c;
    // 2 s intro + five 6 s passes + 4 s outro = 36 s.
    MusicPlaybackTimeline music{SegmentTimeType::Musical, 3840, 15360, 23040, 5, false, true};
    auto end = ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::NaturalEnd, 100, 100, 100);
    ASSERT_EQ(MusicTimelineStatus::Ok, end.status);
    EXPECT_EQ(100u + 36u * 48000u, end.frame);
    EXPECT_EQ(4u, end.iteration);
    auto pos = ResolveMusicPlaybackPosition(music, c.clock, 95999);
    EXPECT_EQ(MusicPlaybackPhase::Intro, pos.phase);
    pos = ResolveMusicPlaybackPosition(music, c.clock, 96000);
    EXPECT_EQ(MusicPlaybackPhase::Loop, pos.phase);
    EXPECT_EQ(3840, pos.position);
    for (std::uint64_t i = 0; i < 5; ++i) {
        const auto out = (8u + i * 6u) * 48000u;
        auto cue = ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
            MusicBoundaryKind::ExitCue, 100, 100 + out - 1, 100);
        ASSERT_EQ(MusicTimelineStatus::Ok, cue.status);
        EXPECT_EQ(100u + out, cue.frame);
        EXPECT_EQ(i, cue.iteration);
        pos = ResolveMusicPlaybackPosition(music, c.clock, out);
        EXPECT_EQ(i == 4 ? MusicPlaybackPhase::Outro : MusicPlaybackPhase::Loop, pos.phase);
        EXPECT_EQ(i == 4 ? 4 : i + 1, pos.iteration);
        EXPECT_EQ(i == 4 ? music.exit : music.entry, pos.position);
    }
    EXPECT_EQ(MusicPlaybackPhase::Ended,
        ResolveMusicPlaybackPosition(music, c.clock, 36u * 48000u).phase);
    EXPECT_EQ(MusicTimelineStatus::Missed, ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::ExitCue, 0, 32u * 48000u, 0).status);
}

TEST(MusicPlaybackTimeline, EntryStartSinglePassAndSilentPartAfterContent) {
    MusicClock c;
    MusicPlaybackTimeline music{SegmentTimeType::Musical, 960, 3840, 1920, 1, true, true};
    const auto pos = ResolveMusicPlaybackPosition(music, c.clock, 0);
    EXPECT_EQ(MusicPlaybackPhase::Loop, pos.phase);
    EXPECT_EQ(960, pos.position);
    const auto end = ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::NaturalEnd, 10, 10, 0);
    EXPECT_EQ(MusicTimelineStatus::Ok, end.status);
    EXPECT_EQ(72010u, end.frame);
    EXPECT_EQ(MusicPlaybackPhase::Loop, ResolveMusicPlaybackPosition(music, c.clock, 70000).phase);
    EXPECT_EQ(MusicPlaybackPhase::Ended, ResolveMusicPlaybackPosition(music, c.clock, 72000).phase);
}

TEST(MusicPlaybackTimeline, InfiniteExitSkipsMissedIterationsWithoutNaturalEnd) {
    MusicClock c;
    MusicPlaybackTimeline music{SegmentTimeType::Musical, 960, 3840, 7680, 0, false, true};
    EXPECT_EQ(MusicTimelineStatus::NoCandidate, ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::NaturalEnd, 0, 0, 0).status);
    const std::uint64_t iteration = 1000000000;
    const auto frame = (3840u + iteration * 2880u) * 25u;
    const auto cue = ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::ExitCue, 123, 123, 123 + frame);
    ASSERT_EQ(MusicTimelineStatus::Ok, cue.status);
    EXPECT_EQ(iteration, cue.iteration);
    EXPECT_EQ(123u + frame, cue.frame);
    const auto pos = ResolveMusicPlaybackPosition(music, c.clock, frame);
    EXPECT_EQ(MusicPlaybackPhase::Loop, pos.phase);
    EXPECT_EQ(iteration + 1u, pos.iteration);
    EXPECT_EQ(960, pos.position);
}

TEST(MusicPlaybackTimeline, OddMeterGridIsRelativeToEntryAndStrictlyAfterCurrentFrame) {
    for (const auto meter : {MeterSignature{3, 4}, MeterSignature{6, 8}}) {
        MusicClock c(meter);
        MusicPlaybackTimeline music{SegmentTimeType::Musical, 240, 5000, 9000, 5, false, true};
        const auto beatTicks = meter.denominator == 4 ? 960 : 480;
        for (const auto kind : {MusicBoundaryKind::NextBeat, MusicBoundaryKind::NextBar}) {
            const auto grid = kind == MusicBoundaryKind::NextBeat ? beatTicks : 2880;
            const auto on = static_cast<std::uint64_t>((240 + grid) * 25);
            auto next = ResolveMusicPlaybackBoundary(music, c.clock, c.markers, kind, 0, on - 1, 0);
            EXPECT_EQ(MusicTimelineStatus::Ok, next.status);
            EXPECT_EQ(on, next.frame);
            next = ResolveMusicPlaybackBoundary(music, c.clock, c.markers, kind, 0, on, 0);
            EXPECT_EQ(MusicTimelineStatus::Ok, next.status);
            EXPECT_GT(next.frame, on);
        }
        const auto out = 5000u * 25u;
        const auto next = ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
            MusicBoundaryKind::NextBar, 0, out - 10, out);
        EXPECT_EQ(MusicTimelineStatus::Ok, next.status);
        EXPECT_EQ(out, next.frame);
        EXPECT_EQ(1u, next.iteration);
        EXPECT_EQ(music.entry, next.position);
    }
}

TEST(MusicPlaybackTimeline, FractionalFrameLoopLengthsUseUnfoldedNativeCoordinates) {
    MusicClock c({4, 4}, 500001, 44100);
    MusicPlaybackTimeline music{SegmentTimeType::Musical, 13, 973, 1100, 0, false, true};
    for (const auto iteration : {0u, 1u, 100u, 1000000u}) {
        std::uint64_t frame = 0;
        ASSERT_EQ(MusicTimelineStatus::Ok,
            MusicPlaybackFrame(music, c.clock, music.exit, iteration, &frame));
        // Independent rational arithmetic; notably not iteration*rounded span.
        const auto ticks = 973ull + iteration * 960ull;
        const auto numerator = ticks * 500001ull;
        const auto expected = (numerator / 960000000ull) * 44100ull +
            (numerator % 960000000ull) * 44100ull / 960000000ull;
        EXPECT_EQ(expected, frame);
        const auto pos = ResolveMusicPlaybackPosition(music, c.clock, frame);
        EXPECT_EQ(MusicTimelineStatus::Ok, pos.status);
        EXPECT_EQ(iteration + 1u, pos.iteration);
        EXPECT_EQ(13, pos.position);
    }
}

TEST(MusicPlaybackTimeline, AbsoluteSubframeEdgesAndNegativeEntryRemainConsistent) {
    MusicClock c;
    MusicPlaybackTimeline music{SegmentTimeType::Absolute, 1000001, 2000001, 3000001, 3, false, true};
    for (std::uint64_t i = 0; i < 3; ++i) {
        std::uint64_t frame = 0;
        ASSERT_EQ(MusicTimelineStatus::Ok, MusicPlaybackFrame(music, c.clock, music.exit, i, &frame));
        const auto pos = ResolveMusicPlaybackPosition(music, c.clock, frame);
        EXPECT_EQ(MusicTimelineStatus::Ok, pos.status);
        EXPECT_EQ(i == 2 ? MusicPlaybackPhase::Outro : MusicPlaybackPhase::Loop, pos.phase);
        EXPECT_EQ(i == 2 ? 2 : i + 1, pos.iteration);
    }
    music = {SegmentTimeType::Musical, -960, 960, 1920, 1, true, true};
    EXPECT_EQ(-960, ResolveMusicPlaybackPosition(music, c.clock, 0).position);
    EXPECT_EQ(0, ResolveMusicPlaybackPosition(music, c.clock, 24000).position);
    std::uint64_t frame = 0;
    EXPECT_EQ(MusicTimelineStatus::Ok, MusicPlaybackFrame(music, c.clock, 1920, 0, &frame));
    EXPECT_EQ(72000u, frame);
}

TEST(MusicPlaybackTimeline, InvalidMissingMissedAndOverflowAreExplicit) {
    MusicClock c;
    MusicPlaybackTimeline music{SegmentTimeType::Musical, 0, 960, 1920, 1, false, false};
    EXPECT_EQ(MusicTimelineStatus::NoCandidate, ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::ExitCue, 0, 0, 0).status);
    EXPECT_EQ(MusicTimelineStatus::Missed, ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::NaturalEnd, 0, 0, 48001).status);
    EXPECT_EQ(MusicTimelineStatus::Unrepresentable, ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::NaturalEnd, UINT64_MAX - 10, UINT64_MAX - 10, 0).status);
    std::uint64_t frame = 999;
    EXPECT_EQ(MusicTimelineStatus::Unrepresentable,
        MusicPlaybackFrame(music, c.clock, 960, UINT64_MAX, &frame));
    EXPECT_EQ(0u, frame);
    music.exit = music.entry;
    EXPECT_EQ(MusicTimelineStatus::Invalid, ResolveMusicPlaybackPosition(music, c.clock, 0).status);
}

TEST(MusicPlaybackTimeline, NegativeOriginOffsetsAnUnsignedIterationProduct) {
    MusicClock c;
    MusicPlaybackTimeline music{SegmentTimeType::Absolute, -3000000000, -2000000000,
        -1000000000, 0, true, true};
    constexpr std::uint64_t iteration = 9223372037;
    constexpr std::uint64_t expected = 442721857824000;
    std::uint64_t frame = 0;
    ASSERT_EQ(MusicTimelineStatus::Ok,
        MusicPlaybackFrame(music, c.clock, music.exit, iteration, &frame));
    EXPECT_EQ(expected, frame);
    const auto cue = ResolveMusicPlaybackBoundary(music, c.clock, c.markers,
        MusicBoundaryKind::ExitCue, 0, 0, expected);
    ASSERT_EQ(MusicTimelineStatus::Ok, cue.status);
    EXPECT_EQ(iteration, cue.iteration);
    EXPECT_EQ(expected, cue.frame);
    const auto position = ResolveMusicPlaybackPosition(music, c.clock, expected - 1);
    EXPECT_EQ(MusicTimelineStatus::Ok, position.status);
    EXPECT_EQ(iteration, position.iteration);
    EXPECT_GE(position.position, music.entry);
    EXPECT_LT(position.position, music.exit);
}
} // namespace
