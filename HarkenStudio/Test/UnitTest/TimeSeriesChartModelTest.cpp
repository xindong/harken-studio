#include "Widgets/TimeSeries/TimeSeriesChartModel.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using VNX::TimeSeriesChartModel;
using VNX::TimeSeriesHitKind;
using VNX::TimeSeriesSample;
using VNX::TimeSeriesStateInterval;
using VNX::TimeSeriesStateKind;
using VNX::TimeSeriesThreshold;
using VNX::buildTimeSeriesEnvelope;

namespace {

std::vector<TimeSeriesSample> samples(std::initializer_list<std::pair<std::int64_t, double>> in)
{
    std::vector<TimeSeriesSample> out;
    out.reserve(in.size());
    for (const auto& item : in)
        out.push_back(TimeSeriesSample{item.first, item.second});
    return out;
}

} // namespace

TEST(TimeSeriesChartModel, RejectsOutOfOrderSamplesAndKeepsPrevious)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 1.0}, {1, 2.0}})));
    EXPECT_FALSE(model.updateSeries("cpu", samples({{2, 3.0}, {1, 4.0}})));
    const auto* series = model.series("cpu");
    ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->samples.size(), 2u);
    EXPECT_EQ(series->samples[0].x, 0);
    EXPECT_EQ(series->samples[1].x, 1);
}

TEST(TimeSeriesChartModel, LaterDuplicateXOverwritesEarlier)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 1.0}, {0, 9.0}, {1, 2.0}})));
    const auto* series = model.series("cpu");
    ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->samples.size(), 2u);
    EXPECT_DOUBLE_EQ(series->samples[0].y, 9.0);
}

TEST(TimeSeriesChartModel, RejectsOverlappingStateIntervals)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 1.0}})));
    EXPECT_FALSE(model.setSeriesStates("cpu", {
        {TimeSeriesStateKind::Gap, 0, 4},
        {TimeSeriesStateKind::Invalid, 3, 6},
    }));
}

TEST(TimeSeriesChartModel, InvalidMasksNumericAndOutranksGap)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 1.0}, {1, 2.0}, {2, 3.0}})));
    ASSERT_TRUE(model.setSeriesStates("cpu", {{TimeSeriesStateKind::Invalid, 1, 2}}));
    ASSERT_TRUE(model.setWindow(0, 3));

    const auto hit = model.hitTest(1, "cpu");
    EXPECT_EQ(hit.kind, TimeSeriesHitKind::Invalid);

    const auto columns = buildTimeSeriesEnvelope(*model.series("cpu"), 0, 3, 3);
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_FALSE(columns[0].empty);
    EXPECT_TRUE(columns[1].empty);
    EXPECT_FALSE(columns[2].empty);
}

TEST(TimeSeriesChartModel, ColumnSampleXUsesBucketCenter)
{
    EXPECT_EQ(VNX::columnSampleX(0, 300, 200, 101), 152);
    EXPECT_EQ(VNX::columnSampleX(0, 40, 200, 199), 39);
    EXPECT_EQ(VNX::columnSampleX(0, 40, 200, 0), 0);
}

TEST(TimeSeriesChartModel, ColumnSampleXLastPixelStaysInsideHalfOpenWindow)
{
    EXPECT_EQ(VNX::columnSampleX(0, 1, 200, 199), 0);
    EXPECT_EQ(VNX::columnSampleX(5, 6, 100, 99), 5);
    EXPECT_LT(VNX::columnSampleX(0, 40, 200, 199), 40);
}

TEST(TimeSeriesChartModel, EnvelopeKeepsSpikeInItsLogicalColumn)
{
    TimeSeriesChartModel model;
    std::vector<TimeSeriesSample> in;
    for (int i = 0; i < 20; ++i)
        in.push_back(TimeSeriesSample{i, i == 7 ? 100.0 : 1.0});
    ASSERT_TRUE(model.updateSeries("cpu", in));

    const auto columns = buildTimeSeriesEnvelope(*model.series("cpu"), 0, 20, 5);
    ASSERT_EQ(columns.size(), 5u);
    bool sawSpike = false;
    for (const auto& column : columns)
    {
        if (!column.empty && column.max >= 100.0)
            sawSpike = true;
    }
    EXPECT_TRUE(sawSpike);
}

