#include "RotaryKnobRenderer.h"

#include <QPainter>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace VNX {

void RotaryKnobRenderer::render(QPainter& painter,
                                const UiTokens& style,
                                const RotaryKnobRenderFrame& frame)
{
    if (frame.bounds.isEmpty())
        return;

    const double position = std::clamp(frame.position, 0.0, 1.0);
    const QColor disabled = style.colors.textDisabled;
    const QColor arcTrack = frame.enabled ? style.colors.borderPrimary : disabled;
    const QColor arcValue = frame.enabled ? style.colors.accent : disabled;
    const QColor indicator = frame.enabled ? style.colors.knobIndicator : disabled;
    const QColor face = frame.enabled ? style.colors.bgTertiary
                                      : style.colors.bgSecondary;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPointF center = frame.bounds.center();
    const qreal radius = std::max<qreal>(0.0,
        std::min(frame.bounds.width(), frame.bounds.height()) * 0.5 - 5.0);
    const QRectF arcRect(center.x() - radius, center.y() - radius,
                         radius * 2.0, radius * 2.0);
    const qreal lineWidth = frame.pressed ? 5.0 : frame.hovered ? 4.5 : 4.0;
    constexpr qreal startDegrees = 225.0;
    constexpr qreal sweepDegrees = 270.0;

    painter.setPen(QPen(arcTrack, lineWidth, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(arcRect, static_cast<int>(startDegrees * 16.0),
                    static_cast<int>(-sweepDegrees * 16.0));

    painter.setPen(QPen(arcValue, lineWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(arcRect, static_cast<int>(startDegrees * 16.0),
                    static_cast<int>(-sweepDegrees * position * 16.0));

    const qreal faceRadius = std::max<qreal>(0.0, radius - 7.0);
    QRadialGradient gradient(center, faceRadius);
    gradient.setColorAt(0.0, face.lighter(125));
    gradient.setColorAt(0.72, face);
    gradient.setColorAt(1.0, face.darker(125));
    painter.setPen(QPen(style.colors.borderPrimary.darker(135), 1.0));
    painter.setBrush(gradient);
    painter.drawEllipse(center, faceRadius, faceRadius);

    constexpr qreal pi = 3.14159265358979323846;
    const qreal angle = (startDegrees - sweepDegrees * position) * pi / 180.0;
    const qreal innerRadius = std::max<qreal>(0.0, radius - 17.0);
    const qreal outerRadius = std::max<qreal>(0.0, radius - 9.0);
    const QPointF inner(center.x() + innerRadius * std::cos(angle),
                        center.y() - innerRadius * std::sin(angle));
    const QPointF outer(center.x() + outerRadius * std::cos(angle),
                        center.y() - outerRadius * std::sin(angle));
    painter.setPen(QPen(indicator, frame.pressed ? 3.0 : 2.5,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(inner, outer);
    painter.restore();
}

}
