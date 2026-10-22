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
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <vector>

namespace VNX {

namespace {

TimelineMarkerShape markerShapeForKind(MarkerKind kind)
{
    switch (kind) {
    case MarkerKind::Entry:
        return TimelineMarkerShape::EntryFlag;
    case MarkerKind::Exit:
        return TimelineMarkerShape::ExitFlag;
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
        if (panel_->deleteMarker(guid))
            panel_->setSelectedMarker(QUuid());
    });
}

int TimelineMarkerRuler::canvasX(int widgetX) const
{
    return widgetX + panel_->horizontalScroll();
}

int64_t TimelineMarkerRuler::coordinateAtWidgetX(int widgetX) const
{
    return panel_->coordinateAtPixel(0, canvasX(widgetX));
}

TimelineStyleView TimelineMarkerRuler::styleView() const
{
    TimelineStyleView style;
    const auto snapshot = ThemeManager::instance().snapshot();
    style.canvasBackground = snapshot->ui.colors.bgSecondary;
    style.canvasBorder = snapshot->ui.colors.borderPrimary;
    style.placeholderText = snapshot->ui.colors.textMuted;
    style.axisText = snapshot->ui.colors.textSecondary;
    style.trackBackground = snapshot->ui.colors.trackBackground;
    style.trackBackgroundAlt = snapshot->ui.colors.trackBackgroundAlt;
    style.timeline = snapshot->domains.timeline;
    style.music = snapshot->domains.music;
    style.metrics = snapshot->ui.metrics;
    return style;
}

TimelineRenderData TimelineMarkerRuler::buildRenderData() const
{
    TimelineRenderData data;
    struct MarkerRow {
        int index = 0;
        int x = 0;
        MarkerKind kind = MarkerKind::Custom;
        QUuid guid;
        QString label;
        QString tooltip;
        bool selected = false;
    };
    std::vector<MarkerRow> rows;
    rows.reserve(static_cast<size_t>(panel_->markerCount()));
    for (int index = 0; index < panel_->markerCount(); ++index) {
        MarkerRow row;
        row.index = index;
        row.guid = panel_->markerGuid(index);
        row.kind = panel_->markerKind(index);
        int64_t position = panel_->markerDisplayPosition(index);
        if (dragging_ && row.guid == activeGuid_)
            position = previewPosition_;
        row.x = panel_->pixelAtCoordinate(0, position) - panel_->horizontalScroll();
        row.selected = row.guid == panel_->selectedMarkerGuid();
        const QString name = row.kind == MarkerKind::Custom
            ? panel_->markerName(index) : markerKindName(row.kind);
        row.label = name.isEmpty() ? tr("Marker") : name;
        row.tooltip = QStringLiteral("%1 — %2").arg(
            row.label, panel_->formatCoordinate(0, panel_->markerDisplayPosition(index)));
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const MarkerRow& left, const MarkerRow& right) { return left.x < right.x; });
    for (size_t i = 0; i < rows.size(); ++i) {
        TimelineMarker marker;
        marker.x = rows[i].x;
        marker.height = height();
        marker.kind = rows[i].kind;
        marker.shape = markerShapeForKind(rows[i].kind);
        marker.label = rows[i].label;
        marker.tooltip = rows[i].tooltip;
        marker.selected = rows[i].selected;
        const int nextX = i + 1 < rows.size() ? rows[i + 1].x : width();
        marker.labelMaxWidth = std::max(0, nextX - rows[i].x - 8);
        data.markers.append(marker);
    }
    return data;
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
    cancelRename();
    const int index = markerAt(event->position().toPoint());
    if (index < 0) {
        panel_->setSelectedMarker(QUuid());
        return;
    }
    setFocus();
    activeGuid_ = panel_->markerGuid(index);
    previewPosition_ = panel_->markerPosition(index);
    pressPos_ = event->position().toPoint();
    dragArmed_ = true;
    dragging_ = false;
    panel_->setSelectedMarker(activeGuid_);
    update();
    event->accept();
}

void TimelineMarkerRuler::mouseMoveEvent(QMouseEvent* event)
{
    if (dragArmed_ && (event->buttons() & Qt::LeftButton) && !activeGuid_.isNull()) {
        if (!dragging_
            && (event->position().toPoint() - pressPos_).manhattanLength() >= 3)
            dragging_ = true;
        if (dragging_) {
            previewPosition_ = panel_->snapCoordinate(
                0, coordinateAtWidgetX(event->position().toPoint().x()),
                event->modifiers().testFlag(Qt::AltModifier));
            panel_->refreshTimelinePaint();
            event->accept();
            return;
        }
    }
    const int index = markerAt(event->position().toPoint());
    if (index >= 0) {
        const QString name = panel_->markerKind(index) == MarkerKind::Custom
            ? panel_->markerName(index) : markerKindName(panel_->markerKind(index));
        const QString label = name.isEmpty() ? tr("Marker") : name;
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1 — %2").arg(
                               label,
                               panel_->formatCoordinate(0, panel_->markerDisplayPosition(index))),
                           this);
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineMarkerRuler::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDragging = dragging_;
    dragging_ = false;
    dragArmed_ = false;
    if (wasDragging && !activeGuid_.isNull()) {
        panel_->moveMarker(activeGuid_, previewPosition_,
                        event->modifiers().testFlag(Qt::AltModifier));
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineMarkerRuler::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const int index = markerAt(event->position().toPoint());
    if (index < 0) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    event->accept();
    if (panel_->markerKind(index) != MarkerKind::Custom)
        return;
    beginRename(panel_->markerGuid(index));
}

