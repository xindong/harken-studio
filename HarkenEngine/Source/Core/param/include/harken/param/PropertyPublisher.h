// Transactional property publication, logic-thread side. The unit that
// moves one update
// tick's resolved property changes to the audio thread is the whole set
// or nothing — never a half-applied mix.
//
// Two phases per tick. Gather: stage every changed target's commit
// (locator, resolved value, ramp) into the tick batch. Publish: one
// admission check for the whole set — HasRoom(N), sound under SPSC because
// this thread is the only producer and the consumer only frees space —
// then push all N and clean every dirty mark, or push NOTHING, keep every
// target dirty, count the deferral, and retry the whole batch next tick.
//
// A deferred batch reserves its admission: from the moment of deferral,
// single-command producers (direct setters, plays, sends) admit only into
// capacity beyond the pending batch's size, so steady traffic cannot
// starve the batch — its delay bound is one tick.
//
// The publisher also maintains the section-10.4 capacity contract: the
// worst-case one-tick publication size, tracked incrementally as target
// wirings grow and shrink, must fit the command queue — a growth that
// would exceed it is refused structurally, so a legal wiring can always
// drain.
//
// Logic thread only. The batch buffer is fixed-capacity, preallocated at
// construction.
#ifndef HARKEN_PARAM_PROPERTY_PUBLISHER_H
#define HARKEN_PARAM_PROPERTY_PUBLISHER_H

#include <harken/Harken.h>

#include "harken/core/LogRing.h"

#include <cstdint>
#include <vector>

namespace harken {

// Internal aliases of the public scope kinds of the target locator
// (scopeKind, objectId, targetId) — defined from the Harken.h values so
// the mirror cannot drift.
enum : std::uint32_t {
    kTargetScopeVoice = VN_TARGET_SCOPE_VOICE,
    kTargetScopeBus = VN_TARGET_SCOPE_BUS,
};

// One resolved property change bound for the audio thread: the scoped
// target locator, the reduced domain value, and the aggregated ramp the
// wire command carries.
struct PropertyCommit {
    std::uint32_t scopeKind = 0;
    std::uint64_t objectId = 0;
    std::uint32_t targetId = 0;
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    float pan2d = 0.0f;
    float spatialWidth = 0.0f;
    std::uint32_t spatialMask = 0;
};

inline constexpr std::uint32_t kSpatialPropertyPan = 1;
inline constexpr std::uint32_t kSpatialPropertyWidth = 2;

class PropertyPublisher {
public:
    // Admission view of the command queue, supplied per call so the
    // publisher owns the discipline and not the transport: FreeSlots
    // reports how many commands the queue can accept right now; push hands
    // one commit over and reports acceptance. The publisher only calls
    // Push after FreeSlots admitted the whole set.
    using FreeSlotsFn = std::uint32_t (*)(void* ctx);
    using PushFn = bool (*)(void* ctx, const PropertyCommit& commit);

    // batchCapacity bounds one tick's staged commits (the section-10.4
    // capacity contract keeps legal wirings inside it).
    explicit PropertyPublisher(std::uint32_t batchCapacity);

    // Gather phase: stages one changed target's commit into the current
    // batch. Returns false when the batch buffer is full.
    bool Stage(const PropertyCommit& commit);
    // The caller resets groupIndex with Restage. A spatial group consumes one
    // slot even when its second target arrives after the batch becomes full.
    bool StageSpatial(const PropertyCommit& commit, std::uint32_t& groupIndex);

    // Commits currently staged (a deferred batch stays staged in full).
    std::uint32_t StagedCount() const;

    // Publish phase: preflights HasRoom(stagedCount) against FreeSlots,
    // then pushes all staged commits in stage order and empties the batch
    // (returns true), or pushes nothing, keeps the batch staged, counts
    // the deferral, and reserves the batch's admission (returns false).
    // An empty batch publishes trivially — no admission query — and
    // releases any held reservation (a deferred batch rebuilt to empty
    // means every deferred target was consumed by other means; nothing is
    // pending anymore).
    bool Publish(FreeSlotsFn freeSlots, PushFn push, void* ctx);

    // Restarts staging for a fresh gather while a deferral may be
    // pending: discards the staged commits but KEEPS the deferred flag,
    // so the reservation survives the rebuild window. The dirty marks are
    // the retry mechanism — a deferred batch's targets were never
    // mark-committed, so the next tick's gather re-collects them at
    // CURRENT resolved values together with newly-dirty targets; the
    // rebuilt batch IS the pending batch (section 10.2's atomic unit is
    // one tick's property commits, freshly reduced each tick).
    void Restage();

    // Admission gate for single-command producers: admits only into
    // capacity beyond the pending deferred batch's size, so steady traffic
    // cannot starve the batch. With no deferred batch pending this is the
    // plain has-capacity check.
    bool AdmitSingle(FreeSlotsFn freeSlots, void* ctx) const;

    // Size of the pending deferred batch currently holding a reservation;
    // zero when no publication is pending. Tracks the CURRENT staged
    // size: after a restage-and-rebuild the pending batch is the rebuilt
    // one, and the next publish's whole-set admission needs exactly its
    // size — section 10.2 reserves "the pending batch's size", and a
    // stale pinned size would over- or under-reserve against it.
    std::uint32_t PendingReservation() const;

    // Publications deferred for lack of room across the publisher's
    // lifetime (paramPublicationDeferredCount).
    std::uint64_t DeferredCount() const;

    // Section-10.4 capacity contract, maintained incrementally. The
    // worst-case one-tick publication — every wired target that could
    // commit in the same tick, unique-counted — must fit the command
    // queue; batchCapacity is sized to the queue's capacity, so one bound
    // serves both the batch buffer and the admission preflight. Growth
    // that would exceed the bound is refused with no state change, and a
    // log record carrying both numbers (proposed worst case, capacity)
    // goes through `log` when non-null. The attach verbs surface this
    // refusal as VN_INVALID_CONFIG.
    bool TryGrowWorstCase(std::uint32_t addedUniqueTargets, LogRing* log);

    // Releases worst-case demand when a wiring detaches; clamped at zero
    // so a mismatched release can never underflow the bound.
    void ShrinkWorstCase(
        std::uint32_t removedUniqueTargets) noexcept;

    // Current worst-case one-tick publication size.
    std::uint32_t WorstCaseCommits() const;

    // The fixed batch bound (equals the command queue capacity).
    std::uint32_t BatchCapacity() const;

private:
    std::uint32_t m_batchCapacity = 0;
    std::uint32_t m_stagedCount = 0;
    // True from the tick a staged batch was refused admission until it
    // publishes: the window in which the batch reserves its admission
    // against single-command producers.
    bool m_deferredPending = false;
    std::uint64_t m_deferredCount = 0;
    std::uint32_t m_worstCaseCommits = 0;
    std::vector<PropertyCommit> m_batch; // fixed capacity, allocated once
};

} // namespace harken

#endif // HARKEN_PARAM_PROPERTY_PUBLISHER_H
