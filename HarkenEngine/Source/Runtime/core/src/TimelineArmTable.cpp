#include "harken/core/TimelineArmTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/AuthoredStopFence.h"

#include <algorithm>
#include <cassert>

namespace harken {
namespace {

std::uint64_t PackIdentity(std::uint32_t slot,
                           std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

bool IdentityMatches(const TimelineArmTable::Row& row,
                     TimelineArmToken token) noexcept {
    return row.identity.load(std::memory_order_acquire) ==
           PackIdentity(token.slot, token.generation);
}

bool SameArm(TimelineArmToken left, TimelineArmToken right) noexcept {
    return left.slot == right.slot && left.generation == right.generation &&
        left.renderSlot == right.renderSlot && left.renderGeneration == right.renderGeneration;
}

TimelineArmTable::AuthoredControlEventSnapshot CaptureControlEvent(
    const ScheduledEventInput& event) noexcept {
    return {event.executeFrame, event.payload, event.executionOrdinal,
        event.eventIndex, event.subeventPhase, event.authoredFadeOutCandidate};
}

bool SameControlEvent(const ScheduledEventInput& event,
    const TimelineArmTable::AuthoredControlEventSnapshot& frozen) noexcept {
    // The true Lane or Pool authenticates its later routing sequence separately.
    return event.executeFrame == frozen.executeFrame && event.payload == frozen.payload &&
        event.executionOrdinal == frozen.executionOrdinal && event.eventIndex == frozen.eventIndex &&
        event.subeventPhase == frozen.subeventPhase &&
        event.authoredFadeOutCandidate == frozen.authoredFadeOutCandidate &&
        event.frontier == FrontierEventBinding{};
}

bool ControlPayloadMatches(const TimelineEventPayload& payload,
    const TimelineArmTable::AuthoredControlMetadata& control,
    const TimelineSemanticTarget& target, bool envelope) noexcept {
    if (payload.target != target || payload.authoredStopOperation != control.identity ||
        payload.envelopeEndFrame != control.identity.stopFrame ||
        payload.resolvedOccurrenceIndex != 0 || payload.envelopeIndex != (envelope ? 1u : 0u) ||
        payload.sourceStartFrame != 0 || payload.variableWeightOwner.Valid()) return false;
    if (envelope) {
        return payload.kind == TimelineEventKind::Envelope &&
            payload.reserved == kTimelineEventTransientControlArm &&
            payload.envelopeDirection == TimelineEnvelopeDirection::FadeOut &&
            payload.envelopeStartFrame == control.fadeStartFrame &&
            payload.envelopeCurve == control.curve && payload.authoredFadeOutCandidate;
    }
    return payload.kind == TimelineEventKind::Stop &&
        payload.reserved == (kTimelineEventTransientControlArm | kTimelineEventAuthoredRootStop) &&
        payload.envelopeStartFrame == 0 && payload.envelopeCurve == nullptr &&
        payload.authoredFadeOutCandidate == (control.eventCount == 2);
}

bool CaptureAuthoredControl(const TimelineArmTable::AuthoredControlInput& input,
    TimelineArmToken expectedArm, TimelineArmTable::AuthoredControlMetadata* out) noexcept {
    const auto* view = input.program.program;
    if (input.operation == nullptr || view == nullptr || input.events == nullptr ||
        input.program.programGeneration == 0 ||
        view->generation != input.program.programGeneration || view->payloads == nullptr ||
        view->role != TimelineProgramRole::Voice || view->authoredStopFence != nullptr ||
        input.eventCount == 0 || input.eventCount > 2 || view->payloadCount != input.eventCount ||
        !input.operation->Active(input.operation->Identity())) return false;
    TimelineArmTable::AuthoredControlMetadata control{};
    control.operation = input.operation;
    control.program = input.program;
    control.payloads = view->payloads;
    control.events = input.events;
    control.identity = input.operation->Identity();
    control.onsetFrame = input.onsetFrame;
    control.fadeStartFrame = input.fadeStartFrame;
    control.eventCount = input.eventCount;
    control.eventCapacity = input.eventCount;
    control.stage = TimelineArmTable::AuthoredControlStage::Bound;
    const bool hasEnvelope = input.eventCount == 2;
    if (hasEnvelope) control.curve = view->payloads[0].envelopeCurve;
    const auto dispatchFrame = hasEnvelope ? std::min(control.identity.stopFrame,
        std::max(input.fadeStartFrame, input.onsetFrame)) : control.identity.stopFrame;
    if (input.onsetFrame == UINT64_MAX || control.identity.stopFrame == UINT64_MAX ||
        (hasEnvelope && input.onsetFrame > control.identity.stopFrame) || input.armDeadline > dispatchFrame ||
        (hasEnvelope && input.fadeStartFrame > control.identity.stopFrame)) return false;
    for (std::uint32_t index = 0; index < input.eventCount; ++index) {
        const bool envelope = hasEnvelope && index == 0;
        const auto& event = input.events[index];
        if (!ControlPayloadMatches(view->payloads[index], control, input.target, envelope) ||
            event.payload != index || event.executeFrame !=
                (envelope ? dispatchFrame : control.identity.stopFrame) ||
            event.subeventPhase != (envelope ? 30u : 50u) ||
            event.executionOrdinal != control.identity.executionOrdinal ||
            event.authoredFadeOutCandidate != hasEnvelope ||
            !SameArm(event.armToken, expectedArm) ||
            event.frontier != FrontierEventBinding{} ||
            (index != 0 && event.eventIndex == input.events[0].eventIndex)) return false;
        control.eventsSnapshot[index] = CaptureControlEvent(event);
    }
    *out = control;
    return true;
}

std::uint64_t NextArmTableIncarnation() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto current = next.load(std::memory_order_relaxed);
    while (current != 0) {
        const auto successor = current == UINT64_MAX ? 0 : current + 1;
        if (next.compare_exchange_weak(current, successor,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) return current;
    }
    return 0;
}

// Row reservation is a non-waiting exclusion for exact row mutation, including
// pending arms that have no reader pin yet.
class ArmMutationAccess {
public:
    explicit ArmMutationAccess(TimelineArmTable::Row* row) noexcept : row_(row) {
        if (row_ != nullptr) {
            held_ = !row_->reservation.test_and_set(std::memory_order_acquire);
            if (!held_) row_ = nullptr;
        }
    }
    ~ArmMutationAccess() { if (held_) row_->reservation.clear(std::memory_order_release); }
    explicit operator bool() const noexcept { return row_ != nullptr; }
private:
    TimelineArmTable::Row* row_;
    bool held_ = false;
};

bool FrontierAdmissionDefers(TimelineArmTable::Row* row, bool cancellation) noexcept {
    if (row == nullptr) return false;
    auto* lease = row->frontierActiveLease.load(std::memory_order_acquire);
    if (lease == nullptr) {
        if (row->frontierAdmissionFinished.load(std::memory_order_acquire)) return false;
        lease = row->frontierAdmissionLease;
    }
    if (lease == nullptr) return false;
    const auto state = lease->Capture().state;
    if (state != AudioSliceState::Ready && state != AudioSliceState::SliceClaimed &&
        state != AudioSliceState::SliceClaimedWithInvalidation) return false;
    if (!cancellation) return true;
    return lease->Invalidate(AudioSliceInvalidateReason::Cancel) ==
        AudioSliceInvalidateDisposition::DeferredAfterClaim;
}

AudioSliceLease* ActiveBoundAudioProofLease(
    TimelineArmTable::Row* row, TimelineArmToken token) noexcept {
    if (row == nullptr || !IdentityMatches(*row, token) ||
        row->frontierReaderPins.load(std::memory_order_acquire) == 0)
        return nullptr;
    auto* lease = row->frontierActiveLease.load(std::memory_order_acquire);
    if (lease == nullptr) return nullptr;
    const auto state = row->state.load(std::memory_order_acquire);
    return state == ArmState::Published || state == ArmState::Started ||
        state == ArmState::AudioClaimed ? lease : nullptr;
}

bool InvalidateActiveBoundAudioProof(AudioSliceLease* lease) noexcept {
    const auto state = lease->Capture().state;
    if (state != AudioSliceState::Ready &&
        state != AudioSliceState::SliceClaimed &&
        state != AudioSliceState::SliceClaimedWithInvalidation) return false;
    return lease->Invalidate(AudioSliceInvalidateReason::Cancel) ==
        AudioSliceInvalidateDisposition::DeferredAfterClaim;
}

bool TombstoneReservedRow(TimelineArmTable::Row* row,
    std::uint64_t audioSequence) noexcept {
    ArmState expected = row->state.load(std::memory_order_acquire);
    while (expected == ArmState::CancelRequested || expected == ArmState::Started) {
        row->tombstoneAudioSequence.store(audioSequence, std::memory_order_release);
        if (row->state.compare_exchange_strong(expected, ArmState::Tombstoned,
            std::memory_order_release, std::memory_order_acquire)) {
            if (row->frontierAdmission != nullptr)
                row->frontierAdmissionFinished.store(true, std::memory_order_release);
            return true;
        }
    }
    return false;
}

} // namespace

TimelineArmTable::TimelineArmTable(std::uint32_t capacity)
    : groupMemberScopes_(capacity == 0 ? nullptr : std::make_unique<GroupMemberScope[]>(capacity)),
      rows_(capacity == 0 ? nullptr : std::make_unique<Row[]>(capacity)),
      capacity_(capacity), incarnation_(NextArmTableIncarnation()) {}

TimelineArmTable::~TimelineArmTable() {
    while (unpublishedGroupArmCancellations_ != nullptr)
        unpublishedGroupArmCancellations_->Release();
    // Quiescent receiver detachment protects pools destroyed after this table.
    // It does not certify any running group or Source disposition.
    while (groupMemberReceivers_ != nullptr)
        DetachGroupMemberReceiver(*groupMemberReceivers_);
    assert(liveOriginalArmRowOwners_.load(std::memory_order_relaxed) == 0 &&
        "original row owners must retire before the table");
    assert(liveSourceCommandProofs_.load(std::memory_order_relaxed) == 0);
    while (privateUnboundPageArmRetirements_ != nullptr)
        privateUnboundPageArmRetirements_->Release();
    // Table destruction returns its actual retained loans without an audio receipt.
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        auto& pin = rows_[index].pendingControlSourceReader;
        if (!pin.Valid()) continue;
        const auto* source = PinnedAuthoredControlSource(rows_[index]);
        assert(source != nullptr);
        if (source != nullptr) {
            const auto previous = const_cast<Row*>(source)->frontierReaderPins.fetch_sub(1u);
            assert(previous != 0); (void)previous;
        }
        pin.owner_ = nullptr; pin.token_ = {};
    }
}

namespace {
bool SameOriginalGroupArm(TimelineArmToken a, TimelineArmToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation &&
        a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
}
}

bool TimelineArmTable::IssueOriginalGroupMember(GroupMemberReceiver& receiver,
    const ScheduledTransactionPool& pool, OriginalGroupIdentity& identity,
    TimelineArmToken arm, OriginalGroupMember* out) const noexcept {
    const auto& group = identity.group;
    if (identity.liveMembers == UINT32_MAX ||
        (identity.liveMembers != 0 && (receiver.owner_ != this || identity.receiver != &receiver ||
            identity.armTableIncarnation != incarnation_ ||
            identity.receiverEpisode != receiver.episode_))) return false;
    if (out == nullptr || out->serial_ != 0 || !group.Exact() || incarnation_ == 0 ||
        nextGroupMemberSerial_ == UINT64_MAX ||
        (receiver.owner_ == nullptr && receiver.episode_ == UINT64_MAX) ||
        (receiver.owner_ != nullptr && (receiver.owner_ != this || receiver.pool_ != &pool)))
        return false;
    const auto* row = Resolve(arm);
    if (row == nullptr || row->renderSlot != arm.renderSlot ||
        row->renderGeneration != arm.renderGeneration ||
        row->parentGroup.decision.slot != group.decision.slot ||
        row->parentGroup.decision.generation != group.decision.generation ||
        row->memberIndex == UINT16_MAX) return false;
    auto& scope = groupMemberScopes_[arm.slot];
    if (SameOriginalGroupArm(scope.arm, arm)) {
        if (scope.jointHeld || scope.activeCount == UINT64_MAX) return false;
    } else {
        // Only a real new full identity can start another producer-side scope.
        if (scope.jointHeld) return false;
        scope.arm = arm;
        scope.activeCount = 0;
    }
    if (receiver.owner_ == nullptr) {
        ++receiver.episode_;
        receiver.owner_ = this;
        receiver.pool_ = &pool;
        receiver.next_ = groupMemberReceivers_;
        if (receiver.next_ != nullptr) receiver.next_->previous_ = &receiver;
        groupMemberReceivers_ = &receiver;
    }
    if (identity.liveMembers == 0) {
        identity.receiver = &receiver;
        identity.armTableIncarnation = incarnation_;
        identity.receiverEpisode = receiver.episode_;
    }
    out->arm_ = arm;
    out->groupIdentity_ = &identity;
    ++identity.liveMembers;
    out->serial_ = ++nextGroupMemberSerial_;
    out->memberIndex_ = row->memberIndex;
    ++scope.activeCount;
    ++receiver.activeCount_;
    return true;
}

void TimelineArmTable::ReturnOriginalGroupMember(OriginalGroupMember& original) const noexcept {
    assert(original.serial_ != 0 && original.Receiver() != nullptr &&
        original.Receiver()->owner_ == this && original.ArmTableIncarnation() == incarnation_ &&
        original.ReceiverEpisode() == original.Receiver()->episode_);
    if (original.serial_ == 0 || original.Receiver() == nullptr ||
        original.Receiver()->owner_ != this || original.ArmTableIncarnation() != incarnation_ ||
        original.ReceiverEpisode() != original.Receiver()->episode_)
        return;
    assert(original.groupIdentity_ != nullptr && original.groupIdentity_->liveMembers != 0);
    if (original.groupIdentity_ == nullptr || original.groupIdentity_->liveMembers == 0) return;
    auto& scope = groupMemberScopes_[original.arm_.slot];
    if (SameOriginalGroupArm(scope.arm, original.arm_)) {
        assert(scope.activeCount != 0);
        if (scope.activeCount == 0) return;
        --scope.activeCount;
    }
    // A genuine old-generation exit never changes the reused row's count or gate.
    auto* receiver = original.Receiver();
    assert(receiver->activeCount_ != 0);
    --receiver->activeCount_;
    --original.groupIdentity_->liveMembers;
    original = {};
    if (receiver->activeCount_ == 0) DetachGroupMemberReceiver(*receiver);
}

void TimelineArmTable::DetachGroupMemberReceiver(GroupMemberReceiver& receiver) const noexcept {
    assert(receiver.owner_ == this);
    if (receiver.previous_ != nullptr) receiver.previous_->next_ = receiver.next_;
    else groupMemberReceivers_ = receiver.next_;
    if (receiver.next_ != nullptr) receiver.next_->previous_ = receiver.previous_;
    receiver.lastDetachedEpisode_ = receiver.episode_;
    receiver.owner_ = nullptr;
    receiver.pool_ = nullptr;
    receiver.previous_ = nullptr;
    receiver.next_ = nullptr;
    receiver.activeCount_ = 0;
}

bool TimelineArmTable::CompleteOriginalGroupMemberCoverage(TimelineArmToken arm,
    const OriginalGroupMember* const* originals, std::size_t count) const noexcept {
    if (!arm.Valid() || arm.slot >= capacity_ || (count != 0 && originals == nullptr)) return false;
    const auto& scope = groupMemberScopes_[arm.slot];
    if (!SameOriginalGroupArm(scope.arm, arm) || scope.activeCount != count) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* original = originals[i];
        if (original == nullptr || original->serial_ == 0 || original->Receiver() == nullptr ||
            original->Receiver()->owner_ != this || original->Receiver()->pool_ == nullptr ||
            original->ArmTableIncarnation() != incarnation_ ||
            original->ReceiverEpisode() != original->Receiver()->episode_ ||
            !SameOriginalGroupArm(original->arm_, arm) || original->groupIdentity_ == nullptr ||
            original->groupIdentity_->liveMembers == 0 || !original->Group().Exact()) return false;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (originals[prior] == original || originals[prior]->serial_ == original->serial_) return false;
    }
    return true;
}

