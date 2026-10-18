// The immutable Project v6 intermediate representation for the front-end's
// output — the shape a later PieceProgram stage lowers to a device.
//
// It is derived, not authored: BuildPieceIR turns a validated CanonicalProject
// and its NormalizedFacts into this once, and nothing mutates it afterward. The
// two frozen facts this stage commits to are that a Track is folded — it is
// never an instantiable node, so no instance, bar or Bus entry is emitted for it
// — and that the runtime lineage table is required, so a Profiler can locate an
// occurrence by lane with no debug symbols.
//
// Each occurrence carries its resource duration and seekability facts and a
// FrameBatch event template; the active-capacity upper bound is a parallel
// product (see CapacityEstimate.h). What this stage deliberately does not
// produce: the preparing-capacity bound (deferred with the prepare fact whose
// shape is not yet defined upstream), the audible programSemanticFingerprint,
// and any prepare-requirement field. Inventing field names for those here would
// pre-commit shapes this stage cannot justify.
#ifndef HARKEN_PROJECT_SCHEMA_PIECE_IR_H
#define HARKEN_PROJECT_SCHEMA_PIECE_IR_H

#include "harken_project_schema/EffectChain.h"
#include "harken_project_schema/Model.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace harkenprojectschema {

namespace detail {
struct PieceIRBuilder;
}

// The compile-time half of the runtime OccurrenceKey. The full runtime key is
// {arrangingInstanceHandle, clipId, loopIteration, occurrenceOrdinal}; only
// clipId and occurrenceOrdinal are known when the program is compiled. The
// arranging instance handle (the Track's arranging host) and the loop iteration
// are runtime values — this stage invents no value for them and leaves no field
// to hold one.
struct OccurrenceRef {
    StableId clipId = kUnmintedStableId;
    // The 0-based position of this occurrence within its Track's normalized clip
    // order. With no loop or repeat in this scope a clip is exactly one
    // occurrence, so the ordinal is that normalized index.
    std::uint32_t occurrenceOrdinal = 0;
};

// A timeline event in an occurrence's FrameBatch template, tagged by its
// Voice-protocol phase. phasePriority follows the block event protocol:
// SetStartCursor 10, Start 20, Envelope 30, Stop 50 — the subeventPhase a later
// ScheduledEventInput carries.
enum class TemplateEventKind : std::uint32_t {
    SetStartCursor = 0,
    Start = 1,
    Envelope = 2,
    Stop = 3,
};

inline constexpr std::uint32_t kPhasePrioritySetStartCursor = 10;
inline constexpr std::uint32_t kPhasePriorityStart = 20;
inline constexpr std::uint32_t kPhasePriorityEnvelope = 30;
inline constexpr std::uint32_t kPhasePriorityStop = 50;

struct TemplateEvent {
    TemplateEventKind kind = TemplateEventKind::Start;
    std::uint32_t phasePriority = kPhasePriorityStart;
};

// The event template an occurrence contributes, for a later stage to lower to
// a runtime FrameBatch. Events are ordered by phasePriority (the subeventPhase);
// eventIndex is the occurrence's semantic order among its Track's occurrences,
// the value a ScheduledEventInput carries as its eventIndex.
//
// A plain clip emits Start and, for a Sound target, Stop. It emits no
// SetStartCursor — that phase serves a baked entry, a pickup or a same-position
// handoff, and a plain clip has no independent source-in to seed one. Authored
// fades are lowered by production registration without changing this structural
// template. A Container or
// Piece target emits no Stop: its end is its own program's or exit marker's to
// decide. A Stop resolves the natural end against the occurrence's
// durationFrames at resourceSampleRate; the executable quantity is frames, never
// the nanosecond estimate, whose floor round-trip would lose frames.
struct FrameBatchTemplate {
    std::uint32_t eventIndex = 0;
    std::vector<TemplateEvent> events;
};

