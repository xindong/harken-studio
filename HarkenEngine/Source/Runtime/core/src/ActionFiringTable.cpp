#include "harken/core/ActionFiringTable.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>

namespace harken {

namespace {

bool SamePackLease(const PackPartitionLease& left,
                   const PackPartitionLease& right) noexcept {
    return left.handle == right.handle &&
           left.packStableId == right.packStableId &&
           left.contentRevision == right.contentRevision &&
           left.partitionRevision == right.partitionRevision &&
           left.resourceRevision == right.resourceRevision &&
           left.resourceToken == right.resourceToken &&
           left.resourceClosureToken == right.resourceClosureToken &&
           left.compositeGeneration == right.compositeGeneration &&
           left.slotGeneration == right.slotGeneration &&
           left.leaseToken == right.leaseToken;
}

bool IsTerminalStep(ActionStepState state) noexcept {
    return state == ActionStepState::Succeeded ||
           state == ActionStepState::NoOp ||
           state == ActionStepState::Suppressed ||
           state == ActionStepState::Failed ||
           state == ActionStepState::Cancelled;
}

std::atomic<std::uint64_t> g_nextActionFiringGeneration{1};

std::uint64_t NextActionFiringGeneration() noexcept {
    std::uint64_t current =
        g_nextActionFiringGeneration.load(std::memory_order_relaxed);
    while (current <= kMaxActionFiringGeneration) {
        if (g_nextActionFiringGeneration.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

} // namespace

bool ActionFiringOwnerStamp::Valid() const noexcept {
    if (kind == ActionFiringOwnerKind::DirectWorld) {
        return worldGeneration != 0 &&
               SamePackLease(pack, PackPartitionLease{});
    }
    if (kind != ActionFiringOwnerKind::Pack) {
        return false;
    }
    const PackPartitionLease& lease = pack;
    return lease.handle != 0 && lease.packStableId != 0 &&
           lease.contentRevision != 0 && lease.partitionRevision != 0 &&
           lease.resourceRevision != 0 && lease.resourceToken != 0 &&
           lease.resourceClosureToken != 0 && lease.slotGeneration != 0 &&
           lease.compositeGeneration != 0 &&
           static_cast<std::uint32_t>(lease.handle >> 32) ==
               lease.slotGeneration &&
           lease.leaseToken != 0;
}

bool operator==(const ActionFiringOwnerStamp& left,
                const ActionFiringOwnerStamp& right) noexcept {
    return left.kind == right.kind &&
           left.projectId == right.projectId &&
           left.worldGeneration == right.worldGeneration &&
           SamePackLease(left.pack, right.pack);
}

bool PreparedActionFiring::Valid() const noexcept {
    return owner_ != nullptr && prepared_ && owner_->IsPrepared(handle_);
}

std::uint64_t PreparedActionFiring::InheritedSequence() const noexcept {
    return owner_ == nullptr ? 0 : owner_->InheritedSequence(*this);
}

std::uint64_t ActionFiringTable::InheritedSequence(const PreparedActionFiring& child) const noexcept {
    if (child.owner_ != this || !child.Valid()) return 0;
    const auto& row = rows_[SlotOf(child.handle_)];
    PreparedChildIdentity identity;
    return CapturePreparedChildIdentity(child, row.cancellationEpoch, &identity) ? identity.Sequence() : 0;
}

bool ActionFiringTable::CapturePreparedChildIdentity(const PreparedActionFiring& prepared,
    std::uint64_t originalCancellationEpoch, PreparedChildIdentity* out) const noexcept {
    if (out == nullptr || out->HasPreparation() || prepared.owner_ != this || !prepared.Valid() ||
        originalCancellationEpoch == 0) return false;
    const auto& row = rows_[SlotOf(prepared.handle_)];
    PreparedChildIdentity identity;
    identity.owner_ = this;
    identity.child_ = prepared.handle_;
    identity.parent_ = row.parent;
    identity.root_ = row.root;
    identity.epoch_ = originalCancellationEpoch;
    identity.rootEpoch_ = row.rootCancellationEpoch;
    identity.sequence_ = row.acceptedLogicSequence;
    if (!IsPreparedChildActive(identity)) return false;
    *out = identity;
    return true;
}

bool ActionFiringTable::MatchesChildIdentity(const PreparedChildIdentity& identity, bool prepared) const noexcept {
    if (identity.owner_ != this || identity.child_ == 0 || identity.parent_ == 0 || identity.root_ == 0 ||
        identity.sequence_ == 0 || identity.epoch_ == 0 || identity.rootEpoch_ == 0) return false;
    const auto slot = SlotOf(identity.child_);
    if (slot >= rows_.ConstructedCount()) return false;
    const auto& child = rows_[slot];
    const auto* parent = Resolve(identity.parent_);
    const auto* root = Resolve(identity.root_);
    return child.generation == GenerationOf(identity.child_) &&
        child.state == (prepared ? SlotState::Prepared : SlotState::Active) &&
        child.parent == identity.parent_ && child.root == identity.root_ &&
        child.cancellationEpoch == identity.epoch_ && child.rootCancellationEpoch == identity.rootEpoch_ &&
        child.acceptedLogicSequence == identity.sequence_ && parent != nullptr && root != nullptr &&
        parent->state == SlotState::Active && parent->root == identity.root_ && root->state == SlotState::Active &&
        parent->cancellationEpoch == identity.epoch_ && root->cancellationEpoch == identity.rootEpoch_ &&
        parent->acceptedLogicSequence == identity.sequence_ && root->acceptedLogicSequence == identity.sequence_;
}

bool ActionFiringTable::IsPreparedChildActive(const PreparedChildIdentity& identity) const noexcept {
    return MatchesChildIdentity(identity, true);
}

bool ActionFiringTable::IsChildActivationCurrent(const ChildActivationReceipt& receipt) const noexcept {
    return MatchesChildIdentity(receipt.identity_, false);
}

ActionFiringHandle ActionFiringTable::MakeHandle(
    std::uint32_t slot, std::uint64_t generation) noexcept {
    return (generation << kActionFiringSlotBits) | slot;
}

std::uint32_t ActionFiringTable::SlotOf(
    ActionFiringHandle handle) noexcept {
    return static_cast<std::uint32_t>(
        handle & (kMaxActionFiringsHard - 1u));
}

std::uint64_t ActionFiringTable::GenerationOf(
    ActionFiringHandle handle) noexcept {
    return handle >> kActionFiringSlotBits;
}

bool ActionFiringTable::Reset(std::uint32_t capacity) {
    if (memguard::IsAudioTagged() || capacity == 0 || capacity > kMaxActionFiringsHard) {
        return false;
    }
    StableRowStorage<Row> rows;
    if (!rows.Reserve(capacity)) return false;
    rows_.Swap(rows);
    liveCount_ = 0;
    highWater_ = 0;
    quarantinedCount_ = 0;
    refusalCount_ = 0;
    failResultAllocationForTests_ = false;
    return true;
}

bool ActionFiringTable::PrepareResults(Row& row, std::uint32_t count) noexcept {
    if (memguard::IsAudioTagged() || (failResultAllocationForTests_ && count != 0)) {
        ++refusalCount_;
        return false;
    }
    try {
        std::vector<ActionFiringStepResult> results(count);
        row.results.swap(results);
        return true;
    } catch (const std::bad_alloc&) {
        ++refusalCount_;
        return false;
    }
}

std::uint64_t ActionFiringTable::ResultStorageBytes() const noexcept {
    std::uint64_t bytes = 0;
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot)
        bytes += rows_[slot].results.capacity() * sizeof(ActionFiringStepResult);
    return bytes;
}

ActionFiringTable::Row* ActionFiringTable::Resolve(
    ActionFiringHandle handle) noexcept {
    if (handle == 0) {
        return nullptr;
    }
    const std::uint32_t slot = SlotOf(handle);
    if (slot >= rows_.ConstructedCount()) {
        return nullptr;
    }
    Row& row = rows_[slot];
    if (row.generation != GenerationOf(handle) ||
        row.state == SlotState::Free || row.state == SlotState::Prepared ||
        row.state == SlotState::Quarantined) {
        return nullptr;
    }
    return &row;
}

const ActionFiringTable::Row* ActionFiringTable::Resolve(
    ActionFiringHandle handle) const noexcept {
    return const_cast<ActionFiringTable*>(this)->Resolve(handle);
}

ActionFiringTable::Row* ActionFiringTable::ResolvePrepared(
    const PreparedActionFiring& prepared) noexcept {
    if (!prepared.Valid() || prepared.owner_ != this) {
        return nullptr;
    }
    const std::uint32_t slot = SlotOf(prepared.handle_);
    if (slot >= rows_.ConstructedCount()) {
        return nullptr;
    }
    Row& row = rows_[slot];
    return row.state == SlotState::Prepared &&
                   row.generation == GenerationOf(prepared.handle_)
               ? &row
               : nullptr;
}

bool ActionFiringTable::SetPreparedProgramFingerprint(
    const PreparedActionFiring& prepared, std::uint64_t fingerprint) noexcept {
    auto* row = ResolvePrepared(prepared);
    if (row == nullptr || fingerprint == 0) return false;
    row->programSemanticFingerprint = fingerprint;
    return true;
}

bool ActionFiringTable::PrepareRoot(
    VnObjectId actionId, std::uint64_t worldGeneration,
    std::uint32_t directStepCount, PreparedActionFiring* out,
    const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin) noexcept {
    return PrepareRoot(actionId,
                       ActionFiringOwnerStamp::DirectWorld(worldGeneration),
                       directStepCount, out, request, emitterOrigin);
}

bool ActionFiringTable::PrepareRoot(
    VnObjectId actionId, const ActionFiringOwnerStamp& owner,
    std::uint32_t directStepCount, PreparedActionFiring* out,
    const MonitorRequestContext& request, const MonitorEmitterOrigin& emitterOrigin) noexcept {
    if (memguard::IsAudioTagged() || out == nullptr || actionId == VnObjectId{} || !owner.Valid() ||
        directStepCount > kMaxActionSteps) {
        return false;
    }
    *out = {};
    for (std::uint32_t slot = 0; slot < rows_.Capacity(); ++slot) {
        if (!rows_.ConstructThrough(slot + 1)) {
            ++refusalCount_;
            return false;
        }
        Row& row = rows_[slot];
        if (row.state != SlotState::Free) {
            continue;
        }
        if (row.generation == 0) {
            row.generation = NextActionFiringGeneration();
            if (row.generation == 0) {
                row.state = SlotState::Quarantined;
                ++quarantinedCount_;
                continue;
            }
        }
        if (!PrepareResults(row, directStepCount)) return false;
        row.state = SlotState::Prepared;
        row.playbackRequest = request;
        row.emitterOrigin = emitterOrigin;
        row.controlCause = {};
        row.actionId = actionId;
        row.owner = owner;
        row.worldGeneration =
            owner.kind == ActionFiringOwnerKind::DirectWorld
                ? owner.worldGeneration
                : owner.pack.compositeGeneration;
        row.cancellationEpoch = 1;
        row.rootCancellationEpoch = 1;
        row.acceptedLogicSequence = 0;
        row.outcome = ActionFiringOutcome::Pending;
        row.terminalReason = ActionTerminalReason::None;
        row.terminalResult = 0;
        row.parent = 0;
        row.root = MakeHandle(slot, row.generation);
        row.firstChild = 0;
        row.nextSibling = 0;
        row.directStepCount = directStepCount;
        row.directStepTerminalCount = 0;
        row.childFiringCount = 0;
        row.livePlayableCount = 0;
        out->owner_ = this;
        out->handle_ = MakeHandle(slot, row.generation);
        out->prepared_ = true;
        return true;
    }
    ++refusalCount_;
    return false;
}

bool ActionFiringTable::PrepareChild(
    const PreparedActionFiring& parent, VnObjectId actionId,
    std::uint64_t worldGeneration, std::uint32_t directStepCount,
    PreparedActionFiring* out) noexcept {
    return PrepareChild(
        parent, actionId,
        ActionFiringOwnerStamp::DirectWorld(worldGeneration),
        directStepCount, out);
}

bool ActionFiringTable::PrepareChild(
    const PreparedActionFiring& parent, VnObjectId actionId,
    const ActionFiringOwnerStamp& owner, std::uint32_t directStepCount,
    PreparedActionFiring* out) noexcept {
    Row* parentRow = ResolvePrepared(parent);
    if (memguard::IsAudioTagged() || out == nullptr || parentRow == nullptr || actionId == VnObjectId{} ||
        !owner.Valid() || directStepCount > kMaxActionSteps) {
        return false;
    }
    *out = {};
    for (std::uint32_t slot = 0; slot < rows_.Capacity(); ++slot) {
        if (!rows_.ConstructThrough(slot + 1)) {
            ++refusalCount_;
            return false;
        }
        Row& row = rows_[slot];
        if (row.state != SlotState::Free) {
            continue;
        }
        if (row.generation == 0) {
            row.generation = NextActionFiringGeneration();
            if (row.generation == 0) {
                row.state = SlotState::Quarantined;
                ++quarantinedCount_;
                continue;
            }
        }
        const ActionFiringHandle handle = MakeHandle(slot, row.generation);
        if (!PrepareResults(row, directStepCount)) return false;
        row.state = SlotState::Prepared;
        row.playbackRequest = parentRow->playbackRequest;
        row.emitterOrigin = parentRow->emitterOrigin;
        row.controlCause = {};
        row.actionId = actionId;
        row.owner = owner;
        row.worldGeneration =
            owner.kind == ActionFiringOwnerKind::DirectWorld
                ? owner.worldGeneration
                : owner.pack.compositeGeneration;
        row.cancellationEpoch = 1;
        row.rootCancellationEpoch = parentRow->rootCancellationEpoch;
        row.acceptedLogicSequence = 0;
        row.outcome = ActionFiringOutcome::Pending;
        row.terminalReason = ActionTerminalReason::None;
        row.terminalResult = 0;
        row.parent = parent.handle_;
        row.root = parentRow->root;
        row.firstChild = 0;
        row.nextSibling = parentRow->firstChild;
        row.directStepCount = directStepCount;
        row.directStepTerminalCount = 0;
        row.childFiringCount = 0;
        row.livePlayableCount = 0;
        parentRow->firstChild = handle;
        ++parentRow->childFiringCount;
        out->owner_ = this;
        out->handle_ = handle;
        out->prepared_ = true;
        return true;
    }
    ++refusalCount_;
    return false;
}

bool ActionFiringTable::PrepareChild(
    ActionFiringHandle parent, VnObjectId actionId,
    const ActionFiringOwnerStamp& owner, std::uint32_t directStepCount,
    PreparedActionFiring* out) noexcept {
    Row* parentRow = Resolve(parent);
    const Row* rootRow = parentRow == nullptr ? nullptr : Resolve(parentRow->root);
    if (memguard::IsAudioTagged() || out == nullptr || parentRow == nullptr || actionId == VnObjectId{} ||
        !owner.Valid() || directStepCount > kMaxActionSteps ||
        rootRow == nullptr || rootRow->state != SlotState::Active ||
        (parent != parentRow->root && rootRow->cancellationEpoch != parentRow->rootCancellationEpoch) ||
        (parentRow->state != SlotState::Active &&
         parentRow->state != SlotState::Stopping)) {
        return false;
    }
    *out = {};
    for (std::uint32_t slot = 0; slot < rows_.Capacity(); ++slot) {
        if (!rows_.ConstructThrough(slot + 1)) {
            ++refusalCount_;
            return false;
        }
        Row& row = rows_[slot];
        if (row.state != SlotState::Free) {
            continue;
        }
        if (row.generation == 0) {
            row.generation = NextActionFiringGeneration();
            if (row.generation == 0) {
                row.state = SlotState::Quarantined;
                ++quarantinedCount_;
                continue;
            }
        }
        const ActionFiringHandle handle = MakeHandle(slot, row.generation);
        if (!PrepareResults(row, directStepCount)) return false;
        row.state = SlotState::Prepared;
        row.playbackRequest = parentRow->playbackRequest;
        row.emitterOrigin = parentRow->emitterOrigin;
        row.controlCause = {};
        row.actionId = actionId;
        row.owner = owner;
        row.worldGeneration =
            owner.kind == ActionFiringOwnerKind::DirectWorld
                ? owner.worldGeneration
                : owner.pack.compositeGeneration;
        row.cancellationEpoch = parentRow->cancellationEpoch;
        row.rootCancellationEpoch = rootRow->cancellationEpoch;
        row.acceptedLogicSequence = parentRow->acceptedLogicSequence;
        row.outcome = ActionFiringOutcome::Pending;
        row.terminalReason = ActionTerminalReason::None;
        row.terminalResult = 0;
        row.parent = parent;
        row.root = parentRow->root;
        row.firstChild = 0;
        row.nextSibling = parentRow->firstChild;
        row.directStepCount = directStepCount;
        row.directStepTerminalCount = 0;
        row.childFiringCount = 0;
        row.livePlayableCount = 0;
        parentRow->firstChild = handle;
        ++parentRow->childFiringCount;
        out->owner_ = this;
        out->handle_ = handle;
        out->prepared_ = true;
        return true;
    }
    ++refusalCount_;
    return false;
}

bool ActionFiringTable::CommitPreparedChild(
    PreparedActionFiring* prepared, ChildActivationReceipt* out) noexcept {
    if (prepared == nullptr || (out != nullptr && out->Sequence() != 0)) {
        return false;
    }
    Row* row = ResolvePrepared(*prepared);
    if (row == nullptr || row->parent == 0 ||
        row->acceptedLogicSequence == 0) {
        return false;
    }
    PreparedChildIdentity identity;
    if (!CapturePreparedChildIdentity(*prepared, row->cancellationEpoch, &identity)) {
        return false;
    }
    const ActionFiringHandle handle = prepared->handle_;
    const std::uint64_t sequence = row->acceptedLogicSequence;
    std::uint32_t committed = 0;
    if (!ActivatePreparedSubtree(handle, sequence, &committed) ||
        committed == 0) {
        return false;
    }
    prepared->prepared_ = false;
    liveCount_ += committed;
    highWater_ = (std::max)(highWater_, liveCount_);
    if (out != nullptr) out->identity_ = identity;
    return true;
}

bool ActionFiringTable::SetPreparedStep(
    const PreparedActionFiring& prepared, std::uint32_t index,
    const ActionFiringStepResult& result) noexcept {
    Row* row = ResolvePrepared(prepared);
    if (row == nullptr || index >= row->directStepCount) {
        return false;
    }
    ActionFiringStepResult& stored = row->results[index];
    const bool wasTerminal = IsTerminalStep(stored.state);
    const bool isTerminal = IsTerminalStep(result.state);
    stored = result;
    if (wasTerminal != isTerminal) {
        if (isTerminal) {
            ++row->directStepTerminalCount;
        } else {
            --row->directStepTerminalCount;
        }
    }
    return true;
}

bool ActionFiringTable::UpdateStep(
    ActionFiringHandle handle, std::uint32_t index,
    const ActionFiringStepResult& result) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || index >= row->directStepCount ||
        result.authoredEventIndex != index) {
        return false;
    }
    ActionFiringStepResult& stored = row->results[index];
    const bool wasTerminal = IsTerminalStep(stored.state);
    const bool isTerminal = IsTerminalStep(result.state);
    stored = result;
    stored.logicCommitSequence = row->acceptedLogicSequence;
    if (wasTerminal != isTerminal) {
        if (isTerminal) {
            ++row->directStepTerminalCount;
        } else {
            --row->directStepTerminalCount;
        }
    }
    return true;
}

bool ActionFiringTable::TransitionStep(
    ActionFiringHandle handle, std::uint32_t index,
    ActionStepState expected,
    const ActionFiringStepResult& result) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || index >= row->directStepCount ||
        result.authoredEventIndex != index) {
        return false;
    }
    ActionFiringStepResult& stored = row->results[index];
    if (stored.state != expected) {
        return false;
    }
    const bool wasTerminal = IsTerminalStep(stored.state);
    const bool isTerminal = IsTerminalStep(result.state);
    stored = result;
    stored.logicCommitSequence = row->acceptedLogicSequence;
    if (wasTerminal != isTerminal) {
        if (isTerminal) {
            ++row->directStepTerminalCount;
        } else {
            --row->directStepTerminalCount;
        }
    }
    return TryFinalizeSubtree(handle);
}

