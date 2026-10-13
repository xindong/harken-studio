// MarkerIndex contract tests.
//
// The index answers three kinds of question -- where is marker C, what frame is
// position P, and what is the next bar/beat/marker after here -- and the cases below
// pin each, plus the boundaries that matter: the entry sentinel is never a stored
// marker, the grid is arithmetic within the representable int64 axis, the next marker
// may not exist and says so instead of searching forever, and an over-capacity or
// malformed build is refused. Grid figures are checked against hand arithmetic,
// not the index's own.

#include "Common/ObjectIdTestSupport.h"
#include "harken/core/ClockKernel.h"
#include "harken/core/MarkerIndex.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

using harken::BoundaryType;
using harken::ClockKernel;
using harken::MarkerId;
using harken::MarkerIndex;
using harken::MarkerKind;
using harken::MarkerLookup;
using harken::MarkerPoint;
using harken::MeterSignature;
using harken::MusicalTick;
using harken::PieceTimeType;
using harken::SampleFrame;
using harken::TempoSegment;

namespace {

// 120 BPM at 48 kHz is exactly 25 frames per tick, so frame expectations are a
// plain multiplication the test can write by hand.
ClockKernel MakeMusicalClock(TempoSegment& seg) {
    seg = TempoSegment{MusicalTick{0}, 500'000};
    ClockKernel clock;
    EXPECT_TRUE(clock.Reset(48000, &seg, 1));
    return clock;
}

} // namespace

// A bar and beat are numerator*PPQ*4/denominator and PPQ*4/denominator ticks, for
// common and odd meters alike.
TEST(MarkerIndex, BarAndBeatGridForCommonAndOddMeters) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    struct Case {
        std::int32_t num;
        std::int32_t den;
        std::int64_t bar;
        std::int64_t beat;
    };
    const Case cases[] = {
        {4, 4, 3840, 960}, {3, 4, 2880, 960},
        {5, 4, 4800, 960}, {7, 8, 3360, 480}, {6, 8, 2880, 480},
    };
    for (const Case& c : cases) {
        MarkerIndex index;
        ASSERT_TRUE(index.Reset(PieceTimeType::Musical, &clock,
                                MeterSignature{c.num, c.den}, nullptr, 0, 4));
        EXPECT_EQ(index.BarTicks(), c.bar) << c.num << "/" << c.den;
        EXPECT_EQ(index.BeatTicks(), c.beat) << c.num << "/" << c.den;
    }
}

// The next bar and beat are the next representable grid line strictly after the
// position and carry the frame their tick maps to.
TEST(MarkerIndex, NextBarAndBeatAreStrictlyAfterWithinRepresentableRange) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    MarkerIndex index;
    ASSERT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            nullptr, 0, 4)); // bar 3840, beat 960

    // (from, expected next bar): a position on a bar line advances to the next.
    const std::array<std::array<std::int64_t, 2>, 4> bars = {{
        {0, 3840}, {100, 3840}, {3840, 7680}, {-100, 0},
    }};
    for (const auto& c : bars) {
        const auto b = index.NextBoundary(c[0], BoundaryType::Bar);
        EXPECT_TRUE(b.hasCandidate);
        EXPECT_EQ(b.position, c[1]) << "from " << c[0];
        EXPECT_EQ(b.frame.value, c[1] * 25); // 25 frames/tick
    }
    const auto beat = index.NextBoundary(960, BoundaryType::Beat);
    EXPECT_TRUE(beat.hasCandidate);
    EXPECT_EQ(beat.position, 1920); // next beat strictly after tick 960
}

TEST(MarkerIndex, GridReportsNoCandidateBeyondTheLastRepresentableLine) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    MarkerIndex index;
    ASSERT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            nullptr, 0, 4));

    constexpr std::int64_t beatTicks = 960;
    const std::int64_t lastBeat = (INT64_MAX / beatTicks) * beatTicks;
    const auto finalCandidate =
        index.NextBoundary(lastBeat - 1, BoundaryType::Beat);
    ASSERT_TRUE(finalCandidate.hasCandidate);
    EXPECT_EQ(finalCandidate.position, lastBeat);
    EXPECT_FALSE(index.NextBoundary(lastBeat, BoundaryType::Beat).hasCandidate);
    EXPECT_FALSE(index.NextBoundary(INT64_MAX, BoundaryType::Beat).hasCandidate);
}

