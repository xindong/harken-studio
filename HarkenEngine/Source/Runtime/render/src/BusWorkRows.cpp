#include "harken/render/BusTree.h"

namespace harken {

BusTree::WorkRow* BusTree::ResolveWorkRow(WorkRef ref) noexcept {
    auto* row = m_workRows.TryAt(ref.slot);
    return row != nullptr && ref.generation != 0 && row->generation == ref.generation &&
        row->phase.load(std::memory_order_acquire) != WorkPhase::Free ? row : nullptr;
}

BusTree::WorkRef BusTree::PrepareWorkRow() noexcept {
    DrainWorkRows();
    try {
        std::uint32_t slot;
        if (m_freeWorkRows.empty()) {
            slot = m_workRows.ConstructedCount();
            if (slot == UINT32_MAX) return {};
            // Every issued row can return without allocating in a destructor.
            m_freeWorkRows.reserve(static_cast<std::size_t>(slot) + 1);
            if (!m_workRows.ConstructThrough(slot + 1)) return {};
        } else {
            slot = m_freeWorkRows.back();
            m_freeWorkRows.pop_back();
        }
        auto& row = m_workRows[slot];
        assert(row.generation != UINT64_MAX);
        ++row.generation;
        row.slot = slot;
        row.bus = {};
        row.host = {};
        row.hostId = {};
        row.logicEffects = {};
        row.next = nullptr;
        row.observed = {};
        row.nextRecoveryProbeFrame = 0;
        row.recoveryProbeDelay = 0;
        row.references.store(1, std::memory_order_relaxed);
        row.phase.store(WorkPhase::Preparing, std::memory_order_release);
        ++m_workRowsInUse;
        return {slot, row.generation};
    } catch (...) { return {}; }
}

BusTree::Bus* BusTree::PreparedWorkRow(WorkRef ref) noexcept {
    auto* row = ResolveWorkRow(ref);
    return row != nullptr && row->phase.load(std::memory_order_acquire) == WorkPhase::Preparing
        ? &row->bus : nullptr;
}

bool BusTree::RetainWorkRow(WorkRef ref) noexcept {
    auto* row = ResolveWorkRow(ref);
    if (row == nullptr) return false;
    auto count = row->references.load(std::memory_order_relaxed);
    while (count != 0 && count != UINT32_MAX) {
        if (row->references.compare_exchange_weak(count, count + 1,
                std::memory_order_relaxed)) return true;
    }
    return false;
}

void BusTree::RecycleWorkRow(WorkRow& row) noexcept {
    assert(row.references.load(std::memory_order_relaxed) == 0);
    row.bus = {};
    row.phase.store(WorkPhase::Free, std::memory_order_release);
    --m_workRowsInUse;
    if (row.generation != UINT64_MAX) m_freeWorkRows.push_back(row.slot);
}

void BusTree::ReleaseWorkRow(WorkRef ref) noexcept {
    auto* row = ResolveWorkRow(ref);
    if (row == nullptr) return;
    const auto previous = row->references.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous != 0);
    if (previous != 1) return;
    if (row->phase.load(std::memory_order_acquire) == WorkPhase::Preparing) {
        for (const auto& effect : row->bus.fxSlotEffectRef) {
            if (effect.type == kEffectTypeNone) continue;
            const bool cancelled = m_effectBank.CancelPrepared(effect);
            assert(cancelled);
            (void)cancelled;
        }
        RecycleWorkRow(*row);
        return;
    }
    // Published effects have one retirement producer: the audio coordinator.
    // A zero reference count seals this incarnation against new candidates.
    auto* head = m_workRetireRequests.load(std::memory_order_relaxed);
    do { row->next = head; }
    while (!m_workRetireRequests.compare_exchange_weak(head, row,
        std::memory_order_release, std::memory_order_relaxed));
}

