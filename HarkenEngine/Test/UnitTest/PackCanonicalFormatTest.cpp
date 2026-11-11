#include "Common/ObjectIdTestSupport.h"
#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/VariableExecutionFingerprint.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "harken/media/CanonicalAttenuationFingerprint.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace harken;

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) {
    return (value + align - 1) / align * align;
}

class ImageBuilder {
public:
    explicit ImageBuilder(std::uint32_t packKind) : packKind_(packKind) {}

    std::size_t AddEntry(std::uint32_t kind, std::uint64_t packed,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = 0) {
        if (kind == kPackEntryKindBusDefinitionSnapshot ||
            kind == kPackEntryKindBindingAttachment ||
            kind == kPackEntryKindPackDependencyEdge) {
            const auto index = AddEntry(kind, VnObjectId{}, std::move(payload), vintage);
            SetRelationOrdinal(index, packed);
            return index;
        }
        return AddEntry(kind, harkentest::ObjectId(packed), std::move(payload), vintage);
    }

    std::size_t AddEntry(std::uint32_t kind, VnObjectId id,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = 0) {
        if (vintage == 0) {
            vintage = PackEntryVintage(kind);
        }
        Entry entry;
        entry.kind = kind;
        entry.vintage = vintage;
        entry.id = id;
        entry.payload = std::move(payload);
        entries_.push_back(std::move(entry));
        return entries_.size() - 1;
    }

    void SetRelationOrdinal(std::size_t index, std::uint64_t ordinal) {
        entries_[index].relationOrdinal = ordinal;
    }

    std::vector<std::uint8_t> Build() {
        const std::uint64_t dirOffset = kPackFileHeaderBytes;
        const std::uint64_t dirBytes =
            static_cast<std::uint64_t>(entries_.size()) *
            kPackDirectoryEntryBytes;
        const std::uint64_t dirEnd = dirOffset + dirBytes;
        payloadOffsets_.assign(entries_.size(), 0);
        std::uint64_t cursor = AlignUp(dirEnd, kPackPayloadAlign);
        std::uint64_t contentEnd = dirEnd;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            payloadOffsets_[i] = cursor;
            contentEnd = cursor + entries_[i].payload.size();
            cursor = AlignUp(contentEnd, kPackPayloadAlign);
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(contentEnd), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            std::memcpy(bytes.data() +
                            static_cast<std::size_t>(payloadOffsets_[i]),
                        entries_[i].payload.data(), entries_[i].payload.size());
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            PackDirectoryEntry record;
            record.kind = entries_[i].kind;
            record.vintage = entries_[i].vintage;
            record.id = entries_[i].id;
            record.relationOrdinal = entries_[i].relationOrdinal;
            record.offset = payloadOffsets_[i];
            record.size = entries_[i].payload.size();
            record.payloadChecksum = Fnv1a64(
                bytes.data() + static_cast<std::size_t>(payloadOffsets_[i]),
                static_cast<std::size_t>(record.size));
            SerializePackDirectoryEntry(
                record, bytes.data() + static_cast<std::size_t>(dirOffset) +
                            i * kPackDirectoryEntryBytes);
        }
        PackFileHeader header;
        header.formatVersion = kPackFormatVersion;
        header.headerBytes = kPackFileHeaderBytes;
        header.fileBytes = contentEnd;
        header.packKind = packKind_;
        header.entryCount = static_cast<std::uint32_t>(entries_.size());
        header.directoryOffset = dirOffset;
        header.directoryChecksum =
            Fnv1a64(bytes.data() + static_cast<std::size_t>(dirOffset),
                    static_cast<std::size_t>(dirBytes));
        SerializePackFileHeader(header, bytes.data());
        return bytes;
    }

    std::uint64_t PayloadOffset(std::size_t i) const {
        return payloadOffsets_[i];
    }

private:
    struct Entry {
        std::uint32_t kind = 0;
        std::uint32_t vintage = 0;
        VnObjectId id{};
        std::uint64_t relationOrdinal = 0;
        std::vector<std::uint8_t> payload;
    };

    std::uint32_t packKind_ = kPackKindInit;
    std::vector<Entry> entries_;
    std::vector<std::uint64_t> payloadOffsets_;
};

void RecomputeChecksums(std::vector<std::uint8_t>* bytes,
                        bool includePayloadChecksums = true) {
    PackFileHeader header;
    ASSERT_TRUE(ReadPackFileHeader(bytes->data(), &header));
    const std::size_t dir = static_cast<std::size_t>(header.directoryOffset);
    for (std::uint32_t i = 0; includePayloadChecksums && i < header.entryCount; ++i) {
        std::uint8_t* record =
            bytes->data() + dir + i * kPackDirectoryEntryBytes;
        PackDirectoryEntry entry = ReadPackDirectoryEntry(record);
        entry.payloadChecksum =
            Fnv1a64(bytes->data() + static_cast<std::size_t>(entry.offset),
                    static_cast<std::size_t>(entry.size));
        SerializePackDirectoryEntry(entry, record);
    }
    header.directoryChecksum = Fnv1a64(
        bytes->data() + dir,
        static_cast<std::size_t>(header.entryCount) * kPackDirectoryEntryBytes);
    SerializePackFileHeader(header, bytes->data());
}

template <typename Mutator>
void RewriteCanonicalPartitionWorld(std::vector<std::uint8_t>* bytes,
                                    Mutator mutate) {
    ASSERT_NE(bytes, nullptr);
    PackFileHeader packHeader;
    ASSERT_TRUE(ReadPackFileHeader(bytes->data(), &packHeader));
    bool found = false;
    const std::size_t directoryOffset =
        static_cast<std::size_t>(packHeader.directoryOffset);
    for (std::uint32_t i = 0; i < packHeader.entryCount; ++i) {
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            bytes->data() + directoryOffset +
            i * kPackDirectoryEntryBytes);
        if (entry.kind != kPackEntryKindCanonicalDefinitionPartition) {
            continue;
        }
        ASSERT_FALSE(found);
        std::uint8_t* payload =
            bytes->data() + static_cast<std::size_t>(entry.offset);
        PackCanonicalPartitionHeader partitionHeader;
        ASSERT_TRUE(ReadPackCanonicalPartitionHeader(
            payload, entry.size, &partitionHeader));
        CurrentWorldBodyImage decoded;
        ASSERT_EQ(CurrentWorldBodyDecodeResult::Ok,
                  decoded.DecodeWithResult(
                      payload + partitionHeader.headerBytes,
                      static_cast<std::size_t>(partitionHeader.bodyBytes),
                      partitionHeader.bodyVintage,
                      CurrentWorldDecodeMode::StrictArtifact));
        ContainerWorldInstallViewV1 world = *decoded.ProductionView();
        mutate(&world);
        std::vector<std::uint8_t> replacement;
        ASSERT_TRUE(EncodeCurrentWorldShippingBody(world, replacement));
        ASSERT_EQ(partitionHeader.bodyBytes, replacement.size());
        std::memcpy(payload + partitionHeader.headerBytes,
                    replacement.data(), replacement.size());
        partitionHeader.bodyChecksum =
            Fnv1a64(replacement.data(), replacement.size());
        SerializePackCanonicalPartitionHeader(partitionHeader, payload);
        partitionHeader.headerChecksum = Fnv1a64(payload, kPackCanonicalPartitionHeaderHeaderChecksumOffset);
        SerializePackCanonicalPartitionHeader(partitionHeader, payload);
        found = true;
    }
    ASSERT_TRUE(found);
    RecomputeChecksums(bytes);
}



void ApplyNeutralMasterTriple(PackRequirementBlock* block,
                              CanonicalBusTriple triple =
                                  harkentest::packfoundation::DefaultFixtureBusTriple()) {
    block->busContentGeneration = triple.busContentGeneration;
    block->publishedBusFingerprint = triple.publishedBusFingerprint;
    block->compileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
}

void ApplyNeutralMasterTriple(PackCanonicalPartitionHeader* header,
                              CanonicalBusTriple triple =
                                  harkentest::packfoundation::DefaultFixtureBusTriple()) {
    header->busContentGeneration = triple.busContentGeneration;
    header->publishedBusFingerprint = triple.publishedBusFingerprint;
    header->compileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
}

std::vector<std::uint8_t> RequirementPayload(std::uint64_t packStableId,
                                             std::uint64_t contentRevision,
                                             std::uint64_t partitionRevision,
                                             std::uint32_t vintage = kPackPackRequirementBlockVintage) {
    PackRequirementBlock block;
    block.schemaVersion = vintage;
    block.storageModelVersion = vintage;
    block.packStableId = packStableId;
    block.packContentRevision = contentRevision;
    block.definitionPartitionRevision = partitionRevision;
    block.resourceClosureRevision = 1;
    ApplyNeutralMasterTriple(&block);
    std::vector<std::uint8_t> payload(kPackRequirementBlockBytes, 0);
    SerializePackRequirementBlock(block, payload.data());
    return payload;
}



std::vector<std::uint8_t> PartitionPayload(std::uint64_t packStableId,
                                           std::uint64_t revision,
                                           std::uint32_t bodyVintage) {
    ContainerWorldInstallViewV1 world{};
    world.identity.runtimeSemanticFingerprint =
        ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    PackCanonicalPartitionHeader header;
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = bodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = 0;
    ApplyNeutralMasterTriple(&header);
    std::vector<std::uint8_t> payload(header.headerBytes + body.size(), 0);
    SerializePackCanonicalPartitionHeader(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, payload.data());
    std::memcpy(payload.data() + header.headerBytes,
                body.data(), body.size());
    return payload;
}

void SetAbsoluteTime(AuthoredTimeRationalV1* time,
                     std::int64_t nanoseconds = 0) {
    time->domain = AuthoredTimeDomainV1::AbsoluteNanoseconds;
    time->numerator = nanoseconds;
    time->denominator = 1;
}

