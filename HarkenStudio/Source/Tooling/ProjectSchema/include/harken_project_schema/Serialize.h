// A deterministic text rendering of a front-end result.
//
// This serializer is output-only and exists to serve fixtures: it gives
// "byte-identical" a concrete artifact to compare when proving determinism, and
// it turns a canonical model or a set of refusals into something a golden test
// and a human can both read. It is not a persistence format — there is no
// reader here, the schema it renders is an experimental candidate, and nothing
// promises the text is stable across versions of this library.
//
// The rendering is total and ordered: every field that reaches audio or
// identity is emitted, in a fixed order, with integers formatted without
// locale or float ambiguity. Two renderings of the same result are equal byte
// for byte, which is the whole point.
#ifndef HARKEN_PROJECT_SCHEMA_SERIALIZE_H
#define HARKEN_PROJECT_SCHEMA_SERIALIZE_H

#include "harken_project_schema/CapacityEstimate.h"
#include "harken_project_schema/EffectChainSelection.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/PieceIR.h"
#include "harken_project_schema/PieceProgram.h"
#include "harken_project_schema/ReferenceResolution.h"

#include <string>

namespace harkenprojectschema {

// Renders a full front-end result: the diagnostics (in the order the front-end
// produced them) and, when the compile succeeded, the canonical model followed
// by its normalization facts. Suitable for both the valid and the invalid
// fixtures.
std::string RenderResult(const FrontEndResult& result);

// Renders only the canonical model. Precondition: the model is present.
std::string RenderProject(const CanonicalProject& project);

// Renders only the normalization facts. Precondition: the facts are present.
std::string RenderNormalizedFacts(const NormalizedFacts& facts);

// Renders the immutable IR: its folded Tracks, its Pieces, the required lineage
// table and the lineage fingerprint. Output-only and deterministic, for the same
// fixture and determinism uses as the rest of this file.
std::string RenderPieceIR(const PieceIR& ir);

// Renders the active-capacity estimate: the per-scope peaks, the program bound,
// and whether that bound is a lower bound because unknown-duration occurrences
// were excluded.
std::string RenderCapacityEstimate(const CapacityEstimate& estimate);

// Renders the assembled program: both fingerprints, the required capabilities,
// then the IR and the estimate. Output-only and deterministic.
std::string RenderPieceProgram(const PieceProgram& program);

// Renders the per-occurrence reference-slot resolution facts: one occurrence
// block per clip, each slot's winner and its full candidate explain, in a fixed
// order. Output-only and deterministic, like the rest of this file.
std::string RenderReferenceResolution(const ReferenceResolutionFacts& facts);

// Renders the per-occurrence merged-send facts: one occurrence per line with its
// key and its merged sends, each as target bus and level bits, in winning order.
std::string RenderSendResolution(const SendResolutionFacts& facts);

// Renders the effect-chain facts: the compiled chain table (each chain's entries,
// its parameter blocks and its resource demand) followed by one occurrence per
// line naming the chain it selected and the declarations that selection overrode.
std::string RenderEffectChainSelection(const EffectChainSelectionFacts& facts);

// Renders only the diagnostics, one per line, in the given order.
std::string RenderDiagnostics(const std::vector<Diagnostic>& diagnostics);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_SERIALIZE_H
