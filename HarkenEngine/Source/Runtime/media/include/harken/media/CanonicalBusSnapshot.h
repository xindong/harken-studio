#ifndef HARKEN_MEDIA_CANONICAL_BUS_SNAPSHOT_H
#define HARKEN_MEDIA_CANONICAL_BUS_SNAPSHOT_H

#include "harken/core/ChannelLayout.h"
#include "harken/media/PackLogicImage.h"

#include <cstdint>
#include <vector>

namespace harken {

inline void StampRequirementCoverage(std::vector<std::uint8_t>* payload) {
    StorePackRequirementBlockPayloadChecksumCoverage(payload->data(),
        Fnv1a64(payload->data(), kPackRequirementBlockPayloadChecksumCoverageOffset));
}

inline void StampSnapshotChecksum(std::vector<std::uint8_t>* payload) {
    constexpr auto checksumOffset = kPackBusDefinitionSnapshotHeaderChecksumOffset;
    constexpr auto checksumEnd = kPackBusDefinitionSnapshotHeaderCompileTargetProfileFingerprintOffset;
    const std::uint8_t zeros[checksumEnd - checksumOffset] = {};
    std::uint64_t hash = Fnv1a64(payload->data(), checksumOffset);
    hash = Fnv1a64(zeros, sizeof(zeros), hash);
    hash = Fnv1a64(payload->data() + checksumEnd, payload->size() - checksumEnd, hash);
    StorePackBusDefinitionSnapshotHeaderChecksum(payload->data(), hash);
}

inline void ApplyCanonicalBusTriple(PackRequirementBlock* block,
                                    CanonicalBusTriple triple) {
    block->busContentGeneration = triple.busContentGeneration;
    block->publishedBusFingerprint = triple.publishedBusFingerprint;
    block->compileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
}

inline void ApplyCanonicalBusTriple(PackCanonicalPartitionHeader* header,
                                    CanonicalBusTriple triple) {
    header->busContentGeneration = triple.busContentGeneration;
    header->publishedBusFingerprint = triple.publishedBusFingerprint;
    header->compileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
}

inline void ApplyCanonicalBusTriple(PackDependencyEdge* edge,
                                    CanonicalBusTriple triple) {
    edge->expectedBusContentGeneration = triple.busContentGeneration;
    edge->expectedPublishedBusFingerprint = triple.publishedBusFingerprint;
    edge->expectedCompileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
}

inline PackCanonicalImage NeutralMasterSnapshotImage() {
    PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 2;
    header.storageModelVersion = 2;
    header.headerBytes = kPackBusDefinitionSnapshotHeaderBytes;
    header.busRowBytes = kPackBusDefinitionBusBytes;
    header.sendRowBytes = kPackBusDefinitionSendBytes;
    header.downmixRowBytes = kPackBusDefinitionDownmixBytes;
    header.coefficientRowBytes = kPackBusDefinitionCoefficientBytes;
    header.publicationCapabilityVintage = 3;
    header.contentGeneration = kNeutralMasterBusContentGeneration;
    header.profileVintage = 1;
    header.renderSampleRateHz = 48000;
    header.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    header.runtimeCapabilityMask = busfingerprint::kRequiredBusCapabilityMaskV1;
    header.compileTargetProfileFingerprint = busfingerprint::ProfileFingerprintV1(
        header.profileVintage, header.renderSampleRateHz, header.mixLayout,
        header.deviceLayout, header.runtimeCapabilityMask);
    header.busCount = 1;
    header.downmixCount = 1;
    header.coefficientCount = 64;
    PackBusDefinitionBus bus{};
    bus.busId = VN_BUS_MASTER;
    bus.faderAfterSlot = busfingerprint::kNeutralFaderAfterSlotV1;
    bus.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    bus.downmixCount = 1;
    bus.downmixFirst = 0;
    image.busDefinitionBusesV2.push_back(bus);
    PackBusDefinitionDownmix downmix{};
    downmix.busId = VN_BUS_MASTER;
    downmix.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmix.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmix.coefficientFirst = 0;
    downmix.coefficientCount = 64;
    image.busDefinitionDownmixesV2.push_back(downmix);
    image.busDefinitionCoefficients.assign(64, 0.0f);
    DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                         image.busDefinitionCoefficients.data());
    header.publishedBusFingerprint = CanonicalVolumeBusFingerprint(image);
    header.contentGeneration = header.publishedBusFingerprint;
    return image;
}

inline CanonicalBusTriple NeutralMasterSnapshotTriple() {
    return SnapshotBusTriple(NeutralMasterSnapshotImage().busDefinitionSnapshotHeader);
}

inline std::vector<std::uint8_t> NeutralMasterSnapshotPayload(
    CanonicalBusTriple triple = NeutralMasterSnapshotTriple()) {
    auto image = NeutralMasterSnapshotImage();
    image.busDefinitionSnapshotHeader.contentGeneration = triple.busContentGeneration;
    image.busDefinitionSnapshotHeader.publishedBusFingerprint =
        triple.publishedBusFingerprint;
    image.busDefinitionSnapshotHeader.compileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
    constexpr std::uint64_t kHeader = kPackBusDefinitionSnapshotHeaderBytes;
    constexpr std::uint64_t kBusOffset = kHeader;
    constexpr std::uint64_t kDownmixOffset =
        kBusOffset + kPackBusDefinitionBusBytes;
    constexpr std::uint64_t kCoefficientOffset =
        kDownmixOffset + kPackBusDefinitionDownmixBytes;
    auto header = image.busDefinitionSnapshotHeader;
    header.busOffset = kBusOffset;
    header.sendOffset = kDownmixOffset;
    header.effectOffset = kDownmixOffset;
    header.effectParameterOffset = kDownmixOffset;
    header.downmixOffset = kDownmixOffset;
    header.coefficientOffset = kCoefficientOffset;
    header.payloadBytes =
        (kCoefficientOffset - kHeader) +
        (64u * kPackBusDefinitionCoefficientBytes);
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    SerializePackBusDefinitionBus(image.busDefinitionBusesV2.front(),
                                    payload.data() + header.busOffset);
    SerializePackBusDefinitionDownmix(image.busDefinitionDownmixesV2.front(),
                                        payload.data() + header.downmixOffset);
    for (std::uint32_t i = 0; i < 64; ++i) {
        SerializePackBusDefinitionCoefficient(image.busDefinitionCoefficients[i], payload.data() + header.coefficientOffset +
                       i * kPackBusDefinitionCoefficientBytes);
    }
    SerializePackBusDefinitionSnapshotHeader(header, payload.data());
    StampSnapshotChecksum(&payload);
    return payload;
}

} // namespace harken

#endif
