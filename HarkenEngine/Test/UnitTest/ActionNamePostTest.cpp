#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"

#include "TraceFormat.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/PackComposition.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
constexpr std::uint64_t kCallableFingerprint = 0xC0FFEE;
constexpr std::uint64_t kNameBindingRevision = 1;

const std::array<std::uint8_t, 16> kProjectGuid = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

void SetAbsoluteTime(AuthoredTimeRationalV1* time) {
    time->domain = AuthoredTimeDomainV1::AbsoluteNanoseconds;
    time->numerator = 0;
    time->denominator = 1;
}

std::string WriteTemp(const std::string& name,
                      const std::vector<std::uint8_t>& bytes) {
    const auto directory =
        std::filesystem::path(HARKEN_TEMP_DIR) / "plan13-name-post";
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
                                                 std::uint32_t valueBits,
                                                 std::uint32_t actionCount = 1,
                                                 VnObjectId variableId = harkentest::ObjectId(7)) {
    VariableDefinitionV1 variable{};
    variable.variableId = variableId;
    variable.kind = VariableKindV1::Smooth;
    variable.rangeMin = -1.0f;
    variable.rangeMax = 1.0f;
    variable.defaultValue = 0.0f;

    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::SetVariable;
    step.target.kind = ActionTargetKindV1::Variable;
    step.target.stableId = variableId;
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
    std::vector<ActionProgramViewV1> actions(actionCount, action);
    for (std::uint32_t i = 0; i < actionCount; ++i) {
        actions[i].actionStableId = harkentest::OffsetObjectId(actionId, i);
        actions[i].programSemanticFingerprint = ComputeActionProgramFingerprintV1(actions[i]);
    }

    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = ObjectIdFromBytes(kProjectGuid);
    world.capacity.maxActionDefinitions = actionCount > 64 ? 4096 : 64;
    world.identity.capabilityMask = kWorldCapabilityVariableDefinitions |
                                    kWorldCapabilityActionDefinitions;
    world.variables = &variable;
    world.variableCount = 1;
    world.actions = actions.data();
    world.actionCount = actionCount;
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
    header.ownedDefinitionCount = actionCount + 1;
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
                                           std::uint64_t packId = kContentId,
                                           std::uint32_t actionCount = 1,
                                           VnObjectId variableId = harkentest::ObjectId(7)) {
    std::vector<std::uint8_t> partition = ActionPartitionPayload(
        packId, revision, actionId, valueBits, actionCount, variableId);
    const std::uint64_t bodyBytes =
        partition.size() > kCanonicalDefinitionPartitionHeaderV2Bytes
            ? partition.size() - kCanonicalDefinitionPartitionHeaderV2Bytes
            : 0;
    ImageBuilder builder(kPackKindCanonicalContent);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, packId,
                     RequirementPayload(packId, revision, revision, actionCount + 1,
                                        bodyBytes));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, packId,
                     std::move(partition));
    return builder.Build();
}

PackGameplayCatalogActiveDraft MakeActive(
    std::string_view name, VnObjectId actionStableId,
    std::uint64_t ownerId = kContentId) {
    PackGameplayCatalogActiveDraft row;
    row.actionStableId = harkentest::ObjectId(actionStableId);
    row.nameBindingRevision = kNameBindingRevision;
    row.callableFingerprint = kCallableFingerprint;
    row.ownerPackStableId = ownerId;
    std::uint8_t closure[16];
    StoreLE64(closure, ownerId);
    StoreLE64(closure + 8, 1);
    row.providerClosureFingerprint = Fnv1a64(closure, sizeof(closure));
    row.name = name;
    return row;
}

std::vector<std::uint8_t> BuildInitWithCatalog(
    const std::vector<PackGameplayCatalogActiveDraft>& active,
    const std::vector<PackGameplayCatalogRetiredDraft>& retired) {
    PackGameplayCatalogHeader identity;
    identity.projectGuid = kProjectGuid;
    identity.initPackStableId = kInitId;
    identity.initContentRevision = 1;
    std::vector<std::uint8_t> catalog;
    EXPECT_TRUE(EncodeGameplayCatalog(identity, active, retired, &catalog));
    std::uint64_t bodyBytes = 0;
    std::vector<std::uint8_t> partition =
        harkentest::packfoundation::PartitionPayload(kInitId, 1, 0, 0,
            &bodyBytes, active.size() > 64 ? 4096 : 64,
            ObjectIdFromBytes(kProjectGuid));
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
            &bodyBytes, 64, ObjectIdFromBytes(kProjectGuid));
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

VnPostActionByNameDesc MakeByName(const char* name) {
    VnPostActionByNameDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Name = name;
    return desc;
}

