// The deferred-intent pool: intents accepted now that are due beyond the
// schedulable window, held until the window reaches them.
//
// An intent whose due frame is past the horizon cannot be filed as a scheduled
// transaction yet, and refusing it would make "how far ahead may I schedule" a
// question authors have to answer. So it waits here, stamped with its owner, and
// materializes into a transaction once the window covers it.
//
// Two ceilings, not one. The pool has a total capacity, and a single owner has a
// per-owner cap, because one Action queueing thousands of far-future intents
// would otherwise consume the room every other Action needs while looking
// perfectly legal. Both refuse; neither evicts.
//
// Cancellation is by stamp rather than by search. An intent carries the owner's
// schedule epoch, so cancelling the owner invalidates every intent it queued
// without the canceller having to find them. Invalidated intents are dropped
// when the window reaches them, which keeps cancellation constant-time and
// leaves the pool's occupancy honest until then.
//
// Logic thread only. Storage is sized once at construction.
#ifndef HARKEN_CORE_DEFERRED_INTENT_POOL_H
#define HARKEN_CORE_DEFERRED_INTENT_POOL_H

#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/PlayableInstanceTable.h"

#include <cstdint>
#include <vector>

namespace harken {

class ActionDeferredRecordTable;

enum class DeferredIntentOwnerDomain : std::uint8_t {
    Playable = 0,
    Action = 1,
};

// One far-future intent. The payload is opaque: the pool holds and releases
// intents, it does not interpret them.
struct DeferredIntent {
    DeferredIntentOwnerDomain ownerDomain =
        DeferredIntentOwnerDomain::Playable;
    std::uint64_t ownerHandle = 0;
    std::uint64_t ownerEpoch = 0;
    std::uint64_t dueFrame = 0;
    std::uint64_t payload = 0;
    LogicCommitSequence commitSequence = 0;
    std::uint64_t executionOrdinal = 0;
    std::uint64_t notBeforeQuantum = 0;
};

enum class DeferredIntentRefusal : std::uint32_t {
    None = 0,
    PoolFull = 1,
    OwnerCapReached = 2,
    GenerationExhausted = 3,
    ProtocolViolation = 4,
};

enum class DeferredIntentEntryState : std::uint8_t {
    Free = 0,
    Held = 1,
    Claimed = 2,
    Quarantined = 3,
    Prepared = 4,
};

struct DeferredIntentToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;

    bool Valid() const { return slot != UINT32_MAX && generation != 0; }
};

struct DeferredIntentRetryReservation {
    DeferredIntentToken poolToken{};
    std::uint32_t recordSlot = UINT32_MAX;
    std::uint64_t recordGeneration = 0;

    bool Valid() const noexcept {
        return poolToken.Valid() && recordSlot != UINT32_MAX &&
               recordGeneration != 0;
    }
};

struct DeferredIntentAddResult {
    DeferredIntentRefusal status = DeferredIntentRefusal::None;
    DeferredIntentToken token;
};

struct DeferredIntentClaim {
    DeferredIntentToken token;
    DeferredIntent intent;
};

class DeferredIntentPool {
public:
    // capacity bounds the whole pool; maxPerOwner bounds any single owner. A
    // maxPerOwner of 0 means an owner may use the whole pool, which is only
    // sensible in a single-owner test.
    DeferredIntentPool(std::uint32_t capacity, std::uint32_t maxPerOwner);
    DeferredIntentPool(const DeferredIntentPool&) = delete;
    DeferredIntentPool& operator=(const DeferredIntentPool&) = delete;
    DeferredIntentPool(DeferredIntentPool&&) = delete;
    DeferredIntentPool& operator=(DeferredIntentPool&&) = delete;
    ~DeferredIntentPool();

    std::uint32_t Capacity() const {
        return static_cast<std::uint32_t>(m_entries.size());
    }
    std::uint32_t MaxPerOwner() const { return m_maxPerOwner; }

