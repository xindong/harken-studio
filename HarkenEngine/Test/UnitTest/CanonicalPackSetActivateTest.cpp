#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "PackLogicStore.h"
#include "TraceFormat.h"
#include "harken/dsp/GainRamp.h"
#include "harken/param/EffectParamLocator.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif
#ifndef HARKEN_HOST_PATH
#error "HARKEN_HOST_PATH must point at the harken_host executable"
#endif

namespace {

using harkentest::ActivateInitPathV1;
using harkentest::ActivateInitPathWithBus2V1;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::PackAdmissionOpenForInit;
using harkentest::PackMasterHolder;

TEST(CanonicalPackSetActivate, PathInitOnlyActivateRetryUnload) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-activate";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const auto first = ActivateInitPathV1(directory);
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_NE(0u, first.initHandle);
    EXPECT_EQ(1u, first.report.resultSetRevision);
    EXPECT_EQ(1u, first.report.memberCount);
    EXPECT_TRUE(PackAdmissionOpenForInit(session.Engine(), first.initHandle));
    ASSERT_NE(nullptr, PackMasterHolder(session.Engine()));
    EXPECT_EQ(0u, session.Engine()->masterClaim.packHeld);
    EXPECT_EQ(0u, session.Engine()->masterClaim.previewHeld);
    EXPECT_EQ(0u, session.Engine()->masterClaim.directHeld);

    const auto retry = ActivateInitPathV1(directory);
    ASSERT_EQ(VN_OK, retry.result);
    EXPECT_EQ(first.initHandle, retry.initHandle);
    EXPECT_EQ(first.report.resultSetRevision, retry.report.resultSetRevision);
    ASSERT_NE(nullptr, PackMasterHolder(session.Engine()));

    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = first.initHandle;
    unload.baseSetRevision = first.report.resultSetRevision;
    unload.unloadRevision = first.report.resultSetRevision + 1;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
    EXPECT_EQ(0u, session.Engine()->masterClaim.packHeld);
    EXPECT_EQ(0u, session.Engine()->masterClaim.fingerprint);
    EXPECT_TRUE(session.Engine()->packSetTombstone.hasTombstone);
    EXPECT_FALSE(PackAdmissionOpenForInit(session.Engine(), first.initHandle));
    EXPECT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));

    const auto afterUnload = ActivateInitPathV1(directory);
    ASSERT_EQ(VN_OK, afterUnload.result);
    EXPECT_NE(0u, afterUnload.initHandle);
    EXPECT_FALSE(session.Engine()->packSetTombstone.hasTombstone);
    ASSERT_NE(nullptr, PackMasterHolder(session.Engine()));
}

TEST(CanonicalPackSetActivate, CommandQueueRefusalReturnsPreparedTopologyPlan) {
    VnConfig config = MakeNullConfig();
    config.commandQueueDepth = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto acquired = [&]() {
        std::uint32_t count = 0;
        for (std::uint32_t index = 0; index < engine->topologyPlans.Capacity(); ++index) {
            if (engine->topologyPlans.IsAcquired(index)) ++count;
        }
        return count;
    };
    const auto before = acquired();
    ASSERT_EQ(0u, before);
    for (std::uint32_t index = 0; index < engine->commands.Capacity(); ++index) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    }
    const auto accepted = engine->commands.PublishedHead();
    const auto consumed = engine->commands.ConsumedCount();
    ASSERT_EQ(engine->commands.Capacity(), accepted - consumed);
    const auto failed = ActivateInitPathV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-command-pressure");
    ASSERT_EQ(VN_BACKPRESSURE, failed.result);
    EXPECT_EQ(before, acquired());
    EXPECT_EQ(accepted, engine->commands.PublishedHead());
    EXPECT_EQ(consumed, engine->commands.ConsumedCount());
    EXPECT_EQ(nullptr, engine->packComposition.Published());
    EXPECT_EQ(nullptr, PackMasterHolder(engine));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto retried = ActivateInitPathV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-command-pressure");
    ASSERT_EQ(VN_OK, retried.result);
    EXPECT_EQ(before + 1u, acquired());
}

TEST(CanonicalPackSetActivate, PreviewDifferentFingerprintBlocksPack) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-activate-preview";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_FALSE(session.Engine()->previewMasterHolders.empty());
    auto& holder = session.Engine()->previewMasterHolders.front();
    holder.occupied = true;
    holder.ownerId = 0xFEED0001ull;
    holder.fingerprint = 0xFEEDF00Dull;
    holder.revision = 1;
    const auto first = ActivateInitPathV1(directory);
    EXPECT_EQ(VN_IN_USE, first.result);
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
    holder = {};
}

TEST(CanonicalPackSetActivate, MasterBus2WritesPackCookieAndUnloads) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-activate-bus2";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto first = ActivateInitPathWithBus2V1(directory);
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_NE(0u, first.initHandle);
    const std::uint32_t bus2 =
        session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2));
    ASSERT_NE(harken::kInvalidBusIndex, bus2);
    EXPECT_NE(0u, bus2);
    EXPECT_EQ(first.initHandle, session.Engine()->busOwnerCookies[bus2]);
    EXPECT_EQ(0u, session.Engine()->busTopologyOwnerIds[bus2]);
    EXPECT_EQ(0u, session.Engine()->busTopologyOwnerIds[0]);

    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = first.initHandle;
    unload.baseSetRevision = first.report.resultSetRevision;
    unload.unloadRevision = first.report.resultSetRevision + 1;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2)));
    EXPECT_TRUE(session.Engine()->buses.AliveAt(0));
}

TEST(CanonicalPackSetActivate, LaterMissingMemberPreservesEmptyState) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "late-member";
    const std::string initPath = (directory / "Init.pack").string();
    const std::string missingPath = (directory / "missing-content.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(initPath, harkentest::BuildMasterOnlyInitBytes()));
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = UINT64_MAX - 1;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = missingPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_NE(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(), report));
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
    EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
}

TEST(CanonicalPackSetActivate, MasterClashPreservesEmptyPackState) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_FALSE(session.Engine()->previewMasterHolders.empty());
    auto& holder = session.Engine()->previewMasterHolders.front();
    holder.occupied = true;
    holder.ownerId = 0xFEED0001ull;
    holder.fingerprint = 0xFEEDF00Dull;
    holder.revision = 1;
    const auto result = harkentest::ActivateInitPathV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "master-clash");
    EXPECT_EQ(VN_IN_USE, result.result);
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
    EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
    holder = {};
}

TEST(CanonicalPackSetActivate, InitReplaceKeepsHandleAndSingleSlot) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto first = harkentest::ActivateInitPathV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "replace");
    ASSERT_EQ(VN_OK, first.result);
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report, first.initHandle, 1));
    EXPECT_EQ(first.initHandle, report.initHandle);
    EXPECT_EQ(first.initHandle, row.handle);
    EXPECT_EQ(1u, session.Engine()->packs.LoadedCount());
}

TEST(CanonicalPackSetActivate, UnloadWithForeignChildPreservesGraph) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto first = harkentest::ActivateInitPathWithBus2V1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "foreign-child");
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_EQ(VN_OK, Vn_CreateBus(VN_OBJECT_ID(0, 3), VN_OBJECT_ID(0, 2)));
    const auto bus2 = session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2));
    const auto bus3 = session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 3));
    const auto previous = session.Engine()->packComposition.PublishedHandle();
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = first.initHandle;
    unload.baseSetRevision = 1;
    unload.unloadRevision = 2;
    EXPECT_EQ(VN_IN_USE, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(bus2, session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2)));
    EXPECT_EQ(bus2, session.Engine()->buses.ParentOf(bus3));
    EXPECT_EQ(previous, session.Engine()->packComposition.PublishedHandle());
    EXPECT_FALSE(session.Engine()->packSetTombstone.hasTombstone);
}

TEST(CanonicalPackSetActivate, UnloadRetryIncludesBaseRevision) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto first = harkentest::ActivateInitPathV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "unload-retry");
    ASSERT_EQ(VN_OK, first.result);
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = first.initHandle;
    unload.baseSetRevision = 1;
    unload.unloadRevision = 2;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    unload.baseSetRevision = 0;
    EXPECT_EQ(VN_REVISION_CONFLICT, Vn_UnloadCanonicalPackSetV1(&unload));
}

struct ReentrantSetProvider {
    std::vector<std::uint8_t> bytes = harkentest::BuildMasterOnlyInitBytes();
    VnAttenuationDesc profile = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    VnPCMDesc pcm{};
    VnVoiceHandle voice = 0;
    std::uint32_t acquired = 0, released = 0;
    bool invalidSpan = false;
    bool failAcquire = false;

