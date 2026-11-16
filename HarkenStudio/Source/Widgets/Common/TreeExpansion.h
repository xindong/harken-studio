#pragma once

#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QTreeWidget>

namespace VNX::TreeExpansion {

inline void setExpanded(QTreeWidgetItem* item, bool expanded, bool recursive = false)
{
    if (!item) return;
    item->setExpanded(expanded);
    if (recursive) {
        for (int i = 0; i < item->childCount(); ++i)
            setExpanded(item->child(i), expanded, true);
    }
}

inline void toggle(QTreeWidgetItem* item, Qt::KeyboardModifiers modifiers)
{
    if (!item) return;
    setExpanded(item, !item->isExpanded(), modifiers.testFlag(Qt::ControlModifier));
}

template<typename Handler>
void handleNativeMouseEvent(QTreeWidget& tree, QMouseEvent* event, Handler&& handler)
{
    if (event->button() != Qt::LeftButton || !event->modifiers().testFlag(Qt::ControlModifier)) {
        handler();
        return;
    }
    const QPersistentModelIndex index = tree.indexFromItem(tree.itemAt(event->position().toPoint()), 0);
    const bool wasExpanded = tree.isExpanded(index);
    handler();
    if (index.isValid() && tree.isExpanded(index) != wasExpanded)
        setExpanded(tree.itemFromIndex(index), !wasExpanded, true);
}

}
