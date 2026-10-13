#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorStore.h"
#include "Widgets/Monitor/MonitorVoicesPanel.h"
#include "Widgets/Monitor/MonitorVoicesSource.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTableView>

#include <algorithm>
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
Monitor::MonitorDecodedRecord makeVoiceRecord(std::uint32_t kind,
                                              std::uint64_t handle,
                                              std::uint64_t generation,
                                              std::uint64_t frame,
                                              std::uint64_t sequence,
                                              std::uint32_t reason,
                                              bool numeric,
                                              float volumeDb,
                                              std::uint32_t extraValidity = 0)
{
    Monitor::MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = kind;
    rec.header.recordVersion = 1;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.sourceSequence = sequence;
    rec.header.streamId = 8;
    rec.header.monitorSessionEpoch = 10;
    rec.header.executeFrame = frame;
    rec.header.entityKind = VN_MONITOR_ENTITY_VOICE;
    rec.header.instanceHandle = handle;
    rec.header.instanceGeneration = generation;
    rec.header.playableInstanceHandle = 0;
    rec.header.reason = reason;
    rec.header.validityFlags =
        extraValidity | (numeric ? VN_MONITOR_VALID_NUMERIC : 0u);
    rec.header.telemetry.occurrenceOrdinal = 0;
    if (kind == VN_MONITOR_RECORD_STATE_UPSERT)
    {
        VNX::Monitor::MonitorStateUpsert payload {};
        payload.structSize = sizeof(payload);
        payload.entityKind = VN_MONITOR_ENTITY_VOICE;
        payload.instanceHandle = handle;
        payload.instanceGeneration = generation;
        payload.volumeDb = volumeDb;
        rec.payload.resize(sizeof(payload));
        std::memcpy(rec.payload.data(), &payload, sizeof(payload));
    }
    else
    {
        VNX::Monitor::MonitorStateTombstone payload {};
        payload.structSize = sizeof(payload);
        payload.entityKind = VN_MONITOR_ENTITY_VOICE;
        payload.instanceHandle = handle;
        payload.instanceGeneration = generation;
        payload.reason = reason;
        rec.payload.resize(sizeof(payload));
        std::memcpy(rec.payload.data(), &payload, sizeof(payload));
    }
    return rec;
}

void addBaseline(Monitor::MonitorStoreSnapshot& snapshot,
                 std::uint64_t streamId = 8,
                 std::uint64_t epoch = 10)
{
    Monitor::MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = VN_MONITOR_RECORD_STREAM_BASELINE;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.streamId = streamId;
    rec.header.monitorSessionEpoch = epoch;
    rec.header.sourceSequence = 0;
    rec.header.executeFrame = 0;
    VNX::Monitor::MonitorStreamBaseline payload {};
    payload.structSize = sizeof(payload);
    payload.streamId = streamId;
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
    snapshot->generation = 11;
    snapshot->records = std::move(records);
    for (Monitor::MonitorRecordId id = 0; id < snapshot->records.size(); ++id)
        snapshot->byRecordKind.emplace(snapshot->records[id].header.recordKind, id);
    addBaseline(*snapshot);
    return snapshot;
}

} // namespace

TEST(MonitorVoices, NoLegalFrameDoesNotKeepPreviousRows)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 41, 2, 10, 1,
                        VN_ACTION_TERMINAL_NONE, true, -12.5f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), MonitorVoicesTableState::Rows);
    EXPECT_EQ(panel.visibleRowCount(), 1);

    MonitorCursorModel empty;
    panel.setSharedCursor(&empty);
    EXPECT_EQ(panel.tableState(), MonitorVoicesTableState::NoLegalFrame);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.table()->isHidden());
}

TEST(MonitorVoices, VoiceStreamWithoutBaselineIsIncomplete)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_NONE, true, -3.0f),
    });
    snapshot->records.erase(
        std::remove_if(snapshot->records.begin(), snapshot->records.end(),
                       [](const Monitor::MonitorDecodedRecord& rec) {
                           return rec.header.recordKind == VN_MONITOR_RECORD_STREAM_BASELINE;
                       }),
        snapshot->records.end());
    snapshot->byRecordKind.clear();
    for (Monitor::MonitorRecordId id = 0; id < snapshot->records.size(); ++id)
        snapshot->byRecordKind.emplace(snapshot->records[id].header.recordKind, id);

    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(5);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), MonitorVoicesTableState::Incomplete);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.table()->isHidden());
}

