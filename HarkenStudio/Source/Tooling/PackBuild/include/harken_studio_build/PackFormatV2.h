#ifndef HARKEN_STUDIO_BUILD_PACK_FORMAT_V2_H
#define HARKEN_STUDIO_BUILD_PACK_FORMAT_V2_H

#include <harken_studio/ObjectId.h>
#include <harken_studio_build/AuthoredParameters.h>
#include <harken_studio/AudioValues.h>

#include "harken_studio_build/WorldBodyVintage.h"
#include "harken_studio_build/PackFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace harkenstudiobuild {

inline constexpr std::uint32_t kPackFormatVersion2 = HARKEN_PACK_SCHEMA_VERSION;
inline constexpr std::uint32_t kPackV2HeaderBytes = 44;
inline constexpr std::uint32_t kPackV2DirectoryEntryBytes = 56;
inline constexpr std::uint32_t kPackKindResident = 0;
inline constexpr std::uint32_t kPackKindContent = 1;
inline constexpr std::uint32_t kPackKindInit = 2;
inline constexpr std::uint32_t kPackKindCanonicalContent = 3;
inline constexpr std::uint32_t kPackEntryKindInitRoutingCatalog = 20;
inline constexpr std::uint32_t kPackEntryKindCanonicalDefinitionPartition = 21;
inline constexpr std::uint32_t kPackEntryKindGameplayCatalog = 25;
inline constexpr std::uint32_t kPackEntryKindVariablePolicyOverlay = 26;
inline constexpr std::uint32_t kPackEntryKindVariableRequirements = 27;
inline constexpr std::uint32_t kPackEntryKindVariableNameCatalog = 28;
inline constexpr std::uint32_t kPackVariablePolicyOverlayFormat = 2;
inline constexpr std::uint32_t kPackCanonicalEntryVintage = 1;
inline constexpr std::uint32_t kPackCanonicalRequirementVintage = 2;
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
inline constexpr std::uint32_t kPackV2SchemaVintage = 2;
inline constexpr std::uint64_t kPackV2PayloadAlign = 8;
inline constexpr std::uint32_t kPackVoiceRecipeMaxInstalls = 8;
inline constexpr std::uint32_t kPackVoiceRecipeMaxBindings = 8;
inline constexpr std::uint32_t kPackV2VoiceRecipeMaxParams = 64;
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
    harkenstudio::ObjectId id{};
    std::uint64_t relationOrdinal = 0;
    std::uint64_t offset = 0;  // payload file offset
    std::uint64_t size = 0;    // payload byte size
    std::uint64_t payloadChecksum = 0;
};

struct PackBusEntry {
    harkenstudio::ObjectId busId{};
    harkenstudio::ObjectId parentBusId{};
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
};

struct PackBusSendEntry {
    harkenstudio::ObjectId fromBusId{};
    harkenstudio::ObjectId toBusId{};
    float levelDb = 0.0f;
};


struct PackBindingDefinitionEntry {
    harkenstudio::ObjectId bindingId{};
    BindingDefinition definition{};
};

struct PackBindingAttachmentEntry {
    harkenstudio::ObjectId bindingId{};
    std::uint32_t scopeKind = 0;
    harkenstudio::ObjectId objectId{};
};

struct PackVoiceRecipeEntryHeader {
    harkenstudio::ObjectId playableId{};
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
    harkenstudio::ObjectId effectId{};
};

struct PackVoiceRecipeInstallWire : PackVoiceRecipeInstall {
    std::uint32_t paramBytes = 0;
};

struct PackBusEffectEntry {
    harkenstudio::ObjectId busId{};
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t bypass = 0;
    std::uint32_t faderPointGuard = 0;
    std::uint32_t paramCount = 0;
    std::uint32_t paramBytes = 0;
    std::array<float, kPackV2VoiceRecipeMaxParams> params{};
    harkenstudio::ObjectId effectId{};
};

