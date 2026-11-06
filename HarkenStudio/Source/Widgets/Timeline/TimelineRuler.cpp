#include "TimelineRuler.h"

#include "TimelineEditorPanel.h"
#include "TimelineClipCanvas.h"
#include "TimelineRenderer.h"
#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QActionGroup>
#include <QGuiApplication>
#include <QContextMenuEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QResizeEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QToolButton>
#include "Core/ToolTip.h"

#include <algorithm>

namespace VNX {

TimelineRuler::TimelineRuler(TimelineEditorPanel* panel, QWidget* parent)
    : QWidget(parent)
    , panel_(panel)
{
    setObjectName(QStringLiteral("timelineRuler"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    axisModeButton_ = new QToolButton(this);
    axisModeButton_->setObjectName(QStringLiteral("timelineAxisMode"));
    axisModeButton_->setAutoRaise(true);
    axisModeButton_->setArrowType(Qt::DownArrow);
    axisModeButton_->setCursor(Qt::PointingHandCursor);
    axisModeButton_->setAccessibleName(tr("Timeline axis"));
    QtControlSkin::setSemanticProperty(axisModeButton_, "flatIconButton", true);
    QtControlSkin::setSemanticProperty(axisModeButton_, "navigationRole", QStringLiteral("chevron"));
    auto* menu = new QMenu(axisModeButton_);
    menu->setObjectName(QStringLiteral("timelineAxisModeMenu"));
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    secondsAction_ = menu->addAction(tr("Seconds"));
    secondsAction_->setObjectName(QStringLiteral("timelineAxisSeconds"));
    secondsAction_->setData(static_cast<int>(TimelineEditorPanel::AxisMode::Seconds));
    measureBeatsAction_ = menu->addAction(tr("Measure-Beats"));
    measureBeatsAction_->setObjectName(QStringLiteral("timelineAxisMeasureBeats"));
    measureBeatsAction_->setData(static_cast<int>(TimelineEditorPanel::AxisMode::MeasureBeats));
    secondsWithMeasureBeatsAction_ = menu->addAction(tr("Seconds with Measure-Beats"));
    secondsWithMeasureBeatsAction_->setObjectName(QStringLiteral("timelineAxisSecondsWithMeasureBeats"));
    secondsWithMeasureBeatsAction_->setData(static_cast<int>(TimelineEditorPanel::AxisMode::SecondsWithMeasureBeats));
    measureBeatsWithSecondsAction_ = menu->addAction(tr("Measure-Beats with Seconds"));
    measureBeatsWithSecondsAction_->setObjectName(QStringLiteral("timelineAxisMeasureBeatsWithSeconds"));
    measureBeatsWithSecondsAction_->setData(static_cast<int>(TimelineEditorPanel::AxisMode::MeasureBeatsWithSeconds));
    for (auto* action : {secondsAction_, measureBeatsAction_,
                         secondsWithMeasureBeatsAction_, measureBeatsWithSecondsAction_}) {
        action->setCheckable(true);
        group->addAction(action);
    }
    connect(axisModeButton_, &QToolButton::clicked, this, [this, menu] {
        menu->popup(axisModeButton_->mapToGlobal(QPoint(0, axisModeButton_->height())));
    });
    connect(group, &QActionGroup::triggered, this, [this](QAction* action) {
        panel_->setAxisMode(static_cast<TimelineEditorPanel::AxisMode>(action->data().toInt()));
    });
    markerLane_ = new TimelineMarkerRuler(panel_, this);
    connect(panel_, &TimelineEditorPanel::cursorDragFinished, this, [this] {
        if (dragMode_ == DragMode::Cursor)
            dragMode_ = DragMode::None;
    });
    cursorValue_ = new QLabel(this, Qt::ToolTip | Qt::WindowTransparentForInput);
    cursorValue_->setObjectName(QStringLiteral("timelineCursorValue"));
    cursorValue_->setAttribute(Qt::WA_ShowWithoutActivating);
    cursorValue_->setAutoFillBackground(true);
    cursorValue_->setFrameStyle(QFrame::Box | QFrame::Plain);
    cursorValue_->setContentsMargins(7, 4, 7, 4);
    cursorValue_->hide();
    refresh();
}

void TimelineRuler::refresh()
{
    const bool active = panel_->hasTimeline();
    syncGeometry();
    axisModeButton_->setVisible(active);
    axisModeButton_->setEnabled(active);
    const bool musical = panel_->timelineDomain() == TimelineDomain::Musical;
    for (auto* action : {secondsAction_, measureBeatsAction_,
                         secondsWithMeasureBeatsAction_, measureBeatsWithSecondsAction_}) {
        const auto mode = static_cast<TimelineEditorPanel::AxisMode>(action->data().toInt());
        action->setChecked(mode == panel_->axisMode());
        action->setEnabled(active && (mode == TimelineEditorPanel::AxisMode::Seconds || musical));
    }
    markerLane_->update();
    if (!active) {
        dragMode_ = DragMode::None;
        cursorValue_->hide();
    }
    const auto theme = ThemeManager::instance().snapshot();
    QPalette palette = cursorValue_->palette();
    palette.setColor(QPalette::Window, theme->ui.colors.bgSecondary);
    palette.setColor(QPalette::WindowText, theme->ui.colors.textPrimary);
    cursorValue_->setPalette(palette);
    if (panel_->isEditCursorVisible() && panel_->isCursorDragActive())
        showCursorValue();
    else
        cursorValue_->hide();
    updateMouseCursor();
    update();
}

void TimelineRuler::syncGeometry()
{
    const bool segment = panel_->viewKind() == TimelineEditorPanel::ViewKind::Segment;
    const int markerHeight = segment ? panel_->timelineMetrics().markerLaneHeight : 0;
    setFixedHeight(timeRulerHeight());
    markerLane_->setGeometry(panel_->laneHeaderWidth(), height() - markerHeight,
        panel_->timelineViewportWidth(), markerHeight);
    markerLane_->setVisible(segment);
    const auto& metrics = ThemeManager::instance().metrics();
    const int extent = metrics.navigationChevronExtent;
    const int x = segment
        ? panel_->laneHeaderWidth() - panel_->laneHeaderResizeWidth() - metrics.spacingXSmall - extent
        : (panel_->laneHeaderWidth() - extent) / 2;
    axisModeButton_->setGeometry(x, (timeRulerHeight() - extent) / 2, extent, extent);
    axisModeButton_->setIconSize(QSize(extent, extent));
}

void TimelineRuler::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    syncGeometry();
}

int TimelineRuler::timeRulerHeight() const
{
    if (!panel_->hasTimeline())
        return panel_->timelineMetrics().rulerHeight;
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Body), this);
    const int primaryHeight = std::max(metrics.height() + 5,
        ThemeManager::instance().metrics().navigationChevronExtent);
    const int secondaryHeight = panel_->hasSecondaryAxis()
        ? QFontMetrics(FontManager::instance().getFont(FontRole::Caption), this).height() : 0;
    const int markerHeight = panel_->viewKind() == TimelineEditorPanel::ViewKind::Segment
        ? panel_->timelineMetrics().markerLaneHeight : 0;
    return primaryHeight + secondaryHeight + markerHeight;
}

