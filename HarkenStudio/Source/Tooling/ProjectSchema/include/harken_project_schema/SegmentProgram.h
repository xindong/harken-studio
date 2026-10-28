// The immutable Project v6 program: the compiler's final assembled value for
// this library's scope.
//
// A SegmentProgram bundles a built SegmentIR (its folded Tracks, Segments, occurrence
// templates and required RuntimeLineageTable) with the active-capacity estimate,
// the capability prerequisites it needs a runtime to satisfy, and a program
// semantic fingerprint. It is assembled once by BuildSegmentProgram and never
// mutated, the same type-level immutability SegmentIR uses.
//
// -- programSemanticFingerprint -------------------------------------------
//
// An FNV-1a 64-bit hash over the program identities and audible content. The
// honest field coverage keeps a partial hash from being mistaken for a whole:
//
//   INCLUDED, per Segment (in AuthoringGuid order):
//     - the Segment AuthoringGuid
//     - time spec: kind, every tempo row (tick, microsPerQuarter), every meter
//       row (tick, numerator, denominator)
//     - start: kind, resolved coordinate, marker id
//     - marker table: each marker's AuthoringGuid, markerId, kind and position
//     - the set of Tracks it owns, by Track AuthoringGuid
//   INCLUDED, per Track (in AuthoringGuid order), and per occurrence within it
//   (in normalized order):
//     - the Track AuthoringGuid and occurrence Clip AuthoringGuid
//     - content kind and content target AuthoringGuid
//     - time, fade-in and fade-out lengths, semanticOrderKey
//     - whether it has a natural duration, and the authoritative durationFrames
//       and resourceSampleRate pair
//     - the FrameBatch template: eventIndex and each event's phasePriority and
//       kind
//     - the folded start-time base values: baseVolumeDb and basePitchRatio (as
//       their raw float bits) and the loop count. These are the audible gain,
//       pitch and looping a voice starts with, so a change to them changes the
//       rendered sound and must move this hash
//     - the routing facts: the merged send list (count, then each send's target
//       bus id and level as raw float bits) and the resolved output bus (a
//       resolved flag, then the bus id). Where a sound is routed is program
//       semantics, so a change to it moves this hash. The bus ids are hashed as
//       their raw values, which is not the excluded raw stable id below: a bus id
//       is an authoring-side external identity this compiler consumes and never
//       mints, so there is no minted numbering here to renumber, and the value
//       itself is the identity that must be pinned
//     - the selected effect chain: a selected flag, then — when selected — the
//       entry count and, per entry in execution order, the effect type, the entry
//       flags, and the entry's parameter payload in declared order (each param id
//       with its value as raw bits). Which effects run, in what order, with what
//       values is what a voice sounds like. The chain's contents are hashed
//       through the reference, never the reference itself: the chain index and the
//       parameter-block index are storage addressing, so renumbering identical
//       chains or blocks leaves this hash unchanged
//
//   EXCLUDED:
//     - an effect entry's authored stableNodeId — the same lineage-not-sound rule
//       as raw stable ids below: renumbering authored effect nodes must move
//       lineage, not this hash
//     - physical effect slot numbers — a slot assignment belongs to whichever
//       backend lowers the chain, and one chain lowered onto two slot layouts is
//       one program. This stage does not compute a slot number at all
//     - the RuntimeLineageTable and runtimeLineageFingerprint — lineage is a
//       Profiler aid, not program semantics; a change confined to it must not
//       move this hash
//     - raw stable ids — identities are hashed as their authored 128-bit GUIDs.
//       A renumbering that preserves the relative ClipId order leaves this hash
//       unchanged while it changes the lineage fingerprint. Swapping ClipIds at
//       the same time and semanticOrderKey changes the frozen final ordering key,
//       so that execution-order change intentionally moves this hash.
//     - editorOrder — display-only, and not carried in the IR in the first place
//     - durationNs — a derived floor; the authoritative frames-at-rate pair is
//       hashed instead, so the estimate's rounding never feeds the identity
//     - seekable — no SetStartCursor or compatibility decision consumes it yet,
//       so it does not alter output; include it when either decision uses it
//
// Recompiling the same authored input yields the same programSemanticFingerprint;
// changing only the minted numbering while preserving relative ClipId order
// changes runtimeLineageFingerprint but not this one; changing an included
// identity, ordering key or audible field changes this one.
#ifndef HARKEN_PROJECT_SCHEMA_SEGMENT_PROGRAM_H
#define HARKEN_PROJECT_SCHEMA_SEGMENT_PROGRAM_H

#include "harken_project_schema/CapacityEstimate.h"
#include "harken_project_schema/SegmentIR.h"

#include <cstdint>
#include <utility>

namespace harkenprojectschema {

// The runtime capabilities a program requires. This is the minimal set of
// capabilities that actually exist in this vintage's tree — expressed as "what
// is needed", never a version number. Capabilities this vintage does not model
// (Variable, Transition, streaming) are deliberately absent from the taxonomy
// rather than present-and-unset: a program cannot require what the compiler
// cannot yet produce, and inventing bits for them would pre-commit a shape.
enum class ProgramCapability : std::uint32_t {
    MusicalTiming = 0,          // a Segment measured in musical time (constant tempo/meter map)
    AbsoluteTiming = 1,         // a Segment in absolute time, or a directly triggered Track
    NaturalLengthPlayback = 2,  // a clip taking a Sound's natural length (use-content)
    ContainerPlacement = 3,     // a clip placing a Container
    NestedSegmentPlacement = 4,   // a clip placing a Segment
    VoiceEffectChain = 5,       // an occurrence selected a chain that has effects to run
};

inline constexpr std::uint32_t kProgramCapabilityCount = 6;

// The prerequisites as a bitset: bit i is set iff ProgramCapability i is
// required.
struct CapabilityPrerequisites {
    std::uint32_t requiredMask = 0;

    bool Requires(ProgramCapability capability) const {
        return (requiredMask >> static_cast<std::uint32_t>(capability)) & 1u;
    }
};

namespace detail {
struct SegmentProgramBuilder;
}

class SegmentProgram {
public:
    SegmentProgram(const SegmentProgram&) = default;
    SegmentProgram(SegmentProgram&&) noexcept = default;
    SegmentProgram& operator=(const SegmentProgram&) = default;
    SegmentProgram& operator=(SegmentProgram&&) noexcept = default;

    const SegmentIR& ir() const { return ir_; }
    const CapacityEstimate& capacityEstimate() const { return estimate_; }
    const CapabilityPrerequisites& capabilities() const { return capabilities_; }
    std::uint64_t programSemanticFingerprint() const {
        return programSemanticFingerprint_;
    }
    // A convenience passthrough: the lineage fingerprint lives on the IR and is
    // deliberately distinct from the semantic one.
    std::uint64_t runtimeLineageFingerprint() const {
        return ir_.runtimeLineageFingerprint();
    }

private:
    SegmentProgram(SegmentIR ir, CapacityEstimate estimate,
                 CapabilityPrerequisites capabilities,
                 std::uint64_t programSemanticFingerprint)
        : ir_(std::move(ir)), estimate_(std::move(estimate)),
          capabilities_(capabilities),
          programSemanticFingerprint_(programSemanticFingerprint) {}

    friend struct detail::SegmentProgramBuilder;

    SegmentIR ir_;
    CapacityEstimate estimate_;
    CapabilityPrerequisites capabilities_;
    std::uint64_t programSemanticFingerprint_ = 0;
};

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_SEGMENT_PROGRAM_H