TEST(TimeSeriesChartModel, EmptyColumnIsNotZero)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 5.0}, {9, 5.0}})));
    const auto columns = buildTimeSeriesEnvelope(*model.series("cpu"), 0, 10, 10);
    ASSERT_EQ(columns.size(), 10u);
    EXPECT_FALSE(columns[0].empty);
    EXPECT_TRUE(columns[1].empty);
    EXPECT_FALSE(columns[9].empty);
}

TEST(TimeSeriesChartModel, AutoYDoesNotInventZero)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 8.0}, {1, 10.0}})));
    ASSERT_TRUE(model.setWindow(0, 2));
    const auto range = model.autoYRange("cpu");
    ASSERT_TRUE(range.has_value());
    EXPECT_DOUBLE_EQ(range->first, 8.0);
    EXPECT_DOUBLE_EQ(range->second, 10.0);
}

TEST(TimeSeriesChartModel, ConstantSeriesPadsAroundValue)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 4.0}, {1, 4.0}})));
    ASSERT_TRUE(model.setWindow(0, 2));
    const auto range = model.autoYRange("cpu");
    ASSERT_TRUE(range.has_value());
    EXPECT_DOUBLE_EQ(range->first, 4.0 - TimeSeriesChartModel::kConstantYPad);
    EXPECT_DOUBLE_EQ(range->second, 4.0 + TimeSeriesChartModel::kConstantYPad);
}

TEST(TimeSeriesChartModel, RetainLimitDropsOldestSamples)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.setRetainLimit(4));
    std::vector<TimeSeriesSample> in;
    for (int i = 0; i < 10; ++i)
        in.push_back(TimeSeriesSample{i, static_cast<double>(i)});
    ASSERT_TRUE(model.updateSeries("cpu", in));
    const auto* series = model.series("cpu");
    ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->samples.size(), 4u);
    EXPECT_EQ(series->samples.front().x, 6);
    EXPECT_EQ(series->samples.back().x, 9);
}

TEST(TimeSeriesChartModel, LongFeedStaysAtRetainLimit)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.setRetainLimit(8192));
    std::vector<TimeSeriesSample> in;
    in.reserve(20000);
    for (int i = 0; i < 20000; ++i)
        in.push_back(TimeSeriesSample{i, 1.0});
    ASSERT_TRUE(model.updateSeries("cpu", in));
    ASSERT_NE(model.series("cpu"), nullptr);
    EXPECT_LE(static_cast<int>(model.series("cpu")->samples.size()), 8192);
}

TEST(TimeSeriesChartModel, HistoryOutsideRetainIsUnavailable)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{10, 1.0}, {11, 2.0}})));
    const auto hit = model.hitTest(0, "cpu");
    EXPECT_EQ(hit.kind, TimeSeriesHitKind::HistoryUnavailable);
}

TEST(TimeSeriesChartModel, HiddenSeriesDoesNotFillEmptyView)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 1.0}})));
    ASSERT_TRUE(model.setSeriesVisible("cpu", false));
    ASSERT_TRUE(model.setWindow(0, 1));
    EXPECT_TRUE(model.isEmptyView());
}

TEST(TimeSeriesChartModel, StateColumnDoesNotBlendNeighborNumeric)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 1.0}, {1, 50.0}, {2, 2.0}})));
    ASSERT_TRUE(model.setSeriesStates("cpu", {{TimeSeriesStateKind::Invalid, 1, 2}}));
    const auto columns = buildTimeSeriesEnvelope(*model.series("cpu"), 0, 3, 3);
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_TRUE(columns[1].hasState);
    EXPECT_EQ(columns[1].state, TimeSeriesStateKind::Invalid);
    EXPECT_TRUE(columns[1].empty);
    EXPECT_FALSE(columns[0].empty);
    EXPECT_FALSE(columns[2].empty);
    EXPECT_LT(columns[0].max, 50.0);
    EXPECT_LT(columns[2].max, 50.0);
}

TEST(TimeSeriesChartModel, ThresholdsDoNotChangeAutoY)
{
    TimeSeriesChartModel model;
    ASSERT_TRUE(model.updateSeries("cpu", samples({{0, 2.0}, {1, 3.0}})));
    ASSERT_TRUE(model.setWindow(0, 2));
    ASSERT_TRUE(model.setSeriesThresholds("cpu", {TimeSeriesThreshold{"warn", 50.0}}));
    const auto range = model.autoYRange("cpu");
    ASSERT_TRUE(range.has_value());
    EXPECT_DOUBLE_EQ(range->second, 3.0);
}
