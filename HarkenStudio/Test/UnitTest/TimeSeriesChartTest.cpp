#include "Widgets/TimeSeries/TimeSeriesChart.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <memory>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

std::unique_ptr<harken::studio::TimeSeriesChart> makeChart(int width, int height, qreal dpr)
{
    ensureApp();
    auto chart = std::make_unique<harken::studio::TimeSeriesChart>();
    chart->setFixedSize(width, height);
    chart->setTestDevicePixelRatio(dpr);
    return chart;
}

harken::studio::TimeSeriesChartModel spikedModel()
{
    harken::studio::TimeSeriesChartModel model;
    std::vector<harken::studio::TimeSeriesSample> samples;
    for (int i = 0; i < 40; ++i)
        samples.push_back(harken::studio::TimeSeriesSample{i, i == 13 ? 80.0 : 1.0});
    model.updateSeries("cpu", samples);
    model.setWindow(0, 40);
    return model;
}

} // namespace

TEST(TimeSeriesChart, NarrowZoomKeepsSpikeInRenderData)
{
    auto chart = makeChart(8, 64, 1.0);
    chart->updateData(spikedModel());
    bool sawSpike = false;
    ASSERT_FALSE(chart->renderSeries().empty());
    for (const auto& column : chart->renderSeries().front().columns)
    {
        if (!column.empty && column.max >= 80.0)
            sawSpike = true;
    }
    EXPECT_TRUE(sawSpike);
}
