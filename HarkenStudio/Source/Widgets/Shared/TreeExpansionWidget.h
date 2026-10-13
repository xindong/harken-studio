#pragma once

#include <QPersistentModelIndex>
#include <QTreeWidget>

namespace VNX {

class TreeExpansionWidget : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QPersistentModelIndex togglePressIndex_;
    QPersistentModelIndex suppressToggleReleaseIndex_;
};

}
