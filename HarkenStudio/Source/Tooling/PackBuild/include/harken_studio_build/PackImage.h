#ifndef HARKEN_STUDIO_BUILD_PACK_LOGIC_IMAGE_H
#define HARKEN_STUDIO_BUILD_PACK_LOGIC_IMAGE_H

#include <harken_studio/ObjectId.h>
#include "harken_studio_build/VariableLabel.h"
#include <harken_studio_build/AuthoredParameters.h>
#include <harken_studio/AudioValues.h>
#include "BusFingerprint.h"

#include "harken_studio_build/PackFormat.h"
#include "harken_studio_build/PackFormatV2.h"

#include <cstdint>
#include "harken_studio_build/PackVariableNameCatalog.h"
#include <span>
#include <vector>

namespace harkenstudiobuild {
struct PackLogicImage {
    std::uint32_t packKind = kPackKindContent;

    std::vector<PackTocEntry> media;
    std::vector<PackBusEntry> buses;
    std::vector<PackBusSendEntry> busSends;
    std::vector<PackBusEffectEntry> busEffects;
    std::vector<PackEffectDynamicRouteEntry> dynamicRoutes;

    struct Attenuation {
        harkenstudio::ObjectId id{};
        harkenstudiobuild::AttenuationDefinition desc{};
    };
    std::vector<Attenuation> attenuations;

    struct BindingDefinition {
        harkenstudio::ObjectId id{};
        harkenstudiobuild::BindingDefinition desc{};
    };
    std::vector<BindingDefinition> bindingDefinitions;

    std::vector<PackBindingAttachmentEntry> bindingAttachments;

    struct Container {
        PackContainerEntryHeader header{};
        std::vector<PackContainerChildV2> children;
    };
    std::vector<Container> containers;

    struct VoiceRecipe {
        PackVoiceRecipeEntryHeader header{};
        PackVoiceRecipeTone tone{};
        PackVoiceRecipeSpatial spatial{};
        PackVoiceRecipePlayback playback{};
        std::vector<PackVoiceRecipeInstall> installs;
        std::vector<harkenstudio::ObjectId> bindings;
    };
    std::vector<VoiceRecipe> voiceRecipes;
    struct Stamp {
        std::uint32_t kind = 0;
        harkenstudio::ObjectId id{};
        std::uint64_t checksum = 0;
    };
    std::vector<Stamp> stamps;

