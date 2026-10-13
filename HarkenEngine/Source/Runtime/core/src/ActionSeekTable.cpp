#include "harken/core/ActionSeekTable.h"

#include <atomic>
#include <new>

namespace harken {
namespace {

std::atomic<std::uint64_t> g_nextActionSeekGeneration{1};

std::uint64_t NextActionSeekGeneration() noexcept {
    std::uint64_t current =
        g_nextActionSeekGeneration.load(std::memory_order_relaxed);
    while (current <= kMaxActionSeekGeneration) {
        if (g_nextActionSeekGeneration.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

std::uint32_t Value(ActionSeekLifecycle state) noexcept {
    return static_cast<std::uint32_t>(state);
}

} // namespace

VnActionSeekTicket ActionSeekTable::MakeTicket(
    std::uint32_t slot, std::uint64_t generation) noexcept {
    return (generation << kActionSeekSlotBits) | slot;
}

std::uint32_t ActionSeekTable::SlotOf(VnActionSeekTicket ticket) noexcept {
    return static_cast<std::uint32_t>(ticket & (kMaxActionSeeksHard - 1u));
}

std::uint64_t ActionSeekTable::GenerationOf(
    VnActionSeekTicket ticket) noexcept {
    return ticket >> kActionSeekSlotBits;
}

bool ActionSeekTable::Reset(std::uint32_t capacity) {
    if (capacity == 0 || capacity > kMaxActionSeeksHard) return false;
    std::unique_ptr<Row[]> rows;
    try {
        rows = std::make_unique<Row[]>(capacity);
    } catch (const std::bad_alloc&) {
        return false;
    }
    rows_ = std::move(rows);
    capacity_ = capacity;
    return true;
}

ActionSeekTable::Row* ActionSeekTable::Resolve(
    VnActionSeekTicket ticket) noexcept {
    if (ticket == 0) return nullptr;
    const std::uint32_t slot = SlotOf(ticket);
    if (slot >= capacity_) return nullptr;
    Row& row = rows_[slot];
    return row.generation == GenerationOf(ticket) ? &row : nullptr;
}

const ActionSeekTable::Row* ActionSeekTable::Resolve(
    VnActionSeekTicket ticket) const noexcept {
    return const_cast<ActionSeekTable*>(this)->Resolve(ticket);
}

bool ActionSeekTable::Prepare(
    VnActionFiringHandle firing, std::uint32_t targetCount,
    std::uint64_t acceptedLogicSequence,
    VnActionSeekTicket* outTicket) noexcept {
    if (firing == 0 || targetCount == 0 || acceptedLogicSequence == 0 ||
        outTicket == nullptr) {
        return false;
    }
    *outTicket = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.lifecycle.load(std::memory_order_relaxed) !=
            Value(ActionSeekLifecycle::Free)) {
            continue;
        }
        if (row.generation == 0) {
            row.generation = NextActionSeekGeneration();
            if (row.generation == 0) {
                row.lifecycle.store(Value(ActionSeekLifecycle::Quarantined),
                                    std::memory_order_relaxed);
                continue;
            }
        }
        row.firing = firing;
        row.acceptedLogicSequence = acceptedLogicSequence;
        row.appliedAudioBlockSequence = 0;
        row.minimumAppliedSourceFrame = 0;
        row.maximumAppliedSourceFrame = 0;
        row.targetCount = targetCount;
        row.appliedCount = 0;
        row.cancelledCount = 0;
        row.requestedCancelResult.store(VN_OK, std::memory_order_relaxed);
        row.result = VN_OK;
        row.lifecycle.store(Value(ActionSeekLifecycle::Prepared),
                            std::memory_order_relaxed);
        *outTicket = MakeTicket(slot, row.generation);
        return true;
    }
    return false;
}

bool ActionSeekTable::Commit(VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr) return false;
    std::uint32_t expected = Value(ActionSeekLifecycle::Prepared);
    return row->lifecycle.compare_exchange_strong(
        expected, Value(ActionSeekLifecycle::Accepted),
        std::memory_order_release, std::memory_order_relaxed);
}

bool ActionSeekTable::Abandon(VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr || row->lifecycle.load(std::memory_order_relaxed) !=
                              Value(ActionSeekLifecycle::Prepared)) {
        return false;
    }
    return Recycle(SlotOf(ticket), *row);
}

bool ActionSeekTable::HasOutstanding(
    VnActionFiringHandle firing) const noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        const auto state = static_cast<ActionSeekLifecycle>(
            row.lifecycle.load(std::memory_order_acquire));
        if (state != ActionSeekLifecycle::Free &&
            state != ActionSeekLifecycle::Quarantined && row.firing == firing) {
            return true;
        }
    }
    return false;
}

bool ActionSeekTable::RequestCancel(VnActionSeekTicket ticket,
                                    VnResult result) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr) return false;
    row->requestedCancelResult.store(result, std::memory_order_relaxed);
    std::uint32_t expected = Value(ActionSeekLifecycle::Accepted);
    return row->lifecycle.compare_exchange_strong(
        expected, Value(ActionSeekLifecycle::CancelRequested),
        std::memory_order_release, std::memory_order_acquire);
}

