#include "TimeSeriesChartModel.h"

#include <algorithm>
#include <limits>

namespace VNX {
namespace {

bool samplesStrictlyIncreasing(const std::vector<TimeSeriesSample>& samples)
{
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        if (samples[i].x < samples[i - 1].x)
            return false;
    }
    return true;
}

bool statesValid(const std::vector<TimeSeriesStateInterval>& states)
{
    std::vector<TimeSeriesStateInterval> ordered = states;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.start < b.start;
    });
    for (const auto& state : ordered)
    {
        if (state.end <= state.start)
            return false;
    }
    for (std::size_t i = 1; i < ordered.size(); ++i)
    {
        if (ordered[i].start < ordered[i - 1].end)
            return false;
    }
    return true;
}

int statePriority(TimeSeriesStateKind kind)
{
    switch (kind)
    {
    case TimeSeriesStateKind::Invalid:
        return 3;
    case TimeSeriesStateKind::WarmUp:
        return 2;
    case TimeSeriesStateKind::Gap:
        return 1;
    }
    return 0;
}

const TimeSeriesStateInterval* stateAt(
    const std::vector<TimeSeriesStateInterval>& states,
    std::int64_t x)
{
    const TimeSeriesStateInterval* best = nullptr;
    for (const auto& state : states)
    {
        if (x < state.start || x >= state.end)
            continue;
        if (best == nullptr || statePriority(state.kind) > statePriority(best->kind))
            best = &state;
    }
    return best;
}

bool isNumericAt(const TimeSeriesSeriesSnapshot& series, std::int64_t x)
{
    return stateAt(series.states, x) == nullptr;
}

std::vector<TimeSeriesSample> coalesceSamples(const std::vector<TimeSeriesSample>& samples)
{
    std::vector<TimeSeriesSample> out;
    out.reserve(samples.size());
    for (const auto& sample : samples)
    {
        if (!out.empty() && out.back().x == sample.x)
            out.back() = sample;
        else
            out.push_back(sample);
    }
    return out;
}

} // namespace

bool TimeSeriesChartModel::setRetainLimit(int limit)
{
    if (limit <= 0)
        return false;
    retainLimit_ = limit;
    for (auto& series : series_)
        trimSeries(series);
    clampWindowToRetained();
    return true;
}

bool TimeSeriesChartModel::setWindow(std::int64_t start, std::int64_t end)
{
    if (end <= start)
        return false;
    windowStart_ = start;
    windowEnd_ = end;
    return clampWindowToRetained();
}

bool TimeSeriesChartModel::updateSeries(
    const std::string& id,
    const std::vector<TimeSeriesSample>& samples)
{
    if (!samplesStrictlyIncreasing(samples))
        return false;

    auto* existing = mutableSeries(id);
    TimeSeriesSeriesSnapshot* target = existing;
    TimeSeriesSeriesSnapshot created;
    if (target == nullptr)
    {
        created.id = id;
        series_.push_back(created);
        target = &series_.back();
    }

    target->samples = coalesceSamples(samples);
    trimSeries(*target);
    clampWindowToRetained();
    return true;
}

bool TimeSeriesChartModel::setSeriesVisible(const std::string& id, bool visible)
{
    auto* series = mutableSeries(id);
    if (series == nullptr)
        return false;
    series->visible = visible;
    return true;
}

bool TimeSeriesChartModel::setSeriesStates(
    const std::string& id,
    const std::vector<TimeSeriesStateInterval>& states)
{
    if (!statesValid(states))
        return false;
    auto* series = mutableSeries(id);
    if (series == nullptr)
        return false;
    series->states = states;
    trimStates(*series);
    return true;
}

bool TimeSeriesChartModel::setSeriesThresholds(
    const std::string& id,
    const std::vector<TimeSeriesThreshold>& thresholds)
{
    auto* series = mutableSeries(id);
    if (series == nullptr)
        return false;
    series->thresholds = thresholds;
    return true;
}

bool TimeSeriesChartModel::setManualYRange(const std::string& id, double min, double max)
{
    if (!(min < max))
        return false;
    auto* series = mutableSeries(id);
    if (series == nullptr)
        return false;
    series->yMin = min;
    series->yMax = max;
    return true;
}

