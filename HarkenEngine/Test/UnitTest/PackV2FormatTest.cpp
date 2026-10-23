// Pack container v2 byte-contract tests. A test-local image builder
// assembles complete in-memory v2 files — header, typed directory,
// 8-aligned payloads in add order with zero padding, 64-aligned media
// blobs, FNV-1a checksums — and ParsePackV2Image proves full-field
// round trips for every entry kind plus the refusal matrix: header
// sanity, checksum tampering, unknown kinds and vintages, pack-kind
// discipline, directory/payload id agreement, duplicate ids and
// relations, wire strictness (reserved bytes, zero-fill tails,
// name rules), non-finite send levels, and layout normalization
// (directory anchored at the header, 8-aligned payloads, no aliased
// payload/blob ranges). CheckPackClosure cases drive a synthetic resolver over
// std::set-backed id sets to prove the closure rule and the refusal
// triple (reference class, referrer id, missing id). Pure byte-level
// cases — no engine session, no file I/O.
#include "Common/ObjectIdTestSupport.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "harken/param/EffectParamLocator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace {

using namespace harken;

constexpr std::uint32_t kLegacySelectionRoundRobin = 1;

TEST(PackV2Format, CurveWireSizesArePinned) {
    EXPECT_EQ(184u, kPackV2CurveWireBytes);
    EXPECT_EQ(600u, kPackV2AttenuationPayloadBytes);
    EXPECT_EQ(608u, kPackV3AttenuationPayloadBytes);
    EXPECT_EQ(792u, kPackV4AttenuationPayloadBytes);
    EXPECT_EQ(180u, kPackV2BindingCurveWireBytes);
    EXPECT_EQ(228u, kPackV2BindingDefinitionPayloadBytes);
    EXPECT_EQ(32u, kPackV2BusPayloadBytes);
    EXPECT_EQ(48u, kPackV6BusPayloadBytes);
}

TEST(PackV2Format, PackContainerUsesPublishedSchemaVersion) {
    static_assert(kPackFormatVersion == HARKEN_PACK_SCHEMA_VERSION);
    static_assert(kPackFormatVersion2 == HARKEN_PACK_SCHEMA_VERSION);
    EXPECT_EQ(HARKEN_PACK_SCHEMA_VERSION, kPackFormatVersion);
    EXPECT_EQ(HARKEN_PACK_SCHEMA_VERSION, kPackFormatVersion2);
}

TEST(PackV2Format, VolumeSnapshotParentForestAcceptsDeepUnorderedChainAndRejectsCycles) {
    constexpr std::size_t kBusCount = 4096;
    std::vector<PackBusDefinitionBusV2> buses;
    buses.reserve(kBusCount);
    PackBusDefinitionBusV2 master{};
    master.busId = VN_BUS_MASTER;
    buses.push_back(master);
    for (std::size_t i = 1; i < kBusCount; ++i) {
        PackBusDefinitionBusV2 bus{};
        bus.busId = harkentest::ObjectId(i);
        bus.parentId = buses.back().busId;
        buses.push_back(bus);
    }
    EXPECT_TRUE(ValidateCanonicalBusParentForestV2(buses));
    std::reverse(buses.begin(), buses.end());
    EXPECT_TRUE(ValidateCanonicalBusParentForestV2(buses));

    const auto parent = buses[1].parentId;
    buses[1].parentId = buses[0].busId;
    EXPECT_FALSE(ValidateCanonicalBusParentForestV2(buses));
    buses[1].parentId = harkentest::ObjectId(kBusCount + 1);
    EXPECT_FALSE(ValidateCanonicalBusParentForestV2(buses));
    buses[1].parentId = parent;
    buses.back().parentId = buses.front().busId;
    EXPECT_FALSE(ValidateCanonicalBusParentForestV2(buses));
    buses.back().parentId = {};
    std::rotate(buses.begin(), buses.begin() + kBusCount / 3, buses.end());
    EXPECT_TRUE(ValidateCanonicalBusParentForestV2(buses));
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) {
    return (value + align - 1) / align * align;
}

// --- test-local v2 image builder ---
// Assembles the exact file shape the parser expects; every checksum is
// computed over the final bytes, so a test that wants a corrupt image
// either patches bytes AFTER Build (checksum mismatch cases) or hands
// AddEntry a deliberately malformed payload BEFORE Build (wire
// strictness cases, where checksums must still match).

class ImageBuilder {
public:
    explicit ImageBuilder(std::uint32_t packKind) : packKind_(packKind) {}

    std::size_t AddEntry(std::uint32_t kind, std::uint64_t id,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = kPackV2SchemaVintage) {
        Entry entry;
        entry.kind = kind;
        entry.vintage = vintage;
        entry.id = id;
        entry.payload = std::move(payload);
        entries_.push_back(std::move(entry));
        return entries_.size() - 1;
    }

    // Media entries carry the v1 TOC record as payload; blobOffset and
    // blobChecksum are only known at Build, so the record is serialized
    // there from this spec.
    std::size_t AddMedia(std::uint64_t directoryId, VnObjectId soundId,
                         std::uint16_t channelCount,
                         std::vector<float> planar) {
        Entry entry;
        entry.kind = kPackEntryKindMedia;
        entry.vintage = kPackV2SchemaVintage;
        entry.id = directoryId;
        entry.isMedia = true;
        entry.soundId = soundId;
        entry.channelCount = channelCount;
        entry.planar = std::move(planar);
        entries_.push_back(std::move(entry));
        return entries_.size() - 1;
    }

    std::vector<std::uint8_t> Build() {
        const std::uint64_t dirOffset = kPackV2HeaderBytes;
        const std::uint64_t dirBytes =
            static_cast<std::uint64_t>(entries_.size()) *
            kPackV2DirectoryEntryBytes;
        const std::uint64_t dirEnd = dirOffset + dirBytes;

        payloadOffsets_.assign(entries_.size(), 0);
        blobOffsets_.assign(entries_.size(), 0);

        std::uint64_t cursor = AlignUp(dirEnd, kPackV2PayloadAlign);
        std::uint64_t contentEnd = dirEnd;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            payloadOffsets_[i] = cursor;
            contentEnd = cursor + PayloadSize(entries_[i]);
            cursor = AlignUp(contentEnd, kPackV2PayloadAlign);
        }
        std::uint64_t blobCursor = AlignUp(contentEnd, kPackBlobAlign);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (!entries_[i].isMedia) {
                continue;
            }
            blobOffsets_[i] = blobCursor;
            contentEnd = blobCursor + BlobBytes(entries_[i]);
            blobCursor = AlignUp(contentEnd, kPackBlobAlign);
        }
        const std::uint64_t fileBytes = contentEnd;

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes),
                                        0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry& entry = entries_[i];
            std::uint8_t* payload =
                bytes.data() + static_cast<std::size_t>(payloadOffsets_[i]);
            if (entry.isMedia) {
                std::memcpy(bytes.data() +
                                static_cast<std::size_t>(blobOffsets_[i]),
                            entry.planar.data(),
                            static_cast<std::size_t>(BlobBytes(entry)));
                PackTocEntry toc;
                toc.soundId = entry.soundId;
                toc.sampleRate = 48000;
                toc.channelCount = entry.channelCount;
                toc.sampleFormat = kPackSampleFormatF32Planar;
                toc.frameCount = entry.planar.size() / entry.channelCount;
                toc.blobOffset = blobOffsets_[i];
                toc.blobBytes = BlobBytes(entry);
                toc.blobChecksum = Fnv1a64(
                    bytes.data() + static_cast<std::size_t>(blobOffsets_[i]),
                    static_cast<std::size_t>(toc.blobBytes));
                toc.flags = 0;
                toc.loopStartFrame = 0;
                toc.loopEndFrame = 0;
                SerializePackTocEntry(toc, payload);
            } else {
                std::memcpy(payload, entry.payload.data(),
                            entry.payload.size());
            }
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry& entry = entries_[i];
            PackV2DirectoryEntry record;
            record.kind = entry.kind;
            record.vintage = entry.vintage;
            if (entry.kind == kPackEntryKindBusSend || entry.kind == kPackEntryKindBindingAttachment) {
                record.relationOrdinal = entry.id;
            } else {
                record.id = harkentest::ObjectId(entry.id);
            }
            record.offset = payloadOffsets_[i];
            record.size = PayloadSize(entry);
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
        header.fileBytes = fileBytes;
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
    std::uint64_t BlobOffset(std::size_t i) const { return blobOffsets_[i]; }

private:
    struct Entry {
        std::uint32_t kind = 0;
        std::uint32_t vintage = 0;
        std::uint64_t id = 0;
        std::vector<std::uint8_t> payload;
        bool isMedia = false;
        VnObjectId soundId = harkentest::ObjectId(0);
        std::uint16_t channelCount = 0;
        std::vector<float> planar;
    };

    static std::uint64_t PayloadSize(const Entry& entry) {
        return entry.isMedia ? kPackV2MediaPayloadBytes
                             : entry.payload.size();
    }
    static std::uint64_t BlobBytes(const Entry& entry) {
        return static_cast<std::uint64_t>(entry.planar.size()) *
               sizeof(float);
    }

    std::uint32_t packKind_ = kPackKindContent;
    std::vector<Entry> entries_;
    std::vector<std::uint64_t> payloadOffsets_;
    std::vector<std::uint64_t> blobOffsets_;
};

// --- payload factories ---

std::vector<std::uint8_t> BusPayload(VnBusId busId, VnBusId parentBusId) {
    std::vector<std::uint8_t> payload(kPackV2BusPayloadBytes);
    PackBusEntry entry;
    entry.busId = busId;
    entry.parentBusId = parentBusId;
    SerializePackV2Bus(entry, payload.data());
    return payload;
}

std::vector<std::uint8_t> BusMixPayload(VnBusId busId, VnBusId parentBusId,
                                        float volume, float outputBusVolume,
                                        float makeUpGain) {
    std::vector<std::uint8_t> payload(kPackV6BusPayloadBytes);
    PackBusEntry entry;
    entry.busId = busId;
    entry.parentBusId = parentBusId;
    entry.volume = volume;
    entry.outputBusVolume = outputBusVolume;
    entry.makeUpGain = makeUpGain;
    SerializePackV6Bus(entry, payload.data());
    return payload;
}

std::vector<std::uint8_t> BusSendPayload(VnBusId fromBusId, VnBusId toBusId,
                                         float levelDb) {
    std::vector<std::uint8_t> payload(kPackV2BusSendPayloadBytes);
    PackBusSendEntry entry;
    entry.fromBusId = fromBusId;
    entry.toBusId = toBusId;
    entry.levelDb = levelDb;
    SerializePackV2BusSend(entry, payload.data());
    return payload;
}

std::vector<std::uint8_t> AttenuationPayload(const VnAttenuationDesc& desc) {
    VnAttenuationDesc wire = desc;
    if (!(wire.proximityFrequency > 0.0f)) {
        wire.proximityFrequency = 250.0f;
    }
    std::vector<std::uint8_t> payload(kPackV3AttenuationPayloadBytes);
    SerializePackV3Attenuation(wire, payload.data());
    return payload;
}

