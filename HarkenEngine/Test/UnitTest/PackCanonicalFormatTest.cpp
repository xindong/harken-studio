#include "Common/ObjectIdTestSupport.h"
#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
            vintage = PackV2CanonicalDirectoryVintage(kind);
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
        const std::uint64_t dirOffset = kPackV2HeaderBytes;
        const std::uint64_t dirBytes =
            static_cast<std::uint64_t>(entries_.size()) *
            kPackV2DirectoryEntryBytes;
        const std::uint64_t dirEnd = dirOffset + dirBytes;
        payloadOffsets_.assign(entries_.size(), 0);
        std::uint64_t cursor = AlignUp(dirEnd, kPackV2PayloadAlign);
        std::uint64_t contentEnd = dirEnd;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            payloadOffsets_[i] = cursor;
            contentEnd = cursor + entries_[i].payload.size();
            cursor = AlignUp(contentEnd, kPackV2PayloadAlign);
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(contentEnd), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            std::memcpy(bytes.data() +
                            static_cast<std::size_t>(payloadOffsets_[i]),
                        entries_[i].payload.data(), entries_[i].payload.size());
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            PackV2DirectoryEntry record;
            record.kind = entries_[i].kind;
            record.vintage = entries_[i].vintage;
            record.id = entries_[i].id;
            record.relationOrdinal = entries_[i].relationOrdinal;
            record.offset = payloadOffsets_[i];
            record.size = entries_[i].payload.size();
            record.payloadChecksum = Fnv1a64(
                bytes.data() + static_cast<std::size_t>(payloadOffsets_[i]),
                static_cast<std::size_t>(record.size));
            SerializePackV2DirectoryEntry(
                record, bytes.data() + static_cast<std::size_t>(dirOffset) +
                            i * kPackV2DirectoryEntryBytes);
        }
        PackV2Header header;
        header.formatVersion = kPackFormatVersion2;
        header.headerBytes = kPackV2HeaderBytes;
        header.fileBytes = contentEnd;
        header.packKind = packKind_;
        header.entryCount = static_cast<std::uint32_t>(entries_.size());
        header.directoryOffset = dirOffset;
        header.directoryChecksum =
            Fnv1a64(bytes.data() + static_cast<std::size_t>(dirOffset),
                    static_cast<std::size_t>(dirBytes));
        SerializePackV2Header(header, bytes.data());
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

void RecomputeChecksums(std::vector<std::uint8_t>* bytes) {
    PackV2Header header;
    ASSERT_TRUE(ParsePackV2Header(bytes->data(), &header));
    const std::size_t dir = static_cast<std::size_t>(header.directoryOffset);
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        std::uint8_t* record =
            bytes->data() + dir + i * kPackV2DirectoryEntryBytes;
        PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(record);
        entry.payloadChecksum =
            Fnv1a64(bytes->data() + static_cast<std::size_t>(entry.offset),
                    static_cast<std::size_t>(entry.size));
        SerializePackV2DirectoryEntry(entry, record);
    }
    header.directoryChecksum = Fnv1a64(
        bytes->data() + dir,
        static_cast<std::size_t>(header.entryCount) * kPackV2DirectoryEntryBytes);
    SerializePackV2Header(header, bytes->data());
}

