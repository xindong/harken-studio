#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/BusTopologyProjection.h"
#include "Monitor/MonitorStore.h"
#include "Widgets/Monitor/MonitorBusesModel.h"
#include "Widgets/Monitor/MonitorBusesPanel.h"
#include "Widgets/Monitor/MonitorBusesSource.h"
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
Monitor::MonitorDecodedRecord makeBusUpsert(std::uint64_t handle,
                                            std::uint64_t generation,
                                            std::uint64_t frame,
                                            std::uint64_t parent,
                                            std::uint32_t mute,
                                            std::uint32_t solo)
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
    rec.header.entityKind = VN_MONITOR_ENTITY_BUS;
    rec.header.instanceHandle = handle;
    rec.header.instanceGeneration = generation;
    rec.header.stableId = harkentest::StudioObjectId(handle + 50);
    VNX::Monitor::MonitorStateUpsert payload {};
    payload.structSize = sizeof(payload);
    payload.entityKind = VN_MONITOR_ENTITY_BUS;
    payload.instanceHandle = handle;
    payload.instanceGeneration = generation;
    payload.routeBusId = harkentest::StudioObjectId(parent);
    payload.mute01 = mute;
    payload.solo01 = solo;
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
    snapshot->generation = 33;
    snapshot->records = std::move(records);
    for (Monitor::MonitorRecordId id = 0; id < snapshot->records.size(); ++id)
        snapshot->byRecordKind.emplace(snapshot->records[id].header.recordKind, id);
    addBaseline(*snapshot);
    return snapshot;
}

} // namespace

TEST(MonitorBuses, FlatParentMuteSoloWithoutTree)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeBusUpsert(9, 1, 10, 2, 1, 1),
    });
    MonitorBusesPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), Monitor::MonitorBusesTableState::Rows);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    const auto* row = panel.visibleRowAt(0);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->routeBusId, harkentest::StudioObjectId(2u));
    EXPECT_EQ(row->mute01, 1u);
    EXPECT_EQ(row->solo01, 1u);
    EXPECT_EQ(row->treeState, Monitor::BusCellState::Unsupported);
    EXPECT_FALSE(panel.treeViewSupported());
}

TEST(MonitorBuses, NoLegalFrameDoesNotKeepPreviousRows)
{
    ensureApp();
    auto snapshot = snapshotFrom({makeBusUpsert(9, 1, 10, 2, 0, 0)});
    MonitorBusesPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.visibleRowCount(), 1);
    MonitorCursorModel empty;
    panel.setSharedCursor(&empty);
    EXPECT_EQ(panel.tableState(), Monitor::MonitorBusesTableState::NoLegalFrame);
    EXPECT_EQ(panel.visibleRowCount(), 0);
}

TEST(MonitorBuses, DropSummaryMakesReplayIncomplete)
{
    ensureApp();
    auto snapshot = snapshotFrom({makeBusUpsert(9, 1, 10, 2, 1, 1)});
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

    MonitorBusesPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), Monitor::MonitorBusesTableState::Incomplete);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.table()->isHidden());
}

TEST(MonitorBuses, SameSnapshotGenerationOnRows)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeBusUpsert(9, 1, 10, 2, 1, 1),
        makeBusUpsert(11, 1, 10, 2, 0, 0),
    });
    MonitorBusesPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 2);
    EXPECT_EQ(panel.visibleRowAt(0)->storeGeneration, snapshot->generation);
    EXPECT_EQ(panel.visibleRowAt(1)->storeGeneration, snapshot->generation);
}
