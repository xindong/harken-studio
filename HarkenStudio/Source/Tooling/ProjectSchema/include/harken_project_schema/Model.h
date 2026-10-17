// The frozen typed shape of a Project v6 authoring model, plus the primitive
// value types it is built from.
//
// This is the *canonical* model: the output of a successful compile front-end,
// after identity has been minted and the structural and placement graphs have
// been proven well-formed. Invalid states are made unrepresentable here rather
// than validated away later — the type of a content object decides what it may
// carry, so a Track literally has no place to store a route target or a clock,
// and a Clip literally has no place to store a routing field. The lenient shape
// that a hostile or mistaken authoring layer might hand us lives in
// ProjectInput.h; refusing it is the front-end's job, not this file's.
//
// The schema is a candidate marked experimental. Serialization of it (Serialize.h)
// exists only to make fixtures legible and to prove determinism; it promises
// nothing about persistence compatibility across versions.
#ifndef HARKEN_PROJECT_SCHEMA_MODEL_H
#define HARKEN_PROJECT_SCHEMA_MODEL_H

#include "harken_playable_properties.g.h"
#include <harken_studio/ObjectId.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace harkenprojectschema {

using AuthoringGuid = harkenstudio::ObjectId;

// The all-zero GUID is reserved to mean "no object". An authored object may not
// carry it, so it can stand in for absence without colliding with a real id.
inline constexpr AuthoringGuid kNullGuid{0, 0};

using harkenstudio::IsNull;

using StableId = harkenstudio::ObjectId;

inline constexpr StableId kUnmintedStableId{};

// The five content object kinds. Ordinals are part of no wire format here — the
// numeric schema/ABI ids are assigned by the separate generated schema registry,
// not by this candidate model — but a fixed order keeps switch coverage and
// diagnostic spelling stable.
enum class ContentKind : std::uint32_t {
    Layer = 0,
    Sound = 1,
    Container = 2,
    Track = 3,
    Piece = 4,
};

// The ownership rule of the structural tree, stated once. Every other part of
// the front-end asks this rather than re-encoding the table, so "who may own
// whom" cannot drift into two disagreeing copies.
//
//   Layer     -> Layer | Sound | Container | Track | Piece
//   Container -> Sound | Container | Track | Piece         (never a Layer)
//   Track     -> Sound | Container | Piece                 (never a Track, never a Layer)
//   Piece     -> Track                                     (only Tracks)
//   Sound     -> nothing
//
// The two absences that carry the most contract weight are Track never owning a
// Track (Tracks are flat; deeper structure is Track<->Piece content nesting)
// and Piece owning only Tracks.
inline bool MayOwn(ContentKind owner, ContentKind child) {
    switch (owner) {
        case ContentKind::Layer:
            return true;  // every kind may be filed under a Layer
        case ContentKind::Container:
            return child != ContentKind::Layer;
        case ContentKind::Track:
            return child == ContentKind::Sound ||
                   child == ContentKind::Container ||
                   child == ContentKind::Piece;
        case ContentKind::Piece:
            return child == ContentKind::Track;
        case ContentKind::Sound:
            return false;
    }
    return false;
}

// What a Track clip is allowed to point at. A placement references a Playable,
// and only Sound, Container and Piece are Playable targets — a clip pointing at
// a Track or a Layer is a type error, not a dangling reference.
inline bool IsPlayableRefKind(ContentKind kind) {
    return kind == ContentKind::Sound || kind == ContentKind::Container ||
           kind == ContentKind::Piece;
}

// A Track clip's reference to the Playable it places. The kind is carried
// alongside the id so a clip that resolves to the wrong kind of object is a
// typed refusal rather than a silent misplacement.
struct ContentRef {
    ContentKind kind = ContentKind::Sound;
    StableId target = kUnmintedStableId;
};

struct ContentPropertyRefV1 {
    ContentKind kind = ContentKind::Sound;
    StableId target = kUnmintedStableId;
};

// Musical time is a signed 64-bit tick count at a fixed 960 ticks per quarter
// note. Signed because a pickup (an anacrusis) sits at a negative coordinate
// ahead of the origin.
using MusicalTick = std::int64_t;
inline constexpr std::int64_t kMusicalPpq = 960;

