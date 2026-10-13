#pragma once

#include "TimelineRenderer.h"

#include <QUuid>
#include <QWidget>

#include <cstdint>

class QLineEdit;
class QMenu;

namespace VNX {

class TimelineEditorPanel;

class TimelineMarkerRuler final : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineMarkerRuler(TimelineEditorPanel* panel, QWidget* parent = nullptr);

    const TimelineRenderData& lastRenderData() const { return lastRenderData_; }
    bool isDragging() const { return dragging_; }
    QUuid draggingGuid() const { return activeGuid_; }
    int64_t previewPosition() const { return previewPosition_; }
    QMenu* createContextMenu(const QPoint& localPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    int markerAt(const QPoint& point) const;
    TimelineStyleView styleView() const;
    TimelineRenderData buildRenderData() const;
    int canvasX(int widgetX) const;
    int64_t coordinateAtWidgetX(int widgetX) const;
    void beginRename(const QUuid& guid);
    void commitRename();
    void cancelRename();
    void populateCreationMenu(QMenu* menu, int64_t position);

    TimelineEditorPanel* panel_ = nullptr;
    TimelineRenderData lastRenderData_;
    QUuid activeGuid_;
    int64_t previewPosition_ = 0;
    QPoint pressPos_;
    bool dragging_ = false;
    bool dragArmed_ = false;
    QLineEdit* nameEditor_ = nullptr;
    QUuid renamingGuid_;
};

}
