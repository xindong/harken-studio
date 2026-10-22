#ifndef HARKEN_CORE_FRONTIER_ACTIVATION_H
#define HARKEN_CORE_FRONTIER_ACTIVATION_H

#include "harken/core/AudioSliceLease.h"
#include "harken/core/AuthoredStopOperationIdentity.h"
#include "harken/core/Plan03GroupIdentity.h"
#include "harken/core/OccurrenceToken.h"
#include "harken/core/TimelineExecutionOrder.h"
#include "harken/core/VariableReadCut.h"

#include <atomic>

namespace harken {

class FrontierTimelineFinalizer;

struct FrontierOwnerIdentity {
    std::uint64_t child = 0;
    std::uint64_t childEpoch = 0;
    std::uint64_t parent = 0;
    std::uint64_t parentEpoch = 0;
    std::uint64_t invocation = 0;
    std::uint64_t invocationEpoch = 0;
    std::uint32_t serviceSlot = UINT32_MAX;
    std::uint64_t serviceGeneration = 0;

    bool Valid() const noexcept {
        return child != 0 && childEpoch != 0 && invocation != 0 && invocationEpoch != 0 &&
            ((parent == 0 && parentEpoch == 0) || (parent != 0 && parentEpoch != 0)) &&
            serviceSlot != UINT32_MAX && serviceGeneration != 0;
    }
    bool operator==(const FrontierOwnerIdentity&) const = default;
};

struct FrontierEntryReceiptRef {
    DecisionToken coordinator{};
    std::uint64_t attempt = 0;
    FrontierOwnerIdentity owner{};

    bool Valid() const noexcept { return coordinator.Valid() && attempt != 0 && owner.Valid(); }
    bool operator==(const FrontierEntryReceiptRef& other) const noexcept {
        return coordinator.slot == other.coordinator.slot &&
            coordinator.generation == other.coordinator.generation &&
            attempt == other.attempt && owner == other.owner;
    }
};

struct FrontierActivationDescriptor {
    FrontierEntryReceiptRef entry{};
    Plan03GroupSliceIdentity primary{};
    TimelineExecutionOrder order{};
    std::uint64_t worldGeneration = 0;
    std::uint64_t worldSemanticFingerprint = 0;
    std::uint32_t expectedMemberCount = 0;
    std::uint32_t expectedEntryEventCount = 0;
    std::uint64_t programGeneration = 0;
    VariableReadCutRef initialVariables{};

    bool operator==(const FrontierActivationDescriptor&) const = default;

    bool Valid() const noexcept {
        return entry.Valid() && primary.Exact() &&
            primary.decision.slot == entry.coordinator.slot &&
            primary.decision.generation == entry.coordinator.generation &&
            order.logicCommitSequence != 0 && worldGeneration != 0 &&
            worldSemanticFingerprint != 0 && expectedEntryEventCount != 0 &&
            programGeneration != 0;
    }
};

struct TimelineProgramView;

enum class FrontierOperationKind : std::uint8_t { Invalid, Entry, Dependent, Genuine };

struct FrontierDueOperationRef {
    FrontierOperationKind kind = FrontierOperationKind::Invalid;
    FrontierOwnerIdentity owner{};
    TimelineExecutionOrder order{};
    Plan03GroupSliceIdentity group{};
    TransportEpoch transport = kInvalidTransportEpoch;
    std::uint64_t world = 0;
    std::uint64_t transactionId = 0;
    std::uint32_t transactionSlot = UINT32_MAX;
    const TimelineProgramView* program = nullptr;
    std::uint64_t programGeneration = 0;
    std::uint32_t armSlot = UINT32_MAX;
    std::uint32_t armGeneration = 0;
    std::uint32_t renderSlot = UINT32_MAX;
    std::uint32_t renderGeneration = 0;

    bool Valid() const noexcept {
        if (kind == FrontierOperationKind::Invalid || !owner.Valid() ||
            order.logicCommitSequence == 0 || transport == kInvalidTransportEpoch || world == 0) return false;
        if (group.Valid()) return group.Exact() && group.transportEpoch == transport;
        return transactionId != 0 && transactionSlot != UINT32_MAX &&
            program != nullptr && programGeneration != 0 && armSlot != UINT32_MAX &&
            armGeneration != 0 && renderSlot != UINT32_MAX && renderGeneration != 0;
    }
};

enum class FrontierEventState : std::uint8_t {
    Legacy, Pending, Applied, Rejected, LaneApplied
};

struct FrontierEventBinding {
    FrontierOperationKind kind = FrontierOperationKind::Invalid;
    FrontierEntryReceiptRef entry{};
    FrontierOwnerIdentity owner{};
    Plan03GroupSliceIdentity group{};
    TransportEpoch transport = kInvalidTransportEpoch;
    std::uint64_t world = 0;
    std::uint32_t operationOrdinal = 0;
    std::uint32_t operationEventCount = 0;
    std::uint32_t entryEventOrdinal = UINT32_MAX;

