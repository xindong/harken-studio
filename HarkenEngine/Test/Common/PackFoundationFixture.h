#pragma once

#include "Common/ObjectIdTestSupport.h"
#include "harken/abi/VoiceSlotLayout.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/CanonicalBusSnapshot.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "harken/param/VariableService.h"
#include <harken/BusDefinitionFingerprintValue.h>
#include <harken/Harken.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace harkentest::packfoundation {

inline constexpr std::uint64_t kInitPackStableId = 11;
inline constexpr std::uint64_t kResidentPackStableId = 22;
inline constexpr std::uint64_t kUnloadedPackStableId = 33;
inline constexpr std::uint64_t kActionBase = 0xAC710100ull;
inline constexpr VnObjectId kVariableId = harkentest::ObjectId(7);

struct Fixture {
    std::uint32_t catalogCount = 0;
    std::uint32_t residentCount = 0;
    std::uint64_t catalogBytes = 0;
    std::uint64_t initBodyBytes = 0;
    std::uint64_t residentBodyBytes = 0;
    std::vector<std::uint8_t> init;
    std::vector<std::uint8_t> resident;
    std::vector<std::uint8_t> replacement;
};

inline std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) {
    return (value + align - 1) / align * align;
}

class ImageBuilder {
public:
    explicit ImageBuilder(std::uint32_t packKind) : packKind_(packKind) {}

    std::size_t AddEntry(std::uint32_t kind, std::uint64_t id,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = 0) {
        entries_.push_back({kind, {}, id, std::move(payload), {}, vintage});
        return entries_.size() - 1;
    }

    std::size_t AddEntry(std::uint32_t kind, VnObjectId id,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = harken::kPackCanonicalEntryVintage) {
        entries_.push_back({kind, id, 0, std::move(payload), {}, vintage});
        return entries_.size() - 1;
    }

    void SetRelationOrdinal(std::size_t index, std::uint64_t ordinal) {
        entries_[index].relationOrdinal = ordinal;
    }

    void AddMedia(VnObjectId id, std::vector<float> samples) {
        entries_.push_back({harken::kPackEntryKindMedia, id, 0,
                           std::vector<std::uint8_t>(harken::kPackV2MediaPayloadBytes),
                           std::move(samples)});
    }

    std::vector<std::uint8_t> Build() {
        const std::uint64_t directoryOffset = harken::kPackV2HeaderBytes;
        const std::uint64_t directoryBytes =
            entries_.size() * harken::kPackV2DirectoryEntryBytes;
        const std::uint64_t directoryEnd =
            directoryOffset + directoryBytes;
        std::vector<std::uint64_t> offsets(entries_.size(), 0);
        std::uint64_t cursor =
            AlignUp(directoryEnd, harken::kPackV2PayloadAlign);
        std::uint64_t contentEnd = directoryEnd;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            offsets[i] = cursor;
            contentEnd = cursor + entries_[i].payload.size();
            cursor = AlignUp(contentEnd, harken::kPackV2PayloadAlign);
        }

