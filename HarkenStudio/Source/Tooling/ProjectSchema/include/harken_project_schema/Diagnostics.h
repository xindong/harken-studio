// The refusals the compile front-end can raise, and the record it raises them
// as.
//
// Every diagnostic here is a hard error: the front-end produces a canonical
// model only when it raises none, so there is no warning tier and no partial
// output. The codes name a specific contract violation rather than a generic
// "invalid", because a diagnostic's job is to be attributable — an explain
// report and a test both need to say which rule was broken, not merely that one
// was.
//
// A diagnostic carries the authoring GUIDs it is about and, for the structural
// failures, the chain of GUIDs from a root to the offending point. That chain
// is the closure/cycle explain in its minimal form: it lets a reader see the
// path that reached a dangling edge or closed a cycle without a separate report
// file.
#ifndef HARKEN_PROJECT_SCHEMA_DIAGNOSTICS_H
#define HARKEN_PROJECT_SCHEMA_DIAGNOSTICS_H

#include "harken_project_schema/Model.h"

#include <cstdint>
#include <vector>

namespace harkenprojectschema {

// The contract violations, numbered by the stage that raises them so a smaller
// code is a failure found earlier. The order is also the tiebreak used to make
// the diagnostic list deterministic, so it is append-only in spirit: reordering
// would reorder output.
enum class DiagnosticCode : std::uint32_t {
    // Stage 1 — parse: a field authored where its kind forbids it.
    ClipsOnNonTrack = 100,
    TimeSpecOnNonPiece = 101,        // a time spec on anything but a Piece
    StartPointOnNonPiece = 102,
    MarkersOnNonPiece = 103,
    PieceMissingTimeSpec = 104,
    PieceMissingStartPoint = 105,
    ReservedMarkerId = 106,             // a marker authored with the reserved id 0
    StartPointMarkerReserved = 107,     // a Marker start point naming the reserved id 0
    StartPointMarkerMissing = 108,      // a Marker start point whose marker is not in the table
    MusicalMapEmpty = 109,           // a Musical piece with an empty tempo/meter map
    NonMusicalMapPresent = 110,      // an Absolute piece carrying maps
    NullGuid = 111,                  // an object, clip or marker authored with the null GUID
    InvalidContentKind = 112,         // a content-kind ordinal outside the enum domain
    InvalidPieceTimeKind = 113,       // a piece-time ordinal outside the enum domain
    InvalidStartPointKind = 114,      // a start-point ordinal outside the enum domain
    InvalidMarkerKind = 115,             // a marker-kind ordinal outside the enum domain
    DuplicateMarkerId = 116,             // two markers in one Piece share a non-zero marker id
    EntryStartPointMarkerPresent = 117,  // an Entry start point carrying a marker id
    SendTargetBusUnset = 118,         // a send entry carrying the reserved bus id 0
    GraphNodeIdReserved = 119,        // an effect node carrying the reserved node id 0
    GraphNodeIdDuplicate = 120,       // two effect nodes in one graph share a node id
    GraphEffectTypeReserved = 121,    // an effect node carrying the reserved effect type
    GraphParamIdDuplicate = 122,      // one effect node repeats a param id
    GraphEdgeEndpointMissing = 123,   // an edge endpoint names a node the graph never defines
    GraphCycle = 124,                 // a cycle in the effect graph
    UnknownPlayablePropertyBits = 125,
    NonCanonicalAbsentProperty = 126,
    OccurrencePropertiesOnNonContainer = 127,
    OccurrenceOrdinalOverflow = 128,
    ContainerChildOccurrencesOnNonContainer = 129,
    EntryMarkerOnNonPiece = 130,

    // Stage 2 — stable identity.
    DuplicateGuid = 200,             // two authored objects share one GUID
    DuplicateStableId = 201,         // two objects claim one persisted stable id
    ReservedStableId = 202,          // a persisted object claims reserved id zero
    MintExhausted = 203,              // no stable id remains above the high-water mark

    // Stage 3 — structural ownership graph.
    DanglingOwnership = 300,         // an ownership edge to a GUID no node defines
    IllegalOwnership = 301,          // an ownership edge the kind rule forbids
    MultipleOwners = 302,            // an object owned by more than one parent
    RootAlsoOwned = 303,             // a declared root that is also owned
    OwnedNotRootNorOwned = 304,      // an object neither a root nor owned by anyone
    StructuralCycle = 305,           // an ownership cycle
    UnknownRoot = 306,               // a declared root GUID no node defines
    DuplicateRoot = 307,              // one root GUID is declared more than once
    DuplicateOwnershipEdge = 308,     // one parent repeats the same child edge
    DuplicateOccurrenceOrdinal = 309,

    // Stage 3 — placement graph.
    DanglingContentRef = 400,        // a clip referencing a GUID no node defines
    IllegalContentRefType = 401,     // a clip referencing a non-Playable kind
    ContainerOccurrenceCycle = 402,

    // Stage 4 — time and marker normalization.
    MusicalMapNotSingletonAtZero = 500,  // a Musical map is not exactly one row at tick 0
    MultipleExitMarkers = 501,              // a Piece declares more than one exit marker
    NegativeClipDuration = 502,
    ClipEndOverflow = 503,
    InvalidPieceTimelineBounds = 504,

    // Reference-slot resolution.
    ReferenceChainDepthExceeded = 600,   // a placement/organizational chain ran past the depth ceiling

    // Send resolution.
    SendCountExceedsLayoutLimit = 601,   // merged sends exceed the per-voice send-slot layout bound

    // Effect-chain selection and linearization.
    EffectChainNodeNotLinear = 602,      // a selected graph node merges or splits the signal path
    EffectChainNotConnected = 603,       // a selected graph's nodes do not form one path

    // Effect-chain lowering to a fixed slot layout.
    EffectChainExceedsSlotLimit = 604,   // a lowered chain is longer than the fixed slot layout bound

    // Effect-graph declaration consistency.
    EffectGraphContentWithoutDeclaration = 605,  // an object holds graph content it never declares
};

// One refusal. The primary and secondary GUIDs are the objects the failure is
// about — for an ownership edge, the owner and the child; for a clip reference,
// the owning object and the clip. path, when present, is the chain of GUIDs
// walked to reach the failure, root first.
struct Diagnostic {
    DiagnosticCode code = DiagnosticCode::NullGuid;
    AuthoringGuid primary{};
    AuthoringGuid secondary{};
    // A numeric detail whose meaning depends on the code: an offending marker or
    // stable id, a high-water mark, or a kind ordinal. Zero when the code needs
    // no number.
    std::uint64_t detail = 0;
    std::vector<AuthoringGuid> path;
};

// The human-facing spelling of a code, stable across runs. Diagnostics and
// explain output use it so a reader is never handed a bare ordinal.
const char* DiagnosticCodeName(DiagnosticCode code);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_DIAGNOSTICS_H
