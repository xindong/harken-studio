#include "MonitorIdentityCut.h"
#include "MonitorIdentitySnapshot.h"
#include "MonitorIdentityCodec.h"
#include "Common/MonitorExecutionTestSupport.h"
#include "Common/TestUtil.h"
#include "Common/ObjectIdTestSupport.h"
#include "PlayableRuntime.h"
#include "MonitorCaptureHubTestHooks.h"
#include "MonitorCaptureHub.h"
#include "MonitorExecutionCodec.h"
#include "monitor_fixture_decode.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <vector>

namespace {
using namespace harken;
using namespace harken::monitor;

using namespace harkentest::monitor;

VnMonitorHostSessionV1 BeginHost() {
    VnMonitorHostSessionV1 info{};
    info.structSize = sizeof(info);
    EXPECT_EQ(VN_OK, Vn_MonitorBeginHostSession(&info));
    return info;
}

VnMonitorHostRegistrationV1 HostRegistration(const VnMonitorHostSessionV1& host,
    VnEmitterHandle emitter, std::uint64_t local = 1) {
    return {sizeof(VnMonitorHostRegistrationV1), VN_MONITOR_HOST_UNITY_EMITTER,
        host.engineInstance, host.sdkSession, emitter, local, 1, 0};
}

VnMonitorHostRegistrationV2 HostDescription(const VnMonitorHostSessionV1& host, VnEmitterHandle emitter) {
    VnMonitorHostRegistrationV2 value{};
    const auto legacy = HostRegistration(host, emitter);
    std::memcpy(&value, &legacy, sizeof(legacy));
    value.structSize = sizeof(value);
    const char name[] = "Player";
    const char path[] = "Root/Player";
    const char scene[] = "Level";
    value.nameBytes = sizeof(name) - 1;
    value.pathBytes = sizeof(path) - 1;
    value.sceneBytes = sizeof(scene) - 1;
    std::memcpy(value.name, name, value.nameBytes);
    std::memcpy(value.path, path, value.pathBytes);
    std::memcpy(value.scene, scene, value.sceneBytes);
    return value;
}

TEST(MonitorHostDescription, CaptureOffRetainsAtomicValuesAndInvalidUpdatesCannotChangeIdentity) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    auto& registry = engine.Engine()->monitorEmitters;
    MonitorEmitterOrigin origin{};
    MonitorHostDescription before{}, after{};
    ASSERT_TRUE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, before));
    EXPECT_EQ(1u, origin.registrationRevision);
    EXPECT_EQ(host.sdkSession, origin.host.sdkSession);
    EXPECT_EQ(0u, IdentityRefreshesForTests());
    EXPECT_FALSE(ExecutionObservationEnabled());
    registration.name[63] = 0xff; // Bytes outside the declared text never become stored values.
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_TRUE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
    EXPECT_EQ(0u, after.name[63]);

    registration.name[0] = 'X';
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_TRUE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
    EXPECT_TRUE(before.Same(after));
    registration.registrationRevision = 2;
    registration.pathBytes = 257;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHostV2(&registration));
    EXPECT_EQ(1u, registry.CaptureOrigin(emitter).registrationRevision);
    registration.pathBytes = 11;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_TRUE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
    EXPECT_EQ(2u, origin.registrationRevision);
    EXPECT_EQ('X', after.name[0]);

    auto legacy = HostRegistration(host, emitter);
    legacy.registrationRevision = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&legacy));
    EXPECT_TRUE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
    legacy.registrationRevision = 3;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&legacy));
    EXPECT_FALSE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
    registration.registrationRevision = 3;
    registration.nameBytes = registration.pathBytes = registration.sceneBytes = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    EXPECT_TRUE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
    EXPECT_EQ(0u, after.nameBytes);
    EXPECT_EQ(3u, origin.registrationRevision);
    EXPECT_EQ(VN_OK, Vn_MonitorEndHostSession(host.engineInstance, host.sdkSession));
    EXPECT_FALSE(registry.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, after));
}

TEST(MonitorHostDescription, MalformedUnicodeAndUnknownFlagsRejectBeforeRegistration) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    const std::vector<std::vector<std::uint8_t>> invalid{
        {0x80}, {0xc0, 0x80}, {0xe0, 0x80, 0x80}, {0xed, 0xa0, 0x80},
        {0xf4, 0x90, 0x80, 0x80}, {0xf0, 0x9f, 0x8e}, {0xff}};
    for (const auto& bytes : invalid) {
        registration.nameBytes = static_cast<std::uint16_t>(bytes.size());
        std::memcpy(registration.name, bytes.data(), bytes.size());
        EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHostV2(&registration));
        EXPECT_FALSE(engine.Engine()->monitorEmitters.CaptureOrigin(emitter).host.Valid());
    }
    const std::uint8_t valid[]{0xf0, 0x9f, 0x8e, 0xb5, 0xe4, 0xb8, 0xad};
    registration.nameBytes = sizeof(valid);
    std::memcpy(registration.name, valid, sizeof(valid));
    registration.descriptionFlags = 2;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHostV2(&registration));
    registration.descriptionFlags = 1;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    MonitorEmitterOrigin origin{};
    MonitorHostDescription value{};
    ASSERT_TRUE(engine.Engine()->monitorEmitters.CaptureDescriptionAt(static_cast<std::uint32_t>(emitter), origin, value));
    EXPECT_EQ(sizeof(valid), value.nameBytes);
    EXPECT_EQ(0, std::memcmp(valid, value.name, sizeof(valid)));
    EXPECT_EQ(1u, value.flags);
}

TEST(MonitorHostDescription, DestroyReuseAndHostRestartNeverReuseOldDescriptions) {
    auto config = harkentest::MakeNullConfig();
    config.maxEmitters = 1;
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle first = 0, second = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&first));
    const auto registration = HostDescription(host, first);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(first));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&second));
    ASSERT_NE(first, second);
    MonitorEmitterOrigin origin{};
    MonitorHostDescription value{};
    EXPECT_FALSE(engine.Engine()->monitorEmitters.CaptureDescriptionAt(0, origin, value));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_MonitorRegisterHostV2(&registration));
    auto replacement = HostDescription(host, second);
    replacement.localSequence = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&replacement));
    EXPECT_TRUE(engine.Engine()->monitorEmitters.CaptureDescriptionAt(0, origin, value));
    const auto later = BeginHost();
    EXPECT_FALSE(engine.Engine()->monitorEmitters.CaptureDescriptionAt(0, origin, value));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorRegisterHostV2(&replacement));
    replacement.sdkSession = later.sdkSession;
    replacement.localSequence = 1;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&replacement));
    EXPECT_TRUE(engine.Engine()->monitorEmitters.CaptureDescriptionAt(0, origin, value));
    EXPECT_EQ(later.sdkSession, origin.host.sdkSession);
}

TEST(MonitorHostDescription, DefaultHostIsUniqueWithinSessionAndRetainsHistoricalOwnership) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    const auto host = BeginHost();
    VnEmitterHandle first = 0, other = 0, replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&first));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&other));
    auto registration = HostDescription(host, first);
    registration.ownershipKind = VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    registration.registrationRevision = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));

    auto duplicate = HostDescription(host, other);
    duplicate.ownershipKind = VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER;
    duplicate.localSequence = 2;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHostV2(&duplicate));
    auto legacyDuplicate = HostRegistration(host, other, 2);
    legacyDuplicate.ownershipKind = VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHost(&legacyDuplicate));
    EXPECT_FALSE(engine.Engine()->monitorEmitters.CaptureOrigin(other).host.Valid());
    duplicate.ownershipKind = VN_MONITOR_HOST_UNITY_EMITTER;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&duplicate));

    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(first));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    ASSERT_NE(first, replacement);
    ASSERT_EQ(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(replacement));
    registration.emitter = replacement;
    registration.localSequence = 3;
    registration.registrationRevision = 1;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    EXPECT_NE(VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID, first);
    EXPECT_NE(VN_MONITOR_STUDIO_DEFAULT_GAME_OBJECT_ID, replacement);
    const auto records = ReadIdentityEvents(session);
    const auto retired = std::find_if(records.begin(), records.end(), [first](const auto& row) {
        return row.fact.factKind == VN_MONITOR_FACT_EMITTER_DESTROYED &&
            row.fact.originEmitter == first;
    });
    ASSERT_NE(records.end(), retired);
    EXPECT_EQ(VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER, retired->fact.hostOwnership);
    EXPECT_EQ(host.sdkSession, retired->fact.hostSession);
    EXPECT_EQ(1u, retired->fact.hostLocalSequence);
    EXPECT_EQ(2u, retired->fact.originHostRevision);

    const auto later = BeginHost();
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorRegisterHostV2(&registration));
    registration.sdkSession = later.sdkSession;
    registration.localSequence = 1;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    duplicate.sdkSession = later.sdkSession;
    duplicate.ownershipKind = VN_MONITOR_HOST_STUDIO_DEFAULT_EMITTER;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorRegisterHostV2(&duplicate));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionIdentity, EmitterDestroyRetainsHostAndGenerationBeforeSlotReuse) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    const auto host = BeginHost();
    VnEmitterHandle first = 0, reused = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&first));
    auto registration = HostRegistration(host, first);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(first));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_DestroyEmitter(first));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reused));
    ASSERT_EQ(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(reused));
    ASSERT_NE(first, reused);
    registration = HostRegistration(host, reused, 2);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    const auto records = ReadIdentityEvents(session);
    ASSERT_EQ(5u, records.size());
    EXPECT_EQ(VN_MONITOR_FACT_EMITTER_CREATED, records[0].fact.factKind);
    EXPECT_FALSE(FactHas(records[0].fact, VN_MONITOR_FACT_FIELD_HOST_KEY));
    EXPECT_EQ(VN_MONITOR_FACT_EMITTER_BINDING_CHANGED, records[1].fact.factKind);
    const auto& retired = records[2];
    EXPECT_EQ(VN_MONITOR_FACT_EMITTER_DESTROYED, retired.fact.factKind);
    EXPECT_EQ(VN_MONITOR_SUBJECT_EMITTER, retired.fact.subjectNamespace);
    EXPECT_EQ(first, retired.header.instanceHandle);
    EXPECT_EQ(first >> 32, retired.header.instanceGeneration);
    EXPECT_EQ(host.engineInstance, retired.header.engineMonitorInstanceId);
    EXPECT_EQ(first, retired.fact.originEmitter);
    EXPECT_EQ(host.sdkSession, retired.fact.hostSession);
    EXPECT_EQ(1u, retired.fact.hostLocalSequence);
    EXPECT_EQ(1u, retired.fact.originHostRevision);
    EXPECT_EQ(static_cast<unsigned>(MonitorBindingState::Detached), retired.fact.stateBinding);
    EXPECT_EQ(static_cast<unsigned>(MonitorBindingState::Detached), retired.fact.spatialBinding);
    EXPECT_FALSE(FactHas(retired.fact, VN_MONITOR_FACT_FIELD_CURRENT_STATE_EMITTER));
    EXPECT_FALSE(FactHas(retired.fact, VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER));
    EXPECT_FALSE(FactHas(retired.fact, VN_MONITOR_FACT_FIELD_CONTENT_OBJECT));
    EXPECT_EQ(reused, records[3].header.instanceHandle);
    EXPECT_FALSE(FactHas(records[3].fact, VN_MONITOR_FACT_FIELD_HOST_KEY));
    EXPECT_EQ(2u, records[4].fact.hostLocalSequence);
    EXPECT_EQ(reused, records[4].fact.currentStateEmitter);
    EXPECT_EQ(reused, records[4].fact.currentSpatialEmitter);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionIdentity, StateOnlyCreationAndDestroyNeverClaimSpatialBinding) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VnEmitterCreateDescV1 desc{sizeof(desc), VN_EMITTER_CREATE_STATE_ONLY, 0};
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitterEx(&desc, &emitter));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    const auto records = ReadIdentityEvents(session);
    ASSERT_EQ(2u, records.size());
    for (const auto& row : records) {
        EXPECT_EQ(static_cast<unsigned>(MonitorEmitterKind::ExplicitStateOnly), row.fact.originKind);
        EXPECT_EQ(static_cast<unsigned>(MonitorBindingState::None), row.fact.spatialBinding);
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_CURRENT_SPATIAL_EMITTER));
    }
    EXPECT_EQ(static_cast<unsigned>(MonitorBindingState::Bound), records[0].fact.stateBinding);
    EXPECT_EQ(static_cast<unsigned>(MonitorBindingState::Detached), records[1].fact.stateBinding);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionIdentity, HostReconnectAndEndPublishOnlyActualRegistrationChanges) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto first = BeginHost();
    auto registration = HostRegistration(first, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    const auto second = BeginHost();
    ASSERT_NE(first.sdkSession, second.sdkSession);
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorEndHostSession(first.engineInstance, first.sdkSession));
    registration = HostRegistration(second, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    ASSERT_EQ(VN_OK, Vn_MonitorEndHostSession(second.engineInstance, second.sdkSession));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorEndHostSession(second.engineInstance, second.sdkSession));
    const auto records = ReadIdentityEvents(session);
    ASSERT_EQ(5u, records.size());
    EXPECT_EQ(first.sdkSession, records[1].fact.hostSession);
    EXPECT_FALSE(FactHas(records[2].fact, VN_MONITOR_FACT_FIELD_HOST_KEY));
    EXPECT_EQ(second.sdkSession, records[3].fact.hostSession);
    EXPECT_FALSE(FactHas(records[4].fact, VN_MONITOR_FACT_FIELD_HOST_KEY));
    for (std::size_t i = 1; i < records.size(); ++i) {
        EXPECT_EQ(VN_MONITOR_FACT_EMITTER_BINDING_CHANGED, records[i].fact.factKind);
        EXPECT_EQ(emitter, records[i].fact.currentStateEmitter);
        EXPECT_EQ(emitter, records[i].fact.currentSpatialEmitter);
    }
    EXPECT_TRUE(engine.Engine()->emitters.IsPublicLive(emitter));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionIdentity, CaptureOffKeepsHostValuesWithoutInventingHistoricalEvents) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostRegistration(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    auto session = OpenIdentityEvents();
    EXPECT_TRUE(ReadIdentityEvents(session).empty());
    registration.registrationRevision = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    const auto update = ReadIdentityEvents(session);
    ASSERT_EQ(1u, update.size());
    EXPECT_EQ(2u, update[0].fact.originHostRevision);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    registration.registrationRevision = 3;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    session = OpenIdentityEvents();
    EXPECT_TRUE(ReadIdentityEvents(session).empty());
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    const auto destroyed = ReadIdentityEvents(session);
    ASSERT_EQ(1u, destroyed.size());
    EXPECT_EQ(VN_MONITOR_FACT_EMITTER_DESTROYED, destroyed[0].fact.factKind);
    EXPECT_EQ(host.sdkSession, destroyed[0].fact.hostSession);
    EXPECT_EQ(3u, destroyed[0].fact.originHostRevision);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionIdentity, CapabilitiesNameRealEmitterProducersAndRestartUsesNewEngine) {
    std::array<IdentityRecord, 2> snapshots{};
    for (unsigned i = 0; i != snapshots.size(); ++i) {
        harkentest::EngineSession engine(harkentest::MakeNullConfig());
        ASSERT_EQ(VN_OK, engine.InitResult());
        const auto session = OpenIdentityEvents();
        VnMonitorEventSessionStatusV2 status{};
        status.structSize = sizeof(status);
        ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(session, reinterpret_cast<VnMonitorEventSessionStatus*>(&status)));
        for (auto kind : {VN_MONITOR_FACT_EMITTER_CREATED, VN_MONITOR_FACT_EMITTER_DESTROYED, VN_MONITOR_FACT_EMITTER_BINDING_CHANGED, VN_MONITOR_FACT_PLAYABLE_PREPARED})
            EXPECT_NE(0u, status.supportedFactKinds[0] & (UINT64_C(1) << kind));
        EXPECT_EQ(0u, status.supportedFactKinds[0] & (UINT64_C(1) << VN_MONITOR_FACT_RENDER_STARTED));
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        const auto records = ReadIdentityEvents(session);
        ASSERT_EQ(1u, records.size());
        snapshots[i] = records[0];
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    }
    EXPECT_EQ(snapshots[0].header.instanceHandle, snapshots[1].header.instanceHandle);
    EXPECT_NE(snapshots[0].header.engineMonitorInstanceId, snapshots[1].header.engineMonitorInstanceId);
}