        std::vector<std::uint64_t> mediaOffsets(entries_.size(), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].samples.empty()) {
                continue;
            }
            mediaOffsets[i] = AlignUp(contentEnd, harken::kPackBlobAlign);
            contentEnd = mediaOffsets[i] + entries_[i].samples.size() * sizeof(float);
        }

        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(contentEnd), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!entries_[i].samples.empty()) {
                harken::PackTocEntry media;
                media.soundId = entries_[i].id;
                media.sampleRate = 48000;
                media.channelCount = 1;
                media.sampleFormat = harken::kPackSampleFormatF32Planar;
                media.frameCount = entries_[i].samples.size();
                media.blobOffset = mediaOffsets[i];
                media.blobBytes = entries_[i].samples.size() * sizeof(float);
                std::memcpy(bytes.data() + media.blobOffset,
                            entries_[i].samples.data(), media.blobBytes);
                media.blobChecksum = harken::Fnv1a64(bytes.data() + media.blobOffset,
                                                   media.blobBytes);
                harken::SerializePackTocEntry(media, entries_[i].payload.data());
            }
            std::memcpy(bytes.data() + offsets[i],
                        entries_[i].payload.data(),
                        entries_[i].payload.size());
            harken::PackV2DirectoryEntry record;
            record.kind = entries_[i].kind;
            if (entries_[i].vintage != 0) {
                record.vintage = entries_[i].vintage;
            } else if (!entries_[i].samples.empty()) {
                record.vintage = harken::kPackV2SchemaVintage;
            } else {
                record.vintage =
                    harken::PackV2CanonicalDirectoryVintage(entries_[i].kind);
            }
            record.id = entries_[i].id;
            record.relationOrdinal = entries_[i].relationOrdinal;
            record.offset = offsets[i];
            record.size = entries_[i].payload.size();
            record.payloadChecksum = harken::Fnv1a64(
                bytes.data() + offsets[i], entries_[i].payload.size());
            harken::SerializePackV2DirectoryEntry(
                record, bytes.data() + directoryOffset +
                            i * harken::kPackV2DirectoryEntryBytes);
        }

        harken::PackV2Header header;
        header.formatVersion = harken::kPackFormatVersion2;
        header.headerBytes = harken::kPackV2HeaderBytes;
        header.fileBytes = contentEnd;
        header.packKind = packKind_;
        header.entryCount = static_cast<std::uint32_t>(entries_.size());
        header.directoryOffset = directoryOffset;
        header.directoryChecksum = harken::Fnv1a64(
            bytes.data() + directoryOffset,
            static_cast<std::size_t>(directoryBytes));
        harken::SerializePackV2Header(header, bytes.data());
        return bytes;
    }

private:
    struct Entry {
        std::uint32_t kind = 0;
        VnObjectId id{};
        std::uint64_t relationOrdinal = 0;
        std::vector<std::uint8_t> payload;
        std::vector<float> samples;
        std::uint32_t vintage = 0;
    };

    std::uint32_t packKind_ = 0;
    std::vector<Entry> entries_;
};

using harken::ApplyCanonicalBusTriple;
using harken::StampRequirementCoverage;
using harken::StampSnapshotChecksum;

inline harken::CanonicalBusTriple DefaultFixtureBusTriple() {
    return harken::NeutralMasterSnapshotTriple();
}

inline std::vector<std::uint8_t> NeutralMasterSnapshotPayloadV1(
    harken::CanonicalBusTriple triple = DefaultFixtureBusTriple()) {
    return harken::NeutralMasterSnapshotPayload(triple);
}

template <typename Builder>
inline void AppendNeutralMasterSnapshotV1(
    Builder* builder,
    harken::CanonicalBusTriple triple = DefaultFixtureBusTriple()) {
    builder->AddEntry(harken::kPackEntryKindBusDefinitionSnapshot, 1,
                      NeutralMasterSnapshotPayloadV1(triple));
}

struct RuntimeBusFixtureV1 {
    harken::CanonicalBusTriple triple{};
    std::vector<std::uint8_t> snapshot;
};

inline RuntimeBusFixtureV1 DefaultRuntimeBusFixtureV1() {
    RuntimeBusFixtureV1 fixture;
    fixture.triple = DefaultFixtureBusTriple();
    fixture.snapshot = NeutralMasterSnapshotPayloadV1(fixture.triple);
    return fixture;
}

