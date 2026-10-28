// Program registration: the compile/load-time step that turns a compiled
// SegmentProgram into device-rate scheduled work the runtime files verbatim.
//
// A SegmentProgram carries authored positions in a segment's own domain -- musical
// ticks under a tempo map for a Musical segment, nanoseconds for an Absolute one --
// and natural-end lengths as frame counts at each resource's own sample rate.
// None of that is a mix-frame yet. Registration is where a device sample rate and
// a session placement enter: every authored position is resolved to an absolute
// execute frame through the segment's ClockKernel, and every natural-end length is
// rescaled from its resource rate to the device rate by exact integer division
// (never through a nanosecond intermediate, which would floor twice and drift).
//
// This is a tooling responsibility, not a runtime one. The library links
// harken_core (for the clock, marker index and the planner's ProgramView) and
// harken_project_schema (for the SegmentProgram it consumes), the same tooling ->
// runtime direction harken_remote already takes; harken_core links neither back.
// The runtime consumes the finished result -- a set of ProgramViews whose events
// are already absolute-frame offsets -- through the planner it already has, and
// re-derives nothing, exactly as it consumes a resolved reference slot rather than
// walking the ancestor chain itself.
//
// The result owns everything a ProgramView borrows: the event arrays, and the
// per-segment ClockKernel and MarkerIndex a view points at for boundary realignment.
// Because a deferred play makes the planner retain a ProgramView by pointer, a
// ResolvedProgram is fixed in memory once built (non-copyable, non-movable) and
// handed out by unique_ptr; every pointer it hands the planner stays valid for its
// lifetime.
#ifndef HARKEN_PROGRAM_REGISTRATION_H
#define HARKEN_PROGRAM_REGISTRATION_H

#include <harken/ObjectId.h>
#include "harken/core/ClockKernel.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/MarkerIndex.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/TimeCoordinates.h"
#include "harken/core/TimelinePlanner.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/TimelineRuntimeInstall.h"

#include "harken_program_registration/VoiceStartInstall.h"

#include "harken_project_schema/SegmentProgram.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace harkenprogramreg {

// The lineage of one resolved occurrence, carried so a consumer can attribute a
// scheduled transaction back to the authored clip it came from. Plain integers on
// the runtime side: the compile-time OccurrenceKey half (clipId, occurrenceOrdinal)
// plus the owning track and segment. The runtime OccurrenceKey's other half
// (arranging instance, loop iteration) is a runtime value and is not part of the
// compiled program.
struct OccurrenceLineage {
    bool hasSegment = false;
    VnObjectId segmentId{};
    VnObjectId trackId{};
    VnObjectId clipId{};
    std::uint32_t occurrenceOrdinal = 0;
    // The clip's content target -- the subject the occurrence plays or expands.
    // The path above locates the occurrence; this says what it points at, which
    // is what a consumer needs to attribute a refused or skipped occurrence to an
    // authored object rather than only to a position on a track.
    VnObjectId subjectId{};
};

// One parameter's accepted value range as this session resolved it — the concrete
// bounds the engine's capability capture reports, never a registry sentinel (the
// capture resolves an open or rate-dependent bound to a finite value before it
// crosses the API). Holding them here is what lets a value be refused while a
// program is being registered rather than at the moment a voice starts.
struct EffectParameterRange {
    float clampMin = 0.0f;
    float clampMax = 0.0f;
};

// One registered effect type as the session resolved it.
//
// poolCapacity is how many instances of the type the session preallocated.
// hostsVoiceInserts says the type may run in a per-voice slot at all — a type
// that only hosts on a bus cannot serve a voice chain, and saying so here turns
// that into a named refusal. parameters is the type's private parameter index
// space in index order, one entry per index, so its size is the parameter count
// and entry i carries index i's accepted range; keeping the count and the ranges
// as one thing removes the chance of their disagreeing.
//
// descriptorSize and descriptorVintage are the type's captured descriptor facts.
// They exist here because building the descriptor the installation seam takes
// requires knowing its layout, and a layout is only safe to assume when the type
// says which one it has: registration checks the size relation and the vintage
// before any descriptor is written and refuses the type otherwise.
struct EffectTypeAvailability {
    std::uint32_t poolCapacity = 0;
    std::uint32_t descriptorSize = 0;
    std::uint32_t descriptorVintage = 0;
    bool hostsVoiceInserts = false;
    std::vector<EffectParameterRange> parameters;
};

