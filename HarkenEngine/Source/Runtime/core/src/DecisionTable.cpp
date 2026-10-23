#include "harken/core/DecisionTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/CheckedIssuer.h"
#include "harken/core/QueueRuntime.h"

#include <limits>
#include <optional>
#include <cassert>
#include <utility>

namespace harken {
namespace {

std::uint64_t PackIdentity(std::uint32_t slot,
                           std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

bool SameOriginalDecisionRequest(const DecisionReserveRequest& a,
    const DecisionReserveRequest& b) noexcept {
    return a.ticket.sequence == b.ticket.sequence &&
        a.world == b.world && a.transport == b.transport &&
        a.expectedContinuationGeneration == b.expectedContinuationGeneration &&
        a.sliceCount == b.sliceCount &&
        a.transitionGate.slot == b.transitionGate.slot &&
        a.transitionGate.generation == b.transitionGate.generation;
}

bool ValidRequest(const DecisionReserveRequest& request) noexcept {
    return request.ticket.Valid() && request.world != kNoWorldGeneration &&
           request.transport != kInvalidTransportEpoch &&
           request.expectedContinuationGeneration != 0 &&
           request.sliceCount != 0;
}

bool ValidPreparedRequest(
    const DecisionReserveRequest& request) noexcept {
    return !request.ticket.Valid() &&
           request.world != kNoWorldGeneration &&
           request.transport != kInvalidTransportEpoch &&
           request.expectedContinuationGeneration != 0 &&
           request.sliceCount != 0;
}

bool ValidConfig(std::uint32_t capacity,
                 std::uint32_t maxParticipants) noexcept {
    return capacity != 0 && capacity <= kMaxDecisionRowsHard &&
           maxParticipants != 0 &&
           maxParticipants <= kMaxParticipantsPerDecisionHard;
}

} // namespace

struct DecisionTable::FrontierSidecar {
    TimelineEventPayload controlPayload{};
    TimelineProgramView controlProgram{};
    FrontierActivation activation;
    std::optional<Plan03GroupReaderPin> pagePin;
    DecisionFuturePin observerPin;
    const ScheduledTransactionPool* operationPool = nullptr;
    FrontierOwnerIdentity operationOwner{};
    FrontierPublicationProof* publicationProof = nullptr;
};

DecisionTable::FrontierPublicationProof::FrontierPublicationProof(
    FrontierPublicationProof&& other) noexcept { *this = std::move(other); }

DecisionTable::FrontierPublicationProof& DecisionTable::FrontierPublicationProof::operator=(
    FrontierPublicationProof&& other) noexcept {
    if (this != &other) {
        Release();
        table_ = std::exchange(other.table_, nullptr);
        row_ = std::exchange(other.row_, nullptr);
        activation_ = std::exchange(other.activation_, nullptr);
        token_ = std::exchange(other.token_, DecisionToken{});
        if (table_ != nullptr) {
            auto& sidecar = table_->frontiers_[token_.slot];
            assert(sidecar.publicationProof == &other);
            sidecar.publicationProof = this;
        }
    }
    return *this;
}

void DecisionTable::FrontierPublicationProof::Release() noexcept {
    if (table_ != nullptr) {
        assert(activation_->State() == FrontierReceiptState::PublicationHeld);
        activation_->state_.store(FrontierReceiptState::Prepared, std::memory_order_release);
    }
    Detach();
}

void DecisionTable::FrontierPublicationProof::Detach() noexcept {
    if (table_ != nullptr) {
        auto& sidecar = table_->frontiers_[token_.slot];
        assert(sidecar.publicationProof == this && activation_ == &sidecar.activation);
        sidecar.publicationProof = nullptr;
        row_->reservation.clear(std::memory_order_release);
    }
    table_ = nullptr;
    row_ = nullptr;
    activation_ = nullptr;
    token_ = {};
}

bool DecisionTable::FrontierPublicationHeld(DecisionToken token) const noexcept {
    const auto* activation = ResolveFrontierActivation(token);
    return activation != nullptr && activation->State() == FrontierReceiptState::PublicationHeld;
}

bool DecisionTable::ProveFrontierActivationPublication(DecisionToken token,
    const FrontierEntryReceiptRef& entry, const ScheduledTransactionPool& pool,
    FrontierPublicationProof* out) noexcept {
    auto* row = Resolve(token);
    if (row == nullptr || out == nullptr || out->Valid() || PreparedTicketHeld(token) ||
        frontiers_[token.slot].publicationProof != nullptr ||
        row->reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto refuse = [&]() noexcept { row->reservation.clear(std::memory_order_release); return false; };
    if (Resolve(token) != row || !CanPublishFrontierActivation(token) ||
        !MatchesPreparedFrontierActivationPool(entry, pool) ||
        entry.coordinator.slot != token.slot || entry.coordinator.generation != token.generation ||
        row->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
        row->consumedSlices.load(std::memory_order_acquire) != 0 ||
        row->lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        row->authoredStopInvalidated.load(std::memory_order_acquire)) return refuse();
    auto& sidecar = frontiers_[token.slot];
    auto expected = FrontierReceiptState::Prepared;
    if (!sidecar.activation.state_.compare_exchange_strong(expected, FrontierReceiptState::PublicationHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return refuse();
    out->table_ = this;
    out->row_ = row;
    out->activation_ = &sidecar.activation;
    out->token_ = token;
    sidecar.publicationProof = out;
    return true;
}

bool DecisionTable::IsFrontierPublicationProofFor(const FrontierPublicationProof& proof,
    const FrontierActivationDescriptor& descriptor) const noexcept {
    if (proof.table_ != this || proof.row_ == nullptr || !descriptor.Valid() ||
        proof.token_.slot != descriptor.entry.coordinator.slot ||
        proof.token_.generation != descriptor.entry.coordinator.generation ||
        proof.row_ != Resolve(proof.token_)) return false;
    const auto& sidecar = frontiers_[proof.token_.slot];
    return sidecar.publicationProof == &proof && proof.activation_ == &sidecar.activation &&
        proof.activation_->State() == FrontierReceiptState::PublicationHeld &&
        proof.activation_->Descriptor() == descriptor;
}

void DecisionTable::ApplyFrontierActivationPublication(FrontierPublicationProof&& proof) noexcept {
    assert(proof.Valid() && proof.activation_->State() == FrontierReceiptState::PublicationHeld);
    if (!proof.Valid()) return;
    auto* activation = proof.activation_;
    // Dispose logic ownership before the audio owner can advance Pending.
    proof.Detach();
    activation->state_.store(FrontierReceiptState::Pending, std::memory_order_release);
}

PreparedDecisionReservation::PreparedDecisionReservation(
    PreparedDecisionReservation&& other) noexcept {
    *this = std::move(other);
}

PreparedDecisionReservation& PreparedDecisionReservation::operator=(
    PreparedDecisionReservation&& other) noexcept {
    if (this == &other ||
        (owner_ != nullptr && owner_->PreparedTicketHeld(token_)) ||
        (other.owner_ != nullptr && other.owner_->PreparedTicketHeld(other.token_))) return *this;
    owner_ = std::exchange(other.owner_, nullptr);
    token_ = std::exchange(other.token_, {});
    prepared_ = std::exchange(other.prepared_, false);
    originalRequest_ = std::exchange(other.originalRequest_, {});
    originalLifetimeMarker_ = std::exchange(other.originalLifetimeMarker_, 0);
    attachedTransitionGate_ = std::exchange(other.attachedTransitionGate_, {});
    attachedSliceCount_ = std::exchange(other.attachedSliceCount_, 0);
    return *this;
}

DecisionTable::PreparedTicketCommitProof::PreparedTicketCommitProof(
    PreparedTicketCommitProof&& other) noexcept {
    *this = std::move(other);
}

DecisionTable::PreparedTicketCommitProof&
DecisionTable::PreparedTicketCommitProof::operator=(PreparedTicketCommitProof&& other) noexcept {
    if (this != &other) {
        Release();
        table_ = std::exchange(other.table_, nullptr);
        row_ = std::exchange(other.row_, nullptr);
        original_ = std::exchange(other.original_, nullptr);
        token_ = std::exchange(other.token_, {});
        sequence_ = std::exchange(other.sequence_, 0);
    }
    return *this;
}

void DecisionTable::PreparedTicketCommitProof::Release() noexcept {
    if (Valid()) {
        row_->commit.store(DecisionCommitState::Open, std::memory_order_release);
        row_->reservation.clear(std::memory_order_release);
    }
    table_ = nullptr;
    row_ = nullptr;
    original_ = nullptr;
    token_ = {};
    sequence_ = 0;
}

bool DecisionTable::PreparedTicketHeld(DecisionToken token) const noexcept {
    const auto* row = Resolve(token);
    return row != nullptr && row->commit.load(std::memory_order_acquire) ==
        DecisionCommitState::PreparedTicketHeld;
}

bool DecisionTable::ProvePreparedReservationCommit(PreparedDecisionReservation& original,
    const DecisionReserveRequest& originalInput, LogicCommitSequence expectedSequence,
    PreparedTicketCommitProof* out) noexcept {
    if (out == nullptr || out->Valid() || original.owner_ != this || !original.prepared_ ||
        expectedSequence == 0 || !ValidPreparedRequest(originalInput)) return false;
    const auto token = original.token_;
    Row* row = Resolve(token);
    if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto fail = [&]() noexcept { row->reservation.clear(std::memory_order_release); return false; };
    const auto& issued = original.originalRequest_;
    const auto expectedGate = original.attachedTransitionGate_.Valid()
        ? original.attachedTransitionGate_ : issued.transitionGate;
    const auto expectedSlices = original.attachedTransitionGate_.Valid()
        ? original.attachedSliceCount_ : issued.sliceCount;
    const auto& sidecar = frontiers_[token.slot];
    if (Resolve(token) != row || row->ticket.Valid() ||
        (original.originalLifetimeMarker_ != 0 &&
         (row->originalLifetimeMarker != original.originalLifetimeMarker_ ||
          !SameOriginalDecisionRequest(issued, row->originalLifetimeRequest))) ||
        originalInput.ticket.sequence != issued.ticket.sequence ||
        originalInput.world != issued.world || originalInput.transport != issued.transport ||
        originalInput.expectedContinuationGeneration != issued.expectedContinuationGeneration ||
        originalInput.sliceCount != issued.sliceCount ||
        originalInput.transitionGate.slot != issued.transitionGate.slot ||
        originalInput.transitionGate.generation != issued.transitionGate.generation ||
        row->world != issued.world || row->transport != issued.transport ||
        row->continuationGeneration.load(std::memory_order_acquire) != issued.expectedContinuationGeneration ||
        row->sliceCount != expectedSlices || row->transitionGate.slot != expectedGate.slot ||
        row->transitionGate.generation != expectedGate.generation ||
        row->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
        row->consumedSlices.load(std::memory_order_acquire) != 0 ||
        row->lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        row->authoredStopInvalidated.load(std::memory_order_acquire) ||
        row->participantCount != 0 || row->pages.Valid() || row->armGroup.Valid() ||
        row->futurePinCount.load(std::memory_order_acquire) != 0 ||
        sidecar.activation.State() != FrontierReceiptState::Empty || sidecar.pagePin.has_value() ||
        sidecar.observerPin.Valid() || sidecar.operationPool != nullptr) return fail();
    DecisionCommitState expected = DecisionCommitState::Open;
    if (!row->commit.compare_exchange_strong(expected, DecisionCommitState::PreparedTicketHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return fail();
    out->table_ = this;
    out->row_ = row;
    out->original_ = &original;
    out->token_ = token;
    out->sequence_ = expectedSequence;
    return true;
}

void DecisionTable::ConsumePreparedReservationCommit(PreparedTicketCommitProof&& proof) noexcept {
    assert(proof.Valid() && proof.sequence_ != 0);
    if (!proof.Valid()) return;
    proof.row_->ticket.sequence = proof.sequence_;
    proof.original_->owner_ = nullptr;
    proof.original_->token_ = {};
    proof.original_->prepared_ = false;
    proof.original_->originalRequest_ = {};
    proof.original_->originalLifetimeMarker_ = 0;
    proof.original_->attachedTransitionGate_ = {};
    proof.original_->attachedSliceCount_ = 0;
    proof.Release();
}

DecisionTable::DecisionTable(std::uint32_t capacity,
                             std::uint32_t maxParticipantsPerDecision)
    : capacity_(ValidConfig(capacity, maxParticipantsPerDecision)
                    ? capacity
                    : 0),
      maxParticipantsPerDecision_(
          ValidConfig(capacity, maxParticipantsPerDecision)
              ? maxParticipantsPerDecision
              : 0) {
    if (!participants_.Reserve(capacity_ * maxParticipantsPerDecision_) ||
        !frontiers_.Reserve(capacity_) || !rows_.Reserve(capacity_))
        throw std::bad_alloc{};
}

bool DecisionTable::PrepareSlotStorage(std::uint32_t slot) noexcept {
    return slot < capacity_ &&
        participants_.ConstructThrough((slot + 1u) * maxParticipantsPerDecision_) &&
        frontiers_.ConstructThrough(slot + 1u) && rows_.ConstructThrough(slot + 1u);
}

bool DecisionTable::PrepareBacking(std::uint32_t count, BackingPreparation* out) noexcept {
    if (out == nullptr) return false;
    for (const auto& held : *out) if (held.Valid()) return false;
    count = std::min(count, capacity_);
    auto occupied = rows_.ConstructedCount();
    while (occupied != 0 && rows_[occupied - 1u].slotState.load(std::memory_order_acquire) == SlotState::Free)
        --occupied;
    BackingPreparation candidate;
    if (!participants_.PrepareBacking(count * maxParticipantsPerDecision_,
            occupied * maxParticipantsPerDecision_, &candidate[0]) ||
        !frontiers_.PrepareBacking(count, occupied, &candidate[1]) ||
        !rows_.PrepareBacking(count, occupied, &candidate[2])) return false;
    *out = std::move(candidate);
    return true;
}

std::uint64_t DecisionTable::RowStorageBytes() const noexcept {
    return rows_.RequestedBytes() + participants_.RequestedBytes() + frontiers_.RequestedBytes();
}

std::uint64_t DecisionTable::RowCommittedBytes() const noexcept {
    return rows_.CommittedBytes() + participants_.CommittedBytes() + frontiers_.CommittedBytes();
}

std::uint64_t DecisionTable::RowReservedBytes() const noexcept {
    return rows_.ReservedBytes() + participants_.ReservedBytes() + frontiers_.ReservedBytes();
}

DecisionTable::~DecisionTable() {
    for (std::uint32_t index = 0; index < frontiers_.ConstructedCount(); ++index)
        if (frontiers_[index].publicationProof != nullptr) frontiers_[index].publicationProof->Release();
    for (std::uint32_t index = 0; index < rows_.ConstructedCount(); ++index)
        assert(rows_[index].originalLifetimePinCount == 0);
}

OriginalDecisionLifetimePin::~OriginalDecisionLifetimePin() {
    const bool released = Release();
    assert(released);
    (void)released;
}

OriginalDecisionLifetimePin::OriginalDecisionLifetimePin(
    OriginalDecisionLifetimePin&& other) noexcept { *this = std::move(other); }

OriginalDecisionLifetimePin& OriginalDecisionLifetimePin::operator=(
    OriginalDecisionLifetimePin&& other) noexcept {
    if (this != &other) {
        const bool released = Release();
        assert(released);
        if (!released) return *this;
        owner_ = std::exchange(other.owner_, nullptr);
        token_ = std::exchange(other.token_, {});
        marker_ = std::exchange(other.marker_, 0);
    }
    return *this;
}

bool OriginalDecisionLifetimePin::Borrow(OriginalDecisionLifetimePin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginalDecisionLifetime(*this, out);
}

bool OriginalDecisionLifetimePin::Release() noexcept {
    return owner_ == nullptr || owner_->ReleaseOriginalDecisionLifetime(*this);
}

bool DecisionTable::CanIssueOriginalDecisionLifetime(
    const OriginalDecisionLifetimePin* out) const noexcept {
    return out == nullptr || (!out->Valid() && !originalLifetimeMarkerExhausted_ &&
        nextOriginalLifetimeMarker_ != 0);
}

void DecisionTable::IssueOriginalDecisionLifetime(Row& row, DecisionToken token,
    const DecisionReserveRequest& request, OriginalDecisionLifetimePin& out) noexcept {
    assert(!out.Valid() && row.originalLifetimePinCount == 0 &&
        row.originalLifetimeMarker == 0);
    const auto issued = harken::TryIssue(&nextOriginalLifetimeMarker_,
        &originalLifetimeMarkerExhausted_);
    assert(issued.status == harken::IssueStatus::Ok);
    row.originalLifetimeMarker = issued.value;
    row.originalLifetimePinCount = 1;
    row.originalLifetimeRequest = request;
    out.owner_ = this;
    out.token_ = token;
    out.marker_ = issued.value;
}

bool DecisionTable::MatchesOriginalDecisionLifetime(
    const OriginalDecisionLifetimePin& original) const noexcept {
    if (original.owner_ != this || original.marker_ == 0) return false;
    const auto* row = Resolve(original.token_);
    return row != nullptr && row->originalLifetimeMarker == original.marker_ &&
        row->originalLifetimePinCount != 0;
}

bool DecisionTable::IsOriginalDecisionLifetimePinFor(
    const OriginalDecisionLifetimePin& original, DecisionToken token) const noexcept {
    return MatchesOriginalDecisionLifetime(original) &&
        original.token_.slot == token.slot && original.token_.generation == token.generation;
}

bool DecisionTable::MatchesOriginalPreparedDecision(const OriginalDecisionLifetimePin& original,
    const PreparedDecisionReservation& preparation) const noexcept {
    if (!MatchesOriginalDecisionLifetime(original) || preparation.owner_ != this || !preparation.Valid() ||
        preparation.token_.slot != original.token_.slot || preparation.token_.generation != original.token_.generation ||
        preparation.originalLifetimeMarker_ != original.marker_) return false;
    const auto& row = rows_[original.token_.slot];
    const auto commit = row.commit.load(std::memory_order_acquire);
    return !row.ticket.Valid() && (commit == DecisionCommitState::Open || commit == DecisionCommitState::PreparedTicketHeld) &&
        row.audioState.load(std::memory_order_acquire) == AudioGroupState::Pending &&
        row.consumedSlices.load(std::memory_order_acquire) == 0 &&
        row.lastAudioSequence.load(std::memory_order_acquire) == 0 &&
        !row.authoredStopInvalidated.load(std::memory_order_acquire) &&
        SameOriginalDecisionRequest(preparation.originalRequest_, row.originalLifetimeRequest) &&
        row.world == preparation.originalRequest_.world && row.transport == preparation.originalRequest_.transport &&
        row.continuationGeneration.load(std::memory_order_acquire) == preparation.originalRequest_.expectedContinuationGeneration;
}

bool DecisionTable::MatchesOriginalDecisionFactory(
    const OriginalDecisionLifetimePin& original,
    const DecisionReserveRequest& request) const noexcept {
    if (!MatchesOriginalDecisionLifetime(original)) return false;
    return SameOriginalDecisionRequest(rows_[original.token_.slot].originalLifetimeRequest, request);
}

bool DecisionTable::BorrowOriginalDecisionLifetime(
    const OriginalDecisionLifetimePin& original, OriginalDecisionLifetimePin* out) noexcept {
    if (out == nullptr || out->Valid() || !MatchesOriginalDecisionLifetime(original)) return false;
    auto& row = rows_[original.token_.slot];
    if (row.originalLifetimePinCount == UINT32_MAX) return false;
    ++row.originalLifetimePinCount;
    out->owner_ = this;
    out->token_ = original.token_;
    out->marker_ = original.marker_;
    return true;
}

bool DecisionTable::ReleaseOriginalDecisionLifetime(
    OriginalDecisionLifetimePin& original) noexcept {
    if (!MatchesOriginalDecisionLifetime(original)) return false;
    auto& row = rows_[original.token_.slot];
    --row.originalLifetimePinCount;
    // Immutable factory metadata remains until actual row disposition.
    original.owner_ = nullptr;
    original.token_ = {};
    original.marker_ = 0;
    return true;
}

bool DecisionTable::RegisterOriginalLifetimeBaselineResolver(void* context,
    OriginalLifetimeBaselineResolver resolver) noexcept {
    if (context == nullptr || resolver == nullptr || originalLifetimeBaselineResolver_ != nullptr) return false;
    for (std::uint32_t i = 0; i < rows_.ConstructedCount(); ++i)
        if (rows_[i].originalLifetimePinCount != 0) return false;
    originalLifetimeBaselineContext_ = context;
    originalLifetimeBaselineResolver_ = resolver;
    return true;
}
OriginalDecisionLifetimePin* DecisionTable::ResolveOriginalLifetimeBaseline(
    DecisionToken token, OriginalDecisionLifetimePin* explicitBaseline) noexcept {
    if (explicitBaseline != nullptr || originalLifetimeBaselineResolver_ == nullptr) return explicitBaseline;
    auto* original = originalLifetimeBaselineResolver_(originalLifetimeBaselineContext_, *this, token);
    // A numerical token cannot create a native original. The returned object
    // must still match this receiver's issued full token and private marker.
    return original != nullptr && IsOriginalDecisionLifetimePinFor(*original, token) ? original : nullptr;
}

bool DecisionTable::OriginalDecisionRetirementReady(const Row& row, DecisionToken token,
    const OriginalDecisionLifetimePin* baseline) const noexcept {
    return baseline != nullptr
        ? IsOriginalDecisionLifetimePinFor(*baseline, token) && row.originalLifetimePinCount == 1
        : row.originalLifetimePinCount == 0;
}

void DecisionTable::ConsumeOriginalDecisionBaseline(Row& row, DecisionToken token,
    OriginalDecisionLifetimePin* baseline) noexcept {
    if (baseline != nullptr) {
        // Live -> Reclaiming already owns this exact native row exclusively.
        assert(baseline->owner_ == this && baseline->token_.slot == token.slot &&
            baseline->token_.generation == token.generation &&
            baseline->marker_ == row.originalLifetimeMarker &&
            row.originalLifetimePinCount == 1);
        --row.originalLifetimePinCount;
        baseline->owner_ = nullptr;
        baseline->token_ = {};
        baseline->marker_ = 0;
    }
    assert(row.originalLifetimePinCount == 0);
    row.originalLifetimeMarker = 0;
    row.originalLifetimeRequest = {};
}

const TimelineProgramView* DecisionTable::PrepareFrontierControlProgram(DecisionToken token,
    std::uint64_t programGeneration) noexcept {
    if (PreparedTicketHeld(token)) { return nullptr; }
    auto* row = Resolve(token);
    if (row == nullptr || programGeneration == 0) return nullptr;
    auto& sidecar = frontiers_[token.slot];
    if (sidecar.controlProgram.generation != 0)
        return sidecar.controlProgram.generation == programGeneration ? &sidecar.controlProgram : nullptr;
    if (row->commit.load(std::memory_order_acquire) != DecisionCommitState::Open)
        return nullptr;
    sidecar.controlPayload = {};
    sidecar.controlPayload.kind = TimelineEventKind::ControllerActivation;
    sidecar.controlProgram = {programGeneration, &sidecar.controlPayload, 1,
        TimelineProgramRole::ControllerActivation};
    return &sidecar.controlProgram;
}

bool DecisionTable::CaptureFrontierControlProgram(DecisionToken token,
    TimelineTransactionProgramRef* out) const noexcept {
    if (out == nullptr || Resolve(token) == nullptr) return false;
    *out = {};
    const auto& sidecar = frontiers_[token.slot];
    if (sidecar.controlProgram.generation == 0) return true;
    if (sidecar.controlProgram.role != TimelineProgramRole::ControllerActivation ||
        sidecar.controlProgram.payloads != &sidecar.controlPayload ||
        sidecar.controlProgram.payloadCount != 1 ||
        sidecar.controlPayload.kind != TimelineEventKind::ControllerActivation)
        return false;
    *out = {sidecar.controlProgram.generation, &sidecar.controlProgram};
    return true;
}

void DecisionTable::ResetFrontierStorage(std::uint32_t slot) noexcept {
    if (rows_[slot].commit.load(std::memory_order_acquire) == DecisionCommitState::PreparedTicketHeld) return;
    auto& sidecar = frontiers_[slot];
    if (sidecar.activation.State() == FrontierReceiptState::PublicationHeld) return;
    assert(!sidecar.pagePin.has_value() && !sidecar.observerPin.Valid());
    assert(sidecar.operationPool == nullptr);
    sidecar.controlProgram = {};
    sidecar.controlPayload = {};
    auto& activation = sidecar.activation;
    activation.lease_ = nullptr;
    activation.descriptor_ = {};
    activation.receipt_ = {};
    activation.logicPayloadReady_.store(false, std::memory_order_relaxed);
    activation.sourceWaitSequence_.store(0, std::memory_order_relaxed);
    activation.state_.store(FrontierReceiptState::Empty, std::memory_order_release);
}

std::uint64_t DecisionTable::FrontierStorageBytes() const noexcept {
    return frontiers_.RequestedBytes();
}

bool DecisionTable::ValidateFrontierDecisionApply(DecisionToken token,
    std::uint64_t audioSequence, FrontierDecisionApplyProof* out) noexcept {
    Row* row = Resolve(token);
    auto* activation = ResolveFrontierActivation(token);
    if (row == nullptr || activation == nullptr || out == nullptr || audioSequence == 0 ||
        activation->State() != FrontierReceiptState::AudioStaging || activation->lease_ == nullptr ||
        activation->lease_->Capture().state != AudioSliceState::Ready ||
        row->commit.load(std::memory_order_acquire) != DecisionCommitState::Committed ||
        row->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
        row->sliceCount != 1 || row->consumedSlices.load(std::memory_order_acquire) != 0 ||
        row->lastAudioSequence.load(std::memory_order_acquire) > audioSequence) return false;
    out->row_ = row;
    out->table_ = this;
    out->activation_ = activation;
    out->lease_ = activation->lease_;
    out->audioSequence_ = audioSequence;
    out->sliceIndex_ = 0;
    out->prelude_ = false;
    return true;
}

void DecisionTable::ApplyClaimedFrontierDecision(const FrontierDecisionApplyProof& proof) noexcept {
    assert(proof.table_ == this && proof.Valid() && proof.lease_ != nullptr);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    auto& row = *static_cast<Row*>(proof.row_);
    assert(row.audioState.load(std::memory_order_acquire) == AudioGroupState::Pending ||
        row.audioState.load(std::memory_order_acquire) == AudioGroupState::AudioClaimed ||
        row.audioState.load(std::memory_order_acquire) == AudioGroupState::Admitted);
    row.audioState.store(proof.prelude_ ? AudioGroupState::AudioClaimed : AudioGroupState::Admitted,
        std::memory_order_release);
    row.lastAudioSequence.store(proof.audioSequence_, std::memory_order_relaxed);
    row.consumedSlices.store(static_cast<std::uint16_t>(proof.sliceIndex_ + 1u), std::memory_order_release);
}

bool DecisionTable::HasFrontierDecisionBinding(DecisionToken token) const noexcept {
    return Resolve(token) != nullptr && frontiers_[token.slot].operationPool != nullptr;
}

bool DecisionTable::MayMutateFrontierDecision(DecisionToken token,
    AudioSliceInvalidateReason reason) const noexcept {
    if (PreparedTicketHeld(token) || FrontierPublicationHeld(token)) { return false; }
    if (!HasFrontierDecisionBinding(token)) return true;
    const auto& sidecar = frontiers_[token.slot];
    return sidecar.operationPool->InvalidateFrontierOwnerOperations(sidecar.operationOwner, token, reason);
}

bool DecisionTable::BindFrontierDecision(DecisionToken token,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    auto* row = Resolve(token);
    Plan03PageGroupView group{};
    if (row == nullptr || !binding.Valid() || binding.role == FrontierGroupRole::Coordinator ||
        binding.operation.group.decision.slot != token.slot || binding.operation.group.decision.generation != token.generation ||
        binding.operation.world != row->world || binding.operation.transport != row->transport ||
        !pool.PrivatePlan03GroupBindable(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &group) || group.frontier != &binding ||
        group.lease == nullptr || group.lease->Capture().state != AudioSliceState::Ready ||
        ResolveFrontierActivation(token) != nullptr) return false;
    auto& sidecar = frontiers_[token.slot];
    if (sidecar.operationPool != nullptr)
        return sidecar.operationPool == &pool && sidecar.operationOwner == binding.operation.owner;
    if (!AcquireFuturePin(token, &sidecar.observerPin)) return false;
    sidecar.operationOwner = binding.operation.owner;
    sidecar.operationPool = &pool;
    return true;
}

bool DecisionTable::IsFrontierDecisionBindingFor(DecisionToken token,
    const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) const noexcept {
    const auto* row = Resolve(token);
    Plan03PageGroupView original{};
    return row != nullptr && binding.Valid() && binding.role != FrontierGroupRole::Coordinator &&
        binding.operation.group.decision.slot == token.slot &&
        binding.operation.group.decision.generation == token.generation &&
        binding.operation.world == row->world && binding.operation.transport == row->transport &&
        HasFrontierDecisionBinding(token) && frontiers_[token.slot].operationPool == &pool &&
        frontiers_[token.slot].operationOwner == binding.operation.owner &&
        pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &original) &&
        original.frontier == &binding;
}

bool DecisionTable::ReleaseFrontierDecisionBinding(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    if (!HasFrontierDecisionBinding(token)) return false;
    auto& sidecar = frontiers_[token.slot];
    if (sidecar.operationPool->HasFrontierOwnerOperations(sidecar.operationOwner, token) ||
        !sidecar.observerPin.Release()) return false;
    sidecar.operationPool = nullptr;
    sidecar.operationOwner = {};
    return true;
}

bool DecisionTable::CanRollbackPreparedFrontierDecision(DecisionToken token, DecisionToken coordinator,
    const FrontierGroupBinding* binding) const noexcept {
    const auto* row = Resolve(token);
    const auto* activation = ResolveFrontierActivation(coordinator);
    if (row == nullptr || activation == nullptr || activation->State() != FrontierReceiptState::Prepared ||
        activation->lease_ == nullptr || activation->lease_->Capture().state != AudioSliceState::Ready ||
        row->ticket.sequence != activation->Descriptor().order.logicCommitSequence ||
        row->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
        row->consumedSlices.load(std::memory_order_acquire) != 0 ||
        row->lastAudioSequence.load(std::memory_order_acquire) != 0) return false;
    if (token.slot != coordinator.slot || token.generation != coordinator.generation) {
        Plan03PageGroupView group{};
        if (binding == nullptr || !binding->Valid() || binding->role != FrontierGroupRole::EntryMember ||
            !(binding->entry == activation->Descriptor().entry) ||
            !(binding->primary == activation->Descriptor().primary) ||
            binding->operation.group.decision.slot != token.slot ||
            binding->operation.group.decision.generation != token.generation ||
            !HasFrontierDecisionBinding(token) ||
            !(frontiers_[token.slot].operationOwner == binding->operation.owner) ||
            !frontiers_[token.slot].operationPool->PrivatePlan03GroupBindable(
                binding->operation.group) ||
            !frontiers_[token.slot].operationPool->ResolvePlan03GroupSliceProducer(binding->operation.group, &group) ||
            group.frontier != binding || group.lease == nullptr ||
            group.lease->Capture().state != AudioSliceState::Ready) return false;
    } else if (binding != nullptr) return false;
    const auto state = row->commit.load(std::memory_order_acquire);
    return state == DecisionCommitState::Open || state == DecisionCommitState::Committed;
}

bool DecisionTable::RollbackPreparedFrontierDecision(DecisionToken token, DecisionToken coordinator,
    const FrontierGroupBinding* binding) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    if (!CanRollbackPreparedFrontierDecision(token, coordinator, binding)) return false;
    auto* row = Resolve(token);
    auto expected = DecisionCommitState::Committed;
    if (!row->commit.compare_exchange_strong(expected, DecisionCommitState::Open,
            std::memory_order_acq_rel, std::memory_order_acquire)) return expected == DecisionCommitState::Open;
    row->participantCount = 0;
    row->pages = {};
    row->armGroup = {};
    return true;
}

bool DecisionTable::CanRollbackPrivatePendingQueueFrontierDecision(const FrontierGroupBinding& binding,
    const ScheduledTransactionPool& pool, const PrivatePendingQueueFrontierBinding& queue) const noexcept {
    const auto* row = Resolve(binding.operation.group.decision);
    if (!queue.Matches(binding, pool) || !MatchesPreparedFrontierActivationPool(binding.entry, pool) ||
        !IsFrontierDecisionBindingFor(binding.operation.group.decision, binding, pool) || row == nullptr ||
        row->transitionGate.Valid() != binding.transition.Valid() ||
        (binding.transition.Valid() && (row->transitionGate.slot != binding.transition.slot ||
            row->transitionGate.generation != binding.transition.generation))) return false;
    if (binding.role == FrontierGroupRole::EntryMember)
        return CanRollbackPreparedFrontierDecision(binding.operation.group.decision, binding.entry.coordinator, &binding);
    const auto* activation = ResolveFrontierActivation(binding.entry.coordinator);
    Plan03PageGroupView native{};
    if (binding.role != FrontierGroupRole::Future || binding.operation.kind != FrontierOperationKind::Dependent ||
        binding.operation.group.sliceIndex != 1 || row == nullptr || activation == nullptr ||
        binding.entry != activation->Descriptor().entry || binding.primary != activation->Descriptor().primary ||
        row->sliceCount != 2 || row->ticket.sequence != activation->Descriptor().order.logicCommitSequence ||
        row->world != binding.operation.world || row->transport != binding.operation.transport ||
        !row->transitionGate.Valid() || row->transitionGate.slot != binding.transition.slot ||
        row->transitionGate.generation != binding.transition.generation ||
        row->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
        row->consumedSlices.load(std::memory_order_acquire) != 0 || row->lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        row->authoredStopInvalidated.load(std::memory_order_acquire) ||
        !pool.PrivateImmediateLaneFrontierRollbackReady(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &native) || native.frontier != &binding ||
        native.lease == nullptr || native.lease->Capture().state != AudioSliceState::Ready) return false;
    const auto state = row->commit.load(std::memory_order_acquire);
    return state == DecisionCommitState::Open || state == DecisionCommitState::Committed;
}

bool DecisionTable::RollbackPrivatePendingQueueFrontierDecision(const FrontierGroupBinding& binding,
    const ScheduledTransactionPool& pool, const PrivatePendingQueueFrontierBinding& queue) noexcept {
    const auto token = binding.operation.group.decision;
    if (PreparedTicketHeld(token) || !CanRollbackPrivatePendingQueueFrontierDecision(binding, pool, queue)) return false;
    if (binding.role == FrontierGroupRole::EntryMember)
        return RollbackPreparedFrontierDecision(token, binding.entry.coordinator, &binding);
    auto* row = Resolve(token);
    auto expected = DecisionCommitState::Committed;
    if (!row->commit.compare_exchange_strong(expected, DecisionCommitState::Open,
            std::memory_order_acq_rel, std::memory_order_acquire)) return expected == DecisionCommitState::Open;
    row->participantCount = 0; row->pages = {}; row->armGroup = {};
    return true;
}

bool DecisionTable::ValidateFrontierMemberDecisionApply(const FrontierGroupBinding& binding,
    AudioSliceLease& lease, std::uint64_t audioSequence, FrontierDecisionApplyProof* out) noexcept {
    const auto token = binding.operation.group.decision;
    auto* row = Resolve(token);
    if (row == nullptr || out == nullptr || audioSequence == 0 || !binding.Valid() ||
        binding.role == FrontierGroupRole::Coordinator || !HasFrontierDecisionBinding(token) ||
        binding.operation.group.sliceIndex >= row->sliceCount ||
        row->transitionGate.Valid() != binding.transition.Valid() ||
        (row->transitionGate.Valid() && (row->transitionGate.slot != binding.transition.slot ||
            row->transitionGate.generation != binding.transition.generation)) ||
        (binding.role == FrontierGroupRole::EntryMember && binding.operation.group.sliceIndex != 0) ||
        row->commit.load(std::memory_order_acquire) != DecisionCommitState::Committed ||
        row->audioState.load(std::memory_order_acquire) != (binding.operation.group.sliceIndex == 0 ?
            AudioGroupState::Pending : (binding.variableSide.Valid() && row->sliceCount == 2 ?
                AudioGroupState::AudioClaimed : AudioGroupState::Admitted)) ||
        row->authoredStopInvalidated.load(std::memory_order_acquire) ||
        row->consumedSlices.load(std::memory_order_acquire) != binding.operation.group.sliceIndex ||
        row->lastAudioSequence.load(std::memory_order_acquire) > audioSequence ||
        row->world != binding.operation.world || row->transport != binding.operation.transport ||
        lease.Capture().state != AudioSliceState::Ready) return false;
    const auto& sidecar = frontiers_[token.slot];
    Plan03PageGroupView group{};
    if (!(sidecar.operationOwner == binding.operation.owner) ||
        !sidecar.operationPool->ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease) return false;
    out->row_ = row;
    out->table_ = this;
    out->activation_ = nullptr;
    out->lease_ = &lease;
    out->audioSequence_ = audioSequence;
    out->sliceIndex_ = binding.operation.group.sliceIndex;
    out->prelude_ = binding.variableSide.Valid() && row->sliceCount == 2 && out->sliceIndex_ == 0;
    return true;
}

bool DecisionTable::PrepareFrontierActivation(DecisionToken token,
    const FrontierActivationDescriptor& descriptor, ScheduledTransactionPool& pool) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    Row* row = Resolve(token);
    Plan03PageGroupView primary{};
    const bool committedExact = row != nullptr &&
        row->commit.load(std::memory_order_acquire) == DecisionCommitState::Committed &&
        descriptor.order.logicCommitSequence == row->ticket.sequence &&
        descriptor.expectedMemberCount == row->participantCount;
    if (row == nullptr || (!IsOpen(token) && !committedExact) || !descriptor.Valid() ||
        descriptor.entry.coordinator.slot != token.slot ||
        descriptor.entry.coordinator.generation != token.generation ||
        descriptor.worldGeneration != row->world || descriptor.primary.transportEpoch != row->transport ||
        descriptor.expectedMemberCount > maxParticipantsPerDecision_ ||
        !pool.ResolvePlan03GroupSliceProducer(descriptor.primary, &primary) ||
        primary.memberCount != 0 || primary.sourceControlCount != 0) return false;
    auto& sidecar = frontiers_[token.slot];
    if (sidecar.activation.State() != FrontierReceiptState::Empty || sidecar.pagePin.has_value() ||
        sidecar.observerPin.Valid()) return false;
    sidecar.pagePin.emplace();
    if (!pool.AcquirePlan03GroupReader(descriptor.primary, &*sidecar.pagePin)) {
        sidecar.pagePin.reset();
        return false;
    }
    if (!AcquireFuturePin(token, &sidecar.observerPin)) {
        (void)sidecar.pagePin->Release(0);
        sidecar.pagePin.reset();
        return false;
    }
    if (!sidecar.activation.Prepare(descriptor, primary.lease)) {
        (void)sidecar.observerPin.Release();
        (void)sidecar.pagePin->Release(0);
        sidecar.pagePin.reset();
        return false;
    }
    return true;
}

FrontierActivation* DecisionTable::ResolveFrontierActivation(DecisionToken token) noexcept {
    return Resolve(token) != nullptr && frontiers_[token.slot].activation.State() != FrontierReceiptState::Empty ?
        &frontiers_[token.slot].activation : nullptr;
}

const FrontierActivation* DecisionTable::ResolveFrontierActivation(DecisionToken token) const noexcept {
    return const_cast<DecisionTable*>(this)->ResolveFrontierActivation(token);
}

bool DecisionTable::MatchesPreparedFrontierActivationPool(const FrontierEntryReceiptRef& entry,
    const ScheduledTransactionPool& pool) const noexcept {
    const auto* activation = ResolveFrontierActivation(entry.coordinator);
    Plan03PageGroupView primary{};
    if (!entry.Valid() || activation == nullptr || activation->State() != FrontierReceiptState::Prepared ||
        !(activation->Descriptor().entry == entry)) return false;
    const auto& sidecar = frontiers_[entry.coordinator.slot];
    return sidecar.pagePin.has_value() && sidecar.observerPin.Valid() &&
        pool.IsPlan03GroupReaderFor(*sidecar.pagePin, activation->Descriptor().primary) &&
        pool.ResolvePlan03GroupSliceProducer(activation->Descriptor().primary, &primary) &&
        primary.memberCount == 0 && primary.sourceControlCount == 0 && primary.lease != nullptr &&
        primary.lease == activation->lease_ && primary.lease->Capture().state == AudioSliceState::Ready;
}

bool DecisionTable::CaptureFrontierActivationSidecar(DecisionToken token,
    const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
    FrontierActivationSidecarSnapshot* out) const noexcept {
    const auto* activation = ResolveFrontierActivation(token);
    if (out == nullptr || !entry.Valid() ||
        entry.coordinator.slot != token.slot ||
        entry.coordinator.generation != token.generation ||
        activation == nullptr || !(activation->Descriptor().entry == entry)) return false;
    const auto& sidecar = frontiers_[token.slot];
    FrontierActivationSidecarSnapshot snapshot{};
    snapshot.state = activation->State();
    snapshot.primary = activation->Descriptor().primary;
    snapshot.pagePinHeld = sidecar.pagePin.has_value();
    snapshot.observerPinHeld = sidecar.observerPin.Valid();
    snapshot.receiptRead = activation->Read(entry, completedAudioSequence, &snapshot.receipt);
    *out = snapshot;
    return true;
}

bool DecisionTable::CanPublishFrontierActivation(DecisionToken token) const noexcept {
    const Row* row = Resolve(token);
    const auto* activation = ResolveFrontierActivation(token);
    if (row == nullptr || activation == nullptr ||
        activation->State() != FrontierReceiptState::Prepared ||
        row->commit.load(std::memory_order_acquire) != DecisionCommitState::Committed ||
        activation->descriptor_.order.logicCommitSequence != row->ticket.sequence ||
        activation->descriptor_.expectedMemberCount != row->participantCount) return false;
    const auto* participants = Participants(token.slot);
    for (std::uint32_t i = 0; i < row->participantCount; ++i) {
        if (participants[i].kind != DecisionParticipantKind::Subdecision) return false;
    }
    return true;
}

bool DecisionTable::FrontierActivationBindingsDetached(const FrontierEntryReceiptRef& entry,
    const ScheduledTransactionPool& pool, std::uint64_t completedAudioSequence) const noexcept {
    const auto* activation = ResolveFrontierActivation(entry.coordinator);
    if (!entry.Valid() || activation == nullptr || !(activation->Descriptor().entry == entry)) return false;
    const auto& sidecar = frontiers_[entry.coordinator.slot];
    if (activation->State() == FrontierReceiptState::Prepared) {
        if (completedAudioSequence != 0 || !sidecar.pagePin.has_value() || !sidecar.observerPin.Valid())
            return false;
    } else {
        FrontierActivationReceipt receipt{};
        if (activation->Read(entry, completedAudioSequence, &receipt) != FrontierReceiptRead::Ready) return false;
    }
    // Exclude only this activation's original structural loan, never an arbitrary reader.
    return pool.FrontierCoordinatorDetached(activation->Descriptor().primary, entry,
        completedAudioSequence, sidecar.pagePin.has_value() ? &*sidecar.pagePin : nullptr) &&
        !pool.HasFrontierOwnerOperations(entry.owner, entry.coordinator);
}

bool DecisionTable::PublishFrontierActivation(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    if (!CanPublishFrontierActivation(token)) return false;
    auto* activation = ResolveFrontierActivation(token);
    auto expected = FrontierReceiptState::Prepared;
    return activation->state_.compare_exchange_strong(expected, FrontierReceiptState::Pending,
        std::memory_order_release, std::memory_order_relaxed);
}

bool DecisionTable::CancelPreparedFrontierActivation(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    auto* activation = ResolveFrontierActivation(token);
    if (activation == nullptr || activation->State() != FrontierReceiptState::Prepared) return false;
    auto& sidecar = frontiers_[token.slot];
    if (!sidecar.pagePin.has_value() || !sidecar.pagePin->Release(0) ||
        !sidecar.observerPin.Release()) return false;
    sidecar.pagePin.reset();
    activation->lease_ = nullptr;
    activation->descriptor_ = {};
    activation->receipt_ = {};
    activation->state_.store(FrontierReceiptState::Empty, std::memory_order_release);
    return true;
}

bool DecisionTable::ReconcileFrontierActivation(DecisionToken token,
    const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
    FrontierActivationReceipt* out) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    auto* activation = ResolveFrontierActivation(token);
    FrontierActivationReceipt receipt{};
    if (activation == nullptr || out == nullptr ||
        activation->Read(entry, completedAudioSequence, &receipt) != FrontierReceiptRead::Ready) return false;
    auto& sidecar = frontiers_[token.slot];
    if (sidecar.pagePin.has_value()) {
        if (!sidecar.pagePin->Release(completedAudioSequence)) return false;
        sidecar.pagePin.reset();
        activation->lease_ = nullptr;
    }
    if (!sidecar.observerPin.Release()) return false;
    *out = receipt;
    return true;
}

DecisionParticipantRef* DecisionTable::Participants(
    std::uint32_t slot) noexcept {
    return &participants_[slot * maxParticipantsPerDecision_];
}

const DecisionParticipantRef* DecisionTable::Participants(
    std::uint32_t slot) const noexcept {
    return const_cast<DecisionTable*>(this)->Participants(slot);
}

DecisionTable::Row* DecisionTable::Resolve(DecisionToken token) noexcept {
    if (!token.Valid() || token.slot >= rows_.ConstructedCount()) {
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

const DecisionTable::Row* DecisionTable::Resolve(
    DecisionToken token) const noexcept {
    return const_cast<DecisionTable*>(this)->Resolve(token);
}

std::uint32_t DecisionTable::LiveCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Live) {
            ++count;
        }
    }
    return count;
}

std::uint32_t DecisionTable::QuarantinedCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < rows_.ConstructedCount(); ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Quarantined) {
            ++count;
        }
    }
    return count;
}