bool ActionFiringTable::SetPreparedDeferredStopCount(
    const PreparedActionFiring& prepared, std::uint32_t index,
    std::uint32_t capturedCount) noexcept {
    Row* row = ResolvePrepared(prepared);
    if (row == nullptr || index >= row->directStepCount) {
        return false;
    }
    ActionFiringStepResult& stored = row->results[index];
    if (stored.state != ActionStepState::AwaitingMaterialization ||
        stored.capturedPlayableCount != 0 ||
        stored.remainingPlayableCount != 0) {
        return false;
    }
    stored.capturedPlayableCount = capturedCount;
    stored.remainingPlayableCount = capturedCount;
    if (capturedCount == 0) {
        stored.state = ActionStepState::Suppressed;
        stored.result = 0;
        stored.terminalReason = ActionTerminalReason::None;
        ++row->directStepTerminalCount;
    }
    return true;
}

bool ActionFiringTable::SettleDeferredStopLeaf(
    ActionFiringHandle handle, std::uint32_t index, bool stopped,
    std::int32_t result, ActionTerminalReason reason) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->state != SlotState::Active ||
        index >= row->directStepCount) {
        return false;
    }
    ActionFiringStepResult& stored = row->results[index];
    if ((stored.state != ActionStepState::AwaitingMaterialization &&
         stored.state != ActionStepState::Active) ||
        stored.capturedPlayableCount == 0 ||
        stored.remainingPlayableCount == 0 ||
        (stopped && result != 0) ||
        (!stopped && result > 0) ||
        (result < 0 && reason == ActionTerminalReason::None)) {
        return false;
    }
    if (stopped) {
        stored.state = ActionStepState::Active;
        return true;
    }
    if (result == 0) {
        --stored.capturedPlayableCount;
    }
    if (result < 0 && stored.result >= 0) {
        stored.result = result;
        stored.terminalReason = reason;
    }
    --stored.remainingPlayableCount;
    if (stored.remainingPlayableCount == 0) {
        if (stored.result < 0) {
            stored.state = ActionStepState::Failed;
        } else if (stored.state == ActionStepState::Active) {
            stored.state = ActionStepState::Succeeded;
        } else {
            stored.state = ActionStepState::NoOp;
        }
        ++row->directStepTerminalCount;
    }
    return TryFinalize(*row);
}

