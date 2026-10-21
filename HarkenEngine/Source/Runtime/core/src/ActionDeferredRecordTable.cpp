#include "harken/core/ActionDeferredRecordTable.h"

#include <algorithm>
#include <cassert>
#include <new>

namespace harken {

namespace {
bool DeferredPrecedes(const DeferredIntent& a, std::uint32_t aSlot,
                      const DeferredIntent& b, std::uint32_t bSlot) noexcept {
    if (a.dueFrame != b.dueFrame) return a.dueFrame < b.dueFrame;
    if (a.commitSequence != b.commitSequence) return a.commitSequence < b.commitSequence;
    if (a.executionOrdinal != b.executionOrdinal) return a.executionOrdinal < b.executionOrdinal;
    if (a.payload != b.payload) return a.payload < b.payload;
    return aSlot < bSlot;
}
}

bool ActionDeferredRecordTable::Reset(std::uint32_t capacity) {
    if (capacity == 0 || preparedPairLoans_ != 0) {
        return false;
    }
    StableRowStorage<Row> rows;
    if (!rows.Reserve(capacity)) return false;
    rows_.Swap(rows);
    preparedCount_ = 0;
    liveCount_ = 0;
    highWater_ = 0;
    quarantinedCount_ = 0;
    return true;
}

ActionDeferredRecordTable::Row* ActionDeferredRecordTable::Resolve(
    ActionDeferredRecordToken token) noexcept {
    if (!token.Valid() || token.slot >= rows_.size()) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    return row.generation == token.generation ? &row : nullptr;
}

const ActionDeferredRecordTable::Row* ActionDeferredRecordTable::Resolve(
    ActionDeferredRecordToken token) const noexcept {
    if (!token.Valid() || token.slot >= rows_.size()) {
        return nullptr;
    }
    const Row& row = rows_[token.slot];
    return row.generation == token.generation ? &row : nullptr;
}

ActionDeferredRecordPrepareResult ActionDeferredRecordTable::Prepare(
    const ActionDeferredRecord& record) noexcept {
    if (record.firing == 0 || record.ownerCancellationEpoch == 0 ||
        !record.owner.Valid() ||
        record.firstRowFrame > record.lastRowFrame ||
        record.lastRowFrame > record.deadlineFrame) {
        return {ActionDeferredRecordPrepareStatus::Invalid, {}};
    }
    if (!rows_.ConstructThrough(rows_.Capacity()))
        return {ActionDeferredRecordPrepareStatus::Capacity, {}};
    for (std::uint32_t slot = 0; slot < rows_.size(); ++slot) {
        Row& row = rows_[slot];
        if (row.state != ActionDeferredRecordState::Free) {
            continue;
        }
        row.state = ActionDeferredRecordState::Prepared;
        row.record = record;
        row.poolToken = {};
        row.heldCancelOwned = false;
        row.retryReservation = {};
        ++preparedCount_;
        return {ActionDeferredRecordPrepareStatus::Prepared,
                {slot, row.generation}};
    }
    return {ActionDeferredRecordPrepareStatus::Capacity, {}};
}

bool ActionDeferredRecordTable::Abandon(
    ActionDeferredRecordToken* token) noexcept {
    if (token == nullptr) {
        return false;
    }
    Row* row = Resolve(*token);
    if (row == nullptr || row->state != ActionDeferredRecordState::Prepared ||
        row->preparedPairMarker != 0) {
        return false;
    }
    --preparedCount_;
    row->state = ActionDeferredRecordState::Free;
    row->record = {};
    row->poolToken = {};
    row->heldCancelOwned = false;
    row->retryReservation = {};
    *token = {};
    return true;
}

bool ActionDeferredRecordTable::Commit(ActionDeferredRecordToken* token,
                                       DeferredIntentToken poolToken) noexcept {
    if (token == nullptr || !poolToken.Valid()) {
        return false;
    }
    Row* row = Resolve(*token);
    if (row == nullptr || row->state != ActionDeferredRecordState::Prepared ||
        row->preparedPairMarker != 0) {
        return false;
    }
    CommitValidated(*row, token, poolToken);
    return true;
}

ActionDeferredRecordTable::~ActionDeferredRecordTable() {
    assert(preparedPairLoans_ == 0);
}

bool ActionDeferredRecordTable::PreparedPairCommitProof::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesPreparedPair(*this);
}

