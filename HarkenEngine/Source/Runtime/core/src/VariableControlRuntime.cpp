#include "harken/core/VariableControlRuntime.h"
#include "harken/core/ScheduledTransactionPool.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

namespace harken {
namespace {

std::uint64_t Pack(std::uint32_t slot,
                   std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

DecisionToken UnpackDecision(std::uint64_t packed) noexcept {
    return {static_cast<std::uint32_t>(packed),
            static_cast<std::uint32_t>(packed >> 32)};
}

BankVersionToken UnpackBank(std::uint64_t packed) noexcept {
    return {static_cast<std::uint32_t>(packed),
            static_cast<std::uint32_t>(packed >> 32)};
}

} // namespace

PreparedVariableControlInstance::PreparedVariableControlInstance(
    PreparedVariableControlInstance&& other) noexcept {
    *this = std::move(other);
}

PreparedVariableControlInstance& PreparedVariableControlInstance::operator=(
    PreparedVariableControlInstance&& other) noexcept {
    if (this == &other ||
        (owner_ != nullptr && owner_->PreparedCommitHeld(token_)) ||
        (other.owner_ != nullptr && other.owner_->PreparedCommitHeld(other.token_))) return *this;
    owner_ = other.owner_;
    token_ = other.token_;
    prepared_ = other.prepared_;
    originalRequest_ = other.originalRequest_;
    originalSides_ = other.originalSides_;
    other.owner_ = nullptr;
    other.token_ = {};
    other.prepared_ = false;
    other.originalRequest_ = {};
    other.originalSides_ = {};
    return *this;
}

VariableControlInstanceTable::PreparedCommitProof::PreparedCommitProof(
    PreparedCommitProof&& other) noexcept {
    *this = std::move(other);
}

VariableControlInstanceTable::PreparedCommitProof&
VariableControlInstanceTable::PreparedCommitProof::operator=(PreparedCommitProof&& other) noexcept {
    if (this == &other) return *this;
    Release();
    owner_ = other.owner_;
    entry_ = other.entry_;
    original_ = other.original_;
    token_ = other.token_;
    other.owner_ = nullptr;
    other.entry_ = nullptr;
    other.original_ = nullptr;
    other.token_ = {};
    return *this;
}

void VariableControlInstanceTable::PreparedCommitProof::Release() noexcept {
    if (entry_ != nullptr) {
        assert(entry_->state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld);
        entry_->state.store(SlotState::Initializing, std::memory_order_release);
    }
    owner_ = nullptr;
    entry_ = nullptr;
    original_ = nullptr;
    token_ = {};
}

bool VariableControlInstanceTable::PreparedCommitHeld(VariableControlInstanceToken token) const noexcept {
    return token.Valid() && token.slot < capacity_ &&
        entries_[token.slot].generation.load(std::memory_order_acquire) == token.generation &&
        entries_[token.slot].state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld;
}

bool VariableControlInstanceTable::ProvePreparedCommit(
    PreparedVariableControlInstance& original, PreparedCommitProof* out) noexcept {
    if (out == nullptr || out->Valid() || original.owner_ != this || !original.prepared_ ||
        !original.token_.Valid() || original.token_.slot >= capacity_) return false;
    auto& entry = entries_[original.token_.slot];
    if (entry.generation.load(std::memory_order_acquire) != original.token_.generation) return false;
    SlotState expected = SlotState::Initializing;
    if (!entry.state.compare_exchange_strong(expected, SlotState::PreparedCommitHeld,
        std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    const auto& request = original.originalRequest_;
    const auto sideMatches = [](const SideRow& side,
        const PreparedVariableControlInstance::SideReceipt& receipt) noexcept {
        const auto bank = Pack(receipt.bank.slot, receipt.bank.generation);
        return receipt.issued && receipt.decision.Valid() && receipt.bank.Valid() && receipt.generation != 0 &&
            side.state.load(std::memory_order_acquire) == VariableControlSideState::Pending &&
            side.generation.load(std::memory_order_acquire) == receipt.generation &&
            side.decision.load(std::memory_order_acquire) == Pack(receipt.decision.slot, receipt.decision.generation) &&
            side.newestObserved.load(std::memory_order_acquire) == bank &&
            side.newestPending.load(std::memory_order_acquire) == bank &&
            side.lastApplied.load(std::memory_order_acquire) == 0 && side.frontierPool == nullptr &&
            side.frontierOwner == FrontierOwnerIdentity{};
    };
    if (entry.generation.load(std::memory_order_acquire) != original.token_.generation ||
        entry.world.load(std::memory_order_acquire) != request.world ||
        entry.rootInstance.load(std::memory_order_acquire) != request.rootInstance ||
        entry.containerIdHigh.load(std::memory_order_acquire) != request.containerStableId.high ||
        entry.containerIdLow.load(std::memory_order_acquire) != request.containerStableId.low ||
        entry.programSemanticFingerprint.load(std::memory_order_acquire) != request.programSemanticFingerprint ||
        !sideMatches(entry.discrete, original.originalSides_[0]) ||
        !sideMatches(entry.continuous, original.originalSides_[1])) {
        entry.state.store(SlotState::Initializing, std::memory_order_release);
        return false;
    }
    out->owner_ = this;
    out->entry_ = &entry;
    out->original_ = &original;
    out->token_ = original.token_;
    return true;
}

void VariableControlInstanceTable::ConsumePreparedAcquire(PreparedCommitProof&& proof) noexcept {
    assert(proof.Valid() && proof.owner_ != nullptr && proof.original_ != nullptr);
    if (!proof.Valid()) return;
    assert(proof.entry_->state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld);
    proof.original_->owner_ = nullptr;
    proof.original_->token_ = {};
    proof.original_->prepared_ = false;
    proof.original_->originalRequest_ = {};
    proof.original_->originalSides_ = {};
    proof.entry_->state.store(SlotState::Live, std::memory_order_release);
    proof.owner_->NoteLiveAcquire();
    proof.owner_ = nullptr;
    proof.entry_ = nullptr;
    proof.original_ = nullptr;
    proof.token_ = {};
}

bool ResolveWeightSegment(const WeightSegmentV1& segment,
                          std::uint64_t frame,
                          float* outWeight) noexcept {
    if (outWeight == nullptr || segment.segmentGeneration == 0 ||
        !segment.decision.Valid() || !segment.bank.Valid() ||
        !std::isfinite(segment.startWeight) ||
        !std::isfinite(segment.endWeight) || segment.startWeight < 0.0f ||
        segment.endWeight < 0.0f ||
        segment.endFrame <= segment.startFrame ||
        segment.endFrame - segment.startFrame < 2u) {
        return false;
    }
    float resolved = segment.startWeight;
    if (frame >= segment.endFrame - 1u) {
        resolved = segment.endWeight;
    } else if (frame > segment.startFrame) {
        const double numerator = static_cast<double>(
            frame - segment.startFrame);
        const double denominator = static_cast<double>(
            segment.endFrame - segment.startFrame - 1u);
        resolved = static_cast<float>(
            static_cast<double>(segment.startWeight) +
            (static_cast<double>(segment.endWeight) -
             static_cast<double>(segment.startWeight)) *
                (numerator / denominator));
        if (!std::isfinite(resolved) || resolved < 0.0f) return false;
    }
    *outWeight = resolved;
    return true;
}

WeightSegmentStatus RetargetWeightSegment(
    const WeightSegmentV1* current,
    std::uint64_t decisionFrame,
    std::uint32_t envelopeFrames,
    float targetWeight,
    DecisionToken decision,
    BankVersionToken bank,
    WeightSegmentV1* outSegment) noexcept {
    if (current == nullptr || outSegment == nullptr ||
        envelopeFrames < 2u || !std::isfinite(targetWeight) ||
        targetWeight < 0.0f || !decision.Valid() || !bank.Valid() ||
        decisionFrame > UINT64_MAX - envelopeFrames) {
        return WeightSegmentStatus::Invalid;
    }
    float startWeight = 0.0f;
    if (!ResolveWeightSegment(*current, decisionFrame, &startWeight)) {
        return WeightSegmentStatus::Invalid;
    }
    if (current->segmentGeneration == UINT32_MAX) {
        return WeightSegmentStatus::IdentityExhausted;
    }
    WeightSegmentV1 next{};
    next.startFrame = decisionFrame;
    next.endFrame = decisionFrame + envelopeFrames;
    next.startWeight = startWeight;
    next.endWeight = targetWeight;
    next.segmentGeneration = current->segmentGeneration + 1u;
    next.decision = decision;
    next.bank = bank;
    *outSegment = next;
    return WeightSegmentStatus::Ok;
}

bool ResolveVariableControlCadenceFrame(
    std::uint64_t anchorFrame,
    std::uint32_t evaluationPeriodFrames,
    std::uint64_t observedFrame,
    std::uint64_t* outFrame) noexcept {
    if (evaluationPeriodFrames == 0 || outFrame == nullptr) return false;
    if (observedFrame <= anchorFrame) {
        *outFrame = anchorFrame;
        return true;
    }
    const std::uint64_t delta = observedFrame - anchorFrame;
    const std::uint64_t period = evaluationPeriodFrames;
    const std::uint64_t steps =
        delta / period + static_cast<std::uint64_t>(delta % period != 0);
    if (steps > (UINT64_MAX - anchorFrame) / period) return false;
    *outFrame = anchorFrame + steps * period;
    return true;
}

VariableControlInstanceTable::VariableControlInstanceTable(
    std::uint32_t capacity)
    : entries_(capacity == 0 ? nullptr : std::make_unique<Entry[]>(capacity)),
      capacity_(capacity) {}

VariableControlInstanceTable::Entry*
VariableControlInstanceTable::Resolve(
    VariableControlInstanceToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    Entry& entry = entries_[token.slot];
    return entry.state.load(std::memory_order_acquire) == SlotState::Live &&
                   entry.generation.load(std::memory_order_acquire) ==
                       token.generation
               ? &entry
               : nullptr;
}

const VariableControlInstanceTable::Entry*
VariableControlInstanceTable::Resolve(
    VariableControlInstanceToken token) const noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    const Entry& entry = entries_[token.slot];
    return entry.state.load(std::memory_order_acquire) == SlotState::Live &&
                   entry.generation.load(std::memory_order_acquire) ==
                       token.generation
               ? &entry
               : nullptr;
}

VariableControlInstanceTable::Entry*
VariableControlInstanceTable::ResolvePreparing(
    VariableControlInstanceToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    Entry& entry = entries_[token.slot];
    const SlotState state = entry.state.load(std::memory_order_acquire);
    return (state == SlotState::Initializing || state == SlotState::Live) &&
                   entry.generation.load(std::memory_order_acquire) ==
                       token.generation
               ? &entry
               : nullptr;
}

void VariableControlInstanceTable::ResetEntry(Entry& entry) noexcept {
    entry.world.store(0, std::memory_order_relaxed);
    entry.rootInstance.store(0, std::memory_order_relaxed);
    entry.containerIdHigh.store(0, std::memory_order_relaxed);
    entry.containerIdLow.store(0, std::memory_order_relaxed);
    entry.programSemanticFingerprint.store(0, std::memory_order_relaxed);
    for (SideRow* side : {&entry.discrete, &entry.continuous}) {
        side->frontierPool = nullptr;
        side->frontierOwner = {};
        side->decision.store(0, std::memory_order_relaxed);
        side->newestObserved.store(0, std::memory_order_relaxed);
        side->newestPending.store(0, std::memory_order_relaxed);
        side->lastApplied.store(0, std::memory_order_relaxed);
        side->generation.store(1, std::memory_order_relaxed);
        side->state.store(VariableControlSideState::Empty,
                          std::memory_order_relaxed);
    }
}

bool VariableControlInstanceTable::MayMutateSide(SideRow& side) noexcept {
    return side.frontierPool == nullptr ||
        side.frontierPool->InvalidateFrontierOwnerOperations(side.frontierOwner,
            UnpackDecision(side.decision.load(std::memory_order_acquire)));
}

VariableControlInstanceTable::FrontierSideProof::~FrontierSideProof() { Release(); }

void VariableControlInstanceTable::FrontierSideProof::Release() noexcept {
    if (side_ != nullptr && !applied_)
        side_->state.store(priorState_, std::memory_order_release);
    side_ = nullptr;
    lease_ = nullptr;
}

bool VariableControlInstanceTable::BindFrontierSide(VariableControlInstanceToken instance,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept {
    auto* entry = ResolvePreparing(instance);
    const auto& identity = binding.variableSide;
    Plan03PageGroupView group{};
    if (entry == nullptr || !binding.Valid() || !identity.Valid() ||
        instance.slot != identity.slot || instance.generation != identity.generation ||
        entry->world.load(std::memory_order_acquire) != binding.operation.world ||
        entry->rootInstance.load(std::memory_order_acquire) != binding.operation.owner.child ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) ||
        group.frontier == nullptr || !(group.frontier->variableSide == identity) ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready) return false;
    auto& side = identity.side == 0 ? entry->discrete : entry->continuous;
    const auto sideState = side.state.load(std::memory_order_acquire);
    if ((sideState != VariableControlSideState::Pending &&
            (binding.role != FrontierGroupRole::Future || sideState != VariableControlSideState::Committed)) ||
        side.generation.load(std::memory_order_acquire) != identity.sideGeneration ||
        side.decision.load(std::memory_order_acquire) != Pack(binding.operation.group.decision.slot,
            binding.operation.group.decision.generation) ||
        side.newestPending.load(std::memory_order_acquire) != Pack(identity.bankSlot, identity.bankGeneration) ||
        (side.frontierPool != nullptr && (side.frontierPool != &pool ||
            !(side.frontierOwner == binding.operation.owner)))) return false;
    if (side.frontierPool == nullptr) {
        side.frontierOwner = binding.operation.owner;
        side.frontierPool = &pool;
    }
    return true;
}

bool VariableControlInstanceTable::ProveFrontierSide(const FrontierGroupBinding& binding,
    AudioSliceLease& lease, FrontierSideProof* out) noexcept {
    const auto& identity = binding.variableSide;
    if (out == nullptr || out->Valid() || !binding.Valid() || !identity.Valid() ||
        lease.Capture().state != AudioSliceState::Ready) return false;
    auto* entry = Resolve({identity.slot, identity.generation});
    if (entry == nullptr || entry->world.load(std::memory_order_acquire) != binding.operation.world ||
        entry->rootInstance.load(std::memory_order_acquire) != binding.operation.owner.child) return false;
    auto& side = identity.side == 0 ? entry->discrete : entry->continuous;
    Plan03PageGroupView group{};
    if (side.frontierPool == nullptr || !(side.frontierOwner == binding.operation.owner) ||
        !side.frontierPool->ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease) return false;
    auto expected = side.state.load(std::memory_order_acquire);
    const auto priorState = expected;
    if (expected != VariableControlSideState::Pending &&
        (binding.role != FrontierGroupRole::Future || expected != VariableControlSideState::Committed)) return false;
    if (!side.state.compare_exchange_strong(expected, VariableControlSideState::Transitioning,
        std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    if (side.generation.load(std::memory_order_acquire) != identity.sideGeneration ||
        side.decision.load(std::memory_order_acquire) != Pack(binding.operation.group.decision.slot,
            binding.operation.group.decision.generation) ||
        side.newestPending.load(std::memory_order_acquire) != Pack(identity.bankSlot, identity.bankGeneration) ||
        (priorState == VariableControlSideState::Committed &&
            side.lastApplied.load(std::memory_order_acquire) != Pack(identity.bankSlot, identity.bankGeneration))) {
        side.state.store(priorState, std::memory_order_release);
        return false;
    }
    out->side_ = &side;
    out->lease_ = &lease;
    out->bank_ = Pack(identity.bankSlot, identity.bankGeneration);
    out->priorState_ = priorState;
    out->applied_ = false;
    return true;
}

void VariableControlInstanceTable::ApplyFrontierSide(FrontierSideProof& proof) noexcept {
    assert(proof.side_ != nullptr && proof.lease_ != nullptr && !proof.applied_);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    proof.side_->lastApplied.store(proof.bank_, std::memory_order_relaxed);
    proof.side_->state.store(VariableControlSideState::Committed, std::memory_order_release);
    proof.applied_ = true;
}

void VariableControlInstanceTable::NoteLiveAcquire() noexcept {
    const std::uint32_t live =
        liveCount_.fetch_add(1, std::memory_order_acq_rel) + 1u;
    std::uint32_t high = highWater_.load(std::memory_order_acquire);
    while (high < live &&
           !highWater_.compare_exchange_weak(
               high, live, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
}

VariableControlInstanceStatus VariableControlInstanceTable::Acquire(
    const VariableControlInstanceCreateRequest& request,
    VariableControlInstanceToken* out) noexcept {
    return AcquireInternal(request, out, false);
}

VariableControlInstanceStatus VariableControlInstanceTable::PrepareAcquire(
    const VariableControlInstanceCreateRequest& request,
    PreparedVariableControlInstance* out) noexcept {
    if (out == nullptr || out->Valid()) {
        return VariableControlInstanceStatus::Invalid;
    }
    VariableControlInstanceToken token{};
    const VariableControlInstanceStatus status =
        AcquireInternal(request, &token, true);
    if (status != VariableControlInstanceStatus::Ok) return status;
    out->owner_ = this;
    out->token_ = token;
    out->prepared_ = true;
    out->originalRequest_ = request;
    out->originalSides_ = {};
    return VariableControlInstanceStatus::Ok;
}

VariableControlInstanceStatus VariableControlInstanceTable::AcquireInternal(
    const VariableControlInstanceCreateRequest& request,
    VariableControlInstanceToken* out, bool prepared) noexcept {
    if (out == nullptr || request.world == 0 || request.rootInstance == 0 ||
        request.containerStableId == VnObjectId{} ||
        request.programSemanticFingerprint == 0) {
        return VariableControlInstanceStatus::Invalid;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Entry& entry = entries_[slot];
        SlotState expected = SlotState::Free;
        if (!entry.state.compare_exchange_strong(
                expected, SlotState::Initializing,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        const std::uint32_t generation =
            entry.generation.load(std::memory_order_relaxed);
        if (generation == 0) {
            entry.state.store(SlotState::Quarantined,
                              std::memory_order_release);
            continue;
        }
        ResetEntry(entry);
        entry.world.store(request.world, std::memory_order_relaxed);
        entry.rootInstance.store(request.rootInstance,
                                 std::memory_order_relaxed);
        entry.containerIdHigh.store(request.containerStableId.high, std::memory_order_relaxed);
        entry.containerIdLow.store(request.containerStableId.low, std::memory_order_relaxed);
        entry.programSemanticFingerprint.store(
            request.programSemanticFingerprint,
            std::memory_order_relaxed);
        *out = {slot, generation};
        if (!prepared) {
            entry.state.store(SlotState::Live, std::memory_order_release);
            NoteLiveAcquire();
        }
        return VariableControlInstanceStatus::Ok;
    }
    return VariableControlInstanceStatus::Capacity;
}

bool VariableControlInstanceTable::CommitPreparedAcquire(
    PreparedVariableControlInstance* prepared,
    VariableControlInstanceToken* out) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || out == nullptr ||
        !prepared->token_.Valid() || prepared->token_.slot >= capacity_) {
        return false;
    }
    Entry& entry = entries_[prepared->token_.slot];
    if (entry.generation.load(std::memory_order_acquire) !=
        prepared->token_.generation) {
        return false;
    }
    SlotState expected = SlotState::Initializing;
    if (!entry.state.compare_exchange_strong(
            expected, SlotState::Live, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    *out = prepared->token_;
    prepared->owner_ = nullptr;
    prepared->token_ = {};
    prepared->prepared_ = false;
    prepared->originalRequest_ = {};
    prepared->originalSides_ = {};
    NoteLiveAcquire();
    return true;
}

bool VariableControlInstanceTable::CancelPreparedAcquire(
    PreparedVariableControlInstance* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || !prepared->token_.Valid() ||
        prepared->token_.slot >= capacity_) {
        return false;
    }
    Entry& entry = entries_[prepared->token_.slot];
    if (entry.generation.load(std::memory_order_acquire) !=
        prepared->token_.generation || entry.state.load(std::memory_order_acquire) != SlotState::Initializing) {
        return false;
    }
    for (const auto* side : {&entry.discrete, &entry.continuous})
        if (side->frontierPool != nullptr &&
            side->frontierPool->HasFrontierOwnerOperations(side->frontierOwner)) return false;
    SlotState expected = SlotState::Initializing;
    if (!entry.state.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    ResetEntry(entry);
    const auto generation = entry.generation.load(std::memory_order_relaxed);
    entry.generation.store(generation == UINT32_MAX ? 0 : generation + 1u, std::memory_order_relaxed);
    entry.state.store(generation == UINT32_MAX ? SlotState::Quarantined : SlotState::Free, std::memory_order_release);
    prepared->owner_ = nullptr;
    prepared->token_ = {};
    prepared->prepared_ = false;
    prepared->originalRequest_ = {};
    prepared->originalSides_ = {};
    return true;
}

VariableControlInstanceStatus VariableControlInstanceTable::PrepareSide(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    DecisionToken decision,
    BankVersionToken bank) noexcept {
    return PrepareSideInternal(instance, side, decision, bank, nullptr);
}

VariableControlInstanceStatus VariableControlInstanceTable::PrepareSide(
    PreparedVariableControlInstance& original, VariableControlSide side,
    DecisionToken decision, BankVersionToken bank) noexcept {
    if (original.owner_ != this || !original.prepared_) return VariableControlInstanceStatus::Stale;
    return PrepareSideInternal(original.token_, side, decision, bank, &original);
}

VariableControlInstanceStatus VariableControlInstanceTable::PrepareSideInternal(
    VariableControlInstanceToken instance, VariableControlSide side,
    DecisionToken decision, BankVersionToken bank,
    PreparedVariableControlInstance* original) noexcept {
    Entry* entry = ResolvePreparing(instance);
    if (entry == nullptr) return VariableControlInstanceStatus::Stale;
    if (original != nullptr && entry->state.load(std::memory_order_acquire) != SlotState::Initializing)
        return VariableControlInstanceStatus::Stale;
    if (!decision.Valid() || !bank.Valid() ||
        (side != VariableControlSide::Discrete &&
         side != VariableControlSide::Continuous)) {
        return VariableControlInstanceStatus::Invalid;
    }
    SideRow* target =
        side == VariableControlSide::Discrete
            ? &entry->discrete
            : &entry->continuous;
    if (target->frontierPool != nullptr) return VariableControlInstanceStatus::Stale;
    VariableControlSideState expected = VariableControlSideState::Empty;
    if (!target->state.compare_exchange_strong(
            expected, VariableControlSideState::Transitioning,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return expected == VariableControlSideState::Quarantined
            ? VariableControlInstanceStatus::Capacity
            : VariableControlInstanceStatus::Invalid;
    }
    const std::uint64_t generation =
        target->generation.load(std::memory_order_relaxed);
    if (generation == 0 || generation == UINT64_MAX) {
        target->generation.store(0, std::memory_order_relaxed);
        target->state.store(VariableControlSideState::Quarantined,
                            std::memory_order_release);
        return VariableControlInstanceStatus::Capacity;
    }
    target->decision.store(Pack(decision.slot, decision.generation),
                           std::memory_order_relaxed);
    const std::uint64_t packedBank = Pack(bank.slot, bank.generation);
    target->newestObserved.store(packedBank, std::memory_order_relaxed);
    target->newestPending.store(packedBank, std::memory_order_relaxed);
    target->lastApplied.store(0, std::memory_order_relaxed);
    target->generation.store(generation + 1u, std::memory_order_relaxed);
    target->state.store(VariableControlSideState::Pending,
                        std::memory_order_release);
    if (original != nullptr)
        original->originalSides_[side == VariableControlSide::Discrete ? 0 : 1] =
            {decision, bank, generation + 1u, true};
    return VariableControlInstanceStatus::Ok;
}

VariableControlInstanceStatus VariableControlInstanceTable::CommitSilentSide(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    BankVersionToken bank) noexcept {
    Entry* entry = ResolvePreparing(instance);
    if (entry == nullptr) return VariableControlInstanceStatus::Stale;
    if (!bank.Valid() ||
        (side != VariableControlSide::Discrete &&
         side != VariableControlSide::Continuous)) {
        return VariableControlInstanceStatus::Invalid;
    }
    SideRow* target = side == VariableControlSide::Discrete
        ? &entry->discrete
        : &entry->continuous;
    if (target->frontierPool != nullptr) return VariableControlInstanceStatus::Stale;
    VariableControlSideState expected = VariableControlSideState::Empty;
    if (!target->state.compare_exchange_strong(
            expected, VariableControlSideState::Transitioning,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return VariableControlInstanceStatus::Invalid;
    }
    const std::uint64_t packedBank = Pack(bank.slot, bank.generation);
    target->decision.store(0, std::memory_order_relaxed);
    target->newestObserved.store(packedBank, std::memory_order_relaxed);
    target->newestPending.store(0, std::memory_order_relaxed);
    target->lastApplied.store(packedBank, std::memory_order_relaxed);
    target->state.store(VariableControlSideState::Committed,
                        std::memory_order_release);
    return VariableControlInstanceStatus::Ok;
}

VariableControlInstanceStatus
VariableControlInstanceTable::AdvanceCommittedSideBank(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    BankVersionToken expectedApplied,
    BankVersionToken newerBank) noexcept {
    Entry* entry = Resolve(instance);
    if (entry == nullptr) return VariableControlInstanceStatus::Stale;
    if (!expectedApplied.Valid() || !newerBank.Valid() ||
        newerBank.slot != expectedApplied.slot ||
        newerBank.generation <= expectedApplied.generation ||
        (side != VariableControlSide::Discrete &&
         side != VariableControlSide::Continuous)) {
        return VariableControlInstanceStatus::Invalid;
    }
    SideRow& target = side == VariableControlSide::Discrete
        ? entry->discrete
        : entry->continuous;
    if (target.frontierPool != nullptr &&
        (target.state.load(std::memory_order_acquire) != VariableControlSideState::Committed ||
         target.lastApplied.load(std::memory_order_acquire) != Pack(expectedApplied.slot, expectedApplied.generation) ||
         target.newestObserved.load(std::memory_order_acquire) != Pack(expectedApplied.slot, expectedApplied.generation)))
        return VariableControlInstanceStatus::Stale;
    if (!MayMutateSide(target)) return VariableControlInstanceStatus::Stale;
    VariableControlSideState expectedState =
        VariableControlSideState::Committed;
    if (!target.state.compare_exchange_strong(
            expectedState, VariableControlSideState::Transitioning,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return VariableControlInstanceStatus::Stale;
    }
    const std::uint64_t expectedPacked =
        Pack(expectedApplied.slot, expectedApplied.generation);
    if (target.lastApplied.load(std::memory_order_acquire) != expectedPacked ||
        target.newestObserved.load(std::memory_order_acquire) !=
            expectedPacked) {
        target.state.store(VariableControlSideState::Committed,
                           std::memory_order_release);
        return VariableControlInstanceStatus::Stale;
    }
    const std::uint64_t newerPacked =
        Pack(newerBank.slot, newerBank.generation);
    target.newestObserved.store(newerPacked, std::memory_order_relaxed);
    target.newestPending.store(0, std::memory_order_relaxed);
    target.lastApplied.store(newerPacked, std::memory_order_relaxed);
    target.state.store(VariableControlSideState::Committed,
                       std::memory_order_release);
    return VariableControlInstanceStatus::Ok;
}

VariableControlInstanceStatus
VariableControlInstanceTable::PrepareNewerSide(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    DecisionToken decision,
    BankVersionToken bank,
    DecisionToken* outSupersededDecision) noexcept {
    Entry* entry = Resolve(instance);
    if (entry == nullptr) return VariableControlInstanceStatus::Stale;
    if (!decision.Valid() || !bank.Valid() ||
        outSupersededDecision == nullptr ||
        (side != VariableControlSide::Discrete &&
         side != VariableControlSide::Continuous)) {
        return VariableControlInstanceStatus::Invalid;
    }
    SideRow& target = side == VariableControlSide::Discrete
        ? entry->discrete
        : entry->continuous;
    if (target.frontierPool != nullptr) {
        const auto observed = UnpackBank(target.newestObserved.load(std::memory_order_acquire));
        if (!observed.Valid() || bank.slot != observed.slot || bank.generation <= observed.generation)
            return VariableControlInstanceStatus::Stale;
    }
    if (!MayMutateSide(target)) return VariableControlInstanceStatus::Stale;
    for (;;) {
        VariableControlSideState priorState =
            target.state.load(std::memory_order_acquire);
        if (priorState == VariableControlSideState::Transitioning) {
            if (target.frontierPool != nullptr) return VariableControlInstanceStatus::Stale;
            continue;
        }
        if (priorState == VariableControlSideState::Quarantined) {
            return VariableControlInstanceStatus::Capacity;
        }
        if (priorState != VariableControlSideState::Pending &&
            priorState != VariableControlSideState::Committed) {
            return VariableControlInstanceStatus::Invalid;
        }
        if (target.state.compare_exchange_weak(
                priorState, VariableControlSideState::Transitioning,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            const BankVersionToken observed = UnpackBank(
                target.newestObserved.load(std::memory_order_acquire));
            if (!observed.Valid() || bank.slot != observed.slot ||
                bank.generation <= observed.generation) {
                target.state.store(priorState, std::memory_order_release);
                return VariableControlInstanceStatus::Stale;
            }
            const DecisionToken superseded = UnpackDecision(
                target.decision.load(std::memory_order_acquire));
            if (!superseded.Valid() &&
                (priorState != VariableControlSideState::Committed ||
                 !UnpackBank(target.lastApplied.load(
                     std::memory_order_acquire)).Valid())) {
                target.state.store(priorState, std::memory_order_release);
                return VariableControlInstanceStatus::Invalid;
            }
            const std::uint64_t generation =
                target.generation.load(std::memory_order_relaxed);
            if (generation == 0 || generation == UINT64_MAX) {
                target.generation.store(0, std::memory_order_relaxed);
                target.state.store(VariableControlSideState::Quarantined,
                                   std::memory_order_release);
                return VariableControlInstanceStatus::Capacity;
            }
            const std::uint64_t packedBank =
                Pack(bank.slot, bank.generation);
            target.decision.store(
                Pack(decision.slot, decision.generation),
                std::memory_order_relaxed);
            target.newestObserved.store(packedBank,
                                        std::memory_order_relaxed);
            target.newestPending.store(packedBank,
                                       std::memory_order_relaxed);
            target.generation.store(generation + 1u,
                                    std::memory_order_relaxed);
            target.state.store(VariableControlSideState::Pending,
                               std::memory_order_release);
            *outSupersededDecision = superseded;
            return VariableControlInstanceStatus::Ok;
        }
    }
}

bool VariableControlInstanceTable::CancelPreparedSideAndRestoreCommitted(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    DecisionToken preparedDecision,
    DecisionToken priorDecision) noexcept {
    Entry* entry = Resolve(instance);
    if (entry == nullptr || !preparedDecision.Valid() ||
        (side != VariableControlSide::Discrete &&
         side != VariableControlSide::Continuous)) {
        return false;
    }
    SideRow& target = side == VariableControlSide::Discrete
        ? entry->discrete
        : entry->continuous;
    if (target.decision.load(std::memory_order_acquire) !=
        Pack(preparedDecision.slot, preparedDecision.generation)) {
        return false;
    }
    if (!MayMutateSide(target)) return false;
    VariableControlSideState expected = VariableControlSideState::Pending;
    if (!target.state.compare_exchange_strong(
            expected, VariableControlSideState::Transitioning,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    const std::uint64_t applied =
        target.lastApplied.load(std::memory_order_acquire);
    if (!UnpackBank(applied).Valid() ||
        target.decision.load(std::memory_order_acquire) !=
            Pack(preparedDecision.slot, preparedDecision.generation)) {
        target.state.store(VariableControlSideState::Pending,
                           std::memory_order_release);
        return false;
    }
    target.decision.store(
        priorDecision.Valid()
            ? Pack(priorDecision.slot, priorDecision.generation)
            : 0,
        std::memory_order_relaxed);
    target.newestObserved.store(applied, std::memory_order_relaxed);
    target.newestPending.store(0, std::memory_order_relaxed);
    target.state.store(VariableControlSideState::Committed,
                       std::memory_order_release);
    return true;
}

bool VariableControlInstanceTable::ActivateSide(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    DecisionToken decision,
    BankVersionToken bank) noexcept {
    VariableControlActivationProof proof{};
    return ProveActivation(instance, decision, &proof) &&
           proof.side == side && proof.bank.slot == bank.slot &&
           proof.bank.generation == bank.generation &&
           ActivateProven(proof);
}

bool VariableControlInstanceTable::CanActivateDecision(
    VariableControlInstanceToken instance,
    DecisionToken decision) const noexcept {
    VariableControlActivationProof proof{};
    return ProveActivation(instance, decision, &proof);
}

bool VariableControlInstanceTable::ProveActivation(
    VariableControlInstanceToken instance,
    DecisionToken decision,
    VariableControlActivationProof* out) const noexcept {
    const Entry* entry = Resolve(instance);
    if (entry == nullptr || !decision.Valid() || out == nullptr) return false;
    const std::uint64_t packed = Pack(decision.slot, decision.generation);
    const auto prove = [packed](const SideRow& side,
                                VariableControlSide selected,
                                VariableControlInstanceToken token,
                                DecisionToken exact,
                                VariableControlActivationProof* proof) {
        const VariableControlSideState state =
            side.state.load(std::memory_order_acquire);
        const std::uint64_t sideDecision =
            side.decision.load(std::memory_order_acquire);
        const std::uint64_t sideBank =
            side.newestPending.load(std::memory_order_acquire);
        const std::uint64_t generation =
            side.generation.load(std::memory_order_acquire);
        if (state != VariableControlSideState::Pending ||
            sideDecision != packed || !UnpackBank(sideBank).Valid() ||
            generation == 0 ||
            side.state.load(std::memory_order_acquire) != state ||
            side.generation.load(std::memory_order_acquire) != generation) {
            return false;
        }
        proof->instance = token;
        proof->side = selected;
        proof->decision = exact;
        proof->bank = UnpackBank(sideBank);
        proof->sideGeneration = generation;
        return true;
    };
    VariableControlActivationProof discreteProof{};
    VariableControlActivationProof continuousProof{};
    const bool discrete = prove(
        entry->discrete, VariableControlSide::Discrete, instance,
        decision, &discreteProof);
    const bool continuous = prove(
        entry->continuous, VariableControlSide::Continuous, instance,
        decision, &continuousProof);
    if (discrete == continuous) return false;
    *out = discrete ? discreteProof : continuousProof;
    return true;
}

bool VariableControlInstanceTable::ActivateProven(
    const VariableControlActivationProof& proof) noexcept {
    Entry* entry = Resolve(proof.instance);
    if (entry == nullptr || !proof.decision.Valid() ||
        !proof.bank.Valid() || proof.sideGeneration == 0 ||
        (proof.side != VariableControlSide::Discrete &&
         proof.side != VariableControlSide::Continuous)) {
        return false;
    }
    SideRow& target = proof.side == VariableControlSide::Discrete
        ? entry->discrete
        : entry->continuous;
    if (target.frontierPool != nullptr) return false;
    VariableControlSideState expected = VariableControlSideState::Pending;
    if (!target.state.compare_exchange_strong(
            expected, VariableControlSideState::Transitioning,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    const std::uint64_t packedDecision =
        Pack(proof.decision.slot, proof.decision.generation);
    const std::uint64_t packedBank =
        Pack(proof.bank.slot, proof.bank.generation);
    if (target.generation.load(std::memory_order_acquire) !=
            proof.sideGeneration ||
        target.decision.load(std::memory_order_acquire) != packedDecision ||
        target.newestPending.load(std::memory_order_acquire) != packedBank) {
        target.state.store(VariableControlSideState::Pending,
                           std::memory_order_release);
        return false;
    }
    target.lastApplied.store(packedBank, std::memory_order_relaxed);
    target.state.store(VariableControlSideState::Committed,
                       std::memory_order_release);
    return true;
}

void VariableControlInstanceTable::ApplyClaimedActivation(
    const VariableControlActivationProof& proof) noexcept {
    Entry* entry = Resolve(proof.instance);
    if (entry == nullptr) return;
    SideRow& target = proof.side == VariableControlSide::Discrete
        ? entry->discrete
        : entry->continuous;
    if (target.frontierPool != nullptr) return;
    target.lastApplied.store(
        Pack(proof.bank.slot, proof.bank.generation),
        std::memory_order_relaxed);
    target.state.store(VariableControlSideState::Committed,
                       std::memory_order_release);
}

bool VariableControlInstanceTable::ActivateDecision(
    VariableControlInstanceToken instance,
    DecisionToken decision) noexcept {
    VariableControlActivationProof proof{};
    return ProveActivation(instance, decision, &proof) &&
           ActivateProven(proof);
}

bool VariableControlInstanceTable::CancelSide(
    VariableControlInstanceToken instance,
    VariableControlSide side,
    DecisionToken decision) noexcept {
    Entry* entry = Resolve(instance);
    if (entry == nullptr || !decision.Valid() ||
        (side != VariableControlSide::Discrete &&
         side != VariableControlSide::Continuous)) {
        return false;
    }
    SideRow& target = side == VariableControlSide::Discrete
        ? entry->discrete
        : entry->continuous;
    if (target.decision.load(std::memory_order_acquire) !=
        Pack(decision.slot, decision.generation)) {
        return false;
    }
    if (!MayMutateSide(target)) return false;
    VariableControlSideState expected =
        VariableControlSideState::Pending;
    return target.state.compare_exchange_strong(
        expected, VariableControlSideState::Terminal,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool VariableControlInstanceTable::Release(
    VariableControlInstanceToken instance) noexcept {
    Entry* entry = Resolve(instance);
    if (entry != nullptr) {
        for (const auto* side : {&entry->discrete, &entry->continuous})
            if (side->frontierPool != nullptr &&
                side->frontierPool->HasFrontierOwnerOperations(side->frontierOwner)) return false;
    }
    if (entry == nullptr ||
        entry->discrete.state.load(std::memory_order_acquire) ==
            VariableControlSideState::Pending ||
        entry->discrete.state.load(std::memory_order_acquire) ==
            VariableControlSideState::Transitioning ||
        entry->continuous.state.load(std::memory_order_acquire) ==
            VariableControlSideState::Pending ||
        entry->continuous.state.load(std::memory_order_acquire) ==
            VariableControlSideState::Transitioning) {
        return false;
    }
    SlotState expected = SlotState::Live;
    if (!entry->state.compare_exchange_strong(
            expected, SlotState::Reclaiming,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    const std::uint32_t generation =
        entry->generation.load(std::memory_order_relaxed);
    if (generation == UINT32_MAX) {
        entry->generation.store(0, std::memory_order_relaxed);
        entry->state.store(SlotState::Quarantined,
                           std::memory_order_release);
        liveCount_.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    entry->generation.store(generation + 1u, std::memory_order_relaxed);
    entry->state.store(SlotState::Free, std::memory_order_release);
    liveCount_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool VariableControlInstanceTable::Capture(
    VariableControlInstanceToken instance,
    VariableControlInstanceSnapshot* out) const noexcept {
    if (out == nullptr) return false;
    const Entry* entry = Resolve(instance);
    if (entry == nullptr) return false;
    VariableControlInstanceSnapshot snapshot{};
    snapshot.world = entry->world.load(std::memory_order_relaxed);
    snapshot.rootInstance =
        entry->rootInstance.load(std::memory_order_relaxed);
    snapshot.containerStableId = {
        entry->containerIdHigh.load(std::memory_order_relaxed),
        entry->containerIdLow.load(std::memory_order_relaxed)};
    snapshot.programSemanticFingerprint =
        entry->programSemanticFingerprint.load(std::memory_order_relaxed);
    const auto captureSide = [](const SideRow& source,
                                VariableControlSideSnapshot* target) {
        target->state = source.state.load(std::memory_order_acquire);
        target->decision = UnpackDecision(
            source.decision.load(std::memory_order_relaxed));
        target->newestObserved = UnpackBank(
            source.newestObserved.load(std::memory_order_relaxed));
        target->newestPending = UnpackBank(
            source.newestPending.load(std::memory_order_relaxed));
        target->lastApplied = UnpackBank(
            source.lastApplied.load(std::memory_order_relaxed));
    };
    captureSide(entry->discrete, &snapshot.discrete);
    captureSide(entry->continuous, &snapshot.continuous);
    if (entry->state.load(std::memory_order_acquire) != SlotState::Live ||
        entry->generation.load(std::memory_order_acquire) !=
            instance.generation) {
        return false;
    }
    *out = snapshot;
    return true;
}

} // namespace harken
