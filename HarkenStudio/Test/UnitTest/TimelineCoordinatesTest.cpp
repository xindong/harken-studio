#include "Core/Timeline/TimelineCoordinates.h"

#include <gtest/gtest.h>

#include <limits>

using namespace VNX;

TEST(TimelineCoordinates, TimeSpecValidationKeepsIntegerTruthSources)
{
    EXPECT_EQ(TimelineCoordinates::kPpq, 960);
    EXPECT_TRUE(TimelineCoordinates::validate(PieceTimeSpec{}));
    EXPECT_TRUE(TimelineCoordinates::validate(PieceTimeSpec::musical()));

    PieceTimeSpec missingTempo = PieceTimeSpec::musical();
    missingTempo.tempoMap.clear();
    EXPECT_FALSE(TimelineCoordinates::validate(missingTempo));

    PieceTimeSpec shiftedMeter = PieceTimeSpec::musical();
    shiftedMeter.meterMap.front().tick = 1;
    EXPECT_FALSE(TimelineCoordinates::validate(shiftedMeter));

    PieceTimeSpec zeroTempo = PieceTimeSpec::musical(0);
    EXPECT_FALSE(TimelineCoordinates::validate(zeroTempo));

    PieceTimeSpec invalidMeter = PieceTimeSpec::musical(500000, 4, 0);
    EXPECT_FALSE(TimelineCoordinates::validate(invalidMeter));
    EXPECT_FALSE(TimelineCoordinates::validate(
        PieceTimeSpec::musical(500000, 4, 7)));
    EXPECT_TRUE(TimelineCoordinates::validate(
        PieceTimeSpec::musical(10000000000000LL)));
}

TEST(TimelineCoordinates, MusicalAndAbsoluteConversionsSupportNegativeCoordinates)
{
    const PieceTimeSpec musical = PieceTimeSpec::musical(500000, 4, 4);
    EXPECT_EQ(TimelineCoordinates::ticksToNanoseconds(960, musical).value, 500000000);
    EXPECT_EQ(TimelineCoordinates::ticksToNanoseconds(-960, musical).value, -500000000);
    EXPECT_EQ(TimelineCoordinates::nanosecondsToTicks(500000000, musical).value, 960);
    EXPECT_EQ(TimelineCoordinates::nanosecondsToTicks(-500000000, musical).value, -960);

    const CoordinateResult absolute = TimelineCoordinates::toNanoseconds(-123, {});
    ASSERT_TRUE(absolute);
    EXPECT_EQ(absolute.value, -123);
    EXPECT_DOUBLE_EQ(*TimelineCoordinates::toSeconds(1500000000, {}), 1.5);

    const PieceTimeSpec slow = PieceTimeSpec::musical(10000000000000LL);
    EXPECT_EQ(TimelineCoordinates::ticksToNanoseconds(2, slow).value,
              20833333333333LL);
}

TEST(TimelineCoordinates, HalfValuesRoundAwayFromZero)
{
    const PieceTimeSpec musical = PieceTimeSpec::musical(500004, 4, 4);
    EXPECT_EQ(TimelineCoordinates::ticksToNanoseconds(1, musical).value, 520838);
    EXPECT_EQ(TimelineCoordinates::ticksToNanoseconds(-1, musical).value, -520838);
    EXPECT_EQ(TimelineCoordinates::fromSeconds(0.0000000005, {}).value, 1);
    EXPECT_EQ(TimelineCoordinates::fromSeconds(-0.0000000005, {}).value, -1);
    EXPECT_EQ(TimelineCoordinates::beatsToTicks(0.5 / 960.0).value, 1);
    EXPECT_EQ(TimelineCoordinates::beatsToTicks(-0.5 / 960.0).value, -1);
    EXPECT_DOUBLE_EQ(*TimelineCoordinates::ticksToBeats(1440), 1.5);
}

TEST(TimelineCoordinates, DefaultSnappingUsesSecondsAndBeats)
{
    EXPECT_EQ(TimelineCoordinates::snap(1499999999, TimelineDomain::Absolute).value,
              1000000000);
    EXPECT_EQ(TimelineCoordinates::snap(1500000000, TimelineDomain::Absolute).value,
              2000000000);
    EXPECT_EQ(TimelineCoordinates::snap(-1500000000, TimelineDomain::Absolute).value,
              -2000000000);
    EXPECT_EQ(TimelineCoordinates::snap(480, TimelineDomain::Musical).value, 960);
    EXPECT_EQ(TimelineCoordinates::snap(-480, TimelineDomain::Musical).value, -960);
    const PieceTimeSpec sixEight = PieceTimeSpec::musical(500000, 6, 8);
    EXPECT_EQ(TimelineCoordinates::snap(240, sixEight).value, 480);
    EXPECT_FALSE(TimelineCoordinates::snap(1, static_cast<TimelineDomain>(99)));
}

TEST(TimelineCoordinates, ConversionRejectsOverflowWithoutWrapping)
{
    const PieceTimeSpec musical = PieceTimeSpec::musical();
    const CoordinateResult high = TimelineCoordinates::ticksToNanoseconds(
        std::numeric_limits<int64_t>::max(), musical);
    const CoordinateResult low = TimelineCoordinates::ticksToNanoseconds(
        std::numeric_limits<int64_t>::min(), musical);
    EXPECT_FALSE(high);
    EXPECT_FALSE(low);
    EXPECT_EQ(high.validation.error, TimelineError::CoordinateOverflow);
    EXPECT_EQ(low.validation.error, TimelineError::CoordinateOverflow);

    const CoordinateResult seconds = TimelineCoordinates::fromSeconds(
        std::numeric_limits<double>::infinity(), {});
    EXPECT_FALSE(seconds);
    EXPECT_EQ(seconds.validation.error, TimelineError::CoordinateOverflow);
}

TEST(TimelineCoordinates, DomainConversionPreservesWallClockAtOneRoundingBoundary)
{
    const PieceTimeSpec absolute;
    const PieceTimeSpec musical = PieceTimeSpec::musical(500000, 4, 4);
    const CoordinateResult ticks = TimelineCoordinates::convert(
        750000000, absolute, musical);
    ASSERT_TRUE(ticks);
    EXPECT_EQ(ticks.value, 1440);
    const CoordinateResult nanoseconds = TimelineCoordinates::convert(
        ticks.value, musical, absolute);
    ASSERT_TRUE(nanoseconds);
    EXPECT_EQ(nanoseconds.value, 750000000);

    PieceTimeSpec invalidAbsolute;
    invalidAbsolute.tempoMap = {{0, 500000}};
    EXPECT_FALSE(TimelineCoordinates::convert(
        1, invalidAbsolute, invalidAbsolute));
}
