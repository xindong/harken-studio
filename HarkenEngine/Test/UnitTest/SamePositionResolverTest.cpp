// SamePositionResolver golden matrix.
//
// The exit gate names the coverage: target nominal duration shorter than, longer
// than and equal to the source position; Musical-to-Musical across two tempos;
// Absolute-to-Absolute; Musical-to-Absolute both ways; negative entry-relative
// positions; and the exact-multiple half-open boundary that must land on 0. Every
// resolved position is checked against a hand-written signed floor-mod -- the
// resolver's own arithmetic under test -- and the resulting frame against the
// already-verified ClockKernel, which the resolver delegates its conversions to
// (it has no conversion code of its own to share).

#include "harken/core/ClockKernel.h"
#include "harken/core/SamePositionResolver.h"

#include <gtest/gtest.h>

#include <cstdint>

using harken::AbsoluteTime;
using harken::AreReuseCompatible;
using harken::ClockKernel;
using harken::CompatibilityKey;
using harken::MusicalTick;
using harken::PieceTimeType;
using harken::ResolveSamePosition;
using harken::SamePositionKind;
using harken::SamePositionOutcome;
using harken::SamePositionReject;
using harken::SampleFrame;
using harken::TempoSegment;

namespace {

// Independent signed floor-mod, written a different way than the resolver's, as
// the reference for every resolved position.
std::int64_t FloorModRef(std::int64_t a, std::int64_t m) {
    const std::int64_t remainder = a % m;
    return remainder < 0 ? remainder + m : remainder;
}

ClockKernel MakeClock(std::int64_t sampleRate, std::int64_t micros,
                      TempoSegment& seg) {
    seg = TempoSegment{MusicalTick{0}, micros};
    ClockKernel clock;
    EXPECT_TRUE(clock.Reset(sampleRate, &seg, 1));
    return clock;
}

// A distinct, reusable key: seekable, a positive duration, all fields set so a
// short-circuit needs a deliberate match rather than shared defaults.
CompatibilityKey MakeKey(std::uint64_t identity, std::uint64_t generation,
                         std::int64_t nominalDuration, bool seekable = true) {
    CompatibilityKey k;
    k.stableIdentity = identity;
    k.programGeneration = generation;
    k.mediaVariant = 100 + identity;
    k.bakedEntry = 200 + identity;
    k.sourceLoopRegionStart = 0;
    k.sourceLoopRegionEnd = nominalDuration;
    k.loopPolicy = 1;
    k.loopIteration = 0;
    k.nominalTimeOrigin = 0;
    k.nominalDuration = nominalDuration;
    k.routeTarget = 300 + identity;
    k.envelopeId = 400 + identity;
    k.seekable = seekable;
    k.clockMappingId = 500 + identity;
    return k;
}

} // namespace

// The new winner is the same stable identity at the same program generation: the
// incumbent keeps playing, even when other key fields differ.
TEST(SamePositionResolver, ContinueCurrentOnSameIdentityAndGeneration) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa);
    ClockKernel tgt = MakeClock(48000, 500'000, ta);

    CompatibilityKey source = MakeKey(1, 1, 1000);
    CompatibilityKey target = MakeKey(1, 1, 400); // same identity+gen, other diffs
    target.routeTarget = 999;                     // deliberately different route

    const SamePositionOutcome out = ResolveSamePosition(
        source, target, PieceTimeType::Musical, src, PieceTimeType::Musical, tgt,
        1000);
    EXPECT_EQ(out.kind, SamePositionKind::ContinueCurrent);
}

// A differing program generation is a genuinely different winner: it hands off,
// it does not continue.
TEST(SamePositionResolver, DifferentGenerationHandsOff) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa);
    ClockKernel tgt = MakeClock(48000, 500'000, ta);

    const SamePositionOutcome out = ResolveSamePosition(
        MakeKey(1, 1, 400), MakeKey(1, 2, 400), PieceTimeType::Musical, src,
        PieceTimeType::Musical, tgt, 1000);
    EXPECT_EQ(out.kind, SamePositionKind::Handoff);
}

