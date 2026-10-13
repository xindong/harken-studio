// The runtime end of reference-slot resolution: a resolved value as it arrives
// in a compiled program, and the load-time check that decides whether it can be
// honoured.
//
// The runtime does not resolve. It receives one already-decided value per slot
// per occurrence and validates it, which is why nothing here walks a placement
// or organizational chain and why no ancestor is reachable from a ticket. The
// precedence rule lives on the compile side and exists exactly once.
//
// Validation happens at load, before admission, because the alternative is
// discovering a dangling target while a voice is already starting. A bus target
// that no longer exists, or one whose channel layout the material cannot be
// converted into, refuses here.
//
// A validated bus target is handed back as a lease: the id plus the bus
// generation it was checked against. Holding the generation is what makes a
// later topology change invalidate the lease instead of silently rerouting
// audio to whatever now occupies that id.
//
// The level a value came from is carried for diagnostics only. The runtime never
// branches on it — behaving differently depending on which level won would make
// the same authored value mean different things in different placements.
#ifndef HARKEN_MEDIA_RESOLVED_SLOT_TICKET_H
#define HARKEN_MEDIA_RESOLVED_SLOT_TICKET_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

// Slot ordinals and the target kind each one points at are generated from the
// same table the compile-side resolver reads, so the two numberings cannot be
// edited apart. Validation dispatches on the generated target kind rather than
// on a mapping written per slot, which is what stops a new slot from arriving
// with no existence check while every existing check still passes.
#include "harken/media/generated/harken_resolved_slot_kinds.g.h"

#include <cstdint>

namespace harken {

// One already-resolved slot value. Immutable: the runtime reads it and never
// recomputes it.
struct ResolvedSlotTicket {
    ResolvedSlotKind kind = ResolvedSlotKind::OutputBus;
    // The winning target id. Its meaning follows the kind: a bus id, an
    // attenuation id, or a binding id.
    VnObjectId value{};
    // Which precedence level produced the value. Diagnostics only.
    std::uint32_t sourceLevel = 0;
};

// What the environment can answer about a target. Every callback may be null,
// and a null callback answers "no" — an environment that cannot confirm a
// target is not an environment in which the target may be used.
struct ResolvedSlotEnvironment {
    void* ctx = nullptr;
    bool (*busExists)(void* ctx, VnBusId bus) = nullptr;
    // Whether material routed here can be converted into the bus's channel
    // layout under the existing bus-tree contract.
    bool (*busLayoutCompatible)(void* ctx, VnBusId bus) = nullptr;
    // The bus's current generation, or 0 when the bus does not exist.
    std::uint32_t (*busGeneration)(void* ctx, VnBusId bus) = nullptr;
    bool (*attenuationExists)(void* ctx, VnObjectId attenuationId) = nullptr;
};

enum class ResolvedSlotRefusal : std::uint32_t {
    None = 0,
    // The ticket names a slot ordinal this runtime does not know. Refused
    // rather than treated as one of the ordinals it does know.
    UnknownSlotKind = 1,
    // A value of 0 names nothing. It reaches here only from a program that
    // failed to resolve, so it is a refusal and not a default.
    MissingTarget = 2,
    // The target does not exist in this session.
    DanglingTarget = 3,
    // The bus exists but its channel layout cannot take this material.
    IncompatibleLayout = 4
};

// A validated bus target together with the generation it was validated against.
// Zeroed when validation refused.
struct BusTargetLease {
    VnBusId bus{};
    std::uint32_t generation = 0;
};

// Validates one resolved slot against the session. Writes a lease when the
// ticket names a bus and validation passed; outLease may be null for the slots
// that do not name a bus. A refusal leaves a supplied lease zeroed.
ResolvedSlotRefusal ValidateResolvedSlot(const ResolvedSlotTicket& ticket,
                                         const ResolvedSlotEnvironment& env,
                                         BusTargetLease* outLease);

// Whether a lease still describes the bus it was taken against. False once the
// bus is gone or its generation has moved, so a caller re-validates instead of
// routing into a bus that has since been replaced.
bool BusTargetLeaseStillValid(const BusTargetLease& lease,
                              const ResolvedSlotEnvironment& env);

} // namespace harken

#endif // HARKEN_MEDIA_RESOLVED_SLOT_TICKET_H