std::uint32_t TimelineArmTable::FreeCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].state.load(std::memory_order_acquire) ==
            ArmState::Free) {
            ++count;
        }
    }
    return count;
}

TimelineArmTable::Row* TimelineArmTable::Resolve(
    TimelineArmToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    if (!IdentityMatches(row, token)) {
        return nullptr;
    }
    const ArmState state = row.state.load(std::memory_order_acquire);
    if (state == ArmState::Free || state == ArmState::Tombstoned ||
        state == ArmState::Quarantined) {
        return nullptr;
    }
    return row.renderSlot == token.renderSlot &&
                   row.renderGeneration == token.renderGeneration
               ? &row
               : nullptr;
}

const TimelineArmTable::Row* TimelineArmTable::Resolve(
    TimelineArmToken token) const noexcept {
    return const_cast<TimelineArmTable*>(this)->Resolve(token);
}

bool TimelineArmTable::Reserve(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t startFrame, std::uint64_t armDeadline,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, startFrame,
                           armDeadline, programGeneration, target, {},
                           UINT16_MAX, false, false, false, outToken);
}

bool TimelineArmTable::PrepareAuthoredControl(const AuthoredControlInput& input,
    TimelineArmToken* outToken) noexcept {
    AuthoredControlMetadata control{};
    if (outToken == nullptr || outToken->Valid() ||
        !CaptureAuthoredControl(input, {}, &control)) return false;
    const auto dispatchFrame = control.eventCount == 2 ? std::min(control.identity.stopFrame,
        std::max(control.fadeStartFrame, control.onsetFrame)) : control.identity.stopFrame;
    return ReserveInternal(input.renderSlot, input.renderGeneration, dispatchFrame,
        input.armDeadline, input.program.programGeneration, input.target, {}, UINT16_MAX,
        false, true, false, outToken, &control);
}

bool TimelineArmTable::PreparePendingAuthoredControl(
    const PendingAuthoredControlInput& input, TimelineArmToken* outToken) noexcept {
    return PreparePendingAuthoredControlInternal(input, {}, outToken);
}

bool TimelineArmTable::PreparePendingAuthoredControlInternal(
    const PendingAuthoredControlInput& input, ParentAudioGroupRef selectedSourceGroup,
    TimelineArmToken* outToken) noexcept {
    const auto* view = input.program.program;
    const auto source = input.sourceStart;
    if (outToken == nullptr || outToken->Valid() || incarnation_ == 0 ||
        !source.Valid() || source.slot >= capacity_ || input.operation == nullptr ||
        view == nullptr || input.events == nullptr || input.eventCapacity == 0 ||
        input.eventCapacity > 2 || input.program.programGeneration == 0 ||
        view->generation != input.program.programGeneration || view->payloads == nullptr ||
        view->payloadCount > input.eventCapacity || view->role != TimelineProgramRole::Voice ||
        view->authoredStopFence != nullptr || !input.operation->Active(input.operation->Identity()) ||
        (input.fadeStartFrame < input.operation->Identity().stopFrame && input.eventCapacity < 2))
        return false;
    auto& row = rows_[source.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row.state.load(std::memory_order_acquire);
    const bool original = IdentityMatches(row, source) && row.renderSlot == source.renderSlot &&
        row.renderGeneration == source.renderGeneration && row.pendingImmediateStart && !row.startBound &&
        row.target == input.target && row.authoredControl.operation == nullptr &&
        row.parentGroup.decision.slot == selectedSourceGroup.decision.slot &&
        row.parentGroup.decision.generation == selectedSourceGroup.decision.generation &&
        row.memberIndex == (selectedSourceGroup.Valid() ? 0 : UINT16_MAX) &&
        !row.frontierOperation.Valid() && row.frontierAdmission == nullptr &&
        row.frontierAdmissionLease == nullptr &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row.startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        row.logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
        (state == ArmState::Prepared || state == ArmState::PendingApply || state == ArmState::Armed);
    bool prepared = false;
    if (original) {
        AuthoredControlMetadata control{};
        control.operation = input.operation;
        control.identity = input.operation->Identity();
        control.program = input.program;
        control.payloads = view->payloads;
        control.events = input.events;
        control.fadeStartFrame = input.fadeStartFrame;
        control.sourceStart = source;
        control.eventCapacity = input.eventCapacity;
        control.stage = AuthoredControlStage::Pending;
        control.selectedSourceGroup = selectedSourceGroup;
        prepared = ReserveInternal(source.renderSlot, source.renderGeneration, 0, 0,
            input.program.programGeneration, input.target, {}, UINT16_MAX, false, true,
            false, outToken, &control);
    }
    row.reservation.clear(std::memory_order_release);
    return prepared;
}

bool TimelineArmTable::ProvePendingAuthoredControlCommit(TimelineArmToken token,
    const PendingAuthoredControlInput& expected, PreparedCommitProof* out) noexcept {
    if (out == nullptr || out->Valid() || !token.Valid() || token.slot >= capacity_) return false;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto& control = row.authoredControl;
    const auto* view = expected.program.program;
    const bool valid = IdentityMatches(row, token) && row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration && row.state.load(std::memory_order_acquire) == ArmState::Prepared &&
        control.stage == AuthoredControlStage::Pending && control.operation != nullptr &&
        expected.operation == control.operation && expected.operation->Identity() == control.identity &&
        expected.operation->Active(control.identity) && view != nullptr &&
        view == control.program.program && expected.program.programGeneration == control.program.programGeneration &&
        view->generation == control.program.programGeneration && row.programGeneration == view->generation &&
        view->role == TimelineProgramRole::Voice && view->authoredStopFence == nullptr &&
        view->payloads == control.payloads && view->payloadCount <= control.eventCapacity &&
        expected.events == control.events && expected.eventCapacity == control.eventCapacity &&
        expected.fadeStartFrame == control.fadeStartFrame && SameArm(expected.sourceStart, control.sourceStart) &&
        expected.target == row.target && token.renderSlot == expected.sourceStart.renderSlot &&
        token.renderGeneration == expected.sourceStart.renderGeneration &&
        !row.pendingImmediateStart && !row.startBound && row.startFrame == 0 && row.armDeadline == 0 &&
        !row.parentGroup.Valid() && row.memberIndex == UINT16_MAX &&
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row.frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        row.armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        row.logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
        !row.frontierOperation.Valid() && row.frontierAdmission == nullptr &&
        row.frontierAdmissionLease == nullptr && row.frontierActiveLease.load(std::memory_order_acquire) == nullptr;
    if (!valid) {
        row.reservation.clear(std::memory_order_release);
        return false;
    }
    out->owner_ = this;
    out->row_ = &row;
    return true;
}

bool TimelineArmTable::ProvePendingAuthoredControlBinding(TimelineArmToken token,
    const TimelineArmReaderPin& sourceReader, const AuthoredControlInput& final,
    PendingAuthoredControlBindingProof* out) noexcept {
    return ProvePendingAuthoredControlBindingInternal(token, sourceReader, final, {}, out);
}

bool TimelineArmTable::ProvePendingAuthoredControlBindingInternal(TimelineArmToken token,
    const TimelineArmReaderPin& sourceReader, const AuthoredControlInput& final,
    ParentAudioGroupRef selectedSourceGroup, PendingAuthoredControlBindingProof* out) noexcept {
    if (out == nullptr || out->Valid() || incarnation_ == 0 || !token.Valid() || token.slot >= capacity_ ||
        sourceReader.owner_ != this || !sourceReader.token_.Valid() ||
        sourceReader.token_.slot >= capacity_ || sourceReader.token_.slot == token.slot) return false;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    auto& source = rows_[sourceReader.token_.slot];
    if (source.reservation.test_and_set(std::memory_order_acquire)) {
        row.reservation.clear(std::memory_order_release);
        return false;
    }
    const auto reject = [&]() {
        source.reservation.clear(std::memory_order_release);
        row.reservation.clear(std::memory_order_release);
        return false;
    };
    const auto& control = row.authoredControl;
    const auto sourceToken = control.sourceStart;
    const auto* view = final.program.program;
    if (!IdentityMatches(row, token) || row.renderSlot != token.renderSlot ||
        row.renderGeneration != token.renderGeneration || row.state.load(std::memory_order_acquire) != ArmState::Armed ||
        control.stage != AuthoredControlStage::Pending || control.operation == nullptr ||
        row.pendingImmediateStart || row.startBound || row.startFrame != 0 || row.armDeadline != 0 ||
        row.armedCompletionSequence.load(std::memory_order_acquire) == 0 ||
        row.frontierReaderPins.load(std::memory_order_acquire) != 0 ||
        row.frontierLastReadSequence.load(std::memory_order_acquire) != 0 ||
        row.startedBlockSequence.load(std::memory_order_acquire) != 0 ||
        row.logicalCompletionSequence.load(std::memory_order_acquire) != 0 ||
        row.tombstoneAudioSequence.load(std::memory_order_acquire) != 0 ||
        row.parentGroup.Valid() || row.memberIndex != UINT16_MAX || row.frontierOperation.Valid() ||
        row.frontierAdmission != nullptr || row.frontierAdmissionLease != nullptr ||
        row.frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
        !SameArm(sourceReader.token_, sourceToken) || !IdentityMatches(source, sourceToken) ||
        source.renderSlot != token.renderSlot || source.renderGeneration != token.renderGeneration ||
        source.state.load(std::memory_order_acquire) != ArmState::Armed || !source.pendingImmediateStart || !source.startBound ||
        source.authoredControl.operation != nullptr || source.target != row.target ||
        control.selectedSourceGroup.decision.slot != selectedSourceGroup.decision.slot ||
        control.selectedSourceGroup.decision.generation != selectedSourceGroup.decision.generation ||
        source.parentGroup.decision.slot != selectedSourceGroup.decision.slot ||
        source.parentGroup.decision.generation != selectedSourceGroup.decision.generation ||
        source.memberIndex != (selectedSourceGroup.Valid() ? 0 : UINT16_MAX) || source.frontierOperation.Valid() ||
        source.frontierAdmission != nullptr || source.frontierAdmissionLease != nullptr ||
        source.frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
        source.frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        source.frontierReaderPins.load(std::memory_order_acquire) == UINT32_MAX ||
        source.originalPlainControl.Valid() || row.pendingControlSourceReader.Valid() ||
        source.armedCompletionSequence.load(std::memory_order_acquire) == 0 ||
        source.startedBlockSequence.load(std::memory_order_acquire) != 0 ||
        source.logicalCompletionSequence.load(std::memory_order_acquire) != 0 ||
        source.tombstoneAudioSequence.load(std::memory_order_acquire) != 0 ||
        final.operation != control.operation || final.operation->Identity() != control.identity ||
        view == nullptr || view != control.program.program || view->payloads != control.payloads ||
        final.program.programGeneration != control.program.programGeneration || final.events != control.events ||
        final.fadeStartFrame != control.fadeStartFrame || final.target != row.target ||
        final.renderSlot != token.renderSlot || final.renderGeneration != token.renderGeneration ||
        final.onsetFrame != source.startFrame || final.eventCount > control.eventCapacity ||
        final.eventCount != (control.fadeStartFrame < control.identity.stopFrame &&
            source.startFrame < control.identity.stopFrame ? 2u : 1u)) return reject();
    AuthoredControlMetadata captured{};
    if (!CaptureAuthoredControl(final, token, &captured) ||
        !control.operation->TryAcquireApply(control.identity)) return reject();
    captured.sourceStart = sourceToken;
    captured.eventCapacity = control.eventCapacity;
    captured.selectedSourceGroup = control.selectedSourceGroup;
    out->owner_ = this;
    out->row_ = &row;
    out->source_ = &source;
    out->operation_ = control.operation;
    out->final_ = captured;
    out->deadline_ = final.armDeadline;
    return true;
}

TimelineArmTable::PendingAuthoredControlBindingProof::PendingAuthoredControlBindingProof(
    PendingAuthoredControlBindingProof&& other) noexcept { *this = std::move(other); }

TimelineArmTable::PendingAuthoredControlBindingProof&
TimelineArmTable::PendingAuthoredControlBindingProof::operator=(PendingAuthoredControlBindingProof&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        row_ = std::exchange(other.row_, nullptr);
        source_ = std::exchange(other.source_, nullptr);
        operation_ = std::exchange(other.operation_, nullptr);
        final_ = other.final_;
        deadline_ = other.deadline_;
    }
    return *this;
}

TimelineArmTable::PendingAuthoredControlBindingProof::~PendingAuthoredControlBindingProof() { Release(); }

void TimelineArmTable::PendingAuthoredControlBindingProof::Release() noexcept {
    if (owner_ == nullptr) return;
    operation_->ReleaseApply();
    source_->reservation.clear(std::memory_order_release);
    row_->reservation.clear(std::memory_order_release);
    owner_ = nullptr;
    row_ = nullptr;
    source_ = nullptr;
    operation_ = nullptr;
}

void TimelineArmTable::ConsumePendingAuthoredControlBinding(PendingAuthoredControlBindingProof&& proof) noexcept {
    assert(proof.Valid());
    auto& row = *proof.row_;
    assert(row.authoredControl.stage == AuthoredControlStage::Pending && !row.startBound);
    row.startFrame = proof.final_.eventCount == 2 ? std::min(proof.final_.identity.stopFrame,
        std::max(proof.final_.fadeStartFrame, proof.final_.onsetFrame)) : proof.final_.identity.stopFrame;
    row.armDeadline = proof.deadline_;
    row.authoredControl = proof.final_;
    row.startBound = true;
    auto& source = *proof.source_;
    (void)source.frontierReaderPins.fetch_add(1u, std::memory_order_release);
    row.pendingControlSourceReader.owner_ = proof.owner_;
    row.pendingControlSourceReader.token_ = proof.final_.sourceStart;
    source.originalPlainControl = {static_cast<std::uint32_t>(row.identity.load()),
        row.generation, row.renderSlot, row.renderGeneration};
    source.originalPlainControlIdentity = proof.final_.identity;
    proof.Release();
}

