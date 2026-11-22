#ifndef HARKEN_STUDIO_BUILD_PACK_FORMAT_V2_H
#define HARKEN_STUDIO_BUILD_PACK_FORMAT_V2_H

#include <harken_studio/ObjectId.h>
#include <harken_studio_build/AuthoredParameters.h>
#include <harken_studio/AudioValues.h>

#include "harken_studio_build/WorldBodyVintage.h"
#include "harken_studio_build/PackFormat.h"

#include <harken_studio_build/World.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace harken::studio::build {

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

struct PackBusEntry {
    harken::studio::types::ObjectId busId{};
    harken::studio::types::ObjectId parentBusId{};
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
};

struct PackBusSendEntry {
    harken::studio::types::ObjectId fromBusId{};
    harken::studio::types::ObjectId toBusId{};
    float levelDb = 0.0f;
};

struct PackBindingDefinitionEntry {
    harken::studio::types::ObjectId bindingId{};
    BindingDefinition definition{};
};

struct PackBindingAttachmentEntry {
    harken::studio::types::ObjectId bindingId{};
    std::uint32_t scopeKind = 0;
    harken::studio::types::ObjectId objectId{};
};

struct PackVoiceRecipeEntryHeader {
    harken::studio::types::ObjectId playableId{};
    std::uint32_t installCount = 0;
    std::uint32_t bindingCount = 0;
};

struct PackVoiceRecipeInstall {
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t bypass = 0;
    std::uint32_t paramCount = 0;
    std::array<float, HK_PACK_VOICE_RECIPE_MAX_PARAMS> params{};
    harken::studio::types::ObjectId effectId{};
};

struct PackVoiceRecipeInstallWire : PackVoiceRecipeInstall {
    std::uint32_t paramBytes = 0;
};

struct PackBusEffectEntry {
    harken::studio::types::ObjectId busId{};
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t bypass = 0;
    std::uint32_t faderPointGuard = 0;
    std::uint32_t paramCount = 0;
    std::uint32_t paramBytes = 0;
    std::array<float, HK_PACK_VOICE_RECIPE_MAX_PARAMS> params{};
    harken::studio::types::ObjectId effectId{};
};

struct PackEffectDynamicRouteEntry {
    harken::studio::types::ObjectId targetBusId{};
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t expectedEffectType = 0;
    std::uint32_t signalForm = 0;
    std::uint32_t sourceKind = 0;
    std::uint32_t tap = 0;
    harken::studio::types::ObjectId sourceId{};
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
    harken::studio::types::ObjectId attenuationId{};
    float distanceScale = 1.0f;
    std::uint32_t reserved0 = 0;
};

struct PackVoiceRecipePlayback {
    std::uint32_t present = 0;
    std::uint32_t loopCount = 0;
};
struct PackContainerEntryHeader {
    harken::studio::types::ObjectId containerId{};
    std::uint32_t selectionOrder = 0;
    std::uint32_t avoidRepeatWindow = 0;
    harken::studio::types::ObjectId selectionVariableId{};
    std::uint32_t defaultIndex = 0;
    std::uint32_t childCount = 0;
};

struct PackContainerChild {
    harken::studio::types::ObjectId playableId{};
    std::uint32_t weight = 0;
    std::uint32_t matchValue = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t delayJitterMs = 0;
};

inline constexpr std::uint32_t kCanonicalDefinitionPartitionBodyVintage =
    kCurrentWorldShippingBodyVintage;
inline constexpr std::uint32_t kInitCatalogIdentityClassAction = 1;
inline constexpr std::uint32_t kInitCatalogIdentityClassPlayable = 2;
inline constexpr std::uint32_t kInitCatalogIdentityClassVariable = 3;
inline constexpr std::uint32_t kInitCatalogIdentityClassResource = 4;

