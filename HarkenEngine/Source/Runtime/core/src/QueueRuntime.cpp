#include "harken/core/QueueRuntime.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/CompositeCompletionOwnerPool.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/ScheduledTransactionPool.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <cassert>

namespace harken {

namespace {

bool SameDecision(DecisionToken a, DecisionToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}

bool SameOccurrence(OccurrenceToken a, OccurrenceToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}

bool ValidOccurrence(OccurrenceToken token) noexcept {
    return token.slot != UINT32_MAX && token.generation != 0;
}

const CompositeCompletionOwnerPool& EmptyCompositeOwners() noexcept {
    static const CompositeCompletionOwnerPool pool(0, 1);
    return pool;
}

bool ValidRetiredSourceProof(
    const RetiredQueueSourceProof& proof) noexcept {
    return proof.sourceArm.Valid() && proof.sourceRenderHandle != 0 &&
        proof.logicalCompletionSequence != 0 &&
        proof.tombstoneAudioSequence <= proof.logicalCompletionSequence &&
        proof.sourceArm.renderSlot ==
            static_cast<std::uint32_t>(proof.sourceRenderHandle) &&
        proof.sourceArm.renderGeneration ==
            static_cast<std::uint32_t>(proof.sourceRenderHandle >> 32);
}

bool EmptyRetiredSourceProof(
    const RetiredQueueSourceProof& proof) noexcept {
    return !proof.sourceArm.Valid() && proof.sourceRenderHandle == 0 &&
        proof.logicalCompletionSequence == 0 &&
        proof.tombstoneAudioSequence == 0;
}

bool ValidEffectiveState(const EffectiveQueueState& state,
                         std::uint32_t childCount) noexcept {
    return state.version != 0 && state.cursor < childCount &&
           state.randomInitialRecentCount <= kMaxQueueAvoidHistoryHard;
}

bool QueueCycleWraps(const ContainerProgramViewV3& program,
                    std::uint64_t cycleOrdinal) noexcept {
    if (program.queueCycle != QueueCycleV1::Loop ||
        cycleOrdinal == UINT64_MAX) {
        return false;
    }
    const std::uint32_t limit = program.queueCycleCount;
    return limit == 0 || cycleOrdinal + 1u < limit;
}

bool ValidCheckpointKey(const QueueCheckpointKey& key) noexcept {
    return key.world != 0 && key.containerStableId != VnObjectId{} &&
           key.programSemanticFingerprint != 0 && key.resetEpoch != 0 &&
           ((key.emitterSlot == UINT32_MAX &&
             key.emitterGeneration == 0) ||
            (key.emitterSlot != UINT32_MAX &&
             key.emitterGeneration != 0));
}

bool SameCheckpointKey(const QueueCheckpointKey& a,
                       const QueueCheckpointKey& b) noexcept {
    return a.world == b.world &&
           a.containerStableId == b.containerStableId &&
           a.programSemanticFingerprint ==
               b.programSemanticFingerprint &&
           a.resetEpoch == b.resetEpoch &&
           a.emitterSlot == b.emitterSlot &&
           a.emitterGeneration == b.emitterGeneration;
}

bool ValidEmitterIdentity(std::uint32_t slot,
                          std::uint32_t generation) noexcept {
    return (slot == UINT32_MAX && generation == 0) ||
           (slot != UINT32_MAX && generation != 0);
}

bool SameEffectiveState(const EffectiveQueueState& a,
                        const EffectiveQueueState& b) noexcept {
    return a.version == b.version && a.cursor == b.cursor &&
           a.currentChildOrdinal == b.currentChildOrdinal &&
           a.cycleOrdinal == b.cycleOrdinal &&
           a.selectionOrdinal == b.selectionOrdinal &&
           a.randomInitialRecentCount == b.randomInitialRecentCount &&
           a.randomInitialRecent == b.randomInitialRecent;
}

bool ValidateProjection(const ConditionalQueuePrepareRequest& request,
                        const EffectiveQueueState& current, std::uint32_t childCount,
                        EffectiveQueueState* first, std::uint32_t* count) noexcept {
    if (request.expectedCurrentVersion != current.version || current.version == UINT64_MAX ||
        (request.projectedStates == nullptr) != (request.projectedStateCount == 0)) return false;
    const auto* states = request.projectedStates != nullptr ? request.projectedStates : &request.next;
    const std::uint32_t size = request.projectedStates != nullptr ? request.projectedStateCount : 1;
    if (size > static_cast<std::uint64_t>(childCount) + 1u ||
        size > UINT64_MAX - current.version) return false;
    for (std::uint32_t index = 0; index < size; ++index) {
        const auto& state = states[index];
        if (!ValidEffectiveState(state, childCount) ||
            state.version != current.version + index + 1u) return false;
        if (index != 0) {
            const auto& prior = states[index - 1u];
            if (prior.selectionOrdinal == UINT64_MAX ||
                state.selectionOrdinal != prior.selectionOrdinal + 1u) return false;
            if (state.cycleOrdinal == prior.cycleOrdinal) {
                if (state.cursor != prior.cursor + 1u ||
                    state.randomInitialRecentCount != prior.randomInitialRecentCount ||
                    state.randomInitialRecent != prior.randomInitialRecent) return false;
            } else if (index + 1u != size || prior.cycleOrdinal == UINT64_MAX ||
                       state.cycleOrdinal != prior.cycleOrdinal + 1u ||
                       prior.cursor + 1u != childCount || state.cursor != 0) {
                return false;
            }
        }
    }
    if (!SameEffectiveState(states[size - 1u], request.next)) return false;
    *first = states[0];
    *count = size;
    return true;
}

} // namespace

QueueCheckpointTable::QueueCheckpointTable(std::uint32_t capacity)
    : rows_(capacity != 0 && capacity <= kMaxQueueCheckpointRowsHard
                ? std::make_unique<Row[]>(capacity)
                : nullptr),
      capacity_(rows_ ? capacity : 0) {}

QueueInstanceTable::~QueueInstanceTable() {
    assert(liveCatalogResetScopeMarker_ == 0);
    for (std::uint32_t index = 0; index < originalConditionalCapacity_; ++index) {
        const auto& original = originalConditionalRows_[index];
        assert(original.state == OriginalConditionalState::Free ||
            original.state == OriginalConditionalState::Quarantined);
        assert(original.loans == 0 && !original.parent.Valid());
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        assert(rows_[slot].originalLifetimePinCount == 0);
        assert(rows_[slot].originalLifetimeMarker == 0);
    }
}

bool QueueInstanceTable::OriginalLifetimePin::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalLifetimePin(*this);
}

bool QueueInstanceTable::OriginalLifetimePin::Borrow(OriginalLifetimePin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalLifetimePin(*this, out);
}

void QueueInstanceTable::OriginalLifetimePin::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseOriginalLifetimePin(*this);
    owner_ = nullptr;
    token_ = {};
    marker_ = 0;
}

bool QueueInstanceTable::MatchesOriginalLifetimePin(
    const OriginalLifetimePin& original) const noexcept {
    if (original.owner_ != this || !original.token_.Valid() ||
        original.token_.slot >= capacity_ || original.marker_ == 0) return false;
    const auto& row = rows_[original.token_.slot];
    const auto state = row.slotState.load(std::memory_order_acquire);
    return row.generation == original.token_.generation &&
        (state == SlotState::Initializing || state == SlotState::Live) &&
        row.originalLifetimeMarker == original.marker_ && row.originalLifetimePinCount != 0;
}

bool QueueInstanceTable::IsOriginalLifetimePinFor(const OriginalLifetimePin& original,
    QueueInstanceToken expected) const noexcept {
    return MatchesOriginalLifetimePin(original) && original.token_.slot == expected.slot &&
        original.token_.generation == expected.generation;
}

bool QueueInstanceTable::BorrowOriginalLifetimePin(const OriginalLifetimePin& original,
    OriginalLifetimePin* out) noexcept {
    if (!MatchesOriginalLifetimePin(original) || out == nullptr || out->owner_ != nullptr)
        return false;
    auto& row = rows_[original.token_.slot];
    if (row.originalLifetimePinCount == UINT32_MAX) return false;
    ++row.originalLifetimePinCount;
    out->owner_ = this;
    out->token_ = original.token_;
    out->marker_ = original.marker_;
    return true;
}

void QueueInstanceTable::ReleaseOriginalLifetimePin(OriginalLifetimePin& original) noexcept {
    const bool matches = MatchesOriginalLifetimePin(original);
    assert(matches);
    if (!matches) return;
    auto& row = rows_[original.token_.slot];
    if (--row.originalLifetimePinCount == 0) row.originalLifetimeMarker = 0;
}

void QueueInstanceTable::ConsumeOriginalLifetimePinAfterReclaim(
    OriginalLifetimePin* original, Row& row) noexcept {
    if (original == nullptr) return;
    // The successful native reclaim CAS has made ordinary Valid() inapplicable.
    assert(original->owner_ == this && original->token_.slot < capacity_ &&
        &rows_[original->token_.slot] == &row &&
        row.generation == original->token_.generation &&
        row.originalLifetimeMarker == original->marker_ &&
        row.originalLifetimePinCount == 1);
    row.originalLifetimePinCount = 0;
    row.originalLifetimeMarker = 0;
    original->owner_ = nullptr;
    original->token_ = {};
    original->marker_ = 0;
}

std::uint32_t QueueCheckpointTable::LiveCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        count += rows_[slot].live ? 1u : 0u;
    }
    return count;
}

bool QueueCheckpointTable::Store(
    const QueueCheckpointKey& key,
    const QueueCheckpointImage& image) noexcept {
    if (!ValidCheckpointKey(key) || image.effective.version == 0 ||
        !image.replayCurrent) {
        return false;
    }
    Row* freeRow = nullptr;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.live && SameCheckpointKey(row.key, key)) {
            if (row.resetReserved) return false;
            row.image = image;
            return true;
        }
        if (!row.live && freeRow == nullptr) freeRow = &row;
    }
    if (freeRow == nullptr) return false;
    freeRow->key = key;
    freeRow->image = image;
    freeRow->live = true;
    return true;
}

bool QueueCheckpointTable::Load(
    const QueueCheckpointKey& key,
    QueueCheckpointImage* out) const noexcept {
    if (out == nullptr || !ValidCheckpointKey(key)) return false;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        if (row.live && SameCheckpointKey(row.key, key)) {
            *out = row.image;
            return true;
        }
    }
    return false;
}

bool QueueCheckpointTable::Clear(
    const QueueCheckpointKey& key) noexcept {
    if (!ValidCheckpointKey(key)) return false;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.live && SameCheckpointKey(row.key, key)) {
            if (row.resetReserved) return false;
            row = {};
            return true;
        }
    }
    return false;
}

QueueResetStatusV1 QueueCheckpointTable::PrepareReset(
    const QueueCheckpointKey& key,
    PreparedQueueCheckpointResetV1* out) noexcept {
    if (out == nullptr || out->Valid() || !ValidCheckpointKey(key)) {
        return QueueResetStatusV1::InvalidInput;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (!row.live || !SameCheckpointKey(row.key, key)) continue;
        if (row.resetReserved) return QueueResetStatusV1::Backpressure;
        row.resetReserved = true;
        out->owner_ = this;
        out->slot_ = slot;
        out->prepared_ = true;
        return QueueResetStatusV1::Ok;
    }
    out->owner_ = this;
    out->prepared_ = true;
    out->idempotent_ = true;
    return QueueResetStatusV1::Ok;
}

QueueResetStatusV1 QueueCheckpointTable::CommitPreparedReset(
    PreparedQueueCheckpointResetV1* prepared,
    LogicCommitSequence sequence) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || sequence == 0) {
        return QueueResetStatusV1::InvalidInput;
    }
    if (prepared->idempotent_) {
        prepared->owner_ = nullptr;
        prepared->prepared_ = false;
        prepared->idempotent_ = false;
        return QueueResetStatusV1::Ok;
    }
    if (prepared->slot_ >= capacity_) return QueueResetStatusV1::NotFound;
    Row& row = rows_[prepared->slot_];
    if (!row.live || !row.resetReserved) {
        return QueueResetStatusV1::NotFound;
    }
    row = {};
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->prepared_ = false;
    return QueueResetStatusV1::Ok;
}

QueueResetStatusV1 QueueCheckpointTable::CancelPreparedReset(
    PreparedQueueCheckpointResetV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_) {
        return QueueResetStatusV1::NotFound;
    }
    if (prepared->idempotent_) {
        prepared->owner_ = nullptr;
        prepared->prepared_ = false;
        prepared->idempotent_ = false;
        return QueueResetStatusV1::Ok;
    }
    if (prepared->slot_ >= capacity_) return QueueResetStatusV1::NotFound;
    Row& row = rows_[prepared->slot_];
    if (!row.live || !row.resetReserved) {
        return QueueResetStatusV1::NotFound;
    }
    row.resetReserved = false;
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->prepared_ = false;
    return QueueResetStatusV1::Ok;
}

void QueueCheckpointTable::ClearWorld(WorldGeneration world) noexcept {
    if (world == 0) return;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].live && rows_[slot].key.world == world) {
            rows_[slot] = {};
        }
    }
}

void QueueCheckpointTable::ClearAll() noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        rows_[slot] = {};
    }
}

QueueInstanceTable::QueueInstanceTable(std::uint32_t capacity,
    std::uint32_t originalConditionalCapacity)
    : rows_(capacity != 0 && capacity <= kMaxQueueInstancesHard
                ? std::make_unique<Row[]>(capacity)
                : nullptr),
      originalConditionalRows_(rows_ && originalConditionalCapacity != 0 &&
              originalConditionalCapacity <= kMaxQueueInstancesHard
          ? std::make_unique<OriginalConditionalRow[]>(originalConditionalCapacity)
          : nullptr),
      originalConditionalCapacity_(originalConditionalRows_ ? originalConditionalCapacity : 0),
      capacity_(rows_ ? capacity : 0) {}

bool QueueInstanceTable::Valid() const noexcept {
    return rows_ != nullptr && capacity_ != 0;
}

bool QueueInstanceTable::PreparedCatalogResetScope::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesCatalogResetScope(*this);
}

void QueueInstanceTable::PreparedCatalogResetScope::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseCatalogResetScope(*this);
    owner_ = nullptr;
    marker_ = 0;
    matchedCount_ = 0;
    nativeOnly_ = false;
}

bool QueueInstanceTable::MatchesCatalogResetScope(
    const PreparedCatalogResetScope& original) const noexcept {
    return original.owner_ == this && original.marker_ != 0 &&
        original.marker_ == liveCatalogResetScopeMarker_;
}

QueueResetStatusV1 QueueInstanceTable::PrepareCatalogResetScope(
    const QueueResetTargetV1& target, PreparedCatalogResetScope* out) noexcept {
    return PrepareCatalogResetScopeInternal(target, false, out);
}

QueueResetStatusV1 QueueInstanceTable::PrepareNativeConditionalCatalogResetScope(
    const QueueResetTargetV1& target, PreparedCatalogResetScope* out) noexcept {
    return PrepareCatalogResetScopeInternal(target, true, out);
}

bool QueueInstanceTable::HasCurrentNativeConditional(const Row& row) const noexcept {
    if (row.originalConditionalRow >= originalConditionalCapacity_) return false;
    const auto& original = originalConditionalRows_[row.originalConditionalRow];
    if (original.state != OriginalConditionalState::Committed || original.selectedFutureConsumed ||
        original.marker != row.originalConditionalMarker ||
        original.queue.slot >= capacity_ || &rows_[original.queue.slot] != &row ||
        original.queue.generation != row.generation || !original.parent.Valid() ||
        (original.clockState != OriginalConditionalClockState::Unbound &&
         original.clockState != OriginalConditionalClockState::Bound) ||
        original.clockPool == nullptr || !original.clockGroups[0].Valid() ||
        !original.clockGroups[1].Valid()) return false;
    EffectiveQueueState current{};
    return row.pendingVersion.load(std::memory_order_acquire) == original.version.version &&
        row.pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Waiting &&
        row.failurePhase.load(std::memory_order_acquire) == QueueFailurePhase::Healthy &&
        !row.conditionalReserved.load(std::memory_order_acquire) &&
        SameDecision(row.pendingDecision, original.decision) &&
        SameOccurrence(row.pendingExpectedCurrent, original.current) &&
        SameOccurrence(row.pendingSuccessor, original.successor) &&
        SameCompletionOwner(row.pendingCompletionOwner, original.completionOwner) &&
        LoadMirror(row, &current) && SameEffectiveState(current, original.currentState);
}

QueueResetStatusV1 QueueInstanceTable::PrepareCatalogResetScopeInternal(
    const QueueResetTargetV1& target, bool nativeOnly, PreparedCatalogResetScope* out) noexcept {
    if (!Valid() || out == nullptr || out->owner_ != nullptr || target.world == 0 ||
        target.containerStableId == VnObjectId{} || target.programSemanticFingerprint == 0 ||
        !ValidEmitterIdentity(target.emitterSlot, target.emitterGeneration))
        return QueueResetStatusV1::InvalidInput;
    if (liveCatalogResetScopeMarker_ != 0) return QueueResetStatusV1::Backpressure;
    if (nextCatalogResetScopeMarker_ == UINT64_MAX)
        return QueueResetStatusV1::RuntimeRecreateRequired;
    liveCatalogResetScopeMarker_ = ++nextCatalogResetScopeMarker_;
    out->owner_ = this;
    out->marker_ = liveCatalogResetScopeMarker_;
    out->matchedCount_ = 0;
    out->nativeOnly_ = nativeOnly;
    return ExtendCatalogResetScope(*out, target);
}

QueueResetStatusV1 QueueInstanceTable::ExtendCatalogResetScope(
    PreparedCatalogResetScope& original, const QueueResetTargetV1& target) noexcept {
    if (!MatchesCatalogResetScope(original) || target.world == 0 ||
        target.containerStableId == VnObjectId{} || target.programSemanticFingerprint == 0 ||
        !ValidEmitterIdentity(target.emitterSlot, target.emitterGeneration))
        return QueueResetStatusV1::InvalidInput;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        const auto state = row.slotState.load(std::memory_order_acquire);
        if ((state != SlotState::Live && state != SlotState::Initializing) ||
            !SameResetTarget(row, target) || row.catalogResetScopeMarker == original.marker_ ||
            (original.nativeOnly_ && !HasCurrentNativeConditional(row)))
            continue;
        assert(row.catalogResetScopeMarker == 0);
        row.catalogResetScopeGeneration = row.generation;
        row.catalogResetScopeMarker = original.marker_;
        ++original.matchedCount_;
    }
    return QueueResetStatusV1::Ok;
}

bool QueueInstanceTable::CaptureCatalogResetScopeQueue(
    const PreparedCatalogResetScope& original, std::uint32_t index,
    QueueInstanceToken* outToken, QueueInstanceSnapshot* outSnapshot) const noexcept {
    if (!MatchesCatalogResetScope(original) || index >= original.matchedCount_ ||
        outToken == nullptr || outSnapshot == nullptr) return false;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        if (row.catalogResetScopeMarker != original.marker_) continue;
        if (index-- != 0) continue;
        const QueueInstanceToken token{slot, row.catalogResetScopeGeneration};
        const auto state = row.slotState.load(std::memory_order_acquire);
        if (row.generation != token.generation ||
            (state != SlotState::Initializing && state != SlotState::Live) ||
            !CaptureRowSnapshot(row, outSnapshot)) return false;
        *outToken = token;
        return true;
    }
    return false;
}

void QueueInstanceTable::ReleaseCatalogResetScope(
    PreparedCatalogResetScope& original) noexcept {
    assert(MatchesCatalogResetScope(original));
    if (!MatchesCatalogResetScope(original)) return;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.catalogResetScopeMarker != original.marker_) continue;
        const auto state = row.slotState.load(std::memory_order_acquire);
        assert(row.generation == row.catalogResetScopeGeneration &&
            (state == SlotState::Live || state == SlotState::Initializing));
        (void)state;
        row.catalogResetScopeMarker = 0;
        row.catalogResetScopeGeneration = 0;
    }
    liveCatalogResetScopeMarker_ = 0;
}

