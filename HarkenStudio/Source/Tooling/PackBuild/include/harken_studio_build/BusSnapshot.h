#ifndef HARKEN_STUDIO_BUILD_CANONICAL_BUS_SNAPSHOT_H
#define HARKEN_STUDIO_BUILD_CANONICAL_BUS_SNAPSHOT_H

#include "harken_studio_build/ChannelLayout.h"
#include "harken_studio_build/PackImage.h"
#include <harken_bus_compiler/BusCompiler.h>

#include <cstdint>
#include <vector>

namespace harkenstudiobuild {

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
    header.mixLayout = harkenstudio::kChannelLayoutStereo;
    header.deviceLayout = harkenstudio::kChannelLayoutStereo;
    header.runtimeCapabilityMask = busfingerprint::kRequiredBusCapabilityMaskV1;
    header.compileTargetProfileFingerprint = busfingerprint::ProfileFingerprintV1(
        header.profileVintage, header.renderSampleRateHz, header.mixLayout,
        header.deviceLayout, header.runtimeCapabilityMask);
    header.busCount = 1;
    header.downmixCount = 1;
    header.coefficientCount = 64;
    PackBusDefinitionBusV1 bus{};
    bus.busId = CanonicalSnapshotPackedBusIdV1(harkenstudio::kMasterBusObjectId);
    bus.faderAfterSlot = busfingerprint::kNeutralFaderAfterSlotV1;
    bus.outputLayout = harkenstudio::kChannelLayoutStereo;
    bus.downmixCount = 1;
    bus.downmixFirst = 0;
    image.busDefinitionBuses.push_back(bus);
    PackBusDefinitionDownmixV1 downmix{};
    downmix.busId = CanonicalSnapshotPackedBusIdV1(harkenstudio::kMasterBusObjectId);
    downmix.sourceLayout = harkenstudio::kChannelLayoutStereo;
    downmix.destinationLayout = harkenstudio::kChannelLayoutStereo;
    downmix.coefficientFirst = 0;
    downmix.coefficientCount = 64;
    image.busDefinitionDownmixes.push_back(downmix);
    image.busDefinitionCoefficients.assign(64, 0.0f);
    DefaultDownmixMatrix(harkenstudio::kChannelLayoutStereo, harkenstudio::kChannelLayoutStereo,
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
    header.schemaVersion = 2;
    header.storageModelVersion = 2;
    header.headerBytes = kPackBusDefinitionSnapshotV2HeaderBytes;
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
    header.busRowBytes = kPackBusDefinitionBusV2RowBytes;
    header.sendRowBytes = kPackBusDefinitionSendV2RowBytes;
    header.downmixRowBytes = kPackBusDefinitionDownmixV2RowBytes;
    header.coefficientRowBytes = kPackBusDefinitionCoefficientRowBytes;
    for (const auto& bus : projection.buses) {
        image.busDefinitionBusesV2.push_back({bus.busId, bus.parentId,
            bus.feedsFirstOrdinal, bus.faderDb, bus.makeUpGainDb,
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
    header.busOffset = header.headerBytes;
    header.sendOffset = header.busOffset + header.busCount * header.busRowBytes;
    header.effectOffset = header.sendOffset + header.sendCount * header.sendRowBytes;
    header.effectParameterOffset = header.effectOffset;
    header.downmixOffset = header.effectOffset;
    header.coefficientOffset = header.downmixOffset +
        header.downmixCount * header.downmixRowBytes;
    header.payloadBytes = header.coefficientOffset +
        header.coefficientCount * header.coefficientRowBytes - header.headerBytes;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(header.headerBytes + header.payloadBytes), 0);
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        SerializePackBusDefinitionBusV2(image.busDefinitionBusesV2[i],
            payload.data() + header.busOffset + i * header.busRowBytes);
    }
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        SerializePackBusDefinitionSendV2(image.busDefinitionSendsV2[i],
            payload.data() + header.sendOffset + i * header.sendRowBytes);
    }
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        SerializePackBusDefinitionDownmixV2(image.busDefinitionDownmixesV2[i],
            payload.data() + header.downmixOffset + i * header.downmixRowBytes);
    }
    for (std::uint32_t i = 0; i < header.coefficientCount; ++i) {
        StoreLEF32(payload.data() + header.coefficientOffset +
                       i * header.coefficientRowBytes,
                   image.busDefinitionCoefficients[i]);
    }
    SerializePackBusDefinitionSnapshotHeaderV2(header, payload.data());
    StampSnapshotChecksum(&payload);
    return payload;
}

} // namespace harkenstudiobuild

#endif
