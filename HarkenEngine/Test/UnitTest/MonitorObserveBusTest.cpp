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
        VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE;
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
    std::uint32_t eventClass = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint32_t mute01 = 0;
    std::uint32_t solo01 = 0;
    VnObjectId routeBusId{};
    VnObjectId stableId{};
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
        if (rec.recordBytes < envelope || offset + rec.recordBytes > total) {
            break;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.recordBytes >= envelope + sizeof(VnMonitorEventPayload)) {
            VnMonitorEventPayload payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.eventClass = payload.eventClass;
        }
        if ((rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT ||
             rec.recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE) &&
            rec.recordBytes >= envelope + sizeof(VnMonitorStateUpsert)) {
            VnMonitorStateUpsert payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.entityKind = payload.entityKind;
            rec.instanceHandle = payload.instanceHandle;
            rec.instanceGeneration = payload.instanceGeneration;
            rec.mute01 = payload.mute01;
            rec.solo01 = payload.solo01;
            rec.routeBusId = payload.routeBusId;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE &&
            rec.recordBytes >= envelope + sizeof(VnMonitorStateTombstone)) {
            VnMonitorStateTombstone payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.entityKind = payload.entityKind;
            rec.instanceHandle = payload.instanceHandle;
            rec.instanceGeneration = payload.instanceGeneration;
        }
        out.push_back(rec);
        offset += rec.recordBytes;
    }
    return out;
}

VnConfig BusConfig() {
    VnConfig config = harkentest::MakeNullConfig(0, 256, 48000);
    config.fxSlotsPerBus = 1;
    config.delayEffectPoolCapacity = 4;
    return config;
}

VnFeedbackDelayDesc DelayDesc() {
    VnFeedbackDelayDesc desc{};
    desc.structSize = sizeof(desc);
    desc.delayMs = 20.0f;
    desc.feedback = 0.1f;
    desc.wet = 1.0f;
    desc.dry = 1.0f;
    return desc;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

} // namespace

TEST(MonitorObserve, BusCreateMuteAndEffectReleaseOnce) {
    harkentest::EngineSession engine(BusConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusMute(harkentest::ObjectId(10), 1, 0));
    const VnFeedbackDelayDesc delay = DelayDesc();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(harkentest::ObjectId(10), 0, VN_EFFECT_FEEDBACK_DELAY,
                                     AsBase(&delay)));
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(harkentest::ObjectId(10), 0));
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(harkentest::ObjectId(10), 0));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    std::uint32_t busEvents = 0;
    std::uint32_t effectTombstones = 0;
    bool sawMute = false;
    bool sawParent = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.entityKind == VN_MONITOR_ENTITY_BUS) {
            ++busEvents;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.entityKind == VN_MONITOR_ENTITY_BUS) {
            if (rec.stableId == harkentest::ObjectId(10) && rec.mute01 == 1u) {
                sawMute = true;
            }
            if (rec.stableId == harkentest::ObjectId(10) && rec.routeBusId == VN_BUS_MASTER) {
                sawParent = true;
            }
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE &&
            rec.entityKind == VN_MONITOR_ENTITY_EFFECT) {
            ++effectTombstones;
        }
    }
    EXPECT_EQ(0u, busEvents);
    EXPECT_TRUE(sawMute);
    EXPECT_TRUE(sawParent);
    EXPECT_EQ(1u, effectTombstones);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
