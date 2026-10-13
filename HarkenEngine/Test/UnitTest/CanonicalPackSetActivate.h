#pragma once

#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"
#include "Engine.h"
#include "PlayableRuntime.h"

#include <harken/Harken.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/Utf8Path.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "harken/param/ParamTargetTable.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace harkentest {

inline VnResult ActivateCanonicalMembersV1(
    const VnCanonicalPackSetMemberV1* members, std::uint32_t count,
    VnCanonicalPackSetMemberResultV1* rows,
    VnCanonicalPackSetActivationReportV1& report, VnPackHandle previous = 0,
    std::uint64_t base = 0) {
    VnCanonicalPackSetActivationDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.expectedCurrentInitHandle = previous;
    desc.baseSetRevision = base;
    desc.setRevision = base + 1;
    desc.memberCount = count;
    desc.memberStructSize = sizeof(*members);
    desc.members = members;
    desc.outputMemberCapacity = count;
    desc.outputMemberStructSize = sizeof(*rows);
    desc.outputMembers = rows;
    report.structSize = sizeof(report);
    return Vn_ActivateCanonicalPackSetV1(&desc, &report);
}

inline VnResult ActivateInitLoadDescV1(const VnPackLoadDesc& load,
                                       std::uint64_t packStableId,
                                       VnPackHandle* outHandle) {
    if (outHandle != nullptr) {
        *outHandle = 0;
    }
    if (load.structSize != sizeof(load) ||
        (load.flags &
         ~(VN_PACK_LOAD_FLAG_PROVIDER | VN_PACK_LOAD_FLAG_ROOT)) != 0) {
        return VN_INVALID_ARG;
    }
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = packStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.flags = load.flags;
    member.utf8Path = load.utf8Path;
    member.utf8ContainmentRoot = load.utf8ContainmentRoot;
    member.providerUserData = load.providerUserData;
    member.provide = load.provide;
    member.release = load.release;
    VnCanonicalPackSetMemberResultV1 memberResult{};
    memberResult.structSize = sizeof(memberResult);
    VnCanonicalPackSetActivationReportV1 report{};
    const VnResult result =
        ActivateCanonicalMembersV1(&member, 1, &memberResult, report);
    if (outHandle != nullptr) {
        *outHandle = report.initHandle;
    }
    return result;
}

inline VnResult ActivateExistingInitPackV1(const char* utf8Path,
                                           std::uint64_t packStableId,
                                           VnPackHandle* outHandle) {
    VnPackLoadDesc load{};
    load.structSize = sizeof(load);
    load.utf8Path = utf8Path;
    return ActivateInitLoadDescV1(load, packStableId, outHandle);
}

inline const harken::PreviewMasterHolderV1* PackMasterHolder(
    harken::Engine* engine) {
    if (engine == nullptr) {
        return nullptr;
    }
    for (const auto& holder : engine->previewMasterHolders) {
        if (holder.occupied &&
            holder.ownerId == harken::kCanonicalPackSetMasterOwnerId) {
            return &holder;
        }
    }
    return nullptr;
}

inline bool PackAdmissionOpenForInit(harken::Engine* engine,
                                     VnPackHandle initHandle) {
    if (engine == nullptr || initHandle == 0) {
        return false;
    }
    for (const auto& row : engine->currentWorldBusAdmission) {
        if (row.occupied &&
            row.key.tag == harken::kBusAdmissionTagPackActivation &&
            row.key.fields[0] == initHandle &&
            row.state == harken::BusAdmissionClaimState::Open) {
            return true;
        }
    }
    return false;
}

inline std::vector<std::uint8_t> BuildMasterOnlyInitBytes(
    std::uint64_t packStableId = packfoundation::kInitPackStableId,
    const packfoundation::RuntimeBusFixtureV1& bus =
        packfoundation::DefaultRuntimeBusFixtureV1()) {
    packfoundation::ImageBuilder builder(harken::kPackKindInit);
    std::uint64_t bodyBytes = 0;
    std::vector<std::uint8_t> partition = packfoundation::PartitionPayload(
        packStableId, 1, 0, 0, &bodyBytes, 64, {}, bus.triple);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, packStableId,
                     packfoundation::RequirementPayload(
                         packStableId, 1, 1, 0, bodyBytes, bus.triple));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition,
                     packStableId, std::move(partition));
    builder.AddEntry(harken::kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    return builder.Build();
}

