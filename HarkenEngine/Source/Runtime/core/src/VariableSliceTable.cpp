#include "harken/core/VariableSliceTable.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace harken {

bool VariableSliceTable::Reset(std::uint32_t capacity,
                               std::uint32_t maxTargetsPerSlice,
                               std::uint32_t targetPoolCapacity,
                               std::uint32_t payloadCapacity) {
    if (memguard::IsAudioTagged()) return false;
    if (payloadCapacity == 0) payloadCapacity = capacity;
    if (capacity == 0 || capacity > kVariableSliceRowHardLimit ||
        payloadCapacity > capacity ||
        maxTargetsPerSlice > kVariableSliceTargetHardLimit ||
        targetPoolCapacity > kVariableSliceTargetPoolHardLimit) {
        return false;
    }
    const auto sourceWords = (static_cast<std::uint64_t>(capacity) + 63u) / 64u;
    if (sourceWords * payloadCapacity > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t))
        return false;
    try {
        StableRowStorage<Row> rows;
        if (!rows.Reserve(capacity)) return false;
        std::vector<Payload> payloads(payloadCapacity);
        std::vector<std::uint32_t> freeRows, freePayloads;
        freeRows.reserve(capacity);
        freePayloads.reserve(payloadCapacity);
        std::vector<VariableSliceTarget> targets(targetPoolCapacity);
        std::vector<std::uint64_t> sourceBits(static_cast<std::size_t>(sourceWords * payloadCapacity));
        std::vector<FreeRange> freeRanges;
        freeRanges.reserve(static_cast<std::size_t>(payloadCapacity) + 1);
        if (targetPoolCapacity != 0) {
            freeRanges.push_back({0, targetPoolCapacity});
        }
        for (std::uint32_t slot = 0; slot < capacity; ++slot) {
            freeRows.push_back(capacity - slot - 1u);
        }
        for (std::uint32_t slot = payloadCapacity; slot > 0; --slot)
            freePayloads.push_back(slot - 1u);
        rows_.Swap(rows);
        payloads_ = std::move(payloads);
        freeRows_ = std::move(freeRows);
        freePayloads_ = std::move(freePayloads);
        targets_ = std::move(targets);
        sourceBits_ = std::move(sourceBits);
        sourceWords_ = static_cast<std::uint32_t>(sourceWords);
        freeRanges_ = std::move(freeRanges);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    capacity_ = capacity;
    maxTargetsPerSlice_ = maxTargetsPerSlice;
    targetPoolCapacity_ = targetPoolCapacity;
    payloadCapacity_ = payloadCapacity;
    return true;
}