std::vector<std::uint8_t> ActionTargetPartitionPayload(
    std::uint64_t packStableId, std::uint64_t revision,
    bool includeLocalTarget, bool mismatchLocalTarget) {
    constexpr VnObjectId kSoundId = harkentest::ObjectId(3101);
    constexpr VnObjectId kActionId = harkentest::ObjectId(4101);
    constexpr std::uint64_t kExternalFingerprint = 0x123456789abcdef0ull;

    PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = harkentest::ObjectId(kSoundId);
    property.contentRevision = 1;
    property.routeStableId = harkentest::ObjectId(2);
    property.compatibilityFingerprint = 3;
    property.payload.presentMask = kResolvedSectionGroupPropertyMaskV1;
    property.payload.values[3].bits = 1;
    property.payload.values[6].objectId = VN_BUS_MASTER;
    property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    property.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    property.programSemanticFingerprint =
        ComputePlayablePropertyFingerprintV1(property);

    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::Play;
    step.target.kind = ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::ObjectId(kSoundId);
    step.target.expectedProgramSemanticFingerprint = includeLocalTarget
        ? property.programSemanticFingerprint ^
              (mismatchLocalTarget ? 1ull : 0ull)
        : kExternalFingerprint;
    SetAbsoluteTime(&step.delay);
    SetAbsoluteTime(&step.fade);
    SetAbsoluteTime(&step.cooldown);

    ActionProgramViewV1 action{};
    action.actionStableId = harkentest::ObjectId(kActionId);
    action.steps = &step;
    action.stepCount = 1;
    action.expandedStepOccurrenceCount = 1;
    action.expandedFiringCount = 1;
    action.programSemanticFingerprint =
        ComputeActionProgramFingerprintV1(action);

    ContainerWorldInstallViewV1 world{};
    world.identity.capabilityMask = kWorldCapabilityActionDefinitions;
    if (includeLocalTarget) {
        world.identity.capabilityMask |= kWorldCapabilityPlayableProperties;
        world.playableProperties = &property;
        world.playablePropertyCount = 1;
    }
    world.actions = &action;
    world.actionCount = 1;
    world.identity.runtimeSemanticFingerprint =
        ComputeCurrentWorldFingerprintV4(world);

    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    PackCanonicalPartitionHeader header;
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = includeLocalTarget ? 2 : 1;
    ApplyNeutralMasterTriple(&header);
    std::vector<std::uint8_t> payload(
        kPackCanonicalPartitionHeaderBytes + body.size(), 0);
    SerializePackCanonicalPartitionHeader(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, payload.data());
    std::memcpy(payload.data() + kPackCanonicalPartitionHeaderBytes,
                body.data(), body.size());
    return payload;
}

std::vector<std::uint8_t> AdvancedTransitionPartitionPayload(
    std::uint64_t packStableId, std::uint64_t revision) {
    const auto playablePayload = [](std::uint64_t presentMask) {
        PlayablePropertyPayloadViewV1 payload{};
        payload.presentMask = presentMask;
        payload.values[3].bits = 1;
        payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
        payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
        return payload;
    };

    PlayablePropertyProgramViewV1 properties[2]{};
    properties[0].contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Sound;
    properties[0].stableId = harkentest::ObjectId(10);
    properties[0].contentRevision = 101;
    properties[0].routeStableId = harkentest::ObjectId(201);
    properties[0].compatibilityFingerprint = 301;
    properties[0].payload = playablePayload(1 | kResolvedSectionGroupPropertyMaskV1);
    properties[0].payload.values[6].objectId = VN_BUS_MASTER;
    properties[0].programSemanticFingerprint =
        ComputePlayablePropertyFingerprintV1(properties[0]);
    properties[1].contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Container;
    properties[1].authoredStableOrder = 1;
    properties[1].stableId = harkentest::ObjectId(20);
    properties[1].contentRevision = 102;
    properties[1].routeStableId = harkentest::ObjectId(202);
    properties[1].compatibilityFingerprint = 302;
    properties[1].payload = playablePayload(kResolvedSectionGroupPropertyMaskV1);
    properties[1].payload.values[6].objectId = VN_BUS_MASTER;
    properties[1].programSemanticFingerprint =
        ComputePlayablePropertyFingerprintV1(properties[1]);

    ContainerChildProgramViewV2 child{};
    child.playable.kind = PlayableKindV1::Sound;
    child.playable.stableId = properties[0].stableId;
    child.playable.expectedProgramSemanticFingerprint =
        properties[0].programSemanticFingerprint;
    child.playable.contentRevision = properties[0].contentRevision;
    child.playable.routeStableId = properties[0].routeStableId;
    child.playable.compatibilityFingerprint =
        properties[0].compatibilityFingerprint;

    PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind =
        playableproperties::ContentPropertyKindV1::Container;
    occurrence.parentStableId = properties[1].stableId;
    occurrence.parentProgramSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    occurrence.childPlayable = child.playable;
    occurrence.payload = playablePayload(1);
    occurrence.occurrenceSemanticFingerprint =
        ComputePlayableOccurrenceFingerprintV1(occurrence);

    TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceExit = TransitionSourceExitV1::NextMarker;
    rule.requiredMarkerStableId = harkentest::ObjectId(0x1234);
    TransitionProgramViewV1 transition{};
    transition.ownerContainerStableId = properties[1].stableId;
    transition.ruleCount = 1;
    transition.rules = &rule;

    ContainerProgramViewV3 container{};
    container.containerStableId = properties[1].stableId;
    container.programSemanticFingerprint =
        properties[1].programSemanticFingerprint;
    container.mode = ContainerModeV1::SequenceQueue;
    container.childCount = 1;
    container.children = &child;
    container.transitionProgramIndex = 0;

    ContainerWorldInstallViewV1 world{};
    world.identity.capabilityMask =
        kWorldCapabilityPlayableProperties |
        kWorldCapabilitySequenceQueue |
        kWorldCapabilityTransition;
    world.playableProperties = properties;
    world.playablePropertyCount = 2;
    world.playableOccurrences = &occurrence;
    world.playableOccurrenceCount = 1;
    world.containers = &container;
    world.containerCount = 1;
    world.transitions = &transition;
    world.transitionCount = 1;
    world.identity.runtimeSemanticFingerprint =
        ComputeCurrentWorldFingerprintV4(world);
    EXPECT_EQ(WorldInstallStatus::CapabilityUnavailable,
              ValidateCurrentWorldV1(world));

    std::vector<std::uint8_t> body;
    EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    PackCanonicalPartitionHeader header;
    header.headerBytes = kPackCanonicalPartitionHeaderBytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = 2;
    ApplyNeutralMasterTriple(&header);
    std::vector<std::uint8_t> payload(
        kPackCanonicalPartitionHeaderBytes + body.size(), 0);
    SerializePackCanonicalPartitionHeader(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), kPackCanonicalPartitionHeaderHeaderChecksumOffset);
    SerializePackCanonicalPartitionHeader(header, payload.data());
    std::memcpy(payload.data() + kPackCanonicalPartitionHeaderBytes,
                body.data(), body.size());
    return payload;
}

std::vector<std::uint8_t> EmptyPartitionPayload(std::uint64_t packStableId,
                                                std::uint64_t revision) {
    return PartitionPayload(packStableId, revision,
                            kCanonicalDefinitionPartitionBodyVintage);
}

void StampRequirementCoverage(std::vector<std::uint8_t>* payload) {
    StoreLE64(payload->data() + 104, Fnv1a64(payload->data(), 104));
}

std::vector<std::uint8_t> BuildMinimalInit() {
    constexpr std::uint64_t kInitId = 11;
    ImageBuilder builder(kPackKindInit);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kInitId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     EmptyPartitionPayload(kInitId, 1));
    harkentest::packfoundation::AppendNeutralMasterSnapshot(&builder);
    return builder.Build();
}

struct Kind26RowSpec {
    VnObjectId variableId = harkentest::ObjectId(0);
    std::uint8_t writers = 1;
    std::uint64_t executionFingerprint = 1;
    std::vector<std::int32_t> discreteAnchors;
    std::vector<VariableLabelDefinitionV1> labels;
};

std::vector<std::uint8_t> Kind26Payload(std::vector<Kind26RowSpec> rows) {
    std::size_t bytes = kPackVariablePolicyHeaderBytes + rows.size() *
        (kPackVariablePolicyRowBytes + kPackVariableAnchorCountBytes + kPackVariableLabelHeaderBytes);
    for (const auto& row : rows) {
        bytes += row.discreteAnchors.size() * kPackVariableAnchorBytes;
        bytes += row.labels.size() * kPackVariableLabelBytes;
    }
    std::vector<std::uint8_t> payload(bytes, 0);
    SerializePackVariablePolicyHeader({static_cast<std::uint32_t>(rows.size())}, payload.data());
    std::size_t cursor = kPackVariablePolicyHeaderBytes;
    for (const auto& row : rows) {
        SerializePackVariablePolicyRow({row.variableId, row.writers, row.executionFingerprint}, payload.data() + cursor);
        cursor += kPackVariablePolicyRowBytes;
    }
    for (const auto& row : rows) {
        SerializePackVariableAnchorCount({static_cast<std::uint32_t>(row.discreteAnchors.size())}, payload.data() + cursor);
        cursor += kPackVariableAnchorCountBytes;
        for (std::int32_t anchor : row.discreteAnchors) {
            SerializePackVariableAnchor(anchor, payload.data() + cursor);
            cursor += kPackVariableAnchorBytes;
        }
    }
    for (const auto& row : rows) {
        const auto mapping = ComputeVariableLabelMappingFingerprintV1(row.variableId, row.labels);
        SerializePackVariableLabelHeader({mapping, static_cast<std::uint32_t>(row.labels.size())}, payload.data() + cursor);
        cursor += kPackVariableLabelHeaderBytes;
        for (const auto& label : row.labels) {
            SerializePackVariableLabel({label.labelId, label.anchor}, payload.data() + cursor);
            cursor += kPackVariableLabelBytes;
        }
    }
    return payload;
}

std::vector<std::uint8_t> BuildInitWithTypedEntry(
    std::uint32_t kind, std::uint64_t id, std::vector<std::uint8_t> payload,
    std::uint32_t vintage = 0) {
    constexpr std::uint64_t kInitId = 11;
    ImageBuilder builder(kPackKindInit);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kInitId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     EmptyPartitionPayload(kInitId, 1));
    if (kind != kPackEntryKindBusDefinitionSnapshot) {
        harkentest::packfoundation::AppendNeutralMasterSnapshot(&builder);
    }
    builder.AddEntry(kind, id, std::move(payload), vintage);
    return builder.Build();
}