template <typename Mutator>
void RewriteCanonicalPartitionWorld(std::vector<std::uint8_t>* bytes,
                                    Mutator mutate) {
    ASSERT_NE(bytes, nullptr);
    PackV2Header packHeader;
    ASSERT_TRUE(ParsePackV2Header(bytes->data(), &packHeader));
    bool found = false;
    const std::size_t directoryOffset =
        static_cast<std::size_t>(packHeader.directoryOffset);
    for (std::uint32_t i = 0; i < packHeader.entryCount; ++i) {
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            bytes->data() + directoryOffset +
            i * kPackV2DirectoryEntryBytes);
        if (entry.kind != kPackEntryKindCanonicalDefinitionPartition) {
            continue;
        }
        ASSERT_FALSE(found);
        std::uint8_t* payload =
            bytes->data() + static_cast<std::size_t>(entry.offset);
        PackCanonicalPartitionHeader partitionHeader;
        ASSERT_TRUE(ParsePackCanonicalPartitionHeaderV2(
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
        SerializePackCanonicalPartitionHeaderV2(partitionHeader, payload);
        partitionHeader.headerChecksum = Fnv1a64(payload, 80);
        SerializePackCanonicalPartitionHeaderV2(partitionHeader, payload);
        found = true;
    }
    ASSERT_TRUE(found);
    RecomputeChecksums(bytes);
}

void FillLocator(PackInitCatalogRow* row, const char* locator) {
    const std::size_t n = std::strlen(locator);
    row->locatorByteCount = static_cast<std::uint32_t>(n + 1);
    std::memcpy(row->relativeLocator.data(), locator, n + 1);
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
                                             std::uint32_t vintage = kPackCanonicalRequirementVintage) {
    PackRequirementBlock block;
    block.schemaVersion = vintage;
    block.storageModelVersion = vintage;
    block.packStableId = packStableId;
    block.packContentRevision = contentRevision;
    block.definitionPartitionRevision = partitionRevision;
    block.resourceClosureRevision = 1;
    ApplyNeutralMasterTriple(&block);
    const bool versionTwo = vintage == kPackCanonicalRequirementVintage;
    std::vector<std::uint8_t> payload(
        versionTwo ? kPackRequirementBlockV2Bytes : kPackRequirementBlockBytes, 0);
    if (versionTwo) SerializePackRequirementBlockV2(block, payload.data());
    else SerializePackRequirementBlock(block, payload.data());
    return payload;
}

std::vector<std::uint8_t> CatalogPayload(const PackInitCatalogHeader& header,
                                         const std::vector<PackInitCatalogRow>& rows,
                                         const std::vector<std::uint64_t>& startup) {
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(header.catalogBytes), 0);
    SerializePackInitCatalogHeader(header, payload.data());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        SerializePackInitCatalogRow(
            rows[i], payload.data() + kInitCatalogHeaderBytes +
                         i * kInitCatalogRowBytes);
    }
    for (std::size_t i = 0; i < startup.size(); ++i) {
        StoreLE64(payload.data() + kInitCatalogHeaderBytes +
                      rows.size() * kInitCatalogRowBytes + i * 8,
                  startup[i]);
    }
    PackInitCatalogHeader stamped = header;
    stamped.checksum =
        Fnv1a64(payload.data() + kInitCatalogHeaderBytes,
                static_cast<std::size_t>(header.catalogBytes -
                                         kInitCatalogHeaderBytes));
    SerializePackInitCatalogHeader(stamped, payload.data());
    return payload;
}