VariableSliceTable::Row* VariableSliceTable::Resolve(
    VariableSliceRef ref) noexcept {
    if (!ref.Valid() || ref.slot >= rows_.ConstructedCount()) {
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

bool VariableSliceTable::PrepareRow(std::uint32_t slot) noexcept {
    const auto previous = rows_.ConstructedCount();
    if (slot >= capacity_ || !rows_.ConstructThrough(slot + 1u)) return false;
    for (auto index = previous; index <= slot; ++index)
        rows_[index].identity.store((std::uint64_t{1} << 32) | index, std::memory_order_relaxed);
    return true;
}

VariableSliceTable::Payload* VariableSliceTable::ResolvePayload(Row* row) noexcept {
    return row != nullptr && row->payload < payloadCapacity_ ? &payloads_[row->payload] : nullptr;
}

const VariableSliceTable::Payload* VariableSliceTable::ResolvePayload(const Row* row) const noexcept {
    return row != nullptr && row->payload < payloadCapacity_ ? &payloads_[row->payload] : nullptr;
}

VariableSliceTable::ReservationStatus VariableSliceTable::ReservationAvailability(
    std::uint32_t exactTargetCount) const noexcept {
    if (exactTargetCount > maxTargetsPerSlice_ || exactTargetCount > targetPoolCapacity_)
        return ReservationStatus::TargetLimit;
    if (freeRows_.empty()) return ReservationStatus::ProofCapacity;
    if (freePayloads_.empty()) return ReservationStatus::PayloadCapacity;
    if (exactTargetCount != 0 && std::none_of(freeRanges_.begin(), freeRanges_.end(),
            [&](const FreeRange& range) { return range.count >= exactTargetCount; }))
        return ReservationStatus::TargetSpan;
    return ReservationStatus::Ready;
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
    if (ReservationAvailability(exactTargetCount) != ReservationStatus::Ready) return false;
    const auto freeSlot = freeRows_.back();
    std::size_t rangeIndex = freeRanges_.size();
    for (std::size_t i = 0; exactTargetCount != 0 && i < freeRanges_.size(); ++i) {
        if (freeRanges_[i].count >= exactTargetCount) {
            rangeIndex = i;
            break;
        }
    }
    if (exactTargetCount != 0 && rangeIndex == freeRanges_.size()) {
        return false;
    }

    const std::uint32_t spanOffset = exactTargetCount == 0 ? 0 : freeRanges_[rangeIndex].offset;
    if (!PrepareRow(freeSlot)) return false;
    if (exactTargetCount == 0) {
        // Bank-only slices still own a row and an actual consumption receipt.
    } else if (freeRanges_[rangeIndex].count == exactTargetCount) {
        freeRanges_.erase(freeRanges_.begin() +
                          static_cast<std::ptrdiff_t>(rangeIndex));
    } else {
        freeRanges_[rangeIndex].offset += exactTargetCount;
        freeRanges_[rangeIndex].count -= exactTargetCount;
    }

    Row& row = rows_[freeSlot];
    const std::uint64_t identity =
        row.identity.load(std::memory_order_acquire);
    freeRows_.pop_back();
    row.payload = freePayloads_.back();
    freePayloads_.pop_back();
    auto& payload = payloads_[row.payload];
    const VariableSliceRef ref{freeSlot, static_cast<std::uint32_t>(identity >> 32)};
    row.source = {ref, world, transport, ticket, bankOrdinal};
    row.effectiveFrame = effectiveFrame;
    row.requiredCommandFrontier = 0;
    payload = {spanOffset, exactTargetCount, 0, 0};
    row.sourceObservers = 0;
    row.logicAcknowledged = false;
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
    Payload* payload = ResolvePayload(row);
    if (row == nullptr ||
        payload == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            VariableSliceState::Prepared ||
        payload->targetCount >= payload->targetSpanCapacity ||
        payload->targetSpanOffset > targetPoolCapacity_ ||
        payload->targetCount >= targetPoolCapacity_ - payload->targetSpanOffset) {
        return false;
    }
    targets_[payload->targetSpanOffset + payload->targetCount] = target;
    ++payload->targetCount;
    return true;
}

bool VariableSliceTable::SourceIdentityMatches(const Row& row,
                                              const VariableSliceSource& source) const noexcept {
    return row.source.world == source.world && row.source.transport == source.transport &&
        row.source.ticket == source.ticket && row.source.bankOrdinal == source.bankOrdinal;
}

bool VariableSliceTable::CaptureSource(VariableSliceRef ref, VariableSliceSource* out) const noexcept {
    const Row* row = Resolve(ref);
    if (row == nullptr || out == nullptr) return false;
    const auto state = row->state.load(std::memory_order_acquire);
    if (state == VariableSliceState::Free || state == VariableSliceState::Quarantined) return false;
    *out = row->source;
    return true;
}

bool VariableSliceTable::RetainSource(const VariableSliceSource& source) noexcept {
    Row* row = Resolve(source.slice);
    if (row == nullptr || !SourceIdentityMatches(*row, source) ||
        row->sourceObservers == UINT32_MAX) return false;
    const auto state = row->state.load(std::memory_order_acquire);
    if (state == VariableSliceState::Free || state == VariableSliceState::Quarantined) return false;
    ++row->sourceObservers;
    return true;
}

bool VariableSliceTable::ReleaseSource(const VariableSliceSource& source) noexcept {
    Row* row = Resolve(source.slice);
    if (row == nullptr || !SourceIdentityMatches(*row, source) || row->sourceObservers == 0) return false;
    --row->sourceObservers;
    return true;
}

bool VariableSliceTable::SourceApplied(const VariableSliceSource& source) const noexcept {
    const Row* row = Resolve(source.slice);
    return row != nullptr && row->state.load(std::memory_order_acquire) == VariableSliceState::SliceApplied &&
        SourceIdentityMatches(*row, source);
}

bool VariableSliceTable::AddSource(VariableSliceRef ref, const VariableSliceSource& source) noexcept {
    Row* row = Resolve(ref);
    Payload* payload = ResolvePayload(row);
    const Row* sourceRow = Resolve(source.slice);
    if (row == nullptr || payload == nullptr || sourceRow == nullptr || !SourceIdentityMatches(*sourceRow, source) ||
        row->state.load(std::memory_order_acquire) != VariableSliceState::Prepared ||
        source.world != row->source.world || source.transport != row->source.transport ||
        sourceRow->effectiveFrame > row->effectiveFrame ||
        (source.slice.slot == ref.slot && source.slice.generation == ref.generation)) return false;
    auto& word = sourceBits_[static_cast<std::size_t>(row->payload) * sourceWords_ + source.slice.slot / 64u];
    const auto mask = std::uint64_t{1} << (source.slice.slot % 64u);
    if ((word & mask) != 0) return true;
    if (!RetainSource(source)) return false;
    word |= mask;
    ++payload->sourceCount;
    return true;
}

std::uint32_t VariableSliceTable::SourceObserverCount(VariableSliceRef ref) const noexcept {
    const Row* row = Resolve(ref);
    return row == nullptr ? 0 : row->sourceObservers;
}

const VariableSliceSource* VariableSliceTable::Source(VariableSliceRef ref, std::uint32_t index) const noexcept {
    std::uint32_t cursor = 0;
    const VariableSliceSource* source = nullptr;
    for (std::uint32_t i = 0; i <= index; ++i) {
        source = NextSource(ref, &cursor);
        if (source == nullptr) break;
    }
    return source;
}

const VariableSliceSource* VariableSliceTable::NextSource(VariableSliceRef ref, std::uint32_t* cursor) const noexcept {
    const Row* row = Resolve(ref);
    const auto* payload = ResolvePayload(row);
    if (payload == nullptr || payload->sourceCount == 0 || cursor == nullptr || *cursor >= capacity_) return nullptr;
    const auto base = static_cast<std::size_t>(row->payload) * sourceWords_;
    for (auto wordIndex = *cursor / 64u; wordIndex < sourceWords_; ++wordIndex) {
        auto word = sourceBits_[base + wordIndex];
        if (wordIndex == *cursor / 64u) word &= UINT64_MAX << (*cursor % 64u);
        if (word == 0) continue;
        const auto slot = wordIndex * 64u + static_cast<std::uint32_t>(std::countr_zero(word));
        *cursor = slot + 1u;
        return &rows_[slot].source;
    }
    *cursor = capacity_;
    return nullptr;
}

bool VariableSliceTable::ClearPreparedSources(VariableSliceRef ref) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr || row->state.load(std::memory_order_acquire) != VariableSliceState::Prepared) return false;
    auto* payload = ResolvePayload(row);
    if (payload == nullptr) return false;
    std::uint32_t cursor = 0;
    while (const auto* source = NextSource(ref, &cursor)) (void)ReleaseSource(*source);
    const auto base = static_cast<std::size_t>(row->payload) * sourceWords_;
    std::fill_n(sourceBits_.begin() + base, sourceWords_, std::uint64_t{0});
    payload->sourceCount = 0;
    return true;
}

