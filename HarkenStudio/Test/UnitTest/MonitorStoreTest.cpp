#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorDecoder.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/support/MonitorFixtureLoader.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

using VNX::Monitor::decodePackedRecords;
using VNX::Monitor::MonitorDecodeBatch;
using VNX::Monitor::MonitorDecodedRecord;
using VNX::Monitor::MonitorStore;
using VNX::Monitor::MonitorStoreBudget;
using VNX::Monitor::MonitorStoreReject;
using VNX::Monitor::TestSupport::loadEditorFixture;
using VNX::Monitor::TestSupport::loadRuntimeLegalEvent;

namespace {

void resetBudget()
{
    MonitorStoreBudget::instance().resetForTests();
}
VNX::Monitor::MonitorCapabilities makeCaps(uint64_t sessionBytes, uint64_t processBytes)
{
    VNX::Monitor::MonitorCapabilities caps {};
    caps.resolvedConsumerQueueBytes = sessionBytes;
    caps.hardConsumerQueueBytes = processBytes;
    return caps;
}

MonitorDecodeBatch decodeNamed(const char* file)
{
    const auto fixture = loadEditorFixture(file);
    if (fixture.status != VN_OK)
    {
        MonitorDecodeBatch failed;
        failed.status = fixture.status;
        return failed;
    }
    return decodePackedRecords(fixture.packed.data(), fixture.packed.size(),
                               fixture.expectedRecords);
}

MonitorDecodeBatch decodeLegal()
{
    const auto fixture = loadRuntimeLegalEvent();
    EXPECT_EQ(fixture.status, VN_OK);
    return decodePackedRecords(fixture.packed.data(), fixture.packed.size(),
                               fixture.expectedRecords);
}

} // namespace

TEST(MonitorStore, RejectsFailedDecoderBatch)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    ASSERT_TRUE(store.usable());
    auto batch = decodeNamed("required_unknown.bin");
    ASSERT_NE(batch.status, VN_OK);
    const auto result = store.appendBatch(std::move(batch));
    EXPECT_EQ(result.reject, MonitorStoreReject::DecoderFailed);
    EXPECT_EQ(store.generation(), 0u);
    EXPECT_FALSE(store.snapshot());
}

TEST(MonitorStore, RejectsIncompleteBatch)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    auto batch = decodeLegal();
    ASSERT_EQ(batch.status, VN_OK);
    batch.recordsWritten = 99;
    const auto result = store.appendBatch(std::move(batch));
    EXPECT_EQ(result.reject, MonitorStoreReject::IncompleteBatch);
    EXPECT_EQ(store.generation(), 0u);
}

TEST(MonitorStore, AppendsLegalEventAndKeepsFields)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    auto batch = decodeLegal();
    ASSERT_EQ(batch.status, VN_OK);
    const auto result = store.appendBatch(std::move(batch));
    EXPECT_EQ(result.status, VN_OK);
    EXPECT_EQ(result.generation, 1u);
    const auto snap = store.snapshot();
    ASSERT_TRUE(snap);
    ASSERT_EQ(snap->records.size(), 1u);
    EXPECT_EQ(snap->records[0].header.recordKind, VN_MONITOR_RECORD_EVENT);
    EXPECT_EQ(snap->records[0].header.sourceSequence, 1u);
    EXPECT_EQ(snap->records[0].header.deliverySequence, 0u);
    EXPECT_EQ(snap->byStream[1].size(), 1u);
    EXPECT_TRUE(snap->byStream[0].empty());
    const auto byKind = snap->findByRecordKind(VN_MONITOR_RECORD_EVENT);
    ASSERT_EQ(byKind.size(), 1u);
    EXPECT_EQ(byKind[0].header.sourceSequence, 1u);
    EXPECT_FALSE(byKind[0].payload.empty());
    EXPECT_EQ(snap->findByExecuteFrame(snap->records[0].header.executeFrame).size(), 1u);
    EXPECT_EQ(snap->findByInstance(snap->records[0].header.instanceHandle,
                                   snap->records[0].header.instanceGeneration).size(), 1u);
}

TEST(MonitorStore, OptionalUnknownIsStoredButNotIndexed)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    auto batch = decodeNamed("optional_unknown.bin");
    ASSERT_EQ(batch.status, VN_OK);
    const auto result = store.appendBatch(std::move(batch));
    EXPECT_EQ(result.status, VN_OK);
    const auto snap = store.snapshot();
    ASSERT_TRUE(snap);
    ASSERT_EQ(snap->records.size(), 1u);
    EXPECT_FALSE(snap->records[0].skippedBytes.empty());
    EXPECT_TRUE(snap->byStream[0].empty());
    EXPECT_TRUE(snap->byStream[1].empty());
    EXPECT_TRUE(snap->byStream[2].empty());
    EXPECT_TRUE(snap->byStream[3].empty());
    EXPECT_TRUE(snap->byRecordKind.empty());
}

