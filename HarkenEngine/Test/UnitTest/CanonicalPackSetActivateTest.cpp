#include "harken/media/PackStreamResolver.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "AllocationProbe.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "Common/VorbisFixture.h"
#include "harken/media/VorbisDecoder.h"
#include "harken/media/PackMediaRange.h"

#include "Engine.h"
#include "PackLogicStore.h"
#include "ProjectGenerationTransaction.h"
#include "TraceFormat.h"
#include "TraceRecorder.h"
#include "MonitorIdentityCodec.h"
#include "harken/media/generated/harken_pack_effect_schema.g.h"
#include "MonitorCaptureHubTestHooks.h"
#include <map>
#include <set>
#include "harken/dsp/GainRamp.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/core/PackCompanionNames.h"
#include "harken/core/MediaContentRevision.h"
#include "Common/StreamBuilder.h"
#include "harken/core/VariableExecutionFingerprint.h"
#include "harken/media/generated/harken_pack_effect_schema.g.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
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
using harkentest::PackAdmissionOpenForInit;
using harkentest::PackMasterHolder;

VnConfig CanonicalActivationConfig(std::uint32_t maxVoices = 0,
                                   std::uint32_t blockFrames = 0,
                                   std::uint32_t sampleRate = 0) {
    auto config = harkentest::MakeNullConfig(maxVoices, blockFrames, sampleRate);
    // Pack activation coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    return config;
}

TEST(CanonicalPackSetActivate, PathInitOnlyActivateRetryUnload) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-activate";
    EngineSession session(CanonicalActivationConfig());
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


std::vector<std::uint8_t> CanonicalLimiterInit(float ceilingDb, bool bypass = false,
    std::uint64_t contentRevision = 1, bool childBus = false, std::uint32_t effectCount = 8) {
    using namespace harkentest::packfoundation;
    ImageBuilder builder(harken::kPackKindInit);
    const auto buses = childBus ? Bus2RuntimeBusFixture() : DefaultRuntimeBusFixture();
    harken::VariableDefinitionV1 variable{};
    variable.variableId = kVariableId;
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = -1.0f;
    variable.rangeMax = 1.0f;
    harken::ContainerWorldInstallViewV1 world{};
    world.identity.capabilityMask = harken::kWorldCapabilityVariableDefinitions;
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    if (!harken::EncodeCurrentWorldShippingBody(world, body)) return {};
    harken::PackCanonicalPartitionHeader header{};
    header.headerBytes = harken::kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = harken::kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = harken::Fnv1a64(body.data(), body.size());
    header.packStableId = kInitPackStableId;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = 1;
    ApplyCanonicalBusTriple(&header, buses.triple);
    std::vector<std::uint8_t> partition(harken::kPackCanonicalPartitionHeaderBytes + body.size(), 0);
    harken::SerializePackCanonicalPartitionHeader(header, partition.data());
    header.headerChecksum = harken::Fnv1a64(partition.data(), harken::kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    harken::SerializePackCanonicalPartitionHeader(header, partition.data());
    std::memcpy(partition.data() + harken::kPackCanonicalPartitionHeaderBytes, body.data(), body.size());
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
        RequirementPayload(kInitPackStableId, contentRevision, 1, 1, body.size(), buses.triple));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition,
        kInitPackStableId, std::move(partition));
    std::uint64_t execution = 0;
    if (!harken::ComputeVariableExecutionFingerprintV1(variable.variableId, 1, 1, 3,
            variable.rangeMin, variable.rangeMax, variable.defaultValue, nullptr, 0, &execution)) return {};
    std::vector<std::uint8_t> overlay(harken::kPackVariablePolicyHeaderBytes +
        harken::kPackVariablePolicyRowBytes + harken::kPackVariableAnchorCountBytes +
        harken::kPackVariableLabelHeaderBytes, 0);
    harken::SerializePackVariablePolicyHeader({1}, overlay.data());
    std::size_t cursor = harken::kPackVariablePolicyHeaderBytes;
    harken::SerializePackVariablePolicyRow({variable.variableId, 3, execution}, overlay.data() + cursor);
    cursor += harken::kPackVariablePolicyRowBytes;
    harken::SerializePackVariableAnchorCount({0}, overlay.data() + cursor);
    cursor += harken::kPackVariableAnchorCountBytes;
    harken::SerializePackVariableLabelHeader({
        harken::ComputeVariableLabelMappingFingerprintV1(variable.variableId, {}), 0}, overlay.data() + cursor);
    builder.AddEntry(harken::kPackEntryKindVariablePolicyOverlay, 0, std::move(overlay));
    AppendRuntimeBusFixture(&builder, buses);
    for (std::uint32_t slot = 0; slot < effectCount; ++slot) {
        harken::PackBusEffectEntry effect{};
        effect.busId = childBus ? VN_OBJECT_ID(0, 2) : VN_BUS_MASTER;
        effect.slot = slot;
        effect.effectId = harkentest::ObjectId(0xEA00 + slot);
        effect.effectType = VN_EFFECT_LIMITER;
        effect.descriptorVintage = harken::FindPackEffectSchemaByType(VN_EFFECT_LIMITER)->descriptorVintage;
        effect.faderPointGuard = 8;
        effect.bypass = bypass ? 1 : 0;
        effect.paramCount = 2;
        effect.paramBytes = 8;
        effect.params[0] = slot == 7 ? ceilingDb : 0.0f;
        effect.params[1] = 100.0f;
        std::vector<std::uint8_t> payload(harken::kPackBusEffectBytes);
        harken::SerializePackBusEffect(effect, payload.data());
        const auto index = builder.AddEntry(harken::kPackEntryKindBusEffect,
            static_cast<std::uint64_t>(slot), std::move(payload));
        builder.SetRelationOrdinal(index, slot);
    }
    return builder.Build();
}

TEST(CanonicalPackSetActivate, InitBusLimitersRenderReplaceRollbackAndUnload) {
    EngineSession session(CanonicalActivationConfig(4, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-init-limiters";
    const auto first = harkentest::ActivateInitBytesV1(directory, CanonicalLimiterInit(-24),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    engine.nullBackend.Pump(1);
    auto* master = engine.buses.BusState(0);
    ASSERT_NE(nullptr, master);
    for (unsigned slot = 0; slot < 8; ++slot) {
        EXPECT_EQ(VN_EFFECT_LIMITER, master->fxSlotEffectRef[slot].type);
        EXPECT_EQ(first.initHandle, engine.busEffectOwnerCookies[0][slot]);
    }
    const auto sound = harkentest::ObjectId(0xEA20);
    const auto samples = harkentest::MakeDC(48000, 0.5f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    auto play = harkentest::MakePlayDesc(sound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine.nullBackend.Pump(4);
    EXPECT_NEAR(std::pow(10.0f, -24.0f / 20.0f), engine.nullBackend.LastBlock()[0], 2e-5f);

    const auto replacementPath = (directory / "Replacement.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(replacementPath, CanonicalLimiterInit(-18, true, 2)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.utf8Path = replacementPath.c_str();
    member.expectedHandle = first.initHandle;
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    const auto oldRef = engine.busEffectInstanceRefs[0][7];
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    const auto refused = harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        first.initHandle, 1);
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, refused);
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_EQ(oldRef.generation, engine.busEffectInstanceRefs[0][7].generation);
    engine.nullBackend.Pump(1);
    EXPECT_NEAR(std::pow(10.0f, -24.0f / 20.0f), engine.nullBackend.LastBlock()[0], 2e-5f);
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        first.initHandle, 1));
    // Reused instances follow the normal bypass transition.
    engine.nullBackend.Pump(8);
    for (unsigned frame = 0; frame < 64; ++frame)
        EXPECT_NEAR(0.5f, engine.nullBackend.LastBlock()[frame * 2], 2e-5f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = report.initHandle;
    unload.baseSetRevision = report.resultSetRevision;
    unload.unloadRevision = report.resultSetRevision + 1;
    const auto foreignBinding = harkentest::ObjectId(0xEA32);
    VnBindingDesc binding{};
    binding.structSize = sizeof(binding);
    binding.variableId = harkentest::packfoundation::kVariableId;
    binding.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    binding.targetId = harken::PackEffectParamLocator(VN_EFFECT_LIMITER, 7, 0);
    binding.curve.structSize = sizeof(binding.curve);
    binding.curve.pointCount = 2;
    binding.curve.points[0] = {-1.0f, 0.0f};
    binding.curve.points[1] = {1.0f, 0.0f};
    binding.curve.handles[0] = harkentest::EndpointHandles(binding.curve.points[0], binding.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(foreignBinding, &binding));
    VnBindingAttachDesc attachment{sizeof(attachment), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, VN_BUS_MASTER};
    ASSERT_EQ(VN_OK, Vn_AttachBinding(foreignBinding, &attachment));
    EXPECT_EQ(VN_IN_USE, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    ASSERT_EQ(VN_OK, Vn_DetachBinding(foreignBinding, &attachment));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    for (unsigned slot = 0; slot < 8; ++slot) {
        EXPECT_EQ(harken::kEffectTypeNone, master->fxSlotEffectRef[slot].type);
        EXPECT_FLOAT_EQ(0.0f, master->fxBypassMix[slot]);
        EXPECT_FLOAT_EQ(0.0f, master->fxBypassTarget[slot]);
    }
    EXPECT_EQ(VN_OK, harkentest::ActivateInitBytesV1(directory, CanonicalLimiterInit(-24),
        harkentest::packfoundation::kInitPackStableId).result);
}


TEST(CanonicalPackSetActivate, InitBusLimitersProtectMasterOwnershipBothDirections) {
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    engine.mixer.SetMaxCommandsPerQuantumForTest(9);
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-limiter-master-owner";
    ASSERT_FALSE(engine.previewMasterHolders.empty());
    auto& foreign = engine.previewMasterHolders.front();
    foreign = {true, 0xFEE0001, 1, harken::NeutralMasterFingerprintV1(engine)};
    EXPECT_EQ(VN_IN_USE, harkentest::ActivateInitBytesV1(directory, CanonicalLimiterInit(-24),
        harkentest::packfoundation::kInitPackStableId).result);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    foreign = {};
    const auto activated = harkentest::ActivateInitBytesV1(directory, CanonicalLimiterInit(-24),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, activated.result);
    EXPECT_FALSE(harken::PreviewMasterCompatible(engine, harken::NeutralMasterFingerprintV1(engine), 0xFEE0001));
    EXPECT_TRUE(harken::PreviewMasterCompatible(engine, harken::NeutralMasterFingerprintV1(engine),
        harken::kCanonicalPackSetMasterOwnerId));
    engine.nullBackend.Pump(1);
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = activated.initHandle;
    unload.baseSetRevision = activated.report.resultSetRevision;
    unload.unloadRevision = activated.report.resultSetRevision + 1;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
}

TEST(CanonicalPackSetActivate, FileBudgetRefusalPrecedesWholeFileAllocationAndCanRetry) {
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-file-budget";
    std::filesystem::create_directories(directory);
    const auto path = directory / "Init.bytes";
    const auto pathText = path.string();
    const auto tracePath = (directory / "budget.vntrace").string();
    constexpr std::size_t oversizedBytes = 4u * 1024u * 1024u;
    const std::vector<std::uint8_t> oversized(oversizedBytes, 0);
    const auto valid = harkentest::BuildMasterOnlyInitBytes();
    for (const bool trace : {false, true}) {
        SCOPED_TRACE(trace);
        auto config = CanonicalActivationConfig();
        config.maxPackDecodeStagingBytes = 64u * 1024u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        if (trace) ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
        ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, oversized));
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            VnPackHandle handle = 0;
            allocation_probe::count = 0;
            allocation_probe::bytes = 0;
            allocation_probe::enabled = true;
            const auto result = harkentest::ActivateExistingInitPackV1(
                pathText.c_str(), harkentest::packfoundation::kInitPackStableId, &handle);
            allocation_probe::enabled = false;
            const auto allocated = allocation_probe::bytes;
            // The bounded file path judges capacity by resident need after
            // metadata validation, so this invalid file is refused as such.
            EXPECT_EQ(trace ? VN_PACK_RESIDENT_CAPACITY : VN_BAD_PACK, result);
            EXPECT_LT(allocated, oversizedBytes);
            EXPECT_EQ(0u, handle);
            EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
            EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
            RecordProperty(trace ? "trace_refusal_allocated_bytes" : "file_refusal_allocated_bytes",
                           static_cast<int>(allocated));
        }
        ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, valid));
        VnPackHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::ActivateExistingInitPackV1(
            pathText.c_str(), harkentest::packfoundation::kInitPackStableId, &handle));
        EXPECT_NE(0u, handle);
        if (trace) EXPECT_EQ(VN_OK, Vn_TraceStop());
    }
}