    void Mutate() {
        EXPECT_EQ(VN_IN_USE, Vn_Init(nullptr));
        EXPECT_EQ(VN_IN_USE, Vn_Term());
        EXPECT_EQ(VN_IN_USE, Vn_Update());
        EXPECT_EQ(VN_IN_USE, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
        EXPECT_EQ(VN_IN_USE, Vn_UnregisterPCM(harkentest::ObjectId(1)));
        EXPECT_EQ(VN_IN_USE, Vn_ReplacePCM(harkentest::ObjectId(1), &pcm));
        EXPECT_EQ(VN_IN_USE, Vn_RegisterAttenuation(harkentest::ObjectId(18), &profile));
        EXPECT_EQ(VN_IN_USE, Vn_ReplaceAttenuation(harkentest::ObjectId(17), &profile));
        EXPECT_EQ(VN_IN_USE, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(17)));
        EXPECT_EQ(VN_IN_USE, Vn_CreateBus(harkentest::ObjectId(444), VN_BUS_MASTER));
        EXPECT_EQ(VN_IN_USE, Vn_DestroyBus(harkentest::ObjectId(50)));
        EXPECT_EQ(VN_IN_USE, Vn_ReparentBus(harkentest::ObjectId(50), VN_BUS_MASTER));
        EXPECT_EQ(VN_IN_USE, Vn_ConnectBusSend(harkentest::ObjectId(50), VN_BUS_MASTER, 0.0f));
        EXPECT_EQ(VN_IN_USE, Vn_DisconnectBusSend(harkentest::ObjectId(50), VN_BUS_MASTER));
        EXPECT_EQ(VN_IN_USE, Vn_SetBusOutputLayout(VN_BUS_MASTER, VN_CHANNEL_LAYOUT_MONO));
        EXPECT_EQ(VN_IN_USE, Vn_SetBusVolume(harkentest::ObjectId(50), -9.0f, 0));
        EXPECT_EQ(VN_IN_USE, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
        EXPECT_EQ(VN_IN_USE, Vn_Stop(voice, 0));
        EXPECT_EQ(VN_IN_USE, Vn_MonitorOpenSession(nullptr, nullptr));
        VnEmitterHandle emitter = 0;
        EXPECT_EQ(VN_IN_USE, Vn_CreateEmitter(&emitter));
        EXPECT_EQ(0u, emitter);
        auto& engine = *harken::detail::ActiveEngine();
        harken::ProjectGenerationInstallViewV1 view;
        harken::PreparedProjectGenerationV1 prepared;
        EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::Backpressure,
                  harken::PrepareProjectGenerationV1(engine, view, prepared).status);
        EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure,
                  harken::CommitPreparedProjectGenerationV1(engine, prepared).status);
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(snapshot);
        EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    }

    static VnResult Acquire(void* context, std::uint64_t, const char*,
                            std::uint32_t, std::uint64_t, VnPackByteSpan* span) {
        auto& self = *static_cast<ReentrantSetProvider*>(context);
        ++self.acquired;
        self.Mutate();
        if (self.failAcquire) { return VN_DEVICE_ERROR; }
        span->structSize = sizeof(*span);
        span->bytes = self.invalidSpan ? nullptr : self.bytes.data();
        span->byteCount = self.invalidSpan ? 0 : self.bytes.size();
        return VN_OK;
    }

    static void Release(void* context, std::uint64_t, VnPackByteSpan) {
        auto& self = *static_cast<ReentrantSetProvider*>(context);
        ++self.released;
        self.Mutate();
    }
};

TEST(CanonicalPackSetActivate, ProviderAcquireAndReleaseRejectAllMutationFamilies) {
    for (int mode = 0; mode != 3; ++mode) {
        SCOPED_TRACE(mode);
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ReentrantSetProvider provider;
        provider.invalidSpan = mode == 1;
        provider.failAcquire = mode == 2;
        const auto samples = harkentest::MakeDC(48000, 0.5f);
        provider.pcm = harkentest::MonoDesc(samples, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &provider.pcm));
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(17), &provider.profile));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(50), VN_BUS_MASTER));
        auto play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &provider.voice));
        const auto cookie = engine.registry.CookieOf(harkentest::ObjectId(1));
        VnCanonicalPackSetMemberV1 member{};
        member.structSize = sizeof(member);
        member.packStableId = harkentest::packfoundation::kInitPackStableId;
        member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
        member.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
        member.providerUserData = &provider;
        member.provide = ReentrantSetProvider::Acquire;
        member.release = ReentrantSetProvider::Release;
        VnCanonicalPackSetMemberResultV1 row{};
        row.structSize = sizeof(row);
        VnCanonicalPackSetActivationReportV1 report{};
        EXPECT_EQ(mode == 0 ? VN_OK : VN_PACK_PROVIDER_FAILED,
                  harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
        EXPECT_EQ(1u, provider.acquired);
        EXPECT_EQ(mode == 2 ? 0u : 1u, provider.released);
        EXPECT_EQ(cookie, engine.registry.CookieOf(harkentest::ObjectId(1)));
        EXPECT_EQ(0u, engine.registry.CookieOf(harkentest::ObjectId(2)));
        EXPECT_TRUE(engine.spatial.HasProfile(harkentest::ObjectId(17)));
        EXPECT_FALSE(engine.spatial.HasProfile(harkentest::ObjectId(18)));
        EXPECT_NE(harken::kInvalidBusIndex, engine.buses.FindIndex(harkentest::ObjectId(50)));
        EXPECT_EQ(harken::kInvalidBusIndex, engine.buses.FindIndex(harkentest::ObjectId(444)));
        EXPECT_NE(0u, harkentest::RenderHandle(&engine, provider.voice));
        EXPECT_FALSE(engine.canonicalPackSetBusy);
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(18), &provider.profile));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(444), VN_BUS_MASTER));
    }
}

TEST(CanonicalPackSetActivate, SameRevisionWrongExpectedHandleIsNotExactRetry) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto first = ActivateInitPathV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "retry-wrong-handle");
    ASSERT_EQ(VN_OK, first.result);
    const auto prior = session.Engine()->packComposition.PublishedHandle();
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_NE(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report, first.initHandle + 1));
    member.expectedHandle = first.initHandle + 1;
    EXPECT_NE(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    EXPECT_EQ(prior, session.Engine()->packComposition.PublishedHandle());
    EXPECT_EQ(1u, session.Engine()->packs.LoadedCount());
    member.expectedHandle = 0;
    EXPECT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    EXPECT_EQ(first.initHandle, row.handle);
}

TEST(CanonicalPackSetActivate, LoadPackRefusesCurrentVintageInit) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-loadpack-init";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto written = ActivateInitPathV1(directory);
    ASSERT_EQ(VN_OK, written.result);
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = written.initHandle;
    unload.baseSetRevision = written.report.resultSetRevision;
    unload.unloadRevision = written.report.resultSetRevision + 1;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));

    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.utf8Path = written.initPath.c_str();
    VnPackHandle handle = 0;
    const VnResult loaded =
        Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&load), &handle);
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, loaded);
    EXPECT_EQ(0u, handle);

    VnPackDesc vintage{};
    vintage.structSize = sizeof(vintage);
    vintage.utf8Path = written.initPath.c_str();
    VnPackHandle vintageHandle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, Vn_LoadPack(&vintage, &vintageHandle));
    EXPECT_EQ(0u, vintageHandle);
}

TEST(CanonicalPackSetActivate, LoadPackRefusesInitWithoutKind24Snapshot) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    using namespace harkentest::packfoundation;
    ImageBuilder builder(harken::kPackKindInit);
    std::uint64_t bodyBytes = 0;
    auto partition = PartitionPayload(kInitPackStableId, 1, 0, 0, &bodyBytes);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
                     RequirementPayload(kInitPackStableId, 1, 1, 0, bodyBytes));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition,
                     kInitPackStableId, std::move(partition));
    auto bytes = builder.Build();
    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.flags = VN_PACK_LOAD_FLAG_PROVIDER;
    load.providerUserData = &bytes;
    load.provide = [](void* data, std::uint64_t, const char*, std::uint32_t,
                      std::uint64_t, VnPackByteSpan* out) -> VnResult {
        const auto& owned = *static_cast<const std::vector<std::uint8_t>*>(data);
        out->structSize = sizeof(*out);
        out->bytes = owned.data();
        out->byteCount = owned.size();
        return VN_OK;
    };
    VnPackHandle handle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&load), &handle));
    EXPECT_EQ(0u, handle);
}

