#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "PackLogicStore.h"
#include "TraceFormat.h"
#include "TraceRecorder.h"
#include "harken/dsp/GainRamp.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/core/PackCompanionNames.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/core/VariableExecutionFingerprint.h"

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
    const auto bus = DefaultRuntimeBusFixtureV1();
    std::uint64_t size = 0;
    auto partition = PartitionPayload(id, revision, init ? 0 : actions,
        std::bit_cast<std::uint32_t>(0.5f), &size, 64, project, bus.triple);
    ImageBuilder image(init ? harken::kPackKindInit : harken::kPackKindCanonicalContent);
    image.AddEntry(harken::kPackEntryKindPackRequirementBlock, id,
        RequirementPayload(id, revision, revision, init ? 0 : actions + 1, size, bus.triple));
    image.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
    if (init) image.AddEntry(harken::kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    else image.AddEntry(harken::kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(PackId("Init"), 1, bus.triple), harken::kPackCanonicalRequirementVintage);
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
    const auto bus = DefaultRuntimeBusFixtureV1();
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
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = id;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = world.playablePropertyCount + 1;
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
    ImageBuilder image(kPackKindCanonicalContent);
    image.AddEntry(kPackEntryKindPackRequirementBlock, id,
        RequirementPayload(id, 1, 1, header.ownedDefinitionCount, body.size(), bus.triple));
    image.AddEntry(kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
    image.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
        DependencyPayload(PackId("Init"), 1, bus.triple), kPackCanonicalRequirementVintage);
    image.AddMedia(soundId, std::move(samples));
    return image.Build();
}

} // namespace companionfixture

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
    const auto bus = DefaultRuntimeBusFixtureV1();
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = initId;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = 1;
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
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
    lookup.utf8Name = "Mode";
    EXPECT_EQ(VN_OK, Vn_ResolveVariableByName(&lookup, &resolved));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetValueByLabelName(variableId, "Eight", 0));
    EXPECT_EQ(VN_OK, Vn_SetValueByLabelName(variableId, "Active", 0));
    names.variableCount = 0;
    names.labelCount = 0;
    EXPECT_EQ(VN_BAD_PACK, Activate(members, report, 2));
    EXPECT_EQ(VN_OK, Vn_ResolveVariableByName(&lookup, &resolved));
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

TEST(CanonicalPackSetActivate, ProductionV1LoadsAndInspectsButCannotActivate) {
    EngineSession session(CanonicalActivationConfig());
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
    harkentest::EngineSession session(CanonicalActivationConfig());
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
    EXPECT_EQ(0u, engine->busSendAdj[foreignFlat]);
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
    EngineSession session(CanonicalActivationConfig());
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
    EngineSession session(CanonicalActivationConfig());
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
    const auto bus = DefaultRuntimeBusFixtureV1();
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = initId;
    header.partitionRevision = 1;
    header.ownedDefinitionCount = variables.size();
    ApplyCanonicalBusTriple(&header, bus.triple);
    std::vector<std::uint8_t> partition(header.headerBytes + body.size());
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    std::memcpy(partition.data() + header.headerBytes, body.data(), body.size());
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, initId,
        RequirementPayload(initId, 1, 1, variables.size(), body.size(), bus.triple));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, initId, std::move(partition));
    builder.AddEntry(kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    auto desc = harkentest::MakeAttenuationDesc(1, 10);
    desc.proximityFrequency = 250;
    for (unsigned i = 0; i < 65; ++i) {
        std::vector<std::uint8_t> payload(kPackV3AttenuationPayloadBytes);
        SerializePackV3Attenuation(desc, payload.data());
        builder.AddEntry(kPackEntryKindAttenuation, harkentest::ObjectId(1000+i), std::move(payload), kPackV2AuthoredToneSchemaVintage);
    }
    std::vector<std::uint8_t> overlay(16 + variables.size() * 36, 0);
    StoreLE32(overlay.data(), variables.size());
    for (std::size_t i = 0; i < variables.size(); ++i) {
        auto* row = overlay.data() + 16 + i * 32;
        const auto& variable = variables[i];
        StoreObjectId(row, variable.variableId);
        row[16] = 3;
        std::uint64_t execution = 0;
        ASSERT_TRUE(ComputeVariableExecutionFingerprintV1(variable.variableId, 1, 1, 3,
            variable.rangeMin, variable.rangeMax, variable.defaultValue, nullptr, 0, &execution));
        StoreLE64(row + 24, execution);
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
            if (companion) {
                std::vector<VnCanonicalPackSetMemberV2> named{
                    companionfixture::Member(initId, true, &provider, &names)};
                result = companionfixture::Activate(named, report);
            } else {
                result = harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report);
            }
            const bool fits = allowance == 4ull * mapRows;
            EXPECT_EQ(fits ? VN_OK : VN_PACK_RESIDENT_CAPACITY, result);
            EXPECT_EQ(fits ? 257u : 0u, session.Engine()->variables.DefinitionCount());
            EXPECT_EQ(fits ? 65u : 0u, session.Engine()->spatial.ProfileCount());
            if (!fits) EXPECT_EQ(nullptr, session.Engine()->packComposition.Published());
        }
    }
}
