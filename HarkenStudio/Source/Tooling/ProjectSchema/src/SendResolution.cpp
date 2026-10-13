#include "harken_project_schema/SendResolution.h"

#include <harken_studio_build/BuildLimits.h>

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace harkenprojectschema {
namespace {

// One clip's placement facts, gathered from the model so occurrences can be
// enumerated in the normalized order the facts already fix.
struct ClipPlacement {
    StableId trackId = kUnmintedStableId;
    ContentRef content;
};

}  // namespace

SendResolutionResult BuildSendResolution(const CanonicalProject& project,
                                         const NormalizedFacts& facts) {
    SendResolutionResult result;

    std::map<StableId, const std::vector<Send>*> sendsById;
    std::map<StableId, AuthoringGuid> guidById;
    std::map<StableId, ClipPlacement> clipPlacement;
    for (const ContentObject& obj : project.objects) {
        const Chassis& c = obj.chassis();
        sendsById.emplace(c.id, &c.sends);
        guidById.emplace(c.id, c.guid);
        if (obj.kind == ContentKind::Track) {
            for (const Clip& clip : obj.track.clips) {
                clipPlacement.emplace(
                    clip.id, ClipPlacement{obj.track.chassis.id, clip.contentRef});
            }
        }
    }

    SendResolutionFacts out;
    // facts.tracks is ordered by ascending trackId; clipsByOrder is the
    // normalized audio order. Iterating both yields occurrences in
    // (trackId, occurrenceOrdinal) order without a further sort.
    for (const TrackNormalization& track : facts.tracks) {
        for (std::size_t ordinal = 0; ordinal < track.clipsByOrder.size();
             ++ordinal) {
            const StableId clipId = track.clipsByOrder[ordinal];
            const auto placementIt = clipPlacement.find(clipId);
            if (placementIt == clipPlacement.end()) {
                continue;  // a clip the model does not define is impossible here
            }
            const StableId subject = placementIt->second.content.target;

            const auto sendsIt = sendsById.find(subject);
            if (sendsIt == sendsById.end()) continue;
            const auto& merged = *sendsIt->second;

            if (merged.size() > harkenstudiobuild::kMaxVoiceSendSlots) {
                // More distinct target buses than the per-voice send array can
                // hold. Refuse loudly, all-or-nothing, rather than truncate.
                Diagnostic d;
                d.code = DiagnosticCode::SendCountExceedsLayoutLimit;
                const auto guidIt = guidById.find(subject);
                if (guidIt != guidById.end()) {
                    d.primary = guidIt->second;
                }
                d.detail = merged.size();
                result.diagnostics.push_back(d);
                return result;
            }

            OccurrenceSendResolution occ;
            occ.trackId = track.track;
            occ.clipId = clipId;
            occ.occurrenceOrdinal = static_cast<std::uint32_t>(ordinal);
            occ.subjectId = subject;
            occ.sends = merged;
            out.occurrences.push_back(std::move(occ));
        }
    }

    result.facts = std::move(out);
    return result;
}

}  // namespace harkenprojectschema
