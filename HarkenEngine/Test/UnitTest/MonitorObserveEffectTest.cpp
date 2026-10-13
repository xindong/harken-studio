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
    std::uint32_t reason = 0;
    std::uint32_t eventClass = 0;
    std::uint32_t validityFlags = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint32_t bypass01 = 0;
    std::uint32_t latencyFrames = 0;
    std::uint32_t tailFrames = 0;
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
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.recordBytes >= envelope + sizeof(VnMonitorStateUpsert)) {
            VnMonitorStateUpsert payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.bypass01 = payload.bypass01;
            rec.latencyFrames = payload.latencyFrames;
            rec.tailFrames = payload.tailFrames;
            rec.entityKind = payload.entityKind;
            rec.instanceHandle = payload.instanceHandle;
            rec.instanceGeneration = payload.instanceGeneration;
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

VnConfig EffectConfig() {
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

TEST(MonitorObserve, EffectInstallPublishesFrozenScalars) {
    harkentest::EngineSession engine(EffectConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    const VnFeedbackDelayDesc delay = DelayDesc();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(harkentest::ObjectId(10), 0, VN_EFFECT_FEEDBACK_DELAY,
                                     AsBase(&delay)));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(harkentest::ObjectId(10), 0, 1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    std::uint32_t effectEvents = 0;
    bool sawTail = false;
    bool sawBypass = false;
    bool latencyUnmeasured = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.entityKind == VN_MONITOR_ENTITY_EFFECT) {
            ++effectEvents;
        }
        if (rec.recordKind != VN_MONITOR_RECORD_STATE_UPSERT ||
            rec.entityKind != VN_MONITOR_ENTITY_EFFECT) {
            continue;
        }
        EXPECT_NE(0u, rec.instanceGeneration);
        EXPECT_EQ(0u, rec.latencyFrames);
        EXPECT_NE(0u, rec.validityFlags & VN_MONITOR_VALID_UNSUPPORTED);
        if ((rec.validityFlags & VN_MONITOR_VALID_NUMERIC) != 0 &&
            rec.tailFrames != 0) {
            sawTail = true;
        }
        if (rec.bypass01 == 1u) {
            sawBypass = true;
        }
        if (rec.latencyFrames == 0u &&
            (rec.validityFlags & VN_MONITOR_VALID_UNSUPPORTED) != 0) {
            latencyUnmeasured = true;
        }
    }
    EXPECT_EQ(1u, effectEvents);
    EXPECT_TRUE(sawTail);
    EXPECT_TRUE(sawBypass);
    EXPECT_TRUE(latencyUnmeasured);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, EffectInstallCopiesCommittedEmptySlotBypass) {
    harkentest::EngineSession engine(EffectConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(harkentest::ObjectId(10), 0, 1));
    const VnFeedbackDelayDesc delay = DelayDesc();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(harkentest::ObjectId(10), 0, VN_EFFECT_FEEDBACK_DELAY,
                                     AsBase(&delay)));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool sawInstallBypass = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.entityKind == VN_MONITOR_ENTITY_EFFECT && rec.bypass01 == 1u) {
            sawInstallBypass = true;
        }
    }
    EXPECT_TRUE(sawInstallBypass);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, EffectInstallOnReusedBusDoesNotKeepOldBypass) {
    harkentest::EngineSession engine(EffectConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(harkentest::ObjectId(10), 0, 1));
    ASSERT_EQ(VN_OK, Vn_DestroyBus(harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(11), VN_BUS_MASTER));
    const VnFeedbackDelayDesc delay = DelayDesc();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(harkentest::ObjectId(11), 0, VN_EFFECT_FEEDBACK_DELAY,
                                     AsBase(&delay)));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool sawFreshInstall = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.entityKind == VN_MONITOR_ENTITY_EFFECT) {
            EXPECT_EQ(0u, rec.bypass01);
            sawFreshInstall = true;
        }
    }
    EXPECT_TRUE(sawFreshInstall);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, EffectInstallOnMasterSlotZeroPublishesNonZeroHandle) {
    harkentest::EngineSession engine(EffectConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    const VnFeedbackDelayDesc delay = DelayDesc();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_FEEDBACK_DELAY,
                                     AsBase(&delay)));
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(VN_BUS_MASTER, 0));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool sawMasterEffect = false;
    bool sawMatchingRelease = false;
    std::uint64_t upsertHandle = 0;
    std::uint64_t upsertGeneration = 0;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.entityKind == VN_MONITOR_ENTITY_EFFECT) {
            EXPECT_NE(0u, rec.instanceHandle);
            EXPECT_NE(0u, rec.instanceGeneration);
            upsertHandle = rec.instanceHandle;
            upsertGeneration = rec.instanceGeneration;
            sawMasterEffect = true;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE &&
            rec.entityKind == VN_MONITOR_ENTITY_EFFECT) {
            EXPECT_NE(0u, rec.instanceHandle);
            EXPECT_EQ(upsertHandle, rec.instanceHandle);
            EXPECT_EQ(upsertGeneration, rec.instanceGeneration);
            sawMatchingRelease = true;
        }
    }
    EXPECT_TRUE(sawMasterEffect);
    EXPECT_TRUE(sawMatchingRelease);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
