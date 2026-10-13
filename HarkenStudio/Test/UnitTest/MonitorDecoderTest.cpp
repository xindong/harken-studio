#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorDecoder.h"
#include "Monitor/support/MonitorFixtureLoader.h"
#include "MonitorRecordPack.h"

#include <gtest/gtest.h>

#include <fstream>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using VNX::Monitor::decodePackedRecords;
using VNX::Monitor::MonitorDecodeBatch;
using VNX::Monitor::TestSupport::loadEditorFixture;
using VNX::Monitor::TestSupport::loadRuntimeLegalEvent;

namespace {



} // namespace

TEST(MonitorDecoder, LegalEventFromCommittedFixture)
{
    const auto fixture = loadRuntimeLegalEvent();
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_OK);
    EXPECT_EQ(decoded.recordsWritten, 1u);
    EXPECT_EQ(decoded.bytesConsumed, fixture.packed.size());
    ASSERT_EQ(decoded.records.size(), 1u);
    EXPECT_EQ(decoded.records[0].header.structSize, sizeof(VNX::Monitor::MonitorRecordHeader));
    EXPECT_EQ(decoded.records[0].header.recordKind, VN_MONITOR_RECORD_EVENT);
    EXPECT_EQ(decoded.records[0].header.sourceSequence, 1u);
    EXPECT_EQ(decoded.records[0].header.deliverySequence, 0u);
    EXPECT_EQ(decoded.records[0].header.monitorSessionEpoch, 1u);
    EXPECT_FALSE(decoded.records[0].payload.empty());
    EXPECT_TRUE(decoded.records[0].skippedBytes.empty());
}
TEST(MonitorDecoder, EmptyBatchIsZeroRecords)
{
    const auto decoded = decodePackedRecords(nullptr, 0, 0);
    EXPECT_EQ(decoded.status, VN_OK);
    EXPECT_EQ(decoded.recordsWritten, 0u);
    EXPECT_TRUE(decoded.records.empty());
}