std::uint32_t QueueInstanceTable::LiveCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        if (rows_[i].slotState.load(std::memory_order_acquire) ==
            SlotState::Live) {
            ++count;
        }
    }
    return count;
}

std::uint32_t QueueInstanceTable::QuarantinedCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        if (rows_[i].slotState.load(std::memory_order_acquire) ==
            SlotState::Quarantined) {
            ++count;
        }
    }
    return count;
}

QueueInstanceTable::Row* QueueInstanceTable::Resolve(
    QueueInstanceToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.generation != token.generation) {
        return nullptr;
    }
    return &row;
}

const QueueInstanceTable::Row* QueueInstanceTable::Resolve(
    QueueInstanceToken token) const noexcept {
    if (!token.Valid() || token.slot >= capacity_) return nullptr;
    const Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.generation != token.generation) {
        return nullptr;
    }
    return &row;
}

bool QueueInstanceTable::LoadMirror(
    const Row& row, EffectiveQueueState* out) noexcept {
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        const std::uint64_t first =
            row.mirrorSequence.load(std::memory_order_acquire);
        if (first == 0 || (first & 1u) != 0) continue;
        EffectiveQueueState state{};
        state.version = row.mirrorVersion.load(std::memory_order_relaxed);
        state.cursor = row.mirrorCursor.load(std::memory_order_relaxed);
        state.currentChildOrdinal =
            row.mirrorCurrentChildOrdinal.load(std::memory_order_relaxed);
        state.cycleOrdinal =
            row.mirrorCycleOrdinal.load(std::memory_order_relaxed);
        state.selectionOrdinal =
            row.mirrorSelectionOrdinal.load(std::memory_order_relaxed);
        state.randomInitialRecentCount =
            row.mirrorRandomInitialRecentCount.load(
                std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < kMaxQueueAvoidHistoryHard; ++i) {
            state.randomInitialRecent[i] =
                row.mirrorRandomInitialRecent[i].load(
                    std::memory_order_relaxed);
        }
        const std::uint64_t second =
            row.mirrorSequence.load(std::memory_order_acquire);
        if (first == second && (second & 1u) == 0) {
            *out = state;
            return true;
        }
    }
    return false;
}

bool QueueInstanceTable::MayMutateFrontier(const Row& row) noexcept {
    return row.frontierPool == nullptr || row.frontierPool->InvalidateFrontierOwnerOperations(
        row.frontierOwner, {}, AudioSliceInvalidateReason::Cancel);
}

bool QueueInstanceTable::HasFrontierReaders(const Row& row) noexcept {
    return row.frontierPool != nullptr && row.frontierPool->HasFrontierOwnerOperations(row.frontierOwner);
}

bool QueueInstanceTable::BindFrontierInitial(QueueInstanceToken token,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept {
    if (!token.Valid() || token.slot >= capacity_ || !binding.Valid() || !binding.queueInitial.Valid() ||
        binding.variableSide.Valid() || binding.queueInitial.slot != token.slot ||
        binding.queueInitial.generation != token.generation ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group)) return false;
    auto& row = rows_[token.slot];
    const auto state = row.slotState.load(std::memory_order_acquire);
    Plan03PageGroupView group{};
    if ((state != SlotState::Initializing && state != SlotState::Live) || row.generation != token.generation ||
        row.world != binding.operation.world || row.rootInstance != binding.operation.owner.child ||
        row.programSemanticFingerprint != binding.queueInitial.programSemanticFingerprint ||
        row.mirrorVersion.load(std::memory_order_acquire) != binding.queueInitial.version ||
        row.initialActivationSequence.load(std::memory_order_acquire) != 0 ||
        (row.frontierInitial != nullptr && row.frontierInitial != &binding) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) || group.frontier != &binding ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready ||
        (row.frontierPool != nullptr && (row.frontierPool != &pool || !(row.frontierOwner == binding.operation.owner))))
        return false;
    row.frontierPool = &pool;
    row.frontierInitial = &binding;
    row.frontierOwner = binding.operation.owner;
    return true;
}

bool QueueInstanceTable::ProveFrontierInitial(const FrontierGroupBinding& binding, AudioSliceLease& lease,
    std::uint64_t audioSequence, FrontierInitialProof* out) noexcept {
    if (!binding.Valid() || !binding.queueInitial.Valid() || binding.variableSide.Valid() ||
        out == nullptr || out->Valid() || audioSequence == 0) return false;
    const auto& identity = binding.queueInitial;
    auto* row = Resolve({identity.slot, identity.generation});
    Plan03PageGroupView group{};
    if (row == nullptr || row->frontierPool == nullptr || row->frontierInitial != &binding ||
        !(row->frontierOwner == binding.operation.owner) ||
        row->world != binding.operation.world || row->rootInstance != binding.operation.owner.child ||
        row->programSemanticFingerprint != identity.programSemanticFingerprint ||
        row->mirrorVersion.load(std::memory_order_acquire) != identity.version ||
        row->initialActivationSequence.load(std::memory_order_acquire) != 0 ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        !row->frontierPool->ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease || lease.Capture().state != AudioSliceState::Ready)
        return false;
    out->row_ = row;
    out->lease_ = &lease;
    out->audioSequence_ = audioSequence;
    return true;
}

void QueueInstanceTable::ApplyFrontierInitial(const FrontierInitialProof& proof) noexcept {
    assert(proof.Valid() && proof.lease_ != nullptr);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    static_cast<Row*>(proof.row_)->initialActivationSequence.store(proof.audioSequence_, std::memory_order_release);
}

void QueueInstanceTable::FrontierInitialCompletionProof::Release() noexcept {
    if (row_ != nullptr)
        static_cast<Row*>(row_)->conditionalReplacementReserved.store(false, std::memory_order_release);
    row_ = nullptr;
    lease_ = nullptr;
    if (pool_ != nullptr && claim_.Valid())
        (void)pool_->ReleaseImmediateLaneFrontier(&claim_);
    pool_ = nullptr;
}

bool QueueInstanceTable::ProveFrontierInitialCompletion(const FrontierGroupBinding& binding,
    const Plan03PageGroupView& source, AudioSliceLease& lease,
    const ScheduledTransactionPool& pool, std::uint64_t sourceSequence,
    const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool* composites,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t completedAudioSequence, FrontierInitialCompletionProof* out) noexcept {
    if (out == nullptr || out->Valid() || sourceSequence == 0 || !binding.Valid() ||
        binding.role != FrontierGroupRole::Future || !binding.queueInitial.Valid() ||
        binding.queueConditional.Valid() || binding.variableSide.Valid() ||
        binding.operation.group.backing != Plan03GroupBacking::ImmediateLane ||
        binding.operation.group.sliceIndex != 1u || source.frontier == nullptr ||
        source.lease == nullptr || source.memberCount != 2u || source.sourceControlCount != 1u ||
        source.identity.backing != Plan03GroupBacking::ImmediateLane || source.identity.sliceIndex != 0u ||
        source.identity.decision.slot != binding.operation.group.decision.slot ||
        source.identity.decision.generation != binding.operation.group.decision.generation ||
        !pool.ClaimImmediateLaneFrontier(binding.operation.group, &out->claim_)) return false;
    out->pool_ = &pool;
    const auto fail = [&]() noexcept { out->Release(); return false; };
    const auto& initial = *source.frontier;
    const auto& identity = binding.queueInitial;
    auto* row = Resolve({identity.slot, identity.generation});
    Plan03PageGroupView actualSource{}, actualTarget{};
    if (row == nullptr || row->frontierPool != &pool || row->frontierInitial != &initial ||
        initial.role != FrontierGroupRole::EntryMember || !(initial.entry == binding.entry) ||
        !(initial.primary == binding.primary) || !(initial.operation.owner == binding.operation.owner) ||
        initial.queueInitial.slot != identity.slot || initial.queueInitial.generation != identity.generation ||
        initial.queueInitial.version != identity.version ||
        initial.queueInitial.programSemanticFingerprint != identity.programSemanticFingerprint ||
        initial.transition.slot != binding.transition.slot ||
        initial.transition.generation != binding.transition.generation ||
        initial.transition.audibleFrame != binding.transition.audibleFrame ||
        initial.operation.order.executeFrame != binding.transition.audibleFrame ||
        !(initial.operation.order < binding.operation.order) ||
        !(row->frontierOwner == binding.operation.owner) ||
        row->world != binding.operation.world || row->rootInstance != binding.operation.owner.child ||
        row->programSemanticFingerprint != identity.programSemanticFingerprint ||
        row->mirrorVersion.load(std::memory_order_acquire) != identity.version ||
        row->initialActivationSequence.load(std::memory_order_acquire) != sourceSequence ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        !pool.ResolvePlan03GroupSliceRt(source.identity, &actualSource) ||
        actualSource.frontier != source.frontier || actualSource.lease != source.lease ||
        source.lease->Capture().state != AudioSliceState::SliceApplied ||
        !pool.ResolvePlan03GroupSliceRt(binding.operation.group, &actualTarget) ||
        actualTarget.frontier != &binding || actualTarget.lease != &lease ||
        lease.Capture().state != AudioSliceState::Ready) return fail();
    if (!binding.canonicalInitialCompletion) return true;
    bool expected = false;
    if (!row->conditionalReplacementReserved.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) return fail();
    out->row_ = row;
    out->lease_ = &lease;
    const auto& conditional = binding.queueInitialCompletion;
    if (row->resetReserved.load(std::memory_order_acquire) ||
        row->resetPending.load(std::memory_order_acquire) ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        row->mirrorVersion.load(std::memory_order_acquire) != conditional.currentVersion ||
        row->pendingVersion.load(std::memory_order_acquire) != conditional.pendingVersion ||
        row->pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        !SameDecision(row->pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->pendingExpectedCurrent, conditional.current) ||
        !SameOccurrence(row->pendingSuccessor, conditional.successor) ||
        row->pendingCompletionOwner.kind != CompletionOwnerKind::SingleQueue ||
        !SameDecision(row->pendingCompletionOwner.singleDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->pendingCompletion.occurrence, conditional.current) ||
        !SameCompletionOwner(row->pendingCompletion.owner, row->pendingCompletionOwner) ||
        row->pendingCompletionIsLogicalBoundary || quantumStartFrame >= quantumEndFrame)
        return fail();
    const auto status = ProveConditionalForQuantum({conditional.slot, conditional.generation},
        {conditional.pendingVersion}, occurrences,
        composites == nullptr ? EmptyCompositeOwners() : *composites,
        quantumStartFrame, quantumEndFrame, completedAudioSequence, true, &out->completion_);
    const auto& proof = out->completion_;
    if (status != QueueConditionalProofStatus::ReadyToActivate ||
        proof.completionMode != QueueConditionalCompletionMode::InQuantum ||
        !SameDecision(proof.decision, binding.operation.group.decision) ||
        !SameOccurrence(proof.currentOccurrence, conditional.current) ||
        !SameOccurrence(proof.successorOccurrence, conditional.successor) ||
        binding.operation.order.executeFrame < quantumStartFrame ||
        binding.operation.order.executeFrame - quantumStartFrame != proof.completion.activationOffset)
        return fail();
    return true;
}

void QueueInstanceTable::ApplyFrontierInitialCompletion(const FrontierInitialCompletionProof& proof) noexcept {
    assert(proof.Valid() && proof.row_ != nullptr && proof.lease_ != nullptr);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    auto& row = *static_cast<Row*>(proof.row_);
    assert(row.conditionalReplacementReserved.load(std::memory_order_acquire));
    row.pendingActivation.store(ConditionalQueueActivation::Activated, std::memory_order_release);
}

QueueInstanceTable::FrontierConditionalProof::~FrontierConditionalProof() { Release(); }

void QueueInstanceTable::FrontierConditionalProof::Release() noexcept {
    if (row_ != nullptr)
        static_cast<Row*>(row_)->conditionalReplacementReserved.store(false, std::memory_order_release);
    row_ = nullptr;
    lease_ = nullptr;
}

bool QueueInstanceTable::BindFrontierConditional(QueueInstanceToken token,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept {
    const auto& identity = binding.queueConditional;
    auto* row = Resolve(token);
    Plan03PageGroupView group{};
    if (row == nullptr || !binding.Valid() || !identity.Valid() || binding.queueInitial.Valid() ||
        binding.variableSide.Valid() || token.slot != identity.slot || token.generation != identity.generation ||
        row->world != binding.operation.world || row->rootInstance != binding.operation.owner.child ||
        row->mirrorVersion.load(std::memory_order_acquire) != identity.currentVersion ||
        row->pendingVersion.load(std::memory_order_acquire) != identity.pendingVersion ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->conditionalReplacementReserved.load(std::memory_order_acquire) ||
        !SameDecision(row->pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->pendingExpectedCurrent, identity.current) ||
        !SameOccurrence(row->pendingSuccessor, identity.successor) ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) || group.frontier != &binding ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready ||
        (row->frontierPool != nullptr && (row->frontierPool != &pool || !(row->frontierOwner == binding.operation.owner))))
        return false;
    row->frontierPool = &pool;
    row->frontierOwner = binding.operation.owner;
    return true;
}

bool QueueInstanceTable::BindRetainedOriginalFrontierConditional(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parent,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap) ||
        !IsOriginalLifetimePinFor(parent, cap.Token())) return false;
    if (IsCurrentOriginalConditionalFutureReservation(cap))
        return BindFrontierConditional(cap.Token(), binding, pool);
    if (!IsSelectedOriginalConditionalCurrent(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    auto& row = rows_[original.queue.slot];
    const auto& identity = binding.queueConditional;
    Plan03PageGroupView group{};
    if (!binding.Valid() || !identity.Valid() || binding.queueInitial.Valid() ||
        binding.variableSide.Valid() || original.clockPool != &pool ||
        original.parent.marker_ != parent.marker_ ||
        !(binding.operation.group == original.clockGroups[1].Identity()) ||
        binding.operation.world != original.world || binding.operation.owner.child != original.rootInstance ||
        identity.slot != original.queue.slot || identity.generation != original.queue.generation ||
        identity.currentVersion != original.currentState.version || identity.pendingVersion != original.version.version ||
        !SameDecision(binding.operation.group.decision, original.decision) ||
        !SameOccurrence(identity.current, original.current) || !SameOccurrence(identity.successor, original.successor) ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) || group.frontier != &binding ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready ||
        (row.frontierPool != nullptr && (row.frontierPool != &pool || row.frontierOwner != binding.operation.owner)))
        return false;
    row.frontierPool = &pool;
    row.frontierOwner = binding.operation.owner;
    return true;
}

bool QueueInstanceTable::ProveFrontierConditional(const FrontierGroupBinding& binding, AudioSliceLease& lease,
    const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool* composites,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame, std::uint64_t completedAudioSequence,
    FrontierConditionalProof* out, FrontierConditionalSlice slice) noexcept {
    const auto& identity = binding.queueConditional;
    if (!binding.Valid() || !identity.Valid() || binding.variableSide.Valid() || binding.queueInitial.Valid() ||
        out == nullptr || out->Valid()) return false;
    auto* row = Resolve({identity.slot, identity.generation});
    Plan03PageGroupView group{};
    if (row == nullptr || row->frontierPool == nullptr || !(row->frontierOwner == binding.operation.owner) ||
        row->world != binding.operation.world || row->rootInstance != binding.operation.owner.child ||
        row->mirrorVersion.load(std::memory_order_acquire) != identity.currentVersion ||
        row->pendingVersion.load(std::memory_order_acquire) != identity.pendingVersion ||
        !SameDecision(row->pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->pendingExpectedCurrent, identity.current) ||
        !SameOccurrence(row->pendingSuccessor, identity.successor) ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        !row->frontierPool->ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease || lease.Capture().state != AudioSliceState::Ready)
        return false;
    bool expected = false;
    if (!row->conditionalReplacementReserved.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    out->row_ = row;
    out->lease_ = &lease;
    out->slice_ = slice;
    if (slice == FrontierConditionalSlice::TargetLock) {
        if (!binding.transition.Valid() || binding.operation.group.sliceIndex != 1 ||
            binding.operation.order.executeFrame != binding.transition.audibleFrame ||
            row->pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Activated) {
            out->Release();
            return false;
        }
        return true;
    }
    const auto status = ProveConditionalForQuantum({identity.slot, identity.generation}, {identity.pendingVersion},
        occurrences, composites == nullptr ? EmptyCompositeOwners() : *composites,
        quantumStartFrame, quantumEndFrame, completedAudioSequence, true, &out->completion_);
    const auto& proof = out->completion_;
    if (slice == FrontierConditionalSlice::Prelude) {
        if (!binding.transition.Valid() || binding.operation.group.sliceIndex != 0 ||
            binding.operation.order.executeFrame != binding.transition.audibleFrame ||
            (status != QueueConditionalProofStatus::Waiting && status != QueueConditionalProofStatus::ReadyToActivate) ||
            (status == QueueConditionalProofStatus::ReadyToActivate &&
                (binding.operation.order.executeFrame < quantumStartFrame ||
                 binding.operation.order.executeFrame - quantumStartFrame >= proof.completion.activationOffset))) {
            out->Release();
            return false;
        }
        return true;
    }
    if ((status != QueueConditionalProofStatus::ReadyToActivate &&
            status != QueueConditionalProofStatus::ReadyToActivateRetiredSource) ||
        !SameDecision(proof.decision, binding.operation.group.decision) ||
        !SameOccurrence(proof.currentOccurrence, identity.current) || !SameOccurrence(proof.successorOccurrence, identity.successor) ||
        (status == QueueConditionalProofStatus::ReadyToActivate &&
            (binding.operation.order.executeFrame < quantumStartFrame ||
             binding.operation.order.executeFrame - quantumStartFrame != proof.completion.activationOffset))) {
        out->Release();
        return false;
    }
    return true;
}

void QueueInstanceTable::ApplyFrontierConditional(const FrontierConditionalProof& proof) noexcept {
    assert(proof.Valid() && proof.lease_ != nullptr);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    auto& row = *static_cast<Row*>(proof.row_);
    assert(row.conditionalReplacementReserved.load(std::memory_order_acquire));
    if (proof.slice_ == FrontierConditionalSlice::Activation)
        row.pendingActivation.store(ConditionalQueueActivation::Activated, std::memory_order_release);
}

bool QueueInstanceTable::StoreMirror(
    Row& row, const EffectiveQueueState& state) noexcept {
    if (state.randomInitialRecentCount > kMaxQueueAvoidHistoryHard) {
        return false;
    }
    const std::uint64_t sequence =
        row.mirrorSequence.load(std::memory_order_relaxed);
    if ((sequence & 1u) != 0 || sequence > UINT64_MAX - 2u) return false;
    if (!MayMutateFrontier(row)) return false;
    row.mirrorSequence.store(sequence + 1u, std::memory_order_release);
    row.mirrorCursor.store(state.cursor, std::memory_order_relaxed);
    row.mirrorCurrentChildOrdinal.store(
        state.currentChildOrdinal, std::memory_order_relaxed);
    row.mirrorCycleOrdinal.store(state.cycleOrdinal,
                                 std::memory_order_relaxed);
    row.mirrorSelectionOrdinal.store(state.selectionOrdinal,
                                     std::memory_order_relaxed);
    row.mirrorRandomInitialRecentCount.store(
        state.randomInitialRecentCount, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < kMaxQueueAvoidHistoryHard; ++i) {
        row.mirrorRandomInitialRecent[i].store(
            state.randomInitialRecent[i], std::memory_order_relaxed);
    }
    row.mirrorVersion.store(state.version, std::memory_order_relaxed);
    row.mirrorSequence.store(sequence + 2u, std::memory_order_release);
    return true;
}

bool QueueInstanceTable::SameResetTarget(
    const Row& row, const QueueResetTargetV1& target) noexcept {
    return row.world == target.world &&
           row.containerStableId == target.containerStableId &&
           row.programSemanticFingerprint ==
               target.programSemanticFingerprint &&
           row.emitterSlot == target.emitterSlot &&
           row.emitterGeneration == target.emitterGeneration;
}

void QueueInstanceTable::ReleaseResetReservations(
    const QueueResetTargetV1& target,
    std::uint32_t slotEndExclusive, std::uint64_t reservation) noexcept {
    const std::uint32_t end =
        slotEndExclusive < capacity_ ? slotEndExclusive : capacity_;
    for (std::uint32_t slot = 0; slot < end; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) ==
                SlotState::Live &&
            SameResetTarget(row, target) &&
            row.resetReservation == reservation &&
            row.resetReserved.exchange(false,
                                       std::memory_order_acq_rel)) {
            row.resetReservation = 0;
        }
    }
}

