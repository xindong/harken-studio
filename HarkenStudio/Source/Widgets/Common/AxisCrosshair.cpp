#include "AxisCrosshair.h"

#include "Core/FontManager.h"
#include "Core/ThemeManager.h"

#include <QPainter>
#include <QPen>
#include <QRegion>

#include <algorithm>

namespace harken::studio {
namespace {

void drawIndicatorSurface(QPainter& painter, const QColor& background, const QRectF& bounds,
                          qreal radius, const QPen& border)
{
    painter.setPen(border);
    painter.setBrush(background);
    painter.drawRoundedRect(bounds, radius, radius);
}

}

AxisIndicatorButton::AxisIndicatorButton(const QString& iconId, QWidget* parent) : QAbstractButton(parent)
{
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    const auto refreshIcon = [this, iconId] {
        auto& theme = ThemeManager::instance();
        setIcon(theme.resolveIcon(iconId, {.tint = theme.snapshot()->domains.curve.colors.readoutText}).icon);
    };
    refreshIcon();
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, refreshIcon);
}

void AxisIndicatorButton::paintEvent(QPaintEvent*)
{
    const auto theme = ThemeManager::instance().snapshot();
    const auto& colors = theme->ui.colors;
    const int borderWidth = theme->ui.metrics.borderWidth;
    const qreal inset = borderWidth / 2.0;
    const auto circle = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& background = underMouse() ? colors.bgHover : theme->domains.curve.colors.readoutBackground;
    drawIndicatorSurface(painter, background, circle, circle.width() / 2, QPen(colors.borderSecondary, borderWidth));
    const int extent = theme->ui.metrics.iconExtent;
    icon().paint(&painter, (width() - extent) / 2, (height() - extent) / 2, extent, extent);
}

void AxisIndicatorButton::resizeEvent(QResizeEvent* event)
{
    QAbstractButton::resizeEvent(event);
    setMask(QRegion(rect().adjusted(-1, -1, 1, 1), QRegion::Ellipse).intersected(rect()));
}

bool AxisIndicatorButton::hitButton(const QPoint& position) const
{
    return mask().contains(position);
}

void drawAxisReadout(QPainter& painter, const CurveStyle& style,
                     const QRectF& bounds, const QString& label)
{
    painter.save();
    drawIndicatorSurface(painter, style.colors.readoutBackground, bounds, style.metrics.readoutRadius, Qt::NoPen);
    painter.setFont(FontManager::instance().getFont(FontRole::Caption));
    painter.setPen(style.colors.readoutText);
    const int available = static_cast<int>(std::max(0.0, bounds.width() - style.metrics.readoutHorizontalPadding));
    painter.drawText(bounds, Qt::AlignCenter, painter.fontMetrics().elidedText(label, Qt::ElideRight, available));
    painter.restore();
}

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

    painter.setFont(FontManager::instance().getFont(FontRole::Caption));
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
    drawAxisReadout(painter, style, QRectF(corner, bubbleSize), crosshair.label);
    painter.restore();
}

}
