// .pack container v2 byte layout: the logic-entry format branch. Shared
// by the engine-side reader (pack logic image) and the compiler tool
// (packc), exactly like PackFormat.h is for v1.
//
// v2 is selected by formatVersion == 2 in the shared 'VSPK' header
// position; the v1 layout and its parser are untouched, and a v1 reader
// refuses a v2 file through its existing formatVersion check. A v2 file
// carries a sectioned, typed entry directory — per entry: kind,
// layout vintage, id, payload offset, payload size, payload
// checksum — plus one directory checksum. All fields are little-endian
// and serialized one at a time (the v1 no-implicit-padding discipline);
// live descriptor structs are never memory-dumped. Each entry kind has a
// pointer-free wire schema. Directory vintage is an internal layout id
// under formatVersion 2, not a product schema. Readers accept the current
// write table per kind and refuse unknown or retired layouts; they never
// silently reinterpret old bytes. Writers emit one current table per kind.
// Extra fields occupy reserved or empty slots, or require a formatVersion
// bump. They do not mint a new vintage, and absence of an authored field
// does not select an older table.
//
// File shape: Header, then the directory (entryCount fixed-size
// records), then the payload region (each payload 8-byte aligned), then
// the media blob region (64-byte aligned, reference-mode registration
// pointing straight into the loaded buffer — the v1 rule). The media
// payload record is byte-for-byte the v1 TOC entry, so the media-entry
// wire format is untouched by this version branch; its blobOffset is
// file-absolute into the blob region.
//
// This header is a pure byte codec: serializers write exactly the wire
// bytes, parsers accept exactly them (reserved fields and padding must
// read back zero — a nonzero reserved byte is a malformed container).
// Directory vintage names the current write table; unknown or retired
// values refuse rather than reinterpret the bytes.
// Semantic validation — value ranges, kind discipline, id uniqueness,
// reference closure — belongs to the image layer and the compiler, not
// here. Curve segments use absolute Bezier handle coordinates; each live
// handle x must remain in its endpoint interval, but h1x <= h2x is not
// required. Unused point and handle slots are canonical bitwise +0.0f.
// Attenuation volume y is authored in [-96, 0] dB and evaluated in linear
// amplitude; lowpass y is positive Hz, width y is [0, 1], and binding y has
// no generic range beyond finiteness.
//
// This curve replacement is one coordinated compatibility break: schema
// vintage 2 rejects vintage 1, remote protocol major 2 rejects major 1, and
// trace format v2 rejects v1. No boundary translates the retired shape tags.
#ifndef HARKEN_MEDIA_PACK_FORMAT_V2_H
#define HARKEN_MEDIA_PACK_FORMAT_V2_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/media/CurrentWorldBodyVintage.h"
#include "harken/media/PackFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kPackFormatVersion2 = HARKEN_PACK_SCHEMA_VERSION;

// Header: magic(4) + formatVersion(4) + headerBytes(4) + fileBytes(8) +
// packKind(4) + entryCount(4) + directoryOffset(8) + directoryChecksum(8).
inline constexpr std::uint32_t kPackV2HeaderBytes = 44;

// Directory record: kind(4) + vintage(4) + id(8) + offset(8) + size(8) +
// payloadChecksum(8).
inline constexpr std::uint32_t kPackV2DirectoryEntryBytes = 56;

// Pack kinds (header field). Resident packs carry logic-only
// infrastructure (zero media entries, enforced by compiler and loader);
// content packs carry media plus reclaimable logic.
inline constexpr std::uint32_t kPackKindResident = 0;
inline constexpr std::uint32_t kPackKindContent = 1;
inline constexpr std::uint32_t kPackKindInit = 2;
inline constexpr std::uint32_t kPackKindCanonicalContent = 3;

// Entry kinds. ABI-frozen values, append-only, never renumbered.
inline constexpr std::uint32_t kPackEntryKindMedia = 1;
inline constexpr std::uint32_t kPackEntryKindBus = 2;
inline constexpr std::uint32_t kPackEntryKindBusSend = 3;
inline constexpr std::uint32_t kPackEntryKindAttenuation = 4;
inline constexpr std::uint32_t kPackEntryKindDiscreteVariable = 5;
inline constexpr std::uint32_t kPackEntryKindMeterWiring = 6;
inline constexpr std::uint32_t kPackEntryKindBindingDefinition = 7;
inline constexpr std::uint32_t kPackEntryKindBindingAttachment = 8;
// Shipping canonical packs do not emit this kind. packc still writes it
// for non-canonical / unit-test content packs.
inline constexpr std::uint32_t kPackEntryKindContainer = 9;
inline constexpr std::uint32_t kPackEntryKindAction = 10;
// Shipping canonical packs do not emit this kind. packc still writes it
// for non-canonical / unit-test content packs.
inline constexpr std::uint32_t kPackEntryKindVoiceRecipe = 11;
inline constexpr std::uint32_t kPackEntryKindBusEffect = 12;
inline constexpr std::uint32_t kPackEntryKindEffectDynamicRoute = 13;
inline constexpr std::uint32_t kPackEntryKindInitRoutingCatalog = 20;
inline constexpr std::uint32_t kPackEntryKindCanonicalDefinitionPartition = 21;
inline constexpr std::uint32_t kPackEntryKindPackRequirementBlock = 22;
inline constexpr std::uint32_t kPackEntryKindPackDependencyEdge = 23;
inline constexpr std::uint32_t kPackEntryKindBusDefinitionSnapshot = 24;
inline constexpr std::uint32_t kPackEntryKindGameplayCatalog = 25;
inline constexpr std::uint32_t kPackEntryKindVariablePolicyOverlay = 26;
inline constexpr std::uint32_t kPackEntryKindVariableRequirements = 27;
inline constexpr std::uint32_t kPackEntryKindVariableNameCatalog = 28;
inline constexpr std::uint32_t kPackCanonicalEntryVintage = 1;
inline constexpr std::uint32_t kPackCanonicalRequirementVintage = 2;
inline constexpr std::uint32_t kPackBusDefinitionSnapshotVintage = 1;
inline constexpr std::uint32_t kPackBusDefinitionSnapshotVintage2 = 2;
inline constexpr std::uint32_t kPackGameplayCatalogVintage = 1;
inline constexpr std::uint32_t kGameplayCatalogHeaderBytes = 96;
inline constexpr std::uint32_t kGameplayCatalogActiveRowBytes = 72;
inline constexpr std::uint32_t kGameplayCatalogRetiredRowBytes = 40;
inline constexpr std::uint32_t kGameplayCatalogMaxEntries = 4096;
inline constexpr std::uint32_t kGameplayCatalogMaxBytes = 1048576;
inline constexpr std::uint32_t kGameplayCatalogMaxNameBytes = 128;
inline constexpr std::uint32_t kGameplayCatalogRetiredReasonRenamed = 1;
inline constexpr std::uint32_t kGameplayCatalogRetiredReasonDeleted = 2;
inline constexpr std::uint32_t kGameplayCatalogRetiredReasonReused = 3;
inline constexpr std::uint32_t kGameplayCatalogFlagHasRetired = 1u;
inline constexpr std::uint32_t kGameplayCatalogActiveFlagOptional = 1u;

// Directory vintage is a per-kind layout id inside formatVersion 2, not a
// third product schema. Current writers emit:
//   VoiceRecipe  kPackV2PlaybackRecipeSchemaVintage
//   Attenuation  kPackV2AuthoredToneSchemaVintage
//   BusEffect / EffectDynamicRoute  kPackV2DynamicSchemaVintage
//   remaining kinds  kPackV2SchemaVintage
//   canonical kinds  kPackCanonicalEntryVintage
// Appended fields occupy reserved / empty slots, or bump formatVersion.
// Do not add another vintage integer for an optional authored field, and
// do not pick a smaller vintage because that field is absent. Readers
// refuse retired VoiceRecipe 2/3/4 and Attenuation 2. Vintage 2 is the
// Bezier curve break for remaining kinds: readers reject vintage 1
// rather than mixing schemas inside one container.
inline constexpr std::uint32_t kPackV2SchemaVintage = 2;
inline constexpr std::uint32_t kPackV2AuthoredToneSchemaVintage = 3;
inline constexpr std::uint32_t kPackV2SpatialRecipeSchemaVintage = 4;
inline constexpr std::uint32_t kPackV2PlaybackRecipeSchemaVintage = 5;
inline constexpr std::uint32_t kPackV2BusMixSchemaVintage = 6;
inline constexpr std::uint32_t kPackV2DynamicSchemaVintage = 1;

// Payload alignment inside the payload region. Media blobs keep the v1
// 64-byte rule (kPackBlobAlign).
inline constexpr std::uint64_t kPackV2PayloadAlign = 8;

// Fixed payload sizes. Variable-size kinds have sizing helpers below.
// Current Attenuation write is kPackV3AttenuationPayloadBytes; current
// VoiceRecipe write is PackV5VoiceRecipePayloadBytes.
inline constexpr std::uint64_t kPackV2MediaPayloadBytes =
    kPackTocEntryBytes;                                    // the v1 record
inline constexpr std::uint64_t kPackV2BusPayloadBytes = 32;
inline constexpr std::uint64_t kPackV6BusPayloadBytes = 48;
inline constexpr std::uint64_t kPackV2BusSendPayloadBytes = 40;
inline constexpr std::uint64_t kPackV2CurveWireBytes = 184;
inline constexpr std::uint64_t kPackV2AttenuationPayloadBytes =
    48 + 3 * kPackV2CurveWireBytes;                        // 600
inline constexpr std::uint64_t kPackV3AttenuationPayloadBytes =
    kPackV2AttenuationPayloadBytes + 8;                    // 608
inline constexpr std::uint64_t kPackV4AttenuationPayloadBytes =
    kPackV3AttenuationPayloadBytes + kPackV2CurveWireBytes; // 792
inline constexpr std::uint64_t kPackV2DiscreteVariablePayloadBytes = 16;
inline constexpr std::uint64_t kPackV2MeterWiringPayloadBytes = 40;
inline constexpr std::uint64_t kPackV2BindingCurveWireBytes = 180;
inline constexpr std::uint64_t kPackV2BindingDefinitionPayloadBytes =
    48 + kPackV2BindingCurveWireBytes;                     // 212
inline constexpr std::uint64_t kPackV2BindingAttachmentPayloadBytes = 40;
inline constexpr std::uint64_t kPackV2ContainerHeaderBytes = 48;
inline constexpr std::uint64_t kPackV2ContainerChildBytes = 32;
inline constexpr std::uint32_t kPackV2VoiceRecipeMaxInstalls = 8;
inline constexpr std::uint32_t kPackV2VoiceRecipeMaxBindings = 8;
inline constexpr std::uint32_t kPackV2VoiceRecipeMaxParams = 64;
inline constexpr std::uint32_t kPackV2EffectDescriptorVintage = 2;
inline constexpr std::uint64_t kPackV2VoiceRecipeHeaderBytes = 24;
inline constexpr std::uint64_t kPackV3VoiceRecipeToneBytes = 48;
inline constexpr std::uint64_t kPackV4VoiceRecipeSpatialBytes = 24;
inline constexpr std::uint64_t kPackV5VoiceRecipePlaybackBytes = 8;
inline constexpr std::uint64_t kPackV2VoiceRecipeInstallBytes =
    48 + static_cast<std::uint64_t>(kPackV2VoiceRecipeMaxParams) * 4;
inline constexpr std::uint64_t kPackV2VoiceRecipeBindingBytes = 16;
inline constexpr std::uint64_t kPackV2BusEffectPayloadBytes = 320;
inline constexpr std::uint64_t kPackV2EffectDynamicRoutePayloadBytes = 72;
inline constexpr std::uint32_t kPackVoiceToneLowpassEnabled = 1u << 0;
inline constexpr std::uint32_t kPackVoiceToneHighpassEnabled = 1u << 1;
inline constexpr std::uint32_t kPackVoiceToneLowShelfEnabled = 1u << 2;
inline constexpr std::uint32_t kPackVoiceToneHighShelfEnabled = 1u << 3;
inline constexpr std::uint32_t kPackVoiceToneEnableMask =
    kPackVoiceToneLowpassEnabled | kPackVoiceToneHighpassEnabled |
    kPackVoiceToneLowShelfEnabled | kPackVoiceToneHighShelfEnabled;

inline std::uint64_t PackV2ContainerPayloadBytes(std::uint32_t childCount) {
    return kPackV2ContainerHeaderBytes +
           static_cast<std::uint64_t>(childCount) * kPackV2ContainerChildBytes;
}

inline std::uint64_t PackV2VoiceRecipePayloadBytes(
    std::uint32_t installCount, std::uint32_t bindingCount) {
    return kPackV2VoiceRecipeHeaderBytes +
           static_cast<std::uint64_t>(installCount) *
               kPackV2VoiceRecipeInstallBytes +
           static_cast<std::uint64_t>(bindingCount) *
               kPackV2VoiceRecipeBindingBytes;
}

inline std::uint64_t PackV3VoiceRecipePayloadBytes(
    std::uint32_t installCount, std::uint32_t bindingCount) {
    return kPackV2VoiceRecipeHeaderBytes + kPackV3VoiceRecipeToneBytes +
           static_cast<std::uint64_t>(installCount) *
               kPackV2VoiceRecipeInstallBytes +
           static_cast<std::uint64_t>(bindingCount) *
               kPackV2VoiceRecipeBindingBytes;
}

inline std::uint64_t PackV4VoiceRecipePayloadBytes(
    std::uint32_t installCount, std::uint32_t bindingCount) {
    return kPackV2VoiceRecipeHeaderBytes + kPackV3VoiceRecipeToneBytes +
           kPackV4VoiceRecipeSpatialBytes +
           static_cast<std::uint64_t>(installCount) *
               kPackV2VoiceRecipeInstallBytes +
           static_cast<std::uint64_t>(bindingCount) *
               kPackV2VoiceRecipeBindingBytes;
}

inline std::uint64_t PackV5VoiceRecipePayloadBytes(
    std::uint32_t installCount, std::uint32_t bindingCount) {
    return kPackV2VoiceRecipeHeaderBytes + kPackV3VoiceRecipeToneBytes +
           kPackV4VoiceRecipeSpatialBytes + kPackV5VoiceRecipePlaybackBytes +
           static_cast<std::uint64_t>(installCount) *
               kPackV2VoiceRecipeInstallBytes +
           static_cast<std::uint64_t>(bindingCount) *
               kPackV2VoiceRecipeBindingBytes;
}

inline void StoreLEF32(std::uint8_t* out, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    StoreLE32(out, bits);
}

