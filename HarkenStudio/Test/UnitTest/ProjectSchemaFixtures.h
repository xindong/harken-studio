// Authored inputs for the Project v6 front-end tests, built in code the way the
// reference-slot tests build their world in code: a fixture is a function that
// returns a ProjectInput, small enough to state a topology in a few lines.
//
#ifndef HARKEN_PROJECT_SCHEMA_TEST_FIXTURES_H
#define HARKEN_PROJECT_SCHEMA_TEST_FIXTURES_H

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/ProjectInput.h"

namespace harkenprojectschema {
namespace fixtures {

// A GUID from a small integer, so a fixture reads by number and two objects are
// obviously distinct.
inline AuthoringGuid G(std::uint64_t n) { return harkentest::StudioObjectId(n); }

// A minimal Absolute time spec (no maps), the well-formed shape for a Piece that
// is not measured musically.
inline InputPieceTimeSpec AbsoluteTime() {
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Absolute;
    return spec;
}

// A minimal Musical time spec: one tempo row and one meter row at tick 0, the
// constant-map shape of this initial scope.
inline InputPieceTimeSpec MusicalTime() {
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Musical;
    spec.tempoMap.push_back(TempoMapEntry{});
    spec.meterMap.push_back(MeterMapEntry{});
    return spec;
}

inline InputStartPoint EntryStart() {
    InputStartPoint sp;
    sp.kind = StartPointKind::Entry;
    return sp;
}

inline InputNode Node(ContentKind kind, std::uint64_t guid) {
    InputNode n;
    n.kind = kind;
    n.guid = G(guid);
    n.presence.bits = UINT64_C(0x0000000000000fff);
    return n;
}

inline InputClip Clip(std::uint64_t guid, std::uint64_t targetGuid,
                      std::int64_t time = 0,
                      SemanticOrderKey order = 0,
                      std::uint32_t stableOccurrenceOrdinal = 0) {
    InputClip c;
    c.guid = G(guid);
    c.targetGuid = G(targetGuid);
    c.stableOccurrenceOrdinal = stableOccurrenceOrdinal;
    c.time = time;
    c.semanticOrderKey = order;
    return c;
}

inline InputMarker MakeMarker(std::uint64_t guid, MarkerId markerId, MarkerKind kind,
                        std::int64_t position = 0) {
    InputMarker c;
    c.guid = G(guid);
    c.markerId = markerId;
    c.kind = kind;
    c.position = position;
    return c;
}

inline InputStartPoint MarkerStart(MarkerId markerId) {
    InputStartPoint sp;
    sp.kind = StartPointKind::Marker;
    sp.markerId = markerId;
    return sp;
}

inline ProjectInput Valid() {
    ProjectInput in;

    InputNode layer = Node(ContentKind::Layer, 1);
    layer.ownedChildren = {G(3)};

    InputNode standaloneTrack = Node(ContentKind::Track, 2);
    standaloneTrack.ownedChildren = {G(8)};
    standaloneTrack.clips = {Clip(201, 8, 0)};

    InputNode layerTrack = Node(ContentKind::Track, 3);
    layerTrack.ownedChildren = {G(4), G(5), G(6)};
    layerTrack.clips = {
        Clip(301, 4, 0, 1, 0),
        Clip(302, 4, 480, 2, 1),
        Clip(303, 5, 960, 3, 2),
        Clip(304, 6, 1920, 4, 3),
    };

    InputNode sound = Node(ContentKind::Sound, 4);
    InputNode container = Node(ContentKind::Container, 5);

    InputNode piece = Node(ContentKind::Piece, 6);
    piece.ownedChildren = {G(7)};
    piece.timeSpec = MusicalTime();
    piece.startPoint = EntryStart();

    InputNode pieceTrack = Node(ContentKind::Track, 7);

    in.nodes = {layer,     standaloneTrack, layerTrack, sound,
                container, piece,           pieceTrack,
                Node(ContentKind::Sound, 8)};
    in.roots = {G(1), G(2)};
    return in;
}

inline ProjectInput EntryTimeline() {
    auto piece = Node(ContentKind::Piece, 1);
    piece.ownedChildren = {G(2)};
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();
    piece.entryMarker = InputEntryMarker{G(5), 100};
    piece.markers = {MakeMarker(6, 1, MarkerKind::Custom, 200),
                     MakeMarker(7, 2, MarkerKind::Exit, 300)};
    auto track = Node(ContentKind::Track, 2);
    track.ownedChildren = {G(3)};
    track.clips = {Clip(4, 3, -100)};
    track.clips[0].duration = 500;
    ProjectInput input;
    input.nodes = {piece, track, Node(ContentKind::Sound, 3)};
    input.roots = {G(1)};
    return input;
}

// ---- invalid: parent-Track ----------------------------------------------
// A Track owns a Track. Tracks are flat; the edge is refused by the ownership
// kind rule. Seen from the child, this is exactly "a Track with a parent-Track".
inline ProjectInput InvalidParentTrack() {
    ProjectInput in;
    InputNode outer = Node(ContentKind::Track, 1);
    outer.ownedChildren = {G(2)};
    InputNode inner = Node(ContentKind::Track, 2);
    in.nodes = {outer, inner};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: a time field on a Track ------------------------------------
// A Track carrying a time spec. The frozen Track contract has no time-side
// field; the front-end refuses the input that tried to spell one.
inline ProjectInput InvalidTrackTimeField() {
    ProjectInput in;
    InputNode track = Node(ContentKind::Track, 1);
    track.timeSpec = AbsoluteTime();
    in.nodes = {track};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: a Piece owning a non-Track ---------------------------------
// A Piece owns a Sound. A Piece owns Tracks only.
inline ProjectInput InvalidPieceOwnsNonTrack() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    piece.ownedChildren = {G(2)};
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();
    InputNode sound = Node(ContentKind::Sound, 2);
    in.nodes = {piece, sound};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: a structural cycle -----------------------------------------
// Two Containers own each other. Legal by kind, but the ownership graph must be
// acyclic. Neither is a declared root, and each is owned exactly once, so the
// only fault is the cycle.
inline ProjectInput InvalidStructuralCycle() {
    ProjectInput in;
    InputNode a = Node(ContentKind::Container, 1);
    a.ownedChildren = {G(2)};
    InputNode b = Node(ContentKind::Container, 2);
    b.ownedChildren = {G(1)};
    in.nodes = {a, b};
    in.roots = {};
    return in;
}

// ---- invalid: a duplicate stable id --------------------------------------
// Two distinct objects claim one persisted runtime id. Identity refuses it
// rather than merging them.
inline ProjectInput InvalidDuplicateStableId() {
    ProjectInput in;
    InputNode a = Node(ContentKind::Sound, 1);
    a.guid = harkentest::StudioObjectId(42);
    InputNode b = Node(ContentKind::Sound, 2);
    b.guid = harkentest::StudioObjectId(42);
    in.nodes = {a, b};
    in.roots = {G(1), G(2)};
    return in;
}

// ---- invalid: a dangling content reference -------------------------------
// A clip references a GUID no node defines. Structure is otherwise clean, so the
// placement pass runs and refuses the reference.
inline ProjectInput InvalidDanglingContentRef() {
    ProjectInput in;
    InputNode track = Node(ContentKind::Track, 1);
    track.clips = {Clip(100, 999)};  // 999 is defined nowhere
    in.nodes = {track};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: a content reference of the wrong kind ----------------------
// A clip references a Track. Only Sound, Container and Piece are Playable, so a
// clip pointing at a Track is a typed refusal.
inline ProjectInput InvalidContentRefType() {
    ProjectInput in;
    InputNode track = Node(ContentKind::Track, 1);
    track.clips = {Clip(100, 2)};
    InputNode other = Node(ContentKind::Track, 2);
    in.nodes = {track, other};
    in.roots = {G(1), G(2)};
    return in;
}

// ---- invalid: MarkerId 0 in the marker table -----------------------------------
// A Piece authors a marker with the reserved id 0. The entry point is a sentinel,
// never "the marker with id 0"; a marker at 0 is the entry point smuggled into the
// table and is refused.
inline ProjectInput InvalidReservedMarkerId() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();
    InputMarker marker;
    marker.guid = G(50);
    marker.markerId = kReservedMarkerId;  // 0
    marker.kind = MarkerKind::Custom;
    piece.markers = {marker};
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: a Marker start point at the reserved id -----------------------
// A Piece begins at "marker 0". Same contract as above from the start-point side:
// entry is StartPointKind::Entry, never a marker with id 0.
inline ProjectInput InvalidStartPointReserved() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    piece.timeSpec = AbsoluteTime();
    InputStartPoint sp;
    sp.kind = StartPointKind::Marker;
    sp.markerId = kReservedMarkerId;  // 0
    piece.startPoint = sp;
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- normalization: ordering and start coordinate ------------------------
//
// A project whose authored order is deliberately not the audio order, so the
// derived facts have something to normalize.
//
// Expected facts: T's clips ordered by time (0, 480, 960); P's markers ordered by
// ascending markerId (5, 7, 9); P's start coordinate resolved to 100.
inline ProjectInput NormalizationOrdering() {
    ProjectInput in;

    InputNode sound = Node(ContentKind::Sound, 4);

    InputNode track = Node(ContentKind::Track, 2);
    track.ownedChildren = {G(4)};
    track.clips = {
        Clip(203, 4, 960, 0, 0),
        Clip(201, 4, 0, 0, 1),
        Clip(202, 4, 480, 0, 2),
    };

    InputNode piece = Node(ContentKind::Piece, 6);
    piece.ownedChildren = {G(7)};
    piece.timeSpec = MusicalTime();
    piece.startPoint = MarkerStart(7);
    piece.markers = {
        MakeMarker(91, 9, MarkerKind::Custom, 300),
        MakeMarker(92, 7, MarkerKind::Custom, 100),
        MakeMarker(93, 5, MarkerKind::Exit, 50),
    };

    InputNode pieceTrack = Node(ContentKind::Track, 7);

    in.nodes = {sound, track, piece, pieceTrack};
    in.roots = {G(2), G(6)};
    return in;
}

// ---- invalid: a Musical map that is not one row at tick 0 ----------------
// One tempo row placed off the origin. An empty map is refused at parse; this is
// the second half of the frozen rule, enforced by the normalization stage.
inline ProjectInput InvalidMusicalMapNotSingleton() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Musical;
    spec.tempoMap.push_back(TempoMapEntry{480, 500000});  // off the origin
    spec.meterMap.push_back(MeterMapEntry{});             // a valid singleton
    piece.timeSpec = spec;
    piece.startPoint = EntryStart();
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: more than one exit marker -------------------------------------
// Exit is unique by kind; a Piece may declare at most one.
inline ProjectInput InvalidMultipleExitMarkers() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();
    piece.markers = {
        MakeMarker(51, 1, MarkerKind::Exit),
        MakeMarker(52, 2, MarkerKind::Exit),
    };
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: an object neither a root nor owned -------------------------
// A Sound that no node owns and no root declares. It is unreachable, so the
// structural pass refuses it rather than dropping it silently.
inline ProjectInput InvalidOrphanNode() {
    ProjectInput in;
    InputNode rooted = Node(ContentKind::Sound, 1);
    InputNode orphan = Node(ContentKind::Sound, 2);
    in.nodes = {rooted, orphan};
    in.roots = {G(1)};  // 2 is neither declared here nor owned anywhere
    return in;
}

// ---- invalid: an ownership edge to a missing object ----------------------
// A Layer owns a GUID no node defines. The edge cannot be followed, so it is
// refused as dangling.
inline ProjectInput InvalidDanglingOwnership() {
    ProjectInput in;
    InputNode layer = Node(ContentKind::Layer, 1);
    layer.ownedChildren = {G(999)};  // defined nowhere
    in.nodes = {layer};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: a Piece without a time spec --------------------------------
// A Piece must define its coordinate system. It has a start point, so only the
// missing time spec is at fault.
inline ProjectInput InvalidPieceMissingTimeSpec() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    piece.startPoint = EntryStart();  // present, so only the time spec is missing
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- IR: mixed lineage ----------------------------------------------------
//
// A project that exercises both lineage depths at once:
//
// So the lineage table holds two Track->Clip rows for T2 and one
// Piece->Track->Clip row for T3.
inline ProjectInput IrLineageMix() {
    ProjectInput in;

    InputNode sound = Node(ContentKind::Sound, 4);

    InputNode standalone = Node(ContentKind::Track, 2);
    standalone.ownedChildren = {G(4)};
    standalone.clips = {
        Clip(201, 4, 100, 0, 0),
        Clip(202, 4, 50, 0, 1),
    };

    InputNode piece = Node(ContentKind::Piece, 6);
    piece.ownedChildren = {G(7)};
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();

    InputNode pieceTrack = Node(ContentKind::Track, 7);
    pieceTrack.ownedChildren = {G(8)};
    pieceTrack.clips = {Clip(701, 8, 0)};

    in.nodes = {sound, standalone, piece, pieceTrack, Node(ContentKind::Sound, 8)};
    in.roots = {G(2), G(6)};
    return in;
}

// ---- clip sort: every comparison level matters ---------------------------
//
// Two Tracks whose clips force the tiebreak past the first key. On TrackKey the
// clips share a time, so only semanticOrderKey decides; on TrackId they share a
// time and a key, so only the persisted clip id decides. The persisted ids are
// chosen to disagree with the key order and the authored order, so dropping the
// key level or the id level from the sort would change the result.
//
//   4  Sound      S
//   2  TrackKey   clip id 10 (time 100, key 9), clip id 20 (time 100, key 3)
//   3  TrackId    clip id 30 (time 200, key 5), clip id 15 (time 200, key 5)
inline ProjectInput ClipSortMultiLevel() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 4);

    InputNode trackKey = Node(ContentKind::Track, 2);
    trackKey.ownedChildren = {G(4)};
    InputClip k1 = Clip(201, 4, 100, 9, 0);
    k1.guid = harkentest::StudioObjectId(10);
    InputClip k2 = Clip(202, 4, 100, 3, 1);
    k2.guid = harkentest::StudioObjectId(20);
    trackKey.clips = {k1, k2};

    InputNode trackId = Node(ContentKind::Track, 3);
    trackId.ownedChildren = {G(5)};
    InputClip i1 = Clip(301, 5, 200, 5, 0);
    i1.guid = harkentest::StudioObjectId(30);
    InputClip i2 = Clip(302, 5, 200, 5, 1);
    i2.guid = harkentest::StudioObjectId(15);
    trackId.clips = {i1, i2};

    in.nodes = {sound, trackKey, trackId, Node(ContentKind::Sound, 5)};
    in.roots = {G(2), G(3)};
    return in;
}

// ---- facts ordering: several Pieces and Tracks ---------------------------
// Enough top-level objects that "the fact lists are sorted by stable id" is a
// claim with content, not a single-element triviality.
//
//   10 Sound   S
//   20 Piece   P1  owns Track 30
//   21 Piece   P2  owns Track 31
//   40 Track   Tc  root; one clip -> S
inline ProjectInput MultiPieceMultiTrack() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 60);

    InputNode p1 = Node(ContentKind::Piece, 50);
    p1.ownedChildren = {G(20)};
    p1.timeSpec = AbsoluteTime();
    p1.startPoint = EntryStart();

    InputNode p2 = Node(ContentKind::Piece, 40);
    p2.ownedChildren = {G(10)};
    p2.timeSpec = AbsoluteTime();
    p2.startPoint = EntryStart();

    InputNode tc = Node(ContentKind::Track, 30);
    tc.ownedChildren = {G(60)};
    tc.clips = {Clip(400, 60, 0)};

    InputNode t30 = Node(ContentKind::Track, 20);
    InputNode t31 = Node(ContentKind::Track, 10);

    in.nodes = {sound, p1, p2, tc, t30, t31};
    in.roots = {G(50), G(40), G(30)};
    return in;
}

// ---- invalid: a Musical meter map off the origin -------------------------
// The mirror of InvalidMusicalMapNotSingleton, on the meter side, so the
// diagnostic detail distinguishing tempo (0) from meter (1) is exercised both
// ways.
inline ProjectInput InvalidMeterMapNotSingleton() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Musical;
    spec.tempoMap.push_back(TempoMapEntry{});           // valid singleton
    spec.meterMap.push_back(MeterMapEntry{480, 4, 4});  // off the origin
    piece.timeSpec = spec;
    piece.startPoint = EntryStart();
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: both Musical maps have more than one row -------------------
// Two rows each — so both raise, and the pair proves the diagnostics come out
// ordered by detail (tempo before meter).
inline ProjectInput InvalidMusicalBothMapsBad() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Musical;
    spec.tempoMap.push_back(TempoMapEntry{0, 500000});
    spec.tempoMap.push_back(TempoMapEntry{480, 400000});
    spec.meterMap.push_back(MeterMapEntry{0, 4, 4});
    spec.meterMap.push_back(MeterMapEntry{960, 3, 4});
    piece.timeSpec = spec;
    piece.startPoint = EntryStart();
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: two rows, the first at tick 0 ------------------------------
// The rule is "exactly one row at tick 0", not "the first row is at tick 0". A
// map whose first row sits at the origin but which carries a second row is still
// refused, which this pins.
inline ProjectInput InvalidMusicalMultiRowFirstAtZero() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Musical;
    spec.tempoMap.push_back(TempoMapEntry{0, 500000});    // first row at the origin
    spec.tempoMap.push_back(TempoMapEntry{480, 400000});  // but a second row exists
    spec.meterMap.push_back(MeterMapEntry{});             // valid singleton
    piece.timeSpec = spec;
    piece.startPoint = EntryStart();
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- valid: an empty Piece -----------------------------------------------
// A Piece with no markers and no Tracks. Nothing forbids it, so it compiles and its
// facts carry an empty marker list — a regression guard for the empty case.
inline ProjectInput EmptyPiece() {
    ProjectInput in;
    InputNode piece = Node(ContentKind::Piece, 1);
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();
    in.nodes = {piece};
    in.roots = {G(1)};
    return in;
}

// ---- estimation: a Musical Piece with overlapping occurrences -------------
//
// At 120 BPM one quarter is 500 ms, so with a one-second Sound the two
// occurrences overlap and the Piece scope's peak is 2.
inline ProjectInput MusicalOverlap() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 4);
    InputNode piece = Node(ContentKind::Piece, 6);
    piece.ownedChildren = {G(7)};
    piece.timeSpec = MusicalTime();
    piece.startPoint = EntryStart();
    InputNode track = Node(ContentKind::Track, 7);
    track.ownedChildren = {G(4)};
    track.clips = {
        Clip(701, 4, 0, 0, 0),
        Clip(702, 4, 960, 0, 1),
    };
    in.nodes = {sound, piece, track};
    in.roots = {G(6)};
    return in;
}

