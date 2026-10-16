#ifndef HARKEN_TEST_MONITOR_EXECUTION_TEST_SUPPORT_H
#define HARKEN_TEST_MONITOR_EXECUTION_TEST_SUPPORT_H

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorExecutionCodec.h"
#include "../UnitTest/monitor_fixture_decode.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

namespace harkentest::monitor {
using namespace harken::monitor;

struct IdentityRecord {
    VnMonitorRecordHeader header{};
    VnMonitorExecutionFactV1 fact{};
};

inline VnMonitorSessionHandle OpenIdentityEvents() {
    VnMonitorSessionDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    desc.requestedExecutionFactVersion = 1;
    desc.consumerQueueBytes = 65536;
    VnMonitorSessionHandle result = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorOpenEventSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &result));
    StopDrainForTests();
    return result;
}

inline std::vector<IdentityRecord> ReadIdentityEvents(VnMonitorSessionHandle session, std::uint32_t* operations = nullptr) {
    DrainOnceForTests();
    std::array<std::uint8_t, 65536> buffer{}, skipped{};
    VnMonitorPollDesc desc{};
    desc.structSize = sizeof(desc);
    desc.buffer = buffer.data();
    desc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(session, &desc, &result));
    auto legacyInput = buffer;
    // The unchanged fixture-only envelope reader requires delivery zero.
    // Strip that transport stamp in a copy; decode the actual payload below.
    for (std::uint64_t offset = 0; offset < result.bytesWritten;) {
        if (result.bytesWritten - offset < kPackedEnvelopeBytes) { ADD_FAILURE(); return {}; }
        std::uint64_t size = 0;
        for (unsigned i = 0; i != 8; ++i) size |= static_cast<std::uint64_t>(buffer[offset + 16 + i]) << (8 * i);
        if (size < kPackedEnvelopeBytes || size > result.bytesWritten - offset) { ADD_FAILURE(); return {}; }
        WriteU64(legacyInput.data() + offset + 48, 0);
        offset += size;
    }
    std::vector<VnMonitorDecodedRecord> decoded(result.recordsWritten);
    const auto read = vn_monitor_decode_records(legacyInput.data(), result.bytesWritten,
        decoded.data(), static_cast<std::uint32_t>(decoded.size()), skipped.data(), skipped.size());
    EXPECT_EQ(VN_OK, read.status);
    std::vector<IdentityRecord> records;
    std::uint64_t offset = 0;
    for (const auto& record : decoded) {
        EXPECT_NE(VN_MONITOR_RECORD_DROP_SUMMARY, record.header.recordKind)
            << "Identity assertions require a capture without dropped records";
        if (record.header.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT) {
            IdentityRecord row{record.header};
            if (!DecodeExecutionFact(record.header, buffer.data() + offset + kPackedEnvelopeBytes,
                record.header.payloadBytes, row.fact)) { ADD_FAILURE(); return {}; }
            records.push_back(row);
        }
        if (operations && record.header.recordKind == VN_MONITOR_RECORD_EVENT_DETAIL) {
            ExecutionPayloadReader detail{buffer.data() + offset + kPackedEnvelopeBytes + 4};
            std::uint32_t category = 0;
            detail(category);
            if (category == VN_MONITOR_EVENT_CATEGORY_OPERATION) ++*operations;
        }
        offset += record.header.recordBytes;
    }
    return records;
}

inline void CollectIdentityEvents(VnMonitorSessionHandle session, std::vector<IdentityRecord>& records) {
    const auto next = ReadIdentityEvents(session);
    records.insert(records.end(), next.begin(), next.end());
}

} // namespace harkentest::monitor

#endif
