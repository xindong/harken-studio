#ifndef HARKEN_PROJECT_SCHEMA_SEND_RESOLUTION_H
#define HARKEN_PROJECT_SCHEMA_SEND_RESOLUTION_H

#include "harken_project_schema/Diagnostics.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/NormalizedFacts.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace harkenprojectschema {

// The merged sends of one clip occurrence, in the order the winning entries were
// first seen along the chain. sends holds at most kMaxVoiceSendSlots entries, one
// per distinct target bus.
struct OccurrenceSendResolution {
    StableId trackId = kUnmintedStableId;
    StableId clipId = kUnmintedStableId;
    std::uint32_t occurrenceOrdinal = 0;
    StableId subjectId = kUnmintedStableId;
    std::vector<Send> sends;
};

// The send-resolution facts of a compiled project: one entry per clip
// occurrence, ordered by (trackId, occurrenceOrdinal) — a total order that does
// not depend on the order the model was built in.
struct SendResolutionFacts {
    std::vector<OccurrenceSendResolution> occurrences;
};

// The result of merging a project's sends. On success facts holds the
// per-occurrence merged sends and diagnostics is empty. An occurrence whose
// merged sends exceed the layout bound is a deterministic refusal: diagnostics
// carries a single SendCountExceedsLayoutLimit and facts is absent,
// all-or-nothing like the front-end.
struct SendResolutionResult {
    std::vector<Diagnostic> diagnostics;
    std::optional<SendResolutionFacts> facts;

    bool ok() const { return diagnostics.empty(); }
};

// Merges every clip occurrence's sends along the fold chain and records the
// result. The project must be one the front-end has already validated, so every
// send has a legal target bus and the chain is acyclic.
SendResolutionResult BuildSendResolution(const CanonicalProject& project,
                                         const NormalizedFacts& facts);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_SEND_RESOLUTION_H