std::vector<IdentityRecord> ReadRequestEvents(VnMonitorSessionHandle session, std::uint32_t* operations = nullptr) {
    auto all = ReadIdentityEvents(session, operations);
    std::erase_if(all, [](const auto& row) {
        return row.fact.factKind != VN_MONITOR_FACT_REQUEST_SUBMITTED && row.fact.factKind != VN_MONITOR_FACT_REQUEST_RETURNED;
    });
    return all;
}

TEST(MonitorExecutionRequest, RefusalsAndEmptyStopAllKeepOneRequestAndLegacyProjection) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    VnMonitorSessionHandle legacy = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&desc, &legacy));
    StopDrainForTests();
    const auto before = engine.Engine()->monitorRequests.LastSequence();
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostAction(nullptr, nullptr));
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostActionByName(nullptr, nullptr));
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostActionPinned(nullptr, nullptr));
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostActionResolved(nullptr, 0, 0, nullptr));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(0, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_StopActionFiring(0, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_StopEmitter(0, 0));
    EXPECT_EQ(VN_OK, Vn_StopAll(0));
    EXPECT_EQ(before + 8, engine.Engine()->monitorRequests.LastSequence());
    std::uint32_t typedOperations = 0, oldOperations = 0;
    const auto records = ReadRequestEvents(session, &typedOperations);
    ASSERT_EQ(16u, records.size());
    EXPECT_EQ(0u, typedOperations);
    EXPECT_TRUE(ReadRequestEvents(legacy, &oldOperations).empty());
    EXPECT_EQ(8u, oldOperations);
    for (unsigned i = 0; i != 8; ++i) {
        const auto& submitted = records[i * 2];
        const auto& returned = records[i * 2 + 1];
        EXPECT_EQ(VN_MONITOR_FACT_REQUEST_SUBMITTED, submitted.fact.factKind);
        EXPECT_EQ(VN_MONITOR_FACT_REQUEST_RETURNED, returned.fact.factKind);
        EXPECT_EQ(before + i + 1, submitted.fact.request.sequence);
        EXPECT_EQ(submitted.fact.request.sequence, returned.fact.request.sequence);
        EXPECT_EQ(static_cast<unsigned>(MonitorRequestOrigin::NativeAPI), returned.fact.request.origin);
        EXPECT_FALSE(FactHas(returned.fact, VN_MONITOR_FACT_FIELD_SUBJECT));
        EXPECT_FALSE(FactHas(returned.fact, VN_MONITOR_FACT_FIELD_TARGET_VALIDATED));
        EXPECT_LE(submitted.fact.timestampNs, returned.fact.timestampNs);
        EXPECT_LT(submitted.header.sourceSequence, returned.header.sourceSequence);
        EXPECT_EQ(i < 4 ? VN_INVALID_ARG : (i < 7 ? VN_STALE_HANDLE : VN_OK), returned.fact.result);
        EXPECT_EQ(i >= 4 && i < 7, FactHas(returned.fact, VN_MONITOR_FACT_FIELD_TARGET_HANDLE));
    }
    EXPECT_EQ(1u, records.back().fact.target.scope);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(legacy));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionRequest, AcceptedControlActionReturnsItsOwnGenerationAndDefaultEmitter) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    auto variable = harkentest::MakeVariableDefinition(harkentest::ObjectId(89004),
        VariableKindV1::Smooth, -1.0f, 1.0f, 0.0f);
    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::SetVariable;
    step.target.kind = ActionTargetKindV1::Variable;
    step.target.stableId = variable.variableId;
    for (auto* time : {&step.delay, &step.fade, &step.cooldown}) {
        time->domain = AuthoredTimeDomainV1::AbsoluteNanoseconds;
        time->denominator = 1;
    }
    ActionProgramViewV1 action{};
    action.actionStableId = harkentest::ObjectId(89002);
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = harkentest::ObjectId(89003);
    world.identity.capabilityMask = kWorldCapabilityActionDefinitions | kWorldCapabilityVariableDefinitions;
    world.variables = &variable;
    world.variableCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    ASSERT_EQ(WorldInstallStatus::Ok, InstallContainerWorldV1(*engine.Engine(), world).status);
    const auto session = OpenIdentityEvents();
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = action.actionStableId;
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    const auto records = ReadRequestEvents(session);
    ASSERT_EQ(2u, records.size());
    const auto& returned = records[1];
    EXPECT_EQ(VN_MONITOR_SUBJECT_ACTION, returned.fact.subjectNamespace);
    EXPECT_EQ(firing, returned.header.instanceHandle);
    EXPECT_EQ(firing >> kActionFiringSlotBits, returned.header.instanceGeneration);
    EXPECT_EQ(action.actionStableId, returned.header.stableId);
    EXPECT_EQ(action.programSemanticFingerprint, returned.header.programSemanticFingerprint);
    EXPECT_EQ(world.identity.projectId, returned.fact.projectId);
    EXPECT_EQ(firing, returned.fact.root);
    EXPECT_TRUE(FactHas(returned.fact, VN_MONITOR_FACT_FIELD_TARGET_VALIDATED));
    EXPECT_EQ(static_cast<unsigned>(MonitorEmitterKind::DefaultState), returned.fact.originKind);
    ActionFiringSnapshot snapshot{};
    ASSERT_TRUE(engine.Engine()->actionFirings.Capture(firing, &snapshot));
    EXPECT_EQ(snapshot.playbackRequest.key.sequence, returned.fact.request.sequence);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, PreparedCandidatesUseLastObservedContentAfterMetadataPrefill) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    PlayableInstanceTable table(1);
    const MonitorRequestContext request{{ExecutionEngineInstance(), 141}, MonitorRequestOrigin::NativeAPI,
        VN_MONITOR_OPERATION_POST_ACTION};
    PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared, request));
    MonitorContentIdentity content{harkentest::ObjectId(90201), harkentest::ObjectId(90202), 17, 200, 300};
    PlayableRootMetadata metadata{};
    metadata.definitionKind = PlayableKind::Sound;
    metadata.actionFiring = UINT64_C(1) << kActionFiringSlotBits;
    metadata.actionExecutionOrdinal = 4;
    for (unsigned variation = 0; variation != 6; ++variation) {
        if (variation == 1) content.stableId = harkentest::ObjectId(90203);
        if (variation == 2) ++content.worldGeneration;
        if (variation == 3) ++content.programSemanticFingerprint;
        if (variation == 4) ++content.contentRevision;
        if (variation == 5) content.projectId = harkentest::ObjectId(90204);
        metadata.projectId = content.projectId;
        metadata.definitionStableId = content.stableId;
        metadata.worldGeneration = content.worldGeneration;
        metadata.programSemanticFingerprint = content.programSemanticFingerprint;
        ASSERT_TRUE(table.SetPreparedRootMetadata(prepared, metadata));
        ASSERT_TRUE(table.SetPreparedContentIdentity(prepared, content));
        ASSERT_TRUE(table.SetPreparedContentIdentity(prepared, content));
        const auto rows = ReadIdentityEvents(session);
        ASSERT_EQ(1u, rows.size()) << variation;
        const auto& row = rows.front();
        EXPECT_EQ(VN_MONITOR_FACT_PLAYABLE_PREPARED, row.fact.factKind);
        EXPECT_EQ(VN_MONITOR_SUBJECT_PLAYABLE, row.fact.subjectNamespace);
        EXPECT_EQ(prepared.Handle(), row.header.instanceHandle);
        EXPECT_EQ(prepared.Handle(), row.fact.root);
        EXPECT_EQ(content.stableId, row.header.stableId);
        EXPECT_EQ(content.projectId, row.fact.projectId);
        EXPECT_EQ(content.worldGeneration, row.header.worldGeneration);
        EXPECT_EQ(content.programSemanticFingerprint, row.header.programSemanticFingerprint);
        EXPECT_EQ(content.contentRevision, row.fact.contentRevision);
        EXPECT_EQ(request.key.sequence, row.fact.playbackRequest.sequence);
        EXPECT_EQ(metadata.actionFiring, row.fact.selection.action);
        EXPECT_EQ(4u, row.fact.selection.executionOrdinal);
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_REASON));
        EXPECT_EQ(0u, table.LiveCount());
    }
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    EXPECT_TRUE(ReadIdentityEvents(session).empty());
    EXPECT_EQ(table.Capacity(), table.FreeCount());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, PreparedCandidateReuseAndRollbackPreserveNativeGenerationAndOnce) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    PlayableInstanceTable table(1);
    MonitorContentIdentity content{harkentest::ObjectId(90301), harkentest::ObjectId(90302), 19, 220, 320};
    PreparedPlayableInstance first;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &first));
    ASSERT_TRUE(table.SetPreparedContentIdentity(first, content));
    const auto old = first;
    ASSERT_TRUE(table.CancelPrepared(&first));
    PreparedPlayableInstance next;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &next));
    ASSERT_EQ(static_cast<std::uint32_t>(old.Handle()), static_cast<std::uint32_t>(next.Handle()));
    ASSERT_NE(old.Handle(), next.Handle());
    EXPECT_FALSE(table.SetPreparedContentIdentity(old, content));
    ASSERT_TRUE(table.SetPreparedContentIdentity(next, content));
    auto rows = ReadIdentityEvents(session);
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ(old.Handle(), rows[0].header.instanceHandle);
    EXPECT_EQ(next.Handle(), rows[1].header.instanceHandle);
    for (const auto& row : rows) EXPECT_EQ(VN_MONITOR_FACT_PLAYABLE_PREPARED, row.fact.factKind);
    PlayableInstanceTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(next, &proof));
    EXPECT_FALSE(table.SetPreparedContentIdentity(next, content));
    EXPECT_TRUE(ReadIdentityEvents(session).empty());
    proof.Release();
    ASSERT_TRUE(table.CommitPrepared(&next, false));
    EXPECT_FALSE(table.SetPreparedContentIdentity(next, content));
    rows = ReadIdentityEvents(session);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(VN_MONITOR_FACT_PLAYABLE_ACTIVE, rows[0].fact.factKind);
    ASSERT_TRUE(table.RollbackCommitted(next.Handle()));
    ASSERT_TRUE(table.SetPreparedContentIdentity(next, content));
    EXPECT_TRUE(ReadIdentityEvents(session).empty());
    ++content.contentRevision;
    ASSERT_TRUE(table.SetPreparedContentIdentity(next, content));
    rows = ReadIdentityEvents(session);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(VN_MONITOR_FACT_PLAYABLE_PREPARED, rows[0].fact.factKind);
    EXPECT_EQ(next.Handle(), rows[0].header.instanceHandle);
    EXPECT_EQ(content.contentRevision, rows[0].fact.contentRevision);
    ASSERT_TRUE(table.CancelPrepared(&next));
    EXPECT_EQ(table.Capacity(), table.FreeCount());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, RetiredChildKeepsItsContentAndActualParentAcrossReuse) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    PlayableInstanceTable table(2);
    const MonitorRequestContext request{{ExecutionEngineInstance(), 123}, MonitorRequestOrigin::StudioPreview,
        VN_MONITOR_OPERATION_PLAY_PREVIEW};
    PreparedPlayableInstance parent;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &parent, request));
    PlayableRootMetadata metadata{};
    metadata.projectId = harkentest::ObjectId(90001);
    metadata.definitionStableId = harkentest::ObjectId(90002);
    metadata.definitionKind = PlayableKind::Container;
    metadata.programSemanticFingerprint = 100;
    metadata.worldGeneration = 17;
    metadata.actionFiring = UINT64_C(1) << kActionFiringSlotBits;
    metadata.actionExecutionOrdinal = 3;
    ASSERT_TRUE(table.SetPreparedRootMetadata(parent, metadata));
    ASSERT_TRUE(table.CommitPrepared(&parent));
    const std::array<PlayableTerminalOutcome, 5> outcomes{{
        {PlayableTerminalKind::NaturalCompletion, VN_OK, 0},
        {PlayableTerminalKind::RequestedStop, VN_OK, 11},
        {PlayableTerminalKind::ParentCancellation, VN_OK, 12},
        {PlayableTerminalKind::ReplacementReset, VN_OK, 13},
        {PlayableTerminalKind::Failure, VN_BAD_STREAM_FILE, 14}}};
    std::array<std::uint64_t, outcomes.size()> children{};
    for (unsigned i = 0; i != children.size(); ++i) {
        PreparedPlayableInstance child;
        ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, parent.Handle(), &child));
        children[i] = child.Handle();
        ASSERT_TRUE(table.SetPreparedContentIdentity(child,
            {metadata.projectId, harkentest::ObjectId(90100 + i), 17, 200 + i, 300 + i}));
        ASSERT_TRUE(table.CommitPrepared(&child));
        ASSERT_TRUE(table.SetTerminalOutcome(child.Handle(), outcomes[i]));
        ASSERT_TRUE(table.AdvanceLifecycle(child.Handle(), PlayableLifecycle::Retired));
    }
    const auto rows = ReadIdentityEvents(session);
    for (unsigned i = 0; i != children.size(); ++i) {
        std::array<unsigned, 4> phases{};
        for (const auto& row : rows) {
            if (row.header.instanceHandle != children[i]) continue;
            if (row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_PREPARED) ++phases[0];
            else if (row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_ACTIVE) ++phases[1];
            else if (row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_FINISHED ||
                     row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_RETIRED) {
                ++phases[row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_FINISHED ? 2 : 3];
                EXPECT_TRUE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
                EXPECT_TRUE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_RESULT));
                EXPECT_TRUE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_REASON));
                EXPECT_EQ(static_cast<unsigned>(outcomes[i].kind), row.fact.terminalKind);
                EXPECT_EQ(outcomes[i].resultCode, row.fact.result);
                EXPECT_EQ(outcomes[i].reasonCode, row.header.reason);
            }
            else ADD_FAILURE() << "Unexpected child fact " << row.fact.factKind;
            if (row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_FINISHED &&
                row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_RETIRED) {
                EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
                EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_RESULT));
            }
            EXPECT_EQ(harkentest::ObjectId(90100 + i), row.header.stableId);
            EXPECT_EQ(300 + i, row.fact.contentRevision);
            EXPECT_EQ(parent.Handle(), row.header.playableInstanceHandle);
            EXPECT_EQ(parent.Handle(), row.fact.root);
            EXPECT_EQ(metadata.actionFiring, row.header.actionInstanceHandle);
            EXPECT_EQ(metadata.actionFiring, row.fact.selection.action);
            EXPECT_EQ(3u, row.fact.selection.executionOrdinal);
            EXPECT_EQ(request.key.sequence, row.fact.playbackRequest.sequence);
            EXPECT_NE(children[i], row.header.playableInstanceHandle);
        }
        EXPECT_EQ((std::array<unsigned, 4>{1, 1, 1, 1}), phases);
    }
    EXPECT_EQ(static_cast<std::uint32_t>(children[0]), static_cast<std::uint32_t>(children[1]));
    EXPECT_NE(children[0], children[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, InternalVoiceKeepsRetirementIdentityAndSelectionWithoutExternalKey) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VoiceLedger ledger(1, 2);
    harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
    ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(
        static_cast<std::uint32_t>(ledger.MonitorSelectionCapacity()), &monitorBacking));
    const auto parent = UINT64_C(1) << 32;
    const auto leaf = parent | 2;
    const auto render = (UINT64_C(4) << 32) | 1;
    const MonitorRequestContext request{{ExecutionEngineInstance(), 124}, MonitorRequestOrigin::StudioPreview,
        VN_MONITOR_OPERATION_PLAY_PREVIEW};
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned({0, parent}, &slot, request));
    VoiceLedger::MonitorContent content{};
    content.content = {harkentest::ObjectId(91001), harkentest::ObjectId(91002), 19, 201, 301};
    content.kind = VN_MONITOR_CONTENT_SOUND;
    content.root = parent;
    content.leaf = leaf;
    content.resourceOwner = parent;
    content.ownerEpoch = 9;
    content.logicCommit = 57;
    ledger.RecordMonitorContent(slot, content);
    ledger.BindRender(slot, render, 128);
    MonitorControlCause selected{};
    selected.actionFiring = UINT64_C(2) << kActionFiringSlotBits;
    selected.parentPlayable = parent;
    selected.executionOrdinal = 7;
    ledger.RecordMonitorSelection(render, selected);
    auto other = selected;
    other.executionOrdinal = 8;
    ledger.RecordMonitorSelection(render, other);
    VoiceLedger::MonitorIdentity before{};
    ASSERT_TRUE(ledger.CaptureMonitorIdentity(slot, &before));
    ledger.Reap(render);
    VnVoiceHandle reused = 0;
    ASSERT_TRUE(ledger.Acquire(&reused, &slot));
    ASSERT_NE(before.logicalHandle, reused);
    const auto rows = ReadIdentityEvents(session);
    ASSERT_EQ(3u, rows.size());
    EXPECT_EQ(VN_MONITOR_FACT_VOICE_LIVE, rows[0].fact.factKind);
    EXPECT_EQ(VN_MONITOR_FACT_SELECTION_COMMITTED, rows[1].fact.factKind);
    EXPECT_EQ(VN_MONITOR_FACT_VOICE_ENDED, rows[2].fact.factKind);
    for (const auto& row : rows) {
        EXPECT_EQ(before.logicalHandle, row.header.instanceHandle);
        EXPECT_EQ(before.generation, row.header.instanceGeneration);
        EXPECT_EQ(before.logicalHandle, row.fact.logicalVoice);
        EXPECT_EQ(render, row.fact.renderActivation);
        EXPECT_EQ(leaf, row.fact.serviceLeaf);
        EXPECT_EQ(parent, row.fact.resourceOwner);
        EXPECT_EQ(9u, row.fact.ownerEpoch);
        EXPECT_EQ(parent, row.fact.root);
        EXPECT_EQ(content.content.stableId, row.header.stableId);
        EXPECT_EQ(301u, row.fact.contentRevision);
        EXPECT_EQ(57u, row.header.telemetry.logicCommitSequence);
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
    }
    EXPECT_EQ(7u, rows[1].fact.selection.executionOrdinal);
    EXPECT_EQ(7u, rows[2].fact.selection.executionOrdinal);
    EXPECT_FALSE(FactHas(rows[1].fact, VN_MONITOR_FACT_FIELD_SELECTION_REQUEST));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, DelayedSelectionKeepsOriginalValuesAfterLedgerReuse) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VoiceLedger ledger(1, 2);
    harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
    ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(
        static_cast<std::uint32_t>(ledger.MonitorSelectionCapacity()), &monitorBacking));
    const auto parent = UINT64_C(1) << 32;
    const auto render = (UINT64_C(4) << 32) | 1;
    const MonitorRequestContext request{{ExecutionEngineInstance(), 125}, MonitorRequestOrigin::StudioPreview,
        VN_MONITOR_OPERATION_PLAY_PREVIEW};
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned({0, parent}, &slot, request));
    VoiceLedger::MonitorContent content{};
    content.content = {harkentest::ObjectId(92001), harkentest::ObjectId(92002), 19, 201, 301};
    content.kind = VN_MONITOR_CONTENT_SOUND;
    content.root = parent;
    content.occurrence = {parent, harkentest::ObjectId(92003), 4, 7, true, true};
    ledger.RecordMonitorContent(slot, content);
    VoiceLedger::MonitorSelection candidate{};
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, render, &candidate));
    ledger.BindRender(slot, render, 128);
    ledger.Reap(render);
    VnVoiceHandle reused = 0;
    ASSERT_TRUE(ledger.Acquire(&reused, &slot));
    const auto newerRender = (UINT64_C(5) << 32) | 1;
    ledger.BindRender(slot, newerRender, 128);
    MonitorLifecycleFacts saved{};
    std::uint32_t generation = 0;
    ASSERT_TRUE(candidate.CaptureFacts(&saved, &generation));
    ASSERT_NE(reused, saved.logicalVoice);
    MonitorControlCause cause{};
    cause.parentPlayable = parent;
    candidate.SetCause(cause);
    auto alias = candidate;
    ledger.PublishMonitorSelection(candidate);
    ledger.PublishMonitorSelection(candidate);
    ledger.PublishMonitorSelection(alias);
    const auto rows = ReadIdentityEvents(session);
    unsigned selected = 0;
    for (const auto& row : rows) {
        if (row.fact.factKind != VN_MONITOR_FACT_SELECTION_COMMITTED) continue;
        ++selected;
        EXPECT_EQ(saved.logicalVoice, row.header.instanceHandle);
        EXPECT_EQ(generation, row.header.instanceGeneration);
        EXPECT_EQ(render, row.fact.renderActivation);
        EXPECT_EQ(content.content.stableId, row.header.stableId);
        EXPECT_EQ(301u, row.fact.contentRevision);
        EXPECT_EQ(content.occurrence.clipId, row.header.telemetry.clipId);
        EXPECT_EQ(4u, row.header.telemetry.loopIteration);
        EXPECT_EQ(request.key.sequence, row.fact.playbackRequest.sequence);
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_SELECTION_REQUEST));
    }
    EXPECT_EQ(1u, selected);
    VoiceLedger::MonitorIdentity current{};
    ASSERT_TRUE(ledger.CaptureMonitorIdentity(slot, &current));
    EXPECT_EQ(reused, current.logicalHandle);
    EXPECT_EQ(0u, current.selectionCause.parentPlayable);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorSelectionPool, CauseForkRollbackPreservesTheOriginalAliasAndReturnsStorage) {
    VoiceLedger ledger(1, 1, 3);
    harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
    EXPECT_EQ(0u, ledger.MonitorSelectionStorageBytes());
    ledger.SetMonitorSelectionCommitPageLimitForTests(0);
    EXPECT_FALSE(ledger.PrepareMonitorSelectionBacking(3, &monitorBacking));
    EXPECT_FALSE(monitorBacking.Valid());
    ledger.SetMonitorSelectionCommitPageLimitForTests(SIZE_MAX);
    ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(
        static_cast<std::uint32_t>(ledger.MonitorSelectionCapacity()), &monitorBacking));
    const auto bytes = ledger.MonitorSelectionStorageBytes();
    const auto use = monitorBacking.Use();
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned({0, UINT64_C(1) << 32}, &slot));
    VoiceLedger::MonitorSelection original;
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, UINT64_C(1) << 32, &original));
    auto alias = original;
    auto branch = original;
    MonitorControlCause cause{};
    cause.request = {{41, 42}, MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_POST_ACTION};
    cause.parentPlayable = UINT64_C(2) << 32;
    cause.executionOrdinal = 7;
    branch.SetCause(cause);
    EXPECT_FALSE(original.Cause().request.key.Valid());
    EXPECT_FALSE(alias.Cause().request.key.Valid());
    EXPECT_EQ(cause.request.key.sequence, branch.Cause().request.key.sequence);
    MonitorLifecycleFacts before{}, changed{};
    ASSERT_TRUE(original.CaptureFacts(&before));
    ASSERT_TRUE(branch.CaptureFacts(&changed));
    EXPECT_EQ(before.logicalVoice, changed.logicalVoice);
    branch = {};
    VoiceLedger::MonitorSelection recycled;
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, UINT64_C(1) << 32, &recycled));
    original = {};
    ASSERT_TRUE(alias.CaptureFacts(&before));
    alias = {};
    VoiceLedger::MonitorSelection second;
    EXPECT_TRUE(ledger.CaptureMonitorSelection(slot, UINT64_C(1) << 32, &second));
    VoiceLedger::MonitorSelection third;
    EXPECT_TRUE(ledger.CaptureMonitorSelection(slot, UINT64_C(1) << 32, &third));
    EXPECT_EQ(3u, ledger.MonitorSelectionCapacity());
    EXPECT_EQ(bytes, ledger.MonitorSelectionStorageBytes());
}

