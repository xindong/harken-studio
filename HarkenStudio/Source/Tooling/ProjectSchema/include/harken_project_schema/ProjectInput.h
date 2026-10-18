// The lenient authoring input the compile front-end consumes.
//
// This is deliberately looser than the canonical model. An authoring layer — or
// a malformed document, or a mistaken migration — can hand the front-end shapes
// the frozen schema forbids: a Track carrying a time spec, a Piece owning a
// Sound, a marker smuggled in under the reserved id, an ownership edge that closes
// a cycle. Making those expressible is the point: the front-end's contract is
// that it turns a well-formed input into the canonical model and refuses an
// ill-formed one with diagnostics and zero partial output, and a refusal it
// cannot even express is a refusal it cannot test.
//
// So the fields here are a superset. A field that is legal on only one kind
// (clips on a Track, a time spec on a Piece) is present on every node, and the
// parser rejects it where it does not belong. The canonical model in Model.h is
// the narrow, per-kind shape that a successful parse produces.
//
// Ownership is expressed as edges — each node lists the authoring GUIDs of the
// children it claims to own — rather than as a parent pointer on the child.
// That is what lets a single malformed edge stand in for both "a Track nested
// under a Track" and "a Track owning a Track": they are the same edge seen from
// two ends, and there is exactly one place to author it and one rule to reject
// it.
#ifndef HARKEN_PROJECT_SCHEMA_PROJECT_INPUT_H
#define HARKEN_PROJECT_SCHEMA_PROJECT_INPUT_H

#include "harken_project_schema/Model.h"
#include "harken_project_schema/CompileTargetProfile.h"
#include "harken_project_schema/RuntimeIdentityIndex.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace harkenprojectschema {

// A clip as authored, before its reference is resolved to a stable id. The
// target is named by authoring GUID and by the kind the author believes it to
// be; the front-end checks that the GUID resolves and that the resolved kind is
// a legal Playable, so a clip pointing at a Track, a Layer or a missing object
// is caught rather than trusted.
struct InputClip : PlayablePropertyPayloadV1 {
    AuthoringGuid guid{};
    AuthoringGuid targetGuid{};
    std::uint32_t stableOccurrenceOrdinal = 0;
    std::int64_t time = 0;
    std::int64_t duration = 0;
    FadeSpec fades;
    SemanticOrderKey semanticOrderKey = 0;
};

struct InputOwnershipEdge {
    AuthoringGuid childGuid{};

    InputOwnershipEdge() = default;
    InputOwnershipEdge(AuthoringGuid guid) : childGuid(guid) {}
};

struct InputOwnershipEdges : std::vector<InputOwnershipEdge> {
    using std::vector<InputOwnershipEdge>::operator=;
};

struct InputContainerChildOccurrence : PlayablePropertyPayloadV1 {
    AuthoringGuid childGuid{};
    std::uint32_t stableOccurrenceOrdinal = 0;

    InputContainerChildOccurrence() = default;
    InputContainerChildOccurrence(AuthoringGuid guid) : childGuid(guid) {}
};

struct InputContainerChildOccurrences
    : std::vector<InputContainerChildOccurrence> {
    using std::vector<InputContainerChildOccurrence>::operator=;

    bool compatibilityOrdinalOverflow = false;

    InputContainerChildOccurrences& operator=(
        std::initializer_list<AuthoringGuid> guids) {
        clear();
        compatibilityOrdinalOverflow =
            guids.size() > std::numeric_limits<std::uint32_t>::max();
        if (compatibilityOrdinalOverflow) return *this;
        reserve(guids.size());
        std::uint32_t ordinal = 0;
        for (const AuthoringGuid guid : guids) {
            InputContainerChildOccurrence edge(guid);
            edge.stableOccurrenceOrdinal = ordinal++;
            push_back(std::move(edge));
        }
        return *this;
    }

    InputContainerChildOccurrences& operator=(
        std::vector<AuthoringGuid>&& guids) {
        clear();
        compatibilityOrdinalOverflow =
            guids.size() > std::numeric_limits<std::uint32_t>::max();
        if (compatibilityOrdinalOverflow) return *this;
        reserve(guids.size());
        std::uint32_t ordinal = 0;
        for (const AuthoringGuid guid : guids) {
            InputContainerChildOccurrence edge(guid);
            edge.stableOccurrenceOrdinal = ordinal++;
            push_back(std::move(edge));
        }
        return *this;
    }
};