namespace companionfixture {

constexpr VnObjectId project{0x0102030405060708, 0x090a0b0c0d0e0f10};
constexpr VnObjectId action = harkentest::ObjectId(harkentest::packfoundation::kActionBase);

std::uint64_t PackId(std::string suffix) {
    const auto path = "01234567-89ab-cdef-0123-456789abcdef/" + suffix;
    return harken::Fnv1a64(path.data(), path.size());
}

struct Provider {
    std::vector<std::uint8_t> bytes;
    unsigned reads = 0;
    unsigned releases = 0;
    static VnResult Read(void* context, std::uint64_t, const char*, std::uint32_t,
                         std::uint64_t, VnPackByteSpan* span) {
        auto& self = *static_cast<Provider*>(context);
        ++self.reads;
        *span = {sizeof(*span), 0, self.bytes.data(), self.bytes.size()};
        return VN_OK;
    }
    static void Release(void* context, std::uint64_t, VnPackByteSpan) {
        ++static_cast<Provider*>(context)->releases;
    }
};

std::vector<std::uint8_t> Build(bool init, std::uint64_t id, std::uint64_t revision = 1,
                                std::uint32_t actions = 1) {
    using namespace harkentest::packfoundation;
    const auto bus = DefaultRuntimeBusFixture();
    std::uint64_t size = 0;
    auto partition = PartitionPayload(id, revision, init ? 0 : actions,
        std::bit_cast<std::uint32_t>(0.5f), &size, 64, project, bus.triple);
    ImageBuilder image(init ? harken::kPackKindInit : harken::kPackKindCanonicalContent);
    image.AddEntry(harken::kPackEntryKindPackRequirementBlock, id,
        RequirementPayload(id, revision, revision, init ? 0 : actions + 1, size, bus.triple));
    image.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
    if (init) image.AddEntry(harken::kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    else image.AddEntry(harken::kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(PackId("Init"), 1, bus.triple), harken::kPackPackDependencyEdgeVintage);
    return image.Build();
}

VnCanonicalPackSetMemberV2 Member(std::uint64_t id, bool init, Provider* provider,
    const VnPackNamesV1* names, VnPackHandle handle = 0,
    std::uint32_t operation = VN_PACK_MEMBER_CONTENT) {
    VnCanonicalPackSetMemberV2 row{};
    row.structSize = sizeof(row);
    row.operation = operation;
    row.source.structSize = sizeof(row.source);
    row.source.packStableId = id;
    row.source.packKind = init ? VN_CANONICAL_PACK_SET_MEMBER_INIT : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    row.source.expectedHandle = handle;
    row.names = names;
    if (provider != nullptr) {
        row.source.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
        row.source.providerUserData = provider;
        row.source.provide = Provider::Read;
        row.source.release = Provider::Release;
    }
    return row;
}

VnResult Activate(std::vector<VnCanonicalPackSetMemberV2>& members,
    VnCanonicalPackSetActivationReportV1& report, std::uint64_t base = 0) {
    std::vector<VnCanonicalPackSetMemberResultV1> results(members.size());
    for (auto& row : results) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.expectedCurrentInitHandle = base == 0 ? 0 : report.initHandle;
    desc.baseSetRevision = base;
    desc.setRevision = base + 1;
    desc.memberCount = static_cast<std::uint32_t>(members.size());
    desc.memberStructSize = sizeof(members.front());
    desc.members = members.data();
    desc.outputMemberCapacity = desc.memberCount;
    desc.outputMemberStructSize = sizeof(results.front());
    desc.outputMembers = results.data();
    report.structSize = sizeof(report);
    const auto result = Vn_ActivateCanonicalPackSetV2(&desc, &report);
    if (result == VN_OK) {
        for (std::size_t i = 0; i < members.size(); ++i)
            members[i].source.expectedHandle = results[i].handle;
    }
    return result;
}

VnResult Resolve(const char* name, VnActionResolved* result) {
    VnPostActionByNameDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Name = name;
    result->structSize = sizeof(*result);
    return Vn_ResolveActionByName(&desc, result);
}

std::vector<std::uint8_t> BuildPreview(bool random, bool multipleSteps = false,
    VnAttenuationId attenuation = {}, const harken::WorldCapacitySummaryV1& capacity = {}) {
    using namespace harken;
    using namespace harkentest::packfoundation;
    const auto id = PackId("action_" + ObjectIdString(action));
    const auto bus = DefaultRuntimeBusFixture();
    const auto soundId = harkentest::ObjectId(8200);
    const auto containerId = harkentest::ObjectId(8201);
    std::vector<float> samples(attenuation == VnObjectId{} ? 480 : 48000, 0.8f);
    const float* planes[] = {samples.data()};
    std::uint64_t mediaRevision = 0;
    EXPECT_TRUE(ComputeMediaPcmContentRevisionV1(48000, 1, samples.size(), planes, 1, &mediaRevision));
    std::array<PlayablePropertyProgramViewV1, 2> properties{};
    for (std::size_t i = 0; i < properties.size(); ++i) {
        auto& row = properties[i];
        row.authoredStableOrder = static_cast<std::uint32_t>(i);
        row.stableId = i == 0 ? soundId : containerId;
        row.contentPropertyKind = i == 0 ? playableproperties::ContentPropertyKindV1::Sound :
            playableproperties::ContentPropertyKindV1::Container;
        row.contentRevision = i == 0 ? mediaRevision : 1;
        row.routeStableId = VN_BUS_MASTER;
        row.compatibilityFingerprint = 1;
        row.payload.presentMask = kResolvedSectionGroupPropertyMaskV1 | 1u;
        row.payload.values[0].bits = std::bit_cast<std::uint32_t>(i == 0 ? -6.0f : 0.0f);
        row.payload.values[3].bits = 1;
        row.payload.values[6].objectId = VN_BUS_MASTER;
        row.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
        row.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
        if (i == 0 && attenuation != VnObjectId{}) {
            row.payload.presentMask |= (1ull << 10) | (1ull << 13);
            row.payload.values[10].bits = 1;
            row.payload.values[13].objectId = attenuation;
        }
        row.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(row);
    }
    PlayableRefV1 sound{};
    sound.stableId = soundId;
    sound.expectedProgramSemanticFingerprint = properties[0].programSemanticFingerprint;
    sound.contentRevision = mediaRevision;
    sound.routeStableId = VN_BUS_MASTER;
    sound.compatibilityFingerprint = 1;
    std::array<ContainerChildProgramViewV2, 2> children{};
    std::array<PlayableOccurrencePropertyViewV1, 2> occurrences{};
    for (std::uint32_t i = 0; i < children.size(); ++i) {
        auto& child = children[i];
        child.playable = sound;
        child.stableChildOrdinal = i;
        child.authoredStableOrder = i;
        child.frequencyWeight = 1;
        auto& row = occurrences[i];
        row.parentKind = playableproperties::ContentPropertyKindV1::Container;
        row.parentStableId = containerId;
        row.parentProgramSemanticFingerprint = properties[1].programSemanticFingerprint;
        row.stableOccurrenceOrdinal = i;
        row.childPlayable = sound;
        row.payload.presentMask = 1;
        row.payload.values[0].bits = std::bit_cast<std::uint32_t>(-6.0f * (i + 1));
        row.payload.values[3].bits = 1;
        row.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
        row.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
        row.occurrenceSemanticFingerprint = ComputePlayableOccurrenceFingerprintV1(row);
    }
    ContainerProgramViewV3 container{};
    container.containerStableId = containerId;
    container.programSemanticFingerprint = properties[1].programSemanticFingerprint;
    container.childCount = 2;
    container.children = children.data();
    std::array<ActionStepProgramViewV1, 2> steps{};
    for (std::uint32_t i = 0; i < steps.size(); ++i) {
        auto& step = steps[i];
        step.authoredEventIndex = i;
        step.target.kind = random ? ActionTargetKindV1::Container : ActionTargetKindV1::Sound;
        step.target.stableId = random ? containerId : soundId;
        step.target.expectedProgramSemanticFingerprint = properties[random ? 1 : 0].programSemanticFingerprint;
        SetAbsoluteTime(&step.delay);
        SetAbsoluteTime(&step.fade);
        SetAbsoluteTime(&step.cooldown);
    }
    ActionProgramViewV1 program{};
    program.actionStableId = action;
    program.steps = steps.data();
    program.stepCount = multipleSteps ? 2 : 1;
    program.expandedStepOccurrenceCount = program.stepCount;
    program.expandedFiringCount = 1;
    program.programSemanticFingerprint = ComputeActionProgramFingerprintV1(program);
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = project;
    world.capacity = capacity;
    world.identity.capabilityMask = kWorldCapabilityActionDefinitions | kWorldCapabilityPlayableProperties |
        (random ? kWorldCapabilityRandomDraw : 0);
    world.actions = &program;
    world.actionCount = 1;
    world.playableProperties = properties.data();
    world.playablePropertyCount = random ? 2 : 1;
    world.containers = random ? &container : nullptr;
    world.containerCount = random ? 1 : 0;
    world.playableOccurrences = random ? occurrences.data() : nullptr;
    world.playableOccurrenceCount = random ? 2 : 0;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    EXPECT_TRUE(ValidateCurrentWorldSectionGroupsV1(world));
    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    CurrentWorldBodyImage decoded;
    EXPECT_EQ(CurrentWorldBodyDecodeResult::Ok, decoded.DecodeWithResult(body.data(), body.size(),
        kCanonicalDefinitionPartitionBodyVintage, CurrentWorldDecodeMode::StrictArtifact));
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = id;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = world.playablePropertyCount + 1;
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeader(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, partition.data());
    std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
    ImageBuilder image(kPackKindCanonicalContent);
    image.AddEntry(kPackEntryKindPackRequirementBlock, id,
        RequirementPayload(id, 1, 1, header.ownedDefinitionCount, body.size(), bus.triple));
    image.AddEntry(kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
    image.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(PackId("Init"), 1, bus.triple), kPackPackDependencyEdgeVintage);
    image.AddMedia(soundId, std::move(samples));
    return image.Build();
}

} // namespace companionfixture

TEST(CanonicalPackSetActivate, ProviderMembersShareDecodeBudgetAndPreservePriorPublication) {
    using namespace companionfixture;
    using namespace harken;
    Provider init{Build(true, PackId("Init"))};
    Provider content{Build(false, PackId("content"))};
    for (auto* provider : {&init, &content}) {
        // Trailing encoded padding is legal and belongs to the acquired owner.
        provider->bytes.resize(64 * 1024, 0);
        PackFileHeader header;
        ASSERT_TRUE(ReadPackFileHeader(provider->bytes.data(), &header));
        header.fileBytes = provider->bytes.size();
        SerializePackFileHeader(header, provider->bytes.data());
    }
    PackCanonicalImage first;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(init.bytes.data(), init.bytes.size(), &first));
    const auto encodedSum = init.bytes.size() + content.bytes.size();
    const auto previousMapAllowance = 4 * (sizeof(PackAttenuationBindingMapV1) +
        first.attenuations.size() * sizeof(PackAttenuationBindingMapV1::Binding));
    ASSERT_GT(first.staging.Bytes(), previousMapAllowance);
    const auto limitedBytes = encodedSum + previousMapAllowance +
        (first.staging.Bytes() - previousMapAllowance) / 2;
    ASSERT_LE(encodedSum + previousMapAllowance, limitedBytes);
    ASSERT_GT(encodedSum + first.staging.Bytes(), limitedBytes);
    RecordProperty("member_encoded_sum", std::to_string(encodedSum));
    RecordProperty("shared_decode_limit", std::to_string(limitedBytes));
    RecordProperty("previous_encoded_map_limit", std::to_string(encodedSum + previousMapAllowance));
    RecordProperty("first_retained_decode_bytes", std::to_string(first.staging.Bytes()));
    {
        auto budget = detail::MakePackDecodeBudget(limitedBytes);
        detail::PackDecodeCharge encoded(budget);
        encoded.Add(init.bytes.size());
        PackCanonicalImage retained;
        ASSERT_EQ(VN_OK, ParsePackCanonicalImageWithBudget(init.bytes.data(), init.bytes.size(),
            &retained, nullptr, budget));
        EXPECT_THROW(encoded.Add(content.bytes.size()), detail::PackDecodeCapacity);
        EXPECT_EQ(init.bytes.size() + retained.staging.Bytes(), budget->live);
    }
    // The old encoded aggregate, including its map allowance, fits. Retaining
    // the first parsed image makes the second encoded owner exceed the ledger.
    for (auto* provider : {&init, &content}) {
        auto budget = detail::MakePackDecodeBudget(limitedBytes);
        detail::PackDecodeCharge encoded(budget);
        encoded.Add(provider->bytes.size());
        PackCanonicalImage image;
        ASSERT_EQ(VN_OK, ParsePackCanonicalImageWithBudget(provider->bytes.data(),
            provider->bytes.size(), &image, nullptr, budget));
        EXPECT_LT(budget->live, limitedBytes);
    }
    for (const auto limit : {limitedBytes, std::uint64_t{512 * 1024}}) {
        SCOPED_TRACE(limit);
        auto config = CanonicalActivationConfig();
        config.maxPackDecodeStagingBytes = limit;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        std::array<VnCanonicalPackSetMemberV1, 2> members{
            Member(PackId("Init"), true, &init, nullptr).source,
            Member(PackId("content"), false, &content, nullptr).source};
        std::array<VnCanonicalPackSetMemberResultV1, 2> rows{};
        for (auto& row : rows) row.structSize = sizeof(row);
        VnCanonicalPackSetActivationReportV1 report{};
        ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 1, rows.data(), report));
        const auto prior = session.Engine()->packComposition.PublishedHandle();
        const auto generation = session.Engine()->packComposition.CompositeGeneration();
        members[0].expectedHandle = report.initHandle;
        init.reads = init.releases = content.reads = content.releases = 0;
        const auto result = harkentest::ActivateCanonicalMembersV1(members.data(), 2, rows.data(),
            report, prior->initHandle, prior->ledger.setRevision);
        EXPECT_EQ(limit == limitedBytes ? VN_PACK_RESIDENT_CAPACITY : VN_OK, result);
        // Reaching the second provider proves the first member parsed successfully.
        EXPECT_EQ(1u, init.reads);
        EXPECT_EQ(1u, content.reads);
        EXPECT_EQ(init.reads, init.releases);
        EXPECT_EQ(content.reads, content.releases);
        if (limit == limitedBytes) {
            EXPECT_EQ(prior.get(), session.Engine()->packComposition.Published());
            EXPECT_EQ(generation, session.Engine()->packComposition.CompositeGeneration());
            EXPECT_EQ(1u, session.Engine()->packs.LoadedCount());
            ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 1, rows.data(),
                report, prior->initHandle, prior->ledger.setRevision));
        } else {
            EXPECT_EQ(generation + 1, session.Engine()->packComposition.CompositeGeneration());
            EXPECT_EQ(2u, session.Engine()->packs.LoadedCount());
        }
    }
}

TEST(CanonicalPackSetActivate, CompanionInitOwnsVariableAndLabelNames) {
    using namespace companionfixture;
    using namespace harken;
    using namespace harkentest::packfoundation;
    const auto variableId = harkentest::ObjectId(7);
    const VariableLabelDefinitionV1 label{8, "", harkentest::ObjectId(9)};
    const std::int32_t anchor = 8;
    std::uint64_t execution = 0;
    ASSERT_TRUE(ComputeVariableExecutionFingerprintV1(variableId, 0, 1, 3,
        0, 8, 0, &anchor, 1, &execution));
    auto variable = harkentest::MakeVariableDefinition(variableId, VariableKindV1::Step, 0, 8, 0);
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = project;
    world.identity.capabilityMask = kWorldCapabilityVariableDefinitions;
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    const auto initId = PackId("Init");
    const auto bus = DefaultRuntimeBusFixture();
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = initId;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = 1;
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeader(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, partition.data());
    std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
    std::vector<std::uint8_t> overlay(96, 0);
    StoreLE32(overlay.data(), 1);
    StoreLE32(overlay.data() + 4, 2);
    StoreObjectId(overlay.data() + 16, variableId);
    overlay[32] = 3;
    StoreLE64(overlay.data() + 40, execution);
    StoreLE32(overlay.data() + 48, 1);
    StoreLE32(overlay.data() + 52, 8);
    StoreLE64(overlay.data() + 56, ComputeVariableLabelMappingFingerprintV1(variableId, std::span(&label, 1)));
    StoreLE32(overlay.data() + 64, 1);
    StoreObjectId(overlay.data() + 72, label.labelId);
    StoreLE32(overlay.data() + 88, 8);
    ImageBuilder image(kPackKindInit);
    image.AddEntry(kPackEntryKindPackRequirementBlock, initId,
        RequirementPayload(initId, 1, 1, 1, body.size(), bus.triple));
    image.AddEntry(kPackEntryKindCanonicalDefinitionPartition, initId, std::move(partition));
    image.AddEntry(kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    image.AddEntry(kPackEntryKindVariablePolicyOverlay, 0, std::move(overlay));
    Provider init{image.Build()};
    VnPackVariableNameV1 variableName{variableId, "State"};
    VnPackLabelNameV1 labelName{variableId, label.labelId, "Eight"};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, {}, "", "", 1, 1, &variableName, &labelName};
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<VnCanonicalPackSetMemberV2> members{Member(initId, true, &init, &names)};
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(members, report));
    VnPostActionByNameDesc lookup{};
    lookup.structSize = sizeof(lookup);
    lookup.utf8Name = "State";
    VnVariableResolved resolved{};
    resolved.structSize = sizeof(resolved);
    ASSERT_EQ(VN_OK, Vn_ResolveVariableByName(&lookup, &resolved));
    EXPECT_EQ(variableId, resolved.variableId);
    VnEmitterHandle localEmitter = 0;
    VnEmitterHandle siblingEmitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&localEmitter));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&siblingEmitter));
    const auto expectNamedValue = [&](VnEmitterHandle emitter, float expected, bool local) {
        VnVariableQueryDescV1 namedQuery{};
        namedQuery.structSize = sizeof(namedQuery);
        namedQuery.variableId = variableId;
        namedQuery.emitter = emitter;
        VnVariableQueryResultV1 namedValue{};
        namedValue.structSize = sizeof(namedValue);
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&namedQuery, &namedValue));
        EXPECT_FLOAT_EQ(expected, namedValue.currentValue);
        EXPECT_EQ(local ? 1u : 0u, namedValue.scope);
        EXPECT_EQ(local ? 1u : 0u, namedValue.hasEmitterOverride);
    };
    ASSERT_EQ(VN_OK, Vn_SetValueByName("State", 3));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValueByName(localEmitter, "State", 5));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(0, 3, false));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(localEmitter, 5, true));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(siblingEmitter, 3, false));
    ASSERT_EQ(VN_OK, Vn_SetValueByLabelName(variableId, "Eight", 0));
    const auto generation = session.Engine()->variables.CurrentWorldGeneration();
    members[0].operation = VN_PACK_MEMBER_NAME_ONLY;
    variableName.utf8Name = "Mode";
    labelName.utf8Name = "Active";
    ASSERT_EQ(VN_OK, Activate(members, report, 1));
    EXPECT_EQ(generation, session.Engine()->variables.CurrentWorldGeneration());
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    VnVariableQueryDescV1 query{};
    query.structSize = sizeof(query);
    query.variableId = variableId;
    VnVariableQueryResultV1 value{};
    value.structSize = sizeof(value);
    ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &value));
    EXPECT_FLOAT_EQ(8.0f, value.currentValue);
    EXPECT_EQ(VN_NO_VARIABLE, Vn_ResolveVariableByName(&lookup, &resolved));
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValueByName("State", 2));
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetEmitterValueByName(localEmitter, "State", 4));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(0, 8, false));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(localEmitter, 8, false));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(siblingEmitter, 8, false));
    lookup.utf8Name = "Mode";
    EXPECT_EQ(VN_OK, Vn_ResolveVariableByName(&lookup, &resolved));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetValueByLabelName(variableId, "Eight", 0));
    EXPECT_EQ(VN_OK, Vn_SetValueByLabelName(variableId, "Active", 0));
    names.variableCount = 0;
    names.labelCount = 0;
    EXPECT_EQ(VN_BAD_PACK, Activate(members, report, 2));
    EXPECT_EQ(VN_OK, Vn_ResolveVariableByName(&lookup, &resolved));
    ASSERT_EQ(VN_OK, Vn_SetValueByName("Mode", 2));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValueByName(localEmitter, "Mode", 4));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(0, 2, false));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(localEmitter, 4, true));
    ASSERT_NO_FATAL_FAILURE(expectNamedValue(siblingEmitter, 2, false));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(localEmitter));
    ASSERT_EQ(VN_OK, Vn_DestroyEmitter(siblingEmitter));
}

TEST(CanonicalPackSetActivate, CompanionInspectionDiscoversProjectWithoutWeakeningActivation) {
    using namespace companionfixture;
    Provider init{Build(true, PackId("Init"))};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, {}, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackInspectDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.pack = {sizeof(VnPackByteSpan), 0, init.bytes.data(), init.bytes.size()};
    desc.names = &names;
    desc.maxInputBytes = init.bytes.size() + 4096;
    VnPackInspectReportV1 facts{};
    facts.structSize = sizeof(facts);
    ASSERT_EQ(VN_OK, Vn_InspectCanonicalPackV1(&desc, &facts));
    EXPECT_EQ(project, facts.projectId);
    EXPECT_EQ(PackId("Init"), facts.packStableId);
    EXPECT_EQ(VnObjectId{}, names.projectId);
    names.projectId = VN_OBJECT_ID(5, 9);
    EXPECT_EQ(VN_GAMEPLAY_WRONG_PROJECT, Vn_InspectCanonicalPackV1(&desc, &facts));
    names.projectId = {};
    desc.maxInputBytes = init.bytes.size() - 1;
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, Vn_InspectCanonicalPackV1(&desc, &facts));
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<VnCanonicalPackSetMemberV2> members{Member(PackId("Init"), true, &init, &names)};
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_INVALID_ARG, Activate(members, report));
}