bool TimelineArmTable::ProveAuthoredControlBucket(TimelineArmToken token,
    const TimelineArmReaderPin& reader, const TimelineTransactionProgramRef& program,
    std::uint64_t quantumBegin, std::uint32_t blockFrames, std::uint64_t audioSequence,
    const ScheduledEventInput* dueEvents, std::uint32_t dueCount,
    AuthoredControlBucketProof* out) noexcept {
    if (out == nullptr || out->Valid() || !token.Valid() || token.slot >= capacity_ ||
        reader.owner_ != this || !SameArm(reader.token_, token) ||
        blockFrames == 0 || quantumBegin > UINT64_MAX - blockFrames ||
        quantumBegin % blockFrames != 0 || audioSequence != quantumBegin / blockFrames + 1u ||
        dueEvents == nullptr || dueCount == 0 || dueCount > 2) return false;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto reject = [&]() {
        row.reservation.clear(std::memory_order_release);
        return false;
    };
    const auto& control = row.authoredControl;
    const auto* view = program.program;
    if (!IdentityMatches(row, token) || row.renderSlot != token.renderSlot ||
        row.renderGeneration != token.renderGeneration || control.operation == nullptr ||
        control.stage != AuthoredControlStage::Bound ||
        row.state.load(std::memory_order_acquire) != ArmState::Published ||
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        row.frontierLastReadSequence.load(std::memory_order_acquire) >= audioSequence ||
        row.startedBlockSequence.load(std::memory_order_acquire) != 0 ||
        row.logicalCompletionSequence.load(std::memory_order_acquire) != 0 ||
        row.tombstoneAudioSequence.load(std::memory_order_acquire) != 0 ||
        row.parentGroup.Valid() || row.memberIndex != UINT16_MAX ||
        row.frontierAdmission != nullptr || row.frontierAdmissionLease != nullptr ||
        row.frontierOperation.Valid() || row.pendingImmediateStart || !row.startBound ||
        view == nullptr || view != control.program.program ||
        program.programGeneration != control.program.programGeneration ||
        row.programGeneration != program.programGeneration ||
        view->generation != program.programGeneration || view->role != TimelineProgramRole::Voice ||
        view->authoredStopFence != nullptr || view->payloads != control.payloads ||
        view->payloadCount != control.eventCount || control.events == nullptr ||
        control.eventCount == 0 || control.eventCount > 2 ||
        !control.operation->Active(control.identity)) return reject();
    std::uint32_t expectedCount = 0;
    std::uint32_t matched = 0;
    bool containsStop = false;
    for (std::uint32_t index = 0; index < control.eventCount; ++index) {
        const auto& frozen = control.eventsSnapshot[index];
        const bool envelope = control.eventCount == 2 && index == 0;
        if (!ControlPayloadMatches(view->payloads[index], control, row.target, envelope) ||
            !SameControlEvent(control.events[index], frozen) ||
            !SameArm(control.events[index].armToken, token)) return reject();
        if (frozen.executeFrame < quantumBegin || frozen.executeFrame >= quantumBegin + blockFrames)
            continue;
        ++expectedCount;
        bool found = false;
        for (std::uint32_t due = 0; due < dueCount; ++due) {
            if (!SameControlEvent(dueEvents[due], frozen) ||
                !SameArm(dueEvents[due].armToken, token)) continue;
            if (found || (matched & (1u << due)) != 0) return reject();
            found = true;
            matched |= 1u << due;
        }
        if (!found) return reject();
        containsStop |= !envelope;
    }
    if (control.sourceStart.Valid()) {
        const auto* source = PinnedAuthoredControlSource(row);
        if (source == nullptr || !SameArm(row.pendingControlSourceReader.token_, control.sourceStart) ||
            !SameArm(source->originalPlainControl, token) ||
            source->originalPlainControlIdentity != control.identity ||
            source->target != row.target || source->startFrame != control.onsetFrame ||
            !source->pendingImmediateStart || !source->startBound) return reject();
    }
    if (expectedCount != dueCount || matched != (1u << dueCount) - 1u ||
        !control.operation->TryAcquireApply(control.identity)) return reject();
    out->owner_ = this;
    out->row_ = &row;
    out->operation_ = control.operation;
    out->audioSequence_ = audioSequence;
    out->containsStop_ = containsStop;
    out->applied_ = false;
    return true;
}

TimelineArmTable::AuthoredControlBucketProof::AuthoredControlBucketProof(
    AuthoredControlBucketProof&& other) noexcept {
    *this = std::move(other);
}

TimelineArmTable::AuthoredControlBucketProof&
TimelineArmTable::AuthoredControlBucketProof::operator=(AuthoredControlBucketProof&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        row_ = std::exchange(other.row_, nullptr);
        operation_ = std::exchange(other.operation_, nullptr);
        audioSequence_ = other.audioSequence_;
        containsStop_ = other.containsStop_;
        applied_ = other.applied_;
    }
    return *this;
}

TimelineArmTable::AuthoredControlBucketProof::~AuthoredControlBucketProof() { Release(); }

void TimelineArmTable::AuthoredControlBucketProof::Release() noexcept {
    if (owner_ != nullptr) TimelineArmTable::ReleaseAuthoredControlProof(*this);
}

void TimelineArmTable::ReleaseAuthoredControlProof(AuthoredControlBucketProof& proof) noexcept {
    assert(proof.Valid() && proof.row_ != nullptr && proof.operation_ != nullptr);
    proof.operation_->ReleaseApply();
    proof.row_->reservation.clear(std::memory_order_release);
    proof.owner_ = nullptr;
    proof.row_ = nullptr;
    proof.operation_ = nullptr;
}

void TimelineArmTable::ApplyAuthoredControlBucket(AuthoredControlBucketProof& proof) noexcept {
    assert(proof.Valid() && !proof.applied_);
    auto& row = *proof.row_;
    row.frontierLastReadSequence.store(proof.audioSequence_, std::memory_order_release);
    if (proof.containsStop_) {
        if (row.pendingControlSourceReader.Valid()) {
            auto& source = proof.owner_->rows_[row.pendingControlSourceReader.token_.slot];
            assert(proof.owner_->PinnedAuthoredControlSource(row) == &source);
            source.appliedPlainStopSource = row.pendingControlSourceReader.token_;
            source.appliedPlainStop = row.authoredControl.identity;
            auto read = source.frontierLastReadSequence.load(std::memory_order_relaxed);
            while (read < proof.audioSequence_ && !source.frontierLastReadSequence.compare_exchange_weak(
                read, proof.audioSequence_, std::memory_order_release, std::memory_order_relaxed)) {}
            source.appliedPlainStopSequence.store(proof.audioSequence_, std::memory_order_release);
        }
        row.logicalCompletionSequence.store(proof.audioSequence_, std::memory_order_relaxed);
        row.tombstoneAudioSequence.store(proof.audioSequence_, std::memory_order_relaxed);
        row.state.store(ArmState::Tombstoned, std::memory_order_release);
    }
    proof.applied_ = true;
}

bool TimelineArmTable::Prepare(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t startFrame, std::uint64_t armDeadline,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, startFrame,
                           armDeadline, programGeneration, target, {},
                           UINT16_MAX, false, true, false, outToken);
}

bool TimelineArmTable::ReserveGrouped(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t startFrame, std::uint64_t armDeadline,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    ParentAudioGroupRef parentGroup, std::uint16_t memberIndex,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, startFrame,
                           armDeadline, programGeneration, target, parentGroup,
                           memberIndex, true, false, false, outToken);
}

bool TimelineArmTable::PrepareGrouped(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t startFrame, std::uint64_t armDeadline,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    ParentAudioGroupRef parentGroup, std::uint16_t memberIndex,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, startFrame,
                           armDeadline, programGeneration, target, parentGroup,
                           memberIndex, true, true, false, outToken);
}

bool TimelineArmTable::ReservePendingImmediate(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, 0, 0,
                           programGeneration, target, {}, UINT16_MAX,
                           false, false, true, outToken);
}

bool TimelineArmTable::PreparePendingImmediate(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, 0, 0,
                           programGeneration, target, {}, UINT16_MAX,
                           false, true, true, outToken);
}

bool TimelineArmTable::ReservePendingImmediateGrouped(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    ParentAudioGroupRef parentGroup, std::uint16_t memberIndex,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, 0, 0,
                           programGeneration, target, parentGroup, memberIndex,
                           true, false, true, outToken);
}

bool TimelineArmTable::PreparePendingImmediateGrouped(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    ParentAudioGroupRef parentGroup, std::uint16_t memberIndex,
    TimelineArmToken* outToken) noexcept {
    return ReserveInternal(renderSlot, renderGeneration, 0, 0,
                           programGeneration, target, parentGroup, memberIndex,
                           true, true, true, outToken);
}

bool TimelineArmTable::ReserveInternal(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t startFrame, std::uint64_t armDeadline,
    ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
    ParentAudioGroupRef parentGroup, std::uint16_t memberIndex, bool grouped,
    bool prepared, bool pendingImmediateStart,
    TimelineArmToken* outToken, const AuthoredControlMetadata* control) noexcept {
    if (outToken == nullptr || renderSlot == UINT32_MAX ||
        renderGeneration == 0 ||
        (grouped && (!parentGroup.Valid() || memberIndex == UINT16_MAX)) ||
        (!grouped && (parentGroup.Valid() || memberIndex != UINT16_MAX))) {
        return false;
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.state.load(std::memory_order_acquire) != ArmState::Free ||
            row.reservation.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        ArmState expected = ArmState::Free;
        if (row.originalLifetimePinned.load(std::memory_order_acquire)) {
            row.reservation.clear(std::memory_order_release);
            continue;
        }
        row.originalLifetimeIssued = false;
        assert(!row.pendingControlSourceReader.Valid());
        row.originalPlainControl = {};
        row.originalPlainControlIdentity = {};
        row.appliedPlainStopSource = {};
        row.appliedPlainStop = {};
        row.appliedPlainStopSequence.store(0, std::memory_order_relaxed);
        row.renderSlot = renderSlot;
        row.renderGeneration = renderGeneration;
        row.startFrame = startFrame;
        row.armDeadline = armDeadline;
        row.pendingImmediateStart = pendingImmediateStart;
        row.startBound = !pendingImmediateStart &&
            (control == nullptr || control->stage != AuthoredControlStage::Pending);
        row.programGeneration = programGeneration;
        row.target = target;
        row.parentGroup = parentGroup;
        row.memberIndex = memberIndex;
        row.authoredControl = control == nullptr ? AuthoredControlMetadata{} : *control;
        row.tombstoneAudioSequence.store(0, std::memory_order_relaxed);
        row.armedCompletionSequence.store(0, std::memory_order_relaxed);
        row.startedBlockSequence.store(0, std::memory_order_relaxed);
        row.logicalCompletionSequence.store(0, std::memory_order_relaxed);
        row.frontierReaderPins.store(0, std::memory_order_relaxed);
        row.frontierLastReadSequence.store(0, std::memory_order_relaxed);
        row.frontierOperation = {};
        row.frontierAdmission = nullptr;
        row.originalSourceSubmission = nullptr;
        row.originalSourceEpisode = 0;
        row.frontierAdmissionLease = nullptr;
        row.frontierActiveLease.store(nullptr, std::memory_order_relaxed);
        row.frontierAdmissionFinished.store(true, std::memory_order_relaxed);
        row.nativePrivateWithdrawn.store(false, std::memory_order_relaxed);
        row.identity.store(PackIdentity(slot, row.generation),
                           std::memory_order_release);
        if (row.state.compare_exchange_strong(
                expected,
                prepared ? ArmState::Prepared : ArmState::PendingApply,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            auto& sourceFactory = groupMemberScopes_[slot];
            sourceFactory.sourceFactoryArm = {slot, row.generation, renderSlot, renderGeneration};
            sourceFactory.unparentedPreparedSourceFactory = prepared && !grouped && control == nullptr;
            row.reservation.clear(std::memory_order_release);
            *outToken = {slot, row.generation, renderSlot, renderGeneration};
            return true;
        }
        row.reservation.clear(std::memory_order_release);
    }
    return false;
}

bool TimelineArmTable::ProvePreparedCommit(
    const PreparedCommitInput& expected, PreparedCommitProof* out) noexcept {
    const auto token = expected.arm;
    if (out == nullptr || out->Valid() || !token.Valid() ||
        token.slot >= capacity_ || expected.armDeadline > expected.startFrame ||
        (expected.parentGroup.Valid() != (expected.memberIndex != UINT16_MAX)) ||
        (expected.pendingImmediateStart &&
         (expected.startFrame != 0 || expected.armDeadline != 0))) return false;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const bool valid = IdentityMatches(row, token) &&
        row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration &&
        row.state.load(std::memory_order_acquire) == ArmState::Prepared &&
        row.authoredControl.stage != AuthoredControlStage::Pending &&
        row.startFrame == expected.startFrame &&
        row.armDeadline == expected.armDeadline &&
        row.pendingImmediateStart == expected.pendingImmediateStart &&
        row.startBound == !expected.pendingImmediateStart &&
        row.programGeneration == expected.programGeneration &&
        row.target == expected.target &&
        row.parentGroup.decision.slot == expected.parentGroup.decision.slot &&
        row.parentGroup.decision.generation == expected.parentGroup.decision.generation &&
        row.memberIndex == expected.memberIndex &&
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row.frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        row.armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        row.logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
        !row.frontierOperation.Valid() && row.frontierAdmission == nullptr &&
        row.frontierAdmissionLease == nullptr &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row.frontierAdmissionFinished.load(std::memory_order_acquire);
    if (!valid) {
        row.reservation.clear(std::memory_order_release);
        return false;
    }
    out->owner_ = this;
    out->row_ = &row;
    return true;
}

void TimelineArmTable::ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept {
    assert(proof.Valid());
    assert(proof.row_->state.load(std::memory_order_relaxed) == ArmState::Prepared);
    proof.row_->state.store(ArmState::PendingApply, std::memory_order_release);
    proof.Release();
}

bool TimelineArmTable::CommitPrepared(TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || !IdentityMatches(*row, token) ||
        row->renderSlot != token.renderSlot || row->renderGeneration != token.renderGeneration ||
        row->authoredControl.stage == AuthoredControlStage::Pending) return false;
    ArmState expected = ArmState::Prepared;
    return row->state.compare_exchange_strong(
        expected, ArmState::PendingApply, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

TimelineArmTable::OriginalArmRowOwner::OriginalArmRowOwner(
    OriginalArmRowOwner&& other) noexcept { *this = std::move(other); }

TimelineArmTable::OriginalArmRowOwner& TimelineArmTable::OriginalArmRowOwner::operator=(
    OriginalArmRowOwner&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        row_ = std::exchange(other.row_, nullptr);
        token_ = std::exchange(other.token_, {});
    }
    return *this;
}

void TimelineArmTable::OriginalArmRowOwner::Release() noexcept {
    if (owner_ == nullptr) return;
    auto* table = std::exchange(owner_, nullptr);
    auto* row = std::exchange(row_, nullptr);
    token_ = {};
    // Reclaim may immediately reuse the row after this last Row access.
    row->originalLifetimePinned.store(false, std::memory_order_release);
    const auto previous = table->liveOriginalArmRowOwners_.fetch_sub(1,
        std::memory_order_relaxed);
    assert(previous != 0);
    (void)previous;
}

bool TimelineArmTable::IssueOriginalArmRowOwner(const PreparedCommitProof& preparation,
    OriginalArmRowOwner* out) noexcept {
    if (out == nullptr || out->Valid() || incarnation_ == 0 ||
        preparation.owner_ != this || !preparation.Valid()) return false;
    auto& row = *preparation.row_;
    if (row.state.load(std::memory_order_acquire) != ArmState::Prepared ||
        row.authoredControl.operation != nullptr ||
        row.authoredControl.stage != AuthoredControlStage::None ||
        row.originalLifetimeIssued ||
        row.originalLifetimePinned.load(std::memory_order_acquire)) return false;
    const auto slot = static_cast<std::uint32_t>(&row - rows_.get());
    assert(slot < capacity_ && row.generation != 0);
    row.originalLifetimeIssued = true;
    row.originalLifetimePinned.store(true, std::memory_order_release);
    liveOriginalArmRowOwners_.fetch_add(1, std::memory_order_relaxed);
    out->row_ = &row;
    out->token_ = {slot, row.generation, row.renderSlot, row.renderGeneration};
    out->owner_ = this;
    return true;
}

bool TimelineArmTable::IssueOriginalControlHeaderRowOwner(
    const PreparedCommitProof& preparation, OriginalControlHeaderRowOwner* out) noexcept {
    if (out == nullptr || out->Valid() || incarnation_ == 0 ||
        preparation.owner_ != this || !preparation.Valid()) return false;
    auto& row = *preparation.row_;
    if (row.state.load(std::memory_order_acquire) != ArmState::Prepared ||
        row.authoredControl.stage != AuthoredControlStage::Pending ||
        row.authoredControl.operation == nullptr || row.pendingImmediateStart ||
        row.startBound || row.originalLifetimeIssued ||
        row.originalLifetimePinned.load(std::memory_order_acquire)) return false;
    const auto slot = static_cast<std::uint32_t>(&row - rows_.get());
    assert(slot < capacity_ && row.generation != 0);
    row.originalLifetimeIssued = true;
    row.originalLifetimePinned.store(true, std::memory_order_release);
    liveOriginalArmRowOwners_.fetch_add(1, std::memory_order_relaxed);
    out->lifetime_.row_ = &row;
    out->lifetime_.token_ = {slot, row.generation, row.renderSlot, row.renderGeneration};
    out->scope_.incarnation = incarnation_;
    out->scope_.sourceStart = row.authoredControl.sourceStart;
    out->scope_.operation = row.authoredControl.operation;
    out->scope_.operationIdentity = row.authoredControl.identity;
    out->scope_.program = row.authoredControl.program;
    out->scope_.target = row.target;
    out->lifetime_.owner_ = this;
    return true;
}

bool TimelineArmTable::CancelPrepared(TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || !IdentityMatches(*row, token) || row->renderSlot != token.renderSlot ||
        row->renderGeneration != token.renderGeneration ||
        row->frontierReaderPins.load(std::memory_order_acquire) != 0 ||
        row->originalLifetimePinned.load(std::memory_order_acquire)) {
        return false;
    }
    ArmState expected = ArmState::Prepared;
    if (!row->state.compare_exchange_strong(
            expected, token.generation == UINT32_MAX ? ArmState::Quarantined : ArmState::Free,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    row->identity.store(0, std::memory_order_release);
    if (token.generation != UINT32_MAX) row->generation = token.generation + 1u;
    row->renderSlot = UINT32_MAX;
    row->renderGeneration = 0;
    row->startFrame = 0;
    row->armDeadline = 0;
    row->pendingImmediateStart = false;
    row->startBound = true;
    row->programGeneration = 0;
    row->target = {};
    row->parentGroup = {};
    row->memberIndex = UINT16_MAX;
    row->authoredControl = {};
    return true;
}

bool TimelineArmTable::RetirePendingImmediateBatchExact(
    const PendingImmediateRetirementInput* inputs, std::size_t inputCount,
    Row** reservedRows, std::size_t reservedCapacity) noexcept {
    if (inputs == nullptr || inputCount == 0 || inputCount > capacity_ ||
        reservedRows == nullptr || reservedCapacity < inputCount) return false;
    std::size_t reserved = 0;
    for (; reserved < inputCount; ++reserved) {
        const auto& input = inputs[reserved];
        if (!input.arm.Valid() || input.arm.slot >= capacity_ ||
            !input.parentGroup.Valid() || input.memberIndex == UINT16_MAX ||
            input.binding == nullptr || input.lease == nullptr ||
            !input.binding->Valid() ||
            input.binding->role != FrontierGroupRole::EntryMember ||
            input.binding->operation.group.decision.slot !=
                input.parentGroup.decision.slot ||
            input.binding->operation.group.decision.generation !=
                input.parentGroup.decision.generation) break;
        bool duplicate = false;
        for (std::size_t prior = 0; prior < reserved; ++prior)
            duplicate = duplicate || inputs[prior].arm.slot == input.arm.slot;
        if (duplicate) break;
        Row* row = Resolve(input.arm);
        if (row == nullptr ||
            row->reservation.test_and_set(std::memory_order_acquire)) break;
        const auto lease = input.lease->Capture();
        const bool valid = IdentityMatches(*row, input.arm) &&
            row->renderSlot == input.arm.renderSlot &&
            row->renderGeneration == input.arm.renderGeneration &&
            row->state.load(std::memory_order_acquire) == ArmState::PendingApply &&
            !row->originalLifetimePinned.load(std::memory_order_acquire) &&
            !row->pendingImmediateStart && row->startBound &&
            row->startFrame == input.startFrame &&
            row->armDeadline == input.startFrame &&
            row->parentGroup.decision.slot == input.parentGroup.decision.slot &&
            row->parentGroup.decision.generation ==
                input.parentGroup.decision.generation &&
            row->memberIndex == input.memberIndex &&
            row->frontierAdmission == input.binding &&
            row->frontierAdmissionLease == input.lease &&
            row->frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
            row->frontierReaderPins.load(std::memory_order_acquire) == 0 &&
            row->frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
            !row->frontierOperation.Valid() &&
            row->armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
            row->startedBlockSequence.load(std::memory_order_acquire) == 0 &&
            row->logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
            row->tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
            lease.state == AudioSliceState::SliceInvalidated &&
            (lease.pendingReasonBits & AudioSliceReasonCancel) != 0;
        if (!valid) {
            row->reservation.clear(std::memory_order_release);
            break;
        }
        reservedRows[reserved] = row;
    }
    if (reserved != inputCount) {
        for (std::size_t index = 0; index < reserved; ++index) {
            reservedRows[index]->reservation.clear(std::memory_order_release);
            reservedRows[index] = nullptr;
        }
        return false;
    }
    for (std::size_t index = 0; index < inputCount; ++index) {
        const auto token = inputs[index].arm;
        Row& row = *reservedRows[index];
        row.frontierAdmission = nullptr;
        row.frontierAdmissionLease = nullptr;
        row.frontierAdmissionFinished.store(true, std::memory_order_release);
        row.identity.store(0, std::memory_order_release);
        row.state.store(token.generation == UINT32_MAX
                            ? ArmState::Quarantined : ArmState::Free,
                        std::memory_order_release);
        if (token.generation != UINT32_MAX) row.generation = token.generation + 1u;
        row.renderSlot = UINT32_MAX;
        row.renderGeneration = 0;
        row.startFrame = 0;
        row.armDeadline = 0;
        row.pendingImmediateStart = false;
        row.startBound = true;
        row.programGeneration = 0;
        row.target = {};
        row.parentGroup = {};
        row.memberIndex = UINT16_MAX;
        row.authoredControl = {};
        row.reservation.clear(std::memory_order_release);
        reservedRows[index] = nullptr;
    }
    return true;
}

bool TimelineArmTable::BeginUnpublishedPendingRetirement(
    const PendingImmediateRetirementInput* inputs, std::size_t inputCount,
    Row** reservedRows, std::size_t reservedCapacity,
    UnpublishedPendingRetirementProof* out) noexcept {
    if (inputs == nullptr || inputCount == 0 || inputCount > capacity_ ||
        reservedRows == nullptr || reservedCapacity < inputCount ||
        out == nullptr || out->Valid()) return false;
    std::size_t reserved = 0;
    for (; reserved < inputCount; ++reserved) {
        const auto& input = inputs[reserved];
        if (!input.arm.Valid() || !input.parentGroup.Valid() ||
            input.memberIndex == UINT16_MAX || input.binding == nullptr ||
            input.lease == nullptr || !input.binding->Valid() ||
            input.binding->role != FrontierGroupRole::EntryMember ||
            input.binding->operation.group.decision.slot != input.parentGroup.decision.slot ||
            input.binding->operation.group.decision.generation != input.parentGroup.decision.generation)
            break;
        bool duplicate = false;
        for (std::size_t prior = 0; prior < reserved; ++prior)
            duplicate = duplicate || inputs[prior].arm.slot == input.arm.slot;
        if (duplicate) break;
        Row* row = Resolve(input.arm);
        if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire)) break;
        const bool ownsPin = input.ownedReader != nullptr && input.ownedReader->Valid();
        const bool exactPin = !ownsPin ||
            (input.ownedReader->owner_ == this &&
             input.ownedReader->token_.slot == input.arm.slot &&
             input.ownedReader->token_.generation == input.arm.generation &&
             input.ownedReader->token_.renderSlot == input.arm.renderSlot &&
             input.ownedReader->token_.renderGeneration == input.arm.renderGeneration);
        const bool valid = IdentityMatches(*row, input.arm) && exactPin &&
            row->renderSlot == input.arm.renderSlot && row->renderGeneration == input.arm.renderGeneration &&
            row->state.load(std::memory_order_acquire) == ArmState::PendingApply &&
            !row->originalLifetimePinned.load(std::memory_order_acquire) &&
            !row->pendingImmediateStart && row->startBound &&
            // Scheduled starts may fall inside a block; their arm deadline is
            // the preceding block boundary, as required by prepared commit.
            row->startFrame == input.startFrame && row->armDeadline <= input.startFrame &&
            row->parentGroup.decision.slot == input.parentGroup.decision.slot &&
            row->parentGroup.decision.generation == input.parentGroup.decision.generation &&
            row->memberIndex == input.memberIndex &&
            row->frontierAdmission == input.binding && row->frontierAdmissionLease == input.lease &&
            row->frontierReaderPins.load(std::memory_order_acquire) == (ownsPin ? 1u : 0u) &&
            row->frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
            row->frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
            !row->frontierOperation.Valid() &&
            row->armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
            row->startedBlockSequence.load(std::memory_order_acquire) == 0 &&
            row->logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
            row->tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
            input.lease->Capture().state == AudioSliceState::Ready;
        if (!valid) {
            row->reservation.clear(std::memory_order_release);
            break;
        }
        reservedRows[reserved] = row;
    }
    if (reserved != inputCount) {
        for (std::size_t index = 0; index < reserved; ++index) {
            reservedRows[index]->reservation.clear(std::memory_order_release);
            reservedRows[index] = nullptr;
        }
        return false;
    }
    out->owner_ = this;
    out->inputs_ = inputs;
    out->rows_ = reservedRows;
    out->count_ = inputCount;
    out->nextLease_ = 0;
    return true;
}