// Musical to Musical across two tempos: the position stays in ticks (floor-mod
// into the target's tick duration) and each side converts through its own tempo
// map. Covers shorter/longer/equal durations, a negative source, and an exact
// multiple landing on 0.
TEST(SamePositionResolver, MusicalToMusicalAcrossTempos) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa);  // 25 frames/tick
    ClockKernel tgt = MakeClock(48000, 1'000'000, ta); // 60 BPM => 50 frames/tick

    struct Case {
        std::int64_t sourceTick;
        std::int64_t targetDurTicks;
        std::int64_t expectPos;
    };
    const Case cases[] = {
        {1000, 400, 200}, // duration shorter than position
        {200, 1000, 200}, // duration longer than position
        {400, 400, 0},    // equal: exact multiple lands on 0
        {-100, 400, 300}, // negative source wraps up
        {800, 400, 0},    // exact multiple lands on 0
    };
    for (const Case& c : cases) {
        const SamePositionOutcome out = ResolveSamePosition(
            MakeKey(1, 1, 0), MakeKey(2, 1, c.targetDurTicks),
            PieceTimeType::Musical, src, PieceTimeType::Musical, tgt,
            c.sourceTick);
        ASSERT_EQ(out.kind, SamePositionKind::Handoff);
        EXPECT_TRUE(out.targetPositionIsTicks);
        EXPECT_EQ(out.targetPosition, FloorModRef(c.sourceTick, c.targetDurTicks));
        EXPECT_EQ(out.targetPosition, c.expectPos);
        EXPECT_GE(out.targetPosition, 0);
        EXPECT_LT(out.targetPosition, c.targetDurTicks);
        EXPECT_EQ(out.targetFrame.value,
                  tgt.TickToFrame(MusicalTick{out.targetPosition}).value);
    }
}

// Absolute to Absolute: the shared unit is nanoseconds; floor-mod into the target
// ns duration, frame through the sample rate.
TEST(SamePositionResolver, AbsoluteToAbsolute) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa);
    ClockKernel tgt = MakeClock(48000, 500'000, ta);

    struct Case {
        std::int64_t sourceNs;
        std::int64_t targetDurNs;
    };
    const Case cases[] = {
        {100'000'000, 40'000'000},  // shorter
        {20'000'000, 100'000'000},  // longer
        {40'000'000, 40'000'000},   // equal -> 0
        {-10'000'000, 40'000'000},  // negative wraps up
        {80'000'000, 40'000'000},   // exact multiple -> 0
        {INT64_MIN, INT64_MAX},      // negative extreme without reference overflow
    };
    for (const Case& c : cases) {
        const SamePositionOutcome out = ResolveSamePosition(
            MakeKey(1, 1, 0), MakeKey(2, 1, c.targetDurNs),
            PieceTimeType::Absolute, src, PieceTimeType::Absolute, tgt,
            c.sourceNs);
        ASSERT_EQ(out.kind, SamePositionKind::Handoff);
        EXPECT_FALSE(out.targetPositionIsTicks);
        EXPECT_EQ(out.targetPosition, FloorModRef(c.sourceNs, c.targetDurNs));
        EXPECT_GE(out.targetPosition, 0);
        EXPECT_LT(out.targetPosition, c.targetDurNs);
        EXPECT_EQ(out.targetFrame.value,
                  tgt.TimeToFrame(AbsoluteTime{out.targetPosition}).value);
    }
}

// Musical source into an Absolute target: the shared unit falls back to
// nanoseconds. The source's tick position is expressed in ns through its clock,
// then floor-mod into the target ns duration.
TEST(SamePositionResolver, MusicalSourceToAbsoluteTarget) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa); // Musical source
    ClockKernel tgt = MakeClock(48000, 500'000, ta); // Absolute target (clock for ns)

    const std::int64_t sourceTick = 100;
    const std::int64_t targetDurNs = 40'000'000;
    // floor(100 * 500000 * 1000 / 960), calculated independently of the clock.
    const std::int64_t sourceNs = 52'083'333;

    const SamePositionOutcome out = ResolveSamePosition(
        MakeKey(1, 1, 0), MakeKey(2, 1, targetDurNs), PieceTimeType::Musical, src,
        PieceTimeType::Absolute, tgt, sourceTick);
    ASSERT_EQ(out.kind, SamePositionKind::Handoff);
    EXPECT_FALSE(out.targetPositionIsTicks);
    EXPECT_EQ(out.targetPosition, FloorModRef(sourceNs, targetDurNs));
    EXPECT_EQ(out.targetFrame.value,
              tgt.TimeToFrame(AbsoluteTime{out.targetPosition}).value);
}

TEST(SamePositionResolver,
     MusicalSourceKeepsSubframePrecisionWhenFoldingIntoAbsoluteTarget) {
    TempoSegment sourceSegment;
    TempoSegment targetSegment;
    ClockKernel sourceClock = MakeClock(44'100, 500'000, sourceSegment);
    ClockKernel targetClock = MakeClock(44'100, 500'000, targetSegment);

    constexpr std::int64_t exactThreeTickDurationNs = 1'562'500;
    const SamePositionOutcome out = ResolveSamePosition(
        MakeKey(1, 1, 0), MakeKey(2, 1, exactThreeTickDurationNs),
        PieceTimeType::Musical, sourceClock, PieceTimeType::Absolute,
        targetClock, 3);

    ASSERT_EQ(out.kind, SamePositionKind::Handoff);
    EXPECT_FALSE(out.targetPositionIsTicks);
    EXPECT_EQ(out.targetPosition, 0);
    EXPECT_EQ(out.targetFrame.value, 0);
}