bool TimeSeriesChartModel::clearManualYRange(const std::string& id)
{
    auto* series = mutableSeries(id);
    if (series == nullptr)
        return false;
    series->yMin.reset();
    series->yMax.reset();
    return true;
}

const TimeSeriesSeriesSnapshot* TimeSeriesChartModel::series(const std::string& id) const
{
    for (const auto& series : series_)
    {
        if (series.id == id)
            return &series;
    }
    return nullptr;
}

bool TimeSeriesChartModel::retainedRange(std::int64_t* minX, std::int64_t* maxX) const
{
    bool any = false;
    std::int64_t lo = std::numeric_limits<std::int64_t>::max();
    std::int64_t hi = std::numeric_limits<std::int64_t>::min();
    for (const auto& series : series_)
    {
        if (series.samples.empty())
            continue;
        any = true;
        lo = std::min(lo, series.samples.front().x);
        hi = std::max(hi, series.samples.back().x);
    }
    if (!any)
        return false;
    if (minX)
        *minX = lo;
    if (maxX)
        *maxX = hi;
    return true;
}

bool TimeSeriesChartModel::clampWindowToRetained()
{
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    if (!retainedRange(&lo, &hi))
        return true;
    const std::int64_t span = windowEnd_ - windowStart_;
    if (span <= 0)
        return false;
    if (windowStart_ < lo)
    {
        windowStart_ = lo;
        windowEnd_ = lo + span;
    }
    if (windowEnd_ - 1 > hi)
    {
        windowEnd_ = hi + 1;
        windowStart_ = windowEnd_ - span;
        if (windowStart_ < lo)
            windowStart_ = lo;
    }
    return windowEnd_ > windowStart_;
}

std::optional<std::pair<double, double>> TimeSeriesChartModel::autoYRange(
    const std::string& id) const
{
    const auto* target = series(id);
    if (target == nullptr || !target->visible)
        return std::nullopt;

    bool any = false;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (const auto& sample : target->samples)
    {
        if (sample.x < windowStart_ || sample.x >= windowEnd_)
            continue;
        if (!isNumericAt(*target, sample.x))
            continue;
        any = true;
        lo = std::min(lo, sample.y);
        hi = std::max(hi, sample.y);
    }
    if (!any)
        return std::nullopt;
    if (lo == hi)
    {
        lo -= kConstantYPad;
        hi += kConstantYPad;
    }
    return std::make_pair(lo, hi);
}

std::optional<std::pair<double, double>> TimeSeriesChartModel::resolvedYRange(
    const std::string& id) const
{
    const auto* target = series(id);
    if (target == nullptr)
        return std::nullopt;
    if (target->yMin && target->yMax)
        return std::make_pair(*target->yMin, *target->yMax);
    return autoYRange(id);
}

bool TimeSeriesChartModel::isEmptyView() const
{
    for (const auto& series : series_)
    {
        if (!series.visible)
            continue;
        for (const auto& sample : series.samples)
        {
            if (sample.x >= windowStart_ && sample.x < windowEnd_ && isNumericAt(series, sample.x))
                return false;
        }
        for (const auto& state : series.states)
        {
            if (state.end > windowStart_ && state.start < windowEnd_)
                return false;
        }
    }
    return true;
}

TimeSeriesHit TimeSeriesChartModel::hitTest(std::int64_t x, const std::string& seriesId) const
{
    TimeSeriesHit hit;
    hit.seriesId = seriesId;
    hit.x = x;

    std::int64_t lo = 0;
    std::int64_t hi = 0;
    if (retainedRange(&lo, &hi) && (x < lo || x > hi))
    {
        hit.kind = TimeSeriesHitKind::HistoryUnavailable;
        return hit;
    }

    const auto* target = series(seriesId);
    if (target == nullptr)
    {
        hit.kind = TimeSeriesHitKind::Empty;
        return hit;
    }

    if (const auto* state = stateAt(target->states, x))
    {
        switch (state->kind)
        {
        case TimeSeriesStateKind::Invalid:
            hit.kind = TimeSeriesHitKind::Invalid;
            break;
        case TimeSeriesStateKind::WarmUp:
            hit.kind = TimeSeriesHitKind::WarmUp;
            break;
        case TimeSeriesStateKind::Gap:
            hit.kind = TimeSeriesHitKind::Gap;
            break;
        }
        return hit;
    }

    for (const auto& sample : target->samples)
    {
        if (sample.x == x)
        {
            hit.kind = TimeSeriesHitKind::Numeric;
            hit.y = sample.y;
            return hit;
        }
    }
    hit.kind = TimeSeriesHitKind::Empty;
    return hit;
}