VnResult PostByName(const char* name, VnActionFiringHandle* firing) {
    const VnPostActionByNameDesc desc = MakeByName(name);
    return Vn_PostActionByName(&desc, firing);
}

VnResult ResolveByName(const char* name, VnActionResolved* resolved) {
    const VnPostActionByNameDesc desc = MakeByName(name);
    return Vn_ResolveActionByName(&desc, resolved);
}

void LoadInitAndContent(VnPackHandle* initHandle, VnPackHandle* contentHandle) {
    *initHandle = 0;
    *contentHandle = 0;
    const std::string initPath = WriteTemp(
        "name-init.pack",
        BuildInitBare());
    const std::string contentPath = WriteTemp(
        "name-content.pack", BuildContentPack(1, kActionId, 0x3f000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (auto& row : rows) {
        row.structSize = sizeof(row);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    *initHandle = report.initHandle;
    *contentHandle = rows[1].handle;
    ASSERT_NE(0u, *initHandle);
    ASSERT_NE(0u, *contentHandle);
    PackGameplayCatalogHeader header;
    header.projectGuid = kProjectGuid;
    header.initPackStableId = kInitId;
    header.initContentRevision = 1;
    std::vector<std::uint8_t> catalog;
    ASSERT_TRUE(EncodeGameplayCatalog(header, {MakeActive("Play_Fire", kActionId)}, {}, &catalog));
    VnPackByteSpan span{};
    span.structSize = sizeof(span);
    span.bytes = catalog.data();
    span.byteCount = catalog.size();
    ASSERT_EQ(VN_OK, Vn_SetGameplayCatalog(*initHandle, &span));
}

VnActionPinnedDesc MakePin(const char* name, VnObjectId actionId) {
    VnActionPinnedDesc desc{};
    desc.structSize = sizeof(desc);
    std::memcpy(desc.projectGuid, kProjectGuid.data(), 16);
    desc.actionId = actionId;
    desc.utf8ExpectedName = name;
    desc.nameBindingRevision = kNameBindingRevision;
    desc.callableFingerprint = kCallableFingerprint;
    return desc;
}

struct PublicationSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t digest = 0;
    VnPackHandle initHandle = 0;
    std::size_t owners = 0;
};

PublicationSnapshot CapturePublication(harken::Engine* engine) {
    PublicationSnapshot snap;
    const auto* published = engine->packComposition.Published();
    if (published != nullptr) {
        snap.generation = engine->packComposition.CompositeGeneration();
        snap.digest = published->hasGameplayCatalog
                          ? published->gameplayCatalog.header.digest
                          : 0;
        snap.initHandle = published->initHandle;
        snap.owners = published->owners.size();
    }
    return snap;
}

void ExpectUnchanged(const PublicationSnapshot& before, harken::Engine* engine) {
    const PublicationSnapshot after = CapturePublication(engine);
    EXPECT_EQ(before.generation, after.generation);
    EXPECT_EQ(before.digest, after.digest);
    EXPECT_EQ(before.initHandle, after.initHandle);
    EXPECT_EQ(before.owners, after.owners);
}

std::uint16_t ReadTraceU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
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

struct TraceApiCall {
    std::uint16_t callId = 0;
    std::size_t payload = 0;
    std::uint32_t payloadBytes = 0;
};

bool CollectTraceApiCalls(const std::vector<std::uint8_t>& bytes,
                          std::vector<TraceApiCall>* out) {
    if (out == nullptr || bytes.size() < kTraceFixedHeaderBytes) {
        return false;
    }
    const std::uint32_t headerBytes = ReadTraceU32(bytes, 8);
    if (headerBytes < kTraceFixedHeaderBytes || headerBytes > bytes.size()) {
        return false;
    }
    std::size_t offset = headerBytes;
    while (offset + kTraceRecordHeaderBytes <= bytes.size()) {
        const std::uint32_t payloadBytes = ReadTraceU32(bytes, offset);
        const std::uint16_t type = ReadTraceU16(bytes, offset + 4);
        offset += kTraceRecordHeaderBytes;
        if (payloadBytes > bytes.size() - offset) {
            return false;
        }
        if (type == kTraceRecordApiCall && payloadBytes >= 2) {
            TraceApiCall rec;
            rec.callId = ReadTraceU16(bytes, offset);
            rec.payload = offset;
            rec.payloadBytes = payloadBytes;
            out->push_back(rec);
        }
        offset += payloadBytes;
    }
    return true;
}

bool ReadPostActionByName128SuccessTail(const std::vector<std::uint8_t>& bytes,
                                        const TraceApiCall& rec,
                                        std::uint32_t* result,
                                        std::uint64_t* firing) {
    constexpr std::uint32_t kSuccessTailBytes = 28;
    if (result == nullptr || firing == nullptr ||
        rec.callId != kTraceCallPostActionByName ||
        rec.payloadBytes < 4 + kSuccessTailBytes ||
        rec.payload + rec.payloadBytes > bytes.size()) {
        return false;
    }
    if (ReadTraceU32(bytes, rec.payload + 4) != 2) {
        return false;
    }
    const std::size_t tail = rec.payload + rec.payloadBytes - kSuccessTailBytes;
    *result = ReadTraceU32(bytes, tail);
    *firing = ReadTraceU64(bytes, tail + 4);
    return true;
}

} // namespace

TEST(ActionNamePost, GrammarRefusesBeforeLookup) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_INVALID_ARG, PostByName("", &firing));
    EXPECT_EQ(0u, firing);

    const std::string tooLong(129, 'A');
    firing = 1;
    EXPECT_EQ(VN_INVALID_ARG, PostByName(tooLong.c_str(), &firing));
    EXPECT_EQ(0u, firing);

    firing = 1;
    EXPECT_EQ(VN_INVALID_ARG, PostByName("Play/Fire", &firing));
    EXPECT_EQ(0u, firing);

    firing = 1;
    EXPECT_EQ(VN_UNKNOWN_ID, PostByName("CON", &firing));
    EXPECT_EQ(0u, firing);
    for (const char* invalid : {"Play Fire", "Play-Fire", "Play.Fire", "1Play", "_Play"}) {
        EXPECT_EQ(VN_INVALID_ARG, PostByName(invalid, &firing));
        EXPECT_EQ(0u, firing);
    }
}