inline float LoadLEF32(const std::uint8_t* in) {
    const std::uint32_t bits = LoadLE32(in);
    float v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

// --- in-memory working forms ---
// Plain working copies, like the v1 PackHeader/PackTocEntry pair; only
// the Serialize/Parse helpers define bytes. Where a kind's registration
// currency is a public descriptor struct, the working form embeds that
// struct directly (structSize filled by the parser) so readers hand it
// to the registration seam without a second translation. Pointer-bearing
// descriptor fields such as container children stay OUT of the working
// forms; the consumer supplies storage and wires the pointer.

struct PackV2Header {
    std::uint32_t formatVersion = 0;
    std::uint32_t headerBytes = 0;
    std::uint64_t fileBytes = 0;
    std::uint32_t packKind = 0;
    std::uint32_t entryCount = 0;
    std::uint64_t directoryOffset = 0;
    std::uint64_t directoryChecksum = 0;
};

struct PackV2DirectoryEntry {
    std::uint32_t kind = 0;
    std::uint32_t vintage = 0;
    VnObjectId id{};
    std::uint64_t relationOrdinal = 0;
    std::uint64_t offset = 0;  // payload file offset
    std::uint64_t size = 0;    // payload byte size
    std::uint64_t payloadChecksum = 0;
};

struct PackBusEntry {
    VnBusId busId{};
    VnBusId parentBusId{};
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
    float makeUpGain = 0.0f;
};

struct PackBusSendEntry {
    VnBusId fromBusId{};
    VnBusId toBusId{};
    float levelDb = 0.0f;
};

struct PackMeterWiringEntry {
    VnBusId busId{};
    VnVariableId variableId{};
    std::uint32_t attackMs = 0;
    std::uint32_t releaseMs = 0;
};

struct PackBindingAttachmentEntry {
    VnBindingId bindingId{};
    std::uint32_t scopeKind = 0;
    VnObjectId objectId{};
};

struct PackVoiceRecipeEntryHeader {
    VnSoundId playableId{};
    std::uint32_t installCount = 0;
    std::uint32_t bindingCount = 0;
};

struct PackVoiceRecipeInstall {
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t bypass = 0;
    std::uint32_t paramCount = 0;
    std::array<float, kPackV2VoiceRecipeMaxParams> params{};
    VnObjectId effectId{};
};

struct PackBusEffectEntry {
    VnBusId busId{};
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t bypass = 0;
    std::uint32_t faderPointGuard = 0;
    std::uint32_t paramCount = 0;
    std::uint32_t paramBytes = 0;
    std::array<float, kPackV2VoiceRecipeMaxParams> params{};
    VnObjectId effectId{};
};

struct PackEffectDynamicRouteEntry {
    VnBusId targetBusId{};
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t expectedEffectType = 0;
    std::uint32_t signalForm = 0;
    std::uint32_t sourceKind = 0;
    std::uint32_t tap = 0;
    VnObjectId sourceId{};
    float levelDb = 0.0f;
};

struct PackVoiceRecipeTone {
    std::uint32_t present = 0;
    std::uint32_t sectionEnableMask = 0;
    float lowpassFrequencyHz = 0.0f;
    float lowpassQ = 0.0f;
    float highpassFrequencyHz = 0.0f;
    float highpassQ = 0.0f;
    float lowShelfFrequencyHz = 0.0f;
    float lowShelfQ = 0.0f;
    float lowShelfGainDb = 0.0f;
    float highShelfFrequencyHz = 0.0f;
    float highShelfQ = 0.0f;
    float highShelfGainDb = 0.0f;
};

struct PackVoiceRecipeSpatial {
    VnAttenuationId attenuationId{};
    float distanceScale = 1.0f;
    std::uint32_t reserved0 = 0;
};

struct PackVoiceRecipePlayback {
    std::uint32_t present = 0;
    std::uint32_t loopCount = 0;
};

// Container payload minus the child array (children are serialized
// inline after this header; the consumer owns their storage).
struct PackContainerEntryHeader {
    VnObjectId containerId{};
    std::uint32_t selectionOrder = 0;
    std::uint32_t avoidRepeatWindow = 0;
    VnVariableId selectionVariableId{};
    std::uint32_t defaultIndex = 0;
    std::uint32_t childCount = 0;
};

struct PackContainerChildV2 {
    VnObjectId playableId{};
    std::uint32_t weight = 0;
    std::uint32_t matchValue = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t delayJitterMs = 0;
};

// --- header ---

inline void SerializePackV2Header(const PackV2Header& h, std::uint8_t* out) {
    out[0] = kPackMagic[0];
    out[1] = kPackMagic[1];
    out[2] = kPackMagic[2];
    out[3] = kPackMagic[3];
    StoreLE32(out + 4, h.formatVersion);
    StoreLE32(out + 8, h.headerBytes);
    StoreLE64(out + 12, h.fileBytes);
    StoreLE32(out + 20, h.packKind);
    StoreLE32(out + 24, h.entryCount);
    StoreLE64(out + 28, h.directoryOffset);
    StoreLE64(out + 36, h.directoryChecksum);
}

// Parses in[0..kPackV2HeaderBytes). Returns false when the magic does
// not match; field sanity beyond the magic is the caller's validation
// (the v1 split of duties).
inline bool ParsePackV2Header(const std::uint8_t* in, PackV2Header* out) {
    if (in[0] != kPackMagic[0] || in[1] != kPackMagic[1] ||
        in[2] != kPackMagic[2] || in[3] != kPackMagic[3]) {
        return false;
    }
    out->formatVersion = LoadLE32(in + 4);
    out->headerBytes = LoadLE32(in + 8);
    out->fileBytes = LoadLE64(in + 12);
    out->packKind = LoadLE32(in + 20);
    out->entryCount = LoadLE32(in + 24);
    out->directoryOffset = LoadLE64(in + 28);
    out->directoryChecksum = LoadLE64(in + 36);
    return true;
}

// --- directory ---

inline void SerializePackV2DirectoryEntry(const PackV2DirectoryEntry& e,
                                          std::uint8_t* out) {
    StoreLE32(out + 0, e.kind);
    StoreLE32(out + 4, e.vintage);
    StoreObjectId(out + 8, e.id);
    StoreLE64(out + 24, e.offset);
    StoreLE64(out + 32, e.size);
    StoreLE64(out + 40, e.payloadChecksum);
    StoreLE64(out + 48, e.relationOrdinal);
}

inline PackV2DirectoryEntry ParsePackV2DirectoryEntry(const std::uint8_t* in) {
    PackV2DirectoryEntry e;
    e.kind = LoadLE32(in + 0);
    e.vintage = LoadLE32(in + 4);
    e.id = LoadObjectId(in + 8);
    e.offset = LoadLE64(in + 24);
    e.size = LoadLE64(in + 32);
    e.payloadChecksum = LoadLE64(in + 40);
    e.relationOrdinal = LoadLE64(in + 48);
    return e;
}

// --- fixed-size logic payloads ---

inline void SerializePackV2Bus(const PackBusEntry& e, std::uint8_t* out) {
    StoreObjectId(out + 0, e.busId);
    StoreObjectId(out + 16, e.parentBusId);
}

inline bool ParsePackV2Bus(const std::uint8_t* in, std::uint64_t size,
                           PackBusEntry* out) {
    if (size != kPackV2BusPayloadBytes) {
        return false;
    }
    out->busId = LoadObjectId(in + 0);
    out->parentBusId = LoadObjectId(in + 16);
    out->volume = 0.0f;
    out->outputBusVolume = 0.0f;
    out->makeUpGain = 0.0f;
    return true;
}

inline void SerializePackV6Bus(const PackBusEntry& e, std::uint8_t* out) {
    StoreObjectId(out + 0, e.busId);
    StoreObjectId(out + 16, e.parentBusId);
    StoreLEF32(out + 32, e.volume);
    StoreLEF32(out + 36, e.outputBusVolume);
    StoreLEF32(out + 40, e.makeUpGain);
    StoreLE32(out + 44, 0);
}

inline bool ParsePackV6Bus(const std::uint8_t* in, std::uint64_t size,
                           PackBusEntry* out) {
    if (size != kPackV6BusPayloadBytes || LoadLE32(in + 44) != 0) {
        return false;
    }
    out->busId = LoadObjectId(in + 0);
    out->parentBusId = LoadObjectId(in + 16);
    out->volume = LoadLEF32(in + 32);
    out->outputBusVolume = LoadLEF32(in + 36);
    out->makeUpGain = LoadLEF32(in + 40);
    return true;
}

inline void SerializePackV2BusSend(const PackBusSendEntry& e,
                                   std::uint8_t* out) {
    StoreObjectId(out + 0, e.fromBusId);
    StoreObjectId(out + 16, e.toBusId);
    StoreLEF32(out + 32, e.levelDb);
    StoreLE32(out + 36, 0);
}

inline bool ParsePackV2BusSend(const std::uint8_t* in, std::uint64_t size,
                               PackBusSendEntry* out) {
    if (size != kPackV2BusSendPayloadBytes || LoadLE32(in + 36) != 0) {
        return false;
    }
    out->fromBusId = LoadObjectId(in + 0);
    out->toBusId = LoadObjectId(in + 16);
    out->levelDb = LoadLEF32(in + 32);
    return true;
}

inline void SerializePackV2BusEffect(const PackBusEffectEntry& e,
                                     std::uint8_t* out) {
    StoreObjectId(out + 304, e.effectId);
    StoreObjectId(out + 0, e.busId);
    StoreLE32(out + 16, e.slot);
    StoreLE32(out + 20, e.effectType);
    StoreLE32(out + 24, e.descriptorVintage);
    StoreLE32(out + 28, e.bypass);
    StoreLE32(out + 32, e.faderPointGuard);
    StoreLE32(out + 36, e.paramCount);
    StoreLE32(out + 40, e.paramBytes);
    StoreLE32(out + 44, 0);
    for (std::uint32_t i = 0; i < kPackV2VoiceRecipeMaxParams; ++i) {
        StoreLEF32(out + 48 + static_cast<std::size_t>(i) * 4,
                   i < e.paramCount ? e.params[i] : 0.0f);
    }
}

inline bool ParsePackV2BusEffect(const std::uint8_t* in,
                                 std::uint64_t size,
                                 PackBusEffectEntry* out) {
    if (size != kPackV2BusEffectPayloadBytes ||
        LoadLE32(in + 44) != 0) {
        return false;
    }
    out->busId = LoadObjectId(in + 0);
    out->effectId = LoadObjectId(in + 304);
    out->slot = LoadLE32(in + 16);
    out->effectType = LoadLE32(in + 20);
    out->descriptorVintage = LoadLE32(in + 24);
    out->bypass = LoadLE32(in + 28);
    out->faderPointGuard = LoadLE32(in + 32);
    out->paramCount = LoadLE32(in + 36);
    out->paramBytes = LoadLE32(in + 40);
    if (out->paramCount > kPackV2VoiceRecipeMaxParams ||
        out->paramBytes != out->paramCount * sizeof(float)) {
        return false;
    }
    for (std::uint32_t i = 0; i < kPackV2VoiceRecipeMaxParams; ++i) {
        const std::uint32_t bits =
            LoadLE32(in + 48 + static_cast<std::size_t>(i) * 4);
        if (i >= out->paramCount && bits != 0) {
            return false;
        }
        out->params[i] = LoadLEF32(
            in + 48 + static_cast<std::size_t>(i) * 4);
    }
    return true;
}

inline void SerializePackV2EffectDynamicRoute(
    const PackEffectDynamicRouteEntry& e, std::uint8_t* out) {
    StoreObjectId(out + 0, e.targetBusId);
    StoreLE32(out + 16, e.effectSlot);
    StoreLE32(out + 20, e.routeSlot);
    StoreLE32(out + 24, e.expectedEffectType);
    StoreLE32(out + 28, e.signalForm);
    StoreLE32(out + 32, e.sourceKind);
    StoreLE32(out + 36, e.tap);
    StoreObjectId(out + 40, e.sourceId);
    StoreLEF32(out + 56, e.levelDb);
    StoreLE32(out + 60, 0);
    StoreLE64(out + 64, 0);
}

inline bool ParsePackV2EffectDynamicRoute(
    const std::uint8_t* in, std::uint64_t size,
    PackEffectDynamicRouteEntry* out) {
    if (size != kPackV2EffectDynamicRoutePayloadBytes ||
        LoadLE32(in + 60) != 0 || LoadLE64(in + 64) != 0) {
        return false;
    }
    out->targetBusId = LoadObjectId(in + 0);
    out->effectSlot = LoadLE32(in + 16);
    out->routeSlot = LoadLE32(in + 20);
    out->expectedEffectType = LoadLE32(in + 24);
    out->signalForm = LoadLE32(in + 28);
    out->sourceKind = LoadLE32(in + 32);
    out->tap = LoadLE32(in + 36);
    out->sourceId = LoadObjectId(in + 40);
    out->levelDb = LoadLEF32(in + 56);
    return true;
}

// Attenuation override curve, 184 bytes: enabled(4) + pointCount(4) +
// 8 points x (x,y)(64) + 7 handles x (h1x,h1y,h2x,h2y)(112). Unused
// point/handle slots must be serialized as bitwise +0.0f (the writer
// zero-fills; the parser enforces).
inline void SerializePackV2AttenuationCurve(const VnAttenuationCurve& c,
                                            std::uint8_t* out) {
    StoreLE32(out + 0, c.enabled);
    StoreLE32(out + 4, c.pointCount);
    const std::uint32_t points =
        c.pointCount <= VN_ATTENUATION_MAX_CURVE_POINTS
            ? c.pointCount
            : VN_ATTENUATION_MAX_CURVE_POINTS;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const bool live = i < points;
        StoreLEF32(out + 8 + i * 8, live ? c.points[i].x : 0.0f);
        StoreLEF32(out + 12 + i * 8, live ? c.points[i].y : 0.0f);
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        const bool live = points > 0 && i + 1 < points;
        StoreLEF32(out + 72 + i * 16,
                   live ? c.handles[i].h1x : 0.0f);
        StoreLEF32(out + 76 + i * 16,
                   live ? c.handles[i].h1y : 0.0f);
        StoreLEF32(out + 80 + i * 16,
                   live ? c.handles[i].h2x : 0.0f);
        StoreLEF32(out + 84 + i * 16,
                   live ? c.handles[i].h2y : 0.0f);
    }
}