std::vector<std::uint8_t> PartitionPayload(std::uint64_t packStableId,
                                           std::uint64_t revision,
                                           std::uint32_t bodyVintage,
                                           std::uint32_t vintage = kPackCanonicalRequirementVintage) {
    ContainerWorldInstallViewV1 world{};
    world.identity.runtimeSemanticFingerprint =
        ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    if (bodyVintage == kCurrentWorldBodyVintageV6) {
        EXPECT_TRUE(EncodeCurrentWorldBodyV6(
            harkentest::AsHistoricalPrefix(world), body));
    } else if (bodyVintage == kCurrentWorldBodyVintageV7) {
        EXPECT_TRUE(EncodeCurrentWorldBodyV7(
            harkentest::AsHistoricalPrefix(world), body));
    } else if (bodyVintage == kCurrentWorldBodyVintageV8) {
        EXPECT_TRUE(EncodeCurrentWorldBodyV8(
            harkentest::AsHistoricalPrefix(world), body));
    } else {
        EXPECT_TRUE(EncodeCurrentWorldShippingBody(world, body));
    }
    PackCanonicalPartitionHeader header;
    const bool versionTwo = vintage == kPackCanonicalRequirementVintage;
    header.headerBytes = versionTwo ? kCanonicalDefinitionPartitionHeaderV2Bytes
                                   : kCanonicalDefinitionPartitionHeaderBytes;
    header.bodyVintage = bodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = 0;
    ApplyNeutralMasterTriple(&header);
    std::vector<std::uint8_t> payload(header.headerBytes + body.size(), 0);
    const auto serialize = versionTwo ? SerializePackCanonicalPartitionHeaderV2
                                      : SerializePackCanonicalPartitionHeader;
    serialize(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), versionTwo ? 80 : 56);
    serialize(header, payload.data());
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
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = includeLocalTarget ? 2 : 1;
    ApplyNeutralMasterTriple(&header);
    std::vector<std::uint8_t> payload(
        kCanonicalDefinitionPartitionHeaderV2Bytes + body.size(), 0);
    SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    std::memcpy(payload.data() + kCanonicalDefinitionPartitionHeaderV2Bytes,
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
    rule.sourceExit = TransitionSourceExitV1::NextBeat;
    rule.requiredClockFingerprint = 0x1234;
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
    header.headerBytes = kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount = 2;
    ApplyNeutralMasterTriple(&header);
    std::vector<std::uint8_t> payload(
        kCanonicalDefinitionPartitionHeaderV2Bytes + body.size(), 0);
    SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    header.headerChecksum = Fnv1a64(payload.data(), 80);
    SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    std::memcpy(payload.data() + kCanonicalDefinitionPartitionHeaderV2Bytes,
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
    constexpr std::uint64_t kContentId = 22;
    PackInitCatalogHeader header;
    header.schemaVersion = 1;
    header.storageModelVersion = 1;
    header.projectBuildId = 7;
    header.platformId = 1;
    header.languageId = 2;
    header.totalDefinitionCount = 2;
    header.totalResourceCount = 0;
    header.catalogEntryCount = 2;
    header.startupPackCount = 1;
    header.recommendedCatalogCapacity = 64;
    header.recommendedCatalogBytes = 4096;
    EXPECT_TRUE(InitCatalogBytes(header.catalogEntryCount, header.startupPackCount,
                                 &header.catalogBytes));

    PackInitCatalogRow initOwned;
    initOwned.stableId = harkentest::ObjectId(1001);
    initOwned.identityClass = kInitCatalogIdentityClassAction;
    initOwned.ownerPackStableId = kInitId;
    FillLocator(&initOwned, "Init.pack");
    PackInitCatalogRow contentOwned;
    contentOwned.stableId = harkentest::ObjectId(2002);
    contentOwned.identityClass = kInitCatalogIdentityClassAction;
    contentOwned.ownerPackStableId = kContentId;
    contentOwned.expectedContentHash = 0xABCDEF;
    FillLocator(&contentOwned, "Action.pack");

    ImageBuilder builder(kPackKindInit);
    const std::size_t catalogIndex = builder.AddEntry(
        kPackEntryKindInitRoutingCatalog, 0,
        CatalogPayload(header, {initOwned, contentOwned}, {kContentId}));
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kInitId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     EmptyPartitionPayload(kInitId, 1));
    harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&builder);
    std::vector<std::uint8_t> bytes = builder.Build();

    const std::uint64_t catalogOffset = builder.PayloadOffset(catalogIndex);
    const std::uint64_t initHashField =
        catalogOffset + kInitCatalogHeaderBytes + kInitCatalogContentHashOffset;
    EXPECT_EQ(0u, LoadLE64(bytes.data() + static_cast<std::size_t>(initHashField)));
    const std::uint64_t hashed = Fnv1a64(bytes.data(), bytes.size());
    StoreLE64(bytes.data() + static_cast<std::size_t>(initHashField), hashed);
    return bytes;
}

struct Kind26RowSpec {
    VnObjectId variableId = harkentest::ObjectId(0);
    std::uint8_t writers = 1;
    std::uint64_t executionFingerprint = 1;
    std::vector<std::int32_t> discreteAnchors;
};