std::vector<Kind26RowSpec> TwoOverlayRows() {
    return {
        Kind26RowSpec{harkentest::ObjectId(7), 1, 0x1111111111111111ull, {0, 8}},
        Kind26RowSpec{harkentest::ObjectId(11), 3, 0x2222222222222222ull, {}},
    };
}

struct Kind27RowSpec {
    std::uint8_t identityClass = 1;
    VnObjectId stableId = harkentest::ObjectId(0);
    std::uint64_t expectedExecutionFingerprint = 1;
};

std::vector<std::uint8_t> Kind27Payload(std::vector<Kind27RowSpec> rows) {
    std::vector<std::uint8_t> payload(kPackVariableRequirementsHeaderBytes + rows.size() * kPackVariableRequirementBytes, 0);
    SerializePackVariableRequirementsHeader({static_cast<std::uint32_t>(rows.size())}, payload.data());
    std::size_t offset = kPackVariableRequirementsHeaderBytes;
    for (const auto& row : rows) {
        SerializePackVariableRequirement({row.stableId, row.expectedExecutionFingerprint,
            ComputeVariableLabelMappingFingerprintV1(row.stableId, {})}, payload.data() + offset);
        payload[offset + kPackVariableRequirementIdentityClassOffset] = row.identityClass;
        offset += kPackVariableRequirementBytes;
    }
    return payload;
}

std::vector<Kind27RowSpec> TwoRequirementRows() {
    return {
        Kind27RowSpec{1, harkentest::ObjectId(7), 0x1111111111111111ull},
        Kind27RowSpec{1, harkentest::ObjectId(11), 0x2222222222222222ull},
    };
}

std::vector<std::uint8_t> BuildCanonicalContentWithKind27(
    std::vector<std::uint8_t> payload) {
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement = RequirementPayload(22, 1, 0);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, 22,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindVariableRequirements, 0, std::move(payload));
    return builder.Build();
}

std::vector<std::uint8_t> BuildCanonicalMediaFixture(
    std::initializer_list<std::size_t> frameCounts) {
    harkentest::packfoundation::ImageBuilder builder(kPackKindCanonicalContent);
    auto requirement = RequirementPayload(22, 1, 0);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, std::uint64_t{22},
                     std::move(requirement));
    std::uint64_t soundId = 101;
    for (const auto frames : frameCounts) {
        builder.AddMedia(harkentest::ObjectId(soundId),
                         std::vector<float>(frames,
                                            static_cast<float>(soundId) / 1024.0f));
        ++soundId;
    }
    return builder.Build();
}

void ExpectCanonicalMediaRefusal(const std::vector<std::uint8_t>& bytes,
                                 VnResult expected) {
    const auto expectedHash = Fnv1a64(bytes.data(), bytes.size());
    for (const bool includeArtifactHash : {false, true}) {
        SCOPED_TRACE(includeArtifactHash);
        PackCanonicalImage image;
        image.packStableId = 77;
        image.hasPartition = true;
        image.partitionBody = {1, 2, 3};
        std::uint64_t artifactHash = 0;
        const auto result = includeArtifactHash
            ? ParsePackCanonicalImage(bytes.data(), bytes.size(), &image, &artifactHash)
            : ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
        EXPECT_EQ(expected, result);
        if (includeArtifactHash) EXPECT_EQ(expectedHash, artifactHash);
        EXPECT_EQ(77u, image.packStableId);
        EXPECT_TRUE(image.hasPartition);
        EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3}), image.partitionBody);
        EXPECT_TRUE(image.media.empty());
    }
}

std::filesystem::path CanonicalReadTestPath(const char* filename) {
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) / "canonical-bounded-reader";
    std::filesystem::create_directories(directory);
    return directory / filename;
}

TEST(PackCanonicalFormat, BoundedFileKeepsDirectoryOrderAndFinalSampleIdentity) {
    auto bytes = BuildCanonicalMediaFixture({40006, 17000, 90001});
    PackFileHeader header;
    ASSERT_TRUE(ReadPackFileHeader(bytes.data(), &header));
    auto* firstRecord = bytes.data() + header.directoryOffset + kPackDirectoryEntryBytes;
    auto* lastRecord = firstRecord + 2 * kPackDirectoryEntryBytes;
    const auto first = ReadPackDirectoryEntry(firstRecord);
    const auto last = ReadPackDirectoryEntry(lastRecord);
    auto media = ReadPackMedia(bytes.data() + first.offset);
    media.channelCount = 2;
    media.frameCount /= 2;
    for (std::size_t i = 0; i < media.blobBytes / sizeof(float); ++i) {
        const float sample = static_cast<float>(static_cast<int>(i % 32768) - 16384) * 0x1p-15f;
        std::memcpy(bytes.data() + media.blobOffset + i * sizeof(float), &sample, sizeof(sample));
    }
    media.blobChecksum = Fnv1a64(bytes.data() + media.blobOffset, media.blobBytes);
    SerializePackMedia(media, bytes.data() + first.offset);
    auto finalMedia = ReadPackMedia(bytes.data() + last.offset);
    const float negativeZero = -0.0f;
    std::memcpy(bytes.data() + finalMedia.blobOffset + finalMedia.blobBytes - sizeof(float),
                &negativeZero, sizeof(float));
    finalMedia.blobChecksum = Fnv1a64(bytes.data() + finalMedia.blobOffset, finalMedia.blobBytes);
    SerializePackMedia(finalMedia, bytes.data() + last.offset);
    SerializePackDirectoryEntry(last, firstRecord);
    SerializePackDirectoryEntry(first, lastRecord);
    RecomputeChecksums(&bytes);
    PackCanonicalImage expected;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &expected));
    const auto path = CanonicalReadTestPath("directory-and-bits.pack");
    ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
    PCMRegistry registry;
    PackCanonicalImage actual;
    std::vector<PCMRegistry::PreparedPackSamples> samples;
    std::uint64_t count = 0, hash = 0;
    ASSERT_EQ(VN_OK, ReadPackCanonicalFile(path.string().c_str(), registry, &actual,
                                          &samples, &count, &hash, bytes.size()));
    EXPECT_EQ(bytes.size(), count);
    EXPECT_EQ(Fnv1a64(bytes.data(), bytes.size()), hash);
    ASSERT_EQ(expected.media.size(), samples.size());
    std::vector<const PCMRegistry::PreparedPackSamples*> candidates;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& row = expected.media[i];
        EXPECT_EQ(row.soundId, actual.media[i].soundId);
        const float* planes[kMaxChannels]{};
        for (std::uint32_t ch = 0; ch < row.channelCount; ++ch)
            planes[ch] = reinterpret_cast<const float*>(bytes.data() + row.blobOffset) + ch * row.frameCount;
        VnPCMDesc desc{};
        desc.structSize = sizeof(desc);
        desc.sampleRate = row.sampleRate;
        desc.channels = row.channelCount;
        desc.frames = row.frameCount;
        desc.layout = VN_PCM_LAYOUT_PLANAR;
        desc.policy = VN_PCM_POLICY_REFERENCE;
        desc.samplesPlanar = planes;
        ASSERT_EQ(VN_OK, registry.RegisterPCM(row.soundId, &desc));
        EXPECT_EQ(registry.Lookup(row.soundId).contentRevision, samples[i].View().contentRevision);
        for (std::uint32_t ch = 0; ch < row.channelCount; ++ch) {
            for (std::uint64_t frame = 0; frame < row.frameCount; ++frame) {
                const float value = samples[i].View().ReadSample(ch, frame);
                ASSERT_EQ(0, std::memcmp(&value, planes[ch] + frame, sizeof(float)));
            }
        }
        candidates.push_back(&samples[i]);
    }
    std::vector<PCMRegistry::PreparedPackSamples> duplicate;
    ASSERT_EQ(VN_OK, ReadPackCanonicalFile(path.string().c_str(), registry, &actual,
        &duplicate, &count, &hash, bytes.size(), candidates));
    for (std::size_t i = 0; i < samples.size(); ++i)
        EXPECT_EQ(samples[i].View().channels[0], duplicate[i].View().channels[0]);
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, ReadPackCanonicalFile(path.string().c_str(), registry,
        &actual, &duplicate, &count, &hash, bytes.size() - 1));
    EXPECT_EQ(samples[0].View().channels[0], duplicate[0].View().channels[0]);
    const float changedLast = 0.125f;
    std::memcpy(bytes.data() + finalMedia.blobOffset + finalMedia.blobBytes - sizeof(float),
                &changedLast, sizeof(float));
    finalMedia.blobChecksum = Fnv1a64(bytes.data() + finalMedia.blobOffset, finalMedia.blobBytes);
    SerializePackMedia(finalMedia, bytes.data() + last.offset);
    RecomputeChecksums(&bytes);
    ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
    bool retry = false;
    detail::StablePageReservation::FailNextReservationForTests();
    EXPECT_EQ(VN_BACKPRESSURE, ReadPackCanonicalFile(path.string().c_str(), registry, &actual,
        &duplicate, &count, &hash, bytes.size(), candidates, &retry));
    EXPECT_TRUE(retry);
    EXPECT_EQ(samples[0].View().channels[0], duplicate[0].View().channels[0]);
}

TEST(PackCanonicalFormat, FileDecodeBudgetRetainsSampleRowsAndRefusalPreservesOwners) {
    const auto bytes = BuildCanonicalMediaFixture({1024});
    const auto path = CanonicalReadTestPath("decode-budget.pack");
    ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
    PCMRegistry registry;
    auto budget = detail::MakePackDecodeBudget(1024 * 1024);
    detail::PackDecodeCharge sampleStaging(budget);
    PackCanonicalImage image;
    std::vector<PCMRegistry::PreparedPackSamples> samples;
    std::uint64_t count = 0, hash = 0;
    ASSERT_EQ(VN_OK, ReadPackCanonicalFile(path.string().c_str(), registry, &image,
        &samples, &count, &hash, bytes.size(), {}, nullptr, nullptr, budget, &sampleStaging));
    ASSERT_EQ(1u, samples.size());
    EXPECT_EQ(sizeof(PCMRegistry::PreparedPackSamples), sampleStaging.Bytes());
    EXPECT_EQ(image.staging.Bytes() + sampleStaging.Bytes(), budget->live);
    const auto* priorSamples = samples[0].View().channels[0];
    const auto live = budget->live;
    budget->limit = live;
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, ReadPackCanonicalFile(path.string().c_str(), registry,
        &image, &samples, &count, &hash, bytes.size(), {}, nullptr, nullptr, budget, &sampleStaging));
    EXPECT_EQ(priorSamples, samples[0].View().channels[0]);
    EXPECT_EQ(live, budget->live);
    image = {};
    samples.clear();
    samples.shrink_to_fit();
    sampleStaging.Reset();
    EXPECT_EQ(0u, budget->live);
}

