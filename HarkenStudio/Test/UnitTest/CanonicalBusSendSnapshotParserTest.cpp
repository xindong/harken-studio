#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_studio_build/PackImage.h"

#include <harken/Harken.h>
#include "harken_studio_build/BusSnapshot.h"
#include "harken_studio_build/PackFormatV2.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

std::uint64_t AlignPackPayload(std::uint64_t value) {
    return (value + harkenstudiobuild::kPackPayloadAlign - 1) /
        harkenstudiobuild::kPackPayloadAlign * harkenstudiobuild::kPackPayloadAlign;
}

std::vector<std::uint8_t> MakeBusSendSnapshotPack(
    std::uint32_t capability, bool destinationIsChildOfSource,
    std::initializer_list<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> sends) {
    const harkenstudio::ObjectId source = harkentest::StudioObjectId(801);
    const harkenstudio::ObjectId destination = harkentest::StudioObjectId(802);
    harkenstudiobuild::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& snapshot = image.busDefinitionSnapshotHeader;
    snapshot.schemaVersion = 2;
    snapshot.storageModelVersion = 2;
    snapshot.headerBytes = harkenstudiobuild::kPackBusDefinitionSnapshotHeaderBytes;
    snapshot.publicationCapabilityVintage = capability;
    snapshot.profileVintage = 1;
    snapshot.renderSampleRateHz = 48000;
    snapshot.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    snapshot.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    snapshot.runtimeCapabilityMask =
        harkenstudiobuild::busfingerprint::kRequiredBusCapabilityMaskV1;
    snapshot.compileTargetProfileFingerprint =
        harkenstudiobuild::busfingerprint::ProfileFingerprintV1(
            snapshot.profileVintage, snapshot.renderSampleRateHz,
            snapshot.mixLayout, snapshot.deviceLayout,
            snapshot.runtimeCapabilityMask);
    const harkenstudio::ObjectId ids[] = {harkenstudio::kMasterBusObjectId, source, destination};
    const harkenstudio::ObjectId parents[] = {harkenstudio::ObjectId{}, harkenstudio::kMasterBusObjectId,
        destinationIsChildOfSource ? source : harkenstudio::kMasterBusObjectId};
    for (std::uint32_t i = 0; i < 3; ++i) {
        image.busDefinitionBusesV2.push_back({ids[i], parents[i], i,
            0.0f, 0.0f,
            harkenstudiobuild::busfingerprint::kNeutralFaderAfterSlotV1,
            VN_CHANNEL_LAYOUT_STEREO, 0, 0, i, 1});
        image.busDefinitionDownmixesV2.push_back({ids[i],
            VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, i * 64, 64});
        for (std::uint32_t coefficient = 0; coefficient < 64; ++coefficient) {
            image.busDefinitionCoefficients.push_back(
                coefficient == 0 || coefficient == 9 ? 1.0f : 0.0f);
        }
    }
    for (const auto& [from, to] : sends) {
        image.busDefinitionSendsV2.push_back({from, to, 0.0f});
    }
    snapshot.busCount = 3;
    snapshot.sendCount = static_cast<std::uint32_t>(sends.size());
    snapshot.downmixCount = 3;
    snapshot.coefficientCount = 192;
    snapshot.busRowBytes = harkenstudiobuild::kPackBusDefinitionBusBytes;
    snapshot.sendRowBytes = harkenstudiobuild::kPackBusDefinitionSendBytes;
    snapshot.downmixRowBytes = harkenstudiobuild::kPackBusDefinitionDownmixBytes;
    snapshot.coefficientRowBytes = harkenstudiobuild::kPackBusDefinitionCoefficientBytes;
    snapshot.contentGeneration = harkenstudiobuild::CanonicalVolumeBusFingerprint(image);
    snapshot.publishedBusFingerprint = snapshot.contentGeneration;
    snapshot.busOffset = snapshot.headerBytes;
    snapshot.sendOffset = snapshot.busOffset + snapshot.busCount * snapshot.busRowBytes;
    snapshot.effectOffset = snapshot.sendOffset + snapshot.sendCount * snapshot.sendRowBytes;
    snapshot.effectParameterOffset = snapshot.effectOffset;
    snapshot.downmixOffset = snapshot.effectOffset;
    snapshot.coefficientOffset = snapshot.downmixOffset +
        snapshot.downmixCount * snapshot.downmixRowBytes;
    snapshot.payloadBytes = snapshot.coefficientOffset +
        snapshot.coefficientCount * snapshot.coefficientRowBytes - snapshot.headerBytes;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(snapshot.headerBytes + snapshot.payloadBytes), 0);
    for (std::uint32_t i = 0; i < snapshot.busCount; ++i) {
        harkenstudiobuild::SerializePackBusDefinitionBus(image.busDefinitionBusesV2[i],
            payload.data() + snapshot.busOffset + i * snapshot.busRowBytes);
        harkenstudiobuild::SerializePackBusDefinitionDownmix(image.busDefinitionDownmixesV2[i],
            payload.data() + snapshot.downmixOffset + i * snapshot.downmixRowBytes);
    }
    for (std::uint32_t i = 0; i < snapshot.sendCount; ++i) {
        harkenstudiobuild::SerializePackBusDefinitionSend(image.busDefinitionSendsV2[i],
            payload.data() + snapshot.sendOffset + i * snapshot.sendRowBytes);
    }
    for (std::uint32_t i = 0; i < snapshot.coefficientCount; ++i) {
        harkenstudiobuild::StoreLEF32(payload.data() + snapshot.coefficientOffset +
            i * snapshot.coefficientRowBytes, image.busDefinitionCoefficients[i]);
    }
    harkenstudiobuild::SerializePackBusDefinitionSnapshotHeader(snapshot, payload.data());
    snapshot.checksum = harkenstudiobuild::Fnv1a64(payload.data(), payload.size());
    harkenstudiobuild::SerializePackBusDefinitionSnapshotHeader(snapshot, payload.data());

    harkenstudiobuild::PackRequirementBlock requirement;
    requirement.schemaVersion = 2;
    requirement.storageModelVersion = 2;
    requirement.packStableId = 801;
    requirement.packContentRevision = 1;
    requirement.busContentGeneration = snapshot.contentGeneration;
    requirement.publishedBusFingerprint = snapshot.publishedBusFingerprint;
    requirement.compileTargetProfileFingerprint =
        snapshot.compileTargetProfileFingerprint;
    std::vector<std::uint8_t> requirementPayload(
        harkenstudiobuild::kPackRequirementBlockBytes, 0);
    harkenstudiobuild::SerializePackRequirementBlock(requirement, requirementPayload.data());
    requirement.payloadChecksumCoverage =
        harkenstudiobuild::Fnv1a64(requirementPayload.data(), 104);
    harkenstudiobuild::SerializePackRequirementBlock(requirement, requirementPayload.data());

    constexpr std::uint32_t entryCount = 2;
    const std::uint64_t directoryOffset = harkenstudiobuild::kPackFileHeaderBytes;
    const std::uint64_t requirementOffset = AlignPackPayload(directoryOffset +
        entryCount * harkenstudiobuild::kPackDirectoryEntryBytes);
    const std::uint64_t snapshotOffset = AlignPackPayload(
        requirementOffset + requirementPayload.size());
    std::vector<std::uint8_t> bytes(snapshotOffset + payload.size(), 0);
    std::memcpy(bytes.data() + requirementOffset, requirementPayload.data(),
                requirementPayload.size());
    std::memcpy(bytes.data() + snapshotOffset, payload.data(), payload.size());
    harkenstudiobuild::PackDirectoryEntry requirementEntry;
    requirementEntry.kind = harkenstudiobuild::kPackEntryKindPackRequirementBlock;
    requirementEntry.vintage = harkenstudiobuild::kPackPackRequirementBlockVintage;
    requirementEntry.offset = requirementOffset;
    requirementEntry.size = requirementPayload.size();
    requirementEntry.payloadChecksum = harkenstudiobuild::Fnv1a64(
        requirementPayload.data(), requirementPayload.size());
    harkenstudiobuild::SerializePackDirectoryEntry(requirementEntry,
        bytes.data() + directoryOffset);
    harkenstudiobuild::PackDirectoryEntry snapshotEntry;
    snapshotEntry.kind = harkenstudiobuild::kPackEntryKindBusDefinitionSnapshot;
    snapshotEntry.vintage = harkenstudiobuild::kPackBusDefinitionSnapshotVintage;
    snapshotEntry.relationOrdinal = 1;
    snapshotEntry.offset = snapshotOffset;
    snapshotEntry.size = payload.size();
    snapshotEntry.payloadChecksum = harkenstudiobuild::Fnv1a64(payload.data(), payload.size());
    harkenstudiobuild::SerializePackDirectoryEntry(snapshotEntry,
        bytes.data() + directoryOffset + harkenstudiobuild::kPackDirectoryEntryBytes);
    harkenstudiobuild::PackFileHeader packHeader;
    packHeader.formatVersion = harkenstudiobuild::kPackFormatVersion;
    packHeader.headerBytes = harkenstudiobuild::kPackFileHeaderBytes;
    packHeader.fileBytes = bytes.size();
    packHeader.packKind = harkenstudiobuild::kPackKindInit;
    packHeader.entryCount = entryCount;
    packHeader.directoryOffset = directoryOffset;
    packHeader.directoryChecksum = harkenstudiobuild::Fnv1a64(
        bytes.data() + directoryOffset,
        entryCount * harkenstudiobuild::kPackDirectoryEntryBytes);
    harkenstudiobuild::SerializePackFileHeader(packHeader, bytes.data());
    return bytes;
}

void ExpectParser(const std::vector<std::uint8_t>& bytes, bool accepted) {
    harkenstudiobuild::PackCanonicalImage image;
    EXPECT_EQ(accepted ? harkenstudiobuild::kPackOk : harkenstudiobuild::kPackInvalid,
        harkenstudiobuild::ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(CanonicalBusSendSnapshotParser, CapabilityFourRejectsCombinedGraphCycles) {
    const harkenstudio::ObjectId source = harkentest::StudioObjectId(801);
    const harkenstudio::ObjectId destination = harkentest::StudioObjectId(802);
    ExpectParser(MakeBusSendSnapshotPack(4, false,
        {{source, destination}}), true);
    ExpectParser(MakeBusSendSnapshotPack(4, false,
        {{source, destination}, {destination, source}}), false);
    ExpectParser(MakeBusSendSnapshotPack(4, true,
        {{source, destination}}), false);
}

} // namespace
