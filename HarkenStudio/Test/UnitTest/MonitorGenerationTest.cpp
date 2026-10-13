#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorGeneration.h"
#include "Monitor/MonitorStore.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using VNX::Monitor::MonitorDecodedRecord;
using VNX::Monitor::MonitorGenerationStatus;
using VNX::Monitor::MonitorRecordId;
using VNX::Monitor::MonitorStoreSnapshot;
using VNX::Monitor::projectGeneration;

namespace {



MonitorDecodedRecord activated(uint64_t seq,
                               uint64_t streamId,
                               uint64_t epoch,
                               uint64_t plan,
                               uint64_t frame)
{
    MonitorDecodedRecord rec;
    rec.header.structSize = sizeof(VNX::Monitor::MonitorRecordHeader);
    rec.header.recordKind = VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED;
    rec.header.streamKind = VN_MONITOR_STREAM_STATE;
    rec.header.streamId = streamId;
    rec.header.sourceSequence = seq;
    rec.header.monitorSessionEpoch = epoch;
    rec.header.capturePlanGeneration = plan;
    rec.header.executeFrame = frame;
    return rec;
}
void addRecord(MonitorStoreSnapshot& snap, MonitorDecodedRecord rec)
{
    const auto id = static_cast<MonitorRecordId>(snap.records.size());
    snap.byRecordKind.emplace(rec.header.recordKind, id);
    snap.records.push_back(std::move(rec));
}

} // namespace

TEST(MonitorGeneration, EmptySnapshotMarksActivationMissing)
{
    const auto out = projectGeneration({});
    EXPECT_EQ(out.status, MonitorGenerationStatus::ActivationMissing);
    EXPECT_TRUE(out.bounds.empty());
}

TEST(MonitorGeneration, EmptyStoreSnapshotMarksActivationMissing)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    const auto out = projectGeneration(snap);
    EXPECT_EQ(out.status, MonitorGenerationStatus::ActivationMissing);
    EXPECT_STREQ(out.reason, "activation-missing");
}

TEST(MonitorGeneration, BoundsArePerEpochAndStreamFromHeaderExecuteFrame)
{
    auto snap = std::make_shared<MonitorStoreSnapshot>();
    addRecord(*snap, activated(1, 8, 10, 3, 100));
    addRecord(*snap, activated(2, 8, 10, 4, 200));
    addRecord(*snap, activated(1, 9, 10, 5, 50));
    const auto out = projectGeneration(snap);
    EXPECT_EQ(out.status, MonitorGenerationStatus::Bounded);
    ASSERT_EQ(out.bounds.size(), 2u);
    EXPECT_EQ(out.bounds[0].streamId, 8u);
    EXPECT_EQ(out.bounds[0].capturePlanGeneration, 4u);
    EXPECT_EQ(out.bounds[0].executeFrame, 200u);
    EXPECT_EQ(out.bounds[1].streamId, 9u);
    EXPECT_EQ(out.bounds[1].executeFrame, 50u);
    EXPECT_EQ(snap->records[0].header.capturePlanGeneration, 3u);
}

TEST(MonitorGeneration, DoesNotReadControlPlanOrWidgets)
{
    const char* files[] = {
        "Source/Monitor/MonitorGeneration.cpp",
        "Source/Monitor/MonitorGeneration.h",
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
        EXPECT_EQ(text.find("Vn_MonitorApplyCapturePlan"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorAcquireCaptureControl"), std::string::npos) << path;
    }
}
