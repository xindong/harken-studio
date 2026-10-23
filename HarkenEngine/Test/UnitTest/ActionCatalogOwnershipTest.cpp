#include "Common/ObjectIdTestSupport.h"
#include "PackLogicStore.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"

#include "harken/core/PackComposition.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using namespace harken;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::packfoundation::ImageBuilder;
using harkentest::packfoundation::RequirementPayload;

constexpr std::uint64_t kInitId = 11;
constexpr std::uint64_t kContentId = 22;
constexpr VnObjectId kActionId = harkentest::ObjectId(2002);
constexpr VnObjectId kProjectId{0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL};
constexpr VnPackHandle kInitHandle = static_cast<VnPackHandle>(1) << 32;
constexpr VnPackHandle kContentHandle = static_cast<VnPackHandle>(2) << 32;

void SetAbsoluteTime(AuthoredTimeRationalV1* time) {
    time->domain = AuthoredTimeDomainV1::AbsoluteNanoseconds;
    time->numerator = 0;
    time->denominator = 1;
}

std::string WriteTemp(const std::string& name,
                      const std::vector<std::uint8_t>& bytes) {
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "plan13-catalog-owner";
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

std::vector<std::uint8_t> ActionPartitionPayload(std::uint64_t packStableId,
                                                 std::uint64_t revision,
                                                 VnObjectId actionId,
                                                 std::uint32_t valueBits) {
    VariableDefinitionV1 variable{};
    variable.variableId = harkentest::ObjectId(7);
    variable.kind = VariableKindV1::Smooth;
    variable.rangeMin = -1.0f;
    variable.rangeMax = 1.0f;
    variable.defaultValue = 0.0f;

    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::SetVariable;
    step.target.kind = ActionTargetKindV1::Variable;
    step.target.stableId = harkentest::ObjectId(7);
    step.valueBits = valueBits;
    SetAbsoluteTime(&step.delay);
    SetAbsoluteTime(&step.fade);
    SetAbsoluteTime(&step.cooldown);

    ActionProgramViewV1 action{};
    action.actionStableId = actionId;
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);

    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = kProjectId;
    world.identity.capabilityMask = kWorldCapabilityVariableDefinitions |
                                    kWorldCapabilityActionDefinitions;
    world.variables = &variable;
    world.variableCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.identity.runtimeSemanticFingerprint =
        ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    PackCanonicalPartitionHeader header;
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = 2;
    harkentest::packfoundation::ApplyCanonicalBusTriple(
        &header, harkentest::packfoundation::DefaultFixtureBusTriple());
    std::vector<std::uint8_t> payload(
        kCanonicalDefinitionPartitionHeaderV2Bytes + body.size(), 0);
    SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    std::memcpy(payload.data() + kCanonicalDefinitionPartitionHeaderV2Bytes,
                body.data(), body.size());
    return payload;
}

std::vector<std::uint8_t> BuildContentPack(std::uint64_t revision,
                                           VnObjectId actionId,
                                           std::uint32_t valueBits,
                                           std::uint64_t packStableId = kContentId) {
    std::vector<std::uint8_t> partition = ActionPartitionPayload(
        packStableId, revision, actionId, valueBits);
    const std::uint64_t bodyBytes =
        partition.size() > kCanonicalDefinitionPartitionHeaderV2Bytes
            ? partition.size() - kCanonicalDefinitionPartitionHeaderV2Bytes
            : 0;
    ImageBuilder builder(kPackKindCanonicalContent);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, packStableId,
                     RequirementPayload(packStableId, revision, revision, 2,
                                        bodyBytes));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, packStableId,
                     std::move(partition));
    return builder.Build();
}

PackGameplayCatalogActiveDraft MakeActive(std::string_view name,
                                          VnObjectId actionStableId) {
    PackGameplayCatalogActiveDraft row;
    row.actionStableId = harkentest::ObjectId(actionStableId);
    row.nameBindingRevision = 1;
    row.callableFingerprint = 0xC0FFEE;
    row.ownerPackStableId = kContentId;
    std::uint8_t closure[16];
    StoreLE64(closure, kContentId);
    StoreLE64(closure + 8, 1);
    row.providerClosureFingerprint = Fnv1a64(closure, sizeof(closure));
    row.name = name;
    return row;
}

