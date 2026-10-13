#include "Widgets/Monitor/MonitorPerformanceChartSource.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Widgets/TimeSeries/TimeSeriesChart.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QVariantMap>

#include <vector>

using namespace VNX;

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

TimeSeriesChartModel cpuModel()
{
    TimeSeriesChartModel model;
    std::vector<TimeSeriesSample> samples;
    for (int i = 0; i < 40; ++i)
        samples.push_back(TimeSeriesSample{i, i == 13 ? 80.0 : 1.0});
    model.updateSeries("cpu", samples);
    model.setSeriesStates("cpu", {{TimeSeriesStateKind::Gap, 20, 24}});
    model.setWindow(0, 40);
    return model;
}

} // namespace

TEST(MonitorPerformance, EmptyModelHasNoAvailableTimeSeries)
{
    ensureApp();
    MonitorPerformancePanel panel;
    EXPECT_FALSE(panel.timeSeriesAvailable());
}

TEST(MonitorPerformance, ParamsRoundTripWindowAndSeriesVisibility)
{
    ensureApp();
    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    ASSERT_TRUE(panel.setSeriesVisible(QStringLiteral("cpu"), false));
    ASSERT_TRUE(panel.setWindow(4, 20));
    panel.setPaused(true);

    const QVariantMap params = panel.snapshotParams();
    MonitorPerformancePanel restored;
    ASSERT_TRUE(restored.applyChartModel(cpuModel()));
    restored.restoreParams(params);
    EXPECT_EQ(restored.chartModel().windowStart(), 4);
    EXPECT_EQ(restored.chartModel().windowEnd(), 20);
    ASSERT_NE(restored.chartModel().series("cpu"), nullptr);
    EXPECT_FALSE(restored.chartModel().series("cpu")->visible);
    EXPECT_TRUE(restored.paused());
}

TEST(MonitorPerformance, ClickLocksCursorWithoutChartSignal)
{
    ensureApp();
    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    panel.resize(400, 200);
    ASSERT_NE(panel.chart(), nullptr);
    panel.chart()->setFixedWidth(200);

    const MonitorCursorReject reject = panel.lockFromChartX(100);
    EXPECT_EQ(reject, MonitorCursorReject::None);
    ASSERT_TRUE(panel.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 20);
}

TEST(MonitorPerformance, ClickWithoutManualRetainedWindowStillLocks)
{
    ensureApp();
    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    EXPECT_EQ(panel.lockFromChartX(0), MonitorCursorReject::None);
    ASSERT_TRUE(panel.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 0);
}

TEST(MonitorPerformance, SingleSampleClickLocksThatFrame)
{
    ensureApp();
    TimeSeriesChartModel model;
    model.updateSeries("cpu", {TimeSeriesSample{7, 1.0}});
    model.setWindow(7, 8);

    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(model));
    panel.resize(400, 200);
    ASSERT_NE(panel.chart(), nullptr);
    panel.chart()->setFixedWidth(200);

    const MonitorCursorReject reject = panel.lockFromChartX(0);
    EXPECT_EQ(reject, MonitorCursorReject::None);
    ASSERT_TRUE(panel.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 7);
}

TEST(MonitorPerformance, RightmostPixelLocksLastRetainedFrame)
{
    ensureApp();
    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    panel.resize(400, 200);
    ASSERT_NE(panel.chart(), nullptr);
    panel.chart()->setFixedWidth(200);

    const MonitorCursorReject reject = panel.lockFromChartX(199);
    EXPECT_EQ(reject, MonitorCursorReject::None);
    ASSERT_TRUE(panel.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 39);
}

TEST(MonitorPerformance, ClickLocksColumnCenterWhenSpanExceedsWidth)
{
    ensureApp();
    TimeSeriesChartModel model;
    std::vector<TimeSeriesSample> samples;
    for (int i = 0; i < 300; ++i)
        samples.push_back(TimeSeriesSample{i, 1.0});
    model.updateSeries("cpu", samples);
    model.setWindow(0, 300);

    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(model));
    panel.resize(400, 200);
    ASSERT_NE(panel.chart(), nullptr);
    panel.chart()->setFixedWidth(200);

    const MonitorCursorReject reject = panel.lockFromChartX(101);
    EXPECT_EQ(reject, MonitorCursorReject::None);
    ASSERT_TRUE(panel.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 152);
}

TEST(MonitorPerformance, GapColumnsStayMarkedInRenderData)
{
    ensureApp();
    MonitorPerformancePanel panel;
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    panel.chart()->setFixedSize(40, 64);
    panel.chart()->updateData(panel.chartModel());
    ASSERT_FALSE(panel.chart()->renderSeries().empty());

    bool sawGap = false;
    bool sawSpike = false;
    for (const auto& column : panel.chart()->renderSeries().front().columns)
    {
        if (column.hasState && column.state == TimeSeriesStateKind::Gap)
            sawGap = true;
        if (!column.empty && column.max >= 80.0)
            sawSpike = true;
    }
    EXPECT_TRUE(sawGap);
    EXPECT_TRUE(sawSpike);
}

TEST(MonitorPerformance, PausedBlocksLiveAdvance)
{
    ensureApp();
    MonitorPerformancePanel panel;
    panel.setPaused(true);
    EXPECT_FALSE(panel.advanceLiveForTest(12));
    EXPECT_FALSE(panel.cursorModel().snapshot().executeFrame.has_value());
}

TEST(MonitorPerformance, HostOwnedSourcePushesModelToAttachedPanel)
{
    ensureApp();
    MonitorPerformanceChartSource source;
    MonitorPerformancePanel panel;
    source.attach(&panel);
    EXPECT_FALSE(panel.timeSeriesAvailable());

    source.apply(cpuModel());
    EXPECT_TRUE(panel.timeSeriesAvailable());
    ASSERT_NE(panel.chartModel().series("cpu"), nullptr);
    ASSERT_NE(source.model().series("cpu"), nullptr);
}
