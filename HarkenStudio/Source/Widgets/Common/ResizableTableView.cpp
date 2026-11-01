#include "ResizableTableView.h"

#include "Core/ThemeManager.h"

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>

namespace VNX {
namespace {

class TableBoundary : public QWidget
{
public:
    TableBoundary(QWidget* parent, ThemeManager* theme, bool column, int section)
        : QWidget(parent), theme_(theme), column_(column)
    {
        setProperty("tableResizeColumn", column);
        setProperty("tableResizeSection", section);
        setCursor(column ? Qt::SplitHCursor : Qt::SplitVCursor);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setPen(underMouse() ? theme_->colors().accent : theme_->colors().borderSecondary);
        if (column_) painter.drawLine(width() / 2, 0, width() / 2, height());
        else painter.drawLine(0, height() - 1, width(), height() - 1);
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    ThemeManager* theme_;
    bool column_;
};

}

ResizableTableView::ResizableTableView(QWidget* parent, ThemeManager* theme)
    : QTableView(parent), theme_(theme ? theme : &ThemeManager::instance())
{
    setProperty("compositeTable", true);
    setShowGrid(false);
    horizontalHeader()->hide();
    verticalHeader()->hide();
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    qApp->installEventFilter(this);
    connect(horizontalHeader(), &QHeaderView::sectionResized, this, [this] { scheduleHandles(); });
    connect(verticalHeader(), &QHeaderView::sectionResized, this, [this] { scheduleHandles(); });
    connect(theme_, &ThemeManager::themeChanged, this, [this] { scheduleHandles(); });
}

void ResizableTableView::setMinimumRowHeight(int height)
{
    minimumRowHeight_ = height;
    verticalHeader()->setMinimumSectionSize(height);
}

void ResizableTableView::updateGeometries()
{
    QTableView::updateGeometries();
    scheduleHandles();
}

void ResizableTableView::scrollContentsBy(int dx, int dy)
{
    QTableView::scrollContentsBy(dx, dy);
    scheduleHandles();
}

void ResizableTableView::scheduleHandles()
{
    if (handlesPending_) return;
    handlesPending_ = true;
    QTimer::singleShot(0, this, [this] { handlesPending_ = false; refreshHandles(); });
}

void ResizableTableView::refreshHandles()
{
    if (!model()) return;
    const int columns = model()->columnCount();
    const int rows = model()->rowCount();
    const int count = std::max(0, columns - 1) + rows;
    if (handles_.size() != count) {
        finishResize(true);
        for (auto& handle : handles_) delete handle;
        handles_.clear();
        for (int column = 0; column + 1 < columns; ++column)
            handles_.append(new TableBoundary(viewport(), theme_, true, column));
        for (int row = 0; row < rows; ++row)
            handles_.append(new TableBoundary(viewport(), theme_, false, row));
    }
    const int radius = std::max(2, theme_->metrics().table.resizeHandleRadius);
    const int contentHeight = rows > 0
        ? std::clamp(rowViewportPosition(rows - 1) + rowHeight(rows - 1), 0, viewport()->height()) : 0;
    for (const auto& handle : handles_) {
        const int section = handle->property("tableResizeSection").toInt();
        if (handle->property("tableResizeColumn").toBool()) {
            const auto numberColumn = property("rowNumberColumn");
            const bool fixed = numberColumn.isValid() && section == numberColumn.toInt();
            handle->setAttribute(Qt::WA_TransparentForMouseEvents, fixed);
            handle->setCursor(fixed ? Qt::ArrowCursor : Qt::SplitHCursor);
            const int x = columnViewportPosition(section) + columnWidth(section);
            handle->setGeometry(x - radius, 0, radius * 2, contentHeight);
            handle->setVisible(contentHeight > 0);
        } else {
            const int bottom = rowViewportPosition(section) + rowHeight(section);
            handle->setGeometry(0, bottom - radius * 2, viewport()->width(), radius * 2);
            handle->setVisible(activeHandle_ == handle || (bottom > 0 && bottom <= viewport()->height()));
        }
        handle->raise();
        handle->update();
    }
}

void ResizableTableView::finishResize(bool cancel)
{
    if (!activeHandle_) return;
    auto* handle = activeHandle_.data();
    const int section = handle->property("tableResizeSection").toInt();
    const bool column = handle->property("tableResizeColumn").toBool();
    activeHandle_.clear();
    handle->releaseMouse();
    if (cancel) {
        if (column) setColumnWidth(section, initialWidths_.value(section));
        else setRowHeight(section, initialHeight_);
    } else if (!column) emit rowHeightAdjusted(section, rowHeight(section));
}

bool ResizableTableView::eventFilter(QObject* watched, QEvent* event)
{
    if (activeHandle_ && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        finishResize(true);
        return true;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || !viewport()->isAncestorOf(widget)) return QTableView::eventFilter(watched, event);
    const bool handle = widget->property("tableResizeSection").isValid();
    if (handle && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) return false;
        activeHandle_ = widget;
        dragOrigin_ = mouse->globalPosition().toPoint();
        initialWidths_.clear();
        for (int i = 0; i < model()->columnCount(); ++i) initialWidths_.append(columnWidth(i));
        initialHeight_ = rowHeight(widget->property("tableResizeSection").toInt());
        widget->grabMouse();
        return true;
    }
    if (activeHandle_ == widget && event->type() == QEvent::MouseMove) {
        const auto delta = static_cast<QMouseEvent*>(event)->globalPosition().toPoint() - dragOrigin_;
        const int section = widget->property("tableResizeSection").toInt();
        if (widget->property("tableResizeColumn").toBool())
            setColumnWidth(section, std::max(theme_->metrics().table.minimumColumnWidth,
                initialWidths_[section] + delta.x()));
        else setRowHeight(section, std::max(minimumRowHeight_, initialHeight_ + delta.y()));
        refreshHandles();
        return true;
    }
    if (activeHandle_ == widget && event->type() == QEvent::MouseButtonRelease) {
        finishResize(false);
        return true;
    }
    if (!handle && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const auto index = indexAt(viewport()->mapFromGlobal(mouse->globalPosition().toPoint()));
        if (mouse->button() == Qt::LeftButton && index.isValid())
            selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    return QTableView::eventFilter(watched, event);
}

}