std::vector<std::uint8_t> BuildInitWithGameplayCatalog(
    VnObjectId actionStableId) {
    PackGameplayCatalogHeader identity;
    identity.projectGuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    identity.initPackStableId = kInitId;
    identity.initContentRevision = 1;
    std::vector<std::uint8_t> catalog;
    EXPECT_TRUE(EncodeGameplayCatalog(
        identity, {MakeActive("Play_Fire", actionStableId)}, {}, &catalog));
    std::uint64_t bodyBytes = 0;
    std::vector<std::uint8_t> partition =
        harkentest::packfoundation::PartitionPayload(kInitId, 1, 0, 0,
                                                    &bodyBytes, 64, kProjectId);
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindGameplayCatalog, 0, std::move(catalog));
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     RequirementPayload(kInitId, 1, 1, 0, bodyBytes));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     std::move(partition));
    harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&builder);
    return builder.Build();
}

std::vector<std::uint8_t> BuildInitBare() {
    std::uint64_t bodyBytes = 0;
    std::vector<std::uint8_t> partition =
        harkentest::packfoundation::PartitionPayload(kInitId, 1, 0, 0,
                                                    &bodyBytes, 64, kProjectId);
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     RequirementPayload(kInitId, 1, 1, 0, bodyBytes));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     std::move(partition));
    harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&builder);
    return builder.Build();
}

VnResult ReplacePack(VnPackHandle handle, const std::vector<std::uint8_t>& bytes,
                     const char* name) {
    const std::string path = WriteTemp(name, bytes);
    VnPackLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Path = path.c_str();
    return Vn_ReplacePack(handle, reinterpret_cast<const VnPackDesc*>(&desc));
}

} // namespace

TEST(ActionCatalogOwnership, PrepareCandidateCopiesCatalogOnInitOnly) {
    PackCanonicalImage initImage;
    const std::vector<std::uint8_t> initBytes =
        BuildInitWithGameplayCatalog(kActionId);
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(initBytes.data(), initBytes.size(),
                                             &initImage));
    ASSERT_TRUE(initImage.hasGameplayCatalog);
    PackComposition composition(1);
    PackComposition::CandidateOwner initOwner;
    ASSERT_EQ(VN_OK, composition.PrepareCandidate(initImage, kInitHandle,
                                                  &initOwner));
    EXPECT_TRUE(initOwner.hasGameplayCatalog);
    EXPECT_EQ(1u, initOwner.gameplayCatalog.active.size());
    EXPECT_EQ(kActionId, initOwner.gameplayCatalog.active[0].actionStableId);

    PackCanonicalImage contentImage;
    const std::vector<std::uint8_t> contentBytes =
        BuildContentPack(1, kActionId, 0x3f000000u);
    ASSERT_EQ(VN_OK,
              ParsePackCanonicalImage(contentBytes.data(), contentBytes.size(),
                                      &contentImage));
    PackComposition::CandidateOwner contentOwner;
    ASSERT_EQ(VN_OK, composition.PrepareCandidate(contentImage, kContentHandle,
                                                  &contentOwner));
    EXPECT_FALSE(contentOwner.hasGameplayCatalog);
    EXPECT_TRUE(contentOwner.gameplayCatalog.active.empty());
}

