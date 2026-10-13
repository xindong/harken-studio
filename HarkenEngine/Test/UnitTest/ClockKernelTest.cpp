// ClockKernel time-conversion property tests.
//
// Everything scheduled downstream is a frame, and every frame comes from this
// kernel, so the properties asserted here are the ones the rest of the runtime
// gets to assume: tick -> frame -> tick round-trips exactly, the mapping never
// drifts over hours, floor and half-open boundaries land where the contract says,
// negative (anacrusis) coordinates behave like the rest of the axis, out-of-range
// inputs saturate instead of wrapping, and the same input always produces the
// same output.
//
// Exactness is checked against an INDEPENDENT reference, never against the kernel
// itself. The kernel forms its answer by 128-bit division; the reference here
// only multiplies and compares (in plain int64, gated so it never overflows),
// verifying the defining inequality f*D <= t*B < (f+1)*D that characterizes
// f = floor(t*B/D). A wrong rounding, denominator, or sign in the kernel's
// division fails the bracket; the two paths share no division code.

#include "harken/core/ClockKernel.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <random>

using harken::AbsoluteTime;
using harken::ClockKernel;
using harken::MusicalTick;
using harken::SampleFrame;
using harken::TempoSegment;

namespace {

// PPQ * microseconds-per-second: the denominator relating ticks to frames. Kept
// as a local literal so the test does not borrow the kernel's own constant.
constexpr std::int64_t kTickFrameDenom = 960 * 1'000'000; // 9.6e8
constexpr std::int64_t kTickTimeDenom = 960;

// Sample rates and tempos under test. Tempos are stored as integer microseconds
// per quarter (what the kernel holds), covering roughly 20-300 BPM including
// values that are not clean divisors, so rounding is genuinely exercised.
constexpr std::int64_t kSampleRates[] = {44100, 48000, 96000};
constexpr std::int64_t kMicrosPerQuarter[] = {
    3'000'000, // ~20 BPM
    1'500'000, // ~40 BPM
    666'667,   // ~90 BPM (not a clean divisor)
    500'000,   // 120 BPM (exactly 25 frames/tick at 48 kHz)
    428'571,   // ~140 BPM
    300'000,   // 200 BPM
    250'000,   // 240 BPM
    200'000,   // 300 BPM
    523'809,   // deliberately awkward
};

// Musical positions: subdivisions and triplets (960 divides by 3, so 320/640/160
// are exact triplet ticks), bar lines for common and odd meters (4/4=3840,
// 3/4=2880, 5/4=4800, 7/8=3360), and half-open boundary neighbours.
constexpr std::int64_t kMusicalTicks[] = {
    0,    1,    120,  160,  240,  320,  480, 640, 720,
    960,  1920, 2880, 3360, 3840, 4800, 959, 961, 3839,
    3841, 5'760'000, 13'824'000, 30'000'000, 31'000'000,
};

constexpr std::int64_t kNegativeTicks[] = {-1, -160, -320, -960, -3840, -13'824'000};

// Builds a constant-tempo kernel over a caller-owned segment. `seg` must outlive
// the returned kernel (the kernel only stores a pointer to it), so callers keep
// the segment in their own scope -- exactly how a real consumer owns its map.
ClockKernel MakeConstant(std::int64_t sampleRateHz, TempoSegment& seg) {
    ClockKernel clock;
    EXPECT_TRUE(clock.Reset(sampleRateHz, &seg, 1));
    return clock;
}

// Checked int64 multiply. Returns false on overflow instead of invoking it, so
// the bracket reference never relies on undefined behaviour.
bool MulFits(std::int64_t a, std::int64_t b, std::int64_t* out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return false;
    }
    const std::int64_t r = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
    if (r / a != b) {
        return false;
    }
    *out = r;
    return true;
}