std::vector<std::uint8_t> Kind26Payload(std::vector<Kind26RowSpec> rows) {
    std::size_t bytes = 16 + rows.size() * 32;
    for (const auto& row : rows) {
        bytes += 4 + row.discreteAnchors.size() * 4;
    }
    std::vector<std::uint8_t> payload(bytes, 0);
    StoreLE32(payload.data(), static_cast<std::uint32_t>(rows.size()));
    std::size_t rowOffset = 16;
    for (const auto& row : rows) {
        StoreObjectId(payload.data() + rowOffset, row.variableId);
        payload[rowOffset + 16] = row.writers;
        StoreLE64(payload.data() + rowOffset + 24, row.executionFingerprint);
        rowOffset += 32;
    }
    std::size_t cursor = 16 + rows.size() * 32;
    for (const auto& row : rows) {
        StoreLE32(payload.data() + cursor,
                  static_cast<std::uint32_t>(row.discreteAnchors.size()));
        cursor += 4;
        for (std::int32_t anchor : row.discreteAnchors) {
            StoreLE32(payload.data() + cursor,
                      static_cast<std::uint32_t>(anchor));
            cursor += 4;
        }
    }
    return payload;
}

std::vector<std::uint8_t> BuildInitWithTypedEntry(
    std::uint32_t kind, std::uint64_t id, std::vector<std::uint8_t> payload,
    std::uint32_t vintage = kPackCanonicalEntryVintage) {
    constexpr std::uint64_t kInitId = 11;
    constexpr std::uint64_t kContentId = 22;
    PackInitCatalogHeader header;
    header.schemaVersion = 1;
    header.storageModelVersion = 1;
    header.projectBuildId = 7;
    header.platformId = 1;
    header.languageId = 2;
    header.totalDefinitionCount = 2;
    header.totalResourceCount = 0;
    header.catalogEntryCount = 2;
    header.startupPackCount = 1;
    header.recommendedCatalogCapacity = 64;
    header.recommendedCatalogBytes = 4096;
    EXPECT_TRUE(InitCatalogBytes(header.catalogEntryCount, header.startupPackCount,
                                 &header.catalogBytes));

    PackInitCatalogRow initOwned;
    initOwned.stableId = harkentest::ObjectId(1001);
    initOwned.identityClass = kInitCatalogIdentityClassAction;
    initOwned.ownerPackStableId = kInitId;
    FillLocator(&initOwned, "Init.pack");
    PackInitCatalogRow contentOwned;
    contentOwned.stableId = harkentest::ObjectId(2002);
    contentOwned.identityClass = kInitCatalogIdentityClassAction;
    contentOwned.ownerPackStableId = kContentId;
    contentOwned.expectedContentHash = 0xABCDEF;
    FillLocator(&contentOwned, "Action.pack");

    ImageBuilder builder(kPackKindInit);
    const std::size_t catalogIndex = builder.AddEntry(
        kPackEntryKindInitRoutingCatalog, 0,
        CatalogPayload(header, {initOwned, contentOwned}, {kContentId}));
    std::vector<std::uint8_t> requirement =
        RequirementPayload(kInitId, 1, 1);
    StampRequirementCoverage(&requirement);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     std::move(requirement));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     EmptyPartitionPayload(kInitId, 1));
    if (kind != kPackEntryKindBusDefinitionSnapshot) {
        harkentest::packfoundation::AppendNeutralMasterSnapshotV1(&builder);
    }
    builder.AddEntry(kind, id, std::move(payload), vintage);
    std::vector<std::uint8_t> bytes = builder.Build();

    const std::uint64_t catalogOffset = builder.PayloadOffset(catalogIndex);
    const std::uint64_t initHashField =
        catalogOffset + kInitCatalogHeaderBytes + kInitCatalogContentHashOffset;
    const std::uint64_t hashed = Fnv1a64(bytes.data(), bytes.size());
    StoreLE64(bytes.data() + static_cast<std::size_t>(initHashField), hashed);
    return bytes;
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
    std::vector<std::uint8_t> payload(16 + rows.size() * 32, 0);
    StoreLE32(payload.data(), static_cast<std::uint32_t>(rows.size()));
    std::size_t offset = 16;
    for (const auto& row : rows) {
        payload[offset] = row.identityClass;
        StoreObjectId(payload.data() + offset + 8, row.stableId);
        StoreLE64(payload.data() + offset + 24,
                  row.expectedExecutionFingerprint);
        offset += 32;
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
    std::vector<std::uint8_t> bus(kPackV2BusPayloadBytes);
    PackBusEntry busEntry;
    busEntry.busId = harkentest::ObjectId(10);
    busEntry.parentBusId = harkentest::ObjectId(1);
    SerializePackV2Bus(busEntry, bus.data());
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
                                kCurrentWorldBodyVintageV8}) {
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
                                      kCurrentWorldShippingBodyVintage + 1));
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
              kCanonicalDefinitionPartitionHeaderV2Bytes +
                  kCurrentWorldV3PrefixWireBytes + 4);
    StoreLE32(partition.data() + kCanonicalDefinitionPartitionHeaderV2Bytes +
                  kCurrentWorldV3PrefixWireBytes,
              kHardActionDefinitionCapacity + 1u);
    StoreLE64(partition.data() + 24,
              Fnv1a64(partition.data() + kCanonicalDefinitionPartitionHeaderV2Bytes,
                      partition.size() - kCanonicalDefinitionPartitionHeaderV2Bytes));
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
    PackContainerChildV2 child;
    child.playableId = harkentest::ObjectId(100);
    child.weight = 1;
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(
        PackV2ContainerPayloadBytes(1)));
    SerializePackV2ContainerHeader(header, payload.data());
    SerializePackV2ContainerChild(child,
                                  payload.data() + kPackV2ContainerHeaderBytes);
    builder.AddEntry(kPackEntryKindContainer, 200, std::move(payload),
                     kPackV2SchemaVintage);
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
        PackV5VoiceRecipePayloadBytes(0, 0)), 0);
    SerializePackV2VoiceRecipeHeader(header, payload.data());
    builder.AddEntry(kPackEntryKindVoiceRecipe, 100, std::move(payload),
                     kPackV2PlaybackRecipeSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, RetiredAttenuationVintageIsIncompatible) {
    ImageBuilder builder(kPackKindCanonicalContent);
    VnAttenuationDesc desc{};
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = 100.0f;
    desc.airAbsorptionNearHz = 20000.0f;
    desc.airAbsorptionFarHz = 20000.0f;
    std::vector<std::uint8_t> payload(kPackV2AttenuationPayloadBytes);
    SerializePackV2Attenuation(desc, payload.data());
    builder.AddEntry(kPackEntryKindAttenuation, 500, std::move(payload),
                     kPackV2SchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackCanonicalImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(PackCanonicalFormat, ConstructionAInitParsesAndFilledHashChecksumsAreRejected) {
    std::vector<std::uint8_t> stamped = BuildMinimalInit();
    PackCanonicalImage image;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(stamped.data(), stamped.size(), &image));
    EXPECT_TRUE(image.hasCatalog);
    const PackInitCatalogRow* row = nullptr;
    ASSERT_EQ(PackCatalogIdentityState::KnownUnloaded,
              LookupInitCatalogIdentity(image, harkentest::ObjectId(1001), &row));
    ASSERT_NE(row, nullptr);
    EXPECT_NE(0u, row->expectedContentHash);

    PackV2Header header;
    ASSERT_TRUE(ParsePackV2Header(stamped.data(), &header));
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            stamped.data() + static_cast<std::size_t>(header.directoryOffset) +
            i * kPackV2DirectoryEntryBytes);
        if (entry.kind != kPackEntryKindInitRoutingCatalog)
            continue;
        PackInitCatalogHeader catalogHeader;
        ASSERT_TRUE(ParsePackInitCatalogHeader(stamped.data() + entry.offset,
                                               entry.size, &catalogHeader));
        catalogHeader.checksum =
            Fnv1a64(stamped.data() + entry.offset + kInitCatalogHeaderBytes,
                    static_cast<std::size_t>(catalogHeader.catalogBytes -
                                             kInitCatalogHeaderBytes));
        SerializePackInitCatalogHeader(catalogHeader,
                                       stamped.data() + entry.offset);
    }
    RecomputeChecksums(&stamped);
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(stamped.data(), stamped.size(), &image));
}