bool DecisionTable::Reserve(const DecisionReserveRequest& request,
                            DecisionToken* outToken,
                            OriginalDecisionLifetimePin* outOriginal) noexcept {
    if (outToken == nullptr || maxParticipantsPerDecision_ == 0 ||
        !ValidRequest(request) || !CanIssueOriginalDecisionLifetime(outOriginal)) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (!PrepareSlotStorage(slot)) return false;
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Free ||
            row.reservation.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        SlotState expected = SlotState::Free;
        row.ticket = request.ticket;
        row.world = request.world;
        row.transport = request.transport;
        row.continuationGeneration.store(
            request.expectedContinuationGeneration,
            std::memory_order_relaxed);
        row.audioState.store(AudioGroupState::Pending,
                             std::memory_order_relaxed);
        row.authoredStopInvalidated.store(false, std::memory_order_relaxed);
        row.sliceCount = request.sliceCount;
        row.consumedSlices.store(0, std::memory_order_relaxed);
        row.lastAudioSequence.store(0, std::memory_order_relaxed);
        row.transitionGate = request.transitionGate;
        row.participantCount = 0;
        row.pages = {};
        row.armGroup = {};
        row.futurePinCount.store(0, std::memory_order_relaxed);
        row.commit.store(DecisionCommitState::Open,
                         std::memory_order_relaxed);
        row.identity.store(PackIdentity(slot, row.generation),
                           std::memory_order_release);
        if (row.slotState.compare_exchange_strong(
                expected, SlotState::Live, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (outOriginal != nullptr)
                IssueOriginalDecisionLifetime(row, {slot, row.generation}, request, *outOriginal);
            row.reservation.clear(std::memory_order_release);
            *outToken = {slot, row.generation};
            return true;
        }
        row.identity.store(0, std::memory_order_release);
        row.reservation.clear(std::memory_order_release);
    }
    return false;
}