inline RuntimeBusFixtureV1 MasterSendRuntimeBusFixtureV1(
    float levelDb, std::uint32_t sendRepeat = 1,
    VnBusId targetBusId = VN_BUS_MASTER) {
    const auto profile = DefaultFixtureBusTriple();
    constexpr std::uint64_t kHeader = harken::kPackBusDefinitionSnapshotHeaderBytes;
    constexpr std::uint64_t kBusOffset = kHeader;
    const std::uint64_t kSendOffset =
        kBusOffset + 2u * harken::kPackBusDefinitionBusRowBytes;
    const std::uint64_t kDownmixOffset =
        kSendOffset + static_cast<std::uint64_t>(sendRepeat) *
                          harken::kPackBusDefinitionSendRowBytes;
    const std::uint64_t kCoefficientOffset =
        kDownmixOffset + 2u * harken::kPackBusDefinitionDownmixRowBytes;
    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 1;
    header.storageModelVersion = 1;
    header.headerBytes = harken::kPackBusDefinitionSnapshotHeaderBytes;
    header.publicationCapabilityVintage = 1;
    header.contentGeneration = profile.busContentGeneration;
    header.compileTargetProfileFingerprint =
        profile.compileTargetProfileFingerprint;
    header.busCount = 2;
    header.sendCount = sendRepeat;
    header.downmixCount = 2;
    header.coefficientCount = 128;
    header.busOffset = kBusOffset;
    header.sendOffset = kSendOffset;
    header.effectOffset = kDownmixOffset;
    header.effectParameterOffset = kDownmixOffset;
    header.downmixOffset = kDownmixOffset;
    header.coefficientOffset = kCoefficientOffset;
    header.payloadBytes =
        (kCoefficientOffset - kHeader) +
        (128u * harken::kPackBusDefinitionCoefficientRowBytes);
    header.profileVintage = 1;
    header.renderSampleRateHz = 48000;
    header.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.runtimeCapabilityMask = harken::busfingerprint::kRequiredBusCapabilityMaskV1;
    harken::PackBusDefinitionBusV1 master{};
    master.busId = harken::CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    master.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    master.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    master.downmixCount = 1;
    master.downmixFirst = 0;
    image.busDefinitionBuses.push_back(master);
    harken::PackBusDefinitionBusV1 bus2{};
    bus2.busId = harken::CanonicalSnapshotPackedBusIdV1(VN_OBJECT_ID(0, 2));
    bus2.parentId = harken::CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    bus2.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    bus2.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    bus2.downmixCount = 1;
    bus2.downmixFirst = 1;
    image.busDefinitionBuses.push_back(bus2);
    harken::PackBusDefinitionSendV1 send{};
    send.source = harken::CanonicalSnapshotPackedBusIdV1(VN_OBJECT_ID(0, 2));
    send.target = harken::CanonicalSnapshotPackedBusIdV1(targetBusId);
    send.levelDb = levelDb;
    for (std::uint32_t i = 0; i < sendRepeat; ++i) {
        image.busDefinitionSends.push_back(send);
    }
    harken::PackBusDefinitionDownmixV1 downmixMaster{};
    downmixMaster.busId = harken::CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    downmixMaster.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.coefficientFirst = 0;
    downmixMaster.coefficientCount = 64;
    image.busDefinitionDownmixes.push_back(downmixMaster);
    harken::PackBusDefinitionDownmixV1 downmixBus2{};
    downmixBus2.busId = 2;
    downmixBus2.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.coefficientFirst = 64;
    downmixBus2.coefficientCount = 64;
    image.busDefinitionDownmixes.push_back(downmixBus2);
    image.busDefinitionCoefficients.assign(128, 0.0f);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data());
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data() + 64);
    header.publishedBusFingerprint = harken::CanonicalNeutralBusFingerprint(image);
    RuntimeBusFixtureV1 fixture;
    fixture.triple = {header.contentGeneration, header.publishedBusFingerprint,
                      header.compileTargetProfileFingerprint};
    fixture.snapshot.assign(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    harken::SerializePackBusDefinitionBusV1(
        master, fixture.snapshot.data() + header.busOffset);
    harken::SerializePackBusDefinitionBusV1(
        bus2, fixture.snapshot.data() + header.busOffset +
                  harken::kPackBusDefinitionBusRowBytes);
    for (std::uint32_t i = 0; i < sendRepeat; ++i) {
        harken::SerializePackBusDefinitionSendV1(
            send, fixture.snapshot.data() + header.sendOffset +
                      i * harken::kPackBusDefinitionSendRowBytes);
    }
    harken::SerializePackBusDefinitionDownmixV1(
        downmixMaster, fixture.snapshot.data() + header.downmixOffset);
    harken::SerializePackBusDefinitionDownmixV1(
        downmixBus2, fixture.snapshot.data() + header.downmixOffset +
                         harken::kPackBusDefinitionDownmixRowBytes);
    for (std::uint32_t i = 0; i < 128; ++i) {
        harken::StoreLEF32(fixture.snapshot.data() + header.coefficientOffset +
                              i * harken::kPackBusDefinitionCoefficientRowBytes,
                          image.busDefinitionCoefficients[i]);
    }
    harken::SerializePackBusDefinitionSnapshotHeaderV1(header, fixture.snapshot.data());
    StampSnapshotChecksum(&fixture.snapshot);
    return fixture;
}