TEST(CanonicalPackSetActivate, ProductionV1LoadsAndInspectsButCannotActivate) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    using namespace harkentest::packfoundation;
    harken::PackRequirementBlock requirement;
    requirement.schemaVersion = 1;
    requirement.storageModelVersion = 1;
    requirement.packStableId = kInitPackStableId;
    requirement.packContentRevision = 1;
    std::vector<std::uint8_t> payload(harken::kPackRequirementBlockBytes, 0);
    harken::SerializePackRequirementBlock(requirement, payload.data());
    harken::StoreLE64(payload.data() + 104, harken::Fnv1a64(payload.data(), 104));
    ImageBuilder builder(harken::kPackKindInit);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
                     std::move(payload), harken::kPackCanonicalEntryVintage);
    const auto activation = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-production-v1",
        builder.Build(), kInitPackStableId);
    EXPECT_EQ(activation.result, VN_PACK_INCOMPATIBLE);
    EXPECT_EQ(activation.initHandle, 0u);
    EXPECT_EQ(session.Engine()->packs.LoadedCount(), 0u);
    EXPECT_EQ(PackMasterHolder(session.Engine()), nullptr);

    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.utf8Path = activation.initPath.c_str();
    VnInitPackInspectReport report{};
    report.structSize = sizeof(report);
    EXPECT_EQ(VN_OK, Vn_InspectInitPack(&load, &report));
    EXPECT_EQ(report.packStableId, kInitPackStableId);
    VnPackHandle handle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&load), &handle));
    EXPECT_EQ(handle, 0u);
}

TEST(CanonicalPackSetActivate, InspectInitWithoutKind24SnapshotIsBadPack) {
    using namespace harkentest::packfoundation;
    ImageBuilder builder(harken::kPackKindInit);
    std::uint64_t bodyBytes = 0;
    auto partition = PartitionPayload(kInitPackStableId, 1, 0, 0, &bodyBytes);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
                     RequirementPayload(kInitPackStableId, 1, 1, 0, bodyBytes));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition,
                     kInitPackStableId, std::move(partition));
    auto bytes = builder.Build();
    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.flags = VN_PACK_LOAD_FLAG_PROVIDER;
    load.providerUserData = &bytes;
    load.provide = [](void* data, std::uint64_t, const char*, std::uint32_t,
                      std::uint64_t, VnPackByteSpan* out) -> VnResult {
        const auto& owned = *static_cast<const std::vector<std::uint8_t>*>(data);
        out->structSize = sizeof(*out);
        out->bytes = owned.data();
        out->byteCount = owned.size();
        return VN_OK;
    };
    VnInitPackInspectReport report{};
    report.structSize = sizeof(report);
    EXPECT_EQ(VN_BAD_PACK, Vn_InspectInitPack(&load, &report));
    EXPECT_EQ(VN_BAD_PACK, report.result);
}

TEST(CanonicalPackSetActivate, ActivateInitWithoutKind24SnapshotIsBadPack) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    using namespace harkentest::packfoundation;
    ImageBuilder builder(harken::kPackKindInit);
    std::uint64_t bodyBytes = 0;
    auto partition = PartitionPayload(kInitPackStableId, 1, 0, 0, &bodyBytes);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
                     RequirementPayload(kInitPackStableId, 1, 1, 0, bodyBytes));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition,
                     kInitPackStableId, std::move(partition));
    const auto result = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-no-kind24",
        builder.Build(), kInitPackStableId);
    EXPECT_EQ(VN_BAD_PACK, result.result);
    EXPECT_EQ(0u, result.initHandle);
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
}

TEST(CanonicalPackSetActivate, LoadPackRefusesLiveActivatedInit) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-loadpack-live-init";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto written = ActivateInitPathV1(directory);
    ASSERT_EQ(VN_OK, written.result);
    ASSERT_NE(0u, written.initHandle);

    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.utf8Path = written.initPath.c_str();
    VnPackHandle handle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&load), &handle));
    EXPECT_EQ(0u, handle);
}

TEST(CanonicalPackSetActivate, PendingVoiceRecipePinsPackAttenuationGeneration) {
    constexpr std::uint32_t kBlock = 256;
    constexpr std::uint32_t kRate = 48000;
    VnConfig config = MakeNullConfig(1, kBlock, kRate);
    config.voiceStealPolicy = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(kRate, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, kRate);
    const VnSoundId sound = harkentest::ObjectId(1);
    const VnAttenuationId atten = harkentest::ObjectId(55);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));

    const VnAttenuationDesc firstProfile =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    const harken::Spatializer::PackProfileInput firstInput{atten, &firstProfile};
    harken::Spatializer::PreparedProfileSet prepared;
    ASSERT_EQ(VN_OK, engine->spatial.PreparePackProfiles(
                         &firstInput, 1, 1, &prepared));
    engine->spatial.CommitPreparedSet(&prepared);
    ASSERT_EQ(1u, engine->spatial.CurrentProfileGeneration(atten));

    constexpr VnPackHandle kRecipeHandle = 1;
    harken::PackLogicStore::PreparedClaimToken token;
    ASSERT_EQ(VN_OK, engine->logicPacks.PrepareClaim(kRecipeHandle, &token));
    harken::PackLogicRecord content;
    harken::PackLogicImage::VoiceRecipe recipe;
    recipe.header.playableId = sound;
    recipe.spatial.attenuationId = atten;
    recipe.spatial.distanceScale = 1.0f;
    content.voiceRecipes.push_back(std::move(recipe));
    engine->logicPacks.CommitPreparedClaim(&token, std::move(content));
    ASSERT_NE(nullptr, engine->logicPacks.FindVoiceRecipe(sound));

    VnVoiceHandle incumbent = 0;
    VnPlayDesc play = harkentest::MakePlayDesc(sound);
    play.priority = 128;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &incumbent));
    engine->nullBackend.Pump(1);

    VnVoiceHandle incoming = 0;
    VnPlayDesc play3d =
        harkentest::MakePlayDesc3D(sound, 0.0f, 0.0f, 0.0f, 0.0f);
    play3d.priority = 128;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play3d, &incoming));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(incoming).kind);
    const std::uint32_t slot = engine->ledger.Classify(incoming).ledgerSlot;
    const harken::PendingStartRecord& rec = engine->pendingStarts[slot];
    EXPECT_TRUE(rec.hasAttenuation);
    EXPECT_EQ(atten, rec.attenuationId);
    EXPECT_EQ(1u, rec.attenuationGeneration);
    EXPECT_TRUE(rec.hasDistanceScale);

    const VnAttenuationDesc nextProfile =
        harkentest::MakeAttenuationDesc(1.0f, 200.0f);
    const harken::Spatializer::PackProfileInput nextInput{atten, &nextProfile};
    harken::Spatializer::PreparedProfileSet replaced;
    const std::vector<harken::Spatializer::RetainedProfileKey> retained{
        {atten, rec.attenuationGeneration}};
    ASSERT_EQ(VN_OK, engine->spatial.PreparePackProfiles(
                         &nextInput, 1, 2, &replaced, retained));
    engine->spatial.CommitPreparedSet(&replaced);
    EXPECT_NE(nullptr, engine->spatial.FindProfile(atten, 1));
    EXPECT_NE(nullptr, engine->spatial.FindProfile(atten, 2));
    EXPECT_EQ(2u, engine->spatial.CurrentProfileGeneration(atten));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    harken::Spatializer::SpatialVoiceState spatial{};
    ASSERT_TRUE(engine->spatial.CaptureVoice(
        harkentest::RenderHandle(engine, incoming), &spatial));
    EXPECT_EQ(atten, spatial.attenuationId);
    EXPECT_EQ(1u, spatial.attenuationGeneration);
    EXPECT_EQ(VN_OK, engine->spatial.BindVoice(
                         harkentest::RenderHandle(engine, incoming), atten, 1));
}

TEST(CanonicalPackSetActivate, ContentWrongInitTripleIsStaleRevision) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-stale-triple";
    const auto bus = harkentest::packfoundation::DefaultRuntimeBusFixtureV1();
    auto wrong = bus.triple;
    ++wrong.busContentGeneration;
    std::uint64_t bodyBytes = 0;
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::BuildMasterOnlyInitBytes(
            harkentest::packfoundation::kInitPackStableId, bus)));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::packfoundation::BuildContentWithInitDependency(
            harkentest::packfoundation::kResidentPackStableId, 1,
            harkentest::packfoundation::kInitPackStableId, 1, wrong, &bodyBytes)));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = harkentest::packfoundation::kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_PACK_STALE_REVISION,
              harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                    report));
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
}