TEST(ActionNamePost, StructSizeFlagsReservedRefuse) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnActionFiringHandle firing = 1;
    VnPostActionByNameDesc desc = MakeByName("Play_Fire");
    desc.structSize = sizeof(desc) - 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostActionByName(&desc, &firing));
    EXPECT_EQ(0u, firing);

    desc = MakeByName("Play_Fire");
    desc.flags = 1;
    firing = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostActionByName(&desc, &firing));

    desc = MakeByName("Play_Fire");
    desc.reserved0 = 1;
    firing = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_PostActionByName(&desc, &firing));

    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved) - 1;
    EXPECT_EQ(VN_INVALID_ARG, ResolveByName("Play_Fire", &resolved));
}

TEST(ActionNamePost, MissingKind25IsCapabilityUnavailableRawPostStillWorks) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string initPath = WriteTemp("bare-init.pack", BuildInitBare());
    const std::string contentPath = WriteTemp(
        "bare-content.pack", BuildContentPack(1, kActionId, 0x3f000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (auto& row : rows) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));
    EXPECT_FALSE(session.Engine()->packComposition.Published()->hasGameplayCatalog);

    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, PostByName("Play_Fire", &firing));
    EXPECT_EQ(0u, firing);

    VnPostActionDesc numeric{};
    numeric.structSize = sizeof(numeric);
    numeric.actionId = harkentest::ObjectId(kActionId);
    ASSERT_EQ(VN_OK, Vn_PostAction(&numeric, &firing));
    EXPECT_NE(0u, firing);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(ActionNamePost, UnknownAndRetiredNamesDoNotFire) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    PackGameplayCatalogRetiredDraft retired;
    retired.lastActionStableId = harkentest::ObjectId(kActionId);
    retired.lastNameBindingRevision = kNameBindingRevision;
    retired.reason = kGameplayCatalogRetiredReasonRenamed;
    retired.name = "Play_Fire";
    const std::string initPath = WriteTemp(
        "retired-init.pack",
        BuildInitWithCatalog({MakeActive("Play_Storm", kActionId)}, {retired}));
    const std::string contentPath = WriteTemp(
        "retired-content.pack", BuildContentPack(1, kActionId, 0x3f000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (auto& row : rows) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 2, rows, report));

    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_UNKNOWN_ID, PostByName("Play_Missing", &firing));
    EXPECT_EQ(0u, firing);

    firing = 1;
    EXPECT_EQ(VN_GAMEPLAY_NAME_RETIRED, PostByName("Play_Fire", &firing));
    EXPECT_EQ(0u, firing);
}

