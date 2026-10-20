#pragma once

#include "harken/render/VoicePool.h"

#include <cassert>

namespace harken::detail {

struct VoicePoolReleaseTestAccess {
    static void SetRingCommitPageLimit(VoicePool& pool, std::uint32_t channel, std::size_t pages) {
        pool.m_ringStorage[channel].SetCommitPageLimitForTests(pages);
    }
    struct OriginalCallbackObservation {
        bool reader = false;
        bool storageHeld = false;
        bool actor = false;
        std::uint64_t bodyCount = 0;
        bool disposed = false;
        bool returnClaimed = false;
        bool loanReturned = false;
        bool pendingInstallation = false;
        bool installedInstallation = false;
        bool callbackHeld = false;
        bool rendering = false;
        bool free = false;
    };
    // Callback-thread or quiescent test observation only; no payload authority.
    static OriginalCallbackObservation ObserveOriginalCallback(const VoicePool& pool,
        std::uint32_t slot) {
        assert(slot < pool.m_slots.size());
        const auto& native = pool.m_slots[slot];
        const auto& lease = pool.m_originalCallbackStorage[slot];
        const auto lifetime = native.originalBodyLifetime.load(std::memory_order_acquire);
        return {native.originalCallbackStorageReader.load(std::memory_order_acquire),
            lease.storageHeld, lease.actor.Valid(), lifetime & VoicePool::kOriginalBodyPinMask,
            (lifetime & VoicePool::kOriginalBodyDisposed) != 0,
            (lifetime & VoicePool::kOriginalBodyReturnClaimed) != 0,
            (lifetime & VoicePool::kOriginalBodyLoanReturned) != 0,
            native.pendingOriginalSource.Valid(), native.installedOriginalSource.Valid(),
            native.state.load(std::memory_order_acquire) == VoicePool::SlotState::OriginalCallbackHeld,
            native.state.load(std::memory_order_acquire) == VoicePool::SlotState::Rendering,
            native.state.load(std::memory_order_acquire) == VoicePool::SlotState::Free};
    }
    static bool BorrowActualCallbackActor(VoicePool& pool, std::uint32_t slot,
        OriginalSourceMutationActor* out) {
        assert(slot < pool.m_slots.size());
        auto& lease = pool.m_originalCallbackStorage[slot];
        // This borrows the already issued authentic actor. A numeric index
        // cannot issue a root actor or fake a native standing/installation.
        return lease.storageHeld &&
            pool.m_slots[slot].originalCallbackStorageReader.load(std::memory_order_acquire) &&
            lease.actor.Borrow(out);
    }
    static std::uint64_t OriginalOwnerCount(const VoicePool& pool) {
        return pool.m_liveOriginalVoiceOwners.load(std::memory_order_relaxed);
    }
    static void SetCancelTimelineReadHook(VoicePool& pool, void (*hook)(void*), void* context) {
        pool.m_cancelTimelineReadContext.store(context, std::memory_order_relaxed);
        pool.m_cancelTimelineReadHook.store(hook, std::memory_order_release);
    }
    static void SetReleaseHeldHook(VoicePool& pool, void (*hook)(void*), void* context) {
        pool.m_releaseHeldContextForTest.store(context, std::memory_order_relaxed);
        pool.m_releaseHeldHookForTest.store(hook, std::memory_order_release);
    }
    static void SetReleaseSequence(VoicePool& pool, std::uint64_t sequence) {
        const bool prepared = pool.m_slots.ConstructThrough(1);
        assert(prepared);
        (void)prepared;
        pool.m_slots[0].releaseSequence.store(sequence, std::memory_order_relaxed);
    }
    static void Quarantine(VoicePool& pool, NotificationQueue& notifications) {
        pool.QuarantineSlot(0, pool.m_slots[0], notifications, 1);
    }
    // Native fault-exit control; it does not manufacture a release receipt.
    static void QuarantineOriginalSlot(VoicePool& pool, NotificationQueue& notifications,
        std::uint32_t slot, std::uint64_t block) {
        assert(slot < pool.m_slots.size());
        pool.QuarantineSlot(slot, pool.m_slots[slot], notifications, block);
    }
    static std::uint32_t OriginalSlotGeneration(const VoicePool& pool, std::uint32_t slot) {
        assert(slot < pool.m_slots.size());
        return pool.m_slots[slot].generation.load(std::memory_order_acquire);
    }
    static void SetWorkerDeadline(VoicePool& pool, std::uint64_t ticks) {
        pool.m_workers.SetJobDeadlineTicksForTest(ticks);
    }
    static void JoinWorkers(VoicePool& pool) {
        pool.m_workers.Stop();
    }
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    // Configure only between blocks. Keep context alive through actual Stop/join.
    // These functions delay already-issued native work and never create a cap.
    static void SetOriginalSourceWorkerGates(VoicePool& pool,
        void (*coordinator)(void*, std::uint64_t) noexcept,
        void (*claimed)(void*, std::uint64_t, std::uint32_t) noexcept,
        void* context) {
        original_source_worker_test::Configure(pool.m_workers,
            coordinator, claimed, context);
    }
    // Atomic observations while real Stop is blocked by an actual claimed job.
    // They do not issue a Source/body capability or acknowledge a native end.
    static bool OriginalWorkerStopRequested(const VoicePool& pool) {
        return pool.m_workers.m_stop.load(std::memory_order_acquire);
    }
    static bool OriginalCallbackStorageReader(const VoicePool& pool, std::uint32_t slot) {
        assert(slot < pool.m_slots.size());
        return pool.m_slots[slot].originalCallbackStorageReader.load(std::memory_order_acquire);
    }
    struct OriginalWorkerJobObservation {
        std::uint64_t abandonedGeneration = 0;
        std::uint32_t abandonedJobCount = 0;
        std::uint32_t renderSlot = UINT32_MAX;
        std::uint32_t renderGeneration = 0;
        bool finishedAtAbandonment = true;
    };
    // Only after the real callback returned and before another parallel batch.
    // Job mappings and frozen classification are observation, not payload rights.
    static OriginalWorkerJobObservation ObserveOriginalWorkerJob(const VoicePool& pool,
        std::uint32_t job) {
        const auto& workers = pool.m_workers;
        assert(workers.m_abandonedGeneration != 0 && job < workers.m_abandonedJobCount &&
            job < pool.m_jobSlots.size() && job < pool.m_jobGenerations.size());
        return {workers.m_abandonedGeneration, workers.m_abandonedJobCount,
            pool.m_jobSlots[job], pool.m_jobGenerations[job],
            workers.JobCompletedForLastBatch(job)};
    }
    static bool IsActuallyQuarantined(const VoicePool& pool, std::uint32_t slot) {
        assert(slot < pool.m_slots.size());
        return pool.m_slots[slot].state.load(std::memory_order_acquire) == VoicePool::SlotState::Quarantined;
    }
#endif
};

} // namespace harken::detail