    std::uint32_t EntryCount() const {
        return static_cast<std::uint32_t>(
            media.size() + buses.size() + busSends.size() +
            busEffects.size() + dynamicRoutes.size() +
            attenuations.size() + bindingDefinitions.size() +
            bindingAttachments.size() + containers.size() +
            voiceRecipes.size());
    }
};

enum class PackCatalogIdentityState : std::uint32_t {
    Absent = 0,
    KnownUnloaded = 1,
};

struct GameplayCatalogImage {
    PackGameplayCatalogHeader header{};
    std::vector<PackGameplayCatalogActiveRow> active;
    std::vector<PackGameplayCatalogRetiredRow> retired;
    std::vector<std::uint8_t> namePool;
};

struct PackCanonicalImage {
    std::uint32_t packKind = 0;
    std::uint64_t packStableId = 0;
    PackRequirementBlock requirement{};
    bool hasRequirement = false;
    bool hasCatalog = false;
    bool hasGameplayCatalog = false;
    GameplayCatalogImage gameplayCatalog;
    bool hasVariableNameCatalog = false;
    PackVariableNameCatalog variableNameCatalog;
    bool hasPartition = false;
    std::uint64_t catalogPayloadOffset = 0;
    std::uint64_t catalogDirectoryChecksum = 0;
    PackInitCatalogHeader catalogHeader{};
    std::vector<PackInitCatalogRow> catalogRows;
    std::vector<std::uint64_t> startupPackIds;
    PackCanonicalPartitionHeader partitionHeader{};
    std::vector<std::uint8_t> partitionBody;
    std::vector<PackDependencyEdge> dependencyEdges;
    struct VariablePolicyOverlay {
        harkenstudio::ObjectId variableId{};
        std::uint8_t writers = 0;
        std::uint64_t executionFingerprint = 0;
        std::vector<std::int32_t> discreteAnchors;
        std::uint64_t labelMappingFingerprint = 0;
        std::uint64_t catalogFingerprint = 0;
        std::vector<VariableLabelDefinitionV1> labels;
    };
    std::vector<VariablePolicyOverlay> variablePolicyOverlays;
    bool hasVariablePolicyOverlay = false;
    struct VariableRequirement {
        std::uint8_t identityClass = 0;
        harkenstudio::ObjectId stableId{};
        std::uint64_t expectedExecutionFingerprint = 0;
        std::uint64_t expectedLabelMappingFingerprint = 0;
    };
    std::vector<VariableRequirement> variableRequirements;
    bool hasVariableRequirements = false;
    std::vector<PackTocEntry> media;
    std::vector<PackBusEntry> buses;
    std::vector<PackBusSendEntry> busSends;
    std::vector<PackBusEffectEntry> busEffects;
    std::vector<PackEffectDynamicRouteEntry> dynamicRoutes;
    std::vector<PackLogicImage::Attenuation> attenuations;
    std::vector<PackLogicImage::BindingDefinition> bindingDefinitions;
    std::vector<PackBindingAttachmentEntry> bindingAttachments;
    bool hasBusDefinitionSnapshot = false;
    PackBusDefinitionSnapshotHeaderV1 busDefinitionSnapshotHeader{};
    std::vector<PackBusDefinitionBusV1> busDefinitionBuses;
    std::vector<PackBusDefinitionSendV1> busDefinitionSends;
    std::vector<PackBusDefinitionEffectV1> busDefinitionEffects;
    std::vector<PackBusDefinitionEffectParameterV1> busDefinitionEffectParameters;
    std::vector<PackBusDefinitionDownmixV1> busDefinitionDownmixes;
    std::vector<PackBusDefinitionBusV2> busDefinitionBusesV2;
    std::vector<PackBusDefinitionSendV2> busDefinitionSendsV2;
    std::vector<PackBusDefinitionDownmixV2> busDefinitionDownmixesV2;
    std::vector<float> busDefinitionCoefficients;
};

inline PackCatalogIdentityState LookupInitCatalogIdentity(
    const PackCanonicalImage& image, harkenstudio::ObjectId stableId,
    const PackInitCatalogRow** outRow = nullptr) {
    if (outRow != nullptr) {
        *outRow = nullptr;
    }
    std::size_t lo = 0;
    std::size_t hi = image.catalogRows.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const harkenstudio::ObjectId midId = image.catalogRows[mid].stableId;
        if (midId == stableId) {
            if (outRow != nullptr) {
                *outRow = &image.catalogRows[mid];
            }
            return PackCatalogIdentityState::KnownUnloaded;
        }
        if (midId < stableId) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return PackCatalogIdentityState::Absent;
}
harkenstudiobuild::PackResult ParsePackV2Image(const std::uint8_t* bytes, std::uint64_t fileBytes,
                          PackLogicImage* out);
harkenstudiobuild::PackResult ValidateCanonicalInitDependency(
    std::span<const PackDependencyEdge> edges,
    std::uint64_t expectedInitPackStableId) noexcept;

inline constexpr std::uint64_t kNeutralMasterBusContentGeneration = 1;
inline constexpr std::uint64_t kNeutralMasterPublishedBusFingerprint =
    0xB15C0001ull;
inline constexpr std::uint64_t kNeutralMasterCompileTargetProfileFingerprint =
    0xB15C0002ull;

struct CanonicalBusTriple {
    std::uint64_t busContentGeneration = 0;
    std::uint64_t publishedBusFingerprint = 0;
    std::uint64_t compileTargetProfileFingerprint = 0;
};

inline CanonicalBusTriple NeutralMasterBusTriple() noexcept {
    return {kNeutralMasterBusContentGeneration,
            kNeutralMasterPublishedBusFingerprint,
            kNeutralMasterCompileTargetProfileFingerprint};
}

inline bool CanonicalBusTripleFullyNonzero(const CanonicalBusTriple& triple) noexcept {
    return triple.busContentGeneration != 0 &&
           triple.publishedBusFingerprint != 0 &&
           triple.compileTargetProfileFingerprint != 0;
}

inline bool CanonicalBusTripleEqual(const CanonicalBusTriple& left,
                                    const CanonicalBusTriple& right) noexcept {
    return left.busContentGeneration == right.busContentGeneration &&
           left.publishedBusFingerprint == right.publishedBusFingerprint &&
           left.compileTargetProfileFingerprint ==
               right.compileTargetProfileFingerprint;
}

inline CanonicalBusTriple RequirementBusTriple(const PackRequirementBlock& block) noexcept {
    return {block.busContentGeneration, block.publishedBusFingerprint,
            block.compileTargetProfileFingerprint};
}

inline CanonicalBusTriple PartitionBusTriple(
    const PackCanonicalPartitionHeader& header) noexcept {
    return {header.busContentGeneration, header.publishedBusFingerprint,
            header.compileTargetProfileFingerprint};
}

inline CanonicalBusTriple SnapshotBusTriple(
    const PackBusDefinitionSnapshotHeaderV1& header) noexcept {
    return {header.contentGeneration, header.publishedBusFingerprint,
            header.compileTargetProfileFingerprint};
}

inline CanonicalBusTriple DependencyBusTriple(const PackDependencyEdge& edge) noexcept {
    return {edge.expectedBusContentGeneration, edge.expectedPublishedBusFingerprint,
            edge.expectedCompileTargetProfileFingerprint};
}

inline std::uint64_t CanonicalNeutralBusFingerprint(const PackCanonicalImage& image) noexcept {
    const auto& header = image.busDefinitionSnapshotHeader;
    std::uint64_t hash = busfingerprint::kOffsetBasis;
    const auto byte = [&](std::uint8_t value) { hash = busfingerprint::Step(hash, value); };
    const auto u32 = [&](std::uint32_t value) { busfingerprint::AppendU32(byte, value); };
    const auto u64 = [&](std::uint64_t value) { busfingerprint::AppendU64(byte, value); };
    const auto f32 = [&](float value) {
        u32(busfingerprint::CanonicalFloatBits(value));
    };
    constexpr char tag[] = "harken.bus.published.v1";
    u32(static_cast<std::uint32_t>(sizeof(tag) - 1));
    for (std::size_t i = 0; i + 1 < sizeof(tag); ++i) {
        byte(static_cast<std::uint8_t>(tag[i]));
    }
    u32(1);
    u32(header.publicationCapabilityVintage);
    u32(header.profileVintage);
    u32(header.renderSampleRateHz);
    u32(header.mixLayout);
    u32(header.deviceLayout);
    u64(header.runtimeCapabilityMask);
    u64(header.compileTargetProfileFingerprint);
    u32(header.busCount);
    u32(header.sendCount);
    u32(0);
    u32(0);
    u32(header.downmixCount);
    u32(header.coefficientCount);
    for (const auto& row : image.busDefinitionBuses) {
        u64(row.busId);
        u64(row.parentId);
        u32(row.feedsFirstOrdinal);
        f32(row.faderDb);
        u32(row.faderAfterSlot);
        u32(row.outputLayout);
        u32(row.effectFirst);
        u32(row.effectCount);
        u32(row.downmixFirst);
        u32(row.downmixCount);
    }
    for (const auto& row : image.busDefinitionSends) {
        u64(row.source);
        u64(row.target);
        f32(row.levelDb);
        u32(0);
    }
    for (const auto& row : image.busDefinitionDownmixes) {
        u64(row.busId);
        u32(row.sourceLayout);
        u32(row.destinationLayout);
        u32(row.coefficientFirst);
        u32(row.coefficientCount);
    }
    for (float value : image.busDefinitionCoefficients) {
        f32(value);
    }
    return busfingerprint::Finalize(hash);
}

inline std::uint64_t CanonicalVolumeBusFingerprint(const PackCanonicalImage& image) noexcept {
    const auto& header = image.busDefinitionSnapshotHeader;
    std::uint64_t hash = busfingerprint::kOffsetBasis;
    const auto byte = [&](std::uint8_t value) { hash = busfingerprint::Step(hash, value); };
    const auto u32 = [&](std::uint32_t value) { busfingerprint::AppendU32(byte, value); };
    const auto u64 = [&](std::uint64_t value) { busfingerprint::AppendU64(byte, value); };
    const auto f32 = [&](float value) { u32(busfingerprint::CanonicalFloatBits(value)); };
    const auto id = [&](harkenstudio::ObjectId value) {
        std::uint8_t bytes[16];
        StoreObjectId(bytes, value);
        for (std::uint8_t valueByte : bytes) byte(valueByte);
    };
    constexpr char tag[] = "harken.bus.published.v2";
    u32(static_cast<std::uint32_t>(sizeof(tag) - 1));
    for (std::size_t i = 0; i + 1 < sizeof(tag); ++i) byte(static_cast<std::uint8_t>(tag[i]));
    u32(2);
    u32(header.publicationCapabilityVintage);
    u32(header.profileVintage);
    u32(header.renderSampleRateHz);
    u32(header.mixLayout);
    u32(header.deviceLayout);
    u64(header.runtimeCapabilityMask);
    u64(header.compileTargetProfileFingerprint);
    u32(header.busCount);
    u32(header.sendCount);
    u32(header.effectCount);
    u32(header.effectParameterCount);
    u32(header.downmixCount);
    u32(header.coefficientCount);
    for (const auto& row : image.busDefinitionBusesV2) {
        id(row.busId);
        id(row.parentId);
        u32(row.feedsFirstOrdinal);
        f32(row.faderDb);
        u32(row.faderAfterSlot);
        u32(row.outputLayout);
        u32(row.effectFirst);
        u32(row.effectCount);
        u32(row.downmixFirst);
        u32(row.downmixCount);
        f32(row.makeUpGainDb);
        f32(row.outputBusVolumeDb);
    }
    for (const auto& row : image.busDefinitionSendsV2) {
        id(row.source);
        id(row.target);
        f32(row.levelDb);
        u32(0);
    }
    for (const auto& row : image.busDefinitionDownmixesV2) {
        id(row.busId);
        u32(row.sourceLayout);
        u32(row.destinationLayout);
        u32(row.coefficientFirst);
        u32(row.coefficientCount);
    }
    for (float value : image.busDefinitionCoefficients) f32(value);
    return busfingerprint::Finalize(hash);
}
harkenstudiobuild::PackResult ParsePackCanonicalImage(const std::uint8_t* bytes,
                                 std::uint64_t fileBytes,
                                 PackCanonicalImage* out);
struct PackClosureResolver {
    void* ctx = nullptr;
    bool (*playableExists)(void* ctx, harkenstudio::ObjectId id) = nullptr;
    bool (*busExists)(void* ctx, harkenstudio::ObjectId id) = nullptr;
    bool (*bindingExists)(void* ctx, harkenstudio::ObjectId id) = nullptr;
};
enum class PackClosureRef : std::uint32_t {
    SendBus = 1,          // a send edge's from/to bus
    MeterBus = 2,         // a meter wiring's bus
    AttachmentBinding = 3, // an attachment's binding id
    AttachmentBus = 4,     // a BUS attachment's object
    AttachmentPlayable = 5, // a VOICE attachment's object
    ContainerChild = 6,    // a container child playable
    BusParent = 10,        // a bus entry's parent bus
    RecipePlayable = 11,   // a Voice Recipe's playable id
    RecipeBinding = 12,    // a Voice Recipe's binding id
    BusEffectTarget = 13,  // a Bus Effect's target bus
    DynamicRouteTarget = 14, // a route's target bus
    DynamicRouteSource = 15  // a route's source bus
};
harkenstudiobuild::PackResult CheckPackClosure(const PackLogicImage& image,
                          const PackClosureResolver& resolver,
                          PackClosureRef* outRefClass,
                          harkenstudio::ObjectId* outReferrerId,
                          harkenstudio::ObjectId* outMissingId);

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_PACK_LOGIC_IMAGE_H