TEST(ActionNamePost, WrongProjectGuidLeavesRawNumericUnchanged) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnPostActionByNameDesc desc = MakeByName("Play_Fire");
    desc.projectGuid[0] = 99;
    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_GAMEPLAY_WRONG_PROJECT, Vn_PostActionByName(&desc, &firing));
    EXPECT_EQ(0u, firing);

    VnPostActionDesc numeric{};
    numeric.structSize = sizeof(numeric);
    numeric.actionId = harkentest::ObjectId(kActionId);
    ASSERT_EQ(VN_OK, Vn_PostAction(&numeric, &firing));
    EXPECT_NE(0u, firing);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(ActionNamePost, MismatchedProviderClosureRefusesContentPublication) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto row = MakeActive("Play_Fire", kActionId);
    row.providerClosureFingerprint ^= 1;
    const std::string initPath = WriteTemp(
        "closure-init.pack", BuildInitWithCatalog({row}, {}));
    const std::string contentPath = WriteTemp(
        "closure-content.pack", BuildContentPack(1, kActionId, 0x3f000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (auto& output : rows) output.structSize = sizeof(output);
    VnCanonicalPackSetActivationReportV1 initial{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 1, rows, initial));
    const auto before = CapturePublication(session.Engine());
    members[0].expectedHandle = initial.initHandle;
    VnCanonicalPackSetActivationReportV1 replacement{};
    EXPECT_EQ(VN_PACK_STALE_REVISION,
              harkentest::ActivateCanonicalMembersV1(
                  members, 2, rows, replacement, initial.initHandle, 1));
    ExpectUnchanged(before, session.Engine());
}

TEST(ActionNamePost, MatchingNamePostsCatalogActionId) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
    ASSERT_NE(0u, firing);
    VnActionFiringInfo info{};
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_QueryActionFiring(firing, &info));
    EXPECT_EQ(kActionId, info.actionId);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(ActionNamePost, UnloadedOwnerIsContentNotLoadedWithoutLoadMutation) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string initPath = WriteTemp(
        "unloaded-init.pack",
        BuildInitWithCatalog({MakeActive("Play_Fire", kActionId)}, {}));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = kInitId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.utf8Path = initPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(&member, 1, &row, report));
    const VnPackHandle initHandle = report.initHandle;
    const auto* published = session.Engine()->packComposition.Published();
    ASSERT_NE(nullptr, published);
    ASSERT_TRUE(published->hasGameplayCatalog);
    const std::uint64_t digest = published->gameplayCatalog.header.digest;
    const std::uint64_t generation =
        session.Engine()->packComposition.CompositeGeneration();
    const std::size_t owners = published->owners.size();

    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_CONTENT_NOT_LOADED, PostByName("Play_Fire", &firing));
    EXPECT_EQ(0u, firing);

    const auto* after = session.Engine()->packComposition.Published();
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(generation,
              session.Engine()->packComposition.CompositeGeneration());
    EXPECT_EQ(digest, after->gameplayCatalog.header.digest);
    EXPECT_EQ(owners, after->owners.size());
    EXPECT_EQ(initHandle, after->initHandle);
}

TEST(ActionNamePost, PinnedFieldMismatchIsContractMismatch) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnActionPinnedDesc desc{};
    desc.structSize = sizeof(desc);
    std::memcpy(desc.projectGuid, kProjectGuid.data(), 16);
    desc.actionId = harkentest::ObjectId(kActionId);
    desc.utf8ExpectedName = "Play_Fire";
    desc.nameBindingRevision = 99;
    desc.callableFingerprint = kCallableFingerprint;
    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_GAMEPLAY_CONTRACT_MISMATCH,
              Vn_PostActionPinned(&desc, &firing));
    EXPECT_EQ(0u, firing);

    desc.nameBindingRevision = kNameBindingRevision;
    desc.callableFingerprint = 0xDEAD;
    firing = 1;
    EXPECT_EQ(VN_GAMEPLAY_CONTRACT_MISMATCH,
              Vn_PostActionPinned(&desc, &firing));
    EXPECT_EQ(0u, firing);

    desc.callableFingerprint = kCallableFingerprint;
    desc.utf8ExpectedName = "Play_Storm";
    firing = 1;
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_PostActionPinned(&desc, &firing));
    EXPECT_EQ(0u, firing);
}

