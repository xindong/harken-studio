
#include "BlueprintCanvasView.h"

#include "Core/ThemeManager.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QScrollBar>
#include <QSettings>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace VNX {

BlueprintCanvasView::BlueprintCanvasView(QWidget* parent)
    : QGraphicsView(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setDragMode(QGraphicsView::RubberBandDrag);
    if (QSettings().value(QStringLiteral("Blueprint/InteractionMode")) == QLatin1String("Pan"))
        setInteractionMode(InteractionMode::Pan);
}

BlueprintCanvasView::~BlueprintCanvasView() = default;

void BlueprintCanvasView::setInteractionMode(InteractionMode mode)
{
    if (interactionMode_ == mode) return;
    interactionMode_ = mode;
    setInteractive(mode == InteractionMode::Select);
    setDragMode(mode == InteractionMode::Select ? QGraphicsView::RubberBandDrag : QGraphicsView::NoDrag);
    updateInteractionCursor_();
    QSettings().setValue(QStringLiteral("Blueprint/InteractionMode"),
        mode == InteractionMode::Pan ? QStringLiteral("Pan") : QStringLiteral("Select"));
    emit interactionModeChanged(mode);
}

bool BlueprintCanvasView::viewportEvent(QEvent* event)
{
    const bool handled = QGraphicsView::viewportEvent(event);
    if (panButton_ != Qt::NoButton || interactionMode_ == InteractionMode::Pan)
        updateInteractionCursor_();
    return handled;
}

void BlueprintCanvasView::updateInteractionCursor_()
{
    const auto shape = panButton_ != Qt::NoButton ? Qt::ClosedHandCursor
        : interactionMode_ == InteractionMode::Pan ? Qt::OpenHandCursor : Qt::ArrowCursor;
    if (cursor().shape() != shape) setCursor(shape);
    if (viewport()->cursor().shape() != shape) viewport()->setCursor(shape);
}

double BlueprintCanvasView::gridSpacing() const
{
    const double zoom = std::clamp(transform().m11(), kMinZoom, kMaxZoom);
    double pitch = kGridSpacing;
    while (pitch * zoom < 16.0) pitch *= 2.0;
    while (pitch * zoom >= 32.0) pitch *= 0.5;
    return pitch;
}

void BlueprintCanvasView::drawBackground(QPainter* p, const QRectF& rect)
{
    const auto theme = ThemeManager::instance().snapshot();
    const auto& bp = theme->domains.blueprint;

    const double scale = p->worldTransform().m11();
    if (scale <= 0.0) {
        p->fillRect(rect, bp.canvasBackground);
        return;
    }

    const double pitch = gridSpacing();
    const double screenPitch = pitch * scale;
    const double dpr = p->device()->devicePixelRatioF();
    if (gridTile_.isNull() || gridTilePitch_ != screenPitch || gridTileDpr_ != dpr
        || gridTileBackground_ != bp.canvasBackground || gridTileDot_ != bp.gridDot) {
        const int pixels = static_cast<int>(std::ceil(screenPitch * dpr));
        gridTile_ = QPixmap(pixels, pixels);
        gridTile_.fill(bp.canvasBackground);
        QPainter tilePainter(&gridTile_);
        tilePainter.setRenderHint(QPainter::Antialiasing);
        tilePainter.setPen(Qt::NoPen);
        tilePainter.setBrush(bp.gridDot);
        const double radius = pixels / screenPitch;
        tilePainter.drawEllipse(QPointF(pixels / 2.0, pixels / 2.0), radius, radius);
        gridTilePitch_ = screenPitch;
        gridTileDpr_ = dpr;
        gridTileBackground_ = bp.canvasBackground;
        gridTileDot_ = bp.gridDot;
    }
    QBrush brush(gridTile_);
    brush.setTransform(QTransform(pitch / gridTile_.width(), 0.0, 0.0,
        pitch / gridTile_.height(), -pitch / 2.0, -pitch / 2.0));
    p->save();
    p->setRenderHint(QPainter::Antialiasing, false);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    p->fillRect(rect, brush);
    p->restore();
}

void BlueprintCanvasView::wheelEvent(QWheelEvent* event)
{
    event->accept();
    if (event->angleDelta().y() == 0) return;
    const double stepFactor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    const double current = transform().m11();
    const double target = std::clamp(current * stepFactor, kMinZoom, kMaxZoom);
    if (!qFuzzyCompare(target, current)) {
        const QPointF anchor = viewportTransform().inverted().map(event->position());
        scale(target / current, target / current);
        const QPointF offset = viewportTransform().map(anchor) - event->position();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(offset.x()));
        verticalScrollBar()->setValue(verticalScrollBar()->value() + qRound(offset.y()));
        emit zoomChanged(transform().m11());
    }
}

void BlueprintCanvasView::setZoomFactor(double factor)
{
    if (factor <= 0.0) return;
    const double clamped = std::clamp(factor, kMinZoom, kMaxZoom);
    const QPointF center = mapToScene(viewport()->rect().center());
    setTransform(QTransform::fromScale(clamped, clamped));
    centerOn(center);
    emit zoomChanged(transform().m11());
}

void BlueprintCanvasView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton
        || (event->button() == Qt::LeftButton && interactionMode_ == InteractionMode::Pan))
    {
        panButton_ = event->button();
        panStartPos_ = event->pos();
        panLastPos_  = event->pos();
        if (panButton_ == Qt::RightButton) rightButtonDragOccurred_ = false;
        updateInteractionCursor_();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void BlueprintCanvasView::mouseMoveEvent(QMouseEvent* event)
{
    if (panButton_ != Qt::NoButton && (event->buttons() & panButton_))
    {
        const QPoint cumulative = event->pos() - panStartPos_;
        if (panButton_ == Qt::RightButton && cumulative.manhattanLength() > QApplication::startDragDistance())
            rightButtonDragOccurred_ = true;
        const QPoint step = event->pos() - panLastPos_;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - step.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - step.y());
        panLastPos_ = event->pos();
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void BlueprintCanvasView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == panButton_)
    {
        panButton_ = Qt::NoButton;
        updateInteractionCursor_();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void BlueprintCanvasView::contextMenuEvent(QContextMenuEvent* event)
{
    if (rightButtonDragOccurred_)
    {
        rightButtonDragOccurred_ = false;
        event->accept();
        return;
    }

    QMenu menu(this);
    populateContextMenu(menu, event);
    if (!menu.isEmpty()) {
        menu.exec(event->globalPos());
    } else {
        QGraphicsView::contextMenuEvent(event);
    }
}

void BlueprintCanvasView::populateContextMenu(QMenu& menu, QContextMenuEvent* event)
{
    Q_UNUSED(menu);
    Q_UNUSED(event);
}

}
