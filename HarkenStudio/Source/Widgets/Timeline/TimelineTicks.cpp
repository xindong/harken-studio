#include "TimelineTicks.h"

#include "Core/AuthoringPolicy.h"
#include "Core/NumericFormat.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/Timeline/TimelineSemantics.h"
#include "Widgets/Common/TimeAxis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace VNX {
namespace {

int64_t saturatingMul(int64_t left, int64_t right)
{
    if (left == 0 || right == 0)
        return 0;
    if (left > 0 && right > 0
        && left > std::numeric_limits<int64_t>::max() / right)
        return std::numeric_limits<int64_t>::max();
    if (left < 0 && right < 0
        && left < std::numeric_limits<int64_t>::max() / right)
        return std::numeric_limits<int64_t>::max();
    if (left > 0 && right < 0
        && right < std::numeric_limits<int64_t>::min() / left)
        return std::numeric_limits<int64_t>::min();
    if (left < 0 && right > 0
        && left < std::numeric_limits<int64_t>::min() / right)
        return std::numeric_limits<int64_t>::min();
    return left * right;
}

int64_t floorDiv(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    const int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
        --quotient;
    return quotient;
}

int64_t coordinateAt(double pixel, int64_t origin, double zoom, int64_t unit, int pixelOrigin)
{
    return NanosecondPosition{origin}.shifted((pixel - pixelOrigin) * double(unit) / zoom).rounded();
}

TimelineTickSet buildAbsoluteTicks(int64_t origin,
                                   int viewOffset,
                                   double zoom,
                                   int width,
                                   int64_t unit,
                                   int64_t snapGrid,
                                   const ThemeMetrics::Timeline& metrics,
                                   int pixelOrigin)
{
    static constexpr double kSteps[] = {
        0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60,
        120, 300, 600, 900, 1800, 3600
    };
    const double pxPerSecond = zoom
        * static_cast<double>(TimelineCoordinates::kNanosecondsPerSecond)
        / static_cast<double>(unit);
    const double snapSeconds = static_cast<double>(snapGrid)
        / static_cast<double>(unit);
    double majorSeconds = kSteps[std::size(kSteps) - 1];
    const double requiredSeconds = metrics.minLabelSpacing / pxPerSecond;
    if (requiredSeconds > majorSeconds) {
        const double magnitude = std::pow(10.0, std::floor(std::log10(requiredSeconds)));
        for (double multiple : {1.0, 2.0, 5.0, 10.0}) {
            if (multiple * magnitude >= requiredSeconds) {
                majorSeconds = multiple * magnitude;
                break;
            }
        }
    }
    for (double step : kSteps) {
        if (step + 1.0e-12 < snapSeconds)
            continue;
        if (step * pxPerSecond >= metrics.minLabelSpacing) {
            majorSeconds = step;
            break;
        }
    }
    const int64_t majorStep = static_cast<int64_t>(std::clamp(
        static_cast<long double>(majorSeconds) * unit, 1.0L,
        static_cast<long double>(std::numeric_limits<int64_t>::max())));
    int64_t minorStep = majorStep;
    for (double step : kSteps) {
        const int64_t candidate = std::max<int64_t>(
            1, static_cast<int64_t>(std::llround(step * static_cast<double>(unit))));
        if (candidate >= majorStep)
            break;
        if (majorStep % candidate != 0)
            continue;
        const double candidatePx = static_cast<double>(candidate)
            * zoom
            / static_cast<double>(unit);
        if (candidatePx >= metrics.minTickSpacing) {
            minorStep = candidate;
            break;
        }
    }
    if (minorStep >= majorStep)
        minorStep = majorStep;

    TimelineTickSet set;
    QVector<double> labelValues;
    QVector<int> labelIndices;
    set.majorStep = majorStep;
    set.minorStep = minorStep;
    const int64_t visibleStart = coordinateAt(viewOffset, origin, zoom, unit, pixelOrigin);
    const int64_t visibleEnd = coordinateAt(double(viewOffset) + width, origin, zoom, unit, pixelOrigin);
    const int64_t first = floorDiv(visibleStart, minorStep) * minorStep;
    for (int64_t coordinate = first; coordinate <= visibleEnd;
         coordinate = saturatingMul(floorDiv(coordinate, minorStep) + 1, minorStep)) {
        if (coordinate < first)
            break;
        TimelineTick tick;
        tick.coordinate = coordinate;
        tick.major = coordinate % majorStep == 0;
        tick.labeled = tick.major;
        if (tick.labeled) {
            const double seconds = static_cast<double>(coordinate)
                / static_cast<double>(unit);
            labelValues.append(seconds);
            labelIndices.append(set.ticks.size());
        }
        set.ticks.append(tick);
        if (coordinate == std::numeric_limits<int64_t>::max())
            break;
    }
    const QStringList labels = formatAxisValues(labelValues, 2);
    for (int i = 0; i < labels.size(); ++i)
        set.ticks[labelIndices[i]].label = labels[i] + QStringLiteral(" s");
    return set;
}

TimelineTickSet buildMusicalTicks(const SegmentTimeSpec& spec,
                                  int64_t origin, int viewOffset, double zoom,
                                  int width, int64_t unit,
                                  const ThemeMetrics::Timeline& metrics, int pixelOrigin)
{
    constexpr int64_t minimumStep = TimelineCoordinates::kPpq / 4;
    const int64_t bar = saturatingMul(unit, spec.meterMap.front().numerator);
    const double pixelsPerTick = zoom / double(unit);
    int64_t labelStep = bar;
    while ((labelStep < minimumStep || double(labelStep) * pixelsPerTick < metrics.minLabelSpacing)
        && labelStep <= std::numeric_limits<int64_t>::max() / 2)
        labelStep *= 2;
    int64_t minorStep = bar;
    if (unit >= minimumStep && double(unit) * pixelsPerTick >= metrics.minTickSpacing) {
        minorStep = unit;
        while (minorStep / 2 >= minimumStep && minorStep % 2 == 0
            && double(minorStep / 2) * pixelsPerTick >= metrics.minTickSpacing)
            minorStep /= 2;
    } else {
        while ((minorStep < minimumStep || double(minorStep) * pixelsPerTick < metrics.minTickSpacing)
            && minorStep <= std::numeric_limits<int64_t>::max() / 2)
            minorStep *= 2;
    }
    TimelineTickSet set;
    set.majorStep = labelStep;
    set.minorStep = minorStep;
    const int64_t visibleStart = coordinateAt(viewOffset, origin, zoom, unit, pixelOrigin);
    const int64_t visibleEnd = coordinateAt(double(viewOffset) + width, origin, zoom, unit, pixelOrigin);
    int64_t coordinate = visibleStart - visibleStart % minorStep;
    if (coordinate > visibleStart && coordinate >= std::numeric_limits<int64_t>::min() + minorStep)
        coordinate -= minorStep;
    for (; coordinate <= visibleEnd;) {
        TimelineTick tick;
        tick.coordinate = coordinate;
        tick.major = coordinate % bar == 0;
        tick.beat = coordinate % unit == 0;
        tick.labeled = coordinate % labelStep == 0;
        if (tick.labeled)
            tick.label = formatTimelineBar(coordinate, spec);
        set.ticks.append(tick);
        if (coordinate > std::numeric_limits<int64_t>::max() - minorStep) break;
        coordinate += minorStep;
    }
    return set;
}


}

