#include "TransitionFadeTimeline.h"

#include "Core/CurveSemantics.h"
#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Widgets/Common/TimeAxis.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/CurveEditor/CurveRenderer.h"
#include "Widgets/Timeline/TimelineRenderer.h"

#include <QContextMenuEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

constexpr double kMinimumSpan = 0.05;
constexpr double kMaximumSpan = 600.0;
constexpr double kSnapPixels = 6.0;
constexpr int kOut = 0;
constexpr int kIn = 1;

QString secondsText(double seconds)
{
    return QStringLiteral("%1 s").arg(formatDecimal(seconds, 3));
}

}

TransitionFadeTimeline::TransitionFadeTimeline(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("transitionFadeTimeline"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, [this] { updateGeometry(); update(); });
    connect(&WaveformCache::instance(), &WaveformCache::cacheReady, this, [this](const QString& path) {
        if (waveforms_.remove(path) > 0)
            update();
    });
}

void TransitionFadeTimeline::setPreview(const TransitionPreview& preview)
{
    if (active_.side >= 0)
        return;
    preview_ = preview;
    original_ = preview;
    update();
    emit viewChanged();
}

void TransitionFadeTimeline::fitView()
{
    double extent = std::max({1.0, preview_.sourceTailSeconds(), preview_.targetLeadInSeconds()});
    for (int side : {kOut, kIn}) {
        const TransitionFadeDef& definition = fade(side);
        extent = std::max({extent, std::abs(double(definition.offsetSeconds)),
                           std::abs(double(definition.offsetSeconds + definition.durationSeconds))});
    }
    extent *= 1.25;
    viewStart_ = -extent;
    viewSpan_ = extent * 2.0;
    update();
    emit viewChanged();
}

double TransitionFadeTimeline::contentStart() const
{
    double start = std::min({preview_.source.start, preview_.target.start, viewStart_});
    for (int side : {kOut, kIn})
        start = std::min(start, double(fade(side).offsetSeconds));
    return start;
}

double TransitionFadeTimeline::contentEnd() const
{
    double end = std::max({preview_.source.end, preview_.target.end, viewStart_ + viewSpan_});
    for (int side : {kOut, kIn})
        end = std::max(end, double(fade(side).offsetSeconds + fade(side).durationSeconds));
    return end;
}

void TransitionFadeTimeline::setViewStart(double seconds)
{
    viewStart_ = seconds;
    update();
    emit viewChanged();
}

void TransitionFadeTimeline::zoomBy(double factor)
{
    const double pivot = viewStart_ + viewSpan_ / 2.0;
    const double span = std::clamp(viewSpan_ / factor, kMinimumSpan, kMaximumSpan);
    viewStart_ = pivot - span / 2.0;
    viewSpan_ = span;
    update();
    emit viewChanged();
}

const WaveformPeakData* TransitionFadeTimeline::waveform(const QString& path) const
{
    if (path.isEmpty())
        return nullptr;
    auto it = waveforms_.constFind(path);
    if (it == waveforms_.constEnd()) {
        WaveformPeakData peaks;
        if (!WaveformCache::load(path, peaks))
            peaks = {};
        it = waveforms_.insert(path, std::move(peaks));
    }
    return it->isValid() ? &*it : nullptr;
}

QSize TransitionFadeTimeline::sizeHint() const
{
    const auto& metrics = ThemeManager::instance().metrics();
    return {metrics.timeline.laneHeight * 8, metrics.timeline.rulerHeight + metrics.timeline.laneHeight * 4};
}

QSize TransitionFadeTimeline::minimumSizeHint() const
{
    const auto& metrics = ThemeManager::instance().metrics();
    return {metrics.timeline.laneHeight * 4, metrics.timeline.rulerHeight + metrics.timeline.laneHeight * 2};
}

TransitionFadeDef& TransitionFadeTimeline::fade(int side)
{
    return side == kOut ? preview_.fadeOut : preview_.fadeIn;
}