TEST(MonitorSelectionPool, FrozenAliasesOutliveLedgerAndPreserveMoveAndCopyOwnership) {
    VoiceLedger::MonitorSelection retained;
    const auto render = UINT64_C(2) << 32;
    {
        VoiceLedger ledger(1, 1, 2);
        harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
        ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(1, &monitorBacking));
        std::uint32_t slot = 0;
        ASSERT_TRUE(ledger.AcquirePlayableOwned({0, UINT64_C(1) << 32}, &slot));
        ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, render, &retained));
        auto alias = retained;
        retained = std::move(alias);
        EXPECT_EQ(0u, alias.RenderActivation());
    }
    MonitorLifecycleFacts saved{};
    ASSERT_TRUE(retained.CaptureFacts(&saved));
    EXPECT_EQ(render, saved.renderActivation);
    VoiceLedger::MonitorSelectionBacking laterBacking;
    ASSERT_TRUE(retained.PrepareMonitorSelectionBacking(1, &laterBacking));
    const auto laterUse = laterBacking.Use();
    laterBacking.Release();
    auto branch = retained;
    MonitorControlCause cause{};
    cause.executionOrdinal = 71;
    branch.SetCause(cause);
    EXPECT_EQ(71u, branch.Cause().executionOrdinal);
    EXPECT_NE(71u, retained.Cause().executionOrdinal);
    branch = {};
    auto alias = retained;
    retained = {};
    EXPECT_EQ(render, alias.RenderActivation());
    retained = alias;
    alias = {};
    EXPECT_EQ(render, retained.RenderActivation());
    retained = {};
}