bool TimelineArmTable::AbortUnpublishedPendingRetirement(
    UnpublishedPendingRetirementProof* proof) noexcept {
    if (proof == nullptr || proof->owner_ != this || proof->nextLease_ != 0) return false;
    for (std::size_t index = 0; index < proof->count_; ++index)
        if (proof->inputs_[index].lease->Capture().state != AudioSliceState::Ready)
            return false;
    for (std::size_t index = 0; index < proof->count_; ++index) {
        proof->rows_[index]->reservation.clear(std::memory_order_release);
        proof->rows_[index] = nullptr;
    }
    proof->owner_ = nullptr;
    proof->inputs_ = nullptr;
    proof->rows_ = nullptr;
    proof->count_ = 0;
    return true;
}

bool TimelineArmTable::FinishUnpublishedPendingRetirement(
    UnpublishedPendingRetirementProof* proof) noexcept {
    if (proof == nullptr || proof->owner_ != this) return false;
    for (std::size_t index = 0; index < proof->count_; ++index)
        if (proof->rows_[index] == nullptr ||
            proof->rows_[index]->originalLifetimePinned.load(std::memory_order_acquire))
            return false;
    // Inputs and held rows stay private and immutable for this proof's lifetime.
    for (; proof->nextLease_ < proof->count_; ++proof->nextLease_) {
        const auto index = proof->nextLease_;
        auto* lease = proof->inputs_[index].lease;
        bool duplicate = false;
        for (std::size_t prior = 0; prior < index; ++prior)
            duplicate = duplicate || proof->inputs_[prior].lease == lease;
        if (!duplicate && lease->Invalidate(AudioSliceInvalidateReason::Cancel) !=
                AudioSliceInvalidateDisposition::InvalidatedReady) return false;
    }
    for (std::size_t index = 0; index < proof->count_; ++index) {
        const auto& input = proof->inputs_[index];
        Row& row = *proof->rows_[index];
        if (input.ownedReader != nullptr && input.ownedReader->Valid()) {
            input.ownedReader->owner_ = nullptr;
            input.ownedReader->token_ = {};
        }
        row.frontierReaderPins.store(0, std::memory_order_release);
        row.frontierAdmission = nullptr;
        row.frontierAdmissionLease = nullptr;
        row.frontierAdmissionFinished.store(true, std::memory_order_release);
        row.identity.store(0, std::memory_order_release);
        row.state.store(input.arm.generation == UINT32_MAX
                            ? ArmState::Quarantined : ArmState::Free,
                        std::memory_order_release);
        if (input.arm.generation != UINT32_MAX) row.generation = input.arm.generation + 1u;
        row.renderSlot = UINT32_MAX;
        row.renderGeneration = 0;
        row.startFrame = 0;
        row.armDeadline = 0;
        row.pendingImmediateStart = false;
        row.startBound = true;
        row.programGeneration = 0;
        row.target = {};
        row.parentGroup = {};
        row.memberIndex = UINT16_MAX;
        row.authoredControl = {};
        row.reservation.clear(std::memory_order_release);
        proof->rows_[index] = nullptr;
    }
    proof->owner_ = nullptr;
    proof->inputs_ = nullptr;
    proof->rows_ = nullptr;
    proof->count_ = 0;
    proof->nextLease_ = 0;
    return true;
}

namespace {

bool Transition(TimelineArmTable::Row* row, ArmState from,
                ArmState to) noexcept {
    if (row == nullptr) {
        return false;
    }
    ArmState expected = from;
    return row->state.compare_exchange_strong(
        expected, to, std::memory_order_acq_rel, std::memory_order_acquire);
}

} // namespace

TimelineArmTable::ControlCommandAckStatus TimelineArmTable::AcknowledgeControlCommand(
    TimelineArmToken token, std::uint64_t originalCompletionSequence,
    std::uint64_t currentAudioSequence) noexcept {
    using Status = ControlCommandAckStatus;
    if (!token.Valid() || token.slot >= capacity_ || originalCompletionSequence == 0 ||
        currentAudioSequence < originalCompletionSequence) return Status::Invalid;
    auto& row = rows_[token.slot];
    ArmMutationAccess access(&row);
    if (!access) return Status::DeferredReservation;
    const auto state = row.state.load(std::memory_order_acquire);
    if (row.generation < token.generation) return Status::Invalid;
    if (row.generation > token.generation ||
        (state == ArmState::Quarantined && token.generation == UINT32_MAX &&
            row.identity.load(std::memory_order_acquire) == 0)) return Status::Stale;
    if (!IdentityMatches(row, token) || row.renderSlot != token.renderSlot ||
        row.renderGeneration != token.renderGeneration) return Status::Invalid;
    if (state == ArmState::Tombstoned) return Status::AlreadySettled;
    if (state == ArmState::Armed)
        return row.armedCompletionSequence.load(std::memory_order_acquire) ==
            originalCompletionSequence ? Status::AlreadySettled : Status::Invalid;
    if (state == ArmState::PendingApply) {
        row.armedCompletionSequence.store(originalCompletionSequence, std::memory_order_relaxed);
        row.state.store(ArmState::Armed, std::memory_order_release);
        return Status::Applied;
    }
    if (state == ArmState::CancelRequested) {
        if (FrontierAdmissionDefers(&row, true)) return Status::DeferredReservation;
        return TombstoneReservedRow(&row, currentAudioSequence) ?
            Status::Cancelled : Status::Invalid;
    }
    return Status::Invalid;
}

void TimelineArmTable::SourceCommandRowProof::Release() noexcept {
    if (row_ == nullptr) return;
    row_->reservation.clear(std::memory_order_release);
    owner_->liveSourceCommandProofs_.fetch_sub(1, std::memory_order_relaxed);
    owner_ = nullptr;
    row_ = nullptr;
    token_ = {};
    completion_ = 0;
    cancellation_ = false;
}

TimelineArmTable::SourceCommandRowStatus TimelineArmTable::ProvePendingSourceCommand(
    TimelineArmToken token, SourceCommandRowProof* out, const void* originalSubmission) noexcept {
    using Status = SourceCommandRowStatus;
    if (out == nullptr || out->Valid() || incarnation_ == 0 ||
        !token.Valid() || token.slot >= capacity_) return Status::Invalid;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire))
        return Status::DeferredReservation;
    if (IdentityMatches(row, token) && row.originalSourceSubmission != nullptr &&
        row.originalSourceSubmission != originalSubmission) {
        row.reservation.clear(std::memory_order_release);
        return Status::RequiresOriginalSubmission;
    }
    if (!IdentityMatches(row, token) || row.renderSlot != token.renderSlot ||
        row.renderGeneration != token.renderGeneration ||
        row.state.load(std::memory_order_acquire) != ArmState::PendingApply ||
        row.authoredControl.operation != nullptr ||
        row.originalSourceSubmission != originalSubmission) {
        row.reservation.clear(std::memory_order_release);
        return Status::NotPendingSource;
    }
    out->owner_ = this;
    out->row_ = &row;
    out->token_ = token;
    out->completion_ = 0;
    out->cancellation_ = false;
    liveSourceCommandProofs_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ready;
}

void TimelineArmTable::AcknowledgeSourceCommand(SourceCommandRowProof& proof,
    std::uint64_t completionSequence) noexcept {
    assert(proof.Valid() && completionSequence != 0 && proof.completion_ == 0 &&
        !proof.cancellation_ &&
        proof.row_->state.load(std::memory_order_acquire) == ArmState::PendingApply);
    proof.row_->armedCompletionSequence.store(completionSequence, std::memory_order_relaxed);
    proof.row_->state.store(ArmState::Armed, std::memory_order_release);
    proof.completion_ = completionSequence;
}

