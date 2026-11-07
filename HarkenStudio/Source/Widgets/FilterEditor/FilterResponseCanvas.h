#pragma once

#include "Core/FilterSemantics.h"
#include "Widgets/CurveEditor/CurvePlotGeometry.h"

#include <QWidget>

namespace VNX {

class FilterResponseCanvas final : public QWidget
{
    Q_OBJECT

public:
    explicit FilterResponseCanvas(QWidget* parent = nullptr);
    void setFilter(const FilterState& filter, double sampleRate = 48000.0);
    const FilterState& filter() const { return filter_; }
    void setReadOnly(bool readOnly);
    void cancelEdit();
    void commitEdit();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    CurveRenderData renderData() const;

signals:
    void editStarted();
    void sectionEdited(FilterKind kind, double frequency, double q, double gain);
    void editFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    CurvePlotGeometry geometry() const;
    QPointF sectionPoint(int index, const CurvePlotGeometry& plot) const;
    int sectionAt(const QPointF& position) const;
    void editSection(int index, const FilterSection& section);
    void finishDrag(bool cancel);

    FilterState filter_;
    double sampleRate_ = 48000.0;
    CurvePlotProfile profile_;
    FilterSection dragStart_;
    QPointF hoverPosition_;
    int hovered_ = -1;
    int dragged_ = -1;
    bool hoverActive_ = false;
    bool readOnly_ = false;
};

}