// One clip occurrence template on a Track. A Clip forms no public instance; it
// contributes this occurrence fact — its resolved Playable reference, its
// normalized time, its fades, its same-frame order key, its resource duration
// facts and its FrameBatch event template.
struct IrOccurrence {
    OccurrenceRef key;
    AuthoringGuid clipGuid{};
    ContentRef content;
    AuthoringGuid contentTargetGuid{};
    std::int64_t time = 0;
    std::int64_t authoredDuration = 0;
    FadeSpec fades;
    // The clip's authored same-frame tiebreak, carried through so the audible
    // ordering is legible on the occurrence and not only implied by its position.
    SemanticOrderKey semanticOrderKey = 0;

    // Occurrence facts derived from the referenced resource. hasDuration is
    // false for a Container or Piece target, whose end is decided by its own
    // program or exit marker rather than a natural length; such an occurrence
    // carries no duration and, later, gets no Stop.
    //
    // For a Sound target, durationFrames at resourceSampleRate is the
    // authoritative natural length — the executable quantity a Stop resolves
    // against at registration. durationNs is a derived floor of that same length
    // (frames * 1e9 / rate) for interval math only; the authoritative value is
    // always the frame pair, never the nanoseconds.
    bool hasDuration = false;
    std::uint64_t durationFrames = 0;
    std::uint32_t resourceSampleRate = 0;
    std::int64_t durationNs = 0;
    bool seekable = false;

    float baseVolumeDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    float basePitchRatio = 1.0f;
    std::uint32_t loopCount = 1;

    // The routing facts a later stage installs when the voice starts. sends is
    // the occurrence's merged send list (SendResolution's per-occurrence result),
    // one entry per distinct target bus in winning order. outputBusResolved and
    // resolvedOutputBus carry the reference-slot resolution of the subject's
    // output bus: when outputBusResolved is true the voice's dry path routes to
    // resolvedOutputBus, otherwise the bus is Unresolved and a later loader
    // supplies the caller default rather than this stage inventing one.
    std::vector<Send> sends;
    bool outputBusResolved = false;
    BusId resolvedOutputBus = kReservedBusId;

    // The subject's own spatial placement, installed when the voice starts.
    // Spatial identity is an object property, not a chain contribution, so these
    // are the subject's authored values carried verbatim — never folded up the
    // chain, unlike the gain/pitch bases. panning supplies the authored pan;
    // spread controls the stereo source width in the spatial matrix. Source
    // channels are preserved until rendering. is3DEnabled fixes the flag at play;
    // attenuationResolved and resolvedAttenuation carry the reference-slot
    // resolution of the subject's Attenuation profile, the same shape as the
    // output bus above: when resolved, resolvedAttenuation names the profile a
    // later stage binds; otherwise none binds and the voice gets geometric pan
    // only.
    float panning = 0.0f;
    float spread = 0.0f;
    bool is3DEnabled = false;
    float attenuationDistanceScaling = 1.0f;
    bool attenuationResolved = false;
    harkenstudio::ObjectId resolvedAttenuation{};

    // The effect chain selected for this occurrence. hasEffectChain false means no
    // object along the chain declared one, so the occurrence runs no effects and
    // effectChainIndex addresses nothing. When true, effectChainIndex indexes the
    // IR's chain table, and the chain it names may legitimately hold zero entries:
    // that is the declared-empty case, an object overriding an inherited chain with
    // no effects, which is a different fact from never selecting a chain at all.
    // The index is an addressing detail and not audible content — the semantic
    // fingerprint covers the chain it dereferences, never this number.
    bool hasEffectChain = false;
    std::uint32_t effectChainIndex = 0;

    FrameBatchTemplate frameBatch;
};

// A folded Track in the IR. A Track is never an instantiable node: it emits no
// instance, bar or Bus entry, only its occurrence template sequence in
// normalized order. BuildPieceIR has already written the folded numeric values
// into each occurrence; installing them when a voice starts happens later.
// Structurally, a Track simply does not appear among instantiable objects, and
// that absence is what this type encodes.
struct IrTrack {
    StableId trackId = kUnmintedStableId;
    AuthoringGuid guid{};
    std::vector<IrOccurrence> occurrences;
};