void ActionDeferredRecordTable::PreparedPairCommitProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleasePreparedPair(*this);
    owner_ = nullptr;
    pool_ = nullptr;
    record_ = {};
    poolToken_ = {};
    marker_ = 0;
}

bool ActionDeferredRecordTable::ProvePreparedPair(
    ActionDeferredRecordToken token, DeferredIntentPool& pool,
    DeferredIntentToken poolToken, PreparedPairCommitProof* out) noexcept {
    if (out == nullptr || out->owner_ != nullptr ||
        nextPreparedPairMarker_ == UINT64_MAX || preparedPairLoans_ == UINT32_MAX)
        return false;
    Row* row = Resolve(token);
    auto* poolEntry = pool.Validate(poolToken, DeferredIntentEntryState::Prepared);
    if (row == nullptr || row->state != ActionDeferredRecordState::Prepared ||
        row->preparedPairMarker != 0 || poolEntry == nullptr ||
        poolEntry->preparedPairOwner != nullptr ||
        poolEntry->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
        poolEntry->intent.ownerHandle != row->record.firing ||
        poolEntry->intent.ownerEpoch != row->record.ownerCancellationEpoch ||
        poolEntry->intent.payload != token.slot ||
        poolEntry->intent.dueFrame != row->record.intendedBoundaryFrame ||
        poolEntry->intent.executionOrdinal != row->record.executionOrdinal)
        return false;
    const auto marker = ++nextPreparedPairMarker_;
    row->preparedPairMarker = marker;
    poolEntry->preparedPairOwner = this;
    poolEntry->preparedPairMarker = marker;
    ++preparedPairLoans_;
    out->owner_ = this;
    out->pool_ = &pool;
    out->record_ = token;
    out->poolToken_ = poolToken;
    out->marker_ = marker;
    return true;
}

bool ActionDeferredRecordTable::MatchesPreparedPair(
    const PreparedPairCommitProof& proof) const noexcept {
    if (proof.owner_ != this || proof.pool_ == nullptr || proof.marker_ == 0)
        return false;
    const Row* row = Resolve(proof.record_);
    const auto* entry = proof.pool_->Validate(proof.poolToken_, DeferredIntentEntryState::Prepared);
    return row != nullptr && row->state == ActionDeferredRecordState::Prepared &&
        row->preparedPairMarker == proof.marker_ && entry != nullptr &&
        entry->preparedPairOwner == this && entry->preparedPairMarker == proof.marker_;
}

void ActionDeferredRecordTable::ReleasePreparedPair(PreparedPairCommitProof& proof) noexcept {
    assert(MatchesPreparedPair(proof));
    if (!MatchesPreparedPair(proof)) return;
    auto& row = *Resolve(proof.record_);
    auto& entry = *proof.pool_->Validate(proof.poolToken_, DeferredIntentEntryState::Prepared);
    row.preparedPairMarker = 0;
    entry.preparedPairOwner = nullptr;
    entry.preparedPairMarker = 0;
    --preparedPairLoans_;
}

void ActionDeferredRecordTable::ConsumePreparedPair(PreparedPairCommitProof&& proof) noexcept {
    assert(MatchesPreparedPair(proof));
    if (!MatchesPreparedPair(proof)) return;
    auto& row = *Resolve(proof.record_);
    auto& entry = *proof.pool_->Validate(proof.poolToken_, DeferredIntentEntryState::Prepared);
    row.preparedPairMarker = 0;
    entry.preparedPairOwner = nullptr;
    entry.preparedPairMarker = 0;
    --preparedPairLoans_;
    proof.pool_->CommitValidatedPrepared(entry);
    auto consumed = proof.record_;
    CommitValidated(row, &consumed, proof.poolToken_);
    proof.owner_ = nullptr;
    proof.pool_ = nullptr;
    proof.record_ = {};
    proof.poolToken_ = {};
    proof.marker_ = 0;
}

bool ActionDeferredRecordTable::CommitPreparedPair(
    ActionDeferredRecordToken* token, DeferredIntentPool* pool,
    DeferredIntentToken poolToken) noexcept {
    if (token == nullptr || pool == nullptr) return false;
    PreparedPairCommitProof proof;
    if (!ProvePreparedPair(*token, *pool, poolToken, &proof)) return false;
    ConsumePreparedPair(std::move(proof));
    *token = {};
    return true;
}

