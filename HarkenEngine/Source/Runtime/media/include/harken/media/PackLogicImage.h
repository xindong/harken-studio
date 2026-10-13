// Pack logic image: the validated in-memory form of a v2 .pack file's
// entry set, and the reference-closure checker over it.
//
// ParsePackV2Image is the v2 counterpart of the v1 ReadAndValidate
// front half: it proves every container invariant — header sanity,
// directory checksum, per-entry payload checksums, schema vintages,
// wire-level parse rules, media blob integrity (the v1 checks,
// replicated so the v1 parser stays untouched line for line), pack-kind
// entry discipline, per-kind id uniqueness, and intra-pack relation
// uniqueness — before the caller touches any registry or table state.
// Illegal directory vintage, and canonical pack/entry kinds on this
// path, refuse with VN_PACK_INCOMPATIBLE. Every other failure refuses
// the whole image with VN_BAD_PACK.
//
// CheckPackClosure then proves the section-19.4 closure rule: every
// logic reference the pack makes must resolve against {the pack itself +
// already-loaded state}, the latter reached through caller-supplied
// resolver callbacks so this module stays free of engine dependencies
// (and unit tests can drive it synthetically). Closure is deliberately
// STRICTER than live registration where the two differ. That is pack-only
// content discipline, not a claim of validator equivalence.
//
// Logic thread only, like every other pack surface.
#ifndef HARKEN_MEDIA_PACK_LOGIC_IMAGE_H
#define HARKEN_MEDIA_PACK_LOGIC_IMAGE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>
#include <harken/BusDefinitionFingerprintValue.h>
#include "harken/core/VariableLabel.h"

#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"

#include <cstdint>
#include "harken/media/PackVariableNameCatalog.h"
#include <span>
#include <vector>

namespace harken {

// One parsed v2 pack, typed by entry kind. Media entries keep the v1
// record form (their wire format is the v1 TOC entry, untouched);
// container children live in per-entry vectors, and the
// registration-facing descriptor structs are assembled by the
// consumer, which owns wiring the array pointers.
struct PackLogicImage {
    std::uint32_t packKind = kPackKindContent;

    std::vector<PackTocEntry> media;
    std::vector<PackBusEntry> buses;
    std::vector<PackBusSendEntry> busSends;
    std::vector<PackBusEffectEntry> busEffects;
    std::vector<PackEffectDynamicRouteEntry> dynamicRoutes;

    struct Attenuation {
        VnAttenuationId id{};
        VnAttenuationDesc desc{};
    };
    std::vector<Attenuation> attenuations;

    struct BindingDefinition {
        VnBindingId id{};
        VnBindingDesc desc{};
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
        std::vector<VnBindingId> bindings;
    };
    std::vector<VoiceRecipe> voiceRecipes;

    // One (kind, id, payloadChecksum) triple per directory entry, in
    // directory order. Replace staging diffs revisions with them: a
    // same-id entry whose payload checksum is unchanged is a no-op, so
    // an idempotent replace touches nothing. Relation kinds carry
    // ordinal ids and are diffed by their payload identity instead.
    struct Stamp {
        std::uint32_t kind = 0;
        VnObjectId id{};
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
        VnObjectId variableId{};
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
        VnObjectId stableId{};
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
    const PackCanonicalImage& image, VnObjectId stableId,
    const PackInitCatalogRow** outRow = nullptr) {
    if (outRow != nullptr) {
        *outRow = nullptr;
    }
    std::size_t lo = 0;
    std::size_t hi = image.catalogRows.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const VnObjectId midId = image.catalogRows[mid].stableId;
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

inline std::uint64_t CanonicalVolumeBusFingerprint(const PackCanonicalImage& image) noexcept {
    const auto& header = image.busDefinitionSnapshotHeader;
    std::uint64_t hash = busfingerprint::kOffsetBasis;
    const auto byte = [&](std::uint8_t value) { hash = busfingerprint::Step(hash, value); };
    const auto u32 = [&](std::uint32_t value) { busfingerprint::AppendU32(byte, value); };
    const auto u64 = [&](std::uint64_t value) { busfingerprint::AppendU64(byte, value); };
    const auto f32 = [&](float value) { u32(busfingerprint::CanonicalFloatBits(value)); };
    const auto id = [&](VnObjectId value) {
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
// Parses and validates a whole v2 file image (bytes[0..fileBytes)).
// VN_PACK_INCOMPATIBLE for an illegal directory vintage (layout id the
// current reader does not accept) and for canonical pack/entry kinds on
// this path. VN_BAD_PACK for every structural failure: header/directory/
// payload checksum or bounds violations, unknown kinds, wire parse
// refusals (size mismatches, nonzero reserved bytes), media entries
// failing the v1 container invariants, entry kinds illegal for the pack
// kind (a content pack carrying a resident-only kind, a resident pack
// carrying media/containers or a non-BUS-scope binding surface),
// directory ids disagreeing with payload ids, duplicate ids within a
// kind, or duplicate relations (send edges, attachments). The caller
// passes the exact file size; a header whose fileBytes disagrees
// refuses. Touches no engine state.
VnResult ParsePackV2Image(const std::uint8_t* bytes, std::uint64_t fileBytes,
                          PackLogicImage* out);

// Checks the complete decoded edge list before optional-edge filtering. The
// expected Init identity must come from the export plan or validated Init,
// not from the edge under validation. Does not resolve loaded providers.
VnResult ValidateCanonicalInitDependency(
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

// The v2 snapshot parent check accepts any Bus row order and bounds a full
// 4096-row chain without recursive traversal.
bool ValidateCanonicalBusParentForestV2(
    std::span<const PackBusDefinitionBusV2> buses);

// Shipping decode of Init / CanonicalContent files (LoadCanonicalPack).
// The old LoadPackV2 / ParsePackV2Image path refuses those artifacts
// with VN_PACK_INCOMPATIBLE before this runs. Illegal directory vintage,
// a program-chapter bodyVintage that is not the current write table,
// and v2 Container / VoiceRecipe entries are VN_PACK_INCOMPATIBLE too.
VnResult ParsePackCanonicalImage(const std::uint8_t* bytes,
                                 std::uint64_t fileBytes,
                                 PackCanonicalImage* out);

// Already-loaded-state view for the closure check. Every callback
// answers "does this id resolve outside the pack right now": playables
// cover the whole flat playable-ID space (PCM, streams, containers),
// buses the live bus table, and bindings the definition registry. Null
// function pointers answer "no".
struct PackClosureResolver {
    void* ctx = nullptr;
    bool (*playableExists)(void* ctx, VnObjectId id) = nullptr;
    bool (*busExists)(void* ctx, VnBusId id) = nullptr;
    bool (*bindingExists)(void* ctx, VnBindingId id) = nullptr;
};

// The reference classes a closure failure is reported against, for the
// refusal log record.
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

// Verifies the closure rule. VN_OK when every reference resolves against
// {the image itself + the resolver}; VN_UNKNOWN_ID otherwise, with the
// failing reference class, the referring entry's id, and the missing id
// written to the out parameters (any may be null) so the loader's log
// record can name them. Touches no engine state.
VnResult CheckPackClosure(const PackLogicImage& image,
                          const PackClosureResolver& resolver,
                          PackClosureRef* outRefClass,
                          VnObjectId* outReferrerId,
                          VnObjectId* outMissingId);

} // namespace harken

#endif // HARKEN_MEDIA_PACK_LOGIC_IMAGE_H
