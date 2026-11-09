#include "TransitionFadePreviewField.h"

#include "Core/CurveSemantics.h"
#include "Core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

constexpr double kDefaultHalfSpan = 1.0;

double halfSpan(const TransitionPreview& preview)
{
    double extent = 0.0;
    for (const TransitionFadeDef* fade : {&preview.fadeOut, &preview.fadeIn}) {
        if (fade->durationSeconds <= 0.0f)
            continue;
        extent = std::max({extent, std::abs(double(fade->offsetSeconds)),
                           std::abs(double(fade->offsetSeconds) + fade->durationSeconds)});
    }
    return extent > 0.0 ? extent : kDefaultHalfSpan;
}

double fadeGain(const TransitionFadeDef& fade, bool out, double seconds)
{
    if (fade.durationSeconds <= 0.0f)
        return 1.0;
    const double progress = std::clamp((seconds - fade.offsetSeconds) / fade.durationSeconds, 0.0, 1.0);
    const double value = curveBezierValueAt(fade.curve, fade.handles, float(progress));
    return out ? 1.0 - value : value;
}

}

TransitionFadePreviewField::TransitionFadePreviewField(QWidget* parent)
    : PreviewField(parent)
{
    setObjectName(QStringLiteral("transitionFadePreview"));
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Open Transition Fade Editor"));
    setDimmed(true);
}

void TransitionFadePreviewField::setPreview(const TransitionPreview& preview, bool active)
{
    preview_ = preview;
    active_ = active;
    setDimmed(!active_);
    update();
}

void TransitionFadePreviewField::paintPreview(QPainter& painter, const QRectF& bounds)
{
    painter.setRenderHint(QPainter::Antialiasing);
    const auto theme = ThemeManager::instance().snapshot();
    const auto& colors = theme->ui.colors;
    const auto& timeline = theme->domains.timeline;
    const double half = halfSpan(preview_);
    const auto xAt = [&](double seconds) {
        return bounds.center().x() + seconds / half * bounds.width() / 2.0;
    };
    const auto band = [&](double from, double to, const QRectF& lane, QColor color) {
        const double left = std::clamp(xAt(from), bounds.left(), bounds.right());
        const double right = std::clamp(xAt(to), bounds.left(), bounds.right());
        if (right <= left)
            return;
        color.setAlphaF(active_ ? 0.28 : 0.14);
        painter.fillRect(QRectF(left, lane.top(), right - left, lane.height()), color);
    };
    const QRectF upper(bounds.left(), bounds.top(), bounds.width(), bounds.height() / 2.0);
    const QRectF lower(bounds.left(), bounds.center().y(), bounds.width(), bounds.height() / 2.0);
    painter.save();
    painter.setClipRect(bounds);
    band(0.0, preview_.source.playedEnd, upper, timeline.exit);
    band(preview_.target.playedStart, 0.0, lower, timeline.entry);

    const qreal width = theme->domains.curve.metrics.curveLineWidth;
    painter.setPen(QPen(timeline.transition, 1.0));
    painter.drawLine(QPointF(xAt(0.0), bounds.top()), QPointF(xAt(0.0), bounds.bottom()));

    const int samples = std::max(2, int(std::ceil(bounds.width())));
    const auto curve = [&](bool out) {
        const TransitionPreviewClip& clip = out ? preview_.source : preview_.target;
        const TransitionFadeDef& fade = out ? preview_.fadeOut : preview_.fadeIn;
        QPainterPath path;
        for (int step = 0; step <= samples; ++step) {
            const double seconds = -half + 2.0 * half * step / samples;
            const bool playing = seconds >= clip.playedStart && seconds <= clip.playedEnd;
            const double gain = playing ? fadeGain(fade, out, seconds) : 0.0;
            const QPointF point(xAt(seconds), bounds.bottom() - gain * bounds.height());
            if (step == 0) path.moveTo(point);
            else path.lineTo(point);
        }
        return path;
    };
    painter.setPen(QPen(active_ ? colors.accent : colors.textSecondary, width));
    painter.drawPath(curve(true));
    painter.drawPath(curve(false));
    painter.restore();
}

}