int64_t timelineCoordinateUnit(const SegmentTimeSpec& spec)
{
    return TimelineSemantics::coordinateUnitFor(spec);
}

int64_t timelineSnapGrid(const SegmentTimeSpec& spec)
{
    return TimelineSemantics::snapGridFor(spec);
}

QString formatTimelineBar(int64_t coordinate, const SegmentTimeSpec& spec)
{
    const int64_t bar = saturatingMul(timelineCoordinateUnit(spec), spec.meterMap.front().numerator);
    const int64_t index = floorDiv(coordinate, bar);
    return index >= 0 ? QString::number(qulonglong(index) + 1) : QString::number(index + 1);
}

TimelineTickSet buildTimelineTicks(const SegmentTimeSpec& spec,
                                   int64_t viewOrigin,
                                   int viewOffset,
                                   double pixelsPerUnit,
                                   int width,
                                   const ThemeMetrics::Timeline& metrics,
                                   int pixelOrigin)
{
    if (!AuthoringPolicy::validateSegmentTime(spec))
        return {};
    const int64_t unit = std::max<int64_t>(1, timelineCoordinateUnit(spec));
    const int64_t snapGrid = std::max<int64_t>(1, timelineSnapGrid(spec));
    if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0)
        return {};
    const int span = std::max(1, width);
    if (spec.domain == TimelineDomain::Musical)
        return buildMusicalTicks(spec, viewOrigin, viewOffset, pixelsPerUnit, span,
            unit, metrics, pixelOrigin);
    return buildAbsoluteTicks(viewOrigin, viewOffset, pixelsPerUnit, span,
                              unit, snapGrid, metrics, pixelOrigin);
}

}