// What the session supplies at registration time; none of it lives in the
// compiled program. deviceSampleRateHz is the rate every position resolves to.
// entryAnchorFrame is the absolute mix frame the target segment's entry (tick 0 /
// nanosecond 0) lands on -- a segment placed later on the timeline moves this, the
// event offsets are unchanged. rootSegmentId names the single segment whose owned
// tracks are registered; a clip inside one of those tracks that points at a
// container or a nested segment becomes a start-only placeholder and is not expanded
// here (that expansion happens when the nested content is itself triggered).
struct RegistrationParams {
    std::int64_t deviceSampleRateHz = 0;
    std::uint64_t entryAnchorFrame = 0;
    VnObjectId rootSegmentId{};
    harken::PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerInstanceScheduleEpoch = 0;

    // The resolved per-voice send-slot count for this session, the authoritative
    // second capacity gate. The compiler already refused any occurrence past the
    // POD-layout bound; an occurrence whose merged sends exceed this session's
    // resolved count is refused here. It is a session value, not a program one.
    std::uint32_t resolvedVoiceSendSlots = 0;

    // Maps a schema bus id to the engine bus id the session created for it. Every
    // resolved output bus and send target is looked up here; a bus the map does
    // not name is a deterministic refusal. Registration consumes this map and
    // never builds it — the session that created the buses supplies it.
    std::map<VnObjectId, VnBusId> busIdMap;

    // Maps a subject's schema id to the engine sound id the session registered
    // for it. Every occurrence whose content target is a Sound resolves its sound
    // here, so an unregistered subject is a named registration refusal instead of
    // a refused play later; an occurrence targeting a Container or a Segment starts
    // no voice, needs no sound, and is not looked up. Registration consumes this
    // map and never builds it -- the session that registered the resident PCM or
    // the stream is the only party that knows the engine id.
    std::map<VnObjectId, VnSoundId> soundIdMap;

    // The caller's default output bus for an occurrence whose bus is unresolved,
    // or 0 for none. A preview loader passes its master bus here so an unresolved
    // routing plays somewhere audible; the production pipeline passes 0 and leaves
    // an unresolved bus unresolved for a later link stage. Applying the default is
    // recorded on the install (outputBusDefaulted), never silent.
    VnBusId callerDefaultOutputBus{};

    std::map<VnObjectId, VnAttenuationId> attenuationIdMap;

    // The resolved per-voice insert-effect slot count for this session — the
    // budget an occurrence's chain must fit. Like the send-slot count above it is
    // a session value and may sit below the layout ceiling the compiler already
    // refused past, so a chain that lowered successfully can still be refused
    // here. Zero means this session hosts no per-voice effects at all, which
    // refuses any occurrence that selected a chain with entries in it.
    std::uint32_t resolvedVoiceFxSlots = 0;

    // Maps a compiled effect type to what the session registered for it. The key
    // is the schema's effect type id, which the schema states the engine's effect
    // registry owns — the compiler stores that id without interpreting it, and this
    // map is where it becomes an engine type. A demanded type this map does not
    // name is a deterministic refusal rather than an install-time surprise.
    // Registration consumes the map and never queries the engine: the session that
    // initialized the pools is the only party that knows its resolved numbers.
    std::map<std::uint32_t, EffectTypeAvailability> effectTypes;
};

// Why a registration produced no program. One code per structural refusal, so a
// caller attributes a failure without guessing.
enum class RegistrationRefusal : std::uint32_t {
    None = 0,
    InvalidProgram = 1,       // the SegmentProgram embeds no usable IR
    InvalidDeviceRate = 2,    // device rate outside the clock's accepted domain
    RootSegmentNotFound = 3,    // rootSegmentId names no segment in the program
    ClockResetFailed = 4,     // a segment's tempo map is outside the clock's domain
    MarkerIndexResetFailed = 5,  // a segment's meter or markers are inconsistent
    TransactionEventsEmpty = 6, // an occurrence resolved to no events
    SendCountExceedsResolvedSlots = 7, // more merged sends than the session's resolved slots
    BusIdUnmapped = 8,        // a resolved bus or send target has no engine bus id
    AttenuationTargetUnmapped = 9, // a resolved Attenuation target has no engine profile id
    EffectChainReferenceInvalid = 11, // an occurrence's chain index is outside the program's chain table
    EffectChainExceedsResolvedSlots = 12, // a chain is longer than the session's resolved effect slots
    EffectTypeUnavailable = 13,      // a chain demands an effect type this session did not register
    EffectTypeCannotHostVoice = 14,  // a demanded effect type does not run in a per-voice slot
    EffectTypeParameterUnknown = 15, // a chain authors a parameter id past the type's parameter count
    EffectTypePoolInsufficient = 16, // a chain needs more instances of a type than its pool holds
    EffectTypeParameterSetIncomplete = 17, // an entry does not author every parameter index exactly once
    EffectTypeParameterValueOutOfRange = 18, // an authored value falls outside the parameter's resolved range
    EffectTypeDescriptorLayoutUnsupported = 19, // the type's descriptor size is not the header plus one float per parameter
    EffectTypeDescriptorVintageUnsupported = 20, // the type reports a descriptor layout vintage this build cannot write
    EffectTypeDescriptorTooLarge = 21, // the type's descriptor exceeds the writer's buffer
    SoundTargetUnmapped = 22, // an occurrence's Sound subject has no engine sound id
    InvalidFadeSpec = 23
};

