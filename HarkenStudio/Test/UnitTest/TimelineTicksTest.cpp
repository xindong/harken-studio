#include "Widgets/Common/TimeAxis.h"
#include "Widgets/Timeline/TimelineTicks.h"
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

TEST(TimelineTicks, TimeLabelsPreserveResolutionAcrossZoomLevels)
{
    const QFontMetrics metrics{QFont{}};
    QSet<int> precisions;
    for (const double span : {1e7, 1e8, 1e9, 1e10, 1e11}) {
        for (const int64_t origin : {int64_t(0), int64_t(59990000000LL)}) {
            const auto ticks = VNX::buildNanosecondTimeAxisTicks({origin}, span, 1601,
                metrics, 1, VNX::TimeAxisLabelFormat::MinutesSecondsMilliseconds);
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
    EXPECT_EQ(VNX::formatTimeMinutesSecondsMilliseconds(59950000000LL, 1, 1), QStringLiteral("01:00.0"));
    EXPECT_EQ(VNX::formatTimeMinutesSecondsMilliseconds(-499999LL, 0), QStringLiteral("00:00"));
    EXPECT_EQ(VNX::formatTimeMinutesSecondsMilliseconds(1707317500LL, 0), QStringLiteral("00:01.707"));
    EXPECT_EQ(VNX::formatTimeMinutesSecondsMilliseconds(1500000000LL, 0), QStringLiteral("00:01.5"));
    EXPECT_EQ(VNX::formatTimeMinutesSecondsMilliseconds(0), QStringLiteral("00:00.000"));
}

TEST(TimelineTicks, MusicalRulersKeepDistinctAlignedTicksAtIntegerLimits)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    VNX::ThemeMetrics::Timeline metrics;
    metrics.minLabelSpacing = 64;
    metrics.minTickSpacing = 8;
    for (const auto spec : {VNX::SegmentTimeSpec::musical(),
        VNX::SegmentTimeSpec::musical(500000, 7, 8),
        VNX::SegmentTimeSpec::musical(500000, 3, 32),
        VNX::SegmentTimeSpec::musical(500000, 1, 3840)}) {
        const int64_t unit = VNX::timelineCoordinateUnit(spec);
        for (const int64_t origin : {minimum, maximum - 1000}) {
            const auto ticks = VNX::buildTimelineTicks(spec, origin, 0,
                double(unit) * 1600.0 / 1000.0, 1601, metrics, 0);
            ASSERT_GT(ticks.ticks.size(), 1);
            std::optional<int64_t> previous;
            QSet<QString> labels;
            for (const auto& tick : ticks.ticks) {
                if (previous) EXPECT_GT(tick.coordinate, *previous);
                EXPECT_EQ(tick.coordinate % ticks.minorStep, 0);
                EXPECT_LE(VNX::NanosecondPosition{origin}.distanceTo(tick.coordinate), 1001.0);
                EXPECT_GE(VNX::NanosecondPosition{origin}.distanceTo(tick.coordinate),
                    -double(ticks.minorStep));
                if (tick.labeled) {
                    EXPECT_FALSE(labels.contains(tick.label));
                    labels.insert(tick.label);
                }
                previous = tick.coordinate;
            }
        }
        const auto full = VNX::buildTimelineTicks(spec, minimum, 0,
            double(unit) * 1600.0 / VNX::NanosecondPosition{minimum}.distanceTo(maximum),
            1601, metrics, 0);
        EXPECT_FALSE(full.ticks.isEmpty());
        EXPECT_LE(full.ticks.size(), 1601);
    }
    EXPECT_EQ(VNX::formatTimelineBar(maximum, VNX::SegmentTimeSpec::musical(500000, 1, 3840)),
        QString::number(qulonglong(maximum) + 1));
}

TEST(TimelineTicks, MusicalGridPreservesMeterHierarchyAcrossZoomLevels)
{
    VNX::ThemeMetrics::Timeline metrics;
    metrics.minLabelSpacing = 64;
    metrics.minTickSpacing = 8;
    for (const auto spec : {VNX::SegmentTimeSpec::musical(),
        VNX::SegmentTimeSpec::musical(500000, 3, 4),
        VNX::SegmentTimeSpec::musical(487805, 5, 8),
        VNX::SegmentTimeSpec::musical(487805, 7, 8),
        VNX::SegmentTimeSpec::musical(750000, 9, 16),
        VNX::SegmentTimeSpec::musical(500000, 1, 32),
        VNX::SegmentTimeSpec::musical(500000, 3, 32),
        VNX::SegmentTimeSpec::musical(500000, 7, 32),
        VNX::SegmentTimeSpec::musical(500000, 1, 3840)}) {
        const int64_t beat = VNX::timelineCoordinateUnit(spec);
        const int64_t bar = beat * spec.meterMap.front().numerator;
        for (int exponent = -10; exponent <= 10; ++exponent) {
            SCOPED_TRACE(::testing::Message() << spec.meterMap.front().numerator
                << '/' << spec.meterMap.front().denominator << " zoom exponent " << exponent);
            for (const int64_t origin : {-3 * bar - beat / 2, int64_t(0), 5 * bar + beat / 2}) {
                const auto ticks = VNX::buildTimelineTicks(spec, origin, 0,
                    std::ldexp(1.0, exponent), 1024, metrics, 0);
                ASSERT_FALSE(ticks.ticks.isEmpty());
                ASSERT_GE(ticks.minorStep, VNX::TimelineCoordinates::kPpq / 4);
                int64_t ratio;
                if (ticks.minorStep <= beat) {
                    EXPECT_EQ(beat % ticks.minorStep, 0);
                    ratio = beat / ticks.minorStep;
                } else {
                    EXPECT_EQ(ticks.minorStep % bar, 0);
                    ratio = ticks.minorStep / bar;
                }
                EXPECT_GT(ratio, 0);
                EXPECT_EQ(ratio & (ratio - 1), 0);
                std::optional<int64_t> previous;
                for (const auto& tick : ticks.ticks) {
                    if (previous) EXPECT_EQ(tick.coordinate - *previous, ticks.minorStep);
                    EXPECT_EQ(tick.beat, tick.coordinate % beat == 0);
                    EXPECT_EQ(tick.major, tick.coordinate % bar == 0);
                    EXPECT_FALSE(tick.major && !tick.beat);
                    if (tick.labeled) EXPECT_EQ(tick.coordinate % ticks.majorStep, 0);
                    previous = tick.coordinate;
                }
            }
        }
    }
}