// ---- common chassis: scalar fields and sends shared by all five kinds -----
//
// A set of non-default common-chassis scalar values, a two-entry send list, and
// the reference-slot overrides, applied identically to any node. The
// values are deliberately distinct from the field defaults so a field silently
// left at its default would fail a passthrough check, and every float is an exact
// binary fraction so an exact comparison is legitimate.
inline void ApplyChassisProbe(InputNode* node) {
    node->volume = 1.5f;
    node->pitch = -2.25f;
    node->makeUpGain = 0.5f;
    node->loopCount = 0;
    node->streamingMode = 2;
    node->outputBusVolume = -6.0f;
    node->sends = {Send{harkentest::StudioObjectId(7), 0.25f}, Send{harkentest::StudioObjectId(9), -0.5f}};
    node->mainOutputBusRef = harkentest::StudioObjectId(4001);
    node->spatial.panning = -0.75f;
    node->spatial.spread = 0.375f;
    node->spatial.is3DEnabled = true;
    node->spatial.attenuationDistanceScaling = 2.0f;
    node->spatial.attenuationRef = harkentest::StudioObjectId(4002);
    node->presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
}

// ---- chassis: every kind carries the same chassis ------------------------
//
// One root of each kind, each carrying the identical non-default chassis probe.
// Proves the common chassis (scalars and sends) is shared by all five kinds —
// Layer included — and that the front-end passes every field through for each.
inline ProjectInput SharedChassisAllKinds() {
    ProjectInput in;

    InputNode layer = Node(ContentKind::Layer, 1);
    ApplyChassisProbe(&layer);

    InputNode sound = Node(ContentKind::Sound, 2);
    ApplyChassisProbe(&sound);

    InputNode container = Node(ContentKind::Container, 3);
    ApplyChassisProbe(&container);

    InputNode track = Node(ContentKind::Track, 4);
    ApplyChassisProbe(&track);

    InputNode piece = Node(ContentKind::Piece, 5);
    ApplyChassisProbe(&piece);
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();

    in.nodes = {layer, sound, container, track, piece};
    in.roots = {G(1), G(2), G(3), G(4), G(5)};
    return in;
}

