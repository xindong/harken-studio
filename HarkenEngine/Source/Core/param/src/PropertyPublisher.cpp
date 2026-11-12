// Two-phase transactional publication: the staged batch is admitted
// with ONE whole-set check and pushed
// in stage order, or deferred whole with its admission reserved against
// single-command producers until it lands. The batch buffer is allocated
// once at construction and never grows.
#include "harken/param/PropertyPublisher.h"

#include <cassert>

namespace harken {

PropertyPublisher::PropertyPublisher(std::uint32_t batchCapacity)
    : m_batchCapacity(batchCapacity) {
    m_batch.resize(batchCapacity);
}

bool PropertyPublisher::Stage(const PropertyCommit& commit) {
    if (m_stagedCount >= m_batchCapacity) {
        // Only reachable when the wiring exceeds the section-10.4 bound;
        // the caller keeps the target dirty and re-presents next tick.
        return false;
    }
    m_batch[m_stagedCount] = commit;
    ++m_stagedCount;
    return true;
}

bool PropertyPublisher::StageSpatial(const PropertyCommit& commit,
                                      std::uint32_t& groupIndex) {
    if (groupIndex >= m_stagedCount) {
        const auto index = m_stagedCount;
        if (!Stage(commit)) return false;
        groupIndex = index;
        return true;
    }
    auto& group = m_batch[groupIndex];
    assert(group.scopeKind == commit.scopeKind && group.objectId == commit.objectId);
    if ((commit.spatialMask & kSpatialPropertyPan) != 0) {
        group.targetId = commit.targetId;
        group.resolved = commit.resolved;
        group.pan2d = commit.pan2d;
    }
    if ((commit.spatialMask & kSpatialPropertyWidth) != 0)
        group.spatialWidth = commit.spatialWidth;
    group.spatialMask |= commit.spatialMask;
    return true;
}

std::uint32_t PropertyPublisher::StagedCount() const {
    return m_stagedCount;
}

bool PropertyPublisher::Publish(FreeSlotsFn freeSlots, PushFn push,
                                void* ctx) {
    if (m_stagedCount == 0) {
        // An empty set publishes trivially, no admission query. A pending
        // deferral rebuilt to empty is complete: nothing awaits admission,
        // so the reservation lifts.
        m_deferredPending = false;
        return true;
    }
    // ONE admission check for the whole set. Sound under SPSC: this thread
    // is the only producer, the consumer only frees space, so the answer
    // cannot shrink between the check and the pushes below.
    const std::uint32_t free = freeSlots(ctx);
    if (free < m_stagedCount) {
        // No room for the whole set: push NOTHING (zero partial
        // application), keep the batch staged, count the deferral, and
        // hold the admission reservation until the batch lands.
        m_deferredPending = true;
        ++m_deferredCount;
        return false;
    }
    for (std::uint32_t i = 0; i < m_stagedCount; ++i) {
        const bool accepted = push(ctx, m_batch[i]);
        (void)accepted;
        assert(accepted && "push refused after whole-set admission");
    }
    m_stagedCount = 0;
    m_deferredPending = false;
    return true;
}

void PropertyPublisher::Restage() {
    // The deferred flag survives: the reservation holds across the rebuild
    // window and tracks whatever the fresh gather stages next.
    m_stagedCount = 0;
}

bool PropertyPublisher::AdmitSingle(FreeSlotsFn freeSlots, void* ctx) const {
    // A single command needs one slot beyond the deferred batch's
    // reservation; with nothing deferred this reduces to FreeSlots > 0.
    return freeSlots(ctx) > PendingReservation();
}

std::uint32_t PropertyPublisher::PendingReservation() const {
    return m_deferredPending ? m_stagedCount : 0;
}

std::uint64_t PropertyPublisher::DeferredCount() const {
    return m_deferredCount;
}

bool PropertyPublisher::TryGrowWorstCase(std::uint32_t addedUniqueTargets,
                                         LogRing* log) {
    const std::uint64_t proposed =
        static_cast<std::uint64_t>(m_worstCaseCommits) + addedUniqueTargets;
    if (proposed > m_batchCapacity) {
        if (log != nullptr) {
            LogRecord record{};
            record.level = VN_LOG_WARN;
            record.code = kLogCodePropertyCapacityRefused;
            record.arg0 = proposed;
            record.arg1 = m_batchCapacity;
            log->Push(record);
        }
        return false;
    }
    m_worstCaseCommits = static_cast<std::uint32_t>(proposed);
    return true;
}

void PropertyPublisher::ShrinkWorstCase(
    std::uint32_t removedUniqueTargets) noexcept {
    m_worstCaseCommits = (removedUniqueTargets < m_worstCaseCommits)
                             ? m_worstCaseCommits - removedUniqueTargets
                             : 0;
}

std::uint32_t PropertyPublisher::WorstCaseCommits() const {
    return m_worstCaseCommits;
}

std::uint32_t PropertyPublisher::BatchCapacity() const {
    return m_batchCapacity;
}

} // namespace harken