TEST(ActionCatalogOwnership, KeepAndPrepareCommitExposeSessionCatalog) {
    PackCanonicalImage initImage;
    const std::vector<std::uint8_t> initBytes =
        BuildInitWithGameplayCatalog(kActionId);
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(initBytes.data(), initBytes.size(),
                                             &initImage));
    PackComposition composition(1);
    PackComposition::CandidateOwner initOwner;
    ASSERT_EQ(VN_OK, composition.PrepareCandidate(initImage, kInitHandle,
                                                  &initOwner));
    std::vector<PackComposition::CandidateOwner> owners;
    owners.push_back(std::move(initOwner));
    ASSERT_EQ(VN_OK, composition.Commit(std::move(owners), &initImage,
                                        kInitHandle, 8, 1));
    const auto* published = composition.Published();
    ASSERT_NE(nullptr, published);
    EXPECT_TRUE(published->hasGameplayCatalog);
    ASSERT_FALSE(published->owners.empty());
    EXPECT_TRUE(published->owners.front().hasGameplayCatalog);
    EXPECT_EQ(initImage.gameplayCatalog.header.digest,
              published->gameplayCatalog.header.digest);
    const auto keep = PackComposition::Keep(published->owners.front());
    EXPECT_TRUE(keep.hasGameplayCatalog);
    EXPECT_EQ(published->gameplayCatalog.header.digest,
              keep.gameplayCatalog.header.digest);
    const auto bareBytes = BuildInitBare();
    PackCanonicalImage bare;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bareBytes.data(), bareBytes.size(), &bare));
    PackComposition::CandidateOwner replacement;
    ASSERT_EQ(VN_OK, composition.PrepareCandidate(bare, kInitHandle, &replacement));
    ASSERT_EQ(VN_OK, composition.Commit({std::move(replacement)}, &bare, kInitHandle, 8, 2));
    EXPECT_TRUE(composition.Published()->hasGameplayCatalog);
    EXPECT_FALSE(composition.Published()->owners.front().hasGameplayCatalog);
    ++bare.requirement.packContentRevision;
    ASSERT_EQ(VN_OK, composition.PrepareCandidate(bare, kInitHandle, &replacement));
    ASSERT_EQ(VN_OK, composition.Commit({std::move(replacement)}, &bare, kInitHandle, 8, 3));
    EXPECT_FALSE(composition.Published()->hasGameplayCatalog);
}

TEST(ActionCatalogOwnership, IndependentDirectorySurvivesContentActivationAndRejectsInvalidReplacement) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("directory-init.pack", BuildInitBare()),
        WriteTemp("directory-content.pack", BuildContentPack(1, kActionId, 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 initial{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 1, rows, initial));
    const auto initHandle = initial.initHandle;
    EXPECT_FALSE(session.Engine()->packComposition.Published()->hasGameplayCatalog);
    PackGameplayCatalogHeader header;
    StoreObjectId(header.projectGuid.data(), kProjectId);
    header.initPackStableId = kInitId;
    header.initContentRevision = 1;
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeGameplayCatalog(header, {MakeActive("Play_Fire", kActionId)}, {}, &bytes));
    VnPackByteSpan span{};
    span.structSize = sizeof(span);
    span.bytes = bytes.data();
    span.byteCount = bytes.size();
    ASSERT_EQ(VN_OK, Vn_SetGameplayCatalog(initHandle, &span));
    VnPostActionByNameDesc name{};
    name.structSize = sizeof(name);
    name.utf8Name = "Play_Fire";
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_CONTENT_NOT_LOADED, Vn_PostActionByName(&name, &firing));
    members[0].expectedHandle = initHandle;
    VnCanonicalPackSetActivationReportV1 loaded{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, loaded,
        initHandle, initial.resultSetRevision));
    ASSERT_EQ(VN_OK, Vn_PostActionByName(&name, &firing));
    const auto* before = session.Engine()->packComposition.Published();
    auto invalid = MakeActive("Play_Fire", kActionId);
    invalid.ownerPackStableId = 33;
    ASSERT_TRUE(EncodeGameplayCatalog(header, {invalid}, {}, &bytes));
    span.bytes = bytes.data();
    span.byteCount = bytes.size();
    EXPECT_EQ(VN_PACK_STALE_REVISION, Vn_SetGameplayCatalog(initHandle, &span));
    EXPECT_EQ(before, session.Engine()->packComposition.Published());
    ASSERT_TRUE(EncodeGameplayCatalog(header, {MakeActive("Play_Renamed", kActionId)}, {}, &bytes));
    span.bytes = bytes.data();
    span.byteCount = bytes.size();
    ASSERT_EQ(VN_OK, Vn_SetGameplayCatalog(initHandle, &span));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_PostActionByName(&name, &firing));
    name.utf8Name = "Play_Renamed";
    EXPECT_EQ(VN_OK, Vn_PostActionByName(&name, &firing));
    EXPECT_FALSE(session.Engine()->packComposition.Published()->owners.front().hasGameplayCatalog);
}

