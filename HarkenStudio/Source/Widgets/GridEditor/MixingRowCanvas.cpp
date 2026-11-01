#include "MixingRowCanvas.h"

#include "Core/FontManager.h"
#include "Core/ThemeManager.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QScopedValueRollback>

#include <algorithm>

namespace VNX {

MixingRowCanvas::MixingRowCanvas(QWidget* parent) : CurveCanvas(parent)
{
    connect(this, &CurveCanvas::overlaySelected, this, [this](int overlay) {
        if (overlay >= 0 && overlay < overlayClips_.size()) emit clipSelected(overlayClips_[overlay]);
    });
}

void MixingRowCanvas::setClips(const QVector<MixingCurveClip>& clips, int selected)
{
    cancelDrag();
    clips_ = clips;
    selected_ = selected >= 0 && selected < clips.size() && !clips[selected].points.isEmpty()
        ? selected : -1;
    overlayClips_.clear();
    QVector<CurveOverlay> overlays;
    const auto& colors = ThemeManager::instance().colors();
    for (int i = 0; i < clips.size(); ++i) {
        if (clips[i].points.isEmpty()) continue;
        if (i == selected_) setCurve(clips[i].points, clips[i].handles);
        else {
            overlayClips_.append(i);
            overlays.append({clips[i].points, clips[i].handles, CurveColorRole::GridFade,
                             colors.categoricalColor(i).darker(150), true});
        }
    }
    setCurveVisible(profile().xAxisVisible && selected_ >= 0);
    setOverlays(profile().xAxisVisible ? overlays : QVector<CurveOverlay>{});
}

int MixingRowCanvas::titleHeight() const
{
    return QFontMetrics(FontManager::instance().getFont(FontRole::Caption)).height()
        + ThemeManager::instance().metrics().table.cellPaddingY * 2;
}

QRectF MixingRowCanvas::graphRect() const
{
    auto graph = CurveCanvas::graphRect();
    graph.setTop(std::min(graph.bottom() - 1.0, graph.top() + titleHeight()));
    return graph;
}

QVector<CurvePointDef> MixingRowCanvas::clipPoints(int clip) const
{
    return clip == selected_ ? curve() : clips_[clip].points;
}

QRectF MixingRowCanvas::clipTitleRect(int clip) const
{
    if (clip < 0 || clip >= clips_.size()) return {};
    const auto points = clipPoints(clip);
    if (points.size() < 2) return {};
    const auto graph = graphRect();
    const auto toX = [&](float x) { return graph.left() + (x - xMin()) / (xMax() - xMin()) * graph.width(); };
    return QRectF(QPointF(toX(points.front().x), graph.top() - titleHeight()),
                  QPointF(toX(points.back().x), graph.top()));
}

int MixingRowCanvas::titleAt(const QPointF& position) const
{
    if (position.y() >= graphRect().top()
        - ThemeManager::instance().snapshot()->domains.curve.metrics.pointSelectedRadius) return -1;
    if (selected_ >= 0 && clipTitleRect(selected_).contains(position)) return selected_;
    for (int i = clips_.size() - 1; i >= 0; --i)
        if (clipTitleRect(i).contains(position)) return i;
    return -1;
}

void MixingRowCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    auto style = ThemeManager::instance().snapshot()->domains.curve;
    auto data = renderData();
    data.frameVisible = false;
    const auto& colors = ThemeManager::instance().colors();
    if (selected_ >= 0)
        data.curveColor = colors.categoricalColor(selected_);
    for (int i = 0; i < data.overlays.size(); ++i)
        data.overlays[i].color = colors.categoricalColor(overlayClips_[i]).darker(150);
    if (!isEnabled()) {
        style.colors.plotBackground = colors.bgSecondary;
        style.colors.axisText = colors.textDisabled;
    }
    CurveRenderer::renderBackground(painter, style, data);
    if (!profile().xAxisVisible) {
        CurveRenderer::renderForeground(painter, style, CurveColorRole::GridFade, data);
        return;
    }
    auto drawOrder = overlayClips_;
    if (selected_ >= 0 && selected_ < clips_.size()) drawOrder.append(selected_);
    const auto drawClip = [&](int index, const QPainterPath& path) {
        const bool selected = index == selected_;
        const auto baseColor = colors.categoricalColor(index);
        const QColor color = selected ? baseColor : baseColor.darker(150);
        const auto title = clipTitleRect(index);
        painter.save();
        painter.setClipRect(QRectF(data.graphRect.left(), title.top(), data.graphRect.width(),
            data.graphRect.bottom() - title.top()).intersected(rect()));
        if (!path.isEmpty()) {
            const auto first = path.pointAtPercent(0.0);
            const auto last = path.currentPosition();
            auto fill = path;
            fill.lineTo(last.x(), data.graphRect.bottom());
            fill.lineTo(first.x(), data.graphRect.bottom());
            fill.closeSubpath();
            QLinearGradient gradient(0.0, data.graphRect.top(), 0.0, data.graphRect.bottom());
            QColor top = color;
            top.setAlphaF(top.alphaF() * (selected ? 85 : 55) / 255.0);
            QColor bottom = color;
            bottom.setAlpha(0);
            gradient.setColorAt(0.0, top);
            gradient.setColorAt(1.0, bottom);
            painter.fillPath(fill, gradient);
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QPen(color, 1.0, Qt::DashLine, Qt::FlatCap));
            for (const auto& endpoint : {first, last})
                painter.drawLine(endpoint, QPointF(endpoint.x(), data.graphRect.bottom()));
        }
        QColor header = color;
        header.setAlphaF(header.alphaF() * (selected ? 65 : 35) / 255.0);
        painter.fillRect(title, style.colors.canvasBackground);
        painter.fillRect(title, header);
        painter.setFont(FontManager::instance().getFont(FontRole::Caption));
        painter.setPen(selected ? ThemeManager::instance().colors().textPrimary
                                : ThemeManager::instance().colors().textSecondary);
        QRegion visibleTitle(title.intersected(QRectF(data.graphRect.left(), title.top(),
            data.graphRect.width(), title.height())).toAlignedRect());
        for (int above = drawOrder.indexOf(index) + 1; above < drawOrder.size(); ++above)
            visibleTitle -= clipTitleRect(drawOrder[above]).toAlignedRect();
        QRect label;
        for (const auto& part : visibleTitle)
            if (part.width() > label.width()) label = part;
        label.adjust(ThemeManager::instance().metrics().table.cellPaddingX, 0,
                     -ThemeManager::instance().metrics().table.cellPaddingX, 0);
        painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(clips_[index].title, Qt::ElideRight, std::max(0, int(label.width()))));
        painter.restore();
    };
    for (int i = 0; i < overlayClips_.size(); ++i)
        drawClip(overlayClips_[i], data.overlays[i].discretePath.value_or(data.overlays[i].path));
    if (selected_ >= 0 && selected_ < clips_.size()) {
        drawClip(selected_, data.discretePath.value_or(data.continuousPath));
        style.colors.pointBorder = data.curveColor;
        style.colors.pointSelectedBorder = data.curveColor;
        style.colors.pointSelectedFill = data.curveColor;
    }
    CurveRenderer::renderForeground(painter, style, CurveColorRole::GridFade, data);
}