inline std::vector<std::uint8_t> BuildMasterBus2InitBytes(
    std::uint64_t packStableId = packfoundation::kInitPackStableId) {
    return BuildMasterOnlyInitBytes(packStableId,
                                    packfoundation::Bus2RuntimeBusFixtureV1());
}

inline VnBindingDesc MakeCanonicalBusVolumeBindingDesc(
    VnVariableId variableId, std::uint32_t transitionMs = 0) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = variableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = harken::kBusTargetBusVolume;
    desc.transitionMs = transitionMs;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, -6.0f};
    desc.curve.points[1] = {1.0f, 0.0f};
    desc.curve.handles[0] = EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

inline VnBindingDesc MakeCanonicalVoiceVolumeBindingDesc(
    VnVariableId variableId, std::uint32_t transitionMs = 0) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = variableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = VN_TARGET_VOICE_VOLUME;
    desc.transitionMs = transitionMs;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, -6.0f};
    desc.curve.points[1] = {1.0f, 0.0f};
    desc.curve.handles[0] = EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

inline VnBindingDesc MakeCanonicalVoiceEffectParamBindingDesc(
    VnVariableId variableId, std::uint32_t packedTargetId) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = variableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    desc.targetId = packedTargetId;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 1.0f};
    desc.curve.handles[0] = EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

inline std::vector<std::uint8_t> WithCanonicalBindingRows(
    const std::vector<std::uint8_t>& original, VnBindingId bindingId,
    const VnBindingDesc& desc, const harken::PackBindingAttachmentEntry* attachmentOrNull) {
    using harken::kPackEntryKindBindingAttachment;
    using harken::kPackEntryKindBindingDefinition;
    using harken::kPackV2DirectoryEntryBytes;
    using harken::kPackV2SchemaVintage;
    using harken::ParsePackV2DirectoryEntry;
    using harken::ParsePackV2Header;
    using harken::SerializePackV2BindingAttachment;
    using harken::SerializePackV2BindingDefinition;
    harken::PackV2Header header;
    if (!ParsePackV2Header(original.data(), &header)) {
        return {};
    }
    packfoundation::ImageBuilder builder(header.packKind);
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = ParsePackV2DirectoryEntry(
            original.data() + header.directoryOffset + i * kPackV2DirectoryEntryBytes);
        const std::uint32_t vintage =
            entry.vintage == 0 ? harken::kPackCanonicalEntryVintage : entry.vintage;
        const auto copied = builder.AddEntry(
            entry.kind, static_cast<VnObjectId>(entry.id),
            std::vector<std::uint8_t>(original.begin() + static_cast<std::size_t>(entry.offset),
                                      original.begin() + static_cast<std::size_t>(entry.offset + entry.size)),
            vintage);
        builder.SetRelationOrdinal(copied, entry.relationOrdinal);
    }
    std::vector<std::uint8_t> definition(harken::kPackV2BindingDefinitionPayloadBytes);
    SerializePackV2BindingDefinition(bindingId, desc, definition.data());
    builder.AddEntry(kPackEntryKindBindingDefinition, bindingId, std::move(definition),
                     kPackV2SchemaVintage);
    if (attachmentOrNull != nullptr) {
        std::vector<std::uint8_t> attachment(harken::kPackV2BindingAttachmentPayloadBytes);
        SerializePackV2BindingAttachment(*attachmentOrNull, attachment.data());
        const auto index = builder.AddEntry(kPackEntryKindBindingAttachment, VnObjectId{},
                                            std::move(attachment), kPackV2SchemaVintage);
        builder.SetRelationOrdinal(index, 0);
    }
    return builder.Build();
}

inline std::vector<std::uint8_t> BuildMasterInitWithBusBindingBytes(
    VnBindingId bindingId, VnVariableId variableId, VnObjectId busId,
    const packfoundation::RuntimeBusFixtureV1& bus =
        packfoundation::DefaultRuntimeBusFixtureV1()) {
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = bindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS;
    attachment.objectId = busId;
    return WithCanonicalBindingRows(
        BuildMasterOnlyInitBytes(packfoundation::kInitPackStableId, bus), bindingId,
        MakeCanonicalBusVolumeBindingDesc(variableId), &attachment);
}