inline bool ParsePackV2AttenuationCurve(const std::uint8_t* in,
                                        VnAttenuationCurve* out) {
    out->enabled = LoadLE32(in + 0);
    out->pointCount = LoadLE32(in + 4);
    for (std::uint32_t i = 0; i < 8; ++i) {
        out->points[i].x = LoadLEF32(in + 8 + i * 8);
        out->points[i].y = LoadLEF32(in + 12 + i * 8);
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        out->handles[i].h1x = LoadLEF32(in + 72 + i * 16);
        out->handles[i].h1y = LoadLEF32(in + 76 + i * 16);
        out->handles[i].h2x = LoadLEF32(in + 80 + i * 16);
        out->handles[i].h2y = LoadLEF32(in + 84 + i * 16);
    }
    // Zero-fill discipline on the unused tail: a live slot count decides
    // which slots may be nonzero. Out-of-contract pointCount values parse
    // (semantic validation refuses them later); the tail rule then applies
    // to the in-range prefix only.
    const std::uint32_t points =
        out->pointCount <= VN_ATTENUATION_MAX_CURVE_POINTS
            ? out->pointCount
            : VN_ATTENUATION_MAX_CURVE_POINTS;
    for (std::uint32_t i = points; i < 8; ++i) {
        if (LoadLE32(in + 8 + i * 8) != 0 || LoadLE32(in + 12 + i * 8) != 0) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        const bool live = points > 0 && i + 1 < points;
        if (!live &&
            (LoadLE32(in + 72 + i * 16) != 0 ||
             LoadLE32(in + 76 + i * 16) != 0 ||
             LoadLE32(in + 80 + i * 16) != 0 ||
             LoadLE32(in + 84 + i * 16) != 0)) {
            return false;
        }
    }
    return true;
}

// Attenuation profile, 600 bytes: 12 scalar fields (48) then the three
// override curves in declaration order (volume, lowpass, width).
inline void SerializePackV2Attenuation(const VnAttenuationDesc& d,
                                       std::uint8_t* out) {
    StoreLE32(out + 0, d.rolloff);
    StoreLEF32(out + 4, d.minDistance);
    StoreLEF32(out + 8, d.maxDistance);
    StoreLEF32(out + 12, d.airAbsorptionNearHz);
    StoreLEF32(out + 16, d.airAbsorptionFarHz);
    StoreLEF32(out + 20, d.proximityRadius);
    StoreLEF32(out + 24, d.proximityMaxBoostDb);
    StoreLE32(out + 28, d.coneEnabled);
    StoreLEF32(out + 32, d.coneInnerDeg);
    StoreLEF32(out + 36, d.coneOuterDeg);
    StoreLEF32(out + 40, d.coneOuterGainDb);
    StoreLEF32(out + 44, d.coneOuterLowpassHz);
    SerializePackV2AttenuationCurve(d.volumeCurve, out + 48);
    SerializePackV2AttenuationCurve(d.lowpassCurve, out + 232);
    SerializePackV2AttenuationCurve(d.widthCurve, out + 416);
}

inline bool ParsePackV2Attenuation(const std::uint8_t* in, std::uint64_t size,
                                   VnAttenuationDesc* out) {
    if (size != kPackV2AttenuationPayloadBytes) {
        return false;
    }
    *out = VnAttenuationDesc{};
    out->structSize = static_cast<std::uint32_t>(offsetof(VnAttenuationDesc, highpassCurve));
    out->rolloff = LoadLE32(in + 0);
    out->minDistance = LoadLEF32(in + 4);
    out->maxDistance = LoadLEF32(in + 8);
    out->airAbsorptionNearHz = LoadLEF32(in + 12);
    out->airAbsorptionFarHz = LoadLEF32(in + 16);
    out->proximityRadius = LoadLEF32(in + 20);
    out->proximityMaxBoostDb = LoadLEF32(in + 24);
    out->coneEnabled = LoadLE32(in + 28);
    out->coneInnerDeg = LoadLEF32(in + 32);
    out->coneOuterDeg = LoadLEF32(in + 36);
    out->coneOuterGainDb = LoadLEF32(in + 40);
    out->coneOuterLowpassHz = LoadLEF32(in + 44);
    out->proximityFrequency = 250.0f;
    return ParsePackV2AttenuationCurve(in + 48, &out->volumeCurve) &&
           ParsePackV2AttenuationCurve(in + 232, &out->lowpassCurve) &&
           ParsePackV2AttenuationCurve(in + 416, &out->widthCurve);
}

inline void SerializePackV3Attenuation(const VnAttenuationDesc& d,
                                       std::uint8_t* out) {
    SerializePackV2Attenuation(d, out);
    StoreLEF32(out + kPackV2AttenuationPayloadBytes,
               d.proximityFrequency);
    StoreLE32(out + kPackV2AttenuationPayloadBytes + 4, 0);
}

inline bool ParsePackV3Attenuation(const std::uint8_t* in, std::uint64_t size,
                                   VnAttenuationDesc* out) {
    if (size != kPackV3AttenuationPayloadBytes ||
        LoadLE32(in + kPackV2AttenuationPayloadBytes + 4) != 0 ||
        !ParsePackV2Attenuation(in, kPackV2AttenuationPayloadBytes, out)) {
        return false;
    }
    out->proximityFrequency =
        LoadLEF32(in + kPackV2AttenuationPayloadBytes);
    return true;
}

inline bool ParsePackAttenuation(const std::uint8_t* in, std::uint64_t size,
                                  std::uint32_t formatVersion,
                                  VnAttenuationDesc* out) {
    if (formatVersion == kPackFormatVersion3) {
        return ParsePackV3Attenuation(in, size, out);
    }
    if (formatVersion != kPackFormatVersion4 || size != kPackV4AttenuationPayloadBytes ||
        !ParsePackV3Attenuation(in, kPackV3AttenuationPayloadBytes, out) ||
        !ParsePackV2AttenuationCurve(in + kPackV3AttenuationPayloadBytes,
                                    &out->highpassCurve)) {
        return false;
    }
    out->structSize = sizeof(VnAttenuationDesc);
    return true;
}

inline void SerializePackV2DiscreteVariable(VnVariableId id,
                                            std::uint8_t* out) {
    StoreObjectId(out, id);
}

inline bool ParsePackV2DiscreteVariable(const std::uint8_t* in,
                                        std::uint64_t size,
                                        VnVariableId* out) {
    if (size != kPackV2DiscreteVariablePayloadBytes) {
        return false;
    }
    *out = LoadObjectId(in);
    return true;
}

inline void SerializePackV2MeterWiring(const PackMeterWiringEntry& e,
                                       std::uint8_t* out) {
    StoreObjectId(out + 0, e.busId);
    StoreObjectId(out + 16, e.variableId);
    StoreLE32(out + 32, e.attackMs);
    StoreLE32(out + 36, e.releaseMs);
}

inline bool ParsePackV2MeterWiring(const std::uint8_t* in, std::uint64_t size,
                                   PackMeterWiringEntry* out) {
    if (size != kPackV2MeterWiringPayloadBytes) {
        return false;
    }
    out->busId = LoadObjectId(in + 0);
    out->variableId = LoadObjectId(in + 16);
    out->attackMs = LoadLE32(in + 32);
    out->releaseMs = LoadLE32(in + 36);
    return true;
}

// Binding curve, 180 bytes: pointCount(4) + 8 points(64) +
// 7 handles x (h1x,h1y,h2x,h2y)(112). Same bitwise +0.0f tail discipline
// as the attenuation curve.
inline void SerializePackV2BindingCurve(const VnCurveDesc& c,
                                        std::uint8_t* out) {
    StoreLE32(out + 0, c.pointCount);
    const std::uint32_t points =
        c.pointCount <= VN_CURVE_MAX_POINTS ? c.pointCount
                                            : VN_CURVE_MAX_POINTS;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const bool live = i < points;
        StoreLEF32(out + 4 + i * 8, live ? c.points[i].x : 0.0f);
        StoreLEF32(out + 8 + i * 8, live ? c.points[i].y : 0.0f);
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        const bool live = points > 0 && i + 1 < points;
        StoreLEF32(out + 68 + i * 16,
                   live ? c.handles[i].h1x : 0.0f);
        StoreLEF32(out + 72 + i * 16,
                   live ? c.handles[i].h1y : 0.0f);
        StoreLEF32(out + 76 + i * 16,
                   live ? c.handles[i].h2x : 0.0f);
        StoreLEF32(out + 80 + i * 16,
                   live ? c.handles[i].h2y : 0.0f);
    }
}

inline bool ParsePackV2BindingCurve(const std::uint8_t* in,
                                    VnCurveDesc* out) {
    *out = VnCurveDesc{};
    out->structSize = sizeof(VnCurveDesc);
    out->pointCount = LoadLE32(in + 0);
    for (std::uint32_t i = 0; i < 8; ++i) {
        out->points[i].x = LoadLEF32(in + 4 + i * 8);
        out->points[i].y = LoadLEF32(in + 8 + i * 8);
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        out->handles[i].h1x = LoadLEF32(in + 68 + i * 16);
        out->handles[i].h1y = LoadLEF32(in + 72 + i * 16);
        out->handles[i].h2x = LoadLEF32(in + 76 + i * 16);
        out->handles[i].h2y = LoadLEF32(in + 80 + i * 16);
    }
    const std::uint32_t points =
        out->pointCount <= VN_CURVE_MAX_POINTS ? out->pointCount
                                               : VN_CURVE_MAX_POINTS;
    for (std::uint32_t i = points; i < 8; ++i) {
        if (LoadLE32(in + 4 + i * 8) != 0 || LoadLE32(in + 8 + i * 8) != 0) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < 7; ++i) {
        const bool live = points > 0 && i + 1 < points;
        if (!live &&
            (LoadLE32(in + 68 + i * 16) != 0 ||
             LoadLE32(in + 72 + i * 16) != 0 ||
             LoadLE32(in + 76 + i * 16) != 0 ||
             LoadLE32(in + 80 + i * 16) != 0)) {
            return false;
        }
    }
    return true;
}

// Binding definition, 212 bytes: bindingId(8) + variableId(8) +
// targetScopeKind(4) + targetId(4) + transitionMs(4) + reserved(4) +
// curve(180). The directory id duplicates bindingId; the image layer
// cross-checks them.
inline void SerializePackV2BindingDefinition(VnBindingId bindingId,
                                             const VnBindingDesc& d,
                                             std::uint8_t* out) {
    StoreObjectId(out + 0, bindingId);
    StoreObjectId(out + 16, d.variableId);
    StoreLE32(out + 32, d.targetScopeKind);
    StoreLE32(out + 36, d.targetId);
    StoreLE32(out + 40, d.transitionMs);
    StoreLE32(out + 44, 0);
    SerializePackV2BindingCurve(d.curve, out + 48);
}

inline bool ParsePackV2BindingDefinition(const std::uint8_t* in,
                                         std::uint64_t size,
                                         VnBindingId* outId,
                                         VnBindingDesc* out) {
    if (size != kPackV2BindingDefinitionPayloadBytes ||
        LoadLE32(in + 44) != 0) {
        return false;
    }
    *outId = LoadObjectId(in + 0);
    *out = VnBindingDesc{};
    out->structSize = sizeof(VnBindingDesc);
    out->variableId = LoadObjectId(in + 16);
    out->targetScopeKind = LoadLE32(in + 32);
    out->targetId = LoadLE32(in + 36);
    out->transitionMs = LoadLE32(in + 40);
    return ParsePackV2BindingCurve(in + 48, &out->curve);
}

inline void SerializePackV2BindingAttachment(
    const PackBindingAttachmentEntry& e, std::uint8_t* out) {
    StoreObjectId(out + 0, e.bindingId);
    StoreLE32(out + 16, e.scopeKind);
    StoreLE32(out + 20, 0);
    StoreObjectId(out + 24, e.objectId);
}

inline bool ParsePackV2BindingAttachment(const std::uint8_t* in,
                                         std::uint64_t size,
                                         PackBindingAttachmentEntry* out) {
    if (size != kPackV2BindingAttachmentPayloadBytes ||
        LoadLE32(in + 20) != 0) {
        return false;
    }
    out->bindingId = LoadObjectId(in + 0);
    out->scopeKind = LoadLE32(in + 16);
    out->objectId = LoadObjectId(in + 24);
    return true;
}

// Voice Recipe: a 16-byte header followed by installCount fixed install
// records and bindingCount u64 binding ids. Each install record carries a
// bounded parameter payload in canonical per-type parameter-index order. The
// fixed 64-float payload keeps parsing bounded and lets future built-ins grow
// without changing record alignment; unused float slots and both reserved
// words must be bitwise zero. The image layer enforces ascending slot order,
// ascending binding ids, type/parameter semantics, and playable closure.
inline void SerializePackV2VoiceRecipeHeader(
    const PackVoiceRecipeEntryHeader& h, std::uint8_t* out) {
    StoreObjectId(out + 0, h.playableId);
    StoreLE32(out + 16, h.installCount);
    StoreLE32(out + 20, h.bindingCount);
}

inline bool ParsePackV2VoiceRecipeHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackVoiceRecipeEntryHeader* out) {
    if (size < kPackV2VoiceRecipeHeaderBytes) {
        return false;
    }
    out->playableId = LoadObjectId(in + 0);
    out->installCount = LoadLE32(in + 16);
    out->bindingCount = LoadLE32(in + 20);
    if (out->installCount == 0 ||
        out->installCount > kPackV2VoiceRecipeMaxInstalls ||
        out->bindingCount > kPackV2VoiceRecipeMaxBindings) {
        return false;
    }
    return size == PackV2VoiceRecipePayloadBytes(out->installCount,
                                                 out->bindingCount);
}

inline bool ParsePackV3VoiceRecipeHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackVoiceRecipeEntryHeader* out) {
    if (size < kPackV2VoiceRecipeHeaderBytes +
                   kPackV3VoiceRecipeToneBytes) {
        return false;
    }
    out->playableId = LoadObjectId(in + 0);
    out->installCount = LoadLE32(in + 16);
    out->bindingCount = LoadLE32(in + 20);
    if (out->installCount > kPackV2VoiceRecipeMaxInstalls ||
        out->bindingCount > kPackV2VoiceRecipeMaxBindings) {
        return false;
    }
    return size == PackV3VoiceRecipePayloadBytes(out->installCount,
                                                 out->bindingCount);
}

inline bool ParsePackV4VoiceRecipeHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackVoiceRecipeEntryHeader* out) {
    if (size < kPackV2VoiceRecipeHeaderBytes +
                   kPackV3VoiceRecipeToneBytes +
                   kPackV4VoiceRecipeSpatialBytes) {
        return false;
    }
    out->playableId = LoadObjectId(in + 0);
    out->installCount = LoadLE32(in + 16);
    out->bindingCount = LoadLE32(in + 20);
    if (out->installCount > kPackV2VoiceRecipeMaxInstalls ||
        out->bindingCount > kPackV2VoiceRecipeMaxBindings) {
        return false;
    }
    return size == PackV4VoiceRecipePayloadBytes(out->installCount,
                                                 out->bindingCount);
}