TEST(PackCanonicalFormat, BoundedFileSharesOnlyExactDuplicateMediaWithinFile) {
    for (const bool equal : {true, false}) {
        SCOPED_TRACE(equal);
        auto bytes = BuildCanonicalMediaFixture({90001, 90001});
        PackFileHeader header;
        ASSERT_TRUE(ReadPackFileHeader(bytes.data(), &header));
        auto* firstRecord = bytes.data() + header.directoryOffset + kPackDirectoryEntryBytes;
        auto* secondRecord = firstRecord + kPackDirectoryEntryBytes;
        const auto first = ReadPackDirectoryEntry(firstRecord);
        auto second = ReadPackDirectoryEntry(secondRecord);
        const auto firstMedia = ReadPackMedia(bytes.data() + first.offset);
        auto secondMedia = ReadPackMedia(bytes.data() + second.offset);
        second.id = first.id;
        secondMedia.soundId = firstMedia.soundId;
        std::memcpy(bytes.data() + secondMedia.blobOffset,
                    bytes.data() + firstMedia.blobOffset, firstMedia.blobBytes);
        const float last = equal ? 101.0f / 1024.0f : -0.0f;
        std::memcpy(bytes.data() + secondMedia.blobOffset + secondMedia.blobBytes - sizeof(float),
                    &last, sizeof(last));
        secondMedia.blobChecksum = Fnv1a64(bytes.data() + secondMedia.blobOffset, secondMedia.blobBytes);
        SerializePackMedia(secondMedia, bytes.data() + second.offset);
        SerializePackDirectoryEntry(second, firstRecord);
        SerializePackDirectoryEntry(first, secondRecord);
        RecomputeChecksums(&bytes);
        PackCanonicalImage image;
        ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
        const auto path = CanonicalReadTestPath("same-file-duplicate.pack");
        ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
        PCMRegistry registry;
        std::vector<PCMRegistry::PreparedPackSamples> samples;
        std::uint64_t count = 0, hash = 0;
        bool retry = true;
        ASSERT_EQ(VN_OK, ReadPackCanonicalFile(path.string().c_str(), registry, &image,
            &samples, &count, &hash, bytes.size(), {}, &retry));
        EXPECT_FALSE(retry);
        ASSERT_EQ(2u, samples.size());
        EXPECT_EQ(equal, samples[0].View().channels[0] == samples[1].View().channels[0]);
        const float actual = samples[0].View().ReadSample(0, secondMedia.frameCount - 1);
        EXPECT_EQ(0, std::memcmp(&last, &actual, sizeof(last)));
        EXPECT_EQ(101.0f / 1024.0f, samples[1].View().ReadSample(0, firstMedia.frameCount - 1));
        EXPECT_EQ(bytes.size(), count);
        EXPECT_EQ(Fnv1a64(bytes.data(), bytes.size()), hash);
        EXPECT_EQ(0u, registry.Count());
    }
}

TEST(PackCanonicalFormat, BoundedFileCacheEnvelopeRefusesBeforeOverlappingPayloadAllocation) {
    auto bytes = BuildMinimalInit();
    PackFileHeader header;
    ASSERT_TRUE(ReadPackFileHeader(bytes.data(), &header));
    ASSERT_GT(header.entryCount, 1u);
    auto* directory = bytes.data() + header.directoryOffset;
    const auto first = ReadPackDirectoryEntry(directory);
    auto* lastRecord = directory + (header.entryCount - 1) * kPackDirectoryEntryBytes;
    auto last = ReadPackDirectoryEntry(lastRecord);
    std::uint64_t cached = kPackFileHeaderBytes +
        static_cast<std::uint64_t>(header.entryCount) * kPackDirectoryEntryBytes;
    for (std::uint32_t i = 0; i + 1 < header.entryCount; ++i) {
        const auto entry = ReadPackDirectoryEntry(directory + i * kPackDirectoryEntryBytes);
        cached += std::max<std::uint64_t>(entry.size, 1);
    }
    last.offset = first.offset;
    last.size = bytes.size() - last.offset;
    ASSERT_LE(cached, bytes.size());
    ASSERT_GT(cached + last.size, bytes.size());
    // A distinct overlapping cache key exceeds the encoded envelope. Its bad
    // checksum would refuse earlier than Finish if the payload were allocated.
    last.payloadChecksum = Fnv1a64(bytes.data() + last.offset, last.size) ^ 1;
    SerializePackDirectoryEntry(last, lastRecord);
    RecomputeChecksums(&bytes, false);
    const auto path = CanonicalReadTestPath("cache-envelope.pack");
    ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
    PCMRegistry registry;
    auto budget = detail::MakePackDecodeBudget(1024 * 1024);
    detail::PackDecodeCharge sampleStaging(budget);
    PackCanonicalImage image;
    image.packStableId = 77;
    std::vector<PCMRegistry::PreparedPackSamples> samples;
    std::uint64_t count = 19, hash = 23;
    bool retry = false;
    EXPECT_EQ(VN_BAD_PACK, ReadPackCanonicalFile(path.string().c_str(), registry, &image,
        &samples, &count, &hash, bytes.size(), {}, &retry, nullptr, budget, &sampleStaging));
    EXPECT_TRUE(retry);
    EXPECT_EQ(77u, image.packStableId);
    EXPECT_TRUE(samples.empty());
    EXPECT_EQ(19u, count);
    EXPECT_EQ(23u, hash);
    EXPECT_EQ(0u, budget->live);
    // The requested fallback reaches the ordinary payload checksum rejection.
    EXPECT_EQ(VN_BAD_PACK, ParsePackCanonicalImageWithBudget(bytes.data(), bytes.size(),
        &image, &hash, budget));
    EXPECT_EQ(77u, image.packStableId);
    EXPECT_EQ(0u, budget->live);
}

TEST(PackCanonicalFormat, BoundedFileRejectsCorruptionMetadataChangeAndShortRead) {
    const auto original = BuildCanonicalMediaFixture({90001});
    for (const int fault : {0, 1, 2, 3, 4}) {
        SCOPED_TRACE(fault);
        auto bytes = original;
        if (fault == 0) bytes.back() ^= 1;
        if (fault == 1) bytes.pop_back();
        const auto path = CanonicalReadTestPath("refusal.pack");
        ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
        struct Change { std::filesystem::path path; int fault; bool applied = false; } change{path, fault};
        PackCanonicalReadHooksForTests hooks;
        hooks.userData = &change;
        hooks.beforeSequentialRead = [](void* context, std::uint64_t offset) {
            auto& change = *static_cast<Change*>(context);
            if (change.applied || change.fault < 2 || (change.fault == 4 && offset == 0)) return;
            change.applied = true;
            if (change.fault == 4) {
                std::filesystem::resize_file(change.path, offset + 16);
            } else if (change.fault == 3) {
                std::ofstream append(change.path, std::ios::binary | std::ios::app);
                append.put(0);
            } else {
                std::fstream edit(change.path, std::ios::binary | std::ios::in | std::ios::out);
                edit.seekp(0);
                edit.put('X');
            }
        };
        PCMRegistry registry;
        PackCanonicalImage image;
        image.packStableId = 77;
        std::vector<PCMRegistry::PreparedPackSamples> samples;
        std::uint64_t count = 19, hash = 23;
        bool retry = true;
        EXPECT_EQ(VN_BAD_PACK, ReadPackCanonicalFile(path.string().c_str(), registry, &image,
            &samples, &count, &hash, SIZE_MAX, {}, &retry, &hooks));
        EXPECT_EQ(77u, image.packStableId);
        EXPECT_TRUE(samples.empty());
        EXPECT_FALSE(retry);
        EXPECT_EQ(19u, count);
        EXPECT_EQ(23u, hash);
        EXPECT_EQ(0u, registry.Count());
    }
}

TEST(PackCanonicalFormat, BoundedFileHashesCurrentInitBytes) {
    const auto bytes = BuildMinimalInit();
    PackCanonicalImage parsed;
    const auto path = CanonicalReadTestPath("init-hash.pack");
    PCMRegistry registry;
    std::vector<PCMRegistry::PreparedPackSamples> samples;
    std::uint64_t count = 0, hash = 0;
    ASSERT_TRUE(harkentest::packfoundation::WriteFile(path, bytes));
    ASSERT_EQ(VN_OK, ReadPackCanonicalFile(path.string().c_str(), registry, &parsed,
        &samples, &count, &hash, bytes.size()));
    EXPECT_EQ(Fnv1a64(bytes.data(), bytes.size()), hash);
    EXPECT_TRUE(samples.empty());
}

TEST(PackCanonicalFormat, AdjacentMediaWithLongerFirstBlobPreserveOddTail) {
    const auto bytes = BuildCanonicalMediaFixture({7, 3, 5});
    const auto expectedHash = Fnv1a64(bytes.data(), bytes.size());
    const std::array<std::uint64_t, 3> frames{7, 3, 5};
    for (const bool includeArtifactHash : {false, true}) {
        SCOPED_TRACE(includeArtifactHash);
        PackCanonicalImage image;
        std::uint64_t artifactHash = 0;
        const auto result = includeArtifactHash
            ? ParsePackCanonicalImage(bytes.data(), bytes.size(), &image, &artifactHash)
            : ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
        ASSERT_EQ(VN_OK, result);
        if (includeArtifactHash) EXPECT_EQ(expectedHash, artifactHash);
        ASSERT_EQ(frames.size(), image.media.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            EXPECT_EQ(harkentest::ObjectId(101 + i), image.media[i].soundId);
            EXPECT_EQ(frames[i], image.media[i].frameCount);
            EXPECT_EQ(frames[i] * sizeof(float), image.media[i].blobBytes);
        }
    }
}