bool DecisionTable::PrepareReservation(
    const DecisionReserveRequest& request,
    PreparedDecisionReservation* out,
    OriginalDecisionLifetimePin* outOriginal) noexcept {
    if (out == nullptr || out->Valid() ||
        maxParticipantsPerDecision_ == 0 ||
        !ValidPreparedRequest(request) || !CanIssueOriginalDecisionLifetime(outOriginal)) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (!PrepareSlotStorage(slot)) return false;
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Free ||
            row.reservation.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        SlotState expected = SlotState::Free;
        row.ticket = {};
        row.world = request.world;
        row.transport = request.transport;
        row.continuationGeneration.store(
            request.expectedContinuationGeneration,
            std::memory_order_relaxed);
        row.audioState.store(AudioGroupState::Pending,
                             std::memory_order_relaxed);
        row.authoredStopInvalidated.store(false, std::memory_order_relaxed);
        row.sliceCount = request.sliceCount;
        row.consumedSlices.store(0, std::memory_order_relaxed);
        row.lastAudioSequence.store(0, std::memory_order_relaxed);
        row.transitionGate = request.transitionGate;
        row.participantCount = 0;
        row.pages = {};
        row.armGroup = {};
        row.futurePinCount.store(0, std::memory_order_relaxed);
        row.commit.store(DecisionCommitState::Open,
                         std::memory_order_relaxed);
        row.identity.store(PackIdentity(slot, row.generation),
                           std::memory_order_release);
        if (row.slotState.compare_exchange_strong(
                expected, SlotState::Live, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (outOriginal != nullptr)
                IssueOriginalDecisionLifetime(row, {slot, row.generation}, request, *outOriginal);
            row.reservation.clear(std::memory_order_release);
            out->owner_ = this;
            out->token_ = {slot, row.generation};
            out->prepared_ = true;
            out->originalRequest_ = request;
            out->attachedTransitionGate_ = {};
            out->attachedSliceCount_ = 0;
            out->originalLifetimeMarker_ = row.originalLifetimeMarker;
            return true;
        }
        row.identity.store(0, std::memory_order_release);
        row.reservation.clear(std::memory_order_release);
    }
    return false;
}