inline bool ParsePackV5VoiceRecipeHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackVoiceRecipeEntryHeader* out) {
    if (size < kPackV2VoiceRecipeHeaderBytes +
                   kPackV3VoiceRecipeToneBytes +
                   kPackV4VoiceRecipeSpatialBytes +
                   kPackV5VoiceRecipePlaybackBytes) {
        return false;
    }
    out->playableId = LoadObjectId(in + 0);
    out->installCount = LoadLE32(in + 16);
    out->bindingCount = LoadLE32(in + 20);
    if (out->installCount > kPackV2VoiceRecipeMaxInstalls ||
        out->bindingCount > kPackV2VoiceRecipeMaxBindings) {
        return false;
    }
    return size == PackV5VoiceRecipePayloadBytes(out->installCount,
                                                 out->bindingCount);
}

inline void SerializePackV3VoiceRecipeTone(
    const PackVoiceRecipeTone& tone, std::uint8_t* out) {
    StoreLE32(out + 0, tone.present);
    StoreLE32(out + 4, tone.sectionEnableMask);
    StoreLEF32(out + 8, tone.lowpassFrequencyHz);
    StoreLEF32(out + 12, tone.lowpassQ);
    StoreLEF32(out + 16, tone.highpassFrequencyHz);
    StoreLEF32(out + 20, tone.highpassQ);
    StoreLEF32(out + 24, tone.lowShelfFrequencyHz);
    StoreLEF32(out + 28, tone.lowShelfQ);
    StoreLEF32(out + 32, tone.lowShelfGainDb);
    StoreLEF32(out + 36, tone.highShelfFrequencyHz);
    StoreLEF32(out + 40, tone.highShelfQ);
    StoreLEF32(out + 44, tone.highShelfGainDb);
}

inline bool ParsePackV3VoiceRecipeTone(
    const std::uint8_t* in, PackVoiceRecipeTone* out) {
    out->present = LoadLE32(in + 0);
    out->sectionEnableMask = LoadLE32(in + 4);
    if (out->present > 1 ||
        (out->sectionEnableMask & ~kPackVoiceToneEnableMask) != 0 ||
        (out->present == 0 && out->sectionEnableMask != 0)) {
        return false;
    }
    out->lowpassFrequencyHz = LoadLEF32(in + 8);
    out->lowpassQ = LoadLEF32(in + 12);
    out->highpassFrequencyHz = LoadLEF32(in + 16);
    out->highpassQ = LoadLEF32(in + 20);
    out->lowShelfFrequencyHz = LoadLEF32(in + 24);
    out->lowShelfQ = LoadLEF32(in + 28);
    out->lowShelfGainDb = LoadLEF32(in + 32);
    out->highShelfFrequencyHz = LoadLEF32(in + 36);
    out->highShelfQ = LoadLEF32(in + 40);
    out->highShelfGainDb = LoadLEF32(in + 44);
    if (out->present == 0) {
        for (std::uint32_t i = 8; i < kPackV3VoiceRecipeToneBytes;
             i += 4) {
            if (LoadLE32(in + i) != 0) {
                return false;
            }
        }
    }
    return true;
}

inline void SerializePackV4VoiceRecipeSpatial(
    const PackVoiceRecipeSpatial& spatial, std::uint8_t* out) {
    StoreObjectId(out + 0, spatial.attenuationId);
    StoreLEF32(out + 16, spatial.distanceScale);
    StoreLE32(out + 20, 0);
}

inline bool ParsePackV4VoiceRecipeSpatial(
    const std::uint8_t* in, PackVoiceRecipeSpatial* out) {
    out->attenuationId = LoadObjectId(in + 0);
    out->distanceScale = LoadLEF32(in + 16);
    out->reserved0 = LoadLE32(in + 20);
    return out->attenuationId != VnObjectId{} &&
           std::isfinite(out->distanceScale) &&
           out->distanceScale > 0.0f &&
           out->reserved0 == 0;
}

// Vintage 5 makes the spatial block positional rather than a promise of
// spatial authoring, so it also admits an all-zero absent form: a recipe can
// carry playback policy without carrying an attenuation.
inline bool ParsePackV5VoiceRecipeSpatial(
    const std::uint8_t* in, PackVoiceRecipeSpatial* out) {
    out->attenuationId = LoadObjectId(in + 0);
    out->distanceScale = LoadLEF32(in + 16);
    out->reserved0 = LoadLE32(in + 20);
    if (out->reserved0 != 0) {
        return false;
    }
    if (out->attenuationId == VnObjectId{}) {
        return LoadLE32(in + 16) == 0;
    }
    return std::isfinite(out->distanceScale) && out->distanceScale > 0.0f;
}

inline void SerializePackV5VoiceRecipePlayback(
    const PackVoiceRecipePlayback& playback, std::uint8_t* out) {
    StoreLE32(out + 0, playback.present);
    StoreLE32(out + 4, playback.loopCount);
}

inline bool ParsePackV5VoiceRecipePlayback(
    const std::uint8_t* in, PackVoiceRecipePlayback* out) {
    out->present = LoadLE32(in + 0);
    out->loopCount = LoadLE32(in + 4);
    if (out->present > 1) {
        return false;
    }
    return out->present != 0 || out->loopCount == 0;
}

inline void SerializePackV2VoiceRecipeInstall(
    const PackVoiceRecipeInstall& install, std::uint8_t* out) {
    StoreObjectId(out + 288, install.effectId);
    StoreLE32(out + 0, install.slot);
    StoreLE32(out + 4, install.effectType);
    StoreLE32(out + 8, install.descriptorVintage);
    StoreLE32(out + 12, install.bypass);
    StoreLE32(out + 16, install.paramCount);
    StoreLE32(out + 20, install.paramCount * 4);
    StoreLE32(out + 24, 0);
    StoreLE32(out + 28, 0);
    for (std::uint32_t i = 0; i < kPackV2VoiceRecipeMaxParams; ++i) {
        StoreLEF32(out + 32 + static_cast<std::size_t>(i) * 4,
                   i < install.paramCount ? install.params[i] : 0.0f);
    }
}

inline bool ParsePackV2VoiceRecipeInstall(
    const std::uint8_t* in, PackVoiceRecipeInstall* out) {
    out->effectId = LoadObjectId(in + 288);
    out->slot = LoadLE32(in + 0);
    out->effectType = LoadLE32(in + 4);
    out->descriptorVintage = LoadLE32(in + 8);
    out->bypass = LoadLE32(in + 12);
    out->paramCount = LoadLE32(in + 16);
    const std::uint32_t payloadBytes = LoadLE32(in + 20);
    if (out->bypass > 1 ||
        out->paramCount > kPackV2VoiceRecipeMaxParams ||
        payloadBytes != out->paramCount * 4 ||
        LoadLE32(in + 24) != 0 || LoadLE32(in + 28) != 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < kPackV2VoiceRecipeMaxParams; ++i) {
        out->params[i] =
            LoadLEF32(in + 32 + static_cast<std::size_t>(i) * 4);
        if (i >= out->paramCount &&
            LoadLE32(in + 32 + static_cast<std::size_t>(i) * 4) != 0) {
            return false;
        }
    }
    return true;
}

inline void SerializePackV2VoiceRecipeBinding(VnBindingId bindingId,
                                               std::uint8_t* out) {
    StoreObjectId(out, bindingId);
}

inline VnBindingId ParsePackV2VoiceRecipeBinding(const std::uint8_t* in) {
    return LoadObjectId(in);
}

// --- variable-size logic payloads ---

// Container: header (32) then childCount children of 24 bytes each —
// playableId(8) + weight(4) + matchValue(4) + delayMs(4) +
// delayJitterMs(4). The serializer writes header + children from the
// caller's array; the parser fills the header and then reads children
// one at a time into caller storage (childCount entries, validated
// against the payload size here).
inline void SerializePackV2ContainerHeader(const PackContainerEntryHeader& h,
                                           std::uint8_t* out) {
    StoreObjectId(out + 0, h.containerId);
    StoreLE32(out + 16, h.selectionOrder);
    StoreLE32(out + 20, h.avoidRepeatWindow);
    StoreObjectId(out + 24, h.selectionVariableId);
    StoreLE32(out + 40, h.defaultIndex);
    StoreLE32(out + 44, h.childCount);
}

inline void SerializePackV2ContainerChild(const PackContainerChildV2& c,
                                          std::uint8_t* out) {
    StoreObjectId(out + 0, c.playableId);
    StoreLE32(out + 16, c.weight);
    StoreLE32(out + 20, c.matchValue);
    StoreLE32(out + 24, c.delayMs);
    StoreLE32(out + 28, c.delayJitterMs);
}

inline bool ParsePackV2ContainerHeader(const std::uint8_t* in,
                                       std::uint64_t size,
                                       PackContainerEntryHeader* out) {
    if (size < kPackV2ContainerHeaderBytes) {
        return false;
    }
    out->containerId = LoadObjectId(in + 0);
    out->selectionOrder = LoadLE32(in + 16);
    out->avoidRepeatWindow = LoadLE32(in + 20);
    out->selectionVariableId = LoadObjectId(in + 24);
    out->defaultIndex = LoadLE32(in + 40);
    out->childCount = LoadLE32(in + 44);
    return size == PackV2ContainerPayloadBytes(out->childCount);
}

inline PackContainerChildV2 ParsePackV2ContainerChild(const std::uint8_t* in) {
    PackContainerChildV2 c{};
    c.playableId = LoadObjectId(in + 0);
    c.weight = LoadLE32(in + 16);
    c.matchValue = LoadLE32(in + 20);
    c.delayMs = LoadLE32(in + 24);
    c.delayJitterMs = LoadLE32(in + 28);
    return c;
}

inline constexpr std::uint32_t kInitCatalogHeaderBytes = 88;
inline constexpr std::uint32_t kInitCatalogRowBytes = 312;
inline constexpr std::uint32_t kInitCatalogContentHashOffset = 304;
inline constexpr std::uint32_t kInitCatalogMaxLocatorBytes = 256;
inline constexpr std::uint32_t kInitCatalogMaxStartupPacks = 256;
inline constexpr std::uint32_t kInitCatalogMaxEntries = 1048576;
inline constexpr std::uint32_t kPackRequirementBlockBytes = 112;
inline constexpr std::uint32_t kPackRequirementBlockV2Bytes = 136;
inline constexpr std::uint32_t kPackDependencyEdgeBytes = 32;
inline constexpr std::uint32_t kPackDependencyEdgeV2Bytes = 48;
inline constexpr std::uint32_t kCanonicalDefinitionPartitionHeaderBytes = 64;
inline constexpr std::uint32_t kCanonicalDefinitionPartitionHeaderV2Bytes = 88;
inline constexpr std::uint32_t kPackBusDefinitionSnapshotHeaderBytes = 160;
inline constexpr std::uint32_t kPackBusDefinitionSnapshotV2HeaderBytes = 184;
inline constexpr std::uint32_t kPackBusDefinitionBusRowBytes = 64;
inline constexpr std::uint32_t kPackBusDefinitionSendRowBytes = 32;
inline constexpr std::uint32_t kPackBusDefinitionBusV2RowBytes = 80;
inline constexpr std::uint32_t kPackBusDefinitionSendV2RowBytes = 48;
inline constexpr std::uint32_t kPackBusDefinitionDownmixV2RowBytes = 40;
inline constexpr std::uint32_t kPackBusDefinitionEffectRowBytes = 48;
inline constexpr std::uint32_t kPackBusDefinitionEffectParameterRowBytes = 16;
inline constexpr std::uint32_t kPackBusDefinitionDownmixRowBytes = 32;
inline constexpr std::uint32_t kPackBusDefinitionCoefficientRowBytes = 4;
inline constexpr std::uint32_t kPackBusDefinitionDownmixCoefficientCount = 64;
inline constexpr char kCanonicalDefinitionPartitionMagic[8] = {
    'V', 'N', 'D', 'E', 'F', 'P', '0', '1'};
inline constexpr char kCanonicalDefinitionPartitionMagicV2[8] = {
    'V', 'N', 'D', 'E', 'F', 'P', '0', '2'};
// Internal layout id of the pack playback-program chapter (CurrentWorld
// body). Same role as directory vintage: not a product schema beside
// projectSchemaVersion / packFormatVersion, and not an independent
// catalog format. Shipping writers emit this table; a different value
// is VN_PACK_INCOMPATIBLE, not VN_BAD_PACK, and not a second program
// contract.
inline constexpr std::uint32_t kCanonicalDefinitionPartitionBodyVintage =
    kCurrentWorldShippingBodyVintage;
inline constexpr std::uint32_t kInitCatalogIdentityClassAction = 1;
inline constexpr std::uint32_t kInitCatalogIdentityClassPlayable = 2;
inline constexpr std::uint32_t kInitCatalogIdentityClassVariable = 3;
inline constexpr std::uint32_t kInitCatalogIdentityClassResource = 4;
inline constexpr std::uint32_t kInitCatalogFlagStartupRequired = 1u;
inline constexpr std::uint32_t kPackDependencyEdgeKindInit = 1;
inline constexpr std::uint32_t kPackDependencyEdgeKindShared = 2;
inline constexpr std::uint32_t kPackDependencyEdgeKindContent = 3;

inline bool PackV2IsCanonicalKind(std::uint32_t packKind) {
    return packKind == kPackKindInit || packKind == kPackKindCanonicalContent;
}

inline bool PackV2IsCanonicalEntryKind(std::uint32_t entryKind) {
    return entryKind == kPackEntryKindInitRoutingCatalog ||
           entryKind == kPackEntryKindCanonicalDefinitionPartition ||
           entryKind == kPackEntryKindPackRequirementBlock ||
           entryKind == kPackEntryKindPackDependencyEdge ||
           entryKind == kPackEntryKindBusDefinitionSnapshot ||
           entryKind == kPackEntryKindGameplayCatalog ||
           entryKind == kPackEntryKindVariablePolicyOverlay ||
           entryKind == kPackEntryKindVariableRequirements ||
           entryKind == kPackEntryKindVariableNameCatalog;
}

inline std::uint32_t PackV2CanonicalDirectoryVintage(std::uint32_t entryKind) {
    if (entryKind == kPackEntryKindCanonicalDefinitionPartition ||
        entryKind == kPackEntryKindPackRequirementBlock ||
        entryKind == kPackEntryKindPackDependencyEdge) {
        return kPackCanonicalRequirementVintage;
    }
    if (entryKind == kPackEntryKindBusDefinitionSnapshot) {
        return kPackBusDefinitionSnapshotVintage;
    }
    return kPackCanonicalEntryVintage;
}

