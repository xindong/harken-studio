#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorDecoder.h"
#include "Monitor/support/MonitorFixtureLoader.h"
#include "MonitorRecordPack.h"
#include "MonitorExecutionCodec.h"

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

TEST(MonitorDecoder, ReadsEngineExecutionIdentityWithoutNativeLayoutAssumptions)
{
    for (const auto subject : {VN_MONITOR_SUBJECT_PLAYABLE, VN_MONITOR_SUBJECT_ACTION, VN_MONITOR_SUBJECT_LOGICAL_VOICE}) {
        SCOPED_TRACE(subject);
        harken::MonitorExecutionFact source{};
        source.engine = 19;
        source.sourceSequence = 31;
        source.kind = VN_MONITOR_FACT_PLAYABLE_ACTIVE;
        source.subjectNamespace = subject;
        source.contentKind = VN_MONITOR_CONTENT_SOUND;
        const uint64_t generation = subject == VN_MONITOR_SUBJECT_ACTION ? (UINT64_C(1) << 40) + 7 : 7;
        source.body.execution.identity.handle = (generation << (subject == VN_MONITOR_SUBJECT_ACTION ? 12 : 32)) | 3;
        source.body.execution.identity.project = {UINT64_C(0x123456789abcdef0), UINT64_C(0x1020304050607080)};
        source.body.execution.identity.object = {41, 59};
        source.timestamp = 123456789;
        for (const auto field : {VN_MONITOR_FACT_FIELD_SUBJECT, VN_MONITOR_FACT_FIELD_PROJECT,
                                VN_MONITOR_FACT_FIELD_CONTENT_OBJECT, VN_MONITOR_FACT_FIELD_TIMESTAMP})
            ASSERT_TRUE(harken::monitor::SetSourceField(source, field));
        std::vector<uint8_t> packed(harken::monitor::kExecutionRecordBytes);
        ASSERT_EQ(harken::monitor::PackExecutionFact(packed.data(), packed.size(), source, 71, 72), packed.size());
        const auto batch = decodePackedRecords(packed.data(), packed.size(), 1);
        ASSERT_EQ(batch.status, VN_OK);
        ASSERT_EQ(batch.records.size(), 1u);
        VNX::Monitor::MonitorExecutionFactV1 fact{};
        ASSERT_TRUE(VNX::Monitor::decodeExecutionFact(batch.records.front(), &fact));
        EXPECT_EQ(batch.records.front().header.instanceGeneration, generation);
        EXPECT_EQ(batch.records.front().header.stableId, (harkenstudio::ObjectId{41, 59}));
        EXPECT_EQ(fact.projectId, (harkenstudio::ObjectId{UINT64_C(0x123456789abcdef0), UINT64_C(0x1020304050607080)}));
        EXPECT_EQ(fact.timestampNs, source.timestamp);
        EXPECT_EQ(fact.subjectNamespace, subject);
        EXPECT_TRUE(VNX::Monitor::monitorFactHas(fact, VNX::Monitor::kMonitorFactFieldContentObject));
    }
}

TEST(MonitorDecoder, ExecutionRejectsMalformedIdentityAndPreservesOutputOnFailure)
{
    harken::MonitorExecutionFact source{};
    source.engine = 19;
    source.sourceSequence = 31;
    source.kind = VN_MONITOR_FACT_PLAYABLE_ACTIVE;
    source.subjectNamespace = VN_MONITOR_SUBJECT_PLAYABLE;
    source.body.execution.identity.handle = UINT64_C(0x100000003);
    ASSERT_TRUE(harken::monitor::SetSourceField(source, VN_MONITOR_FACT_FIELD_SUBJECT));
    std::vector<uint8_t> packed(harken::monitor::kExecutionRecordBytes);
    ASSERT_EQ(harken::monitor::PackExecutionFact(packed.data(), packed.size(), source, 71, 72), packed.size());
    const auto batch = decodePackedRecords(packed.data(), packed.size(), 1);
    ASSERT_EQ(batch.status, VN_OK);
    ASSERT_EQ(batch.records.size(), 1u);
    for (int corruption = 0; corruption != 5; ++corruption) {
        SCOPED_TRACE(corruption);
        auto record = batch.records.front();
        if (corruption == 0) record.payload.pop_back();
        if (corruption == 1) record.header.instanceGeneration += 1;
        if (corruption == 2) record.payload[60] = 1;
        if (corruption == 3) record.payload[23] = 128;
        if (corruption == 4) record.payload[64] = 1;
        VNX::Monitor::MonitorExecutionFactV1 fact{};
        fact.timestampNs = 997;
        EXPECT_FALSE(VNX::Monitor::decodeExecutionFact(record, &fact));
        EXPECT_EQ(fact.timestampNs, 997u);
    }
}