inline std::vector<std::uint8_t> DependencyPayload(
    std::uint64_t prerequisitePackStableId, std::uint64_t contentRevision,
    harken::CanonicalBusTriple triple) {
    harken::PackDependencyEdge edge;
    edge.prerequisitePackStableId = prerequisitePackStableId;
    edge.edgeKind = harken::kPackDependencyEdgeKindInit;
    edge.required01 = 1;
    edge.expectedPrerequisiteContentRevision = contentRevision;
    ApplyCanonicalBusTriple(&edge, triple);
    std::vector<std::uint8_t> payload(harken::kPackDependencyEdgeV2Bytes, 0);
    harken::SerializePackDependencyEdgeV2(edge, payload.data());
    return payload;
}

inline RuntimeBusFixtureV1 Bus2RuntimeBusFixtureV1() {
    const auto profile = DefaultFixtureBusTriple();
    constexpr std::uint64_t kHeader = harken::kPackBusDefinitionSnapshotHeaderBytes;
    constexpr std::uint64_t kBusOffset = kHeader;
    constexpr std::uint64_t kDownmixOffset =
        kBusOffset + 2u * harken::kPackBusDefinitionBusRowBytes;
    constexpr std::uint64_t kCoefficientOffset =
        kDownmixOffset + 2u * harken::kPackBusDefinitionDownmixRowBytes;
    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 1;
    header.storageModelVersion = 1;
    header.headerBytes = harken::kPackBusDefinitionSnapshotHeaderBytes;
    header.publicationCapabilityVintage = 1;
    header.contentGeneration = profile.busContentGeneration;
    header.compileTargetProfileFingerprint =
        profile.compileTargetProfileFingerprint;
    header.busCount = 2;
    header.downmixCount = 2;
    header.coefficientCount = 128;
    header.busOffset = kBusOffset;
    header.sendOffset = kDownmixOffset;
    header.effectOffset = kDownmixOffset;
    header.effectParameterOffset = kDownmixOffset;
    header.downmixOffset = kDownmixOffset;
    header.coefficientOffset = kCoefficientOffset;
    header.payloadBytes =
        (kCoefficientOffset - kHeader) +
        (128u * harken::kPackBusDefinitionCoefficientRowBytes);
    header.profileVintage = 1;
    header.renderSampleRateHz = 48000;
    header.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.runtimeCapabilityMask = harken::busfingerprint::kRequiredBusCapabilityMaskV1;
    harken::PackBusDefinitionBusV1 master{};
    master.busId = harken::CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    master.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    master.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    master.downmixCount = 1;
    master.downmixFirst = 0;
    image.busDefinitionBuses.push_back(master);
    harken::PackBusDefinitionBusV1 bus2{};
    bus2.busId = harken::CanonicalSnapshotPackedBusIdV1(VN_OBJECT_ID(0, 2));
    bus2.parentId = harken::CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    bus2.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    bus2.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    bus2.downmixCount = 1;
    bus2.downmixFirst = 1;
    image.busDefinitionBuses.push_back(bus2);
    harken::PackBusDefinitionDownmixV1 downmixMaster{};
    downmixMaster.busId = harken::CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    downmixMaster.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.coefficientFirst = 0;
    downmixMaster.coefficientCount = 64;
    image.busDefinitionDownmixes.push_back(downmixMaster);
    harken::PackBusDefinitionDownmixV1 downmixBus2{};
    downmixBus2.busId = 2;
    downmixBus2.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.coefficientFirst = 64;
    downmixBus2.coefficientCount = 64;
    image.busDefinitionDownmixes.push_back(downmixBus2);
    image.busDefinitionCoefficients.assign(128, 0.0f);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data());
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data() + 64);
    header.publishedBusFingerprint = harken::CanonicalNeutralBusFingerprint(image);
    RuntimeBusFixtureV1 fixture;
    fixture.triple = {header.contentGeneration, header.publishedBusFingerprint,
                      header.compileTargetProfileFingerprint};
    fixture.snapshot.assign(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    harken::SerializePackBusDefinitionBusV1(
        master, fixture.snapshot.data() + header.busOffset);
    harken::SerializePackBusDefinitionBusV1(
        bus2, fixture.snapshot.data() + header.busOffset +
                  harken::kPackBusDefinitionBusRowBytes);
    harken::SerializePackBusDefinitionDownmixV1(
        downmixMaster, fixture.snapshot.data() + header.downmixOffset);
    harken::SerializePackBusDefinitionDownmixV1(
        downmixBus2, fixture.snapshot.data() + header.downmixOffset +
                         harken::kPackBusDefinitionDownmixRowBytes);
    for (std::uint32_t i = 0; i < 128; ++i) {
        harken::StoreLEF32(fixture.snapshot.data() + header.coefficientOffset +
                              i * harken::kPackBusDefinitionCoefficientRowBytes,
                          image.busDefinitionCoefficients[i]);
    }
    harken::SerializePackBusDefinitionSnapshotHeaderV1(header, fixture.snapshot.data());
    StampSnapshotChecksum(&fixture.snapshot);
    return fixture;
}