// Absolute time is a signed 64-bit nanosecond count, converted to frames only
// when the program is registered against a device rate. This library performs
// no time normalization — that is a later compiler stage — so it stores time
// values without interpreting them.
using AbsoluteNanos = std::int64_t;

// One tempo-map row: the tempo in force from a tick until the next row. Pieces
// in this initial scope carry exactly one row at tick 0, but the field is a map
// from day one so a later vintage adds rows without a schema change.
struct TempoMapEntry {
    MusicalTick tick = 0;
    // Microseconds per quarter note, the integer tempo representation, kept
    // integral so a serialized model is byte-reproducible without float
    // formatting choices entering the picture.
    std::int64_t microsPerQuarter = 500000;  // 120 BPM
};

// One meter-map row: the time signature in force from a tick. Same initial-scope
// rule as the tempo map — exactly one row at tick 0 — and same map-from-day-one
// shape.
struct MeterMapEntry {
    MusicalTick tick = 0;
    std::uint16_t numerator = 4;
    std::uint16_t denominator = 4;
};

enum class PieceTimeKind : std::uint32_t {
    Absolute = 0,
    Musical = 1,
};

// A Piece's time coordinate system. Absolute pieces measure in nanoseconds and
// carry no maps. Musical pieces measure in ticks and carry a tempo map and a
// meter map. The two maps are only meaningful for Musical, which the front-end
// enforces on the way in.
struct PieceTimeSpec {
    PieceTimeKind kind = PieceTimeKind::Absolute;
    std::vector<TempoMapEntry> tempoMap;
    std::vector<MeterMapEntry> meterMap;
};

// A Piece starts either at its built-in entry or at one of its own non-entry
// markers. Entry is a sentinel; it is not a marker and does not occupy MarkerId 0.
enum class StartPointKind : std::uint32_t {
    Entry = 0,
    Marker = 1,
};

// A marker id. Zero is reserved and can never name a real marker: the entry point is
// StartPointKind::Entry, never "the marker with id 0". A marker authored with id 0 is
// therefore an attempt to smuggle the entry point into the marker table, which the
// front-end refuses.
using MarkerId = std::uint32_t;
inline constexpr MarkerId kReservedMarkerId = 0;

// The kinds a marker in the marker table may take. The entry point is deliberately
// absent: the marker table holds only custom and exit markers, and entry lives in the
// Piece's StartPoint instead.
enum class MarkerKind : std::uint32_t {
    Custom = 0,
    Exit = 1,
};

// Where a Piece begins. For Marker, markerId is the non-zero id of a marker that must
// exist in the Piece's marker table; for Entry, markerId is unused and stays zero.
struct StartPoint {
    StartPointKind kind = StartPointKind::Entry;
    MarkerId markerId = kReservedMarkerId;
};

// One custom or exit marker. Carries its own authoring identity (it is minted a
// stable id like any authored object) and a position in the Piece's time
// coordinate.
struct Marker {
    StableId id = kUnmintedStableId;
    AuthoringGuid guid{};
    MarkerId markerId = kReservedMarkerId;
    MarkerKind kind = MarkerKind::Custom;
    std::int64_t position = 0;
};

// A crossfade envelope for a clip, in the clip's own time domain. The front-end
// carries it through without interpreting it; length validation belongs to the
// time-normalization stage, not to structural/placement validation.
struct FadeSpec {
    std::int64_t inLength = 0;
    std::int64_t outLength = 0;
};

// The tiebreak among clips that land on the same frame. It is authored, stable
// across Track moves and reorders, and distinct from editorOrder (which is
// display-only and never reaches audio ordering).
using SemanticOrderKey = std::uint64_t;

// One placement on a Track: a reference to a Playable, where it sits, how long
// its fades are, and its same-frame tiebreak. Its effective length is derived
// from the referenced content's natural length by the fixed use-content policy;
// length is not a Clip field. Natural placement, zero content offset and the
// use-content policy are constants in this initial scope, not clip fields. The
// authored common-property payload supplies the placement values; the Clip
// still carries no independent clock binding.
// A bus id names a destination bus. Zero is reserved to mean "unset": it can
// never name a real bus, so a send authored against it is an unset entry the
// front-end refuses rather than routing a level nowhere.
using BusId = harkenstudio::ObjectId;
inline constexpr BusId kReservedBusId{};