bool DecisionTable::CommitPreparedReservation(
    PreparedDecisionReservation* prepared,
    LogicCommitSequence sequence,
    DecisionToken* outToken) noexcept {
    if (PreparedTicketHeld(prepared != nullptr ? prepared->token_ : DecisionToken{})) { return false; }
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || sequence == 0 || outToken == nullptr) {
        return false;
    }
    Row* row = Resolve(prepared->token_);
    if (row == nullptr || row->ticket.Valid() ||
        (prepared->originalLifetimeMarker_ != 0 &&
         (row->originalLifetimeMarker != prepared->originalLifetimeMarker_ ||
          !SameOriginalDecisionRequest(prepared->originalRequest_, row->originalLifetimeRequest))) ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Open) {
        return false;
    }
    row->ticket.sequence = sequence;
    *outToken = prepared->token_;
    prepared->owner_ = nullptr;
    prepared->token_ = {};
    prepared->prepared_ = false;
    prepared->originalLifetimeMarker_ = 0;
    prepared->attachedTransitionGate_ = {};
    prepared->attachedSliceCount_ = 0;
    return true;
}

bool DecisionTable::CancelPreparedReservation(
    PreparedDecisionReservation* prepared,
    OriginalDecisionLifetimePin* originalBaseline) noexcept {
    if (PreparedTicketHeld(prepared != nullptr ? prepared->token_ : DecisionToken{})) { return false; }
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_) {
        return false;
    }
    const DecisionToken token = prepared->token_;
    originalBaseline = ResolveOriginalLifetimeBaseline(token, originalBaseline);
    if (originalBaseline != nullptr && !IsOriginalDecisionLifetimePinFor(*originalBaseline, token)) return false;
    Row* row = Resolve(token);
    if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    struct ReservationUnlock {
        std::atomic_flag& flag;
        ~ReservationUnlock() { flag.clear(std::memory_order_release); }
    } unlock{row->reservation};
    if (row->ticket.Valid() || row->futurePinCount.load(std::memory_order_acquire) != 0 ||
        ResolveFrontierActivation(token) != nullptr || HasFrontierDecisionBinding(token)) {
        return false;
    }
    const auto commit = row->commit.load(std::memory_order_acquire);
    if ((commit != DecisionCommitState::Open && commit != DecisionCommitState::Cancelled) ||
        (commit == DecisionCommitState::Open && !Cancel(token))) {
        return false;
    }
    const bool reclaimed = ReclaimCancelled(token, originalBaseline);
    const auto& sidecar = frontiers_[token.slot];
    const bool disposedFinalGeneration = !reclaimed && token.generation == UINT32_MAX &&
        row->generation == token.generation && row->identity.load(std::memory_order_acquire) == 0 &&
        row->slotState.load(std::memory_order_acquire) == SlotState::Quarantined &&
        sidecar.operationPool == nullptr && !sidecar.pagePin.has_value() &&
        !sidecar.observerPin.Valid() && sidecar.controlProgram.generation == 0 &&
        sidecar.activation.State() == FrontierReceiptState::Empty;
    if (!reclaimed && !disposedFinalGeneration) {
        return false;
    }
    prepared->owner_ = nullptr;
    prepared->token_ = {};
    prepared->prepared_ = false;
    prepared->originalLifetimeMarker_ = 0;
    prepared->attachedTransitionGate_ = {};
    prepared->attachedSliceCount_ = 0;
    return true;
}