// The next marker is the next stored marker by position, or an explicit no-candidate --
// a one-shot marker with nothing after it does not send the query searching forever.
TEST(MarkerIndex, NextMarkerReportsNoCandidateWhenNoneFollows) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    const MarkerPoint markers[] = {
        {harkentest::ObjectId(5), MarkerKind::Custom, 1000},
        {harkentest::ObjectId(9), MarkerKind::Custom, 5000},
    };
    MarkerIndex index;
    ASSERT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            markers, 2, 4));

    auto c0 = index.NextBoundary(500, BoundaryType::Marker);
    EXPECT_TRUE(c0.hasCandidate);
    EXPECT_EQ(c0.position, 1000);
    auto c1 = index.NextBoundary(1000, BoundaryType::Marker); // strictly after
    EXPECT_TRUE(c1.hasCandidate);
    EXPECT_EQ(c1.position, 5000);
    auto c2 = index.NextBoundary(5000, BoundaryType::Marker);
    EXPECT_FALSE(c2.hasCandidate); // no marker beyond the last
    auto c3 = index.NextBoundary(9000, BoundaryType::Marker);
    EXPECT_FALSE(c3.hasCandidate);
}

// An Absolute Piece has no musical grid: bar/beat queries report no candidate and
// the grid ticks are zero, while marker queries still work in nanoseconds.
TEST(MarkerIndex, AbsolutePieceHasNoBarBeatGrid) {
    ClockKernel clock;
    TempoSegment seg{MusicalTick{0}, 500'000};
    ASSERT_TRUE(clock.Reset(48000, &seg, 1)); // valid clock; time type drives use
    const MarkerPoint markers[] = {{harkentest::ObjectId(7), MarkerKind::Custom, 1'000'000}}; // 1 ms in ns
    MarkerIndex index;
    ASSERT_TRUE(index.Reset(PieceTimeType::Absolute, &clock, MeterSignature{4, 4},
                            markers, 1, 4));

    EXPECT_EQ(index.BarTicks(), 0);
    EXPECT_EQ(index.BeatTicks(), 0);
    EXPECT_FALSE(index.NextBoundary(0, BoundaryType::Bar).hasCandidate);
    EXPECT_FALSE(index.NextBoundary(0, BoundaryType::Beat).hasCandidate);
    auto marker = index.NextBoundary(0, BoundaryType::Marker);
    EXPECT_TRUE(marker.hasCandidate);
    EXPECT_EQ(marker.position, 1'000'000);
    // ns -> frame at 48 kHz: floor(1e6 * 48000 / 1e9) = 48.
    EXPECT_EQ(marker.frame.value, 48);
}

// Lookups by id, by position and the unique exit; the entry id 0 is never found.
TEST(MarkerIndex, LookupByIdPositionAndExit) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    const MarkerPoint markers[] = {
        {harkentest::ObjectId(5), MarkerKind::Custom, 1920},
        {harkentest::ObjectId(9), MarkerKind::Exit, 7680},
        {harkentest::ObjectId(3), MarkerKind::Custom, 960},
    };
    MarkerIndex index;
    ASSERT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            markers, 3, 8));

    const MarkerLookup byId = index.ByMarkerId(harkentest::ObjectId(5));
    ASSERT_TRUE(byId.present);
    EXPECT_EQ(byId.position, 1920);
    EXPECT_EQ(byId.frame.value, 1920 * 25);
    EXPECT_EQ(byId.kind, MarkerKind::Custom);

    EXPECT_FALSE(index.ByMarkerId(harkentest::ObjectId(0)).present);    // entry sentinel
    EXPECT_FALSE(index.ByMarkerId(harkentest::ObjectId(404)).present);  // unknown

    const MarkerLookup byPos = index.ByPosition(960);
    ASSERT_TRUE(byPos.present);
    EXPECT_EQ(byPos.kind, MarkerKind::Custom);
    EXPECT_FALSE(index.ByPosition(1000).present); // no marker exactly there

    const MarkerLookup exit = index.ExitMarker();
    ASSERT_TRUE(exit.present);
    EXPECT_EQ(exit.kind, MarkerKind::Exit);
    EXPECT_EQ(exit.position, 7680);
    EXPECT_EQ(exit.frame.value, 7680 * 25);
}

