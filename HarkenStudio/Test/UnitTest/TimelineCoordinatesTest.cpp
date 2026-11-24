#include "Core/Timeline/TimelineCoordinates.h"

#include <gtest/gtest.h>

#include <limits>

using namespace harken::studio;

TEST(TimelineCoordinates, ConversionRejectsOverflowWithoutWrapping)
{
    const CoordinateResult seconds = TimelineCoordinates::fromSeconds(
        std::numeric_limits<double>::infinity());
    EXPECT_FALSE(seconds);
}