bool QueueInstanceTable::Acquire(
    const QueueInstanceCreateRequest& request,
    QueueInstanceToken* outToken, OriginalLifetimePin* originalLifetime) noexcept {
    return AcquireInternal(request, outToken, false, originalLifetime);
}

bool QueueInstanceTable::PrepareAcquire(
    const QueueInstanceCreateRequest& request,
    PreparedQueueInstanceV1* out, OriginalLifetimePin* originalLifetime) noexcept {
    if (out == nullptr || out->Valid()) {
        return false;
    }
    QueueInstanceToken token{};
    if (!AcquireInternal(request, &token, true, originalLifetime)) {
        return false;
    }
    out->owner_ = this;
    out->token_ = token;
    out->prepared_ = true;
    return true;
}

bool QueueInstanceTable::AcquireInternal(
    const QueueInstanceCreateRequest& request,
    QueueInstanceToken* outToken, bool prepared, OriginalLifetimePin* originalLifetime) noexcept {
    if (outToken == nullptr ||
        (originalLifetime != nullptr &&
            (originalLifetime->owner_ != nullptr || nextOriginalLifetimeMarker_ == UINT64_MAX)) ||
        request.world == 0 ||
        request.rootInstance == 0 || request.containerStableId == VnObjectId{} ||
        request.programSemanticFingerprint == 0 || request.childCount == 0 ||
        !ValidEmitterIdentity(request.emitterSlot,
                              request.emitterGeneration) ||
        !ValidEffectiveState(request.initial, request.childCount)) {
        return false;
    }
    const EffectiveQueueState resetInitial =
        request.resetInitial.version == 0 ? request.initial
                                          : request.resetInitial;
    if (!ValidEffectiveState(resetInitial, request.childCount)) return false;
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        Row& row = rows_[i];
        SlotState expected = SlotState::Free;
        if (!row.slotState.compare_exchange_strong(
                expected, SlotState::Initializing, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        assert(row.originalLifetimePinCount == 0 && row.originalLifetimeMarker == 0);
        assert(row.originalConditionalRow == UINT32_MAX && row.originalConditionalMarker == 0);
        row.world = request.world;
        row.projectId = request.projectId;
        row.frontierPool = nullptr;
        row.frontierInitial = nullptr;
        row.frontierOwner = {};
        row.initialActivationSequence.store(0, std::memory_order_relaxed);
        row.rootInstance = request.rootInstance;
        row.containerStableId = request.containerStableId;
        row.programSemanticFingerprint = request.programSemanticFingerprint;
        row.childCount = request.childCount;
        row.cycle = request.cycle;
        row.resumePosition = request.resumePosition;
        row.resetEpoch = request.resetEpoch;
        row.emitterSlot = request.emitterSlot;
        row.emitterGeneration = request.emitterGeneration;
        row.resetInitial = resetInitial;
        row.resetNext = {};
        row.resetReserved.store(false, std::memory_order_relaxed);
        row.resetPending.store(false, std::memory_order_relaxed);
        row.resetExecuteFrame = 0;
        row.resetReservation = 0;
        row.resetIntentCounter = 0;
        row.resetIntentGeneration = 0;
        row.resetOrder = {};
        row.pendingResetIntent = 0;
        row.signalReservation = SignalReservation::None;
        row.nextStateOwner = NextStateOwner::None;
        row.signalAttemptCounter = 0;
        row.signalIdentity = {};
        row.conditionalReserved.store(false, std::memory_order_relaxed);
        row.conditionalReplacementReserved.store(
            false, std::memory_order_relaxed);
        row.pendingConsumesReset.store(false,
                                       std::memory_order_relaxed);
        row.pendingRetiredSource = {};
        if (!StoreMirror(row, request.initial)) {
            row.slotState.store(SlotState::Quarantined,
                                std::memory_order_release);
            return false;
        }
        row.pendingVersion.store(0, std::memory_order_relaxed);
        row.pendingActivation.store(ConditionalQueueActivation::Waiting,
                                    std::memory_order_relaxed);
        row.failurePhase.store(QueueFailurePhase::Healthy,
                               std::memory_order_relaxed);
        row.failureReason = QueueFailureReason::RenderProgressLost;
        row.failureFrame = 0;
        if (originalLifetime != nullptr) {
            row.originalLifetimeMarker = ++nextOriginalLifetimeMarker_;
            row.originalLifetimePinCount = 1;
            originalLifetime->owner_ = this;
            originalLifetime->token_ = {i, row.generation};
            originalLifetime->marker_ = row.originalLifetimeMarker;
        }
        *outToken = {i, row.generation};
        if (!prepared) {
            row.slotState.store(SlotState::Live,
                                std::memory_order_release);
        }
        return true;
    }
    return false;
}

bool QueueInstanceTable::CommitPreparedAcquire(
    PreparedQueueInstanceV1* prepared,
    QueueInstanceToken* outToken) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || outToken == nullptr ||
        !prepared->token_.Valid() || prepared->token_.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[prepared->token_.slot];
    if (row.generation != prepared->token_.generation ||
        row.conditionalReserved.load(std::memory_order_acquire)) {
        return false;
    }
    SlotState expected = SlotState::Initializing;
    if (!row.slotState.compare_exchange_strong(
            expected, SlotState::Live, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    *outToken = prepared->token_;
    prepared->owner_ = nullptr;
    prepared->token_ = {};
    prepared->prepared_ = false;
    return true;
}

bool QueueInstanceTable::CancelPreparedAcquire(
    PreparedQueueInstanceV1* prepared, OriginalLifetimePin* originalBaseline) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || !prepared->token_.Valid() ||
        prepared->token_.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[prepared->token_.slot];
    if ((originalBaseline != nullptr &&
            !IsOriginalLifetimePinFor(*originalBaseline, prepared->token_)) ||
        row.generation != prepared->token_.generation ||
        row.catalogResetScopeMarker != 0 ||
        row.originalLifetimePinCount != (originalBaseline != nullptr ? 1u : 0u) ||
        row.conditionalReserved.load(std::memory_order_acquire) ||
        HasFrontierReaders(row) ||
        row.pendingVersion.load(std::memory_order_acquire) != 0) {
        return false;
    }
    SlotState expected = SlotState::Initializing;
    if (!row.slotState.compare_exchange_strong(
            expected, SlotState::Free, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    ConsumeOriginalLifetimePinAfterReclaim(originalBaseline, row);
    row.world = 0;
    row.projectId = {};
    row.rootInstance = 0;
    row.containerStableId = {};
    row.programSemanticFingerprint = 0;
    row.childCount = 0;
    row.resetInitial = {};
    row.resetNext = {};
    row.mirrorSequence.store(0, std::memory_order_relaxed);
    row.mirrorVersion.store(0, std::memory_order_relaxed);
    row.mirrorCursor.store(0, std::memory_order_relaxed);
    row.mirrorCurrentChildOrdinal.store(0, std::memory_order_relaxed);
    row.mirrorCycleOrdinal.store(0, std::memory_order_relaxed);
    row.mirrorSelectionOrdinal.store(0, std::memory_order_relaxed);
    row.mirrorRandomInitialRecentCount.store(0,
                                              std::memory_order_relaxed);
    for (auto& recent : row.mirrorRandomInitialRecent) {
        recent.store(0, std::memory_order_relaxed);
    }
    prepared->owner_ = nullptr;
    prepared->token_ = {};
    prepared->prepared_ = false;
    return true;
}

bool QueueInstanceTable::Release(QueueInstanceToken token,
    OriginalLifetimePin* originalBaseline) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr ||
        (originalBaseline != nullptr && !IsOriginalLifetimePinFor(*originalBaseline, token)) ||
        row->catalogResetScopeMarker != 0 ||
        row->originalLifetimePinCount != (originalBaseline != nullptr ? 1u : 0u) ||
        HasFrontierReaders(*row) || SignalOwnsNext(*row) ||
        row->pendingVersion.load(std::memory_order_acquire) != 0 ||
        row->resetReserved.load(std::memory_order_acquire) ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->conditionalReplacementReserved.load(
            std::memory_order_acquire)) {
        return false;
    }
    SlotState expected = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    ConsumeOriginalLifetimePinAfterReclaim(originalBaseline, *row);
    if (row->generation ==
        (std::numeric_limits<std::uint32_t>::max)()) {
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        return false;
    }
    ++row->generation;
    row->pendingRetiredSource = {};
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool QueueInstanceTable::Capture(QueueInstanceToken token,
                                 QueueInstanceSnapshot* out) const noexcept {
    if (out == nullptr) return false;
    const Row* row = Resolve(token);
    return row != nullptr && CaptureRowSnapshot(*row, out);
}

bool QueueInstanceTable::CaptureRowSnapshot(
    const Row& originalRow, QueueInstanceSnapshot* out) noexcept {
    if (out == nullptr) return false;
    const Row* row = &originalRow;
    QueueInstanceSnapshot snapshot{};
    snapshot.world = row->world;
    snapshot.rootInstance = row->rootInstance;
    snapshot.containerStableId = row->containerStableId;
    snapshot.programSemanticFingerprint = row->programSemanticFingerprint;
    snapshot.initialActivationSequence = row->initialActivationSequence.load(std::memory_order_acquire);
    snapshot.childCount = row->childCount;
    snapshot.cycle = row->cycle;
    snapshot.resumePosition = row->resumePosition;
    snapshot.resetEpoch = row->resetEpoch;
    snapshot.emitterSlot = row->emitterSlot;
    snapshot.emitterGeneration = row->emitterGeneration;
    if (!LoadMirror(*row, &snapshot.logicMirror)) return false;
    snapshot.pendingActivation =
        row->pendingActivation.load(std::memory_order_acquire);
    snapshot.pendingVersion =
        row->pendingVersion.load(std::memory_order_acquire);
    snapshot.resetPending =
        row->resetPending.load(std::memory_order_acquire);
    snapshot.failurePhase =
        row->failurePhase.load(std::memory_order_acquire);
    if (snapshot.failurePhase != QueueFailurePhase::Healthy) {
        snapshot.failureReason = row->failureReason;
        snapshot.failureFrame = row->failureFrame;
    }
    *out = snapshot;
    return true;
}

bool QueueInstanceTable::ProvePendingConditionalIdentity(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    DecisionToken decision) const noexcept {
    const Row* row = Resolve(token);
    return row != nullptr && version.Valid() && decision.Valid() &&
        row->pendingVersion.load(std::memory_order_acquire) ==
            version.version &&
        SameDecision(row->pendingDecision, decision);
}

bool QueueInstanceTable::ResolveEffective(
    QueueInstanceToken token, EffectiveQueueState* out) const noexcept {
    if (out == nullptr) return false;
    const Row* row = Resolve(token);
    if (row == nullptr) return false;
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        EffectiveQueueState state{};
        if (!LoadMirror(*row, &state)) continue;
        const std::uint64_t pendingVersion =
            row->pendingVersion.load(std::memory_order_acquire);
        if (pendingVersion > state.version &&
            row->pendingActivation.load(std::memory_order_acquire) ==
                ConditionalQueueActivation::Activated) {
            *out = row->pendingNext;
            return true;
        }
        EffectiveQueueState confirmed{};
        if (LoadMirror(*row, &confirmed) &&
            confirmed.version == state.version) {
            *out = confirmed;
            return true;
        }
    }
    return false;
}

bool QueueInstanceTable::ResolveNextSelection(
    QueueInstanceToken token, EffectiveQueueState* outCurrent,
    EffectiveQueueState* outNext, bool* outReset) const noexcept {
    if (outCurrent == nullptr || outNext == nullptr || outReset == nullptr) {
        return false;
    }
    const Row* row = Resolve(token);
    if (row == nullptr || SignalOwnsNext(*row) ||
        !ResolveEffective(token, outCurrent)) return false;
    *outNext = {};
    *outReset = false;
    if (!row->resetPending.load(std::memory_order_acquire)) return true;
    if (!ProjectReset(*row, *outCurrent, outNext)) return false;
    *outReset = true;
    return true;
}

bool QueueInstanceTable::ProjectReset(
    const Row& row, const EffectiveQueueState& current,
    EffectiveQueueState* out) noexcept {
    if (out == nullptr || current.version == UINT64_MAX ||
        !ValidEffectiveState(row.resetInitial, row.childCount)) return false;
    *out = row.resetInitial;
    out->version = current.version + 1u;
    return true;
}

bool QueueInstanceTable::ResolveNextSelection(
    QueueInstanceToken token, QueueExecutionOrderV1 selectionOrder,
    EffectiveQueueState* outCurrent, EffectiveQueueState* outNext,
    std::uint64_t* outResetIntent) const noexcept {
    if (outCurrent == nullptr || outNext == nullptr || outResetIntent == nullptr ||
        selectionOrder.sequence == 0) return false;
    const Row* row = Resolve(token);
    if (row == nullptr || SignalOwnsNext(*row) ||
        !ResolveEffective(token, outCurrent)) return false;
    *outNext = {};
    *outResetIntent = 0;
    if (row->resetIntentGeneration == 0 ||
        !QueueExecutionPrecedes(row->resetOrder, selectionOrder)) return true;
    if (!ProjectReset(*row, *outCurrent, outNext)) return false;
    *outResetIntent = row->resetIntentGeneration;
    return true;
}

void QueueInstanceTable::ConsumeResetIntent(Row& row, std::uint64_t intent) noexcept {
    if (intent == 0 || row.resetIntentGeneration != intent) return;
    row.resetPending.store(false, std::memory_order_release);
    row.resetExecuteFrame = 0;
    row.resetNext = {};
    row.resetIntentGeneration = 0;
    row.resetOrder = {};
}

bool QueueInstanceTable::ResolveResetSelection(
    QueueInstanceToken token, EffectiveQueueState* outCurrent,
    EffectiveQueueState* outNext) const noexcept {
    if (outCurrent == nullptr || outNext == nullptr) {
        return false;
    }
    const Row* row = Resolve(token);
    if (row == nullptr || SignalOwnsNext(*row) ||
        row->failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        row->resetPending.load(std::memory_order_acquire) ||
        row->resetIntentGeneration != 0 ||
        row->resetReserved.load(std::memory_order_acquire) ||
        !LoadMirror(*row, outCurrent) ||
        outCurrent->version == UINT64_MAX ||
        !ValidEffectiveState(row->resetInitial, row->childCount)) {
        return false;
    }
    *outNext = row->resetInitial;
    outNext->version = outCurrent->version + 1u;
    return true;
}

QueueResetStatusV1 QueueInstanceTable::PrepareReset(
    const QueueResetTargetV1& target,
    PreparedQueueResetSetV1* out, std::uint64_t executeFrame,
    std::uint32_t executionOrdinal) noexcept {
    return PrepareResetInternal(target, nullptr, out, executeFrame, executionOrdinal);
}

QueueResetStatusV1 QueueInstanceTable::PrepareResetRemainder(
    const QueueResetTargetV1& target, const PreparedCatalogResetScope& nativeScope,
    PreparedQueueResetSetV1* out, std::uint64_t executeFrame,
    std::uint32_t executionOrdinal) noexcept {
    if (!MatchesCatalogResetScope(nativeScope) || !nativeScope.nativeOnly_)
        return QueueResetStatusV1::InvalidInput;
    return PrepareResetInternal(target, &nativeScope, out, executeFrame, executionOrdinal);
}

QueueResetStatusV1 QueueInstanceTable::PrepareResetInternal(
    const QueueResetTargetV1& target, const PreparedCatalogResetScope* nativeScope,
    PreparedQueueResetSetV1* out, std::uint64_t executeFrame,
    std::uint32_t executionOrdinal) noexcept {
    if (out == nullptr || out->Valid() || target.world == 0 ||
        target.containerStableId == VnObjectId{} ||
        target.programSemanticFingerprint == 0 ||
        !ValidEmitterIdentity(target.emitterSlot,
                              target.emitterGeneration)) {
        return QueueResetStatusV1::InvalidInput;
    }
    if (resetReservationCounter_ == UINT64_MAX)
        return QueueResetStatusV1::RuntimeRecreateRequired;
    const auto reservation = ++resetReservationCounter_;
    const auto normalizedFrame = executeFrame == 0 ? logicNow_ : executeFrame;
    std::uint32_t matched = 0;
    std::uint32_t reserved = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) !=
                SlotState::Live ||
            !SameResetTarget(row, target)) {
            continue;
        }
        if (nativeScope != nullptr && row.catalogResetScopeMarker == nativeScope->marker_ &&
            row.catalogResetScopeGeneration == row.generation) continue;
        ++matched;
        if (SignalOwnsNext(row)) {
            ReleaseResetReservations(target, slot, reservation);
            return QueueResetStatusV1::Backpressure;
        }
        if (row.resetIntentGeneration != 0) {
            if (row.resetPending.load(std::memory_order_acquire) &&
                normalizedFrame <= logicNow_) continue;
            ReleaseResetReservations(target, slot, reservation);
            return QueueResetStatusV1::Backpressure;
        }
        if (row.failurePhase.load(std::memory_order_acquire) !=
                QueueFailurePhase::Healthy ||
            row.conditionalReserved.load(std::memory_order_acquire) ||
            (row.pendingVersion.load(std::memory_order_acquire) != 0 &&
             !row.conditionalReplacementReserved.load(
                 std::memory_order_acquire))) {
            ReleaseResetReservations(target, slot, reservation);
            return QueueResetStatusV1::Backpressure;
        }
        bool expected = false;
        if (!row.resetReserved.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            ReleaseResetReservations(target, slot, reservation);
            return QueueResetStatusV1::Backpressure;
        }
        row.resetReservation = reservation;
        EffectiveQueueState current{};
        if (!LoadMirror(row, &current) || current.version == UINT64_MAX ||
            row.resetIntentCounter == UINT64_MAX ||
            !ValidEffectiveState(row.resetInitial, row.childCount)) {
            ReleaseResetReservations(target, slot + 1u, reservation);
            return QueueResetStatusV1::RuntimeRecreateRequired;
        }
        ++reserved;
    }
    out->owner_ = this;
    out->target_ = target;
    out->matchedCount_ = matched;
    out->reservedCount_ = reserved;
    out->prepared_ = true;
    out->reservation_ = reservation;
    out->order_ = {normalizedFrame, 0, executionOrdinal};
    out->nativeScopeMarker_ = nativeScope == nullptr ? 0 : nativeScope->marker_;
    return QueueResetStatusV1::Ok;
}

