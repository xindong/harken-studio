// Compile-time folding of an occurrence's gain and pitch contributions.
//
// The contribution-folding shape is the frozen one: an occurrence's base gain
// and pitch are the sum of every contributing object's authored values along a
// fixed chain, resolved once at compile time into the values a later stage
// installs when the voice starts. There is no per-node summation node and no
// runtime chain walk; the numbers are pre-composed here.
//
// The gain contract specifies the mathematical sum, not a cross-implementation
// guarantee about intermediate rounding or parenthesization.
// A Bus is not a content object in this model and carries no gain or pitch, so —
// unlike the legacy pitch fold, which also summed the bus chain — only the
// content chain is folded here.
#ifndef HARKEN_PROJECT_SCHEMA_PROPERTY_FOLD_H
#define HARKEN_PROJECT_SCHEMA_PROPERTY_FOLD_H

#include "harken_project_schema/Model.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The clamp applied to the summed cent offset before it becomes a ratio, in
// cents each way. It matches the legacy runtime-link bound.
inline constexpr float kPitchCentClamp = 7200.0f;

// One occurrence's folded base values, the installable result of the fold.
struct FoldedProperties {
    float baseVolumeDb = 0.0f;    // dB sum along the fold chain
    float outputBusVolumeDb = 0.0f;
    float basePitchRatio = 1.0f;  // 2^(clamp(cent sum) / 1200)
    std::uint32_t loopCount = 1;  // the subject's own value, not inherited
};

// The raw components of an occurrence's contribution chain, before any consumer
// imposes an iteration order. Only the membership and each component's internal
// ordering are shared here; how a consumer concatenates and deduplicates the
// components is the consumer's own declared policy, because the two consumers
// need different orders:
//   - the gain/pitch fold concatenates placement before organizational and sums,
//     an order in which float addition is not associative, so it is fixed;
//   - send resolution concatenates organizational before placement and keeps the
//     first send seen per target bus, so an object's own design chain outranks
//     the arrangement lane it is placed on.
// Sharing the components — not a pre-ordered list — is what keeps both consumers
// walking the same chain members while each stays free to order them.
struct FoldChainComponents {
    StableId subject = kUnmintedStableId;
    std::vector<StableId> placementChain;       // [Track, owning Segment], nearest first
    std::vector<StableId> organizationalChain;  // structural owners, nearest first
};

// A reusable fold context built once from a canonical project: each object's gain
// and pitch contribution and loop count, and the parent map the organizational
// walk needs (the downward ownership edges, inverted).
class PropertyFoldContext {
public:
    explicit PropertyFoldContext(const CanonicalProject& project);

    // Folds the properties for `subject` played through the placement of
    // `trackId` (kUnmintedStableId means a direct trigger with no placement
    // chain). The organizational chain is derived from the subject's owners.
    FoldedProperties Fold(StableId subject, StableId trackId,
                          const PlayablePropertyPayloadV1* occurrence = nullptr) const;

    // The subject's contribution-chain components for a placement of `trackId`
    // (kUnmintedStableId = a direct trigger, empty placement chain). This is the
    // shared membership other stages (send resolution) walk in their own order;
    // Fold consumes it too, so the fold and those stages provably see the same
    // chain members.
    FoldChainComponents ChainComponents(StableId subject, StableId trackId) const;

private:
    struct Contribution {
        float gainDb = 0.0f;
        std::optional<float> outputBusVolumeDb;
        float pitchCents = 0.0f;  // pitch
        std::uint32_t loopCount = 1;
        ContentKind kind = ContentKind::Sound;
    };

    const Contribution* Find(StableId id) const;

    std::map<StableId, Contribution> objects_;
    std::map<StableId, StableId> parent_;  // child -> its structural owner
};

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_PROPERTY_FOLD_H