    DeferredIntentAddResult AddPrepared(const DeferredIntent& intent);
    bool CommitPrepared(DeferredIntentToken token);
    bool AbandonPrepared(DeferredIntentToken token);
    DeferredIntentAddResult AddClaimable(const DeferredIntent& intent);
    DeferredIntentRefusal Add(const DeferredIntent& intent) {
        return AddClaimable(intent).status;
    }

    std::uint32_t ClaimDue(std::uint64_t windowEndFrame,
                           const PlayableInstanceTable& instances,
                           DeferredIntentClaim* out, std::uint32_t capacity,
                           std::uint32_t* staleDropped = nullptr,
                           std::uint32_t* dueCount = nullptr,
                           std::uint64_t currentQuantum = UINT64_MAX);
    DeferredIntentRefusal Retry(DeferredIntentToken token,
                                std::uint64_t notBeforeQuantum = 0);
    DeferredIntentAddResult Replace(DeferredIntentToken token,
                                    const DeferredIntent& intent);
    bool Release(DeferredIntentToken token);
    bool Cancel(DeferredIntentToken token);
    bool CanResetForTransport() const noexcept;
    bool ResetForTransport() noexcept;

    // Intents held, and held for one owner.
    std::uint32_t Count() const { return m_count; }
    std::uint32_t PreparedCount() const { return m_preparedCount; }
    std::uint32_t CountForOwner(PlayableInstanceHandle owner) const;
    std::uint32_t CountForOwner(DeferredIntentOwnerDomain domain,
                                std::uint64_t ownerHandle) const;
    std::uint32_t HighWater() const { return m_highWater; }
    std::uint64_t EarliestHeldDueBefore(std::uint64_t cutoffFrame) const;
    bool SetGenerationForTests(std::uint32_t slot,
                               std::uint64_t generation);
    bool SetTokenGenerationForTests(DeferredIntentToken token,
                                    std::uint64_t generation,
                                    DeferredIntentToken* outToken);

    // Releases every intent due before windowEndFrame, appending the ones whose
    // owner stamp still validates to out and dropping the rest. When provided,
    // droppedPayloads receives the opaque payload of each stale intent so its
    // caller-owned side storage can be reclaimed. Returns the stale count.
    std::uint32_t Materialize(std::uint64_t windowEndFrame,
                              const PlayableInstanceTable& instances,
                              std::vector<DeferredIntent>* out,
                              std::vector<std::uint64_t>* droppedPayloads =
                                  nullptr);

    // Drops every intent whose owner stamp no longer validates, without waiting
    // for the window. Returns how many were dropped. Reclaiming room early is
    // optional: correctness does not depend on it, only occupancy does.
    std::uint32_t DropStale(
        const PlayableInstanceTable& instances,
        std::vector<std::uint64_t>* droppedPayloads = nullptr);

private:
    friend class ActionDeferredRecordTable;

    struct Entry {
        DeferredIntentEntryState state = DeferredIntentEntryState::Free;
        std::uint64_t generation = 1;
        DeferredIntent intent;
        DeferredIntentRetryReservation retryReservation{};
        const ActionDeferredRecordTable* preparedPairOwner = nullptr;
        std::uint64_t preparedPairMarker = 0;
    };

    Entry* Validate(DeferredIntentToken token,
                    DeferredIntentEntryState expected);
    void CommitValidatedPrepared(Entry& entry) noexcept;
    std::uint32_t OccupancyForOwner(DeferredIntentOwnerDomain domain,
                                    std::uint64_t ownerHandle) const;
    void FreeEntry(Entry& entry);

    std::vector<Entry> m_entries;
    std::uint32_t m_maxPerOwner = 0;
    std::uint32_t m_count = 0;
    std::uint32_t m_preparedCount = 0;
    std::uint32_t m_highWater = 0;
};

} // namespace harken

#endif // HARKEN_CORE_DEFERRED_INTENT_POOL_H