QueueResetStatusV1 QueueInstanceTable::CommitPreparedReset(
    PreparedQueueResetSetV1* prepared,
    LogicCommitSequence sequence,
    std::uint64_t executeFrame) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || sequence == 0 ||
        (executeFrame != UINT64_MAX && executeFrame != prepared->order_.executeFrame)) {
        return QueueResetStatusV1::InvalidInput;
    }
    std::uint32_t matchedReservations = 0;
    if (prepared->nativeScopeMarker_ != 0 &&
        prepared->nativeScopeMarker_ != liveCatalogResetScopeMarker_)
        return QueueResetStatusV1::InvalidInput;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        if (prepared->nativeScopeMarker_ != 0 &&
            row.catalogResetScopeMarker == prepared->nativeScopeMarker_ && SameResetTarget(row, prepared->target_)) {
            // A remainder can commit only after the real shared winner selected
            // every excluded native row under this exact authored Reset order.
            if (row.generation != row.catalogResetScopeGeneration ||
                row.originalConditionalRow >= originalConditionalCapacity_ ||
                !originalConditionalRows_[row.originalConditionalRow].selectedFutureConsumed ||
                row.resetIntentGeneration == 0 || row.resetOrder.sequence != sequence ||
                row.resetOrder.executeFrame != prepared->order_.executeFrame ||
                row.resetOrder.executionOrdinal != prepared->order_.executionOrdinal) return QueueResetStatusV1::InvalidInput;
        }
        if (row.resetReservation != prepared->reservation_) continue;
        EffectiveQueueState current{};
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
            !SameResetTarget(row, prepared->target_) ||
            !row.resetReserved.load(std::memory_order_acquire) ||
            row.resetIntentGeneration != 0 || row.resetIntentCounter == UINT64_MAX ||
            !LoadMirror(row, &current) || current.version == UINT64_MAX)
            return QueueResetStatusV1::RuntimeRecreateRequired;
        ++matchedReservations;
    }
    if (matchedReservations != prepared->reservedCount_)
        return QueueResetStatusV1::RuntimeRecreateRequired;
    std::uint32_t committed = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) !=
                SlotState::Live ||
            !SameResetTarget(row, prepared->target_) ||
            row.resetReservation != prepared->reservation_ ||
            !row.resetReserved.load(std::memory_order_acquire)) {
            continue;
        }
        EffectiveQueueState current{};
        (void)LoadMirror(row, &current);
        (void)ProjectReset(row, current, &row.resetNext);
        row.resetIntentGeneration = ++row.resetIntentCounter;
        row.resetOrder = prepared->order_;
        row.resetOrder.sequence = sequence;
        row.resetExecuteFrame = row.resetOrder.executeFrame;
        row.resetPending.store(row.resetExecuteFrame <= logicNow_, std::memory_order_release);
        row.resetReservation = 0;
        row.resetReserved.store(false, std::memory_order_release);
        ++committed;
    }
    if (committed != prepared->reservedCount_) {
        return QueueResetStatusV1::RuntimeRecreateRequired;
    }
    prepared->owner_ = nullptr;
    prepared->matchedCount_ = 0;
    prepared->reservedCount_ = 0;
    prepared->prepared_ = false;
    return QueueResetStatusV1::Ok;
}

void QueueInstanceTable::ApplyDue(std::uint64_t now) noexcept {
    logicNow_ = now;
    if (rows_ == nullptr) {
        return;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
            row.signalReservation != SignalReservation::None ||
            row.resetIntentGeneration == 0 || row.resetExecuteFrame > now ||
            row.resetReserved.load(std::memory_order_acquire) ||
            row.resetPending.load(std::memory_order_acquire)) {
            continue;
        }
        row.resetPending.store(true, std::memory_order_release);
    }
}

QueueResetStatusV1 QueueInstanceTable::CancelPreparedReset(
    PreparedQueueResetSetV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_) {
        return QueueResetStatusV1::NotFound;
    }
    std::uint32_t cancelled = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) !=
                SlotState::Live ||
            !SameResetTarget(row, prepared->target_) ||
            row.resetReservation != prepared->reservation_ ||
            !row.resetReserved.exchange(false,
                                        std::memory_order_acq_rel)) {
            continue;
        }
        row.resetReservation = 0;
        ++cancelled;
    }
    if (cancelled != prepared->reservedCount_) {
        return QueueResetStatusV1::RuntimeRecreateRequired;
    }
    prepared->owner_ = nullptr;
    prepared->matchedCount_ = 0;
    prepared->reservedCount_ = 0;
    prepared->prepared_ = false;
    return QueueResetStatusV1::Ok;
}

bool QueueInstanceTable::OriginalConditionalFutureReservation::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalConditionalFutureReservation(*this);
}

bool QueueInstanceTable::OriginalConditionalFutureReservation::Borrow(
    OriginalConditionalFutureReservation* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalConditionalFutureReservation(*this, out);
}

void QueueInstanceTable::OriginalConditionalFutureReservation::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseOriginalConditionalFutureReservation(*this);
    owner_ = nullptr;
    row_ = UINT32_MAX;
    generation_ = 0;
    marker_ = 0;
    queue_ = {};
    version_ = {};
}

bool QueueInstanceTable::OriginalConditionalLaneClock::Valid() const noexcept {
    return original_.owner_ != nullptr && original_.owner_->MatchesOriginalConditionalLaneClock(*this);
}

bool QueueInstanceTable::OriginalConditionalLaneClock::Borrow(
    OriginalConditionalLaneClock* out) const noexcept {
    return original_.owner_ != nullptr &&
        original_.owner_->BorrowOriginalConditionalLaneClock(*this, out);
}

bool QueueInstanceTable::OriginalConditionalLaneClock::CaptureState(
    OriginalConditionalLaneClockState* out) const noexcept {
    if (out == nullptr || !Valid()) return false;
    const auto& native = original_.owner_->originalConditionalRows_[original_.row_];
    *out = native.state == OriginalConditionalState::Retired
        ? OriginalConditionalLaneClockState::Retired
        : native.clockState == OriginalConditionalClockState::Bound
            ? OriginalConditionalLaneClockState::Bound : OriginalConditionalLaneClockState::Unbound;
    return true;
}

bool QueueInstanceTable::OriginalConditionalLaneClock::CaptureInitialBinding(
    OriginalConditionalInitialClockBinding* out) const noexcept {
    if (out == nullptr || !Valid()) return false;
    const auto& native = original_.owner_->originalConditionalRows_[original_.row_];
    if ((native.clockState != OriginalConditionalClockState::Bound &&
         native.clockState != OriginalConditionalClockState::Retired) ||
        native.clockBinding.installedAckSequence == 0) return false;
    *out = native.clockBinding;
    return true;
}

bool QueueInstanceTable::BindPrivateInitialOriginalConditionalClock(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parentOriginal,
    const OriginalConditionalLaneClock& clock, ScheduledTransactionPool& pool,
    const PrivateOriginalQueueGroupClockProof& privateClock,
    const TimelineArmTable& arms, const DecisionTable& decisions, TimelineArmToken sourceArm,
    PlayableInstanceHandle sourceRoot, std::uint64_t sourceRootEpoch,
    OccurrenceToken sourceCurrent, const CompletionOwnerRef& sourceOwner,
    std::uint64_t installedAckSequence) noexcept {
    if (installedAckSequence == 0 || sourceRoot == 0 || sourceRootEpoch == 0 ||
        !sourceArm.Valid() || !IsOriginalConditionalLaneClockFor(clock, cap, parentOriginal) ||
        (!IsCurrentOriginalConditionalFutureReservation(cap) &&
         !IsSelectedOriginalConditionalCurrent(cap))) return false;
    auto& original = originalConditionalRows_[cap.row_];
    auto& row = rows_[original.queue.slot];
    if (original.clockState != OriginalConditionalClockState::Unbound || original.clockPool != &pool ||
        original.rootInstance != sourceRoot || row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.nextStateOwner != NextStateOwner::RealConditional || row.frontierPool != &pool ||
        row.frontierInitial == nullptr || row.initialActivationSequence.load(std::memory_order_acquire) != 0 ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row.resetReserved.load(std::memory_order_acquire) ||
        original.logicalBoundary || original.completionOwner.kind != CompletionOwnerKind::SingleQueue ||
        !SameCompletionOwner(original.completionOwner, sourceOwner) ||
        !SameDecision(sourceOwner.singleDecision, original.decision) ||
        !SameOccurrence(original.current, sourceCurrent) ||
        original.completion.renderGeneration != sourceArm.renderGeneration ||
        !IsBoundKnownCompletionPredicate(original.completion) ||
        !SameCompletionOwner(original.completion.owner, sourceOwner)) return false;
    std::array<std::uint64_t, 4> frames{};
    const FrontierGroupBinding* initial = nullptr;
    const FrontierGroupBinding* conditional = nullptr;
    if (!pool.CapturePrivateOriginalQueueClockInitialSource(privateClock, original.clockGroups[0],
            original.clockGroups[1], arms, decisions, sourceArm, &frames, &initial, &conditional) ||
        initial != row.frontierInitial || !(initial->operation.owner == row.frontierOwner) ||
        initial->operation.owner.child != sourceRoot || initial->operation.owner.childEpoch != sourceRootEpoch ||
        initial->operation.world != original.world ||
        initial->queueInitial.slot != original.queue.slot ||
        initial->queueInitial.generation != original.queue.generation ||
        initial->queueInitial.version != original.currentState.version ||
        initial->queueInitial.programSemanticFingerprint != original.programSemanticFingerprint ||
        conditional->queueConditional.slot != original.queue.slot ||
        conditional->queueConditional.generation != original.queue.generation ||
        conditional->queueConditional.currentVersion != original.currentState.version ||
        conditional->queueConditional.pendingVersion != original.version.version ||
        !SameDecision(conditional->operation.group.decision, original.decision) ||
        !SameOccurrence(conditional->queueConditional.current, original.current) ||
        !SameOccurrence(conditional->queueConditional.successor, original.successor) ||
        original.clockRelativeFrames[0] > UINT64_MAX - frames[0] ||
        original.clockRelativeFrames[1] > UINT64_MAX - frames[0] ||
        frames[1] != frames[0] + original.clockRelativeFrames[0] ||
        frames[2] != frames[0] + original.clockRelativeFrames[1]) return false;
    DecisionParticipantRef queueParticipant{};
    queueParticipant.kind = DecisionParticipantKind::QueueInstance;
    queueParticipant.slot = original.queue.slot;
    queueParticipant.generation = original.queue.generation;
    if (!decisions.IsFrontierDecisionBindingFor(initial->operation.group.decision, *initial, pool) ||
        !decisions.IsFrontierDecisionBindingFor(conditional->operation.group.decision, *conditional, pool) ||
        !decisions.HasExclusiveParticipant(initial->operation.group.decision, queueParticipant) ||
        !decisions.HasExclusiveParticipant(conditional->operation.group.decision, queueParticipant)) return false;
    KnownCompletionPredicateV1 rebound{};
    if (!ReanchorKnownCompletionBeforePublication(original.completion, frames[3], &rebound) ||
        (!original.logicalFloor && rebound.absoluteLogicalCompleteFrame != frames[2]) ||
        (original.logicalFloor && rebound.absoluteLogicalCompleteFrame > frames[2])) return false;
    TimelineArmObservation latest{};
    if (!arms.Observe(sourceArm, &latest) || latest.armedCompletionSequence != installedAckSequence ||
        latest.startedBlockSequence != 0 || latest.logicalCompletionSequence != 0 ||
        latest.tombstoneAudioSequence != 0 ||
        (latest.state != ArmState::Armed && latest.state != ArmState::Published) ||
        (!IsCurrentOriginalConditionalFutureReservation(cap) &&
         !IsSelectedOriginalConditionalCurrent(cap))) return false;
    // Every fallible native check precedes these private producer-only writes.
    // Bound records geometry; final admission must recheck live health and Source.
    if (!original.selectedFutureConsumed) {
        row.pendingCompletion = rebound;
        row.pendingLogicalCompletionFrame = original.logicalFloor ? frames[2] : 0;
    }
    original.clockBinding = {frames[0], frames[1], frames[2], frames[3], installedAckSequence, sourceArm};
    original.clockState = OriginalConditionalClockState::Bound;
    return true;
}

bool QueueInstanceTable::MatchesOriginalConditionalLaneClock(
    const OriginalConditionalLaneClock& clock) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(clock.original_)) return false;
    const auto& original = originalConditionalRows_[clock.original_.row_];
    return original.clockState != OriginalConditionalClockState::Absent && original.clockPool != nullptr &&
        original.clockGroups[0].Valid() && original.clockGroups[1].Valid();
}

bool QueueInstanceTable::BorrowOriginalConditionalLaneClock(
    const OriginalConditionalLaneClock& clock, OriginalConditionalLaneClock* out) noexcept {
    if (out == nullptr || out->original_.owner_ != nullptr ||
        !MatchesOriginalConditionalLaneClock(clock)) return false;
    return clock.original_.Borrow(&out->original_);
}

bool QueueInstanceTable::IsOriginalConditionalLaneClockFor(
    const OriginalConditionalLaneClock& clock,
    const OriginalConditionalFutureReservation& cap,
    const OriginalLifetimePin& parentOriginal) const noexcept {
    if (!MatchesOriginalConditionalLaneClock(clock) ||
        !MatchesOriginalConditionalFutureReservation(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    return clock.original_.row_ == cap.row_ && clock.original_.generation_ == cap.generation_ &&
        clock.original_.marker_ == cap.marker_ &&
        IsOriginalConditionalFutureReservationFor(cap, parentOriginal, original.queue,
            original.version, original.decision, original.current);
}

bool QueueInstanceTable::PrepareOriginalConditionalLaneClock(
    const OriginalConditionalFutureReservation& cap,
    const OriginalLifetimePin& parentOriginal, ScheduledTransactionPool& pool,
    const OriginalQueueFutureGroupRegistration& originalC0,
    const OriginalQueueFutureGroupRegistration& originalC1,
    OriginalConditionalLaneClock* out) noexcept {
    if (out == nullptr || out->original_.owner_ != nullptr ||
        !MatchesOriginalConditionalFutureReservation(cap)) return false;
    auto& original = originalConditionalRows_[cap.row_];
    if (original.clockState != OriginalConditionalClockState::Absent ||
        (original.state != OriginalConditionalState::Prepared &&
         original.state != OriginalConditionalState::Committed) ||
        !IsOriginalConditionalFutureReservationFor(cap, parentOriginal, original.queue,
            original.version, original.decision, original.current) ||
        !pool.IsOriginalQueueFutureGroupFamily(originalC0, originalC1) ||
        !SameDecision(originalC1.Identity().decision, original.decision)) return false;
    const auto& row = rows_[original.queue.slot];
    EffectiveQueueState current{};
    if (row.originalConditionalRow != cap.row_ || row.originalConditionalMarker != cap.marker_ ||
        row.generation != original.queue.generation || row.world != original.world ||
        row.rootInstance != original.rootInstance ||
        row.programSemanticFingerprint != original.programSemanticFingerprint ||
        row.failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        row.pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        !SameDecision(row.pendingDecision, original.decision) ||
        !SameOccurrence(row.pendingExpectedCurrent, original.current) ||
        !SameOccurrence(row.pendingSuccessor, original.successor) ||
        !SameCompletionOwner(row.pendingCompletionOwner, original.completionOwner) ||
        !LoadMirror(row, &current) || !SameEffectiveState(current, original.currentState)) return false;
    if (original.state == OriginalConditionalState::Prepared) {
        if (!row.conditionalReserved.load(std::memory_order_acquire) ||
            row.pendingVersion.load(std::memory_order_acquire) != 0 ||
            row.pendingNext.version != original.version.version) return false;
    } else if (!IsCurrentOriginalConditionalFutureReservation(cap)) return false;
    std::array<std::uint64_t, 2> relative{};
    std::array<OriginalQueueFutureGroupLifetimePin, 2> groups{};
    OriginalConditionalFutureReservation output;
    if (!pool.CaptureOriginalQueueGroupRelativeFrames(originalC0, originalC1, &relative) ||
        !originalC0.BorrowLifetime(&groups[0]) || !originalC1.BorrowLifetime(&groups[1]) ||
        !cap.Borrow(&output)) return false;
    // All real issuers succeeded. No absolute firstS/Bound/Ready is constructed.
    original.clockPool = &pool;
    original.clockGroups = std::move(groups);
    original.clockRelativeFrames = relative;
    original.clockState = OriginalConditionalClockState::Unbound;
    out->original_ = std::move(output);
    return true;
}

bool QueueInstanceTable::MatchesOriginalConditionalFutureReservation(
    const OriginalConditionalFutureReservation& cap) const noexcept {
    if (cap.owner_ != this || cap.marker_ == 0 || cap.generation_ == 0 ||
        cap.row_ >= originalConditionalCapacity_) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    return original.state != OriginalConditionalState::Free &&
        original.state != OriginalConditionalState::Quarantined &&
        original.generation == cap.generation_ && original.marker == cap.marker_ &&
        original.loans != 0 && original.queue.slot == cap.queue_.slot &&
        original.queue.generation == cap.queue_.generation &&
        original.version.version == cap.version_.version &&
        IsOriginalLifetimePinFor(original.parent, original.queue);
}

bool QueueInstanceTable::CapturePreparedOriginalConditionalFamily(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parent,
    const PreparedQueueConditionalV1& prepared,
    FrontierQueueInitialIdentity* outInitial, FrontierQueueConditionalIdentity* outConditional,
    WorldGeneration* outWorld, PlayableInstanceHandle* outRoot) const noexcept {
    if (outInitial == nullptr || outConditional == nullptr || outWorld == nullptr || outRoot == nullptr ||
        !MatchesOriginalConditionalFutureReservation(cap) ||
        !IsOriginalLifetimePinFor(parent, cap.queue_) || !IsAssociatedOriginalConditional(cap) ||
        prepared.owner_ != this || !prepared.Valid() || prepared.originalRow_ != cap.row_ ||
        prepared.originalMarker_ != cap.marker_) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    if (original.state != OriginalConditionalState::Prepared || original.parent.marker_ != parent.marker_ ||
        !row.conditionalReserved.load(std::memory_order_acquire) ||
        row.pendingVersion.load(std::memory_order_acquire) != 0 ||
        !MatchesPreparedOriginalConditional(prepared, row)) return false;
    const FrontierQueueInitialIdentity initial{original.queue.slot, original.queue.generation,
        original.currentState.version, original.programSemanticFingerprint};
    const FrontierQueueConditionalIdentity conditional{original.queue.slot, original.queue.generation,
        original.currentState.version, original.version.version, original.current, original.successor};
    if (!initial.Valid() || !conditional.Valid() || original.world == 0 || original.rootInstance == 0) return false;
    *outInitial = initial; *outConditional = conditional;
    *outWorld = original.world; *outRoot = original.rootInstance;
    return true;
}

bool QueueInstanceTable::CapturePreparedOriginalConditionalCurrent(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parent,
    const PreparedQueueConditionalV1& prepared, KnownCompletionPredicateV1* predicate,
    CompletionOwnerRef* owner) const noexcept {
    if (predicate == nullptr || owner == nullptr) return false;
    FrontierQueueInitialIdentity initial{};
    FrontierQueueConditionalIdentity conditional{};
    WorldGeneration world = 0;
    PlayableInstanceHandle root = 0;
    if (!CapturePreparedOriginalConditionalFamily(cap, parent, prepared, &initial, &conditional, &world, &root)) return false;
    const auto& native = originalConditionalRows_[cap.row_];
    if (native.logicalBoundary || native.completionOwner.kind != CompletionOwnerKind::SingleQueue ||
        !IsBoundKnownCompletionPredicate(native.completion) ||
        !SameCompletionOwner(native.completion.owner, native.completionOwner) ||
        !SameDecision(native.completionOwner.singleDecision, native.decision) ||
        !SameOccurrence(native.completion.occurrence, native.current)) return false;
    *predicate = native.completion;
    *owner = native.completionOwner;
    return true;
}

bool QueueInstanceTable::IsOriginalConditionalFutureReservationFor(
    const OriginalConditionalFutureReservation& cap,
    const OriginalLifetimePin& parentOriginal, QueueInstanceToken queue,
    ConditionalQueueVersionToken version, DecisionToken decision,
    OccurrenceToken current) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap) ||
        !IsOriginalLifetimePinFor(parentOriginal, queue)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    return original.parent.marker_ == parentOriginal.marker_ &&
        original.queue.slot == queue.slot && original.queue.generation == queue.generation &&
        original.version.version == version.version &&
        SameDecision(original.decision, decision) && SameOccurrence(original.current, current);
}

