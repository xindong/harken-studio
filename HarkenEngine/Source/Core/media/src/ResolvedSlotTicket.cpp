#include "harken/media/ResolvedSlotTicket.h"

namespace harken {

ResolvedSlotRefusal ValidateResolvedSlot(const ResolvedSlotTicket& ticket,
                                         const ResolvedSlotEnvironment& env,
                                         BusTargetLease* outLease) {
    if (outLease != nullptr) {
        *outLease = BusTargetLease{};
    }
    // The generated table decides both whether the ordinal is known and which
    // existence check applies. Recognizing a slot and choosing its check used to
    // be two hand-written lists, and a slot present in one but not the other
    // would have passed validation without ever being checked.
    const ResolvedSlotKindRow* row = FindResolvedSlotKind(ticket.kind);
    if (row == nullptr) {
        return ResolvedSlotRefusal::UnknownSlotKind;
    }
    if (ticket.value == VnObjectId{}) {
        // 0 names nothing. A program that could not resolve a slot must not
        // reach the runtime with a zero standing in for a default: the default
        // is a level on the compile side and has a value of its own.
        return ResolvedSlotRefusal::MissingTarget;
    }

    // Dispatch on what the value points at, not on which slot named it. Two
    // slots pointing at the same kind of target then share one check instead of
    // gaining a second copy of it.
    switch (row->targetKind) {
    case ResolvedSlotTargetKind::Bus: {
        const VnBusId bus = static_cast<VnBusId>(ticket.value);
        if (env.busExists == nullptr || !env.busExists(env.ctx, bus)) {
            return ResolvedSlotRefusal::DanglingTarget;
        }
        if (env.busLayoutCompatible == nullptr ||
            !env.busLayoutCompatible(env.ctx, bus)) {
            return ResolvedSlotRefusal::IncompatibleLayout;
        }
        if (env.busGeneration == nullptr) {
            return ResolvedSlotRefusal::DanglingTarget;
        }
        const std::uint32_t generation = env.busGeneration(env.ctx, bus);
        if (generation == 0) {
            return ResolvedSlotRefusal::DanglingTarget;
        }
        if (outLease != nullptr) {
            // A zero generation cannot identify a bus instance, so both fields
            // are written only after identity has been confirmed.
            outLease->bus = bus;
            outLease->generation = generation;
        }
        return ResolvedSlotRefusal::None;
    }
    case ResolvedSlotTargetKind::Attenuation:
        if (env.attenuationExists == nullptr ||
            !env.attenuationExists(env.ctx, ticket.value)) {
            return ResolvedSlotRefusal::DanglingTarget;
        }
        return ResolvedSlotRefusal::None;
    }
    // A target kind the table names but this function has no case for. Refusing
    // is the only safe answer: the alternative is admitting a target nothing
    // checked.
    return ResolvedSlotRefusal::UnknownSlotKind;
}

bool BusTargetLeaseStillValid(const BusTargetLease& lease,
                               const ResolvedSlotEnvironment& env) {
    if (lease.bus == VnObjectId{} || lease.generation == 0 ||
        env.busExists == nullptr || env.busGeneration == nullptr ||
        !env.busExists(env.ctx, lease.bus)) {
        return false;
    }
    const std::uint32_t current = env.busGeneration(env.ctx, lease.bus);
    return current != 0 && current == lease.generation;
}

} // namespace harken