void MixingRowCanvas::mousePressEvent(QMouseEvent* event)
{
    const int clip = titleAt(event->position());
    if (event->button() != Qt::LeftButton || clip < 0) {
        CurveCanvas::mousePressEvent(event);
        return;
    }
    cancelDrag();
    setFocus();
    if (clip != selected_) emit clipSelected(clip);
    if (selected_ != clip) return;
    dismissValueEditor();
    setSelectedPointIndex(-1);
    moving_ = true;
    moved_ = false;
    moveOrigin_ = event->position();
    movePoints_ = curve();
    moveHandles_ = curveHandles();
    setCursor(Qt::SizeHorCursor);
}

void MixingRowCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!moving_) {
        CurveCanvas::mouseMoveEvent(event);
        if (titleAt(event->position()) >= 0) setCursor(Qt::SizeHorCursor);
        return;
    }
    if (!moved_ && (event->position() - moveOrigin_).manhattanLength() < QApplication::startDragDistance()) return;
    const float requested = (event->position().x() - moveOrigin_.x()) / graphRect().width() * (xMax() - xMin());
    const float delta = std::clamp(requested, std::min(0.0f, xMin() - movePoints_.front().x),
                                 std::max(0.0f, xMax() - movePoints_.back().x));
    auto points = movePoints_;
    auto handles = moveHandles_;
    for (auto& point : points) point.x += delta;
    for (auto& handle : handles) { handle.h1x += delta; handle.h2x += delta; }
    {
        QScopedValueRollback guard(updatingClip_, true);
        setCurve(points, handles);
    }
    moved_ = delta != 0.0f;
    emit curvePreviewed(points, handles);
    update();
}

void MixingRowCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (!moving_) { CurveCanvas::mouseReleaseEvent(event); return; }
    if (event->button() != Qt::LeftButton) return;
    moving_ = false;
    if (moved_) emit curveEdited(curve(), curveHandles(), tr("Move Mixing Curve"));
    moved_ = false;
    movePoints_.clear();
    moveHandles_.clear();
    emit curvePreviewFinished();
}

void MixingRowCanvas::cancelDrag()
{
    if (updatingClip_) return;
    if (moving_) {
        moving_ = false;
        moved_ = false;
        setCurve(movePoints_, moveHandles_);
        movePoints_.clear();
        moveHandles_.clear();
        emit curvePreviewFinished();
    }
    CurveCanvas::cancelDrag();
}

void MixingRowCanvas::keyPressEvent(QKeyEvent* event)
{
    if (moving_ && event->key() == Qt::Key_Escape) { cancelDrag(); event->accept(); return; }
    CurveCanvas::keyPressEvent(event);
}

void MixingRowCanvas::leaveEvent(QEvent* event)
{
    if (!moving_) unsetCursor();
    CurveCanvas::leaveEvent(event);
}

}
