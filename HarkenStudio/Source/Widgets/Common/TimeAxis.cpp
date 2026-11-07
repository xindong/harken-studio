#include "TimeAxis.h"

#include "Core/FontManager.h"
#include "Core/NumericFormat.h"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX {
namespace {

double tickStep(double minimum)
{
    const double magnitude = std::pow(10.0, std::floor(std::log10(std::max(1e-12, minimum))));
    for (double multiple : {1.0, 2.0, 5.0, 10.0})
        if (multiple * magnitude >= minimum)
            return multiple * magnitude;
    return magnitude * 10.0;
}

}

NanosecondPosition NanosecondPosition::shifted(double offset) const
{
    const double delta = fraction + offset;
    const double whole = std::floor(delta);
    const bool negative = whole < 0.0;
    const int64_t limit = negative ? std::numeric_limits<int64_t>::min()
                                   : std::numeric_limits<int64_t>::max();
    const uint64_t available = negative ? uint64_t(nanoseconds) - uint64_t(limit)
                                        : uint64_t(limit) - uint64_t(nanoseconds);
    if (std::abs(whole) >= double(std::numeric_limits<uint64_t>::max()))
        return {limit, 0.0};
    const uint64_t amount = static_cast<uint64_t>(std::abs(whole));
    if (amount > available)
        return {limit, 0.0};
    const int64_t value = static_cast<int64_t>(negative ? uint64_t(nanoseconds) - amount
                                                       : uint64_t(nanoseconds) + amount);
    return {value, value == std::numeric_limits<int64_t>::max() ? 0.0 : delta - whole};
}

double NanosecondPosition::distanceTo(int64_t coordinate) const
{
    const double distance = coordinate >= nanoseconds
        ? double(uint64_t(coordinate) - uint64_t(nanoseconds))
        : -double(uint64_t(nanoseconds) - uint64_t(coordinate));
    return distance - fraction;
}

int64_t NanosecondPosition::rounded() const
{
    return nanoseconds + (fraction > 0.5 || (fraction == 0.5 && nanoseconds >= 0) ? 1 : 0);
}

QString formatTimeNanoseconds(int64_t nanoseconds, int minimumDecimals, int maximumDecimals)
{
    maximumDecimals = std::clamp(maximumDecimals, 0, 9);
    minimumDecimals = std::clamp(minimumDecimals, 0, maximumDecimals);
    uint64_t quantum = 1;
    for (int i = maximumDecimals; i < 9; ++i)
        quantum *= 10;
    uint64_t magnitude = nanoseconds < 0 ? uint64_t(0) - uint64_t(nanoseconds)
                                        : uint64_t(nanoseconds);
    magnitude = (magnitude + quantum / 2) / quantum * quantum;
    QString fraction = QString::number(magnitude % 1000000000)
        .rightJustified(9, QLatin1Char('0')).left(maximumDecimals);
    while (fraction.size() > minimumDecimals && fraction.endsWith(QLatin1Char('0')))
        fraction.chop(1);
    return (nanoseconds < 0 && magnitude > 0 ? QStringLiteral("-") : QString())
        + QString::number(magnitude / 1000000000)
        + (fraction.isEmpty() ? QString() : QLatin1Char('.') + fraction);
}

QString formatTimeMinutesSecondsMilliseconds(int64_t nanoseconds,
    int minimumDecimals, int maximumDecimals)
{
    maximumDecimals = std::clamp(maximumDecimals, 0, 3);
    minimumDecimals = std::clamp(minimumDecimals, 0, maximumDecimals);
    uint64_t quantum = 1000000;
    for (int i = maximumDecimals; i < 3; ++i)
        quantum *= 10;
    const uint64_t magnitude = nanoseconds < 0 ? uint64_t(0) - uint64_t(nanoseconds)
                                              : uint64_t(nanoseconds);
    const uint64_t milliseconds = (magnitude + quantum / 2) / quantum * (quantum / 1000000);
    QString fraction = QString::number(milliseconds % 1000)
        .rightJustified(3, QLatin1Char('0')).left(maximumDecimals);
    while (fraction.size() > minimumDecimals && fraction.endsWith(QLatin1Char('0')))
        fraction.chop(1);
    return (nanoseconds < 0 && milliseconds > 0 ? QStringLiteral("-") : QString())
        + QString::number(milliseconds / 60000).rightJustified(2, QLatin1Char('0'))
        + QLatin1Char(':') + QString::number(milliseconds / 1000 % 60).rightJustified(2, QLatin1Char('0'))
        + (fraction.isEmpty() ? QString() : QLatin1Char('.') + fraction);
}

