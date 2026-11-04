#pragma once

#include "TimelineRenderer.h"

#include <QUuid>
#include <QWidget>

#include <cstdint>

class QMenu;

namespace VNX {

class TimelineEditorPanel;

class TimelineMarkerRuler final : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineMarkerRuler(TimelineEditorPanel* panel, QWidget* parent = nullptr);

    const TimelineRenderData& lastRenderData() const { return lastRenderData_; }
    bool isDragging() const;
    QUuid draggingGuid() const;
    QUuid hoveredGuid() const;
    int64_t previewPosition() const;
    QMenu* createContextMenu(const QPoint& localPos);
    void cancelInteraction();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int markerAt(const QPoint& point) const;
    TimelineStyleView styleView() const;
    TimelineRenderData buildRenderData() const;
    int64_t coordinateAtWidgetX(int widgetX) const;
    void populateCreationMenu(QMenu* menu, int64_t position);
    void updateHover(const QPoint& position);

    TimelineEditorPanel* panel_ = nullptr;
    TimelineRenderData lastRenderData_;
};

}
