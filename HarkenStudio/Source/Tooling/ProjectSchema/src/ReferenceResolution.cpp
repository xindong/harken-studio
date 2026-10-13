#include "harken_project_schema/ReferenceResolution.h"

#include <cstddef>
#include <utility>

namespace harkenprojectschema {
namespace {

using harkenreferenceslots::kReferenceSlotCount;
using harkenreferenceslots::ReferenceSlot;
using harkenreferenceslots::ReferenceSlotResolution;
using harkenreferenceslots::ReferenceSlotSource;
using harkenreferenceslots::ResolutionOutcome;
using harkenreferenceslots::ResolveReferenceSlot;

// A margin above the object count so the depth ceiling comfortably clears any
// acyclic chain and only ever guards against a malformed source.
constexpr std::uint32_t kDepthMargin = 16;

struct ResolveCtx {
    const std::map<StableId,
                   std::array<std::optional<harkenstudio::ObjectId>,
                              kReferenceSlotCount>>* refValues;
};

bool SlotValueCallback(void* ctx, harkenstudio::ObjectId objectId, ReferenceSlot slot,
                       harkenstudio::ObjectId* outValue) {
    const auto* c = static_cast<const ResolveCtx*>(ctx);
    const auto it = c->refValues->find(objectId);
    if (it == c->refValues->end()) {
        return false;
    }
    const auto& value =
        it->second[static_cast<std::size_t>(slot)];
    if (!value.has_value()) {
        return false;
    }
    *outValue = *value;
    return true;
}

}  // namespace

ReferenceResolutionContext::ReferenceResolutionContext(
    const CanonicalProject& project)
    : ReferenceResolutionContext(
          project,
          static_cast<std::uint32_t>(project.objects.size()) + kDepthMargin) {}

ReferenceResolutionContext::ReferenceResolutionContext(
    const CanonicalProject& project, std::uint32_t depthCeiling)
    : depthCeiling_(depthCeiling) {
    for (const ContentObject& obj : project.objects) {
        const Chassis& c = obj.chassis();
        std::array<std::optional<harkenstudio::ObjectId>, kReferenceSlotCount> values{};
        if (c.mainOutputBusRef != harkenstudio::ObjectId{}) {
            values[static_cast<std::size_t>(ReferenceSlot::OutputBus)] =
                c.mainOutputBusRef;
        }
        if (c.presence.Has(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef)) {
            values[static_cast<std::size_t>(ReferenceSlot::Attenuation)] =
                c.spatial.attenuationRef;
        }
        refValues_.emplace(c.id, values);
        kinds_.emplace(c.id, obj.kind);

        // Invert the downward ownership edges into the child -> owner map. Every
        // owner kind lists its structural children the same way; a Piece lists
        // its Tracks separately, so both are folded in here.
        switch (obj.kind) {
            case ContentKind::Layer:
                for (const StableId child : obj.layer.ownedChildren) {
                    parent_.emplace(child, c.id);
                }
                break;
            case ContentKind::Container:
                for (const StableId child : obj.container.ownedChildren) {
                    parent_.emplace(child, c.id);
                }
                break;
            case ContentKind::Track:
                for (const StableId child : obj.track.ownedChildren) {
                    parent_.emplace(child, c.id);
                }
                break;
            case ContentKind::Piece:
                for (const StableId child : obj.piece.tracks) {
                    parent_.emplace(child, c.id);
                }
                break;
            case ContentKind::Sound:
                break;
        }
    }
}

std::vector<StableId> ReferenceResolutionContext::PlacementChainForTrack(
    StableId trackId) const {
    std::vector<StableId> chain;
    chain.push_back(trackId);
    // The one enclosing Piece, if the Track is owned by one. Deeper nesting is
    // occurrence expansion, deferred to the runtime arranging instance.
    const auto it = parent_.find(trackId);
    if (it != parent_.end()) {
        const auto kindIt = kinds_.find(it->second);
        if (kindIt != kinds_.end() && kindIt->second == ContentKind::Piece) {
            chain.push_back(it->second);
        }
    }
    return chain;
}

std::array<ReferenceSlotResolution, kReferenceSlotCount>
ReferenceResolutionContext::Resolve(
    StableId subject, const std::vector<StableId>&) const {
    ResolveCtx ctx;
    ctx.refValues = &refValues_;

    ReferenceSlotSource source;
    source.ctx = &ctx;
    source.slotValue = &SlotValueCallback;
    source.placementAncestor = nullptr;
    source.organizationalAncestor = nullptr;
    source.slotDefault = nullptr;  // no project-level default at this stage

    std::array<ReferenceSlotResolution, kReferenceSlotCount> result;
    for (std::uint32_t i = 0; i < kReferenceSlotCount; ++i) {
        result[i] = ResolveReferenceSlot(subject, static_cast<ReferenceSlot>(i),
                                         source, depthCeiling_);
    }
    return result;
}

namespace {

// One clip's placement facts, gathered from the model so occurrences can be
// enumerated in the normalized order the facts already fix.
struct ClipPlacement {
    StableId trackId = kUnmintedStableId;
    ContentRef content;
};

}  // namespace

ReferenceResolutionResult BuildReferenceResolution(
    const CanonicalProject& project, const NormalizedFacts& facts,
    std::uint32_t depthCeiling) {
    ReferenceResolutionResult result;
    const ReferenceResolutionContext context(project, depthCeiling);

    // clipId -> its Track and referenced content, plus id -> guid for a refusal
    // diagnostic that can name the object it was resolving.
    std::map<StableId, ClipPlacement> clipPlacement;
    std::map<StableId, AuthoringGuid> guidById;
    for (const ContentObject& obj : project.objects) {
        guidById.emplace(obj.chassis().id, obj.chassis().guid);
        if (obj.kind == ContentKind::Track) {
            for (const Clip& clip : obj.track.clips) {
                clipPlacement.emplace(
                    clip.id, ClipPlacement{obj.track.chassis.id, clip.contentRef});
            }
        }
    }

    ReferenceResolutionFacts out;
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
            const ClipPlacement& placement = placementIt->second;
            const StableId subject = placement.content.target;

            OccurrenceReferenceResolution occ;
            occ.trackId = track.track;
            occ.clipId = clipId;
            occ.occurrenceOrdinal = static_cast<std::uint32_t>(ordinal);
            occ.subjectId = subject;
            occ.subjectKind = placement.content.kind;
            occ.resolvedSlots = context.Resolve(subject, {});

            for (const ReferenceSlotResolution& slot : occ.resolvedSlots) {
                if (slot.outcome == ResolutionOutcome::DepthCeilingExceeded) {
                    // An unreachable invariant on a structurally valid project:
                    // a chain past the ceiling means the front-end's cycle guard
                    // has a hole. Refuse loudly, all-or-nothing, rather than
                    // record a truncated answer.
                    Diagnostic d;
                    d.code = DiagnosticCode::ReferenceChainDepthExceeded;
                    const auto guidIt = guidById.find(subject);
                    if (guidIt != guidById.end()) {
                        d.primary = guidIt->second;
                    }
                    d.secondary = subject;
                    result.diagnostics.push_back(d);
                    return result;
                }
            }
            out.occurrences.push_back(std::move(occ));
        }
    }

    result.facts = std::move(out);
    return result;
}

ReferenceResolutionResult BuildReferenceResolution(
    const CanonicalProject& project, const NormalizedFacts& facts) {
    return BuildReferenceResolution(
        project, facts,
        static_cast<std::uint32_t>(project.objects.size()) + kDepthMargin);
}

}  // namespace harkenprojectschema