TimeAxisTicks buildTimeAxisTicks(double start, double span, int width,
    double minimumStep, int maximumDecimals, const QFontMetrics& metrics)
{
    TimeAxisTicks result;
    if (span <= 0.0 || !std::isfinite(start) || !std::isfinite(span) || width <= 0)
        return result;
    const int estimatedDecimals = std::clamp(int(std::ceil(-std::log10(span))) + 2, 0, maximumDecimals);
    const int labelWidth = std::max(metrics.horizontalAdvance(QStringLiteral("000000")),
        metrics.horizontalAdvance(QString::number(start + span, 'f', estimatedDecimals)) + metrics.horizontalAdvance(QStringLiteral("  ")));
    result.majorStep = tickStep(std::max(minimumStep, span * labelWidth / width));
    result.minorStep = result.majorStep / 5.0 >= minimumStep ? result.majorStep / 5.0 : result.majorStep;
    const int decimals = axisDecimalsForStep(result.majorStep, maximumDecimals);
    const double first = std::ceil(start / result.minorStep) * result.minorStep;
    QVector<double> values;
    QVector<int> labelIndices;
    for (int i = 0; i < 512; ++i) {
        const double value = first + i * result.minorStep;
        if (value > start + span + result.minorStep * 0.001)
            break;
        const int x = int(std::lround((value - start) / span * (width - 1)));
        const bool major = std::abs(value / result.majorStep - std::round(value / result.majorStep)) < 0.001;
        if (major) {
            values.append(value);
            labelIndices.append(result.ticks.size());
        }
        result.ticks.append({value, x, major, {}});
    }
    const QStringList labels = formatAxisValues(values, decimals);
    for (int i = 0; i < labels.size(); ++i)
        result.ticks[labelIndices[i]].label = labels[i];
    return result;
}

TimeAxisTicks buildNanosecondTimeAxisTicks(NanosecondPosition start, double span,
    int width, const QFontMetrics& metrics, int64_t minimumStep, TimeAxisLabelFormat format)
{
    TimeAxisTicks result;
    if (span <= 0.0 || !std::isfinite(span) || width <= 0)
        return result;
    const int estimatedDecimals = std::clamp(int(std::ceil(-std::log10(span / 1e9))) + 2, 0, 9);
    const auto labelWidthAt = [&](int64_t coordinate, int decimals) {
        if (format == TimeAxisLabelFormat::MinutesSecondsMilliseconds)
            return metrics.horizontalAdvance(
                formatTimeMinutesSecondsMilliseconds(coordinate, decimals, decimals)) + metrics.horizontalAdvance(QStringLiteral("  "));
        QString label = formatTimeNanoseconds(coordinate).section(QLatin1Char('.'), 0, 0);
        if (estimatedDecimals > 0)
            label += QLatin1Char('.') + QString(estimatedDecimals, QLatin1Char('0'));
        return metrics.horizontalAdvance(label) + metrics.horizontalAdvance(QStringLiteral("  "));
    };
    const auto stepForDecimals = [&](int decimals) {
        const int labelWidth = std::max({metrics.horizontalAdvance(QStringLiteral("000000")), labelWidthAt(start.rounded(), decimals),
            labelWidthAt(start.shifted(span).rounded(), decimals)});
        return static_cast<int64_t>(std::clamp(
            tickStep(std::max(double(std::max<int64_t>(1, minimumStep)), span * labelWidth / width)), 1.0, 5e18));
    };
    int64_t majorStep = stepForDecimals(0);
    if (format == TimeAxisLabelFormat::MinutesSecondsMilliseconds) {
        for (;;) {
            const int64_t nextStep = stepForDecimals(axisDecimalsForStep(double(majorStep) / 1e9, 3));
            if (nextStep <= majorStep)
                break;
            majorStep = nextStep;
        }
    }
    const int64_t minorStep = majorStep >= 5 && majorStep / 5 >= minimumStep ? majorStep / 5 : majorStep;
    result.majorStep = double(majorStep) / 1e9;
    result.minorStep = double(minorStep) / 1e9;
    const int decimals = axisDecimalsForStep(result.majorStep, 3);
    const int64_t remainder = start.nanoseconds % minorStep;
    const int64_t offset = remainder < 0 ? -remainder
        : remainder > 0 ? minorStep - remainder : start.fraction > 0.0 ? minorStep : 0;
    if (start.nanoseconds > std::numeric_limits<int64_t>::max() - offset)
        return result;
    int64_t value = start.nanoseconds + offset;
    QStringList labels;
    QVector<int> labelIndices;
    for (int i = 0; i < 512; ++i) {
        const double distance = start.distanceTo(value);
        if (distance > span)
            break;
        const int x = int(std::lround(distance / span * (width - 1)));
        const bool major = value % majorStep == 0;
        if (major) {
            const QString label = format == TimeAxisLabelFormat::MinutesSecondsMilliseconds
                ? formatTimeMinutesSecondsMilliseconds(value, decimals, decimals) : formatTimeNanoseconds(value);
            if (format == TimeAxisLabelFormat::Seconds || labels.isEmpty() || label != labels.back()) {
                labels.append(label);
                labelIndices.append(result.ticks.size());
            }
        }
        result.ticks.append({double(value) / 1e9, x, major, {}, false, {}, value});
        if (value > std::numeric_limits<int64_t>::max() - minorStep)
            break;
        value += minorStep;
    }
    if (format == TimeAxisLabelFormat::Seconds)
        labels = compactAxisLabels(std::move(labels));
    for (int i = 0; i < labels.size(); ++i)
        result.ticks[labelIndices[i]].label = labels[i];
    return result;
}

