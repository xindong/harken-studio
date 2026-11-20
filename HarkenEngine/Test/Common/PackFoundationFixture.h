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
inline constexpr std::uint64_t kActionBase = 0xAC710100ull;
inline constexpr VnObjectId kVariableId = harkentest::ObjectId(7);

struct Fixture {
    std::uint32_t residentCount = 0;
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
                         std::uint32_t vintage = 0) {
        entries_.push_back({kind, id, 0, std::move(payload), {}, vintage});
        return entries_.size() - 1;
    }

    void SetRelationOrdinal(std::size_t index, std::uint64_t ordinal) {
        entries_[index].relationOrdinal = ordinal;
    }

    // Samples are planar: channel 0's frames, then channel 1's, and so on.
    void AddMedia(VnObjectId id, std::vector<float> samples,
                  harken::PcmLoopRegion loop = {}, std::uint16_t channels = 1) {
        entries_.push_back({HK_PACK_ENTRY_KIND_MEDIA, id, 0,
                           std::vector<std::uint8_t>(harken::kPackBytes<HkPackMedia>),
                           std::move(samples), 0, loop, channels});
    }

    void AddVorbis(VnObjectId id, std::vector<std::uint8_t> blob, std::uint64_t frames,
                   std::uint16_t channels, harken::PcmLoopRegion loop = {}) {
        AddMedia(id, {}, loop, channels);
        entries_.back().encoded = std::move(blob);
        entries_.back().frames = frames;
    }

    std::vector<std::uint8_t> Build() {
        const std::uint64_t directoryOffset = harken::kPackBytes<HkPackFileHeader>;
        const std::uint64_t directoryBytes =
            entries_.size() * harken::kPackBytes<HkPackDirectoryEntry>;
        const std::uint64_t directoryEnd =
            directoryOffset + directoryBytes;
        std::vector<std::uint64_t> offsets(entries_.size(), 0);
        std::uint64_t cursor =
            AlignUp(directoryEnd, HK_PACK_PAYLOAD_ALIGN);
        std::uint64_t contentEnd = directoryEnd;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            offsets[i] = cursor;
            contentEnd = cursor + entries_[i].payload.size();
            cursor = AlignUp(contentEnd, HK_PACK_PAYLOAD_ALIGN);
        }

        std::vector<std::uint64_t> mediaOffsets(entries_.size(), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].samples.empty() && entries_[i].encoded.empty()) {
                continue;
            }
            mediaOffsets[i] = AlignUp(contentEnd, HK_PACK_BLOB_ALIGN);
            contentEnd = mediaOffsets[i] + entries_[i].samples.size() * sizeof(float) + entries_[i].encoded.size();
        }

        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(contentEnd), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!entries_[i].samples.empty() || !entries_[i].encoded.empty()) {
                harken::PackTocEntry media;
                media.soundId = entries_[i].id;
                media.sampleRate = 48000;
                media.channelCount = entries_[i].channels;
                media.sampleFormat = HK_PACK_SAMPLE_FORMAT_F32_PLANAR;
                media.frameCount = entries_[i].samples.size() / entries_[i].channels;
                media.flags = entries_[i].loop.endFrame != 0 ||
                                      entries_[i].loop.startFrame != 0
                                  ? HK_PACK_MEDIA_FLAG_LOOPABLE : 0;
                media.loopStartFrame = entries_[i].loop.startFrame;
                media.loopEndFrame = entries_[i].loop.endFrame;
                media.blobOffset = mediaOffsets[i];
                const bool encoded = !entries_[i].encoded.empty();
                if (encoded) {
                    media.frameCount = entries_[i].frames;
                    media.sampleFormat = HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT;
                }
                media.blobBytes = encoded ? entries_[i].encoded.size() : entries_[i].samples.size() * sizeof(float);
                const void* source = encoded ? static_cast<const void*>(entries_[i].encoded.data())
                                             : static_cast<const void*>(entries_[i].samples.data());
                std::memcpy(bytes.data() + media.blobOffset, source, media.blobBytes);
                media.blobChecksum = harken::Fnv1a64(bytes.data() + media.blobOffset,
                                                   media.blobBytes);
                harken::SerializePackRow<HkPackMedia>(media, entries_[i].payload.data());
            }
            std::memcpy(bytes.data() + offsets[i],
                        entries_[i].payload.data(),
                        entries_[i].payload.size());
            harken::PackDirectoryEntry record;
            record.kind = entries_[i].kind;
            if (entries_[i].vintage != 0) {
                record.vintage = entries_[i].vintage;
            } else {
                record.vintage =
                    harken::PackEntryVintage(entries_[i].kind);
            }
            record.id = entries_[i].id;
            record.relationOrdinal = entries_[i].relationOrdinal;
            record.offset = offsets[i];
            record.size = entries_[i].payload.size();
            record.payloadChecksum = harken::Fnv1a64(
                bytes.data() + offsets[i], entries_[i].payload.size());
            harken::SerializePackRow<HkPackDirectoryEntry>(
                record, bytes.data() + directoryOffset +
                            i * harken::kPackBytes<HkPackDirectoryEntry>);
        }

        harken::PackFileHeader header;
        header.formatVersion = HK_PACK_FORMAT_VERSION;
        header.headerBytes = harken::kPackBytes<HkPackFileHeader>;
        header.fileBytes = contentEnd;
        header.packKind = packKind_;
        header.entryCount = static_cast<std::uint32_t>(entries_.size());
        header.directoryOffset = directoryOffset;
        header.directoryChecksum = harken::Fnv1a64(
            bytes.data() + directoryOffset,
            static_cast<std::size_t>(directoryBytes));
        harken::SerializePackRow<HkPackFileHeader>(header, bytes.data());
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
        harken::PcmLoopRegion loop{};
        std::uint16_t channels = 1;
        std::vector<std::uint8_t> encoded;
        std::uint64_t frames = 0;
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

