#include "Widgets/Timeline/TimelineTicks.h"
#include "Widgets/Common/TimeAxis.h"
#include "Core/Timeline/TimelineCoordinates.h"

#include <gtest/gtest.h>

#include <QFont>
#include <QFontMetrics>
#include <QSet>

#include <cmath>
#include <limits>

TEST(TimelineTicks, NanosecondReadoutRoundsToDisplayPrecisionWithoutOverflow)
{
    EXPECT_EQ(VNX::formatTimeNanoseconds(1354351650, 0, 0), QStringLiteral("1"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(1354351650, 0, 1), QStringLiteral("1.4"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(1354351650, 0, 2), QStringLiteral("1.35"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(1999999999, 0, 2), QStringLiteral("2"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(-1500000000, 0, 0), QStringLiteral("-2"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(-1, 0, 6), QStringLiteral("0"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(std::numeric_limits<int64_t>::max(), 0, 0),
        QStringLiteral("9223372037"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(std::numeric_limits<int64_t>::min(), 0, 3),
        QStringLiteral("-9223372036.855"));
}

TEST(TimelineTicks, NanosecondPositionsPreserveSmallOffsetsAtIntegerLimits)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    const VNX::NanosecondPosition positive{maximum - 1000};
    const VNX::NanosecondPosition negative{minimum + 1000};
    for (const auto origin : {positive, negative}) {
        const auto moved = origin.shifted(0.25).shifted(16.25);
        EXPECT_EQ(moved.nanoseconds, origin.nanoseconds + 16);
        EXPECT_DOUBLE_EQ(moved.fraction, 0.5);
        EXPECT_DOUBLE_EQ(moved.distanceTo(origin.nanoseconds + 500), 483.5);
        const auto restored = moved.shifted(-16.5);
        EXPECT_EQ(restored.nanoseconds, origin.nanoseconds);
        EXPECT_DOUBLE_EQ(restored.fraction, 0.0);
    }
    EXPECT_EQ(positive.shifted(1001).rounded(), maximum);
    EXPECT_EQ(negative.shifted(-1001).rounded(), minimum);
    EXPECT_EQ(VNX::NanosecondPosition{minimum}.shifted(std::ldexp(1.0, 63)).rounded(), 0);
    EXPECT_EQ(VNX::NanosecondPosition{maximum}.shifted(-std::ldexp(1.0, 63)).rounded(), -1);
    EXPECT_EQ(VNX::formatTimeNanoseconds(maximum), QStringLiteral("9223372036.854775807"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(minimum), QStringLiteral("-9223372036.854775808"));
    EXPECT_EQ(VNX::formatTimeNanoseconds(-1), QStringLiteral("-0.000000001"));
}

TEST(TimelineTicks, NanosecondRulerKeepsDistinctAlignedTicksAtIntegerLimits)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    const QFontMetrics metrics{QFont{}};
    for (const int64_t origin : {minimum, maximum - 1000}) {
        const auto ticks = VNX::buildNanosecondTimeAxisTicks({origin}, 1000, 1601, metrics);
        ASSERT_GT(ticks.ticks.size(), 1);
        const int64_t minor = std::llround(ticks.minorStep * 1e9);
        const int64_t major = std::llround(ticks.majorStep * 1e9);
        const int64_t remainder = origin % minor;
        int64_t coordinate = origin + (remainder < 0 ? -remainder : minor - remainder);
        int previousPixel = -1;
        QSet<QString> labels;
        bool sawExactSecondFraction = false;
        for (const auto& tick : ticks.ticks) {
            EXPECT_GT(tick.pixel, previousPixel);
            EXPECT_GE(tick.pixel, 0);
            EXPECT_LE(tick.pixel, 1600);
            EXPECT_EQ(tick.pixel, std::lround(double(coordinate - origin) * 1.6));
            EXPECT_EQ(tick.major, coordinate % major == 0);
            if (tick.major) {
                EXPECT_FALSE(labels.contains(tick.label));
                labels.insert(tick.label);
                QString label = tick.label;
                while (label.contains(QLatin1Char('.')) && label.endsWith(QLatin1Char('0')))
                    label.chop(1);
                EXPECT_EQ(label, VNX::formatTimeNanoseconds(coordinate));
                sawExactSecondFraction = sawExactSecondFraction
                    || label == (origin < 0 ? QStringLiteral("-9223372036.854775")
                                             : QStringLiteral("9223372036.854775"));
            } else {
                EXPECT_TRUE(tick.label.isEmpty());
            }
            previousPixel = tick.pixel;
            if (coordinate > maximum - minor)
                break;
            coordinate += minor;
        }
        EXPECT_GT(labels.size(), 1);
        EXPECT_TRUE(sawExactSecondFraction);
    }
    const auto full = VNX::buildNanosecondTimeAxisTicks({minimum},
        VNX::NanosecondPosition{minimum}.distanceTo(maximum), 1601, metrics);
    EXPECT_FALSE(full.ticks.isEmpty());
    EXPECT_LE(full.ticks.size(), 1601);
}

TEST(TimelineTicks, NanosecondRulerSpacingDoesNotDependOnPanFraction)
{
    const QFontMetrics metrics{QFont{}};
    const auto origin = VNX::buildNanosecondTimeAxisTicks({0}, 5e9, 320, metrics);
    const auto panned = VNX::buildNanosecondTimeAxisTicks({3123456789, 0.25}, 5e9, 320, metrics);
    EXPECT_DOUBLE_EQ(panned.majorStep, origin.majorStep);
    EXPECT_DOUBLE_EQ(panned.minorStep, origin.minorStep);
}

TEST(TimelineTicks, AbsoluteHighZoomKeepsSubsecondMinorTicks)
{
    VNX::ThemeMetrics::Timeline metrics;
    metrics.minLabelSpacing = 48;
    metrics.minTickSpacing = 8;
    const VNX::TimelineTickSet ticks = VNX::buildTimelineTicks(
        VNX::PieceTimeSpec{}, 0, 0, 800, 800, metrics);
    EXPECT_LT(ticks.minorStep, ticks.majorStep);
    EXPECT_GE(ticks.majorStep, VNX::TimelineCoordinates::kNanosecondsPerSecond);
    bool sawLabeledSecond = false;
    for (const VNX::TimelineTick& tick : ticks.ticks) {
        if (tick.labeled && tick.coordinate == 0)
            sawLabeledSecond = true;
        if (tick.labeled)
            EXPECT_FALSE(tick.label.contains(QLatin1Char('.')));
    }
    EXPECT_TRUE(sawLabeledSecond);
}

TEST(TimelineTicks, LegacyMusicalCoordinatesNeverBecomeAuthoringTicks)
{
    VNX::ThemeMetrics::Timeline metrics;
    for (int zoom : {4, 100, 800}) {
        const VNX::TimelineTickSet ticks = VNX::buildTimelineTicks(
            VNX::PieceTimeSpec::musical(500000, 6, 8), -480, 0, zoom, 800, metrics);
        EXPECT_TRUE(ticks.ticks.isEmpty());
    }
}

TEST(TimelineTicks, LowAndFractionalZoomKeepTickDensityBounded)
{
    VNX::ThemeMetrics::Timeline metrics;
    metrics.minLabelSpacing = 48;
    metrics.minTickSpacing = 8;
    for (double scale : {4.0, 0.02, 0.00001}) {
        const auto ticks = VNX::buildTimelineTicks({}, 0, 0, scale, 640, metrics, 0);
        ASSERT_FALSE(ticks.ticks.isEmpty());
        EXPECT_EQ(ticks.ticks.front().coordinate, 0);
        EXPECT_LE(ticks.ticks.size(), 640 / metrics.minTickSpacing + 2);
        EXPECT_GE(ticks.majorStep / 1000000000.0 * scale, metrics.minLabelSpacing);
    }
}
