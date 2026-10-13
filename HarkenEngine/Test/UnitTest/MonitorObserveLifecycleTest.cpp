#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Engine.h"
#include "Common/PackFoundationFixture.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/TestUtil.h"
#include "harken/core/ActionFiringTable.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/TransitionInstancePool.h"
#include "harken/core/VariableControlRuntime.h"

#include <gtest/gtest.h>

#include "monitor_fixture_decode.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#ifndef HARKEN_MONITOR_FIXTURE_DIR
#define HARKEN_MONITOR_FIXTURE_DIR "."
#endif

namespace {

VnMonitorSessionDesc MakeLifecycleDesc(std::uint64_t queueBytes = 65536) {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE;
    desc.consumerQueueBytes = queueBytes;
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
    std::uint32_t streamKind = 0;
    std::uint32_t entityKind = 0;
    std::uint32_t reason = 0;
    std::uint32_t eventClass = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t worldGeneration = 0;
    std::uint64_t executeFrame = 0;
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint64_t playableInstanceHandle = 0;
    std::uint64_t actionInstanceHandle = 0;
    std::uint64_t sourceSequence = 0;
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
        rec.streamKind = ReadU32(bytes + offset + 12);
        rec.recordBytes = ReadU64(bytes + offset + 16);
        rec.sourceSequence = ReadU64(bytes + offset + 40);
        rec.worldGeneration = ReadU64(bytes + offset + 72);
        rec.executeFrame = ReadU64(bytes + offset + 128);
        rec.entityKind = ReadU32(bytes + offset + 156);
        rec.instanceHandle = ReadU64(bytes + offset + 200);
        rec.instanceGeneration = ReadU64(bytes + offset + 208);
        rec.actionInstanceHandle = ReadU64(bytes + offset + 216);
        rec.playableInstanceHandle = ReadU64(bytes + offset + 224);
        rec.reason = ReadU32(bytes + offset + 248);
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

bool HasEnvelope(const PackedRecord& rec, std::uint64_t handle,
                 std::uint64_t generation) {
    return rec.instanceHandle == handle && rec.instanceGeneration == generation;
}

harken::QueueInstanceCreateRequest MakeQueueCreate() {
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = 11;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 17;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.cursor = 0;
    create.initial.currentChildOrdinal = 3;
    return create;
}

std::string FixturePath(const char* relative) {
    return std::string(HARKEN_MONITOR_FIXTURE_DIR) + "/" + relative;
}

} // namespace

TEST(MonitorObserve, LifecyclePlayableContainerEventsReachPoll) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::StopDrainForTests();