bool ActionFiringTable::SetLivePlayableCount(
    ActionFiringHandle handle, std::uint32_t count) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->state == SlotState::TerminalRetained) {
        return false;
    }
    row->livePlayableCount = count;
    return true;
}

bool ActionFiringTable::AddLivePlayableCount(
    ActionFiringHandle handle, std::uint32_t count) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->state == SlotState::TerminalRetained ||
        count > UINT32_MAX - row->livePlayableCount) {
        return false;
    }
    row->livePlayableCount += count;
    return true;
}

bool ActionFiringTable::AcquireDeferred(ActionFiringHandle handle) noexcept {
    if (handle == 0 || SlotOf(handle) >= rows_.ConstructedCount()) {
        return false;
    }
    Row& candidate = rows_[SlotOf(handle)];
    if (candidate.generation != GenerationOf(handle) ||
        (candidate.state != SlotState::Prepared &&
         candidate.state != SlotState::Active &&
         candidate.state != SlotState::Stopping) ||
        candidate.ownedDeferredCount == UINT32_MAX) {
        return false;
    }
    ++candidate.ownedDeferredCount;
    return true;
}

bool ActionFiringTable::ReleaseDeferred(ActionFiringHandle handle) noexcept {
    if (handle == 0 || SlotOf(handle) >= rows_.ConstructedCount()) {
        return false;
    }
    Row& candidate = rows_[SlotOf(handle)];
    if (candidate.generation != GenerationOf(handle) ||
        (candidate.state != SlotState::Prepared &&
         candidate.state != SlotState::Active &&
         candidate.state != SlotState::Stopping) ||
        candidate.ownedDeferredCount == 0) {
        return false;
    }
    --candidate.ownedDeferredCount;
    return candidate.state == SlotState::Prepared || TryFinalize(candidate);
}

