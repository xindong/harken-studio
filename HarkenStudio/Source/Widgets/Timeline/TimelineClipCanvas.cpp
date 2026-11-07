#include "TimelineClipCanvas.h"

#include "TimelineEditorPanel.h"
#include "TimelineRenderer.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/Timeline/TimelineEditing.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include "Core/ToolTip.h"

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

TimelineMarkerShape markerShapeForKind(MarkerKind kind)
{
    switch (kind) {
    case MarkerKind::Entry:
        return TimelineMarkerShape::EntryHandle;
    case MarkerKind::Exit:
        return TimelineMarkerShape::ExitHandle;
    case MarkerKind::Custom:
        return TimelineMarkerShape::Diamond;
    }
    return TimelineMarkerShape::Diamond;
}

}

TimelineMarkerRuler::TimelineMarkerRuler(TimelineEditorPanel* panel, QWidget* parent)
    : QWidget(parent), panel_(panel)
{
    setObjectName(QStringLiteral("timelineMarkerRuler"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    ShortcutManager::instance().bindWidgetAction(Shortcuts::Delete, this, [this]() {
        const QUuid guid = panel_->selectedMarkerGuid();
        if (guid.isNull())
            return;
        for (int index = 0; index < panel_->markerCount(); ++index)
            if (panel_->markerGuid(index) == guid && panel_->markerKind(index) == MarkerKind::Custom)
                return;
        if (panel_->deleteMarker(guid))
            panel_->setSelectedMarker(QUuid());
    });
}

int64_t TimelineMarkerRuler::coordinateAtWidgetX(int widgetX) const
{
    return panel_->coordinateAtViewportPixel(0, widgetX);
}

TimelineStyleView TimelineMarkerRuler::styleView() const
{
    TimelineStyleView style;
    const auto snapshot = ThemeManager::instance().snapshot();
    style.canvasBackground = snapshot->ui.colors.bgSecondary;
    style.canvasBorder = snapshot->ui.colors.borderPrimary;
    style.placeholderText = snapshot->ui.colors.textSecondary;
    style.axisText = snapshot->ui.colors.textSecondary;
    style.axisTick = snapshot->ui.colors.borderTick;
    style.textPrimary = snapshot->ui.colors.textPrimary;
    style.uniformTimeAxisGrid = snapshot->ui.colors.borderTick;
    style.trackBackground = snapshot->ui.colors.trackBackground;
    style.trackBackgroundAlt = snapshot->ui.colors.trackBackgroundAlt;
    style.timeline = snapshot->domains.timeline;
    style.music = snapshot->domains.music;
    style.curve = snapshot->domains.curve;
    style.metrics = snapshot->ui.metrics;
    return style;
}

TimelineRenderData TimelineMarkerRuler::buildRenderData() const
{
    TimelineRenderData data;
    data.viewport = rect();
    data.bottomBorder = true;
    data.timeAxis = panel_->timeTicks();
    data.markers.reserve(panel_->markerCount());
    for (int index = 0; index < panel_->markerCount(); ++index) {
        const QUuid guid = panel_->markerGuid(index);
        const int64_t position = panel_->markerDisplayPosition(index);
        TimelineMarker marker;
        marker.x = panel_->viewportPixelAtCoordinate(0, position);
        marker.height = height();
        marker.kind = panel_->markerKind(index);
        marker.shape = markerShapeForKind(marker.kind);
        marker.selected = guid == panel_->selectedMarkerGuid();
        marker.hovered = guid == panel_->hoveredMarkerGuid();
        data.markers.append(marker);
    }
    std::sort(data.markers.begin(), data.markers.end(),
        [](const TimelineMarker& left, const TimelineMarker& right) { return left.x < right.x; });
    data.editCursor.visible = panel_->isEditCursorVisible();
    data.editCursor.highlighted = panel_->isCursorHovered() || panel_->isCursorDragActive();
    data.editCursor.x = panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
    return data;
}

void TimelineMarkerRuler::cancelInteraction()
{
    panel_->cancelMarkerInteraction();
    panel_->endCursorDrag(true);
    panel_->setCursorHovered(false);
    unsetCursor();
    update();
}

void TimelineMarkerRuler::hideEvent(QHideEvent* event)
{
    cancelInteraction();
    QWidget::hideEvent(event);
}

void TimelineMarkerRuler::leaveEvent(QEvent* event)
{
    panel_->setHoveredMarker({});
    panel_->setCursorHovered(false);
    ToolTip::hideText();
    if (!panel_->isMarkerDragActive() && !panel_->isCursorDragActive())
        unsetCursor();
    QWidget::leaveEvent(event);
}

void TimelineMarkerRuler::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape
        && (panel_->isMarkerDragActive() || panel_->isCursorDragActive())) {
        cancelInteraction();
        panel_->refreshTimelinePaint();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineMarkerRuler::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    lastRenderData_ = buildRenderData();
    TimelineRenderer::render(painter, styleView(), lastRenderData_);
}

void TimelineMarkerRuler::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint point = event->position().toPoint();
    if (panel_->hitsEditCursor(point.x())) {
        panel_->cancelMarkerInteraction();
        panel_->setCursorHovered(true);
        setFocus();
        setCursor(Qt::SizeHorCursor);
        ToolTip::hideText();
        panel_->beginCursorDrag(point.x(), event->modifiers().testFlag(Qt::AltModifier), false);
        event->accept();
        return;
    }
    const int index = markerAt(point);
    if (index < 0) {
        panel_->setSelectedMarker(QUuid());
        return;
    }
    setFocus();
    panel_->beginMarkerDrag(panel_->markerGuid(index), point.x(),
        event->modifiers().testFlag(Qt::AltModifier));
    setCursor(Qt::SizeHorCursor);
    update();
    event->accept();
}