bool ActionSeekTable::TryBeginApply(VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr) return false;
    std::uint32_t expected = Value(ActionSeekLifecycle::Accepted);
    return row->lifecycle.compare_exchange_strong(
        expected, Value(ActionSeekLifecycle::Applying),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool ActionSeekTable::FinishApplied(
    VnActionSeekTicket ticket, std::uint64_t audioBlockSequence,
    std::uint64_t minimumSourceFrame,
    std::uint64_t maximumSourceFrame) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr || row->lifecycle.load(std::memory_order_relaxed) !=
                              Value(ActionSeekLifecycle::Applying)) {
        return false;
    }
    row->result = VN_OK;
    row->appliedCount = row->targetCount;
    row->cancelledCount = 0;
    row->appliedAudioBlockSequence = audioBlockSequence;
    row->minimumAppliedSourceFrame = minimumSourceFrame;
    row->maximumAppliedSourceFrame = maximumSourceFrame;
    row->lifecycle.store(Value(ActionSeekLifecycle::Applied),
                         std::memory_order_release);
    return true;
}

bool ActionSeekTable::FinishCancelled(VnActionSeekTicket ticket,
                                      VnResult result) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr || row->lifecycle.load(std::memory_order_acquire) !=
                              Value(ActionSeekLifecycle::CancelRequested)) {
        return false;
    }
    row->result = result;
    row->appliedCount = 0;
    row->cancelledCount = row->targetCount;
    row->lifecycle.store(Value(ActionSeekLifecycle::Cancelled),
                         std::memory_order_release);
    return true;
}

bool ActionSeekTable::FinishRequestedCancellation(
    VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr || row->lifecycle.load(std::memory_order_acquire) !=
                              Value(ActionSeekLifecycle::CancelRequested)) {
        return false;
    }
    row->result = static_cast<VnResult>(
        row->requestedCancelResult.load(std::memory_order_relaxed));
    row->appliedCount = 0;
    row->cancelledCount = row->targetCount;
    row->lifecycle.store(Value(ActionSeekLifecycle::Cancelled),
                         std::memory_order_release);
    return true;
}

bool ActionSeekTable::CancelAcceptedFromAudio(
    VnActionSeekTicket ticket, VnResult result) noexcept {
    if (!RequestCancel(ticket, result)) return false;
    return FinishCancelled(ticket, result);
}

VnResult ActionSeekTable::Query(
    VnActionSeekTicket ticket, VnActionSeekInfoV1* outInfo) const noexcept {
    const Row* row = Resolve(ticket);
    if (row == nullptr || outInfo == nullptr) return VN_STALE_HANDLE;
    const auto state = static_cast<ActionSeekLifecycle>(
        row->lifecycle.load(std::memory_order_acquire));
    if (state == ActionSeekLifecycle::Free ||
        state == ActionSeekLifecycle::Prepared ||
        state == ActionSeekLifecycle::Quarantined) {
        return VN_STALE_HANDLE;
    }
    VnActionSeekInfoV1 info{};
    info.structSize = sizeof(info);
    info.state = state == ActionSeekLifecycle::Applied
                     ? VN_ACTION_SEEK_APPLIED
                     : state == ActionSeekLifecycle::Cancelled
                           ? VN_ACTION_SEEK_CANCELLED
                           : VN_ACTION_SEEK_ACCEPTED;
    info.result = (state == ActionSeekLifecycle::Applied ||
                   state == ActionSeekLifecycle::Cancelled)
                      ? row->result
                      : VN_OK;
    info.firing = row->firing;
    info.targetCount = row->targetCount;
    info.appliedCount = row->appliedCount;
    info.cancelledCount = row->cancelledCount;
    info.acceptedLogicSequence = row->acceptedLogicSequence;
    info.appliedAudioBlockSequence = row->appliedAudioBlockSequence;
    info.minimumAppliedSourceFrame = row->minimumAppliedSourceFrame;
    info.maximumAppliedSourceFrame = row->maximumAppliedSourceFrame;
    *outInfo = info;
    return VN_OK;
}

VnResult ActionSeekTable::Release(VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(ticket);
    if (row == nullptr) return VN_STALE_HANDLE;
    const auto state = static_cast<ActionSeekLifecycle>(
        row->lifecycle.load(std::memory_order_acquire));
    if (state != ActionSeekLifecycle::Applied &&
        state != ActionSeekLifecycle::Cancelled) {
        return VN_IN_USE;
    }
    Recycle(SlotOf(ticket), *row);
    return VN_OK;
}

void ActionSeekTable::CancelAllAfterAudioJoin(VnResult result) noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        const auto state = static_cast<ActionSeekLifecycle>(
            row.lifecycle.load(std::memory_order_relaxed));
        if (state == ActionSeekLifecycle::Accepted ||
            state == ActionSeekLifecycle::CancelRequested ||
            state == ActionSeekLifecycle::Applying) {
            row.result = result;
            row.appliedCount = 0;
            row.cancelledCount = row.targetCount;
            row.lifecycle.store(Value(ActionSeekLifecycle::Cancelled),
                                std::memory_order_release);
        }
    }
}

bool ActionSeekTable::Recycle(std::uint32_t, Row& row) noexcept {
    row.firing = 0;
    row.acceptedLogicSequence = 0;
    row.appliedAudioBlockSequence = 0;
    row.minimumAppliedSourceFrame = 0;
    row.maximumAppliedSourceFrame = 0;
    row.targetCount = 0;
    row.appliedCount = 0;
    row.cancelledCount = 0;
    row.requestedCancelResult.store(VN_OK, std::memory_order_relaxed);
    row.result = VN_OK;
    row.generation = NextActionSeekGeneration();
    if (row.generation == 0) {
        row.lifecycle.store(Value(ActionSeekLifecycle::Quarantined),
                            std::memory_order_release);
        return false;
    }
    row.lifecycle.store(Value(ActionSeekLifecycle::Free),
                        std::memory_order_release);
    return true;
}

} // namespace harken