TEST(PackCanonicalFormat, MediaDirectoryOrderCanReversePhysicalBlobOrder) {
    auto bytes = BuildCanonicalMediaFixture({7, 3, 5});
    PackFileHeader header;
    ASSERT_TRUE(ReadPackFileHeader(bytes.data(), &header));
    ASSERT_EQ(4u, header.entryCount);
    auto* firstRecord = bytes.data() + header.directoryOffset + kPackDirectoryEntryBytes;
    auto* lastRecord = bytes.data() + header.directoryOffset + 3 * kPackDirectoryEntryBytes;
    const auto first = ReadPackDirectoryEntry(firstRecord);
    const auto last = ReadPackDirectoryEntry(lastRecord);
    ASSERT_EQ(kPackEntryKindMedia, first.kind);
    ASSERT_EQ(kPackEntryKindMedia, last.kind);
    SerializePackDirectoryEntry(last, firstRecord);
    SerializePackDirectoryEntry(first, lastRecord);
    RecomputeChecksums(&bytes, false);
    const auto expectedHash = Fnv1a64(bytes.data(), bytes.size());
    const std::array<std::uint64_t, 3> frames{5, 3, 7};
    for (const bool includeArtifactHash : {false, true}) {
        SCOPED_TRACE(includeArtifactHash);
        PackCanonicalImage image;
        std::uint64_t artifactHash = 0;
        const auto result = includeArtifactHash
            ? ParsePackCanonicalImage(bytes.data(), bytes.size(), &image, &artifactHash)
            : ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
        ASSERT_EQ(VN_OK, result);
        if (includeArtifactHash) EXPECT_EQ(expectedHash, artifactHash);
        ASSERT_EQ(frames.size(), image.media.size());
        EXPECT_GT(image.media[0].blobOffset, image.media[1].blobOffset);
        EXPECT_GT(image.media[1].blobOffset, image.media[2].blobOffset);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            EXPECT_EQ(harkentest::ObjectId(103 - i), image.media[i].soundId);
            EXPECT_EQ(frames[i], image.media[i].frameCount);
            EXPECT_EQ(frames[i] * sizeof(float), image.media[i].blobBytes);
        }
    }
}

TEST(PackCanonicalFormat, AdjacentMediaCorruptionKeepsOutputUnchanged) {
    const auto original = BuildCanonicalMediaFixture({7, 3, 5});
    PackCanonicalImage valid;
    ASSERT_EQ(VN_OK,
              ParsePackCanonicalImage(original.data(), original.size(), &valid));
    ASSERT_EQ(3u, valid.media.size());
    for (std::size_t i = 0; i < valid.media.size(); ++i) {
        SCOPED_TRACE(i);
        auto bytes = original;
        const auto& media = valid.media[i];
        bytes[static_cast<std::size_t>(media.blobOffset + media.blobBytes - 1)] ^= 1u;
        ExpectCanonicalMediaRefusal(bytes, VN_BAD_PACK);
    }
}

TEST(PackCanonicalFormat, AdjacentMediaRejectNextPayloadAndBlobBounds) {
    const auto original = BuildCanonicalMediaFixture({3, 7});
    PackCanonicalImage valid;
    ASSERT_EQ(VN_OK,
              ParsePackCanonicalImage(original.data(), original.size(), &valid));
    for (unsigned invalid = 0; invalid < 9; ++invalid) {
        SCOPED_TRACE(invalid);
        auto bytes = original;
        PackFileHeader header;
        ASSERT_TRUE(ReadPackFileHeader(bytes.data(), &header));
        auto* record = bytes.data() + header.directoryOffset +
                       2 * kPackDirectoryEntryBytes;
        auto next = ReadPackDirectoryEntry(record);
        switch (invalid) {
        case 0:
            --next.size;
            next.payloadChecksum = Fnv1a64(bytes.data() + next.offset, next.size);
            break;
        case 1:
            next.offset = header.directoryOffset;
            break;
        case 2:
            next.offset = UINT64_MAX - (kPackPayloadAlign - 1);
            break;
        case 3:
            next.offset = bytes.size() - bytes.size() % kPackPayloadAlign -
                          kPackPayloadAlign;
            break;
        case 4:
            ++next.offset;
            break;
        default: {
            auto media = ReadPackMedia(bytes.data() + next.offset);
            if (invalid == 5) media.blobOffset = 0;
            if (invalid == 6)
                media.blobOffset = AlignUp(bytes.size(), kPackBlobAlign);
            if (invalid == 7) {
                ++media.frameCount;
                media.blobBytes += sizeof(float);
            }
            if (invalid == 8) ++media.blobOffset;
            SerializePackMedia(media, bytes.data() + next.offset);
            next.payloadChecksum = Fnv1a64(bytes.data() + next.offset, next.size);
            break;
        }
        }
        SerializePackDirectoryEntry(next, record);
        // Deliberately invalid spans must not be followed while restamping.
        RecomputeChecksums(&bytes, false);
        ExpectCanonicalMediaRefusal(bytes, VN_BAD_PACK);
    }
}

TEST(PackCanonicalFormat, AdjacentMediaKeepOriginalValidationOrder) {
    const auto original = BuildCanonicalMediaFixture({3, 7});
    PackCanonicalImage valid;
    ASSERT_EQ(VN_OK,
              ParsePackCanonicalImage(original.data(), original.size(), &valid));
    ASSERT_EQ(2u, valid.media.size());
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        SCOPED_TRACE(invalid);
        auto bytes = original;
        PackFileHeader header;
        ASSERT_TRUE(ReadPackFileHeader(bytes.data(), &header));
        auto* record = bytes.data() + header.directoryOffset +
                       2 * kPackDirectoryEntryBytes;
        auto next = ReadPackDirectoryEntry(record);
        if (invalid == 0) {
            next.payloadChecksum ^= 1u;
        } else {
            next.vintage = PackEntryVintage(next.kind) + 1;
            if (invalid == 2)
                bytes[static_cast<std::size_t>(valid.media[0].blobOffset)] ^= 1u;
        }
        SerializePackDirectoryEntry(next, record);
        RecomputeChecksums(&bytes, false);
        ExpectCanonicalMediaRefusal(
            bytes, invalid == 1 ? VN_PACK_INCOMPATIBLE : VN_BAD_PACK);
    }
}

TEST(PackCanonicalFormat, StrictInitDependencyAcceptsExactRequiredPairing) {
    const PackDependencyEdge init{11, kPackDependencyEdgeKindInit, 1, 7, 0};
    EXPECT_EQ(VN_OK, ValidateCanonicalInitDependency(std::span(&init, 1), 11));
    const std::array edges{init, PackDependencyEdge{22, kPackDependencyEdgeKindContent, 1, 0, 0}};
    EXPECT_EQ(VN_OK, ValidateCanonicalInitDependency(edges, 11));
}

TEST(PackCanonicalFormat, StrictInitDependencyRejectsAmbiguousOrUnpinnedPairing) {
    const PackDependencyEdge init{11, kPackDependencyEdgeKindInit, 1, 7, 0};
    EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency(std::span(&init, 1), 0));
    EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency({}, 11));
    const PackDependencyEdge content{22, kPackDependencyEdgeKindContent, 1, 0, 0};
    EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency(std::span(&content, 1), 11));
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        auto edge = init;
        if (invalid == 0) edge.prerequisitePackStableId = 12;
        if (invalid == 1) edge.required01 = 0;
        if (invalid == 2) edge.required01 = 2;
        if (invalid == 3) edge.expectedPrerequisiteContentRevision = 0;
        if (invalid == 4) edge.reserved0 = 1;
        EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency(std::span(&edge, 1), 11));
    }
    std::array duplicates{init, init};
    EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency(duplicates, 11));
    duplicates[1].required01 = 0;
    EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency(duplicates, 11));
    std::swap(duplicates[0], duplicates[1]);
    EXPECT_EQ(VN_BAD_PACK, ValidateCanonicalInitDependency(duplicates, 11));
}

TEST(PackCanonicalFormat, RuntimeRefusesInitKindWithoutActivatingMedia) {
    const std::vector<std::uint8_t> bytes = BuildMinimalInit();
    PackLogicImage image;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(0u, image.EntryCount());
    EXPECT_TRUE(image.media.empty());
}

TEST(PackCanonicalFormat, RuntimeRefusesCanonicalEntryOnContentPack) {
    ImageBuilder builder(kPackKindContent);
    std::vector<std::uint8_t> requirement = RequirementPayload(22, 1, 0);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, 22,
                     std::move(requirement));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, IllegalLogicVintageIsIncompatible) {
    ImageBuilder builder(kPackKindInit);
    std::vector<std::uint8_t> bus(kPackBusBytes);
    PackBusEntry busEntry;
    busEntry.busId = harkentest::ObjectId(10);
    busEntry.parentBusId = harkentest::ObjectId(1);
    SerializePackBus(busEntry, bus.data());
    builder.AddEntry(kPackEntryKindBus, 10, std::move(bus), 3);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, IllegalCanonicalVintageIsIncompatible) {
    ImageBuilder builder(kPackKindInit);
    std::vector<std::uint8_t> requirement = RequirementPayload(11, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, 11,
                     std::move(requirement), 1);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, RetiredProgramBodyVintageIsIncompatible) {
    for (const auto vintage : {kCurrentWorldBodyVintageV6,
                                kCurrentWorldBodyVintageV7,
                                kCurrentWorldBodyVintageV8,
                                kCurrentWorldBodyVintageV9}) {
        ImageBuilder builder(kPackKindInit);
        std::vector<std::uint8_t> requirement = RequirementPayload(11, 1, 1);
        StampRequirementCoverage(&requirement);
        builder.AddEntry(kPackEntryKindPackRequirementBlock, 11,
                         std::move(requirement));
        builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11,
                         PartitionPayload(11, 1, vintage));
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackCanonicalImage image;
        EXPECT_EQ(VN_PACK_INCOMPATIBLE,
                  ParsePackCanonicalImage(bytes.data(), bytes.size(), &image))
            << vintage;
    }
}

