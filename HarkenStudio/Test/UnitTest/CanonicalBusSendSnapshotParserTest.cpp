#include "UnitTest/ObjectIdTestSupport.h"
#include "harken_studio_build/PackImage.h"

#include <harken/Harken.h>
#include <harken/media/CanonicalBusSnapshot.h>
#include <harken/media/PackFormatV2.h>
#include <harken/media/PackLogicImage.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

std::uint64_t AlignPackPayload(std::uint64_t value) {
    return (value + harken::kPackPayloadAlign - 1) /
        harken::kPackPayloadAlign * harken::kPackPayloadAlign;
}

std::vector<std::uint8_t> MakeBusSendSnapshotPack(
    std::uint32_t capability, bool destinationIsChildOfSource,
    std::initializer_list<std::pair<VnObjectId, VnObjectId>> sends,
    const std::function<void(harken::PackCanonicalImage&)>& edit = {}) {
    const VnObjectId source = harkentest::ObjectId(801);
    const VnObjectId destination = harkentest::ObjectId(802);
    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& snapshot = image.busDefinitionSnapshotHeader;
    snapshot.schemaVersion = 2;
    snapshot.storageModelVersion = 2;
    snapshot.headerBytes = harken::kPackBusDefinitionSnapshotHeaderBytes;
    snapshot.publicationCapabilityVintage = capability;
    snapshot.profileVintage = 1;
    snapshot.renderSampleRateHz = 48000;
    snapshot.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    snapshot.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    snapshot.runtimeCapabilityMask =
        harken::busfingerprint::kRequiredBusCapabilityMaskV1;
    snapshot.compileTargetProfileFingerprint =
        harken::busfingerprint::ProfileFingerprintV1(
            snapshot.profileVintage, snapshot.renderSampleRateHz,
            snapshot.mixLayout, snapshot.deviceLayout,
            snapshot.runtimeCapabilityMask);
    const VnObjectId ids[] = {VN_BUS_MASTER, source, destination};
    const VnObjectId parents[] = {VnObjectId{}, VN_BUS_MASTER,
        destinationIsChildOfSource ? source : VN_BUS_MASTER};
    for (std::uint32_t i = 0; i < 3; ++i) {
        image.busDefinitionBusesV2.push_back({ids[i], parents[i], i,
            0.0f, 0.0f, 0.0f,
            harken::busfingerprint::kNeutralFaderAfterSlotV1,
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
    snapshot.busRowBytes = harken::kPackBusDefinitionBusBytes;
    snapshot.sendRowBytes = harken::kPackBusDefinitionSendBytes;
    snapshot.downmixRowBytes = harken::kPackBusDefinitionDownmixBytes;
    snapshot.coefficientRowBytes = harken::kPackBusDefinitionCoefficientBytes;
    if (edit) edit(image);
    snapshot.contentGeneration = harken::CanonicalVolumeBusFingerprint(image,
        image.busDefinitionEffects, image.busDefinitionEffectParameters);
    snapshot.publishedBusFingerprint = snapshot.contentGeneration;
    snapshot.busOffset = snapshot.headerBytes;
    snapshot.sendOffset = snapshot.busOffset + snapshot.busCount * snapshot.busRowBytes;
    snapshot.effectOffset = snapshot.sendOffset + snapshot.sendCount * snapshot.sendRowBytes;
    snapshot.effectParameterOffset = snapshot.effectOffset +
        snapshot.effectCount * harken::kPackBusDefinitionEffectBytes;
    snapshot.downmixOffset = snapshot.effectParameterOffset +
        snapshot.effectParameterCount * harken::kPackBusDefinitionEffectParameterBytes;
    snapshot.coefficientOffset = snapshot.downmixOffset +
        snapshot.downmixCount * snapshot.downmixRowBytes;
    snapshot.payloadBytes = snapshot.coefficientOffset +
        snapshot.coefficientCount * snapshot.coefficientRowBytes - snapshot.headerBytes;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(snapshot.headerBytes + snapshot.payloadBytes), 0);
    for (std::uint32_t i = 0; i < snapshot.busCount; ++i) {
        harken::SerializePackBusDefinitionBus(image.busDefinitionBusesV2[i],
            payload.data() + snapshot.busOffset + i * snapshot.busRowBytes);
        harken::SerializePackBusDefinitionDownmix(image.busDefinitionDownmixesV2[i],
            payload.data() + snapshot.downmixOffset + i * snapshot.downmixRowBytes);
    }
    for (std::uint32_t i = 0; i < snapshot.sendCount; ++i) {
        harken::SerializePackBusDefinitionSend(image.busDefinitionSendsV2[i],
            payload.data() + snapshot.sendOffset + i * snapshot.sendRowBytes);
    }
    for (std::uint32_t i = 0; i < snapshot.coefficientCount; ++i) {
        harken::StoreLEF32(payload.data() + snapshot.coefficientOffset +
            i * snapshot.coefficientRowBytes, image.busDefinitionCoefficients[i]);
    }
    for (std::uint32_t i = 0; i < snapshot.effectCount; ++i)
        harken::SerializePackBusDefinitionEffect(image.busDefinitionEffects[i],
            payload.data() + snapshot.effectOffset + i * harken::kPackBusDefinitionEffectBytes);
    for (std::uint32_t i = 0; i < snapshot.effectParameterCount; ++i)
        harken::SerializePackBusDefinitionEffectParameter(image.busDefinitionEffectParameters[i],
            payload.data() + snapshot.effectParameterOffset + i * harken::kPackBusDefinitionEffectParameterBytes);
    harken::SerializePackBusDefinitionSnapshotHeader(snapshot, payload.data());
    snapshot.checksum = harken::Fnv1a64(payload.data(), payload.size());
    harken::SerializePackBusDefinitionSnapshotHeader(snapshot, payload.data());

    harken::PackRequirementBlock requirement;
    requirement.schemaVersion = 2;
    requirement.storageModelVersion = 2;
    requirement.packStableId = 801;
    requirement.packContentRevision = 1;
    requirement.busContentGeneration = snapshot.contentGeneration;
    requirement.publishedBusFingerprint = snapshot.publishedBusFingerprint;
    requirement.compileTargetProfileFingerprint =
        snapshot.compileTargetProfileFingerprint;
    std::vector<std::uint8_t> requirementPayload(
        harken::kPackRequirementBlockBytes, 0);
    harken::SerializePackRequirementBlock(requirement, requirementPayload.data());
    requirement.payloadChecksumCoverage =
        harken::Fnv1a64(requirementPayload.data(), 104);
    harken::SerializePackRequirementBlock(requirement, requirementPayload.data());

    constexpr std::uint32_t entryCount = 2;
    const std::uint64_t directoryOffset = harken::kPackFileHeaderBytes;
    const std::uint64_t requirementOffset = AlignPackPayload(directoryOffset +
        entryCount * harken::kPackDirectoryEntryBytes);
    const std::uint64_t snapshotOffset = AlignPackPayload(
        requirementOffset + requirementPayload.size());
    std::vector<std::uint8_t> bytes(snapshotOffset + payload.size(), 0);
    std::memcpy(bytes.data() + requirementOffset, requirementPayload.data(),
                requirementPayload.size());
    std::memcpy(bytes.data() + snapshotOffset, payload.data(), payload.size());
    harken::PackDirectoryEntry requirementEntry;
    requirementEntry.kind = harken::kPackEntryKindPackRequirementBlock;
    requirementEntry.vintage = harken::kPackPackRequirementBlockVintage;
    requirementEntry.offset = requirementOffset;
    requirementEntry.size = requirementPayload.size();
    requirementEntry.payloadChecksum = harken::Fnv1a64(
        requirementPayload.data(), requirementPayload.size());
    harken::SerializePackDirectoryEntry(requirementEntry,
        bytes.data() + directoryOffset);
    harken::PackDirectoryEntry snapshotEntry;
    snapshotEntry.kind = harken::kPackEntryKindBusDefinitionSnapshot;
    snapshotEntry.vintage = harken::kPackBusDefinitionSnapshotVintage;
    snapshotEntry.relationOrdinal = 1;
    snapshotEntry.offset = snapshotOffset;
    snapshotEntry.size = payload.size();
    snapshotEntry.payloadChecksum = harken::Fnv1a64(payload.data(), payload.size());
    harken::SerializePackDirectoryEntry(snapshotEntry,
        bytes.data() + directoryOffset + harken::kPackDirectoryEntryBytes);
    harken::PackFileHeader packHeader;
    packHeader.formatVersion = harken::kPackFormatVersion;
    packHeader.headerBytes = harken::kPackFileHeaderBytes;
    packHeader.fileBytes = bytes.size();
    packHeader.packKind = harken::kPackKindInit;
    packHeader.entryCount = entryCount;
    packHeader.directoryOffset = directoryOffset;
    packHeader.directoryChecksum = harken::Fnv1a64(
        bytes.data() + directoryOffset,
        entryCount * harken::kPackDirectoryEntryBytes);
    harken::SerializePackFileHeader(packHeader, bytes.data());
    return bytes;
}

void ExpectBothParsers(const std::vector<std::uint8_t>& bytes, bool accepted) {
    harken::PackCanonicalImage runtime;
    harkenstudiobuild::PackCanonicalImage studio;
    EXPECT_EQ(accepted ? VN_OK : VN_BAD_PACK,
        harken::ParsePackCanonicalImage(bytes.data(), bytes.size(), &runtime));
    EXPECT_EQ(accepted ? harkenstudiobuild::kPackOk : harkenstudiobuild::kPackInvalid,
        harkenstudiobuild::ParsePackCanonicalImage(bytes.data(), bytes.size(), &studio));
}

TEST(CanonicalBusSendSnapshotParser, CapabilityFourRejectsCombinedGraphCycles) {
    const VnObjectId source = harkentest::ObjectId(801);
    const VnObjectId destination = harkentest::ObjectId(802);
    ExpectBothParsers(MakeBusSendSnapshotPack(4, false,
        {{source, destination}}), true);
    ExpectBothParsers(MakeBusSendSnapshotPack(4, false,
        {{source, destination}, {destination, source}}), false);
    ExpectBothParsers(MakeBusSendSnapshotPack(4, true,
        {{source, destination}}), false);
}

TEST(CanonicalBusSendSnapshotParser, EffectPublicationSharesIdentitySliceAndValueValidation) {
    const auto withEffects = [](harken::PackCanonicalImage& image) {
        auto& header = image.busDefinitionSnapshotHeader;
        header.effectCount = 2;
        header.effectParameterCount = 4;
        header.runtimeCapabilityMask |= (UINT64_C(1) << 19) |
            (UINT64_C(1) << (32 + VN_EFFECT_LIMITER));
        header.compileTargetProfileFingerprint = harken::busfingerprint::ProfileFingerprintV1(
            header.profileVintage, header.renderSampleRateHz, header.mixLayout,
            header.deviceLayout, header.runtimeCapabilityMask);
        image.busDefinitionBusesV2[1].effectCount = 2;
        image.busDefinitionBusesV2[2].effectFirst = 2;
        image.busDefinitionEffects = {
            {harkentest::ObjectId(801), harkentest::ObjectId(901), 0, VN_EFFECT_LIMITER, 2, 0, 0, 2},
            {harkentest::ObjectId(801), harkentest::ObjectId(902), 1, VN_EFFECT_LIMITER, 2, 1, 2, 2}};
        image.busDefinitionEffectParameters = {{0, -6.0f}, {1, 100.0f}, {0, -12.0f}, {1, 50.0f}};
    };
    ExpectBothParsers(MakeBusSendSnapshotPack(5, false, {}, withEffects), true);
    for (const auto bit : {19u, 32u + VN_EFFECT_LIMITER}) {
        ExpectBothParsers(MakeBusSendSnapshotPack(5, false, {}, [&](auto& image) {
            withEffects(image);
            auto& header = image.busDefinitionSnapshotHeader;
            header.runtimeCapabilityMask &= ~(UINT64_C(1) << bit);
            header.compileTargetProfileFingerprint = harken::busfingerprint::ProfileFingerprintV1(
                header.profileVintage, header.renderSampleRateHz, header.mixLayout,
                header.deviceLayout, header.runtimeCapabilityMask);
        }), false);
    }
    for (const auto& corrupt : std::vector<std::function<void(harken::PackCanonicalImage&)>>{
        [](auto& image) { image.busDefinitionEffects[0].flags = 2; },
        [](auto& image) { image.busDefinitionEffects[1].effectId = image.busDefinitionEffects[0].effectId; },
        [](auto& image) { image.busDefinitionEffects[1].slot = 0; },
        [](auto& image) { image.busDefinitionEffects[1].parameterFirst = 1; },
        [](auto& image) { image.busDefinitionEffects[1].parameterCount = 3; },
        [](auto& image) { image.busDefinitionEffectParameters[1].index = 0; },
        [](auto& image) { image.busDefinitionEffectParameters[0].value = 1.0f; },
        [](auto& image) { image.busDefinitionBusesV2[1].faderAfterSlot = 4; },
        [](auto& image) { image.busDefinitionBusesV2[2].effectFirst = 0; },
    }) {
        ExpectBothParsers(MakeBusSendSnapshotPack(5, false, {}, [&](auto& image) {
            withEffects(image);
            corrupt(image);
        }), false);
    }
}

} // namespace