std::vector<std::uint8_t> RetiredAttenuationPayload(
    const VnAttenuationDesc& desc) {
    std::vector<std::uint8_t> payload(kPackV2AttenuationPayloadBytes);
    SerializePackV2Attenuation(desc, payload.data());
    return payload;
}

std::vector<std::uint8_t> DiscreteVariablePayload(VnVariableId variableId) {
    std::vector<std::uint8_t> payload(kPackV2DiscreteVariablePayloadBytes);
    SerializePackV2DiscreteVariable(variableId, payload.data());
    return payload;
}

std::vector<std::uint8_t> MeterWiringPayload(VnBusId busId,
                                             VnVariableId variableId,
                                             std::uint32_t attackMs,
                                             std::uint32_t releaseMs) {
    std::vector<std::uint8_t> payload(kPackV2MeterWiringPayloadBytes);
    PackMeterWiringEntry entry;
    entry.busId = busId;
    entry.variableId = variableId;
    entry.attackMs = attackMs;
    entry.releaseMs = releaseMs;
    SerializePackV2MeterWiring(entry, payload.data());
    return payload;
}

std::vector<std::uint8_t> BindingDefinitionPayload(VnBindingId bindingId,
                                                   const VnBindingDesc& desc) {
    std::vector<std::uint8_t> payload(kPackV2BindingDefinitionPayloadBytes);
    SerializePackV2BindingDefinition(bindingId, desc, payload.data());
    return payload;
}

std::vector<std::uint8_t> AttachmentPayload(VnBindingId bindingId,
                                            std::uint32_t scopeKind,
                                            std::uint64_t objectId) {
    std::vector<std::uint8_t> payload(kPackV2BindingAttachmentPayloadBytes);
    PackBindingAttachmentEntry entry;
    entry.bindingId = bindingId;
    entry.scopeKind = scopeKind;
    entry.objectId = harkentest::ObjectId(objectId);
    SerializePackV2BindingAttachment(entry, payload.data());
    return payload;
}

// The header's childCount is taken from the header struct, the buffer is
// sized from the array — deliberately independent so a disagreeing pair
// can be built for the size-mismatch refusal.
std::vector<std::uint8_t> ContainerPayload(
    const PackContainerEntryHeader& header,
    const std::vector<PackContainerChildV2>& children) {
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(
        kPackV2ContainerHeaderBytes +
        children.size() * kPackV2ContainerChildBytes));
    SerializePackV2ContainerHeader(header, payload.data());
    for (std::size_t i = 0; i < children.size(); ++i) {
        SerializePackV2ContainerChild(
            children[i], payload.data() + kPackV2ContainerHeaderBytes +
                             i * kPackV2ContainerChildBytes);
    }
    return payload;
}

constexpr std::uint64_t kVoiceRecipeV5PrefixBytes =
    kPackV2VoiceRecipeHeaderBytes + kPackV3VoiceRecipeToneBytes +
    kPackV4VoiceRecipeSpatialBytes + kPackV5VoiceRecipePlaybackBytes;

void WriteEmptyVoiceRecipeSlots(std::uint8_t* payload) {
    PackVoiceRecipeTone tone;
    SerializePackV3VoiceRecipeTone(
        tone, payload + kPackV2VoiceRecipeHeaderBytes);
    PackVoiceRecipeSpatial spatial;
    spatial.distanceScale = 0.0f;
    SerializePackV4VoiceRecipeSpatial(
        spatial, payload + kPackV2VoiceRecipeHeaderBytes +
                     kPackV3VoiceRecipeToneBytes);
    PackVoiceRecipePlayback playback;
    SerializePackV5VoiceRecipePlayback(
        playback, payload + kPackV2VoiceRecipeHeaderBytes +
                      kPackV3VoiceRecipeToneBytes +
                      kPackV4VoiceRecipeSpatialBytes);
}

std::vector<std::uint8_t> GainRecipePayload(
    VnSoundId playable, std::uint32_t slot, float gainDb,
    const std::vector<VnBindingId>& bindings = {}) {
    PackVoiceRecipeEntryHeader header;
    header.playableId = playable;
    header.installCount = 1;
    header.bindingCount = static_cast<std::uint32_t>(bindings.size());
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(PackV5VoiceRecipePayloadBytes(
            header.installCount, header.bindingCount)),
        0);
    SerializePackV2VoiceRecipeHeader(header, payload.data());
    WriteEmptyVoiceRecipeSlots(payload.data());
    PackVoiceRecipeInstall install;
    install.slot = slot;
    install.effectType = VN_EFFECT_GAIN;
    install.descriptorVintage = kPackV2EffectDescriptorVintage;
    install.paramCount = 1;
    install.params[0] = gainDb;
    SerializePackV2VoiceRecipeInstall(
        install, payload.data() + kVoiceRecipeV5PrefixBytes);
    std::uint8_t* binding = payload.data() + kVoiceRecipeV5PrefixBytes +
                            kPackV2VoiceRecipeInstallBytes;
    for (VnBindingId id : bindings) {
        SerializePackV2VoiceRecipeBinding(id, binding);
        binding += kPackV2VoiceRecipeBindingBytes;
    }
    return payload;
}

// Builds a recipe payload carrying two gain installs at the given slots. The
// installs are otherwise well-formed (correct vintage, one finite param each),
// so the only defect a test can express through the slot arguments is the
// parser's strict-ascending slot rule — exercised with genuine install records
// rather than a single install with a hand-patched field.
std::vector<std::uint8_t> TwoInstallGainRecipePayload(VnSoundId playable,
                                                      std::uint32_t slot0,
                                                      std::uint32_t slot1) {
    PackVoiceRecipeEntryHeader header;
    header.playableId = playable;
    header.installCount = 2;
    header.bindingCount = 0;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(PackV5VoiceRecipePayloadBytes(
            header.installCount, header.bindingCount)),
        0);
    SerializePackV2VoiceRecipeHeader(header, payload.data());
    WriteEmptyVoiceRecipeSlots(payload.data());
    const std::uint32_t slots[2] = {slot0, slot1};
    for (std::uint32_t n = 0; n < 2; ++n) {
        PackVoiceRecipeInstall install;
        install.slot = slots[n];
        install.effectType = VN_EFFECT_GAIN;
        install.descriptorVintage = kPackV2EffectDescriptorVintage;
        install.paramCount = 1;
        install.params[0] = -6.0f;
        SerializePackV2VoiceRecipeInstall(
            install, payload.data() + kVoiceRecipeV5PrefixBytes +
                         static_cast<std::size_t>(n) *
                             kPackV2VoiceRecipeInstallBytes);
    }
    return payload;
}

std::vector<std::uint8_t> ToneRecipePayload(VnSoundId playable,
                                            bool toneOnly = false) {
    PackVoiceRecipeEntryHeader header;
    header.playableId = playable;
    header.installCount = toneOnly ? 0u : 1u;
    header.bindingCount = 0;
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(
        PackV5VoiceRecipePayloadBytes(header.installCount,
                                      header.bindingCount)), 0);
    SerializePackV2VoiceRecipeHeader(header, payload.data());
    WriteEmptyVoiceRecipeSlots(payload.data());
    PackVoiceRecipeTone tone;
    tone.present = 1;
    tone.sectionEnableMask = kPackVoiceToneLowShelfEnabled;
    tone.lowpassFrequencyHz = 20000.0f;
    tone.lowpassQ = 0.70710678f;
    tone.highpassFrequencyHz = 20.0f;
    tone.highpassQ = 0.70710678f;
    tone.lowShelfFrequencyHz = 150.0f;
    tone.lowShelfQ = 0.8f;
    tone.lowShelfGainDb = 3.0f;
    tone.highShelfFrequencyHz = 6000.0f;
    tone.highShelfQ = 0.70710678f;
    tone.highShelfGainDb = 0.0f;
    SerializePackV3VoiceRecipeTone(
        tone, payload.data() + kPackV2VoiceRecipeHeaderBytes);
    if (!toneOnly) {
        PackVoiceRecipeInstall install;
        install.slot = 0;
        install.effectType = VN_EFFECT_GAIN;
        install.descriptorVintage = kPackV2EffectDescriptorVintage;
        install.paramCount = 1;
        install.params[0] = -6.0f;
        SerializePackV2VoiceRecipeInstall(
            install, payload.data() + kVoiceRecipeV5PrefixBytes);
    }
    return payload;
}

std::vector<std::uint8_t> SpatialRecipePayload(
    VnSoundId playable, VnAttenuationId attenuation, float distanceScale) {
    PackVoiceRecipeEntryHeader header;
    header.playableId = playable;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(PackV5VoiceRecipePayloadBytes(0, 0)), 0);
    SerializePackV2VoiceRecipeHeader(header, payload.data());
    WriteEmptyVoiceRecipeSlots(payload.data());
    PackVoiceRecipeSpatial spatial;
    spatial.attenuationId = attenuation;
    spatial.distanceScale = distanceScale;
    SerializePackV4VoiceRecipeSpatial(
        spatial, payload.data() + kPackV2VoiceRecipeHeaderBytes +
                     kPackV3VoiceRecipeToneBytes);
    return payload;
}

std::vector<std::uint8_t> PlaybackRecipePayload(
    VnSoundId playable, std::uint32_t present, std::uint32_t loopCount,
    VnAttenuationId attenuation = harkentest::ObjectId(0), float distanceScale = 0.0f) {
    PackVoiceRecipeEntryHeader header;
    header.playableId = playable;
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(PackV5VoiceRecipePayloadBytes(0, 0)), 0);
    SerializePackV2VoiceRecipeHeader(header, payload.data());
    PackVoiceRecipeTone tone;
    SerializePackV3VoiceRecipeTone(
        tone, payload.data() + kPackV2VoiceRecipeHeaderBytes);
    PackVoiceRecipeSpatial spatial;
    spatial.attenuationId = attenuation;
    spatial.distanceScale = distanceScale;
    SerializePackV4VoiceRecipeSpatial(
        spatial, payload.data() + kPackV2VoiceRecipeHeaderBytes +
                     kPackV3VoiceRecipeToneBytes);
    PackVoiceRecipePlayback playback;
    playback.present = present;
    playback.loopCount = loopCount;
    SerializePackV5VoiceRecipePlayback(
        playback, payload.data() + kPackV2VoiceRecipeHeaderBytes +
                      kPackV3VoiceRecipeToneBytes +
                      kPackV4VoiceRecipeSpatialBytes);
    return payload;
}

TEST(PackV2Format, VoiceRecipeGainRoundTripsWithCanonicalBindings) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(kPackEntryKindVoiceRecipe, 100,
                     GainRecipePayload(harkentest::ObjectId(100), 0, -6.0f, {harkentest::ObjectId(700), harkentest::ObjectId(701)}),
                     kPackV2PlaybackRecipeSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(1u, image.voiceRecipes.size());
    const PackLogicImage::VoiceRecipe& recipe = image.voiceRecipes[0];
    ASSERT_EQ(1u, recipe.installs.size());
    EXPECT_EQ(VN_EFFECT_GAIN, recipe.installs[0].effectType);
    EXPECT_FLOAT_EQ(-6.0f, recipe.installs[0].params[0]);
    EXPECT_EQ((std::vector<VnBindingId>{harkentest::ObjectId(700), harkentest::ObjectId(701)}), recipe.bindings);
}

