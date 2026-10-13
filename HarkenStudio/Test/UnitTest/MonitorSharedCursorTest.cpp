#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorStore.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorPerformanceChartSource.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Widgets/Monitor/MonitorVoicesPanel.h"
#include "Widgets/Monitor/MonitorVoicesSource.h"
#include "Widgets/TimeSeries/MonitorCursorMark.h"
#include "Widgets/TimeSeries/TimeSeriesChartModel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
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
MonitorCursorClock exactClock()
{
    MonitorCursorClock clock;
    clock.deviceEpoch = 7;
    clock.clockAnchorGeneration = 3;
    clock.deviceUncertaintyFrames = 0;
    return clock;
}

MonitorCursorClock estimateClock()
{
    MonitorCursorClock clock;
    clock.deviceEpoch = 7;
    clock.clockAnchorGeneration = 3;
    clock.deviceUncertaintyFrames = 4;
    return clock;
}

TimeSeriesChartModel cpuModel()
{
    TimeSeriesChartModel model;
    std::vector<TimeSeriesSample> samples;
    for (int i = 0; i < 40; ++i)
        samples.push_back(TimeSeriesSample{i, 1.0});
    model.updateSeries("cpu", samples);
    model.setWindow(0, 40);
    return model;
}

Monitor::MonitorDecodedRecord makeVoiceRecord(std::uint64_t handle,
                                              std::uint64_t generation,
                                              std::uint64_t frame,
                                              std::uint64_t sequence)
{
    Monitor::MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = VN_MONITOR_RECORD_STATE_UPSERT;
    rec.header.recordVersion = 1;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.sourceSequence = sequence;
    rec.header.streamId = 8;
    rec.header.monitorSessionEpoch = 10;
    rec.header.executeFrame = frame;
    rec.header.entityKind = VN_MONITOR_ENTITY_VOICE;
    rec.header.instanceHandle = handle;
    rec.header.instanceGeneration = generation;
    rec.header.validityFlags = VN_MONITOR_VALID_NUMERIC;
    VNX::Monitor::MonitorStateUpsert payload {};
    payload.structSize = sizeof(payload);
    payload.entityKind = VN_MONITOR_ENTITY_VOICE;
    payload.instanceHandle = handle;
    payload.instanceGeneration = generation;
    payload.volumeDb = -3.0f;
    rec.payload.resize(sizeof(payload));
    std::memcpy(rec.payload.data(), &payload, sizeof(payload));
    return rec;
}

void addBaseline(Monitor::MonitorStoreSnapshot& snapshot)
{
    Monitor::MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = VN_MONITOR_RECORD_STREAM_BASELINE;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.streamId = 8;
    rec.header.monitorSessionEpoch = 10;
    rec.header.sourceSequence = 0;
    rec.header.executeFrame = 0;
    VNX::Monitor::MonitorStreamBaseline payload {};
    payload.structSize = sizeof(payload);
    payload.streamId = 8;
    payload.firstExpectedSourceSequence = 1;
    rec.payload.resize(sizeof(payload));
    std::memcpy(rec.payload.data(), &payload, sizeof(payload));
    snapshot.byRecordKind.emplace(rec.header.recordKind,
                                  static_cast<Monitor::MonitorRecordId>(snapshot.records.size()));
    snapshot.records.push_back(std::move(rec));
}

std::shared_ptr<Monitor::MonitorStoreSnapshot> voiceSnapshotAt(std::uint64_t frame)
{
    auto snapshot = std::make_shared<Monitor::MonitorStoreSnapshot>();
    snapshot->generation = 11;
    snapshot->records.push_back(makeVoiceRecord(9, 1, frame, 1));
    snapshot->byRecordKind.emplace(snapshot->records.back().header.recordKind, 0);
    addBaseline(*snapshot);
    return snapshot;
}

} // namespace

TEST(MonitorSharedCursor, AttachedPanelsShareGeneration)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorPerformancePanel performance;
    MonitorVoicesPanel voices;
    performance.setSharedCursor(&shared);
    voices.setSharedCursor(&shared);
    EXPECT_EQ(&performance.cursorModel(), &shared);
    EXPECT_EQ(&voices.cursorModel(), &shared);

    ASSERT_TRUE(performance.applyChartModel(cpuModel()));
    voices.bindSnapshot(voiceSnapshotAt(20));

    EXPECT_EQ(shared.lockTo(20, exactClock()), MonitorCursorReject::None);
    const auto generation = shared.snapshot().generation;
    EXPECT_GT(generation, 0u);
    EXPECT_EQ(performance.cursorModel().snapshot().executeFrame, 20);
    EXPECT_EQ(voices.cursorModel().snapshot().executeFrame, 20);
    EXPECT_EQ(performance.cursorModel().snapshot().generation, generation);
    EXPECT_EQ(voices.cursorModel().snapshot().generation, generation);
}