using harken::NeutralMasterSnapshotPayload;

template <typename Builder>
inline void AppendNeutralMasterSnapshot(
    Builder* builder,
    harken::CanonicalBusTriple triple = DefaultFixtureBusTriple()) {
    builder->AddEntry(HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT, 1,
                      NeutralMasterSnapshotPayload(triple));
}

struct RuntimeBusFixture {
    harken::CanonicalBusTriple triple{};
    std::vector<std::uint8_t> snapshot;
    std::vector<harken::PackBusEntry> buses;
    std::vector<harken::PackBusSendEntry> sends;
};

template <typename Builder>
inline void AppendRuntimeBusFixture(Builder* builder, const RuntimeBusFixture& fixture) {
    for (const auto& bus : fixture.buses) {
        std::vector<std::uint8_t> payload(harken::kPackBytes<HkPackBus>);
        harken::SerializePackRow<HkPackBus>(bus, payload.data());
        builder->AddEntry(HK_PACK_ENTRY_KIND_BUS, bus.busId, std::move(payload),
                          HK_PACK_BUS_VINTAGE);
    }
    for (std::size_t i = 0; i < fixture.sends.size(); ++i) {
        std::vector<std::uint8_t> payload(harken::kPackBytes<HkPackBusSend>);
        harken::SerializePackRow<HkPackBusSend>(fixture.sends[i], payload.data());
        builder->AddEntry(HK_PACK_ENTRY_KIND_BUS_SEND, static_cast<std::uint64_t>(i),
                          std::move(payload), HK_PACK_BUS_SEND_VINTAGE);
    }
    builder->AddEntry(HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT, 1, fixture.snapshot);
}

inline RuntimeBusFixture DefaultRuntimeBusFixture() {
    RuntimeBusFixture fixture;
    fixture.triple = DefaultFixtureBusTriple();
    fixture.snapshot = NeutralMasterSnapshotPayload(fixture.triple);
    return fixture;
}