// A negative (pre-entry) marker position is carried through, not refused, and maps to
// a negative frame.
TEST(MarkerIndex, NegativeMarkerPositionCarriedThrough) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    const MarkerPoint markers[] = {{harkentest::ObjectId(4), MarkerKind::Custom, -960}};
    MarkerIndex index;
    ASSERT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            markers, 1, 4));
    const MarkerLookup c = index.ByMarkerId(harkentest::ObjectId(4));
    ASSERT_TRUE(c.present);
    EXPECT_EQ(c.position, -960);
    EXPECT_EQ(c.frame.value, -960 * 25);
}

// The index is deterministic: the same markers in a different build order query
// identically, because the table sorts by (position, id).
TEST(MarkerIndex, DeterministicRegardlessOfBuildOrder) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    const MarkerPoint forward[] = {
        {harkentest::ObjectId(3), MarkerKind::Custom, 960}, {harkentest::ObjectId(5), MarkerKind::Custom, 1920},
        {harkentest::ObjectId(9), MarkerKind::Exit, 7680}};
    const MarkerPoint reversed[] = {
        {harkentest::ObjectId(9), MarkerKind::Exit, 7680}, {harkentest::ObjectId(5), MarkerKind::Custom, 1920},
        {harkentest::ObjectId(3), MarkerKind::Custom, 960}};
    MarkerIndex a;
    MarkerIndex b;
    ASSERT_TRUE(a.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                        forward, 3, 8));
    ASSERT_TRUE(b.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                        reversed, 3, 8));
    for (std::int64_t from : {0, 960, 1920, 5000}) {
        const auto ca = a.NextBoundary(from, BoundaryType::Marker);
        const auto cb = b.NextBoundary(from, BoundaryType::Marker);
        EXPECT_EQ(ca.hasCandidate, cb.hasCandidate);
        EXPECT_EQ(ca.position, cb.position);
    }
    EXPECT_EQ(a.ByMarkerId(harkentest::ObjectId(5)).position, b.ByMarkerId(harkentest::ObjectId(5)).position);
}

// Reset refuses a build over its declared capacity, and accepts one at it.
TEST(MarkerIndex, RefusesBuildOverCapacity) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    const MarkerPoint markers[] = {
        {harkentest::ObjectId(1), MarkerKind::Custom, 100}, {harkentest::ObjectId(2), MarkerKind::Custom, 200},
        {harkentest::ObjectId(3), MarkerKind::Custom, 300}};
    MarkerIndex index;
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                             markers, 3, 2)); // three markers into capacity two
    EXPECT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            markers, 3, 3));
}

// Reset refuses malformed marker tables and meters, and a null/invalid clock.
TEST(MarkerIndex, ResetValidatesTheContract) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    MarkerIndex index;

    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, nullptr,
                             MeterSignature{4, 4}, nullptr, 0, 4)); // null clock
    ClockKernel invalid;
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &invalid,
                             MeterSignature{4, 4}, nullptr, 0, 4)); // invalid clock

    const MarkerPoint entryId[] = {{harkentest::ObjectId(0), MarkerKind::Custom, 100}};
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                             entryId, 1, 4)); // reserved entry id

    const MarkerPoint dup[] = {{harkentest::ObjectId(7), MarkerKind::Custom, 100}, {harkentest::ObjectId(7), MarkerKind::Custom, 200}};
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                             dup, 2, 4)); // duplicate id

    const MarkerPoint twoExits[] = {{harkentest::ObjectId(1), MarkerKind::Exit, 100}, {harkentest::ObjectId(2), MarkerKind::Exit, 200}};
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                             twoExits, 2, 4)); // more than one exit

    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{0, 4},
                             nullptr, 0, 4)); // non-positive numerator
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 7},
                             nullptr, 0, 4)); // denominator not dividing PPQ*4

    EXPECT_TRUE(index.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                            nullptr, 0, 4)); // an empty table is valid
    EXPECT_TRUE(index.Valid());
    EXPECT_EQ(index.MarkerCount(), 0u);
}


TEST(MarkerIndex, ResetRejectsOutOfDomainEnums) {
    TempoSegment seg;
    ClockKernel clock = MakeMusicalClock(seg);
    MarkerIndex index;

    EXPECT_FALSE(index.Reset(static_cast<PieceTimeType>(2), &clock,
                             MeterSignature{4, 4}, nullptr, 0, 4));

    const MarkerPoint invalidKind[] = {
        {harkentest::ObjectId(1), static_cast<MarkerKind>(3), 100},
    };
    EXPECT_FALSE(index.Reset(PieceTimeType::Musical, &clock,
                             MeterSignature{4, 4}, invalidKind, 1, 4));
}
