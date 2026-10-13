#pragma once

#include <QWidget>

class QMenu;
class QVBoxLayout;
class QUuid;

namespace VNX {

class TimelineEditorPanel;
class TableWidget;

class TimelineHeaderColumn final : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineHeaderColumn(TimelineEditorPanel* panel, QWidget* parent = nullptr);

    QVBoxLayout* laneLayout() const { return laneLayout_; }
    QMenu* createLaneMenu(int laneIndex);
    void rebuildRows();
    void refreshRows();
    QRect laneRect(int index) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void renameLane(const QUuid& guid);

    TimelineEditorPanel* panel_ = nullptr;
    TableWidget* table_ = nullptr;
    QVBoxLayout* laneLayout_ = nullptr;
};

}