struct PackEffectDynamicRouteEntry {
    harkenstudio::ObjectId targetBusId{};
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t expectedEffectType = 0;
    std::uint32_t signalForm = 0;
    std::uint32_t sourceKind = 0;
    std::uint32_t tap = 0;
    harkenstudio::ObjectId sourceId{};
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
    harkenstudio::ObjectId attenuationId{};
    float distanceScale = 1.0f;
    std::uint32_t reserved0 = 0;
};

struct PackVoiceRecipePlayback {
    std::uint32_t present = 0;
    std::uint32_t loopCount = 0;
};
struct PackContainerEntryHeader {
    harkenstudio::ObjectId containerId{};
    std::uint32_t selectionOrder = 0;
    std::uint32_t avoidRepeatWindow = 0;
    harkenstudio::ObjectId selectionVariableId{};
    std::uint32_t defaultIndex = 0;
    std::uint32_t childCount = 0;
};

struct PackContainerChild {
    harkenstudio::ObjectId playableId{};
    std::uint32_t weight = 0;
    std::uint32_t matchValue = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t delayJitterMs = 0;
};

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










































































inline constexpr std::uint32_t kInitCatalogHeaderBytes = 88;
inline constexpr std::uint32_t kInitCatalogRowBytes = 312;
inline constexpr std::uint32_t kInitCatalogContentHashOffset = 304;
inline constexpr std::uint32_t kInitCatalogMaxLocatorBytes = 256;
inline constexpr std::uint32_t kInitCatalogMaxStartupPacks = 256;
inline constexpr std::uint32_t kInitCatalogMaxEntries = 1048576;
inline constexpr std::uint32_t kCanonicalDefinitionPartitionHeaderBytes = 64;
inline constexpr std::uint32_t kCanonicalDefinitionPartitionHeaderV2Bytes = 88;
inline constexpr char kCanonicalDefinitionPartitionMagic[8] = {
    'V', 'N', 'D', 'E', 'F', 'P', '0', '1'};
inline constexpr char kCanonicalDefinitionPartitionMagicV2[8] = {
    'V', 'N', 'D', 'E', 'F', 'P', '0', '2'};
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
    harkenstudio::ObjectId stableId{};
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
inline constexpr harkenstudio::ObjectId CanonicalSnapshotBusIdV1(std::uint64_t packed) noexcept {
    return packed == 0 ? harkenstudio::ObjectId{} : harkenstudio::ObjectId{0, packed};
}

inline constexpr std::uint64_t CanonicalSnapshotPackedBusIdV1(harkenstudio::ObjectId id) noexcept {
    return id.high == 0 ? id.low : 0;
}

inline constexpr bool CanonicalSnapshotIsMasterV1(std::uint64_t packed) noexcept {
    return packed == harkenstudio::kMasterBusObjectId.low;
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
    harkenstudio::ObjectId busId{};
    harkenstudio::ObjectId parentId{};
    std::uint32_t feedsFirstOrdinal = 0;
    float faderDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    std::uint32_t faderAfterSlot = 0;
    std::uint32_t outputLayout = 0;
    std::uint32_t effectFirst = 0;
    std::uint32_t effectCount = 0;
    std::uint32_t downmixFirst = 0;
    std::uint32_t downmixCount = 0;
};

struct PackBusDefinitionSend {
    harkenstudio::ObjectId source{};
    harkenstudio::ObjectId target{};
    float levelDb = 0.0f;
};