// Non-fatal registration observations. Values are append-only: existing numeric
// codes are never renumbered and new codes are appended at the end.
enum class RegistrationNoteCode : std::uint32_t {
    None = 0,
    AttenuationUnresolvedNoBinding = 1,
    // The occurrence selected a chain that declares no effects. Not a failure: it
    // is an object deliberately overriding an inherited chain with nothing, and
    // saying so distinguishes it from an occurrence that inherited a chain and
    // from one that never had a declaration to inherit.
    EffectChainSelectedEmpty = 4,
};

struct RegistrationNote {
    RegistrationNoteCode code = RegistrationNoteCode::None;
    OccurrenceLineage lineage;
    VnObjectId relatedId{};
};

// Forward declaration so ResolvedProgram can befriend the one function allowed to
// populate it; the full definition follows the class (it owns a ResolvedProgram).
struct RegistrationResult;

// One occurrence resolved to absolute-frame events, with the metadata to build its
// ProgramView and to attribute it. Owned by the ResolvedProgram; the ProgramView
// handed to the planner points into this.
struct ResolvedOccurrence {
    OccurrenceLineage lineage;
    VoiceStartInstall install;
    std::vector<harken::EventTemplate> events;
    std::vector<harken::EventTemplate> timelineEvents;
    harken::TimelineSemanticTarget timelineTarget{};
    std::uint64_t entryAnchorFrame = 0;
    harken::TransactionId transactionId = 0;
    harken::SegmentTimeType timeType = harken::SegmentTimeType::Absolute;
    // Index into the ResolvedProgram's clock table; the ProgramView's clock and
    // marker index are taken from there. kNoClock when the occurrence carries no
    // boundary grid.
    std::size_t clockIndex = 0;
    bool hasClock = false;
};

// A compiled program resolved to device-rate scheduled work. Non-copyable and
// non-movable: it owns storage a ProgramView borrows by pointer (the event arrays,
// and the ClockKernel/MarkerIndex a view aligns boundaries against), and a deferred
// play makes the planner keep a ProgramView pointer, so the object must not move.
class ResolvedProgram {
public:
    ResolvedProgram() = default;
    ResolvedProgram(const ResolvedProgram&) = delete;
    ResolvedProgram& operator=(const ResolvedProgram&) = delete;
    ResolvedProgram(ResolvedProgram&&) = delete;
    ResolvedProgram& operator=(ResolvedProgram&&) = delete;

    std::size_t OccurrenceCount() const { return views_.size(); }

    // A ProgramView for occurrence i, stable for this program's lifetime. Safe to
    // pass to TimelinePlanner::PostPlay and to retain across a deferral.
    const harken::ProgramView& View(std::size_t i) const { return views_[i]; }

    const OccurrenceLineage& Lineage(std::size_t i) const {
        return occurrences_[i].lineage;
    }
    const VoiceStartInstall& Install(std::size_t i) const {
        return occurrences_[i].install;
    }
    std::size_t TimelineOccurrenceCount() const {
        return timelineOccurrenceToResolved_.size();
    }
    std::size_t TimelineResolvedOccurrenceIndex(std::size_t i) const {
        return timelineOccurrenceToResolved_[i];
    }
    const harken::ProgramView& TimelineView(std::size_t i) const {
        return timelineViews_[i];
    }
    const harken::TimelineProgramView& TimelineProgram() const {
        return timelineProgram_;
    }
    std::size_t LogicalOnlyOccurrenceCount() const {
        return logicalOnlyOccurrences_.size();
    }
    const harken::LogicalOnlyOccurrence& LogicalOnlyOccurrenceAt(
        std::size_t i) const {
        return logicalOnlyOccurrences_[i];
    }
    harken::TimelineRuntimeInstallResult InstallTimeline() const noexcept;
    harken::TransactionId TransactionId(std::size_t i) const {
        return occurrences_[i].transactionId;
    }

    // The single logic commit sequence stamped into every resolved event of this
    // registration -- one authored load is one commit.
    harken::LogicCommitSequence CommitSequence() const { return commitSequence_; }

    // The session's registered per-type effect pool capacities, copied in at
    // registration. Carried on the program because a consumer accounting for the
    // program's effect demand runs after registration, when the params are gone;
    // the session that resolved the pools is the only party that ever knew them.
    // Keys are engine effect types. Every type a chain in this program demands is
    // present -- a chain demanding an unregistered type refused the registration.
    const std::map<std::uint32_t, std::uint32_t>& EffectTypeCapacities() const {
        return effectTypeCapacities_;
    }

private:
    friend RegistrationResult RegisterSegmentProgram(
        const harkenprojectschema::SegmentProgram& program,
        const RegistrationParams& params, harken::LogicCommitCoordinator& commit);

