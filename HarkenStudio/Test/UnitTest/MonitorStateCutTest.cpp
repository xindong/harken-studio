#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorDrop.h"
#include "Monitor/MonitorStateCut.h"
#include "Monitor/MonitorStore.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using VNX::Monitor::MonitorCutViewStatus;
using VNX::Monitor::MonitorDecodedRecord;
using VNX::Monitor::MonitorDropReasonKind;
using VNX::Monitor::MonitorRecordId;
using VNX::Monitor::MonitorStoreSnapshot;
using VNX::Monitor::projectStateCut;

namespace {



template <typename T>
MonitorDecodedRecord makeRecord(uint32_t kind,
                                uint64_t seq,
                                uint64_t streamId,
                                uint64_t epoch,
                                uint64_t world,
                                const T& payload,
                                uint64_t handle = 0,
                                uint64_t generation = 0)
{
    MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = kind;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.streamId = streamId;
    rec.header.sourceSequence = seq;
    rec.header.monitorSessionEpoch = epoch;
    rec.header.worldGeneration = world;
    rec.header.instanceHandle = handle;
    rec.header.instanceGeneration = generation;
    rec.payload.resize(sizeof(T));
    std::memcpy(rec.payload.data(), &payload, sizeof(T));
    return rec;
}
void addRecord(MonitorStoreSnapshot& snap, MonitorDecodedRecord rec)
{
    const auto id = static_cast<MonitorRecordId>(snap.records.size());
    snap.byRecordKind.emplace(rec.header.recordKind, id);
    snap.records.push_back(std::move(rec));
}

VNX::Monitor::MonitorStateCutBegin beginPayload(uint64_t cutId, uint64_t world)
{
    VNX::Monitor::MonitorStateCutBegin payload {};
    payload.structSize = sizeof(VNX::Monitor::MonitorStateCutBegin);
    payload.stateCutId = cutId;
    payload.worldGeneration = world;
    return payload;
}

VNX::Monitor::MonitorStateUpsert upsertPayload(uint64_t cutId, uint64_t handle, uint64_t generation)
{
    VNX::Monitor::MonitorStateUpsert payload {};
    payload.structSize = sizeof(VNX::Monitor::MonitorStateUpsert);
    payload.stateCutId = cutId;
    payload.instanceHandle = handle;
    payload.instanceGeneration = generation;
    payload.entityKind = VN_MONITOR_ENTITY_VOICE;
    return payload;
}

VNX::Monitor::MonitorStateTombstone tombPayload(uint64_t cutId, uint64_t handle, uint64_t generation)
{
    VNX::Monitor::MonitorStateTombstone payload {};
    payload.structSize = sizeof(VNX::Monitor::MonitorStateTombstone);
    payload.stateCutId = cutId;
    payload.instanceHandle = handle;
    payload.instanceGeneration = generation;
    payload.entityKind = VN_MONITOR_ENTITY_VOICE;
    return payload;
}

VNX::Monitor::MonitorStateCutEnd endPayload(uint64_t cutId, uint32_t result)
{
    VNX::Monitor::MonitorStateCutEnd payload {};
    payload.structSize = sizeof(VNX::Monitor::MonitorStateCutEnd);
    payload.stateCutId = cutId;
    payload.result = result;
    return payload;
}

VNX::Monitor::MonitorDropSummary dropPayload(uint32_t stage, uint64_t streamId, uint64_t first, uint64_t last)
{
    VNX::Monitor::MonitorDropSummary payload {};
    payload.structSize = sizeof(VNX::Monitor::MonitorDropSummary);
    payload.stage = stage;
    payload.streamId = streamId;
    payload.firstSourceSequence = first;
    payload.lastSourceSequence = last;
    payload.recordCount = 1;
    return payload;
}

std::shared_ptr<MonitorStoreSnapshot> completeWorld()
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 8, 10, 2,
                                beginPayload(7, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 2, 8, 10, 2,
                                upsertPayload(7, 100, 1), 100, 1));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_END, 3, 8, 10, 2,
                                endPayload(7, VN_MONITOR_STATE_CUT_COMPLETE)));
    return snap;
}

} // namespace

TEST(MonitorStateCut, EmptySnapshotIsNone)
{
    const auto out = projectStateCut({});
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::None);
    EXPECT_FALSE(out.mandatoryResync);
    EXPECT_TRUE(out.current.objects.empty());
}

TEST(MonitorStateCut, EmptyStoreSnapshotIsNone)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    const auto out = projectStateCut(snap);
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::None);
    EXPECT_STREQ(out.current.reason, "no-cut-records");
}

TEST(MonitorStateCut, CompletePublishesCurrentWorld)
{
    const auto out = projectStateCut(completeWorld());
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::Complete);
    ASSERT_EQ(out.current.objects.size(), 1u);
    EXPECT_EQ(out.current.objects[0].key.instanceHandle, 100u);
    EXPECT_FALSE(out.current.objects[0].tombstoned);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::None);
}