TEST(MonitorSharedCursor, LockIgnoresAdvanceLive)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorPerformancePanel performance;
    MonitorVoicesPanel voices;
    performance.setSharedCursor(&shared);
    voices.setSharedCursor(&shared);
    ASSERT_TRUE(performance.applyChartModel(cpuModel()));
    voices.bindSnapshot(voiceSnapshotAt(8));

    ASSERT_TRUE(shared.advanceLive(10, exactClock()));
    EXPECT_EQ(shared.lockTo(8, exactClock()), MonitorCursorReject::None);
    const auto lockedGeneration = shared.snapshot().generation;
    ASSERT_TRUE(shared.advanceLive(40, exactClock()));
    EXPECT_EQ(shared.snapshot().mode, MonitorCursorMode::LockHistory);
    EXPECT_EQ(shared.snapshot().executeFrame, 8);
    EXPECT_EQ(shared.snapshot().generation, lockedGeneration);
    EXPECT_EQ(performance.cursorModel().snapshot().executeFrame, 8);
    EXPECT_EQ(voices.cursorModel().snapshot().executeFrame, 8);
    EXPECT_EQ(performance.cursorModel().snapshot().generation, lockedGeneration);
}

TEST(MonitorSharedCursor, FollowLiveAdvancesAll)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorPerformancePanel performance;
    MonitorVoicesPanel voices;
    performance.setSharedCursor(&shared);
    voices.setSharedCursor(&shared);
    ASSERT_TRUE(performance.applyChartModel(cpuModel()));
    voices.bindSnapshot(voiceSnapshotAt(8));

    ASSERT_TRUE(shared.advanceLive(10, exactClock()));
    EXPECT_EQ(shared.lockTo(8, exactClock()), MonitorCursorReject::None);
    ASSERT_TRUE(shared.advanceLive(40, exactClock()));
    EXPECT_EQ(shared.snapshot().executeFrame, 8);
    ASSERT_TRUE(shared.followLive());
    EXPECT_EQ(shared.snapshot().mode, MonitorCursorMode::FollowLive);
    EXPECT_EQ(shared.snapshot().executeFrame, 40);
    const auto generation = shared.snapshot().generation;
    EXPECT_EQ(performance.cursorModel().snapshot().executeFrame, 40);
    EXPECT_EQ(voices.cursorModel().snapshot().executeFrame, 40);
    EXPECT_EQ(performance.cursorModel().snapshot().generation, generation);
    EXPECT_EQ(voices.cursorModel().snapshot().generation, generation);
}

TEST(MonitorSharedCursor, UncertaintyVisibleExactEstimateUnknown)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorPerformancePanel performance;
    performance.setSharedCursor(&shared);
    ASSERT_TRUE(performance.applyChartModel(cpuModel()));
    ASSERT_NE(performance.cursorMark(), nullptr);
    EXPECT_EQ(performance.cursorMark()->objectName(),
              QStringLiteral("MonitorPerformanceCursorMark"));
    EXPECT_EQ(performance.findChild<QLabel*>(QStringLiteral("MonitorPerformanceCursor")),
              nullptr);

    EXPECT_EQ(performance.cursorMark()->displayedUncertainty(),
              MonitorCursorUncertaintyKind::Unknown);
    EXPECT_FALSE(performance.cursorMark()->showsLine());

    ASSERT_TRUE(shared.advanceLive(12, exactClock()));
    EXPECT_EQ(shared.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Exact);
    EXPECT_EQ(performance.cursorMark()->displayedUncertainty(),
              MonitorCursorUncertaintyKind::Exact);
    EXPECT_TRUE(performance.cursorMark()->showsLine());

    ASSERT_TRUE(shared.advanceLive(13, estimateClock()));
    EXPECT_EQ(shared.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Estimate);
    EXPECT_EQ(performance.cursorMark()->displayedUncertainty(),
              MonitorCursorUncertaintyKind::Estimate);

    MonitorCursorClock missingAnchor;
    missingAnchor.deviceUncertaintyFrames = 0;
    ASSERT_TRUE(shared.advanceLive(14, missingAnchor));
    EXPECT_EQ(shared.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Unknown);
    EXPECT_EQ(performance.cursorMark()->displayedUncertainty(),
              MonitorCursorUncertaintyKind::Unknown);

    EXPECT_EQ(shared.lockTo(12, exactClock()), MonitorCursorReject::None);
    shared.setContributorWindow(kMonitorCursorContributorPerformance, 20, 30);
    EXPECT_TRUE(shared.snapshot().historyUnavailable);
    EXPECT_FALSE(performance.cursorMark()->showsLine());
    EXPECT_EQ(performance.cursorMark()->displayedUncertainty(),
              MonitorCursorUncertaintyKind::Unknown);
}

