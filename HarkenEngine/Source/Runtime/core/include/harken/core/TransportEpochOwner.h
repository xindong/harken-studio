// The session's owner of the transport epoch: the one place a TransportEpoch is
// minted and raised.
//
// The epoch is the session's time-geometry generation (see TimeCoordinates.h).
// The pool and planner only consume it -- they compare it and reject a mismatch --
// but something has to decide when the geometry has changed and hand out the next
// generation. That is this owner. It mints the first epoch as 1 (zero is the
// reserved "no epoch" sentinel) and raises it whenever an operation invalidates
// every existing frame mapping at once: a sample-rate change, or an offline/
// realtime transport rebuild.
//
// Raising never wraps. The epoch is a uint64, so exhausting it is not a real
// runtime's concern, but if the counter ever reached its maximum, reusing a value
// -- or resetting to 1 -- would let a stale reference to the old geometry pass for
// the new one, exactly the ABA hazard the epoch exists to prevent. So at the
// boundary RaiseEpoch refuses and reports RuntimeRecreateRequired instead: the
// session cannot be rebuilt in place, only torn down and recreated. The current
// epoch is left untouched, never advanced into the reserved value.
//
// Rebuild coordination. Raising the epoch is only the first step of moving the
// runtime to a new time geometry; the horizon structures must be reset under it,
// in this order, so no structure is left holding or materializing old-geometry
// work:
//
//   1. status = owner.RaiseEpoch();  // stop here and recreate if RuntimeRecreateRequired
//   2. drain the deferred-intent backlog (the session reconstructs the deferred
//      pool for the new geometry, so no old intent materializes into it);
//   3. pool.Reset(poolConfig, newStartFrame, owner.Current());  // discards old
//      pages/transactions and stamps the new epoch;
//   4. planner.Reset(plannerConfig, &pool, &freshDeferredPool, &instances);  //
//      re-binds and clears its own program retention.
//
// Any old-epoch page or transaction that outlives the reset -- for instance if a
// consumer reads the pre-reset pool with the new epoch -- fails validation in
// constant time on the existing stale-transaction path (the epoch compare at
// consume), so no old-rate frame is ever executed. This owner adds no new
// invalidation mechanism; it only decides when the epoch changes.
//
// Value type, single-threaded (the session's logic thread), no allocation.
#ifndef HARKEN_CORE_TRANSPORT_EPOCH_OWNER_H
#define HARKEN_CORE_TRANSPORT_EPOCH_OWNER_H

#include "harken/core/TimeCoordinates.h"

#include <cstdint>

namespace harken {

// The outcome of a raise. RuntimeRecreateRequired is a terminal condition: the
// epoch space is exhausted and the runtime must be recreated rather than rebuilt
// in place. This status remains internal because the public C ABI has no
// matching VnResult yet, and adding one is a boundary change deferred with the
// rest of the ABI. When surfaced it maps to that future code (a
// VN_RUNTIME_RECREATE_REQUIRED), not to any existing code, because none carries
// the "recreate, do not retry" meaning.
enum class EpochRaiseStatus : std::uint32_t {
    Raised = 0,
    RuntimeRecreateRequired = 1
};

struct EpochRaiseResult {
    EpochRaiseStatus status = EpochRaiseStatus::Raised;
    // On Raised, the new current epoch. On RuntimeRecreateRequired, the unchanged
    // current epoch (never advanced into the reserved value).
    TransportEpoch epoch = kInvalidTransportEpoch;
};

class TransportEpochOwner {
public:
    // Mints the first epoch as 1.
    TransportEpochOwner() = default;

    // Resumes at a persisted epoch (save state, or a test near the wrap boundary).
    // The reserved sentinel 0 is treated as a fresh mint of 1, since 0 never names
    // a real geometry.
    explicit TransportEpochOwner(TransportEpoch initial)
        : current_(initial == kInvalidTransportEpoch ? 1 : initial) {}

    // The epoch in force; never the reserved 0.
    TransportEpoch Current() const { return current_; }

    // True once raising further would wrap, so a caller can see the terminal
    // condition coming without forcing it.
    bool Exhausted() const { return current_ == UINT64_MAX; }

    // Advances to the next epoch, or refuses at the wrap boundary. On refusal the
    // current epoch is unchanged.
    EpochRaiseResult RaiseEpoch() {
        if (current_ == UINT64_MAX) {
            return {EpochRaiseStatus::RuntimeRecreateRequired, current_};
        }
        ++current_;
        return {EpochRaiseStatus::Raised, current_};
    }

private:
    TransportEpoch current_ = 1; // 0 is the reserved sentinel; a session starts at 1
};

} // namespace harken

#endif // HARKEN_CORE_TRANSPORT_EPOCH_OWNER_H