TEST(MonitorExecutionLifecycle, PublishedSelectionAliasesCannotReachAReusedDiagnosticSlot) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VoiceLedger ledger(1, 1, 1);
    harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
    ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(
        static_cast<std::uint32_t>(ledger.MonitorSelectionCapacity()), &monitorBacking));
    const auto firstRender = UINT64_C(1) << 32;
    const auto nextRender = UINT64_C(2) << 32;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned({0, UINT64_C(1) << 32}, &slot));
    VoiceLedger::MonitorSelection first;
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, firstRender, &first));
    auto oldAlias = first;
    ledger.BindRender(slot, firstRender, 128);
    ledger.PublishMonitorSelection(first);
    ledger.Reap(firstRender);
    ASSERT_TRUE(ledger.AcquirePlayableOwned({0, UINT64_C(2) << 32}, &slot));
    VoiceLedger::MonitorSelection next;
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, nextRender, &next));
    ledger.BindRender(slot, nextRender, 128);
    MonitorControlCause staleCause{};
    staleCause.executionOrdinal = 99;
    oldAlias.SetCause(staleCause);
    ledger.PublishMonitorSelection(oldAlias);
    oldAlias = {};
    EXPECT_EQ(nextRender, next.RenderActivation());
    EXPECT_EQ(UINT32_MAX, next.Cause().executionOrdinal);
    ledger.PublishMonitorSelection(next);
    const auto rows = ReadIdentityEvents(session);
    std::vector<std::uint64_t> selected;
    for (const auto& row : rows)
        if (row.fact.factKind == VN_MONITOR_FACT_SELECTION_COMMITTED)
            selected.push_back(row.fact.renderActivation);
    EXPECT_EQ((std::vector<std::uint64_t>{firstRender, nextRender}), selected);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, SelectionPoolExhaustionReportsLossAndLeavesNativeVoiceAdmitted) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VoiceLedger ledger(1, 1, 1);
    harken::VoiceLedger::MonitorSelectionBacking monitorBacking;
    ASSERT_TRUE(ledger.PrepareMonitorSelectionBacking(
        static_cast<std::uint32_t>(ledger.MonitorSelectionCapacity()), &monitorBacking));
    VnVoiceHandle voice = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.Acquire(&voice, &slot));
    const auto render = UINT64_C(1) << 32;
    ledger.BindRender(slot, render, 128);
    VoiceLedger::MonitorSelection retained, overflow;
    ASSERT_TRUE(ledger.CaptureMonitorSelection(slot, render, &retained));
    EXPECT_FALSE(ledger.CaptureMonitorSelection(slot, render, &overflow));
    EXPECT_EQ(VoiceLedger::HandleKind::Live, ledger.Classify(voice).kind);
    EXPECT_EQ(render, ledger.Classify(voice).renderHandle);
    auto branch = retained;
    MonitorControlCause cause{};
    cause.executionOrdinal = 3;
    branch.SetCause(cause);
    EXPECT_EQ(0u, branch.RenderActivation());
    EXPECT_EQ(render, retained.RenderActivation());
    EXPECT_EQ(UINT32_MAX, retained.Cause().executionOrdinal);
    ledger.PublishMonitorSelection(retained, &cause);
    DrainOnceForTests();
    VnMonitorEventSessionStatusV2 status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(session,
        reinterpret_cast<VnMonitorEventSessionStatus*>(&status)));
    EXPECT_EQ(2u, status.sourceDroppedRecords);
    EXPECT_EQ(VoiceLedger::HandleKind::Live, ledger.Classify(voice).kind);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, ParkAndReturnKeepLogicalIdentityAndChangeRenderActivation) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto session = OpenIdentityEvents();
    VoiceLedger ledger(1, 1);
    VnVoiceHandle voice = 0;
    std::uint32_t slot = 0;
    const auto firstRender = UINT64_C(1) << 32;
    const auto nextRender = UINT64_C(2) << 32;
    ASSERT_TRUE(ledger.Acquire(&voice, &slot, {}, {}, harkentest::ObjectId(92001)));
    ledger.BindRender(slot, firstRender, 128);
    ASSERT_TRUE(ledger.Park(firstRender, &slot));
    ledger.ObserveVoice(MonitorLifecyclePhase::VoicePark, slot);
    ledger.ReturnPark(slot, nextRender);
    const auto rows = ReadIdentityEvents(session);
    ASSERT_EQ(3u, rows.size());
    EXPECT_EQ(firstRender, rows[0].fact.renderActivation);
    EXPECT_EQ(VN_MONITOR_FACT_VOICE_PARKED, rows[1].fact.factKind);
    EXPECT_FALSE(FactHas(rows[1].fact, VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION));
    EXPECT_EQ(nextRender, rows[2].fact.renderActivation);
    for (const auto& row : rows) {
        EXPECT_EQ(voice, row.header.instanceHandle);
        EXPECT_EQ(voice, row.fact.logicalVoice);
        EXPECT_EQ(VN_MONITOR_CONTENT_MEDIA, row.fact.contentKind);
        EXPECT_EQ(harkentest::ObjectId(92001), row.header.stableId);
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_ROOT));
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorExecutionLifecycle, QueueAndTransitionRetainOwnerContextAfterOwnerRetirement) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto capture = OpenIdentityEvents();
    const auto request = engine.monitorRequests.Begin(MonitorRequestOrigin::StudioPreview,
        VN_MONITOR_OPERATION_PLAY_PREVIEW);
    PreparedPlayableInstance root;
    ASSERT_TRUE(engine.playableInstances.PrepareAcquire(PlayableKind::Container, 0, &root, request));
    const MonitorContentIdentity content{harkentest::ObjectId(94001), harkentest::ObjectId(94002), 7, 17, 23};
    ASSERT_TRUE(engine.playableInstances.SetPreparedContentIdentity(root, content));
    ASSERT_TRUE(engine.playableInstances.CommitPrepared(&root));
    const auto identity = engine.CaptureMonitorOwner(root.Handle());
    QueueInstanceTable queues(1);
    QueueInstanceCreateRequest create{};
    create.projectId = content.projectId;
    create.world = content.worldGeneration;
    create.rootInstance = root.Handle();
    create.containerStableId = content.stableId;
    create.programSemanticFingerprint = content.programSemanticFingerprint;
    create.childCount = 2;
    create.initial.version = 1;
    create.initial.currentChildOrdinal = 3;
    create.monitorIdentity = identity;
    QueueInstanceToken queue;
    ASSERT_TRUE(queues.Acquire(create, &queue));
    auto next = create.initial;
    next.version = 2;
    next.currentChildOrdinal = 4;
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(root.Handle(), PlayableLifecycle::Retired));
    ASSERT_TRUE(queues.AdvanceAfterSignalCompletion(queue, 1, next));
    ASSERT_EQ(QueueFailureDisposition::NewlyFailed,
        queues.FailContainerBeforeConditional(queue, QueueFailureReason::Capacity, 64));
    TransitionInstancePool transitions(1);
    TransitionInstanceToken transition;
    ASSERT_TRUE(transitions.Acquire({64, 1, identity}, &transition));
    ASSERT_TRUE(transitions.CancelBeforePublication(transition));
    TransitionInstanceToken reused;
    ASSERT_TRUE(transitions.Acquire({128, 2}, &reused));
    EXPECT_NE(transition.generation, reused.generation);
    unsigned matched = 0;
    for (const auto& row : ReadIdentityEvents(capture)) {
        const bool queueFact = row.fact.subjectNamespace == VN_MONITOR_SUBJECT_QUEUE;
        const bool transitionFact = row.fact.subjectNamespace == VN_MONITOR_SUBJECT_TRANSITION &&
            row.header.instanceGeneration == transition.generation;
        if (!queueFact && !transitionFact) continue;
        ++matched;
        EXPECT_EQ(content.stableId, row.header.stableId);
        EXPECT_EQ(content.contentRevision, row.fact.contentRevision);
        EXPECT_EQ(root.Handle(), row.fact.root);
        EXPECT_EQ(root.Handle(), row.header.playableInstanceHandle);
        EXPECT_EQ(root.Handle(), row.fact.resourceOwner);
        EXPECT_EQ(identity.ownerEpoch, row.fact.ownerEpoch);
        EXPECT_EQ(request.key.sequence, row.fact.playbackRequest.sequence);
        EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
    }
    EXPECT_EQ(4u, matched);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(MonitorExecutionLifecycle, UncommittedVoiceReleaseDoesNotInventLiveOrEnd) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto capture = OpenIdentityEvents();
    VoiceLedger ledger(1, 1);
    VoiceLedger::PreparedPlayableIdentity identity;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.PreparePlayableOwned({0, UINT64_C(1) << 32}, &slot, &identity));
    ledger.Release(slot);
    EXPECT_TRUE(ReadIdentityEvents(capture).empty());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(MonitorExecutionLifecycle, PlaybackEndsDuringUpdateSurviveNotificationAndOrphanReclaim) {
    for (const bool orphan : {false, true}) {
        SCOPED_TRACE(orphan);
        struct CallbackState {
            Engine* engine = nullptr;
            bool orphan = false;
            bool armed = false;
            bool pumped = false;
            std::uint64_t firstFrame = UINT64_MAX;
            std::uint64_t endFrame = 0;
            void Pump(unsigned blocks) {
                for (unsigned block = 0; block < blocks; ++block) {
                    const auto startFrame = engine->mixer.BlocksRendered() * 64u;
                    engine->nullBackend.Pump(1);
                    const auto* output = engine->nullBackend.LastBlock();
                    for (unsigned frame = 0; frame < 64; ++frame) {
                        if (output[frame * 2u] <= 0.01f) continue;
                        firstFrame = std::min(firstFrame, startFrame + frame);
                        endFrame = startFrame + frame + 1u;
                    }
                }
            }
        } state{};
        state.orphan = orphan;
        auto config = harkentest::MakeNullConfig(4, 64, 48000);
        config.notificationUserData = &state;
        config.onNotification = [](const VnNotification*, void* context) {
            auto& current = *static_cast<CallbackState*>(context);
            if (!current.armed || current.pumped) return;
            current.pumped = true;
            current.Pump(128);
            if (current.orphan) {
                VnNotification discarded{};
                while (current.engine->notifications.TryPop(discarded)) {}
            }
        };
        harkentest::EngineSession engine(config);
        ASSERT_EQ(VN_OK, engine.InitResult());
        state.engine = engine.Engine();
        const auto sound = harkentest::ObjectId(93011);
        std::vector<float> samples(4096, 0.25f);
        const auto pcm = harkentest::MonoDesc(samples, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
        PlayablePropertyProgramViewV1 property{};
        property.contentPropertyKind = playableproperties::ContentPropertyKindV1::Sound;
        property.stableId = sound;
        property.contentRevision = state.engine->registry.Lookup(sound).contentRevision;
        property.routeStableId = VN_BUS_MASTER;
        property.compatibilityFingerprint = 1;
        property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
        property.payload.values[3].bits = 1;
        property.payload.values[6].objectId = VN_BUS_MASTER;
        property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
        property.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
        property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
        ContainerWorldInstallViewV1 world{};
        world.identity.projectId = harkentest::ObjectId(93012);
        world.identity.capabilityMask = kWorldCapabilityPlayableProperties;
        world.playableProperties = &property;
        world.playablePropertyCount = 1;
        world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
        ASSERT_EQ(WorldInstallStatus::Ok, InstallContainerWorldV1(*state.engine, world).status);
        const auto capture = OpenIdentityEvents();
        const auto started = StartCurrentPlayableByIdentity(PlayableKindV1::Sound, sound, 0);
        ASSERT_EQ(PlayableStartStatus::Accepted, started.status);
        for (unsigned block = 0; block < 64 && state.firstFrame == UINT64_MAX; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            state.Pump(1);
        }
        ASSERT_NE(UINT64_MAX, state.firstFrame);
        ASSERT_LT(state.endFrame - state.firstFrame, samples.size());
        state.armed = true;
        auto& notifications = orphan ? state.engine->logicNotifications : state.engine->notifications;
        notifications.Push(VnNotification{});
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_TRUE(state.pumped);
        ASSERT_NE(UINT64_MAX, state.firstFrame);
        EXPECT_EQ(samples.size(), state.endFrame - state.firstFrame);
        auto rows = ReadIdentityEvents(capture);
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) {
            return row.header.instanceHandle != started.instance ||
                (row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_STARTED &&
                 row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_FINISHED);
        }), rows.end());
        ASSERT_EQ(2u, rows.size());
        EXPECT_EQ(VN_MONITOR_FACT_PLAYABLE_STARTED, rows[0].fact.factKind);
        EXPECT_EQ(VN_MONITOR_FACT_PLAYABLE_FINISHED, rows[1].fact.factKind);
        EXPECT_TRUE(FactHas(rows[1].fact, VN_MONITOR_FACT_FIELD_EXECUTE_FRAME));
        EXPECT_TRUE(FactHas(rows[1].fact, VN_MONITOR_FACT_FIELD_AUDIO_BLOCK));
        EXPECT_EQ(state.firstFrame, rows[0].header.executeFrame);
        EXPECT_EQ(state.endFrame, rows[1].header.executeFrame);
        EXPECT_EQ((state.endFrame - 1u) / 64u + 1u, rows[1].fact.audioBlock);
        EXPECT_EQ(VN_MONITOR_TERMINAL_NATURAL_COMPLETION, rows[1].fact.terminalKind);
        EXPECT_LE(rows[0].fact.timestampNs, rows[1].fact.timestampNs);
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
    }
}