std::uint32_t ActionFiringTable::OwnedDeferredCount(
    ActionFiringHandle handle) const noexcept {
    const Row* row = Resolve(handle);
    return row == nullptr ? 0 : row->ownedDeferredCount;
}

std::uint64_t ActionFiringTable::CancellationEpoch(
    ActionFiringHandle handle) const noexcept {
    const Row* row = Resolve(handle);
    return row == nullptr ? 0 : row->cancellationEpoch;
}

bool ActionFiringTable::BumpCancellationEpoch(
    ActionFiringHandle handle, std::uint64_t* outEpoch) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->state == SlotState::TerminalRetained ||
        row->cancellationEpoch == UINT64_MAX) {
        return false;
    }
    ++row->cancellationEpoch;
    if (outEpoch != nullptr) {
        *outEpoch = row->cancellationEpoch;
    }
    return true;
}

bool ActionFiringTable::CompletePlayable(
    ActionFiringHandle handle, std::uint32_t executionOrdinal,
    std::int32_t result, ActionTerminalReason reason) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr ||
        (row->state != SlotState::Active &&
         row->state != SlotState::Stopping) ||
        row->livePlayableCount == 0) {
        return false;
    }
    const bool stopping = row->state == SlotState::Stopping;
    ActionFiringStepResult* matched = nullptr;
    for (std::uint32_t index = 0; index < row->directStepCount; ++index) {
        ActionFiringStepResult& candidate = row->results[index];
        if (candidate.executionOrdinal == executionOrdinal &&
            candidate.producedPlayableInstance != 0) {
            matched = &candidate;
            break;
        }
    }
    if (matched == nullptr ||
        (!stopping && matched->state != ActionStepState::Active) ||
        (stopping && matched->state != ActionStepState::Cancelled)) {
        return false;
    }
    if (!stopping) {
        if (result < 0) {
            if (reason == ActionTerminalReason::None ||
                reason == ActionTerminalReason::Natural ||
                reason == ActionTerminalReason::StopRequested ||
                reason == ActionTerminalReason::WorldReplaced ||
                reason == ActionTerminalReason::TransportReset) {
                return false;
            }
            matched->state = ActionStepState::Failed;
            matched->result = result;
            matched->terminalReason = reason;
        } else if (result == 0) {
            matched->state = ActionStepState::Succeeded;
            matched->result = 0;
            matched->terminalReason = ActionTerminalReason::None;
        } else {
            return false;
        }
        ++row->directStepTerminalCount;
    }
    --row->livePlayableCount;

    return TryFinalize(*row);
}

