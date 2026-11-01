#include "SpectrogramMeterRenderer.h"
#include "MeterScale.h"
#include "Core/FontManager.h"

#include <QPainter>
#include <QPen>

namespace VNX {

SpectrogramMeterLayout SpectrogramMeterRenderer::layout(
    const QRect& bounds,
    const MeterStyle::Spectrogram& style)
{
    const auto& metrics = style.metrics;
    return {MeterScale::plotBounds(bounds,
        {metrics.plotInsetLeft, metrics.plotInsetTop, metrics.plotInsetRight, metrics.plotInsetBottom}, true)};
}

void SpectrogramMeterRenderer::render(
    QPainter& painter,
    const MeterStyle& style,
    const SpectrogramMeterRenderFrame& frame)
{
    const auto& spectrogram = style.spectrogramMeter;
    const auto& colors = spectrogram.colors;
    const auto& metrics = spectrogram.metrics;
    painter.fillRect(frame.canvasBounds, colors.canvasBackground);
    if (!frame.hasPlot)
        return;

    painter.fillRect(frame.plotBounds, colors.emptyHistory);
    if (!frame.waterfallImage.isNull())
        painter.drawImage(frame.plotBounds, frame.waterfallImage);

    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics font(painter.font());
    const QStringList labels = meterTickLabels(frame.frequencyTicks);
    for (int i = 0; i < frame.frequencyTicks.size(); ++i)
    {
        const auto& tick = frame.frequencyTicks[i];
        painter.setPen(QPen(colors.grid, metrics.gridLineWidth));
        painter.drawLine(QPointF(frame.plotBounds.left(), tick.y),
                         QPointF(frame.plotBounds.right(), tick.y));
        painter.setPen(colors.axisText);
        painter.drawText(
            QRectF(frame.canvasBounds.left() + metrics.frequencyLabelLeft,
                   tick.y - font.height() / 2.0,
                   frame.plotBounds.left() - frame.canvasBounds.left() - 6,
                   font.height()),
            Qt::AlignRight | Qt::AlignVCenter,
            labels[i]);
    }

    painter.setPen(QPen(colors.grid, metrics.gridLineWidth));
    painter.drawLine(frame.plotBounds.topLeft(), frame.plotBounds.bottomLeft());
    painter.drawLine(frame.plotBounds.topRight(), frame.plotBounds.bottomRight());

}

}
