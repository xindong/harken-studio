#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "harken/core/MonitorIdentity.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

VnMonitorSessionDesc MakeDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE |
        VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE |
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
    desc.consumerQueueBytes = 65536;
    return desc;
}

struct PollArgs {
    std::vector<unsigned char> buffer;
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    explicit PollArgs(std::uint64_t bytes = 65536)
        : buffer(static_cast<std::size_t>(bytes)) {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

std::uint32_t ReadU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const unsigned char* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

struct PackedRecord {
    std::uint32_t recordKind = 0;
    std::uint32_t entityKind = 0;
    std::uint32_t reason = 0;
    std::uint32_t eventClass = 0;
    std::uint32_t validityFlags = 0;
    std::uint64_t recordBytes = 0;
    VnObjectId stableId = harkentest::ObjectId(0);
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint32_t metricId = 0;
};

std::vector<PackedRecord> ParseRecords(const PollArgs& poll) {
    std::vector<PackedRecord> out;
    const unsigned char* bytes = poll.buffer.data();
    std::uint64_t offset = 0;
    const std::uint64_t total = poll.result.bytesWritten;
    const std::uint32_t envelope = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    while (offset + envelope <= total) {
        PackedRecord rec;
        rec.recordKind = ReadU32(bytes + offset);
        rec.recordBytes = ReadU64(bytes + offset + 16);
        rec.entityKind = ReadU32(bytes + offset + 156);
        rec.stableId = harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes + offset + 184, 16));
        rec.instanceHandle = ReadU64(bytes + offset + 200);
        rec.instanceGeneration = ReadU64(bytes + offset + 208);
        rec.reason = ReadU32(bytes + offset + 248);
        rec.validityFlags = ReadU32(bytes + offset + 252);
        if (rec.recordBytes < envelope || offset + rec.recordBytes > total) {
            break;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.recordBytes >= envelope + sizeof(VnMonitorEventPayload)) {
            VnMonitorEventPayload payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.eventClass = payload.eventClass;
            if (rec.reason == 0) {
                rec.reason = payload.reason;
            }
        }
        if (rec.recordKind == VN_MONITOR_RECORD_TIME_SERIES &&
            rec.recordBytes >= envelope + sizeof(VnMonitorTimeSeriesSample)) {
            VnMonitorTimeSeriesSample sample{};
            std::memcpy(&sample, bytes + offset + envelope, sizeof(sample));
            rec.metricId = sample.metricId;
        }
        out.push_back(rec);
        offset += rec.recordBytes;
    }
    return out;
}

} // namespace

TEST(MonitorObserve, DspGapPublishesUnsupportedDrop) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    harken::monitor::StopDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool sawGap = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_TIME_SERIES &&
            rec.metricId >= 101u && rec.metricId <= 105u) {
            EXPECT_EQ(0u, rec.validityFlags & VN_MONITOR_VALID_UNSUPPORTED)
                << "M1-5 CPU metrics must not be marked as DSP timing";
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.eventClass == VN_MONITOR_EVENT_DROP &&
            rec.reason == VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE &&
            rec.entityKind == VN_MONITOR_ENTITY_NONE &&
            rec.instanceHandle == 0u && rec.instanceGeneration == 0u &&
            rec.stableId == harkentest::ObjectId(0u)) {
            sawGap = true;
        }
    }
    EXPECT_TRUE(sawGap);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, DspGapPublishesToLaterSession) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionDesc desc = MakeDesc();
    auto pollSeesGap = [](VnMonitorSessionHandle session) {
        PollArgs poll;
        EXPECT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
        const std::vector<PackedRecord> records = ParseRecords(poll);
        for (const PackedRecord& rec : records) {
            if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
                rec.eventClass == VN_MONITOR_EVENT_DROP &&
                rec.reason == VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE &&
                rec.entityKind == VN_MONITOR_ENTITY_NONE &&
                rec.instanceHandle == 0u && rec.instanceGeneration == 0u &&
                rec.stableId == harkentest::ObjectId(0u)) {
                return true;
            }
        }
        return false;
    };

    VnMonitorSessionHandle first = 0;
    harken::monitor::StopDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &first));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    EXPECT_TRUE(pollSeesGap(first));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));

    VnMonitorSessionHandle second = 0;
    harken::monitor::StopDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &second));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    EXPECT_TRUE(pollSeesGap(second));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorObserve, DspGapPublishesWhenLogicRingIsFull) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle first = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    harken::monitor::StopDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &first));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(harken::monitor::FillLogicRingForTests());
    EXPECT_EQ(0u, harken::monitor::LogicRingFreeSlotsForTests());

    VnMonitorSessionHandle second = 0;
    VnMonitorSessionDesc eventDesc = desc;
    // Keep the synthetic fill traffic from overflowing this event consumer.
    eventDesc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    eventDesc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&eventDesc, &second));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(second, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool sawGap = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.eventClass == VN_MONITOR_EVENT_DROP &&
            rec.reason == VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE &&
            rec.entityKind == VN_MONITOR_ENTITY_NONE &&
            rec.instanceHandle == 0u && rec.instanceGeneration == 0u &&
            rec.stableId == harkentest::ObjectId(0u)) {
            sawGap = true;
        }
    }
    EXPECT_TRUE(sawGap);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
}
