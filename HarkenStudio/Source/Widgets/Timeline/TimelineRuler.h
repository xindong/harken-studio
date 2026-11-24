#pragma once

#include "TimelineRenderer.h"

#include <QWidget>

class QLabel;

namespace harken::studio {

class TimelineEditorPanel;

class TimelineRuler final : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineRuler(TimelineEditorPanel* panel, QWidget* parent = nullptr);

    void refresh();
    int timeRulerHeight() const;
    QRect timeAxisRect() const;
    static QSize cursorHandleSize();
    QRect cursorHandleRect() const;

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
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class DragMode { None, Cursor };

    TimelineStyleView styleView() const;
    void updateMouseCursor();
    bool hitsCursor(const QPoint& position) const;
    void updateHover(const QPoint& position);
    void showCursorValue();

    TimelineEditorPanel* panel_ = nullptr;
    DragMode dragMode_ = DragMode::None;
    QPoint hoverPos_ = QPoint(-1, -1);
    QLabel* cursorValue_ = nullptr;
};

}