TEST(CanonicalPackSetActivate, MonitorV3MatchesActualOccurrenceAndRetainsOlderConsumers) {
    using namespace companionfixture;
    using namespace harken::monitor;
    for (bool random : {false, true}) {
        SCOPED_TRACE(random);
        EngineSession session(CanonicalActivationConfig(8, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto initId = PackId("Init");
        const auto contentId = PackId("action_" + harken::ObjectIdString(action));
        Provider init{Build(true, initId)}, content{BuildPreview(random)};
        VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
        VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
        std::vector<VnCanonicalPackSetMemberV2> members{
            Member(initId, true, &init, &initNames), Member(contentId, false, &content, &names)};
        VnCanonicalPackSetActivationReportV1 activation{};
        ASSERT_EQ(VN_OK, Activate(members, activation));
        VnPackInspectDescV1 inspect{};
        inspect.structSize = sizeof(inspect);
        inspect.pack = {sizeof(VnPackByteSpan), 0, content.bytes.data(), content.bytes.size()};
        inspect.init = {sizeof(VnPackByteSpan), 0, init.bytes.data(), init.bytes.size()};
        inspect.names = &names;
        inspect.maxInputBytes = content.bytes.size() + init.bytes.size() + 4096;
        std::array<VnPackOccurrenceFactV1, 2> occurrences{};
        inspect.occurrences = occurrences.data(); inspect.occurrenceCapacity = occurrences.size();
        VnPackMediaFactV1 media{}; inspect.media = &media; inspect.mediaCapacity = 1;
        VnPackInspectReportV1 report{}; report.structSize = sizeof(report);
        ASSERT_EQ(VN_OK, Vn_InspectCanonicalPackV1(&inspect, &report));
        std::array<VnMonitorSessionHandle, 3> monitors{};
        for (unsigned i = 0; i != monitors.size(); ++i) {
            VnMonitorSessionDescV2 desc{}; desc.structSize = sizeof(desc);
            desc.requestedExecutionFactVersion = i + 1;
            desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
            desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
            desc.consumerQueueBytes = 65536;
            ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &monitors[i]));
        }
        StopDrainForTests();
        std::set<std::uint32_t> selected;
        std::array<unsigned, 3> starts{}, retired{};
        std::map<std::uint64_t, VnMonitorPlayableProvenance> active;
        const auto poll = [&] {
            DrainOnceForTests();
            for (unsigned version = 0; version != monitors.size(); ++version) {
                std::array<std::uint8_t, 65536> bytes{};
                VnMonitorPollDesc desc{sizeof(VnMonitorPollDesc), 0, bytes.data(), bytes.size()};
                VnMonitorPollResult result{}; result.structSize = sizeof(result);
                ASSERT_EQ(VN_OK, Vn_MonitorPoll(monitors[version], &desc, &result));
                for (std::size_t at = 0; at < result.bytesWritten;) {
                    std::uint64_t size = 0;
                    ExecutionPayloadReader sizeReader{bytes.data() + at + 16};
                    sizeReader(size);
                    ASSERT_GE(size, kPackedEnvelopeBytes); ASSERT_LE(size, result.bytesWritten - at);
                    VnMonitorRecordHeader header{};
                    ASSERT_TRUE(ReadIdentityHeader(bytes.data() + at, size, header));
                    if (header.recordKind == VN_MONITOR_RECORD_EXECUTION_FACT) {
                        EXPECT_LE(header.recordVersion, version + 1);
                        VnMonitorExecutionFactV3 value{};
                        ASSERT_TRUE(DecodeExecutionFact(header, bytes.data() + at + kPackedEnvelopeBytes, header.payloadBytes, value));
                        if (value.fact.factKind == VN_MONITOR_FACT_PLAYABLE_STARTED) {
                            ++starts[version];
                            if (version == 2) {
                                const auto& actual = value.playable;
                                ASSERT_EQ(VN_MONITOR_PLAYABLE_PROVENANCE_KNOWN, actual.knownFlags);
                                const VnPackOccurrenceFactV1* match = nullptr;
                                for (unsigned n = 0; n < report.occurrenceCount; ++n)
                                    if (occurrences[n].childOrdinal == actual.childOrdinal) match = &occurrences[n];
                                ASSERT_NE(nullptr, match);
                                EXPECT_EQ(match->rootPlayableId, actual.rootPlayableId);
                                EXPECT_EQ(match->rootProgramFingerprint, actual.rootProgramFingerprint);
                                EXPECT_EQ(match->occurrenceFingerprint, actual.occurrenceFingerprint);
                                EXPECT_EQ(match->mediaChecksum, actual.mediaChecksum);
                                EXPECT_EQ(match->soundId, header.stableId);
                                EXPECT_EQ(match->soundProgramFingerprint, header.programSemanticFingerprint);
                                selected.insert(actual.childOrdinal);
                                active[header.instanceHandle] = actual;
                            }
                        }
                        if (value.fact.factKind == VN_MONITOR_FACT_PLAYABLE_RETIRED && value.fact.contentKind == VN_MONITOR_CONTENT_SOUND) {
                            ++retired[version];
                            if (version == 2) {
                                const auto found = active.find(header.instanceHandle);
                                ASSERT_NE(active.end(), found);
                                EXPECT_EQ(found->second.childOrdinal, value.playable.childOrdinal);
                                EXPECT_EQ(found->second.mediaChecksum, value.playable.mediaChecksum);
                                EXPECT_EQ(found->second.occurrenceFingerprint, value.playable.occurrenceFingerprint);
                                active.erase(found);
                            }
                        }
                    }
                    at += size;
                }
            }
        };
        VnActionResolved resolved{}; ASSERT_EQ(VN_OK, Resolve("Play_A", &resolved));
        for (unsigned seed = 0; seed != 16; ++seed) {
            VnActionFiringHandle firing = 0;
            ASSERT_EQ(VN_OK, Vn_PostActionResolved(&resolved, 0, seed, &firing));
            for (unsigned block = 0; block != 24; ++block) {
                session.Engine()->nullBackend.Pump(1);
                ASSERT_EQ(VN_OK, Vn_Update());
                poll();
            }
            ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
        }
        for (unsigned i = 0; i != monitors.size(); ++i) {
            EXPECT_EQ(16u, starts[i]); EXPECT_EQ(16u, retired[i]);
            EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(monitors[i]));
        }
        EXPECT_EQ(random ? 2u : 1u, selected.size());
        EXPECT_TRUE(active.empty());
    }
}

TEST(CanonicalPackSetActivate, CompanionInspectionKeepsMediaAndOccurrenceFactsDistinct) {
    using namespace companionfixture;
    auto init = Build(true, PackId("Init"));
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    for (const bool random : {false, true}) {
        SCOPED_TRACE(random ? "Random" : "Sound");
        auto pack = BuildPreview(random);
        VnPackInspectDescV1 desc{};
        desc.structSize = sizeof(desc);
        desc.pack = {sizeof(VnPackByteSpan), 0, pack.data(), pack.size()};
        desc.init = {sizeof(VnPackByteSpan), 0, init.data(), init.size()};
        desc.names = &names;
        desc.maxInputBytes = pack.size() + init.size() + 4096;
        VnPackInspectReportV1 report{};
        report.structSize = sizeof(report);
        ASSERT_EQ(VN_OK, Vn_InspectCanonicalPackV1(&desc, &report));
        EXPECT_EQ(1u, report.mediaCount);
        ASSERT_EQ(random ? 2u : 1u, report.occurrenceCount);
        EXPECT_EQ(random ? VN_PACK_PREVIEW_RANDOM_SOUNDS : VN_PACK_PREVIEW_SOUND, report.previewKind);
        std::array<VnPackOccurrenceFactV1, 2> rows{};
        VnPackMediaFactV1 media{};
        desc.occurrences = rows.data();
        desc.occurrenceCapacity = 2;
        desc.media = &media;
        desc.mediaCapacity = 1;
        ASSERT_EQ(VN_OK, Vn_InspectCanonicalPackV1(&desc, &report));
        EXPECT_EQ(480u, media.frameCount);
        EXPECT_EQ(48000u, media.sampleRate);
        EXPECT_EQ(media.soundId, rows[0].soundId);
        EXPECT_EQ(media.mediaChecksum, rows[0].mediaChecksum);
        EXPECT_NE(0u, rows[0].flags & VN_PACK_OCCURRENCE_FIXED_GAIN);
        EXPECT_FLOAT_EQ(random ? -12.0f : -6.0f, rows[0].fixedGainDb);
        EXPECT_NEAR(std::pow(10.0, rows[0].fixedGainDb / 20.0), rows[0].fixedGainLinear, 1e-6);
        EXPECT_NE(0u, rows[0].flags & VN_PACK_OCCURRENCE_DURATION);
        EXPECT_DOUBLE_EQ(0.01, rows[0].durationSeconds);
        if (random) {
            EXPECT_EQ(rows[0].soundId, rows[1].soundId);
            EXPECT_NE(rows[0].occurrenceFingerprint, rows[1].occurrenceFingerprint);
            EXPECT_FLOAT_EQ(-18.0f, rows[1].fixedGainDb);
            desc.occurrenceCapacity = 1;
            rows[0].fixedGainDb = 123.0f;
            media.frameCount = 99;
            EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, Vn_InspectCanonicalPackV1(&desc, &report));
            EXPECT_FLOAT_EQ(123.0f, rows[0].fixedGainDb);
            EXPECT_EQ(99u, media.frameCount);
        }
        pack = BuildPreview(random, true);
        desc.pack = {sizeof(VnPackByteSpan), 0, pack.data(), pack.size()};
        desc.maxInputBytes = pack.size() + init.size() + 4096;
        ASSERT_EQ(VN_OK, Vn_InspectCanonicalPackV1(&desc, &report));
        EXPECT_EQ(VN_PACK_PREVIEW_UNSUPPORTED, report.previewKind);
        EXPECT_EQ(0u, report.occurrenceCount);
        EXPECT_EQ(1u, report.mediaCount);
        names.utf8Language = "English";
        EXPECT_EQ(VN_BAD_PACK, Vn_InspectCanonicalPackV1(&desc, &report));
        names.utf8Language = "";
        desc.maxInputBytes = 1;
        EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, Vn_InspectCanonicalPackV1(&desc, &report));
    }
}

TEST(CanonicalPackSetActivate, CompanionNamesKeepOwnersAndAcceptedVariableWrites) {
    using namespace companionfixture;
    EngineSession session(CanonicalActivationConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto initId = PackId("Init");
    const auto contentId = PackId("action_" + harken::ObjectIdString(action));
    Provider init{Build(true, initId)}, content{Build(false, contentId)};
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    std::vector<VnCanonicalPackSetMemberV2> members{Member(initId, true, &init, &initNames)};
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(members, report));
    const auto initialWorld = engine->variables.CurrentWorldGeneration();
    const auto initialActivation = report.activationGeneration;
    members[0] = Member(initId, true, nullptr, nullptr, report.initHandle, VN_PACK_MEMBER_KEEP);
    members.push_back(Member(contentId, false, &content, &names));
    ASSERT_EQ(VN_OK, Activate(members, report, 1));
    EXPECT_EQ(initialWorld, engine->variables.CurrentWorldGeneration());
    EXPECT_EQ(initialActivation, report.activationGeneration);
    EXPECT_EQ(1u, init.reads);
    VnActionResolved old{};
    ASSERT_EQ(VN_OK, Resolve("Play_A", &old));
    EXPECT_EQ(action, old.actionId);
    const auto token = engine->packComposition.Published()->owners[1].resourceToken;
    VnVariableWriteV1 write{harkentest::packfoundation::kVariableId, 0.75f, 0};
    VnVariableBatchDescV1 batch{sizeof(batch), 1, 0, 0, &write};
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnVariableTicketV1 emitterTicket{};
    write.value = 0.25f;
    ASSERT_EQ(VN_OK, Vn_SetEmitterValues(emitter, &batch, &emitterTicket));
    names.utf8RootName = "Play_Renamed";
    members[1].operation = VN_PACK_MEMBER_NAME_ONLY;
    ASSERT_EQ(VN_OK, Activate(members, report, 2));
    EXPECT_EQ(initialWorld, engine->variables.CurrentWorldGeneration());
    EXPECT_EQ(initialActivation, report.activationGeneration);
    EXPECT_EQ(token, engine->packComposition.Published()->owners[1].resourceToken);
    EXPECT_EQ(1u, init.reads);
    EXPECT_EQ(2u, content.reads);
    VnActionResolved current{};
    EXPECT_EQ(VN_UNKNOWN_ID, Resolve("Play_A", &current));
    ASSERT_EQ(VN_OK, Resolve("Play_Renamed", &current));
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_STALE_HANDLE, Vn_PostActionResolved(&old, 0, 0, &firing));
    VnVariableTicketQueryDescV1 query{sizeof(query), 0, ticket};
    VnVariableTicketQueryResultV1 outcome{};
    outcome.structSize = sizeof(outcome);
    ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &outcome));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_PENDING, outcome.outcome);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &outcome));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, outcome.outcome);
    query.ticket = emitterTicket;
    ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &outcome));
    EXPECT_EQ(VN_VARIABLE_OUTCOME_APPLIED, outcome.outcome);
    EXPECT_EQ(VN_OK, Vn_PostActionResolved(&current, 0, 0, &firing));
    VnActionPinnedDesc pin{};
    pin.structSize = sizeof(pin);
    pin.actionId = action;
    pin.utf8ExpectedName = "Play_Renamed";
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_PostActionPinned(&pin, &firing));
    const std::uint8_t invalidCatalog = 0;
    VnPackByteSpan catalog{sizeof(catalog), 0, &invalidCatalog, 1};
    EXPECT_EQ(VN_AUTHORITY_CONFLICT, Vn_SetGameplayCatalog(report.initHandle, &catalog));
    EXPECT_EQ(init.reads, init.releases);
    EXPECT_EQ(content.reads, content.releases);
}

TEST(CanonicalPackSetActivate, CompanionRefusalsKeepNamesAndContentAtomic) {
    using namespace companionfixture;
    EngineSession session(CanonicalActivationConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto initId = PackId("Init");
    const auto contentId = PackId("action_" + harken::ObjectIdString(action));
    Provider init{Build(true, initId)}, content{Build(false, contentId, 1, 2)};
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    std::vector<VnCanonicalPackSetMemberV2> members{
        Member(initId, true, &init, &initNames), Member(contentId, false, &content, &names)};
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(members, report));
    members[0] = Member(initId, true, nullptr, nullptr, report.initHandle, VN_PACK_MEMBER_KEEP);
    members[1].operation = VN_PACK_MEMBER_NAME_ONLY;
    const auto original = session.Engine()->packComposition.PublishedHandle();
    names.rootActionId = harkentest::ObjectId(harkentest::packfoundation::kActionBase + 1);
    EXPECT_EQ(VN_BAD_PACK, Activate(members, report, 1));
    names.rootActionId = action;
    names.utf8Language = "English";
    EXPECT_EQ(VN_BAD_PACK, Activate(members, report, 1));
    names.utf8Language = "";
    names.projectId = harkentest::ObjectId(99);
    EXPECT_EQ(VN_GAMEPLAY_WRONG_PROJECT, Activate(members, report, 1));
    names.projectId = project;
    names.utf8RootName = "Play_Renamed";
    content.bytes = Build(false, contentId, 2, 2);
    EXPECT_EQ(VN_PACK_STALE_REVISION, Activate(members, report, 1));
    EXPECT_EQ(original, session.Engine()->packComposition.PublishedHandle());
    VnActionResolved resolved{};
    EXPECT_EQ(VN_OK, Resolve("Play_A", &resolved));
    EXPECT_EQ(VN_UNKNOWN_ID, Resolve("Play_Renamed", &resolved));
    members[1].operation = VN_PACK_MEMBER_CONTENT;
    ASSERT_EQ(VN_OK, Activate(members, report, 1));
    EXPECT_EQ(VN_OK, Resolve("Play_Renamed", &resolved));
    const auto revision = report.resultSetRevision;
    EXPECT_EQ(VN_OK, Activate(members, report, 1));
    names.utf8RootName = "Play_Stale";
    EXPECT_EQ(VN_REVISION_CONFLICT, Activate(members, report, 1));
    names.utf8RootName = "Play_Renamed";
    EXPECT_EQ(revision, report.resultSetRevision);
    members.resize(1);
    ASSERT_EQ(VN_OK, Activate(members, report, revision));
    EXPECT_EQ(VN_UNKNOWN_ID, Resolve("Play_Renamed", &resolved));
    EXPECT_EQ(1u, init.reads);
}

TEST(CanonicalPackSetActivate, CommandQueueRefusalReturnsPreparedTopologyPlan) {
    VnConfig config = CanonicalActivationConfig();
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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
        // Stop before destructive probes if the mutation barrier is missing.
        ASSERT_EQ(VN_IN_USE, Vn_Update());
        EXPECT_EQ(VN_IN_USE, Vn_Init(nullptr));
        EXPECT_EQ(VN_IN_USE, Vn_Term());
        EXPECT_EQ(VN_IN_USE, Vn_LoadPack(nullptr, nullptr));
        EXPECT_EQ(VN_IN_USE, Vn_ReplacePack(0, nullptr));
        EXPECT_EQ(VN_IN_USE, Vn_UnloadCanonicalPackSetV1(nullptr));
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
        EngineSession session(CanonicalActivationConfig());
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

TEST(CanonicalPackSetActivate, SinglePackProvidersRejectReentrantMutationAndRecover) {
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "single-pack-provider-reentry";
    std::filesystem::create_directories(directory);
    const auto tracePath = (directory / "provider.vntrace").string();
    for (const bool replace : {false, true}) {
        for (int mode = 0; mode != 5; ++mode) {
            SCOPED_TRACE(replace);
            SCOPED_TRACE(mode);
            auto config = CanonicalActivationConfig();
            config.maxPackDecodeStagingBytes = 64u * 1024u;
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            auto* engine = session.Engine();
            ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
            if (replace) {
                ASSERT_EQ(VN_OK, ActivateInitPathV1(directory).result);
            }
            const auto publication = engine->packComposition.PublishedHandle();
            const auto loadedCount = engine->packs.LoadedCount();
            ReentrantSetProvider provider;
            provider.invalidSpan = mode == 1;
            provider.failAcquire = mode == 2;
            if (mode == 3) provider.bytes.resize(8);
            if (mode == 4) provider.bytes.resize(128u * 1024u);
            const auto samples = harkentest::MakeDC(48000, 0.5f);
            provider.pcm = harkentest::MonoDesc(samples, 48000);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &provider.pcm));
            ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(17), &provider.profile));
            ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(50), VN_BUS_MASTER));
            auto play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &provider.voice));
            VnPackLoadDesc load{};
            load.structSize = sizeof(load);
            load.flags = VN_PACK_LOAD_FLAG_PROVIDER;
            load.providerUserData = &provider;
            load.provide = ReentrantSetProvider::Acquire;
            load.release = ReentrantSetProvider::Release;
            VnPackHandle handle = 0;
            const auto* desc = reinterpret_cast<const VnPackDesc*>(&load);
            const auto result = replace ? Vn_ReplacePack(UINT64_MAX, desc)
                                        : Vn_LoadPack(desc, &handle);
            const VnResult expected[] = {VN_PACK_INIT_LIFECYCLE, VN_PACK_PROVIDER_FAILED,
                VN_PACK_PROVIDER_FAILED, VN_BAD_PACK, VN_PACK_RESIDENT_CAPACITY};
            EXPECT_EQ(expected[mode], result);
            EXPECT_EQ(1u, provider.acquired);
            EXPECT_EQ(mode == 2 ? 0u : 1u, provider.released);
            EXPECT_EQ(0u, handle);
            ASSERT_EQ(engine, harken::detail::ActiveEngine());
            EXPECT_EQ(publication, engine->packComposition.PublishedHandle());
            EXPECT_EQ(loadedCount, engine->packs.LoadedCount());
            EXPECT_FALSE(engine->canonicalPackSetBusy);
            ASSERT_NE(nullptr, engine->trace);
            EXPECT_TRUE(engine->trace->Active());
            EXPECT_EQ(VN_OK, Vn_TraceStop());
            EXPECT_EQ(VN_OK, Vn_Update());
            EXPECT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(444), VN_BUS_MASTER));
        }
    }
}

