#include "harken/core/VariableSliceTable.h"

#include <new>
#include <utility>

namespace harken {

bool VariableSliceTable::Reset(std::uint32_t capacity,
                               std::uint32_t maxTargetsPerSlice,
                               std::uint32_t targetPoolCapacity) {
    if (capacity == 0 || capacity > kVariableSliceRowHardLimit ||
        maxTargetsPerSlice > kVariableSliceTargetHardLimit ||
        targetPoolCapacity > kVariableSliceTargetPoolHardLimit) {
        return false;
    }
    try {
        auto rows = std::make_unique<Row[]>(capacity);
        std::vector<VariableSliceTarget> targets(targetPoolCapacity);
        std::vector<FreeRange> freeRanges;
        freeRanges.reserve(static_cast<std::size_t>(capacity) + 1);
        if (targetPoolCapacity != 0) {
            freeRanges.push_back({0, targetPoolCapacity});
        }
        for (std::uint32_t slot = 0; slot < capacity; ++slot) {
            rows[slot].identity.store(
                (static_cast<std::uint64_t>(1) << 32) | slot,
                std::memory_order_relaxed);
        }
        rows_ = std::move(rows);
        targets_ = std::move(targets);
        freeRanges_ = std::move(freeRanges);
    } catch (const std::bad_alloc&) {
        return false;
    }
    capacity_ = capacity;
    maxTargetsPerSlice_ = maxTargetsPerSlice;
    targetPoolCapacity_ = targetPoolCapacity;
    return true;
}

VariableSliceTable::Row* VariableSliceTable::Resolve(
    VariableSliceRef ref) noexcept {
    if (!ref.Valid() || ref.slot >= capacity_) {
        return nullptr;
    }
    Row& row = rows_[ref.slot];
    const std::uint64_t identity = row.identity.load(std::memory_order_acquire);
    if (static_cast<std::uint32_t>(identity) != ref.slot ||
        static_cast<std::uint32_t>(identity >> 32) != ref.generation) {
        return nullptr;
    }
    return &row;
}

const VariableSliceTable::Row* VariableSliceTable::Resolve(
    VariableSliceRef ref) const noexcept {
    return const_cast<VariableSliceTable*>(this)->Resolve(ref);
}

bool VariableSliceTable::Reserve(std::uint64_t world, std::uint64_t transport,
                                 std::uint64_t ticket,
                                 std::uint64_t effectiveFrame,
                                 std::uint64_t bankOrdinal,
                                 std::uint32_t exactTargetCount,
                                 VariableSliceRef* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    if (exactTargetCount == 0 || exactTargetCount > maxTargetsPerSlice_ ||
        exactTargetCount > targetPoolCapacity_) {
        return false;
    }
    std::uint32_t freeSlot = UINT32_MAX;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].state.load(std::memory_order_acquire) ==
            VariableSliceState::Free) {
            freeSlot = slot;
            break;
        }
    }
    if (freeSlot == UINT32_MAX) {
        return false;
    }
    std::size_t rangeIndex = freeRanges_.size();
    for (std::size_t i = 0; i < freeRanges_.size(); ++i) {
        if (freeRanges_[i].count >= exactTargetCount) {
            rangeIndex = i;
            break;
        }
    }
    if (rangeIndex == freeRanges_.size()) {
        return false;
    }

    const std::uint32_t spanOffset = freeRanges_[rangeIndex].offset;
    if (freeRanges_[rangeIndex].count == exactTargetCount) {
        freeRanges_.erase(freeRanges_.begin() +
                          static_cast<std::ptrdiff_t>(rangeIndex));
    } else {
        freeRanges_[rangeIndex].offset += exactTargetCount;
        freeRanges_[rangeIndex].count -= exactTargetCount;
    }

    Row& row = rows_[freeSlot];
    const std::uint64_t identity =
        row.identity.load(std::memory_order_acquire);
    row.world = world;
    row.transport = transport;
    row.ticket = ticket;
    row.effectiveFrame = effectiveFrame;
    row.bankOrdinal = bankOrdinal;
    row.targetSpanOffset = spanOffset;
    row.targetSpanCapacity = exactTargetCount;
    row.targetCount = 0;
    row.appliedBlockSequence.store(0, std::memory_order_relaxed);
    row.pendingInvalidationReasons.store(0, std::memory_order_relaxed);
    row.state.store(VariableSliceState::Prepared, std::memory_order_release);
    out->slot = freeSlot;
    out->generation = static_cast<std::uint32_t>(identity >> 32);
    return true;
}

