// The active-capacity upper bound derived from a built PieceIR.
//
// This is a parallel product, not a field of the immutable IR: estimation is a
// separate analysis that reads the IR and can fail on its own (a musical
// coordinate that overflows nanoseconds), so keeping it out of the IR leaves the
// IR untouched and lets the estimate be recomputed or skipped independently.
//
// The bound counts, per arranging scope, the peak number of occurrences whose
// intervals overlap. A scope is a Piece (all its Tracks share the Piece's clock
// and origin) or a directly triggered standalone Track. Occurrences whose end is
// not a natural length — a Container or Piece target, terminated by its own
// program or exit marker — have no interval to overlap; they are excluded from the
// peak and counted separately, and their presence marks the active bound a lower
// bound rather than silently dropping them or counting them as unbounded.
//
// The preparing-capacity bound is deliberately absent: it needs the prepare
// fact whose shape is not yet defined upstream, so it is deferred with that
// fact rather than estimated on a guess.
#ifndef HARKEN_PROJECT_SCHEMA_CAPACITY_ESTIMATE_H
#define HARKEN_PROJECT_SCHEMA_CAPACITY_ESTIMATE_H

#include "harken_project_schema/Model.h"
#include "harken_project_schema/PieceIR.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The capacity of one arranging scope.
struct ScopeCapacity {
    bool isPiece = false;  // true: scopeId is a Piece; false: a standalone Track
    StableId scopeId = kUnmintedStableId;
    // The peak number of natural-length occurrences whose intervals overlap.
    std::uint32_t peakActiveOccurrences = 0;
    // Occurrences with no natural end (Container or Piece targets), excluded from
    // the peak above.
    std::uint32_t unknownDurationOccurrences = 0;
};

// The whole-program active-capacity estimate.
struct CapacityEstimate {
    std::vector<ScopeCapacity> scopes;  // ascending by scopeId

    // A conservative program-wide bound: the sum of the per-scope peaks, taking
    // every scope to be potentially triggered at once.
    std::uint32_t activeUpperBound = 0;
    // The total number of unknown-duration occurrences across all scopes. When
    // it is non-zero, activeUpperBound omits those occurrences and so is a lower
    // bound, which activeIsLowerBound records.
    std::uint32_t unknownDurationOccurrences = 0;
    bool activeIsLowerBound = false;
};

// Estimates the active capacity of a built IR. Returns no value when a musical
// occurrence's tick coordinate cannot be expressed in nanoseconds without
// overflowing a signed 64-bit count — a deterministic refusal at the authored
// coordinate rather than a silent wrap.
std::optional<CapacityEstimate> EstimateCapacity(const PieceIR& ir);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_CAPACITY_ESTIMATE_H