inline std::vector<std::uint8_t> RequirementPayload(
    std::uint64_t packStableId, std::uint64_t contentRevision,
    std::uint64_t partitionRevision, std::uint32_t definitionCount,
    std::uint64_t definitionBytes,
    harken::CanonicalBusTriple triple = DefaultFixtureBusTriple()) {
    harken::PackRequirementBlock block;
    block.schemaVersion = 2;
    block.storageModelVersion = 2;
    block.packStableId = packStableId;
    block.packContentRevision = contentRevision;
    block.definitionPartitionRevision = partitionRevision;
    block.resourceClosureRevision = 1;
    block.semanticFingerprint = packStableId ^ contentRevision;
    block.lineageFingerprint = packStableId;
    block.ownedDefinitionCount = definitionCount;
    block.installedDefinitionBytes = definitionBytes;
    block.replacementPeakBytes = definitionBytes * 2;
    block.decodeStagingBytes = definitionBytes;
    block.requiredCapabilityMask = static_cast<std::uint32_t>(
        harken::kWorldCapabilityUnit1ActionMask);
    ApplyCanonicalBusTriple(&block, triple);
    std::vector<std::uint8_t> payload(
        harken::kPackRequirementBlockV2Bytes, 0);
    harken::SerializePackRequirementBlockV2(block, payload.data());
    StampRequirementCoverage(&payload);
    return payload;
}

inline void SetAbsoluteTime(harken::AuthoredTimeRationalV1* time) {
    time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    time->numerator = 0;
    time->denominator = 1;
}

inline std::vector<std::uint8_t> PartitionPayload(
    std::uint64_t packStableId, std::uint64_t revision,
    std::uint32_t actionCount, std::uint32_t valueBits,
    std::uint64_t* bodyBytes, std::uint32_t maxActionDefinitions = 64,
    VnObjectId projectId = {},
    harken::CanonicalBusTriple triple = DefaultFixtureBusTriple()) {
    harken::VariableDefinitionV1 variable{};
    variable.variableId = kVariableId;
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMin = -1.0f;
    variable.rangeMax = 1.0f;

    std::vector<harken::ActionStepProgramViewV1> steps(actionCount);
    std::vector<harken::ActionProgramViewV1> actions(actionCount);
    for (std::uint32_t i = 0; i < actionCount; ++i) {
        steps[i].opcode = harken::ActionOpcodeV1::SetVariable;
        steps[i].target.kind = harken::ActionTargetKindV1::Variable;
        steps[i].target.stableId = kVariableId;
        steps[i].valueBits = valueBits;
        SetAbsoluteTime(&steps[i].delay);
        SetAbsoluteTime(&steps[i].fade);
        SetAbsoluteTime(&steps[i].cooldown);

        actions[i].actionStableId = harkentest::ObjectId(kActionBase + i);
        actions[i].steps = &steps[i];
        actions[i].stepCount = 1;
        actions[i].expandedStepOccurrenceCount = 1;
        actions[i].expandedFiringCount = 1;
        actions[i].programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(actions[i]);
    }

    harken::ContainerWorldInstallViewV1 world{};
    world.identity.projectId = projectId;
    world.capacity.maxActionDefinitions = maxActionDefinitions;
    world.identity.capabilityMask =
        (actionCount == 0 ? 0 : harken::kWorldCapabilityVariableDefinitions) |
        (actionCount == 0 ? 0 : harken::kWorldCapabilityActionDefinitions);
    world.variables = actionCount == 0 ? nullptr : &variable;
    world.variableCount = actionCount == 0 ? 0 : 1;
    world.actions = actionCount == 0 ? nullptr : actions.data();
    world.actionCount = actionCount;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<std::uint8_t> body;
    if (!harken::EncodeCurrentWorldShippingBody(world, body)) {
        return {};
    }
    *bodyBytes = body.size();

    harken::PackCanonicalPartitionHeader header;
    header.headerBytes = harken::kCanonicalDefinitionPartitionHeaderV2Bytes;
    header.bodyVintage = harken::kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = harken::Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount =
        actionCount == 0 ? 0 : actionCount + 1;
    ApplyCanonicalBusTriple(&header, triple);
    std::vector<std::uint8_t> payload(
        harken::kCanonicalDefinitionPartitionHeaderV2Bytes + body.size(), 0);
    harken::SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    header.headerChecksum = harken::Fnv1a64(payload.data(), 80);
    harken::SerializePackCanonicalPartitionHeaderV2(header, payload.data());
    std::memcpy(payload.data() +
                    harken::kCanonicalDefinitionPartitionHeaderV2Bytes,
                body.data(), body.size());
    return payload;
}

