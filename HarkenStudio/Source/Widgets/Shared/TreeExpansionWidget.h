#pragma once

#include <QPersistentModelIndex>
#include <QPointer>
#include <QTimer>
#include <QTreeWidget>

namespace VNX {

class TreeExpansionWidget : public QTreeWidget
{
public:
    explicit TreeExpansionWidget(QWidget* parent = nullptr);
    ~TreeExpansionWidget() override;

protected:
    bool viewportEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void stopDragNavigation();

private:
    void updateDragNavigation();
    void refreshDragPosition();
    qreal dragScrollVelocity() const;

    QPersistentModelIndex togglePressIndex_;
    QPersistentModelIndex suppressToggleReleaseIndex_;
    QPointer<const QMimeData> dragMime_;
    QMetaObject::Connection dragMimeDestroyed_;
    QPoint dragPosition_;
    Qt::DropActions dragActions_;
    Qt::MouseButtons dragButtons_;
    Qt::KeyboardModifiers dragModifiers_;
    QPersistentModelIndex dragHoverIndex_;
    QTimer dragScrollTimer_;
    QTimer dragExpandTimer_;
    qreal dragScrollRemainder_ = 0;
    bool dragWheelScrolling_ = false;
    bool refreshingDragPosition_ = false;
};

}
