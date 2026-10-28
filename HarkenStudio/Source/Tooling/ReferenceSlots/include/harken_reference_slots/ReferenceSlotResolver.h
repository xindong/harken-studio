// Reference-slot resolution: the single core that decides which authored
// value a content object's reference slot resolves to.
//
// The rule is compile-time only. A resolved value is baked into the compiled
// program, and the runtime consumes that result — it never walks ancestor
// chains of its own. Keeping the walk on one side of that line is what stops
// the same precedence rule from existing twice.
//
// Precedence is fixed and total: an explicit override on the referenced
// object beats the nearest ancestor in the placement chain, which beats the
// nearest ancestor in the organizational chain, which beats the slot's own
// named default. Every level is named, so a resolved value can always be
// attributed to the level that produced it; there is no unnamed fallback to
// explain away.
//
// Resolution takes a slot id and applies the same rules to the object's output
// bus and Attenuation. Each resolves to a single winning value across levels.
//
// Resolution and explanation are one pass. The walk visits every level even
// after it has a winner, because the question a tool asks is not only "what did
// this resolve to" but "why not the layer's value" — and answering the second
// needs the candidates that lost. Compile time can afford the full walk; there
// is no runtime path here to keep short.
//
// This library deliberately depends on neither the engine nor any UI toolkit:
// slot values cross the boundary as plain 64-bit ids, and even a slot's default
// arrives through the caller rather than being written in here. That is what
// lets the program compiler, the offline explain runner, and the tests all
// share one rule without any of them pulling in the others' world.
#ifndef HARKEN_REFERENCE_SLOTS_REFERENCE_SLOT_RESOLVER_H
#define HARKEN_REFERENCE_SLOTS_REFERENCE_SLOT_RESOLVER_H

// The slot and level vocabulary, plus the slot meta table, are generated from
// harken_reference_slot_table.json. The runtime's own slot enumeration comes out
// of that same table, so the two numberings cannot disagree by being edited
// apart.
#include "harken_reference_slots.g.h"
#include <harken_studio/ObjectId.h>

#include <cstdint>
#include <vector>

namespace harkenreferenceslots {

// The row for one slot, or nullptr for an ordinal no slot uses. Callers get a
// refusal rather than a guess.
const ReferenceSlotMeta* FindReferenceSlot(ReferenceSlot slot);

// Where slot values and ancestor chains come from. Every callback may be null,
// and a null callback answers "nothing here" — the same convention the pack
// closure resolver uses, so an absent source degrades to an unresolved slot
// instead of undefined behaviour.
//
// The ancestor callbacks yield ancestors only, nearest first, index from 0. The
// subject is passed separately because "the object itself" is a distinct
// precedence level; folding it into index 0 of one chain would leave the other
// chain's indexing meaning something different.
struct ReferenceSlotSource {
    void* ctx = nullptr;

    // The explicit value authored on one object for one slot. False means the
    // object declares nothing here, which is what lets resolution continue
    // upward. Never guesses.
    bool (*slotValue)(void* ctx, harkenstudio::ObjectId objectId, ReferenceSlot slot,
                      harkenstudio::ObjectId* outValue) = nullptr;

    // Ancestors in the placement chain — the Track or Segment an object is
    // arranged on. False means the index is past the end of the chain.
    bool (*placementAncestor)(void* ctx, harkenstudio::ObjectId subjectId,
                              std::uint32_t index,
                              harkenstudio::ObjectId* outObjectId) = nullptr;

    // Ancestors in the organizational chain — the AudioLayer an object is
    // filed under.
    bool (*organizationalAncestor)(void* ctx, harkenstudio::ObjectId subjectId,
                                   std::uint32_t index,
                                   harkenstudio::ObjectId* outObjectId) = nullptr;

    // The slot's project-level default. False means the slot has no default,
    // in which case an object inheriting nothing resolves to nothing and the
    // caller decides whether that is an error. The value comes from here rather
    // than from a table in this library, so no engine or project constant has
    // to be written into the rule.
    bool (*slotDefault)(void* ctx, ReferenceSlot slot,
                        harkenstudio::ObjectId* outValue) = nullptr;
};

// One level that supplied a value. Levels that supplied nothing produce no
// candidate, so the list reads as "everything that had an opinion", in
// descending precedence.
struct ReferenceSlotCandidate {
    ResolutionLevel level = ResolutionLevel::SlotDefault;
    // The object that authored the value. 0 for the slot default, which is not
    // authored on any object.
    harkenstudio::ObjectId sourceObjectId{};
    // Distance up the chain: 1 is the nearest ancestor. 0 for the subject
    // itself and for the slot default.
    std::uint32_t ancestorDepth = 0;
    harkenstudio::ObjectId value{};
    // Exactly one candidate is the winner; the rest were shadowed by it.
    bool winner = false;
};

enum class ResolutionOutcome : std::uint32_t {
    // A level supplied a value.
    Resolved = 0,
    // No level supplied one, and the slot has no default. The resolver does not
    // invent a value; the caller decides whether this refuses the build.
    Unresolved = 1,
    // A chain ran past the depth ceiling. Reported rather than resolved,
    // because "no higher-precedence value exists" would only be true within the
    // depth actually walked. A cyclic chain lands here too, which is how a
    // malformed source becomes a refusal instead of a hang.
    DepthCeilingExceeded = 2
};

struct ReferenceSlotResolution {
    ResolutionOutcome outcome = ResolutionOutcome::Unresolved;
    // Winner, meaningful only when outcome is Resolved.
    ResolutionLevel level = ResolutionLevel::SlotDefault;
    harkenstudio::ObjectId sourceObjectId{};
    std::uint32_t ancestorDepth = 0;
    harkenstudio::ObjectId value{};
    // How far each chain was actually walked. This is what distinguishes "the
    // organizational chain held nothing" from "the organizational chain was
    // never consulted", a difference an explain report must not blur.
    std::uint32_t placementDepthWalked = 0;
    std::uint32_t organizationalDepthWalked = 0;
    // Every level that supplied a value, in descending precedence. This is the
    // explain output: one row per candidate, the winner flagged, and the slot
    // default present as a level of its own whenever the slot has one.
    std::vector<ReferenceSlotCandidate> candidates;
};

// Resolves one slot on one object and explains the result in the same pass.
//
// depthCeiling bounds each chain independently. It exists because the chains
// come from a caller-supplied source that this library cannot inspect: without
// a bound, a cyclic implementation would walk forever. Exceeding it is a
// deterministic refusal.
ReferenceSlotResolution ResolveReferenceSlot(harkenstudio::ObjectId subjectId,
                                             ReferenceSlot slot,
                                             const ReferenceSlotSource& source,
                                             std::uint32_t depthCeiling);

} // namespace harkenreferenceslots

#endif // HARKEN_REFERENCE_SLOTS_REFERENCE_SLOT_RESOLVER_H