bool DecisionTable::IsOpen(DecisionToken token) const noexcept {
    const Row* row = Resolve(token);
    return row != nullptr &&
           row->commit.load(std::memory_order_acquire) ==
               DecisionCommitState::Open;
}

bool DecisionTable::MatchesOpenReservation(DecisionToken token,
    const DecisionReserveRequest& request) const noexcept {
    const Row* row = Resolve(token);
    return row != nullptr &&
        row->commit.load(std::memory_order_acquire) == DecisionCommitState::Open &&
        row->ticket.sequence == request.ticket.sequence &&
        row->world == request.world && row->transport == request.transport &&
        row->continuationGeneration.load(std::memory_order_acquire) ==
            request.expectedContinuationGeneration &&
        row->sliceCount == request.sliceCount &&
        row->transitionGate.slot == request.transitionGate.slot &&
        row->transitionGate.generation == request.transitionGate.generation &&
        row->futurePinCount.load(std::memory_order_acquire) == 0;
}

bool DecisionTable::IsCancelled(DecisionToken token) const noexcept {
    const Row* row = Resolve(token);
    return row != nullptr &&
           row->commit.load(std::memory_order_acquire) ==
               DecisionCommitState::Cancelled;
}

bool DecisionTable::MatchesCancelledReservation(DecisionToken token,
    const DecisionReserveRequest& request) const noexcept {
    const Row* row = Resolve(token);
    return row != nullptr &&
        row->commit.load(std::memory_order_acquire) == DecisionCommitState::Cancelled &&
        row->ticket.sequence == request.ticket.sequence &&
        row->world == request.world && row->transport == request.transport &&
        row->continuationGeneration.load(std::memory_order_acquire) == request.expectedContinuationGeneration &&
        row->sliceCount == request.sliceCount &&
        row->transitionGate.slot == request.transitionGate.slot &&
        row->transitionGate.generation == request.transitionGate.generation &&
        row->futurePinCount.load(std::memory_order_acquire) == 0;
}

