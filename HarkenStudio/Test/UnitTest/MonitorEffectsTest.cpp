#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/EffectMonitorProjection.h"
#include "Monitor/MonitorStore.h"
#include "Widgets/Monitor/MonitorEffectsModel.h"
#include "Widgets/Monitor/MonitorEffectsPanel.h"
#include "Widgets/Monitor/MonitorEffectsSource.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTableView>

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
Monitor::MonitorDecodedRecord makeEffectUpsert(std::uint64_t handle,
                                               std::uint64_t generation,
                                               std::uint64_t frame,
                                               std::uint32_t bypass,
                                               std::uint32_t tail,
                                               std::uint32_t latency,
                                               bool numericLatency)
{
    Monitor::MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = VN_MONITOR_RECORD_STATE_UPSERT;
    rec.header.recordVersion = 1;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.sourceSequence = 1;
    rec.header.streamId = 8;
    rec.header.monitorSessionEpoch = 10;
    rec.header.executeFrame = frame;
    rec.header.entityKind = VN_MONITOR_ENTITY_EFFECT;
    rec.header.instanceHandle = handle;
    rec.header.instanceGeneration = generation;
    rec.header.stableId = harkentest::StudioObjectId(handle + 100);
    rec.header.validityFlags = numericLatency ? VN_MONITOR_VALID_NUMERIC : 0u;
    VNX::Monitor::MonitorStateUpsert payload {};
    payload.structSize = sizeof(payload);
    payload.entityKind = VN_MONITOR_ENTITY_EFFECT;
    payload.instanceHandle = handle;
    payload.instanceGeneration = generation;
    payload.bypass01 = bypass;
    payload.tailFrames = tail;
    payload.latencyFrames = latency;
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

std::shared_ptr<Monitor::MonitorStoreSnapshot> snapshotFrom(
    std::vector<Monitor::MonitorDecodedRecord> records)
{
    auto snapshot = std::make_shared<Monitor::MonitorStoreSnapshot>();
    snapshot->generation = 22;
    snapshot->records = std::move(records);
    for (Monitor::MonitorRecordId id = 0; id < snapshot->records.size(); ++id)
        snapshot->byRecordKind.emplace(snapshot->records[id].header.recordKind, id);
    addBaseline(*snapshot);
    return snapshot;
}

} // namespace

TEST(MonitorEffects, WiredBypassTailAndGapLatency)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeEffectUpsert(7, 3, 10, 1, 48, 0, false),
    });
    MonitorEffectsPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), Monitor::MonitorEffectsTableState::Rows);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    const auto* row = panel.visibleRowAt(0);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->bypass01, 1u);
    EXPECT_TRUE(row->bypassPublished);
    EXPECT_EQ(row->tailFrames, 48u);
    EXPECT_TRUE(row->tailPublished);
    EXPECT_EQ(row->latencyState, Monitor::EffectCellState::Gap);
    EXPECT_EQ(row->paramsState, Monitor::EffectCellState::Unavailable);
    EXPECT_EQ(row->grState, Monitor::EffectCellState::Unsupported);
}

TEST(MonitorEffects, NumericZeroLatencyIsNotGap)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeEffectUpsert(7, 3, 10, 0, 0, 0, true),
    });
    MonitorEffectsPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.visibleRowAt(0)->latencyState, Monitor::EffectCellState::Numeric);
    EXPECT_EQ(panel.tableModel()->cellText(0, MonitorEffectsModel::Latency), QStringLiteral("0"));
}

TEST(MonitorEffects, NoLegalFrameDoesNotKeepPreviousRows)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeEffectUpsert(7, 3, 10, 1, 48, 0, false),
    });
    MonitorEffectsPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.visibleRowCount(), 1);
    MonitorCursorModel empty;
    panel.setSharedCursor(&empty);
    EXPECT_EQ(panel.tableState(), Monitor::MonitorEffectsTableState::NoLegalFrame);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.table()->isHidden());
}

TEST(MonitorEffects, DropSummaryMakesReplayIncomplete)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeEffectUpsert(7, 3, 10, 1, 48, 0, false),
    });
    Monitor::MonitorDecodedRecord drop;
    drop.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    drop.header.recordKind = VN_MONITOR_RECORD_DROP_SUMMARY;
    drop.header.streamKind = VN_MONITOR_STREAM_STATE;
    drop.header.sourceSequence = 2;
    drop.header.executeFrame = 10;
    VNX::Monitor::MonitorDropSummary payload {};
    payload.structSize = sizeof(payload);
    payload.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    payload.streamId = 2;
    payload.firstSourceSequence = 1;
    payload.lastSourceSequence = 2;
    payload.recordCount = 1;
    drop.payload.resize(sizeof(payload));
    std::memcpy(drop.payload.data(), &payload, sizeof(payload));
    snapshot->byRecordKind.emplace(drop.header.recordKind,
                                   static_cast<Monitor::MonitorRecordId>(snapshot->records.size()));
    snapshot->records.push_back(std::move(drop));

    MonitorEffectsPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), Monitor::MonitorEffectsTableState::Incomplete);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.table()->isHidden());
}

TEST(MonitorEffects, SameSnapshotGenerationOnRows)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeEffectUpsert(7, 3, 10, 1, 48, 0, false),
        makeEffectUpsert(8, 1, 10, 0, 12, 0, false),
    });
    MonitorEffectsPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 2);
    EXPECT_EQ(panel.visibleRowAt(0)->storeGeneration, snapshot->generation);
    EXPECT_EQ(panel.visibleRowAt(1)->storeGeneration, snapshot->generation);
    EXPECT_EQ(panel.boundGeneration(), snapshot->generation);
}