bool TimelineArmTable::BindOriginalSourceSubmission(
    const PreparedCommitProof& preparation, const OriginalArmRowOwner& original,
    const void* submission) noexcept {
    if (submission == nullptr || preparation.owner_ != this || original.owner_ != this ||
        preparation.row_ == nullptr || preparation.row_ != original.row_ ||
        !IdentityMatches(*original.row_, original.token_) ||
        original.row_->state.load(std::memory_order_acquire) != ArmState::Prepared ||
        !original.row_->originalLifetimePinned.load(std::memory_order_acquire) ||
        original.row_->authoredControl.operation != nullptr ||
        original.row_->authoredControl.stage != AuthoredControlStage::None ||
        (original.row_->originalSourceSubmission != nullptr &&
         original.row_->originalSourceSubmission != submission)) return false;
    original.row_->originalSourceSubmission = submission;
    return true;
}

bool TimelineArmTable::BindOriginalSourceEpisode(const PreparedCommitProof& preparation,
    const OriginalArmRowOwner& original, const void* submission, std::uint64_t sourceEpisode) noexcept {
    if (preparation.owner_ != this || preparation.row_ == nullptr || original.owner_ != this ||
        original.row_ != preparation.row_ || submission == nullptr || sourceEpisode == 0 ||
        !IdentityMatches(*original.row_, original.token_) ||
        original.row_->renderSlot != original.token_.renderSlot ||
        original.row_->renderGeneration != original.token_.renderGeneration ||
        original.row_->originalSourceSubmission != submission ||
        !original.row_->originalLifetimePinned.load(std::memory_order_acquire) ||
        (original.row_->originalSourceEpisode != 0 &&
         original.row_->originalSourceEpisode != sourceEpisode)) return false;
    original.row_->originalSourceEpisode = sourceEpisode;
    return true;
}

bool TimelineArmTable::IssueOriginalUnpublishedSourceDisposition(
    const OriginalArmRowOwner& original, const void* submission,
    std::uint64_t sourceEpisode, std::uint64_t originalMarker,
    std::uint64_t sourceIdentity, OriginalUnpublishedSourceDisposition* out) const noexcept {
    if (out == nullptr || out->Valid() || original.owner_ != this || original.row_ == nullptr ||
        !original.token_.Valid() || original.token_.slot >= capacity_ || incarnation_ == 0 ||
        submission == nullptr || sourceEpisode == 0 || sourceIdentity == 0) return false;
    const auto token = original.token_;
    const auto& row = *original.row_;
    if (original.row_ != &rows_[token.slot] || !IdentityMatches(row, token) ||
        row.renderSlot != token.renderSlot || row.renderGeneration != token.renderGeneration ||
        row.originalSourceSubmission != submission || row.originalSourceEpisode != sourceEpisode ||
        !row.originalLifetimeIssued ||
        !row.originalLifetimePinned.load(std::memory_order_acquire) ||
        row.authoredControl.operation != nullptr || row.authoredControl.stage != AuthoredControlStage::None)
        return false;
    // Producer-only native output, authenticated by the still-held original
    // Source/Arm association. A joint rollback may already hold the row gate.
    out->owner_ = this; out->row_ = &row; out->submission_ = submission; out->token_ = token;
    out->tableIncarnation_ = incarnation_; out->sourceEpisode_ = sourceEpisode;
    out->originalMarker_ = originalMarker; out->sourceIdentity_ = sourceIdentity;
    return true;
}

TimelineArmTable::UnparentedUnpublishedSourceRetirement
TimelineArmTable::ConsumeOriginalUnparentedUnpublishedSource(
    const OriginalUnpublishedSourceDisposition& original) noexcept {
    using Status = UnparentedUnpublishedSourceRetirement;
    const auto token = original.token_;
    if (original.owner_ != this || !token.Valid() || token.slot >= capacity_ ||
        original.tableIncarnation_ != incarnation_ || original.row_ != &rows_[token.slot] ||
        original.submission_ == nullptr || original.sourceIdentity_ == 0 || original.sourceEpisode_ == 0 ||
        (original.unboundOriginal_
            ? original.commandQueue_ == nullptr || original.originalRange_ == nullptr ||
              original.rangeOrigin_ != 0 || original.rangeHead_ != 0 || original.rangeCount_ != 0
            : original.commandQueue_ == nullptr || original.originalRange_ == nullptr ||
              original.rangeOrigin_ == 0 || original.rangeCount_ == 0)) return Status::NotEligible;
    auto& row = rows_[token.slot];
    ArmMutationAccess access(&row);
    if (!access) return Status::Busy;
    auto& scope = groupMemberScopes_[token.slot];
    if (!IdentityMatches(row, token) || row.renderSlot != token.renderSlot ||
        row.renderGeneration != token.renderGeneration ||
        !SameOriginalGroupArm(scope.sourceFactoryArm, token) || !scope.unparentedPreparedSourceFactory ||
        row.originalSourceSubmission != original.submission_ || row.originalSourceEpisode != original.sourceEpisode_ ||
        !row.originalLifetimeIssued || row.parentGroup.Valid() || row.memberIndex != UINT16_MAX)
        return Status::NotEligible;
    const auto state = row.state.load(std::memory_order_acquire);
    if (scope.jointHeld || (SameOriginalGroupArm(scope.arm, token) && scope.activeCount != 0) ||
        (state != ArmState::Prepared && state != ArmState::PendingApply) ||
        row.originalLifetimePinned.load(std::memory_order_acquire) ||
        row.frontierReaderPins.load(std::memory_order_acquire) != 0 ||
        row.frontierLastReadSequence.load(std::memory_order_acquire) != 0 ||
        row.frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
        row.frontierAdmission != nullptr || row.frontierAdmissionLease != nullptr || row.frontierOperation.Valid() ||
        row.armedCompletionSequence.load(std::memory_order_acquire) != 0 ||
        row.startedBlockSequence.load(std::memory_order_acquire) != 0 ||
        row.logicalCompletionSequence.load(std::memory_order_acquire) != 0 ||
        row.tombstoneAudioSequence.load(std::memory_order_acquire) != 0 ||
        row.appliedPlainStopSequence.load(std::memory_order_acquire) != 0 ||
        row.pendingControlSourceReader.Valid() || row.originalPlainControl.Valid() ||
        row.appliedPlainStopSource.Valid() || row.nativePrivateWithdrawn.load(std::memory_order_acquire) ||
        row.authoredControl.operation != nullptr || row.authoredControl.stage != AuthoredControlStage::None)
        return Status::Busy;
    // Native factory, original Source disposition and all exits were checked
    // under the same row gate. No fallible operation follows this seal.
    scope.jointHeld = true;
    row.identity.store(0, std::memory_order_release);
    if (token.generation != UINT32_MAX) row.generation = token.generation + 1u;
    row.renderSlot = UINT32_MAX; row.renderGeneration = 0;
    row.startFrame = row.armDeadline = 0; row.pendingImmediateStart = false; row.startBound = true;
    row.programGeneration = 0; row.target = {}; row.parentGroup = {}; row.memberIndex = UINT16_MAX;
    row.originalSourceSubmission = nullptr; row.originalSourceEpisode = 0; row.originalLifetimeIssued = false;
    scope.sourceFactoryArm = {}; scope.unparentedPreparedSourceFactory = false;
    row.frontierAdmissionFinished.store(true, std::memory_order_release);
    row.state.store(token.generation == UINT32_MAX ? ArmState::Quarantined : ArmState::Free,
        std::memory_order_release);
    scope.jointHeld = false;
    return Status::Consumed;
}

TimelineArmToken TimelineArmTable::PreparedCommitProof::OriginalPreparedToken() const noexcept {
    if (owner_ == nullptr || row_ == nullptr) return {};
    return {static_cast<std::uint32_t>(row_ - owner_->rows_.get()), row_->generation,
        row_->renderSlot, row_->renderGeneration};
}

TimelineArmTable::SourceCommandRowStatus TimelineArmTable::ProveOriginalSourceCancellation(
    const OriginalArmRowOwner& original, const void* submission,
    SourceCommandRowProof* out) noexcept {
    using Status = SourceCommandRowStatus;
    if (original.owner_ != this || !original.Valid() || submission == nullptr ||
        out == nullptr || out->Valid() || original.row_ == nullptr)
        return Status::Invalid;
    auto& row = *original.row_;
    const auto token = original.token_;
    if (!IdentityMatches(row, token) || row.originalSourceSubmission != submission ||
        !row.originalLifetimePinned.load(std::memory_order_acquire)) return Status::Invalid;
    if (row.state.load(std::memory_order_acquire) != ArmState::Tombstoned &&
        !RequestCancel(token)) return Status::DeferredReservation;
    if (row.reservation.test_and_set(std::memory_order_acquire))
        return Status::DeferredReservation;
    const auto state = row.state.load(std::memory_order_acquire);
    if (!IdentityMatches(row, token) || row.originalSourceSubmission != submission ||
        row.authoredControl.operation != nullptr ||
        (state != ArmState::CancelRequested && state != ArmState::Tombstoned)) {
        row.reservation.clear(std::memory_order_release);
        return Status::NotPendingSource;
    }
    if (state != ArmState::Tombstoned && FrontierAdmissionDefers(&row, true)) {
        row.reservation.clear(std::memory_order_release);
        return Status::DeferredReservation;
    }
    out->owner_ = this;
    out->row_ = &row;
    out->token_ = token;
    out->completion_ = 0;
    out->cancellation_ = true;
    liveSourceCommandProofs_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ready;
}

void TimelineArmTable::TombstoneOriginalSourceCommand(SourceCommandRowProof& proof,
    std::uint64_t audioSequence) noexcept {
    assert(proof.Valid() && proof.cancellation_);
    if (proof.row_->state.load(std::memory_order_acquire) != ArmState::Tombstoned) {
        const bool tombstoned = TombstoneReservedRow(proof.row_, audioSequence);
        assert(tombstoned);
        (void)tombstoned;
    }
}

bool TimelineArmTable::AcknowledgeApplied(
    std::uint64_t packedIdentity, std::uint64_t completionSequence) noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(packedIdentity);
    if (slot >= capacity_) {
        return false;
    }
    Row& row = rows_[slot];
    ArmMutationAccess access(&row);
    if (!access) return false;
    if (row.identity.load(std::memory_order_acquire) != packedIdentity ||
        row.originalSourceSubmission != nullptr) {
        return false;
    }
    row.armedCompletionSequence.store(completionSequence,
                                      std::memory_order_relaxed);
    if (Transition(&row, ArmState::PendingApply, ArmState::Armed)) {
        return true;
    }
    row.armedCompletionSequence.store(0, std::memory_order_relaxed);
    return false;
}

bool TimelineArmTable::ResolveIdentity(
    std::uint64_t packedIdentity, TimelineArmToken* outToken) const noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(packedIdentity);
    if (slot >= capacity_ || outToken == nullptr) {
        return false;
    }
    Row& row = rows_[slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    void (*hook)(void*) =
        resolveIdentityHook_.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(resolveIdentityHookContext_.load(std::memory_order_acquire));
    }
    const ArmState state = row.state.load(std::memory_order_acquire);
    const bool valid =
        state != ArmState::Free && state != ArmState::Quarantined &&
        row.identity.load(std::memory_order_acquire) == packedIdentity;
    if (valid) {
        *outToken = {slot, static_cast<std::uint32_t>(packedIdentity >> 32),
                     row.renderSlot, row.renderGeneration};
    }
    row.reservation.clear(std::memory_order_release);
    return valid;
}

bool TimelineArmTable::AcknowledgeApplied(
    TimelineArmToken token, std::uint64_t completionSequence) noexcept {
    Row* row = Resolve(token);
    if (row != nullptr) {
        if (const auto hook = tokenAcknowledgeHook_.load(std::memory_order_acquire))
            hook(tokenAcknowledgeHookContext_.load(std::memory_order_relaxed));
    }
    ArmMutationAccess access(row);
    if (!access || !IdentityMatches(*row, token) || row->renderSlot != token.renderSlot ||
        row->renderGeneration != token.renderGeneration || row->originalSourceSubmission != nullptr) {
        return false;
    }
    row->armedCompletionSequence.store(completionSequence,
                                       std::memory_order_relaxed);
    if (Transition(row, ArmState::PendingApply, ArmState::Armed)) {
        return true;
    }
    row->armedCompletionSequence.store(0, std::memory_order_relaxed);
    return false;
}

bool TimelineArmTable::Publish(TimelineArmToken token) noexcept {
    return PublishExact(token) == ArmPublishStatus::Published;
}

ArmPublishStatus TimelineArmTable::PublishExact(
    TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr) return ArmPublishStatus::Invalid;
    ArmMutationAccess access(row);
    if (!access) return ArmPublishStatus::DeferredReservation;
    if (!IdentityMatches(*row, token) ||
        row->renderSlot != token.renderSlot ||
        row->renderGeneration != token.renderGeneration)
        return ArmPublishStatus::Invalid;
    if ((row->pendingImmediateStart && !row->startBound) ||
        row->authoredControl.stage == AuthoredControlStage::Pending)
        return ArmPublishStatus::Invalid;
    return Transition(row, ArmState::Armed, ArmState::Published)
        ? ArmPublishStatus::Published : ArmPublishStatus::Invalid;
}

bool TimelineArmTable::BindPendingImmediateStart(
    TimelineArmToken token, std::uint64_t startFrame,
    std::uint64_t armDeadline) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || armDeadline > startFrame ||
        row->reservation.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    const bool valid = IdentityMatches(*row, token) &&
        row->state.load(std::memory_order_acquire) == ArmState::Armed &&
        row->pendingImmediateStart && !row->startBound &&
        row->authoredControl.operation == nullptr &&
        row->frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row->frontierAdmission == nullptr &&
        !row->frontierOperation.Valid();
    if (valid) {
        row->startFrame = startFrame;
        row->armDeadline = armDeadline;
        row->startBound = true;
    }
    row->reservation.clear(std::memory_order_release);
    return valid;
}

bool TimelineArmTable::ProvePrivatePendingImmediateArmBatch(
    const PrivatePendingImmediateArmInput* inputs,
    std::size_t inputCount, Row** reservedRows,
    std::size_t reservedCapacity,
    PrivatePendingImmediateArmProof* out) noexcept {
    if (inputs == nullptr || inputCount == 0 || inputCount > capacity_ ||
        reservedRows == nullptr || reservedCapacity < inputCount ||
        out == nullptr || out->Valid()) return false;
    std::size_t reserved = 0;
    for (; reserved < inputCount; ++reserved) {
        const auto& input = inputs[reserved];
        if (!input.arm.Valid() || input.arm.slot >= capacity_ ||
            input.armDeadline > input.startFrame || input.pin == nullptr ||
            input.pin->Valid() ||
            (input.admission == nullptr) != (input.lease == nullptr))
            break;
        bool duplicatePin = false;
        for (std::size_t prior = 0; prior < reserved; ++prior)
            duplicatePin = duplicatePin || inputs[prior].pin == input.pin;
        if (duplicatePin) break;
        Row* row = Resolve(input.arm);
        if (row == nullptr ||
            row->reservation.test_and_set(std::memory_order_acquire))
            break;
        const bool grouped = row->parentGroup.Valid();
        const bool validAdmission = input.admission == nullptr ||
            (input.admission->Valid() &&
             input.lease->Capture().state == AudioSliceState::Ready &&
             (!grouped ||
              ((input.admission->role == FrontierGroupRole::EntryMember ||
                input.admission->role == FrontierGroupRole::Future) &&
               row->parentGroup.decision.slot ==
                   input.admission->operation.group.decision.slot &&
               row->parentGroup.decision.generation ==
                   input.admission->operation.group.decision.generation)));
        const bool valid = IdentityMatches(*row, input.arm) &&
            row->renderSlot == input.arm.renderSlot &&
            row->renderGeneration == input.arm.renderGeneration &&
            row->state.load(std::memory_order_acquire) == ArmState::Armed &&
            row->pendingImmediateStart && !row->startBound &&
            row->frontierReaderPins.load(std::memory_order_acquire) == 0 &&
            row->frontierAdmission == nullptr &&
            row->frontierAdmissionLease == nullptr &&
            row->frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
            !row->frontierOperation.Valid() && validAdmission;
        if (!valid) {
            row->reservation.clear(std::memory_order_release);
            break;
        }
        reservedRows[reserved] = row;
    }
    if (reserved != inputCount) {
        for (std::size_t index = 0; index < reserved; ++index) {
            reservedRows[index]->reservation.clear(std::memory_order_release);
            reservedRows[index] = nullptr;
        }
        return false;
    }
    out->owner_ = this;
    out->inputs_ = inputs;
    out->rows_ = reservedRows;
    out->count_ = inputCount;
    return true;
}