    harken::PlayableInstanceTable table(4);
    const harken::PlayableInstanceHandle sound =
        table.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(sound, 0u);
    harken::monitor::DrainOnceForTests();
    const harken::PlayableInstanceHandle container =
        table.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(container, 0u);
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(table.AdvanceLifecycle(
        sound, harken::PlayableLifecycle::LogicalComplete));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(table.AdvanceLifecycle(
        sound, harken::PlayableLifecycle::AudibleRetiring));
    harken::monitor::DrainOnceForTests();
    const std::uint64_t retiredGeneration = sound >> 32;
    ASSERT_TRUE(
        table.AdvanceLifecycle(sound, harken::PlayableLifecycle::Retired));
    harken::monitor::DrainOnceForTests();
    ASSERT_NE(0u, table.BumpScheduleEpoch(container));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_START,
                        VN_MONITOR_ENTITY_PLAYABLE),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_START,
                        VN_MONITOR_ENTITY_CONTAINER),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT,
                        VN_MONITOR_EVENT_RESULT, VN_MONITOR_ENTITY_PLAYABLE),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_STOP,
                        VN_MONITOR_ENTITY_PLAYABLE),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_STATE_TOMBSTONE, 0,
                        VN_MONITOR_ENTITY_PLAYABLE),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_REJECT,
                        VN_MONITOR_ENTITY_CONTAINER),
              1u);

    bool tombstoneOk = false;
    bool completeOk = false;
    for (const PackedRecord& rec : records) {
        if (rec.recordKind == VN_MONITOR_RECORD_STATE_TOMBSTONE &&
            HasEnvelope(rec, sound, retiredGeneration)) {
            tombstoneOk = true;
        }
        if (rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.eventClass == VN_MONITOR_EVENT_RESULT &&
            HasEnvelope(rec, sound, retiredGeneration)) {
            completeOk = true;
        }
    }
    EXPECT_TRUE(tombstoneOk);
    EXPECT_TRUE(completeOk);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorObserve, EventSessionSealsOperationsAndOriginalDiagnostics) {
    std::vector<std::uint32_t> callbackCodes;
    auto config = harkentest::MakeNullConfig();
    config.logUserData = &callbackCodes;
    config.logCallback = [](void* context, std::uint32_t, std::uint32_t code, const char*) {
        static_cast<std::vector<std::uint32_t>*>(context)->push_back(code);
    };
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &handle));
    harken::monitor::StopDrainForTests();
    VnMonitorEventSessionStatus status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_LIVE, status.state);
    EXPECT_NE(0u, status.monitorSessionEpoch);
    EXPECT_NE(0u, status.startedTimestampNs);

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ObjectId(991337);
    VnActionFiringHandle firing = 0;
    const auto refused = Vn_PostAction(&post, &firing);
    ASSERT_NE(VN_OK, refused);
    EXPECT_EQ(0u, firing);
    ASSERT_EQ(VN_OK, Vn_StopAll(0));
    const auto timestamp = harken::LogTimestampNs();
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, runtime);
    runtime->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodePackClosureRefused,
                                555, 9, timestamp});
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(handle));
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, status.state);
    ASSERT_EQ(VN_OK, Vn_StopAll(1));
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    std::vector<VnMonitorEventDetail> details;
    std::vector<VnMonitorDecodedRecord> legacyRecords(poll.result.recordsWritten);
    std::vector<std::uint8_t> skipped(poll.result.bytesWritten);
    auto legacyInput = poll.buffer;
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;
         offset += ReadU64(legacyInput.data() + offset + 16)) {
        std::fill_n(legacyInput.data() + offset + 48, 8, 0);
    }
    const auto legacy = vn_monitor_decode_records(
        legacyInput.data(), poll.result.bytesWritten, legacyRecords.data(),
        static_cast<std::uint32_t>(legacyRecords.size()), skipped.data(), skipped.size());
    ASSERT_EQ(VN_OK, legacy.status);
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* bytes = poll.buffer.data() + offset;
        const auto size = ReadU64(bytes + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(bytes) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetail detail{};
            std::memcpy(&detail, bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            details.push_back(detail);
        }
        offset += size;
    }
    ASSERT_EQ(3u, details.size());
    EXPECT_EQ(VN_MONITOR_OPERATION_POST_ACTION, details[0].code);
    EXPECT_EQ(refused, details[0].result);
    EXPECT_EQ(VN_LOG_ERROR, details[0].level);
    EXPECT_EQ(VN_MONITOR_OPERATION_STOP_ALL, details[1].code);
    EXPECT_EQ(VN_OK, details[1].result);
    EXPECT_EQ(VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC, details[2].category);
    EXPECT_EQ(harken::kLogCodePackClosureRefused, details[2].code);
    EXPECT_EQ(VN_LOG_WARN, details[2].level);
    EXPECT_EQ(timestamp, details[2].timestampNs);
    EXPECT_EQ(555u, details[2].arg0);
    EXPECT_NE(0u, details[2].textBytes);
    EXPECT_NE(callbackCodes.end(), std::find(callbackCodes.begin(), callbackCodes.end(),
                                            harken::kLogCodePackClosureRefused));
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, status.state);
    EXPECT_EQ(0u, status.sourceDroppedRecords);
    EXPECT_EQ(0u, status.deliveryDroppedRecords);
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorGetEventSessionStatus(handle, &status));
}