TEST(PackCanonicalFormat, ToolingDecodeDistinguishesKnownUnloadedAndAbsent) {
    const std::vector<std::uint8_t> bytes = BuildMinimalInit();
    PackCanonicalImage image;
    const VnResult parsed =
        ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
    if (parsed != VN_OK) {
        PackV2Header header{};
        ParsePackV2Header(bytes.data(), &header);
        ADD_FAILURE() << "canonical parse=" << parsed
                      << " packKind=" << header.packKind
                      << " entries=" << header.entryCount
                      << " fileBytes=" << header.fileBytes
                      << " size=" << bytes.size();
    }
    ASSERT_EQ(VN_OK, parsed);
    EXPECT_EQ(kPackKindInit, image.packKind);
    EXPECT_EQ(11u, image.packStableId);
    const PackInitCatalogRow* row = nullptr;
    EXPECT_EQ(PackCatalogIdentityState::KnownUnloaded,
              LookupInitCatalogIdentity(image, harkentest::ObjectId(1001), &row));
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(11u, row->ownerPackStableId);
    EXPECT_EQ(PackCatalogIdentityState::KnownUnloaded,
              LookupInitCatalogIdentity(image, harkentest::ObjectId(2002), &row));
    EXPECT_EQ(22u, row->ownerPackStableId);
    EXPECT_EQ(PackCatalogIdentityState::Absent,
              LookupInitCatalogIdentity(image, harkentest::ObjectId(9999), &row));
    EXPECT_EQ(nullptr, row);
}

