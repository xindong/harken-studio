#include <harken/Harken.h>
#include "Monitor/MonitorPerformanceLiveHost.h"
#include "MonitorRecordPack.h"
#include "Widgets/Monitor/MonitorPerformanceChartSource.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Widgets/TimeSeries/MonitorCursorModel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QThread>
#include <vector>

using VNX::Monitor::MonitorPerformanceLiveHost;
using VNX::Monitor::MonitorPerformanceLiveTick;
using VNX::MonitorCursorMode;
using VNX::MonitorPerformanceChartSource;
using VNX::MonitorPerformancePanel;
using harken::monitor::kTimeSeriesCallbackCpuStreamId;
using harken::monitor::kTimeSeriesMetricCallbackCpu;
using harken::monitor::kTimeSeriesUnitMicroseconds;
using harken::monitor::PackedTimeSeriesBytes;
using harken::monitor::PackNumericTimeSeries;
using harken::monitor::PackTimeSeriesRecord;

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

std::vector<uint8_t> packNumeric(uint64_t streamId,
                                 uint32_t metricId,
                                 uint32_t unitId,
                                 uint32_t valueBits,
                                 uint64_t executeFrame,
                                 uint64_t sourceSequence = 7,
                                 uint64_t plan = 3)
{
    std::vector<uint8_t> bytes(PackedTimeSeriesBytes());
    PackNumericTimeSeries(bytes.data(),
                          static_cast<uint32_t>(bytes.size()),
                          sourceSequence,
                          streamId,
                          metricId,
                          unitId,
                          30,
                          valueBits,
                          plan,
                          executeFrame);
    return bytes;
}

} // namespace

TEST(MonitorPerformance, HostIngestAdvancesLiveCursorWithoutTestHook)
{
    ensureApp();
    MonitorPerformanceLiveHost host;
    const auto packed = packNumeric(kTimeSeriesCallbackCpuStreamId,
                                    kTimeSeriesMetricCallbackCpu,
                                    kTimeSeriesUnitMicroseconds,
                                    12,
                                    40);
    const auto tick = host.ingestPacked(packed.data(), packed.size(), 1);
    ASSERT_TRUE(tick.hasLiveFrame);
    EXPECT_EQ(tick.liveExecuteFrame, 40);
    EXPECT_EQ(tick.sourceSequence, 7u);
    EXPECT_EQ(tick.capturePlanGeneration, 3u);
    EXPECT_TRUE(tick.polledOffUiThread);
    EXPECT_FALSE(tick.samples.empty());

    MonitorPerformancePanel panel;
    MonitorPerformanceChartSource source;
    source.attach(&panel);
    source.applyLiveTick(tick, false);
    EXPECT_TRUE(panel.timeSeriesAvailable());
    EXPECT_TRUE(panel.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 40);
    EXPECT_EQ(panel.cursorModel().snapshot().mode, MonitorCursorMode::FollowLive);
    ASSERT_NE(panel.chartModel().series("callbackCpu"), nullptr);
    EXPECT_FALSE(panel.chartModel().series("callbackCpu")->samples.empty());
    EXPECT_EQ(panel.chartModel().series("callbackCpu")->samples.back().x, 40);
}

TEST(MonitorPerformanceLive, SecondTickKeepsPriorSamples)
{
    ensureApp();
    MonitorPerformanceLiveHost host;
    MonitorPerformancePanel panel;
    MonitorPerformanceChartSource source;
    source.attach(&panel);

    const auto first = packNumeric(kTimeSeriesCallbackCpuStreamId,
                                   kTimeSeriesMetricCallbackCpu,
                                   kTimeSeriesUnitMicroseconds,
                                   4,
                                   11,
                                   1);
    source.applyLiveTick(host.ingestPacked(first.data(), first.size(), 1), false);
    const auto second = packNumeric(kTimeSeriesCallbackCpuStreamId,
                                    kTimeSeriesMetricCallbackCpu,
                                    kTimeSeriesUnitMicroseconds,
                                    8,
                                    12,
                                    2);
    source.applyLiveTick(host.ingestPacked(second.data(), second.size(), 1), false);
    const auto* series = panel.chartModel().series("callbackCpu");
    ASSERT_NE(series, nullptr);
    ASSERT_EQ(series->samples.size(), 2u);
    EXPECT_EQ(series->samples[0].x, 11);
    EXPECT_EQ(series->samples[1].x, 12);
    EXPECT_EQ(*panel.cursorModel().snapshot().executeFrame, 12);
}