bool QueueInstanceTable::IsCurrentOriginalConditionalFutureReservation(
    const OriginalConditionalFutureReservation& cap) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    EffectiveQueueState current{};
    return original.state == OriginalConditionalState::Committed &&
        row.originalConditionalRow == cap.row_ && row.originalConditionalMarker == cap.marker_ &&
        row.generation == original.queue.generation && row.world == original.world &&
        row.rootInstance == original.rootInstance &&
        row.programSemanticFingerprint == original.programSemanticFingerprint &&
        row.pendingVersion.load(std::memory_order_acquire) == original.version.version &&
        row.pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Waiting &&
        row.failurePhase.load(std::memory_order_acquire) == QueueFailurePhase::Healthy &&
        !row.conditionalReserved.load(std::memory_order_acquire) &&
        SameDecision(row.pendingDecision, original.decision) &&
        SameOccurrence(row.pendingExpectedCurrent, original.current) &&
        SameOccurrence(row.pendingSuccessor, original.successor) &&
        SameCompletionOwner(row.pendingCompletionOwner, original.completionOwner) &&
        LoadMirror(row, &current) && SameEffectiveState(current, original.currentState);
}

bool QueueInstanceTable::IsSelectedOriginalConditionalCurrent(
    const OriginalConditionalFutureReservation& cap) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    EffectiveQueueState current{};
    return original.state == OriginalConditionalState::Committed && original.selectedFutureConsumed &&
        original.selectedFutureOwner != nullptr && original.selectedFutureMarker != 0 &&
        row.slotState.load(std::memory_order_acquire) == SlotState::Live &&
        row.originalConditionalRow == cap.row_ && row.originalConditionalMarker == cap.marker_ &&
        row.generation == original.queue.generation && row.world == original.world &&
        row.rootInstance == original.rootInstance && row.programSemanticFingerprint == original.programSemanticFingerprint &&
        row.pendingVersion.load(std::memory_order_acquire) == original.selectedVersion.version &&
        row.pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Waiting &&
        row.failurePhase.load(std::memory_order_acquire) == QueueFailurePhase::Healthy &&
        !row.conditionalReserved.load(std::memory_order_acquire) &&
        SameDecision(row.pendingDecision, original.selectedDecision) &&
        SameOccurrence(row.pendingExpectedCurrent, original.current) &&
        SameOccurrence(row.pendingSuccessor, original.selectedSuccessor) &&
        SameCompletionOwner(row.pendingCompletionOwner, original.completionOwner) &&
        LoadMirror(row, &current) && SameEffectiveState(current, original.currentState);
}

bool QueueInstanceTable::BorrowOriginalConditionalFutureReservation(
    const OriginalConditionalFutureReservation& cap,
    OriginalConditionalFutureReservation* out) noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap) || out == nullptr ||
        out->owner_ != nullptr) return false;
    auto& original = originalConditionalRows_[cap.row_];
    if (original.loans == UINT32_MAX) return false;
    ++original.loans;
    out->owner_ = this;
    out->row_ = cap.row_;
    out->generation_ = cap.generation_;
    out->marker_ = cap.marker_;
    out->queue_ = cap.queue_;
    out->version_ = cap.version_;
    return true;
}

void QueueInstanceTable::ReleaseOriginalConditionalFutureReservation(
    OriginalConditionalFutureReservation& cap) noexcept {
    const bool matches = MatchesOriginalConditionalFutureReservation(cap);
    assert(matches);
    if (!matches) return;
    auto& original = originalConditionalRows_[cap.row_];
    --original.loans;
    ReclaimOriginalConditionalIfUnowned(original);
}

bool QueueInstanceTable::MatchesPreparedOriginalConditional(
    const PreparedQueueConditionalV1& prepared, const Row& row) const noexcept {
    if (prepared.originalRow_ == UINT32_MAX)
        return prepared.originalMarker_ == 0 && row.originalConditionalRow == UINT32_MAX &&
            row.originalConditionalMarker == 0;
    if (prepared.originalRow_ >= originalConditionalCapacity_ ||
        prepared.originalMarker_ == 0 || row.originalConditionalRow != prepared.originalRow_ ||
        row.originalConditionalMarker != prepared.originalMarker_) return false;
    const auto& original = originalConditionalRows_[prepared.originalRow_];
    return original.state == OriginalConditionalState::Prepared &&
        original.marker == prepared.originalMarker_ &&
        original.queue.slot == prepared.queue_.slot &&
        original.queue.generation == prepared.queue_.generation &&
        original.version.version == prepared.version_.version &&
        original.world == row.world && original.rootInstance == row.rootInstance &&
        original.programSemanticFingerprint == row.programSemanticFingerprint &&
        SameDecision(original.decision, row.pendingDecision) &&
        SameOccurrence(original.current, row.pendingExpectedCurrent) &&
        SameOccurrence(original.successor, row.pendingSuccessor) &&
        SameCompletionOwner(original.completionOwner, row.pendingCompletionOwner) &&
        SameEffectiveState(original.next, row.pendingNext) &&
        SameEffectiveState(original.first, row.pendingFirst) &&
        original.stepCount == row.pendingStepCount &&
        original.logicalBoundary == row.pendingCompletionIsLogicalBoundary &&
        original.logicalFloor == row.pendingCompletionHasLogicalFloor &&
        original.logicalCompletionFrame == row.pendingLogicalCompletionFrame &&
        original.resetIntent == row.pendingResetIntent &&
        original.consumesReset == row.pendingConsumesReset.load(std::memory_order_acquire) &&
        IsOriginalLifetimePinFor(original.parent, original.queue);
}

bool QueueInstanceTable::RegisterOriginalConditionalOwner(void* context,
    OriginalConditionalBaselineResolver resolver,
    OriginalConditionalAssociationEnding associationEnding) noexcept {
    if (context == nullptr || resolver == nullptr || associationEnding == nullptr ||
        originalConditionalOwnerContext_ != nullptr) return false;
    for (std::uint32_t index = 0; index < originalConditionalCapacity_; ++index)
        if (originalConditionalRows_[index].state != OriginalConditionalState::Free ||
            originalConditionalRows_[index].loans != 0) return false;
    originalConditionalOwnerContext_ = context;
    originalConditionalBaselineResolver_ = resolver;
    originalConditionalAssociationEnding_ = associationEnding;
    return true;
}

bool QueueInstanceTable::OriginalConditionalOwnerEmpty(
    const OriginalConditionalFutureReservation& baseline,
    const OriginalConditionalLaneClock& clock) const noexcept {
    // Invalid-but-not-returned caps must not be overwritten.
    return baseline.owner_ == nullptr && clock.original_.owner_ == nullptr;
}

bool QueueInstanceTable::IsOriginalConditionalClockEmpty(
    const OriginalConditionalLaneClock& clock) const noexcept {
    return clock.original_.owner_ == nullptr;
}

bool QueueInstanceTable::IsEndedOriginalConditional(
    const OriginalConditionalFutureReservation& cap) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    return original.state == OriginalConditionalState::Retired &&
        row.generation == original.queue.generation &&
        (row.originalConditionalRow != cap.row_ || row.originalConditionalMarker != cap.marker_);
}

bool QueueInstanceTable::IsAssociatedOriginalConditional(
    const OriginalConditionalFutureReservation& cap) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    const auto state = row.slotState.load(std::memory_order_acquire);
    return (state == SlotState::Initializing || state == SlotState::Live) &&
        (original.state == OriginalConditionalState::Prepared ||
         original.state == OriginalConditionalState::Committed) &&
        row.generation == original.queue.generation &&
        row.originalConditionalRow == cap.row_ && row.originalConditionalMarker == cap.marker_;
}

void QueueInstanceTable::EndOriginalConditionalFuture(Row& row) noexcept {
    if (row.originalConditionalRow == UINT32_MAX) {
        assert(row.originalConditionalMarker == 0);
        return;
    }
    assert(row.originalConditionalRow < originalConditionalCapacity_);
    const auto originalIndex = row.originalConditionalRow;
    auto& original = originalConditionalRows_[originalIndex];
    assert(original.marker == row.originalConditionalMarker &&
        (original.state == OriginalConditionalState::Prepared ||
         original.state == OriginalConditionalState::Committed) &&
        original.queue.slot < capacity_ && &rows_[original.queue.slot] == &row &&
        original.queue.generation == row.generation && original.parent.Valid());
    original.state = OriginalConditionalState::Retired;
    if (original.clockState == OriginalConditionalClockState::Unbound ||
        original.clockState == OriginalConditionalClockState::Bound)
        original.clockState = OriginalConditionalClockState::Retired;
    row.originalConditionalRow = UINT32_MAX;
    row.originalConditionalMarker = 0;
    if (originalConditionalBaselineResolver_ != nullptr) {
        auto* baseline = originalConditionalBaselineResolver_(
            originalConditionalOwnerContext_, *this, original.queue);
        // A same-valued token is not authority. Authenticate the actual record
        // address, full generation, issuer marker and acquired-parent family.
        if (baseline != nullptr && MatchesOriginalConditionalFutureReservation(*baseline) &&
            baseline->row_ == originalIndex && baseline->generation_ == original.generation &&
            baseline->marker_ == original.marker &&
            baseline->queue_.slot == original.queue.slot &&
            baseline->queue_.generation == original.queue.generation &&
            baseline->version_.version == original.version.version)
            originalConditionalAssociationEnding_(originalConditionalOwnerContext_, *this, *baseline);
    }
    // The callback may have returned the last two native loans and reclaimed
    // this record. The native state check makes this second attempt idempotent.
    ReclaimOriginalConditionalIfUnowned(original);
}

void QueueInstanceTable::EndOriginalConditionalAssociationsAfterQuiescence() noexcept {
    // Only owning Engine teardown enters after actual callback quiescence. This
    // returns genuine retained native associations, never foreign loan counts.
    for (std::uint32_t slot = 0; slot < capacity_; ++slot)
        if (rows_[slot].originalConditionalRow != UINT32_MAX)
            EndOriginalConditionalFuture(rows_[slot]);
}

void QueueInstanceTable::EndOriginalConditionalsForQuiescentOwnerTeardown() noexcept {
    EndOriginalConditionalAssociationsAfterQuiescence();
    for (std::uint32_t index = 0; index < originalConditionalCapacity_; ++index) {
        auto& original = originalConditionalRows_[index];
        assert(original.loans == 0);
        ReclaimOriginalConditionalIfUnowned(original);
        assert(original.clockPool == nullptr);
    }
}

void QueueInstanceTable::ReclaimOriginalConditionalIfUnowned(
    OriginalConditionalRow& original) noexcept {
    if (original.state != OriginalConditionalState::Retired || original.loans != 0) return;
    for (auto& group : original.clockGroups) group.Release();
    original.clockPool = nullptr;
    original.clockRelativeFrames = {};
    original.clockBinding = {};
    original.clockState = OriginalConditionalClockState::Absent;
    original.parent.Release();
    original.marker = 0;
    original.queue = {};
    original.version = {};
    original.decision = {};
    original.current = {};
    original.successor = {};
    assert(original.selectedFutureOwner == nullptr && original.selectedFutureMarker == 0);
    original.selectedFutureConsumed = false;
    original.selectedDecision = {};
    original.selectedSuccessor = {};
    original.selectedVersion = {};
    if (original.generation == UINT32_MAX) original.state = OriginalConditionalState::Quarantined;
    else {
        ++original.generation;
        original.state = OriginalConditionalState::Free;
    }
}

bool QueueInstanceTable::PrepareConditional(
    QueueInstanceToken token,
    const ConditionalQueuePrepareRequest& request,
    ConditionalQueueVersionToken* outVersion) noexcept {
    if (outVersion == nullptr) {
        return false;
    }
    PreparedQueueConditionalV1 prepared{};
    return PrepareConditional(token, request, &prepared) &&
           CommitPreparedConditional(&prepared, outVersion);
}

bool QueueInstanceTable::PrepareConditional(
    QueueInstanceToken token, const ConditionalQueuePrepareRequest& request,
    PreparedQueueConditionalV1* out) noexcept {
    return PrepareConditionalInternal(token, request, out, nullptr, nullptr);
}

bool QueueInstanceTable::PrepareConditional(
    QueueInstanceToken token, const ConditionalQueuePrepareRequest& request,
    PreparedQueueConditionalV1* out, const OriginalLifetimePin& parentOriginal,
    OriginalConditionalFutureReservation* outOriginal) noexcept {
    if (outOriginal == nullptr) return false;
    return PrepareConditionalInternal(token, request, out, &parentOriginal, outOriginal);
}

