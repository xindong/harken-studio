#include "Monitor/MonitorPerformanceChartSource.h"

#include "Monitor/MonitorPerformancePanel.h"
#include "TimeSeries/MonitorCursorModel.h"

#include <algorithm>
#include <unordered_map>

namespace VNX {
namespace {

constexpr uint64_t kStreamIds[] = {101, 102, 103, 104, 105};

} // namespace

void MonitorPerformanceChartSource::attach(MonitorPerformancePanel* panel,
                                          MonitorCursorModel* sharedCursor)
{
    panel_ = panel;
    if (panel_)
        panel_->setSharedCursor(sharedCursor);
    pushToPanel();
}

void MonitorPerformanceChartSource::apply(const TimeSeriesChartModel& model)
{
    model_ = model;
    pushToPanel();
}

TimeSeriesChartModel MonitorPerformanceChartSource::projectLiveSamples(
    const std::vector<Monitor::MonitorPerformanceLiveSample>& samples)
{
    TimeSeriesChartModel model;
    appendLiveSamples(model, samples);
    return model;
}

void MonitorPerformanceChartSource::appendLiveSamples(
    TimeSeriesChartModel& model,
    const std::vector<Monitor::MonitorPerformanceLiveSample>& samples)
{
    std::unordered_map<uint64_t, std::vector<TimeSeriesSample>> numeric;
    std::unordered_map<uint64_t, std::vector<TimeSeriesStateInterval>> gaps;
    for (const auto& sample : samples)
    {
        if (!Monitor::MonitorPerformanceLiveHost::isKnownStream(sample.streamId))
            continue;
        const auto x = static_cast<std::int64_t>(sample.executeFrame);
        if (Monitor::MonitorPerformanceLiveHost::isNumeric(sample.validityFlags))
        {
            numeric[sample.streamId].push_back(
                TimeSeriesSample{x, static_cast<double>(sample.valueBits)});
        }
        else
        {
            gaps[sample.streamId].push_back({TimeSeriesStateKind::Gap, x, x + 1});
        }
    }

    for (uint64_t id : kStreamIds)
    {
        const char* seriesId =
            Monitor::MonitorPerformanceLiveHost::seriesIdForStream(id);
        if (seriesId == nullptr)
            continue;
        auto nit = numeric.find(id);
        if (nit != numeric.end() && !nit->second.empty())
        {
            std::vector<TimeSeriesSample> merged;
            if (const auto* existing = model.series(seriesId))
                merged = existing->samples;
            merged.insert(merged.end(), nit->second.begin(), nit->second.end());
            std::sort(merged.begin(), merged.end(),
                      [](const TimeSeriesSample& a, const TimeSeriesSample& b) {
                          return a.x < b.x;
                      });
            model.updateSeries(seriesId, merged);
        }
        auto git = gaps.find(id);
        if (git != gaps.end() && !git->second.empty())
        {
            std::vector<TimeSeriesStateInterval> merged;
            if (const auto* existing = model.series(seriesId))
                merged = existing->states;
            merged.insert(merged.end(), git->second.begin(), git->second.end());
            if (model.series(seriesId) == nullptr)
                model.updateSeries(seriesId, {});
            model.setSeriesStates(seriesId, merged);
        }
    }

    std::int64_t minX = 0;
    std::int64_t maxX = 0;
    if (model.retainedRange(&minX, &maxX) && maxX >= minX)
        model.setWindow(minX, maxX + 1);
}

void MonitorPerformanceChartSource::applyLiveTick(
    const Monitor::MonitorPerformanceLiveTick& tick, bool paused)
{
    appendLiveSamples(model_, tick.samples);
    pushToPanel();
    if (!paused && tick.hasLiveFrame && panel_)
        panel_->cursorModel().advanceLive(tick.liveExecuteFrame);
}

void MonitorPerformanceChartSource::pushToPanel() const
{
    if (panel_)
        panel_->applyChartModel(model_);
}

} // namespace VNX
