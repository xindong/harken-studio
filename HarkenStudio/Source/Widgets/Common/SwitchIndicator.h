#pragma once

#include "Core/ThemeManager.h"

#include <QPainter>

namespace VNX::SwitchIndicator {

inline constexpr int Width = 24;
inline constexpr int Height = 13;

inline void draw(QPainter* painter, const QRect& rect, Qt::CheckState state, bool hover,
                 const QColor& highlightedText = QColor())
{
    const auto& colors = ThemeManager::instance().colors();
    const bool on = state == Qt::Checked;
    const bool partial = state == Qt::PartiallyChecked;
    const QColor activeColor = highlightedText.isValid() ? highlightedText : colors.accent;
    const QColor inactiveColor = highlightedText.isValid() ? highlightedText : colors.textMuted;
    QColor fill = highlightedText.isValid() ? QColor(Qt::transparent) : colors.bgPrimary;
    if (on) {
        fill = activeColor;
        fill.setAlphaF(fill.alphaF() * 26 / 255.0);
    }
    QColor border = (on || hover) ? activeColor : inactiveColor;
    if (partial && !hover && !highlightedText.isValid())
        border = colors.accent.darker(150);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF track = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter->setPen(QPen(border, 1.0));
    painter->setBrush(fill);
    painter->drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    painter->setPen(Qt::NoPen);
    if (partial) {
        painter->setBrush(activeColor);
        painter->drawRoundedRect(QRectF(track.center().x() - 4.0,
                                       track.center().y() - 1.0, 8.0, 2.0), 1.0, 1.0);
    } else {
        const qreal diameter = rect.height() - 6.0;
        const qreal x = on ? rect.left() + rect.width() - 3.0 - diameter
                           : rect.left() + 3.0;
        painter->setBrush(on ? activeColor : inactiveColor);
        painter->drawEllipse(QRectF(x, rect.top() + 3.0, diameter, diameter));
    }
    painter->restore();
}

}
