#pragma once

#include "Core/ThemeManager.h"

#include <QPainter>

namespace VNX::SwitchIndicator {

inline int width() { return ThemeManager::instance().metrics().toggle.compactWidth; }
inline int height() { return ThemeManager::instance().metrics().toggle.compactHeight; }

inline void draw(QPainter* painter, const QRect& rect, Qt::CheckState state, bool hover,
                 const QColor& highlightedText = QColor())
{
    const auto& colors = ThemeManager::instance().colors();
    const auto& metrics = ThemeManager::instance().metrics();
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
    const qreal inset = metrics.borderWidth / 2.0;
    const QRectF track = QRectF(rect).adjusted(inset, inset, -inset, -inset);
    painter->setPen(QPen(border, metrics.borderWidth));
    painter->setBrush(fill);
    painter->drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    painter->setPen(Qt::NoPen);
    if (partial) {
        painter->setBrush(activeColor);
        const qreal length = track.width() / 3;
        const qreal thickness = metrics.accentBorderWidth;
        painter->drawRoundedRect(QRectF(track.center().x() - length / 2,
                                       track.center().y() - thickness / 2, length, thickness),
                                 thickness / 2, thickness / 2);
    } else {
        const qreal padding = metrics.toggle.compactThumbPadding;
        const qreal diameter = rect.height() - padding * 2;
        const qreal x = on ? rect.left() + rect.width() - padding - diameter
                           : rect.left() + padding;
        painter->setBrush(on ? activeColor : inactiveColor);
        painter->drawEllipse(QRectF(x, rect.top() + padding, diameter, diameter));
    }
    painter->restore();
}

}