inline bool CheckedMulU64(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

inline bool CheckedAddU64(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (b > UINT64_MAX - a) {
        return false;
    }
    *out = a + b;
    return true;
}

inline bool InitCatalogBytes(std::uint32_t entryCount, std::uint32_t startupCount,
                             std::uint64_t* outBytes) {
    std::uint64_t rowBytes = 0;
    std::uint64_t startupBytes = 0;
    std::uint64_t afterRows = 0;
    if (!CheckedMulU64(entryCount, kInitCatalogRowBytes, &rowBytes) ||
        !CheckedMulU64(startupCount, 8, &startupBytes) ||
        !CheckedAddU64(kInitCatalogHeaderBytes, rowBytes, &afterRows) ||
        !CheckedAddU64(afterRows, startupBytes, outBytes)) {
        return false;
    }
    return true;
}

inline bool GameplayCatalogBytes(std::uint32_t activeCount,
                                 std::uint32_t retiredCount,
                                 std::uint32_t namePoolBytes,
                                 std::uint64_t* outBytes) {
    std::uint64_t activeBytes = 0;
    std::uint64_t retiredBytes = 0;
    std::uint64_t afterHeader = 0;
    std::uint64_t afterActive = 0;
    if (!CheckedMulU64(activeCount, kGameplayCatalogActiveRowBytes,
                       &activeBytes) ||
        !CheckedMulU64(retiredCount, kGameplayCatalogRetiredRowBytes,
                       &retiredBytes) ||
        !CheckedAddU64(kGameplayCatalogHeaderBytes, activeBytes, &afterHeader) ||
        !CheckedAddU64(afterHeader, retiredBytes, &afterActive) ||
        !CheckedAddU64(afterActive, namePoolBytes, outBytes)) {
        return false;
    }
    return true;
}

struct PackInitCatalogHeader {
    std::uint32_t schemaVersion = 0;
    std::uint32_t storageModelVersion = 0;
    std::uint64_t projectBuildId = 0;
    std::uint32_t platformId = 0;
    std::uint32_t languageId = 0;
    std::uint64_t totalDefinitionCount = 0;
    std::uint64_t totalResourceCount = 0;
    std::uint32_t catalogEntryCount = 0;
    std::uint32_t startupPackCount = 0;
    std::uint64_t catalogBytes = 0;
    std::uint32_t recommendedCatalogCapacity = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t recommendedCatalogBytes = 0;
    std::uint64_t checksum = 0;
    std::uint64_t reserved1 = 0;
};

struct PackInitCatalogRow {
    VnObjectId stableId{};
    std::uint32_t identityClass = 0;
    std::uint32_t flags = 0;
    std::uint64_t ownerPackStableId = 0;
    std::uint64_t prerequisitePackStableId = 0;
    std::uint32_t variantKey = 0;
    std::uint32_t locatorByteCount = 0;
    std::array<char, kInitCatalogMaxLocatorBytes> relativeLocator{};
    std::uint64_t expectedContentHash = 0;
};

struct PackRequirementBlock {
    std::uint32_t schemaVersion = 0;
    std::uint32_t storageModelVersion = 0;
    std::uint64_t packStableId = 0;
    std::uint64_t packContentRevision = 0;
    std::uint64_t definitionPartitionRevision = 0;
    std::uint64_t resourceClosureRevision = 0;
    std::uint64_t semanticFingerprint = 0;
    std::uint64_t lineageFingerprint = 0;
    std::uint32_t ownedDefinitionCount = 0;
    std::uint32_t ownedResourceCount = 0;
    std::uint64_t installedDefinitionBytes = 0;
    std::uint64_t installedResourceBytes = 0;
    std::uint64_t replacementPeakBytes = 0;
    std::uint64_t decodeStagingBytes = 0;
    std::uint32_t requiredCapabilityMask = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t payloadChecksumCoverage = 0;
    std::uint64_t busContentGeneration = 0;
    std::uint64_t publishedBusFingerprint = 0;
    std::uint64_t compileTargetProfileFingerprint = 0;
};

struct PackDependencyEdge {
    std::uint64_t prerequisitePackStableId = 0;
    std::uint32_t edgeKind = 0;
    std::uint32_t required01 = 0;
    std::uint64_t expectedPrerequisiteContentRevision = 0;
    std::uint64_t reserved0 = 0;
    std::uint64_t expectedBusContentGeneration = 0;
    std::uint64_t expectedPublishedBusFingerprint = 0;
    std::uint64_t expectedCompileTargetProfileFingerprint = 0;
};

struct PackBusDefinitionSnapshotHeaderV1 {
    std::uint32_t schemaVersion = 0;
    std::uint32_t storageModelVersion = 0;
    std::uint32_t headerBytes = 0;
    std::uint32_t publicationCapabilityVintage = 0;
    std::uint64_t contentGeneration = 0;
    std::uint64_t publishedBusFingerprint = 0;
    std::uint32_t busCount = 0;
    std::uint32_t sendCount = 0;
    std::uint32_t effectCount = 0;
    std::uint32_t effectParameterCount = 0;
    std::uint32_t downmixCount = 0;
    std::uint32_t coefficientCount = 0;
    std::uint64_t busOffset = 0;
    std::uint64_t sendOffset = 0;
    std::uint64_t effectOffset = 0;
    std::uint64_t effectParameterOffset = 0;
    std::uint64_t downmixOffset = 0;
    std::uint64_t coefficientOffset = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t checksum = 0;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::uint32_t profileVintage = 0;
    std::uint32_t renderSampleRateHz = 0;
    std::uint32_t mixLayout = 0;
    std::uint32_t deviceLayout = 0;
    std::uint64_t runtimeCapabilityMask = 0;
    std::uint64_t reserved0 = 0;
    std::uint32_t busRowBytes = 0;
    std::uint32_t sendRowBytes = 0;
    std::uint32_t downmixRowBytes = 0;
    std::uint32_t coefficientRowBytes = 0;
    std::uint64_t reserved1 = 0;
};

// Kind-24 snapshot still stores bus ids as uint64 low words. Live runtime
// buses are VnObjectId; master is {0, 1}. Convert at the Activate boundary
// instead of widening the snapshot wire (that is U3-10b).
inline constexpr VnBusId CanonicalSnapshotBusIdV1(std::uint64_t packed) noexcept {
    return packed == 0 ? VnObjectId{} : VN_OBJECT_ID(0, packed);
}

inline constexpr std::uint64_t CanonicalSnapshotPackedBusIdV1(VnBusId id) noexcept {
    return id.high == 0 ? id.low : 0;
}

inline constexpr bool CanonicalSnapshotIsMasterV1(std::uint64_t packed) noexcept {
    return packed == VN_BUS_MASTER.low;
}

struct PackBusDefinitionBusV1 {
    std::uint64_t busId = 0;
    std::uint64_t parentId = 0;
    std::uint32_t feedsFirstOrdinal = 0;
    std::uint32_t flags = 0;
    float faderDb = 0.0f;
    std::uint32_t faderAfterSlot = 0;
    std::uint32_t outputLayout = 0;
    std::uint32_t effectFirst = 0;
    std::uint32_t effectCount = 0;
    std::uint32_t downmixFirst = 0;
    std::uint32_t downmixCount = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
};

struct PackBusDefinitionSendV1 {
    std::uint64_t source = 0;
    std::uint64_t target = 0;
    float levelDb = 0.0f;
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
};

struct PackBusDefinitionEffectV1 {
    std::uint64_t busId = 0;
    std::uint64_t effectStableId = 0;
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t flags = 0;
    std::uint32_t parameterFirst = 0;
    std::uint32_t parameterCount = 0;
    std::uint64_t reserved0 = 0;
};

struct PackBusDefinitionEffectParameterV1 {
    std::uint32_t parameterIndex = 0;
    float value = 0.0f;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct PackBusDefinitionDownmixV1 {
    std::uint64_t busId = 0;
    std::uint32_t sourceLayout = 0;
    std::uint32_t destinationLayout = 0;
    std::uint32_t coefficientFirst = 0;
    std::uint32_t coefficientCount = 0;
    std::uint64_t reserved0 = 0;
};

struct PackBusDefinitionBusV2 {
    VnObjectId busId{};
    VnObjectId parentId{};
    std::uint32_t feedsFirstOrdinal = 0;
    float faderDb = 0.0f;
    float makeUpGainDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    std::uint32_t faderAfterSlot = 0;
    std::uint32_t outputLayout = 0;
    std::uint32_t effectFirst = 0;
    std::uint32_t effectCount = 0;
    std::uint32_t downmixFirst = 0;
    std::uint32_t downmixCount = 0;
};

struct PackBusDefinitionSendV2 {
    VnObjectId source{};
    VnObjectId target{};
    float levelDb = 0.0f;
};

struct PackBusDefinitionDownmixV2 {
    VnObjectId busId{};
    std::uint32_t sourceLayout = 0;
    std::uint32_t destinationLayout = 0;
    std::uint32_t coefficientFirst = 0;
    std::uint32_t coefficientCount = 0;
};

struct PackCanonicalPartitionHeader {
    std::uint32_t headerBytes = 0;
    std::uint32_t bodyVintage = 0; // internal program layout id, not a product schema
    std::uint64_t bodyBytes = 0;
    std::uint64_t bodyChecksum = 0;
    std::uint64_t packStableId = 0;
    std::uint64_t partitionRevision = 0;
    std::uint64_t ownedDefinitionCount = 0;
    std::uint64_t headerChecksum = 0;
    std::uint64_t busContentGeneration = 0;
    std::uint64_t publishedBusFingerprint = 0;
    std::uint64_t compileTargetProfileFingerprint = 0;
};

inline void SerializePackInitCatalogHeader(const PackInitCatalogHeader& h,
                                           std::uint8_t* out) {
    StoreLE32(out + 0, h.schemaVersion);
    StoreLE32(out + 4, h.storageModelVersion);
    StoreLE64(out + 8, h.projectBuildId);
    StoreLE32(out + 16, h.platformId);
    StoreLE32(out + 20, h.languageId);
    StoreLE64(out + 24, h.totalDefinitionCount);
    StoreLE64(out + 32, h.totalResourceCount);
    StoreLE32(out + 40, h.catalogEntryCount);
    StoreLE32(out + 44, h.startupPackCount);
    StoreLE64(out + 48, h.catalogBytes);
    StoreLE32(out + 56, h.recommendedCatalogCapacity);
    StoreLE32(out + 60, h.reserved0);
    StoreLE64(out + 64, h.recommendedCatalogBytes);
    StoreLE64(out + 72, h.checksum);
    StoreLE64(out + 80, h.reserved1);
}

inline bool ParsePackInitCatalogHeader(const std::uint8_t* in,
                                       std::uint64_t size,
                                       PackInitCatalogHeader* out) {
    if (size < kInitCatalogHeaderBytes) {
        return false;
    }
    out->schemaVersion = LoadLE32(in + 0);
    out->storageModelVersion = LoadLE32(in + 4);
    out->projectBuildId = LoadLE64(in + 8);
    out->platformId = LoadLE32(in + 16);
    out->languageId = LoadLE32(in + 20);
    out->totalDefinitionCount = LoadLE64(in + 24);
    out->totalResourceCount = LoadLE64(in + 32);
    out->catalogEntryCount = LoadLE32(in + 40);
    out->startupPackCount = LoadLE32(in + 44);
    out->catalogBytes = LoadLE64(in + 48);
    out->recommendedCatalogCapacity = LoadLE32(in + 56);
    out->reserved0 = LoadLE32(in + 60);
    out->recommendedCatalogBytes = LoadLE64(in + 64);
    out->checksum = LoadLE64(in + 72);
    out->reserved1 = LoadLE64(in + 80);
    return out->schemaVersion == 1 && out->storageModelVersion == 1 &&
           out->reserved0 == 0 && out->reserved1 == 0;
}

inline void SerializePackInitCatalogRow(const PackInitCatalogRow& row,
                                        std::uint8_t* out) {
    StoreObjectId(out + 0, row.stableId);
    StoreLE32(out + 16, row.identityClass);
    StoreLE32(out + 20, row.flags);
    StoreLE64(out + 24, row.ownerPackStableId);
    StoreLE64(out + 32, row.prerequisitePackStableId);
    StoreLE32(out + 40, row.variantKey);
    StoreLE32(out + 44, row.locatorByteCount);
    std::memcpy(out + 48, row.relativeLocator.data(),
                kInitCatalogMaxLocatorBytes);
    StoreLE64(out + kInitCatalogContentHashOffset, row.expectedContentHash);
}

inline bool ParsePackInitCatalogRow(const std::uint8_t* in,
                                    PackInitCatalogRow* out) {
    out->stableId = LoadObjectId(in + 0);
    out->identityClass = LoadLE32(in + 16);
    out->flags = LoadLE32(in + 20);
    out->ownerPackStableId = LoadLE64(in + 24);
    out->prerequisitePackStableId = LoadLE64(in + 32);
    out->variantKey = LoadLE32(in + 40);
    out->locatorByteCount = LoadLE32(in + 44);
    std::memcpy(out->relativeLocator.data(), in + 48,
                kInitCatalogMaxLocatorBytes);
    out->expectedContentHash = LoadLE64(in + kInitCatalogContentHashOffset);
    if (out->stableId == VnObjectId{} || out->ownerPackStableId == 0 ||
        out->locatorByteCount == 0 ||
        out->locatorByteCount > kInitCatalogMaxLocatorBytes ||
        (out->flags & ~kInitCatalogFlagStartupRequired) != 0 ||
        (out->identityClass < kInitCatalogIdentityClassAction ||
         out->identityClass > kInitCatalogIdentityClassResource)) {
        return false;
    }
    if (out->relativeLocator[out->locatorByteCount - 1] != '\0') {
        return false;
    }
    for (std::uint32_t i = out->locatorByteCount; i < kInitCatalogMaxLocatorBytes;
         ++i) {
        if (out->relativeLocator[i] != 0) {
            return false;
        }
    }
    const char* locator = out->relativeLocator.data();
    if (locator[0] == '/' || locator[0] == '\\') {
        return false;
    }
    for (std::uint32_t i = 0; i + 1 < out->locatorByteCount; ++i) {
        if (locator[i] == '\\' || locator[i] == ':' ||
            (locator[i] == '.' && locator[i + 1] == '.')) {
            return false;
        }
    }
    return true;
}

inline void SerializePackRequirementBlock(const PackRequirementBlock& block,
                                          std::uint8_t* out) {
    StoreLE32(out + 0, block.schemaVersion);
    StoreLE32(out + 4, block.storageModelVersion);
    StoreLE64(out + 8, block.packStableId);
    StoreLE64(out + 16, block.packContentRevision);
    StoreLE64(out + 24, block.definitionPartitionRevision);
    StoreLE64(out + 32, block.resourceClosureRevision);
    StoreLE64(out + 40, block.semanticFingerprint);
    StoreLE64(out + 48, block.lineageFingerprint);
    StoreLE32(out + 56, block.ownedDefinitionCount);
    StoreLE32(out + 60, block.ownedResourceCount);
    StoreLE64(out + 64, block.installedDefinitionBytes);
    StoreLE64(out + 72, block.installedResourceBytes);
    StoreLE64(out + 80, block.replacementPeakBytes);
    StoreLE64(out + 88, block.decodeStagingBytes);
    StoreLE32(out + 96, block.requiredCapabilityMask);
    StoreLE32(out + 100, block.reserved0);
    StoreLE64(out + 104, block.payloadChecksumCoverage);
}

inline bool ParsePackRequirementBlock(const std::uint8_t* in, std::uint64_t size,
                                      PackRequirementBlock* out) {
    if (size != kPackRequirementBlockBytes) {
        return false;
    }
    out->schemaVersion = LoadLE32(in + 0);
    out->storageModelVersion = LoadLE32(in + 4);
    out->packStableId = LoadLE64(in + 8);
    out->packContentRevision = LoadLE64(in + 16);
    out->definitionPartitionRevision = LoadLE64(in + 24);
    out->resourceClosureRevision = LoadLE64(in + 32);
    out->semanticFingerprint = LoadLE64(in + 40);
    out->lineageFingerprint = LoadLE64(in + 48);
    out->ownedDefinitionCount = LoadLE32(in + 56);
    out->ownedResourceCount = LoadLE32(in + 60);
    out->installedDefinitionBytes = LoadLE64(in + 64);
    out->installedResourceBytes = LoadLE64(in + 72);
    out->replacementPeakBytes = LoadLE64(in + 80);
    out->decodeStagingBytes = LoadLE64(in + 88);
    out->requiredCapabilityMask = LoadLE32(in + 96);
    out->reserved0 = LoadLE32(in + 100);
    out->payloadChecksumCoverage = LoadLE64(in + 104);
    return out->schemaVersion == 1 && out->storageModelVersion == 1 &&
           out->packStableId != 0 && out->reserved0 == 0;
}

inline void SerializePackDependencyEdge(const PackDependencyEdge& edge,
                                        std::uint8_t* out) {
    StoreLE64(out + 0, edge.prerequisitePackStableId);
    StoreLE32(out + 8, edge.edgeKind);
    StoreLE32(out + 12, edge.required01);
    StoreLE64(out + 16, edge.expectedPrerequisiteContentRevision);
    StoreLE64(out + 24, edge.reserved0);
}

inline bool ParsePackDependencyEdge(const std::uint8_t* in, std::uint64_t size,
                                    PackDependencyEdge* out) {
    if (size != kPackDependencyEdgeBytes) {
        return false;
    }
    out->prerequisitePackStableId = LoadLE64(in + 0);
    out->edgeKind = LoadLE32(in + 8);
    out->required01 = LoadLE32(in + 12);
    out->expectedPrerequisiteContentRevision = LoadLE64(in + 16);
    out->reserved0 = LoadLE64(in + 24);
    return out->prerequisitePackStableId != 0 &&
           (out->edgeKind == kPackDependencyEdgeKindInit ||
            out->edgeKind == kPackDependencyEdgeKindShared ||
            out->edgeKind == kPackDependencyEdgeKindContent) &&
           out->required01 <= 1 && out->reserved0 == 0;
}

inline void SerializePackRequirementBlockV2(const PackRequirementBlock& block,
                                            std::uint8_t* out) {
    SerializePackRequirementBlock(block, out);
    StoreLE64(out + 112, block.busContentGeneration);
    StoreLE64(out + 120, block.publishedBusFingerprint);
    StoreLE64(out + 128, block.compileTargetProfileFingerprint);
}

inline bool ParsePackRequirementBlockV2(const std::uint8_t* in, std::uint64_t size,
                                        PackRequirementBlock* out) {
    if (size != kPackRequirementBlockV2Bytes) {
        return false;
    }
    out->schemaVersion = LoadLE32(in + 0);
    out->storageModelVersion = LoadLE32(in + 4);
    out->packStableId = LoadLE64(in + 8);
    out->packContentRevision = LoadLE64(in + 16);
    out->definitionPartitionRevision = LoadLE64(in + 24);
    out->resourceClosureRevision = LoadLE64(in + 32);
    out->semanticFingerprint = LoadLE64(in + 40);
    out->lineageFingerprint = LoadLE64(in + 48);
    out->ownedDefinitionCount = LoadLE32(in + 56);
    out->ownedResourceCount = LoadLE32(in + 60);
    out->installedDefinitionBytes = LoadLE64(in + 64);
    out->installedResourceBytes = LoadLE64(in + 72);
    out->replacementPeakBytes = LoadLE64(in + 80);
    out->decodeStagingBytes = LoadLE64(in + 88);
    out->requiredCapabilityMask = LoadLE32(in + 96);
    out->reserved0 = LoadLE32(in + 100);
    out->payloadChecksumCoverage = LoadLE64(in + 104);
    out->busContentGeneration = LoadLE64(in + 112);
    out->publishedBusFingerprint = LoadLE64(in + 120);
    out->compileTargetProfileFingerprint = LoadLE64(in + 128);
    return out->schemaVersion == 2 && out->storageModelVersion == 2 &&
           out->packStableId != 0 && out->reserved0 == 0;
}

inline void SerializePackDependencyEdgeV2(const PackDependencyEdge& edge,
                                          std::uint8_t* out) {
    StoreLE64(out + 0, edge.prerequisitePackStableId);
    StoreLE32(out + 8, edge.edgeKind);
    StoreLE32(out + 12, edge.required01);
    StoreLE64(out + 16, edge.expectedPrerequisiteContentRevision);
    StoreLE64(out + 24, edge.expectedBusContentGeneration);
    StoreLE64(out + 32, edge.expectedPublishedBusFingerprint);
    StoreLE64(out + 40, edge.expectedCompileTargetProfileFingerprint);
}

inline bool ParsePackDependencyEdgeV2(const std::uint8_t* in, std::uint64_t size,
                                      PackDependencyEdge* out) {
    if (size != kPackDependencyEdgeV2Bytes) {
        return false;
    }
    out->prerequisitePackStableId = LoadLE64(in + 0);
    out->edgeKind = LoadLE32(in + 8);
    out->required01 = LoadLE32(in + 12);
    out->expectedPrerequisiteContentRevision = LoadLE64(in + 16);
    out->reserved0 = 0;
    out->expectedBusContentGeneration = LoadLE64(in + 24);
    out->expectedPublishedBusFingerprint = LoadLE64(in + 32);
    out->expectedCompileTargetProfileFingerprint = LoadLE64(in + 40);
    return out->prerequisitePackStableId != 0 &&
           (out->edgeKind == kPackDependencyEdgeKindInit ||
            out->edgeKind == kPackDependencyEdgeKindShared ||
            out->edgeKind == kPackDependencyEdgeKindContent) &&
           out->required01 <= 1;
}

inline void SerializePackBusDefinitionSnapshotHeaderV1(
    const PackBusDefinitionSnapshotHeaderV1& header, std::uint8_t* out) {
    StoreLE32(out + 0, header.schemaVersion);
    StoreLE32(out + 4, header.storageModelVersion);
    StoreLE32(out + 8, header.headerBytes);
    StoreLE32(out + 12, header.publicationCapabilityVintage);
    StoreLE64(out + 16, header.contentGeneration);
    StoreLE64(out + 24, header.publishedBusFingerprint);
    StoreLE32(out + 32, header.busCount);
    StoreLE32(out + 36, header.sendCount);
    StoreLE32(out + 40, header.effectCount);
    StoreLE32(out + 44, header.effectParameterCount);
    StoreLE32(out + 48, header.downmixCount);
    StoreLE32(out + 52, header.coefficientCount);
    StoreLE64(out + 56, header.busOffset);
    StoreLE64(out + 64, header.sendOffset);
    StoreLE64(out + 72, header.effectOffset);
    StoreLE64(out + 80, header.effectParameterOffset);
    StoreLE64(out + 88, header.downmixOffset);
    StoreLE64(out + 96, header.coefficientOffset);
    StoreLE64(out + 104, header.payloadBytes);
    StoreLE64(out + 112, header.checksum);
    StoreLE64(out + 120, header.compileTargetProfileFingerprint);
    StoreLE32(out + 128, header.profileVintage);
    StoreLE32(out + 132, header.renderSampleRateHz);
    StoreLE32(out + 136, header.mixLayout);
    StoreLE32(out + 140, header.deviceLayout);
    StoreLE64(out + 144, header.runtimeCapabilityMask);
    StoreLE64(out + 152, header.reserved0);
}

inline void SerializePackBusDefinitionSnapshotHeaderV2(
    const PackBusDefinitionSnapshotHeaderV1& header, std::uint8_t* out) {
    SerializePackBusDefinitionSnapshotHeaderV1(header, out);
    StoreLE32(out + 160, header.busRowBytes);
    StoreLE32(out + 164, header.sendRowBytes);
    StoreLE32(out + 168, header.downmixRowBytes);
    StoreLE32(out + 172, header.coefficientRowBytes);
    StoreLE64(out + 176, header.reserved1);
}

inline bool ParsePackBusDefinitionSnapshotHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackBusDefinitionSnapshotHeaderV1* out, std::uint32_t vintage) {
    if (size < (vintage == 2 ? kPackBusDefinitionSnapshotV2HeaderBytes
                             : kPackBusDefinitionSnapshotHeaderBytes)) {
        return false;
    }
    out->schemaVersion = LoadLE32(in + 0);
    out->storageModelVersion = LoadLE32(in + 4);
    out->headerBytes = LoadLE32(in + 8);
    out->publicationCapabilityVintage = LoadLE32(in + 12);
    out->contentGeneration = LoadLE64(in + 16);
    out->publishedBusFingerprint = LoadLE64(in + 24);
    out->busCount = LoadLE32(in + 32);
    out->sendCount = LoadLE32(in + 36);
    out->effectCount = LoadLE32(in + 40);
    out->effectParameterCount = LoadLE32(in + 44);
    out->downmixCount = LoadLE32(in + 48);
    out->coefficientCount = LoadLE32(in + 52);
    out->busOffset = LoadLE64(in + 56);
    out->sendOffset = LoadLE64(in + 64);
    out->effectOffset = LoadLE64(in + 72);
    out->effectParameterOffset = LoadLE64(in + 80);
    out->downmixOffset = LoadLE64(in + 88);
    out->coefficientOffset = LoadLE64(in + 96);
    out->payloadBytes = LoadLE64(in + 104);
    out->checksum = LoadLE64(in + 112);
    out->compileTargetProfileFingerprint = LoadLE64(in + 120);
    out->profileVintage = LoadLE32(in + 128);
    out->renderSampleRateHz = LoadLE32(in + 132);
    out->mixLayout = LoadLE32(in + 136);
    out->deviceLayout = LoadLE32(in + 140);
    out->runtimeCapabilityMask = LoadLE64(in + 144);
    out->reserved0 = LoadLE64(in + 152);
    if (vintage == 2) {
        out->busRowBytes = LoadLE32(in + 160);
        out->sendRowBytes = LoadLE32(in + 164);
        out->downmixRowBytes = LoadLE32(in + 168);
        out->coefficientRowBytes = LoadLE32(in + 172);
        out->reserved1 = LoadLE64(in + 176);
    }
    std::uint64_t expectedSize = 0;
    if (out->schemaVersion != vintage || out->storageModelVersion != vintage ||
        out->headerBytes != (vintage == 2 ? kPackBusDefinitionSnapshotV2HeaderBytes
                                          : kPackBusDefinitionSnapshotHeaderBytes) ||
        (vintage == 2 ? (out->publicationCapabilityVintage != 3u &&
                         out->publicationCapabilityVintage != 4u)
                      : out->publicationCapabilityVintage != 1u) ||
        out->profileVintage != 1 ||
        out->reserved0 != 0 ||
        (vintage == 2 &&
         (out->busRowBytes != kPackBusDefinitionBusV2RowBytes ||
          out->sendRowBytes != kPackBusDefinitionSendV2RowBytes ||
          out->downmixRowBytes != kPackBusDefinitionDownmixV2RowBytes ||
          out->coefficientRowBytes != kPackBusDefinitionCoefficientRowBytes ||
          out->reserved1 != 0)) ||
        !CheckedAddU64(out->headerBytes, out->payloadBytes, &expectedSize) ||
        size != expectedSize) {
        return false;
    }
    std::uint8_t checksumZero[8] = {};
    std::uint64_t hash = Fnv1a64(in, 112);
    hash = Fnv1a64(checksumZero, 8, hash);
    hash = Fnv1a64(in + 120, static_cast<std::size_t>(size - 120), hash);
    return hash == out->checksum;
}

inline bool ParsePackBusDefinitionSnapshotHeaderV1(
    const std::uint8_t* in, std::uint64_t size,
    PackBusDefinitionSnapshotHeaderV1* out) {
    return ParsePackBusDefinitionSnapshotHeader(in, size, out, 1);
}

inline bool ParsePackBusDefinitionSnapshotHeaderV2(
    const std::uint8_t* in, std::uint64_t size,
    PackBusDefinitionSnapshotHeaderV1* out) {
    return ParsePackBusDefinitionSnapshotHeader(in, size, out, 2);
}

inline void SerializePackCanonicalPartitionHeader(
    const PackCanonicalPartitionHeader& header, std::uint8_t* out) {
    std::memcpy(out, kCanonicalDefinitionPartitionMagic, 8);
    StoreLE32(out + 8, header.headerBytes);
    StoreLE32(out + 12, header.bodyVintage);
    StoreLE64(out + 16, header.bodyBytes);
    StoreLE64(out + 24, header.bodyChecksum);
    StoreLE64(out + 32, header.packStableId);
    StoreLE64(out + 40, header.partitionRevision);
    StoreLE64(out + 48, header.ownedDefinitionCount);
    StoreLE64(out + 56, header.headerChecksum);
}

inline bool ParsePackCanonicalPartitionHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackCanonicalPartitionHeader* out) {
    if (size < kCanonicalDefinitionPartitionHeaderBytes) {
        return false;
    }
    if (std::memcmp(in, kCanonicalDefinitionPartitionMagic, 8) != 0) {
        return false;
    }
    out->headerBytes = LoadLE32(in + 8);
    out->bodyVintage = LoadLE32(in + 12);
    out->bodyBytes = LoadLE64(in + 16);
    out->bodyChecksum = LoadLE64(in + 24);
    out->packStableId = LoadLE64(in + 32);
    out->partitionRevision = LoadLE64(in + 40);
    out->ownedDefinitionCount = LoadLE64(in + 48);
    out->headerChecksum = LoadLE64(in + 56);
    // bodyVintage is an internal layout id. This parser only proves the
    // envelope; the image layer maps a non-current value to
    // VN_PACK_INCOMPATIBLE instead of folding it into VN_BAD_PACK.
    if (out->headerBytes != kCanonicalDefinitionPartitionHeaderBytes ||
        out->packStableId == 0 ||
        size != static_cast<std::uint64_t>(out->headerBytes) + out->bodyBytes) {
        return false;
    }
    return Fnv1a64(in, 56) == out->headerChecksum &&
           Fnv1a64(in + out->headerBytes,
                   static_cast<std::size_t>(out->bodyBytes)) ==
               out->bodyChecksum;
}

