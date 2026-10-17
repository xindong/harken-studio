// The deterministic time-and-order facts the normalization stage derives from a
// canonical model.
//
// These are a product parallel to CanonicalProject, not a mutation of it. The
// canonical model keeps authored order — the order a round-trip must reproduce —
// while these facts carry the *derived* order and coordinate that downstream
// stages consume: a Piece's resolved start coordinate, its marker table in the one
// canonical enumeration order, and each Track's clips in audio order. Keeping
// the two apart is what lets authored order stay faithful while audio order is
// still stated once, unambiguously.
//
// Every list here is ordered by a total key, so two runs over the same input
// produce byte-identical facts. Nothing here is authored; it is all computed,
// which is the other reason it does not belong inside the authored model.
#ifndef HARKEN_PROJECT_SCHEMA_NORMALIZED_FACTS_H
#define HARKEN_PROJECT_SCHEMA_NORMALIZED_FACTS_H

#include "harken_project_schema/Model.h"

#include <cstdint>
#include <vector>

namespace harkenprojectschema {

// The normalized time facts of one Piece.
struct PieceNormalization {
    StableId piece = kUnmintedStableId;
    PieceTimelineSemantics timelineSemantics = PieceTimelineSemantics::Legacy18;

    StartPointKind startKind = StartPointKind::Entry;
    std::int64_t startCoordinate = 0;
    // The marker the start names, or the reserved id 0 for an Entry start.
    MarkerId startMarkerId = kReservedMarkerId;
    PieceTimelineBounds bounds{};

    // The Piece's markers in ascending MarkerId order — the canonical enumeration,
    // independent of the order they were authored in. Holds marker stable ids.
    std::vector<StableId> markersByMarkerId;
};

// The normalized clip order of one Track.
struct TrackNormalization {
    StableId track = kUnmintedStableId;

    // The Track's clips in audio order: ascending by (time, semanticOrderKey,
    // clipId). The clip id is the final tiebreak and is unique, so the order is
    // total; authored order is only the input a stable sort starts from and
    // never decides the result. Holds clip stable ids.
    std::vector<StableId> clipsByOrder;
};

// The full set of normalization facts for a compiled project. Both lists are
// ordered by ascending stable id of the Piece or Track they describe, so the
// product's own order is deterministic before any serialization.
struct NormalizedFacts {
    std::vector<PieceNormalization> pieces;
    std::vector<TrackNormalization> tracks;
};

std::optional<PieceTimelineBounds> DerivePieceTimelineBounds(
    const CanonicalProject& project, const PieceDef& piece) noexcept;

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_NORMALIZED_FACTS_H