bool VariableSliceTable::AddTarget(
    VariableSliceRef ref, const VariableSliceTarget& target) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            VariableSliceState::Prepared ||
        row->targetCount >= row->targetSpanCapacity ||
        row->targetSpanOffset > targetPoolCapacity_ ||
        row->targetCount >= targetPoolCapacity_ - row->targetSpanOffset) {
        return false;
    }
    targets_[row->targetSpanOffset + row->targetCount] = target;
    ++row->targetCount;
    return true;
}

bool VariableSliceTable::Publish(VariableSliceRef ref) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr) {
        return false;
    }
    if (row->targetCount != row->targetSpanCapacity) {
        return false;
    }
    VariableSliceState expected = VariableSliceState::Prepared;
    return row->state.compare_exchange_strong(
        expected, VariableSliceState::PageCommitted,
        std::memory_order_release, std::memory_order_relaxed);
}

VariableSliceInvalidationResult VariableSliceTable::Invalidate(
    VariableSliceRef ref, std::uint32_t reason) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr || reason == 0) {
        return VariableSliceInvalidationResult::InvalidReference;
    }
    row->pendingInvalidationReasons.fetch_or(reason, std::memory_order_acq_rel);
    VariableSliceState expected = VariableSliceState::PageCommitted;
    if (row->state.compare_exchange_strong(
            expected, VariableSliceState::Invalidated,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return VariableSliceInvalidationResult::InvalidatedBeforeClaim;
    }
    switch (expected) {
    case VariableSliceState::AudioClaimed:
        return VariableSliceInvalidationResult::AudioClaimedApplyWhole;
    case VariableSliceState::SliceApplied:
        return VariableSliceInvalidationResult::AlreadyApplied;
    case VariableSliceState::Invalidated:
        return VariableSliceInvalidationResult::AlreadyInvalidated;
    case VariableSliceState::Free:
    case VariableSliceState::Prepared:
    case VariableSliceState::Quarantined:
    case VariableSliceState::PageCommitted:
        return VariableSliceInvalidationResult::NotPublished;
    }
    return VariableSliceInvalidationResult::NotPublished;
}

bool VariableSliceTable::Apply(
    VariableSliceRef ref, std::uint64_t blockSequence, void* context,
    bool (*validate)(void*, const VariableSliceTarget&) noexcept,
    void (*apply)(void*, const VariableSliceTarget&) noexcept) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr || validate == nullptr || apply == nullptr) {
        return false;
    }
    VariableSliceState expected = VariableSliceState::PageCommitted;
    if (!row->state.compare_exchange_strong(
            expected, VariableSliceState::AudioClaimed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (expected == VariableSliceState::Invalidated) {
            std::uint64_t expectedFence = 0;
            (void)row->appliedBlockSequence.compare_exchange_strong(
                expectedFence, blockSequence, std::memory_order_release,
                std::memory_order_relaxed);
        }
        return false;
    }
#if !defined(HARKEN_RT_QUALIFICATION)
    const AfterAudioClaimHookForTest hook =
        afterAudioClaimHookForTest_.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(afterAudioClaimHookContextForTest_.load(
                 std::memory_order_relaxed),
             ref);
    }
#endif
    const std::uint32_t base = row->targetSpanOffset;
    if (row->targetCount != row->targetSpanCapacity ||
        base > targetPoolCapacity_ ||
        row->targetCount > targetPoolCapacity_ - base) {
        row->appliedBlockSequence.store(blockSequence,
                                        std::memory_order_release);
        row->state.store(VariableSliceState::Invalidated,
                         std::memory_order_release);
        return false;
    }
    for (std::uint32_t i = 0; i < row->targetCount; ++i) {
        if (!validate(context, targets_[base + i])) {
            row->appliedBlockSequence.store(blockSequence,
                                            std::memory_order_release);
            row->state.store(VariableSliceState::Invalidated,
                             std::memory_order_release);
            return false;
        }
    }
    for (std::uint32_t i = 0; i < row->targetCount; ++i) {
        apply(context, targets_[base + i]);
    }
    row->appliedBlockSequence.store(blockSequence, std::memory_order_release);
    row->state.store(VariableSliceState::SliceApplied,
                     std::memory_order_release);
    return true;
}

