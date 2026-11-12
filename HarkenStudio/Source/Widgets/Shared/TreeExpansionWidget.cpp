#include "TreeExpansionWidget.h"

#include "TreeRowStyle.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/TreeExpansion.h"

#include <QApplication>
#include <QDragMoveEvent>
#include <QHideEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#include <QWindow>
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace VNX {
namespace {

#ifdef Q_OS_WIN
QPointer<TreeExpansionWidget> nativeDragTree;
HHOOK nativeDragWheelHook = nullptr;

LRESULT CALLBACK forwardDragWheel(int code, WPARAM removed, LPARAM message)
{
    if (code == HC_ACTION && removed == PM_REMOVE && nativeDragTree) {
        auto* msg = reinterpret_cast<MSG*>(message);
        if (msg->message == WM_MOUSEWHEEL || msg->message == WM_MOUSEHWHEEL) {
            auto* viewport = nativeDragTree->viewport();
            auto* window = viewport->window();
            const auto hwnd = reinterpret_cast<HWND>(window->winId());
            POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
            if (ScreenToClient(hwnd, &point)) {
                const QPoint position = (QPointF(point.x, point.y)
                    / window->windowHandle()->devicePixelRatio()).toPoint();
                auto* hovered = QApplication::widgetAt(window->mapToGlobal(position));
                if (hovered == viewport || viewport->isAncestorOf(hovered)) {
                    const MSG wheel = *msg;
                    msg->message = WM_NULL;
                    SendMessageW(hwnd, wheel.message, wheel.wParam, wheel.lParam);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, removed, message);
}

void startNativeDragWheel(TreeExpansionWidget* tree)
{
    nativeDragTree = tree;
    if (!nativeDragWheelHook)
        nativeDragWheelHook = SetWindowsHookExW(WH_GETMESSAGE, forwardDragWheel, nullptr,
                                              GetCurrentThreadId());
}

void stopNativeDragWheel(TreeExpansionWidget* tree)
{
    if (nativeDragTree != tree) return;
    if (nativeDragWheelHook) UnhookWindowsHookEx(nativeDragWheelHook);
    nativeDragWheelHook = nullptr;
    nativeDragTree.clear();
}
#endif

QRect toggleRectForItem(const QTreeWidget* tree, QTreeWidgetItem* item)
{
    const QModelIndex index = tree->indexFromItem(item, 0);
    QRect row = tree->visualRect(index);
    const int chevronExtent = ThemeManager::instance().metrics().iconExtent;
    if (item->data(0, TreeRowStyle::kBucketRole).toBool()) {
        row = row.united(tree->visualRect(index.siblingAtColumn(tree->columnCount() - 1)));
        return QRect(row.right() - chevronExtent, row.top(), chevronExtent, row.height());
    }

    return QRect(row.left() - tree->indentation(), row.top(),
                 tree->indentation(), row.height());
}
}

TreeExpansionWidget::TreeExpansionWidget(QWidget* parent)
    : QTreeWidget(parent)
{
    dragScrollTimer_.setInterval(16);
    connect(&dragScrollTimer_, &QTimer::timeout, this, [this] {
        if (!dragMime_ || !viewport()->rect().contains(dragPosition_)) {
            stopDragNavigation();
            return;
        }
        const qreal velocity = dragScrollVelocity();
        if (velocity == 0) {
            updateDragNavigation();
            return;
        }
        dragScrollRemainder_ += velocity * dragScrollTimer_.interval() / 1000.0;
        const int delta = static_cast<int>(dragScrollRemainder_);
        dragScrollRemainder_ -= delta;
        if (delta != 0)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
    });
    dragExpandTimer_.setSingleShot(true);
    dragExpandTimer_.setInterval(600);
    connect(&dragExpandTimer_, &QTimer::timeout, this, [this] {
        if (!dragMime_ || !dragHoverIndex_.isValid()) return;
        const auto index = dragHoverIndex_;
        dragHoverIndex_ = QModelIndex();
        expand(index);
        refreshDragPosition();
    });
}

TreeExpansionWidget::~TreeExpansionWidget()
{
    stopDragNavigation();
}

bool TreeExpansionWidget::viewportEvent(QEvent* event)
{
    const auto type = event->type();
    if (type == QEvent::DragLeave || type == QEvent::Drop)
        stopDragNavigation();
    const bool handled = QTreeWidget::viewportEvent(event);
    if (type == QEvent::DragEnter || type == QEvent::DragMove) {
        const auto* drag = static_cast<QDragMoveEvent*>(event);
        if (type == QEvent::DragEnter)
            stopDragNavigation();
        if (dragMime_ || drag->isAccepted()) {
            if (dragMime_ != drag->mimeData()) {
                disconnect(dragMimeDestroyed_);
                dragMime_ = drag->mimeData();
                dragMimeDestroyed_ = connect(dragMime_, &QObject::destroyed,
                                            this, &TreeExpansionWidget::stopDragNavigation);
#ifdef Q_OS_WIN
                startNativeDragWheel(this);
#endif
            }
            if (dragPosition_ != drag->position().toPoint())
                dragWheelScrolling_ = false;
            dragPosition_ = drag->position().toPoint();
            dragActions_ = drag->possibleActions();
            dragButtons_ = drag->buttons();
            dragModifiers_ = drag->modifiers();
            updateDragNavigation();
        }
    }
    return handled;
}

void TreeExpansionWidget::hideEvent(QHideEvent* event)
{
    stopDragNavigation();
    QTreeWidget::hideEvent(event);
}

void TreeExpansionWidget::scrollContentsBy(int dx, int dy)
{
    QTreeWidget::scrollContentsBy(dx, dy);
    refreshDragPosition();
}

void TreeExpansionWidget::wheelEvent(QWheelEvent* event)
{
    if (dragMime_) {
        dragWheelScrolling_ = true;
        dragPosition_ = event->position().toPoint();
        dragButtons_ = event->buttons();
        dragModifiers_ = event->modifiers();
        dragScrollTimer_.stop();
        dragScrollRemainder_ = 0;
        dragExpandTimer_.stop();
        dragHoverIndex_ = QModelIndex();
    }
    QTreeWidget::wheelEvent(event);
    refreshDragPosition();
}

void TreeExpansionWidget::stopDragNavigation()
{
#ifdef Q_OS_WIN
    stopNativeDragWheel(this);
#endif
    dragScrollTimer_.stop();
    dragExpandTimer_.stop();
    disconnect(dragMimeDestroyed_);
    dragMime_.clear();
    dragHoverIndex_ = QModelIndex();
    dragScrollRemainder_ = 0;
    dragWheelScrolling_ = false;
}

qreal TreeExpansionWidget::dragScrollVelocity() const
{
    if (dragWheelScrolling_) return 0;
    const QRect area = viewport()->rect();
    if (!area.contains(dragPosition_)) return 0;
    const int rowHeight = ThemeManager::instance().metrics().layout.rowHeight;
    const int margin = qMin(rowHeight, area.height() / 2);
    if (margin <= 0) return 0;
    const auto* scroll = verticalScrollBar();
    qreal penetration = 0;
    int direction = 0;
    if (dragPosition_.y() - area.top() < margin && scroll->value() > scroll->minimum()) {
        penetration = 1.0 - qreal(dragPosition_.y() - area.top()) / margin;
        direction = -1;
    } else if (area.bottom() - dragPosition_.y() < margin && scroll->value() < scroll->maximum()) {
        penetration = 1.0 - qreal(area.bottom() - dragPosition_.y()) / margin;
        direction = 1;
    }
    const qreal unit = verticalScrollMode() == QAbstractItemView::ScrollPerPixel ? rowHeight : 1;
    return direction * unit * (2.0 + 18.0 * penetration * penetration);
}

void TreeExpansionWidget::updateDragNavigation()
{
    if (!dragMime_ || !viewport()->rect().contains(dragPosition_)) {
        stopDragNavigation();
        return;
    }
    const qreal velocity = dragScrollVelocity();
    if (velocity != 0) {
        if (dragScrollRemainder_ * velocity < 0) dragScrollRemainder_ = 0;
        if (!dragScrollTimer_.isActive()) dragScrollTimer_.start();
        dragExpandTimer_.stop();
        dragHoverIndex_ = QModelIndex();
        return;
    }
    dragScrollTimer_.stop();
    dragScrollRemainder_ = 0;
    auto* item = itemAt(dragPosition_);
    const QPersistentModelIndex index = item && item->childCount() > 0
        && !item->isExpanded() && !item->isDisabled() ? indexFromItem(item) : QModelIndex();
    if (index != dragHoverIndex_) {
        dragExpandTimer_.stop();
        dragHoverIndex_ = index;
        if (index.isValid()) dragExpandTimer_.start();
    }
}

void TreeExpansionWidget::refreshDragPosition()
{
    if (!dragMime_ || refreshingDragPosition_) return;
    const QScopedValueRollback guard(refreshingDragPosition_, true);
    QDragMoveEvent move(dragPosition_, dragActions_, dragMime_, dragButtons_, dragModifiers_);
    dragMoveEvent(&move);
    updateDragNavigation();
}

void TreeExpansionWidget::mousePressEvent(QMouseEvent* event)
{
    togglePressIndex_ = QModelIndex();
    suppressToggleReleaseIndex_ = QModelIndex();
    if (event->button() == Qt::LeftButton) {
        QTreeWidgetItem* treeItem = itemAt(event->position().toPoint());
        if (treeItem && treeItem->childCount() > 0
            && toggleRectForItem(this, treeItem).contains(event->position().toPoint())) {
            togglePressIndex_ = indexFromItem(treeItem, 0);
            event->accept();
            return;
        }
    }
    QTreeWidget::mousePressEvent(event);
}

void TreeExpansionWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        QTreeWidgetItem* treeItem = itemAt(event->position().toPoint());
        const QModelIndex index = indexFromItem(treeItem, 0);
        if (index.isValid() && suppressToggleReleaseIndex_ == index) {
            suppressToggleReleaseIndex_ = QModelIndex();
            event->accept();
            return;
        }
        const bool toggleClick = togglePressIndex_.isValid() && togglePressIndex_ == index;
        togglePressIndex_ = QModelIndex();
        if (toggleClick && treeItem && treeItem->childCount() > 0
            && toggleRectForItem(this, treeItem).contains(event->position().toPoint())) {
            TreeExpansion::toggle(treeItem, event->modifiers());
            event->accept();
            return;
        }
    }
    QTreeWidget::mouseReleaseEvent(event);
}

void TreeExpansionWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    QTreeWidgetItem* treeItem = itemAt(event->position().toPoint());
    if (!treeItem) {
        QTreeWidget::mouseDoubleClickEvent(event);
        return;
    }

    const QPoint pos = event->position().toPoint();

    if (event->button() == Qt::LeftButton && treeItem->childCount() > 0
        && toggleRectForItem(this, treeItem).contains(pos)) {
        togglePressIndex_ = indexFromItem(treeItem, 0);
        suppressToggleReleaseIndex_ = togglePressIndex_;
        TreeExpansion::toggle(treeItem, event->modifiers());
        event->accept();
        return;
    }

    TreeExpansion::handleNativeMouseEvent(*this, event, [this, event] {
        QTreeWidget::mouseDoubleClickEvent(event);
    });
}

}