TEST(ActionNamePost, ResolveThenPostResolvedMatchingGenerationSucceeds) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &resolved));
    EXPECT_EQ(kActionId, resolved.actionId);
    EXPECT_EQ(kNameBindingRevision, resolved.nameBindingRevision);
    EXPECT_EQ(kCallableFingerprint, resolved.callableFingerprint);
    EXPECT_EQ(session.Engine()->packComposition.CompositeGeneration(),
              resolved.publicationGeneration);

    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostActionResolved(&resolved, 0, 0, &firing));
    ASSERT_NE(0u, firing);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(ActionNamePost, PostResolvedAfterReplaceIsStaleHandle) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &resolved));
    const std::uint64_t oldGeneration = resolved.publicationGeneration;

    ASSERT_EQ(VN_OK, ReplacePack(contentHandle,
                                 BuildContentPack(2, kActionId, 0xbf000000u),
                                 "name-next.pack"));
    EXPECT_NE(oldGeneration,
              session.Engine()->packComposition.CompositeGeneration());
    const PublicationSnapshot afterReplace =
        CapturePublication(session.Engine());

    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_PostActionResolved(&resolved, 0, 0, &firing));
    EXPECT_EQ(0u, firing);
    ExpectUnchanged(afterReplace, session.Engine());
    ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(contentHandle));
    const auto afterUnload = CapturePublication(session.Engine());
    const std::string initPath = WriteTemp(
        "name-reload-init.pack",
        BuildInitWithCatalog({MakeActive("Play_Fire", kActionId)}, {}));
    const std::string oldPath = WriteTemp(
        "name-old.pack", BuildContentPack(1, kActionId, 0x3f000000u));
    const std::string reloadPath = WriteTemp(
        "name-reload.pack", BuildContentPack(2, kActionId, 0xbf000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].expectedHandle = initHandle;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = oldPath.c_str();
    VnCanonicalPackSetMemberResultV1 outputs[2]{};
    for (auto& output : outputs) output.structSize = sizeof(output);
    VnCanonicalPackSetActivationReportV1 report{};
    EXPECT_EQ(VN_PACK_STALE_REVISION,
              harkentest::ActivateCanonicalMembersV1(
                  members, 2, outputs, report, initHandle, 0));
    ExpectUnchanged(afterUnload, session.Engine());
    members[1].utf8Path = reloadPath.c_str();
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(
                  members, 2, outputs, report, initHandle, 0));
    contentHandle = outputs[1].handle;
    ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(ActionNamePost, Call04ResolveThenUnloadContentRefuses) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);

    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &resolved));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(contentHandle));
    const PublicationSnapshot afterUnload =
        CapturePublication(session.Engine());

    VnActionFiringHandle firing = 1;
    const VnResult result =
        Vn_PostActionResolved(&resolved, 0, 0, &firing);
    EXPECT_TRUE(result == VN_STALE_HANDLE || result == VN_CONTENT_NOT_LOADED)
        << result;
    EXPECT_EQ(0u, firing);
    ExpectUnchanged(afterUnload, session.Engine());
}

TEST(ActionNamePost, Call05RestartRefusesStaleToken) {
    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        VnPackHandle initHandle = 0;
        VnPackHandle contentHandle = 0;
        LoadInitAndContent(&initHandle, &contentHandle);
        ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &resolved));
        ASSERT_NE(harkentest::ObjectId(0u), resolved.actionId);
    }

    EngineSession restarted(MakeNullConfig());
    ASSERT_EQ(VN_OK, restarted.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);
    VnActionFiringHandle firing = 1;
    const VnResult result =
        Vn_PostActionResolved(&resolved, 0, 0, &firing);
    EXPECT_EQ(VN_STALE_HANDLE, result);
    EXPECT_EQ(0u, firing);
}