TEST(MonitorExecutionLifecycle, TwoHostsPlayingSameRandomKeepDistinctRequestsRootsAndVoices) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto sound = harkentest::ObjectId(93001);
    const auto container = harkentest::ObjectId(93002);
    const auto project = harkentest::ObjectId(93003);
    std::vector<float> pcmData(48000, 0.125f);
    const auto pcm = harkentest::MonoDesc(pcmData, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    const auto revision = engine.Engine()->registry.Lookup(sound).contentRevision;
    PlayablePropertyPayloadViewV1 base{};
    base.values[3].bits = 1;
    base.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    base.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    std::array<PlayablePropertyProgramViewV1, 2> properties{};
    for (unsigned i = 0; i != properties.size(); ++i) {
        auto& property = properties[i];
        property.contentPropertyKind = i == 0 ? playableproperties::ContentPropertyKindV1::Sound :
            playableproperties::ContentPropertyKindV1::Container;
        property.authoredStableOrder = i;
        property.stableId = i == 0 ? sound : container;
        property.contentRevision = i == 0 ? revision : 1;
        property.routeStableId = VN_BUS_MASTER;
        property.compatibilityFingerprint = i + 1;
        property.payload = base;
        property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
        property.payload.values[6].objectId = VN_BUS_MASTER;
        property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
    }
    ContainerChildProgramViewV2 child{};
    child.playable = {PlayableKindV1::Sound, 0, sound, properties[0].programSemanticFingerprint,
        revision, VN_BUS_MASTER, 1};
    child.stableChildOrdinal = 7;
    child.frequencyWeight = 1;
    PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind = playableproperties::ContentPropertyKindV1::Container;
    occurrence.parentStableId = container;
    occurrence.parentProgramSemanticFingerprint = properties[1].programSemanticFingerprint;
    occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
    occurrence.childPlayable = child.playable;
    occurrence.payload = base;
    occurrence.occurrenceSemanticFingerprint = ComputePlayableOccurrenceFingerprintV1(occurrence);
    ContainerProgramViewV3 program{};
    program.containerStableId = container;
    program.programSemanticFingerprint = properties[1].programSemanticFingerprint;
    program.mode = ContainerModeV1::Random;
    program.randomDiscipline = RandomDisciplineV1::Draw;
    program.memoryScope = MemoryScopeV1::Global;
    program.childCount = 1;
    program.children = &child;
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = project;
    world.identity.capabilityMask = kWorldCapabilityPlayableProperties | kWorldCapabilityRandomDraw;
    world.playableProperties = properties.data();
    world.playablePropertyCount = properties.size();
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.containers = &program;
    world.containerCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    ASSERT_EQ(WorldInstallStatus::Ok, InstallContainerWorldV1(*engine.Engine(), world).status);
    const auto host = BeginHost();
    std::array<VnEmitterHandle, 2> emitters{};
    for (unsigned i = 0; i != emitters.size(); ++i) {
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitters[i]));
        const auto registration = HostRegistration(host, emitters[i], i + 1);
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    }
    const auto session = OpenIdentityEvents();
    std::array<std::uint64_t, 2> requests{}, roots{}, voices{};
    for (unsigned i = 0; i != requests.size(); ++i) {
        const auto request = engine.Engine()->monitorRequests.Begin(
            MonitorRequestOrigin::NativeAPI, VN_MONITOR_OPERATION_PLAY_PREVIEW);
        const auto started = StartCurrentPlayableByIdentity(
            PlayableKindV1::Container, container, emitters[i], request);
        ASSERT_EQ(PlayableStartStatus::Accepted, started.status);
        requests[i] = request.key.sequence;
        roots[i] = started.instance;
    }
    const auto rows = ReadIdentityEvents(session);
    ASSERT_NE(0u, requests[0]);
    ASSERT_NE(0u, requests[1]);
    EXPECT_NE(requests[0], requests[1]);
    EXPECT_NE(roots[0], roots[1]);
    for (unsigned i = 0; i != requests.size(); ++i) {
        unsigned leaves = 0, liveVoices = 0;
        for (const auto& row : rows) {
            if (row.fact.playbackRequest.sequence != requests[i]) continue;
            EXPECT_EQ(emitters[i], row.fact.originEmitter);
            EXPECT_EQ(host.sdkSession, row.fact.hostSession);
            EXPECT_EQ(i + 1, row.fact.hostLocalSequence);
            EXPECT_EQ(roots[i], row.fact.root);
            if (row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_ACTIVE && row.fact.contentKind == VN_MONITOR_CONTENT_SOUND) {
                ++leaves;
                EXPECT_EQ(sound, row.header.stableId);
                EXPECT_EQ(revision, row.fact.contentRevision);
                EXPECT_EQ(roots[i], row.header.playableInstanceHandle);
            }
            if (row.fact.factKind == VN_MONITOR_FACT_VOICE_LIVE) {
                ++liveVoices;
                voices[i] = row.fact.logicalVoice;
                EXPECT_EQ(sound, row.header.stableId);
                EXPECT_EQ(revision, row.fact.contentRevision);
                EXPECT_EQ(project, row.fact.projectId);
                EXPECT_TRUE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_SERVICE_LEAF));
                EXPECT_TRUE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_RENDER_ACTIVATION));
                EXPECT_NE(roots[i], row.fact.serviceLeaf);
                if (FactHas(row.fact, VN_MONITOR_FACT_FIELD_RESOURCE_OWNER)) {
                    const auto& services = engine.Engine()->playableServices;
                    const auto owner = std::find_if(services.begin(), services.end(), [&](const auto& service) {
                        return service.ownerInstance == row.fact.resourceOwner &&
                            service.ownerEpoch == row.fact.ownerEpoch;
                    });
                    EXPECT_NE(services.end(), owner);
                    EXPECT_TRUE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_OWNER_EPOCH));
                } else {
                    EXPECT_FALSE(FactHas(row.fact, VN_MONITOR_FACT_FIELD_OWNER_EPOCH));
                }
            }
        }
        EXPECT_EQ(1u, leaves);
        EXPECT_EQ(1u, liveVoices);
    }
    EXPECT_NE(voices[0], voices[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

} // namespace

namespace {
TEST(MonitorIdentitySnapshot, CopiesCommittedSlotsAndDefaultEmitterWithoutPublishingHistory) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto events = OpenIdentityEvents();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto host = BeginHost();
    auto registration = HostRegistration(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    PreparedPlayableInstance root{}, child{}, hidden{};
    auto& table = engine.playableInstances;
    const auto origin = engine.monitorEmitters.CaptureOrigin(emitter);
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root, {}, origin));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root.Handle(), &child));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &hidden));
    const MonitorContentIdentity content{harkentest::ObjectId(3), harkentest::ObjectId(4), 5, 6, 7};
    ASSERT_TRUE(table.SetPreparedContentIdentity(child, content));
    ASSERT_TRUE(table.CommitPrepared(&root, false));
    ASSERT_TRUE(table.CommitPrepared(&child));
    ReadIdentityEvents(events);
    unsigned playableCount = 0, emitterCount = 0, defaultCount = 0;
    for (std::size_t slot = 0; slot != IdentitySlotCapacity(engine); ++slot) {
        MonitorExecutionFact fact{};
        if (!CaptureIdentitySlot(engine, slot, fact)) continue;
        EXPECT_EQ(VN_MONITOR_FACT_IDENTITY_UPSERT, fact.kind);
        EXPECT_EQ(0u, fact.sourceSequence);
        EXPECT_EQ(0u, fact.timestamp);
        EXPECT_FALSE(SourceHas(fact, VN_MONITOR_FACT_FIELD_REASON));
        fact.sourceSequence = slot + 1;
        std::array<std::uint8_t, kIdentityRecordBytes> bytes{};
        EXPECT_EQ(bytes.size(), PackIdentityFact(bytes.data(), bytes.size(), fact, 1, 1));
        const auto handle = fact.body.execution.identity.handle;
        if (fact.subjectNamespace == VN_MONITOR_SUBJECT_PLAYABLE) {
            ++playableCount;
            EXPECT_NE(hidden.Handle(), handle);
            if (handle == child.Handle()) {
                EXPECT_EQ(content.stableId, fact.body.execution.identity.object);
                EXPECT_EQ(root.Handle(), fact.body.execution.relations.parent);
                EXPECT_EQ(root.Handle(), fact.body.execution.relations.root);
            }
        } else if (fact.subjectNamespace == VN_MONITOR_SUBJECT_EMITTER) {
            ++emitterCount;
            if (handle == engine.emitters.DefaultState()) ++defaultCount;
            else {
                EXPECT_EQ(emitter, handle);
                EXPECT_EQ(host.sdkSession, fact.body.execution.emitters.hostSession);
            }
        }
    }
    EXPECT_EQ(2u, playableCount);
    EXPECT_EQ(2u, emitterCount);
    EXPECT_EQ(1u, defaultCount);
    EXPECT_TRUE(ReadIdentityEvents(events).empty());
    MonitorExecutionFact absent{};
    EXPECT_FALSE(CaptureIdentitySlot(engine, IdentitySlotCapacity(engine), absent));
    ASSERT_TRUE(table.CancelPrepared(&hidden));
    ASSERT_TRUE(table.AdvanceLifecycle(child.Handle(), PlayableLifecycle::Retired));
    MonitorLifecycleFacts values{};
    EXPECT_EQ(0u, table.CaptureMonitorIdentityAt(static_cast<std::uint32_t>(child.Handle()), &values));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
}

TEST(MonitorIdentitySnapshot, ActionChildrenRemainIdentifiableUntilNativeRelease) {
    ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));
    PreparedActionFiring root{}, child{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    ActionFiringSnapshot snapshot{};
    EXPECT_EQ(0u, table.CaptureMonitorIdentityAt(0, &snapshot));
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    ASSERT_TRUE(table.CommitPreparedChild(&child));
    EXPECT_EQ(child.Handle(), table.CaptureMonitorIdentityAt(1, &snapshot));
    EXPECT_EQ(root.Handle(), snapshot.parent);
    EXPECT_EQ(root.Handle(), snapshot.root);
    ActionFiringStepResult post{};
    post.childFiring = child.Handle();
    post.state = ActionStepState::Active;
    ASSERT_TRUE(table.UpdateStep(root.Handle(), 0, post));
    ASSERT_TRUE(table.MarkTerminal(child.Handle()));
    EXPECT_EQ(root.Handle(), table.CaptureMonitorIdentityAt(0, &snapshot));
    EXPECT_EQ(ActionFiringState::TerminalRetained, snapshot.state);
    EXPECT_EQ(child.Handle(), table.CaptureMonitorIdentityAt(1, &snapshot));
    EXPECT_EQ(ActionFiringState::TerminalRetained, snapshot.state);
    ASSERT_TRUE(table.ReleaseRoot(root.Handle()));
    EXPECT_EQ(0u, table.CaptureMonitorIdentityAt(1, &snapshot));
    EXPECT_EQ(0u, table.CaptureMonitorIdentityAt(0, &snapshot));
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(3), 8, 0, &root));
    ASSERT_TRUE(table.Commit(&root, 101));
    EXPECT_EQ(root.Handle(), table.CaptureMonitorIdentityAt(0, &snapshot));
    EXPECT_EQ(0u, snapshot.parent);
    EXPECT_EQ(harkentest::ObjectId(3), snapshot.actionId);
}

TEST(MonitorIdentitySnapshot, InternalAndParkedVoicesKeepLogicalIdentityWithoutRenderOwnership) {
    VoiceLedger ledger(2, 2);
    const VoiceLedger::PlayableParentRef parent{0, UINT64_C(1) << 32};
    std::uint32_t committed = 0, prepared = 0;
    ASSERT_TRUE(ledger.AcquirePlayableOwned(parent, &committed));
    ASSERT_TRUE(ledger.PreparePlayableOwned(parent, &prepared));
    MonitorLifecycleFacts values{};
    const auto logical = ledger.CaptureMonitorIdentityAt(committed, &values);
    ASSERT_NE(0u, logical);
    EXPECT_EQ(parent.internalToken, values.parentPlayable);
    EXPECT_EQ(0u, values.renderActivation);
    EXPECT_EQ(0u, ledger.CaptureMonitorIdentityAt(prepared, &values));
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(prepared));
    const auto render = UINT64_C(2) << 32;
    ledger.BindRender(committed, render, 128);
    ASSERT_TRUE(ledger.Park(render, &committed));
    EXPECT_EQ(logical, ledger.CaptureMonitorIdentityAt(committed, &values));
    EXPECT_EQ(0u, values.renderActivation);
    ledger.EndParked(committed);
    EXPECT_EQ(0u, ledger.CaptureMonitorIdentityAt(committed, &values));
}
} // namespace