struct PackBusDefinitionDownmix {
    harkenstudio::ObjectId busId{};
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
    if (out->stableId == harkenstudio::ObjectId{} || out->ownerPackStableId == 0 ||
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
    harkenstudio::ObjectId actionStableId{};
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
    harkenstudio::ObjectId lastActionStableId{};
    std::uint64_t lastNameBindingRevision = 0;
    std::uint32_t reason = 0;
    std::uint16_t nameLength = 0;
    std::uint16_t reserved0 = 0;
    std::uint32_t nameOffset = 0;
    std::uint32_t reserved1 = 0;
};

struct PackGameplayCatalogActiveDraft {
    harkenstudio::ObjectId actionStableId{};
    std::uint64_t nameBindingRevision = 0;
    std::uint64_t callableFingerprint = 0;
    std::uint64_t ownerPackStableId = 0;
    std::uint64_t providerClosureFingerprint = 0;
    std::uint16_t flags = 0;
    std::string_view name;
};

struct PackGameplayCatalogRetiredDraft {
    harkenstudio::ObjectId lastActionStableId{};
    std::uint64_t lastNameBindingRevision = 0;
    std::uint32_t reason = 0;
    std::string_view name;
};

inline std::uint64_t GameplayCallableFingerprint(
    harkenstudio::ObjectId actionId, std::uint64_t nameRevision, std::string_view name) {
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
    if (out->actionStableId == harkenstudio::ObjectId{} || out->ownerPackStableId == 0 ||
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
    return out->lastActionStableId != harkenstudio::ObjectId{} && out->nameLength >= 1 &&
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
            row.actionStableId == harkenstudio::ObjectId{} || row.ownerPackStableId == 0 ||
            row.nameBindingRevision < 1 ||
            (row.flags & ~kGameplayCatalogActiveFlagOptional) != 0) {
            return false;
        }
    }
    for (const auto& row : retired) {
        if (row.name.empty() || row.name.size() > kGameplayCatalogMaxNameBytes ||
            row.lastActionStableId == harkenstudio::ObjectId{} ||
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

}

#include <harken_studio_build/generated/harken_pack_wire.g.h>

namespace harkenstudiobuild {

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
    if (spatial.attenuationId == harkenstudio::ObjectId{}) return std::bit_cast<std::uint32_t>(spatial.distanceScale) == 0;
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

inline harkenstudio::ObjectId ReadPackVoiceRecipeBinding(const std::uint8_t* in) noexcept {
    harkenstudio::ObjectId row{};
    (void)ParsePackVoiceRecipeBinding(in, kPackVoiceRecipeBindingBytes, &row);
    return row;
}

template <class Container>
inline bool ReadPackContainerPayload(const std::uint8_t* in, std::uint64_t size,
                                      harkenstudio::ObjectId expectedId, Container& container) {
    if (!ReadPackContainerHeader(in, size, &container.header) ||
        container.header.containerId == harkenstudio::ObjectId{} || container.header.containerId != expectedId) return false;
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
                                       harkenstudio::ObjectId expectedId, Recipe& recipe) {
    if (!ReadPackVoiceRecipeHeader(in, size, &recipe.header) ||
        recipe.header.playableId == harkenstudio::ObjectId{} || recipe.header.playableId != expectedId) return false;
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
                                AttenuationDefinition* out) noexcept {
    *out = {};
    return ParsePackAttenuation(in, size, out);
}

inline void WritePackBindingDefinition(harkenstudio::ObjectId bindingId, const BindingDefinition& definition,
                                       std::uint8_t* out) noexcept {
    SerializePackBindingDefinition({bindingId, definition}, out);
}

inline bool ReadPackBindingDefinition(const std::uint8_t* in, std::uint64_t size,
                                      harkenstudio::ObjectId* outId, BindingDefinition* out) noexcept {
    PackBindingDefinitionEntry row{};
    if (!ParsePackBindingDefinition(in, size, &row)) return false;
    *outId = row.bindingId;
    *out = row.definition;
    return true;
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
    if (entryKind == kPackEntryKindCanonicalDefinitionPartition) {
        return kPackCanonicalRequirementVintage;
    }
    if (entryKind == kPackEntryKindPackRequirementBlock) {
        return kPackPackRequirementBlockVintage;
    }
    if (entryKind == kPackEntryKindPackDependencyEdge) {
        return kPackPackDependencyEdgeVintage;
    }
    if (entryKind == kPackEntryKindBusDefinitionSnapshot) {
        return kPackBusDefinitionSnapshotVintage;
    }
    return kPackCanonicalEntryVintage;
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

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_PACK_FORMAT_V2_H
