#pragma once

#include <QEasingCurve>

namespace VNX::LocationPingAnimation {

inline constexpr int kDurationMs = 620;

inline qreal popProgress(qreal progress)
{
    return QEasingCurve(QEasingCurve::OutCubic).valueForProgress(qMin(progress / 0.22, 1.0));
}

inline qreal fadeOpacity(qreal progress)
{
    return progress < 0.62 ? 1.0 : 1.0 - (progress - 0.62) / (1.0 - 0.62);
}

}
