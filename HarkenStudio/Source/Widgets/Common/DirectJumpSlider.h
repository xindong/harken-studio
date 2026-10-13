#pragma once

#include <QSlider>

class QMouseEvent;

namespace VNX {

class DirectJumpSlider final : public QSlider
{
public:
    explicit DirectJumpSlider(Qt::Orientation orientation,
                              QWidget* parent = nullptr);
    QRect handleRect() const;

protected:
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    int positionFromPoint(const QPointF& point) const;

    bool directDrag_ = false;
    int dragOffset_ = 0;
};

}