// A marker as authored. markerId zero is expressible on purpose: it is exactly the
// "entry smuggled into the marker table" case the front-end must refuse, and an
// input that could not spell it could not test the refusal.
struct InputMarker {
    AuthoringGuid guid{};
    MarkerId markerId = kReservedMarkerId;
    MarkerKind kind = MarkerKind::Custom;
    std::int64_t position = 0;
};

// A Piece's authored time spec. Carried as its own optional so a node that has
// no business owning one (anything but a Piece) can still be handed one, and be
// refused for it.
struct InputPieceTimeSpec {
    PieceTimeKind kind = PieceTimeKind::Absolute;
    std::vector<TempoMapEntry> tempoMap;
    std::vector<MeterMapEntry> meterMap;
};

// A Piece's authored start point.
struct InputStartPoint {
    StartPointKind kind = StartPointKind::Entry;
    MarkerId markerId = kReservedMarkerId;
};

// One authored content node. The kind decides which of the optional fields are
// legal; all are present so an illegal combination can be authored and refused.
//
//   persistedStableId  - the runtime id minted on a previous load, if this node
//                        has been seen before. Absent means the registry mints a
//                        fresh id. Two nodes claiming the same persisted id is a
//                        conflict, not a merge.
//   volume..signalGraph - the common chassis fields, legal on every kind because
//                        the chassis is shared by all five. They are passed
//                        through unchanged; the only rules the front-end enforces
//                        over them are that a send may not carry the reserved bus
//                        id 0 and that the effect graph is a structurally legal
//                        DAG.
//   ownedChildren      - structural ownership edges, by child authoring GUID.
//   clips              - placement ledger; legal only on a Track.
//   timeSpec/startPoint/markers - time-side authoring; legal only on a Piece.
struct InputNode : PlayablePropertyPayloadV1 {
    ContentKind kind = ContentKind::Sound;
    AuthoringGuid guid{};
    std::int64_t editorOrder = 0;

    // The common chassis fields, mirroring Chassis: the mixing and playback
    // scalars, the send list, the reference-slot overrides (output bus and
    // Attenuation inside spatial), the spatial-placement group and the signal
    // graph. Present on every node because every kind shares the chassis;
    // defaults match the canonical model's.
    bool hasSignalGraph = false;

    InputOwnershipEdges ownedChildren;
    InputContainerChildOccurrences containerChildOccurrences;

    std::vector<InputClip> clips;

    std::optional<InputPieceTimeSpec> timeSpec;
    std::optional<InputStartPoint> startPoint;
    std::vector<InputMarker> markers;
};

// A whole authored project. Nodes are a flat list; the structural tree is
// recovered from the ownership edges. roots names the top-level nodes — the ones
// filed under nothing — which is how a Track that exists standalone is
// distinguished from one owned by a Layer.
//
// The node order and the root order are the authored order and are honoured as
// given: the front-end processes nodes in this order, so a fixture controls the
// order its diagnostics come out in without depending on any container's
// iteration order.
struct ProjectInput {
    std::vector<InputNode> nodes;
    std::vector<AuthoringGuid> roots;
    harkenstudio::ObjectId projectId{};

    // One immutable project-wide identity view may be shared by every
    // compiler consuming this input. It is ephemeral and never serialized.
    std::shared_ptr<const RuntimeIdentityIndexV1> identityIndex;

    // The immutable target profile selected for this project generation.
    // CurrentWorld and Bus compilation retain the same pointer instance.
    std::shared_ptr<const CompileTargetProfileV1> compileTargetProfile;
};

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_PROJECT_INPUT_H