const TransitionFadeDef& TransitionFadeTimeline::fade(int side) const
{
    return side == kOut ? preview_.fadeOut : preview_.fadeIn;
}

QRect TransitionFadeTimeline::rulerRect() const
{
    return {0, 0, width(), ThemeManager::instance().metrics().timeline.rulerHeight};
}

QRect TransitionFadeTimeline::plotRect() const
{
    const QRect ruler = rulerRect();
    return {0, ruler.bottom() + 1, width(), std::max(0, height() - ruler.height())};
}

QRect TransitionFadeTimeline::laneRect(int side) const
{
    const QRect plot = plotRect();
    const int half = plot.height() / 2;
    return side == kOut ? QRect(plot.left(), plot.top(), plot.width(), half)
                        : QRect(plot.left(), plot.top() + half, plot.width(), plot.height() - half);
}

QRectF TransitionFadeTimeline::fadeRect(int side) const
{
    const auto& metrics = ThemeManager::instance().metrics();
    const QRect lane = laneRect(side);
    const int title = TimelineRenderer::clipTitleHeight(metrics);
    return QRectF(lane.left(), lane.top() + metrics.timeline.clipBottomInset + title,
                  lane.width(), std::max(1, lane.height() - metrics.timeline.clipBottomInset * 2 - title));
}

double TransitionFadeTimeline::xAt(double seconds) const
{
    const QRect plot = plotRect();
    return plot.left() + (seconds - viewStart_) / viewSpan_ * plot.width();
}

double TransitionFadeTimeline::secondsAt(double x) const
{
    const QRect plot = plotRect();
    return viewStart_ + (x - plot.left()) / std::max(1, plot.width()) * viewSpan_;
}

QPointF TransitionFadeTimeline::fadePoint(int side, float x, float progress) const
{
    const TransitionFadeDef& definition = fade(side);
    const QRectF area = fadeRect(side);
    const float gain = side == kOut ? 1.0f - progress : progress;
    return {xAt(definition.offsetSeconds + double(x) * definition.durationSeconds),
            area.bottom() - double(gain) * area.height()};
}

QPointF TransitionFadeTimeline::targetPoint(Hit target) const
{
    const TransitionFadeDef& definition = fade(target.side);
    if (target.part == Part::Handle1 || target.part == Part::Handle2) {
        const CurveHandlePairDef& handles = definition.handles.front();
        return target.part == Part::Handle1 ? fadePoint(target.side, handles.h1x, handles.h1y)
                                            : fadePoint(target.side, handles.h2x, handles.h2y);
    }
    const CurvePointDef& point = target.part == Part::Start ? definition.curve.front()
                                                            : definition.curve.back();
    return fadePoint(target.side, point.x, point.value);
}

bool TransitionFadeTimeline::singleSegment(int side) const
{
    return fade(side).curve.size() == 2 && fade(side).handles.size() == 1;
}

QPainterPath TransitionFadeTimeline::fadePath(int side) const
{
    QPainterPath path(targetPoint({side, Part::Start}));
    if (singleSegment(side)) {
        path.cubicTo(targetPoint({side, Part::Handle1}), targetPoint({side, Part::Handle2}),
                     targetPoint({side, Part::End}));
        return path;
    }
    const TransitionFadeDef& definition = fade(side);
    constexpr int kSamples = 64;
    for (int step = 1; step <= kSamples; ++step) {
        const float x = float(step) / kSamples;
        path.lineTo(fadePoint(side, x, curveBezierValueAt(definition.curve, definition.handles, x)));
    }
    return path;
}

TransitionFadeTimeline::Hit TransitionFadeTimeline::hitTest(const QPoint& position) const
{
    const int first = selected_ >= 0 ? selected_ : kOut;
    const auto near = [&](Hit target, qreal radius) {
        return QLineF(position, targetPoint(target)).length() <= radius;
    };
    for (int side : {first, 1 - first}) {
        if (!laneRect(side).contains(position))
            continue;
        for (Part part : {Part::End, Part::Start})
            if (near({side, part}, 9.0))
                return {side, part};
        if (fade(side).durationSeconds > 0.0f && singleSegment(side)
            && (selected_ == side || hovered_.side == side))
            for (Part part : {Part::Handle1, Part::Handle2})
                if (near({side, part}, 8.0))
                    return {side, part};
        QPainterPathStroker stroke;
        stroke.setWidth(10.0);
        if (fade(side).durationSeconds > 0.0f && stroke.createStroke(fadePath(side)).contains(position))
            return {side, Part::Curve};
    }
    return {};
}

