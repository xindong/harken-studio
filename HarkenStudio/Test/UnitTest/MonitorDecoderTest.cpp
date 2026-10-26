#include <harken/Harken.h>
#include "Monitor/MonitorDecoder.h"
#include "Monitor/support/MonitorFixtureLoader.h"
#include "MonitorExecutionCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using VNX::Monitor::decodePackedRecords;
using VNX::Monitor::TestSupport::loadEditorFixture;

TEST(MonitorDecoder, ShortHeaderIsIncompatible)
{
    const auto fixture = loadEditorFixture("short_header.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_NE(decoded.status, VN_OK);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, TruncatedRecordIsIncompatible)
{
    const auto fixture = loadEditorFixture("truncated_record.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_NE(decoded.status, VN_OK);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, CountBytesMismatchIsIncompatible)
{
    const auto fixture = loadEditorFixture("count_bytes_mismatch.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    const auto decoded = decodePackedRecords(fixture.packed.data(),
                                             fixture.packed.size(),
                                             fixture.expectedRecords);
    EXPECT_NE(decoded.status, VN_OK);
    EXPECT_EQ(decoded.recordsWritten, 0u);
}

TEST(MonitorDecoder, ReadsEngineExecutionIdentityWithoutNativeLayoutAssumptions)
{
    harken::MonitorExecutionFact source{};
    source.engine = 19;
    source.sourceSequence = 31;
    source.kind = VN_MONITOR_FACT_PLAYABLE_ACTIVE;
    source.subjectNamespace = VN_MONITOR_SUBJECT_ACTION;
    source.contentKind = VN_MONITOR_CONTENT_SOUND;
    const uint64_t generation = (UINT64_C(1) << 40) + 7;
    source.body.execution.identity.handle = (generation << 12) | 3;
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
    EXPECT_EQ(fact.projectId, (harkenstudio::ObjectId{UINT64_C(0x123456789abcdef0), UINT64_C(0x1020304050607080)}));
}