TEST(CanonicalPackSetActivate, SnapshotSendInstallsProspectiveEdge) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float kLevelDb = -6.0f;
    const auto first = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-send",
        harkentest::BuildMasterSendInitBytes(kLevelDb),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    auto* engine = session.Engine();
    const auto source = engine->buses.FindIndex(VN_OBJECT_ID(0, 2));
    const auto master = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, source);
    ASSERT_EQ(0u, master);
    float expected = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(kLevelDb, &expected));
    bool found = false;
    for (std::uint32_t planIndex = 0; planIndex < engine->topologyPlans.Capacity();
         ++planIndex) {
        if (!engine->topologyPlans.IsAcquired(planIndex)) {
            continue;
        }
        const auto plan = engine->topologyPlans.View(planIndex);
        if (!plan.valid || plan.sendTargets == nullptr ||
            plan.preparedSendLevels == nullptr || plan.sendLevelApply == nullptr) {
            continue;
        }
        for (std::uint32_t slot = 0; slot < plan.slotsPerBus; ++slot) {
            const auto flat =
                static_cast<std::size_t>(source) * plan.slotsPerBus + slot;
            if (plan.sendTargets[flat] != master || plan.sendLevelApply[flat] == 0) {
                continue;
            }
            EXPECT_NEAR(expected, plan.preparedSendLevels[flat], 1.0e-5f);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(CanonicalPackSetActivate, SnapshotSendPreservesForeignLiveSend) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto foreignSource = VN_OBJECT_ID(0, 80);
    const auto foreignTarget = VN_OBJECT_ID(0, 81);
    ASSERT_EQ(VN_OK, Vn_CreateBus(foreignSource, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(foreignTarget, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(foreignSource, foreignTarget, -9.0f));
    auto* engine = session.Engine();
    const auto sourceSlot = engine->buses.FindIndex(foreignSource);
    const auto targetSlot = engine->buses.FindIndex(foreignTarget);
    ASSERT_NE(harken::kInvalidBusIndex, sourceSlot);
    ASSERT_NE(harken::kInvalidBusIndex, targetSlot);
    const auto sendSlots = engine->config.busSendSlotsPerBus;
    std::uint32_t foreignSlot = sendSlots;
    for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
        if (engine->busSendAdj[static_cast<std::size_t>(sourceSlot) * sendSlots + slot] ==
            targetSlot) foreignSlot = slot;
    }
    ASSERT_LT(foreignSlot, sendSlots);
    const auto foreignFlat = static_cast<std::size_t>(sourceSlot) * sendSlots + foreignSlot;
    const auto first = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-foreign-send",
        harkentest::BuildMasterSendInitBytes(-6.0f),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    EXPECT_EQ(targetSlot, engine->busSendAdj[foreignFlat]);
    EXPECT_FLOAT_EQ(-9.0f, engine->busSendLevelDb[foreignFlat]);
    EXPECT_EQ(0u, engine->busSendOwnerCookies[foreignFlat]);
    const auto packSource = engine->buses.FindIndex(VN_OBJECT_ID(0, 2));
    ASSERT_NE(harken::kInvalidBusIndex, packSource);
    bool foundPackSend = false;
    for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
        const auto flat = static_cast<std::size_t>(packSource) * sendSlots + slot;
        if (engine->busSendAdj[flat] != 0) continue;
        EXPECT_EQ(first.initHandle, engine->busSendOwnerCookies[flat]);
        EXPECT_FLOAT_EQ(-6.0f, engine->busSendLevelDb[flat]);
        EXPECT_EQ(VN_IN_USE, Vn_SetBusSendLevel(VN_OBJECT_ID(0, 2),
            VN_BUS_MASTER, -12.0f, 0));
        EXPECT_EQ(VN_IN_USE, Vn_DisconnectBusSend(VN_OBJECT_ID(0, 2),
            VN_BUS_MASTER));
        foundPackSend = true;
    }
    EXPECT_TRUE(foundPackSend);
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = first.initHandle;
    unload.baseSetRevision = first.report.resultSetRevision;
    unload.unloadRevision = first.report.resultSetRevision + 1;
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(targetSlot, engine->busSendAdj[foreignFlat]);
    EXPECT_FLOAT_EQ(-9.0f, engine->busSendLevelDb[foreignFlat]);
}

TEST(CanonicalPackSetActivate, ForeignSendCycleRefusesReplacementAndAllowsRetry) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-foreign-cycle";
    const auto first = harkentest::ActivateInitPathWithBus2V1(directory);
    ASSERT_EQ(VN_OK, first.result);
    const auto foreignBus = VN_OBJECT_ID(0, 80);
    const auto packBus = VN_OBJECT_ID(0, 2);
    ASSERT_EQ(VN_OK, Vn_CreateBus(foreignBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(foreignBus, packBus, -9.0f));
    auto* engine = session.Engine();
    const auto foreignSlot = engine->buses.FindIndex(foreignBus);
    const auto packSlot = engine->buses.FindIndex(packBus);
    ASSERT_NE(harken::kInvalidBusIndex, foreignSlot);
    ASSERT_NE(harken::kInvalidBusIndex, packSlot);
    const auto sendSlots = engine->config.busSendSlotsPerBus;
    std::uint32_t edgeSlot = sendSlots;
    for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
        if (engine->busSendAdj[static_cast<std::size_t>(foreignSlot) * sendSlots + slot] ==
            packSlot) edgeSlot = slot;
    }
    ASSERT_LT(edgeSlot, sendSlots);
    const auto foreignFlat = static_cast<std::size_t>(foreignSlot) * sendSlots + edgeSlot;
    const auto beforePublication = engine->packComposition.PublishedHandle();
    const auto beforeRevision = engine->acceptedBusTopologyRevision;
    const auto candidatePath = (directory / "replacement.pack").string();
    const auto activate = [&](VnBusId sendTarget,
                              VnCanonicalPackSetActivationReportV1* report) -> VnResult {
        const auto fixture = harkentest::packfoundation::MasterSendRuntimeBusFixtureV1(
            -6.0f, 1, sendTarget);
        if (!harkentest::WriteBytes(candidatePath,
            harkentest::BuildMasterOnlyInitBytes(
                harkentest::packfoundation::kInitPackStableId, fixture)))
            return VN_DEVICE_ERROR;
        VnCanonicalPackSetMemberV1 member{};
        member.structSize = sizeof(member);
        member.packStableId = harkentest::packfoundation::kInitPackStableId;
        member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
        member.expectedHandle = first.initHandle;
        member.utf8Path = candidatePath.c_str();
        VnCanonicalPackSetMemberResultV1 row{};
        row.structSize = sizeof(row);
        return harkentest::ActivateCanonicalMembersV1(
            &member, 1, &row, *report, first.initHandle,
            first.report.resultSetRevision);
    };
    VnCanonicalPackSetActivationReportV1 refused{};
    EXPECT_EQ(VN_BAD_PACK, activate(foreignBus, &refused));
    EXPECT_EQ(beforePublication, engine->packComposition.PublishedHandle());
    EXPECT_EQ(beforeRevision, engine->acceptedBusTopologyRevision);
    EXPECT_EQ(packSlot, engine->busSendAdj[foreignFlat]);
    EXPECT_FLOAT_EQ(-9.0f, engine->busSendLevelDb[foreignFlat]);
    EXPECT_EQ(0u, engine->busSendOwnerCookies[foreignFlat]);
    VnCanonicalPackSetActivationReportV1 accepted{};
    ASSERT_EQ(VN_OK, activate(VN_BUS_MASTER, &accepted));
    EXPECT_EQ(first.report.resultSetRevision + 1, accepted.resultSetRevision);
    EXPECT_EQ(packSlot, engine->busSendAdj[foreignFlat]);
    EXPECT_FLOAT_EQ(-9.0f, engine->busSendLevelDb[foreignFlat]);
    EXPECT_EQ(0u, engine->busSendOwnerCookies[foreignFlat]);
}

TEST(CanonicalPackSetActivate, ForeignSendConsumesPackSourceCapacityWithoutMutation) {
    auto config = harkentest::MakeNullConfig();
    config.busSendSlotsPerBus = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-foreign-capacity";
    const auto first = harkentest::ActivateInitPathWithBus2V1(directory);
    ASSERT_EQ(VN_OK, first.result);
    const auto foreignBus = VN_OBJECT_ID(0, 80);
    const auto packBus = VN_OBJECT_ID(0, 2);
    ASSERT_EQ(VN_OK, Vn_CreateBus(foreignBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(packBus, foreignBus, -9.0f));
    auto* engine = session.Engine();
    const auto packSlot = engine->buses.FindIndex(packBus);
    const auto foreignSlot = engine->buses.FindIndex(foreignBus);
    ASSERT_NE(harken::kInvalidBusIndex, packSlot);
    ASSERT_NE(harken::kInvalidBusIndex, foreignSlot);
    const auto foreignFlat = static_cast<std::size_t>(packSlot);
    const auto beforePublication = engine->packComposition.PublishedHandle();
    const auto beforeRevision = engine->acceptedBusTopologyRevision;
    const auto candidatePath = (directory / "replacement.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(candidatePath,
        harkentest::BuildMasterSendInitBytes(-6.0f)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = candidatePath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_NO_BUS, harkentest::ActivateCanonicalMembersV1(
        &member, 1, &row, report, first.initHandle,
        first.report.resultSetRevision));
    EXPECT_EQ(beforePublication, engine->packComposition.PublishedHandle());
    EXPECT_EQ(beforeRevision, engine->acceptedBusTopologyRevision);
    EXPECT_EQ(foreignSlot, engine->busSendAdj[foreignFlat]);
    EXPECT_FLOAT_EQ(-9.0f, engine->busSendLevelDb[foreignFlat]);
    EXPECT_EQ(0u, engine->busSendOwnerCookies[foreignFlat]);
    ASSERT_EQ(VN_OK, Vn_DisconnectBusSend(packBus, foreignBus));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        &member, 1, &row, report, first.initHandle,
        first.report.resultSetRevision));
    EXPECT_EQ(0u, engine->busSendAdj[foreignFlat]);
    EXPECT_EQ(report.initHandle, engine->busSendOwnerCookies[foreignFlat]);
}