// One entry in a content object's send list: a level dispatched to a target bus
// in parallel with the object's main output. targetBusId names the destination
// and is never the reserved id 0 on a valid entry; sendLevel is carried
// verbatim. This candidate model stores the level without interpreting it — the
// same discipline the time and tempo values follow — so equality compares its
// exact bits rather than a rounded decimal, keeping "equal" in step with
// "serializes identically".
struct Send {
    BusId targetBusId = kReservedBusId;
    float sendLevel = 0.0f;
};

inline bool operator==(const Send& a, const Send& b) {
    return a.targetBusId == b.targetBusId &&
           std::bit_cast<std::uint32_t>(a.sendLevel) ==
               std::bit_cast<std::uint32_t>(b.sendLevel);
}
inline bool operator!=(const Send& a, const Send& b) { return !(a == b); }

// A node id, unique within one signal graph. Zero is reserved to mean "unset"
// and can never name a real node, so a node authored with it — or an edge
// endpoint pointing at nothing that node ever declares — is refused.
using GraphNodeId = std::uint32_t;
inline constexpr GraphNodeId kReservedGraphNodeId = 0;

// An effect type id. The whole id space from zero upward is owned by the
// engine's effect registry, which this model does not consult — it stores the id
// and the front-end only checks it is not the reserved value.
//
// The reserved value is the maximum, not zero, and the asymmetry with every
// other reserved id here is deliberate. A reserved id has to be a value its
// owner will never mint, and this id space has a different owner than the
// others: the engine numbers its effect types densely from zero, so zero is the
// first real type rather than a free sentinel. Reserving it would make that type
// unauthorable — a graph naming it would be refused as carrying no type at all —
// so the sentinel is taken from the end the registry grows away from.
using EffectTypeId = std::uint32_t;
inline constexpr EffectTypeId kReservedEffectType =
    std::numeric_limits<EffectTypeId>::max();

// One authored parameter on an effect node: an id paired with a value held as raw
// bits. The generated effect registry owns what a paramId means and whether its
// bits are a float, an int or an enum; this candidate model stores the bits
// verbatim and never interprets them (a float parameter is bit_cast to this word
// by the authoring layer), so equality and serialization compare the exact bits.
//
// The id's meaning is fixed by contract even though this model does not read it:
// a paramId IS the effect type's private, zero-based parameter index. The
// registry exposes no parameter name to match against, so an index is the only
// thing a loader can address a value by — every stage that consumes a compiled
// chain depends on this, from bounding an id by the type's parameter count to
// placing values in descriptor order. A consequence worth stating: because the
// index space is dense and no value may be invented for an index nobody authored,
// an effect node is expected to author every index of its type exactly once, and a
// loader refuses a node that does not.
struct EffectParam {
    std::uint32_t paramId = 0;
    std::uint32_t valueBits = 0;
};

inline bool operator==(const EffectParam& a, const EffectParam& b) {
    return a.paramId == b.paramId && a.valueBits == b.valueBits;
}
inline bool operator!=(const EffectParam& a, const EffectParam& b) {
    return !(a == b);
}

// One node in an object's effect signal graph: an effect of some type carrying
// its authored parameters. nodeId is unique within the graph and never the
// reserved 0; effectType names the effect and is never the reserved value; params
// are kept in authored order.
struct EffectNode {
    GraphNodeId nodeId = kReservedGraphNodeId;
    EffectTypeId effectType = kReservedEffectType;
    std::vector<EffectParam> params;
    harkenstudio::ObjectId effectId{};
};

inline bool operator==(const EffectNode& a, const EffectNode& b) {
    return a.nodeId == b.nodeId && a.effectType == b.effectType &&
           a.params == b.params && a.effectId == b.effectId;
}
inline bool operator!=(const EffectNode& a, const EffectNode& b) {
    return !(a == b);
}

// One directed edge in the graph: signal flows from fromNodeId to toNodeId. Both
// endpoints must name nodes the graph defines, and the graph must stay acyclic.
// Effect graphs in this schema carry no feedback. Edges are kept in authored order.
struct GraphEdge {
    GraphNodeId fromNodeId = kReservedGraphNodeId;
    GraphNodeId toNodeId = kReservedGraphNodeId;
};

inline bool operator==(const GraphEdge& a, const GraphEdge& b) {
    return a.fromNodeId == b.fromNodeId && a.toNodeId == b.toNodeId;
}
inline bool operator!=(const GraphEdge& a, const GraphEdge& b) {
    return !(a == b);
}

