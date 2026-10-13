#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/PlayableLifecycleProjection.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorPlayablesPanel.h"
#include "Widgets/Monitor/MonitorPlayablesSource.h"

#include <gtest/gtest.h>

#include <QApplication>
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
Monitor::MonitorDecodedRecord makePlayableUpsert(std::uint64_t handle,
                                                 std::uint64_t generation,
                                                 std::uint64_t frame)
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
    rec.header.entityKind = VN_MONITOR_ENTITY_PLAYABLE;
    rec.header.instanceHandle = handle;
    rec.header.instanceGeneration = generation;
    rec.header.reason = VN_ACTION_TERMINAL_NONE;
    VNX::Monitor::MonitorStateUpsert payload {};
    payload.structSize = sizeof(payload);
    payload.entityKind = VN_MONITOR_ENTITY_PLAYABLE;
    payload.instanceHandle = handle;
    payload.instanceGeneration = generation;
    rec.payload.resize(sizeof(payload));
    std::memcpy(rec.payload.data(), &payload, sizeof(payload));
    return rec;
}

std::shared_ptr<Monitor::MonitorStoreSnapshot> snapshotFrom(
    std::vector<Monitor::MonitorDecodedRecord> records)
{
    auto snapshot = std::make_shared<Monitor::MonitorStoreSnapshot>();
    snapshot->generation = 11;
    snapshot->records = std::move(records);
    for (Monitor::MonitorRecordId id = 0; id < snapshot->records.size(); ++id)
        snapshot->byRecordKind.emplace(snapshot->records[id].header.recordKind, id);
    return snapshot;
}

} // namespace

TEST(MonitorPlayables, NoLegalFrameDoesNotMintRows)
{
    ensureApp();
    auto snapshot = snapshotFrom({makePlayableUpsert(41, 2, 10)});
    MonitorPlayablesPanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    MonitorCursorModel empty;
    panel.setSharedCursor(&empty);
    EXPECT_EQ(panel.visibleRowCount(), 0);
}

TEST(MonitorPlayables, HostOwnedSourceWiresSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.advanceLive(12);
    MonitorPlayablesPanel panel;
    MonitorPlayablesSource source;
    source.attach(&panel, &shared);
    EXPECT_EQ(&panel.cursorModel(), &shared);
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    source.attach(&panel, nullptr);
    EXPECT_NE(&panel.cursorModel(), &shared);
    EXPECT_FALSE(panel.cursorModel().snapshot().executeFrame.has_value());
}

TEST(MonitorPlayables, SharedCursorMayDieBeforePanel)
{
    ensureApp();
    auto* shared = new MonitorCursorModel;
    MonitorPlayablesPanel panel;
    panel.setSharedCursor(shared);
    panel.setSharedCursor(nullptr);
    delete shared;
    EXPECT_EQ(panel.visibleRowCount(), 0);
}



TEST(MonitorPlayables, UnknownLayoutInstanceIsDropped)
{
    ensureApp();
    QVariantMap params;
    params.insert(QStringLiteral("kindFilter"), QStringLiteral("variableControl"));
    MonitorPlayablesPanel panel;
    panel.restoreParams(params);
    EXPECT_NE(panel.snapshotParams().value(QStringLiteral("kindFilter")).toString(),
              QStringLiteral("variableControl"));
}
