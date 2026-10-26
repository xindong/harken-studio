#include "Widgets/Common/TimeAxis.h"

#include <gtest/gtest.h>

#include <QFont>
#include <QFontMetrics>
#include <QSet>

#include <cmath>
#include <limits>

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
    EXPECT_EQ(VNX::NanosecondPosition{minimum}.shifted(std::ldexp(1.0, 63)).rounded(), 0);
    EXPECT_EQ(VNX::NanosecondPosition{maximum}.shifted(-std::ldexp(1.0, 63)).rounded(), -1);
}

TEST(TimelineTicks, NanosecondRulerKeepsDistinctAlignedTicksAtIntegerLimits)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    const QFontMetrics metrics{QFont{}};
    for (const int64_t origin : {minimum, maximum - 1000}) {
        const auto ticks = VNX::buildNanosecondTimeAxisTicks({origin}, 1000, 1601, metrics);
        ASSERT_GT(ticks.ticks.size(), 1);
        int previousPixel = -1;
        QSet<QString> labels;
        for (const auto& tick : ticks.ticks) {
            EXPECT_GT(tick.pixel, previousPixel);
            EXPECT_GE(tick.pixel, 0);
            EXPECT_LE(tick.pixel, 1600);
            if (tick.major) {
                EXPECT_FALSE(labels.contains(tick.label));
                labels.insert(tick.label);
            }
            previousPixel = tick.pixel;
        }
        EXPECT_GT(labels.size(), 1);
    }
    const auto full = VNX::buildNanosecondTimeAxisTicks({minimum},
        VNX::NanosecondPosition{minimum}.distanceTo(maximum), 1601, metrics);
    EXPECT_FALSE(full.ticks.isEmpty());
    EXPECT_LE(full.ticks.size(), 1601);
}
