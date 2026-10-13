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

TimeAxisTicks buildTimeAxisTicks(double start, double span, int width,
    double minimumStep, int maximumDecimals, const QFontMetrics& metrics)
{
    TimeAxisTicks result;
    if (span <= 0.0 || !std::isfinite(start) || !std::isfinite(span) || width <= 0)
        return result;
    const int estimatedDecimals = std::clamp(int(std::ceil(-std::log10(span))) + 2, 0, maximumDecimals);
    const int labelWidth = std::max(48,
        metrics.horizontalAdvance(QString::number(start + span, 'f', estimatedDecimals)) + 12);
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
    int width, const QFontMetrics& metrics)
{
    TimeAxisTicks result;
    if (span <= 0.0 || !std::isfinite(span) || width <= 0)
        return result;
    const int estimatedDecimals = std::clamp(int(std::ceil(-std::log10(span / 1e9))) + 2, 0, 9);
    const auto labelWidthAt = [&](int64_t coordinate) {
        QString label = formatTimeNanoseconds(coordinate).section(QLatin1Char('.'), 0, 0);
        if (estimatedDecimals > 0)
            label += QLatin1Char('.') + QString(estimatedDecimals, QLatin1Char('0'));
        return metrics.horizontalAdvance(label) + 12;
    };
    const int labelWidth = std::max({48, labelWidthAt(start.rounded()), labelWidthAt(start.shifted(span).rounded())});
    const int64_t majorStep = static_cast<int64_t>(std::clamp(
        tickStep(std::max(1.0, span * labelWidth / width)), 1.0, 5e18));
    const int64_t minorStep = majorStep >= 5 ? majorStep / 5 : majorStep;
    result.majorStep = double(majorStep) / 1e9;
    result.minorStep = double(minorStep) / 1e9;
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
            labels.append(formatTimeNanoseconds(value));
            labelIndices.append(result.ticks.size());
        }
        result.ticks.append({double(value) / 1e9, x, major, {}});
        if (value > std::numeric_limits<int64_t>::max() - minorStep)
            break;
        value += minorStep;
    }
    labels = compactAxisLabels(std::move(labels));
    for (int i = 0; i < labels.size(); ++i)
        result.ticks[labelIndices[i]].label = labels[i];
    return result;
}

void drawTimeAxisRuler(QPainter& painter, const ThemeColors& colors,
    const QRect& bounds, const TimeAxisTicks& ticks)
{
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics metrics(painter.font(), painter.device());
    int lastLabelRight = bounds.left();
    for (const auto& tick : ticks.ticks) {
        const int x = bounds.left() + tick.pixel;
        painter.setPen(tick.major ? colors.textSecondary : colors.textMuted);
        painter.drawLine(x, bounds.bottom() + 1 - (tick.major ? 4 : 2), x, bounds.bottom());
        if (tick.label.isEmpty())
            continue;
        const int textWidth = metrics.horizontalAdvance(tick.label);
        const int left = std::clamp(x - textWidth / 2, bounds.left() + 4,
            std::max(bounds.left() + 4, bounds.right() - 4 - textWidth));
        if (left >= lastLabelRight + 4 && left + textWidth <= bounds.right() - 4) {
            painter.setPen(colors.textSecondary);
            painter.drawText(QRect(left, bounds.top(), textWidth, bounds.height() - 5), Qt::AlignCenter, tick.label);
            lastLabelRight = left + textWidth;
        }
    }
    painter.restore();
}

void drawTimeAxisGrid(QPainter& painter, const QColor& majorColor,
    const QRect& bounds, const TimeAxisTicks& ticks, const QColor& minorColor)
{
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const auto& tick : ticks.ticks) {
        if (!tick.major && !minorColor.isValid())
            continue;
        painter.setPen(tick.major ? majorColor : minorColor);
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
    for (int dx : {-2, 1})
        painter.drawLine(bounds.center().x() + dx, bounds.top() + 2,
            bounds.center().x() + dx, bounds.bottom() - 2);
    painter.restore();
}

}