TEST(ActionCatalogOwnership, MissingKind25ReplaceIsNoOp) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("bare-init.pack", BuildInitBare()),
        WriteTemp("bare-content.pack", BuildContentPack(1, kActionId, 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    EXPECT_FALSE(session.Engine()->packComposition.Published()->hasGameplayCatalog);
    const VnPackHandle content = rows[1].handle;
    EXPECT_EQ(VN_OK, ReplacePack(content,
                                 BuildContentPack(2, harkentest::ObjectId(3003), 0xbf000000u),
                                 "bare-next.pack"));
}

TEST(ActionCatalogOwnership, StaleActionIdRefusesBeforeMutation) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("stale-init.pack", BuildInitWithGameplayCatalog(kActionId)),
        WriteTemp("stale-content.pack", BuildContentPack(1, kActionId, 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    const VnPackHandle initHandle = rows[0].handle;
    const VnPackHandle content = rows[1].handle;
    const auto* published = session.Engine()->packComposition.Published();
    ASSERT_NE(nullptr, published);
    ASSERT_TRUE(published->hasGameplayCatalog);
    const std::uint64_t digest = published->gameplayCatalog.header.digest;
    const auto publishedHandle =
        session.Engine()->packComposition.PublishedHandle();
    const std::uint64_t generation =
        session.Engine()->packComposition.CompositeGeneration();
    EXPECT_EQ(VN_PACK_STALE_REVISION,
              ReplacePack(content, BuildContentPack(2, harkentest::ObjectId(3003), 0xbf000000u),
                          "stale-next.pack"));
    const auto* after = session.Engine()->packComposition.Published();
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(publishedHandle.get(), after);
    EXPECT_EQ(generation,
              session.Engine()->packComposition.CompositeGeneration());
    EXPECT_TRUE(after->hasGameplayCatalog);
    EXPECT_EQ(digest, after->gameplayCatalog.header.digest);
    EXPECT_EQ(initHandle, after->initHandle);
    EXPECT_EQ(2u, after->owners.size());
}

TEST(ActionCatalogOwnership, StaleInitialContentActivationKeepsPublishedPair) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("first-stale-init.pack", BuildInitWithGameplayCatalog(kActionId)),
        WriteTemp("first-stale-content.pack",
                  BuildContentPack(1, harkentest::ObjectId(3003), 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 initial{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 1, rows, initial));
    members[0].expectedHandle = initial.initHandle;
    const auto before = session.Engine()->packComposition.PublishedHandle();
    const auto generation = session.Engine()->packComposition.CompositeGeneration();
    VnCanonicalPackSetActivationReportV1 rejected{};
    EXPECT_EQ(VN_PACK_STALE_REVISION,
              harkentest::ActivateCanonicalMembersV1(
                  members, 2, rows, rejected, initial.initHandle, 1));
    EXPECT_EQ(before.get(), session.Engine()->packComposition.Published());
    EXPECT_EQ(generation, session.Engine()->packComposition.CompositeGeneration());
    EXPECT_EQ(0u, rows[1].handle);
}

TEST(ActionCatalogOwnership, ActionFromWrongOwnerCannotBypassCatalog) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("wrong-owner-init.pack", BuildInitWithGameplayCatalog(kActionId)),
        WriteTemp("wrong-owner-content.pack",
                  BuildContentPack(1, kActionId, 0x3f000000u, 33))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : 33;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 initial{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 1, rows, initial));
    members[0].expectedHandle = initial.initHandle;
    const auto before = session.Engine()->packComposition.PublishedHandle();
    const auto generation = session.Engine()->packComposition.CompositeGeneration();
    VnCanonicalPackSetActivationReportV1 rejected{};
    EXPECT_EQ(VN_PACK_STALE_REVISION,
              harkentest::ActivateCanonicalMembersV1(
                  members, 2, rows, rejected, initial.initHandle, 1));
    EXPECT_EQ(before.get(), session.Engine()->packComposition.Published());
    EXPECT_EQ(generation, session.Engine()->packComposition.CompositeGeneration());
    EXPECT_EQ(0u, rows[1].handle);
}