void TimelineMarkerRuler::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    if (panel_->isCursorDragActive() && (event->buttons() & Qt::LeftButton)) {
        panel_->updateCursorDrag(point.x(), event->modifiers().testFlag(Qt::AltModifier));
        event->accept();
        return;
    }
    if (panel_->isMarkerDragActive() && (event->buttons() & Qt::LeftButton)) {
        panel_->updateMarkerDrag(point.x(), event->modifiers().testFlag(Qt::AltModifier));
        event->accept();
        return;
    }
    updateHover(point);
    event->accept();
}

void TimelineMarkerRuler::updateHover(const QPoint& position)
{
    const bool cursor = rect().contains(position) && panel_->hitsEditCursor(position.x());
    const int index = cursor ? -1 : markerAt(position);
    const QUuid marker = index >= 0 ? panel_->markerGuid(index) : QUuid();
    const bool changed = panel_->isCursorHovered() != cursor || panel_->hoveredMarkerGuid() != marker;
    panel_->setCursorHovered(cursor);
    panel_->setHoveredMarker(marker);
    setCursor(cursor || !marker.isNull() ? Qt::SizeHorCursor : Qt::ArrowCursor);
    if (!changed)
        return;
    const QString tooltip = cursor ? panel_->cursorTooltip() : panel_->markerTooltip(marker);
    if (tooltip.isEmpty())
        ToolTip::hideText();
    else
        ToolTip::showText(mapToGlobal(position), tooltip, this);
}