TEST(PackV2Format, AuthoredToneRecipeRoundTripsAndAllowsToneOnly) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(kPackEntryKindVoiceRecipe, 101, ToneRecipePayload(harkentest::ObjectId(101)),
                     kPackV2PlaybackRecipeSchemaVintage);
    builder.AddEntry(kPackEntryKindVoiceRecipe, 102,
                     ToneRecipePayload(harkentest::ObjectId(102), true),
                     kPackV2PlaybackRecipeSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(2u, image.voiceRecipes.size());
    EXPECT_EQ(150.0f, image.voiceRecipes[0].tone.lowShelfFrequencyHz);
    EXPECT_EQ(0u, image.voiceRecipes[1].header.installCount);
    EXPECT_EQ(1u, image.voiceRecipes[1].tone.present);
}

TEST(PackV2Format, RetiredVoiceRecipeVintagesRefuse) {
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 103, ToneRecipePayload(harkentest::ObjectId(103)),
                         kPackV2SchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_PACK_INCOMPATIBLE,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
        EXPECT_EQ(0u, image.EntryCount());
    }
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 104, ToneRecipePayload(harkentest::ObjectId(104)),
                         kPackV2AuthoredToneSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_PACK_INCOMPATIBLE,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
        EXPECT_EQ(0u, image.EntryCount());
    }
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 105,
                         SpatialRecipePayload(harkentest::ObjectId(105), harkentest::ObjectId(701), 1.5f),
                         kPackV2SpatialRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_PACK_INCOMPATIBLE,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
        EXPECT_EQ(0u, image.EntryCount());
    }
}

TEST(PackV2Format, SpatialRecipeRoundTripsWithoutAuthoredTone) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(kPackEntryKindVoiceRecipe, 104,
                     SpatialRecipePayload(harkentest::ObjectId(104), harkentest::ObjectId(701), 1.5f),
                     kPackV2PlaybackRecipeSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(1u, image.voiceRecipes.size());
    EXPECT_EQ(0u, image.voiceRecipes[0].tone.present);
    EXPECT_EQ(harkentest::ObjectId(701u), image.voiceRecipes[0].spatial.attenuationId);
    EXPECT_FLOAT_EQ(1.5f, image.voiceRecipes[0].spatial.distanceScale);
}

// Vintage 5 is positional: the spatial block sits between tone and playback
// whether or not the recipe authors an attenuation, so a pack that carries
// only a pass count still round trips with an all-zero spatial block.
TEST(PackV2Format, PlaybackRecipeRoundTripsWithAbsentSpatial) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(kPackEntryKindVoiceRecipe, 105,
                     PlaybackRecipePayload(harkentest::ObjectId(105), 1, 3),
                     kPackV2PlaybackRecipeSchemaVintage);
    builder.AddEntry(kPackEntryKindVoiceRecipe, 106,
                     PlaybackRecipePayload(harkentest::ObjectId(106), 1, 0, harkentest::ObjectId(702), 2.0f),
                     kPackV2PlaybackRecipeSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(2u, image.voiceRecipes.size());
    EXPECT_EQ(0u, image.voiceRecipes[0].tone.present);
    EXPECT_EQ(harkentest::ObjectId(0u), image.voiceRecipes[0].spatial.attenuationId);
    EXPECT_EQ(1u, image.voiceRecipes[0].playback.present);
    EXPECT_EQ(3u, image.voiceRecipes[0].playback.loopCount);
    EXPECT_EQ(harkentest::ObjectId(702u), image.voiceRecipes[1].spatial.attenuationId);
    EXPECT_FLOAT_EQ(2.0f, image.voiceRecipes[1].spatial.distanceScale);
    EXPECT_EQ(0u, image.voiceRecipes[1].playback.loopCount);
}

TEST(PackV2Format, PlaybackRecipeMalformedBlockRefuses) {
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 105,
                         PlaybackRecipePayload(harkentest::ObjectId(105), 2, 3),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 105,
                         PlaybackRecipePayload(harkentest::ObjectId(105), 0, 3),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    // A nonzero attenuation still owes a positive distance scale.
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 105,
                         PlaybackRecipePayload(harkentest::ObjectId(105), 1, 2, harkentest::ObjectId(703), 0.0f),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
}

TEST(PackV2Format, VoiceRecipeMalformedInstallAndBindingOrderRefuse) {
    {
        std::vector<std::uint8_t> payload =
            GainRecipePayload(harkentest::ObjectId(100), 0, -6.0f);
        StoreLE32(payload.data() + kVoiceRecipeV5PrefixBytes + 8,
                  kPackV2EffectDescriptorVintage + 1);
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100, std::move(payload),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100,
                         GainRecipePayload(harkentest::ObjectId(100), 0, -6.0f, {harkentest::ObjectId(701), harkentest::ObjectId(700)}),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    // Two real installs whose slots descend [1, 0]: strict-ascending order is
    // violated even though both records are individually well-formed.
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100,
                         TwoInstallGainRecipePayload(harkentest::ObjectId(100), 1, 0),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    // Two installs on the same slot [0, 0]: a repeat is not strictly greater
    // than its predecessor, so the ascending rule refuses it too.
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100,
                         TwoInstallGainRecipePayload(harkentest::ObjectId(100), 0, 0),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
}

TEST(PackV2Format, VoiceRecipeTruncatedAndNonZeroTailRefuse) {
    {
        std::vector<std::uint8_t> payload =
            GainRecipePayload(harkentest::ObjectId(100), 0, -6.0f);
        payload.pop_back();
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100, std::move(payload),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    {
        std::vector<std::uint8_t> payload =
            GainRecipePayload(harkentest::ObjectId(100), 0, -6.0f);
        StoreLE32(payload.data() + kVoiceRecipeV5PrefixBytes + 36,
                  0x3f800000u);
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100, std::move(payload),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
}

// --- shared refusal scaffolding ---

std::vector<float> SmallStereoPlanar() {
    // 4 frames, 2 channels, planar: left plane then right plane.
    return {0.1f, 0.2f, 0.3f, 0.4f, -0.1f, -0.2f, -0.3f, -0.4f};
}

// Minimal valid content image. The header corruption cases start from these
// bytes.
std::vector<std::uint8_t> MinimalContentBytes() {
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    return builder.Build();
}

void ExpectRefused(const std::vector<std::uint8_t>& bytes) {
    PackLogicImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    // A refusal never publishes partial state: the out image stays empty.
    EXPECT_EQ(0u, image.EntryCount());
}

// Locates the first directory record of the given kind the way a reader
// would — walking the fixed-size records at the header's directory
// offset — and returns its payload offset. Fails the test when the
// kind is absent.
std::uint64_t PayloadOffsetOfKind(const std::vector<std::uint8_t>& bytes,
                                  std::uint32_t kind) {
    PackV2Header header;
    EXPECT_TRUE(ParsePackV2Header(bytes.data(), &header));
    for (std::uint32_t k = 0; k < header.entryCount; ++k) {
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            bytes.data() +
            static_cast<std::size_t>(header.directoryOffset) +
            k * kPackV2DirectoryEntryBytes);
        if (entry.kind == kind) {
            return entry.offset;
        }
    }
    ADD_FAILURE() << "no directory entry of kind " << kind;
    return 0;
}

// Recomputes BOTH checksum layers over the current bytes — each
// directory record's payload checksum from the range it points at, then
// the header's directory checksum over the rewritten records — so a
// test can rewrite layout fields (offsets, sizes, file length) and still
// present a checksum-consistent file whose only defect is the layout
// fact under test.
void RecomputeChecksums(std::vector<std::uint8_t>* bytes) {
    PackV2Header header;
    ASSERT_TRUE(ParsePackV2Header(bytes->data(), &header));
    const std::size_t dir =
        static_cast<std::size_t>(header.directoryOffset);
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
        static_cast<std::size_t>(header.entryCount) *
            kPackV2DirectoryEntryBytes);
    SerializePackV2Header(header, bytes->data());
}

// --- round trips ---

TEST(PackV2Format, ContentPackRoundTripCarriesEveryField) {
    const std::vector<float> planar = SmallStereoPlanar();

    ImageBuilder builder(kPackKindContent);
    const std::size_t mediaIndex = builder.AddMedia(100, harkentest::ObjectId(100), 2, planar);

    PackContainerEntryHeader containerHeader;
    containerHeader.containerId = harkentest::ObjectId(200);
    containerHeader.selectionOrder = kLegacySelectionRoundRobin;
    containerHeader.avoidRepeatWindow = 3;
    containerHeader.selectionVariableId = harkentest::ObjectId(910);
    containerHeader.defaultIndex = 1;
    containerHeader.childCount = 2;
    std::vector<PackContainerChildV2> children(2);
    children[0].playableId = harkentest::ObjectId(100);
    children[0].weight = 5;
    children[0].matchValue = 7;
    children[0].delayMs = 120;
    children[0].delayJitterMs = 60;
    children[1].playableId = harkentest::ObjectId(777);
    children[1].weight = 1;
    children[1].matchValue = 0;
    children[1].delayMs = 0;
    children[1].delayJitterMs = 0;
    builder.AddEntry(kPackEntryKindContainer, 200,
                     ContainerPayload(containerHeader, children));

    VnBindingDesc bindingDesc{};
    bindingDesc.variableId = harkentest::ObjectId(910);
    bindingDesc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    bindingDesc.targetId = 0;
    bindingDesc.transitionMs = 80;
    bindingDesc.curve.pointCount = 3;
    bindingDesc.curve.points[0] = {0.0f, 0.0f};
    bindingDesc.curve.points[1] = {0.5f, -6.0f};
    bindingDesc.curve.points[2] = {1.0f, -12.0f};
    bindingDesc.curve.handles[0] = {0.1f, -1.0f, 0.4f, -5.0f};
    bindingDesc.curve.handles[1] = {0.6f, -7.0f, 0.9f, -11.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 400,
                     BindingDefinitionPayload(harkentest::ObjectId(400), bindingDesc));

    builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                     AttachmentPayload(harkentest::ObjectId(400), VN_TARGET_SCOPE_VOICE, 100));

    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));

    EXPECT_EQ(kPackKindContent, image.packKind);
    EXPECT_EQ(4u, image.EntryCount());

    ASSERT_EQ(1u, image.media.size());
    const PackTocEntry& media = image.media[0];
    EXPECT_EQ(harkentest::ObjectId(100u), media.soundId);
    EXPECT_EQ(48000u, media.sampleRate);
    EXPECT_EQ(2u, media.channelCount);
    EXPECT_EQ(kPackSampleFormatF32Planar, media.sampleFormat);
    EXPECT_EQ(4u, media.frameCount);
    EXPECT_EQ(planar.size() * sizeof(float), media.blobBytes);
    EXPECT_EQ(builder.BlobOffset(mediaIndex), media.blobOffset);
    EXPECT_EQ(0u, media.blobOffset % kPackBlobAlign);
    EXPECT_EQ(Fnv1a64(planar.data(), planar.size() * sizeof(float)),
              media.blobChecksum);
    EXPECT_EQ(0, std::memcmp(bytes.data() +
                                 static_cast<std::size_t>(media.blobOffset),
                             planar.data(), planar.size() * sizeof(float)));
    EXPECT_EQ(0u, media.flags);
    EXPECT_EQ(0u, media.loopStartFrame);
    EXPECT_EQ(0u, media.loopEndFrame);

    ASSERT_EQ(1u, image.containers.size());
    const PackLogicImage::Container& container = image.containers[0];
    EXPECT_EQ(harkentest::ObjectId(200u), container.header.containerId);
    EXPECT_EQ(kLegacySelectionRoundRobin,
              container.header.selectionOrder);
    EXPECT_EQ(3u, container.header.avoidRepeatWindow);
    EXPECT_EQ(harkentest::ObjectId(910u), container.header.selectionVariableId);
    EXPECT_EQ(1u, container.header.defaultIndex);
    EXPECT_EQ(2u, container.header.childCount);
    ASSERT_EQ(2u, container.children.size());
    EXPECT_EQ(harkentest::ObjectId(100u), container.children[0].playableId);
    EXPECT_EQ(5u, container.children[0].weight);
    EXPECT_EQ(7u, container.children[0].matchValue);
    EXPECT_EQ(120u, container.children[0].delayMs);
    EXPECT_EQ(60u, container.children[0].delayJitterMs);
    EXPECT_EQ(harkentest::ObjectId(777u), container.children[1].playableId);
    EXPECT_EQ(1u, container.children[1].weight);
    EXPECT_EQ(0u, container.children[1].matchValue);
    EXPECT_EQ(0u, container.children[1].delayMs);
    EXPECT_EQ(0u, container.children[1].delayJitterMs);

    ASSERT_EQ(1u, image.bindingDefinitions.size());
    const PackLogicImage::BindingDefinition& definition =
        image.bindingDefinitions[0];
    EXPECT_EQ(harkentest::ObjectId(400u), definition.id);
    EXPECT_EQ(sizeof(VnBindingDesc), definition.desc.structSize);
    EXPECT_EQ(harkentest::ObjectId(910u), definition.desc.variableId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_VOICE),
              definition.desc.targetScopeKind);
    EXPECT_EQ(0u, definition.desc.targetId);
    EXPECT_EQ(80u, definition.desc.transitionMs);
    EXPECT_EQ(sizeof(VnCurveDesc), definition.desc.curve.structSize);
    EXPECT_EQ(3u, definition.desc.curve.pointCount);
    EXPECT_EQ(0.0f, definition.desc.curve.points[0].x);
    EXPECT_EQ(0.0f, definition.desc.curve.points[0].y);
    EXPECT_EQ(0.5f, definition.desc.curve.points[1].x);
    EXPECT_EQ(-6.0f, definition.desc.curve.points[1].y);
    EXPECT_EQ(1.0f, definition.desc.curve.points[2].x);
    EXPECT_EQ(-12.0f, definition.desc.curve.points[2].y);
    EXPECT_EQ(0.1f, definition.desc.curve.handles[0].h1x);
    EXPECT_EQ(-1.0f, definition.desc.curve.handles[0].h1y);
    EXPECT_EQ(0.4f, definition.desc.curve.handles[0].h2x);
    EXPECT_EQ(-5.0f, definition.desc.curve.handles[0].h2y);
    EXPECT_EQ(0.6f, definition.desc.curve.handles[1].h1x);
    EXPECT_EQ(-7.0f, definition.desc.curve.handles[1].h1y);
    EXPECT_EQ(0.9f, definition.desc.curve.handles[1].h2x);
    EXPECT_EQ(-11.0f, definition.desc.curve.handles[1].h2y);

    ASSERT_EQ(1u, image.bindingAttachments.size());
    EXPECT_EQ(harkentest::ObjectId(400u), image.bindingAttachments[0].bindingId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_VOICE),
              image.bindingAttachments[0].scopeKind);
    EXPECT_EQ(harkentest::ObjectId(100u), image.bindingAttachments[0].objectId);
}