bool QueueInstanceTable::PrepareConditionalInternal(
    QueueInstanceToken token, const ConditionalQueuePrepareRequest& request,
    PreparedQueueConditionalV1* out, const OriginalLifetimePin* parentOriginal,
    OriginalConditionalFutureReservation* outOriginal) noexcept {
    std::uint32_t originalIndex = UINT32_MAX;
    OriginalLifetimePin parentLoan;
    if ((parentOriginal == nullptr) != (outOriginal == nullptr)) return false;
    if (outOriginal != nullptr) {
        if (outOriginal->owner_ != nullptr ||
            nextOriginalConditionalFactoryMarker_ == UINT64_MAX ||
            !IsOriginalLifetimePinFor(*parentOriginal, token)) return false;
        for (std::uint32_t index = 0; index < originalConditionalCapacity_; ++index)
            if (originalConditionalRows_[index].state == OriginalConditionalState::Free) {
                originalIndex = index;
                break;
            }
        if (originalIndex == UINT32_MAX || !parentOriginal->Borrow(&parentLoan)) return false;
    }
    const CompletionOwnerRef owner =
        !request.completionIsLogicalBoundary &&
                request.completionOwner.kind == CompletionOwnerKind::None
            ? request.completion.owner
            : request.completionOwner;
    const bool validSingle =
        !request.completionIsLogicalBoundary &&
        owner.kind == CompletionOwnerKind::SingleQueue &&
        IsBoundKnownCompletionPredicate(request.completion) &&
        SameOccurrence(request.completion.occurrence,
                       request.expectedCurrentOccurrence) &&
        SameCompletionOwner(owner, request.completion.owner) &&
        SameDecision(owner.singleDecision, request.rootDecision);
    const bool validComposite =
        !request.completionIsLogicalBoundary &&
        owner.kind == CompletionOwnerKind::Composite &&
        owner.composite.Valid();
    const bool validLogicalBoundary =
        request.completionIsLogicalBoundary &&
        !request.completionHasLogicalFloor &&
        owner.kind == CompletionOwnerKind::None &&
        request.completion.owner.kind == CompletionOwnerKind::None;
    const bool validRetiredSource =
        ValidRetiredSourceProof(request.retiredSource);
    const bool validRetiredSignalSource =
        validRetiredSource && !request.completionIsLogicalBoundary &&
        !request.completionHasLogicalFloor &&
        owner.kind == CompletionOwnerKind::None &&
        request.completion.owner.kind == CompletionOwnerKind::None;
    const bool noRetiredSource =
        EmptyRetiredSourceProof(request.retiredSource);
    const bool validLogicalFloor =
        !request.completionHasLogicalFloor ||
        (!request.completionIsLogicalBoundary &&
         (validSingle || validComposite));
    const bool validLogicalFrame =
        request.completionIsLogicalBoundary ||
        request.completionHasLogicalFloor ||
        request.logicalCompletionFrame == 0;
    if (out == nullptr || out->Valid() || !request.rootDecision.Valid() ||
        !ValidOccurrence(request.expectedCurrentOccurrence) ||
        !ValidOccurrence(request.successorOccurrence) ||
        (!validRetiredSource && !noRetiredSource) ||
        (validLogicalBoundary && !noRetiredSource) ||
        !validLogicalFloor || !validLogicalFrame ||
        (static_cast<std::uint32_t>(validSingle) +
             static_cast<std::uint32_t>(validComposite) +
             static_cast<std::uint32_t>(validLogicalBoundary) +
             static_cast<std::uint32_t>(validRetiredSignalSource) !=
         1u)) {
        return false;
    }
    if (!token.Valid() || token.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[token.slot];
    const SlotState slotState =
        row.slotState.load(std::memory_order_acquire);
    if (row.generation != token.generation ||
        SignalOwnsNext(row) ||
        (slotState != SlotState::Initializing &&
         slotState != SlotState::Live) ||
        row.failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        row.pendingVersion.load(std::memory_order_acquire) != 0 ||
        row.originalConditionalRow != UINT32_MAX || row.originalConditionalMarker != 0 ||
        row.resetReserved.load(std::memory_order_acquire)) {
        return false;
    }
    EffectiveQueueState current{};
    if (!LoadMirror(row, &current)) return false;
    EffectiveQueueState first{};
    std::uint32_t stepCount = 0;
    if (!ValidateProjection(request, current, row.childCount, &first, &stepCount)) {
        return false;
    }
    const bool consumesReset =
        row.resetPending.load(std::memory_order_acquire);
    EffectiveQueueState resetProjection{};
    if (consumesReset &&
        (!ProjectReset(row, current, &resetProjection) ||
         !SameEffectiveState(first, resetProjection))) {
        return false;
    }
    bool expected = false;
    if (!row.conditionalReserved.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    row.pendingDecision = request.rootDecision;
    row.pendingExpectedCurrent = request.expectedCurrentOccurrence;
    row.pendingSuccessor = request.successorOccurrence;
    row.pendingCompletion = request.completion;
    row.pendingCompletionOwner = owner;
    row.pendingCompletionIsLogicalBoundary =
        request.completionIsLogicalBoundary;
    row.pendingCompletionHasLogicalFloor =
        request.completionHasLogicalFloor;
    row.pendingLogicalCompletionFrame = request.logicalCompletionFrame;
    row.pendingRetiredSource = request.retiredSource;
    row.pendingNext = request.next;
    row.nextStateOwner = NextStateOwner::RealConditional;
    row.pendingFirst = first;
    row.pendingStepCount = stepCount;
    row.pendingActivation.store(ConditionalQueueActivation::Waiting,
                                std::memory_order_relaxed);
    row.pendingConsumesReset.store(consumesReset,
                                   std::memory_order_relaxed);
    row.pendingResetIntent = consumesReset ? row.resetIntentGeneration : 0;
    if (outOriginal != nullptr) {
        auto& original = originalConditionalRows_[originalIndex];
        assert(original.state == OriginalConditionalState::Free &&
            original.loans == 0 && !original.parent.Valid() &&
            original.clockState == OriginalConditionalClockState::Absent &&
            original.clockPool == nullptr && !original.clockGroups[0].Valid() &&
            !original.clockGroups[1].Valid());
        original.parent = std::move(parentLoan);
        original.queue = token;
        original.version = {request.next.version};
        original.world = row.world;
        original.rootInstance = row.rootInstance;
        original.programSemanticFingerprint = row.programSemanticFingerprint;
        original.decision = row.pendingDecision;
        original.current = row.pendingExpectedCurrent;
        original.successor = row.pendingSuccessor;
        original.completion = row.pendingCompletion;
        original.completionOwner = row.pendingCompletionOwner;
        original.logicalBoundary = row.pendingCompletionIsLogicalBoundary;
        original.logicalFloor = row.pendingCompletionHasLogicalFloor;
        original.logicalCompletionFrame = row.pendingLogicalCompletionFrame;
        original.retiredSource = row.pendingRetiredSource;
        original.currentState = current;
        original.next = row.pendingNext;
        original.first = row.pendingFirst;
        original.stepCount = row.pendingStepCount;
        original.resetIntent = row.pendingResetIntent;
        original.consumesReset = consumesReset;
        original.marker = ++nextOriginalConditionalFactoryMarker_;
        original.loans = 1;
        original.state = OriginalConditionalState::Prepared;
        row.originalConditionalRow = originalIndex;
        row.originalConditionalMarker = original.marker;
        outOriginal->owner_ = this;
        outOriginal->row_ = originalIndex;
        outOriginal->generation_ = original.generation;
        outOriginal->marker_ = original.marker;
        outOriginal->queue_ = token;
        outOriginal->version_ = original.version;
    }
    out->owner_ = this;
    out->queue_ = token;
    out->version_.version = request.next.version;
    out->prepared_ = true;
    out->originalRow_ = row.originalConditionalRow;
    out->originalMarker_ = row.originalConditionalMarker;
    return true;
}

bool QueueInstanceTable::CommitPreparedConditional(
    PreparedQueueConditionalV1* prepared,
    ConditionalQueueVersionToken* outVersion) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || outVersion == nullptr ||
        !prepared->queue_.Valid() || !prepared->version_.Valid() ||
        prepared->queue_.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[prepared->queue_.slot];
    const SlotState slotState =
        row.slotState.load(std::memory_order_acquire);
    if (row.generation != prepared->queue_.generation ||
        (slotState != SlotState::Initializing &&
         slotState != SlotState::Live) ||
        !row.conditionalReserved.load(std::memory_order_acquire) ||
        row.pendingVersion.load(std::memory_order_acquire) != 0 ||
        row.pendingNext.version != prepared->version_.version ||
        !MatchesPreparedOriginalConditional(*prepared, row)) {
        return false;
    }
    if (prepared->originalRow_ != UINT32_MAX)
        originalConditionalRows_[prepared->originalRow_].state = OriginalConditionalState::Committed;
    row.pendingVersion.store(prepared->version_.version,
                             std::memory_order_release);
    row.conditionalReserved.store(false, std::memory_order_release);
    *outVersion = prepared->version_;
    prepared->owner_ = nullptr;
    prepared->queue_ = {};
    prepared->version_ = {};
    prepared->prepared_ = false;
    prepared->originalRow_ = UINT32_MAX;
    prepared->originalMarker_ = 0;
    return true;
}

bool QueueInstanceTable::ProvePrivateConditionalTimingBeforePublication(
    QueueInstanceToken queue,
    ConditionalQueueVersionToken version,
    DecisionToken decision,
    OccurrenceToken currentOccurrence,
    std::uint64_t newLogicalStartFrame,
    std::uint64_t newLogicalCompletionFrame,
    PrivateConditionalTimingProof* out) noexcept {
    if (out == nullptr || out->Valid() || !queue.Valid() ||
        queue.slot >= capacity_ || !version.Valid() ||
        !decision.Valid() || currentOccurrence.generation == 0) return false;
    Row& row = rows_[queue.slot];
    if (row.generation != queue.generation ||
        row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        row.pendingNext.version != version.version ||
        row.nextStateOwner != NextStateOwner::RealConditional ||
        row.pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting ||
        row.conditionalReserved.load(std::memory_order_acquire) ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row.resetReserved.load(std::memory_order_acquire) ||
        row.failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        !SameDecision(row.pendingDecision, decision) ||
        !SameOccurrence(row.pendingExpectedCurrent, currentOccurrence) ||
        HasFrontierReaders(row)) return false;
    const bool single =
        row.pendingCompletionOwner.kind == CompletionOwnerKind::SingleQueue;
    if ((single && (row.pendingCompletionIsLogicalBoundary ||
                   !IsBoundKnownCompletionPredicate(row.pendingCompletion) ||
                   !SameCompletionOwner(row.pendingCompletionOwner,
                                        row.pendingCompletion.owner) ||
                   !SameDecision(row.pendingCompletionOwner.singleDecision,
                                 decision) ||
                   !SameOccurrence(row.pendingCompletion.occurrence,
                                   currentOccurrence))) ||
        (!single && newLogicalStartFrame != 0) ||
        (!(row.pendingCompletionIsLogicalBoundary ||
           row.pendingCompletionHasLogicalFloor) &&
         newLogicalCompletionFrame != 0)) return false;
    KnownCompletionPredicateV1 completion{};
    if (single && !ReanchorKnownCompletionBeforePublication(
            row.pendingCompletion, newLogicalStartFrame,
            &completion)) return false;
    out->owner_ = this;
    out->row_ = &row;
    out->slot_ = queue.slot;
    out->completion_ = completion;
    out->logicalCompletionFrame_ = newLogicalCompletionFrame;
    out->updatesCompletion_ = single;
    return true;
}

bool QueueInstanceTable::ProveRetainedOriginalConditionalTimingBeforePublication(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parent,
    QueueInstanceToken queue, ConditionalQueueVersionToken version, DecisionToken decision,
    OccurrenceToken currentOccurrence, std::uint64_t newLogicalStartFrame,
    std::uint64_t newLogicalCompletionFrame, PrivateConditionalTimingProof* out) noexcept {
    if (out == nullptr || out->Valid() ||
        !IsOriginalConditionalFutureReservationFor(cap, parent, queue, version, decision, currentOccurrence))
        return false;
    if (IsCurrentOriginalConditionalFutureReservation(cap))
        return ProvePrivateConditionalTimingBeforePublication(queue, version, decision, currentOccurrence,
            newLogicalStartFrame, newLogicalCompletionFrame, out);
    if (!IsSelectedOriginalConditionalCurrent(cap)) return false;
    auto& original = originalConditionalRows_[cap.row_];
    auto& row = rows_[queue.slot];
    if (original.state != OriginalConditionalState::Committed ||
        original.clockState != OriginalConditionalClockState::Unbound ||
        row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.nextStateOwner != NextStateOwner::RealConditional ||
        row.pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        row.conditionalReserved.load(std::memory_order_acquire) ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row.resetReserved.load(std::memory_order_acquire) ||
        row.failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        HasFrontierReaders(row) || original.logicalBoundary ||
        original.completionOwner.kind != CompletionOwnerKind::SingleQueue ||
        !IsBoundKnownCompletionPredicate(original.completion) ||
        !SameCompletionOwner(original.completionOwner, original.completion.owner) ||
        !SameDecision(original.completionOwner.singleDecision, decision) ||
        !SameOccurrence(original.completion.occurrence, currentOccurrence) ||
        (!original.logicalFloor && newLogicalCompletionFrame != 0)) return false;
    KnownCompletionPredicateV1 completion{};
    if (!ReanchorKnownCompletionBeforePublication(original.completion, newLogicalStartFrame, &completion))
        return false;
    // This proof belongs to the retained original future. The selected pending
    // future is rebound separately from the same authentic first Source clock.
    out->owner_ = this;
    out->row_ = &row;
    out->slot_ = queue.slot;
    out->completion_ = completion;
    out->retainedOriginal_ = &original;
    out->retainedGeneration_ = original.generation;
    return true;
}

void QueueInstanceTable::ApplyPrivateConditionalTimingBeforePublication(
    const PrivateConditionalTimingProof& proof) noexcept {
    assert(proof.owner_ == this && proof.row_ == &rows_[proof.slot_]);
    if (proof.retainedOriginal_ != nullptr) {
        auto& original = *static_cast<OriginalConditionalRow*>(proof.retainedOriginal_);
        assert(original.generation == proof.retainedGeneration_ && original.selectedFutureConsumed &&
            original.clockState == OriginalConditionalClockState::Unbound);
        original.completion = proof.completion_;
        return;
    }
    auto& row = *static_cast<Row*>(proof.row_);
    if (proof.updatesCompletion_)
        row.pendingCompletion = proof.completion_;
    row.pendingLogicalCompletionFrame =
        proof.logicalCompletionFrame_;
}

bool QueueInstanceTable::CancelPreparedConditional(
    PreparedQueueConditionalV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || !prepared->queue_.Valid() ||
        prepared->queue_.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[prepared->queue_.slot];
    const SlotState slotState =
        row.slotState.load(std::memory_order_acquire);
    if (row.generation != prepared->queue_.generation ||
        (slotState != SlotState::Initializing &&
         slotState != SlotState::Live) ||
        !row.conditionalReserved.load(std::memory_order_acquire) ||
        row.pendingVersion.load(std::memory_order_acquire) != 0 ||
        !MatchesPreparedOriginalConditional(*prepared, row)) {
        return false;
    }
    EndOriginalConditionalFuture(row);
    row.pendingDecision = {};
    row.pendingExpectedCurrent = {};
    row.pendingSuccessor = {};
    row.pendingCompletion = {};
    row.pendingCompletionOwner = {};
    row.pendingCompletionIsLogicalBoundary = false;
    row.pendingCompletionHasLogicalFloor = false;
    row.pendingLogicalCompletionFrame = 0;
    row.pendingRetiredSource = {};
    row.pendingNext = {};
    row.nextStateOwner = NextStateOwner::None;
    row.pendingActivation.store(ConditionalQueueActivation::Waiting,
                                std::memory_order_relaxed);
    row.pendingConsumesReset.store(false, std::memory_order_relaxed);
    row.conditionalReserved.store(false, std::memory_order_release);
    prepared->owner_ = nullptr;
    prepared->queue_ = {};
    prepared->version_ = {};
    prepared->prepared_ = false;
    prepared->originalRow_ = UINT32_MAX;
    prepared->originalMarker_ = 0;
    return true;
}

QueueResetStatusV1 QueueInstanceTable::PrepareConditionalReplacement(
    QueueInstanceToken token,
    ConditionalQueueVersionToken expectedVersion,
    DecisionToken expectedDecision,
    const ConditionalQueuePrepareRequest& replacement,
    PreparedQueueConditionalReplacementV1* out) noexcept {
    return PrepareConditionalReplacementInternal(token, expectedVersion, expectedDecision,
        replacement, out, false);
}

QueueResetStatusV1 QueueInstanceTable::PrepareConditionalReplacementRetainingOriginalCurrent(
    QueueInstanceToken token, ConditionalQueueVersionToken expectedVersion,
    DecisionToken expectedDecision, const ConditionalQueuePrepareRequest& replacement,
    OccurrenceStatePool& occurrences,
    const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& current,
    DecisionTable& decisions, const OriginalDecisionLifetimePin& currentDecision,
    PreparedQueueConditionalReplacementV1* out) noexcept {
    const auto* row = Resolve(token);
    const auto& completion = replacement.completion;
    const auto occurrence = replacement.expectedCurrentOccurrence;
    if (out == nullptr || out->Valid() || row == nullptr ||
        completion.owner.kind != CompletionOwnerKind::SingleQueue ||
        !decisions.IsOriginalDecisionLifetimePinFor(currentDecision, completion.owner.singleDecision) ||
        !occurrences.IsOriginalSingleCompletionOwnerPinFor(current, occurrence) ||
        !occurrences.IsOriginalSingleCompletionOwnerCurrent(current, completion.owner,
            occurrences.CompletionProgressOf(occurrence), completion.renderGeneration,
            occurrences.ScheduleEpochOf(occurrence)) ||
        !SameOccurrence(row->pendingExpectedCurrent, occurrence) ||
        !SameCompletionOwner(row->pendingCompletionOwner, completion.owner) ||
        !SameCompletionOwner(row->pendingCompletion.owner, completion.owner) ||
        row->pendingCompletion.renderGeneration != completion.renderGeneration ||
        row->pendingCompletion.playbackFactsRevision != completion.playbackFactsRevision ||
        row->pendingCompletion.absoluteLogicalStartFrame != completion.absoluteLogicalStartFrame ||
        row->pendingCompletion.absoluteLogicalCompleteFrame != completion.absoluteLogicalCompleteFrame ||
        row->pendingCompletion.totalLogicalOutputFrames != completion.totalLogicalOutputFrames ||
        row->pendingCompletion.progressBaselineOutputFrames != completion.progressBaselineOutputFrames ||
        row->pendingCompletion.progressBaselineRevision != completion.progressBaselineRevision ||
        row->pendingCompletion.progressBaselineCompletedThroughBlockSequence !=
            completion.progressBaselineCompletedThroughBlockSequence ||
        !SameOccurrence(row->pendingCompletion.occurrence, completion.occurrence) ||
        !SamePlayableProgramKey(row->pendingCompletion.program, completion.program))
        return QueueResetStatusV1::InvalidInput;
    OccurrenceStatePool::OriginalSingleCompletionOwnerPin retainedOccurrence;
    OriginalDecisionLifetimePin retainedDecision;
    if (!current.Borrow(&retainedOccurrence) || !currentDecision.Borrow(&retainedDecision))
        return QueueResetStatusV1::Backpressure;
    const auto result = PrepareConditionalReplacementInternal(token, expectedVersion,
        expectedDecision, replacement, out, true);
    if (result == QueueResetStatusV1::Ok) {
        out->currentOccurrence_ = std::move(retainedOccurrence);
        out->currentDecision_ = std::move(retainedDecision);
    }
    return result;
}

QueueResetStatusV1 QueueInstanceTable::PrepareConditionalReplacementInternal(
    QueueInstanceToken token, ConditionalQueueVersionToken expectedVersion,
    DecisionToken expectedDecision, const ConditionalQueuePrepareRequest& replacement,
    PreparedQueueConditionalReplacementV1* out, bool retainedCurrent) noexcept {
    const CompletionOwnerRef owner =
        !replacement.completionIsLogicalBoundary &&
                replacement.completionOwner.kind == CompletionOwnerKind::None
            ? replacement.completion.owner
            : replacement.completionOwner;
    const bool validSingle =
        !replacement.completionIsLogicalBoundary &&
        owner.kind == CompletionOwnerKind::SingleQueue &&
        IsBoundKnownCompletionPredicate(replacement.completion) &&
        SameOccurrence(replacement.completion.occurrence,
                       replacement.expectedCurrentOccurrence) &&
        SameCompletionOwner(owner, replacement.completion.owner) &&
        (retainedCurrent || SameDecision(owner.singleDecision, replacement.rootDecision));
    const bool validComposite =
        !replacement.completionIsLogicalBoundary &&
        owner.kind == CompletionOwnerKind::Composite &&
        owner.composite.Valid();
    const bool validLogicalBoundary =
        replacement.completionIsLogicalBoundary &&
        !replacement.completionHasLogicalFloor &&
        owner.kind == CompletionOwnerKind::None &&
        replacement.completion.owner.kind == CompletionOwnerKind::None;
    const bool validRetiredSource =
        ValidRetiredSourceProof(replacement.retiredSource);
    const bool noRetiredSource =
        EmptyRetiredSourceProof(replacement.retiredSource);
    const bool validLogicalFloor =
        !replacement.completionHasLogicalFloor ||
        (!replacement.completionIsLogicalBoundary &&
         (validSingle || validComposite));
    const bool validLogicalFrame =
        replacement.completionIsLogicalBoundary ||
        replacement.completionHasLogicalFloor ||
        replacement.logicalCompletionFrame == 0;
    if (out == nullptr || out->Valid() || !expectedVersion.Valid() ||
        !expectedDecision.Valid() || !replacement.rootDecision.Valid() ||
        !ValidOccurrence(replacement.expectedCurrentOccurrence) ||
        !ValidOccurrence(replacement.successorOccurrence) ||
        (!validRetiredSource && !noRetiredSource) ||
        (validLogicalBoundary && !noRetiredSource) ||
        !validLogicalFloor || !validLogicalFrame ||
        (static_cast<std::uint32_t>(validSingle) +
             static_cast<std::uint32_t>(validComposite) +
             static_cast<std::uint32_t>(validLogicalBoundary) !=
         1u)) {
        return QueueResetStatusV1::InvalidInput;
    }
    Row* row = Resolve(token);
    EffectiveQueueState current{};
    EffectiveQueueState first{};
    std::uint32_t stepCount = 0;
    if (row == nullptr || SignalOwnsNext(*row) || !LoadMirror(*row, &current) ||
        row->failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            expectedVersion.version ||
        row->pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting ||
        !SameDecision(row->pendingDecision, expectedDecision) ||
        !ValidateProjection(replacement, current, row->childCount, &first, &stepCount)) {
        return QueueResetStatusV1::Backpressure;
    }
    auto resetFirst = row->resetInitial;
    resetFirst.version = current.version + 1u;
    if (!SameEffectiveState(first, resetFirst)) return QueueResetStatusV1::InvalidInput;
    bool expected = false;
    if (!row->conditionalReplacementReserved.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return QueueResetStatusV1::Backpressure;
    }
    out->owner_ = this;
    out->queue_ = token;
    out->expectedVersion_ = expectedVersion;
    out->expectedDecision_ = expectedDecision;
    out->replacement_ = replacement;
    out->replacement_.projectedStates = nullptr;
    out->replacement_.projectedStateCount = 0;
    out->replacement_.completionOwner = owner;
    out->projectionFirst_ = first;
    out->projectionCount_ = stepCount;
    out->prepared_ = true;
    return QueueResetStatusV1::Ok;
}

QueueResetStatusV1
QueueInstanceTable::CommitPreparedConditionalReplacement(
    PreparedQueueConditionalReplacementV1* prepared,
    LogicCommitSequence sequence) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || sequence == 0) {
        return QueueResetStatusV1::InvalidInput;
    }
    Row* row = Resolve(prepared->queue_);
    EffectiveQueueState current{};
    EffectiveQueueState resetProjection{};
    if (row == nullptr ||
        !row->conditionalReplacementReserved.load(
            std::memory_order_acquire) ||
        !row->resetReserved.load(std::memory_order_acquire) ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            prepared->expectedVersion_.version ||
        row->pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting ||
        !SameDecision(row->pendingDecision,
                      prepared->expectedDecision_) ||
        !LoadMirror(*row, &current) ||
        !ProjectReset(*row, current, &resetProjection) ||
        !SameEffectiveState(resetProjection, prepared->projectionFirst_) ||
        row->resetIntentCounter == UINT64_MAX) {
        return QueueResetStatusV1::RuntimeRecreateRequired;
    }
    const ConditionalQueuePrepareRequest& replacement =
        prepared->replacement_;
    if (!MayMutateFrontier(*row)) return QueueResetStatusV1::Backpressure;
    EndOriginalConditionalFuture(*row);
    row->pendingDecision = replacement.rootDecision;
    row->pendingExpectedCurrent = replacement.expectedCurrentOccurrence;
    row->pendingSuccessor = replacement.successorOccurrence;
    row->pendingCompletion = replacement.completion;
    row->pendingCompletionOwner = replacement.completionOwner;
    row->pendingCompletionIsLogicalBoundary =
        replacement.completionIsLogicalBoundary;
    row->pendingCompletionHasLogicalFloor =
        replacement.completionHasLogicalFloor;
    row->pendingLogicalCompletionFrame = replacement.logicalCompletionFrame;
    row->pendingRetiredSource = replacement.retiredSource;
    row->pendingNext = replacement.next;
    row->pendingFirst = prepared->projectionFirst_;
    row->pendingStepCount = prepared->projectionCount_;
    row->pendingVersion.store(replacement.next.version, std::memory_order_release);
    row->pendingConsumesReset.store(true, std::memory_order_release);
    row->pendingResetIntent = row->resetIntentCounter + 1u;
    row->conditionalReplacementReserved.store(
        false, std::memory_order_release);
    prepared->owner_ = nullptr;
    prepared->queue_ = {};
    prepared->expectedVersion_ = {};
    prepared->expectedDecision_ = {};
    prepared->replacement_ = {};
    prepared->prepared_ = false;
    prepared->currentOccurrence_.Release();
    prepared->currentDecision_.Release();
    return QueueResetStatusV1::Ok;
}

QueueResetStatusV1
QueueInstanceTable::CancelPreparedConditionalReplacement(
    PreparedQueueConditionalReplacementV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_) {
        return QueueResetStatusV1::NotFound;
    }
    Row* row = Resolve(prepared->queue_);
    if (row == nullptr ||
        !row->conditionalReplacementReserved.exchange(
            false, std::memory_order_acq_rel)) {
        return QueueResetStatusV1::RuntimeRecreateRequired;
    }
    prepared->owner_ = nullptr;
    prepared->queue_ = {};
    prepared->expectedVersion_ = {};
    prepared->expectedDecision_ = {};
    prepared->replacement_ = {};
    prepared->prepared_ = false;
    prepared->currentOccurrence_.Release();
    prepared->currentDecision_.Release();
    return QueueResetStatusV1::Ok;
}