void TimelineMarkerRuler::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (panel_->isCursorDragActive()) {
        panel_->updateCursorDrag(event->position().toPoint().x(),
            event->modifiers().testFlag(Qt::AltModifier));
        panel_->endCursorDrag();
        updateHover(event->position().toPoint());
        event->accept();
        return;
    }
    if (panel_->isMarkerDragActive()) {
        panel_->updateMarkerDrag(event->position().toPoint().x(),
            event->modifiers().testFlag(Qt::AltModifier));
        panel_->endMarkerDrag();
        updateHover(event->position().toPoint());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineMarkerRuler::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (panel_->hitsEditCursor(event->position().toPoint().x())) {
            panel_->editCursorValue();
            event->accept();
            return;
        }
        const int index = markerAt(event->position().toPoint());
        if (index >= 0) {
            panel_->editMarkerValue(panel_->markerGuid(index));
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineMarkerRuler::populateCreationMenu(QMenu* menu, int64_t position)
{
    const int64_t placed = panel_->snapCoordinate(0, position, false);
    const bool hasEntry = panel_->hasMarkerKind(MarkerKind::Entry);
    const bool hasExit = panel_->hasMarkerKind(MarkerKind::Exit);
    QAction* addEntry = menu->addAction(tr("Add Entry Marker"));
    addEntry->setObjectName(QStringLiteral("timelineAddEntryMarker"));
    addEntry->setEnabled(!hasEntry);
    connect(addEntry, &QAction::triggered, this, [this, placed] {
        panel_->addMarker(MarkerKind::Entry, placed);
    });
    QAction* addExit = menu->addAction(tr("Add Exit Marker"));
    addExit->setObjectName(QStringLiteral("timelineAddExitMarker"));
    addExit->setEnabled(!hasExit);
    connect(addExit, &QAction::triggered, this, [this, placed] {
        panel_->addMarker(MarkerKind::Exit, placed);
    });
}

QMenu* TimelineMarkerRuler::createContextMenu(const QPoint& localPos)
{
    const int index = markerAt(localPos);
    if (index >= 0)
        return panel_->createMarkerContextMenu(panel_->markerGuid(index), this);
    auto* menu = new QMenu(this);
    populateCreationMenu(menu, coordinateAtWidgetX(localPos.x()));
    if (panel_->isEditCursorVisible()) {
        menu->addSeparator();
        QAction* moveCursor = menu->addAction(tr("Move Cursor Here"));
        connect(moveCursor, &QAction::triggered, this, [this, localPos] {
            panel_->setEditCursor(coordinateAtWidgetX(localPos.x()));
        });
    }
    return menu;
}

void TimelineMarkerRuler::contextMenuEvent(QContextMenuEvent* event)
{
    cancelInteraction();
    if (panel_->hitsEditCursor(event->pos().x())) {
        panel_->showCursorContextMenu(event->globalPos());
        event->accept();
        return;
    }
    QMenu* menu = createContextMenu(event->pos());
    if (!menu->isEmpty())
        menu->exec(event->globalPos());
    menu->deleteLater();
    event->accept();
}

int TimelineMarkerRuler::markerAt(const QPoint& point) const
{
    if (!rect().contains(point))
        return -1;
    int nearest = -1;
    const auto& metrics = ThemeManager::instance().metrics();
    int nearestDistance = metrics.timeAxis.handleWidth + metrics.timeAxis.handleHitRadius + 1;
    for (int index = panel_->markerCount() - 1; index >= 0; --index) {
        if (panel_->markerKind(index) == MarkerKind::Custom)
            continue;
        const int x = panel_->viewportPixelAtCoordinate(0, panel_->markerDisplayPosition(index));
        if (x < 0 || x >= width())
            continue;
        const QRect grip = TimelineRenderer::markerHandleRect(
            x, height(), markerShapeForKind(panel_->markerKind(index)), metrics.timeAxis);
        const bool overGrip = !grip.isEmpty() && grip.adjusted(-metrics.spacingCompact, -metrics.spacingCompact, metrics.spacingCompact, metrics.spacingCompact).contains(point);
        const int distance = std::abs(point.x() - (overGrip ? grip.center().x() : x));
        const int radius = overGrip ? grip.width() / 2 + metrics.spacingCompact : metrics.timeAxis.handleHitRadius;
        if (distance <= radius && distance < nearestDistance) {
            nearest = index;
            nearestDistance = distance;
        }
    }
    return nearest;
}

bool TimelineMarkerRuler::isDragging() const
{
    return panel_->isMarkerDragging();
}

QUuid TimelineMarkerRuler::draggingGuid() const
{
    return panel_->draggingMarkerGuid();
}

QUuid TimelineMarkerRuler::hoveredGuid() const
{
    return panel_->hoveredMarkerGuid();
}

int64_t TimelineMarkerRuler::previewPosition() const
{
    return panel_->markerDragPreviewPosition();
}

}