TEST(CanonicalPackSetActivate, SameRevisionWrongExpectedHandleIsNotExactRetry) {
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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

TEST(CanonicalPackSetActivate, RetiredRequirementVintageIsRejected) {
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    using namespace harkentest::packfoundation;
    auto payload = RequirementPayload(kInitPackStableId, 1, 1, 0, 0);
    ImageBuilder builder(harken::kPackKindInit);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
                     std::move(payload), 1u);
    const auto activation = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-retired-requirement",
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
    EXPECT_EQ(VN_PACK_INCOMPATIBLE, Vn_InspectInitPack(&load, &report));
    EXPECT_EQ(report.packStableId, 0u);
    VnPackHandle handle = 0;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    VnConfig config = CanonicalActivationConfig(1, kBlock, kRate);
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
        harkentest::MakePlayDesc3D(sound, 0.0f, 0.0f, -50.5f, 0.0f);
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
    const auto before = engine->mixer.BlocksRendered();
    harkentest::Capture firstBlock;
    harkentest::PumpAndCapture(engine, 1, firstBlock);
    ASSERT_EQ(before + 1, engine->mixer.BlocksRendered());
    ASSERT_EQ(kBlock, firstBlock.left.size());
    for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
        EXPECT_NEAR(0.25f, firstBlock.left[frame], 1e-5f) << frame;
        EXPECT_NEAR(0.25f, firstBlock.right[frame], 1e-5f) << frame;
    }
    harken::Spatializer::SpatialVoiceState spatial{};
    ASSERT_TRUE(engine->spatial.CaptureVoice(
        harkentest::RenderHandle(engine, incoming), &spatial));
    EXPECT_EQ(atten, spatial.attenuationId);
    EXPECT_EQ(1u, spatial.attenuationGeneration);
    EXPECT_EQ(VN_OK, engine->spatial.BindVoice(
                         harkentest::RenderHandle(engine, incoming), atten, 1));
    ASSERT_EQ(VN_OK, Vn_StopAll(0));
    for (unsigned step = 0; step < 4; ++step) {
        engine->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(nullptr, engine->spatial.FindProfile(atten, 1));
}

TEST(CanonicalPackSetActivate, ContentWrongInitTripleIsStaleRevision) {
    harkentest::EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-stale-triple";
    const auto bus = harkentest::packfoundation::DefaultRuntimeBusFixture();
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
    harkentest::EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float kLevelDb = -6.0f;
    const auto first = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-send",
        harkentest::BuildMasterSendInitBytes(kLevelDb),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    auto* engine = session.Engine();
    const auto source = engine->buses.FindIndex(VN_OBJECT_ID(0, 2));
    const auto target = engine->buses.FindIndex(VN_OBJECT_ID(0, 3));
    ASSERT_NE(harken::kInvalidBusIndex, source);
    ASSERT_NE(harken::kInvalidBusIndex, target);
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
            if (plan.sendTargets[flat] != target || plan.sendLevelApply[flat] == 0) {
                continue;
            }
            EXPECT_NEAR(expected, plan.preparedSendLevels[flat], 1.0e-5f);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(CanonicalPackSetActivate, SnapshotSendPreservesForeignLiveSend) {
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    const auto packTarget = engine->buses.FindIndex(VN_OBJECT_ID(0, 3));
    ASSERT_NE(harken::kInvalidBusIndex, packTarget);
    bool foundPackSend = false;
    for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
        const auto flat = static_cast<std::size_t>(packSource) * sendSlots + slot;
        if (engine->busSendAdj[flat] != packTarget) continue;
        EXPECT_EQ(first.initHandle, engine->busSendOwnerCookies[flat]);
        EXPECT_FLOAT_EQ(-6.0f, engine->busSendLevelDb[flat]);
        EXPECT_EQ(VN_IN_USE, Vn_SetBusSendLevel(VN_OBJECT_ID(0, 2),
            VN_OBJECT_ID(0, 3), -12.0f, 0));
        EXPECT_EQ(VN_IN_USE, Vn_DisconnectBusSend(VN_OBJECT_ID(0, 2),
            VN_OBJECT_ID(0, 3)));
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
    harkentest::EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-foreign-cycle";
    const auto initialBus = harkentest::packfoundation::MasterSendRuntimeBusFixture(0.0f, 0);
    const auto first = harkentest::ActivateInitBytesV1(directory,
        harkentest::BuildMasterOnlyInitBytes(
            harkentest::packfoundation::kInitPackStableId, initialBus),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    const auto foreignBus = VN_OBJECT_ID(0, 3);
    const auto packBus = VN_OBJECT_ID(0, 2);
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
    const auto activate = [&](VnCanonicalPackSetActivationReportV1* report) -> VnResult {
        const auto fixture = harkentest::packfoundation::MasterSendRuntimeBusFixture(
            -6.0f);
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
    EXPECT_EQ(VN_BAD_PACK, activate(&refused));
    EXPECT_EQ(beforePublication, engine->packComposition.PublishedHandle());
    EXPECT_EQ(beforeRevision, engine->acceptedBusTopologyRevision);
    EXPECT_EQ(packSlot, engine->busSendAdj[foreignFlat]);
    EXPECT_FLOAT_EQ(-9.0f, engine->busSendLevelDb[foreignFlat]);
    EXPECT_EQ(0u, engine->busSendOwnerCookies[foreignFlat]);
    ASSERT_EQ(VN_OK, Vn_DisconnectBusSend(foreignBus, packBus));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    VnCanonicalPackSetActivationReportV1 accepted{};
    ASSERT_EQ(VN_OK, activate(&accepted));
    EXPECT_EQ(first.report.resultSetRevision + 1, accepted.resultSetRevision);
    EXPECT_EQ(harken::kInvalidBusIndex, engine->busSendAdj[foreignFlat]);
    EXPECT_EQ(0u, engine->busSendOwnerCookies[foreignFlat]);
}

TEST(CanonicalPackSetActivate, ForeignSendConsumesPackSourceCapacityWithoutMutation) {
    auto config = CanonicalActivationConfig();
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
    EXPECT_EQ(engine->buses.FindIndex(VN_OBJECT_ID(0, 3)),
              engine->busSendAdj[foreignFlat]);
    EXPECT_EQ(report.initHandle, engine->busSendOwnerCookies[foreignFlat]);
}

TEST(CanonicalPackSetActivate, DuplicateSnapshotSendIsBadPack) {
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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

TEST(CanonicalPackSetActivate, TraceActiveActivationAndUnloadRemainAvailable) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-trace-active";
    EngineSession session(CanonicalActivationConfig());
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
    unload.expectedInitHandle = activated.initHandle;
    unload.baseSetRevision = 1;
    unload.unloadRevision = 2;
    EXPECT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(nullptr, PackMasterHolder(session.Engine()));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
}

TEST(CanonicalPackSetActivate, TraceStartActivateWritesCall135) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-pack-set-activate-journal";
    EngineSession session(CanonicalActivationConfig());
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

int ReplayCompanionTrace(const std::filesystem::path& tracePath, std::string& output) {
    std::string command = std::string("\"") + HARKEN_HOST_PATH +
        "\" trace-replay \"" + tracePath.string() + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
    FILE* pipe = _popen(command.c_str(), "rt");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) return -1;
    char chunk[512];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) output += chunk;
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

TEST(CanonicalPackSetActivate, CompanionTraceOwnsInputsAndReplaysRenameReplacementAndRefusals) {
    using namespace companionfixture;
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "companion-trace";
    const auto tracePath = directory / "session.vtrc";
    const auto contentPath = (directory / "Play_A.bytes").string();
    std::filesystem::create_directories(directory);
    EngineSession session(CanonicalActivationConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.string().c_str()));
    const auto initId = PackId("Init");
    const auto contentId = PackId("action_" + harken::ObjectIdString(action));
    Provider init{Build(true, initId)};
    ASSERT_TRUE(harkentest::WriteBytes(contentPath, Build(false, contentId)));
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    auto content = Member(contentId, false, nullptr, &names);
    content.source.utf8Path = contentPath.c_str();
    std::vector<VnCanonicalPackSetMemberV2> members{Member(initId, true, &init, &initNames), content};
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(members, report));
    members[0] = Member(initId, true, nullptr, nullptr, report.initHandle, VN_PACK_MEMBER_KEEP);
    VnActionResolved old{};
    ASSERT_EQ(VN_OK, Resolve("Play_A", &old));
    VnVariableWriteV1 write{harkentest::packfoundation::kVariableId, 0.75f, 0};
    VnVariableBatchDescV1 batch{sizeof(batch), 1, 0, 0, &write};
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
    names.utf8RootName = "Play_Renamed";
    members[1].operation = VN_PACK_MEMBER_NAME_ONLY;
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, Activate(members, report, 1));
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    ASSERT_EQ(VN_OK, Activate(members, report, 1));
    ASSERT_EQ(VN_OK, Activate(members, report, 1));
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_STALE_HANDLE, Vn_PostActionResolved(&old, 0, 0, &firing));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
    names.utf8Language = "English";
    EXPECT_EQ(VN_BAD_PACK, Activate(members, report, 2));
    names.utf8Language = "";
    ASSERT_TRUE(harkentest::WriteBytes(contentPath, Build(false, contentId, 2)));
    members[1].operation = VN_PACK_MEMBER_CONTENT;
    ASSERT_EQ(VN_OK, Activate(members, report, 2));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(members[1].source.expectedHandle));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(members[1].source.expectedHandle));
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = report.initHandle;
    unload.baseSetRevision = session.Engine()->packComposition.Published()->ledger.setRevision;
    unload.unloadRevision = unload.baseSetRevision + 1;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    // Only the recorded immutable inputs remain usable.
    ASSERT_TRUE(harkentest::WriteBytes(contentPath, {0}));
    init.bytes.clear();
    std::ifstream stream(tracePath, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    std::size_t resourceCount = 0, firstResource = 0;
    for (std::size_t offset = ReadTraceU32(bytes, 8); offset + harken::kTraceRecordHeaderBytes <= bytes.size();) {
        const auto size = ReadTraceU32(bytes, offset);
        const auto type = bytes[offset + 4] | (static_cast<unsigned>(bytes[offset + 5]) << 8);
        if (type == harken::kTraceRecordPackBytes) {
            ++resourceCount;
            if (firstResource == 0) firstResource = offset + harken::kTraceRecordHeaderBytes;
        }
        offset += harken::kTraceRecordHeaderBytes + size;
    }
    EXPECT_EQ(3u, resourceCount);
    std::string output;
    EXPECT_EQ(0, ReplayCompanionTrace(tracePath, output)) << output;
    ASSERT_NE(0u, firstResource);
    bytes[firstResource + 8] ^= 1;
    const auto corrupt = directory / "corrupt.vtrc";
    ASSERT_TRUE(harkentest::WriteBytes(corrupt, bytes));
    output.clear();
    EXPECT_NE(0, ReplayCompanionTrace(corrupt, output)) << output;
}

TEST(CanonicalPackSetActivate, CompanionV3WithoutExternalMediaKeepsTraceReplayable) {
    using namespace companionfixture;
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "companion-v3-trace";
    std::filesystem::create_directories(directory);
    const auto tracePath = directory / "session.vtrc";
    EngineSession session(CanonicalActivationConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto initId = PackId("Init");
    const auto contentId = PackId("action_" + harken::ObjectIdString(action));
    Provider init{Build(true, initId)}, content{Build(false, contentId)};
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    VnPackStreamResolverV1 unused{sizeof(unused), 0, nullptr,
        +[](void*) -> VnResult { ADD_FAILURE(); return VN_DEVICE_ERROR; },
        +[](void*) { ADD_FAILURE(); },
        +[](void*, std::uint64_t, const char*, std::uint64_t, std::uint64_t,
            const char**) -> VnResult { ADD_FAILURE(); return VN_DEVICE_ERROR; }, nullptr};
    VnCanonicalPackSetMemberV3 members[]{
        {sizeof(VnCanonicalPackSetMemberV3), 0, Member(initId, true, &init, &initNames), nullptr},
        {sizeof(VnCanonicalPackSetMemberV3), 0, Member(contentId, false, &content, &names), &unused}};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (auto& row : rows) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationDescV3 desc{};
    desc.structSize = sizeof(desc); desc.setRevision = 1;
    desc.memberCount = desc.outputMemberCapacity = 2;
    desc.memberStructSize = sizeof(members[0]); desc.members = members;
    desc.outputMemberStructSize = sizeof(rows[0]); desc.outputMembers = rows;
    VnCanonicalPackSetActivationReportV1 report{}; report.structSize = sizeof(report);
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.string().c_str()));
    ASSERT_EQ(VN_OK, Vn_ActivateCanonicalPackSetV3(&desc, &report));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    std::string output;
    EXPECT_EQ(0, ReplayCompanionTrace(tracePath, output)) << output;
}

TEST(CanonicalPackSetActivate, CompanionTraceRechecksBadBytesBeforeUnreadMembers) {
    using namespace companionfixture;
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "companion-trace-bad-bytes";
    std::filesystem::create_directories(directory);
    const auto trace = directory / "bad-bytes.vtrc";
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_TraceStart(trace.string().c_str()));
    const auto initId = PackId("Init");
    const auto contentId = PackId("action_" + harken::ObjectIdString(action));
    Provider init{Build(true, initId)}, bad{Build(false, contentId)}, unread;
    bad.bytes[0] ^= 1;
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    std::vector<VnCanonicalPackSetMemberV2> members{Member(initId, true, &init, &initNames)};
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(members, report));
    members[0] = Member(initId, true, nullptr, nullptr, report.initHandle, VN_PACK_MEMBER_KEEP);
    members.push_back(Member(contentId, false, &bad, &names));
    members.push_back(Member(contentId + 1, false, &unread, &names));
    EXPECT_EQ(VN_BAD_PACK, Activate(members, report, 1));
    EXPECT_EQ(1u, bad.reads);
    EXPECT_EQ(1u, bad.releases);
    EXPECT_EQ(0u, unread.reads);
    EXPECT_EQ(1u, session.Engine()->packComposition.Published()->ledger.setRevision);
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    std::string output;
    EXPECT_EQ(0, ReplayCompanionTrace(trace, output)) << output;

    // A different recorded result must be rejected by replaying the captured bad bytes.
    std::ifstream stream(trace, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    std::size_t resultOffset = 0;
    for (std::size_t offset = ReadTraceU32(bytes, 8); offset + harken::kTraceRecordHeaderBytes <= bytes.size();) {
        const auto size = ReadTraceU32(bytes, offset);
        const auto type = bytes[offset + 4] | (static_cast<unsigned>(bytes[offset + 5]) << 8);
        const auto payload = offset + harken::kTraceRecordHeaderBytes;
        ASSERT_LE(payload + size, bytes.size());
        if (type == harken::kTraceRecordApiCall &&
            ReadTraceU32(bytes, payload) == harken::kTraceCallActivateCanonicalPackSetV2 &&
            size >= 40 && ReadTraceU32(bytes, payload + size - 40) == static_cast<std::uint32_t>(VN_BAD_PACK))
            resultOffset = payload + size - 40;
        offset = payload + size;
    }
    ASSERT_NE(0u, resultOffset);
    const auto changedResult = static_cast<std::uint32_t>(VN_PACK_PROVIDER_FAILED);
    for (unsigned i = 0; i < 4; ++i) bytes[resultOffset + i] = static_cast<std::uint8_t>(changedResult >> (i * 8));
    const auto changed = directory / "changed-result.vtrc";
    ASSERT_TRUE(harkentest::WriteBytes(changed, bytes));
    output.clear();
    EXPECT_NE(0, ReplayCompanionTrace(changed, output)) << output;
}

TEST(CanonicalPackSetActivate, CompanionTraceReplaysEmbeddedMediaAfterSourceLoss) {
    using namespace companionfixture;
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "companion-trace-media";
    std::filesystem::create_directories(directory);
    const auto trace = directory / "media.vtrc";
    EngineSession session(CanonicalActivationConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_TraceStart(trace.string().c_str()));
    const auto initId = PackId("Init");
    const auto contentId = PackId("action_" + harken::ObjectIdString(action));
    Provider init{Build(true, initId)}, content{BuildPreview(false)};
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    std::vector<VnCanonicalPackSetMemberV2> members{
        Member(initId, true, &init, &initNames), Member(contentId, false, &content, &names)};
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(members, report));
    VnActionResolved resolved{};
    ASSERT_EQ(VN_OK, Resolve("Play_A", &resolved));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostActionResolved(&resolved, 0, 0, &firing));
    for (unsigned i = 0; i < 4; ++i) {
        session.Engine()->nullBackend.Pump(4);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    init.bytes.clear();
    content.bytes.clear();
    std::string output;
    EXPECT_EQ(0, ReplayCompanionTrace(trace, output)) << output;
}

TEST(CanonicalPackSetActivate, CompanionTraceFailureDoesNotChangeActivationResult) {
    using namespace companionfixture;
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto tracePath = std::filesystem::path(HARKEN_TEMP_DIR) / "companion-trace-failure.vtrc";
    ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.string().c_str()));
    const auto initId = PackId("Init");
    Provider init{Build(true, initId)};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    std::vector<VnCanonicalPackSetMemberV2> members{Member(initId, true, &init, &names)};
    VnCanonicalPackSetActivationReportV1 report{};
    harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::Append);
    const auto result = Activate(members, report);
    harken::SetTraceRecorderFaultForTests(harken::TraceRecorderFault::None);
    EXPECT_EQ(VN_OK, result);
    EXPECT_NE(0u, report.initHandle);
    EXPECT_TRUE(session.Engine()->traceFailedSticky);
    EXPECT_EQ(report.initHandle, session.Engine()->packComposition.Published()->initHandle);
}

