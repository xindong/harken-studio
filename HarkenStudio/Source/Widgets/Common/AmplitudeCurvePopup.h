#pragma once

#include <functional>

class QAction;
class QMenu;
class QPoint;
class QRect;
class QWidget;

namespace harken::studio {

QAction* addAmplitudePowerLawAction(QMenu* menu, QWidget* owner, const QPoint& globalPosition,
    const QRect& globalAxisBounds, double exponent, std::function<void(double)> changed);

void showAmplitudeCurveMenu(QWidget* owner, const QPoint& globalPosition,
    const QRect& globalAxisBounds, double exponent, std::function<void(double)> changed);

}
