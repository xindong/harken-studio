#include "Core/Timeline/TimelineCoordinates.h"

#include <gtest/gtest.h>

#include <limits>

using namespace VNX;

TEST(TimelineCoordinates, ConversionRejectsOverflowWithoutWrapping)
{
    const PieceTimeSpec musical = PieceTimeSpec::musical();
    const CoordinateResult high = TimelineCoordinates::ticksToNanoseconds(
        std::numeric_limits<int64_t>::max(), musical);
    const CoordinateResult low = TimelineCoordinates::ticksToNanoseconds(
        std::numeric_limits<int64_t>::min(), musical);
    EXPECT_FALSE(high);
    EXPECT_FALSE(low);

    const CoordinateResult seconds = TimelineCoordinates::fromSeconds(
        std::numeric_limits<double>::infinity(), {});
    EXPECT_FALSE(seconds);
}