TEST(MonitorPerformanceLive, PausedHostDoesNotAdvanceCursor)
{
    ensureApp();
    MonitorPerformanceLiveHost host;
    const auto packed = packNumeric(kTimeSeriesCallbackCpuStreamId,
                                    kTimeSeriesMetricCallbackCpu,
                                    kTimeSeriesUnitMicroseconds,
                                    9,
                                    22);
    const auto tick = host.ingestPacked(packed.data(), packed.size(), 1);
    ASSERT_TRUE(tick.hasLiveFrame);

    MonitorPerformancePanel panel;
    panel.setPaused(true);
    MonitorPerformanceChartSource source;
    source.attach(&panel);
    source.applyLiveTick(tick, panel.paused());
    EXPECT_TRUE(panel.timeSeriesAvailable());
    EXPECT_FALSE(panel.cursorModel().snapshot().executeFrame.has_value());
}

TEST(MonitorPerformanceLive, UnsupportedSampleDoesNotInventZero)
{
    ensureApp();
    MonitorPerformanceLiveHost host;
    std::vector<uint8_t> bytes(PackedTimeSeriesBytes());
    PackTimeSeriesRecord(bytes.data(),
                         static_cast<uint32_t>(bytes.size()),
                         1,
                         kTimeSeriesCallbackCpuStreamId,
                         kTimeSeriesMetricCallbackCpu,
                         kTimeSeriesUnitMicroseconds,
                         30,
                         0,
                         VN_MONITOR_VALID_UNSUPPORTED,
                         1,
                         8);
    const auto tick = host.ingestPacked(bytes.data(), bytes.size(), 1);
    EXPECT_FALSE(tick.hasLiveFrame);

    MonitorPerformancePanel panel;
    MonitorPerformanceChartSource source;
    source.attach(&panel);
    source.applyLiveTick(tick, false);
    const auto* series = panel.chartModel().series("callbackCpu");
    ASSERT_NE(series, nullptr);
    EXPECT_TRUE(series->samples.empty());
    EXPECT_FALSE(series->states.empty());
    EXPECT_FALSE(panel.cursorModel().snapshot().executeFrame.has_value());
}

TEST(MonitorPerformanceLive, LiveFrameIsSampleExecuteFrameNotWindowEdge)
{
    ensureApp();
    MonitorPerformanceLiveHost host;
    const auto packed = packNumeric(kTimeSeriesCallbackCpuStreamId,
                                    kTimeSeriesMetricCallbackCpu,
                                    kTimeSeriesUnitMicroseconds,
                                    4,
                                    11);
    const auto tick = host.ingestPacked(packed.data(), packed.size(), 1);
    ASSERT_TRUE(tick.hasLiveFrame);
    EXPECT_EQ(tick.liveExecuteFrame, 11);

    const auto model = MonitorPerformanceChartSource::projectLiveSamples(tick.samples);
    std::int64_t minX = 0;
    std::int64_t maxX = 0;
    ASSERT_TRUE(model.retainedRange(&minX, &maxX));
    EXPECT_EQ(maxX, 11);
    EXPECT_EQ(tick.liveExecuteFrame, maxX);
    EXPECT_EQ(model.windowEnd(), maxX + 1);
    EXPECT_NE(tick.liveExecuteFrame, model.windowEnd());
}

TEST(MonitorPerformanceLive, UiThreadPollIsRejected)
{
    ensureApp();
    MonitorPerformanceLiveHost host;
    host.setUiThreadCheck([]() { return true; });
    const auto tick = host.ingestOnce();
    EXPECT_FALSE(tick.polledOffUiThread);
    EXPECT_EQ(tick.lastResult, VN_MONITOR_WRONG_THREAD);
    EXPECT_FALSE(tick.hasLiveFrame);
}

TEST(MonitorPerformanceLive, ObservationSessionDoesNotRequestCaptureControl)
{
    EXPECT_EQ(VN_MONITOR_PERM_OBSERVE_MEASUREMENTS, 1u << 2);
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, 1u << 2);
    EXPECT_NE(VN_MONITOR_PERM_OBSERVE_MEASUREMENTS, VN_MONITOR_PERM_CAPTURE_CONTROL);
}