TEST(CanonicalPackSetActivate, HostReplaysSuccessfulActivateCall135) {
    const std::filesystem::path directory =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-pack-set-activate-host-replay";
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-bus2";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixture()),
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

TEST(CanonicalPackSetActivate, AllocationAfterBindingPreparationRollsBackAndRetries) {
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) /
                           "canonical-binding-allocation-failure";
    const auto bytes = harkentest::BuildMasterInitWithBusBindingBytes(
        kBusBindingId, kBindingVariableId, VN_BUS_MASTER);
    const auto baselineWorstCase = engine->propertyPublisher.WorstCaseCommits();
    const auto dependencySlot = engine->bindings.Dependencies().ParamBindingCapacity();
    allocation_probe::count = 0;
    allocation_probe::failAt = 1;
    allocation_probe::countContext = engine;
    allocation_probe::countIf = +[](void* context) noexcept {
        const auto* current = static_cast<harken::Engine*>(context);
        return current->bindings.Dependencies().HasBindingPreparation(
            current->bindings.Dependencies().ParamBindingCapacity());
    };
    allocation_probe::enabled = true;
    const auto failed = harkentest::ActivateInitBytesV1(
        directory, bytes, harkentest::packfoundation::kInitPackStableId);
    allocation_probe::enabled = false;
    allocation_probe::countIf = nullptr;
    allocation_probe::countContext = nullptr;
    const bool faultConsumed = allocation_probe::failAt == 0;
    allocation_probe::failAt = 0;
    EXPECT_TRUE(faultConsumed);
    EXPECT_NE(VN_OK, failed.result);
    EXPECT_EQ(0u, failed.initHandle);
    EXPECT_EQ(nullptr, engine->bindings.Find(kBusBindingId));
    EXPECT_FALSE(engine->bindings.Dependencies().HasBindingPreparation(dependencySlot));
    EXPECT_EQ(baselineWorstCase, engine->propertyPublisher.WorstCaseCommits());
    const auto retry = harkentest::ActivateInitBytesV1(
        directory, bytes, harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, retry.result);
    EXPECT_TRUE(engine->bindings.HasOwnedAttachment(
        kBusBindingId, VN_TARGET_SCOPE_BUS, VN_BUS_MASTER, retry.initHandle));
}

TEST(CanonicalPackSetActivate, AfterAllPreparedBeforeCommitRollsBackBindings) {
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-replace";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixture()),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    const auto unchanged = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixture()),
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
            harkentest::packfoundation::Bus2RuntimeBusFixture()),
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
    EXPECT_EQ(first.initHandle, live->ownerCookie);
    EXPECT_EQ(20u, live->transitionMs);
    ASSERT_EQ(VN_OK, Vn_Update());
    live = session.Engine()->bindings.Find(kBusBindingId);
    ASSERT_NE(nullptr, live);
    EXPECT_EQ(20u, live->transitionMs);

    ASSERT_TRUE(harkentest::WriteBytes(directory / "Init.pack",
                                      harkentest::BuildMasterBus2InitBytes()));
    member.utf8Path = first.initPath.c_str();
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
                                                          first.initHandle, 2));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kBusBindingId));
}

TEST(CanonicalPackSetActivate, CallerTakenCookieIsSkippedOnUnload) {
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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


TEST(CanonicalPackSetActivate, InitNewBusLimiterBindingUsesProspectiveEffect) {
    EngineSession session(CanonicalActivationConfig(4, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-limiter-new-bus-binding";
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = kBusBindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attachment.objectId = VN_OBJECT_ID(0, 2);
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = kBindingVariableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    desc.targetId = harken::PackEffectParamLocator(VN_EFFECT_LIMITER, 7, 0);
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {-1.0f, -6.0f};
    desc.curve.points[1] = {1.0f, -6.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    const auto bytes = harkentest::WithCanonicalBindingRows(
        CanonicalLimiterInit(-18, false, 1, true), kBusBindingId, desc, &attachment);
    const auto activated = harkentest::ActivateInitBytesV1(directory, bytes,
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, activated.result);
    EXPECT_TRUE(engine.bindings.HasOwnedAttachment(kBusBindingId, VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
        attachment.objectId, activated.initHandle));
    engine.nullBackend.Pump(1);
    VnVariableWriteV1 write{kBindingVariableId, 0.5f, 0};
    VnVariableBatchDescV1 batch{sizeof(batch), 1, 0, 0, &write};
    VnVariableTicketV1 ticket{};
    ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(&engine));
    engine.nullBackend.Pump(1);
    const auto bus = engine.buses.FindIndex(attachment.objectId);
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    EXPECT_TRUE(engine.effectParamTargets.CanBind(harken::EffectParamTargetStore::Scope::Bus,
        bus, harken::UnpackEffectParamLocator(desc.targetId)));
    const auto sound = harkentest::ObjectId(0xEA21);
    const auto samples = harkentest::MakeDC(48000, 0.5f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    auto play = harkentest::MakePlayDesc(sound);
    play.busId = attachment.objectId;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    // Include the existing 10 ms effect-parameter transition before measuring the target.
    engine.nullBackend.Pump(16);
    EXPECT_NEAR(std::pow(10.0f, -24.0f / 20.0f), engine.nullBackend.LastBlock()[0], 2e-5f);
    const auto invalidPath = (directory / "MissingEffect.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(invalidPath, harkentest::WithCanonicalBindingRows(
        CanonicalLimiterInit(-18, false, 2, true, 0), kBusBindingId, desc, &attachment)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.utf8Path = invalidPath.c_str();
    member.expectedHandle = activated.initHandle;
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_INVALID_ARG, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        activated.initHandle, 1));
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    engine.nullBackend.Pump(1);
    EXPECT_NEAR(std::pow(10.0f, -24.0f / 20.0f), engine.nullBackend.LastBlock()[0], 2e-5f);
    const auto replacementPath = (directory / "BoundReplacement.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(replacementPath, harkentest::WithCanonicalBindingRows(
        CanonicalLimiterInit(-12, false, 2, true), kBusBindingId, desc, &attachment)));
    member.utf8Path = replacementPath.c_str();
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        activated.initHandle, 1));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(16);
    EXPECT_NEAR(std::pow(10.0f, -18.0f / 20.0f), engine.nullBackend.LastBlock()[0], 2e-5f);
    const auto secondHandle = report.initHandle;
    const auto secondRevision = report.resultSetRevision;
    ASSERT_TRUE(harkentest::WriteBytes(replacementPath, CanonicalLimiterInit(-9, false, 3, true)));
    member.expectedHandle = secondHandle;
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        secondHandle, secondRevision));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-9.0f, engine.effectParamTargets.AuthoredBase(
        harken::EffectParamTargetStore::Scope::Bus, bus, 7, 0));
    EXPECT_FLOAT_EQ(-9.0f, engine.effectParamTargets.LastCommitted(
        harken::EffectParamTargetStore::Scope::Bus, bus, 7, 0));
    // Retained detector gain needs the authored 100 ms release to reach the higher ceiling.
    engine.nullBackend.Pump(80);
    EXPECT_NEAR(std::pow(10.0f, -9.0f / 20.0f), engine.nullBackend.LastBlock()[0], 2e-5f);
    EXPECT_EQ(VN_OK, UnloadSet(report.initHandle, report.resultSetRevision));
}

TEST(CanonicalPackSetActivate, InitBusEffectParamIsNotKindRefused) {
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
        VnConfig config = CanonicalActivationConfig();
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
        VnConfig config = CanonicalActivationConfig();
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
        VnConfig config = CanonicalActivationConfig();
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
    for (const bool trace : {false, true}) {
    SCOPED_TRACE(trace);
    auto config = CanonicalActivationConfig();
    config.maxPackDecodeStagingBytes = 64u * 1024u;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-replace-content";
    std::filesystem::create_directories(directory);
    const auto tracePath = (directory / "replacement-budget.vntrace").string();
    if (trace) ASSERT_EQ(VN_OK, Vn_TraceStart(tracePath.c_str()));
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kVoicePlayableId, &pcm));
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
    const auto* publication = session.Engine()->packComposition.Published();
    companionfixture::Provider oversized;
    oversized.bytes.resize(4u * 1024u * 1024u);
    ASSERT_TRUE(harkentest::WriteBytes(directory / "Content.pack", oversized.bytes));
    VnPackLoadDesc replace{};
    replace.structSize = sizeof(replace);
    replace.utf8Path = contentPath.c_str();
    for (const unsigned source : {0u, 1u}) {
        SCOPED_TRACE(source);
        if (source == 1) {
            replace.flags = VN_PACK_LOAD_FLAG_PROVIDER;
            replace.providerUserData = &oversized;
            replace.provide = companionfixture::Provider::Read;
            replace.release = companionfixture::Provider::Release;
        }
        allocation_probe::bytes = 0;
        allocation_probe::enabled = true;
        const auto result = Vn_ReplacePack(contentHandle, reinterpret_cast<const VnPackDesc*>(&replace));
        allocation_probe::enabled = false;
        const auto allocated = allocation_probe::bytes;
        EXPECT_EQ(source == 0 && !trace ? VN_BAD_PACK : VN_PACK_RESIDENT_CAPACITY, result);
        EXPECT_LT(allocated, oversized.bytes.size());
        EXPECT_EQ(publication, session.Engine()->packComposition.Published());
        EXPECT_EQ(2u, session.Engine()->packs.LoadedCount());
    }
    EXPECT_EQ(1u, oversized.reads);
    EXPECT_EQ(1u, oversized.releases);
    replace.flags = 0;
    replace.providerUserData = nullptr;
    replace.provide = nullptr;
    replace.release = nullptr;
    const auto nextId = harkentest::ObjectId(8404);
    ASSERT_TRUE(harkentest::WriteBytes(
        directory / "Content.pack",
        harkentest::BuildContentWithVoiceBindingBytes(nextId, kBindingVariableId,
                                                     kVoicePlayableId)));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(contentHandle, reinterpret_cast<const VnPackDesc*>(&replace)));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(kVoiceBindingId));
    EXPECT_EQ(nullptr, session.Engine()->bindings.Find(nextId));
    const auto* contentOwner = OwnerForHandle(session.Engine(), contentHandle);
    ASSERT_NE(nullptr, contentOwner);
    ASSERT_NE(nullptr, contentOwner->voiceBindings);
    ASSERT_EQ(1u, contentOwner->voiceBindings->size());
    EXPECT_EQ(nextId, contentOwner->voiceBindings->front().bindingId);
    if (trace) EXPECT_EQ(VN_OK, Vn_TraceStop());
    }
}

TEST(CanonicalPackSetActivate, UnloadPackOnInitLeavesCanonicalBindings) {
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-drop-bus2";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixture()),
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
    EngineSession session(CanonicalActivationConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-binding-keep-bus2-row";
    const auto first = harkentest::ActivateInitBytesV1(
        directory,
        harkentest::BuildMasterInitWithBusBindingBytes(
            kBusBindingId, kBindingVariableId, VN_OBJECT_ID(0, 2),
            harkentest::packfoundation::Bus2RuntimeBusFixture()),
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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

namespace packstream {

using namespace companionfixture;

constexpr VnObjectId kSound = harkentest::ObjectId(8300);
constexpr std::uint64_t kFrames = 2 * harken::kPackMediaRangeChunkFrames + 123;

// Stereo planar signal with both signed zeros; the length is not chunk aligned.
std::vector<float> PlanarSignal(float scale) {
    std::vector<float> samples(2 * kFrames);
    for (std::uint64_t frame = 0; frame < kFrames; ++frame) {
        const auto value = scale * static_cast<float>(static_cast<int>(frame % 199) - 99) / 128.0f;
        samples[frame] = value;
        samples[kFrames + frame] = -0.5f * value;
    }
    samples[99] = -0.0f;
    samples[kFrames + 298] = 0.0f;
    return samples;
}

// Content pack whose action plays one Sound backed by its own stereo media.
std::vector<std::uint8_t> BuildSoundContent(std::uint64_t id, std::uint64_t revision,
                                            bool streamed, const std::vector<float>& samples,
                                            std::uint64_t providerId = 0, bool leadingMedia = false,
                                            std::uint64_t encodedRevision = 0) {
    using namespace harken;
    using namespace harkentest::packfoundation;
    const auto bus = DefaultRuntimeBusFixture();
    const auto frames = samples.size() / 2;
    const float* planes[] = {samples.data(), samples.data() + frames};
    std::uint64_t mediaRevision = 0;
    EXPECT_TRUE(ComputeMediaPcmContentRevisionV1(48000, 2, frames, planes, 1, &mediaRevision));
    if (encodedRevision) mediaRevision = encodedRevision;
    PlayablePropertyProgramViewV1 property{};
    property.stableId = kSound;
    property.contentPropertyKind = playableproperties::ContentPropertyKindV1::Sound;
    property.contentRevision = mediaRevision;
    property.routeStableId = VN_BUS_MASTER;
    property.compatibilityFingerprint = 1;
    property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1 | 1u;
    property.payload.values[3].bits = 1;
    property.payload.values[6].objectId = VN_BUS_MASTER;
    property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    property.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    if (streamed) {
        constexpr auto slot = static_cast<std::size_t>(playableproperties::PlayablePropertyIdV1::StreamingMode) - 1;
        property.payload.presentMask |= UINT64_C(1) << slot;
        property.payload.values[slot].bits = 1;
    }
    property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
    ActionStepProgramViewV1 step{};
    step.target.kind = ActionTargetKindV1::Sound;
    step.target.stableId = kSound;
    step.target.expectedProgramSemanticFingerprint = property.programSemanticFingerprint;
    SetAbsoluteTime(&step.delay);
    SetAbsoluteTime(&step.fade);
    SetAbsoluteTime(&step.cooldown);
    ActionProgramViewV1 program{};
    program.actionStableId = action;
    program.steps = &step;
    program.stepCount = 1;
    program.expandedStepOccurrenceCount = 1;
    program.expandedFiringCount = 1;
    program.programSemanticFingerprint = ComputeActionProgramFingerprintV1(program);
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = project;
    world.identity.capabilityMask = kWorldCapabilityActionDefinitions | kWorldCapabilityPlayableProperties;
    world.actions = &program;
    world.actionCount = 1;
    world.playableProperties = &property;
    world.playablePropertyCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = id;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = 2;
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeader(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, partition.data());
    std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
    ImageBuilder image(kPackKindCanonicalContent);
    image.AddEntry(kPackEntryKindPackRequirementBlock, id,
        RequirementPayload(id, revision, revision, header.ownedDefinitionCount, body.size(), bus.triple));
    image.AddEntry(kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
    image.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(PackId("Init"), 1, bus.triple), kPackPackDependencyEdgeVintage);
    // An unrelated media blob ahead of the Sound's moves its file offset.
    if (leadingMedia) image.AddMedia(harkentest::ObjectId(8399), std::vector<float>(4096, 0.125f));
    if (providerId == 0) {
        image.AddMedia(kSound, samples, {}, 2);
        return image.Build();
    }
    PackDependencyEdge edge;
    edge.prerequisitePackStableId = providerId;
    edge.edgeKind = kPackDependencyEdgeKindContent;
    edge.required01 = 1;
    std::vector<std::uint8_t> payload(kPackDependencyEdgeBytes, 0);
    SerializePackDependencyEdge(edge, payload.data());
    const auto index = image.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
                                      std::move(payload), kPackPackDependencyEdgeVintage);
    image.SetRelationOrdinal(index, 1);
    return image.Build();
}

// Content pack carrying only the stereo media; its Sound is defined by a
// member that declares a required content edge to this pack.
std::vector<std::uint8_t> BuildMediaProvider(std::uint64_t id, const std::vector<float>& samples) {
    using namespace harkentest::packfoundation;
    const auto bus = DefaultRuntimeBusFixture();
    std::uint64_t size = 0;
    auto partition = PartitionPayload(id, 1, 0, 0, &size, 64, project, bus.triple);
    ImageBuilder image(harken::kPackKindCanonicalContent);
    image.AddEntry(harken::kPackEntryKindPackRequirementBlock, id,
        RequirementPayload(id, 1, 1, 0, size, bus.triple));
    image.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
    image.AddEntry(harken::kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(PackId("Init"), 1, bus.triple), harken::kPackPackDependencyEdgeVintage);
    image.AddMedia(kSound, samples, {}, 2);
    return image.Build();
}

struct PathSet {
    std::filesystem::path directory;
    std::string init;
    std::string content;
    VnPackNamesV1 initNames{sizeof(VnPackNamesV1), 0, project, {}, "", "", 0, 0, nullptr, nullptr};
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, project, action, "Play_A", "", 0, 0, nullptr, nullptr};
    std::vector<VnCanonicalPackSetMemberV2> members;

    explicit PathSet(const char* name)
        : directory(std::filesystem::path(HARKEN_TEMP_DIR) / name) {
        std::filesystem::create_directories(directory);
        init = (directory / "Init.pack").string();
        content = (directory / "Content.pack").string();
        EXPECT_TRUE(harkentest::WriteBytes(init, Build(true, PackId("Init"))));
        members = {Member(PackId("Init"), true, nullptr, &initNames),
                   Member(ContentId(), false, nullptr, &names)};
        members[0].source.utf8Path = init.c_str();
        members[1].source.utf8Path = content.c_str();
    }
    static std::uint64_t ContentId() { return PackId("action_" + harken::ObjectIdString(action)); }
};

std::uint64_t Audible(const harkentest::Capture& capture) {
    for (std::size_t i = 0; i < capture.left.size(); ++i)
        if (capture.left[i] != 0.0f || capture.right[i] != 0.0f) return i;
    return capture.left.size();
}

void PumpFiring(harken::Engine* engine, std::uint32_t blocks, harkentest::Capture& capture) {
    for (std::uint32_t block = 0; block < blocks; ++block) {
        engine->streams.SyncServe();
        harkentest::PumpAndCapture(engine, 1, capture);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
}

harkentest::Capture PostAndCapture(harken::Engine* engine, std::uint32_t blocks) {
    harkentest::Capture capture;
    VnPostActionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.actionId = action;
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_OK, Vn_PostAction(&desc, &firing));
    PumpFiring(engine, blocks, capture);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    return capture;
}

VnConfig StreamConfig() {
    auto config = CanonicalActivationConfig(8, 256, 48000);
    config.prebufferMs = 1;
    return config;
}

constexpr std::uint32_t kBlocks = static_cast<std::uint32_t>(kFrames / 256 + 8);

// The audible sample sequence, aligned at its first non-silent frame.
void ExpectSameAudible(const harkentest::Capture& expected, const harkentest::Capture& actual,
                       std::uint64_t frames) {
    const auto first = Audible(expected), second = Audible(actual);
    ASSERT_LE(first + frames, expected.left.size());
    ASSERT_LE(second + frames, actual.left.size());
    for (std::uint64_t i = 0; i < frames; ++i) {
        ASSERT_EQ(expected.left[first + i], actual.left[second + i]) << i;
        ASSERT_EQ(expected.right[first + i], actual.right[second + i]) << i;
    }
}

harkentest::Capture ResidentReference(const char* name, const std::vector<float>& samples) {
    EngineSession session(StreamConfig());
    EXPECT_EQ(VN_OK, session.InitResult());
    PathSet set(name);
    EXPECT_TRUE(harkentest::WriteBytes(set.content,
        BuildSoundContent(PathSet::ContentId(), 1, false, samples)));
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_OK, Activate(set.members, report));
    EXPECT_TRUE(session.Engine()->registry.Lookup(kSound).Valid());
    return PostAndCapture(session.Engine(), kBlocks);
}

} // namespace packstream