bool DecisionTable::AttachPreparedTransitionGateBeforeCommit(
    PreparedDecisionReservation& original,
    TransitionAudibleGateRef transitionGate,
    std::uint16_t sliceCount) noexcept {
    if (original.owner_ != this || !original.prepared_ ||
        original.attachedTransitionGate_.Valid() || original.originalRequest_.transitionGate.Valid() ||
        !transitionGate.Valid() || sliceCount == 0 || PreparedTicketHeld(original.token_))
        return false;
    auto* row = Resolve(original.token_);
    if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto fail = [&]() noexcept { row->reservation.clear(std::memory_order_release); return false; };
    const auto& request = original.originalRequest_;
    const auto& sidecar = frontiers_[original.token_.slot];
    if (row->ticket.Valid() || row->commit.load(std::memory_order_acquire) != DecisionCommitState::Open ||
        row->world != request.world || row->transport != request.transport ||
        row->continuationGeneration.load(std::memory_order_acquire) != request.expectedContinuationGeneration ||
        row->sliceCount != request.sliceCount || row->transitionGate.Valid() ||
        row->audioState.load(std::memory_order_acquire) != AudioGroupState::Pending ||
        row->consumedSlices.load(std::memory_order_acquire) != 0 ||
        row->lastAudioSequence.load(std::memory_order_acquire) != 0 ||
        row->authoredStopInvalidated.load(std::memory_order_acquire) || row->participantCount != 0 ||
        row->pages.Valid() || row->armGroup.Valid() || row->futurePinCount.load(std::memory_order_acquire) != 0 ||
        sidecar.activation.State() != FrontierReceiptState::Empty || sidecar.pagePin.has_value() ||
        sidecar.observerPin.Valid() || sidecar.operationPool != nullptr ||
        (original.originalLifetimeMarker_ != 0 &&
         (row->originalLifetimeMarker != original.originalLifetimeMarker_ ||
          row->originalLifetimePinCount != 1 ||
          !SameOriginalDecisionRequest(request, row->originalLifetimeRequest)))) return fail();
    row->transitionGate = transitionGate;
    row->sliceCount = sliceCount;
    original.attachedTransitionGate_ = transitionGate;
    original.attachedSliceCount_ = sliceCount;
    row->reservation.clear(std::memory_order_release);
    return true;
}

bool DecisionTable::AttachTransitionGateBeforeCommit(
    DecisionToken token,
    TransitionAudibleGateRef transitionGate,
    std::uint16_t sliceCount) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    if (!transitionGate.Valid() || sliceCount == 0) {
        return false;
    }
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Open ||
        row->transitionGate.Valid()) {
        return false;
    }
    row->transitionGate = transitionGate;
    row->sliceCount = sliceCount;
    return true;
}

