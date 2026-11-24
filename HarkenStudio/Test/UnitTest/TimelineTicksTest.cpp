#include "Widgets/Common/TimeAxis.h"
#include "Core/Timeline/TimelineCoordinates.h"

#include <gtest/gtest.h>

#include <QFont>
#include <QFontMetrics>
#include <QSet>

#include <cmath>
#include <limits>
#include <optional>

TEST(TimelineTicks, NanosecondPositionsPreserveSmallOffsetsAtIntegerLimits)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    const harken::studio::NanosecondPosition positive{maximum - 1000};
    const harken::studio::NanosecondPosition negative{minimum + 1000};
    for (const auto origin : {positive, negative}) {
        const auto moved = origin.shifted(0.25).shifted(16.25);
        EXPECT_EQ(moved.nanoseconds, origin.nanoseconds + 16);
        EXPECT_DOUBLE_EQ(moved.fraction, 0.5);
        EXPECT_DOUBLE_EQ(moved.distanceTo(origin.nanoseconds + 500), 483.5);
        const auto restored = moved.shifted(-16.5);
        EXPECT_EQ(restored.nanoseconds, origin.nanoseconds);
        EXPECT_DOUBLE_EQ(restored.fraction, 0.0);
    }
    EXPECT_EQ(harken::studio::NanosecondPosition{minimum}.shifted(std::ldexp(1.0, 63)).rounded(), 0);
    EXPECT_EQ(harken::studio::NanosecondPosition{maximum}.shifted(-std::ldexp(1.0, 63)).rounded(), -1);
}

TEST(TimelineTicks, NanosecondRulerKeepsDistinctAlignedTicksAtIntegerLimits)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    const QFontMetrics metrics{QFont{}};
    for (const int64_t origin : {minimum, maximum - 1000}) {
        const auto ticks = harken::studio::buildNanosecondTimeAxisTicks({origin}, 1000, 1601, metrics);
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
    const auto full = harken::studio::buildNanosecondTimeAxisTicks({minimum},
        harken::studio::NanosecondPosition{minimum}.distanceTo(maximum), 1601, metrics);
    EXPECT_FALSE(full.ticks.isEmpty());
    EXPECT_LE(full.ticks.size(), 1601);
}

TEST(TimelineTicks, TimeLabelsPreserveResolutionAcrossZoomLevels)
{
    const QFontMetrics metrics{QFont{}};
    QSet<int> precisions;
    for (const double span : {1e7, 1e8, 1e9, 1e10, 1e11}) {
        for (const int64_t origin : {int64_t(0), int64_t(59990000000LL)}) {
            const auto ticks = harken::studio::buildNanosecondTimeAxisTicks({origin}, span, 1601,
                metrics, 1, harken::studio::TimeAxisLabelFormat::MinutesSecondsMilliseconds);
            QSet<QString> labels;
            std::optional<int> precision;
            for (const auto& tick : ticks.ticks) {
                if (tick.label.isEmpty()) continue;
                EXPECT_FALSE(labels.contains(tick.label));
                labels.insert(tick.label);
                const auto parts = tick.label.split(QLatin1Char(':'));
                ASSERT_EQ(parts.size(), 2);
                const auto seconds = parts[1].split(QLatin1Char('.'));
                ASSERT_LE(seconds.size(), 2);
                const QString fraction = seconds.size() == 2 ? seconds[1] : QString();
                ASSERT_LE(fraction.size(), 3);
                if (precision) EXPECT_EQ(fraction.size(), *precision);
                else precision = fraction.size();
                EXPECT_GE(seconds[0].toInt(), 0);
                EXPECT_LT(seconds[0].toInt(), 60);
                const int64_t displayed = (parts[0].toLongLong() * 60 + seconds[0].toInt()) * 1000000000LL
                    + fraction.leftJustified(3, QLatin1Char('0')).toInt() * 1000000LL;
                EXPECT_LE(std::abs(displayed - tick.coordinate), 500000);
            }
            EXPECT_GT(labels.size(), 1);
            ASSERT_TRUE(precision.has_value());
            precisions.insert(*precision);
        }
    }
    EXPECT_EQ(precisions, (QSet<int>{0, 1, 2, 3}));
    EXPECT_EQ(harken::studio::formatTimeMinutesSecondsMilliseconds(59950000000LL, 1, 1), QStringLiteral("01:00.0"));
    EXPECT_EQ(harken::studio::formatTimeMinutesSecondsMilliseconds(-499999LL, 0), QStringLiteral("00:00"));
    EXPECT_EQ(harken::studio::formatTimeMinutesSecondsMilliseconds(1707317500LL, 0), QStringLiteral("00:01.707"));
    EXPECT_EQ(harken::studio::formatTimeMinutesSecondsMilliseconds(1500000000LL, 0), QStringLiteral("00:01.5"));
    EXPECT_EQ(harken::studio::formatTimeMinutesSecondsMilliseconds(0), QStringLiteral("00:00.000"));
}