// ---- invalid: a send targeting the reserved bus id -----------------------
// A send whose target bus is the reserved id 0 (unset). The reserved id can
// never name a real bus, so a send entry carrying it is refused.
inline ProjectInput InvalidSendUnsetTargetBus() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    sound.sends = {Send{kReservedBusId, 0.5f}};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// ---- invalid: two sends at the reserved bus id ---------------------------
// Two unset sends on one node, so the pair proves the refusals come out in a
// deterministic order — by the send index carried in the diagnostic detail.
inline ProjectInput InvalidTwoUnsetSends() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    sound.sends = {Send{kReservedBusId, 0.1f}, Send{kReservedBusId, 0.2f}};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// ---- signal graph: a small valid effect graph ----------------------------
//
// Two effect nodes joined by one edge, with per-node params, declared and applied
// identically to any node. Chosen distinct from the defaults so a passthrough
// check catches a field silently left at its default, and structurally clean
// (unique non-zero ids, effect types other than the UINT32_MAX sentinel, no
// duplicate params, resolving edge, acyclic) so it compiles — and a straight
// line, so it also linearizes.
inline void ApplyGraphProbe(InputNode* node) {
    SignalGraph graph;
    EffectNode first;
    first.nodeId = 1;
    first.effectType = 10;
    first.params = {EffectParam{100, 0x3f800000u}, EffectParam{101, 7u}};
    EffectNode second;
    second.nodeId = 2;
    second.effectType = 11;
    graph.nodes = {first, second};
    graph.edges = {GraphEdge{1, 2}};
    node->hasSignalGraph = true;
    node->signalGraph = graph;
}