TEST(MonitorVoices, DropSummaryMakesReplayIncomplete)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_NONE, true, -3.0f),
    });
    Monitor::MonitorDecodedRecord drop;
    drop.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    drop.header.recordKind = VN_MONITOR_RECORD_DROP_SUMMARY;
    drop.header.streamKind = VN_MONITOR_STREAM_STATE;
    drop.header.sourceSequence = 2;
    drop.header.executeFrame = 5;
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

    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(5);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), MonitorVoicesTableState::Incomplete);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.table()->isHidden());
}

TEST(MonitorVoices, EmptyFrameIsNotUnsupported)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 41, 2, 10, 1,
                        VN_ACTION_TERMINAL_NONE, true, -12.5f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_TOMBSTONE, 41, 2, 11, 2,
                        VN_ACTION_TERMINAL_NATURAL, false, 0.0f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(11);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.tableState(), MonitorVoicesTableState::EmptyFrame);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_FALSE(panel.table()->isHidden());
}

TEST(MonitorVoices, ReplayKeepsOneRowAndDropsEnded)
{
    ensureApp();
    const std::uint64_t handle = (2ull << 32) | 7ull;
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, handle, 2, 1, 1,
                        VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE, false, 0.0f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, handle, 2, 2, 2,
                        VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE, false, 0.0f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, handle, 2, 3, 3,
                        VN_ACTION_TERMINAL_NONE, true, -12.5f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_TOMBSTONE, handle, 2, 4, 4,
                        VN_ACTION_TERMINAL_NATURAL, false, 0.0f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(3);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    const auto* parked = panel.visibleRowAt(0);
    ASSERT_NE(parked, nullptr);
    EXPECT_EQ(parked->instanceHandle, handle);
    EXPECT_EQ(parked->instanceGeneration, 2u);
    EXPECT_EQ(parked->volumeState, MonitorVoicesVolumeState::Numeric);
    EXPECT_FLOAT_EQ(parked->volumeDb, -12.5f);
    EXPECT_EQ(MonitorVoicesModel::identityText(*parked),
              QStringLiteral("%1:2").arg(handle));

    MonitorCursorModel shared;
    shared.setRetainedWindow(1, 5);
    shared.advanceLive(3);
    panel.setSharedCursor(&shared);
    EXPECT_EQ(shared.lockTo(4), MonitorCursorReject::None);
    EXPECT_EQ(panel.tableState(), MonitorVoicesTableState::EmptyFrame);
    EXPECT_EQ(panel.visibleRowCount(), 0);
}

TEST(MonitorVoices, GenerationChangeIsNewRow)
{
    ensureApp();
    const std::uint64_t handleA = (2ull << 32) | 7ull;
    const std::uint64_t handleB = (3ull << 32) | 7ull;
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, handleA, 2, 1, 1,
                        VN_ACTION_TERMINAL_NONE, true, -6.0f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_TOMBSTONE, handleA, 2, 2, 2,
                        VN_ACTION_TERMINAL_NATURAL, false, 0.0f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, handleB, 3, 3, 3,
                        VN_ACTION_TERMINAL_NONE, true, -9.0f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(3);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.visibleRowAt(0)->instanceGeneration, 3u);
    EXPECT_EQ(panel.visibleRowAt(0)->instanceHandle, handleB);
}

TEST(MonitorVoices, LiveVolumeStaysGap)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE, false, 0.0f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(5);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.visibleRowAt(0)->volumeState, MonitorVoicesVolumeState::Gap);
}

TEST(MonitorVoices, UnsubscribedVolumeBeatsGap)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_NONE, true, -3.0f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(5);
    panel.bindSnapshot(snapshot, false);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.visibleRowAt(0)->volumeState, MonitorVoicesVolumeState::Unsubscribed);
}