    bool Present() const noexcept {
        return kind != FrontierOperationKind::Invalid || entry.coordinator.Valid() || owner.child != 0 ||
            transport != kInvalidTransportEpoch || world != 0 || operationEventCount != 0 || group.Valid();
    }
    bool Valid() const noexcept {
        return (kind == FrontierOperationKind::Entry || kind == FrontierOperationKind::Dependent ||
            kind == FrontierOperationKind::Genuine) && entry.Valid() && owner.Valid() &&
            transport != kInvalidTransportEpoch && world != 0 && operationEventCount != 0 &&
            (!group.Valid() || (group.Exact() && group.transportEpoch == transport)) &&
            (kind != FrontierOperationKind::Entry || entryEventOrdinal != UINT32_MAX);
    }
    bool operator==(const FrontierEventBinding&) const = default;
};

enum class FrontierGroupRole : std::uint8_t { Legacy, Coordinator, EntryMember, Future };

struct FrontierVariableSideIdentity {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint8_t side = UINT8_MAX;
    std::uint64_t sideGeneration = 0;
    std::uint32_t bankSlot = UINT32_MAX;
    std::uint32_t bankGeneration = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0 && side < 2 &&
            sideGeneration != 0 && bankSlot != UINT32_MAX && bankGeneration != 0;
    }
    bool operator==(const FrontierVariableSideIdentity&) const = default;
};

struct FrontierImmediateLaneMemberIdentity {
    std::size_t committedRow = SIZE_MAX;
    TransportEpoch transportEpoch = kInvalidTransportEpoch;
    DecisionToken decision{};
    std::uint32_t rowClaimGeneration = 0;
    std::uint16_t sliceIndex = 0;

    bool Valid() const noexcept {
        return committedRow != SIZE_MAX &&
            transportEpoch != kInvalidTransportEpoch && decision.Valid() &&
            rowClaimGeneration != 0;
    }
    bool Empty() const noexcept {
        return *this == FrontierImmediateLaneMemberIdentity{};
    }
    Plan03GroupSliceIdentity Expand() const noexcept {
        if (!Valid()) return {};
        Plan03GroupSliceIdentity expanded{};
        expanded.absoluteQuantum = 0;
        expanded.pageGeneration = 1;
        expanded.decision = decision;
        expanded.sliceIndex = sliceIndex;
        expanded.committedRow = committedRow;
        expanded.rowClaimGeneration = rowClaimGeneration;
        expanded.transportEpoch = transportEpoch;
        expanded.backing = Plan03GroupBacking::ImmediateLane;
        return expanded;
    }
    bool Capture(Plan03GroupSliceIdentity identity) noexcept {
        if (!identity.Exact() ||
            identity.backing != Plan03GroupBacking::ImmediateLane ||
            identity.absoluteQuantum != 0 || identity.pageGeneration != 1)
            return false;
        FrontierImmediateLaneMemberIdentity candidate{};
        candidate.committedRow = identity.committedRow;
        candidate.transportEpoch = identity.transportEpoch;
        candidate.decision = identity.decision;
        candidate.rowClaimGeneration = identity.rowClaimGeneration;
        candidate.sliceIndex = identity.sliceIndex;
        if (!(candidate.Expand() == identity)) return false;
        *this = candidate;
        return true;
    }
    bool operator==(const FrontierImmediateLaneMemberIdentity& other) const noexcept {
        return committedRow == other.committedRow &&
            transportEpoch == other.transportEpoch &&
            decision.slot == other.decision.slot &&
            decision.generation == other.decision.generation &&
            rowClaimGeneration == other.rowClaimGeneration &&
            sliceIndex == other.sliceIndex;
    }
};

struct FrontierQueueDescendantIdentity {
    FrontierImmediateLaneMemberIdentity selectedSideMember{};
    FrontierVariableSideIdentity selectedSide{};