bool QueueInstanceTable::ActivateConditional(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    DecisionToken expectedDecision,
    OccurrenceToken expectedCurrentOccurrence) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || !version.Valid() ||
        (row->frontierPool != nullptr && row->frontierPool->HasFrontierOwnerOperations(row->frontierOwner, expectedDecision)) ||
        row->conditionalReplacementReserved.load(
            std::memory_order_acquire) ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        !SameDecision(row->pendingDecision, expectedDecision) ||
        !SameOccurrence(row->pendingExpectedCurrent,
                        expectedCurrentOccurrence)) {
        return false;
    }
    ConditionalQueueActivation expected =
        ConditionalQueueActivation::Waiting;
    return row->pendingActivation.compare_exchange_strong(
        expected, ConditionalQueueActivation::Activated,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool QueueInstanceTable::SettleActivated(
    QueueInstanceToken token,
    ConditionalQueueVersionToken version) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || !version.Valid() ||
        row->conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        row->pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Activated) {
        return false;
    }
    if (!StoreMirror(*row, row->pendingNext)) return false;
    if (row->pendingConsumesReset.exchange(false,
                                           std::memory_order_acq_rel)) {
        ConsumeResetIntent(*row, row->pendingResetIntent);
    }
    row->pendingResetIntent = 0;
    EndOriginalConditionalFuture(*row);
    row->nextStateOwner = NextStateOwner::None;
    row->pendingVersion.store(0, std::memory_order_release);
    row->pendingCompletionIsLogicalBoundary = false;
    row->pendingCompletionHasLogicalFloor = false;
    row->pendingLogicalCompletionFrame = 0;
    row->pendingRetiredSource = {};
    row->pendingActivation.store(ConditionalQueueActivation::Waiting,
                                 std::memory_order_release);
    return true;
}

bool QueueInstanceTable::AdvanceAfterSignalCompletion(
    QueueInstanceToken token,
    std::uint64_t expectedCurrentVersion,
    const EffectiveQueueState& next) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || expectedCurrentVersion == UINT64_MAX ||
        SignalOwnsNext(*row) ||
        next.version != expectedCurrentVersion + 1u ||
        !ValidEffectiveState(next, row->childCount) ||
        row->failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        row->pendingVersion.load(std::memory_order_acquire) != 0 ||
        row->resetReserved.load(std::memory_order_acquire)) {
        return false;
    }
    EffectiveQueueState current{};
    if (!LoadMirror(*row, &current) ||
        current.version != expectedCurrentVersion) {
        return false;
    }
    const bool consumesReset =
        row->resetPending.load(std::memory_order_acquire);
    EffectiveQueueState resetProjection{};
    if (consumesReset &&
        (!ProjectReset(*row, current, &resetProjection) ||
         !SameEffectiveState(next, resetProjection))) {
        return false;
    }
    if (!StoreMirror(*row, next)) return false;
    if (consumesReset) {
        ConsumeResetIntent(*row, row->resetIntentGeneration);
    }
    const std::uint64_t handle =
        (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
    MonitorLifecycleFacts facts{};
    facts.projectId = row->projectId;
    facts.programSemanticFingerprint = row->programSemanticFingerprint;
    facts.arrangingInstance = row->rootInstance;
    ObserveMonitorLifecycle(
        MonitorLifecyclePhase::QueueAdvance, handle, token.generation,
        row->world, row->containerStableId, row->rootInstance, 0, 0, 0,
        next.version, facts);
    return true;
}

QueueSignalStatusV1 QueueInstanceTable::PrepareSignalAdvance(
    QueueInstanceToken token, const EffectiveQueueState& current,
    const EffectiveQueueState& next, QueueExecutionOrderV1 order,
    std::uint64_t resetIntent, PreparedQueueSignalAdvanceV1* out) noexcept {
    if (out == nullptr || out->Valid() || order.sequence == 0 ||
        current.version == UINT64_MAX || next.version != current.version + 1u)
        return QueueSignalStatusV1::InvalidInput;
    Row* row = Resolve(token);
    if (row == nullptr) return QueueSignalStatusV1::Stale;
    if (SignalOwnsNext(*row) || row->nextStateOwner != NextStateOwner::None ||
        row->pendingVersion.load(std::memory_order_acquire) != 0 ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row->resetReserved.load(std::memory_order_acquire) ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy)
        return QueueSignalStatusV1::Backpressure;
    if (!ValidEffectiveState(current, row->childCount) ||
        !ValidEffectiveState(next, row->childCount)) return QueueSignalStatusV1::InvalidInput;
    const auto mirrorSequence = row->mirrorSequence.load(std::memory_order_acquire);
    if ((mirrorSequence & 1u) != 0 || mirrorSequence > UINT64_MAX - 2u ||
        row->signalAttemptCounter == UINT64_MAX)
        return QueueSignalStatusV1::RuntimeRecreateRequired;
    row->signalReservation = SignalReservation::Prepared;
    row->signalIdentity = {token, ++row->signalAttemptCounter, resetIntent, mirrorSequence, order};
    out->owner_ = this;
    out->identity_ = row->signalIdentity;
    out->current_ = current;
    out->next_ = next;
    if (!ValidatePreparedSignalAdvance(*out)) {
        (void)CancelPreparedSignalAdvance(out);
        return QueueSignalStatusV1::Stale;
    }
    return QueueSignalStatusV1::Ok;
}

bool QueueInstanceTable::ValidatePreparedSignalAdvance(
    const PreparedQueueSignalAdvanceV1& prepared) const noexcept {
    if (prepared.owner_ != this || !prepared.Valid()) return false;
    const auto& identity = prepared.identity_;
    const Row* row = Resolve(identity.queue);
    if (row == nullptr || row->signalReservation == SignalReservation::None ||
        row->signalIdentity.attempt != identity.attempt ||
        row->nextStateOwner != NextStateOwner::None ||
        row->mirrorSequence.load(std::memory_order_acquire) != identity.mirrorSequence ||
        row->pendingVersion.load(std::memory_order_acquire) != 0 ||
        row->resetReserved.load(std::memory_order_acquire) ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy)
        return false;
    EffectiveQueueState current{};
    if (!LoadMirror(*row, &current) || !SameEffectiveState(current, prepared.current_) ||
        current.version == UINT64_MAX || prepared.next_.version != current.version + 1u ||
        !ValidEffectiveState(prepared.next_, row->childCount)) return false;
    const auto applicableIntent = row->resetIntentGeneration != 0 &&
        QueueExecutionPrecedes(row->resetOrder, identity.order)
        ? row->resetIntentGeneration : 0;
    if (applicableIntent != identity.resetIntent) return false;
    EffectiveQueueState resetProjection{};
    return applicableIntent == 0 ||
        (ProjectReset(*row, current, &resetProjection) &&
         SameEffectiveState(resetProjection, prepared.next_));
}

bool QueueInstanceTable::CommitPreparedSignalAdvance(
    PreparedQueueSignalAdvanceV1* prepared) noexcept {
    if (prepared == nullptr || !ValidatePreparedSignalAdvance(*prepared)) return false;
    Row* row = Resolve(prepared->identity_.queue);
    if (row->signalReservation != SignalReservation::Prepared) return false;
    row->signalReservation = SignalReservation::ReadyToPublish;
    return true;
}

bool QueueInstanceTable::CancelPreparedSignalAdvance(
    PreparedQueueSignalAdvanceV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this || !prepared->Valid()) return false;
    Row* row = Resolve(prepared->identity_.queue);
    if (row == nullptr || row->signalReservation == SignalReservation::None ||
        row->signalIdentity.attempt != prepared->identity_.attempt ||
        row->nextStateOwner != NextStateOwner::None) return false;
    row->signalReservation = SignalReservation::None;
    row->signalIdentity = {};
    prepared->owner_ = nullptr;
    return true;
}

bool QueueInstanceTable::PublishPreparedSignalAdvance(
    PreparedQueueSignalAdvanceV1* prepared, AcceptedQueueSignalAdvanceV1* out) noexcept {
    if (out == nullptr || out->Valid() || prepared == nullptr ||
        !ValidatePreparedSignalAdvance(*prepared)) return false;
    Row* row = Resolve(prepared->identity_.queue);
    if (row->signalReservation != SignalReservation::ReadyToPublish) return false;
    row->pendingNext = prepared->next_;
    row->nextStateOwner = NextStateOwner::AcceptedSignal;
    row->signalReservation = SignalReservation::None;
    *out = prepared->identity_;
    prepared->owner_ = nullptr;
    return true;
}

bool QueueInstanceTable::SettleSignalAdvance(
    const AcceptedQueueSignalAdvanceV1& accepted, QueueSignalOutcomeV1 outcome) noexcept {
    if (!accepted.Valid()) return false;
    Row* row = Resolve(accepted.queue);
    if (row == nullptr || row->nextStateOwner != NextStateOwner::AcceptedSignal ||
        row->signalIdentity.attempt != accepted.attempt ||
        row->signalIdentity.resetIntent != accepted.resetIntent ||
        row->signalIdentity.mirrorSequence != accepted.mirrorSequence ||
        row->signalIdentity.order.executeFrame != accepted.order.executeFrame ||
        row->signalIdentity.order.sequence != accepted.order.sequence ||
        row->signalIdentity.order.executionOrdinal != accepted.order.executionOrdinal ||
        row->mirrorSequence.load(std::memory_order_acquire) != accepted.mirrorSequence ||
        (accepted.resetIntent != 0 && row->resetIntentGeneration != accepted.resetIntent))
        return false;
    if (outcome == QueueSignalOutcomeV1::InFlight) return true;
    if (outcome == QueueSignalOutcomeV1::Applied) {
        if (!StoreMirror(*row, row->pendingNext)) return false;
        ConsumeResetIntent(*row, accepted.resetIntent);
        const auto handle = (static_cast<std::uint64_t>(accepted.queue.generation) << 32) |
            accepted.queue.slot;
        MonitorLifecycleFacts facts{};
        facts.projectId = row->projectId;
        facts.programSemanticFingerprint = row->programSemanticFingerprint;
        facts.arrangingInstance = row->rootInstance;
        ObserveMonitorLifecycle(MonitorLifecyclePhase::QueueAdvance, handle,
            accepted.queue.generation, row->world, row->containerStableId,
            row->rootInstance, 0, 0, 0, row->pendingNext.version, facts);
    } else if (outcome != QueueSignalOutcomeV1::DefinitivelyNotApplied) {
        return false;
    }
    row->pendingNext = {};
    row->nextStateOwner = NextStateOwner::None;
    row->signalIdentity = {};
    return true;
}

bool QueueInstanceTable::RollbackInitializingConditional(
    const PreparedQueueInstanceV1& preparedInstance,
    QueueInstanceToken expectedQueue,
    ConditionalQueueVersionToken expectedVersion,
    DecisionToken expectedDecision) noexcept {
    if (preparedInstance.owner_ != this || !preparedInstance.prepared_ ||
        !preparedInstance.token_.Valid() || !expectedQueue.Valid() ||
        preparedInstance.token_.slot != expectedQueue.slot ||
        preparedInstance.token_.generation != expectedQueue.generation ||
        expectedQueue.slot >= capacity_ || !expectedVersion.Valid() ||
        !expectedDecision.Valid()) return false;
    Row& row = rows_[expectedQueue.slot];
    if (row.generation != expectedQueue.generation ||
        row.slotState.load(std::memory_order_acquire) != SlotState::Initializing ||
        row.pendingVersion.load(std::memory_order_acquire) != expectedVersion.version ||
        row.pendingNext.version != expectedVersion.version ||
        !SameDecision(row.pendingDecision, expectedDecision) ||
        row.pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        row.conditionalReserved.load(std::memory_order_acquire) ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire)) return false;
    if (!MayMutateFrontier(row)) return false;
    EndOriginalConditionalFuture(row);
    row.pendingVersion.store(0, std::memory_order_release);
    row.nextStateOwner = NextStateOwner::None;
    row.pendingCompletionIsLogicalBoundary = false;
    row.pendingCompletionHasLogicalFloor = false;
    row.pendingLogicalCompletionFrame = 0;
    row.pendingRetiredSource = {};
    row.pendingConsumesReset.store(false, std::memory_order_release);
    row.pendingActivation.store(ConditionalQueueActivation::Waiting, std::memory_order_release);
    return true;
}

bool QueueInstanceTable::RetireConditionalForAbort(
    QueueInstanceToken token,
    ConditionalQueueVersionToken version,
    DecisionToken expectedDecision) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || !version.Valid() ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        !SameDecision(row->pendingDecision, expectedDecision)) {
        return false;
    }
    if (!MayMutateFrontier(*row) || row->conditionalReplacementReserved.load(std::memory_order_acquire)) return false;
    const ConditionalQueueActivation activation =
        row->pendingActivation.load(std::memory_order_acquire);
    if (activation == ConditionalQueueActivation::Activated) {
        if (!StoreMirror(*row, row->pendingNext)) return false;
        if (row->pendingConsumesReset.exchange(
                false, std::memory_order_acq_rel)) {
            ConsumeResetIntent(*row, row->pendingResetIntent);
        }
        row->pendingResetIntent = 0;
    } else if (activation != ConditionalQueueActivation::Waiting &&
               activation !=
                   ConditionalQueueActivation::InvalidatedIdentity &&
               activation !=
                   ConditionalQueueActivation::InvalidatedDeadline &&
               activation !=
                   ConditionalQueueActivation::InvalidatedTransport &&
               activation !=
                    ConditionalQueueActivation::InvalidatedProgress &&
                activation !=
                    ConditionalQueueActivation::InvalidatedCapacity) {
        return false;
    }
    EndOriginalConditionalFuture(*row);
    row->pendingVersion.store(0, std::memory_order_release);
    row->nextStateOwner = NextStateOwner::None;
    row->pendingCompletionIsLogicalBoundary = false;
    row->pendingCompletionHasLogicalFloor = false;
    row->pendingLogicalCompletionFrame = 0;
    row->pendingRetiredSource = {};
    row->pendingConsumesReset.store(false, std::memory_order_release);
    row->pendingActivation.store(ConditionalQueueActivation::Waiting,
                                 std::memory_order_release);
    return true;
}