bool ActionFiringTable::CompleteAuthoredStop(
    ActionFiringHandle handle, std::uint32_t executionOrdinal) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->state != SlotState::Active) {
        return false;
    }
    ActionFiringStepResult* matched = nullptr;
    for (std::uint32_t index = 0; index < row->directStepCount; ++index) {
        ActionFiringStepResult& candidate = row->results[index];
        if (candidate.executionOrdinal == executionOrdinal &&
            candidate.capturedPlayableCount != 0) {
            matched = &candidate;
            break;
        }
    }
    if (matched == nullptr || matched->state != ActionStepState::Active ||
        matched->remainingPlayableCount == 0) {
        return false;
    }
    --matched->remainingPlayableCount;
    if (matched->remainingPlayableCount == 0) {
        matched->state = matched->result < 0
                             ? ActionStepState::Failed
                             : ActionStepState::Succeeded;
        if (matched->result >= 0) {
            matched->result = 0;
            matched->terminalReason = ActionTerminalReason::None;
        }
        ++row->directStepTerminalCount;
    }
    return TryFinalize(*row);
}

bool ActionFiringTable::PropagateTerminal(Row& row) noexcept {
    if (row.state != SlotState::TerminalRetained || row.parent == 0) {
        return row.state == SlotState::TerminalRetained;
    }
    Row* parent = Resolve(row.parent);
    if (parent == nullptr) {
        return false;
    }
    const ActionFiringHandle handle = MakeHandle(rows_.IndexOf(row), row.generation);
    ActionFiringStepResult* post = nullptr;
    for (std::uint32_t index = 0; index < parent->directStepCount; ++index) {
        ActionFiringStepResult& candidate = parent->results[index];
        if (candidate.childFiring == handle) {
            post = &candidate;
            break;
        }
    }
    if (post == nullptr) {
        return false;
    }
    if (parent->state == SlotState::TerminalRetained) {
        return IsTerminalStep(post->state);
    }
    if (parent->state != SlotState::Active &&
        parent->state != SlotState::Stopping) {
        return false;
    }
    if (!IsTerminalStep(post->state)) {
        switch (row.outcome) {
        case ActionFiringOutcome::Succeeded:
            post->state = ActionStepState::Succeeded;
            post->result = 0;
            post->terminalReason = ActionTerminalReason::None;
            break;
        case ActionFiringOutcome::SucceededWithStepFailure:
        case ActionFiringOutcome::Failed:
            post->state = ActionStepState::Failed;
            post->result = row.terminalResult;
            post->terminalReason = row.terminalReason;
            break;
        case ActionFiringOutcome::Cancelled:
            post->state = ActionStepState::Cancelled;
            post->result = row.terminalResult;
            post->terminalReason = row.terminalReason;
            break;
        case ActionFiringOutcome::Pending:
            return false;
        }
        ++parent->directStepTerminalCount;
    }
    return TryFinalize(*parent);
}

bool ActionFiringTable::TryFinalize(Row& row) noexcept {
    if (row.state == SlotState::TerminalRetained) {
        return PropagateTerminal(row);
    }
    if (row.directStepTerminalCount != row.directStepCount ||
        row.livePlayableCount != 0 || row.ownedDeferredCount != 0) {
        return true;
    }
    for (ActionFiringHandle child = row.firstChild; child != 0;) {
        Row* childRow = Resolve(child);
        if (childRow == nullptr) {
            return false;
        }
        if (childRow->state != SlotState::TerminalRetained) {
            return true;
        }
        child = childRow->nextSibling;
    }
    bool hasSuccess = false;
    bool hasFailure = false;
    bool hasCancellation = false;
    std::int32_t terminalResult = 0;
    ActionTerminalReason terminalReason = ActionTerminalReason::None;
    for (std::uint32_t index = 0; index < row.directStepCount; ++index) {
        const ActionFiringStepResult& result = row.results[index];
        if (result.state == ActionStepState::Succeeded ||
            result.state == ActionStepState::NoOp) {
            hasSuccess = true;
        } else if (result.state == ActionStepState::Failed) {
            if (!hasFailure) {
                terminalResult = result.result;
                terminalReason = result.terminalReason;
            }
            hasFailure = true;
        } else if (result.state == ActionStepState::Cancelled) {
            if (!hasCancellation) {
                terminalResult = result.result;
                terminalReason = result.terminalReason;
            }
            hasCancellation = true;
        }
    }
    const bool stopping = row.state == SlotState::Stopping;
    row.state = SlotState::TerminalRetained;
    if (stopping || hasCancellation) {
        row.outcome = ActionFiringOutcome::Cancelled;
        if (row.terminalReason == ActionTerminalReason::None) {
            row.terminalReason = terminalReason;
        }
        row.terminalResult = terminalResult;
    } else if (hasFailure) {
        row.outcome = hasSuccess
                          ? ActionFiringOutcome::SucceededWithStepFailure
                          : ActionFiringOutcome::Failed;
        row.terminalReason =
            terminalReason == ActionTerminalReason::None
                ? ActionTerminalReason::RuntimeError
                : terminalReason;
        row.terminalResult = terminalResult;
    } else {
        row.outcome = ActionFiringOutcome::Succeeded;
        row.terminalReason = ActionTerminalReason::Natural;
        row.terminalResult = 0;
    }
    return row.parent == 0 || PropagateTerminal(row);
}

bool ActionFiringTable::TryFinalizeSubtree(
    ActionFiringHandle handle) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr) {
        return false;
    }
    for (ActionFiringHandle child = row->firstChild; child != 0;) {
        Row* childRow = Resolve(child);
        if (childRow == nullptr) {
            return false;
        }
        const ActionFiringHandle next = childRow->nextSibling;
        if (!TryFinalizeSubtree(child)) {
            return false;
        }
        child = next;
    }
    return TryFinalize(*row);
}

bool ActionFiringTable::Commit(
    PreparedActionFiring* prepared,
    std::uint64_t acceptedLogicSequence) noexcept {
    return CommitTree(prepared, acceptedLogicSequence);
}