// Absolute source into a Musical target: the target's tick duration is expressed
// in ns through its clock, the source ns folds into it, and the frame is the ns
// mapping (the wall-clock-aligned start of the musical target).
TEST(SamePositionResolver, AbsoluteSourceToMusicalTarget) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa); // Absolute source (clock for ns)
    ClockKernel tgt = MakeClock(48000, 500'000, ta); // Musical target

    const std::int64_t sourceNs = 1'200'000'000; // 1.2 s
    const std::int64_t targetDurTicks = 960;
    // 960 ticks at 500000 microseconds per quarter is exactly 500 ms.
    const std::int64_t targetDurNs = 500'000'000;

    const SamePositionOutcome out = ResolveSamePosition(
        MakeKey(1, 1, 0), MakeKey(2, 1, targetDurTicks), PieceTimeType::Absolute,
        src, PieceTimeType::Musical, tgt, sourceNs);
    ASSERT_EQ(out.kind, SamePositionKind::Handoff);
    EXPECT_FALSE(out.targetPositionIsTicks);
    EXPECT_EQ(out.targetPosition, FloorModRef(sourceNs, targetDurNs));
    EXPECT_GE(out.targetPosition, 0);
    EXPECT_LT(out.targetPosition, targetDurNs);
    EXPECT_EQ(out.targetFrame.value,
              tgt.TimeToFrame(AbsoluteTime{out.targetPosition}).value);
}

// A target that cannot be seeked is refused before arming, not guessed at.
TEST(SamePositionResolver, RejectsNotSeekable) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa);
    ClockKernel tgt = MakeClock(48000, 500'000, ta);
    const SamePositionOutcome out = ResolveSamePosition(
        MakeKey(1, 1, 400), MakeKey(2, 1, 400, /*seekable=*/false),
        PieceTimeType::Musical, src, PieceTimeType::Musical, tgt, 100);
    EXPECT_EQ(out.kind, SamePositionKind::Rejected);
    EXPECT_EQ(out.reject, SamePositionReject::NotSeekable);
}

// A non-positive nominal duration is refused (no interval to fold into), never
// resolved to the target entry.
TEST(SamePositionResolver, RejectsMissingNominalDuration) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(48000, 500'000, sa);
    ClockKernel tgt = MakeClock(48000, 500'000, ta);
    EXPECT_EQ(ResolveSamePosition(MakeKey(1, 1, 400), MakeKey(2, 1, 0),
                                  PieceTimeType::Musical, src,
                                  PieceTimeType::Musical, tgt, 100)
                  .reject,
              SamePositionReject::MissingNominalDuration);
    EXPECT_EQ(ResolveSamePosition(MakeKey(1, 1, 400), MakeKey(2, 1, -5),
                                  PieceTimeType::Musical, src,
                                  PieceTimeType::Musical, tgt, 100)
                  .reject,
              SamePositionReject::MissingNominalDuration);
}

// Clocks that cannot be reconciled -- a different sample rate, or an invalid clock
// -- refuse the handoff; a same-rate tempo difference does not (that is a normal
// Musical-to-Musical handoff).
TEST(SamePositionResolver, RejectsIncompatibleClockMapping) {
    TempoSegment sa;
    TempoSegment ta;
    ClockKernel src = MakeClock(44100, 500'000, sa);
    ClockKernel tgt = MakeClock(48000, 500'000, ta);
    ClockKernel invalid; // never Reset

    EXPECT_EQ(ResolveSamePosition(MakeKey(1, 1, 400), MakeKey(2, 1, 400),
                                  PieceTimeType::Musical, src,
                                  PieceTimeType::Musical, tgt, 100)
                  .reject,
              SamePositionReject::IncompatibleClockMapping); // 44100 vs 48000

    EXPECT_EQ(ResolveSamePosition(MakeKey(1, 1, 400), MakeKey(2, 1, 400),
                                  PieceTimeType::Musical, src,
                                  PieceTimeType::Musical, invalid, 100)
                  .reject,
              SamePositionReject::IncompatibleClockMapping); // invalid target clock
}

// The full compatibility key is what governs reuse: matching only an object id is
// not enough. Every field participates.
TEST(SamePositionResolver, ReuseCompatibilityComparesTheWholeKey) {
    const CompatibilityKey base = MakeKey(1, 1, 400);
    EXPECT_TRUE(AreReuseCompatible(base, base));

    CompatibilityKey sameIdDiffVariant = base;
    sameIdDiffVariant.mediaVariant = base.mediaVariant + 1; // same "object id"
    EXPECT_FALSE(AreReuseCompatible(base, sameIdDiffVariant));

    CompatibilityKey diffEnvelope = base;
    diffEnvelope.envelopeId += 1;
    EXPECT_FALSE(AreReuseCompatible(base, diffEnvelope));

    CompatibilityKey diffLoop = base;
    diffLoop.loopIteration += 1;
    EXPECT_FALSE(AreReuseCompatible(base, diffLoop));
}