    bool Valid() const noexcept {
        return selectedSideMember.Valid() && selectedSide.Valid();
    }
    bool Empty() const noexcept {
        return selectedSideMember.Empty() &&
            selectedSide == FrontierVariableSideIdentity{};
    }
    bool Freeze(Plan03GroupSliceIdentity member,
                FrontierVariableSideIdentity side) noexcept {
        if (!side.Valid()) return false;
        FrontierImmediateLaneMemberIdentity compact{};
        if (!compact.Capture(member)) return false;
        selectedSideMember = compact;
        selectedSide = side;
        return true;
    }
    bool operator==(const FrontierQueueDescendantIdentity&) const = default;
};

struct FrontierQueueInitialIdentity {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint64_t version = 0;
    std::uint64_t programSemanticFingerprint = 0;
    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0 && version != 0 && programSemanticFingerprint != 0;
    }
    bool operator==(const FrontierQueueInitialIdentity&) const = default;
};

struct FrontierQueueConditionalIdentity {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint64_t currentVersion = 0;
    std::uint64_t pendingVersion = 0;
    OccurrenceToken current{};
    OccurrenceToken successor{};
    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0 && currentVersion != 0 && pendingVersion > currentVersion &&
            current.slot != UINT32_MAX && current.generation != 0 && successor.slot != UINT32_MAX && successor.generation != 0;
    }
};

struct FrontierTransitionIdentity {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint64_t audibleFrame = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

struct FrontierGroupBinding {
    FrontierGroupRole role = FrontierGroupRole::Legacy;
    FrontierEntryReceiptRef entry{};
    FrontierEntryReceiptRef prerequisite{};
    Plan03GroupSliceIdentity prerequisiteMember{};
    FrontierDueOperationRef operation{};
    Plan03GroupSliceIdentity primary{};
    FrontierVariableSideIdentity variableSide{};
    FrontierQueueDescendantIdentity queueDescendant{};
    FrontierQueueInitialIdentity queueInitial{};
    FrontierQueueConditionalIdentity queueConditional{};
    FrontierQueueConditionalIdentity queueInitialCompletion{};
    bool canonicalInitialCompletion = false;
    FrontierTransitionIdentity transition{};
    VariableReadCutRef initialVariables{};

    bool Valid() const noexcept {
        if (role == FrontierGroupRole::Legacy || !entry.Valid() || !operation.Valid() ||
            !operation.group.Exact()) return false;
        if (initialVariables.Valid() && role != FrontierGroupRole::Coordinator) return false;
        if (canonicalInitialCompletion) {
            if (role != FrontierGroupRole::Future || !queueInitial.Valid() ||
                queueConditional.Valid() || variableSide.Valid() ||
                !queueInitialCompletion.Valid() ||
                operation.group.backing != Plan03GroupBacking::ImmediateLane ||
                operation.group.sliceIndex != 1u ||
                queueInitialCompletion.slot != queueInitial.slot ||
                queueInitialCompletion.generation != queueInitial.generation ||
                queueInitialCompletion.currentVersion != queueInitial.version) return false;
        } else if (queueInitialCompletion.slot != UINT32_MAX ||
                   queueInitialCompletion.generation != 0 ||
                   queueInitialCompletion.currentVersion != 0 ||
                   queueInitialCompletion.pendingVersion != 0 ||
                   queueInitialCompletion.current.slot != OccurrenceToken{}.slot ||
                   queueInitialCompletion.current.generation != 0 ||
                   queueInitialCompletion.successor.slot != OccurrenceToken{}.slot ||
                   queueInitialCompletion.successor.generation != 0) return false;
        if (role == FrontierGroupRole::Future)
            return operation.kind == FrontierOperationKind::Dependent ||
                operation.kind == FrontierOperationKind::Genuine;
        const bool kindValid = operation.kind == FrontierOperationKind::Entry ||
            (role == FrontierGroupRole::EntryMember &&
             operation.kind == FrontierOperationKind::Dependent);
        return kindValid && primary.Exact() &&
            primary.decision.slot == entry.coordinator.slot &&
            primary.decision.generation == entry.coordinator.generation;
    }
};

// Copied by value into the original span before worker dispatch. No scratch reference
// or mutable receipt is reachable through this proof, including on abandoned jobs.
struct FrozenFrontierEventProof {
    FrontierEventState state = FrontierEventState::Legacy;
private:
    friend class VoicePool;
    // Successful staging records the origin; full identities stay with their receipt owners.
    enum class Origin : std::uint8_t { Unstaged, Page, Lane };
    Origin origin_ = Origin::Unstaged;
public:
    std::uint64_t quantum = 0;
    std::uint32_t renderGeneration = 0;
    std::uint64_t cutoffFrame = UINT64_MAX;
    AuthoredStopOperationIdentity cutoffOperation{};

