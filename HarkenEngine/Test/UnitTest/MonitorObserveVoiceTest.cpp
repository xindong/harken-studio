#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "harken/core/MonitorIdentity.h"
#include "harken/core/VoiceLedger.h"
#include "Engine.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

VnMonitorSessionDesc MakeVoiceDesc() {
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
    std::uint64_t worldGeneration = 0;
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint64_t playableInstanceHandle = 0;
    std::uint32_t occurrenceOrdinal = 0;
    std::uint32_t payloadReason = 0;
    float volumeDb = 0.0f;
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
        rec.worldGeneration = ReadU64(bytes + offset + 72);
        rec.entityKind = ReadU32(bytes + offset + 156);
        rec.instanceHandle = ReadU64(bytes + offset + 200);
        rec.instanceGeneration = ReadU64(bytes + offset + 208);
        rec.playableInstanceHandle = ReadU64(bytes + offset + 224);
        rec.reason = ReadU32(bytes + offset + 248);
        rec.validityFlags = ReadU32(bytes + offset + 252);
        rec.occurrenceOrdinal = ReadU32(bytes + offset + 296);
        if (rec.recordBytes < envelope || offset + rec.recordBytes > total) {
            break;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.recordBytes >= envelope + sizeof(VnMonitorEventPayload)) {
            VnMonitorEventPayload payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.eventClass = payload.eventClass;
            rec.payloadReason = payload.reason;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.recordBytes >= envelope + sizeof(VnMonitorStateUpsert)) {
            VnMonitorStateUpsert payload{};
            std::memcpy(&payload, bytes + offset + envelope, sizeof(payload));
            rec.volumeDb = payload.volumeDb;
        }
        out.push_back(rec);
        offset += rec.recordBytes;
    }
    return out;
}

std::uint32_t CountKind(const std::vector<PackedRecord>& records,
                        std::uint32_t recordKind, std::uint32_t eventClass,
                        std::uint32_t entityKind) {
    std::uint32_t count = 0;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == recordKind && rec.entityKind == entityKind &&
            (eventClass == 0 || rec.eventClass == eventClass)) {
            ++count;
        }
    }
    return count;
}

VnVoiceHandle MakeRenderHandle(std::uint32_t slot, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

VnConfig VoiceParkConfig() {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    config.maxParkedVoices = 2;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    return config;
}

void RegisterVoiceLeaf(VnSoundId id) {
    static std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

void RegisterFarProfile(VnAttenuationId id) {
    const VnAttenuationDesc desc = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(id, &desc));
}

} // namespace

