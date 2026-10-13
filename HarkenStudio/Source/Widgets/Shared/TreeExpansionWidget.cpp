#include "TreeExpansionWidget.h"

#include "TreeRowStyle.h"
#include "Core/ThemeManager.h"

#include <QMouseEvent>

namespace VNX {
namespace {

QRect toggleRectForItem(const QTreeWidget* tree, QTreeWidgetItem* item)
{
    const QModelIndex index = tree->indexFromItem(item, 0);
    QRect row = tree->visualRect(index);
    const int chevronExtent = ThemeManager::instance().metrics().navigationChevronExtent;
    if (item->data(0, TreeRowStyle::kBucketRole).toBool()) {
        row = row.united(tree->visualRect(index.siblingAtColumn(tree->columnCount() - 1)));
        return QRect(row.right() - chevronExtent, row.top(), chevronExtent, row.height());
    }

    return QRect(row.left() - tree->indentation(), row.top(),
                 tree->indentation(), row.height());
}
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
            treeItem->setExpanded(!treeItem->isExpanded());
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
        treeItem->setExpanded(!treeItem->isExpanded());
        event->accept();
        return;
    }

    QTreeWidget::mouseDoubleClickEvent(event);
}

}