void drawTimeAxisRuler(QPainter& painter, const ThemeColors& colors,
    const ThemeMetrics::TimeAxis& geometry,
    const QRect& bounds, const TimeAxisTicks& ticks, FontRole role,
    TimeAxisLabelAlignment alignment, bool pairedLabels)
{
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QFont primaryFont = FontManager::instance().getFont(role);
    const QFont secondaryFont = FontManager::instance().getFont(FontRole::Caption);
    const QFontMetrics metrics(primaryFont, painter.device());
    const QFontMetrics secondaryMetrics(secondaryFont, painter.device());
    const int secondaryHeight = pairedLabels ? secondaryMetrics.height() : 0;
    int lastLabelRight = bounds.left();
    for (const auto& tick : ticks.ticks) {
        const int x = bounds.left() + tick.pixel;
        painter.setPen(colors.borderTick);
        const int tickHeight = tick.major ? geometry.majorTickLength : geometry.minorTickLength;
        painter.drawLine(x, bounds.bottom() + 1 - tickHeight, x, bounds.bottom());
        if (tick.label.isEmpty())
            continue;
        const int textWidth = std::max(metrics.horizontalAdvance(tick.label),
            pairedLabels ? secondaryMetrics.horizontalAdvance(tick.secondaryLabel) : 0);
        const int padding = geometry.labelPadding;
        const int left = alignment == TimeAxisLabelAlignment::AfterTick ? x + padding
            : std::clamp(x - textWidth / 2, bounds.left() + padding,
                std::max(bounds.left() + padding, bounds.right() - padding - textWidth));
        if (left >= lastLabelRight + padding && left + textWidth <= bounds.right() - padding) {
            if (pairedLabels && !tick.secondaryLabel.isEmpty()) {
                painter.setFont(secondaryFont);
                painter.setPen(colors.textMuted);
                painter.drawText(QRect(left, bounds.top(), textWidth, secondaryHeight),
                    Qt::AlignLeft | Qt::AlignVCenter, tick.secondaryLabel);
                painter.setPen(colors.borderTick);
                painter.drawLine(left, bounds.top() + secondaryHeight - 1,
                    left + textWidth - 1, bounds.top() + secondaryHeight - 1);
            }
            painter.setFont(primaryFont);
            painter.setPen(colors.textSecondary);
            painter.drawText(QRect(left, bounds.top() + secondaryHeight, textWidth,
                    bounds.height() - secondaryHeight - geometry.majorTickLength),
                Qt::AlignLeft | Qt::AlignVCenter, tick.label);
            lastLabelRight = left + textWidth;
        }
    }
    painter.restore();
}

void drawTimeAxisGrid(QPainter& painter, const QColor& majorColor,
    const QRect& bounds, const TimeAxisTicks& ticks, const QColor& minorColor,
    const QColor& beatColor)
{
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const auto& tick : ticks.ticks) {
        const QColor color = tick.major ? majorColor
            : tick.beat && beatColor.isValid() ? beatColor : minorColor;
        if (!color.isValid())
            continue;
        painter.setPen(color);
        painter.drawLine(bounds.left() + tick.pixel, bounds.top(), bounds.left() + tick.pixel, bounds.bottom() + 1);
    }
    painter.restore();
}

void drawTimeAxisHandle(QPainter& painter, const QColor& color,
    const QColor& grooveColor, const QRect& bounds)
{
    painter.save();
    painter.fillRect(bounds, color);
    painter.setPen(grooveColor);
    const int inset = bounds.height() / 6;
    for (int x : {bounds.left() + bounds.width() / 3, bounds.right() - bounds.width() / 3})
        painter.drawLine(x, bounds.top() + inset, x, bounds.bottom() - inset);
    painter.restore();
}

QRect timeAxisHandleRect(int x, int top, bool start, const ThemeMetrics::TimeAxis& geometry)
{
    return {start ? x : x - geometry.handleWidth + 1, top,
            geometry.handleWidth, geometry.handleBandHeight};
}

}