bool ActionFiringTable::CommitTree(
    PreparedActionFiring* prepared,
    std::uint64_t acceptedLogicSequence) noexcept {
    if (prepared == nullptr || acceptedLogicSequence == 0) {
        return false;
    }
    Row* row = ResolvePrepared(*prepared);
    if (row == nullptr || row->parent != 0 || row->root != prepared->handle_) {
        return false;
    }
    const ActionFiringHandle rootHandle = prepared->handle_;
    std::uint32_t committed = 0;
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        Row& candidate = rows_[slot];
        if (candidate.state == SlotState::Prepared &&
            candidate.root == prepared->handle_) {
            candidate.acceptedLogicSequence = acceptedLogicSequence;
            candidate.state = SlotState::Active;
            for (std::uint32_t index = 0;
                 index < candidate.directStepCount; ++index) {
                candidate.results[index].logicCommitSequence =
                    acceptedLogicSequence;
            }
            ++committed;
        }
    }
    prepared->prepared_ = false;
    liveCount_ += committed;
    highWater_ = (std::max)(highWater_, liveCount_);
    const bool ok = committed != 0 && TryFinalizeSubtree(rootHandle);
    if (ok) {
        MonitorLifecycleFacts facts{};
        facts.playbackRequest = row->playbackRequest;
        facts.emitterOrigin = row->emitterOrigin;
        facts.controlCause = row->controlCause;
        facts.projectId = row->owner.projectId;
        facts.parentAction = row->parent;
        facts.root = row->root;
        facts.relatedAction = 0;
        facts.programSemanticFingerprint = row->programSemanticFingerprint;
        facts.logicCommitSequence = acceptedLogicSequence;
        ObserveMonitorLifecycle(
            MonitorLifecyclePhase::ActionAccept, rootHandle,
            GenerationOf(rootHandle), row->worldGeneration, row->actionId, 0,
            rootHandle, 0, 0, 0, facts);
        Row* live = Resolve(rootHandle);
        if (live != nullptr && live->state == SlotState::TerminalRetained) {
            const bool reject =
                live->outcome == ActionFiringOutcome::Failed ||
                live->outcome == ActionFiringOutcome::Cancelled;
            MonitorLifecycleFacts terminalFacts{};
            terminalFacts.playbackRequest = live->playbackRequest;
            terminalFacts.emitterOrigin = live->emitterOrigin;
            terminalFacts.controlCause = live->controlCause;
            terminalFacts.projectId = live->owner.projectId;
            terminalFacts.parentAction = live->parent;
            terminalFacts.root = live->root;
            terminalFacts.relatedAction = rootHandle;
            terminalFacts.programSemanticFingerprint = live->programSemanticFingerprint;
            terminalFacts.logicCommitSequence = live->acceptedLogicSequence;
            ObserveMonitorLifecycle(
                reject ? MonitorLifecyclePhase::ActionReject
                       : MonitorLifecyclePhase::ActionResult,
                rootHandle, GenerationOf(rootHandle), live->worldGeneration,
                live->actionId, 0, rootHandle,
                static_cast<std::uint32_t>(live->terminalReason), 0, 0,
                terminalFacts);
        }
    }
    return ok;
}

bool ActionFiringTable::UnlinkChild(
    Row& parent, ActionFiringHandle child) noexcept {
    ActionFiringHandle* link = &parent.firstChild;
    while (*link != 0) {
        if (*link == child) {
            const std::uint32_t slot = SlotOf(child);
            if (slot >= rows_.ConstructedCount()) {
                return false;
            }
            *link = rows_[slot].nextSibling;
            rows_[slot].nextSibling = 0;
            if (parent.childFiringCount > 0) {
                --parent.childFiringCount;
            }
            return true;
        }
        Row* sibling = Resolve(*link);
        if (sibling == nullptr) {
            return false;
        }
        link = &sibling->nextSibling;
    }
    return false;
}

bool ActionFiringTable::ActivatePreparedSubtree(
    ActionFiringHandle handle, std::uint64_t acceptedLogicSequence,
    std::uint32_t* committed) noexcept {
    if (committed == nullptr || handle == 0 || acceptedLogicSequence == 0) {
        return false;
    }
    const std::uint32_t slot = SlotOf(handle);
    if (slot >= rows_.ConstructedCount()) {
        return false;
    }
    Row& row = rows_[slot];
    if (row.state != SlotState::Prepared ||
        row.generation != GenerationOf(handle)) {
        return false;
    }
    ActionFiringHandle child = row.firstChild;
    while (child != 0) {
        const std::uint32_t childSlot = SlotOf(child);
        if (childSlot >= rows_.ConstructedCount()) {
            return false;
        }
        const ActionFiringHandle next = rows_[childSlot].nextSibling;
        if (!ActivatePreparedSubtree(child, acceptedLogicSequence,
                                     committed)) {
            return false;
        }
        child = next;
    }
    row.acceptedLogicSequence = acceptedLogicSequence;
    for (std::uint32_t index = 0; index < row.directStepCount; ++index) {
        row.results[index].logicCommitSequence = acceptedLogicSequence;
    }
    row.state = SlotState::Active;
    ++*committed;
    return true;
}

bool ActionFiringTable::Recycle(std::uint32_t slot, Row& row) noexcept {
    if (memguard::IsAudioTagged()) return false;
    row.actionId = {};
    row.programSemanticFingerprint = 0;
    row.worldGeneration = 0;
    row.owner = {};
    row.cancellationEpoch = 0;
    row.rootCancellationEpoch = 0;
    row.acceptedLogicSequence = 0;
    row.outcome = ActionFiringOutcome::Pending;
    row.terminalReason = ActionTerminalReason::None;
    row.terminalResult = 0;
    row.parent = 0;
    row.root = 0;
    row.firstChild = 0;
    row.nextSibling = 0;
    row.directStepCount = 0;
    row.directStepTerminalCount = 0;
    row.childFiringCount = 0;
    row.livePlayableCount = 0;
    row.ownedDeferredCount = 0;
    std::vector<ActionFiringStepResult>{}.swap(row.results);
    if (row.generation == kMaxActionFiringGeneration) {
        row.state = SlotState::Quarantined;
        ++quarantinedCount_;
        return false;
    }
    row.generation = NextActionFiringGeneration();
    if (row.generation == 0) {
        row.state = SlotState::Quarantined;
        ++quarantinedCount_;
        return false;
    }
    row.state = SlotState::Free;
    return true;
}

bool ActionFiringTable::Abandon(PreparedActionFiring* prepared) noexcept {
    if (memguard::IsAudioTagged()) return false;
    if (prepared == nullptr) {
        return false;
    }
    Row* row = ResolvePrepared(*prepared);
    if (row == nullptr) {
        return false;
    }
    const ActionFiringHandle handle = prepared->handle_;
    if (row->parent == 0) {
        if (row->root != handle) {
            return false;
        }
        prepared->prepared_ = false;
        return AbandonPreparedSubtree(handle);
    }
    Row* parent = Resolve(row->parent);
    if (parent == nullptr || !UnlinkChild(*parent, handle)) {
        return false;
    }
    prepared->prepared_ = false;
    return AbandonPreparedSubtree(handle);
}

bool ActionFiringTable::AbandonPreparedSubtree(
    ActionFiringHandle handle) noexcept {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= rows_.ConstructedCount()) {
        return false;
    }
    Row& row = rows_[slot];
    if (row.state != SlotState::Prepared ||
        row.generation != GenerationOf(handle) ||
        row.ownedDeferredCount != 0) {
        return false;
    }
    ActionFiringHandle child = row.firstChild;
    while (child != 0) {
        const std::uint32_t childSlot = SlotOf(child);
        if (childSlot >= rows_.ConstructedCount()) {
            return false;
        }
        const ActionFiringHandle next = rows_[childSlot].nextSibling;
        if (!AbandonPreparedSubtree(child)) {
            return false;
        }
        child = next;
    }
    (void)Recycle(slot, row);
    return true;
}