QueueConditionalProofStatus
QueueInstanceTable::ProveConditionalForQuantum(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    const OccurrenceStatePool& occurrences,
    const CompositeCompletionOwnerPool& composites,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence,
    bool currentQuantumScratchAdmitted,
    QueueConditionalProof* outProof) const noexcept {
    const Row* row = Resolve(token);
    if (outProof == nullptr || row == nullptr || !version.Valid() ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        row->pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting) {
        return QueueConditionalProofStatus::Stale;
    }
    const auto evaluate = [&](const KnownCompletionPredicateV1& predicate,
                              KnownCompletionQuantumProof* proof) {
        CompletionOwnerRef actualOwner{};
        if (!occurrences.CaptureCompletionOwner(
                predicate.occurrence, &actualOwner) ||
            !SameCompletionOwner(actualOwner, predicate.owner)) {
            return std::pair{QueueConditionalProofStatus::FailedIdentity,
                             CompletionProofStatus::Invalid};
        }
        CompletionProgressSnapshot snapshot{};
        const CompletionProgressRow* progress =
            occurrences.CompletionProgressOf(predicate.occurrence);
        if (progress == nullptr ||
            !CaptureCompletionProgressExact(
                *progress, predicate.occurrence,
                predicate.renderGeneration, &snapshot)) {
            return std::pair{QueueConditionalProofStatus::FailedProgress,
                             CompletionProofStatus::Invalid};
        }
        const bool completeBeforeQuantum =
            predicate.progressBaselineOutputFrames <=
                UINT64_MAX - predicate.totalLogicalOutputFrames &&
            snapshot.logicalOutputFramesProduced ==
                predicate.progressBaselineOutputFrames +
                    predicate.totalLogicalOutputFrames &&
            snapshot.progressExhausted == 0 &&
            snapshot.playbackFactsRevision ==
                predicate.playbackFactsRevision &&
            snapshot.completedThroughBlockSequence >=
                predicate.progressBaselineCompletedThroughBlockSequence &&
            snapshot.completedThroughBlockSequence <=
                expectedCompletedThroughBlockSequence &&
            snapshot.completedThroughBlockSequence -
                    predicate.progressBaselineCompletedThroughBlockSequence <=
                UINT64_MAX - predicate.progressBaselineRevision &&
            snapshot.progressRevision ==
                predicate.progressBaselineRevision +
                    (snapshot.completedThroughBlockSequence -
                     predicate.progressBaselineCompletedThroughBlockSequence) &&
            predicate.absoluteLogicalCompleteFrame < quantumStartFrame;
        if (completeBeforeQuantum) {
            return std::pair{
                QueueConditionalProofStatus::ReadyToActivate,
                CompletionProofStatus::CompletedBeforeQuantum};
        }
        if (
            expectedCompletedThroughBlockSequence <
                predicate.progressBaselineCompletedThroughBlockSequence ||
            expectedCompletedThroughBlockSequence -
                    predicate.progressBaselineCompletedThroughBlockSequence >
                UINT64_MAX - predicate.progressBaselineRevision) {
            return std::pair{QueueConditionalProofStatus::FailedProgress,
                             CompletionProofStatus::Invalid};
        }
        const std::uint64_t expectedProgressRevision =
            predicate.progressBaselineRevision +
            (expectedCompletedThroughBlockSequence -
             predicate.progressBaselineCompletedThroughBlockSequence);
        return std::pair{
            QueueConditionalProofStatus::ReadyToActivate,
            EvaluateKnownCompletionForQuantum(
                predicate, snapshot, quantumStartFrame, quantumEndFrame,
                expectedCompletedThroughBlockSequence,
                expectedProgressRevision, currentQuantumScratchAdmitted,
                proof)};
    };
    KnownCompletionQuantumProof proof{};
    CompletionProofStatus status = CompletionProofStatus::Invalid;
    if (row->pendingCompletionOwner.kind == CompletionOwnerKind::None &&
        !row->pendingCompletionIsLogicalBoundary &&
        ValidRetiredSourceProof(row->pendingRetiredSource)) {
        if (occurrences.StageOf(row->pendingExpectedCurrent) !=
            OccurrenceStage::AwaitingEventFence) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        QueueConditionalProof result{};
        result.queue = token;
        result.version = version;
        result.decision = row->pendingDecision;
        result.currentOccurrence = row->pendingExpectedCurrent;
        result.successorOccurrence = row->pendingSuccessor;
        result.retiredSourceArm = row->pendingRetiredSource.sourceArm;
        result.retiredSourceRenderHandle =
            row->pendingRetiredSource.sourceRenderHandle;
        result.retiredSourceLogicalCompletionSequence =
            row->pendingRetiredSource.logicalCompletionSequence;
        result.retiredSourceTombstoneAudioSequence =
            row->pendingRetiredSource.tombstoneAudioSequence;
        result.completionMode =
            QueueConditionalCompletionMode::RetiredSource;
        *outProof = result;
        return QueueConditionalProofStatus::ReadyToActivateRetiredSource;
    }
    if (row->pendingCompletionIsLogicalBoundary) {
        if (row->pendingCompletionHasLogicalFloor ||
            row->pendingCompletionOwner.kind != CompletionOwnerKind::None ||
            row->pendingCompletion.owner.kind != CompletionOwnerKind::None) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        if (row->pendingLogicalCompletionFrame < quantumStartFrame) {
            return QueueConditionalProofStatus::MissedDeadline;
        }
        if (row->pendingLogicalCompletionFrame >= quantumEndFrame) {
            return QueueConditionalProofStatus::Waiting;
        }
        const std::uint64_t offset =
            row->pendingLogicalCompletionFrame - quantumStartFrame;
        if (offset > UINT32_MAX) {
            return QueueConditionalProofStatus::FailedProgress;
        }
        proof.activationOffset = static_cast<std::uint32_t>(offset);
        QueueConditionalProof result{};
        result.queue = token;
        result.version = version;
        result.decision = row->pendingDecision;
        result.currentOccurrence = row->pendingExpectedCurrent;
        result.successorOccurrence = row->pendingSuccessor;
        result.completion = proof;
        result.completionMode =
            QueueConditionalCompletionMode::LogicalBoundary;
        *outProof = result;
        return QueueConditionalProofStatus::ReadyToActivate;
    }
    if (row->pendingCompletionOwner.kind ==
        CompletionOwnerKind::SingleQueue) {
        if (!SameOccurrence(row->pendingCompletion.occurrence,
                            row->pendingExpectedCurrent) ||
            !SameCompletionOwner(row->pendingCompletion.owner,
                                 row->pendingCompletionOwner)) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        const auto evaluated = evaluate(row->pendingCompletion, &proof);
        if (evaluated.first !=
            QueueConditionalProofStatus::ReadyToActivate) {
            return evaluated.first;
        }
        status = evaluated.second;
    } else if (row->pendingCompletionOwner.kind ==
               CompletionOwnerKind::Composite) {
        CompositeCompletionOwnerInfo info{};
        if (!composites.Capture(
                row->pendingCompletionOwner.composite, &info) ||
            !SameOccurrence(info.composedOccurrence,
                            row->pendingExpectedCurrent) ||
            info.leafCount == 0 || info.leafCount > 64) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        status = CompletionProofStatus::CompletesAfterQuantum;
        for (std::uint32_t index = 0; index < info.leafCount; ++index) {
            KnownScheduleLeafV1 leaf{};
            if (!composites.CaptureLeaf(
                    row->pendingCompletionOwner.composite, index, &leaf) ||
                !SameCompletionOwner(
                    leaf.predicate.owner,
                    CompletionOwnerRef::Composite(
                        row->pendingCompletionOwner.composite,
                        leaf.predicate.playbackFactsRevision))) {
                return QueueConditionalProofStatus::FailedIdentity;
            }
            KnownCompletionQuantumProof leafProof{};
            const auto evaluated = evaluate(leaf.predicate, &leafProof);
            if (evaluated.first !=
                QueueConditionalProofStatus::ReadyToActivate) {
                return evaluated.first;
            }
            if (evaluated.second == CompletionProofStatus::ProgressLost ||
                evaluated.second == CompletionProofStatus::Invalid) {
                return QueueConditionalProofStatus::FailedProgress;
            }
            if (evaluated.second ==
                CompletionProofStatus::CurrentQuantumNotAdmitted) {
                return QueueConditionalProofStatus::Waiting;
            }
            if (leaf.predicate.absoluteLogicalCompleteFrame ==
                info.absoluteLogicalCompleteFrame) {
                status = evaluated.second;
                proof = leafProof;
            }
        }
    } else {
        return QueueConditionalProofStatus::FailedIdentity;
    }
    if (status == CompletionProofStatus::CompletesAfterQuantum ||
        status == CompletionProofStatus::CurrentQuantumNotAdmitted) {
        return QueueConditionalProofStatus::Waiting;
    }
    const bool completeBeforeQuantum =
        status == CompletionProofStatus::CompletedBeforeQuantum;
    const bool validRetiredSource =
        ValidRetiredSourceProof(row->pendingRetiredSource);
    if (row->pendingCompletionHasLogicalFloor) {
        if (status != CompletionProofStatus::CompletesInQuantum &&
            !completeBeforeQuantum) {
            return QueueConditionalProofStatus::FailedProgress;
        }
        if (row->pendingLogicalCompletionFrame >= quantumEndFrame) {
            return QueueConditionalProofStatus::Waiting;
        }
        if (completeBeforeQuantum &&
            row->pendingLogicalCompletionFrame < quantumStartFrame &&
            !validRetiredSource) {
            return QueueConditionalProofStatus::MissedDeadline;
        }
        if (row->pendingLogicalCompletionFrame >= quantumStartFrame) {
            const std::uint64_t floorOffset =
                row->pendingLogicalCompletionFrame - quantumStartFrame;
            if (floorOffset > UINT32_MAX) {
                return QueueConditionalProofStatus::FailedProgress;
            }
            proof.activationOffset = std::max(
                proof.activationOffset,
                static_cast<std::uint32_t>(floorOffset));
        }
    }
    const bool logicalFloorReady =
        row->pendingCompletionHasLogicalFloor &&
        (status == CompletionProofStatus::CompletesInQuantum ||
         completeBeforeQuantum);
    const bool retiredSourceReady =
        completeBeforeQuantum &&
        validRetiredSource;
    if (status == CompletionProofStatus::CompletesInQuantum ||
        logicalFloorReady ||
        retiredSourceReady) {
        QueueConditionalProof result{};
        result.queue = token;
        result.version = version;
        result.decision = row->pendingDecision;
        result.currentOccurrence = row->pendingExpectedCurrent;
        result.successorOccurrence = row->pendingSuccessor;
        result.retiredSourceArm = row->pendingRetiredSource.sourceArm;
        result.retiredSourceRenderHandle =
            row->pendingRetiredSource.sourceRenderHandle;
        result.retiredSourceLogicalCompletionSequence =
            row->pendingRetiredSource.logicalCompletionSequence;
        result.retiredSourceTombstoneAudioSequence =
            row->pendingRetiredSource.tombstoneAudioSequence;
        result.completion = proof;
        result.completionMode =
            retiredSourceReady
                ? QueueConditionalCompletionMode::RetiredSource
                : QueueConditionalCompletionMode::InQuantum;
        *outProof = result;
        return retiredSourceReady
            ? QueueConditionalProofStatus::ReadyToActivateRetiredSource
            : QueueConditionalProofStatus::ReadyToActivate;
    }
    return completeBeforeQuantum
               ? QueueConditionalProofStatus::MissedDeadline
               : QueueConditionalProofStatus::FailedProgress;
}

QueueConditionalProofStatus
QueueInstanceTable::ProveConditionalForQuantum(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    const OccurrenceStatePool& occurrences,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence,
    bool currentQuantumScratchAdmitted,
    QueueConditionalProof* outProof) const noexcept {
    return ProveConditionalForQuantum(
        token, version, occurrences, EmptyCompositeOwners(),
        quantumStartFrame, quantumEndFrame,
        expectedCompletedThroughBlockSequence,
        currentQuantumScratchAdmitted, outProof);
}

bool QueueInstanceTable::ActivateProvenConditional(
    const QueueConditionalProof& proof) noexcept {
    if ((proof.completionMode != QueueConditionalCompletionMode::InQuantum &&
         proof.completionMode !=
             QueueConditionalCompletionMode::RetiredSource &&
         proof.completionMode !=
             QueueConditionalCompletionMode::LogicalBoundary) ||
        (proof.completionMode ==
             QueueConditionalCompletionMode::RetiredSource &&
         (!ValidOccurrence(proof.successorOccurrence) ||
          !proof.retiredSourceArm.Valid() ||
          proof.retiredSourceRenderHandle == 0 ||
          proof.retiredSourceLogicalCompletionSequence == 0 ||
          proof.retiredSourceTombstoneAudioSequence >
              proof.retiredSourceLogicalCompletionSequence ||
          proof.retiredSourceArm.renderSlot !=
              static_cast<std::uint32_t>(
                  proof.retiredSourceRenderHandle) ||
          proof.retiredSourceArm.renderGeneration !=
              static_cast<std::uint32_t>(
                  proof.retiredSourceRenderHandle >> 32)))) {
        return false;
    }
    return ActivateConditional(proof.queue, proof.version, proof.decision,
                               proof.currentOccurrence);
}

QueueConditionalProofStatus
QueueInstanceTable::EvaluateConditionalForQuantum(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    const OccurrenceStatePool& occurrences,
    const CompositeCompletionOwnerPool& composites,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence,
    bool currentQuantumScratchAdmitted) noexcept {
    QueueConditionalProof proof{};
    const QueueConditionalProofStatus status =
        ProveConditionalForQuantum(
            token, version, occurrences, composites, quantumStartFrame,
            quantumEndFrame, expectedCompletedThroughBlockSequence,
            currentQuantumScratchAdmitted, &proof);
    if (status == QueueConditionalProofStatus::ReadyToActivate ||
        status ==
            QueueConditionalProofStatus::ReadyToActivateRetiredSource) {
        return ActivateProvenConditional(proof) ? status
                                                : QueueConditionalProofStatus::Stale;
    }
    if (status == QueueConditionalProofStatus::FailedIdentity) {
        (void)FailContainer(
            token, version,
            ConditionalQueueActivation::InvalidatedIdentity,
            QueueFailureReason::CompletionOwnerLost, quantumStartFrame);
    } else if (status == QueueConditionalProofStatus::FailedProgress) {
        (void)FailContainer(
            token, version,
            ConditionalQueueActivation::InvalidatedProgress,
            QueueFailureReason::RenderProgressLost, quantumStartFrame);
    } else if (status == QueueConditionalProofStatus::MissedDeadline) {
        (void)FailContainer(
            token, version,
            ConditionalQueueActivation::InvalidatedDeadline,
            QueueFailureReason::Deadline, quantumStartFrame);
    }
    return status;
}

QueueConditionalProofStatus
QueueInstanceTable::EvaluateConditionalForQuantum(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    const OccurrenceStatePool& occurrences,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence,
    bool currentQuantumScratchAdmitted) noexcept {
    return EvaluateConditionalForQuantum(
        token, version, occurrences, EmptyCompositeOwners(),
        quantumStartFrame, quantumEndFrame,
        expectedCompletedThroughBlockSequence,
        currentQuantumScratchAdmitted);
}

QueueFailureDisposition QueueInstanceTable::FailContainer(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    ConditionalQueueActivation activation, QueueFailureReason reason,
    std::uint64_t failureFrame) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr) return QueueFailureDisposition::Stale;
    if (row->failurePhase.load(std::memory_order_acquire) !=
        QueueFailurePhase::Healthy) {
        return QueueFailureDisposition::AlreadyFailed;
    }
    if (!version.Valid() ||
        row->pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        (activation != ConditionalQueueActivation::InvalidatedIdentity &&
         activation != ConditionalQueueActivation::InvalidatedDeadline &&
         activation != ConditionalQueueActivation::InvalidatedTransport &&
         activation != ConditionalQueueActivation::InvalidatedProgress &&
         activation != ConditionalQueueActivation::InvalidatedCapacity)) {
        return QueueFailureDisposition::Stale;
    }
    if (!MayMutateFrontier(*row)) return QueueFailureDisposition::DeferredByFrontier;
    ConditionalQueueActivation expected =
        ConditionalQueueActivation::Waiting;
    if (!row->pendingActivation.compare_exchange_strong(
            expected, activation, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return row->failurePhase.load(std::memory_order_acquire) !=
                       QueueFailurePhase::Healthy
                   ? QueueFailureDisposition::AlreadyFailed
                   : QueueFailureDisposition::Stale;
    }
    row->failureReason = reason;
    row->failureFrame = failureFrame;
    row->failurePhase.store(QueueFailurePhase::FailingSourceActive,
                            std::memory_order_release);
    const std::uint64_t handle =
        (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
    MonitorLifecycleFacts facts{};
    facts.projectId = row->projectId;
    facts.programSemanticFingerprint = row->programSemanticFingerprint;
    facts.arrangingInstance = row->rootInstance;
    ObserveMonitorLifecycle(
        MonitorLifecyclePhase::QueueFail, handle, token.generation, row->world,
        row->containerStableId, row->rootInstance, 0,
        static_cast<std::uint32_t>(reason), 0, 0, facts);
    return QueueFailureDisposition::NewlyFailed;
}

QueueFailureDisposition QueueInstanceTable::FailContainerBeforeConditional(
    QueueInstanceToken token, QueueFailureReason reason,
    std::uint64_t failureFrame) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr) return QueueFailureDisposition::Stale;
    if (row->failurePhase.load(std::memory_order_acquire) !=
        QueueFailurePhase::Healthy) {
        return QueueFailureDisposition::AlreadyFailed;
    }
    if ((reason != QueueFailureReason::TargetPreparationFailed &&
         reason != QueueFailureReason::Capacity &&
         reason != QueueFailureReason::WorldReplaced &&
         reason != QueueFailureReason::TransportReset) ||
        row->pendingVersion.load(std::memory_order_acquire) != 0) {
        return QueueFailureDisposition::Stale;
    }
    if (!MayMutateFrontier(*row)) return QueueFailureDisposition::DeferredByFrontier;
    row->failureReason = reason;
    row->failureFrame = failureFrame;
    QueueFailurePhase expected = QueueFailurePhase::Healthy;
    if (!row->failurePhase.compare_exchange_strong(
            expected, QueueFailurePhase::FailingSourceActive,
            std::memory_order_release, std::memory_order_acquire)) {
        return QueueFailureDisposition::AlreadyFailed;
    }
    const std::uint64_t handle =
        (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
    MonitorLifecycleFacts facts{};
    facts.projectId = row->projectId;
    facts.programSemanticFingerprint = row->programSemanticFingerprint;
    facts.arrangingInstance = row->rootInstance;
    ObserveMonitorLifecycle(
        MonitorLifecyclePhase::QueueFail, handle, token.generation, row->world,
        row->containerStableId, row->rootInstance, 0,
        static_cast<std::uint32_t>(reason), 0, 0, facts);
    return QueueFailureDisposition::NewlyFailed;
}

QueueSequenceStepStatus ResolveSequenceQueueStep(
    const ContainerProgramViewV3& program,
    const EffectiveQueueState& current,
    EffectiveQueueState* outNext) noexcept {
    if (outNext == nullptr ||
        program.mode != ContainerModeV1::SequenceQueue ||
        program.childCount == 0 || program.children == nullptr ||
        current.version == 0 || current.cursor >= program.childCount ||
        program.children[current.cursor].stableChildOrdinal !=
            current.currentChildOrdinal ||
        current.version == UINT64_MAX ||
        current.selectionOrdinal == UINT64_MAX) {
        return QueueSequenceStepStatus::Invalid;
    }
    EffectiveQueueState next = current;
    ++next.version;
    ++next.selectionOrdinal;
    if (current.cursor + 1u < program.childCount) {
        next.cursor = current.cursor + 1u;
    } else if (program.queueCycle == QueueCycleV1::Once ||
               !QueueCycleWraps(program, current.cycleOrdinal)) {
        return QueueSequenceStepStatus::NaturalEnd;
    } else {
        next.cursor = 0;
        ++next.cycleOrdinal;
    }
    next.currentChildOrdinal =
        program.children[next.cursor].stableChildOrdinal;
    *outNext = next;
    return QueueSequenceStepStatus::Successor;
}

SelectionStatusV1 BuildRandomQueuePermutationV1(
    const ContainerProgramViewV3& program,
    const RandomQueueSeedContext& seed,
    const EffectiveQueueState& state,
    SelectionChildV1* childScratch,
    std::uint32_t* remainingScratch,
    std::uint32_t* permutation,
    std::uint32_t capacity) noexcept {
    if (program.mode != ContainerModeV1::RandomQueue ||
        program.memoryScope != MemoryScopeV1::Emitter ||
        program.childCount == 0 || program.children == nullptr ||
        program.childCount > capacity || childScratch == nullptr ||
        remainingScratch == nullptr || permutation == nullptr ||
        program.avoidLastN >= program.childCount ||
        program.avoidLastN > kMaxQueueAvoidHistoryHard ||
        state.version == 0 || state.selectionOrdinal == 0 ||
        state.randomInitialRecentCount > program.avoidLastN ||
        seed.worldSemanticFingerprint == 0 || seed.rootStableId == VnObjectId{} ||
        seed.acceptedPostOrdinal == 0 || seed.resetEpoch == 0 ||
        (program.memoryScope == MemoryScopeV1::Emitter &&
         (seed.emitterSlot == UINT32_MAX ||
          seed.emitterGeneration == 0))) {
        return SelectionStatusV1::InvalidInput;
    }
    for (std::uint32_t index = 0; index < program.childCount; ++index) {
        const ContainerChildProgramViewV2& child =
            program.children[index];
        childScratch[index].stableChildOrdinal =
            child.stableChildOrdinal;
        childScratch[index].authoredStableOrder =
            child.authoredStableOrder;
        childScratch[index].orderWeight = child.orderWeight;
    }
    SelectionSeedInputV1 input{};
    input.worldSemanticFingerprint = seed.worldSemanticFingerprint;
    input.programStableId = program.containerStableId;
    input.programSemanticFingerprint =
        program.programSemanticFingerprint;
    input.memoryScope = program.memoryScope;
    input.emitterSlot = seed.emitterSlot;
    input.emitterGeneration = seed.emitterGeneration;
    input.resetEpoch = seed.resetEpoch;
    input.rootStableId = seed.rootStableId;
    input.callerSeed = seed.callerSeed;
    input.acceptedPostOrdinal = seed.acceptedPostOrdinal;
    input.queueCycleOrdinal = state.cycleOrdinal;
    input.selectionOrdinal = state.selectionOrdinal;
    input.discipline = SelectionDisciplineV1::Rotation;
    std::uint64_t randomSeed = 0;
    if (!ComputeSelectionSeedV1(input, &randomSeed)) {
        return SelectionStatusV1::InvalidInput;
    }
    SplitMix64 stream(randomSeed);
    return BuildRandomRotationV1(
        childScratch, program.childCount,
        state.randomInitialRecentCount == 0
            ? nullptr
            : state.randomInitialRecent.data(),
        state.randomInitialRecentCount, &stream, remainingScratch,
        capacity, permutation, capacity);
}

QueueSequenceStepStatus ResolveRandomQueueStep(
    const ContainerProgramViewV3& program,
    const RandomQueueSeedContext& seed,
    const EffectiveQueueState& current,
    SelectionChildV1* childScratch,
    std::uint32_t* remainingScratch,
    std::uint32_t* permutation,
    std::uint32_t capacity,
    EffectiveQueueState* outNext) noexcept {
    if (outNext == nullptr || current.cursor >= program.childCount ||
        current.version == UINT64_MAX ||
        BuildRandomQueuePermutationV1(
            program, seed, current, childScratch, remainingScratch,
            permutation, capacity) != SelectionStatusV1::Ok ||
        permutation[current.cursor] != current.currentChildOrdinal) {
        return QueueSequenceStepStatus::Invalid;
    }
    EffectiveQueueState next = current;
    ++next.version;
    if (current.cursor + 1u < program.childCount) {
        next.cursor = current.cursor + 1u;
        next.currentChildOrdinal = permutation[next.cursor];
        *outNext = next;
        return QueueSequenceStepStatus::Successor;
    }
    if (program.queueCycle == QueueCycleV1::Once ||
        !QueueCycleWraps(program, current.cycleOrdinal)) {
        return QueueSequenceStepStatus::NaturalEnd;
    }
    if (current.selectionOrdinal == UINT64_MAX) {
        return QueueSequenceStepStatus::Invalid;
    }
    next.cursor = 0;
    ++next.cycleOrdinal;
    ++next.selectionOrdinal;
    next.randomInitialRecentCount = std::min(
        program.avoidLastN, program.childCount - 1u);
    for (std::uint32_t index = 0;
         index < next.randomInitialRecentCount; ++index) {
        next.randomInitialRecent[index] =
            permutation[program.childCount -
                        next.randomInitialRecentCount + index];
    }
    if (BuildRandomQueuePermutationV1(
            program, seed, next, childScratch, remainingScratch,
            permutation, capacity) != SelectionStatusV1::Ok) {
        return QueueSequenceStepStatus::Invalid;
    }
    next.currentChildOrdinal = permutation[0];
    *outNext = next;
    return QueueSequenceStepStatus::Successor;
}

} // namespace harken
