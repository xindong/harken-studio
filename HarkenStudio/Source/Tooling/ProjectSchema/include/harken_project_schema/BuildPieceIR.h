// Building the immutable PieceIR from a compiled model and its normalization
// facts.
//
// This is the stage after the front-end: the front-end proves a project
// well-formed and derives its normalization facts; this turns the two into the
// IR. It is a pure function of the model, normalization, resources and routing
// facts: the same inputs yield a byte-identical IR and the same lineage
// fingerprint, which is what lets the build be reproduced and the fingerprint
// be trusted as an identity.
#ifndef HARKEN_PROJECT_SCHEMA_BUILD_PIECE_IR_H
#define HARKEN_PROJECT_SCHEMA_BUILD_PIECE_IR_H

#include "harken_project_schema/EffectChainSelection.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/NormalizedFacts.h"
#include "harken_project_schema/PieceIR.h"
#include "harken_project_schema/ReferenceResolution.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/SendResolution.h"

#include <optional>

namespace harkenprojectschema {

// Builds the IR only when the model and normalization facts match exactly, and
// every clip that references a Sound has resource facts. Every Track and Piece
// must have one facts row, with the same clip or marker ids, and a Track may have
// at most one Piece owner; a Sound target must have a valid entry in resources.
// Any mismatch or missing/invalid resource fact returns no value, never a
// partial IR — the same zero-partial-output rule the front-end follows. A
// Track's clip order is taken from the validated facts, and each occurrence is
// enriched with its resource's duration and seekability.
//
// A Container or Piece target is left without duration facts: its end is decided
// by its own program or exit marker, not a natural length, so it needs no resource
// entry and is not refused for lacking one.
//
// resolution, sends and effectChains are the front-end's required routing and
// effect facts. Their occurrence keys must match the normalized occurrence set
// exactly, with the same subject and ordinal on every row; otherwise no IR is
// produced. A built occurrence carries its resolved output bus, merged send list
// and selected chain reference, so all three enter both the IR and the semantic
// fingerprint. The chain table itself is carried once on the IR and referenced by
// index; every reference must resolve, so a selection indexing outside that table,
// or an entry whose parameter block does not resolve inside its own chain,
// produces no IR.
std::optional<PieceIR> BuildPieceIR(
    const CanonicalProject& project, const NormalizedFacts& facts,
    const ResourceTable& resources,
    const ReferenceResolutionFacts& resolution,
    const SendResolutionFacts& sends,
    const EffectChainSelectionFacts& effectChains);

// Builds neutral routing only for tests that exercise the IR's structural
// shape. Programs intended for delivery must use BuildPieceIR with complete
// routing facts instead.
std::optional<PieceIR> BuildPieceIRForStructureOnly(
    const CanonicalProject& project, const NormalizedFacts& facts,
    const ResourceTable& resources);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_BUILD_PIECE_IR_H