// One root of each kind, each carrying the identical signal-graph probe. Proves
// the graph is on the shared chassis for all five kinds — Layer included — and
// passed through by the front-end for each.
inline ProjectInput SharedGraphAllKinds() {
    ProjectInput in;

    InputNode layer = Node(ContentKind::Layer, 1);
    ApplyGraphProbe(&layer);

    InputNode sound = Node(ContentKind::Sound, 2);
    ApplyGraphProbe(&sound);

    InputNode container = Node(ContentKind::Container, 3);
    ApplyGraphProbe(&container);

    InputNode track = Node(ContentKind::Track, 4);
    ApplyGraphProbe(&track);

    InputNode piece = Node(ContentKind::Piece, 5);
    ApplyGraphProbe(&piece);
    piece.timeSpec = AbsoluteTime();
    piece.startPoint = EntryStart();

    in.nodes = {layer, sound, container, track, piece};
    in.roots = {G(1), G(2), G(3), G(4), G(5)};
    return in;
}

// A one-node Sound whose graph carries the reserved node id 0.
inline ProjectInput InvalidGraphReservedNodeId() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode n;
    n.nodeId = kReservedGraphNodeId;
    n.effectType = 10;
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {n};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// Two graph nodes sharing one node id.
inline ProjectInput InvalidGraphDuplicateNodeId() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode a;
    a.nodeId = 5;
    a.effectType = 10;
    EffectNode b;
    b.nodeId = 5;
    b.effectType = 11;
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {a, b};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// A graph node carrying the reserved effect type.
inline ProjectInput InvalidGraphReservedEffectType() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode n;
    n.nodeId = 1;
    n.effectType = kReservedEffectType;
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {n};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// One node whose params repeat a param id.
inline ProjectInput InvalidGraphDuplicateParamId() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode n;
    n.nodeId = 1;
    n.effectType = 10;
    n.params = {EffectParam{5, 0u}, EffectParam{5, 1u}};
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {n};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// An edge whose endpoint names a node the graph does not define.
inline ProjectInput InvalidGraphEdgeEndpointMissing() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode n;
    n.nodeId = 1;
    n.effectType = 10;
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {n};
    sound.signalGraph.edges = {GraphEdge{1, 99}};  // 99 is defined nowhere
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// Two nodes joined into a cycle. Effect graphs in this schema carry no feedback,
// so the graph must be acyclic.
inline ProjectInput InvalidGraphCycle() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode a;
    a.nodeId = 1;
    a.effectType = 10;
    EffectNode b;
    b.nodeId = 2;
    b.effectType = 11;
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {a, b};
    sound.signalGraph.edges = {GraphEdge{1, 2}, GraphEdge{2, 1}};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// Two graph nodes both at the reserved node id 0, so the pair proves the
