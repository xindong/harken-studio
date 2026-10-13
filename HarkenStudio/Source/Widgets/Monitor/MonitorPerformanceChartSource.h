#pragma once

#include "Monitor/MonitorPerformanceLiveHost.h"
#include "TimeSeries/TimeSeriesChartModel.h"

namespace VNX {

class MonitorCursorModel;
class MonitorPerformancePanel;

class MonitorPerformanceChartSource
{
public:
    void attach(MonitorPerformancePanel* panel, MonitorCursorModel* sharedCursor = nullptr);
    void apply(const TimeSeriesChartModel& model);
    void applyLiveTick(const Monitor::MonitorPerformanceLiveTick& tick, bool paused);
    const TimeSeriesChartModel& model() const { return model_; }
    MonitorPerformancePanel* panel() const { return panel_; }

    static TimeSeriesChartModel projectLiveSamples(
        const std::vector<Monitor::MonitorPerformanceLiveSample>& samples);
    static void appendLiveSamples(
        TimeSeriesChartModel& model,
        const std::vector<Monitor::MonitorPerformanceLiveSample>& samples);

private:
    void pushToPanel() const;

    TimeSeriesChartModel model_;
    MonitorPerformancePanel* panel_ = nullptr;
};

} // namespace VNX