TEST(PackCanonicalFormat, UnknownProgramBodyVintageIsIncompatible) {
    ImageBuilder builder(kPackKindInit);
    std::vector<std::uint8_t> requirement = RequirementPayload(11, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, 11,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11,
                     PartitionPayload(11, 1,
                                      kCurrentWorldBodyVintageV20 + 1));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, CurrentProgramBodyDamageIsBadPack) {
    ImageBuilder builder(kPackKindInit);
    std::vector<std::uint8_t> requirement = RequirementPayload(11, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, 11,
                     std::move(requirement));
    auto partition = EmptyPartitionPayload(11, 1);
    ASSERT_GT(partition.size(),
              kPackCanonicalPartitionHeaderBytes +
                  kPackBodyWorldPrefixBytes + 4);
    StoreLE32(partition.data() + kPackCanonicalPartitionHeaderBytes +
                  kPackBodyWorldPrefixBytes,
              kHardActionDefinitionCapacity + 1u);
    StoreLE64(partition.data() + 24,
              Fnv1a64(partition.data() + kPackCanonicalPartitionHeaderBytes,
                      partition.size() - kPackCanonicalPartitionHeaderBytes));
    StoreLE64(partition.data() + 80, Fnv1a64(partition.data(), 80));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, 11,
                     std::move(partition));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat,
     SemanticFingerprintMismatchIsBadPackWithoutMutatingOutput) {
    constexpr std::uint64_t kPackStableId = 22;
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kPackStableId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kPackStableId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition,
                     kPackStableId,
                     EmptyPartitionPayload(kPackStableId, 1));
    std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage valid;
    ASSERT_EQ(VN_OK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &valid));

    RewriteCanonicalPartitionWorld(
        &bytes, [](ContainerWorldInstallViewV1* world) {
            world->identity.runtimeSemanticFingerprint ^= 1u;
        });

    PackCanonicalImage image;
    image.packStableId = 77;
    image.hasPartition = true;
    image.partitionBody = {1, 2, 3};
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(77u, image.packStableId);
    EXPECT_TRUE(image.hasPartition);
    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3}), image.partitionBody);
}

TEST(PackCanonicalFormat,
     AdvancedTransitionCapabilityMismatchIsPackIncompatible) {
    constexpr std::uint64_t kPackStableId = 22;
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kPackStableId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kPackStableId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition,
                     kPackStableId,
                     AdvancedTransitionPartitionPayload(kPackStableId, 1));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat,
     WorldCapacityOverflowIsPackResidentCapacity) {
    constexpr std::uint64_t kPackStableId = 22;
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kPackStableId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kPackStableId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition,
                     kPackStableId,
                     EmptyPartitionPayload(kPackStableId, 1));
    std::vector<std::uint8_t> bytes = builder.Build();

    RewriteCanonicalPartitionWorld(
        &bytes, [](ContainerWorldInstallViewV1* world) {
            world->capacity.maxContainerDepth = 65;
            world->identity.runtimeSemanticFingerprint =
                ComputeCurrentWorldFingerprintV4(*world);
            EXPECT_EQ(WorldInstallStatus::Capacity,
                      ValidateCurrentWorldV1(*world));
        });

    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat,
     PartitionLocalAcceptsWellFormedAbsentExternalTarget) {
    constexpr std::uint64_t kPackStableId = 22;
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kPackStableId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kPackStableId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition,
                     kPackStableId,
                     ActionTargetPartitionPayload(kPackStableId, 1, false,
                                                  false));
    const std::vector<std::uint8_t> bytes = builder.Build();

    PackCanonicalImage image;
    EXPECT_EQ(VN_OK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    EXPECT_TRUE(image.hasPartition);
    EXPECT_EQ(1u, image.partitionHeader.ownedDefinitionCount);
}

TEST(PackCanonicalFormat,
     PartitionLocalRejectsWrongFingerprintForPresentTarget) {
    constexpr std::uint64_t kPackStableId = 22;
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kPackStableId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kPackStableId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition,
                     kPackStableId,
                     ActionTargetPartitionPayload(kPackStableId, 1, true,
                                                  true));
    const std::vector<std::uint8_t> bytes = builder.Build();

    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, ContainerKindIsIncompatible) {
    ImageBuilder builder(kPackKindCanonicalContent);
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(200);
    header.childCount = 1;
    PackContainerChild child;
    child.playableId = harkentest::ObjectId(100);
    child.weight = 1;
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(
        PackContainerPayloadBytes(1)));
    SerializePackContainerHeader(header, payload.data());
    SerializePackContainerChild(child,
                                  payload.data() + kPackContainerHeaderBytes);
    builder.AddEntry(kPackEntryKindContainer, 200, std::move(payload),
                     kPackContainerVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, VoiceRecipeKindIsIncompatible) {
    ImageBuilder builder(kPackKindCanonicalContent);
    PackVoiceRecipeEntryHeader header;
    header.playableId = harkentest::ObjectId(100);
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(
        PackVoiceRecipePayloadBytes(0, 0)), 0);
    SerializePackVoiceRecipeHeader(header, payload.data());
    builder.AddEntry(kPackEntryKindVoiceRecipe, 100, std::move(payload),
                     kPackVoiceRecipeVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, AttenuationPayloadMatchesOuterVersionExactly) {
    VnAttenuationDesc desc{};
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1;
    desc.maxDistance = 100;
    desc.airAbsorptionNearHz = desc.airAbsorptionFarHz = 20000;
    desc.proximityFrequency = 400;
    for (const auto version : {3u, kPackFormatVersion, kPackFormatVersion + 1}) {
        for (const auto size : {kPackAttenuationBytes - 1,
                               kPackAttenuationBytes,
                               kPackAttenuationBytes + 1,
                               kPackAttenuationBytes + kPackAttenuationCurveBytes}) {
            ImageBuilder builder(kPackKindInit);
            auto requirement = RequirementPayload(11, 1, 0);
            StampRequirementCoverage(&requirement);
            builder.AddEntry(kPackEntryKindPackRequirementBlock, 11,
                             std::move(requirement), kPackPackRequirementBlockVintage);
            std::vector<std::uint8_t> payload(kPackAttenuationBytes);
            SerializePackAttenuation(desc, payload.data());
            payload.resize(size);
            builder.AddEntry(kPackEntryKindAttenuation, 500, std::move(payload),
                             kPackAttenuationVintage);
            auto bytes = builder.Build();
            StoreLE32(bytes.data() + 4, version);
            PackCanonicalImage image;
            const bool valid = version == kPackFormatVersion &&
                               size == kPackAttenuationBytes;
            ASSERT_EQ(valid ? VN_OK : VN_BAD_PACK,
                      ParsePackCanonicalImage(bytes.data(), bytes.size(), &image))
                << version << "/" << size;
            if (valid) {
                ASSERT_EQ(1u, image.attenuations.size());
                const auto& parsed = image.attenuations[0].desc;
                EXPECT_EQ(612u, parsed.structSize);
                EXPECT_FLOAT_EQ(400, parsed.proximityFrequency);
                EXPECT_EQ(0u, parsed.highpassCurve.enabled);
                const auto fingerprint = CanonicalAttenuationDefinitionRevisionV1(image.attenuations[0]);
                EXPECT_EQ(UINT64_C(0xf602a15155735ed9), fingerprint);
            }
        }
    }
}

TEST(PackCanonicalFormat, RetiredAttenuationVintageIsIncompatible) {
    ImageBuilder builder(kPackKindCanonicalContent);
    VnAttenuationDesc desc{};
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = 100.0f;
    desc.airAbsorptionNearHz = 20000.0f;
    desc.airAbsorptionFarHz = 20000.0f;
    std::vector<std::uint8_t> payload(kPackAttenuationBytes);
    SerializePackAttenuation(desc, payload.data());
    builder.AddEntry(kPackEntryKindAttenuation, 500, std::move(payload),
                     2u);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, RetiredInitRoutingCatalogIsRejected) {
    const auto bytes = BuildInitWithTypedEntry(kPackEntryKindInitRoutingCatalog, 0, {0});
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, KindTenAndLegacyVariableKindsStayBadPack) {
    for (std::uint32_t packKind : {kPackKindResident, kPackKindContent}) {
        ImageBuilder action(packKind);
        action.AddEntry(kPackEntryKindAction, 300,
                        std::vector<std::uint8_t>(8, 0xA5), 2u);
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(action.Build().data(), action.Build().size(),
                                   &image));
        ImageBuilder variable(packKind);
        std::vector<std::uint8_t> payload{0};
        variable.AddEntry(kPackEntryKindDiscreteVariable, 920, std::move(payload),
                          2u);
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(variable.Build().data(),
                                   variable.Build().size(), &image));
    }
}

TEST(PackCanonicalFormat, ExistingResidentAndContentGoldensStillParse) {
    ImageBuilder resident(kPackKindResident);
    std::vector<std::uint8_t> bus(kPackBusBytes);
    PackBusEntry busEntry;
    busEntry.busId = harkentest::ObjectId(10);
    busEntry.parentBusId = harkentest::ObjectId(1);
    SerializePackBus(busEntry, bus.data());
    resident.AddEntry(kPackEntryKindBus, 10, std::move(bus), kPackBusVintage);
    PackLogicImage image;
    const auto residentBytes = resident.Build();
    ASSERT_EQ(VN_OK, ParsePackV2Image(residentBytes.data(), residentBytes.size(),
                                      &image));
    EXPECT_EQ(kPackKindResident, image.packKind);
    EXPECT_EQ(1u, image.buses.size());
    EXPECT_EQ(harkentest::ObjectId(10u), image.buses[0].busId);
    EXPECT_EQ(harkentest::ObjectId(1u), image.buses[0].parentBusId);
    EXPECT_EQ(0.0f, image.buses[0].volume);
    EXPECT_EQ(0.0f, image.buses[0].outputBusVolume);
    EXPECT_EQ(0.0f, image.buses[0].makeUpGain);

    ImageBuilder content(kPackKindContent);
    const auto empty = content.Build();
    PackLogicImage emptyImage;
    ASSERT_EQ(VN_OK,
              ParsePackV2Image(empty.data(), empty.size(), &emptyImage));
    EXPECT_EQ(kPackKindContent, emptyImage.packKind);
}

std::vector<std::uint8_t> EffectPresetPayload(
    PackEffectPresetHeader header, std::vector<PackEffectPresetParameter> parameters) {
    std::vector<std::uint8_t> payload(kPackEffectPresetHeaderBytes +
        parameters.size() * kPackEffectPresetParameterBytes);
    PackWireBuffer<std::uint8_t> output{payload.data(), payload.size()};
    EXPECT_TRUE(AppendPackEffectPresetHeader(output, header));
    for (const auto& parameter : parameters)
        EXPECT_TRUE(AppendPackEffectPresetParameter(output, parameter));
    return payload;
}