TEST(PackV2Format, AttenuationPayloadMatchesOuterVersionExactly) {
    VnAttenuationDesc desc{};
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1;
    desc.maxDistance = 100;
    desc.airAbsorptionNearHz = desc.airAbsorptionFarHz = 20000;
    desc.proximityFrequency = 400;
    desc.highpassCurve.enabled = 1;
    desc.highpassCurve.pointCount = 1;
    desc.highpassCurve.points[0] = {0, 1000};
    for (const auto version : {3u, 4u}) {
        for (const auto size : {607u, 608u, 609u, 791u, 792u, 793u}) {
            auto payload = AttenuationPayload(desc);
            payload.resize(kPackV4AttenuationPayloadBytes);
            SerializePackV2AttenuationCurve(desc.highpassCurve,
                payload.data() + kPackV3AttenuationPayloadBytes);
            payload.resize(size);
            ImageBuilder builder(kPackKindResident);
            builder.AddEntry(kPackEntryKindAttenuation, 500, std::move(payload),
                             kPackV2AuthoredToneSchemaVintage);
            auto bytes = builder.Build();
            StoreLE32(bytes.data() + 4, version);
            PackLogicImage image;
            const bool valid = (version == 3 && size == 608) ||
                               (version == 4 && size == 792);
            ASSERT_EQ(valid ? VN_OK : VN_BAD_PACK,
                      ParsePackV2Image(bytes.data(), bytes.size(), &image))
                << version << "/" << size;
            if (valid) {
                ASSERT_EQ(1u, image.attenuations.size());
                const auto& parsed = image.attenuations[0].desc;
                EXPECT_EQ(version == 3 ? 612u : 796u, parsed.structSize);
                EXPECT_FLOAT_EQ(400, parsed.proximityFrequency);
                EXPECT_EQ(version == 4 ? 1u : 0u, parsed.highpassCurve.enabled);
                EXPECT_FLOAT_EQ(version == 4 ? 1000.0f : 0.0f,
                                parsed.highpassCurve.points[0].y);
            }
        }
    }
}

TEST(PackV2Format, ResidentPackRoundTripCarriesEveryField) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBus, 11, BusPayload(harkentest::ObjectId(11), harkentest::ObjectId(10)));
    builder.AddEntry(kPackEntryKindBusSend, 0, BusSendPayload(harkentest::ObjectId(11), harkentest::ObjectId(10), -6.5f));

    VnAttenuationDesc attenuation{};
    attenuation.rolloff = VN_ROLLOFF_LOGARITHMIC;
    attenuation.minDistance = 1.5f;
    attenuation.maxDistance = 42.0f;
    attenuation.airAbsorptionNearHz = 18000.0f;
    attenuation.airAbsorptionFarHz = 3500.0f;
    attenuation.proximityRadius = 0.75f;
    attenuation.proximityMaxBoostDb = 6.0f;
    attenuation.coneEnabled = 1;
    attenuation.coneInnerDeg = 30.0f;
    attenuation.coneOuterDeg = 120.0f;
    attenuation.coneOuterGainDb = -9.0f;
    attenuation.coneOuterLowpassHz = 1200.0f;
    attenuation.volumeCurve.enabled = 1;
    attenuation.volumeCurve.pointCount = 2;
    attenuation.volumeCurve.points[0] = {1.5f, 0.0f};
    attenuation.volumeCurve.points[1] = {42.0f, -60.0f};
    attenuation.volumeCurve.handles[0] =
        {8.0f, -2.0f, 35.0f, -48.0f};
    builder.AddEntry(kPackEntryKindAttenuation, 500,
                     AttenuationPayload(attenuation),
                     kPackV2AuthoredToneSchemaVintage);


    VnBindingDesc bindingDesc{};
    bindingDesc.variableId = harkentest::ObjectId(920);
    bindingDesc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    bindingDesc.targetId = 0;
    bindingDesc.transitionMs = 100;
    bindingDesc.curve.pointCount = 2;
    bindingDesc.curve.points[0] = {0.0f, 0.0f};
    bindingDesc.curve.points[1] = {1.0f, -12.0f};
    bindingDesc.curve.handles[0] = {0.2f, -1.0f, 0.8f, -10.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 401,
                     BindingDefinitionPayload(harkentest::ObjectId(401), bindingDesc));
    builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                     AttachmentPayload(harkentest::ObjectId(401), VN_TARGET_SCOPE_BUS, 10));

    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));

    EXPECT_EQ(kPackKindResident, image.packKind);
    EXPECT_EQ(6u, image.EntryCount());

    ASSERT_EQ(2u, image.buses.size());
    EXPECT_EQ(harkentest::ObjectId(10u), image.buses[0].busId);
    EXPECT_EQ(harkentest::ObjectId(1u), image.buses[0].parentBusId);
    EXPECT_EQ(0.0f, image.buses[0].volume);
    EXPECT_EQ(0.0f, image.buses[0].outputBusVolume);
    EXPECT_EQ(0.0f, image.buses[0].makeUpGain);
    EXPECT_EQ(harkentest::ObjectId(11u), image.buses[1].busId);
    EXPECT_EQ(harkentest::ObjectId(10u), image.buses[1].parentBusId);
    EXPECT_EQ(0.0f, image.buses[1].volume);
    EXPECT_EQ(0.0f, image.buses[1].outputBusVolume);
    EXPECT_EQ(0.0f, image.buses[1].makeUpGain);

    ASSERT_EQ(1u, image.busSends.size());
    EXPECT_EQ(harkentest::ObjectId(11u), image.busSends[0].fromBusId);
    EXPECT_EQ(harkentest::ObjectId(10u), image.busSends[0].toBusId);
    EXPECT_EQ(-6.5f, image.busSends[0].levelDb);

    ASSERT_EQ(1u, image.attenuations.size());
    EXPECT_EQ(harkentest::ObjectId(500u), image.attenuations[0].id);
    const VnAttenuationDesc& parsed = image.attenuations[0].desc;
    EXPECT_EQ(612u, parsed.structSize);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_ROLLOFF_LOGARITHMIC),
              parsed.rolloff);
    EXPECT_EQ(1.5f, parsed.minDistance);
    EXPECT_EQ(42.0f, parsed.maxDistance);
    EXPECT_EQ(18000.0f, parsed.airAbsorptionNearHz);
    EXPECT_EQ(3500.0f, parsed.airAbsorptionFarHz);
    EXPECT_EQ(0.75f, parsed.proximityRadius);
    EXPECT_EQ(250.0f, parsed.proximityFrequency);
    EXPECT_EQ(6.0f, parsed.proximityMaxBoostDb);
    EXPECT_EQ(1u, parsed.coneEnabled);
    EXPECT_EQ(30.0f, parsed.coneInnerDeg);
    EXPECT_EQ(120.0f, parsed.coneOuterDeg);
    EXPECT_EQ(-9.0f, parsed.coneOuterGainDb);
    EXPECT_EQ(1200.0f, parsed.coneOuterLowpassHz);
    EXPECT_EQ(1u, parsed.volumeCurve.enabled);
    EXPECT_EQ(2u, parsed.volumeCurve.pointCount);
    EXPECT_EQ(1.5f, parsed.volumeCurve.points[0].x);
    EXPECT_EQ(0.0f, parsed.volumeCurve.points[0].y);
    EXPECT_EQ(42.0f, parsed.volumeCurve.points[1].x);
    EXPECT_EQ(-60.0f, parsed.volumeCurve.points[1].y);
    EXPECT_EQ(8.0f, parsed.volumeCurve.handles[0].h1x);
    EXPECT_EQ(-2.0f, parsed.volumeCurve.handles[0].h1y);
    EXPECT_EQ(35.0f, parsed.volumeCurve.handles[0].h2x);
    EXPECT_EQ(-48.0f, parsed.volumeCurve.handles[0].h2y);
    EXPECT_EQ(0u, parsed.lowpassCurve.enabled);
    EXPECT_EQ(0u, parsed.lowpassCurve.pointCount);
    EXPECT_EQ(0u, parsed.widthCurve.enabled);
    EXPECT_EQ(0u, parsed.widthCurve.pointCount);

    ASSERT_EQ(1u, image.bindingDefinitions.size());
    const PackLogicImage::BindingDefinition& definition =
        image.bindingDefinitions[0];
    EXPECT_EQ(harkentest::ObjectId(401u), definition.id);
    EXPECT_EQ(harkentest::ObjectId(920u), definition.desc.variableId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_BUS),
              definition.desc.targetScopeKind);
    EXPECT_EQ(0u, definition.desc.targetId);
    EXPECT_EQ(100u, definition.desc.transitionMs);
    EXPECT_EQ(2u, definition.desc.curve.pointCount);
    EXPECT_EQ(0.0f, definition.desc.curve.points[0].x);
    EXPECT_EQ(0.0f, definition.desc.curve.points[0].y);
    EXPECT_EQ(1.0f, definition.desc.curve.points[1].x);
    EXPECT_EQ(-12.0f, definition.desc.curve.points[1].y);
    EXPECT_EQ(0.2f, definition.desc.curve.handles[0].h1x);
    EXPECT_EQ(-1.0f, definition.desc.curve.handles[0].h1y);
    EXPECT_EQ(0.8f, definition.desc.curve.handles[0].h2x);
    EXPECT_EQ(-10.0f, definition.desc.curve.handles[0].h2y);

    ASSERT_EQ(1u, image.bindingAttachments.size());
    EXPECT_EQ(harkentest::ObjectId(401u), image.bindingAttachments[0].bindingId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_BUS),
              image.bindingAttachments[0].scopeKind);
    EXPECT_EQ(harkentest::ObjectId(10u), image.bindingAttachments[0].objectId);
}