    bool Eligible(std::uint64_t currentQuantum, std::uint32_t generation) const noexcept {
        return state == FrontierEventState::Legacy ||
            (state == FrontierEventState::LaneApplied &&
             origin_ == Origin::Lane &&
             quantum == currentQuantum && renderGeneration == generation) ||
            (state == FrontierEventState::Applied && quantum == currentQuantum &&
             renderGeneration == generation && origin_ == Origin::Page);
    }
};

enum class FrontierReceiptState : std::uint8_t {
    Empty, Prepared, Pending, AudioStaging, AudioFinal, LogicFinal, PublicationHeld
};
enum class FrontierReceiptOutcome : std::uint8_t { Applied, DefinitivelyNotApplied };
enum class FrontierReceiptOrigin : std::uint8_t { Audio, LogicBeforeAudio };
enum class FrontierReceiptReason : std::uint8_t {
    None, Cancelled, Deadline, Identity, MissingMember, DuplicateMember,
    Binding, AppendCapacity, Predicate, Dependency, AuthoredStop
};
enum class FrontierReceiptRead : std::uint8_t { InFlight, Ready, Stale };
enum class FrontierCancelDisposition : std::uint8_t {
    CancelledBeforeAudio, DeferredToAudio, AppliedNeedsLifecycle, AlreadyCancelled, Stale
};

struct FrontierActivationReceipt {
    FrontierEntryReceiptRef entry{};
    TimelineExecutionOrder order{};
    FrontierReceiptOutcome outcome = FrontierReceiptOutcome::DefinitivelyNotApplied;
    FrontierReceiptOrigin origin = FrontierReceiptOrigin::Audio;
    FrontierReceiptReason reason = FrontierReceiptReason::None;
    std::uint64_t audioSequence = 0;
    std::uint32_t failingMember = UINT32_MAX;
    AuthoredStopOperationIdentity stop{};
};

// The page lease arbitrates execution; this state only chooses the receipt writer.
// Storage and the lease are pinned by DecisionTable until explicit reconciliation.
class FrontierActivation {
public:
    const FrontierActivationDescriptor& Descriptor() const noexcept { return descriptor_; }
    FrontierReceiptState State() const noexcept { return state_.load(std::memory_order_acquire); }
    std::uint64_t SourceWaitSequence(const FrontierEntryReceiptRef& entry,
                                    std::uint64_t completedAudioSequence) const noexcept {
        if (!(descriptor_.entry == entry) || State() != FrontierReceiptState::Pending ||
            lease_ == nullptr || lease_->Capture().state != AudioSliceState::Ready) return 0;
        const auto sequence = sourceWaitSequence_.load(std::memory_order_acquire);
        return sequence <= completedAudioSequence ? sequence : 0;
    }
    bool TryBeginAudio(const FrontierEntryReceiptRef& entry) noexcept {
        if (!(descriptor_.entry == entry)) return false;
        auto expected = FrontierReceiptState::Pending;
        return state_.compare_exchange_strong(expected, FrontierReceiptState::AudioStaging,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
    FrontierCancelDisposition Cancel(const FrontierEntryReceiptRef& entry,
                                    AudioSliceInvalidateReason reason) noexcept {
        const auto state = State();
        if (!(descriptor_.entry == entry) || lease_ == nullptr || state == FrontierReceiptState::Prepared ||
            state == FrontierReceiptState::PublicationHeld) {
            return FrontierCancelDisposition::Stale;
        }
        const auto disposition = lease_->Invalidate(reason);
        if (disposition == AudioSliceInvalidateDisposition::InvalidReason) {
            return FrontierCancelDisposition::Stale;
        }
        if (disposition == AudioSliceInvalidateDisposition::DeferredAfterClaim) {
            return FrontierCancelDisposition::DeferredToAudio;
        }
        if (disposition == AudioSliceInvalidateDisposition::AppliedNeedsLifecycle) {
            return FrontierCancelDisposition::AppliedNeedsLifecycle;
        }
        auto expected = FrontierReceiptState::Pending;
        if (state_.compare_exchange_strong(expected, FrontierReceiptState::LogicFinal,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            receipt_.entry = descriptor_.entry;
            receipt_.order = descriptor_.order;
            receipt_.origin = FrontierReceiptOrigin::LogicBeforeAudio;
            receipt_.reason = reason == AudioSliceInvalidateReason::Deadline ?
                FrontierReceiptReason::Deadline : FrontierReceiptReason::Cancelled;
            logicPayloadReady_.store(true, std::memory_order_release);
            return FrontierCancelDisposition::CancelledBeforeAudio;
        }
        return expected == FrontierReceiptState::AudioStaging ?
            FrontierCancelDisposition::DeferredToAudio :
            expected == FrontierReceiptState::LogicFinal || expected == FrontierReceiptState::AudioFinal ?
                FrontierCancelDisposition::AlreadyCancelled : FrontierCancelDisposition::Stale;
    }
    bool FinishAudio(const FrontierEntryReceiptRef& entry, FrontierReceiptOutcome outcome,
                     FrontierReceiptReason reason, std::uint64_t audioSequence,
                     std::uint32_t failingMember = UINT32_MAX,
                     AuthoredStopOperationIdentity stop = {}) noexcept {
        if (!(descriptor_.entry == entry) || lease_ == nullptr || audioSequence == 0 ||
            State() != FrontierReceiptState::AudioStaging) return false;
        const auto leaseState = lease_->Capture().state;
        if ((outcome == FrontierReceiptOutcome::Applied &&
                (leaseState != AudioSliceState::SliceApplied || reason != FrontierReceiptReason::None)) ||
            (outcome == FrontierReceiptOutcome::DefinitivelyNotApplied &&
                (leaseState != AudioSliceState::SliceInvalidated || reason == FrontierReceiptReason::None)) ||
            (reason == FrontierReceiptReason::AuthoredStop && !stop.Valid())) return false;
        receipt_ = {descriptor_.entry, descriptor_.order, outcome, FrontierReceiptOrigin::Audio,
                    reason, audioSequence, failingMember, stop};
        state_.store(FrontierReceiptState::AudioFinal, std::memory_order_release);
        return true;
    }
    FrontierReceiptRead Read(const FrontierEntryReceiptRef& entry,
                             std::uint64_t completedAudioSequence,
                             FrontierActivationReceipt* out) const noexcept {
        if (out == nullptr || !(descriptor_.entry == entry)) return FrontierReceiptRead::Stale;
        const auto state = State();
        if (state == FrontierReceiptState::LogicFinal) {
            if (!logicPayloadReady_.load(std::memory_order_acquire)) return FrontierReceiptRead::InFlight;
        } else if (state == FrontierReceiptState::AudioFinal) {
            if (receipt_.audioSequence > completedAudioSequence) return FrontierReceiptRead::InFlight;
        } else {
            return state == FrontierReceiptState::Empty ? FrontierReceiptRead::Stale : FrontierReceiptRead::InFlight;
        }
        *out = receipt_;
        return FrontierReceiptRead::Ready;
    }

private:
    friend class DecisionTable;
    friend class FrontierTimelineFinalizer;
    void PublishSourceWait(const FrontierEntryReceiptRef& entry, std::uint64_t sequence) noexcept {
        if (!(descriptor_.entry == entry) || sequence == 0 || State() != FrontierReceiptState::Pending ||
            lease_ == nullptr || lease_->Capture().state != AudioSliceState::Ready) return;
        if (sourceWaitSequence_.load(std::memory_order_relaxed) == 0)
            sourceWaitSequence_.store(sequence, std::memory_order_release);
    }
    bool Prepare(const FrontierActivationDescriptor& descriptor, AudioSliceLease* lease) noexcept {
        if (State() != FrontierReceiptState::Empty || !descriptor.Valid() || lease == nullptr ||
            lease->Capture().state != AudioSliceState::Ready) return false;
        descriptor_ = descriptor;
        receipt_ = {};
        lease_ = lease;
        sourceWaitSequence_.store(0, std::memory_order_relaxed);
        logicPayloadReady_.store(false, std::memory_order_relaxed);
        state_.store(FrontierReceiptState::Prepared, std::memory_order_release);
        return true;
    }
    FrontierActivationDescriptor descriptor_{};
    FrontierActivationReceipt receipt_{};
    AudioSliceLease* lease_ = nullptr;
    std::atomic<FrontierReceiptState> state_{FrontierReceiptState::Empty};
    std::atomic<bool> logicPayloadReady_{false};
    std::atomic<std::uint64_t> sourceWaitSequence_{0};
};

static_assert(std::atomic<FrontierReceiptState>::is_always_lock_free);

} // namespace harken

#endif // HARKEN_CORE_FRONTIER_ACTIVATION_H