TEST(PackCanonicalFormat, KindTenAndLegacyVariableKindsStayBadPack) {
    for (std::uint32_t packKind : {kPackKindResident, kPackKindContent}) {
        ImageBuilder action(packKind);
        action.AddEntry(kPackEntryKindAction, 300,
                        std::vector<std::uint8_t>(8, 0xA5), kPackV2SchemaVintage);
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(action.Build().data(), action.Build().size(),
                                   &image));
        ImageBuilder variable(packKind);
        std::vector<std::uint8_t> payload(kPackV2DiscreteVariablePayloadBytes);
        SerializePackV2DiscreteVariable(harkentest::ObjectId(920), payload.data());
        variable.AddEntry(kPackEntryKindDiscreteVariable, 920, std::move(payload),
                          kPackV2SchemaVintage);
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(variable.Build().data(),
                                   variable.Build().size(), &image));
    }
}

TEST(PackCanonicalFormat, ExistingResidentAndContentGoldensStillParse) {
    ImageBuilder resident(kPackKindResident);
    std::vector<std::uint8_t> bus(kPackV2BusPayloadBytes);
    PackBusEntry busEntry;
    busEntry.busId = harkentest::ObjectId(10);
    busEntry.parentBusId = harkentest::ObjectId(1);
    SerializePackV2Bus(busEntry, bus.data());
    resident.AddEntry(kPackEntryKindBus, 10, std::move(bus), kPackV2SchemaVintage);
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
    PackV2Header header;
    if (!ParsePackV2Header(original.data(), &header)) {
        ADD_FAILURE() << "Invalid source fixture";
        return {};
    }
    ImageBuilder builder(header.packKind);
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = ParsePackV2DirectoryEntry(
            original.data() + header.directoryOffset + i * kPackV2DirectoryEntryBytes);
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
        std::vector<std::uint8_t> payload(kPackV2BindingDefinitionPayloadBytes);
        SerializePackV2BindingDefinition(harkentest::ObjectId(8401), desc, payload.data());
        builder.AddEntry(kPackEntryKindBindingDefinition, harkentest::ObjectId(8401),
                         std::move(payload), kPackV2SchemaVintage);
    }
    if ((rows & 2u) != 0) {
        const PackBindingAttachmentEntry attachment =
            MakeCanonicalVoiceAttachment(scope);
        std::vector<std::uint8_t> payload(kPackV2BindingAttachmentPayloadBytes);
        SerializePackV2BindingAttachment(attachment, payload.data());
        builder.AddEntry(kPackEntryKindBindingAttachment, 0, std::move(payload),
                         kPackV2SchemaVintage);
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
        std::vector<std::uint8_t> payload(kPackV2BindingDefinitionPayloadBytes);
        SerializePackV2BindingDefinition(harkentest::ObjectId(8401), desc, payload.data());
        std::uint64_t contentBodyBytes = 0;
        const auto original =
            harkentest::packfoundation::BuildContent(1, 0, 0, &contentBodyBytes);
        ImageBuilder builder(kPackKindCanonicalContent);
        PackV2Header header;
        ASSERT_TRUE(ParsePackV2Header(original.data(), &header));
        for (std::uint32_t i = 0; i < header.entryCount; ++i) {
            const auto entry = ParsePackV2DirectoryEntry(
                original.data() + header.directoryOffset + i * kPackV2DirectoryEntryBytes);
            const auto copied = builder.AddEntry(
                entry.kind, entry.id,
                std::vector<std::uint8_t>(original.begin() + entry.offset,
                                          original.begin() + entry.offset +
                                              entry.size),
                entry.vintage);
            builder.SetRelationOrdinal(copied, entry.relationOrdinal);
        }
        builder.AddEntry(kPackEntryKindBindingDefinition, harkentest::ObjectId(8401),
                         payload, kPackV2SchemaVintage);
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
                     PartitionPayload(kPackId, 1, kCanonicalDefinitionPartitionBodyVintage,
                                      partitionVintage), partitionVintage);
    if (packKind == kPackKindCanonicalContent) {
        PackDependencyEdge edge;
        edge.prerequisitePackStableId = 11;
        edge.edgeKind = kPackDependencyEdgeKindInit;
        edge.required01 = 1;
        edge.expectedPrerequisiteContentRevision = 1;
        const bool versionTwo = dependencyVintage == kPackCanonicalRequirementVintage;
        std::vector<std::uint8_t> payload(versionTwo ? kPackDependencyEdgeV2Bytes
                                                   : kPackDependencyEdgeBytes, 0);
        if (versionTwo) {
            const auto triple = harkentest::packfoundation::DefaultFixtureBusTriple();
            edge.expectedBusContentGeneration = triple.busContentGeneration;
            edge.expectedPublishedBusFingerprint = triple.publishedBusFingerprint;
            edge.expectedCompileTargetProfileFingerprint = triple.compileTargetProfileFingerprint;
            SerializePackDependencyEdgeV2(edge, payload.data());
        } else {
            SerializePackDependencyEdge(edge, payload.data());
        }
        builder.AddEntry(kPackEntryKindPackDependencyEdge, 0, std::move(payload), dependencyVintage);
    }
    return builder.Build();
}