inline std::vector<std::uint8_t> BuildContentWithVoiceWorldBytes(
    std::uint64_t contentStableId, std::uint64_t contentRevision,
    std::uint64_t initStableId, std::uint64_t initContentRevision,
    VnObjectId playableId, VnVariableId variableId) {
    using harken::ApplyCanonicalBusTriple;
    using harken::ComputeCurrentWorldFingerprintV4;
    using harken::ComputePlayablePropertyFingerprintV1;
    using harken::ContainerWorldInstallViewV1;
    using harken::EncodeCurrentWorldShippingBody;
    using harken::Fnv1a64;
    using harken::kCanonicalDefinitionPartitionBodyVintage;
    using harken::kCanonicalDefinitionPartitionHeaderV2Bytes;
    using harken::kPackCanonicalRequirementVintage;
    using harken::kPackEntryKindCanonicalDefinitionPartition;
    using harken::kPackEntryKindPackDependencyEdge;
    using harken::kPackEntryKindPackRequirementBlock;
    using harken::kPackKindCanonicalContent;
    using harken::kResolvedSectionGroupPropertyMaskV1;
    using harken::kWorldCapabilityPlayableProperties;
    using harken::kWorldCapabilityVariableDefinitions;
    using harken::PackCanonicalPartitionHeader;
    using harken::PlayablePropertyProgramViewV1;
    using harken::SerializePackCanonicalPartitionHeaderV2;
    using harken::VariableDefinitionV1;
    const auto triple = packfoundation::DefaultFixtureBusTriple();
    PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind = harken::playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = playableId;
    property.contentRevision = 1;
    property.routeStableId = VN_BUS_MASTER;
    property.compatibilityFingerprint = 1;
    property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
    property.payload.values[3].bits = 1;
    property.payload.values[5].bits = std::bit_cast<std::uint32_t>(0.0f);
    property.payload.values[6].objectId = VN_BUS_MASTER;
    property.payload.values[8].bits = std::bit_cast<std::uint32_t>(0.0f);
    property.payload.values[9].bits = std::bit_cast<std::uint32_t>(0.0f);
    property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    property.programSemanticFingerprint = ComputePlayablePropertyFingerprintV1(property);
    VariableDefinitionV1 variable{};
    variable.variableId = variableId;
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = -1.0f;
    variable.rangeMax = 1.0f;
    ContainerWorldInstallViewV1 world{};
    world.identity.capabilityMask =
        kWorldCapabilityPlayableProperties | kWorldCapabilityVariableDefinitions;
    world.playableProperties = &property;
    world.playablePropertyCount = 1;
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    if (!EncodeCurrentWorldShippingBody(world, body)) {
        return {};
    }
    PackCanonicalPartitionHeader header{};
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = contentStableId;
    header.partitionRevision = contentRevision;
    header.ownedDefinitionCount = 2;
    ApplyCanonicalBusTriple(&header, triple);
    std::vector<std::uint8_t> partition(
        kCanonicalDefinitionPartitionHeaderV2Bytes + body.size(), 0);
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    header.headerChecksum = Fnv1a64(partition.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, partition.data());
    std::memcpy(partition.data() + kCanonicalDefinitionPartitionHeaderV2Bytes, body.data(),
                body.size());
    packfoundation::ImageBuilder builder(kPackKindCanonicalContent);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, contentStableId,
                     packfoundation::RequirementPayload(contentStableId, contentRevision,
                                                        contentRevision, 2, body.size(),
                                                        triple));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, contentStableId,
                     std::move(partition));
    builder.AddEntry(kPackEntryKindPackDependencyEdge, VnObjectId{},
                     packfoundation::DependencyPayload(initStableId, initContentRevision,
                                                       triple),
                     kPackCanonicalRequirementVintage);
    return builder.Build();
}