TEST(MonitorDecoder, OptionalUnknownSkipsAndKeepsBytes)
{
    const auto fixture = loadEditorFixture("optional_unknown.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_OK);
    ASSERT_EQ(decoded.records.size(), 1u);
    EXPECT_TRUE(decoded.records[0].payload.empty());
    EXPECT_EQ(decoded.records[0].skippedBytes.size(), decoded.records[0].header.recordBytes);
}

TEST(MonitorDecoder, RequiredUnknownFailsWholeBatch)
{
    const auto fixture = loadEditorFixture("required_unknown.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
    EXPECT_TRUE(decoded.records.empty());
}

TEST(MonitorDecoder, LegalThenRequiredUnknownDropsPrefix)
{
    const auto fixture = loadEditorFixture("legal_then_required_unknown.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
    EXPECT_TRUE(decoded.records.empty());
}

TEST(MonitorDecoder, ShortHeaderIsIncompatible)
{
    const auto fixture = loadEditorFixture("short_header.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, TruncatedRecordIsIncompatible)
{
    const auto fixture = loadEditorFixture("truncated_record.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, CountBytesMismatchIsIncompatible)
{
    const auto fixture = loadEditorFixture("count_bytes_mismatch.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, NativeLayoutIsIncompatible)
{
    const auto fixture = loadEditorFixture("native_layout.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, BigEndianCountPrefixIsRejectedByLoader)
{
    const auto fixture = loadEditorFixture("be_count_prefix.bin");
    EXPECT_EQ(fixture.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_TRUE(fixture.packed.empty());
}

TEST(MonitorDecoder, EmptyEditorFixtureIsZeroRecords)
{
    const auto fixture = loadEditorFixture("empty_batch.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_EQ(decoded.status, VN_OK);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, NullBytesWithCountIsInvalidArg)
{
    const auto decoded = decodePackedRecords(nullptr, 16, 1);
    EXPECT_EQ(decoded.status, VN_INVALID_ARG);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, SourcesDoNotLinkWidgetsOrFixtureDecoder)
{
    const char* files[] = {
        "Source/Monitor/MonitorDecoder.cpp",
        "Source/Monitor/MonitorDecoder.h",
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
        EXPECT_EQ(text.find("monitor_fixture_decode"), std::string::npos) << path;
        EXPECT_EQ(text.find("setPrincipal"), std::string::npos) << path;
    }
}

TEST(MonitorDecoder, AllocatorDenyFirstPublishesNothing)
{
    const auto fixture = loadRuntimeLegalEvent();
    ASSERT_EQ(fixture.status, VN_OK);
    std::vector<std::size_t> sizes;
    const auto decoded = decodePackedRecords(
        fixture.packed.data(),
        fixture.packed.size(),
        fixture.expectedRecords,
        [&](std::size_t bytes) {
            sizes.push_back(bytes);
            return false;
        });
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
    EXPECT_TRUE(decoded.records.empty());
    ASSERT_EQ(sizes.size(), 1u);
    EXPECT_EQ(sizes[0], static_cast<std::size_t>(fixture.packed.size()));
}

TEST(MonitorDecoder, AllocatorDenyMiddlePublishesNothing)
{
    const auto fixture = loadRuntimeLegalEvent();
    ASSERT_EQ(fixture.status, VN_OK);
    std::vector<std::size_t> sizes;
    const auto decoded = decodePackedRecords(
        fixture.packed.data(),
        fixture.packed.size(),
        fixture.expectedRecords,
        [&](std::size_t bytes) {
            sizes.push_back(bytes);
            return sizes.size() != 2u;
        });
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
    EXPECT_TRUE(decoded.records.empty());
    ASSERT_EQ(sizes.size(), 2u);
}

TEST(MonitorDecoder, AllocatorDenyLastPublishesNothing)
{
    const auto fixture = loadRuntimeLegalEvent();
    ASSERT_EQ(fixture.status, VN_OK);
    std::vector<std::size_t> sizes;
    const auto decoded = decodePackedRecords(
        fixture.packed.data(),
        fixture.packed.size(),
        fixture.expectedRecords,
        [&](std::size_t bytes) {
            sizes.push_back(bytes);
            return sizes.size() < 3u;
        });
    EXPECT_EQ(decoded.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(decoded.recordsWritten, 0u);
    EXPECT_TRUE(decoded.records.empty());
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_GT(sizes.back(), 0u);
    EXPECT_NE(sizes.back(), static_cast<std::size_t>(fixture.packed.size()));
}

TEST(MonitorDecoder, EventDetailVersionsKeepIdentityAndRejectMismatchedPayloads)
{
    for (const auto version : {1u, 2u}) {
        SCOPED_TRACE(version);
        VNX::Monitor::MonitorEventDetailV2 detail{};
        detail.structSize = version == 1 ? sizeof(VNX::Monitor::MonitorEventDetail) : sizeof(detail);
        detail.category = VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC;
        detail.code = 42;
        detail.level = VN_LOG_WARN;
        detail.timestampNs = 123456789;
        detail.textBytes = 4;
        std::copy_n("warn", 4, detail.text);
        detail.projectGuid[0] = 19;
        VnMonitorRecordHeader header{};
        const auto bytes = harken::monitor::PackedRecordBytes(detail.structSize);
        harken::monitor::FillSyntheticHeader(&header, 1, bytes, detail.structSize,
            VN_MONITOR_RECORD_EVENT_DETAIL, 0);
        header.recordVersion = version;
        header.streamKind = VN_MONITOR_STREAM_EVENT;
        header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
        std::vector<std::uint8_t> packed(bytes);
        harken::monitor::PackHeader(packed.data(), header);
        std::memcpy(packed.data() + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, &detail, detail.structSize);
        auto batch = decodePackedRecords(packed.data(), packed.size(), 1);
        ASSERT_EQ(batch.status, VN_OK);
        ASSERT_EQ(batch.records.size(), 1u);
        auto& record = batch.records.front();
        VNX::Monitor::MonitorEventDetail decoded{};
        VNX::Monitor::MonitorEventDetailV2 identity{};
        ASSERT_TRUE(VNX::Monitor::decodeEventDetail(record, &decoded, &identity));
        EXPECT_EQ(decoded.timestampNs, detail.timestampNs);
        EXPECT_EQ(decoded.level, VN_LOG_WARN);
        EXPECT_EQ(std::string(decoded.text, decoded.textBytes), "warn");
        EXPECT_EQ(identity.projectGuid[0], version == 2 ? 19u : 0u);
        record.payload.push_back(0);
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(record, &decoded));
        record.payload.pop_back();
        record.payload.pop_back();
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(record, &decoded));
        record.payload.push_back(0);
        record.header.recordVersion = version == 1 ? 2 : 1;
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(record, &decoded));
        record.header.recordVersion = version;
        record.payload[20] = 1;
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(record, &decoded));
        record.payload[20] = 0;
        record.payload[52] = 193;
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(record, &decoded));
        record.payload[52] = 4;
        record.payload[60] = 1;
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(record, &decoded));

        packed[4] = 3;
        const auto future = decodePackedRecords(packed.data(), packed.size(), 1);
        ASSERT_EQ(future.status, VN_OK);
        ASSERT_EQ(future.records.size(), 1u);
        EXPECT_TRUE(future.records.front().unknownOptional);
        EXPECT_FALSE(VNX::Monitor::decodeEventDetail(future.records.front(), &decoded));
    }
}