TEST(PackCanonicalFormat, SharedEffectDefinitionsEnforceIdentityAndDescriptorContract) {
    const PackEffectPresetHeader header{harkentest::ObjectId(8801), VN_EFFECT_LIMITER,
        kPackEffectDescriptorVintage, 2};
    const std::vector<PackEffectPresetParameter> parameters{
        {0, std::bit_cast<std::uint32_t>(-12.0f)},
        {1, std::bit_cast<std::uint32_t>(1.0f)}};
    const auto payload = EffectPresetPayload(header, parameters);
    const auto bytes = BuildInitWithTypedEntry(kPackEntryKindEffectPreset, 8801, payload);
    PackCanonicalImage image;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(1u, image.effectPresets.size());
    EXPECT_EQ(header.presetId, image.effectPresets.front().header.presetId);
    ASSERT_EQ(2u, image.effectPresets.front().parameters.size());
    EXPECT_EQ(parameters[0].valueBits, image.effectPresets.front().parameters[0].valueBits);
    std::vector<std::vector<std::uint8_t>> malformed;
    auto changed = header;
    changed.presetId = {};
    malformed.push_back(EffectPresetPayload(changed, parameters));
    changed = header;
    changed.presetId = harkentest::ObjectId(8802);
    malformed.push_back(EffectPresetPayload(changed, parameters));
    changed = header;
    changed.effectType = static_cast<std::uint32_t>(kPackEffectPresetReferenceType);
    malformed.push_back(EffectPresetPayload(changed, parameters));
    changed = header;
    ++changed.descriptorVintage;
    malformed.push_back(EffectPresetPayload(changed, parameters));
    changed = header;
    --changed.paramCount;
    malformed.push_back(EffectPresetPayload(changed, parameters));
    auto changedParameters = parameters;
    changedParameters[1].paramId = 0;
    malformed.push_back(EffectPresetPayload(header, changedParameters));
    changedParameters = parameters;
    changedParameters[0].valueBits = std::bit_cast<std::uint32_t>(-25.0f);
    malformed.push_back(EffectPresetPayload(header, changedParameters));
    malformed.push_back(payload);
    malformed.back().pop_back();
    for (const auto& invalid : malformed) {
        const auto invalidBytes = BuildInitWithTypedEntry(kPackEntryKindEffectPreset, 8801, invalid);
        PackCanonicalImage unchanged;
        unchanged.packStableId = 0xDEAD;
        EXPECT_EQ(VN_BAD_PACK, ParsePackCanonicalImage(invalidBytes.data(), invalidBytes.size(), &unchanged));
        EXPECT_EQ(0xDEADu, unchanged.packStableId);
        EXPECT_TRUE(unchanged.effectPresets.empty());
    }
}

TEST(PackCanonicalFormat, SharedEffectDefinitionsRequireUniqueInitOwnership) {
    const auto presetId = harkentest::ObjectId(8801);
    const auto payload = EffectPresetPayload({presetId, VN_EFFECT_GAIN,
        kPackEffectDescriptorVintage, 1}, {{0, std::bit_cast<std::uint32_t>(-6.0f)}});
    for (const auto packKind : {kPackKindInit, kPackKindCanonicalContent}) {
        ImageBuilder builder(packKind);
        auto requirement = RequirementPayload(11, 1, 0);
        StampRequirementCoverage(&requirement);
        builder.AddEntry(kPackEntryKindPackRequirementBlock, 11, std::move(requirement));
        builder.AddEntry(kPackEntryKindEffectPreset, presetId, payload);
        if (packKind == kPackKindInit)
            builder.AddEntry(kPackEntryKindEffectPreset, presetId, payload);
        const auto bytes = builder.Build();
        PackCanonicalImage image;
        EXPECT_EQ(VN_BAD_PACK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    }
}

TEST(PackCanonicalFormat, Kind26Vintage1IsCanonicalEntry) {
    const std::vector<std::uint8_t> bytes = BuildInitWithTypedEntry(
        kPackEntryKindVariablePolicyOverlay, 0, Kind26Payload(TwoOverlayRows()));
    PackCanonicalImage image;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    EXPECT_TRUE(image.hasVariablePolicyOverlay);
    ASSERT_EQ(2u, image.variablePolicyOverlays.size());
    EXPECT_EQ(harkentest::ObjectId(7u), image.variablePolicyOverlays[0].variableId);
    EXPECT_EQ(1u, image.variablePolicyOverlays[0].writers);
    EXPECT_EQ(0x1111111111111111ull,
              image.variablePolicyOverlays[0].executionFingerprint);
    ASSERT_EQ(2u, image.variablePolicyOverlays[0].discreteAnchors.size());
    EXPECT_EQ(0, image.variablePolicyOverlays[0].discreteAnchors[0]);
    EXPECT_EQ(8, image.variablePolicyOverlays[0].discreteAnchors[1]);
    EXPECT_EQ(harkentest::ObjectId(11u), image.variablePolicyOverlays[1].variableId);
    EXPECT_EQ(3u, image.variablePolicyOverlays[1].writers);
    EXPECT_TRUE(image.variablePolicyOverlays[1].discreteAnchors.empty());
}

TEST(PackCanonicalFormat, Kind26Vintage2Incompatible) {
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindVariablePolicyOverlay, 0,
                     Kind26Payload(TwoOverlayRows()), 2);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, Kind26LabelMappingsStayValidated) {
    const auto variableId = harkentest::ObjectId(7);
    const VariableLabelDefinitionV1 label{8, "Step", harkentest::ObjectId(9)};
    const auto mapping = ComputeVariableLabelMappingFingerprintV1(variableId, std::span(&label, 1));
    const auto payload = Kind26Payload({{variableId, 1, 1, {8}, {label}}});
    const auto bytes = BuildInitWithTypedEntry(kPackEntryKindVariablePolicyOverlay, 0, payload);
    PackCanonicalImage image;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(1u, image.variablePolicyOverlays.size());
    const auto& overlay = image.variablePolicyOverlays.front();
    EXPECT_EQ(variableId, overlay.variableId);
    EXPECT_EQ(mapping, overlay.labelMappingFingerprint);
    ASSERT_EQ(1u, overlay.labels.size());
    EXPECT_EQ(label.labelId, overlay.labels.front().labelId);
    EXPECT_EQ(label.anchor, overlay.labels.front().anchor);
    EXPECT_TRUE(overlay.labels.front().name.empty());

    std::vector<std::vector<std::uint8_t>> malformed(2, payload);
    malformed[0].pop_back();
    const auto labelHeaderOffset = kPackVariablePolicyHeaderBytes + kPackVariablePolicyRowBytes +
        kPackVariableAnchorCountBytes + kPackVariableAnchorBytes;
    malformed[1][labelHeaderOffset + kPackVariableLabelHeaderLabelMappingFingerprintOffset] ^= 1;
    for (const auto& invalid : malformed) {
        const auto invalidBytes = BuildInitWithTypedEntry(kPackEntryKindVariablePolicyOverlay, 0, invalid);
        EXPECT_EQ(VN_BAD_PACK, ParsePackCanonicalImage(invalidBytes.data(), invalidBytes.size(), &image));
    }
}

TEST(PackCanonicalFormat, Kind26OnContentRefused) {
    ImageBuilder builder(kPackKindCanonicalContent);
    std::vector<std::uint8_t> requirement = RequirementPayload(22, 1, 0);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, 22,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindVariablePolicyOverlay, 0,
                     Kind26Payload(TwoOverlayRows()));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, Kind26DuplicateRefused) {
    const std::vector<std::uint8_t> bytes = BuildInitWithTypedEntry(
        kPackEntryKindVariablePolicyOverlay, 0,
        Kind26Payload({{harkentest::ObjectId(7), 1, 1, {}}, {harkentest::ObjectId(7), 1, 2, {}}}));
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, Kind27OnInitRefused) {
    const std::vector<std::uint8_t> bytes = BuildInitWithTypedEntry(
        kPackEntryKindVariableRequirements, 0,
        Kind27Payload(TwoRequirementRows()));
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, Kind27Vintage1IsCanonicalEntry) {
    const std::vector<std::uint8_t> bytes =
        BuildCanonicalContentWithKind27(Kind27Payload(TwoRequirementRows()));
    PackCanonicalImage image;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    EXPECT_TRUE(image.hasVariableRequirements);
    ASSERT_EQ(2u, image.variableRequirements.size());
    EXPECT_EQ(1u, image.variableRequirements[0].identityClass);
    EXPECT_EQ(harkentest::ObjectId(7u), image.variableRequirements[0].stableId);
    EXPECT_EQ(0x1111111111111111ull,
              image.variableRequirements[0].expectedExecutionFingerprint);
    EXPECT_EQ(1u, image.variableRequirements[1].identityClass);
    EXPECT_EQ(harkentest::ObjectId(11u), image.variableRequirements[1].stableId);
    EXPECT_EQ(0x2222222222222222ull,
              image.variableRequirements[1].expectedExecutionFingerprint);
}

TEST(PackCanonicalFormat, Kind27SortedUnique) {
    PackCanonicalImage image;
    const std::vector<std::uint8_t> unsorted = BuildCanonicalContentWithKind27(
        Kind27Payload({{1, harkentest::ObjectId(11), 1}, {1, harkentest::ObjectId(7), 2}}));
    EXPECT_EQ(VN_BAD_PACK, ParsePackCanonicalImage(unsorted.data(),
                                                   unsorted.size(), &image));
    const std::vector<std::uint8_t> duplicate = BuildCanonicalContentWithKind27(
        Kind27Payload({{1, harkentest::ObjectId(7), 1}, {1, harkentest::ObjectId(7), 2}}));
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(duplicate.data(), duplicate.size(),
                                      &image));
}

TEST(PackCanonicalFormat, Kind27IdentityClass2RefusedInU1) {
    const std::vector<std::uint8_t> bytes = BuildCanonicalContentWithKind27(
        Kind27Payload({{2, harkentest::ObjectId(7), 1}}));
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

VnBindingDesc MakeCanonicalVoiceBindingDesc(std::uint32_t scope) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = harkentest::ObjectId(8402);
    desc.targetScopeKind = scope;
    desc.targetId = VN_TARGET_VOICE_VOLUME;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, -6.0f};
    desc.curve.points[1] = {1.0f, 0.0f};
    return desc;
}