bool AddFits(std::int64_t a, std::int64_t b, std::int64_t* out) {
    if ((b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    *out = a + b;
    return true;
}

// Asserts f == floor(dtick * (micros*sr) / D) via the bracket, in plain int64,
// skipping only if a product would overflow (it does not for the matrix here).
void ExpectFrameIsFloor(std::int64_t sampleRateHz, std::int64_t micros,
                        std::int64_t dtick, std::int64_t frame) {
    std::int64_t rate = 0;
    ASSERT_TRUE(MulFits(micros, sampleRateHz, &rate));

    std::int64_t tB = 0;
    std::int64_t fD = 0;
    std::int64_t f1D = 0;
    if (!MulFits(dtick, rate, &tB) || !MulFits(frame, kTickFrameDenom, &fD) ||
        !MulFits(frame + 1, kTickFrameDenom, &f1D)) {
        return; // out of the int64-safe range for this reference; round-trip still covers it
    }
    EXPECT_LE(fD, tB) << "frame*D must not exceed tick*rate";
    EXPECT_LT(tB, f1D) << "tick*rate must be below (frame+1)*D";
}

void ExpectMultiSegmentFrameIsAccumulatedFloor(
    std::int64_t sampleRateHz, const TempoSegment* segments,
    std::size_t segmentCount, std::int64_t tick, std::int64_t frame) {
    std::size_t index = 0;
    for (std::size_t i = 1; i < segmentCount; ++i) {
        if (segments[i].startTick.value <= tick) {
            index = i;
        } else {
            break;
        }
    }

    std::int64_t numerator = 0;
    for (std::size_t i = 0; i < index; ++i) {
        std::int64_t rate = 0;
        std::int64_t spanNumerator = 0;
        std::int64_t nextNumerator = 0;
        ASSERT_TRUE(MulFits(segments[i].microsPerQuarter, sampleRateHz, &rate));
        ASSERT_TRUE(MulFits(segments[i + 1].startTick.value -
                                segments[i].startTick.value,
                            rate, &spanNumerator));
        ASSERT_TRUE(AddFits(numerator, spanNumerator, &nextNumerator));
        numerator = nextNumerator;
    }

    std::int64_t rate = 0;
    std::int64_t spanNumerator = 0;
    std::int64_t exactNumerator = 0;
    ASSERT_TRUE(MulFits(segments[index].microsPerQuarter, sampleRateHz, &rate));
    ASSERT_TRUE(MulFits(tick - segments[index].startTick.value, rate,
                        &spanNumerator));
    ASSERT_TRUE(AddFits(numerator, spanNumerator, &exactNumerator));

    std::int64_t frameNumerator = 0;
    std::int64_t nextFrameNumerator = 0;
    ASSERT_TRUE(MulFits(frame, kTickFrameDenom, &frameNumerator));
    ASSERT_TRUE(MulFits(frame + 1, kTickFrameDenom, &nextFrameNumerator));
    EXPECT_LE(frameNumerator, exactNumerator);
    EXPECT_LT(exactNumerator, nextFrameNumerator);
}

void ExpectMultiSegmentTimeIsAccumulatedFloor(
    const TempoSegment* segments, std::size_t segmentCount,
    std::int64_t tick, std::int64_t timeNs) {
    std::size_t index = 0;
    for (std::size_t i = 1; i < segmentCount; ++i) {
        if (segments[i].startTick.value <= tick) {
            index = i;
        } else {
            break;
        }
    }

    std::int64_t numerator = 0;
    for (std::size_t i = 0; i < index; ++i) {
        std::int64_t nanosPerQuarter = 0;
        std::int64_t spanNumerator = 0;
        std::int64_t nextNumerator = 0;
        ASSERT_TRUE(MulFits(segments[i].microsPerQuarter, 1000,
                            &nanosPerQuarter));
        ASSERT_TRUE(MulFits(segments[i + 1].startTick.value -
                                segments[i].startTick.value,
                            nanosPerQuarter, &spanNumerator));
        ASSERT_TRUE(AddFits(numerator, spanNumerator, &nextNumerator));
        numerator = nextNumerator;
    }

    std::int64_t nanosPerQuarter = 0;
    std::int64_t spanNumerator = 0;
    std::int64_t exactNumerator = 0;
    ASSERT_TRUE(MulFits(segments[index].microsPerQuarter, 1000,
                        &nanosPerQuarter));
    ASSERT_TRUE(MulFits(tick - segments[index].startTick.value,
                        nanosPerQuarter, &spanNumerator));
    ASSERT_TRUE(AddFits(numerator, spanNumerator, &exactNumerator));

    std::int64_t timeNumerator = 0;
    std::int64_t nextTimeNumerator = 0;
    ASSERT_TRUE(MulFits(timeNs, kTickTimeDenom, &timeNumerator));
    ASSERT_TRUE(MulFits(timeNs + 1, kTickTimeDenom, &nextTimeNumerator));
    EXPECT_LE(timeNumerator, exactNumerator);
    EXPECT_LT(exactNumerator, nextTimeNumerator);
}

TEST(ClockKernel, CheckedSpanConversionRejectsInvalidAndPreservesEndpoints) {
    const harken::TempoSegment segment{harken::MusicalTick{0}, 500000};
    harken::ClockKernel clock;
    ASSERT_TRUE(clock.Reset(48000, &segment, 1));
    std::uint64_t frames = 99;
    EXPECT_FALSE(clock.TryTimeSpanToFrames(harken::AbsoluteTime{0}, -1,
                                           &frames));
    EXPECT_EQ(frames, 99u);
    ASSERT_TRUE(clock.TryTimeSpanToFrames(harken::AbsoluteTime{0}, 1000000000,
                                          &frames));
    EXPECT_EQ(frames, 48000u);
    ASSERT_TRUE(clock.TryTickSpanToFrames(harken::MusicalTick{0}, 960,
                                          &frames));
    EXPECT_EQ(frames, 24000u);
    EXPECT_FALSE(clock.TryTimeSpanToFrames(
        harken::AbsoluteTime{INT64_MAX - 1}, 2, &frames));
    EXPECT_FALSE(clock.TryTickSpanToFrames(
        harken::MusicalTick{INT64_MAX - 1}, 2, &frames));
}

} // namespace

// tick -> frame -> tick recovers the original tick for every sample rate, tempo
// and musical position, including triplets and odd-meter bar lines. In this
// matrix frames-per-tick is always >= 9, so the mapping is injective and the
// round trip is exact.
TEST(ClockKernel, MusicalRoundTripIsExactAcrossTheMatrix) {
    for (std::int64_t sr : kSampleRates) {
        for (std::int64_t micros : kMicrosPerQuarter) {
            TempoSegment seg{MusicalTick{0}, micros};
            ClockKernel clock = MakeConstant(sr, seg);

            for (std::int64_t tick : kMusicalTicks) {
                const SampleFrame frame = clock.TickToFrame(MusicalTick{tick});
                const MusicalTick back = clock.FrameToTick(frame);
                EXPECT_EQ(back.value, tick)
                    << "sr=" << sr << " micros=" << micros << " tick=" << tick;
                ExpectFrameIsFloor(sr, micros, tick, frame.value);
            }
        }
    }
}

// The same for negative (anacrusis) positions: a weak-up before the entry
// downbeat maps to a negative frame and round-trips exactly.
TEST(ClockKernel, AnacrusisMapsToNegativeFramesAndRoundTrips) {
    for (std::int64_t sr : kSampleRates) {
        for (std::int64_t micros : kMicrosPerQuarter) {
            TempoSegment seg{MusicalTick{0}, micros};
            ClockKernel clock = MakeConstant(sr, seg);

            for (std::int64_t tick : kNegativeTicks) {
                const SampleFrame frame = clock.TickToFrame(MusicalTick{tick});
                EXPECT_LT(frame.value, 0)
                    << "sr=" << sr << " micros=" << micros << " tick=" << tick;
                EXPECT_EQ(clock.FrameToTick(frame).value, tick);
                ExpectFrameIsFloor(sr, micros, tick, frame.value);
            }
        }
    }
}

// Entry is the shared origin: tick 0 is frame 0, and frame 0 is tick 0.
TEST(ClockKernel, EntryIsTheOrigin) {
    TempoSegment seg{MusicalTick{0}, 500'000};
    ClockKernel clock = MakeConstant(48000, seg);
    EXPECT_EQ(clock.TickToFrame(MusicalTick{0}).value, 0);
    EXPECT_EQ(clock.FrameToTick(SampleFrame{0}).value, 0);
    EXPECT_EQ(clock.TimeToFrame(AbsoluteTime{0}).value, 0);
    EXPECT_EQ(clock.FrameToTime(SampleFrame{0}).value, 0);
}

// A large deterministic fuzz over ticks confirms the round trip and the floor
// bracket hold well beyond the hand-picked positions. Fixed seed: the corpus is
// the same on every run and every machine.
TEST(ClockKernel, MusicalRoundTripFuzz) {
    std::mt19937_64 rng(0x9E3779B97F4A7C15ull);
    std::uniform_int_distribution<std::int64_t> ticks(-20'000'000, 20'000'000);

    for (std::int64_t sr : kSampleRates) {
        for (std::int64_t micros : kMicrosPerQuarter) {
            TempoSegment seg{MusicalTick{0}, micros};
            ClockKernel clock = MakeConstant(sr, seg);
            for (int i = 0; i < 2000; ++i) {
                const std::int64_t tick = ticks(rng);
                const SampleFrame frame = clock.TickToFrame(MusicalTick{tick});
                EXPECT_EQ(clock.FrameToTick(frame).value, tick)
                    << "sr=" << sr << " micros=" << micros << " tick=" << tick;
                ExpectFrameIsFloor(sr, micros, tick, frame.value);
            }
        }
    }
}

// Half-open interval semantics: every frame in [TickToFrame(t), TickToFrame(t+1))
// resolves to tick t, and the upper end resolves to t+1. The boundary belongs to
// the later tick.
TEST(ClockKernel, FrameToTickIsHalfOpen) {
    TempoSegment seg{MusicalTick{0}, 428'571}; // ~140 BPM, non-integer K
    ClockKernel clock = MakeConstant(44100, seg);

    for (std::int64_t tick : {0, 1, 320, 960, 3840, -320, -960}) {
        const std::int64_t lo = clock.TickToFrame(MusicalTick{tick}).value;
        const std::int64_t hi = clock.TickToFrame(MusicalTick{tick + 1}).value;
        ASSERT_LT(lo, hi) << "expected >1 frame per tick at this tempo";

        EXPECT_EQ(clock.FrameToTick(SampleFrame{lo}).value, tick);
        EXPECT_EQ(clock.FrameToTick(SampleFrame{hi - 1}).value, tick)
            << "frame just below the boundary is still the earlier tick";
        EXPECT_EQ(clock.FrameToTick(SampleFrame{hi}).value, tick + 1)
            << "the boundary frame belongs to the later tick";
    }
}

// No accumulated drift over an hours-long span: at 120 BPM / 48 kHz the mapping
// is exactly 25 frames per tick, so a two-hour position has an exact closed-form
// frame with no rounding at all -- computed here from seconds, independently.
TEST(ClockKernel, NoDriftExactTempoOverHours) {
    TempoSegment seg{MusicalTick{0}, 500'000};
    ClockKernel clock = MakeConstant(48000, seg);

    // 120 BPM = 2 beats/s, 960 PPQ => 1920 ticks/s.
    for (std::int64_t seconds : {60, 600, 3600, 7200, 36000}) {
        const std::int64_t tick = seconds * 1920;
        const std::int64_t expectedFrame = seconds * 48000; // seconds * sample rate
        EXPECT_EQ(clock.TickToFrame(MusicalTick{tick}).value, expectedFrame)
            << "seconds=" << seconds;
        EXPECT_EQ(clock.FrameToTick(SampleFrame{expectedFrame}).value, tick);
    }
}

// No drift for a tempo whose frames-per-tick is NOT an integer: at hours scale
// the frame still satisfies the exact floor bracket, i.e. the error against the
// true rational position never exceeds one frame however far out we go.
TEST(ClockKernel, NoDriftAwkwardTempoOverHours) {
    for (std::int64_t sr : kSampleRates) {
        TempoSegment seg{MusicalTick{0}, 523'809};
        ClockKernel clock = MakeConstant(sr, seg);
        for (std::int64_t seconds : {600, 3600, 7200, 18000, 36000}) {
            const std::int64_t tick = seconds * 2000; // arbitrary hours-scale tick
            const SampleFrame frame = clock.TickToFrame(MusicalTick{tick});
            ExpectFrameIsFloor(sr, 523'809, tick, frame.value);
            EXPECT_EQ(clock.FrameToTick(frame).value, tick);
        }
    }
}

// Absolute nanoseconds convert independently of the tempo map: only the sample
// rate relates ns to frames. frame -> time -> frame recovers the frame exactly
// (ns are finer than frames, so this is the lossless direction).
TEST(ClockKernel, AbsoluteRoundTripAndTempoIndependence) {
    for (std::int64_t sr : kSampleRates) {
        TempoSegment slowSeg{MusicalTick{0}, 3'000'000};
        TempoSegment fastSeg{MusicalTick{0}, 200'000};
        ClockKernel slow = MakeConstant(sr, slowSeg);
        ClockKernel fast = MakeConstant(sr, fastSeg);

        for (std::int64_t frame : {0, 1, 47, 48000, -1, -48000, 172'800'000}) {
            const AbsoluteTime t = slow.FrameToTime(SampleFrame{frame});
            EXPECT_EQ(slow.TimeToFrame(t).value, frame) << "sr=" << sr;
            // Absolute conversion must not depend on the tempo map.
            EXPECT_EQ(fast.FrameToTime(SampleFrame{frame}).value, t.value);
            EXPECT_EQ(fast.TimeToFrame(t).value, frame);
        }
    }
}

// A multi-segment tempo map exercises the map/segment interface (not special-
// cased for the constant case): anchors accumulate exactly, positions in each
// segment convert against that segment's tempo, the boundary is continuous, and
// round trips still hold.
TEST(ClockKernel, MultiSegmentTempoMap) {
    // 120 BPM until tick 3840 (bar 2 of 4/4), then 60 BPM. At 48 kHz: 25
    // frames/tick then 50 frames/tick.
    const std::array<TempoSegment, 2> segs = {
        TempoSegment{MusicalTick{0}, 500'000},
        TempoSegment{MusicalTick{3840}, 1'000'000},
    };
    ClockKernel clock;
    ASSERT_TRUE(clock.Reset(48000, segs.data(), segs.size()));

    // First segment: 25 frames/tick.
    EXPECT_EQ(clock.TickToFrame(MusicalTick{0}).value, 0);
    EXPECT_EQ(clock.TickToFrame(MusicalTick{960}).value, 960 * 25);
    // Boundary anchor: 3840 ticks * 25 = 96000 frames.
    const std::int64_t boundary = 3840 * 25;
    EXPECT_EQ(clock.TickToFrame(MusicalTick{3840}).value, boundary);
    // Second segment: 50 frames/tick beyond the anchor.
    EXPECT_EQ(clock.TickToFrame(MusicalTick{3840 + 960}).value,
              boundary + 960 * 50);

    // Continuity and round trips across the boundary.
    for (std::int64_t tick : {0, 1, 960, 3839, 3840, 3841, 4800, 9600}) {
        const SampleFrame frame = clock.TickToFrame(MusicalTick{tick});
        EXPECT_EQ(clock.FrameToTick(frame).value, tick) << "tick=" << tick;
    }
    // A frame exactly at the boundary resolves to the boundary tick (half-open).
    EXPECT_EQ(clock.FrameToTick(SampleFrame{boundary}).value, 3840);
    EXPECT_EQ(clock.FrameToTick(SampleFrame{boundary - 1}).value, 3839);
}

TEST(ClockKernel, MultiSegmentRemainderCarriesAcrossTempoBoundaries) {
    const std::array<TempoSegment, 2> segs = {
        TempoSegment{MusicalTick{0}, 500'000},
        TempoSegment{MusicalTick{1}, 1'000'000},
    };
    ClockKernel clock;
    ASSERT_TRUE(clock.Reset(44100, segs.data(), segs.size()));

    EXPECT_EQ(clock.TickToFrame(MusicalTick{1}).value, 22);
    EXPECT_EQ(clock.TickToFrame(MusicalTick{2}).value, 68);
    EXPECT_EQ(clock.FrameToTick(SampleFrame{67}).value, 1);
    EXPECT_EQ(clock.FrameToTick(SampleFrame{68}).value, 2);
}

TEST(ClockKernel, MultiSegmentAwkwardTemposMatchExactAccumulatedFloor) {
    const std::array<std::array<TempoSegment, 4>, 3> maps = {{
        {{TempoSegment{MusicalTick{0}, 500'000},
          TempoSegment{MusicalTick{1}, 1'000'000},
          TempoSegment{}, TempoSegment{}}},
        {{TempoSegment{MusicalTick{0}, 666'667},
          TempoSegment{MusicalTick{2}, 428'571},
          TempoSegment{MusicalTick{5}, 523'809},
          TempoSegment{}}},
        {{TempoSegment{MusicalTick{0}, 523'809},
          TempoSegment{MusicalTick{1}, 666'667},
          TempoSegment{MusicalTick{4}, 428'571},
          TempoSegment{MusicalTick{9}, 500'001}}},
    }};
    constexpr std::array<std::size_t, 3> counts = {2, 3, 4};

    for (std::size_t mapIndex = 0; mapIndex < maps.size(); ++mapIndex) {
        ClockKernel clock;
        ASSERT_TRUE(clock.Reset(44100, maps[mapIndex].data(), counts[mapIndex]));
        for (std::int64_t tick = -8; tick <= 24; ++tick) {
            const SampleFrame frame = clock.TickToFrame(MusicalTick{tick});
            ExpectMultiSegmentFrameIsAccumulatedFloor(
                44100, maps[mapIndex].data(), counts[mapIndex], tick,
                frame.value);
            EXPECT_EQ(clock.FrameToTick(frame).value, tick)
                << "map=" << mapIndex << " tick=" << tick;
        }
    }
}

TEST(ClockKernel, TickToTimeMatchesExactAccumulatedFloor) {
    const std::array<std::array<TempoSegment, 4>, 3> maps = {{
        {{TempoSegment{MusicalTick{0}, 500'000},
          TempoSegment{MusicalTick{1}, 1'000'000},
          TempoSegment{}, TempoSegment{}}},
        {{TempoSegment{MusicalTick{0}, 666'667},
          TempoSegment{MusicalTick{2}, 428'571},
          TempoSegment{MusicalTick{5}, 523'809},
          TempoSegment{}}},
        {{TempoSegment{MusicalTick{0}, 523'809},
          TempoSegment{MusicalTick{1}, 666'667},
          TempoSegment{MusicalTick{4}, 428'571},
          TempoSegment{MusicalTick{9}, 500'001}}},
    }};
    constexpr std::array<std::size_t, 3> counts = {2, 3, 4};

    for (std::size_t mapIndex = 0; mapIndex < maps.size(); ++mapIndex) {
        ClockKernel clock;
        ASSERT_TRUE(clock.Reset(44'100, maps[mapIndex].data(), counts[mapIndex]));
        for (std::int64_t tick = -8; tick <= 24; ++tick) {
            const AbsoluteTime time = clock.TickToTime(MusicalTick{tick});
            ExpectMultiSegmentTimeIsAccumulatedFloor(
                maps[mapIndex].data(), counts[mapIndex], tick, time.value);
        }
    }
}

// Out-of-range inputs saturate to the int64 extremes rather than wrapping. With
// frames-per-tick >= 1, a tick near INT64_MAX has a frame past the range and
// clamps; the negative extreme clamps the other way. FrameToTime near the
// extremes clamps too, since a frame's nanosecond exceeds int64 there.
TEST(ClockKernel, SaturatesInsteadOfWrapping) {
    TempoSegment seg{MusicalTick{0}, 500'000}; // 25 frames/tick
    ClockKernel clock = MakeConstant(48000, seg);

    EXPECT_EQ(clock.TickToFrame(MusicalTick{INT64_MAX}).value, INT64_MAX);
    EXPECT_EQ(clock.TickToFrame(MusicalTick{INT64_MIN}).value, INT64_MIN);
    EXPECT_EQ(clock.TickToTime(MusicalTick{INT64_MAX}).value, INT64_MAX);
    EXPECT_EQ(clock.TickToTime(MusicalTick{INT64_MIN}).value, INT64_MIN);

    // frame * 1e9 / sr overflows int64 well before INT64_MAX frames.
    EXPECT_EQ(clock.FrameToTime(SampleFrame{INT64_MAX}).value, INT64_MAX);
    EXPECT_EQ(clock.FrameToTime(SampleFrame{INT64_MIN}).value, INT64_MIN);

    // TimeToFrame at the ns extreme stays defined (does not wrap negative).
    EXPECT_GT(clock.TimeToFrame(AbsoluteTime{INT64_MAX}).value, 0);
    EXPECT_LT(clock.TimeToFrame(AbsoluteTime{INT64_MIN}).value, 0);
}

// Identical inputs give identical outputs, on this run and any other: pure
// integer arithmetic with no per-call state. Two independently reset kernels
// agree, so there is no hidden global influencing the result.
TEST(ClockKernel, DeterministicAndStateless) {
    TempoSegment segA{MusicalTick{0}, 428'571};
    TempoSegment segB{MusicalTick{0}, 428'571};
    ClockKernel a = MakeConstant(44100, segA);
    ClockKernel b = MakeConstant(44100, segB);

    std::mt19937_64 rng(0xD1B54A32D192ED03ull);
    std::uniform_int_distribution<std::int64_t> ticks(-5'000'000, 5'000'000);
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t tick = ticks(rng);
        const std::int64_t fa = a.TickToFrame(MusicalTick{tick}).value;
        const std::int64_t fb = b.TickToFrame(MusicalTick{tick}).value;
        EXPECT_EQ(fa, fb);
        // Recomputing on the same kernel yields the same value.
        EXPECT_EQ(a.TickToFrame(MusicalTick{tick}).value, fa);
    }
}

// Reset rejects malformed maps and out-of-domain parameters, and an invalid
// kernel maps everything to zero rather than reading uninitialized state.
TEST(ClockKernel, ResetValidatesTheContract) {
    ClockKernel clock;
    EXPECT_FALSE(clock.Valid());
    EXPECT_EQ(clock.TickToFrame(MusicalTick{1000}).value, 0);
    EXPECT_EQ(clock.TickToTime(MusicalTick{1000}).value, 0);

    // Empty / null map.
    EXPECT_FALSE(clock.Reset(48000, nullptr, 0));
    const TempoSegment one{MusicalTick{0}, 500'000};
    EXPECT_FALSE(clock.Reset(48000, &one, 0));

    // First segment must start at tick 0.
    const TempoSegment offset{MusicalTick{960}, 500'000};
    EXPECT_FALSE(clock.Reset(48000, &offset, 1));

    // Sample rate out of domain.
    EXPECT_FALSE(clock.Reset(0, &one, 1));
    EXPECT_FALSE(clock.Reset(3'000'001, &one, 1));

    // Tempo out of domain.
    const TempoSegment zeroTempo{MusicalTick{0}, 0};
    EXPECT_FALSE(clock.Reset(48000, &zeroTempo, 1));
    const TempoSegment hugeTempo{MusicalTick{0}, 60'000'000'001};
    EXPECT_FALSE(clock.Reset(48000, &hugeTempo, 1));

    // Non-ascending segment boundaries.
    const std::array<TempoSegment, 2> descending = {
        TempoSegment{MusicalTick{0}, 500'000},
        TempoSegment{MusicalTick{0}, 400'000},
    };
    EXPECT_FALSE(clock.Reset(48000, descending.data(), descending.size()));

    // A valid map makes the kernel usable.
    EXPECT_TRUE(clock.Reset(48000, &one, 1));
    EXPECT_TRUE(clock.Valid());
    EXPECT_EQ(clock.SampleRateHz(), 48000);
    EXPECT_EQ(clock.SegmentCount(), 1u);
}

TEST(ClockKernel, ResetRequiresAtLeastOneFramePerTick) {
    ClockKernel clock;
    const TempoSegment tooFast{MusicalTick{0}, 1};
    EXPECT_FALSE(clock.Reset(44100, &tooFast, 1));

    const TempoSegment belowBoundary{MusicalTick{0}, 19'999};
    EXPECT_FALSE(clock.Reset(48000, &belowBoundary, 1));

    const TempoSegment exactBoundary{MusicalTick{0}, 20'000};
    ASSERT_TRUE(clock.Reset(48000, &exactBoundary, 1));
    EXPECT_EQ(clock.TickToFrame(MusicalTick{1}).value, 1);
    EXPECT_EQ(clock.FrameToTick(SampleFrame{1}).value, 1);

    const std::array<TempoSegment, 2> invalidLaterSegment = {
        TempoSegment{MusicalTick{0}, 500'000},
        TempoSegment{MusicalTick{960}, 19'999},
    };
    EXPECT_FALSE(clock.Reset(48000, invalidLaterSegment.data(),
                             invalidLaterSegment.size()));
}