namespace {
struct StateRecord {
    VnMonitorRecordHeader header{};
    VnMonitorIdentityFactV1 fact{};
    VnMonitorIdentityCutBeginV1 begin{};
    VnMonitorIdentityCutEndV1 end{};
    VnMonitorHostObjectSymbolV1 symbol{};
    VnMonitorDropSummary drop{};
};
VnMonitorSessionHandle OpenState(bool events = false, std::uint64_t queue = 65536) {
    StopDrainForTests();
    VnMonitorSessionDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.requestedExecutionFactVersion = 1;
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_STATE | (events ? VN_MONITOR_STREAM_EVENT : 0);
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_STATE | (events ? VN_MONITOR_PERM_OBSERVE_EVENTS : 0);
    desc.consumerQueueBytes = queue;
    VnMonitorSessionHandle result = 0;
    const auto* base = reinterpret_cast<const VnMonitorSessionDesc*>(&desc);
    EXPECT_EQ(VN_OK, events ? Vn_MonitorOpenEventSession(base, &result) : Vn_MonitorOpenSession(base, &result));
    return result;
}
std::vector<StateRecord> PollState(VnMonitorSessionHandle session) {
    std::array<std::uint8_t, 65536> bytes{};
    VnMonitorPollDesc desc{};
    desc.structSize = sizeof(desc);
    desc.buffer = bytes.data();
    desc.bufferBytes = bytes.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(session, &desc, &result));
    std::vector<StateRecord> records;
    for (std::uint64_t offset = 0; offset < result.bytesWritten;) {
        ExecutionPayloadReader length{bytes.data() + offset + 16};
        std::uint64_t size = 0;
        length(size);
        if (size < kPackedEnvelopeBytes || size > result.bytesWritten - offset) { ADD_FAILURE(); break; }
        StateRecord row{};
        if (!ReadIdentityHeader(bytes.data() + offset, size, row.header)) { ADD_FAILURE(); break; }
        const auto* payload = bytes.data() + offset + kPackedEnvelopeBytes;
        if (row.header.recordKind == VN_MONITOR_RECORD_IDENTITY_FACT)
            EXPECT_TRUE(DecodeIdentityFact(row.header, payload, row.header.payloadBytes, row.fact));
        else if (row.header.recordKind == VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN)
            EXPECT_TRUE(DecodeIdentityBegin(row.header, payload, row.header.payloadBytes, row.begin));
        else if (row.header.recordKind == VN_MONITOR_RECORD_IDENTITY_CUT_END)
            EXPECT_TRUE(DecodeIdentityEnd(row.header, payload, row.header.payloadBytes, row.end));
        else if (row.header.recordKind == VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL)
            EXPECT_TRUE(DecodeHostSymbol(row.header, payload, row.header.payloadBytes, row.symbol));
        else if (row.header.recordKind == VN_MONITOR_RECORD_DROP_SUMMARY) {
            EXPECT_GE(row.header.payloadBytes, sizeof(row.drop));
            if (row.header.payloadBytes >= sizeof(row.drop)) std::memcpy(&row.drop, payload, sizeof(row.drop));
        }
        records.push_back(row);
        offset += size;
    }
    return records;
}
VnMonitorSubscriptionStatusV2 StateStatus(VnMonitorSessionHandle session) {
    VnMonitorSubscriptionStatusV2 result{};
    result.structSize = sizeof(result);
    EXPECT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(session, reinterpret_cast<VnMonitorSubscriptionStatus*>(&result)));
    return result;
}
VnMonitorStateCutStatus CutStatus(VnMonitorSessionHandle session, std::uint64_t id) {
    VnMonitorStateCutStatus result{};
    result.structSize = sizeof(result);
    EXPECT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(session, id, &result));
    return result;
}
TEST(MonitorIdentityHub, StateOnlyLateOpenCapturesExistingHostAndDefaultWithoutInventingEvents) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto host = BeginHost();
    auto registration = HostRegistration(host, emitter, 42);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHost(&registration));
    EXPECT_EQ(0u, IdentityRefreshesForTests());
    const auto session = OpenState();
    EXPECT_FALSE(ExecutionObservationEnabled());
    EXPECT_EQ(VN_MONITOR_OBSERVATION_PENDING, StateStatus(session).baselineAvailability);
    DrainOnceForTests();
    const auto records = PollState(session);
    ASSERT_EQ(4u, records.size());
    MonitorIdentityCutVerifier verify;
    EXPECT_TRUE(verify.Begin(records.front().header, records.front().begin));
    bool found = false;
    for (std::size_t i = 1; i + 1 < records.size(); ++i) {
        EXPECT_TRUE(verify.Row(records[i].header, records[i].fact));
        if (records[i].header.instanceHandle == emitter) {
            found = true;
            EXPECT_TRUE(FactHas(records[i].fact.fact, VN_MONITOR_FACT_FIELD_HOST_KEY));
            EXPECT_EQ(42u, records[i].fact.fact.hostLocalSequence);
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(verify.End(records.back().header, records.back().end));
    EXPECT_EQ(VN_MONITOR_STATE_CUT_COMPLETE, CutStatus(session, records.front().begin.stateCutId).result);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(session).baselineAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
TEST(MonitorIdentityHub, FrozenRowsPrecedePostCutDeltaAndKeepDestroyedGeneration) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnEmitterHandle old = 0, replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&old));
    const auto first = OpenState();
    DrainOneIdentityForTests(); // Begin is sent, but rows still live in staging.
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(old));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    const auto second = OpenState();
    DrainOnceForTests();
    const auto records = PollState(first);
    ASSERT_EQ(6u, records.size());
    EXPECT_EQ(VN_MONITOR_RECORD_IDENTITY_CUT_END, records[3].header.recordKind);
    EXPECT_EQ(old, records[1].header.instanceHandle);
    EXPECT_EQ(0u, records[4].fact.stateCutId);
    EXPECT_EQ(VN_MONITOR_FACT_IDENTITY_TOMBSTONE, records[4].fact.fact.factKind);
    EXPECT_EQ(old, records[4].header.instanceHandle);
    EXPECT_EQ(replacement, records[5].header.instanceHandle);
    EXPECT_GT(records[5].fact.identityRevision, records[4].fact.identityRevision);
    const auto later = PollState(second);
    ASSERT_EQ(4u, later.size());
    EXPECT_NE(records[0].begin.stateCutId, later[0].begin.stateCutId);
    EXPECT_EQ(replacement, later[1].header.instanceHandle);
    EXPECT_EQ(VN_MONITOR_RECORD_IDENTITY_CUT_END, later.back().header.recordKind);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}
TEST(MonitorIdentityHub, MissingMiddleRowCannotCompleteEvenWhenBothControlsArrive) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto session = OpenState();
    DropIdentityRowForTests(0);
    DrainOnceForTests();
    const auto records = PollState(session);
    ASSERT_EQ(3u, records.size());
    EXPECT_EQ(VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, records.front().header.recordKind);
    EXPECT_EQ(VN_MONITOR_RECORD_IDENTITY_CUT_END, records.back().header.recordKind);
    EXPECT_EQ(VN_MONITOR_STATE_CUT_TRUNCATED, CutStatus(session, records.front().begin.stateCutId).result);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(session).baselineAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
TEST(MonitorIdentityHub, CallbackAndNestedMutationsDeferSnapshotUntilCompleteBoundary) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    auto* native = detail::ActiveEngine();
    ASSERT_NE(nullptr, native);
    VnMonitorSessionHandle session = 0;
    VnEmitterHandle emitter = 0;
    {
        IdentityMutationScope outer;
        native->callbackDispatchDepth = 1;
        session = OpenState();
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        VnMonitorStateCutDesc desc{};
        desc.structSize = sizeof(desc);
        std::uint64_t id = 0;
        EXPECT_EQ(VN_BACKPRESSURE, Vn_MonitorRequestStateCut(session, &desc, &id));
        EXPECT_EQ(0u, IdentityRefreshesForTests());
        native->callbackDispatchDepth = 0;
    }
    EXPECT_EQ(1u, IdentityRefreshesForTests());
    DrainOnceForTests();
    const auto records = PollState(session);
    ASSERT_EQ(4u, records.size());
    EXPECT_EQ(emitter, records[1].header.instanceHandle);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(session).baselineAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
TEST(MonitorIdentityHub, CapacityLimitPreservesTrueCountsAndReportsIncomplete) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    const auto session = OpenState();
    DrainOnceForTests();
    PollState(session);
    VnMonitorStateCutDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedCapacityHint = 1;
    std::uint64_t id = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(session, &desc, &id));
    DrainOnceForTests();
    const auto records = PollState(session);
    ASSERT_EQ(3u, records.size());
    EXPECT_EQ(2u, records.front().begin.namespaceTrueCount[VN_MONITOR_SUBJECT_EMITTER]);
    EXPECT_EQ(1u, records.back().end.namespaceEmittedCount[VN_MONITOR_SUBJECT_EMITTER]);
    EXPECT_EQ(VN_MONITOR_STATE_CUT_TRUNCATED, CutStatus(session, id).result);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}
} // namespace

namespace {
VnMonitorEventSessionStatusV2 EventState(VnMonitorSessionHandle session) {
    VnMonitorEventSessionStatusV2 result{};
    result.structSize = sizeof(result);
    EXPECT_EQ(VN_OK, Vn_MonitorGetEventSessionStatus(session, reinterpret_cast<VnMonitorEventSessionStatus*>(&result)));
    return result;
}
TEST(MonitorIdentityHub, SealWaitsForStateButEventOnlyDoesNotInheritItsFence) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto events = OpenIdentityEvents();
    const auto state = OpenState(true);
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(events));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(state));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(events).state);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, EventState(state).state);
    DrainOnceForTests();
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, EventState(state).state);
    EXPECT_EQ(3u, PollState(state).size());
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(state).state);
    EXPECT_TRUE(ReadIdentityEvents(events).empty());
    EXPECT_EQ(0u, EventState(events).sourceDroppedRecords);
    const auto refreshes = IdentityRefreshesForTests();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    EXPECT_EQ(refreshes, IdentityRefreshesForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
}
TEST(MonitorIdentityHub, SealInsideOuterUpdateDefersUntilFinalMutationAndCallbackSealIsRejected) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto state = OpenState(true);
    const auto events = OpenIdentityEvents();
    DrainOnceForTests();
    PollState(state);
    VnEmitterHandle emitter = 0;
    {
        IdentityMutationScope update;
        engine.Engine()->callbackDispatchDepth = 1;
        EXPECT_EQ(VN_BACKPRESSURE, Vn_MonitorSealEventSession(state));
        engine.Engine()->callbackDispatchDepth = 0;
        ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(state));
        EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, EventState(state).state);
        EXPECT_EQ(0u, EventState(state).stoppedTimestampNs);
        ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(events));
        EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(events).state);
        EXPECT_NE(0u, EventState(events).stoppedTimestampNs);
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    }
    EXPECT_NE(0u, EventState(state).stoppedTimestampNs);
    DrainOnceForTests();
    const auto records = PollState(state);
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [&](const auto& row) {
        return row.header.recordKind == VN_MONITOR_RECORD_IDENTITY_FACT &&
            row.header.instanceHandle == emitter && row.fact.stateCutId == 0;
    }));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(state).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
}
TEST(MonitorIdentityHub, SourceOverflowIsScopedToStateAndDoesNotChargeEventOnly) {
    auto config = harkentest::MakeNullConfig();
    config.maxEmitters = 1;
    config.maxActionFirings = 1;
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto events = OpenIdentityEvents();
    const auto state = OpenState();
    DrainOnceForTests();
    PollState(state);
    // Overflow the independent STATE ring through real logical mutations.
    for (std::uint32_t i = 0; i != IdentityRingCapacityForTests() / 2 + 2; ++i) {
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
        // Consume only EVENT, leaving STATE full without losing EVENT facts.
        DrainOneProducerForTests(false);
        PollState(events);
    }
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(events));
    DrainOnceForTests();
    EXPECT_EQ(0u, EventState(events).sourceDroppedRecords);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(events).state);
    std::uint32_t count = 0;
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL, Vn_MonitorGetConsumerStatus(state, nullptr, 0, &count));
    std::vector<VnMonitorDropSummary> drops(count);
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(state, drops.data(), count, &count));
    EXPECT_TRUE(std::any_of(drops.begin(), drops.end(), [](const auto& drop) {
        return drop.stage == VN_MONITOR_DROP_SOURCE_PRODUCER && drop.streamId == kIdentityDeltaStream && drop.recordCount != 0;
    }));
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(state).baselineAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
TEST(MonitorIdentityHub, DeliveryOverflowKeepsBaselineAndDeltaRangesInStatusAndSummaries) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnEmitterHandle first = 0, second = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&first));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&second));
    // Only Begin and one row fit. Neither End nor a drop summary can fit after them.
    const auto state = OpenState(false, 1250);
    DrainOnceForTests();
    for (unsigned i = 0; i != 2; ++i) {
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(emitter));
    }
    DrainOnceForTests();
    std::array<VnMonitorDropSummary, 10> drops{};
    std::uint32_t count = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(state, drops.data(), drops.size(), &count));
    ASSERT_EQ(2u, count);
    const auto records = PollState(state);
    ASSERT_EQ(2u, records.size());
    ASSERT_EQ(VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, records[0].header.recordKind);
    const auto& begin = records[0].begin;
    EXPECT_EQ(kIdentityDeltaStream, drops[0].streamId);
    EXPECT_EQ(begin.deltaSourceSequence + 1, drops[0].firstSourceSequence);
    EXPECT_EQ(begin.deltaSourceSequence + 4, drops[0].lastSourceSequence);
    EXPECT_EQ(4u, drops[0].recordCount);
    EXPECT_EQ(4 * kIdentityRecordBytes, drops[0].byteCount);
    EXPECT_EQ(kIdentityBaselineStream, drops[1].streamId);
    EXPECT_EQ(records[1].header.sourceSequence + 1, drops[1].firstSourceSequence);
    EXPECT_EQ(records[0].header.sourceSequence + 4, drops[1].lastSourceSequence);
    EXPECT_EQ(3u, drops[1].recordCount);
    EXPECT_EQ(2 * kIdentityRecordBytes + PackedRecordBytes(kIdentityEndBytes), drops[1].byteCount);
    for (unsigned i = 0; i != count; ++i) EXPECT_EQ(VN_MONITOR_DROP_HUB_FANOUT, drops[i].stage);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(state).baselineAvailability);
    EXPECT_EQ(VN_MONITOR_STATE_CUT_TRUNCATED, CutStatus(state, begin.stateCutId).result);

    // The next real mutation flushes both saved ranges before its own record.
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(second));
    DrainOnceForTests();
    const auto summaries = PollState(state);
    ASSERT_EQ(2u, summaries.size());
    for (unsigned i = 0; i != count; ++i) {
        EXPECT_EQ(VN_MONITOR_RECORD_DROP_SUMMARY, summaries[i].header.recordKind);
        EXPECT_EQ(VN_MONITOR_STREAM_STATE, summaries[i].header.streamKind);
        EXPECT_EQ(drops[i].streamId, summaries[i].drop.streamId);
        EXPECT_EQ(drops[i].firstSourceSequence, summaries[i].drop.firstSourceSequence);
        EXPECT_EQ(drops[i].lastSourceSequence, summaries[i].drop.lastSourceSequence);
        EXPECT_EQ(drops[i].recordCount, summaries[i].drop.recordCount);
        EXPECT_EQ(drops[i].byteCount, summaries[i].drop.byteCount);
        EXPECT_EQ(drops[i].stage, summaries[i].drop.stage);
    }
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(state, drops.data(), drops.size(), &count));
    ASSERT_EQ(1u, count);
    EXPECT_EQ(kIdentityDeltaStream, drops[0].streamId);
    EXPECT_EQ(begin.deltaSourceSequence + 5, drops[0].firstSourceSequence);
    EXPECT_EQ(drops[0].firstSourceSequence, drops[0].lastSourceSequence);
    EXPECT_EQ(1u, drops[0].recordCount);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
TEST(MonitorIdentityHub, OldWorldPlayableAndTerminalActionStayInBaselineUntilActualRecycle) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    PreparedPlayableInstance playable{};
    auto& table = engine.Engine()->playableInstances;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &playable));
    const MonitorContentIdentity content{harkentest::ObjectId(3), harkentest::ObjectId(4), 5, 6, 7};
    ASSERT_TRUE(table.SetPreparedContentIdentity(playable, content));
    ASSERT_TRUE(table.CommitPrepared(&playable, false));
    PreparedActionFiring firing{};
    auto& actions = engine.Engine()->actionFirings;
    ASSERT_TRUE(actions.PrepareRoot(harkentest::ObjectId(8), 5, 0, &firing));
    ASSERT_TRUE(actions.Commit(&firing, 19));
    ActionFiringSnapshot retained{};
    ASSERT_TRUE(actions.Capture(firing.Handle(), &retained));
    ASSERT_EQ(ActionFiringState::TerminalRetained, retained.state);
    SetCurrentWorldGenerationForTests(999);
    const auto state = OpenState();
    DrainOnceForTests();
    const auto records = PollState(state);
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [&](const auto& row) {
        return row.fact.fact.subjectNamespace == VN_MONITOR_SUBJECT_PLAYABLE &&
            row.header.instanceHandle == playable.Handle() && row.header.worldGeneration == 5;
    }));
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [&](const auto& row) {
        return row.fact.fact.subjectNamespace == VN_MONITOR_SUBJECT_ACTION && row.header.instanceHandle == firing.Handle();
    }));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing.Handle()));
    DrainOnceForTests();
    const auto delta = PollState(state);
    ASSERT_EQ(1u, delta.size());
    EXPECT_EQ(VN_MONITOR_FACT_IDENTITY_TOMBSTONE, delta[0].fact.fact.factKind);
    EXPECT_EQ(firing.Handle(), delta[0].header.instanceHandle);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
} // namespace