QRect TimelineRuler::timeAxisRect() const
{
    if (!panel_->hasTimeline())
        return {};
    return QRect(panel_->laneHeaderWidth(), 0, panel_->timelineViewportWidth(), timeRulerHeight());
}

TimelineStyleView TimelineRuler::styleView() const
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

void TimelineRuler::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    if (!panel_->hasTimeline())
        return;
    const TimelineStyleView style = styleView();
    const int rulerHeight = timeRulerHeight();
    const int headerWidth = panel_->laneHeaderWidth();
    painter.fillRect(rect(), style.canvasBackground);

    const int canvasWidth = std::max(1, panel_->timelineViewportWidth());
    const auto theme = ThemeManager::instance().snapshot();
    const bool dualAxis = panel_->hasSecondaryAxis();
    const int labelHeight = markerLane_->isVisible() ? markerLane_->y() : rulerHeight;
    auto primaryColors = theme->ui.colors;
    primaryColors.textSecondary = primaryColors.textPrimary;
    drawTimeAxisRuler(painter, primaryColors,
        QRect(headerWidth, 0, canvasWidth, labelHeight),
        panel_->timeTicks(), FontRole::Body, TimeAxisLabelAlignment::AfterTick, dualAxis);
    painter.setPen(style.canvasBorder);
    painter.drawLine(0, rulerHeight - 1, width(), rulerHeight - 1);
    painter.drawLine(headerWidth - 1, 0, headerWidth - 1, rulerHeight - 1);
    painter.drawLine(headerWidth + canvasWidth, 0, headerWidth + canvasWidth, height());
    const QRect grip = cursorHandleRect();
    if (!grip.isEmpty()) {
        const int x = headerWidth + panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
        const bool highlighted = panel_->isCursorDragActive() || panel_->isCursorHovered();
        const QColor color = highlighted ? style.music.cursor.lighter(130) : style.music.cursor;
        TimelineRenderer::drawCursorLine(painter, color, x, timeAxisRect());
        TimelineRenderer::drawCursorHandle(painter, color, grip);
    }
}

QRect TimelineRuler::cursorHandleRect() const
{
    if (!panel_->isEditCursorVisible())
        return {};
    const QRect axis = timeAxisRect();
    const int x = panel_->laneHeaderWidth()
        + panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
    if (axis.isEmpty() || x < axis.left() || x > axis.right())
        return {};
    const auto size = cursorHandleSize();
    return QRect(QPoint(x - size.width() / 2, axis.top()), size);
}

bool TimelineRuler::hitsCursor(const QPoint& position) const
{
    const QRect handle = cursorHandleRect();
    return (!handle.isEmpty() && handle.adjusted(-3, 0, 3, 2).contains(position))
        || (timeAxisRect().contains(position)
            && panel_->hitsEditCursor(position.x() - panel_->laneHeaderWidth()));
}