// --- header refusals ---

TEST(PackV2Format, RefusesWrongMagic) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    bytes[0] = 'X';
    ExpectRefused(bytes);
}

TEST(PackV2Format, RefusesOtherSchemaVersions) {
    for (const auto version : {kPackFormatVersion2 - 1,
                               kPackFormatVersion2 + 1}) {
        std::vector<std::uint8_t> bytes = MinimalContentBytes();
        StoreLE32(bytes.data() + 4, version);
        ExpectRefused(bytes);
    }
}

TEST(PackV2Format, RefusesMediaOnlyHeaderLayout) {
    const std::vector<float> planar = {0.0f, 0.25f, 0.5f, 0.75f};
    const std::uint64_t blobOffset =
        AlignUp(kPackHeaderBytes + kPackTocEntryBytes, kPackBlobAlign);
    const std::uint64_t blobBytes = planar.size() * sizeof(float);
    const std::uint64_t fileBytes = blobOffset + blobBytes;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes), 0);
    std::memcpy(bytes.data() + static_cast<std::size_t>(blobOffset),
                planar.data(), static_cast<std::size_t>(blobBytes));
    PackTocEntry toc;
    toc.soundId = harkentest::ObjectId(100);
    toc.sampleRate = 48000;
    toc.channelCount = 1;
    toc.sampleFormat = kPackSampleFormatF32Planar;
    toc.frameCount = planar.size();
    toc.blobOffset = blobOffset;
    toc.blobBytes = blobBytes;
    toc.blobChecksum =
        Fnv1a64(planar.data(), static_cast<std::size_t>(blobBytes));
    SerializePackTocEntry(toc, bytes.data() + kPackHeaderBytes);
    PackHeader header;
    header.formatVersion = kPackFormatVersion;
    header.headerBytes = kPackHeaderBytes;
    header.fileBytes = fileBytes;
    header.entryCount = 1;
    header.tocOffset = kPackHeaderBytes;
    header.tocChecksum =
        Fnv1a64(bytes.data() + kPackHeaderBytes, kPackTocEntryBytes);
    SerializePackHeader(header, bytes.data());

    ExpectRefused(bytes);
}

TEST(PackV2Format, RefusesWrongHeaderBytes) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    StoreLE32(bytes.data() + 8, 43);
    ExpectRefused(bytes);
}

TEST(PackV2Format, RefusesFileBytesMismatch) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    StoreLE64(bytes.data() + 12,
              static_cast<std::uint64_t>(bytes.size()) + 1);
    ExpectRefused(bytes);
}

TEST(PackV2Format, RefusesUnknownPackKind) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    StoreLE32(bytes.data() + 20, 4);
    ExpectRefused(bytes);
}

TEST(PackV2Format, EmptyContentPackRoundTrips) {
    const std::vector<std::uint8_t> bytes =
        ImageBuilder(kPackKindContent).Build();
    ASSERT_EQ(kPackV2HeaderBytes, bytes.size());
    PackLogicImage image;
    ASSERT_EQ(VN_OK,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(kPackKindContent, image.packKind);
    EXPECT_EQ(0u, image.EntryCount());
}

TEST(PackV2Format, EmptyResidentPackRefuses) {
    ExpectRefused(ImageBuilder(kPackKindResident).Build());
}

TEST(PackV2Format, EmptyContentPackRefusesTrailingBytes) {
    std::vector<std::uint8_t> bytes =
        ImageBuilder(kPackKindContent).Build();
    bytes.push_back(0);
    StoreLE64(bytes.data() + 12, bytes.size());
    ExpectRefused(bytes);
}

TEST(PackV2Format, EmptyContentPackRefusesMalformedDirectoryMetadata) {
    const std::vector<std::uint8_t> canonical =
        ImageBuilder(kPackKindContent).Build();
    {
        std::vector<std::uint8_t> bytes = canonical;
        StoreLE64(bytes.data() + 28, kPackV2HeaderBytes - 1);
        ExpectRefused(bytes);
    }
    {
        std::vector<std::uint8_t> bytes = canonical;
        bytes[36] ^= 0x01;
        ExpectRefused(bytes);
    }
}

TEST(PackV2Format, RefusesDirectoryOutOfBounds) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    // One 40-byte record cannot fit in the last 8 bytes of the file.
    StoreLE64(bytes.data() + 28,
              static_cast<std::uint64_t>(bytes.size()) - 8);
    ExpectRefused(bytes);
}

// --- checksum refusals ---

TEST(PackV2Format, RefusesDirectoryChecksumMismatch) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    bytes[kPackV2HeaderBytes] ^= 0x01;
    ExpectRefused(bytes);
}

TEST(PackV2Format, RefusesPayloadChecksumMismatch) {
    ImageBuilder builder(kPackKindContent);
    const std::size_t index =
        builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    std::vector<std::uint8_t> bytes = builder.Build();
    bytes[static_cast<std::size_t>(builder.PayloadOffset(index))] ^= 0x01;
    ExpectRefused(bytes);
}

TEST(PackV2Format, RefusesMediaBlobCorruption) {
    ImageBuilder builder(kPackKindContent);
    const std::size_t index =
        builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    std::vector<std::uint8_t> bytes = builder.Build();
    // The directory checksum covers the payload record, not the blob;
    // only the blob's own checksum can catch this flip.
    bytes[static_cast<std::size_t>(builder.BlobOffset(index))] ^= 0x01;
    ExpectRefused(bytes);
}

TEST(PackV2Format, PayloadChecksumGuardsWireLegalCorruption) {
    // One valid resident image carrying an attenuation (minDistance =
    // 1.0f) and a bus-scope binding definition; both corruption
    // variants below start from these intact bytes and locate their
    // targets by parsing the built directory, never by builder
    // bookkeeping.
    ImageBuilder builder(kPackKindResident);
    VnAttenuationDesc attenuation{};
    attenuation.rolloff = VN_ROLLOFF_LINEAR;
    attenuation.minDistance = 1.0f;
    attenuation.maxDistance = 100.0f;
    attenuation.airAbsorptionNearHz = 20000.0f;
    attenuation.airAbsorptionFarHz = 20000.0f;
    builder.AddEntry(kPackEntryKindAttenuation, 500,
                     AttenuationPayload(attenuation),
                     kPackV2AuthoredToneSchemaVintage);
    VnBindingDesc bindingDesc{};
    bindingDesc.variableId = harkentest::ObjectId(920);
    bindingDesc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    bindingDesc.targetId = 0;
    bindingDesc.curve.pointCount = 2;
    bindingDesc.curve.points[0] = {0.0f, 0.0f};
    bindingDesc.curve.points[1] = {1.0f, -12.0f};
    bindingDesc.curve.handles[0] =
        {0.0f, 0.0f, 1.0f, -12.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 401,
                     BindingDefinitionPayload(harkentest::ObjectId(401), bindingDesc));
    const std::vector<std::uint8_t> intact = builder.Build();
    {
        PackLogicImage image;
        ASSERT_EQ(VN_OK,
                  ParsePackV2Image(intact.data(), intact.size(), &image));
    }

    // Flip the LOWEST bit of the attenuation's minDistance float
    // (payload offset 4): 1.0f becomes 1.0000001f — still finite,
    // still a legal wire value in a live scalar field, with every
    // reserved word and zero-fill tail untouched, so the payload parse
    // accepts the record; the directory checksum covers directory
    // bytes only and cannot see it. The per-entry payload checksum
    // compare is the SOLE check that can refuse this file.
    {
        std::vector<std::uint8_t> bytes = intact;
        const std::size_t offset = static_cast<std::size_t>(
            PayloadOffsetOfKind(bytes, kPackEntryKindAttenuation));
        bytes[offset + 4] ^= 0x01;
        ExpectRefused(bytes);
    }
    // The same shape on the binding definition's curve: point[0].y
    // lives at payload offset 32 (curve start) + 8. Its 0.0f becomes
    // the smallest denormal — a finite float in a LIVE point slot
    // (pointCount = 2), so the wire strictness rules (exact sizes,
    // reserved word, unused-slot zero fill) all still pass and only
    // the payload checksum can refuse.
    {
        std::vector<std::uint8_t> bytes = intact;
        const std::size_t offset = static_cast<std::size_t>(
            PayloadOffsetOfKind(bytes, kPackEntryKindBindingDefinition));
        bytes[offset + 32 + 8] ^= 0x01;
        ExpectRefused(bytes);
    }
}

// --- unknown kind / vintage refusals ---

TEST(PackV2Format, RefusesUnknownEntryKind) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(99, 1, std::vector<std::uint8_t>(8, 0));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesUnknownVintage) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)), 3);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(0u, image.EntryCount());
}

TEST(PackV2Format, RefusesPreviousSchemaVintage) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)), 1);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(0u, image.EntryCount());
}