void TimelineArmTable::ApplyPrivatePendingImmediateArmBatch(
    PrivatePendingImmediateArmProof& proof) noexcept {
    assert(proof.Valid());
    for (std::size_t index = 0; index < proof.count_; ++index) {
        const auto& input = proof.inputs_[index];
        Row* row = proof.rows_[index];
        assert(row != nullptr && input.pin != nullptr &&
               !input.pin->Valid());
        row->startFrame = input.startFrame;
        row->armDeadline = input.armDeadline;
        row->startBound = true;
        row->frontierReaderPins.store(1, std::memory_order_release);
        input.pin->owner_ = proof.owner_;
        input.pin->token_ = input.arm;
        if (input.admission != nullptr) {
            row->frontierAdmission = input.admission;
            row->frontierAdmissionLease = input.lease;
            row->frontierAdmissionFinished.store(false,
                                                  std::memory_order_release);
        }
        row->state.store(ArmState::Published, std::memory_order_release);
    }
    proof.Release();
}

bool TimelineArmTable::ClaimAudioSlice(TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || row->nativePrivateWithdrawn.load(std::memory_order_acquire) ||
        row->authoredControl.operation != nullptr || (row->pendingImmediateStart && !row->startBound) ||
        FrontierAdmissionDefers(row, false)) return false;
    ArmState expected = row->state.load(std::memory_order_acquire);
    while (expected == ArmState::Published ||
           expected == ArmState::CancelRequested) {
        if (row->state.compare_exchange_weak(
                expected, ArmState::AudioClaimed,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return expected == ArmState::AudioClaimed ||
           expected == ArmState::Started;
}

bool TimelineArmTable::ProveLaneStart(
    TimelineArmToken token, std::uint64_t executeFrame,
    std::uint64_t audioSequence, LaneAudioProof* out,
    ProgramGeneration programGeneration,
    const TimelineSemanticTarget* target) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || out->Valid() ||
        audioSequence == 0 ||
        row->reservation.test_and_set(std::memory_order_acquire))
        return false;
    if (row->authoredControl.operation != nullptr ||
        row->frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        row->frontierAdmission != nullptr || row->parentGroup.Valid() ||
        !IdentityMatches(*row, token) ||
        (programGeneration != 0 && row->programGeneration != programGeneration) ||
        (target != nullptr && !(row->target == *target)) ||
        !row->startBound || row->startFrame != executeFrame ||
        row->state.load(std::memory_order_acquire) != ArmState::Published) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    out->row_ = row;
    out->token_ = token;
    out->audioSequence_ = audioSequence;
    return true;
}

bool TimelineArmTable::LaneStartCancelled(const LaneAudioProof& proof) noexcept {
    if (!proof.Valid()) return false;
    const auto& source = *proof.row_;
    const auto applied = source.appliedPlainStopSequence.load(std::memory_order_acquire);
    return applied != 0 && applied <= proof.audioSequence_ &&
        SameArm(source.appliedPlainStopSource, proof.token_) &&
        source.appliedPlainStop.Valid() &&
        source.appliedPlainStop == source.originalPlainControlIdentity &&
        source.appliedPlainStop.stopFrame <= source.startFrame;
}

bool TimelineArmTable::ProveLaneCancelledStart(const TimelineArmReaderPin& reader,
    const ConsumedEvent& original, ProgramGeneration armGeneration,
    std::uint64_t audioSequence, LaneAudioProof* out) noexcept {
    if (out == nullptr || out->Valid() || audioSequence == 0 ||
        !original.frozenTxnValid || reader.owner_ != this ||
        !SameArm(reader.token_, original.armToken) || reader.token_.slot >= capacity_) return false;
    auto& row = rows_[reader.token_.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row.state.load(std::memory_order_acquire);
    if (!PinnedPlainSourceStartMatches(reader, original, armGeneration) ||
        !row.pendingImmediateStart || row.startedBlockSequence.load(std::memory_order_acquire) != 0 ||
        (state != ArmState::Published && state != ArmState::CancelRequested && state != ArmState::Tombstoned)) {
        row.reservation.clear(std::memory_order_release); return false;
    }
    out->row_ = &row;
    out->token_ = reader.token_;
    out->audioSequence_ = audioSequence;
    if (LaneStartCancelled(*out)) return true;
    out->Release();
    return false;
}

bool TimelineArmTable::ReleasePendingControlSourceReader(Row& control,
    TimelineArmToken token, std::uint64_t completedAudioSequence) noexcept {
    auto& pin = control.pendingControlSourceReader;
    if (!pin.Valid()) return true;
    if (pin.owner_ != this || pin.token_.slot >= capacity_ ||
        !SameArm(pin.token_, control.authoredControl.sourceStart)) return false;
    auto& source = rows_[pin.token_.slot];
    if (source.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto lastRead = source.frontierLastReadSequence.load(std::memory_order_acquire);
    const auto pins = source.frontierReaderPins.load(std::memory_order_acquire);
    const bool valid = PinnedAuthoredControlSource(control) == &source &&
        SameArm(source.originalPlainControl, token) &&
        source.originalPlainControlIdentity == control.authoredControl.identity &&
        pins != 0 && (lastRead == 0 || completedAudioSequence > lastRead);
    if (valid) {
        // The issuance identity remains until this Source generation truly retires.
        (void)source.frontierReaderPins.fetch_sub(1u, std::memory_order_release);
        pin.owner_ = nullptr; pin.token_ = {};
    }
    source.reservation.clear(std::memory_order_release);
    return valid;
}

void TimelineArmTable::ApplyLaneStart(LaneAudioProof& proof) noexcept {
    assert(proof.Valid());
    if (LaneStartCancelled(proof)) {
        proof.row_->frontierLastReadSequence.store(proof.audioSequence_, std::memory_order_release);
        if (proof.row_->state.load(std::memory_order_acquire) != ArmState::Tombstoned)
            proof.row_->state.store(ArmState::CancelRequested, std::memory_order_release);
        proof.Release();
        return;
    }
    proof.row_->startedBlockSequence.store(proof.audioSequence_,
                                           std::memory_order_relaxed);
    proof.row_->frontierLastReadSequence.store(proof.audioSequence_,
                                              std::memory_order_release);
    proof.row_->state.store(ArmState::Started, std::memory_order_release);
    if (proof.row_->frontierAdmission != nullptr)
        proof.row_->frontierAdmissionFinished.store(true,
                                                   std::memory_order_release);
    proof.Release();
}

bool TimelineArmTable::ProveLaneFutureStart(TimelineArmToken token,
    const FrontierGroupBinding& binding, std::uint16_t memberIndex,
    std::uint64_t executeFrame, std::uint64_t audioSequence,
    LaneAudioProof* out, ProgramGeneration programGeneration,
    const TimelineSemanticTarget& target,
    bool entryInSameBucket) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || out->Valid() ||
        audioSequence == 0 || row->reservation.test_and_set(
            std::memory_order_acquire)) return false;
    const bool valid = row->authoredControl.operation == nullptr && binding.role == FrontierGroupRole::EntryMember &&
        binding.operation.group.backing == Plan03GroupBacking::ImmediateLane &&
        row->frontierReaderPins.load(std::memory_order_acquire) != 0 &&
        row->frontierAdmission == &binding &&
        row->frontierAdmissionLease != nullptr &&
        row->frontierAdmissionLease->Capture().state ==
            (entryInSameBucket ? AudioSliceState::Ready :
                AudioSliceState::SliceApplied) &&
        !row->frontierAdmissionFinished.load(std::memory_order_acquire) &&
        row->parentGroup.Valid() &&
        row->parentGroup.decision.slot == binding.operation.group.decision.slot &&
        row->parentGroup.decision.generation ==
            binding.operation.group.decision.generation &&
        row->memberIndex == memberIndex &&
        IdentityMatches(*row, token) &&
        row->programGeneration == programGeneration && row->target == target &&
        row->startBound && row->startFrame == executeFrame &&
        row->state.load(std::memory_order_acquire) == ArmState::Published &&
        row->frontierLastReadSequence.load(std::memory_order_acquire) <=
            audioSequence;
    if (!valid) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    out->row_ = row;
    out->token_ = token;
    out->audioSequence_ = audioSequence;
    return true;
}

void TimelineArmTable::ApplyClaimedAudioSlice(
    TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || row->nativePrivateWithdrawn.load(std::memory_order_acquire) ||
        row->authoredControl.operation != nullptr || (row->pendingImmediateStart && !row->startBound) ||
        FrontierAdmissionDefers(row, false)) return;
    row->state.store(ArmState::AudioClaimed, std::memory_order_release);
}

bool TimelineArmTable::RequestCancelAppliedAudioSlice(
    TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    auto* activeAudioLease = ActiveBoundAudioProofLease(row, token);
    if (activeAudioLease != nullptr &&
        InvalidateActiveBoundAudioProof(activeAudioLease)) return false;
    ArmMutationAccess access(row);
    if (!access ||
        (activeAudioLease == nullptr && FrontierAdmissionDefers(row, true)))
        return false;
    ArmState expected = ArmState::AudioClaimed;
    if (row->state.compare_exchange_strong(
            expected, ArmState::CancelRequested,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    return expected == ArmState::CancelRequested ||
           expected == ArmState::Tombstoned;
}

void TimelineArmTable::CompleteClaimedAudioSlice(
    TimelineArmToken token, std::uint64_t audioSequence) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || row->authoredControl.operation != nullptr ||
        FrontierAdmissionDefers(row, false)) return;
    row->logicalCompletionSequence.store(audioSequence,
                                         std::memory_order_relaxed);
    row->tombstoneAudioSequence.store(audioSequence,
                                      std::memory_order_relaxed);
    row->state.store(ArmState::Tombstoned, std::memory_order_release);
}

bool TimelineArmTable::Start(TimelineArmToken token) noexcept {
    return Start(token, 0);
}

bool TimelineArmTable::Start(TimelineArmToken token,
                             std::uint64_t blockSequence) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || row->nativePrivateWithdrawn.load(std::memory_order_acquire) ||
        row->authoredControl.operation != nullptr || FrontierAdmissionDefers(row, false)) {
        return false;
    }
    const auto state = row->state.load(std::memory_order_acquire);
    if (state != ArmState::Published && state != ArmState::AudioClaimed) return false;
    row->startedBlockSequence.store(blockSequence,
                                    std::memory_order_relaxed);
    if (Transition(row, ArmState::Published, ArmState::Started) ||
        Transition(row, ArmState::AudioClaimed, ArmState::Started)) {
        return true;
    }
    row->startedBlockSequence.store(0, std::memory_order_relaxed);
    return false;
}

bool TimelineArmTable::PinFrontierReader(TimelineArmToken token,
    TimelineArmReaderPin* out) noexcept {
    if (out == nullptr || out->Valid() || !token.Valid() || token.slot >= capacity_) return false;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row.state.load(std::memory_order_acquire);
    const auto pins = row.frontierReaderPins.load(std::memory_order_relaxed);
    const bool valid = IdentityMatches(row, token) && row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration && pins != UINT32_MAX &&
        row.authoredControl.stage != AuthoredControlStage::Pending &&
        state != ArmState::Free && state != ArmState::Quarantined &&
        (pins != 0 || state == ArmState::Prepared || state == ArmState::PendingApply || state == ArmState::Armed);
    if (valid) {
        (void)row.frontierReaderPins.fetch_add(1u, std::memory_order_acq_rel);
        out->owner_ = this;
        out->token_ = token;
    }
    row.reservation.clear(std::memory_order_release);
    return valid;
}

bool TimelineArmTable::BindFrontierAdmission(TimelineArmToken token,
    const FrontierGroupBinding& binding, AudioSliceLease& lease) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || !binding.Valid() ||
        row->frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        row->reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row->state.load(std::memory_order_acquire);
    const bool grouped = row->parentGroup.Valid();
    const bool valid = row->authoredControl.operation == nullptr && row->frontierAdmission == nullptr && lease.Capture().state == AudioSliceState::Ready &&
        (state == ArmState::Prepared || state == ArmState::PendingApply || state == ArmState::Armed) &&
        (!grouped || ((binding.role == FrontierGroupRole::EntryMember || binding.role == FrontierGroupRole::Future) &&
            row->parentGroup.decision.slot == binding.operation.group.decision.slot &&
            row->parentGroup.decision.generation == binding.operation.group.decision.generation));
    if (valid) {
        row->frontierAdmission = &binding;
        row->frontierAdmissionLease = &lease;
        row->frontierAdmissionFinished.store(false, std::memory_order_release);
    }
    row->reservation.clear(std::memory_order_release);
    return valid;
}

bool TimelineArmTable::WitnessUnpublishedFrontierRetirement(
    TimelineArmToken token, const FrontierGroupBinding& binding,
    const AudioSliceLease& lease) const noexcept {
    if (incarnation_ == 0 || !token.Valid() || token.slot >= capacity_) return false;
    auto& row = const_cast<Row&>(rows_[token.slot]);
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row.state.load(std::memory_order_acquire);
    const bool admissionMatches =
        (row.frontierAdmission == nullptr && row.frontierAdmissionLease == nullptr) ||
        (row.frontierAdmission == &binding && row.frontierAdmissionLease == &lease);
    const bool witnessed = IdentityMatches(row, token) &&
        row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration &&
        (state == ArmState::CancelRequested || state == ArmState::Tombstoned) &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        admissionMatches;
    row.reservation.clear(std::memory_order_release);
    return witnessed;
}

bool TimelineArmTable::FrontierAdmissionReleasedForRetirement(
    TimelineArmToken token, const FrontierGroupBinding& binding,
    const AudioSliceLease& lease) const noexcept {
    if (!token.Valid() || token.slot >= capacity_) return false;
    auto& row = const_cast<Row&>(rows_[token.slot]);
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row.state.load(std::memory_order_acquire);
    const bool admissionMatches =
        (row.frontierAdmission == nullptr && row.frontierAdmissionLease == nullptr) ||
        (row.frontierAdmission == &binding && row.frontierAdmissionLease == &lease);
    const bool released = IdentityMatches(row, token) &&
        row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration &&
        (state == ArmState::CancelRequested || state == ArmState::Tombstoned) &&
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row.frontierAdmissionFinished.load(std::memory_order_acquire) &&
        admissionMatches;
    row.reservation.clear(std::memory_order_release);
    return released;
}

bool TimelineArmTable::FrontierArmReclaimedAfterRetirementWitness(
    TimelineArmToken token, std::uint64_t witnessedIncarnation) const noexcept {
    if (witnessedIncarnation == 0 || witnessedIncarnation != incarnation_ ||
        !token.Valid() || token.slot >= capacity_) return false;
    auto& row = const_cast<Row&>(rows_[token.slot]);
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto state = row.state.load(std::memory_order_acquire);
    const bool reclaimed = row.generation > token.generation ||
        (token.generation == UINT32_MAX && row.generation == UINT32_MAX &&
         state == ArmState::Quarantined &&
         row.identity.load(std::memory_order_acquire) == 0);
    row.reservation.clear(std::memory_order_release);
    return reclaimed;
}