TEST(CanonicalPackSetActivate, TypedVorbisExternalAndResidentUseCanonicalActionClosure) {
    using namespace packstream;
    struct Locator final : harken::StreamByteLocator {
        std::string path;
        std::atomic<bool> enabled{false};
        mutable std::atomic<unsigned> attempts{0}, cancellations{0};
        mutable std::atomic<unsigned> opens{0};
        std::shared_ptr<const harken::StreamByteSource> Open(harken::StreamReadCancellation cancel) const override {
            attempts.fetch_add(1);
            while (!enabled.load() && !cancel.IsCancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (cancel.IsCancelled()) { cancellations.fetch_add(1); return {}; }
            opens.fetch_add(1);
            return harken::OpenStreamByteSource(path.c_str());
        }
    };
    const auto fixture = harken::test::MakeVorbisFixture(2, true);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    std::vector<float> planar(fixture.reference.size());
    for (std::size_t i = 0; i < media.frames; ++i) {
        planar[i] = fixture.reference[2 * i];
        planar[media.frames + i] = fixture.reference[2 * i + 1];
    }
    const auto externalBytes = harken::test::WrapVorbisStream(fixture.media);
    harkentest::StreamSpec pcmSpec;
    pcmSpec.soundId = kSound; pcmSpec.channelCount = 2; pcmSpec.chunkFrames = 4096;
    const auto pcmBytes = harkentest::BuildStreamBytes(pcmSpec, planar);
    harken::StreamHeader pcmHeader;
    ASSERT_TRUE(harken::ParseStreamHeader(pcmBytes.data(), &pcmHeader));
    const auto pcmToc = harken::ParseStreamTocEntry(pcmBytes.data() + pcmHeader.tocOffset);
    std::vector<std::uint64_t> pcmChecksums(pcmToc.chunkCount);
    for (unsigned i = 0; i < pcmToc.chunkCount; ++i)
        pcmChecksums[i] = harken::LoadLE64(pcmBytes.data() + pcmToc.chunkChecksumOffset + 8 * i);
    harken::MediaStreamSemanticHeaderV1 pcmSemantic{pcmToc.sampleRate, pcmToc.channelCount, pcmToc.frameCount,
        pcmToc.sampleFormat, pcmHeader.formatVersion, pcmHeader.chunkFrames, pcmToc.chunkCount, 0, 0};
    std::uint64_t pcmRevision = 0;
    ASSERT_TRUE(harken::ComputeMediaStreamContentRevisionV1(pcmSemantic, pcmChecksums.data(), &pcmRevision));
    auto pcmExternalBytes = harken::test::WrapVorbisStream(pcmBytes);
    harken::StoreLE16(pcmExternalBytes.data() + 6, 0);
    for (const auto workers : {0u, 2u}) {
        harkentest::Capture resident;
        for (unsigned mode = 0; mode < 4; ++mode) {
            SCOPED_TRACE(::testing::Message() << workers << '/' << mode);
            auto config = StreamConfig();
            config.renderWorkerCount = workers;
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            auto& engine = *session.Engine();
            PathSet set("pack-vorbis-typed");
            VnPackHandle init = 0;
            ASSERT_EQ(VN_OK, harkentest::ActivateExistingInitPackV1(set.init.c_str(), PackId("Init"), &init));
            const auto content = BuildSoundContent(PathSet::ContentId(), 1, mode != 0, planar,
                0, false, mode == 3 ? pcmRevision : media.revision);
            ASSERT_TRUE(harkentest::WriteBytes(set.content, content));
            std::vector<harken::PCMRegistry::PreparedPackSamples> prepared(1);
            auto locator = std::make_shared<Locator>();
            locator->path = (set.directory / "Media.stream").string();
            ASSERT_TRUE(harkentest::WriteBytes(locator->path, mode == 3 ? pcmExternalBytes : externalBytes));
            if (mode == 0) {
                ASSERT_EQ(VN_OK, engine.registry.PrepareVorbisSamples(kSound, engine.vorbis,
                    fixture.media, 4 * 1024 * 1024, &prepared[0]));
            } else {
                auto external = std::make_shared<harken::PackExternalStream>();
                external->locator = locator;
                external->fileBytes = mode == 3 ? pcmExternalBytes.size() : externalBytes.size();
                if (mode == 3) {
                    external->codec = harken::PackExternalStream::Codec::Pcm;
                    external->pcmPrefix.assign(pcmBytes.begin(), pcmBytes.begin() + pcmToc.payloadOffset +
                        2 * harken::StreamChunkBytes(pcmHeader.chunkFrames, pcmToc.channelCount));
                }
                auto range = std::make_shared<harken::PackMediaRange>();
                range->external = external;
                range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
                range->sampleRate = media.sampleRate; range->channelCount = media.channels;
                range->frameCount = media.frames;
                range->contentRevision = mode == 3 ? pcmRevision : media.revision;
                if (mode == 1) {
                    ASSERT_GT(media.seek.size(), 2u);
                    const auto end = media.packetDataOffset + media.seek[2].packetOffset;
                    std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + end);
                    harken::VorbisPrepareResult result;
                    range->vorbis = harken::VorbisStreamMedia::Prepare(engine.vorbis, std::move(prefix),
                        fixture.media.size(), 4 * 1024 * 1024, result, true);
                    ASSERT_NE(nullptr, range->vorbis);
                }
                ASSERT_EQ(VN_OK, engine.registry.PreparePackRangeSamples(kSound, range, &prepared[0]));
            }
            VnPackLoadDesc load{};
            load.structSize = sizeof(load); load.utf8Path = set.content.c_str();
            VnPackHandle handle = 0;
            ASSERT_EQ(VN_OK, harken::LoadProvisionalCanonicalContentForTests(engine, &load, &handle, &prepared));
            EXPECT_EQ(0u, locator->opens.load());
            engine.streams.SetThrottled(true);
            VnPostActionDesc post{};
            post.structSize = sizeof(post); post.actionId = action;
            VnActionFiringHandle firing = 0;
            ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
            harkentest::Capture captured;
            VnResult supplyFailure = VN_OK;
            unsigned supplyTerminal = 0;
            const auto pump = [&](unsigned blocks) {
                for (unsigned block = 0; block < blocks; ++block) {
                    if (mode != 0 && locator->enabled.load() &&
                        (mode == 3 || engine.vorbis.states.ActiveBytes() != 0))
                        engine.streams.SyncServe();
                    const auto before = engine.mixer.BlocksRendered();
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    do {
                        engine.nullBackend.Pump(1);
                        if (engine.mixer.BlocksRendered() != before) break;
                        std::this_thread::yield();
                    } while (std::chrono::steady_clock::now() < deadline);
                    ASSERT_EQ(before + 1, engine.mixer.BlocksRendered());
                    const auto* output = engine.nullBackend.LastBlock();
                    for (unsigned i = 0; i < 256; ++i) {
                        captured.left.push_back(output[2 * i]);
                        captured.right.push_back(output[2 * i + 1]);
                    }
                    ASSERT_EQ(VN_OK, Vn_Update());
                    for (unsigned slot = 0; slot < engine.config.maxStreams; ++slot) {
                        const auto* ring = engine.streams.RingOf(slot);
                        if (ring->preparationResult.load() != VN_OK) supplyFailure = ring->preparationResult.load();
                        if (static_cast<unsigned>(ring->TerminalReason()) > supplyTerminal)
                            supplyTerminal = static_cast<unsigned>(ring->TerminalReason());
                    }
                    if (mode == 2 && locator->enabled.load() && engine.vorbis.states.ActiveBytes() == 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };
            pump(16);
            EXPECT_EQ(0u, locator->opens.load());
            if (mode == 2) EXPECT_EQ(captured.left.size(), Audible(captured));
            else EXPECT_LT(Audible(captured), captured.left.size());
            locator->enabled.store(true);
            engine.streams.SetThrottled(false);
            pump(static_cast<std::uint32_t>(media.frames / 256 + 64));
            VnActionFiringInfo info{};
            info.structSize = sizeof(info);
            ASSERT_EQ(VN_OK, Vn_QueryActionFiring(firing, &info));
            VnActionStepResult step{};
            step.structSize = sizeof(step);
            ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
            SCOPED_TRACE(::testing::Message() << "state=" << info.state << " outcome=" << info.outcome
                << " reason=" << info.terminalReason << " attempts=" << locator->attempts.load()
                << " cancelled=" << locator->cancellations.load() << " slots=" << engine.streams.FreeSlotCount()
                << " step=" << step.result << " supply=" << supplyFailure << " terminal=" << supplyTerminal);
            ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
            if (mode == 0) resident = std::move(captured);
            else {
                if (mode != 2) EXPECT_EQ(Audible(resident), Audible(captured));
                ExpectSameAudible(resident, captured, media.frames);
                EXPECT_GE(locator->opens.load(), 1u);
            }
        }
    }
}

TEST(CanonicalPackSetActivate, ExternalOpenOwnsLeaseBeforeValidationAndRetainsContextThroughRelease) {
    struct Context {
        unsigned mode = 0, retains = 0, releases = 0, opens = 0, closes = 0;
        std::atomic<bool> cancelled{false};
        static VnResult Retain(void* raw) { ++static_cast<Context*>(raw)->retains; return VN_OK; }
        static void Release(void* raw) { ++static_cast<Context*>(raw)->releases; }
        static void Close(void* raw) { ++static_cast<Context*>(raw)->closes; }
        static std::int32_t Read(void*, std::uint64_t, void* output, std::uint32_t count,
            std::uint32_t* read, const VnReadCancellationViewV1*) {
            std::memset(output, 0, count); *read = count; return VN_BYTE_READ_OK;
        }
        static VnResult Open(void* raw, std::uint64_t, const char*, std::uint64_t, std::uint64_t bytes,
            const VnReadCancellationViewV1*, VnByteSourceDescV1* source) {
            auto& self = *static_cast<Context*>(raw); ++self.opens;
            *source = {self.mode == 1 ? 0u : static_cast<unsigned>(sizeof(*source)), 0,
                bytes + (self.mode == 2 ? 1 : 0), raw, &Read, &Close};
            if (self.mode == 3) self.cancelled.store(true);
            return VN_OK;
        }
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
        Context context; context.mode = mode;
        VnPackStreamResolverV1 descriptor{sizeof(descriptor), 0, &context,
            &Context::Retain, &Context::Release, nullptr, &Context::Open};
        std::shared_ptr<const harken::PackStreamResolver> captured;
        ASSERT_EQ(VN_OK, harken::PackStreamResolver::Capture(descriptor, &captured));
        auto locator = harken::MakePackHostStreamLocator(captured, 1, "Media.stream", 2, 128);
        ASSERT_NE(nullptr, locator);
        auto source = locator->Open({&context.cancelled});
        EXPECT_EQ(mode == 0, source != nullptr);
        EXPECT_EQ(1u, context.opens);
        captured.reset(); locator.reset();
        EXPECT_EQ(mode == 0 ? 0u : 1u, context.closes);
        EXPECT_EQ(mode == 0 ? 0u : 1u, context.releases);
        source.reset();
        EXPECT_EQ(1u, context.retains); EXPECT_EQ(1u, context.releases); EXPECT_EQ(1u, context.closes);
    }
}

TEST(CanonicalPackSetActivate, ExternalResolverUsesRealTransactionAndRetainsExactGeneration) {
    using namespace packstream;
    struct Host {
        std::string path;
        std::atomic<unsigned> retains{0}, releases{0}, paths{0}, opens{0}, closes{0};
        bool failRetain = false;
        struct Source { Host* owner; std::shared_ptr<const harken::StreamByteSource> bytes; };
        static VnResult Retain(void* raw) {
            auto& host = *static_cast<Host*>(raw);
            if (host.failRetain) return VN_DEVICE_ERROR;
            ++host.retains; return VN_OK;
        }
        static void Release(void* raw) { ++static_cast<Host*>(raw)->releases; }
        static VnResult Path(void* raw, std::uint64_t pack, const char* locator,
            std::uint64_t revision, std::uint64_t bytes, const char** path) {
            auto& host = *static_cast<Host*>(raw);
            EXPECT_EQ(PathSet::ContentId(), pack); EXPECT_STREQ("Media.stream", locator);
            EXPECT_NE(0u, revision); EXPECT_GT(bytes, 0u);
            ++host.paths; *path = host.path.c_str(); return VN_OK;
        }
        static VnResult Open(void* raw, std::uint64_t pack, const char* locator,
            std::uint64_t revision, std::uint64_t bytes, const VnReadCancellationViewV1* cancel,
            VnByteSourceDescV1* result) {
            auto& host = *static_cast<Host*>(raw);
            EXPECT_EQ(PathSet::ContentId(), pack); EXPECT_STREQ("Media.stream", locator);
            EXPECT_NE(0u, revision);
            if (cancel->isCancelled(cancel->context)) return VN_DEVICE_ERROR;
            auto lease = std::make_unique<Source>(); lease->owner = &host;
            lease->bytes = harken::OpenStreamByteSource(host.path.c_str());
            if (!lease->bytes) return VN_DEVICE_ERROR;
            EXPECT_EQ(bytes, lease->bytes->ByteCount());
            *result = {sizeof(*result), 0, lease->bytes->ByteCount(), lease.get(), &Read, &Close};
            ++host.opens; lease.release(); return VN_OK;
        }
        static std::int32_t Read(void* raw, std::uint64_t offset, void* destination,
            std::uint32_t bytes, std::uint32_t* count, const VnReadCancellationViewV1* cancel) {
            if (cancel->isCancelled(cancel->context)) return VN_BYTE_READ_CANCELLED;
            const auto result = static_cast<Source*>(raw)->bytes->ReadAt(offset, destination, bytes, {});
            *count = static_cast<std::uint32_t>(result.bytes);
            return result.status == harken::StreamReadStatus::Ok ? VN_BYTE_READ_OK : VN_BYTE_READ_IO_ERROR;
        }
        static void Close(void* raw) {
            std::unique_ptr<Source> source(static_cast<Source*>(raw)); ++source->owner->closes;
        }
        VnPackStreamResolverV1 Descriptor(bool callback) {
            return {sizeof(VnPackStreamResolverV1), 0, this, &Retain, &Release,
                callback ? nullptr : &Path, callback ? &Open : nullptr};
        }
    };
    const auto fixture = harken::test::MakeVorbisFixture(2, true);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    std::vector<float> planar(fixture.reference.size());
    for (std::size_t frame = 0; frame < media.frames; ++frame) {
        planar[frame] = fixture.reference[2 * frame]; planar[media.frames + frame] = fixture.reference[2 * frame + 1];
    }
    const auto bytes = harken::test::WrapVorbisStream(fixture.media);
    harkentest::Capture reference;
    for (const unsigned mode : {0u, 1u, 2u}) {
        SCOPED_TRACE(mode);
        Host original, replacement;
        {
            EngineSession session(StreamConfig());
            ASSERT_EQ(VN_OK, session.InitResult());
            auto& engine = *session.Engine();
            PathSet set("pack-external-resolver");
            original.path = (set.directory / "Media.stream").string();
            replacement.path = (set.directory / "Other.stream").string();
            ASSERT_TRUE(harkentest::WriteBytes(original.path, bytes));
            ASSERT_TRUE(harkentest::WriteBytes(replacement.path, bytes));
            Provider provider{BuildSoundContent(PathSet::ContentId(), 1, true, planar, 0, false, media.revision)};
            ASSERT_TRUE(harkentest::WriteBytes(set.content, provider.bytes));
            if (mode) set.members[1] = Member(PathSet::ContentId(), false, &provider, &set.names);
            auto originalResolver = original.Descriptor(mode == 2);
            auto replacementResolver = replacement.Descriptor(mode == 2);
            std::vector<VnCanonicalPackSetMemberV3> members(2);
            for (std::size_t i = 0; i < members.size(); ++i)
                members[i] = {sizeof(members[i]), 0, set.members[i], nullptr};
            auto range = std::make_shared<harken::PackMediaRange>();
            auto external = std::make_shared<harken::PackExternalStream>();
            external->fileBytes = bytes.size(); range->external = external;
            range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
            range->sampleRate = media.sampleRate; range->channelCount = media.channels;
            range->frameCount = media.frames; range->contentRevision = media.revision;
            ASSERT_GT(media.seek.size(), 2u);
            const auto prefixEnd = media.packetDataOffset + media.seek[2].packetOffset;
            harken::VorbisPrepareResult prepared;
            range->vorbis = harken::VorbisStreamMedia::Prepare(engine.vorbis,
                {fixture.media.begin(), fixture.media.begin() + prefixEnd}, fixture.media.size(),
                4 * 1024 * 1024, prepared, true);
            ASSERT_NE(nullptr, range->vorbis);
            std::vector<std::vector<harken::PackExternalMediaReference>> typed(2);
            typed[1].push_back({kSound, "Media.stream", range});
            std::vector<VnCanonicalPackSetMemberResultV1> results(2);
            for (auto& result : results) result.structSize = sizeof(result);
            VnCanonicalPackSetActivationDescV3 desc{};
            desc.structSize = sizeof(desc); desc.setRevision = 1;
            desc.memberCount = desc.outputMemberCapacity = 2;
            desc.memberStructSize = sizeof(members[0]); desc.members = members.data();
            desc.outputMemberStructSize = sizeof(results[0]); desc.outputMembers = results.data();
            VnCanonicalPackSetActivationReportV1 report{}; report.structSize = sizeof(report);
            const auto activate = [&] { return harken::ActivateCanonicalPackSetV3(engine, &desc, &report, &typed); };
            if (mode) {
                EXPECT_EQ(VN_PACK_PROVIDER_FAILED, activate());
                EXPECT_EQ(nullptr, engine.packComposition.Published());
                members[1].streams = &originalResolver;
            }
            ASSERT_EQ(VN_OK, activate());
            EXPECT_EQ(0u, original.opens.load());
            EXPECT_EQ(mode ? 1u : 0u, original.retains.load());
            auto oldReference = engine.registry.ReferenceOf(kSound, engine.registry.CookieOf(kSound));
            ASSERT_NE(nullptr, oldReference.Range());
            const auto oldToken = engine.packComposition.Published()->owners[1].resourceToken;
            const auto captured = PostAndCapture(&engine, static_cast<unsigned>(media.frames / 256 + 64));
            if (!mode) reference = captured;
            else ExpectSameAudible(reference, captured, media.frames);
            if (mode == 2) EXPECT_GT(original.opens.load(), 0u);
            for (std::size_t i = 0; i < members.size(); ++i) members[i].member.source.expectedHandle = results[i].handle;
            members[0].member = Member(PackId("Init"), true, nullptr, nullptr, results[0].handle, VN_PACK_MEMBER_KEEP);
            desc.expectedCurrentInitHandle = report.initHandle; desc.baseSetRevision = 1; desc.setRevision = 2;
            if (mode) {
                members[1].streams = &replacementResolver;
                replacement.failRetain = true;
                EXPECT_EQ(VN_DEVICE_ERROR, activate());
                EXPECT_EQ(oldToken, engine.packComposition.Published()->owners[1].resourceToken);
                replacement.failRetain = false;
            }
            ASSERT_EQ(VN_OK, activate());
            EXPECT_NE(oldToken, engine.packComposition.Published()->owners[1].resourceToken);
            EXPECT_NE(oldReference.Range()->external,
                engine.registry.ReferenceOf(kSound, engine.registry.CookieOf(kSound)).Range()->external);
            EXPECT_EQ(0u, original.releases.load());
            // An old closure still resolves its exact old locator after commit.
            const auto oldSource = oldReference.Range()->external->locator->Open({});
            ASSERT_NE(nullptr, oldSource); EXPECT_EQ(bytes.size(), oldSource->ByteCount());
            const auto newer = PostAndCapture(&engine, static_cast<unsigned>(media.frames / 256 + 64));
            ExpectSameAudible(reference, newer, media.frames);
            if (mode == 2) EXPECT_GT(replacement.opens.load(), 0u);
            EXPECT_EQ(0u, replacement.releases.load());
            const auto newToken = engine.packComposition.Published()->owners[1].resourceToken;
            const auto pathCalls = replacement.paths.load();
            members[1].streams = nullptr;
            members[1].member.operation = VN_PACK_MEMBER_NAME_ONLY;
            members[1].member.source.expectedHandle = results[1].handle;
            desc.baseSetRevision = 2; desc.setRevision = 3;
            ASSERT_EQ(VN_OK, activate());
            EXPECT_EQ(newToken, engine.packComposition.Published()->owners[1].resourceToken);
            EXPECT_EQ(pathCalls, replacement.paths.load());
            members[1].member = Member(PathSet::ContentId(), false, nullptr, nullptr,
                results[1].handle, VN_PACK_MEMBER_KEEP);
            typed[1].clear(); desc.baseSetRevision = 3; desc.setRevision = 4;
            ASSERT_EQ(VN_OK, activate());
            EXPECT_EQ(newToken, engine.packComposition.Published()->owners[1].resourceToken);
        }
        EXPECT_EQ(original.retains.load(), original.releases.load());
        EXPECT_EQ(replacement.retains.load(), replacement.releases.load());
        EXPECT_EQ(original.opens.load(), original.closes.load());
        EXPECT_EQ(replacement.opens.load(), replacement.closes.load());
    }
}

TEST(CanonicalPackSetActivate, StreamedPackMediaPlaysResidentSamplesWithoutResidentCopy) {
    using namespace packstream;
    const auto samples = PlanarSignal(1.0f);
    const auto resident = ResidentReference("pack-stream-resident", samples);
    const auto content = BuildSoundContent(PathSet::ContentId(), 1, true, samples);
    auto config = StreamConfig();
    // Streamed media is not resident, so it is not charged to decode staging.
    config.maxPackDecodeStagingBytes = content.size() - 1024;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    PathSet set("pack-stream-streamed");
    ASSERT_TRUE(harkentest::WriteBytes(set.content, content));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(set.members, report));
    std::weak_ptr<const harken::StreamByteSource> source;
    {
        const auto reference = engine->registry.ReferenceOf(kSound, engine->registry.CookieOf(kSound));
        ASSERT_TRUE(reference.Valid());
        EXPECT_FALSE(reference.View().Valid());
        const auto range = reference.Range();
        ASSERT_NE(nullptr, range);
        EXPECT_EQ(2u, range->channelCount);
        EXPECT_EQ(kFrames, range->frameCount);
        source = range->source;
    }
    const auto streamed = PostAndCapture(engine, kBlocks);
    ExpectSameAudible(resident, streamed, kFrames);

    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = report.initHandle;
    unload.baseSetRevision = report.resultSetRevision;
    unload.unloadRevision = report.resultSetRevision + 1;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    for (int pass = 0; pass < 200 && !source.expired(); ++pass) {
        engine->streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_Update());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(source.expired());
    EXPECT_EQ(0u, engine->registry.CookieOf(kSound));
}

TEST(CanonicalPackSetActivate, StreamedPackVoiceKeepsOriginalSourceAcrossNewRevision) {
    using namespace packstream;
    const auto first = PlanarSignal(1.0f), second = PlanarSignal(0.5f);
    const auto firstReference = ResidentReference("pack-stream-replace-ref1", first);
    const auto secondReference = ResidentReference("pack-stream-replace-ref2", second);
    EngineSession session(StreamConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    PathSet set("pack-stream-replace");
    ASSERT_TRUE(harkentest::WriteBytes(set.content,
        BuildSoundContent(PathSet::ContentId(), 1, true, first)));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(set.members, report));

    harkentest::Capture old;
    VnActionResolved resolved{};
    ASSERT_EQ(VN_OK, Resolve("Play_A", &resolved));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostActionResolved(&resolved, 0, 0, &firing));
    PumpFiring(engine, 4, old);
    // The next revision lives at another path and the original file is
    // deleted; the running voice keeps reading its opened source.
    const auto next = (set.directory / "ContentNext.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(next,
        BuildSoundContent(PathSet::ContentId(), 2, true, second)));
    set.members[1].source.utf8Path = next.c_str();
    ASSERT_EQ(VN_OK, Activate(set.members, report, 1));
    ASSERT_TRUE(std::filesystem::remove(set.content));
    PumpFiring(engine, kBlocks, old);
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    ExpectSameAudible(firstReference, old, kFrames);
    ExpectSameAudible(secondReference, PostAndCapture(engine, kBlocks), kFrames);
}

