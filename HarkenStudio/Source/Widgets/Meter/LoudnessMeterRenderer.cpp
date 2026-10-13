#include "LoudnessMeterRenderer.h"

#include "MeterInteractionRenderer.h"
#include "Core/FontManager.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QtGlobal>

#include <algorithm>

namespace VNX {
namespace {

QColor baseColor(const MeterStyle& style, LoudnessMeterColorRole role)
{
    switch (role) {
    case LoudnessMeterColorRole::Momentary:
        return style.lufsMomentary;
    case LoudnessMeterColorRole::ShortTerm:
        return style.lufsShortTerm;
    case LoudnessMeterColorRole::Integrated:
        return style.lufsIntegrated;
    }
    Q_UNREACHABLE_RETURN(style.lufsMomentary);
}

void renderBar(QPainter& painter,
               const MeterStyle& meterStyle,
               const LoudnessMeterBarFrame& frame,
               const QFont& labelFont,
               const QFont& valueFont)
{
    const auto& style = meterStyle.loudnessMeter;
    const auto& colors = style.colors;
    const auto& metrics = style.metrics;
    const QRect& area = frame.bounds;
    const QColor color = baseColor(meterStyle, frame.colorRole);

    painter.fillRect(area, colors.trackBackground);

    const qreal barHeight = area.bottom() - frame.levelTop;
    const QRectF barRect(area.left(),
                         frame.levelTop,
                         area.width(),
                         barHeight);
    QLinearGradient gradient(0, barRect.top(), 0, area.bottom());
    gradient.setColorAt(metrics.gradientTopStop,
                        color.lighter(metrics.gradientTopLighterFactor));
    gradient.setColorAt(metrics.gradientBottomStop,
                        color.darker(metrics.gradientBottomDarkerFactor));
    painter.fillRect(barRect, gradient);

    if (barHeight > metrics.highlightMinimumBarHeight) {
        QColor highlight = color.lighter(metrics.highlightLighterFactor);
        highlight.setAlpha(metrics.highlightAlpha);
        painter.fillRect(QRectF(barRect.left(),
                                barRect.top(),
                                barRect.width(),
                                std::min(metrics.highlightHeight, barHeight)),
                         highlight);
    }

    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    if (frame.targetVisible) {
        painter.setPen(QPen(colors.targetLine, metrics.targetLineWidth));
        painter.drawLine(QPointF(area.left(), frame.targetY),
                         QPointF(area.right(), frame.targetY));
        painter.setPen(colors.targetText);
        painter.drawText(QRectF(area.left() + metrics.targetLabelHorizontalInset,
                                frame.targetY - metrics.targetLabelTopOffset,
                                area.width()
                                    - 2.0 * metrics.targetLabelHorizontalInset,
                                metrics.targetLabelHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         compactAxisLabels({frame.targetLabel}).front());
    }

    painter.setPen(colors.labelText);
    painter.setFont(labelFont);
    painter.drawText(QRectF(area.left(),
                            area.top(),
                            area.width(),
                            metrics.channelLabelHeight),
                     Qt::AlignCenter,
                     frame.channelLabel);

    painter.setPen(colors.valueText);
    painter.setFont(valueFont);
    painter.drawText(QRectF(area.left(),
                            area.bottom() - metrics.valueLabelBottomOffset,
                            area.width(),
                            metrics.valueLabelHeight),
                     Qt::AlignCenter,
                     frame.valueLabel);
}

}

LoudnessMeterLayout LoudnessMeterRenderer::layout(
    const QRect& bounds,
    const MeterStyle::Loudness& style)
{
    const auto& metrics = style.metrics;
    LoudnessMeterLayout result;
    result.meterBounds = bounds.adjusted(metrics.outerInsetLeft,
                                         metrics.outerInsetTop,
                                         -metrics.outerInsetRight,
                                         -metrics.outerInsetBottom);
    const int barWidth = std::max(
        metrics.minimumBarWidth,
        (result.meterBounds.width() - metrics.reservedWidth) / 3);
    for (int index = 0; index < static_cast<int>(result.barBounds.size()); ++index) {
        result.barBounds[index] = QRect(
            result.meterBounds.left() + index * (barWidth + metrics.barGap),
            result.meterBounds.top(),
            barWidth,
            result.meterBounds.height());
    }
    return result;
}

void LoudnessMeterRenderer::render(QPainter& painter,
                                   const MeterStyle& style,
                                   const LoudnessMeterRenderFrame& frame)
{
    painter.fillRect(frame.canvasBounds,
                     style.loudnessMeter.colors.canvasBackground);
    for (const LoudnessMeterBarFrame& bar : frame.bars) {
        renderBar(painter,
                  style,
                  bar,
                  frame.labelFont,
                  frame.valueFont);
    }
    MeterInteractionRenderer::render(painter,
                                     style.interaction,
                                     frame.interaction);
}

}
