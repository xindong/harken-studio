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

inline QColor meterGridColor(QColor color, bool major)
{
    color.setAlphaF(major ? 0.8 : 0.5);
    return color;
}

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

enum class LoudnessMeterColorRole
{
    Momentary,
    ShortTerm,
    Integrated
};

struct LoudnessMeterBarFrame
{
    QRect bounds;
    LoudnessMeterColorRole colorRole = LoudnessMeterColorRole::Momentary;
    qreal levelTop = 0.0;
    bool targetVisible = false;
    qreal targetY = 0.0;
    QString targetLabel;
    QString channelLabel;
    QString valueLabel;
};

struct LoudnessMeterRenderFrame
{
    QRect canvasBounds;
    std::array<LoudnessMeterBarFrame, 3> bars;
    QFont labelFont;
    QFont valueFont;
    MeterInteractionOverlay interaction;
};

struct LoudnessMeterLayout
{
    QRect meterBounds;
    std::array<QRect, 3> barBounds;
};

struct VUMeterHeaderFrame
{
    QRect canvasBounds;
    QRect titleBounds;
    QString title;
    QFont titleFont;
};

struct VUAnalogTickFrame
{
    qreal normalizedPosition = 0.0;
    QString text;
    bool overload = false;
};

struct VUAnalogDialFrame
{
    QRect bounds;
    qreal needlePosition = 0.0;
    QString valueLabel;
    QString channelLabel;
};

struct VUAnalogMeterRenderFrame
{
    VUMeterHeaderFrame header;
    std::array<VUAnalogDialFrame, 4> dials;
    int dialCount = 0;
    QVector<VUAnalogTickFrame> ticks;
    qreal overloadStartPosition = 0.0;
    qreal overloadEndPosition = 0.0;
    QFont valueFont;
    QFont labelFont;
};

struct VUBarFrame
{
    QRect bounds;
    qreal levelTop = 0.0;
    QVector<qreal> gridLines;
    QString valueLabel;
    QString channelLabel;
};

struct VUBarMeterRenderFrame
{
    VUMeterHeaderFrame header;
    std::array<VUBarFrame, 4> bars;
    int barCount = 0;
    QFont valueFont;
    QFont labelFont;
};

enum class VUMeterLayoutMode
{
    AnalogTwo,
    AnalogFour,
    BarTwo,
    BarFour
};

struct VUMeterLayout
{
    QRect titleBounds;
    QRect contentBounds;
    std::array<QRect, 4> itemBounds;
    int itemCount = 0;
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
