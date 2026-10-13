#include "Widgets/TimeSeries/TimeSeriesChart.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>

#include <memory>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

std::unique_ptr<VNX::TimeSeriesChart> makeChart(int width, int height, qreal dpr)
{
    ensureApp();
    auto chart = std::make_unique<VNX::TimeSeriesChart>();
    chart->setFixedSize(width, height);
    chart->setTestDevicePixelRatio(dpr);
    return chart;
}

VNX::TimeSeriesChartModel spikedModel()
{
    VNX::TimeSeriesChartModel model;
    std::vector<VNX::TimeSeriesSample> samples;
    for (int i = 0; i < 40; ++i)
        samples.push_back(VNX::TimeSeriesSample{i, i == 13 ? 80.0 : 1.0});
    model.updateSeries("cpu", samples);
    model.setWindow(0, 40);
    return model;
}

} // namespace

TEST(TimeSeriesChart, BufferMatchesLogicalSizeTimesDpr)
{
    auto chart = makeChart(200, 80, 2.0);
    chart->updateData(spikedModel());
    EXPECT_EQ(chart->bufferPixelSize(), QSize(400, 160));
    EXPECT_DOUBLE_EQ(chart->devicePixelRatioUsed(), 2.0);
}

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

TEST(TimeSeriesChart, ProductionPathReadsWidgetDprWithoutSetter)
{
    auto chart = makeChart(100, 40, 1.0);
    chart->updateData(spikedModel());
    EXPECT_EQ(chart->bufferPixelSize(), QSize(100, 40));
}

TEST(TimeSeriesChart, RightmostHoverStaysInsideHalfOpenWindow)
{
    auto chart = makeChart(200, 80, 1.0);
    VNX::TimeSeriesChartModel model;
    model.updateSeries("cpu", {{0, 1.0}});
    model.setWindow(0, 1);
    chart->updateData(model);
    EXPECT_EQ(chart->xFromLogicalForTest(199), 0);
    EXPECT_EQ(model.hitTest(chart->xFromLogicalForTest(199), "cpu").kind, VNX::TimeSeriesHitKind::Numeric);
}

TEST(TimeSeriesChart, VisibleSeriesHoverIgnoresHiddenFirst)
{
    ensureApp();
    VNX::TimeSeriesChartModel model;
    model.updateSeries("hidden", {{0, 1.0}});
    model.updateSeries("cpu", {{0, 9.0}});
    model.setSeriesVisible("hidden", false);
    model.setWindow(0, 1);
    auto chart = makeChart(8, 40, 1.0);
    chart->updateData(model);
    ASSERT_EQ(chart->renderSeries().size(), 1u);
    EXPECT_EQ(chart->renderSeries().front().id, "cpu");
}

TEST(TimeSeriesChart, RenderedStatesStayDistinct)
{
    ensureApp();
    VNX::TimeSeriesChartModel model;
    model.updateSeries("cpu", {{0, 1.0}, {1, 2.0}, {2, 3.0}});
    model.setSeriesStates("cpu", {
        {VNX::TimeSeriesStateKind::Gap, 0, 1},
        {VNX::TimeSeriesStateKind::Invalid, 1, 2},
        {VNX::TimeSeriesStateKind::WarmUp, 2, 3},
    });
    model.setWindow(0, 3);
    auto chart = makeChart(3, 40, 1.0);
    chart->updateData(model);
    ASSERT_EQ(chart->renderSeries().front().columns.size(), 3u);
    EXPECT_TRUE(chart->renderSeries().front().columns[0].hasState);
    EXPECT_EQ(chart->renderSeries().front().columns[0].state, VNX::TimeSeriesStateKind::Gap);
    EXPECT_EQ(chart->renderSeries().front().columns[1].state, VNX::TimeSeriesStateKind::Invalid);
    EXPECT_EQ(chart->renderSeries().front().columns[2].state, VNX::TimeSeriesStateKind::WarmUp);
}

TEST(TimeSeriesChart, GapDoesNotCreateRenderColumn)
{
    ensureApp();
    VNX::TimeSeriesChartModel model;
    model.updateSeries("cpu", {
        {0, 1.0},
        {1, 2.0},
        {2, 3.0},
        {3, 4.0},
    });
    model.setSeriesStates("cpu", {{VNX::TimeSeriesStateKind::Gap, 1, 3}});
    model.setWindow(0, 4);
    auto chart = makeChart(4, 40, 1.0);
    chart->updateData(model);
    ASSERT_EQ(chart->renderSeries().size(), 1u);
    ASSERT_EQ(chart->renderSeries().front().columns.size(), 4u);
    EXPECT_FALSE(chart->renderSeries().front().columns[0].empty);
    EXPECT_TRUE(chart->renderSeries().front().columns[1].empty);
    EXPECT_TRUE(chart->renderSeries().front().columns[2].empty);
    EXPECT_FALSE(chart->renderSeries().front().columns[3].empty);
}