inline void SerializePackCanonicalPartitionHeaderV2(
    const PackCanonicalPartitionHeader& header, std::uint8_t* out) {
    std::memcpy(out, kCanonicalDefinitionPartitionMagicV2, 8);
    StoreLE32(out + 8, header.headerBytes);
    StoreLE32(out + 12, header.bodyVintage);
    StoreLE64(out + 16, header.bodyBytes);
    StoreLE64(out + 24, header.bodyChecksum);
    StoreLE64(out + 32, header.packStableId);
    StoreLE64(out + 40, header.partitionRevision);
    StoreLE64(out + 48, header.ownedDefinitionCount);
    StoreLE64(out + 56, header.busContentGeneration);
    StoreLE64(out + 64, header.publishedBusFingerprint);
    StoreLE64(out + 72, header.compileTargetProfileFingerprint);
    StoreLE64(out + 80, header.headerChecksum);
}

inline bool ParsePackCanonicalPartitionHeaderV2(
    const std::uint8_t* in, std::uint64_t size,
    PackCanonicalPartitionHeader* out) {
    if (size < kCanonicalDefinitionPartitionHeaderV2Bytes) {
        return false;
    }
    if (std::memcmp(in, kCanonicalDefinitionPartitionMagicV2, 8) != 0) {
        return false;
    }
    out->headerBytes = LoadLE32(in + 8);
    out->bodyVintage = LoadLE32(in + 12);
    out->bodyBytes = LoadLE64(in + 16);
    out->bodyChecksum = LoadLE64(in + 24);
    out->packStableId = LoadLE64(in + 32);
    out->partitionRevision = LoadLE64(in + 40);
    out->ownedDefinitionCount = LoadLE64(in + 48);
    out->busContentGeneration = LoadLE64(in + 56);
    out->publishedBusFingerprint = LoadLE64(in + 64);
    out->compileTargetProfileFingerprint = LoadLE64(in + 72);
    out->headerChecksum = LoadLE64(in + 80);
    std::uint64_t expectedSize = 0;
    if (out->headerBytes != kCanonicalDefinitionPartitionHeaderV2Bytes ||
        out->packStableId == 0 ||
        !CheckedAddU64(out->headerBytes, out->bodyBytes, &expectedSize) ||
        size != expectedSize) {
        return false;
    }
    return Fnv1a64(in, 80) == out->headerChecksum &&
           Fnv1a64(in + out->headerBytes,
                   static_cast<std::size_t>(out->bodyBytes)) ==
               out->bodyChecksum;
}

