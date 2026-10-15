#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/PrivateOriginalQueueGroupClockProof.h"
#include "harken/core/PrivateSelectedQueueFutureSpan.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>

namespace harken {

namespace {

constexpr std::uint32_t kLaneGateInvalid = 0x80000000u;
constexpr std::uint32_t kLaneGateReasonShift = 25u;
constexpr std::uint32_t kLaneGateReasonMask = 0x3fu << kLaneGateReasonShift;
constexpr std::uint32_t kLaneGateClaims = (1u << kLaneGateReasonShift) - 1u;

bool ValidateTransactionOccurrence(
    const TransactionOccurrenceRef& reference,
    PlayableInstanceHandle transactionOwner,
    std::uint64_t transactionOwnerEpoch,
    const PlayableInstanceTable& instances,
    const OccurrenceStatePool& occurrences) {
    const OccurrenceKey* key = occurrences.KeyOf(reference.token);
    if (key == nullptr) return false;
    std::uint64_t arrangingEpoch = reference.arrangingInstanceScheduleEpoch;
    if (arrangingEpoch == 0) {
        // Legacy references may omit the stamp only for the same full handle.
        if (key->arrangingInstance != transactionOwner) return false;
        arrangingEpoch = transactionOwnerEpoch;
    }
    return occurrences.ValidateTransaction(
        reference.token, reference.occurrenceScheduleEpoch,
        arrangingEpoch, instances);
}

} // namespace

PreparedTransactionBatch::~PreparedTransactionBatch() {
    if (transferSelected_ && owner_ != nullptr) owner_->DropPreparedPageBatchOriginal(this);
}

PreparedTransactionBatch::PreparedTransactionBatch(PreparedTransactionBatch&& other) noexcept {
    *this = std::move(other);
}

PreparedTransactionBatch& PreparedTransactionBatch::operator=(PreparedTransactionBatch&& other) noexcept {
    if (this == &other || Held() || other.Held()) return *this;
    if (transferSelected_ && owner_ != nullptr) owner_->DropPreparedPageBatchOriginal(this);
    const bool associated = other.transferSelected_ && other.Valid();
    owner_ = other.owner_;
    claimSlot_ = other.claimSlot_;
    generation_ = other.generation_;
    prepared_ = other.prepared_;
    transferSelected_ = other.transferSelected_;
    if (associated) owner_->preparedTransactionClaims_[claimSlot_].transferOriginal = this;
    other.owner_ = nullptr;
    other.claimSlot_ = UINT32_MAX;
    other.generation_ = 0;
    other.prepared_ = false;
    other.transferSelected_ = false;
    return *this;
}

bool PreparedTransactionBatch::Valid() const noexcept {
    return owner_ != nullptr && prepared_ &&
        (!transferSelected_ || owner_->OwnsPreparedPageBatchOriginal(*this));
}

bool PreparedTransactionBatch::Held() const noexcept {
    return transferSelected_ && owner_ != nullptr && owner_->PreparedPageBatchHeld(*this);
}

PreparedPageBatchTransferProof::PreparedPageBatchTransferProof(
    PreparedPageBatchTransferProof&& other) noexcept { *this = std::move(other); }

PreparedPageBatchTransferProof& PreparedPageBatchTransferProof::operator=(
    PreparedPageBatchTransferProof&& other) noexcept {
    if (this == &other || claim_ != nullptr || other.claim_ != nullptr) return *this;
    return *this;
}

bool PreparedPageBatchTransferProof::Valid() const noexcept {
    if (owner_ == nullptr || claim_ == nullptr || original_ == nullptr) return false;
    const auto& claim = *static_cast<const ScheduledTransactionPool::PreparedTransactionClaim*>(claim_);
    return claim.transferProof == this && claim.transferOriginal == original_ &&
        owner_->OwnsPreparedPageBatchOriginal(*original_);
}

bool PreparedPageBatchTransferProof::MatchesOriginalScope(
    const PreparedTransactionBatch& original, const PreparedPageGroupTransferScope& scope,
    const std::array<ConsumedEvent, 5>& inputs,
    const std::array<FrontierEventBinding, 5>& bindings) const noexcept {
    if (!Valid() || original_ != &original ||
        !owner_->MatchesPreparedPageBatchInput(original, scope, inputs, bindings)) return false;
    for (std::size_t index = 0; index < bindings.size(); ++index)
        if (!(bindings_[index] == bindings[index])) return false;
    // The actual row query checks every frozen input, not just copied keys.
    return owner_->MatchesPreparedPageBatchInput(original, scope, inputs_, bindings_);
}

void PreparedPageBatchTransferProof::Release() noexcept {
    if (claim_ != nullptr) {
        auto& claim = *static_cast<ScheduledTransactionPool::PreparedTransactionClaim*>(claim_);
        assert(claim.transferProof == this && claim.transferOriginal == original_);
        claim.transferProof = nullptr;
    }
    owner_ = nullptr;
    claim_ = nullptr;
    original_ = nullptr;
    inputs_ = {};
    bindings_ = {};
}

bool ScheduledTransactionPool::OwnsPreparedPageBatchOriginal(
    const PreparedTransactionBatch& original) const noexcept {
    if (!Valid() || original.owner_ != this || !original.prepared_ || !original.transferSelected_ ||
        original.claimSlot_ >= preparedTransactionClaims_.size()) return false;
    const auto& claim = preparedTransactionClaims_[original.claimSlot_];
    if (!claim.used || claim.committed || claim.generation != original.generation_ ||
        claim.transferOriginal != &original || claim.transactionCount != 5 ||
        !claim.transferScope.Valid() || claim.transferRing >= pageCount_ ||
        claim.transferTransport != epoch_) return false;
    const auto& header = pageHeaders_[claim.transferRing];
    return header.absoluteQuantumIndex == claim.transferAbsoluteQuantum &&
        header.pageGeneration == claim.transferPageGeneration && header.transportEpoch == epoch_ &&
        !header.frozen && pagePreparedEventCount_[claim.transferRing] >= 5 &&
        pagePublicationHolds_[claim.transferRing] >= 5;
}

bool ScheduledTransactionPool::PreparedPageBatchHeld(const PreparedTransactionBatch& original) const noexcept {
    return OwnsPreparedPageBatchOriginal(original) &&
        preparedTransactionClaims_[original.claimSlot_].transferProof != nullptr;
}

void ScheduledTransactionPool::SpendPreparedPageBatchOriginal(std::size_t slot) noexcept {
    auto& claim = preparedTransactionClaims_[slot];
    assert(claim.transferProof == nullptr);
    if (claim.transferOriginal == nullptr) return;
    if (claim.transferOriginal != nullptr) claim.transferOriginal->transferSelected_ = false;
    claim.transferOriginal = nullptr;
    claim.transferScope = {};
    claim.transferRing = UINT32_MAX;
    claim.transferAbsoluteQuantum = 0;
    claim.transferPageGeneration = 0;
    claim.transferTransport = kInvalidTransportEpoch;
    claim.transferFrame = 0;
    claim.transferTransactionSlots = {};
    claim.transferRowSequences = {};
    claim.transferRequestArms = {};
}

void ScheduledTransactionPool::DetachPreparedPageBatchTransfer(std::size_t slot) noexcept {
    auto& claim = preparedTransactionClaims_[slot];
    if (claim.transferProof != nullptr) claim.transferProof->Release();
    if (claim.transferOriginal != nullptr) {
        auto& original = *claim.transferOriginal;
        original.owner_ = nullptr;
        original.claimSlot_ = UINT32_MAX;
        original.generation_ = 0;
        original.prepared_ = false;
    }
    SpendPreparedPageBatchOriginal(slot);
}

void ScheduledTransactionPool::DropPreparedPageBatchOriginal(PreparedTransactionBatch* original) noexcept {
    if (original->claimSlot_ < preparedTransactionClaims_.size() &&
        preparedTransactionClaims_[original->claimSlot_].transferOriginal == original)
        DetachPreparedPageBatchTransfer(original->claimSlot_);
    original->owner_ = nullptr;
    original->claimSlot_ = UINT32_MAX;
    original->generation_ = 0;
    original->prepared_ = false;
    original->transferSelected_ = false;
}

bool ScheduledTransactionPool::MatchesPreparedPageBatchInput(
    const PreparedTransactionBatch& original, const PreparedPageGroupTransferScope& scope,
    const std::array<ConsumedEvent, 5>& inputs,
    const std::array<FrontierEventBinding, 5>& bindings) const noexcept {
    if (!OwnsPreparedPageBatchOriginal(original)) return false;
    const auto& claim = preparedTransactionClaims_[original.claimSlot_];
    if (!(claim.transferScope == scope) || claim.frozenSequence != scope.sequence) return false;
    const auto base = static_cast<std::size_t>(claim.transferRing) * config_.maxEventsPerQuantum;
    for (std::size_t position = 0; position < inputs.size(); ++position) {
        const auto slot = claim.transferTransactionSlots[position];
        if (slot >= transactions_.size()) return false;
        const auto& record = transactions_[slot];
        const auto& savedArm = claim.transferRequestArms[position];
        if (!record.prepared || record.used || record.preparedClaimSlot != original.claimSlot_ ||
            record.preparedClaimGeneration != original.generation_ || record.preparedRequestIndex != position ||
            record.epoch != claim.transferTransport || record.remainingEvents != 1 ||
            record.summary.eventCount != 1 || record.summary.pageSpan != 1 ||
            record.firstFrame != claim.transferFrame || record.lastFrame != claim.transferFrame ||
            record.occurrence.present || record.occurrence.token.slot != 0 ||
            record.occurrence.token.generation != 0 || record.occurrence.occurrenceScheduleEpoch != 0 ||
            record.occurrence.arrangingInstanceScheduleEpoch != 0 ||
            record.immediateEntry.gate != nullptr || record.immediateEntry.generation != 0 ||
            record.armToken.slot != savedArm.slot || record.armToken.generation != savedArm.generation ||
            record.armToken.renderSlot != savedArm.renderSlot ||
            record.armToken.renderGeneration != savedArm.renderGeneration) return false;
        const EventRow* actual = nullptr;
        for (std::uint32_t index = pageEventCount_[claim.transferRing];
             index < pageEventCount_[claim.transferRing] + pagePreparedEventCount_[claim.transferRing]; ++index) {
            const auto& row = pageEvents_[base + index];
            if (row.txnSlot != slot) continue;
            if (actual != nullptr || row.rowSequence != claim.transferRowSequences[position]) return false;
            actual = &row;
        }
        if (actual == nullptr) return false;
        const auto& row = *actual;
        const auto& expected = inputs[position];
        const auto arm = row.armToken.Valid() ? row.armToken : record.armToken;
        if (row.executeFrame != claim.transferFrame || row.preparedEventOrdinal != 0 || row.frontier.Present() ||
            expected.frontier.Present() || expected.frozenTxnValid ||
            expected.immediateEntry.gate != nullptr || expected.immediateEntry.generation != 0 ||
            row.txnSlot != expected.txnSlot || row.rowSequence != expected.rowSequence ||
            row.executeFrame != expected.executeFrame || row.logicCommitSequence != scope.sequence ||
            expected.logicCommitSequence != scope.sequence || row.transactionId != scope.sequence ||
            expected.transactionId != scope.sequence || row.executionOrdinal != expected.executionOrdinal ||
            row.eventIndex != expected.eventIndex || row.transactionOrdinal != expected.transactionOrdinal ||
            row.subeventPhase != expected.subeventPhase || row.payload != expected.payload ||
            row.authoredFadeOutCandidate != expected.authoredFadeOutCandidate ||
            arm.slot != expected.armToken.slot || arm.generation != expected.armToken.generation ||
            arm.renderSlot != expected.armToken.renderSlot || arm.renderGeneration != expected.armToken.renderGeneration ||
            record.program.program != expected.program.program ||
            record.program.programGeneration != expected.program.programGeneration ||
            record.ownerInstance != expected.ownerInstanceHandle ||
            record.ownerScheduleEpoch != expected.ownerInstanceScheduleEpoch) return false;
        const auto& binding = bindings[position];
        if (!binding.Valid() || binding.kind != FrontierOperationKind::Entry ||
            !(binding.entry == bindings[0].entry) || !(binding.owner == binding.entry.owner) ||
            binding.entry.attempt != scope.attemptGeneration || binding.world != scope.world ||
            binding.transport != claim.transferTransport || !binding.group.Exact() ||
            binding.group.absoluteQuantum != claim.transferAbsoluteQuantum ||
            binding.operationOrdinal != 0 || binding.operationEventCount != 1 ||
            binding.entryEventOrdinal != position) return false;
    }
    return true;
}

bool ScheduledTransactionPool::ProvePreparedPageBatchTransfer(
    PreparedTransactionBatch& original, const PreparedPageGroupTransferScope& scope,
    const std::array<ConsumedEvent, 5>& inputs,
    const std::array<FrontierEventBinding, 5>& bindings,
    PreparedPageBatchTransferProof* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || out->claim_ != nullptr ||
        !MatchesPreparedPageBatchInput(original, scope, inputs, bindings)) return false;
    auto& claim = preparedTransactionClaims_[original.claimSlot_];
    if (claim.transferProof != nullptr || !CanChargePreparedTransactionIngress(claim)) return false;
    // Older reservations acquire real current-base ingress only after all checks.
    if (claim.preparedIngressQuantum != baseQuantumIndex_) {
        preparedIngressThisQuantum_ += claim.transactionCount;
        claim.preparedIngressQuantum = baseQuantumIndex_;
    }
    out->inputs_ = inputs;
    out->bindings_ = bindings;
    out->owner_ = this;
    out->claim_ = &claim;
    out->original_ = &original;
    claim.transferProof = out;
    return true;
}

PreparedTransactionBatch ScheduledTransactionPool::ConsumePreparedPageBatchTransfer(
    PreparedPageBatchTransferProof&& proof) noexcept {
    assert(proof.Valid());
    auto& pool = *proof.owner_;
    auto& original = *proof.original_;
    auto& claim = *static_cast<PreparedTransactionClaim*>(proof.claim_);
    const auto ring = claim.transferRing;
    const auto eventBase = static_cast<std::size_t>(ring) * pool.config_.maxEventsPerQuantum;
    const auto batchBase = static_cast<std::size_t>(ring) * pool.config_.maxFrameBatchesPerQuantum;
    const auto sequence = claim.transferScope.sequence;
    assert(claim.preparedIngressQuantum == pool.baseQuantumIndex_);
    for (std::size_t position = 0; position < claim.transferTransactionSlots.size(); ++position) {
        const auto slot = claim.transferTransactionSlots[position];
        auto& record = pool.transactions_[slot];
        const auto live = pool.pageEventCount_[ring];
        const auto prepared = pool.pagePreparedEventCount_[ring];
        std::uint32_t found = prepared;
        for (std::uint32_t index = 0; index < prepared; ++index)
            if (pool.pageEvents_[eventBase + live + index].txnSlot == slot) { found = index; break; }
        // Held claim ownership guarantees existence; the scan only locates moved storage.
        assert(found < prepared);
        if (found != 0) std::swap(pool.pageEvents_[eventBase + live],
                                 pool.pageEvents_[eventBase + live + found]);
        auto& row = pool.pageEvents_[eventBase + live];
        assert(row.rowSequence == claim.transferRowSequences[position]);
        row.frontier = proof.bindings_[position];
        FrameBatchRow* batch = nullptr;
        for (std::uint32_t index = 0; index < pool.pageBatchOccupiedCount_[ring]; ++index)
            if (pool.pageBatchRows_[batchBase + index].frame == row.executeFrame) {
                batch = &pool.pageBatchRows_[batchBase + index]; break;
            }
        assert(batch != nullptr && batch->preparedReferences != 0);
        --batch->preparedReferences;
        if (batch->liveReferences++ == 0) ++pool.pageBatchCount_[ring];
        ++pool.pageEventCount_[ring];
        --pool.pagePreparedEventCount_[ring];
        ++pool.pageHeaders_[ring].committedTransactionCount;
        record.used = true;
        record.prepared = false;
        record.id = sequence;
        ++pool.liveTransactionCount_;
        pool.transactionEventsHighWater_ = std::max(pool.transactionEventsHighWater_, record.summary.eventCount);
        pool.transactionSpanHighWater_ = std::max(pool.transactionSpanHighWater_, record.lastFrame - record.firstFrame);
    }
    pool.pageSorted_[ring] = 0;
    pool.eventHighWater_ = std::max(pool.eventHighWater_, pool.pageEventCount_[ring]);
    pool.frameBatchHighWater_ = std::max(pool.frameBatchHighWater_, pool.pageBatchCount_[ring]);
    pool.transactionHighWater_ = std::max(pool.transactionHighWater_, pool.liveTransactionCount_);
    pool.preparedIngressThisQuantum_ -= claim.transactionCount;
    pool.ingressThisQuantum_ += claim.transactionCount;
    pool.ingressHighWater_ = std::max(pool.ingressHighWater_, pool.ingressThisQuantum_);
    claim.committed = true;
    claim.committedIngressQuantum = pool.baseQuantumIndex_;
    PreparedTransactionBatch retained;
    retained.owner_ = &pool;
    retained.claimSlot_ = original.claimSlot_;
    retained.generation_ = original.generation_;
    retained.prepared_ = true;
    proof.Release();
    pool.DetachPreparedPageBatchTransfer(original.claimSlot_);
    return retained;
}

PreparedPlan03GroupSlice::~PreparedPlan03GroupSlice() {
    if (transferSelected_ && owner_ != nullptr) owner_->DropPreparedPageGroupOriginal(this);
}

PreparedPlan03GroupSlice::PreparedPlan03GroupSlice(PreparedPlan03GroupSlice&& other) noexcept {
    *this = std::move(other);
}

PreparedPlan03GroupSlice& PreparedPlan03GroupSlice::operator=(
    PreparedPlan03GroupSlice&& other) noexcept {
    if (this == &other || Held() || other.Held()) return *this;
    if (transferSelected_ && owner_ != nullptr) owner_->DropPreparedPageGroupOriginal(this);
    const bool associated = other.transferSelected_ && other.Valid();
    owner_ = other.owner_;
    rowIndex_ = other.rowIndex_;
    generation_ = other.generation_;
    prepared_ = other.prepared_;
    transferSelected_ = other.transferSelected_;
    if (associated) owner_->pagePlan03Groups_[rowIndex_].transferOriginal = this;
    other.owner_ = nullptr;
    other.rowIndex_ = SIZE_MAX;
    other.generation_ = 0;
    other.prepared_ = false;
    other.transferSelected_ = false;
    return *this;
}

bool PreparedPlan03GroupSlice::Valid() const noexcept {
    return owner_ != nullptr && prepared_ &&
        (!transferSelected_ || owner_->OwnsPreparedPageGroupOriginal(*this));
}

bool PreparedPlan03GroupSlice::Held() const noexcept {
    return transferSelected_ && owner_ != nullptr && owner_->PreparedPageGroupHeld(*this);
}

bool PreparedPlan03GroupSlice::CaptureProspectiveIdentity(
    Plan03GroupSliceIdentity* out) const noexcept {
    if (out == nullptr || !transferSelected_ || !Valid()) return false;
    *out = owner_->pagePlan03Groups_[rowIndex_].transferProspectiveIdentity;
    return true;
}

PreparedPageGroupTransferProof::PreparedPageGroupTransferProof(
    PreparedPageGroupTransferProof&& other) noexcept {
    *this = std::move(other);
}

PreparedPageGroupTransferProof& PreparedPageGroupTransferProof::operator=(
    PreparedPageGroupTransferProof&& other) noexcept {
    // A held proof's address is part of its actual row association.
    if (this == &other || row_ != nullptr || other.row_ != nullptr) return *this;
    return *this;
}

bool PreparedPageGroupTransferProof::Valid() const noexcept {
    if (owner_ == nullptr || row_ == nullptr || original_ == nullptr) return false;
    const auto& row = *static_cast<const ScheduledTransactionPool::Plan03GroupSliceRow*>(row_);
    return row.transferProof == this && row.transferOriginal == original_ &&
        owner_->OwnsPreparedPageGroupOriginal(*original_) && original_->Held();
}

bool PreparedPageGroupTransferProof::MatchesOriginalScope(
    const PreparedPlan03GroupSlice& original, const PreparedPageGroupTransferScope& scope,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept {
    return Valid() && original_ == &original &&
        owner_->MatchesPreparedPageGroupInput(original, scope, input, armTable);
}

void PreparedPageGroupTransferProof::Release() noexcept {
    if (row_ != nullptr) {
        auto& row = *static_cast<ScheduledTransactionPool::Plan03GroupSliceRow*>(row_);
        if (row.frontierBindings != nullptr) {
            row.frontierBindings->Release();
            return;
        }
        assert(row.transferProof == this && row.transferOriginal == original_);
        owner_->ReleasePreparedPageGroupTransferLease(this);
        row.transferProof = nullptr;
    }
    owner_ = nullptr;
    row_ = nullptr;
    original_ = nullptr;
}

CommittedPlan03GroupSlice::~CommittedPlan03GroupSlice() {
    if (jointCancellation_ != nullptr) jointCancellation_->Release();
    if (cancellationPending_ && owner_ != nullptr) owner_->DropUnpublishedCancellationOriginal(*this);
    if (frontierBindings_ != nullptr) frontierBindings_->Release();
    if (privateRetirement_ != nullptr) privateRetirement_->Release();
}


namespace {
bool SameJointArm(TimelineArmToken left, TimelineArmToken right) noexcept {
    return left.slot == right.slot && left.generation == right.generation &&
        left.renderSlot == right.renderSlot && left.renderGeneration == right.renderGeneration;
}
bool JointArmUnread(const TimelineArmTable::Row& row, TimelineArmToken token) noexcept {
    const auto state = row.state.load(std::memory_order_acquire);
    return row.identity.load(std::memory_order_acquire) ==
            ((static_cast<std::uint64_t>(token.generation) << 32) | token.slot) &&
        row.generation == token.generation && row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration &&
        (state == ArmState::Prepared || state == ArmState::PendingApply) &&
        row.frontierAdmission == nullptr && row.frontierAdmissionLease == nullptr &&
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row.variableReadPins.load(std::memory_order_acquire) == 0 &&
        row.frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr && !row.frontierOperation.Valid() &&
        row.armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        row.logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
        row.appliedPlainStopSequence.load(std::memory_order_acquire) == 0 &&
        !row.pendingControlSourceReader.Valid() && !row.originalPlainControl.Valid() &&
        !row.appliedPlainStopSource.Valid() && !row.nativePrivateWithdrawn.load(std::memory_order_acquire) &&
        row.authoredControl.operation == nullptr && row.authoredControl.stage == TimelineArmTable::AuthoredControlStage::None;
}
}

bool UnpublishedGroupArmCancellationProof::HoldsOriginalArm(
    const TimelineArmTable& table, TimelineArmToken token) const noexcept {
    if (!Valid() || armTable_ != &table || armTableIncarnation_ != table.Incarnation()) return false;
    for (const auto& held : arms_)
        if (held.heldBy_ == this && held.row_ != nullptr && SameJointArm(held.token_, token)) return true;
    return false;
}

bool UnpublishedGroupArmCancellationProof::ConsumedOriginalArm(TimelineArmToken token) const noexcept {
    if (!consumed_) return false;
    for (const auto& held : arms_)
        if (SameJointArm(held.token_, token)) return true;
    return false;
}

void UnpublishedGroupArmCancellationProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseUnpublishedGroupArmCancellation(*this);
    consumed_ = false; groups_ = {}; arms_ = {};
}

bool ScheduledTransactionPool::CompleteJointGroupMemberCoverage(
    const UnpublishedGroupArmCancellationProof& proof, TimelineArmToken arm) const noexcept {
    const auto& table = *proof.armTable_;
    const auto& scope = table.groupMemberScopes_[arm.slot];
    if (!SameJointArm(scope.arm, arm)) return false;
    std::uint64_t count = 0;
    for (std::size_t group = 0; group < proof.groups_.size(); ++group) {
        const auto& held = proof.groups_[group];
        const auto& row = pagePlan03Groups_[held.identity_.committedRow];
        const auto base = held.identity_.committedRow * config_.maxArmsPerGroup;
        for (std::uint16_t member = 0; member < row.memberCount; ++member) {
            const auto& original = pageGroupMemberOriginals_[base + member];
            if (!SameJointArm(original.arm_, arm)) continue;
            if (original.serial_ == 0 || original.Receiver() != &groupMemberReceiver_ ||
                groupMemberReceiver_.owner_ != &table || groupMemberReceiver_.pool_ != this ||
                original.ReceiverEpisode() != groupMemberReceiver_.episode_ ||
                original.ArmTableIncarnation() != proof.armTableIncarnation_ ||
                !(original.Group() == held.identity_) ||
                original.memberIndex_ != table.rows_[arm.slot].memberIndex ||
                original.Group().decision.slot != table.rows_[arm.slot].parentGroup.decision.slot ||
                original.Group().decision.generation != table.rows_[arm.slot].parentGroup.decision.generation ||
                !SameJointArm(pagePlan03GroupMembers_[base + member], arm)) return false;
            for (std::size_t priorGroup = 0; priorGroup <= group; ++priorGroup) {
                const auto& priorHeld = proof.groups_[priorGroup];
                const auto priorBase = priorHeld.identity_.committedRow * config_.maxArmsPerGroup;
                const auto priorCount = priorGroup == group ? member :
                    pagePlan03Groups_[priorHeld.identity_.committedRow].memberCount;
                for (std::uint16_t prior = 0; prior < priorCount; ++prior)
                    if (pageGroupMemberOriginals_[priorBase + prior].serial_ == original.serial_) return false;
            }
            ++count;
        }
    }
    return count != 0 && count == scope.activeCount;
}

UnpublishedGroupArmCancellationStatus ScheduledTransactionPool::BeginUnpublishedGroupArmCancellation(
    std::span<CommittedPlan03GroupSlice* const> originals, TimelineArmTable& arms,
    std::span<UnpublishedGroupCancellationHold> groupStorage,
    std::span<UnpublishedArmCancellationHold> armStorage,
    UnpublishedGroupArmCancellationProof* out) noexcept {
    using Status = UnpublishedGroupArmCancellationStatus;
    if (out == nullptr || out->Valid() || out->consumed_ || originals.empty() || !Valid() ||
        plan03ArmTable_ != &arms || config_.maxPlan03GroupHeadersPerPage == 0) return Status::NotEligible;
    if (groupStorage.size() < originals.size()) return Status::StorageTooSmall;
    for (auto& held : groupStorage) if (held.heldBy_ != nullptr) return Status::Busy;
    for (auto& held : armStorage) if (held.heldBy_ != nullptr) return Status::Busy;
    std::size_t armCount = 0;
    for (std::size_t index = 0; index < originals.size(); ++index) {
        auto* original = originals[index];
        if (original == nullptr || !original->Valid() || original->owner_ != this ||
            original->privateRetirement_ != nullptr || original->frontierBindings_ != nullptr ||
            original->jointCancellation_ != nullptr || original->rowIndex_ >= pagePlan03Groups_.size()) return Status::NotEligible;
        for (std::size_t prior = 0; prior < index; ++prior)
            if (originals[prior] == original || originals[prior]->rowIndex_ == original->rowIndex_) return Status::NotEligible;
        const auto rowIndex = original->rowIndex_;
        const auto ring = rowIndex / config_.maxPlan03GroupHeadersPerPage;
        const auto& row = pagePlan03Groups_[rowIndex];
        const auto& page = pageHeaders_[ring];
        const Plan03GroupSliceIdentity identity{page.absoluteQuantumIndex, page.pageGeneration,
            row.decision, row.sliceIndex, rowIndex, original->generation_, epoch_};
        const auto snapshot = pagePlan03GroupLeases_[rowIndex].Capture();
        if (row.memberCount == 0 || row.sourceControlCount != 0 ||
            row.immediateEntry.gate != nullptr || row.immediateEntry.generation != 0 ||
            row.frontier.role != FrontierGroupRole::Legacy || row.frontier.operation.Valid()) return Status::NotEligible;
        if (!MatchesCommittedPlan03GroupSlice(*original, identity) ||
            row.frontierPinsOwned || row.frontierDependents != 0 || row.unpublishedRetirementStage != 0 ||
            row.transferOriginal != nullptr || row.transferProof != nullptr || row.frontierBindings != nullptr ||
            row.privateUnboundArmRetirement != nullptr || row.jointCancellation != nullptr ||
            (row.cancellationOriginal != nullptr && row.cancellationOriginal != original) ||
            page.frozen || page.publishCutoffFrame != 0 ||
            pageQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pagePlan03GroupReaders_[rowIndex].pins.load(std::memory_order_acquire) != 0 ||
            pagePlan03GroupReaders_[rowIndex].lastAudioSequence.load(std::memory_order_acquire) != 0 ||
            snapshot.state != AudioSliceState::Ready || snapshot.pendingReasonBits != 0 || snapshot.invalidateAfterSlice)
            return Status::Busy;
        auto& group = groupStorage[index];
        group.original_ = original; group.identity_ = identity; group.lease_ = &pagePlan03GroupLeases_[rowIndex];
        const auto base = rowIndex * config_.maxArmsPerGroup;
        for (std::uint16_t member = 0; member < row.memberCount; ++member) {
            const auto token = pagePlan03GroupMembers_[base + member];
            if (!token.Valid() || token.slot >= arms.capacity_) return Status::NotEligible;
            bool duplicate = false;
            for (std::size_t prior = 0; prior < armCount; ++prior) duplicate |= SameJointArm(armStorage[prior].token_, token);
            if (duplicate) continue;
            if (armCount == armStorage.size()) return Status::StorageTooSmall;
            auto& held = armStorage[armCount++];
            held.token_ = token; held.row_ = nullptr; held.submission_ = nullptr; held.sourceEpisode_ = 0;
        }
    }
    // A local unregistered view verifies every actual original before taking gates.
    out->armTable_ = &arms; out->armTableIncarnation_ = arms.Incarnation();
    out->groups_ = groupStorage.first(originals.size()); out->arms_ = armStorage.first(armCount);
    for (const auto& held : out->arms_) {
        if (!CompleteJointGroupMemberCoverage(*out, held.token_) || arms.groupMemberScopes_[held.token_.slot].jointHeld) {
            out->armTable_ = nullptr; out->groups_ = {}; out->arms_ = {}; return Status::Busy;
        }
    }
    std::size_t groupPrefix = 0, armPrefix = 0;
    const auto abortPrefix = [&]() noexcept {
        for (std::size_t index = 0; index < armPrefix; ++index) {
            auto& held = out->arms_[index];
            arms.groupMemberScopes_[held.token_.slot].jointHeld = false;
            held.row_->reservation.clear(std::memory_order_release); held.heldBy_ = nullptr; held.row_ = nullptr;
        }
        for (std::size_t index = 0; index < groupPrefix; ++index) {
            out->groups_[index].lease_->ReleasePrivateTransfer(); out->groups_[index].heldBy_ = nullptr;
        }
        out->armTable_ = nullptr; out->groups_ = {}; out->arms_ = {}; out->armTableIncarnation_ = 0;
    };
    for (; groupPrefix < out->groups_.size(); ++groupPrefix) {
        auto& held = out->groups_[groupPrefix];
        if (!held.lease_->HoldPrivateTransfer()) { abortPrefix(); return Status::Busy; }
        held.heldBy_ = out;
    }
    for (; armPrefix < out->arms_.size(); ++armPrefix) {
        auto& held = out->arms_[armPrefix];
        auto* row = arms.Resolve(held.token_);
        if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire)) { abortPrefix(); return Status::Busy; }
        if (!JointArmUnread(*row, held.token_) || !row->originalLifetimeIssued ||
            row->originalSourceSubmission == nullptr || row->originalSourceEpisode == 0 ||
            !CompleteJointGroupMemberCoverage(*out, held.token_)) {
            row->reservation.clear(std::memory_order_release); abortPrefix(); return Status::NotEligible;
        }
        held.row_ = row; held.submission_ = row->originalSourceSubmission; held.sourceEpisode_ = row->originalSourceEpisode;
        held.heldBy_ = out; arms.groupMemberScopes_[held.token_.slot].jointHeld = true;
    }
    // All refusal points precede the persistent native cancellation protection.
    for (auto& held : out->groups_) {
        auto& row = pagePlan03Groups_[held.identity_.committedRow];
        const auto ring = held.identity_.committedRow / config_.maxPlan03GroupHeadersPerPage;
        if (!row.publicationHeld) { ++pagePublicationHolds_[ring]; row.publicationHeld = true; }
        row.cancellationOriginal = held.original_; row.jointCancellation = out;
        held.original_->cancellationPending_ = true; held.original_->jointCancellation_ = out;
    }
    out->owner_ = this; out->poolNext_ = unpublishedGroupArmCancellations_; unpublishedGroupArmCancellations_ = out;
    out->armTableNext_ = arms.unpublishedGroupArmCancellations_; arms.unpublishedGroupArmCancellations_ = out;
    return Status::Held;
}

void ScheduledTransactionPool::ReleaseUnpublishedGroupArmCancellation(UnpublishedGroupArmCancellationProof& proof) noexcept {
    assert(proof.owner_ == this && proof.armTable_ != nullptr);
    for (auto& held : proof.arms_) {
        proof.armTable_->groupMemberScopes_[held.token_.slot].jointHeld = false;
        held.row_->reservation.clear(std::memory_order_release); held.row_ = nullptr; held.heldBy_ = nullptr;
    }
    for (auto& held : proof.groups_) {
        held.lease_->ReleasePrivateTransfer(); held.heldBy_ = nullptr;
        held.original_->jointCancellation_ = nullptr;
        pagePlan03Groups_[held.identity_.committedRow].jointCancellation = nullptr;
    }
    auto** poolLink = &unpublishedGroupArmCancellations_;
    while (*poolLink != &proof) poolLink = &(*poolLink)->poolNext_;
    *poolLink = proof.poolNext_;
    auto** armLink = &proof.armTable_->unpublishedGroupArmCancellations_;
    while (*armLink != &proof) armLink = &(*armLink)->armTableNext_;
    *armLink = proof.armTableNext_;
    proof.owner_ = nullptr; proof.armTable_ = nullptr; proof.armTableIncarnation_ = 0;
    proof.poolNext_ = proof.armTableNext_ = nullptr;
}

UnpublishedGroupArmCancellationStatus ScheduledTransactionPool::FinishUnpublishedGroupArmCancellation(
    UnpublishedGroupArmCancellationProof& proof,
    std::span<const TimelineArmTable::OriginalUnpublishedSourceDisposition* const> sources) noexcept {
    using Status = UnpublishedGroupArmCancellationStatus;
    if (proof.owner_ != this || proof.armTable_ == nullptr || proof.armTableIncarnation_ != proof.armTable_->Incarnation() ||
        sources.size() != proof.arms_.size()) return Status::NotEligible;
    for (const auto& held : proof.groups_) {
        const auto index = held.identity_.committedRow;
        const auto ring = index / config_.maxPlan03GroupHeadersPerPage;
        const auto& row = pagePlan03Groups_[index]; const auto snapshot = held.lease_->Capture();
        if (held.heldBy_ != &proof || held.original_->jointCancellation_ != &proof ||
            row.jointCancellation != &proof || row.cancellationOriginal != held.original_ ||
            !row.publicationHeld || pagePublicationHolds_[ring] == 0 || pagePlan03GroupCount_[ring] == 0 ||
            !MatchesCommittedPlan03GroupSlice(*held.original_, held.identity_) || pageHeaders_[ring].frozen ||
            pageQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pagePlan03GroupReaders_[index].pins.load(std::memory_order_acquire) != 0 ||
            pagePlan03GroupReaders_[index].lastAudioSequence.load(std::memory_order_acquire) != 0 ||
            snapshot.state != AudioSliceState::PrivateTransferReserved || snapshot.pendingReasonBits != 0 || snapshot.invalidateAfterSlice)
            return Status::Busy;
    }
    for (const auto& held : proof.arms_) {
        const auto* row = held.row_;
        if (held.heldBy_ != &proof || row == nullptr || !JointArmUnread(*row, held.token_) ||
            !proof.armTable_->groupMemberScopes_[held.token_.slot].jointHeld ||
            !CompleteJointGroupMemberCoverage(proof, held.token_) ||
            row->originalLifetimePinned.load(std::memory_order_acquire)) return Status::Busy;
        const TimelineArmTable::OriginalUnpublishedSourceDisposition* source = nullptr;
        for (const auto* candidate : sources)
            if (candidate != nullptr && SameJointArm(candidate->token_, held.token_)) {
                if (source != nullptr) return Status::NotEligible;
                source = candidate;
            }
        if (source == nullptr) return Status::Busy;
        if (source->owner_ != proof.armTable_ || source->row_ != row || source->tableIncarnation_ != proof.armTableIncarnation_ ||
            source->submission_ != held.submission_ || row->originalSourceSubmission != held.submission_ ||
            source->sourceEpisode_ != held.sourceEpisode_ || row->originalSourceEpisode != held.sourceEpisode_ ||
            source->sourceIdentity_ == 0 || source->sourceEpisode_ == 0 ||
            (source->unboundOriginal_ ?
                (source->commandQueue_ == nullptr || source->originalRange_ == nullptr || source->rangeOrigin_ != 0 ||
                    source->rangeHead_ != 0 || source->rangeCount_ != 0) :
                (source->commandQueue_ == nullptr || source->originalRange_ == nullptr || source->rangeOrigin_ == 0 || source->rangeCount_ == 0)))
            return Status::NotEligible;
    }
    ConsumeHeldUnpublishedGroupArmCancellation(proof);
    return Status::Consumed;
}

void ScheduledTransactionPool::ConsumeHeldUnpublishedGroupArmCancellation(UnpublishedGroupArmCancellationProof& proof) noexcept {
    // All groups, scopes and Source dispositions have passed while gates stay held.
    // ReleasePrivateTransfer cannot expose audio: native page protection remains
    // until this same tail removes every real group recipe and obligation.
    for (auto& held : proof.groups_) {
        const auto index = held.identity_.committedRow;
        const auto ring = index / config_.maxPlan03GroupHeadersPerPage;
        const auto base = index * config_.maxArmsPerGroup;
        const auto count = pagePlan03Groups_[index].memberCount;
        held.lease_->ReleasePrivateTransfer();
        ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + base, count);
        std::fill(pagePlan03GroupMembers_.begin() + base, pagePlan03GroupMembers_.begin() + base + count, TimelineArmToken{});
        --pagePlan03GroupCount_[ring]; --pagePublicationHolds_[ring]; pagePlan03Groups_[index] = {};
        if (pagePlan03GroupClaimGenerations_[index] == UINT32_MAX) pagePlan03GroupRowStates_[index] = Plan03GroupRowState::Retired;
        else { ++pagePlan03GroupClaimGenerations_[index]; pagePlan03GroupRowStates_[index] = Plan03GroupRowState::Free; }
        auto& original = *held.original_;
        original.owner_ = nullptr; original.rowIndex_ = SIZE_MAX; original.generation_ = 0;
        original.privateFactory_ = false; original.privateExecuteFrame_ = original.privateArmTableIncarnation_ = 0;
        original.jointCancellation_ = nullptr; original.cancellationPending_ = false; held.heldBy_ = nullptr;
    }
    for (auto& held : proof.arms_) {
        auto& row = *held.row_; auto& scope = proof.armTable_->groupMemberScopes_[held.token_.slot];
        assert(scope.activeCount == 0 && scope.jointHeld);
        row.identity.store(0, std::memory_order_release);
        if (held.token_.generation != UINT32_MAX) row.generation = held.token_.generation + 1u;
        row.renderSlot = UINT32_MAX; row.renderGeneration = 0; row.startFrame = row.armDeadline = 0;
        row.pendingImmediateStart = false; row.startBound = true; row.programGeneration = 0;
        row.target = {}; row.parentGroup = {}; row.memberIndex = UINT16_MAX;
        row.originalSourceSubmission = nullptr; row.originalSourceEpisode = 0; row.originalLifetimeIssued = false;
        scope.sourceFactoryArm = {}; scope.unparentedPreparedSourceFactory = false;
        row.frontierAdmissionFinished.store(true, std::memory_order_release);
        row.state.store(held.token_.generation == UINT32_MAX ? ArmState::Quarantined : ArmState::Free, std::memory_order_release);
        scope.jointHeld = false; row.reservation.clear(std::memory_order_release);
        held.row_ = nullptr; held.submission_ = nullptr; held.heldBy_ = nullptr;
    }
    auto** poolLink = &unpublishedGroupArmCancellations_;
    while (*poolLink != &proof) poolLink = &(*poolLink)->poolNext_;
    *poolLink = proof.poolNext_;
    auto** armLink = &proof.armTable_->unpublishedGroupArmCancellations_;
    while (*armLink != &proof) armLink = &(*armLink)->armTableNext_;
    *armLink = proof.armTableNext_;
    proof.owner_ = nullptr; proof.armTable_ = nullptr; proof.armTableIncarnation_ = 0;
    proof.poolNext_ = proof.armTableNext_ = nullptr; proof.consumed_ = true;
}

void ScheduledTransactionPool::DropUnpublishedCancellationOriginal(CommittedPlan03GroupSlice& original) noexcept {
    assert(original.cancellationPending_ && original.owner_ == this);
    const bool returned = RollbackCommittedPlan03GroupSlice(&original);
    assert(returned); (void)returned;
}

bool PrivateUnboundPageArmRetirementProof::Valid() const noexcept {
    if (owner_ == nullptr || armTable_ == nullptr || heldLeases_ != 2 || heldArms_ != 2 ||
        armTableIncarnation_ != armTable_->Incarnation()) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        if (originals_[i] == nullptr || originals_[i]->privateRetirement_ != this ||
            rows_[i] == nullptr || leases_[i] == nullptr ||
            leases_[i]->Capture().state != AudioSliceState::PrivateTransferReserved ||
            !owner_->MatchesCommittedPlan03GroupSlice(*originals_[i], identities_[i])) return false;
    }
    return true;
}

void PrivateUnboundPageArmRetirementProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleasePrivateUnboundPageArmRetirement(*this);
}

void ScheduledTransactionPool::ReleasePrivateUnboundPageArmRetirement(
    PrivateUnboundPageArmRetirementProof& proof) noexcept {
    assert(proof.owner_ == this && proof.armTable_ != nullptr);
    for (std::size_t i = 0; i < proof.heldArms_; ++i) {
        proof.rows_[i]->reservation.clear(std::memory_order_release);
        proof.rows_[i] = nullptr;
    }
    for (std::size_t i = 0; i < proof.heldLeases_; ++i)
        proof.leases_[i]->ReleasePrivateTransfer();
    for (std::size_t i = 0; i < 2; ++i) {
        assert(proof.originals_[i]->privateRetirement_ == &proof);
        proof.originals_[i]->privateRetirement_ = nullptr;
        auto& row = pagePlan03Groups_[proof.identities_[i].committedRow];
        assert(row.privateUnboundArmRetirement == &proof);
        row.privateUnboundArmRetirement = nullptr;
    }
    auto** poolLink = &privateUnboundPageArmRetirements_;
    while (*poolLink != &proof) poolLink = &(*poolLink)->poolNext_;
    *poolLink = proof.poolNext_;
    auto** armLink = &proof.armTable_->privateUnboundPageArmRetirements_;
    while (*armLink != &proof) armLink = &(*armLink)->armTableNext_;
    *armLink = proof.armTableNext_;
    proof.owner_ = nullptr;
    proof.armTable_ = nullptr;
    proof.armTableIncarnation_ = 0;
    proof.originals_ = {};
    proof.identities_ = {};
    proof.arms_ = {};
    proof.rows_ = {};
    proof.leases_ = {};
    proof.heldArms_ = proof.heldLeases_ = 0;
    proof.poolNext_ = proof.armTableNext_ = nullptr;
}

bool ScheduledTransactionPool::ProvePrivateUnboundPageArmRetirement(
    CommittedPlan03GroupSlice& first, CommittedPlan03GroupSlice& second,
    TimelineArmTable& arms, const std::array<TimelineArmToken, 2>& members,
    std::uint64_t executeFrame, PrivateUnboundPageArmRetirementProof* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || out->armTable_ != nullptr ||
        &first == &second || first.Held() || second.Held() || plan03ArmTable_ != &arms ||
        !members[0].Valid() || !members[1].Valid() || members[0].slot == members[1].slot ||
        config_.maxPlan03GroupHeadersPerPage == 0) return false;
    const std::array<CommittedPlan03GroupSlice*, 2> originals{&first, &second};
    std::array<Plan03GroupSliceIdentity, 2> identities{};
    std::array<AudioSliceLease*, 2> leases{};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& original = *originals[i];
        if (!original.Valid() || original.owner_ != this || !original.privateFactory_ ||
            original.privateExecuteFrame_ != executeFrame ||
            original.privateArmTableIncarnation_ != arms.Incarnation() ||
            original.rowIndex_ >= pagePlan03Groups_.size()) return false;
        const auto index = original.rowIndex_;
        const auto ring = index / config_.maxPlan03GroupHeadersPerPage;
        const auto& row = pagePlan03Groups_[index];
        const auto& page = pageHeaders_[ring];
        identities[i] = {page.absoluteQuantumIndex, page.pageGeneration, row.decision,
            row.sliceIndex, index, original.generation_, epoch_};
        const auto& actual = pagePlan03GroupMembers_[index * config_.maxArmsPerGroup];
        const auto& arm = members[i];
        const auto lease = pagePlan03GroupLeases_[index].Capture();
        if (!MatchesCommittedPlan03GroupSlice(original, identities[i]) ||
            executeFrame / quantumFrames_ != page.absoluteQuantumIndex ||
            !row.publicationHeld || row.privateUnboundArmRetirement != nullptr ||
            row.transferOriginal != nullptr || row.transferProof != nullptr ||
            row.memberCount != 1 || row.sourceControlCount != 0 ||
            row.immediateEntry.gate != nullptr || row.immediateEntry.generation != 0 ||
            row.frontier.role != FrontierGroupRole::Legacy || row.frontier.operation.Valid() ||
            row.frontierPinsOwned || row.frontierDependents != 0 || row.unpublishedRetirementStage != 0 ||
            page.frozen || page.publishCutoffFrame != 0 ||
            pageQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pagePlan03GroupReaders_[index].pins.load(std::memory_order_acquire) != 0 ||
            pagePlan03GroupReaders_[index].lastAudioSequence.load(std::memory_order_acquire) != 0 ||
            actual.slot != arm.slot || actual.generation != arm.generation ||
            actual.renderSlot != arm.renderSlot || actual.renderGeneration != arm.renderGeneration ||
            lease.state != AudioSliceState::Ready || lease.pendingReasonBits != 0 ||
            lease.invalidateAfterSlice) return false;
        leases[i] = &pagePlan03GroupLeases_[index];
    }
    if (identities[0].committedRow == identities[1].committedRow) return false;
    std::size_t heldLeases = 0;
    std::size_t heldArms = 0;
    std::array<TimelineArmTable::Row*, 2> rows{};
    const auto abortPrefix = [&]() noexcept {
        for (std::size_t i = 0; i < heldArms; ++i)
            rows[i]->reservation.clear(std::memory_order_release);
        for (std::size_t i = 0; i < heldLeases; ++i) leases[i]->ReleasePrivateTransfer();
    };
    for (; heldLeases < 2; ++heldLeases) {
        if (!leases[heldLeases]->HoldPrivateTransfer()) { abortPrefix(); return false; }
    }
    for (; heldArms < 2; ++heldArms) {
        const auto& token = members[heldArms];
        auto* row = arms.Resolve(token);
        if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire)) {
            abortPrefix(); return false;
        }
        rows[heldArms] = row;
        const auto& identity = identities[heldArms];
        if (row->identity.load(std::memory_order_acquire) !=
                ((static_cast<std::uint64_t>(token.generation) << 32) | token.slot) ||
            row->generation != token.generation || row->renderSlot != token.renderSlot ||
            row->renderGeneration != token.renderGeneration ||
            row->state.load(std::memory_order_acquire) != ArmState::PendingApply ||
            row->pendingImmediateStart || !row->startBound || row->startFrame != executeFrame ||
            row->armDeadline != executeFrame || row->memberIndex != 0 ||
            row->parentGroup.decision.slot != identity.decision.slot ||
            row->parentGroup.decision.generation != identity.decision.generation ||
            row->frontierAdmission != nullptr || row->frontierAdmissionLease != nullptr ||
            row->frontierReaderPins.load(std::memory_order_acquire) != 0 ||
            row->originalLifetimePinned.load(std::memory_order_acquire) ||
            row->variableReadPins.load(std::memory_order_acquire) != 0 ||
            row->frontierLastReadSequence.load(std::memory_order_acquire) != 0 ||
            row->frontierActiveLease.load(std::memory_order_acquire) != nullptr || row->frontierOperation.Valid() ||
            row->armedCompletionSequence.load(std::memory_order_acquire) != 0 ||
            row->startedBlockSequence.load(std::memory_order_acquire) != 0 ||
            row->logicalCompletionSequence.load(std::memory_order_acquire) != 0 ||
            row->tombstoneAudioSequence.load(std::memory_order_acquire) != 0) {
            row->reservation.clear(std::memory_order_release);
            abortPrefix(); return false;
        }
    }
    out->owner_ = this;
    out->armTable_ = &arms;
    out->armTableIncarnation_ = arms.Incarnation();
    out->originals_ = originals;
    out->identities_ = identities;
    out->arms_ = members;
    out->rows_ = rows;
    out->leases_ = leases;
    out->heldLeases_ = heldLeases;
    out->heldArms_ = heldArms;
    for (std::size_t i = 0; i < 2; ++i) {
        originals[i]->privateRetirement_ = out;
        pagePlan03Groups_[identities[i].committedRow].privateUnboundArmRetirement = out;
    }
    out->poolNext_ = privateUnboundPageArmRetirements_;
    privateUnboundPageArmRetirements_ = out;
    out->armTableNext_ = arms.privateUnboundPageArmRetirements_;
    arms.privateUnboundPageArmRetirements_ = out;
    return true;
}

void ScheduledTransactionPool::ConsumePrivateUnboundPageArmRetirement(
    PrivateUnboundPageArmRetirementProof&& proof) noexcept {
    assert(proof.Valid());
    for (auto* row : proof.rows_)
        assert(row != nullptr && !row->originalLifetimePinned.load(std::memory_order_acquire) &&
            row->variableReadPins.load(std::memory_order_acquire) == 0);
    for (auto* lease : proof.leases_) lease->ConsumePrivateTransferCancellation();
    proof.heldLeases_ = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        auto& row = *proof.rows_[i];
        const auto generation = proof.arms_[i].generation;
        row.frontierAdmissionFinished.store(true, std::memory_order_release);
        row.identity.store(0, std::memory_order_release);
        row.state.store(generation == UINT32_MAX ? ArmState::Quarantined : ArmState::Free,
            std::memory_order_release);
        if (generation != UINT32_MAX) row.generation = generation + 1u;
        row.renderSlot = UINT32_MAX;
        row.renderGeneration = 0;
        row.startFrame = row.armDeadline = 0;
        row.pendingImmediateStart = false;
        row.startBound = true;
        row.programGeneration = 0;
        row.target = {};
        row.parentGroup = {};
        row.memberIndex = UINT16_MAX;
    }
    proof.Release();
}

bool PreparedPageFrontierBindingsProof::Valid() const noexcept {
    return owner_ != nullptr && owner_->PreparedPageFrontierBindingsHeld(*this);
}

void PreparedPageFrontierBindingsProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleasePreparedPageFrontierBindings(*this);
}

bool ScheduledTransactionPool::PreparedPageFrontierBindingsHeld(
    const PreparedPageFrontierBindingsProof& proof) const noexcept {
    if (proof.owner_ != this || preparedPageFrontierBindingHolds_ == 0) return false;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& identity = proof.identities_[i];
        const auto* output = proof.outputs_[i];
        if (identity.committedRow >= pagePlan03Groups_.size() || output == nullptr ||
            output->frontierBindings_ != &proof) return false;
        const auto& row = pagePlan03Groups_[identity.committedRow];
        const auto lease = pagePlan03GroupLeases_[identity.committedRow].Capture();
        if (row.frontierBindings != &proof || row.frontier.role != FrontierGroupRole::Legacy ||
            row.frontierPinsOwned || row.frontierDependents != 0 ||
            pagePlan03GroupReaders_[identity.committedRow].pins.load(std::memory_order_acquire) != 0 ||
            lease.state != AudioSliceState::PrivateTransferReserved || lease.pendingReasonBits != 0 ||
            lease.invalidateAfterSlice) return false;
        if (proof.minted_[i]) {
            if (proof.groupProofs_[i] != nullptr || !row.publicationHeld ||
                !MatchesCommittedPlan03GroupSlice(*output, identity)) return false;
        } else {
            const auto* group = proof.groupProofs_[i];
            if (output->Valid() || group == nullptr || !group->Valid() ||
                group->owner_ != this || group->row_ != &row ||
                !(row.transferProspectiveIdentity == identity)) return false;
        }
    }
    return true;
}

void ScheduledTransactionPool::ReleasePreparedPageFrontierBindings(
    PreparedPageFrontierBindingsProof& proof, bool ownerDying) noexcept {
    assert(proof.owner_ == this && preparedPageFrontierBindingHolds_ != 0);
    // Remove all associations before unwinding any original proof callbacks.
    for (std::size_t i = 0; i < 3; ++i) {
        auto& row = pagePlan03Groups_[proof.identities_[i].committedRow];
        assert(row.frontierBindings == &proof);
        row.frontierBindings = nullptr;
        auto& output = *proof.outputs_[i];
        assert(output.frontierBindings_ == &proof);
        output.frontierBindings_ = nullptr;
        if (ownerDying && proof.minted_[i]) {
            output.owner_ = nullptr;
            output.rowIndex_ = SIZE_MAX;
            output.generation_ = 0;
            output.privateFactory_ = false;
            output.privateExecuteFrame_ = 0;
            output.privateArmTableIncarnation_ = 0;
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (proof.minted_[i])
            pagePlan03GroupLeases_[proof.identities_[i].committedRow].ReleasePrivateTransfer();
        else
            proof.groupProofs_[i]->Release();
    }
    --preparedPageFrontierBindingHolds_;
    proof.owner_ = nullptr;
    proof.groupProofs_ = {};
    proof.outputs_ = {};
    proof.identities_ = {};
    proof.bindings_ = {};
    proof.minted_ = {};
}

bool ScheduledTransactionPool::ProvePreparedPageFrontierBindings(
    const std::array<PreparedPageGroupTransferProof*, 3>& groups,
    const PreparedPageBatchTransferProof& batch,
    const std::array<FrontierGroupBinding, 3>& bindings,
    const std::array<CommittedPlan03GroupSlice*, 3>& outputs,
    PreparedPageFrontierBindingsProof* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || !batch.Valid() || batch.owner_ != this ||
        preparedPageFrontierBindingHolds_ == SIZE_MAX) return false;
    std::array<Plan03GroupSliceIdentity, 3> identities{};
    const auto sameArm = [](const TimelineArmToken& a, const TimelineArmToken& b) noexcept {
        return a.slot == b.slot && a.generation == b.generation &&
            a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
    };
    const auto& batchScope = static_cast<const PreparedTransactionClaim*>(batch.claim_)->transferScope;
    for (std::size_t i = 0; i < 3; ++i) {
        if (groups[i] == nullptr || !groups[i]->Valid() || groups[i]->owner_ != this ||
            outputs[i] == nullptr || outputs[i]->Valid() || outputs[i]->Held()) return false;
        const auto& row = *static_cast<const Plan03GroupSliceRow*>(groups[i]->row_);
        identities[i] = row.transferProspectiveIdentity;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (groups[i] == groups[prior] || outputs[i] == outputs[prior] ||
                identities[i].committedRow == identities[prior].committedRow) return false;
        const auto ring = identities[i].committedRow / config_.maxPlan03GroupHeadersPerPage;
        const auto lease = pagePlan03GroupLeases_[identities[i].committedRow].Capture();
        if (row.frontierBindings != nullptr || row.privateUnboundArmRetirement != nullptr ||
        row.jointCancellation != nullptr || row.cancellationOriginal != nullptr ||
            !(row.transferScope == batchScope) || row.memberCount != (i == 2 ? 0u : 1u) ||
            row.sourceControlCount != 0 || row.immediateEntry.gate != nullptr || row.immediateEntry.generation != 0 ||
            row.transferArmTable != plan03ArmTable_ || row.transferArmTableIncarnation == 0 ||
            row.frontier.role != FrontierGroupRole::Legacy || row.frontierPinsOwned ||
            row.frontierDependents != 0 || row.publicationHeld || row.unpublishedRetirementStage != 0 ||
            pagePlan03GroupReaders_[identities[i].committedRow].pins.load(std::memory_order_acquire) != 0 ||
            pagePlan03GroupReaders_[identities[i].committedRow].lastAudioSequence.load(std::memory_order_acquire) != 0 ||
            lease.state != AudioSliceState::PrivateTransferReserved || lease.pendingReasonBits != 0 ||
            lease.invalidateAfterSlice || pageHeaders_[ring].frozen || pageHeaders_[ring].publishCutoffFrame != 0 ||
            pageQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX ||
            pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != UINT64_MAX) return false;
        if (i != 0 && (identities[i].absoluteQuantum != identities[0].absoluteQuantum ||
                identities[i].pageGeneration != identities[0].pageGeneration ||
                row.transferArmTableIncarnation !=
                    pagePlan03Groups_[identities[0].committedRow].transferArmTableIncarnation)) return false;
        if (i < 2 && (row.transferMemberIndex != 0 || !row.transferMember.Valid() ||
                !sameArm(pagePlan03GroupMembers_[identities[i].committedRow * config_.maxArmsPerGroup],
                    row.transferMember) || !sameArm(batch.inputs_[3u + i].armToken, row.transferMember))) return false;
    }
    for (std::size_t event = 0; event < 5; ++event) {
        const auto groupIndex = event == 0 ? 2u : event < 3 ? event - 1u : event - 3u;
        const auto& actual = batch.inputs_[event];
        const auto& planned = batch.bindings_[event];
        const auto& row = pagePlan03Groups_[identities[groupIndex].committedRow];
        if (actual.executeFrame != row.transferExecuteFrame ||
            actual.logicCommitSequence != batchScope.sequence ||
            actual.program.program == nullptr || actual.program.programGeneration == 0 ||
            planned.kind != FrontierOperationKind::Entry || !(planned.group == identities[groupIndex]) ||
            !(planned.entry == batch.bindings_[0].entry) || planned.entry.attempt != batchScope.attemptGeneration ||
            planned.world != batchScope.world || planned.transport != epoch_ ||
            planned.entryEventOrdinal != event || planned.operationEventCount != 1 ||
            (event < 3 && (!sameArm(actual.armToken, TimelineArmToken{}) ||
                actual.ownerInstanceHandle != planned.owner.child ||
                actual.ownerInstanceScheduleEpoch != planned.owner.childEpoch))) return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto eventIndex = i == 2 ? 0u : i + 1u;
        const auto& actual = batch.inputs_[eventIndex];
        const auto& planned = batch.bindings_[eventIndex];
        const auto& binding = bindings[i];
        const TimelineExecutionOrder order{actual.executeFrame, actual.logicCommitSequence,
            actual.executionOrdinal, actual.eventIndex, actual.transactionOrdinal,
            actual.subeventPhase, actual.rowSequence};
        if (!binding.Valid() || binding.role != (i == 2 ? FrontierGroupRole::Coordinator : FrontierGroupRole::EntryMember) ||
            binding.operation.kind != FrontierOperationKind::Entry ||
            !(binding.entry == planned.entry) || !(binding.primary == identities[2]) ||
            !(binding.operation.group == identities[i]) || !(binding.operation.owner == planned.owner) ||
            !(binding.operation.order == order) || binding.operation.world != batchScope.world ||
            binding.operation.transport != epoch_ || binding.prerequisite.Valid() ||
            binding.prerequisiteMember.Valid() || binding.canonicalInitialCompletion ||
            binding.queueInitial.Valid() || binding.queueConditional.Valid() || binding.queueDescendant.Valid() ||
            binding.transition.Valid()) return false;
        if (i == 2) {
            if (binding.operation.program != nullptr || binding.operation.programGeneration != 0 ||
                !(binding.operation.owner == binding.entry.owner)) return false;
        } else if (binding.operation.program != actual.program.program ||
                binding.operation.programGeneration != actual.program.programGeneration) return false;
    }
    out->owner_ = this;
    out->groupProofs_ = groups;
    out->outputs_ = outputs;
    out->identities_ = identities;
    out->bindings_ = bindings;
    ++preparedPageFrontierBindingHolds_;
    for (std::size_t i = 0; i < 3; ++i) {
        pagePlan03Groups_[identities[i].committedRow].frontierBindings = out;
        outputs[i]->frontierBindings_ = out;
    }
    return true;
}

void ScheduledTransactionPool::ConsumePreparedPageGroupIntoFrontierBindings(
    PreparedPageGroupTransferProof&& group, PreparedPageFrontierBindingsProof& bindings,
    CommittedPlan03GroupSlice& output) noexcept {
    assert(bindings.Valid());
    auto& pool = *bindings.owner_;
    std::size_t i = 0;
    while (i < 3 && bindings.groupProofs_[i] != &group) ++i;
    assert(i < 3 && !bindings.minted_[i] && bindings.outputs_[i] == &output);
    const auto index = bindings.identities_[i].committedRow;
    const auto ring = index / pool.config_.maxPlan03GroupHeadersPerPage;
    auto& row = pool.pagePlan03Groups_[index];
    output.owner_ = &pool;
    output.rowIndex_ = index;
    output.generation_ = bindings.identities_[i].rowClaimGeneration;
    output.privateFactory_ = true;
    output.privateExecuteFrame_ = row.transferExecuteFrame;
    output.privateArmTableIncarnation_ = row.transferArmTableIncarnation;
    --pool.pagePreparedPlan03GroupCount_[ring];
    ++pool.pagePlan03GroupCount_[ring];
    ++pool.pagePublicationHolds_[ring];
    row.publicationHeld = true;
    pool.pagePlan03GroupRowStates_[index] = Plan03GroupRowState::Live;
    pool.pagePlan03GroupClaimGenerations_[index] = output.generation_;
    if (row.memberCount != 0)
        pool.pageGroupIdentities_[index].group.rowClaimGeneration = pool.pagePlan03GroupClaimGenerations_[index];
    pool.plan03ArmTableEstablishedByPrepared_ = false;
    // Spend the original without releasing the real lease or the continuation.
    auto& original = *group.original_;
    original.owner_ = nullptr;
    original.rowIndex_ = SIZE_MAX;
    original.generation_ = 0;
    original.prepared_ = original.transferSelected_ = false;
    row.transferOriginal = nullptr;
    row.transferProof = nullptr;
    row.transferPreparedIdentity = row.transferProspectiveIdentity = {};
    row.transferScope = {};
    row.transferExecuteFrame = 0;
    row.transferArmTable = nullptr;
    row.transferArmTableIncarnation = 0;
    row.transferMember = {};
    row.transferMemberIndex = 0;
    group.owner_ = nullptr;
    group.row_ = nullptr;
    group.original_ = nullptr;
    bindings.groupProofs_[i] = nullptr;
    bindings.minted_[i] = true;
}

void ScheduledTransactionPool::ConsumePreparedPageFrontierBindings(
    PreparedPageFrontierBindingsProof&& proof) noexcept {
    assert(proof.Valid() && proof.minted_[0] && proof.minted_[1] && proof.minted_[2]);
    auto& pool = *proof.owner_;
    for (const auto i : {2u, 0u, 1u}) {
        const auto index = proof.identities_[i].committedRow;
        auto& row = pool.pagePlan03Groups_[index];
        row.frontier = proof.bindings_[i];
        row.frontierPinsOwned = true;
        row.frontierDependents = i == 2 ? 2u : 0u;
        pool.pagePlan03GroupReaders_[index].pins.store(i == 2 ? 3u : 1u, std::memory_order_release);
    }
    // The coordinator lease is released last, after both member bindings exist.
    pool.ReleasePreparedPageFrontierBindings(proof);
}

bool ScheduledTransactionPool::IssueGroupMemberOriginals(const Plan03GroupSliceIdentity& group,
    const TimelineArmTable& table, const TimelineArmToken* members,
    std::uint16_t count, TimelineArmTable::OriginalGroupMember* originals,
    TimelineArmTable::OriginalGroupIdentity& identity) noexcept {
    if (count == 0) return true;
    if (members == nullptr || originals == nullptr || identity.liveMembers != 0) return false;
    for (std::uint16_t i = 0; i < count; ++i)
        if (originals[i].serial_ != 0) return false;
    identity.group = group;
    std::uint16_t issued = 0;
    for (; issued < count; ++issued) {
        if (!table.IssueOriginalGroupMember(groupMemberReceiver_, *this,
                identity, members[issued], &originals[issued])) {
            ReturnGroupMemberOriginals(originals, issued);
            return false;
        }
    }
    return true;
}

void ScheduledTransactionPool::ReturnGroupMemberOriginals(
    TimelineArmTable::OriginalGroupMember* originals, std::size_t count) noexcept {
    if (originals == nullptr) return;
    for (std::size_t i = 0; i < count; ++i) {
        auto& original = originals[i];
        if (original.serial_ == 0) continue;
        assert(original.Receiver() == &groupMemberReceiver_);
        if (original.ReceiverEpisode() != 0 &&
            original.ReceiverEpisode() <= groupMemberReceiver_.lastDetachedEpisode_) {
            // This exact attachment ended under real receiver quiescence.
            // Its old recipe exit cannot decrement a later attachment episode.
            assert(original.groupIdentity_ != nullptr && original.groupIdentity_->liveMembers != 0);
            --original.groupIdentity_->liveMembers;
            original = {};
        } else if (groupMemberReceiver_.owner_ != nullptr &&
            original.ReceiverEpisode() == groupMemberReceiver_.episode_) {
            groupMemberReceiver_.owner_->ReturnOriginalGroupMember(original);
        } else {
            assert(false && "an original member must retain its real receiver episode");
        }
    }
}

void ScheduledTransactionPool::ReturnAllGroupMemberOriginals() noexcept {
    ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get(), pagePlan03GroupMembers_.size());
    ReturnGroupMemberOriginals(immediateGroupMemberOriginals_.get(),
        static_cast<std::size_t>(config_.maxImmediateLaneGroupRows) * config_.maxArmsPerGroup);
    ReturnGroupMemberOriginals(replacementGroupMemberOriginals_.get(), config_.maxArmsPerGroup);
}

PrivateImmediateLaneGroupsPublicationProof::PrivateImmediateLaneGroupsPublicationProof(
    PrivateImmediateLaneGroupsPublicationProof&& other) noexcept { *this = std::move(other); }

PrivateImmediateLaneGroupsPublicationProof& PrivateImmediateLaneGroupsPublicationProof::operator=(
    PrivateImmediateLaneGroupsPublicationProof&& other) noexcept {
    if (this != &other) {
        Release();
        if (other.owner_ != nullptr) other.owner_->MovePrivateImmediateLaneGroupsPublication(other, *this);
    }
    return *this;
}

void PrivateImmediateLaneGroupsPublicationProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleasePrivateImmediateLaneGroupsPublication(*this);
}

bool ScheduledTransactionPool::InitialLaneCohortPublicationHeld(std::uint64_t root) const noexcept {
    for (auto* proof = laneGroupPublications_; proof != nullptr; proof = proof->next_)
        if (proof->root_ == root) return true;
    return false;
}

void ScheduledTransactionPool::MovePrivateImmediateLaneGroupsPublication(
    PrivateImmediateLaneGroupsPublicationProof& from,
    PrivateImmediateLaneGroupsPublicationProof& to) noexcept {
    auto** link = &laneGroupPublications_;
    while (*link != &from) link = &(*link)->next_;
    to.owner_ = this;
    to.lane_ = from.lane_;
    assert(to.lane_ != nullptr && to.lane_->groupsPublication_ == &from);
    to.lane_->groupsPublication_ = &to;
    to.root_ = from.root_;
    to.next_ = from.next_;
    *link = &to;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index)
        if (immediateLaneGroups_[index].publicationProof == &from)
            immediateLaneGroups_[index].publicationProof = &to;
    from.owner_ = nullptr;
    from.lane_ = nullptr;
    from.root_ = 0;
    from.next_ = nullptr;
}

void ScheduledTransactionPool::ReleasePrivateImmediateLaneGroupsPublication(
    PrivateImmediateLaneGroupsPublicationProof& proof, bool publish) noexcept {
    assert(proof.owner_ == this);
    auto** link = &laneGroupPublications_;
    while (*link != &proof) link = &(*link)->next_;
    *link = proof.next_;
    assert(proof.lane_ != nullptr && proof.lane_->groupsPublication_ == &proof);
    proof.lane_->groupsPublication_ = nullptr;
    proof.owner_ = nullptr;
    proof.lane_ = nullptr;
    proof.root_ = 0;
    proof.next_ = nullptr;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        auto& row = immediateLaneGroups_[index];
        if (row.publicationProof != &proof) continue;
        row.publicationProof = nullptr;
        row.lease.ReleasePublication();
        if (publish) row.state.store(LaneGroupState::Published, std::memory_order_release);
    }
}

bool ScheduledTransactionPool::ProvePrivateImmediateLaneGroupsPublication(
    PrivateImmediateLanePublicationProof& lane,
    PrivateImmediateLaneGroupsPublicationProof* out) noexcept {
    if (!Valid() || out == nullptr || out->Valid() || !lane.Valid() ||
        lane.attempt_ == nullptr || lane.attempt_->privatePublicationProof != &lane ||
        lane.groupsPublication_ != nullptr ||
        !lane.owner_->OwnsPrivate(lane.handle_, lane.attempt_->root) ||
        InitialLaneCohortPublicationHeld(lane.attempt_->root)) return false;
    const auto root = lane.attempt_->root;
    const auto pinCount = lane.owner_->PrivatePinGroupCount(lane.handle_);
    if (pinCount != lane.attempt_->groupCount ||
        (pinCount == 0 && lane.attempt_->declaredGroupCount != 0) ||
        (pinCount != 0 && (!lane.attempt_->groupPinsInstalled || lane.attempt_->pinGroupPool != this)))
        return false;
    std::size_t count = 0;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        if (row.cohortRoot != root || row.selectedFutureDomain != 0) continue;
        if (row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
            row.publicationProof != nullptr || row.transferOriginal != nullptr ||
            row.transferProof != nullptr || row.clockProof != nullptr ||
            row.laneOwner != lane.owner_ || row.laneSlot != lane.handle_.slot ||
            row.laneGeneration != lane.handle_.generation || !row.frameRebased ||
            row.bucketOrdinal == UINT32_MAX || row.groupIndex == UINT32_MAX ||
            row.readers.lastAudioSequence.load(std::memory_order_acquire) != 0) return false;
        Plan03GroupSliceIdentity identity{};
        if (!lane.owner_->CapturePrivatePinGroup(lane.handle_, row.groupIndex, &identity) ||
            identity.backing != Plan03GroupBacking::ImmediateLane ||
            identity.committedRow != index || identity.rowClaimGeneration != row.generation ||
            identity.transportEpoch != epoch_ || identity.absoluteQuantum != 0 || identity.pageGeneration != 1 ||
            identity.decision.slot != row.decision.slot || identity.decision.generation != row.decision.generation ||
            identity.sliceIndex != row.sliceIndex ||
            !lane.owner_->ProvePrivateGroupPlacement(lane.handle_, identity, row.bucketOrdinal, row.groupIndex))
            return false;
        const auto& actual = lane.owner_->GroupBase(lane.handle_.slot)[row.groupIndex];
        if (row.dueQuantum != actual.dueQuantum) return false;
        const auto lease = row.lease.Capture();
        const auto* effective = EffectiveImmediateLaneGroupLease(index);
        if (lease.state != AudioSliceState::Ready || lease.pendingReasonBits != 0 || lease.invalidateAfterSlice ||
            effective == nullptr || effective->Capture().state != AudioSliceState::Ready) return false;
        const auto& owner = row.frontier.operation.owner;
        if (!row.frontierPinsOwned || !row.frontier.Valid() || row.frontier.operation.group != identity ||
            row.frontier.operation.order.executeFrame != row.rebasedFrame ||
            owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
            row.decision.slot >= config_.maxImmediateLaneDecisionGates ||
            immediateLaneOwnerGates_[owner.serviceSlot].state.load(std::memory_order_acquire) != 0 ||
            immediateLaneDecisionGates_[row.decision.slot].state.load(std::memory_order_acquire) != 0)
            return false;
        ++count;
    }
    if (count != pinCount) return false;
    // The complete census passed before any shared primary lease changes state.
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        auto& row = immediateLaneGroups_[index];
        if (row.cohortRoot != root || row.selectedFutureDomain != 0) continue;
        if (!row.lease.HoldPublication()) {
            for (std::size_t prior = 0; prior < index; ++prior) {
                auto& held = immediateLaneGroups_[prior];
                if (held.publicationProof != out) continue;
                held.publicationProof = nullptr;
                held.lease.ReleasePublication();
            }
            return false;
        }
        row.publicationProof = out;
    }
    out->owner_ = this;
    out->lane_ = &lane;
    lane.groupsPublication_ = out;
    out->root_ = root;
    out->next_ = laneGroupPublications_;
    laneGroupPublications_ = out;
    return true;
}

bool ScheduledTransactionPool::IsPrivateImmediateLaneGroupsPublicationProofFor(
    const PrivateImmediateLaneGroupsPublicationProof& proof,
    const Plan03GroupSliceIdentity& identity, const FrontierEntryReceiptRef& entry) const noexcept {
    Plan03PageGroupView group{};
    if (proof.owner_ != this || proof.lane_ == nullptr || !proof.lane_->Valid() ||
        proof.lane_->groupsPublication_ != &proof ||
        !ResolveImmediateLaneGroup(identity, &group, false)) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    return row.publicationProof == &proof && row.cohortRoot == proof.root_ &&
        row.laneOwner == proof.lane_->owner_ && row.laneSlot == proof.lane_->handle_.slot &&
        row.laneGeneration == proof.lane_->handle_.generation &&
        row.state.load(std::memory_order_acquire) == LaneGroupState::Private &&
        row.lease.Capture().state == AudioSliceState::PublicationReserved &&
        group.lease->Capture().state == AudioSliceState::PublicationReserved &&
        group.frontier != nullptr && group.frontier->entry == entry &&
        group.frontier->operation.group == identity;
}

void ScheduledTransactionPool::ApplyPrivateImmediateLaneGroupsPublication(
    PrivateImmediateLaneGroupsPublicationProof&& proof) noexcept {
    assert(proof.Valid());
    if (proof.Valid()) proof.owner_->ReleasePrivateImmediateLaneGroupsPublication(proof, true);
}

ScheduledTransactionPool::~ScheduledTransactionPool() {
    assert(selectedFutureDomains_ == 0);
    // Device and producer quiescence precedes pool destruction.
    while (laneGroupPublications_ != nullptr) laneGroupPublications_->Release();
    while (unpublishedGroupArmCancellations_ != nullptr)
        unpublishedGroupArmCancellations_->Release();
    for (auto& row : pagePlan03Groups_) {
        if (row.cancellationOriginal == nullptr) continue;
        auto& original = *row.cancellationOriginal;
        original.owner_ = nullptr; original.rowIndex_ = SIZE_MAX; original.generation_ = 0;
        original.cancellationPending_ = false;
        row.cancellationOriginal = nullptr;
    }
    ReturnAllGroupMemberOriginals();
    if (immediateLaneGroups_ != nullptr)
        for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
            auto& row = immediateLaneGroups_[index];
            if (row.clockProof != nullptr) row.clockProof->Release();
            assert(row.originalRegistrationPinCount == 0);
            if (row.transferOriginal != nullptr)
                DropPrivateImmediateLaneGroupOriginal(row.transferOriginal);
            assert(row.originalFactoryMarker == 0);
        }
    for (auto& row : pagePlan03Groups_)
        if (row.frontierBindings != nullptr)
            ReleasePreparedPageFrontierBindings(*row.frontierBindings, true);
    while (privateUnboundPageArmRetirements_ != nullptr)
        privateUnboundPageArmRetirements_->Release();
    for (std::size_t slot = 0; slot < preparedTransactionClaims_.size(); ++slot)
        DetachPreparedPageBatchTransfer(slot);
    for (std::size_t row = 0; row < pagePlan03Groups_.size(); ++row)
        DetachPreparedPageGroupTransfer(row);
}

bool ScheduledTransactionPool::OwnsPreparedPageGroupOriginal(
    const PreparedPlan03GroupSlice& original) const noexcept {
    if (original.owner_ != this || !original.prepared_ || !original.transferSelected_ ||
        original.rowIndex_ >= pagePlan03Groups_.size() ||
        pagePlan03GroupRowStates_[original.rowIndex_] != Plan03GroupRowState::Prepared ||
        pagePlan03GroupClaimGenerations_[original.rowIndex_] != original.generation_) return false;
    const auto& row = pagePlan03Groups_[original.rowIndex_];
    const auto ring = original.rowIndex_ / config_.maxPlan03GroupHeadersPerPage;
    const auto& header = pageHeaders_[ring];
    const auto& identity = row.transferPreparedIdentity;
    return row.transferOriginal == &original && identity.committedRow == original.rowIndex_ &&
        identity.rowClaimGeneration == original.generation_ && identity.transportEpoch == epoch_ &&
        identity.absoluteQuantum == header.absoluteQuantumIndex &&
        identity.pageGeneration == header.pageGeneration && header.transportEpoch == epoch_ &&
        !header.frozen && identity.decision.slot == row.decision.slot &&
        identity.decision.generation == row.decision.generation && identity.sliceIndex == row.sliceIndex &&
        pagePreparedPlan03GroupCount_[ring] != 0;
}

bool ScheduledTransactionPool::PreparedPageGroupHeld(
    const PreparedPlan03GroupSlice& original) const noexcept {
    return OwnsPreparedPageGroupOriginal(original) &&
        pagePlan03Groups_[original.rowIndex_].transferProof != nullptr &&
        pagePlan03GroupLeases_[original.rowIndex_].Capture().state == AudioSliceState::PrivateTransferReserved;
}

void ScheduledTransactionPool::ReleasePreparedPageGroupTransferLease(
    PreparedPageGroupTransferProof* proof) noexcept {
    const auto& row = *static_cast<const Plan03GroupSliceRow*>(proof->row_);
    assert(row.transferProof == proof);
    pagePlan03GroupLeases_[row.transferPreparedIdentity.committedRow].ReleasePrivateTransfer();
}

void ScheduledTransactionPool::DetachPreparedPageGroupTransfer(std::size_t rowIndex) noexcept {
    auto& row = pagePlan03Groups_[rowIndex];
    if (row.transferProof != nullptr) row.transferProof->Release();
    if (row.transferOriginal != nullptr) {
        auto& original = *row.transferOriginal;
        original.owner_ = nullptr;
        original.rowIndex_ = SIZE_MAX;
        original.generation_ = 0;
        original.prepared_ = false;
        original.transferSelected_ = false;
    }
    row.transferOriginal = nullptr;
    row.transferPreparedIdentity = {};
    row.transferProspectiveIdentity = {};
    row.transferScope = {};
    row.transferExecuteFrame = 0;
    row.transferArmTable = nullptr;
    row.transferArmTableIncarnation = 0;
    row.transferMember = {};
    row.transferMemberIndex = 0;
}

void ScheduledTransactionPool::DropPreparedPageGroupOriginal(
    PreparedPlan03GroupSlice* original) noexcept {
    if (original->rowIndex_ < pagePlan03Groups_.size() &&
        pagePlan03Groups_[original->rowIndex_].transferOriginal == original) {
        DetachPreparedPageGroupTransfer(original->rowIndex_);
    }
    original->owner_ = nullptr;
    original->rowIndex_ = SIZE_MAX;
    original->generation_ = 0;
    original->prepared_ = false;
    original->transferSelected_ = false;
}

bool ScheduledTransactionPool::MatchesPreparedPageGroupInput(
    const PreparedPlan03GroupSlice& original, const PreparedPageGroupTransferScope& scope,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept {
    if (!OwnsPreparedPageGroupOriginal(original)) return false;
    const auto& row = pagePlan03Groups_[original.rowIndex_];
    if (!(row.transferScope == scope) || row.transferExecuteFrame != input.executeFrame ||
        row.transferArmTable != &armTable || row.transferArmTableIncarnation != armTable.Incarnation() ||
        plan03ArmTable_ != &armTable || row.decision.slot != input.decision.slot ||
        row.decision.generation != input.decision.generation || row.sliceIndex != input.sliceIndex ||
        row.memberCount != input.memberCount || input.memberCount > 1 ||
        row.sourceControlCount != 0 || input.sourceControlCount != 0 ||
        row.immediateEntry.gate != input.immediateEntry.gate ||
        row.immediateEntry.generation != input.immediateEntry.generation ||
        input.immediateEntry.gate != nullptr || input.immediateEntry.generation != 0 ||
        row.frontier.role != FrontierGroupRole::Legacy || row.frontierDependents != 0 ||
        row.frontierPinsOwned || row.publicationHeld || row.unpublishedRetirementStage != 0) return false;
    if (input.memberCount == 0) return true;
    if (input.members == nullptr) return false;
    const auto& token = input.members[0];
    const auto& saved = row.transferMember;
    const auto& actual = pagePlan03GroupMembers_[original.rowIndex_ * config_.maxArmsPerGroup];
    if (token.slot != saved.slot || token.generation != saved.generation ||
        token.renderSlot != saved.renderSlot || token.renderGeneration != saved.renderGeneration ||
        actual.slot != saved.slot || actual.generation != saved.generation ||
        actual.renderSlot != saved.renderSlot || actual.renderGeneration != saved.renderGeneration ||
        armTable.Resolve(token) == nullptr) return false;
    TimelineArmObservation observed{};
    return armTable.Observe(token, &observed) && observed.memberIndex == row.transferMemberIndex &&
        observed.parentGroup.decision.slot == row.decision.slot &&
        observed.parentGroup.decision.generation == row.decision.generation;
}

bool ScheduledTransactionPool::ProvePreparedPageGroupTransfer(
    PreparedPlan03GroupSlice& original, const PreparedPageGroupTransferScope& scope,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
    PreparedPageGroupTransferProof* out) noexcept {
    if (out == nullptr || out->row_ != nullptr || out->owner_ != nullptr ||
        !MatchesPreparedPageGroupInput(original, scope, input, armTable)) return false;
    const auto index = original.rowIndex_;
    auto& row = pagePlan03Groups_[index];
    auto& lease = pagePlan03GroupLeases_[index];
    const auto snapshot = lease.Capture();
    if (row.transferProof != nullptr || pagePlan03GroupReaders_[index].pins.load(std::memory_order_acquire) != 0 ||
        pagePlan03GroupReaders_[index].lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        snapshot.state != AudioSliceState::Ready || snapshot.pendingReasonBits != 0 ||
        snapshot.invalidateAfterSlice || !lease.HoldPrivateTransfer()) return false;
    row.transferProof = out;
    out->owner_ = this;
    out->row_ = &row;
    out->original_ = &original;
    return true;
}

ConsumedPreparedPageGroup ScheduledTransactionPool::ConsumePreparedPageGroupTransfer(
    PreparedPageGroupTransferProof&& proof) noexcept {
    assert(proof.row_ != nullptr && proof.original_ != nullptr);
    auto& row = *static_cast<Plan03GroupSliceRow*>(proof.row_);
    assert(row.frontierBindings == nullptr);
    assert(row.transferProof == &proof && row.transferOriginal == proof.original_);
    auto& pool = *proof.owner_;
    const auto index = proof.original_->rowIndex_;
    const auto ring = index / pool.config_.maxPlan03GroupHeadersPerPage;
    ConsumedPreparedPageGroup result{};
    result.identity = row.transferProspectiveIdentity;
    result.owner.owner_ = &pool;
    result.owner.rowIndex_ = index;
    result.owner.generation_ = result.identity.rowClaimGeneration;
    result.owner.privateFactory_ = true;
    result.owner.privateExecuteFrame_ = row.transferExecuteFrame;
    result.owner.privateArmTableIncarnation_ = row.transferArmTableIncarnation;
    --pool.pagePreparedPlan03GroupCount_[ring];
    ++pool.pagePlan03GroupCount_[ring];
    ++pool.pagePublicationHolds_[ring];
    row.publicationHeld = true;
    pool.pagePlan03GroupRowStates_[index] = Plan03GroupRowState::Live;
    pool.pagePlan03GroupClaimGenerations_[index] = result.identity.rowClaimGeneration;
    if (row.memberCount != 0)
        pool.pageGroupIdentities_[index].group.rowClaimGeneration = pool.pagePlan03GroupClaimGenerations_[index];
    pool.plan03ArmTableEstablishedByPrepared_ = false;
    // Retain the real publication hold before releasing the original lease.
    pool.DetachPreparedPageGroupTransfer(index);
    return result;
}

PreparedPrivateImmediateLaneGroup::~PreparedPrivateImmediateLaneGroup() {
    if (owner_ != nullptr) owner_->DropPrivateImmediateLaneGroupOriginal(this);
}

PreparedPrivateImmediateLaneGroup::PreparedPrivateImmediateLaneGroup(
    PreparedPrivateImmediateLaneGroup&& other) noexcept {
    *this = std::move(other);
}

PreparedPrivateImmediateLaneGroup& PreparedPrivateImmediateLaneGroup::operator=(
    PreparedPrivateImmediateLaneGroup&& other) noexcept {
    if (this == &other || Held() || other.Held()) return *this;
    if (owner_ != nullptr) owner_->DropPrivateImmediateLaneGroupOriginal(this);
    // Cleanup ownership follows the original after native publication.
    // It does not restore the private transfer grant.
    Plan03PageGroupView cleanup{};
    const bool associated = other.owner_ != nullptr && other.originalMarker_ != 0 &&
        other.owner_->ResolveImmediateLaneGroup(other.identity_, &cleanup, false) &&
        other.owner_->immediateLaneGroups_[other.identity_.committedRow].transferOriginal == &other &&
        other.owner_->immediateLaneGroups_[other.identity_.committedRow].originalFactoryMarker ==
            other.originalMarker_;
    owner_ = other.owner_;
    identity_ = other.identity_;
    cohortRoot_ = other.cohortRoot_;
    selectedFutureDomain_ = other.selectedFutureDomain_;
    provisionalAnchorFrame_ = other.provisionalAnchorFrame_;
    executeFrame_ = other.executeFrame_;
    memberCount_ = other.memberCount_;
    sourceControlCount_ = other.sourceControlCount_;
    immediateEntry_ = other.immediateEntry_;
    armTable_ = other.armTable_;
    armTableIncarnation_ = other.armTableIncarnation_;
    originalMarker_ = std::exchange(other.originalMarker_, 0);
    if (associated) owner_->immediateLaneGroups_[identity_.committedRow].transferOriginal = this;
    other.owner_ = nullptr;
    other.identity_ = {};
    return *this;
}

bool PreparedPrivateImmediateLaneGroup::Valid() const noexcept {
    return owner_ != nullptr && owner_->OwnsPrivateImmediateLaneGroupOriginal(*this);
}

bool PreparedPrivateImmediateLaneGroup::Held() const noexcept {
    return owner_ != nullptr && owner_->PrivateImmediateLaneGroupTransferHeld(*this);
}

PrivateImmediateLaneGroupTransferProof::PrivateImmediateLaneGroupTransferProof(
    PrivateImmediateLaneGroupTransferProof&& other) noexcept {
    *this = std::move(other);
}

PrivateImmediateLaneGroupTransferProof& PrivateImmediateLaneGroupTransferProof::operator=(
    PrivateImmediateLaneGroupTransferProof&& other) noexcept {
    // Held proof storage is stable until Release or its one direct Consume.
    if (this == &other || row_ != nullptr || other.row_ != nullptr) return *this;
    return *this;
}

bool PrivateImmediateLaneGroupTransferProof::Valid() const noexcept {
    if (owner_ == nullptr || row_ == nullptr || original_ == nullptr) return false;
    const auto& row = *static_cast<const ScheduledTransactionPool::LaneGroupRow*>(row_);
    return row.transferProof == this && row.transferOriginal == original_ &&
        row.generation == identity_.rowClaimGeneration && original_->Valid();
}

bool PrivateImmediateLaneGroupTransferProof::MatchesOriginalScope(
    const PreparedPrivateImmediateLaneGroup& original,
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept {
    return Valid() && original_ == &original &&
        owner_->MatchesPrivateImmediateLaneGroupInput(original, cohortRoot,
            provisionalAnchorFrame, input, armTable);
}

void PrivateImmediateLaneGroupTransferProof::Release() noexcept {
    if (row_ != nullptr) {
        auto& row = *static_cast<ScheduledTransactionPool::LaneGroupRow*>(row_);
        assert(row.transferProof == this);
        owner_->ReleasePrivateImmediateLaneGroupTransferLease(this);
        row.transferProof = nullptr;
    }
    owner_ = nullptr;
    row_ = nullptr;
    original_ = nullptr;
    identity_ = {};
}

bool ScheduledTransactionPool::OwnsPrivateImmediateLaneGroupOriginal(
    const PreparedPrivateImmediateLaneGroup& original) const noexcept {
    Plan03PageGroupView view{};
    if (original.owner_ != this ||
        !ResolveImmediateLaneGroup(original.identity_, &view, false)) return false;
    const auto& row = immediateLaneGroups_[original.identity_.committedRow];
    return original.originalMarker_ != 0 &&
        row.originalFactoryMarker == original.originalMarker_ &&
        row.transferOriginal == &original && row.cohortRoot == original.cohortRoot_ &&
        row.selectedFutureDomain == original.selectedFutureDomain_ &&
        row.state.load(std::memory_order_acquire) == LaneGroupState::Private &&
        row.relativeFrame == original.executeFrame_ - original.provisionalAnchorFrame_ &&
        row.memberCount == original.memberCount_ &&
        row.sourceControlCount == original.sourceControlCount_ &&
        plan03ArmTable_ == original.armTable_ && original.armTableIncarnation_ != 0 &&
        original.armTable_->Incarnation() == original.armTableIncarnation_;
}

void ScheduledTransactionPool::ReleasePrivateImmediateLaneGroupTransferLease(
    PrivateImmediateLaneGroupTransferProof* proof) noexcept {
    auto& row = *static_cast<LaneGroupRow*>(proof->row_);
    assert(row.transferProof == proof);
    row.lease.ReleasePrivateTransfer();
}

bool ScheduledTransactionPool::PrivateImmediateLaneGroupTransferHeld(
    const PreparedPrivateImmediateLaneGroup& original) const noexcept {
    return OwnsPrivateImmediateLaneGroupOriginal(original) &&
        immediateLaneGroups_[original.identity_.committedRow].transferProof != nullptr;
}

bool ScheduledTransactionPool::PrivateImmediateLaneGroupTransferHeld(
    const Plan03GroupSliceIdentity& identity) const noexcept {
    Plan03PageGroupView view{};
    return ResolveImmediateLaneGroup(identity, &view, false) &&
        immediateLaneGroups_[identity.committedRow].transferProof != nullptr;
}

void ScheduledTransactionPool::DropPrivateImmediateLaneGroupOriginal(
    PreparedPrivateImmediateLaneGroup* original) noexcept {
    Plan03PageGroupView view{};
    if (original->owner_ == this &&
        ResolveImmediateLaneGroup(original->identity_, &view, false) &&
        immediateLaneGroups_[original->identity_.committedRow].transferOriginal == original) {
        auto& row = immediateLaneGroups_[original->identity_.committedRow];
        if (row.transferProof != nullptr) row.transferProof->Release();
        assert(row.originalRegistrationPinCount == 0);
        row.transferOriginal = nullptr;
        row.originalFactoryMarker = 0;
    }
    original->owner_ = nullptr;
    original->identity_ = {};
    original->originalMarker_ = 0;
}

bool ScheduledTransactionPool::MatchesPrivateImmediateLaneGroupInput(
    const PreparedPrivateImmediateLaneGroup& original,
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept {
    if (!OwnsPrivateImmediateLaneGroupOriginal(original) ||
        original.cohortRoot_ != cohortRoot ||
        original.provisionalAnchorFrame_ != provisionalAnchorFrame ||
        original.executeFrame_ != input.executeFrame || original.armTable_ != &armTable ||
        original.identity_.decision.slot != input.decision.slot ||
        original.identity_.decision.generation != input.decision.generation ||
        original.identity_.sliceIndex != input.sliceIndex ||
        original.memberCount_ != input.memberCount ||
        original.sourceControlCount_ != input.sourceControlCount ||
        original.immediateEntry_.gate != input.immediateEntry.gate ||
        original.immediateEntry_.generation != input.immediateEntry.generation ||
        (input.memberCount != 0 && input.members == nullptr)) return false;
    const auto* actual = immediateLaneGroupMembers_.get() +
        original.identity_.committedRow * config_.maxArmsPerGroup;
    const auto* originalIndices = immediateLaneGroupMemberIndices_.get() +
        original.identity_.committedRow * config_.maxArmsPerGroup;
    for (std::uint16_t index = 0; index < input.memberCount; ++index) {
        TimelineArmObservation expected{};
        if (armTable.Resolve(input.members[index]) == nullptr ||
            !armTable.Observe(input.members[index], &expected) ||
            expected.parentGroup.decision.slot != original.identity_.decision.slot ||
            expected.parentGroup.decision.generation != original.identity_.decision.generation)
            return false;
        std::uint16_t position = input.memberCount;
        for (std::uint16_t candidate = 0; candidate < input.memberCount; ++candidate) {
            if (actual[candidate].slot == input.members[index].slot &&
                actual[candidate].generation == input.members[index].generation &&
                actual[candidate].renderSlot == input.members[index].renderSlot &&
                actual[candidate].renderGeneration == input.members[index].renderGeneration) {
                position = candidate;
                break;
            }
        }
        if (position == input.memberCount || expected.memberIndex != originalIndices[position])
            return false;
        for (std::uint16_t prior = 0; prior < index; ++prior)
            if (input.members[prior].slot == input.members[index].slot &&
                input.members[prior].generation == input.members[index].generation) return false;
    }
    return true;
}

bool ScheduledTransactionPool::MatchesHeldPrivateImmediateLaneGroupOriginal(
    const PreparedPrivateImmediateLaneGroup& original,
    const PrivateImmediateLaneGroupTransferProof& proof,
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input,
    const TimelineArmTable& armTable) const noexcept {
    return original.owner_ == this && proof.owner_ == this &&
        proof.original_ == &original &&
        proof.MatchesOriginalScope(original, cohortRoot,
            provisionalAnchorFrame, input, armTable);
}

bool ScheduledTransactionPool::ProvePrivateImmediateLaneGroupTransfer(
    PreparedPrivateImmediateLaneGroup& original, std::uint64_t cohortRoot,
    std::uint64_t provisionalAnchorFrame, const Plan03GroupSliceInput& input,
    const TimelineArmTable& armTable, PrivateImmediateLaneGroupTransferProof* out) noexcept {
    if (out == nullptr || out->row_ != nullptr || out->owner_ != nullptr ||
        !MatchesPrivateImmediateLaneGroupInput(original, cohortRoot,
            provisionalAnchorFrame, input, armTable)) return false;
    auto& row = immediateLaneGroups_[original.identity_.committedRow];
    if (row.transferProof != nullptr || row.laneOwner != nullptr ||
        row.laneSlot != UINT32_MAX || row.laneGeneration != 0 || row.frameRebased ||
        row.bucketOrdinal != UINT32_MAX || row.groupIndex != UINT32_MAX ||
        row.frontier.role != FrontierGroupRole::Legacy || row.frontierPinsOwned ||
        row.frontierDependents != 0 || row.readers.pins.load(std::memory_order_acquire) != 0 ||
        row.readers.lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        row.lease.Capture().state != AudioSliceState::Ready ||
        !row.lease.HoldPrivateTransfer()) return false;
    row.transferProof = out;
    out->owner_ = this;
    out->row_ = &row;
    out->original_ = &original;
    out->identity_ = original.identity_;
    return true;
}

Plan03GroupSliceIdentity ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(
    PrivateImmediateLaneGroupTransferProof&& proof) noexcept {
    assert(proof.row_ != nullptr && proof.original_ != nullptr);
    auto& row = *static_cast<LaneGroupRow*>(proof.row_);
    assert(row.transferProof == &proof && row.transferOriginal == proof.original_);
    const auto identity = proof.identity_;
    assert(row.originalRegistrationPinCount == 0);
    row.originalFactoryMarker = 0;
    row.transferOriginal = nullptr;
    proof.original_->owner_ = nullptr;
    proof.original_->identity_ = {};
    proof.original_->originalMarker_ = 0;
    proof.Release();
    return identity;
}

bool ScheduledTransactionPool::CancelPrivateImmediateLaneGroup(
    PreparedPrivateImmediateLaneGroup* original, Plan03GroupSliceIdentity* rawHolder) noexcept {
    if (original == nullptr || rawHolder == nullptr ||
        !OwnsPrivateImmediateLaneGroupOriginal(*original) ||
        !(*rawHolder == original->identity_) ||
        !CancelPrivateImmediateLaneGroupInDomain(original->cohortRoot_, original->identity_,
            nullptr, original->selectedFutureDomain_)) return false;
    original->owner_ = nullptr;
    original->identity_ = {};
    original->originalMarker_ = 0;
    *rawHolder = {};
    return true;
}

PrivateOriginalQueueGroupClockProof::PrivateOriginalQueueGroupClockProof(
    PrivateOriginalQueueGroupClockProof&& other) noexcept { Take(other); }

PrivateOriginalQueueGroupClockProof& PrivateOriginalQueueGroupClockProof::operator=(
    PrivateOriginalQueueGroupClockProof&& other) noexcept {
    if (this != &other) { Release(); Take(other); }
    return *this;
}

void PrivateOriginalQueueGroupClockProof::Take(
    PrivateOriginalQueueGroupClockProof& other) noexcept {
    if (other.owner_ == nullptr) return;
    owner_ = other.owner_;
    rows_ = other.rows_;
    leases_ = other.leases_;
    rowCount_ = other.rowCount_;
    leaseCount_ = other.leaseCount_;
    root_ = other.root_;
    anchor_ = other.anchor_;
    placement_ = std::move(other.placement_);
    originals_ = std::move(other.originals_);
    // Cleanup association moves even if an enclosing Lane has ended its proof.
    owner_->RetargetPrivateOriginalQueueGroupClockProof(other, *this);
    other.owner_ = nullptr;
    other.rows_ = {};
    other.leases_ = {};
    other.rowCount_ = other.leaseCount_ = 0;
    other.root_ = other.anchor_ = 0;
}

bool PrivateOriginalQueueGroupClockProof::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesPrivateOriginalQueueGroupClockProof(*this);
}

bool PrivateOriginalQueueGroupClockProof::CaptureAnchorFrame(std::uint64_t* out) const noexcept {
    if (out == nullptr || !Valid()) return false;
    *out = anchor_;
    return true;
}

bool PrivateOriginalQueueGroupClockProof::ReturnPlacement(
    PrivateImmediateLanePlacementProof* out) noexcept {
    if (out == nullptr || out->Valid() || owner_ == nullptr || !placement_.Valid()) return false;
    owner_->ReturnPrivateOriginalQueueGroupClockProof(*this, out);
    return true;
}

void PrivateOriginalQueueGroupClockProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReturnPrivateOriginalQueueGroupClockProof(*this);
}

void ScheduledTransactionPool::RetargetPrivateOriginalQueueGroupClockProof(
    PrivateOriginalQueueGroupClockProof& from, PrivateOriginalQueueGroupClockProof& to) noexcept {
    assert(from.owner_ == this && to.owner_ == this && from.rowCount_ != 0);
    for (std::uint32_t i = 0; i < from.rowCount_; ++i) {
        const auto& identity = from.rows_[i];
        assert(identity.transportEpoch == epoch_ &&
            identity.committedRow < config_.maxImmediateLaneGroupRows);
        auto& row = immediateLaneGroups_[identity.committedRow];
        assert(row.clockProof == &from && row.generation == identity.rowClaimGeneration &&
            row.decision.slot == identity.decision.slot &&
            row.decision.generation == identity.decision.generation && row.sliceIndex == identity.sliceIndex &&
            row.state.load(std::memory_order_acquire) == LaneGroupState::Private);
        row.clockProof = &to;
    }
}

void ScheduledTransactionPool::ReturnPrivateOriginalQueueGroupClockProof(
    PrivateOriginalQueueGroupClockProof& proof, PrivateImmediateLanePlacementProof* returnPlacement) noexcept {
    assert(proof.owner_ == this && proof.rowCount_ != 0 && proof.leaseCount_ != 0);
    for (std::uint32_t i = 0; i < proof.rowCount_; ++i) {
        const auto& identity = proof.rows_[i];
        assert(identity.transportEpoch == epoch_ &&
            identity.committedRow < config_.maxImmediateLaneGroupRows);
        auto& row = immediateLaneGroups_[identity.committedRow];
        assert(row.clockProof == &proof && row.generation == identity.rowClaimGeneration &&
            row.decision.slot == identity.decision.slot &&
            row.decision.generation == identity.decision.generation && row.sliceIndex == identity.sliceIndex &&
            row.state.load(std::memory_order_acquire) == LaneGroupState::Private);
        row.clockProof = nullptr;
    }
    for (std::uint32_t i = 0; i < proof.leaseCount_; ++i)
        proof.leases_[i]->ReleasePrivateTransfer();
    proof.owner_ = nullptr;
    proof.rows_ = {};
    proof.leases_ = {};
    proof.rowCount_ = proof.leaseCount_ = 0;
    proof.root_ = proof.anchor_ = 0;
    for (auto& original : proof.originals_) original.Release();
    if (returnPlacement != nullptr) *returnPlacement = std::move(proof.placement_);
    else proof.placement_.Release();
}

bool ScheduledTransactionPool::MatchesPrivateOriginalQueueGroupClockProof(
    const PrivateOriginalQueueGroupClockProof& proof) const noexcept {
    if (proof.owner_ != this || proof.rowCount_ < 2 || proof.rowCount_ > 4 ||
        proof.leaseCount_ == 0 || proof.leaseCount_ > 2 || !proof.placement_.Valid() ||
        !proof.originals_[0].Valid() || !proof.originals_[1].Valid()) return false;
    std::uint64_t actualAnchor = 0;
    if (!proof.placement_.CaptureAnchorFrame(&actualAnchor) || actualAnchor != proof.anchor_)
        return false;
    for (std::uint32_t i = 0; i < proof.rowCount_; ++i) {
        const auto& identity = proof.rows_[i];
        Plan03PageGroupView actual{};
        if (!ResolveImmediateLaneGroup(identity, &actual, false)) return false;
        const auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.clockProof != &proof || row.cohortRoot != proof.root_ ||
            row.selectedFutureDomain != immediateLaneGroups_[proof.originals_[0].Identity().committedRow].selectedFutureDomain ||
            row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
            row.laneOwner == nullptr || !row.frameRebased ||
            row.relativeFrame > UINT64_MAX - proof.anchor_ ||
            row.rebasedFrame != proof.anchor_ + row.relativeFrame ||
            !row.laneOwner->IsPrivatePlacementProofFor(proof.placement_,
                {row.laneSlot, row.laneGeneration}, proof.root_)) return false;
    }
    for (const auto& original : proof.originals_) {
        auto* const actualLease = EffectiveImmediateLaneGroupLease(original.identity_.committedRow);
        bool selected = false;
        for (std::uint32_t i = 0; i < proof.leaseCount_; ++i)
            selected = selected || proof.leases_[i] == actualLease;
        if (!selected) return false;
    }
    for (std::uint32_t i = 0; i < proof.leaseCount_; ++i) {
        const auto lease = proof.leases_[i]->Capture();
        if (lease.state != AudioSliceState::PrivateTransferReserved ||
            lease.pendingReasonBits != 0 || lease.invalidateAfterSlice) return false;
    }
    return true;
}

bool ScheduledTransactionPool::CapturePrivateOriginalQueueClockInitialSource(
    const PrivateOriginalQueueGroupClockProof& proof,
    const OriginalQueueFutureGroupLifetimePin& c0,
    const OriginalQueueFutureGroupLifetimePin& c1,
    const TimelineArmTable& arms, const DecisionTable& decisions,
    TimelineArmToken source, std::array<std::uint64_t, 4>* outFrames,
    const FrontierGroupBinding** outInitial,
    const FrontierGroupBinding** outConditional) const noexcept {
    if (outFrames == nullptr || outInitial == nullptr || outConditional == nullptr ||
        !MatchesPrivateOriginalQueueGroupClockProof(proof) || plan03ArmTable_ != &arms ||
        !MatchesOriginalQueueFutureGroupLifetime(c0) || !MatchesOriginalQueueFutureGroupLifetime(c1) ||
        c0.identity_ != proof.originals_[0].identity_ || c1.identity_ != proof.originals_[1].identity_ ||
        c0.marker_ != proof.originals_[0].marker_ || c1.marker_ != proof.originals_[1].marker_) return false;
    // Structural Valid is insufficient after an asynchronous owner/Decision stop.
    for (std::uint32_t index = 0; index < proof.rowCount_; ++index) {
        const auto& identity = proof.rows_[index];
        const auto& row = immediateLaneGroups_[identity.committedRow];
        const auto& owner = row.frontier.operation.owner;
        DecisionCommittedSnapshot nativeDecision{};
        if (!row.frontier.Valid() || owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
            identity.decision.slot >= config_.maxImmediateLaneDecisionGates ||
            !decisions.CaptureCommitted(identity.decision, &nativeDecision) ||
            nativeDecision.world != row.frontier.operation.world ||
            nativeDecision.transport != row.frontier.operation.transport ||
            nativeDecision.sliceCount <= identity.sliceIndex ||
            nativeDecision.audioState != AudioGroupState::Pending ||
            nativeDecision.authoredStopInvalidated || nativeDecision.consumedSlices != 0 ||
            nativeDecision.lastAudioSequence != 0 ||
            nativeDecision.ticket.sequence != row.frontier.operation.order.logicCommitSequence) return false;
        const auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
        const auto& decisionGate = immediateLaneDecisionGates_[identity.decision.slot];
        if (ownerGate.generation.load(std::memory_order_acquire) != owner.serviceGeneration ||
            decisionGate.generation.load(std::memory_order_acquire) != identity.decision.generation ||
            decisionGate.ownerSlot.load(std::memory_order_acquire) != owner.serviceSlot ||
            decisionGate.ownerGeneration.load(std::memory_order_acquire) != owner.serviceGeneration ||
            ((ownerGate.state.load(std::memory_order_acquire) |
              decisionGate.state.load(std::memory_order_acquire)) & (kLaneGateClaims | kLaneGateInvalid)) != 0)
            return false;
    }
    Plan03PageGroupView initial{}, conditional{};
    if (!ResolveImmediateLaneGroup(c0.identity_, &initial, false) ||
        !ResolveImmediateLaneGroup(c1.identity_, &conditional, false) ||
        initial.frontier == nullptr || conditional.frontier == nullptr ||
        !initial.frontier->queueInitial.Valid() || initial.frontier->queueConditional.Valid() ||
        initial.frontier->variableSide.Valid() ||
        !conditional.frontier->queueConditional.Valid() || conditional.frontier->queueInitial.Valid() ||
        conditional.frontier->variableSide.Valid() ||
        conditional.frontier->role != FrontierGroupRole::EntryMember ||
        conditional.frontier->operation.kind != FrontierOperationKind::Dependent ||
        !(initial.frontier->operation.owner == conditional.frontier->operation.owner) ||
        initial.frontier->operation.world != conditional.frontier->operation.world ||
        initial.frontier->operation.transport != conditional.frontier->operation.transport) return false;
    const auto& first = immediateLaneGroups_[c0.identity_.committedRow];
    const auto& second = immediateLaneGroups_[c1.identity_.committedRow];
    if (first.laneOwner != second.laneOwner || first.laneSlot != second.laneSlot ||
        first.laneGeneration != second.laneGeneration || first.rebasedFrame > second.rebasedFrame) return false;
    std::uint64_t actualSourceFrame = 0;
    if (!first.laneOwner->CapturePrivatePlacedInitialSource(proof.placement_, *this,
            arms, decisions, *initial.frontier, source, &actualSourceFrame)) return false;
    const std::array<std::uint64_t, 4> frames{
        proof.anchor_, first.rebasedFrame, second.rebasedFrame, actualSourceFrame};
    *outFrames = frames;
    *outInitial = initial.frontier;
    *outConditional = conditional.frontier;
    return true;
}

bool ScheduledTransactionPool::CapturePrivateOriginalQueueC0Source(
    const PrivateOriginalQueueGroupClockProof& proof,
    const OriginalQueueFutureGroupLifetimePin& primaryOriginal,
    const OriginalQueueFutureGroupLifetimePin& initialOriginal,
    const TimelineArmTable& arms, const DecisionTable& decisions,
    TimelineArmToken source, std::array<std::uint64_t, 3>* outFrames,
    const FrontierGroupBinding** outPrimary,
    const FrontierGroupBinding** outInitial) const noexcept {
    if (outFrames == nullptr || outPrimary == nullptr || outInitial == nullptr ||
        !MatchesPrivateOriginalQueueGroupClockProof(proof) || proof.rowCount_ != 2u ||
        proof.leaseCount_ != 1u || plan03ArmTable_ != &arms ||
        !MatchesOriginalQueueFutureGroupLifetime(primaryOriginal) ||
        !MatchesOriginalQueueFutureGroupLifetime(initialOriginal) ||
        primaryOriginal.identity_ != proof.originals_[0].identity_ ||
        initialOriginal.identity_ != proof.originals_[1].identity_ ||
        primaryOriginal.marker_ != proof.originals_[0].marker_ ||
        initialOriginal.marker_ != proof.originals_[1].marker_) return false;
    // Structural Valid is insufficient after an asynchronous owner/Decision stop.
    for (std::uint32_t index = 0; index < proof.rowCount_; ++index) {
        const auto& identity = proof.rows_[index];
        const auto& row = immediateLaneGroups_[identity.committedRow];
        const auto& owner = row.frontier.operation.owner;
        DecisionCommittedSnapshot nativeDecision{};
        if (!row.frontier.Valid() || owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
            identity.decision.slot >= config_.maxImmediateLaneDecisionGates ||
            !decisions.CaptureCommitted(identity.decision, &nativeDecision) ||
            nativeDecision.world != row.frontier.operation.world ||
            nativeDecision.transport != row.frontier.operation.transport ||
            nativeDecision.sliceCount <= identity.sliceIndex ||
            nativeDecision.audioState != AudioGroupState::Pending ||
            nativeDecision.authoredStopInvalidated || nativeDecision.consumedSlices != 0 ||
            nativeDecision.lastAudioSequence != 0 ||
            nativeDecision.ticket.sequence != row.frontier.operation.order.logicCommitSequence) return false;
        const auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
        const auto& decisionGate = immediateLaneDecisionGates_[identity.decision.slot];
        if (ownerGate.generation.load(std::memory_order_acquire) != owner.serviceGeneration ||
            decisionGate.generation.load(std::memory_order_acquire) != identity.decision.generation ||
            decisionGate.ownerSlot.load(std::memory_order_acquire) != owner.serviceSlot ||
            decisionGate.ownerGeneration.load(std::memory_order_acquire) != owner.serviceGeneration ||
            ((ownerGate.state.load(std::memory_order_acquire) |
              decisionGate.state.load(std::memory_order_acquire)) & (kLaneGateClaims | kLaneGateInvalid)) != 0)
            return false;
    }
    Plan03PageGroupView primary{}, initial{};
    if (!ResolveImmediateLaneGroup(primaryOriginal.identity_, &primary, false) ||
        !ResolveImmediateLaneGroup(initialOriginal.identity_, &initial, false) ||
        primary.frontier == nullptr || initial.frontier == nullptr ||
        primary.memberCount != 0 || primary.sourceControlCount != 0 ||
        primary.frontier->role != FrontierGroupRole::Coordinator ||
        primary.frontier->operation.kind != FrontierOperationKind::Entry ||
        primary.frontier->primary != primaryOriginal.identity_ ||
        primary.frontier->operation.group != primaryOriginal.identity_ ||
        primary.frontier->queueInitial.Valid() || primary.frontier->queueConditional.Valid() ||
        primary.frontier->variableSide.Valid() ||
        initial.frontier->role != FrontierGroupRole::EntryMember ||
        initial.frontier->operation.kind != FrontierOperationKind::Entry ||
        initial.frontier->primary != primaryOriginal.identity_ ||
        initial.frontier->operation.group != initialOriginal.identity_ ||
        !(initial.frontier->entry == primary.frontier->entry) ||
        !initial.frontier->queueInitial.Valid() || initial.frontier->queueConditional.Valid() ||
        initial.frontier->variableSide.Valid() ||
        initial.frontier->operation.world != primary.frontier->operation.world ||
        initial.frontier->operation.transport != primary.frontier->operation.transport ||
        primary.lease == nullptr || initial.lease != primary.lease) return false;
    const auto& first = immediateLaneGroups_[primaryOriginal.identity_.committedRow];
    const auto& second = immediateLaneGroups_[initialOriginal.identity_.committedRow];
    if (first.selectedFutureDomain != 0 || second.selectedFutureDomain != 0 ||
        first.laneOwner == nullptr || first.laneOwner != second.laneOwner ||
        first.laneSlot != second.laneSlot || first.laneGeneration != second.laneGeneration ||
        first.rebasedFrame != second.rebasedFrame) return false;
    std::uint64_t actualSourceFrame = 0;
    if (!first.laneOwner->CapturePrivatePlacedC0Source(proof.placement_, *this,
            arms, decisions, *primary.frontier, *initial.frontier, source,
            &actualSourceFrame)) return false;
    const std::array<std::uint64_t, 3> frames{
        proof.anchor_, second.rebasedFrame, actualSourceFrame};
    *outFrames = frames;
    *outPrimary = primary.frontier;
    *outInitial = initial.frontier;
    return true;
}

bool ScheduledTransactionPool::IsPrivateOriginalQueueGroupClockProofFor(
    const PrivateOriginalQueueGroupClockProof& proof,
    const OriginalQueueFutureGroupRegistration& first,
    const OriginalQueueFutureGroupRegistration& second) const noexcept {
    return MatchesPrivateOriginalQueueGroupClockProof(proof) &&
        IsOriginalQueueFutureGroupRegistrationFor(first, proof.originals_[0].identity_, proof.root_) &&
        IsOriginalQueueFutureGroupRegistrationFor(second, proof.originals_[1].identity_, proof.root_) &&
        first.lifetime_.marker_ == proof.originals_[0].marker_ &&
        second.lifetime_.marker_ == proof.originals_[1].marker_;
}

PrivateSelectedQueueFutureDomain::PrivateSelectedQueueFutureDomain(
    PrivateSelectedQueueFutureDomain&& other) noexcept { *this = std::move(other); }
PrivateSelectedQueueFutureDomain& PrivateSelectedQueueFutureDomain::operator=(
    PrivateSelectedQueueFutureDomain&& other) noexcept {
    if (this != &other) {
        // An attached immovable calendar holds this exact owner address.
        assert(other.calendar_ == nullptr);
        if (other.calendar_ != nullptr) return *this;
        const bool returned = Return(); assert(returned);
        if (returned) {
            oldC0_ = std::move(other.oldC0_);
            owner_ = std::exchange(other.owner_, nullptr);
            lane_ = std::exchange(other.lane_, nullptr);
            laneSlot_ = std::exchange(other.laneSlot_, UINT32_MAX);
            laneGeneration_ = std::exchange(other.laneGeneration_, 0);
            marker_ = std::exchange(other.marker_, 0);
        }
    }
    return *this;
}
PrivateSelectedQueueFutureDomain::~PrivateSelectedQueueFutureDomain() {
    const bool returned = Return(); assert(returned); (void)returned;
}
bool ScheduledTransactionPool::PreparePrivateSelectedQueueFutureDomain(
    const OriginalQueueFutureGroupRegistration& oldC0,
    const OriginalQueueFutureGroupRegistration& oldC1,
    const LogicCommitCoordinator::PreparedIssue& sequence,
    ImmediateActionLane& distinctNewLane,
    PrivateSelectedQueueFutureDomain* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || !sequence.Valid() ||
        !IsOriginalQueueFutureGroupFamily(oldC0, oldC1) || oldC0.selectedFutureDomain_ != 0 ||
        !distinctNewLane.Valid() || nextSelectedFutureDomain_ == UINT64_MAX ||
        selectedFutureDomains_ == UINT32_MAX) return false;
    const auto& c0 = immediateLaneGroups_[oldC0.Identity().committedRow];
    const auto& c1 = immediateLaneGroups_[oldC1.Identity().committedRow];
    if (c0.laneOwner == nullptr || c0.laneOwner != c1.laneOwner ||
        c0.laneOwner == &distinctNewLane || c0.laneSlot != c1.laneSlot ||
        c0.laneGeneration != c1.laneGeneration) return false;
    if (!oldC0.BorrowLifetime(&out->oldC0_)) return false;
    out->marker_ = ++nextSelectedFutureDomain_;
    out->owner_ = this;
    out->lane_ = &distinctNewLane;
    ++selectedFutureDomains_;
    return true;
}
bool ScheduledTransactionPool::PreparePrivateSelectedQueueFutureDomain(
    const OriginalQueueFutureGroupRegistration& oldC0,
    const OriginalQueueFutureGroupRegistration& oldC1,
    const OriginalImmediateLaneAttemptLifetimePin& originalAttempt,
    const PreparedQueueReplacementOrder& order,
    ImmediateActionLane& newLane,
    PrivateSelectedQueueFutureDomain* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || !order.Valid() ||
        !IsOriginalQueueFutureGroupFamily(oldC0, oldC1) || oldC0.selectedFutureDomain_ != 0 ||
        !newLane.Valid() || nextSelectedFutureDomain_ == UINT64_MAX ||
        selectedFutureDomains_ == UINT32_MAX) return false;
    const auto& c0 = immediateLaneGroups_[oldC0.Identity().committedRow];
    const auto& c1 = immediateLaneGroups_[oldC1.Identity().committedRow];
    // The genuine C0 member has not been filed before the first Source ACK.
    // The same factory's C1 already owns the original attempt; its lifetime
    // proves the receiver while the original C0 remains private and unbound.
    const bool unfiledC0 = c0.laneOwner == nullptr && c0.laneSlot == UINT32_MAX &&
        c0.laneGeneration == 0 && c0.state.load(std::memory_order_acquire) == LaneGroupState::Private;
    if (c1.laneOwner == nullptr ||
        (!unfiledC0 && (c0.laneOwner != c1.laneOwner || c0.laneSlot != c1.laneSlot ||
            c0.laneGeneration != c1.laneGeneration)) ||
        !c1.laneOwner->IsOriginalAttemptLifetimePinFor(originalAttempt,
            {c1.laneSlot, c1.laneGeneration}, c1.cohortRoot)) return false;
    if (!oldC0.BorrowLifetime(&out->oldC0_)) return false;
    out->marker_ = ++nextSelectedFutureDomain_;
    out->owner_ = this;
    out->lane_ = &newLane;
    ++selectedFutureDomains_;
    return true;
}
bool PrivateSelectedQueueFutureDomain::OwnsOriginal(
    const OriginalQueueFutureGroupRegistration& original) const noexcept {
    return owner_ != nullptr && owner_->IsOriginalQueueFutureGroupRegistrationFor(
        original, original.Identity(), marker_) && original.selectedFutureDomain_ == marker_;
}
bool PrivateSelectedQueueFutureDomain::IsOldC0Original(
    const OriginalQueueFutureGroupRegistration& original) const noexcept {
    return owner_ != nullptr && oldC0_.Valid() &&
        owner_->IsOriginalQueueFutureGroupRegistrationFor(original, oldC0_.Identity()) &&
        original.selectedFutureDomain_ == 0;
}
bool PrivateSelectedQueueFutureDomain::ReserveCalendar(ImmediateLaneDemand demand,
    ImmediateLaneHandle* out, OriginalImmediateLaneAttemptLifetimePin* original) noexcept {
    if (owner_ == nullptr || laneSlot_ != UINT32_MAX || out == nullptr ||
        original == nullptr || demand.groups != 2u || demand.members > 1u ||
        demand.buckets != 1u || !lane_->Reserve(marker_, demand, out, original)) return false;
    laneSlot_ = out->slot;
    laneGeneration_ = out->generation;
    return true;
}
bool PrivateSelectedQueueFutureDomain::ReserveGroup(std::uint64_t anchor,
    const Plan03GroupSliceInput& input, const TimelineArmTable& arms,
    Plan03GroupSliceIdentity* out, PreparedPrivateImmediateLaneGroup* original) noexcept {
    return owner_ != nullptr && original != nullptr &&
        owner_->ReserveImmediateLaneGroupInDomain(marker_, anchor, input, arms, out, original, marker_);
}
bool PrivateSelectedQueueFutureDomain::BindGroup(const Plan03GroupSliceIdentity& group,
    std::uint32_t slot, std::uint32_t generation) noexcept {
    return owner_ != nullptr && slot == laneSlot_ && generation == laneGeneration_ &&
        owner_->BindImmediateLaneGroupOwnerInDomain(
        group, *lane_, marker_, slot, generation, marker_);
}
bool PrivateSelectedQueueFutureDomain::Rebase(std::uint64_t anchor) noexcept {
    return owner_ != nullptr && owner_->RebaseImmediateLaneGroupsInDomain(marker_, anchor, marker_);
}
bool PrivateSelectedQueueFutureDomain::PublishGroups() noexcept {
    return owner_ != nullptr && owner_->PublishImmediateLaneGroupsInDomain(marker_, marker_);
}
bool PrivateSelectedQueueFutureDomain::CancelGroups(std::uint64_t completed,
    OriginalQueueFutureGroupRegistration* baselines, std::size_t capacity) noexcept {
    return owner_ != nullptr && owner_->CancelImmediateLaneGroupsInDomain(
        marker_, completed, baselines, capacity, marker_);
}
bool PrivateSelectedQueueFutureDomain::Return() noexcept {
    return owner_ == nullptr || owner_->ReturnPrivateSelectedQueueFutureDomain(*this);
}
void PrivateSelectedQueueFutureDomain::ReturnOwnStorageAfterQuiescence() noexcept {
    if (owner_ == nullptr) return;
    assert(calendar_ == nullptr && owner_->selectedFutureDomains_ != 0);
    --owner_->selectedFutureDomains_;
    oldC0_.Release();
    owner_ = nullptr;
    lane_ = nullptr;
    laneSlot_ = UINT32_MAX;
    laneGeneration_ = 0;
    marker_ = 0;
}
bool ScheduledTransactionPool::ReturnPrivateSelectedQueueFutureDomain(
    PrivateSelectedQueueFutureDomain& domain) noexcept {
    if (domain.owner_ != this || domain.marker_ == 0 || domain.calendar_ != nullptr) return false;
    ImmediateLaneObservation laneObservation{};
    if (domain.laneSlot_ != UINT32_MAX && domain.lane_->Observe(
            {domain.laneSlot_, domain.laneGeneration_}, &laneObservation)) return false;
    for (std::size_t i = 0; i < config_.maxImmediateLaneGroupRows; ++i) {
        const auto& row = immediateLaneGroups_[i];
        if (row.selectedFutureDomain == domain.marker_ &&
            row.state.load(std::memory_order_acquire) != LaneGroupState::Free) return false;
    }
    assert(selectedFutureDomains_ != 0);
    --selectedFutureDomains_;
    auto oldC0 = std::move(domain.oldC0_);
    domain.owner_ = nullptr;
    domain.lane_ = nullptr;
    domain.laneSlot_ = UINT32_MAX;
    domain.laneGeneration_ = 0;
    domain.marker_ = 0;
    oldC0.Release();
    return true;
}

bool ScheduledTransactionPool::IsOriginalQueueFutureGroupFamily(
    const OriginalQueueFutureGroupRegistration& first,
    const OriginalQueueFutureGroupRegistration& second) const noexcept {
    return MatchesOriginalQueueFutureGroupLifetime(first.lifetime_) &&
        MatchesOriginalQueueFutureGroupLifetime(second.lifetime_) &&
        first.Identity() != second.Identity() && first.cohortRoot_ != 0 &&
        first.cohortRoot_ == second.cohortRoot_ &&
        first.selectedFutureDomain_ == second.selectedFutureDomain_ &&
        first.provisionalAnchorFrame_ == second.provisionalAnchorFrame_ &&
        first.armTable_ != nullptr && first.armTable_ == second.armTable_ &&
        first.armTable_ == plan03ArmTable_ &&
        first.armTableIncarnation_ == second.armTableIncarnation_ &&
        first.armTableIncarnation_ == first.armTable_->Incarnation();
}

bool ScheduledTransactionPool::CaptureOriginalQueueGroupRelativeFrames(
    const OriginalQueueFutureGroupRegistration& first,
    const OriginalQueueFutureGroupRegistration& second,
    std::array<std::uint64_t, 2>* out) const noexcept {
    if (out == nullptr || !IsOriginalQueueFutureGroupFamily(first, second)) return false;
    // Immutable factory geometry; this observation does not issue a clock/ACK.
    const std::array<std::uint64_t, 2> relative{
        first.executeFrame_ - first.provisionalAnchorFrame_,
        second.executeFrame_ - second.provisionalAnchorFrame_};
    *out = relative;
    return true;
}

bool ScheduledTransactionPool::ProvePrivateOriginalQueueGroupClock(
    const OriginalQueueFutureGroupRegistration& first,
    const OriginalQueueFutureGroupRegistration& second,
    PrivateImmediateLanePlacementProof* actualPlacement,
    PrivateOriginalQueueGroupClockProof* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr || actualPlacement == nullptr ||
        !actualPlacement->Valid() || quantumFrames_ == 0 ||
        !MatchesOriginalQueueFutureGroupLifetime(first.lifetime_) ||
        !MatchesOriginalQueueFutureGroupLifetime(second.lifetime_) ||
        first.Identity() == second.Identity() || first.cohortRoot_ != second.cohortRoot_ ||
        first.selectedFutureDomain_ != second.selectedFutureDomain_ ||
        first.provisionalAnchorFrame_ != second.provisionalAnchorFrame_ ||
        first.armTable_ != second.armTable_ ||
        first.armTable_ != plan03ArmTable_ || first.armTable_ == nullptr ||
        first.armTableIncarnation_ != second.armTableIncarnation_ ||
        first.armTableIncarnation_ != first.armTable_->Incarnation()) return false;
    std::uint64_t anchor = 0;
    if (!actualPlacement->CaptureAnchorFrame(&anchor)) return false;
    std::array<Plan03GroupSliceIdentity, 4> rows{};
    std::array<AudioSliceLease*, 2> leases{};
    std::uint32_t rowCount = 0, leaseCount = 0;
    const auto appendRow = [&](const Plan03GroupSliceIdentity& identity) noexcept {
        for (std::uint32_t i = 0; i < rowCount; ++i)
            if (rows[i] == identity) return true;
        if (rowCount == rows.size()) return false;
        Plan03PageGroupView actual{};
        if (!ResolveImmediateLaneGroup(identity, &actual, false)) return false;
        const auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.clockProof != nullptr || row.transferProof != nullptr ||
            row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
            row.cohortRoot != first.cohortRoot_ || row.selectedFutureDomain != first.selectedFutureDomain_ ||
            row.laneOwner == nullptr ||
            !row.frameRebased || row.relativeFrame > UINT64_MAX - anchor ||
            row.rebasedFrame != anchor + row.relativeFrame ||
            row.dueQuantum != row.rebasedFrame / quantumFrames_ ||
            row.bucketOrdinal == UINT32_MAX || row.groupIndex == UINT32_MAX ||
            !row.frontierPinsOwned || actual.frontier == nullptr ||
            !actual.frontier->Valid() || actual.frontier->operation.group != identity ||
            actual.frontier->operation.order.executeFrame != row.rebasedFrame ||
            row.readers.lastAudioSequence.load(std::memory_order_acquire) != 0 ||
            !row.laneOwner->IsPrivatePlacementProofFor(*actualPlacement,
                {row.laneSlot, row.laneGeneration}, first.cohortRoot_) ||
            !row.laneOwner->ProvePrivateGroupPlacement({row.laneSlot, row.laneGeneration},
                identity, row.bucketOrdinal, row.groupIndex)) return false;
        const auto& owner = row.frontier.operation.owner;
        if (owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
            identity.decision.slot >= config_.maxImmediateLaneDecisionGates) return false;
        const auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
        const auto& decisionGate = immediateLaneDecisionGates_[identity.decision.slot];
        if (ownerGate.generation.load(std::memory_order_acquire) != owner.serviceGeneration ||
            decisionGate.generation.load(std::memory_order_acquire) != identity.decision.generation ||
            decisionGate.ownerSlot.load(std::memory_order_acquire) != owner.serviceSlot ||
            decisionGate.ownerGeneration.load(std::memory_order_acquire) != owner.serviceGeneration ||
            ((ownerGate.state.load(std::memory_order_acquire) |
              decisionGate.state.load(std::memory_order_acquire)) &
             (kLaneGateClaims | kLaneGateInvalid)) != 0) return false;
        rows[rowCount++] = identity;
        return true;
    };
    for (const auto* original : {&first, &second}) {
        const auto identity = original->Identity();
        if (!appendRow(identity)) return false;
        const auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.frontier.role == FrontierGroupRole::EntryMember) {
            if (!appendRow(row.frontier.primary)) return false;
            const auto& primary = immediateLaneGroups_[row.frontier.primary.committedRow];
            if (primary.frontier.role != FrontierGroupRole::Coordinator ||
                primary.frontier.primary != row.frontier.primary ||
                primary.frontier.entry != row.frontier.entry) return false;
        } else if (row.frontier.role != FrontierGroupRole::Coordinator ||
            row.frontier.primary != identity) return false;
        auto* const lease = EffectiveImmediateLaneGroupLease(identity.committedRow);
        if (lease == nullptr) return false;
        bool seen = false;
        for (std::uint32_t i = 0; i < leaseCount; ++i) seen = seen || leases[i] == lease;
        if (!seen) leases[leaseCount++] = lease;
    }
    std::array<OriginalQueueFutureGroupLifetimePin, 2> originals{};
    if (!first.BorrowLifetime(&originals[0]) || !second.BorrowLifetime(&originals[1])) return false;
    std::uint32_t held = 0;
    for (; held < leaseCount; ++held) {
        if (leases[held]->HoldPrivateTransfer()) continue;
        for (std::uint32_t i = 0; i < held; ++i) leases[i]->ReleasePrivateTransfer();
        return false;
    }
    // Every fallible issuer/hold is complete before taking the actual placement.
    out->owner_ = this;
    out->placement_ = std::move(*actualPlacement);
    out->originals_ = std::move(originals);
    out->rows_ = rows;
    out->leases_ = leases;
    out->rowCount_ = rowCount;
    out->leaseCount_ = leaseCount;
    out->root_ = first.cohortRoot_;
    out->anchor_ = anchor;
    for (std::uint32_t i = 0; i < rowCount; ++i)
        immediateLaneGroups_[rows[i].committedRow].clockProof = out;
    return true;
}

bool OriginalQueueFutureGroupLifetimePin::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesOriginalQueueFutureGroupLifetime(*this);
}

bool OriginalQueueFutureGroupLifetimePin::Borrow(
    OriginalQueueFutureGroupLifetimePin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalQueueFutureGroupLifetime(*this, out);
}

void OriginalQueueFutureGroupLifetimePin::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseOriginalQueueFutureGroupLifetime(*this);
    owner_ = nullptr;
    identity_ = {};
    marker_ = 0;
}

bool OriginalQueueFutureGroupRegistration::MatchesOriginalScope(
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept {
    return lifetime_.owner_ != nullptr &&
        lifetime_.owner_->MatchesOriginalQueueFutureGroupScope(*this,
            cohortRoot, provisionalAnchorFrame, input, armTable);
}

bool OriginalQueueFutureGroupRegistration::CaptureOriginalScope(
    const ScheduledTransactionPool& pool, const TimelineArmTable& armTable,
    OriginalQueueFutureGroupScope* out) const noexcept {
    return pool.CaptureOriginalQueueFutureGroupScope(*this, armTable, out);
}

bool ScheduledTransactionPool::CaptureOriginalQueueFutureGroupScope(
    const OriginalQueueFutureGroupRegistration& original, const TimelineArmTable& armTable,
    OriginalQueueFutureGroupScope* out) const noexcept {
    if (out == nullptr || !MatchesOriginalQueueFutureGroupLifetime(original.lifetime_) ||
        original.armTable_ != &armTable || plan03ArmTable_ != &armTable ||
        original.armTableIncarnation_ == 0 || armTable.Incarnation() != original.armTableIncarnation_)
        return false;
    OriginalQueueFutureGroupScope captured{};
    captured.cohortRoot = original.cohortRoot_;
    captured.provisionalAnchorFrame = original.provisionalAnchorFrame_;
    captured.selectedFutureDomain = original.selectedFutureDomain_;
    captured.input.executeFrame = original.executeFrame_;
    captured.input.decision = original.Identity().decision;
    captured.input.sliceIndex = original.Identity().sliceIndex;
    captured.input.memberCount = original.memberCount_;
    captured.input.sourceControlCount = original.sourceControlCount_;
    captured.input.immediateEntry = original.immediateEntry_;
    captured.input.members = immediateLaneGroupMembers_.get() +
        original.Identity().committedRow * config_.maxArmsPerGroup;
    if (!MatchesOriginalQueueFutureGroupScope(original, captured.cohortRoot,
            captured.provisionalAnchorFrame, captured.input, armTable)) return false;
    *out = captured;
    return true;
}

bool ScheduledTransactionPool::MatchesOriginalQueueFutureGroupLifetime(
    const OriginalQueueFutureGroupLifetimePin& original) const noexcept {
    const auto& identity = original.identity_;
    if (original.owner_ != this || original.marker_ == 0 || !identity.Exact() ||
        identity.backing != Plan03GroupBacking::ImmediateLane ||
        identity.transportEpoch != epoch_ || identity.absoluteQuantum != 0 ||
        identity.pageGeneration != 1 || immediateLaneGroups_ == nullptr ||
        identity.committedRow >= config_.maxImmediateLaneGroupRows) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    const auto state = row.state.load(std::memory_order_acquire);
    return (state == LaneGroupState::Private || state == LaneGroupState::Published) &&
        row.generation == identity.rowClaimGeneration &&
        row.decision.slot == identity.decision.slot &&
        row.decision.generation == identity.decision.generation &&
        row.sliceIndex == identity.sliceIndex &&
        row.originalFactoryMarker == original.marker_ &&
        row.originalRegistrationPinCount != 0;
}

bool ScheduledTransactionPool::ResolveRetainedHiddenConditionalGroup(
    const OriginalQueueFutureGroupLifetimePin& primary,
    const OriginalQueueFutureGroupLifetimePin& member,
    Plan03PageGroupView* out) const noexcept {
    if (out == nullptr) return false;
    const auto retained = [this](const OriginalQueueFutureGroupLifetimePin& original) noexcept {
        const auto& identity = original.identity_;
        if (original.owner_ != this || original.marker_ == 0 || !identity.Exact() ||
            identity.backing != Plan03GroupBacking::ImmediateLane ||
            identity.transportEpoch != epoch_ || identity.absoluteQuantum != 0 ||
            identity.pageGeneration != 1 || immediateLaneGroups_ == nullptr ||
            identity.committedRow >= config_.maxImmediateLaneGroupRows) return false;
        const auto& row = immediateLaneGroups_[identity.committedRow];
        // The supplied native loan keeps marker and immutable identity alive.
        // Its mutable producer-only registration count is not sampled by audio.
        return row.state.load(std::memory_order_acquire) == LaneGroupState::Published &&
            row.generation == identity.rowClaimGeneration &&
            row.decision.slot == identity.decision.slot &&
            row.decision.generation == identity.decision.generation &&
            row.sliceIndex == identity.sliceIndex && row.originalFactoryMarker == original.marker_;
    };
    if (!retained(primary) || !retained(member) ||
        primary.identity_.committedRow == member.identity_.committedRow) return false;
    const auto& first = immediateLaneGroups_[primary.identity_.committedRow];
    const auto& second = immediateLaneGroups_[member.identity_.committedRow];
    if (first.cohortRoot == 0 || first.cohortRoot != second.cohortRoot ||
        first.selectedFutureDomain != second.selectedFutureDomain ||
        first.laneOwner == nullptr || first.laneOwner != second.laneOwner ||
        first.laneSlot != second.laneSlot || first.laneGeneration != second.laneGeneration ||
        !first.frameRebased || !second.frameRebased || first.rebasedFrame != second.rebasedFrame ||
        first.dueQuantum != second.dueQuantum || first.bucketOrdinal != second.bucketOrdinal ||
        !first.frontier.Valid() || !second.frontier.Valid() ||
        first.frontier.role != FrontierGroupRole::Coordinator ||
        second.frontier.role != FrontierGroupRole::EntryMember ||
        !(first.frontier.operation.group == primary.identity_) ||
        !(second.frontier.operation.group == member.identity_) ||
        !(first.frontier.primary == primary.identity_) ||
        !(second.frontier.primary == primary.identity_) ||
        !(first.frontier.entry == second.frontier.entry) ||
        !(first.frontier.operation.owner == second.frontier.operation.owner) ||
        !second.frontier.queueConditional.Valid()) return false;
    Plan03PageGroupView actual{};
    if (!ResolveImmediateLaneGroup(member.identity_, &actual, false) ||
        actual.frontier != &second.frontier || actual.lease != &first.lease) return false;
    *out = actual;
    return true;
}

bool ScheduledTransactionPool::IsOriginalQueueFutureGroupRegistrationFor(
    const OriginalQueueFutureGroupRegistration& original,
    const Plan03GroupSliceIdentity& expected) const noexcept {
    return MatchesOriginalQueueFutureGroupLifetime(original.lifetime_) &&
        original.lifetime_.identity_ == expected;
}

bool ScheduledTransactionPool::IsOriginalQueueFutureGroupRegistrationFor(
    const OriginalQueueFutureGroupRegistration& original,
    const Plan03GroupSliceIdentity& expected, std::uint64_t originalCohortRoot) const noexcept {
    return originalCohortRoot != 0 && original.cohortRoot_ == originalCohortRoot &&
        IsOriginalQueueFutureGroupRegistrationFor(original, expected) &&
        immediateLaneGroups_[expected.committedRow].selectedFutureDomain == original.selectedFutureDomain_;
}

bool ScheduledTransactionPool::BorrowOriginalQueueFutureGroupLifetime(
    const OriginalQueueFutureGroupLifetimePin& original,
    OriginalQueueFutureGroupLifetimePin* out) noexcept {
    if (!MatchesOriginalQueueFutureGroupLifetime(original) ||
        out == nullptr || out->owner_ != nullptr) return false;
    auto& row = immediateLaneGroups_[original.identity_.committedRow];
    if (row.originalRegistrationPinCount == UINT32_MAX) return false;
    ++row.originalRegistrationPinCount;
    out->owner_ = this;
    out->identity_ = original.identity_;
    out->marker_ = original.marker_;
    return true;
}

void ScheduledTransactionPool::ReleaseOriginalQueueFutureGroupLifetime(
    OriginalQueueFutureGroupLifetimePin& original) noexcept {
    const bool matches = MatchesOriginalQueueFutureGroupLifetime(original);
    assert(matches);
    if (!matches) return;
    auto& row = immediateLaneGroups_[original.identity_.committedRow];
    if (--row.originalRegistrationPinCount == 0) {
        assert(row.transferOriginal == nullptr && row.transferProof == nullptr);
        row.originalFactoryMarker = 0;
    }
}

bool ScheduledTransactionPool::ConsumePrivateImmediateLaneGroupTransfer(
    PrivateImmediateLaneGroupTransferProof&& proof,
    OriginalQueueFutureGroupRegistration* out) noexcept {
    if (out == nullptr || out->lifetime_.owner_ != nullptr ||
        proof.owner_ != this || !proof.Valid()) return false;
    auto& row = *static_cast<LaneGroupRow*>(proof.row_);
    auto& original = *proof.original_;
    if (original.originalMarker_ == 0 ||
        row.originalFactoryMarker != original.originalMarker_ ||
        row.originalRegistrationPinCount != 0) return false;

    out->cohortRoot_ = original.cohortRoot_;
    out->selectedFutureDomain_ = original.selectedFutureDomain_;
    out->provisionalAnchorFrame_ = original.provisionalAnchorFrame_;
    out->executeFrame_ = original.executeFrame_;
    out->memberCount_ = original.memberCount_;
    out->sourceControlCount_ = original.sourceControlCount_;
    out->immediateEntry_ = original.immediateEntry_;
    out->armTable_ = original.armTable_;
    out->armTableIncarnation_ = original.armTableIncarnation_;

    // Establish the actual replacement storage owner before dropping exclusion.
    row.originalRegistrationPinCount = 1;
    out->lifetime_.owner_ = this;
    out->lifetime_.identity_ = proof.identity_;
    out->lifetime_.marker_ = original.originalMarker_;

    row.transferOriginal = nullptr;
    original.owner_ = nullptr;
    original.identity_ = {};
    original.originalMarker_ = 0;
    proof.Release();
    return true;
}

bool ScheduledTransactionPool::MatchesOriginalQueueFutureGroupScope(
    const OriginalQueueFutureGroupRegistration& original,
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept {
    return MatchesOriginalQueueFutureGroupScopeImpl(original, cohortRoot,
        provisionalAnchorFrame, input, armTable, false);
}

bool ScheduledTransactionPool::MatchesOriginalQueueFutureGroupScopeImpl(
    const OriginalQueueFutureGroupRegistration& original,
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
    bool retiredSounds) const noexcept {
    if (!MatchesOriginalQueueFutureGroupLifetime(original.lifetime_) ||
        original.cohortRoot_ != cohortRoot ||
        original.provisionalAnchorFrame_ != provisionalAnchorFrame ||
        original.executeFrame_ != input.executeFrame || original.armTable_ != &armTable ||
        plan03ArmTable_ != &armTable || original.armTableIncarnation_ == 0 ||
        armTable.Incarnation() != original.armTableIncarnation_ ||
        original.lifetime_.identity_.decision.slot != input.decision.slot ||
        original.lifetime_.identity_.decision.generation != input.decision.generation ||
        original.lifetime_.identity_.sliceIndex != input.sliceIndex ||
        original.memberCount_ != input.memberCount ||
        original.sourceControlCount_ != input.sourceControlCount ||
        original.immediateEntry_.gate != input.immediateEntry.gate ||
        original.immediateEntry_.generation != input.immediateEntry.generation ||
        (input.memberCount != 0 && input.members == nullptr)) return false;

    const auto rowIndex = original.lifetime_.identity_.committedRow;
    const auto& row = immediateLaneGroups_[rowIndex];
    if (row.cohortRoot != original.cohortRoot_ ||
        row.selectedFutureDomain != original.selectedFutureDomain_ ||
        row.memberCount != original.memberCount_ ||
        row.sourceControlCount != original.sourceControlCount_) return false;
    const auto* actual = immediateLaneGroupMembers_.get() + rowIndex * config_.maxArmsPerGroup;
    const auto* originalIndices = immediateLaneGroupMemberIndices_.get() +
        rowIndex * config_.maxArmsPerGroup;
    for (std::uint16_t index = 0; index < input.memberCount; ++index) {
        TimelineArmObservation expected{};
        if (!armTable.Observe(input.members[index], &expected) ||
            expected.parentGroup.decision.slot != original.lifetime_.identity_.decision.slot ||
            expected.parentGroup.decision.generation != original.lifetime_.identity_.decision.generation)
            return false;
        if (retiredSounds && index >= input.sourceControlCount) {
            const auto* arm = armTable.RowAt(input.members[index].slot);
            if (arm == nullptr || expected.state != ArmState::Tombstoned ||
                expected.startedBlockSequence != 0 || expected.tombstoneAudioSequence == 0 ||
                arm->renderSlot != input.members[index].renderSlot ||
                arm->renderGeneration != input.members[index].renderGeneration) return false;
        } else if (armTable.Resolve(input.members[index]) == nullptr) return false;
        std::uint16_t position = input.memberCount;
        for (std::uint16_t candidate = 0; candidate < input.memberCount; ++candidate) {
            if (actual[candidate].slot == input.members[index].slot &&
                actual[candidate].generation == input.members[index].generation &&
                actual[candidate].renderSlot == input.members[index].renderSlot &&
                actual[candidate].renderGeneration == input.members[index].renderGeneration) {
                position = candidate;
                break;
            }
        }
        if (position == input.memberCount || expected.memberIndex != originalIndices[position])
            return false;
        for (std::uint16_t prior = 0; prior < index; ++prior)
            if (input.members[prior].slot == input.members[index].slot &&
                input.members[prior].generation == input.members[index].generation) return false;
    }
    return true;
}

std::uint32_t ScheduledTransactionPool::RingIndex(std::uint32_t page) const {
    return pageCount_ == 0 ? 0 : (ringStart_ + page) % pageCount_;
}

bool ScheduledTransactionPool::Reset(const Config& config,
                                     std::uint64_t startFrame,
                                     TransportEpoch epoch) {
    if (laneGroupPublications_ != nullptr) return false;
    if (HasHeldCurrentSlices() || currentSliceIncarnation_ == UINT64_MAX) return false;
    if (selectedFutureDomains_ != 0 || preparedPageFrontierBindingHolds_ != 0 ||
        privateUnboundPageArmRetirements_ != nullptr || unpublishedGroupArmCancellations_ != nullptr || privateMaterializationActive_) return false;
    if (immediateLaneGroups_ != nullptr)
        for (std::size_t row = 0; row < config_.maxImmediateLaneGroupRows; ++row) {
            const auto state = immediateLaneGroups_[row].state.load(std::memory_order_acquire);
            if (immediateLaneGroups_[row].clockProof != nullptr ||
                immediateLaneGroups_[row].originalRegistrationPinCount != 0 ||
                (state != LaneGroupState::Free && state != LaneGroupState::Retired))
                return false;
        }
    if (HasPlan03GroupReaders() || preparedTransactionClaimCount_ != 0 ||
        std::any_of(pagePublicationHolds_.begin(), pagePublicationHolds_.end(),
                    [](auto count) { return count != 0; }) ||
        std::any_of(pagePreparedPlan03GroupCount_.begin(), pagePreparedPlan03GroupCount_.end(),
                    [](auto count) { return count != 0; })) return false;
    for (std::size_t row = 0; row < pagePlan03Groups_.size(); ++row)
        DetachPreparedPageGroupTransfer(row);
    for (std::size_t slot = 0; slot < preparedTransactionClaims_.size(); ++slot)
        DetachPreparedPageBatchTransfer(slot);
    ReturnAllGroupMemberOriginals();
    config_ = {};
    pageCount_ = 0;
    quantumFrames_ = 0;
    epoch_ = kInvalidTransportEpoch;
    terminalState_.store(1, std::memory_order_relaxed);
    plan03ArmTable_ = nullptr;
    plan03ArmTableEstablishedByPrepared_ = false;

    if (epoch == kInvalidTransportEpoch) {
        return false;
    }
    if (config.quantumFrames == 0 || config.scheduleHorizonFrames == 0) {
        return false;
    }
    if ((config.maxPlan03GroupHeadersPerPage == 0) !=
        (config.maxArmsPerGroup == 0)) {
        return false;
    }
    if (config.maxImmediateLaneGroupRows != 0 &&
        (config.maxArmsPerGroup == 0 ||
         config.maxImmediateLaneOwnerGates == 0 ||
         config.maxImmediateLaneDecisionGates == 0)) return false;
    if (config.maxPlan03GroupHeadersPerPage >
            kMaxPlan03GroupHeadersPerPageHard ||
        config.maxArmsPerGroup > kMaxPlan03ArmsPerGroupHard) {
        return false;
    }
    if (config.scheduleHorizonFrames % config.quantumFrames != 0) {
        return false;
    }
    if (startFrame % config.quantumFrames != 0) {
        return false;
    }
    if (config.maxScheduledTransactions == 0 ||
        config.maxTransactionSpanFrames == 0 ||
        config.maxIngressTransactionsPerQuantum == 0 ||
        config.maxFrameBatchesPerQuantum == 0 ||
        config.maxEventsPerQuantum == 0 || config.maxTransactionEvents == 0 ||
        config.maxCurrentSlicesPerQuantum > config.maxEventsPerQuantum) {
        return false;
    }
    if (config.maxScheduledTransactions > SIZE_MAX / sizeof(PreparedTransactionClaim)) return false;
    const std::uint64_t pages =
        config.scheduleHorizonFrames / config.quantumFrames;
    const std::uint64_t currentSliceCapacity =
        static_cast<std::uint64_t>(config.maxCurrentSlicesPerQuantum == 0
            ? config.maxEventsPerQuantum : config.maxCurrentSlicesPerQuantum) +
        config.maxNativeStartCalendarsPerQuantum;
    if (currentSliceCapacity > UINT32_MAX ||
        currentSliceCapacity > SIZE_MAX / sizeof(CurrentSliceRow) ||
        pages > SIZE_MAX / sizeof(CurrentSliceRow) / currentSliceCapacity) return false;
    if (pages == 0 || pages > 0xFFFFFFFFull) {
        return false;
    }
    if (config.maxPlan03GroupHeadersPerPage != 0 &&
        pages > (SIZE_MAX - config.maxScheduledTransactions) /
            config.maxPlan03GroupHeadersPerPage) return false;
    // A page can never hold more distinct-frame batches than events, so a batch
    // ceiling above the event ceiling is a config mistake rather than a wider
    // bound.
    if (config.maxFrameBatchesPerQuantum > config.maxEventsPerQuantum) {
        return false;
    }
    // The window's far edge must fit in uint64: startFrame + horizon must not
    // wrap, or WindowEndFrame and the page arithmetic would fold into the past.
    if (startFrame > UINT64_MAX - config.scheduleHorizonFrames) {
        return false;
    }
    // The publish lead must leave at least one writable page. A lead of pageCount
    // or more would place the default publish cutoff at or past the far edge,
    // freezing the whole window and leaving nowhere to reserve. A lead of zero is
    // legal: it means no default look-ahead publishing, so the frontier stays at
    // the window start and freezes nothing, which keeps the cutoff invariant
    // (within [windowStart, windowEnd) and monotonic) intact.
    if (config.publishLeadQuanta >= pages) {
        return false;
    }

    config_ = config;
    pageCount_ = static_cast<std::uint32_t>(pages);
    quantumFrames_ = config.quantumFrames;
    epoch_ = epoch;
    baseFrame_ = startFrame;
    baseQuantumIndex_ = startFrame / config.quantumFrames;
    sessionBaseQuantumIndex_ = baseQuantumIndex_;
    consumedFrontierQuantum_ =
        std::make_unique<std::atomic<std::uint64_t>>(sessionBaseQuantumIndex_);
    ringStart_ = 0;
    publishCutoffFrame_ = startFrame; // nothing within the window is frozen yet
    nextRowSequence_ = 1;
    nextCurrentSliceSequence_ = 1;
    consumeTick_ = 0;

    pageHeaders_.assign(pageCount_, HorizonPageHeader{});
    pageEventCount_.assign(pageCount_, 0);
    pagePreparedEventCount_.assign(pageCount_, 0);
    pagePublicationHolds_.assign(pageCount_, 0);
    pageSorted_.assign(pageCount_, 1);
    pageQuantumStamp_ =
        std::make_unique<std::atomic<std::uint64_t>[]>(pageCount_);
    pageEmptyQuantumStamp_ =
        std::make_unique<std::atomic<std::uint64_t>[]>(pageCount_);
    for (std::uint32_t p = 0; p < pageCount_; ++p) {
        pageQuantumStamp_[p].store(kUnpublishedQuantumStamp,
                                   std::memory_order_relaxed);
        pageEmptyQuantumStamp_[p].store(kUnpublishedQuantumStamp,
                                        std::memory_order_relaxed);
    }
    pageEvents_.assign(static_cast<std::size_t>(pageCount_) *
                           config.maxEventsPerQuantum,
                       EventRow{});
    pageEventOrder_.assign(static_cast<std::size_t>(pageCount_) *
                               config.maxEventsPerQuantum,
                           0);
    if (config_.maxCurrentSlicesPerQuantum == 0) {
        config_.maxCurrentSlicesPerQuantum = config_.maxEventsPerQuantum;
    }
    currentSliceStorageCapacity_ = static_cast<std::uint32_t>(currentSliceCapacity);
    ++currentSliceIncarnation_;
    pageCurrentSliceCount_.assign(pageCount_, 0);
    pageCurrentSlices_.assign(
        static_cast<std::size_t>(pageCount_) *
            currentSliceStorageCapacity_,
        CurrentSliceRow{});
    pageCurrentSliceOrder_.assign(
        static_cast<std::size_t>(pageCount_) *
            currentSliceStorageCapacity_,
        0);
    const std::size_t groupRows =
        static_cast<std::size_t>(pageCount_) *
        config_.maxPlan03GroupHeadersPerPage;
    if (config_.maxPlan03GroupHeadersPerPage != 0 &&
        groupRows / config_.maxPlan03GroupHeadersPerPage != pageCount_) {
        return false;
    }
    if (groupRows > SIZE_MAX / sizeof(Plan03GroupSliceRow)) return false;
    if (config_.maxArmsPerGroup != 0 &&
        groupRows > std::numeric_limits<std::size_t>::max() /
                        config_.maxArmsPerGroup) {
        return false;
    }
    pagePlan03GroupCount_.assign(pageCount_, 0);
    pagePreparedPlan03GroupCount_.assign(pageCount_, 0);
    pagePlan03Groups_.assign(groupRows, Plan03GroupSliceRow{});
    pagePlan03GroupMembers_.assign(
        groupRows * config_.maxArmsPerGroup, TimelineArmToken{});
    pageGroupIdentities_ = groupRows == 0 ? nullptr :
        std::make_unique<TimelineArmTable::OriginalGroupIdentity[]>(groupRows);
    pageGroupMemberOriginals_ = pagePlan03GroupMembers_.empty() ? nullptr :
        std::make_unique<TimelineArmTable::OriginalGroupMember[]>(pagePlan03GroupMembers_.size());
    replacementGroupMemberOriginals_ = config_.maxArmsPerGroup == 0 ? nullptr :
        std::make_unique<TimelineArmTable::OriginalGroupMember[]>(config_.maxArmsPerGroup);
    pagePlan03GroupRowStates_.assign(
        groupRows, Plan03GroupRowState::Free);
    pagePlan03GroupClaimGenerations_.assign(groupRows, 1);
    pagePlan03GroupReaders_ = groupRows == 0 ? nullptr :
        std::make_unique<Plan03GroupReaderState[]>(groupRows);
    pagePlan03GroupLeases_ =
        groupRows == 0 ? nullptr
                       : std::make_unique<AudioSliceLease[]>(groupRows);
    const auto laneRows = static_cast<std::size_t>(config_.maxImmediateLaneGroupRows);
    if (config_.maxArmsPerGroup != 0 &&
        laneRows > SIZE_MAX / config_.maxArmsPerGroup) return false;
    immediateGroupIdentities_ = laneRows == 0 ? nullptr :
        std::make_unique<TimelineArmTable::OriginalGroupIdentity[]>(laneRows);
    immediateLaneGroups_ = laneRows == 0 ? nullptr :
        std::make_unique<LaneGroupRow[]>(laneRows);
    immediateLaneGroupMembers_ = laneRows == 0 ? nullptr :
        std::make_unique<TimelineArmToken[]>(laneRows * config_.maxArmsPerGroup);
    immediateGroupMemberOriginals_ = laneRows == 0 ? nullptr :
        std::make_unique<TimelineArmTable::OriginalGroupMember[]>(laneRows * config_.maxArmsPerGroup);
    immediateLaneGroupMemberIndices_ = laneRows == 0 ? nullptr :
        std::make_unique<std::uint16_t[]>(laneRows * config_.maxArmsPerGroup);
    immediateLaneOwnerGates_ = config_.maxImmediateLaneOwnerGates == 0 ? nullptr :
        std::make_unique<LaneFrontierGate[]>(config_.maxImmediateLaneOwnerGates);
    immediateLaneDecisionGates_ = config_.maxImmediateLaneDecisionGates == 0 ? nullptr :
        std::make_unique<LaneFrontierGate[]>(config_.maxImmediateLaneDecisionGates);
    pageBatchCount_.assign(pageCount_, 0);
    pageBatchOccupiedCount_.assign(pageCount_, 0);
    pageBatchRows_.assign(static_cast<std::size_t>(pageCount_) *
                              config.maxFrameBatchesPerQuantum,
                          FrameBatchRow{});
    for (std::uint32_t p = 0; p < pageCount_; ++p) {
        pageHeaders_[p].transportEpoch = epoch;
        pageHeaders_[p].absoluteQuantumIndex = baseQuantumIndex_ + p;
        pageHeaders_[p].pageGeneration = 1;
        pageHeaders_[p].plan03GroupCount = 0;
    }

    transactions_.assign(config.maxScheduledTransactions, TransactionRecord{});
    freeTransactions_.clear();
    freeTransactions_.reserve(config.maxScheduledTransactions);
    for (std::uint32_t i = config.maxScheduledTransactions; i > 0; --i) {
        freeTransactions_.push_back(i - 1);
    }
    preparedTransactionClaims_.assign(
        config.maxScheduledTransactions, PreparedTransactionClaim{});
    freePreparedTransactionClaims_.clear();
    freePreparedTransactionClaims_.reserve(config.maxScheduledTransactions);
    for (std::uint32_t i = config.maxScheduledTransactions; i > 0; --i) {
        freePreparedTransactionClaims_.push_back(i - 1);
    }
    unplacedCredits_.assign(
        static_cast<std::size_t>(config.maxScheduledTransactions) + groupRows,
        UnplacedCreditRow{});
    privatePreparedGroups_.clear();
    privatePreparedGroups_.resize(groupRows);
    privateCommittedGroups_.clear();
    privateCommittedGroups_.resize(groupRows);
    privateCohortCredits_.assign(unplacedCredits_.size(), nullptr);
    privateCohortRequests_.resize(config.maxScheduledTransactions);
    privateCohortGroups_.resize(groupRows);
    unplacedTransactions_ = 0;
    unplacedPreparedClaims_ = 0;
    unplacedEvents_ = 0;
    unplacedFrameBatches_ = 0;
    unplacedGroupHeaders_ = 0;
    unplacedIngressThisQuantum_ = 0;
    privateMaterializationFaultStepForTest_ = 0;

    scratchEventPage_.assign(config.maxTransactionEvents, 0);
    scratchEventFrame_.assign(config.maxTransactionEvents, 0);
    refusalCounts_.assign(
        static_cast<std::size_t>(ReserveRefusal::CommitSerialExhausted) + 1, 0);

    liveTransactionCount_ = 0;
    transactionHighWater_ = 0;
    frameBatchHighWater_ = 0;
    eventHighWater_ = 0;
    transactionEventsHighWater_ = 0;
    transactionSpanHighWater_ = 0;
    ingressThisQuantum_ = 0;
    preparedIngressThisQuantum_ = 0;
    preparedTransactionClaimCount_ = 0;
    ingressHighWater_ = 0;
    droppedLateCount_ = 0;
    advanceBlockedByConsumer_ = 0;
    consumerFrontierEnabled_ = false;

    terminalState_.store(0, std::memory_order_release);
    return true;
}

bool ScheduledTransactionPool::ReserveUnplacedBatchCredit(
    const UnplacedBatchCreditRequest& request,
    UnplacedBatchCredit* out) noexcept {
    if (!Valid() || out == nullptr || out->Valid() ||
        (request.transactions == 0 && request.groupHeaders == 0) ||
        request.preparedClaims > request.transactions ||
        (request.transactions == 0 &&
            (request.preparedClaims != 0 || request.events != 0 ||
             request.frameBatches != 0)) ||
        (request.transactions != 0 &&
            (request.events < request.transactions ||
             request.preparedClaims == 0)) ||
        request.frameBatches > request.events ||
        nextUnplacedCreditGeneration_ == UINT64_MAX) return false;
    if (unplacedTransactions_ + request.transactions > freeTransactions_.size() ||
        unplacedPreparedClaims_ + request.preparedClaims >
            freePreparedTransactionClaims_.size()) return false;
    const auto ingress = static_cast<std::uint64_t>(ingressThisQuantum_) +
        preparedIngressThisQuantum_ + unplacedIngressThisQuantum_ +
        request.transactions;
    if (ingress > config_.maxIngressTransactionsPerQuantum) return false;
    std::uint64_t occupiedEvents = 0;
    std::uint64_t occupiedBatches = 0;
    for (std::uint32_t page = 0; page < pageCount_; ++page) {
        occupiedEvents += pageEventCount_[page] + pagePreparedEventCount_[page];
        occupiedBatches += pageBatchOccupiedCount_[page];
    }
    std::uint64_t occupiedGroups = 0;
    for (const auto state : pagePlan03GroupRowStates_)
        occupiedGroups += state != Plan03GroupRowState::Free ? 1u : 0u;
    if (occupiedEvents + unplacedEvents_ + request.events > pageEvents_.size() ||
        occupiedBatches + unplacedFrameBatches_ + request.frameBatches >
            pageBatchRows_.size() ||
        occupiedGroups + unplacedGroupHeaders_ + request.groupHeaders >
            pagePlan03GroupRowStates_.size()) return false;
    for (std::size_t slot = 0; slot < unplacedCredits_.size(); ++slot) {
        auto& row = unplacedCredits_[slot];
        if (row.active) continue;
        row.request = request;
        row.generation = nextUnplacedCreditGeneration_++;
        row.epoch = epoch_;
        row.chargedQuantum = baseQuantumIndex_;
        row.active = true;
        unplacedTransactions_ += request.transactions;
        unplacedPreparedClaims_ += request.preparedClaims;
        unplacedEvents_ += request.events;
        unplacedFrameBatches_ += request.frameBatches;
        unplacedGroupHeaders_ += request.groupHeaders;
        unplacedIngressThisQuantum_ += request.transactions;
        *out = {slot, row.generation, epoch_};
        return true;
    }
    return false;
}

bool ScheduledTransactionPool::CancelUnplacedBatchCredit(
    UnplacedBatchCredit* credit) noexcept {
    if (credit == nullptr || !credit->Valid() || credit->epoch != epoch_ ||
        credit->slot >= unplacedCredits_.size()) return false;
    auto& row = unplacedCredits_[credit->slot];
    if (!row.active || row.materializing ||
        row.generation != credit->generation ||
        row.epoch != credit->epoch) return false;
    const auto& request = row.request;
    if (unplacedTransactions_ < request.transactions ||
        unplacedPreparedClaims_ < request.preparedClaims ||
        unplacedEvents_ < request.events ||
        unplacedFrameBatches_ < request.frameBatches ||
        unplacedGroupHeaders_ < request.groupHeaders ||
        (row.chargedQuantum == baseQuantumIndex_ &&
         unplacedIngressThisQuantum_ < request.transactions)) return false;
    unplacedTransactions_ -= request.transactions;
    unplacedPreparedClaims_ -= request.preparedClaims;
    unplacedEvents_ -= request.events;
    unplacedFrameBatches_ -= request.frameBatches;
    unplacedGroupHeaders_ -= request.groupHeaders;
    if (row.chargedQuantum == baseQuantumIndex_)
        unplacedIngressThisQuantum_ -= request.transactions;
    row = {};
    *credit = {};
    return true;
}

bool ScheduledTransactionPool::PreflightMaterializationGroups(
    const Plan03GroupSliceInput* groups, std::size_t groupCount,
    const TimelineArmTable& armTable,
    std::uint64_t publicationFrontier,
    Plan03GroupReserveStatus* groupStatus) const noexcept {
    if (groupStatus == nullptr || (groupCount != 0 && groups == nullptr))
        return false;
    if (groupCount != 0 && plan03ArmTable_ != nullptr &&
        plan03ArmTable_ != &armTable) {
        *groupStatus = Plan03GroupReserveStatus::Invalid;
        return false;
    }
    bool groupsAdmitted = true;
    for (std::size_t index = 0; index < groupCount; ++index) {
        const auto& group = groups[index];
        if (!group.decision.Valid() ||
            (group.memberCount != 0 && group.members == nullptr) ||
            group.sourceControlCount > group.memberCount) {
            *groupStatus = Plan03GroupReserveStatus::Invalid;
            groupsAdmitted = false;
            break;
        }
        if (group.memberCount > config_.maxArmsPerGroup) {
            *groupStatus = Plan03GroupReserveStatus::MemberCapacity;
            groupsAdmitted = false;
            break;
        }
        for (std::uint16_t member = 0; member < group.memberCount;
             ++member) {
            TimelineArmObservation observation{};
            if (!armTable.Observe(group.members[member], &observation) ||
                observation.parentGroup.decision.slot !=
                    group.decision.slot ||
                observation.parentGroup.decision.generation !=
                    group.decision.generation) {
                *groupStatus = Plan03GroupReserveStatus::ArmIdentityMismatch;
                groupsAdmitted = false;
                break;
            }
            for (std::uint16_t prior = 0; prior < member; ++prior) {
                TimelineArmObservation earlier{};
                if (!armTable.Observe(group.members[prior], &earlier) ||
                    earlier.memberIndex == observation.memberIndex ||
                    (group.members[prior].slot == group.members[member].slot &&
                     group.members[prior].generation ==
                         group.members[member].generation)) {
                    *groupStatus = Plan03GroupReserveStatus::DuplicateMember;
                    groupsAdmitted = false;
                    break;
                }
            }
            if (!groupsAdmitted) break;
        }
        if (!groupsAdmitted) break;
        std::uint32_t page = 0;
        if (!PageForFrame(group.executeFrame, &page)) {
            *groupStatus = Plan03GroupReserveStatus::OutsideWindow;
            groupsAdmitted = false;
            break;
        }
        if (group.executeFrame < std::max(
                publicationFrontier, publishCutoffFrame_)) {
            *groupStatus = Plan03GroupReserveStatus::PageFrozen;
            groupsAdmitted = false;
            break;
        }
        bool priorPage = false;
        std::uint32_t samePageCount = 1;
        for (std::size_t other = 0; other < groupCount; ++other) {
            if (other == index) continue;
            std::uint32_t otherPage = 0;
            if (PageForFrame(groups[other].executeFrame, &otherPage) &&
                otherPage == page) {
                priorPage = priorPage || other < index;
                ++samePageCount;
                if (groups[other].decision.slot == group.decision.slot &&
                    groups[other].decision.generation ==
                        group.decision.generation &&
                    groups[other].sliceIndex == group.sliceIndex) {
                    *groupStatus = Plan03GroupReserveStatus::DuplicateSlice;
                    groupsAdmitted = false;
                    break;
                }
            }
        }
        if (!groupsAdmitted) break;
        const auto ring = RingIndex(page);
        const auto rowBase = static_cast<std::size_t>(ring) *
            config_.maxPlan03GroupHeadersPerPage;
        std::uint32_t usable = 0;
        for (std::uint32_t row = 0;
             row < config_.maxPlan03GroupHeadersPerPage; ++row) {
            const auto slot = rowBase + row;
            if (pagePlan03GroupRowStates_[slot] ==
                    Plan03GroupRowState::Free &&
                pagePlan03GroupReaders_[slot].pins.load(
                    std::memory_order_acquire) == 0) ++usable;
            const auto& existing = pagePlan03Groups_[slot];
            if (pagePlan03GroupRowStates_[slot] !=
                    Plan03GroupRowState::Free &&
                pagePlan03GroupRowStates_[slot] !=
                    Plan03GroupRowState::Retired &&
                existing.decision.slot == group.decision.slot &&
                existing.decision.generation ==
                    group.decision.generation &&
                existing.sliceIndex == group.sliceIndex) {
                *groupStatus = Plan03GroupReserveStatus::DuplicateSlice;
                groupsAdmitted = false;
                break;
            }
        }
        if (!groupsAdmitted) break;
        if (usable < samePageCount) {
            *groupStatus = Plan03GroupReserveStatus::PageGroupCapacity;
            groupsAdmitted = false;
            break;
        }
        if (!priorPage) {
            *groupStatus = PreflightPlan03GroupSlices(
                group.executeFrame, samePageCount);
            if (*groupStatus != Plan03GroupReserveStatus::Committed) {
                groupsAdmitted = false;
                break;
            }
        }
    }
    return groupsAdmitted;
}

bool ScheduledTransactionPool::PreflightUnplacedBatchMaterialization(
    const UnplacedBatchCredit& credit,
    const TransactionRequest* requests, std::size_t requestCount,
    const Plan03GroupSliceInput* groups, std::size_t groupCount,
    const TimelineArmTable& armTable,
    std::uint64_t publicationFrontier,
    ReserveRefusal* transactionRefusal,
    Plan03GroupReserveStatus* groupStatus) noexcept {
    if (transactionRefusal == nullptr || groupStatus == nullptr) return false;
    *transactionRefusal = ReserveRefusal::None;
    *groupStatus = Plan03GroupReserveStatus::Committed;
    if (!Valid() || !credit.Valid() || credit.epoch != epoch_ ||
        credit.slot >= unplacedCredits_.size() ||
        requestCount > UINT32_MAX || groupCount > UINT32_MAX ||
        (requestCount != 0 && requests == nullptr) ||
        (groupCount != 0 && groups == nullptr)) {
        *transactionRefusal = ReserveRefusal::EmptyTransaction;
        return false;
    }
    const auto& holder = unplacedCredits_[credit.slot];
    const auto& demand = holder.request;
    if (!holder.active || holder.materializing ||
        holder.generation != credit.generation ||
        holder.epoch != credit.epoch ||
        demand.transactions != requestCount ||
        demand.preparedClaims != (requestCount == 0 ? 0u : 1u) ||
        demand.groupHeaders != groupCount ||
        unplacedTransactions_ < demand.transactions ||
        unplacedPreparedClaims_ < demand.preparedClaims ||
        unplacedEvents_ < demand.events ||
        unplacedFrameBatches_ < demand.frameBatches ||
        unplacedGroupHeaders_ < demand.groupHeaders ||
        (holder.chargedQuantum == baseQuantumIndex_ &&
         unplacedIngressThisQuantum_ < demand.transactions)) {
        *transactionRefusal = ReserveRefusal::PoolFull;
        return false;
    }
    std::uint32_t eventCount = 0;
    std::uint32_t distinctFrameCount = 0;
    for (std::size_t request = 0; request < requestCount; ++request) {
        const auto& candidate = requests[request];
        if (candidate.events == nullptr || candidate.eventCount == 0 ||
            candidate.eventCount > demand.events - eventCount) {
            *transactionRefusal = ReserveRefusal::EmptyTransaction;
            return false;
        }
        for (std::size_t event = 0; event < candidate.eventCount; ++event) {
            const auto frame = candidate.events[event].executeFrame;
            bool priorFrame = false;
            for (std::size_t priorRequest = 0;
                 priorRequest <= request && !priorFrame; ++priorRequest) {
                const auto& prior = requests[priorRequest];
                const auto limit = priorRequest == request
                    ? event : prior.eventCount;
                for (std::size_t priorEvent = 0;
                     priorEvent < limit; ++priorEvent) {
                    if (prior.events[priorEvent].executeFrame == frame) {
                        priorFrame = true;
                        break;
                    }
                }
            }
            if (!priorFrame) ++distinctFrameCount;
        }
        eventCount += static_cast<std::uint32_t>(candidate.eventCount);
    }
    if (eventCount != demand.events ||
        distinctFrameCount > demand.frameBatches) {
        *transactionRefusal = ReserveRefusal::PageBatchCapacity;
        return false;
    }

    // The exact holder stays live while its charge is excluded. The logic
    // writer cannot reenter another admission until this preflight returns.
    unplacedTransactions_ -= demand.transactions;
    unplacedPreparedClaims_ -= demand.preparedClaims;
    unplacedEvents_ -= demand.events;
    unplacedFrameBatches_ -= demand.frameBatches;
    unplacedGroupHeaders_ -= demand.groupHeaders;
    const bool chargedHere = holder.chargedQuantum == baseQuantumIndex_;
    if (chargedHere) unplacedIngressThisQuantum_ -= demand.transactions;

    bool admitted = false;
    do {
        if (requestCount != 0) {
            *transactionRefusal = PreflightTransactionBatch(
                requests, requestCount, publicationFrontier);
            if (*transactionRefusal != ReserveRefusal::None) break;
            if (freePreparedTransactionClaims_.size() <=
                unplacedPreparedClaims_) {
                *transactionRefusal = ReserveRefusal::PoolFull;
                break;
            }
        }
        if (groupCount != 0 &&
            (plan03ArmTable_ != nullptr && plan03ArmTable_ != &armTable)) {
            *groupStatus = Plan03GroupReserveStatus::Invalid;
            break;
        }
        admitted = PreflightMaterializationGroups(
            groups, groupCount, armTable, publicationFrontier,
            groupStatus);
    } while (false);

    unplacedTransactions_ += demand.transactions;
    unplacedPreparedClaims_ += demand.preparedClaims;
    unplacedEvents_ += demand.events;
    unplacedFrameBatches_ += demand.frameBatches;
    unplacedGroupHeaders_ += demand.groupHeaders;
    if (chargedHere) unplacedIngressThisQuantum_ += demand.transactions;
    return admitted;
}

ScheduledTransactionPool::PrivateMaterializationTransaction::
    ~PrivateMaterializationTransaction() {
    if (owner_ != nullptr)
        (void)owner_->RollbackPrivateMaterialization(this);
}

bool ScheduledTransactionPool::TryMaterializeUnplacedBatch(
    UnplacedBatchCredit* credit,
    const TransactionRequest* requests, std::size_t requestCount,
    const Plan03GroupSliceInput* groups, std::size_t groupCount,
    const TimelineArmTable& armTable,
    std::uint64_t publicationFrontier,
    PrivateMaterializationTransaction* out,
    ReserveRefusal* transactionRefusal,
    Plan03GroupReserveStatus* groupStatus) noexcept {
    if (credit == nullptr || out == nullptr || out->Valid() ||
        privateMaterializationActive_ ||
        !PreflightUnplacedBatchMaterialization(
            *credit, requests, requestCount, groups, groupCount,
            armTable, publicationFrontier,
            transactionRefusal, groupStatus)) return false;
    if (groupCount > privatePreparedGroups_.size()) {
        *groupStatus = Plan03GroupReserveStatus::PageGroupCapacity;
        return false;
    }
    if (out->transactions_.Valid() || out->committed_ ||
        std::any_of(privatePreparedGroups_.begin(),
                    privatePreparedGroups_.begin() + groupCount,
                    [](const auto& handle) { return handle.Valid(); }) ||
        std::any_of(privateCommittedGroups_.begin(),
                    privateCommittedGroups_.begin() + groupCount,
                    [](const auto& handle) { return handle.Valid(); })) {
        terminalState_.store(1, std::memory_order_release);
        *groupStatus = Plan03GroupReserveStatus::Invalid;
        return false;
    }
    auto& holder = unplacedCredits_[credit->slot];
    const auto& demand = holder.request;
    holder.materializing = true;
    unplacedTransactions_ -= demand.transactions;
    unplacedPreparedClaims_ -= demand.preparedClaims;
    unplacedEvents_ -= demand.events;
    unplacedFrameBatches_ -= demand.frameBatches;
    unplacedGroupHeaders_ -= demand.groupHeaders;
    if (holder.chargedQuantum == baseQuantumIndex_)
        unplacedIngressThisQuantum_ -= demand.transactions;
    out->owner_ = this;
    out->credit_ = credit;
    out->groups_ = privatePreparedGroups_.data();
    out->committedGroups_ = privateCommittedGroups_.data();
    out->groupCount_ = static_cast<std::uint32_t>(groupCount);
    privateMaterializationActive_ = true;
    if (requestCount != 0) {
        *transactionRefusal = PrepareTransactionBatch(
            requests, requestCount, publicationFrontier,
            &out->transactions_);
        if (*transactionRefusal != ReserveRefusal::None) {
            (void)RollbackPrivateMaterialization(out);
            return false;
        }
    }
    if (privateMaterializationFaultStepForTest_ == 1u) {
        privateMaterializationFaultStepForTest_ = 0;
        *transactionRefusal = ReserveRefusal::PoolFull;
        (void)RollbackPrivateMaterialization(out);
        return false;
    }
    for (std::size_t index = 0; index < groupCount; ++index) {
        *groupStatus = PreparePlan03GroupSlice(
            groups[index], armTable, &out->groups_[index]);
        if (*groupStatus != Plan03GroupReserveStatus::Committed) {
            (void)RollbackPrivateMaterialization(out);
            return false;
        }
        if (privateMaterializationFaultStepForTest_ == index + 2u) {
            privateMaterializationFaultStepForTest_ = 0;
            *groupStatus = Plan03GroupReserveStatus::PageGroupCapacity;
            (void)RollbackPrivateMaterialization(out);
            return false;
        }
    }
    return true;
}

bool ScheduledTransactionPool::TryMaterializeUnplacedCohort(
    const UnplacedBatchCohortMember* members, std::size_t memberCount,
    const TimelineArmTable& armTable,
    std::uint64_t publicationFrontier,
    PrivateMaterializationTransaction* out,
    ReserveRefusal* transactionRefusal,
    Plan03GroupReserveStatus* groupStatus) noexcept {
    if (transactionRefusal == nullptr || groupStatus == nullptr) return false;
    *transactionRefusal = ReserveRefusal::None;
    *groupStatus = Plan03GroupReserveStatus::Committed;
    if (!Valid() || members == nullptr || memberCount == 0 ||
        memberCount > UINT32_MAX ||
        memberCount > privateCohortCredits_.size() || out == nullptr ||
        out->Valid() || privateMaterializationActive_) {
        *transactionRefusal = ReserveRefusal::PoolFull;
        return false;
    }
    std::size_t requestCount = 0;
    std::size_t groupCount = 0;
    std::uint64_t totalTransactions = 0;
    std::uint64_t totalClaims = 0;
    std::uint64_t totalEvents = 0;
    std::uint64_t totalBatches = 0;
    std::uint64_t totalGroups = 0;
    std::uint64_t chargedIngress = 0;
    for (std::size_t index = 0; index < memberCount; ++index) {
        const auto& member = members[index];
        const auto* credit = member.credit;
        if (credit == nullptr || !credit->Valid() ||
            credit->epoch != epoch_ ||
            credit->slot >= unplacedCredits_.size() ||
            (member.requestCount != 0 && member.requests == nullptr) ||
            (member.groupCount != 0 && member.groups == nullptr) ||
            member.requestCount > privateCohortRequests_.size() - requestCount ||
            member.groupCount > privateCohortGroups_.size() - groupCount) {
            *transactionRefusal = ReserveRefusal::PoolFull;
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior)
            if (privateCohortCredits_[prior]->slot == credit->slot) {
                *transactionRefusal = ReserveRefusal::PoolFull;
                return false;
            }
        const auto& holder = unplacedCredits_[credit->slot];
        const auto& demand = holder.request;
        if (!holder.active || holder.materializing ||
            holder.generation != credit->generation ||
            holder.epoch != credit->epoch ||
            demand.transactions != member.requestCount ||
            demand.preparedClaims !=
                (member.requestCount == 0 ? 0u : 1u) ||
            demand.groupHeaders != member.groupCount) {
            *transactionRefusal = ReserveRefusal::PoolFull;
            return false;
        }
        std::uint32_t events = 0;
        std::uint32_t distinctFrames = 0;
        for (std::size_t request = 0; request < member.requestCount;
             ++request) {
            const auto& candidate = member.requests[request];
            if (candidate.events == nullptr || candidate.eventCount == 0 ||
                candidate.eventCount > demand.events - events) {
                *transactionRefusal = ReserveRefusal::EmptyTransaction;
                return false;
            }
            for (std::size_t event = 0; event < candidate.eventCount; ++event) {
                const auto frame = candidate.events[event].executeFrame;
                bool priorFrame = false;
                for (std::size_t earlier = 0;
                     earlier <= request && !priorFrame; ++earlier) {
                    const auto& previous = member.requests[earlier];
                    const auto limit = earlier == request
                        ? event : previous.eventCount;
                    for (std::size_t earlierEvent = 0;
                         earlierEvent < limit; ++earlierEvent)
                        if (previous.events[earlierEvent].executeFrame == frame) {
                            priorFrame = true;
                            break;
                        }
                }
                if (!priorFrame) ++distinctFrames;
            }
            events += static_cast<std::uint32_t>(candidate.eventCount);
            privateCohortRequests_[requestCount++] = candidate;
        }
        if (events != demand.events ||
            distinctFrames > demand.frameBatches) {
            *transactionRefusal = ReserveRefusal::PageBatchCapacity;
            return false;
        }
        for (std::size_t group = 0; group < member.groupCount; ++group)
            privateCohortGroups_[groupCount++] = member.groups[group];
        privateCohortCredits_[index] = member.credit;
        totalTransactions += demand.transactions;
        totalClaims += demand.preparedClaims;
        totalEvents += demand.events;
        totalBatches += demand.frameBatches;
        totalGroups += demand.groupHeaders;
        if (holder.chargedQuantum == baseQuantumIndex_)
            chargedIngress += demand.transactions;
    }
    if (totalTransactions > unplacedTransactions_ ||
        totalClaims > unplacedPreparedClaims_ ||
        totalEvents > unplacedEvents_ ||
        totalBatches > unplacedFrameBatches_ ||
        totalGroups > unplacedGroupHeaders_ ||
        chargedIngress > unplacedIngressThisQuantum_ ||
        requestCount > UINT32_MAX || groupCount > UINT32_MAX) {
        *transactionRefusal = ReserveRefusal::PoolFull;
        return false;
    }
    if (out->transactions_.Valid() || out->committed_ ||
        std::any_of(privatePreparedGroups_.begin(),
                    privatePreparedGroups_.begin() + groupCount,
                    [](const auto& handle) { return handle.Valid(); }) ||
        std::any_of(privateCommittedGroups_.begin(),
                    privateCommittedGroups_.begin() + groupCount,
                    [](const auto& handle) { return handle.Valid(); })) {
        terminalState_.store(1, std::memory_order_release);
        *groupStatus = Plan03GroupReserveStatus::Invalid;
        return false;
    }
    auto excludeCredits = [&] {
        unplacedTransactions_ -= totalTransactions;
        unplacedPreparedClaims_ -= totalClaims;
        unplacedEvents_ -= totalEvents;
        unplacedFrameBatches_ -= totalBatches;
        unplacedGroupHeaders_ -= totalGroups;
        unplacedIngressThisQuantum_ -=
            static_cast<std::uint32_t>(chargedIngress);
    };
    auto restoreCredits = [&] {
        unplacedTransactions_ += totalTransactions;
        unplacedPreparedClaims_ += totalClaims;
        unplacedEvents_ += totalEvents;
        unplacedFrameBatches_ += totalBatches;
        unplacedGroupHeaders_ += totalGroups;
        unplacedIngressThisQuantum_ +=
            static_cast<std::uint32_t>(chargedIngress);
    };
    excludeCredits();
    if (requestCount != 0) {
        *transactionRefusal = PreflightTransactionBatch(
            privateCohortRequests_.data(), requestCount,
            publicationFrontier);
        if (*transactionRefusal == ReserveRefusal::None &&
            freePreparedTransactionClaims_.size() <=
                unplacedPreparedClaims_)
            *transactionRefusal = ReserveRefusal::PoolFull;
    }
    if (*transactionRefusal != ReserveRefusal::None ||
        !PreflightMaterializationGroups(
            privateCohortGroups_.data(), groupCount, armTable,
            publicationFrontier, groupStatus)) {
        restoreCredits();
        return false;
    }
    for (std::size_t index = 0; index < memberCount; ++index)
        unplacedCredits_[privateCohortCredits_[index]->slot].materializing = true;
    out->owner_ = this;
    out->cohortCredits_ = privateCohortCredits_.data();
    out->cohortCreditCount_ = static_cast<std::uint32_t>(memberCount);
    out->groups_ = privatePreparedGroups_.data();
    out->committedGroups_ = privateCommittedGroups_.data();
    out->groupCount_ = static_cast<std::uint32_t>(groupCount);
    privateMaterializationActive_ = true;
    if (requestCount != 0) {
        *transactionRefusal = PrepareTransactionBatch(
            privateCohortRequests_.data(), requestCount,
            publicationFrontier, &out->transactions_);
        if (*transactionRefusal != ReserveRefusal::None) {
            (void)RollbackPrivateMaterialization(out);
            return false;
        }
    }
    if (privateMaterializationFaultStepForTest_ == 1u) {
        privateMaterializationFaultStepForTest_ = 0;
        *transactionRefusal = ReserveRefusal::PoolFull;
        (void)RollbackPrivateMaterialization(out);
        return false;
    }
    for (std::size_t index = 0; index < groupCount; ++index) {
        *groupStatus = PreparePlan03GroupSlice(
            privateCohortGroups_[index], armTable, &out->groups_[index]);
        if (*groupStatus != Plan03GroupReserveStatus::Committed) {
            (void)RollbackPrivateMaterialization(out);
            return false;
        }
        if (privateMaterializationFaultStepForTest_ == index + 2u) {
            privateMaterializationFaultStepForTest_ = 0;
            *groupStatus = Plan03GroupReserveStatus::PageGroupCapacity;
            (void)RollbackPrivateMaterialization(out);
            return false;
        }
    }
    return true;
}

bool ScheduledTransactionPool::CanRollbackPrivateMaterialization(
    const PrivateMaterializationTransaction& transaction) const noexcept {
    if (transaction.owner_ != this || transaction.committed_)
        return false;
    for (std::uint32_t index = 0; index < transaction.groupCount_;
         ++index) {
        const auto& prepared = transaction.groups_[index];
        if (!prepared.Valid()) continue;
        if (prepared.owner_ != this ||
            prepared.rowIndex_ >= pagePlan03Groups_.size() ||
            pagePlan03GroupRowStates_[prepared.rowIndex_] !=
                Plan03GroupRowState::Prepared ||
            pagePlan03GroupClaimGenerations_[prepared.rowIndex_] !=
                prepared.generation_) return false;
        const auto ring = static_cast<std::uint32_t>(
            prepared.rowIndex_ / config_.maxPlan03GroupHeadersPerPage);
        if (pagePreparedPlan03GroupCount_[ring] == 0 ||
            pageHeaders_[ring].frozen ||
            pagePlan03GroupReaders_[prepared.rowIndex_].pins.load(
                std::memory_order_acquire) != 0 ||
            pagePlan03GroupLeases_[prepared.rowIndex_].Capture().state !=
                AudioSliceState::Ready) return false;
    }
    const auto& prepared = transaction.transactions_;
    if (!prepared.Valid()) return true;
    if (prepared.owner_ != this ||
        prepared.claimSlot_ >= preparedTransactionClaims_.size())
        return false;
    const auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    if (!claim.used || claim.committed ||
        claim.generation != prepared.generation_ ||
        claim.transactionCount == 0 ||
        (claim.preparedIngressQuantum == baseQuantumIndex_ &&
         preparedIngressThisQuantum_ < claim.transactionCount))
        return false;
    std::uint32_t matched = 0;
    for (const auto& record : transactions_)
        if (record.prepared &&
            record.preparedClaimSlot == prepared.claimSlot_ &&
            record.preparedClaimGeneration == prepared.generation_)
            ++matched;
    if (matched != claim.transactionCount) return false;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto base = static_cast<std::size_t>(ring) *
            config_.maxEventsPerQuantum;
        std::uint32_t ownedEvents = 0;
        for (std::uint32_t index = 0;
             index < pagePreparedEventCount_[ring]; ++index) {
            const auto& row = pageEvents_[
                base + pageEventCount_[ring] + index];
            if (row.txnSlot >= transactions_.size()) return false;
            const auto& record = transactions_[row.txnSlot];
            if (record.preparedClaimSlot != prepared.claimSlot_ ||
                record.preparedClaimGeneration != prepared.generation_)
                continue;
            ++ownedEvents;
            bool batchFound = false;
            const auto batchBase = static_cast<std::size_t>(ring) *
                config_.maxFrameBatchesPerQuantum;
            for (std::uint32_t batch = 0;
                 batch < pageBatchOccupiedCount_[ring]; ++batch)
                if (pageBatchRows_[batchBase + batch].frame ==
                        row.executeFrame &&
                    pageBatchRows_[batchBase + batch].preparedReferences != 0) {
                    batchFound = true;
                    break;
                }
            if (!batchFound) return false;
        }
        if (ownedEvents != 0 &&
            (pageHeaders_[ring].frozen ||
             pagePublicationHolds_[ring] < ownedEvents)) return false;
        const auto batchBase = static_cast<std::size_t>(ring) *
            config_.maxFrameBatchesPerQuantum;
        for (std::uint32_t batch = 0;
             batch < pageBatchOccupiedCount_[ring]; ++batch) {
            const auto& frameBatch = pageBatchRows_[batchBase + batch];
            std::uint32_t ownedReferences = 0;
            for (std::uint32_t index = 0;
                 index < pagePreparedEventCount_[ring]; ++index) {
                const auto& row = pageEvents_[
                    base + pageEventCount_[ring] + index];
                if (row.executeFrame != frameBatch.frame ||
                    row.txnSlot >= transactions_.size()) continue;
                const auto& record = transactions_[row.txnSlot];
                if (record.preparedClaimSlot == prepared.claimSlot_ &&
                    record.preparedClaimGeneration == prepared.generation_)
                    ++ownedReferences;
            }
            if (frameBatch.preparedReferences < ownedReferences)
                return false;
        }
    }
    return true;
}

bool ScheduledTransactionPool::CanChargePreparedTransactionIngress(
    const PreparedTransactionClaim& claim) const noexcept {
    const auto occupied = static_cast<std::uint64_t>(ingressThisQuantum_) +
        preparedIngressThisQuantum_ + unplacedIngressThisQuantum_;
    if (claim.transactionCount == 0 || occupied > config_.maxIngressTransactionsPerQuantum)
        return false;
    if (claim.preparedIngressQuantum == baseQuantumIndex_)
        return preparedIngressThisQuantum_ >= claim.transactionCount;
    return claim.transactionCount <= config_.maxIngressTransactionsPerQuantum - occupied;
}

bool ScheduledTransactionPool::CanCommitPrivateMaterialization(
    const PrivateMaterializationTransaction& transaction,
    LogicCommitSequence sequence) const noexcept {
    if (sequence == 0 || !CanRollbackPrivateMaterialization(transaction))
        return false;
    for (std::uint32_t index = 0; index < transaction.groupCount_; ++index) {
        const auto& prepared = transaction.groups_[index];
        if (!prepared.Valid() || !prepared.prepared_ ||
            transaction.committedGroups_[index].Valid()) return false;
    }
    const auto& prepared = transaction.transactions_;
    if (!prepared.Valid()) return true;
    const auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    if (!prepared.prepared_ || claim.transactionCount == 0 ||
        (claim.frozenSequence != 0 &&
         claim.frozenSequence != sequence) ||
        !CanChargePreparedTransactionIngress(claim)) return false;
    return true;
}

bool ScheduledTransactionPool::RollbackPrivateMaterialization(
    PrivateMaterializationTransaction* transaction) noexcept {
    if (transaction == nullptr || transaction->owner_ != this ||
        transaction->committed_) {
        terminalState_.store(1, std::memory_order_release);
        return false;
    }
    const auto creditCount = transaction->cohortCreditCount_ != 0
        ? transaction->cohortCreditCount_ : 1u;
    const auto creditAt = [&](std::uint32_t index) {
        return transaction->cohortCreditCount_ != 0
            ? transaction->cohortCredits_[index] : transaction->credit_;
    };
    for (std::uint32_t index = 0; index < creditCount; ++index) {
        const auto* credit = creditAt(index);
        if (credit == nullptr || !credit->Valid() ||
            credit->slot >= unplacedCredits_.size()) {
            terminalState_.store(1, std::memory_order_release);
            return false;
        }
        const auto& holder = unplacedCredits_[credit->slot];
        if (!holder.active || !holder.materializing ||
            holder.generation != credit->generation ||
            holder.epoch != credit->epoch) {
            terminalState_.store(1, std::memory_order_release);
            return false;
        }
    }
    // The exact prepared handles are private to this logic call. Their pages
    // are held from publication, and no other logic admission can run between
    // this proof and the checked cancellation below.
    if (!CanRollbackPrivateMaterialization(*transaction)) {
        terminalState_.store(1, std::memory_order_release);
        return false;
    }
    bool rolledBack = true;
    for (std::uint32_t index = transaction->groupCount_;
         index != 0; --index) {
        auto& group = transaction->groups_[index - 1u];
        auto& committed = transaction->committedGroups_[index - 1u];
        if (committed.Valid())
            rolledBack = RollbackCommittedPlan03GroupSlice(&committed) &&
                rolledBack;
        else if (group.Valid())
            rolledBack = CancelPreparedPlan03GroupSlice(&group) && rolledBack;
    }
    if (transaction->transactions_.Valid())
        rolledBack = CancelPreparedTransactionBatch(
            &transaction->transactions_) && rolledBack;
    if (!rolledBack) {
        terminalState_.store(1, std::memory_order_release);
        return false;
    }
    for (std::uint32_t index = 0; index < creditCount; ++index) {
        auto* credit = creditAt(index);
        auto& holder = unplacedCredits_[credit->slot];
        const auto& demand = holder.request;
        unplacedTransactions_ += demand.transactions;
        unplacedPreparedClaims_ += demand.preparedClaims;
        unplacedEvents_ += demand.events;
        unplacedFrameBatches_ += demand.frameBatches;
        unplacedGroupHeaders_ += demand.groupHeaders;
        if (holder.chargedQuantum == baseQuantumIndex_)
            unplacedIngressThisQuantum_ += demand.transactions;
        holder.materializing = false;
    }
    privateMaterializationActive_ = false;
    transaction->owner_ = nullptr;
    transaction->credit_ = nullptr;
    transaction->cohortCredits_ = nullptr;
    transaction->cohortCreditCount_ = 0;
    transaction->groups_ = nullptr;
    transaction->committedGroups_ = nullptr;
    transaction->groupCount_ = 0;
    transaction->committed_ = false;
    return true;
}

bool ScheduledTransactionPool::PrivateMaterializationTransaction::Commit(
    LogicCommitSequence sequence) noexcept {
    if (owner_ == nullptr || committed_ || sequence == 0) return false;
    if (!owner_->CanCommitPrivateMaterialization(*this, sequence))
        return false;
    if (transactions_.Valid() &&
        !owner_->CommitPreparedTransactionBatch(
            &transactions_, sequence, true)) {
        owner_->terminalState_.store(1, std::memory_order_release);
        return false;
    }
    for (std::uint32_t index = 0; index < groupCount_; ++index) {
        if (!owner_->CommitPreparedPlan03GroupSlice(
                &groups_[index], &committedGroups_[index], true)) {
            owner_->terminalState_.store(1, std::memory_order_release);
            return false;
        }
    }
    committed_ = true;
    return true;
}

bool ScheduledTransactionPool::PrivateMaterializationTransaction::
    ReleasePublication() noexcept {
    if (owner_ == nullptr || !committed_)
        return false;
    const auto creditCount = cohortCreditCount_ != 0
        ? cohortCreditCount_ : 1u;
    const auto creditAt = [&](std::uint32_t index) {
        return cohortCreditCount_ != 0
            ? cohortCredits_[index] : credit_;
    };
    for (std::uint32_t index = 0; index < creditCount; ++index) {
        const auto* credit = creditAt(index);
        if (credit == nullptr || !credit->Valid() ||
            credit->slot >= owner_->unplacedCredits_.size()) {
            owner_->terminalState_.store(1, std::memory_order_release);
            return false;
        }
        const auto& holder = owner_->unplacedCredits_[credit->slot];
        if (!holder.active || !holder.materializing ||
            holder.generation != credit->generation ||
            holder.epoch != credit->epoch) {
            owner_->terminalState_.store(1, std::memory_order_release);
            return false;
        }
    }
    if (transactions_.Valid() &&
        !owner_->CanReleasePreparedTransactionPublication(transactions_)) {
        owner_->terminalState_.store(1, std::memory_order_release);
        return false;
    }
    for (std::uint32_t index = 0; index < groupCount_; ++index) {
        if (!committedGroups_[index].Valid()) {
            owner_->terminalState_.store(1, std::memory_order_release);
            return false;
        }
    }
    bool released = true;
    if (transactions_.Valid())
        released = owner_->ReleasePreparedTransactionPublication(
            &transactions_) && released;
    for (std::uint32_t index = 0; index < groupCount_; ++index)
        released = owner_->ReleaseCommittedPlan03GroupSliceOwnership(
            &committedGroups_[index]) && released;
    if (!released) {
        owner_->terminalState_.store(1, std::memory_order_release);
        return false;
    }
    for (std::uint32_t index = 0; index < creditCount; ++index) {
        auto* credit = creditAt(index);
        owner_->unplacedCredits_[credit->slot] = {};
        *credit = {};
    }
    owner_->privateMaterializationActive_ = false;
    owner_ = nullptr;
    credit_ = nullptr;
    cohortCredits_ = nullptr;
    cohortCreditCount_ = 0;
    groups_ = nullptr;
    committedGroups_ = nullptr;
    groupCount_ = 0;
    committed_ = false;
    return true;
}

ReserveRefusal ScheduledTransactionPool::CheckUnplacedPageBudget(
    std::uint64_t additionalEvents,
    std::uint64_t additionalBatches) const noexcept {
    std::uint64_t occupiedEvents = unplacedEvents_;
    std::uint64_t occupiedBatches = unplacedFrameBatches_;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        occupiedEvents += pageEventCount_[ring] + pagePreparedEventCount_[ring];
        occupiedBatches += pageBatchOccupiedCount_[ring];
    }
    if (occupiedEvents > pageEvents_.size() ||
        additionalEvents > pageEvents_.size() - occupiedEvents)
        return ReserveRefusal::PageEventCapacity;
    if (occupiedBatches > pageBatchRows_.size() ||
        additionalBatches > pageBatchRows_.size() - occupiedBatches)
        return ReserveRefusal::PageBatchCapacity;
    return ReserveRefusal::None;
}

bool ScheduledTransactionPool::HasUnplacedGroupBudget(
    std::uint32_t additionalGroups) const noexcept {
    std::uint64_t occupied = unplacedGroupHeaders_;
    for (const auto state : pagePlan03GroupRowStates_)
        occupied += state != Plan03GroupRowState::Free ? 1u : 0u;
    return occupied <= pagePlan03GroupRowStates_.size() &&
        additionalGroups <= pagePlan03GroupRowStates_.size() - occupied;
}

bool ScheduledTransactionPool::CanResetForEpoch(
    std::uint64_t startFrame, TransportEpoch epoch) const noexcept {
    if (laneGroupPublications_ != nullptr) return false;
    if (selectedFutureDomains_ != 0 || preparedPageFrontierBindingHolds_ != 0 || privateUnboundPageArmRetirements_ != nullptr || unpublishedGroupArmCancellations_ != nullptr ||
        privateMaterializationActive_ || HasPlan03GroupReaders()) return false;
    if (!Valid() || epoch == kInvalidTransportEpoch || pageCount_ == 0 ||
        quantumFrames_ == 0 || startFrame % quantumFrames_ != 0 ||
        startFrame > UINT64_MAX - config_.scheduleHorizonFrames ||
        consumedFrontierQuantum_ == nullptr || pageQuantumStamp_ == nullptr ||
        pageEmptyQuantumStamp_ == nullptr) {
        return false;
    }
    for (std::size_t row = 0; row < config_.maxImmediateLaneGroupRows; ++row) {
        const auto state = immediateLaneGroups_[row].state.load(std::memory_order_acquire);
        if (immediateLaneGroups_[row].clockProof != nullptr ||
                immediateLaneGroups_[row].originalRegistrationPinCount != 0 ||
            (state != LaneGroupState::Free && state != LaneGroupState::Retired))
            return false;
    }
    for (std::size_t gate = 0; gate < config_.maxImmediateLaneOwnerGates; ++gate)
        if (immediateLaneOwnerGates_[gate].references != 0 ||
            (immediateLaneOwnerGates_[gate].state.load(std::memory_order_acquire) &
             kLaneGateClaims) != 0)
            return false;
    for (std::size_t gate = 0; gate < config_.maxImmediateLaneDecisionGates; ++gate)
        if (immediateLaneDecisionGates_[gate].references != 0 ||
            (immediateLaneDecisionGates_[gate].state.load(std::memory_order_acquire) &
             kLaneGateClaims) != 0)
            return false;

    const std::size_t pageCount = pageCount_;
    const auto exactProduct = [](std::size_t left, std::size_t right,
                                 std::size_t actual) noexcept {
        return right == 0 ? actual == 0
                          : left <= std::numeric_limits<std::size_t>::max() /
                                        right &&
                                left * right == actual;
    };
    const std::size_t groupRows =
        pageCount * config_.maxPlan03GroupHeadersPerPage;
    if (pageHeaders_.size() != pageCount ||
        pageEventCount_.size() != pageCount ||
        pagePreparedEventCount_.size() != pageCount ||
        pagePublicationHolds_.size() != pageCount ||
        pageSorted_.size() != pageCount ||
        !exactProduct(pageCount, config_.maxEventsPerQuantum,
                      pageEvents_.size()) ||
        pageEventOrder_.size() != pageEvents_.size() ||
        pageCurrentSliceCount_.size() != pageCount ||
        !exactProduct(pageCount, currentSliceStorageCapacity_,
                      pageCurrentSlices_.size()) ||
        pageCurrentSliceOrder_.size() != pageCurrentSlices_.size() ||
        pagePlan03GroupCount_.size() != pageCount ||
        pagePreparedPlan03GroupCount_.size() != pageCount ||
        !exactProduct(pageCount, config_.maxPlan03GroupHeadersPerPage,
                      pagePlan03Groups_.size()) ||
        pagePlan03GroupRowStates_.size() != pagePlan03Groups_.size() ||
        pagePlan03GroupClaimGenerations_.size() !=
            pagePlan03Groups_.size() ||
        !exactProduct(groupRows, config_.maxArmsPerGroup,
                      pagePlan03GroupMembers_.size()) ||
        (groupRows == 0) != (pagePlan03GroupLeases_ == nullptr) ||
        pageBatchCount_.size() != pageCount ||
        pageBatchOccupiedCount_.size() != pageCount ||
        !exactProduct(pageCount, config_.maxFrameBatchesPerQuantum,
                      pageBatchRows_.size()) ||
        transactions_.size() != config_.maxScheduledTransactions ||
        freeTransactions_.capacity() < config_.maxScheduledTransactions ||
        preparedTransactionClaims_.size() !=
            config_.maxScheduledTransactions ||
        unplacedCredits_.size() !=
            static_cast<std::size_t>(config_.maxScheduledTransactions) + groupRows ||
        privatePreparedGroups_.size() != groupRows ||
        privateCommittedGroups_.size() != groupRows ||
        privateCohortCredits_.size() != unplacedCredits_.size() ||
        privateCohortRequests_.size() !=
            config_.maxScheduledTransactions ||
        privateCohortGroups_.size() != groupRows ||
        freePreparedTransactionClaims_.capacity() <
            config_.maxScheduledTransactions ||
        scratchEventPage_.size() != config_.maxTransactionEvents ||
        scratchEventFrame_.size() != config_.maxTransactionEvents ||
        refusalCounts_.size() !=
        static_cast<std::size_t>(ReserveRefusal::CommitSerialExhausted) + 1) {
        return false;
    }

    for (std::uint32_t page = 0; page < pageCount_; ++page) {
        if (pageHeaders_[page].pageGeneration == UINT64_MAX ||
            pageEventCount_[page] > config_.maxEventsPerQuantum ||
            pagePreparedEventCount_[page] != 0 ||
            pagePublicationHolds_[page] != 0 ||
            pageCurrentSliceCount_[page] >
                currentSliceStorageCapacity_ ||
            pagePlan03GroupCount_[page] >
                config_.maxPlan03GroupHeadersPerPage ||
            pagePreparedPlan03GroupCount_[page] != 0 ||
            pageBatchCount_[page] > config_.maxFrameBatchesPerQuantum ||
            pageBatchOccupiedCount_[page] != pageBatchCount_[page]) {
            return false;
        }
        const std::size_t groupBase =
            static_cast<std::size_t>(page) *
            config_.maxPlan03GroupHeadersPerPage;
        for (std::uint32_t group = 0;
             group < config_.maxPlan03GroupHeadersPerPage; ++group) {
            const std::size_t rowIndex = groupBase + group;
            if (pagePlan03GroupRowStates_[rowIndex] !=
                Plan03GroupRowState::Live) {
                continue;
            }
            const auto* effectiveLease = EffectivePlan03GroupLease(rowIndex);
            if (effectiveLease == nullptr) return false;
            const AudioSliceState state = effectiveLease->Capture().state;
            if (state != AudioSliceState::SliceApplied &&
                state != AudioSliceState::SliceInvalidated) {
                return false;
            }
            if (plan03ArmTable_ == nullptr) {
                return false;
            }
            const Plan03GroupSliceRow& row =
                pagePlan03Groups_[rowIndex];
            if (row.memberCount > config_.maxArmsPerGroup) {
                return false;
            }
            const std::size_t memberBase =
                rowIndex * config_.maxArmsPerGroup;
            for (std::uint16_t member = 0; member < row.memberCount; ++member) {
                TimelineArmObservation observation{};
                if (plan03ArmTable_->Observe(
                        pagePlan03GroupMembers_[memberBase + member],
                        &observation)) {
                    return false;
                }
            }
        }
    }
    return preparedIngressThisQuantum_ == 0 &&
           preparedTransactionClaimCount_ == 0;
}

bool ScheduledTransactionPool::ResetForEpoch(
    std::uint64_t startFrame, TransportEpoch epoch) noexcept {
    if (!CanResetForEpoch(startFrame, epoch)) {
        return false;
    }
    ReturnAllGroupMemberOriginals();
    for (std::size_t slot = 0; slot < preparedTransactionClaims_.size(); ++slot)
        DetachPreparedPageBatchTransfer(slot);
    for (std::size_t row = 0; row < pagePlan03Groups_.size(); ++row)
        DetachPreparedPageGroupTransfer(row);

    for (std::uint32_t page = 0; page < pageCount_; ++page) {
        const std::size_t groupBase =
            static_cast<std::size_t>(page) *
            config_.maxPlan03GroupHeadersPerPage;
        for (std::uint32_t group = 0;
             group < config_.maxPlan03GroupHeadersPerPage; ++group) {
            const std::size_t rowIndex = groupBase + group;
            if (pagePlan03GroupRowStates_[rowIndex] ==
                Plan03GroupRowState::Live) {
                (void)pagePlan03GroupLeases_[rowIndex].ResetForReuse();
            }
        }
    }

    const std::uint64_t baseQuantum = startFrame / quantumFrames_;
    epoch_ = epoch;
    baseFrame_ = startFrame;
    baseQuantumIndex_ = baseQuantum;
    sessionBaseQuantumIndex_ = baseQuantum;
    consumedFrontierQuantum_->store(baseQuantum, std::memory_order_release);
    ringStart_ = 0;
    publishCutoffFrame_ = startFrame;
    nextRowSequence_ = 1;
    nextCurrentSliceSequence_ = 1;
    consumeTick_ = 0;

    for (std::uint32_t page = 0; page < pageCount_; ++page) {
        pageQuantumStamp_[page].store(kUnpublishedQuantumStamp,
                                      std::memory_order_release);
        pageEmptyQuantumStamp_[page].store(kUnpublishedQuantumStamp,
                                           std::memory_order_release);
        HorizonPageHeader& header = pageHeaders_[page];
        const std::uint64_t generation = header.pageGeneration + 1;
        header = {};
        header.transportEpoch = epoch;
        header.absoluteQuantumIndex = baseQuantum + page;
        header.pageGeneration = generation;
    }
    std::fill(pageEventCount_.begin(), pageEventCount_.end(), 0);
    std::fill(pagePreparedEventCount_.begin(),
              pagePreparedEventCount_.end(), 0);
    std::fill(pagePublicationHolds_.begin(), pagePublicationHolds_.end(), 0);
    std::fill(pageSorted_.begin(), pageSorted_.end(), 1);
    std::fill(pageEvents_.begin(), pageEvents_.end(), EventRow{});
    std::fill(pageEventOrder_.begin(), pageEventOrder_.end(), 0);
    std::fill(pageCurrentSliceCount_.begin(), pageCurrentSliceCount_.end(), 0);
    std::fill(pageCurrentSlices_.begin(), pageCurrentSlices_.end(),
              CurrentSliceRow{});
    std::fill(pageCurrentSliceOrder_.begin(), pageCurrentSliceOrder_.end(), 0);
    std::fill(pagePlan03GroupCount_.begin(), pagePlan03GroupCount_.end(), 0);
    std::fill(pagePreparedPlan03GroupCount_.begin(),
              pagePreparedPlan03GroupCount_.end(), 0);
    std::fill(pagePlan03Groups_.begin(), pagePlan03Groups_.end(),
              Plan03GroupSliceRow{});
    std::fill(pagePlan03GroupMembers_.begin(), pagePlan03GroupMembers_.end(),
              TimelineArmToken{});
    std::fill(pagePlan03GroupRowStates_.begin(),
              pagePlan03GroupRowStates_.end(), Plan03GroupRowState::Free);
    std::fill(pagePlan03GroupClaimGenerations_.begin(),
              pagePlan03GroupClaimGenerations_.end(), 1);
    std::fill(pageBatchCount_.begin(), pageBatchCount_.end(), 0);
    std::fill(pageBatchOccupiedCount_.begin(),
              pageBatchOccupiedCount_.end(), 0);
    std::fill(pageBatchRows_.begin(), pageBatchRows_.end(),
              FrameBatchRow{});
    plan03ArmTable_ = nullptr;
    plan03ArmTableEstablishedByPrepared_ = false;

    std::fill(transactions_.begin(), transactions_.end(), TransactionRecord{});
    freeTransactions_.resize(config_.maxScheduledTransactions);
    for (std::uint32_t index = 0;
         index < config_.maxScheduledTransactions; ++index) {
        freeTransactions_[index] =
            config_.maxScheduledTransactions - index - 1;
    }
    std::fill(preparedTransactionClaims_.begin(),
              preparedTransactionClaims_.end(),
              PreparedTransactionClaim{});
    freePreparedTransactionClaims_.resize(
        config_.maxScheduledTransactions);
    for (std::uint32_t index = 0;
         index < config_.maxScheduledTransactions; ++index) {
        freePreparedTransactionClaims_[index] =
            config_.maxScheduledTransactions - index - 1;
    }
    std::fill(unplacedCredits_.begin(), unplacedCredits_.end(),
              UnplacedCreditRow{});
    unplacedTransactions_ = 0;
    unplacedPreparedClaims_ = 0;
    unplacedEvents_ = 0;
    unplacedFrameBatches_ = 0;
    unplacedGroupHeaders_ = 0;
    unplacedIngressThisQuantum_ = 0;
    privateMaterializationFaultStepForTest_ = 0;
    std::fill(scratchEventPage_.begin(), scratchEventPage_.end(), 0);
    std::fill(scratchEventFrame_.begin(), scratchEventFrame_.end(), 0);
    std::fill(refusalCounts_.begin(), refusalCounts_.end(), 0);

    liveTransactionCount_ = 0;
    transactionHighWater_ = 0;
    frameBatchHighWater_ = 0;
    eventHighWater_ = 0;
    transactionEventsHighWater_ = 0;
    transactionSpanHighWater_ = 0;
    ingressThisQuantum_ = 0;
    preparedIngressThisQuantum_ = 0;
    preparedTransactionClaimCount_ = 0;
    ingressHighWater_ = 0;
    droppedLateCount_ = 0;
    advanceBlockedByConsumer_ = 0;
    consumerFrontierEnabled_ = false;
    return true;
}

bool ScheduledTransactionPool::PageForFrame(std::uint64_t frame,
                                            std::uint32_t* outPage) const {
    if (!Valid() || quantumFrames_ == 0) {
        return false;
    }
    if (frame < baseFrame_ || frame >= WindowEndFrame()) {
        return false;
    }
    const std::uint64_t page = (frame - baseFrame_) / quantumFrames_;
    if (outPage != nullptr) {
        *outPage = static_cast<std::uint32_t>(page);
    }
    return true;
}

const HorizonPageHeader* ScheduledTransactionPool::Page(
    std::uint32_t page) const {
    if (!Valid() || page >= pageCount_) {
        return nullptr;
    }
    return &pageHeaders_[RingIndex(page)];
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
std::size_t ScheduledTransactionPool::CapturePageEventsForTest(
    std::uint32_t page, ConsumedEvent* out,
    std::size_t capacity) const noexcept {
    if (!Valid() || page >= pageCount_ ||
        (capacity != 0 && out == nullptr)) return 0;
    const std::uint32_t ring = RingIndex(page);
    const std::size_t count = pageEventCount_[ring];
    const std::size_t base = static_cast<std::size_t>(ring) *
        config_.maxEventsPerQuantum;
    for (std::size_t index = 0; index < std::min(count, capacity); ++index) {
        const EventRow& row = pageEvents_[base + index];
        ConsumedEvent& event = out[index];
        event = {};
        event.executeFrame = row.executeFrame;
        event.logicCommitSequence = row.logicCommitSequence;
        event.executionOrdinal = row.executionOrdinal;
        event.eventIndex = row.eventIndex;
        event.transactionOrdinal = row.transactionOrdinal;
        event.subeventPhase = row.subeventPhase;
        event.rowSequence = row.rowSequence;
        event.payload = row.payload;
        event.authoredFadeOutCandidate = row.authoredFadeOutCandidate;
        event.transactionId = row.transactionId;
        event.armToken = row.armToken;
        event.txnSlot = row.txnSlot;
        event.frozenTxnValid = row.frozenTxnValid;
        event.frontier = row.frontier;
        if (row.txnSlot < transactions_.size()) {
            const TransactionRecord& record = transactions_[row.txnSlot];
            event.program = record.program;
            event.ownerInstanceHandle = record.ownerInstance;
            event.ownerInstanceScheduleEpoch = record.ownerScheduleEpoch;
            event.immediateEntry = record.immediateEntry;
        }
    }
    return count;
}

bool ScheduledTransactionPool::CaptureFrameBatchReferencesForTest(
    std::uint64_t frame, std::uint32_t* live,
    std::uint32_t* prepared) const noexcept {
    if (live == nullptr || prepared == nullptr) return false;
    std::uint32_t page = 0;
    if (!Valid() || !PageForFrame(frame, &page)) return false;
    const auto ring = RingIndex(page);
    const auto base = static_cast<std::size_t>(ring) *
        config_.maxFrameBatchesPerQuantum;
    for (std::uint32_t index = 0;
         index < pageBatchOccupiedCount_[ring]; ++index) {
        const auto& batch = pageBatchRows_[base + index];
        if (batch.frame != frame) continue;
        *live = batch.liveReferences;
        *prepared = batch.preparedReferences;
        return true;
    }
    return false;
}
#endif

bool ScheduledTransactionPool::IsFrozen(std::uint32_t page) const {
    if (!Valid() || page >= pageCount_) {
        return false;
    }
    return pageHeaders_[RingIndex(page)].frozen;
}

void ScheduledTransactionPool::Publish(
    std::uint64_t cutoffFrame, const PlayableInstanceTable* instances,
    const OccurrenceStatePool* occurrences, TimelineArmTable* armTable) {
    if (!Valid()) return;
    // Private promotion does not release a page to audio. Publish only the
    // contiguous prefix preceding the first still-owned reservation.
    for (std::uint32_t page = 0; page < pageCount_; ++page) {
        const auto ring = RingIndex(page);
        if (pagePublicationHolds_[ring] != 0 || pagePreparedPlan03GroupCount_[ring] != 0) {
            cutoffFrame = std::min(cutoffFrame,
                baseFrame_ + static_cast<std::uint64_t>(page) * quantumFrames_);
            break;
        }
    }
    if (cutoffFrame <= publishCutoffFrame_) {
        return; // monotonic: never unfreeze a published page
    }
    publishCutoffFrame_ = cutoffFrame;
    const std::uint64_t freezeCutoff = std::min(cutoffFrame, WindowEndFrame());
    for (std::uint32_t p = 0; p < pageCount_; ++p) {
        const std::uint64_t pageEnd =
            baseFrame_ + static_cast<std::uint64_t>(p + 1) * quantumFrames_;
        if (pageEnd <= freezeCutoff) {
            const std::uint32_t ring = RingIndex(p);
            if (!pageHeaders_[ring].frozen) {
                pageHeaders_[ring].frozen = true;
                pageHeaders_[ring].publishCutoffFrame = cutoffFrame;
                SortPage(ring);
                const std::uint32_t sliceCount =
                    pageCurrentSliceCount_[ring];
                const std::size_t sliceBase =
                    static_cast<std::size_t>(ring) *
                    currentSliceStorageCapacity_;
                std::uint32_t* sliceOrder =
                    &pageCurrentSliceOrder_[sliceBase];
                for (std::uint32_t i = 0; i < sliceCount; ++i) {
                    sliceOrder[i] = i;
                }
                const CurrentSliceRow* sliceRows =
                    &pageCurrentSlices_[sliceBase];
                std::sort(sliceOrder, sliceOrder + sliceCount,
                          [sliceRows](std::uint32_t a, std::uint32_t b) {
                              const CurrentSliceRow& x = sliceRows[a];
                              const CurrentSliceRow& y = sliceRows[b];
                              if (x.executeFrame != y.executeFrame) {
                                  return x.executeFrame < y.executeFrame;
                              }
                              if (x.logicCommitSequence !=
                                  y.logicCommitSequence) {
                                  return x.logicCommitSequence <
                                         y.logicCommitSequence;
                              }
                              if (x.executionOrdinal != y.executionOrdinal) {
                                  return x.executionOrdinal < y.executionOrdinal;
                              }
                              return x.rowSequence < y.rowSequence;
                          });
                pageHeaders_[ring].plan03GroupCount =
                    pagePlan03GroupCount_[ring];
                const std::size_t base =
                    static_cast<std::size_t>(ring) *
                    config_.maxEventsPerQuantum;
                const std::uint32_t count = pageEventCount_[ring];
                for (std::uint32_t i = 0; i < count; ++i) {
                    EventRow& row = pageEvents_[base + i];
                    TransactionRecord& transaction = transactions_[row.txnSlot];
                    bool valid = transaction.used && transaction.epoch == epoch_;
                    for (std::uint32_t prior = 0;
                         valid && prior < i; ++prior) {
                        const EventRow& priorRow = pageEvents_[base + prior];
                        if (priorRow.txnSlot == row.txnSlot) {
                            valid = priorRow.frozenTxnValid;
                            break;
                        }
                    }
                    if (valid && instances != nullptr) {
                        valid = instances->ValidateStamp(
                            transaction.ownerInstance,
                            transaction.ownerScheduleEpoch);
                    }
                    if (valid && transaction.occurrence.present) {
                        valid = instances != nullptr && occurrences != nullptr &&
                                ValidateTransactionOccurrence(
                                    transaction.occurrence,
                                    transaction.ownerInstance,
                                    transaction.ownerScheduleEpoch,
                                    *instances, *occurrences);
                    }
                    row.frozenTxnValid = valid;
                    const TimelineArmToken arm = row.armToken.Valid()
                        ? row.armToken
                        : transaction.armToken;
                    if (!valid && armTable != nullptr && arm.Valid()) {
                        (void)armTable->RequestCancel(arm);
                    }
                }
                pageEmptyQuantumStamp_[ring].store(
                    count == 0 && sliceCount == 0 && pagePlan03GroupCount_[ring] == 0
                        ? pageHeaders_[ring].absoluteQuantumIndex : kUnpublishedQuantumStamp,
                    std::memory_order_relaxed);
                pageQuantumStamp_[ring].store(
                    pageHeaders_[ring].absoluteQuantumIndex,
                    std::memory_order_release);
            }
        }
    }
}

void ScheduledTransactionPool::SortPage(std::uint32_t ring) {
    if (pageSorted_[ring]) {
        return;
    }
    const std::uint32_t count = pageEventCount_[ring];
    const std::size_t base =
        static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
    std::uint32_t* order = &pageEventOrder_[base];
    for (std::uint32_t i = 0; i < count; ++i) {
        order[i] = i;
    }
    const EventRow* rows = &pageEvents_[base];
    std::sort(order, order + count,
              [rows](std::uint32_t a, std::uint32_t b) {
                  const EventRow& x = rows[a];
                  const EventRow& y = rows[b];
                  if (x.executeFrame != y.executeFrame)
                      return x.executeFrame < y.executeFrame;
                  if (x.logicCommitSequence != y.logicCommitSequence)
                      return x.logicCommitSequence < y.logicCommitSequence;
                  if (x.executionOrdinal != y.executionOrdinal)
                      return x.executionOrdinal < y.executionOrdinal;
                  if (x.eventIndex != y.eventIndex)
                      return x.eventIndex < y.eventIndex;
                  if (x.transactionOrdinal != y.transactionOrdinal)
                      return x.transactionOrdinal < y.transactionOrdinal;
                  if (x.subeventPhase != y.subeventPhase)
                      return x.subeventPhase < y.subeventPhase;
                  return x.rowSequence < y.rowSequence;
              });
    pageSorted_[ring] = 1;
}

ReserveResult ScheduledTransactionPool::Reserve(
    const TransactionRequest& request, PrePublishLatePolicy latePolicy) {
    return Reserve(request, latePolicy, publishCutoffFrame_);
}

ReserveResult ScheduledTransactionPool::Reserve(
    const TransactionRequest& request, PrePublishLatePolicy latePolicy,
    std::uint64_t publicationFrontier) {
    ReserveResult result;
    result.transactionId = request.transactionId;
    publicationFrontier =
        std::max(publicationFrontier, publishCutoffFrame_);

    auto refuse = [&](ReserveRefusal reason) {
        result.status = ReserveStatus::Refused;
        result.refusal = reason;
        refusalCounts_[static_cast<std::size_t>(reason)] += 1;
        return result;
    };

    if (!Valid()) {
        result.status = ReserveStatus::Refused;
        return result;
    }
    if (request.transportEpoch == kInvalidTransportEpoch ||
        request.transportEpoch != epoch_) {
        return refuse(ReserveRefusal::InvalidTransportEpoch);
    }
    if (request.events == nullptr || request.eventCount == 0) {
        return refuse(ReserveRefusal::EmptyTransaction);
    }
    if (request.eventCount > config_.maxTransactionEvents) {
        return refuse(ReserveRefusal::TransactionEventsExceeded);
    }
    bool frontierBound = false;
    for (std::size_t i = 0; i < request.eventCount; ++i) {
        const auto& event = request.events[i];
        if (!event.frontier.Present()) continue;
        frontierBound = true;
        if (!event.frontier.Valid() || event.frontier.transport != request.transportEpoch ||
            (event.frontier.group.Valid() && event.frontier.group.absoluteQuantum !=
                event.executeFrame / config_.quantumFrames)) return refuse(ReserveRefusal::FrontierIdentity);
    }

    // Raw span before any late shift.
    std::uint64_t minFrame = request.events[0].executeFrame;
    std::uint64_t maxFrame = request.events[0].executeFrame;
    for (std::size_t i = 1; i < request.eventCount; ++i) {
        const std::uint64_t f = request.events[i].executeFrame;
        minFrame = std::min(minFrame, f);
        maxFrame = std::max(maxFrame, f);
    }

    // Late handling: the earliest event before the publish frontier takes the
    // caller's policy. A shift preserves the relative timing of the whole
    // transaction, so its span is unchanged.
    std::uint64_t delta = 0;
    if (minFrame < publicationFrontier) {
        if (frontierBound) return refuse(ReserveRefusal::Late);
        switch (latePolicy) {
            case PrePublishLatePolicy::Refuse:
                return refuse(ReserveRefusal::Late);
            case PrePublishLatePolicy::Drop:
                result.status = ReserveStatus::DroppedLate;
                droppedLateCount_ += 1;
                return result;
            case PrePublishLatePolicy::NextEquivalentBoundary:
                return refuse(ReserveRefusal::NeedsBoundaryResolution);
            case PrePublishLatePolicy::ExecuteAtEarliestSafeFrame:
                delta = publicationFrontier - minFrame;
                result.rescheduled = true;
                break;
        }
    }

    const std::uint64_t firstFrame = minFrame + delta;
    const std::uint64_t lastFrame = maxFrame + delta;
    result.firstFrame = firstFrame;
    result.lastFrame = lastFrame;

    if (lastFrame - firstFrame > config_.maxTransactionSpanFrames) {
        return refuse(ReserveRefusal::SpanExceeded);
    }

    // Resolve every event to its page at the final (shifted) frame.
    for (std::size_t i = 0; i < request.eventCount; ++i) {
        const std::uint64_t frame = request.events[i].executeFrame + delta;
        std::uint32_t page = 0;
        if (!PageForFrame(frame, &page)) {
            return refuse(ReserveRefusal::OutsideWindow);
        }
        // A frame that is still before the frontier after policy handling, or in
        // a frozen page, cannot be written: publishing must not move.
        if (frame < publicationFrontier || pageHeaders_[RingIndex(page)].frozen) {
            return refuse(ReserveRefusal::Late);
        }
        scratchEventPage_[i] = page;
        scratchEventFrame_[i] = frame;
    }

    // Ingress rate and pool room.
    if (static_cast<std::uint64_t>(ingressThisQuantum_) +
            preparedIngressThisQuantum_ + unplacedIngressThisQuantum_ >=
        config_.maxIngressTransactionsPerQuantum) {
        return refuse(ReserveRefusal::IngressRateExceeded);
    }
    if (freeTransactions_.size() <= unplacedTransactions_) {
        return refuse(ReserveRefusal::PoolFull);
    }
    if (nextCommitSerial_ == UINT64_MAX) {
        return refuse(ReserveRefusal::CommitSerialExhausted);
    }

    // All-or-nothing capacity pre-check, per distinct touched page. Bounded by
    // the transaction's event count, which is itself bounded by
    // maxTransactionEvents.
    std::uint64_t reservedNewBatches = 0;
    for (std::size_t i = 0; i < request.eventCount; ++i) {
        // Only run the check once per distinct page (at its first occurrence).
        bool firstOccurrence = true;
        for (std::size_t j = 0; j < i; ++j) {
            if (scratchEventPage_[j] == scratchEventPage_[i]) {
                firstOccurrence = false;
                break;
            }
        }
        if (!firstOccurrence) {
            continue;
        }
        const std::uint32_t page = scratchEventPage_[i];
        const std::uint32_t ring = RingIndex(page);
        const std::size_t batchBase =
            static_cast<std::size_t>(ring) * config_.maxFrameBatchesPerQuantum;

        std::uint32_t newEvents = 0;
        std::uint32_t newBatches = 0;
        for (std::size_t k = i; k < request.eventCount; ++k) {
            if (scratchEventPage_[k] != page) {
                continue;
            }
            ++newEvents;
            // A frame is a new batch when it is neither already filed in the page
            // nor already counted earlier in this same transaction.
            const std::uint64_t frame = scratchEventFrame_[k];
            bool existing = false;
            for (std::uint32_t b = 0;
                 b < pageBatchOccupiedCount_[ring]; ++b) {
                if (pageBatchRows_[batchBase + b].frame == frame) {
                    existing = true;
                    break;
                }
            }
            if (!existing) {
                bool countedEarlier = false;
                for (std::size_t k2 = i; k2 < k; ++k2) {
                    if (scratchEventPage_[k2] == page &&
                        scratchEventFrame_[k2] == frame) {
                        countedEarlier = true;
                        break;
                    }
                }
                if (!countedEarlier) {
                    ++newBatches;
                }
            }
        }
        const std::uint32_t occupiedEvents =
            pageEventCount_[ring] + pagePreparedEventCount_[ring];
        if (occupiedEvents > config_.maxEventsPerQuantum ||
            newEvents > config_.maxEventsPerQuantum - occupiedEvents) {
            return refuse(ReserveRefusal::PageEventCapacity);
        }
        if (pageBatchOccupiedCount_[ring] >
                config_.maxFrameBatchesPerQuantum ||
            newBatches > config_.maxFrameBatchesPerQuantum -
                             pageBatchOccupiedCount_[ring]) {
            return refuse(ReserveRefusal::PageBatchCapacity);
        }
        reservedNewBatches += newBatches;
    }
    const auto globalBudget = CheckUnplacedPageBudget(
        request.eventCount, reservedNewBatches);
    if (globalBudget != ReserveRefusal::None) return refuse(globalBudget);

    // Every check passed: commit. From here nothing can fail, so there is no
    // half-written state to roll back.
    const std::uint32_t slot = freeTransactions_.back();
    freeTransactions_.pop_back();
    TransactionRecord& rec = transactions_[slot];
    rec = TransactionRecord{};
    rec.used = true;
    rec.id = request.transactionId;
    rec.commitSerial = nextCommitSerial_++;
    rec.epoch = request.transportEpoch;
    rec.ownerInstance = request.ownerInstanceHandle;
    rec.ownerScheduleEpoch = request.ownerInstanceScheduleEpoch;
    rec.occurrence = request.occurrence;
    rec.program = request.program;
    rec.armToken = request.armToken;
    rec.immediateEntry = request.immediateEntry;
    rec.firstFrame = firstFrame;
    rec.lastFrame = lastFrame;
    rec.remainingEvents = static_cast<std::uint32_t>(request.eventCount);

    std::uint32_t distinctPages = 0;
    std::uint32_t totalNewBatches = 0;
    for (std::size_t i = 0; i < request.eventCount; ++i) {
        const std::uint32_t page = scratchEventPage_[i];
        const std::uint32_t ring = RingIndex(page);
        const std::uint64_t frame = scratchEventFrame_[i];
        const ScheduledEventInput& in = request.events[i];

        const std::size_t eventBase =
            static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        const std::uint32_t liveEventCount = pageEventCount_[ring];
        if (pagePreparedEventCount_[ring] != 0) {
            pageEvents_[eventBase + liveEventCount +
                        pagePreparedEventCount_[ring]] =
                pageEvents_[eventBase + liveEventCount];
        }
        EventRow& row = pageEvents_[eventBase + liveEventCount];
        row.executeFrame = frame;
        row.logicCommitSequence = in.logicCommitSequence;
        row.executionOrdinal = in.executionOrdinal;
        row.eventIndex = in.eventIndex;
        row.transactionOrdinal = in.transactionOrdinal;
        row.subeventPhase = in.subeventPhase;
        row.payload = in.payload;
        row.authoredFadeOutCandidate = in.authoredFadeOutCandidate;
        row.armToken = in.armToken;
        row.frontier = in.frontier;
        row.transactionId = request.transactionId;
        row.txnSlot = slot;
        row.rowSequence = nextRowSequence_++;
        ++pageEventCount_[ring];
        pageSorted_[ring] = 0;
        eventHighWater_ = std::max(eventHighWater_, pageEventCount_[ring]);

        // Register the frame as a batch if new to the page.
        const std::size_t batchBase =
            static_cast<std::size_t>(ring) * config_.maxFrameBatchesPerQuantum;
        bool existing = false;
        for (std::uint32_t b = 0;
             b < pageBatchOccupiedCount_[ring]; ++b) {
            FrameBatchRow& batch = pageBatchRows_[batchBase + b];
            if (batch.frame == frame) {
                existing = true;
                if (batch.liveReferences == 0) {
                    ++pageBatchCount_[ring];
                    ++totalNewBatches;
                    frameBatchHighWater_ =
                        std::max(frameBatchHighWater_,
                                 pageBatchCount_[ring]);
                }
                ++batch.liveReferences;
                break;
            }
        }
        if (!existing) {
            FrameBatchRow& batch = pageBatchRows_[
                batchBase + pageBatchOccupiedCount_[ring]];
            batch = {frame, 1, 0};
            ++pageBatchOccupiedCount_[ring];
            ++pageBatchCount_[ring];
            ++totalNewBatches;
            frameBatchHighWater_ =
                std::max(frameBatchHighWater_, pageBatchCount_[ring]);
        }

        // First event landing on a page counts the page for this transaction and
        // bumps that page's committed-transaction tally once.
        bool pageSeenEarlier = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (scratchEventPage_[j] == page) {
                pageSeenEarlier = true;
                break;
            }
        }
        if (!pageSeenEarlier) {
            ++distinctPages;
            ++pageHeaders_[ring].committedTransactionCount;
        }
    }

    rec.summary.frameBatchCount = totalNewBatches;
    rec.summary.eventCount = static_cast<std::uint32_t>(request.eventCount);
    rec.summary.pageSpan = distinctPages;

    ++liveTransactionCount_;
    transactionHighWater_ =
        std::max(transactionHighWater_, liveTransactionCount_);
    transactionEventsHighWater_ = std::max(
        transactionEventsHighWater_,
        static_cast<std::uint32_t>(request.eventCount));
    transactionSpanHighWater_ =
        std::max(transactionSpanHighWater_, lastFrame - firstFrame);
    ++ingressThisQuantum_;
    ingressHighWater_ = std::max(ingressHighWater_, ingressThisQuantum_);

    result.status = ReserveStatus::Committed;
    result.refusal = ReserveRefusal::None;
    if (request.eventCount == 1) {
        const std::uint32_t ring = RingIndex(scratchEventPage_[0]);
        result.committed = {request.transportEpoch, slot, rec.commitSerial,
            scratchEventFrame_[0] / quantumFrames_,
            pageHeaders_[ring].pageGeneration};
    }
    return result;
}

OrdinaryStartCancelResult
ScheduledTransactionPool::CancelUnpublishedOrdinaryStartExact(
    CommittedTransactionRef ref,
    const OrdinaryStartTransactionIdentity& expected) noexcept {
    if (!ref.Valid() || !Valid() || ref.epoch != epoch_ ||
        ref.slot >= transactions_.size() ||
        expected.startFrame / quantumFrames_ != ref.absoluteQuantum)
        return OrdinaryStartCancelResult::Stale;

    const TransactionRecord& record = transactions_[ref.slot];
    if (!record.used || record.commitSerial != ref.commitSerial ||
        record.epoch != ref.epoch)
        return OrdinaryStartCancelResult::Stale;
    if (record.prepared || record.preparedClaimSlot != UINT32_MAX ||
        record.id != expected.transactionId ||
        record.ownerInstance != expected.ownerInstance ||
        record.ownerScheduleEpoch != expected.ownerScheduleEpoch ||
        record.program.program != expected.program.program ||
        record.program.programGeneration != expected.program.programGeneration ||
        record.armToken.slot != expected.arm.slot ||
        record.armToken.generation != expected.arm.generation ||
        record.armToken.renderSlot != expected.arm.renderSlot ||
        record.armToken.renderGeneration != expected.arm.renderGeneration ||
        record.firstFrame != expected.startFrame ||
        record.lastFrame != expected.startFrame)
        return OrdinaryStartCancelResult::ProtocolViolation;
    if (record.remainingEvents != 1 || record.summary.eventCount != 1 ||
        record.summary.pageSpan != 1 || record.occurrence.present ||
        record.immediateEntry.Valid() || expected.program.program == nullptr ||
        expected.program.programGeneration == 0)
        return OrdinaryStartCancelResult::NotEligible;

    std::uint32_t page = 0;
    if (!PageForFrame(expected.startFrame, &page))
        return OrdinaryStartCancelResult::Stale;
    const std::uint32_t ring = RingIndex(page);
    const HorizonPageHeader& header = pageHeaders_[ring];
    if (header.transportEpoch != ref.epoch ||
        header.absoluteQuantumIndex != ref.absoluteQuantum ||
        header.pageGeneration != ref.pageGeneration)
        return OrdinaryStartCancelResult::Stale;
    if (header.frozen ||
        pageQuantumStamp_[ring].load(std::memory_order_acquire) !=
            kUnpublishedQuantumStamp ||
        pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) !=
            kUnpublishedQuantumStamp ||
        (consumerFrontierEnabled_ &&
         consumedFrontierQuantum_->load(std::memory_order_acquire) > ref.absoluteQuantum) ||
        pagePreparedPlan03GroupCount_[ring] != 0 ||
        pageCurrentSliceCount_[ring] != 0)
        return OrdinaryStartCancelResult::NotEligible;
    if (expected.program.program->generation != expected.program.programGeneration ||
        expected.program.program->role != TimelineProgramRole::Voice ||
        expected.program.program->payloads == nullptr ||
        expected.program.program->payloadCount != 1 ||
        !expected.arm.Valid())
        return OrdinaryStartCancelResult::ProtocolViolation;

    const std::size_t eventBase =
        static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
    const std::uint32_t live = pageEventCount_[ring];
    const std::uint32_t prepared = pagePreparedEventCount_[ring];
    if (live == 0 || live > config_.maxEventsPerQuantum ||
        prepared > config_.maxEventsPerQuantum - live ||
        header.committedTransactionCount == 0 || liveTransactionCount_ == 0 ||
        freeTransactions_.size() >= freeTransactions_.capacity())
        return OrdinaryStartCancelResult::ProtocolViolation;
    const auto sameArm = [&](TimelineArmToken arm) noexcept {
        return arm.slot == expected.arm.slot &&
            arm.generation == expected.arm.generation &&
            arm.renderSlot == expected.arm.renderSlot &&
            arm.renderGeneration == expected.arm.renderGeneration;
    };
    if (pagePlan03GroupCount_[ring] != 0) {
        if (expected.serviceSlot == UINT32_MAX ||
            expected.serviceGeneration == 0)
            return OrdinaryStartCancelResult::NotEligible;
        // Live Frontier groups may share this unpublished Page and frame batch,
        // but none may own or read the ordinary Start being removed.
        const std::size_t groupBase = static_cast<std::size_t>(ring) *
            config_.maxPlan03GroupHeadersPerPage;
        std::uint32_t liveGroups = 0;
        std::uint32_t groupHolds = 0;
        for (std::uint32_t index = 0;
             index < config_.maxPlan03GroupHeadersPerPage; ++index) {
            const std::size_t rowIndex = groupBase + index;
            const auto state = pagePlan03GroupRowStates_[rowIndex];
            if (state == Plan03GroupRowState::Prepared)
                return OrdinaryStartCancelResult::NotEligible;
            if (state != Plan03GroupRowState::Live) continue;
            ++liveGroups;
            const auto& group = pagePlan03Groups_[rowIndex];
            const Plan03GroupSliceIdentity identity{
                ref.absoluteQuantum, ref.pageGeneration, group.decision,
                group.sliceIndex, rowIndex,
                pagePlan03GroupClaimGenerations_[rowIndex], ref.epoch};
            if (!group.frontier.Valid())
                return OrdinaryStartCancelResult::NotEligible;
            if (!identity.Exact() ||
                !(group.frontier.operation.group == identity) ||
                group.memberCount > config_.maxArmsPerGroup)
                return OrdinaryStartCancelResult::ProtocolViolation;
            const auto& operation = group.frontier.operation;
            const auto& entryOwner = group.frontier.entry.owner;
            const auto& owner = operation.owner;
            if (owner.child == expected.ownerInstance ||
                owner.parent == expected.ownerInstance ||
                entryOwner.child == expected.ownerInstance ||
                entryOwner.parent == expected.ownerInstance ||
                (owner.serviceSlot == expected.serviceSlot &&
                 owner.serviceGeneration == expected.serviceGeneration) ||
                (entryOwner.serviceSlot == expected.serviceSlot &&
                 entryOwner.serviceGeneration == expected.serviceGeneration) ||
                operation.transactionSlot == ref.slot ||
                (operation.program == expected.program.program &&
                 operation.programGeneration == expected.program.programGeneration) ||
                sameArm({operation.armSlot, operation.armGeneration,
                         operation.renderSlot, operation.renderGeneration}))
                return OrdinaryStartCancelResult::NotEligible;
            const std::size_t memberBase = rowIndex * config_.maxArmsPerGroup;
            for (std::uint16_t member = 0; member < group.memberCount; ++member)
                if (sameArm(pagePlan03GroupMembers_[memberBase + member]))
                    return OrdinaryStartCancelResult::NotEligible;
            if (group.publicationHeld) ++groupHolds;
        }
        if (liveGroups != pagePlan03GroupCount_[ring] ||
            groupHolds > pagePublicationHolds_[ring])
            return OrdinaryStartCancelResult::ProtocolViolation;
    }
    for (std::size_t slot = 0; slot < transactions_.size(); ++slot) {
        if (slot == ref.slot) continue;
        const auto& other = transactions_[slot];
        if ((!other.used && !other.prepared) || other.remainingEvents == 0)
            continue;
        if ((other.program.program == expected.program.program &&
             other.program.programGeneration ==
                 expected.program.programGeneration) ||
            sameArm(other.armToken))
            return OrdinaryStartCancelResult::NotEligible;
    }
    std::uint32_t found = UINT32_MAX;
    for (std::uint32_t scanRing = 0; scanRing < pageCount_; ++scanRing) {
        const std::uint32_t scanLive = pageEventCount_[scanRing];
        const std::uint32_t scanPrepared = pagePreparedEventCount_[scanRing];
        if (scanLive > config_.maxEventsPerQuantum ||
            scanPrepared > config_.maxEventsPerQuantum - scanLive)
            return OrdinaryStartCancelResult::ProtocolViolation;
        const std::size_t scanBase =
            static_cast<std::size_t>(scanRing) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0; index < scanLive + scanPrepared; ++index) {
            const EventRow& event = pageEvents_[scanBase + index];
            if (event.txnSlot != ref.slot && sameArm(event.armToken))
                return OrdinaryStartCancelResult::NotEligible;
            if (event.txnSlot != ref.slot) continue;
            if (scanRing != ring || index >= live || found != UINT32_MAX)
                return OrdinaryStartCancelResult::ProtocolViolation;
            found = index;
        }
    }
    if (found == UINT32_MAX)
        return OrdinaryStartCancelResult::ProtocolViolation;
    const EventRow& event = pageEvents_[eventBase + found];
    if (event.transactionId != expected.transactionId ||
        event.executeFrame != expected.startFrame ||
        event.armToken.slot != expected.arm.slot ||
        event.armToken.generation != expected.arm.generation ||
        event.armToken.renderSlot != expected.arm.renderSlot ||
        event.armToken.renderGeneration != expected.arm.renderGeneration ||
        event.frontier.Present() || event.payload >= expected.program.program->payloadCount ||
        event.payload != 0 || event.rowSequence == 0 ||
        expected.program.program->payloads[event.payload].kind != TimelineEventKind::Start)
        return OrdinaryStartCancelResult::ProtocolViolation;

    const std::size_t batchBase =
        static_cast<std::size_t>(ring) * config_.maxFrameBatchesPerQuantum;
    const std::uint32_t occupied = pageBatchOccupiedCount_[ring];
    if (occupied == 0 || occupied > config_.maxFrameBatchesPerQuantum ||
        pageBatchCount_[ring] == 0 || pageBatchCount_[ring] > occupied)
        return OrdinaryStartCancelResult::ProtocolViolation;
    std::uint32_t batchIndex = UINT32_MAX;
    std::uint32_t liveBatches = 0;
    for (std::uint32_t index = 0; index < occupied; ++index) {
        const FrameBatchRow& batch = pageBatchRows_[batchBase + index];
        if (batch.liveReferences == 0 && batch.preparedReferences == 0)
            return OrdinaryStartCancelResult::ProtocolViolation;
        if (batch.liveReferences != 0) ++liveBatches;
        if (batch.frame == expected.startFrame) {
            if (batchIndex != UINT32_MAX || batch.liveReferences == 0)
                return OrdinaryStartCancelResult::ProtocolViolation;
            batchIndex = index;
        }
    }
    if (batchIndex == UINT32_MAX || liveBatches != pageBatchCount_[ring])
        return OrdinaryStartCancelResult::ProtocolViolation;
    FrameBatchRow& batch = pageBatchRows_[batchBase + batchIndex];
    std::uint32_t actualLive = 0;
    std::uint32_t actualPrepared = 0;
    for (std::uint32_t index = 0; index < live + prepared; ++index) {
        if (pageEvents_[eventBase + index].executeFrame != expected.startFrame)
            continue;
        if (index < live) ++actualLive;
        else ++actualPrepared;
    }
    if (batch.liveReferences != actualLive ||
        batch.preparedReferences != actualPrepared)
        return OrdinaryStartCancelResult::ProtocolViolation;

    std::uint32_t distinctLiveTransactions = 0;
    for (std::uint32_t index = 0; index < live; ++index) {
        const auto slot = pageEvents_[eventBase + index].txnSlot;
        if (slot >= transactions_.size() || !transactions_[slot].used)
            return OrdinaryStartCancelResult::ProtocolViolation;
        bool seen = false;
        for (std::uint32_t prior = 0; prior < index; ++prior)
            seen = seen || pageEvents_[eventBase + prior].txnSlot == slot;
        if (!seen) ++distinctLiveTransactions;
    }
    if (distinctLiveTransactions != header.committedTransactionCount)
        return OrdinaryStartCancelResult::ProtocolViolation;

    // Verify every occupied batch against the page before changing packed rows.
    // A neighboring ordinary transaction can occupy another frame without a
    // Frontier group on this page.
    {
        std::uint32_t countedLive = 0;
        std::uint32_t countedPrepared = 0;
        for (std::uint32_t index = 0; index < occupied; ++index) {
            const FrameBatchRow& candidate = pageBatchRows_[batchBase + index];
            std::uint32_t candidateLive = 0;
            std::uint32_t candidatePrepared = 0;
            for (std::uint32_t eventIndex = 0; eventIndex < live + prepared;
                 ++eventIndex) {
                if (pageEvents_[eventBase + eventIndex].executeFrame !=
                    candidate.frame) continue;
                if (eventIndex < live) ++candidateLive;
                else ++candidatePrepared;
            }
            if (candidate.liveReferences != candidateLive ||
                candidate.preparedReferences != candidatePrepared)
                return OrdinaryStartCancelResult::ProtocolViolation;
            for (std::uint32_t prior = 0; prior < index; ++prior)
                if (pageBatchRows_[batchBase + prior].frame == candidate.frame)
                    return OrdinaryStartCancelResult::ProtocolViolation;
            countedLive += candidateLive;
            countedPrepared += candidatePrepared;
        }
        if (countedLive != live || countedPrepared != prepared)
            return OrdinaryStartCancelResult::ProtocolViolation;
    }

    // All checks precede the single-writer commit. The PageStamp is still absent.
    const std::uint32_t last = live + prepared - 1;
    pageEvents_[eventBase + found] = pageEvents_[eventBase + live - 1];
    if (prepared != 0)
        pageEvents_[eventBase + live - 1] = pageEvents_[eventBase + last];
    pageEvents_[eventBase + last] = {};
    --pageEventCount_[ring];
    pageSorted_[ring] = 0;
    if (--batch.liveReferences == 0) --pageBatchCount_[ring];
    if (batch.liveReferences == 0 && batch.preparedReferences == 0) {
        const std::uint32_t lastBatch = --pageBatchOccupiedCount_[ring];
        batch = pageBatchRows_[batchBase + lastBatch];
        pageBatchRows_[batchBase + lastBatch] = {};
    }
    --pageHeaders_[ring].committedTransactionCount;
    --liveTransactionCount_;
    transactions_[ref.slot] = {};
    freeTransactions_.push_back(ref.slot);
    return OrdinaryStartCancelResult::Removed;
}

bool ScheduledTransactionPool::CaptureUnpublishedFrontierStartRowSequence(
    const TransactionRequest& expected,
    const FrontierEntryReceiptRef& entry,
    const Plan03GroupSliceIdentity& member,
    std::uint64_t* outRowSequence) const noexcept {
    if (outRowSequence == nullptr) return false;
    *outRowSequence = 0;
    if (!Valid() || expected.events == nullptr || expected.eventCount != 1 ||
        expected.transportEpoch != epoch_ || !entry.Valid() ||
        !member.Exact() || member.transportEpoch != epoch_ ||
        expected.program.program == nullptr ||
        expected.program.programGeneration == 0)
        return false;
    const ScheduledEventInput& input = expected.events[0];
    std::uint32_t page = 0;
    if (!PageForFrame(input.executeFrame, &page)) return false;
    const std::uint32_t ring = RingIndex(page);
    const HorizonPageHeader& header = pageHeaders_[ring];
    if (header.transportEpoch != epoch_ ||
        header.absoluteQuantumIndex != member.absoluteQuantum ||
        header.pageGeneration != member.pageGeneration || header.frozen ||
        pageQuantumStamp_[ring].load(std::memory_order_acquire) !=
            kUnpublishedQuantumStamp ||
        pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) !=
            kUnpublishedQuantumStamp)
        return false;
    const std::uint32_t live = pageEventCount_[ring];
    if (live > config_.maxEventsPerQuantum) return false;
    const std::size_t base = static_cast<std::size_t>(ring) *
        config_.maxEventsPerQuantum;
    for (std::uint32_t index = 0; index < live; ++index) {
        const EventRow& event = pageEvents_[base + index];
        if (event.transactionId != expected.transactionId ||
            event.executeFrame != input.executeFrame ||
            event.logicCommitSequence != input.logicCommitSequence ||
            event.executionOrdinal != input.executionOrdinal ||
            event.eventIndex != input.eventIndex ||
            event.transactionOrdinal != input.transactionOrdinal ||
            event.subeventPhase != input.subeventPhase ||
            event.payload != input.payload ||
            event.authoredFadeOutCandidate != input.authoredFadeOutCandidate ||
            event.armToken.slot != input.armToken.slot ||
            event.armToken.generation != input.armToken.generation ||
            event.armToken.renderSlot != input.armToken.renderSlot ||
            event.armToken.renderGeneration != input.armToken.renderGeneration ||
            event.frontier.kind != FrontierOperationKind::Entry ||
            !(event.frontier.entry == entry) ||
            !(event.frontier.owner == entry.owner) ||
            !(event.frontier.group == member) ||
            event.frontier.transport != epoch_ || event.rowSequence == 0 ||
            event.txnSlot >= transactions_.size())
            continue;
        const TransactionRecord& record = transactions_[event.txnSlot];
        if (!record.used || record.prepared || record.epoch != epoch_ ||
            record.id != expected.transactionId ||
            record.ownerInstance != expected.ownerInstanceHandle ||
            record.ownerScheduleEpoch != expected.ownerInstanceScheduleEpoch ||
            record.program.program != expected.program.program ||
            record.program.programGeneration !=
                expected.program.programGeneration)
            continue;
        if (*outRowSequence != 0) {
            *outRowSequence = 0;
            return false;
        }
        *outRowSequence = event.rowSequence;
    }
    return *outRowSequence != 0;
}

CancelledFrontierStartResult
ScheduledTransactionPool::CancelUnpublishedFrontierStartExact(
    const TimelineTransactionProgramRef* relatedPrograms,
    std::size_t relatedProgramCount,
    const TransactionRequest& expected,
    const FrontierEntryReceiptRef& entry,
    const Plan03GroupSliceIdentity& member,
    std::uint64_t expectedRowSequence,
    const PlayableInstanceTable& instances) noexcept {
    using Result = CancelledFrontierStartResult;
    if (!Valid() || relatedPrograms == nullptr || relatedProgramCount == 0 ||
        expected.events == nullptr || expected.eventCount != 1 ||
        expected.transportEpoch != epoch_ || expected.transactionId == 0 ||
        expected.program.program == nullptr || expected.program.programGeneration == 0 ||
        !entry.Valid() || !member.Exact() || expectedRowSequence == 0 ||
        expected.immediateEntry.Valid() || expected.occurrence.present)
        return Result::NotEligible;
    const ScheduledEventInput& input = expected.events[0];
    if (member.transportEpoch != epoch_ ||
        input.armToken.slot != expected.armToken.slot ||
        input.armToken.generation != expected.armToken.generation ||
        input.armToken.renderSlot != expected.armToken.renderSlot ||
        input.armToken.renderGeneration != expected.armToken.renderGeneration ||
        expected.program.program->generation != expected.program.programGeneration ||
        expected.program.program->role != TimelineProgramRole::Voice ||
        expected.program.program->payloads == nullptr ||
        input.payload >= expected.program.program->payloadCount ||
        expected.program.program->payloads[input.payload].kind != TimelineEventKind::Start)
        return Result::NotEligible;

    const auto related = [&](TimelineTransactionProgramRef program) noexcept {
        for (std::size_t index = 0; index < relatedProgramCount; ++index) {
            if (relatedPrograms[index].program == program.program &&
                relatedPrograms[index].programGeneration == program.programGeneration)
                return true;
        }
        return false;
    };
    if (!related(expected.program)) return Result::NotEligible;

    std::uint32_t slot = UINT32_MAX;
    for (std::uint32_t index = 0; index < transactions_.size(); ++index) {
        const TransactionRecord& record = transactions_[index];
        if ((record.used || record.prepared) && related(record.program)) {
            if (slot != UINT32_MAX) return Result::NotEligible;
            slot = index;
        }
    }
    if (slot == UINT32_MAX) return Result::Stale;
    const TransactionRecord& record = transactions_[slot];
    if (!record.used || record.prepared ||
        record.preparedClaimSlot != UINT32_MAX ||
        record.remainingEvents != 1 || record.summary.eventCount != 1 ||
        record.summary.pageSpan != 1 || record.occurrence.present ||
        record.immediateEntry.Valid() ||
        record.epoch != epoch_ || record.id != expected.transactionId ||
        record.ownerInstance != expected.ownerInstanceHandle ||
        record.ownerScheduleEpoch != expected.ownerInstanceScheduleEpoch ||
        record.program.program != expected.program.program ||
        record.program.programGeneration != expected.program.programGeneration ||
        record.armToken.slot != expected.armToken.slot ||
        record.armToken.generation != expected.armToken.generation ||
        record.armToken.renderSlot != expected.armToken.renderSlot ||
        record.armToken.renderGeneration != expected.armToken.renderGeneration ||
        record.firstFrame != input.executeFrame ||
        record.lastFrame != input.executeFrame ||
        instances.ValidateStamp(record.ownerInstance, record.ownerScheduleEpoch))
        return Result::NotEligible;

    std::uint32_t page = 0;
    if (!PageForFrame(input.executeFrame, &page)) return Result::Stale;
    const std::uint32_t ring = RingIndex(page);
    const HorizonPageHeader& header = pageHeaders_[ring];
    const std::uint64_t quantum = input.executeFrame / quantumFrames_;
    if (header.transportEpoch != epoch_ ||
        header.absoluteQuantumIndex != quantum || header.pageGeneration == 0)
        return Result::Stale;
    if (header.frozen ||
        pageQuantumStamp_[ring].load(std::memory_order_acquire) != kUnpublishedQuantumStamp ||
        pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != kUnpublishedQuantumStamp ||
        (consumerFrontierEnabled_ &&
         consumedFrontierQuantum_->load(std::memory_order_acquire) > quantum))
        return Result::NotEligible;

    const std::uint32_t live = pageEventCount_[ring];
    const std::uint32_t prepared = pagePreparedEventCount_[ring];
    if (live == 0 || live > config_.maxEventsPerQuantum ||
        prepared > config_.maxEventsPerQuantum - live ||
        header.committedTransactionCount == 0 || liveTransactionCount_ == 0 ||
        freeTransactions_.size() >= freeTransactions_.capacity())
        return Result::ProtocolViolation;
    std::uint32_t found = UINT32_MAX;
    for (std::uint32_t scanRing = 0; scanRing < pageCount_; ++scanRing) {
        const std::uint32_t scanLive = pageEventCount_[scanRing];
        const std::uint32_t scanPrepared = pagePreparedEventCount_[scanRing];
        if (scanLive > config_.maxEventsPerQuantum ||
            scanPrepared > config_.maxEventsPerQuantum - scanLive)
            return Result::ProtocolViolation;
        const std::size_t scanBase = static_cast<std::size_t>(scanRing) *
            config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0; index < scanLive + scanPrepared; ++index) {
            if (pageEvents_[scanBase + index].txnSlot != slot) continue;
            if (scanRing != ring || index >= live || found != UINT32_MAX)
                return Result::ProtocolViolation;
            found = index;
        }
    }
    if (found == UINT32_MAX) return Result::ProtocolViolation;
    const std::size_t eventBase = static_cast<std::size_t>(ring) *
        config_.maxEventsPerQuantum;
    const EventRow& event = pageEvents_[eventBase + found];
    if (event.rowSequence != expectedRowSequence ||
        event.transactionId != expected.transactionId ||
        event.executeFrame != input.executeFrame ||
        event.logicCommitSequence != input.logicCommitSequence ||
        event.executionOrdinal != input.executionOrdinal ||
        event.eventIndex != input.eventIndex ||
        event.transactionOrdinal != input.transactionOrdinal ||
        event.subeventPhase != input.subeventPhase ||
        event.payload != input.payload ||
        event.authoredFadeOutCandidate != input.authoredFadeOutCandidate ||
        event.armToken.slot != input.armToken.slot ||
        event.armToken.generation != input.armToken.generation ||
        event.armToken.renderSlot != input.armToken.renderSlot ||
        event.armToken.renderGeneration != input.armToken.renderGeneration ||
        !event.frontier.Valid() ||
        event.frontier.kind != FrontierOperationKind::Entry ||
        !(event.frontier.entry == entry) ||
        !(event.frontier.owner == entry.owner) ||
        !(event.frontier.group == member) ||
        event.frontier.transport != epoch_)
        return Result::ProtocolViolation;

    const std::size_t batchBase = static_cast<std::size_t>(ring) *
        config_.maxFrameBatchesPerQuantum;
    const std::uint32_t occupied = pageBatchOccupiedCount_[ring];
    if (occupied == 0 || occupied > config_.maxFrameBatchesPerQuantum ||
        pageBatchCount_[ring] == 0 || pageBatchCount_[ring] > occupied)
        return Result::ProtocolViolation;
    std::uint32_t batchIndex = UINT32_MAX;
    std::uint32_t liveBatches = 0;
    for (std::uint32_t index = 0; index < occupied; ++index) {
        const FrameBatchRow& batch = pageBatchRows_[batchBase + index];
        if (batch.liveReferences != 0) ++liveBatches;
        if (batch.frame == input.executeFrame) {
            if (batchIndex != UINT32_MAX || batch.liveReferences == 0)
                return Result::ProtocolViolation;
            batchIndex = index;
        }
    }
    if (batchIndex == UINT32_MAX || liveBatches != pageBatchCount_[ring])
        return Result::ProtocolViolation;
    const FrameBatchRow& batch = pageBatchRows_[batchBase + batchIndex];
    std::uint32_t actualLive = 0;
    std::uint32_t actualPrepared = 0;
    for (std::uint32_t index = 0; index < live + prepared; ++index) {
        if (pageEvents_[eventBase + index].executeFrame != input.executeFrame)
            continue;
        if (index < live) ++actualLive;
        else ++actualPrepared;
    }
    if (batch.liveReferences != actualLive ||
        batch.preparedReferences != actualPrepared)
        return Result::ProtocolViolation;

    // All identity and accounting checks precede the single-writer mutation.
    const std::uint32_t last = live + prepared - 1;
    pageEvents_[eventBase + found] = pageEvents_[eventBase + live - 1];
    if (prepared != 0)
        pageEvents_[eventBase + live - 1] = pageEvents_[eventBase + last];
    pageEvents_[eventBase + last] = {};
    --pageEventCount_[ring];
    pageSorted_[ring] = 0;
    FrameBatchRow& changedBatch = pageBatchRows_[batchBase + batchIndex];
    if (--changedBatch.liveReferences == 0) --pageBatchCount_[ring];
    if (changedBatch.liveReferences == 0 && changedBatch.preparedReferences == 0) {
        const std::uint32_t lastBatch = --pageBatchOccupiedCount_[ring];
        changedBatch = pageBatchRows_[batchBase + lastBatch];
        pageBatchRows_[batchBase + lastBatch] = {};
    }
    --pageHeaders_[ring].committedTransactionCount;
    --liveTransactionCount_;
    transactions_[slot] = {};
    freeTransactions_.push_back(slot);
    return Result::Removed;
}

CancelledFrontierStartResult
ScheduledTransactionPool::CancelUnpublishedFrontierEntryExact(
    const UnpublishedFrontierEntryExpected& expected,
    const PlayableInstanceTable& instances) noexcept {
    using Result = CancelledFrontierStartResult;
    const auto& descriptor = expected.descriptor;
    if (!Valid() || !descriptor.Valid() || descriptor.expectedMemberCount != 1 ||
        descriptor.expectedEntryEventCount != expected.events.size() ||
        !expected.member.Exact() || expected.member.transportEpoch != epoch_ ||
        expected.member.absoluteQuantum != descriptor.primary.absoluteQuantum ||
        expected.member.pageGeneration != descriptor.primary.pageGeneration ||
        expected.member.decision.slot == descriptor.entry.coordinator.slot ||
        expected.executeFrame != descriptor.order.executeFrame ||
        expected.executeFrame / quantumFrames_ != expected.member.absoluteQuantum)
        return Result::NotEligible;
    const auto sameArm = [](const TimelineArmToken& a,
                            const TimelineArmToken& b) noexcept {
        return a.slot == b.slot && a.generation == b.generation &&
            a.renderSlot == b.renderSlot &&
            a.renderGeneration == b.renderGeneration;
    };
    for (std::size_t i = 0; i < expected.events.size(); ++i) {
        const auto& item = expected.events[i];
        if (item.program.program == nullptr || item.program.programGeneration == 0 ||
            item.program.program->generation != item.program.programGeneration ||
            item.program.program->payloads == nullptr ||
            item.program.program->payloadCount != 1 ||
            item.ownerInstance == 0 || item.ownerScheduleEpoch == 0 ||
            item.frontierOwnerInstance == 0 || item.frontierOwnerEpoch == 0 ||
            item.serviceSlot == UINT32_MAX || item.serviceGeneration == 0 ||
            instances.ValidateStamp(item.ownerInstance, item.ownerScheduleEpoch) ||
            !(item.group == (i == 0 ? descriptor.primary : expected.member)))
            return Result::NotEligible;
        const auto role = i == 2 ? TimelineProgramRole::Voice :
            TimelineProgramRole::ControllerActivation;
        const auto kind = i == 2 ? TimelineEventKind::Start :
            TimelineEventKind::ControllerActivation;
        if (item.program.program->role != role ||
            item.program.program->payloads[0].kind != kind ||
            (i != 2 && item.arm.Valid()) || (i == 2 && !item.arm.Valid()))
            return Result::NotEligible;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (expected.events[prior].program.program == item.program.program &&
                expected.events[prior].program.programGeneration ==
                    item.program.programGeneration)
                return Result::NotEligible;
    }

    std::uint32_t page = 0;
    if (!PageForFrame(expected.executeFrame, &page)) return Result::Stale;
    const std::uint32_t ring = RingIndex(page);
    const auto& header = pageHeaders_[ring];
    const auto quantum = expected.executeFrame / quantumFrames_;
    if (header.transportEpoch != epoch_ ||
        header.absoluteQuantumIndex != quantum ||
        header.pageGeneration != descriptor.primary.pageGeneration)
        return Result::Stale;
    if (header.frozen ||
        pageQuantumStamp_[ring].load(std::memory_order_acquire) != kUnpublishedQuantumStamp ||
        pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != kUnpublishedQuantumStamp ||
        (consumerFrontierEnabled_ &&
         consumedFrontierQuantum_->load(std::memory_order_acquire) > quantum))
        return Result::NotEligible;

    std::array<std::uint32_t, 3> slots{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (std::uint32_t slot = 0; slot < transactions_.size(); ++slot) {
        const auto& record = transactions_[slot];
        if (!record.used && !record.prepared) continue;
        for (std::size_t i = 0; i < expected.events.size(); ++i) {
            const auto& item = expected.events[i];
            if (record.program.program != item.program.program ||
                record.program.programGeneration != item.program.programGeneration)
                continue;
            // This is the full used/prepared reader set for each exact program.
            if (slots[i] != UINT32_MAX || !record.used || record.prepared ||
                record.preparedClaimSlot != UINT32_MAX ||
                record.remainingEvents != 1 || record.summary.eventCount != 1 ||
                record.summary.pageSpan != 1 || record.commitSerial != 0 ||
                record.occurrence.present || record.immediateEntry.Valid() ||
                record.epoch != epoch_ ||
                record.id != descriptor.order.logicCommitSequence ||
                record.ownerInstance != item.ownerInstance ||
                record.ownerScheduleEpoch != item.ownerScheduleEpoch ||
                record.firstFrame != expected.executeFrame ||
                record.lastFrame != expected.executeFrame ||
                !sameArm(record.armToken, item.arm))
                return Result::NotEligible;
            slots[i] = slot;
        }
    }
    for (auto slot : slots)
        if (slot == UINT32_MAX) return Result::Stale;

    const auto live = pageEventCount_[ring];
    const auto prepared = pagePreparedEventCount_[ring];
    if (live < expected.events.size() || live > config_.maxEventsPerQuantum ||
        prepared > config_.maxEventsPerQuantum - live ||
        header.committedTransactionCount < expected.events.size() ||
        liveTransactionCount_ < expected.events.size() ||
        freeTransactions_.capacity() < expected.events.size() ||
        freeTransactions_.size() > freeTransactions_.capacity() - expected.events.size())
        return Result::ProtocolViolation;
    std::array<std::uint32_t, 3> positions{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::array<std::uint64_t, 3> sequences{};
    std::uint32_t entryRows = 0;
    for (std::uint32_t scanRing = 0; scanRing < pageCount_; ++scanRing) {
        const auto scanLive = pageEventCount_[scanRing];
        const auto scanPrepared = pagePreparedEventCount_[scanRing];
        if (scanLive > config_.maxEventsPerQuantum ||
            scanPrepared > config_.maxEventsPerQuantum - scanLive)
            return Result::ProtocolViolation;
        const auto base = static_cast<std::size_t>(scanRing) *
            config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0; index < scanLive + scanPrepared; ++index) {
            const auto& event = pageEvents_[base + index];
            if (event.frontier.kind == FrontierOperationKind::Entry &&
                event.frontier.entry == descriptor.entry)
                ++entryRows;
            for (std::size_t i = 0; i < slots.size(); ++i) {
                if (event.txnSlot != slots[i]) continue;
                if (scanRing != ring || index >= live ||
                    positions[i] != UINT32_MAX ||
                    event.transactionId != descriptor.order.logicCommitSequence ||
                    event.executeFrame != expected.executeFrame ||
                    event.rowSequence == 0 || event.payload != 0 ||
                    !sameArm(event.armToken, expected.events[i].arm) ||
                    !event.frontier.Valid() ||
                    event.frontier.kind != FrontierOperationKind::Entry ||
                    !(event.frontier.entry == descriptor.entry) ||
                    !(event.frontier.group == expected.events[i].group) ||
                    event.frontier.transport != epoch_ ||
                    event.frontier.world != descriptor.worldGeneration ||
                    event.frontier.operationEventCount != 1 ||
                    event.frontier.entryEventOrdinal != i ||
                    event.frontier.owner.child !=
                        expected.events[i].frontierOwnerInstance ||
                    event.frontier.owner.childEpoch !=
                        expected.events[i].frontierOwnerEpoch ||
                    event.frontier.owner.serviceSlot != expected.events[i].serviceSlot ||
                    event.frontier.owner.serviceGeneration !=
                        expected.events[i].serviceGeneration ||
                    event.frontier.owner.invocation != descriptor.entry.owner.invocation ||
                    event.frontier.owner.invocationEpoch !=
                        descriptor.entry.owner.invocationEpoch)
                    return Result::ProtocolViolation;
                positions[i] = index;
                sequences[i] = event.rowSequence;
                if (i == 0 &&
                    (event.logicCommitSequence != descriptor.order.logicCommitSequence ||
                     event.executionOrdinal != descriptor.order.executionOrdinal ||
                     event.eventIndex != descriptor.order.eventIndex ||
                     event.transactionOrdinal != descriptor.order.transactionOrdinal ||
                     event.subeventPhase != descriptor.order.subeventPhase ||
                     event.rowSequence != descriptor.order.rowSequence ||
                     !(event.frontier.owner == descriptor.entry.owner)))
                    return Result::ProtocolViolation;
            }
        }
    }
    if (entryRows != descriptor.expectedEntryEventCount) return Result::NotEligible;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (positions[i] == UINT32_MAX) return Result::ProtocolViolation;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (sequences[i] == sequences[prior]) return Result::ProtocolViolation;
    }

    const auto batchBase = static_cast<std::size_t>(ring) *
        config_.maxFrameBatchesPerQuantum;
    const auto occupied = pageBatchOccupiedCount_[ring];
    if (occupied == 0 || occupied > config_.maxFrameBatchesPerQuantum ||
        pageBatchCount_[ring] == 0 || pageBatchCount_[ring] > occupied)
        return Result::ProtocolViolation;
    std::uint32_t batchIndex = UINT32_MAX;
    std::uint32_t liveBatches = 0;
    for (std::uint32_t index = 0; index < occupied; ++index) {
        const auto& batch = pageBatchRows_[batchBase + index];
        if (batch.liveReferences != 0) ++liveBatches;
        if (batch.frame == expected.executeFrame) {
            if (batchIndex != UINT32_MAX || batch.liveReferences == 0)
                return Result::ProtocolViolation;
            batchIndex = index;
        }
    }
    if (batchIndex == UINT32_MAX || liveBatches != pageBatchCount_[ring])
        return Result::ProtocolViolation;
    const auto eventBase = static_cast<std::size_t>(ring) *
        config_.maxEventsPerQuantum;
    std::uint32_t actualLive = 0;
    std::uint32_t actualPrepared = 0;
    for (std::uint32_t index = 0; index < live + prepared; ++index) {
        if (pageEvents_[eventBase + index].executeFrame != expected.executeFrame)
            continue;
        if (index < live) ++actualLive;
        else ++actualPrepared;
    }
    const auto& batch = pageBatchRows_[batchBase + batchIndex];
    if (batch.liveReferences != actualLive ||
        batch.preparedReferences != actualPrepared ||
        actualLive < expected.events.size())
        return Result::ProtocolViolation;

    // Every identity and counter is checked before this single-writer commit.
    std::sort(positions.begin(), positions.end(), std::greater<>{});
    for (const auto position : positions) {
        const auto currentLive = pageEventCount_[ring];
        const auto last = currentLive + prepared - 1;
        pageEvents_[eventBase + position] =
            pageEvents_[eventBase + currentLive - 1];
        if (prepared != 0)
            pageEvents_[eventBase + currentLive - 1] = pageEvents_[eventBase + last];
        pageEvents_[eventBase + last] = {};
        --pageEventCount_[ring];
    }
    pageSorted_[ring] = 0;
    auto& changedBatch = pageBatchRows_[batchBase + batchIndex];
    changedBatch.liveReferences -= static_cast<std::uint32_t>(expected.events.size());
    if (changedBatch.liveReferences == 0) --pageBatchCount_[ring];
    if (changedBatch.liveReferences == 0 && changedBatch.preparedReferences == 0) {
        const auto lastBatch = --pageBatchOccupiedCount_[ring];
        changedBatch = pageBatchRows_[batchBase + lastBatch];
        pageBatchRows_[batchBase + lastBatch] = {};
    }
    pageHeaders_[ring].committedTransactionCount -=
        static_cast<std::uint32_t>(expected.events.size());
    liveTransactionCount_ -= static_cast<std::uint32_t>(expected.events.size());
    for (const auto slot : slots) {
        transactions_[slot] = {};
        freeTransactions_.push_back(slot);
    }
    return Result::Removed;
}

bool ScheduledTransactionPool::HasFrontierEntryEvent(
    const FrontierEntryReceiptRef& entry) const noexcept {
    if (!entry.Valid()) return false;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto count = pageEventCount_[ring] + pagePreparedEventCount_[ring];
        const auto base = static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0; index < count; ++index)
            if (pageEvents_[base + index].frontier.kind == FrontierOperationKind::Entry &&
                pageEvents_[base + index].frontier.entry == entry)
                return true;
    }
    return false;
}

ReserveRefusal ScheduledTransactionPool::PreflightSingleEvent(
    std::uint64_t executeFrame,
    std::uint64_t publicationFrontier) const noexcept {
    return PreflightSingleEventTransactions(
        executeFrame, 1, publicationFrontier);
}

ReserveRefusal ScheduledTransactionPool::PreflightSingleEventTransactions(
    std::uint64_t executeFrame,
    std::uint32_t additionalTransactionCount,
    std::uint64_t publicationFrontier) const noexcept {
    if (!Valid() || config_.maxTransactionEvents == 0 ||
        additionalTransactionCount == 0) {
        return ReserveRefusal::OutsideWindow;
    }
    publicationFrontier =
        std::max(publicationFrontier, publishCutoffFrame_);
    const std::uint64_t frame =
        std::max(executeFrame, publicationFrontier);
    std::uint32_t page = 0;
    if (!PageForFrame(frame, &page)) {
        return ReserveRefusal::OutsideWindow;
    }
    const std::uint32_t ring = RingIndex(page);
    if (pageHeaders_[ring].frozen) {
        return ReserveRefusal::Late;
    }
    const std::uint64_t occupiedIngress =
        static_cast<std::uint64_t>(ingressThisQuantum_) +
        preparedIngressThisQuantum_ + unplacedIngressThisQuantum_;
    if (occupiedIngress > config_.maxIngressTransactionsPerQuantum ||
        additionalTransactionCount >
            config_.maxIngressTransactionsPerQuantum -
                occupiedIngress) {
        return ReserveRefusal::IngressRateExceeded;
    }
    if (unplacedTransactions_ > freeTransactions_.size() ||
        additionalTransactionCount > freeTransactions_.size() - unplacedTransactions_) {
        return ReserveRefusal::PoolFull;
    }
    const std::uint32_t occupiedEvents =
        pageEventCount_[ring] + pagePreparedEventCount_[ring];
    if (occupiedEvents > config_.maxEventsPerQuantum ||
        additionalTransactionCount >
            config_.maxEventsPerQuantum - occupiedEvents) {
        return ReserveRefusal::PageEventCapacity;
    }
    const std::size_t batchBase =
        static_cast<std::size_t>(ring) *
        config_.maxFrameBatchesPerQuantum;
    bool existingBatch = false;
    for (std::uint32_t batch = 0;
         batch < pageBatchOccupiedCount_[ring]; ++batch) {
        if (pageBatchRows_[batchBase + batch].frame == frame) {
            existingBatch = true;
            break;
        }
    }
    if (!existingBatch &&
        pageBatchOccupiedCount_[ring] >=
            config_.maxFrameBatchesPerQuantum) {
        return ReserveRefusal::PageBatchCapacity;
    }
    return CheckUnplacedPageBudget(additionalTransactionCount,
                                   existingBatch ? 0 : 1);
}

ReserveRefusal
ScheduledTransactionPool::PreflightSingleEventTransactionBatch(
    const std::uint64_t* executeFrames,
    std::uint32_t transactionCount,
    std::uint64_t publicationFrontier) const noexcept {
    if (!Valid() || config_.maxTransactionEvents == 0 ||
        executeFrames == nullptr || transactionCount == 0) {
        return ReserveRefusal::OutsideWindow;
    }
    publicationFrontier =
        std::max(publicationFrontier, publishCutoffFrame_);
    const std::uint64_t occupiedIngress =
        static_cast<std::uint64_t>(ingressThisQuantum_) +
        preparedIngressThisQuantum_ + unplacedIngressThisQuantum_;
    if (occupiedIngress > config_.maxIngressTransactionsPerQuantum ||
        transactionCount > config_.maxIngressTransactionsPerQuantum -
                               occupiedIngress) {
        return ReserveRefusal::IngressRateExceeded;
    }
    if (unplacedTransactions_ > freeTransactions_.size() ||
        transactionCount > freeTransactions_.size() - unplacedTransactions_) {
        return ReserveRefusal::PoolFull;
    }

    std::uint64_t totalNewBatches = 0;
    for (std::uint32_t index = 0; index < transactionCount; ++index) {
        const std::uint64_t frame =
            std::max(executeFrames[index], publicationFrontier);
        std::uint32_t page = 0;
        if (!PageForFrame(frame, &page)) {
            return ReserveRefusal::OutsideWindow;
        }
        const std::uint32_t ring = RingIndex(page);
        if (pageHeaders_[ring].frozen) {
            return ReserveRefusal::Late;
        }
        bool pageSeenEarlier = false;
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            std::uint32_t priorPage = 0;
            const std::uint64_t priorFrame =
                std::max(executeFrames[prior], publicationFrontier);
            pageSeenEarlier = pageSeenEarlier ||
                (PageForFrame(priorFrame, &priorPage) && priorPage == page);
        }
        if (pageSeenEarlier) continue;

        std::uint32_t additionalEvents = 0;
        std::uint32_t additionalBatches = 0;
        const std::size_t batchBase =
            static_cast<std::size_t>(ring) *
            config_.maxFrameBatchesPerQuantum;
        for (std::uint32_t candidate = 0;
             candidate < transactionCount; ++candidate) {
            const std::uint64_t candidateFrame =
                std::max(executeFrames[candidate], publicationFrontier);
            std::uint32_t candidatePage = 0;
            if (!PageForFrame(candidateFrame, &candidatePage) ||
                candidatePage != page) {
                continue;
            }
            ++additionalEvents;
            bool existingBatch = false;
            for (std::uint32_t batch = 0;
                 batch < pageBatchOccupiedCount_[ring]; ++batch) {
                if (pageBatchRows_[batchBase + batch].frame ==
                    candidateFrame) {
                    existingBatch = true;
                    break;
                }
            }
            if (existingBatch) continue;
            bool frameSeenEarlier = false;
            for (std::uint32_t prior = 0; prior < candidate; ++prior) {
                frameSeenEarlier = frameSeenEarlier ||
                    std::max(executeFrames[prior], publicationFrontier) ==
                        candidateFrame;
            }
            if (!frameSeenEarlier) ++additionalBatches;
        }
        const std::uint32_t occupiedEvents =
            pageEventCount_[ring] + pagePreparedEventCount_[ring];
        if (occupiedEvents > config_.maxEventsPerQuantum ||
            additionalEvents >
                config_.maxEventsPerQuantum - occupiedEvents) {
            return ReserveRefusal::PageEventCapacity;
        }
        if (pageBatchOccupiedCount_[ring] >
                config_.maxFrameBatchesPerQuantum ||
            additionalBatches > config_.maxFrameBatchesPerQuantum -
                                    pageBatchOccupiedCount_[ring]) {
            return ReserveRefusal::PageBatchCapacity;
        }
        totalNewBatches += additionalBatches;
    }
    return CheckUnplacedPageBudget(transactionCount, totalNewBatches);
}

ReserveRefusal ScheduledTransactionPool::PreflightTransaction(
    const TransactionRequest& request,
    std::uint64_t publicationFrontier) const noexcept {
    return PreflightTransactionBatch(&request, 1, publicationFrontier);
}

ReserveRefusal ScheduledTransactionPool::PreflightTransactionPair(
    const TransactionRequest& first,
    const TransactionRequest& second,
    std::uint64_t publicationFrontier) const noexcept {
    const TransactionRequest requests[] = {first, second};
    return PreflightTransactionBatch(requests, 2, publicationFrontier);
}

ReserveRefusal ScheduledTransactionPool::PreflightTransactionBatch(
    const TransactionRequest* requests,
    std::size_t requestCount,
    std::uint64_t publicationFrontier) const noexcept {
    if (!Valid()) {
        return ReserveRefusal::EmptyTransaction;
    }
    if (requests == nullptr || requestCount == 0) {
        return ReserveRefusal::EmptyTransaction;
    }
    publicationFrontier =
        std::max(publicationFrontier, publishCutoffFrame_);
    const std::uint64_t occupiedIngress =
        static_cast<std::uint64_t>(ingressThisQuantum_) +
        preparedIngressThisQuantum_ + unplacedIngressThisQuantum_;
    if (occupiedIngress > config_.maxIngressTransactionsPerQuantum ||
        requestCount > config_.maxIngressTransactionsPerQuantum -
                           occupiedIngress) {
        return ReserveRefusal::IngressRateExceeded;
    }
    if (unplacedTransactions_ > freeTransactions_.size() ||
        requestCount > freeTransactions_.size() - unplacedTransactions_) {
        return ReserveRefusal::PoolFull;
    }

    for (std::size_t requestIndex = 0;
         requestIndex < requestCount; ++requestIndex) {
        const TransactionRequest& request = requests[requestIndex];
        if (request.transportEpoch == kInvalidTransportEpoch ||
            request.transportEpoch != epoch_) {
            return ReserveRefusal::InvalidTransportEpoch;
        }
        if (request.events == nullptr || request.eventCount == 0) {
            return ReserveRefusal::EmptyTransaction;
        }
        if (request.eventCount > config_.maxTransactionEvents) {
            return ReserveRefusal::TransactionEventsExceeded;
        }
        std::uint64_t firstFrame = request.events[0].executeFrame;
        std::uint64_t lastFrame = firstFrame;
        for (std::size_t eventIndex = 0;
             eventIndex < request.eventCount; ++eventIndex) {
            const auto& event = request.events[eventIndex];
            if (event.frontier.Present() && (!event.frontier.Valid() ||
                event.frontier.transport != request.transportEpoch ||
                (event.frontier.group.Valid() && event.frontier.group.absoluteQuantum !=
                    event.executeFrame / config_.quantumFrames))) return ReserveRefusal::FrontierIdentity;
            const std::uint64_t frame =
                request.events[eventIndex].executeFrame;
            firstFrame = std::min(firstFrame, frame);
            lastFrame = std::max(lastFrame, frame);
            std::uint32_t page = 0;
            if (!PageForFrame(frame, &page)) {
                return ReserveRefusal::OutsideWindow;
            }
            if (frame < publicationFrontier ||
                pageHeaders_[RingIndex(page)].frozen) {
                return ReserveRefusal::Late;
            }
        }
        if (lastFrame - firstFrame > config_.maxTransactionSpanFrames) {
            return ReserveRefusal::SpanExceeded;
        }
    }

    const auto visitEarlierEvents = [&](std::size_t requestIndex,
                                        std::size_t eventIndex,
                                        const auto& visitor) {
        for (std::size_t priorRequest = 0;
             priorRequest <= requestIndex; ++priorRequest) {
            const std::size_t limit = priorRequest == requestIndex
                ? eventIndex
                : requests[priorRequest].eventCount;
            for (std::size_t priorEvent = 0;
                 priorEvent < limit; ++priorEvent) {
                visitor(requests[priorRequest].events[priorEvent]);
            }
        }
    };

    std::uint64_t totalAdditionalEvents = 0;
    std::uint64_t totalAdditionalBatches = 0;
    for (std::size_t requestIndex = 0;
         requestIndex < requestCount; ++requestIndex) {
        const TransactionRequest& request = requests[requestIndex];
        for (std::size_t eventIndex = 0;
             eventIndex < request.eventCount; ++eventIndex) {
            const std::uint64_t candidateFrame =
                request.events[eventIndex].executeFrame;
            std::uint32_t candidatePage = 0;
            (void)PageForFrame(candidateFrame, &candidatePage);
            bool pageSeenEarlier = false;
            visitEarlierEvents(
                requestIndex, eventIndex,
                [&](const ScheduledEventInput& earlier) {
                    std::uint32_t earlierPage = 0;
                    pageSeenEarlier = pageSeenEarlier ||
                        (PageForFrame(earlier.executeFrame,
                                      &earlierPage) &&
                         earlierPage == candidatePage);
                });
            if (pageSeenEarlier) {
                continue;
            }

            const std::uint32_t ring = RingIndex(candidatePage);
            const std::size_t batchBase =
                static_cast<std::size_t>(ring) *
                config_.maxFrameBatchesPerQuantum;
            std::uint32_t additionalEvents = 0;
            std::uint32_t additionalBatches = 0;
            for (std::size_t demandRequest = 0;
                 demandRequest < requestCount; ++demandRequest) {
                const TransactionRequest& demand = requests[demandRequest];
                for (std::size_t demandEvent = 0;
                     demandEvent < demand.eventCount; ++demandEvent) {
                    const std::uint64_t frame =
                        demand.events[demandEvent].executeFrame;
                    std::uint32_t page = 0;
                    if (!PageForFrame(frame, &page) ||
                        page != candidatePage) {
                        continue;
                    }
                    ++additionalEvents;
                    bool existingBatch = false;
                    for (std::uint32_t batch = 0;
                         batch < pageBatchOccupiedCount_[ring]; ++batch) {
                        if (pageBatchRows_[batchBase + batch].frame == frame) {
                            existingBatch = true;
                            break;
                        }
                    }
                    if (existingBatch) {
                        continue;
                    }
                    bool frameSeenEarlier = false;
                    visitEarlierEvents(
                        demandRequest, demandEvent,
                        [&](const ScheduledEventInput& earlier) {
                            frameSeenEarlier = frameSeenEarlier ||
                                earlier.executeFrame == frame;
                        });
                    if (!frameSeenEarlier) {
                        ++additionalBatches;
                    }
                }
            }
            const std::uint32_t occupiedEvents =
                pageEventCount_[ring] + pagePreparedEventCount_[ring];
            if (occupiedEvents > config_.maxEventsPerQuantum ||
                additionalEvents >
                    config_.maxEventsPerQuantum - occupiedEvents) {
                return ReserveRefusal::PageEventCapacity;
            }
            if (pageBatchOccupiedCount_[ring] >
                    config_.maxFrameBatchesPerQuantum ||
                additionalBatches >
                    config_.maxFrameBatchesPerQuantum -
                        pageBatchOccupiedCount_[ring]) {
                return ReserveRefusal::PageBatchCapacity;
            }
            totalAdditionalEvents += additionalEvents;
            totalAdditionalBatches += additionalBatches;
        }
    }
    return CheckUnplacedPageBudget(totalAdditionalEvents,
                                   totalAdditionalBatches);
}

ReserveRefusal ScheduledTransactionPool::PrepareTransactionBatch(
    const TransactionRequest* requests, std::size_t requestCount,
    std::uint64_t publicationFrontier,
    PreparedTransactionBatch* out,
    const PreparedPageGroupTransferScope* transferScope) noexcept {
    if (out == nullptr || out->Valid() ||
        (transferScope != nullptr && (!transferScope->Valid() || requests == nullptr ||
            requestCount != 5 || privateMaterializationActive_))) {
        return ReserveRefusal::EmptyTransaction;
    }
    const ReserveRefusal preflight = PreflightTransactionBatch(
        requests, requestCount, publicationFrontier);
    if (preflight != ReserveRefusal::None) {
        refusalCounts_[static_cast<std::size_t>(preflight)] += 1;
        return preflight;
    }
    if (transferScope != nullptr) {
        const auto frame = requests[0].events[0].executeFrame;
        for (std::size_t index = 0; index < requestCount; ++index) {
            const auto& request = requests[index];
            if (request.eventCount != 1 || request.transportEpoch != epoch_ ||
                request.transactionId != transferScope->sequence || request.occurrence.present ||
                request.occurrence.token.slot != 0 || request.occurrence.token.generation != 0 ||
                request.occurrence.occurrenceScheduleEpoch != 0 ||
                request.occurrence.arrangingInstanceScheduleEpoch != 0 ||
                request.immediateEntry.gate != nullptr || request.immediateEntry.generation != 0 ||
                request.events[0].frontier.Present() || request.events[0].executeFrame != frame ||
                request.events[0].logicCommitSequence != transferScope->sequence)
                return ReserveRefusal::EmptyTransaction;
        }
    }
    if (freePreparedTransactionClaims_.size() <= unplacedPreparedClaims_) {
        refusalCounts_[static_cast<std::size_t>(ReserveRefusal::PoolFull)] += 1;
        return ReserveRefusal::PoolFull;
    }

    const std::uint32_t claimSlot =
        freePreparedTransactionClaims_.back();
    freePreparedTransactionClaims_.pop_back();
    PreparedTransactionClaim& claim =
        preparedTransactionClaims_[claimSlot];
    claim.used = true;
    claim.committed = false;
    claim.frozenSequence = 0;
    claim.transactionCount = static_cast<std::uint32_t>(requestCount);
    claim.preparedIngressQuantum = baseQuantumIndex_;

    for (std::size_t requestIndex = 0;
         requestIndex < requestCount; ++requestIndex) {
        const TransactionRequest& request = requests[requestIndex];
        const std::uint32_t transactionSlot = freeTransactions_.back();
        freeTransactions_.pop_back();
        TransactionRecord& record = transactions_[transactionSlot];
        record = TransactionRecord{};
        record.prepared = true;
        record.preparedClaimSlot = claimSlot;
        record.preparedClaimGeneration = claim.generation;
        record.preparedRequestIndex = static_cast<std::uint32_t>(requestIndex);
        if (transferScope != nullptr) {
            claim.transferTransactionSlots[requestIndex] = transactionSlot;
            claim.transferRequestArms[requestIndex] = request.armToken;
        }
        record.epoch = request.transportEpoch;
        record.ownerInstance = request.ownerInstanceHandle;
        record.ownerScheduleEpoch = request.ownerInstanceScheduleEpoch;
        record.occurrence = request.occurrence;
        record.program = request.program;
        record.armToken = request.armToken;
        record.immediateEntry = request.immediateEntry;
        record.firstFrame = request.events[0].executeFrame;
        record.lastFrame = record.firstFrame;
        record.remainingEvents =
            static_cast<std::uint32_t>(request.eventCount);
        record.summary.eventCount = record.remainingEvents;

        std::uint32_t distinctPages = 0;
        std::uint32_t newBatches = 0;
        for (std::size_t eventIndex = 0;
             eventIndex < request.eventCount; ++eventIndex) {
            const ScheduledEventInput& input = request.events[eventIndex];
            record.firstFrame =
                std::min(record.firstFrame, input.executeFrame);
            record.lastFrame =
                std::max(record.lastFrame, input.executeFrame);
            std::uint32_t page = 0;
            (void)PageForFrame(input.executeFrame, &page);
            const std::uint32_t ring = RingIndex(page);
            const std::size_t eventBase =
                static_cast<std::size_t>(ring) *
                config_.maxEventsPerQuantum;
            EventRow& row = pageEvents_[
                eventBase + pageEventCount_[ring] +
                pagePreparedEventCount_[ring]];
            row = {};
            row.executeFrame = input.executeFrame;
            row.logicCommitSequence = input.logicCommitSequence;
            row.executionOrdinal = input.executionOrdinal;
            row.eventIndex = input.eventIndex;
            row.transactionOrdinal = input.transactionOrdinal;
            row.subeventPhase = input.subeventPhase;
            row.payload = input.payload;
            row.authoredFadeOutCandidate =
                input.authoredFadeOutCandidate;
            row.armToken = input.armToken;
            row.frontier = input.frontier;
            row.txnSlot = transactionSlot;
            row.preparedEventOrdinal = static_cast<std::uint32_t>(eventIndex);
            row.rowSequence = nextRowSequence_++;
            if (transferScope != nullptr) claim.transferRowSequences[requestIndex] = row.rowSequence;
            ++pagePreparedEventCount_[ring];
            ++pagePublicationHolds_[ring];

            const std::size_t batchBase =
                static_cast<std::size_t>(ring) *
                config_.maxFrameBatchesPerQuantum;
            FrameBatchRow* batch = nullptr;
            for (std::uint32_t batchIndex = 0;
                 batchIndex < pageBatchOccupiedCount_[ring];
                 ++batchIndex) {
                FrameBatchRow& candidate =
                    pageBatchRows_[batchBase + batchIndex];
                if (candidate.frame == input.executeFrame) {
                    batch = &candidate;
                    break;
                }
            }
            if (batch == nullptr) {
                batch = &pageBatchRows_[
                    batchBase + pageBatchOccupiedCount_[ring]++];
                *batch = {input.executeFrame, 0, 0};
                ++newBatches;
            }
            ++batch->preparedReferences;

            bool pageSeenEarlier = false;
            for (std::size_t prior = 0; prior < eventIndex; ++prior) {
                std::uint32_t priorPage = 0;
                (void)PageForFrame(
                    request.events[prior].executeFrame, &priorPage);
                if (priorPage == page) {
                    pageSeenEarlier = true;
                    break;
                }
            }
            if (!pageSeenEarlier) {
                ++distinctPages;
            }
        }
        record.summary.frameBatchCount = newBatches;
        record.summary.pageSpan = distinctPages;
    }

    preparedIngressThisQuantum_ +=
        static_cast<std::uint32_t>(requestCount);
    ++preparedTransactionClaimCount_;
    out->owner_ = this;
    out->claimSlot_ = claimSlot;
    out->generation_ = claim.generation;
    out->prepared_ = true;
    if (transferScope != nullptr) {
        std::uint32_t page = 0;
        (void)PageForFrame(requests[0].events[0].executeFrame, &page);
        const auto ring = RingIndex(page);
        claim.transferOriginal = out;
        claim.transferScope = *transferScope;
        claim.transferRing = ring;
        claim.transferAbsoluteQuantum = pageHeaders_[ring].absoluteQuantumIndex;
        claim.transferPageGeneration = pageHeaders_[ring].pageGeneration;
        claim.transferTransport = epoch_;
        claim.transferFrame = requests[0].events[0].executeFrame;
        out->transferSelected_ = true;
    }
    return ReserveRefusal::None;
}

bool ScheduledTransactionPool::FreezePreparedTransactionBatch(
    const PreparedTransactionBatch& prepared,
    LogicCommitSequence sequence) noexcept {
    if (prepared.owner_ != this || !prepared.Valid() || prepared.Held() || sequence == 0 ||
        prepared.claimSlot_ >= preparedTransactionClaims_.size()) return false;
    auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    if (!claim.used || claim.committed || claim.generation != prepared.generation_ ||
        (prepared.transferSelected_ && claim.transferScope.sequence != sequence) ||
        (claim.frozenSequence != 0 && claim.frozenSequence != sequence)) return false;
    if (claim.frozenSequence == sequence) return true;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto base = static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = pageEventCount_[ring];
             index < pageEventCount_[ring] + pagePreparedEventCount_[ring]; ++index) {
            auto& row = pageEvents_[base + index];
            const auto& record = transactions_[row.txnSlot];
            if (record.preparedClaimSlot != prepared.claimSlot_ ||
                record.preparedClaimGeneration != prepared.generation_) continue;
            row.logicCommitSequence = sequence;
            row.transactionId = sequence;
        }
    }
    claim.frozenSequence = sequence;
    return true;
}

bool ScheduledTransactionPool::CapturePreparedTransactionEvent(
    const PreparedTransactionBatch& prepared,
    std::uint32_t requestIndex, std::uint32_t eventOrdinal,
    ConsumedEvent* out) const noexcept {
    if (out == nullptr || prepared.owner_ != this || !prepared.prepared_ ||
        prepared.claimSlot_ >= preparedTransactionClaims_.size()) return false;
    const auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    if (!claim.used || claim.generation != prepared.generation_ ||
        claim.frozenSequence == 0 || requestIndex >= claim.transactionCount) return false;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto base = static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0;
             index < pageEventCount_[ring] + pagePreparedEventCount_[ring]; ++index) {
            const auto& row = pageEvents_[base + index];
            const auto& record = transactions_[row.txnSlot];
            if (record.preparedClaimSlot != prepared.claimSlot_ ||
                record.preparedClaimGeneration != prepared.generation_ ||
                record.preparedRequestIndex != requestIndex ||
                row.preparedEventOrdinal != eventOrdinal) continue;
            *out = {};
            out->executeFrame = row.executeFrame;
            out->logicCommitSequence = row.logicCommitSequence;
            out->executionOrdinal = row.executionOrdinal;
            out->eventIndex = row.eventIndex;
            out->transactionOrdinal = row.transactionOrdinal;
            out->subeventPhase = row.subeventPhase;
            out->rowSequence = row.rowSequence;
            out->transactionId = row.transactionId;
            out->txnSlot = row.txnSlot;
            out->payload = row.payload;
            out->authoredFadeOutCandidate = row.authoredFadeOutCandidate;
            out->armToken = row.armToken.Valid() ? row.armToken : record.armToken;
            out->frontier = row.frontier;
            out->program = record.program;
            out->ownerInstanceHandle = record.ownerInstance;
            out->ownerInstanceScheduleEpoch = record.ownerScheduleEpoch;
            return true;
        }
    }
    return false;
}

bool ScheduledTransactionPool::MatchesFrozenPreparedTransactionEvents(
    const PreparedTransactionBatch& prepared, LogicCommitSequence sequence,
    TransportEpoch transport, const ConsumedEvent* originals, std::size_t count,
    const FrontierEventBinding* bindings) const noexcept {
    if (prepared.owner_ != this || !prepared.prepared_ || originals == nullptr || count == 0 ||
        sequence == 0 || transport != epoch_ ||
        prepared.claimSlot_ >= preparedTransactionClaims_.size()) return false;
    const auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    if (!claim.used || claim.committed || claim.generation != prepared.generation_ ||
        claim.frozenSequence != sequence || claim.transactionCount != count) return false;
    std::size_t matched = 0;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto base = static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = pageEventCount_[ring];
             index < pageEventCount_[ring] + pagePreparedEventCount_[ring]; ++index) {
            const auto& row = pageEvents_[base + index];
            if (row.txnSlot >= transactions_.size()) return false;
            const auto& record = transactions_[row.txnSlot];
            if (record.preparedClaimSlot != prepared.claimSlot_ ||
                record.preparedClaimGeneration != prepared.generation_) continue;
            if (!record.prepared || record.preparedRequestIndex >= count ||
                record.epoch != transport || record.remainingEvents != 1 ||
                row.preparedEventOrdinal != 0) return false;
            const auto& expected = originals[record.preparedRequestIndex];
            const auto arm = row.armToken.Valid() ? row.armToken : record.armToken;
            if (expected.frontier.Present() || expected.frozenTxnValid ||
                row.txnSlot != expected.txnSlot || row.rowSequence != expected.rowSequence ||
                row.executeFrame != expected.executeFrame || row.logicCommitSequence != sequence ||
                expected.logicCommitSequence != sequence || row.transactionId != sequence ||
                expected.transactionId != sequence || row.executionOrdinal != expected.executionOrdinal ||
                row.eventIndex != expected.eventIndex || row.transactionOrdinal != expected.transactionOrdinal ||
                row.subeventPhase != expected.subeventPhase || row.payload != expected.payload ||
                row.authoredFadeOutCandidate != expected.authoredFadeOutCandidate ||
                arm.slot != expected.armToken.slot || arm.generation != expected.armToken.generation ||
                arm.renderSlot != expected.armToken.renderSlot ||
                arm.renderGeneration != expected.armToken.renderGeneration ||
                record.program.program != expected.program.program ||
                record.program.programGeneration != expected.program.programGeneration ||
                record.ownerInstance != expected.ownerInstanceHandle ||
                record.ownerScheduleEpoch != expected.ownerInstanceScheduleEpoch) return false;
            if (bindings == nullptr ? row.frontier.Present() :
                (!bindings[record.preparedRequestIndex].Valid() ||
                 !(row.frontier == bindings[record.preparedRequestIndex]))) return false;
            ++matched;
        }
    }
    return matched == count;
}

bool ScheduledTransactionPool::BindPreparedTransactionEventFrontier(
    const PreparedTransactionBatch& prepared,
    std::uint32_t requestIndex, std::uint32_t eventOrdinal,
    const FrontierEventBinding& binding) noexcept {
    if (!binding.Valid() || prepared.owner_ != this || !prepared.Valid() || prepared.Held() ||
        prepared.claimSlot_ >= preparedTransactionClaims_.size()) return false;
    const auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    if (!claim.used || claim.committed || claim.generation != prepared.generation_ ||
        requestIndex >= claim.transactionCount) return false;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto base = static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = pageEventCount_[ring];
             index < pageEventCount_[ring] + pagePreparedEventCount_[ring]; ++index) {
            auto& row = pageEvents_[base + index];
            const auto& record = transactions_[row.txnSlot];
            if (record.preparedClaimSlot != prepared.claimSlot_ ||
                record.preparedClaimGeneration != prepared.generation_ ||
                record.preparedRequestIndex != requestIndex || row.preparedEventOrdinal != eventOrdinal) continue;
            if (binding.transport != record.epoch ||
                (binding.group.Valid() && binding.group.absoluteQuantum != row.executeFrame / quantumFrames_) ||
                (row.frontier.Present() && !(row.frontier == binding))) return false;
            row.frontier = binding;
            return true;
        }
    }
    return false;
}

bool ScheduledTransactionPool::CommitPreparedTransactionBatch(
    PreparedTransactionBatch* prepared,
    LogicCommitSequence sequence, bool retainPublicationHold) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->Valid() || prepared->Held() || sequence == 0 ||
        prepared->claimSlot_ >= preparedTransactionClaims_.size()) {
        return false;
    }
    PreparedTransactionClaim& claim =
        preparedTransactionClaims_[prepared->claimSlot_];
    if (!claim.used || claim.committed || claim.generation != prepared->generation_ ||
        !CanChargePreparedTransactionIngress(claim)) {
        return false;
    }
    std::uint32_t matchedTransactions = 0;
    for (const TransactionRecord& record : transactions_) {
        if (record.prepared &&
            record.preparedClaimSlot == prepared->claimSlot_ &&
            record.preparedClaimGeneration == prepared->generation_) {
            ++matchedTransactions;
        }
    }
    if (matchedTransactions != claim.transactionCount) {
        return false;
    }
    if (!FreezePreparedTransactionBatch(*prepared, sequence)) return false;
    SpendPreparedPageBatchOriginal(prepared->claimSlot_);

    for (std::uint32_t transactionSlot = 0;
         transactionSlot < transactions_.size(); ++transactionSlot) {
        TransactionRecord& record = transactions_[transactionSlot];
        if (!record.prepared ||
            record.preparedClaimSlot != prepared->claimSlot_ ||
            record.preparedClaimGeneration != prepared->generation_) {
            continue;
        }
        for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
            const std::size_t eventBase =
                static_cast<std::size_t>(ring) *
                config_.maxEventsPerQuantum;
            bool transactionTouchesPage = false;
            for (;;) {
                const std::uint32_t liveCount = pageEventCount_[ring];
                const std::uint32_t preparedCount =
                    pagePreparedEventCount_[ring];
                std::uint32_t found = preparedCount;
                for (std::uint32_t index = 0;
                     index < preparedCount; ++index) {
                    if (pageEvents_[eventBase + liveCount + index].txnSlot ==
                        transactionSlot) {
                        found = index;
                        break;
                    }
                }
                if (found == preparedCount) {
                    break;
                }
                if (found != 0) {
                    std::swap(pageEvents_[eventBase + liveCount],
                              pageEvents_[eventBase + liveCount + found]);
                }
                EventRow& row = pageEvents_[eventBase + liveCount];

                const std::size_t batchBase =
                    static_cast<std::size_t>(ring) *
                    config_.maxFrameBatchesPerQuantum;
                for (std::uint32_t batchIndex = 0;
                     batchIndex < pageBatchOccupiedCount_[ring];
                     ++batchIndex) {
                    FrameBatchRow& batch =
                        pageBatchRows_[batchBase + batchIndex];
                    if (batch.frame != row.executeFrame) {
                        continue;
                    }
                    --batch.preparedReferences;
                    if (batch.liveReferences == 0) {
                        ++pageBatchCount_[ring];
                        frameBatchHighWater_ =
                            std::max(frameBatchHighWater_,
                                     pageBatchCount_[ring]);
                    }
                    ++batch.liveReferences;
                    break;
                }
                ++pageEventCount_[ring];
                --pagePreparedEventCount_[ring];
                pageSorted_[ring] = 0;
                eventHighWater_ =
                    std::max(eventHighWater_, pageEventCount_[ring]);
                transactionTouchesPage = true;
            }
            if (transactionTouchesPage) {
                ++pageHeaders_[ring].committedTransactionCount;
            }
        }
        record.used = true;
        record.prepared = false;
        record.id = sequence;
        ++liveTransactionCount_;
        transactionEventsHighWater_ = std::max(
            transactionEventsHighWater_, record.summary.eventCount);
        transactionSpanHighWater_ = std::max(
            transactionSpanHighWater_,
            record.lastFrame - record.firstFrame);
    }

    transactionHighWater_ =
        std::max(transactionHighWater_, liveTransactionCount_);
    if (claim.preparedIngressQuantum == baseQuantumIndex_)
        preparedIngressThisQuantum_ -= claim.transactionCount;
    ingressThisQuantum_ += claim.transactionCount;
    ingressHighWater_ = std::max(ingressHighWater_, ingressThisQuantum_);
    claim.committed = true;
    claim.committedIngressQuantum = baseQuantumIndex_;
    if (!retainPublicationHold) FinishPreparedTransactionClaim(prepared);
    return true;
}

void ScheduledTransactionPool::FinishPreparedTransactionClaim(
    PreparedTransactionBatch* prepared) noexcept {
    auto& claim = preparedTransactionClaims_[prepared->claimSlot_];
    SpendPreparedPageBatchOriginal(prepared->claimSlot_);
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const auto base = static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
        for (std::uint32_t index = 0; index < pageEventCount_[ring]; ++index) {
            const auto& row = pageEvents_[base + index];
            const auto& record = transactions_[row.txnSlot];
            if (record.preparedClaimSlot == prepared->claimSlot_ &&
                record.preparedClaimGeneration == prepared->generation_) --pagePublicationHolds_[ring];
        }
    }
    for (auto& record : transactions_) {
        if (record.preparedClaimSlot == prepared->claimSlot_ &&
            record.preparedClaimGeneration == prepared->generation_) {
            record.preparedClaimSlot = UINT32_MAX;
            record.preparedClaimGeneration = 0;
        }
    }
    --preparedTransactionClaimCount_;
    claim.used = false;
    claim.committed = false;
    claim.frozenSequence = 0;
    claim.transactionCount = 0;
    claim.preparedIngressQuantum = 0;
    claim.committedIngressQuantum = 0;
    if (claim.generation != UINT32_MAX) {
        ++claim.generation;
        freePreparedTransactionClaims_.push_back(prepared->claimSlot_);
    }
    prepared->owner_ = nullptr;
    prepared->claimSlot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->prepared_ = false;
}

bool ScheduledTransactionPool::CanReleasePreparedTransactionPublication(
    const PreparedTransactionBatch& prepared) const noexcept {
    if (prepared.owner_ != this || !prepared.Valid() || prepared.Held() ||
        prepared.claimSlot_ >= preparedTransactionClaims_.size()) return false;
    const auto& claim = preparedTransactionClaims_[prepared.claimSlot_];
    return claim.used && claim.committed && claim.generation == prepared.generation_;
}

bool ScheduledTransactionPool::ReleasePreparedTransactionPublication(
    PreparedTransactionBatch* prepared) noexcept {
    if (prepared == nullptr || !CanReleasePreparedTransactionPublication(*prepared)) return false;
    FinishPreparedTransactionClaim(prepared);
    return true;
}

bool ScheduledTransactionPool::CancelPreparedTransactionBatch(
    PreparedTransactionBatch* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->Valid() || prepared->Held() ||
        prepared->claimSlot_ >= preparedTransactionClaims_.size()) {
        return false;
    }
    PreparedTransactionClaim& claim =
        preparedTransactionClaims_[prepared->claimSlot_];
    const bool countedInCurrentQuantum =
        claim.committed &&
        claim.committedIngressQuantum == baseQuantumIndex_;
    const bool preparedInCurrentQuantum =
        !claim.committed &&
        claim.preparedIngressQuantum == baseQuantumIndex_;
    if (!claim.used || claim.generation != prepared->generation_ ||
        claim.transactionCount == 0 ||
        (countedInCurrentQuantum &&
         ingressThisQuantum_ < claim.transactionCount) ||
        (preparedInCurrentQuantum &&
         preparedIngressThisQuantum_ < claim.transactionCount)) {
        return false;
    }

    std::uint32_t matches = 0;
    for (const auto& record : transactions_) {
        if (record.preparedClaimSlot == prepared->claimSlot_ &&
            record.preparedClaimGeneration == prepared->generation_ &&
            (claim.committed ? record.used : record.prepared)) ++matches;
    }
    if (matches != claim.transactionCount) return false;
    SpendPreparedPageBatchOriginal(prepared->claimSlot_);

    std::uint32_t releasedTransactions = 0;
    for (std::uint32_t transactionSlot = 0;
         transactionSlot < transactions_.size(); ++transactionSlot) {
        TransactionRecord& record = transactions_[transactionSlot];
        if (!(claim.committed ? record.used : record.prepared) ||
            record.preparedClaimSlot != prepared->claimSlot_ ||
            record.preparedClaimGeneration != prepared->generation_) {
            continue;
        }
        for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
            const std::size_t eventBase =
                static_cast<std::size_t>(ring) *
                config_.maxEventsPerQuantum;
            bool touchesPage = false;
            for (;;) {
                const std::uint32_t liveCount = pageEventCount_[ring];
                const std::uint32_t preparedCount =
                    pagePreparedEventCount_[ring];
                const auto start = claim.committed ? 0u : liveCount;
                const auto count = claim.committed ? liveCount : preparedCount;
                std::uint32_t found = count;
                for (std::uint32_t index = 0;
                     index < count; ++index) {
                    if (pageEvents_[eventBase + start + index].txnSlot ==
                        transactionSlot) {
                        found = index;
                        break;
                    }
                }
                if (found == count) {
                    break;
                }
                EventRow& row =
                    pageEvents_[eventBase + start + found];
                const std::size_t batchBase =
                    static_cast<std::size_t>(ring) *
                    config_.maxFrameBatchesPerQuantum;
                for (std::uint32_t batchIndex = 0;
                     batchIndex < pageBatchOccupiedCount_[ring];
                     ++batchIndex) {
                    FrameBatchRow& batch =
                        pageBatchRows_[batchBase + batchIndex];
                    if (batch.frame != row.executeFrame) {
                        continue;
                    }
                    if (claim.committed) {
                        if (--batch.liveReferences == 0) --pageBatchCount_[ring];
                    } else {
                        --batch.preparedReferences;
                    }
                    if (batch.liveReferences == 0 &&
                        batch.preparedReferences == 0) {
                        const std::uint32_t last =
                            --pageBatchOccupiedCount_[ring];
                        batch = pageBatchRows_[batchBase + last];
                        pageBatchRows_[batchBase + last] = {};
                    }
                    break;
                }
                const std::uint32_t last =
                    liveCount + preparedCount - 1;
                if (claim.committed) {
                    row = pageEvents_[eventBase + liveCount - 1];
                    if (preparedCount != 0) {
                        pageEvents_[eventBase + liveCount - 1] = pageEvents_[eventBase + last];
                    }
                    --pageEventCount_[ring];
                    pageSorted_[ring] = 0;
                } else {
                    row = pageEvents_[eventBase + last];
                    --pagePreparedEventCount_[ring];
                }
                pageEvents_[eventBase + last] = {};
                --pagePublicationHolds_[ring];
                touchesPage = true;
            }
            if (claim.committed && touchesPage) --pageHeaders_[ring].committedTransactionCount;
        }
        record = {};
        freeTransactions_.push_back(transactionSlot);
        ++releasedTransactions;
    }
    if (releasedTransactions != claim.transactionCount) {
        return false;
    }

    if (claim.committed) {
        if (countedInCurrentQuantum)
            ingressThisQuantum_ -= claim.transactionCount;
        liveTransactionCount_ -= claim.transactionCount;
    } else {
        if (preparedInCurrentQuantum)
            preparedIngressThisQuantum_ -= claim.transactionCount;
    }
    FinishPreparedTransactionClaim(prepared);
    return true;
}

ReserveResult ScheduledTransactionPool::ReserveCurrentSlice(
    const ScheduledCurrentSliceInput& input, PrePublishLatePolicy latePolicy,
    std::uint64_t publicationFrontier) {
    return ReserveCurrentSliceImpl(input, latePolicy, publicationFrontier, nullptr);
}

ReserveResult ScheduledTransactionPool::ReserveHeldCurrentSlice(
    const ScheduledCurrentSliceInput& input, PrePublishLatePolicy latePolicy,
    std::uint64_t publicationFrontier, HeldCurrentSliceToken* out) {
    if (out == nullptr || out->IsHeld()) return {};
    return ReserveCurrentSliceImpl(input, latePolicy, publicationFrontier, out);
}

ReserveResult ScheduledTransactionPool::ReserveCurrentSliceImpl(
    const ScheduledCurrentSliceInput& input, PrePublishLatePolicy latePolicy,
    std::uint64_t publicationFrontier, HeldCurrentSliceToken* held) {
    ReserveResult result{};
    publicationFrontier = std::max(publicationFrontier, publishCutoffFrame_);
    std::uint64_t frame = input.executeFrame;
    if (!Valid() || input.logicCommitSequence == 0 ||
        (input.kind != CurrentSliceKind::VariableBankCut &&
         input.kind != CurrentSliceKind::MeterProbeWithdrawal &&
         input.kind != CurrentSliceKind::NativeStartCalendar) ||
        input.slice.slot == UINT32_MAX || input.slice.generation == 0) {
        result.status = ReserveStatus::Refused;
        return result;
    }
    if (frame < publicationFrontier) {
        if (latePolicy == PrePublishLatePolicy::Drop) {
            result.status = ReserveStatus::DroppedLate;
            ++droppedLateCount_;
            return result;
        }
        if (latePolicy == PrePublishLatePolicy::Refuse) {
            result.status = ReserveStatus::Refused;
            result.refusal = ReserveRefusal::Late;
            ++refusalCounts_[static_cast<std::size_t>(result.refusal)];
            return result;
        }
        if (latePolicy == PrePublishLatePolicy::NextEquivalentBoundary) {
            result.status = ReserveStatus::Refused;
            result.refusal = ReserveRefusal::NeedsBoundaryResolution;
            ++refusalCounts_[static_cast<std::size_t>(result.refusal)];
            return result;
        }
        frame = publicationFrontier;
        result.rescheduled = true;
    }
    std::uint32_t page = 0;
    if (!PageForFrame(frame, &page)) {
        result.status = ReserveStatus::Refused;
        result.refusal = ReserveRefusal::OutsideWindow;
        ++refusalCounts_[static_cast<std::size_t>(result.refusal)];
        return result;
    }
    const std::uint32_t ring = RingIndex(page);
    if (pageHeaders_[ring].frozen &&
        pageQuantumStamp_[ring].load(std::memory_order_acquire) ==
            pageHeaders_[ring].absoluteQuantumIndex) {
        result.status = ReserveStatus::Refused;
        result.refusal = ReserveRefusal::Late;
        ++refusalCounts_[static_cast<std::size_t>(result.refusal)];
        return result;
    }
    const auto base = static_cast<std::size_t>(ring) * currentSliceStorageCapacity_;
    const bool native = input.kind == CurrentSliceKind::NativeStartCalendar;
    std::uint32_t quotaCount = 0;
    for (std::uint32_t i = 0; i < pageCurrentSliceCount_[ring]; ++i)
        if ((pageCurrentSlices_[base + i].kind == CurrentSliceKind::NativeStartCalendar) == native) ++quotaCount;
    const auto quota = native ? config_.maxNativeStartCalendarsPerQuantum : config_.maxCurrentSlicesPerQuantum;
    if (quotaCount >= quota || pageCurrentSliceCount_[ring] >= currentSliceStorageCapacity_ ||
        (held != nullptr && pagePublicationHolds_[ring] == UINT64_MAX)) {
        result.status = ReserveStatus::Refused;
        result.refusal = ReserveRefusal::PageEventCapacity;
        ++refusalCounts_[static_cast<std::size_t>(result.refusal)];
        return result;
    }
    if (nextCurrentSliceSequence_ == 0 || nextCurrentSliceSequence_ == UINT64_MAX) {
        result.status = ReserveStatus::Refused;
        result.refusal = ReserveRefusal::CommitSerialExhausted;
        ++refusalCounts_[static_cast<std::size_t>(result.refusal)];
        return result;
    }
    CurrentSliceRow& row =
        pageCurrentSlices_[base + pageCurrentSliceCount_[ring]++];
    row.executeFrame = frame;
    row.logicCommitSequence = input.logicCommitSequence;
    row.kind = input.kind;
    row.slice = input.slice;
    row.rowSequence = nextCurrentSliceSequence_++;
    row.executionOrdinal = input.executionOrdinal;
    row.publicationHeld = held != nullptr;
    if (held != nullptr) {
        ++pagePublicationHolds_[ring];
        held->owner_ = this;
        held->incarnation_ = currentSliceIncarnation_;
        held->epoch_ = epoch_;
        held->absoluteQuantum_ = pageHeaders_[ring].absoluteQuantumIndex;
        held->pageGeneration_ = pageHeaders_[ring].pageGeneration;
        held->rowSequence_ = row.rowSequence;
        held->executeFrame_ = frame;
        held->kind_ = row.kind;
        held->slice_ = row.slice;
    }
    result.status = ReserveStatus::Committed;
    result.firstFrame = frame;
    result.lastFrame = frame;
    return result;
}

bool ScheduledTransactionPool::HasHeldCurrentSlices() const noexcept {
    return std::any_of(pageCurrentSlices_.begin(), pageCurrentSlices_.end(),
        [](const CurrentSliceRow& row) { return row.publicationHeld; });
}

bool ScheduledTransactionPool::LocateHeldCurrentSlice(
    const HeldCurrentSliceToken& token, std::uint32_t* ring,
    std::uint32_t* index) const noexcept {
    // Terminal geometry remains intact so private reservations can roll back.
    if (token.owner_ != this || token.epoch_ != epoch_ ||
        token.incarnation_ != currentSliceIncarnation_ ||
        token.absoluteQuantum_ < baseQuantumIndex_ ||
        token.absoluteQuantum_ - baseQuantumIndex_ >= pageCount_) return false;
    const auto rowRing = RingIndex(static_cast<std::uint32_t>(
        token.absoluteQuantum_ - baseQuantumIndex_));
    const auto& header = pageHeaders_[rowRing];
    if (header.frozen || header.absoluteQuantumIndex != token.absoluteQuantum_ ||
        header.pageGeneration != token.pageGeneration_ ||
        pagePublicationHolds_[rowRing] == 0) return false;
    const auto base = static_cast<std::size_t>(rowRing) * currentSliceStorageCapacity_;
    for (std::uint32_t i = 0; i < pageCurrentSliceCount_[rowRing]; ++i) {
        const auto& row = pageCurrentSlices_[base + i];
        if (row.publicationHeld && row.rowSequence == token.rowSequence_ &&
            row.executeFrame == token.executeFrame_ &&
            row.slice.slot == token.slice_.slot &&
            row.slice.generation == token.slice_.generation) {
            *ring = rowRing;
            *index = i;
            return true;
        }
    }
    return false;
}

bool ScheduledTransactionPool::CanReleaseHeldCurrentSlice(
    const HeldCurrentSliceToken& token) const noexcept {
    std::uint32_t ring = 0, index = 0;
    return Valid() && LocateHeldCurrentSlice(token, &ring, &index);
}

bool ScheduledTransactionPool::ReleaseHeldCurrentSlice(HeldCurrentSliceToken* token) noexcept {
    std::uint32_t ring = 0, index = 0;
    if (!Valid() || token == nullptr || !LocateHeldCurrentSlice(*token, &ring, &index)) return false;
    const auto base = static_cast<std::size_t>(ring) * currentSliceStorageCapacity_;
    pageCurrentSlices_[base + index].publicationHeld = false;
    --pagePublicationHolds_[ring];
    token->owner_ = nullptr;
    return true;
}

bool ScheduledTransactionPool::CancelHeldCurrentSlice(HeldCurrentSliceToken* token) noexcept {
    std::uint32_t ring = 0, index = 0;
    if (token == nullptr || !LocateHeldCurrentSlice(*token, &ring, &index)) return false;
    const auto base = static_cast<std::size_t>(ring) * currentSliceStorageCapacity_;
    auto& count = pageCurrentSliceCount_[ring];
    for (std::uint32_t i = index + 1; i < count; ++i)
        pageCurrentSlices_[base + i - 1] = pageCurrentSlices_[base + i];
    pageCurrentSlices_[base + --count] = {};
    --pagePublicationHolds_[ring];
    token->owner_ = nullptr;
    return true;
}

HeldCurrentSliceToken::~HeldCurrentSliceToken() {
    if (owner_ != nullptr) (void)owner_->CancelHeldCurrentSlice(this);
}

HeldCurrentSliceToken::HeldCurrentSliceToken(HeldCurrentSliceToken&& other) noexcept {
    *this = std::move(other);
}

HeldCurrentSliceToken& HeldCurrentSliceToken::operator=(HeldCurrentSliceToken&& other) noexcept {
    if (this == &other) return *this;
    if (owner_ != nullptr && !owner_->CancelHeldCurrentSlice(this)) return *this;
    owner_ = std::exchange(other.owner_, nullptr);
    incarnation_ = other.incarnation_;
    epoch_ = other.epoch_;
    absoluteQuantum_ = other.absoluteQuantum_;
    pageGeneration_ = other.pageGeneration_;
    rowSequence_ = other.rowSequence_;
    executeFrame_ = other.executeFrame_;
    kind_ = other.kind_;
    slice_ = other.slice_;
    return *this;
}

Plan03GroupReserveStatus ScheduledTransactionPool::PreflightPlan03GroupSlices(
    std::uint64_t executeFrame,
    std::uint32_t additionalGroupCount) const noexcept {
    if (!Valid() || additionalGroupCount == 0 ||
        config_.maxPlan03GroupHeadersPerPage == 0) {
        return Plan03GroupReserveStatus::Invalid;
    }
    std::uint32_t page = 0;
    if (!PageForFrame(executeFrame, &page)) {
        return Plan03GroupReserveStatus::OutsideWindow;
    }
    const std::uint32_t ring = RingIndex(page);
    if (pageHeaders_[ring].frozen) {
        return Plan03GroupReserveStatus::PageFrozen;
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    std::uint32_t free = 0;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        if (pagePlan03GroupRowStates_[rowBase + group] ==
            Plan03GroupRowState::Free) {
            ++free;
        }
    }
    if (additionalGroupCount > free) {
        return Plan03GroupReserveStatus::PageGroupCapacity;
    }
    if (!HasUnplacedGroupBudget(additionalGroupCount)) {
        return Plan03GroupReserveStatus::PageGroupCapacity;
    }
    return Plan03GroupReserveStatus::Committed;
}

Plan03GroupReserveStatus ScheduledTransactionPool::ReservePlan03GroupSlice(
    const Plan03GroupSliceInput& input,
    const TimelineArmTable& armTable) noexcept {
    if (!Valid() || !input.decision.Valid() ||
        (input.memberCount != 0 && input.members == nullptr) ||
        input.sourceControlCount > input.memberCount ||
        config_.maxPlan03GroupHeadersPerPage == 0) {
        return Plan03GroupReserveStatus::Invalid;
    }
    if (plan03ArmTable_ != nullptr && plan03ArmTable_ != &armTable) {
        return Plan03GroupReserveStatus::Invalid;
    }
    if (input.memberCount > config_.maxArmsPerGroup) {
        return Plan03GroupReserveStatus::MemberCapacity;
    }
    std::uint32_t page = 0;
    if (!PageForFrame(input.executeFrame, &page)) {
        return Plan03GroupReserveStatus::OutsideWindow;
    }
    const std::uint32_t ring = RingIndex(page);
    if (pageHeaders_[ring].frozen) {
        return Plan03GroupReserveStatus::PageFrozen;
    }
    for (std::uint16_t inputIndex = 0; inputIndex < input.memberCount;
         ++inputIndex) {
        TimelineArmObservation observation{};
        if (!armTable.Observe(input.members[inputIndex], &observation) ||
            observation.parentGroup.decision.slot != input.decision.slot ||
            observation.parentGroup.decision.generation !=
                input.decision.generation) {
            return Plan03GroupReserveStatus::ArmIdentityMismatch;
        }
        for (std::uint16_t prior = 0; prior < inputIndex; ++prior) {
            TimelineArmObservation priorObservation{};
            if (!armTable.Observe(input.members[prior], &priorObservation)) {
                return Plan03GroupReserveStatus::ArmIdentityMismatch;
            }
            if (priorObservation.memberIndex == observation.memberIndex ||
                (input.members[prior].slot == input.members[inputIndex].slot &&
                 input.members[prior].generation ==
                     input.members[inputIndex].generation)) {
                return Plan03GroupReserveStatus::DuplicateMember;
            }
        }
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    std::size_t groupRow = pagePlan03Groups_.size();
    for (std::uint32_t existing = 0;
         existing < config_.maxPlan03GroupHeadersPerPage; ++existing) {
        const std::size_t rowIndex = rowBase + existing;
        const Plan03GroupRowState state =
            pagePlan03GroupRowStates_[rowIndex];
        if (state == Plan03GroupRowState::Free) {
            if (groupRow == pagePlan03Groups_.size()) {
                groupRow = rowIndex;
            }
            continue;
        }
        if (state == Plan03GroupRowState::Retired) {
            continue;
        }
        const Plan03GroupSliceRow& row =
            pagePlan03Groups_[rowIndex];
        if (row.sliceIndex == input.sliceIndex &&
            row.decision.slot == input.decision.slot &&
            row.decision.generation == input.decision.generation) {
            return Plan03GroupReserveStatus::DuplicateSlice;
        }
    }
    if (groupRow == pagePlan03Groups_.size() ||
        !HasUnplacedGroupBudget(1)) {
        return Plan03GroupReserveStatus::PageGroupCapacity;
    }
    auto& readers = pagePlan03GroupReaders_[groupRow];
    if (readers.pins.load(std::memory_order_acquire) != 0) {
        return Plan03GroupReserveStatus::Invalid;
    }
    readers.lastAudioSequence.store(0, std::memory_order_relaxed);
    const std::size_t memberBase =
        groupRow * config_.maxArmsPerGroup;
    for (std::uint16_t inputIndex = 0; inputIndex < input.memberCount;
         ++inputIndex) {
        TimelineArmObservation observation{};
        (void)armTable.Observe(input.members[inputIndex], &observation);
        std::uint16_t outputIndex = inputIndex;
        while (outputIndex > 0) {
            TimelineArmObservation priorObservation{};
            (void)armTable.Observe(
                pagePlan03GroupMembers_[memberBase + outputIndex - 1],
                &priorObservation);
            if (priorObservation.memberIndex < observation.memberIndex) {
                break;
            }
            pagePlan03GroupMembers_[memberBase + outputIndex] =
                pagePlan03GroupMembers_[memberBase + outputIndex - 1];
            --outputIndex;
        }
        pagePlan03GroupMembers_[memberBase + outputIndex] =
            input.members[inputIndex];
    }
    const Plan03GroupSliceIdentity memberOrigin{
        pageHeaders_[ring].absoluteQuantumIndex, pageHeaders_[ring].pageGeneration,
        input.decision, input.sliceIndex, groupRow,
        pagePlan03GroupClaimGenerations_[groupRow], epoch_};
    if (!IssueGroupMemberOriginals(memberOrigin, armTable,
            pagePlan03GroupMembers_.data() + memberBase, input.memberCount,
            pageGroupMemberOriginals_.get() + memberBase, pageGroupIdentities_[groupRow])) {
        std::fill(pagePlan03GroupMembers_.begin() + memberBase,
            pagePlan03GroupMembers_.begin() + memberBase + input.memberCount, TimelineArmToken{});
        return Plan03GroupReserveStatus::ArmIdentityMismatch;
    }
    Plan03GroupSliceRow& row = pagePlan03Groups_[groupRow];
    row.decision = input.decision;
    row.sliceIndex = input.sliceIndex;
    row.memberCount = input.memberCount;
    row.sourceControlCount = input.sourceControlCount;
    row.immediateEntry = input.immediateEntry;
    ++pagePlan03GroupCount_[ring];
    plan03ArmTable_ = &armTable;
    plan03ArmTableEstablishedByPrepared_ = false;
    pagePlan03GroupRowStates_[groupRow] = Plan03GroupRowState::Live;
    return Plan03GroupReserveStatus::Committed;
}

Plan03GroupReserveStatus ScheduledTransactionPool::PreparePlan03GroupSlice(
    const Plan03GroupSliceInput& input,
    const TimelineArmTable& armTable,
    PreparedPlan03GroupSlice* out,
    const PreparedPageGroupTransferScope* transferScope) noexcept {
    if (out == nullptr || out->Valid() || !Valid() ||
        !input.decision.Valid() ||
        (input.memberCount != 0 && input.members == nullptr) ||
        input.sourceControlCount > input.memberCount ||
        input.memberCount > config_.maxArmsPerGroup ||
        config_.maxPlan03GroupHeadersPerPage == 0 ||
        (plan03ArmTable_ != nullptr && plan03ArmTable_ != &armTable)) {
        return Plan03GroupReserveStatus::Invalid;
    }
    if (transferScope != nullptr && (!transferScope->Valid() || input.memberCount > 1 ||
        input.sourceControlCount != 0 || input.immediateEntry.gate != nullptr ||
        input.immediateEntry.generation != 0 || armTable.Incarnation() == 0))
        return Plan03GroupReserveStatus::Invalid;
    std::uint32_t page = 0;
    if (!PageForFrame(input.executeFrame, &page)) {
        return Plan03GroupReserveStatus::OutsideWindow;
    }
    const std::uint32_t ring = RingIndex(page);
    if (pageHeaders_[ring].frozen) {
        return Plan03GroupReserveStatus::PageFrozen;
    }
    for (std::uint16_t index = 0; index < input.memberCount; ++index) {
        TimelineArmObservation observation{};
        if ((transferScope != nullptr && armTable.Resolve(input.members[index]) == nullptr) ||
            !armTable.Observe(input.members[index], &observation) ||
            observation.parentGroup.decision.slot != input.decision.slot ||
            observation.parentGroup.decision.generation !=
                input.decision.generation) {
            return Plan03GroupReserveStatus::ArmIdentityMismatch;
        }
        for (std::uint16_t prior = 0; prior < index; ++prior) {
            TimelineArmObservation priorObservation{};
            if (!armTable.Observe(input.members[prior], &priorObservation) ||
                priorObservation.memberIndex == observation.memberIndex ||
                (input.members[prior].slot == input.members[index].slot &&
                 input.members[prior].generation ==
                     input.members[index].generation)) {
                return Plan03GroupReserveStatus::DuplicateMember;
            }
        }
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    std::size_t rowIndex = pagePlan03Groups_.size();
    for (std::uint32_t existing = 0;
         existing < config_.maxPlan03GroupHeadersPerPage; ++existing) {
        const std::size_t candidate = rowBase + existing;
        const Plan03GroupRowState state =
            pagePlan03GroupRowStates_[candidate];
        if (state == Plan03GroupRowState::Free) {
            if (rowIndex == pagePlan03Groups_.size()) {
                rowIndex = candidate;
            }
            continue;
        }
        if (state == Plan03GroupRowState::Retired) {
            continue;
        }
        const Plan03GroupSliceRow& row =
            pagePlan03Groups_[candidate];
        if (row.sliceIndex == input.sliceIndex &&
            row.decision.slot == input.decision.slot &&
            row.decision.generation == input.decision.generation) {
            return Plan03GroupReserveStatus::DuplicateSlice;
        }
    }
    if (rowIndex == pagePlan03Groups_.size() ||
        !HasUnplacedGroupBudget(1)) {
        return Plan03GroupReserveStatus::PageGroupCapacity;
    }
    auto& readers = pagePlan03GroupReaders_[rowIndex];
    if (readers.pins.load(std::memory_order_acquire) != 0) {
        return Plan03GroupReserveStatus::Invalid;
    }
    readers.lastAudioSequence.store(0, std::memory_order_relaxed);
    const std::size_t memberBase = rowIndex * config_.maxArmsPerGroup;
    for (std::uint16_t inputIndex = 0; inputIndex < input.memberCount;
         ++inputIndex) {
        TimelineArmObservation observation{};
        (void)armTable.Observe(input.members[inputIndex], &observation);
        std::uint16_t outputIndex = inputIndex;
        while (outputIndex > 0) {
            TimelineArmObservation priorObservation{};
            (void)armTable.Observe(
                pagePlan03GroupMembers_[memberBase + outputIndex - 1],
                &priorObservation);
            if (priorObservation.memberIndex < observation.memberIndex) {
                break;
            }
            pagePlan03GroupMembers_[memberBase + outputIndex] =
                pagePlan03GroupMembers_[memberBase + outputIndex - 1];
            --outputIndex;
        }
        pagePlan03GroupMembers_[memberBase + outputIndex] =
            input.members[inputIndex];
    }
    const Plan03GroupSliceIdentity memberOrigin{
        pageHeaders_[ring].absoluteQuantumIndex, pageHeaders_[ring].pageGeneration,
        input.decision, input.sliceIndex, rowIndex,
        pagePlan03GroupClaimGenerations_[rowIndex], epoch_};
    if (!IssueGroupMemberOriginals(memberOrigin, armTable,
            pagePlan03GroupMembers_.data() + memberBase, input.memberCount,
            pageGroupMemberOriginals_.get() + memberBase, pageGroupIdentities_[rowIndex])) {
        std::fill(pagePlan03GroupMembers_.begin() + memberBase,
            pagePlan03GroupMembers_.begin() + memberBase + input.memberCount, TimelineArmToken{});
        return Plan03GroupReserveStatus::ArmIdentityMismatch;
    }
    Plan03GroupSliceRow& row = pagePlan03Groups_[rowIndex];
    row.decision = input.decision;
    row.sliceIndex = input.sliceIndex;
    row.memberCount = input.memberCount;
    row.sourceControlCount = input.sourceControlCount;
    row.immediateEntry = input.immediateEntry;
    if (plan03ArmTable_ == nullptr) {
        plan03ArmTable_ = &armTable;
        plan03ArmTableEstablishedByPrepared_ = true;
    }
    pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Prepared;
    ++pagePreparedPlan03GroupCount_[ring];
    out->owner_ = this;
    out->rowIndex_ = rowIndex;
    out->generation_ = pagePlan03GroupClaimGenerations_[rowIndex];
    out->prepared_ = true;
    if (transferScope != nullptr) {
        out->transferSelected_ = true;
        row.transferOriginal = out;
        row.transferPreparedIdentity = {pageHeaders_[ring].absoluteQuantumIndex,
            pageHeaders_[ring].pageGeneration, input.decision, input.sliceIndex,
            rowIndex, out->generation_, epoch_};
        row.transferProspectiveIdentity = row.transferPreparedIdentity;
        if (out->generation_ != UINT32_MAX) ++row.transferProspectiveIdentity.rowClaimGeneration;
        row.transferScope = *transferScope;
        row.transferExecuteFrame = input.executeFrame;
        row.transferArmTable = &armTable;
        row.transferArmTableIncarnation = armTable.Incarnation();
        if (input.memberCount == 1) {
            TimelineArmObservation member{};
            (void)armTable.Observe(input.members[0], &member);
            row.transferMember = input.members[0];
            row.transferMemberIndex = member.memberIndex;
        }
    }
    return Plan03GroupReserveStatus::Committed;
}

bool ScheduledTransactionPool::CancelPreparedPlan03GroupSlice(
    PreparedPlan03GroupSlice* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ ||
        prepared->rowIndex_ >= pagePlan03Groups_.size()) {
        return false;
    }
    const std::size_t rowIndex = prepared->rowIndex_;
    if (pagePlan03Groups_[rowIndex].transferProof != nullptr ||
        (prepared->transferSelected_ && !OwnsPreparedPageGroupOriginal(*prepared))) return false;
    if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Prepared ||
        pagePlan03GroupClaimGenerations_[rowIndex] !=
            prepared->generation_) {
        return false;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        rowIndex / config_.maxPlan03GroupHeadersPerPage);
    if (pagePreparedPlan03GroupCount_[ring] == 0) {
        return false;
    }
    DetachPreparedPageGroupTransfer(rowIndex);
    --pagePreparedPlan03GroupCount_[ring];
    const std::uint16_t memberCount =
        pagePlan03Groups_[rowIndex].memberCount;
    const std::size_t memberBase = rowIndex * config_.maxArmsPerGroup;
    ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + memberBase, memberCount);
    std::fill(pagePlan03GroupMembers_.begin() + memberBase,
              pagePlan03GroupMembers_.begin() + memberBase + memberCount,
              TimelineArmToken{});
    pagePlan03Groups_[rowIndex] = {};
    if (pagePlan03GroupClaimGenerations_[rowIndex] == UINT32_MAX) {
        pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Retired;
    } else {
        ++pagePlan03GroupClaimGenerations_[rowIndex];
        pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Free;
    }
    bool anyPlan03 = false;
    for (std::size_t index = 0; index < pagePlan03GroupRowStates_.size();
         ++index) {
        if (pagePlan03GroupRowStates_[index] == Plan03GroupRowState::Live ||
            pagePlan03GroupRowStates_[index] ==
                Plan03GroupRowState::Prepared) {
            anyPlan03 = true;
            break;
        }
    }
    if (!anyPlan03 && plan03ArmTableEstablishedByPrepared_) {
        plan03ArmTable_ = nullptr;
        plan03ArmTableEstablishedByPrepared_ = false;
    }
    prepared->owner_ = nullptr;
    prepared->rowIndex_ = SIZE_MAX;
    prepared->generation_ = 0;
    prepared->prepared_ = false;
    return true;
}

bool ScheduledTransactionPool::CommitPreparedPlan03GroupSlice(
    PreparedPlan03GroupSlice* prepared) noexcept {
    return CommitPreparedPlan03GroupSlice(prepared, nullptr);
}

bool ScheduledTransactionPool::CommitPreparedPlan03GroupSlice(
    PreparedPlan03GroupSlice* prepared,
    CommittedPlan03GroupSlice* committed, bool retainPublicationHold) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ ||
        prepared->rowIndex_ >= pagePlan03Groups_.size() ||
        (committed != nullptr && (committed->Valid() || committed->Held())) ||
        (retainPublicationHold && committed == nullptr)) {
        return false;
    }
    const std::size_t rowIndex = prepared->rowIndex_;
    if (pagePlan03Groups_[rowIndex].transferProof != nullptr ||
        (prepared->transferSelected_ && !OwnsPreparedPageGroupOriginal(*prepared))) return false;
    if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Prepared ||
        pagePlan03GroupClaimGenerations_[rowIndex] !=
            prepared->generation_) {
        return false;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        rowIndex / config_.maxPlan03GroupHeadersPerPage);
    if (pagePreparedPlan03GroupCount_[ring] == 0) {
        return false;
    }
    DetachPreparedPageGroupTransfer(rowIndex);
    --pagePreparedPlan03GroupCount_[ring];
    ++pagePlan03GroupCount_[ring];
    if (retainPublicationHold) {
        ++pagePublicationHolds_[ring];
        pagePlan03Groups_[rowIndex].publicationHeld = true;
    }
    pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Live;
    if (pagePlan03GroupClaimGenerations_[rowIndex] != UINT32_MAX) {
        ++pagePlan03GroupClaimGenerations_[rowIndex];
    }
    if (pagePlan03Groups_[rowIndex].memberCount != 0)
        pageGroupIdentities_[rowIndex].group.rowClaimGeneration = pagePlan03GroupClaimGenerations_[rowIndex];
    if (committed != nullptr) {
        committed->owner_ = this;
        committed->rowIndex_ = rowIndex;
        committed->generation_ =
            pagePlan03GroupClaimGenerations_[rowIndex];
        committed->privateFactory_ = false;
        committed->privateExecuteFrame_ = 0;
        committed->privateArmTableIncarnation_ = 0;
    }
    plan03ArmTableEstablishedByPrepared_ = false;
    prepared->owner_ = nullptr;
    prepared->rowIndex_ = SIZE_MAX;
    prepared->generation_ = 0;
    prepared->prepared_ = false;
    return true;
}

bool ScheduledTransactionPool::MatchesCommittedPlan03GroupSlice(
    const CommittedPlan03GroupSlice& committed, const Plan03GroupSliceIdentity& identity) const noexcept {
    if (!Valid() || config_.maxPlan03GroupHeadersPerPage == 0 ||
        committed.owner_ != this || committed.rowIndex_ >= pagePlan03Groups_.size() ||
        identity.backing != Plan03GroupBacking::Page || !identity.Exact() ||
        identity.committedRow != committed.rowIndex_ || identity.rowClaimGeneration != committed.generation_ ||
        pagePlan03GroupRowStates_[committed.rowIndex_] != Plan03GroupRowState::Live ||
        pagePlan03GroupClaimGenerations_[committed.rowIndex_] != committed.generation_) return false;
    const auto& row = pagePlan03Groups_[committed.rowIndex_];
    const auto ring = committed.rowIndex_ / config_.maxPlan03GroupHeadersPerPage;
    const auto& page = pageHeaders_[ring];
    return identity.transportEpoch == epoch_ && page.transportEpoch == epoch_ &&
        identity.absoluteQuantum == page.absoluteQuantumIndex && identity.pageGeneration == page.pageGeneration &&
        identity.decision.slot == row.decision.slot && identity.decision.generation == row.decision.generation &&
        identity.sliceIndex == row.sliceIndex;
}

bool ScheduledTransactionPool::RollbackCommittedPlan03GroupSlice(
    CommittedPlan03GroupSlice* committed) noexcept {
    if (committed == nullptr || committed->privateRetirement_ != nullptr ||
        committed->frontierBindings_ != nullptr || committed->jointCancellation_ != nullptr || committed->owner_ != this ||
        committed->rowIndex_ >= pagePlan03Groups_.size()) {
        return false;
    }
    const std::size_t rowIndex = committed->rowIndex_;
    if (pagePlan03GroupReaders_[rowIndex].pins.load(std::memory_order_acquire) != 0) {
        return false;
    }
    if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live ||
        pagePlan03GroupClaimGenerations_[rowIndex] !=
            committed->generation_) {
        return false;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        rowIndex / config_.maxPlan03GroupHeadersPerPage);
    const auto& row = pagePlan03Groups_[rowIndex];
    const Plan03GroupSliceIdentity current{
        pageHeaders_[ring].absoluteQuantumIndex,
        pageHeaders_[ring].pageGeneration, row.decision, row.sliceIndex,
        rowIndex, pagePlan03GroupClaimGenerations_[rowIndex], epoch_};
    if (IsUnpublishedRetirementParticipant(current) &&
        row.cancellationOriginal != committed) return false;
    if (pageHeaders_[ring].frozen || pagePlan03GroupCount_[ring] == 0) {
        return false;
    }
    const AudioSliceState leaseState =
        pagePlan03GroupLeases_[rowIndex].Capture().state;
    if (leaseState != AudioSliceState::Ready &&
        !pagePlan03GroupLeases_[rowIndex].ResetForReuse()) {
        return false;
    }
    --pagePlan03GroupCount_[ring];
    if (pagePlan03Groups_[rowIndex].publicationHeld) --pagePublicationHolds_[ring];
    const std::uint16_t memberCount =
        pagePlan03Groups_[rowIndex].memberCount;
    const std::size_t memberBase = rowIndex * config_.maxArmsPerGroup;
    ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + memberBase, memberCount);
    std::fill(pagePlan03GroupMembers_.begin() + memberBase,
              pagePlan03GroupMembers_.begin() + memberBase + memberCount,
              TimelineArmToken{});
    pagePlan03Groups_[rowIndex] = {};
    if (pagePlan03GroupClaimGenerations_[rowIndex] == UINT32_MAX) {
        pagePlan03GroupRowStates_[rowIndex] =
            Plan03GroupRowState::Retired;
    } else {
        ++pagePlan03GroupClaimGenerations_[rowIndex];
        pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Free;
    }
    committed->owner_ = nullptr;
    committed->rowIndex_ = SIZE_MAX;
    committed->generation_ = 0;
    committed->privateFactory_ = false;
    committed->privateExecuteFrame_ = 0;
    committed->privateArmTableIncarnation_ = 0;
    committed->cancellationPending_ = false;
    return true;
}

bool ScheduledTransactionPool::CanReleaseCommittedPlan03GroupSliceOwnership(
    const CommittedPlan03GroupSlice& committed) const noexcept {
    return !committed.Held() && committed.owner_ == this &&
        committed.rowIndex_ < pagePlan03Groups_.size() &&
        pagePlan03GroupRowStates_[committed.rowIndex_] == Plan03GroupRowState::Live &&
        pagePlan03GroupClaimGenerations_[committed.rowIndex_] == committed.generation_;
}

bool ScheduledTransactionPool::ReleaseCommittedPlan03GroupSliceOwnership(
    CommittedPlan03GroupSlice* committed) noexcept {
    if (committed == nullptr || !CanReleaseCommittedPlan03GroupSliceOwnership(*committed)) return false;
    const std::size_t rowIndex = committed->rowIndex_;
    if (pagePlan03Groups_[rowIndex].publicationHeld) {
        const auto ring = rowIndex / config_.maxPlan03GroupHeadersPerPage;
        --pagePublicationHolds_[ring];
        pagePlan03Groups_[rowIndex].publicationHeld = false;
    }
    committed->owner_ = nullptr;
    committed->rowIndex_ = SIZE_MAX;
    committed->generation_ = 0;
    committed->privateFactory_ = false;
    committed->privateExecuteFrame_ = 0;
    committed->privateArmTableIncarnation_ = 0;
    return true;
}

Plan03GroupReserveStatus
ScheduledTransactionPool::PreparePlan03GroupSliceReplacement(
    const Plan03GroupSliceIdentity& expected,
    const Plan03GroupSliceInput& replacement,
    const TimelineArmTable& armTable,
    PreparedPlan03GroupSliceReplacement* out) noexcept {
    if (out == nullptr || out->Valid() || !expected.Valid() ||
        !replacement.decision.Valid() ||
        replacement.sliceIndex != expected.sliceIndex ||
        (replacement.memberCount != 0 && replacement.members == nullptr) ||
        replacement.sourceControlCount > replacement.memberCount ||
        replacement.memberCount > config_.maxArmsPerGroup ||
        plan03ArmTable_ != &armTable) {
        return Plan03GroupReserveStatus::Invalid;
    }
    std::uint32_t page = 0;
    if (!PageForFrame(replacement.executeFrame, &page) ||
        replacement.executeFrame / quantumFrames_ !=
            expected.absoluteQuantum) {
        return Plan03GroupReserveStatus::OutsideWindow;
    }
    const std::uint32_t ring = RingIndex(page);
    if (pageHeaders_[ring].frozen) {
        return Plan03GroupReserveStatus::PageFrozen;
    }
    if (pageHeaders_[ring].pageGeneration != expected.pageGeneration) {
        return Plan03GroupReserveStatus::OutsideWindow;
    }
    for (std::uint16_t index = 0; index < replacement.memberCount;
         ++index) {
        TimelineArmObservation observation{};
        if (!armTable.Observe(replacement.members[index], &observation) ||
            observation.parentGroup.decision.slot !=
                replacement.decision.slot ||
            observation.parentGroup.decision.generation !=
                replacement.decision.generation) {
            return Plan03GroupReserveStatus::ArmIdentityMismatch;
        }
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        const std::size_t rowIndex = rowBase + group;
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) {
            continue;
        }
        const Plan03GroupSliceRow& row = pagePlan03Groups_[rowIndex];
        if (!MatchesPlan03GroupRow(expected, rowIndex) ||
            row.sliceIndex != expected.sliceIndex ||
            row.decision.slot != expected.decision.slot ||
            row.decision.generation != expected.decision.generation) {
            continue;
        }
        if (row.publicationHeld ||
            pagePlan03GroupReaders_[rowIndex].pins.load(std::memory_order_acquire) != 0)
            return Plan03GroupReserveStatus::PageFrozen;
        const Plan03GroupSliceIdentity current{
            pageHeaders_[ring].absoluteQuantumIndex,
            pageHeaders_[ring].pageGeneration, row.decision, row.sliceIndex,
            rowIndex, pagePlan03GroupClaimGenerations_[rowIndex], epoch_};
        if (IsUnpublishedRetirementParticipant(current) ||
            !pagePlan03GroupLeases_[rowIndex].PrepareReplacement()) {
            return Plan03GroupReserveStatus::PageFrozen;
        }
        out->owner_ = this;
        out->rowIndex_ = rowIndex;
        out->expected_ = current;
        out->replacement_ = replacement;
        out->prepared_ = true;
        return Plan03GroupReserveStatus::Committed;
    }
    return Plan03GroupReserveStatus::Invalid;
}

bool ScheduledTransactionPool::CommitPreparedPlan03GroupSliceReplacement(
    PreparedPlan03GroupSliceReplacement* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ ||
        prepared->rowIndex_ >= pagePlan03Groups_.size() ||
        pagePlan03GroupRowStates_[prepared->rowIndex_] !=
            Plan03GroupRowState::Live) {
        return false;
    }
    Plan03GroupSliceRow& row =
        pagePlan03Groups_[prepared->rowIndex_];
    if (row.sliceIndex != prepared->expected_.sliceIndex ||
        row.decision.slot != prepared->expected_.decision.slot ||
        row.decision.generation !=
            prepared->expected_.decision.generation) {
        return false;
    }
    Plan03ExactGroupSnapshot current{};
    if (!prepared->expected_.Exact() ||
        !CaptureExactPlan03Group(prepared->expected_, &current) ||
        IsUnpublishedRetirementParticipant(prepared->expected_)) return false;
    const Plan03GroupSliceInput& replacement = prepared->replacement_;
    const std::size_t memberBase =
        prepared->rowIndex_ * config_.maxArmsPerGroup;
    auto replacementOrigin = prepared->expected_;
    replacementOrigin.decision = replacement.decision;
    replacementOrigin.sliceIndex = replacement.sliceIndex;
    if (plan03ArmTable_ == nullptr ||
        !IssueGroupMemberOriginals(replacementOrigin, *plan03ArmTable_,
            replacement.members, replacement.memberCount, replacementGroupMemberOriginals_.get(), replacementGroupIdentity_))
        return false;
    if (!pagePlan03GroupLeases_[prepared->rowIndex_].CommitPreparedReplacement()) {
        ReturnGroupMemberOriginals(replacementGroupMemberOriginals_.get(), replacement.memberCount);
        return false;
    }
    ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + memberBase, row.memberCount);
    auto& pageIdentity = pageGroupIdentities_[prepared->rowIndex_];
    assert(pageIdentity.liveMembers == 0);
    // Move the shared origin out of reusable replacement scratch before releasing it.
    pageIdentity = replacementGroupIdentity_;
    for (std::uint16_t index = 0; index < replacement.memberCount; ++index) {
        pagePlan03GroupMembers_[memberBase + index] = replacement.members[index];
        pageGroupMemberOriginals_[memberBase + index] = replacementGroupMemberOriginals_[index];
        pageGroupMemberOriginals_[memberBase + index].groupIdentity_ = &pageIdentity;
        replacementGroupMemberOriginals_[index] = {};
    }
    replacementGroupIdentity_ = {};
    row.decision = replacement.decision;
    row.sliceIndex = replacement.sliceIndex;
    row.memberCount = replacement.memberCount;
    row.sourceControlCount = replacement.sourceControlCount;
    prepared->owner_ = nullptr;
    prepared->rowIndex_ = 0;
    prepared->expected_ = {};
    prepared->replacement_ = {};
    prepared->prepared_ = false;
    return true;
}

bool ScheduledTransactionPool::CancelPreparedPlan03GroupSliceReplacement(
    PreparedPlan03GroupSliceReplacement* prepared) noexcept {
    Plan03ExactGroupSnapshot current{};
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ ||
        prepared->rowIndex_ >= pagePlan03Groups_.size() ||
        pagePlan03GroupRowStates_[prepared->rowIndex_] !=
            Plan03GroupRowState::Live ||
        !prepared->expected_.Exact() ||
        !CaptureExactPlan03Group(prepared->expected_, &current) ||
        !pagePlan03GroupLeases_[prepared->rowIndex_]
             .CancelPreparedReplacement()) {
        return false;
    }
    prepared->owner_ = nullptr;
    prepared->rowIndex_ = 0;
    prepared->expected_ = {};
    prepared->replacement_ = {};
    prepared->prepared_ = false;
    return true;
}

bool ScheduledTransactionPool::TransactionValid(
    TransactionRecord& record, TransportEpoch currentEpoch,
    const PlayableInstanceTable& instances,
    const OccurrenceStatePool& occurrences) const {
    if (record.cacheTick == consumeTick_) {
        return record.cacheValid;
    }
    bool ok = record.epoch != kInvalidTransportEpoch &&
              record.epoch == currentEpoch;
    if (ok) {
        ok = instances.ValidateStamp(record.ownerInstance,
                                     record.ownerScheduleEpoch);
    }
    if (ok && record.occurrence.present) {
        ok = ValidateTransactionOccurrence(
            record.occurrence, record.ownerInstance,
            record.ownerScheduleEpoch, instances, occurrences);
    }
    record.cacheTick = consumeTick_;
    record.cacheValid = ok;
    return ok;
}

std::size_t ScheduledTransactionPool::ConsumePage(
    std::uint32_t page, TransportEpoch currentEpoch,
    const PlayableInstanceTable& instances,
    const OccurrenceStatePool& occurrences, ConsumedEvent* out,
    std::size_t outCapacity) {
    if (!Valid() || page >= pageCount_ || out == nullptr) {
        return 0;
    }
    const std::uint32_t ring = RingIndex(page);
    SortPage(ring);
    ++consumeTick_; // a fresh memo epoch for the per-transaction stale cache

    const std::uint32_t count = pageEventCount_[ring];
    const std::size_t eventBase =
        static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
    const std::uint32_t* order = &pageEventOrder_[eventBase];
    std::size_t written = 0;
    for (std::uint32_t k = 0; k < count && written < outCapacity; ++k) {
        const EventRow& row = pageEvents_[eventBase + order[k]];
        TransactionRecord& rec = transactions_[row.txnSlot];
        // The row's slot is only reclaimed after its last event leaves, so a live
        // row always names its own live transaction; the id compare is a
        // defensive guard, not a correctness dependency.
        if (!rec.used || rec.id != row.transactionId) {
            continue;
        }
        if (!TransactionValid(rec, currentEpoch, instances, occurrences)) {
            continue; // whole transaction stale: skip every event it filed
        }
        ConsumedEvent& e = out[written++];
        e.executeFrame = row.executeFrame;
        e.logicCommitSequence = row.logicCommitSequence;
        e.executionOrdinal = row.executionOrdinal;
        e.eventIndex = row.eventIndex;
        e.transactionOrdinal = row.transactionOrdinal;
        e.subeventPhase = row.subeventPhase;
        e.rowSequence = row.rowSequence;
        e.payload = row.payload;
        e.authoredFadeOutCandidate = row.authoredFadeOutCandidate;
        e.transactionId = row.transactionId;
        e.program = rec.program;
        e.armToken = row.armToken.Valid() ? row.armToken : rec.armToken;
        e.txnSlot = row.txnSlot;
        e.frozenTxnValid = row.frozenTxnValid;
        e.ownerInstanceHandle = rec.ownerInstance;
        e.ownerInstanceScheduleEpoch = rec.ownerScheduleEpoch;
        e.frontier = row.frontier;
        e.immediateEntry = rec.immediateEntry;
    }
    return written;
}

std::uint64_t ScheduledTransactionPool::PageStamp(
    std::uint64_t absoluteQuantum) const noexcept {
    if (!Valid() || absoluteQuantum < sessionBaseQuantumIndex_ ||
        pageCount_ == 0) {
        return kUnpublishedQuantumStamp;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        (absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    return pageQuantumStamp_[ring].load(std::memory_order_acquire);
}

bool ScheduledTransactionPool::VerifyImmediateEntryPage(
    std::uint64_t absoluteQuantum, ImmediateEntryAttemptRef entry,
    std::uint32_t expectedEvents,
    std::uint32_t expectedGroups) const noexcept {
    if (!entry.Valid() ||
        entry.gate->Observe(entry.generation) !=
            ImmediateEntryAttemptState::Finalizing ||
        PageStamp(absoluteQuantum) != absoluteQuantum) return false;
    const auto ring = static_cast<std::uint32_t>(
        (absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    const auto eventBase = static_cast<std::size_t>(ring) *
        config_.maxEventsPerQuantum;
    std::uint32_t events = 0;
    for (std::uint32_t index = 0; index < pageEventCount_[ring]; ++index) {
        const auto& row = pageEvents_[eventBase + index];
        const auto& owner = transactions_[row.txnSlot].immediateEntry;
        if (owner.gate != entry.gate ||
            owner.generation != entry.generation) continue;
        if (!row.frozenTxnValid) return false;
        ++events;
    }
    const auto groupBase = static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    std::uint32_t groups = 0;
    for (std::uint32_t index = 0;
         index < config_.maxPlan03GroupHeadersPerPage; ++index) {
        const auto rowIndex = groupBase + index;
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) continue;
        const auto& owner = pagePlan03Groups_[rowIndex].immediateEntry;
        if (owner.gate != entry.gate ||
            owner.generation != entry.generation) continue;
        const auto* lease = EffectivePlan03GroupLease(rowIndex);
        if (lease == nullptr ||
            lease->Capture().state == AudioSliceState::SliceInvalidated) {
            return false;
        }
        ++groups;
    }
    return events == expectedEvents && groups == expectedGroups &&
        entry.gate->Observe(entry.generation) ==
            ImmediateEntryAttemptState::Finalizing;
}

std::uint64_t ScheduledTransactionPool::PublishedEmptyPrefixEnd(
    std::uint64_t frontier, std::uint64_t end) const noexcept {
    if (!Valid() || frontier < sessionBaseQuantumIndex_ || pageCount_ == 0)
        return frontier;
    auto page = frontier;
    for (std::uint32_t scanned = 0; scanned < pageCount_ && page < end; ++scanned) {
        const auto ring = static_cast<std::uint32_t>(
            (page - sessionBaseQuantumIndex_) % pageCount_);
        if (pageQuantumStamp_[ring].load(std::memory_order_acquire) != page ||
            pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire) != page ||
            pageQuantumStamp_[ring].load(std::memory_order_acquire) != page)
            break;
        ++page;
    }
    return page;
}

bool ScheduledTransactionPool::TryAdvancePublishedEmptyPrefixRt(
    std::uint64_t frontier, std::uint64_t end) noexcept {
    const auto next = PublishedEmptyPrefixEnd(frontier, end);
    return next > frontier && consumedFrontierQuantum_->compare_exchange_strong(
        frontier, next, std::memory_order_acq_rel, std::memory_order_acquire);
}

std::size_t ScheduledTransactionPool::ConsumePageRt(
    std::uint64_t absoluteQuantum, ConsumedEvent* out,
    std::size_t outCapacity) const noexcept {
    if (!Valid() || out == nullptr ||
        PageStamp(absoluteQuantum) != absoluteQuantum) {
        return 0;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        (absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    const std::uint32_t count = pageEventCount_[ring];
    const std::size_t base =
        static_cast<std::size_t>(ring) * config_.maxEventsPerQuantum;
    const std::uint32_t* order = &pageEventOrder_[base];
    std::size_t written = 0;
    for (std::uint32_t i = 0; i < count && written < outCapacity; ++i) {
        const EventRow& row = pageEvents_[base + order[i]];
        const TransactionRecord& transaction = transactions_[row.txnSlot];
        if (transaction.immediateEntry.Valid() &&
            !transaction.immediateEntry.gate->AdmitAudio(
                transaction.immediateEntry.generation)) continue;
        ConsumedEvent& event = out[written++];
        event.executeFrame = row.executeFrame;
        event.logicCommitSequence = row.logicCommitSequence;
        event.executionOrdinal = row.executionOrdinal;
        event.eventIndex = row.eventIndex;
        event.transactionOrdinal = row.transactionOrdinal;
        event.subeventPhase = row.subeventPhase;
        event.rowSequence = row.rowSequence;
        event.payload = row.payload;
        event.authoredFadeOutCandidate = row.authoredFadeOutCandidate;
        event.transactionId = row.transactionId;
        event.program = transaction.program;
        event.armToken = row.armToken.Valid()
            ? row.armToken
            : transaction.armToken;
        event.txnSlot = row.txnSlot;
        event.frozenTxnValid = row.frozenTxnValid;
        event.ownerInstanceHandle = transaction.ownerInstance;
        event.ownerInstanceScheduleEpoch = transaction.ownerScheduleEpoch;
        event.frontier = row.frontier;
        event.immediateEntry = transaction.immediateEntry;
    }
    return written;
}

std::size_t ScheduledTransactionPool::ConsumeCurrentSlicesRt(
    std::uint64_t absoluteQuantum, ConsumedCurrentSlice* out,
    std::size_t outCapacity) const noexcept {
    if (!Valid() || PageStamp(absoluteQuantum) != absoluteQuantum) {
        return 0;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        (absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    const std::uint32_t count = pageCurrentSliceCount_[ring];
    if (count > outCapacity || (count != 0 && out == nullptr)) {
        return count;
    }
    const std::size_t base =
        static_cast<std::size_t>(ring) *
        currentSliceStorageCapacity_;
    const std::uint32_t* order = &pageCurrentSliceOrder_[base];
    for (std::uint32_t i = 0; i < count; ++i) {
        const CurrentSliceRow& row = pageCurrentSlices_[base + order[i]];
        out[i] = {row.executeFrame, row.logicCommitSequence, row.kind,
                  row.slice, row.executionOrdinal, row.rowSequence};
    }
    return count;
}

std::size_t ScheduledTransactionPool::CapturePlan03PageGroupsRt(
    std::uint64_t absoluteQuantum, Plan03PageGroupView* out,
    std::size_t outCapacity) const noexcept {
    if (!Valid() || PageStamp(absoluteQuantum) != absoluteQuantum) {
        return 0;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        (absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    const std::uint32_t count = pagePlan03GroupCount_[ring];
    if (count > outCapacity || (count != 0 && out == nullptr)) {
        return count;
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    std::uint32_t written = 0;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        const std::size_t rowIndex = rowBase + group;
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) {
            continue;
        }
        const Plan03GroupSliceRow& row = pagePlan03Groups_[rowIndex];
        if (row.immediateEntry.Valid() &&
            !row.immediateEntry.gate->AdmitAudio(
                row.immediateEntry.generation)) continue;
        out[written].identity = {absoluteQuantum,
                                 pageHeaders_[ring].pageGeneration,
                                 row.decision, row.sliceIndex, rowIndex,
                                 pagePlan03GroupClaimGenerations_[rowIndex], epoch_};
        out[written].memberCount = row.memberCount;
        out[written].sourceControlCount = row.sourceControlCount;
        out[written].members = row.memberCount == 0
            ? nullptr
            : &pagePlan03GroupMembers_[rowIndex * config_.maxArmsPerGroup];
        out[written].lease = EffectivePlan03GroupLease(rowIndex);
        out[written].frontier = row.frontier.role == FrontierGroupRole::Legacy ? nullptr : &row.frontier;
        out[written].immediateEntry = row.immediateEntry;
        ++written;
    }
    return written;
}

std::size_t ScheduledTransactionPool::CaptureLivePlan03Groups(
    Plan03PageGroupView* out, std::size_t outCapacity) const noexcept {
    if (!Valid() || pageCount_ == 0 ||
        (config_.maxPlan03GroupHeadersPerPage == 0 &&
         config_.maxImmediateLaneGroupRows == 0)) {
        return 0;
    }
    std::size_t needed = 0;
    std::size_t written = 0;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        const std::size_t rowBase =
            static_cast<std::size_t>(ring) *
            config_.maxPlan03GroupHeadersPerPage;
        for (std::uint32_t group = 0;
             group < config_.maxPlan03GroupHeadersPerPage; ++group) {
            const std::size_t rowIndex = rowBase + group;
            const Plan03GroupRowState state =
                pagePlan03GroupRowStates_[rowIndex];
            if (state != Plan03GroupRowState::Live &&
                state != Plan03GroupRowState::Prepared) {
                continue;
            }
            ++needed;
            if (out == nullptr || written >= outCapacity) {
                continue;
            }
            const Plan03GroupSliceRow& row = pagePlan03Groups_[rowIndex];
            out[written].identity = {pageHeaders_[ring].absoluteQuantumIndex,
                                     pageHeaders_[ring].pageGeneration,
                                     row.decision, row.sliceIndex, rowIndex,
                                     pagePlan03GroupClaimGenerations_[rowIndex], epoch_};
            out[written].memberCount = row.memberCount;
            out[written].sourceControlCount = row.sourceControlCount;
            out[written].members = row.memberCount == 0
                ? nullptr
                : &pagePlan03GroupMembers_[rowIndex * config_.maxArmsPerGroup];
            out[written].lease = EffectivePlan03GroupLease(rowIndex);
            out[written].frontier = row.frontier.role == FrontierGroupRole::Legacy ? nullptr : &row.frontier;
            ++written;
        }
    }
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        const auto state = row.state.load(std::memory_order_acquire);
        if (state != LaneGroupState::Private && state != LaneGroupState::Published)
            continue;
        ++needed;
        if (out == nullptr || written >= outCapacity) continue;
        const Plan03GroupSliceIdentity identity{
            0, 1, row.decision, row.sliceIndex, index, row.generation,
            epoch_, Plan03GroupBacking::ImmediateLane};
        if (!ResolveImmediateLaneGroup(identity, &out[written], false))
            return outCapacity + 1u;
        ++written;
    }
    return needed > outCapacity ? needed : written;
}

bool ScheduledTransactionPool::CaptureExactPlan03Group(
    const Plan03GroupSliceIdentity& identity,
    Plan03ExactGroupSnapshot* out) const noexcept {
    Plan03PageGroupView view{};
    if (out == nullptr || identity.backing != Plan03GroupBacking::Page ||
        !identity.Exact() || !ResolvePlan03GroupSliceProducer(identity, &view) ||
        !(view.identity == identity) || view.lease == nullptr) return false;
    const auto ring = identity.committedRow / config_.maxPlan03GroupHeadersPerPage;
    const auto& page = pageHeaders_[ring];
    if (page.absoluteQuantumIndex != identity.absoluteQuantum ||
        page.pageGeneration != identity.pageGeneration ||
        page.transportEpoch != identity.transportEpoch) return false;
    const auto& row = pagePlan03Groups_[identity.committedRow];
    const auto& readers = pagePlan03GroupReaders_[identity.committedRow];
    Plan03ExactGroupSnapshot snapshot{};
    snapshot.identity = identity;
    snapshot.page = page;
    snapshot.pageStamp = pageQuantumStamp_[ring].load(std::memory_order_acquire);
    snapshot.emptyPageStamp = pageEmptyQuantumStamp_[ring].load(std::memory_order_acquire);
    snapshot.globalPublishCutoffFrame = publishCutoffFrame_;
    snapshot.livePageGroupCount = pagePlan03GroupCount_[ring];
    snapshot.pagePublicationHolds = pagePublicationHolds_[ring];
    snapshot.readerPins = readers.pins.load(std::memory_order_acquire);
    snapshot.lastAudioSequence = readers.lastAudioSequence.load(std::memory_order_acquire);
    snapshot.frontierDependents = row.frontierDependents;
    snapshot.memberCount = row.memberCount;
    snapshot.frontierPinsOwned = row.frontierPinsOwned;
    snapshot.publicationHeld = row.publicationHeld;
    snapshot.unpublishedRetirementStage = row.unpublishedRetirementStage;
    snapshot.frontier = row.frontier;
    snapshot.effectiveLease = view.lease->Capture();
    *out = snapshot;
    return true;
}

Plan03UnpublishedRetirementResult
ScheduledTransactionPool::BeginUnpublishedFrontierRetirement(
    const Plan03GroupSliceIdentity& member,
    const Plan03GroupSliceIdentity& primary,
    const FrontierEntryReceiptRef& entry,
    const FrontierActivationReceipt& receipt,
    Plan03UnpublishedRetirementToken* out) noexcept {
    using Result = Plan03UnpublishedRetirementResult;
    const Plan03UnpublishedRetirementToken token{member, primary, entry};
    if (out == nullptr || !token.Valid() || member == primary ||
        member.backing != Plan03GroupBacking::Page ||
        primary.backing != Plan03GroupBacking::Page ||
        member.absoluteQuantum != primary.absoluteQuantum ||
        member.pageGeneration != primary.pageGeneration ||
        member.transportEpoch != primary.transportEpoch ||
        !(receipt.entry == entry) ||
        receipt.outcome != FrontierReceiptOutcome::DefinitivelyNotApplied ||
        receipt.origin != FrontierReceiptOrigin::LogicBeforeAudio ||
        receipt.reason != FrontierReceiptReason::Cancelled ||
        receipt.audioSequence != 0) return Result::NotEligible;
    if (out->Valid() && !(*out == token)) return Result::Stale;
    Plan03ExactGroupSnapshot coordinator{};
    if (!CaptureExactPlan03Group(primary, &coordinator)) return Result::Stale;
    const auto ring = primary.committedRow / config_.maxPlan03GroupHeadersPerPage;
    auto& primaryRow = pagePlan03Groups_[primary.committedRow];
    const auto stage = primaryRow.unpublishedRetirementStage;
    const bool active = stage != 0;
    if (active && (!(primaryRow.unpublishedRetirementMember == member) ||
        !(primaryRow.unpublishedRetirementEntry == entry) ||
        pagePublicationHolds_[ring] == 0)) return Result::Fault;
    if (active && (plan03ArmTable_ == nullptr ||
        primaryRow.unpublishedRetirementArmTableIncarnation == 0 ||
        plan03ArmTable_->Incarnation() !=
            primaryRow.unpublishedRetirementArmTableIncarnation)) return Result::Fault;
    if (coordinator.page.frozen || coordinator.pageStamp == primary.absoluteQuantum ||
        coordinator.lastAudioSequence != 0 ||
        coordinator.effectiveLease.state != AudioSliceState::SliceInvalidated ||
        (coordinator.effectiveLease.pendingReasonBits & AudioSliceReasonCancel) == 0 ||
        coordinator.frontier.role != FrontierGroupRole::Coordinator ||
        !(coordinator.frontier.primary == primary) ||
        !(coordinator.frontier.entry == entry) ||
        !(coordinator.frontier.operation.group == primary) ||
        !(coordinator.frontier.operation.owner == entry.owner) ||
        (active && stage > 3)) return active ? Result::Fault : Result::NotEligible;
    if (stage == 3) {
        *out = token;
        return Result::Ready;
    }
    Plan03ExactGroupSnapshot child{};
    if (!CaptureExactPlan03Group(member, &child))
        return active ? Result::Fault : Result::NotEligible;
    if (child.page.frozen || child.pageStamp == member.absoluteQuantum ||
        child.lastAudioSequence != 0 ||
        child.effectiveLease.state != AudioSliceState::SliceInvalidated ||
        (child.effectiveLease.pendingReasonBits & AudioSliceReasonCancel) == 0 ||
        child.frontier.role != FrontierGroupRole::EntryMember ||
        !(child.frontier.primary == primary) ||
        !(child.frontier.entry == entry) ||
        !(child.frontier.operation.group == member) ||
        !(child.frontier.operation.owner == entry.owner) ||
        child.memberCount == 0 || child.frontierDependents != 0 ||
        child.publicationHeld || coordinator.publicationHeld ||
        child.page.absoluteQuantumIndex != coordinator.page.absoluteQuantumIndex ||
        child.page.pageGeneration != coordinator.page.pageGeneration)
        return active ? Result::Fault : Result::NotEligible;
    if (stage == 2) {
        if (child.frontierPinsOwned || coordinator.frontierPinsOwned ||
            coordinator.frontierDependents != 0) return Result::Fault;
        *out = token;
        return Result::Ready;
    }
    if (stage == 1) {
        if (child.frontierPinsOwned || coordinator.frontierDependents != 0 ||
            !coordinator.frontierPinsOwned || coordinator.readerPins == 0)
            return Result::Fault;
        auto& primaryReaders = pagePlan03GroupReaders_[primary.committedRow];
        primaryReaders.pins.store(coordinator.readerPins - 1u,
                                  std::memory_order_release);
        primaryRow.frontierPinsOwned = false;
        primaryRow.unpublishedRetirementStage = 2;
        *out = token;
        return Result::Ready;
    }
    if (stage != 0 || !child.frontierPinsOwned ||
        !coordinator.frontierPinsOwned || coordinator.frontierDependents != 1 ||
        child.readerPins == 0 || coordinator.readerPins < 2 ||
        pagePublicationHolds_[ring] == UINT32_MAX) return Result::NotEligible;
    if (plan03ArmTable_ == nullptr) return Result::NotEligible;
    const auto armTableIncarnation = plan03ArmTable_->Incarnation();
    Plan03PageGroupView memberView{};
    if (armTableIncarnation == 0 ||
        !ResolvePlan03GroupSliceProducer(member, &memberView) ||
        !(memberView.identity == member) ||
        memberView.memberCount != child.memberCount ||
        memberView.members == nullptr || memberView.frontier == nullptr ||
        memberView.lease == nullptr) return Result::NotEligible;
    for (std::uint16_t index = 0; index < child.memberCount; ++index) {
        if (!plan03ArmTable_->WitnessUnpublishedFrontierRetirement(
                memberView.members[index], *memberView.frontier,
                *memberView.lease)) return Result::NotEligible;
    }
    std::uint32_t ownedGroups = 0;
    const auto rowBase = static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    for (std::uint32_t index = 0; index < config_.maxPlan03GroupHeadersPerPage;
         ++index) {
        const auto rowIndex = rowBase + index;
        if (pagePlan03GroupRowStates_[rowIndex] != Plan03GroupRowState::Live ||
            !(pagePlan03Groups_[rowIndex].frontier.entry == entry)) continue;
        if (rowIndex != member.committedRow && rowIndex != primary.committedRow)
            return Result::NotEligible;
        ++ownedGroups;
    }
    if (ownedGroups != 2) return Result::NotEligible;
    ++pagePublicationHolds_[ring];
    if (pageHeaders_[ring].frozen ||
        pageQuantumStamp_[ring].load(std::memory_order_acquire) == primary.absoluteQuantum ||
        pagePlan03GroupReaders_[member.committedRow].lastAudioSequence.load(
            std::memory_order_acquire) != 0 ||
        pagePlan03GroupReaders_[primary.committedRow].lastAudioSequence.load(
            std::memory_order_acquire) != 0) {
        --pagePublicationHolds_[ring];
        return Result::NotEligible;
    }
    auto& memberRow = pagePlan03Groups_[member.committedRow];
    auto& memberReaders = pagePlan03GroupReaders_[member.committedRow];
    auto& primaryReaders = pagePlan03GroupReaders_[primary.committedRow];
    primaryRow.unpublishedRetirementMember = member;
    primaryRow.unpublishedRetirementEntry = entry;
    primaryRow.unpublishedRetirementArmTableIncarnation = armTableIncarnation;
    memberRow.frontierPinsOwned = false;
    memberReaders.pins.store(child.readerPins - 1u,
                             std::memory_order_release);
    --primaryRow.frontierDependents;
    primaryReaders.pins.store(coordinator.readerPins - 1u,
                              std::memory_order_release);
    primaryRow.unpublishedRetirementStage = 1;
    *out = token;
    return Result::Pending;
}

Plan03UnpublishedRetirementResult
ScheduledTransactionPool::FinishUnpublishedFrontierRetirement(
    const Plan03UnpublishedRetirementToken& token) noexcept {
    using Result = Plan03UnpublishedRetirementResult;
    if (!token.Valid()) return Result::Stale;
    Plan03ExactGroupSnapshot primary{};
    if (!CaptureExactPlan03Group(token.primary, &primary)) return Result::Stale;
    const auto ring = token.primary.committedRow /
        config_.maxPlan03GroupHeadersPerPage;
    auto& primaryRow = pagePlan03Groups_[token.primary.committedRow];
    const auto stage = primaryRow.unpublishedRetirementStage;
    if (stage != 2 && stage != 3) return stage == 1 ? Result::Pending : Result::Stale;
    if (plan03ArmTable_ == nullptr ||
        primaryRow.unpublishedRetirementArmTableIncarnation == 0 ||
        plan03ArmTable_->Incarnation() !=
            primaryRow.unpublishedRetirementArmTableIncarnation) return Result::Stale;
    if (!(primaryRow.unpublishedRetirementMember == token.member) ||
        !(primaryRow.unpublishedRetirementEntry == token.entry) ||
        pagePublicationHolds_[ring] == 0 || primary.page.frozen ||
        primary.pageStamp == token.primary.absoluteQuantum ||
        primary.lastAudioSequence != 0 || primary.frontierPinsOwned ||
        primary.frontierDependents != 0 ||
        primary.effectiveLease.state != AudioSliceState::SliceInvalidated ||
        (primary.effectiveLease.pendingReasonBits & AudioSliceReasonCancel) == 0)
        return Result::Fault;
    const auto retireRow = [&](const Plan03GroupSliceIdentity& identity) noexcept {
        const auto rowIndex = identity.committedRow;
        auto& row = pagePlan03Groups_[rowIndex];
        if (pagePlan03GroupCount_[ring] == 0 ||
            pagePlan03GroupRowStates_[rowIndex] != Plan03GroupRowState::Live ||
            pagePlan03GroupClaimGenerations_[rowIndex] != identity.rowClaimGeneration ||
            row.publicationHeld || row.frontierPinsOwned ||
            row.frontierDependents != 0 ||
            pagePlan03GroupReaders_[rowIndex].pins.load(std::memory_order_acquire) != 0 ||
            pagePlan03GroupReaders_[rowIndex].lastAudioSequence.load(
                std::memory_order_acquire) != 0) return false;
        auto& lease = pagePlan03GroupLeases_[rowIndex];
        if (lease.Capture().state != AudioSliceState::Ready &&
            !lease.ResetForReuse()) return false;
        const auto memberBase = rowIndex * config_.maxArmsPerGroup;
        ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + memberBase, row.memberCount);
        std::fill(pagePlan03GroupMembers_.begin() + memberBase,
                  pagePlan03GroupMembers_.begin() + memberBase + row.memberCount,
                  TimelineArmToken{});
        --pagePlan03GroupCount_[ring];
        row = {};
        if (pagePlan03GroupClaimGenerations_[rowIndex] == UINT32_MAX)
            pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Retired;
        else {
            ++pagePlan03GroupClaimGenerations_[rowIndex];
            pagePlan03GroupRowStates_[rowIndex] = Plan03GroupRowState::Free;
        }
        return true;
    };
    if (stage == 2) {
        Plan03ExactGroupSnapshot member{};
        if (!CaptureExactPlan03Group(token.member, &member) ||
            member.page.frozen || member.pageStamp == token.member.absoluteQuantum ||
            member.lastAudioSequence != 0 || member.frontierPinsOwned ||
            member.frontierDependents != 0 ||
            member.effectiveLease.state != AudioSliceState::SliceInvalidated)
            return Result::Fault;
        if (member.readerPins != 0) return Result::Pending;
        Plan03PageGroupView memberView{};
        if (plan03ArmTable_ == nullptr ||
            !ResolvePlan03GroupSliceProducer(token.member, &memberView) ||
            memberView.frontier == nullptr || memberView.lease == nullptr ||
            !(memberView.identity == token.member)) return Result::Fault;
        for (std::uint16_t index = 0; index < memberView.memberCount; ++index) {
            const auto arm = memberView.members[index];
            if (!plan03ArmTable_->FrontierAdmissionReleasedForRetirement(
                    arm, *memberView.frontier, *memberView.lease) &&
                !plan03ArmTable_->FrontierArmReclaimedAfterRetirementWitness(
                    arm, primaryRow.unpublishedRetirementArmTableIncarnation))
                return Result::Pending;
        }
        if (!retireRow(token.member)) return Result::Fault;
        primaryRow.unpublishedRetirementStage = 3;
    }
    if (pagePlan03GroupReaders_[token.primary.committedRow].pins.load(
            std::memory_order_acquire) != 0) return Result::Pending;
    if (!retireRow(token.primary)) return Result::Fault;
    --pagePublicationHolds_[ring];
    return Result::Ready;
}

void ScheduledTransactionPool::DropPreparedOccupancy() noexcept {
    if (HasHeldCurrentSlices()) return;
    if ( preparedPageFrontierBindingHolds_ != 0 || privateUnboundPageArmRetirements_ != nullptr ||
        unpublishedGroupArmCancellations_ != nullptr) return;
    for (const auto& claim : preparedTransactionClaims_)
        if (claim.transferProof != nullptr) return;
    for (const auto& group : pagePlan03Groups_)
        if (group.transferProof != nullptr) return;
    if (HasPlan03GroupReaders()) return;
    for (const auto& group : pagePlan03Groups_) {
        if (group.publicationHeld || group.unpublishedRetirementStage != 0) return;
    }
    for (const auto& claim : preparedTransactionClaims_) {
        if (claim.used && claim.committed) return;
    }
    if (!Valid() || pageCount_ == 0) {
        return;
    }
    if (config_.maxPlan03GroupHeadersPerPage != 0) {
        for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
            const std::size_t rowBase =
                static_cast<std::size_t>(ring) *
                config_.maxPlan03GroupHeadersPerPage;
            for (std::uint32_t group = 0;
                 group < config_.maxPlan03GroupHeadersPerPage; ++group) {
                const std::size_t rowIndex = rowBase + group;
                const Plan03GroupRowState state =
                    pagePlan03GroupRowStates_[rowIndex];
                if (state != Plan03GroupRowState::Prepared &&
                    state != Plan03GroupRowState::Live) {
                    continue;
                }
                DetachPreparedPageGroupTransfer(rowIndex);
                if (state == Plan03GroupRowState::Prepared &&
                    pagePreparedPlan03GroupCount_[ring] > 0) {
                    --pagePreparedPlan03GroupCount_[ring];
                }
                if (state == Plan03GroupRowState::Live &&
                    pagePlan03GroupCount_[ring] > 0) {
                    --pagePlan03GroupCount_[ring];
                }
                const std::uint16_t memberCount =
                    pagePlan03Groups_[rowIndex].memberCount;
                const std::size_t memberBase =
                    rowIndex * config_.maxArmsPerGroup;
                ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + memberBase, memberCount);
                std::fill(
                    pagePlan03GroupMembers_.begin() + memberBase,
                    pagePlan03GroupMembers_.begin() + memberBase +
                        memberCount,
                    TimelineArmToken{});
                pagePlan03Groups_[rowIndex] = {};
                (void)pagePlan03GroupLeases_[rowIndex].ResetForReuse();
                if (pagePlan03GroupClaimGenerations_[rowIndex] ==
                    UINT32_MAX) {
                    pagePlan03GroupRowStates_[rowIndex] =
                        Plan03GroupRowState::Retired;
                } else {
                    ++pagePlan03GroupClaimGenerations_[rowIndex];
                    pagePlan03GroupRowStates_[rowIndex] =
                        Plan03GroupRowState::Free;
                }
            }
        }
        bool anyPlan03 = false;
        for (std::size_t index = 0; index < pagePlan03GroupRowStates_.size();
             ++index) {
            if (pagePlan03GroupRowStates_[index] ==
                    Plan03GroupRowState::Live ||
                pagePlan03GroupRowStates_[index] ==
                    Plan03GroupRowState::Prepared) {
                anyPlan03 = true;
                break;
            }
        }
        if (!anyPlan03 && plan03ArmTableEstablishedByPrepared_) {
            plan03ArmTable_ = nullptr;
            plan03ArmTableEstablishedByPrepared_ = false;
        }
    }
    for (std::uint32_t claimSlot = 0;
         claimSlot < preparedTransactionClaims_.size(); ++claimSlot) {
        PreparedTransactionClaim& claim =
            preparedTransactionClaims_[claimSlot];
        if (!claim.used || claim.transactionCount == 0) {
            continue;
        }
        DetachPreparedPageBatchTransfer(claimSlot);
        std::uint32_t releasedTransactions = 0;
        for (std::uint32_t transactionSlot = 0;
             transactionSlot < transactions_.size(); ++transactionSlot) {
            TransactionRecord& record = transactions_[transactionSlot];
            if (!record.prepared ||
                record.preparedClaimSlot != claimSlot ||
                record.preparedClaimGeneration != claim.generation) {
                continue;
            }
            for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
                const std::size_t eventBase =
                    static_cast<std::size_t>(ring) *
                    config_.maxEventsPerQuantum;
                for (;;) {
                    const std::uint32_t liveCount = pageEventCount_[ring];
                    const std::uint32_t preparedCount =
                        pagePreparedEventCount_[ring];
                    std::uint32_t found = preparedCount;
                    for (std::uint32_t index = 0; index < preparedCount;
                         ++index) {
                        if (pageEvents_[eventBase + liveCount + index]
                                .txnSlot == transactionSlot) {
                            found = index;
                            break;
                        }
                    }
                    if (found == preparedCount) {
                        break;
                    }
                    EventRow& row =
                        pageEvents_[eventBase + liveCount + found];
                    const std::size_t batchBase =
                        static_cast<std::size_t>(ring) *
                        config_.maxFrameBatchesPerQuantum;
                    for (std::uint32_t batchIndex = 0;
                         batchIndex < pageBatchOccupiedCount_[ring];
                         ++batchIndex) {
                        FrameBatchRow& batch =
                            pageBatchRows_[batchBase + batchIndex];
                        if (batch.frame != row.executeFrame) {
                            continue;
                        }
                        if (batch.preparedReferences > 0) {
                            --batch.preparedReferences;
                        }
                        if (batch.liveReferences == 0 &&
                            batch.preparedReferences == 0) {
                            const std::uint32_t last =
                                --pageBatchOccupiedCount_[ring];
                            batch = pageBatchRows_[batchBase + last];
                            pageBatchRows_[batchBase + last] = {};
                        }
                        break;
                    }
                    const std::uint32_t lastPrepared =
                        liveCount + preparedCount - 1;
                    row = pageEvents_[eventBase + lastPrepared];
                    pageEvents_[eventBase + lastPrepared] = {};
                    --pagePreparedEventCount_[ring];
                    --pagePublicationHolds_[ring];
                }
            }
            record = {};
            freeTransactions_.push_back(transactionSlot);
            ++releasedTransactions;
        }
        if (preparedIngressThisQuantum_ >= claim.transactionCount) {
            preparedIngressThisQuantum_ -= claim.transactionCount;
        } else {
            preparedIngressThisQuantum_ = 0;
        }
        if (preparedTransactionClaimCount_ > 0) {
            --preparedTransactionClaimCount_;
        }
        claim.used = false;
        claim.transactionCount = 0;
        if (claim.generation != UINT32_MAX) {
            ++claim.generation;
            freePreparedTransactionClaims_.push_back(claimSlot);
        }
    }
    preparedTransactionClaimCount_ = 0;
    for (std::uint32_t ring = 0; ring < pageCount_; ++ring) {
        pagePreparedPlan03GroupCount_[ring] = 0;
        pagePlan03GroupCount_[ring] = 0;
    }
}

bool ScheduledTransactionPool::ResolvePlan03GroupSliceRt(
    const Plan03GroupSliceIdentity& identity,
    Plan03PageGroupView* out) const noexcept {
    if (identity.backing == Plan03GroupBacking::ImmediateLane)
        return ResolveImmediateLaneGroup(identity, out, true);
    if (out == nullptr || !identity.Valid() ||
        PageStamp(identity.absoluteQuantum) != identity.absoluteQuantum) {
        return false;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        (identity.absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    if (pageHeaders_[ring].pageGeneration != identity.pageGeneration) {
        return false;
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        const std::size_t rowIndex = rowBase + group;
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) {
            continue;
        }
        const Plan03GroupSliceRow& row = pagePlan03Groups_[rowIndex];
        if (!MatchesPlan03GroupRow(identity, rowIndex) ||
            row.sliceIndex != identity.sliceIndex ||
            row.decision.slot != identity.decision.slot ||
            row.decision.generation != identity.decision.generation) {
            continue;
        }
        Plan03PageGroupView view{};
        view.identity = identity;
        view.identity.committedRow = rowIndex;
        view.identity.rowClaimGeneration = pagePlan03GroupClaimGenerations_[rowIndex];
        view.identity.transportEpoch = epoch_;
        view.memberCount = row.memberCount;
        view.sourceControlCount = row.sourceControlCount;
        view.members = row.memberCount == 0
            ? nullptr
            : &pagePlan03GroupMembers_[rowIndex * config_.maxArmsPerGroup];
        view.lease = EffectivePlan03GroupLease(rowIndex);
        view.frontier = row.frontier.role == FrontierGroupRole::Legacy ? nullptr : &row.frontier;
        view.immediateEntry = row.immediateEntry;
        if (view.lease == nullptr) return false;
        if (view.immediateEntry.Valid() &&
            !view.immediateEntry.gate->AdmitAudio(
                view.immediateEntry.generation)) return false;
        *out = view;
        return true;
    }
    return false;
}

AudioSliceLease* ScheduledTransactionPool::EffectivePlan03GroupLease(std::size_t rowIndex) const noexcept {
    if (rowIndex >= pagePlan03Groups_.size()) return nullptr;
    const auto& binding = pagePlan03Groups_[rowIndex].frontier;
    if (binding.role != FrontierGroupRole::EntryMember) return &pagePlan03GroupLeases_[rowIndex];
    const auto& primary = binding.primary;
    if (!binding.Valid() || primary.committedRow >= pagePlan03Groups_.size() ||
        primary.committedRow == rowIndex || !MatchesPlan03GroupRow(primary, primary.committedRow) ||
        primary.committedRow / config_.maxPlan03GroupHeadersPerPage !=
            rowIndex / config_.maxPlan03GroupHeadersPerPage) return nullptr;
    const auto& row = pagePlan03Groups_[primary.committedRow];
    if (pagePlan03GroupRowStates_[primary.committedRow] != Plan03GroupRowState::Live ||
        row.frontier.role != FrontierGroupRole::Coordinator || !(row.frontier.entry == binding.entry) ||
        !(row.frontier.operation.group == primary)) return nullptr;
    return &pagePlan03GroupLeases_[primary.committedRow];
}

bool ScheduledTransactionPool::BindPlan03GroupFrontier(const Plan03GroupSliceIdentity& identity,
    const FrontierGroupBinding& binding) noexcept {
    if (identity.backing == Plan03GroupBacking::ImmediateLane) {
        Plan03PageGroupView group{};
        if (!identity.Exact() || !binding.Valid() ||
            !(binding.operation.group == identity) ||
            !PrivatePlan03GroupBindable(identity) ||
            !ResolveImmediateLaneGroup(identity, &group, false)) return false;
        auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.frontier.role != FrontierGroupRole::Legacy ||
            quantumFrames_ == 0 || !row.frameRebased ||
            binding.operation.order.executeFrame != row.rebasedFrame ||
            binding.operation.order.executeFrame / quantumFrames_ !=
                row.dueQuantum ||
            row.laneOwner == nullptr ||
            !row.laneOwner->OwnsPrivate(
                {row.laneSlot, row.laneGeneration}, row.cohortRoot) ||
            row.readers.pins.load(std::memory_order_acquire) == UINT32_MAX)
            return false;
        if (!MatchesInitialSoundQueueCompanionBinding(identity, binding)) return false;
        const auto& owner = binding.operation.owner;
        if (owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
            identity.decision.slot >= config_.maxImmediateLaneDecisionGates)
            return false;
        auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
        auto& decisionGate = immediateLaneDecisionGates_[identity.decision.slot];
        const auto bindable = [](const LaneFrontierGate& gate,
                                 std::uint64_t generation) noexcept {
            return gate.references == 0
                ? (gate.state.load(std::memory_order_acquire) & kLaneGateClaims) == 0 &&
                    !(gate.generation.load(std::memory_order_acquire) == generation &&
                      (gate.state.load(std::memory_order_acquire) & kLaneGateInvalid) != 0)
                : gate.references != UINT32_MAX &&
                    gate.generation.load(std::memory_order_acquire) == generation &&
                    (gate.state.load(std::memory_order_acquire) & kLaneGateInvalid) == 0;
        };
        if (!bindable(ownerGate, owner.serviceGeneration) ||
            !bindable(decisionGate, identity.decision.generation) ||
            (decisionGate.references != 0 &&
             (decisionGate.ownerSlot.load(std::memory_order_acquire) != owner.serviceSlot ||
              decisionGate.ownerGeneration.load(std::memory_order_acquire) !=
                  owner.serviceGeneration))) return false;
        if (binding.role == FrontierGroupRole::Coordinator) {
            if (!(binding.primary == identity) || row.memberCount != 0 ||
                row.sourceControlCount != 0) return false;
        } else if (binding.role == FrontierGroupRole::EntryMember) {
            Plan03PageGroupView primary{};
            if (binding.primary == identity ||
                !ResolveImmediateLaneGroup(binding.primary, &primary, false) ||
                primary.frontier == nullptr ||
                primary.frontier->role != FrontierGroupRole::Coordinator ||
                !(primary.frontier->entry == binding.entry) ||
                immediateLaneGroups_[binding.primary.committedRow].cohortRoot != row.cohortRoot ||
                immediateLaneGroups_[binding.primary.committedRow].selectedFutureDomain != row.selectedFutureDomain ||
                immediateLaneGroups_[binding.primary.committedRow].laneOwner != row.laneOwner ||
                immediateLaneGroups_[binding.primary.committedRow].laneSlot != row.laneSlot ||
                immediateLaneGroups_[binding.primary.committedRow].laneGeneration != row.laneGeneration)
                return false;
            auto& coordinator = immediateLaneGroups_[binding.primary.committedRow];
            if (coordinator.publicationProof != nullptr || coordinator.transferProof != nullptr || coordinator.clockProof != nullptr || !coordinator.frontierPinsOwned ||
                coordinator.frontierDependents == UINT32_MAX ||
                coordinator.readers.pins.load(std::memory_order_acquire) == UINT32_MAX)
                return false;
            coordinator.readers.pins.fetch_add(1u, std::memory_order_release);
            ++coordinator.frontierDependents;
        }
        const auto acquireGate = [](LaneFrontierGate& gate,
                                    std::uint64_t generation) noexcept {
            if (gate.references == 0) {
                gate.generation.store(generation, std::memory_order_release);
                gate.state.store(0, std::memory_order_release);
            }
            ++gate.references;
        };
        acquireGate(ownerGate, owner.serviceGeneration);
        if (decisionGate.references == 0) {
            decisionGate.ownerSlot.store(owner.serviceSlot, std::memory_order_release);
            decisionGate.ownerGeneration.store(owner.serviceGeneration,
                                               std::memory_order_release);
        }
        acquireGate(decisionGate, identity.decision.generation);
        row.frontier = binding;
        row.frontierPinsOwned = true;
        row.readers.pins.fetch_add(1u, std::memory_order_release);
        return true;
    }
    Plan03PageGroupView group{};
    if (!identity.Exact() || !binding.Valid() || !(binding.operation.group == identity) ||
        !ResolvePlan03GroupSliceProducer(identity, &group)) return false;
    const auto ring = identity.committedRow / config_.maxPlan03GroupHeadersPerPage;
    auto& row = pagePlan03Groups_[identity.committedRow];
    auto& readers = pagePlan03GroupReaders_[identity.committedRow];
    const auto pins = readers.pins.load(std::memory_order_acquire);
    if (row.frontierBindings != nullptr || row.privateUnboundArmRetirement != nullptr ||
        row.jointCancellation != nullptr || row.cancellationOriginal != nullptr ||
        pageHeaders_[ring].frozen || row.frontier.role != FrontierGroupRole::Legacy ||
        pins == UINT32_MAX || readers.lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        group.lease->Capture().state != AudioSliceState::Ready) return false;
    if (binding.role == FrontierGroupRole::Coordinator) {
        if (!(binding.primary == identity) || row.memberCount != 0 || row.sourceControlCount != 0)
            return false;
    } else if (binding.role == FrontierGroupRole::EntryMember) {
        Plan03PageGroupView primary{};
        if (binding.primary == identity || binding.primary.absoluteQuantum != identity.absoluteQuantum ||
            binding.primary.pageGeneration != identity.pageGeneration ||
            !ResolvePlan03GroupSliceProducer(binding.primary, &primary) || primary.frontier == nullptr ||
            primary.frontier->role != FrontierGroupRole::Coordinator ||
            !(primary.frontier->entry == binding.entry) ||
            primary.lease->Capture().state != AudioSliceState::Ready) return false;
        auto& primaryRow = pagePlan03Groups_[binding.primary.committedRow];
        auto& primaryReaders = pagePlan03GroupReaders_[binding.primary.committedRow];
        const auto primaryPins = primaryReaders.pins.load(std::memory_order_acquire);
        if (!primaryRow.frontierPinsOwned || primaryPins == UINT32_MAX ||
            primaryRow.frontierDependents == UINT32_MAX) return false;
        primaryReaders.pins.store(primaryPins + 1u, std::memory_order_release);
        ++primaryRow.frontierDependents;
    }
    row.frontier = binding;
    row.frontierPinsOwned = true;
    readers.pins.store(pins + 1u, std::memory_order_release);
    return true;
}

bool ScheduledTransactionPool::InvalidateFrontierOwnerOperations(
    const FrontierOwnerIdentity& owner, DecisionToken decision, AudioSliceInvalidateReason reason) const noexcept {
    if (!owner.Valid()) return false;
    // Check all matching shared gates before changing even the first invalid bit.
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        const auto& heldOwner = row.frontier.operation.owner;
        if (row.publicationProof != nullptr && row.frontierPinsOwned &&
            heldOwner.serviceSlot == owner.serviceSlot && heldOwner.serviceGeneration == owner.serviceGeneration &&
            (!decision.Valid() || (row.decision.slot == decision.slot && row.decision.generation == decision.generation)))
            return false;
    }
    bool mutableOwner = true;
    const auto invalidateGate = [reason](LaneFrontierGate& gate,
                                         std::uint64_t generation) noexcept {
        if (gate.generation.load(std::memory_order_acquire) != generation)
            return true;
        const auto reasonBit = 1u <<
            (kLaneGateReasonShift + static_cast<std::uint32_t>(reason));
        const auto prior = gate.state.fetch_or(kLaneGateInvalid | reasonBit,
                                              std::memory_order_acq_rel);
        return (prior & kLaneGateClaims) == 0;
    };
    if (decision.Valid()) {
        if (decision.slot < config_.maxImmediateLaneDecisionGates) {
            auto& gate = immediateLaneDecisionGates_[decision.slot];
            if (gate.ownerSlot.load(std::memory_order_acquire) == owner.serviceSlot &&
                gate.ownerGeneration.load(std::memory_order_acquire) ==
                    owner.serviceGeneration)
                mutableOwner = invalidateGate(gate, decision.generation);
        }
    } else if (owner.serviceSlot < config_.maxImmediateLaneOwnerGates) {
        mutableOwner = invalidateGate(
            immediateLaneOwnerGates_[owner.serviceSlot], owner.serviceGeneration);
    }
    for (std::size_t i = 0; i < pagePlan03Groups_.size(); ++i) {
        const auto& row = pagePlan03Groups_[i];
        const auto& binding = row.frontier;
        if (pagePlan03GroupRowStates_[i] != Plan03GroupRowState::Live || !row.frontierPinsOwned ||
            !binding.Valid() || !(binding.operation.owner == owner) ||
            (decision.Valid() && (binding.operation.group.decision.slot != decision.slot ||
             binding.operation.group.decision.generation != decision.generation))) continue;
        auto* lease = EffectivePlan03GroupLease(i);
        if (lease == nullptr) mutableOwner = false;
        else if (lease->Invalidate(reason) ==
                 AudioSliceInvalidateDisposition::DeferredAfterClaim) mutableOwner = false;
    }
    return mutableOwner;
}

void ScheduledTransactionPool::SettleInvalidatedImmediateLaneFrontiers(
    bool (*held)(void*, std::uint64_t, std::uint32_t,
                 std::uint32_t) noexcept,
    void* context) noexcept {
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        const auto state = row.state.load(std::memory_order_acquire);
        if ((state != LaneGroupState::Private && state != LaneGroupState::Published) ||
            !row.frontierPinsOwned || !row.frontier.Valid() || row.clockProof != nullptr || row.publicationProof != nullptr) continue;
        if (held != nullptr && held(context, row.cohortRoot,
                                    row.laneSlot, row.laneGeneration)) continue;
        const auto& owner = row.frontier.operation.owner;
        if (owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
            row.decision.slot >= config_.maxImmediateLaneDecisionGates) continue;
        const auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
        const auto& decisionGate = immediateLaneDecisionGates_[row.decision.slot];
        // A winning audio claim may not have claimed the shared lease yet.
        // Let it finish before moving cancellation onto that lease.
        if (((ownerGate.state.load(std::memory_order_acquire) |
              decisionGate.state.load(std::memory_order_acquire)) &
             kLaneGateClaims) != 0) continue;
        std::uint32_t reasons = 0;
        if (ownerGate.generation.load(std::memory_order_acquire) ==
            owner.serviceGeneration) {
            const auto gateState = ownerGate.state.load(std::memory_order_acquire);
            if ((gateState & kLaneGateInvalid) != 0)
                reasons |= (gateState & kLaneGateReasonMask) >> kLaneGateReasonShift;
        }
        if (decisionGate.generation.load(std::memory_order_acquire) ==
                row.decision.generation &&
            decisionGate.ownerSlot.load(std::memory_order_acquire) ==
                owner.serviceSlot &&
            decisionGate.ownerGeneration.load(std::memory_order_acquire) ==
                owner.serviceGeneration) {
            const auto gateState = decisionGate.state.load(std::memory_order_acquire);
            if ((gateState & kLaneGateInvalid) != 0)
                reasons |= (gateState & kLaneGateReasonMask) >> kLaneGateReasonShift;
        }
        auto* lease = EffectiveImmediateLaneGroupLease(index);
        if (lease == nullptr) continue;
        for (std::uint32_t reason = 0; reason < 6; ++reason)
            if ((reasons & (1u << reason)) != 0)
                (void)lease->Invalidate(static_cast<AudioSliceInvalidateReason>(reason));
    }
}

bool ScheduledTransactionPool::HasFrontierOwnerOperations(const FrontierOwnerIdentity& owner, DecisionToken decision) const noexcept {
    for (std::size_t i = 0; i < config_.maxImmediateLaneGroupRows; ++i) {
        const auto& row = immediateLaneGroups_[i];
        const auto state = row.state.load(std::memory_order_acquire);
        if ((state == LaneGroupState::Private || state == LaneGroupState::Published) &&
            row.frontierPinsOwned && row.frontier.Valid() &&
            row.frontier.operation.owner == owner &&
            (!decision.Valid() || (row.decision.slot == decision.slot &&
                row.decision.generation == decision.generation))) return true;
    }
    for (std::size_t i = 0; i < pagePlan03Groups_.size(); ++i) {
        const auto& row = pagePlan03Groups_[i];
        if (pagePlan03GroupRowStates_[i] == Plan03GroupRowState::Live && row.frontierPinsOwned &&
            row.frontier.Valid() && row.frontier.operation.owner == owner &&
            (!decision.Valid() || (row.decision.slot == decision.slot && row.decision.generation == decision.generation))) return true;
    }
    return false;
}

bool ScheduledTransactionPool::FrontierMemberBindingDetached(const Plan03GroupSliceIdentity& identity,
    const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence) const noexcept {
    Plan03PageGroupView group{};
    if (!identity.Exact() || !entry.Valid() ||
        identity.backing != Plan03GroupBacking::ImmediateLane ||
        !ResolvePlan03GroupSliceProducer(identity, &group) || group.identity != identity ||
        group.frontier == nullptr || !group.frontier->Valid() || group.frontier->entry != entry ||
        group.frontier->operation.group != identity || group.lease == nullptr ||
        (group.frontier->role != FrontierGroupRole::EntryMember &&
         group.frontier->role != FrontierGroupRole::Future)) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    const auto state = group.lease->Capture().state;
    const auto last = row.readers.lastAudioSequence.load(std::memory_order_acquire);
    // Other readers retain their own loans; only this exact frontier loan has ended.
    return row.state.load(std::memory_order_acquire) == LaneGroupState::Published &&
        !row.frontierPinsOwned && row.frontierDependents == 0 &&
        row.publicationProof == nullptr && row.transferProof == nullptr && row.clockProof == nullptr &&
        (state == AudioSliceState::SliceApplied || state == AudioSliceState::SliceInvalidated) &&
        (last == 0 || completedAudioSequence > last);
}

bool ScheduledTransactionPool::FrontierCoordinatorDetached(const Plan03GroupSliceIdentity& identity,
    const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
    const Plan03GroupReaderPin* activationReader) const noexcept {
    Plan03PageGroupView group{};
    if (!identity.Exact() || !entry.Valid() || identity.decision.slot != entry.coordinator.slot ||
        identity.decision.generation != entry.coordinator.generation ||
        !ResolvePlan03GroupSliceProducer(identity, &group) || !(group.identity == identity) ||
        group.memberCount != 0 || group.sourceControlCount != 0 || group.lease == nullptr) return false;
    if (activationReader != nullptr && (activationReader->owner_ != this ||
        !(activationReader->identity_ == identity))) return false;
    if (group.frontier != nullptr && (!group.frontier->Valid() ||
        group.frontier->role != FrontierGroupRole::Coordinator || !(group.frontier->entry == entry) ||
        !(group.frontier->primary == identity) || !(group.frontier->operation.group == identity) ||
        !(group.frontier->operation.owner == entry.owner))) return false;
    const auto detached = [&](const auto& row, const auto& readers) noexcept {
        const auto last = readers.lastAudioSequence.load(std::memory_order_acquire);
        return !row.frontierPinsOwned && row.frontierDependents == 0 &&
            readers.pins.load(std::memory_order_acquire) == (activationReader == nullptr ? 0u : 1u) &&
            (last == 0 || completedAudioSequence > last);
    };
    // Published descriptors stay immutable after their actual reader loans return.
    if (identity.backing == Plan03GroupBacking::ImmediateLane) {
        const auto& row = immediateLaneGroups_[identity.committedRow];
        return row.publicationProof == nullptr && row.transferOriginal == nullptr && row.transferProof == nullptr && row.clockProof == nullptr &&
            detached(row, row.readers);
    }
    const auto& row = pagePlan03Groups_[identity.committedRow];
    return row.transferOriginal == nullptr && row.transferProof == nullptr && row.frontierBindings == nullptr &&
        row.privateUnboundArmRetirement == nullptr && row.jointCancellation == nullptr &&
        row.cancellationOriginal == nullptr && detached(row, pagePlan03GroupReaders_[identity.committedRow]);
}

bool ScheduledTransactionPool::DetachPlan03GroupFrontier(const Plan03GroupSliceIdentity& identity,
    std::uint64_t completedAudioSequence) noexcept {
    if (identity.backing == Plan03GroupBacking::ImmediateLane) {
        Plan03PageGroupView group{};
        if (!ResolveImmediateLaneGroup(identity, &group, false)) return false;
        auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.publicationProof != nullptr || row.transferProof != nullptr || row.clockProof != nullptr || !row.frontierPinsOwned ||
            row.frontierDependents != 0) return false;
        const auto pins = row.readers.pins.load(std::memory_order_acquire);
        const auto read = row.readers.lastAudioSequence.load(std::memory_order_acquire);
        const auto state = group.lease->Capture().state;
        const bool privateUnread = row.state.load(std::memory_order_acquire) ==
            LaneGroupState::Private && read == 0 && state == AudioSliceState::Ready;
        if (pins == 0 || (read != 0 && completedAudioSequence <= read) ||
            (!privateUnread && state != AudioSliceState::SliceApplied &&
                state != AudioSliceState::SliceInvalidated)) return false;
        const auto& owner = row.frontier.operation.owner;
        auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
        auto& decisionGate = immediateLaneDecisionGates_[row.decision.slot];
        if (ownerGate.references == 0 || decisionGate.references == 0 ||
            (ownerGate.state.load(std::memory_order_acquire) & kLaneGateClaims) != 0 ||
            (decisionGate.state.load(std::memory_order_acquire) & kLaneGateClaims) != 0)
            return false;
        if (row.frontier.role == FrontierGroupRole::EntryMember) {
            auto& primary = immediateLaneGroups_[row.frontier.primary.committedRow];
            const auto primaryPins = primary.readers.pins.load(std::memory_order_acquire);
            const auto primaryRead = primary.readers.lastAudioSequence.load(std::memory_order_acquire);
            if (primary.publicationProof != nullptr || primary.transferProof != nullptr || primary.clockProof != nullptr || primaryPins == 0 ||
                primary.frontierDependents == 0 ||
                (primaryRead != 0 && completedAudioSequence <= primaryRead)) return false;
            --primary.frontierDependents;
            primary.readers.pins.store(primaryPins - 1u, std::memory_order_release);
        }
        row.frontierPinsOwned = false;
        row.readers.pins.store(pins - 1u, std::memory_order_release);
        --ownerGate.references;
        --decisionGate.references;
        if (privateUnread) row.frontier = {};
        return true;
    }
    Plan03PageGroupView group{};
    if (!identity.Exact() || !ResolvePlan03GroupSliceProducer(identity, &group)) return false;
    auto& row = pagePlan03Groups_[identity.committedRow];
    if (row.frontierBindings != nullptr || row.privateUnboundArmRetirement != nullptr ||
        row.jointCancellation != nullptr || row.cancellationOriginal != nullptr ||
        !row.frontier.Valid() || !row.frontierPinsOwned || row.frontierDependents != 0) return false;
    const auto ring = identity.committedRow / config_.maxPlan03GroupHeadersPerPage;
    const auto leaseState = group.lease->Capture().state;
    auto& readers = pagePlan03GroupReaders_[identity.committedRow];
    const auto pins = readers.pins.load(std::memory_order_acquire);
    const auto lastRead = readers.lastAudioSequence.load(std::memory_order_acquire);
    const bool privateUnread = !pageHeaders_[ring].frozen && lastRead == 0 &&
        leaseState == AudioSliceState::Ready;
    if (!privateUnread && !row.publicationHeld && lastRead == 0 && consumerFrontierEnabled_ &&
        consumedFrontierQuantum_->load(std::memory_order_acquire) <= identity.absoluteQuantum)
        return false;
    if (pins == 0 || (lastRead != 0 && completedAudioSequence <= lastRead) ||
        (!privateUnread && leaseState != AudioSliceState::SliceApplied &&
            leaseState != AudioSliceState::SliceInvalidated)) return false;
    if (row.frontier.role == FrontierGroupRole::EntryMember) {
        const auto primaryIndex = row.frontier.primary.committedRow;
        auto& primaryRow = pagePlan03Groups_[primaryIndex];
        auto& primaryReaders = pagePlan03GroupReaders_[primaryIndex];
        const auto primaryPins = primaryReaders.pins.load(std::memory_order_acquire);
        const auto primaryRead = primaryReaders.lastAudioSequence.load(std::memory_order_acquire);
        if (primaryPins == 0 || primaryRow.frontierDependents == 0 ||
            (primaryRead != 0 && completedAudioSequence <= primaryRead)) return false;
        --primaryRow.frontierDependents;
        primaryReaders.pins.store(primaryPins - 1u, std::memory_order_release);
    }
    row.frontierPinsOwned = false;
    readers.pins.store(pins - 1u, std::memory_order_release);
    if (privateUnread) row.frontier = {};
    return true;
}

bool ScheduledTransactionPool::AcquireImmediateLaneFutureDependent(
    const Plan03GroupSliceIdentity& identity) noexcept {
    Plan03PageGroupView group{};
    if (identity.backing != Plan03GroupBacking::ImmediateLane ||
        !ResolveImmediateLaneGroup(identity, &group, false) ||
        group.frontier == nullptr ||
        group.frontier->role != FrontierGroupRole::EntryMember ||
        immediateLaneGroups_[identity.committedRow].transferProof != nullptr ||
        immediateLaneGroups_[identity.committedRow].clockProof != nullptr ||
        immediateLaneGroups_[identity.committedRow].publicationProof != nullptr ||
        !immediateLaneGroups_[identity.committedRow].frontierPinsOwned ||
        immediateLaneGroups_[identity.committedRow].frontierDependents ==
            UINT32_MAX) return false;
    ++immediateLaneGroups_[identity.committedRow].frontierDependents;
    return true;
}

bool ScheduledTransactionPool::ReleaseImmediateLaneFutureDependent(
    const Plan03GroupSliceIdentity& identity) noexcept {
    Plan03PageGroupView group{};
    if (identity.backing != Plan03GroupBacking::ImmediateLane ||
        !ResolveImmediateLaneGroup(identity, &group, false) ||
        group.frontier == nullptr ||
        group.frontier->role != FrontierGroupRole::EntryMember ||
        immediateLaneGroups_[identity.committedRow].transferProof != nullptr ||
        immediateLaneGroups_[identity.committedRow].clockProof != nullptr ||
        immediateLaneGroups_[identity.committedRow].publicationProof != nullptr ||
        immediateLaneGroups_[identity.committedRow].frontierDependents == 0)
        return false;
    --immediateLaneGroups_[identity.committedRow].frontierDependents;
    return true;
}

bool ScheduledTransactionPool::MatchesPlan03GroupRow(
    const Plan03GroupSliceIdentity& identity, std::size_t rowIndex) const noexcept {
    const bool hasExactFields = identity.committedRow != SIZE_MAX ||
        identity.rowClaimGeneration != 0 || identity.transportEpoch != kInvalidTransportEpoch;
    return !hasExactFields || (identity.Exact() &&
        identity.committedRow == rowIndex && identity.transportEpoch == epoch_ &&
        identity.rowClaimGeneration == pagePlan03GroupClaimGenerations_[rowIndex]);
}

bool ScheduledTransactionPool::IsUnpublishedRetirementParticipant(
    const Plan03GroupSliceIdentity& identity) const noexcept {
    if (identity.backing != Plan03GroupBacking::Page || !identity.Exact())
        return false;
    Plan03ExactGroupSnapshot current{};
    if (!CaptureExactPlan03Group(identity, &current)) return false;
    const auto& row = pagePlan03Groups_[identity.committedRow];
    if (row.frontierBindings != nullptr || row.privateUnboundArmRetirement != nullptr ||
        row.jointCancellation != nullptr || row.cancellationOriginal != nullptr) return true;
    const auto& binding = row.frontier;
    if (binding.role == FrontierGroupRole::Coordinator) {
        return row.unpublishedRetirementStage >= 1 &&
            row.unpublishedRetirementStage <= 3 &&
            row.unpublishedRetirementMember.Exact() &&
            row.unpublishedRetirementMember.backing == Plan03GroupBacking::Page &&
            row.unpublishedRetirementMember.absoluteQuantum == identity.absoluteQuantum &&
            row.unpublishedRetirementMember.pageGeneration == identity.pageGeneration &&
            row.unpublishedRetirementMember.transportEpoch == identity.transportEpoch &&
            row.unpublishedRetirementMember.committedRow /
                config_.maxPlan03GroupHeadersPerPage ==
                identity.committedRow / config_.maxPlan03GroupHeadersPerPage &&
            row.unpublishedRetirementEntry == binding.entry &&
            binding.primary == identity && binding.operation.group == identity;
    }
    if (binding.role != FrontierGroupRole::EntryMember ||
        !(binding.operation.group == identity) ||
        !binding.primary.Exact() ||
        binding.primary.backing != Plan03GroupBacking::Page ||
        binding.primary.absoluteQuantum != identity.absoluteQuantum ||
        binding.primary.pageGeneration != identity.pageGeneration ||
        binding.primary.transportEpoch != identity.transportEpoch ||
        binding.primary.committedRow /
            config_.maxPlan03GroupHeadersPerPage !=
            identity.committedRow / config_.maxPlan03GroupHeadersPerPage)
        return false;
    Plan03ExactGroupSnapshot primary{};
    if (!CaptureExactPlan03Group(binding.primary, &primary)) return false;
    const auto& coordinator = pagePlan03Groups_[binding.primary.committedRow];
    return coordinator.unpublishedRetirementStage >= 1 &&
        coordinator.unpublishedRetirementStage <= 2 &&
        coordinator.unpublishedRetirementMember == identity &&
        coordinator.unpublishedRetirementEntry == binding.entry &&
        coordinator.frontier.role == FrontierGroupRole::Coordinator &&
        coordinator.frontier.primary == binding.primary &&
        coordinator.frontier.operation.group == binding.primary &&
        coordinator.frontier.entry == binding.entry;
}

bool ScheduledTransactionPool::HasPlan03GroupReaders() const noexcept {
    if (pagePlan03GroupReaders_ != nullptr)
        for (std::size_t i = 0; i < pagePlan03Groups_.size(); ++i)
            if (pagePlan03GroupReaders_[i].pins.load(std::memory_order_acquire) != 0)
                return true;
    if (immediateLaneGroups_ != nullptr)
        for (std::size_t i = 0; i < config_.maxImmediateLaneGroupRows; ++i)
            if (immediateLaneGroups_[i].readers.pins.load(std::memory_order_acquire) != 0)
                return true;
    return false;
}

std::uint64_t ScheduledTransactionPool::Plan03GroupReaderStorageBytes() const noexcept {
    return (pagePlan03GroupReaders_ == nullptr ? 0 :
        static_cast<std::uint64_t>(pagePlan03Groups_.size()) * sizeof(Plan03GroupReaderState)) +
        static_cast<std::uint64_t>(config_.maxImmediateLaneGroupRows) *
            sizeof(Plan03GroupReaderState);
}

bool ScheduledTransactionPool::AcquirePlan03GroupReader(
    const Plan03GroupSliceIdentity& identity, Plan03GroupReaderPin* out) noexcept {
    if (identity.backing == Plan03GroupBacking::ImmediateLane) {
        if (out == nullptr || out->Valid() || !PrivatePlan03GroupBindable(identity))
            return false;
        auto& readers = immediateLaneGroups_[identity.committedRow].readers;
        const auto pins = readers.pins.load(std::memory_order_acquire);
        if (pins == UINT32_MAX) return false;
        readers.pins.store(pins + 1u, std::memory_order_release);
        out->owner_ = this;
        out->identity_ = identity;
        return true;
    }
    Plan03PageGroupView view{};
    if (out == nullptr || out->Valid() || !identity.Exact() ||
        !ResolvePlan03GroupSliceProducer(identity, &view)) return false;
    const auto ring = identity.committedRow / config_.maxPlan03GroupHeadersPerPage;
    if (pagePlan03Groups_[identity.committedRow].cancellationOriginal != nullptr ||
        pageHeaders_[ring].frozen || view.lease->Capture().state != AudioSliceState::Ready) {
        return false;
    }
    auto& readers = pagePlan03GroupReaders_[identity.committedRow];
    const auto count = readers.pins.load(std::memory_order_acquire);
    if (count == UINT32_MAX) return false;
    if (count == 0) readers.lastAudioSequence.store(0, std::memory_order_relaxed);
    readers.pins.store(count + 1u, std::memory_order_release);
    out->owner_ = this;
    out->identity_ = identity;
    return true;
}

bool ScheduledTransactionPool::NotePlan03GroupReadRt(
    const Plan03GroupSliceIdentity& identity, std::uint64_t audioSequence) const noexcept {
    if (identity.backing == Plan03GroupBacking::ImmediateLane) {
        Plan03PageGroupView view{};
        if (audioSequence == 0 ||
            !ResolveImmediateLaneGroup(identity, &view, true)) return false;
        auto& readers = immediateLaneGroups_[identity.committedRow].readers;
        if (readers.pins.load(std::memory_order_acquire) == 0 ||
            audioSequence < readers.lastAudioSequence.load(std::memory_order_relaxed))
            return false;
        readers.lastAudioSequence.store(audioSequence, std::memory_order_release);
        return true;
    }
    Plan03PageGroupView view{};
    if (audioSequence == 0 || !identity.Exact() ||
        !ResolvePlan03GroupSliceRt(identity, &view)) return false;
    auto& readers = pagePlan03GroupReaders_[identity.committedRow];
    if (readers.pins.load(std::memory_order_acquire) == 0 ||
        audioSequence < readers.lastAudioSequence.load(std::memory_order_relaxed)) return false;
    readers.lastAudioSequence.store(audioSequence, std::memory_order_release);
    return true;
}

bool Plan03GroupReaderPin::Release(std::uint64_t completedAudioSequence) noexcept {
    return owner_ == nullptr || owner_->ReleasePlan03GroupReader(this, completedAudioSequence);
}

bool ScheduledTransactionPool::ReleasePlan03GroupReader(
    Plan03GroupReaderPin* pin, std::uint64_t completedAudioSequence) noexcept {
    if (pin != nullptr && pin->identity_.backing == Plan03GroupBacking::ImmediateLane)
        return ReleaseImmediateLaneGroupReader(pin, completedAudioSequence);
    Plan03PageGroupView view{};
    if (pin == nullptr || pin->owner_ != this || !pin->identity_.Exact() ||
        !ResolvePlan03GroupSliceProducer(pin->identity_, &view)) return false;
    const auto rowIndex = pin->identity_.committedRow;
    auto& readers = pagePlan03GroupReaders_[rowIndex];
    const auto count = readers.pins.load(std::memory_order_acquire);
    const auto lastRead = readers.lastAudioSequence.load(std::memory_order_acquire);
    const auto leaseState = view.lease->Capture().state;
    const auto ring = rowIndex / config_.maxPlan03GroupHeadersPerPage;
    const bool privateUnread = !pageHeaders_[ring].frozen && lastRead == 0 &&
        leaseState == AudioSliceState::Ready;
    if (count == 0 || (lastRead != 0 && completedAudioSequence <= lastRead) ||
        (!privateUnread && leaseState != AudioSliceState::SliceApplied &&
            leaseState != AudioSliceState::SliceInvalidated)) return false;
    readers.pins.store(count - 1u, std::memory_order_release);
    pin->owner_ = nullptr;
    pin->identity_ = {};
    return true;
}

bool ScheduledTransactionPool::ResolvePlan03GroupSliceProducer(
    const Plan03GroupSliceIdentity& identity,
    Plan03PageGroupView* out) const noexcept {
    if (identity.backing == Plan03GroupBacking::ImmediateLane)
        return ResolveImmediateLaneGroup(identity, out, false);
    if (out == nullptr || !identity.Valid() ||
        identity.absoluteQuantum < baseQuantumIndex_ ||
        identity.absoluteQuantum >=
            baseQuantumIndex_ + pageCount_) {
        return false;
    }
    const std::uint32_t ring = static_cast<std::uint32_t>(
        (identity.absoluteQuantum - sessionBaseQuantumIndex_) % pageCount_);
    if (pageHeaders_[ring].pageGeneration != identity.pageGeneration) {
        return false;
    }
    const std::size_t rowBase =
        static_cast<std::size_t>(ring) *
        config_.maxPlan03GroupHeadersPerPage;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        const std::size_t rowIndex = rowBase + group;
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) {
            continue;
        }
        const Plan03GroupSliceRow& row = pagePlan03Groups_[rowIndex];
        if (!MatchesPlan03GroupRow(identity, rowIndex) ||
            row.sliceIndex != identity.sliceIndex ||
            row.decision.slot != identity.decision.slot ||
            row.decision.generation != identity.decision.generation) {
            continue;
        }
        out->identity = identity;
        out->identity.committedRow = rowIndex;
        out->identity.rowClaimGeneration = pagePlan03GroupClaimGenerations_[rowIndex];
        out->identity.transportEpoch = epoch_;
        out->memberCount = row.memberCount;
        out->sourceControlCount = row.sourceControlCount;
        out->members = row.memberCount == 0
            ? nullptr
            : &pagePlan03GroupMembers_[rowIndex * config_.maxArmsPerGroup];
        out->lease = EffectivePlan03GroupLease(rowIndex);
        out->frontier = row.frontier.role == FrontierGroupRole::Legacy ? nullptr : &row.frontier;
        if (out->lease == nullptr) return false;
        return true;
    }
    return false;
}

bool ScheduledTransactionPool::ReleaseImmediateLaneGroupReader(
    Plan03GroupReaderPin* pin, std::uint64_t completedAudioSequence) noexcept {
    Plan03PageGroupView group{};
    if (pin == nullptr || pin->owner_ != this ||
        !ResolveImmediateLaneGroup(pin->identity_, &group, false)) return false;
    auto& row = immediateLaneGroups_[pin->identity_.committedRow];
    auto& readers = row.readers;
    const auto pins = readers.pins.load(std::memory_order_acquire);
    const auto read = readers.lastAudioSequence.load(std::memory_order_acquire);
    const auto state = group.lease->Capture().state;
    const bool privateUnread = row.state.load(std::memory_order_acquire) ==
        LaneGroupState::Private && read == 0 && state == AudioSliceState::Ready;
    if (pins == 0 || (read != 0 && completedAudioSequence <= read) ||
        (!privateUnread && state != AudioSliceState::SliceApplied &&
            state != AudioSliceState::SliceInvalidated)) return false;
    readers.pins.store(pins - 1u, std::memory_order_release);
    pin->owner_ = nullptr;
    pin->identity_ = {};
    return true;
}

bool ScheduledTransactionPool::OriginalQueueFutureGroupRetirementReady(
    const Plan03GroupSliceIdentity& identity,
    const OriginalQueueFutureGroupRegistration* originalBaseline) const noexcept {
    Plan03PageGroupView actual{};
    if (!ResolveImmediateLaneGroup(identity, &actual, false)) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    return originalBaseline != nullptr
        ? IsOriginalQueueFutureGroupRegistrationFor(*originalBaseline, identity) &&
            row.originalRegistrationPinCount == 1u
        : row.originalRegistrationPinCount == 0u;
}

bool ScheduledTransactionPool::CancelPrivateImmediateLaneGroup(
    std::uint64_t cohortRoot, const Plan03GroupSliceIdentity& identity,
    OriginalQueueFutureGroupRegistration* originalBaseline) noexcept {
    return CancelPrivateImmediateLaneGroupInDomain(cohortRoot, identity, originalBaseline, 0);
}

bool ScheduledTransactionPool::CancelPrivateImmediateLaneGroupInDomain(
    std::uint64_t cohortRoot, const Plan03GroupSliceIdentity& identity,
    OriginalQueueFutureGroupRegistration* originalBaseline, std::uint64_t domain) noexcept {
    Plan03PageGroupView actual{};
    if (cohortRoot == 0 || !ResolveImmediateLaneGroup(identity, &actual, false)) return false;
    auto& row = immediateLaneGroups_[identity.committedRow];
    // This capability exits one never-filed row, without disposing its neighbors.
    if (row.transferProof != nullptr || row.clockProof != nullptr ||
        !OriginalQueueFutureGroupRetirementReady(identity, originalBaseline)) return false;
    if (row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
        (row.cohortRoot != cohortRoot || row.selectedFutureDomain != domain) || row.laneOwner != nullptr ||
        row.laneSlot != UINT32_MAX || row.laneGeneration != 0 ||
        row.frameRebased || row.bucketOrdinal != UINT32_MAX ||
        row.frontier.role != FrontierGroupRole::Legacy || row.frontierPinsOwned ||
        row.frontierDependents != 0 || row.readers.pins.load(std::memory_order_acquire) != 0 ||
        row.readers.lastAudioSequence.load(std::memory_order_acquire) != 0) return false;
    const auto lease = row.lease.Capture();
    if (lease.state != AudioSliceState::Ready &&
        !(lease.state == AudioSliceState::SliceInvalidated &&
          lease.pendingReasonBits == AudioSliceReasonCancel)) return false;
    if (lease.state == AudioSliceState::Ready &&
        row.lease.Invalidate(AudioSliceInvalidateReason::Cancel) !=
            AudioSliceInvalidateDisposition::InvalidatedReady) return false;
    if (!row.lease.ResetForReuse()) return false;
    if (immediateLaneGroupDisposalObserver_ != nullptr)
        immediateLaneGroupDisposalObserver_(immediateLaneGroupDisposalContext_, identity,
            row.cohortRoot, row.selectedFutureDomain);
    if (originalBaseline != nullptr) originalBaseline->Release();
    ReturnGroupMemberOriginals(immediateGroupMemberOriginals_.get() +
        identity.committedRow * config_.maxArmsPerGroup, row.memberCount);
    row.cohortRoot = 0;
            row.selectedFutureDomain = 0;
    row.groupIndex = UINT32_MAX;
    row.rebasedFrame = 0;
    row.dueQuantum = UINT64_MAX;
    row.frontier = {};
    row.initialSoundQueueCompanion = {};
    row.memberCount = 0;
    row.sourceControlCount = 0;
    row.transferOriginal = nullptr;
    row.originalFactoryMarker = 0;
    row.state.store(LaneGroupState::Free, std::memory_order_release);
    return true;
}

bool ScheduledTransactionPool::CancelImmediateLaneGroups(
    std::uint64_t cohortRoot, std::uint64_t completedAudioSequence,
    OriginalQueueFutureGroupRegistration* originalBaselines,
    std::size_t originalBaselineCapacity) noexcept {
    return CancelImmediateLaneGroupsInDomain(cohortRoot, completedAudioSequence, originalBaselines, originalBaselineCapacity, 0);
}

bool ScheduledTransactionPool::CancelImmediateLaneGroupsInDomain(
    std::uint64_t cohortRoot, std::uint64_t completedAudioSequence,
    OriginalQueueFutureGroupRegistration* originalBaselines,
    std::size_t originalBaselineCapacity, std::uint64_t domain) noexcept {
    if (domain == 0 && InitialLaneCohortPublicationHeld(cohortRoot)) return false;
    if (cohortRoot == 0 ||
        (originalBaselines == nullptr && originalBaselineCapacity != 0) ||
        (originalBaselines != nullptr &&
            originalBaselineCapacity != config_.maxImmediateLaneGroupRows)) return false;
    bool complete = true;
    // Entry members must dispose while their genuine primary is still alive.
    // A busy member keeps that primary for the next native retry.
    for (unsigned pass = 0; pass < 2u; ++pass) {
        for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
            auto& row = immediateLaneGroups_[index];
            const auto state = row.state.load(std::memory_order_acquire);
            if ((state != LaneGroupState::Private && state != LaneGroupState::Published) ||
                (row.cohortRoot != cohortRoot || row.selectedFutureDomain != domain)) continue;
            const bool coordinator = row.frontier.role == FrontierGroupRole::Coordinator;
            if (coordinator != (pass == 1u)) continue;
            const Plan03GroupSliceIdentity identity{0, 1, row.decision, row.sliceIndex,
                index, row.generation, epoch_, Plan03GroupBacking::ImmediateLane};
            if (coordinator) {
                bool liveMember = false;
                for (std::size_t memberIndex = 0;
                     memberIndex < config_.maxImmediateLaneGroupRows; ++memberIndex) {
                    const auto& member = immediateLaneGroups_[memberIndex];
                    const auto memberState = member.state.load(std::memory_order_acquire);
                    if ((memberState == LaneGroupState::Private ||
                         memberState == LaneGroupState::Published) &&
                        member.frontier.role == FrontierGroupRole::EntryMember &&
                        member.frontier.primary == identity) {
                        // The native full primary association still needs this lease.
                        // Keep it even if another part of that binding is malformed.
                        liveMember = true;
                        break;
                    }
                }
                if (liveMember) {
                    complete = false;
                    continue;
                }
            }
            auto* const original = originalBaselines != nullptr && originalBaselines[index].Valid()
                ? &originalBaselines[index] : nullptr;
            if (row.transferProof != nullptr || row.clockProof != nullptr ||
                !OriginalQueueFutureGroupRetirementReady(identity, original) ||
                row.frontierPinsOwned || row.frontierDependents != 0 ||
                row.readers.pins.load(std::memory_order_acquire) != 0 ||
                (row.readers.lastAudioSequence.load(std::memory_order_acquire) != 0 &&
                 completedAudioSequence <= row.readers.lastAudioSequence.load(
                     std::memory_order_acquire))) {
                complete = false;
                continue;
            }
            if (row.lease.Capture().state == AudioSliceState::Ready)
                (void)row.lease.Invalidate(AudioSliceInvalidateReason::Cancel);
            const auto leaseState = row.lease.Capture().state;
            if (leaseState != AudioSliceState::SliceApplied &&
                leaseState != AudioSliceState::SliceInvalidated) {
                complete = false;
                continue;
            }
            if (!row.lease.ResetForReuse()) {
                complete = false;
                continue;
            }
            if (immediateLaneGroupDisposalObserver_ != nullptr)
                immediateLaneGroupDisposalObserver_(immediateLaneGroupDisposalContext_, identity,
                    row.cohortRoot, row.selectedFutureDomain);
            if (original != nullptr) original->Release();
            ReturnGroupMemberOriginals(immediateGroupMemberOriginals_.get() +
                index * config_.maxArmsPerGroup, row.memberCount);
            row.cohortRoot = 0;
            row.selectedFutureDomain = 0;
            row.laneOwner = nullptr;
            row.laneSlot = UINT32_MAX;
            row.laneGeneration = 0;
            row.bucketOrdinal = UINT32_MAX;
            row.groupIndex = UINT32_MAX;
            row.rebasedFrame = 0;
            row.frameRebased = false;
            row.dueQuantum = UINT64_MAX;
            row.frontier = {};
            row.initialSoundQueueCompanion = {};
            row.memberCount = 0;
            row.sourceControlCount = 0;
            row.transferOriginal = nullptr;
            row.originalFactoryMarker = 0;
            row.state.store(LaneGroupState::Free, std::memory_order_release);
        }
    }
    return complete;
}

bool ScheduledTransactionPool::ReserveImmediateLaneGroup(
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
    Plan03GroupSliceIdentity* out, PreparedPrivateImmediateLaneGroup* original) noexcept {
    return ReserveImmediateLaneGroupInDomain(cohortRoot, provisionalAnchorFrame, input, armTable, out, original, 0);
}

bool ScheduledTransactionPool::ReserveImmediateLaneGroupInDomain(
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
    Plan03GroupSliceIdentity* out, PreparedPrivateImmediateLaneGroup* original, std::uint64_t domain) noexcept {
    if (domain == 0 && InitialLaneCohortPublicationHeld(cohortRoot)) return false;
    if ((original != nullptr && (original->owner_ != nullptr || armTable.Incarnation() == 0 ||
            nextOriginalLaneGroupFactoryMarker_ == UINT64_MAX)) ||
        !Valid() || cohortRoot == 0 || out == nullptr || out->Valid() ||
        !input.decision.Valid() || input.executeFrame < provisionalAnchorFrame ||
        input.memberCount > config_.maxArmsPerGroup ||
        input.sourceControlCount > input.memberCount ||
        (input.memberCount != 0 && input.members == nullptr) ||
        immediateLaneGroups_ == nullptr ||
        (plan03ArmTable_ != nullptr && plan03ArmTable_ != &armTable)) return false;
    for (std::uint16_t member = 0; member < input.memberCount; ++member) {
        TimelineArmObservation observation{};
        if ((original != nullptr && armTable.Resolve(input.members[member]) == nullptr) ||
            !armTable.Observe(input.members[member], &observation) ||
            observation.parentGroup.decision.slot != input.decision.slot ||
            observation.parentGroup.decision.generation != input.decision.generation)
            return false;
        for (std::uint16_t prior = 0; prior < member; ++prior) {
            TimelineArmObservation priorObservation{};
            if (!armTable.Observe(input.members[prior], &priorObservation) ||
                priorObservation.memberIndex == observation.memberIndex ||
                (input.members[prior].slot == input.members[member].slot &&
                 input.members[prior].generation == input.members[member].generation))
                return false;
        }
    }
    std::size_t freeRow = config_.maxImmediateLaneGroupRows;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        const auto state = row.state.load(std::memory_order_acquire);
        if (state == LaneGroupState::Free && freeRow == config_.maxImmediateLaneGroupRows &&
            row.generation != UINT32_MAX && row.originalRegistrationPinCount == 0 &&
            row.originalFactoryMarker == 0 && row.transferOriginal == nullptr &&
            row.transferProof == nullptr && row.clockProof == nullptr &&
            row.readers.pins.load(std::memory_order_acquire) == 0 &&
            row.lease.Capture().state == AudioSliceState::Ready)
            freeRow = index;
        if (state != LaneGroupState::Free && (row.cohortRoot == cohortRoot && row.selectedFutureDomain == domain) &&
            row.decision.slot == input.decision.slot &&
            row.decision.generation == input.decision.generation &&
            row.sliceIndex == input.sliceIndex) return false;
    }
    if (freeRow == config_.maxImmediateLaneGroupRows) return false;
    auto& row = immediateLaneGroups_[freeRow];
    if (row.generation == UINT32_MAX ||
        row.readers.pins.load(std::memory_order_acquire) != 0 ||
        row.lease.Capture().state != AudioSliceState::Ready) return false;
    ++row.generation;
    row.cohortRoot = cohortRoot;
    row.selectedFutureDomain = domain;
    row.laneOwner = nullptr;
    row.laneSlot = UINT32_MAX;
    row.laneGeneration = 0;
    row.bucketOrdinal = UINT32_MAX;
    row.groupIndex = UINT32_MAX;
    row.relativeFrame = input.executeFrame - provisionalAnchorFrame;
    row.rebasedFrame = 0;
    row.frameRebased = false;
    row.dueQuantum = UINT64_MAX;
    row.decision = input.decision;
    row.sliceIndex = input.sliceIndex;
    row.memberCount = input.memberCount;
    row.sourceControlCount = input.sourceControlCount;
    row.frontier = {};
    row.initialSoundQueueCompanion = {};
    row.frontierPinsOwned = false;
    row.frontierDependents = 0;
    row.readers.lastAudioSequence.store(0, std::memory_order_relaxed);
    auto* members = immediateLaneGroupMembers_.get() +
        freeRow * config_.maxArmsPerGroup;
    auto* memberIndices = immediateLaneGroupMemberIndices_.get() +
        freeRow * config_.maxArmsPerGroup;
    for (std::uint16_t inputIndex = 0; inputIndex < input.memberCount; ++inputIndex) {
        TimelineArmObservation observation{};
        (void)armTable.Observe(input.members[inputIndex], &observation);
        std::uint16_t outputIndex = inputIndex;
        while (outputIndex > 0) {
            TimelineArmObservation prior{};
            (void)armTable.Observe(members[outputIndex - 1], &prior);
            if (prior.memberIndex < observation.memberIndex) break;
            members[outputIndex] = members[outputIndex - 1];
            memberIndices[outputIndex] = memberIndices[outputIndex - 1];
            --outputIndex;
        }
        members[outputIndex] = input.members[inputIndex];
        memberIndices[outputIndex] = observation.memberIndex;
    }
    const Plan03GroupSliceIdentity memberOrigin{0, 1, input.decision, input.sliceIndex,
        freeRow, row.generation, epoch_, Plan03GroupBacking::ImmediateLane};
    if (!IssueGroupMemberOriginals(memberOrigin, armTable, members, input.memberCount,
            immediateGroupMemberOriginals_.get() + freeRow * config_.maxArmsPerGroup, immediateGroupIdentities_[freeRow])) {
        row.memberCount = 0;
        row.sourceControlCount = 0;
        row.cohortRoot = 0;
        row.selectedFutureDomain = 0;
        return false;
    }
    plan03ArmTable_ = &armTable;
    *out = {0, 1, input.decision, input.sliceIndex, freeRow,
            row.generation, epoch_, Plan03GroupBacking::ImmediateLane};
    row.transferOriginal = original;
    row.transferProof = nullptr;
    row.clockProof = nullptr;
    row.originalRegistrationPinCount = 0;
    row.originalFactoryMarker = 0;
    if (original != nullptr) {
        row.originalFactoryMarker = ++nextOriginalLaneGroupFactoryMarker_;
        original->originalMarker_ = row.originalFactoryMarker;
        original->owner_ = this;
        original->identity_ = *out;
        original->cohortRoot_ = cohortRoot;
        original->selectedFutureDomain_ = domain;
        original->provisionalAnchorFrame_ = provisionalAnchorFrame;
        original->executeFrame_ = input.executeFrame;
        original->memberCount_ = input.memberCount;
        original->sourceControlCount_ = input.sourceControlCount;
        original->immediateEntry_ = input.immediateEntry;
        original->armTable_ = &armTable;
        original->armTableIncarnation_ = armTable.Incarnation();
    }
    row.state.store(LaneGroupState::Private, std::memory_order_release);
    return true;
}

bool ScheduledTransactionPool::DeclareInitialSoundQueueCompanion(
    const std::array<const OriginalQueueFutureGroupRegistration*, 4>& originals,
    const ImmediateActionLane& lane, std::uint32_t laneSlot, std::uint32_t laneGeneration,
    std::uint64_t root, std::uint64_t attempt, std::uint64_t world,
    const FrontierOwnerIdentity& sound, const FrontierOwnerIdentity& queue,
    const FrontierQueueInitialIdentity& initial,
    const FrontierQueueConditionalIdentity& conditional) noexcept {
    if (root == 0 || attempt == 0 || world == 0 || !sound.Valid() || !queue.Valid() ||
        sound == queue || sound.child == queue.child || sound.parent != 0 || queue.parent != 0 ||
        sound.parentEpoch != 0 || queue.parentEpoch != 0 || sound.invocation != sound.child ||
        sound.invocationEpoch != sound.childEpoch || queue.invocation != sound.invocation ||
        queue.invocationEpoch != sound.invocationEpoch || !initial.Valid() || !conditional.Valid() ||
        initial.slot != conditional.slot || initial.generation != conditional.generation ||
        initial.version != conditional.currentVersion ||
        !lane.OwnsPrivate({laneSlot, laneGeneration}, root)) return false;
    InitialSoundQueueCompanionAssociation association{};
    association.root = root; association.attempt = attempt; association.world = world;
    association.lane = &lane; association.laneSlot = laneSlot; association.laneGeneration = laneGeneration;
    association.sound = sound; association.queue = queue;
    association.initial = initial; association.conditional = conditional;
    for (std::size_t index = 0; index < originals.size(); ++index) {
        if (originals[index] == nullptr || !IsOriginalQueueFutureGroupRegistrationFor(
                *originals[index], originals[index]->Identity(), root) ||
            (index != 0 && !IsOriginalQueueFutureGroupFamily(*originals[0], *originals[index])))
            return false;
        const auto identity = originals[index]->Identity();
        const auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
            row.selectedFutureDomain != 0 || row.initialSoundQueueCompanion.attempt != 0 ||
            row.frontier.role != FrontierGroupRole::Legacy ||
            row.laneOwner != nullptr || row.frameRebased || row.frontierPinsOwned ||
            row.transferOriginal != nullptr || row.transferProof != nullptr || row.clockProof != nullptr ||
            row.readers.pins.load(std::memory_order_acquire) != 0 ||
            row.readers.lastAudioSequence.load(std::memory_order_acquire) != 0 ||
            row.lease.Capture().state != AudioSliceState::Ready || row.sourceControlCount != 0 ||
            row.memberCount != (index == 3u ? 1u : 0u) || identity.sliceIndex != 0u)
            return false;
        for (std::size_t prior = 0; prior < index; ++prior)
            if (association.groups[prior] == identity) return false;
        association.groups[index] = identity;
        association.markers[index] = row.originalFactoryMarker;
    }
    const auto firstFrame = originals[0]->executeFrame_;
    const auto nextFrame = originals[2]->executeFrame_;
    if (firstFrame != originals[0]->provisionalAnchorFrame_ ||
        originals[1]->executeFrame_ != firstFrame || nextFrame <= firstFrame ||
        originals[3]->executeFrame_ != nextFrame) return false;
    // All preflight checks passed; existing original baselines own all four rows.
    for (const auto& identity : association.groups)
        immediateLaneGroups_[identity.committedRow].initialSoundQueueCompanion = association;
    return true;
}

bool ScheduledTransactionPool::MatchesInitialSoundQueueCompanionRows(
    const InitialSoundQueueCompanionAssociation& original, bool audio) const noexcept {
    if (original.attempt == 0 || original.root == 0 || original.world == 0 || original.lane == nullptr ||
        original.laneSlot == UINT32_MAX || original.laneGeneration == 0 || immediateLaneGroups_ == nullptr)
        return false;
    for (std::size_t index = 0; index < original.groups.size(); ++index) {
        const auto& identity = original.groups[index];
        if (!identity.Exact() || identity.backing != Plan03GroupBacking::ImmediateLane ||
            identity.transportEpoch != epoch_ || identity.absoluteQuantum != 0 || identity.pageGeneration != 1 ||
            identity.committedRow >= config_.maxImmediateLaneGroupRows || original.markers[index] == 0)
            return false;
        const auto& row = immediateLaneGroups_[identity.committedRow];
        const auto state = row.state.load(std::memory_order_acquire);
        if ((audio ? state != LaneGroupState::Published : state != LaneGroupState::Private) ||
            row.generation != identity.rowClaimGeneration || row.decision.slot != identity.decision.slot ||
            row.decision.generation != identity.decision.generation || row.sliceIndex != identity.sliceIndex ||
            row.cohortRoot != original.root || row.selectedFutureDomain != 0 ||
            (!audio && row.originalFactoryMarker != original.markers[index])) return false;
        const auto& link = row.initialSoundQueueCompanion;
        if (link.root != original.root || link.attempt != original.attempt || link.world != original.world ||
            link.lane != original.lane || link.laneSlot != original.laneSlot ||
            link.laneGeneration != original.laneGeneration || !(link.sound == original.sound) ||
            !(link.queue == original.queue) || !(link.initial == original.initial) ||
            link.conditional.slot != original.conditional.slot ||
            link.conditional.generation != original.conditional.generation ||
            link.conditional.currentVersion != original.conditional.currentVersion ||
            link.conditional.pendingVersion != original.conditional.pendingVersion ||
            link.conditional.current.slot != original.conditional.current.slot ||
            link.conditional.current.generation != original.conditional.current.generation ||
            link.conditional.successor.slot != original.conditional.successor.slot ||
            link.conditional.successor.generation != original.conditional.successor.generation ||
            link.groups != original.groups || link.markers != original.markers)
            return false;
        // Structural Return can clear the producer-only factory marker/count.
        // Audio reads the frozen association certificate until real fenced Free.
        if (audio && (row.laneOwner != original.lane || row.laneSlot != original.laneSlot ||
                row.laneGeneration != original.laneGeneration || !row.frameRebased)) return false;
    }
    return true;
}

bool ScheduledTransactionPool::MatchesInitialSoundQueueCompanionBinding(
    const Plan03GroupSliceIdentity& identity, const FrontierGroupBinding& binding,
    bool audio) const noexcept {
    const auto& original = immediateLaneGroups_[identity.committedRow].initialSoundQueueCompanion;
    if (original.attempt == 0) return true;
    if (!MatchesInitialSoundQueueCompanionRows(original, audio)) return false;
    std::size_t index = original.groups.size();
    for (std::size_t candidate = 0; candidate < original.groups.size(); ++candidate)
        if (identity == original.groups[candidate]) index = candidate;
    if (index == original.groups.size() || !binding.Valid() ||
        binding.entry.attempt != original.attempt || binding.operation.transport != identity.transportEpoch ||
        binding.operation.kind != (index == 3u ? FrontierOperationKind::Dependent : FrontierOperationKind::Entry) ||
        binding.operation.world != original.world || binding.variableSide.Valid() ||
        !binding.queueDescendant.Empty()) return false;
    const auto coordinator = index < 2u ? 0u : 2u;
    const auto& entryOwner = index < 2u ? original.sound : original.queue;
    const auto& operationOwner = index == 0u ? original.sound : original.queue;
    if (!(binding.entry.owner == entryOwner) || !(binding.operation.owner == operationOwner) ||
        binding.entry.coordinator.slot != original.groups[coordinator].decision.slot ||
        binding.entry.coordinator.generation != original.groups[coordinator].decision.generation ||
        !(binding.operation.group == identity) || !(binding.primary == original.groups[coordinator]) ||
        binding.role != (index == coordinator ? FrontierGroupRole::Coordinator : FrontierGroupRole::EntryMember))
        return false;
    if (index == 2u && (!binding.prerequisite.Valid() || binding.prerequisite.attempt != original.attempt ||
            !(binding.prerequisite.owner == original.sound) ||
            binding.prerequisite.coordinator.slot != original.groups[0].decision.slot ||
            binding.prerequisite.coordinator.generation != original.groups[0].decision.generation ||
            !(binding.prerequisiteMember == original.groups[1]))) return false;
    if (index == 1u)
        return binding.queueInitial == original.initial && !binding.queueConditional.Valid();
    if (index == 3u) {
        const auto& conditional = binding.queueConditional;
        const auto& expected = original.conditional;
        return !binding.queueInitial.Valid() && conditional.Valid() &&
            conditional.slot == expected.slot && conditional.generation == expected.generation &&
            conditional.currentVersion == expected.currentVersion && conditional.pendingVersion == expected.pendingVersion &&
            conditional.current.slot == expected.current.slot && conditional.current.generation == expected.current.generation &&
            conditional.successor.slot == expected.successor.slot && conditional.successor.generation == expected.successor.generation;
    }
    return !binding.queueInitial.Valid() && !binding.queueConditional.Valid();
}

bool ScheduledTransactionPool::MatchesInitialSoundQueueCompanionPublished(
    const InitialSoundQueueCompanionAssociation& original) const noexcept {
    if (!MatchesInitialSoundQueueCompanionRows(original, true)) return false;
    // Compare the complete same factory binding, including transport, versions,
    // occurrences and empty descendant. This observes, and never mints, the relation.
    for (const auto& identity : original.groups) {
        const auto& row = immediateLaneGroups_[identity.committedRow];
        if (!MatchesInitialSoundQueueCompanionBinding(identity, row.frontier, true)) return false;
    }
    const auto& c0 = immediateLaneGroups_[original.groups[0].committedRow];
    const auto& initial = immediateLaneGroups_[original.groups[1].committedRow];
    const auto& c1 = immediateLaneGroups_[original.groups[2].committedRow];
    const auto& conditional = immediateLaneGroups_[original.groups[3].committedRow];
    return c0.rebasedFrame == initial.rebasedFrame && c1.rebasedFrame == conditional.rebasedFrame &&
        c1.rebasedFrame > c0.rebasedFrame && c0.laneOwner == c1.laneOwner &&
        c0.laneSlot == c1.laneSlot && c0.laneGeneration == c1.laneGeneration &&
        c1.frontier.prerequisite == c0.frontier.entry &&
        c1.frontier.prerequisiteMember == original.groups[1] &&
        initial.frontier.entry == c0.frontier.entry && conditional.frontier.entry == c1.frontier.entry &&
        initial.frontier.queueInitial == original.initial &&
        conditional.frontier.queueConditional.slot == original.conditional.slot &&
        conditional.frontier.queueConditional.generation == original.conditional.generation &&
        conditional.frontier.queueConditional.currentVersion == original.conditional.currentVersion &&
        conditional.frontier.queueConditional.pendingVersion == original.conditional.pendingVersion &&
        conditional.frontier.queueConditional.current.slot == original.conditional.current.slot &&
        conditional.frontier.queueConditional.current.generation == original.conditional.current.generation &&
        conditional.frontier.queueConditional.successor.slot == original.conditional.successor.slot &&
        conditional.frontier.queueConditional.successor.generation == original.conditional.successor.generation &&
        EffectiveImmediateLaneGroupLease(original.groups[1].committedRow) == &c0.lease &&
        EffectiveImmediateLaneGroupLease(original.groups[3].committedRow) == &c1.lease && &c0.lease != &c1.lease;
}

bool ScheduledTransactionPool::IsInitialSoundQueueCompanionC0(
    const Plan03GroupSliceIdentity& primary, const Plan03GroupSliceIdentity& member,
    const FrontierOwnerIdentity& sound, const FrontierOwnerIdentity& queue) const noexcept {
    if (!primary.Exact() || primary.backing != Plan03GroupBacking::ImmediateLane ||
        primary.committedRow >= config_.maxImmediateLaneGroupRows || immediateLaneGroups_ == nullptr) return false;
    // The finalizer retains the actual Lane capture and group readers. Validate
    // the native Published full identity before reading its frozen association.
    Plan03PageGroupView actual{};
    if (!ResolvePlan03GroupSliceRt(primary, &actual) || !(actual.identity == primary) ||
        actual.frontier != &immediateLaneGroups_[primary.committedRow].frontier) return false;
    const auto& original = immediateLaneGroups_[primary.committedRow].initialSoundQueueCompanion;
    return original.groups[0] == primary && original.groups[1] == member &&
        original.sound == sound && original.queue == queue && MatchesInitialSoundQueueCompanionPublished(original);
}

bool ScheduledTransactionPool::IsInitialSoundQueueCompanionC1(
    const Plan03GroupSliceIdentity& primary, const Plan03GroupSliceIdentity& member,
    const Plan03GroupSliceIdentity& prerequisiteMember,
    const FrontierOwnerIdentity& sound, const FrontierOwnerIdentity& queue) const noexcept {
    if (!primary.Exact() || primary.backing != Plan03GroupBacking::ImmediateLane ||
        primary.committedRow >= config_.maxImmediateLaneGroupRows || immediateLaneGroups_ == nullptr) return false;
    // The finalizer retains the actual Lane capture and group readers. Validate
    // the native Published full identity before reading its frozen association.
    Plan03PageGroupView actual{};
    if (!ResolvePlan03GroupSliceRt(primary, &actual) || !(actual.identity == primary) ||
        actual.frontier != &immediateLaneGroups_[primary.committedRow].frontier) return false;
    const auto& original = immediateLaneGroups_[primary.committedRow].initialSoundQueueCompanion;
    return original.groups[2] == primary && original.groups[3] == member &&
        original.groups[1] == prerequisiteMember && original.sound == sound && original.queue == queue &&
        MatchesInitialSoundQueueCompanionPublished(original);
}

bool ScheduledTransactionPool::BindImmediateLaneGroupOwner(
    const Plan03GroupSliceIdentity& identity,
    const ImmediateActionLane& lane, std::uint64_t cohortRoot,
    std::uint32_t laneSlot, std::uint32_t laneGeneration) noexcept {
    return BindImmediateLaneGroupOwnerInDomain(identity, lane, cohortRoot, laneSlot, laneGeneration, 0);
}

bool ScheduledTransactionPool::BindImmediateLaneGroupOwnerInDomain(
    const Plan03GroupSliceIdentity& identity,
    const ImmediateActionLane& lane, std::uint64_t cohortRoot,
    std::uint32_t laneSlot, std::uint32_t laneGeneration, std::uint64_t domain) noexcept {
    if (!identity.Exact() ||
        identity.backing != Plan03GroupBacking::ImmediateLane ||
        identity.transportEpoch != epoch_ ||
        identity.absoluteQuantum != 0 || identity.pageGeneration != 1 ||
        identity.committedRow >= config_.maxImmediateLaneGroupRows ||
        cohortRoot == 0 || laneSlot == UINT32_MAX ||
        laneGeneration == 0 || immediateLaneGroups_ == nullptr ||
        !lane.OwnsPrivate({laneSlot, laneGeneration}, cohortRoot))
        return false;
    auto& row = immediateLaneGroups_[identity.committedRow];
    if (row.transferProof != nullptr || row.clockProof != nullptr || row.state.load(std::memory_order_acquire) !=
            LaneGroupState::Private ||
        (row.cohortRoot != cohortRoot || row.selectedFutureDomain != domain) ||
        row.generation != identity.rowClaimGeneration ||
        row.decision.slot != identity.decision.slot ||
        row.decision.generation != identity.decision.generation ||
        row.sliceIndex != identity.sliceIndex ||
        row.laneOwner != nullptr || row.laneSlot != UINT32_MAX ||
        row.laneGeneration != 0)
        return false;
    row.laneOwner = &lane;
    row.laneSlot = laneSlot;
    row.laneGeneration = laneGeneration;
    return true;
}

bool ScheduledTransactionPool::RebaseImmediateLaneGroups(
    std::uint64_t cohortRoot, std::uint64_t anchorFrame) noexcept {
    return RebaseImmediateLaneGroupsInDomain(cohortRoot, anchorFrame, 0);
}

bool ScheduledTransactionPool::RebaseImmediateLaneGroupsInDomain(
    std::uint64_t cohortRoot, std::uint64_t anchorFrame, std::uint64_t domain) noexcept {
    if (domain == 0 && InitialLaneCohortPublicationHeld(cohortRoot)) return false;
    if (cohortRoot == 0 || quantumFrames_ == 0) return false;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        if (row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
            (row.cohortRoot != cohortRoot || row.selectedFutureDomain != domain)) continue;
        if (row.transferProof != nullptr || row.clockProof != nullptr ||
            row.relativeFrame > UINT64_MAX - anchorFrame) return false;
    }
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        auto& row = immediateLaneGroups_[index];
        if (row.state.load(std::memory_order_acquire) == LaneGroupState::Private &&
            (row.cohortRoot == cohortRoot && row.selectedFutureDomain == domain)) {
            row.rebasedFrame = anchorFrame + row.relativeFrame;
            row.dueQuantum = row.rebasedFrame / quantumFrames_;
            row.frameRebased = true;
        }
    }
    return true;
}

bool ScheduledTransactionPool::PublishImmediateLaneGroups(
    std::uint64_t cohortRoot) noexcept {
    return PublishImmediateLaneGroupsInDomain(cohortRoot, 0);
}

bool ScheduledTransactionPool::PublishImmediateLaneGroupsInDomain(
    std::uint64_t cohortRoot, std::uint64_t domain) noexcept {
    if (domain == 0 && InitialLaneCohortPublicationHeld(cohortRoot)) return false;
    if (cohortRoot == 0) return false;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        if (row.state.load(std::memory_order_acquire) == LaneGroupState::Private &&
            (row.cohortRoot == cohortRoot && row.selectedFutureDomain == domain) &&
            (row.transferProof != nullptr || row.clockProof != nullptr || !row.frameRebased || row.laneOwner == nullptr ||
             row.laneSlot == UINT32_MAX ||
             row.laneGeneration == 0 ||
             row.bucketOrdinal == UINT32_MAX ||
             row.groupIndex == UINT32_MAX ||
             !row.laneOwner->OwnsPrivate(
                 {row.laneSlot, row.laneGeneration}, cohortRoot)))
            return false;
    }
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        auto& row = immediateLaneGroups_[index];
        if (row.state.load(std::memory_order_acquire) == LaneGroupState::Private &&
            (row.cohortRoot == cohortRoot && row.selectedFutureDomain == domain))
            row.state.store(LaneGroupState::Published, std::memory_order_release);
    }
    return true;
}

bool ScheduledTransactionPool::BindImmediateLaneGroupBuckets(
    const Plan03ImmediateLaneGroupDescriptor* groups,
    std::uint32_t count, const ImmediateActionLane& lane,
    std::uint32_t laneSlot, std::uint32_t laneGeneration) noexcept {
    if (groups == nullptr || count == 0 || immediateLaneGroups_ == nullptr ||
        laneSlot == UINT32_MAX || laneGeneration == 0) return false;
    const ImmediateLaneHandle handle{laneSlot, laneGeneration};
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& group = groups[index];
        const auto& identity = group.identity;
        if (!identity.Exact() ||
            identity.backing != Plan03GroupBacking::ImmediateLane ||
            identity.transportEpoch != epoch_ ||
            identity.absoluteQuantum != 0 || identity.pageGeneration != 1 ||
            identity.committedRow >= config_.maxImmediateLaneGroupRows ||
            group.laneSlot != laneSlot ||
            group.laneGeneration != laneGeneration ||
            group.bucketOrdinal == UINT32_MAX ||
            group.groupIndex != index ||
            !lane.ProvePrivateGroupPlacement(handle, identity,
                group.bucketOrdinal, group.groupIndex)) return false;
        const auto& row = immediateLaneGroups_[identity.committedRow];
        if (row.transferProof != nullptr || row.clockProof != nullptr ||
            row.state.load(std::memory_order_acquire) != LaneGroupState::Private ||
            row.generation != identity.rowClaimGeneration ||
            row.decision.slot != identity.decision.slot ||
            row.decision.generation != identity.decision.generation ||
            row.sliceIndex != identity.sliceIndex ||
            row.laneOwner != &lane || row.laneSlot != laneSlot ||
            row.laneGeneration != laneGeneration || !row.frameRebased ||
            row.dueQuantum != group.dueQuantum ||
            row.bucketOrdinal != UINT32_MAX ||
            row.groupIndex != UINT32_MAX) return false;
        for (std::uint32_t prior = 0; prior < index; ++prior)
            if (groups[prior].identity.committedRow == identity.committedRow)
                return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& row = immediateLaneGroups_[groups[index].identity.committedRow];
        row.bucketOrdinal = groups[index].bucketOrdinal;
        row.groupIndex = groups[index].groupIndex;
    }
    return true;
}

bool ScheduledTransactionPool::ValidateImmediateLaneGroupClaim(
    const Plan03GroupSliceIdentity& identity,
    const AudioSliceLease* lease,
    const ImmediateLaneBucketClaimReceipt& receipt) const noexcept {
    Plan03PageGroupView view{};
    if (lease == nullptr || !receipt.Valid() ||
        !ResolveImmediateLaneGroup(identity, &view, true) ||
        view.frontier == nullptr || view.lease != lease ||
        view.frontier->role == FrontierGroupRole::Legacy) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    return row.frameRebased && row.laneOwner != nullptr &&
        row.laneSlot == receipt.Slot() &&
        row.laneGeneration == receipt.Generation() &&
        row.dueQuantum == receipt.Quantum() &&
        row.rebasedFrame == view.frontier->operation.order.executeFrame &&
        view.frontier->operation.group == identity &&
        row.laneOwner->ValidateGroupClaimReceipt(
            receipt, identity, row.bucketOrdinal, row.groupIndex);
}

bool ScheduledTransactionPool::ProveImmediateLaneGroupBeforeClaim(
    const Plan03GroupSliceIdentity& identity,
    const AudioSliceLease* lease, const ImmediateActionLane& lane,
    ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket) const noexcept {
    Plan03PageGroupView view{};
    if (lease == nullptr || bucket == nullptr || !handle.Valid() ||
        !ResolveImmediateLaneGroup(identity, &view, true) ||
        view.frontier == nullptr || view.lease != lease ||
        view.frontier->role == FrontierGroupRole::Legacy)
        return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    ImmediateLaneGroupRef declared{};
    const auto* bucketGroups = lane.Groups(handle.slot, bucket);
    bool inBucket = false;
    for (std::uint32_t index = 0;
         bucketGroups != nullptr && index < bucket->groupCount; ++index)
        inBucket = inBucket || bucketGroups[index].identity == identity;
    return row.frameRebased && row.laneOwner == &lane &&
        inBucket && lane.IsGroupAdmitted(handle.slot, bucket, row.groupIndex) &&
        row.laneSlot == handle.slot &&
        row.laneGeneration == handle.generation &&
        row.dueQuantum == bucket->quantum &&
        row.rebasedFrame == view.frontier->operation.order.executeFrame &&
        view.frontier->operation.group == identity &&
        lane.CapturePublishedGroupDescriptor(handle, row.groupIndex,
            &declared) &&
        declared.identity == identity &&
        declared.bucketOrdinal == row.bucketOrdinal &&
        declared.groupIndex == row.groupIndex &&
        declared.dueQuantum == bucket->quantum;
}

std::size_t ScheduledTransactionPool::CapturePrivateSelectedLaneGroupsForPinning(
    const ImmediateActionLane& lane, ImmediateLaneHandle handle,
    std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
    std::size_t outCapacity) const noexcept {
    if (cohortRoot == 0 || immediateLaneGroups_ == nullptr ||
        !lane.OwnsPrivate(handle, cohortRoot)) return 0;
    std::size_t count = 0;
    std::size_t ordinaryCount = 0;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        const auto state = row.state.load(std::memory_order_acquire);
        // A foreign or unbound domain sharing the numeric root does not belong
        // to this attempt and must not poison its ordinary-domain fallback.
        if (state == LaneGroupState::Free || row.cohortRoot != cohortRoot ||
            row.laneOwner != &lane || row.laneSlot != handle.slot ||
            row.laneGeneration != handle.generation) continue;
        if (row.selectedFutureDomain == 0) { ++ordinaryCount; continue; }
        // The genuine Domain factory stores its own marker as domain and root.
        // Same-Lane mixtures cannot acquire a partial manifest here.
        if (row.selectedFutureDomain != cohortRoot || state != LaneGroupState::Private)
            return outCapacity + 1u;
        if (out != nullptr && count < outCapacity)
            out[count] = {{0, 1, row.decision, row.sliceIndex, index,
                row.generation, epoch_, Plan03GroupBacking::ImmediateLane},
                row.dueQuantum, false, row.laneSlot, row.laneGeneration};
        ++count;
    }
    return count != 0 && ordinaryCount != 0 ? outCapacity + 1u : count;
}

std::size_t ScheduledTransactionPool::CaptureImmediateLaneCohortGroups(
    std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
    std::size_t outCapacity) const noexcept {
    return CaptureImmediateLaneCohortGroupsImpl(cohortRoot, out, outCapacity, false);
}

std::size_t ScheduledTransactionPool::CaptureLiveImmediateLaneCohortGroups(
    std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
    std::size_t outCapacity) const noexcept {
    return CaptureImmediateLaneCohortGroupsImpl(cohortRoot, out, outCapacity, true);
}

std::size_t ScheduledTransactionPool::CaptureImmediateLaneCohortGroupsImpl(
    std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
    std::size_t outCapacity, bool includePublished) const noexcept {
    std::size_t count = 0;
    if (cohortRoot == 0 || immediateLaneGroups_ == nullptr) return 0;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        const auto state = row.state.load(std::memory_order_acquire);
        if ((state != LaneGroupState::Private &&
             (!includePublished || state != LaneGroupState::Published)) ||
            (row.cohortRoot != cohortRoot || row.selectedFutureDomain != 0)) continue;
        if (out != nullptr && count < outCapacity) {
            out[count] = {{0, 1, row.decision, row.sliceIndex, index,
                row.generation, epoch_, Plan03GroupBacking::ImmediateLane},
                row.dueQuantum, false, row.laneSlot, row.laneGeneration};
        }
        ++count;
    }
    return count;
}

std::size_t ScheduledTransactionPool::CaptureImmediateLaneGroupsRt(
    std::uint64_t absoluteQuantum, Plan03PageGroupView* out,
    std::size_t outCapacity) const noexcept {
    std::size_t count = 0;
    if (immediateLaneGroups_ == nullptr) return 0;
    for (std::size_t index = 0; index < config_.maxImmediateLaneGroupRows; ++index) {
        const auto& row = immediateLaneGroups_[index];
        if (row.state.load(std::memory_order_acquire) != LaneGroupState::Published ||
            row.dueQuantum != absoluteQuantum) continue;
        if (out != nullptr && count < outCapacity) {
            const Plan03GroupSliceIdentity identity{
                0, 1, row.decision, row.sliceIndex, index, row.generation,
                epoch_, Plan03GroupBacking::ImmediateLane};
            if (!ResolveImmediateLaneGroup(identity, &out[count], true))
                return outCapacity + 1u;
        }
        ++count;
    }
    return count;
}

AudioSliceLease* ScheduledTransactionPool::EffectiveImmediateLaneGroupLease(
    std::size_t rowIndex) const noexcept {
    if (rowIndex >= config_.maxImmediateLaneGroupRows) return nullptr;
    const auto& row = immediateLaneGroups_[rowIndex];
    if (row.frontier.role != FrontierGroupRole::EntryMember)
        return const_cast<AudioSliceLease*>(&row.lease);
    const auto& primary = row.frontier.primary;
    if (primary.backing != Plan03GroupBacking::ImmediateLane ||
        primary.committedRow >= config_.maxImmediateLaneGroupRows ||
        primary.committedRow == rowIndex) return nullptr;
    const auto& coordinator = immediateLaneGroups_[primary.committedRow];
    if (coordinator.generation != primary.rowClaimGeneration ||
        coordinator.cohortRoot != row.cohortRoot ||
        coordinator.selectedFutureDomain != row.selectedFutureDomain ||
        coordinator.frontier.role != FrontierGroupRole::Coordinator ||
        coordinator.state.load(std::memory_order_acquire) == LaneGroupState::Free)
        return nullptr;
    if (!row.frameRebased || !coordinator.frameRebased ||
        row.laneOwner == nullptr ||
        row.laneOwner != coordinator.laneOwner ||
        row.laneSlot != coordinator.laneSlot ||
        row.laneGeneration != coordinator.laneGeneration)
        return nullptr;
    const bool bothPrivate = row.bucketOrdinal == UINT32_MAX &&
        coordinator.bucketOrdinal == UINT32_MAX;
    const bool bothPublished = row.bucketOrdinal != UINT32_MAX &&
        coordinator.bucketOrdinal != UINT32_MAX;
    if (!bothPrivate && !bothPublished) return nullptr;
    if (row.dueQuantum != coordinator.dueQuantum ||
        (bothPublished && row.bucketOrdinal != coordinator.bucketOrdinal))
        return const_cast<AudioSliceLease*>(&row.lease);
    return const_cast<AudioSliceLease*>(&coordinator.lease);
}

bool ScheduledTransactionPool::ResolveImmediateLaneGroup(
    const Plan03GroupSliceIdentity& identity, Plan03PageGroupView* out,
    bool audio) const noexcept {
    const Plan03GroupSliceIdentity exact = identity;
    if (out == nullptr || !identity.Exact() ||
        identity.backing != Plan03GroupBacking::ImmediateLane ||
        identity.transportEpoch != epoch_ || identity.absoluteQuantum != 0 ||
        identity.pageGeneration != 1 ||
        identity.committedRow >= config_.maxImmediateLaneGroupRows ||
        immediateLaneGroups_ == nullptr) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    const auto state = row.state.load(std::memory_order_acquire);
    if ((audio && state != LaneGroupState::Published) ||
        (!audio && state != LaneGroupState::Private &&
         state != LaneGroupState::Published) ||
        row.generation != identity.rowClaimGeneration ||
        row.decision.slot != identity.decision.slot ||
        row.decision.generation != identity.decision.generation ||
        row.sliceIndex != identity.sliceIndex) return false;
    if (audio) {
        ImmediateLaneObservation observation{};
        if (row.laneOwner == nullptr ||
            !row.laneOwner->Observe(
                {row.laneSlot, row.laneGeneration}, &observation) ||
            !observation.committed)
            return false;
    }
    *out = {};
    out->identity = exact;
    out->memberCount = row.memberCount;
    out->sourceControlCount = row.sourceControlCount;
    out->members = row.memberCount == 0 ? nullptr :
        immediateLaneGroupMembers_.get() +
        exact.committedRow * config_.maxArmsPerGroup;
    out->lease = EffectiveImmediateLaneGroupLease(exact.committedRow);
    out->frontier = row.frontier.role == FrontierGroupRole::Legacy ? nullptr :
        &row.frontier;
    return out->lease != nullptr;
}

bool ScheduledTransactionPool::PrivatePlan03GroupBindable(
    const Plan03GroupSliceIdentity& identity) const noexcept {
    Plan03PageGroupView view{};
    if (identity.backing == Plan03GroupBacking::ImmediateLane)
        return ResolveImmediateLaneGroup(identity, &view, false) &&
            immediateLaneGroups_[identity.committedRow].publicationProof == nullptr &&
            immediateLaneGroups_[identity.committedRow].transferProof == nullptr &&
            immediateLaneGroups_[identity.committedRow].clockProof == nullptr &&
            immediateLaneGroups_[identity.committedRow].state.load(
                std::memory_order_acquire) == LaneGroupState::Private &&
            view.lease->Capture().state == AudioSliceState::Ready &&
            immediateLaneGroups_[identity.committedRow].readers.lastAudioSequence.load(
                std::memory_order_acquire) == 0;
    return PageStamp(identity.absoluteQuantum) != identity.absoluteQuantum &&
        ResolvePlan03GroupSliceProducer(identity, &view);
}

bool ScheduledTransactionPool::PrivateImmediateLaneGroupRetirementReady(
    const Plan03GroupSliceIdentity& identity, std::uint64_t completedAudioSequence) const noexcept {
    Plan03PageGroupView group{};
    if (identity.backing != Plan03GroupBacking::ImmediateLane ||
        !ResolveImmediateLaneGroup(identity, &group, false)) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    const auto last = row.readers.lastAudioSequence.load(std::memory_order_acquire);
    const auto lease = row.lease.Capture();
    return row.state.load(std::memory_order_acquire) == LaneGroupState::Private &&
        row.transferProof == nullptr && row.clockProof == nullptr && !row.frontierPinsOwned && row.frontierDependents == 0 &&
        row.readers.pins.load(std::memory_order_acquire) == 0 &&
        (last == 0 || completedAudioSequence > last) &&
        (lease.state == AudioSliceState::Ready ||
            (lease.state == AudioSliceState::SliceInvalidated &&
             lease.pendingReasonBits == AudioSliceReasonCancel));
}

bool ScheduledTransactionPool::MatchesPrivateImmediateLaneGroupPlacement(
    const Plan03GroupSliceIdentity& identity, const ImmediateActionLane& lane,
    const PrivateImmediateLanePlacementProof& placement) const noexcept {
    if (identity.backing != Plan03GroupBacking::ImmediateLane || !PrivatePlan03GroupBindable(identity)) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    std::uint64_t anchor = 0;
    return quantumFrames_ != 0 && row.laneOwner == &lane && row.frameRebased &&
        lane.IsPrivatePlacementProofFor(placement, {row.laneSlot, row.laneGeneration}, row.cohortRoot) &&
        placement.CaptureAnchorFrame(&anchor) && row.relativeFrame <= UINT64_MAX - anchor &&
        row.rebasedFrame == anchor + row.relativeFrame && row.dueQuantum == row.rebasedFrame / quantumFrames_;
}

bool ScheduledTransactionPool::PrivateImmediateLaneFrontierRollbackReady(
    const Plan03GroupSliceIdentity& identity) const noexcept {
    Plan03PageGroupView group{};
    if (identity.backing != Plan03GroupBacking::ImmediateLane ||
        !PrivatePlan03GroupBindable(identity) ||
        !ResolveImmediateLaneGroup(identity, &group, false) ||
        group.frontier == nullptr || !group.frontier->Valid()) return false;
    const auto& row = immediateLaneGroups_[identity.committedRow];
    const auto& owner = group.frontier->operation.owner;
    if (!row.frontierPinsOwned ||
        owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
        identity.decision.slot >= config_.maxImmediateLaneDecisionGates)
        return false;
    const auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
    const auto& decisionGate = immediateLaneDecisionGates_[identity.decision.slot];
    return (ownerGate.state.load(std::memory_order_acquire) & kLaneGateClaims) == 0 &&
        (decisionGate.state.load(std::memory_order_acquire) & kLaneGateClaims) == 0;
}

bool ScheduledTransactionPool::ClaimImmediateLaneFrontier(
    const Plan03GroupSliceIdentity& identity,
    ImmediateLaneFrontierClaim* out) const noexcept {
    return ClaimImmediateLaneFrontierBound(identity, out, false);
}

bool ScheduledTransactionPool::ClaimPrivateImmediateLaneFrontier(
    const Plan03GroupSliceIdentity& identity,
    ImmediateLaneFrontierClaim* out) const noexcept {
    return ClaimImmediateLaneFrontierBound(identity, out, true);
}

bool ScheduledTransactionPool::ClaimImmediateLaneFrontierBound(
    const Plan03GroupSliceIdentity& identity,
    ImmediateLaneFrontierClaim* out, bool producerPrivate) const noexcept {
    Plan03PageGroupView group{};
    if (out == nullptr || out->Valid() ||
        (producerPrivate && !PrivatePlan03GroupBindable(identity)) ||
        !ResolveImmediateLaneGroup(identity, &group, !producerPrivate) ||
        group.frontier == nullptr || !group.frontier->Valid() ||
        !immediateLaneGroups_[identity.committedRow].frontierPinsOwned)
        return false;
    const auto& owner = group.frontier->operation.owner;
    if (owner.serviceSlot >= config_.maxImmediateLaneOwnerGates ||
        identity.decision.slot >= config_.maxImmediateLaneDecisionGates)
        return false;
    auto& ownerGate = immediateLaneOwnerGates_[owner.serviceSlot];
    auto& decisionGate = immediateLaneDecisionGates_[identity.decision.slot];
    if (decisionGate.ownerSlot.load(std::memory_order_acquire) != owner.serviceSlot ||
        decisionGate.ownerGeneration.load(std::memory_order_acquire) !=
            owner.serviceGeneration) return false;
    const auto claimGate = [](LaneFrontierGate& gate,
                              std::uint64_t generation) noexcept {
        if (gate.generation.load(std::memory_order_acquire) != generation ||
            (gate.state.load(std::memory_order_acquire) & kLaneGateInvalid) != 0)
            return false;
        auto state = gate.state.load(std::memory_order_acquire);
        if ((state & kLaneGateInvalid) != 0 ||
            (state & kLaneGateClaims) == kLaneGateClaims ||
            !gate.state.compare_exchange_strong(state, state + 1u,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        return true;
    };
    const auto& native = immediateLaneGroups_[identity.committedRow];
    if (!producerPrivate && native.laneOwner != nullptr &&
        !native.laneOwner->BeforeConditionalProjectionNativeWrite(
            {native.laneSlot, native.laneGeneration}, identity)) return false;
    if (!claimGate(ownerGate, owner.serviceGeneration)) return false;
    if (!claimGate(decisionGate, identity.decision.generation)) {
        ownerGate.state.fetch_sub(1u, std::memory_order_acq_rel);
        return false;
    }
    *out = {owner.serviceSlot, owner.serviceGeneration,
            identity.decision.slot, identity.decision.generation};
    return true;
}

bool ScheduledTransactionPool::ReleaseImmediateLaneFrontier(
    ImmediateLaneFrontierClaim* claim) const noexcept {
    if (claim == nullptr || !claim->Valid() ||
        claim->ownerSlot >= config_.maxImmediateLaneOwnerGates ||
        claim->decisionSlot >= config_.maxImmediateLaneDecisionGates)
        return false;
    auto& ownerGate = immediateLaneOwnerGates_[claim->ownerSlot];
    auto& decisionGate = immediateLaneDecisionGates_[claim->decisionSlot];
    if (ownerGate.generation.load(std::memory_order_acquire) !=
            claim->ownerGeneration ||
        decisionGate.generation.load(std::memory_order_acquire) !=
            claim->decisionGeneration ||
        (ownerGate.state.load(std::memory_order_acquire) & kLaneGateClaims) == 0 ||
        (decisionGate.state.load(std::memory_order_acquire) & kLaneGateClaims) == 0)
        return false;
    decisionGate.state.fetch_sub(1u, std::memory_order_acq_rel);
    ownerGate.state.fetch_sub(1u, std::memory_order_acq_rel);
    *claim = {};
    return true;
}

std::uint32_t ScheduledTransactionPool::Advance() {
    if ( preparedPageFrontierBindingHolds_ != 0 || privateUnboundPageArmRetirements_ != nullptr ||
        unpublishedGroupArmCancellations_ != nullptr) return 0;
    if (!Valid()) {
        return 0;
    }
    std::uint64_t heldIngress = 0;
    for (const auto& claim : preparedTransactionClaims_) {
        if (claim.transferProof == nullptr) continue;
        assert(claim.used && !claim.committed && claim.preparedIngressQuantum == baseQuantumIndex_);
        heldIngress += claim.transactionCount;
    }
    assert(heldIngress <= preparedIngressThisQuantum_);
    if (WindowEndFrame() > UINT64_MAX - quantumFrames_) {
        if (heldIngress != 0) return 0;
        // Absolute frame geometry never folds back to zero. Leave every page
        // and frontier value untouched, enter the terminal invalid state, and
        // let the session recreate the Runtime under its recreate-required
        // policy.
        terminalState_.store(1, std::memory_order_release);
        return 0;
    }
    const std::uint32_t leaving = ringStart_;
    // A claim on a later page may remain held while this unrelated page leaves
    // the horizon. Its own page hold prevents that page from advancing.
    if (pagePreparedPlan03GroupCount_[leaving] != 0 ||
        pagePublicationHolds_[leaving] != 0) {
        ++advanceBlockedByConsumer_;
        return 0;
    }
    if (consumerFrontierEnabled_ &&
        pageHeaders_[leaving].absoluteQuantumIndex >=
            consumedFrontierQuantum_->load(std::memory_order_acquire)) {
        ++advanceBlockedByConsumer_;
        return 0;
    }
    const std::size_t groupRowBase =
        static_cast<std::size_t>(leaving) *
        config_.maxPlan03GroupHeadersPerPage;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        const std::size_t rowIndex = groupRowBase + group;
        if (pagePlan03GroupReaders_[rowIndex].pins.load(std::memory_order_acquire) != 0) {
            ++advanceBlockedByConsumer_;
            return 0;
        }
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) {
            continue;
        }
        if (plan03ArmTable_ == nullptr) {
            ++advanceBlockedByConsumer_;
            return 0;
        }
        const Plan03GroupSliceRow& row =
            pagePlan03Groups_[rowIndex];
        const std::size_t memberBase =
            rowIndex * config_.maxArmsPerGroup;
        bool memberLive = false;
        for (std::uint16_t member = 0; member < row.memberCount; ++member) {
            TimelineArmObservation observation{};
            if (plan03ArmTable_->Observe(
                    pagePlan03GroupMembers_[memberBase + member],
                    &observation) &&
                observation.state != ArmState::CancelRequested &&
                observation.state != ArmState::Tombstoned) {
                memberLive = true;
                break;
            }
        }
        if (!memberLive) {
            continue;
        }
        const auto* effectiveLease = EffectivePlan03GroupLease(rowIndex);
        if (effectiveLease == nullptr) {
            ++advanceBlockedByConsumer_;
            return 0;
        }
        const AudioSliceState state = effectiveLease->Capture().state;
        if (state != AudioSliceState::SliceApplied &&
            state != AudioSliceState::SliceInvalidated) {
            ++advanceBlockedByConsumer_;
            return 0;
        }
        ++advanceBlockedByConsumer_;
        return 0;
    }
    if (pageHeaders_[leaving].pageGeneration == UINT64_MAX) {
        if (heldIngress != 0) {
            return 0;
        }
        terminalState_.store(1, std::memory_order_release);
        return 0;
    }
    pageQuantumStamp_[leaving].store(kUnpublishedQuantumStamp,
                                     std::memory_order_release);
    pageEmptyQuantumStamp_[leaving].store(kUnpublishedQuantumStamp,
                                          std::memory_order_release);
    const std::uint32_t dropped = pageEventCount_[leaving];
    const std::size_t eventBase =
        static_cast<std::size_t>(leaving) * config_.maxEventsPerQuantum;

    for (std::uint32_t i = 0; i < dropped; ++i) {
        const EventRow& row = pageEvents_[eventBase + i];
        TransactionRecord& rec = transactions_[row.txnSlot];
        if (!rec.used || rec.id != row.transactionId) {
            continue;
        }
        if (rec.remainingEvents > 0) {
            --rec.remainingEvents;
        }
        if (rec.remainingEvents == 0) {
            rec.used = false;
            freeTransactions_.push_back(row.txnSlot);
            if (liveTransactionCount_ > 0) {
                --liveTransactionCount_;
            }
        }
    }

    pageEventCount_[leaving] = 0;
    pagePreparedEventCount_[leaving] = 0;
    pageCurrentSliceCount_[leaving] = 0;
    for (std::uint32_t group = 0;
         group < config_.maxPlan03GroupHeadersPerPage; ++group) {
        const std::size_t rowIndex = groupRowBase + group;
        if (pagePlan03GroupRowStates_[rowIndex] !=
            Plan03GroupRowState::Live) {
            continue;
        }
        (void)pagePlan03GroupLeases_[rowIndex].ResetForReuse();
        const std::uint16_t memberCount =
            pagePlan03Groups_[rowIndex].memberCount;
        const std::size_t memberBase = rowIndex * config_.maxArmsPerGroup;
        ReturnGroupMemberOriginals(pageGroupMemberOriginals_.get() + memberBase, memberCount);
        std::fill(pagePlan03GroupMembers_.begin() + memberBase,
                  pagePlan03GroupMembers_.begin() + memberBase + memberCount,
                  TimelineArmToken{});
        pagePlan03Groups_[rowIndex] = {};
        pagePlan03GroupRowStates_[rowIndex] =
            pagePlan03GroupClaimGenerations_[rowIndex] == UINT32_MAX
                ? Plan03GroupRowState::Retired
                : Plan03GroupRowState::Free;
    }
    pagePlan03GroupCount_[leaving] = 0;
    pageBatchCount_[leaving] = 0;
    pageBatchOccupiedCount_[leaving] = 0;
    const std::size_t batchBase =
        static_cast<std::size_t>(leaving) *
        config_.maxFrameBatchesPerQuantum;
    std::fill(pageBatchRows_.begin() + batchBase,
              pageBatchRows_.begin() + batchBase +
                  config_.maxFrameBatchesPerQuantum,
              FrameBatchRow{});
    pageSorted_[leaving] = 1;

    // Reopen the leaving slot at the far end: a new generation and the next
    // absolute quantum index, so a reference to the old occupant cannot pass for
    // the new one.
    const std::uint64_t newAbsoluteQuantum = baseQuantumIndex_ + pageCount_;
    HorizonPageHeader& header = pageHeaders_[leaving];
    header.pageGeneration += 1;
    header.absoluteQuantumIndex = newAbsoluteQuantum;
    header.transportEpoch = epoch_;
    header.frozen = false;
    header.publishCutoffFrame = 0;
    header.committedTransactionCount = 0;
    header.plan03GroupCount = 0;

    ringStart_ = (ringStart_ + 1) % pageCount_;
    baseFrame_ += quantumFrames_;
    baseQuantumIndex_ += 1;
    ingressThisQuantum_ = 0; // ingress is bounded per quantum
    preparedIngressThisQuantum_ = static_cast<std::uint32_t>(heldIngress);
    for (auto& claim : preparedTransactionClaims_)
        if (claim.transferProof != nullptr) claim.preparedIngressQuantum = baseQuantumIndex_;
    unplacedIngressThisQuantum_ = 0;
    return dropped;
}

std::uint64_t ScheduledTransactionPool::RefusalCount(
    ReserveRefusal reason) const {
    const std::size_t index = static_cast<std::size_t>(reason);
    if (index >= refusalCounts_.size()) {
        return 0;
    }
    return refusalCounts_[index];
}

bool ScheduledTransactionPool::HasLiveProgramGeneration(
    ProgramGeneration generation) const {
    if (generation == 0) {
        return false;
    }
    for (const TransactionRecord& transaction : transactions_) {
        if (transaction.used &&
            transaction.program.programGeneration == generation &&
            transaction.remainingEvents != 0) {
            return true;
        }
    }
    return false;
}

bool ScheduledTransactionPool::HasProgramReference(
    TimelineTransactionProgramRef program) const {
    if (program.program == nullptr || program.programGeneration == 0) {
        return false;
    }
    for (const TransactionRecord& transaction : transactions_) {
        if ((transaction.used || transaction.prepared) &&
            transaction.remainingEvents != 0 &&
            transaction.program.program == program.program &&
            transaction.program.programGeneration == program.programGeneration) {
            return true;
        }
    }
    return false;
}

} // namespace harken
