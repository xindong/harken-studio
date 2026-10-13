// Kind-24 shipping writer: one stereo Master snapshot, packed bus ids.
// 10a does not project authored UUID buses (that is U3-10b).
#ifndef HARKEN_MEDIA_CANONICAL_BUS_SNAPSHOT_H
#define HARKEN_MEDIA_CANONICAL_BUS_SNAPSHOT_H

#include "harken/core/ChannelLayout.h"
#include "harken/media/PackLogicImage.h"

#include <cstdint>
#include <vector>

namespace harken {

inline void StampRequirementCoverage(std::vector<std::uint8_t>* payload) {
    StoreLE64(payload->data() + 104, Fnv1a64(payload->data(), 104));
}

inline void StampSnapshotChecksum(std::vector<std::uint8_t>* payload) {
    StoreLE64(payload->data() + 112, 0);
    const std::uint8_t zeros[8] = {};
    std::uint64_t hash = Fnv1a64(payload->data(), 112);
    hash = Fnv1a64(zeros, 8, hash);
    hash = Fnv1a64(payload->data() + 120, payload->size() - 120, hash);
    StoreLE64(payload->data() + 112, hash);
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

// Topology is packed VN_BUS_MASTER + stereo identity. contentGeneration stays
// kNeutralMasterBusContentGeneration. publishedBusFingerprint is
// CanonicalNeutralBusFingerprint of this image (Activate requires that), not
// the unused kNeutralMasterPublishedBusFingerprint placeholder.
inline PackCanonicalImage NeutralMasterSnapshotImage() {
    PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 1;
    header.storageModelVersion = 1;
    header.headerBytes = kPackBusDefinitionSnapshotHeaderBytes;
    header.publicationCapabilityVintage = 1;
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
    PackBusDefinitionBusV1 bus{};
    bus.busId = CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    bus.faderAfterSlot = busfingerprint::kNeutralFaderAfterSlotV1;
    bus.outputLayout = VN_CHANNEL_LAYOUT_STEREO;
    bus.downmixCount = 1;
    bus.downmixFirst = 0;
    image.busDefinitionBuses.push_back(bus);
    PackBusDefinitionDownmixV1 downmix{};
    downmix.busId = CanonicalSnapshotPackedBusIdV1(VN_BUS_MASTER);
    downmix.sourceLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmix.destinationLayout = VN_CHANNEL_LAYOUT_STEREO;
    downmix.coefficientFirst = 0;
    downmix.coefficientCount = 64;
    image.busDefinitionDownmixes.push_back(downmix);
    image.busDefinitionCoefficients.assign(64, 0.0f);
    DefaultDownmixMatrix(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                         image.busDefinitionCoefficients.data());
    header.publishedBusFingerprint = CanonicalNeutralBusFingerprint(image);
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
        kBusOffset + kPackBusDefinitionBusRowBytes;
    constexpr std::uint64_t kCoefficientOffset =
        kDownmixOffset + kPackBusDefinitionDownmixRowBytes;
    auto header = image.busDefinitionSnapshotHeader;
    header.busOffset = kBusOffset;
    header.sendOffset = kDownmixOffset;
    header.effectOffset = kDownmixOffset;
    header.effectParameterOffset = kDownmixOffset;
    header.downmixOffset = kDownmixOffset;
    header.coefficientOffset = kCoefficientOffset;
    header.payloadBytes =
        (kCoefficientOffset - kHeader) +
        (64u * kPackBusDefinitionCoefficientRowBytes);
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    SerializePackBusDefinitionBusV1(image.busDefinitionBuses.front(),
                                    payload.data() + header.busOffset);
    SerializePackBusDefinitionDownmixV1(image.busDefinitionDownmixes.front(),
                                        payload.data() + header.downmixOffset);
    for (std::uint32_t i = 0; i < 64; ++i) {
        StoreLEF32(payload.data() + header.coefficientOffset +
                       i * kPackBusDefinitionCoefficientRowBytes,
                   image.busDefinitionCoefficients[i]);
    }
    SerializePackBusDefinitionSnapshotHeaderV1(header, payload.data());
    StampSnapshotChecksum(&payload);
    return payload;
}

} // namespace harken

#endif