inline std::vector<std::uint8_t> BuildContent(
    std::uint64_t revision, std::uint32_t actionCount,
    std::uint32_t valueBits, std::uint64_t* bodyBytes) {
    std::vector<std::uint8_t> partition = PartitionPayload(
        kResidentPackStableId, revision, actionCount, valueBits, bodyBytes);
    if (partition.empty()) {
        return {};
    }
    ImageBuilder builder(harken::kPackKindCanonicalContent);
    builder.AddEntry(
        harken::kPackEntryKindPackRequirementBlock,
        kResidentPackStableId,
        RequirementPayload(kResidentPackStableId, revision, revision,
                           actionCount + 1, *bodyBytes));
    builder.AddEntry(
        harken::kPackEntryKindCanonicalDefinitionPartition,
        kResidentPackStableId, std::move(partition));
    return builder.Build();
}

inline std::vector<std::uint8_t> BuildContentWithInitDependency(
    std::uint64_t contentStableId, std::uint64_t contentRevision,
    std::uint64_t initStableId, std::uint64_t initContentRevision,
    harken::CanonicalBusTriple triple, std::uint64_t* bodyBytes) {
    std::vector<std::uint8_t> partition = PartitionPayload(
        contentStableId, contentRevision, 0, 0, bodyBytes, 64, {}, triple);
    if (partition.empty()) {
        return {};
    }
    ImageBuilder builder(harken::kPackKindCanonicalContent);
    builder.AddEntry(harken::kPackEntryKindPackRequirementBlock, contentStableId,
                     RequirementPayload(contentStableId, contentRevision,
                                        contentRevision, 0, *bodyBytes, triple));
    builder.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition,
                     contentStableId, std::move(partition));
    builder.AddEntry(harken::kPackEntryKindPackDependencyEdge, VnObjectId{},
                     DependencyPayload(initStableId, initContentRevision, triple),
                     harken::kPackCanonicalRequirementVintage);
    return builder.Build();
}

inline bool FillLocator(harken::PackInitCatalogRow* row,
                        const char* locator) {
    const std::size_t length = std::strlen(locator);
    if (length + 1 > row->relativeLocator.size()) {
        return false;
    }
    row->locatorByteCount = static_cast<std::uint32_t>(length + 1);
    std::memcpy(row->relativeLocator.data(), locator, length + 1);
    return true;
}

inline std::vector<std::uint8_t> CatalogPayload(
    harken::PackInitCatalogHeader header,
    const std::vector<harken::PackInitCatalogRow>& rows,
    const std::vector<std::uint64_t>& startup) {
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(header.catalogBytes), 0);
    harken::SerializePackInitCatalogHeader(header, payload.data());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        harken::SerializePackInitCatalogRow(
            rows[i], payload.data() + harken::kInitCatalogHeaderBytes +
                         i * harken::kInitCatalogRowBytes);
    }
    for (std::size_t i = 0; i < startup.size(); ++i) {
        harken::StoreLE64(
            payload.data() + harken::kInitCatalogHeaderBytes +
                rows.size() * harken::kInitCatalogRowBytes + i * 8,
            startup[i]);
    }
    header.checksum = harken::Fnv1a64(
        payload.data() + harken::kInitCatalogHeaderBytes,
        static_cast<std::size_t>(header.catalogBytes -
                                 harken::kInitCatalogHeaderBytes));
    harken::SerializePackInitCatalogHeader(header, payload.data());
    return payload;
}