bool VariableSliceTable::SetPreparedEffectiveFrame(VariableSliceRef ref,
                                                   std::uint64_t frame) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr || row->state.load(std::memory_order_acquire) !=
                              VariableSliceState::Prepared) return false;
    std::uint32_t cursor = 0;
    while (const auto* source = NextSource(ref, &cursor)) {
        const Row* sourceRow = Resolve(source->slice);
        if (sourceRow == nullptr || !SourceIdentityMatches(*sourceRow, *source) ||
            sourceRow->effectiveFrame > frame) return false;
    }
    row->effectiveFrame = frame;
    return true;
}

std::uint64_t VariableSliceTable::EffectiveFrame(VariableSliceRef ref) const noexcept {
    const Row* row = Resolve(ref);
    return row == nullptr ? 0 : row->effectiveFrame;
}

bool VariableSliceTable::SetPreparedCommandFrontier(VariableSliceRef ref,
                                                    std::uint64_t frontier) noexcept {
    auto* row = Resolve(ref);
    if (row == nullptr || row->state.load(std::memory_order_acquire) != VariableSliceState::Prepared)
        return false;
    row->requiredCommandFrontier = frontier;
    return true;
}

std::uint32_t VariableSliceTable::SourceCount(VariableSliceRef ref) const noexcept {
    const auto* payload = ResolvePayload(Resolve(ref));
    return payload == nullptr ? 0 : payload->sourceCount;
}