TEST(ActionNamePost, Call09RenameAdoptStringsAndPinsRefuseRawIdRemains) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    PackGameplayCatalogRetiredDraft retired;
    retired.lastActionStableId = harkentest::ObjectId(kActionId);
    retired.lastNameBindingRevision = kNameBindingRevision;
    retired.reason = kGameplayCatalogRetiredReasonRenamed;
    retired.name = "Play_Fire";
    const std::string initPath = WriteTemp(
        "call09-init.pack",
        BuildInitWithCatalog({MakeActive("Play_Storm", kActionId)}, {retired}));
    const std::string contentPath = WriteTemp(
        "call09-content.pack", BuildContentPack(1, kActionId, 0x3f000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetMemberResultV1 outputs[2]{};
    for (auto& output : outputs) output.structSize = sizeof(output);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 2, outputs, report));
    const PublicationSnapshot snap = CapturePublication(session.Engine());

    VnActionFiringHandle firing = 1;
    EXPECT_EQ(VN_GAMEPLAY_NAME_RETIRED, PostByName("Play_Fire", &firing));
    EXPECT_EQ(0u, firing);
    VnActionPinnedDesc pinA = MakePin("Play_Fire", kActionId);
    firing = 1;
    EXPECT_EQ(VN_GAMEPLAY_NAME_RETIRED, Vn_PostActionPinned(&pinA, &firing));
    EXPECT_EQ(0u, firing);
    ExpectUnchanged(snap, session.Engine());

    firing = 0;
    ASSERT_EQ(VN_OK, PostByName("Play_Storm", &firing));
    ASSERT_NE(0u, firing);
    VnActionFiringInfo info{};
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_QueryActionFiring(firing, &info));
    EXPECT_EQ(kActionId, info.actionId);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));

    VnPostActionDesc numeric{};
    numeric.structSize = sizeof(numeric);
    numeric.actionId = harkentest::ObjectId(kActionId);
    firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&numeric, &firing));
    EXPECT_NE(0u, firing);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST(ActionNamePost, ReusedStringFollowsNewIdAndOldPinRefuses) {
    constexpr VnObjectId kNewActionId = harkentest::ObjectId(3003);
    VnActionPinnedDesc oldPin =
        MakePin("Play_Fire", kActionId);
    {
        EngineSession session(MakeNullConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        VnPackHandle initHandle = 0;
        VnPackHandle contentHandle = 0;
        LoadInitAndContent(&initHandle, &contentHandle);
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
        EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    }

    EngineSession reused(MakeNullConfig());
    ASSERT_EQ(VN_OK, reused.InitResult());
    const std::string initPath = WriteTemp(
        "key03-init.pack",
        BuildInitWithCatalog({MakeActive("Play_Fire", kNewActionId)}, {}));
    const std::string contentPath = WriteTemp(
        "key03-content.pack", BuildContentPack(1, kNewActionId, 0x3f000000u));
    VnCanonicalPackSetMemberV1 members[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packStableId = kInitId;
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].utf8Path = initPath.c_str();
    members[1].structSize = sizeof(members[1]);
    members[1].packStableId = kContentId;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].utf8Path = contentPath.c_str();
    VnCanonicalPackSetMemberResultV1 outputs[2]{};
    for (auto& output : outputs) output.structSize = sizeof(output);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 2, outputs, report));
    const PublicationSnapshot snap = CapturePublication(reused.Engine());

    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
    ASSERT_NE(0u, firing);
    VnActionFiringInfo info{};
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_QueryActionFiring(firing, &info));
    EXPECT_EQ(kNewActionId, info.actionId);
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));

    firing = 1;
    const VnResult pinned = Vn_PostActionPinned(&oldPin, &firing);
    EXPECT_TRUE(pinned == VN_GAMEPLAY_CONTRACT_MISMATCH ||
                pinned == VN_GAMEPLAY_NAME_RETIRED)
        << pinned;
    EXPECT_EQ(0u, firing);
    ExpectUnchanged(snap, reused.Engine());

    VnPostActionDesc numeric{};
    numeric.structSize = sizeof(numeric);
    numeric.actionId = harkentest::ObjectId(kActionId);
    firing = 1;
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_PostAction(&numeric, &firing));
    EXPECT_EQ(0u, firing);
}

TEST(ActionNamePost, NamePostDoesNotLoadOrTouchSidecars) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    LoadInitAndContent(&initHandle, &contentHandle);
    const PublicationSnapshot snap = CapturePublication(session.Engine());
    for (int i = 0; i < 32; ++i) {
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
        ASSERT_NE(0u, firing);
        ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    }
    ExpectUnchanged(snap, session.Engine());
}

TEST(ActionNamePost, FrozenWorkload4096Names256Warm128BytesTwoContentPacks) {
    constexpr std::uint32_t count = 4096;
    constexpr std::uint32_t warmCount = 256;
    std::vector<std::string> names;
    std::vector<PackGameplayCatalogActiveDraft> rows;
    names.reserve(count);
    rows.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "Workload%04u", i);
        names.emplace_back(prefix);
        names.back().resize(128, 'x');
        rows.push_back(MakeActive(names.back(), harkentest::OffsetObjectId(kActionId, i),
                                   i < count / 2 ? kContentId : 33));
    }
    const auto init = BuildInitWithCatalog(rows, {});
    const auto contentA = BuildContentPack(1, kActionId, 0x3f000000u, kContentId, count / 2, harkentest::ObjectId(7));
    const auto contentB = BuildContentPack(1, harkentest::OffsetObjectId(kActionId, count / 2), 0x3f000000u, 33, count / 2, harkentest::ObjectId(8));
    VnConfig config = MakeNullConfig();
    config.maxActions = 4096;
    config.maxResidentDefinitionBytes = 32ull << 20;
    config.maxCompositeIndexBytes = 4ull << 20;
    config.maxPackDecodeStagingBytes = 32ull << 20;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string paths[3] = {
        WriteTemp("workload-init.pack", init),
        WriteTemp("workload-a.pack", contentA),
        WriteTemp("workload-b.pack", contentB)};
    const std::uint64_t stableIds[3] = {kInitId, kContentId, 33};
    VnCanonicalPackSetMemberV1 members[3]{};
    VnCanonicalPackSetMemberResultV1 outputs[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].packStableId = stableIds[i];
        members[i].packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                     : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[i].utf8Path = paths[i].c_str();
        outputs[i].structSize = sizeof(outputs[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK,
              harkentest::ActivateCanonicalMembersV1(members, 3, outputs, report));
    ASSERT_EQ(count, session.Engine()->packComposition.Published()->gameplayCatalog.active.size());
    const auto before = CapturePublication(session.Engine());
    std::array<VnActionResolved, warmCount> tokens{};
    for (std::uint32_t i = 0; i < warmCount; ++i) {
        tokens[i].structSize = sizeof(VnActionResolved);
        ASSERT_EQ(VN_OK, ResolveByName(names[i * (count / warmCount)].c_str(), &tokens[i]));
    }
    for (int form = 0; form < 3; ++form) {
        std::uint64_t elapsed = 0;
        std::uint64_t allocations = 0;
        bool allocationObserverAvailable = false;
        for (std::uint32_t i = 0; i < count + warmCount; ++i) {
            const auto index = i % warmCount;
            const auto started = std::chrono::steady_clock::now();
            VnActionFiringHandle firing = 0;
            VnPostActionDesc numeric{};
            numeric.structSize = sizeof(numeric);
            numeric.actionId = tokens[index].actionId;
            VnResult result;
            harken::memguard::AllocationObservation observed;
            {
                harken::memguard::AllocationObservationScope observation(observed);
                result = form == 0
                    ? PostByName(names[index * (count / warmCount)].c_str(), &firing)
                    : form == 1 ? Vn_PostActionResolved(&tokens[index], 0, 0, &firing)
                                : Vn_PostAction(&numeric, &firing);
            }
            const auto stopped = std::chrono::steady_clock::now();
            ASSERT_EQ(VN_OK, result) << "form=" << form << " iteration=" << i;
            ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
            if (i % 64 == 63) ASSERT_EQ(VN_OK, Vn_Update());
            if (i >= warmCount) {
                elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(stopped - started).count();
                allocationObserverAvailable = observed.available;
                allocations += observed.logic.allocations + observed.logic.reallocations;
            }
        }
        std::printf("[NameWorkload] form=%d names=4096 warm=256 bytes=128 packs=3 posts=%u ns/post=%llu\n",
                    form, count, static_cast<unsigned long long>(elapsed / count));
        std::printf("[NameWorkload] form=%d native-allocation-observer=%s allocations=%llu\n", form,
                    allocationObserverAvailable ? "available" : "unavailable",
                    static_cast<unsigned long long>(allocations));
        if (allocationObserverAvailable) EXPECT_EQ(0u, allocations);
    }
    ExpectUnchanged(before, session.Engine());
}