TEST(CanonicalPackSetActivate, StreamedPackChangedBytesNeverReachOutput) {
    using namespace packstream;
    const auto samples = PlanarSignal(1.0f);
    const auto reference = ResidentReference("pack-stream-corrupt-ref", samples);
    auto config = StreamConfig();
    // A ring shorter than one chunk keeps the changed final chunk outside the
    // start preparation, so verified material is published first.
    config.streamRingBytes = (1024) * sizeof(float);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    PathSet set("pack-stream-corrupt");
    ASSERT_TRUE(harkentest::WriteBytes(set.content,
        BuildSoundContent(PathSet::ContentId(), 1, true, samples)));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(set.members, report));
    {
        // The media blob is the file tail; change the last right-channel frame.
        std::fstream file(set.content, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-static_cast<std::streamoff>(sizeof(float)), std::ios::end);
        const float changed = 0.75f;
        file.write(reinterpret_cast<const char*>(&changed), sizeof(changed));
        ASSERT_TRUE(file.good());
    }
    const auto streamed = PostAndCapture(engine, kBlocks);
    const auto second = Audible(streamed);
    const std::uint64_t verified = 2 * harken::kPackMediaRangeChunkFrames;
    ExpectSameAudible(reference, streamed, verified - 256);
    for (std::uint64_t i = second + verified; i < streamed.left.size(); ++i) {
        ASSERT_EQ(0.0f, streamed.left[i]) << i;
        ASSERT_EQ(0.0f, streamed.right[i]) << i;
    }
}

TEST(CanonicalPackSetActivate, StreamOnlyMediaGainsResidentWhenDefinitionTurnsResident) {
    using namespace packstream;
    const auto samples = PlanarSignal(1.0f);
    const auto reference = ResidentReference("pack-stream-upgrade-ref", samples);
    EngineSession session(StreamConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    PathSet set("pack-stream-upgrade");
    ASSERT_TRUE(harkentest::WriteBytes(set.content,
        BuildSoundContent(PathSet::ContentId(), 1, true, samples)));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, Activate(set.members, report));
    const auto cookie = engine->registry.CookieOf(kSound);
    EXPECT_FALSE(engine->registry.Lookup(kSound).Valid());

    // The next revision defines the same bit-exact media as resident. The
    // registered entry is reused and gains resident samples at commit.
    const auto next = (set.directory / "ContentResident.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(next, BuildSoundContent(PathSet::ContentId(), 2, false, samples)));
    set.members[1].source.utf8Path = next.c_str();
    ASSERT_EQ(VN_OK, Activate(set.members, report, 1));
    EXPECT_EQ(cookie, engine->registry.CookieOf(kSound));
    const auto upgraded = engine->registry.Lookup(kSound);
    ASSERT_TRUE(upgraded.Valid());
    EXPECT_EQ(2u, upgraded.channelCount);
    EXPECT_EQ(kFrames, upgraded.frames);
    ExpectSameAudible(reference, PostAndCapture(engine, kBlocks), kFrames);

    // The entry now has resident samples and its original range. After the
    // original file changes in place, a streamed revision of the same samples
    // from another file must not reuse that stale range.
    ASSERT_TRUE(harkentest::WriteBytes(set.content,
        BuildSoundContent(PathSet::ContentId(), 1, true, samples, 0, true)));
    const auto streamedAgain = (set.directory / "ContentStreamedAgain.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(streamedAgain,
        BuildSoundContent(PathSet::ContentId(), 3, true, samples)));
    set.members[1].source.utf8Path = streamedAgain.c_str();
    ASSERT_EQ(VN_OK, Activate(set.members, report, 2));
    EXPECT_NE(cookie, engine->registry.CookieOf(kSound));
    ExpectSameAudible(reference, PostAndCapture(engine, kBlocks), kFrames);
}

TEST(CanonicalPackSetActivate, StreamedPackRebuiltInPlaceActivatesNextRevision) {
    using namespace packstream;
    const auto first = PlanarSignal(1.0f), second = PlanarSignal(0.5f);
    const auto secondReference = ResidentReference("pack-stream-inplace-ref", second);
    const auto firstReference = ResidentReference("pack-stream-inplace-ref1", first);
    struct Case { bool nextStreamed; bool sameSamplesMoved; };
    for (const auto rebuilt : {Case{true, false}, Case{false, false}, Case{true, true}}) {
        SCOPED_TRACE(rebuilt.nextStreamed);
        SCOPED_TRACE(rebuilt.sameSamplesMoved);
        EngineSession session(StreamConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        PathSet set("pack-stream-inplace");
        ASSERT_TRUE(harkentest::WriteBytes(set.content,
            BuildSoundContent(PathSet::ContentId(), 1, true, first)));
        VnCanonicalPackSetActivationReportV1 report{};
        ASSERT_EQ(VN_OK, Activate(set.members, report));
        // The published range still reads this path through its open source.
        // With the same samples at a new offset, comparing against it must
        // read the rewritten bytes, which no longer match its checksums.
        const auto& next = rebuilt.sameSamplesMoved ? first : second;
        ASSERT_TRUE(harkentest::WriteBytes(set.content,
            BuildSoundContent(PathSet::ContentId(), 2, rebuilt.nextStreamed, next, 0,
                              rebuilt.sameSamplesMoved)));
        ASSERT_EQ(VN_OK, Activate(set.members, report, 1));
        EXPECT_EQ(!rebuilt.nextStreamed, engine->registry.Lookup(kSound).Valid());
        ExpectSameAudible(rebuilt.sameSamplesMoved ? firstReference : secondReference,
                          PostAndCapture(engine, kBlocks), kFrames);
    }
}

TEST(CanonicalPackSetActivate, ProviderMediaStreamsForAnotherMembersDefinitionAndGainsResident) {
    using namespace packstream;
    const auto samples = PlanarSignal(1.0f);
    const auto reference = ResidentReference("pack-stream-provider-ref", samples);
    EngineSession session(StreamConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    PathSet set("pack-stream-provider");
    const auto providerId = PackId("zzz_media_provider");
    ASSERT_LT(PathSet::ContentId(), providerId);
    const std::string provider = (set.directory / "Provider.pack").string();
    const std::string next = (set.directory / "ContentResident.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(provider, BuildMediaProvider(providerId, samples)));
    ASSERT_TRUE(harkentest::WriteBytes(set.content,
        BuildSoundContent(PathSet::ContentId(), 1, true, samples, providerId)));
    ASSERT_TRUE(harkentest::WriteBytes(next,
        BuildSoundContent(PathSet::ContentId(), 2, false, samples, providerId)));
    std::array<VnCanonicalPackSetMemberV1, 3> members{set.members[0].source, set.members[1].source, {}};
    members[2] = members[1];
    members[2].packStableId = providerId;
    members[2].utf8Path = provider.c_str();
    std::array<VnCanonicalPackSetMemberResultV1, 3> rows{};
    for (auto& row : rows) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 3, rows.data(), report));
    // The provider has no Sound definition of its own; the other member's
    // streaming choice still keeps its media out of resident memory.
    {
        const auto streamed = engine->registry.ReferenceOf(kSound, engine->registry.CookieOf(kSound));
        EXPECT_FALSE(streamed.View().Valid());
        EXPECT_NE(nullptr, streamed.Range());
    }
    ExpectSameAudible(reference, PostAndCapture(engine, kBlocks), kFrames);

    // The next revision defines the provider's unchanged media as resident.
    for (std::size_t i = 0; i < members.size(); ++i) members[i].expectedHandle = rows[i].handle;
    members[1].utf8Path = next.c_str();
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), 3, rows.data(), report,
                                                          report.initHandle, report.resultSetRevision));
    const auto upgraded = engine->registry.Lookup(kSound);
    ASSERT_TRUE(upgraded.Valid());
    EXPECT_EQ(kFrames, upgraded.frames);
    ExpectSameAudible(reference, PostAndCapture(engine, kBlocks), kFrames);
}

} // namespace