TEST(CanonicalPackSetActivate, DuplicateSnapshotSendIsBadPack) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto first = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-dup-send",
        harkentest::BuildMasterSendInitBytes(-6.0f,
                                            harkentest::packfoundation::kInitPackStableId,
                                            2),
        harkentest::packfoundation::kInitPackStableId);
    EXPECT_EQ(VN_BAD_PACK, first.result);
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
}

TEST(CanonicalPackSetActivate, ExactRetryIncludesProviderIdentity) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    struct Provider {
        std::vector<std::uint8_t> bytes = harkentest::BuildMasterOnlyInitBytes();
        static VnResult Acquire(void* context, std::uint64_t, const char*,
                                std::uint32_t, std::uint64_t, VnPackByteSpan* span) {
            auto& self = *static_cast<Provider*>(context);
            span->structSize = sizeof(*span);
            span->bytes = self.bytes.data();
            span->byteCount = self.bytes.size();
            return VN_OK;
        }
        static void Release(void*, std::uint64_t, VnPackByteSpan) {}
    };
    Provider firstProvider;
    Provider secondProvider;
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
    member.providerUserData = &firstProvider;
    member.provide = Provider::Acquire;
    member.release = Provider::Release;
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    const auto firstHandle = report.initHandle;
    ASSERT_NE(0u, firstHandle);
    member.providerUserData = &secondProvider;
    EXPECT_NE(VN_OK,
              harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    ASSERT_NE(nullptr, session.Engine()->packComposition.Published());
    EXPECT_EQ(firstHandle, session.Engine()->packComposition.Published()->initHandle);
    member.providerUserData = &firstProvider;
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    EXPECT_EQ(firstHandle, report.initHandle);
}


std::uint32_t ReadTraceU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint64_t ReadTraceU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool SkipTraceFileRef(const std::vector<std::uint8_t>& bytes, std::size_t* offset) {
    if (*offset + sizeof(std::uint32_t) > bytes.size()) {
        return false;
    }
    const std::uint32_t len = ReadTraceU32(bytes, *offset);
    *offset += sizeof(std::uint32_t);
    if (len != harken::kTraceStrNull) {
        if (*offset + len > bytes.size()) {
            return false;
        }
        *offset += len;
    }
    if (*offset + sizeof(std::uint64_t) + 1 > bytes.size()) {
        return false;
    }
    *offset += sizeof(std::uint64_t) + 1;
    return true;
}

std::uint32_t CountTraceCall(const std::vector<std::uint8_t>& bytes, std::uint16_t want) {
    if (bytes.size() < harken::kTraceFixedHeaderBytes) {
        return 0;
    }
    std::size_t offset = ReadTraceU32(bytes, 8);
    std::uint32_t count = 0;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        const std::uint32_t payloadBytes = ReadTraceU32(bytes, offset);
        const std::uint16_t type = static_cast<std::uint16_t>(
            bytes[offset + 4] | (static_cast<std::uint16_t>(bytes[offset + 5]) << 8));
        offset += harken::kTraceRecordHeaderBytes;
        if (offset + payloadBytes > bytes.size()) {
            break;
        }
        if (type == harken::kTraceRecordApiCall && payloadBytes >= 2) {
            const std::uint16_t callId = static_cast<std::uint16_t>(
                bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
            if (callId == want) {
                ++count;
            }
        }
        offset += payloadBytes;
    }
    return count;
}

bool FindActivate135Tail(const std::vector<std::uint8_t>& bytes, std::uint64_t* initHandle,
                         std::uint64_t* outputHandle, std::uint32_t* result,
                         std::uint32_t* providerTag, std::uint32_t* pathPresent) {
    if (bytes.size() < harken::kTraceFixedHeaderBytes) {
        return false;
    }
    std::size_t offset = ReadTraceU32(bytes, 8);
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        const std::uint32_t payloadBytes = ReadTraceU32(bytes, offset);
        const std::uint16_t type = static_cast<std::uint16_t>(
            bytes[offset + 4] | (static_cast<std::uint16_t>(bytes[offset + 5]) << 8));
        const std::size_t payload = offset + harken::kTraceRecordHeaderBytes;
        if (payload + payloadBytes > bytes.size()) {
            return false;
        }
        const std::size_t recordEnd = payload + payloadBytes;
        offset = recordEnd;
        if (type != harken::kTraceRecordApiCall || payloadBytes < 4) {
            continue;
        }
        const std::uint16_t callId = static_cast<std::uint16_t>(
            bytes[payload] | (static_cast<std::uint16_t>(bytes[payload + 1]) << 8));
        if (callId != harken::kTraceCallActivateCanonicalPackSet) {
            continue;
        }
        std::size_t cursor = payload + 4;
        const auto pull32 = [&](std::uint32_t* out) {
            if (cursor + sizeof(std::uint32_t) > recordEnd) {
                return false;
            }
            *out = ReadTraceU32(bytes, cursor);
            cursor += sizeof(std::uint32_t);
            return true;
        };
        const auto pull64 = [&](std::uint64_t* out) {
            if (cursor + sizeof(std::uint64_t) > recordEnd) {
                return false;
            }
            *out = ReadTraceU64(bytes, cursor);
            cursor += sizeof(std::uint64_t);
            return true;
        };
        const auto skip32 = [&](std::uint32_t count) {
            const std::size_t bytesNeeded = sizeof(std::uint32_t) * static_cast<std::size_t>(count);
            if (cursor + bytesNeeded > recordEnd) {
                return false;
            }
            cursor += bytesNeeded;
            return true;
        };
        const auto skip64 = [&](std::uint32_t count) {
            const std::size_t bytesNeeded = sizeof(std::uint64_t) * static_cast<std::size_t>(count);
            if (cursor + bytesNeeded > recordEnd) {
                return false;
            }
            cursor += bytesNeeded;
            return true;
        };
        std::uint32_t vintage = 0;
        if (!pull32(&vintage) || vintage != 1u || !skip32(2) || !skip64(3)) {
            return false;
        }
        std::uint32_t memberCount = 0;
        if (!pull32(&memberCount) || memberCount == 0 || !skip32(1)) {
            return false;
        }
        for (std::uint32_t i = 0; i < memberCount; ++i) {
            std::uint32_t tag = 0;
            std::uint32_t present = 0;
            if (!skip32(2) || !skip64(2) || !skip32(2) || !pull32(&tag) || tag > 2u ||
                !pull32(&present) || present > 1u) {
                return false;
            }
            if (i == 0) {
                *providerTag = tag;
                *pathPresent = present;
            }
            if (!SkipTraceFileRef(bytes, &cursor) || cursor > recordEnd) {
                return false;
            }
        }
        std::uint32_t replayClass = 0;
        std::uint32_t outputCount = 0;
        if (!pull32(result) || !pull32(&replayClass) || replayClass != 0u || !skip32(2) ||
            !pull64(initHandle) || !skip64(6) || !pull32(&outputCount) ||
            outputCount != memberCount || !skip32(1)) {
            return false;
        }
        for (std::uint32_t i = 0; i < outputCount; ++i) {
            std::uint64_t handle = 0;
            if (!skip32(1) || !skip64(1) || !pull64(&handle)) {
                return false;
            }
            if (i == 0) {
                *outputHandle = handle;
            }
        }
        return cursor == recordEnd;
    }
    return false;
}

TEST(CanonicalPackSetActivate, TraceActiveActivateNoLongerReturnsCapabilityUnavailable) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-trace-active";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string tracePath =
        (std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set.vtrc").string();
    std::filesystem::create_directories(std::filesystem::path(HARKEN_TEMP_DIR));
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    const auto activated = ActivateInitPathV1(directory);
    EXPECT_EQ(VN_OK, activated.result);
    EXPECT_NE(0u, activated.initHandle);
    EXPECT_NE(nullptr, PackMasterHolder(session.Engine()));
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = 1;
    unload.baseSetRevision = 1;
    unload.unloadRevision = 2;
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_UnloadCanonicalPackSetV1(&unload));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
}