inline void SerializePackBusDefinitionBusV1(const PackBusDefinitionBusV1& row,
                                            std::uint8_t* out) {
    StoreLE64(out + 0, row.busId);
    StoreLE64(out + 8, row.parentId);
    StoreLE32(out + 16, row.feedsFirstOrdinal);
    StoreLE32(out + 20, row.flags);
    StoreLEF32(out + 24, row.faderDb);
    StoreLE32(out + 28, row.faderAfterSlot);
    StoreLE32(out + 32, row.outputLayout);
    StoreLE32(out + 36, row.effectFirst);
    StoreLE32(out + 40, row.effectCount);
    StoreLE32(out + 44, row.downmixFirst);
    StoreLE32(out + 48, row.downmixCount);
    StoreLE32(out + 52, row.reserved0);
    StoreLE64(out + 56, row.reserved1);
}

inline bool ParsePackBusDefinitionBusV1(const std::uint8_t* in,
                                        PackBusDefinitionBusV1* out) {
    out->busId = LoadLE64(in + 0);
    out->parentId = LoadLE64(in + 8);
    out->feedsFirstOrdinal = LoadLE32(in + 16);
    out->flags = LoadLE32(in + 20);
    out->faderDb = LoadLEF32(in + 24);
    out->faderAfterSlot = LoadLE32(in + 28);
    out->outputLayout = LoadLE32(in + 32);
    out->effectFirst = LoadLE32(in + 36);
    out->effectCount = LoadLE32(in + 40);
    out->downmixFirst = LoadLE32(in + 44);
    out->downmixCount = LoadLE32(in + 48);
    out->reserved0 = LoadLE32(in + 52);
    out->reserved1 = LoadLE64(in + 56);
    return out->flags == 0 && out->reserved0 == 0 && out->reserved1 == 0;
}

inline void SerializePackBusDefinitionSendV1(const PackBusDefinitionSendV1& row,
                                             std::uint8_t* out) {
    StoreLE64(out + 0, row.source);
    StoreLE64(out + 8, row.target);
    StoreLEF32(out + 16, row.levelDb);
    StoreLE32(out + 20, row.reserved0);
    StoreLE64(out + 24, row.reserved1);
}

inline bool ParsePackBusDefinitionSendV1(const std::uint8_t* in,
                                         PackBusDefinitionSendV1* out) {
    out->source = LoadLE64(in + 0);
    out->target = LoadLE64(in + 8);
    out->levelDb = LoadLEF32(in + 16);
    out->reserved0 = LoadLE32(in + 20);
    out->reserved1 = LoadLE64(in + 24);
    return out->reserved0 == 0 && out->reserved1 == 0;
}

inline void SerializePackBusDefinitionEffectV1(
    const PackBusDefinitionEffectV1& row, std::uint8_t* out) {
    StoreLE64(out + 0, row.busId);
    StoreLE64(out + 8, row.effectStableId);
    StoreLE32(out + 16, row.slot);
    StoreLE32(out + 20, row.effectType);
    StoreLE32(out + 24, row.descriptorVintage);
    StoreLE32(out + 28, row.flags);
    StoreLE32(out + 32, row.parameterFirst);
    StoreLE32(out + 36, row.parameterCount);
    StoreLE64(out + 40, row.reserved0);
}

inline bool ParsePackBusDefinitionEffectV1(const std::uint8_t* in,
                                           PackBusDefinitionEffectV1* out) {
    out->busId = LoadLE64(in + 0);
    out->effectStableId = LoadLE64(in + 8);
    out->slot = LoadLE32(in + 16);
    out->effectType = LoadLE32(in + 20);
    out->descriptorVintage = LoadLE32(in + 24);
    out->flags = LoadLE32(in + 28);
    out->parameterFirst = LoadLE32(in + 32);
    out->parameterCount = LoadLE32(in + 36);
    out->reserved0 = LoadLE64(in + 40);
    return (out->flags & ~1u) == 0 && out->reserved0 == 0;
}

inline void SerializePackBusDefinitionEffectParameterV1(
    const PackBusDefinitionEffectParameterV1& row, std::uint8_t* out) {
    StoreLE32(out + 0, row.parameterIndex);
    StoreLEF32(out + 4, row.value);
    StoreLE32(out + 8, row.reserved0);
    StoreLE32(out + 12, row.reserved1);
}

inline bool ParsePackBusDefinitionEffectParameterV1(
    const std::uint8_t* in, PackBusDefinitionEffectParameterV1* out) {
    out->parameterIndex = LoadLE32(in + 0);
    out->value = LoadLEF32(in + 4);
    out->reserved0 = LoadLE32(in + 8);
    out->reserved1 = LoadLE32(in + 12);
    return out->reserved0 == 0 && out->reserved1 == 0;
}

inline void SerializePackBusDefinitionDownmixV1(
    const PackBusDefinitionDownmixV1& row, std::uint8_t* out) {
    StoreLE64(out + 0, row.busId);
    StoreLE32(out + 8, row.sourceLayout);
    StoreLE32(out + 12, row.destinationLayout);
    StoreLE32(out + 16, row.coefficientFirst);
    StoreLE32(out + 20, row.coefficientCount);
    StoreLE64(out + 24, row.reserved0);
}

inline bool ParsePackBusDefinitionDownmixV1(const std::uint8_t* in,
                                            PackBusDefinitionDownmixV1* out) {
    out->busId = LoadLE64(in + 0);
    out->sourceLayout = LoadLE32(in + 8);
    out->destinationLayout = LoadLE32(in + 12);
    out->coefficientFirst = LoadLE32(in + 16);
    out->coefficientCount = LoadLE32(in + 20);
    out->reserved0 = LoadLE64(in + 24);
    return out->reserved0 == 0;
}

inline void SerializePackBusDefinitionBusV2(const PackBusDefinitionBusV2& row,
                                             std::uint8_t* out) {
    StoreObjectId(out + 0, row.busId);
    StoreObjectId(out + 16, row.parentId);
    StoreLE32(out + 32, row.feedsFirstOrdinal);
    StoreLEF32(out + 36, row.faderDb);
    StoreLEF32(out + 40, row.makeUpGainDb);
    StoreLEF32(out + 44, row.outputBusVolumeDb);
    StoreLE32(out + 48, row.faderAfterSlot);
    StoreLE32(out + 52, row.outputLayout);
    StoreLE32(out + 56, row.effectFirst);
    StoreLE32(out + 60, row.effectCount);
    StoreLE32(out + 64, row.downmixFirst);
    StoreLE32(out + 68, row.downmixCount);
    StoreLE64(out + 72, 0);
}

inline bool ParsePackBusDefinitionBusV2(const std::uint8_t* in,
                                         PackBusDefinitionBusV2* out) {
    out->busId = LoadObjectId(in + 0);
    out->parentId = LoadObjectId(in + 16);
    out->feedsFirstOrdinal = LoadLE32(in + 32);
    out->faderDb = LoadLEF32(in + 36);
    out->makeUpGainDb = LoadLEF32(in + 40);
    out->outputBusVolumeDb = LoadLEF32(in + 44);
    out->faderAfterSlot = LoadLE32(in + 48);
    out->outputLayout = LoadLE32(in + 52);
    out->effectFirst = LoadLE32(in + 56);
    out->effectCount = LoadLE32(in + 60);
    out->downmixFirst = LoadLE32(in + 64);
    out->downmixCount = LoadLE32(in + 68);
    return LoadLE64(in + 72) == 0;
}

inline void SerializePackBusDefinitionSendV2(const PackBusDefinitionSendV2& row,
                                              std::uint8_t* out) {
    StoreObjectId(out + 0, row.source);
    StoreObjectId(out + 16, row.target);
    StoreLEF32(out + 32, row.levelDb);
    StoreLE32(out + 36, 0);
    StoreLE64(out + 40, 0);
}

inline bool ParsePackBusDefinitionSendV2(const std::uint8_t* in,
                                          PackBusDefinitionSendV2* out) {
    out->source = LoadObjectId(in + 0);
    out->target = LoadObjectId(in + 16);
    out->levelDb = LoadLEF32(in + 32);
    return LoadLE32(in + 36) == 0 && LoadLE64(in + 40) == 0;
}

inline void SerializePackBusDefinitionDownmixV2(
    const PackBusDefinitionDownmixV2& row, std::uint8_t* out) {
    StoreObjectId(out + 0, row.busId);
    StoreLE32(out + 16, row.sourceLayout);
    StoreLE32(out + 20, row.destinationLayout);
    StoreLE32(out + 24, row.coefficientFirst);
    StoreLE32(out + 28, row.coefficientCount);
    StoreLE64(out + 32, 0);
}

inline bool ParsePackBusDefinitionDownmixV2(const std::uint8_t* in,
                                             PackBusDefinitionDownmixV2* out) {
    out->busId = LoadObjectId(in + 0);
    out->sourceLayout = LoadLE32(in + 16);
    out->destinationLayout = LoadLE32(in + 20);
    out->coefficientFirst = LoadLE32(in + 24);
    out->coefficientCount = LoadLE32(in + 28);
    return LoadLE64(in + 32) == 0;
}

struct PackGameplayCatalogHeader {
    std::uint32_t schemaVersion = 0;
    std::uint32_t layoutVintage = 0;
    std::array<std::uint8_t, 16> projectGuid{};
    std::uint64_t digest = 0;
    std::uint32_t activeCount = 0;
    std::uint32_t retiredCount = 0;
    std::uint32_t namePoolBytes = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t requiredCapabilityMask = 0;
    std::uint64_t publicationPairingFingerprint = 0;
    std::uint64_t initPackStableId = 0;
    std::uint64_t initContentRevision = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved1 = 0;
    std::uint64_t headerChecksum = 0;
};

struct PackGameplayCatalogActiveRow {
    VnObjectId actionStableId{};
    std::uint64_t nameBindingRevision = 0;
    std::uint64_t callableFingerprint = 0;
    std::uint64_t ownerPackStableId = 0;
    std::uint64_t providerClosureFingerprint = 0;
    std::uint32_t nameOffset = 0;
    std::uint16_t nameLength = 0;
    std::uint16_t flags = 0;
    std::uint64_t reserved0 = 0;
    std::uint64_t rowChecksum = 0;
};

struct PackGameplayCatalogRetiredRow {
    VnObjectId lastActionStableId{};
    std::uint64_t lastNameBindingRevision = 0;
    std::uint32_t reason = 0;
    std::uint16_t nameLength = 0;
    std::uint16_t reserved0 = 0;
    std::uint32_t nameOffset = 0;
    std::uint32_t reserved1 = 0;
};

struct PackGameplayCatalogActiveDraft {
    VnObjectId actionStableId{};
    std::uint64_t nameBindingRevision = 0;
    std::uint64_t callableFingerprint = 0;
    std::uint64_t ownerPackStableId = 0;
    std::uint64_t providerClosureFingerprint = 0;
    std::uint16_t flags = 0;
    std::string_view name;
};