TEST(MonitorVoices, UnsupportedVolumeBeatsGapAndNumeric)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_NONE, true, -3.0f,
                        VN_MONITOR_VALID_UNSUPPORTED),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(5);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.visibleRowAt(0)->volumeState, MonitorVoicesVolumeState::Unsupported);
}

TEST(MonitorVoices, UnsubscribedVolumeBeatsUnsupported)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_NONE, false, 0.0f,
                        VN_MONITOR_VALID_UNSUPPORTED),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(5);
    panel.bindSnapshot(snapshot, false);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.visibleRowAt(0)->volumeState, MonitorVoicesVolumeState::Unsubscribed);
}

TEST(MonitorVoices, SharedCursorReattachKeepsSameModel)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.setRetainedWindow(1, 9);
    shared.advanceLive(8);
    MonitorVoicesPanel voices;
    MonitorVoicesSource voiceSource;
    voiceSource.attach(&voices, &shared);
    voiceSource.attach(&voices, &shared);
    EXPECT_EQ(&voices.cursorModel(), &shared);
    EXPECT_TRUE(shared.advanceLive(3));
    EXPECT_EQ(voices.cursorModel().snapshot().executeFrame, 3);
}

TEST(MonitorVoices, SharedOwnedSharedSwitchUnsubscribesOldCursor)
{
    ensureApp();
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 5, 1,
                        VN_ACTION_TERMINAL_NONE, true, -3.0f),
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, 9, 1, 6, 2,
                        VN_ACTION_TERMINAL_NONE, true, -6.0f),
    });
    MonitorVoicesPanel panel;
    MonitorCursorModel first;
    first.setRetainedWindow(1, 8);
    first.advanceLive(5);
    panel.setSharedCursor(&first);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_FLOAT_EQ(panel.visibleRowAt(0)->volumeDb, -3.0f);

    panel.setSharedCursor(nullptr);
    panel.cursorModel().advanceLive(6);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_FLOAT_EQ(panel.visibleRowAt(0)->volumeDb, -6.0f);

    MonitorCursorModel second;
    second.setRetainedWindow(1, 8);
    second.advanceLive(5);
    panel.setSharedCursor(&second);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_FLOAT_EQ(panel.visibleRowAt(0)->volumeDb, -3.0f);

    EXPECT_TRUE(first.advanceLive(6));
    EXPECT_FLOAT_EQ(panel.visibleRowAt(0)->volumeDb, -3.0f);
    EXPECT_EQ(second.lockTo(6), MonitorCursorReject::None);
    EXPECT_FLOAT_EQ(panel.visibleRowAt(0)->volumeDb, -6.0f);
}

TEST(MonitorVoices, SelectedRowKeyIsHandleAndGeneration)
{
    ensureApp();
    const std::uint64_t handle = (4ull << 32) | 11ull;
    auto snapshot = snapshotFrom({
        makeVoiceRecord(VN_MONITOR_RECORD_STATE_UPSERT, handle, 4, 6, 1,
                        VN_ACTION_TERMINAL_NONE, true, -1.0f),
    });
    MonitorVoicesPanel panel;
    panel.cursorModel().advanceLive(6);
    panel.bindSnapshot(snapshot);
    ASSERT_EQ(panel.visibleRowCount(), 1);
    panel.table()->selectRow(0);
    const auto* selected = panel.visibleRowAt(0);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->instanceHandle, handle);
    EXPECT_EQ(selected->instanceGeneration, 4u);
}

TEST(MonitorVoices, SourcesStayReadOnly)
{
    const char* files[] = {
        "Source/Widgets/Monitor/MonitorVoicesPanel.cpp",
        "Source/Widgets/Monitor/MonitorVoicesSource.cpp",
    };
    for (const char* file : files)
    {
        const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/" + file;
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("Vn_MonitorApplyCapturePlan"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorAcquireCaptureControl"), std::string::npos) << path;
        EXPECT_EQ(text.find("MonitorAdapter::open"), std::string::npos) << path;
    }
}
