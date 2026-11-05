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
#include "harken/media/generated/harken_pack_wire_constants.g.h"

#include <harken/core/CurrentWorld.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace harken {


// Header: magic(4) + formatVersion(4) + headerBytes(4) + fileBytes(8) +
// packKind(4) + entryCount(4) + directoryOffset(8) + directoryChecksum(8).

// Directory record: kind(4) + vintage(4) + id(8) + offset(8) + size(8) +
// payloadChecksum(8).

// Pack kinds (header field). Resident packs carry logic-only
// infrastructure (zero media entries, enforced by compiler and loader);
// content packs carry media plus reclaimable logic.
inline constexpr std::uint32_t kPackKindResident = 0;
inline constexpr std::uint32_t kPackKindContent = 1;
inline constexpr std::uint32_t kPackKindInit = 2;
inline constexpr std::uint32_t kPackKindCanonicalContent = 3;

// Payload alignment inside the payload region. Media blobs keep the v1
// 64-byte rule (kPackBlobAlign).

// Fixed payload sizes. Variable-size kinds have sizing helpers below.
// Current Attenuation write is kPackAttenuationBytes; current
// VoiceRecipe write is PackVoiceRecipePayloadBytes.
inline constexpr std::uint32_t kPackVoiceRecipeMaxInstalls = 8;
inline constexpr std::uint32_t kPackVoiceRecipeMaxBindings = 8;
inline constexpr std::uint32_t kPackEffectDescriptorVintage = 2;
inline constexpr std::uint32_t kPackVoiceToneLowpassEnabled = 1u << 0;
inline constexpr std::uint32_t kPackVoiceToneHighpassEnabled = 1u << 1;
inline constexpr std::uint32_t kPackVoiceToneLowShelfEnabled = 1u << 2;
inline constexpr std::uint32_t kPackVoiceToneHighShelfEnabled = 1u << 3;
inline constexpr std::uint32_t kPackVoiceToneEnableMask =
    kPackVoiceToneLowpassEnabled | kPackVoiceToneHighpassEnabled |
    kPackVoiceToneLowShelfEnabled | kPackVoiceToneHighShelfEnabled;


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
// Plain working copies, like the v1 PackMediaFileHeader/PackTocEntry pair; only
// the Serialize/Parse helpers define bytes. Where a kind's registration
// currency is a public descriptor struct, the working form embeds that
// struct directly (structSize filled by the parser) so readers hand it
// to the registration seam without a second translation. Pointer-bearing
// descriptor fields such as container children stay OUT of the working
// forms; the consumer supplies storage and wires the pointer.


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


struct PackBindingDefinitionEntry {
    VnBindingId bindingId{};
    VnBindingDesc definition{};
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
    std::array<float, kPackVoiceRecipeMaxParams> params{};
    VnObjectId effectId{};
};