void ActionDeferredRecordTable::CommitValidated(
    Row& row, ActionDeferredRecordToken* token,
    DeferredIntentToken poolToken) noexcept {
    row.poolToken = poolToken;
    row.heldCancelOwned = false;
    row.retryReservation = {};
    row.state = ActionDeferredRecordState::Held;
    --preparedCount_;
    ++liveCount_;
    highWater_ = std::max(highWater_, liveCount_);
    *token = {};
}

bool ActionDeferredRecordTable::Claim(
    const DeferredIntentClaim& claim, ActionDeferredRecordToken* outToken,
    ActionDeferredRecord* outRecord) noexcept {
    if (outToken == nullptr || outRecord == nullptr ||
        claim.intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
        claim.intent.payload >= rows_.size()) {
        return false;
    }
    Row& row = rows_[static_cast<std::size_t>(claim.intent.payload)];
    if (row.state != ActionDeferredRecordState::Held ||
        row.heldCancelOwned || row.retryReservation.Valid() ||
        row.poolToken.slot != claim.token.slot ||
        row.poolToken.generation != claim.token.generation) {
        return false;
    }
    row.state = ActionDeferredRecordState::Claimed;
    *outToken = {static_cast<std::uint32_t>(claim.intent.payload),
                 row.generation};
    *outRecord = row.record;
    return true;
}

bool ActionDeferredRecordTable::InspectPlayPrerequisites(
    DeferredIntentPool& pool, const DeferredIntent& candidate,
    std::uint32_t candidateSlot, bool* outHeld,
    ActionDeferredPairState* outFaultState) const noexcept {
    if (outHeld == nullptr) return false;
    *outHeld = false;
    // Expanded parent/child firings share the accepted ticket, not the firing handle.
    for (std::uint32_t slot = 0; slot < rows_.size(); ++slot) {
        const Row& prerequisite = rows_[slot];
        if (prerequisite.state != ActionDeferredRecordState::Held || prerequisite.heldCancelOwned ||
            (prerequisite.record.step.opcode != ActionOpcodeV1::Reset &&
             prerequisite.record.step.opcode != ActionOpcodeV1::SetVariable) ||
            prerequisite.record.intendedBoundaryFrame > candidate.dueFrame) continue;
        const auto* prior = pool.Validate(prerequisite.poolToken, DeferredIntentEntryState::Held);
        if (prior == nullptr || prior->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
            prior->intent.ownerHandle != prerequisite.record.firing ||
            prior->intent.ownerEpoch != prerequisite.record.ownerCancellationEpoch ||
            prior->intent.payload != slot || prior->intent.dueFrame != prerequisite.record.intendedBoundaryFrame ||
            prior->intent.executionOrdinal != prerequisite.record.executionOrdinal ||
            prerequisite.retryReservation.Valid() || prior->retryReservation.Valid()) {
            if (outFaultState != nullptr)
                (void)InspectPair(pool, prerequisite.poolToken, {slot, prerequisite.generation}, outFaultState);
            return false;
        }
        if (prior->intent.commitSequence == candidate.commitSequence &&
            DeferredPrecedes(prior->intent, slot, candidate, candidateSlot)) {
            *outHeld = true;
            return true;
        }
    }
    return true;
}

bool ActionDeferredRecordTable::InspectClaimedPlayPrerequisites(
    DeferredIntentPool* pool, DeferredIntentToken poolToken,
    ActionDeferredRecordToken recordToken, bool* outHeld,
    ActionDeferredPairState* outFaultState) const noexcept {
    const Row* row = Resolve(recordToken);
    const auto* entry = pool == nullptr ? nullptr : pool->Validate(poolToken, DeferredIntentEntryState::Claimed);
    if (outHeld == nullptr || row == nullptr || row->state != ActionDeferredRecordState::Claimed ||
        row->record.step.opcode != ActionOpcodeV1::Play || row->retryReservation.Valid() ||
        row->poolToken.slot != poolToken.slot || row->poolToken.generation != poolToken.generation ||
        entry == nullptr || entry->retryReservation.Valid() ||
        entry->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
        entry->intent.ownerHandle != row->record.firing ||
        entry->intent.ownerEpoch != row->record.ownerCancellationEpoch ||
        entry->intent.payload != recordToken.slot || entry->intent.dueFrame != row->record.intendedBoundaryFrame ||
        entry->intent.executionOrdinal != row->record.executionOrdinal) {
        if (pool != nullptr && outFaultState != nullptr)
            (void)InspectPair(*pool, poolToken, recordToken, outFaultState);
        return false;
    }
    return InspectPlayPrerequisites(*pool, entry->intent, recordToken.slot, outHeld, outFaultState);
}

