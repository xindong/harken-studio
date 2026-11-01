#include "PeakMeterRenderer.h"

#include "Core/ThemeAssetResolver.h"
#include "Core/FontManager.h"
#include "MeterInteractionRenderer.h"
#include "MeterScale.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPen>

namespace VNX {
namespace {

void renderBar(QPainter& painter,
               const MeterStyle::Peak& style,
               const PeakMeterBarFrame& frame,
               const QFont& labelFont,
               const ThemeImageResourcePtr& peakCap,
               qreal horizontalInset,
               qreal annotationOpacity)
{
    const auto& colors = style.colors;
    const auto& metrics = style.metrics;
    const QRect& area = frame.bounds;

    painter.save();
    painter.setClipRect(area, Qt::IntersectClip);

    const qreal barX = area.left() + horizontalInset;
    const qreal barWidth = area.width() - 2.0 * horizontalInset;
    if (frame.showLevel) {
        const QRectF barRect(barX,
                             frame.levelTop,
                             barWidth,
                             area.bottom() - frame.levelTop);
        QLinearGradient gradient(0, area.top(), 0, area.bottom());
        const auto gradientStop = [&](qreal position) {
            const auto& axis = frame.amplitudeAxis;
            return 1.0 - axis.dbToPosition(axis.maximumDb
                - position * (axis.maximumDb - axis.minimumDb));
        };
        gradient.setColorAt(gradientStop(metrics.gradientErrorStop), colors.gradientError);
        gradient.setColorAt(gradientStop(metrics.gradientWarningStop), colors.gradientWarning);
        gradient.setColorAt(gradientStop(metrics.gradientYellowStop), colors.gradientYellow);
        gradient.setColorAt(gradientStop(metrics.gradientAccentStop), colors.gradientAccent);

        painter.save();
        painter.setClipRect(barRect.toAlignedRect(), Qt::IntersectClip);
        painter.fillRect(area, gradient);
        painter.restore();
    }

    if (frame.showRms) {
        const qreal rmsX = barX + barWidth * metrics.rmsLeftInsetRatio;
        const qreal rmsWidth = barWidth * metrics.rmsWidthRatio;
        painter.fillRect(QRectF(rmsX,
                                frame.rmsTop,
                                rmsWidth,
                                area.bottom() - frame.rmsTop),
                         colors.rmsOverlay);
    }

    if (frame.showHold) {
        painter.fillRect(QRectF(area.left(),
                                frame.holdY + metrics.holdHaloTopOffset,
                                area.width(),
                                metrics.holdHaloHeight),
                         colors.holdHalo);
        painter.fillRect(QRectF(area.left(),
                                frame.holdY + metrics.holdLineTopOffset,
                                area.width(),
                                metrics.holdLineHeight),
                         colors.holdLine);
        if (peakCap && !peakCap->image.isNull()) {
            const qreal height = peakCap->image.height()
                / peakCap->image.devicePixelRatio();
            painter.drawImage(QRectF(area.left(), frame.holdY - height / 2.0,
                                     area.width(), height),
                              peakCap->image);
        }
    }

    painter.restore();
    painter.save();
    painter.setOpacity(painter.opacity() * annotationOpacity);
    painter.setPen(colors.label);
    painter.setFont(labelFont);
    painter.drawText(QRect(area.left(),
                           area.bottom() - metrics.channelLabelBottomOffset,
                           area.width(),
                           metrics.labelHeight),
                     Qt::AlignCenter,
                     frame.channelLabel);
    painter.drawText(QRect(area.left(),
                           area.bottom() - metrics.valueLabelBottomOffset,
                           area.width(),
                           metrics.labelHeight),
                     Qt::AlignCenter,
                     frame.valueLabel);
    painter.restore();
}

}

PeakMeterLayout PeakMeterRenderer::layout(const QRect& bounds,
                                          const MeterStyle::Peak& style,
                                          int channelCount,
                                          PeakMeterLayoutMode mode)
{
    const auto& metrics = style.metrics;
    const bool separateScale = mode == PeakMeterLayoutMode::Axis;
    const int gap = separateScale ? metrics.channelGap : 1;
    const int reservedWidth = separateScale ? metrics.reservedWidth : gap;
    PeakMeterLayout result;
    result.meterBounds = MeterScale::plotBounds(bounds,
        {metrics.outerInsetLeft, metrics.outerInsetTop, metrics.outerInsetRight, metrics.outerInsetBottom}, false);
    if (!separateScale) {
        result.meterBounds.setLeft(bounds.left());
        result.meterBounds.setRight(bounds.right());
    }
    const int barWidth = channelCount == 1 ? result.meterBounds.width()
        : (result.meterBounds.width() - reservedWidth) / 2;
    result.barBounds[0] = QRect(result.meterBounds.left(),
                                result.meterBounds.top(),
                                barWidth,
                                result.meterBounds.height());
    result.barBounds[1] = QRect(result.meterBounds.left() + barWidth + gap,
                                result.meterBounds.top(),
                                channelCount == 1 ? barWidth : result.meterBounds.width() - reservedWidth - barWidth,
                                result.meterBounds.height());
    return result;
}

void PeakMeterRenderer::render(QPainter& painter,
                               const MeterStyle& style,
                               const PeakMeterRenderFrame& frame)
{
    const auto& colors = style.peakMeter.colors;
    const auto& metrics = style.peakMeter.metrics;
    const bool inlineScale = frame.layoutMode == PeakMeterLayoutMode::Inline;
    const qreal annotationOpacity = inlineScale ? 0.5 : 1.0;
    painter.fillRect(frame.canvasBounds, colors.canvasBackground);
    QRect plot;
    for (int index = 0; index < frame.barCount; ++index) {
        plot = plot.united(frame.bars[index].bounds);
        painter.fillRect(frame.bars[index].bounds, colors.trackBackground);
    }
    painter.save();
    painter.setOpacity(painter.opacity() * annotationOpacity);
    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics font(painter.font());
    const QStringList labels = meterTickLabels(frame.ticks);
    for (int index = 0; index < frame.ticks.size(); ++index) {
        const auto& tick = frame.ticks[index];
        const int width = font.horizontalAdvance(labels[index]);
        const QRectF label = inlineScale
            ? QRectF(plot.left() + (plot.width() - width) / 2.0,
                     tick.y - font.height() / 2.0,
                     width, font.height())
            : QRectF(frame.scaleBounds.left(), tick.y - font.height() / 2.0,
                     frame.scaleBounds.width() - 6, font.height());
        painter.setPen(QPen(colors.grid, metrics.gridLineWidth));
        if (inlineScale) {
            const qreal left = plot.left() + 2.0;
            const qreal right = plot.right() - 2.0;
            if (labels[index].isEmpty())
                painter.drawLine(QPointF(left, tick.y), QPointF(right, tick.y));
            else {
                if (label.left() - 2.0 > left)
                    painter.drawLine(QPointF(left, tick.y), QPointF(label.left() - 2.0, tick.y));
                if (label.right() + 2.0 < right)
                    painter.drawLine(QPointF(label.right() + 2.0, tick.y), QPointF(right, tick.y));
            }
        } else {
            painter.drawLine(QPointF(plot.left(), tick.y), QPointF(plot.right(), tick.y));
        }
        if (labels[index].isEmpty())
            continue;
        painter.setPen(colors.tickText);
        painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter, labels[index]);
    }
    painter.restore();
    for (int index = 0; index < frame.barCount; ++index)
        renderBar(painter, style.peakMeter, frame.bars[index], frame.labelFont, frame.peakCap,
            inlineScale ? 0.0 : metrics.barHorizontalInset, annotationOpacity);
    if (!inlineScale && plot.isValid()) {
        painter.setPen(QPen(colors.grid, metrics.gridLineWidth));
        painter.drawLine(plot.topLeft(), plot.bottomLeft());
        painter.drawLine(plot.topRight(), plot.bottomRight());
    }
    MeterInteractionRenderer::render(painter,
                                     style.interaction,
                                     frame.interaction);
}

}
