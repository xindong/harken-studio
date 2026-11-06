#ifndef HARKEN_STUDIO_BUILD_CANONICAL_BUS_SNAPSHOT_H
#define HARKEN_STUDIO_BUILD_CANONICAL_BUS_SNAPSHOT_H

#include "harken_studio_build/ChannelLayout.h"
#include "harken_studio_build/PackImage.h"
#include <harken_bus_compiler/BusCompiler.h>

#include <cstdint>
#include <vector>

namespace harkenstudiobuild {

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
    header.schemaVersion = kPackBusDefinitionSnapshotVintage;
    header.storageModelVersion = kPackBusDefinitionSnapshotVintage;
    header.headerBytes = kPackBusDefinitionSnapshotHeaderBytes;
    header.busRowBytes = kPackBusDefinitionBusBytes;
    header.sendRowBytes = kPackBusDefinitionSendBytes;
    header.downmixRowBytes = kPackBusDefinitionDownmixBytes;
    header.coefficientRowBytes = kPackBusDefinitionCoefficientBytes;
    header.publicationCapabilityVintage = 3;
    header.contentGeneration = kNeutralMasterBusContentGeneration;
    header.profileVintage = 1;
    header.renderSampleRateHz = 48000;
    header.mixLayout = harkenstudio::kChannelLayoutStereo;
    header.deviceLayout = harkenstudio::kChannelLayoutStereo;
    header.runtimeCapabilityMask = busfingerprint::kRequiredBusCapabilityMaskV1;
    header.compileTargetProfileFingerprint = busfingerprint::ProfileFingerprintV1(
        header.profileVintage, header.renderSampleRateHz, header.mixLayout,
        header.deviceLayout, header.runtimeCapabilityMask);
    header.busCount = 1;
    header.downmixCount = 1;
    header.coefficientCount = 64;
    PackBusDefinitionBus bus{};
    bus.busId = harkenstudio::kMasterBusObjectId;
    bus.faderAfterSlot = busfingerprint::kNeutralFaderAfterSlotV1;
    bus.outputLayout = harkenstudio::kChannelLayoutStereo;
    bus.downmixCount = 1;
    bus.downmixFirst = 0;
    image.busDefinitionBusesV2.push_back(bus);
    PackBusDefinitionDownmix downmix{};
    downmix.busId = harkenstudio::kMasterBusObjectId;
    downmix.sourceLayout = harkenstudio::kChannelLayoutStereo;
    downmix.destinationLayout = harkenstudio::kChannelLayoutStereo;
    downmix.coefficientFirst = 0;
    downmix.coefficientCount = 64;
    image.busDefinitionDownmixesV2.push_back(downmix);
    image.busDefinitionCoefficients.assign(64, 0.0f);
    DefaultDownmixMatrix(harkenstudio::kChannelLayoutStereo, harkenstudio::kChannelLayoutStereo,
                         image.busDefinitionCoefficients.data());
    header.publishedBusFingerprint = CanonicalVolumeBusFingerprint(image);
    header.contentGeneration = header.publishedBusFingerprint;
    return image;
}

inline CanonicalBusTriple NeutralMasterSnapshotTriple() {
    return SnapshotBusTriple(NeutralMasterSnapshotImage().busDefinitionSnapshotHeader);
}

