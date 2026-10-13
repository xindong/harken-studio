#ifndef HARKEN_PROJECT_SCHEMA_EFFECT_CHAIN_SELECTION_H
#define HARKEN_PROJECT_SCHEMA_EFFECT_CHAIN_SELECTION_H

#include "harken_project_schema/Diagnostics.h"
#include "harken_project_schema/EffectChain.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/NormalizedFacts.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The chain selected for one clip occurrence. When declared is false no object on
// the chain declared a graph: the occurrence runs no effects and references no
// chain. When declared is true, owner names the winning object and chainIndex
// addresses its stored chain, which may legitimately hold zero entries.
//
struct OccurrenceEffectChainSelection {
    StableId trackId = kUnmintedStableId;
    StableId clipId = kUnmintedStableId;
    std::uint32_t occurrenceOrdinal = 0;
    StableId subjectId = kUnmintedStableId;
    bool declared = false;
    StableId owner = kUnmintedStableId;
    std::uint32_t chainIndex = 0;
    std::vector<StableId> shadowedOwners;
};

// The effect-chain facts of a compiled project. chains holds one entry per
// distinct selected owner, ascending by owner id; occurrences is one entry per
// clip occurrence, ordered by (trackId, occurrenceOrdinal). Both orders are total
// and independent of the order the model was built in.
struct EffectChainSelectionFacts {
    std::vector<CompiledEffectChain> chains;
    std::vector<OccurrenceEffectChainSelection> occurrences;
};

// The result of selecting a project's effect chains. On success facts holds the
// chain table and the per-occurrence selections and diagnostics is empty. A
// selected graph that is not a straight path is a deterministic refusal:
// diagnostics carries the failure and facts is absent, all-or-nothing like the
// front-end.
struct EffectChainSelectionResult {
    std::vector<Diagnostic> diagnostics;
    std::optional<EffectChainSelectionFacts> facts;

    bool ok() const { return diagnostics.empty(); }
};

// Selects and linearizes every clip occurrence's effect chain. The project must be
// one the front-end has already validated, so every authored graph is a legal DAG
// with unique node ids and resolving edges.
EffectChainSelectionResult BuildEffectChainSelection(
    const CanonicalProject& project, const NormalizedFacts& facts);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_EFFECT_CHAIN_SELECTION_H
