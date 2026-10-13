#include "MeterInteractionRenderer.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QStringList>

#include <algorithm>

namespace VNX {

void MeterInteractionRenderer::render(QPainter& painter,
                                      const MeterStyle::Interaction& style,
                                      const MeterInteractionOverlay& overlay)
{
    if (!overlay.visible)
        return;

    painter.setPen(QPen(style.colors.accentGuide,
                        style.metrics.accentGuideLineWidth));
    for (const QLineF& guide : overlay.accentGuides)
        painter.drawLine(guide);

    painter.setPen(QPen(style.colors.crosshair,
                        style.metrics.crosshairLineWidth));
    painter.drawLine(QPointF(overlay.position.x(), overlay.plotBounds.top()),
                     QPointF(overlay.position.x(), overlay.plotBounds.bottom()));
    painter.drawLine(QPointF(overlay.plotBounds.left(), overlay.position.y()),
                     QPointF(overlay.plotBounds.right(), overlay.position.y()));

    painter.setFont(overlay.font);
    const QFontMetrics fontMetrics(overlay.font);
    const QStringList lines = overlay.text.split('\n');
    qreal width = 0.0;
    for (const QString& line : lines)
        width = std::max(width, static_cast<qreal>(fontMetrics.horizontalAdvance(line)));
    width += style.metrics.horizontalPadding;
    const qreal height = lines.size() * style.metrics.lineHeight
                       + style.metrics.extraHeight;

    qreal x = overlay.position.x() + style.metrics.horizontalOffset;
    qreal y = overlay.position.y() - height - style.metrics.aboveGap;
    if (x + width > overlay.plotBounds.right())
        x = overlay.position.x() - width - style.metrics.horizontalOffset;
    if (y < overlay.plotBounds.top())
        y = overlay.position.y() + style.metrics.belowOffset;

    painter.setPen(Qt::NoPen);
    painter.setBrush(style.colors.readoutBackground);
    painter.drawRoundedRect(QRectF(x, y, width, height),
                            style.metrics.radius,
                            style.metrics.radius);

    painter.setPen(style.colors.readoutText);
    qreal lineY = y + style.metrics.extraHeight / 2.0;
    for (const QString& line : lines) {
        painter.drawText(QRectF(x, lineY, width, style.metrics.lineHeight),
                         Qt::AlignCenter,
                         line);
        lineY += style.metrics.lineHeight;
    }
}

}