    // Per-segment conversion context a ProgramView borrows. Held in a deque so its
    // address, and the address of the tempo segments the ClockKernel borrows, are
    // stable as more segments are appended.
    struct ClockContext {
        std::vector<harken::TempoSegment> segments;
        harken::ClockKernel kernel;
        harken::MarkerIndex markerIndex;
        bool hasMarkerIndex = false;
    };

    std::deque<ClockContext> clocks_;
    std::deque<ResolvedOccurrence> occurrences_;
    std::vector<harken::ProgramView> views_;
    std::vector<harken::ProgramView> timelineViews_;
    std::vector<std::size_t> timelineOccurrenceToResolved_;
    std::vector<harken::LogicalOnlyOccurrence> logicalOnlyOccurrences_;
    std::vector<harken::TimelineEventPayload> timelinePayloads_;
    harken::TimelineProgramView timelineProgram_{};
    harken::LogicCommitSequence commitSequence_ = 0;
    std::map<std::uint32_t, std::uint32_t> effectTypeCapacities_;
};

// The numbers behind a refusal that is about a mismatch. A code alone says which
// rule broke; these say what the rule wanted and what it got, which is the
// difference between a caller knowing an effect type was rejected and knowing that
// the type declared a 40-byte descriptor where its parameter count implies 36.
// effectType names the type the refusal is about; expected and actual are read in
// terms of the code and are both zero for a refusal that carries no such pair.
struct RegistrationRefusalContext {
    std::uint32_t effectType = 0;
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;
};

// The outcome of a registration. On success program is non-null, owns the
// resolved work, and notes carries queryable non-fatal observations. On failure
// program and notes are empty, refusal says why, and refusalContext carries the
// numbers behind it when the refusal is a mismatch.
struct RegistrationResult {
    bool ok = false;
    RegistrationRefusal refusal = RegistrationRefusal::None;
    RegistrationRefusalContext refusalContext;
    std::unique_ptr<ResolvedProgram> program;
    std::vector<RegistrationNote> notes;
};

// Resolves the named root segment of a compiled program to device-rate scheduled
// work. Issues one commit sequence from the coordinator and stamps it into every
// event. Deterministic: the same program, params and coordinator state always
// produce bit-identical frames on any toolchain (pure integer conversion, no
// floating point).
RegistrationResult RegisterSegmentProgram(
    const harkenprojectschema::SegmentProgram& program,
    const RegistrationParams& params, harken::LogicCommitCoordinator& commit);

// Exact floor(frameCount * toRateHz / fromRateHz) for a natural-end length crossing
// from a resource's sample rate to the device rate. Non-negative inputs, fromRateHz
// positive. A portable 128-bit intermediate preserves the exact quotient; only a
// quotient outside uint64 is saturated to UINT64_MAX rather than wrapped. Exposed
// because it is the one place the resource -> device rescale is defined and is
// worth pinning on its own.
std::uint64_t RescaleFrameCountFloor(std::uint64_t frameCount,
                                     std::uint64_t fromRateHz,
                                     std::uint64_t toRateHz);
bool TryRescaleFrameCountFloor(std::uint64_t frameCount,
                               std::uint64_t fromRateHz,
                               std::uint64_t toRateHz,
                               std::uint64_t* outFrames) noexcept;
bool TryMaterializeSchedulePosition(harken::ScheduleTimeKindV1 domain,
                                    std::int64_t authoredPosition,
                                    std::int64_t microsPerQuarter,
                                    std::uint64_t deviceRateHz,
                                    std::int64_t* outFrame) noexcept;
bool TryMaterializeScheduleSpan(harken::ScheduleTimeKindV1 domain,
                                std::int64_t authoredStart,
                                std::int64_t authoredLength,
                                std::int64_t microsPerQuarter,
                                std::uint64_t deviceRateHz,
                                std::uint64_t* outFrames) noexcept;
bool TryLowerScheduleFades(harken::ScheduleTimeKindV1 domain,
                           std::int64_t authoredStart,
                           std::int64_t authoredFadeIn,
                           std::int64_t authoredFadeOut,
                           std::int64_t microsPerQuarter,
                           std::uint64_t deviceRateHz,
                           std::uint64_t audibleSpanFrames,
                           std::uint64_t* outFadeInFrames,
                           std::uint64_t* outFadeOutFrames) noexcept;

} // namespace harkenprogramreg

#endif // HARKEN_PROGRAM_REGISTRATION_H
