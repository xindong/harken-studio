#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VNX {

enum class TimeSeriesStateKind
{
    Gap,
    Invalid,
    WarmUp,
};

struct TimeSeriesSample
{
    std::int64_t x = 0;
    double y = 0.0;
};

struct TimeSeriesStateInterval
{
    TimeSeriesStateKind kind = TimeSeriesStateKind::Gap;
    std::int64_t start = 0;
    std::int64_t end = 0;
};

struct TimeSeriesThreshold
{
    std::string id;
    double value = 0.0;
};

struct TimeSeriesEnvelopeColumn
{
    int column = 0;
    bool empty = true;
    bool hasState = false;
    TimeSeriesStateKind state = TimeSeriesStateKind::Gap;
    double min = 0.0;
    double max = 0.0;
};

enum class TimeSeriesHitKind
{
    Numeric,
    Gap,
    Invalid,
    WarmUp,
    HistoryUnavailable,
    Empty,
};

struct TimeSeriesHit
{
    TimeSeriesHitKind kind = TimeSeriesHitKind::Empty;
    std::string seriesId;
    std::int64_t x = 0;
    double y = 0.0;
};

struct TimeSeriesSeriesSnapshot
{
    std::string id;
    bool visible = true;
    std::optional<double> yMin;
    std::optional<double> yMax;
    std::vector<TimeSeriesSample> samples;
    std::vector<TimeSeriesStateInterval> states;
    std::vector<TimeSeriesThreshold> thresholds;
};

class TimeSeriesChartModel
{
public:
    static constexpr int kDefaultRetainLimit = 8192;
    static constexpr double kConstantYPad = 1.0;

    bool setRetainLimit(int limit);
    int retainLimit() const { return retainLimit_; }

    bool setWindow(std::int64_t start, std::int64_t end);
    std::int64_t windowStart() const { return windowStart_; }
    std::int64_t windowEnd() const { return windowEnd_; }

    bool updateSeries(const std::string& id, const std::vector<TimeSeriesSample>& samples);
    bool setSeriesVisible(const std::string& id, bool visible);
    bool setSeriesStates(const std::string& id, const std::vector<TimeSeriesStateInterval>& states);
    bool setSeriesThresholds(const std::string& id, const std::vector<TimeSeriesThreshold>& thresholds);
    bool setManualYRange(const std::string& id, double min, double max);
    bool clearManualYRange(const std::string& id);

    const TimeSeriesSeriesSnapshot* series(const std::string& id) const;
    const std::vector<TimeSeriesSeriesSnapshot>& series() const { return series_; }

    bool retainedRange(std::int64_t* minX, std::int64_t* maxX) const;
    bool clampWindowToRetained();
    std::optional<std::pair<double, double>> autoYRange(const std::string& id) const;
    std::optional<std::pair<double, double>> resolvedYRange(const std::string& id) const;
    bool isEmptyView() const;
    TimeSeriesHit hitTest(std::int64_t x, const std::string& seriesId) const;

private:
    TimeSeriesSeriesSnapshot* mutableSeries(const std::string& id);
    void trimSeries(TimeSeriesSeriesSnapshot& series);
    void trimStates(TimeSeriesSeriesSnapshot& series);

    int retainLimit_ = kDefaultRetainLimit;
    std::int64_t windowStart_ = 0;
    std::int64_t windowEnd_ = 1;
    std::vector<TimeSeriesSeriesSnapshot> series_;
};

std::int64_t columnSampleX(
    std::int64_t windowStart,
    std::int64_t windowEnd,
    int logicalWidth,
    int column);

std::vector<TimeSeriesEnvelopeColumn> buildTimeSeriesEnvelope(
    const TimeSeriesSeriesSnapshot& series,
    std::int64_t windowStart,
    std::int64_t windowEnd,
    int logicalWidth);

} // namespace VNX