VariableSliceState VariableSliceTable::State(VariableSliceRef ref) const noexcept {
    const Row* row = Resolve(ref);
    return row == nullptr ? VariableSliceState::Free
                          : row->state.load(std::memory_order_acquire);
}

std::uint64_t VariableSliceTable::AppliedBlockSequence(
    VariableSliceRef ref) const noexcept {
    const Row* row = Resolve(ref);
    return row == nullptr
               ? 0
               : row->appliedBlockSequence.load(std::memory_order_acquire);
}

std::uint32_t VariableSliceTable::TargetCount(
    VariableSliceRef ref) const noexcept {
    const Row* row = Resolve(ref);
    if (row == nullptr) {
        return 0;
    }
    const VariableSliceState state =
        row->state.load(std::memory_order_acquire);
    return state == VariableSliceState::Free ||
                   state == VariableSliceState::Quarantined
               ? 0
               : row->targetCount;
}

VariableSliceTarget* VariableSliceTable::Target(
    VariableSliceRef ref, std::uint32_t index) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            VariableSliceState::Prepared ||
        index >= row->targetCount ||
        row->targetSpanOffset > targetPoolCapacity_ ||
        index >= targetPoolCapacity_ - row->targetSpanOffset) {
        return nullptr;
    }
    return &targets_[row->targetSpanOffset + index];
}

const VariableSliceTarget* VariableSliceTable::Target(
    VariableSliceRef ref, std::uint32_t index) const noexcept {
    const Row* row = Resolve(ref);
    if (row == nullptr) {
        return nullptr;
    }
    const VariableSliceState state =
        row->state.load(std::memory_order_acquire);
    if (state == VariableSliceState::Free ||
        state == VariableSliceState::Quarantined ||
        index >= row->targetCount ||
        row->targetSpanOffset > targetPoolCapacity_ ||
        index >= targetPoolCapacity_ - row->targetSpanOffset) {
        return nullptr;
    }
    return &targets_[row->targetSpanOffset + index];
}

void VariableSliceTable::ReclaimSettled(
    std::uint64_t observedBlockSequence) noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        const VariableSliceState state =
            row.state.load(std::memory_order_acquire);
        if (state != VariableSliceState::SliceApplied &&
            state != VariableSliceState::Invalidated) {
            continue;
        }
        const std::uint64_t identity =
            row.identity.load(std::memory_order_acquire);
        const VariableSliceRef ref{
            slot, static_cast<std::uint32_t>(identity >> 32)};
        (void)Reclaim(ref, observedBlockSequence);
    }
}

bool VariableSliceTable::HasUnreclaimedWorldRows(
    std::uint64_t world) const noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        const VariableSliceState state =
            row.state.load(std::memory_order_acquire);
        if (state != VariableSliceState::Free &&
            state != VariableSliceState::Quarantined && row.world == world) {
            return true;
        }
    }
    return false;
}

bool VariableSliceTable::HasUnreclaimedTransportRows(
    std::uint64_t transport) const noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        const VariableSliceState state =
            row.state.load(std::memory_order_acquire);
        if (state != VariableSliceState::Free &&
            state != VariableSliceState::Quarantined &&
            row.transport == transport) {
            return true;
        }
    }
    return false;
}

bool VariableSliceTable::AbandonPrepared(VariableSliceRef ref) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            VariableSliceState::Prepared) {
        return false;
    }
    return RecycleRow(*row, ref);
}

bool VariableSliceTable::Reclaim(VariableSliceRef ref,
                                 std::uint64_t observedBlockSequence) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr) {
        return false;
    }
    const VariableSliceState state = row->state.load(std::memory_order_acquire);
    if (state != VariableSliceState::SliceApplied &&
        state != VariableSliceState::Invalidated) {
        return false;
    }
    const std::uint64_t applied =
        row->appliedBlockSequence.load(std::memory_order_acquire);
    if (state == VariableSliceState::Invalidated && applied == 0) {
        return false;
    }
    if (observedBlockSequence <= applied) {
        return false;
    }
    return RecycleRow(*row, ref);
}