bool ActionFiringTable::MarkStoppingSubtree(
    ActionFiringHandle handle, ActionTerminalReason reason,
    const MonitorControlCause& cause) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || reason == ActionTerminalReason::None) {
        return false;
    }
    if (row->state == SlotState::TerminalRetained) {
        return true;
    }
    if (row->state != SlotState::Active &&
        row->state != SlotState::Stopping) {
        return false;
    }
    // A child can finalize its parent while cancellation recurses. Capture
    // the first accepted cause before that terminal propagation publishes it.
    if (row->state == SlotState::Active) row->controlCause = cause;
    for (ActionFiringHandle child = row->firstChild; child != 0;) {
        Row* childRow = Resolve(child);
        if (childRow == nullptr) {
            return false;
        }
        const ActionFiringHandle next = childRow->nextSibling;
        if (!MarkStoppingSubtree(child, reason, cause)) {
            return false;
        }
        child = next;
    }
    for (std::uint32_t index = 0; index < row->directStepCount; ++index) {
        ActionFiringStepResult& result = row->results[index];
        if (!IsTerminalStep(result.state)) {
            result.state = ActionStepState::Cancelled;
            result.result = 0;
            result.terminalReason = reason;
            ++row->directStepTerminalCount;
        }
    }
    row->state = SlotState::Stopping;
    row->terminalReason = reason;
    return TryFinalize(*row);
}

bool ActionFiringTable::MarkStopping(
    ActionFiringHandle handle, ActionTerminalReason reason,
    const MonitorControlCause& cause) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->parent != 0 ||
        reason == ActionTerminalReason::None) {
        return false;
    }
    if (row->state == SlotState::TerminalRetained) {
        return true;
    }
    if (row->state == SlotState::Active &&
        !BumpCancellationEpoch(handle)) {
        return false;
    }
    const bool ok = MarkStoppingSubtree(handle, reason, cause);
    if (ok) {
        MonitorLifecycleFacts facts{};
        facts.playbackRequest = row->playbackRequest;
        facts.emitterOrigin = row->emitterOrigin;
        facts.controlCause = row->controlCause;
        facts.projectId = row->owner.projectId;
        facts.parentAction = row->parent;
        facts.root = row->root;
        facts.relatedAction = 0;
        facts.programSemanticFingerprint = row->programSemanticFingerprint;
        facts.logicCommitSequence = row->acceptedLogicSequence;
        ObserveMonitorLifecycle(
            MonitorLifecyclePhase::ActionStop, handle, GenerationOf(handle),
            row->worldGeneration, row->actionId, 0, handle,
            static_cast<std::uint32_t>(reason), 0, 0, facts);
    }
    return ok;
}

bool ActionFiringTable::MarkTreeStopping(
    ActionFiringHandle member, ActionTerminalReason reason,
    const MonitorControlCause& cause) noexcept {
    Row* row = Resolve(member);
    if (row == nullptr || reason == ActionTerminalReason::None) {
        return false;
    }
    const Row* root = Resolve(row->root);
    if (root == nullptr) {
        return false;
    }
    if (root->state == SlotState::TerminalRetained) {
        return true;
    }
    const ActionFiringHandle rootHandle = row->root;
    const std::uint64_t generation = GenerationOf(rootHandle);
    const std::uint64_t worldGeneration = root->worldGeneration;
    const VnObjectId actionId = root->actionId;
    if (root->state == SlotState::Active &&
        !BumpCancellationEpoch(rootHandle)) {
        return false;
    }
    const bool ok = MarkStoppingSubtree(rootHandle, reason, cause);
    if (ok) {
        MonitorLifecycleFacts facts{};
        facts.playbackRequest = root->playbackRequest;
        facts.emitterOrigin = root->emitterOrigin;
        facts.controlCause = root->controlCause;
        facts.projectId = root->owner.projectId;
        facts.parentAction = root->parent;
        facts.root = root->root;
        facts.relatedAction = rootHandle;
        facts.programSemanticFingerprint = root->programSemanticFingerprint;
        facts.logicCommitSequence = root->acceptedLogicSequence;
        ObserveMonitorLifecycle(
            MonitorLifecyclePhase::ActionStop, rootHandle, generation,
            worldGeneration, actionId, 0, rootHandle,
            static_cast<std::uint32_t>(reason), 0, 0, facts);
    }
    return ok;
}

bool ActionFiringTable::MarkTerminal(
    ActionFiringHandle handle, ActionFiringOutcome outcome,
    ActionTerminalReason reason) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->ownedDeferredCount != 0 ||
        (row->state != SlotState::Active &&
         row->state != SlotState::Stopping)) {
        return false;
    }
    ActionFiringHandle child = row->firstChild;
    while (child != 0) {
        Row* childRow = Resolve(child);
        if (childRow == nullptr ||
            childRow->state != SlotState::TerminalRetained) {
            return false;
        }
        child = childRow->nextSibling;
    }
    row->state = SlotState::TerminalRetained;
    row->outcome = outcome;
    row->terminalReason = reason;
    row->terminalResult = 0;
    for (std::uint32_t index = 0; index < row->directStepCount; ++index) {
        const ActionFiringStepResult& result = row->results[index];
        if (result.state == ActionStepState::Failed ||
            result.state == ActionStepState::Cancelled) {
            row->terminalResult = result.result;
            break;
        }
    }
    row->livePlayableCount = 0;
    const bool ok = row->parent == 0 || PropagateTerminal(*row);
    if (ok) {
        const bool reject =
            outcome == ActionFiringOutcome::Failed ||
            outcome == ActionFiringOutcome::Cancelled;
        MonitorLifecycleFacts facts{};
        facts.playbackRequest = row->playbackRequest;
        facts.emitterOrigin = row->emitterOrigin;
        facts.controlCause = row->controlCause;
        facts.projectId = row->owner.projectId;
        facts.parentAction = row->parent;
        facts.root = row->root;
        facts.relatedAction = 0;
        facts.programSemanticFingerprint = row->programSemanticFingerprint;
        facts.logicCommitSequence = row->acceptedLogicSequence;
        ObserveMonitorLifecycle(
            reject ? MonitorLifecyclePhase::ActionReject
                   : MonitorLifecyclePhase::ActionResult,
            handle, GenerationOf(handle), row->worldGeneration, row->actionId,
            0, handle, static_cast<std::uint32_t>(reason), 0, 0, facts);
    }
    return ok;
}

bool ActionFiringTable::ReleaseTerminalSubtree(
    ActionFiringHandle handle) noexcept {
    Row* row = Resolve(handle);
    if (row == nullptr || row->state != SlotState::TerminalRetained ||
        row->ownedDeferredCount != 0) {
        return false;
    }
    ActionFiringHandle child = row->firstChild;
    while (child != 0) {
        Row* childRow = Resolve(child);
        if (childRow == nullptr) {
            return false;
        }
        const ActionFiringHandle next = childRow->nextSibling;
        if (!ReleaseTerminalSubtree(child)) {
            return false;
        }
        child = next;
    }
    const std::uint32_t slot = SlotOf(handle);
    --liveCount_;
    (void)Recycle(slot, *row);
    return true;
}

bool ActionFiringTable::ReleaseRoot(ActionFiringHandle handle) noexcept {
    if (memguard::IsAudioTagged()) return false;
    Row* row = Resolve(handle);
    if (row == nullptr || row->state != SlotState::TerminalRetained ||
        row->parent != 0) {
        return false;
    }
    return ReleaseTerminalSubtree(handle);
}