double TransitionFadeTimeline::snapped(double seconds) const
{
    for (double candidate : {0.0, preview_.source.end, preview_.target.start})
        if (std::abs(xAt(seconds) - xAt(candidate)) <= kSnapPixels)
            return candidate;
    return seconds;
}

void TransitionFadeTimeline::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const auto theme = ThemeManager::instance().snapshot();
    const auto& colors = theme->ui.colors;
    const auto& metrics = theme->ui.metrics;
    const QRect plot = plotRect();
    const QFontMetrics captionMetrics(FontManager::instance().getFont(FontRole::Caption), this);
    const TimeAxisTicks ticks = buildTimeAxisTicks(viewStart_, viewSpan_, plot.width(), 0.001, 3, captionMetrics);

    painter.fillRect(rulerRect(), colors.bgSecondary);
    drawTimeAxisRuler(painter, colors, metrics.timeAxis, rulerRect(), ticks);

    TimelineStyleView style;
    style.canvasBackground = colors.bgPrimary;
    style.canvasBorder = colors.borderPrimary;
    style.placeholderText = colors.textSecondary;
    style.axisText = colors.textSecondary;
    style.axisTick = colors.borderTick;
    style.trackBackground = colors.trackBackground;
    style.trackBackgroundAlt = colors.trackBackgroundAlt;
    style.timeline = theme->domains.timeline;
    style.music = theme->domains.music;
    style.waveform = theme->domains.waveform;
    style.curve = theme->domains.curve;
    style.metrics = metrics;
    style.accent = colors.accent;
    style.textPrimary = colors.textPrimary;

    TimelineRenderData data;
    data.viewport = plot;
    data.timeAxis = ticks;
    const int titleHeight = TimelineRenderer::clipTitleHeight(metrics);
    const TransitionPreviewClip* clips[] = {&preview_.source, &preview_.target};
    for (int side : {kOut, kIn}) {
        const TransitionPreviewClip& source = *clips[side];
        TimelineLaneRect lane;
        lane.rect = laneRect(side);
        lane.alternate = side == kIn;
        data.laneRects.append(lane);
        const double left = xAt(source.start);
        const double right = xAt(source.end);
        TimelineClipItem clip;
        clip.rect = QRect(int(std::floor(left)), lane.rect.top() + metrics.timeline.clipBottomInset,
                          std::max(metrics.timeline.clipMinimumWidth, int(std::ceil(right - left))),
                          lane.rect.height() - metrics.timeline.clipBottomInset * 2);
        clip.label = source.name;
        clip.typeIcon = source.icon;
        clip.objectColor = source.color;
        clip.state = selected_ == side ? TimelineClipVisualState::Selected : TimelineClipVisualState::Normal;
        clip.laneIndex = side;
        if (source.waveformView && right > left && clip.rect.height() > titleHeight) {
            const double visibleLeft = std::clamp(left, double(plot.left()), double(plot.right()));
            const double visibleRight = std::clamp(right, double(plot.left()), double(plot.right()));
            if (visibleRight > visibleLeft) {
                const double frames = double(source.waveformView->endFrame() - source.waveformView->startFrame());
                clip.waveform = waveform(source.waveformPath);
                clip.waveformPath = source.waveformPath;
                clip.waveformView = source.waveformView;
                clip.waveformViewport = clip.rect.adjusted(0, titleHeight, 0, 0);
                clip.waveformRect = clip.waveformViewport;
                clip.waveformRect.setLeft(int(std::lround(visibleLeft)));
                clip.waveformRect.setWidth(int(std::lround(visibleRight)) - int(std::lround(visibleLeft)));
                clip.waveformStartFrame = double(source.waveformView->startFrame())
                    + frames * (visibleLeft - left) / (right - left);
                clip.waveformFrameCount = frames * (visibleRight - visibleLeft) / (right - left);
            }
        }
        data.clipItems.append(clip);
    }
    TimelineRenderer::render(painter, style, data);

    painter.save();
    painter.setClipRect(plot);
    const auto veil = [&](int side, double start, double end) {
        const QRect clip = data.clipItems[side].rect;
        if (end <= start)
            return;
        const QRect region(int(std::floor(xAt(start))), clip.top(),
                           int(std::ceil(xAt(end) - xAt(start))), clip.height());
        painter.fillRect(region.intersected(clip), style.timeline.outOfSpanVeil);
    };
    veil(kOut, preview_.source.start, preview_.source.playedStart);
    veil(kOut, preview_.source.playedEnd, preview_.source.end);
    veil(kIn, preview_.target.start, preview_.target.playedStart);
    veil(kIn, preview_.target.playedEnd, preview_.target.end);
    painter.restore();

    TimelineRenderer::drawVerticalLine(painter, style.timeline.transition,
                                       int(std::lround(xAt(0.0))), plot);

    painter.save();
    painter.setClipRect(plot);
    for (int side : {kOut, kIn}) {
        CurveRenderData curve;
        curve.frameVisible = false;
        curve.clipToPlot = true;
        curve.graphRect = fadeRect(side);
        curve.curveColor = style.curve.colors.gridFade;
        const bool hasDuration = fade(side).durationSeconds > 0.0f;
        if (hasDuration)
            curve.continuousPath = fadePath(side);
        for (Part part : {Part::Start, Part::End})
            curve.points.append({targetPoint({side, part}), selected_ == side, hovered_ == Hit{side, part}, {}});
        if (hasDuration && singleSegment(side) && (selected_ == side || hovered_.side == side)) {
            curve.handles.append({targetPoint({side, Part::Start}), targetPoint({side, Part::Handle1}),
                                  active_ == Hit{side, Part::Handle1}});
            curve.handles.append({targetPoint({side, Part::End}), targetPoint({side, Part::Handle2}),
                                  active_ == Hit{side, Part::Handle2}});
        }
        CurveRenderer::renderForeground(painter, style.curve, CurveColorRole::GridFade, curve);
    }
    painter.restore();
}