void TimelineRuler::updateHover(const QPoint& position)
{
    hoverPos_ = position;
    const bool hovered = hitsCursor(position);
    const bool wasHovered = panel_->isCursorHovered();
    panel_->setCursorHovered(hovered);
    if (hovered && !wasHovered)
        ToolTip::showText(mapToGlobal(position), panel_->cursorTooltip(), this);
    else if (!hovered)
        ToolTip::hideText();
    updateMouseCursor();
}

void TimelineRuler::updateMouseCursor()
{
    if (!panel_->hasTimeline()) {
        setCursor(Qt::ArrowCursor);
    } else if (dragMode_ == DragMode::Cursor) {
        setCursor(Qt::SizeHorCursor);
    } else {
        setCursor(hitsCursor(hoverPos_) ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }
}

void TimelineRuler::showCursorValue()
{
    cursorValue_->setText(panel_->cursorTooltip());
    cursorValue_->adjustSize();
    const QRect axis = timeAxisRect();
    const int x = axis.left() + panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
    const QPoint anchor = mapToGlobal(QPoint(std::clamp(x, axis.left(), axis.right()), axis.top()));
    QPoint position(anchor.x() - cursorValue_->width() / 2, anchor.y() - cursorValue_->height() - 6);
    if (const auto* screen = QGuiApplication::screenAt(anchor)) {
        const QRect available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right() - cursorValue_->width() + 1)));
        position.setY(std::max(available.top(), position.y()));
    }
    cursorValue_->move(position);
    cursorValue_->show();
}

void TimelineRuler::mousePressEvent(QMouseEvent* event)
{
    const bool onCursor = hitsCursor(event->position().toPoint());
    if (event->button() != Qt::LeftButton
        || (!timeAxisRect().contains(event->position().toPoint()) && !onCursor)) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (panel_->isEditCursorVisible()) {
        hoverPos_ = event->position().toPoint();
        setFocus();
        dragMode_ = DragMode::Cursor;
        panel_->beginCursorDrag(hoverPos_.x() - panel_->laneHeaderWidth(),
            event->modifiers().testFlag(Qt::AltModifier), !onCursor);
        updateMouseCursor();
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TimelineRuler::mouseMoveEvent(QMouseEvent* event)
{
    if (panel_->hasTimeline()) {
        hoverPos_ = event->position().toPoint();
        if (dragMode_ == DragMode::Cursor && event->buttons().testFlag(Qt::LeftButton)) {
            panel_->updateCursorDrag(hoverPos_.x() - panel_->laneHeaderWidth(),
                event->modifiers().testFlag(Qt::AltModifier));
            event->accept();
        } else {
            updateHover(hoverPos_);
            QWidget::mouseMoveEvent(event);
        }
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineRuler::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragMode_ == DragMode::Cursor) {
        hoverPos_ = event->position().toPoint();
        panel_->updateCursorDrag(hoverPos_.x() - panel_->laneHeaderWidth(),
            event->modifiers().testFlag(Qt::AltModifier));
        panel_->endCursorDrag();
        cursorValue_->hide();
        dragMode_ = DragMode::None;
        updateHover(hoverPos_);
        refresh();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineRuler::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && panel_->hasTimeline()) {
        if (hitsCursor(event->position().toPoint())) {
            dragMode_ = DragMode::None;
            panel_->editCursorValue();
            event->accept();
            return;
        }
        dragMode_ = DragMode::None;
        cursorValue_->hide();
        if (event->position().x() >= panel_->laneHeaderWidth())
            panel_->fitClips();
        refresh();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineRuler::contextMenuEvent(QContextMenuEvent* event)
{
    if (hitsCursor(event->pos())) {
        dragMode_ = DragMode::None;
        panel_->showCursorContextMenu(event->globalPos());
        event->accept();
        return;
    }
    if (markerLane_->isVisible() && timeAxisRect().contains(event->pos())) {
        QMenu* menu = markerLane_->createContextMenu(
            QPoint(event->pos().x() - panel_->laneHeaderWidth(), 0));
        if (!menu->isEmpty())
            menu->exec(event->globalPos());
        menu->deleteLater();
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

void TimelineRuler::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && dragMode_ == DragMode::Cursor) {
        panel_->endCursorDrag(true);
        updateHover(hoverPos_);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineRuler::hideEvent(QHideEvent* event)
{
    if (dragMode_ == DragMode::Cursor)
        panel_->endCursorDrag();
    dragMode_ = DragMode::None;
    hoverPos_ = QPoint(-1, -1);
    panel_->setCursorHovered(false);
    ToolTip::hideText();
    updateMouseCursor();
    cursorValue_->hide();
    QWidget::hideEvent(event);
}

void TimelineRuler::leaveEvent(QEvent* event)
{
    hoverPos_ = QPoint(-1, -1);
    panel_->setCursorHovered(false);
    ToolTip::hideText();
    updateMouseCursor();
    update();
    QWidget::leaveEvent(event);
}

}