PackBindingAttachmentEntry MakeCanonicalVoiceAttachment(std::uint32_t scope) {
    PackBindingAttachmentEntry attachment{};
    attachment.bindingId = harkentest::ObjectId(8401);
    attachment.scopeKind = scope;
    attachment.objectId = harkentest::ObjectId(1234);
    return attachment;
}

std::vector<std::uint8_t> WithCanonicalScopeRows(
    const std::vector<std::uint8_t>& original, std::uint32_t scope, unsigned rows) {
    PackFileHeader header;
    if (!ReadPackFileHeader(original.data(), &header)) {
        ADD_FAILURE() << "Invalid source fixture";
        return {};
    }
    ImageBuilder builder(header.packKind);
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = ReadPackDirectoryEntry(
            original.data() + header.directoryOffset + i * kPackDirectoryEntryBytes);
        const auto copied = builder.AddEntry(
            entry.kind, entry.id,
            std::vector<std::uint8_t>(original.begin() + entry.offset,
                                      original.begin() + entry.offset +
                                          entry.size),
            entry.vintage);
        builder.SetRelationOrdinal(copied, entry.relationOrdinal);
    }
    if ((rows & 1u) != 0) {
        const VnBindingDesc desc = MakeCanonicalVoiceBindingDesc(scope);
        std::vector<std::uint8_t> payload(kPackBindingDefinitionBytes);
        WritePackBindingDefinition(harkentest::ObjectId(8401), desc, payload.data());
        builder.AddEntry(kPackEntryKindBindingDefinition, harkentest::ObjectId(8401),
                         std::move(payload), kPackBindingDefinitionVintage);
    }
    if ((rows & 2u) != 0) {
        const PackBindingAttachmentEntry attachment =
            MakeCanonicalVoiceAttachment(scope);
        std::vector<std::uint8_t> payload(kPackBindingAttachmentBytes);
        SerializePackBindingAttachment(attachment, payload.data());
        builder.AddEntry(kPackEntryKindBindingAttachment, 0, std::move(payload),
                         kPackBindingAttachmentVintage);
    }
    return builder.Build();
}

TEST(PackCanonicalFormat, BindingTransitionRangeIsRejectedAtBinaryAdmission) {
    for (std::uint32_t ms : {60000u, 60001u, UINT32_MAX}) {
        VnBindingDesc desc{};
        desc.structSize = sizeof(desc);
        desc.variableId = harkentest::ObjectId(8402);
        desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
        desc.targetId = VN_TARGET_VOICE_VOLUME;
        desc.transitionMs = ms;
        desc.curve.structSize = sizeof(desc.curve);
        desc.curve.pointCount = 1;
        desc.curve.points[0] = {0.0f, 0.0f};
        std::vector<std::uint8_t> payload(kPackBindingDefinitionBytes);
        WritePackBindingDefinition(harkentest::ObjectId(8401), desc, payload.data());
        std::uint64_t contentBodyBytes = 0;
        const auto original =
            harkentest::packfoundation::BuildContent(1, 0, 0, &contentBodyBytes);
        ImageBuilder builder(kPackKindCanonicalContent);
        PackFileHeader header;
        ASSERT_TRUE(ReadPackFileHeader(original.data(), &header));
        for (std::uint32_t i = 0; i < header.entryCount; ++i) {
            const auto entry = ReadPackDirectoryEntry(
                original.data() + header.directoryOffset + i * kPackDirectoryEntryBytes);
            const auto copied = builder.AddEntry(
                entry.kind, entry.id,
                std::vector<std::uint8_t>(original.begin() + entry.offset,
                                          original.begin() + entry.offset +
                                              entry.size),
                entry.vintage);
            builder.SetRelationOrdinal(copied, entry.relationOrdinal);
        }
        builder.AddEntry(kPackEntryKindBindingDefinition, harkentest::ObjectId(8401),
                         payload, kPackBindingDefinitionVintage);
        const auto packed = builder.Build();
        PackCanonicalImage image;
        EXPECT_EQ(ms == 60000 ? VN_OK : VN_BAD_PACK,
                  ParsePackCanonicalImage(packed.data(), packed.size(), &image));
    }
}

std::vector<std::uint8_t> CanonicalLayoutFixture(std::uint32_t packKind,
                                                std::uint32_t requirementVintage,
                                                std::uint32_t partitionVintage,
                                                std::uint32_t dependencyVintage) {
    constexpr std::uint64_t kPackId = 22;
    ImageBuilder builder(packKind);
    auto requirement = RequirementPayload(kPackId, 1, 1, requirementVintage);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kPackId,
                     std::move(requirement), requirementVintage);
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kPackId,
                     PartitionPayload(kPackId, 1, kCanonicalDefinitionPartitionBodyVintage), partitionVintage);
    if (packKind == kPackKindCanonicalContent) {
        PackDependencyEdge edge;
        edge.prerequisitePackStableId = 11;
        edge.edgeKind = kPackDependencyEdgeKindInit;
        edge.required01 = 1;
        edge.expectedPrerequisiteContentRevision = 1;
        std::vector<std::uint8_t> payload(kPackDependencyEdgeBytes, 0);
        const auto triple = harkentest::packfoundation::DefaultFixtureBusTriple();
        edge.expectedBusContentGeneration = triple.busContentGeneration;
        edge.expectedPublishedBusFingerprint = triple.publishedBusFingerprint;
        edge.expectedCompileTargetProfileFingerprint = triple.compileTargetProfileFingerprint;
        SerializePackDependencyEdge(edge, payload.data());
        builder.AddEntry(kPackEntryKindPackDependencyEdge, 0, std::move(payload), dependencyVintage);
    }
    return builder.Build();
}



TEST(PackCanonicalFormat, RetiredOrMixedCanonicalLayoutsAreIncompatible) {
    for (std::uint32_t requirement : {1u, 2u}) {
        for (std::uint32_t partition : {1u, 2u}) {
            for (std::uint32_t dependency : {1u, 2u}) {
                SCOPED_TRACE(::testing::Message() << requirement << ", " << partition << ", " << dependency);
                const auto bytes = CanonicalLayoutFixture(kPackKindCanonicalContent,
                    requirement, partition, dependency);
                PackCanonicalImage image;
                image.packStableId = 77;
                const auto result = ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
                if (requirement == 2 && partition == 2 && dependency == 2) {
                    EXPECT_EQ(result, VN_OK);
                } else {
                    EXPECT_EQ(result, VN_PACK_INCOMPATIBLE);
                    EXPECT_EQ(image.packStableId, 77u);
                }
            }
        }
    }
}

TEST(PackCanonicalFormat, ParserAcceptsCanonicalBindingScopes) {
    const auto bus = harkentest::packfoundation::DefaultRuntimeBusFixture();
    harkentest::packfoundation::ImageBuilder initBuilder(kPackKindInit);
    std::uint64_t initBodyBytes = 0;
    std::vector<std::uint8_t> initPartition =
        harkentest::packfoundation::PartitionPayload(
            harkentest::packfoundation::kInitPackStableId, 1, 0, 0, &initBodyBytes,
            64, {}, bus.triple);
    initBuilder.AddEntry(kPackEntryKindPackRequirementBlock,
                         harkentest::packfoundation::kInitPackStableId,
                         harkentest::packfoundation::RequirementPayload(
                             harkentest::packfoundation::kInitPackStableId, 1, 1, 0,
                             initBodyBytes, bus.triple));
    initBuilder.AddEntry(kPackEntryKindCanonicalDefinitionPartition,
                         harkentest::packfoundation::kInitPackStableId,
                         std::move(initPartition));
    initBuilder.AddEntry(kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
    const std::vector<std::uint8_t> init = initBuilder.Build();
    std::uint64_t contentBodyBytes = 0;
    const std::vector<std::uint8_t> contentBytes =
        harkentest::packfoundation::BuildContent(1, 0, 0, &contentBodyBytes);
    ASSERT_FALSE(init.empty());
    ASSERT_FALSE(contentBytes.empty());
    for (const auto* original : {&init, &contentBytes}) {
        for (unsigned rows = 1; rows <= 3; ++rows) {
            for (std::uint32_t scope :
                 {std::uint32_t(VN_TARGET_SCOPE_VOICE),
                  std::uint32_t(VN_TARGET_SCOPE_VOICE_EFFECT_PARAM),
                  std::uint32_t(VN_TARGET_SCOPE_BUS),
                  std::uint32_t(VN_TARGET_SCOPE_BUS_EFFECT_PARAM), UINT32_MAX}) {
                SCOPED_TRACE(::testing::Message()
                             << "init=" << (original == &init) << " rows=" << rows
                             << " scope=" << scope);
                const auto bytes = WithCanonicalScopeRows(*original, scope, rows);
                PackCanonicalImage image;
                image.packStableId = 0xDEAD;
                const auto result =
                    ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
                const bool initPack = original == &init;
                const bool definitionOk =
                    scope <= VN_TARGET_SCOPE_BUS_EFFECT_PARAM &&
                    (!initPack ||
                     (scope != VN_TARGET_SCOPE_VOICE &&
                      scope != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM));
                const bool attachmentOk =
                    scope != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM &&
                    scope <= VN_TARGET_SCOPE_BUS_EFFECT_PARAM &&
                    (!initPack || scope == VN_TARGET_SCOPE_BUS ||
                     scope == VN_TARGET_SCOPE_BUS_EFFECT_PARAM);
                const bool accepted =
                    ((rows & 1u) == 0 || definitionOk) &&
                    ((rows & 2u) == 0 || attachmentOk);
                if (accepted) {
                    ASSERT_EQ(VN_OK, result);
                    EXPECT_EQ((rows & 1u) ? 1u : 0u, image.bindingDefinitions.size());
                    EXPECT_EQ((rows & 2u) ? 1u : 0u, image.bindingAttachments.size());
                } else {
                    EXPECT_EQ(VN_BAD_PACK, result);
                    EXPECT_EQ(0xDEADu, image.packStableId);
                    EXPECT_TRUE(image.bindingDefinitions.empty());
                    EXPECT_TRUE(image.bindingAttachments.empty());
                }
            }
        }
    }
}

} // namespace
