#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorDrop.h"
#include "Monitor/MonitorStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using VNX::Monitor::MonitorDecodedRecord;
using VNX::Monitor::MonitorDropProjection;
using VNX::Monitor::MonitorDropReasonKind;
using VNX::Monitor::MonitorRecordId;
using VNX::Monitor::MonitorStoreSnapshot;
using VNX::Monitor::MonitorSubscriptionProjection;
using VNX::Monitor::MonitorSubscriptionProjectionState;
using VNX::Monitor::projectDrop;

namespace {



bool hasKind(const MonitorDropProjection& out, MonitorDropReasonKind kind)
{
    return std::any_of(out.reasons.begin(), out.reasons.end(),
                       [kind](const auto& reason) { return reason.kind == kind; });
}
void addRecord(MonitorStoreSnapshot& snap, MonitorDecodedRecord rec)
{
    const auto id = static_cast<MonitorRecordId>(snap.records.size());
    snap.byRecordKind.emplace(rec.header.recordKind, id);
    snap.records.push_back(std::move(rec));
}

MonitorDecodedRecord eventOn(uint64_t seq, uint64_t streamId, uint64_t epoch)
{
    MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = VN_MONITOR_RECORD_EVENT;
    rec.header.streamKind = VN_MONITOR_STREAM_EVENT;
    rec.header.streamId = streamId;
    rec.header.sourceSequence = seq;
    rec.header.monitorSessionEpoch = epoch;
    return rec;
}

template <typename T>
MonitorDecodedRecord withPayload(uint32_t kind, uint64_t seq, uint64_t streamId, uint64_t epoch, const T& payload)
{
    MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = kind;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.streamId = streamId;
    rec.header.sourceSequence = seq;
    rec.header.monitorSessionEpoch = epoch;
    rec.payload.resize(sizeof(T));
    std::memcpy(rec.payload.data(), &payload, sizeof(T));
    return rec;
}

} // namespace

TEST(MonitorDrop, EmptySnapshotHasNoReasons)
{
    const auto out = projectDrop({});
    EXPECT_TRUE(out.reasons.empty());
}

TEST(MonitorDrop, NotConnectedDoesNotInventUnauthorizedGap)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    MonitorSubscriptionProjection subscription;
    subscription.state = MonitorSubscriptionProjectionState::NotConnected;
    const auto out = projectDrop(snap, subscription, 0);
    EXPECT_FALSE(hasKind(out, MonitorDropReasonKind::UnauthorizedNotGap));
    EXPECT_FALSE(hasKind(out, MonitorDropReasonKind::StageGap));
}

TEST(MonitorDrop, ConnectedDeniedIsNotStageGap)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    MonitorSubscriptionProjection subscription;
    subscription.state = MonitorSubscriptionProjectionState::Connected;
    subscription.status.acceptedPermissionMask = 0;
    const auto out = projectDrop(snap, subscription, 0);
    EXPECT_TRUE(hasKind(out, MonitorDropReasonKind::UnauthorizedNotGap));
    EXPECT_FALSE(hasKind(out, MonitorDropReasonKind::StageGap));
}

TEST(MonitorDrop, StoreLocalSaturationIsSeparateReason)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    const auto out = projectDrop(snap, {}, 3);
    ASSERT_TRUE(hasKind(out, MonitorDropReasonKind::StoreLocalSaturation));
    EXPECT_FALSE(hasKind(out, MonitorDropReasonKind::StageGap));
    for (const auto& reason : out.reasons)
    {
        if (reason.kind == MonitorDropReasonKind::StoreLocalSaturation)
        {
            EXPECT_EQ(reason.stage, 0u);
            EXPECT_EQ(reason.recordCount, 3u);
        }
    }
}

TEST(MonitorDrop, ZeroPayloadStreamIdIsMalformed)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    VNX::Monitor::MonitorDropSummary payload {};
    payload.structSize = sizeof(VNX::Monitor::MonitorDropSummary);
    payload.stage = VN_MONITOR_DROP_SOURCE_PRODUCER;
    payload.streamId = 0;
    payload.firstSourceSequence = 1;
    payload.lastSourceSequence = 2;
    addRecord(*snap, withPayload(VN_MONITOR_RECORD_DROP_SUMMARY, 1, 8, 10, payload));
    const auto out = projectDrop(snap);
    EXPECT_TRUE(hasKind(out, MonitorDropReasonKind::MalformedDrop));
    EXPECT_FALSE(hasKind(out, MonitorDropReasonKind::StageGap));
}

TEST(MonitorDrop, BaselineIsEpochScoped)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    addRecord(*snap, eventOn(1, 8, 11));
    VNX::Monitor::MonitorStreamBaseline baseline {};
    baseline.structSize = sizeof(VNX::Monitor::MonitorStreamBaseline);
    baseline.streamId = 8;
    baseline.firstExpectedSourceSequence = 1;
    addRecord(*snap, withPayload(VN_MONITOR_RECORD_STREAM_BASELINE, 1, 8, 10, baseline));
    const auto out = projectDrop(snap);
    bool missingOn11 = false;
    for (const auto& reason : out.reasons)
    {
        if (reason.kind == MonitorDropReasonKind::BaselineMissing &&
            reason.epoch == 11 && reason.streamId == 8)
            missingOn11 = true;
    }
    EXPECT_TRUE(missingOn11);
}

TEST(MonitorDrop, DoesNotWriteEngineStageOrWidgets)
{
    const char* files[] = {
        "Source/Monitor/MonitorDrop.cpp",
        "Source/Monitor/MonitorDrop.h",
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
