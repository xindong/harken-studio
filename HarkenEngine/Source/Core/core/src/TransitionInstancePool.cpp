#include "harken/core/TransitionInstancePool.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/ScheduledTransactionPool.h"

#include <cassert>
#include <utility>

namespace harken {
namespace {

struct Reservation {
    explicit Reservation(std::atomic_flag& value) noexcept
        : flag(value.test_and_set(std::memory_order_acquire) ? nullptr : &value) {}
    ~Reservation() { if (flag != nullptr) flag->clear(std::memory_order_release); }
    std::atomic_flag* flag;
};

bool ValidCapacity(std::uint32_t capacity) noexcept {
    return capacity != 0 && capacity <= kMaxTransitionInstancesHard;
}

std::uint64_t PackIdentity(std::uint32_t slot,
                           std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

} // namespace

TransitionInstancePool::TransitionInstancePool(std::uint32_t capacity)
    : rows_(ValidCapacity(capacity) ? std::make_unique<Row[]>(capacity)
                                    : nullptr),
      capacity_(ValidCapacity(capacity) ? capacity : 0) {}

void TransitionInstancePool::FrontierProof::Release() noexcept {
    if (row_ != nullptr) row_->reservation.clear(std::memory_order_release);
    row_ = nullptr;
    lease_ = nullptr;
}

TransitionInstancePool::PrivateTimingProof::PrivateTimingProof(
    PrivateTimingProof&& other) noexcept {
    *this = std::move(other);
}

TransitionInstancePool::PrivateTimingProof&
TransitionInstancePool::PrivateTimingProof::operator=(PrivateTimingProof&& other) noexcept {
    if (this != &other) {
        Release();
        row_ = other.row_;
        audibleFrame_ = other.audibleFrame_;
        lockBlock_ = other.lockBlock_;
        other.row_ = nullptr;
    }
    return *this;
}

void TransitionInstancePool::PrivateTimingProof::Release() noexcept {
    if (row_ != nullptr) row_->reservation.clear(std::memory_order_release);
    row_ = nullptr;
}

bool TransitionInstancePool::ProvePrivateTimingBeforePublication(
    TransitionInstanceToken token, const ImmediateActionLane& lane,
    ImmediateLaneHandle handle, std::uint64_t root,
    std::uint64_t oldAudibleFrame, std::uint64_t oldLockBlock,
    std::uint64_t newAudibleFrame, std::uint64_t newLockBlock,
    PrivateTimingProof* out) noexcept {
    if (out == nullptr || out->Valid() || !lane.OwnsPrivate(handle, root) ||
        !lane.OwnsNeverCommitted(handle, root)) return false;
    auto* row = Resolve(token);
    if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire))
        return false;
    if (row->audibleFrame != oldAudibleFrame || row->lockBlock != oldLockBlock ||
        row->frontierPool != nullptr ||
        row->gateState.load(std::memory_order_acquire) != TransitionAudibleGateState::Replaceable ||
        row->retiredAtAudioSequence.load(std::memory_order_acquire) != 0) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    out->row_ = row;
    out->audibleFrame_ = newAudibleFrame;
    out->lockBlock_ = newLockBlock;
    return true;
}

void TransitionInstancePool::ApplyPrivateTimingBeforePublication(
    const PrivateTimingProof& proof) noexcept {
    assert(proof.Valid());
    proof.row_->audibleFrame = proof.audibleFrame_;
    proof.row_->lockBlock = proof.lockBlock_;
}

bool TransitionInstancePool::MayMutateFrontier(Row& row) const noexcept {
    return row.frontierPool == nullptr || row.frontierPool->InvalidateFrontierOwnerOperations(
        row.frontierOwner, row.frontierDecision);
}

bool TransitionInstancePool::BindFrontier(TransitionInstanceToken token,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept {
    auto* row = Resolve(token);
    if (row == nullptr) row = ResolvePrepared(token);
    Plan03PageGroupView group{};
    const auto& identity = binding.transition;
    if (row == nullptr || !binding.Valid() || !identity.Valid() ||
        token.slot != identity.slot || token.generation != identity.generation ||
        identity.audibleFrame != row->audibleFrame ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) || group.frontier != &binding ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready) return false;
    Reservation guard(row->reservation);
    if (guard.flag == nullptr || row->gateState.load(std::memory_order_acquire) !=
            TransitionAudibleGateState::Replaceable) return false;
    if (row->frontierPool != nullptr)
        return row->frontierPool == &pool && row->frontierOwner == binding.operation.owner &&
            row->frontierDecision.slot == binding.operation.group.decision.slot &&
            row->frontierDecision.generation == binding.operation.group.decision.generation;
    row->frontierPool = &pool;
    row->frontierOwner = binding.operation.owner;
    row->frontierDecision = binding.operation.group.decision;
    return true;
}