bool BusTree::PublishWorkRow(WorkRef ref) noexcept {
    auto* row = ResolveWorkRow(ref);
    if (row == nullptr || row->phase.load(std::memory_order_acquire) != WorkPhase::Preparing ||
        row->references.load(std::memory_order_relaxed) == 0) return false;
    row->host = {row->bus.busIndex, row->bus.generation};
    row->hostId = row->bus.id;
    for (const auto& effect : row->bus.fxSlotEffectRef) {
        if (effect.type == kEffectTypeNone) continue;
        const bool committed = m_effectBank.CommitPrepared(effect);
        assert(committed && "private Bus preparation owns each exact effect");
        if (!committed) std::terminate();
        m_effectBank.PublishInstalled(effect);
    }
    row->phase.store(WorkPhase::Published, std::memory_order_release);
    return true;
}

void BusTree::BindInitialWorkIdentity(std::uint32_t index) noexcept {
    auto& row = m_workRows[m_workSlots[index].slot];
    assert(!row.host.Valid());
    row.host = {row.bus.busIndex, row.bus.generation};
    row.hostId = row.bus.id;
}

BusTree::WorkRef BusTree::AudioWorkRef(std::uint32_t index) const noexcept {
    return index < m_audioCount ? m_workSlots[index] : WorkRef{};
}

bool BusTree::AudioAdoptWorkRow(BusSlotRef bus, WorkRef ref) noexcept {
    auto* row = ResolveWorkRow(ref);
    if (row == nullptr || row->phase.load(std::memory_order_acquire) != WorkPhase::Published ||
        !AudioRefMatches(bus) || row->bus.busIndex != bus.index ||
        row->bus.generation != bus.generation) return false;
    if (m_workSlots[bus.index] == ref) return true;
    if (!RetainWorkRow(ref)) return false;
    const auto old = m_workSlots[bus.index];
    m_workSlots[bus.index] = ref;
    AudioReleaseWorkRow(old);
    return true;
}

bool BusTree::AudioDetachWorkRow(BusSlotRef bus) noexcept {
    if (bus.index == 0 || !AudioRefMatches(bus)) return false;
    const auto old = m_workSlots[bus.index];
    m_workSlots[bus.index] = {};
    AudioReleaseWorkRow(old);
    return true;
}

void BusTree::RetireWorkRow(WorkRow& row) noexcept {
    assert(row.references.load(std::memory_order_acquire) == 0);
    RetireBusSlotEffects(row.bus);
    row.phase.store(WorkPhase::Retired, std::memory_order_release);
    auto* head = m_workRetired.load(std::memory_order_relaxed);
    do { row.next = head; }
    while (!m_workRetired.compare_exchange_weak(head, &row,
        std::memory_order_release, std::memory_order_relaxed));
}

void BusTree::AudioReleaseWorkRow(WorkRef ref) noexcept {
    auto* row = ResolveWorkRow(ref);
    if (row == nullptr) return;
    const auto previous = row->references.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous != 0);
    if (previous == 1) RetireWorkRow(*row);
}

void BusTree::ServiceWorkRowRetirements() noexcept {
    auto* row = m_workRetireRequests.exchange(nullptr, std::memory_order_acquire);
    while (row != nullptr) {
        auto* next = row->next;
        RetireWorkRow(*row);
        row = next;
    }
}

void BusTree::DrainWorkRows() noexcept {
    auto* row = m_workRetired.exchange(nullptr, std::memory_order_acquire);
    if (row == nullptr) return;
    m_effectBank.DrainRetire();
    while (row != nullptr) {
        auto* next = row->next;
        RecycleWorkRow(*row);
        row = next;
    }
}

void BusTree::ShutdownWorkRows() noexcept {
    // Engine destruction has joined workers and excluded every callback.
    m_workRetireRequests.store(nullptr, std::memory_order_relaxed);
    m_workRetired.store(nullptr, std::memory_order_relaxed);
    m_effectBank.DrainRetire();
    for (auto& row : m_workRows) {
        if (row.phase.load(std::memory_order_relaxed) == WorkPhase::Free) continue;
        ReleasePreviewEffectChain(row.bus, true);
        for (auto& effect : row.bus.fxSlotEffectRef) {
            if (effect.type != kEffectTypeNone) m_effectBank.Recycle(effect);
            effect = {};
        }
    }
}

} // namespace harken