// An object's effect signal graph: the effect nodes and the directed edges
// between them. The front-end proves the graph is a structurally legal DAG —
// unique non-zero node ids, effect types other than the UINT32_MAX sentinel, no
// duplicate param ids within a node, edges whose endpoints resolve, and no cycle
// — but does not compile it,
// judge its topology (linear versus branching is a later capability gate), or
// reach the engine. An empty graph (no nodes) is the default and costs nothing
// to serialize.
//
// Whether the owning object declares a graph at all is carried by property 16
// in the chassis presence mask: this type describes a declaration's content,
// not its existence. How a declaration relates to an ancestor's is not carried
// here either, and deliberately: selection picks one declaration along the chain
// and inheritance follows from which objects declare, so there is no per-graph
// combination setting to disagree with the selected result.
struct SignalGraph {
    std::vector<EffectNode> nodes;
    std::vector<GraphEdge> edges;
};

// The spatial-placement group: panning and the 3D attenuation fields, gathered
// out of the flat mixing scalars into one substructure. Panning is the first
// registered piece of a whole spatialization capability — stereo balance in this
// initial scope, later spatialization mix, 2D position and panning modes
// — and it couples with the 3D attenuation system rather than with the pure gain
// scalars, so it does not belong flattened beside volume and pitch. The mixing
// group stays a pure gain domain, and future spatial fields and the attenuation
// reference slot mount here. Every value is stored verbatim and not interpreted;
// equality compares floats by their exact bits so it stays in step with
// serialization.
struct SpatialSpec {
    float panning = 0.0f;
    float spread = 0.0f;
    bool is3DEnabled = false;
    float attenuationDistanceScaling = 1.0f;
    harkenstudio::ObjectId attenuationRef{};
    float spatializationMix = 1.0f;
};

inline bool operator==(const SpatialSpec& a, const SpatialSpec& b) {
    return std::bit_cast<std::uint32_t>(a.panning) ==
               std::bit_cast<std::uint32_t>(b.panning) &&
           std::bit_cast<std::uint32_t>(a.spread) ==
               std::bit_cast<std::uint32_t>(b.spread) &&
           a.is3DEnabled == b.is3DEnabled &&
           std::bit_cast<std::uint32_t>(a.attenuationDistanceScaling) ==
               std::bit_cast<std::uint32_t>(b.attenuationDistanceScaling) &&
           a.attenuationRef == b.attenuationRef &&
           std::bit_cast<std::uint32_t>(a.spatializationMix) ==
               std::bit_cast<std::uint32_t>(b.spatializationMix);
}
inline bool operator!=(const SpatialSpec& a, const SpatialSpec& b) {
    return !(a == b);
}

// The exact common-property payload shared by definitions and occurrences.
// Presence, never value comparison, distinguishes an absent local value from
// an explicitly authored canonical default.
struct PlayablePropertyPayloadV1 {
    harkenplayableproperties::ChassisPresenceV1 presence;
    float volume = 0.0f;
    float pitch = 0.0f;
    float makeUpGain = 0.0f;
    std::uint32_t loopCount = 1;
    std::uint32_t streamingMode = 0;
    float outputBusVolume = 0.0f;
    std::vector<Send> sends;
    harkenstudio::ObjectId mainOutputBusRef{};
    SpatialSpec spatial;
    SignalGraph signalGraph;
};

// The common content-object chassis: the mixing and playback scalars, the send
// list, the spatial-placement group and the effect signal graph that all five
// kinds share, on top of identity and the display-only editorOrder. The
// generated schema registry owns the wire field numbering; this candidate model
// carries the values by name so the front-end can pass them through and the
// serializer can prove determinism.
//
// Every value here is stored verbatim and not interpreted: dB, panning law, the
// meaning of a streaming mode or a curve type, how a send level combines
// downstream, and what an effect node does are all a later compiler stage's
// business, exactly as the raw time values are stored here and normalized later.
// Structural, placement and identity validation never read these fields; the
// rules the front-end does enforce over them are narrow — a send may not carry
// the reserved bus id, and the effect graph must be a structurally legal DAG.
//
// The initial default of each field matches the neutral authoring state of the
// legacy component fields it restores: gains at zero, panning centered, one
// loop pass, unit distance scaling. mute and solo are deliberately absent — they are
// an editor concern that never reaches the runtime data.
//
struct Chassis : PlayablePropertyPayloadV1 {
    StableId id = kUnmintedStableId;
    AuthoringGuid guid{};
    std::int64_t editorOrder = 0;

};