namespace {
TEST(MonitorIdentityHub, TypedStateNeverEntersLegacyInternalQueuesAndIgnoresEventExhaustion) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto events = OpenIdentityEvents();
    const auto state = OpenState();
    // Drain the ordinary subscription's pre-existing DSP capability diagnostic
    // before isolating the typed baseline delivery under test.
    DrainOnceForTests();
    PollState(state);
    EnableInternalConsumersForTests(7);
    VnMonitorStateCutDesc cut{};
    cut.structSize = sizeof(cut);
    std::uint64_t id = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(state, &cut, &id));
    DrainOnceForTests();
    for (unsigned i = 0; i != 3; ++i) EXPECT_EQ(0u, InternalQueuedCountForTests(i));
    MonitorExecutionFact foreign{};
    foreign.engine = ExecutionEngineInstance() + 1;
    foreign.kind = VN_MONITOR_FACT_REQUEST_SUBMITTED;
    foreign.flags = kMonitorFactCallBody;
    EXPECT_FALSE(PublishExecutionFact(foreign));
    EXPECT_EQ(1u, EventState(events).identityUnavailableCount);
    EXPECT_EQ(0u, StateStatus(state).identityUnavailableCount);
    EnableInternalConsumersForTests(0);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
} // namespace

namespace {
TEST(MonitorIdentityHub, FullPlayablePoolAndDeepParentsProduceCompleteBoundedCut) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    auto& table = engine.Engine()->playableInstances;
    const auto capacity = table.Capacity();
    std::uint64_t parent = 0, root = 0;
    for (std::uint32_t i = 0; i != capacity; ++i) {
        PreparedPlayableInstance child{};
        ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, parent, &child));
        ASSERT_TRUE(table.CommitPrepared(&child));
        parent = child.Handle();
        if (root == 0) root = parent;
    }
    const auto before = std::chrono::steady_clock::now();
    const auto state = OpenState(false, 1024 * 1024);
    const auto after = std::chrono::steady_clock::now();
    RecordProperty("playable_slots", static_cast<int>(capacity));
    RecordProperty("identity_slots", static_cast<int>(IdentitySlotCapacity(*engine.Engine())));
    RecordProperty("source_fact_bytes", static_cast<int>(sizeof(MonitorExecutionFact)));
    RecordProperty("open_and_freeze_us", std::to_string(
        std::chrono::duration<double, std::micro>(after - before).count()));
    DrainOnceForTests();
    std::vector<StateRecord> records;
    for (;;) {
        auto batch = PollState(state);
        if (batch.empty()) break;
        records.insert(records.end(), batch.begin(), batch.end());
    }
    ASSERT_EQ(capacity + 3, records.size());
    EXPECT_EQ(capacity, records.front().begin.namespaceTrueCount[VN_MONITOR_SUBJECT_PLAYABLE]);
    MonitorIdentityCutVerifier verify;
    ASSERT_TRUE(verify.Begin(records.front().header, records.front().begin));
    for (std::size_t i = 1; i + 1 < records.size(); ++i) {
        EXPECT_TRUE(verify.Row(records[i].header, records[i].fact));
        if (records[i].fact.fact.subjectNamespace == VN_MONITOR_SUBJECT_PLAYABLE)
            EXPECT_EQ(root, records[i].fact.fact.root);
    }
    EXPECT_TRUE(verify.End(records.back().header, records.back().end));
    std::array<double, 32> micros{};
    for (auto& sample : micros) {
        const auto start = std::chrono::steady_clock::now();
        RefreshIdentityBoundary();
        sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    std::sort(micros.begin(), micros.end());
    RecordProperty("unchanged_boundary_median_us", std::to_string(micros[16]));
    RecordProperty("unchanged_boundary_p95_us", std::to_string(micros[30]));
    DrainOnceForTests();
    EXPECT_TRUE(PollState(state).empty());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
} // namespace

namespace {
TEST(MonitorIdentityHub, ExhaustedDeltaMarksCurrentCutIncompleteAndStillDrains) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto events = OpenIdentityEvents();
    const auto state = OpenState(true);
    DrainOnceForTests();
    PollState(state);
    const auto cut = EventState(state).baselineStateCutId;
    ExhaustIdentitySourcesForTests(false);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    EXPECT_EQ(VN_MONITOR_STATE_CUT_TRUNCATED, CutStatus(state, cut).result);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, EventState(state).baselineAvailability);
    EXPECT_EQ(1u, EventState(state).identityUnavailableCount);
    EXPECT_EQ(0u, EventState(events).identityUnavailableCount);
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(state));
    DrainOnceForTests();
    PollState(state);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(state).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
TEST(MonitorIdentityHub, ExhaustedBaselineDoesNotLeaveAnUnfinishableSeal) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    StopDrainForTests();
    ExhaustIdentitySourcesForTests(true);
    const auto state = OpenState(true);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, EventState(state).baselineAvailability);
    EXPECT_EQ(0u, EventState(state).baselineStateCutId);
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(state));
    DrainOnceForTests();
    PollState(state);
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(state).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}
} // namespace

namespace {
VnMonitorSessionHandle OpenSymbols(std::uint32_t streams, bool permission = true,
    bool requested = true, std::uint64_t budget = 65536) {
    StopDrainForTests();
    VnMonitorSessionDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.requestedExecutionFactVersion = 1;
    desc.requestedStreamKindMask = streams;
    desc.requestSymbols = requested ? 1 : 0;
    desc.requestedPermissionMask = (permission ? VN_MONITOR_PERM_SYMBOLS : 0) |
        ((streams & VN_MONITOR_STREAM_STATE) ? VN_MONITOR_PERM_OBSERVE_STATE : 0) |
        ((streams & VN_MONITOR_STREAM_EVENT) ? VN_MONITOR_PERM_OBSERVE_EVENTS : 0);
    desc.consumerQueueBytes = budget;
    VnMonitorSessionHandle result = 0;
    auto* base = reinterpret_cast<const VnMonitorSessionDesc*>(&desc);
    EXPECT_EQ(VN_OK, (streams & VN_MONITOR_STREAM_EVENT) ? Vn_MonitorOpenEventSession(base, &result) :
        Vn_MonitorOpenSession(base, &result));
    return result;
}
std::vector<StateRecord> Symbols(const std::vector<StateRecord>& records) {
    std::vector<StateRecord> result;
    for (const auto& row : records)
        if (row.header.recordKind == VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL) result.push_back(row);
    return result;
}
std::vector<VnMonitorDropSummary> SymbolDrops(VnMonitorSessionHandle session) {
    std::array<VnMonitorDropSummary, 32> rows{};
    std::uint32_t count = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(session, rows.data(), rows.size(), &count));
    return {rows.begin(), rows.begin() + count};
}

TEST(MonitorHostSymbolHub, LateStateRestoresDescriptionsAndPermissionsSelectTheFrozenRows) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    EXPECT_EQ(0u, SymbolPublishedForTests());
    const auto events = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    EXPECT_EQ(0u, IdentityRefreshesForTests());
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE);
    const auto opaque = OpenSymbols(VN_MONITOR_STREAM_STATE, false);
    DrainOnceForTests();
    EXPECT_TRUE(Symbols(PollState(events)).empty());
    const auto baseline = PollState(state);
    const auto without = PollState(opaque);
    ASSERT_GE(baseline.size(), 5u);
    EXPECT_EQ(3u, baseline.front().begin.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL]);
    EXPECT_EQ(kHostSymbolProducer, baseline.front().begin.symbolProducerId);
    EXPECT_EQ(0u, without.front().begin.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL]);
    EXPECT_EQ(0u, without.front().begin.symbolProducerId);
    EXPECT_TRUE(Symbols(without).empty());
    MonitorIdentityCutVerifier verify;
    ASSERT_TRUE(verify.Begin(baseline.front().header, baseline.front().begin));
    for (std::size_t i = 1; i + 1 < baseline.size(); ++i) {
        const auto& row = baseline[i];
        if (row.header.recordKind == VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL) EXPECT_TRUE(verify.Row(row.header, row.symbol));
        else EXPECT_TRUE(verify.Row(row.header, row.fact));
    }
    EXPECT_TRUE(verify.End(baseline.back().header, baseline.back().end));
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(state).symbolAvailability);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_DENIED, StateStatus(opaque).symbolAvailability);
    registration.registrationRevision = 2;
    registration.name[0] = 'X';
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOnceForTests();
    const auto eventRows = Symbols(PollState(events));
    const auto stateRows = PollState(state);
    const auto stateSymbols = Symbols(stateRows);
    ASSERT_EQ(3u, eventRows.size());
    ASSERT_EQ(3u, stateSymbols.size());
    for (unsigned i = 0; i != 3; ++i) {
        EXPECT_EQ(eventRows[i].header.sourceSequence, stateSymbols[i].header.sourceSequence);
        EXPECT_EQ(VN_MONITOR_STREAM_EVENT, eventRows[i].header.streamKind);
        EXPECT_EQ(VN_MONITOR_STREAM_STATE, stateSymbols[i].header.streamKind);
        EXPECT_EQ(kHostSymbolStream, stateSymbols[i].header.streamId);
        EXPECT_EQ(2u, stateSymbols[i].symbol.registrationRevision);
        EXPECT_NE(0u, stateSymbols[i].header.monitorSessionEpoch);
    }
    for (const auto& row : stateRows)
        if (row.header.recordKind == VN_MONITOR_RECORD_IDENTITY_FACT)
            EXPECT_EQ(stateSymbols[0].symbol.cutRevision, row.fact.identityRevision);
    EXPECT_TRUE(Symbols(PollState(opaque)).empty());
    for (auto session : {events, state, opaque}) EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorHostSymbolHub, EventFirstOpenAndReopenSeedSilentlyAndRequestIsNotPermission) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    for (unsigned turn = 0; turn != 2; ++turn) {
        const auto event = OpenSymbols(VN_MONITOR_STREAM_EVENT);
        const auto noRequest = OpenSymbols(VN_MONITOR_STREAM_EVENT, true, false);
        DrainOnceForTests();
        EXPECT_TRUE(Symbols(PollState(event)).empty());
        ++registration.registrationRevision;
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
        DrainOnceForTests();
        EXPECT_EQ(3u, Symbols(PollState(event)).size());
        EXPECT_TRUE(Symbols(PollState(noRequest)).empty());
        EXPECT_EQ(0u, IdentityRefreshesForTests());
        for (auto session : {event, noRequest}) EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
        const auto before = SymbolPublishedForTests();
        ++registration.registrationRevision;
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
        EXPECT_EQ(before, SymbolPublishedForTests());
    }
}

TEST(MonitorHostSymbolHub, EventDeliveryProgressesDuringAnotherSessionsImmutableBaseline) {
    auto config = harkentest::MakeNullConfig();
    config.maxEmitters = 8;
    harkentest::EngineSession engine(config);
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto host = BeginHost();
    std::array<VnMonitorHostRegistrationV2, 8> registrations{};
    for (unsigned i = 0; i != registrations.size(); ++i) {
        VnEmitterHandle emitter = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
        registrations[i] = HostDescription(host, emitter);
        registrations[i].localSequence = i + 1;
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registrations[i]));
    }
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT);
    const auto events = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    registrations[0].registrationRevision = 2;
    registrations[0].name[0] = 'X';
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registrations[0]));
    for (unsigned i = 0; i != 3; ++i) DrainOneTypedForTests();
    EXPECT_EQ(3u, Symbols(PollState(events)).size());
    auto records = PollState(state);
    EXPECT_TRUE(Symbols(records).empty());
    ASSERT_EQ(3u * kHostSymbolRecordBytes, DeferredSymbolBytesForTests(state));
    EXPECT_EQ(DeferredSymbolBytesForTests(state), ConsumerUsedBytesForTests(state));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(state));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, EventState(state).state);
    DrainOnceForTests();
    auto tail = PollState(state);
    records.insert(records.end(), tail.begin(), tail.end());
    bool ended = false;
    unsigned frozen = 0, deltas = 0;
    std::uint64_t lastDelivery = 0;
    for (const auto& row : records) {
        EXPECT_GT(row.header.deliverySequence, lastDelivery);
        lastDelivery = row.header.deliverySequence;
        if (row.header.recordKind == VN_MONITOR_RECORD_IDENTITY_CUT_END) ended = true;
        if (row.header.recordKind != VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL) continue;
        if (row.header.streamId == kIdentityBaselineStream) {
            EXPECT_FALSE(ended);
            EXPECT_EQ(1u, row.symbol.registrationRevision);
            ++frozen;
        } else {
            EXPECT_TRUE(ended);
            EXPECT_EQ(2u, row.symbol.registrationRevision);
            ++deltas;
        }
    }
    EXPECT_EQ(24u, frozen);
    EXPECT_EQ(3u, deltas);
    EXPECT_EQ(0u, DeferredSymbolBytesForTests(state));
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(state).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
}