void TimelineMarkerRuler::populateCreationMenu(QMenu* menu, int64_t position)
{
    const PieceTimeSpec spec = panel_->effectiveTimeSpec(0);
    const CoordinateResult snapped = TimelineCoordinates::snap(position, spec);
    const int64_t placed = snapped ? snapped.value : position;
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
    QAction* addCustom = menu->addAction(tr("Add Custom Marker"));
    addCustom->setObjectName(QStringLiteral("timelineAddCustomMarker"));
    connect(addCustom, &QAction::triggered, this, [this, placed] {
        panel_->addMarker(MarkerKind::Custom, placed, tr("Marker"));
    });
}

QMenu* TimelineMarkerRuler::createContextMenu(const QPoint& localPos)
{
    auto* menu = new QMenu(this);
    const int index = markerAt(localPos);
    if (index >= 0) {
        const QUuid guid = panel_->markerGuid(index);
        QAction* rename = menu->addAction(tr("Rename Marker"));
        rename->setEnabled(panel_->markerKind(index) == MarkerKind::Custom);
        connect(rename, &QAction::triggered, this, [this, guid] { beginRename(guid); });
        QAction* remove = menu->addAction(tr("Delete Marker"));
        connect(remove, &QAction::triggered, this, [this, guid] {
            if (panel_->deleteMarker(guid))
                panel_->setSelectedMarker(QUuid());
        });
        return menu;
    }
    populateCreationMenu(menu, coordinateAtWidgetX(localPos.x()));
    menu->addSeparator();
    QAction* moveCursor = menu->addAction(tr("Move Cursor Here"));
    connect(moveCursor, &QAction::triggered, this, [this, localPos] {
        panel_->setEditCursor(coordinateAtWidgetX(localPos.x()));
    });
    return menu;
}

void TimelineMarkerRuler::contextMenuEvent(QContextMenuEvent* event)
{
    cancelRename();
    QMenu* menu = createContextMenu(event->pos());
    menu->exec(event->globalPos());
    menu->deleteLater();
    event->accept();
}

void TimelineMarkerRuler::beginRename(const QUuid& guid)
{
    int index = -1;
    for (int i = 0; i < panel_->markerCount(); ++i) {
        if (panel_->markerGuid(i) == guid) {
            index = i;
            break;
        }
    }
    if (index < 0 || panel_->markerKind(index) != MarkerKind::Custom)
        return;
    if (!nameEditor_) {
        nameEditor_ = new QLineEdit(this);
        nameEditor_->setObjectName(QStringLiteral("timelineMarkerNameEditor"));
        nameEditor_->installEventFilter(this);
        connect(nameEditor_, &QLineEdit::returnPressed, this, &TimelineMarkerRuler::commitRename);
    }
    renamingGuid_ = guid;
    const int x = panel_->pixelAtCoordinate(0, panel_->markerDisplayPosition(index))
        - panel_->horizontalScroll();
    nameEditor_->setGeometry(x + 6, 0, std::max(48, width() - x - 8), height());
    nameEditor_->setText(panel_->markerName(index));
    nameEditor_->selectAll();
    nameEditor_->show();
    nameEditor_->setFocus();
}

void TimelineMarkerRuler::commitRename()
{
    if (!nameEditor_ || renamingGuid_.isNull())
        return;
    const QString name = nameEditor_->text();
    const QUuid guid = renamingGuid_;
    renamingGuid_ = QUuid();
    nameEditor_->hide();
    panel_->renameMarker(guid, name);
}

void TimelineMarkerRuler::cancelRename()
{
    if (!nameEditor_)
        return;
    renamingGuid_ = QUuid();
    nameEditor_->hide();
}

bool TimelineMarkerRuler::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == nameEditor_) {
        if (event->type() == QEvent::FocusOut
            && static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason)
            commitRename();
        else if (event->type() == QEvent::KeyPress
                 && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancelRename();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

int TimelineMarkerRuler::markerAt(const QPoint& point) const
{
    for (int index = panel_->markerCount() - 1; index >= 0; --index)
        if (std::abs(canvasX(point.x()) - panel_->pixelAtCoordinate(
                0, panel_->markerDisplayPosition(index)))
            <= panel_->timelineMetrics().markerHitRadius)
            return index;
    return -1;
}

}