std::uint32_t VariableSliceTable::InvalidationReasons(VariableSliceRef ref) const noexcept {
    const Row* row = Resolve(ref);
    return row == nullptr ? 0 : row->pendingInvalidationReasons.load(std::memory_order_acquire);
}

std::size_t VariableSliceTable::StorageBytes() const noexcept {
    return rows_.RequestedBytes() +
        payloads_.capacity() * sizeof(Payload) +
        (freeRows_.capacity() + freePayloads_.capacity()) * sizeof(std::uint32_t) +
        targets_.capacity() * sizeof(VariableSliceTarget) +
        sourceBits_.capacity() * sizeof(std::uint64_t) + freeRanges_.capacity() * sizeof(FreeRange);
}

bool VariableSliceTable::CanPublishPrepared(VariableSliceRef ref, bool allowPreparedSources) const noexcept {
    const Row* row = Resolve(ref);
    const auto* payload = ResolvePayload(row);
    if (row == nullptr || row->state.load(std::memory_order_acquire) != VariableSliceState::Prepared) {
        return false;
    }
    if (payload == nullptr || payload->targetCount != payload->targetSpanCapacity) {
        return false;
    }
    std::uint32_t cursor = 0;
    while (const auto* source = NextSource(ref, &cursor)) {
        const Row* sourceRow = Resolve(source->slice);
        if (sourceRow == nullptr || !SourceIdentityMatches(*sourceRow, *source) ||
            sourceRow->effectiveFrame > row->effectiveFrame) return false;
        const auto sourceState = sourceRow->state.load(std::memory_order_acquire);
        if ((!allowPreparedSources && sourceState == VariableSliceState::Prepared) ||
            sourceState == VariableSliceState::Free ||
            sourceState == VariableSliceState::Quarantined) return false;
    }
    return true;
}