TEST(ActionCatalogOwnership, MatchingReplaceKeepsInitCatalogBytes) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("match-init.pack", BuildInitWithGameplayCatalog(kActionId)),
        WriteTemp("match-content.pack", BuildContentPack(1, kActionId, 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    const VnPackHandle initHandle = rows[0].handle;
    const VnPackHandle content = rows[1].handle;
    const auto* before = session.Engine()->packComposition.Published();
    ASSERT_NE(nullptr, before);
    const std::uint64_t digest = before->gameplayCatalog.header.digest;
    ASSERT_EQ(VN_OK, ReplacePack(content,
                                 BuildContentPack(2, kActionId, 0xbf000000u),
                                 "match-next.pack"));
    const auto* after = session.Engine()->packComposition.Published();
    ASSERT_NE(nullptr, after);
    EXPECT_TRUE(after->hasGameplayCatalog);
    EXPECT_EQ(digest, after->gameplayCatalog.header.digest);
    EXPECT_EQ(initHandle, after->initHandle);
    EXPECT_EQ(kActionId, after->gameplayCatalog.active.front().actionStableId);
}

TEST(ActionCatalogOwnership, InitHandleReplaceStaysInitLifecycle) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("init-life-init.pack", BuildInitWithGameplayCatalog(kActionId)),
        WriteTemp("init-life-content.pack", BuildContentPack(1, kActionId, 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    const VnPackHandle initHandle = rows[0].handle;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              ReplacePack(initHandle, BuildContentPack(2, kActionId, 0x3f000000u),
                          "init-life-next.pack"));
    EXPECT_TRUE(session.Engine()->packComposition.Published()->hasGameplayCatalog);
}

TEST(ActionCatalogOwnership, PrepareCanonicalOwnersFaultKeepsOldPair) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[] = {
        WriteTemp("fault-init.pack", BuildInitWithGameplayCatalog(kActionId)),
        WriteTemp("fault-content.pack", BuildContentPack(1, kActionId, 0x3f000000u))};
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = i == 0 ? kInitId : kContentId;
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                    : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    const VnPackHandle initHandle = rows[0].handle;
    const VnPackHandle content = rows[1].handle;
    const auto published = session.Engine()->packComposition.PublishedHandle();
    const std::uint64_t generation =
        session.Engine()->packComposition.CompositeGeneration();
    const std::uint64_t digest = published->gameplayCatalog.header.digest;
    SetPackStagingFaultForTests(PackStagingFault::CompositionPublish);
    const VnResult result =
        ReplacePack(content, BuildContentPack(2, kActionId, 0xbf000000u),
                    "fault-next.pack");
    SetPackStagingFaultForTests(PackStagingFault::None);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, result);
    EXPECT_EQ(published.get(), session.Engine()->packComposition.Published());
    EXPECT_EQ(generation,
              session.Engine()->packComposition.CompositeGeneration());
    EXPECT_TRUE(session.Engine()->packComposition.Published()->hasGameplayCatalog);
    EXPECT_EQ(digest,
              session.Engine()->packComposition.Published()->gameplayCatalog
                  .header.digest);
    EXPECT_EQ(initHandle,
              session.Engine()->packComposition.Published()->initHandle);
}