TEST(CanonicalPackSetActivate, TraceStartActivateWritesCall135) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-activate-journal";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::filesystem::path tracePath =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-activate-journal.vtrc";
    std::filesystem::create_directories(tracePath.parent_path());
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.string().c_str()));
    const auto activated = ActivateInitPathV1(directory);
    EXPECT_EQ(VN_OK, activated.result);
    EXPECT_NE(0u, activated.initHandle);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    std::ifstream in(tracePath, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    EXPECT_EQ(1u, CountTraceCall(bytes, harken::kTraceCallActivateCanonicalPackSet));
    std::uint64_t initHandle = 0;
    std::uint64_t outputHandle = 0;
    std::uint32_t result = 0;
    std::uint32_t providerTag = 99;
    std::uint32_t pathPresent = 99;
    ASSERT_TRUE(FindActivate135Tail(bytes, &initHandle, &outputHandle, &result,
                                    &providerTag, &pathPresent));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), result);
    EXPECT_EQ(0u, providerTag);
    EXPECT_EQ(1u, pathPresent);
    EXPECT_EQ(activated.initHandle, initHandle);
    EXPECT_EQ(activated.initHandle, outputHandle);
    EXPECT_NE(0u, outputHandle);
}

TEST(CanonicalPackSetActivate, HostReplaysSuccessfulActivateCall135) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-activate-host-replay";
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::filesystem::path tracePath =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-activate-host-replay.vtrc";
    std::filesystem::create_directories(tracePath.parent_path());
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.string().c_str()));
    const auto activated = ActivateInitPathV1(directory);
    EXPECT_EQ(VN_OK, activated.result);
    EXPECT_NE(0u, activated.initHandle);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    std::string command = std::string("\"") + HARKEN_HOST_PATH +
        "\" trace-replay \"" + tracePath.string() + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
    FILE* pipe = _popen(command.c_str(), "rt");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    ASSERT_NE(nullptr, pipe);
    std::string output;
    char chunk[512];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr)
        output += chunk;
#ifdef _WIN32
    const int exitCode = _pclose(pipe);
#else
    const int exitCode = pclose(pipe);
#endif
    EXPECT_EQ(0, exitCode) << output;
    EXPECT_EQ(std::string::npos, output.find("unknown call id 135")) << output;
}

TEST(CanonicalPackSetActivate, HostKeepsPublishedAfterBadPackActivate) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-att03-keep";
    std::uint64_t bodyBytes = 0;
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack", harkentest::BuildMasterOnlyInitBytes()));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::packfoundation::BuildContent(1, 1, 0, &bodyBytes)));
    const std::filesystem::path missing = directory / "missing-content.pack";
    std::error_code removeError;
    std::filesystem::remove(missing, removeError);
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    const std::string missingPath = missing.string();
    std::string command = std::string("\"") + HARKEN_HOST_PATH +
        "\" att03-keep --init \"" + initPath + "\" --content \"" + contentPath +
        "\" --bad \"" + missingPath + "\" --action " +
        std::to_string(harkentest::packfoundation::kActionBase) + " 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
    FILE* pipe = _popen(command.c_str(), "rt");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    ASSERT_NE(nullptr, pipe);
    std::string output;
    char chunk[512];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr)
        output += chunk;
#ifdef _WIN32
    const int exitCode = _pclose(pipe);
#else
    const int exitCode = pclose(pipe);
#endif
    EXPECT_EQ(0, exitCode) << output;
    EXPECT_NE(std::string::npos, output.find("att03-keep refused VnResult="))
        << output;
    EXPECT_EQ(std::string::npos, output.find("att03-keep refused VnResult=0"))
        << output;
    EXPECT_NE(std::string::npos, output.find("published=1")) << output;
    EXPECT_NE(std::string::npos, output.find("att03-keep post VnResult=0"))
        << output;
    EXPECT_EQ(std::string::npos, output.find("firing=0 ")) << output;
    EXPECT_NE(std::string::npos, output.find("LoadedCount=")) << output;
}

TEST(CanonicalPackSetActivate, TraceActiveBadStructSizeWritesNoActivateJournal) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::filesystem::path tracePath =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-activate-badarg.vtrc";
    std::filesystem::create_directories(tracePath.parent_path());
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.string().c_str()));
    VnCanonicalPackSetActivationDescV1 desc{};
    desc.structSize = 1;
    VnCanonicalPackSetActivationReportV1 report{};
    report.structSize = sizeof(report);
    EXPECT_EQ(VN_INVALID_ARG, Vn_ActivateCanonicalPackSetV1(&desc, &report));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    std::ifstream in(tracePath, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    EXPECT_EQ(0u, CountTraceCall(bytes, harken::kTraceCallActivateCanonicalPackSet));
}



TEST(CanonicalPackSetActivate, JournalsFailedCanonicalLoadWithoutPublish) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-load-refusal";
    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterOnlyInitBytes()));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceBindingBytes(
            harkentest::ObjectId(8403), harkentest::ObjectId(7),
            harkentest::ObjectId(1234))));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    const std::string journalPath =
        (std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-load-refusal" /
         "failed-load.vtrc")
            .string();
    std::filesystem::create_directories(std::filesystem::path(journalPath).parent_path());
    ASSERT_EQ(VN_OK, Vn_TraceStart(journalPath.c_str()));

    VnPackLoadDesc initLoad{};
    initLoad.structSize = sizeof(initLoad);
    initLoad.utf8Path = initPath.c_str();
    VnPackHandle initHandle = 1;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&initLoad),
                          &initHandle));
    EXPECT_EQ(0u, initHandle);

    VnPackLoadDesc contentLoad{};
    contentLoad.structSize = sizeof(contentLoad);
    contentLoad.utf8Path = contentPath.c_str();
    VnPackHandle contentHandle = 1;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              Vn_LoadPack(reinterpret_cast<const VnPackDesc*>(&contentLoad),
                          &contentHandle));
    EXPECT_EQ(0u, contentHandle);

    VnPackDesc vintage{};
    vintage.structSize = sizeof(vintage);
    vintage.utf8Path = initPath.c_str();
    VnPackHandle vintageHandle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, Vn_LoadPack(&vintage, &vintageHandle));
    EXPECT_EQ(0u, vintageHandle);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
    EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());

    std::ifstream in(journalPath, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    ASSERT_GE(bytes.size(), harken::kTraceFixedHeaderBytes);
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof(headerBytes));
    std::uint32_t loadCalls = 0;
    std::size_t offset = headerBytes;
    while (offset + harken::kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset, sizeof(payloadBytes));
        std::memcpy(&type, bytes.data() + offset + 4, sizeof(type));
        offset += harken::kTraceRecordHeaderBytes;
        ASSERT_LE(payloadBytes, bytes.size() - offset);
        if (type == harken::kTraceRecordApiCall && payloadBytes >= 2) {
            std::uint16_t callId = 0;
            std::memcpy(&callId, bytes.data() + offset, sizeof(callId));
            if (callId == harken::kTraceCallLoadPack) {
                ++loadCalls;
            }
        }
        offset += payloadBytes;
    }
    EXPECT_EQ(3u, loadCalls);

    std::string command = std::string("\"") + HARKEN_HOST_PATH +
        "\" trace-replay \"" + journalPath + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
    FILE* pipe = _popen(command.c_str(), "rt");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    ASSERT_NE(nullptr, pipe);
    std::string output;
    char chunk[512];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr)
        output += chunk;
#ifdef _WIN32
    const int exitCode = _pclose(pipe);
#else
    const int exitCode = pclose(pipe);
#endif
    EXPECT_EQ(0, exitCode) << output;
}

constexpr VnBindingId kBusBindingId = harkentest::ObjectId(8401);
constexpr VnBindingId kVoiceBindingId = harkentest::ObjectId(8403);
constexpr VnVariableId kBindingVariableId = harkentest::ObjectId(7);
constexpr VnObjectId kVoicePlayableId = harkentest::ObjectId(1234);

const harken::PackComposition::PublishedOwner* OwnerForHandle(
    harken::Engine* engine, VnPackHandle handle) {
    const auto* published = engine->packComposition.Published();
    if (published == nullptr) {
        return nullptr;
    }
    for (const auto& owner : published->owners) {
        if (owner.handle == handle) {
            return &owner;
        }
    }
    return nullptr;
}

VnResult UnloadSet(VnPackHandle initHandle, std::uint64_t baseRevision) {
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = initHandle;
    unload.baseSetRevision = baseRevision;
    unload.unloadRevision = baseRevision + 1;
    return Vn_UnloadCanonicalPackSetV1(&unload);
}