struct PackBusDefinitionSnapshotWriter {
    const PackCanonicalImage& image;
    const auto& Value() noexcept { return image.busDefinitionSnapshotHeader; }
    const auto& Header() noexcept { return image.busDefinitionSnapshotHeader; }
    bool PrepareBuses(std::uint64_t count) noexcept { return count == image.busDefinitionBusesV2.size(); }
    const auto& Buses(std::uint64_t i) noexcept { return image.busDefinitionBusesV2[static_cast<std::size_t>(i)]; }
    bool PrepareSends(std::uint64_t count) noexcept { return count == image.busDefinitionSendsV2.size(); }
    const auto& Sends(std::uint64_t i) noexcept { return image.busDefinitionSendsV2[static_cast<std::size_t>(i)]; }
    bool PrepareDownmixes(std::uint64_t count) noexcept { return count == image.busDefinitionDownmixesV2.size(); }
    const auto& Downmixes(std::uint64_t i) noexcept { return image.busDefinitionDownmixesV2[static_cast<std::size_t>(i)]; }
    bool PrepareCoefficients(std::uint64_t count) noexcept { return count == image.busDefinitionCoefficients.size(); }
    const auto& Coefficients(std::uint64_t i) noexcept { return image.busDefinitionCoefficients[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { return true; }
};

inline std::vector<std::uint8_t> EncodeBusDefinitionSnapshotPayload(PackCanonicalImage& image) {
    std::uint64_t size = 0;
    if (!LayoutPackBusDefinitionSnapshot(image.busDefinitionSnapshotHeader, size)) return {};
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(size), 0);
    PackWireBuffer<std::uint8_t> output{payload.data(), payload.size()};
    PackBusDefinitionSnapshotWriter visitor{image};
    if (!AppendPackBusDefinitionSnapshot(output, visitor) || output.offset != payload.size()) return {};
    StampSnapshotChecksum(&payload);
    return payload;
}

inline std::vector<std::uint8_t> NeutralMasterSnapshotPayload(
    CanonicalBusTriple triple = NeutralMasterSnapshotTriple()) {
    auto image = NeutralMasterSnapshotImage();
    image.busDefinitionSnapshotHeader.contentGeneration = triple.busContentGeneration;
    image.busDefinitionSnapshotHeader.publishedBusFingerprint =
        triple.publishedBusFingerprint;
    image.busDefinitionSnapshotHeader.compileTargetProfileFingerprint =
        triple.compileTargetProfileFingerprint;
    return EncodeBusDefinitionSnapshotPayload(image);
}

inline std::vector<std::uint8_t> VolumeBusSnapshotPayload(
    const harkenbuscompiler::PublishedBusProjectionV1& projection) {
    using namespace harkenbuscompiler;
    if (projection.capabilityVintage !=
            BusPublicationCapabilityVintageV1::TopologyAndVolumeNeutralOtherProperties &&
         projection.capabilityVintage !=
            BusPublicationCapabilityVintageV1::TopologyVolumeAndSendNeutralOtherProperties ||
        !projection.compileTargetProfile || projection.fingerprint == 0 ||
        projection.buses.empty() || projection.effects.size() != 0 ||
        projection.effectParameters.size() != 0 ||
        projection.downmix.size() != projection.buses.size() ||
        projection.downmixCoefficients.size() != projection.buses.size() * 64) {
        return {};
    }
    PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = kPackBusDefinitionSnapshotVintage;
    header.storageModelVersion = kPackBusDefinitionSnapshotVintage;
    header.headerBytes = kPackBusDefinitionSnapshotHeaderBytes;
    header.publicationCapabilityVintage =
        static_cast<std::uint32_t>(projection.capabilityVintage);
    header.contentGeneration = projection.fingerprint;
    header.publishedBusFingerprint = projection.fingerprint;
    header.profileVintage = projection.compileTargetProfile->vintage;
    header.renderSampleRateHz = projection.compileTargetProfile->sampleRate;
    header.mixLayout = projection.compileTargetProfile->mixLayout;
    header.deviceLayout = projection.compileTargetProfile->deviceLayout;
    header.runtimeCapabilityMask = projection.compileTargetProfile->capabilityMask;
    header.compileTargetProfileFingerprint = projection.compileTargetProfileFingerprint;
    header.busCount = static_cast<std::uint32_t>(projection.buses.size());
    header.sendCount = static_cast<std::uint32_t>(projection.sends.size());
    header.downmixCount = static_cast<std::uint32_t>(projection.downmix.size());
    header.coefficientCount = static_cast<std::uint32_t>(projection.downmixCoefficients.size());
    header.busRowBytes = kPackBusDefinitionBusBytes;
    header.sendRowBytes = kPackBusDefinitionSendBytes;
    header.downmixRowBytes = kPackBusDefinitionDownmixBytes;
    header.coefficientRowBytes = kPackBusDefinitionCoefficientBytes;
    for (const auto& bus : projection.buses) {
        image.busDefinitionBusesV2.push_back({bus.busId, bus.parentId,
            bus.feedsFirstOrdinal, bus.faderDb,
            bus.outputBusVolumeDb, bus.faderAfterSlot, bus.outputLayout,
            bus.effectFirst, bus.effectCount, bus.downmixFirst, bus.downmixCount});
    }
    for (const auto& send : projection.sends) {
        image.busDefinitionSendsV2.push_back({send.sourceBusId,
            send.targetBusId, send.sendLevelDb});
    }
    for (const auto& row : projection.downmix) {
        image.busDefinitionDownmixesV2.push_back({row.busId, row.sourceLayout,
            row.destinationLayout, row.coefficientFirst, row.coefficientCount});
    }
    image.busDefinitionCoefficients = projection.downmixCoefficients;
    if (CanonicalVolumeBusFingerprint(image) != projection.fingerprint) return {};
    return EncodeBusDefinitionSnapshotPayload(image);
}

} // namespace harkenstudiobuild

#endif