bool TimelineArmTable::PrivateFrontierAdmissionReady(
    TimelineArmToken token, const FrontierGroupBinding& binding,
    AudioSliceLease& lease) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->reservation.test_and_set(std::memory_order_acquire)) return false;
    const bool ready = IdentityMatches(*row, token) &&
        row->renderSlot == token.renderSlot &&
        row->renderGeneration == token.renderGeneration &&
        row->state.load(std::memory_order_acquire) == ArmState::Armed &&
        row->frontierAdmission == &binding &&
        row->frontierAdmissionLease == &lease &&
        row->frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row->frontierReaderPins.load(std::memory_order_acquire) != 0 &&
        row->frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        row->startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        lease.Capture().state == AudioSliceState::Ready;
    row->reservation.clear(std::memory_order_release);
    return ready;
}

bool TimelineArmTable::ProveBoundFrontierArm(TimelineArmToken token,
    const FrontierEntryReceiptRef& entry, std::uint64_t audioSequence,
    bool starts, BoundAudioProof* out, const FrontierGroupBinding* expectedBinding) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || out->Valid() || audioSequence == 0 ||
        row->reservation.test_and_set(std::memory_order_acquire)) return false;
    if (row->authoredControl.operation != nullptr ||
        row->frontierAdmissionFinished.load(std::memory_order_acquire) ||
        row->frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        row->frontierAdmission == nullptr || row->frontierAdmissionLease == nullptr ||
        !(row->frontierAdmission->entry == entry) ||
        (expectedBinding != nullptr && row->frontierAdmission != expectedBinding) ||
        row->frontierAdmissionLease->Capture().state != AudioSliceState::Ready ||
        row->state.load(std::memory_order_acquire) != ArmState::Published ||
        row->frontierLastReadSequence.load(std::memory_order_acquire) > audioSequence ||
        (starts && row->startFrame != row->frontierAdmission->operation.order.executeFrame)) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    out->row_ = row;
    out->lease_ = row->frontierAdmissionLease;
    out->binding_ = row->frontierAdmission;
    out->sequence_ = audioSequence;
    out->starts_ = starts;
    row->frontierActiveLease.store(out->lease_, std::memory_order_release);
    return true;
}

bool TimelineArmTable::ProveGroupedFrontierOperation(TimelineArmToken token, const FrontierGroupBinding& binding,
    AudioSliceLease& lease, const ScheduledTransactionPool& pool,
    std::uint64_t audioSequence, bool starts, BoundAudioProof* out) noexcept {
    Plan03PageGroupView group{};
    if (!pool.ResolvePlan03GroupSliceRt(binding.operation.group, &group) ||
        group.frontier != &binding || group.lease != &lease ||
        !pool.NotePlan03GroupReadRt(binding.operation.group, audioSequence)) return false;
    Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || out->Valid() || !binding.Valid() ||
        binding.role != FrontierGroupRole::Future || audioSequence == 0 ||
        row->reservation.test_and_set(std::memory_order_acquire)) return false;
    if (row->authoredControl.operation != nullptr ||
        row->frontierReaderPins.load(std::memory_order_acquire) == 0 || row->frontierAdmission == nullptr ||
        !(row->frontierAdmission->entry == binding.entry) ||
        (!row->frontierAdmissionFinished.load(std::memory_order_acquire) && row->frontierAdmission != &binding) ||
        !row->parentGroup.Valid() || row->parentGroup.decision.slot != binding.operation.group.decision.slot ||
        row->parentGroup.decision.generation != binding.operation.group.decision.generation ||
        (starts ? row->state.load(std::memory_order_acquire) != ArmState::Published :
            (row->state.load(std::memory_order_acquire) != ArmState::Started &&
             row->state.load(std::memory_order_acquire) != ArmState::AudioClaimed &&
             !(row->state.load(std::memory_order_acquire) == ArmState::Published &&
                row->frontierAdmission == &binding))) ||
        row->frontierLastReadSequence.load(std::memory_order_acquire) > audioSequence ||
        lease.Capture().state != AudioSliceState::Ready ||
        (starts && row->startFrame != binding.operation.order.executeFrame) ||
        (row->frontierOperation.Valid() && !(row->frontierOperation.order < binding.operation.order))) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    out->row_ = row;
    out->lease_ = &lease;
    out->binding_ = &binding;
    out->sequence_ = audioSequence;
    out->starts_ = starts;
    row->frontierActiveLease.store(&lease, std::memory_order_release);
    return true;
}

bool TimelineArmTable::ProveLaneGroupedFutureStart(TimelineArmToken token,
    const FrontierGroupBinding& binding, AudioSliceLease& lease,
    const ScheduledTransactionPool& pool, std::uint64_t audioSequence,
    std::uint16_t memberIndex, ProgramGeneration programGeneration,
    const TimelineSemanticTarget& target, BoundAudioProof* out) noexcept {
    if (binding.operation.group.backing != Plan03GroupBacking::ImmediateLane ||
        !binding.queueInitial.Valid() || binding.queueConditional.Valid() ||
        binding.variableSide.Valid() || binding.operation.group.sliceIndex != 1u ||
        memberIndex != 1u || programGeneration == 0 ||
        !ProveGroupedFrontierOperation(token, binding, lease, pool,
            audioSequence, true, out)) return false;
    const auto* row = static_cast<Row*>(out->row_);
    if (!IdentityMatches(*row, token) || row->memberIndex != memberIndex ||
        row->programGeneration != programGeneration || !(row->target == target)) {
        out->Release();
        return false;
    }
    return true;
}

bool TimelineArmTable::ProveDeferredFrontierStart(TimelineArmToken token, const FrontierGroupBinding& binding,
    AudioSliceLease& lease, const ScheduledTransactionPool& pool,
    std::uint64_t audioSequence, BoundAudioProof* out) noexcept {
    if (!ProveGroupedFrontierOperation(token, binding, lease, pool, audioSequence, false, out)) return false;
    if (out->row_->state.load(std::memory_order_acquire) == ArmState::Published &&
        out->row_->startFrame > binding.operation.order.executeFrame &&
        out->row_->frontierAdmission == &binding && !out->row_->frontierOperation.Valid()) return true;
    out->Release();
    return false;
}

void TimelineArmTable::ApplyBoundFrontierArm(BoundAudioProof& proof, bool completesControl) noexcept {
    assert(proof.Valid() && proof.lease_ != nullptr && proof.binding_ != nullptr);
    const auto state = proof.lease_->Capture().state;
    assert(state == AudioSliceState::SliceClaimed || state == AudioSliceState::SliceClaimedWithInvalidation);
    (void)state;
    proof.row_->frontierOperation = proof.binding_->operation;
    proof.row_->frontierLastReadSequence.store(proof.sequence_, std::memory_order_release);
    if (proof.starts_)
        proof.row_->startedBlockSequence.store(proof.sequence_, std::memory_order_relaxed);
    if (completesControl) {
        proof.row_->logicalCompletionSequence.store(proof.sequence_, std::memory_order_relaxed);
        proof.row_->tombstoneAudioSequence.store(proof.sequence_, std::memory_order_relaxed);
        proof.row_->state.store(ArmState::Tombstoned, std::memory_order_release);
    } else if (proof.starts_) {
        proof.row_->state.store(ArmState::Started, std::memory_order_release);
    } else if (proof.row_->state.load(std::memory_order_acquire) == ArmState::Published) {
        proof.row_->state.store(ArmState::AudioClaimed, std::memory_order_release);
    }
}

bool TimelineArmTable::ReleaseFrontierReader(TimelineArmToken token,
    std::uint64_t completedAudioSequence) noexcept {
    if (!token.Valid() || token.slot >= capacity_) return false;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
    const auto pins = row.frontierReaderPins.load(std::memory_order_acquire);
    const auto lastRead = row.frontierLastReadSequence.load(std::memory_order_acquire);
    const auto state = row.state.load(std::memory_order_acquire);
    const bool admissionFinished = row.frontierAdmissionFinished.load(std::memory_order_acquire) ||
        state == ArmState::Prepared || state == ArmState::PendingApply || state == ArmState::Armed ||
        (row.frontierAdmissionLease != nullptr &&
         row.frontierAdmissionLease->Capture().state == AudioSliceState::SliceInvalidated);
    const bool valid = IdentityMatches(row, token) && row.renderSlot == token.renderSlot &&
        row.renderGeneration == token.renderGeneration && pins != 0 &&
        admissionFinished && (lastRead == 0 || completedAudioSequence > lastRead);
    if (valid) {
        if (pins == 1) row.frontierAdmissionFinished.store(true, std::memory_order_release);
        (void)row.frontierReaderPins.fetch_sub(1u, std::memory_order_acq_rel);
    }
    row.reservation.clear(std::memory_order_release);
    return valid;
}

bool TimelineArmReaderPin::Release(std::uint64_t completedAudioSequence) noexcept {
    if (owner_ == nullptr) return true;
    if (!owner_->ReleaseFrontierReader(token_, completedAudioSequence)) return false;
    owner_ = nullptr;
    token_ = {};
    return true;
}

const TimelineArmTable::Row* TimelineArmTable::PinnedPlainRow(
    const TimelineArmReaderPin& pin) const noexcept {
    if (pin.owner_ != this || !pin.token_.Valid() || pin.token_.slot >= capacity_) return nullptr;
    const auto& row = rows_[pin.token_.slot];
    const auto state = row.state.load(std::memory_order_acquire);
    if (!IdentityMatches(row, pin.token_) || row.renderSlot != pin.token_.renderSlot ||
        row.renderGeneration != pin.token_.renderGeneration ||
        state == ArmState::Free || state == ArmState::Quarantined ||
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        row.parentGroup.Valid() || row.memberIndex != UINT16_MAX ||
        row.frontierAdmission != nullptr || row.frontierAdmissionLease != nullptr ||
        row.frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
        row.frontierOperation.Valid()) return nullptr;
    return &row;
}

const TimelineArmTable::Row* TimelineArmTable::PinnedAuthoredControlSource(
    const Row& control) const noexcept {
    const auto& pin = control.pendingControlSourceReader;
    const auto selected = control.authoredControl.selectedSourceGroup;
    if (!selected.Valid()) return PinnedPlainRow(pin);
    if (pin.owner_ != this || !pin.token_.Valid() || pin.token_.slot >= capacity_ ||
        !SameArm(pin.token_, control.authoredControl.sourceStart)) return nullptr;
    const auto& row = rows_[pin.token_.slot];
    const auto state = row.state.load(std::memory_order_acquire);
    if (!IdentityMatches(row, pin.token_) || row.renderSlot != pin.token_.renderSlot ||
        row.renderGeneration != pin.token_.renderGeneration ||
        state == ArmState::Free || state == ArmState::Quarantined ||
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        row.parentGroup.decision.slot != selected.decision.slot ||
        row.parentGroup.decision.generation != selected.decision.generation ||
        row.memberIndex != 0 || row.authoredControl.operation != nullptr) return nullptr;
    return &row;
}

bool TimelineArmTable::PinnedPlainSourceStartMatches(const TimelineArmReaderPin& pin,
    const ConsumedEvent& event, ProgramGeneration armGeneration) const noexcept {
    const auto* row = PinnedPlainRow(pin);
    const auto* program = event.program.program;
    if (row == nullptr || !SameArm(pin.token_, event.armToken) ||
        row->authoredControl.operation != nullptr || !row->startBound ||
        row->programGeneration != armGeneration || armGeneration == 0 ||
        event.executeFrame != row->startFrame || event.subeventPhase != 20 ||
        event.frontier.Present() || program == nullptr || program->role != TimelineProgramRole::Voice ||
        program->generation != event.program.programGeneration || program->authoredStopFence != nullptr ||
        program->payloads == nullptr || event.payload >= program->payloadCount) return false;
    const auto& payload = program->payloads[event.payload];
    return payload.kind == TimelineEventKind::Start && payload.reserved == 0 &&
        payload.target == row->target;
}

const TimelineArmTable::AuthoredControlMetadata* TimelineArmTable::PinnedPlainControlMatches(
    const TimelineArmReaderPin& pin, const ConsumedEvent& event,
    const AuthoredStopOperation* operation) const noexcept {
    const auto* row = PinnedPlainRow(pin);
    if (row == nullptr || !SameArm(pin.token_, event.armToken)) return nullptr;
    const auto& control = row->authoredControl;
    const auto* view = event.program.program;
    if (control.stage != AuthoredControlStage::Bound || control.operation == nullptr ||
        (operation != nullptr && operation != control.operation) ||
        control.operation->Identity() != control.identity || row->pendingImmediateStart || !row->startBound ||
        view == nullptr || view != control.program.program ||
        event.program.programGeneration != control.program.programGeneration ||
        row->programGeneration != control.program.programGeneration ||
        view->generation != control.program.programGeneration || view->role != TimelineProgramRole::Voice ||
        view->authoredStopFence != nullptr || view->payloads != control.payloads ||
        view->payloads == nullptr || control.events == nullptr || control.eventCount == 0 ||
        control.eventCount > 2 || view->payloadCount != control.eventCount ||
        event.frontier.Present() || event.payload >= control.eventCount) return nullptr;
    for (std::uint32_t index = 0; index < control.eventCount; ++index) {
        if (!ControlPayloadMatches(view->payloads[index], control, row->target,
                control.eventCount == 2 && index == 0) ||
            !SameControlEvent(control.events[index], control.eventsSnapshot[index]) ||
            !SameArm(control.events[index].armToken, pin.token_)) return nullptr;
    }
    const ScheduledEventInput input{event.executeFrame, event.logicCommitSequence,
        event.executionOrdinal, event.eventIndex, event.transactionOrdinal, event.subeventPhase,
        event.payload, event.authoredFadeOutCandidate, event.armToken, event.frontier};
    return SameControlEvent(input, control.eventsSnapshot[event.payload]) ? &control : nullptr;
}

void TimelineArmTable::ReleaseWithdrawnPlainLaneReader(TimelineArmReaderPin& pin) noexcept {
    // Only the actual Lane scope exclusion can issue this reference return.
    const auto* original = PinnedPlainRow(pin);
    assert(original != nullptr);
    auto* row = const_cast<Row*>(original);
    pin.owner_ = nullptr;
    pin.token_ = {};
    const auto previous = row->frontierReaderPins.fetch_sub(1u, std::memory_order_release);
    assert(previous != 0);
    (void)previous;
}

