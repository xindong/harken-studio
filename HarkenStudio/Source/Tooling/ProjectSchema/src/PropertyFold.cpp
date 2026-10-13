#include "harken_project_schema/PropertyFold.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <vector>

namespace harkenprojectschema {

PropertyFoldContext::PropertyFoldContext(const CanonicalProject& project) {
    for (const ContentObject& obj : project.objects) {
        const Chassis& c = obj.chassis();
        Contribution contribution;
        contribution.gainDb = c.volume + c.makeUpGain;
        if (c.presence.Has(
                harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)) {
            contribution.outputBusVolumeDb = c.outputBusVolume;
        }
        contribution.pitchCents = c.pitch;
        contribution.loopCount = c.loopCount;
        contribution.kind = obj.kind;
        objects_.emplace(c.id, contribution);

        // Invert the downward ownership edges into child -> owner. A Piece lists
        // its Tracks separately from the ownedChildren the other kinds use, so
        // both are folded into the one parent map.
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

const PropertyFoldContext::Contribution* PropertyFoldContext::Find(
    StableId id) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

FoldChainComponents PropertyFoldContext::ChainComponents(
    StableId subject, StableId trackId) const {
    FoldChainComponents components;
    components.subject = subject;

    // Placement chain: the Track that carries the clip, then the Piece that owns
    // that Track. It stops there; deeper nesting is occurrence expansion the IR
    // defers to the runtime arranging instance.
    if (trackId != kUnmintedStableId) {
        components.placementChain.push_back(trackId);
        const auto ownerIt = parent_.find(trackId);
        if (ownerIt != parent_.end()) {
            const Contribution* owner = Find(ownerIt->second);
            if (owner != nullptr && owner->kind == ContentKind::Piece) {
                components.placementChain.push_back(ownerIt->second);
            }
        }
    }

    // Organizational chain: every structural owner of the subject, whatever its
    // kind. Bounded by the object count; ownership is acyclic and single-owner,
    // so the walk terminates at a root well within that.
    StableId cursor = subject;
    for (std::size_t guard = 0; guard <= objects_.size(); ++guard) {
        const auto it = parent_.find(cursor);
        if (it == parent_.end()) {
            break;  // reached a root
        }
        cursor = it->second;
        components.organizationalChain.push_back(cursor);
    }
    return components;
}

FoldedProperties PropertyFoldContext::Fold(StableId subject,
                                           StableId trackId,
                                           const PlayablePropertyPayloadV1* occurrence) const {
    FoldedProperties result;
    const Contribution* subjectContribution = Find(subject);
    if (subjectContribution == nullptr) {
        // A subject the model does not define cannot be folded; leave the
        // neutral base values. This is unreachable for a validated project.
        return result;
    }
    result.loopCount = subjectContribution->loopCount;
    if (subjectContribution->outputBusVolumeDb) {
        result.outputBusVolumeDb = *subjectContribution->outputBusVolumeDb;
    }

    // Assemble the shared chain members in the fold's own order — subject, then
    // the placement chain, then the organizational chain — deduplicating so an
    // object reached by both chains contributes once. This placement-first order
    // fixes the float summation sequence, which is not associative and is pinned
    // by the fold's rendering.
    const FoldChainComponents components = ChainComponents(subject, trackId);
    std::vector<StableId> contributors;
    contributors.reserve(objects_.size());
    std::set<StableId> seen;
    const auto addContributor = [&contributors, &seen](StableId id) {
        if (seen.insert(id).second) {
            contributors.push_back(id);
        }
    };
    addContributor(components.subject);
    for (const StableId id : components.placementChain) {
        addContributor(id);
    }
    for (const StableId id : components.organizationalChain) {
        addContributor(id);
    }

    float gainDb = 0.0f;
    float cents = 0.0f;
    for (const StableId contributorId : contributors) {
        if (const Contribution* contribution = Find(contributorId)) {
            gainDb += contribution->gainDb;
            cents += contribution->pitchCents;
        }
    }
    if (occurrence) {
        using Property = harkenplayableproperties::PlayablePropertyIdV1;
        if (occurrence->presence.Has(Property::Volume)) gainDb += occurrence->volume;
        if (occurrence->presence.Has(Property::MakeUpGain)) gainDb += occurrence->makeUpGain;
        if (occurrence->presence.Has(Property::Pitch)) cents += occurrence->pitch;
    }

    const float clamped =
        std::clamp(cents, -kPitchCentClamp, kPitchCentClamp);
    result.baseVolumeDb = gainDb;
    result.basePitchRatio = static_cast<float>(
        std::exp2(static_cast<double>(clamped) / 1200.0));
    return result;
}

}  // namespace harkenprojectschema