TEST(MonitorStore, DuplicateKeyRejectsWholeBatch)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    ASSERT_EQ(store.appendBatch(decodeLegal()).status, VN_OK);
    const auto second = store.appendBatch(decodeLegal());
    EXPECT_EQ(second.reject, MonitorStoreReject::DuplicateKey);
    EXPECT_EQ(store.generation(), 1u);
    EXPECT_EQ(store.snapshot()->records.size(), 1u);
}

TEST(MonitorStore, SessionCapacityRejectsWithoutPublishing)
{
    resetBudget();
    MonitorStore store(makeCaps(1, 1ull << 20));
    const auto result = store.appendBatch(decodeLegal());
    EXPECT_EQ(result.reject, MonitorStoreReject::CapacitySession);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(result.saturationCount, 1u);
    EXPECT_EQ(store.generation(), 0u);
    EXPECT_FALSE(store.snapshot());
}

TEST(MonitorStore, ProcessHardCapIsShared)
{
    resetBudget();
    MonitorStore first(makeCaps(1ull << 20, 400));
    ASSERT_EQ(first.appendBatch(decodeLegal()).status, VN_OK);
    MonitorStore second(makeCaps(1ull << 20, 400));
    const auto result = second.appendBatch(decodeLegal());
    EXPECT_EQ(result.reject, MonitorStoreReject::CapacityProcess);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(second.generation(), 0u);
}

TEST(MonitorStore, AllocatorDenyPublishesNothing)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20),
                       [](std::size_t) { return false; });
    const auto result = store.appendBatch(decodeLegal());
    EXPECT_EQ(result.reject, MonitorStoreReject::AllocatorDenied);
    EXPECT_EQ(store.generation(), 0u);
    EXPECT_FALSE(store.snapshot());
}

TEST(MonitorStore, SnapshotSurvivesLaterAppend)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    ASSERT_EQ(store.appendBatch(decodeLegal()).status, VN_OK);
    const auto oldSnap = store.snapshot();
    auto extra = decodeNamed("optional_unknown.bin");
    ASSERT_EQ(store.appendBatch(std::move(extra)).status, VN_OK);
    EXPECT_EQ(oldSnap->records.size(), 1u);
    EXPECT_EQ(store.snapshot()->records.size(), 2u);
    EXPECT_EQ(oldSnap->generation, 1u);
    EXPECT_EQ(store.generation(), 2u);
}

TEST(MonitorStore, ZeroResolvedRejects)
{
    resetBudget();
    MonitorStore store(makeCaps(0, 1ull << 20));
    ASSERT_TRUE(store.usable());
    const auto result = store.appendBatch(decodeLegal());
    EXPECT_EQ(result.reject, MonitorStoreReject::CapacitySession);
}

TEST(MonitorStore, MissingQueueBudgetsAreUnusable)
{
    resetBudget();
    VNX::Monitor::MonitorCapabilities caps {};
    caps.consumerQueueBudgetAvailable = false;
    MonitorStore store(caps);
    EXPECT_FALSE(store.usable());
    auto batch = decodeLegal();
    EXPECT_EQ(store.appendBatch(std::move(batch)).reject, MonitorStoreReject::InvalidArg);
}

TEST(MonitorStore, DropFieldsStayUntouched)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    auto batch = decodeLegal();
    const auto before = batch.records[0].header.telemetry.reserved0;
    ASSERT_EQ(store.appendBatch(std::move(batch)).status, VN_OK);
    EXPECT_EQ(store.snapshot()->records[0].header.telemetry.reserved0, before);
}

TEST(MonitorStore, FindByInstanceFiltersHashCollisions)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    auto batch = decodeLegal();
    ASSERT_EQ(batch.status, VN_OK);
    ASSERT_EQ(store.appendBatch(std::move(batch)).status, VN_OK);
    const auto snap = store.snapshot();
    ASSERT_TRUE(snap);
    const auto real = snap->records[0].header;
    const auto hits = snap->findByInstance(real.instanceHandle, real.instanceGeneration);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].header.instanceHandle, real.instanceHandle);
    EXPECT_EQ(hits[0].header.instanceGeneration, real.instanceGeneration);
    EXPECT_TRUE(snap->findByInstance(real.instanceHandle + 1, real.instanceGeneration).empty());
}

TEST(MonitorStore, SourcesStayDataLayer)
{
    const char* files[] = {
        "Source/Monitor/MonitorStore.cpp",
        "Source/Monitor/MonitorStore.h",
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
        EXPECT_EQ(text.find("VNX::Monitor::MonitorPollResult"), std::string::npos) << path;
    }
}
