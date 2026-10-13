#include "AxisCrosshair.h"

#include "Core/FontManager.h"

#include <QPainter>
#include <QPen>

#include <algorithm>

namespace VNX {

void drawAxisCrosshairLines(QPainter& painter, const CurveStyle& style,
                           const QRectF& bounds, const QPointF& position,
                           Qt::Orientations axes)
{
    if (bounds.isEmpty() || !axes)
        return;

    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto drawLines = [&] {
        if (axes.testFlag(Qt::Vertical))
            painter.drawLine(QPointF(position.x(), bounds.top()),
                QPointF(position.x(), bounds.bottom()));
        if (axes.testFlag(Qt::Horizontal))
            painter.drawLine(QPointF(bounds.left(), position.y()),
                QPointF(bounds.right(), position.y()));
    };
    QPen pen(style.colors.crosshairContrast, style.metrics.crosshairLineWidth,
        Qt::SolidLine, Qt::FlatCap);
    painter.setPen(pen);
    drawLines();
    pen.setColor(style.colors.crosshair);
    pen.setDashPattern({style.metrics.crosshairDashLength, style.metrics.crosshairDashGap});
    painter.setPen(pen);
    drawLines();
    painter.restore();
}

void drawAxisCrosshair(QPainter& painter, const CurveStyle& style,
                      const QRectF& bounds, const AxisCrosshair& crosshair)
{
    if (!crosshair.visible || bounds.isEmpty())
        return;

    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    drawAxisCrosshairLines(painter, style, bounds, crosshair.position);

    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QSizeF bubbleSize(std::min<qreal>(bounds.width(),
        painter.fontMetrics().horizontalAdvance(crosshair.label)
            + style.metrics.readoutHorizontalPadding),
        std::min(bounds.height(), std::max<qreal>(style.metrics.readoutHeight,
            painter.fontMetrics().height() + 4.0)));
    QPointF corner(crosshair.position.x() + style.metrics.readoutHorizontalOffset,
                   crosshair.position.y() - style.metrics.readoutAboveOffset);
    if (corner.x() + bubbleSize.width() > bounds.right())
        corner.setX(crosshair.position.x() - bubbleSize.width() - style.metrics.readoutHorizontalOffset);
    if (corner.y() < bounds.top())
        corner.setY(crosshair.position.y() + style.metrics.readoutBelowOffset);
    corner.setX(std::clamp(corner.x(), bounds.left(), bounds.right() - bubbleSize.width()));
    corner.setY(std::clamp(corner.y(), bounds.top(), bounds.bottom() - bubbleSize.height()));
    const QRectF bubble(corner, bubbleSize);
    painter.setPen(Qt::NoPen);
    painter.setBrush(style.colors.readoutBackground);
    painter.drawRoundedRect(bubble, style.metrics.readoutRadius, style.metrics.readoutRadius);
    painter.setPen(style.colors.readoutText);
    painter.drawText(bubble, Qt::AlignCenter,
        painter.fontMetrics().elidedText(crosshair.label, Qt::ElideRight,
            static_cast<int>(std::max(0.0, bubble.width() - style.metrics.readoutHorizontalPadding))));
    painter.restore();
}

}
