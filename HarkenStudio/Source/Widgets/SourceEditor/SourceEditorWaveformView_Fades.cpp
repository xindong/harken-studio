#include "SourceEditorWaveformView.h"

#include "Core/ThemeManager.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/CurveEditor/CurveRenderer.h"

#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QPainter>
#include <QPainterPathStroker>
#include "Core/ToolTip.h"

#include <algorithm>
#include <cmath>

namespace VNX {

const SoundSourceFadeDef& SourceEditorWaveformView::fade(int index) const
{
    return index == 0 ? previewSource_.fadeIn : previewSource_.fadeOut;
}

QRectF SourceEditorWaveformView::fadeGraphRect() const
{
    QRectF graph = channelViewportRect();
    graph.setLeft(waveformRect().left());
    return graph.adjusted(0.0, 0.0, -1.0, -1.0);
}

QPointF SourceEditorWaveformView::fadePointPosition(int index, float x, float value) const
{
    const auto& definition = fade(index);
    const double start = index == 0 ? double(previewSource_.trimStartFrame)
        : double(previewSource_.trimEndFrame.value_or(totalFrames())) - double(definition.durationFrames);
    const double frame = start + double(x) * double(definition.durationFrames);
    const QRectF graph = fadeGraphRect();
    return {graph.left() + (frame - viewStart_) / std::max(1.0, viewFrameCount()) * graph.width(),
            graph.bottom() - double(value) * graph.height()};
}

QPointF SourceEditorWaveformView::fadeTargetPosition(FadeHit target) const
{
    const auto& definition = fade(target.index);
    if (target.part == FadePart::Handle1 || target.part == FadePart::Handle2) {
        const auto& handles = definition.handles.front();
        return target.part == FadePart::Handle1
            ? fadePointPosition(target.index, handles.h1x, handles.h1y)
            : fadePointPosition(target.index, handles.h2x, handles.h2y);
    }
    const auto& point = target.part == FadePart::Start
        ? definition.curve.front() : definition.curve.back();
    return fadePointPosition(target.index, point.x, point.value);
}

QPainterPath SourceEditorWaveformView::fadePath(int index) const
{
    QPainterPath path(fadeTargetPosition({index, FadePart::Start}));
    path.cubicTo(fadeTargetPosition({index, FadePart::Handle1}),
        fadeTargetPosition({index, FadePart::Handle2}),
        fadeTargetPosition({index, FadePart::End}));
    return path;
}

SourceEditorWaveformView::FadeHit SourceEditorWaveformView::hitTestFade(const QPoint& position) const
{
    if (!editable_ || presentation_ != Presentation::TrimEditor || totalFrames() == 0
        || !waveformRect().contains(position)
        || position.y() < channelViewportRect().top())
        return {};

    const int first = selectedFade_ >= 0 ? selectedFade_ : 0;
    const QRectF graph = fadeGraphRect();
    const auto hit = [&](FadeHit target, qreal radius) {
        const QPointF point = fadeTargetPosition(target);
        return point.x() >= graph.left() && point.x() <= graph.right()
            && QLineF(position, point).length() <= radius;
    };
    for (const int index : {first, 1 - first}) {
        for (const FadePart part : {FadePart::Start, FadePart::End}) {
            const FadeHit target{index, part};
            if (hit(target, 9.0))
                return target;
        }
    }
    for (const int index : {first, 1 - first}) {
        if (fade(index).durationFrames == 0
            || (selectedFade_ != index && hoveredFade_.index != index))
            continue;
        for (const FadePart part : {FadePart::Handle1, FadePart::Handle2}) {
            const FadeHit target{index, part};
            if (hit(target, 8.0))
                return target;
        }
    }
    QPainterPathStroker stroke;
    stroke.setWidth(10.0);
    for (const int index : {first, 1 - first}) {
        if (fade(index).durationFrames > 0 && stroke.createStroke(fadePath(index)).contains(position))
            return {index, FadePart::Curve};
    }
    return {};
}

QString SourceEditorWaveformView::fadeTooltip(FadeHit target) const
{
    if (target.index < 0)
        return {};
    const QString name = target.index == 0 ? tr("Fade In") : tr("Fade Out");
    const QString duration = tr("%1: %2").arg(name, formatFrame(fade(target.index).durationFrames));
    if (target.part == FadePart::Handle1 || target.part == FadePart::Handle2)
        return duration + tr("\nDrag to shape the Bezier curve. Shift snaps to 45-degree angles.");
    if (target.part == FadePart::Curve)
        return duration + tr("\nDrag the curve to adjust its shape.");
    const bool top = target.index == 0 ? target.part == FadePart::End : target.part == FadePart::Start;
    return duration + (top ? tr("\nDrag horizontally to adjust duration. Double-click to enter a value.")
                          : tr("\nFixed at the trim edge. Select to edit the Bezier handles."))
        + tr("\nAlt-drag an anchor to pull its Bezier handle.");
}

uint64_t SourceEditorWaveformView::fadeDurationLimit(int index) const
{
    const uint64_t start = std::min(previewSource_.trimStartFrame, totalFrames());
    const uint64_t end = std::min(previewSource_.trimEndFrame.value_or(totalFrames()), totalFrames());
    const uint64_t span = end > start ? end - start : 0;
    return span - std::min(span, fade(1 - index).durationFrames);
}

void SourceEditorWaveformView::paintFades(QPainter& painter)
{
    if (totalFrames() == 0)
        return;
    const auto theme = ThemeManager::instance().snapshot();
    const auto& style = theme->domains.curve;
    const QRectF graph = fadeGraphRect();
    painter.save();
    painter.setClipRect(graph);
    for (int index = 0; index < 2; ++index) {
        const bool selected = selectedFade_ == index;
        const bool hovered = hoveredFade_.index == index;
        CurveRenderData data;
        data.frameVisible = false;
        data.clipToPlot = true;
        data.graphRect = graph;
        data.curveColor = style.colors.gridFade;
        if (fade(index).durationFrames > 0)
            data.continuousPath = fadePath(index);
        if (editable_) {
            for (const FadePart part : {FadePart::Start, FadePart::End}) {
                const FadeHit target{index, part};
                const QPointF point = fadeTargetPosition(target);
                if (point.x() >= graph.left() && point.x() <= graph.right())
                    data.points.append({point, selected, hoveredFade_ == target});
            }
            if ((selected || hovered) && fade(index).durationFrames > 0) {
                data.handles.append({fadeTargetPosition({index, FadePart::Start}),
                    fadeTargetPosition({index, FadePart::Handle1}),
                    activeFade_ == FadeHit{index, FadePart::Handle1}});
                data.handles.append({fadeTargetPosition({index, FadePart::End}),
                    fadeTargetPosition({index, FadePart::Handle2}),
                    activeFade_ == FadeHit{index, FadePart::Handle2}});
            }
        }
        CurveRenderer::renderForeground(painter, style, CurveColorRole::GridFade, data);
    }
    painter.restore();
}

void SourceEditorWaveformView::beginFadeDrag(FadeHit target, const QPoint& position, bool pullHandle)
{
    setFocus();
    originalSource_ = previewSource_;
    activeFade_ = target;
    hoveredFade_ = target;
    selectedFade_ = target.index;
    hoveredHandle_ = Handle::None;
    dragPosition_ = position;
    dragMoved_ = false;
    fadeDragOrigin_ = position;
    fadeGrabOffset_ = target.part == FadePart::Curve
        ? QPointF{} : fadeTargetPosition(target) - QPointF(position);
    fadePullingHandle_ = pullHandle;
    if (pullHandle)
        activeFade_.part = target.part == FadePart::Start ? FadePart::Handle1 : FadePart::Handle2;
    setCursor(target.part == FadePart::Curve ? Qt::SizeVerCursor : Qt::PointingHandCursor);
    if (target.part == FadePart::Curve) {
        const QPainterPath path = fadePath(target.index);
        double lo = 0.0;
        double hi = 1.0;
        for (int step = 0; step < 32; ++step) {
            const double mid = (lo + hi) * 0.5;
            if (path.pointAtPercent(mid).x() < position.x())
                lo = mid;
            else
                hi = mid;
        }
        fadeDragT_ = std::clamp((lo + hi) * 0.5, 0.05, 0.95);
    }
    ToolTip::hideText();
    showHandleValue();
    update();
}

void SourceEditorWaveformView::setFadeDuration(int index, uint64_t duration)
{
    auto& definition = index == 0 ? previewSource_.fadeIn : previewSource_.fadeOut;
    const uint64_t next = std::min(duration, fadeDurationLimit(index));
    if (definition.durationFrames == next)
        return;
    definition.durationFrames = next;
    update();
    emit previewChanged();
}

void SourceEditorWaveformView::dragFade(const QPoint& position, Qt::KeyboardModifiers modifiers)
{
    if (activeFade_.index < 0 || (position == dragPosition_ && !dragMoved_))
        return;
    if (fadePullingHandle_) {
        const QPointF delta = QPointF(position) - fadeDragOrigin_;
        if (std::hypot(delta.x(), delta.y()) < 2.0)
            return;
        fadePullingHandle_ = false;
    }
    const int index = activeFade_.index;
    const FadePart part = activeFade_.part;
    const bool top = index == 0 ? part == FadePart::End : part == FadePart::Start;
    if (!top && (part == FadePart::Start || part == FadePart::End))
        return;
    dragMoved_ = true;
    dragPosition_ = position;
    if (top) {
        const uint64_t frame = frameAt(position.x() + fadeGrabOffset_.x());
        const uint64_t edge = index == 0 ? previewSource_.trimStartFrame
            : previewSource_.trimEndFrame.value_or(totalFrames());
        const uint64_t duration = index == 0 ? (frame > edge ? frame - edge : 0)
            : (frame < edge ? edge - frame : 0);
        setFadeDuration(index, duration);
        showHandleValue();
        if (position.x() < waveformRect().left() || position.x() > waveformRect().right())
            edgeScroll_.start();
        else
            edgeScroll_.stop();
        return;
    }
    const QPointF left = fadePointPosition(index, 0.0f, 0.0f);
    const QPointF right = fadePointPosition(index, 1.0f, 1.0f);
    if (right.x() <= left.x())
        return;
    auto& definition = index == 0 ? previewSource_.fadeIn : previewSource_.fadeOut;
    auto& handles = definition.handles.front();
    if (part == FadePart::Curve) {
        const auto& original = index == 0 ? originalSource_.fadeIn : originalSource_.fadeOut;
        const auto& initial = original.handles.front();
        const QPointF delta((position.x() - fadeDragOrigin_.x()) / (right.x() - left.x()),
            (position.y() - fadeDragOrigin_.y()) / (right.y() - left.y()));
        QPointF first, second;
        curveDistributeBodyDelta(fadeDragT_, delta, &first, &second);
        handles = {float(std::clamp(initial.h1x + first.x(), 0.0, 1.0)),
                   float(std::clamp(initial.h1y + first.y(), 0.0, 1.0)),
                   float(std::clamp(initial.h2x + second.x(), 0.0, 1.0)),
                   float(std::clamp(initial.h2y + second.y(), 0.0, 1.0))};
    } else {
        QPointF target = QPointF(position) + fadeGrabOffset_;
        if (modifiers.testFlag(Qt::ShiftModifier)) {
            const QPointF anchor = fadeTargetPosition({index,
                part == FadePart::Handle1 ? FadePart::Start : FadePart::End});
            target = curveSnapHandleToAngle(anchor, target, QRectF(left, right).normalized());
        }
        const float x = float(std::clamp((target.x() - left.x()) / (right.x() - left.x()), 0.0, 1.0));
        const float y = float(std::clamp((target.y() - left.y()) / (right.y() - left.y()), 0.0, 1.0));
        if (part == FadePart::Handle1) {
            handles.h1x = x;
            handles.h1y = y;
        } else {
            handles.h2x = x;
            handles.h2y = y;
        }
    }
    update();
    emit previewChanged();
}

void SourceEditorWaveformView::showFadeMenu(int index, const QPoint& position)
{
    cancelGesture();
    selectedFade_ = index;
    ToolTip::hideText();
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("sourceFadeContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSection(index == 0 ? tr("Fade In") : tr("Fade Out"));
    auto* enter = menu->addAction(tr("Enter Duration..."), this,
        [this, index] { editHandleValue(Handle::None, index); });
    enter->setObjectName(QStringLiteral("sourceFadeEnterDuration"));
    auto* reset = menu->addAction(tr("Reset to Linear"), this, [this, index] {
        originalSource_ = previewSource_;
        auto& definition = index == 0 ? previewSource_.fadeIn : previewSource_.fadeOut;
        const float start = definition.curve.front().value;
        const float end = definition.curve.back().value;
        definition.handles = {{1.0f / 3.0f, start + (end - start) / 3.0f,
                               2.0f / 3.0f, start + (end - start) * 2.0f / 3.0f}};
        if (previewSource_ != originalSource_) {
            originalSource_ = previewSource_;
            emit previewChanged();
            if (commit_)
                commit_(previewSource_);
        }
        update();
    });
    reset->setObjectName(QStringLiteral("sourceFadeReset"));
    auto* remove = menu->addAction(tr("Remove Fade"), this, [this, index] {
        originalSource_ = previewSource_;
        setFadeDuration(index, 0);
        if (previewSource_ != originalSource_) {
            originalSource_ = previewSource_;
            if (commit_)
                commit_(previewSource_);
        }
    });
    remove->setObjectName(QStringLiteral("sourceFadeRemove"));
    remove->setEnabled(fade(index).durationFrames > 0);
    menu->popup(position);
    update();
}

}
