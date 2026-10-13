#include "InspectorSlotButton.h"

#include "Core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>

namespace VNX {

InspectorSlotButton::InspectorSlotButton(InspectorSlotKind kind, QWidget* parent)
    : QToolButton(parent)
    , kind_(kind)
{
    const auto& inspector = ThemeManager::instance().metrics().inspector;
    setFixedSize(inspector.bindingSlotWidth, inspector.slotHeight);
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setProperty("flatIconButton", true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(kind == InspectorSlotKind::Binding
        ? tr("Variable binding") : tr("Attenuation binding"));
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this] { update(); });
}

void InspectorSlotButton::setBound(bool bound)
{
    if (bound_ == bound) return;
    bound_ = bound;
    update();
}

void InspectorSlotButton::paintEvent(QPaintEvent* event)
{
    QToolButton::paintEvent(event);
    const auto& colors = ThemeManager::instance().colors();
    const QColor color = isEnabled() ? colors.textSecondary : colors.textDisabled;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate((width() - 14) / 2.0, (height() - 20) / 2.0);
    painter.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(bound_ ? QBrush(color) : Qt::NoBrush);
    if (kind_ == InspectorSlotKind::Binding) {
        painter.drawLine(QPointF(7, 3.5), QPointF(7, 7.5));
        painter.drawLine(QPointF(7, 12.5), QPointF(7, 16.5));
        painter.drawRoundedRect(QRectF(4, 7.5, 6, 5), 0.5, 0.5);
    } else {
        QPainterPath contour(QPointF(4, 3.5));
        contour.cubicTo(4.5, 8.5, 6, 13.5, 10, 16.5);
        contour.lineTo(4, 16.5);
        contour.closeSubpath();
        painter.drawPath(contour);
    }
}

}