bool VariableSliceTable::Publish(VariableSliceRef ref) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr || !CanPublishPrepared(ref)) return false;
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
    void (*apply)(void*, const VariableSliceTarget&) noexcept,
    std::uint64_t settledCommandFrontier) noexcept {
    Row* row = Resolve(ref);
    if (row == nullptr || blockSequence == 0 || validate == nullptr || apply == nullptr) {
        return false;
    }
    if (row->state.load(std::memory_order_acquire) == VariableSliceState::PageCommitted) {
        if (row->requiredCommandFrontier > settledCommandFrontier) return false;
        // A consumed page can precede a FIFO batch. Keep valid dependencies waiting.
        bool waiting = false;
        bool failed = false;
        std::uint32_t cursor = 0;
        while (const auto* source = NextSource(ref, &cursor)) {
            const auto* sourceRow = Resolve(source->slice);
            if (sourceRow == nullptr) {
                failed = true;
                continue;
            }
            const auto state = sourceRow->state.load(std::memory_order_acquire);
            if (!SourceIdentityMatches(*sourceRow, *source) || state == VariableSliceState::Invalidated ||
                state == VariableSliceState::Free || state == VariableSliceState::Quarantined) failed = true;
            else if (state != VariableSliceState::SliceApplied) waiting = true;
        }
        if (waiting && !failed) return false;
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
    std::uint32_t cursor = 0;
    while (const auto* source = NextSource(ref, &cursor)) {
        if (!SourceApplied(*source)) {
            row->pendingInvalidationReasons.fetch_or(kVariableSliceSourceRejected, std::memory_order_relaxed);
            row->appliedBlockSequence.store(blockSequence, std::memory_order_release);
            row->state.store(VariableSliceState::Invalidated, std::memory_order_release);
            return false;
        }
    }
    const auto* payload = ResolvePayload(row);
    const std::uint32_t base = payload == nullptr ? 0 : payload->targetSpanOffset;
    if (payload == nullptr || payload->targetCount != payload->targetSpanCapacity ||
        base > targetPoolCapacity_ ||
        payload->targetCount > targetPoolCapacity_ - base) {
        row->pendingInvalidationReasons.fetch_or(kVariableSliceTargetRejected, std::memory_order_relaxed);
        row->appliedBlockSequence.store(blockSequence,
                                        std::memory_order_release);
        row->state.store(VariableSliceState::Invalidated,
                         std::memory_order_release);
        return false;
    }
    for (std::uint32_t i = 0; i < payload->targetCount; ++i) {
        if (!validate(context, targets_[base + i])) {
            row->pendingInvalidationReasons.fetch_or(kVariableSliceTargetRejected, std::memory_order_relaxed);
            row->appliedBlockSequence.store(blockSequence,
                                            std::memory_order_release);
            row->state.store(VariableSliceState::Invalidated,
                             std::memory_order_release);
            return false;
        }
    }
    for (std::uint32_t i = 0; i < payload->targetCount; ++i) {
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
    const auto* payload = ResolvePayload(row);
    return state == VariableSliceState::Free ||
                   state == VariableSliceState::Quarantined || payload == nullptr
               ? 0
               : payload->targetCount;
}

VariableSliceTarget* VariableSliceTable::Target(
    VariableSliceRef ref, std::uint32_t index) noexcept {
    Row* row = Resolve(ref);
    auto* payload = ResolvePayload(row);
    if (row == nullptr || payload == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            VariableSliceState::Prepared ||
        index >= payload->targetCount ||
        payload->targetSpanOffset > targetPoolCapacity_ ||
        index >= targetPoolCapacity_ - payload->targetSpanOffset) {
        return nullptr;
    }
    return &targets_[payload->targetSpanOffset + index];
}

const VariableSliceTarget* VariableSliceTable::Target(
    VariableSliceRef ref, std::uint32_t index) const noexcept {
    const Row* row = Resolve(ref);
    const auto* payload = ResolvePayload(row);
    if (row == nullptr || payload == nullptr) {
        return nullptr;
    }
    const VariableSliceState state =
        row->state.load(std::memory_order_acquire);
    if (state == VariableSliceState::Free ||
        state == VariableSliceState::Quarantined ||
        index >= payload->targetCount ||
        payload->targetSpanOffset > targetPoolCapacity_ ||
        index >= targetPoolCapacity_ - payload->targetSpanOffset) {
        return nullptr;
    }
    return &targets_[payload->targetSpanOffset + index];
}

void VariableSliceTable::ReclaimSettled(
    std::uint64_t observedBlockSequence) noexcept {
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
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
        const auto applied = row.appliedBlockSequence.load(std::memory_order_acquire);
        if (!row.logicAcknowledged || applied == 0 || observedBlockSequence <= applied) continue;
        if (ReleasePayload(row, ref) && row.sourceObservers == 0) (void)RecycleRow(row, ref);
    }
}

bool VariableSliceTable::HasUnreclaimedWorldRows(
    std::uint64_t world) const noexcept {
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        const Row& row = rows_[slot];
        const VariableSliceState state =
            row.state.load(std::memory_order_acquire);
        if (state != VariableSliceState::Free &&
            state != VariableSliceState::Quarantined && row.source.world == world) {
            return true;
        }
    }
    return false;
}