TEST(CanonicalPackSetActivate, InitMasterBusBindingInstallsAndUnloads) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-master";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    EXPECT_EQ(first.initHandle,
              session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    EXPECT_TRUE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, first.initHandle));
    const auto* owner = OwnerForHandle(session.Engine(), first.initHandle);
    ASSERT_NE(nullptr, owner);
    ASSERT_NE(nullptr, owner->voiceBindings);
    EXPECT_TRUE(owner->voiceBindings->empty());
    std::uint32_t busIndex = harken::kInvalidBusIndex;
    session.Engine()->bindings.ForEachAttachment(
        [&](const harken::BindingTable::Attachment& attachment,
            const harken::BindingTable::Definition&) {
            if (attachment.bindingId == kBusBindingId) {
                busIndex = attachment.busIndex;
            }
        });
    EXPECT_EQ(session.Engine()->buses.FindIndex(VN_BUS_MASTER), busIndex);
    const std::uint32_t afterInstall =
        session.Engine()->propertyPublisher.WorstCaseCommits();
    ASSERT_EQ(VN_OK, UnloadSet(first.initHandle, first.report.resultSetRevision));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    EXPECT_FALSE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, first.initHandle));
    EXPECT_LT(session.Engine()->propertyPublisher.WorstCaseCommits(), afterInstall);
}

TEST(CanonicalPackSetActivate, InitBus2BindingUsesLiveIndex) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-bus2";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixtureV1()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    const std::uint32_t bus2 =
        session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2));
    ASSERT_NE(harken::kInvalidBusIndex, bus2);
    std::uint32_t attached = harken::kInvalidBusIndex;
    session.Engine()->bindings.ForEachAttachment(
        [&](const harken::BindingTable::Attachment& attachment,
            const harken::BindingTable::Definition&) {
            if (attachment.bindingId == kBusBindingId) {
                attached = attachment.busIndex;
            }
        });
    EXPECT_EQ(bus2, attached);
    ASSERT_EQ(VN_OK, UnloadSet(first.initHandle, first.report.resultSetRevision));
    EXPECT_EQ(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2)));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

TEST(CanonicalPackSetActivate, AfterAllPreparedBeforeCommitRollsBackBindings) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-fault";
    harken::SetPackStagingFaultForTests(
        harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    const auto failed = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, failed.result);
    EXPECT_EQ(0u, failed.initHandle);
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));

    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    harken::SetPackStagingFaultForTests(
        harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
              harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                    first.initHandle, 1));
    EXPECT_EQ(first.initHandle,
              session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    harken::SetPackStagingFaultForTests(
        harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
              UnloadSet(first.initHandle, first.report.resultSetRevision));
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    EXPECT_EQ(first.initHandle,
              session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
}

TEST(CanonicalPackSetActivate, SameHandleReplaceDiffsBindings) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-replace";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixtureV1()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    const auto unchanged = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixtureV1()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, unchanged.result);
    EXPECT_EQ(first.initHandle, unchanged.initHandle);
    EXPECT_EQ(1u, session.Engine()->bindings.AttachmentCountOf(kBusBindingId));
    EXPECT_TRUE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_OBJECT_ID(0, 2), first.initHandle));
    const std::uint32_t afterUnchanged =
        session.Engine()->propertyPublisher.WorstCaseCommits();
    const auto again = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixtureV1()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, again.result);
    EXPECT_EQ(first.initHandle, again.initHandle);
    EXPECT_EQ(1u, session.Engine()->bindings.AttachmentCountOf(kBusBindingId));
    EXPECT_EQ(afterUnchanged, session.Engine()->propertyPublisher.WorstCaseCommits());

    auto curveChanged = harkentest::MakeCanonicalBusVolumeBindingDesc(kBindingVariableId, 20);
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = kBusBindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS;
    attachment.objectId = VN_OBJECT_ID(0, 2);
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::WithCanonicalBindingRows(
            harkentest::BuildMasterBus2InitBytes(), kBusBindingId, curveChanged,
            &attachment)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                          first.initHandle, 1));
    const auto* live = session.Engine()->bindings.Find(kBusBindingId);
    ASSERT_NE(nullptr, live);
    EXPECT_TRUE(live->pendingInstall);
    EXPECT_EQ(20u, live->pendingTransitionMs);
    EXPECT_EQ(0u, live->transitionMs);
    ASSERT_EQ(VN_OK, Vn_Update());
    live = session.Engine()->bindings.Find(kBusBindingId);
    ASSERT_NE(nullptr, live);
    EXPECT_FALSE(live->pendingInstall);
    EXPECT_EQ(20u, live->transitionMs);

    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterBus2InitBytes()));
    member.utf8Path = first.initPath.c_str();
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                          first.initHandle, 2));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

TEST(CanonicalPackSetActivate, CallerTakenCookieIsSkippedOnUnload) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-cookie";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    VnBindingAttachDesc detach{};
    detach.structSize = sizeof(detach);
    detach.scopeKind = VN_TARGET_SCOPE_BUS;
    detach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_DetachBinding(kBusBindingId, &detach));
    ASSERT_EQ(VN_OK, Vn_UnregisterBinding(kBusBindingId));
    VnBindingDesc desc =
        harkentest::MakeCanonicalBusVolumeBindingDesc(kBindingVariableId);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBusBindingId, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBusBindingId, &attach));
    EXPECT_EQ(0u, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    ASSERT_EQ(VN_OK, UnloadSet(first.initHandle, first.report.resultSetRevision));
    EXPECT_NE(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    EXPECT_TRUE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, 0));
}

TEST(CanonicalPackSetActivate, InitAndContentVoiceBindingsStayOnOwnCookies) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kVoicePlayableId, &pcm));
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-voice";
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER)));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceBindingBytes(kVoiceBindingId, kBindingVariableId,
                                                     kVoicePlayableId)));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = harkentest::packfoundation::kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                          report));
    const VnPackHandle initHandle = report.initHandle;
    const VnPackHandle contentHandle = rows[1].handle;
    EXPECT_EQ(initHandle, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    EXPECT_EQ(0u, session.Engine()->bindings.DefinitionOwnerOf(kVoiceBindingId));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    const auto* initOwner = OwnerForHandle(session.Engine(), initHandle);
    const auto* contentOwner = OwnerForHandle(session.Engine(), contentHandle);
    ASSERT_NE(nullptr, initOwner);
    ASSERT_NE(nullptr, contentOwner);
    ASSERT_NE(nullptr, initOwner->voiceBindings);
    ASSERT_NE(nullptr, contentOwner->voiceBindings);
    EXPECT_TRUE(initOwner->voiceBindings->empty());
    ASSERT_EQ(1u, contentOwner->voiceBindings->size());
    EXPECT_EQ(kVoiceBindingId, contentOwner->voiceBindings->front().bindingId);
    EXPECT_FALSE(session.Engine()->bindings.HasOwnedAttachment(
        kVoiceBindingId, VN_TARGET_SCOPE_VOICE, kVoicePlayableId, contentHandle));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(contentHandle));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    EXPECT_EQ(initHandle, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
}

TEST(CanonicalPackSetActivate, ContentBusBindingDoesNotEnterTable) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kVoicePlayableId, &pcm));
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-content-bus";
    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterOnlyInitBytes()));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                   VN_BUS_MASTER, kVoicePlayableId)));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = harkentest::packfoundation::kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                          report));
    const VnPackHandle contentHandle = rows[1].handle;
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    EXPECT_EQ(0u, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    EXPECT_FALSE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, contentHandle));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(contentHandle));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

TEST(CanonicalPackSetActivate, InitVoiceBindingIsRefused) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-init-voice";
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = kVoiceBindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_VOICE;
    attachment.objectId = kVoicePlayableId;
    const auto failed = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::WithCanonicalBindingRows(
            harkentest::BuildMasterOnlyInitBytes(), kVoiceBindingId,
            harkentest::MakeCanonicalVoiceVolumeBindingDesc(kBindingVariableId),
            &attachment),
        harkentest::packfoundation::kInitPackStableId);
    EXPECT_EQ(VN_BAD_PACK, failed.result);
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
}

TEST(CanonicalPackSetActivate, InitBusEffectParamIsNotKindRefused) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-bus-effect";
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = kBusBindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attachment.objectId = VN_BUS_MASTER;
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = kBindingVariableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    desc.targetId = harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0);
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 1.0f};
    desc.curve.handles[0] =
        harkentest::EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    const auto failed = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::WithCanonicalBindingRows(harkentest::BuildMasterOnlyInitBytes(),
                                            kBusBindingId, desc, &attachment),
        harkentest::packfoundation::kInitPackStableId);
    EXPECT_NE(VN_OK, failed.result);
    EXPECT_NE(VN_PACK_INIT_LIFECYCLE, failed.result);
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
}

