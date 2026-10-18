// The compile front-end validates the first four stages of the Project v6
// compiler, then turns a lenient authored input into either a canonical model
// with its normalization, reference-resolution and send-resolution facts or a
// set of refusals.
//
//   stage 1  parse            - per-kind field legality; a field authored where
//                               its kind forbids it is refused here.
//   stage 2  stable identity  - mint each object's runtime id through the
//                               registry; duplicate GUIDs and duplicate
//                               persisted ids are refused here.
//   stage 3  graph validation - build the structural ownership graph and the
//                               placement graph; illegal or dangling edges,
//                               multiple owners, and ownership cycles are
//                               refused here.
//   stage 4  time and marker     - the frozen time rules a Musical Piece must obey
//            normalization      (exactly one tempo and one meter row, each at
//                               tick 0) and the single-exit-marker rule are
//                               enforced here, and the deterministic order and
//                               start coordinate facts are derived.
//
// Reference-slot and send resolution run after the validated model and
// normalization facts exist, using their respective deterministic rules.
// Contribution folding, decision-frame facts, IR and program emission remain
// later work. Clip slip/crop/repeat, quantize and snap-grid rules are likewise
// outside this front-end.
//
// The output contract is all-or-nothing: FrontEndResult carries a canonical
// model and all three fact products only when diagnostics is empty. There is no
// partial project: a single refusal means all four are absent, so a caller
// cannot accidentally consume a half-validated tree.
//
// Determinism is a contract, not an accident: the same input compiled twice
// yields byte-identical diagnostics in the same order, a byte-identical model,
// and byte-identical normalization, reference-resolution and send-resolution
// facts. The implementation earns this by processing nodes in authored order,
// ordering every derived list by a total key, and never letting an unordered
// container's iteration reach the output.
#ifndef HARKEN_PROJECT_SCHEMA_FRONT_END_H
#define HARKEN_PROJECT_SCHEMA_FRONT_END_H

#include "harken_project_schema/Diagnostics.h"
#include "harken_project_schema/EffectChainSelection.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/NormalizedFacts.h"
#include "harken_project_schema/ProjectInput.h"
#include "harken_project_schema/ReferenceResolution.h"
#include "harken_project_schema/SendResolution.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The result of a front-end compile. When diagnostics is empty, project holds
// the canonical model, facts holds its normalization facts, resolution holds its
// reference-resolution facts, sends holds its send-resolution facts, and
// effectChains holds its effect-chain selection facts; the five are present
// together or not at all. When diagnostics is non-empty, all five are absent and
// the build has failed.
struct FrontEndResult {
    std::vector<Diagnostic> diagnostics;
    std::optional<CanonicalProject> project;
    std::optional<NormalizedFacts> facts;
    std::optional<ReferenceResolutionFacts> resolution;
    std::optional<SendResolutionFacts> sends;
    std::optional<EffectChainSelectionFacts> effectChains;

    bool ok() const { return diagnostics.empty(); }
};

// Compiles an authored input through the four front-end stages. The high-water
// mark for minting is taken from input.mintHighWaterMark. The result is
// deterministic in the input alone.
FrontEndResult CompileFrontEnd(const ProjectInput& input);

// The explicit-ceiling overload exposes the resolution refusal path to tests;
// production callers use the deterministic one-argument form above.
FrontEndResult CompileFrontEnd(const ProjectInput& input,
                               std::uint32_t resolutionDepthCeiling);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_FRONT_END_H