bool VariableSliceTable::HasUnreclaimedTransportRows(
    std::uint64_t transport) const noexcept {
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        const Row& row = rows_[slot];
        const VariableSliceState state =
            row.state.load(std::memory_order_acquire);
        if (state != VariableSliceState::Free &&
            state != VariableSliceState::Quarantined &&
            row.source.transport == transport) {
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
    // An explicit logic-side reclaim acknowledges that its target consumer is done.
    row->logicAcknowledged = true;
    if (!ReleasePayload(*row, ref)) return false;
    return RecycleRow(*row, ref);
}

bool VariableSliceTable::Acknowledge(VariableSliceRef ref) noexcept {
    auto* row = Resolve(ref);
    if (row == nullptr) return false;
    const auto state = row->state.load(std::memory_order_acquire);
    if (state != VariableSliceState::SliceApplied && state != VariableSliceState::Invalidated) return false;
    row->logicAcknowledged = true;
    return true;
}

bool VariableSliceTable::ReleaseSpan(std::uint32_t offset,
                                     std::uint32_t count) noexcept {
    if (count == 0) return offset <= targetPoolCapacity_;
    if (offset > targetPoolCapacity_ ||
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
    if (row.sourceObservers != 0) return false;
    if (!ReleasePayload(row, ref)) {
        row.state.store(VariableSliceState::Quarantined, std::memory_order_release);
        return false;
    }
    const std::uint64_t identity = row.identity.load(std::memory_order_relaxed);
    const std::uint32_t generation = static_cast<std::uint32_t>(identity >> 32);
    if (generation == UINT32_MAX) {
        row.state.store(VariableSliceState::Quarantined,
                         std::memory_order_release);
        return false;
    }
    row.source = {};
    row.effectiveFrame = 0;
    row.logicAcknowledged = false;
    row.appliedBlockSequence.store(0, std::memory_order_relaxed);
    row.pendingInvalidationReasons.store(0, std::memory_order_relaxed);
    row.identity.store((static_cast<std::uint64_t>(generation + 1) << 32) |
                            ref.slot,
                        std::memory_order_release);
    row.state.store(VariableSliceState::Free, std::memory_order_release);
    freeRows_.push_back(ref.slot);
    return true;
}

bool VariableSliceTable::ReleasePayload(Row& row, VariableSliceRef ref) noexcept {
    auto* payload = ResolvePayload(&row);
    if (payload == nullptr) return true;
    if (!ReleaseSpan(payload->targetSpanOffset, payload->targetSpanCapacity)) return false;
    std::uint32_t cursor = 0;
    while (const auto* source = NextSource(ref, &cursor)) (void)ReleaseSource(*source);
    const auto base = static_cast<std::size_t>(row.payload) * sourceWords_;
    std::fill_n(sourceBits_.begin() + base, sourceWords_, std::uint64_t{0});
    *payload = {};
    freePayloads_.push_back(row.payload);
    row.payload = UINT32_MAX;
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
    return static_cast<std::uint32_t>(freeRows_.size());
}

bool VariableSliceTable::SetFreeRowGenerationForTest(
    std::uint32_t slot, std::uint32_t generation) noexcept {
    if (slot >= capacity_ || generation == 0 || !PrepareRow(slot) ||
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