inline bool BuildFixture(std::uint32_t catalogCount,
                         std::uint32_t residentCount,
                         Fixture* fixture) {
    if (fixture == nullptr || catalogCount == 0 || residentCount == 0 ||
        residentCount >= catalogCount ||
        catalogCount > harken::kInitCatalogMaxEntries) {
        return false;
    }

    Fixture next;
    next.catalogCount = catalogCount;
    next.residentCount = residentCount;
    next.resident = BuildContent(
        1, residentCount, 0x3f000000u, &next.residentBodyBytes);
    std::uint64_t replacementBodyBytes = 0;
    next.replacement = BuildContent(
        2, residentCount, 0xbf000000u, &replacementBodyBytes);
    if (next.resident.empty() || next.replacement.empty() ||
        replacementBodyBytes != next.residentBodyBytes) {
        return false;
    }

    const std::uint64_t residentHash = harken::Fnv1a64(
        next.resident.data(), next.resident.size());
    std::vector<harken::PackInitCatalogRow> rows(catalogCount);
    for (std::uint32_t i = 0; i < catalogCount; ++i) {
        rows[i].stableId = harkentest::ObjectId(kActionBase + i);
        rows[i].identityClass =
            harken::kInitCatalogIdentityClassAction;
        if (i < residentCount) {
            rows[i].ownerPackStableId = kResidentPackStableId;
            rows[i].expectedContentHash = residentHash;
            if (!FillLocator(&rows[i], "Content.pack")) {
                return false;
            }
        } else {
            rows[i].ownerPackStableId = kUnloadedPackStableId;
            rows[i].expectedContentHash = 1;
            if (!FillLocator(&rows[i], "Unloaded.pack")) {
                return false;
            }
        }
    }

    harken::PackInitCatalogHeader catalog;
    catalog.schemaVersion = 1;
    catalog.storageModelVersion = 1;
    catalog.projectBuildId = 0xF3000001;
    catalog.platformId = 1;
    catalog.languageId = 1;
    catalog.totalDefinitionCount = catalogCount + 1;
    catalog.catalogEntryCount = catalogCount;
    catalog.startupPackCount = 1;
    catalog.recommendedCatalogCapacity = catalogCount;
    if (!harken::InitCatalogBytes(catalog.catalogEntryCount,
                                 catalog.startupPackCount,
                                 &catalog.catalogBytes)) {
        return false;
    }
    catalog.recommendedCatalogBytes = catalog.catalogBytes;
    next.catalogBytes = catalog.catalogBytes;

    std::vector<std::uint8_t> initPartition = PartitionPayload(
        kInitPackStableId, 1, 0, 0, &next.initBodyBytes);
    if (initPartition.empty()) {
        return false;
    }
    ImageBuilder initBuilder(harken::kPackKindInit);
    initBuilder.AddEntry(
        harken::kPackEntryKindInitRoutingCatalog, 0,
        CatalogPayload(catalog, rows, {kResidentPackStableId}));
    initBuilder.AddEntry(
        harken::kPackEntryKindPackRequirementBlock, kInitPackStableId,
        RequirementPayload(kInitPackStableId, 1, 1, 0, next.initBodyBytes));
    initBuilder.AddEntry(
        harken::kPackEntryKindCanonicalDefinitionPartition,
        kInitPackStableId, std::move(initPartition));
    AppendNeutralMasterSnapshotV1(&initBuilder);
    next.init = initBuilder.Build();
    if (next.init.empty()) {
        return false;
    }
    *fixture = std::move(next);
    return true;
}

inline bool WriteFile(const std::filesystem::path& path,
                      const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

inline bool WriteFixture(const std::filesystem::path& directory,
                         const Fixture& fixture) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return !error &&
           WriteFile(directory / "Init.pack", fixture.init) &&
           WriteFile(directory / "Content.pack", fixture.resident) &&
           WriteFile(directory / "ContentNext.pack", fixture.replacement);
}

}