bool DecisionTable::Commit(DecisionToken token,
                           const DecisionCommitPayload& payload) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    if (!payload.pages.Valid() || !payload.armGroup.Valid() ||
        payload.participantCount > maxParticipantsPerDecision_ ||
        (payload.participantCount != 0 && payload.participants == nullptr)) {
        return false;
    }
    for (std::uint32_t index = 0; index < payload.participantCount; ++index) {
        if (!payload.participants[index].Valid()) {
            return false;
        }
    }
    Row* row = Resolve(token);
    if (row == nullptr) {
        return false;
    }
    DecisionCommitState expected = DecisionCommitState::Open;
    if (!row->commit.compare_exchange_strong(
            expected, DecisionCommitState::CommitSealed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    DecisionParticipantRef* destination = Participants(token.slot);
    for (std::uint32_t index = 0; index < payload.participantCount; ++index) {
        destination[index] = payload.participants[index];
    }
    row->participantCount = payload.participantCount;
    row->pages = payload.pages;
    row->armGroup = payload.armGroup;
    row->commit.store(DecisionCommitState::Committed,
                      std::memory_order_release);
    return true;
}

bool DecisionTable::Cancel(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr || ResolveFrontierActivation(token) != nullptr || HasFrontierDecisionBinding(token)) {
        return false;
    }
    DecisionCommitState expected = DecisionCommitState::Open;
    return row->commit.compare_exchange_strong(
        expected, DecisionCommitState::Cancelled,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool DecisionTable::TryClaimAudio(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr || ResolveFrontierActivation(token) != nullptr || HasFrontierDecisionBinding(token) ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return false;
    }
    AudioGroupState expected = AudioGroupState::Pending;
    return row->audioState.compare_exchange_strong(
        expected, AudioGroupState::AudioClaimed, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void DecisionTable::ApplyClaimedAudioSlice(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return; }
    Row* row = Resolve(token);
    if (row == nullptr || ResolveFrontierActivation(token) != nullptr || HasFrontierDecisionBinding(token)) return;
    const AudioGroupState prior = row->audioState.exchange(
        AudioGroupState::Admitted, std::memory_order_acq_rel);
    if (prior == AudioGroupState::Invalidated) {
        row->audioState.store(AudioGroupState::PostStartInvalidated,
                              std::memory_order_release);
    } else if (prior == AudioGroupState::DeadlineInvalidated) {
        row->audioState.store(
            AudioGroupState::PostStartDeadlineInvalidated,
            std::memory_order_release);
    }
}

bool DecisionTable::TryInvalidatePending(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    if (!MayMutateFrontierDecision(token, AudioSliceInvalidateReason::Cancel)) return false;
    if (auto* activation = ResolveFrontierActivation(token)) {
        const auto result = activation->Cancel(activation->Descriptor().entry, AudioSliceInvalidateReason::Cancel);
        if (result != FrontierCancelDisposition::CancelledBeforeAudio &&
            result != FrontierCancelDisposition::AlreadyCancelled) return false;
    }
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return false;
    }
    AudioGroupState expected = AudioGroupState::Pending;
    return row->audioState.compare_exchange_strong(
        expected, AudioGroupState::Invalidated, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool DecisionTable::MarkAudioAdmitted(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr || ResolveFrontierActivation(token) != nullptr || HasFrontierDecisionBinding(token) ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return false;
    }
    AudioGroupState expected = AudioGroupState::AudioClaimed;
    if (row->audioState.compare_exchange_strong(
            expected, AudioGroupState::Admitted, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return true;
    }
    return expected == AudioGroupState::PostStartInvalidated ||
           expected == AudioGroupState::PostStartDeadlineInvalidated;
}

bool DecisionTable::RecordAppliedSlice(
    DecisionToken token, PreparedGroupToken pages, std::uint16_t sliceIndex,
    std::uint64_t audioSequence) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr || ResolveFrontierActivation(token) != nullptr || HasFrontierDecisionBinding(token) || !pages.Valid() || audioSequence == 0 ||
        row->commit.load(std::memory_order_acquire) != DecisionCommitState::Committed ||
        row->pages.slot != pages.slot || row->pages.generation != pages.generation ||
        sliceIndex >= row->sliceCount) return false;
    const auto state = row->audioState.load(std::memory_order_acquire);
    if (state != AudioGroupState::AudioClaimed && state != AudioGroupState::Admitted &&
        state != AudioGroupState::PostStartInvalidated &&
        state != AudioGroupState::PostStartDeadlineInvalidated) return false;
    const auto consumed = row->consumedSlices.load(std::memory_order_acquire);
    if (sliceIndex < consumed) return true;
    if (sliceIndex != consumed ||
        audioSequence < row->lastAudioSequence.load(std::memory_order_acquire)) return false;
    row->lastAudioSequence.store(audioSequence, std::memory_order_relaxed);
    row->consumedSlices.store(static_cast<std::uint16_t>(consumed + 1u), std::memory_order_release);
    return true;
}

bool DecisionTable::Retire(DecisionToken token,
                           std::uint64_t audioSequence) noexcept {
    return TryRetire(token, audioSequence) == DecisionRetirementDisposition::Retired;
}

DecisionRetirementDisposition DecisionTable::TryRetire(DecisionToken token,
    std::uint64_t audioSequence) noexcept {
    if (PreparedTicketHeld(token) || FrontierPublicationHeld(token))
        return DecisionRetirementDisposition::NotEligible;
    if (auto* activation = ResolveFrontierActivation(token)) {
        const auto receiptState = activation->State();
        if (receiptState != FrontierReceiptState::AudioFinal &&
            receiptState != FrontierReceiptState::LogicFinal)
            return DecisionRetirementDisposition::NotEligible;
    }
    Row* row = Resolve(token);
    if (row == nullptr || audioSequence == 0 ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return DecisionRetirementDisposition::NotEligible;
    }
    AudioGroupState state =
        row->audioState.load(std::memory_order_acquire);
    if (state != AudioGroupState::Invalidated &&
        state != AudioGroupState::DeadlineInvalidated &&
        state != AudioGroupState::Admitted &&
        state != AudioGroupState::PostStartInvalidated &&
        state != AudioGroupState::PostStartDeadlineInvalidated &&
        state != AudioGroupState::Retired)
        return DecisionRetirementDisposition::NotEligible;
    if (HasFrontierDecisionBinding(token)) {
        const auto& sidecar = frontiers_[token.slot];
        const auto disposition = sidecar.operationPool->TryInvalidateFrontierOwnerOperations(
            sidecar.operationOwner, token, AudioSliceInvalidateReason::Cancel);
        if (disposition == FrontierOwnerInvalidationDisposition::NotEligible)
            return DecisionRetirementDisposition::NotEligible;
        if (disposition == FrontierOwnerInvalidationDisposition::DeferredAfterClaim)
            return DecisionRetirementDisposition::DeferredByFrontier;
    }
    while (state == AudioGroupState::Invalidated ||
           state == AudioGroupState::DeadlineInvalidated ||
           state == AudioGroupState::Admitted ||
           state == AudioGroupState::PostStartInvalidated ||
           state == AudioGroupState::PostStartDeadlineInvalidated) {
        if (row->audioState.compare_exchange_weak(
                state, AudioGroupState::Retired,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            row->lastAudioSequence.store(audioSequence,
                                         std::memory_order_release);
            return DecisionRetirementDisposition::Retired;
        }
    }
    return state == AudioGroupState::Retired ? DecisionRetirementDisposition::Retired :
        DecisionRetirementDisposition::NotEligible;
}

bool DecisionTable::ReclaimRetired(DecisionToken token,
    OriginalDecisionLifetimePin* originalBaseline) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    originalBaseline = ResolveOriginalLifetimeBaseline(token, originalBaseline);
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->audioState.load(std::memory_order_acquire) !=
            AudioGroupState::Retired ||
        row->futurePinCount.load(std::memory_order_acquire) != 0 ||
        !OriginalDecisionRetirementReady(*row, token, originalBaseline)) {
        return false;
    }
    SlotState expected = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    ConsumeOriginalDecisionBaseline(*row, token, originalBaseline);
    ResetFrontierStorage(token.slot);
    row->identity.store(0, std::memory_order_release);
    if (row->generation ==
        (std::numeric_limits<std::uint32_t>::max)()) {
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        // The exact-baseline overload reports completed permanent disposition.
        return originalBaseline != nullptr;
    }
    ++row->generation;
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

DecisionDeadlineDisposition DecisionTable::InvalidateForDeadline(
    DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return DecisionDeadlineDisposition::NotEligible; }
    if (!MayMutateFrontierDecision(token, AudioSliceInvalidateReason::Deadline))
        return DecisionDeadlineDisposition::DeferredByFrontier;
    if (auto* activation = ResolveFrontierActivation(token); activation != nullptr && activation->lease_ != nullptr) {
        const auto result = activation->Cancel(activation->Descriptor().entry, AudioSliceInvalidateReason::Deadline);
        if (result == FrontierCancelDisposition::DeferredToAudio) return DecisionDeadlineDisposition::DeferredByFrontier;
        if (result == FrontierCancelDisposition::Stale) return DecisionDeadlineDisposition::NotEligible;
    }
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return DecisionDeadlineDisposition::Stale;
    }
    AudioGroupState state = row->audioState.load(std::memory_order_acquire);
    for (;;) {
        AudioGroupState desired{};
        switch (state) {
        case AudioGroupState::Pending:
            desired = AudioGroupState::DeadlineInvalidated;
            break;
        case AudioGroupState::AudioClaimed:
            desired = AudioGroupState::PostStartDeadlineInvalidated;
            break;
        case AudioGroupState::Admitted:
            desired = AudioGroupState::PostStartDeadlineInvalidated;
            break;
        case AudioGroupState::DeadlineInvalidated:
        case AudioGroupState::PostStartDeadlineInvalidated:
            return DecisionDeadlineDisposition::AlreadyInvalidated;
        case AudioGroupState::Invalidated:
        case AudioGroupState::PostStartInvalidated:
        case AudioGroupState::Retired:
            return DecisionDeadlineDisposition::NotEligible;
        }
        if (row->audioState.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return DecisionDeadlineDisposition::NewlyInvalidated;
        }
    }
}