inline RuntimeBusFixture MasterSendRuntimeBusFixture(
    float levelDb, std::uint32_t sendRepeat = 1,
    VnBusId targetBusId = VN_OBJECT_ID(0, 3)) {
    const auto profile = DefaultFixtureBusTriple();
    constexpr std::uint64_t kHeader = harken::kPackBytes<HkPackBusDefinitionSnapshotHeader>;
    constexpr std::uint64_t kBusOffset = kHeader;
    const std::uint64_t kSendOffset =
        kBusOffset + 3u * harken::kPackBytes<HkPackBusDefinitionBus>;
    const std::uint64_t kDownmixOffset =
        kSendOffset + static_cast<std::uint64_t>(sendRepeat) *
                          harken::kPackBytes<HkPackBusDefinitionSend>;
    const std::uint64_t kCoefficientOffset =
        kDownmixOffset + 3u * harken::kPackBytes<HkPackBusDefinitionDownmix>;
    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 2;
    header.storageModelVersion = 2;
    header.headerBytes = harken::kPackBytes<HkPackBusDefinitionSnapshotHeader>;
    header.busRowBytes = harken::kPackBytes<HkPackBusDefinitionBus>;
    header.sendRowBytes = harken::kPackBytes<HkPackBusDefinitionSend>;
    header.downmixRowBytes = harken::kPackBytes<HkPackBusDefinitionDownmix>;
    header.coefficientRowBytes = harken::kPackBytes<HkPackBusDefinitionCoefficient>;
    header.publicationCapabilityVintage = 4;
    header.contentGeneration = profile.busContentGeneration;
    header.compileTargetProfileFingerprint =
        profile.compileTargetProfileFingerprint;
    header.busCount = 3;
    header.sendCount = sendRepeat;
    header.downmixCount = 3;
    header.coefficientCount = 192;
    header.busOffset = kBusOffset;
    header.sendOffset = kSendOffset;
    header.effectOffset = kDownmixOffset;
    header.effectParameterOffset = kDownmixOffset;
    header.downmixOffset = kDownmixOffset;
    header.coefficientOffset = kCoefficientOffset;
    header.payloadBytes =
        (kCoefficientOffset - kHeader) +
        (192u * harken::kPackBytes<HkPackBusDefinitionCoefficient>);
    header.profileVintage = 1;
    header.renderSampleRateHz = 48000;
    header.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.runtimeCapabilityMask = harken::busfingerprint::kRequiredBusCapabilityMaskV1;
    harken::PackBusDefinitionBus master{};
    master.busId = VN_BUS_MASTER;
    master.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    master.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    master.downmixCount = 1;
    master.downmixFirst = 0;
    image.busDefinitionBusesV2.push_back(master);
    harken::PackBusDefinitionBus bus2{};
    bus2.busId = VN_OBJECT_ID(0, 2);
    bus2.parentId = VN_BUS_MASTER;
    bus2.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    bus2.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    bus2.downmixCount = 1;
    bus2.downmixFirst = 1;
    image.busDefinitionBusesV2.push_back(bus2);
    harken::PackBusDefinitionBus bus3 = bus2;
    bus3.busId = VN_OBJECT_ID(0, 3);
    bus3.downmixFirst = 2;
    image.busDefinitionBusesV2.push_back(bus3);
    harken::PackBusDefinitionSend send{};
    send.source = VN_OBJECT_ID(0, 2);
    send.target = targetBusId;
    send.levelDb = levelDb;
    for (std::uint32_t i = 0; i < sendRepeat; ++i) {
        image.busDefinitionSendsV2.push_back(send);
    }
    harken::PackBusDefinitionDownmix downmixMaster{};
    downmixMaster.busId = VN_BUS_MASTER;
    downmixMaster.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.coefficientFirst = 0;
    downmixMaster.coefficientCount = 64;
    image.busDefinitionDownmixesV2.push_back(downmixMaster);
    harken::PackBusDefinitionDownmix downmixBus2{};
    downmixBus2.busId = VN_OBJECT_ID(0, 2);
    downmixBus2.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.coefficientFirst = 64;
    downmixBus2.coefficientCount = 64;
    image.busDefinitionDownmixesV2.push_back(downmixBus2);
    harken::PackBusDefinitionDownmix downmixBus3 = downmixBus2;
    downmixBus3.busId = VN_OBJECT_ID(0, 3);
    downmixBus3.coefficientFirst = 128;
    image.busDefinitionDownmixesV2.push_back(downmixBus3);
    image.busDefinitionCoefficients.assign(192, 0.0f);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data());
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data() + 64);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data() + 128);
    header.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image);
    header.contentGeneration = header.publishedBusFingerprint;
    RuntimeBusFixture fixture;
    fixture.buses = {{bus2.busId, bus2.parentId}, {bus3.busId, bus3.parentId}};
    fixture.sends.assign(sendRepeat, {send.source, send.target, send.levelDb});
    fixture.triple = {header.contentGeneration, header.publishedBusFingerprint,
                      header.compileTargetProfileFingerprint};
    fixture.snapshot.assign(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    harken::SerializePackRow<HkPackBusDefinitionBus>(
        master, fixture.snapshot.data() + header.busOffset);
    harken::SerializePackRow<HkPackBusDefinitionBus>(
        bus2, fixture.snapshot.data() + header.busOffset +
                  harken::kPackBytes<HkPackBusDefinitionBus>);
    harken::SerializePackRow<HkPackBusDefinitionBus>(
        bus3, fixture.snapshot.data() + header.busOffset +
                  2u * harken::kPackBytes<HkPackBusDefinitionBus>);
    for (std::uint32_t i = 0; i < sendRepeat; ++i) {
        harken::SerializePackRow<HkPackBusDefinitionSend>(
            send, fixture.snapshot.data() + header.sendOffset +
                      i * harken::kPackBytes<HkPackBusDefinitionSend>);
    }
    harken::SerializePackRow<HkPackBusDefinitionDownmix>(
        downmixMaster, fixture.snapshot.data() + header.downmixOffset);
    harken::SerializePackRow<HkPackBusDefinitionDownmix>(
        downmixBus2, fixture.snapshot.data() + header.downmixOffset +
                         harken::kPackBytes<HkPackBusDefinitionDownmix>);
    harken::SerializePackRow<HkPackBusDefinitionDownmix>(
        downmixBus3, fixture.snapshot.data() + header.downmixOffset +
                         2u * harken::kPackBytes<HkPackBusDefinitionDownmix>);
    for (std::uint32_t i = 0; i < 192; ++i) {
        harken::SerializePackRow<HkPackBusDefinitionCoefficient>(image.busDefinitionCoefficients[i], fixture.snapshot.data() + header.coefficientOffset +
                              i * harken::kPackBytes<HkPackBusDefinitionCoefficient>);
    }
    harken::SerializePackRow<HkPackBusDefinitionSnapshotHeader>(header, fixture.snapshot.data());
    StampSnapshotChecksum(&fixture.snapshot);
    return fixture;
}