bool ActionFiringTable::Capture(ActionFiringHandle handle,
                                ActionFiringSnapshot* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Row* row = Resolve(handle);
    if (row == nullptr) {
        return false;
    }
    ActionFiringSnapshot snapshot{};
    switch (row->state) {
    case SlotState::Active:
        snapshot.state = ActionFiringState::Active;
        break;
    case SlotState::Stopping:
        snapshot.state = ActionFiringState::Stopping;
        break;
    case SlotState::TerminalRetained:
        snapshot.state = ActionFiringState::TerminalRetained;
        break;
    case SlotState::Free:
    case SlotState::Prepared:
    case SlotState::Quarantined:
        return false;
    }
    snapshot.playbackRequest = row->playbackRequest;
    snapshot.emitterOrigin = row->emitterOrigin;
    snapshot.controlCause = row->controlCause;
    snapshot.root = row->root;
    snapshot.outcome = row->outcome;
    snapshot.terminalReason = row->terminalReason;
    snapshot.actionId = row->actionId;
    snapshot.programSemanticFingerprint = row->programSemanticFingerprint;
    snapshot.worldGeneration = row->worldGeneration;
    snapshot.owner = row->owner;
    snapshot.cancellationEpoch = row->cancellationEpoch;
    snapshot.ownedDeferredCount = row->ownedDeferredCount;
    snapshot.acceptedLogicSequence = row->acceptedLogicSequence;
    snapshot.parent = row->parent;
    snapshot.directStepCount = row->directStepCount;
    snapshot.directStepTerminalCount = row->directStepTerminalCount;
    snapshot.childFiringCount = row->childFiringCount;
    snapshot.livePlayableCount = row->livePlayableCount;
    *out = snapshot;
    return true;
}

ActionFiringHandle ActionFiringTable::CaptureMonitorIdentityAt(
    std::uint32_t slot, ActionFiringSnapshot* out) const noexcept {
    if (out == nullptr) return 0;
    *out = {};
    if (slot >= rows_.ConstructedCount()) return 0;
    const auto handle = MakeHandle(slot, rows_[slot].generation);
    return Capture(handle, out) ? handle : 0;
}

ActionFiringHandle ActionFiringTable::ActiveRootAt(std::uint32_t slot) const noexcept {
    if (slot >= rows_.ConstructedCount()) return 0;
    const Row& row = rows_[slot];
    if (row.parent != 0 ||
        (row.state != SlotState::Active && row.state != SlotState::Stopping)) return 0;
    return MakeHandle(slot, row.generation);
}

bool ActionFiringTable::CaptureTerminalTreeOwners(
    ActionFiringHandle root, ActionFiringOwnerStamp* outOwners,
    std::uint32_t ownerCapacity,
    std::uint32_t* outOwnerCount) const noexcept {
    if (outOwnerCount == nullptr ||
        (ownerCapacity != 0 && outOwners == nullptr)) {
        return false;
    }
    *outOwnerCount = 0;
    const Row* rootRow = Resolve(root);
    if (rootRow == nullptr || rootRow->parent != 0 ||
        rootRow->state != SlotState::TerminalRetained) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        const Row& row = rows_[slot];
        if (row.root != root || row.state == SlotState::Free ||
            row.state == SlotState::Prepared ||
            row.state == SlotState::Quarantined) {
            continue;
        }
        if (row.state != SlotState::TerminalRetained ||
            row.ownedDeferredCount != 0 || !row.owner.Valid()) {
            *outOwnerCount = 0;
            return false;
        }
        bool duplicate = false;
        for (std::uint32_t index = 0; index < *outOwnerCount; ++index) {
            if (outOwners[index] == row.owner) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (*outOwnerCount == ownerCapacity) {
            *outOwnerCount = 0;
            return false;
        }
        outOwners[(*outOwnerCount)++] = row.owner;
    }
    return *outOwnerCount != 0;
}

bool ActionFiringTable::BelongsToTree(
    ActionFiringHandle root, ActionFiringHandle member) const noexcept {
    const Row* rootRow = Resolve(root);
    const Row* memberRow = Resolve(member);
    return rootRow != nullptr && rootRow->parent == 0 &&
           memberRow != nullptr && memberRow->root == root;
}

bool ActionFiringTable::TreeLivePlayableCount(
    ActionFiringHandle root, std::uint32_t* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = 0;
    const Row* rootRow = Resolve(root);
    if (rootRow == nullptr || rootRow->parent != 0) {
        return false;
    }
    std::uint32_t total = 0;
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        const Row& row = rows_[slot];
        if ((row.state == SlotState::Active ||
             row.state == SlotState::Stopping ||
             row.state == SlotState::TerminalRetained) &&
            row.root == root) {
            if (total > UINT32_MAX - row.livePlayableCount) {
                return false;
            }
            total += row.livePlayableCount;
        }
    }
    *out = total;
    return true;
}

bool ActionFiringTable::TreeOwnedDeferredCount(
    ActionFiringHandle root, std::uint32_t* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = 0;
    const Row* rootRow = Resolve(root);
    if (rootRow == nullptr || rootRow->parent != 0) {
        return false;
    }
    std::uint32_t total = 0;
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        const Row& row = rows_[slot];
        if ((row.state == SlotState::Active ||
             row.state == SlotState::Stopping ||
             row.state == SlotState::TerminalRetained) &&
            row.root == root) {
            if (total > UINT32_MAX - row.ownedDeferredCount) {
                return false;
            }
            total += row.ownedDeferredCount;
        }
    }
    *out = total;
    return true;
}

bool ActionFiringTable::CaptureStep(
    ActionFiringHandle handle, std::uint32_t index,
    ActionFiringStepResult* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Row* row = Resolve(handle);
    if (row == nullptr || index >= row->directStepCount) {
        return false;
    }
    *out = row->results[index];
    return true;
}

std::uint32_t ActionFiringTable::FreeCount() const noexcept {
    auto free = rows_.Capacity() - rows_.ConstructedCount();
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot)
        if (rows_[slot].state == SlotState::Free) ++free;
    return free;
}

bool ActionFiringTable::SetFreeGenerationForTests(
    std::uint32_t slot, std::uint64_t generation) noexcept {
    if (slot >= rows_.Capacity() || generation == 0 ||
        generation > kMaxActionFiringGeneration ||
        !rows_.ConstructThrough(slot + 1) ||
        rows_[slot].state != SlotState::Free) {
        return false;
    }
    rows_[slot].generation = generation;
    return true;
}

bool ActionFiringTable::IsPrepared(ActionFiringHandle handle) const noexcept {
    if (handle == 0) {
        return false;
    }
    const std::uint32_t slot = SlotOf(handle);
    return slot < rows_.ConstructedCount() && rows_[slot].state == SlotState::Prepared &&
           rows_[slot].generation == GenerationOf(handle);
}

} // namespace harken