TEST(PackCanonicalFormat, ProductionV1LayoutsKeepBusBindingScopes) {
    for (auto kind : {kPackKindInit, kPackKindCanonicalContent}) {
        const auto original = CanonicalLayoutFixture(kind, 1, 1, 1);
        for (std::uint32_t scope : {VN_TARGET_SCOPE_VOICE, VN_TARGET_SCOPE_BUS,
                                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM}) {
            const auto bytes = WithCanonicalScopeRows(original, scope, 3);
            PackCanonicalImage image;
            ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
            EXPECT_EQ(image.requirement.storageModelVersion, 1u);
            EXPECT_FALSE(image.hasBusDefinitionSnapshot);
            ASSERT_EQ(image.bindingDefinitions.size(), 1u);
            ASSERT_EQ(image.bindingAttachments.size(), 1u);
            EXPECT_EQ(image.bindingDefinitions[0].desc.targetScopeKind, scope);
            EXPECT_EQ(image.bindingAttachments[0].scopeKind, scope);
        }
    }
}

TEST(PackCanonicalFormat, MixedCanonicalLayoutsAreIncompatible) {
    for (std::uint32_t requirement : {1u, 2u}) {
        for (std::uint32_t partition : {1u, 2u}) {
            for (std::uint32_t dependency : {1u, 2u}) {
                SCOPED_TRACE(::testing::Message() << requirement << ", " << partition << ", " << dependency);
                const auto bytes = CanonicalLayoutFixture(kPackKindCanonicalContent,
                    requirement, partition, dependency);
                PackCanonicalImage image;
                image.packStableId = 77;
                const auto result = ParsePackCanonicalImage(bytes.data(), bytes.size(), &image);
                if (requirement == partition && partition == dependency) {
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
    const auto bus = harkentest::packfoundation::DefaultRuntimeBusFixtureV1();
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