TEST(MonitorSharedCursor, AbsentPanelDoesNotPreventVoiceSelection)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorVoicesPanel voices;
    voices.setSharedCursor(&shared);
    voices.bindSnapshot(voiceSnapshotAt(20));
    EXPECT_EQ(shared.lockTo(20, exactClock()), MonitorCursorReject::None);
    const auto generation = shared.snapshot().generation;
    EXPECT_EQ(voices.cursorModel().snapshot().executeFrame, 20);
    EXPECT_EQ(voices.cursorModel().snapshot().generation, generation);
    EXPECT_EQ(&voices.cursorModel(), &shared);
}

TEST(MonitorSharedCursor, PerformanceContributorDoesNotClearVoicesWindow)
{
    ensureApp();
    MonitorCursorModel shared;
    MonitorVoicesPanel voices;
    MonitorPerformancePanel performance;
    voices.setSharedCursor(&shared);
    voices.bindSnapshot(voiceSnapshotAt(80));
    EXPECT_EQ(shared.lockTo(80), MonitorCursorReject::None);
    EXPECT_EQ(shared.lockTo(12), MonitorCursorReject::HistoryUnavailable);

    performance.setSharedCursor(&shared);
    ASSERT_TRUE(performance.applyChartModel(cpuModel()));
    EXPECT_EQ(shared.lockTo(80), MonitorCursorReject::None);
    EXPECT_EQ(shared.lockTo(12), MonitorCursorReject::None);
    EXPECT_EQ(&performance.cursorModel(), &shared);
}

TEST(MonitorSharedCursor, HostOwnedSourceWiresSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorPerformanceChartSource source;
    MonitorPerformancePanel panel;
    source.attach(&panel, &shared);
    EXPECT_EQ(&panel.cursorModel(), &shared);
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    EXPECT_EQ(shared.lockTo(12, exactClock()), MonitorCursorReject::None);
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    const auto lockedGeneration = shared.snapshot().generation;
    EXPECT_EQ(panel.cursorModel().snapshot().generation, lockedGeneration);
    source.attach(&panel, &shared);
    EXPECT_EQ(&panel.cursorModel(), &shared);
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    EXPECT_EQ(panel.cursorModel().snapshot().mode, MonitorCursorMode::LockHistory);
    ASSERT_TRUE(shared.advanceLive(30, exactClock()));
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    ASSERT_TRUE(shared.followLive());
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 30);
    EXPECT_EQ(&panel.cursorModel(), &shared);
}

TEST(MonitorSharedCursor, SharedCursorMayDieBeforePanel)
{
    ensureApp();
    auto shared = std::make_unique<MonitorCursorModel>();
    auto performance = std::make_unique<MonitorPerformancePanel>();
    auto voices = std::make_unique<MonitorVoicesPanel>();
    performance->setSharedCursor(shared.get());
    voices->setSharedCursor(shared.get());
    EXPECT_EQ(&performance->cursorModel(), shared.get());
    shared.reset();
    performance.reset();
    voices.reset();
}

TEST(MonitorSharedCursor, DetachBeforeSharedCursorDies)
{
    ensureApp();
    MonitorCursorModel shared;
    MonitorPerformancePanel performance;
    MonitorVoicesPanel voices;
    performance.setSharedCursor(&shared);
    voices.setSharedCursor(&shared);
    performance.setSharedCursor(nullptr);
    voices.setSharedCursor(nullptr);
    ASSERT_TRUE(shared.advanceLive(9));
    EXPECT_FALSE(performance.cursorModel().snapshot().executeFrame.has_value());
    EXPECT_FALSE(voices.cursorModel().snapshot().executeFrame.has_value());
}

TEST(MonitorSharedCursor, PauseBlocksTestAdvanceNotSharedLock)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setSessionIdentity(7, 3);
    MonitorPerformancePanel panel;
    panel.setSharedCursor(&shared);
    ASSERT_TRUE(panel.applyChartModel(cpuModel()));
    panel.setPaused(true);
    EXPECT_FALSE(panel.advanceLiveForTest(12));
    EXPECT_FALSE(shared.snapshot().executeFrame.has_value());
    ASSERT_TRUE(shared.advanceLive(12, exactClock()));
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    EXPECT_EQ(shared.lockTo(8, exactClock()), MonitorCursorReject::None);
    ASSERT_TRUE(shared.advanceLive(30, exactClock()));
    EXPECT_EQ(shared.snapshot().executeFrame, 8);
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 8);
}

TEST(MonitorSharedCursor, EmptyPeerDoesNotClearUnion)
{
    ensureApp();
    MonitorCursorModel shared;
    MonitorVoicesPanel voices;
    MonitorPerformancePanel performance;
    voices.setSharedCursor(&shared);
    performance.setSharedCursor(&shared);
    voices.bindSnapshot(voiceSnapshotAt(9));
    EXPECT_EQ(shared.lockTo(9), MonitorCursorReject::None);
    EXPECT_EQ(shared.lockTo(1), MonitorCursorReject::HistoryUnavailable);
}