TEST(PackV2Format, Vintage2BusPayloadParsesAsZeroMix) {
    PackBusEntry parsed{};
    parsed.volume = 1.0f;
    parsed.outputBusVolume = 2.0f;
    parsed.makeUpGain = 3.0f;
    const std::vector<std::uint8_t> payload = BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1));
    ASSERT_TRUE(ParsePackV2Bus(payload.data(), payload.size(), &parsed));
    EXPECT_EQ(harkentest::ObjectId(10u), parsed.busId);
    EXPECT_EQ(harkentest::ObjectId(1u), parsed.parentBusId);
    EXPECT_EQ(0.0f, parsed.volume);
    EXPECT_EQ(0.0f, parsed.outputBusVolume);
    EXPECT_EQ(0.0f, parsed.makeUpGain);
    EXPECT_FALSE(ParsePackV2Bus(payload.data(), kPackV6BusPayloadBytes, &parsed));
}

TEST(PackV2Format, Vintage6BusPayloadRoundTripsMixFields) {
    const std::vector<std::uint8_t> payload =
        BusMixPayload(harkentest::ObjectId(10), harkentest::ObjectId(1), -6.0f, -3.5f, 2.0f);
    ASSERT_EQ(kPackV6BusPayloadBytes, payload.size());
    EXPECT_EQ(0u, LoadLE32(payload.data() + 44));

    PackBusEntry parsed{};
    ASSERT_TRUE(ParsePackV6Bus(payload.data(), payload.size(), &parsed));
    EXPECT_EQ(harkentest::ObjectId(10u), parsed.busId);
    EXPECT_EQ(harkentest::ObjectId(1u), parsed.parentBusId);
    EXPECT_EQ(-6.0f, parsed.volume);
    EXPECT_EQ(-3.5f, parsed.outputBusVolume);
    EXPECT_EQ(2.0f, parsed.makeUpGain);
    EXPECT_FALSE(ParsePackV6Bus(payload.data(), kPackV2BusPayloadBytes, &parsed));
}

TEST(PackV2Format, Vintage6BusImageRoundTripCarriesMixFields) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10,
                     BusMixPayload(harkentest::ObjectId(10), harkentest::ObjectId(1), -6.0f, -3.5f, 2.0f),
                     kPackV2BusMixSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    ASSERT_EQ(1u, image.buses.size());
    EXPECT_EQ(harkentest::ObjectId(10u), image.buses[0].busId);
    EXPECT_EQ(harkentest::ObjectId(1u), image.buses[0].parentBusId);
    EXPECT_EQ(-6.0f, image.buses[0].volume);
    EXPECT_EQ(-3.5f, image.buses[0].outputBusVolume);
    EXPECT_EQ(2.0f, image.buses[0].makeUpGain);
}

TEST(PackV2Format, RefusesVintage6BusReservedNonzero) {
    std::vector<std::uint8_t> payload =
        BusMixPayload(harkentest::ObjectId(10), harkentest::ObjectId(1), -6.0f, -3.5f, 2.0f);
    StoreLE32(payload.data() + 44, 1);
    PackBusEntry parsed{};
    EXPECT_FALSE(ParsePackV6Bus(payload.data(), payload.size(), &parsed));

    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, std::move(payload),
                     kPackV2BusMixSchemaVintage);
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesBusVintageAndPayloadSizeMismatch) {
    {
        ImageBuilder builder(kPackKindResident);
        builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)),
                         kPackV2BusMixSchemaVintage);
        ExpectRefused(builder.Build());
    }
    {
        ImageBuilder builder(kPackKindResident);
        builder.AddEntry(kPackEntryKindBus, 10,
                         BusMixPayload(harkentest::ObjectId(10), harkentest::ObjectId(1), -6.0f, 0.0f, 0.0f));
        ExpectRefused(builder.Build());
    }
}

TEST(PackV2Format, RefusesBusMixVintageOnOtherKinds) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBusSend, 0, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(11), 0.0f),
                     kPackV2BusMixSchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(0u, image.EntryCount());
}

TEST(PackV2Format, RetiredAttenuationVintageRefuses) {
    VnAttenuationDesc desc{};
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = 100.0f;
    desc.airAbsorptionNearHz = 20000.0f;
    desc.airAbsorptionFarHz = 20000.0f;
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindAttenuation, 500,
                     RetiredAttenuationPayload(desc), kPackV2SchemaVintage);
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_PACK_INCOMPATIBLE,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(0u, image.EntryCount());
}

// Current layout id with a retired payload size is damage, not a second
// table: vintage 5 still owes the v5 byte count, vintage 3 still owes 608.
TEST(PackV2Format, CurrentVintageWithRetiredPayloadSizeRefuses) {
    {
        PackVoiceRecipeEntryHeader header;
        header.playableId = harkentest::ObjectId(100);
        header.installCount = 1;
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(
            PackV2VoiceRecipePayloadBytes(header.installCount, 0)));
        SerializePackV2VoiceRecipeHeader(header, payload.data());
        PackVoiceRecipeInstall install;
        install.slot = 0;
        install.effectType = VN_EFFECT_GAIN;
        install.descriptorVintage = kPackV2EffectDescriptorVintage;
        install.paramCount = 1;
        install.params[0] = -6.0f;
        SerializePackV2VoiceRecipeInstall(
            install, payload.data() + kPackV2VoiceRecipeHeaderBytes);
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindVoiceRecipe, 100, std::move(payload),
                         kPackV2PlaybackRecipeSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
        EXPECT_EQ(0u, image.EntryCount());
    }
    {
        VnAttenuationDesc desc{};
        desc.rolloff = VN_ROLLOFF_LINEAR;
        desc.minDistance = 1.0f;
        desc.maxDistance = 100.0f;
        desc.airAbsorptionNearHz = 20000.0f;
        desc.airAbsorptionFarHz = 20000.0f;
        ImageBuilder builder(kPackKindResident);
        builder.AddEntry(kPackEntryKindAttenuation, 500,
                         RetiredAttenuationPayload(desc),
                         kPackV2AuthoredToneSchemaVintage);
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        EXPECT_EQ(VN_BAD_PACK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
        EXPECT_EQ(0u, image.EntryCount());
    }
}

// --- pack-kind discipline refusals ---

TEST(PackV2Format, RefusesMediaInResidentPack) {
    ImageBuilder builder(kPackKindResident);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesContainerInResidentPack) {
    ImageBuilder builder(kPackKindResident);
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(200);
    header.childCount = 1;
    std::vector<PackContainerChildV2> children(1);
    children[0].playableId = harkentest::ObjectId(100);
    children[0].weight = 1;
    builder.AddEntry(kPackEntryKindContainer, 200,
                     ContainerPayload(header, children));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesBusInContentPack) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesLegacyVariableKindsInEveryPackKind) {
    for (const std::uint32_t packKind : {kPackKindContent,
                                         kPackKindResident}) {
        ImageBuilder variable(packKind);
        variable.AddEntry(kPackEntryKindDiscreteVariable, 920,
                          DiscreteVariablePayload(harkentest::ObjectId(920)));
        ExpectRefused(variable.Build());

        ImageBuilder meter(packKind);
        meter.AddEntry(kPackEntryKindMeterWiring, 10,
                       MeterWiringPayload(harkentest::ObjectId(10), harkentest::ObjectId(920), 10, 200));
        ExpectRefused(meter.Build());
    }
}

TEST(PackV2Format, RefusesVoiceScopeBindingDefinitionInResidentPack) {
    ImageBuilder builder(kPackKindResident);
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(920);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 0.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 401,
                     BindingDefinitionPayload(harkentest::ObjectId(401), desc));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesVoiceScopeAttachmentInResidentPack) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                     AttachmentPayload(harkentest::ObjectId(401), VN_TARGET_SCOPE_VOICE, 100));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, AcceptsKnownEffectParamBindingDefinitions) {
    for (const std::uint32_t packKind :
         {kPackKindContent, kPackKindResident}) {
        for (const std::uint32_t scope :
             {VN_TARGET_SCOPE_VOICE_EFFECT_PARAM,
              VN_TARGET_SCOPE_BUS_EFFECT_PARAM}) {
            if (packKind == kPackKindResident &&
                scope == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
                continue;
            }
            ImageBuilder builder(packKind);
            VnBindingDesc desc{};
            desc.variableId = harkentest::ObjectId(920);
            desc.targetScopeKind = scope;
            desc.targetId = PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0);
            desc.curve.pointCount = 1;
            desc.curve.points[0] = {0.0f, 0.0f};
            builder.AddEntry(kPackEntryKindBindingDefinition, 402,
                             BindingDefinitionPayload(harkentest::ObjectId(402), desc));
            const std::vector<std::uint8_t> bytes = builder.Build();
            PackLogicImage image;
            ASSERT_EQ(VN_OK,
                      ParsePackV2Image(bytes.data(), bytes.size(), &image));
            ASSERT_EQ(1u, image.bindingDefinitions.size());
            EXPECT_EQ(scope,
                      image.bindingDefinitions.front().desc.targetScopeKind);
        }
    }
}

TEST(PackV2Format, RefusesVoiceEffectParamBindingDefinitionInResidentPack) {
    ImageBuilder builder(kPackKindResident);
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(920);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    desc.targetId = PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0);
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 0.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 402,
                     BindingDefinitionPayload(harkentest::ObjectId(402), desc));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesFutureBindingDefinitionScope) {
    ImageBuilder builder(kPackKindContent);
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(920);
    desc.targetScopeKind = 4;
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 0.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 402,
                     BindingDefinitionPayload(harkentest::ObjectId(402), desc));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, AcceptsBusEffectParamAttachmentsAndRefusesVoiceEffectParam) {
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                         AttachmentPayload(
                             harkentest::ObjectId(402), VN_TARGET_SCOPE_VOICE_EFFECT_PARAM, 1));
        ExpectRefused(builder.Build());
    }
    for (const std::uint32_t packKind :
         {kPackKindContent, kPackKindResident}) {
        ImageBuilder builder(packKind);
        builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                         AttachmentPayload(
                             harkentest::ObjectId(402), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, 1));
        const std::vector<std::uint8_t> bytes = builder.Build();
        PackLogicImage image;
        ASSERT_EQ(VN_OK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
        ASSERT_EQ(1u, image.bindingAttachments.size());
        EXPECT_EQ(VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                  image.bindingAttachments.front().scopeKind);
        EXPECT_EQ(harkentest::ObjectId(1u), image.bindingAttachments.front().objectId);
    }
}

// --- directory id agreement refusals ---

TEST(PackV2Format, RefusesBusIdDisagreeingWithDirectory) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 11, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesMediaSoundIdDisagreeingWithDirectory) {
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(101, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesBusSendOrdinalMismatch) {
    ImageBuilder builder(kPackKindResident);
    // The first send edge must carry ordinal directory id 0.
    builder.AddEntry(kPackEntryKindBusSend, 1, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(11), 0.0f));
    ExpectRefused(builder.Build());
}

// --- duplicate refusals ---