TEST(CanonicalPackSetActivate, VoiceEffectParamDoesNotEnterTable) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kVoicePlayableId, &pcm));
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-vep";
    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterOnlyInitBytes()));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceEffectParamBytes(
            kBusBindingId, kBindingVariableId,
            harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, harken::kGainParamGainDb),
            kVoicePlayableId)));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = harkentest::packfoundation::kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                          report));
    const VnPackHandle contentHandle = rows[1].handle;
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    EXPECT_EQ(0u, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    EXPECT_FALSE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, VN_BUS_MASTER, contentHandle));
    constexpr VnVoiceHandle kRender = 77;
    EXPECT_EQ(VN_UNKNOWN_ID, session.Engine()->bindings.ApplyVoiceEffectInstance(
                                 kBusBindingId, kRender, kRender,
                                 &session.Engine()->logicLogRing));
    EXPECT_FALSE(session.Engine()->bindings.ClearVoiceEffectInstance(kRender, kBusBindingId));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(contentHandle));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

TEST(CanonicalPackSetActivate, BindingCapacityRefusalsLeaveEmptyState) {
    {
        VnConfig config = MakeNullConfig();
        config.maxBindings = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnBindingDesc extra =
            harkentest::MakeCanonicalBusVolumeBindingDesc(kBindingVariableId);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(9001), &extra));
        const auto failed = harkentest::ActivateInitBytesV1(
            std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-def-cap",
            harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                          VN_BUS_MASTER),
            harkentest::packfoundation::kInitPackStableId);
        EXPECT_EQ(VN_NO_BINDING, failed.result);
        EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
        EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    }
    {
        VnConfig config = MakeNullConfig();
        config.maxBindingAttachments = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnBindingDesc extra =
            harkentest::MakeCanonicalBusVolumeBindingDesc(kBindingVariableId);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(9001), &extra));
        VnBindingAttachDesc attach{};
        attach.structSize = sizeof(attach);
        attach.scopeKind = VN_TARGET_SCOPE_BUS;
        attach.objectId = VN_BUS_MASTER;
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(9001), &attach));
        const auto failed = harkentest::ActivateInitBytesV1(
            std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-att-cap",
            harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                          VN_BUS_MASTER),
            harkentest::packfoundation::kInitPackStableId);
        EXPECT_EQ(VN_NO_BINDING, failed.result);
        EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
    }
    {
        VnConfig config = MakeNullConfig();
        config.commandQueueDepth = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::uint32_t capacity =
            session.Engine()->propertyPublisher.BatchCapacity();
        ASSERT_GT(capacity, 0u);
        ASSERT_TRUE(session.Engine()->propertyPublisher.TryGrowWorstCase(
            capacity, &session.Engine()->logicLogRing));
        const auto failed = harkentest::ActivateInitBytesV1(
            std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-pub-cap",
            harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                          VN_BUS_MASTER),
            harkentest::packfoundation::kInitPackStableId);
        EXPECT_EQ(VN_INVALID_CONFIG, failed.result);
        EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
        EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
    }
}

TEST(CanonicalPackSetActivate, ReplaceCanonicalPackSwapsContentBindings) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kVoicePlayableId, &pcm));
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-replace-content";
    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterOnlyInitBytes()));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceBindingBytes(kVoiceBindingId, kBindingVariableId,
                                                     kVoicePlayableId)));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = harkentest::packfoundation::kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                          report));
    const VnPackHandle contentHandle = rows[1].handle;
    const auto nextId = harkentest::ObjectId(8404);
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceBindingBytes(nextId, kBindingVariableId,
                                                     kVoicePlayableId)));
    VnPackLoadDesc replace{};
    replace.structSize = sizeof(replace);
    replace.utf8Path = contentPath.c_str();
    ASSERT_EQ(VN_OK, Vn_ReplacePack(contentHandle, reinterpret_cast<const VnPackDesc*>(&replace)));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(nextId));
    const auto* contentOwner = OwnerForHandle(session.Engine(), contentHandle);
    ASSERT_NE(nullptr, contentOwner);
    ASSERT_NE(nullptr, contentOwner->voiceBindings);
    ASSERT_EQ(1u, contentOwner->voiceBindings->size());
    EXPECT_EQ(nextId, contentOwner->voiceBindings->front().bindingId);
}

TEST(CanonicalPackSetActivate, UnloadPackOnInitLeavesCanonicalBindings) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-unloadpack";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    EXPECT_NE(VN_OK, Vn_UnloadPack(first.initHandle));
    EXPECT_EQ(first.initHandle,
              session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
}

TEST(CanonicalPackSetActivate, ReplaceDroppingBus2DetachesBeforeRetire) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-drop-bus2";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixtureV1()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterOnlyInitBytes()));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                          first.initHandle, 1));
    EXPECT_EQ(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2)));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

TEST(CanonicalPackSetActivate, ReplaceKeepsBindingWhenBus2Retires) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-keep-bus2-row";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixtureV1()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = kBusBindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS;
    attachment.objectId = VN_OBJECT_ID(0, 2);
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::WithCanonicalBindingRows(
            harkentest::BuildMasterOnlyInitBytes(), kBusBindingId,
            harkentest::MakeCanonicalBusVolumeBindingDesc(kBindingVariableId),
            &attachment)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_UNKNOWN_ID,
              harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                    first.initHandle, 1));
    EXPECT_EQ(first.initHandle,
              session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    EXPECT_TRUE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_OBJECT_ID(0, 2), first.initHandle));
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(VN_OBJECT_ID(0, 2)));
}

TEST(CanonicalPackSetActivate, IdentityChangeRebindsSameTriple) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-identity";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    const auto* before = session.Engine()->bindings.Find(kBusBindingId);
    ASSERT_NE(nullptr, before);
    const std::uint32_t oldRevision = before->revision;
    const VnVariableId nextVariable = harkentest::ObjectId(8);
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = kBusBindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS;
    attachment.objectId = VN_BUS_MASTER;
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::WithCanonicalBindingRows(
            harkentest::BuildMasterOnlyInitBytes(), kBusBindingId,
            harkentest::MakeCanonicalBusVolumeBindingDesc(nextVariable),
            &attachment)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                          first.initHandle, 1));
    const auto* after = session.Engine()->bindings.Find(kBusBindingId);
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(nextVariable, after->variableId);
    EXPECT_EQ(first.initHandle, after->ownerCookie);
    EXPECT_EQ(VN_TARGET_SCOPE_BUS, after->scopeKind);
    EXPECT_EQ(harken::kBusTargetBusVolume, after->targetId);
    EXPECT_TRUE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, first.initHandle));
    (void)oldRevision;
}

TEST(CanonicalPackSetActivate, CallerTakenCookieIsSkippedOnReplace) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-cookie-replace";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    VnBindingAttachDesc detach{};
    detach.structSize = sizeof(detach);
    detach.scopeKind = VN_TARGET_SCOPE_BUS;
    detach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_DetachBinding(kBusBindingId, &detach));
    ASSERT_EQ(VN_OK, Vn_UnregisterBinding(kBusBindingId));
    VnBindingDesc desc =
        harkentest::MakeCanonicalBusVolumeBindingDesc(kBindingVariableId);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(kBusBindingId, &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(kBusBindingId, &attach));
    EXPECT_EQ(0u, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = first.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                          first.initHandle, 1));
    EXPECT_EQ(0u, session.Engine()->bindings.DefinitionOwnerOf(kBusBindingId));
    EXPECT_TRUE(session.Engine()->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, 0));
}

TEST(CanonicalPackSetActivate, KeepContentManifestUnloadsAfterInitReplace) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kVoicePlayableId, &pcm));
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-keep";
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Init.pack",
        harkentest::BuildMasterInitWithBusBindingBytes(kBusBindingId, kBindingVariableId,
                                                      VN_BUS_MASTER)));
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceBindingBytes(kVoiceBindingId, kBindingVariableId,
                                                     kVoicePlayableId)));
    const std::string initPath = (directory / "Init.pack").string();
    const std::string contentPath = (directory / "Content.pack").string();
    std::array<VnCanonicalPackSetMemberV1, 2> members{};
    std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i].structSize = sizeof(members[i]);
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packStableId = harkentest::packfoundation::kInitPackStableId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].packStableId = harkentest::packfoundation::kResidentPackStableId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                          report));
    const VnPackHandle contentHandle = rows[1].handle;
    members[0].expectedHandle = report.initHandle;
    members[1].expectedHandle = contentHandle;
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
                                                          report, report.initHandle, 1));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    const auto* contentOwner = OwnerForHandle(session.Engine(), contentHandle);
    ASSERT_NE(nullptr, contentOwner);
    ASSERT_NE(nullptr, contentOwner->voiceBindings);
    ASSERT_EQ(1u, contentOwner->voiceBindings->size());
    EXPECT_EQ(kVoiceBindingId, contentOwner->voiceBindings->front().bindingId);
    ASSERT_EQ(VN_OK, Vn_UnloadPack(contentHandle));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    EXPECT_NE(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

} // namespace
