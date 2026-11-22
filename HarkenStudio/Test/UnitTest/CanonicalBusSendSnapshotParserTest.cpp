#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_studio_build/PackImage.h"

#include <harken/Harken.h>
#include "harken_studio_build/BusSnapshot.h"
#include "harken_studio_build/PackFormatV2.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

std::uint64_t AlignPackPayload(std::uint64_t value) {
    return (value + HK_PACK_PAYLOAD_ALIGN - 1) /
        HK_PACK_PAYLOAD_ALIGN * HK_PACK_PAYLOAD_ALIGN;
}

std::vector<std::uint8_t> MakeBusSendSnapshotPack(
    std::uint32_t capability, bool destinationIsChildOfSource,
    std::initializer_list<std::pair<harken::studio::types::ObjectId, harken::studio::types::ObjectId>> sends,
    const std::function<void(harken::studio::build::PackCanonicalImage&)>& edit = {}) {
    const harken::studio::types::ObjectId source = harkentest::StudioObjectId(801);
    const harken::studio::types::ObjectId destination = harkentest::StudioObjectId(802);
    harken::studio::build::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& snapshot = image.busDefinitionSnapshotHeader;
    snapshot.schemaVersion = 2;
    snapshot.storageModelVersion = 2;
    snapshot.headerBytes = harken::studio::build::kPackBytes<HkPackBusDefinitionSnapshotHeader>;
    snapshot.publicationCapabilityVintage = capability;
    snapshot.profileVintage = 1;
    snapshot.renderSampleRateHz = 48000;
    snapshot.mixLayout = VN_CHANNEL_LAYOUT_STEREO;
    snapshot.deviceLayout = VN_CHANNEL_LAYOUT_STEREO;
    snapshot.runtimeCapabilityMask =
        harken::studio::build::busfingerprint::kRequiredBusCapabilityMaskV1;
    snapshot.compileTargetProfileFingerprint =
        harken::studio::build::busfingerprint::ProfileFingerprintV1(
            snapshot.profileVintage, snapshot.renderSampleRateHz,
            snapshot.mixLayout, snapshot.deviceLayout,
            snapshot.runtimeCapabilityMask);
    const harken::studio::types::ObjectId ids[] = {harken::studio::types::kMasterBusObjectId, source, destination};
    const harken::studio::types::ObjectId parents[] = {harken::studio::types::ObjectId{}, harken::studio::types::kMasterBusObjectId,
        destinationIsChildOfSource ? source : harken::studio::types::kMasterBusObjectId};
    for (std::uint32_t i = 0; i < 3; ++i) {
        image.busDefinitionBusesV2.push_back({ids[i], parents[i], i,
            0.0f, 0.0f,
            harken::studio::build::busfingerprint::kNeutralFaderAfterSlotV1,
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
    snapshot.busRowBytes = harken::studio::build::kPackBytes<HkPackBusDefinitionBus>;
    snapshot.sendRowBytes = harken::studio::build::kPackBytes<HkPackBusDefinitionSend>;
    snapshot.downmixRowBytes = harken::studio::build::kPackBytes<HkPackBusDefinitionDownmix>;
    snapshot.coefficientRowBytes = harken::studio::build::kPackBytes<HkPackBusDefinitionCoefficient>;
    if (edit) edit(image);
    snapshot.contentGeneration = harken::studio::build::CanonicalVolumeBusFingerprint(image,
        image.busDefinitionEffects, image.busDefinitionEffectParameters);
    snapshot.publishedBusFingerprint = snapshot.contentGeneration;
    snapshot.busOffset = snapshot.headerBytes;
    snapshot.sendOffset = snapshot.busOffset + snapshot.busCount * snapshot.busRowBytes;
    snapshot.effectOffset = snapshot.sendOffset + snapshot.sendCount * snapshot.sendRowBytes;
    snapshot.effectParameterOffset = snapshot.effectOffset +
        snapshot.effectCount * harken::studio::build::kPackBytes<HkPackBusDefinitionEffect>;
    snapshot.downmixOffset = snapshot.effectParameterOffset +
        snapshot.effectParameterCount * harken::studio::build::kPackBytes<HkPackBusDefinitionEffectParameter>;
    snapshot.coefficientOffset = snapshot.downmixOffset +
        snapshot.downmixCount * snapshot.downmixRowBytes;
    snapshot.payloadBytes = snapshot.coefficientOffset +
        snapshot.coefficientCount * snapshot.coefficientRowBytes - snapshot.headerBytes;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(snapshot.headerBytes + snapshot.payloadBytes), 0);
    for (std::uint32_t i = 0; i < snapshot.busCount; ++i) {
        harken::studio::build::SerializePackRow<HkPackBusDefinitionBus>(image.busDefinitionBusesV2[i],
            payload.data() + snapshot.busOffset + i * snapshot.busRowBytes);
        harken::studio::build::SerializePackRow<HkPackBusDefinitionDownmix>(image.busDefinitionDownmixesV2[i],
            payload.data() + snapshot.downmixOffset + i * snapshot.downmixRowBytes);
    }
    for (std::uint32_t i = 0; i < snapshot.sendCount; ++i) {
        harken::studio::build::SerializePackRow<HkPackBusDefinitionSend>(image.busDefinitionSendsV2[i],
            payload.data() + snapshot.sendOffset + i * snapshot.sendRowBytes);
    }
    for (std::uint32_t i = 0; i < snapshot.coefficientCount; ++i) {
        harken::studio::build::StoreLEF32(payload.data() + snapshot.coefficientOffset +
            i * snapshot.coefficientRowBytes, image.busDefinitionCoefficients[i]);
    }
    for (std::uint32_t i = 0; i < snapshot.effectCount; ++i)
        harken::studio::build::SerializePackRow<HkPackBusDefinitionEffect>(image.busDefinitionEffects[i],
            payload.data() + snapshot.effectOffset + i * harken::studio::build::kPackBytes<HkPackBusDefinitionEffect>);
    for (std::uint32_t i = 0; i < snapshot.effectParameterCount; ++i)
        harken::studio::build::SerializePackRow<HkPackBusDefinitionEffectParameter>(image.busDefinitionEffectParameters[i],
            payload.data() + snapshot.effectParameterOffset + i * harken::studio::build::kPackBytes<HkPackBusDefinitionEffectParameter>);
    harken::studio::build::SerializePackRow<HkPackBusDefinitionSnapshotHeader>(snapshot, payload.data());
    snapshot.checksum = harken::studio::build::Fnv1a64(payload.data(), payload.size());
    harken::studio::build::SerializePackRow<HkPackBusDefinitionSnapshotHeader>(snapshot, payload.data());

    harken::studio::build::PackRequirementBlock requirement;
    requirement.schemaVersion = 2;
    requirement.storageModelVersion = 2;
    requirement.packStableId = 801;
    requirement.packContentRevision = 1;
    requirement.busContentGeneration = snapshot.contentGeneration;
    requirement.publishedBusFingerprint = snapshot.publishedBusFingerprint;
    requirement.compileTargetProfileFingerprint =
        snapshot.compileTargetProfileFingerprint;
    std::vector<std::uint8_t> requirementPayload(
        harken::studio::build::kPackBytes<HkPackRequirementBlock>, 0);
    harken::studio::build::SerializePackRow<HkPackRequirementBlock>(requirement, requirementPayload.data());
    requirement.payloadChecksumCoverage =
        harken::studio::build::Fnv1a64(requirementPayload.data(), 104);
    harken::studio::build::SerializePackRow<HkPackRequirementBlock>(requirement, requirementPayload.data());

    constexpr std::uint32_t entryCount = 2;
    const std::uint64_t directoryOffset = harken::studio::build::kPackBytes<HkPackFileHeader>;
    const std::uint64_t requirementOffset = AlignPackPayload(directoryOffset +
        entryCount * harken::studio::build::kPackBytes<HkPackDirectoryEntry>);
    const std::uint64_t snapshotOffset = AlignPackPayload(
        requirementOffset + requirementPayload.size());
    std::vector<std::uint8_t> bytes(snapshotOffset + payload.size(), 0);
    std::memcpy(bytes.data() + requirementOffset, requirementPayload.data(),
                requirementPayload.size());
    std::memcpy(bytes.data() + snapshotOffset, payload.data(), payload.size());
    harken::studio::build::PackDirectoryEntry requirementEntry;
    requirementEntry.kind = HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK;
    requirementEntry.vintage = HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE;
    requirementEntry.offset = requirementOffset;
    requirementEntry.size = requirementPayload.size();
    requirementEntry.payloadChecksum = harken::studio::build::Fnv1a64(
        requirementPayload.data(), requirementPayload.size());
    harken::studio::build::SerializePackRow<HkPackDirectoryEntry>(requirementEntry,
        bytes.data() + directoryOffset);
    harken::studio::build::PackDirectoryEntry snapshotEntry;
    snapshotEntry.kind = HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT;
    snapshotEntry.vintage = HK_PACK_BUS_DEFINITION_SNAPSHOT_VINTAGE;
    snapshotEntry.relationOrdinal = 1;
    snapshotEntry.offset = snapshotOffset;
    snapshotEntry.size = payload.size();
    snapshotEntry.payloadChecksum = harken::studio::build::Fnv1a64(payload.data(), payload.size());
    harken::studio::build::SerializePackRow<HkPackDirectoryEntry>(snapshotEntry,
        bytes.data() + directoryOffset + harken::studio::build::kPackBytes<HkPackDirectoryEntry>);
    harken::studio::build::PackFileHeader packHeader;
    packHeader.formatVersion = HK_PACK_FORMAT_VERSION;
    packHeader.headerBytes = harken::studio::build::kPackBytes<HkPackFileHeader>;
    packHeader.fileBytes = bytes.size();
    packHeader.packKind = HK_PACK_KIND_INIT;
    packHeader.entryCount = entryCount;
    packHeader.directoryOffset = directoryOffset;
    packHeader.directoryChecksum = harken::studio::build::Fnv1a64(
        bytes.data() + directoryOffset,
        entryCount * harken::studio::build::kPackBytes<HkPackDirectoryEntry>);
    harken::studio::build::SerializePackRow<HkPackFileHeader>(packHeader, bytes.data());
    return bytes;
}

void ExpectParser(const std::vector<std::uint8_t>& bytes, bool accepted) {
    harken::studio::build::PackCanonicalImage image;
    EXPECT_EQ(accepted ? harken::studio::build::kPackOk : harken::studio::build::kPackInvalid,
        harken::studio::build::ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(CanonicalBusSendSnapshotParser, CapabilityFourRejectsCombinedGraphCycles) {
    const harken::studio::types::ObjectId source = harkentest::StudioObjectId(801);
    const harken::studio::types::ObjectId destination = harkentest::StudioObjectId(802);
    ExpectParser(MakeBusSendSnapshotPack(4, false,
        {{source, destination}}), true);
    ExpectParser(MakeBusSendSnapshotPack(4, false,
        {{source, destination}, {destination, source}}), false);
    ExpectParser(MakeBusSendSnapshotPack(4, true,
        {{source, destination}}), false);
}

TEST(CanonicalBusSendSnapshotParser, EffectPublicationSharesIdentitySliceAndValueValidation) {
    const auto withEffects = [](harken::studio::build::PackCanonicalImage& image) {
        auto& header = image.busDefinitionSnapshotHeader;
        header.effectCount = 2;
        header.effectParameterCount = 4;
        header.runtimeCapabilityMask |= (UINT64_C(1) << 19) |
            (UINT64_C(1) << (32 + VN_EFFECT_LIMITER));
        header.compileTargetProfileFingerprint = harken::studio::build::busfingerprint::ProfileFingerprintV1(
            header.profileVintage, header.renderSampleRateHz, header.mixLayout,
            header.deviceLayout, header.runtimeCapabilityMask);
        image.busDefinitionBusesV2[1].effectCount = 2;
        image.busDefinitionBusesV2[2].effectFirst = 2;
        image.busDefinitionEffects = {
            {harkentest::StudioObjectId(801), harkentest::StudioObjectId(901), 0, VN_EFFECT_LIMITER, 2, 0, 0, 2},
            {harkentest::StudioObjectId(801), harkentest::StudioObjectId(902), 1, VN_EFFECT_LIMITER, 2, 1, 2, 2}};
        image.busDefinitionEffectParameters = {{0, -6.0f}, {1, 100.0f}, {0, -12.0f}, {1, 50.0f}};
    };
    ExpectParser(MakeBusSendSnapshotPack(5, false, {}, withEffects), true);
    for (const auto bit : {19u, 32u + VN_EFFECT_LIMITER}) {
        ExpectParser(MakeBusSendSnapshotPack(5, false, {}, [&](auto& image) {
            withEffects(image);
            auto& header = image.busDefinitionSnapshotHeader;
            header.runtimeCapabilityMask &= ~(UINT64_C(1) << bit);
            header.compileTargetProfileFingerprint = harken::studio::build::busfingerprint::ProfileFingerprintV1(
                header.profileVintage, header.renderSampleRateHz, header.mixLayout,
                header.deviceLayout, header.runtimeCapabilityMask);
        }), false);
    }
    for (const auto& corrupt : std::vector<std::function<void(harken::studio::build::PackCanonicalImage&)>>{
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
        ExpectParser(MakeBusSendSnapshotPack(5, false, {}, [&](auto& image) {
            withEffects(image);
            corrupt(image);
        }), false);
    }
}

} // namespace