TimelineArmOperationDisposition TimelineArmTable::BeginFrontierOperation(
    TimelineArmToken token, const FrontierDueOperationRef& operation,
    TimelineArmOperationKind kind, std::uint64_t audioSequence,
    AudioOperationClaim* out, const FrontierEventBinding* binding,
    const ScheduledTransactionPool* pool) noexcept {
    if (out == nullptr || out->Valid() || !token.Valid() || token.slot >= capacity_ ||
        !operation.Valid() || operation.group.Valid() || audioSequence == 0 ||
        operation.armSlot != token.slot || operation.armGeneration != token.generation ||
        operation.renderSlot != token.renderSlot || operation.renderGeneration != token.renderGeneration)
        return TimelineArmOperationDisposition::Stale;
    auto& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire))
        return TimelineArmOperationDisposition::Deferred;
    const auto reject = [&](TimelineArmOperationDisposition result) {
        row.reservation.clear(std::memory_order_release);
        return result;
    };
    if (row.authoredControl.operation != nullptr)
        return reject(TimelineArmOperationDisposition::Ineligible);
    if (!IdentityMatches(row, token) || row.renderSlot != token.renderSlot ||
        row.renderGeneration != token.renderGeneration ||
        row.programGeneration != operation.programGeneration ||
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 ||
        !row.frontierAdmissionFinished.load(std::memory_order_acquire) ||
        row.frontierLastReadSequence.load(std::memory_order_acquire) > audioSequence)
        return reject(TimelineArmOperationDisposition::Stale);
    const bool transientControl = kind == TimelineArmOperationKind::TransientControl;
    if (row.parentGroup.Valid()) {
        const auto* admission = row.frontierAdmission;
        Plan03PageGroupView prior{};
        if (binding == nullptr || pool == nullptr ||
            binding->kind != FrontierOperationKind::Dependent || admission == nullptr ||
            admission->role != FrontierGroupRole::Future || !(admission->entry == binding->entry) ||
            !(admission->operation.owner == operation.owner) ||
            admission->operation.world != operation.world || admission->operation.transport != operation.transport ||
            admission->operation.group.sliceIndex != binding->operationOrdinal ||
            admission->operation.group.decision.slot != row.parentGroup.decision.slot ||
            admission->operation.group.decision.generation != row.parentGroup.decision.generation ||
            !(admission->operation.order < operation.order) ||
            (kind == TimelineArmOperationKind::Start &&
                (row.startFrame <= admission->operation.order.executeFrame || row.frontierOperation.Valid())) ||
            (transientControl && (!(row.frontierOperation.group == admission->operation.group) ||
                row.frontierOperation.order.logicCommitSequence != operation.order.logicCommitSequence)) ||
            !pool->ResolvePlan03GroupSliceRt(admission->operation.group, &prior) ||
            prior.frontier != admission || prior.lease != row.frontierAdmissionLease || prior.lease == nullptr ||
            prior.lease->Capture().state != AudioSliceState::SliceApplied ||
            !pool->NotePlan03GroupReadRt(admission->operation.group, audioSequence))
            return reject(TimelineArmOperationDisposition::Stale);
    } else if (transientControl) {
        return reject(TimelineArmOperationDisposition::Ineligible);
    }
    auto expected = kind == TimelineArmOperationKind::Start ? ArmState::Published : ArmState::Started;
    if (transientControl) expected = ArmState::AudioClaimed;
    if ((kind == TimelineArmOperationKind::Start && row.startFrame != operation.order.executeFrame) ||
        (row.frontierOperation.Valid() && !(row.frontierOperation.order < operation.order)))
        return reject(TimelineArmOperationDisposition::Ineligible);
    const auto priorOperation = row.frontierOperation;
    row.frontierOperation = operation;
    if (!row.state.compare_exchange_strong(expected, ArmState::AudioClaimed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        row.frontierOperation = priorOperation;
        return reject(expected == ArmState::CancelRequested || expected == ArmState::Tombstoned ?
            TimelineArmOperationDisposition::Cancelled : TimelineArmOperationDisposition::Ineligible);
    }
    row.frontierLastReadSequence.store(audioSequence, std::memory_order_release);
    out->row_ = &row;
    out->kind_ = kind;
    out->sequence_ = audioSequence;
    out->applied_ = false;
    return TimelineArmOperationDisposition::Claimed;
}

void TimelineArmTable::ApplyFrontierOperation(AudioOperationClaim& claim) noexcept {
    assert(claim.row_ != nullptr && !claim.applied_);
    if (claim.kind_ == TimelineArmOperationKind::Start)
        claim.row_->startedBlockSequence.store(claim.sequence_, std::memory_order_relaxed);
    claim.applied_ = true;
}

void TimelineArmTable::FinishFrontierOperation(AudioOperationClaim& claim, bool completesControl) noexcept {
    assert(claim.row_ != nullptr && claim.applied_);
    if (claim.kind_ == TimelineArmOperationKind::TransientControl || completesControl) {
        claim.row_->logicalCompletionSequence.store(claim.sequence_, std::memory_order_relaxed);
        claim.row_->tombstoneAudioSequence.store(claim.sequence_, std::memory_order_relaxed);
        claim.row_->state.store(ArmState::Tombstoned, std::memory_order_release);
    } else {
        claim.row_->state.store(ArmState::Started, std::memory_order_release);
    }
    claim.row_->reservation.clear(std::memory_order_release);
    claim.row_ = nullptr;
}

bool TimelineArmTable::AcknowledgeLogicalComplete(
    TimelineArmToken token,
    std::uint64_t completionSequence) noexcept {
    if (completionSequence == 0 || !token.Valid() ||
        token.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[token.slot];
    ArmMutationAccess access(&row);
    if (!access || !IdentityMatches(row, token)) {
        return false;
    }
    const ArmState state = row.state.load(std::memory_order_acquire);
    if (state != ArmState::Started) {
        return false;
    }
    std::uint64_t expected = 0;
    if (row.logicalCompletionSequence.compare_exchange_strong(
            expected, completionSequence, std::memory_order_release,
            std::memory_order_relaxed)) {
        return true;
    }
    return expected != 0;
}

bool TimelineArmTable::RequestCancel(TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    auto* activeAudioLease = ActiveBoundAudioProofLease(row, token);
    if (activeAudioLease != nullptr &&
        InvalidateActiveBoundAudioProof(activeAudioLease)) return false;
    ArmMutationAccess access(row);
    if (!access ||
        (activeAudioLease == nullptr && FrontierAdmissionDefers(row, true))) {
        return false;
    }
    ArmState expected = row->state.load(std::memory_order_acquire);
    while (expected == ArmState::PendingApply || expected == ArmState::Armed ||
           expected == ArmState::Published) {
        if (row->state.compare_exchange_strong(
                expected, ArmState::CancelRequested,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return expected == ArmState::CancelRequested ||
           expected == ArmState::Tombstoned;
}

bool TimelineArmTable::RequestCancelNeverPublished(
    TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    ArmMutationAccess access(row);
    if (!access || !IdentityMatches(*row, token) ||
        row->renderSlot != token.renderSlot ||
        row->renderGeneration != token.renderGeneration ||
        !row->pendingImmediateStart || row->startBound ||
        row->frontierReaderPins.load(std::memory_order_acquire) != 0 ||
        row->frontierAdmission != nullptr ||
        row->frontierAdmissionLease != nullptr ||
        row->frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
        row->frontierOperation.Valid()) return false;
    if (const auto hook = neverPublishedCancelHook_.load(std::memory_order_acquire))
        hook(neverPublishedCancelHookContext_.load(std::memory_order_relaxed));
    ArmState expected = ArmState::PendingApply;
    if (row->state.compare_exchange_strong(
            expected, ArmState::CancelRequested,
            std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    expected = ArmState::Armed;
    return row->state.compare_exchange_strong(
        expected, ArmState::CancelRequested,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool TimelineArmTable::RequestCancelStartedVoice(TimelineArmToken token) noexcept {
    Row* row = Resolve(token);
    auto* activeAudioLease = ActiveBoundAudioProofLease(row, token);
    if (activeAudioLease != nullptr &&
        InvalidateActiveBoundAudioProof(activeAudioLease)) return false;
    ArmMutationAccess access(row);
    return access &&
        (activeAudioLease != nullptr || !FrontierAdmissionDefers(row, true)) &&
        Transition(row, ArmState::Started, ArmState::CancelRequested);
}

bool TimelineArmTable::Tombstone(TimelineArmToken token,
                                 std::uint64_t audioSequence) noexcept {
    Row* row = Resolve(token);
    auto* activeAudioLease = ActiveBoundAudioProofLease(row, token);
    if (activeAudioLease != nullptr &&
        InvalidateActiveBoundAudioProof(activeAudioLease)) return false;
    ArmMutationAccess access(row);
    if (!access || !IdentityMatches(*row, token) ||
        row->renderSlot != token.renderSlot || row->renderGeneration != token.renderGeneration ||
        (activeAudioLease == nullptr && FrontierAdmissionDefers(row, true))) {
        return false;
    }
    return TombstoneReservedRow(row, audioSequence);
}

std::uint32_t TimelineArmTable::LiveCount() const noexcept {
    std::uint32_t live = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const ArmState state =
            rows_[slot].state.load(std::memory_order_acquire);
        if (state != ArmState::Free && state != ArmState::Tombstoned &&
            state != ArmState::Quarantined) {
            ++live;
        }
    }
    return live;
}

bool TimelineArmTable::State(TimelineArmToken token,
                             ArmState* outState) const noexcept {
    if (outState == nullptr || !token.Valid() || token.slot >= capacity_) {
        return false;
    }
    const Row& row = rows_[token.slot];
    const ArmState state = row.state.load(std::memory_order_acquire);
    if (state == ArmState::Free || state == ArmState::Quarantined ||
        !IdentityMatches(row, token)) {
        return false;
    }
    *outState = state;
    return true;
}

bool TimelineArmTable::AckSnapshot(
    TimelineArmToken token, ArmState* outState,
    std::uint64_t* outCompletionSequence) const noexcept {
    if (outState == nullptr || outCompletionSequence == nullptr ||
        !token.Valid() || token.slot >= capacity_) {
        return false;
    }
    const Row& row = rows_[token.slot];
    const ArmState state = row.state.load(std::memory_order_acquire);
    if (state == ArmState::Free || state == ArmState::Quarantined ||
        !IdentityMatches(row, token)) {
        return false;
    }
    *outState = state;
    *outCompletionSequence = row.armedCompletionSequence.load(
        std::memory_order_relaxed);
    return true;
}

bool TimelineArmTable::Observe(
    TimelineArmToken token,
    TimelineArmObservation* out) const noexcept {
    if (out == nullptr || !token.Valid() || token.slot >= capacity_) {
        return false;
    }
    const Row& row = rows_[token.slot];
    const ArmState state = row.state.load(std::memory_order_acquire);
    if (state == ArmState::Free || state == ArmState::Quarantined ||
        !IdentityMatches(row, token)) {
        return false;
    }
    TimelineArmObservation observation{};
    observation.state = state;
    observation.armedCompletionSequence =
        row.armedCompletionSequence.load(std::memory_order_acquire);
    observation.startedBlockSequence =
        row.startedBlockSequence.load(std::memory_order_acquire);
    observation.logicalCompletionSequence =
        row.logicalCompletionSequence.load(std::memory_order_acquire);
    observation.tombstoneAudioSequence =
        row.tombstoneAudioSequence.load(std::memory_order_acquire);
    observation.parentGroup = row.parentGroup;
    observation.memberIndex = row.memberIndex;
    *out = observation;
    return true;
}

bool TimelineArmTable::Snapshot(
    TimelineArmToken token, ArmState* outState,
    TimelineSemanticTarget* outTarget) noexcept {
    if (outState == nullptr || outTarget == nullptr || !token.Valid() ||
        token.slot >= capacity_) {
        return false;
    }
    Row& row = rows_[token.slot];
    if (row.reservation.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    const ArmState state = row.state.load(std::memory_order_acquire);
    const bool valid = state != ArmState::Free &&
                       state != ArmState::Quarantined &&
                       IdentityMatches(row, token);
    if (valid) {
        *outState = state;
        *outTarget = row.target;
    }
    row.reservation.clear(std::memory_order_release);
    return valid;
}

bool TimelineArmTable::HasUnpublishedTombstoneExact(TimelineArmToken token,
    ParentAudioGroupRef parent, std::uint16_t memberIndex) noexcept {
    if (!token.Valid() || token.slot >= capacity_ || !parent.Valid() ||
        memberIndex == UINT16_MAX) return false;
    Row& row = rows_[token.slot];
    ArmMutationAccess access(&row);
    return access && IdentityMatches(row, token) &&
        row.renderSlot == token.renderSlot && row.renderGeneration == token.renderGeneration &&
        row.state.load(std::memory_order_acquire) == ArmState::Tombstoned &&
        row.parentGroup.decision.slot == parent.decision.slot &&
        row.parentGroup.decision.generation == parent.decision.generation && row.memberIndex == memberIndex &&
        row.armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        row.logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.tombstoneAudioSequence.load(std::memory_order_acquire) == 0 &&
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row.frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row.frontierAdmission == nullptr && row.frontierAdmissionLease == nullptr &&
        !row.frontierOperation.Valid();
}

bool TimelineArmTable::HasUnobservedTombstoneExact(TimelineArmToken token,
    ParentAudioGroupRef parent, std::uint16_t memberIndex) noexcept {
    if (!token.Valid() || token.slot >= capacity_ || !parent.Valid() ||
        memberIndex == UINT16_MAX) return false;
    Row& row = rows_[token.slot];
    ArmMutationAccess access(&row);
    const auto& scope = groupMemberScopes_[token.slot];
    return access && IdentityMatches(row, token) &&
        row.renderSlot == token.renderSlot && row.renderGeneration == token.renderGeneration &&
        row.state.load(std::memory_order_acquire) == ArmState::Tombstoned &&
        row.parentGroup.decision.slot == parent.decision.slot &&
        row.parentGroup.decision.generation == parent.decision.generation && row.memberIndex == memberIndex &&
        !scope.jointHeld && (!SameOriginalGroupArm(scope.arm, token) || scope.activeCount == 0) &&
        !row.originalLifetimePinned.load(std::memory_order_acquire) &&
        row.armedCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.startedBlockSequence.load(std::memory_order_acquire) == 0 &&
        row.logicalCompletionSequence.load(std::memory_order_acquire) == 0 &&
        row.frontierReaderPins.load(std::memory_order_acquire) == 0 &&
        row.frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        row.frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row.frontierAdmission == nullptr && row.frontierAdmissionLease == nullptr &&
        !row.frontierOperation.Valid() && !row.pendingControlSourceReader.Valid() &&
        !row.originalPlainControl.Valid() && !row.appliedPlainStopSource.Valid() &&
        row.authoredControl.operation == nullptr && row.authoredControl.stage == AuthoredControlStage::None;
}

bool TimelineArmTable::HasLiveGeneration(
    ProgramGeneration generation) const noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        const Row& row = rows_[slot];
        const ArmState state = row.state.load(std::memory_order_acquire);
        if (state == ArmState::Free || state == ArmState::Tombstoned ||
            state == ArmState::Quarantined) {
            continue;
        }
        if (row.programGeneration == generation) {
            return true;
        }
    }
    return false;
}

std::uint32_t TimelineArmTable::ReclaimTombstoned(
    std::uint64_t observedAudioSequence) noexcept {
    std::uint32_t reclaimed = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.state.load(std::memory_order_acquire) != ArmState::Tombstoned ||
            observedAudioSequence <= row.tombstoneAudioSequence.load(
                                         std::memory_order_acquire)) {
            continue;
        }
        const std::uint64_t identity =
            row.identity.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(identity) != slot) {
            continue;
        }
        const TimelineArmToken token{
            slot, static_cast<std::uint32_t>(identity >> 32), row.renderSlot,
            row.renderGeneration};
        reclaimed += Reclaim(token, observedAudioSequence) ? 1u : 0u;
    }
    return reclaimed;
}

bool TimelineArmTable::Reclaim(
    TimelineArmToken token, std::uint64_t observedAudioSequence) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        return false;
    }
    Row* row = &rows_[token.slot];
    if (!IdentityMatches(*row, token) ||
        row->reservation.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    ArmState expected = ArmState::Tombstoned;
    if (!IdentityMatches(*row, token) ||
        row->state.load(std::memory_order_acquire) != ArmState::Tombstoned ||
        row->frontierReaderPins.load(std::memory_order_acquire) != 0 ||
        row->originalLifetimePinned.load(std::memory_order_acquire) ||
        observedAudioSequence <= row->tombstoneAudioSequence.load(
                                     std::memory_order_acquire)) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    if (!ReleasePendingControlSourceReader(*row, token, observedAudioSequence)) {
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    if (token.generation == UINT32_MAX) {
        const bool quarantined = row->state.compare_exchange_strong(
            expected, ArmState::Quarantined, std::memory_order_acq_rel,
            std::memory_order_acquire);
        if (quarantined) {
            row->authoredControl = {};
            row->identity.store(0, std::memory_order_release);
        }
        row->reservation.clear(std::memory_order_release);
        return false;
    }
    const bool freed = row->state.compare_exchange_strong(
        expected, ArmState::Free, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (freed) {
        row->identity.store(0, std::memory_order_release);
        row->generation = token.generation + 1;
        row->renderSlot = UINT32_MAX;
        row->renderGeneration = 0;
        row->startFrame = 0;
        row->armDeadline = 0;
        row->pendingImmediateStart = false;
        row->startBound = true;
        row->programGeneration = 0;
        row->target = {};
        row->parentGroup = {};
        row->memberIndex = UINT16_MAX;
        row->authoredControl = {};
        row->tombstoneAudioSequence.store(0, std::memory_order_relaxed);
        row->armedCompletionSequence.store(0, std::memory_order_relaxed);
        row->startedBlockSequence.store(0, std::memory_order_relaxed);
        row->logicalCompletionSequence.store(0,
                                             std::memory_order_relaxed);
    }
    row->reservation.clear(std::memory_order_release);
    return freed;
}

} // namespace harken
