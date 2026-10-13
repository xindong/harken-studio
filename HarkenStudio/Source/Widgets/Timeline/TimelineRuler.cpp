#include "TimelineRuler.h"

#include "TimelineEditorPanel.h"
#include "TimelineRenderer.h"
#include "Core/FontManager.h"
#include "Core/ThemeManager.h"

#include <QGuiApplication>
#include <QContextMenuEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPolygon>
#include <QScreen>

#include <algorithm>

namespace VNX {

namespace {

constexpr int kCursorHandleHeight = 10;

}

TimelineRuler::TimelineRuler(TimelineEditorPanel* panel, QWidget* parent)
    : QWidget(parent)
    , panel_(panel)
{
    setObjectName(QStringLiteral("timelineRuler"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
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
    setFixedHeight(timeRulerHeight());
    if (!active) {
        dragMode_ = DragMode::None;
        cursorValue_->hide();
    }
    const auto theme = ThemeManager::instance().snapshot();
    QPalette palette = cursorValue_->palette();
    palette.setColor(QPalette::Window, theme->ui.colors.bgSecondary);
    palette.setColor(QPalette::WindowText, theme->ui.colors.textPrimary);
    cursorValue_->setPalette(palette);
    if (panel_->isCursorDragActive())
        showCursorValue();
    else
        cursorValue_->hide();
    updateMouseCursor();
    update();
}

int TimelineRuler::timeRulerHeight() const
{
    if (!panel_->hasTimeline())
        return panel_->timelineMetrics().rulerHeight;
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Axis), this);
    return std::max(metrics.height() + 5, ThemeManager::instance().metrics().navigationChevronExtent);
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
    if (panel_->hasTimeline())
        style.music.cursor = snapshot->ui.colors.success;
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
    if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Track) {
        painter.setFont(FontManager::instance().getFont(FontRole::Axis));
        painter.setPen(style.placeholderText);
        painter.drawText(QRect(0, 0, headerWidth, rulerHeight), Qt::AlignCenter, tr("dB\\s"));
    }
    const auto ticks = panel_->timeTicks();
    drawTimeAxisRuler(painter, theme->ui.colors,
        QRect(headerWidth, 0, canvasWidth, rulerHeight), ticks);
    painter.setPen(style.canvasBorder);
    painter.drawLine(0, rulerHeight - 1, width(), rulerHeight - 1);
    painter.drawLine(headerWidth + canvasWidth, 0, headerWidth + canvasWidth, height());
    const QRect grip = cursorHandleRect();
    if (!grip.isEmpty()) {
        painter.setClipRect(timeAxisRect());
        const int x = headerWidth + panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
        const bool highlighted = panel_->isCursorDragActive()
            || grip.adjusted(-3, 0, 3, 2).contains(hoverPos_);
        const QColor color = highlighted ? style.music.cursor.lighter(130) : style.music.cursor;
        TimelineRenderer::drawVerticalLine(painter, style.music.cursor, x, timeAxisRect());
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(QPolygon{grip.topLeft(), grip.topRight(), QPoint(x, grip.bottom())});
    }
}

QRect TimelineRuler::cursorHandleRect() const
{
    const QRect axis = timeAxisRect();
    const int x = panel_->laneHeaderWidth()
        + panel_->viewportPixelAtCoordinate(0, panel_->editCursor());
    if (axis.isEmpty() || x < axis.left() || x > axis.right())
        return {};
    return QRect(x - 5, axis.top(), 11, kCursorHandleHeight);
}

void TimelineRuler::updateMouseCursor()
{
    if (!panel_->hasTimeline()) {
        setCursor(Qt::ArrowCursor);
    } else if (dragMode_ == DragMode::Cursor) {
        setCursor(Qt::SizeHorCursor);
    } else {
        const QRect handle = cursorHandleRect();
        if (timeAxisRect().contains(hoverPos_))
            setCursor(!handle.isEmpty() && handle.adjusted(-3, 0, 3, 2).contains(hoverPos_)
                ? Qt::SizeHorCursor : Qt::ArrowCursor);
        else
            setCursor(Qt::ArrowCursor);
    }
}

void TimelineRuler::showCursorValue()
{
    cursorValue_->setText(tr("Cursor: %1").arg(panel_->formatCoordinate(0, panel_->editCursor())));
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
    if (event->button() != Qt::LeftButton
        || !rect().contains(event->position().toPoint())) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (panel_->hasTimeline()) {
        hoverPos_ = event->position().toPoint();
        if (event->position().x() < panel_->laneHeaderWidth()
            || event->position().x() >= panel_->laneHeaderWidth() + panel_->timelineViewportWidth()) {
            event->ignore();
            return;
        }
        setFocus();
        dragMode_ = DragMode::Cursor;
        const QRect handle = cursorHandleRect();
        const bool onHandle = !handle.isEmpty() && handle.adjusted(-3, 0, 3, 2).contains(hoverPos_);
        panel_->beginCursorDrag(hoverPos_.x() - panel_->laneHeaderWidth(),
            event->modifiers().testFlag(Qt::AltModifier), !onHandle);
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
            updateMouseCursor();
            update();
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
        if (timeAxisRect().contains(event->position().toPoint())) {
            const QRect handle = cursorHandleRect();
            if (!handle.isEmpty() && handle.adjusted(-3, 0, 3, 2).contains(event->position().toPoint())) {
                dragMode_ = DragMode::None;
                panel_->editCursorValue();
                event->accept();
                return;
            }
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
    const QRect handle = cursorHandleRect();
    if (timeAxisRect().contains(event->pos())
        && !handle.isEmpty() && handle.adjusted(-3, 0, 3, 2).contains(event->pos())) {
        dragMode_ = DragMode::None;
        panel_->showCursorContextMenu(event->globalPos());
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

void TimelineRuler::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && dragMode_ == DragMode::Cursor) {
        panel_->endCursorDrag(true);
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
    updateMouseCursor();
    cursorValue_->hide();
    QWidget::hideEvent(event);
}

void TimelineRuler::leaveEvent(QEvent* event)
{
    hoverPos_ = QPoint(-1, -1);
    updateMouseCursor();
    update();
    QWidget::leaveEvent(event);
}

}
