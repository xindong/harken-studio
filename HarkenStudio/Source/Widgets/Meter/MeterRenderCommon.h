#pragma once

#include "Core/NumericFormat.h"
#include "Widgets/Common/AmplitudeAxis.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

#include <array>
#include <memory>

namespace VNX {

template <typename Tick>
QStringList meterTickLabels(const QVector<Tick>& ticks)
{
    QStringList labels;
    labels.reserve(ticks.size());
    for (const auto& tick : ticks)
        labels.append(tick.text);
    return compactAxisLabels(std::move(labels));
}

struct ThemeImageResource;

struct MeterScaleTick
{
    qreal y = 0.0;
    QString text;
    bool major = true;
};

struct MeterInteractionOverlay
{
    bool visible = false;
    QRectF plotBounds;
    QPointF position;
    QVector<QLineF> accentGuides;
    QString text;
    QFont font;
};

struct PeakMeterBarFrame
{
    QRect bounds;
    AmplitudeScale amplitudeAxis {kAmplitudeFloorDb, 6.0, 0.3};
    bool showLevel = false;
    bool showRms = false;
    bool showHold = false;
    bool clipped = false;
    qreal levelTop = 0.0;
    qreal rmsTop = 0.0;
    qreal holdY = 0.0;
    QString channelLabel;
    QString valueLabel;
};

enum class PeakMeterLayoutMode { Axis, Inline };

struct PeakMeterRenderFrame
{
    PeakMeterLayoutMode layoutMode = PeakMeterLayoutMode::Axis;
    QRect canvasBounds;
    QRect scaleBounds;
    QVector<MeterScaleTick> ticks;
    std::array<PeakMeterBarFrame, 2> bars;
    int barCount = 2;
    QFont clipFont;
    QFont labelFont;
    std::shared_ptr<const ThemeImageResource> peakCap;
    MeterInteractionOverlay interaction;
};

struct PeakMeterLayout
{
    QRect meterBounds;
    std::array<QRect, 2> barBounds;
};

inline constexpr int kSpectrumMeterDisplayPoints = 256;

struct SpectrumMeterFrequencyTick
{
    float normalizedPosition = 0.0f;
    QString text;
    bool major = true;
};

struct SpectrumMeterRenderFrame
{
    QRect canvasBounds;
    QRect plotBounds;
    bool hasPlot = false;
    AmplitudeScale amplitudeAxis {kAmplitudeFloorDb, 6.0, 0.3};
    std::array<float, kSpectrumMeterDisplayPoints> smoothedLevels {};
    std::array<float, kSpectrumMeterDisplayPoints> peakLevels {};
    QVector<MeterScaleTick> dbTicks;
    QVector<SpectrumMeterFrequencyTick> frequencyTicks;
    MeterInteractionOverlay interaction;
};

struct SpectrumMeterLayout
{
    QRect plotBounds;
};

struct SpectrogramMeterRenderFrame
{
    QRect canvasBounds;
    QRect plotBounds;
    bool hasPlot = false;
    QImage waterfallImage;
    QVector<MeterScaleTick> frequencyTicks;
};

struct SpectrogramMeterLayout
{
    QRect plotBounds;
};

}