struct PackVoiceRecipeInstallWire : PackVoiceRecipeInstall {
    std::uint32_t paramBytes = 0;
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
    std::array<float, kPackVoiceRecipeMaxParams> params{};
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

struct PackContainerChild {
    VnObjectId playableId{};
    std::uint32_t weight = 0;
    std::uint32_t matchValue = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t delayJitterMs = 0;
};

// --- header ---


// Parses in[0..kPackFileHeaderBytes). Returns false when the magic does
// not match; field sanity beyond the magic is the caller's validation
// (the v1 split of duties).


// --- directory ---


// --- fixed-size logic payloads ---


// Attenuation override curve, 184 bytes: enabled(4) + pointCount(4) +
// 8 points x (x,y)(64) + 7 handles x (h1x,h1y,h2x,h2y)(112). Unused
// point/handle slots must be serialized as bitwise +0.0f (the writer
// zero-fills; the parser enforces).


// Attenuation profile, 600 bytes: 12 scalar fields (48) then the three
// override curves in declaration order (volume, lowpass, width).


// Binding curve, 180 bytes: pointCount(4) + 8 points(64) +
// 7 handles x (h1x,h1y,h2x,h2y)(112). Same bitwise +0.0f tail discipline
// as the attenuation curve.


// Binding definition, 212 bytes: bindingId(8) + variableId(8) +
// targetScopeKind(4) + targetId(4) + transitionMs(4) + reserved(4) +
// curve(180). The directory id duplicates bindingId; the image layer
// cross-checks them.


// Voice Recipe: a 16-byte header followed by installCount fixed install
// records and bindingCount u64 binding ids. Each install record carries a
// bounded parameter payload in canonical per-type parameter-index order. The
// fixed 64-float payload keeps parsing bounded and lets future built-ins grow
// without changing record alignment; unused float slots and both reserved
// words must be bitwise zero. The image layer enforces ascending slot order,
// ascending binding ids, type/parameter semantics, and playable closure.


// Vintage 5 makes the spatial block positional rather than a promise of
// spatial authoring, so it also admits an all-zero absent form: a recipe can
// carry playback policy without carrying an attenuation.


// --- variable-size logic payloads ---

// Container: header (32) then childCount children of 24 bytes each —
// playableId(8) + weight(4) + matchValue(4) + delayMs(4) +
// delayJitterMs(4). The serializer writes header + children from the
// caller's array; the parser fills the header and then reads children
// one at a time into caller storage (childCount entries, validated
// against the payload size here).


inline constexpr std::uint32_t kInitCatalogMaxLocatorBytes = 256;
inline constexpr std::uint32_t kInitCatalogMaxStartupPacks = 256;
inline constexpr std::uint32_t kInitCatalogMaxEntries = 1048576;
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

inline bool PackIsCanonicalKind(std::uint32_t packKind) {
    return packKind == kPackKindInit || packKind == kPackKindCanonicalContent;
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

struct PackBusDefinitionSnapshotHeader {
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

struct PackBusDefinitionBus {
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

struct PackBusDefinitionSend {
    VnObjectId source{};
    VnObjectId target{};
    float levelDb = 0.0f;
};

struct PackBusDefinitionDownmix {
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




















}

namespace harken {
struct PackFilePrefix {
    std::uint32_t formatVersion{};
    std::uint32_t headerBytes{};
};

struct PackFileHeader {
    std::uint32_t formatVersion{};
    std::uint32_t headerBytes{};
    std::uint64_t fileBytes{};
    std::uint32_t packKind{};
    std::uint32_t entryCount{};
    std::uint64_t directoryOffset{};
    std::uint64_t directoryChecksum{};
};

struct PackMediaFileHeader {
    std::uint32_t formatVersion{};
    std::uint32_t headerBytes{};
    std::uint64_t fileBytes{};
    std::uint32_t entryCount{};
    std::uint32_t tocOffset{};
    std::uint64_t tocChecksum{};
};

struct PackDirectoryEntry {
    std::uint32_t kind{};
    std::uint32_t vintage{};
    VnObjectId id{};
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t payloadChecksum{};
    std::uint64_t relationOrdinal{};
};

struct PackVariablePolicyHeader {
    std::uint32_t rowCount{};
};

struct PackVariablePolicyRow {
    VnObjectId variableId{};
    std::uint8_t writers{};
    std::uint64_t executionFingerprint{};
};

struct PackVariableAnchorHeader {
    std::uint32_t count{};
};

struct PackVariableLabelHeader {
    std::uint64_t labelMappingFingerprint{};
    std::uint32_t labelCount{};
};

struct PackVariableLabel {
    VnObjectId labelId{};
    std::int32_t anchor{};
};

struct PackVariableRequirementsHeader {
    std::uint32_t rowCount{};
};

struct PackVariableRequirement {
    VnObjectId stableId{};
    std::uint64_t expectedExecutionFingerprint{};
    std::uint64_t expectedLabelMappingFingerprint{};
};
}

namespace harken {

struct PackWorldVariableWire {
    VnObjectId variableId{};
    std::uint32_t kind = 0;
    float rangeMin = 0;
    float rangeMax = 0;
    float defaultValue = 0;
};

}

#include <harken/media/generated/harken_pack_wire.g.h>

namespace harken {

inline bool ReadPackCanonicalPartitionHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackCanonicalPartitionHeader* out) noexcept {
    if (size < kPackCanonicalPartitionHeaderBytes ||
        !ParsePackCanonicalPartitionHeader(in, kPackCanonicalPartitionHeaderBytes, out)) {
        return false;
    }
    std::uint64_t expectedSize = 0;
    return out->headerBytes == kPackCanonicalPartitionHeaderBytes &&
           out->packStableId != 0 &&
           CheckedAddU64(out->headerBytes, out->bodyBytes, &expectedSize) &&
           size == expectedSize &&
           Fnv1a64(in, kPackCanonicalPartitionHeaderHeaderChecksumOffset) == out->headerChecksum &&
           Fnv1a64(in + out->headerBytes, static_cast<std::size_t>(out->bodyBytes)) == out->bodyChecksum;
}

inline bool PackRequirementBlockValid(const PackRequirementBlock& block) noexcept {
    return block.schemaVersion == kPackPackRequirementBlockVintage &&
           block.storageModelVersion == kPackPackRequirementBlockVintage &&
           block.packStableId != 0 && block.reserved0 == 0;
}

inline bool ReadPackRequirementBlock(const std::uint8_t* in, std::uint64_t size,
                                     PackRequirementBlock* out) noexcept {
    return ParsePackRequirementBlock(in, size, out) && PackRequirementBlockValid(*out);
}

inline bool ReadPackDependencyEdge(const std::uint8_t* in, std::uint64_t size,
                                   PackDependencyEdge* out) noexcept {
    out->reserved0 = 0;
    return ParsePackDependencyEdge(in, size, out) &&
           out->prerequisitePackStableId != 0 &&
           (out->edgeKind == kPackDependencyEdgeKindInit ||
            out->edgeKind == kPackDependencyEdgeKindShared ||
            out->edgeKind == kPackDependencyEdgeKindContent) &&
           out->required01 <= 1;
}


template <class Byte>
struct PackWireBuffer {
    Byte* data;
    std::uint64_t size;
    std::uint64_t offset = 0;
    std::uint64_t Remaining() const noexcept { return size - offset; }
    Byte* Take(std::uint64_t bytes) noexcept {
        if (bytes > size - offset) return nullptr;
        Byte* result = data + offset;
        offset += bytes;
        return result;
    }
};

inline std::uint64_t PackContainerPayloadBytes(std::uint32_t childCount) noexcept {
    struct Visitor {
        PackContainerEntryHeader header{};
        const PackContainerEntryHeader& Value() const noexcept { return header; }
    } visitor;
    visitor.header.childCount = childCount;
    std::uint64_t size = 0;
    return MeasurePackContainerPayload(visitor, size) ? size : 0;
}

inline std::uint64_t PackVoiceRecipePayloadBytes(std::uint32_t installCount,
                                                std::uint32_t bindingCount) noexcept {
    struct Visitor {
        PackVoiceRecipeEntryHeader header{};
        const PackVoiceRecipeEntryHeader& Value() const noexcept { return header; }
    } visitor;
    visitor.header.installCount = installCount;
    visitor.header.bindingCount = bindingCount;
    std::uint64_t size = 0;
    return MeasurePackVoiceRecipePayload(visitor, size) ? size : 0;
}

inline bool ReadPackContainerHeader(const std::uint8_t* in, std::uint64_t size,
                                    PackContainerEntryHeader* out) noexcept {
    return size >= kPackContainerHeaderBytes &&
           ParsePackContainerHeader(in, kPackContainerHeaderBytes, out) &&
           size == PackContainerPayloadBytes(out->childCount);
}

inline PackContainerChild ReadPackContainerChild(const std::uint8_t* in) noexcept {
    PackContainerChild row{};
    (void)ParsePackContainerChild(in, kPackContainerChildBytes, &row);
    return row;
}

inline bool ReadPackVoiceRecipeHeader(const std::uint8_t* in, std::uint64_t size,
                                      PackVoiceRecipeEntryHeader* out) noexcept {
    return size >= kPackVoiceRecipeHeaderBytes &&
           ParsePackVoiceRecipeHeader(in, kPackVoiceRecipeHeaderBytes, out) &&
           out->installCount <= kPackVoiceRecipeMaxInstalls &&
           out->bindingCount <= kPackVoiceRecipeMaxBindings &&
           size == PackVoiceRecipePayloadBytes(out->installCount, out->bindingCount);
}

inline bool PackVoiceRecipeToneValid(const PackVoiceRecipeTone& tone) noexcept {
    const auto* out = &tone;
    if (out->present > 1 ||
        (out->sectionEnableMask & ~kPackVoiceToneEnableMask) != 0 ||
        (out->present == 0 && out->sectionEnableMask != 0)) return false;
    if (out->present != 0) return true;
    const float values[] = {out->lowpassFrequencyHz, out->lowpassQ, out->highpassFrequencyHz,
                            out->highpassQ, out->lowShelfFrequencyHz, out->lowShelfQ,
                            out->lowShelfGainDb, out->highShelfFrequencyHz, out->highShelfQ,
                            out->highShelfGainDb};
    return std::all_of(std::begin(values), std::end(values), [](float value) {
        return std::bit_cast<std::uint32_t>(value) == 0;
    });
}

inline bool ReadPackVoiceRecipeTone(const std::uint8_t* in, PackVoiceRecipeTone* out) noexcept {
    return ParsePackVoiceRecipeTone(in, kPackVoiceRecipeToneBytes, out) && PackVoiceRecipeToneValid(*out);
}

inline bool PackVoiceRecipeSpatialValid(const PackVoiceRecipeSpatial& spatial) noexcept {
    if (spatial.reserved0 != 0) return false;
    if (spatial.attenuationId == VnBindingId{}) return std::bit_cast<std::uint32_t>(spatial.distanceScale) == 0;
    return std::isfinite(spatial.distanceScale) && spatial.distanceScale > 0.0f;
}

inline bool ReadPackVoiceRecipeSpatial(const std::uint8_t* in, PackVoiceRecipeSpatial* out) noexcept {
    *out = {};
    return ParsePackVoiceRecipeSpatial(in, kPackVoiceRecipeSpatialBytes, out) && PackVoiceRecipeSpatialValid(*out);
}

inline bool ReadPackVoiceRecipePlayback(const std::uint8_t* in, PackVoiceRecipePlayback* out) noexcept {
    return ParsePackVoiceRecipePlayback(in, kPackVoiceRecipePlaybackBytes, out) &&
           out->present <= 1 && (out->present != 0 || out->loopCount == 0);
}

inline void WritePackVoiceRecipeInstall(const PackVoiceRecipeInstall& install, std::uint8_t* out) noexcept {
    PackVoiceRecipeInstallWire row;
    static_cast<PackVoiceRecipeInstall&>(row) = install;
    row.paramBytes = install.paramCount * sizeof(float);
    SerializePackVoiceRecipeInstall(row, out);
}

inline bool ReadPackVoiceRecipeInstall(const std::uint8_t* in, PackVoiceRecipeInstall* out) noexcept {
    PackVoiceRecipeInstallWire row{};
    if (!ParsePackVoiceRecipeInstall(in, kPackVoiceRecipeInstallBytes, &row) ||
        row.bypass > 1 || row.paramCount > kPackVoiceRecipeMaxParams ||
        row.paramBytes != row.paramCount * sizeof(float)) return false;
    *out = row;
    return true;
}

inline VnBindingId ReadPackVoiceRecipeBinding(const std::uint8_t* in) noexcept {
    VnBindingId row{};
    (void)ParsePackVoiceRecipeBinding(in, kPackVoiceRecipeBindingBytes, &row);
    return row;
}

template <class Container>
inline bool ReadPackContainerPayload(const std::uint8_t* in, std::uint64_t size,
                                      VnBindingId expectedId, Container& container) {
    if (!ReadPackContainerHeader(in, size, &container.header) ||
        container.header.containerId == VnBindingId{} || container.header.containerId != expectedId) return false;
    container.children.resize(container.header.childCount);
    struct Visitor {
        Container& row;
        auto& Value() noexcept { return row.header; }
        auto& Header() noexcept { return row.header; }
        bool PrepareChildren(std::uint64_t count) noexcept {
            return count == row.children.size();
        }
        auto& Children(std::uint64_t index) noexcept { return row.children[static_cast<std::size_t>(index)]; }
        bool Finish() noexcept { return true; }
    } visitor{container};
    PackWireBuffer<const std::uint8_t> buffer{in, size};
    return ReadPackContainerPayload(buffer, visitor) && buffer.offset == size;
}

template <class Recipe>
inline bool ReadPackVoiceRecipePayload(const std::uint8_t* in, std::uint64_t size,
                                       VnBindingId expectedId, Recipe& recipe) {
    if (!ReadPackVoiceRecipeHeader(in, size, &recipe.header) ||
        recipe.header.playableId == VnBindingId{} || recipe.header.playableId != expectedId) return false;
    recipe.installs.resize(recipe.header.installCount);
    recipe.bindings.resize(recipe.header.bindingCount);
    struct InstallVisitor {
        PackVoiceRecipeInstall& target;
        PackVoiceRecipeInstallWire row{};
        auto& Wire() noexcept { return row; }
        bool Finish() noexcept {
            if (row.bypass > 1 || row.paramCount > kPackVoiceRecipeMaxParams ||
                row.paramBytes != row.paramCount * sizeof(float)) return false;
            target = row;
            return true;
        }
    };
    struct Visitor {
        Recipe& row;
        auto& Value() noexcept { return row.header; }
        auto& Header() noexcept { return row.header; }
        auto& Tone() noexcept { return row.tone; }
        auto& Spatial() noexcept { return row.spatial; }
        auto& Playback() noexcept { return row.playback; }
        bool PrepareInstalls(std::uint64_t count) noexcept {
            if (count != row.installs.size() ||
                !PackVoiceRecipeToneValid(row.tone) || !PackVoiceRecipeSpatialValid(row.spatial) ||
                row.playback.present > 1 || (row.playback.present == 0 && row.playback.loopCount != 0)) return false;
            return true;
        }
        InstallVisitor Installs(std::uint64_t index) noexcept { return {row.installs[static_cast<std::size_t>(index)]}; }
        bool PrepareBindings(std::uint64_t count) noexcept {
            return count == row.bindings.size();
        }
        auto& Bindings(std::uint64_t index) noexcept { return row.bindings[static_cast<std::size_t>(index)]; }
        bool Finish() noexcept { return true; }
    } visitor{recipe};
    PackWireBuffer<const std::uint8_t> buffer{in, size};
    return ReadPackVoiceRecipePayload(buffer, visitor) && buffer.offset == size;
}


inline PackTocEntry ReadPackMedia(const std::uint8_t* in) noexcept {
    PackTocEntry row{};
    (void)ParsePackMedia(in, kPackMediaBytes, &row);
    return row;
}

inline std::uint64_t PackTocByteOffset(std::uint32_t index) noexcept {
    return static_cast<std::uint64_t>(index) * kPackMediaBytes;
}

inline bool ReadPackBusEffect(const std::uint8_t* in, std::uint64_t size,
                              PackBusEffectEntry* out) noexcept {
    return ParsePackBusEffect(in, size, out) &&
           out->paramCount <= kPackVoiceRecipeMaxParams &&
           out->paramBytes == out->paramCount * sizeof(float);
}

inline bool ReadPackAttenuation(const std::uint8_t* in, std::uint64_t size,
                                VnAttenuationDesc* out) noexcept {
    *out = {};
    out->structSize = static_cast<std::uint32_t>(offsetof(VnAttenuationDesc, highpassCurve));
    return ParsePackAttenuation(in, size, out);
}

inline void WritePackBindingDefinition(VnBindingId bindingId, const VnBindingDesc& definition,
                                       std::uint8_t* out) noexcept {
    SerializePackBindingDefinition({bindingId, definition}, out);
}

inline bool ReadPackBindingDefinition(const std::uint8_t* in, std::uint64_t size,
                                      VnBindingId* outId, VnBindingDesc* out) noexcept {
    PackBindingDefinitionEntry row{};
    row.definition.structSize = sizeof(VnBindingDesc);
    row.definition.curve.structSize = sizeof(VnCurveDesc);
    if (!ParsePackBindingDefinition(in, size, &row)) return false;
    *outId = row.bindingId;
    *out = row.definition;
    return true;
}


inline bool PackIsCanonicalEntryKind(std::uint32_t entryKind) {
    return entryKind == kPackEntryKindCanonicalDefinitionPartition ||
           entryKind == kPackEntryKindPackRequirementBlock ||
           entryKind == kPackEntryKindPackDependencyEdge ||
           entryKind == kPackEntryKindBusDefinitionSnapshot ||
           entryKind == kPackEntryKindVariablePolicyOverlay ||
           entryKind == kPackEntryKindVariableRequirements;
}
inline bool ReadPackBusDefinitionSnapshotHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackBusDefinitionSnapshotHeader* out) noexcept {
    if (size < kPackBusDefinitionSnapshotHeaderBytes ||
        !ParsePackBusDefinitionSnapshotHeader(in, kPackBusDefinitionSnapshotHeaderBytes, out)) {
        return false;
    }
    std::uint64_t expectedSize = 0;
    if (out->schemaVersion != kPackBusDefinitionSnapshotVintage ||
        out->storageModelVersion != kPackBusDefinitionSnapshotVintage ||
        out->headerBytes != kPackBusDefinitionSnapshotHeaderBytes ||
        (out->publicationCapabilityVintage != 3u && out->publicationCapabilityVintage != 4u) ||
        out->profileVintage != 1 || out->reserved0 != 0 || out->reserved1 != 0 ||
        out->busRowBytes != kPackBusDefinitionBusBytes ||
        out->sendRowBytes != kPackBusDefinitionSendBytes ||
        out->downmixRowBytes != kPackBusDefinitionDownmixBytes ||
        out->coefficientRowBytes != kPackBusDefinitionCoefficientBytes ||
        !CheckedAddU64(out->headerBytes, out->payloadBytes, &expectedSize) ||
        size != expectedSize) {
        return false;
    }
    constexpr auto checksumOffset = kPackBusDefinitionSnapshotHeaderChecksumOffset;
    constexpr auto checksumEnd = kPackBusDefinitionSnapshotHeaderCompileTargetProfileFingerprintOffset;
    const std::uint8_t checksumZero[checksumEnd - checksumOffset] = {};
    std::uint64_t hash = Fnv1a64(in, checksumOffset);
    hash = Fnv1a64(checksumZero, sizeof(checksumZero), hash);
    hash = Fnv1a64(in + checksumEnd, static_cast<std::size_t>(size - checksumEnd), hash);
    return hash == out->checksum;
}

} // namespace harken

namespace harken {

inline bool ReadPackMediaFileHeader(const std::uint8_t* in, PackMediaFileHeader* out) noexcept {
    return ParsePackMediaFileHeader(in, kPackMediaFileHeaderBytes, out);
}

inline bool ReadPackFileHeader(const std::uint8_t* in, PackFileHeader* out) noexcept {
    return ParsePackFileHeader(in, kPackFileHeaderBytes, out);
}

inline PackDirectoryEntry ReadPackDirectoryEntry(const std::uint8_t* in) noexcept {
    PackDirectoryEntry row;
    (void)ParsePackDirectoryEntry(in, kPackDirectoryEntryBytes, &row);
    return row;
}

}



#endif // HARKEN_MEDIA_PACK_FORMAT_V2_H