inline std::vector<std::uint8_t> BuildContentWithVoiceBindingBytes(
    VnBindingId bindingId, VnVariableId variableId, VnObjectId playableId) {
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = bindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_VOICE;
    attachment.objectId = playableId;
    return WithCanonicalBindingRows(
        BuildContentWithVoiceWorldBytes(
            packfoundation::kResidentPackStableId, 1, packfoundation::kInitPackStableId, 1,
            playableId, variableId),
        bindingId, MakeCanonicalVoiceVolumeBindingDesc(variableId), &attachment);
}

inline std::vector<std::uint8_t> BuildContentWithBusBindingBytes(
    VnBindingId bindingId, VnVariableId variableId, VnObjectId busId,
    VnObjectId playableId = harkentest::ObjectId(1234)) {
    harken::PackBindingAttachmentEntry attachment{};
    attachment.bindingId = bindingId;
    attachment.scopeKind = VN_TARGET_SCOPE_BUS;
    attachment.objectId = busId;
    return WithCanonicalBindingRows(
        BuildContentWithVoiceWorldBytes(
            packfoundation::kResidentPackStableId, 1, packfoundation::kInitPackStableId, 1,
            playableId, variableId),
        bindingId, MakeCanonicalBusVolumeBindingDesc(variableId), &attachment);
}

inline std::vector<std::uint8_t> BuildContentWithVoiceEffectParamBytes(
    VnBindingId bindingId, VnVariableId variableId, std::uint32_t packedTargetId,
    VnObjectId playableId = harkentest::ObjectId(1234)) {
    return WithCanonicalBindingRows(
        BuildContentWithVoiceWorldBytes(
            packfoundation::kResidentPackStableId, 1, packfoundation::kInitPackStableId, 1,
            playableId, variableId),
        bindingId, MakeCanonicalVoiceEffectParamBindingDesc(variableId, packedTargetId),
        nullptr);
}

inline std::vector<std::uint8_t> BuildMasterSendInitBytes(
    float levelDb,
    std::uint64_t packStableId = packfoundation::kInitPackStableId,
    std::uint32_t sendRepeat = 1) {
    return BuildMasterOnlyInitBytes(
        packStableId,
        packfoundation::MasterSendRuntimeBusFixtureV1(levelDb, sendRepeat));
}

inline bool WriteBytes(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

struct ActivateInitPathResult {
    VnResult result = VN_NOT_INITIALIZED;
    VnPackHandle initHandle = 0;
    VnCanonicalPackSetActivationReportV1 report{};
    std::string initPath;
};

inline ActivateInitPathResult ActivateInitBytesV1(
    const std::filesystem::path& directory, const std::vector<std::uint8_t>& bytes,
    std::uint64_t packStableId) {
    ActivateInitPathResult out;
    out.initPath = (directory / "Init.pack").string();
    if (!WriteBytes(directory / "Init.pack", bytes)) {
        out.result = VN_DEVICE_ERROR;
        return out;
    }
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = packStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.utf8Path = out.initPath.c_str();
    VnCanonicalPackSetMemberResultV1 memberResult{};
    memberResult.structSize = sizeof(memberResult);
    VnCanonicalPackSetActivationDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.setRevision = 1;
    desc.memberCount = 1;
    desc.memberStructSize = sizeof(member);
    desc.members = &member;
    desc.outputMemberCapacity = 1;
    desc.outputMemberStructSize = sizeof(memberResult);
    desc.outputMembers = &memberResult;
    out.report.structSize = sizeof(out.report);
    out.result = Vn_ActivateCanonicalPackSetV1(&desc, &out.report);
    out.initHandle = out.report.initHandle;
    return out;
}

inline ActivateInitPathResult ActivateInitPathV1(
    const std::filesystem::path& directory,
    std::uint64_t packStableId = packfoundation::kInitPackStableId) {
    return ActivateInitBytesV1(directory, BuildMasterOnlyInitBytes(packStableId),
                               packStableId);
}

inline ActivateInitPathResult ActivateInitPathWithBus2V1(
    const std::filesystem::path& directory,
    std::uint64_t packStableId = packfoundation::kInitPackStableId) {
    return ActivateInitBytesV1(directory, BuildMasterBus2InitBytes(packStableId),
                               packStableId);
}

} // namespace harkentest
