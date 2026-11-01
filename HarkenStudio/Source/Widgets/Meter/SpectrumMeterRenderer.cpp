#include "SpectrumMeterRenderer.h"

#include "MeterInteractionRenderer.h"
#include "MeterScale.h"
#include "Core/FontManager.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <algorithm>

namespace VNX {
namespace {

float levelToY(const QRect& plot, float amplitude, const AmplitudeScale& amplitudeAxis)
{
    return plot.bottom() - amplitudeAxis.amplitudeToPosition(amplitude) * (plot.height() - 1);
}

QPainterPath smoothPath(const QRect& plot,
                        const std::array<float, kSpectrumMeterDisplayPoints>& levels,
                        const AmplitudeScale& amplitudeAxis,
                        float controlRatio)
{
    QPainterPath path;
    for (int i = 0; i < kSpectrumMeterDisplayPoints; ++i) {
        const float x = plot.left()
            + static_cast<float>(i) / (kSpectrumMeterDisplayPoints - 1) * (plot.width() - 1);
        const float y = levelToY(plot, levels[i], amplitudeAxis);
        if (i == 0) {
            path.moveTo(x, y);
            continue;
        }

        const float previousX = plot.left()
            + static_cast<float>(i - 1) / (kSpectrumMeterDisplayPoints - 1) * (plot.width() - 1);
        const float controlOffset = (x - previousX) * controlRatio;
        const float previousY = levelToY(plot, levels[i - 1], amplitudeAxis);
        path.cubicTo(previousX + controlOffset,
                     previousY,
                     x - controlOffset,
                     y,
                     x,
                     y);
    }
    return path;
}

}

SpectrumMeterLayout SpectrumMeterRenderer::layout(
    const QRect& bounds,
    const MeterStyle::Spectrum& style)
{
    const auto& metrics = style.metrics;
    return {MeterScale::plotBounds(bounds,
        {metrics.plotInsetLeft, metrics.plotInsetTop, metrics.plotInsetRight, metrics.plotInsetBottom}, true)};
}

void SpectrumMeterRenderer::render(QPainter& painter,
                                   const MeterStyle& style,
                                   const SpectrumMeterRenderFrame& frame)
{
    const auto& colors = style.spectrumMeter.colors;
    const auto& metrics = style.spectrumMeter.metrics;
    painter.fillRect(frame.canvasBounds, colors.canvasBackground);
    if (!frame.hasPlot)
        return;

    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics font(painter.font());
    const QStringList dbLabels = meterTickLabels(frame.dbTicks);
    for (int i = 0; i < frame.dbTicks.size(); ++i) {
        const auto& tick = frame.dbTicks[i];
        if (tick.y != frame.plotBounds.top()) {
            painter.setPen(QPen(colors.horizontalGrid, metrics.gridLineWidth));
            painter.drawLine(QPointF(frame.plotBounds.left(), tick.y),
                             QPointF(frame.plotBounds.right(), tick.y));
        }
        painter.setPen(colors.axisText);
        painter.drawText(QRectF(frame.canvasBounds.left() + metrics.dbLabelLeft,
                                tick.y - font.height() / 2.0,
                                frame.plotBounds.left() - frame.canvasBounds.left() - 6,
                                font.height()),
                         Qt::AlignRight | Qt::AlignVCenter, dbLabels[i]);
    }
    const QStringList frequencyLabels = meterTickLabels(frame.frequencyTicks);
    for (int i = 0; i < frame.frequencyTicks.size(); ++i) {
        const auto& tick = frame.frequencyTicks[i];
        const qreal x = frame.plotBounds.left()
            + tick.normalizedPosition * (frame.plotBounds.width() - 1);
        painter.setPen(QPen(colors.verticalGrid, metrics.gridLineWidth));
        painter.drawLine(QPointF(x, frame.plotBounds.top()), QPointF(x, frame.plotBounds.bottom()));
        if (frequencyLabels[i].isEmpty())
            continue;
        const qreal width = font.horizontalAdvance(frequencyLabels[i]) + 4;
        const qreal left = std::clamp(x - width / 2.0, qreal(frame.plotBounds.left()),
            std::max(qreal(frame.plotBounds.left()), frame.plotBounds.right() - width + 1));
        painter.setPen(colors.axisText);
        painter.drawText(QRectF(left, frame.plotBounds.bottom() + metrics.frequencyLabelTopOffset,
                                width, font.height()), Qt::AlignCenter, frequencyLabels[i]);
    }

    painter.save();
    painter.setClipRect(frame.plotBounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath linePath = smoothPath(frame.plotBounds,
                                       frame.smoothedLevels,
                                       frame.amplitudeAxis,
                                       metrics.curveControlRatio);
    QPainterPath fillPath = linePath;
    fillPath.lineTo(frame.plotBounds.right(), frame.plotBounds.bottom());
    fillPath.lineTo(frame.plotBounds.left(), frame.plotBounds.bottom());
    fillPath.closeSubpath();

    QLinearGradient fillGradient(0,
                                 frame.plotBounds.top(),
                                 0,
                                 frame.plotBounds.bottom());
    fillGradient.setColorAt(metrics.fillTopStop, colors.fillTop);
    fillGradient.setColorAt(metrics.fillUpperMiddleStop, colors.fillUpperMiddle);
    fillGradient.setColorAt(metrics.fillLowerMiddleStop, colors.fillLowerMiddle);
    fillGradient.setColorAt(metrics.fillBottomStop, colors.fillBottom);
    painter.fillPath(fillPath, fillGradient);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colors.lineGlowWide, metrics.lineGlowWideWidth));
    painter.drawPath(linePath);
    painter.setPen(QPen(colors.lineGlowMedium, metrics.lineGlowMediumWidth));
    painter.drawPath(linePath);
    painter.setPen(QPen(colors.lineGlowNarrow, metrics.lineGlowNarrowWidth));
    painter.drawPath(linePath);
    painter.setPen(QPen(colors.lineHighlight, metrics.lineHighlightWidth));
    painter.drawPath(linePath);

    QPainterPath peakPath;
    for (int i = 0; i < kSpectrumMeterDisplayPoints; ++i) {
        const float x = frame.plotBounds.left()
            + static_cast<float>(i) / (kSpectrumMeterDisplayPoints - 1)
                * (frame.plotBounds.width() - 1);
        const float y = levelToY(frame.plotBounds, frame.peakLevels[i], frame.amplitudeAxis);
        if (i == 0)
            peakPath.moveTo(x, y);
        else
            peakPath.lineTo(x, y);
    }
    painter.setPen(QPen(colors.peakLine, metrics.peakLineWidth));
    painter.drawPath(peakPath);
    painter.restore();

    painter.setPen(QPen(colors.horizontalGrid, metrics.gridLineWidth));
    painter.drawLine(frame.plotBounds.topLeft(), frame.plotBounds.topRight());

    MeterInteractionRenderer::render(painter,
                                     style.interaction,
                                     frame.interaction);
}

}