struct Clip : PlayablePropertyPayloadV1 {
    StableId id = kUnmintedStableId;
    AuthoringGuid guid{};
    ContentRef contentRef;
    std::uint32_t stableOccurrenceOrdinal = 0;
    std::int64_t time = 0;
    std::int64_t duration = 0;
    FadeSpec fades;
    SemanticOrderKey semanticOrderKey = 0;
};

struct ContainerChildOccurrence : PlayablePropertyPayloadV1 {
    std::uint32_t stableOccurrenceOrdinal = 0;
    ContentPropertyRefV1 childPlayableRef;
};

// A Sound. A leaf content object; owns nothing.
struct SoundDef {
    Chassis chassis;
    std::vector<StableId> ownedChildren;  // always empty; kept for a uniform view
};

// A Layer: a purely organizational node that may file any kind beneath it.
struct LayerDef {
    Chassis chassis;
    std::vector<StableId> ownedChildren;
};

// A Container: owns content of any kind except a Layer.
struct ContainerDef {
    Chassis chassis;
    std::vector<StableId> ownedChildren;
    std::vector<ContainerChildOccurrence> children;
};

// A Track: the fifth content object, on the same chassis as the others.
//
// It carries the common chassis (identity, editorOrder and the shared mixing,
// playback, 3D, send and signal-graph fields), an ownership ledger (the
// Sound/Container/Piece children it structurally owns) and a clip placement
// ledger. Beyond the shared chassis and those two ledgers it adds nothing
// Track-specific: no route target
// or any routing field, no clock, tempo, meter, marker or any time-side field, and
// no parent-Track pointer. Those absences are the frozen Track contract, and
// they are enforced here by there being no member to hold them — a later reader
// cannot add a clock to a Track without editing this struct, which is exactly
// the friction the contract wants. (mute and solo are absent from the chassis
// itself, an editor concern that never reaches the runtime data.)
struct TrackDef {
    Chassis chassis;
    std::vector<StableId> ownedChildren;
    std::vector<Clip> clips;
};

struct EntryMarker {
    StableId id = kUnmintedStableId;
    AuthoringGuid guid{};
    std::int64_t position = 0;
};

struct PieceTimelineBounds {
    std::int64_t contentStart = 0;
    std::int64_t contentEnd = 0;
    std::int64_t nominalStart = 0;
    std::int64_t nominalEnd = 0;

    bool operator==(const PieceTimelineBounds&) const = default;
};

struct PieceDef {
    Chassis chassis;
    std::vector<StableId> tracks;  // owned Tracks; the structural children of a Piece
    PieceTimeSpec timeSpec;
    StartPoint startPoint;
    std::vector<Marker> markers;
    std::optional<EntryMarker> entryMarker;
};

// A resolved content object, tagged by kind. Consumers switch on kind and read
// the matching def; the defs are stored inline so a canonical project is one
// self-contained value.
struct ContentObject {
    ContentKind kind = ContentKind::Sound;
    LayerDef layer;
    SoundDef sound;
    ContainerDef container;
    TrackDef track;
    PieceDef piece;

    // The identity shared by whichever def is active. Reading it does not
    // require knowing the kind, which is what lets the graph and the serializer
    // treat objects uniformly.
    const Chassis& chassis() const {
        switch (kind) {
            case ContentKind::Layer:
                return layer.chassis;
            case ContentKind::Sound:
                return sound.chassis;
            case ContentKind::Container:
                return container.chassis;
            case ContentKind::Track:
                return track.chassis;
            case ContentKind::Piece:
                return piece.chassis;
        }
        return sound.chassis;
    }
};

// A successfully compiled Project v6 model: every object minted, every
// ownership edge legal and acyclic, every clip reference resolved to a Playable
// of a legal kind. Objects are stored in a stable order (ascending stable id)
// so the model itself is order-deterministic before any serialization.
struct CanonicalProject {
    harkenstudio::ObjectId projectId{};
    std::vector<ContentObject> objects;
    std::vector<StableId> roots;  // top-level objects, in ascending stable id order
};

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_MODEL_H