void TransitionFadeTimeline::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panOrigin_ = secondsAt(event->position().x());
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const Hit hit = hitTest(event->pos());
    selected_ = hit.side >= 0 ? hit.side
        : laneRect(kOut).contains(event->pos()) ? kOut
        : laneRect(kIn).contains(event->pos()) ? kIn : -1;
    active_ = hit;
    original_ = preview_;
    dragOrigin_ = event->pos();
    moving_ = hit.part == Part::Curve && event->modifiers().testFlag(Qt::ShiftModifier);
    grabOffset_ = hit.part == Part::Start || hit.part == Part::End
        ? targetPoint(hit).x() - event->position().x() : 0.0;
    if (hit.part == Part::Curve && !moving_) {
        const QPainterPath path = fadePath(hit.side);
        double low = 0.0;
        double high = 1.0;
        for (int step = 0; step < 32; ++step) {
            const double middle = (low + high) * 0.5;
            (path.pointAtPercent(middle).x() < event->position().x() ? low : high) = middle;
        }
        dragT_ = std::clamp((low + high) * 0.5, 0.05, 0.95);
    }
    update();
}

void TransitionFadeTimeline::dragTo(const QPoint& position, Qt::KeyboardModifiers modifiers)
{
    const int side = active_.side;
    TransitionFadeDef& definition = fade(side);
    const TransitionFadeDef& initial = side == kOut ? original_.fadeOut : original_.fadeIn;
    const double start = initial.offsetSeconds;
    const double end = start + initial.durationSeconds;
    switch (active_.part) {
    case Part::Start: {
        const double next = std::min(snapped(secondsAt(position.x() + grabOffset_)), end);
        definition.offsetSeconds = float(next);
        definition.durationSeconds = float(end - next);
        break;
    }
    case Part::End: {
        const double next = std::max(snapped(secondsAt(position.x() + grabOffset_)), start);
        definition.durationSeconds = float(next - start);
        break;
    }
    case Part::Curve: {
        if (moving_) {
            const double shift = secondsAt(position.x()) - secondsAt(dragOrigin_.x());
            const double next = snapped(start + shift);
            definition.offsetSeconds = float(next != start + shift
                ? next : snapped(end + shift) - initial.durationSeconds);
            break;
        }
        const QPointF left = fadePoint(side, 0.0f, 0.0f);
        const QPointF right = fadePoint(side, 1.0f, 1.0f);
        if (!singleSegment(side) || std::abs(right.x() - left.x()) < 1.0
            || std::abs(right.y() - left.y()) < 1.0)
            return;
        const CurveHandlePairDef& handles = initial.handles.front();
        const QPointF delta((position.x() - dragOrigin_.x()) / (right.x() - left.x()),
                            (position.y() - dragOrigin_.y()) / (right.y() - left.y()));
        QPointF first;
        QPointF second;
        curveDistributeBodyDelta(dragT_, delta, &first, &second);
        definition.handles.front() = {
            float(std::clamp(handles.h1x + first.x(), 0.0, 1.0)),
            float(std::clamp(handles.h1y + first.y(), 0.0, 1.0)),
            float(std::clamp(handles.h2x + second.x(), 0.0, 1.0)),
            float(std::clamp(handles.h2y + second.y(), 0.0, 1.0))};
        break;
    }
    case Part::Handle1:
    case Part::Handle2: {
        const QPointF left = fadePoint(side, 0.0f, 0.0f);
        const QPointF right = fadePoint(side, 1.0f, 1.0f);
        if (std::abs(right.x() - left.x()) < 1.0 || std::abs(right.y() - left.y()) < 1.0)
            return;
        QPointF target = position;
        if (modifiers.testFlag(Qt::ShiftModifier))
            target = curveSnapHandleToAngle(
                targetPoint({side, active_.part == Part::Handle1 ? Part::Start : Part::End}),
                target, QRectF(left, right).normalized());
        const float x = float(std::clamp((target.x() - left.x()) / (right.x() - left.x()), 0.0, 1.0));
        const float y = float(std::clamp((target.y() - left.y()) / (right.y() - left.y()), 0.0, 1.0));
        CurveHandlePairDef& handles = definition.handles.front();
        if (active_.part == Part::Handle1) {
            handles.h1x = x;
            handles.h1y = y;
        } else {
            handles.h2x = x;
            handles.h2y = y;
        }
        break;
    }
    case Part::None:
        return;
    }
    setToolTip(tooltipFor(active_));
    update();
    emit fadesPreviewed(preview_.fadeOut, preview_.fadeIn);
}

