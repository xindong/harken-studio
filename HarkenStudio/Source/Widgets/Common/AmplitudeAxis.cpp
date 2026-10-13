#include "AmplitudeAxis.h"

#include <QFontMetrics>

#include <algorithm>
#include <cmath>
#include <utility>

namespace VNX {

double AmplitudeScale::dbToPosition(double db) const
{
    if (maximumDb <= minimumDb)
        return 0.0;
    db = std::clamp(db, minimumDb, maximumDb);
    if (exponent == 0.0)
        return (db - minimumDb) / (maximumDb - minimumDb);
    const double power = std::log(10.0) * exponent / 20.0;
    return std::expm1(power * (db - minimumDb))
        / std::expm1(power * (maximumDb - minimumDb));
}

double AmplitudeScale::positionToDb(double position) const
{
    if (maximumDb <= minimumDb)
        return minimumDb;
    position = std::clamp(position, 0.0, 1.0);
    if (exponent == 0.0)
        return minimumDb + position * (maximumDb - minimumDb);
    const double power = std::log(10.0) * exponent / 20.0;
    return minimumDb + std::log1p(position * std::expm1(power * (maximumDb - minimumDb))) / power;
}

double AmplitudeScale::amplitudeToPosition(double amplitude) const
{
    if (maximumDb <= minimumDb)
        return 0.0;
    if (exponent == 1.0) {
        const double minimum = std::pow(10.0, minimumDb / 20.0);
        const double maximum = std::pow(10.0, maximumDb / 20.0);
        return std::clamp((amplitude - minimum) / (maximum - minimum), 0.0, 1.0);
    }
    return amplitude > 0.0 ? dbToPosition(20.0 * std::log10(amplitude)) : 0.0;
}

double AmplitudeScale::positionToAmplitude(double position) const
{
    return position > 0.0 && maximumDb > minimumDb
        ? std::pow(10.0, positionToDb(position) / 20.0) : 0.0;
}

double amplitudeToDb(double amplitude)
{
    return amplitude > 0.0 ? std::max(kAmplitudeFloorDb, 20.0 * std::log10(amplitude)) : kAmplitudeFloorDb;
}

QRect waveformAmplitudeBounds(const QRect& viewport, int contentHeight, int scroll,
    const QFontMetrics& font)
{
    return QRect(viewport.left(), viewport.top() - scroll + font.height() / 2 + 4,
        viewport.width(), std::max(1, contentHeight - font.height() - 8));
}

QVector<AmplitudeAxisTick> buildAmplitudeAxisTicks(const QRect& viewport, double zeroY,
    double amplitudeHeight, const QFontMetrics& font, double targetSpacing,
    const AmplitudeScale& scale, bool bipolar)
{
    QVector<AmplitudeAxisTick> ticks;
    if (!viewport.isValid() || amplitudeHeight <= 0.0 || scale.maximumDb <= scale.minimumDb)
        return ticks;
    const double target = std::max(targetSpacing, double(font.height() + 4));
    const auto labelCenter = [&](int y, bool major) {
        return major ? std::clamp(y, viewport.top() + font.height() / 2,
            std::max(viewport.top() + font.height() / 2, viewport.bottom() - font.height() / 2)) : y;
    };
    const auto append = [&](double db, double direction) {
        if (db < scale.minimumDb || db > scale.maximumDb)
            return;
        const double coordinate = zeroY + direction * scale.dbToPosition(db) * amplitudeHeight;
        if (coordinate < viewport.top() || coordinate > viewport.bottom())
            return;
        const int y = int(std::lround(coordinate));
        for (const auto& tick : ticks)
            if (std::abs(labelCenter(tick.y, tick.major) - labelCenter(y, db == 0.0)) < target)
                return;
        ticks.append({y, {}, db == 0.0, db});
    };
    const auto appendBoth = [&](double db) {
        append(db, -1.0);
        if (bipolar)
            append(db, 1.0);
    };
    appendBoth(0.0);
    appendBoth(std::floor(scale.maximumDb));
    appendBoth(std::ceil(scale.minimumDb));
    QVector<std::pair<double, double>> visibleRanges;
    for (double direction : {-1.0, 1.0}) {
        if (direction > 0.0 && !bipolar)
            break;
        const double start = direction * (viewport.top() - zeroY) / amplitudeHeight;
        const double end = direction * (viewport.bottom() - zeroY) / amplitudeHeight;
        const double minimum = std::max(0.0, std::min(start, end));
        const double maximum = std::min(1.0, std::max(start, end));
        if (minimum >= maximum)
            continue;
        visibleRanges.append({scale.positionToDb(minimum), scale.positionToDb(maximum)});
    }
    const auto appendStep = [&](double step) {
        for (const auto& range : visibleRanges) {
            const int first = int(std::ceil(range.first / step));
            const int last = int(std::floor(range.second / step));
            for (int index = last; index >= first; --index)
                appendBoth(index * step);
        }
    };
    for (double step : {24.0, 12.0, 6.0, 3.0, 1.0})
        appendStep(step);
    std::sort(ticks.begin(), ticks.end(), [](const auto& left, const auto& right) {
        return left.y < right.y;
    });
    for (auto& tick : ticks)
        tick.label = QString::number(tick.db, 'f', 0);
    return ticks;
}

}