inline std::vector<std::uint8_t> DependencyPayload(
    std::uint64_t prerequisitePackStableId, std::uint64_t contentRevision,
    harken::CanonicalBusTriple triple) {
    harken::PackDependencyEdge edge;
    edge.prerequisitePackStableId = prerequisitePackStableId;
    edge.edgeKind = HK_PACK_DEPENDENCY_EDGE_KIND_INIT;
    edge.required01 = 1;
    edge.expectedPrerequisiteContentRevision = contentRevision;
    ApplyCanonicalBusTriple(&edge, triple);
    std::vector<std::uint8_t> payload(harken::kPackBytes<HkPackDependencyEdge>, 0);
    harken::SerializePackRow<HkPackDependencyEdge>(edge, payload.data());
    return payload;
}

inline RuntimeBusFixture Bus2RuntimeBusFixture() {
    const auto profile = DefaultFixtureBusTriple();
    constexpr std::uint64_t kHeader = harken::kPackBytes<HkPackBusDefinitionSnapshotHeader>;
    constexpr std::uint64_t kBusOffset = kHeader;
    constexpr std::uint64_t kDownmixOffset =
        kBusOffset + 2u * harken::kPackBytes<HkPackBusDefinitionBus>;
    constexpr std::uint64_t kCoefficientOffset =
        kDownmixOffset + 2u * harken::kPackBytes<HkPackBusDefinitionDownmix>;
    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 2;
    header.storageModelVersion = 2;
    header.headerBytes = harken::kPackBytes<HkPackBusDefinitionSnapshotHeader>;
    header.busRowBytes = harken::kPackBytes<HkPackBusDefinitionBus>;
    header.sendRowBytes = harken::kPackBytes<HkPackBusDefinitionSend>;
    header.downmixRowBytes = harken::kPackBytes<HkPackBusDefinitionDownmix>;
    header.coefficientRowBytes = harken::kPackBytes<HkPackBusDefinitionCoefficient>;
    header.publicationCapabilityVintage = 3;
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
        (128u * harken::kPackBytes<HkPackBusDefinitionCoefficient>);
    header.profileVintage = 1;
    header.renderSampleRateHz = 48000;
    header.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.runtimeCapabilityMask = harken::busfingerprint::kRequiredBusCapabilityMaskV1;
    harken::PackBusDefinitionBus master{};
    master.busId = VN_BUS_MASTER;
    master.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    master.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    master.downmixCount = 1;
    master.downmixFirst = 0;
    image.busDefinitionBusesV2.push_back(master);
    harken::PackBusDefinitionBus bus2{};
    bus2.busId = VN_OBJECT_ID(0, 2);
    bus2.parentId = VN_BUS_MASTER;
    bus2.faderAfterSlot = harken::busfingerprint::kNeutralFaderAfterSlotV1;
    bus2.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    bus2.downmixCount = 1;
    bus2.downmixFirst = 1;
    image.busDefinitionBusesV2.push_back(bus2);
    harken::PackBusDefinitionDownmix downmixMaster{};
    downmixMaster.busId = VN_BUS_MASTER;
    downmixMaster.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixMaster.coefficientFirst = 0;
    downmixMaster.coefficientCount = 64;
    image.busDefinitionDownmixesV2.push_back(downmixMaster);
    harken::PackBusDefinitionDownmix downmixBus2{};
    downmixBus2.busId = VN_OBJECT_ID(0, 2);
    downmixBus2.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmixBus2.coefficientFirst = 64;
    downmixBus2.coefficientCount = 64;
    image.busDefinitionDownmixesV2.push_back(downmixBus2);
    image.busDefinitionCoefficients.assign(128, 0.0f);
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data());
    harken::DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                                image.busDefinitionCoefficients.data() + 64);
    header.publishedBusFingerprint = harken::CanonicalVolumeBusFingerprint(image);
    header.contentGeneration = header.publishedBusFingerprint;
    RuntimeBusFixture fixture;
    fixture.buses = {{bus2.busId, bus2.parentId}};
    fixture.triple = {header.contentGeneration, header.publishedBusFingerprint,
                      header.compileTargetProfileFingerprint};
    fixture.snapshot.assign(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    harken::SerializePackRow<HkPackBusDefinitionBus>(
        master, fixture.snapshot.data() + header.busOffset);
    harken::SerializePackRow<HkPackBusDefinitionBus>(
        bus2, fixture.snapshot.data() + header.busOffset +
                  harken::kPackBytes<HkPackBusDefinitionBus>);
    harken::SerializePackRow<HkPackBusDefinitionDownmix>(
        downmixMaster, fixture.snapshot.data() + header.downmixOffset);
    harken::SerializePackRow<HkPackBusDefinitionDownmix>(
        downmixBus2, fixture.snapshot.data() + header.downmixOffset +
                         harken::kPackBytes<HkPackBusDefinitionDownmix>);
    for (std::uint32_t i = 0; i < 128; ++i) {
        harken::SerializePackRow<HkPackBusDefinitionCoefficient>(image.busDefinitionCoefficients[i], fixture.snapshot.data() + header.coefficientOffset +
                              i * harken::kPackBytes<HkPackBusDefinitionCoefficient>);
    }
    harken::SerializePackRow<HkPackBusDefinitionSnapshotHeader>(header, fixture.snapshot.data());
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
        harken::kPackBytes<HkPackRequirementBlock>, 0);
    harken::SerializePackRow<HkPackRequirementBlock>(block, payload.data());
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
    header.headerBytes = harken::kPackBytes<HkPackCanonicalPartitionHeader>;
    header.bodyVintage = harken::kCanonicalDefinitionPartitionBodyVintage;
    header.bodyBytes = body.size();
    header.bodyChecksum = harken::Fnv1a64(body.data(), body.size());
    header.packStableId = packStableId;
    header.partitionRevision = revision;
    header.ownedDefinitionCount =
        actionCount == 0 ? 0 : actionCount + 1;
    ApplyCanonicalBusTriple(&header, triple);
    std::vector<std::uint8_t> payload(
        harken::kPackBytes<HkPackCanonicalPartitionHeader> + body.size(), 0);
    harken::SerializePackRow<HkPackCanonicalPartitionHeader>(header, payload.data());
    header.headerChecksum = harken::Fnv1a64(payload.data(), offsetof(HkPackCanonicalPartitionHeader, headerChecksum));
    harken::SerializePackRow<HkPackCanonicalPartitionHeader>(header, payload.data());
    std::memcpy(payload.data() +
                    harken::kPackBytes<HkPackCanonicalPartitionHeader>,
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
    ImageBuilder builder(HK_PACK_KIND_CANONICAL_CONTENT);
    builder.AddEntry(
        HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK,
        kResidentPackStableId,
        RequirementPayload(kResidentPackStableId, revision, revision,
                           actionCount + 1, *bodyBytes));
    builder.AddEntry(
        HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION,
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
    ImageBuilder builder(HK_PACK_KIND_CANONICAL_CONTENT);
    builder.AddEntry(HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK, contentStableId,
                     RequirementPayload(contentStableId, contentRevision,
                                        contentRevision, 0, *bodyBytes, triple));
    builder.AddEntry(HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION,
                     contentStableId, std::move(partition));
    builder.AddEntry(HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE, VnObjectId{},
                     DependencyPayload(initStableId, initContentRevision, triple),
                     HK_PACK_PACK_DEPENDENCY_EDGE_VINTAGE);
    return builder.Build();
}





inline bool BuildFixture(std::uint32_t residentCount,
                         Fixture* fixture) {
    if (fixture == nullptr || residentCount == 0) {
        return false;
    }

    Fixture next;
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

    std::vector<std::uint8_t> initPartition = PartitionPayload(
        kInitPackStableId, 1, 0, 0, &next.initBodyBytes);
    if (initPartition.empty()) {
        return false;
    }
    ImageBuilder initBuilder(HK_PACK_KIND_INIT);
    initBuilder.AddEntry(
        HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK, kInitPackStableId,
        RequirementPayload(kInitPackStableId, 1, 1, 0, next.initBodyBytes));
    initBuilder.AddEntry(
        HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION,
        kInitPackStableId, std::move(initPartition));
    AppendNeutralMasterSnapshot(&initBuilder);
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
