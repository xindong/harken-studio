#include "Widgets/TimeSeries/TimeSeriesChartModel.h"

#include <gtest/gtest.h>

#include <vector>

using VNX::TimeSeriesChartModel;
using VNX::TimeSeriesSample;
using VNX::buildTimeSeriesEnvelope;

TEST(TimeSeriesChartModel, EnvelopeKeepsSpikeInItsLogicalColumn)
{
    TimeSeriesChartModel model;
    std::vector<TimeSeriesSample> in;
    for (int i = 0; i < 20; ++i)
        in.push_back(TimeSeriesSample{i, i == 7 ? 100.0 : 1.0});
    ASSERT_TRUE(model.updateSeries("cpu", in));

    const auto columns = buildTimeSeriesEnvelope(*model.series("cpu"), 0, 20, 5);
    bool sawSpike = false;
    for (const auto& column : columns)
    {
        if (!column.empty && column.max >= 100.0)
            sawSpike = true;
    }
    EXPECT_TRUE(sawSpike);
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