void TransitionFadeTimeline::mouseMoveEvent(QMouseEvent* event)
{
    if (panning_) {
        viewStart_ += panOrigin_ - secondsAt(event->position().x());
        update();
        emit viewChanged();
        return;
    }
    if (active_.side >= 0) {
        dragTo(event->pos(), event->modifiers());
        return;
    }
    const Hit hit = hitTest(event->pos());
    if (!(hit == hovered_)) {
        hovered_ = hit;
        setToolTip(tooltipFor(hit));
        setCursor(hit.part == Part::Start || hit.part == Part::End ? Qt::SizeHorCursor
                  : hit.part == Part::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
    }
}

void TransitionFadeTimeline::finishDrag(bool commit)
{
    const bool changed = !(preview_.fadeOut == original_.fadeOut) || !(preview_.fadeIn == original_.fadeIn);
    active_ = {};
    moving_ = false;
    if (changed && commit)
        emit fadesCommitted(preview_.fadeOut, preview_.fadeIn);
    original_ = preview_;
    update();
}

void TransitionFadeTimeline::mouseReleaseEvent(QMouseEvent* event)
{
    if (panning_ && event->button() == Qt::MiddleButton) {
        panning_ = false;
        unsetCursor();
        return;
    }
    if (event->button() == Qt::LeftButton && active_.side >= 0)
        finishDrag(true);
    else
        QWidget::mouseReleaseEvent(event);
}

void TransitionFadeTimeline::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && hitTest(event->pos()).side < 0)
        fitView();
    else
        QWidget::mouseDoubleClickEvent(event);
}