struct PackGameplayCatalogRetiredDraft {
    VnObjectId lastActionStableId{};
    std::uint64_t lastNameBindingRevision = 0;
    std::uint32_t reason = 0;
    std::string_view name;
};

inline std::uint64_t GameplayCallableFingerprint(
    VnObjectId actionId, std::uint64_t nameRevision, std::string_view name) {
    std::array<std::uint8_t, 26> prefix{};
    StoreObjectId(prefix.data(), actionId);
    StoreLE64(prefix.data() + 16, nameRevision);
    StoreLE16(prefix.data() + 24, static_cast<std::uint16_t>(name.size()));
    auto hash = Fnv1a64(prefix.data(), prefix.size());
    hash = Fnv1a64(name.data(), name.size(), hash);
    std::array<std::uint8_t, 4> suffix{};
    StoreLE32(suffix.data(), kInitCatalogIdentityClassAction);
    return Fnv1a64(suffix.data(), suffix.size(), hash);
}

inline int CompareGameplayCatalogNameBytes(const void* a, std::uint16_t aLen,
                                           const void* b, std::uint16_t bLen) {
    const std::size_t n = aLen < bLen ? aLen : bLen;
    const int c = std::memcmp(a, b, n);
    if (c != 0) {
        return c;
    }
    return aLen < bLen ? -1 : (aLen > bLen ? 1 : 0);
}

inline std::uint64_t GameplayCatalogDigest(const std::uint8_t* payload,
                                           std::size_t size) {
    if (payload == nullptr || size < kGameplayCatalogHeaderBytes) {
        return 0;
    }
    const std::uint8_t zeros[8] = {};
    std::uint64_t hash = Fnv1a64(payload, 24);
    hash = Fnv1a64(zeros, 8, hash);
    hash = Fnv1a64(payload + 32, 24, hash);
    hash = Fnv1a64(zeros, 8, hash);
    hash = Fnv1a64(payload + 64, 24, hash);
    hash = Fnv1a64(zeros, 8, hash);
    if (size > kGameplayCatalogHeaderBytes) {
        hash = Fnv1a64(payload + kGameplayCatalogHeaderBytes,
                       size - kGameplayCatalogHeaderBytes, hash);
    }
    return hash;
}

inline std::uint64_t GameplayCatalogPublicationPairing(
    const PackGameplayCatalogHeader& header) {
    std::uint8_t bytes[40] = {};
    std::memcpy(bytes, header.projectGuid.data(), 16);
    StoreLE64(bytes + 16, header.initPackStableId);
    StoreLE64(bytes + 24, header.initContentRevision);
    StoreLE64(bytes + 32, header.digest);
    return Fnv1a64(bytes, sizeof bytes);
}

inline void SerializePackGameplayCatalogHeader(
    const PackGameplayCatalogHeader& header, std::uint8_t* out) {
    StoreLE32(out + 0, header.schemaVersion);
    StoreLE32(out + 4, header.layoutVintage);
    std::memcpy(out + 8, header.projectGuid.data(), 16);
    StoreLE64(out + 24, header.digest);
    StoreLE32(out + 32, header.activeCount);
    StoreLE32(out + 36, header.retiredCount);
    StoreLE32(out + 40, header.namePoolBytes);
    StoreLE32(out + 44, header.reserved0);
    StoreLE64(out + 48, header.requiredCapabilityMask);
    StoreLE64(out + 56, header.publicationPairingFingerprint);
    StoreLE64(out + 64, header.initPackStableId);
    StoreLE64(out + 72, header.initContentRevision);
    StoreLE32(out + 80, header.flags);
    StoreLE32(out + 84, header.reserved1);
    StoreLE64(out + 88, header.headerChecksum);
}

inline void LoadPackGameplayCatalogHeader(const std::uint8_t* in,
                                          PackGameplayCatalogHeader* out) {
    out->schemaVersion = LoadLE32(in + 0);
    out->layoutVintage = LoadLE32(in + 4);
    std::memcpy(out->projectGuid.data(), in + 8, 16);
    out->digest = LoadLE64(in + 24);
    out->activeCount = LoadLE32(in + 32);
    out->retiredCount = LoadLE32(in + 36);
    out->namePoolBytes = LoadLE32(in + 40);
    out->reserved0 = LoadLE32(in + 44);
    out->requiredCapabilityMask = LoadLE64(in + 48);
    out->publicationPairingFingerprint = LoadLE64(in + 56);
    out->initPackStableId = LoadLE64(in + 64);
    out->initContentRevision = LoadLE64(in + 72);
    out->flags = LoadLE32(in + 80);
    out->reserved1 = LoadLE32(in + 84);
    out->headerChecksum = LoadLE64(in + 88);
}

inline void SerializePackGameplayCatalogActiveRow(
    const PackGameplayCatalogActiveRow& row, std::uint8_t* out) {
    StoreObjectId(out + 0, row.actionStableId);
    StoreLE64(out + 16, row.nameBindingRevision);
    StoreLE64(out + 24, row.callableFingerprint);
    StoreLE64(out + 32, row.ownerPackStableId);
    StoreLE64(out + 40, row.providerClosureFingerprint);
    StoreLE32(out + 48, row.nameOffset);
    StoreLE16(out + 52, row.nameLength);
    StoreLE16(out + 54, row.flags);
    StoreLE64(out + 56, row.reserved0);
    StoreLE64(out + 64, row.rowChecksum);
}

inline bool ParsePackGameplayCatalogActiveRow(
    const std::uint8_t* in, PackGameplayCatalogActiveRow* out) {
    out->actionStableId = LoadObjectId(in + 0);
    out->nameBindingRevision = LoadLE64(in + 16);
    out->callableFingerprint = LoadLE64(in + 24);
    out->ownerPackStableId = LoadLE64(in + 32);
    out->providerClosureFingerprint = LoadLE64(in + 40);
    out->nameOffset = LoadLE32(in + 48);
    out->nameLength = LoadLE16(in + 52);
    out->flags = LoadLE16(in + 54);
    out->reserved0 = LoadLE64(in + 56);
    out->rowChecksum = LoadLE64(in + 64);
    if (out->actionStableId == VnObjectId{} || out->ownerPackStableId == 0 ||
        out->nameBindingRevision < 1 || out->nameLength < 1 ||
        out->nameLength > kGameplayCatalogMaxNameBytes ||
        (out->flags & ~kGameplayCatalogActiveFlagOptional) != 0 ||
        out->reserved0 != 0) {
        return false;
    }
    return Fnv1a64(in, kGameplayCatalogActiveRowBytes - 8) == out->rowChecksum;
}

inline void SerializePackGameplayCatalogRetiredRow(
    const PackGameplayCatalogRetiredRow& row, std::uint8_t* out) {
    StoreObjectId(out + 0, row.lastActionStableId);
    StoreLE64(out + 16, row.lastNameBindingRevision);
    StoreLE32(out + 24, row.reason);
    StoreLE16(out + 28, row.nameLength);
    StoreLE16(out + 30, row.reserved0);
    StoreLE32(out + 32, row.nameOffset);
    StoreLE32(out + 36, row.reserved1);
}

inline bool ParsePackGameplayCatalogRetiredRow(
    const std::uint8_t* in, PackGameplayCatalogRetiredRow* out) {
    out->lastActionStableId = LoadObjectId(in + 0);
    out->lastNameBindingRevision = LoadLE64(in + 16);
    out->reason = LoadLE32(in + 24);
    out->nameLength = LoadLE16(in + 28);
    out->reserved0 = LoadLE16(in + 30);
    out->nameOffset = LoadLE32(in + 32);
    out->reserved1 = LoadLE32(in + 36);
    return out->lastActionStableId != VnObjectId{} && out->nameLength >= 1 &&
           out->nameLength <= kGameplayCatalogMaxNameBytes &&
           (out->reason == kGameplayCatalogRetiredReasonRenamed ||
            out->reason == kGameplayCatalogRetiredReasonDeleted ||
            out->reason == kGameplayCatalogRetiredReasonReused) &&
           out->reserved0 == 0 && out->reserved1 == 0;
}

inline bool EncodeGameplayCatalog(
    const PackGameplayCatalogHeader& identity,
    std::vector<PackGameplayCatalogActiveDraft> active,
    std::vector<PackGameplayCatalogRetiredDraft> retired,
    std::vector<std::uint8_t>* out) {
    if (out == nullptr ||
        active.size() > kGameplayCatalogMaxEntries ||
        retired.size() > kGameplayCatalogMaxEntries) {
        return false;
    }
    for (const auto& row : active) {
        if (row.name.empty() || row.name.size() > kGameplayCatalogMaxNameBytes ||
            row.actionStableId == VnObjectId{} || row.ownerPackStableId == 0 ||
            row.nameBindingRevision < 1 ||
            (row.flags & ~kGameplayCatalogActiveFlagOptional) != 0) {
            return false;
        }
    }
    for (const auto& row : retired) {
        if (row.name.empty() || row.name.size() > kGameplayCatalogMaxNameBytes ||
            row.lastActionStableId == VnObjectId{} ||
            (row.reason != kGameplayCatalogRetiredReasonRenamed &&
             row.reason != kGameplayCatalogRetiredReasonDeleted &&
             row.reason != kGameplayCatalogRetiredReasonReused)) {
            return false;
        }
    }
    std::sort(active.begin(), active.end(),
              [](const PackGameplayCatalogActiveDraft& a,
                 const PackGameplayCatalogActiveDraft& b) {
                  const int c = CompareGameplayCatalogNameBytes(
                      a.name.data(),
                      static_cast<std::uint16_t>(a.name.size()),
                      b.name.data(),
                      static_cast<std::uint16_t>(b.name.size()));
                  if (c != 0) {
                      return c < 0;
                  }
                  return a.actionStableId < b.actionStableId;
              });
    std::sort(retired.begin(), retired.end(),
              [](const PackGameplayCatalogRetiredDraft& a,
                 const PackGameplayCatalogRetiredDraft& b) {
                  const int c = CompareGameplayCatalogNameBytes(
                      a.name.data(),
                      static_cast<std::uint16_t>(a.name.size()),
                      b.name.data(),
                      static_cast<std::uint16_t>(b.name.size()));
                  if (c != 0) {
                      return c < 0;
                  }
                  return a.lastActionStableId < b.lastActionStableId;
              });

    std::uint32_t rawNameBytes = 0;
    for (const auto& row : active) {
        rawNameBytes += static_cast<std::uint32_t>(row.name.size());
    }
    for (const auto& row : retired) {
        rawNameBytes += static_cast<std::uint32_t>(row.name.size());
    }
    const std::uint32_t namePoolBytes = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(rawNameBytes) + 7u) & ~7ull);
    std::uint64_t payloadBytes = 0;
    if (!GameplayCatalogBytes(static_cast<std::uint32_t>(active.size()),
                              static_cast<std::uint32_t>(retired.size()),
                              namePoolBytes, &payloadBytes) ||
        payloadBytes > kGameplayCatalogMaxBytes) {
        return false;
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payloadBytes), 0);
    PackGameplayCatalogHeader header = identity;
    header.schemaVersion = 1;
    header.layoutVintage = kPackGameplayCatalogVintage;
    header.digest = 0;
    header.activeCount = static_cast<std::uint32_t>(active.size());
    header.retiredCount = static_cast<std::uint32_t>(retired.size());
    header.namePoolBytes = namePoolBytes;
    header.reserved0 = 0;
    header.publicationPairingFingerprint = 0;
    header.flags =
        header.retiredCount > 0 ? kGameplayCatalogFlagHasRetired : 0;
    header.reserved1 = 0;
    header.headerChecksum = 0;
    SerializePackGameplayCatalogHeader(header, payload.data());

    std::uint8_t* pool =
        payload.data() + kGameplayCatalogHeaderBytes +
        static_cast<std::size_t>(header.activeCount) *
            kGameplayCatalogActiveRowBytes +
        static_cast<std::size_t>(header.retiredCount) *
            kGameplayCatalogRetiredRowBytes;
    std::uint32_t nameOffset = 0;
    for (std::uint32_t i = 0; i < header.activeCount; ++i) {
        PackGameplayCatalogActiveRow row;
        row.actionStableId = active[i].actionStableId;
        row.nameBindingRevision = active[i].nameBindingRevision;
        row.callableFingerprint = active[i].callableFingerprint;
        row.ownerPackStableId = active[i].ownerPackStableId;
        row.providerClosureFingerprint = active[i].providerClosureFingerprint;
        row.nameOffset = nameOffset;
        row.nameLength = static_cast<std::uint16_t>(active[i].name.size());
        row.flags = active[i].flags;
        std::uint8_t* rowBytes =
            payload.data() + kGameplayCatalogHeaderBytes +
            static_cast<std::size_t>(i) * kGameplayCatalogActiveRowBytes;
        SerializePackGameplayCatalogActiveRow(row, rowBytes);
        row.rowChecksum = Fnv1a64(rowBytes, kGameplayCatalogActiveRowBytes - 8);
        StoreLE64(rowBytes + kGameplayCatalogActiveRowBytes - 8, row.rowChecksum);
        std::memcpy(pool + nameOffset, active[i].name.data(), row.nameLength);
        nameOffset += row.nameLength;
    }
    for (std::uint32_t i = 0; i < header.retiredCount; ++i) {
        PackGameplayCatalogRetiredRow row;
        row.lastActionStableId = retired[i].lastActionStableId;
        row.lastNameBindingRevision = retired[i].lastNameBindingRevision;
        row.reason = retired[i].reason;
        row.nameLength = static_cast<std::uint16_t>(retired[i].name.size());
        row.nameOffset = nameOffset;
        std::uint8_t* rowBytes =
            payload.data() + kGameplayCatalogHeaderBytes +
            static_cast<std::size_t>(header.activeCount) *
                kGameplayCatalogActiveRowBytes +
            static_cast<std::size_t>(i) * kGameplayCatalogRetiredRowBytes;
        SerializePackGameplayCatalogRetiredRow(row, rowBytes);
        std::memcpy(pool + nameOffset, retired[i].name.data(), row.nameLength);
        nameOffset += row.nameLength;
    }

    header.digest = GameplayCatalogDigest(payload.data(), payload.size());
    StoreLE64(payload.data() + 24, header.digest);
    header.publicationPairingFingerprint =
        GameplayCatalogPublicationPairing(header);
    StoreLE64(payload.data() + 56, header.publicationPairingFingerprint);
    header.headerChecksum = Fnv1a64(payload.data(), 88);
    StoreLE64(payload.data() + 88, header.headerChecksum);
    *out = std::move(payload);
    return true;
}

} // namespace harken

#endif // HARKEN_MEDIA_PACK_FORMAT_V2_H