TEST(PackV2Format, RefusesDuplicateBusIds) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesDuplicateMediaIds) {
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesMediaIdCollidingWithContainerId) {
    // Media and containers share the flat playable-ID space, so the
    // same id on one of each is a duplicate even across kinds.
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(100);
    header.childCount = 1;
    std::vector<PackContainerChildV2> children(1);
    children[0].playableId = harkentest::ObjectId(5);
    children[0].weight = 1;
    builder.AddEntry(kPackEntryKindContainer, 100,
                     ContainerPayload(header, children));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesDuplicateSendEdge) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBus, 11, BusPayload(harkentest::ObjectId(11), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBusSend, 0, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(11), 0.0f));
    builder.AddEntry(kPackEntryKindBusSend, 1, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(11), -3.0f));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesSelfSend) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBusSend, 0, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(10), 0.0f));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesDuplicateAttachmentTriple) {
    ImageBuilder builder(kPackKindContent);
    builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                     AttachmentPayload(harkentest::ObjectId(400), VN_TARGET_SCOPE_VOICE, 100));
    builder.AddEntry(kPackEntryKindBindingAttachment, 1,
                     AttachmentPayload(harkentest::ObjectId(400), VN_TARGET_SCOPE_VOICE, 100));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesMeterWiringsSharingBus) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindMeterWiring, 10,
                     MeterWiringPayload(harkentest::ObjectId(10), harkentest::ObjectId(920), 10, 200));
    builder.AddEntry(kPackEntryKindMeterWiring, 10,
                     MeterWiringPayload(harkentest::ObjectId(10), harkentest::ObjectId(921), 10, 200));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesMeterWiringsSharingVariable) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindMeterWiring, 10,
                     MeterWiringPayload(harkentest::ObjectId(10), harkentest::ObjectId(920), 10, 200));
    builder.AddEntry(kPackEntryKindMeterWiring, 11,
                     MeterWiringPayload(harkentest::ObjectId(11), harkentest::ObjectId(920), 10, 200));
    ExpectRefused(builder.Build());
}

// --- wire strictness refusals ---
// These corrupt the payload BEFORE Build, so every checksum still
// matches and the refusal can only come from the wire parse itself.

TEST(PackV2Format, RefusesBusSendReservedNonzero) {
    ImageBuilder builder(kPackKindResident);
    std::vector<std::uint8_t> payload = BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(11), 0.0f);
    payload[36] = 1;
    builder.AddEntry(kPackEntryKindBusSend, 0, std::move(payload));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesBindingDefinitionReservedNonzero) {
    ImageBuilder builder(kPackKindContent);
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(910);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 0.0f};
    std::vector<std::uint8_t> payload = BindingDefinitionPayload(harkentest::ObjectId(400), desc);
    payload[44] = 1;
    builder.AddEntry(kPackEntryKindBindingDefinition, 400,
                     std::move(payload));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesAttenuationHandleTailNonzero) {
    ImageBuilder builder(kPackKindResident);
    VnAttenuationDesc desc{};
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = 100.0f;
    desc.airAbsorptionNearHz = 20000.0f;
    desc.airAbsorptionFarHz = 20000.0f;
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 2;
    desc.volumeCurve.points[0] = {0.0f, 0.0f};
    desc.volumeCurve.points[1] = {100.0f, -96.0f};
    desc.volumeCurve.handles[0] = {0.0f, 0.0f, 100.0f, -96.0f};
    std::vector<std::uint8_t> payload = AttenuationPayload(desc);
    // Curve starts at 48; handles start at 72; slot 1 is the first tail slot.
    StoreLEF32(payload.data() + 48 + 72 + 16, 1.0f);
    builder.AddEntry(kPackEntryKindAttenuation, 500, std::move(payload),
                     kPackV2AuthoredToneSchemaVintage);
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesBindingHandleTailNegativeZero) {
    ImageBuilder builder(kPackKindContent);
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(910);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 1.0f};
    desc.curve.handles[0] = {0.0f, 0.0f, 1.0f, 1.0f};
    std::vector<std::uint8_t> payload = BindingDefinitionPayload(harkentest::ObjectId(400), desc);
    StoreLE32(payload.data() + 48 + 68 + 16, 0x80000000u);
    builder.AddEntry(kPackEntryKindBindingDefinition, 400,
                     std::move(payload));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesLegacyActionTombstoneForAnyPayload) {
    for (std::uint32_t packKind : {kPackKindResident, kPackKindContent}) {
        for (std::size_t payloadBytes : {std::size_t{1}, std::size_t{64}}) {
            ImageBuilder builder(packKind);
            builder.AddEntry(kPackEntryKindAction, 300,
                             std::vector<std::uint8_t>(payloadBytes, 0xA5));
            ExpectRefused(builder.Build());
        }
    }
}

TEST(PackV2Format, RefusesContainerChildCountDisagreeingWithSize) {
    ImageBuilder builder(kPackKindContent);
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(200);
    header.childCount = 3; // payload carries only two children
    std::vector<PackContainerChildV2> children(2);
    children[0].playableId = harkentest::ObjectId(100);
    children[0].weight = 1;
    children[1].playableId = harkentest::ObjectId(101);
    children[1].weight = 1;
    builder.AddEntry(kPackEntryKindContainer, 200,
                     ContainerPayload(header, children));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, RefusesZeroChildContainer) {
    ImageBuilder builder(kPackKindContent);
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(200);
    header.childCount = 0;
    builder.AddEntry(kPackEntryKindContainer, 200,
                     ContainerPayload(header, {}));
    ExpectRefused(builder.Build());
}

TEST(PackV2Format, NonFiniteSendLevelRefused) {
    // Live connect verbs refuse non-finite levels at the call; the pack
    // path applies levels without that gate, so a checksum-correct
    // NaN/Inf must die at the image parse. The payload is assembled with
    // the exact bit pattern written over a finite level BEFORE Build, so
    // every checksum matches and the wire shape stays legal — only the
    // finiteness rule can refuse.
    const auto buildWithLevelBits = [](std::uint32_t levelBits) {
        ImageBuilder builder(kPackKindResident);
        builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
        std::vector<std::uint8_t> payload = BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(1), 0.0f);
        StoreLE32(payload.data() + 32, levelBits);
        builder.AddEntry(kPackEntryKindBusSend, 0, std::move(payload));
        return builder.Build();
    };
    // The finite twin proves the surrounding shape parses clean.
    {
        const std::vector<std::uint8_t> finite =
            buildWithLevelBits(0x00000000u); // +0.0f
        PackLogicImage image;
        ASSERT_EQ(VN_OK,
                  ParsePackV2Image(finite.data(), finite.size(), &image));
        ASSERT_EQ(1u, image.busSends.size());
        EXPECT_EQ(0.0f, image.busSends[0].levelDb);
    }
    ExpectRefused(buildWithLevelBits(0x7FC00000u)); // quiet NaN
    ExpectRefused(buildWithLevelBits(0x7F800000u)); // +infinity
}

TEST(PackV2Format, NonFiniteBusMixSumRefused) {
    const auto buildWithMix = [](float volume, float outputBusVolume,
                                 float makeUpGain) {
        ImageBuilder builder(kPackKindResident);
        builder.AddEntry(kPackEntryKindBus, 10,
                         BusMixPayload(harkentest::ObjectId(10), harkentest::ObjectId(1), volume, outputBusVolume,
                                       makeUpGain),
                         kPackV2BusMixSchemaVintage);
        return builder.Build();
    };
    {
        const std::vector<std::uint8_t> finite =
            buildWithMix(-6.0f, -3.5f, 2.0f);
        PackLogicImage image;
        ASSERT_EQ(VN_OK,
                  ParsePackV2Image(finite.data(), finite.size(), &image));
        ASSERT_EQ(1u, image.buses.size());
        EXPECT_EQ(-6.0f, image.buses[0].volume);
        EXPECT_EQ(-3.5f, image.buses[0].outputBusVolume);
        EXPECT_EQ(2.0f, image.buses[0].makeUpGain);
    }
    ExpectRefused(buildWithMix(3.0e38f, 3.0e38f, 3.0e38f));
}

// --- layout normalization refusals ---
// Each case rebuilds a valid image into a layout-illegal one while
// keeping every other fact consistent (RecomputeChecksums repairs both
// checksum layers after the byte surgery), so the refusal can only come
// from the layout rule under test.

TEST(PackV2Format, MisalignedPayloadRefused) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    {
        PackLogicImage image;
        ASSERT_EQ(VN_OK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    PackV2Header header;
    ASSERT_TRUE(ParsePackV2Header(bytes.data(), &header));
    ASSERT_EQ(1u, header.entryCount);
    std::uint8_t* record = bytes.data() + header.directoryOffset;
    PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(record);
    // Move the single payload 4 bytes forward: pad inserted at its old
    // offset, file length grown, the directory restated, both checksum
    // layers recomputed. The moved payload bytes are untouched, so the
    // only remaining defect is the 8-alignment rule.
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                 4, 0);
    entry.offset += 4;
    SerializePackV2DirectoryEntry(entry,
                                  bytes.data() + header.directoryOffset);
    header.fileBytes = bytes.size();
    SerializePackV2Header(header, bytes.data());
    RecomputeChecksums(&bytes);
    ExpectRefused(bytes);
}

TEST(PackV2Format, DirectoryGapRefused) {
    std::vector<std::uint8_t> bytes = MinimalContentBytes();
    {
        PackLogicImage image;
        ASSERT_EQ(VN_OK,
                  ParsePackV2Image(bytes.data(), bytes.size(), &image));
    }
    PackV2Header header;
    ASSERT_TRUE(ParsePackV2Header(bytes.data(), &header));
    ASSERT_EQ(1u, header.entryCount);
    // Slide the directory (and everything after it) 8 bytes down so it
    // sits at offset 52 instead of the header's end at 44. Every offset
    // is restated and both checksum layers recomputed — payloads stay
    // 8-aligned — so the refusal is the directory-anchoring rule alone.
    bytes.insert(bytes.begin() +
                     static_cast<std::ptrdiff_t>(kPackV2HeaderBytes),
                 8, 0);
    header.directoryOffset = kPackV2HeaderBytes + 8;
    header.fileBytes = bytes.size();
    SerializePackV2Header(header, bytes.data());
    std::uint8_t* record = bytes.data() + header.directoryOffset;
    PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(record);
    entry.offset += 8;
    SerializePackV2DirectoryEntry(entry, record);
    RecomputeChecksums(&bytes);
    ExpectRefused(bytes);
}

TEST(PackV2Format, OverlappingPayloadsRefused) {
    // Two directory records aliasing the SAME payload range: the second
    // attachment's record is re-pointed at the first's bytes (ordinal
    // ids stay distinct, checksums recomputed over the shared range).
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                         AttachmentPayload(harkentest::ObjectId(400), VN_TARGET_SCOPE_VOICE, 100));
        builder.AddEntry(kPackEntryKindBindingAttachment, 1,
                         AttachmentPayload(harkentest::ObjectId(400), VN_TARGET_SCOPE_VOICE, 101));
        std::vector<std::uint8_t> bytes = builder.Build();
        {
            PackLogicImage image;
            ASSERT_EQ(VN_OK,
                      ParsePackV2Image(bytes.data(), bytes.size(), &image));
        }
        PackV2Header header;
        ASSERT_TRUE(ParsePackV2Header(bytes.data(), &header));
        std::uint8_t* second = bytes.data() + header.directoryOffset +
                               kPackV2DirectoryEntryBytes;
        PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(second);
        entry.offset = builder.PayloadOffset(0);
        SerializePackV2DirectoryEntry(entry, second);
        RecomputeChecksums(&bytes);
        ExpectRefused(bytes);
    }
    // Two media entries sharing one blob range — the shape ONLY the
    // overlap sweep can catch: the planar bytes are identical, so the
    // re-pointed blobOffset still matches its blob checksum, both TOC
    // payloads are wire-legal at distinct offsets, and the sound ids
    // stay unique. Without the sweep this file would parse as two
    // sounds silently deduplicated onto one blob.
    {
        ImageBuilder builder(kPackKindContent);
        builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
        builder.AddMedia(101, harkentest::ObjectId(101), 2, SmallStereoPlanar());
        std::vector<std::uint8_t> bytes = builder.Build();
        {
            PackLogicImage image;
            ASSERT_EQ(VN_OK,
                      ParsePackV2Image(bytes.data(), bytes.size(), &image));
        }
        const std::size_t secondToc =
            static_cast<std::size_t>(builder.PayloadOffset(1));
        StoreLE64(bytes.data() + secondToc + 32, builder.BlobOffset(0));
        RecomputeChecksums(&bytes);
        ExpectRefused(bytes);
    }
}