FixedPlayClaimStatus ActionDeferredRecordTable::ClaimFixedPlayPair(
    DeferredIntentPool* pool, FixedPlayClaimMode mode,
    std::uint64_t renderedQuantum, std::uint32_t blockFrames,
    std::uint64_t windowEndFrame, std::uint64_t publishCutoffFrame,
    DeferredIntentClaim* outClaim,
    ActionDeferredPairState* outFaultState) noexcept {
    if (pool == nullptr || outClaim == nullptr || blockFrames == 0) {
        return FixedPlayClaimStatus::ProtocolViolation;
    }
    if (liveCount_ == 0) return FixedPlayClaimStatus::None;
    DeferredIntentPool::Entry* selected = nullptr;
    DeferredIntentToken selectedToken{};
    for (std::uint32_t slot = 0; slot < rows_.size(); ++slot) {
        const Row& row = rows_[slot];
        if (row.state != ActionDeferredRecordState::Held ||
            row.heldCancelOwned || row.retryReservation.Valid() ||
            row.record.step.opcode != ActionOpcodeV1::Play ||
            row.record.intendedBoundaryFrame == 0) {
            continue;
        }
        const std::uint64_t target = row.record.intendedBoundaryFrame;
        const bool expired = renderedQuantum > target / blockFrames ||
                             publishCutoffFrame > target;
        if (mode == FixedPlayClaimMode::BeforeDeadline) {
            if (expired || target >= windowEndFrame) continue;
        } else if (!expired) {
            continue;
        }
        DeferredIntentPool::Entry* entry = pool->Validate(
            row.poolToken, DeferredIntentEntryState::Held);
        if (entry == nullptr ||
            entry->retryReservation.Valid() ||
            entry->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
            entry->intent.ownerHandle != row.record.firing ||
            entry->intent.ownerEpoch != row.record.ownerCancellationEpoch ||
            entry->intent.payload != slot ||
            entry->intent.dueFrame != target ||
            entry->intent.executionOrdinal != row.record.executionOrdinal) {
            if (outFaultState != nullptr) {
                (void)InspectPair(*pool, row.poolToken,
                                  {slot, row.generation}, outFaultState);
            }
            return FixedPlayClaimStatus::ProtocolViolation;
        }
        if (mode == FixedPlayClaimMode::BeforeDeadline &&
            entry->intent.notBeforeQuantum > renderedQuantum) {
            continue;
        }
        if (mode == FixedPlayClaimMode::BeforeDeadline) {
            bool prerequisiteHeld = false;
            if (!InspectPlayPrerequisites(*pool, entry->intent, slot, &prerequisiteHeld, outFaultState))
                return FixedPlayClaimStatus::ProtocolViolation;
            if (prerequisiteHeld) continue;
        }
        if (selected == nullptr ||
            DeferredPrecedes(entry->intent, slot, selected->intent,
                     static_cast<std::uint32_t>(selected->intent.payload))) {
            selected = entry;
            selectedToken = row.poolToken;
        }
    }
    if (selected == nullptr) return FixedPlayClaimStatus::None;
    {
        for (std::uint32_t slot = 0; slot < rows_.size(); ++slot) {
            const Row& row = rows_[slot];
            const bool reservedCancellation =
                row.state == ActionDeferredRecordState::CancelRequested &&
                row.retryReservation.Valid();
            if ((row.state != ActionDeferredRecordState::Held &&
                 !reservedCancellation) ||
                row.heldCancelOwned || row.record.step.opcode != ActionOpcodeV1::Play ||
                row.record.intendedBoundaryFrame != selected->intent.dueFrame) {
                continue;
            }
            const DeferredIntentPool::Entry* blocker = pool->Validate(
                row.poolToken, DeferredIntentEntryState::Held);
            if (blocker == nullptr) {
                if (outFaultState != nullptr) {
                    (void)InspectPair(*pool, row.poolToken,
                                      {slot, row.generation}, outFaultState);
                }
                return FixedPlayClaimStatus::ProtocolViolation;
            }
            if (reservedCancellation &&
                (!blocker->retryReservation.Valid() ||
                 row.retryReservation.recordSlot != slot ||
                 row.retryReservation.recordGeneration != row.generation ||
                 row.retryReservation.poolToken.slot != row.poolToken.slot ||
                 row.retryReservation.poolToken.generation != row.poolToken.generation ||
                 blocker->retryReservation.recordSlot != slot ||
                 blocker->retryReservation.recordGeneration != row.generation ||
                 blocker->retryReservation.poolToken.slot != row.poolToken.slot ||
                 blocker->retryReservation.poolToken.generation != row.poolToken.generation)) {
                if (outFaultState != nullptr) {
                    (void)InspectPair(*pool, row.poolToken,
                                      {slot, row.generation}, outFaultState);
                }
                return FixedPlayClaimStatus::ProtocolViolation;
            }
            if ((row.retryReservation.Valid() ||
                 blocker->retryReservation.Valid() ||
                 (mode == FixedPlayClaimMode::BeforeDeadline &&
                  blocker->intent.notBeforeQuantum > renderedQuantum)) &&
                DeferredPrecedes(blocker->intent, slot, selected->intent,
                         static_cast<std::uint32_t>(selected->intent.payload))) {
                return FixedPlayClaimStatus::None;
            }
        }
    }
    *outClaim = {selectedToken, selected->intent};
    selected->state = DeferredIntentEntryState::Claimed;
    return FixedPlayClaimStatus::Claimed;
}