TEST(ActionNamePost, ActivatedCatalogSupportsNameCallsAndVariableTickets) {
    VnActionResolved stale{};
    stale.structSize = sizeof(stale);
    {
        EngineSession prior(MakeNullConfig());
        ASSERT_EQ(VN_OK, prior.InitResult());
        VnPackHandle init = 0, content = 0;
        LoadInitAndContent(&init, &content);
        ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &stale));
    }
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    ASSERT_NO_FATAL_FAILURE(LoadInitAndContent(&initHandle, &contentHandle));
    const auto writeAndQueryBatch = [&] {
        const VnVariableWriteV1 write{harkentest::ObjectId(7), 0.25f, 0};
        VnVariableBatchDescV1 batch{};
        batch.structSize = sizeof(batch);
        batch.writeCount = 1;
        batch.writes = &write;
        VnVariableTicketV1 ticket{};
        ASSERT_EQ(VN_OK, Vn_SetValues(&batch, &ticket));
        VnVariableTicketQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.ticket = ticket;
        VnVariableTicketQueryResultV1 outcome{};
        outcome.structSize = sizeof(outcome);
        ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &outcome));
        ASSERT_EQ(VN_VARIABLE_TICKET_FOUND, outcome.status);
        ASSERT_EQ(VN_VARIABLE_OUTCOME_PENDING, outcome.outcome);
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(session.Engine()));
        ASSERT_EQ(VN_OK, Vn_QueryVariableTicket(&query, &outcome));
        ASSERT_EQ(VN_VARIABLE_TICKET_FOUND, outcome.status);
        ASSERT_EQ(VN_VARIABLE_OUTCOME_APPLIED, outcome.outcome);
    };
    ASSERT_NO_FATAL_FAILURE(writeAndQueryBatch());
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &resolved));
    VnActionPinnedDesc pin = MakePin("Play_Fire", kActionId);
    firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostActionPinned(&pin, &firing));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostActionResolved(&resolved, 0, 0, &firing));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_PostActionResolved(&stale, 0, 0, &firing));
    EXPECT_EQ(0u, firing);
    ASSERT_NO_FATAL_FAILURE(writeAndQueryBatch());
}

