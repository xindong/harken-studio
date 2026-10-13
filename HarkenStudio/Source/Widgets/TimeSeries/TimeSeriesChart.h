#pragma once

#include "TimeSeriesChartModel.h"

#include <QImage>
#include <QPoint>
#include <QWidget>

#include <optional>

namespace VNX {

struct TimeSeriesRenderColumn
{
    int column = 0;
    bool empty = true;
    bool hasState = false;
    TimeSeriesStateKind state = TimeSeriesStateKind::Gap;
    double min = 0.0;
    double max = 0.0;
};

struct TimeSeriesRenderSeries
{
    std::string id;
    std::vector<TimeSeriesRenderColumn> columns;
};

class TimeSeriesChart : public QWidget
{
    Q_OBJECT

public:
    explicit TimeSeriesChart(QWidget* parent = nullptr);

    void updateData(const TimeSeriesChartModel& model);
    const TimeSeriesChartModel& model() const { return model_; }
    const std::vector<TimeSeriesRenderSeries>& renderSeries() const { return renderSeries_; }

    bool setWindow(std::int64_t start, std::int64_t end);
    void setTestDevicePixelRatio(qreal dpr);
    void zoomAround(std::int64_t focus, double factor);
    void panBySamples(std::int64_t delta);

    QSize bufferPixelSize() const;
    qreal devicePixelRatioUsed() const { return dpr_; }
    std::int64_t xFromLogicalForTest(int logicalX) const { return xFromLogical(logicalX); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void rebuildBuffer();
    void rebuildRenderSeries();
    std::int64_t xFromLogical(int logicalX) const;

    TimeSeriesChartModel model_;
    std::vector<TimeSeriesRenderSeries> renderSeries_;
    QImage buffer_;
    qreal dpr_ = 0.0;
    bool dprOverride_ = false;
    std::optional<QPoint> hover_;
    std::optional<QPoint> dragOrigin_;
    std::int64_t dragWindowStart_ = 0;
};

} // namespace VNX