inline bool PackIsCanonicalKind(std::uint32_t packKind) {
    return packKind == HK_PACK_KIND_INIT || packKind == HK_PACK_KIND_CANONICAL_CONTENT;
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
inline constexpr harken::studio::types::ObjectId CanonicalSnapshotBusIdV1(std::uint64_t packed) noexcept {
    return packed == 0 ? harken::studio::types::ObjectId{} : harken::studio::types::ObjectId{0, packed};
}

inline constexpr std::uint64_t CanonicalSnapshotPackedBusIdV1(harken::studio::types::ObjectId id) noexcept {
    return id.high == 0 ? id.low : 0;
}

inline constexpr bool CanonicalSnapshotIsMasterV1(std::uint64_t packed) noexcept {
    return packed == harken::studio::types::kMasterBusObjectId.low;
}

struct PackBusDefinitionBus {
    harken::studio::types::ObjectId busId{};
    harken::studio::types::ObjectId parentId{};
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
    harken::studio::types::ObjectId source{};
    harken::studio::types::ObjectId target{};
    float levelDb = 0.0f;
};

struct PackEffectPresetHeader {
    harken::studio::types::ObjectId presetId{};
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t paramCount = 0;
};

struct PackEffectPresetParameter {
    std::uint32_t paramId = 0;
    std::uint32_t valueBits = 0;
};

struct PackEffectPreset {
    PackEffectPresetHeader header{};
    std::vector<PackEffectPresetParameter> parameters;
};

struct PackBusDefinitionEffect {
    harken::studio::types::ObjectId busId{};
    harken::studio::types::ObjectId effectId{};
    std::uint32_t slot = 0;
    std::uint32_t effectType = 0;
    std::uint32_t descriptorVintage = 0;
    std::uint32_t flags = 0;
    std::uint32_t parameterFirst = 0;
    std::uint32_t parameterCount = 0;
};

struct PackBusDefinitionEffectParameter {
    std::uint32_t index = 0;
    float value = 0.0f;
};

struct PackBusDefinitionDownmix {
    harken::studio::types::ObjectId busId{};
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

namespace harken::studio::build {
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

struct StreamHeader {
    std::uint32_t formatVersion{};
    std::uint32_t headerBytes{};
    std::uint64_t fileBytes{};
    std::uint32_t entryCount{};
    std::uint32_t tocOffset{};
    std::uint32_t chunkFrames{};
    std::uint64_t tocChecksum{};
};

struct StreamTocEntry {
    harken::studio::types::ObjectId soundId{};
    std::uint32_t sampleRate{};
    std::uint16_t channelCount{};
    std::uint16_t sampleFormat{};
    std::uint64_t frameCount{};
    std::uint64_t payloadOffset{};
    std::uint64_t payloadBytes{};
    std::uint64_t chunkChecksumOffset{};
    std::uint32_t chunkCount{};
    std::uint32_t flags{};
    std::uint64_t loopStartFrame{};
    std::uint64_t loopEndFrame{};
};

struct PackDirectoryEntry {
    std::uint32_t kind{};
    std::uint32_t vintage{};
    harken::studio::types::ObjectId id{};
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t payloadChecksum{};
    std::uint64_t relationOrdinal{};
};

struct PackVariablePolicyHeader {
    std::uint32_t rowCount{};
};

struct PackVariablePolicyRow {
    harken::studio::types::ObjectId variableId{};
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
    harken::studio::types::ObjectId labelId{};
    std::int32_t anchor{};
};

struct PackVariableRequirementsHeader {
    std::uint32_t rowCount{};
};

struct PackVariableRequirement {
    harken::studio::types::ObjectId stableId{};
    std::uint64_t expectedExecutionFingerprint{};
    std::uint64_t expectedLabelMappingFingerprint{};
};
}

namespace harken::studio::build {

struct PackWorldVariableWire {
    harken::studio::types::ObjectId variableId{};
    std::uint32_t kind = 0;
    float rangeMin = 0;
    float rangeMax = 0;
    float defaultValue = 0;
};

}

namespace harken::studio::build {

struct PackSegmentEntryTimeline {
    harken::studio::types::ObjectId stableMarkerId{};
    std::uint64_t position = 0;
    std::uint64_t contentStart = 0;
    std::uint64_t contentEnd = 0;
    std::uint64_t nominalStart = 0;
    std::uint64_t nominalEnd = 0;
};

}

#include <harken_studio_build/PackWire.h>

namespace harken::studio::build {

inline bool ReadPackCanonicalPartitionHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackCanonicalPartitionHeader* out) noexcept {
    if (size < kPackBytes<HkPackCanonicalPartitionHeader> ||
        !ParsePackRow<HkPackCanonicalPartitionHeader>(in, kPackBytes<HkPackCanonicalPartitionHeader>, out)) {
        return false;
    }
    std::uint64_t expectedSize = 0;
    return out->headerBytes == kPackBytes<HkPackCanonicalPartitionHeader> &&
           out->packStableId != 0 &&
           CheckedAddU64(out->headerBytes, out->bodyBytes, &expectedSize) &&
           size == expectedSize &&
           Fnv1a64(in, offsetof(HkPackCanonicalPartitionHeader, headerChecksum)) == out->headerChecksum &&
           Fnv1a64(in + out->headerBytes, static_cast<std::size_t>(out->bodyBytes)) == out->bodyChecksum;
}

inline bool PackRequirementBlockValid(const PackRequirementBlock& block) noexcept {
    return block.schemaVersion == HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE &&
           block.storageModelVersion == HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE &&
           block.packStableId != 0 && block.reserved0 == 0;
}

inline bool ReadPackRequirementBlock(const std::uint8_t* in, std::uint64_t size,
                                     PackRequirementBlock* out) noexcept {
    return ParsePackRow<HkPackRequirementBlock>(in, size, out) && PackRequirementBlockValid(*out);
}

inline bool ReadPackDependencyEdge(const std::uint8_t* in, std::uint64_t size,
                                   PackDependencyEdge* out) noexcept {
    out->reserved0 = 0;
    return ParsePackRow<HkPackDependencyEdge>(in, size, out) &&
           out->prerequisitePackStableId != 0 &&
           (out->edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT ||
            out->edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_SHARED ||
            out->edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_CONTENT) &&
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
    return size >= kPackBytes<HkPackContainerHeader> &&
           ParsePackRow<HkPackContainerHeader>(in, kPackBytes<HkPackContainerHeader>, out) &&
           size == PackContainerPayloadBytes(out->childCount);
}

inline bool ReadPackVoiceRecipeHeader(const std::uint8_t* in, std::uint64_t size,
                                      PackVoiceRecipeEntryHeader* out) noexcept {
    return size >= kPackBytes<HkPackVoiceRecipeHeader> &&
           ParsePackRow<HkPackVoiceRecipeHeader>(in, kPackBytes<HkPackVoiceRecipeHeader>, out) &&
           out->installCount <= HK_PACK_VOICE_RECIPE_MAX_INSTALLS &&
           out->bindingCount <= HK_PACK_VOICE_RECIPE_MAX_BINDINGS &&
           size == PackVoiceRecipePayloadBytes(out->installCount, out->bindingCount);
}

inline bool PackVoiceRecipeToneValid(const PackVoiceRecipeTone& tone) noexcept {
    const auto* out = &tone;
    if (out->present > 1 ||
        (out->sectionEnableMask & ~HK_PACK_VOICE_TONE_SECTIONS) != 0 ||
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
    return ParsePackRow<HkPackVoiceRecipeTone>(in, kPackBytes<HkPackVoiceRecipeTone>, out) && PackVoiceRecipeToneValid(*out);
}

inline bool PackVoiceRecipeSpatialValid(const PackVoiceRecipeSpatial& spatial) noexcept {
    if (spatial.reserved0 != 0) return false;
    if (spatial.attenuationId == harken::studio::types::ObjectId{}) return std::bit_cast<std::uint32_t>(spatial.distanceScale) == 0;
    return std::isfinite(spatial.distanceScale) && spatial.distanceScale > 0.0f;
}

inline bool ReadPackVoiceRecipeSpatial(const std::uint8_t* in, PackVoiceRecipeSpatial* out) noexcept {
    *out = {};
    return ParsePackRow<HkPackVoiceRecipeSpatial>(in, kPackBytes<HkPackVoiceRecipeSpatial>, out) && PackVoiceRecipeSpatialValid(*out);
}

inline bool PackVoiceRecipePlaybackValid(const PackVoiceRecipePlayback& playback) noexcept {
    return playback.present <= 1 && (playback.present != 0 || playback.loopCount == 0);
}

inline bool ReadPackVoiceRecipePlayback(const std::uint8_t* in, PackVoiceRecipePlayback* out) noexcept {
    return ParsePackRow<HkPackVoiceRecipePlayback>(in, kPackBytes<HkPackVoiceRecipePlayback>, out) && PackVoiceRecipePlaybackValid(*out);
}

inline void WritePackVoiceRecipeInstall(const PackVoiceRecipeInstall& install, std::uint8_t* out) noexcept {
    PackVoiceRecipeInstallWire row;
    static_cast<PackVoiceRecipeInstall&>(row) = install;
    row.paramBytes = install.paramCount * sizeof(float);
    SerializePackRow<HkPackVoiceRecipeInstall>(row, out);
}

inline bool PackVoiceRecipeInstallWireValid(const PackVoiceRecipeInstallWire& row) noexcept {
    return row.bypass <= 1 && row.paramCount <= HK_PACK_VOICE_RECIPE_MAX_PARAMS &&
           row.paramBytes == row.paramCount * sizeof(float);
}

inline bool ReadPackVoiceRecipeInstall(const std::uint8_t* in, PackVoiceRecipeInstall* out) noexcept {
    PackVoiceRecipeInstallWire row{};
    if (!ParsePackRow<HkPackVoiceRecipeInstall>(in, kPackBytes<HkPackVoiceRecipeInstall>, &row) ||
        !PackVoiceRecipeInstallWireValid(row)) return false;
    *out = row;
    return true;
}

template <class Container>
inline bool ReadPackContainerPayload(const std::uint8_t* in, std::uint64_t size,
                                      harken::studio::types::ObjectId expectedId, Container& container) {
    if (!ReadPackContainerHeader(in, size, &container.header) ||
        container.header.containerId == harken::studio::types::ObjectId{} || container.header.containerId != expectedId) return false;
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
                                       harken::studio::types::ObjectId expectedId, Recipe& recipe) {
    if (!ReadPackVoiceRecipeHeader(in, size, &recipe.header) ||
        recipe.header.playableId == harken::studio::types::ObjectId{} || recipe.header.playableId != expectedId) return false;
    recipe.installs.resize(recipe.header.installCount);
    recipe.bindings.resize(recipe.header.bindingCount);
    struct InstallVisitor {
        PackVoiceRecipeInstall& target;
        PackVoiceRecipeInstallWire row{};
        auto& Wire() noexcept { return row; }
        bool Finish() noexcept {
            if (!PackVoiceRecipeInstallWireValid(row)) return false;
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
                !PackVoiceRecipePlaybackValid(row.playback)) return false;
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
    (void)ParsePackRow<HkPackMedia>(in, kPackBytes<HkPackMedia>, &row);
    return row;
}

inline std::uint64_t PackTocByteOffset(std::uint32_t index) noexcept {
    return static_cast<std::uint64_t>(index) * kPackBytes<HkPackMedia>;
}

inline bool ReadPackBusEffect(const std::uint8_t* in, std::uint64_t size,
                              PackBusEffectEntry* out) noexcept {
    return ParsePackRow<HkPackBusEffect>(in, size, out) &&
           out->paramCount <= HK_PACK_VOICE_RECIPE_MAX_PARAMS &&
           out->paramBytes == out->paramCount * sizeof(float);
}

inline bool ReadPackAttenuation(const std::uint8_t* in, std::uint64_t size,
                                AttenuationDefinition* out) noexcept {
    *out = {};
    return ParsePackRow<HkPackAttenuation>(in, size, out);
}

inline void WritePackBindingDefinition(harken::studio::types::ObjectId bindingId, const BindingDefinition& definition,
                                       std::uint8_t* out) noexcept {
    SerializePackRow<HkPackBindingDefinition>(PackBindingDefinitionEntry{bindingId, definition}, out);
}

inline bool ReadPackBindingDefinition(const std::uint8_t* in, std::uint64_t size,
                                      harken::studio::types::ObjectId* outId, BindingDefinition* out) noexcept {
    PackBindingDefinitionEntry row{};
    if (!ParsePackRow<HkPackBindingDefinition>(in, size, &row)) return false;
    *outId = row.bindingId;
    *out = row.definition;
    return true;
}

inline bool PackIsCanonicalEntryKind(std::uint32_t entryKind) {
    return entryKind == HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION ||
           entryKind == HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK ||
           entryKind == HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE ||
           entryKind == HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT ||
           entryKind == HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY ||
           entryKind == HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS ||
           entryKind == HK_PACK_ENTRY_KIND_EFFECT_PRESET;
}
inline bool ReadPackBusDefinitionSnapshotHeader(
    const std::uint8_t* in, std::uint64_t size,
    PackBusDefinitionSnapshotHeader* out) noexcept {
    if (size < kPackBytes<HkPackBusDefinitionSnapshotHeader> ||
        !ParsePackRow<HkPackBusDefinitionSnapshotHeader>(in, kPackBytes<HkPackBusDefinitionSnapshotHeader>, out)) {
        return false;
    }
    std::uint64_t expectedSize = 0;
    if (out->schemaVersion != HK_PACK_BUS_DEFINITION_SNAPSHOT_VINTAGE ||
        out->storageModelVersion != HK_PACK_BUS_DEFINITION_SNAPSHOT_VINTAGE ||
        out->headerBytes != kPackBytes<HkPackBusDefinitionSnapshotHeader> ||
        (out->publicationCapabilityVintage != 3u && out->publicationCapabilityVintage != 4u &&
         out->publicationCapabilityVintage != 5u) ||
        out->profileVintage != 1 || out->reserved0 != 0 || out->reserved1 != 0 ||
        out->busRowBytes != kPackBytes<HkPackBusDefinitionBus> ||
        out->sendRowBytes != kPackBytes<HkPackBusDefinitionSend> ||
        out->downmixRowBytes != kPackBytes<HkPackBusDefinitionDownmix> ||
        out->coefficientRowBytes != kPackBytes<HkPackBusDefinitionCoefficient> ||
        !CheckedAddU64(out->headerBytes, out->payloadBytes, &expectedSize) ||
        size != expectedSize) {
        return false;
    }
    constexpr auto checksumOffset = offsetof(HkPackBusDefinitionSnapshotHeader, checksum);
    constexpr auto checksumEnd = offsetof(HkPackBusDefinitionSnapshotHeader, compileTargetProfileFingerprint);
    const std::uint8_t checksumZero[checksumEnd - checksumOffset] = {};
    std::uint64_t hash = Fnv1a64(in, checksumOffset);
    hash = Fnv1a64(checksumZero, sizeof(checksumZero), hash);
    hash = Fnv1a64(in + checksumEnd, static_cast<std::size_t>(size - checksumEnd), hash);
    return hash == out->checksum;
}

inline bool ReadPackMediaFileHeader(const std::uint8_t* in, PackMediaFileHeader* out) noexcept {
    return ParsePackRow<HkPackMediaFileHeader>(in, kPackBytes<HkPackMediaFileHeader>, out);
}

inline void SerializeStreamHeader(const StreamHeader& header, std::uint8_t* out) noexcept {
    SerializePackRow<HkPackStreamFileHeader>(header, out);
}

inline bool ParseStreamHeader(const std::uint8_t* in, StreamHeader* out) noexcept {
    return ParsePackRow<HkPackStreamFileHeader>(in, kPackBytes<HkPackStreamFileHeader>, out);
}

inline void SerializeStreamTocEntry(const StreamTocEntry& entry, std::uint8_t* out) noexcept {
    SerializePackRow<HkPackStreamFileTocEntry>(entry, out);
}

inline StreamTocEntry ParseStreamTocEntry(const std::uint8_t* in) noexcept {
    StreamTocEntry entry;
    (void)ParsePackRow<HkPackStreamFileTocEntry>(in, kPackBytes<HkPackStreamFileTocEntry>, &entry);
    return entry;
}

inline bool ReadPackFileHeader(const std::uint8_t* in, PackFileHeader* out) noexcept {
    return ParsePackRow<HkPackFileHeader>(in, kPackBytes<HkPackFileHeader>, out);
}

inline PackDirectoryEntry ReadPackDirectoryEntry(const std::uint8_t* in) noexcept {
    PackDirectoryEntry row;
    (void)ParsePackRow<HkPackDirectoryEntry>(in, kPackBytes<HkPackDirectoryEntry>, &row);
    return row;
}

}

#endif // HARKEN_STUDIO_BUILD_PACK_FORMAT_V2_H
