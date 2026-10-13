#include "MeterScale.h"

#include "FrequencyScale.h"
#include "Core/FontManager.h"

#include <QFontMetrics>

#include <algorithm>
#include <cmath>

namespace VNX::MeterScale {

float dbToPosition(float db, float maximumDb)
{
    return (std::clamp(db, kMinDb, maximumDb) - kMinDb) / (maximumDb - kMinDb);
}

float positionToDb(float position, float maximumDb)
{
    position = std::clamp(position, 0.0f, 1.0f);
    return kMinDb + position * (maximumDb - kMinDb);
}

QString frequencyLabel(float frequency)
{
    return frequency >= 1000.0f
        ? formatAxisValue(frequency / 1000.0, 1) + QStringLiteral("k")
        : formatAxisValue(frequency, 0);
}

QRect plotBounds(const QRect& canvas, const QMargins& insets, bool horizontalAxis)
{
    const QFontMetrics font(FontManager::instance().getFont(FontRole::Axis));
    const int verticalInset = std::max(10, font.height() / 2 + 4);
    return canvas.adjusted(std::max({30, insets.left(), font.horizontalAdvance(QStringLiteral("-96.0")) + 10}),
        std::max(insets.top(), verticalInset), -std::max(insets.right(), 10),
        -std::max(insets.bottom(), horizontalAxis ? font.height() + 8 : verticalInset));
}

QVector<SpectrumMeterFrequencyTick> frequencyTicks(float maximum, int extent,
                                                  const QFontMetrics& font, bool vertical)
{
    QVector<SpectrumMeterFrequencyTick> result;
    if (extent < 2 || maximum <= kMinFrequency)
        return result;
    QVector<QPair<qreal, qreal>> labels;
    const auto add = [&](float frequency, bool label) {
        if (frequency < kMinFrequency || frequency > maximum)
            return;
        const float position = FrequencyScale::freqToNorm(frequency, kMinFrequency, maximum, FreqScaleType::Log);
        const qreal pixel = position * (extent - 1);
        for (const auto& tick : result)
            if (std::abs(tick.normalizedPosition - position) * (extent - 1) < 8.0)
                return;
        QString text = frequencyLabel(frequency);
        const qreal size = vertical ? font.height() + 8 : font.horizontalAdvance(text) + 12;
        const qreal start = vertical ? pixel - size / 2.0
            : std::clamp(pixel - size / 2.0, 0.0, std::max(0.0, extent - size));
        for (const auto& occupied : labels)
            if (start < occupied.second && start + size > occupied.first)
                label = false;
        if (label)
            labels.append({start, start + size});
        const bool major = frequency == 100.0f || frequency == 1000.0f || frequency == 10000.0f;
        result.append({position, label ? text : QString{}, major});
    };
    add(kMinFrequency, true);
    add(maximum, true);
    for (float decade : {100.0f, 1000.0f, 10000.0f})
        add(decade, true);
    for (float decade : {10.0f, 100.0f, 1000.0f, 10000.0f})
        for (float factor : {2.0f, 5.0f})
            add(decade * factor, true);
    for (float decade : {10.0f, 100.0f, 1000.0f, 10000.0f})
        for (float factor : {3.0f, 4.0f, 6.0f, 7.0f, 8.0f, 9.0f})
            add(decade * factor, false);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.normalizedPosition < right.normalizedPosition;
    });
    return result;
}

}