TEST(MonitorObserve, EventDetailRetainsPlayableIdentityThroughRetirementAndReuse) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &session));
    harken::monitor::StopDrainForTests();
    harken::PlayableInstanceTable table(1);
    for (const auto world : {7u, 8u}) {
        harken::PreparedPlayableInstance prepared;
        ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Sound, 0, &prepared));
        harken::PlayableRootMetadata identity;
        identity.projectId = VnObjectId{world, 37};
        identity.definitionStableId = harkentest::ObjectId(21);
        identity.programSemanticFingerprint = world * 11;
        identity.worldGeneration = world;
        identity.actionFiring = world * 13;
        ASSERT_TRUE(table.SetPreparedRootMetadata(prepared, identity));
        ASSERT_TRUE(table.CommitPrepared(&prepared));
        ASSERT_NE(0u, table.BumpScheduleEpoch(prepared.Handle()));
        ASSERT_TRUE(table.AdvanceLifecycle(prepared.Handle(), harken::PlayableLifecycle::LogicalComplete));
        ASSERT_TRUE(table.AdvanceLifecycle(prepared.Handle(), harken::PlayableLifecycle::Retired));
        harken::monitor::DrainOnceForTests();
        PollArgs poll;
        ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
        std::uint32_t count = 0;
        for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
            const auto* bytes = poll.buffer.data() + offset;
            const auto size = ReadU64(bytes + 16);
            ASSERT_GT(size, 0u);
            if (ReadU32(bytes) == VN_MONITOR_RECORD_EVENT_DETAIL) {
                EXPECT_EQ(ReadU32(bytes + 4), 2u);
                EXPECT_EQ(ReadU64(bytes + 72), world);
                EXPECT_EQ(ReadU64(bytes + 160), identity.programSemanticFingerprint);
                EXPECT_EQ(harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes + 184, 16)), identity.definitionStableId);
                EXPECT_EQ(ReadU64(bytes + 216), identity.actionFiring);
                EXPECT_EQ(harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(
                    bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES + 248, 16)), identity.projectId);
                ++count;
            }
            offset += size;
        }
        EXPECT_EQ(count, 4u);
    }
    harken::monitor::StartDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, DiagnosticsRetainProducerProjectAcrossWorldReplacement) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    auto* runtime = engine.Engine();
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &session));
    harken::monitor::StopDrainForTests();
    const VnObjectId projectA{1, 99};
    const VnObjectId projectB{2, 99};
    const VnObjectId objectId{3, 99};
    harken::ContainerWorldInstallViewV1 world{};
    const auto install = [&](VnObjectId projectId) {
        world.identity.projectId = projectId;
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(world);
        return harken::InstallContainerWorldV1(*runtime, world).status;
    };
    ASSERT_EQ(harken::WorldInstallStatus::Ok, install(projectA));
    const auto timestamp = harken::LogTimestampNs();
    runtime->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodePackClosureRefused,
        0, 9, timestamp, objectId, runtime->CurrentProjectId()});
    ASSERT_EQ(harken::WorldInstallStatus::Ok, install(projectB));
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    std::uint32_t count = 0;
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* bytes = poll.buffer.data() + offset;
        const auto size = ReadU64(bytes + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(bytes) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetailV2 detail{};
            std::memcpy(&detail, bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            ASSERT_EQ(detail.category, VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC);
            EXPECT_EQ(harken::ObjectIdFromBytes(detail.projectGuid), projectA);
            EXPECT_EQ(detail.timestampNs, timestamp);
            EXPECT_EQ(harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes + 184, 16)), objectId);
            ++count;
        }
        offset += size;
    }
    EXPECT_EQ(count, 1u);
    harken::monitor::StartDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, EffectEventsRetainIdentityAndExcludeUnpublishedReservations) {
    auto config = harkentest::MakeNullConfig();
    config.gainEffectPoolCapacity = 1;
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &session));
    harken::monitor::StopDrainForTests();
    auto& pool = engine.Engine()->effectBank;
    const harken::EffectObjectIdentity identity{{0x19, 0x37}, {0x23, 0x41}};
    const auto canceled = pool.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_NE(canceled.type, harken::kEffectTypeNone);
    ASSERT_TRUE(pool.CancelPrepared(canceled));
    const auto rolledBack = pool.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_TRUE(pool.CommitPrepared(rolledBack));
    pool.Recycle(rolledBack);
    const auto installed = pool.AcquirePrepared(VN_EFFECT_GAIN, identity);
    ASSERT_TRUE(pool.CommitPrepared(installed));
    pool.PublishInstalled(installed);
    pool.PublishInstalled(installed);
    pool.Recycle(installed);
    const auto reused = pool.Acquire(VN_EFFECT_GAIN, {{0x29, 0x37}, {0x31, 0x41}});
    pool.Recycle(reused);
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    std::vector<std::uint32_t> codes;
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* bytes = poll.buffer.data() + offset;
        const auto size = ReadU64(bytes + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(bytes) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetailV2 detail{};
            std::memcpy(&detail, bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            EXPECT_EQ(detail.category, VN_MONITOR_EVENT_CATEGORY_LIFECYCLE);
            EXPECT_EQ(harken::ObjectIdFromBytes(detail.projectGuid), identity.projectId);
            EXPECT_EQ(harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes + 184, 16)), identity.effectId);
            EXPECT_EQ(ReadU64(bytes + 208), installed.generation);
            codes.push_back(detail.code);
        }
        offset += size;
    }
    EXPECT_EQ(codes, (std::vector<std::uint32_t>{VN_MONITOR_LIFECYCLE_EFFECT_INSTALLED,
        VN_MONITOR_LIFECYCLE_EFFECT_RELEASED}));
    harken::monitor::StartDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, ActivatedInitRetainsProjectIdentityWhenOrdinaryUnloadRefuses) {
    using namespace harkentest::packfoundation;
    const VnObjectId projectId{0x23, 0x41};
    const VnObjectId objectId{0x19, 0x37};
    std::uint64_t bodyBytes = 0;
    auto partition = PartitionPayload(kInitPackStableId, 1, 0, 0, &bodyBytes, 64, projectId);
    ImageBuilder builder(harken::kPackKindInit);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
        RequirementPayload(kInitPackStableId, 1, 1, 0, bodyBytes));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition, kInitPackStableId,
        std::move(partition));
    AppendNeutralMasterSnapshotV1(&builder);
    auto bytes = builder.Build();
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    EXPECT_TRUE(harken::IsNull(engine.Engine()->CurrentProjectId()));
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &session));
    harken::monitor::StopDrainForTests();
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
    member.providerUserData = &bytes;
    member.provide = [](void* data, std::uint64_t, const char*, std::uint32_t,
                        std::uint64_t, VnPackByteSpan* out) -> VnResult {
        const auto& source = *static_cast<const std::vector<std::uint8_t>*>(data);
        out->structSize = sizeof(*out);
        out->bytes = source.data();
        out->byteCount = source.size();
        return VN_OK;
    };
    member.release = [](void*, std::uint64_t, VnPackByteSpan) {};
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    EXPECT_EQ(projectId, engine.Engine()->CurrentProjectId());
    const auto published = engine.Engine()->packComposition.PublishedHandle();
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, Vn_UnloadPack(report.initHandle));
    EXPECT_EQ(published.get(), engine.Engine()->packComposition.Published());
    EXPECT_EQ(projectId, engine.Engine()->CurrentProjectId());
    const auto timestamp = harken::LogTimestampNs();
    engine.Engine()->logicLogRing.Push({VN_LOG_WARN, harken::kLogCodePackClosureRefused,
        0, 9, timestamp, objectId, engine.Engine()->CurrentProjectId()});
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    std::uint32_t captured = 0;
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* record = poll.buffer.data() + offset;
        const auto size = ReadU64(record + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(record) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetailV2 detail{};
            std::memcpy(&detail, record + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            if (detail.category == VN_MONITOR_EVENT_CATEGORY_DIAGNOSTIC &&
                detail.timestampNs == timestamp) {
                EXPECT_EQ(projectId, harken::ObjectIdFromBytes(detail.projectGuid));
                EXPECT_EQ(objectId, harken::ObjectIdFromBytes(
                    std::span<const std::uint8_t, 16>(record + 184, 16)));
                ++captured;
            }
        }
        offset += size;
    }
    EXPECT_EQ(1u, captured);
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = report.initHandle;
    unload.baseSetRevision = report.resultSetRevision;
    unload.unloadRevision = report.resultSetRevision + 1;
    EXPECT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    harken::monitor::StartDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LegacyResidentMasterEffectRetainsIdentityAndPackOwnership) {
    using namespace harkentest::packfoundation;
    const VnObjectId projectId{0x23, 0x41};
    const VnObjectId effectId{0x19, 0x37};
    const VnObjectId masterId{0x4d4153544552, 1};
    EXPECT_EQ(0u, harken::CanonicalSnapshotPackedBusIdV1(masterId));
    ImageBuilder builder(harken::kPackKindResident);
    harken::PackBusEntry master{};
    master.busId = masterId;
    master.parentBusId = VN_BUS_MASTER;
    std::vector<std::uint8_t> masterBytes(harken::kPackV2BusPayloadBytes);
    harken::SerializePackV2Bus(master, masterBytes.data());
    builder.AddEntry(harken::kPackEntryKindBus, masterId, std::move(masterBytes),
        harken::kPackV2SchemaVintage);
    harken::PackBusEffectEntry effect{};
    effect.busId = masterId;
    effect.effectId = effectId;
    effect.effectType = VN_EFFECT_GAIN;
    effect.descriptorVintage = 2;
    effect.faderPointGuard = 8;
    effect.paramCount = 1;
    effect.paramBytes = sizeof(float);
    std::vector<std::uint8_t> effectBytes(harken::kPackV2BusEffectPayloadBytes);
    harken::SerializePackV2BusEffect(effect, effectBytes.data());
    builder.AddEntry(harken::kPackEntryKindBusEffect, 0, std::move(effectBytes));
    auto bytes = builder.Build();
    auto config = harkentest::MakeNullConfig();
    config.gainEffectPoolCapacity = 1;
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    EXPECT_TRUE(harken::IsNull(engine.Engine()->CurrentProjectId()));
    harken::ContainerWorldInstallViewV1 world{};
    world.identity.projectId = projectId;
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV5(world);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harken::InstallContainerWorldV1(*engine.Engine(), world).status);
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &session));
    harken::monitor::StopDrainForTests();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "monitor-lifecycle";
    std::filesystem::create_directories(directory);
    const auto path = directory / "legacy-resident-master-effect.pack";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }
    const auto utf8Path = path.string();
    VnPackDesc load{};
    load.structSize = sizeof(load);
    load.utf8Path = utf8Path.c_str();
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&load, &handle));
    EXPECT_EQ(engine.Engine()->CurrentProjectId(), projectId);
    const auto masterIndex = engine.Engine()->buses.FindIndex(masterId);
    ASSERT_NE(masterIndex, harken::kInvalidBusIndex);
    EXPECT_NE(masterIndex, engine.Engine()->buses.FindIndex(VN_BUS_MASTER));
    EXPECT_EQ(engine.Engine()->buses.ParentOf(masterIndex), 0u);
    EXPECT_EQ(engine.Engine()->busEffectOwnerCookies[masterIndex][0], handle);
    EXPECT_EQ(engine.Engine()->busEffectOwnerCookies[0][0], 0u);
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    std::uint32_t installed = 0;
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* record = poll.buffer.data() + offset;
        const auto size = ReadU64(record + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(record) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetailV2 detail{};
            std::memcpy(&detail, record + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            if (detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE &&
                detail.code == VN_MONITOR_LIFECYCLE_EFFECT_INSTALLED) {
                EXPECT_TRUE(harken::IsNull(harken::ObjectIdFromBytes(detail.projectGuid)));
                EXPECT_EQ(harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(record + 184, 16)), effectId);
                ++installed;
            }
        }
        offset += size;
    }
    EXPECT_EQ(installed, 1u);
    const VnObjectId foreignChild{0x4d4153544552, 2};
    ASSERT_EQ(VN_OK, Vn_CreateBus(foreignChild, masterId));
    engine.Engine()->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(handle));
    EXPECT_EQ(masterIndex, engine.Engine()->buses.FindIndex(masterId));
    EXPECT_EQ(handle, engine.Engine()->busEffectOwnerCookies[masterIndex][0]);
    harken::monitor::DrainOnceForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* record = poll.buffer.data() + offset;
        const auto size = ReadU64(record + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(record) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetailV2 detail{};
            std::memcpy(&detail, record + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            EXPECT_FALSE(detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE &&
                         detail.code == VN_MONITOR_LIFECYCLE_EFFECT_RELEASED);
        }
        offset += size;
    }
    ASSERT_EQ(VN_OK, Vn_DestroyBus(foreignChild));
    engine.Engine()->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadPack(handle));
    engine.Engine()->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::monitor::DrainOnceForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    std::uint32_t released = 0;
    for (std::uint64_t offset = 0; offset < poll.result.bytesWritten;) {
        const auto* record = poll.buffer.data() + offset;
        const auto size = ReadU64(record + 16);
        ASSERT_GT(size, 0u);
        if (ReadU32(record) == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetailV2 detail{};
            std::memcpy(&detail, record + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES, sizeof(detail));
            if (detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE &&
                detail.code == VN_MONITOR_LIFECYCLE_EFFECT_RELEASED) {
                EXPECT_TRUE(harken::IsNull(harken::ObjectIdFromBytes(detail.projectGuid)));
                EXPECT_EQ(harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(record + 184, 16)), effectId);
                ++released;
            }
        }
        offset += size;
    }
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(engine.Engine()->buses.FindIndex(masterId), harken::kInvalidBusIndex);
    EXPECT_EQ(engine.Engine()->busEffectOwnerCookies[masterIndex][0], 0u);
    EXPECT_EQ(engine.Engine()->buses.FindIndex(VN_BUS_MASTER), 0u);
    EXPECT_EQ(engine.Engine()->busEffectOwnerCookies[0][0], 0u);
    harken::monitor::StartDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, EventSessionReportsConsumerLossWithoutLogCallback) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    auto desc = MakeLifecycleDesc(1);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &handle));
    harken::monitor::StopDrainForTests();
    ASSERT_EQ(VN_OK, Vn_StopAll(0));
    harken::detail::ActiveEngine()->logicLogRing.Push({VN_LOG_ERROR, 123, 1, 2});
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::monitor::DrainOnceForTests();
    VnMonitorEventSessionStatus status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, status.state);
    EXPECT_EQ(2u, status.deliveryDroppedRecords);
    EXPECT_EQ(0u, status.sourceDroppedRecords);
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorObserve, EventSessionReportsProducerLossAtStopBoundary) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    auto desc = MakeLifecycleDesc();
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &handle));
    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::FillLogicRingForTests());
    ASSERT_EQ(VN_OK, Vn_StopAll(0));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(handle));
    harken::monitor::DrainOnceForTests();
    VnMonitorEventSessionStatus status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(handle, &status));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, status.state);
    EXPECT_EQ(1u, status.sourceDroppedRecords);
    EXPECT_EQ(0u, status.deliveryDroppedRecords);
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorObserve, LifecycleActionQueueTransitionProducerPath) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::ActionFiringTable actions;
    ASSERT_TRUE(actions.Reset(3));
    harken::ActionFiringStepResult pending{};
    pending.state = harken::ActionStepState::Pending;
    pending.authoredEventIndex = 0;

    harken::PreparedActionFiring accepted{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(41), 7, 1, &accepted));
    ASSERT_TRUE(actions.SetPreparedStep(accepted, 0, pending));
    ASSERT_TRUE(actions.Commit(&accepted, 99));
    harken::monitor::DrainOnceForTests();

    harken::PreparedActionFiring stopping{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(42), 7, 1, &stopping));
    const harken::ActionFiringHandle stopHandle = stopping.Handle();
    ASSERT_TRUE(actions.SetPreparedStep(stopping, 0, pending));
    ASSERT_TRUE(actions.Commit(&stopping, 100));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(actions.AddLivePlayableCount(stopHandle, 1));
    ASSERT_TRUE(actions.MarkStopping(
        stopHandle, harken::ActionTerminalReason::StopRequested));
    harken::monitor::DrainOnceForTests();

    harken::PreparedActionFiring rejected{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(43), 7, 1, &rejected));
    const harken::ActionFiringHandle rejectHandle = rejected.Handle();
    ASSERT_TRUE(actions.SetPreparedStep(rejected, 0, pending));
    ASSERT_TRUE(actions.Commit(&rejected, 101));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(actions.MarkTerminal(
        rejectHandle, harken::ActionFiringOutcome::Cancelled,
        harken::ActionTerminalReason::StopRequested));
    harken::monitor::DrainOnceForTests();

    harken::QueueInstanceTable queues(2);
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(MakeQueueCreate(), &queue));
    harken::EffectiveQueueState next{};
    next.version = 2;
    next.cursor = 1;
    next.currentChildOrdinal = 5;
    ASSERT_TRUE(queues.AdvanceAfterSignalCompletion(queue, 1, next));
    harken::monitor::DrainOnceForTests();
    EXPECT_FALSE(queues.AdvanceAfterSignalCompletion(queue, 1, next));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(1u, effective.cursor);

    harken::QueueInstanceCreateRequest failCreate = MakeQueueCreate();
    failCreate.rootInstance = 12;
    harken::QueueInstanceToken failed{};
    ASSERT_TRUE(queues.Acquire(failCreate, &failed));
    harken::EffectiveQueueState failedCursor{};
    ASSERT_TRUE(queues.ResolveEffective(failed, &failedCursor));
    EXPECT_EQ(
        harken::QueueFailureDisposition::NewlyFailed,
        queues.FailContainerBeforeConditional(
            failed, harken::QueueFailureReason::WorldReplaced, 41));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(queues.ResolveEffective(failed, &failedCursor));
    EXPECT_EQ(0u, failedCursor.cursor);

    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(pool.InvalidateReady(token));
    ASSERT_TRUE(pool.BeginRetire(token, 41));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_ACCEPT,
                        VN_MONITOR_ENTITY_ACTION),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_STOP,
                        VN_MONITOR_ENTITY_ACTION),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_REJECT,
                        VN_MONITOR_ENTITY_ACTION),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_QUEUE,
                        VN_MONITOR_ENTITY_QUEUE),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_REJECT,
                        VN_MONITOR_ENTITY_QUEUE),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_START,
                        VN_MONITOR_ENTITY_TRANSITION),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_STOP,
                        VN_MONITOR_ENTITY_TRANSITION),
              1u);
    EXPECT_EQ(1u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_QUEUE, VN_MONITOR_ENTITY_QUEUE));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleDuplicateAndOutOfOrderDoNotAdvanceQueueTwice) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::QueueInstanceTable queues(1);
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(MakeQueueCreate(), &queue));
    harken::EffectiveQueueState next{};
    next.version = 2;
    next.cursor = 1;
    next.currentChildOrdinal = 5;
    ASSERT_TRUE(queues.AdvanceAfterSignalCompletion(queue, 1, next));
    harken::monitor::DrainOnceForTests();
    EXPECT_FALSE(queues.AdvanceAfterSignalCompletion(queue, 1, next));
    harken::EffectiveQueueState later{};
    later.version = 3;
    later.cursor = 0;
    later.currentChildOrdinal = 3;
    EXPECT_FALSE(queues.AdvanceAfterSignalCompletion(queue, 1, later));
    ASSERT_TRUE(queues.AdvanceAfterSignalCompletion(queue, 2, later));
    harken::monitor::DrainOnceForTests();
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(3u, effective.version);
    EXPECT_EQ(0u, effective.cursor);

    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    EXPECT_EQ(2u, CountKind(ParseRecords(poll), VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_QUEUE, VN_MONITOR_ENTITY_QUEUE));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleVariableControlPublishesZeroRows) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::VariableControlInstanceTable table(1);
    harken::VariableControlInstanceCreateRequest request{};
    request.world = 7;
    request.rootInstance = 11;
    request.containerStableId = harkentest::ObjectId(13);
    request.programSemanticFingerprint = 17;
    harken::VariableControlInstanceToken token{};
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
              table.Acquire(request, &token));

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    for (const PackedRecord& rec : records) {
        const bool dspGap =
            rec.recordKind == VN_MONITOR_RECORD_EVENT &&
            rec.eventClass == VN_MONITOR_EVENT_DROP &&
            rec.entityKind == VN_MONITOR_ENTITY_NONE &&
            rec.reason == VN_ACTION_TERMINAL_CAPABILITY_UNAVAILABLE;
        EXPECT_TRUE(dspGap);
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleNoProducerLeavesQueueCursorUntouched) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::QueueInstanceTable queues(1);
    harken::QueueInstanceToken queue{};
    ASSERT_TRUE(queues.Acquire(MakeQueueCreate(), &queue));
    harken::EffectiveQueueState effective{};
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(0u, effective.cursor);

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    EXPECT_EQ(0u, CountKind(ParseRecords(poll), VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_QUEUE, VN_MONITOR_ENTITY_QUEUE));
    ASSERT_TRUE(queues.ResolveEffective(queue, &effective));
    EXPECT_EQ(0u, effective.cursor);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleZeroStepActionEmitsAcceptThenResult) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::ActionFiringTable actions;
    ASSERT_TRUE(actions.Reset(1));
    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(41), 7, 0, &prepared));
    const harken::ActionFiringHandle handle = prepared.Handle();
    ASSERT_TRUE(actions.Commit(&prepared, 99));
    harken::ActionFiringSnapshot snapshot{};
    ASSERT_TRUE(actions.Capture(handle, &snapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained, snapshot.state);

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_EQ(1u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_ACCEPT, VN_MONITOR_ENTITY_ACTION));
    EXPECT_EQ(1u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_RESULT, VN_MONITOR_ENTITY_ACTION));
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_REJECT, VN_MONITOR_ENTITY_ACTION));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleRetiredDropDoesNotRollBackIdentity) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::PlayableInstanceTable table(1);
    const harken::PlayableInstanceHandle sound =
        table.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(sound, 0u);
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(table.AdvanceLifecycle(
        sound, harken::PlayableLifecycle::LogicalComplete));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(table.AdvanceLifecycle(
        sound, harken::PlayableLifecycle::AudibleRetiring));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs prior;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &prior.desc, &prior.result));
    EXPECT_GE(CountKind(ParseRecords(prior), VN_MONITOR_RECORD_EVENT,
                        VN_MONITOR_EVENT_STOP, VN_MONITOR_ENTITY_PLAYABLE),
              1u);
    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::FillLogicRingForTests());
    ASSERT_TRUE(
        table.AdvanceLifecycle(sound, harken::PlayableLifecycle::Retired));
    EXPECT_FALSE(table.Classify(sound).live);
    EXPECT_NE(table.Acquire(harken::PlayableKind::Sound, 0), 0u);

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_STOP, VN_MONITOR_ENTITY_PLAYABLE));
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_STATE_TOMBSTONE, 0,
                            VN_MONITOR_ENTITY_PLAYABLE));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleMarkTreeStoppingEmitsActionStop) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::ActionFiringTable actions;
    ASSERT_TRUE(actions.Reset(1));
    harken::ActionFiringStepResult pending{};
    pending.state = harken::ActionStepState::Pending;
    pending.authoredEventIndex = 0;
    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(44), 7, 1, &prepared));
    const harken::ActionFiringHandle handle = prepared.Handle();
    ASSERT_TRUE(actions.SetPreparedStep(prepared, 0, pending));
    ASSERT_TRUE(actions.Commit(&prepared, 102));
    harken::monitor::DrainOnceForTests();
    ASSERT_TRUE(actions.AddLivePlayableCount(handle, 1));
    ASSERT_TRUE(actions.MarkTreeStopping(
        handle, harken::ActionTerminalReason::StopRequested));
    harken::ActionFiringSnapshot snapshot{};
    ASSERT_TRUE(actions.Capture(handle, &snapshot));
    EXPECT_EQ(harken::ActionFiringState::Stopping, snapshot.state);

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_EQ(1u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_ACCEPT, VN_MONITOR_ENTITY_ACTION));
    EXPECT_EQ(1u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_STOP, VN_MONITOR_ENTITY_ACTION));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleStartDropLeavesNoPartialUpsert) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    ASSERT_TRUE(harken::monitor::FillLogicRingLeavingFreeSlotsForTests(1));
    EXPECT_EQ(1u, harken::monitor::LogicRingFreeSlotsForTests());
    harken::PlayableInstanceTable table(1);
    const harken::PlayableInstanceHandle sound =
        table.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(sound, 0u);
    EXPECT_TRUE(table.Classify(sound).live);
    EXPECT_EQ(1u, harken::monitor::LogicRingFreeSlotsForTests());

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_EVENT,
                            VN_MONITOR_EVENT_START, VN_MONITOR_ENTITY_PLAYABLE));
    EXPECT_EQ(0u, CountKind(records, VN_MONITOR_RECORD_STATE_UPSERT, 0,
                            VN_MONITOR_ENTITY_PLAYABLE));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleTransitionCancelBeforePublicationRejects) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::ResetLifecycleDedupeForTests();

    VnMonitorSessionHandle session = 0;
    VnMonitorSessionDesc desc = MakeLifecycleDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    harken::monitor::StopDrainForTests();

    harken::TransitionInstancePool pool(1);
    harken::TransitionInstanceToken token{};
    ASSERT_TRUE(pool.Acquire({128, 1}, &token));
    ASSERT_TRUE(pool.CancelBeforePublication(token));

    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll.desc, &poll.result));
    const std::vector<PackedRecord> records = ParseRecords(poll);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_START,
                        VN_MONITOR_ENTITY_TRANSITION),
              1u);
    EXPECT_GE(CountKind(records, VN_MONITOR_RECORD_EVENT, VN_MONITOR_EVENT_REJECT,
                        VN_MONITOR_ENTITY_TRANSITION),
              1u);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorObserve, LifecycleLegalEventRegistryLocksKindReasonSet) {
    std::ifstream in(FixturePath("lifecycle/legal_event.bin"), std::ios::binary);
    ASSERT_TRUE(in) << FixturePath("lifecycle/legal_event.bin");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    ASSERT_GE(bytes.size(), 8u);
    uint64_t count = 0;
    std::memcpy(&count, bytes.data(), 8);
    ASSERT_EQ(count, 17u);

    std::ifstream manifest(FixturePath("manifest.json"));
    ASSERT_TRUE(manifest) << FixturePath("manifest.json");
    const std::string manifestBody((std::istreambuf_iterator<char>(manifest)),
                                   std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos,
              manifestBody.find("\"path\": \"lifecycle/legal_event.bin\""));
    EXPECT_NE(std::string::npos, manifestBody.find("\"producerPackage\": \"m2-1\""));
    EXPECT_NE(std::string::npos, manifestBody.find("\"kind\": \"legal-event\""));
    EXPECT_NE(std::string::npos, manifestBody.find("\"schemaVersion\": 1"));
    EXPECT_EQ(std::string::npos, manifestBody.find("\"fingerprint\""));

    std::vector<VnMonitorDecodedRecord> decoded(static_cast<size_t>(count));
    std::vector<uint8_t> skip(bytes.size());
    const VnMonitorDecodeResult result = vn_monitor_decode_records(
        bytes.data() + 8, bytes.size() - 8, decoded.data(),
        static_cast<uint32_t>(decoded.size()), skip.data(), skip.size());
    ASSERT_EQ(VN_OK, result.status);
    ASSERT_EQ(count, result.recordsWritten);

    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> got;
    std::set<std::uint64_t> frames;
    for (std::uint32_t i = 0; i < result.recordsWritten; ++i) {
        const VnMonitorDecodedRecord& rec = decoded[i];
        EXPECT_EQ(VN_MONITOR_RECORD_EVENT, rec.header.recordKind);
        EXPECT_EQ(VN_MONITOR_RECORD_FLAG_REQUIRED, rec.header.flags);
        EXPECT_EQ(VN_MONITOR_STREAM_EVENT, rec.header.streamKind);
        EXPECT_EQ(368u, rec.header.recordBytes);
        EXPECT_EQ(3u, rec.header.producerId);
        EXPECT_EQ(0u, rec.header.monitorSessionEpoch);
        EXPECT_EQ(0u, rec.header.validityFlags);
        EXPECT_NE(0u, rec.header.instanceHandle);
        EXPECT_NE(0u, rec.header.instanceGeneration);
        EXPECT_EQ(sizeof(VnMonitorEventPayload), rec.payloadBytes);
        ASSERT_NE(nullptr, rec.payload);
        VnMonitorEventPayload payload{};
        std::memcpy(&payload, rec.payload, sizeof(payload));
        EXPECT_EQ(rec.header.reason, payload.reason);
        got.emplace(rec.header.entityKind, payload.eventClass, rec.header.reason);
        EXPECT_TRUE(frames.insert(rec.header.executeFrame).second);
    }

    const std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> expected = {
        {VN_MONITOR_ENTITY_PLAYABLE, VN_MONITOR_EVENT_START, 0},
        {VN_MONITOR_ENTITY_PLAYABLE, VN_MONITOR_EVENT_RESULT, 0},
        {VN_MONITOR_ENTITY_PLAYABLE, VN_MONITOR_EVENT_STOP, 0},
        {VN_MONITOR_ENTITY_PLAYABLE, VN_MONITOR_EVENT_REJECT, 0},
        {VN_MONITOR_ENTITY_CONTAINER, VN_MONITOR_EVENT_START, 0},
        {VN_MONITOR_ENTITY_CONTAINER, VN_MONITOR_EVENT_RESULT, 0},
        {VN_MONITOR_ENTITY_CONTAINER, VN_MONITOR_EVENT_STOP, 0},
        {VN_MONITOR_ENTITY_CONTAINER, VN_MONITOR_EVENT_REJECT, 0},
        {VN_MONITOR_ENTITY_ACTION, VN_MONITOR_EVENT_ACCEPT, 0},
        {VN_MONITOR_ENTITY_ACTION, VN_MONITOR_EVENT_STOP, 2},
        {VN_MONITOR_ENTITY_ACTION, VN_MONITOR_EVENT_RESULT, 1},
        {VN_MONITOR_ENTITY_ACTION, VN_MONITOR_EVENT_REJECT, 2},
        {VN_MONITOR_ENTITY_QUEUE, VN_MONITOR_EVENT_QUEUE, 0},
        {VN_MONITOR_ENTITY_QUEUE, VN_MONITOR_EVENT_REJECT, 5},
        {VN_MONITOR_ENTITY_TRANSITION, VN_MONITOR_EVENT_START, 0},
        {VN_MONITOR_ENTITY_TRANSITION, VN_MONITOR_EVENT_STOP, 0},
        {VN_MONITOR_ENTITY_TRANSITION, VN_MONITOR_EVENT_REJECT, 0},
    };
    EXPECT_EQ(expected, got);
}