bool ActionDeferredRecordTable::RetryFixedPlayPair(
    DeferredIntentPool* pool, DeferredIntentToken poolToken,
    ActionDeferredRecordToken recordToken,
    std::uint64_t notBeforeQuantum,
    bool reserveForRetryOwner) noexcept {
    Row* row = Resolve(recordToken);
    if (pool == nullptr || row == nullptr ||
        row->state != ActionDeferredRecordState::Claimed ||
        row->retryReservation.Valid() ||
        row->poolToken.slot != poolToken.slot ||
        row->poolToken.generation != poolToken.generation ||
        row->record.step.opcode != ActionOpcodeV1::Play ||
        row->record.intendedBoundaryFrame == 0) {
        return false;
    }
    DeferredIntentPool::Entry* entry = pool->Validate(
        poolToken, DeferredIntentEntryState::Claimed);
    if (entry == nullptr ||
        entry->retryReservation.Valid() ||
        entry->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
        entry->intent.ownerHandle != row->record.firing ||
        entry->intent.ownerEpoch != row->record.ownerCancellationEpoch ||
        entry->intent.payload != recordToken.slot ||
        entry->intent.dueFrame != row->record.intendedBoundaryFrame ||
        entry->intent.executionOrdinal != row->record.executionOrdinal) {
        return false;
    }
    entry->intent.notBeforeQuantum = notBeforeQuantum;
    if (reserveForRetryOwner) {
        const DeferredIntentRetryReservation reservation{
            poolToken, recordToken.slot, recordToken.generation};
        entry->retryReservation = reservation;
        row->retryReservation = reservation;
    }
    entry->state = DeferredIntentEntryState::Held;
    row->state = ActionDeferredRecordState::Held;
    return true;
}

bool ActionDeferredRecordTable::ClearFixedPlayPairRetryReservation(
    DeferredIntentPool* pool, DeferredIntentToken poolToken,
    ActionDeferredRecordToken recordToken) noexcept {
    Row* row = Resolve(recordToken);
    DeferredIntentPool::Entry* entry = pool == nullptr ? nullptr :
        pool->Validate(poolToken, DeferredIntentEntryState::Held);
    if (row == nullptr || entry == nullptr ||
        row->state != ActionDeferredRecordState::Held ||
        row->poolToken.slot != poolToken.slot ||
        row->poolToken.generation != poolToken.generation ||
        entry->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
        entry->intent.ownerHandle != row->record.firing ||
        entry->intent.ownerEpoch != row->record.ownerCancellationEpoch ||
        entry->intent.payload != recordToken.slot ||
        entry->intent.dueFrame != row->record.intendedBoundaryFrame ||
        entry->intent.executionOrdinal != row->record.executionOrdinal) {
        return false;
    }
    const auto matches = [&](const DeferredIntentRetryReservation& reservation) {
        return reservation.poolToken.slot == poolToken.slot &&
               reservation.poolToken.generation == poolToken.generation &&
               reservation.recordSlot == recordToken.slot &&
               reservation.recordGeneration == recordToken.generation;
    };
    if (!matches(row->retryReservation) ||
        !matches(entry->retryReservation)) return false;
    row->retryReservation = {};
    entry->retryReservation = {};
    return true;
}