// U3-10b 只覆盖失败调用的 journal/replay；成功 Activate 的组合回放留给 U3-11。
TEST(ActionNamePost, JournalsNameApiRefusals128To131WithoutCatalog) {
    VnActionResolved stale{};
    stale.structSize = sizeof(stale);
    {
        EngineSession prior(MakeNullConfig());
        ASSERT_EQ(VN_OK, prior.InitResult());
        VnPackHandle init = 0, content = 0;
        ASSERT_NO_FATAL_FAILURE(LoadInitAndContent(&init, &content));
        ASSERT_EQ(VN_OK, ResolveByName("Play_Fire", &stale));
    }
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string path =
        (std::filesystem::path(HARKEN_TEMP_DIR) / "plan13-name-post" /
         "name-api-refusals.vtrc")
            .string();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    const VnVariableWriteV1 write{harkentest::ObjectId(7), 0.25f, 0};
    VnVariableBatchDescV1 batch{};
    batch.structSize = sizeof(batch);
    batch.writeCount = 1;
    batch.writes = &write;
    VnVariableTicketV1 ticket{};
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValues(&batch, &ticket));
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, PostByName("Play_Fire", &firing));
    EXPECT_EQ(0u, firing);
    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, ResolveByName("Play_Fire", &resolved));
    VnActionPinnedDesc pin = MakePin("Play_Fire", kActionId);
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_PostActionPinned(&pin, &firing));
    EXPECT_EQ(0u, firing);
    EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE,
              Vn_PostActionResolved(&stale, 0, 0, &firing));
    EXPECT_EQ(0u, firing);
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    ASSERT_GE(bytes.size(), kTraceFixedHeaderBytes);
    std::uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.data() + 8, sizeof(headerBytes));
    bool saw[4] = {};
    std::size_t offset = headerBytes;
    while (offset + kTraceRecordHeaderBytes <= bytes.size()) {
        std::uint32_t payloadBytes = 0;
        std::uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.data() + offset, sizeof(payloadBytes));
        std::memcpy(&type, bytes.data() + offset + 4, sizeof(type));
        offset += kTraceRecordHeaderBytes;
        ASSERT_LE(payloadBytes, bytes.size() - offset);
        if (type == kTraceRecordApiCall && payloadBytes >= 2) {
            std::uint16_t callId = 0;
            std::memcpy(&callId, bytes.data() + offset, sizeof(callId));
            if (callId >= kTraceCallPostActionByName &&
                callId <= kTraceCallPostActionResolved) {
                saw[callId - kTraceCallPostActionByName] = true;
            }
        }
        offset += payloadBytes;
    }
    EXPECT_TRUE(saw[0]);
    EXPECT_TRUE(saw[1]);
    EXPECT_TRUE(saw[2]);
    EXPECT_TRUE(saw[3]);

    std::string command = std::string("\"") + HARKEN_HOST_PATH +
        "\" trace-replay \"" + path + "\" 2>&1";
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

TEST(ActionNamePost, TraceStartActivateThenPostByNameJournals128After135) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string path =
        (std::filesystem::path(HARKEN_TEMP_DIR) / "plan13-name-post" /
         "activate-then-byname-128-after-135.vtrc")
            .string();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    ASSERT_EQ(VN_OK, Vn_TraceStart(path.c_str()));
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    ASSERT_NO_FATAL_FAILURE(LoadInitAndContent(&initHandle, &contentHandle));
    ASSERT_NE(0u, initHandle);
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, PostByName("Play_Fire", &firing));
    ASSERT_NE(0u, firing);
    const VnActionFiringHandle postedFiring = firing;
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    ASSERT_EQ(VN_OK, Vn_TraceStop());

    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    std::vector<TraceApiCall> calls;
    ASSERT_TRUE(CollectTraceApiCalls(bytes, &calls));
    std::size_t activateIndex = calls.size();
    std::size_t byNameIndex = calls.size();
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (activateIndex == calls.size() &&
            calls[i].callId == kTraceCallActivateCanonicalPackSet) {
            activateIndex = i;
        }
        if (byNameIndex == calls.size() &&
            calls[i].callId == kTraceCallPostActionByName) {
            byNameIndex = i;
        }
    }
    ASSERT_LT(activateIndex, calls.size());
    ASSERT_LT(byNameIndex, calls.size());
    EXPECT_LT(activateIndex, byNameIndex);
    const auto bound = std::find_if(calls.begin() + activateIndex, calls.begin() + byNameIndex,
        [](const auto& call) { return call.callId == kTraceCallSetGameplayCatalog; });
    EXPECT_NE(bound, calls.begin() + byNameIndex);
    std::uint32_t journalResult = 0xFFFFFFFFu;
    std::uint64_t journalFiring = 0;
    ASSERT_TRUE(ReadPostActionByName128SuccessTail(
        bytes, calls[byNameIndex], &journalResult, &journalFiring));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_OK), journalResult);
    EXPECT_EQ(postedFiring, journalFiring);
    EXPECT_NE(0u, journalFiring);
    std::string command = std::string("\"") + HARKEN_HOST_PATH +
        "\" trace-replay \"" + path + "\" 2>&1";
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