bool VariableSliceTable::ReleaseSpan(std::uint32_t offset,
                                     std::uint32_t count) noexcept {
    if (count == 0 || offset > targetPoolCapacity_ ||
        count > targetPoolCapacity_ - offset) {
        return false;
    }
    std::size_t next = 0;
    while (next < freeRanges_.size() && freeRanges_[next].offset < offset) {
        ++next;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(offset) + count;
    if (next != 0) {
        const FreeRange& previous = freeRanges_[next - 1];
        const std::uint64_t previousEnd =
            static_cast<std::uint64_t>(previous.offset) + previous.count;
        if (previousEnd > offset) {
            return false;
        }
    }
    if (next < freeRanges_.size() && end > freeRanges_[next].offset) {
        return false;
    }

    const bool mergePrevious =
        next != 0 &&
        static_cast<std::uint64_t>(freeRanges_[next - 1].offset) +
                freeRanges_[next - 1].count ==
            offset;
    const bool mergeNext =
        next < freeRanges_.size() && end == freeRanges_[next].offset;
    if (mergePrevious && mergeNext) {
        FreeRange& previous = freeRanges_[next - 1];
        const std::uint64_t combined =
            static_cast<std::uint64_t>(previous.count) + count +
            freeRanges_[next].count;
        if (combined > targetPoolCapacity_) {
            return false;
        }
        previous.count = static_cast<std::uint32_t>(combined);
        freeRanges_.erase(freeRanges_.begin() +
                          static_cast<std::ptrdiff_t>(next));
    } else if (mergePrevious) {
        const std::uint64_t combined =
            static_cast<std::uint64_t>(freeRanges_[next - 1].count) + count;
        if (combined > targetPoolCapacity_) {
            return false;
        }
        freeRanges_[next - 1].count =
            static_cast<std::uint32_t>(combined);
    } else if (mergeNext) {
        const std::uint64_t combined =
            static_cast<std::uint64_t>(freeRanges_[next].count) + count;
        if (combined > targetPoolCapacity_) {
            return false;
        }
        freeRanges_[next].offset = offset;
        freeRanges_[next].count = static_cast<std::uint32_t>(combined);
    } else {
        if (freeRanges_.size() >= freeRanges_.capacity()) {
            return false;
        }
        freeRanges_.insert(freeRanges_.begin() +
                               static_cast<std::ptrdiff_t>(next),
                           FreeRange{offset, count});
    }
    return true;
}

bool VariableSliceTable::RecycleRow(Row& row,
                                    VariableSliceRef ref) noexcept {
    const std::uint64_t identity = row.identity.load(std::memory_order_relaxed);
    const std::uint32_t generation = static_cast<std::uint32_t>(identity >> 32);
    if (generation == UINT32_MAX) {
        row.state.store(VariableSliceState::Quarantined,
                         std::memory_order_release);
        return false;
    }
    if (!ReleaseSpan(row.targetSpanOffset, row.targetSpanCapacity)) {
        row.state.store(VariableSliceState::Quarantined,
                         std::memory_order_release);
        return false;
    }
    row.world = 0;
    row.transport = 0;
    row.ticket = 0;
    row.effectiveFrame = 0;
    row.bankOrdinal = 0;
    row.targetSpanOffset = 0;
    row.targetSpanCapacity = 0;
    row.targetCount = 0;
    row.appliedBlockSequence.store(0, std::memory_order_relaxed);
    row.pendingInvalidationReasons.store(0, std::memory_order_relaxed);
    row.identity.store((static_cast<std::uint64_t>(generation + 1) << 32) |
                            ref.slot,
                        std::memory_order_release);
    row.state.store(VariableSliceState::Free, std::memory_order_release);
    return true;
}

std::uint32_t VariableSliceTable::FreeTargetCapacity() const noexcept {
    std::uint32_t total = 0;
    for (const FreeRange& range : freeRanges_) {
        total += range.count;
    }
    return total;
}

std::uint32_t VariableSliceTable::FreeRowCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].state.load(std::memory_order_acquire) ==
            VariableSliceState::Free) {
            ++count;
        }
    }
    return count;
}

bool VariableSliceTable::SetFreeRowGenerationForTest(
    std::uint32_t slot, std::uint32_t generation) noexcept {
    if (slot >= capacity_ || generation == 0 ||
        rows_[slot].state.load(std::memory_order_acquire) !=
            VariableSliceState::Free) {
        return false;
    }
    rows_[slot].identity.store(
        (static_cast<std::uint64_t>(generation) << 32) | slot,
        std::memory_order_release);
    return true;
}

} // namespace harken