// refusals come out in a deterministic order — by the authored node index
// carried in the diagnostic detail.
inline ProjectInput InvalidGraphTwoReservedNodeIds() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 1);
    EffectNode a;
    a.nodeId = kReservedGraphNodeId;
    a.effectType = 10;
    EffectNode b;
    b.nodeId = kReservedGraphNodeId;
    b.effectType = 11;
    sound.hasSignalGraph = true;
    sound.signalGraph.nodes = {a, b};
    in.nodes = {sound};
    in.roots = {G(1)};
    return in;
}

// ---- estimation: a Musical tick that overflows nanoseconds -----------------
// A clip at a tick so large that tick * ns-per-quarter would overflow a signed
// 64-bit nanosecond count. The front-end accepts the coordinate; the estimate
// refuses it rather than wrapping.
inline ProjectInput MusicalOverflowTick() {
    ProjectInput in;
    InputNode sound = Node(ContentKind::Sound, 4);
    InputNode piece = Node(ContentKind::Piece, 6);
    piece.ownedChildren = {G(7)};
    piece.timeSpec = MusicalTime();
    piece.startPoint = EntryStart();
    InputNode track = Node(ContentKind::Track, 7);
    track.ownedChildren = {G(4)};
    InputClip huge = Clip(701, 4, 0);
    huge.time = 20000000000LL;  // 2e10 ticks * 5e8 ns/quarter overflows int64
    track.clips = {huge};
    in.nodes = {sound, piece, track};
    in.roots = {G(6)};
    return in;
}

}  // namespace fixtures
}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_TEST_FIXTURES_H