TimeSeriesSeriesSnapshot* TimeSeriesChartModel::mutableSeries(const std::string& id)
{
    for (auto& series : series_)
    {
        if (series.id == id)
            return &series;
    }
    return nullptr;
}

void TimeSeriesChartModel::trimSeries(TimeSeriesSeriesSnapshot& series)
{
    if (static_cast<int>(series.samples.size()) > retainLimit_)
    {
        const auto extra = series.samples.size() - static_cast<std::size_t>(retainLimit_);
        series.samples.erase(series.samples.begin(), series.samples.begin() + static_cast<std::ptrdiff_t>(extra));
    }
    trimStates(series);
}

void TimeSeriesChartModel::trimStates(TimeSeriesSeriesSnapshot& series)
{
    if (series.samples.empty())
        return;
    const auto minX = series.samples.front().x;
    series.states.erase(
        std::remove_if(
            series.states.begin(),
            series.states.end(),
            [minX](const TimeSeriesStateInterval& state) { return state.end <= minX; }),
        series.states.end());
}

std::int64_t columnSampleX(std::int64_t windowStart, std::int64_t windowEnd, int logicalWidth, int column)
{
    const auto span = windowEnd - windowStart;
    if (logicalWidth <= 0 || span <= 0)
        return windowStart;
    const auto start = windowStart + (span * static_cast<std::int64_t>(column)) / logicalWidth;
    auto end = windowStart + (span * static_cast<std::int64_t>(column + 1)) / logicalWidth;
    if (column == logicalWidth - 1)
        end = windowEnd;
    std::int64_t sample = start;
    if (end > start)
        sample = start + (end - start) / 2;
    // Half-open window. Last empty buckets can compute start == windowEnd;
    // hover/hitTest must stay on the last retained frame, same as click lock.
    return std::clamp(sample, windowStart, windowEnd - 1);
}

std::vector<TimeSeriesEnvelopeColumn> buildTimeSeriesEnvelope(
    const TimeSeriesSeriesSnapshot& series,
    std::int64_t windowStart,
    std::int64_t windowEnd,
    int logicalWidth)
{
    std::vector<TimeSeriesEnvelopeColumn> columns;
    if (logicalWidth <= 0 || windowEnd <= windowStart)
        return columns;

    columns.resize(static_cast<std::size_t>(logicalWidth));
    const auto span = static_cast<double>(windowEnd - windowStart);
    for (int i = 0; i < logicalWidth; ++i)
    {
        auto& slot = columns[static_cast<std::size_t>(i)];
        slot.column = i;
        slot.empty = true;
        const auto probe = columnSampleX(windowStart, windowEnd, logicalWidth, i);
        if (const auto* state = stateAt(series.states, probe))
        {
            slot.hasState = true;
            slot.state = state->kind;
        }
    }

    for (const auto& sample : series.samples)
    {
        if (sample.x < windowStart || sample.x >= windowEnd)
            continue;
        if (!isNumericAt(series, sample.x))
            continue;

        const auto offset = static_cast<double>(sample.x - windowStart);
        int column = static_cast<int>((offset / span) * static_cast<double>(logicalWidth));
        if (column >= logicalWidth)
            column = logicalWidth - 1;
        if (column < 0)
            column = 0;

        auto& slot = columns[static_cast<std::size_t>(column)];
        if (slot.hasState)
            continue;
        if (slot.empty)
        {
            slot.empty = false;
            slot.min = sample.y;
            slot.max = sample.y;
        }
        else
        {
            slot.min = std::min(slot.min, sample.y);
            slot.max = std::max(slot.max, sample.y);
        }
    }
    return columns;
}

} // namespace VNX