void TransitionFadeTimeline::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        event->ignore();
        return;
    }
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        viewStart_ -= steps * viewSpan_ * 0.1;
    } else {
        const double pivot = secondsAt(event->position().x());
        const double span = std::clamp(viewSpan_ * std::pow(0.8, steps), kMinimumSpan, kMaximumSpan);
        viewStart_ = pivot - (pivot - viewStart_) * span / viewSpan_;
        viewSpan_ = span;
    }
    update();
    emit viewChanged();
    event->accept();
}

void TransitionFadeTimeline::contextMenuEvent(QContextMenuEvent* event)
{
    const Hit hit = hitTest(event->pos());
    const int side = hit.side >= 0 ? hit.side
        : laneRect(kOut).contains(event->pos()) ? kOut
        : laneRect(kIn).contains(event->pos()) ? kIn : -1;
    if (side < 0)
        return;
    selected_ = side;
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("transitionFadeContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSection(side == kOut ? tr("Fade Out") : tr("Fade In"));
    auto* linear = menu->addAction(tr("Reset to Linear"), this, [this, side] { resetCurve(side); });
    linear->setObjectName(QStringLiteral("transitionFadeResetCurve"));
    linear->setEnabled(fade(side).durationSeconds > 0.0f);
    auto* remove = menu->addAction(tr("Remove Fade"), this, [this, side] { resetFade(side); });
    remove->setObjectName(QStringLiteral("transitionFadeRemove"));
    remove->setEnabled(!(fade(side) == canonicalTransitionFade()));
    menu->addSeparator();
    menu->addAction(tr("Fit View"), this, [this] { fitView(); });
    menu->popup(event->globalPos());
    update();
}

void TransitionFadeTimeline::leaveEvent(QEvent* event)
{
    if (hovered_.side >= 0 && active_.side < 0) {
        hovered_ = {};
        update();
    }
    QWidget::leaveEvent(event);
}

void TransitionFadeTimeline::resetFade(int side)
{
    original_ = preview_;
    fade(side) = canonicalTransitionFade();
    finishDrag(true);
}

void TransitionFadeTimeline::resetCurve(int side)
{
    original_ = preview_;
    const TransitionFadeDef canonical = canonicalTransitionFade();
    fade(side).curve = canonical.curve;
    fade(side).handles = {{1.0f / 3.0f, 1.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f}};
    finishDrag(true);
}

QString TransitionFadeTimeline::tooltipFor(Hit target) const
{
    if (target.side < 0)
        return tr("Wheel to zoom, Shift+wheel or middle-drag to pan. Double-click to fit.");
    const TransitionFadeDef& definition = fade(target.side);
    const QString summary = tr("%1\nStart: %2\nDuration: %3")
        .arg(target.side == kOut ? tr("Fade Out") : tr("Fade In"),
             secondsText(definition.offsetSeconds), secondsText(definition.durationSeconds));
    switch (target.part) {
    case Part::Start:
        return summary + tr("\nDrag to move the fade start.");
    case Part::End:
        return summary + tr("\nDrag to adjust the duration.");
    case Part::Curve:
        return summary + tr("\nDrag to shape the curve. Shift-drag to move the fade.");
    case Part::Handle1:
    case Part::Handle2:
        return summary + tr("\nDrag to shape the Bezier curve. Shift snaps to 45-degree angles.");
    case Part::None:
        break;
    }
    return summary;
}

}