bool ActionDeferredRecordTable::InspectPair(
    const DeferredIntentPool& pool, DeferredIntentToken poolToken,
    ActionDeferredRecordToken recordToken,
    ActionDeferredPairState* outState) const noexcept {
    if (outState == nullptr || recordToken.slot >= rows_.size()) return false;
    const Row& row = rows_[recordToken.slot];
    *outState = {};
    outState->recordToken = {recordToken.slot, row.generation};
    outState->recordState = row.state;
    outState->targetPackLease = row.record.targetPackLease;
    outState->recordReservation = row.retryReservation;
    outState->poolToken = poolToken;
    if (poolToken.slot < pool.m_entries.size()) {
        const DeferredIntentPool::Entry& entry =
            pool.m_entries[poolToken.slot];
        outState->poolToken.generation = entry.generation;
        outState->poolState = entry.state;
        outState->poolReservation = entry.retryReservation;
    }
    return true;
}

bool ActionDeferredRecordTable::Capture(
    ActionDeferredRecordToken token,
    ActionDeferredRecord* outRecord) const noexcept {
    const Row* row = Resolve(token);
    if (outRecord == nullptr || row == nullptr ||
        (row->state != ActionDeferredRecordState::Held &&
         row->state != ActionDeferredRecordState::Claimed &&
         row->state != ActionDeferredRecordState::CancelRequested)) {
        return false;
    }
    *outRecord = row->record;
    return true;
}

bool ActionDeferredRecordTable::CaptureAt(
    std::uint32_t slot, ActionDeferredRecordToken* outToken,
    ActionDeferredRecord* outRecord,
    ActionDeferredRecordState* outState) const noexcept {
    if (slot >= rows_.size() || outToken == nullptr ||
        outRecord == nullptr || outState == nullptr) {
        return false;
    }
    const Row& row = rows_[slot];
    if (row.state != ActionDeferredRecordState::Held &&
        row.state != ActionDeferredRecordState::Claimed &&
        row.state != ActionDeferredRecordState::CancelRequested) {
        return false;
    }
    *outToken = {slot, row.generation};
    *outRecord = row.record;
    *outState = row.state;
    return true;
}

bool ActionDeferredRecordTable::CapturePublicationAt(
    std::uint32_t slot, ActionDeferredRecordToken* outToken,
    ActionDeferredRecord* outRecord,
    ActionDeferredRecordState* outState) const noexcept {
    if (slot >= rows_.size() || outToken == nullptr ||
        outRecord == nullptr || outState == nullptr) {
        return false;
    }
    const Row& row = rows_[slot];
    if (row.state != ActionDeferredRecordState::Prepared &&
        row.state != ActionDeferredRecordState::Held &&
        row.state != ActionDeferredRecordState::Claimed &&
        row.state != ActionDeferredRecordState::CancelRequested) {
        return false;
    }
    *outToken = {slot, row.generation};
    *outRecord = row.record;
    *outState = row.state;
    return true;
}

bool ActionDeferredRecordTable::RetryClaim(
    ActionDeferredRecordToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->state != ActionDeferredRecordState::Claimed) {
        return false;
    }
    row->state = ActionDeferredRecordState::Held;
    return true;
}

