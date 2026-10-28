#ifndef HARKEN_PROJECT_SCHEMA_REFERENCE_RESOLUTION_H
#define HARKEN_PROJECT_SCHEMA_REFERENCE_RESOLUTION_H

#include "harken_project_schema/Diagnostics.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/NormalizedFacts.h"

#include "harken_reference_slots/ReferenceSlotResolver.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The resolved reference slots of one clip occurrence. Per the immediate-
// arrangement scope, an occurrence is one clip placement; the subject is the
// Playable the clip references. slots is indexed by ReferenceSlot ordinal, so
// slots[ReferenceSlot::OutputBus] is that clip's resolved output bus, carrying
// the winning value and the full candidate list (the route explain).
struct OccurrenceReferenceResolution {
    StableId trackId = kUnmintedStableId;
    StableId clipId = kUnmintedStableId;
    std::uint32_t occurrenceOrdinal = 0;
    StableId subjectId = kUnmintedStableId;
    ContentKind subjectKind = ContentKind::Sound;
    std::array<harkenreferenceslots::ReferenceSlotResolution,
               harkenreferenceslots::kReferenceSlotCount>
        resolvedSlots;
};

// The reference-resolution facts of a compiled project: one entry per clip
// occurrence, ordered by (trackId, occurrenceOrdinal) — a total order that does
// not depend on the order the model was built in.
struct ReferenceResolutionFacts {
    std::vector<OccurrenceReferenceResolution> occurrences;
};

// The result of resolving a project's reference slots. On success facts holds
// the per-occurrence resolutions and diagnostics is empty. A chain that runs
// past the depth ceiling is a deterministic refusal: diagnostics carries a
// single ReferenceChainDepthExceeded and facts is absent, all-or-nothing like
// the front-end.
struct ReferenceResolutionResult {
    std::vector<Diagnostic> diagnostics;
    std::optional<ReferenceResolutionFacts> facts;

    bool ok() const { return diagnostics.empty(); }
};

class ReferenceResolutionContext {
public:
    // depthCeiling defaults to the object count plus a fixed margin — comfortably
    // above any acyclic chain, so it never trips on a structurally valid project
    // and only guards against a malformed source.
    explicit ReferenceResolutionContext(const CanonicalProject& project);
    ReferenceResolutionContext(const CanonicalProject& project,
                               std::uint32_t depthCeiling);

    std::array<harkenreferenceslots::ReferenceSlotResolution,
               harkenreferenceslots::kReferenceSlotCount>
    Resolve(StableId subject,
            const std::vector<StableId>& placementChain) const;

    // The placement chain of a clip carried on the Track named by trackId: the
    // Track, then the Segment that owns it, if any.
    std::vector<StableId> PlacementChainForTrack(StableId trackId) const;

    std::uint32_t depthCeiling() const { return depthCeiling_; }

private:
    std::map<StableId,
             std::array<std::optional<harkenstudio::ObjectId>,
                        harkenreferenceslots::kReferenceSlotCount>>
        refValues_;
    std::map<StableId, ContentKind> kinds_;
    std::map<StableId, StableId> parent_;  // child -> its structural owner
    std::uint32_t depthCeiling_ = 0;
};

// Resolves every clip occurrence's reference slots and records the results with
// their route explain. The overload taking an explicit depthCeiling exists so
// the refusal path is testable; the two-argument form uses the context's default
// ceiling.
ReferenceResolutionResult BuildReferenceResolution(
    const CanonicalProject& project, const NormalizedFacts& facts);
ReferenceResolutionResult BuildReferenceResolution(
    const CanonicalProject& project, const NormalizedFacts& facts,
    std::uint32_t depthCeiling);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_REFERENCE_RESOLUTION_H
