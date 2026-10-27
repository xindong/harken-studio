#include "harken/core/QueueRuntime.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/CompositeCompletionOwnerPool.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/QueueContinuationLane.h"

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

} // namespace

bool QueueInstanceTable::ValidateProjection(const ConditionalQueuePrepareRequest& request,
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

QueueCheckpointTable::QueueCheckpointTable(std::uint32_t capacity)
    : capacity_(capacity != 0 && capacity <= kMaxQueueCheckpointRowsHard &&
          rows_.Reserve(capacity) ? capacity : 0) {}

bool QueueCheckpointTable::PrepareBacking(std::uint32_t count, BackingPreparation* out) noexcept {
    return rows_.PrepareBacking(count, 0,
        [](const Row& row) noexcept { return !row.live && !row.resetReserved; },
        [this](std::uint32_t slot) noexcept { return rows_.Emplace(slot); }, out);
}

QueueInstanceTable::~QueueInstanceTable() {
    assert(liveCatalogResetScopeMarker_ == 0);
    for (const auto& original : originalConditionalRows_) {
        assert(original.state == OriginalConditionalState::Free ||
            original.state == OriginalConditionalState::Quarantined);
        assert(original.loans == 0 && !original.parent.Valid());
    }
    for (const auto& row : rows_) {
        assert(row.originalLifetimePinCount == 0);
        assert(row.originalLifetimeMarker == 0);
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
        !rows_.IsConstructed(original.token_.slot) || original.marker_ == 0) return false;
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
    for (const auto& row : rows_) count += row.live ? 1u : 0u;
    return count;
}

bool QueueCheckpointTable::Store(
    const QueueCheckpointKey& key,
    const QueueCheckpointImage& image) noexcept {
    if (!ValidCheckpointKey(key) || image.effective.version == 0 ||
        !image.replayCurrent) {
        return false;
    }
    for (auto& row : rows_) {
        if (row.live && SameCheckpointKey(row.key, key)) {
            if (row.resetReserved) return false;
            row.image = image;
            return true;
        }
    }
    Row* freeRow = nullptr;
    const auto available = [](const Row& row) noexcept { return !row.live && !row.resetReserved; };
    const auto construct = [this](std::uint32_t slot) noexcept { return rows_.Emplace(slot); };
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (!rows_.IsConstructed(slot)) {
            if (rows_.PreparedUseActive()) continue;
            if (!construct(slot)) return false;
        }
        if (!available(rows_[slot])) continue;
        if (!rows_.ProtectBackingForAcquire(slot, available, construct)) return false;
        freeRow = &rows_[slot];
        break;
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
    for (const auto& row : rows_) {
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
    for (auto& row : rows_) {
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
    for (auto& row : rows_) {
        if (!row.live || !SameCheckpointKey(row.key, key)) continue;
        if (row.resetReserved) return QueueResetStatusV1::Backpressure;
        row.resetReserved = true;
        out->owner_ = this;
        out->slot_ = rows_.IndexOf(row);
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
    if (!rows_.IsConstructed(prepared->slot_)) return QueueResetStatusV1::NotFound;
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
    if (!rows_.IsConstructed(prepared->slot_)) return QueueResetStatusV1::NotFound;
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
    for (auto& row : rows_) {
        if (row.live && row.key.world == world) row = {};
    }
}

void QueueCheckpointTable::ClearAll() noexcept {
    for (auto& row : rows_) row = {};
}

QueueInstanceTable::QueueInstanceTable(std::uint32_t capacity,
    std::uint32_t originalConditionalCapacity)
    : capacity_(capacity != 0 && capacity <= kMaxQueueInstancesHard &&
          rows_.Reserve(capacity) ? capacity : 0) {
    if (capacity_ != 0 && originalConditionalCapacity != 0 &&
        originalConditionalCapacity <= kMaxQueueInstancesHard &&
        originalConditionalRows_.Reserve(originalConditionalCapacity))
        originalConditionalCapacity_ = originalConditionalCapacity;
}

bool QueueInstanceTable::Valid() const noexcept {
    return capacity_ != 0;
}

bool QueueInstanceTable::AttachRowStoragePreparation(void* context,
    bool (*prepare)(void*, std::uint32_t) noexcept) noexcept {
    if (context == nullptr || prepare == nullptr || rowStorageContext_ != nullptr ||
        rows_.ConstructedCount() != 0) return false;
    rowStorageContext_ = context;
    prepareRowStorage_ = prepare;
    return true;
}

bool QueueInstanceTable::PrepareInstanceSlot(std::uint32_t slot) noexcept {
    if (slot >= capacity_) return false;
    if (rows_.IsConstructed(slot)) return true;
    if (memguard::IsAudioTagged() || rows_.PreparedUseActive()) return false;
    return (prepareRowStorage_ == nullptr || prepareRowStorage_(rowStorageContext_, slot + 1u)) &&
        rows_.Emplace(slot);
}

bool QueueInstanceTable::PrepareConditionalSlot(std::uint32_t slot) noexcept {
    return originalConditionalRows_.Emplace(slot);
}

bool QueueInstanceTable::PrepareBacking(std::uint32_t instances, std::uint32_t conditionals,
    BackingPreparation* out) noexcept {
    if (out == nullptr || out->instances.Valid() || out->conditionals.Valid()) return false;
    BackingPreparation candidate;
    if (!rows_.PrepareBacking(std::min(instances, capacity_), 0,
            [](const Row& row) noexcept { return row.slotState.load(std::memory_order_acquire) == SlotState::Free; },
            [this](std::uint32_t slot) noexcept { return PrepareInstanceSlot(slot); }, &candidate.instances) ||
        !originalConditionalRows_.PrepareBacking(std::min(conditionals, originalConditionalCapacity_), 0,
            [](const OriginalConditionalRow& row) noexcept { return row.state == OriginalConditionalState::Free; },
            [this](std::uint32_t slot) noexcept { return PrepareConditionalSlot(slot); }, &candidate.conditionals)) return false;
    *out = std::move(candidate);
    return true;
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
    if (!originalConditionalRows_.IsConstructed(row.Pending().originalConditionalRow)) return false;
    const auto& original = originalConditionalRows_[row.Pending().originalConditionalRow];
    if (original.state != OriginalConditionalState::Committed || original.selectedFutureConsumed ||
        original.marker != row.Pending().originalConditionalMarker ||
        !rows_.IsConstructed(original.queue.slot) || &rows_[original.queue.slot] != &row ||
        original.queue.generation != row.generation || !original.parent.Valid() ||
        (original.clockState != OriginalConditionalClockState::Unbound &&
         original.clockState != OriginalConditionalClockState::Bound) ||
        original.clockPool == nullptr || !original.clockGroups[0].Valid() ||
        !original.clockGroups[1].Valid()) return false;
    EffectiveQueueState current{};
    return row.Pending().pendingVersion.load(std::memory_order_acquire) == original.version.version &&
        row.Pending().pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Waiting &&
        row.failurePhase.load(std::memory_order_acquire) == QueueFailurePhase::Healthy &&
        !row.conditionalReserved.load(std::memory_order_acquire) &&
        SameDecision(row.Pending().pendingDecision, original.decision) &&
        SameOccurrence(row.Pending().pendingExpectedCurrent, original.current) &&
        SameOccurrence(row.Pending().pendingSuccessor, original.successor) &&
        SameCompletionOwner(row.Pending().pendingCompletionOwner, original.completionOwner) &&
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
    for (auto& row : rows_) {
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
        if (!rows_.IsConstructed(slot)) continue;
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
    for (auto& row : rows_) {
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
    for (const auto& row : rows_) {
        if (row.slotState.load(std::memory_order_acquire) ==
            SlotState::Live) {
            ++count;
        }
    }
    return count;
}

std::uint32_t QueueInstanceTable::QuarantinedCount() const noexcept {
    std::uint32_t count = 0;
    for (const auto& row : rows_) {
        if (row.slotState.load(std::memory_order_acquire) ==
            SlotState::Quarantined) {
            ++count;
        }
    }
    return count;
}

QueueInstanceTable::Row* QueueInstanceTable::Resolve(
    QueueInstanceToken token) noexcept {
    if (!token.Valid() || !rows_.IsConstructed(token.slot)) return nullptr;
    Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.generation != token.generation) {
        return nullptr;
    }
    return &row;
}

const QueueInstanceTable::Row* QueueInstanceTable::Resolve(
    QueueInstanceToken token) const noexcept {
    if (!token.Valid() || !rows_.IsConstructed(token.slot)) return nullptr;
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
    if (!token.Valid() || !rows_.IsConstructed(token.slot) || !binding.Valid() || !binding.queueInitial.Valid() ||
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
    std::uint64_t completedAudioSequence, FrontierInitialCompletionProof* out,
    const QueueFrontierSourceWaitContext* sourceWait, QueueConditionalProof* outCompletion) noexcept {
    if (outCompletion != nullptr) *outCompletion = {};
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != conditional.pendingVersion ||
        row->Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        !SameDecision(row->Pending().pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->Pending().pendingExpectedCurrent, conditional.current) ||
        !SameOccurrence(row->Pending().pendingSuccessor, conditional.successor) ||
        row->Pending().pendingCompletionOwner.kind != CompletionOwnerKind::SingleQueue ||
        !SameDecision(row->Pending().pendingCompletionOwner.singleDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->Pending().pendingCompletion.occurrence, conditional.current) ||
        !SameCompletionOwner(row->Pending().pendingCompletion.owner, row->Pending().pendingCompletionOwner) ||
        row->Pending().pendingCompletionIsLogicalBoundary || quantumStartFrame >= quantumEndFrame)
        return fail();
    SourceWaitAuthority authority;
    if (sourceWait != nullptr && !ProveSourceWaitAuthority(binding, lease, pool, *sourceWait,
            quantumStartFrame, quantumEndFrame, completedAudioSequence, &authority)) return fail();
    QueueConditionalProof localCompletion{};
    auto& proof = outCompletion != nullptr ? *outCompletion : localCompletion;
    const auto status = ProveConditionalCompletion({conditional.slot, conditional.generation},
        {conditional.pendingVersion}, occurrences,
        composites == nullptr ? EmptyCompositeOwners() : *composites,
        quantumStartFrame, quantumEndFrame, completedAudioSequence, true, &proof,
        sourceWait == nullptr ? nullptr : &authority);
    if (status != QueueConditionalProofStatus::ReadyToActivate ||
        (proof.completionMode != QueueConditionalCompletionMode::InQuantum &&
         proof.completionMode != QueueConditionalCompletionMode::SourceWait) ||
        !SameDecision(proof.decision, binding.operation.group.decision) ||
        !SameOccurrence(proof.currentOccurrence, conditional.current) ||
        !SameOccurrence(proof.successorOccurrence, conditional.successor) ||
        (proof.completionMode != QueueConditionalCompletionMode::SourceWait &&
         (binding.operation.order.executeFrame < quantumStartFrame ||
          binding.operation.order.executeFrame - quantumStartFrame != proof.completion.activationOffset)))
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
    row.Pending().pendingActivation.store(ConditionalQueueActivation::Activated, std::memory_order_release);
    if (row.Pending().originalConditionalRow != UINT32_MAX)
        originalConditionalRows_[row.Pending().originalConditionalRow].activated.store(true, std::memory_order_release);
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != identity.pendingVersion ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->conditionalReplacementReserved.load(std::memory_order_acquire) ||
        !SameDecision(row->Pending().pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->Pending().pendingExpectedCurrent, identity.current) ||
        !SameOccurrence(row->Pending().pendingSuccessor, identity.successor) ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) || group.frontier != &binding ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready ||
        (row->frontierPool != nullptr && (row->frontierPool != &pool || !(row->frontierOwner == binding.operation.owner))))
        return false;
    row->frontierPool = &pool;
    row->frontierOwner = binding.operation.owner;
    return true;
}

bool QueueInstanceTable::MatchesPrivatePendingFrontierBinding(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parent,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool,
    const TransitionInstancePool& transitions, const FrontierGroupBinding* counterpart,
    const CompositeCompletionOwnerPool* composites) const noexcept {
    if (!binding.Valid() || binding.variableSide.Valid() || binding.operation.kind != FrontierOperationKind::Dependent ||
        !IsCurrentOriginalConditionalFutureReservation(cap) || !IsOriginalLifetimePinFor(parent, cap.Token())) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    Plan03PageGroupView native{};
    if (original.parent.marker_ != parent.marker_ || row.frontierPool != &pool || row.frontierOwner != binding.operation.owner ||
        row.world != binding.operation.world || row.rootInstance != binding.operation.owner.child ||
        row.initialActivationSequence.load(std::memory_order_acquire) != 0 ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row.resetReserved.load(std::memory_order_acquire) || row.resetPending.load(std::memory_order_acquire) ||
        !pool.PrivateImmediateLaneFrontierRollbackReady(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &native) || native.frontier != &binding ||
        native.lease == nullptr || native.lease->Capture().state != AudioSliceState::Ready) return false;
    const auto matchesConditional = [&](const FrontierQueueConditionalIdentity& identity) noexcept {
        return identity.Valid() && identity.slot == original.queue.slot && identity.generation == original.queue.generation &&
            identity.currentVersion == original.currentState.version && identity.pendingVersion == original.version.version &&
            SameOccurrence(identity.current, original.current) && SameOccurrence(identity.successor, original.successor) &&
            SameDecision(binding.operation.group.decision, original.decision);
    };
    if (counterpart != nullptr) {
        const bool preludeRole = binding.role == FrontierGroupRole::EntryMember && binding.operation.group.sliceIndex == 0;
        const bool targetRole = binding.role == FrontierGroupRole::Future && binding.operation.group.sliceIndex == 1;
        if ((!preludeRole && !targetRole) || !counterpart->Valid() || counterpart->variableSide.Valid()) return false;
        const auto& prelude = preludeRole ? binding : *counterpart;
        const auto& target = targetRole ? binding : *counterpart;
        Plan03PageGroupView source{}, destination{};
        TransitionInstanceSnapshot transition{};
        std::uint64_t currentStart = UINT64_MAX, currentComplete = 0;
        const bool single = row.Pending().pendingCompletionOwner.kind == CompletionOwnerKind::SingleQueue;
        if (row.Pending().pendingCompletionIsLogicalBoundary || !EmptyRetiredSourceProof(row.Pending().pendingRetiredSource) ||
            row.Pending().pendingCompletionIsLogicalBoundary != original.logicalBoundary ||
            row.Pending().pendingCompletionHasLogicalFloor != original.logicalFloor ||
            !SameCompletionOwner(row.Pending().pendingCompletionOwner, original.completionOwner)) return false;
        if (single) {
            const auto& completion = row.Pending().pendingCompletion;
            if (!IsBoundKnownCompletionPredicate(completion) ||
                !SameCompletionOwner(completion.owner, original.completionOwner) ||
                !SameOccurrence(completion.occurrence, original.current) ||
                completion.renderGeneration != original.completion.renderGeneration ||
                !SamePlayableProgramKey(completion.program, original.completion.program) ||
                completion.totalLogicalOutputFrames != original.completion.totalLogicalOutputFrames ||
                completion.playbackFactsRevision != original.completion.playbackFactsRevision ||
                completion.progressBaselineOutputFrames != original.completion.progressBaselineOutputFrames ||
                completion.progressBaselineRevision != original.completion.progressBaselineRevision ||
                completion.progressBaselineCompletedThroughBlockSequence != original.completion.progressBaselineCompletedThroughBlockSequence)
                return false;
            currentStart = completion.absoluteLogicalStartFrame;
            currentComplete = completion.absoluteLogicalCompleteFrame;
        } else {
            CompositeCompletionOwnerInfo info{};
            const auto& owner = row.Pending().pendingCompletionOwner;
            if (owner.kind != CompletionOwnerKind::Composite || composites == nullptr ||
                !composites->Capture(owner.composite, &info) || info.leafCount == 0 ||
                !SameOccurrence(info.composedOccurrence, original.current) || info.aggregateRevision != owner.factsRevision)
                return false;
            for (std::uint32_t index = 0; index < info.leafCount; ++index) {
                KnownScheduleLeafV1 leaf{};
                if (!composites->CaptureLeaf(owner.composite, index, &leaf) ||
                    !IsBoundKnownCompletionPredicate(leaf.predicate) ||
                    !SameOccurrence(leaf.occurrence, leaf.predicate.occurrence) ||
                    leaf.renderGeneration != leaf.predicate.renderGeneration ||
                    !SameCompletionOwner(leaf.predicate.owner,
                        CompletionOwnerRef::Composite(owner.composite, leaf.predicate.playbackFactsRevision)) ||
                    leaf.absoluteLeafStartFrame != leaf.predicate.absoluteLogicalStartFrame) return false;
                currentStart = std::min(currentStart, leaf.absoluteLeafStartFrame);
                currentComplete = std::max(currentComplete, leaf.predicate.absoluteLogicalCompleteFrame);
            }
            if (currentComplete != info.absoluteLogicalCompleteFrame) return false;
        }
        if (row.Pending().pendingCompletionHasLogicalFloor)
            currentComplete = std::max(currentComplete, row.Pending().pendingLogicalCompletionFrame);
        if (prelude.role != FrontierGroupRole::EntryMember || prelude.operation.group.sliceIndex != 0 ||
            target.role != FrontierGroupRole::Future || target.operation.group.sliceIndex != 1 ||
            prelude.operation.kind != FrontierOperationKind::Dependent || target.operation.kind != FrontierOperationKind::Dependent ||
            prelude.entry != target.entry || prelude.primary != target.primary ||
            prelude.operation.owner != target.operation.owner || prelude.operation.world != target.operation.world ||
            prelude.operation.transport != target.operation.transport ||
            !SameDecision(prelude.operation.group.decision, original.decision) ||
            !SameDecision(target.operation.group.decision, original.decision) ||
            !prelude.transition.Valid() || prelude.transition.slot != target.transition.slot ||
            prelude.transition.generation != target.transition.generation || prelude.transition.audibleFrame != target.transition.audibleFrame ||
            prelude.operation.order.executeFrame != prelude.transition.audibleFrame ||
            prelude.operation.order.logicCommitSequence != target.operation.order.logicCommitSequence ||
            prelude.operation.order.executionOrdinal != target.operation.order.executionOrdinal ||
            prelude.operation.order.executeFrame >= target.operation.order.executeFrame ||
            prelude.operation.order.executeFrame <= currentStart ||
            prelude.operation.order.executeFrame >= currentComplete ||
            target.operation.order.executeFrame < currentComplete ||
            !pool.PrivateImmediateLaneFrontierRollbackReady(counterpart->operation.group) ||
            !pool.ResolvePlan03GroupSliceProducer(prelude.operation.group, &source) || source.frontier != &prelude ||
            !pool.ResolvePlan03GroupSliceProducer(target.operation.group, &destination) || destination.frontier != &target ||
            source.lease == nullptr || destination.lease == nullptr ||
            source.lease->Capture().state != AudioSliceState::Ready || destination.lease->Capture().state != AudioSliceState::Ready ||
            source.sourceControlCount != 1 || destination.sourceControlCount != 1 ||
            source.memberCount != 2 || destination.memberCount != 2 || source.members == nullptr || destination.members == nullptr ||
            !transitions.Capture({prelude.transition.slot, prelude.transition.generation}, &transition) ||
            transition.state != TransitionAudibleGateState::Replaceable || transition.retiredAtAudioSequence != 0 ||
            transition.audibleFrame != prelude.transition.audibleFrame) return false;
        for (std::uint16_t member = 0; member < 2; ++member) {
            const auto& a = source.members[member];
            const auto& b = destination.members[member];
            if (!a.Valid() || a.slot != b.slot || a.generation != b.generation ||
                a.renderSlot != b.renderSlot || a.renderGeneration != b.renderGeneration) return false;
        }
        if (single && source.members[0].renderGeneration != row.Pending().pendingCompletion.renderGeneration) return false;
        if (prelude.queueConditional.Valid())
            return !prelude.queueInitial.Valid() && !target.queueInitial.Valid() &&
                matchesConditional(prelude.queueConditional) && matchesConditional(target.queueConditional) &&
                !prelude.canonicalInitialCompletion && !target.canonicalInitialCompletion &&
                target.operation.order.executeFrame == currentComplete;
        return prelude.queueInitial.Valid() && prelude.queueInitial == target.queueInitial &&
            !target.queueConditional.Valid() && row.frontierInitial == &prelude &&
            prelude.queueInitial.slot == original.queue.slot && prelude.queueInitial.generation == original.queue.generation &&
            prelude.queueInitial.version == original.currentState.version &&
            prelude.queueInitial.programSemanticFingerprint == original.programSemanticFingerprint &&
            !prelude.canonicalInitialCompletion && target.canonicalInitialCompletion &&
            matchesConditional(target.queueInitialCompletion) &&
            single && SameDecision(original.completionOwner.singleDecision, original.decision);
    }
    if (binding.role == FrontierGroupRole::EntryMember) {
        if (binding.operation.group.sliceIndex != 0 || !binding.queueConditional.Valid() || binding.queueInitial.Valid() ||
            native.sourceControlCount != 0 || !matchesConditional(binding.queueConditional)) return false;
        if (!binding.transition.Valid()) return true;
        TransitionInstanceSnapshot transition{};
        return binding.operation.order.executeFrame == binding.transition.audibleFrame &&
            transitions.Capture({binding.transition.slot, binding.transition.generation}, &transition) &&
            transition.state == TransitionAudibleGateState::Replaceable &&
            transition.audibleFrame == binding.transition.audibleFrame && transition.retiredAtAudioSequence == 0;
    }
    if (binding.role != FrontierGroupRole::Future || binding.operation.group.sliceIndex != 1 ||
        !binding.queueInitial.Valid() || binding.queueConditional.Valid() || !binding.transition.Valid() ||
        row.frontierInitial == nullptr) return false;
    const auto& initial = *row.frontierInitial;
    Plan03PageGroupView source{};
    TransitionInstanceSnapshot transition{};
    if (!initial.Valid() || initial.role != FrontierGroupRole::EntryMember || initial.operation.kind != FrontierOperationKind::Dependent ||
        initial.operation.group.sliceIndex != 0 || initial.entry != binding.entry || initial.primary != binding.primary ||
        initial.operation.owner != binding.operation.owner || initial.queueInitial != binding.queueInitial ||
        !SameDecision(initial.operation.group.decision, binding.operation.group.decision) ||
        initial.transition.slot != binding.transition.slot || initial.transition.generation != binding.transition.generation ||
        initial.transition.audibleFrame != binding.transition.audibleFrame ||
        initial.operation.order.executeFrame != binding.transition.audibleFrame || !(initial.operation.order < binding.operation.order) ||
        binding.queueInitial.slot != original.queue.slot || binding.queueInitial.generation != original.queue.generation ||
        binding.queueInitial.version != original.currentState.version ||
        binding.queueInitial.programSemanticFingerprint != original.programSemanticFingerprint ||
        !pool.PrivateImmediateLaneFrontierRollbackReady(initial.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(initial.operation.group, &source) || source.frontier != &initial ||
        source.lease == nullptr || source.lease->Capture().state != AudioSliceState::Ready ||
        source.sourceControlCount != native.sourceControlCount || source.memberCount != native.memberCount ||
        !transitions.Capture({binding.transition.slot, binding.transition.generation}, &transition) ||
        transition.state != TransitionAudibleGateState::Replaceable ||
        transition.audibleFrame != binding.transition.audibleFrame || transition.retiredAtAudioSequence != 0) return false;
    if (!binding.canonicalInitialCompletion) return true;
    return matchesConditional(binding.queueInitialCompletion) && !original.logicalBoundary &&
        original.completionOwner.kind == CompletionOwnerKind::SingleQueue && IsBoundKnownCompletionPredicate(original.completion) &&
        SameCompletionOwner(original.completion.owner, original.completionOwner) &&
        SameDecision(original.completionOwner.singleDecision, binding.operation.group.decision) &&
        SameOccurrence(original.completion.occurrence, original.current);
}

bool QueueInstanceTable::CapturePrivatePendingFrontierBinding(
    const OriginalConditionalFutureReservation& cap, const OriginalLifetimePin& parent,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool,
    const TransitionInstancePool& transitions, PrivatePendingQueueFrontierBinding* out,
    const FrontierGroupBinding* counterpart, const CompositeCompletionOwnerPool* composites) const noexcept {
    if (out == nullptr || out->Valid() || !MatchesPrivatePendingFrontierBinding(cap, parent, binding, pool, transitions, counterpart, composites)) return false;
    out->owner_ = this; out->original_ = &cap; out->parent_ = &parent;
    out->binding_ = &binding; out->pool_ = &pool;
    out->counterpart_ = counterpart;
    out->transitions_ = &transitions;
    out->composites_ = composites;
    return true;
}

bool PrivatePendingQueueFrontierBinding::Matches(const FrontierGroupBinding& binding,
    const ScheduledTransactionPool& pool) const noexcept {
    return owner_ != nullptr && original_ != nullptr && parent_ != nullptr && transitions_ != nullptr &&
        binding_ == &binding && pool_ == &pool &&
        owner_->MatchesPrivatePendingFrontierBinding(*original_, *parent_, binding, pool, *transitions_, counterpart_, composites_);
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
    FrontierConditionalProof* out, FrontierConditionalSlice slice,
    const QueueFrontierSourceWaitContext* sourceWait, QueueConditionalProof* outCompletion,
    const FrontierInitialProof* stagedInitial) noexcept {
    if (outCompletion != nullptr) *outCompletion = {};
    const auto& identity = binding.queueConditional;
    if (!binding.Valid() || !identity.Valid() || binding.variableSide.Valid() || binding.queueInitial.Valid() ||
        out == nullptr || out->Valid()) return false;
    auto* row = Resolve({identity.slot, identity.generation});
    Plan03PageGroupView group{};
    if (row == nullptr || row->frontierPool == nullptr || !(row->frontierOwner == binding.operation.owner) ||
        row->world != binding.operation.world || row->rootInstance != binding.operation.owner.child ||
        row->mirrorVersion.load(std::memory_order_acquire) != identity.currentVersion ||
        row->Pending().pendingVersion.load(std::memory_order_acquire) != identity.pendingVersion ||
        !SameDecision(row->Pending().pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->Pending().pendingExpectedCurrent, identity.current) ||
        !SameOccurrence(row->Pending().pendingSuccessor, identity.successor) ||
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
            row->Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Activated) {
            out->Release();
            return false;
        }
        return true;
    }
    SourceWaitAuthority authority;
    const bool recovering = sourceWait != nullptr && slice == FrontierConditionalSlice::Activation &&
        !row->Pending().pendingCompletionIsLogicalBoundary && !ValidRetiredSourceProof(row->Pending().pendingRetiredSource);
    if (recovering && !ProveSourceWaitAuthority(binding, lease, *row->frontierPool, *sourceWait,
            quantumStartFrame, quantumEndFrame, completedAudioSequence, &authority, stagedInitial)) {
        out->Release();
        return false;
    }
    QueueConditionalProof localCompletion{};
    auto& proof = outCompletion != nullptr ? *outCompletion : localCompletion;
    const auto status = ProveConditionalCompletion({identity.slot, identity.generation}, {identity.pendingVersion},
        occurrences, composites == nullptr ? EmptyCompositeOwners() : *composites,
        quantumStartFrame, quantumEndFrame, completedAudioSequence, true, &proof,
        recovering ? &authority : nullptr);
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
            proof.completionMode != QueueConditionalCompletionMode::SourceWait &&
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
    if (proof.slice_ != FrontierConditionalSlice::Activation) return;
    row.Pending().pendingActivation.store(ConditionalQueueActivation::Activated, std::memory_order_release);
    if (row.Pending().originalConditionalRow != UINT32_MAX)
        originalConditionalRows_[row.Pending().originalConditionalRow].activated.store(true, std::memory_order_release);
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
        if (!rows_.IsConstructed(slot)) continue;
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
    const auto available = [](const Row& row) noexcept {
        return row.slotState.load(std::memory_order_acquire) == SlotState::Free;
    };
    const auto construct = [this](std::uint32_t slot) noexcept { return PrepareInstanceSlot(slot); };
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        if (!rows_.IsConstructed(i)) {
            if (rows_.PreparedUseActive()) continue;
            if (!construct(i)) return false;
        }
        Row& row = rows_[i];
        if (!available(row)) continue;
        if (!rows_.ProtectBackingForAcquire(i, available, construct)) return false;
        SlotState expected = SlotState::Free;
        if (!row.slotState.compare_exchange_strong(
                expected, SlotState::Initializing, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        assert(row.originalLifetimePinCount == 0 && row.originalLifetimeMarker == 0);
        assert(row.Pending().originalConditionalRow == UINT32_MAX && row.Pending().originalConditionalMarker == 0);
        row.world = request.world;
        row.projectId = request.projectId;
        row.frontierPool = nullptr;
        row.frontierInitial = nullptr;
        row.frontierOwner = {};
        row.initialActivationSequence.store(0, std::memory_order_relaxed);
        row.rootInstance = request.rootInstance;
        row.containerStableId = request.containerStableId;
        row.programSemanticFingerprint = request.programSemanticFingerprint;
        row.monitorIdentity = request.monitorIdentity;
        row.childCount = request.childCount;
        row.cycle = request.cycle;
        row.resumePosition = request.resumePosition;
        row.resetEpoch = request.resetEpoch;
        row.emitterSlot = request.emitterSlot;
        row.emitterGeneration = request.emitterGeneration;
        row.resetInitial = resetInitial;
        row.resetNextVersion = 0;
        row.resetReserved.store(false, std::memory_order_relaxed);
        row.resetPending.store(false, std::memory_order_relaxed);
        row.resetExecuteFrame = 0;
        row.resetReservation = 0;
        row.resetIntentCounter = 0;
        row.resetIntentGeneration = 0;
        row.resetOrder = {};
        row.monitorReset = {};
        row.Pending().pendingResetIntent = 0;
        row.signalReservation = SignalReservation::None;
        row.nextStateOwner = NextStateOwner::None;
        row.signalAttemptCounter = 0;
        row.signalIdentity = {};
        row.conditionalReserved.store(false, std::memory_order_relaxed);
        row.conditionalReplacementReserved.store(
            false, std::memory_order_relaxed);
        row.stagedReplacementOwner = nullptr;
        row.Pending().pendingConsumesReset.store(false,
                                       std::memory_order_relaxed);
        row.Pending().pendingRetiredSource = {};
        if (!StoreMirror(row, request.initial)) {
            row.slotState.store(SlotState::Quarantined,
                                std::memory_order_release);
            return false;
        }
        row.Pending().pendingVersion.store(0, std::memory_order_relaxed);
        row.Pending().pendingActivation.store(ConditionalQueueActivation::Waiting,
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
        !prepared->token_.Valid() || !rows_.IsConstructed(prepared->token_.slot)) {
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
        !rows_.IsConstructed(prepared->token_.slot)) {
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
        row.Pending().pendingVersion.load(std::memory_order_acquire) != 0) {
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
    row.monitorIdentity = {};
    row.projectId = {};
    row.rootInstance = 0;
    row.containerStableId = {};
    row.programSemanticFingerprint = 0;
    row.childCount = 0;
    row.resetInitial = {};
    row.resetNextVersion = 0;
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
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
    row->Pending().pendingRetiredSource = {};
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool QueueInstanceTable::Capture(QueueInstanceToken token,
                                 QueueInstanceSnapshot* out) const noexcept {
    if (out == nullptr) return false;
    const Row* row = Resolve(token);
    return row != nullptr && CaptureRowSnapshot(*row, out);
}

bool QueueInstanceTable::CaptureMonitorReset(QueueInstanceToken token,
    MonitorQueueResetObservation* out) const noexcept {
    const auto* row = Resolve(token);
    if (out == nullptr || row == nullptr ||
        row->monitorReset.stage == MonitorQueueResetObservation::Stage::None) return false;
    *out = row->monitorReset;
    return true;
}

void QueueInstanceTable::RecordMonitorResetSelection(QueueInstanceToken token,
    QueueExecutionOrderV1 order, const MonitorControlCause& cause) noexcept {
    auto* row = Resolve(token);
    EffectiveQueueState current{};
    // Native replacement uses the same zero-frame normalization at selection.
    if (order.executeFrame == 0) order.executeFrame = logicNow_;
    if (row == nullptr || row->resetIntentGeneration == 0 ||
        row->resetOrder.executeFrame != order.executeFrame ||
        row->resetOrder.sequence != order.sequence ||
        row->resetOrder.executionOrdinal != order.executionOrdinal ||
        !LoadMirror(*row, &current)) return;
    if (row->monitorReset.resetIntent == row->resetIntentGeneration) return;
    row->monitorReset = {cause, order, row->resetIntentGeneration, current.version,
        row->Pending().pendingNext.version, MonitorQueueResetObservation::Stage::Selected};
}

MonitorControlCause QueueInstanceTable::MarkMonitorResetApplied(Row& row,
    std::uint64_t intent, std::uint64_t previousVersion, std::uint64_t nextVersion) noexcept {
    if (intent == 0 || row.monitorReset.resetIntent != intent) return {};
    row.monitorReset.previousVersion = previousVersion;
    row.monitorReset.stage = MonitorQueueResetObservation::Stage::Applied;
    row.monitorReset.nextVersion = nextVersion;
    return row.monitorReset.cause;
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
        row->Pending().pendingActivation.load(std::memory_order_acquire);
    snapshot.pendingVersion =
        row->Pending().pendingVersion.load(std::memory_order_acquire);
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

const QueueInstanceTable::ConditionalState* QueueInstanceTable::FindConditional(
    const Row& row, ConditionalQueueVersionToken version) noexcept {
    if (!version.Valid()) return nullptr;
    for (auto* state = &row.Pending(); state != nullptr;
         state = state->following.load(std::memory_order_acquire)) {
        if (state->pendingVersion.load(std::memory_order_acquire) == version.version) return state;
    }
    return nullptr;
}

QueueInstanceTable::ConditionalState* QueueInstanceTable::FindConditional(
    Row& row, ConditionalQueueVersionToken version) noexcept {
    return const_cast<ConditionalState*>(FindConditional(static_cast<const Row&>(row), version));
}

bool QueueInstanceTable::ResolveConditionalVersion(QueueInstanceToken token, DecisionToken decision,
    ConditionalQueueVersionToken* out) const noexcept {
    const auto* row = Resolve(token);
    if (row == nullptr || out == nullptr || !decision.Valid()) return false;
    for (auto* state = &row->Pending(); state != nullptr;
         state = state->following.load(std::memory_order_acquire)) {
        const auto version = state->pendingVersion.load(std::memory_order_acquire);
        if (version != 0 && SameDecision(state->pendingDecision, decision)) {
            *out = {version};
            return true;
        }
    }
    return false;
}

void QueueInstanceTable::EndConditionalChain(Row& row) noexcept {
    auto* state = &row.Pending();
    row.conditionalHead.store(&row.initialConditional, std::memory_order_release);
    while (state != nullptr) {
        auto* next = state->following.exchange(nullptr, std::memory_order_acq_rel);
        state->pendingVersion.store(0, std::memory_order_release);
        if (state->originalConditionalRow != UINT32_MAX) {
            originalConditionalRows_[state->originalConditionalRow].predecessor.Release();
            EndOriginalConditionalFuture(row, *state);
        }
        state = next;
    }
    row.initialConditional.pendingVersion.store(0, std::memory_order_release);
    row.initialConditional.sourceArms = nullptr;
    row.initialConditional.sourceOccurrences = nullptr;
    row.initialConditional.predecessorReceipt = nullptr;
    row.initialConditional.predecessorDecision = {};
    row.initialConditional.sourceArm = {};
    row.initialConditional.pendingActivation.store(ConditionalQueueActivation::Waiting,
        std::memory_order_release);
}

bool QueueInstanceTable::FollowingSourceAdmitted(const ConditionalState& state) const noexcept {
    if (state.sourceArms == nullptr) return true;
    if (state.pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Activated)
        return true;
    TimelineArmObservation arm{};
    return state.predecessorReceipt != nullptr && state.predecessorReceipt->load(std::memory_order_acquire) &&
        state.sourceArms->Observe(state.sourceArm, &arm) &&
        arm.startedBlockSequence != 0 && SameDecision(arm.parentGroup.decision, state.predecessorDecision) &&
        (arm.state == ArmState::Started ||
         (arm.state == ArmState::Tombstoned && arm.logicalCompletionSequence != 0));
}

QueueConditionalProofStatus QueueInstanceTable::CaptureConditionalSourceDelay(
    QueueInstanceToken token, ConditionalQueueVersionToken version, std::uint64_t* delay) const noexcept {
    const auto* row = Resolve(token);
    const auto* state = row == nullptr ? nullptr : FindConditional(*row, version);
    if (delay == nullptr) return QueueConditionalProofStatus::FailedIdentity;
    *delay = 0;
    if (state == nullptr) return QueueConditionalProofStatus::ReadyToActivate;
    if (state->sourceArms == nullptr) return QueueConditionalProofStatus::ReadyToActivate;
    TimelineArmObservation arm{};
    if (!state->sourceArms->Observe(state->sourceArm, &arm) ||
        !SameDecision(arm.parentGroup.decision, state->predecessorDecision) ||
        arm.state == ArmState::CancelRequested ||
        (arm.state == ArmState::Tombstoned && arm.logicalCompletionSequence == 0))
        return QueueConditionalProofStatus::FailedIdentity;
    if (!FollowingSourceAdmitted(*state)) return QueueConditionalProofStatus::Waiting;
    const auto frozenStart = state->pendingCompletion.absoluteLogicalStartFrame;
    if (arm.actualStartFrame == UINT64_MAX || arm.actualStartFrame < frozenStart)
        return QueueConditionalProofStatus::FailedIdentity;
    *delay = arm.actualStartFrame - frozenStart;
    return QueueConditionalProofStatus::ReadyToActivate;
}

bool QueueInstanceTable::ConditionalSourceReady(QueueInstanceToken token,
    ConditionalQueueVersionToken version) const noexcept {
    const auto* row = Resolve(token);
    const auto* state = row == nullptr ? nullptr : FindConditional(*row, version);
    return state == nullptr || FollowingSourceAdmitted(*state);
}

bool QueueInstanceTable::ProvePendingConditionalIdentity(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    DecisionToken decision) const noexcept {
    const auto* row = Resolve(token);
    const auto* state = row == nullptr ? nullptr : FindConditional(*row, version);
    return state != nullptr && decision.Valid() && SameDecision(state->pendingDecision, decision);
}

bool QueueInstanceTable::ResolveEffective(
    QueueInstanceToken token, EffectiveQueueState* out) const noexcept {
    if (out == nullptr) return false;
    const Row* row = Resolve(token);
    if (row == nullptr) return false;
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        EffectiveQueueState state{};
        if (!LoadMirror(*row, &state)) continue;
        auto effective = state;
        for (auto* pending = &row->Pending(); pending != nullptr;
             pending = pending->following.load(std::memory_order_acquire)) {
            if (pending->pendingVersion.load(std::memory_order_acquire) <= effective.version ||
                pending->pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Activated)
                break;
            effective = pending->pendingNext;
        }
        if (effective.version > state.version) {
            *out = effective;
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
    EffectiveQueueState* outNext, bool* outReset, MonitorQueueResetObservation* observation) const noexcept {
    if (observation != nullptr) *observation = {};
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
    if (observation != nullptr && row->monitorReset.resetIntent == row->resetIntentGeneration)
        *observation = row->monitorReset;
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
    std::uint64_t* outResetIntent, MonitorQueueResetObservation* observation) const noexcept {
    if (observation != nullptr) *observation = {};
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
    if (observation != nullptr && row->monitorReset.resetIntent == *outResetIntent)
        *observation = row->monitorReset;
    return true;
}

void QueueInstanceTable::ConsumeResetIntent(Row& row, std::uint64_t intent) noexcept {
    if (intent == 0 || row.resetIntentGeneration != intent) return;
    row.resetPending.store(false, std::memory_order_release);
    row.resetExecuteFrame = 0;
    row.resetNextVersion = 0;
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
        if (!rows_.IsConstructed(slot)) continue;
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
            (row.Pending().pendingVersion.load(std::memory_order_acquire) != 0 &&
             !row.conditionalReplacementReserved.load(
                 std::memory_order_acquire) && row.stagedReplacementOwner == nullptr)) {
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
    std::uint64_t executeFrame, const MonitorControlCause& cause) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || sequence == 0 ||
        (executeFrame != UINT64_MAX && executeFrame != prepared->order_.executeFrame)) {
        return QueueResetStatusV1::InvalidInput;
    }
    std::uint32_t matchedReservations = 0;
    if (prepared->nativeScopeMarker_ != 0 &&
        prepared->nativeScopeMarker_ != liveCatalogResetScopeMarker_)
        return QueueResetStatusV1::InvalidInput;
    for (const auto& row : rows_) {
        if (prepared->nativeScopeMarker_ != 0 &&
            row.catalogResetScopeMarker == prepared->nativeScopeMarker_ && SameResetTarget(row, prepared->target_)) {
            // A remainder can commit only after the real shared winner selected
            // every excluded native row under this exact authored Reset order.
            if (row.generation != row.catalogResetScopeGeneration ||
                !originalConditionalRows_.IsConstructed(row.Pending().originalConditionalRow) ||
                !originalConditionalRows_[row.Pending().originalConditionalRow].selectedFutureConsumed ||
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
    for (auto& row : rows_) {
        if (row.slotState.load(std::memory_order_acquire) !=
                SlotState::Live ||
            !SameResetTarget(row, prepared->target_) ||
            row.resetReservation != prepared->reservation_ ||
            !row.resetReserved.load(std::memory_order_acquire)) {
            continue;
        }
        EffectiveQueueState current{};
        (void)LoadMirror(row, &current);
        row.resetNextVersion = current.version + 1u;
        row.resetIntentGeneration = ++row.resetIntentCounter;
        row.resetOrder = prepared->order_;
        row.resetOrder.sequence = sequence;
        row.monitorReset = {cause, row.resetOrder, row.resetIntentGeneration, current.version,
            row.resetNextVersion, row.Pending().pendingConsumesReset.load(std::memory_order_acquire) &&
                row.Pending().pendingResetIntent == row.resetIntentGeneration
                ? MonitorQueueResetObservation::Stage::Selected : MonitorQueueResetObservation::Stage::Intent};
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
    for (auto& row : rows_) {
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
    for (auto& row : rows_) {
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
        row.Pending().pendingCompletion = rebound;
        row.Pending().pendingLogicalCompletionFrame = original.logicalFloor ? frames[2] : 0;
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
    if (row.Pending().originalConditionalRow != cap.row_ || row.Pending().originalConditionalMarker != cap.marker_ ||
        row.generation != original.queue.generation || row.world != original.world ||
        row.rootInstance != original.rootInstance ||
        row.programSemanticFingerprint != original.programSemanticFingerprint ||
        row.failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        row.Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        !SameDecision(row.Pending().pendingDecision, original.decision) ||
        !SameOccurrence(row.Pending().pendingExpectedCurrent, original.current) ||
        !SameOccurrence(row.Pending().pendingSuccessor, original.successor) ||
        !SameCompletionOwner(row.Pending().pendingCompletionOwner, original.completionOwner) ||
        !LoadMirror(row, &current) || !SameEffectiveState(current, original.currentState)) return false;
    if (original.state == OriginalConditionalState::Prepared) {
        if (!row.conditionalReserved.load(std::memory_order_acquire) ||
            row.Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
            row.Pending().pendingNext.version != original.version.version) return false;
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
        !originalConditionalRows_.IsConstructed(cap.row_)) return false;
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
        row.Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
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
        row.Pending().originalConditionalRow == cap.row_ && row.Pending().originalConditionalMarker == cap.marker_ &&
        row.generation == original.queue.generation && row.world == original.world &&
        row.rootInstance == original.rootInstance &&
        row.programSemanticFingerprint == original.programSemanticFingerprint &&
        row.Pending().pendingVersion.load(std::memory_order_acquire) == original.version.version &&
        row.Pending().pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Waiting &&
        row.failurePhase.load(std::memory_order_acquire) == QueueFailurePhase::Healthy &&
        !row.conditionalReserved.load(std::memory_order_acquire) &&
        SameDecision(row.Pending().pendingDecision, original.decision) &&
        SameOccurrence(row.Pending().pendingExpectedCurrent, original.current) &&
        SameOccurrence(row.Pending().pendingSuccessor, original.successor) &&
        SameCompletionOwner(row.Pending().pendingCompletionOwner, original.completionOwner) &&
        LoadMirror(row, &current) && SameEffectiveState(current, original.currentState);
}

bool QueueInstanceTable::IsSelectedOriginalConditionalCurrent(
    const OriginalConditionalFutureReservation& cap) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    const auto& row = rows_[original.queue.slot];
    return IsAcceptedSelectedConditionalHead(row, original) &&
        row.Pending().pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Waiting;
}

bool QueueInstanceTable::IsSelectedOriginalConditionalFutureReservationFor(
    const OriginalConditionalFutureReservation& cap,
    const OriginalLifetimePin& parent, QueueInstanceToken queue,
    ConditionalQueueVersionToken version, DecisionToken decision,
    OccurrenceToken current) const noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap) || !IsOriginalLifetimePinFor(parent, queue)) return false;
    const auto& original = originalConditionalRows_[cap.row_];
    return original.queue.slot == queue.slot && original.queue.generation == queue.generation &&
        original.parent.marker_ == parent.marker_ && original.selectedVersion.version == version.version &&
        SameDecision(original.selectedDecision, decision) && SameOccurrence(original.current, current) &&
        IsAcceptedSelectedConditionalHead(rows_[queue.slot], original);
}

bool QueueInstanceTable::IsAcceptedSelectedConditionalHead(const Row& row,
    const OriginalConditionalRow& original) const noexcept {
    EffectiveQueueState current{};
    const auto& head = row.Pending();
    const auto activation = head.pendingActivation.load(std::memory_order_acquire);
    return original.state == OriginalConditionalState::Committed && original.selectedFutureConsumed &&
        original.selectedFutureOwner != nullptr && original.selectedFutureMarker != 0 &&
        IsOriginalLifetimePinFor(original.parent, original.queue) && original.body == &head &&
        originalConditionalRows_.IsConstructed(head.originalConditionalRow) &&
        &originalConditionalRows_[head.originalConditionalRow] == &original &&
        head.originalConditionalMarker == original.marker &&
        FindConditional(row, original.selectedVersion) == &head &&
        row.slotState.load(std::memory_order_acquire) == SlotState::Live &&
        row.generation == original.queue.generation && row.world == original.world &&
        row.rootInstance == original.rootInstance && row.programSemanticFingerprint == original.programSemanticFingerprint &&
        row.Pending().pendingVersion.load(std::memory_order_acquire) == original.selectedVersion.version &&
        (activation == ConditionalQueueActivation::Waiting || activation == ConditionalQueueActivation::Activated) &&
        row.failurePhase.load(std::memory_order_acquire) == QueueFailurePhase::Healthy &&
        !row.conditionalReserved.load(std::memory_order_acquire) &&
        SameDecision(row.Pending().pendingDecision, original.selectedDecision) &&
        SameOccurrence(row.Pending().pendingExpectedCurrent, original.current) &&
        SameOccurrence(row.Pending().pendingSuccessor, original.selectedSuccessor) &&
        SameCompletionOwner(row.Pending().pendingCompletionOwner, original.completionOwner) &&
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
        return prepared.originalMarker_ == 0 && row.Pending().originalConditionalRow == UINT32_MAX &&
            row.Pending().originalConditionalMarker == 0;
    if (!originalConditionalRows_.IsConstructed(prepared.originalRow_) ||
        prepared.originalMarker_ == 0) return false;
    const auto& original = originalConditionalRows_[prepared.originalRow_];
    if (original.body == nullptr) return false;
    const auto& state = *original.body;
    return state.originalConditionalRow == prepared.originalRow_ &&
        state.originalConditionalMarker == prepared.originalMarker_ &&
        original.state == OriginalConditionalState::Prepared &&
        original.marker == prepared.originalMarker_ &&
        original.queue.slot == prepared.queue_.slot &&
        original.queue.generation == prepared.queue_.generation &&
        original.version.version == prepared.version_.version &&
        original.world == row.world && original.rootInstance == row.rootInstance &&
        original.programSemanticFingerprint == row.programSemanticFingerprint &&
        SameDecision(original.decision, state.pendingDecision) &&
        SameOccurrence(original.current, state.pendingExpectedCurrent) &&
        SameOccurrence(original.successor, state.pendingSuccessor) &&
        SameCompletionOwner(original.completionOwner, state.pendingCompletionOwner) &&
        original.next.version == state.pendingNext.version &&
        original.currentState.version != UINT64_MAX &&
        original.first.version == original.currentState.version + 1u &&
        original.next.version >= original.first.version &&
        original.next.version - original.first.version + 1u == original.stepCount &&
        original.stepCount == state.pendingStepCount &&
        original.logicalBoundary == state.pendingCompletionIsLogicalBoundary &&
        original.logicalFloor == state.pendingCompletionHasLogicalFloor &&
        original.logicalCompletionFrame == state.pendingLogicalCompletionFrame &&
        original.resetIntent == state.pendingResetIntent &&
        original.consumesReset == state.pendingConsumesReset.load(std::memory_order_acquire) &&
        IsOriginalLifetimePinFor(original.parent, original.queue);
}

bool QueueInstanceTable::RegisterOriginalConditionalOwner(void* context,
    OriginalConditionalBaselineResolver resolver,
    OriginalConditionalAssociationEnding associationEnding) noexcept {
    if (context == nullptr || resolver == nullptr || associationEnding == nullptr ||
        originalConditionalOwnerContext_ != nullptr) return false;
    for (const auto& original : originalConditionalRows_)
        if (original.state != OriginalConditionalState::Free || original.loans != 0) return false;
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
        (row.Pending().originalConditionalRow != cap.row_ || row.Pending().originalConditionalMarker != cap.marker_);
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
        row.Pending().originalConditionalRow == cap.row_ && row.Pending().originalConditionalMarker == cap.marker_;
}

void QueueInstanceTable::EndOriginalConditionalFuture(Row& row) noexcept {
    EndOriginalConditionalFuture(row, row.Pending());
}

void QueueInstanceTable::EndOriginalConditionalFuture(Row& row, ConditionalState& state) noexcept {
    if (state.originalConditionalRow == UINT32_MAX) {
        assert(state.originalConditionalMarker == 0);
        return;
    }
    assert(state.originalConditionalRow < originalConditionalCapacity_);
    const auto originalIndex = state.originalConditionalRow;
    auto& original = originalConditionalRows_[originalIndex];
    assert(original.marker == state.originalConditionalMarker &&
        (original.state == OriginalConditionalState::Prepared ||
         original.state == OriginalConditionalState::Committed) &&
        original.queue.slot < capacity_ && &rows_[original.queue.slot] == &row &&
        original.queue.generation == row.generation && original.parent.Valid());
    original.state = OriginalConditionalState::Retired;
    if (original.clockState == OriginalConditionalClockState::Unbound ||
        original.clockState == OriginalConditionalClockState::Bound)
        original.clockState = OriginalConditionalClockState::Retired;
    state.originalConditionalRow = UINT32_MAX;
    state.originalConditionalMarker = 0;
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
    for (auto& row : rows_) {
        auto* state = &row.Pending();
        row.conditionalHead.store(&row.initialConditional, std::memory_order_release);
        while (state != nullptr) {
            auto* next = state->following.load(std::memory_order_acquire);
            state->following.store(nullptr, std::memory_order_relaxed);
            state->pendingVersion.store(0, std::memory_order_relaxed);
            if (state->originalConditionalRow != UINT32_MAX) {
                originalConditionalRows_[state->originalConditionalRow].predecessor.Release();
                EndOriginalConditionalFuture(row, *state);
            }
            state = next;
        }
    }
}

void QueueInstanceTable::EndOriginalConditionalsForQuiescentOwnerTeardown() noexcept {
    EndOriginalConditionalAssociationsAfterQuiescence();
    for (auto& original : originalConditionalRows_) {
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
    original.predecessor.Release();
    original.body = nullptr;
    original.activated.store(false, std::memory_order_relaxed);
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

bool QueueInstanceTable::PrepareFollowingConditional(
    QueueInstanceToken token, const ConditionalQueuePrepareRequest& request,
    const OriginalConditionalFutureReservation& predecessor,
    const ContinuationSourceBinding& source,
    PreparedQueueConditionalV1* out, const OriginalLifetimePin& parentOriginal,
    OriginalConditionalFutureReservation* outOriginal) noexcept {
    return PrepareConditionalInternal(token, request, out, &parentOriginal, outOriginal,
        &predecessor, &source);
}

bool QueueInstanceTable::PrepareConditionalInternal(
    QueueInstanceToken token, const ConditionalQueuePrepareRequest& request,
    PreparedQueueConditionalV1* out, const OriginalLifetimePin* parentOriginal,
    OriginalConditionalFutureReservation* outOriginal,
    const OriginalConditionalFutureReservation* predecessor,
    const ContinuationSourceBinding* source) noexcept {
    const auto* sourceArms = source == nullptr ? nullptr : source->arms_;
    const auto sourceArm = source == nullptr ? TimelineArmToken{} : source->arm_;
    OriginalConditionalFutureReservation predecessorLoan;
    ConditionalState* prior = nullptr;
    if (predecessor != nullptr) {
        if (outOriginal == nullptr || source == nullptr || !source->Valid() ||
            !SameOccurrence(source->occurrence_, request.expectedCurrentOccurrence) ||
            sourceArms == nullptr || !sourceArm.Valid() ||
            !MatchesOriginalConditionalFutureReservation(*predecessor)) return false;
        auto& original = originalConditionalRows_[predecessor->row_];
        auto* live = Resolve(token);
        TimelineArmObservation arm{};
        const bool selected = live != nullptr && IsAcceptedSelectedConditionalHead(*live, original);
        const auto version = selected ? original.selectedVersion : original.version;
        const auto successor = selected ? original.selectedSuccessor : original.successor;
        const auto decision = selected ? original.selectedDecision : original.decision;
        if (live == nullptr || original.state != OriginalConditionalState::Committed ||
            (original.selectedFutureConsumed && !selected) || original.queue.slot != token.slot ||
            original.queue.generation != token.generation || original.body == nullptr ||
            FindConditional(*live, version) != original.body ||
            original.body->following.load(std::memory_order_acquire) != nullptr ||
            !SameOccurrence(successor, request.expectedCurrentOccurrence) ||
            request.completionIsLogicalBoundary || request.completionHasLogicalFloor ||
            !IsBoundKnownCompletionPredicate(request.completion) ||
            request.completion.owner.kind != CompletionOwnerKind::SingleQueue ||
            request.completion.renderGeneration != sourceArm.renderGeneration ||
            !sourceArms->Observe(sourceArm, &arm) ||
            !SameDecision(arm.parentGroup.decision, decision) ||
            (arm.state != ArmState::Prepared && arm.state != ArmState::PendingApply &&
             arm.state != ArmState::Armed && arm.state != ArmState::Published &&
             arm.state != ArmState::AudioClaimed && arm.state != ArmState::Started) || !predecessor->Borrow(&predecessorLoan)) return false;
        prior = original.body;
    }
    std::uint32_t originalIndex = UINT32_MAX;
    OriginalLifetimePin parentLoan;
    if ((parentOriginal == nullptr) != (outOriginal == nullptr)) return false;
    if (outOriginal != nullptr) {
        if (outOriginal->owner_ != nullptr ||
            nextOriginalConditionalFactoryMarker_ == UINT64_MAX ||
            !IsOriginalLifetimePinFor(*parentOriginal, token)) return false;
        const auto available = [](const OriginalConditionalRow& original) noexcept {
            return original.state == OriginalConditionalState::Free;
        };
        const auto construct = [this](std::uint32_t slot) noexcept { return PrepareConditionalSlot(slot); };
        for (std::uint32_t index = 0; index < originalConditionalCapacity_; ++index) {
            if (!originalConditionalRows_.IsConstructed(index)) {
                if (originalConditionalRows_.PreparedUseActive()) continue;
                if (!construct(index)) return false;
            }
            if (!available(originalConditionalRows_[index])) continue;
            if (!originalConditionalRows_.ProtectBackingForAcquire(index, available, construct)) return false;
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
    if (!token.Valid() || !rows_.IsConstructed(token.slot)) {
        return false;
    }
    Row& row = rows_[token.slot];
    const SlotState slotState =
        row.slotState.load(std::memory_order_acquire);
    const auto& head = row.Pending();
    const bool followingSelectedReset = prior != nullptr &&
        originalConditionalRows_.IsConstructed(head.originalConditionalRow) &&
        IsAcceptedSelectedConditionalHead(row, originalConditionalRows_[head.originalConditionalRow]) &&
        head.pendingConsumesReset.load(std::memory_order_acquire) && row.resetIntentGeneration != 0 &&
        head.pendingResetIntent == row.resetIntentGeneration && row.resetNextVersion == head.pendingNext.version;
    if (row.generation != token.generation ||
        SignalOwnsNext(row) ||
        (slotState != SlotState::Initializing &&
         slotState != SlotState::Live) ||
        row.failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        (prior == nullptr && (row.Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
            row.Pending().originalConditionalRow != UINT32_MAX || row.Pending().originalConditionalMarker != 0)) ||
        (prior != nullptr && row.resetPending.load(std::memory_order_acquire) && !followingSelectedReset) ||
        row.resetReserved.load(std::memory_order_acquire)) {
        return false;
    }
    EffectiveQueueState current{};
    if (prior != nullptr) current = prior->pendingNext;
    else if (!LoadMirror(row, &current)) return false;
    EffectiveQueueState first{};
    std::uint32_t stepCount = 0;
    if (!ValidateProjection(request, current, row.childCount, &first, &stepCount)) {
        return false;
    }
    const bool consumesReset =
        prior == nullptr && row.resetPending.load(std::memory_order_acquire);
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
    auto& state = prior == nullptr ? row.Pending() : originalConditionalRows_[originalIndex].followingBody;
    state.following.store(nullptr, std::memory_order_relaxed);
    state.pendingVersion.store(0, std::memory_order_relaxed);
    state.sourceArms = sourceArms;
    state.sourceOccurrences = source == nullptr ? nullptr : source->occurrences_;
    state.sourceArm = sourceArm;
    state.predecessorReceipt = predecessor == nullptr ? nullptr
        : &originalConditionalRows_[predecessor->row_].activated;
    state.predecessorDecision = prior == nullptr ? DecisionToken{} : prior->pendingDecision;
    state.pendingDecision = request.rootDecision;
    state.pendingExpectedCurrent = request.expectedCurrentOccurrence;
    state.pendingSuccessor = request.successorOccurrence;
    state.pendingCompletion = request.completion;
    state.pendingCompletionOwner = owner;
    state.pendingCompletionIsLogicalBoundary =
        request.completionIsLogicalBoundary;
    state.pendingCompletionHasLogicalFloor =
        request.completionHasLogicalFloor;
    state.pendingLogicalCompletionFrame = request.logicalCompletionFrame;
    state.pendingRetiredSource = request.retiredSource;
    state.pendingNext = request.next;
    row.nextStateOwner = NextStateOwner::RealConditional;
    state.pendingStepCount = stepCount;
    state.pendingActivation.store(ConditionalQueueActivation::Waiting,
                                std::memory_order_relaxed);
    state.pendingConsumesReset.store(consumesReset,
                                   std::memory_order_relaxed);
    state.pendingResetIntent = consumesReset ? row.resetIntentGeneration : 0;
    if (outOriginal != nullptr) {
        auto& original = originalConditionalRows_[originalIndex];
        assert(original.state == OriginalConditionalState::Free &&
            original.loans == 0 && !original.parent.Valid() &&
            original.clockState == OriginalConditionalClockState::Absent &&
            original.clockPool == nullptr && !original.clockGroups[0].Valid() &&
            !original.clockGroups[1].Valid());
        original.parent = std::move(parentLoan);
        original.predecessor = std::move(predecessorLoan);
        original.body = &state;
        original.activated.store(false, std::memory_order_relaxed);
        original.queue = token;
        original.version = {request.next.version};
        original.world = row.world;
        original.rootInstance = row.rootInstance;
        original.programSemanticFingerprint = row.programSemanticFingerprint;
        original.decision = state.pendingDecision;
        original.current = state.pendingExpectedCurrent;
        original.successor = state.pendingSuccessor;
        original.completion = state.pendingCompletion;
        original.completionOwner = state.pendingCompletionOwner;
        original.logicalBoundary = state.pendingCompletionIsLogicalBoundary;
        original.logicalFloor = state.pendingCompletionHasLogicalFloor;
        original.logicalCompletionFrame = state.pendingLogicalCompletionFrame;
        original.retiredSource = state.pendingRetiredSource;
        original.currentState = current;
        original.next = {state.pendingNext.version};
        original.first = {first.version};
        original.stepCount = state.pendingStepCount;
        original.resetIntent = state.pendingResetIntent;
        original.consumesReset = consumesReset;
        original.marker = ++nextOriginalConditionalFactoryMarker_;
        original.loans = 1;
        original.state = OriginalConditionalState::Prepared;
        state.originalConditionalRow = originalIndex;
        state.originalConditionalMarker = original.marker;
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
    out->originalRow_ = state.originalConditionalRow;
    out->originalMarker_ = state.originalConditionalMarker;
    return true;
}

bool QueueInstanceTable::CommitPreparedConditional(
    PreparedQueueConditionalV1* prepared,
    ConditionalQueueVersionToken* outVersion) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || outVersion == nullptr ||
        !prepared->queue_.Valid() || !prepared->version_.Valid() ||
        !rows_.IsConstructed(prepared->queue_.slot)) {
        return false;
    }
    Row& row = rows_[prepared->queue_.slot];
    if (!MatchesPreparedOriginalConditional(*prepared, row)) return false;
    auto& state = prepared->originalRow_ == UINT32_MAX ? row.Pending()
        : *originalConditionalRows_[prepared->originalRow_].body;
    const SlotState slotState =
        row.slotState.load(std::memory_order_acquire);
    if (row.generation != prepared->queue_.generation ||
        (slotState != SlotState::Initializing &&
         slotState != SlotState::Live) ||
        !row.conditionalReserved.load(std::memory_order_acquire) ||
        state.pendingVersion.load(std::memory_order_acquire) != 0 ||
        state.pendingNext.version != prepared->version_.version ||
        !MatchesPreparedOriginalConditional(*prepared, row)) {
        return false;
    }
    if (prepared->originalRow_ != UINT32_MAX)
        originalConditionalRows_[prepared->originalRow_].state = OriginalConditionalState::Committed;
    state.pendingVersion.store(prepared->version_.version,
                             std::memory_order_release);
    if (prepared->originalRow_ != UINT32_MAX) {
        auto& original = originalConditionalRows_[prepared->originalRow_];
        if (original.predecessor.Valid()) {
            auto& previous = originalConditionalRows_[original.predecessor.row_];
            assert(previous.body != nullptr);
            previous.body->following.store(&state, std::memory_order_release);
        }
    }
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
        !rows_.IsConstructed(queue.slot) || !version.Valid() ||
        !decision.Valid() || currentOccurrence.generation == 0) return false;
    Row& row = rows_[queue.slot];
    if (row.generation != queue.generation ||
        row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.Pending().pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        row.Pending().pendingNext.version != version.version ||
        row.nextStateOwner != NextStateOwner::RealConditional ||
        row.Pending().pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting ||
        row.conditionalReserved.load(std::memory_order_acquire) ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row.resetReserved.load(std::memory_order_acquire) ||
        row.failurePhase.load(std::memory_order_acquire) !=
            QueueFailurePhase::Healthy ||
        !SameDecision(row.Pending().pendingDecision, decision) ||
        !SameOccurrence(row.Pending().pendingExpectedCurrent, currentOccurrence) ||
        HasFrontierReaders(row)) return false;
    const bool single =
        row.Pending().pendingCompletionOwner.kind == CompletionOwnerKind::SingleQueue;
    if ((single && (row.Pending().pendingCompletionIsLogicalBoundary ||
                   !IsBoundKnownCompletionPredicate(row.Pending().pendingCompletion) ||
                   !SameCompletionOwner(row.Pending().pendingCompletionOwner,
                                        row.Pending().pendingCompletion.owner) ||
                   !SameDecision(row.Pending().pendingCompletionOwner.singleDecision,
                                 decision) ||
                   !SameOccurrence(row.Pending().pendingCompletion.occurrence,
                                   currentOccurrence))) ||
        (!single && newLogicalStartFrame != 0) ||
        (!(row.Pending().pendingCompletionIsLogicalBoundary ||
           row.Pending().pendingCompletionHasLogicalFloor) &&
         newLogicalCompletionFrame != 0)) return false;
    KnownCompletionPredicateV1 completion{};
    if (single && !ReanchorKnownCompletionBeforePublication(
            row.Pending().pendingCompletion, newLogicalStartFrame,
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
        row.Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
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
        row.Pending().pendingCompletion = proof.completion_;
    row.Pending().pendingLogicalCompletionFrame =
        proof.logicalCompletionFrame_;
}

bool QueueInstanceTable::CancelPreparedConditional(
    PreparedQueueConditionalV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || !prepared->queue_.Valid() ||
        !rows_.IsConstructed(prepared->queue_.slot)) {
        return false;
    }
    Row& row = rows_[prepared->queue_.slot];
    if (!MatchesPreparedOriginalConditional(*prepared, row)) return false;
    auto& state = prepared->originalRow_ == UINT32_MAX ? row.Pending()
        : *originalConditionalRows_[prepared->originalRow_].body;
    const SlotState slotState =
        row.slotState.load(std::memory_order_acquire);
    if (row.generation != prepared->queue_.generation ||
        (slotState != SlotState::Initializing &&
         slotState != SlotState::Live) ||
        !row.conditionalReserved.load(std::memory_order_acquire) ||
        state.pendingVersion.load(std::memory_order_acquire) != 0 ||
        !MatchesPreparedOriginalConditional(*prepared, row)) {
        return false;
    }
    const bool isHead = &state == &row.Pending();
    if (state.originalConditionalRow != UINT32_MAX)
        originalConditionalRows_[state.originalConditionalRow].predecessor.Release();
    EndOriginalConditionalFuture(row, state);
    state.pendingDecision = {};
    state.pendingExpectedCurrent = {};
    state.pendingSuccessor = {};
    state.pendingCompletion = {};
    state.pendingCompletionOwner = {};
    state.pendingCompletionIsLogicalBoundary = false;
    state.pendingCompletionHasLogicalFloor = false;
    state.pendingLogicalCompletionFrame = 0;
    state.pendingRetiredSource = {};
    state.pendingNext = {};
    if (isHead) row.nextStateOwner = NextStateOwner::None;
    state.pendingActivation.store(ConditionalQueueActivation::Waiting,
                                std::memory_order_relaxed);
    state.pendingConsumesReset.store(false, std::memory_order_relaxed);
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
    PreparedQueueConditionalReplacementV1* out, const QueueContinuationLane* stagedOwner) noexcept {
    return PrepareConditionalReplacementInternal(token, expectedVersion, expectedDecision,
        replacement, out, false, stagedOwner);
}

QueueResetStatusV1 QueueInstanceTable::PrepareConditionalReplacementRetainingOriginalCurrent(
    QueueInstanceToken token, ConditionalQueueVersionToken expectedVersion,
    DecisionToken expectedDecision, const ConditionalQueuePrepareRequest& replacement,
    OccurrenceStatePool& occurrences,
    const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& current,
    DecisionTable& decisions, const OriginalDecisionLifetimePin& currentDecision,
    PreparedQueueConditionalReplacementV1* out, const QueueContinuationLane* stagedOwner) noexcept {
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
        !SameOccurrence(row->Pending().pendingExpectedCurrent, occurrence) ||
        !SameCompletionOwner(row->Pending().pendingCompletionOwner, completion.owner) ||
        !SameCompletionOwner(row->Pending().pendingCompletion.owner, completion.owner) ||
        row->Pending().pendingCompletion.renderGeneration != completion.renderGeneration ||
        row->Pending().pendingCompletion.playbackFactsRevision != completion.playbackFactsRevision ||
        row->Pending().pendingCompletion.absoluteLogicalStartFrame != completion.absoluteLogicalStartFrame ||
        row->Pending().pendingCompletion.absoluteLogicalCompleteFrame != completion.absoluteLogicalCompleteFrame ||
        row->Pending().pendingCompletion.totalLogicalOutputFrames != completion.totalLogicalOutputFrames ||
        row->Pending().pendingCompletion.progressBaselineOutputFrames != completion.progressBaselineOutputFrames ||
        row->Pending().pendingCompletion.progressBaselineRevision != completion.progressBaselineRevision ||
        row->Pending().pendingCompletion.progressBaselineCompletedThroughBlockSequence !=
            completion.progressBaselineCompletedThroughBlockSequence ||
        !SameOccurrence(row->Pending().pendingCompletion.occurrence, completion.occurrence) ||
        !SamePlayableProgramKey(row->Pending().pendingCompletion.program, completion.program))
        return QueueResetStatusV1::InvalidInput;
    OccurrenceStatePool::OriginalSingleCompletionOwnerPin retainedOccurrence;
    OriginalDecisionLifetimePin retainedDecision;
    if (!current.Borrow(&retainedOccurrence) || !currentDecision.Borrow(&retainedDecision))
        return QueueResetStatusV1::Backpressure;
    const auto result = PrepareConditionalReplacementInternal(token, expectedVersion,
        expectedDecision, replacement, out, true, stagedOwner);
    if (result == QueueResetStatusV1::Ok) {
        out->currentOccurrence_ = std::move(retainedOccurrence);
        out->currentDecision_ = std::move(retainedDecision);
    }
    return result;
}

QueueResetStatusV1 QueueInstanceTable::PrepareConditionalReplacementInternal(
    QueueInstanceToken token, ConditionalQueueVersionToken expectedVersion,
    DecisionToken expectedDecision, const ConditionalQueuePrepareRequest& replacement,
    PreparedQueueConditionalReplacementV1* out, bool retainedCurrent,
    const QueueContinuationLane* stagedOwner) noexcept {
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
        (stagedOwner != nullptr && !stagedOwner->MatchesConditional(*this, token, expectedVersion, expectedDecision)) ||
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) !=
            expectedVersion.version ||
        row->Pending().pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting ||
        !SameDecision(row->Pending().pendingDecision, expectedDecision) ||
        !ValidateProjection(replacement, current, row->childCount, &first, &stepCount)) {
        return QueueResetStatusV1::Backpressure;
    }
    auto resetFirst = row->resetInitial;
    resetFirst.version = current.version + 1u;
    if (!SameEffectiveState(first, resetFirst)) return QueueResetStatusV1::InvalidInput;
    bool expected = false;
    if (row->stagedReplacementOwner != nullptr ||
        (stagedOwner == nullptr && !row->conditionalReplacementReserved.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) ||
        (stagedOwner != nullptr && row->conditionalReplacementReserved.load(std::memory_order_acquire))) {
        return QueueResetStatusV1::Backpressure;
    }
    row->stagedReplacementOwner = stagedOwner;
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
    out->stagedOwner_ = stagedOwner;
    out->prepared_ = true;
    return QueueResetStatusV1::Ok;
}

QueueResetStatusV1
QueueInstanceTable::CommitPreparedConditionalReplacement(
    PreparedQueueConditionalReplacementV1* prepared,
    LogicCommitSequence sequence) noexcept {
    return CommitConditionalReplacementInternal(prepared, sequence, false);
}

QueueResetStatusV1 QueueInstanceTable::CommitConditionalReplacementInternal(
    PreparedQueueConditionalReplacementV1* prepared, LogicCommitSequence sequence,
    bool acceptedStaged) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || sequence == 0) {
        return QueueResetStatusV1::InvalidInput;
    }
    Row* row = Resolve(prepared->queue_);
    EffectiveQueueState current{};
    EffectiveQueueState resetProjection{};
    if (row == nullptr ||
        (prepared->stagedOwner_ != nullptr
            ? (!acceptedStaged || row->stagedReplacementOwner != prepared->stagedOwner_)
            : !row->conditionalReplacementReserved.load(std::memory_order_acquire)) ||
        !row->resetReserved.load(std::memory_order_acquire) ||
        row->Pending().pendingVersion.load(std::memory_order_acquire) !=
            prepared->expectedVersion_.version ||
        row->Pending().pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting ||
        !SameDecision(row->Pending().pendingDecision,
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
    EndConditionalChain(*row);
    row->Pending().pendingDecision = replacement.rootDecision;
    row->Pending().pendingExpectedCurrent = replacement.expectedCurrentOccurrence;
    row->Pending().pendingSuccessor = replacement.successorOccurrence;
    row->Pending().pendingCompletion = replacement.completion;
    row->Pending().pendingCompletionOwner = replacement.completionOwner;
    row->Pending().pendingCompletionIsLogicalBoundary =
        replacement.completionIsLogicalBoundary;
    row->Pending().pendingCompletionHasLogicalFloor =
        replacement.completionHasLogicalFloor;
    row->Pending().pendingLogicalCompletionFrame = replacement.logicalCompletionFrame;
    row->Pending().pendingRetiredSource = replacement.retiredSource;
    row->Pending().pendingNext = replacement.next;
    row->Pending().pendingStepCount = prepared->projectionCount_;
    row->Pending().pendingVersion.store(replacement.next.version, std::memory_order_release);
    row->Pending().pendingConsumesReset.store(true, std::memory_order_release);
    row->Pending().pendingResetIntent = row->resetIntentCounter + 1u;
    row->conditionalReplacementReserved.store(
        false, std::memory_order_release);
    row->stagedReplacementOwner = nullptr;
    prepared->owner_ = nullptr;
    prepared->queue_ = {};
    prepared->expectedVersion_ = {};
    prepared->expectedDecision_ = {};
    prepared->replacement_ = {};
    prepared->prepared_ = false;
    prepared->stagedOwner_ = nullptr;
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
    if (row == nullptr || (prepared->stagedOwner_ != nullptr
            ? row->stagedReplacementOwner != prepared->stagedOwner_
            : !row->conditionalReplacementReserved.exchange(false, std::memory_order_acq_rel))) {
        return QueueResetStatusV1::RuntimeRecreateRequired;
    }
    row->stagedReplacementOwner = nullptr;
    prepared->owner_ = nullptr;
    prepared->queue_ = {};
    prepared->expectedVersion_ = {};
    prepared->expectedDecision_ = {};
    prepared->replacement_ = {};
    prepared->prepared_ = false;
    prepared->stagedOwner_ = nullptr;
    prepared->currentOccurrence_.Release();
    prepared->currentDecision_.Release();
    return QueueResetStatusV1::Ok;
}

bool QueueInstanceTable::ActivateConditional(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    DecisionToken expectedDecision,
    OccurrenceToken expectedCurrentOccurrence) noexcept {
    Row* row = Resolve(token);
    auto* state = row == nullptr ? nullptr : FindConditional(*row, version);
    if (row == nullptr || state == nullptr || !version.Valid() ||
        (row->frontierPool != nullptr && row->frontierPool->HasFrontierOwnerOperations(row->frontierOwner, expectedDecision)) ||
        row->conditionalReplacementReserved.load(
            std::memory_order_acquire) ||
        (*state).pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        !SameDecision((*state).pendingDecision, expectedDecision) ||
        !SameOccurrence((*state).pendingExpectedCurrent,
                        expectedCurrentOccurrence)) {
        return false;
    }
    if (!FollowingSourceAdmitted(*state)) return false;
    ConditionalQueueActivation expected =
        ConditionalQueueActivation::Waiting;
    const bool activated = (*state).pendingActivation.compare_exchange_strong(
        expected, ConditionalQueueActivation::Activated,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (activated && state->originalConditionalRow != UINT32_MAX)
        originalConditionalRows_[state->originalConditionalRow].activated.store(true, std::memory_order_release);
    return activated;
}

bool QueueInstanceTable::SettleActivated(
    QueueInstanceToken token,
    ConditionalQueueVersionToken version,
    std::uint64_t completedAudioSequence) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || !version.Valid() ||
        row->conditionalReplacementReserved.load(std::memory_order_acquire) ||
        row->Pending().pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        row->Pending().pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Activated) {
        return false;
    }
    if (row->Pending().sourceArms != nullptr) {
        TimelineArmObservation source{};
        if (!row->Pending().sourceArms->Observe(row->Pending().sourceArm, &source) ||
            source.startedBlockSequence == 0 || source.startedBlockSequence > completedAudioSequence)
            return false;
    }
    EffectiveQueueState monitorPrevious{};
    (void)LoadMirror(*row, &monitorPrevious);
    if (!StoreMirror(*row, row->Pending().pendingNext)) return false;
    MonitorLifecycleFacts facts = row->monitorIdentity;
    facts.controlCause = {};
    facts.logicCommitSequence = 0;
    facts.reasonKnown = false;
    facts.projectId = row->projectId;
    facts.programSemanticFingerprint = row->programSemanticFingerprint;
    facts.arrangingInstance = row->rootInstance;
    if (row->Pending().pendingConsumesReset.exchange(false,
                                           std::memory_order_acq_rel)) {
        facts.controlCause = MarkMonitorResetApplied(*row, row->Pending().pendingResetIntent, monitorPrevious.version, row->Pending().pendingNext.version);
        facts.logicCommitSequence = row->resetOrder.sequence;
        ConsumeResetIntent(*row, row->Pending().pendingResetIntent);
    } else facts.publishLegacy = false;
    const auto handle = (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
    ObserveMonitorLifecycle(MonitorLifecyclePhase::QueueAdvance, handle, token.generation,
        row->world, row->containerStableId, row->rootInstance, 0, 0, 0,
        row->Pending().pendingNext.version, facts);
    auto& settled = row->Pending();
    auto* following = settled.following.load(std::memory_order_acquire);
    settled.pendingResetIntent = 0;
    if (settled.originalConditionalRow != UINT32_MAX)
        originalConditionalRows_[settled.originalConditionalRow].predecessor.Release();
    settled.pendingVersion.store(0, std::memory_order_release);
    settled.following.store(nullptr, std::memory_order_release);
    if (following == nullptr) {
        row->nextStateOwner = NextStateOwner::None;
        row->initialConditional.pendingVersion.store(0, std::memory_order_relaxed);
        row->initialConditional.following.store(nullptr, std::memory_order_relaxed);
    }
    row->conditionalHead.store(following != nullptr ? following : &row->initialConditional,
        std::memory_order_release);
    EndOriginalConditionalFuture(*row, settled);
    if (following == nullptr) {
        auto& empty = row->initialConditional;
        empty.sourceArms = nullptr;
        empty.sourceOccurrences = nullptr;
        empty.predecessorReceipt = nullptr;
        empty.predecessorDecision = {};
        empty.sourceArm = {};
        empty.pendingCompletionIsLogicalBoundary = false;
        empty.pendingCompletionHasLogicalFloor = false;
        empty.pendingLogicalCompletionFrame = 0;
        empty.pendingRetiredSource = {};
        empty.pendingActivation.store(ConditionalQueueActivation::Waiting, std::memory_order_release);
    }
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
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
    MonitorLifecycleFacts facts = row->monitorIdentity;
    facts.reasonKnown = false;
    facts.controlCause = {};
    facts.logicCommitSequence = 0;
    if (consumesReset) {
        facts.controlCause = MarkMonitorResetApplied(*row, row->resetIntentGeneration, current.version, next.version);
        facts.logicCommitSequence = row->resetOrder.sequence;
        ConsumeResetIntent(*row, row->resetIntentGeneration);
    }
    const std::uint64_t handle =
        (static_cast<std::uint64_t>(token.generation) << 32) | token.slot;
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != 0 ||
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
    row->Pending().pendingNext = prepared->next_;
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
        EffectiveQueueState monitorPrevious{};
        (void)LoadMirror(*row, &monitorPrevious);
        if (!StoreMirror(*row, row->Pending().pendingNext)) return false;
        MonitorLifecycleFacts facts = row->monitorIdentity;
        facts.reasonKnown = false;
        facts.controlCause = MarkMonitorResetApplied(*row, accepted.resetIntent, monitorPrevious.version, row->Pending().pendingNext.version);
        facts.logicCommitSequence = accepted.order.sequence;
        ConsumeResetIntent(*row, accepted.resetIntent);
        const auto handle = (static_cast<std::uint64_t>(accepted.queue.generation) << 32) |
            accepted.queue.slot;
        facts.projectId = row->projectId;
        facts.programSemanticFingerprint = row->programSemanticFingerprint;
        facts.arrangingInstance = row->rootInstance;
        ObserveMonitorLifecycle(MonitorLifecyclePhase::QueueAdvance, handle,
            accepted.queue.generation, row->world, row->containerStableId,
            row->rootInstance, 0, 0, 0, row->Pending().pendingNext.version, facts);
    } else if (outcome != QueueSignalOutcomeV1::DefinitivelyNotApplied) {
        return false;
    }
    row->Pending().pendingNext = {};
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
        !rows_.IsConstructed(expectedQueue.slot) || !expectedVersion.Valid() ||
        !expectedDecision.Valid()) return false;
    Row& row = rows_[expectedQueue.slot];
    if (row.generation != expectedQueue.generation ||
        row.slotState.load(std::memory_order_acquire) != SlotState::Initializing ||
        row.Pending().pendingVersion.load(std::memory_order_acquire) != expectedVersion.version ||
        row.Pending().pendingNext.version != expectedVersion.version ||
        !SameDecision(row.Pending().pendingDecision, expectedDecision) ||
        row.Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        row.conditionalReserved.load(std::memory_order_acquire) ||
        row.conditionalReplacementReserved.load(std::memory_order_acquire)) return false;
    if (!MayMutateFrontier(row)) return false;
    EndConditionalChain(row);
    row.Pending().pendingVersion.store(0, std::memory_order_release);
    row.nextStateOwner = NextStateOwner::None;
    row.Pending().pendingCompletionIsLogicalBoundary = false;
    row.Pending().pendingCompletionHasLogicalFloor = false;
    row.Pending().pendingLogicalCompletionFrame = 0;
    row.Pending().pendingRetiredSource = {};
    row.Pending().pendingConsumesReset.store(false, std::memory_order_release);
    row.Pending().pendingActivation.store(ConditionalQueueActivation::Waiting, std::memory_order_release);
    return true;
}

bool QueueInstanceTable::RollbackUnpublishedConditional(
    const OriginalConditionalFutureReservation& cap, const QueueContinuationLane& lane) noexcept {
    if (!MatchesOriginalConditionalFutureReservation(cap) || !lane.Active() || lane.published_ ||
        lane.queues_ != this || lane.queue_.slot != cap.queue_.slot ||
        lane.queue_.generation != cap.queue_.generation) return false;
    auto& original = originalConditionalRows_[cap.row_];
    auto* row = Resolve(cap.queue_);
    auto* state = original.body;
    if (row == nullptr || state == nullptr || original.state != OriginalConditionalState::Committed ||
        !SameDecision(original.decision, lane.decision_) ||
        FindConditional(*row, original.version) != state || state->following.load(std::memory_order_acquire) != nullptr ||
        state->pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting) return false;
    if (original.predecessor.Valid()) {
        auto& prior = originalConditionalRows_[original.predecessor.row_];
        if (prior.body == nullptr || prior.body->following.load(std::memory_order_acquire) != state) return false;
        prior.body->following.store(nullptr, std::memory_order_release);
    } else {
        if (&row->Pending() != state) return false;
        row->initialConditional.pendingVersion.store(0, std::memory_order_release);
        row->initialConditional.following.store(nullptr, std::memory_order_release);
        row->conditionalHead.store(&row->initialConditional, std::memory_order_release);
        row->nextStateOwner = NextStateOwner::None;
    }
    state->pendingVersion.store(0, std::memory_order_release);
    original.predecessor.Release();
    EndOriginalConditionalFuture(*row, *state);
    return true;
}

bool QueueInstanceTable::RetireConditionalForAbort(
    QueueInstanceToken token,
    ConditionalQueueVersionToken version,
    DecisionToken expectedDecision) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || !version.Valid() ||
        row->Pending().pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        !SameDecision(row->Pending().pendingDecision, expectedDecision)) {
        return false;
    }
    if (!MayMutateFrontier(*row) || row->conditionalReplacementReserved.load(std::memory_order_acquire)) return false;
    const ConditionalQueueActivation activation =
        row->Pending().pendingActivation.load(std::memory_order_acquire);
    if (activation == ConditionalQueueActivation::Activated) {
        EffectiveQueueState monitorPrevious{};
        (void)LoadMirror(*row, &monitorPrevious);
        auto* lastActivated = &row->Pending();
        for (auto* next = lastActivated->following.load(std::memory_order_acquire);
             next != nullptr && next->pendingActivation.load(std::memory_order_acquire) == ConditionalQueueActivation::Activated;
             next = next->following.load(std::memory_order_acquire))
            lastActivated = next;
        if (!StoreMirror(*row, lastActivated->pendingNext)) return false;
        if (row->Pending().pendingConsumesReset.exchange(
                false, std::memory_order_acq_rel)) {
            (void)MarkMonitorResetApplied(*row, row->Pending().pendingResetIntent,
                monitorPrevious.version, row->Pending().pendingNext.version);
            ConsumeResetIntent(*row, row->Pending().pendingResetIntent);
        }
        row->Pending().pendingResetIntent = 0;
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
    EndConditionalChain(*row);
    row->Pending().pendingVersion.store(0, std::memory_order_release);
    row->nextStateOwner = NextStateOwner::None;
    row->Pending().pendingCompletionIsLogicalBoundary = false;
    row->Pending().pendingCompletionHasLogicalFloor = false;
    row->Pending().pendingLogicalCompletionFrame = 0;
    row->Pending().pendingRetiredSource = {};
    row->Pending().pendingConsumesReset.store(false, std::memory_order_release);
    row->Pending().pendingActivation.store(ConditionalQueueActivation::Waiting,
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
    return ProveConditionalCompletion(token, version, occurrences, composites,
        quantumStartFrame, quantumEndFrame, expectedCompletedThroughBlockSequence,
        currentQuantumScratchAdmitted, outProof, nullptr);
}

bool QueueInstanceTable::ProveSourceWaitAuthority(const FrontierGroupBinding& binding,
    AudioSliceLease& lease, const ScheduledTransactionPool& pool,
    const QueueFrontierSourceWaitContext& context,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t completedAudioSequence, SourceWaitAuthority* out,
    const FrontierInitialProof* stagedInitial) const noexcept {
    const auto blockFrames = quantumEndFrame - quantumStartFrame;
    std::uint64_t capturedQuantum = 0;
    if (out == nullptr || out->binding_ != nullptr || context.lane == nullptr || context.bucket == nullptr ||
        context.decisions == nullptr || !binding.Valid() ||
        (binding.role != FrontierGroupRole::Future &&
         !(binding.role == FrontierGroupRole::EntryMember && binding.queueConditional.Valid())) ||
        binding.variableSide.Valid() || quantumEndFrame <= quantumStartFrame || blockFrames > UINT32_MAX ||
        completedAudioSequence == UINT64_MAX || completedAudioSequence > UINT64_MAX / blockFrames ||
        quantumStartFrame != completedAudioSequence * blockFrames ||
        binding.operation.order.executeFrame / blockFrames != context.bucket->quantum ||
        !context.lane->CapturedSourceWaitQuantum(context.slot, context.bucket, &capturedQuantum) ||
        capturedQuantum != completedAudioSequence ||
        !pool.ProveImmediateLaneGroupBeforeClaim(binding.operation.group, &lease, *context.lane,
            {context.slot, context.bucket->attemptGeneration}, context.bucket)) return false;
    const auto& identity = binding.queueConditional.Valid() ? binding.queueConditional : binding.queueInitialCompletion;
    const auto* row = Resolve({identity.slot, identity.generation});
    Plan03PageGroupView group{};
    if (!identity.Valid() || row == nullptr || row->frontierPool != &pool ||
        !(row->frontierOwner == binding.operation.owner) || row->world != binding.operation.world ||
        row->rootInstance != binding.operation.owner.child ||
        row->failurePhase.load(std::memory_order_acquire) != QueueFailurePhase::Healthy ||
        row->resetReserved.load(std::memory_order_acquire) || row->resetPending.load(std::memory_order_acquire) ||
        row->conditionalReserved.load(std::memory_order_acquire) ||
        row->mirrorVersion.load(std::memory_order_acquire) != identity.currentVersion ||
        row->Pending().pendingVersion.load(std::memory_order_acquire) != identity.pendingVersion ||
        row->Pending().pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        !SameDecision(row->Pending().pendingDecision, binding.operation.group.decision) ||
        !SameOccurrence(row->Pending().pendingExpectedCurrent, identity.current) ||
        !SameOccurrence(row->Pending().pendingSuccessor, identity.successor) ||
        !pool.ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease ||
        lease.Capture().state != AudioSliceState::Ready) return false;

    const auto* initial = row->frontierInitial;
    if (initial == nullptr || !initial->queueInitial.Valid()) return false;
    Plan03PageGroupView control{};
    if (binding.canonicalInitialCompletion) {
        if (!(binding.entry == initial->entry) || !(binding.primary == initial->primary)) return false;
    } else {
        Plan03PageGroupView prerequisite{};
        if (!binding.queueConditional.Valid() || !binding.primary.Exact() ||
            !pool.ResolvePlan03GroupSliceRt(binding.primary, &control) || control.frontier == nullptr ||
            control.frontier->role != FrontierGroupRole::Coordinator ||
            !(control.frontier->entry == binding.entry) || control.frontier->prerequisite == binding.entry ||
            !control.frontier->prerequisiteMember.Exact() ||
            !pool.ResolvePlan03GroupSliceRt(control.frontier->prerequisiteMember, &prerequisite) ||
            prerequisite.frontier != initial || !(control.frontier->prerequisite == initial->entry) ||
            !(control.frontier->operation.owner == binding.operation.owner) ||
            (binding.role == FrontierGroupRole::EntryMember
                ? control.frontier->operation.order.executeFrame != binding.operation.order.executeFrame
                : !(control.frontier->operation.order < binding.operation.order))) return false;
        const auto* target = context.decisions->ResolveFrontierActivation(binding.entry.coordinator);
        if (target == nullptr || !(target->Descriptor().entry == binding.entry) ||
            !(target->Descriptor().primary == binding.primary) ||
            target->Descriptor().order != control.frontier->operation.order) return false;
    }
    if (!context.lane->CapturedEntrySourceWait(context.slot, context.bucket, binding.entry) &&
        (control.frontier == nullptr || !context.lane->CapturedEntrySourceWait(context.slot,
            context.bucket, control.frontier->prerequisite))) return false;
    auto* activation = context.decisions->ResolveFrontierActivation(initial->entry.coordinator);
    FrontierActivationReceipt receipt{};
    Plan03PageGroupView source{};
    if (stagedInitial != nullptr) {
        const auto& completion = row->Pending().pendingCompletion;
        const auto* events = context.lane->Events(context.slot, context.bucket);
        const ImmediateLaneEvent* sourceStart = nullptr;
        for (std::uint32_t index = 0; events != nullptr && index < context.bucket->eventCount; ++index) {
            const auto& original = events[index];
            const auto& event = original.event;
            if (event.frontier.entry != initial->entry) continue;
            const auto* program = event.program.program;
            const auto* payload = program != nullptr && program->payloads != nullptr &&
                event.payload < program->payloadCount ? &program->payloads[event.payload] : nullptr;
            if (payload == nullptr || payload->kind != TimelineEventKind::Start) continue;
            if (sourceStart != nullptr || original.futureGroupedStart ||
                event.frontier.group != initial->primary || !event.armToken.Valid() ||
                event.executeFrame != initial->operation.order.executeFrame ||
                program->role != TimelineProgramRole::Voice || program->generation != event.program.programGeneration ||
                !context.lane->IsEventAdmitted(context.slot, context.bucket, context.bucket->firstEvent + index)) return false;
            sourceStart = &original;
        }
        if (sourceStart == nullptr || !stagedInitial->Valid() || stagedInitial->row_ != row ||
            stagedInitial->audioSequence_ != completedAudioSequence + 1u ||
            row->initialActivationSequence.load(std::memory_order_acquire) != 0 ||
            identity.currentVersion != initial->queueInitial.version ||
            activation == nullptr || !(activation->Descriptor().entry == initial->entry) ||
            !(activation->Descriptor().primary == initial->primary) ||
            !pool.ResolvePlan03GroupSliceRt(initial->operation.group, &source) ||
            source.frontier != initial || source.lease != stagedInitial->lease_ ||
            source.lease == nullptr || source.lease->Capture().state != AudioSliceState::Ready ||
            source.memberCount != 0u || source.sourceControlCount != 0u ||
            row->Pending().pendingCompletionOwner.kind != CompletionOwnerKind::SingleQueue ||
            !IsBoundKnownCompletionPredicate(completion) || !SameOccurrence(completion.occurrence, identity.current) ||
            completion.renderGeneration != sourceStart->event.armToken.renderGeneration ||
            completion.absoluteLogicalStartFrame != initial->operation.order.executeFrame ||
            completion.absoluteLogicalCompleteFrame != binding.operation.order.executeFrame ||
            !(initial->operation.owner == binding.operation.owner) ||
            !(initial->operation.order < binding.operation.order) ||
            initial->operation.order.executeFrame / blockFrames != context.bucket->quantum ||
            context.bucket->quantum > completedAudioSequence ||
            !pool.ProveImmediateLaneGroupBeforeClaim(source.identity, source.lease, *context.lane,
                {context.slot, context.bucket->attemptGeneration}, context.bucket)) return false;
        const auto delayQuanta = completedAudioSequence - context.bucket->quantum;
        if (delayQuanta > UINT64_MAX / blockFrames) return false;
        out->delayFrames_ = delayQuanta * blockFrames;
        out->binding_ = &binding;
        return true;
    }
    if (activation == nullptr || !(activation->Descriptor().entry == initial->entry) ||
        !(activation->Descriptor().primary == initial->primary) ||
        activation->Read(initial->entry, completedAudioSequence, &receipt) != FrontierReceiptRead::Ready ||
        receipt.outcome != FrontierReceiptOutcome::Applied || receipt.origin != FrontierReceiptOrigin::Audio ||
        receipt.audioSequence == 0 || receipt.audioSequence != row->initialActivationSequence.load(std::memory_order_acquire) ||
        !context.lane->EntryApplied(context.slot, context.bucket->attemptGeneration, initial->entry) ||
        !pool.ResolvePlan03GroupSliceRt(initial->operation.group, &source) ||
        source.frontier != initial || source.lease == nullptr ||
        source.lease->Capture().state != AudioSliceState::SliceApplied ||
        !(initial->operation.owner == binding.operation.owner)) return false;
    const auto originalQuantum = receipt.order.executeFrame / blockFrames;
    if (receipt.audioSequence <= originalQuantum) return false;
    const auto delayQuanta = receipt.audioSequence - 1u - originalQuantum;
    if (delayQuanta > UINT64_MAX / blockFrames) return false;
    // Only the initially published current shares the original Entry clock.
    // Later prepared predicates already use their own actual source clock.
    if (identity.currentVersion == initial->queueInitial.version) {
        out->delayFrames_ = delayQuanta * blockFrames;
    }
    out->binding_ = &binding;
    return true;
}

QueueConditionalProofStatus QueueInstanceTable::PreflightFrontierSourceWait(
    const FrontierGroupBinding& binding, AudioSliceLease& lease,
    const ScheduledTransactionPool& pool, const QueueFrontierSourceWaitContext& context,
    const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool* composites,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t completedAudioSequence, QueueConditionalProof* outProof) const noexcept {
    if (outProof == nullptr) return QueueConditionalProofStatus::FailedIdentity;
    *outProof = {};
    const auto& identity = binding.queueConditional.Valid() ? binding.queueConditional : binding.queueInitialCompletion;
    const auto* row = Resolve({identity.slot, identity.generation});
    if (row != nullptr && (row->Pending().pendingCompletionIsLogicalBoundary || ValidRetiredSourceProof(row->Pending().pendingRetiredSource))) {
        return ProveConditionalCompletion({identity.slot, identity.generation}, {identity.pendingVersion},
            occurrences, composites == nullptr ? EmptyCompositeOwners() : *composites,
            quantumStartFrame, quantumEndFrame, completedAudioSequence, true, outProof, nullptr);
    }
    SourceWaitAuthority authority;
    if (!ProveSourceWaitAuthority(binding, lease, pool, context, quantumStartFrame,
            quantumEndFrame, completedAudioSequence, &authority)) return QueueConditionalProofStatus::FailedIdentity;
    return ProveConditionalCompletion({identity.slot, identity.generation}, {identity.pendingVersion},
        occurrences, composites == nullptr ? EmptyCompositeOwners() : *composites,
        quantumStartFrame, quantumEndFrame, completedAudioSequence, true, outProof, &authority);
}

QueueConditionalProofStatus QueueInstanceTable::ProveConditionalCompletion(
    QueueInstanceToken token, ConditionalQueueVersionToken version,
    const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool& composites,
    std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
    std::uint64_t expectedCompletedThroughBlockSequence, bool currentQuantumScratchAdmitted,
    QueueConditionalProof* outProof, const SourceWaitAuthority* sourceWait) const noexcept {
    const Row* row = Resolve(token);
    auto* state = row == nullptr ? nullptr : FindConditional(*row, version);
    if (outProof == nullptr || row == nullptr || state == nullptr || !version.Valid() ||
        (*state).pendingVersion.load(std::memory_order_acquire) !=
            version.version ||
        (*state).pendingActivation.load(std::memory_order_acquire) !=
            ConditionalQueueActivation::Waiting) {
        return QueueConditionalProofStatus::Stale;
    }
    if (state->sourceOccurrences != nullptr && state->sourceOccurrences != &occurrences)
        return QueueConditionalProofStatus::FailedIdentity;
    std::uint64_t delay = 0;
    const auto sourceClock = CaptureConditionalSourceDelay(token, version, &delay);
    if (sourceClock != QueueConditionalProofStatus::ReadyToActivate) return sourceClock;
    if (sourceWait != nullptr) {
        if (state->sourceArms != nullptr) return QueueConditionalProofStatus::FailedIdentity;
        delay = sourceWait->delayFrames_;
    }
    std::uint64_t originalCompletion = (*state).pendingCompletion.absoluteLogicalCompleteFrame;
    if ((*state).pendingCompletionOwner.kind == CompletionOwnerKind::Composite) {
        CompositeCompletionOwnerInfo info{};
        if (!composites.Capture((*state).pendingCompletionOwner.composite, &info))
            return QueueConditionalProofStatus::FailedIdentity;
        originalCompletion = info.absoluteLogicalCompleteFrame;
    }
    if ((*state).pendingCompletionHasLogicalFloor)
        originalCompletion = std::max(originalCompletion, (*state).pendingLogicalCompletionFrame);
    if (sourceWait != nullptr &&
        (sourceWait->binding_ == nullptr || (*state).pendingCompletionIsLogicalBoundary ||
         ValidRetiredSourceProof((*state).pendingRetiredSource) ||
         sourceWait->binding_->operation.order.executeFrame != originalCompletion ||
         originalCompletion > UINT64_MAX - delay)) return QueueConditionalProofStatus::FailedIdentity;
    const auto evaluate = [&](const KnownCompletionPredicateV1& predicate,
                              KnownCompletionQuantumProof* proof) {
        auto effective = predicate;
        if (delay != 0) {
            if (!IsBoundKnownCompletionPredicate(predicate) ||
                predicate.absoluteLogicalStartFrame > UINT64_MAX - delay ||
                predicate.absoluteLogicalCompleteFrame > UINT64_MAX - delay)
                return std::pair{QueueConditionalProofStatus::FailedIdentity, CompletionProofStatus::Invalid};
            effective.absoluteLogicalStartFrame += delay;
            effective.absoluteLogicalCompleteFrame += delay;
        }
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
            effective.absoluteLogicalCompleteFrame < quantumStartFrame;
        if (completeBeforeQuantum) {
            proof->actualProducedBeforeQuantum = predicate.totalLogicalOutputFrames;
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
                effective, snapshot, quantumStartFrame, quantumEndFrame,
                expectedCompletedThroughBlockSequence,
                expectedProgressRevision, currentQuantumScratchAdmitted,
                proof)};
    };
    KnownCompletionQuantumProof proof{};
    CompletionProofStatus status = CompletionProofStatus::Invalid;
    if ((*state).pendingCompletionOwner.kind == CompletionOwnerKind::None &&
        !(*state).pendingCompletionIsLogicalBoundary &&
        ValidRetiredSourceProof((*state).pendingRetiredSource)) {
        if (occurrences.StageOf((*state).pendingExpectedCurrent) !=
            OccurrenceStage::AwaitingEventFence) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        QueueConditionalProof result{};
        result.queue = token;
        result.version = version;
        result.decision = (*state).pendingDecision;
        result.currentOccurrence = (*state).pendingExpectedCurrent;
        result.successorOccurrence = (*state).pendingSuccessor;
        result.retiredSourceArm = (*state).pendingRetiredSource.sourceArm;
        result.retiredSourceRenderHandle =
            (*state).pendingRetiredSource.sourceRenderHandle;
        result.retiredSourceLogicalCompletionSequence =
            (*state).pendingRetiredSource.logicalCompletionSequence;
        result.retiredSourceTombstoneAudioSequence =
            (*state).pendingRetiredSource.tombstoneAudioSequence;
        result.completionMode =
            QueueConditionalCompletionMode::RetiredSource;
        *outProof = result;
        return QueueConditionalProofStatus::ReadyToActivateRetiredSource;
    }
    if ((*state).pendingCompletionIsLogicalBoundary) {
        if ((*state).pendingCompletionHasLogicalFloor ||
            (*state).pendingCompletionOwner.kind != CompletionOwnerKind::None ||
            (*state).pendingCompletion.owner.kind != CompletionOwnerKind::None) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        if ((*state).pendingLogicalCompletionFrame < quantumStartFrame) {
            return QueueConditionalProofStatus::MissedDeadline;
        }
        if ((*state).pendingLogicalCompletionFrame >= quantumEndFrame) {
            return QueueConditionalProofStatus::Waiting;
        }
        const std::uint64_t offset =
            (*state).pendingLogicalCompletionFrame - quantumStartFrame;
        if (offset > UINT32_MAX) {
            return QueueConditionalProofStatus::FailedProgress;
        }
        proof.activationOffset = static_cast<std::uint32_t>(offset);
        QueueConditionalProof result{};
        result.queue = token;
        result.version = version;
        result.decision = (*state).pendingDecision;
        result.currentOccurrence = (*state).pendingExpectedCurrent;
        result.successorOccurrence = (*state).pendingSuccessor;
        result.completion = proof;
        result.completionMode =
            QueueConditionalCompletionMode::LogicalBoundary;
        *outProof = result;
        return QueueConditionalProofStatus::ReadyToActivate;
    }
    if ((*state).pendingCompletionOwner.kind ==
        CompletionOwnerKind::SingleQueue) {
        if (!SameOccurrence((*state).pendingCompletion.occurrence,
                            (*state).pendingExpectedCurrent) ||
            !SameCompletionOwner((*state).pendingCompletion.owner,
                                 (*state).pendingCompletionOwner)) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        const auto evaluated = evaluate((*state).pendingCompletion, &proof);
        if (evaluated.first !=
            QueueConditionalProofStatus::ReadyToActivate) {
            return evaluated.first;
        }
        status = evaluated.second;
    } else if ((*state).pendingCompletionOwner.kind ==
               CompletionOwnerKind::Composite) {
        CompositeCompletionOwnerInfo info{};
        if (!composites.Capture(
                (*state).pendingCompletionOwner.composite, &info) ||
            !SameOccurrence(info.composedOccurrence,
                            (*state).pendingExpectedCurrent) ||
            info.leafCount == 0 || info.leafCount > 64) {
            return QueueConditionalProofStatus::FailedIdentity;
        }
        status = CompletionProofStatus::CompletesAfterQuantum;
        bool waiting = false;
        for (std::uint32_t index = 0; index < info.leafCount; ++index) {
            KnownScheduleLeafV1 leaf{};
            if (!composites.CaptureLeaf(
                    (*state).pendingCompletionOwner.composite, index, &leaf) ||
                !SameCompletionOwner(
                    leaf.predicate.owner,
                    CompletionOwnerRef::Composite(
                        (*state).pendingCompletionOwner.composite,
                        leaf.predicate.playbackFactsRevision))) {
                return QueueConditionalProofStatus::FailedIdentity;
            }
            KnownCompletionQuantumProof leafProof{};
            const auto evaluated = evaluate(leaf.predicate, &leafProof);
            if (evaluated.first == QueueConditionalProofStatus::Waiting) {
                waiting = true;
                continue;
            }
            if (evaluated.first !=
                QueueConditionalProofStatus::ReadyToActivate) {
                return evaluated.first;
            }
            if (evaluated.second == CompletionProofStatus::ProgressLost ||
                evaluated.second == CompletionProofStatus::Invalid) {
                return QueueConditionalProofStatus::FailedProgress;
            }
            if (evaluated.second ==
                CompletionProofStatus::CurrentQuantumNotAdmitted ||
                evaluated.second == CompletionProofStatus::CompletesAfterQuantum) {
                waiting = true;
                continue;
            }
            if (leaf.predicate.absoluteLogicalCompleteFrame ==
                info.absoluteLogicalCompleteFrame) {
                status = evaluated.second;
                proof = leafProof;
            }
        }
        if (waiting) return QueueConditionalProofStatus::Waiting;
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
        ValidRetiredSourceProof((*state).pendingRetiredSource);
    if ((*state).pendingCompletionHasLogicalFloor) {
        if (status != CompletionProofStatus::CompletesInQuantum &&
            !completeBeforeQuantum) {
            return QueueConditionalProofStatus::FailedProgress;
        }
        if ((*state).pendingLogicalCompletionFrame > UINT64_MAX - delay)
            return QueueConditionalProofStatus::FailedProgress;
        const auto logicalFloor = (*state).pendingLogicalCompletionFrame + delay;
        if (logicalFloor >= quantumEndFrame) {
            return QueueConditionalProofStatus::Waiting;
        }
        if (completeBeforeQuantum &&
            logicalFloor < quantumStartFrame &&
            !validRetiredSource && sourceWait == nullptr) {
            return QueueConditionalProofStatus::MissedDeadline;
        }
        if (logicalFloor >= quantumStartFrame) {
            const std::uint64_t floorOffset =
                logicalFloor - quantumStartFrame;
            if (floorOffset > UINT32_MAX) {
                return QueueConditionalProofStatus::FailedProgress;
            }
            proof.activationOffset = std::max(
                proof.activationOffset,
                static_cast<std::uint32_t>(floorOffset));
        }
    }
    const bool logicalFloorReady =
        (*state).pendingCompletionHasLogicalFloor &&
        (status == CompletionProofStatus::CompletesInQuantum ||
         completeBeforeQuantum);
    const bool retiredSourceReady =
        completeBeforeQuantum &&
        validRetiredSource;
    const bool sourceWaitReady = sourceWait != nullptr &&
        (status == CompletionProofStatus::CompletesInQuantum || completeBeforeQuantum);
    if (status == CompletionProofStatus::CompletesInQuantum ||
        logicalFloorReady ||
        retiredSourceReady || sourceWaitReady) {
        QueueConditionalProof result{};
        result.queue = token;
        result.version = version;
        result.decision = (*state).pendingDecision;
        result.currentOccurrence = (*state).pendingExpectedCurrent;
        result.successorOccurrence = (*state).pendingSuccessor;
        result.retiredSourceArm = (*state).pendingRetiredSource.sourceArm;
        result.retiredSourceRenderHandle =
            (*state).pendingRetiredSource.sourceRenderHandle;
        result.retiredSourceLogicalCompletionSequence =
            (*state).pendingRetiredSource.logicalCompletionSequence;
        result.retiredSourceTombstoneAudioSequence =
            (*state).pendingRetiredSource.tombstoneAudioSequence;
        result.completion = proof;
        result.completionMode =
            sourceWaitReady ? QueueConditionalCompletionMode::SourceWait : retiredSourceReady
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) !=
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
    if (!row->Pending().pendingActivation.compare_exchange_strong(
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
    MonitorLifecycleFacts facts = row->monitorIdentity;
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
        row->Pending().pendingVersion.load(std::memory_order_acquire) != 0) {
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
    MonitorLifecycleFacts facts = row->monitorIdentity;
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