DecisionInvalidationDisposition DecisionTable::Invalidate(
    DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return DecisionInvalidationDisposition::NotEligible; }
    if (!MayMutateFrontierDecision(token, AudioSliceInvalidateReason::Cancel))
        return DecisionInvalidationDisposition::DeferredByFrontier;
    if (auto* activation = ResolveFrontierActivation(token); activation != nullptr && activation->lease_ != nullptr) {
        const auto result = activation->Cancel(activation->Descriptor().entry, AudioSliceInvalidateReason::Cancel);
        if (result == FrontierCancelDisposition::DeferredToAudio) return DecisionInvalidationDisposition::DeferredByFrontier;
        if (result == FrontierCancelDisposition::Stale) return DecisionInvalidationDisposition::NotEligible;
    }
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return DecisionInvalidationDisposition::Stale;
    }
    AudioGroupState state = row->audioState.load(std::memory_order_acquire);
    for (;;) {
        AudioGroupState desired{};
        switch (state) {
        case AudioGroupState::Pending:
            desired = AudioGroupState::Invalidated;
            break;
        case AudioGroupState::AudioClaimed:
        case AudioGroupState::Admitted:
            desired = AudioGroupState::PostStartInvalidated;
            break;
        case AudioGroupState::Invalidated:
        case AudioGroupState::PostStartInvalidated:
            return DecisionInvalidationDisposition::AlreadyInvalidated;
        case AudioGroupState::DeadlineInvalidated:
        case AudioGroupState::PostStartDeadlineInvalidated:
        case AudioGroupState::Retired:
            return DecisionInvalidationDisposition::NotEligible;
        }
        if (row->audioState.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return DecisionInvalidationDisposition::NewlyInvalidated;
        }
    }
}

DecisionInvalidationDisposition DecisionTable::InvalidateForAuthoredStop(
    DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return DecisionInvalidationDisposition::NotEligible; }
    Row* row = Resolve(token);
    if (row == nullptr) return DecisionInvalidationDisposition::Stale;
    const auto result = Invalidate(token);
    if (result == DecisionInvalidationDisposition::NewlyInvalidated) {
        row->authoredStopInvalidated.store(true, std::memory_order_release);
    }
    return result;
}

DecisionFuturePin::~DecisionFuturePin() { (void)Release(); }

DecisionFuturePin::DecisionFuturePin(DecisionFuturePin&& other) noexcept
    : owner_(other.owner_), token_(other.token_) {
    other.owner_ = nullptr;
    other.token_ = {};
}

DecisionFuturePin& DecisionFuturePin::operator=(DecisionFuturePin&& other) noexcept {
    if (this != &other) {
        (void)Release();
        owner_ = other.owner_;
        token_ = other.token_;
        other.owner_ = nullptr;
        other.token_ = {};
    }
    return *this;
}

bool DecisionFuturePin::Release() noexcept {
    if (owner_ == nullptr) return true;
    if (!owner_->ReleaseFuturePin(token_)) return false;
    owner_ = nullptr;
    token_ = {};
    return true;
}

bool DecisionTable::AcquireFuturePin(DecisionToken token, DecisionFuturePin* out) noexcept {
    if (PreparedTicketHeld(token) || FrontierPublicationHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || out->Valid() ||
        row->commit.load(std::memory_order_acquire) == DecisionCommitState::Cancelled ||
        row->audioState.load(std::memory_order_acquire) == AudioGroupState::Retired) return false;
    const auto count = row->futurePinCount.load(std::memory_order_relaxed);
    if (count == UINT32_MAX) return false;
    row->futurePinCount.store(count + 1u, std::memory_order_release);
    out->owner_ = this;
    out->token_ = token;
    return true;
}

bool DecisionTable::ReleaseFuturePin(DecisionToken token) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr) return false;
    const auto count = row->futurePinCount.load(std::memory_order_relaxed);
    if (count == 0) return false;
    row->futurePinCount.store(count - 1u, std::memory_order_release);
    return true;
}

bool DecisionTable::CloneFuturePin(const DecisionFuturePin& source, DecisionFuturePin* out) noexcept {
    if (PreparedTicketHeld(source.token_) || FrontierPublicationHeld(source.token_)) { return false; }
    if (source.owner_ != this || out == nullptr || out->Valid()) return false;
    Row* row = Resolve(source.token_);
    if (row == nullptr || row->commit.load(std::memory_order_acquire) == DecisionCommitState::Cancelled ||
        ResolveFrontierActivation(source.token_) == nullptr) return false;
    const auto count = row->futurePinCount.load(std::memory_order_acquire);
    if (count == 0 || count == UINT32_MAX) return false;
    row->futurePinCount.store(count + 1u, std::memory_order_release);
    out->owner_ = this;
    out->token_ = source.token_;
    return true;
}

bool DecisionTable::SetFuturePinCountForTests(DecisionToken token, std::uint32_t count) noexcept {
    if (PreparedTicketHeld(token) || FrontierPublicationHeld(token)) { return false; }
    Row* row = Resolve(token);
    if (row == nullptr) return false;
    row->futurePinCount.store(count, std::memory_order_relaxed);
    return true;
}

bool DecisionTable::ReclaimCancelled(DecisionToken token,
    OriginalDecisionLifetimePin* originalBaseline) noexcept {
    if (PreparedTicketHeld(token)) { return false; }
    originalBaseline = ResolveOriginalLifetimeBaseline(token, originalBaseline);
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Cancelled ||
        row->futurePinCount.load(std::memory_order_acquire) != 0 ||
        !OriginalDecisionRetirementReady(*row, token, originalBaseline)) {
        return false;
    }
    SlotState expected = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    ConsumeOriginalDecisionBaseline(*row, token, originalBaseline);
    ResetFrontierStorage(token.slot);
    row->identity.store(0, std::memory_order_release);
    if (row->generation == std::numeric_limits<std::uint32_t>::max()) {
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        // The exact-baseline overload reports completed permanent disposition.
        return originalBaseline != nullptr;
    }
    ++row->generation;
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool DecisionTable::CaptureCommitted(
    DecisionToken token, DecisionCommittedSnapshot* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed) {
        return false;
    }
    DecisionCommittedSnapshot snapshot{};
    snapshot.commit = DecisionCommitState::Committed;
    snapshot.ticket = row->ticket;
    snapshot.world = row->world;
    snapshot.transport = row->transport;
    snapshot.continuationGeneration =
        row->continuationGeneration.load(std::memory_order_acquire);
    snapshot.audioState = row->audioState.load(std::memory_order_acquire);
    snapshot.authoredStopInvalidated = row->authoredStopInvalidated.load(std::memory_order_acquire);
    snapshot.sliceCount = row->sliceCount;
    snapshot.consumedSlices =
        row->consumedSlices.load(std::memory_order_acquire);
    snapshot.participantCount = row->participantCount;
    snapshot.pages = row->pages;
    snapshot.armGroup = row->armGroup;
    snapshot.futurePinCount =
        row->futurePinCount.load(std::memory_order_acquire);
    snapshot.lastAudioSequence =
        row->lastAudioSequence.load(std::memory_order_acquire);
    snapshot.transitionGate = row->transitionGate;
    *out = snapshot;
    return true;
}

bool DecisionTable::HasExclusiveParticipant(DecisionToken token,
    DecisionParticipantRef participant) const noexcept {
    const auto* row = Resolve(token);
    if (row == nullptr || !participant.Valid() ||
        row->commit.load(std::memory_order_acquire) != DecisionCommitState::Committed) return false;
    const auto* source = Participants(token.slot);
    std::uint32_t matches = 0;
    for (std::uint32_t i = 0; i < row->participantCount; ++i) {
        if (source[i].kind != participant.kind) continue;
        if (source[i].slot != participant.slot || source[i].generation != participant.generation) return false;
        ++matches;
    }
    return matches == 1;
}

bool DecisionTable::CopyCommittedParticipants(
    DecisionToken token, DecisionParticipantRef* out,
    std::uint32_t capacity, std::uint32_t* outCount) const noexcept {
    if (outCount == nullptr) {
        return false;
    }
    const Row* row = Resolve(token);
    if (row == nullptr ||
        row->commit.load(std::memory_order_acquire) !=
            DecisionCommitState::Committed ||
        row->participantCount > capacity ||
        (row->participantCount != 0 && out == nullptr)) {
        return false;
    }
    const DecisionParticipantRef* source = Participants(token.slot);
    for (std::uint32_t index = 0; index < row->participantCount; ++index) {
        out[index] = source[index];
    }
    *outCount = row->participantCount;
    return true;
}

bool DecisionTable::CaptureCommittedParticipant(DecisionToken token, std::uint32_t index,
    DecisionParticipantRef* out) const noexcept {
    const auto* row = Resolve(token);
    if (out == nullptr || row == nullptr ||
        row->commit.load(std::memory_order_acquire) != DecisionCommitState::Committed ||
        index >= row->participantCount) return false;
    *out = Participants(token.slot)[index];
    return true;
}

bool DecisionTable::SetGenerationForTests(
    std::uint32_t slot, std::uint32_t generation) noexcept {
    if (slot >= capacity_ || generation == 0 || !PrepareSlotStorage(slot) ||
        rows_[slot].slotState.load(std::memory_order_acquire) !=
            SlotState::Free) {
        return false;
    }
    rows_[slot].generation = generation;
    return true;
}

} // namespace harken