// A marker in the IR, echoed from the normalized marker table in ascending MarkerId
// order.
struct IrMarker {
    StableId id = kUnmintedStableId;
    AuthoringGuid guid{};
    MarkerId markerId = kReservedMarkerId;
    MarkerKind kind = MarkerKind::Custom;
    std::int64_t position = 0;
};

// A Piece in the IR: its constant-map time spec, its normalized start, its marker
// table in canonical order, and the stable ids of the Tracks it owns.
struct IrPiece {
    StableId pieceId = kUnmintedStableId;
    AuthoringGuid guid{};
    PieceTimeSpec timeSpec;
    StartPointKind startKind = StartPointKind::Entry;
    std::int64_t startCoordinate = 0;
    MarkerId startMarkerId = kReservedMarkerId;
    std::vector<IrMarker> markers;
    std::vector<StableId> tracks;
};

// One row of the required runtime lineage table: the numeric path a Profiler
// uses to place an occurrence on its lane without debug symbols. A Track
// triggered inside a Piece carries the three-level path Piece -> Track -> Clip
// (hasPiece true); a directly triggered Track carries the two-level path
// Track -> Clip (hasPiece false, pieceId unused).
struct LineageEntry {
    bool hasPiece = false;
    StableId pieceId = kUnmintedStableId;
    StableId trackId = kUnmintedStableId;
    StableId clipId = kUnmintedStableId;
    std::uint32_t occurrenceOrdinal = 0;
};

// The required runtime lineage table. Entries are ordered by (trackId,
// occurrenceOrdinal), a total order that does not depend on the order the IR
// happened to be built in.
struct RuntimeLineageTable {
    std::vector<LineageEntry> entries;
};

// The immutable IR for this stage: every Track folded, every Piece, and the
// required lineage table with its own fingerprint.
//
// runtimeLineageFingerprint hashes the lineage table alone. It is kept separate
// from the audible programSemanticFingerprint on purpose: a change that only
// renumbers lineage must move this fingerprint without moving the semantic one.
// This stage computes no semantic fingerprint and holds no field for one, by
// design — that is a later stage's product.
class PieceIR {
public:
    PieceIR(const PieceIR&) = default;
    PieceIR(PieceIR&&) noexcept = default;
    PieceIR& operator=(const PieceIR&) = default;
    PieceIR& operator=(PieceIR&&) noexcept = default;

    const std::vector<IrTrack>& tracks() const { return tracks_; }
    const std::vector<IrPiece>& pieces() const { return pieces_; }
    const RuntimeLineageTable& lineage() const { return lineage_; }
    // The compiled chains this IR's occurrences reference, ascending by declaring
    // owner. Stored once per declaring object: every occurrence that selected the
    // same declaration indexes the same entry rather than carrying a copy.
    const std::vector<CompiledEffectChain>& effectChains() const {
        return effectChains_;
    }
    std::uint64_t runtimeLineageFingerprint() const {
        return runtimeLineageFingerprint_;
    }

private:
    PieceIR(std::vector<IrTrack> tracks, std::vector<IrPiece> pieces,
            RuntimeLineageTable lineage,
            std::vector<CompiledEffectChain> effectChains,
            std::uint64_t runtimeLineageFingerprint)
        : tracks_(std::move(tracks)), pieces_(std::move(pieces)),
          lineage_(std::move(lineage)),
          effectChains_(std::move(effectChains)),
          runtimeLineageFingerprint_(runtimeLineageFingerprint) {}

    friend struct detail::PieceIRBuilder;

    std::vector<IrTrack> tracks_;  // all Tracks, ascending by trackId
    std::vector<IrPiece> pieces_;  // ascending by pieceId
    RuntimeLineageTable lineage_;
    std::vector<CompiledEffectChain> effectChains_;  // ascending by owner
    std::uint64_t runtimeLineageFingerprint_ = 0;
};

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_PIECE_IR_H