bool TransitionInstancePool::ProveFrontier(const FrontierGroupBinding& binding, AudioSliceLease& lease,
    TransitionAudibleGateState expected, bool locks, FrontierProof* out) noexcept {
    auto* row = Resolve({binding.transition.slot, binding.transition.generation});
    if (row == nullptr || out == nullptr || out->Valid() || !binding.Valid() ||
        (expected != TransitionAudibleGateState::Replaceable && expected != TransitionAudibleGateState::Locked) ||
        (locks && expected != TransitionAudibleGateState::Replaceable) ||
        row->reservation.test_and_set(std::memory_order_acquire)) return false;
    Plan03PageGroupView group{};
    if (row->frontierPool == nullptr || !(row->frontierOwner == binding.operation.owner) ||
        row->frontierDecision.slot != binding.operation.group.decision.slot ||
        row->frontierDecision.generation != binding.operation.group.decision.generation ||
        row->audibleFrame != binding.transition.audibleFrame ||
        row->gateState.load(std::memory_order_acquire) != expected ||
        (locks && binding.operation.order.executeFrame != row->audibleFrame) ||
        !row->frontierPool->ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease || lease.Capture().state != AudioSliceState::Ready) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    out->row_ = row;
    out->lease_ = &lease;
    out->locks_ = locks;
    return true;
}

void TransitionInstancePool::ApplyFrontier(const FrontierProof& proof) noexcept {
    assert(proof.Valid() && proof.lease_ != nullptr);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    if (proof.locks_) proof.row_->gateState.store(TransitionAudibleGateState::Locked, std::memory_order_release);
}

bool TransitionInstancePool::ReleaseFrontierBinding(TransitionInstanceToken token) noexcept {
    auto* row = Resolve(token);
    if (row == nullptr) row = ResolvePrepared(token);
    if (row == nullptr) return false;
    Reservation guard(row->reservation);
    if (guard.flag == nullptr || row->frontierPool == nullptr ||
        row->frontierPool->HasFrontierOwnerOperations(row->frontierOwner, row->frontierDecision)) return false;
    row->frontierPool = nullptr;
    row->frontierOwner = {};
    row->frontierDecision = {};
    return true;
}

std::uint32_t TransitionInstancePool::LiveCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Live) {
            ++count;
        }
    }
    return count;
}

std::uint32_t TransitionInstancePool::FreeCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Free) {
            ++count;
        }
    }
    return count;
}

std::uint32_t TransitionInstancePool::QuarantinedCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Quarantined) {
            ++count;
        }
    }
    return count;
}

bool TransitionInstancePool::Acquire(
    const TransitionInstanceCreateRequest& request,
    TransitionInstanceToken* outToken) noexcept {
    return AcquireInternal(request, outToken, false);
}

bool TransitionInstancePool::PrepareAcquire(
    const TransitionInstanceCreateRequest& request,
    TransitionInstanceToken* outToken) noexcept {
    return AcquireInternal(request, outToken, true);
}

bool TransitionInstancePool::AcquireInternal(
    const TransitionInstanceCreateRequest& request,
    TransitionInstanceToken* outToken, bool prepared) noexcept {
    if (outToken == nullptr) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Free ||
            row.reservation.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        SlotState expected = SlotState::Free;
        row.audibleFrame = request.audibleFrame;
        row.lockBlock = request.lockBlock;
        row.monitorIdentity = request.monitorIdentity;
        row.gateState.store(TransitionAudibleGateState::Replaceable,
                            std::memory_order_relaxed);
        row.retiredAtAudioSequence.store(0, std::memory_order_relaxed);
        row.identity.store(PackIdentity(slot, row.generation),
                           std::memory_order_release);
        if (row.slotState.compare_exchange_strong(
                expected,
                prepared ? SlotState::Prepared : SlotState::Live,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            row.reservation.clear(std::memory_order_release);
            *outToken = {slot, row.generation};
            if (!prepared) {
                const std::uint64_t handle =
                    (static_cast<std::uint64_t>(outToken->generation) << 32) |
                    outToken->slot;
                ObserveMonitorLifecycle(MonitorLifecyclePhase::TransitionStart,
                                        handle, outToken->generation, 0, {}, 0,
                                        0, 0, 0, 0, row.monitorIdentity);
            }
            return true;
        }
        row.identity.store(0, std::memory_order_release);
        row.reservation.clear(std::memory_order_release);
    }
    return false;
}