bool ActionDeferredRecordTable::RebaseHeldPendingStop(
    ActionDeferredRecordToken token, DeferredIntentPool* pool,
    std::uint64_t firstRowFrame, std::uint64_t lastRowFrame) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || pool == nullptr ||
        row->state != ActionDeferredRecordState::Held ||
        !row->record.waitingForImmediateEntry ||
        firstRowFrame > lastRowFrame || lastRowFrame == UINT64_MAX) {
        return false;
    }
    auto* entry = pool->Validate(row->poolToken,
                                 DeferredIntentEntryState::Held);
    if (entry == nullptr ||
        entry->intent.ownerDomain != DeferredIntentOwnerDomain::Action ||
        entry->intent.ownerHandle != row->record.firing ||
        entry->intent.ownerEpoch != row->record.ownerCancellationEpoch ||
        entry->intent.payload != token.slot ||
        entry->intent.dueFrame != UINT64_MAX) {
        return false;
    }
    row->record.firstRowFrame = firstRowFrame;
    row->record.lastRowFrame = lastRowFrame;
    row->record.intendedBoundaryFrame = lastRowFrame;
    row->record.deadlineFrame = lastRowFrame;
    row->record.waitingForImmediateEntry = false;
    entry->intent.dueFrame = lastRowFrame;
    return true;
}

bool ActionDeferredRecordTable::PoolToken(
    ActionDeferredRecordToken token,
    DeferredIntentToken* outToken) const noexcept {
    const Row* row = Resolve(token);
    if (outToken == nullptr || row == nullptr || !row->poolToken.Valid()) {
        return false;
    }
    *outToken = row->poolToken;
    return true;
}

ActionDeferredCancelOwnership ActionDeferredRecordTable::RequestCancel(
    ActionDeferredRecordToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr) {
        return ActionDeferredCancelOwnership::None;
    }
    if (row->state == ActionDeferredRecordState::Held) {
        if (row->retryReservation.Valid()) {
            row->state = ActionDeferredRecordState::CancelRequested;
            return ActionDeferredCancelOwnership::Claimant;
        }
        if (row->heldCancelOwned) {
            return ActionDeferredCancelOwnership::None;
        }
        row->heldCancelOwned = true;
        return ActionDeferredCancelOwnership::Canceller;
    }
    if (row->state == ActionDeferredRecordState::Claimed) {
        row->state = ActionDeferredRecordState::CancelRequested;
        return ActionDeferredCancelOwnership::Claimant;
    }
    return ActionDeferredCancelOwnership::None;
}

bool ActionDeferredRecordTable::Finalize(Row& row) noexcept {
    const bool heldOwned =
        row.state == ActionDeferredRecordState::Held &&
        row.heldCancelOwned;
    if (!heldOwned && row.state != ActionDeferredRecordState::Claimed &&
        row.state != ActionDeferredRecordState::CancelRequested) {
        return false;
    }
    --liveCount_;
    row.record = {};
    row.poolToken = {};
    row.heldCancelOwned = false;
    row.retryReservation = {};
    if (row.generation == UINT64_MAX) {
        row.state = ActionDeferredRecordState::Quarantined;
        ++quarantinedCount_;
    } else {
        ++row.generation;
        row.state = ActionDeferredRecordState::Free;
    }
    return true;
}

bool ActionDeferredRecordTable::FinalizeHeld(
    ActionDeferredRecordToken token) noexcept {
    Row* row = Resolve(token);
    return row != nullptr && row->state == ActionDeferredRecordState::Held &&
           row->heldCancelOwned && Finalize(*row);
}

bool ActionDeferredRecordTable::FinalizeClaim(
    ActionDeferredRecordToken token) noexcept {
    Row* row = Resolve(token);
    return row != nullptr && Finalize(*row);
}

ActionDeferredRecordState ActionDeferredRecordTable::State(
    ActionDeferredRecordToken token) const noexcept {
    const Row* row = Resolve(token);
    return row == nullptr ? ActionDeferredRecordState::Free : row->state;
}

bool ActionDeferredRecordTable::SetFreeGenerationForTests(
    std::uint32_t slot, std::uint64_t generation) noexcept {
    if (slot >= rows_.Capacity() || generation == 0 ||
        !rows_.ConstructThrough(slot + 1u) ||
        rows_[slot].state != ActionDeferredRecordState::Free) {
        return false;
    }
    rows_[slot].generation = generation;
    return true;
}

bool ActionDeferredRecordTable::SetTargetFingerprintForTests(
    ActionDeferredRecordToken token, std::uint64_t fingerprint) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->state != ActionDeferredRecordState::Held ||
        fingerprint == 0) {
        return false;
    }
    row->record.step.target.expectedProgramSemanticFingerprint = fingerprint;
    return true;
}

} // namespace harken