// --- closure ---

struct KnownIds {
    std::set<VnObjectId> playables;
    std::set<VnObjectId> buses;
    std::set<VnObjectId> bindings;
};

bool KnownPlayable(void* ctx, VnObjectId id) {
    return static_cast<const KnownIds*>(ctx)->playables.count(id) != 0;
}
bool KnownBus(void* ctx, VnBusId id) {
    return static_cast<const KnownIds*>(ctx)->buses.count(id) != 0;
}
bool KnownBinding(void* ctx, VnBindingId id) {
    return static_cast<const KnownIds*>(ctx)->bindings.count(id) != 0;
}
PackClosureResolver ResolverOver(KnownIds* ids) {
    PackClosureResolver resolver;
    resolver.ctx = ids;
    resolver.playableExists = KnownPlayable;
    resolver.busExists = KnownBus;
    resolver.bindingExists = KnownBinding;
    return resolver;
}

void ExpectClosureFailure(const PackLogicImage& image,
                          const PackClosureResolver& resolver,
                          PackClosureRef expectedClass,
                          std::uint64_t expectedReferrer,
                          std::uint64_t expectedMissing) {
    PackClosureRef refClass = static_cast<PackClosureRef>(0);
    VnObjectId referrer{UINT64_MAX, UINT64_MAX};
    VnObjectId missing{UINT64_MAX, UINT64_MAX};
    EXPECT_EQ(VN_UNKNOWN_ID, CheckPackClosure(image, resolver, &refClass,
                                              &referrer, &missing));
    EXPECT_EQ(expectedClass, refClass);
    EXPECT_EQ(harkentest::ObjectId(expectedReferrer), referrer);
    EXPECT_EQ(harkentest::ObjectId(expectedMissing), missing);
}

// Content image whose references are all internal: media 100 and container 200
// with media 100 as its child.
void BuildAllInternalContentImage(PackLogicImage* out) {
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(200);
    header.childCount = 1;
    std::vector<PackContainerChildV2> children(1);
    children[0].playableId = harkentest::ObjectId(100);
    children[0].weight = 1;
    builder.AddEntry(kPackEntryKindContainer, 200,
                     ContainerPayload(header, children));
    const std::vector<std::uint8_t> bytes = builder.Build();
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), out));
}

TEST(PackV2Closure, AllInternalContentPackCloses) {
    PackLogicImage image;
    BuildAllInternalContentImage(&image);
    KnownIds ids;
    // Null out parameters must be tolerated on success.
    EXPECT_EQ(VN_OK, CheckPackClosure(image, ResolverOver(&ids), nullptr,
                                      nullptr, nullptr));
}

// Attachment binding known only to the resolver.

void BuildAttachmentBindingImage(PackLogicImage* out) {
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                     AttachmentPayload(harkentest::ObjectId(777), VN_TARGET_SCOPE_VOICE, 100));
    const std::vector<std::uint8_t> bytes = builder.Build();
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), out));
}

TEST(PackV2Closure, AttachmentBindingResolvesThroughResolver) {
    PackLogicImage image;
    BuildAttachmentBindingImage(&image);
    KnownIds ids;
    ids.bindings.insert(harkentest::ObjectId(777));
    EXPECT_EQ(VN_OK, CheckPackClosure(image, ResolverOver(&ids), nullptr,
                                      nullptr, nullptr));
}

TEST(PackV2Closure, MissingAttachmentBindingReportsTriple) {
    PackLogicImage image;
    BuildAttachmentBindingImage(&image);
    KnownIds ids; // empty: nothing resolves externally
    ExpectClosureFailure(image, ResolverOver(&ids),
                         PackClosureRef::AttachmentBinding, 777, 777);
}

// Send edge to a bus known only to the resolver.

void BuildSendToEngineBusImage(PackLogicImage* out) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBusSend, 0, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(999), 0.0f));
    const std::vector<std::uint8_t> bytes = builder.Build();
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), out));
}

TEST(PackV2Closure, SendToEngineBusResolvesThroughResolver) {
    PackLogicImage image;
    BuildSendToEngineBusImage(&image);
    KnownIds ids;
    ids.buses.insert(harkentest::ObjectId(1)); // bus 10's parent, checked before the send
    ids.buses.insert(harkentest::ObjectId(999));
    EXPECT_EQ(VN_OK, CheckPackClosure(image, ResolverOver(&ids), nullptr,
                                      nullptr, nullptr));
}

TEST(PackV2Closure, MissingSendBusReportsTriple) {
    PackLogicImage image;
    BuildSendToEngineBusImage(&image);
    // The resolver knows bus 10's parent — parents are checked first, so
    // this failure isolates the send edge's target.
    KnownIds ids;
    ids.buses.insert(harkentest::ObjectId(1));
    ExpectClosureFailure(image, ResolverOver(&ids), PackClosureRef::SendBus,
                         10, 999);
}

// Bus parents are logic references and close first, before every other
// reference class.

TEST(PackV2Closure, BusParentClosesOverPackBus) {
    // Bus 11's parent is pack bus 10, resolved in-pack: the resolver
    // knows only the root's parent (master 1), never bus 10 itself. A
    // fully deny-all run cannot close any acyclic bus chain — its root
    // parent always lives outside the pack.
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindBus, 11, BusPayload(harkentest::ObjectId(11), harkentest::ObjectId(10)));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    KnownIds ids;
    ids.buses.insert(harkentest::ObjectId(1));
    EXPECT_EQ(VN_OK, CheckPackClosure(image, ResolverOver(&ids), nullptr,
                                      nullptr, nullptr));
}

TEST(PackV2Closure, BusParentClosesThroughResolver) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(999)));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    KnownIds ids;
    ids.buses.insert(harkentest::ObjectId(999));
    EXPECT_EQ(VN_OK, CheckPackClosure(image, ResolverOver(&ids), nullptr,
                                      nullptr, nullptr));
}

TEST(PackV2Closure, MissingBusParentReportsTriple) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(999)));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    // The referrer is the child bus itself; the missing id its parent.
    ExpectClosureFailure(image, PackClosureResolver{},
                         PackClosureRef::BusParent, 10, 999);
}

TEST(PackV2Closure, BusParentCheckedBeforeSends) {
    // Both the parent and the send target are unresolvable; the parent
    // failure must win because parents are checked first.
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(999)));
    builder.AddEntry(kPackEntryKindBusSend, 0, BusSendPayload(harkentest::ObjectId(10), harkentest::ObjectId(888), 0.0f));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), &image));
    ExpectClosureFailure(image, PackClosureResolver{},
                         PackClosureRef::BusParent, 10, 999);
}

// Container child known only to the resolver.

void BuildContainerChildImage(PackLogicImage* out) {
    ImageBuilder builder(kPackKindContent);
    PackContainerEntryHeader header;
    header.containerId = harkentest::ObjectId(200);
    header.childCount = 1;
    std::vector<PackContainerChildV2> children(1);
    children[0].playableId = harkentest::ObjectId(555);
    children[0].weight = 1;
    builder.AddEntry(kPackEntryKindContainer, 200,
                     ContainerPayload(header, children));
    const std::vector<std::uint8_t> bytes = builder.Build();
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), out));
}

TEST(PackV2Closure, ContainerChildResolvesThroughResolver) {
    PackLogicImage image;
    BuildContainerChildImage(&image);
    KnownIds ids;
    ids.playables.insert(harkentest::ObjectId(555));
    EXPECT_EQ(VN_OK, CheckPackClosure(image, ResolverOver(&ids), nullptr,
                                      nullptr, nullptr));
}

TEST(PackV2Closure, MissingContainerChildReportsTriple) {
    PackLogicImage image;
    BuildContainerChildImage(&image);
    KnownIds ids;
    ExpectClosureFailure(image, ResolverOver(&ids),
                         PackClosureRef::ContainerChild, 200, 555);
}

// VOICE attachment objects close over the playable space.

void BuildVoiceAttachmentImage(std::uint64_t objectId, PackLogicImage* out) {
    ImageBuilder builder(kPackKindContent);
    builder.AddMedia(100, harkentest::ObjectId(100), 2, SmallStereoPlanar());
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(910);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 0.0f};
    builder.AddEntry(kPackEntryKindBindingDefinition, 400,
                     BindingDefinitionPayload(harkentest::ObjectId(400), desc));
    builder.AddEntry(kPackEntryKindBindingAttachment, 0,
                     AttachmentPayload(harkentest::ObjectId(400), VN_TARGET_SCOPE_VOICE, objectId));
    const std::vector<std::uint8_t> bytes = builder.Build();
    ASSERT_EQ(VN_OK, ParsePackV2Image(bytes.data(), bytes.size(), out));
}

TEST(PackV2Closure, VoiceAttachmentClosesOverPackMedia) {
    PackLogicImage image;
    BuildVoiceAttachmentImage(100, &image);
    EXPECT_EQ(VN_OK, CheckPackClosure(image, PackClosureResolver{}, nullptr,
                                      nullptr, nullptr));
}

TEST(PackV2Closure, VoiceAttachmentUnknownPlayableReportsTriple) {
    PackLogicImage image;
    BuildVoiceAttachmentImage(999, &image);
    ExpectClosureFailure(image, PackClosureResolver{},
                         PackClosureRef::AttachmentPlayable, 400, 999);
}

// Meter wirings close over buses.

TEST(PackV2Closure, MeterWiringEntryIsRejectedWithoutImageSideEffects) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindBus, 10, BusPayload(harkentest::ObjectId(10), harkentest::ObjectId(1)));
    builder.AddEntry(kPackEntryKindMeterWiring, 10,
                     MeterWiringPayload(harkentest::ObjectId(10), harkentest::ObjectId(920), 10, 200));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_TRUE(image.buses.empty());
    EXPECT_EQ(0u, image.EntryCount());
}

TEST(PackV2Closure,
     MeterWiringEntryWithMissingBusIsRejectedBeforeClosure) {
    ImageBuilder builder(kPackKindResident);
    builder.AddEntry(kPackEntryKindMeterWiring, 10,
                     MeterWiringPayload(harkentest::ObjectId(10), harkentest::ObjectId(920), 10, 200));
    const std::vector<std::uint8_t> bytes = builder.Build();
    PackLogicImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackV2Image(bytes.data(), bytes.size(), &image));
    EXPECT_EQ(0u, image.EntryCount());
}

} // namespace