TEST(MonitorStateCut, MissingEndDoesNotPublishNewWorld)
{
    auto snap = completeWorld();
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 4, 8, 10, 2,
                                beginPayload(8, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 5, 8, 10, 2,
                                upsertPayload(8, 200, 1), 200, 1));
    const auto out = projectStateCut(snap);
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::Complete);
    ASSERT_EQ(out.current.objects.size(), 1u);
    EXPECT_EQ(out.current.objects[0].key.instanceHandle, 100u);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::Truncated);
    EXPECT_EQ(out.pending.objects.size(), 1u);
    EXPECT_EQ(out.pending.objects[0].key.instanceHandle, 200u);
}

TEST(MonitorStateCut, WrongCutIdIsInvalidAndKeepsPreviousWorld)
{
    auto snap = completeWorld();
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 4, 8, 10, 2,
                                beginPayload(8, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 5, 8, 10, 2,
                                upsertPayload(99, 200, 1), 200, 1));
    const auto out = projectStateCut(snap);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::Invalid);
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::Complete);
    ASSERT_EQ(out.current.objects.size(), 1u);
    EXPECT_EQ(out.current.objects[0].key.instanceHandle, 100u);
}

TEST(MonitorStateCut, InterleavedStreamsDoNotShareOpenCut)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 8, 10, 2,
                                beginPayload(7, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 9, 10, 2,
                                beginPayload(8, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 2, 9, 10, 2,
                                upsertPayload(8, 200, 1), 200, 1));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_END, 3, 9, 10, 2,
                                endPayload(8, VN_MONITOR_STATE_CUT_COMPLETE)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 2, 8, 10, 2,
                                upsertPayload(7, 100, 1), 100, 1));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_END, 3, 8, 10, 2,
                                endPayload(7, VN_MONITOR_STATE_CUT_COMPLETE)));
    const auto out = projectStateCut(snap);
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::Complete);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::None);
    ASSERT_EQ(out.current.objects.size(), 2u);
    bool saw100 = false;
    bool saw200 = false;
    for (const auto& object : out.current.objects)
    {
        if (object.key.instanceHandle == 100)
            saw100 = true;
        if (object.key.instanceHandle == 200)
            saw200 = true;
    }
    EXPECT_TRUE(saw100);
    EXPECT_TRUE(saw200);
}

TEST(MonitorStateCut, MissingEndOnEarlierStreamDoesNotBlockLaterComplete)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 8, 10, 2,
                                beginPayload(7, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 2, 8, 10, 2,
                                upsertPayload(7, 100, 1), 100, 1));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 9, 10, 2,
                                beginPayload(8, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 2, 9, 10, 2,
                                upsertPayload(8, 200, 1), 200, 1));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_END, 3, 9, 10, 2,
                                endPayload(8, VN_MONITOR_STATE_CUT_COMPLETE)));
    const auto out = projectStateCut(snap);
    EXPECT_EQ(out.current.status, MonitorCutViewStatus::Complete);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::Truncated);
    EXPECT_STREQ(out.pending.reason, "missing-end");
    ASSERT_EQ(out.pending.objects.size(), 1u);
    EXPECT_EQ(out.pending.objects[0].key.instanceHandle, 100u);
    ASSERT_EQ(out.current.objects.size(), 1u);
    EXPECT_EQ(out.current.objects[0].key.instanceHandle, 200u);
}

TEST(MonitorStateCut, ZeroStructSizeIsInvalid)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    auto begin = beginPayload(7, 2);
    begin.structSize = 0;
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 8, 10, 2, begin));
    const auto out = projectStateCut(snap);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::Invalid);
}

TEST(MonitorStateCut, SourceFanoutDropPreventsComplete)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_BEGIN, 1, 8, 10, 2,
                                beginPayload(7, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_UPSERT, 2, 8, 10, 2,
                                upsertPayload(7, 100, 1), 100, 1));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_DROP_SUMMARY, 3, 8, 10, 2,
                                dropPayload(VN_MONITOR_DROP_HUB_FANOUT, 8, 1, 2)));
    addRecord(*snap, makeRecord(VN_MONITOR_RECORD_STATE_CUT_END, 4, 8, 10, 2,
                                endPayload(7, VN_MONITOR_STATE_CUT_COMPLETE)));
    const auto out = projectStateCut(snap);
    EXPECT_NE(out.current.status, MonitorCutViewStatus::Complete);
    EXPECT_EQ(out.pending.status, MonitorCutViewStatus::Truncated);
}

TEST(MonitorStateCut, DoesNotCallControlSurfaceOrWidgets)
{
    const char* files[] = {
        "Source/Monitor/MonitorStateCut.cpp",
        "Source/Monitor/MonitorStateCut.h",
        "Source/Monitor/MonitorProjectionTypes.h",
        "Source/Monitor/CMakeLists.txt",
    };
    for (const char* file : files)
    {
        const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/" + file;
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("Qt6::Widgets"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorRequestStateCut"), std::string::npos) << path;
    }
}
