#include "harken_reference_slots/ReferenceSlotResolver.h"

namespace harkenreferenceslots {
namespace {

using AncestorFn = bool (*)(void*, harkenstudio::ObjectId, std::uint32_t,
                            harkenstudio::ObjectId*);

bool ReadSlotValue(const ReferenceSlotSource& source, harkenstudio::ObjectId objectId,
                   ReferenceSlot slot, harkenstudio::ObjectId* outValue) {
    if (source.slotValue == nullptr) {
        return false;
    }
    return source.slotValue(source.ctx, objectId, slot, outValue);
}

// Walks one chain nearest-first and records the first ancestor that authored a
// value. Returns false only when the chain ran past the ceiling, which the
// caller turns into a refusal: a truncated walk cannot support the claim that
// no nearer value existed.
bool WalkChain(AncestorFn ancestor, void* ctx, harkenstudio::ObjectId subjectId,
               const ReferenceSlotSource& source, ReferenceSlot slot,
               std::uint32_t depthCeiling, std::uint32_t* outWalked,
               bool* outFound, harkenstudio::ObjectId* outObjectId,
               std::uint32_t* outDepth, harkenstudio::ObjectId* outValue) {
    *outWalked = 0;
    *outFound = false;
    if (ancestor == nullptr) {
        return true;
    }
    for (std::uint32_t index = 0;; ++index) {
        harkenstudio::ObjectId objectId{};
        if (!ancestor(ctx, subjectId, index, &objectId)) {
            return true;
        }
        if (index >= depthCeiling) {
            // Either a genuinely deeper chain than the contract allows or a
            // source that keeps producing ancestors forever. Both are refusals
            // here rather than a partial answer.
            return false;
        }
        *outWalked = index + 1;
        harkenstudio::ObjectId value{};
        if (!*outFound && ReadSlotValue(source, objectId, slot, &value)) {
            // Nearest wins, so the search for this chain's candidate stops at
            // the first authored value. The walk itself continues so the
            // reported depth describes the whole chain.
            *outFound = true;
            *outObjectId = objectId;
            *outDepth = index + 1;
            *outValue = value;
        }
    }
}

void Append(ReferenceSlotResolution* out, ResolutionLevel level,
            harkenstudio::ObjectId sourceObjectId, std::uint32_t ancestorDepth,
            harkenstudio::ObjectId value) {
    ReferenceSlotCandidate candidate;
    candidate.level = level;
    candidate.sourceObjectId = sourceObjectId;
    candidate.ancestorDepth = ancestorDepth;
    candidate.value = value;
    out->candidates.push_back(candidate);
}

} // namespace

const ReferenceSlotMeta* FindReferenceSlot(ReferenceSlot slot) {
    for (const ReferenceSlotMeta& row : kReferenceSlotTable) {
        if (row.slot == slot) {
            return &row;
        }
    }
    return nullptr;
}

ReferenceSlotResolution ResolveReferenceSlot(
    harkenstudio::ObjectId subjectId, ReferenceSlot slot,
    const ReferenceSlotSource& source, std::uint32_t depthCeiling) {
    ReferenceSlotResolution result;
    if (FindReferenceSlot(slot) == nullptr) {
        // An ordinal no slot uses cannot be resolved against the slot table, so
        // it is refused rather than walked as if it addressed something.
        result.outcome = ResolutionOutcome::Unresolved;
        return result;
    }

    // The object's own override, which no chain can outrank.
    harkenstudio::ObjectId ownValue{};
    const bool hasOwn = ReadSlotValue(source, subjectId, slot, &ownValue);

    bool placementFound = false;
    harkenstudio::ObjectId placementObject{};
    std::uint32_t placementDepth = 0;
    harkenstudio::ObjectId placementValue{};
    if (!WalkChain(source.placementAncestor, source.ctx, subjectId, source, slot,
                   depthCeiling, &result.placementDepthWalked, &placementFound,
                   &placementObject, &placementDepth, &placementValue)) {
        result.outcome = ResolutionOutcome::DepthCeilingExceeded;
        return result;
    }

    bool organizationalFound = false;
    harkenstudio::ObjectId organizationalObject{};
    std::uint32_t organizationalDepth = 0;
    harkenstudio::ObjectId organizationalValue{};
    if (!WalkChain(source.organizationalAncestor, source.ctx, subjectId, source,
                   slot, depthCeiling, &result.organizationalDepthWalked,
                   &organizationalFound, &organizationalObject,
                   &organizationalDepth, &organizationalValue)) {
        result.outcome = ResolutionOutcome::DepthCeilingExceeded;
        return result;
    }

    harkenstudio::ObjectId defaultValue{};
    const bool hasDefault =
        source.slotDefault != nullptr &&
        source.slotDefault(source.ctx, slot, &defaultValue);

    // Candidates are appended in descending precedence, so the first one is the
    // winner and the rest are exactly what it shadowed.
    if (hasOwn) {
        Append(&result, ResolutionLevel::ObjectOverride, subjectId, 0, ownValue);
    }
    if (placementFound) {
        Append(&result, ResolutionLevel::PlacementAncestor, placementObject,
               placementDepth, placementValue);
    }
    if (organizationalFound) {
        Append(&result, ResolutionLevel::OrganizationalAncestor,
               organizationalObject, organizationalDepth, organizationalValue);
    }
    if (hasDefault) {
        // The default is emitted even when it loses. A level that only appears
        // when it wins would read as an absence of a level, which is how an
        // unnamed fallback gets back in.
        Append(&result, ResolutionLevel::SlotDefault, {}, 0, defaultValue);
    }

    if (result.candidates.empty()) {
        result.outcome = ResolutionOutcome::Unresolved;
        return result;
    }
    ReferenceSlotCandidate& winner = result.candidates.front();
    winner.winner = true;
    result.outcome = ResolutionOutcome::Resolved;
    result.level = winner.level;
    result.sourceObjectId = winner.sourceObjectId;
    result.ancestorDepth = winner.ancestorDepth;
    result.value = winner.value;
    return result;
}

} // namespace harkenreferenceslots