TransitionInstancePool::Row* TransitionInstancePool::ResolvePrepared(
    TransitionInstanceToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Prepared ||
        row.identity.load(std::memory_order_acquire) !=
            PackIdentity(token.slot, token.generation)) {
        return nullptr;
    }
    return &row;
}

bool TransitionInstancePool::CommitPreparedAcquire(
    TransitionInstanceToken token) noexcept {
    Row* row = ResolvePrepared(token);
    if (row == nullptr) {
        return false;
    }
    SlotState expected = SlotState::Prepared;
    const bool ok = row->slotState.compare_exchange_strong(
        expected, SlotState::Live, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (ok) {
        const std::uint64_t handle =
            (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
        ObserveMonitorLifecycle(MonitorLifecyclePhase::TransitionStart, handle,
                                token.generation, 0, {}, 0, 0, 0, 0, 0, row->monitorIdentity);
    }
    return ok;
}

bool TransitionInstancePool::RecyclePrepared(Row& row) noexcept {
    row.identity.store(0, std::memory_order_release);
    if (row.generation == kMaxTransitionInstanceGeneration) {
        row.slotState.store(SlotState::Quarantined,
                            std::memory_order_release);
        return false;
    }
    ++row.generation;
    row.audibleFrame = 0;
    row.lockBlock = 0;
    row.monitorIdentity = {};
    row.retiredAtAudioSequence.store(0, std::memory_order_relaxed);
    row.gateState.store(TransitionAudibleGateState::Replaceable,
                        std::memory_order_relaxed);
    row.slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool TransitionInstancePool::CancelPreparedAcquire(
    TransitionInstanceToken token) noexcept {
    Row* row = ResolvePrepared(token);
    if (row == nullptr) {
        return false;
    }
    Reservation guard(row->reservation);
    if (guard.flag == nullptr || row->frontierPool != nullptr) return false;
    SlotState expected = SlotState::Prepared;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    return RecyclePrepared(*row);
}

bool TransitionInstancePool::CancelBeforePublication(
    TransitionInstanceToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr) {
        return false;
    }
    Reservation guard(row->reservation);
    if (guard.flag == nullptr || row->frontierPool != nullptr) return false;
    TransitionAudibleGateState expected =
        TransitionAudibleGateState::Replaceable;
    if (!row->gateState.compare_exchange_strong(
            expected, TransitionAudibleGateState::Retired,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    SlotState live = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            live, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        row->gateState.store(TransitionAudibleGateState::Replaceable,
                             std::memory_order_release);
        return false;
    }
    const auto monitorIdentity = row->monitorIdentity;
    row->identity.store(0, std::memory_order_release);
    if (row->generation == kMaxTransitionInstanceGeneration) {
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        return false;
    }
    ++row->generation;
    row->audibleFrame = 0;
    row->lockBlock = 0;
    row->monitorIdentity = {};
    row->retiredAtAudioSequence.store(0, std::memory_order_relaxed);
    row->gateState.store(TransitionAudibleGateState::Replaceable,
                         std::memory_order_relaxed);
    row->slotState.store(SlotState::Free, std::memory_order_release);
    const std::uint64_t handle =
        (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
    ObserveMonitorLifecycle(MonitorLifecyclePhase::TransitionReject, handle,
                            token.generation, 0, {}, 0, 0, 0, 0, 0, monitorIdentity);
    return true;
}

TransitionInstancePool::Row* TransitionInstancePool::Resolve(
    TransitionInstanceToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.identity.load(std::memory_order_acquire) !=
            PackIdentity(token.slot, token.generation)) {
        return nullptr;
    }
    return &row;
}

const TransitionInstancePool::Row* TransitionInstancePool::Resolve(
    TransitionInstanceToken token) const noexcept {
    return const_cast<TransitionInstancePool*>(this)->Resolve(token);
}

bool TransitionInstancePool::InvalidateReady(
    TransitionInstanceToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->gateState.load(std::memory_order_acquire) !=
            TransitionAudibleGateState::Replaceable) {
        return false;
    }
    if (!MayMutateFrontier(*row)) return false;
    Reservation guard(row->reservation);
    if (guard.flag == nullptr) return false;
    TransitionAudibleGateState expected =
        TransitionAudibleGateState::Replaceable;
    return row->gateState.compare_exchange_strong(
        expected, TransitionAudibleGateState::Invalidated,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool TransitionInstancePool::CanLockFromAudioClaim(
    TransitionInstanceToken token) const noexcept {
    const Row* row = Resolve(token);
    return row != nullptr && row->frontierPool == nullptr &&
           row->gateState.load(std::memory_order_acquire) ==
               TransitionAudibleGateState::Replaceable;
}

void TransitionInstancePool::LockFromClaimedAudioSlice(
    TransitionInstanceToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->frontierPool != nullptr) {
        return;
    }
    Reservation guard(row->reservation);
    if (guard.flag == nullptr) return;
    TransitionAudibleGateState state =
        row->gateState.load(std::memory_order_acquire);
    while (state == TransitionAudibleGateState::Replaceable ||
           state == TransitionAudibleGateState::Invalidated) {
        if (row->gateState.compare_exchange_weak(
                state, TransitionAudibleGateState::Locked,
                std::memory_order_release, std::memory_order_acquire)) {
            return;
        }
    }
}

bool TransitionInstancePool::BeginRetire(
    TransitionInstanceToken token,
    std::uint64_t audioSequence) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || audioSequence == 0) {
        return false;
    }
    if (!MayMutateFrontier(*row)) return false;
    Reservation guard(row->reservation);
    if (guard.flag == nullptr) return false;
    TransitionAudibleGateState state =
        row->gateState.load(std::memory_order_acquire);
    while (state == TransitionAudibleGateState::Invalidated ||
           state == TransitionAudibleGateState::Locked) {
        row->retiredAtAudioSequence.store(audioSequence,
                                          std::memory_order_relaxed);
        if (row->gateState.compare_exchange_weak(
                state, TransitionAudibleGateState::Retired,
                std::memory_order_release, std::memory_order_acquire)) {
            const std::uint64_t handle =
                (static_cast<std::uint64_t>(token.generation) << 32) |
                token.slot;
            ObserveMonitorLifecycle(MonitorLifecyclePhase::TransitionStop,
                                    handle, token.generation, 0, {}, 0, 0, 0, 0, 0, row->monitorIdentity);
            return true;
        }
    }
    return state == TransitionAudibleGateState::Retired &&
           row->retiredAtAudioSequence.load(std::memory_order_acquire) ==
               audioSequence;
}

bool TransitionInstancePool::Reclaim(
    TransitionInstanceToken token,
    std::uint64_t observedAudioSequence) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->frontierPool != nullptr ||
        row->gateState.load(std::memory_order_acquire) !=
            TransitionAudibleGateState::Retired ||
        observedAudioSequence <=
            row->retiredAtAudioSequence.load(std::memory_order_acquire)) {
        return false;
    }
    Reservation guard(row->reservation);
    if (guard.flag == nullptr) return false;
    SlotState expected = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    row->identity.store(0, std::memory_order_release);
    if (row->generation == kMaxTransitionInstanceGeneration) {
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        return false;
    }
    ++row->generation;
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool TransitionInstancePool::Capture(
    TransitionInstanceToken token,
    TransitionInstanceSnapshot* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Row* row = Resolve(token);
    if (row == nullptr) {
        return false;
    }
    TransitionInstanceSnapshot snapshot{};
    snapshot.state = row->gateState.load(std::memory_order_acquire);
    snapshot.audibleFrame = row->audibleFrame;
    snapshot.lockBlock = row->lockBlock;
    snapshot.retiredAtAudioSequence =
        row->retiredAtAudioSequence.load(std::memory_order_acquire);
    *out = snapshot;
    return true;
}

bool TransitionInstancePool::SetGenerationForTests(
    std::uint32_t slot, std::uint32_t generation) noexcept {
    if (slot >= capacity_ || generation == 0 ||
        generation > kMaxTransitionInstanceGeneration ||
        rows_[slot].slotState.load(std::memory_order_acquire) !=
            SlotState::Free) {
        return false;
    }
    rows_[slot].generation = generation;
    return true;
}

} // namespace harken