TEST(CanonicalPackSetActivate, LargeInitProjects257VariablesAndUses65thAttenuation) {
    using namespace harken;
    using namespace harkentest::packfoundation;
    const auto initId = companionfixture::PackId("Init");
    std::vector<VariableDefinitionV1> variables;
    for (unsigned i = 0; i < 257; ++i)
        variables.push_back(harkentest::MakeVariableDefinition(harkentest::ObjectId(10000+i), VariableKindV1::Smooth, 0, 8, 2));
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = companionfixture::project;
    world.variables = variables.data();
    world.variableCount = variables.size();
    world.capacity.maxVariableDefinitions = variables.size();
    world.identity.capabilityMask = kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    ASSERT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    const auto bus = DefaultRuntimeBusFixture();
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = initId;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = variables.size();
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeader(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, partition.data());
    std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, initId,
        RequirementPayload(initId, 1, 1, variables.size(), body.size(), bus.triple));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, initId, std::move(partition));
    builder.AddEntry(kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    auto desc = harkentest::MakeAttenuationDesc(1, 10);
    desc.proximityFrequency = 250;
    for (unsigned i = 0; i < 65; ++i) {
        std::vector<std::uint8_t> payload(kPackAttenuationBytes);
        SerializePackAttenuation(desc, payload.data());
        builder.AddEntry(kPackEntryKindAttenuation, harkentest::ObjectId(1000+i), std::move(payload), kPackAttenuationVintage);
    }
    std::vector<std::uint8_t> overlay(kPackVariablePolicyHeaderBytes + variables.size() *
        (kPackVariablePolicyRowBytes + kPackVariableAnchorCountBytes + kPackVariableLabelHeaderBytes), 0);
    SerializePackVariablePolicyHeader({static_cast<std::uint32_t>(variables.size())}, overlay.data());
    std::size_t cursor = kPackVariablePolicyHeaderBytes;
    for (const auto& variable : variables) {
        std::uint64_t execution = 0;
        ASSERT_TRUE(ComputeVariableExecutionFingerprintV1(variable.variableId, 1, 1, 3,
            variable.rangeMin, variable.rangeMax, variable.defaultValue, nullptr, 0, &execution));
        SerializePackVariablePolicyRow({variable.variableId, 3, execution}, overlay.data() + cursor);
        cursor += kPackVariablePolicyRowBytes;
    }
    for (std::size_t i = 0; i < variables.size(); ++i) {
        SerializePackVariableAnchorCount({0}, overlay.data() + cursor);
        cursor += kPackVariableAnchorCountBytes;
    }
    for (const auto& variable : variables) {
        SerializePackVariableLabelHeader({ComputeVariableLabelMappingFingerprintV1(variable.variableId, {}), 0}, overlay.data() + cursor);
        cursor += kPackVariableLabelHeaderBytes;
    }
    builder.AddEntry(kPackEntryKindVariablePolicyOverlay, 0, std::move(overlay));
    companionfixture::Provider provider{builder.Build()};
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = initId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.flags = VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
    member.providerUserData = &provider;
    member.provide = companionfixture::Provider::Read;
    member.release = companionfixture::Provider::Release;
    const auto atten = harkentest::ObjectId(1064);
    companionfixture::Provider content{companionfixture::BuildPreview(false, false, atten, world.capacity)};
    auto contentMember = member;
    contentMember.packStableId = companionfixture::PackId("action_" + ObjectIdString(companionfixture::action));
    contentMember.packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    contentMember.providerUserData = &content;
    const std::array members{member, contentMember};
    {
        EngineSession session(CanonicalActivationConfig(2, 256, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 rows[2]{};
        for (auto& row : rows) row.structSize = sizeof(row);
        VnCanonicalPackSetActivationReportV1 report{};
        for (unsigned nth : {1u, 2u}) {
            harken::detail::ArmLogicAllocFaultForTests(nth, harken::detail::LogicAllocSite::VariableValueStorage);
            const auto result = harkentest::ActivateCanonicalMembersV1(members.data(), members.size(), rows, report);
            harken::detail::DisarmLogicAllocFaultForTests();
            EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, result);
            EXPECT_EQ(0u, session.Engine()->variables.DefinitionCount());
            EXPECT_EQ(0u, session.Engine()->spatial.ProfileCount());
        }
        ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members.data(), members.size(), rows, report));
        auto& e = *session.Engine();
        EXPECT_EQ(257u, e.variables.DefinitionCount());
        EXPECT_EQ(65u, e.packComposition.Published()->attenuationBindings.count);
        ASSERT_EQ(VN_OK, Vn_SetValue(variables.back().variableId, 7));
        harkentest::AdvanceVariableWrite(&e);
        float value = 0;
        ASSERT_TRUE(e.params.ScopedVariableValue(nullptr, 0, variables.back().variableId, &value));
        EXPECT_FLOAT_EQ(7, value);
        const auto generation = e.spatial.CurrentProfileGeneration(atten);
        ASSERT_NE(0u, generation);
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = companionfixture::action;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(post.emitter, {0, 0, -5.5f}));
        VnActionFiringHandle firing = 0;
        const auto baselineBlock = e.mixer.BlocksRendered();
        ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
        for (unsigned i = 0; i < 8; ++i) {
            e.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_GT(e.mixer.BlocksRendered(), baselineBlock);
        ASSERT_EQ(1u, e.voices.ActiveCount());
        unsigned observed = 0;
        e.ledger.ForEachPlayableOwnedLive([&](VnVoiceHandle render, const VoiceLedger::PlayableParentRef&) {
            Spatializer::SpatialVoiceState state{};
            float distance = 0;
            ASSERT_TRUE(e.spatial.CaptureVoice(render, &state, &distance));
            EXPECT_EQ(atten, state.attenuationId);
            EXPECT_EQ(generation, state.attenuationGeneration);
            EXPECT_NEAR(-6.0206f, distance, 0.0001f);
            ++observed;
        });
        EXPECT_EQ(1u, observed);
    }
    std::vector<std::string> symbols;
    symbols.reserve(variables.size());
    for (std::size_t i = 0; i < variables.size(); ++i) symbols.push_back("Variable_" + std::to_string(i));
    std::vector<VnPackVariableNameV1> variableNames;
    for (std::size_t i = 0; i < variables.size(); ++i)
        variableNames.push_back({variables[i].variableId, symbols[i].c_str()});
    VnPackNamesV1 names{sizeof(VnPackNamesV1), 0, companionfixture::project, {}, "", "",
        static_cast<std::uint32_t>(variableNames.size()), 0, variableNames.data(), nullptr};
    std::uint64_t nameBudget = UINT64_MAX;
    PackCompanionNames capturedNames;
    ASSERT_EQ(VN_OK, CapturePackCompanionNames(names, nameBudget, &capturedNames));
    const auto nameBytes = UINT64_MAX - nameBudget;
    const auto mapRows = 65ull * sizeof(PackAttenuationBindingMapV1::Binding);
    for (const bool companion : {false, true}) {
        for (const auto allowance : {2ull * mapRows, 4ull * mapRows - 1u, 4ull * mapRows}) {
            SCOPED_TRACE(testing::Message() << "companion=" << companion << " rows=" << allowance);
            auto config = CanonicalActivationConfig(2, 256, 48000);
            config.maxPackDecodeStagingBytes = provider.bytes.size() + sizeof(PackAttenuationBindingMapV1)*4 +
                allowance + (companion ? nameBytes : 0);
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            VnCanonicalPackSetMemberResultV1 row{};
            row.structSize = sizeof(row);
            VnCanonicalPackSetActivationReportV1 report{};
            VnResult result;
            allocation_probe::count = 0;
            allocation_probe::bytes = 0;
            allocation_probe::enabled = true;
            if (companion) {
                std::vector<VnCanonicalPackSetMemberV2> named{
                    companionfixture::Member(initId, true, &provider, &names)};
                result = companionfixture::Activate(named, report);
            } else {
                result = harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report);
            }
            allocation_probe::enabled = false;
            const auto requestedBytes = allocation_probe::bytes;
            const auto allocationCount = allocation_probe::count;
            const auto key = std::string(companion ? "v2_" : "v1_") + std::to_string(allowance);
            RecordProperty(key + "_requested_bytes", std::to_string(requestedBytes));
            RecordProperty(key + "_allocation_count", std::to_string(allocationCount));
            RecordProperty(key + "_budget", std::to_string(config.maxPackDecodeStagingBytes));
            RecordProperty(key + "_result", static_cast<int>(result));
            // Encoded bytes and attenuation maps alone no longer cover decode staging.
            EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, result);
            EXPECT_EQ(0u, session.Engine()->variables.DefinitionCount());
            EXPECT_EQ(0u, session.Engine()->spatial.ProfileCount());
            EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
            EXPECT_EQ(provider.reads, provider.releases);
        }
        auto config = CanonicalActivationConfig(2, 256, 48000);
        config.maxPackDecodeStagingBytes = 4u * 1024u * 1024u;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetActivationReportV1 report{};
        if (companion) {
            std::vector<VnCanonicalPackSetMemberV2> named{
                companionfixture::Member(initId, true, &provider, &names)};
            ASSERT_EQ(VN_OK, companionfixture::Activate(named, report));
            std::vector<VnCanonicalPackSetMemberV2> kept{
                companionfixture::Member(initId, true, nullptr, nullptr,
                    report.initHandle, VN_PACK_MEMBER_KEEP)};
            ASSERT_EQ(VN_OK, companionfixture::Activate(kept, report, report.resultSetRevision));
        } else {
            VnCanonicalPackSetMemberResultV1 row{};
            row.structSize = sizeof(row);
            ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
        }
        EXPECT_EQ(257u, session.Engine()->variables.DefinitionCount());
        EXPECT_EQ(65u, session.Engine()->spatial.ProfileCount());
    }
}

namespace buseffectfixture {

constexpr VnBusId busId = VN_OBJECT_ID(0, 2);
constexpr VnObjectId effectId = VN_OBJECT_ID(0, 9401);

std::vector<std::uint8_t> Build(std::uint32_t type, std::initializer_list<float> parameters,
                                bool bypass = false) {
    auto image = harken::NeutralMasterSnapshotImage();
    auto& header = image.busDefinitionSnapshotHeader;
    header.publicationCapabilityVintage = 5;
    header.runtimeCapabilityMask |= (UINT64_C(1) << 19) |
        (UINT64_C(1) << (32 + VN_EFFECT_GAIN)) | (UINT64_C(1) << (32 + VN_EFFECT_LIMITER)) |
        (UINT64_C(1) << (32 + VN_EFFECT_COMPRESSOR));
    header.compileTargetProfileFingerprint = harken::busfingerprint::ProfileFingerprintV1(
        header.profileVintage, header.renderSampleRateHz, header.mixLayout,
        header.deviceLayout, header.runtimeCapabilityMask);
    header.busCount = 2;
    header.downmixCount = 2;
    header.coefficientCount = 128;
    auto bus = image.busDefinitionBusesV2.front();
    bus.busId = busId;
    bus.parentId = VN_BUS_MASTER;
    bus.downmixFirst = 1;
    bus.effectCount = type != harken::kEffectTypeNone ? 1u : 0u;
    image.busDefinitionBusesV2.push_back(bus);
    auto downmix = image.busDefinitionDownmixesV2.front();
    downmix.busId = busId;
    downmix.coefficientFirst = 64;
    image.busDefinitionDownmixesV2.push_back(downmix);
    image.busDefinitionCoefficients.resize(128);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
        image.busDefinitionCoefficients.data() + 64);
    if (type != harken::kEffectTypeNone) {
        harken::PackBusDefinitionEffect effect{};
        effect.busId = busId;
        effect.effectId = effectId;
        effect.effectType = type;
        effect.descriptorVintage = harken::FindPackEffectSchemaByType(type)->descriptorVintage;
        effect.flags = bypass ? 1u : 0u;
        effect.parameterCount = static_cast<std::uint32_t>(parameters.size());
        image.busDefinitionEffects.push_back(effect);
        for (float value : parameters)
            image.busDefinitionEffectParameters.push_back({
                static_cast<std::uint32_t>(image.busDefinitionEffectParameters.size()), value});
    }
    header.effectCount = static_cast<std::uint32_t>(image.busDefinitionEffects.size());
    header.effectParameterCount = static_cast<std::uint32_t>(image.busDefinitionEffectParameters.size());
    header.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image,
        image.busDefinitionEffects, image.busDefinitionEffectParameters);
    header.contentGeneration = header.publishedBusFingerprint;
    harkentest::packfoundation::RuntimeBusFixture fixture;
    fixture.buses = {{busId, VN_BUS_MASTER}};
    fixture.triple = harken::SnapshotBusTriple(header);
    fixture.snapshot = harken::EncodeBusDefinitionSnapshotPayload(image);
    return harkentest::BuildMasterOnlyInitBytes(harkentest::packfoundation::kInitPackStableId, fixture);
}

float SettledLevel(harken::Engine& engine) {
    engine.nullBackend.Pump(16);
    EXPECT_EQ(VN_OK, Vn_Update());
    const auto count = engine.nullBackend.BlockFrames() * engine.nullBackend.Channels();
    const auto* block = engine.nullBackend.LastBlock();
    float peak = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(block[i]));
    EXPECT_GT(peak, 1e-4f);
    return peak;
}

}

TEST(CanonicalPackSetActivate, BusEffectsPreserveAudioAndResourcesAcrossReplacementRollbackAndUnload) {
    using namespace buseffectfixture;
    auto config = CanonicalActivationConfig(2, 256, 48000);
    config.gainEffectPoolCapacity = 1;
    config.limiterEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-bus-effect-transaction";
    const auto initial = harkentest::ActivateInitBytesV1(directory,
        Build(VN_EFFECT_GAIN, {-6.0f}, true), harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, initial.result);
    const auto bus = engine.buses.FindIndex(busId);
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    const auto first = engine.busEffectInstanceRefs[bus][0];
    ASSERT_EQ(VN_EFFECT_GAIN, first.type);
    const auto samples = harkentest::MakeDC(48000, 0.8f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    constexpr auto sound = VN_OBJECT_ID(0, 9402);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    auto play = harkentest::MakePlayDesc(sound);
    play.busId = busId;
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const float dry = SettledLevel(engine);
    const auto replace = [&](const std::vector<std::uint8_t>& bytes) -> VnResult {
        if (!harkentest::WriteBytes(initial.initPath, bytes)) return VN_DEVICE_ERROR;
        const auto* published = engine.packComposition.Published();
        VnCanonicalPackSetMemberV1 member{};
        member.structSize = sizeof(member);
        member.packStableId = harkentest::packfoundation::kInitPackStableId;
        member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
        member.expectedHandle = published->initHandle;
        member.utf8Path = initial.initPath.c_str();
        VnCanonicalPackSetMemberResultV1 row{};
        row.structSize = sizeof(row);
        VnCanonicalPackSetActivationReportV1 report{};
        return harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
            published->initHandle, published->ledger.setRevision);
    };
    ASSERT_EQ(VN_OK, replace(Build(VN_EFFECT_GAIN, {-12.0f})));
    EXPECT_EQ(first.poolIndex, engine.busEffectInstanceRefs[bus][0].poolIndex);
    EXPECT_EQ(first.generation, engine.busEffectInstanceRefs[bus][0].generation);
    const float attenuated = SettledLevel(engine);
    EXPECT_NEAR(dry * std::pow(10.0f, -12.0f / 20.0f), attenuated, 2e-3f);
    const auto beforeFailure = engine.packComposition.PublishedHandle();
    const auto commands = engine.commands.FreeSlots();
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    const auto refused = replace(Build(VN_EFFECT_LIMITER, {-18.0f, 50.0f}));
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, refused);
    EXPECT_EQ(beforeFailure.get(), engine.packComposition.Published());
    EXPECT_EQ(commands, engine.commands.FreeSlots());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_NEAR(attenuated, SettledLevel(engine), 2e-3f);
    ASSERT_EQ(VN_OK, replace(Build(VN_EFFECT_LIMITER, {-18.0f, 50.0f})));
    const float limited = SettledLevel(engine);
    EXPECT_LT(limited, dry * 0.3f);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    ASSERT_EQ(VN_OK, replace(Build(harken::kEffectTypeNone, {})));
    EXPECT_NEAR(dry, SettledLevel(engine), 2e-3f);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    ASSERT_EQ(VN_OK, replace(Build(VN_EFFECT_LIMITER, {-18.0f, 50.0f})));
    EXPECT_NEAR(limited, SettledLevel(engine), 2e-3f);
    const auto beforeUnload = engine.packComposition.PublishedHandle();
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::AfterAllPreparedBeforeCommit);
    const auto unloadRefused = UnloadSet(beforeUnload->initHandle, beforeUnload->ledger.setRevision);
    harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, unloadRefused);
    EXPECT_EQ(beforeUnload.get(), engine.packComposition.Published());
    EXPECT_NEAR(limited, SettledLevel(engine), 2e-3f);
    ASSERT_EQ(VN_OK, UnloadSet(beforeUnload->initHandle, beforeUnload->ledger.setRevision));
    EXPECT_NEAR(dry, SettledLevel(engine), 2e-3f);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_LIMITER));
    EXPECT_EQ(harken::kInvalidBusIndex, engine.buses.FindIndex(busId));
}

TEST(CanonicalPackSetActivate, RetainedBusEffectAppliesCompleteParameterRevisionToAudio) {
    using namespace buseffectfixture;
    auto config = CanonicalActivationConfig(2, 256, 48000);
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-bus-effect-parameters";
    const auto initial = harkentest::ActivateInitBytesV1(directory,
        Build(VN_EFFECT_COMPRESSOR, {-6.0f, 2.0f, 1.0f, 10.0f, 0.0f, 0.0f}, true),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, initial.result);
    const auto bus = engine.buses.FindIndex(busId);
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    const auto retained = engine.busEffectInstanceRefs[bus][0];
    const auto samples = harkentest::MakeDC(48000, 0.8f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    constexpr auto sound = VN_OBJECT_ID(0, 9403);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(sound, &pcm));
    auto play = harkentest::MakePlayDesc(sound);
    play.busId = busId;
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const float dry = SettledLevel(engine);
    ASSERT_TRUE(harkentest::WriteBytes(initial.initPath,
        Build(VN_EFFECT_COMPRESSOR, {-24.0f, 4.0f, 1.0f, 10.0f, 0.0f, 0.0f})));
    const auto* published = engine.packComposition.Published();
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = published->initHandle;
    member.utf8Path = initial.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        published->initHandle, published->ledger.setRevision));
    EXPECT_EQ(retained.poolIndex, engine.busEffectInstanceRefs[bus][0].poolIndex);
    EXPECT_EQ(retained.generation, engine.busEffectInstanceRefs[bus][0].generation);
    EXPECT_FLOAT_EQ(-24.0f, engine.effectParamTargets.Reduce(
        harken::EffectParamTargetStore::Scope::Bus, bus, 0, 0));
    EXPECT_FLOAT_EQ(4.0f, engine.effectParamTargets.Reduce(
        harken::EffectParamTargetStore::Scope::Bus, bus, 0, 1));
    const float expectedDb = -24.0f + (20.0f * std::log10(dry) + 24.0f) / 4.0f;
    EXPECT_NEAR(std::pow(10.0f, expectedDb / 20.0f), SettledLevel(engine), 0.003f);
}

TEST(CanonicalPackSetActivate, BusEffectTransactionsRefuseForeignEffectsWithoutTakingOwnership) {
    using namespace buseffectfixture;
    EngineSession session(CanonicalActivationConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-bus-effect-foreign";
    const auto initial = harkentest::ActivateInitBytesV1(directory,
        Build(harken::kEffectTypeNone, {}), harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, initial.result);
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.effectId = VN_OBJECT_ID(0, 9499);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(busId, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine.nullBackend.Pump(1);
    const auto bus = engine.buses.FindIndex(busId);
    const auto foreign = engine.busEffectInstanceRefs[bus][0];
    const auto before = engine.packComposition.PublishedHandle();
    ASSERT_TRUE(harkentest::WriteBytes(initial.initPath, Build(VN_EFFECT_GAIN, {-12.0f})));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = initial.initHandle;
    member.utf8Path = initial.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_IN_USE, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        initial.initHandle, initial.report.resultSetRevision));
    EXPECT_EQ(VN_IN_USE, UnloadSet(initial.initHandle, initial.report.resultSetRevision));
    EXPECT_EQ(before.get(), engine.packComposition.Published());
    EXPECT_EQ(foreign.poolIndex, engine.busEffectInstanceRefs[bus][0].poolIndex);
    EXPECT_EQ(foreign.generation, engine.busEffectInstanceRefs[bus][0].generation);
    EXPECT_EQ(0u, engine.busEffectOwnerCookies[bus][0]);
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(busId, 0));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report,
        initial.initHandle, initial.report.resultSetRevision));
}
