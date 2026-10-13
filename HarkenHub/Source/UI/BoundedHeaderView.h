#pragma once

#include <QHeaderView>

class QMouseEvent;
class QResizeEvent;
class QShowEvent;

namespace VNH {

class BoundedHeaderView final : public QHeaderView
{
public:
    explicit BoundedHeaderView(Qt::Orientation orientation,
                               QWidget* parent = nullptr);

    void setFillSection(int logicalIndex);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void constrainSectionsToViewport();

    int fillSection_ = -1;
    int dragSection_ = -1;
    int dragStartPosition_ = 0;
    int dragStartSize_ = 0;
    int dragMaximumSize_ = 0;
};

}