TEST(MonitorHostSymbolHub, SymbolBacklogCannotStarveEitherExecutionProducer) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto events = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    registration.registrationRevision = 2;
    registration.name[0] = 'X';
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOneProducerForTests(false);
    PollState(events);
    MonitorExecutionFact fact{};
    fact.engine = ExecutionEngineInstance();
    fact.kind = VN_MONITOR_FACT_REQUEST_SUBMITTED;
    fact.flags = kMonitorFactCallBody;
    fact.body.call = {};
    ASSERT_TRUE(PublishExecutionFact(fact));
    ASSERT_TRUE(PublishExecutionFact(fact, true));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(events));

    DrainRoundForTests();
    const auto first = PollState(events);
    EXPECT_EQ(1u, Symbols(first).size());
    for (const auto producer : {kExecutionLogicProducer, kExecutionAudioProducer}) {
        EXPECT_EQ(1u, std::count_if(first.begin(), first.end(), [&](const auto& row) {
            return row.header.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT &&
                row.header.producerId == producer;
        }));
    }
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_SEALED, EventState(events).state);
    DrainOnceForTests();
    EXPECT_EQ(2u, Symbols(PollState(events)).size());
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(events).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(events));
}

TEST(MonitorHostSymbolHub, DeferredRecordsKeepTheirBudgetAfterPollingAndLossIsStream205) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT, true, true, 1700);
    registration.registrationRevision = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOneTypedForTests();
    EXPECT_EQ(kHostSymbolRecordBytes, DeferredSymbolBytesForTests(state));
    PollState(state);
    EXPECT_EQ(kHostSymbolRecordBytes, ConsumerUsedBytesForTests(state));
    DrainOneTypedForTests();
    DrainOneTypedForTests();
    EXPECT_LE(ConsumerUsedBytesForTests(state), 1700u);
    bool symbolDrop = false;
    for (const auto& drop : SymbolDrops(state)) {
        if (drop.streamId != kHostSymbolStream) continue;
        symbolDrop = true;
        EXPECT_EQ(VN_MONITOR_DROP_HUB_FANOUT, drop.stage);
        EXPECT_EQ(2u, drop.recordCount);
        EXPECT_EQ(2u * kHostSymbolRecordBytes, drop.byteCount);
        EXPECT_EQ(2u, drop.firstSourceSequence);
        EXPECT_EQ(3u, drop.lastSourceSequence);
    }
    EXPECT_TRUE(symbolDrop);
    PollState(state);
    DrainOnceForTests();
    EXPECT_EQ(0u, DeferredSymbolBytesForTests(state));
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(state).symbolAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}

TEST(MonitorHostSymbolHub, MissingBaselineSymbolCharges204AndActualPackedBytes) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE);
    DropIdentityRowForTests(2);
    DrainOnceForTests();
    const auto records = PollState(state);
    EXPECT_EQ(2u, Symbols(records).size());
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(state).symbolAvailability);
    const auto drops = SymbolDrops(state);
    ASSERT_EQ(1u, drops.size());
    EXPECT_EQ(kIdentityBaselineStream, drops[0].streamId);
    EXPECT_EQ(VN_MONITOR_DROP_SOURCE_PRODUCER, drops[0].stage);
    EXPECT_EQ(kHostSymbolRecordBytes, drops[0].byteCount);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}

TEST(MonitorHostSymbolHub, RealSourceOverflowRetainsIndependentRangesAndDoesNotChargeOpaqueSessions) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto symbols = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    const auto opaque = OpenSymbols(VN_MONITOR_STREAM_EVENT, false);
    const auto capacity = SymbolRingCapacityForTests();
    ASSERT_EQ(8192u, capacity);
    const auto updates = capacity / 3 + 3;
    for (unsigned i = 0; i != updates; ++i) {
        ++registration.registrationRevision;
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    }
    EXPECT_EQ(3u * updates, SymbolPublishedForTests());
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(symbols));
    DrainOnceForTests();
    bool found = false;
    for (const auto& drop : SymbolDrops(symbols)) {
        if (drop.streamId != kHostSymbolStream || drop.stage != VN_MONITOR_DROP_SOURCE_PRODUCER) continue;
        found = true;
        EXPECT_EQ(capacity + 1, drop.firstSourceSequence);
        EXPECT_EQ(3u * updates, drop.lastSourceSequence);
        EXPECT_EQ(3u * updates - capacity, drop.recordCount);
        EXPECT_EQ((3u * updates - capacity) * kHostSymbolRecordBytes, drop.byteCount);
    }
    EXPECT_TRUE(found);
    for (const auto& drop : SymbolDrops(opaque)) EXPECT_NE(kHostSymbolStream, drop.streamId);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(symbols).symbolAvailability);
    for (auto session : {symbols, opaque}) EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorHostSymbolHub, SealedUnavailableWindowIgnoresLaterExhaustion) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto sealed = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    DrainOnceForTests();
    PollState(sealed);
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(sealed));
    DrainOnceForTests();
    PollState(sealed);
    const auto before = EventState(sealed).identityUnavailableCount;
    ExhaustSymbolSourcesForTests();
    const auto current = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    const auto opaque = OpenSymbols(VN_MONITOR_STREAM_EVENT, false);
    ++registration.registrationRevision;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOnceForTests();
    EXPECT_EQ(before, EventState(sealed).identityUnavailableCount);
    EXPECT_EQ(3u, EventState(current).identityUnavailableCount);
    EXPECT_EQ(0u, EventState(opaque).identityUnavailableCount);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, EventState(current).symbolAvailability);
    for (auto session : {sealed, current, opaque}) EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorHostSymbolHub, SharedRevisionExhaustionPreservesNoopAndReportsRealChanges) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE);
    const auto events = OpenSymbols(VN_MONITOR_STREAM_EVENT);
    DrainOnceForTests();
    PollState(state);
    PollState(events);
    const auto before = SymbolPublishedForTests();
    const auto unavailable = StateStatus(state).identityUnavailableCount;
    ExhaustObservationRevisionForTests();
    RefreshIdentityBoundary();
    DrainOnceForTests();
    EXPECT_EQ(before, SymbolPublishedForTests());
    EXPECT_EQ(unavailable, StateStatus(state).identityUnavailableCount);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(state).symbolAvailability);
    EXPECT_TRUE(PollState(state).empty());
    ++registration.registrationRevision;
    registration.name[0] = 'X';
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOnceForTests();
    EXPECT_EQ(before, SymbolPublishedForTests());
    EXPECT_TRUE(Symbols(PollState(state)).empty());
    EXPECT_TRUE(Symbols(PollState(events)).empty());
    EXPECT_GT(StateStatus(state).identityUnavailableCount, unavailable);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(state).symbolAvailability);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, EventState(events).symbolAvailability);
    for (auto session : {state, events}) EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorHostSymbolHub, CapacityHintCountsFieldsAndAnIncompleteGroupCannotComplete) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE);
    DrainOnceForTests();
    const auto initial = PollState(state);
    ASSERT_FALSE(initial.empty());
    std::uint32_t identities = 0;
    for (unsigned i = 0; i < VN_MONITOR_SUBJECT_HOST_SYMBOL; ++i)
        identities += static_cast<std::uint32_t>(initial.front().begin.namespaceTrueCount[i]);
    VnMonitorStateCutDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedCapacityHint = identities + 1;
    std::uint64_t cut = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(state, &desc, &cut));
    DrainOnceForTests();
    const auto limited = PollState(state);
    ASSERT_EQ(identities + 3u, limited.size());
    EXPECT_EQ(3u, limited.front().begin.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL]);
    EXPECT_EQ(1u, limited.back().end.namespaceEmittedCount[VN_MONITOR_SUBJECT_HOST_SYMBOL]);
    EXPECT_EQ(1u, Symbols(limited).size());
    EXPECT_EQ(VN_MONITOR_STATE_CUT_TRUNCATED, CutStatus(state, cut).result);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_INCOMPLETE, StateStatus(state).symbolAvailability);
    desc.requestedCapacityHint = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(state, &desc, &cut));
    DrainOnceForTests();
    EXPECT_EQ(3u, Symbols(PollState(state)).size());
    EXPECT_EQ(VN_MONITOR_STATE_CUT_COMPLETE, CutStatus(state, cut).result);
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(state).symbolAvailability);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}

TEST(MonitorHostSymbolHub, DeferredTailBlocksRecutAndCloseReopenIsolatesOldWindow) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto old = OpenSymbols(VN_MONITOR_STREAM_STATE);
    const auto epoch = StateStatus(old).monitorSessionEpoch;
    registration.registrationRevision = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOneTypedForTests();
    ASSERT_EQ(kHostSymbolRecordBytes, DeferredSymbolBytesForTests(old));
    // Complete only the frozen baseline while leaving its deferred tail queued.
    for (unsigned i = 0; i != 10; ++i) DrainOneIdentityForTests();
    EXPECT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(old).baselineAvailability);
    PollState(old);
    VnMonitorStateCutDesc desc{};
    desc.structSize = sizeof(desc);
    std::uint64_t cut = 0;
    EXPECT_EQ(VN_BACKPRESSURE, Vn_MonitorRequestStateCut(old, &desc, &cut));
    EXPECT_EQ(kHostSymbolRecordBytes, DeferredSymbolBytesForTests(old));
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(old));
    const auto current = OpenSymbols(VN_MONITOR_STREAM_STATE);
    EXPECT_NE(epoch, StateStatus(current).monitorSessionEpoch);
    registration.registrationRevision = 3;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOnceForTests();
    const auto rows = Symbols(PollState(current));
    ASSERT_EQ(6u, rows.size());
    for (unsigned i = 0; i != rows.size(); ++i) {
        EXPECT_EQ(i < 3 ? 2u : 3u, rows[i].symbol.registrationRevision);
        EXPECT_EQ(i < 3 ? kIdentityBaselineStream : kHostSymbolStream, rows[i].header.streamId);
        EXPECT_EQ(StateStatus(current).monitorSessionEpoch, rows[i].header.monitorSessionEpoch);
    }
    EXPECT_EQ(0u, DeferredSymbolBytesForTests(current));
    EXPECT_TRUE(SymbolDrops(current).empty());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(current));
}

TEST(MonitorHostSymbolHub, CallbackOpenAndOuterSealCaptureOnlyCommittedDescriptions) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    VnMonitorSessionHandle state = 0;
    {
        IdentityMutationScope outer;
        engine.Engine()->callbackDispatchDepth = 1;
        state = OpenSymbols(VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_EVENT);
        EXPECT_EQ(VN_BACKPRESSURE, Vn_MonitorSealEventSession(state));
        engine.Engine()->callbackDispatchDepth = 0;
        registration.registrationRevision = 2;
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
        ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(state));
        EXPECT_EQ(0u, EventState(state).stoppedTimestampNs);
        registration.registrationRevision = 3;
        registration.name[0] = 'Z';
        ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    }
    EXPECT_NE(0u, EventState(state).stoppedTimestampNs);
    DrainOnceForTests();
    const auto records = PollState(state);
    const auto symbols = Symbols(records);
    ASSERT_EQ(3u, symbols.size());
    for (const auto& row : symbols) {
        EXPECT_EQ(3u, row.symbol.registrationRevision);
        EXPECT_EQ(kIdentityBaselineStream, row.header.streamId);
        EXPECT_EQ(records.front().begin.identityRevision, row.symbol.cutRevision);
    }
    EXPECT_EQ(VN_MONITOR_EVENT_SESSION_DRAINED, EventState(state).state);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(state));
}

TEST(MonitorHostSymbolHub, DeliveryDropSummariesKeepEachSessionsStreamProjection) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const auto host = BeginHost();
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    auto registration = HostDescription(host, emitter);
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    const auto state = OpenSymbols(VN_MONITOR_STREAM_STATE, true, true, 1024);
    const auto events = OpenSymbols(VN_MONITOR_STREAM_EVENT, true, true, 1024);
    // Poll each baseline row to leave the deliberately small queues empty.
    for (unsigned i = 0; i != 12; ++i) {
        DrainOneTypedForTests();
        PollState(state);
        PollState(events);
    }
    ASSERT_EQ(VN_MONITOR_OBSERVATION_AVAILABLE, StateStatus(state).baselineAvailability);
    ++registration.registrationRevision;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    // Registration also publishes identity and binding facts. Consume those
    // first so this queue pressure isolates the three symbol field records.
    DrainOneIdentityForTests();
    DrainOneProducerForTests(false);
    PollState(state);
    PollState(events);
    DrainOnceForTests();
    for (auto session : {state, events}) {
        const auto drops = SymbolDrops(session);
        ASSERT_EQ(1u, drops.size());
        EXPECT_EQ(kHostSymbolStream, drops[0].streamId);
        EXPECT_EQ(1u, drops[0].recordCount);
        EXPECT_EQ(kHostSymbolRecordBytes, drops[0].byteCount);
        EXPECT_EQ(3u, drops[0].firstSourceSequence);
        const auto rows = PollState(session);
        EXPECT_EQ(1u, Symbols(rows).size());
        const auto flushed = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.header.recordKind == VN_MONITOR_RECORD_DROP_SUMMARY && row.drop.streamId == kHostSymbolStream;
        });
        ASSERT_NE(rows.end(), flushed);
        EXPECT_EQ(session == state ? VN_MONITOR_STREAM_STATE : VN_MONITOR_STREAM_EVENT, flushed->header.streamKind);
        EXPECT_EQ(1u, flushed->drop.recordCount);
        EXPECT_EQ(kHostSymbolRecordBytes, flushed->drop.byteCount);
        EXPECT_EQ(2u, flushed->drop.firstSourceSequence);
        EXPECT_EQ(2u, flushed->drop.lastSourceSequence);
    }
    ++registration.registrationRevision;
    ASSERT_EQ(VN_OK, Vn_MonitorRegisterHostV2(&registration));
    DrainOnceForTests();
    for (auto session : {state, events}) {
        const auto rows = PollState(session);
        const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.header.recordKind == VN_MONITOR_RECORD_DROP_SUMMARY && row.drop.streamId == kHostSymbolStream;
        });
        ASSERT_NE(rows.end(), found);
        EXPECT_EQ(session == state ? VN_MONITOR_STREAM_STATE : VN_MONITOR_STREAM_EVENT, found->header.streamKind);
        EXPECT_EQ(VN_MONITOR_DROP_HUB_FANOUT, found->drop.stage);
        EXPECT_EQ(1u, found->drop.recordCount);
        EXPECT_EQ(3u, found->drop.firstSourceSequence);
        EXPECT_EQ(3u, found->drop.lastSourceSequence);
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    }
}
} // namespace