TEST(MonitorObserve, VoicePendingLiveParkKeepsPublicKey) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeVoiceDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::VoiceLedger ledger(2);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePending(&pub, &slot, 128));
    harken::monitor::DrainOnceForTests();
    const VnVoiceHandle render = MakeRenderHandle(0, 1);
    ledger.StartPending(slot, render);
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(ledger.Park(render, &slot));
    harken::MonitorLifecycleFacts parkFacts{};
    parkFacts.volumeDb = -12.5f;
    parkFacts.volumeKnown = true;
    parkFacts.notifyType = VN_NOTIFY_VOICE_PARKED;
    ledger.ObserveVoice(harken::MonitorLifecyclePhase::VoicePark, slot,
                        VN_ACTION_TERMINAL_NONE, parkFacts);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_VOICE,
                        VN_MONITOR_ENTITY_VOICE),
              3u);
    std::uint32_t unavailableUpserts = 0;
    bool parked = false;
    for (const PackedRecord& rec : records) {
        if (rec.entityKind != VN_MONITOR_ENTITY_VOICE ||
            rec.instanceHandle != pub) {
            continue;
        }
        EXPECT_EQ(0u, rec.worldGeneration);
        EXPECT_EQ(0u, rec.playableInstanceHandle);
        EXPECT_EQ(0u, rec.occurrenceOrdinal);
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT) {
            EXPECT_EQ(rec.reason, rec.payloadReason);
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.reason == VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE &&
            rec.volumeDb == 0.0f) {
            ++unavailableUpserts;
            EXPECT_EQ(0u, rec.validityFlags & VN_MONITOR_VALID_NUMERIC);
        }
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.reason == VN_ACTION_TERMINAL_NONE && rec.volumeDb == -12.5f) {
            parked = true;
            EXPECT_NE(0u, rec.validityFlags & VN_MONITOR_VALID_NUMERIC);
            EXPECT_EQ(pub >> 32, rec.instanceGeneration);
        }
    }
    EXPECT_GE(unavailableUpserts, 2u);
    EXPECT_TRUE(parked);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, VoiceParkReturnParkKeepsSecondPark) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeVoiceDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::VoiceLedger ledger(2);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePending(&pub, &slot, 128));
    harken::monitor::DrainOnceForTests();
    const VnVoiceHandle renderA = MakeRenderHandle(0, 1);
    ledger.StartPending(slot, renderA);
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(ledger.Park(renderA, &slot));
    harken::MonitorLifecycleFacts firstPark{};
    firstPark.volumeDb = -12.5f;
    firstPark.volumeKnown = true;
    firstPark.notifyType = VN_NOTIFY_VOICE_PARKED;
    ledger.ObserveVoice(harken::MonitorLifecyclePhase::VoicePark, slot,
                        VN_ACTION_TERMINAL_NONE, firstPark);
    harken::monitor::DrainOnceForTests();
    const VnVoiceHandle renderB = MakeRenderHandle(1, 1);
    ledger.ReturnPark(slot, renderB);
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(ledger.Park(renderB, &slot));
    harken::MonitorLifecycleFacts secondPark{};
    secondPark.volumeDb = -18.0f;
    secondPark.volumeKnown = true;
    secondPark.notifyType = VN_NOTIFY_VOICE_PARKED;
    ledger.ObserveVoice(harken::MonitorLifecyclePhase::VoicePark, slot,
                        VN_ACTION_TERMINAL_NONE, secondPark);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    std::uint32_t parkedNumeric = 0;
    bool sawSecondVolume = false;
    for (const PackedRecord& rec : records) {
        if (rec.entityKind != VN_MONITOR_ENTITY_VOICE ||
            rec.instanceHandle != pub) {
            continue;
        }
        EXPECT_EQ(0u, rec.playableInstanceHandle);
        EXPECT_EQ(0u, rec.occurrenceOrdinal);
        if (rec.recordKind != VN_MONITOR_RECORD_STATE_UPSERT) {
            continue;
        }
        if (rec.reason == VN_ACTION_TERMINAL_NONE &&
            (rec.validityFlags & VN_MONITOR_VALID_NUMERIC) != 0) {
            ++parkedNumeric;
            if (rec.volumeDb == -18.0f) {
                sawSecondVolume = true;
            }
        }
    }
    EXPECT_GE(parkedNumeric, 2u);
    EXPECT_TRUE(sawSecondVolume);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, VoiceApiParkCopiesFrozenVolume) {
    harkentest::EngineSession session(VoiceParkConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    harken::monitor::ResetLifecycleDedupeForTests();
    RegisterVoiceLeaf(harkentest::ObjectId(1));
    RegisterFarProfile(harkentest::ObjectId(9));

    VnMonitorSessionHandle monitor = 0;
    VnMonitorSessionDesc desc = MakeVoiceDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &monitor));
    harken::monitor::StopDrainForTests();

    VnPlayDesc play = harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, 0.0f),
                                    harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                    harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    const std::uint32_t ledgerSlot = static_cast<std::uint32_t>(handle);
    ASSERT_TRUE(engine->parkRecords[ledgerSlot].active);
    const float expectedVolume = engine->parkRecords[ledgerSlot].frozenNonSpatialDb +
                                 engine->parkRecords[ledgerSlot].instanceVolumeDb;

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(monitor, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool parked = false;
    for (const PackedRecord& rec : records) {
        if (rec.entityKind != VN_MONITOR_ENTITY_VOICE ||
            rec.instanceHandle != handle) {
            continue;
        }
        EXPECT_EQ(0u, rec.playableInstanceHandle);
        EXPECT_EQ(0u, rec.occurrenceOrdinal);
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_UPSERT &&
            rec.reason == VN_ACTION_TERMINAL_NONE &&
            rec.volumeDb == expectedVolume) {
            parked = true;
            EXPECT_NE(0u, rec.validityFlags & VN_MONITOR_VALID_NUMERIC);
        }
    }
    EXPECT_TRUE(parked);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(monitor));
}

TEST(MonitorObserve, VoiceSweepEndedPublishesTombstone) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeVoiceDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::VoiceLedger ledger(2);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePending(&pub, &slot, 128));
    harken::monitor::DrainOnceForTests();
    const VnVoiceHandle render = MakeRenderHandle(0, 1);
    ledger.StartPending(slot, render);
    harken::monitor::DrainOnceForTests();
    ledger.SweepEnded([](VnVoiceHandle) { return false; });
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    bool tombstone = false;
    for (const PackedRecord& rec : records) {
        if (rec.entityKind != VN_MONITOR_ENTITY_VOICE ||
            rec.instanceHandle != pub) {
            continue;
        }
        EXPECT_EQ(0u, rec.playableInstanceHandle);
        EXPECT_EQ(0u, rec.occurrenceOrdinal);
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE) {
            tombstone = true;
            EXPECT_EQ(VN_ACTION_TERMINAL_NATURAL, rec.reason);
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT) {
            EXPECT_EQ(rec.reason, rec.payloadReason);
            EXPECT_NE(VN_NOTIFY_VOICE_ENDED, rec.payloadReason);
        }
    }
    EXPECT_TRUE(tombstone);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, VoicePlayableOwnedPublishesNothing) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeVoiceDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::VoiceLedger ledger(2);
    harken::VoiceLedger::PlayableParentRef parent{};
    parent.internalToken = 41;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned(parent, &slot));
    ledger.BindRender(slot, MakeRenderHandle(1, 1), 128);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_VOICE, VN_MONITOR_ENTITY_VOICE));
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_STATE_UPSERT, 0,
                            VN_MONITOR_ENTITY_VOICE));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, VoiceAcquireWithoutBindPublishesNothing) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeVoiceDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::VoiceLedger ledger(1);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.Acquire(&pub, &slot));
    ledger.Release(slot);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_VOICE, VN_MONITOR_ENTITY_VOICE));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
