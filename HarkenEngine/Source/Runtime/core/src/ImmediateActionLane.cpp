#include "harken/core/ImmediateActionLane.h"
#include "harken/core/QueueContinuationLane.h"
#include "harken/core/OriginalImmediateLaneConditionalProjection.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace harken {
namespace {

constexpr std::uint8_t kEntryStopRequested = 1u;
constexpr std::uint8_t kEntryAudioAdmitting = 2u;
constexpr std::uint8_t kEventWithdrawalRequested = 1u;
constexpr std::uint8_t kEventAudioAdmitting = 2u;
constexpr std::uint8_t kEventReferenceReleased = 4u;
constexpr std::uint64_t kProjectionAttached = 4u;
constexpr std::uint64_t kProjectionChanging = 8u;
constexpr std::uint64_t kProjectionWritten = 16u;
constexpr std::uint64_t kProjectionStamp = 32u;
constexpr std::uint64_t kProjectionFlags = kProjectionStamp - 1u;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
    "Original conditional admission must remain lock-free");

class AttemptReadAccess {
public:
    explicit AttemptReadAccess(const ImmediateLaneAttempt& attempt) noexcept
        : gate_(attempt.readerGate.test_and_set(std::memory_order_acquire)
                    ? nullptr : &attempt.readerGate) {}
    ~AttemptReadAccess() { if (gate_ != nullptr) gate_->clear(std::memory_order_release); }
    explicit operator bool() const noexcept { return gate_ != nullptr; }
    void Detach() noexcept { gate_ = nullptr; }
private:
    std::atomic_flag* gate_;
};

template <typename T>
bool ValidTimeSpec(const T& value) noexcept {
    if (value.timeDomain == ImmediateLaneTimeDomain::RelativeToAnchor)
        return value.relativeFrame != UINT64_MAX && value.fixedFrame == 0;
    if (value.timeDomain == ImmediateLaneTimeDomain::FixedAbsoluteFrame)
        return value.relativeFrame == 0 && value.fixedFrame != UINT64_MAX;
    return false;
}

template <typename T>
std::uint64_t TimeSpecValue(const T& value) noexcept {
    return value.timeDomain == ImmediateLaneTimeDomain::RelativeToAnchor
        ? value.relativeFrame : value.fixedFrame;
}

template <typename A, typename B>
bool SameTimeSpec(const A& a, const B& b) noexcept {
    return ValidTimeSpec(a) && ValidTimeSpec(b) && a.timeDomain == b.timeDomain &&
        TimeSpecValue(a) == TimeSpecValue(b);
}

bool ValidMemberEventOrdinals(const ImmediateLaneMemberManifest& member,
    const ImmediateLaneEvent* events, std::uint32_t eventCount,
    std::uint32_t quantumFrames) noexcept {
    if (events == nullptr || quantumFrames == 0u || member.eventOrdinal >= eventCount)
        return false;
    if (member.eventSpanCount == 1u)
        return member.secondEventOrdinal == UINT32_MAX;
    if (member.eventSpanCount != 2u || member.secondEventOrdinal <= member.eventOrdinal ||
        member.secondEventOrdinal >= eventCount) return false;
    return events[member.eventOrdinal].event.executeFrame / quantumFrames ==
        events[member.secondEventOrdinal].event.executeFrame / quantumFrames;
}

template <typename T>
bool ResolveLaneDue(const T& value, std::uint64_t anchor, std::uint64_t* due) noexcept {
    if (due == nullptr || !ValidTimeSpec(value)) return false;
    if (value.timeDomain == ImmediateLaneTimeDomain::FixedAbsoluteFrame) {
        *due = value.fixedFrame;
        return true;
    }
    if (value.relativeFrame > UINT64_MAX - anchor) return false;
    *due = anchor + value.relativeFrame;
    return true;
}

bool LessLaneExecutionOrder(const ImmediateLaneEvent& a,
                   const ImmediateLaneEvent& b) noexcept {
    const auto& x = a.event;
    const auto& y = b.event;
    if (x.logicCommitSequence != y.logicCommitSequence)
        return x.logicCommitSequence < y.logicCommitSequence;
    if (x.executionOrdinal != y.executionOrdinal)
        return x.executionOrdinal < y.executionOrdinal;
    if (x.eventIndex != y.eventIndex) return x.eventIndex < y.eventIndex;
    if (x.transactionOrdinal != y.transactionOrdinal)
        return x.transactionOrdinal < y.transactionOrdinal;
    if (x.subeventPhase != y.subeventPhase)
        return x.subeventPhase < y.subeventPhase;
    return x.rowSequence < y.rowSequence;
}

bool LessLaneEvent(const ImmediateLaneEvent& a,
                   const ImmediateLaneEvent& b) noexcept {
    if (TimeSpecValue(a) != TimeSpecValue(b)) return TimeSpecValue(a) < TimeSpecValue(b);
    return LessLaneExecutionOrder(a, b);
}

bool Terminal(ImmediateLaneBucketState state) noexcept {
    return state == ImmediateLaneBucketState::Applied ||
           state == ImmediateLaneBucketState::Cancelled ||
           state == ImmediateLaneBucketState::Missed ||
           state == ImmediateLaneBucketState::CancelledOnly;
}

bool SameOccurrence(OccurrenceToken a, OccurrenceToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}

bool SameArm(TimelineArmToken a, TimelineArmToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation &&
           a.renderSlot == b.renderSlot &&
           a.renderGeneration == b.renderGeneration;
}

} // namespace

PrivatePlainLayoutEdit::~PrivatePlainLayoutEdit() { Abort(); }

PrivatePlainLayoutEdit::PrivatePlainLayoutEdit(PrivatePlainLayoutEdit&& other) noexcept {
    *this = std::move(other);
}

PrivatePlainLayoutEdit& PrivatePlainLayoutEdit::operator=(
    PrivatePlainLayoutEdit&& other) noexcept {
    if (this == &other) return *this;
    Abort();
    owner_ = other.owner_;
    handle_ = other.handle_;
    originalState_ = other.originalState_;
    originalEventCount_ = other.originalEventCount_;
    originalBucketCount_ = other.originalBucketCount_;
    originalNextBucket_ = other.originalNextBucket_;
    originalAnchor_ = other.originalAnchor_;
    originalEvents_ = other.originalEvents_;
    originalBuckets_ = other.originalBuckets_;
    authoredLayout_ = other.authoredLayout_;
    hasAuthoredLayout_ = other.hasAuthoredLayout_;
    originalWasPristineReserved_ = other.originalWasPristineReserved_;
    ready_ = other.ready_;
    if (owner_ != nullptr)
        owner_->attempts_[handle_.slot].privateLayoutEdit = this;
    other.owner_ = nullptr;
    other.ready_ = false;
    return *this;
}

void PrivatePlainLayoutEdit::Abort() noexcept {
    if (owner_ != nullptr) owner_->FinishPrivateLayout(*this, false);
}

void PrivatePlainLayoutEdit::ClearCandidate() noexcept {
    if (owner_ == nullptr) return;
    ready_ = false;
    auto& attempt = owner_->attempts_[handle_.slot];
    auto* buckets = owner_->BucketBase(handle_.slot);
    for (std::uint32_t i = 0; i < attempt.bucketCredit; ++i) {
        auto& bucket = buckets[i];
        bucket.state.store(ImmediateLaneBucketState::Empty, std::memory_order_relaxed);
        bucket.admissionFailure.store(ImmediateLaneAdmissionFailure::None,
                                       std::memory_order_relaxed);
        bucket.attemptGeneration = 0;
        bucket.quantum = 0;
        bucket.sourceWaitSequence.store(0, std::memory_order_relaxed);
        bucket.completedAudioSequence.store(0, std::memory_order_relaxed);
        bucket.firstEvent = bucket.eventCount = 0;
        bucket.firstGroup = bucket.groupCount = bucket.memberCount = 0;
    }
    attempt.eventCount = 0;
    attempt.bucketCount = 0;
    attempt.anchorFrame = 0;
    attempt.nextBucket.store(0, std::memory_order_relaxed);
}

bool PrivatePlainLayoutEdit::StageCandidate(const ImmediateLaneEvent* events,
    std::uint32_t count, std::uint64_t anchorFrame,
    std::uint64_t firstSafeFrame) noexcept {
    if (hasAuthoredLayout_) {
        ClearCandidate();
        return false;
    }
    return StageFixedCandidate(events, count, anchorFrame, firstSafeFrame);
}

bool PrivatePlainLayoutEdit::StageAuthoredControlCandidate(
    std::uint64_t anchorFrame, std::uint64_t firstSafeFrame) noexcept {
    if (owner_ == nullptr) return false;
    ClearCandidate();
    if (!hasAuthoredLayout_) return false;
    std::uint64_t onset = 0;
    if (!ResolveLaneDue(authoredLayout_.start, anchorFrame, &onset) ||
        onset == UINT64_MAX) return false;
    const auto fixed = [](ImmediateLaneEvent event, std::uint64_t due) noexcept {
        event.timeDomain = ImmediateLaneTimeDomain::FixedAbsoluteFrame;
        event.relativeFrame = 0;
        event.fixedFrame = due;
        event.event.executeFrame = due;
        return event;
    };
    const bool hasEnvelope = authoredLayout_.fadeStartFrame < authoredLayout_.stopFrame &&
        onset < authoredLayout_.stopFrame;
    std::array<ImmediateLaneEvent, 3> events{};
    std::uint32_t count = 0;
    events[count++] = fixed(authoredLayout_.start, onset);
    if (hasEnvelope) {
        const auto dispatch = std::min(authoredLayout_.stopFrame,
            std::max(authoredLayout_.fadeStartFrame, onset));
        events[count++] = fixed(authoredLayout_.envelope, dispatch);
    }
    auto stop = fixed(authoredLayout_.stop, authoredLayout_.stopFrame);
    stop.event.authoredFadeOutCandidate = hasEnvelope;
    events[count++] = stop;
    return StageFixedCandidate(events.data(), count, anchorFrame, firstSafeFrame);
}

bool PrivatePlainLayoutEdit::StageFixedCandidate(const ImmediateLaneEvent* events,
    std::uint32_t count, std::uint64_t anchorFrame,
    std::uint64_t firstSafeFrame) noexcept {
    if (owner_ == nullptr) return false;
    ClearCandidate();
    auto& attempt = owner_->attempts_[handle_.slot];
    if (events == nullptr || count == 0 || count > attempt.eventCredit ||
        anchorFrame == UINT64_MAX || firstSafeFrame == UINT64_MAX ||
        anchorFrame < firstSafeFrame ||
        firstSafeFrame % owner_->config_.quantumFrames != 0 ||
        anchorFrame % owner_->config_.quantumFrames != 0) return false;
    for (std::uint32_t i = 0; i < count; ++i)
        if (!ValidTimeSpec(events[i]) ||
            events[i].timeDomain != ImmediateLaneTimeDomain::FixedAbsoluteFrame ||
            events[i].event.program.program == nullptr ||
            events[i].futureGroupedStart || events[i].event.frontier.Present()) return false;
    auto* destination = owner_->EventBase(handle_.slot);
    std::copy_n(events, count, destination);
    std::sort(destination, destination + count, LessLaneEvent);
    attempt.eventCount = count;
    if (!owner_->CandidateFits(handle_.slot, anchorFrame, firstSafeFrame)) {
        ClearCandidate();
        return false;
    }
    owner_->InstallPlacement(handle_, anchorFrame);
    ready_ = true;
    return true;
}

bool PrivatePlainLayoutEdit::SealCandidate(PrivatePlainLayoutCommitProof* out) noexcept {
    if (!Valid() || !ready_ || out == nullptr || out->edit_.Valid()) return false;
    out->edit_ = std::move(*this);
    return true;
}

void PrivatePlainLayoutCommitProof::Consume() noexcept {
    if (Valid()) edit_.owner_->FinishPrivateLayout(edit_, true);
}

PrivateImmediateLanePlacementProof::PrivateImmediateLanePlacementProof(
    PrivateImmediateLanePlacementProof&& other) noexcept { Take(other); }
PrivateImmediateLanePlacementProof& PrivateImmediateLanePlacementProof::operator=(
    PrivateImmediateLanePlacementProof&& other) noexcept {
    if (this != &other) { Release(); Take(other); }
    return *this;
}
PrivateImmediateLanePlacementProof::~PrivateImmediateLanePlacementProof() { Release(); }
void PrivateImmediateLanePlacementProof::Take(PrivateImmediateLanePlacementProof& other) noexcept {
    if (other.owner_ != nullptr) assert(other.owner_->MatchesPrivatePlacementProof(other));
    owner_ = std::exchange(other.owner_, nullptr);
    attempt_ = std::exchange(other.attempt_, nullptr);
    handle_ = std::exchange(other.handle_, ImmediateLaneHandle{});
    root_ = std::exchange(other.root_, 0);
    anchor_ = std::exchange(other.anchor_, 0);
    if (attempt_ != nullptr) {
        assert(attempt_->privatePlacementProof == &other);
        attempt_->privatePlacementProof = this;
    }
}
bool PrivateImmediateLanePlacementProof::Valid() const noexcept {
    return owner_ != nullptr && owner_->MatchesPrivatePlacementProof(*this);
}
bool PrivateImmediateLanePlacementProof::CaptureAnchorFrame(std::uint64_t* out) const noexcept {
    if (out == nullptr || !Valid()) return false;
    *out = anchor_;
    return true;
}

bool ImmediateActionLane::CapturePrivatePendingVariableStartGeometry(
    const PrivatePlainLayoutCommitProof& proof, const TimelineArmTable& arms,
    TimelineArmToken arm, std::uint64_t authoredOffset,
    std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept {
    const auto& edit = proof.edit_;
    const auto* attempt = edit.owner_ == this ? Resolve(edit.handle_) : nullptr;
    if (!proof.Valid() || attempt == nullptr || attempt->privateLayoutEdit != &edit ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::PrivateLayoutHeld)
        return false;
    return CapturePrivatePendingVariableStartGeometryImpl(edit.handle_, arms, arm,
        authoredOffset, logicalFrame, onsetFrame);
}

bool ImmediateActionLane::CapturePrivatePendingVariableStartGeometry(
    const PrivateImmediateLanePlacementProof& proof, const TimelineArmTable& arms,
    TimelineArmToken arm, std::uint64_t authoredOffset,
    std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept {
    return MatchesPrivatePlacementProof(proof) && CapturePrivatePendingVariableStartGeometryImpl(
        proof.handle_, arms, arm, authoredOffset, logicalFrame, onsetFrame);
}

bool ImmediateActionLane::CapturePrivatePreparedVariableStartGeometry(
    const PrivateImmediateLanePlacementProof& placement,
    const TimelineArmTable::PreparedCommitProof& proof, std::uint64_t authoredOffset,
    std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept {
    if (!MatchesPrivatePlacementProof(placement) || !proof.Valid() || proof.Owner() == nullptr ||
        logicalFrame == nullptr || onsetFrame == nullptr || authoredOffset > UINT64_MAX - placement.anchor_)
        return false;
    const auto token = proof.OriginalPreparedToken();
    const auto* native = proof.Owner()->Resolve(token);
    const auto* attempt = Resolve(placement.handle_);
    if (native == nullptr || !native->pendingImmediateStart || native->startBound ||
        native->state.load(std::memory_order_acquire) != ArmState::Prepared ||
        native->startedBlockSequence.load(std::memory_order_acquire) != 0 ||
        native->frontierOperation.Valid() || native->frontierAdmission != nullptr ||
        native->frontierAdmissionLease != nullptr ||
        native->frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
        attempt->eventCount == 0 || attempt->eventCount > config_.eventsPerAttempt) return false;
    const auto* events = EventBase(placement.handle_.slot);
    std::uint32_t found = 0;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& item = events[index];
        const auto& event = item.event;
        if (!SameArm(event.armToken, token)) continue;
        const auto* program = event.program.program;
        if (program == nullptr || program->payloads == nullptr || event.payload >= program->payloadCount ||
            program->generation != event.program.programGeneration ||
            program->generation != native->programGeneration) return false;
        const auto& payload = program->payloads[event.payload];
        std::uint64_t due = 0;
        if (payload.kind != TimelineEventKind::Start || ++found != 1 || payload.target != native->target ||
            item.timeDomain != ImmediateLaneTimeDomain::RelativeToAnchor || item.relativeFrame != authoredOffset ||
            item.futureGroupedStart != (authoredOffset != 0) ||
            !ResolveLaneDue(item, placement.anchor_, &due) || event.executeFrame != due ||
            due != placement.anchor_ + authoredOffset) return false;
    }
    if (found != 1) return false;
    *logicalFrame = placement.anchor_;
    *onsetFrame = placement.anchor_ + authoredOffset;
    return true;
}

bool ImmediateActionLane::CapturePrivateUnboundVariableStartGeometry(
    const PrivateImmediateLanePlacementProof& proof, const TimelineArmTable& arms,
    TimelineArmToken arm, std::uint64_t authoredOffset,
    std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept {
    return MatchesPrivatePlacementProof(proof) && CapturePrivatePendingVariableStartGeometryImpl(
        proof.handle_, arms, arm, authoredOffset, logicalFrame, onsetFrame, true);
}

bool ImmediateActionLane::CapturePrivatePendingVariableStartGeometryImpl(ImmediateLaneHandle handle,
    const TimelineArmTable& arms, TimelineArmToken arm, std::uint64_t authoredOffset,
    std::uint64_t* logicalFrame, std::uint64_t* onsetFrame, bool unbound) const noexcept {
    const auto* attempt = Resolve(handle);
    const auto* native = arms.Resolve(arm);
    if (attempt == nullptr || native == nullptr || logicalFrame == nullptr || onsetFrame == nullptr ||
        !native->pendingImmediateStart || native->startBound == unbound ||
        native->state.load(std::memory_order_acquire) != ArmState::Armed ||
        (unbound && (native->startedBlockSequence.load(std::memory_order_acquire) != 0 ||
            native->frontierOperation.Valid() || native->frontierAdmission != nullptr ||
            native->frontierAdmissionLease != nullptr ||
            native->frontierActiveLease.load(std::memory_order_acquire) != nullptr)) ||
        attempt->eventCount == 0 || attempt->eventCount > config_.eventsPerAttempt) {
        return false;
    }
    const auto* events = EventBase(handle.slot);
    std::uint32_t found = 0;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& item = events[index];
        const auto& event = item.event;
        if (!SameArm(event.armToken, arm)) continue;
        const auto* program = event.program.program;
        if (program == nullptr || program->payloads == nullptr || event.payload >= program->payloadCount ||
            program->generation != event.program.programGeneration ||
            (unbound ? event.program.programGeneration != native->programGeneration
                     : item.armProgramGeneration != native->programGeneration)) return false;
        const auto& payload = program->payloads[event.payload];
        if (payload.kind != TimelineEventKind::Start) continue;
        std::uint64_t due = 0;
        if (++found != 1 || payload.target != native->target ||
            !ResolveLaneDue(item, attempt->anchorFrame, &due) || event.executeFrame != due ||
            (!unbound && due != native->startFrame) || due < authoredOffset ||
            (unbound && (item.futureGroupedStart != (authoredOffset != 0) ||
                item.timeDomain != ImmediateLaneTimeDomain::RelativeToAnchor)) ||
            (item.timeDomain == ImmediateLaneTimeDomain::RelativeToAnchor &&
                item.relativeFrame != authoredOffset)) {
            return false;
        }
        *logicalFrame = due - authoredOffset;
        *onsetFrame = due;
    }
    return found == 1;
}
void PrivateImmediateLanePlacementProof::Release() noexcept {
    if (owner_ != nullptr) owner_->ReturnPrivatePlacementProof(*this);
    owner_ = nullptr;
    attempt_ = nullptr;
    handle_ = {};
    root_ = 0;
    anchor_ = 0;
}
bool ImmediateActionLane::MatchesPrivatePlacementProof(
    const PrivateImmediateLanePlacementProof& proof) const noexcept {
    if (proof.owner_ != this || !proof.handle_.Valid() || proof.root_ == 0) return false;
    const auto* attempt = Resolve(proof.handle_);
    return attempt != nullptr && proof.attempt_ == attempt &&
        attempt->privatePlacementProof == &proof && attempt->privateLayoutEdit == nullptr &&
        attempt->root == proof.root_ && attempt->anchorFrame == proof.anchor_ &&
        attempt->bucketCount != 0 &&
        attempt->state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Prepared;
}
bool ImmediateActionLane::IsPrivatePlacementProofFor(
    const PrivateImmediateLanePlacementProof& proof,
    ImmediateLaneHandle handle, std::uint64_t root) const noexcept {
    return MatchesPrivatePlacementProof(proof) && proof.handle_.slot == handle.slot &&
        proof.handle_.generation == handle.generation && proof.root_ == root;
}
void ImmediateActionLane::ReturnPrivatePlacementProof(
    PrivateImmediateLanePlacementProof& proof) noexcept {
    const bool exact = MatchesPrivatePlacementProof(proof);
    assert(exact);
    if (exact) proof.attempt_->privatePlacementProof = nullptr;
}

bool ImmediateActionLane::CapturePrivatePlacedEvent(const PrivateImmediateLanePlacementProof& proof,
    std::uint32_t ordinal, ImmediateLaneEvent* out) const noexcept {
    const auto* attempt = Resolve(proof.handle_);
    if (out == nullptr || attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || !MatchesPrivatePlacementProof(proof) || ordinal >= attempt->eventCount) return false;
    *out = EventBase(proof.handle_.slot)[ordinal];
    return true;
}

ImmediateActionLane::~ImmediateActionLane() {
    while (eventStorageConsumers_ != nullptr) eventStorageConsumers_->Detach();
    assert(payloadGrowth_.heldRows == 0 && payloadGrowth_.preparedUsers == 0);
    for (std::uint32_t i = 0; i < attempts_.size(); ++i)
        assert(attempts_[i].originalLifetimePins == 0);
    for (std::uint32_t i = 0; i < attempts_.size(); ++i) {
        // Return the exact unpublished short proof before destroying storage.
        if (attempts_[i].privatePublicationProof != nullptr)
            attempts_[i].privatePublicationProof->Release();
        if (attempts_[i].privatePlacementProof != nullptr)
            attempts_[i].privatePlacementProof->Release();
        if (attempts_[i].privateLayoutEdit != nullptr)
            attempts_[i].privateLayoutEdit->Abort();
    }
}

bool ImmediateActionLane::AcquirePrivatePlainLayoutEdit(ImmediateLaneHandle handle,
    std::uint64_t root, ImmediateLaneDemand originalDemand,
    PrivatePlainLayoutEdit* out) noexcept {
    return AcquirePrivatePlainLayoutEditImpl(handle, root, originalDemand, out, false);
}

bool ImmediateActionLane::AcquirePrivateReservedPlainLayoutEdit(ImmediateLaneHandle handle,
    std::uint64_t root, ImmediateLaneDemand originalDemand,
    PrivatePlainLayoutEdit* out) noexcept {
    return AcquirePrivatePlainLayoutEditImpl(handle, root, originalDemand, out, true);
}

bool ImmediateActionLane::AcquirePrivatePlainLayoutEditImpl(ImmediateLaneHandle handle,
    std::uint64_t root, ImmediateLaneDemand originalDemand,
    PrivatePlainLayoutEdit* out, bool pristineReserved) noexcept {
    if (out == nullptr || out->Valid()) return false;
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    const auto initial = attempt->state.load(std::memory_order_acquire);
    if (initial != ImmediateLaneAttemptState::Reserved &&
        initial != ImmediateLaneAttemptState::Prepared) return false;
    if (pristineReserved && initial != ImmediateLaneAttemptState::Reserved) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt) return false;
    const auto state = attempt->state.load(std::memory_order_acquire);
    if ((state != ImmediateLaneAttemptState::Reserved &&
         state != ImmediateLaneAttemptState::Prepared) ||
        root == 0 || attempt->root != root ||
        attempt->eventCredit != originalDemand.events ||
        attempt->bucketCredit != originalDemand.buckets ||
        originalDemand.events == 0 ||
        (!pristineReserved && originalDemand.events > 3) ||
        originalDemand.buckets == 0 ||
        (!pristineReserved && originalDemand.buckets > 3) ||
        originalDemand.groups != 0 || originalDemand.members != 0 ||
        attempt->groupCredit != 0 || attempt->memberCredit != 0 ||
        attempt->groupCount != 0 || attempt->declaredGroupCount != 0 ||
        attempt->memberCount != 0 || attempt->privateEntryCount != 0 ||
        attempt->pinGroupManifestCount != 0 || attempt->pinGroupPool != nullptr ||
        attempt->pinsInstalled || attempt->groupPinsInstalled ||
        attempt->privateLayoutEdit != nullptr || attempt->privatePlacementProof != nullptr ||
        attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->capturedBucketOrdinal != UINT32_MAX) return false;
    if (pristineReserved && (state != ImmediateLaneAttemptState::Reserved ||
        attempt->eventCount != 0 || attempt->bucketCount != 0 ||
        attempt->anchorFrame != 0 ||
        attempt->nextBucket.load(std::memory_order_acquire) != 0)) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    for (std::uint32_t i = 0; i < metadata_[handle.slot].active.demand.events; ++i)
        if (eventState_[base + i].arm.Valid() || eventState_[base + i].fence.Valid()) return false;
    for (std::uint32_t i = 0; i < metadata_[handle.slot].active.demand.groups; ++i)
        if (groupState_[base + i].decision.Valid() || groupState_[base + i].futureOwned != 0) return false;
    for (std::uint32_t i = 0; i < metadata_[handle.slot].active.demand.groups; ++i)
        if (groupPins_[base + i].Valid()) return false;
    for (std::uint32_t i = 0; i < attempt->eventCount; ++i)
        if (EventBase(handle.slot)[i].futureGroupedStart ||
            EventBase(handle.slot)[i].event.frontier.Present()) return false;
    for (std::uint32_t i = 0; i < attempt->bucketCount; ++i)
        if (BucketBase(handle.slot)[i].state.load(std::memory_order_acquire) !=
            ImmediateLaneBucketState::Prepared) return false;
    out->handle_ = handle;
    out->originalState_ = state;
    out->originalEventCount_ = attempt->eventCount;
    out->originalBucketCount_ = attempt->bucketCount;
    out->originalNextBucket_ = attempt->nextBucket.load(std::memory_order_relaxed);
    out->originalAnchor_ = attempt->anchorFrame;
    out->hasAuthoredLayout_ = false;
    out->originalWasPristineReserved_ = pristineReserved;
    if (!pristineReserved) {
        std::copy_n(EventBase(handle.slot), attempt->eventCredit, out->originalEvents_.begin());
        for (std::uint32_t i = 0; i < attempt->bucketCredit; ++i) {
            const auto& b = BucketBase(handle.slot)[i];
            out->originalBuckets_[i] = {b.state.load(std::memory_order_relaxed),
                b.admissionFailure.load(std::memory_order_relaxed), b.attemptGeneration,
                b.quantum, b.firstEvent, b.eventCount, b.firstGroup, b.groupCount, b.memberCount};
        }
    }
    out->owner_ = this;
    out->ready_ = false;
    attempt->privateLayoutEdit = out;
    attempt->state.store(ImmediateLaneAttemptState::PrivateLayoutHeld,
                          std::memory_order_release);
    access.Detach();
    return true;
}

bool ImmediateActionLane::AcquirePrivatePlainAuthoredControlLayoutEdit(
    ImmediateLaneHandle handle, std::uint64_t root, ImmediateLaneDemand originalDemand,
    const PlainAuthoredControlLayoutInput& input, PrivatePlainLayoutEdit* out) noexcept {
    const auto plain = [](const ImmediateLaneEvent& event) noexcept {
        return ValidTimeSpec(event) && event.event.program.program != nullptr &&
            !event.futureGroupedStart && !event.event.frontier.Present();
    };
    const auto worstCase = input.fadeStartFrame < input.stopFrame ? 3u : 2u;
    if (input.stopFrame == UINT64_MAX || input.fadeStartFrame == UINT64_MAX ||
        !plain(input.start) || !plain(input.envelope) || !plain(input.stop) ||
        input.envelope.timeDomain != ImmediateLaneTimeDomain::FixedAbsoluteFrame ||
        input.stop.timeDomain != ImmediateLaneTimeDomain::FixedAbsoluteFrame ||
        input.stop.fixedFrame != input.stopFrame ||
        input.envelope.event.subeventPhase != 30 || input.stop.event.subeventPhase != 50 ||
        originalDemand.events < worstCase || originalDemand.buckets < worstCase ||
        !AcquirePrivatePlainLayoutEdit(handle, root, originalDemand, out)) return false;
    out->authoredLayout_ = input;
    out->hasAuthoredLayout_ = true;
    return true;
}

void ImmediateActionLane::FinishPrivateLayout(PrivatePlainLayoutEdit& edit,
    bool commit) noexcept {
    auto& attempt = attempts_[edit.handle_.slot];
    if (!commit && edit.originalWasPristineReserved_) {
        edit.ClearCandidate();
    } else if (!commit) {
        std::copy_n(edit.originalEvents_.begin(), attempt.eventCredit,
                    EventBase(edit.handle_.slot));
        for (std::uint32_t i = 0; i < attempt.bucketCredit; ++i) {
            auto& b = BucketBase(edit.handle_.slot)[i];
            const auto& saved = edit.originalBuckets_[i];
            b.state.store(saved.state, std::memory_order_relaxed);
            b.admissionFailure.store(saved.failure, std::memory_order_relaxed);
            b.attemptGeneration = saved.generation;
            b.quantum = saved.quantum;
            b.firstEvent = saved.firstEvent;
            b.eventCount = saved.eventCount;
            b.firstGroup = saved.firstGroup;
            b.groupCount = saved.groupCount;
            b.memberCount = saved.memberCount;
        }
        attempt.eventCount = edit.originalEventCount_;
        attempt.bucketCount = edit.originalBucketCount_;
        attempt.anchorFrame = edit.originalAnchor_;
        attempt.nextBucket.store(edit.originalNextBucket_, std::memory_order_relaxed);
    }
    attempt.privateLayoutEdit = nullptr;
    attempt.state.store(commit ? ImmediateLaneAttemptState::Prepared : edit.originalState_,
                         std::memory_order_release);
    edit.owner_ = nullptr;
    edit.ready_ = false;
    attempt.readerGate.clear(std::memory_order_release);
}

OriginalImmediateLaneAttemptLifetimePin::OriginalImmediateLaneAttemptLifetimePin(
    OriginalImmediateLaneAttemptLifetimePin&& other) noexcept { *this = std::move(other); }
OriginalImmediateLaneAttemptLifetimePin& OriginalImmediateLaneAttemptLifetimePin::operator=(
    OriginalImmediateLaneAttemptLifetimePin&& other) noexcept {
    if (this != &other) {
        Return();
        lane_ = std::exchange(other.lane_, nullptr);
        handle_ = std::exchange(other.handle_, ImmediateLaneHandle{});
        root_ = std::exchange(other.root_, 0);
        marker_ = std::exchange(other.marker_, 0);
    }
    return *this;
}
OriginalImmediateLaneAttemptLifetimePin::~OriginalImmediateLaneAttemptLifetimePin() { Return(); }
bool OriginalImmediateLaneAttemptLifetimePin::Valid() const noexcept {
    return lane_ != nullptr && lane_->MatchesOriginalAttempt(*this);
}
bool OriginalImmediateLaneAttemptLifetimePin::Borrow(
    OriginalImmediateLaneAttemptLifetimePin* out) const noexcept {
    return lane_ != nullptr && lane_->BorrowOriginalAttempt(*this, out);
}
void OriginalImmediateLaneAttemptLifetimePin::Return() noexcept {
    if (lane_ != nullptr) lane_->ReturnOriginalAttempt(*this);
}
bool ImmediateActionLane::MatchesOriginalAttempt(
    const OriginalImmediateLaneAttemptLifetimePin& original) const noexcept {
    const auto* row = Resolve(original.handle_);
    return original.lane_ == this && original.marker_ != 0 && row != nullptr &&
        row->root == original.root_ && row->originalLifetimeMarker == original.marker_ &&
        row->originalLifetimePins != 0 &&
        row->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Free;
}
bool ImmediateActionLane::IsOriginalAttemptLifetimePinFor(
    const OriginalImmediateLaneAttemptLifetimePin& original,
    ImmediateLaneHandle handle, std::uint64_t root) const noexcept {
    return MatchesOriginalAttempt(original) && original.root_ == root &&
        original.handle_.slot == handle.slot && original.handle_.generation == handle.generation;
}
bool ImmediateActionLane::ObserveOriginalUnpublishedStorageOnlyRetention(
    const OriginalImmediateLaneAttemptLifetimePin& baseline,
    ImmediateLaneHandle handle, std::uint64_t root,
    std::uint64_t completedAudioSequence) const noexcept {
    if (!IsOriginalAttemptLifetimePinFor(baseline, handle, root)) return false;
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt ||
        !IsOriginalAttemptLifetimePinFor(baseline, handle, root) ||
        attempt->originalLifetimePins <= 1u ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::PrivateCancelled ||
        attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->privatePlacementProof != nullptr || attempt->privateLayoutEdit != nullptr ||
        attempt->pinsInstalled || attempt->groupPinsInstalled ||
        attempt->pinGroupManifestCount != 0 ||
        attempt->conditionalProjectionIssued || attempt->conditionalProjectionBound ||
        attempt->conditionalProjectionAdmission.load(std::memory_order_acquire) != 0 ||
        attempt->nextBucket.load(std::memory_order_acquire) != attempt->bucketCount) return false;
    for (std::uint32_t i = 0; i < attempt->bucketCount; ++i)
        if (BucketBase(handle.slot)[i].state.load(std::memory_order_acquire) !=
            ImmediateLaneBucketState::Cancelled) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    for (std::uint32_t i = 0; i < metadata_[handle.slot].active.demand.groups; ++i)
        if (groupState_[base + i].decision.Valid() || groupState_[base + i].futureOwned != 0) return false;
    for (std::uint32_t i = 0; i < metadata_[handle.slot].active.demand.events; ++i) {
        if (eventState_[base + i].arm.Valid()) return false;
        const auto& fence = eventState_[base + i].fence;
        if (fence.Valid() && fence.lastReadSequence_ != 0 &&
            completedAudioSequence <= fence.lastReadSequence_) return false;
    }
    for (std::uint32_t i = 0; i < metadata_[handle.slot].active.demand.groups; ++i)
        if (groupPins_[base + i].Valid()) return false;
    return true;
}

bool ImmediateActionLane::BorrowOriginalAttempt(
    const OriginalImmediateLaneAttemptLifetimePin& original,
    OriginalImmediateLaneAttemptLifetimePin* out) noexcept {
    if (out == nullptr || out->lane_ != nullptr || !MatchesOriginalAttempt(original)) return false;
    auto& row = attempts_[original.handle_.slot];
    if (row.originalLifetimePins == UINT32_MAX) return false;
    ++row.originalLifetimePins;
    out->lane_ = this; out->handle_ = original.handle_;
    out->root_ = original.root_; out->marker_ = original.marker_;
    return true;
}
void ImmediateActionLane::ReturnOriginalAttempt(
    OriginalImmediateLaneAttemptLifetimePin& original) noexcept {
    assert(MatchesOriginalAttempt(original));
    if (!MatchesOriginalAttempt(original)) return;
    auto& row = attempts_[original.handle_.slot];
    --row.originalLifetimePins;
    original.lane_ = nullptr; original.handle_ = {};
    original.root_ = 0; original.marker_ = 0;
}

ImmediateActionLane::AttemptMetadataBacking::~AttemptMetadataBacking() {
    if (requestedBytes == 0) return;
    std::destroy_n(groups, demand.groups);
    std::destroy_n(declaredGroups, demand.groups);
    std::destroy_n(groupPins, demand.groups);
    std::destroy_n(eventState, demand.events);
    std::destroy_n(groupState, demand.groups);
    std::destroy_n(memberState, demand.members);
    std::destroy_n(sourceWaitEntries, sourceWaitCount);
    std::destroy_n(buckets, demand.buckets);
    std::destroy_n(privateEntryBuckets, privateBucketCount);
    std::destroy_n(waitingScratch, waitingScratchCount);
}

bool ImmediateActionLane::AttemptMetadataBacking::Initialize(ImmediateLaneDemand requested,
    const Config& config, std::size_t pageLimit) noexcept {
    assert(storage.Address() == nullptr);
    const auto entries = static_cast<std::uint64_t>(requested.events) + requested.groups;
    const auto waits = static_cast<std::uint32_t>(std::min<std::uint64_t>(config.eventsPerAttempt, entries));
    const auto scratch = static_cast<std::uint32_t>(std::min<std::uint64_t>(config.maxEventsPerQuantum, entries));
    const auto privateBuckets = PrivateBucketCapacity(requested);
    const auto columns = [&](auto&& visit) noexcept {
        return visit(groups, requested.groups) && visit(declaredGroups, requested.groups) &&
            visit(groupPins, requested.groups) && visit(eventState, requested.events) &&
            visit(groupState, requested.groups) && visit(memberState, requested.members) &&
            visit(sourceWaitEntries, waits) && visit(buckets, requested.buckets) &&
            visit(privateEntryBuckets, privateBuckets) && visit(waitingScratch, scratch);
    };
    std::size_t bytes = 0, alignment = 1;
    if (!columns([&]<class T>(T*&, std::size_t count) noexcept {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>);
        if (bytes > static_cast<std::size_t>(PTRDIFF_MAX) - (alignof(T) - 1u)) return false;
        bytes = (bytes + alignof(T) - 1u) & ~(alignof(T) - 1u);
        if (count > (static_cast<std::size_t>(PTRDIFF_MAX) - bytes) / sizeof(T)) return false;
        bytes += count * sizeof(T);
        alignment = std::max(alignment, alignof(T));
        return true;
    })) return false;
    storage.SetCommitPageLimitForTests(pageLimit);
    if (bytes == 0 || !storage.Reserve(bytes, alignment) || !storage.CommitThrough(bytes)) return false;
    std::size_t offset = 0;
    columns([&]<class T>(T*& column, std::size_t count) noexcept {
        offset = (offset + alignof(T) - 1u) & ~(alignof(T) - 1u);
        column = reinterpret_cast<T*>(static_cast<std::byte*>(storage.Address()) + offset);
        for (std::size_t index = 0; index < count; ++index) std::construct_at(column + index);
        offset += count * sizeof(T);
        return true;
    });
    demand = requested;
    sourceWaitCount = waits;
    waitingScratchCount = scratch;
    privateBucketCount = privateBuckets;
    requestedBytes = bytes;
    return true;
}

bool ImmediateActionLane::AttemptMetadataBacking::Fits(ImmediateLaneDemand requested,
    const Config& config) const noexcept {
    const auto entries = static_cast<std::uint64_t>(requested.events) + requested.groups;
    return requestedBytes != 0 && demand.events >= requested.events && demand.groups >= requested.groups &&
        demand.members >= requested.members && demand.buckets >= requested.buckets &&
        sourceWaitCount >= std::min<std::uint64_t>(config.eventsPerAttempt, entries) &&
        waitingScratchCount >= std::min<std::uint64_t>(config.maxEventsPerQuantum, entries) &&
        privateBucketCount >= PrivateBucketCapacity(requested);
}

void ImmediateActionLane::AttemptMetadataBacking::Swap(AttemptMetadataBacking& other) noexcept {
    storage.Swap(other.storage);
    std::swap(demand, other.demand);
    std::swap(sourceWaitCount, other.sourceWaitCount);
    std::swap(waitingScratchCount, other.waitingScratchCount);
    std::swap(privateBucketCount, other.privateBucketCount);
    std::swap(requestedBytes, other.requestedBytes);
    std::swap(groups, other.groups);
    std::swap(declaredGroups, other.declaredGroups);
    std::swap(groupPins, other.groupPins);
    std::swap(eventState, other.eventState);
    std::swap(groupState, other.groupState);
    std::swap(memberState, other.memberState);
    std::swap(sourceWaitEntries, other.sourceWaitEntries);
    std::swap(buckets, other.buckets);
    std::swap(privateEntryBuckets, other.privateEntryBuckets);
    std::swap(waitingScratch, other.waitingScratch);
}

std::size_t ImmediateActionLane::MetadataStorageBytes() const noexcept {
    auto bytes = attempts_.RequestedBytes() + metadata_.RequestedBytes();
    for (const auto& row : metadata_) bytes += row.active.requestedBytes + row.standby.requestedBytes;
    return bytes;
}

std::size_t ImmediateActionLane::MetadataCommittedBytes() const noexcept {
    auto bytes = attempts_.CommittedBytes() + metadata_.CommittedBytes();
    for (const auto& row : metadata_)
        bytes += row.active.storage.CommittedBytes() + row.standby.storage.CommittedBytes();
    return bytes;
}

std::size_t ImmediateActionLane::MetadataReservedBytes() const noexcept {
    auto bytes = attempts_.ReservedBytes() + metadata_.ReservedBytes();
    for (const auto& row : metadata_)
        bytes += row.active.storage.ReservedBytes() + row.standby.storage.ReservedBytes();
    return bytes;
}

std::size_t ImmediateActionLane::MetadataDirectoryBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& row : metadata_)
        bytes += row.active.storage.DirectoryBytes() + row.standby.storage.DirectoryBytes();
    return bytes;
}

bool ImmediateActionLane::Reset(Config config) {
    static_assert(std::is_trivially_destructible_v<PrivateEntryBucket>);
    static_assert(std::is_nothrow_default_constructible_v<PrivateEntryBucket>);
    static_assert(std::is_trivially_destructible_v<FrontierEntryReceiptRef>);
    static_assert(std::is_nothrow_default_constructible_v<FrontierEntryReceiptRef>);
    static_assert(std::is_trivially_destructible_v<ImmediateLaneEvent>);
    static_assert(std::is_trivially_destructible_v<ImmediateLaneMemberManifest>);
    static_assert(std::is_nothrow_default_constructible_v<ImmediateLaneEvent>);
    static_assert(std::is_nothrow_default_constructible_v<ImmediateLaneMemberManifest>);
    if (memguard::IsAudioTagged() || privateEntries_.HasPreparedBacking() ||
        payloadGrowth_.heldRows != 0 || payloadGrowth_.preparedUsers != 0) return false;
    for (std::uint32_t i = 0; i < attempts_.size(); ++i)
        if (attempts_[i].privateLayoutEdit != nullptr ||
            attempts_[i].privatePlacementProof != nullptr ||
            attempts_[i].privatePublicationProof != nullptr ||
            attempts_[i].originalLifetimePins != 0) return false;
    while (eventStorageConsumers_ != nullptr) eventStorageConsumers_->Detach();
    groupPool_ = nullptr;
    valid_ = false;
    StableRowStorage<AttemptMetadata> oldMetadata;
    metadata_.Swap(oldMetadata);
    detail::StablePageReservation oldEvents, oldMembers;
    eventStorage_.Swap(oldEvents);
    memberStorage_.Swap(oldMembers);
    eventStorageBytes_ = memberStorageBytes_ = 0;
    StableRowStorage<ImmediateLaneAttempt> oldAttempts;
    attempts_.Swap(oldAttempts);
    events_ = nullptr;
    eventPlacementOrder_.reset();
    groupPlacementOrder_.reset();
    members_ = nullptr;
    if (config.attempts == 0 || config.eventsPerAttempt == 0 ||
        config.maxEventsPerQuantum == 0 || config.quantumFrames == 0 ||
        config.maxEventsPerQuantum > static_cast<std::size_t>(PTRDIFF_MAX) /
            sizeof(FrontierEntryReceiptRef) / config.attempts ||
        config.eventsPerAttempt > SIZE_MAX / config.attempts ||
        static_cast<std::size_t>(config.attempts) * config.eventsPerAttempt > SIZE_MAX / 2u)
        return false;
    if (config.maxGroupsPerQuantum == 0)
        config.maxGroupsPerQuantum = config.eventsPerAttempt;
    const auto slots = static_cast<std::size_t>(config.attempts) *
                       config.eventsPerAttempt;
    StableRowStorage<PrivateEntryAssociation> privateEntries;
    if (slots > UINT32_MAX || !privateEntries.Reserve(static_cast<std::uint32_t>(slots)) ||
        !metadata_.Reserve(config.attempts)) return false;
    if (slots > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(ImmediateLaneEvent) ||
        slots > static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(ImmediateLaneMemberManifest) ||
        !eventStorage_.Reserve(slots * sizeof(ImmediateLaneEvent), alignof(ImmediateLaneEvent), true) ||
        !memberStorage_.Reserve(slots * sizeof(ImmediateLaneMemberManifest),
            alignof(ImmediateLaneMemberManifest), true)) return false;
    events_ = static_cast<ImmediateLaneEvent*>(eventStorage_.Address());
    members_ = static_cast<ImmediateLaneMemberManifest*>(memberStorage_.Address());
    if (!attempts_.Reserve(config.attempts)) return false;
    privateEntries_.Swap(privateEntries);
    config_ = config;
    valid_ = true;
    return true;
}

bool ImmediateActionLane::PreparePrivateEntryBacking(std::uint32_t count,
    StableStoragePreparation* out) noexcept {
    if (!valid_) return false;
    std::uint32_t occupied = 0;
    for (std::uint32_t index = 0; index < privateEntries_.size(); ++index)
        if (privateEntries_[index].inUse) occupied = index + 1u;
    return privateEntries_.PrepareBacking(std::min(count, privateEntries_.Capacity()), occupied, out);
}

void ImmediateActionLane::EventStorageRegistration::Detach() noexcept {
    if (owner_ == nullptr) return;
    if (previous_ != nullptr) previous_->next_ = next_;
    else owner_->eventStorageConsumers_ = next_;
    if (next_ != nullptr) next_->previous_ = previous_;
    owner_ = nullptr;
    previous_ = next_ = nullptr;
    context_ = nullptr;
    prepare_ = nullptr;
}

std::uint32_t ImmediateActionLane::EventStorageHighWater() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < attempts_.size(); ++slot)
        count = std::max(count, attempts_[slot].eventStorageCount);
    return count;
}

void ImmediateActionLane::AttachEventStorage(EventStorageRegistration& registration,
    void* context, EventStorageRegistration::Prepare prepare) const noexcept {
    assert(!memguard::IsAudioTagged() && Valid() && prepare != nullptr);
    registration.Detach();
    registration.owner_ = this;
    registration.context_ = context;
    registration.prepare_ = prepare;
    registration.next_ = eventStorageConsumers_;
    if (eventStorageConsumers_ != nullptr) eventStorageConsumers_->previous_ = &registration;
    eventStorageConsumers_ = &registration;
}

bool ImmediateActionLane::PrepareAttemptStorage(std::uint32_t slot,
    ImmediateLaneDemand demand) noexcept {
    auto& attempt = attempts_[slot];
    auto& metadata = metadata_[slot];
    const auto metadataReady = [&]() noexcept {
        return metadata.active.Fits(demand, config_) || metadata.standby.Fits(demand, config_);
    };
    if (demand.events <= attempt.eventStorageCount && demand.members <= attempt.memberStorageCount &&
        metadataReady()) return true;
    if (memguard::IsAudioTagged() || payloadGrowth_.preparedUsers != 0) return false;
    const auto base = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    if (demand.events > attempt.eventStorageCount) {
        for (auto* consumer = eventStorageConsumers_; consumer != nullptr; consumer = consumer->next_)
            if (!consumer->prepare_(consumer->context_, demand.events)) return false;
        if (!eventStorage_.CommitRange((base + attempt.eventStorageCount) * sizeof(ImmediateLaneEvent),
                (demand.events - attempt.eventStorageCount) * sizeof(ImmediateLaneEvent))) return false;
        for (auto index = attempt.eventStorageCount; index < demand.events; ++index)
            std::construct_at(events_ + base + index);
        eventStorageBytes_ += (demand.events - attempt.eventStorageCount) * sizeof(ImmediateLaneEvent);
        attempt.eventStorageCount = demand.events;
    }
    if (demand.members > attempt.memberStorageCount) {
        if (!memberStorage_.CommitRange((base + attempt.memberStorageCount) * sizeof(ImmediateLaneMemberManifest),
                (demand.members - attempt.memberStorageCount) * sizeof(ImmediateLaneMemberManifest))) return false;
        for (auto index = attempt.memberStorageCount; index < demand.members; ++index)
            std::construct_at(members_ + base + index);
        memberStorageBytes_ += (demand.members - attempt.memberStorageCount) * sizeof(ImmediateLaneMemberManifest);
        attempt.memberStorageCount = demand.members;
    }
    if (metadataReady()) return true;
    auto pageLimit = SIZE_MAX;
#if !defined(HARKEN_RT_QUALIFICATION)
    pageLimit = std::min(stateCommitPageLimit_, sourceWaitCommitPageLimit_);
    if (demand.groups != 0) pageLimit = std::min(pageLimit, groupCommitPageLimit_);
#endif
    AttemptMetadataBacking prepared;
    if (!prepared.Initialize(demand, config_, pageLimit)) return false;
    // Preparing a busy identity must leave every active reader's address unchanged.
    metadata.standby.Swap(prepared);
    return true;
}

bool ImmediateActionLane::InstallAttemptMetadata(std::uint32_t slot,
    ImmediateLaneDemand demand) noexcept {
    assert(attempts_[slot].state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Free);
    auto& metadata = metadata_[slot];
    if (!metadata.active.Fits(demand, config_)) {
        if (!metadata.standby.Fits(demand, config_)) return false;
        metadata.active.Swap(metadata.standby);
    }
    // A prepared use may exchange backing, but must never destroy the retired allocation.
    if (!memguard::IsAudioTagged() && payloadGrowth_.preparedUsers == 0 &&
        (payloadGrowth_.heldRows == 0 || metadata.active.Fits(FullAttemptDemand(), config_))) {
        AttemptMetadataBacking retired;
        metadata.standby.Swap(retired);
    }
    return true;
}

bool ImmediateActionLane::PrepareAttemptIdentityStorage() noexcept {
    if (attempts_.size() == attempts_.Capacity() && metadata_.size() == metadata_.Capacity()) return true;
    if (memguard::IsAudioTagged() || payloadGrowth_.preparedUsers != 0) return false;
    try {
        if (eventPlacementOrder_ == nullptr)
            eventPlacementOrder_ = std::make_unique<std::uint32_t[]>(config_.eventsPerAttempt);
        if (groupPlacementOrder_ == nullptr)
            groupPlacementOrder_ = std::make_unique<std::uint32_t[]>(config_.eventsPerAttempt);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return attempts_.ConstructThrough(attempts_.Capacity()) &&
        metadata_.ConstructThrough(metadata_.Capacity());
}

bool ImmediateActionLane::EnsureAttemptBacking(std::uint64_t count,
    std::uint32_t excluded) noexcept {
    if (count == 0) return true;
    if (!PrepareAttemptIdentityStorage()) return false;
    std::uint32_t available = 0;
    const auto free = [&](std::uint32_t slot) noexcept {
        return slot != excluded && attempts_[slot].generation != UINT32_MAX &&
            attempts_[slot].state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Free;
    };
    for (std::uint32_t slot = 0; slot < attempts_.size(); ++slot)
        if (free(slot)) ++available;
    // A hold exceeding current free identities must also survive later retirements.
    const bool all = count > available;
    for (std::uint32_t slot = 0; slot < attempts_.size() && (all || count != 0); ++slot) {
        if (!all && !free(slot)) continue;
        if (!PrepareAttemptStorage(slot, FullAttemptDemand())) return false;
        if (!all) --count;
    }
    return true;
}

bool ImmediateActionLane::PrepareAttemptBacking(StableStoragePreparation* out) noexcept {
    if (!valid_ || out == nullptr || out->Valid() || memguard::IsAudioTagged() ||
        payloadGrowth_.preparedUsers != 0 || payloadGrowth_.heldRows == UINT64_MAX ||
        !EnsureAttemptBacking(payloadGrowth_.heldRows + 1u)) return false;
    ++payloadGrowth_.heldRows;
    out->state_ = &payloadGrowth_;
    out->count_ = 1;
    return true;
}

ImmediateActionLane::PrivateEntryAssociation* ImmediateActionLane::AcquirePrivateEntry(
    std::size_t logical) noexcept {
    if (memberState_[logical].privateEntry != UINT32_MAX) return nullptr;
    std::uint32_t index = 0;
    while (index < privateEntries_.size() && privateEntries_[index].inUse) ++index;
    if (index == privateEntries_.Capacity() || !privateEntries_.ConstructThrough(index + 1u))
        return nullptr;
    auto& entry = privateEntries_[index];
    entry.inUse = true;
    memberState_[logical].privateEntry = index;
    return &entry;
}

void ImmediateActionLane::ReleasePrivateEntries(std::uint32_t slot) noexcept {
    const auto base = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    for (std::uint32_t ordinal = 0; ordinal < attempts_[slot].privateEntryCount; ++ordinal) {
        auto& index = memberState_[base + ordinal].privateEntry;
        assert(index < privateEntries_.size() && privateEntries_[index].inUse);
        privateEntries_[index].inUse = false;
        index = UINT32_MAX;
    }
}

ImmediateLaneAttempt* ImmediateActionLane::Resolve(
    ImmediateLaneHandle handle) noexcept {
    if (!valid_ || !handle.Valid() || handle.slot >= attempts_.size())
        return nullptr;
    auto& attempt = attempts_[handle.slot];
    return attempt.generation == handle.generation ? &attempt : nullptr;
}

const ImmediateLaneAttempt* ImmediateActionLane::Resolve(
    ImmediateLaneHandle handle) const noexcept {
    return const_cast<ImmediateActionLane*>(this)->Resolve(handle);
}

ImmediateLaneEvent* ImmediateActionLane::EventBase(
    std::uint32_t slot) noexcept {
    return events_ + static_cast<std::size_t>(slot) *
                               config_.eventsPerAttempt;
}

const ImmediateLaneEvent* ImmediateActionLane::EventBase(
    std::uint32_t slot) const noexcept {
    return const_cast<ImmediateActionLane*>(this)->EventBase(slot);
}

ImmediateLaneMemberManifest* ImmediateActionLane::MemberBase(std::uint32_t slot) noexcept {
    return members_ + static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
}

const ImmediateLaneMemberManifest* ImmediateActionLane::MemberBase(std::uint32_t slot) const noexcept {
    return const_cast<ImmediateActionLane*>(this)->MemberBase(slot);
}

ImmediateLaneGroupRef* ImmediateActionLane::GroupBase(
    std::uint32_t slot) noexcept {
    return metadata_[slot].active.groups;
}

const ImmediateLaneGroupRef* ImmediateActionLane::GroupBase(
    std::uint32_t slot) const noexcept {
    return const_cast<ImmediateActionLane*>(this)->GroupBase(slot);
}

ImmediateLaneBucket* ImmediateActionLane::BucketBase(
    std::uint32_t slot) noexcept {
    return metadata_[slot].active.buckets;
}

const ImmediateLaneBucket* ImmediateActionLane::BucketBase(
    std::uint32_t slot) const noexcept {
    return const_cast<ImmediateActionLane*>(this)->BucketBase(slot);
}

bool ImmediateActionLane::Reserve(std::uint64_t root,
    std::uint32_t eventCredit, ImmediateLaneHandle* out) noexcept {
    return Reserve(root, {eventCredit, eventCredit, eventCredit,
                          eventCredit}, out);
}

bool ImmediateActionLane::Reserve(std::uint64_t root,
    ImmediateLaneDemand demand, ImmediateLaneHandle* out) noexcept {
    return Reserve(root, demand, out, nullptr);
}

bool ImmediateActionLane::Reserve(std::uint64_t root,
    ImmediateLaneDemand demand, ImmediateLaneHandle* out,
    OriginalImmediateLaneAttemptLifetimePin* original) noexcept {
    if (original != nullptr && (original->lane_ != nullptr ||
        nextOriginalLifetimeMarker_ == UINT64_MAX)) return false;
    if (!valid_ || root == 0 || out == nullptr || out->Valid() ||
        (demand.events == 0 && demand.groups == 0) ||
        demand.events > config_.eventsPerAttempt ||
        demand.groups > config_.eventsPerAttempt ||
        demand.members > config_.eventsPerAttempt ||
        demand.buckets == 0 ||
        demand.buckets > config_.eventsPerAttempt)
        return false;
    if (!PrepareAttemptIdentityStorage()) return false;
    for (std::uint32_t slot = 0; slot < attempts_.size(); ++slot) {
        auto& attempt = attempts_[slot];
        // A producer seeking a free slot must never contend with the audio
        // reader of an unrelated committed attempt. CaptureDue has one chance
        // to claim its bucket in the due quantum.
        if (attempt.state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Free) continue;
        AttemptReadAccess access(attempt);
        if (!access) continue;
#if !defined(HARKEN_RT_QUALIFICATION)
        if (reserveGateHook_ != nullptr)
            reserveGateHook_(reserveGateContext_, slot);
#endif
        if (attempt.state.load(std::memory_order_acquire) !=
                ImmediateLaneAttemptState::Free ||
            attempt.generation == UINT32_MAX) continue;
        const auto storageDemand = payloadGrowth_.preparedUsers != 0 ? FullAttemptDemand() : demand;
        if (payloadGrowth_.preparedUsers != 0 &&
            (attempt.eventStorageCount != config_.eventsPerAttempt ||
             attempt.memberStorageCount != config_.eventsPerAttempt ||
             (!metadata_[slot].active.Fits(storageDemand, config_) &&
              !metadata_[slot].standby.Fits(storageDemand, config_)))) continue;
        if (!PrepareAttemptStorage(slot, storageDemand) || !InstallAttemptMetadata(slot, storageDemand) ||
            (payloadGrowth_.preparedUsers == 0 &&
                !EnsureAttemptBacking(payloadGrowth_.heldRows, slot))) return false;
        ++attempt.generation;
        attempt.root = root;
        attempt.queueContinuation = nullptr;
        attempt.sourceWaitEntryCount = 0;
        attempt.originalLifetimeMarker = 0;
        attempt.originalLifetimePins = 0;
        attempt.conditionalProjectionIssued = false;
        attempt.conditionalProjectionBound = false;
        attempt.conditionalProjectionCancelIssued = false;
        attempt.conditionalProjectionCancelled = false;
        attempt.conditionalProjectionReferencesReturned = false;
        attempt.conditionalProjectionGroups = {};
        attempt.conditionalProjectionAdmission.store(0, std::memory_order_relaxed);
        attempt.conditionalProjectionLastUse.store(0, std::memory_order_relaxed);
        attempt.capturedConditionalProjection = false;
        assert(attempt.conditionalReplacement == nullptr);
        attempt.conditionalReplacementGeneration = 0;
        attempt.capturedConditionalReplacementRead = false;
        attempt.conditionalProjectionDecisions = nullptr;
        attempt.conditionalProjectionEntry = {};
        for (std::uint32_t i = 0; i < demand.events; ++i)
            eventState_[static_cast<std::size_t>(slot) * config_.eventsPerAttempt + i].conditionalScope = 0;
        attempt.eventCredit = demand.events;
        attempt.groupCredit = demand.groups;
        attempt.memberCredit = demand.members;
        attempt.bucketCredit = demand.buckets;
        attempt.eventCount = 0;
        attempt.groupCount = 0;
        attempt.pinGroupManifestCount = 0;
        attempt.pinGroupPool = nullptr;
        attempt.declaredGroupCount = 0;
        attempt.memberCount = 0;
        attempt.privateEntryCount = 0;
        attempt.capturedBucketOrdinal = UINT32_MAX;
        attempt.bucketCount = 0;
        attempt.anchorFrame = 0;
        attempt.pinsInstalled = false;
        attempt.groupPinsInstalled = false;
        const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
        for (std::uint32_t index = 0; index < demand.events; ++index) {
            eventState_[offset + index].withdrawal.store(0, std::memory_order_relaxed);
            eventState_[offset + index].capturedAdmission = 0;
            eventState_[offset + index].withdrawalLeader = index;
            eventState_[offset + index].lastCaptureSequence = 0;
        }
        attempt.nextBucket.store(0, std::memory_order_relaxed);
        attempt.state.store(ImmediateLaneAttemptState::Reserved,
                            std::memory_order_release);
        *out = {slot, attempt.generation};
        if (original != nullptr) {
            attempt.originalLifetimeMarker = ++nextOriginalLifetimeMarker_;
            attempt.originalLifetimePins = 1;
            original->lane_ = this;
            original->handle_ = *out;
            original->root_ = root;
            original->marker_ = attempt.originalLifetimeMarker;
        }
        return true;
    }
    return false;
}

bool ImmediateActionLane::HasDue(std::uint64_t quantum) const noexcept {
    if (!valid_) return false;
    for (std::uint32_t slot = 0; slot < attempts_.size(); ++slot) {
        const auto& attempt = attempts_[slot];
        AttemptReadAccess access(attempt);
        if (!access) continue;
        const auto state = attempt.state.load(std::memory_order_acquire);
        if (state != ImmediateLaneAttemptState::Committed &&
            state != ImmediateLaneAttemptState::Cancelling) continue;
        const auto next = attempt.nextBucket.load(std::memory_order_acquire);
        if (next < attempt.bucketCount && BucketBase(slot)[next].quantum <= quantum)
            return true;
    }
    return false;
}

bool ImmediateActionLane::Observe(ImmediateLaneHandle handle,
    ImmediateLaneObservation* out) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || out == nullptr) return false;
    const auto state = attempt->state.load(std::memory_order_acquire);
    if (state == ImmediateLaneAttemptState::Free) return false;
    *out = {};
    out->committed = state == ImmediateLaneAttemptState::Committed ||
                     state == ImmediateLaneAttemptState::Cancelling;
    if (!out->committed &&
        state != ImmediateLaneAttemptState::PrivateCancelled) return true;
    out->complete = attempt->nextBucket.load(std::memory_order_acquire) ==
                    attempt->bucketCount;
    for (std::uint32_t i = 0; i < attempt->bucketCount; ++i) {
        const auto& current = BucketBase(handle.slot)[i];
        const auto bucket = current.state.load(std::memory_order_acquire);
        out->applied = out->applied || bucket == ImmediateLaneBucketState::Applied;
        out->missed = out->missed || bucket == ImmediateLaneBucketState::Missed;
        out->arenaCapacityMissed = out->arenaCapacityMissed ||
            (bucket == ImmediateLaneBucketState::Missed &&
             current.attemptGeneration == handle.generation &&
             current.admissionFailure.load(std::memory_order_acquire) ==
                 ImmediateLaneAdmissionFailure::ArenaCapacity);
        out->protocolRefused = out->protocolRefused ||
            (bucket == ImmediateLaneBucketState::Missed &&
             current.attemptGeneration == handle.generation &&
             current.admissionFailure.load(std::memory_order_acquire) ==
                 ImmediateLaneAdmissionFailure::FrontierValidation);
        out->cancelled = out->cancelled ||
            bucket == ImmediateLaneBucketState::Cancelled;
        out->cancelledOnly = out->cancelledOnly ||
            bucket == ImmediateLaneBucketState::CancelledOnly;
        out->claimed = out->claimed || bucket == ImmediateLaneBucketState::AudioClaimed;
    }
    return true;
}

bool ImmediateActionLane::CapturePublishedValidationRefusal(
    ImmediateLaneHandle handle, std::uint64_t completedAudioSequence,
    ImmediateLaneValidationRefusalSnapshot* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Cancelling ||
        attempt->nextBucket.load(std::memory_order_acquire) !=
            attempt->bucketCount) return false;
    ImmediateLaneValidationRefusalSnapshot actual{};
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index) {
        const auto& bucket = BucketBase(handle.slot)[index];
        if (bucket.state.load(std::memory_order_acquire) !=
                ImmediateLaneBucketState::Missed ||
            bucket.admissionFailure.load(std::memory_order_acquire) !=
                ImmediateLaneAdmissionFailure::FrontierValidation) continue;
        if (actual.bucketOrdinal != UINT32_MAX ||
            bucket.attemptGeneration != handle.generation ||
            bucket.completedAudioSequence.load(std::memory_order_relaxed) == 0 ||
            bucket.completedAudioSequence.load(std::memory_order_relaxed) > completedAudioSequence ||
            bucket.firstGroup > attempt->groupCount ||
            bucket.groupCount > attempt->groupCount - bucket.firstGroup)
            return false;
        const auto sequence = bucket.completedAudioSequence.load(std::memory_order_relaxed);
        const auto waiting = bucket.sourceWaitSequence.load(std::memory_order_acquire);
        if (bucket.quantum == UINT64_MAX ||
            (waiting == 0 ? sequence != bucket.quantum + 1u :
                waiting <= bucket.quantum || waiting > sequence || sequence <= bucket.quantum)) return false;
        actual = {attempt->root, index, bucket.firstGroup, bucket.groupCount,
                  sequence, bucket.quantum, waiting};
    }
    if (actual.bucketOrdinal == UINT32_MAX) return false;
    *out = actual;
    return true;
}

bool ImmediateActionLane::CapturePublishedValidationRefusalGroup(
    ImmediateLaneHandle handle, std::uint64_t completedAudioSequence,
    std::uint32_t bucketOrdinal, std::uint32_t groupOffset,
    ImmediateLaneGroupRef* out) const noexcept {
    if (out == nullptr) return false;
    *out = {};
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Cancelling ||
        attempt->nextBucket.load(std::memory_order_acquire) !=
            attempt->bucketCount || !attempt->groupPinsInstalled ||
        bucketOrdinal >= attempt->bucketCount)
        return false;
    const auto& bucket = BucketBase(handle.slot)[bucketOrdinal];
    if (bucket.state.load(std::memory_order_acquire) !=
            ImmediateLaneBucketState::Missed ||
        bucket.admissionFailure.load(std::memory_order_acquire) !=
            ImmediateLaneAdmissionFailure::FrontierValidation ||
        bucket.attemptGeneration != handle.generation ||
        bucket.completedAudioSequence.load(std::memory_order_relaxed) == 0 ||
        bucket.completedAudioSequence.load(std::memory_order_relaxed) > completedAudioSequence ||
        bucket.firstGroup > attempt->groupCount ||
        bucket.groupCount > attempt->groupCount - bucket.firstGroup ||
        groupOffset >= bucket.groupCount) return false;
    const auto sequence = bucket.completedAudioSequence.load(std::memory_order_relaxed);
    const auto waiting = bucket.sourceWaitSequence.load(std::memory_order_acquire);
    if (bucket.quantum == UINT64_MAX ||
        (waiting == 0 ? sequence != bucket.quantum + 1u :
            waiting <= bucket.quantum || waiting > sequence || sequence <= bucket.quantum)) return false;
    const auto& actual = GroupBase(handle.slot)[bucket.firstGroup + groupOffset];
    if (!actual.identity.Exact() || actual.laneSlot != handle.slot ||
        actual.laneGeneration != handle.generation ||
        actual.dueQuantum != bucket.quantum ||
        actual.bucketOrdinal != bucketOrdinal ||
        actual.groupIndex != bucket.firstGroup + groupOffset) return false;
    *out = actual;
    return true;
}

bool ImmediateActionLane::OwnsPrivate(
    ImmediateLaneHandle handle, std::uint64_t root) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || root == 0 || attempt->root != root)
        return false;
    const auto state = attempt->state.load(std::memory_order_acquire);
    return state == ImmediateLaneAttemptState::Reserved ||
           state == ImmediateLaneAttemptState::Prepared;
}

bool ImmediateActionLane::OwnsNeverCommitted(
    ImmediateLaneHandle handle, std::uint64_t root) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || root == 0 || attempt->root != root ||
        attempt->captureActive.load(std::memory_order_acquire)) return false;
    const auto state = attempt->state.load(std::memory_order_acquire);
    return state == ImmediateLaneAttemptState::Prepared ||
           state == ImmediateLaneAttemptState::PrivateCancelled;
}

bool ImmediateActionLane::MatchesReservedMembers(ImmediateLaneHandle handle,
    std::uint64_t root, ImmediateLaneDemand originalDemand,
    const ImmediateLaneMemberManifest* originalMembers,
    std::uint32_t memberCount) const noexcept {
    return memberCount == originalDemand.members &&
        MatchesReservedMemberSpan(handle, root, originalDemand, 0,
            originalMembers, memberCount);
}

bool ImmediateActionLane::MatchesReservedMemberSpan(ImmediateLaneHandle handle,
    std::uint64_t root, ImmediateLaneDemand originalDemand,
    std::uint32_t firstMember,
    const ImmediateLaneMemberManifest* originalMembers,
    std::uint32_t memberCount) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || root == 0 || attempt->root != root ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Reserved ||
        attempt->captureActive.load(std::memory_order_acquire) ||
        (memberCount != 0 && originalMembers == nullptr) ||
        attempt->eventCredit != originalDemand.events ||
        attempt->groupCredit != originalDemand.groups ||
        attempt->memberCredit != originalDemand.members ||
        attempt->bucketCredit != originalDemand.buckets ||
        attempt->memberCount != originalDemand.members ||
        firstMember > attempt->memberCount ||
        memberCount > attempt->memberCount - firstMember ||
        attempt->eventCount != 0 || attempt->groupCount != 0 ||
        attempt->pinGroupManifestCount != 0 || attempt->declaredGroupCount != 0 ||
        attempt->bucketCount != 0 || attempt->anchorFrame != 0 ||
        attempt->pinsInstalled || attempt->groupPinsInstalled) return false;
    const auto* members = MemberBase(handle.slot);
    for (std::uint32_t index = 0; index < memberCount; ++index) {
        const auto& actual = members[firstMember + index];
        const auto& original = originalMembers[index];
        if (!(actual.group == original.group) ||
            !(actual.relatedGroup == original.relatedGroup) ||
            !SameArm(actual.arm, original.arm) ||
            actual.eventOrdinal != original.eventOrdinal ||
            actual.secondEventOrdinal != original.secondEventOrdinal ||
            actual.eventSpanCount != original.eventSpanCount ||
            actual.memberIndex != original.memberIndex ||
            actual.kind != original.kind || actual.future != original.future ||
            !SameTimeSpec(actual, original) ||
            actual.programGeneration != original.programGeneration ||
            !(actual.target == original.target) ||
            actual.ownerInstance != original.ownerInstance ||
            actual.ownerEpoch != original.ownerEpoch) return false;
    }
    return true;
}

bool ImmediateActionLane::ValidatePrivateVariableControlEntryScope(
    ImmediateLaneHandle handle, std::uint64_t root,
    ImmediateLaneDemand originalDemand,
    const ImmediateLanePrivateVariableControlEntryScope& scope,
    const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
    const DecisionTable& decisions,
    const std::array<PreparedPrivateImmediateLaneGroup, 3>& originals,
    const std::array<PrivateImmediateLaneGroupTransferProof, 3>& proofs) const noexcept {
    if (!scope.entry.Valid() || scope.memberCount == 0 ||
        scope.memberCount > kMaxKnownScheduleLeaves ||
        scope.coordinatorRequest.expectedContinuationGeneration != scope.entry.attempt ||
        !decisions.MatchesOpenReservation(scope.entry.coordinator, scope.coordinatorRequest) ||
        !MatchesReservedMemberSpan(handle, root, originalDemand,
            scope.firstMember, scope.members, scope.memberCount)) return false;
    std::uint32_t memberBase = 0;
    for (std::size_t index = 0; index < originals.size(); ++index) {
        const auto& input = scope.groups[index];
        if (input.executeFrame != scope.provisionalFrame ||
            !pool.MatchesHeldPrivateImmediateLaneGroupOriginal(originals[index], proofs[index],
                root, scope.provisionalFrame, input, arms)) return false;
        for (std::size_t prior = 0; prior < index; ++prior)
            if (originals[prior].Identity() == originals[index].Identity()) return false;
        if (index < 2) {
            if (input.memberCount == 0 || input.sourceControlCount != 0 ||
                input.members == nullptr || input.memberCount > scope.memberCount - memberBase)
                return false;
            for (std::uint32_t member = 0; member < input.memberCount; ++member) {
                const auto& manifest = scope.members[memberBase + member];
                if (!(manifest.group == originals[index].Identity()) ||
                    !SameArm(manifest.arm, input.members[member])) return false;
            }
            memberBase += input.memberCount;
        } else if (input.memberCount != 0 || input.sourceControlCount != 0 ||
            input.decision.slot != scope.entry.coordinator.slot ||
            input.decision.generation != scope.entry.coordinator.generation) return false;
    }
    return memberBase == scope.memberCount;
}

bool ImmediateActionLane::DeclarePrivateVariableControlEntry(
    ImmediateLaneHandle handle, std::uint64_t root,
    ImmediateLaneDemand originalDemand,
    const ImmediateLanePrivateVariableControlEntryScope& scope,
    const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
    const DecisionTable& decisions,
    const std::array<PreparedPrivateImmediateLaneGroup, 3>& originals,
    const std::array<PrivateImmediateLaneGroupTransferProof, 3>& proofs,
    std::uint32_t* outOrdinal) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || outOrdinal == nullptr ||
        attempt->privateEntryCount >= config_.eventsPerAttempt ||
        !ValidatePrivateVariableControlEntryScope(handle, root, originalDemand,
            scope, pool, arms, decisions, originals, proofs)) return false;
    auto entries = PrivateEntries(handle.slot);
    for (std::uint32_t index = 0; index < attempt->privateEntryCount; ++index) {
        const auto& prior = entries[index];
        if (prior.entry == scope.entry ||
            (prior.entry.coordinator.slot == scope.entry.coordinator.slot &&
             prior.entry.coordinator.generation == scope.entry.coordinator.generation) ||
            (prior.entry.owner.serviceSlot == scope.entry.owner.serviceSlot &&
             prior.entry.owner.serviceGeneration == scope.entry.owner.serviceGeneration) ||
            (scope.firstMember < static_cast<std::uint64_t>(prior.firstMember) + prior.memberCount &&
             prior.firstMember < static_cast<std::uint64_t>(scope.firstMember) + scope.memberCount)) return false;
        for (const auto& group : prior.groups)
            for (const auto& original : originals)
                if (group == original.Identity()) return false;
    }
    const auto ordinal = attempt->privateEntryCount;
    auto* acquired = AcquirePrivateEntry(static_cast<std::size_t>(handle.slot) *
        config_.eventsPerAttempt + ordinal);
    if (acquired == nullptr) return false;
    auto& declared = *acquired;
    declared.entry = scope.entry;
    declared.coordinatorRequest = scope.coordinatorRequest;
    declared.pool = &pool;
    declared.armTable = &arms;
    declared.armTableIncarnation = arms.Incarnation();
    declared.decisions = &decisions;
    declared.provisionalFrame = scope.provisionalFrame;
    declared.firstMember = scope.firstMember;
    declared.memberCount = scope.memberCount;
    declared.firstOwnedGroup = UINT32_MAX;
    declared.ownedGroupCount = 0;
    declared.mapping = {};
    declared.mappingValid = false;
    declared.admission.store(0, std::memory_order_relaxed);
    declared.bucketCount = 0;
    declared.buckets = nullptr;
    declared.bucketCapacity = 0;
    for (std::size_t index = 0; index < originals.size(); ++index)
        declared.groups[index] = originals[index].Identity();
    ++attempt->privateEntryCount;
    *outOrdinal = ordinal;
    return true;
}

bool ImmediateActionLane::MatchesPrivateVariableControlEntry(
    ImmediateLaneHandle handle, std::uint64_t root,
    ImmediateLaneDemand originalDemand, std::uint32_t ordinal,
    const ImmediateLanePrivateVariableControlEntryScope& scope,
    const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
    const DecisionTable& decisions,
    const std::array<PreparedPrivateImmediateLaneGroup, 3>& originals,
    const std::array<PrivateImmediateLaneGroupTransferProof, 3>& proofs) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || ordinal >= attempt->privateEntryCount ||
        !ValidatePrivateVariableControlEntryScope(handle, root, originalDemand,
            scope, pool, arms, decisions, originals, proofs)) return false;
    const auto& declared = PrivateEntries(handle.slot)[ordinal];
    if (!(declared.entry == scope.entry) || declared.pool != &pool ||
        declared.armTable != &arms || declared.armTableIncarnation != arms.Incarnation() ||
        declared.decisions != &decisions ||
        declared.provisionalFrame != scope.provisionalFrame ||
        declared.firstMember != scope.firstMember || declared.memberCount != scope.memberCount) return false;
    const auto& original = declared.coordinatorRequest;
    const auto& current = scope.coordinatorRequest;
    if (original.ticket.sequence != current.ticket.sequence ||
        original.world != current.world || original.transport != current.transport ||
        original.expectedContinuationGeneration != current.expectedContinuationGeneration ||
        original.sliceCount != current.sliceCount ||
        original.transitionGate.slot != current.transitionGate.slot ||
        original.transitionGate.generation != current.transitionGate.generation) return false;
    for (std::size_t index = 0; index < originals.size(); ++index)
        if (!(declared.groups[index] == originals[index].Identity())) return false;
    return true;
}

bool ImmediateActionLane::DeclareAcceptedPrivateVariableControlEntry(
    ImmediateLaneHandle handle, std::uint64_t root, ImmediateLaneDemand originalDemand,
    const ImmediateLaneAcceptedPrivateEntryScope& scope,
    const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
    const DecisionTable& decisions, std::uint32_t* outOrdinal) noexcept {
    auto* attempt = Resolve(handle);
    // Reserved is an earlier private phase than OwnsNeverCommitted's placed
    // phase. No events, pins, placement or capture may have been installed.
    if (attempt == nullptr || outOrdinal == nullptr || root == 0 || attempt->root != root ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Reserved ||
        attempt->captureActive.load(std::memory_order_acquire) || attempt->eventCount != 0 ||
        attempt->groupCount != 0 || attempt->bucketCount != 0 || attempt->anchorFrame != 0 ||
        attempt->pinsInstalled || attempt->groupPinsInstalled || attempt->pinGroupManifestCount != 0 ||
        attempt->privateEntryCount >= config_.eventsPerAttempt ||
        attempt->eventCredit != originalDemand.events || attempt->groupCredit != originalDemand.groups ||
        attempt->memberCredit != originalDemand.members || attempt->bucketCredit != originalDemand.buckets ||
        attempt->memberCount != originalDemand.members ||
        attempt->declaredGroupCount != originalDemand.groups || !scope.entry.Valid() ||
        scope.coordinatorRequest.expectedContinuationGeneration != scope.entry.attempt ||
        !decisions.MatchesOpenReservation(scope.entry.coordinator, scope.coordinatorRequest) ||
        scope.members == nullptr || scope.memberCount == 0 || scope.firstMember > attempt->memberCount ||
        scope.memberCount > attempt->memberCount - scope.firstMember || scope.groups == nullptr ||
        scope.groupCount < 3u || scope.groupCount > originalDemand.groups) return false;
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* members = MemberBase(handle.slot);
    const auto* declaredGroups = declaredGroups_.At(offset);
    auto* matchedGroups = groupPlacementOrder_.get();
    auto entries = PrivateEntries(handle.slot);
    std::uint32_t firstOwnedGroup = 0;
    for (std::uint32_t index = 0; index < attempt->privateEntryCount; ++index) {
        const auto& prior = entries[index];
        if (prior.ownedGroupCount == 0 || prior.firstOwnedGroup != firstOwnedGroup ||
            prior.ownedGroupCount > originalDemand.groups - firstOwnedGroup ||
            prior.entry == scope.entry ||
            (prior.entry.coordinator.slot == scope.entry.coordinator.slot &&
             prior.entry.coordinator.generation == scope.entry.coordinator.generation) ||
            (prior.entry.owner.serviceSlot == scope.entry.owner.serviceSlot &&
             prior.entry.owner.serviceGeneration == scope.entry.owner.serviceGeneration) ||
            (scope.firstMember < static_cast<std::uint64_t>(prior.firstMember) + prior.memberCount &&
             prior.firstMember < static_cast<std::uint64_t>(scope.firstMember) + scope.memberCount)) return false;
        firstOwnedGroup += prior.ownedGroupCount;
    }
    if (scope.groupCount > originalDemand.groups - firstOwnedGroup) return false;
    const auto owns = [&](const Plan03GroupSliceIdentity& identity) noexcept {
        std::uint32_t matches = 0;
        for (std::uint32_t index = 0; index < scope.groupCount; ++index)
            matches += scope.groups[index].schedule.identity == identity;
        return matches == 1u;
    };
    for (std::uint32_t index = 0; index < scope.groupCount; ++index) {
        const auto& group = scope.groups[index];
        OriginalQueueFutureGroupScope captured{};
        std::uint64_t due = 0;
        if (group.original == nullptr || !group.schedule.identity.Exact() ||
            !(group.original->Identity() == group.schedule.identity) ||
            !group.original->CaptureOriginalScope(pool, arms, &captured) ||
            captured.cohortRoot != root || captured.provisionalAnchorFrame != scope.provisionalFrame ||
            captured.selectedFutureDomain != group.scope.selectedFutureDomain ||
            group.scope.cohortRoot != root || group.scope.provisionalAnchorFrame != scope.provisionalFrame ||
            !group.original->MatchesOriginalScope(root, scope.provisionalFrame, group.scope.input, arms) ||
            !pool.PrivatePlan03GroupBindable(group.schedule.identity) ||
            !ResolveLaneDue(group.schedule, scope.provisionalFrame, &due) ||
            due != captured.input.executeFrame || !owns(group.schedule.identity)) return false;
        std::uint32_t declaredMatches = 0;
        for (std::uint32_t candidate = 0; candidate < attempt->declaredGroupCount; ++candidate)
            if (declaredGroups[candidate].identity == group.schedule.identity) {
                if (!SameTimeSpec(declaredGroups[candidate], group.schedule)) return false;
                matchedGroups[index] = candidate;
                ++declaredMatches;
            }
        if (declaredMatches != 1) return false;
        for (std::uint32_t prior = 0; prior < firstOwnedGroup; ++prior)
            if (declaredGroups[groupState_[offset + prior].privateOwnedIndex].identity == group.schedule.identity)
                return false;
        std::uint32_t manifestCount = 0;
        for (std::uint32_t member = 0; member < scope.memberCount; ++member) {
            const auto& manifest = scope.members[member];
            if (!(manifest.group == group.schedule.identity)) continue;
            if (captured.input.members == nullptr || manifest.memberIndex >= captured.input.memberCount ||
                !SameArm(manifest.arm, captured.input.members[manifest.memberIndex])) return false;
            for (std::uint32_t prior = 0; prior < member; ++prior)
                if (scope.members[prior].group == manifest.group &&
                    scope.members[prior].memberIndex == manifest.memberIndex) return false;
            ++manifestCount;
        }
        if (manifestCount != captured.input.memberCount) return false;
    }
    for (const auto& group : scope.outerGroups) if (!owns(group)) return false;
    if (scope.outerGroups[2].decision.slot != scope.entry.coordinator.slot ||
        scope.outerGroups[2].decision.generation != scope.entry.coordinator.generation) return false;
    for (std::uint32_t index = 0; index < scope.memberCount; ++index) {
        const auto& actual = members[scope.firstMember + index];
        const auto& original = scope.members[index];
        if (!owns(original.group) || (original.relatedGroup.Exact() && !owns(original.relatedGroup)) ||
            !(actual.group == original.group) || !(actual.relatedGroup == original.relatedGroup) ||
            !SameArm(actual.arm, original.arm) || actual.eventOrdinal != original.eventOrdinal ||
            actual.secondEventOrdinal != original.secondEventOrdinal ||
            actual.eventSpanCount != original.eventSpanCount || actual.memberIndex != original.memberIndex ||
            actual.kind != original.kind || actual.future != original.future || !SameTimeSpec(actual, original) ||
            actual.programGeneration != original.programGeneration || !(actual.target == original.target) ||
            actual.ownerInstance != original.ownerInstance || actual.ownerEpoch != original.ownerEpoch) return false;
    }
    auto* acquired = AcquirePrivateEntry(offset + attempt->privateEntryCount);
    if (acquired == nullptr) return false;
    auto& declared = *acquired;
    declared.entry = scope.entry;
    declared.coordinatorRequest = scope.coordinatorRequest;
    declared.pool = &pool;
    declared.armTable = &arms;
    declared.armTableIncarnation = arms.Incarnation();
    declared.decisions = &decisions;
    declared.provisionalFrame = scope.provisionalFrame;
    declared.firstMember = scope.firstMember;
    declared.memberCount = scope.memberCount;
    declared.groups = scope.outerGroups;
    declared.firstOwnedGroup = firstOwnedGroup;
    declared.ownedGroupCount = scope.groupCount;
    declared.mapping = {};
    declared.mappingValid = false;
    declared.admission.store(0, std::memory_order_relaxed);
    declared.buckets = nullptr;
    declared.bucketCount = declared.bucketCapacity = 0;
    for (std::uint32_t index = 0; index < scope.groupCount; ++index)
        groupState_[offset + firstOwnedGroup + index].privateOwnedIndex = matchedGroups[index];
    *outOrdinal = attempt->privateEntryCount++;
    return true;
}

bool ImmediateActionLane::HasProgramReference(ProgramGeneration generation) const noexcept {
    return HasProgramReferenceImpl(generation, nullptr);
}

bool ImmediateActionLane::HasProgramReference(
    TimelineTransactionProgramRef program) const noexcept {
    return program.program != nullptr &&
        HasProgramReferenceImpl(program.programGeneration, program.program);
}

bool ImmediateActionLane::HasProgramReferenceImpl(ProgramGeneration generation,
    const TimelineProgramView* program) const noexcept {
    if (!valid_ || generation == 0) return false;
    const auto matches = [generation, program](TimelineTransactionProgramRef reference) noexcept {
        return reference.programGeneration == generation &&
            (program == nullptr || reference.program == program);
    };
    for (std::uint32_t slot = 0; slot < attempts_.size(); ++slot) {
        const auto& attempt = attempts_[slot];
        const auto state = attempt.state.load(std::memory_order_acquire);
        if (state == ImmediateLaneAttemptState::PrivateLayoutHeld) {
            // Held layouts can restore originals or regenerate an omitted authored event.
            for (std::uint32_t event = 0; event < attempt.eventCount; ++event)
                if (matches(EventBase(slot)[event].event.program)) return true;
            const auto* edit = attempt.privateLayoutEdit;
            if (edit != nullptr) {
                for (std::uint32_t event = 0; event < edit->originalEventCount_; ++event)
                    if (matches(edit->originalEvents_[event].event.program)) return true;
                if (edit->hasAuthoredLayout_ &&
                    (matches(edit->authoredLayout_.start.event.program) ||
                     matches(edit->authoredLayout_.envelope.event.program) ||
                     matches(edit->authoredLayout_.stop.event.program))) return true;
            }
            continue;
        }
        if (state == ImmediateLaneAttemptState::Free ||
            state == ImmediateLaneAttemptState::Reserved) continue;
        for (std::uint32_t event = 0; event < attempt.eventCount; ++event)
            if ((eventState_[static_cast<std::size_t>(slot) * config_.eventsPerAttempt +
                    eventState_[static_cast<std::size_t>(slot) * config_.eventsPerAttempt + event].withdrawalLeader].withdrawal
                    .load(std::memory_order_acquire) & kEventReferenceReleased) == 0 &&
                matches(EventBase(slot)[event].event.program)) return true;
    }
    return false;
}

bool ImmediateActionLane::WithdrawPlainNoneFence(ImmediateLaneHandle handle,
    std::uint64_t root, const TimelineTransactionProgramRef& program,
    const AuthoredStopFenceDescriptor& descriptor,
    std::uint64_t completedAudioSequence) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || root == 0 || attempt->root != root ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Committed ||
        !attempt->pinsInstalled || attempt->groupCount != 0 || attempt->memberCount != 0 ||
        attempt->declaredGroupCount != 0 || attempt->privateEntryCount != 0 ||
        program.program == nullptr || program.programGeneration == 0) return false;
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    std::uint32_t ordinal = UINT32_MAX;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& event = EventBase(handle.slot)[index].event;
        if (event.program.program != program.program ||
            event.program.programGeneration != program.programGeneration) continue;
        if (ordinal != UINT32_MAX) return false;
        ordinal = index;
    }
    if (ordinal == UINT32_MAX) return false;
    auto& admission = eventState_[offset + eventState_[offset + ordinal].withdrawalLeader].withdrawal;
    if ((admission.load(std::memory_order_acquire) & kEventReferenceReleased) != 0)
        return false;
    auto& reference = eventState_[offset + ordinal].fence;
    if (!reference.Matches(program) || reference.Descriptor() != &descriptor) return false;
    const auto previous = admission.fetch_or(kEventWithdrawalRequested, std::memory_order_acq_rel);
    if ((previous & kEventAudioAdmitting) != 0) return false;
    if (reference.lastReadSequence_ != 0 &&
        completedAudioSequence <= reference.lastReadSequence_) return false;
    reference.Reset();
    admission.store(kEventWithdrawalRequested | kEventReferenceReleased, std::memory_order_release);
    return true;
}

bool ImmediateActionLane::PlainReferenceAttempt(ImmediateLaneHandle handle,
    std::uint64_t root) const noexcept {
    const auto* attempt = Resolve(handle);
    return attempt != nullptr && root != 0 && attempt->root == root && attempt->pinsInstalled &&
        attempt->state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Committed &&
        attempt->groupCount == 0 && attempt->memberCount == 0 &&
        attempt->declaredGroupCount == 0 && attempt->privateEntryCount == 0;
}

bool ImmediateActionLane::WithdrawPlainSourceStartReference(ImmediateLaneHandle handle,
    std::uint64_t root, const TimelineTransactionProgramRef& program, TimelineArmToken source,
    std::uint64_t completedAudioSequence) noexcept {
    if (!PlainReferenceAttempt(handle, root) || program.program == nullptr ||
        program.programGeneration == 0 || !source.Valid()) return false;
    const auto* attempt = Resolve(handle);
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    std::uint32_t ordinal = UINT32_MAX;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& event = EventBase(handle.slot)[index].event;
        if (event.program.program != program.program ||
            event.program.programGeneration != program.programGeneration ||
            !SameArm(event.armToken, source)) continue;
        if (ordinal != UINT32_MAX) return false;
        ordinal = index;
    }
    if (ordinal == UINT32_MAX) return false;
    const auto scope = eventState_[offset + ordinal].withdrawalLeader;
    auto& admission = eventState_[offset + scope].withdrawal;
    if ((admission.load(std::memory_order_acquire) & kEventReferenceReleased) != 0) return false;
    auto& pin = eventState_[offset + ordinal].arm;
    const auto& original = EventBase(handle.slot)[ordinal];
    auto* table = pin.owner_;
    if (table == nullptr || original.futureGroupedStart ||
        !table->PinnedPlainSourceStartMatches(pin, original.event, original.armProgramGeneration)) return false;
    const auto previous = admission.fetch_or(kEventWithdrawalRequested, std::memory_order_acq_rel);
    if ((previous & kEventAudioAdmitting) != 0) return false;
    const auto last = eventState_[offset + scope].lastCaptureSequence;
    if (last != 0 && completedAudioSequence <= last) return false;
    table->ReleaseWithdrawnPlainLaneReader(pin);
    admission.store(kEventWithdrawalRequested | kEventReferenceReleased, std::memory_order_release);
    return true;
}

bool ImmediateActionLane::WithdrawPlainAuthoredControlReferences(ImmediateLaneHandle handle,
    std::uint64_t root, const TimelineTransactionProgramRef& program, TimelineArmToken control,
    const AuthoredStopOperation& operation, std::uint64_t completedAudioSequence) noexcept {
    if (!PlainReferenceAttempt(handle, root) || program.program == nullptr ||
        program.programGeneration == 0 || !control.Valid()) return false;
    const auto* attempt = Resolve(handle);
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    std::array<std::uint32_t, 2> ordinals{};
    std::uint32_t count = 0, scope = UINT32_MAX, matched = 0;
    const TimelineArmTable::AuthoredControlMetadata* metadata = nullptr;
    TimelineArmTable* table = nullptr;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& original = EventBase(handle.slot)[index];
        const auto& event = original.event;
        if (event.program.program != program.program ||
            event.program.programGeneration != program.programGeneration ||
            !SameArm(event.armToken, control)) continue;
        const auto currentScope = eventState_[offset + index].withdrawalLeader;
        if (count == ordinals.size() ||
            (eventState_[offset + currentScope].withdrawal.load(std::memory_order_acquire) &
                kEventReferenceReleased) != 0 || original.futureGroupedStart) return false;
        auto& pin = eventState_[offset + index].arm;
        if (pin.owner_ == nullptr) return false;
        const auto* current = pin.owner_->PinnedPlainControlMatches(pin, event, &operation);
        if (current == nullptr || (count != 0 &&
            (metadata != current || table != pin.owner_ || scope != currentScope)) ||
            (matched & (1u << event.payload)) != 0) return false;
        metadata = current;
        table = pin.owner_;
        scope = currentScope;
        matched |= 1u << event.payload;
        ordinals[count++] = index;
    }
    if (count == 0 || count != metadata->eventCount || matched != (1u << count) - 1u) return false;
    auto& admission = eventState_[offset + scope].withdrawal;
    const auto previous = admission.fetch_or(kEventWithdrawalRequested, std::memory_order_acq_rel);
    if ((previous & kEventAudioAdmitting) != 0) return false;
    const auto last = eventState_[offset + scope].lastCaptureSequence;
    if (last != 0 && completedAudioSequence <= last) return false;
    for (std::uint32_t index = 0; index < count; ++index)
        table->ReleaseWithdrawnPlainLaneReader(eventState_[offset + ordinals[index]].arm);
    admission.store(kEventWithdrawalRequested | kEventReferenceReleased, std::memory_order_release);
    return true;
}

void ImmediateActionLane::AssignPlainControlWithdrawalScopes(ImmediateLaneHandle handle) noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->groupCount != 0 || attempt->memberCount != 0 ||
        attempt->declaredGroupCount != 0 || attempt->privateEntryCount != 0) return;
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& original = EventBase(handle.slot)[index];
        auto& pin = eventState_[offset + index].arm;
        if (pin.owner_ == nullptr || original.futureGroupedStart) continue;
        const auto* metadata = pin.owner_->PinnedPlainControlMatches(pin, original.event, nullptr);
        if (metadata == nullptr) continue;
        std::array<std::uint32_t, 2> ordinals{};
        std::uint32_t count = 0, matched = 0;
        bool valid = true;
        for (std::uint32_t other = 0; other < attempt->eventCount; ++other) {
            const auto& candidate = EventBase(handle.slot)[other];
            if (candidate.event.program.program != original.event.program.program ||
                candidate.event.program.programGeneration != original.event.program.programGeneration ||
                !SameArm(candidate.event.armToken, original.event.armToken)) continue;
            auto& owned = eventState_[offset + other].arm;
            if (count == ordinals.size() || owned.owner_ != pin.owner_ || candidate.futureGroupedStart ||
                owned.owner_->PinnedPlainControlMatches(owned, candidate.event, metadata->operation) != metadata ||
                (matched & (1u << candidate.event.payload)) != 0) { valid = false; break; }
            matched |= 1u << candidate.event.payload;
            ordinals[count++] = other;
        }
        if (!valid || count != metadata->eventCount || matched != (1u << count) - 1u) continue;
        const auto scope = eventState_[offset + ordinals[0]].withdrawalLeader;
        for (std::uint32_t member = 0; member < count; ++member)
            eventState_[offset + ordinals[member]].withdrawalLeader = scope;
    }
}

bool ImmediateActionLane::Prepare(ImmediateLaneHandle handle,
    const ImmediateLaneEvent* events, std::uint32_t eventCount) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    const auto initial = attempt->state.load(std::memory_order_acquire);
    if (initial != ImmediateLaneAttemptState::Reserved &&
        initial != ImmediateLaneAttemptState::Prepared) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt) return false;
    if (attempt == nullptr ||
        (eventCount != 0 && events == nullptr) ||
        (eventCount == 0 && attempt->declaredGroupCount == 0) ||
        eventCount > attempt->eventCredit ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Reserved)
        return false;
    auto* destination = EventBase(handle.slot);
    for (std::uint32_t index = 0; index < eventCount; ++index) {
        if (!ValidTimeSpec(events[index]) ||
            events[index].event.program.program == nullptr)
            return false;
        destination[index] = events[index];
    }
    if (eventCount != 0)
        std::sort(destination, destination + eventCount, LessLaneEvent);
    attempt->eventCount = eventCount;
    attempt->state.store(ImmediateLaneAttemptState::Prepared,
                         std::memory_order_release);
    return true;
}

bool ImmediateActionLane::DeclareMember(ImmediateLaneHandle handle,
    const ImmediateLaneMemberManifest& member,
    std::uint32_t* outOrdinal) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Reserved ||
        !member.group.Exact() || !member.arm.Valid() ||
        !ValidTimeSpec(member) ||
        member.programGeneration == 0 || member.ownerInstance == 0 ||
        member.ownerEpoch == 0 ||
        member.eventSpanCount == 0 || member.eventSpanCount > 2 ||
        member.secondEventOrdinal != UINT32_MAX ||
        (member.kind != ImmediateLaneMemberKind::SingleEvent &&
         member.future) ||
        ((member.kind == ImmediateLaneMemberKind::FrozenFutureTarget ||
          member.kind == ImmediateLaneMemberKind::HistoricalSourceCompletion) !=
         member.relatedGroup.Exact()) ||
        attempt->memberCount >= attempt->memberCredit) return false;
    auto* members = MemberBase(handle.slot);
    for (std::uint32_t index = 0; index < attempt->memberCount; ++index)
        if (members[index].group == member.group &&
            SameArm(members[index].arm, member.arm)) return false;
    const auto ordinal = attempt->memberCount;
    members[attempt->memberCount++] = member;
    if (outOrdinal != nullptr) *outOrdinal = ordinal;
    return true;
}

bool ImmediateActionLane::DeclareGroup(
    ImmediateLaneHandle handle,
    const ImmediateLaneDeclaredGroup& group) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Reserved ||
        !group.identity.Exact() || !ValidTimeSpec(group) ||
        attempt->declaredGroupCount >= attempt->groupCredit)
        return false;
    auto* groups = declaredGroups_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    std::uint32_t position = attempt->declaredGroupCount;
    for (std::uint32_t index = 0;
         index < attempt->declaredGroupCount; ++index) {
        if (groups[index].identity == group.identity) return false;
        if (position == attempt->declaredGroupCount &&
            TimeSpecValue(group) < TimeSpecValue(groups[index]))
            position = index;
    }
    for (std::uint32_t index = attempt->declaredGroupCount;
         index > position; --index)
        groups[index] = groups[index - 1u];
    groups[position] = group;
    ++attempt->declaredGroupCount;
    return true;
}

bool ImmediateActionLane::PinArms(
    ImmediateLaneHandle handle, TimelineArmTable& arms) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    const auto initial = attempt->state.load(std::memory_order_acquire);
    if (initial != ImmediateLaneAttemptState::Reserved &&
        initial != ImmediateLaneAttemptState::Prepared) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt) return false;
    if (attempt == nullptr || attempt->pinsInstalled ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Prepared)
        return false;
    const auto entries = PrivateEntries(handle.slot);
    for (std::uint32_t index = 0; index < attempt->privateEntryCount; ++index)
        if (entries[index].armTable != &arms ||
            entries[index].armTableIncarnation != arms.Incarnation()) return false;
    auto* eventRows = eventState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    auto* events = EventBase(handle.slot);
    std::uint32_t pinned = 0;
    for (; pinned < attempt->eventCount; ++pinned) {
        const auto& event = events[pinned].event;
        const auto* program = event.program.program;
        if (program != nullptr && program->role == TimelineProgramRole::AuthoredStopFence) {
            const auto* descriptor = program->authoredStopFence;
            if (descriptor == nullptr || event.armToken.slot != UINT32_MAX ||
                event.armToken.generation != 0 || event.armToken.renderSlot != UINT32_MAX ||
                event.armToken.renderGeneration != 0 || event.frontier.Present() ||
                events[pinned].futureGroupedStart || event.payload != 0 ||
                event.subeventPhase != 50 || event.authoredFadeOutCandidate ||
                events[pinned].timeDomain != ImmediateLaneTimeDomain::FixedAbsoluteFrame ||
                events[pinned].fixedFrame != descriptor->identity.stopFrame ||
                event.executeFrame != descriptor->identity.stopFrame ||
                !descriptor->AcquireLaneReference(event.program, &eventRows[pinned].fence)) break;
            continue;
        }
        if (program != nullptr && program->role == TimelineProgramRole::ControllerActivation &&
            event.payload < program->payloadCount &&
            program->payloads[event.payload].kind == TimelineEventKind::ControllerActivation &&
            event.frontier.kind == FrontierOperationKind::Entry &&
            event.frontier.group.Exact() && !event.armToken.Valid())
            continue;
        if (!events[pinned].event.armToken.Valid() ||
            !arms.PinFrontierReader(events[pinned].event.armToken,
                                    &eventRows[pinned].arm))
            break;
        // A composite schedule belongs to its root, while the installed arm
        // belongs to an individual leaf. Preserve both independent identities.
        events[pinned].armProgramGeneration =
            arms.Resolve(events[pinned].event.armToken)->programGeneration;
    }
    if (pinned != attempt->eventCount) {
        for (std::uint32_t prior = 0; prior < pinned; ++prior) {
            (void)eventRows[prior].arm.Release(0);
            eventRows[prior].fence.Reset();
        }
        return false;
    }
    AssignPlainControlWithdrawalScopes(handle);
    attempt->pinsInstalled = true;
    return true;
}

bool ImmediateActionLane::PinGroups(ImmediateLaneHandle handle,
    ScheduledTransactionPool& pool, DecisionTable& decisions,
    std::uint32_t maxGroupsPerQuantum) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePublicationProof != nullptr || attempt->groupPinsInstalled ||
        attempt->bucketCount == 0 || maxGroupsPerQuantum == 0 ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Prepared) return false;
    InvalidatePrivateEntryMappings(handle);
    auto* groupPins = groupPins_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    auto* groupRows = groupState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    auto* groups = GroupBase(handle.slot);
    const auto* events = EventBase(handle.slot);
    const auto* queue = attempt->queueContinuation;
    const auto selectedCount = queue != nullptr
        ? queue->CaptureGroups(*this, handle, pool, groups, attempt->groupCredit)
        : pool.CapturePrivateSelectedLaneGroupsForPinning(
            *this, handle, attempt->root, groups, attempt->groupCredit);
    const auto liveCount = selectedCount != 0 ? selectedCount :
        pool.CaptureImmediateLaneCohortGroups(
            attempt->root, groups, attempt->groupCredit);
    if (liveCount == 0 || liveCount > attempt->groupCredit) return false;
    for (std::uint32_t group = 0; group < liveCount; ++group)
        if (groups[group].laneSlot != handle.slot ||
            groups[group].laneGeneration != handle.generation)
            return false;
    if (attempt->declaredGroupCount != 0) {
        if (liveCount != attempt->declaredGroupCount) return false;
        const auto* declared = declaredGroups_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
        for (std::uint32_t group = 0; group < liveCount; ++group) {
            bool matched = false;
            for (std::uint32_t candidate = 0;
                 candidate < attempt->declaredGroupCount; ++candidate) {
                if (!(groups[group].identity ==
                      declared[candidate].identity)) continue;
                std::uint64_t due = 0;
                matched = ResolveLaneDue(declared[candidate], attempt->anchorFrame, &due) &&
                    groups[group].dueQuantum == due / config_.quantumFrames;
                break;
            }
            if (!matched) return false;
        }
    }
    std::sort(groups, groups + liveCount, [](const auto& a, const auto& b) {
        if (a.dueQuantum != b.dueQuantum) return a.dueQuantum < b.dueQuantum;
        return a.identity.committedRow < b.identity.committedRow;
    });
    attempt->conditionalProjectionDecisions = &decisions;
    attempt->pinGroupManifestCount = static_cast<std::uint32_t>(liveCount);
    attempt->pinGroupPool = &pool;
    for (std::uint32_t i = 0; i < liveCount; ++i) groupRows[i].futureOwned = 0;
    std::uint32_t count = 0;
    const auto rollback = [&]() noexcept {
        (void)ReleasePrivatePinGroupOwnership(handle, pool);
    };
    for (std::uint32_t i = 0; i < liveCount; ++i) {
        const auto identity = groups[i].identity;
        Plan03PageGroupView view{};
        if (!identity.Exact() || groups[i].dueQuantum == UINT64_MAX ||
            !pool.ResolvePlan03GroupSliceProducer(identity, &view) ||
            (queue != nullptr ? !queue->Owns(view) : view.frontier == nullptr) ||
            !pool.AcquirePlan03GroupReader(identity, &groupPins[count])) {
            rollback();
            return false;
        }
        if (!decisions.AcquireFuturePin(identity.decision,
                                       &groupRows[count].decision)) {
            rollback();
            return false;
        }
        ++count;
    }
    std::uint32_t groupIndex = 0;
    auto* buckets = BucketBase(handle.slot);
    for (std::uint32_t b = 0; b < attempt->bucketCount; ++b) {
        auto& bucket = buckets[b];
        bucket.firstGroup = groupIndex;
        bucket.groupCount = 0;
        bucket.admissionFailure.store(ImmediateLaneAdmissionFailure::None,
                                      std::memory_order_relaxed);
        bucket.memberCount = 0;
        if (groupIndex < count &&
            groups[groupIndex].dueQuantum < bucket.quantum) {
            rollback();
            return false;
        }
        while (groupIndex < count &&
               groups[groupIndex].dueQuantum == bucket.quantum) {
            groups[groupIndex].bucketOrdinal = b;
            groups[groupIndex].groupIndex = groupIndex;
            ++bucket.groupCount;
            ++groupIndex;
        }
        if (bucket.groupCount > maxGroupsPerQuantum) {
            rollback();
            return false;
        }
        for (std::uint32_t e = bucket.firstEvent;
             e < bucket.firstEvent + bucket.eventCount; ++e) {
            if (!events[e].event.frontier.Present() ||
                events[e].futureGroupedStart) continue;
            bool found = false;
            for (std::uint32_t g = bucket.firstGroup;
                 g < bucket.firstGroup + bucket.groupCount; ++g)
                found = found || groups[g].identity == events[e].event.frontier.group;
            if (!found) {
                rollback();
                return false;
            }
        }
    }
    if (groupIndex != count) {
        rollback();
        return false;
    }
    auto* manifest = MemberBase(handle.slot);
    auto* memberRows = memberState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    const auto declaredCount = attempt->memberCount;
    std::uint32_t memberCount = 0;
    for (std::uint32_t groupIndex = 0; groupIndex < count; ++groupIndex) {
        groups[groupIndex].futureDependent = false;
        Plan03PageGroupView view{};
        if (!pool.ResolvePlan03GroupSliceProducer(groups[groupIndex].identity,
                &view) || (queue != nullptr ? !queue->Owns(view) : view.frontier == nullptr)) {
            rollback();
            return false;
        }
        groupRows[groupIndex].memberBase = memberCount;
        groupRows[groupIndex].memberCount = 0;
        if (queue != nullptr) continue;
        if ((view.frontier->role != FrontierGroupRole::EntryMember &&
             view.frontier->role != FrontierGroupRole::Future) ||
            (!view.frontier->variableSide.Valid() &&
             !view.frontier->queueInitial.Valid() &&
             !view.frontier->queueConditional.Valid())) continue;
        if (view.memberCount > attempt->memberCredit - memberCount) {
            rollback();
            return false;
        }
        groupRows[groupIndex].memberCount = view.memberCount;
        for (std::uint16_t member = 0; member < view.memberCount; ++member) {
            std::uint32_t declarationMatches = 0;
            ImmediateLaneMemberManifest* declaration = nullptr;
            std::uint32_t declarationIndex = UINT32_MAX;
            for (std::uint32_t declaredIndex = 0;
                 declaredIndex < declaredCount; ++declaredIndex) {
                auto& candidate = manifest[declaredIndex];
                if (!(candidate.group == groups[groupIndex].identity) ||
                    !SameArm(candidate.arm, view.members[member])) continue;
                ++declarationMatches;
                declaration = &candidate;
                declarationIndex = declaredIndex;
            }
            if (declarationMatches != 1 || declaration == nullptr) {
                rollback();
                return false;
            }
            if (declaration->kind != ImmediateLaneMemberKind::SingleEvent) {
                const bool crossSlice = declaration->kind ==
                        ImmediateLaneMemberKind::FrozenFutureTarget ||
                    declaration->kind ==
                        ImmediateLaneMemberKind::HistoricalSourceCompletion;
                const auto sourceGroup = crossSlice ?
                    declaration->relatedGroup : groups[groupIndex].identity;
                const bool futureTarget = declaration->kind ==
                    ImmediateLaneMemberKind::FrozenFutureTarget;
                const bool historicalSource = declaration->kind ==
                    ImmediateLaneMemberKind::HistoricalSourceCompletion;
                if ((crossSlice && (!sourceGroup.Exact() ||
                        sourceGroup.decision.slot !=
                            groups[groupIndex].identity.decision.slot ||
                        sourceGroup.decision.generation !=
                            groups[groupIndex].identity.decision.generation ||
                        sourceGroup.sliceIndex !=
                            (futureTarget ? groups[groupIndex].identity.sliceIndex + 1u :
                             groups[groupIndex].identity.sliceIndex - 1u))) ||
                    (historicalSource && groups[groupIndex].identity.sliceIndex == 0) ||
                    (futureTarget && sourceGroup.sliceIndex == 0)) {
                    rollback();
                    return false;
                }
                std::uint32_t first = UINT32_MAX;
                std::uint32_t second = UINT32_MAX;
                std::uint32_t matches = 0;
                for (std::uint32_t event = 0;
                     event < attempt->eventCount; ++event) {
                    const auto& candidate = events[event];
                    if (!(candidate.event.frontier.group == sourceGroup) ||
                        !SameArm(candidate.event.armToken, view.members[member]))
                        continue;
                    if (first == UINT32_MAX) first = event;
                    else if (matches == 1u) second = event;
                    if (matches >= declaration->eventSpanCount ||
                        !SameTimeSpec(candidate, *declaration) ||
                        candidate.event.program.programGeneration !=
                            declaration->programGeneration ||
                        candidate.event.ownerInstanceHandle !=
                            declaration->ownerInstance ||
                        candidate.event.ownerInstanceScheduleEpoch !=
                            declaration->ownerEpoch ||
                        candidate.event.program.program == nullptr ||
                        candidate.event.payload >=
                            candidate.event.program.program->payloadCount ||
                        !(candidate.event.program.program->payloads[
                            candidate.event.payload].target ==
                            declaration->target)) {
                        rollback();
                        return false;
                    }
                    const auto kind = candidate.event.program.program->payloads[
                        candidate.event.payload].kind;
                    if (declaration->eventSpanCount == 2 ?
                            kind != (matches == 0 ? TimelineEventKind::Start :
                                TimelineEventKind::Envelope) :
                            (historicalSource ? kind != TimelineEventKind::Envelope :
                             futureTarget ? kind != TimelineEventKind::Start :
                                kind != TimelineEventKind::Start &&
                                kind != TimelineEventKind::Envelope)) {
                        rollback();
                        return false;
                    }
                    ++matches;
                }
                if (matches != declaration->eventSpanCount ||
                    first == UINT32_MAX ||
                    (futureTarget &&
                     groups[groupIndex].dueQuantum >=
                         events[first].event.executeFrame / config_.quantumFrames) ||
                    (historicalSource &&
                     groups[groupIndex].dueQuantum <=
                         events[first].event.executeFrame / config_.quantumFrames)) {
                    rollback();
                    return false;
                }
                declaration->eventOrdinal = first;
                declaration->secondEventOrdinal = second;
                if (!ValidMemberEventOrdinals(*declaration, events, attempt->eventCount,
                        config_.quantumFrames)) {
                    rollback();
                    return false;
                }
                memberRows[memberCount].order = declarationIndex;
                declaration->memberIndex = member;
                declaration->future = declaration->kind == ImmediateLaneMemberKind::CurrentEventSpan &&
                    events[first].futureGroupedStart;
                if (declaration->future) {
                    bool bucketFound = false;
                    for (std::uint32_t bucketIndex = 0; bucketIndex < attempt->bucketCount; ++bucketIndex) {
                        auto& bucket = BucketBase(handle.slot)[bucketIndex];
                        if (first < bucket.firstEvent || first >= bucket.firstEvent + bucket.eventCount)
                            continue;
                        bool ownsGroup = false;
                        for (std::uint32_t index = bucket.firstGroup;
                             index < bucket.firstGroup + bucket.groupCount; ++index)
                            ownsGroup = ownsGroup || groups[index].identity == declaration->group;
                        if (!ownsGroup || bucket.memberCount == UINT32_MAX) { rollback(); return false; }
                        ++bucket.memberCount;
                        bucketFound = true;
                        break;
                    }
                    if (!bucketFound) { rollback(); return false; }
                    if (view.frontier->role == FrontierGroupRole::EntryMember)
                        groups[groupIndex].futureDependent = true;
                }
                if (futureTarget) groups[groupIndex].futureDependent = true;
                ++memberCount;
                continue;
            }
            std::uint32_t matches = 0;
            std::uint32_t ordinal = UINT32_MAX;
            for (std::uint32_t event = 0; event < attempt->eventCount; ++event) {
                if (!(events[event].event.frontier.group ==
                        groups[groupIndex].identity) ||
                    !SameArm(events[event].event.armToken,
                             view.members[member])) continue;
                ++matches;
                ordinal = event;
            }
            if (matches != 1 || ordinal == UINT32_MAX ||
                !SameTimeSpec(events[ordinal], *declaration) ||
                events[ordinal].event.program.programGeneration !=
                    declaration->programGeneration ||
                events[ordinal].event.ownerInstanceHandle !=
                    declaration->ownerInstance ||
                events[ordinal].event.ownerInstanceScheduleEpoch !=
                    declaration->ownerEpoch ||
                events[ordinal].event.program.program == nullptr ||
                events[ordinal].event.payload >=
                    events[ordinal].event.program.program->payloadCount ||
                !(events[ordinal].event.program.program->payloads[
                    events[ordinal].event.payload].target ==
                    declaration->target) ||
                events[ordinal].futureGroupedStart !=
                    declaration->future ||
                events[ordinal].event.frontier.kind !=
                    (declaration->future
                        ? FrontierOperationKind::Dependent
                        : FrontierOperationKind::Entry)) {
                rollback();
                return false;
            }
            memberRows[memberCount].order = declarationIndex;
            declaration->memberIndex = member;
            declaration->eventOrdinal = ordinal;
            declaration->secondEventOrdinal = UINT32_MAX;
            if (!ValidMemberEventOrdinals(*declaration, events, attempt->eventCount,
                    config_.quantumFrames)) {
                rollback();
                return false;
            }
            declaration->future =
                events[ordinal].futureGroupedStart;
            if (declaration->future) {
                bool bucketFound = false;
                for (std::uint32_t bucketIndex = 0;
                     bucketIndex < attempt->bucketCount; ++bucketIndex) {
                    auto& bucket = BucketBase(handle.slot)[bucketIndex];
                    if (ordinal < bucket.firstEvent ||
                        ordinal >= bucket.firstEvent + bucket.eventCount)
                        continue;
                    if (bucket.memberCount == UINT32_MAX) {
                        rollback();
                        return false;
                    }
                    if (bucket.groupCount != 0) {
                        bool ownsEntry = false;
                        for (std::uint32_t group = bucket.firstGroup;
                             group < bucket.firstGroup + bucket.groupCount;
                             ++group)
                            ownsEntry = ownsEntry ||
                                groups[group].identity == declaration->group;
                        if (!ownsEntry && groups[groupIndex].dueQuantum >= bucket.quantum) {
                            rollback();
                            return false;
                        }
                    }
                    ++bucket.memberCount;
                    bucketFound = true;
                    break;
                }
                if (!bucketFound) {
                    rollback();
                    return false;
                }
            }
            ++memberCount;
            groups[groupIndex].futureDependent =
                groups[groupIndex].futureDependent ||
                events[ordinal].futureGroupedStart;
        }
        if (groups[groupIndex].futureDependent &&
            !pool.AcquireImmediateLaneFutureDependent(
                groups[groupIndex].identity)) {
            rollback();
            return false;
        }
        if (groups[groupIndex].futureDependent)
            groupRows[groupIndex].futureOwned = 1;
    }
    if (memberCount != declaredCount) {
        rollback();
        return false;
    }
    attempt->groupCount = count;
    if (attempt->privateEntryCount != 0) {
        auto entries = PrivateEntries(handle.slot);
        for (std::uint32_t index = 0; index < attempt->privateEntryCount; ++index) {
            if (!BuildPrivateVariableControlEntryMapping(handle, *attempt,
                    entries[index], &entries[index].mapping)) {
                attempt->groupCount = 0;
                rollback();
                return false;
            }
            entries[index].mappingValid = true;
        }
        if (!FreezePrivateEntryAdmission(handle)) {
            attempt->groupCount = 0;
            rollback();
            return false;
        }
    }
    if (!pool.BindImmediateLaneGroupBuckets(
            groups, count, *this, handle.slot, handle.generation)) {
        attempt->groupCount = 0;
        rollback();
        return false;
    }
    groupPool_ = &pool;
    attempt->groupPinsInstalled = true;
    return true;
}

void ImmediateActionLane::InvalidatePrivateEntryMappings(
    ImmediateLaneHandle handle) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return;
    auto entries = PrivateEntries(handle.slot);
    for (std::uint32_t index = 0; index < attempt->privateEntryCount; ++index) {
        entries[index].mappingValid = false;
        entries[index].bucketCount = 0;
    }
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index)
        eventState_[offset + index].privateEntry = UINT32_MAX;
    for (std::uint32_t index = 0; index < attempt->pinGroupManifestCount; ++index)
        groupState_[offset + index].privateEntry = UINT32_MAX;
}

bool ImmediateActionLane::FreezePrivateEntryAdmission(ImmediateLaneHandle handle) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    auto entries = PrivateEntries(handle.slot);
    auto* eventRows = eventState_.At(offset);
    auto* groupRows = groupState_.At(offset);
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index)
        eventRows[index].privateEntry = UINT32_MAX;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index)
        groupRows[index].privateEntry = UINT32_MAX;
    const auto* buckets = BucketBase(handle.slot);
    const auto* groups = GroupBase(handle.slot);
    std::size_t arenaCount = 0;
    std::uint64_t memberCount = 0;
    const auto arenaCapacity = PrivateBucketCapacity({attempt->eventCredit, attempt->groupCredit,
        attempt->memberCredit, attempt->bucketCredit});
    for (std::uint32_t ordinal = 0; ordinal < attempt->privateEntryCount; ++ordinal) {
        auto& entry = entries[ordinal];
        if (!entry.mappingValid) return false;
        entry.bucketCount = 0;
        entry.buckets = metadata_[handle.slot].active.privateEntryBuckets + arenaCount;
        entry.bucketCapacity = static_cast<std::uint32_t>(
            std::min<std::size_t>(attempt->bucketCount, arenaCapacity - arenaCount));
        const auto addBucket = [&](std::uint32_t actualBucket) noexcept {
            if (actualBucket >= attempt->bucketCount) return false;
            if (PrivateEntryBucketFor(entry, actualBucket) != nullptr) return true;
            if (entry.bucketCount == entry.bucketCapacity) return false;
            auto& cell = entry.buckets[entry.bucketCount++];
            cell.ordinal = actualBucket;
            cell.audioSequence.store(0, std::memory_order_relaxed);
            cell.outcome.store(ImmediateLaneEntryOutcome::Dormant, std::memory_order_relaxed);
            return true;
        };
        const auto owns = [&](const Plan03GroupSliceIdentity& identity) noexcept {
            if (entry.ownedGroupCount == 0)
                return std::find(entry.groups.begin(), entry.groups.end(), identity) != entry.groups.end();
            const auto* ownedGroupRowsRead = groupState_.At(offset) + entry.firstOwnedGroup;
            for (std::uint32_t index = 0; index < entry.ownedGroupCount; ++index)
                if (declaredGroups_[offset + ownedGroupRowsRead[index].privateOwnedIndex].identity == identity) return true;
            return false;
        };
        for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
            if (!owns(groups[index].identity)) continue;
            if (groupRows[index].privateEntry != UINT32_MAX || !addBucket(groups[index].bucketOrdinal)) return false;
            groupRows[index].privateEntry = ordinal;
        }
        for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
            if (!owns(EventBase(handle.slot)[index].event.frontier.group)) continue;
            if (eventRows[index].privateEntry != UINT32_MAX) return false;
            std::uint32_t actualBucket = UINT32_MAX;
            for (std::uint32_t bucket = 0; bucket < attempt->bucketCount; ++bucket)
                if (index >= buckets[bucket].firstEvent &&
                    index - buckets[bucket].firstEvent < buckets[bucket].eventCount) {
                    if (actualBucket != UINT32_MAX) return false;
                    actualBucket = bucket;
                }
            if (!addBucket(actualBucket)) return false;
            eventRows[index].privateEntry = ordinal;
        }
        arenaCount += entry.bucketCount;
        memberCount += entry.memberCount;
        if (PrivateEntryBucketFor(entry, entry.mapping.coordinatorBucket) == nullptr)
            return false;
    }
    return memberCount == attempt->memberCount &&
        std::none_of(eventRows, eventRows + attempt->eventCount,
                       [](const auto& value) { return value.privateEntry == UINT32_MAX; }) &&
        std::none_of(groupRows, groupRows + attempt->groupCount,
                     [](const auto& value) { return value.privateEntry == UINT32_MAX; });
}

const ImmediateActionLane::PrivateEntryBucket* ImmediateActionLane::PrivateEntryBucketFor(
    const PrivateEntryAssociation& entry, std::uint32_t ordinal) const noexcept {
    for (std::uint32_t index = 0; index < entry.bucketCount; ++index)
        if (entry.buckets[index].ordinal == ordinal) return &entry.buckets[index];
    return nullptr;
}

bool ImmediateActionLane::MatchesPrivateEntryAssociation(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& identity) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->root != root ||
        ordinal >= attempt->privateEntryCount ||
        attempt->state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Free)
        return false;
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    return entry.entry == identity && entry.pool != nullptr &&
        entry.decisions != nullptr && entry.armTable != nullptr &&
        entry.armTableIncarnation == entry.armTable->Incarnation();
}

bool ImmediateActionLane::CapturePrivateEntryBucketShape(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& identity,
    ImmediateLanePrivateEntryBucketShape* out) const noexcept {
    if (out == nullptr || !MatchesPrivateEntryAssociation(handle, root, ordinal, identity))
        return false;
    const auto* attempt = Resolve(handle);
    const auto phase = attempt->state.load(std::memory_order_acquire);
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    if ((phase != ImmediateLaneAttemptState::Prepared &&
         phase != ImmediateLaneAttemptState::Committed) ||
        !attempt->pinsInstalled || !attempt->groupPinsInstalled ||
        !entry.mappingValid || entry.bucketCount == 0 ||
        entry.buckets == nullptr || entry.bucketCount > entry.bucketCapacity ||
        entry.bucketCount > kMaxKnownScheduleLeaves + 3u) return false;
    ImmediateLanePrivateEntryBucketShape actual{};
    actual.count = entry.bucketCount;
    actual.coordinator = entry.mapping.coordinatorBucket;
    for (std::uint32_t index = 0; index < actual.count; ++index)
        actual.ordinals[index] = entry.buckets[index].ordinal;
    *out = actual;
    return true;
}

bool ImmediateActionLane::CapturePrivateEntryTerminalProjection(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& identity, std::uint64_t completedAudioSequence,
    ImmediateLanePrivateEntryTerminalProjection* out) const noexcept {
    if (out == nullptr || !MatchesPrivateEntryAssociation(handle, root, ordinal, identity))
        return false;
    const auto* attempt = Resolve(handle);
    AttemptReadAccess access(*attempt);
    if (!access || attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->nextBucket.load(std::memory_order_acquire) != attempt->bucketCount)
        return false;
    const auto phase = attempt->state.load(std::memory_order_acquire);
    if (phase != ImmediateLaneAttemptState::Committed &&
        phase != ImmediateLaneAttemptState::Cancelling &&
        phase != ImmediateLaneAttemptState::PrivateCancelled) return false;
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    if (!entry.mappingValid || entry.bucketCount == 0 ||
        entry.buckets == nullptr || entry.bucketCount > entry.bucketCapacity ||
        entry.bucketCount > kMaxKnownScheduleLeaves + 3u) return false;
    ImmediateLanePrivateEntryTerminalProjection actual{};
    actual.registeredEntryCount = attempt->privateEntryCount;
    actual.shape.count = entry.bucketCount;
    actual.shape.coordinator = entry.mapping.coordinatorBucket;
    for (std::uint32_t index = 0; index < entry.bucketCount; ++index) {
        const auto& cell = entry.buckets[index];
        const auto bucketOrdinal = cell.ordinal;
        if (bucketOrdinal >= attempt->bucketCount) return false;
        actual.shape.ordinals[index] = bucketOrdinal;
        const auto outcome = cell.outcome.load(std::memory_order_acquire);
        const auto sequence = cell.audioSequence.load(std::memory_order_relaxed);
        if (outcome == ImmediateLaneEntryOutcome::Applied ||
            outcome == ImmediateLaneEntryOutcome::Withdrawn ||
            outcome == ImmediateLaneEntryOutcome::NotApplied) {
            if (sequence == 0 || sequence > completedAudioSequence) return false;
        } else {
            const auto state = BucketBase(handle.slot)[bucketOrdinal].state.load(
                std::memory_order_acquire);
            if (outcome != ImmediateLaneEntryOutcome::Dormant || sequence != 0 ||
                (phase != ImmediateLaneAttemptState::Cancelling &&
                 phase != ImmediateLaneAttemptState::PrivateCancelled) ||
                (state != ImmediateLaneBucketState::Committed &&
                 state != ImmediateLaneBucketState::Cancelled &&
                 state != ImmediateLaneBucketState::Missed)) return false;
            actual.omitted.set(index);
        }
        actual.outcomes[index] = {outcome, sequence, bucketOrdinal};
    }
    *out = actual;
    return true;
}

bool ImmediateActionLane::CapturePrivateEntryBucketShape(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal, const FrontierEntryReceiptRef& identity,
    ImmediateLanePrivateEntryResultView* out) const noexcept {
    if (out == nullptr || out->cells == nullptr ||
        !MatchesPrivateEntryAssociation(handle, root, ordinal, identity)) return false;
    const auto* attempt = Resolve(handle);
    const auto phase = attempt->state.load(std::memory_order_acquire);
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    if ((phase != ImmediateLaneAttemptState::Prepared && phase != ImmediateLaneAttemptState::Committed) ||
        !attempt->pinsInstalled || !attempt->groupPinsInstalled || !entry.mappingValid ||
        entry.bucketCount == 0 || entry.buckets == nullptr || entry.bucketCount > entry.bucketCapacity ||
        entry.bucketCount > out->capacity) return false;
    for (std::uint32_t index = 0; index < entry.bucketCount; ++index)
        if (entry.buckets[index].ordinal >= attempt->bucketCount) return false;
    for (std::uint32_t index = 0; index < entry.bucketCount; ++index) {
        out->cells[index] = {};
        out->cells[index].bucketOrdinal = entry.buckets[index].ordinal;
    }
    out->count = entry.bucketCount;
    out->coordinator = entry.mapping.coordinatorBucket;
    out->registeredEntryCount = 0;
    return true;
}

bool ImmediateActionLane::CapturePrivateEntryTerminalProjection(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal, const FrontierEntryReceiptRef& identity,
    std::uint64_t completedAudioSequence, ImmediateLanePrivateEntryResultView* out) const noexcept {
    if (out == nullptr || out->cells == nullptr ||
        !MatchesPrivateEntryAssociation(handle, root, ordinal, identity)) return false;
    const auto* attempt = Resolve(handle);
    AttemptReadAccess access(*attempt);
    if (!access || attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->nextBucket.load(std::memory_order_acquire) != attempt->bucketCount) return false;
    const auto phase = attempt->state.load(std::memory_order_acquire);
    if (phase != ImmediateLaneAttemptState::Committed && phase != ImmediateLaneAttemptState::Cancelling &&
        phase != ImmediateLaneAttemptState::PrivateCancelled) return false;
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    if (!entry.mappingValid || entry.bucketCount == 0 || entry.buckets == nullptr ||
        entry.bucketCount > entry.bucketCapacity || entry.bucketCount > out->capacity ||
        entry.bucketCount != out->count || entry.mapping.coordinatorBucket != out->coordinator) return false;
    // Validate the whole projection before changing stable recipient storage.
    for (std::uint32_t index = 0; index < entry.bucketCount; ++index) {
        const auto& cell = entry.buckets[index];
        if (cell.ordinal >= attempt->bucketCount || out->cells[index].bucketOrdinal != cell.ordinal) return false;
        const auto outcome = cell.outcome.load(std::memory_order_acquire);
        const auto sequence = cell.audioSequence.load(std::memory_order_relaxed);
        if (outcome == ImmediateLaneEntryOutcome::Applied || outcome == ImmediateLaneEntryOutcome::Withdrawn ||
            outcome == ImmediateLaneEntryOutcome::NotApplied) {
            if (sequence == 0 || sequence > completedAudioSequence) return false;
        } else {
            const auto state = BucketBase(handle.slot)[cell.ordinal].state.load(std::memory_order_acquire);
            if (outcome != ImmediateLaneEntryOutcome::Dormant || sequence != 0 ||
                (phase != ImmediateLaneAttemptState::Cancelling && phase != ImmediateLaneAttemptState::PrivateCancelled) ||
                (state != ImmediateLaneBucketState::Committed && state != ImmediateLaneBucketState::Cancelled &&
                 state != ImmediateLaneBucketState::Missed)) return false;
        }
    }
    for (std::uint32_t index = 0; index < entry.bucketCount; ++index) {
        const auto& cell = entry.buckets[index];
        auto& result = out->cells[index];
        const auto outcome = cell.outcome.load(std::memory_order_acquire);
        result.outcome = {outcome, cell.audioSequence.load(std::memory_order_relaxed), cell.ordinal};
        result.omitted = outcome == ImmediateLaneEntryOutcome::Dormant;
    }
    out->registeredEntryCount = attempt->privateEntryCount;
    return true;
}

bool ImmediateActionLane::RequestPrivateEntryStop(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& identity) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePublicationProof != nullptr || attempt->root != root ||
        ordinal >= attempt->privateEntryCount) return false;
    const auto phase = attempt->state.load(std::memory_order_acquire);
    if (phase != ImmediateLaneAttemptState::Reserved &&
        phase != ImmediateLaneAttemptState::Prepared &&
        phase != ImmediateLaneAttemptState::Committed) return false;
    auto& entry = PrivateEntries(handle.slot)[ordinal];
    if (!(entry.entry == identity) || entry.armTable == nullptr ||
        entry.armTableIncarnation != entry.armTable->Incarnation()) return false;
    // The logic producer owns immutable declaration identities. Stop only
    // contests admission; it does not borrow the audio capture gate.
    (void)entry.admission.fetch_or(kEntryStopRequested, std::memory_order_acq_rel);
    return true;
}

bool ImmediateActionLane::CanCancelPrivateEntrySources(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& identity) const noexcept {
    if (!MatchesPrivateEntryAssociation(handle, root, ordinal, identity)) return false;
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    const auto admission = entry.admission.load(std::memory_order_acquire);
    // Stop prevents a new audio claim; an already captured Entry must finish first.
    return (admission & kEntryStopRequested) != 0 &&
        (admission & kEntryAudioAdmitting) == 0;
}

bool ImmediateActionLane::CapturePrivateEntryOutcome(ImmediateLaneHandle handle,
    std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& identity, std::uint32_t bucketOrdinal,
    std::uint64_t completedAudioSequence,
    ImmediateLaneEntryOutcomeSnapshot* out) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || out == nullptr || attempt->root != root ||
        ordinal >= attempt->privateEntryCount) return false;
    const auto& entry = PrivateEntries(handle.slot)[ordinal];
    if (!entry.mappingValid || !(entry.entry == identity)) return false;
    const auto* bucket = PrivateEntryBucketFor(entry, bucketOrdinal);
    if (bucket == nullptr) return false;
    const auto outcome = bucket->outcome.load(std::memory_order_acquire);
    if (outcome != ImmediateLaneEntryOutcome::Applied &&
        outcome != ImmediateLaneEntryOutcome::Withdrawn &&
        outcome != ImmediateLaneEntryOutcome::NotApplied) return false;
    const auto sequence = bucket->audioSequence.load(std::memory_order_relaxed);
    if (sequence == 0 || sequence > completedAudioSequence) return false;
    *out = {outcome, sequence, bucketOrdinal};
    return true;
}

void ImmediateActionLane::BeginPrivateEntryAdmission(std::uint32_t slot,
    std::uint32_t bucketOrdinal) noexcept {
    auto& attempt = attempts_[slot];
    auto entries = PrivateEntries(slot);
    for (std::uint32_t ordinal = 0; ordinal < attempt.privateEntryCount; ++ordinal) {
        auto& entry = entries[ordinal];
        if (!entry.mappingValid) continue;
        auto* bucket = const_cast<PrivateEntryBucket*>(PrivateEntryBucketFor(entry, bucketOrdinal));
        if (bucket == nullptr || bucket->outcome.load(std::memory_order_relaxed) !=
                ImmediateLaneEntryOutcome::Dormant) continue;
        std::uint8_t expected = 0;
        if (entry.admission.compare_exchange_strong(expected, kEntryAudioAdmitting,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            bucket->outcome.store(ImmediateLaneEntryOutcome::Admitting,
                                 std::memory_order_relaxed);
        } else if ((expected & kEntryStopRequested) != 0 &&
                   (expected & kEntryAudioAdmitting) == 0) {
            bucket->outcome.store(ImmediateLaneEntryOutcome::Withdrawing,
                                 std::memory_order_relaxed);
        }
    }
}

void ImmediateActionLane::EndPrivateEntryAdmission(std::uint32_t slot,
    const ImmediateLaneBucket* captured, bool applied) noexcept {
    auto& attempt = attempts_[slot];
    if (attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        captured != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        captured->quantum == UINT64_MAX) return;
    auto entries = PrivateEntries(slot);
    for (std::uint32_t ordinal = 0; ordinal < attempt.privateEntryCount; ++ordinal) {
        auto& entry = entries[ordinal];
        auto* bucket = const_cast<PrivateEntryBucket*>(
            PrivateEntryBucketFor(entry, attempt.capturedBucketOrdinal));
        if (bucket == nullptr) continue;
        const auto previous = bucket->outcome.load(std::memory_order_relaxed);
        if (previous != ImmediateLaneEntryOutcome::Admitting &&
            previous != ImmediateLaneEntryOutcome::Withdrawing) continue;
        bucket->audioSequence.store(attempt.capturedAudioSequence, std::memory_order_relaxed);
        bucket->outcome.store(previous == ImmediateLaneEntryOutcome::Withdrawing
            ? ImmediateLaneEntryOutcome::Withdrawn
            : (applied ? ImmediateLaneEntryOutcome::Applied :
                         ImmediateLaneEntryOutcome::NotApplied), std::memory_order_release);
        if (previous == ImmediateLaneEntryOutcome::Admitting)
            (void)entry.admission.fetch_and(
                static_cast<std::uint8_t>(~kEntryAudioAdmitting), std::memory_order_release);
    }
}

bool ImmediateActionLane::HasRegisteredEntries(std::uint32_t slot,
    std::uint32_t generation) const noexcept {
    return valid_ && slot < attempts_.size() && generation != 0 &&
        attempts_[slot].generation == generation && attempts_[slot].privateEntryCount != 0;
}

bool ImmediateActionLane::CapturedEntryProjectionValid(std::uint32_t slot,
    const ImmediateLaneBucket* bucket) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return false;
    const auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation) return false;
    const auto entries = PrivateEntries(slot);
    for (std::uint32_t ordinal = 0; ordinal < attempt.privateEntryCount; ++ordinal) {
        const auto& entry = entries[ordinal];
        if (!entry.mappingValid) return false;
        const auto* current = PrivateEntryBucketFor(entry, attempt.capturedBucketOrdinal);
        if (current == nullptr) continue;
        const auto phase = current->outcome.load(std::memory_order_relaxed);
        if (phase != ImmediateLaneEntryOutcome::Admitting &&
            phase != ImmediateLaneEntryOutcome::Withdrawing) return false;
    }
    return true;
}

void ImmediateActionLane::BeginEventAdmission(std::uint32_t slot,
    const ImmediateLaneBucket& bucket) noexcept {
    const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    for (std::uint32_t index = bucket.firstEvent;
         index < bucket.firstEvent + bucket.eventCount; ++index) {
        const auto scope = eventState_[offset + index].withdrawalLeader;
        std::uint8_t expected = 0;
        bool admitted = eventState_[offset + scope].withdrawal.compare_exchange_strong(
            expected, kEventAudioAdmitting, std::memory_order_acq_rel, std::memory_order_acquire);
        if (admitted) {
            eventState_[offset + scope].lastCaptureSequence = attempts_[slot].capturedAudioSequence;
#if !defined(HARKEN_RT_QUALIFICATION)
            if (eventAdmissionHook_ != nullptr) eventAdmissionHook_(eventAdmissionContext_, index);
#endif
        }
        else admitted = (expected & kEventAudioAdmitting) != 0 &&
            eventState_[offset + scope].lastCaptureSequence == attempts_[slot].capturedAudioSequence;
        assert(admitted || (expected & kEventWithdrawalRequested) != 0);
        eventState_[offset + index].capturedAdmission = admitted ? 1u : 0u;
        if (admitted) {
            const auto* program = EventBase(slot)[index].event.program.program;
            if (program != nullptr && program->readFence != nullptr)
                program->readFence->RecordRead(attempts_[slot].capturedAudioSequence - 1u);
        }
    }
}

void ImmediateActionLane::EndEventAdmission(std::uint32_t slot,
    const ImmediateLaneBucket& bucket) noexcept {
    const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    for (std::uint32_t index = bucket.firstEvent;
         index < bucket.firstEvent + bucket.eventCount; ++index) {
        if (eventState_[offset + index].capturedAdmission == 0) continue;
        (void)eventState_[offset + eventState_[offset + index].withdrawalLeader].withdrawal.fetch_and(
            static_cast<std::uint8_t>(~kEventAudioAdmitting), std::memory_order_release);
    }
}

bool ImmediateActionLane::IsEventAdmitted(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, std::uint32_t globalIndex) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return false;
    const auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation) return false;
    if (globalIndex >= attempt.eventCount || globalIndex < bucket->firstEvent ||
        globalIndex - bucket->firstEvent >= bucket->eventCount) return false;
    if (eventState_[static_cast<std::size_t>(slot) *
            config_.eventsPerAttempt + globalIndex].capturedAdmission == 0) return false;
    if (ConditionalProjectionEvent(slot, globalIndex) &&
        !attempt.capturedConditionalProjection) return false;
    if (attempt.privateEntryCount == 0) return true;
    const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    const auto ordinal = eventState_[offset + globalIndex].privateEntry;
    if (ordinal >= attempt.privateEntryCount) return false;
    if (!PrivateEntryAt(offset + ordinal).mappingValid) return false;
    const auto* current = PrivateEntryBucketFor(PrivateEntryAt(offset + ordinal),
                                                attempt.capturedBucketOrdinal);
    return current != nullptr && current->outcome.load(std::memory_order_relaxed) ==
        ImmediateLaneEntryOutcome::Admitting;
}

bool ImmediateActionLane::IsGroupAdmitted(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, std::uint32_t globalIndex) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return false;
    const auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation) return false;
    if (globalIndex >= attempt.groupCount || globalIndex < bucket->firstGroup ||
        globalIndex - bucket->firstGroup >= bucket->groupCount) return false;
    if (ConditionalProjectionGroup(slot, globalIndex) &&
        !attempt.capturedConditionalProjection) return false;
    if (attempt.privateEntryCount == 0) return true;
    const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    const auto ordinal = groupState_[offset + globalIndex].privateEntry;
    if (ordinal >= attempt.privateEntryCount) return false;
    if (!PrivateEntryAt(offset + ordinal).mappingValid) return false;
    const auto* current = PrivateEntryBucketFor(PrivateEntryAt(offset + ordinal),
                                                attempt.capturedBucketOrdinal);
    return current != nullptr && current->outcome.load(std::memory_order_relaxed) ==
        ImmediateLaneEntryOutcome::Admitting;
}

bool ImmediateActionLane::AllCapturedEntriesWithdrawing(std::uint32_t slot,
    const ImmediateLaneBucket* bucket) const noexcept {
    if (!CapturedEntryProjectionValid(slot, bucket) ||
        (bucket->eventCount == 0 && bucket->groupCount == 0)) return false;
    for (std::uint32_t i = 0; i < bucket->eventCount; ++i)
        if (IsEventAdmitted(slot, bucket, bucket->firstEvent + i)) return false;
    for (std::uint32_t i = 0; i < bucket->groupCount; ++i)
        if (IsGroupAdmitted(slot, bucket, bucket->firstGroup + i)) return false;
    return true;
}

bool ImmediateActionLane::CanAdmitFutureEvent(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, std::uint32_t globalIndex) const noexcept {
    if (!IsEventAdmitted(slot, bucket, globalIndex)) return false;
    const auto& attempt = attempts_[slot];
    if (attempt.privateEntryCount == 0)
        return bucket->groupCount != 0 || EntryApplied(slot, attempt.generation);
    const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    const auto& entry = PrivateEntryAt(offset + eventState_[offset + globalIndex].privateEntry);
    const auto* coordinator = PrivateEntryBucketFor(entry, entry.mapping.coordinatorBucket);
    if (coordinator == nullptr) return false;
    const auto outcome = coordinator->outcome.load(std::memory_order_acquire);
    return outcome == ImmediateLaneEntryOutcome::Applied ||
        (entry.mapping.coordinatorBucket == attempt.capturedBucketOrdinal &&
         outcome == ImmediateLaneEntryOutcome::Admitting);
}

bool ImmediateActionLane::BuildPrivateVariableControlEntryMapping(
    ImmediateLaneHandle handle, const ImmediateLaneAttempt& attempt,
    const PrivateEntryAssociation& entry,
    ImmediateLanePrivateVariableControlEntryMapping* out) const noexcept {
    if (entry.ownedGroupCount != 0)
        return BuildAcceptedPrivateEntryMapping(handle, attempt, entry, out);
    const auto entryCount = static_cast<std::uint64_t>(attempt.privateEntryCount);
    if (out == nullptr || entryCount == 0 ||
        attempt.groupCount != 3u * entryCount ||
        attempt.eventCount != 3u * entryCount + attempt.memberCount ||
        attempt.pinGroupManifestCount != attempt.groupCount ||
        entry.pool == nullptr || entry.armTable == nullptr || entry.decisions == nullptr ||
        attempt.pinGroupPool != entry.pool ||
        entry.armTableIncarnation != entry.armTable->Incarnation() ||
        entry.memberCount == 0 || entry.memberCount > kMaxKnownScheduleLeaves ||
        static_cast<std::uint64_t>(entry.firstMember) + entry.memberCount > attempt.memberCount)
        return false;
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* groupPins = groupPins_.At(offset);
    const auto* groupRowsRead = groupState_.At(offset);
    const auto* members = MemberBase(handle.slot) + entry.firstMember;
    const auto* groups = GroupBase(handle.slot);
    const auto* events = EventBase(handle.slot);
    ImmediateLanePrivateVariableControlEntryMapping mapping{};
    mapping.firstMember = entry.firstMember;
    mapping.memberCount = entry.memberCount;
    std::array<Plan03PageGroupView, 3> views{};
    std::array<std::uint32_t, 2> sideBases{};
    std::uint32_t memberBase = 0;
    for (std::uint32_t side = 0; side < views.size(); ++side) {
        for (std::uint32_t index = 0; index < attempt.groupCount; ++index) {
            if (!(groups[index].identity == entry.groups[side])) continue;
            if (mapping.groups[side] != UINT32_MAX ||
                groups[index].laneSlot != handle.slot ||
                groups[index].laneGeneration != handle.generation ||
                groups[index].groupIndex != index ||
                groups[index].bucketOrdinal >= attempt.bucketCount ||
                !groupPins[index].Valid() ||
                !(groupPins[index].Identity() == entry.groups[side]) ||
                !groupRowsRead[index].decision.Valid() || groupRowsRead[index].decision.Owner() != entry.decisions ||
                groupRowsRead[index].decision.Token().slot != entry.groups[side].decision.slot ||
                groupRowsRead[index].decision.Token().generation != entry.groups[side].decision.generation ||
                !entry.pool->ResolvePlan03GroupSliceProducer(entry.groups[side], &views[side]))
                return false;
            mapping.groups[side] = index;
        }
        if (mapping.groups[side] == UINT32_MAX || views[side].frontier == nullptr ||
            views[side].lease == nullptr || !views[side].frontier->Valid()) return false;
        const auto& binding = *views[side].frontier;
        if (!(binding.entry == entry.entry) || !(binding.primary == entry.groups[2]) ||
            !(binding.operation.group == entry.groups[side]) ||
            binding.operation.kind != FrontierOperationKind::Entry ||
            binding.operation.world != entry.coordinatorRequest.world ||
            binding.operation.transport != entry.coordinatorRequest.transport ||
            binding.operation.order.executeFrame != attempt.anchorFrame ||
            binding.operation.order.logicCommitSequence != entry.coordinatorRequest.ticket.sequence ||
            groups[mapping.groups[side]].dueQuantum != attempt.anchorFrame / config_.quantumFrames)
            return false;
        if (side == 2) {
            if (binding.role != FrontierGroupRole::Coordinator ||
                !(binding.operation.owner == entry.entry.owner) ||
                views[side].memberCount != 0 || views[side].sourceControlCount != 0)
                return false;
        } else {
            if (binding.role != FrontierGroupRole::EntryMember ||
                !binding.variableSide.Valid() || binding.variableSide.side != side ||
                views[side].memberCount == 0 || views[side].sourceControlCount != 0 ||
                views[side].members == nullptr || views[side].memberCount > entry.memberCount - memberBase)
                return false;
            sideBases[side] = memberBase;
            for (std::uint32_t member = 0; member < views[side].memberCount; ++member) {
                const auto& manifest = members[memberBase + member];
                if (!(manifest.group == entry.groups[side]) ||
                    !SameArm(views[side].members[member], manifest.arm) ||
                    manifest.kind != ImmediateLaneMemberKind::SingleEvent ||
                    manifest.memberIndex != member || manifest.eventSpanCount != 1 ||
                    manifest.secondEventOrdinal != UINT32_MAX) return false;
            }
            memberBase += views[side].memberCount;
        }
    }
    if (memberBase != entry.memberCount) return false;
    const auto* activation = entry.decisions->ResolveFrontierActivation(entry.entry.coordinator);
    DecisionCommittedSnapshot committed{};
    if (activation == nullptr || !entry.decisions->CaptureCommitted(
            entry.entry.coordinator, &committed)) return false;
    const auto& descriptor = activation->Descriptor();
    const auto& request = entry.coordinatorRequest;
    if (!descriptor.Valid() || !(descriptor.entry == entry.entry) ||
        !(descriptor.primary == entry.groups[2]) ||
        !(descriptor.order == views[2].frontier->operation.order) ||
        descriptor.worldGeneration != request.world || descriptor.expectedMemberCount != 2 ||
        committed.commit != DecisionCommitState::Committed ||
        committed.ticket.sequence != request.ticket.sequence || committed.world != request.world ||
        committed.transport != request.transport ||
        committed.continuationGeneration != request.expectedContinuationGeneration ||
        committed.sliceCount != request.sliceCount ||
        committed.transitionGate.slot != request.transitionGate.slot ||
        committed.transitionGate.generation != request.transitionGate.generation ||
        committed.participantCount != 2 || entry.groups[2].committedRow > UINT32_MAX ||
        committed.pages.slot != entry.groups[2].committedRow ||
        committed.pages.generation != entry.groups[2].rowClaimGeneration ||
        committed.audioState != AudioGroupState::Pending || committed.consumedSlices != 0 ||
        committed.lastAudioSequence != 0 || committed.futurePinCount == 0 ||
        committed.authoredStopInvalidated) return false;
    std::array<bool, 2> participantFound{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        DecisionParticipantRef participant{};
        if (!entry.decisions->CaptureCommittedParticipant(entry.entry.coordinator, index,
                &participant) || participant.kind != DecisionParticipantKind::Subdecision)
            return false;
        bool matched = false;
        for (std::uint32_t side = 0; side < 2; ++side) {
            if (participant.slot != entry.groups[side].decision.slot ||
                participant.generation != entry.groups[side].decision.generation) continue;
            if (participantFound[side]) return false;
            participantFound[side] = matched = true;
        }
        if (!matched) return false;
    }
    std::uint32_t nonFutureCount = 0;
    std::array<std::uint32_t, 3> groupEvents{};
    std::array<bool, kMaxKnownScheduleLeaves> memberFound{};
    std::bitset<kMaxKnownScheduleLeaves + 3u> eventFound{};
    for (std::uint32_t index = 0; index < attempt.eventCount; ++index) {
        const auto& lane = events[index];
        const auto& event = lane.event;
        const auto& frontier = event.frontier;
        if (!(frontier.entry == entry.entry)) continue;
        const auto local = frontier.entryEventOrdinal;
        std::uint64_t effectiveDue = 0;
        if (local >= entry.memberCount + 3u || eventFound[local] ||
            !frontier.Valid() || frontier.operationEventCount != 1 ||
            frontier.world != request.world || frontier.transport != request.transport ||
            frontier.kind != (lane.futureGroupedStart ? FrontierOperationKind::Dependent :
                FrontierOperationKind::Entry) ||
            !ResolveLaneDue(lane, attempt.anchorFrame, &effectiveDue) ||
            event.executeFrame != effectiveDue ||
            event.logicCommitSequence != request.ticket.sequence ||
            event.executionOrdinal != descriptor.order.executionOrdinal ||
            event.program.program == nullptr || event.program.program->payloads == nullptr ||
            event.program.programGeneration != event.program.program->generation ||
            event.payload >= event.program.program->payloadCount || !event.frozenTxnValid)
            return false;
        std::uint32_t side = UINT32_MAX;
        for (std::uint32_t candidate = 0; candidate < 3; ++candidate)
            if (frontier.group == entry.groups[candidate]) side = candidate;
        if (side == UINT32_MAX ||
            !(frontier.owner == views[side].frontier->operation.owner)) return false;
        const auto& payload = event.program.program->payloads[event.payload];
        if (local < 3) {
            const auto expectedSide = local == 0 ? 2u : local - 1u;
            const TimelineExecutionOrder order{event.executeFrame, event.logicCommitSequence,
                event.executionOrdinal, event.eventIndex, event.transactionOrdinal,
                event.subeventPhase, event.rowSequence};
            if (side != expectedSide || lane.timeDomain != ImmediateLaneTimeDomain::RelativeToAnchor ||
                lane.relativeFrame != 0 || lane.futureGroupedStart ||
                event.armToken.Valid() || payload.kind != TimelineEventKind::ControllerActivation ||
                event.program.program->role != TimelineProgramRole::ControllerActivation ||
                event.program.programGeneration != descriptor.programGeneration ||
                !(order == views[side].frontier->operation.order) ||
                (side < 2 && (views[side].frontier->operation.program != event.program.program ||
                    views[side].frontier->operation.programGeneration != event.program.programGeneration)))
                return false;
            mapping.events[local] = index;
        } else {
            if (side >= 2) return false;
            std::uint32_t member = UINT32_MAX;
            for (std::uint32_t candidate = sideBases[side];
                 candidate < sideBases[side] + views[side].memberCount; ++candidate) {
                if (!SameArm(event.armToken, members[candidate].arm)) continue;
                if (member != UINT32_MAX) return false;
                member = candidate;
            }
            if (member == UINT32_MAX || memberFound[member] ||
                !SameTimeSpec(lane, members[member]) ||
                lane.futureGroupedStart != members[member].future ||
                event.program.programGeneration != members[member].programGeneration ||
                event.ownerInstanceHandle != members[member].ownerInstance ||
                event.ownerInstanceScheduleEpoch != members[member].ownerEpoch ||
                !(payload.target == members[member].target) ||
                payload.kind != TimelineEventKind::Start ||
                event.program.program->role != TimelineProgramRole::Voice ||
                members[member].eventOrdinal != index) return false;
            memberFound[member] = true;
        }
        eventFound.set(local);
        ++groupEvents[side];
        if (!lane.futureGroupedStart) ++nonFutureCount;
    }
    for (const auto index : mapping.events) if (index == UINT32_MAX) return false;
    for (std::uint32_t member = 0; member < entry.memberCount; ++member)
        if (!memberFound[member]) return false;
    for (std::uint32_t local = 0; local < entry.memberCount + 3u; ++local)
        if (!eventFound[local]) return false;
    if (groupEvents[0] != 1u + views[0].memberCount ||
        groupEvents[1] != 1u + views[1].memberCount || groupEvents[2] != 1 ||
        descriptor.expectedEntryEventCount != nonFutureCount) return false;
    mapping.coordinatorBucket = groups[mapping.groups[2]].bucketOrdinal;
    const auto& bucket = BucketBase(handle.slot)[mapping.coordinatorBucket];
    if (mapping.events[0] < bucket.firstEvent ||
        mapping.events[0] >= static_cast<std::uint64_t>(bucket.firstEvent) + bucket.eventCount)
        return false;
    *out = mapping;
    return true;
}

bool ImmediateActionLane::BuildAcceptedPrivateEntryMapping(
    ImmediateLaneHandle handle, const ImmediateLaneAttempt& attempt,
    const PrivateEntryAssociation& entry,
    ImmediateLanePrivateVariableControlEntryMapping* out) const noexcept {
    if (out == nullptr || attempt.pinGroupManifestCount != attempt.groupCount ||
        entry.pool == nullptr || entry.armTable == nullptr || entry.decisions == nullptr ||
        attempt.pinGroupPool != entry.pool || entry.armTableIncarnation != entry.armTable->Incarnation() ||
        entry.firstOwnedGroup > attempt.groupCount || entry.ownedGroupCount == 0 ||
        entry.ownedGroupCount > attempt.groupCount - entry.firstOwnedGroup ||
        entry.firstMember > attempt.memberCount || entry.memberCount == 0 ||
        entry.memberCount > attempt.memberCount - entry.firstMember) return false;
    const auto offset = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* ownedGroupRowsRead = groupState_.At(offset) + entry.firstOwnedGroup;
    const auto* declaredGroups = declaredGroups_.At(offset);
    const auto* groups = GroupBase(handle.slot);
    const auto* events = EventBase(handle.slot);
    const auto* members = MemberBase(handle.slot) + entry.firstMember;
    const auto owns = [&](const Plan03GroupSliceIdentity& identity) noexcept {
        std::uint32_t matches = 0;
        for (std::uint32_t index = 0; index < entry.ownedGroupCount; ++index)
            matches += ownedGroupRowsRead[index].privateOwnedIndex < attempt.declaredGroupCount &&
                declaredGroups[ownedGroupRowsRead[index].privateOwnedIndex].identity == identity;
        return matches == 1u;
    };
    ImmediateLanePrivateVariableControlEntryMapping mapping{};
    mapping.firstMember = entry.firstMember;
    mapping.memberCount = entry.memberCount;
    mapping.ownedGroupCount = entry.ownedGroupCount;
    std::uint32_t participants = 0;
    const auto& request = entry.coordinatorRequest;
    for (std::uint32_t ownedIndex = 0; ownedIndex < entry.ownedGroupCount; ++ownedIndex) {
        if (ownedGroupRowsRead[ownedIndex].privateOwnedIndex >= attempt.declaredGroupCount) return false;
        const auto& original = declaredGroups[ownedGroupRowsRead[ownedIndex].privateOwnedIndex];
        std::uint32_t actualIndex = UINT32_MAX;
        for (std::uint32_t index = 0; index < attempt.groupCount; ++index)
            if (groups[index].identity == original.identity) {
                if (actualIndex != UINT32_MAX) return false;
                actualIndex = index;
            }
        Plan03PageGroupView view{};
        std::uint64_t due = 0;
        if (actualIndex == UINT32_MAX || !ResolveLaneDue(original, attempt.anchorFrame, &due) ||
            groups[actualIndex].laneSlot != handle.slot || groups[actualIndex].laneGeneration != handle.generation ||
            groups[actualIndex].groupIndex != actualIndex || groups[actualIndex].bucketOrdinal >= attempt.bucketCount ||
            groups[actualIndex].dueQuantum != due / config_.quantumFrames ||
            !groupPins_[offset + actualIndex].Valid() ||
            !(groupPins_[offset + actualIndex].Identity() == original.identity) ||
            !groupState_[offset + actualIndex].decision.Valid() ||
            groupState_[offset + actualIndex].decision.Owner() != entry.decisions ||
            groupState_[offset + actualIndex].decision.Token().slot != original.identity.decision.slot ||
            groupState_[offset + actualIndex].decision.Token().generation != original.identity.decision.generation ||
            !entry.pool->ResolvePlan03GroupSliceProducer(original.identity, &view) ||
            view.frontier == nullptr || view.lease == nullptr || !view.frontier->Valid()) return false;
        const auto& binding = *view.frontier;
        if (!owns(binding.primary) || !(binding.operation.group == original.identity) ||
            binding.operation.world != request.world || binding.operation.transport != request.transport ||
            binding.operation.order.executeFrame != due ||
            binding.operation.order.logicCommitSequence != request.ticket.sequence ||
            binding.operation.owner.invocation != entry.entry.owner.invocation ||
            binding.operation.owner.invocationEpoch != entry.entry.owner.invocationEpoch ||
            (binding.prerequisiteMember.Exact() && !owns(binding.prerequisiteMember)) ||
            (binding.prerequisite.Valid() && !(binding.prerequisite == entry.entry))) return false;
        if (binding.entry == entry.entry && binding.role == FrontierGroupRole::EntryMember) {
            if (binding.operation.kind != FrontierOperationKind::Entry) return false;
            ++participants;
        }
        for (std::uint32_t side = 0; side < entry.groups.size(); ++side) {
            if (!(original.identity == entry.groups[side])) continue;
            if (mapping.groups[side] != UINT32_MAX || !(binding.entry == entry.entry) ||
                !(binding.primary == entry.groups[2]) || due != attempt.anchorFrame ||
                binding.operation.kind != FrontierOperationKind::Entry) return false;
            if (side == 2u ? (binding.role != FrontierGroupRole::Coordinator || view.memberCount != 0 ||
                    view.sourceControlCount != 0 || !(binding.operation.owner == entry.entry.owner)) :
                (binding.role != FrontierGroupRole::EntryMember || !binding.variableSide.Valid() ||
                 binding.variableSide.side != side || view.sourceControlCount != 0)) return false;
            mapping.groups[side] = actualIndex;
        }
    }
    for (const auto index : mapping.groups) if (index == UINT32_MAX) return false;
    const auto* activation = entry.decisions->ResolveFrontierActivation(entry.entry.coordinator);
    DecisionCommittedSnapshot committed{};
    Plan03PageGroupView primary{};
    if (activation == nullptr || !entry.decisions->CaptureCommitted(entry.entry.coordinator, &committed) ||
        !entry.pool->ResolvePlan03GroupSliceProducer(entry.groups[2], &primary) || primary.frontier == nullptr)
        return false;
    const auto& descriptor = activation->Descriptor();
    if (!descriptor.Valid() || !(descriptor.entry == entry.entry) || !(descriptor.primary == entry.groups[2]) ||
        !(descriptor.order == primary.frontier->operation.order) || descriptor.expectedMemberCount != participants ||
        descriptor.worldGeneration != request.world || committed.commit != DecisionCommitState::Committed ||
        committed.ticket.sequence != request.ticket.sequence || committed.world != request.world ||
        committed.transport != request.transport || committed.continuationGeneration != request.expectedContinuationGeneration ||
        committed.sliceCount != request.sliceCount || committed.transitionGate.slot != request.transitionGate.slot ||
        committed.transitionGate.generation != request.transitionGate.generation || committed.participantCount != participants ||
        entry.groups[2].committedRow > UINT32_MAX || committed.pages.slot != entry.groups[2].committedRow ||
        committed.pages.generation != entry.groups[2].rowClaimGeneration || committed.audioState != AudioGroupState::Pending ||
        committed.consumedSlices != 0 || committed.lastAudioSequence != 0 || committed.futurePinCount == 0 ||
        committed.authoredStopInvalidated) return false;
    for (std::uint32_t index = 0; index < participants; ++index) {
        DecisionParticipantRef participant{};
        if (!entry.decisions->CaptureCommittedParticipant(entry.entry.coordinator, index, &participant) ||
            participant.kind != DecisionParticipantKind::Subdecision) return false;
        std::uint32_t matches = 0;
        for (std::uint32_t group = 0; group < entry.ownedGroupCount; ++group) {
            Plan03PageGroupView view{};
            const auto& identity = declaredGroups[ownedGroupRowsRead[group].privateOwnedIndex].identity;
            if (!entry.pool->ResolvePlan03GroupSliceProducer(identity, &view) || view.frontier == nullptr)
                return false;
            if (view.frontier->entry == entry.entry && view.frontier->role == FrontierGroupRole::EntryMember &&
                participant.slot == identity.decision.slot &&
                participant.generation == identity.decision.generation) ++matches;
        }
        if (matches != 1) return false;
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            DecisionParticipantRef previous{};
            if (!entry.decisions->CaptureCommittedParticipant(entry.entry.coordinator, prior, &previous) ||
                (previous.slot == participant.slot && previous.generation == participant.generation)) return false;
        }
    }
    for (std::uint32_t index = 0; index < attempt.eventCount; ++index) {
        const auto& lane = events[index];
        const auto& event = lane.event;
        if (!owns(event.frontier.group)) continue;
        Plan03PageGroupView view{};
        std::uint64_t due = 0;
        if (!event.frontier.Valid() || !entry.pool->ResolvePlan03GroupSliceProducer(event.frontier.group, &view) ||
            view.frontier == nullptr || !(event.frontier.entry == view.frontier->entry) ||
            !(event.frontier.owner == view.frontier->operation.owner) || event.frontier.world != request.world ||
            event.frontier.transport != request.transport || !ResolveLaneDue(lane, attempt.anchorFrame, &due) ||
            event.executeFrame != due || event.logicCommitSequence != request.ticket.sequence ||
            event.executionOrdinal != view.frontier->operation.order.executionOrdinal || event.program.program == nullptr ||
            event.program.program->payloads == nullptr || event.program.programGeneration != event.program.program->generation ||
            event.payload >= event.program.program->payloadCount || !event.frozenTxnValid) return false;
        const auto& payload = event.program.program->payloads[event.payload];
        const bool delayedVariable = payload.kind != TimelineEventKind::ControllerActivation &&
            lane.futureGroupedStart && view.frontier->variableSide.Valid() &&
            view.frontier->operation.kind == FrontierOperationKind::Entry;
        const auto originalKind = delayedVariable ? FrontierOperationKind::Dependent : view.frontier->operation.kind;
        if (event.frontier.kind != originalKind) return false;
        if (payload.kind == TimelineEventKind::ControllerActivation) {
            const TimelineExecutionOrder order{event.executeFrame, event.logicCommitSequence, event.executionOrdinal,
                event.eventIndex, event.transactionOrdinal, event.subeventPhase, event.rowSequence};
            TimelineTransactionProgramRef original{};
            if (event.armToken.Valid() || lane.futureGroupedStart ||
                event.program.program->role != TimelineProgramRole::ControllerActivation ||
                event.frontier.kind != FrontierOperationKind::Entry || !(order == view.frontier->operation.order) ||
                !entry.decisions->CaptureFrontierControlProgram(view.identity.decision, &original) ||
                original.program != event.program.program ||
                original.programGeneration != event.program.programGeneration) return false;
            if (view.frontier->role == FrontierGroupRole::Coordinator) {
                const auto* actualActivation = entry.decisions->ResolveFrontierActivation(view.identity.decision);
                if (actualActivation == nullptr || !actualActivation->Descriptor().Valid() ||
                    !(actualActivation->Descriptor().entry == event.frontier.entry) ||
                    !(actualActivation->Descriptor().primary == view.identity) ||
                    !(actualActivation->Descriptor().order == order) ||
                    actualActivation->Descriptor().programGeneration != event.program.programGeneration) return false;
            } else if (view.frontier->operation.program != event.program.program ||
                view.frontier->operation.programGeneration != event.program.programGeneration) return false;
            for (std::uint32_t side = 0; side < entry.groups.size(); ++side) {
                if (!(event.frontier.group == entry.groups[side])) continue;
                const auto projection = side == 2u ? 0u : side + 1u;
                if (mapping.events[projection] != UINT32_MAX) return false;
                mapping.events[projection] = index;
            }
        } else {
            bool covered = false;
            for (std::uint32_t member = 0; member < entry.memberCount; ++member) {
                const auto& manifest = members[member];
                if (!owns(manifest.group) || (manifest.relatedGroup.Exact() && !owns(manifest.relatedGroup)) ||
                    !ValidMemberEventOrdinals(manifest, events, attempt.eventCount,
                        config_.quantumFrames)) return false;
                const auto source = manifest.relatedGroup.Exact() ? manifest.relatedGroup : manifest.group;
                if (!(source == event.frontier.group) || !SameArm(event.armToken, manifest.arm) ||
                    (index != manifest.eventOrdinal && index != manifest.secondEventOrdinal)) continue;
                if (!SameTimeSpec(lane, manifest) || event.program.programGeneration != manifest.programGeneration ||
                    event.ownerInstanceHandle != manifest.ownerInstance || event.ownerInstanceScheduleEpoch != manifest.ownerEpoch ||
                    !(payload.target == manifest.target)) return false;
                covered = true;
            }
            if (!covered) return false;
        }
        ++mapping.ownedEventCount;
    }
    for (const auto index : mapping.events) if (index == UINT32_MAX) return false;
    if (mapping.ownedEventCount == 0) return false;
    // Use the same semantic spans as the audio finalizer. Queue fades may be
    // typed Dependent events in an Entry, or a separate later slice operation.
    const auto entryEvent = [](const ConsumedEvent& event, const Plan03PageGroupView& view) noexcept {
        if (event.frontier.kind == FrontierOperationKind::Entry) return true;
        return event.frontier.kind == FrontierOperationKind::Dependent &&
            view.frontier->role == FrontierGroupRole::EntryMember &&
            (view.frontier->queueInitial.Valid() ||
             (view.frontier->queueConditional.Valid() && view.frontier->transition.Valid() &&
              (view.sourceControlCount == 0u || view.identity.sliceIndex == 0u)));
    };
    for (std::uint32_t group = 0; group < entry.ownedGroupCount; ++group) {
        Plan03PageGroupView view{};
        if (!entry.pool->ResolvePlan03GroupSliceProducer(declaredGroups[ownedGroupRowsRead[group].privateOwnedIndex].identity, &view) ||
            view.frontier == nullptr) return false;
        const bool coordinator = view.frontier->role == FrontierGroupRole::Coordinator;
        if (view.frontier->role == FrontierGroupRole::EntryMember && view.frontier->variableSide.Valid()) {
            std::uint32_t controls = 0;
            for (std::uint32_t index = 0; index < attempt.eventCount; ++index) {
                const auto& event = events[index].event;
                if (event.frontier.group == view.identity &&
                    event.program.program->payloads[event.payload].kind == TimelineEventKind::ControllerActivation)
                    ++controls;
            }
            if (controls != 1) return false;
        }
        const auto* actualActivation = coordinator
            ? entry.decisions->ResolveFrontierActivation(view.identity.decision) : nullptr;
        DecisionCommittedSnapshot actualDecision{};
        std::uint32_t expected = 0;
        if (coordinator) {
            if (actualActivation == nullptr || !actualActivation->Descriptor().Valid() ||
                !(actualActivation->Descriptor().entry == view.frontier->entry) ||
                !(actualActivation->Descriptor().primary == view.identity) ||
                !(actualActivation->Descriptor().order == view.frontier->operation.order) ||
                !entry.decisions->CaptureCommitted(view.identity.decision, &actualDecision) ||
                actualDecision.commit != DecisionCommitState::Committed ||
                actualDecision.audioState != AudioGroupState::Pending || actualDecision.consumedSlices != 0 ||
                actualDecision.lastAudioSequence != 0 || actualDecision.authoredStopInvalidated ||
                actualDecision.world != request.world || actualDecision.transport != request.transport ||
                actualDecision.ticket.sequence != request.ticket.sequence ||
                actualDecision.pages.slot != view.identity.committedRow ||
                actualDecision.pages.generation != view.identity.rowClaimGeneration ||
                actualDecision.participantCount != actualActivation->Descriptor().expectedMemberCount) return false;
            std::uint32_t participantCount = 0;
            for (std::uint32_t member = 0; member < entry.ownedGroupCount; ++member) {
                Plan03PageGroupView side{};
                if (!entry.pool->ResolvePlan03GroupSliceProducer(declaredGroups[ownedGroupRowsRead[member].privateOwnedIndex].identity, &side) ||
                    side.frontier == nullptr) return false;
                if (side.frontier->role != FrontierGroupRole::EntryMember ||
                    !(side.frontier->entry == view.frontier->entry)) continue;
                if (!(side.frontier->primary == view.identity) || side.lease != view.lease) return false;
                std::uint32_t matches = 0;
                for (std::uint32_t ordinal = 0; ordinal < actualDecision.participantCount; ++ordinal) {
                    DecisionParticipantRef participant{};
                    if (!entry.decisions->CaptureCommittedParticipant(view.identity.decision, ordinal, &participant))
                        return false;
                    matches += participant.kind == DecisionParticipantKind::Subdecision &&
                        participant.slot == side.identity.decision.slot &&
                        participant.generation == side.identity.decision.generation;
                }
                if (matches != 1) return false;
                ++participantCount;
            }
            if (participantCount != actualDecision.participantCount) return false;
            expected = actualActivation->Descriptor().expectedEntryEventCount;
        }
        std::uint32_t count = 0, controllers = 0;
        for (std::uint32_t index = 0; index < attempt.eventCount; ++index) {
            const auto& lane = events[index];
            const auto& event = lane.event;
            if (!owns(event.frontier.group)) continue;
            Plan03PageGroupView source{};
            if (!entry.pool->ResolvePlan03GroupSliceProducer(event.frontier.group, &source) ||
                source.frontier == nullptr) return false;
            const bool initial = entryEvent(event, source);
            const auto operation = initial ? source.frontier->primary : source.identity;
            if (!(operation == view.identity) || initial != coordinator) continue;
            const bool delayedVariable = !initial && source.frontier->variableSide.Valid();
            if (event.frontier.operationOrdinal != 0) return false;
            if (coordinator) {
                const auto& actualDescriptor = actualActivation->Descriptor();
                if (!(event.frontier.entry == actualDescriptor.entry) || source.lease != view.lease ||
                    event.executeFrame != actualDescriptor.order.executeFrame ||
                    event.frontier.entryEventOrdinal >= expected) return false;
                const auto kind = event.program.program->payloads[event.payload].kind;
                if (kind == TimelineEventKind::ControllerActivation && source.identity == view.identity) {
                    ++controllers;
                    if (event.program.programGeneration != actualDescriptor.programGeneration ||
                        (event.frontier.operationEventCount != 1 && event.frontier.operationEventCount != expected))
                        return false;
                } else if (event.frontier.operationEventCount !=
                    (source.frontier->variableSide.Valid() ? 1u : expected)) return false;
            } else if (delayedVariable) {
                std::uint32_t semanticCount = 0;
                for (std::uint32_t other = 0; other < attempt.eventCount; ++other)
                    semanticCount += events[other].event.frontier.entry == event.frontier.entry;
                if (!lane.futureGroupedStart || event.frontier.operationEventCount != 1 ||
                    event.frontier.entryEventOrdinal >= semanticCount) return false;
            } else {
                if (count == 0) expected = event.frontier.operationEventCount;
                if (event.frontier.operationEventCount != expected ||
                    event.frontier.entryEventOrdinal >= expected ||
                    event.executeFrame != view.frontier->operation.order.executeFrame) return false;
            }
            for (std::uint32_t prior = 0; prior < index; ++prior) {
                const auto& previous = events[prior].event;
                if (!owns(previous.frontier.group)) continue;
                Plan03PageGroupView previousSource{};
                if (!entry.pool->ResolvePlan03GroupSliceProducer(previous.frontier.group, &previousSource) ||
                    previousSource.frontier == nullptr) return false;
                const bool previousInitial = entryEvent(previous, previousSource);
                const auto previousOperation = previousInitial ? previousSource.frontier->primary : previousSource.identity;
                if (((previousOperation == operation && previousInitial == initial) ||
                        (delayedVariable && previous.frontier.entry == event.frontier.entry)) &&
                    previous.frontier.entryEventOrdinal == event.frontier.entryEventOrdinal) return false;
            }
            ++count;
        }
        if (coordinator ? (count != expected || controllers != 1) : (count != 0 && count != expected &&
                !view.frontier->variableSide.Valid())) return false;
    }
    mapping.coordinatorBucket = groups[mapping.groups[2]].bucketOrdinal;
    const auto& coordinatorBucket = BucketBase(handle.slot)[mapping.coordinatorBucket];
    if (mapping.events[0] < coordinatorBucket.firstEvent ||
        mapping.events[0] - coordinatorBucket.firstEvent >= coordinatorBucket.eventCount) return false;
    *out = mapping;
    return true;
}

bool ImmediateActionLane::CapturePrivateVariableControlEntryMapping(
    ImmediateLaneHandle handle, std::uint64_t root, std::uint32_t ordinal,
    const FrontierEntryReceiptRef& entry, const ScheduledTransactionPool& pool,
    const TimelineArmTable& arms, const DecisionTable& decisions,
    ImmediateLanePrivateVariableControlEntryMapping* out) const noexcept {
    const auto* attempt = Resolve(handle);
    if (out == nullptr || attempt == nullptr || attempt->root != root ||
        ordinal >= attempt->privateEntryCount || !attempt->groupPinsInstalled ||
        !attempt->pinsInstalled ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared)
        return false;
    const auto& original = PrivateEntries(handle.slot)[ordinal];
    ImmediateLanePrivateVariableControlEntryMapping current{};
    if (!original.mappingValid || !(original.entry == entry) || original.pool != &pool ||
        original.armTable != &arms || original.decisions != &decisions ||
        !BuildPrivateVariableControlEntryMapping(handle, *attempt, original, &current) ||
        !(current == original.mapping)) return false;
    const auto* eventRowsRead = eventState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    const auto* members = MemberBase(handle.slot) + current.firstMember;
    const auto* events = EventBase(handle.slot);
    for (std::uint32_t member = 0; member < current.memberCount; ++member) {
        const auto& manifest = members[member];
        if (!ValidMemberEventOrdinals(manifest, events, attempt->eventCount,
                config_.quantumFrames) || !eventRowsRead[manifest.eventOrdinal].arm.Valid() ||
            (manifest.eventSpanCount == 2u && !eventRowsRead[manifest.secondEventOrdinal].arm.Valid()))
            return false;
    }
    *out = current;
    return true;
}

std::uint32_t ImmediateActionLane::PrivatePinGroupCount(
    ImmediateLaneHandle handle) const noexcept {
    const auto* attempt = Resolve(handle);
    return attempt == nullptr ? 0u : attempt->pinGroupManifestCount;
}

bool ImmediateActionLane::CapturePrivatePlacedLastQuantum(
    ImmediateLaneHandle handle, std::uint64_t* out) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || out == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Prepared ||
        attempt->bucketCount == 0u ||
        attempt->bucketCount > attempt->bucketCredit ||
        attempt->bucketCount > config_.eventsPerAttempt ||
        attempt->eventCount > attempt->eventCredit ||
        attempt->eventCount > config_.eventsPerAttempt ||
        attempt->declaredGroupCount > attempt->groupCredit ||
        attempt->declaredGroupCount > config_.eventsPerAttempt)
        return false;
    const auto* buckets = BucketBase(handle.slot);
    std::uint32_t eventTotal = 0u;
    std::uint32_t groupTotal = 0u;
    std::uint64_t last = 0u;
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index) {
        const auto& bucket = buckets[index];
        if (bucket.attemptGeneration != handle.generation ||
            bucket.state.load(std::memory_order_acquire) !=
                ImmediateLaneBucketState::Prepared ||
            (index != 0u && bucket.quantum <= last) ||
            bucket.firstEvent != eventTotal ||
            bucket.firstGroup != groupTotal ||
            bucket.eventCount > attempt->eventCount - eventTotal ||
            bucket.groupCount > attempt->declaredGroupCount - groupTotal ||
            (bucket.eventCount == 0u && bucket.groupCount == 0u))
            return false;
        eventTotal += bucket.eventCount;
        groupTotal += bucket.groupCount;
        last = bucket.quantum;
    }
    if (eventTotal != attempt->eventCount ||
        groupTotal != attempt->declaredGroupCount)
        return false;
    *out = last;
    return true;
}

bool ImmediateActionLane::CapturePrivatePinGroup(
    ImmediateLaneHandle handle, std::uint32_t index,
    Plan03GroupSliceIdentity* out) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || out == nullptr ||
        index >= attempt->pinGroupManifestCount ||
        attempt->pinGroupManifestCount > attempt->groupCredit ||
        attempt->pinGroupPool == nullptr) return false;
    const auto& group = GroupBase(handle.slot)[index];
    if (!group.identity.Exact() || group.laneSlot != handle.slot ||
        group.laneGeneration != handle.generation) return false;
    *out = group.identity;
    return true;
}

bool ImmediateActionLane::ReleasePrivatePinGroupOwnership(
    ImmediateLaneHandle handle, ScheduledTransactionPool& pool) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePublicationProof != nullptr || attempt->pinGroupPool != &pool ||
        attempt->groupPinsInstalled ||
        attempt->pinGroupManifestCount == 0 ||
        attempt->pinGroupManifestCount > attempt->groupCredit) return false;
    const auto state = attempt->state.load(std::memory_order_acquire);
    if (state != ImmediateLaneAttemptState::Prepared &&
        state != ImmediateLaneAttemptState::PrivateCancelled) return false;
    InvalidatePrivateEntryMappings(handle);
    auto* groupRows = groupState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    auto* groupPins = groupPins_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    const auto* groups = GroupBase(handle.slot);
    bool complete = true;
    for (std::uint32_t i = 0; i < attempt->pinGroupManifestCount; ++i) {
        if (!groups[i].identity.Exact() || groups[i].laneSlot != handle.slot ||
            groups[i].laneGeneration != handle.generation) return false;
        if (groupRows[i].futureOwned == 0) continue;
        if (pool.ReleaseImmediateLaneFutureDependent(groups[i].identity))
            groupRows[i].futureOwned = 0;
        else complete = false;
    }
    if (!complete) return false;
    for (std::uint32_t i = 0; i < attempt->pinGroupManifestCount; ++i) {
        if (groupPins[i].Valid() && !groupPins[i].Release(0)) complete = false;
        if (groupRows[i].decision.Valid() && !groupRows[i].decision.Release()) complete = false;
    }
    return complete;
}

bool ImmediateActionLane::PrivateInstalledInitialStartsRollbackShape(
    ImmediateLaneHandle handle, std::uint64_t root, TimelineArmTable& arms,
    ScheduledTransactionPool& pool) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt || root == 0 || attempt->root != root ||
        attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared ||
        !attempt->pinsInstalled || !attempt->groupPinsInstalled ||
        attempt->pinGroupPool != &pool || attempt->groupCount == 0 ||
        attempt->groupCount != attempt->pinGroupManifestCount ||
        attempt->groupCount > attempt->groupCredit ||
        attempt->groupCount > config_.eventsPerAttempt ||
        attempt->eventCount > attempt->eventCredit ||
        attempt->eventCount > config_.eventsPerAttempt) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* events = EventBase(handle.slot);
    const auto* groups = GroupBase(handle.slot);
    bool actualStart = false;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& event = events[index].event;
        const auto* program = event.program.program;
        if (eventState_[base + index].fence.Valid() || program == nullptr ||
            program->payloads == nullptr || event.payload >= program->payloadCount ||
            program->generation != event.program.programGeneration || !event.frozenTxnValid)
            return false;
        const auto& pin = eventState_[base + index].arm;
        if (program->role == TimelineProgramRole::ControllerActivation) {
            if (pin.Valid() || event.armToken.Valid() ||
                program->payloads[event.payload].kind != TimelineEventKind::ControllerActivation ||
                event.frontier.kind != FrontierOperationKind::Entry) return false;
        } else {
            if (program->role != TimelineProgramRole::Voice ||
                program->payloads[event.payload].kind != TimelineEventKind::Start ||
                !pin.Valid() || pin.owner_ != &arms ||
                !SameArm(pin.token_, event.armToken)) return false;
            actualStart = true;
        }
        bool matching = false;
        for (std::uint32_t group = 0; group < attempt->groupCount; ++group) {
            const auto& original = groups[group];
            if (!(original.identity == event.frontier.group)) continue;
            Plan03PageGroupView native{};
            if (original.laneSlot != handle.slot || original.laneGeneration != handle.generation ||
                !pool.PrivateImmediateLaneFrontierRollbackReady(original.identity) ||
                !pool.ResolvePlan03GroupSliceProducer(original.identity, &native) ||
                native.frontier == nullptr || native.lease == nullptr ||
                !(native.frontier->entry == event.frontier.entry) ||
                !(native.frontier->operation.owner == event.frontier.owner) ||
                native.frontier->operation.world != event.frontier.world ||
                native.frontier->operation.transport != event.frontier.transport)
                return false;
            matching = true;
            break;
        }
        if (!matching) return false;
    }
    return actualStart;
}

bool ImmediateActionLane::PrivateInstalledInitialArmRollbackReady(
    ImmediateLaneHandle handle, std::uint64_t root, TimelineArmTable& arms,
    ScheduledTransactionPool& pool, TimelineArmToken arm,
    const FrontierGroupBinding& binding, AudioSliceLease& lease) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt || root == 0 ||
        attempt->root != root || attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->pinGroupPool != &pool || !attempt->groupPinsInstalled ||
        !attempt->pinsInstalled || attempt->groupCount == 0 ||
        attempt->groupCount != attempt->pinGroupManifestCount ||
        attempt->groupCount > attempt->groupCredit ||
        attempt->groupCount > config_.eventsPerAttempt ||
        attempt->eventCount > attempt->eventCredit ||
        attempt->eventCount > config_.eventsPerAttempt) return false;
    const auto state = attempt->state.load(std::memory_order_acquire);
    if (state != ImmediateLaneAttemptState::Prepared &&
        state != ImmediateLaneAttemptState::PrivateCancelled) return false;
    Plan03PageGroupView actual{};
    bool ownGroup = false;
    const auto* groups = GroupBase(handle.slot);
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        if (!groups[index].identity.Exact() || groups[index].laneSlot != handle.slot ||
            groups[index].laneGeneration != handle.generation) return false;
        ownGroup |= groups[index].identity == binding.operation.group;
    }
    if (!ownGroup || binding.role != FrontierGroupRole::EntryMember ||
        !pool.PrivateImmediateLaneFrontierRollbackReady(binding.operation.group) ||
        !pool.ResolvePlan03GroupSliceProducer(binding.operation.group, &actual) ||
        actual.frontier != &binding || actual.lease != &lease ||
        (actual.memberCount != 0 && actual.members == nullptr)) return false;
    bool actualMember = false;
    for (std::uint16_t index = 0; index < actual.memberCount; ++index)
        actualMember |= SameArm(actual.members[index], arm);
    if (!actualMember) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* events = EventBase(handle.slot);
    bool heldOriginal = false;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& pin = eventState_[base + index].arm;
        if (SameArm(events[index].event.armToken, arm) && pin.Valid() &&
            pin.owner_ == &arms && SameArm(pin.token_, arm)) heldOriginal = true;
    }
    if (!heldOriginal) return false;
    auto* row = arms.Resolve(arm);
    if (row == nullptr || row->reservation.test_and_set(std::memory_order_acquire))
        return false;
    TimelineArmObservation observation{};
    const bool ready = arms.Observe(arm, &observation) &&
        row->renderSlot == arm.renderSlot && row->renderGeneration == arm.renderGeneration &&
        (observation.state == ArmState::Armed || observation.state == ArmState::Published) &&
        observation.startedBlockSequence == 0 &&
        row->frontierAdmission == &binding && row->frontierAdmissionLease == &lease &&
        row->frontierActiveLease.load(std::memory_order_acquire) == nullptr &&
        row->frontierReaderPins.load(std::memory_order_acquire) != 0 &&
        row->frontierLastReadSequence.load(std::memory_order_acquire) == 0 &&
        lease.Capture().state == AudioSliceState::Ready;
    row->reservation.clear(std::memory_order_release);
    return ready;
}

bool ImmediateActionLane::ReleaseCancelledPrivateInitialPins(
    ImmediateLaneHandle handle, std::uint64_t root, TimelineArmTable& arms,
    ScheduledTransactionPool& pool, DecisionTable& decisions,
    std::uint64_t completedAudioSequence) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt || root == 0 ||
        attempt->root != root || attempt->captureActive.load(std::memory_order_acquire) ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::PrivateCancelled ||
        attempt->nextBucket.load(std::memory_order_acquire) != attempt->bucketCount ||
        attempt->pinGroupPool != &pool || !attempt->groupPinsInstalled ||
        attempt->groupCount == 0 || attempt->groupCount != attempt->pinGroupManifestCount ||
        attempt->groupCount > attempt->groupCredit ||
        attempt->groupCount > config_.eventsPerAttempt ||
        attempt->eventCount > attempt->eventCredit ||
        attempt->eventCount > config_.eventsPerAttempt ||
        attempt->bucketCount > attempt->bucketCredit ||
        attempt->bucketCount > config_.eventsPerAttempt) return false;
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index)
        if (BucketBase(handle.slot)[index].attemptGeneration != handle.generation ||
            BucketBase(handle.slot)[index].state.load(std::memory_order_acquire) !=
                ImmediateLaneBucketState::Cancelled) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    auto* eventRows = eventState_.At(base);
    auto* groupPins = groupPins_.At(base);
    auto* groupRows = groupState_.At(base);
    auto* groups = GroupBase(handle.slot);
    const auto* events = EventBase(handle.slot);
    // Validate the complete immutable original manifest before returning any prefix.
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        Plan03PageGroupView actual{};
        if (!groups[index].identity.Exact() || groups[index].laneSlot != handle.slot ||
            groups[index].laneGeneration != handle.generation ||
            !pool.ResolvePlan03GroupSliceProducer(groups[index].identity, &actual) ||
            !(actual.identity == groups[index].identity) ||
            !pool.PrivateImmediateLaneFrontierRollbackReady(groups[index].identity) ||
            actual.frontier == nullptr || actual.lease == nullptr ||
            !(actual.frontier->operation.group == groups[index].identity) ||
            (actual.memberCount != 0 && actual.members == nullptr) ||
            (actual.frontier->role != FrontierGroupRole::Coordinator &&
             actual.frontier->role != FrontierGroupRole::EntryMember) ||
            (groupPins[index].Valid() &&
                !(groupPins[index].Identity() == groups[index].identity)) ||
            (groupRows[index].decision.Valid() && (groupRows[index].decision.Owner() != &decisions ||
                groupRows[index].decision.Token().slot != groups[index].identity.decision.slot ||
                groupRows[index].decision.Token().generation != groups[index].identity.decision.generation)) ||
            (groupRows[index].futureOwned != 0 && !groupPins[index].Valid())) return false;
    }
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        // This native initial-group primitive does not consume a None backing.
        if (eventState_[base + index].fence.Valid() ||
            (eventRows[index].arm.Valid() && (eventRows[index].arm.owner_ != &arms ||
                !SameArm(eventRows[index].arm.token_, events[index].event.armToken)))) return false;
    }
    // Close each actual private admission before returning its owned pin. An exact
    // prefix remains closed on Busy; the cancelled Lane and its manifest remain held.
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        auto& pin = eventRows[index].arm;
        if (!pin.Valid()) continue;
        const auto arm = pin.token_;
        if (arm.slot >= arms.capacity_) return false;
        auto& row = arms.rows_[arm.slot];
        if (row.reservation.test_and_set(std::memory_order_acquire)) return false;
        const auto finish = [&](bool result) noexcept {
            row.reservation.clear(std::memory_order_release);
            return result;
        };
        if (! (row.identity.load(std::memory_order_acquire) ==
                ((static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot)) || row.renderSlot != arm.renderSlot ||
            row.renderGeneration != arm.renderGeneration ||
            row.authoredControl.operation != nullptr ||
            row.frontierReaderPins.load(std::memory_order_acquire) == 0 ||
            row.startedBlockSequence.load(std::memory_order_acquire) != 0 ||
            row.frontierLastReadSequence.load(std::memory_order_acquire) != 0 ||
            row.frontierActiveLease.load(std::memory_order_acquire) != nullptr)
            return finish(false);
        const auto state = row.state.load(std::memory_order_acquire);
        const auto* installedBinding = row.frontierAdmission;
        auto* installedLease = row.frontierAdmissionLease;
        const auto& originalEvent = events[index].event;
        Plan03PageGroupView actual{};
        bool ownBoundGroup = false;
        for (std::uint32_t group = 0; group < attempt->groupCount; ++group) {
            if (!(groups[group].identity == originalEvent.frontier.group)) continue;
            if (!pool.ResolvePlan03GroupSliceProducer(groups[group].identity, &actual) ||
                actual.frontier == nullptr || actual.lease == nullptr) return finish(false);
            ownBoundGroup = true;
            break;
        }
        const auto* binding = actual.frontier;
        auto* lease = actual.lease;
        if (!ownBoundGroup ||
            (installedBinding != nullptr && installedBinding != binding) ||
            (installedLease != nullptr && installedLease != lease) ||
            ((installedBinding == nullptr) != (installedLease == nullptr)))
            return finish(false);
        const auto& event = events[index].event;
        const auto* program = event.program.program;
        if (!ownBoundGroup || lease->Capture().state != AudioSliceState::Ready ||
            program == nullptr || program->role != TimelineProgramRole::Voice ||
            program->payloads == nullptr || event.payload >= program->payloadCount ||
            event.program.programGeneration != program->generation ||
            !event.frozenTxnValid || !SameArm(event.armToken, arm) ||
            events[index].armProgramGeneration != row.programGeneration ||
            program->payloads[event.payload].kind != TimelineEventKind::Start ||
            !(program->payloads[event.payload].target == row.target) ||
            (row.startBound ? event.executeFrame != row.startFrame :
                (!row.pendingImmediateStart || row.startFrame != 0 || row.armDeadline != 0)) ||
            !(event.frontier.entry == binding->entry) ||
            !(event.frontier.owner == binding->operation.owner) ||
            !(event.frontier.group == binding->operation.group) ||
            event.frontier.world != binding->operation.world ||
            event.frontier.transport != binding->operation.transport)
            return finish(false);
        bool originalMember = false;
        if (row.parentGroup.Valid()) {
            originalMember = binding->role == FrontierGroupRole::EntryMember &&
                row.parentGroup.decision.slot == actual.identity.decision.slot &&
                row.parentGroup.decision.generation == actual.identity.decision.generation &&
                row.memberIndex < actual.memberCount && actual.members != nullptr &&
                SameArm(actual.members[row.memberIndex], arm);
        } else {
            // Initial Current composite leaves genuinely bind the owned empty primary.
            originalMember = row.memberIndex == UINT16_MAX &&
                binding->role == FrontierGroupRole::Coordinator &&
                actual.memberCount == 0 &&
                actual.identity == binding->primary &&
                binding->operation.group == binding->primary &&
                event.frontier.kind == FrontierOperationKind::Entry;
        }
        const bool alreadyWithdrawn = row.nativePrivateWithdrawn.load(std::memory_order_acquire);
        if (!originalMember ||
            (installedBinding == nullptr && !alreadyWithdrawn && state != ArmState::Armed) ||
            (!alreadyWithdrawn && state != ArmState::Armed && state != ArmState::Published) ||
            (alreadyWithdrawn && ((state != ArmState::CancelRequested && state != ArmState::Tombstoned) ||
                !row.frontierAdmissionFinished.load(std::memory_order_acquire))))
            return finish(false);
        // This is a native cancellation transition, not an observation or audio receipt.
        // Ready is deliberately retained for genuine member/coordinator rollback.
        if (!alreadyWithdrawn) {
            row.nativePrivateWithdrawn.store(true, std::memory_order_release);
            row.state.store(ArmState::CancelRequested, std::memory_order_release);
            row.frontierAdmissionFinished.store(true, std::memory_order_release);
        }
        finish(true);
    }
    InvalidatePrivateEntryMappings(handle);
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index)
        if (eventRows[index].arm.Valid() && !eventRows[index].arm.Release(completedAudioSequence))
            return false;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        if (groupRows[index].futureOwned == 0) continue;
        if (!pool.ReleaseImmediateLaneFutureDependent(groups[index].identity)) return false;
        groupRows[index].futureOwned = 0;
        groups[index].futureDependent = false;
    }
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index)
        if (groupPins[index].Valid() && !groupPins[index].Release(completedAudioSequence))
            return false;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index)
        if (groupRows[index].decision.Valid() && !groupRows[index].decision.Release()) return false;
    attempt->pinsInstalled = false;
    attempt->groupPinsInstalled = false;
    // Counts, full group identities, and cancelled buckets survive until Retire.
    return true;
}

bool ImmediateActionLane::CandidateFits(std::uint32_t slot,
    std::uint64_t anchor, std::uint64_t firstSafeFrame) const noexcept {
    const auto& attempt = attempts_[slot];
    const auto* events = EventBase(slot);
    const auto base = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    const auto* groups = declaredGroups_.At(base);
    auto* eventOrder = eventPlacementOrder_.get();
    auto* groupOrder = groupPlacementOrder_.get();
    for (std::uint32_t index = 0; index < attempt.eventCount; ++index) {
        std::uint64_t due = 0;
        if (!ResolveLaneDue(events[index], anchor, &due) || due < firstSafeFrame) return false;
        eventOrder[index] = index;
    }
    for (std::uint32_t index = 0; index < attempt.declaredGroupCount; ++index) {
        std::uint64_t due = 0;
        if (!ResolveLaneDue(groups[index], anchor, &due) || due < firstSafeFrame) return false;
        groupOrder[index] = index;
    }
    const auto eventDue = [&](std::uint32_t index) noexcept {
        std::uint64_t due = 0;
        (void)ResolveLaneDue(events[index], anchor, &due);
        return due;
    };
    const auto groupDue = [&](std::uint32_t index) noexcept {
        std::uint64_t due = 0;
        (void)ResolveLaneDue(groups[index], anchor, &due);
        return due;
    };
    std::sort(eventOrder, eventOrder + attempt.eventCount, [&](auto a, auto b) noexcept {
        if (eventDue(a) != eventDue(b)) return eventDue(a) < eventDue(b);
        if (LessLaneExecutionOrder(events[a], events[b])) return true;
        if (LessLaneExecutionOrder(events[b], events[a])) return false;
        return a < b;
    });
    std::sort(groupOrder, groupOrder + attempt.declaredGroupCount, [&](auto a, auto b) noexcept {
        return groupDue(a) != groupDue(b) ? groupDue(a) < groupDue(b) : a < b;
    });
    std::uint32_t firstEvent = 0, firstGroup = 0, bucketCount = 0;
    while (firstEvent < attempt.eventCount || firstGroup < attempt.declaredGroupCount) {
        const auto eventQuantum = firstEvent < attempt.eventCount
            ? eventDue(eventOrder[firstEvent]) / config_.quantumFrames : UINT64_MAX;
        const auto groupQuantum = firstGroup < attempt.declaredGroupCount
            ? groupDue(groupOrder[firstGroup]) / config_.quantumFrames : UINT64_MAX;
        const auto quantum = std::min(eventQuantum, groupQuantum);
        if (++bucketCount > attempt.bucketCredit) return false;
        std::uint32_t eventCount = 0, groupCount = 0;
        while (firstEvent < attempt.eventCount &&
               eventDue(eventOrder[firstEvent]) / config_.quantumFrames == quantum) {
            ++eventCount;
            ++firstEvent;
        }
        while (firstGroup < attempt.declaredGroupCount &&
               groupDue(groupOrder[firstGroup]) / config_.quantumFrames == quantum) {
            ++groupCount;
            ++firstGroup;
        }
        if (eventCount > config_.maxEventsPerQuantum || groupCount > config_.maxGroupsPerQuantum)
            return false;
        std::uint64_t occupiedEvents = eventCount, occupiedGroups = groupCount;
        if (attempt.privateLayoutEdit != nullptr) {
            const auto& edit = *attempt.privateLayoutEdit;
            for (std::uint32_t i = 0; i < edit.originalBucketCount_; ++i)
                if (edit.originalBuckets_[i].quantum == quantum)
                    occupiedEvents = std::max(occupiedEvents,
                        static_cast<std::uint64_t>(edit.originalBuckets_[i].eventCount));
        }
        for (std::uint32_t other = 0; other < attempts_.size(); ++other) {
            if (other == slot) continue;
            const auto& live = attempts_[other];
            const auto state = live.state.load(std::memory_order_acquire);
            if (state != ImmediateLaneAttemptState::Prepared &&
                state != ImmediateLaneAttemptState::Committed &&
                state != ImmediateLaneAttemptState::Cancelling &&
                state != ImmediateLaneAttemptState::PrivateLayoutHeld) continue;
            if (state == ImmediateLaneAttemptState::PrivateLayoutHeld) {
                std::uint32_t oldCount = 0, tentativeCount = 0;
                const auto& edit = *live.privateLayoutEdit;
                for (std::uint32_t i = 0; i < edit.originalBucketCount_; ++i)
                    if (edit.originalBuckets_[i].quantum == quantum)
                        oldCount = edit.originalBuckets_[i].eventCount;
                const auto* staged = BucketBase(other);
                for (std::uint32_t i = 0; i < live.bucketCount; ++i)
                    if (staged[i].quantum == quantum)
                        tentativeCount = staged[i].eventCount;
                occupiedEvents += std::max(oldCount, tentativeCount);
                continue;
            }
            const auto* buckets = BucketBase(other);
            for (std::uint32_t index = 0; index < live.bucketCount; ++index) {
                if (buckets[index].quantum != quantum) continue;
                const auto bucketState = buckets[index].state.load(std::memory_order_acquire);
                if (bucketState != ImmediateLaneBucketState::Prepared &&
                    bucketState != ImmediateLaneBucketState::Committed &&
                    bucketState != ImmediateLaneBucketState::AudioClaimed) continue;
                occupiedEvents += buckets[index].eventCount;
                occupiedGroups += buckets[index].groupCount;
            }
        }
        if (occupiedEvents > config_.maxEventsPerQuantum || occupiedGroups > config_.maxGroupsPerQuantum)
            return false;
    }
    return true;
}

bool ImmediateActionLane::TryPlace(ImmediateLaneHandle handle,
    std::uint64_t earliestFrame, std::uint32_t additionalQuanta,
    std::uint64_t* outAnchorFrame) noexcept {
    return TryPlace(handle, earliestFrame, additionalQuanta, outAnchorFrame, nullptr);
}

bool ImmediateActionLane::TryPlace(ImmediateLaneHandle handle,
    std::uint64_t earliestFrame, std::uint32_t additionalQuanta,
    std::uint64_t* outAnchorFrame, PrivateImmediateLanePlacementProof* outPlacement) noexcept {
    if (outPlacement != nullptr && outPlacement->owner_ != nullptr) return false;
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePlacementProof != nullptr) return false;
    const auto initial = attempt->state.load(std::memory_order_acquire);
    if (initial != ImmediateLaneAttemptState::Reserved &&
        initial != ImmediateLaneAttemptState::Prepared) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt) return false;
    if (attempt == nullptr || outAnchorFrame == nullptr || attempt->groupPinsInstalled ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared ||
        earliestFrame > UINT64_MAX - (config_.quantumFrames - 1u)) return false;
    const auto first = ((earliestFrame + config_.quantumFrames - 1u) /
        config_.quantumFrames) * config_.quantumFrames;
    std::uint64_t anchor = first;
    bool found = false;
    for (std::uint64_t offset = 0; offset <= additionalQuanta; ++offset) {
        if (offset > (UINT64_MAX - first) / config_.quantumFrames) break;
        anchor = first + offset * config_.quantumFrames;
        if (CandidateFits(handle.slot, anchor, first)) { found = true; break; }
    }
    if (!found) return false;
    InstallPlacement(handle, anchor);
    if (outPlacement != nullptr) {
        // The actual successful placement is the issuer. No Capture/DTO path
        // creates this association, and no fallible operation follows it.
        outPlacement->owner_ = this;
        outPlacement->attempt_ = attempt;
        outPlacement->handle_ = handle;
        outPlacement->root_ = attempt->root;
        outPlacement->anchor_ = anchor;
        attempt->privatePlacementProof = outPlacement;
    }
    *outAnchorFrame = anchor;
    return true;
}

bool ImmediateActionLane::TryPlaceExactPrivate(ImmediateLaneHandle handle,
    std::uint64_t anchor, PrivateImmediateLanePlacementProof* outPlacement) noexcept {
    if (outPlacement != nullptr && outPlacement->owner_ != nullptr) return false;
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePlacementProof != nullptr ||
        attempt->groupPinsInstalled ||
        attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared)
        return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt || !CandidateFits(handle.slot, anchor, anchor))
        return false;
    InstallPlacement(handle, anchor);
    if (outPlacement != nullptr) {
        outPlacement->owner_ = this;
        outPlacement->attempt_ = attempt;
        outPlacement->handle_ = handle;
        outPlacement->root_ = attempt->root;
        outPlacement->anchor_ = anchor;
        attempt->privatePlacementProof = outPlacement;
    }
    return true;
}

void ImmediateActionLane::InstallPlacement(ImmediateLaneHandle handle,
    std::uint64_t anchor) noexcept {
    auto* attempt = Resolve(handle);
    auto* events = EventBase(handle.slot);
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    auto* groups = declaredGroups_.At(base);
    auto* eventOrder = eventPlacementOrder_.get();
    auto* groupOrder = groupPlacementOrder_.get();
    auto* eventRows = eventState_.At(base);
    if (attempt->privateEntryCount != 0) {
        auto* groupRows = groupState_.At(base);
        for (std::uint32_t index = 0; index < attempt->declaredGroupCount; ++index)
            groupRows[groupOrder[index]].privateEntry = index;
        const auto entries = PrivateEntries(handle.slot);
        for (std::uint32_t ordinal = 0; ordinal < attempt->privateEntryCount; ++ordinal) {
            const auto& entry = entries[ordinal];
            auto* ownedGroupRows = groupState_.At(base) + entry.firstOwnedGroup;
            for (std::uint32_t index = 0; index < entry.ownedGroupCount; ++index)
                ownedGroupRows[index].privateOwnedIndex = groupRows[ownedGroupRows[index].privateOwnedIndex].privateEntry;
        }
    }
    // Each destination pin is empty because the preceding cycle move emptied it.
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        if (eventOrder[index] == index) continue;
        auto savedEvent = events[index];
        auto savedPin = std::move(eventRows[index].arm);
        auto savedFence = std::move(eventRows[index].fence);
        const auto savedScope = eventState_[base + index].withdrawalLeader;
        auto current = index;
        while (eventOrder[current] != index) {
            const auto next = eventOrder[current];
            events[current] = events[next];
            eventRows[current].arm = std::move(eventRows[next].arm);
            eventRows[current].fence = std::move(eventRows[next].fence);
            eventState_[base + current].withdrawalLeader = eventState_[base + next].withdrawalLeader;
            eventOrder[current] = current;
            current = next;
        }
        events[current] = savedEvent;
        eventRows[current].arm = std::move(savedPin);
        eventRows[current].fence = std::move(savedFence);
        eventState_[base + current].withdrawalLeader = savedScope;
        eventOrder[current] = current;
    }
    for (std::uint32_t index = 0; index < attempt->declaredGroupCount; ++index) {
        if (groupOrder[index] == index) continue;
        const auto saved = groups[index];
        auto current = index;
        while (groupOrder[current] != index) {
            const auto next = groupOrder[current];
            groups[current] = groups[next];
            groupOrder[current] = current;
            current = next;
        }
        groups[current] = saved;
        groupOrder[current] = current;
    }
    auto* buckets = BucketBase(handle.slot);
    std::uint32_t firstEvent = 0, firstGroup = 0, bucketCount = 0;
    const auto eventDue = [&](std::uint32_t index) noexcept {
        std::uint64_t due = 0;
        (void)ResolveLaneDue(events[index], anchor, &due);
        return due;
    };
    const auto groupDue = [&](std::uint32_t index) noexcept {
        std::uint64_t due = 0;
        (void)ResolveLaneDue(groups[index], anchor, &due);
        return due;
    };
    while (firstEvent < attempt->eventCount || firstGroup < attempt->declaredGroupCount) {
        const auto eventQuantum = firstEvent < attempt->eventCount
            ? eventDue(firstEvent) / config_.quantumFrames : UINT64_MAX;
        const auto groupQuantum = firstGroup < attempt->declaredGroupCount
            ? groupDue(firstGroup) / config_.quantumFrames : UINT64_MAX;
        const auto quantum = std::min(eventQuantum, groupQuantum);
        auto& bucket = buckets[bucketCount++];
        bucket.attemptGeneration = handle.generation;
        bucket.quantum = quantum;
        bucket.firstEvent = firstEvent;
        bucket.eventCount = 0;
        bucket.firstGroup = firstGroup;
        bucket.groupCount = 0;
        while (firstEvent < attempt->eventCount && eventDue(firstEvent) / config_.quantumFrames == quantum) {
            events[firstEvent].event.executeFrame = eventDue(firstEvent);
            ++bucket.eventCount;
            ++firstEvent;
        }
        while (firstGroup < attempt->declaredGroupCount && groupDue(firstGroup) / config_.quantumFrames == quantum) {
            ++bucket.groupCount;
            ++firstGroup;
        }
        bucket.state.store(ImmediateLaneBucketState::Prepared, std::memory_order_relaxed);
        bucket.sourceWaitSequence.store(0, std::memory_order_relaxed);
        bucket.completedAudioSequence.store(0, std::memory_order_relaxed);
    }
    attempt->bucketCount = bucketCount;
    attempt->anchorFrame = anchor;
}

bool ImmediateActionLane::Publish(ImmediateLaneHandle handle) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePlacementProof != nullptr) return false;
    const auto initial = attempt->state.load(std::memory_order_acquire);
    if (initial != ImmediateLaneAttemptState::Reserved &&
        initial != ImmediateLaneAttemptState::Prepared) return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt) return false;
    if (!ValidatePrivatePublication(handle, *attempt)) return false;
    ApplyPrivatePublicationImpl(handle, *attempt);
    return true;
}

bool ImmediateActionLane::ValidatePrivatePublication(ImmediateLaneHandle handle,
    const ImmediateLaneAttempt& native) const noexcept {
    const auto* attempt = &native;
    if ((attempt->conditionalProjectionIssued &&
            !attempt->conditionalProjectionBound) || attempt->bucketCount == 0 ||
        (config_.requireArmPins && !attempt->pinsInstalled) ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Prepared)
        return false;
    for (std::uint32_t i = 0; i < attempt->eventCount; ++i)
        if (EventBase(handle.slot)[i].event.frontier.Present() &&
            !attempt->groupPinsInstalled) return false;
    const auto* buckets = BucketBase(handle.slot);
    std::uint32_t nextGroup = 0;
    const auto* groups = GroupBase(handle.slot);
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index) {
        const auto& bucket = buckets[index];
        if (bucket.firstGroup != nextGroup ||
            bucket.groupCount > attempt->groupCount - nextGroup)
            return false;
        for (std::uint32_t group = bucket.firstGroup;
             group < bucket.firstGroup + bucket.groupCount; ++group)
            if (groups[group].dueQuantum != bucket.quantum) return false;
        nextGroup += bucket.groupCount;
    }
    if (nextGroup != attempt->groupCount) return false;
    const auto entries = PrivateEntries(handle.slot);
    for (std::uint32_t index = 0; index < attempt->privateEntryCount; ++index) {
        ImmediateLanePrivateVariableControlEntryMapping mapping{};
        const auto& entry = entries[index];
        if (entry.pool == nullptr || entry.armTable == nullptr || entry.decisions == nullptr ||
            !CapturePrivateVariableControlEntryMapping(handle, attempt->root, index, entry.entry,
                *entry.pool, *entry.armTable, *entry.decisions, &mapping)) return false;
    }
    return true;
}

void ImmediateActionLane::ApplyPrivatePublicationImpl(ImmediateLaneHandle handle,
    ImmediateLaneAttempt& native) noexcept {
    auto* attempt = &native;
    auto* buckets = BucketBase(handle.slot);
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index)
        buckets[index].state.store(ImmediateLaneBucketState::Committed,
                                   std::memory_order_relaxed);
    attempt->state.store(ImmediateLaneAttemptState::Committed,
                         std::memory_order_release);
}

PrivateImmediateLanePublicationProof::PrivateImmediateLanePublicationProof(
    PrivateImmediateLanePublicationProof&& other) noexcept { *this = std::move(other); }

PrivateImmediateLanePublicationProof& PrivateImmediateLanePublicationProof::operator=(
    PrivateImmediateLanePublicationProof&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        attempt_ = std::exchange(other.attempt_, nullptr);
        handle_ = std::exchange(other.handle_, ImmediateLaneHandle{});
        groupsPublication_ = std::exchange(other.groupsPublication_, nullptr);
        if (groupsPublication_ != nullptr) groupsPublication_->lane_ = this;
        if (attempt_ != nullptr) {
            assert(attempt_->privatePublicationProof == &other);
            attempt_->privatePublicationProof = this;
        }
    }
    return *this;
}

PrivateImmediateLanePublicationProof::~PrivateImmediateLanePublicationProof() { Release(); }

void PrivateImmediateLanePublicationProof::Release() noexcept {
    if (groupsPublication_ != nullptr) groupsPublication_->Release();
    if (owner_ != nullptr) {
        assert(attempt_ != nullptr && attempt_->privatePublicationProof == this);
        attempt_->privatePublicationProof = nullptr;
        attempt_->readerGate.clear(std::memory_order_release);
    }
    owner_ = nullptr;
    attempt_ = nullptr;
    handle_ = {};
}

bool ImmediateActionLane::ProvePrivatePublication(ImmediateLaneHandle handle,
    std::uint64_t root, PrivateImmediateLanePublicationProof* out) noexcept {
    auto* attempt = Resolve(handle);
    if (out == nullptr || out->Valid() || attempt == nullptr || root == 0 ||
        attempt->root != root || attempt->privatePlacementProof != nullptr ||
        attempt->privatePublicationProof != nullptr || attempt->privateLayoutEdit != nullptr)
        return false;
    AttemptReadAccess access(*attempt);
    if (!access || Resolve(handle) != attempt ||
        !ValidatePrivatePublication(handle, *attempt)) return false;
    out->owner_ = this;
    out->attempt_ = attempt;
    out->handle_ = handle;
    attempt->privatePublicationProof = out;
    access.Detach();
    return true;
}

void ImmediateActionLane::ApplyPrivatePublication(
    PrivateImmediateLanePublicationProof&& proof) noexcept {
    assert(proof.Valid() && proof.attempt_->privatePublicationProof == &proof);
    if (!proof.Valid() || proof.groupsPublication_ != nullptr) return;
    proof.owner_->ApplyPrivatePublicationImpl(proof.handle_, *proof.attempt_);
    proof.Release();
}

bool ImmediateActionLane::Cancel(ImmediateLaneHandle handle) noexcept {
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePlacementProof != nullptr) return false;
    auto state = attempt->state.load(std::memory_order_acquire);
    if (state == ImmediateLaneAttemptState::Free) return false;
    if (state == ImmediateLaneAttemptState::PrivateCancelled) return true;
    if (state == ImmediateLaneAttemptState::PrivateLayoutHeld) return false;
    if (state == ImmediateLaneAttemptState::Reserved ||
        state == ImmediateLaneAttemptState::Prepared) {
        AttemptReadAccess access(*attempt);
        if (!access || Resolve(handle) != attempt) return false;
        state = attempt->state.load(std::memory_order_acquire);
        if (state != ImmediateLaneAttemptState::Reserved &&
            state != ImmediateLaneAttemptState::Prepared) return false;
        for (std::uint32_t index = 0; index < attempt->bucketCount; ++index)
            BucketBase(handle.slot)[index].state.store(
                ImmediateLaneBucketState::Cancelled,
                std::memory_order_relaxed);
        attempt->nextBucket.store(attempt->bucketCount,
                                  std::memory_order_relaxed);
        attempt->state.store(ImmediateLaneAttemptState::PrivateCancelled,
                             std::memory_order_release);
        return true;
    }
    if (state != ImmediateLaneAttemptState::Committed &&
        state != ImmediateLaneAttemptState::Cancelling) return false;
    attempt->state.store(ImmediateLaneAttemptState::Cancelling,
                         std::memory_order_release);
    bool allCancelled = true;
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index) {
        auto& bucket = BucketBase(handle.slot)[index];
        auto expected = ImmediateLaneBucketState::Committed;
        if (!bucket.state.compare_exchange_strong(
                expected, ImmediateLaneBucketState::Cancelled,
                std::memory_order_acq_rel, std::memory_order_acquire))
            allCancelled = allCancelled &&
                expected != ImmediateLaneBucketState::AudioClaimed;
    }
    if (allCancelled)
        attempt->nextBucket.store(attempt->bucketCount,
                                  std::memory_order_release);
    return allCancelled;
}

const ImmediateLaneBucket* ImmediateActionLane::CaptureDue(
    std::uint32_t slot, std::uint64_t quantum) noexcept {
    if (!valid_ || slot >= attempts_.size() || quantum == UINT64_MAX) return nullptr;
    auto& attempt = attempts_[slot];
    if (attempt.state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Free)
        return nullptr;
    for (std::uint32_t scanned = 0; scanned < config_.eventsPerAttempt; ++scanned) {
        AttemptReadAccess access(attempt);
        if (!access) return nullptr;
#if !defined(HARKEN_RT_QUALIFICATION)
        if (captureGateHook_ != nullptr) captureGateHook_(captureGateContext_);
#endif
        const auto phase = attempt.state.load(std::memory_order_acquire);
        if (phase != ImmediateLaneAttemptState::Committed &&
            phase != ImmediateLaneAttemptState::Cancelling) return nullptr;
        auto next = attempt.nextBucket.load(std::memory_order_relaxed);
        if (next >= attempt.bucketCount) return nullptr;
        auto& bucket = BucketBase(slot)[next];
        auto state = bucket.state.load(std::memory_order_acquire);
        if (state == ImmediateLaneBucketState::Cancelled) {
            attempt.nextBucket.store(attempt.bucketCount, std::memory_order_release);
            return nullptr;
        }
        if (Terminal(state)) {
            (void)attempt.nextBucket.compare_exchange_strong(next, next + 1u,
                std::memory_order_acq_rel, std::memory_order_acquire);
            return nullptr;
        }
        bool withdrawnOld = bucket.quantum < quantum &&
            state == ImmediateLaneBucketState::Committed &&
            EntireConditionalProjectionBucket(slot, bucket) &&
            (attempt.conditionalProjectionAdmission.load(std::memory_order_acquire) &
                kEventWithdrawalRequested) != 0;
        const auto sourceWait = bucket.sourceWaitSequence.load(std::memory_order_acquire);
        const bool resuming = sourceWait != 0 && sourceWait <= quantum + 1u && bucket.quantum <= quantum;
        if (attempt.queueContinuation != nullptr && bucket.quantum <= quantum &&
            state == ImmediateLaneBucketState::Committed && !attempt.queueContinuation->BeginAudio()) {
            auto expected = ImmediateLaneBucketState::Committed;
            (void)bucket.state.compare_exchange_strong(expected, ImmediateLaneBucketState::Cancelled,
                std::memory_order_acq_rel, std::memory_order_acquire);
            attempt.nextBucket.store(attempt.bucketCount, std::memory_order_release);
            attempt.queueContinuation->EndAudio();
            return nullptr;
        }
        if (bucket.quantum < quantum && state == ImmediateLaneBucketState::Committed && !withdrawnOld && !resuming) {
            BeginConditionalProjection(slot, bucket, quantum + 1u);
            if (attempt.capturedConditionalProjection) {
                if (attempt.capturedConditionalReplacementRead)
                    attempt.conditionalReplacement->BeforeOldWrite();
                attempt.conditionalProjectionAdmission.fetch_or(kProjectionWritten, std::memory_order_acq_rel);
            }
            EndConditionalProjection(slot);
            withdrawnOld = EntireConditionalProjectionBucket(slot, bucket) &&
                (attempt.conditionalProjectionAdmission.load(std::memory_order_acquire) & kEventWithdrawalRequested) != 0;
        }
        if (bucket.quantum < quantum && state == ImmediateLaneBucketState::Committed && !withdrawnOld && !resuming) {
            if (bucket.state.compare_exchange_strong(state, ImmediateLaneBucketState::Missed,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                attempt.state.store(ImmediateLaneAttemptState::Cancelling, std::memory_order_release);
                attempt.nextBucket.store(attempt.bucketCount, std::memory_order_release);
            }
            if (attempt.queueContinuation != nullptr) attempt.queueContinuation->EndAudio();
            return nullptr;
        }
        if ((bucket.quantum == quantum || withdrawnOld || resuming) && state == ImmediateLaneBucketState::Committed) {
            attempt.capturedBucketOrdinal = next;
            attempt.capturedAudioSequence = quantum + 1u;
            BeginConditionalProjection(slot, bucket, quantum + 1u);
            BeginEventAdmission(slot, bucket);
            BeginPrivateEntryAdmission(slot, next);
            attempt.captureActive.store(true, std::memory_order_relaxed);
            access.Detach();
#if !defined(HARKEN_RT_QUALIFICATION)
            if (capturedAdmissionHook_ != nullptr)
                capturedAdmissionHook_(capturedAdmissionContext_, slot);
#endif
            if (!withdrawnOld) return &bucket;
            // Even an overdue withdrawal is closed through a real native claim.
            const ImmediateLaneHandle handle{slot, attempt.generation};
            if (!ClaimCancelledOnly(handle, &bucket) || !FinishCancelledOnly(slot, &bucket)) {
                (void)ReleaseCapture(slot, &bucket);
                return nullptr;
            }
            continue;
        }
        return nullptr;
    }
    return nullptr;
}

const QueueContinuationLane* ImmediateActionLane::CapturedQueueContinuation(
    std::uint32_t slot, const ImmediateLaneBucket* bucket) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return nullptr;
    const auto& attempt = attempts_[slot];
    return attempt.captureActive.load(std::memory_order_acquire) &&
        attempt.capturedBucketOrdinal < attempt.bucketCount &&
        bucket == BucketBase(slot) + attempt.capturedBucketOrdinal &&
        bucket->attemptGeneration == attempt.generation ? attempt.queueContinuation : nullptr;
}

bool ImmediateActionLane::CapturedSourceWaitQuantum(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, std::uint64_t* outQuantum) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr || outQuantum == nullptr) return false;
    const auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation || attempt.capturedAudioSequence == 0 ||
        bucket->sourceWaitSequence.load(std::memory_order_acquire) == 0 ||
        bucket->sourceWaitSequence.load(std::memory_order_acquire) > attempt.capturedAudioSequence) return false;
    *outQuantum = attempt.capturedAudioSequence - 1u;
    return true;
}

std::span<FrontierEntryReceiptRef> ImmediateActionLane::CapturedSourceWaitScratch(
    std::uint32_t slot, const ImmediateLaneBucket* bucket) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return {};
    const auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation) return {};
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        config_.maxEventsPerQuantum, static_cast<std::uint64_t>(attempt.eventCredit) + attempt.groupCredit));
    return {metadata_[slot].active.waitingScratch, count};
}

bool ImmediateActionLane::SuspendCaptureForSourceWait(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, const FrontierEntryReceiptRef* waitingEntries,
    std::uint32_t waitingEntryCount) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr ||
        waitingEntryCount > config_.eventsPerAttempt ||
        (waitingEntryCount != 0 && waitingEntries == nullptr)) return false;
    auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation ||
        attempt.nextBucket.load(std::memory_order_acquire) != attempt.capturedBucketOrdinal ||
        attempt.state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Committed ||
        bucket->state.load(std::memory_order_acquire) != ImmediateLaneBucketState::Committed ||
        attempt.capturedAudioSequence == 0 || AllCapturedEntriesWithdrawing(slot, bucket)) return false;
    auto* waits = sourceWaitEntries_.At(static_cast<std::size_t>(slot) * config_.eventsPerAttempt);
    const auto originalCount = attempt.sourceWaitEntryCount;
    const auto fail = [&]() noexcept {
        while (attempt.sourceWaitEntryCount > originalCount)
            waits[--attempt.sourceWaitEntryCount] = {};
        return false;
    };
    const auto record = [&](const FrontierEntryReceiptRef& identity) noexcept {
        if (!identity.Valid()) return true;
        for (std::uint32_t index = 0; index < attempt.sourceWaitEntryCount; ++index)
            if (waits[index].entry == identity) return true;
        if (attempt.sourceWaitEntryCount == config_.eventsPerAttempt) return false;
        waits[attempt.sourceWaitEntryCount++] = {identity, attempt.capturedAudioSequence};
        return true;
    };
    for (std::uint32_t entry = 0; entry < waitingEntryCount; ++entry) {
        const auto& identity = waitingEntries[entry];
        if (!identity.Valid()) return fail();
        bool admitted = false;
        for (std::uint32_t index = 0; index < bucket->eventCount; ++index)
            admitted = admitted || (IsEventAdmitted(slot, bucket, bucket->firstEvent + index) &&
                EventBase(slot)[bucket->firstEvent + index].event.frontier.entry == identity);
        for (std::uint32_t index = 0; index < bucket->groupCount; ++index) {
            if (!IsGroupAdmitted(slot, bucket, bucket->firstGroup + index)) continue;
            Plan03PageGroupView group{};
            if (attempt.pinGroupPool == nullptr ||
                !attempt.pinGroupPool->ResolvePlan03GroupSliceRt(GroupBase(slot)[bucket->firstGroup + index].identity, &group) ||
                group.frontier == nullptr) return fail();
            admitted = admitted || group.frontier->entry == identity;
        }
        if (!admitted || !record(identity)) return fail();
    }
    auto entries = PrivateEntries(slot);
    for (std::uint32_t ordinal = 0; ordinal < attempt.privateEntryCount; ++ordinal) {
        auto& entry = entries[ordinal];
        auto* current = const_cast<PrivateEntryBucket*>(PrivateEntryBucketFor(entry, attempt.capturedBucketOrdinal));
        if (current != nullptr && current->outcome.load(std::memory_order_relaxed) == ImmediateLaneEntryOutcome::Admitting) {
            current->outcome.store(ImmediateLaneEntryOutcome::Dormant, std::memory_order_relaxed);
            (void)entry.admission.fetch_and(static_cast<std::uint8_t>(~kEntryAudioAdmitting), std::memory_order_release);
        }
    }
    BucketBase(slot)[attempt.capturedBucketOrdinal].sourceWaitSequence.store(
        attempt.capturedAudioSequence, std::memory_order_release);
    PreserveSourceWaitSuccessors(slot);
    EndEventAdmission(slot, *bucket);
    EndConditionalProjection(slot);
    attempt.capturedBucketOrdinal = UINT32_MAX;
    attempt.captureActive.store(false, std::memory_order_relaxed);
    attempt.readerGate.clear(std::memory_order_release);
    return true;
}

void ImmediateActionLane::PreserveSourceWaitSuccessors(std::uint32_t slot) noexcept {
    const auto& attempt = attempts_[slot];
    const auto* waits = sourceWaitEntries_.At(static_cast<std::size_t>(slot) * config_.eventsPerAttempt);
    const auto retained = [&](const FrontierEntryReceiptRef& entry) noexcept {
        for (std::uint32_t index = 0; index < attempt.sourceWaitEntryCount; ++index)
            if (waits[index].entry == entry) return true;
        return false;
    };
    // Retention follows the exact suspended Entry, including not-yet-due work.
    for (auto index = attempt.capturedBucketOrdinal + 1u; index < attempt.bucketCount; ++index) {
        auto& successor = BucketBase(slot)[index];
        bool related = false;
        for (std::uint32_t event = 0; event < successor.eventCount; ++event)
            related = retained(EventBase(slot)[successor.firstEvent + event].event.frontier.entry) || related;
        for (std::uint32_t group = 0; group < successor.groupCount; ++group) {
            Plan03PageGroupView view{};
            if (attempt.pinGroupPool != nullptr &&
                attempt.pinGroupPool->ResolvePlan03GroupSliceRt(GroupBase(slot)[successor.firstGroup + group].identity, &view) &&
                view.frontier != nullptr)
                related = retained(view.frontier->entry) ||
                    (view.frontier->role == FrontierGroupRole::Coordinator &&
                     view.frontier->prerequisiteMember.Exact() && retained(view.frontier->prerequisite)) || related;
        }
        if (related && successor.state.load(std::memory_order_acquire) == ImmediateLaneBucketState::Committed)
            successor.sourceWaitSequence.store(attempt.capturedAudioSequence, std::memory_order_release);
    }
}

bool ImmediateActionLane::CapturedEntrySourceWait(std::uint32_t slot, const ImmediateLaneBucket* bucket,
    const FrontierEntryReceiptRef& identity) const noexcept {
    std::uint64_t quantum = 0;
    if (!identity.Valid() || !CapturedSourceWaitQuantum(slot, bucket, &quantum)) return false;
    const auto& attempt = attempts_[slot];
    const auto* waits = sourceWaitEntries_.At(static_cast<std::size_t>(slot) * config_.eventsPerAttempt);
    for (std::uint32_t index = 0; index < attempt.sourceWaitEntryCount; ++index)
        if (waits[index].entry == identity && waits[index].sequence != 0 &&
            waits[index].sequence <= quantum + 1u) return true;
    return false;
}

bool ImmediateActionLane::Claim(ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket) noexcept {
    return Claim(handle, bucket, nullptr);
}

bool ImmediateActionLane::Claim(ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket,
    ImmediateLaneBucketClaimReceipt* receipt) noexcept {
    return ClaimImpl(handle, bucket, receipt, true);
}

bool ImmediateActionLane::ClaimRetainingCaptureOnRefusal(ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket) noexcept {
    return ClaimImpl(handle, bucket, nullptr, false);
}

bool ImmediateActionLane::ClaimImpl(ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket, ImmediateLaneBucketClaimReceipt* receipt,
    bool releaseOnRefusal) noexcept {
    if (receipt != nullptr) *receipt = {};
    if (!valid_ || !handle.Valid() || handle.slot >= attempts_.size() ||
        bucket == nullptr)
        return false;
    auto& attempt = attempts_[handle.slot];
    if (attempt.generation != handle.generation ||
        bucket->attemptGeneration != handle.generation) return false;
    if (!attempt.captureActive.load(std::memory_order_relaxed)) return false;
    const auto next = attempt.nextBucket.load(std::memory_order_acquire);
    if (next >= attempt.bucketCount ||
        bucket != BucketBase(handle.slot) + next) {
        if (releaseOnRefusal) (void)ReleaseCapture(handle.slot, bucket);
        return false;
    }
    auto expected = ImmediateLaneBucketState::Committed;
    const bool claimed = BucketBase(handle.slot)[next].state.compare_exchange_strong(
        expected, ImmediateLaneBucketState::AudioClaimed,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (!claimed) {
        if (releaseOnRefusal) (void)ReleaseCapture(handle.slot, bucket);
    } else if (receipt != nullptr) {
        *receipt = ImmediateLaneBucketClaimReceipt(
            handle.slot, handle.generation, next, bucket->quantum);
    }
    return claimed;
}

bool ImmediateActionLane::ValidateClaimReceipt(
    const ImmediateLaneBucketClaimReceipt& receipt,
    const ImmediateLaneBucket* bucket) const noexcept {
    if (!valid_ || !receipt.Valid() || bucket == nullptr ||
        receipt.Slot() >= attempts_.size()) return false;
    const auto& attempt = attempts_[receipt.Slot()];
    return attempt.generation == receipt.Generation() &&
        attempt.captureActive.load(std::memory_order_acquire) &&
        attempt.nextBucket.load(std::memory_order_acquire) ==
            receipt.BucketOrdinal() &&
        receipt.BucketOrdinal() < attempt.bucketCount &&
        bucket == BucketBase(receipt.Slot()) + receipt.BucketOrdinal() &&
        bucket->attemptGeneration == receipt.Generation() &&
        bucket->quantum == receipt.Quantum() &&
        bucket->state.load(std::memory_order_acquire) ==
            ImmediateLaneBucketState::AudioClaimed;
}

bool ImmediateActionLane::ValidateGroupClaimReceipt(
    const ImmediateLaneBucketClaimReceipt& receipt,
    const Plan03GroupSliceIdentity& group,
    std::uint32_t bucketOrdinal,
    std::uint32_t groupIndex) const noexcept {
    if (!valid_ || !receipt.Valid() || !group.Exact() ||
        receipt.Slot() >= attempts_.size() ||
        receipt.BucketOrdinal() != bucketOrdinal) return false;
    const auto& attempt = attempts_[receipt.Slot()];
    if (!attempt.groupPinsInstalled ||
        bucketOrdinal >= attempt.bucketCount ||
        groupIndex >= attempt.groupCount) return false;
    const auto* bucket = BucketBase(receipt.Slot()) +
        bucketOrdinal;
    if (!ValidateClaimReceipt(receipt, bucket)) return false;
    if (!IsGroupAdmitted(receipt.Slot(), bucket, groupIndex)) return false;
    if (bucket->firstGroup > attempt.groupCount ||
        bucket->groupCount > attempt.groupCount - bucket->firstGroup)
        return false;
    if (groupIndex < bucket->firstGroup ||
        groupIndex >= bucket->firstGroup + bucket->groupCount)
        return false;
    const auto* groups = GroupBase(receipt.Slot());
    return groups[groupIndex].identity == group &&
        groups[groupIndex].dueQuantum == receipt.Quantum();
}

bool ImmediateActionLane::CapturePublishedGroupDescriptor(
    ImmediateLaneHandle handle, std::uint32_t groupIndex,
    ImmediateLaneGroupRef* out) const noexcept {
    if (!valid_ || out == nullptr || !handle.Valid() ||
        handle.slot >= attempts_.size()) return false;
    const auto& attempt = attempts_[handle.slot];
    if (attempt.generation != handle.generation ||
        attempt.state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed ||
        !attempt.captureActive.load(std::memory_order_acquire) ||
        !attempt.groupPinsInstalled || groupIndex >= attempt.groupCount)
        return false;
    const auto descriptor = GroupBase(handle.slot)[groupIndex];
    if (!descriptor.identity.Exact() ||
        descriptor.laneSlot != handle.slot ||
        descriptor.laneGeneration != handle.generation ||
        descriptor.groupIndex != groupIndex ||
        descriptor.bucketOrdinal >= attempt.bucketCount) return false;
    const auto& bucket = BucketBase(handle.slot)[descriptor.bucketOrdinal];
    if (bucket.attemptGeneration != handle.generation ||
        bucket.firstGroup > attempt.groupCount ||
        bucket.groupCount > attempt.groupCount - bucket.firstGroup ||
        groupIndex < bucket.firstGroup ||
        groupIndex >= bucket.firstGroup + bucket.groupCount ||
        descriptor.dueQuantum != bucket.quantum) return false;
    *out = descriptor;
    return true;
}

bool ImmediateActionLane::CapturePublishedGroupCount(
    ImmediateLaneHandle handle, std::uint32_t* out) const noexcept {
    if (!valid_ || out == nullptr || !handle.Valid() ||
        handle.slot >= attempts_.size()) return false;
    const auto& attempt = attempts_[handle.slot];
    if (attempt.generation != handle.generation ||
        attempt.state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed ||
        !attempt.captureActive.load(std::memory_order_acquire) ||
        !attempt.groupPinsInstalled) return false;
    *out = attempt.groupCount;
    return true;
}

bool ImmediateActionLane::CapturePrivatePlacedInitialSource(
    const PrivateImmediateLanePlacementProof& placement,
    const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
    const DecisionTable& decisions, const FrontierGroupBinding& initial,
    TimelineArmToken source, std::uint64_t* outFrame) const noexcept {
    if (outFrame == nullptr || !MatchesPrivatePlacementProof(placement) ||
        !source.Valid() || !initial.Valid() || !initial.queueInitial.Valid()) return false;
    const auto* attempt = placement.attempt_;
    const auto handle = placement.handle_;
    if (!attempt->pinsInstalled || !attempt->groupPinsInstalled ||
        attempt->pinGroupPool != &pool || attempt->groupCount == 0) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* groups = GroupBase(handle.slot);
    const auto* events = EventBase(handle.slot);
    bool selectedInitial = false;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        const auto& decision = groupState_[base + index].decision;
        if (!decision.Valid() || decision.Owner() != &decisions ||
            decision.Token().slot != groups[index].identity.decision.slot ||
            decision.Token().generation != groups[index].identity.decision.generation ||
            decisions.IsCancelled(decision.Token())) return false;
        selectedInitial = selectedInitial || groups[index].identity == initial.operation.group;
    }
    if (!selectedInitial) return false;
    std::uint32_t found = 0;
    std::uint64_t sourceFrame = 0;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& item = events[index];
        const auto& event = item.event;
        if (event.armToken.slot != source.slot || event.armToken.generation != source.generation ||
            event.armToken.renderSlot != source.renderSlot ||
            event.armToken.renderGeneration != source.renderGeneration) continue;
        const auto* program = event.program.program;
        if (program == nullptr || program->payloads == nullptr || event.payload >= program->payloadCount)
            return false;
        if (program->payloads[event.payload].kind != TimelineEventKind::Start) continue;
        const auto& pin = eventState_[base + index].arm;
        const auto* row = arms.Resolve(source);
        std::uint64_t frame = 0;
        if (++found != 1 || !pin.Valid() || pin.owner_ != &arms ||
            pin.token_.slot != source.slot || pin.token_.generation != source.generation ||
            pin.token_.renderSlot != source.renderSlot ||
            pin.token_.renderGeneration != source.renderGeneration || row == nullptr ||
            program->role != TimelineProgramRole::Voice || program->authoredStopFence != nullptr ||
            program->generation != event.program.programGeneration ||
            item.armProgramGeneration == 0 || row->programGeneration != item.armProgramGeneration ||
            program->payloads[event.payload].reserved != 0 ||
            program->payloads[event.payload].target != row->target || event.subeventPhase != 20 ||
            !ResolveLaneDue(item, placement.anchor_, &frame) || event.executeFrame != frame ||
            !row->pendingImmediateStart || !row->startBound || row->startFrame != frame ||
            row->authoredControl.operation != nullptr ||
            row->frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
            row->frontierAdmissionFinished.load(std::memory_order_acquire) ||
            frame < initial.operation.order.executeFrame || !event.frontier.Valid() ||
            !(event.frontier.owner == initial.operation.owner) ||
            !(event.frontier.entry == initial.entry) || event.frontier.world != initial.operation.world ||
            event.frontier.transport != initial.operation.transport ||
            (event.frontier.group != initial.operation.group && event.frontier.group != initial.primary)) return false;
        Plan03PageGroupView admission{};
        if (!pool.ResolvePlan03GroupSliceProducer(event.frontier.group, &admission) ||
            admission.frontier == nullptr || !admission.frontier->Valid() || admission.lease == nullptr ||
            row->frontierAdmission != admission.frontier || row->frontierAdmissionLease != admission.lease ||
            !(admission.frontier->entry == initial.entry) ||
            !(admission.frontier->operation.owner == initial.operation.owner)) return false;
        if (row->parentGroup.Valid()) {
            if (!item.futureGroupedStart || admission.frontier->operation.group != initial.operation.group ||
                row->parentGroup.decision.slot != initial.operation.group.decision.slot ||
                row->parentGroup.decision.generation != initial.operation.group.decision.generation ||
                row->memberIndex >= admission.memberCount) return false;
            const auto actual = admission.members[row->memberIndex];
            if (actual.slot != source.slot || actual.generation != source.generation ||
                actual.renderSlot != source.renderSlot || actual.renderGeneration != source.renderGeneration)
                return false;
        } else if (item.futureGroupedStart || row->memberIndex != UINT16_MAX) return false;
        // These fields are immutable after the producer's true bind. Audio only
        // publishes state/sequence atomics; no seqlock reads plain mutable data.
        TimelineArmObservation observed{};
        if (!arms.Observe(source, &observed) || observed.armedCompletionSequence == 0 ||
            observed.startedBlockSequence != 0 || observed.logicalCompletionSequence != 0 ||
            observed.tombstoneAudioSequence != 0 ||
            (observed.state != ArmState::Armed && observed.state != ArmState::Published)) return false;
        sourceFrame = frame;
    }
    if (found != 1) return false;
    *outFrame = sourceFrame;
    return true;
}

bool ImmediateActionLane::CapturePrivatePlacedC0Source(
    const PrivateImmediateLanePlacementProof& placement,
    const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
    const DecisionTable& decisions, const FrontierGroupBinding& primary,
    const FrontierGroupBinding& initial,
    TimelineArmToken source, std::uint64_t* outFrame) const noexcept {
    if (outFrame == nullptr || !MatchesPrivatePlacementProof(placement) ||
        !source.Valid() || !initial.Valid() || !initial.queueInitial.Valid()) return false;
    Plan03PageGroupView actualPrimary{}, actualInitial{};
    if (!primary.Valid() || primary.role != FrontierGroupRole::Coordinator ||
        primary.operation.kind != FrontierOperationKind::Entry ||
        primary.primary != primary.operation.group || primary.queueInitial.Valid() ||
        primary.queueConditional.Valid() || primary.variableSide.Valid() ||
        initial.role != FrontierGroupRole::EntryMember ||
        initial.operation.kind != FrontierOperationKind::Entry ||
        initial.primary != primary.operation.group || !(initial.entry == primary.entry) ||
        initial.queueConditional.Valid() || initial.variableSide.Valid() ||
        initial.operation.world != primary.operation.world ||
        initial.operation.transport != primary.operation.transport ||
        !pool.ResolvePlan03GroupSliceProducer(primary.operation.group, &actualPrimary) ||
        !pool.ResolvePlan03GroupSliceProducer(initial.operation.group, &actualInitial) ||
        actualPrimary.frontier != &primary || actualInitial.frontier != &initial ||
        actualPrimary.memberCount != 0 || actualPrimary.sourceControlCount != 0 ||
        actualPrimary.lease == nullptr || actualInitial.lease != actualPrimary.lease) return false;
    const auto* attempt = placement.attempt_;
    const auto handle = placement.handle_;
    if (!attempt->pinsInstalled || !attempt->groupPinsInstalled ||
        attempt->pinGroupPool != &pool || attempt->groupCount == 0) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    const auto* groups = GroupBase(handle.slot);
    const auto* events = EventBase(handle.slot);
    std::uint32_t selectedC0 = 0;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        const bool selected = groups[index].identity == primary.operation.group ||
            groups[index].identity == initial.operation.group;
        if (!selected) continue;
        ++selectedC0;
        const auto& decision = groupState_[base + index].decision;
        if (!decision.Valid() || decision.Owner() != &decisions ||
            decision.Token().slot != groups[index].identity.decision.slot ||
            decision.Token().generation != groups[index].identity.decision.generation ||
            decisions.IsCancelled(decision.Token())) return false;
    }
    if (selectedC0 != 2u) return false;
    std::uint32_t found = 0;
    std::uint64_t sourceFrame = 0;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index) {
        const auto& item = events[index];
        const auto& event = item.event;
        if (event.armToken.slot != source.slot || event.armToken.generation != source.generation ||
            event.armToken.renderSlot != source.renderSlot ||
            event.armToken.renderGeneration != source.renderGeneration) continue;
        const auto* program = event.program.program;
        if (program == nullptr || program->payloads == nullptr || event.payload >= program->payloadCount)
            return false;
        if (program->payloads[event.payload].kind != TimelineEventKind::Start) continue;
        const auto& pin = eventState_[base + index].arm;
        const auto* row = arms.Resolve(source);
        const auto& sourceOwner = item.futureGroupedStart
            ? initial.operation.owner : primary.operation.owner;
        const auto& sourceGroup = item.futureGroupedStart
            ? initial.operation.group : primary.operation.group;
        std::uint64_t frame = 0;
        if (++found != 1 || !pin.Valid() || pin.owner_ != &arms ||
            pin.token_.slot != source.slot || pin.token_.generation != source.generation ||
            pin.token_.renderSlot != source.renderSlot ||
            pin.token_.renderGeneration != source.renderGeneration || row == nullptr ||
            program->role != TimelineProgramRole::Voice || program->authoredStopFence != nullptr ||
            program->generation != event.program.programGeneration ||
            item.armProgramGeneration == 0 || row->programGeneration != item.armProgramGeneration ||
            program->payloads[event.payload].reserved != 0 ||
            program->payloads[event.payload].target != row->target || event.subeventPhase != 20 ||
            !ResolveLaneDue(item, placement.anchor_, &frame) || event.executeFrame != frame ||
            !row->pendingImmediateStart || !row->startBound || row->startFrame != frame ||
            row->authoredControl.operation != nullptr ||
            row->frontierActiveLease.load(std::memory_order_acquire) != nullptr ||
            row->frontierAdmissionFinished.load(std::memory_order_acquire) ||
            frame < initial.operation.order.executeFrame || !event.frontier.Valid() ||
            !(event.frontier.owner == sourceOwner) ||
            !(event.frontier.entry == initial.entry) || event.frontier.world != initial.operation.world ||
            event.frontier.transport != initial.operation.transport ||
            event.frontier.group != sourceGroup) return false;
        Plan03PageGroupView admission{};
        if (!pool.ResolvePlan03GroupSliceProducer(event.frontier.group, &admission) ||
            admission.frontier == nullptr || !admission.frontier->Valid() || admission.lease == nullptr ||
            row->frontierAdmission != admission.frontier || row->frontierAdmissionLease != admission.lease ||
            !(admission.frontier->entry == initial.entry) ||
            !(admission.frontier->operation.owner == sourceOwner)) return false;
        if (row->parentGroup.Valid()) {
            if (!item.futureGroupedStart || admission.frontier->operation.group != initial.operation.group ||
                row->parentGroup.decision.slot != initial.operation.group.decision.slot ||
                row->parentGroup.decision.generation != initial.operation.group.decision.generation ||
                row->memberIndex >= admission.memberCount) return false;
            const auto actual = admission.members[row->memberIndex];
            if (actual.slot != source.slot || actual.generation != source.generation ||
                actual.renderSlot != source.renderSlot || actual.renderGeneration != source.renderGeneration)
                return false;
        } else if (item.futureGroupedStart || row->memberIndex != UINT16_MAX) return false;
        // These fields are immutable after the producer's true bind. Audio only
        // publishes state/sequence atomics; no seqlock reads plain mutable data.
        TimelineArmObservation observed{};
        if (!arms.Observe(source, &observed) || observed.armedCompletionSequence == 0 ||
            observed.startedBlockSequence != 0 || observed.logicalCompletionSequence != 0 ||
            observed.tombstoneAudioSequence != 0 ||
            (observed.state != ArmState::Armed && observed.state != ArmState::Published)) return false;
        sourceFrame = frame;
    }
    if (found != 1) return false;
    *outFrame = sourceFrame;
    return true;
}

bool ImmediateActionLane::ProvePrivateGroupPlacement(
    ImmediateLaneHandle handle,
    const Plan03GroupSliceIdentity& group,
    std::uint32_t bucketOrdinal,
    std::uint32_t groupIndex) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || !group.Exact() ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Prepared ||
        bucketOrdinal >= attempt->bucketCount ||
        groupIndex >= attempt->groupCount) return false;
    const auto& bucket = BucketBase(handle.slot)[bucketOrdinal];
    if (bucket.firstGroup > attempt->groupCount ||
        bucket.groupCount > attempt->groupCount - bucket.firstGroup ||
        groupIndex < bucket.firstGroup ||
        groupIndex >= bucket.firstGroup + bucket.groupCount)
        return false;
    const auto& actual = GroupBase(handle.slot)[groupIndex];
    return actual.identity == group &&
        actual.dueQuantum == bucket.quantum &&
        actual.bucketOrdinal == bucketOrdinal &&
        actual.groupIndex == groupIndex;
}

bool ImmediateActionLane::FinishDiscardedPlainWithdrawals(std::uint32_t slot,
    const ImmediateLaneBucket* bucket) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return false;
    auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) || !attempt.pinsInstalled ||
        attempt.state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Committed ||
        attempt.groupCount != 0 || attempt.memberCount != 0 ||
        attempt.declaredGroupCount != 0 || attempt.privateEntryCount != 0 ||
        attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        attempt.nextBucket.load(std::memory_order_acquire) != attempt.capturedBucketOrdinal ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal ||
        bucket->attemptGeneration != attempt.generation || bucket->eventCount == 0 ||
        bucket->state.load(std::memory_order_acquire) != ImmediateLaneBucketState::Committed)
        return false;
    const auto offset = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    for (std::uint32_t index = bucket->firstEvent;
         index < bucket->firstEvent + bucket->eventCount; ++index)
        if ((eventState_[offset + eventState_[offset + index].withdrawalLeader].withdrawal.load(std::memory_order_acquire) &
                kEventWithdrawalRequested) == 0) return false;
    // Discard has returned all original payload use; release before closing its projection.
    EndEventAdmission(slot, *bucket);
    for (std::uint32_t index = bucket->firstEvent;
         index < bucket->firstEvent + bucket->eventCount; ++index)
        eventState_[offset + index].capturedAdmission = 0;
    const ImmediateLaneHandle handle{slot, attempt.generation};
    return ClaimRetainingCaptureOnRefusal(handle, bucket) && FinishCancelledOnly(slot, bucket);
}

bool ImmediateActionLane::ReleaseCapture(std::uint32_t slot,
    const ImmediateLaneBucket* bucket) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return false;
    auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_relaxed)) return false;
    if (attempt.capturedBucketOrdinal >= attempt.bucketCount ||
        bucket != BucketBase(slot) + attempt.capturedBucketOrdinal) return false;
    const auto next = attempt.nextBucket.load(std::memory_order_acquire);
    const bool cancelledHead = next >= attempt.bucketCount &&
        attempt.state.load(std::memory_order_acquire) ==
            ImmediateLaneAttemptState::Cancelling;
    if ((!cancelledHead && (next >= attempt.bucketCount ||
                           bucket != BucketBase(slot) + next)) ||
        bucket->state.load(std::memory_order_acquire) ==
            ImmediateLaneBucketState::AudioClaimed)
        return false;
    if (attempt.privateEntryCount == 0 &&
        bucket->state.load(std::memory_order_acquire) == ImmediateLaneBucketState::Committed &&
        AllCapturedEntriesWithdrawing(slot, bucket)) {
        const ImmediateLaneHandle handle{slot, attempt.generation};
        if (ClaimRetainingCaptureOnRefusal(handle, bucket))
            return FinishCancelledOnly(slot, bucket);
    }
    if (attempt.privateEntryCount != 0) {
        auto& actual = BucketBase(slot)[attempt.capturedBucketOrdinal];
        auto expected = ImmediateLaneBucketState::Committed;
        const auto terminal = AllCapturedEntriesWithdrawing(slot, bucket)
            ? ImmediateLaneBucketState::CancelledOnly : ImmediateLaneBucketState::Missed;
        if (actual.state.compare_exchange_strong(expected, terminal,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (terminal == ImmediateLaneBucketState::Missed) {
                attempt.state.store(ImmediateLaneAttemptState::Cancelling,
                                    std::memory_order_release);
                attempt.nextBucket.store(attempt.bucketCount, std::memory_order_release);
            } else {
                auto expectedNext = attempt.capturedBucketOrdinal;
                (void)attempt.nextBucket.compare_exchange_strong(expectedNext,
                    expectedNext + 1u, std::memory_order_acq_rel, std::memory_order_acquire);
            }
        }
        EndPrivateEntryAdmission(slot, bucket, false);
    }
    EndEventAdmission(slot, *bucket);
    EndConditionalProjection(slot);
    attempt.capturedBucketOrdinal = UINT32_MAX;
    attempt.captureActive.store(false, std::memory_order_relaxed);
    attempt.readerGate.clear(std::memory_order_release);
    return true;
}

bool ImmediateActionLane::RecordAdmissionFailure(
    std::uint32_t slot, const ImmediateLaneBucket* bucket,
    ImmediateLaneAdmissionFailure failure) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr ||
        failure != ImmediateLaneAdmissionFailure::ArenaCapacity) return false;
    auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed) return false;
    const auto next = attempt.nextBucket.load(std::memory_order_acquire);
    if (next >= attempt.bucketCount || bucket != BucketBase(slot) + next ||
        bucket->attemptGeneration != attempt.generation ||
        bucket->state.load(std::memory_order_acquire) !=
            ImmediateLaneBucketState::Committed) return false;
    BucketBase(slot)[next].admissionFailure.store(failure,
                                                  std::memory_order_release);
    return true;
}

bool ImmediateActionLane::RejectCapturedBucket(
    ImmediateLaneHandle handle, const ImmediateLaneBucket* bucket) noexcept {
    if (!valid_ || !handle.Valid() || handle.slot >= attempts_.size() ||
        bucket == nullptr) return false;
    auto& attempt = attempts_[handle.slot];
    if (attempt.generation != handle.generation ||
        !attempt.captureActive.load(std::memory_order_acquire) ||
        attempt.state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed) return false;
    const auto next = attempt.nextBucket.load(std::memory_order_acquire);
    if (next >= attempt.bucketCount ||
        bucket != BucketBase(handle.slot) + next) return false;
    auto& actual = BucketBase(handle.slot)[next];
    if (actual.attemptGeneration != handle.generation ||
        actual.state.load(std::memory_order_acquire) !=
            ImmediateLaneBucketState::Committed) return false;
    auto noFailure = ImmediateLaneAdmissionFailure::None;
    (void)actual.admissionFailure.compare_exchange_strong(
        noFailure, ImmediateLaneAdmissionFailure::FrontierValidation,
        std::memory_order_release, std::memory_order_relaxed);
    actual.completedAudioSequence.store(attempt.capturedAudioSequence, std::memory_order_relaxed);
    auto expected = ImmediateLaneBucketState::Committed;
    if (!actual.state.compare_exchange_strong(
            expected, ImmediateLaneBucketState::Missed,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    attempt.state.store(ImmediateLaneAttemptState::Cancelling,
                        std::memory_order_release);
    attempt.nextBucket.store(attempt.bucketCount, std::memory_order_release);
#if !defined(HARKEN_RT_QUALIFICATION)
    if (validationRefusalHook_ != nullptr)
        validationRefusalHook_(validationRefusalContext_);
#endif
    return true;
}

bool ImmediateActionLane::Finish(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, bool applied) noexcept {
    return FinishBucket(slot, bucket, applied ? ImmediateLaneBucketState::Applied
                                            : ImmediateLaneBucketState::Missed);
}

bool ImmediateActionLane::ClaimCancelledOnly(ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket) noexcept {
    return valid_ && handle.Valid() && handle.slot < attempts_.size() &&
        attempts_[handle.slot].generation == handle.generation &&
        AllCapturedEntriesWithdrawing(handle.slot, bucket) && Claim(handle, bucket);
}

bool ImmediateActionLane::FinishCancelledOnly(std::uint32_t slot,
    const ImmediateLaneBucket* bucket) noexcept {
    if (!AllCapturedEntriesWithdrawing(slot, bucket)) return false;
    return FinishBucket(slot, bucket, ImmediateLaneBucketState::CancelledOnly);
}

bool ImmediateActionLane::FinishBucket(std::uint32_t slot,
    const ImmediateLaneBucket* bucket, ImmediateLaneBucketState terminal) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr)
        return false;
    auto& attempt = attempts_[slot];
    if (!attempt.captureActive.load(std::memory_order_relaxed)) return false;
    const auto next = attempt.nextBucket.load(std::memory_order_acquire);
    if (next >= attempt.bucketCount || bucket != BucketBase(slot) + next)
        return false;
    auto expected = ImmediateLaneBucketState::AudioClaimed;
    BucketBase(slot)[next].completedAudioSequence.store(attempt.capturedAudioSequence, std::memory_order_relaxed);
    if (!BucketBase(slot)[next].state.compare_exchange_strong(
            expected, terminal,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return false;
    if (terminal == ImmediateLaneBucketState::Applied &&
        bucket->sourceWaitSequence.load(std::memory_order_acquire) != 0)
        PreserveSourceWaitSuccessors(slot);
    EndPrivateEntryAdmission(slot, bucket, terminal == ImmediateLaneBucketState::Applied);
    EndEventAdmission(slot, *bucket);
    EndConditionalProjection(slot);
    attempt.capturedBucketOrdinal = UINT32_MAX;
    if (terminal == ImmediateLaneBucketState::Missed) {
        attempt.state.store(ImmediateLaneAttemptState::Cancelling,
                            std::memory_order_release);
        attempt.nextBucket.store(attempt.bucketCount,
                                 std::memory_order_release);
        attempt.captureActive.store(false, std::memory_order_relaxed);
        attempt.readerGate.clear(std::memory_order_release);
        return true;
    }
    const bool cancelling = attempt.state.load(std::memory_order_acquire) ==
        ImmediateLaneAttemptState::Cancelling;
#if !defined(HARKEN_RT_QUALIFICATION)
    if (finishHeadHook_ != nullptr) finishHeadHook_(finishHeadContext_);
#endif
    auto expectedNext = next;
    (void)attempt.nextBucket.compare_exchange_strong(
        expectedNext, cancelling ? attempt.bucketCount : next + 1u,
        std::memory_order_acq_rel, std::memory_order_acquire);
    attempt.captureActive.store(false, std::memory_order_relaxed);
    attempt.readerGate.clear(std::memory_order_release);
    return true;
}

const TimelineArmReaderPin* ImmediateActionLane::CapturedArmReader(
    ImmediateLaneHandle handle, const ImmediateLaneBucket* bucket,
    std::uint32_t eventIndex) const noexcept {
    if (!valid_ || !handle.Valid() || handle.slot >= attempts_.size() || bucket == nullptr)
        return nullptr;
    const auto& attempt = attempts_[handle.slot];
    const auto ordinal = attempt.nextBucket.load(std::memory_order_acquire);
    if (attempt.generation != handle.generation || !attempt.pinsInstalled ||
        !attempt.captureActive.load(std::memory_order_acquire) || ordinal >= attempt.bucketCount ||
        bucket != BucketBase(handle.slot) + ordinal ||
        bucket->attemptGeneration != handle.generation ||
        eventIndex < bucket->firstEvent || eventIndex - bucket->firstEvent >= bucket->eventCount ||
        eventIndex >= attempt.eventCount) return nullptr;
    if (!IsEventAdmitted(handle.slot, bucket, eventIndex)) return nullptr;
    const auto& pin = eventState_[static_cast<std::size_t>(handle.slot) *
                              config_.eventsPerAttempt + eventIndex].arm;
    return pin.Valid() && EventBase(handle.slot)[eventIndex].event.armToken.Valid() ? &pin : nullptr;
}

const AuthoredStopFenceDescriptor* ImmediateActionLane::CapturedNoneFence(
    ImmediateLaneHandle handle, const ImmediateLaneBucket* bucket,
    std::uint32_t eventIndex) const noexcept {
    if (!valid_ || !handle.Valid() || handle.slot >= attempts_.size() || bucket == nullptr)
        return nullptr;
    if (!IsEventAdmitted(handle.slot, bucket, eventIndex)) return nullptr;
    const auto& attempt = attempts_[handle.slot];
    const auto ordinal = attempt.nextBucket.load(std::memory_order_acquire);
    if (attempt.generation != handle.generation || !attempt.pinsInstalled ||
        !attempt.captureActive.load(std::memory_order_acquire) || ordinal >= attempt.bucketCount ||
        bucket != BucketBase(handle.slot) + ordinal || bucket->attemptGeneration != handle.generation ||
        eventIndex < bucket->firstEvent || eventIndex - bucket->firstEvent >= bucket->eventCount ||
        eventIndex >= attempt.eventCount || bucket->quantum == UINT64_MAX) return nullptr;
    const auto& reference = eventState_[static_cast<std::size_t>(handle.slot) *
                                             config_.eventsPerAttempt + eventIndex].fence;
    const auto& event = EventBase(handle.slot)[eventIndex].event;
    if (!reference.Matches(event.program) || event.program.program == nullptr ||
        event.program.program->authoredStopFence != reference.Descriptor()) return nullptr;
    reference.lastReadSequence_ = std::max(reference.lastReadSequence_, attempt.capturedAudioSequence);
    return reference.Descriptor();
}

const ImmediateLaneEvent* ImmediateActionLane::Events(
    std::uint32_t slot, const ImmediateLaneBucket* bucket) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr)
        return nullptr;
    const auto* base = BucketBase(slot);
    const auto& attempt = attempts_[slot];
    if (bucket < base || bucket >= base + attempt.bucketCount ||
        bucket->firstEvent > attempt.eventCount ||
        bucket->eventCount > attempt.eventCount - bucket->firstEvent)
        return nullptr;
    return EventBase(slot) + bucket->firstEvent;
}

const ImmediateLaneGroupRef* ImmediateActionLane::Groups(
    std::uint32_t slot, const ImmediateLaneBucket* bucket) const noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr)
        return nullptr;
    const auto* base = BucketBase(slot);
    const auto& attempt = attempts_[slot];
    if (bucket < base || bucket >= base + attempt.bucketCount ||
        bucket->firstGroup > attempt.groupCount ||
        bucket->groupCount > attempt.groupCount - bucket->firstGroup)
        return nullptr;
    return GroupBase(slot) + bucket->firstGroup;
}

bool ImmediateActionLane::FutureMember(std::uint32_t slot,
    std::uint32_t attemptGeneration,
    const Plan03GroupSliceIdentity& group, std::uint16_t memberIndex,
    TimelineArmToken arm, const ImmediateLaneEvent** event,
    std::uint16_t* eventSpanCount) const noexcept {
    if (!valid_ || slot >= attempts_.size() || event == nullptr ||
        attemptGeneration == 0 ||
        attempts_[slot].generation != attemptGeneration ||
        attempts_[slot].state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed) return false;
    const auto& attempt = attempts_[slot];
    const auto base = static_cast<std::size_t>(slot) *
        config_.eventsPerAttempt;
    for (std::uint32_t index = 0; index < attempt.groupCount; ++index) {
        if (!(groups_[base + index].identity == group)) continue;
        if (memberIndex >= groupState_[base + index].memberCount) return false;
        const auto manifestIndex = groupState_[base + index].memberBase +
            memberIndex;
        if (manifestIndex >= attempt.memberCount) return false;
        const auto& member = members_[base +
            memberState_[base + manifestIndex].order];
        if (!(member.group == group) ||
            member.memberIndex != memberIndex ||
            !SameArm(member.arm, arm) || !member.future ||
            !ValidMemberEventOrdinals(member, events_ + base,
                attempt.eventCount, config_.quantumFrames)) return false;
        const auto& future = events_[base + member.eventOrdinal];
        std::uint64_t due = 0;
        if (!future.futureGroupedStart || !SameTimeSpec(future, member) ||
            !ResolveLaneDue(future, attempt.anchorFrame, &due) ||
            future.event.executeFrame != due ||
            (future.timeDomain == ImmediateLaneTimeDomain::RelativeToAnchor &&
             future.relativeFrame == 0) ||
            !(future.event.frontier.group == group) ||
            !SameArm(future.event.armToken, arm)) return false;
        if (eventSpanCount != nullptr) {
            if (member.kind != ImmediateLaneMemberKind::CurrentEventSpan || member.eventSpanCount == 0u || member.eventSpanCount > 2u)
                return false;
            *eventSpanCount = member.eventSpanCount;
        }
        *event = &future;
        return true;
    }
    return false;
}

bool ImmediateActionLane::CaptureFrozenFutureTarget(
    ImmediateLaneHandle handle, const Plan03GroupSliceIdentity& source,
    const Plan03GroupSliceIdentity& target,
    ImmediateLaneMemberManifest* out, std::uint16_t memberIndex) const noexcept {
    const auto* attempt = Resolve(handle);
    if (attempt == nullptr || out == nullptr || !source.Exact() || !target.Exact() ||
        !attempt->pinsInstalled || !attempt->groupPinsInstalled ||
        attempt->state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed) return false;
    const auto base = static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt;
    std::uint32_t sourceIndex = UINT32_MAX;
    std::uint32_t targetIndex = UINT32_MAX;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        if (groups_[base + index].identity == source) {
            if (sourceIndex != UINT32_MAX) return false;
            sourceIndex = index;
        }
        if (groups_[base + index].identity == target) {
            if (targetIndex != UINT32_MAX) return false;
            targetIndex = index;
        }
    }
    if (sourceIndex == UINT32_MAX || targetIndex == UINT32_MAX ||
        groups_[base + sourceIndex].dueQuantum >= groups_[base + targetIndex].dueQuantum ||
        !groupPins_[base + sourceIndex].Valid() ||
        !groupPins_[base + targetIndex].Valid() ||
        !(groupPins_[base + sourceIndex].Identity() == source) ||
        !(groupPins_[base + targetIndex].Identity() == target) ||
        !groupState_[base + sourceIndex].decision.Valid() ||
        !groupState_[base + targetIndex].decision.Valid()) return false;
    const ImmediateLaneMemberManifest* declaration = nullptr;
    for (std::uint32_t index = 0; index < attempt->memberCount; ++index) {
        const auto& candidate = members_[base + index];
        if (candidate.kind != ImmediateLaneMemberKind::FrozenFutureTarget ||
            !(candidate.group == source) ||
            (memberIndex != UINT16_MAX && candidate.memberIndex != memberIndex)) continue;
        if (declaration != nullptr) return false;
        declaration = &candidate;
    }
    if (declaration == nullptr || !(declaration->relatedGroup == target) ||
        !ValidMemberEventOrdinals(*declaration, events_ + base,
            attempt->eventCount, config_.quantumFrames) ||
        declaration->memberIndex >= groupState_[base + sourceIndex].memberCount ||
        declaration->ownerInstance == 0 || declaration->ownerEpoch == 0) return false;
    const ImmediateLaneMemberManifest* historical = nullptr;
    const ImmediateLaneMemberManifest* current = nullptr;
    for (std::uint32_t index = 0; index < attempt->memberCount; ++index) {
        const auto& candidate = members_[base + index];
        if (!(candidate.group == target)) continue;
        if (candidate.kind == ImmediateLaneMemberKind::HistoricalSourceCompletion) {
            if (historical != nullptr) return false;
            historical = &candidate;
        } else if (candidate.kind == ImmediateLaneMemberKind::CurrentEventSpan &&
                   candidate.memberIndex == declaration->memberIndex) {
            if (current != nullptr) return false;
            current = &candidate;
        }
    }
    if (historical == nullptr || current == nullptr ||
        !(historical->relatedGroup == source) || historical->memberIndex != 0u ||
        historical->eventSpanCount != 1u ||
        !ValidMemberEventOrdinals(*historical, events_ + base,
            attempt->eventCount, config_.quantumFrames) ||
        !ValidMemberEventOrdinals(*current, events_ + base,
            attempt->eventCount, config_.quantumFrames) ||
        current->memberIndex != declaration->memberIndex || current->eventSpanCount != declaration->eventSpanCount ||
        current->eventOrdinal != declaration->eventOrdinal ||
        current->secondEventOrdinal != declaration->secondEventOrdinal ||
        !SameTimeSpec(*current, *declaration) ||
        current->programGeneration != declaration->programGeneration ||
        !SameArm(current->arm, declaration->arm) || !(current->target == declaration->target) ||
        current->ownerInstance != declaration->ownerInstance ||
        current->ownerEpoch != declaration->ownerEpoch) return false;
    const auto& historicalEvent = events_[base + historical->eventOrdinal];
    const auto* historicalProgram = historicalEvent.event.program.program;
    if (!eventState_[base + historical->eventOrdinal].arm.Valid() ||
        !(historicalEvent.event.frontier.group == source) ||
        !SameArm(historicalEvent.event.armToken, historical->arm) ||
        !SameTimeSpec(historicalEvent, *historical) ||
        historicalEvent.event.program.programGeneration != historical->programGeneration ||
        historicalEvent.event.ownerInstanceHandle != historical->ownerInstance ||
        historicalEvent.event.ownerInstanceScheduleEpoch != historical->ownerEpoch ||
        historicalProgram == nullptr || historicalProgram->payloads == nullptr ||
        historicalProgram->generation != historical->programGeneration ||
        historicalEvent.event.payload >= historicalProgram->payloadCount ||
        historicalProgram->payloads[historicalEvent.event.payload].kind != TimelineEventKind::Envelope ||
        !(historicalProgram->payloads[historicalEvent.event.payload].target == historical->target)) return false;
    for (std::uint32_t offset = 0; offset < declaration->eventSpanCount; ++offset) {
        const auto ordinal = offset == 0u ? declaration->eventOrdinal : declaration->secondEventOrdinal;
        const auto& event = events_[base + ordinal];
        const auto* program = event.event.program.program;
        std::uint64_t effectiveDue = 0;
        if (!eventState_[base + ordinal].arm.Valid() ||
            !(event.event.frontier.group == target) ||
            !SameArm(event.event.armToken, declaration->arm) ||
            !SameTimeSpec(event, *declaration) ||
            event.event.ownerInstanceHandle != declaration->ownerInstance ||
            event.event.ownerInstanceScheduleEpoch != declaration->ownerEpoch ||
            event.event.program.programGeneration != declaration->programGeneration ||
            program == nullptr || program->payloads == nullptr ||
            program->generation != declaration->programGeneration ||
            event.event.payload >= program->payloadCount ||
            !(program->payloads[event.event.payload].target == declaration->target) ||
            program->payloads[event.event.payload].kind !=
                (offset == 0 ? TimelineEventKind::Start : TimelineEventKind::Envelope) ||
            !ResolveLaneDue(event, attempt->anchorFrame, &effectiveDue) ||
            event.event.executeFrame != effectiveDue ||
            effectiveDue / config_.quantumFrames != groups_[base + targetIndex].dueQuantum) return false;
    }
    *out = *declaration;
    return true;
}

bool ImmediateActionLane::EntryApplied(std::uint32_t slot,
    std::uint32_t attemptGeneration) const noexcept {
    if (!valid_ || slot >= attempts_.size() ||
        attemptGeneration == 0 ||
        attempts_[slot].generation != attemptGeneration ||
        attempts_[slot].state.load(std::memory_order_acquire) !=
            ImmediateLaneAttemptState::Committed ||
        attempts_[slot].bucketCount == 0) return false;
    return BucketBase(slot)[0].state.load(std::memory_order_acquire) ==
        ImmediateLaneBucketState::Applied;
}

bool ImmediateActionLane::EntryApplied(std::uint32_t slot,
    std::uint32_t generation, const FrontierEntryReceiptRef& identity) const noexcept {
    if (!HasRegisteredEntries(slot, generation)) return EntryApplied(slot, generation);
    const auto& attempt = attempts_[slot];
    const auto entries = PrivateEntries(slot);
    for (std::uint32_t ordinal = 0; ordinal < attempt.privateEntryCount; ++ordinal) {
        const auto& entry = entries[ordinal];
        if (!(entry.entry == identity) || !entry.mappingValid) continue;
        const auto* coordinator = PrivateEntryBucketFor(entry, entry.mapping.coordinatorBucket);
        return coordinator != nullptr && coordinator->outcome.load(std::memory_order_acquire) ==
            ImmediateLaneEntryOutcome::Applied;
    }
    return false;
}

bool ImmediateActionLane::Retire(ImmediateLaneHandle handle,
    std::uint64_t completedAudioSequence) noexcept {
    return Retire(handle, completedAudioSequence, nullptr);
}

bool ImmediateActionLane::Retire(ImmediateLaneHandle handle,
    std::uint64_t completedAudioSequence,
    OriginalImmediateLaneAttemptLifetimePin* baseline) noexcept {
    auto* lifetimeAttempt = Resolve(handle);
    if (lifetimeAttempt != nullptr) {
        if (baseline != nullptr) {
            if (!MatchesOriginalAttempt(*baseline) ||
                baseline->handle_.slot != handle.slot ||
                baseline->handle_.generation != handle.generation ||
                lifetimeAttempt->originalLifetimePins != 1u) return false;
        } else if (lifetimeAttempt->originalLifetimePins != 0) return false;
    }
    auto* attempt = Resolve(handle);
    if (attempt == nullptr || attempt->privatePlacementProof != nullptr) return false;
    AttemptReadAccess access(*attempt);
    if (!access) return false;
    const auto phase = attempt->state.load(std::memory_order_acquire);
    if (phase == ImmediateLaneAttemptState::Committed ||
        phase == ImmediateLaneAttemptState::Cancelling ||
        phase == ImmediateLaneAttemptState::PrivateCancelled) {
        if (attempt->nextBucket.load(std::memory_order_acquire) !=
            attempt->bucketCount) return false;
        // Audio records the attempt terminal in O(1). Only the logic thread
        // walks the remaining descriptors after the capture gate is free.
        if (phase == ImmediateLaneAttemptState::Cancelling ||
            phase == ImmediateLaneAttemptState::PrivateCancelled)
            for (std::uint32_t index = 0; index < attempt->bucketCount; ++index) {
                auto expected = ImmediateLaneBucketState::Committed;
                (void)BucketBase(handle.slot)[index].state.compare_exchange_strong(
                    expected, ImmediateLaneBucketState::Cancelled,
                    std::memory_order_acq_rel, std::memory_order_acquire);
            }
    } else if (phase != ImmediateLaneAttemptState::Reserved &&
               phase != ImmediateLaneAttemptState::Prepared) {
        return false;
    }
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index)
        if (!Terminal(BucketBase(handle.slot)[index].state.load(
                std::memory_order_acquire)))
            return false;
    auto* eventRows = eventState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index)
        if (eventRows[index].arm.Valid() &&
            !eventRows[index].arm.Release(completedAudioSequence))
            return false;
    auto* groupPins = groupPins_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    auto* groups = GroupBase(handle.slot);
    auto* groupRows = groupState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
    if (!attempt->groupPinsInstalled) {
        const auto* groupRowsRead = groupState_.At(static_cast<std::size_t>(handle.slot) * config_.eventsPerAttempt);
        for (std::uint32_t index = 0;
             index < attempt->pinGroupManifestCount; ++index)
            if (groupRows[index].futureOwned != 0 || groupPins[index].Valid() ||
                groupRowsRead[index].decision.Valid()) return false;
    }
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index) {
        if (groupRows[index].futureOwned == 0) continue;
        if (groupPool_ == nullptr || !groupPins[index].Valid() ||
            !groupPool_->ReleaseImmediateLaneFutureDependent(
                groups[index].identity)) return false;
        groupRows[index].futureOwned = 0;
        groups[index].futureDependent = false;
    }
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index)
        if (groupPins[index].Valid() &&
            !groupPins[index].Release(completedAudioSequence))
            return false;
    for (std::uint32_t index = 0; index < attempt->groupCount; ++index)
        if (groupRows[index].decision.Valid() && !groupRows[index].decision.Release())
            return false;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index)
        if (eventRows[index].fence.Valid() && eventRows[index].fence.lastReadSequence_ != 0 &&
            completedAudioSequence <= eventRows[index].fence.lastReadSequence_) return false;
    for (std::uint32_t index = 0; index < attempt->eventCount; ++index)
        eventRows[index].fence.Reset();
    if (baseline != nullptr) ReturnOriginalAttempt(*baseline);
    // Preserve terminal entry receipts while any real reader still refuses release.
    InvalidatePrivateEntryMappings(handle);
    ReleasePrivateEntries(handle.slot);
    for (std::uint32_t index = 0; index < attempt->bucketCount; ++index)
        BucketBase(handle.slot)[index].admissionFailure.store(
            ImmediateLaneAdmissionFailure::None, std::memory_order_relaxed);
    attempt->root = 0;
    attempt->queueContinuation = nullptr;
    attempt->eventCredit = 0;
    attempt->eventCount = 0;
    attempt->groupCount = 0;
    attempt->pinGroupManifestCount = 0;
    attempt->pinGroupPool = nullptr;
    attempt->memberCount = 0;
    attempt->privateEntryCount = 0;
    attempt->bucketCount = 0;
    attempt->pinsInstalled = false;
    attempt->groupPinsInstalled = false;
    attempt->nextBucket.store(0, std::memory_order_relaxed);
    attempt->state.store(ImmediateLaneAttemptState::Free,
                         std::memory_order_release);
    return true;
}

bool ImmediateActionLane::ConditionalProjectionGroup(std::uint32_t slot,
    std::uint32_t index) const noexcept {
    const auto& row = attempts_[slot];
    if (!row.conditionalProjectionBound || index >= row.groupCount) return false;
    const auto id = GroupBase(slot)[index].identity;
    return id == row.conditionalProjectionGroups[0] || id == row.conditionalProjectionGroups[1];
}

bool ImmediateActionLane::ConditionalProjectionEvent(std::uint32_t slot,
    std::uint32_t index) const noexcept {
    const auto& row = attempts_[slot];
    if (!row.conditionalProjectionBound || index >= row.eventCount) return false;
    const auto base = static_cast<std::size_t>(slot) * config_.eventsPerAttempt;
    return eventState_[base + eventState_[base + index].withdrawalLeader].conditionalScope != 0;
}

bool ImmediateActionLane::EntireConditionalProjectionBucket(std::uint32_t slot,
    const ImmediateLaneBucket& bucket) const noexcept {
    if (bucket.eventCount == 0 && bucket.groupCount == 0) return false;
    for (std::uint32_t i = bucket.firstEvent; i < bucket.firstEvent + bucket.eventCount; ++i)
        if (!ConditionalProjectionEvent(slot, i)) return false;
    for (std::uint32_t i = bucket.firstGroup; i < bucket.firstGroup + bucket.groupCount; ++i)
        if (!ConditionalProjectionGroup(slot, i)) return false;
    return true;
}

void ImmediateActionLane::BeginConditionalProjection(std::uint32_t slot,
    const ImmediateLaneBucket& bucket, std::uint64_t actualAudioSequence) noexcept {
    auto& row = attempts_[slot];
    bool present = false;
    for (std::uint32_t i = bucket.firstGroup; i < bucket.firstGroup + bucket.groupCount; ++i)
        present = present || ConditionalProjectionGroup(slot, i);
    for (std::uint32_t i = bucket.firstEvent; i < bucket.firstEvent + bucket.eventCount; ++i)
        present = present || ConditionalProjectionEvent(slot, i);
    row.capturedConditionalProjection = false;
    if (!present) return;
    const auto before = row.conditionalProjectionAdmission.fetch_or(kEventAudioAdmitting, std::memory_order_acq_rel);
    assert((before & kEventAudioAdmitting) == 0);
    if ((before & kEventWithdrawalRequested) == 0) {
        row.capturedConditionalProjection = true;
        if (before <= UINT64_MAX - kProjectionStamp)
            row.conditionalProjectionAdmission.fetch_add(kProjectionStamp, std::memory_order_acq_rel);
        else row.conditionalProjectionAdmission.fetch_or(kProjectionWritten, std::memory_order_acq_rel);
        if ((before & kProjectionAttached) != 0) {
            assert(row.conditionalReplacement != nullptr);
            row.capturedConditionalReplacementRead = row.conditionalReplacement->BorrowOldRead(
                row.conditionalReplacementGeneration);
            if (!row.capturedConditionalReplacementRead) {
                // AcceptedNew is the shared winner even before logic has copied
                // its permanent withdrawal disposition into each exact sidecar.
                row.conditionalProjectionAdmission.fetch_or(kEventWithdrawalRequested, std::memory_order_acq_rel);
                row.conditionalProjectionAdmission.fetch_and(~std::uint64_t(kEventAudioAdmitting), std::memory_order_release);
                row.capturedConditionalProjection = false;
            }
        }
    } else row.conditionalProjectionAdmission.fetch_and(~std::uint64_t(kEventAudioAdmitting), std::memory_order_release);
    // Sequence is the actual callback, including overdue CancelledOnly disposal.
    row.conditionalProjectionLastUse.store(actualAudioSequence, std::memory_order_release);
}

void ImmediateActionLane::EndConditionalProjection(std::uint32_t slot) noexcept {
    auto& row = attempts_[slot];
    if (row.queueContinuation != nullptr) row.queueContinuation->EndAudio();
    if (row.capturedConditionalReplacementRead) {
        row.conditionalReplacement->ReturnOldRead();
        row.capturedConditionalReplacementRead = false;
    }
    if (row.capturedConditionalProjection)
        (void)row.conditionalProjectionAdmission.fetch_and(
            ~std::uint64_t(kEventAudioAdmitting), std::memory_order_release);
    row.capturedConditionalProjection = false;
}

bool ImmediateActionLane::BeforeConditionalProjectionNativeWrite(
    ImmediateLaneHandle handle, const Plan03GroupSliceIdentity& group) const noexcept {
    if (!valid_ || !handle.Valid() || handle.slot >= attempts_.size()) return false;
    auto& row = attempts_[handle.slot];
    if (row.generation != handle.generation) return false;
    if (!row.conditionalProjectionIssued ||
        (group != row.conditionalProjectionGroups[0] && group != row.conditionalProjectionGroups[1])) return true;
    if (!row.captureActive.load(std::memory_order_acquire) || !row.capturedConditionalProjection ||
        row.capturedBucketOrdinal >= row.bucketCount) return false;
    if (row.capturedConditionalReplacementRead) row.conditionalReplacement->BeforeOldWrite();
    row.conditionalProjectionAdmission.fetch_or(kProjectionWritten, std::memory_order_acq_rel);
    return true;
}

bool ImmediateActionLane::FinishDiscardedConditionalWithdrawal(std::uint32_t slot,
    const ImmediateLaneBucket* bucket) noexcept {
    if (!valid_ || slot >= attempts_.size() || bucket == nullptr) return false;
    auto& row = attempts_[slot];
    if (!row.captureActive.load(std::memory_order_acquire) ||
        row.capturedBucketOrdinal >= row.bucketCount ||
        bucket != BucketBase(slot) + row.capturedBucketOrdinal ||
        bucket->attemptGeneration != row.generation ||
        row.state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Committed ||
        row.nextBucket.load(std::memory_order_acquire) != row.capturedBucketOrdinal ||
        bucket->state.load(std::memory_order_acquire) != ImmediateLaneBucketState::Committed ||
        !EntireConditionalProjectionBucket(slot, *bucket) ||
        (row.conditionalProjectionAdmission.load(std::memory_order_acquire) & kEventWithdrawalRequested) == 0)
        return false;
    // Only the actual finalizer Discard calls this after releasing its held proofs.
    EndConditionalProjection(slot);
    const ImmediateLaneHandle handle{slot, row.generation};
    return ClaimRetainingCaptureOnRefusal(handle, bucket) && FinishCancelledOnly(slot, bucket);
}

OriginalConditionalReplacementBatch::OriginalConditionalReplacementBatch(std::uint32_t capacity)
    : members_(capacity == 0 ? nullptr : new Member[capacity]), capacity_(capacity) {}

OriginalConditionalReplacementBatch::~OriginalConditionalReplacementBatch() {
    const bool closed = Close();
    assert(closed);
    (void)closed;
}

bool OriginalConditionalReplacementBatch::Prepare(QueueInstanceTable& queues,
    QueueInstanceTable::PreparedCatalogResetScope* scope,
    LogicCommitCoordinator::PreparedIssue* sequence) noexcept {
    if (sequence == nullptr || !sequence->Valid() || !PrepareScope(queues, scope)) return false;
    sequence_ = std::move(*sequence);
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareDeferred(QueueInstanceTable& queues,
    QueueInstanceTable::PreparedCatalogResetScope* scope,
    const PreparedActionFiring& originalChild) noexcept {
    if (originalChild.InheritedSequence() == 0 || !PrepareScope(queues, scope)) return false;
    deferredChild_ = originalChild;
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareScope(QueueInstanceTable& queues,
    QueueInstanceTable::PreparedCatalogResetScope* scope) noexcept {
    if (queues_ != nullptr || count_ != 0 || generation_ == UINT64_MAX || scope == nullptr ||
        !scope->Valid() ||
        scope->MatchedCount() == 0 || scope->MatchedCount() > capacity_) return false;
    for (std::uint32_t i = 0; i < scope->MatchedCount(); ++i) {
        QueueInstanceToken token{};
        QueueInstanceSnapshot snapshot{};
        if (!queues.CaptureCatalogResetScopeQueue(*scope, i, &token, &snapshot)) return false;
    }
    queues_ = &queues;
    scope_ = std::move(*scope);
    ++generation_;
    word_.store(0, std::memory_order_release);
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareContinuationScope(QueueInstanceTable& queues) noexcept {
    if (queues_ != nullptr || count_ != 0 || generation_ == UINT64_MAX || capacity_ == 0) return false;
    queues_ = &queues;
    ++generation_;
    word_.store(0, std::memory_order_release);
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareContinuations(QueueInstanceTable& queues,
    LogicCommitCoordinator::PreparedIssue* sequence) noexcept {
    if (sequence == nullptr || !sequence->Valid() || !PrepareContinuationScope(queues)) return false;
    sequence_ = std::move(*sequence);
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareContinuations(QueueInstanceTable& queues,
    const PreparedActionFiring& child) noexcept {
    if (child.InheritedSequence() == 0 || !PrepareContinuationScope(queues)) return false;
    deferredChild_ = child;
    return true;
}

bool OriginalConditionalReplacementBatch::AttachContinuation(QueueContinuationLane& original,
    QueueContinuationLane& replacement, PreparedQueueConditionalReplacementV1& conditional) noexcept {
    if (queues_ == nullptr || Sequence() == 0 || count_ == capacity_ ||
        (word_.load(std::memory_order_acquire) & (kAborted | kAccepted)) != 0 ||
        !conditional.Valid() || conditional.owner_ != queues_ || conditional.stagedOwner_ != &original ||
        !original.Replaceable() || !replacement.prepared_ || replacement.published_ ||
        replacement.queues_ != queues_ || replacement.decision_.slot != conditional.replacement_.rootDecision.slot ||
        replacement.decision_.generation != conditional.replacement_.rootDecision.generation ||
        replacement.conditional_.version != conditional.replacement_.next.version ||
        replacement.queue_.slot != original.queue_.slot || replacement.queue_.generation != original.queue_.generation)
        return false;
    for (std::uint32_t i = 0; i < count_; ++i) {
        const auto existing = members_[i].continuation != nullptr ? members_[i].continuation->queue_
            : members_[i].original.conditional_.Token();
        if (existing.slot == original.queue_.slot && existing.generation == original.queue_.generation) return false;
    }
    OriginalImmediateLaneAttemptLifetimePin pin;
    if (!original.originalLane_.Borrow(&pin)) return false;
    std::uint64_t expected = 0;
    if (!original.admission_.compare_exchange_strong(expected, QueueContinuationLane::kChanging,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    original.replacement_ = this;
    original.replacementGeneration_ = generation_;
    expected = QueueContinuationLane::kChanging;
    if (!original.admission_.compare_exchange_strong(expected, QueueContinuationLane::kAttached,
            std::memory_order_release, std::memory_order_acquire)) {
        original.replacement_ = nullptr;
        original.replacementGeneration_ = 0;
        original.admission_.fetch_and(~QueueContinuationLane::kChanging, std::memory_order_release);
        word_.fetch_or(kAborted, std::memory_order_acq_rel);
        return false;
    }
    auto& member = members_[count_++];
    member.continuation = &original;
    member.replacementContinuation = &replacement;
    member.stagedConditional = &conditional;
    member.continuationPin = std::move(pin);
    ++continuationCount_;
    return true;
}

bool OriginalConditionalReplacementBatch::CommitContinuation(
    PreparedQueueConditionalReplacementV1& conditional, LogicCommitSequence sequence) noexcept {
    if (!Accepted() || !orderConsumed_ || sequence != acceptedSequence_ || !conditional.Valid()) return false;
    for (std::uint32_t i = 0; i < count_; ++i) {
        auto& member = members_[i];
        if (member.stagedConditional != &conditional || member.continuation == nullptr) continue;
        if (queues_->CommitConditionalReplacementInternal(&conditional, sequence, true) != QueueResetStatusV1::Ok)
            return false;
        member.stagedConditional = nullptr;
        return true;
    }
    return false;
}

bool QueueContinuationLane::BeginAudio() const noexcept {
    const auto before = admission_.fetch_or(kReading, std::memory_order_acq_rel);
    assert((before & kReading) == 0 && !audioRead_);
    audioRead_ = true;
    if ((before & kWithdrawn) != 0) return false;
    if ((before & kAttached) != 0) {
        replacementRead_ = replacement_->BorrowOldRead(replacementGeneration_);
        if (!replacementRead_) {
            admission_.fetch_or(kWithdrawn, std::memory_order_acq_rel);
            return false;
        }
        replacement_->BeforeOldWrite();
    }
    admission_.fetch_or(kWritten, std::memory_order_acq_rel);
    return true;
}

void QueueContinuationLane::EndAudio() const noexcept {
    if (!audioRead_) return;
    if (replacementRead_) replacement_->ReturnOldRead();
    replacementRead_ = audioRead_ = false;
    admission_.fetch_and(~kReading, std::memory_order_release);
}

bool OriginalConditionalReplacementBatch::Attach(
    const OriginalImmediateLaneConditionalProjection& original,
    PrivateSelectedQueueFutureSpanPool& spans,
    const PrivateSelectedQueueFutureSpanPool::Owner& selected) noexcept {
    if (queues_ == nullptr || Sequence() == 0 || !scope_.Valid() ||
        count_ == capacity_ || count_ - continuationCount_ >= scope_.MatchedCount() ||
        (word_.load(std::memory_order_acquire) & (kAborted | kAccepted)) != 0 ||
        !original.withdrawalOwner_ || !original.Valid() ||
        !queues_->MatchesOriginalConditionalFutureReservation(original.conditional_)) return false;
    const auto queue = original.conditional_.Token();
    bool matched = false;
    for (std::uint32_t i = 0; i < scope_.MatchedCount(); ++i) {
        QueueInstanceToken token{};
        QueueInstanceSnapshot snapshot{};
        if (!queues_->CaptureCatalogResetScopeQueue(scope_, i, &token, &snapshot)) return false;
        matched = matched || (token.slot == queue.slot && token.generation == queue.generation);
    }
    if (!matched) return false;
    for (std::uint32_t i = 0; i < count_; ++i) {
        const auto existing = members_[i].continuation != nullptr ? members_[i].continuation->queue_
            : members_[i].original.conditional_.Token();
        if (existing.slot == queue.slot && existing.generation == queue.generation) return false;
    }
    Member pending;
    if (!selected.Borrow(&pending.selected) ||
        !spans.IsFrozenReplacementOf(pending.selected, original.conditional_, Sequence()) ||
        !original.Borrow(&pending.original)) return false;
    auto* row = original.lane_.lane_->Resolve(original.lane_.handle_);
    if (row == nullptr || row->privatePublicationProof != nullptr) return false;
    const auto state = row->state.load(std::memory_order_acquire);
    if (state != ImmediateLaneAttemptState::Reserved && state != ImmediateLaneAttemptState::Prepared &&
        state != ImmediateLaneAttemptState::Committed) return false;
    auto before = row->conditionalProjectionAdmission.load(std::memory_order_acquire);
    if ((before & kProjectionFlags) != 0 || before > UINT64_MAX - kProjectionStamp ||
        row->conditionalReplacement != nullptr) return false;
    const auto changing = before | kProjectionChanging;
    if (!row->conditionalProjectionAdmission.compare_exchange_strong(before, changing,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    row->conditionalReplacement = this;
    row->conditionalReplacementGeneration = generation_;
    auto expected = changing;
    if (!row->conditionalProjectionAdmission.compare_exchange_strong(expected,
            (changing & ~kProjectionChanging) | kProjectionAttached,
            std::memory_order_release, std::memory_order_acquire)) {
        // Audio may win during attachment. It never reads this pointer unless
        // it acquired the Attached state, so this failed prefix remains Old.
        row->conditionalReplacement = nullptr;
        row->conditionalReplacementGeneration = 0;
        row->conditionalProjectionAdmission.fetch_and(~kProjectionChanging, std::memory_order_release);
        word_.fetch_or(kAborted, std::memory_order_acq_rel);
        return false;
    }
    auto& member = members_[count_++];
    member.original = std::move(pending.original);
    member.selected = std::move(pending.selected);
    member.spans = &spans;
    member.attempt = row;
    return true;
}

bool OriginalConditionalReplacementBatch::TryAccept(const ActionCommitBundle& action) noexcept {
    const auto originalCount = count_ - continuationCount_;
    if (queues_ == nullptr || Sequence() == 0 ||
        action.State() != BundleState::Reserved || action.StepCount() == 0 ||
        count_ == 0 || originalCount != (scope_.Valid() ? scope_.MatchedCount() : 0) ||
        (queueSelectionsPrepared_ != 0 && queueSelectionsPrepared_ != originalCount)) return false;
    for (std::uint32_t i = 0; i < count_; ++i) {
        const auto& member = members_[i];
        if (member.continuation != nullptr) {
            const auto* next = member.replacementContinuation;
            const auto* candidate = member.stagedConditional;
            if (!member.continuationPin.Valid() || !member.continuation->Replaceable() ||
                candidate == nullptr || !candidate->Valid() || candidate->stagedOwner_ != member.continuation ||
                candidate->owner_ != queues_ || next == nullptr || !next->prepared_ || next->published_ ||
                member.continuation->replacement_ != this || member.continuation->replacementGeneration_ != generation_ ||
                member.continuation->admission_.load(std::memory_order_acquire) != QueueContinuationLane::kAttached)
                return false;
            continue;
        }
        if (!member.original.Valid() || member.attempt == nullptr ||
            member.attempt->privatePublicationProof != nullptr ||
            !queues_->IsCurrentOriginalConditionalFutureReservation(member.original.conditional_) ||
            (member.attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Reserved &&
             member.attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared &&
             member.attempt->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Committed) ||
            member.attempt->conditionalReplacement != this ||
            member.attempt->conditionalReplacementGeneration != generation_ ||
            !member.spans->IsFrozenReplacementOf(member.selected, member.original.conditional_, Sequence()) ||
            (queueSelectionsPrepared_ && !member.spans->CanConsumeQueueSelection(
                member.selected, member.original.conditional_, member.order)) ||
            (member.attempt->conditionalProjectionAdmission.load(std::memory_order_acquire) &
                (kProjectionAttached | kProjectionChanging | kProjectionWritten | kEventWithdrawalRequested)) != kProjectionAttached)
            return false;
    }
    std::uint64_t expected = 0;
    if (!word_.compare_exchange_strong(expected, kAccepted,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    // The shared word already selects every attached scope. These infallible
    // copies preserve that same result after the batch's native readers retire.
    for (std::uint32_t i = 0; i < count_; ++i) {
        if (members_[i].continuation != nullptr)
            members_[i].continuation->admission_.fetch_or(QueueContinuationLane::kWithdrawn, std::memory_order_acq_rel);
        else members_[i].attempt->conditionalProjectionAdmission.fetch_or(kEventWithdrawalRequested, std::memory_order_acq_rel);
    }
    if (queueSelectionsPrepared_)
        for (std::uint32_t i = 0; i < count_; ++i)
            if (members_[i].continuation == nullptr)
                members_[i].spans->ConsumeQueueSelection(members_[i].selected, members_[i].order);
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareQueueSelections(
    std::uint64_t executeFrame, std::uint32_t executionOrdinal) noexcept {
    if (queues_ == nullptr || queueSelectionsPrepared_ || !scope_.Valid() || Sequence() == 0 ||
        count_ == 0 || count_ != scope_.MatchedCount() ||
        (word_.load(std::memory_order_acquire) & (kAborted | kAccepted)) != 0) return false;
    const QueueExecutionOrderV1 order{executeFrame == 0 ? queues_->logicNow_ : executeFrame, Sequence(), executionOrdinal};
    for (std::uint32_t i = 0; i < count_; ++i)
        if (!members_[i].spans->CanConsumeQueueSelection(
                members_[i].selected, members_[i].original.conditional_, order)) return false;
    for (std::uint32_t i = 0; i < count_; ++i) {
        members_[i].order = order;
        members_[i].orderPrepared = true;
    }
    queueSelectionsPrepared_ = count_;
    return true;
}

bool OriginalConditionalReplacementBatch::PrepareQueueSelection(
    const OriginalImmediateLaneConditionalProjection& original,
    std::uint64_t executeFrame, std::uint32_t executionOrdinal) noexcept {
    if (queues_ == nullptr || !scope_.Valid() || Sequence() == 0 || !original.Valid() ||
        count_ == 0 || count_ != scope_.MatchedCount() ||
        (word_.load(std::memory_order_acquire) & (kAborted | kAccepted)) != 0) return false;
    const auto queue = original.conditional_.Token();
    for (std::uint32_t i = 0; i < count_; ++i) {
        auto& member = members_[i];
        const auto actual = member.original.conditional_.Token();
        if (queue.slot != actual.slot || queue.generation != actual.generation) continue;
        const QueueExecutionOrderV1 order{executeFrame == 0 ? queues_->logicNow_ : executeFrame, Sequence(), executionOrdinal};
        if (member.orderPrepared || original.pool_ != member.original.pool_ ||
            original.lane_.lane_ != member.original.lane_.lane_ ||
            original.lane_.handle_.slot != member.original.lane_.handle_.slot ||
            original.lane_.handle_.generation != member.original.lane_.handle_.generation ||
            !member.spans->CanConsumeQueueSelection(member.selected, original.conditional_, order)) return false;
        member.order = order;
        member.orderPrepared = true;
        ++queueSelectionsPrepared_;
        return true;
    }
    return false;
}

bool OriginalConditionalReplacementBatch::Accepted() const noexcept {
    return queues_ != nullptr && (word_.load(std::memory_order_acquire) & kAccepted) != 0;
}

LogicCommitSequence OriginalConditionalReplacementBatch::ConsumeAcceptedOrder(
    LogicCommitCoordinator::ConsumedIssue* consumed) noexcept {
    assert(Accepted() && !orderConsumed_ && Sequence() != 0);
    if (!Accepted() || orderConsumed_ || (consumed != nullptr && consumed->Sequence() != 0)) return 0;
    const auto order = Sequence();
    // Consume through the actual held issuer, never a caller-supplied receiver.
    // Deferred Actions keep their genuine prepared child's inherited order.
    if (sequence_.Valid()) {
        auto receipt = sequence_.owner_->ConsumePreparedIssue(std::move(sequence_));
        if (consumed != nullptr) *consumed = std::move(receipt);
    }
    orderConsumed_ = true;
    acceptedSequence_ = order;
    return order;
}

bool OriginalConditionalReplacementBatch::BorrowOldRead(std::uint64_t generation) noexcept {
    assert(generation == generation_ && queues_ != nullptr);
    if (generation != generation_ || queues_ == nullptr) return false;
    // There is at most one actual audio capture per attached Lane. The bounded
    // attachment capacity is uint32, far below this word's 62-bit reader range.
    const auto previous = word_.fetch_add(kRead, std::memory_order_acq_rel);
    if ((previous & kAccepted) == 0) return true;
    word_.fetch_sub(kRead, std::memory_order_release);
    return false;
}

void OriginalConditionalReplacementBatch::BeforeOldWrite() noexcept {
    const auto previous = word_.fetch_or(kAborted, std::memory_order_acq_rel);
    assert((previous & kAccepted) == 0 && previous >= kRead);
    (void)previous;
}

void OriginalConditionalReplacementBatch::ReturnOldRead() noexcept {
    const auto previous = word_.fetch_sub(kRead, std::memory_order_release);
    assert(previous >= kRead);
    (void)previous;
}

bool OriginalConditionalReplacementBatch::Close() noexcept {
    if (queues_ == nullptr) return true;
    for (std::uint32_t i = 0; i < count_; ++i)
        if (members_[i].attempt != nullptr &&
            members_[i].attempt->privatePublicationProof != nullptr) return false;
    if (!Accepted()) word_.fetch_or(kAborted, std::memory_order_acq_rel);
    bool closed = true;
    for (std::uint32_t i = 0; i < count_; ++i) {
        auto& member = members_[i];
        if (member.continuation != nullptr) {
            auto& old = *member.continuation;
            auto before = old.admission_.load(std::memory_order_acquire);
            if ((before & QueueContinuationLane::kReading) != 0 ||
                !old.admission_.compare_exchange_strong(before,
                    (before & ~QueueContinuationLane::kAttached) | QueueContinuationLane::kChanging,
                    std::memory_order_acq_rel, std::memory_order_acquire)) { closed = false; continue; }
            old.replacement_ = nullptr;
            old.replacementGeneration_ = 0;
            old.admission_.fetch_and(~QueueContinuationLane::kChanging, std::memory_order_release);
            member.continuationPin.Return();
            member.continuation = member.replacementContinuation = nullptr;
            member.stagedConditional = nullptr;
            continue;
        }
        if (member.attempt == nullptr) continue;
        auto& row = *member.attempt;
        auto before = row.conditionalProjectionAdmission.load(std::memory_order_acquire);
        if ((before & kEventAudioAdmitting) != 0) { closed = false; continue; }
        if (!row.conditionalProjectionAdmission.compare_exchange_strong(before,
                (before & ~kProjectionAttached) | kProjectionChanging,
                std::memory_order_acq_rel, std::memory_order_acquire)) { closed = false; continue; }
        row.conditionalReplacement = nullptr;
        row.conditionalReplacementGeneration = 0;
        row.conditionalProjectionAdmission.fetch_and(~kProjectionChanging, std::memory_order_release);
        member.original.Return();
        member.selected.Release();
        member.spans = nullptr;
        member.attempt = nullptr;
        member.order = {};
        member.orderPrepared = false;
    }
    if (!closed) return false;
    assert((word_.load(std::memory_order_acquire) & ~(kAborted | kAccepted)) == 0);
    sequence_.Release();
    deferredChild_ = {};
    scope_.Release();
    queues_ = nullptr;
    count_ = 0;
    continuationCount_ = 0;
    queueSelectionsPrepared_ = 0;
    orderConsumed_ = false;
    acceptedSequence_ = 0;
    return true;
}

OriginalImmediateLaneConditionalProjection::OriginalImmediateLaneConditionalProjection(
    OriginalImmediateLaneConditionalProjection&& other) noexcept { *this = std::move(other); }

OriginalImmediateLaneConditionalProjection& OriginalImmediateLaneConditionalProjection::operator=(
    OriginalImmediateLaneConditionalProjection&& other) noexcept {
    if (this == &other) return *this;
    Return();
    lane_ = std::move(other.lane_);
    conditional_ = std::move(other.conditional_);
    for (std::uint32_t i = 0; i < groups_.size(); ++i) groups_[i] = std::move(other.groups_[i]);
    pool_ = std::exchange(other.pool_, nullptr);
    entry_ = std::exchange(other.entry_, FrontierEntryReceiptRef{});
    withdrawalOwner_ = std::exchange(other.withdrawalOwner_, false);
    return *this;
}

OriginalImmediateLaneConditionalProjection::~OriginalImmediateLaneConditionalProjection() { Return(); }

bool OriginalImmediateLaneConditionalProjection::Valid() const noexcept {
    return lane_.Valid() && conditional_.Valid() && pool_ != nullptr &&
        groups_[0].Valid() && groups_[1].Valid() &&
        lane_.lane_->attempts_[lane_.handle_.slot].conditionalProjectionIssued;
}

void OriginalImmediateLaneConditionalProjection::Return() noexcept {
    // An unbound preparation cannot be published. Its unique producer may undo
    // the reservation; structural borrowers cannot clear another owner's scope.
    if (withdrawalOwner_ && Valid()) {
        auto& row = lane_.lane_->attempts_[lane_.handle_.slot];
        if (row.conditionalReplacement != nullptr) (void)row.conditionalReplacement->Close();
        if (!row.conditionalProjectionBound) {
            assert(row.conditionalReplacement == nullptr);
            const auto state = row.state.load(std::memory_order_acquire);
            assert(state != ImmediateLaneAttemptState::Committed);
            (void)state;
            row.conditionalProjectionIssued = false;
            row.conditionalProjectionGroups = {};
            row.conditionalProjectionAdmission.store(0, std::memory_order_release);
        }
    }
    // Bound immutable declarations outlive structural loans and native captures.
    pool_ = nullptr;
    entry_ = {};
    withdrawalOwner_ = false;
    for (auto& group : groups_) group.Release();
    conditional_.Release();
    lane_.Return();
}

bool OriginalImmediateLaneConditionalProjection::Borrow(
    OriginalImmediateLaneConditionalProjection* out) const noexcept {
    if (!Valid() || out == nullptr || out->lane_.lane_ != nullptr) return false;
    OriginalImmediateLaneConditionalProjection result;
    if (!lane_.Borrow(&result.lane_) || !conditional_.Borrow(&result.conditional_) ||
        !groups_[0].Borrow(&result.groups_[0]) || !groups_[1].Borrow(&result.groups_[1])) return false;
    result.pool_ = pool_; result.entry_ = entry_;
    *out = std::move(result);
    return true;
}

bool OriginalImmediateLaneConditionalProjection::PrepareBound(QueueInstanceTable& queues,
    const QueueInstanceTable::OriginalLifetimePin& parent,
    const QueueInstanceTable::OriginalConditionalFutureReservation& conditional,
    const QueueInstanceTable::OriginalConditionalLaneClock& clock,
    ScheduledTransactionPool& pool, const OriginalQueueFutureGroupRegistration& primary,
    const OriginalQueueFutureGroupRegistration& member,
    const OriginalImmediateLaneAttemptLifetimePin& lane,
    OriginalImmediateLaneConditionalProjection* out) noexcept {
    return PrepareOriginal(queues, parent, conditional, clock, pool, primary,
        member, lane, true, out);
}

bool OriginalImmediateLaneConditionalProjection::PrepareUnbound(QueueInstanceTable& queues,
    const QueueInstanceTable::OriginalLifetimePin& parent,
    const QueueInstanceTable::OriginalConditionalFutureReservation& conditional,
    const QueueInstanceTable::OriginalConditionalLaneClock& clock,
    ScheduledTransactionPool& pool, const OriginalQueueFutureGroupRegistration& primary,
    const OriginalQueueFutureGroupRegistration& member,
    const OriginalImmediateLaneAttemptLifetimePin& lane,
    OriginalImmediateLaneConditionalProjection* out) noexcept {
    return PrepareOriginal(queues, parent, conditional, clock, pool, primary,
        member, lane, false, out);
}

bool OriginalImmediateLaneConditionalProjection::PrepareOriginal(QueueInstanceTable& queues,
    const QueueInstanceTable::OriginalLifetimePin& parent,
    const QueueInstanceTable::OriginalConditionalFutureReservation& conditional,
    const QueueInstanceTable::OriginalConditionalLaneClock& clock,
    ScheduledTransactionPool& pool, const OriginalQueueFutureGroupRegistration& primary,
    const OriginalQueueFutureGroupRegistration& member,
    const OriginalImmediateLaneAttemptLifetimePin& lane, bool bound,
    OriginalImmediateLaneConditionalProjection* out) noexcept {
    if (out == nullptr || out->lane_.lane_ != nullptr || !lane.Valid() ||
        !queues.IsOriginalConditionalLaneClockFor(clock, conditional, parent) ||
        !queues.MatchesOriginalConditionalFutureReservation(conditional) ||
        !pool.IsOriginalQueueFutureGroupFamily(primary, member) ||
        primary.selectedFutureDomain_ != 0 || primary.memberCount_ != 0 ||
        primary.sourceControlCount_ != 0 || member.memberCount_ > 1 ||
        member.sourceControlCount_ != 0 || primary.cohortRoot_ != lane.root_ ||
        primary.executeFrame_ != member.executeFrame_) return false;
    const auto& original = queues.originalConditionalRows_[conditional.row_];
    if (original.clockPool != &pool || original.clockGroups[1].Identity() != member.Identity() ||
        (original.state != QueueInstanceTable::OriginalConditionalState::Committed &&
         (bound || original.state != QueueInstanceTable::OriginalConditionalState::Prepared)) ||
        original.consumesReset ||
        original.clockState != (bound
            ? QueueInstanceTable::OriginalConditionalClockState::Bound
            : QueueInstanceTable::OriginalConditionalClockState::Unbound)) return false;
    auto* row = lane.lane_->Resolve(lane.handle_);
    if (row == nullptr || row->privateEntryCount != 0 || row->conditionalProjectionIssued ||
        (row->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Reserved &&
         row->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared)) return false;
    AttemptReadAccess access(*row);
    if (!access) return false;
    // The unplaced association retains actual same-factory groups. A matching
    // cohort number alone cannot associate a second Reserved Lane with them.
    const bool unplaced = !bound &&
        row->state.load(std::memory_order_acquire) == ImmediateLaneAttemptState::Reserved;
    if (unplaced) {
        for (const auto* group : {&primary, &member}) {
            const auto identity = group->Identity();
            if (!identity.Exact() || identity.committedRow >= pool.immediateLaneGroups_.ConstructedCount())
                return false;
            const auto& native = pool.immediateLaneGroups_[identity.committedRow];
            if (native.laneOwner != lane.lane_ || native.laneSlot != lane.handle_.slot ||
                native.laneGeneration != lane.handle_.generation || native.cohortRoot != lane.root_ ||
                native.selectedFutureDomain != 0) return false;
        }
    }
    OriginalImmediateLaneConditionalProjection result;
    if (!lane.Borrow(&result.lane_) || !conditional.Borrow(&result.conditional_) ||
        !primary.BorrowLifetime(&result.groups_[0]) || !member.BorrowLifetime(&result.groups_[1])) return false;
    result.pool_ = &pool;
    row->conditionalProjectionGroups = {primary.Identity(), member.Identity()};
    row->conditionalProjectionIssued = true;
    // The reservation blocks publication until the full native declaration binds.
    access.Detach();
    row->readerGate.clear(std::memory_order_release);
    if (!unplaced && !result.BindDeclared(bound)) {
        row->conditionalProjectionIssued = false;
        row->conditionalProjectionGroups = {};
        return false;
    }
    result.withdrawalOwner_ = true;
    *out = std::move(result);
    return true;
}

bool OriginalImmediateLaneConditionalProjection::BindPrepared() noexcept {
    if (!withdrawalOwner_ || !Valid()) return false;
    const auto& original = conditional_.owner_->originalConditionalRows_[conditional_.row_];
    if (original.state != QueueInstanceTable::OriginalConditionalState::Committed ||
        original.clockState != QueueInstanceTable::OriginalConditionalClockState::Bound)
        return false;
    return BindDeclared();
}

bool OriginalImmediateLaneConditionalProjection::OwnsUnpublishedGroup(
    const ImmediateActionLane& lane, ImmediateLaneHandle handle,
    const Plan03GroupSliceIdentity& group) const noexcept {
    if (!withdrawalOwner_ || !Valid() || lane_.lane_ != &lane ||
        lane_.handle_.slot != handle.slot || lane_.handle_.generation != handle.generation ||
        !lane.OwnsPrivate(handle, lane_.root_) || !group.Exact() ||
        group.committedRow >= pool_->immediateLaneGroups_.ConstructedCount() ||
        (groups_[0].Identity() != group && groups_[1].Identity() != group)) return false;
    const auto& native = pool_->immediateLaneGroups_[group.committedRow];
    return native.laneOwner == &lane && native.laneSlot == handle.slot &&
        native.laneGeneration == handle.generation && native.cohortRoot == lane_.root_ &&
        native.selectedFutureDomain == 0 && native.generation == group.rowClaimGeneration;
}

bool OriginalImmediateLaneConditionalProjection::ReturnAfterApplied(
    const ImmediateActionLane& lane, ImmediateLaneHandle handle,
    const DecisionTable& decisions, std::uint64_t completedAudioSequence) noexcept {
    if (!withdrawalOwner_ || !Valid() || lane_.lane_ != &lane ||
        lane_.handle_.slot != handle.slot || lane_.handle_.generation != handle.generation)
        return false;
    const auto& row = lane.attempts_[handle.slot];
    if (!row.conditionalProjectionBound || row.conditionalProjectionDecisions != &decisions ||
        !entry_.Valid() || row.conditionalProjectionEntry != entry_) return false;
    const auto last = row.conditionalProjectionLastUse.load(std::memory_order_acquire);
    if (last == 0 || completedAudioSequence <= last ||
        (row.conditionalProjectionAdmission.load(std::memory_order_acquire) & kEventAudioAdmitting) != 0)
        return false;
    const auto* activation = decisions.ResolveFrontierActivation(entry_.coordinator);
    FrontierActivationReceipt receipt{};
    if (activation == nullptr || activation->Descriptor().entry != entry_ ||
        activation->Read(entry_, completedAudioSequence, &receipt) != FrontierReceiptRead::Ready ||
        receipt.outcome != FrontierReceiptOutcome::Applied) return false;
    Return();
    return true;
}

bool OriginalImmediateLaneConditionalProjection::BindDeclared(bool commit) noexcept {
    if (!Valid()) return false;
    auto& lane = *lane_.lane_;
    auto* row = lane.Resolve(lane_.handle_);
    if (row == nullptr || row->conditionalProjectionBound || !row->pinsInstalled ||
        !row->groupPinsInstalled || row->pinGroupPool != pool_ || row->privateEntryCount != 0 ||
        row->state.load(std::memory_order_acquire) != ImmediateLaneAttemptState::Prepared) return false;
    AttemptReadAccess access(*row);
    if (!access) return false;
    Plan03PageGroupView primary{}, member{};
    for (const auto& original : groups_) {
        const auto identity = original.Identity();
        if (!identity.Exact() || identity.committedRow >= pool_->immediateLaneGroups_.ConstructedCount()) return false;
        const auto& native = pool_->immediateLaneGroups_[identity.committedRow];
        if (native.laneOwner != &lane || native.laneSlot != lane_.handle_.slot ||
            native.laneGeneration != lane_.handle_.generation || native.cohortRoot != lane_.root_ ||
            native.selectedFutureDomain != 0) return false;
    }
    if (!pool_->ResolvePlan03GroupSliceProducer(groups_[0].Identity(), &primary) ||
        !pool_->ResolvePlan03GroupSliceProducer(groups_[1].Identity(), &member) ||
        primary.frontier == nullptr || member.frontier == nullptr ||
        primary.frontier->role != FrontierGroupRole::Coordinator ||
        member.frontier->role != FrontierGroupRole::EntryMember ||
        primary.frontier->primary != primary.identity || member.frontier->primary != primary.identity ||
        primary.frontier->operation.group != primary.identity ||
        member.frontier->operation.group != member.identity ||
        primary.frontier->operation.kind != FrontierOperationKind::Entry ||
        member.frontier->operation.kind != FrontierOperationKind::Dependent ||
        !(primary.frontier->entry == member.frontier->entry) ||
        primary.frontier->queueConditional.Valid() || !member.frontier->queueConditional.Valid()) return false;
    const auto& original = conditional_.owner_->originalConditionalRows_[conditional_.row_];
    const auto& q = member.frontier->queueConditional;
    if (q.slot != original.queue.slot || q.generation != original.queue.generation ||
        q.currentVersion != original.currentState.version || q.pendingVersion != original.version.version ||
        !SameOccurrence(q.current, original.current) || !SameOccurrence(q.successor, original.successor) ||
        member.identity.decision.slot != original.decision.slot ||
        member.identity.decision.generation != original.decision.generation) return false;
    Plan03PageGroupView current{};
    if (!pool_->ResolvePlan03GroupSliceProducer(original.clockGroups[0].Identity(), &current) ||
        current.frontier == nullptr || current.frontier->role != FrontierGroupRole::EntryMember ||
        !current.frontier->queueInitial.Valid() ||
        current.frontier->queueInitial.slot != original.queue.slot ||
        current.frontier->queueInitial.generation != original.queue.generation ||
        current.frontier->primary == primary.identity || current.lease == nullptr ||
        primary.lease == nullptr || primary.lease != member.lease || primary.lease == current.lease ||
        !(primary.frontier->prerequisite == current.frontier->entry) ||
        primary.frontier->prerequisiteMember != current.identity) return false;
    std::uint32_t groupCount = 0, controllerCount = 0, startCount = 0;
    const auto offset = static_cast<std::size_t>(lane_.handle_.slot) * lane.config_.eventsPerAttempt;
    const auto* groups = lane.GroupBase(lane_.handle_.slot);
    for (std::uint32_t i = 0; i < row->groupCount; ++i) {
        if (groups[i].identity == primary.identity || groups[i].identity == member.identity) {
            if (groups[i].dueQuantum != primary.frontier->operation.order.executeFrame / lane.config_.quantumFrames)
                return false;
            ++groupCount;
        }
    }
    if (groupCount != 2 || row->conditionalProjectionDecisions == nullptr) return false;
    for (std::uint32_t i = 0; i < row->groupCount; ++i) {
        if (groups[i].identity == primary.identity || groups[i].identity == member.identity) continue;
        Plan03PageGroupView sibling{};
        if (!pool_->ResolvePlan03GroupSliceProducer(groups[i].identity, &sibling) ||
            sibling.lease == primary.lease) return false;
    }
    TimelineTransactionProgramRef controller{};
    if (!row->conditionalProjectionDecisions->CaptureFrontierControlProgram(
            primary.frontier->entry.coordinator, &controller)) return false;
    const auto* events = lane.EventBase(lane_.handle_.slot);
    for (std::uint32_t i = 0; i < row->eventCount; ++i) {
        const auto& e = events[i];
        if (e.event.frontier.group != primary.identity && e.event.frontier.group != member.identity) continue;
        if (!(e.event.frontier.entry == primary.frontier->entry) ||
            !(e.event.frontier.owner == primary.frontier->operation.owner) ||
            e.event.logicCommitSequence != primary.frontier->operation.order.logicCommitSequence ||
            e.event.program.program == nullptr ||
            e.event.program.program->payloads == nullptr ||
            e.event.program.program->authoredStopFence != nullptr ||
            e.event.program.programGeneration != e.event.program.program->generation ||
            e.event.payload >= e.event.program.program->payloadCount) return false;
        if (lane.eventState_[offset + i].fence.Valid()) return false;
        const auto& payload = e.event.program.program->payloads[e.event.payload];
        if (e.event.executeFrame != primary.frontier->operation.order.executeFrame) return false;
        if (e.event.frontier.group == primary.identity &&
            e.event.program.program == controller.program &&
            e.event.program.programGeneration == controller.programGeneration &&
            e.event.program.program->role == TimelineProgramRole::ControllerActivation &&
            payload.kind == TimelineEventKind::ControllerActivation && !e.event.armToken.Valid() &&
            !e.futureGroupedStart) ++controllerCount;
        else if (e.event.frontier.group == member.identity &&
            payload.kind == TimelineEventKind::Start && e.futureGroupedStart &&
            e.event.armToken.Valid() && member.memberCount == 1 && member.members != nullptr &&
            SameArm(e.event.armToken, member.members[0])) ++startCount;
        else return false;
    }
    if (controllerCount != 1 || startCount != member.memberCount) return false;
    // No writes above: malformed full manifests leave the original owner intact.
    if (!commit) return true;
    for (std::uint32_t i = 0; i < row->eventCount; ++i)
        if (events[i].event.frontier.group == primary.identity ||
            events[i].event.frontier.group == member.identity)
            lane.eventState_[offset + lane.eventState_[offset + i].withdrawalLeader].conditionalScope = 1;
    row->conditionalProjectionBound = true;
    entry_ = primary.frontier->entry;
    row->conditionalProjectionEntry = entry_;
    return true;
}

bool OriginalImmediateLaneConditionalProjection::CapturedWithdrawalExcludesPair(
    const ImmediateActionLane& lane, ImmediateLaneHandle handle,
    const ImmediateLaneBucket* bucket, const ScheduledTransactionPool& pool,
    const DecisionTable& decisions, const ImmediateLaneGroupRef& primary,
    const ImmediateLaneGroupRef& member) noexcept {
    if (!lane.valid_ || !handle.Valid() || handle.slot >= lane.attempts_.size() ||
        bucket == nullptr) return false;
    const auto& row = lane.attempts_[handle.slot];
    if (row.generation != handle.generation || row.root == 0 ||
        !row.captureActive.load(std::memory_order_acquire) ||
        row.capturedBucketOrdinal >= row.bucketCount ||
        bucket != lane.BucketBase(handle.slot) + row.capturedBucketOrdinal ||
        bucket->attemptGeneration != handle.generation ||
        !row.conditionalProjectionIssued || !row.conditionalProjectionBound ||
        row.capturedConditionalProjection || row.pinGroupPool != &pool ||
        row.conditionalProjectionDecisions != &decisions ||
        !row.conditionalProjectionEntry.Valid() ||
        primary.identity != row.conditionalProjectionGroups[0] ||
        member.identity != row.conditionalProjectionGroups[1]) return false;
    const auto admission = row.conditionalProjectionAdmission.load(std::memory_order_acquire);
    if ((admission & kEventWithdrawalRequested) == 0 ||
        (admission & kEventAudioAdmitting) != 0) return false;
    const auto originalDeclaration = [&](const ImmediateLaneGroupRef& declaration) noexcept {
        ImmediateLaneGroupRef actual{};
        if (declaration.laneSlot != handle.slot ||
            declaration.laneGeneration != handle.generation ||
            declaration.bucketOrdinal != row.capturedBucketOrdinal ||
            declaration.dueQuantum != bucket->quantum ||
            !lane.CapturePublishedGroupDescriptor(handle, declaration.groupIndex, &actual) ||
            actual.identity != declaration.identity ||
            actual.bucketOrdinal != declaration.bucketOrdinal ||
            actual.dueQuantum != declaration.dueQuantum ||
            declaration.identity.committedRow >= pool.config_.maxImmediateLaneGroupRows)
            return false;
        const auto& native = pool.immediateLaneGroups_[declaration.identity.committedRow];
        return native.cohortRoot == row.root && native.selectedFutureDomain == 0 &&
            native.laneOwner == &lane && native.laneSlot == handle.slot &&
            native.laneGeneration == handle.generation &&
            native.groupIndex == declaration.groupIndex &&
            native.bucketOrdinal == declaration.bucketOrdinal &&
            !lane.IsGroupAdmitted(handle.slot, bucket, declaration.groupIndex);
    };
    if (!originalDeclaration(primary) || !originalDeclaration(member)) return false;
    Plan03PageGroupView primaryView{}, memberView{};
    if (!pool.ResolvePlan03GroupSliceRt(primary.identity, &primaryView) ||
        !pool.ResolvePlan03GroupSliceRt(member.identity, &memberView) ||
        primaryView.frontier == nullptr || memberView.frontier == nullptr ||
        primaryView.lease == nullptr || primaryView.lease != memberView.lease)
        return false;
    const auto& control = *primaryView.frontier;
    const auto& conditional = *memberView.frontier;
    if (control.role != FrontierGroupRole::Coordinator ||
        conditional.role != FrontierGroupRole::EntryMember ||
        control.entry != row.conditionalProjectionEntry || conditional.entry != control.entry ||
        control.primary != primary.identity || conditional.primary != primary.identity ||
        control.operation.group != primary.identity || conditional.operation.group != member.identity ||
        control.operation.kind != FrontierOperationKind::Entry ||
        conditional.operation.kind != FrontierOperationKind::Dependent ||
        control.operation.owner != conditional.operation.owner ||
        !conditional.queueConditional.Valid() ||
        primaryView.memberCount != 0 || primaryView.sourceControlCount != 0 ||
        memberView.memberCount > 1 || memberView.sourceControlCount != 0)
        return false;
    const auto* activation = decisions.ResolveFrontierActivation(control.entry.coordinator);
    if (activation == nullptr || activation->Descriptor().entry != control.entry ||
        activation->Descriptor().primary != primary.identity ||
        activation->Descriptor().expectedMemberCount != 1u ||
        activation->Descriptor().expectedEntryEventCount != 1u)
        return false;
    std::uint32_t controllers = 0, starts = 0;
    const auto* events = lane.EventBase(handle.slot);
    for (std::uint32_t i = bucket->firstEvent; i < bucket->firstEvent + bucket->eventCount; ++i) {
        const auto& event = events[i];
        if (event.event.frontier.group != primary.identity &&
            event.event.frontier.group != member.identity) continue;
        if (!lane.ConditionalProjectionEvent(handle.slot, i) ||
            lane.IsEventAdmitted(handle.slot, bucket, i) ||
            event.event.frontier.entry != control.entry ||
            event.event.frontier.owner != control.operation.owner ||
            event.event.executeFrame != control.operation.order.executeFrame ||
            event.event.program.program == nullptr ||
            event.event.program.program->payloads == nullptr ||
            event.event.payload >= event.event.program.program->payloadCount ||
            event.event.program.programGeneration != event.event.program.program->generation)
            return false;
        const auto& payload = event.event.program.program->payloads[event.event.payload];
        if (event.event.frontier.group == primary.identity &&
            event.event.program.program->role == TimelineProgramRole::ControllerActivation &&
            event.event.program.programGeneration == activation->Descriptor().programGeneration &&
            payload.kind == TimelineEventKind::ControllerActivation &&
            !event.event.armToken.Valid() && !event.futureGroupedStart) ++controllers;
        else if (event.event.frontier.group == member.identity &&
            payload.kind == TimelineEventKind::Start && event.futureGroupedStart &&
            event.event.armToken.Valid() && memberView.memberCount == 1u &&
            memberView.members != nullptr && SameArm(event.event.armToken, memberView.members[0]))
            ++starts;
        else return false;
    }
    return controllers == 1u && starts == memberView.memberCount;
}

bool OriginalImmediateLaneConditionalProjection::RequestWithdrawal() noexcept {
    if (!withdrawalOwner_ || !Valid()) return false;
    auto& row = lane_.lane_->attempts_[lane_.handle_.slot];
    if (row.privatePublicationProof != nullptr) return false;
    (void)row.conditionalProjectionAdmission.fetch_or(kEventWithdrawalRequested, std::memory_order_acq_rel);
    return true;
}

bool OriginalImmediateLaneConditionalProjection::TryCompleteNativeWithdrawal(
    DecisionTable& decisions, std::uint64_t completedAudioSequence) noexcept {
    if (!withdrawalOwner_ || !Valid()) return false;
    auto& row = lane_.lane_->attempts_[lane_.handle_.slot];
    if (row.privatePublicationProof != nullptr) return false;
    if (row.conditionalProjectionDecisions != &decisions || !row.conditionalProjectionEntry.Valid()) return false;
    const auto entry = row.conditionalProjectionEntry;
    const auto admission = row.conditionalProjectionAdmission.load(std::memory_order_acquire);
    if ((admission & kEventWithdrawalRequested) == 0 || (admission & kEventAudioAdmitting) != 0) return false;
    const auto last = row.conditionalProjectionLastUse.load(std::memory_order_acquire);
    if (last != 0 && completedAudioSequence <= last) return false;
    if (row.conditionalProjectionCancelled) return true;
    auto* activation = decisions.ResolveFrontierActivation(entry.coordinator);
    if (activation == nullptr || !(activation->Descriptor().entry == entry)) return false;
    if (!row.conditionalProjectionCancelIssued) {
        const auto result = activation->Cancel(entry, AudioSliceInvalidateReason::Cancel);
        if (result != FrontierCancelDisposition::CancelledBeforeAudio &&
            result != FrontierCancelDisposition::AlreadyCancelled) return false;
        // Preserve a real successful prefix if Reconcile later returns Busy.
        row.conditionalProjectionCancelIssued = true;
    }
    FrontierActivationReceipt receipt{};
    if (!decisions.ReconcileFrontierActivation(entry.coordinator, entry, completedAudioSequence, &receipt) ||
        receipt.outcome != FrontierReceiptOutcome::DefinitivelyNotApplied ||
        receipt.reason != FrontierReceiptReason::Cancelled ||
        receipt.origin != FrontierReceiptOrigin::LogicBeforeAudio) return false;
    // This records only the real cancelled receipt and completed native pin returns.
    row.conditionalProjectionCancelled = true;
    return true;
}

bool OriginalImmediateLaneConditionalProjection::ReturnNativeReferences(
    std::uint64_t completedAudioSequence) noexcept {
    if (!Valid()) return false;
    auto& lane = *lane_.lane_;
    auto& row = lane.attempts_[lane_.handle_.slot];
    if (row.privatePublicationProof != nullptr) return false;
    if (!row.conditionalProjectionCancelled) return false;
    if (row.conditionalProjectionReferencesReturned) return true;
    const auto admission = row.conditionalProjectionAdmission.load(std::memory_order_acquire);
    const auto last = row.conditionalProjectionLastUse.load(std::memory_order_acquire);
    if ((admission & kEventWithdrawalRequested) == 0 || (admission & kEventAudioAdmitting) != 0 ||
        (last != 0 && completedAudioSequence <= last)) return false;
    const auto offset = static_cast<std::size_t>(lane_.handle_.slot) * lane.config_.eventsPerAttempt;
    // Request permanently denies only this scope. No Lane reader gate is taken:
    // same-bucket C0 and other admitted events continue while native returns retry.
    for (std::uint32_t i = 0; i < row.eventCount; ++i) {
        if (!lane.ConditionalProjectionEvent(lane_.handle_.slot, i)) continue;
        auto& pin = lane.eventState_[offset + i].arm;
        if (pin.Valid() && !pin.Release(completedAudioSequence)) return false;
    }
    const auto* groups = lane.GroupBase(lane_.handle_.slot);
    for (std::uint32_t i = 0; i < row.groupCount; ++i) {
        if (!lane.ConditionalProjectionGroup(lane_.handle_.slot, i)) continue;
        auto& futureOwned = lane.groupState_[offset + i].futureOwned;
        if (futureOwned != 0) {
            if (lane.groupPool_ != pool_ || !lane.groupPins_[offset + i].Valid() ||
                !pool_->ReleaseImmediateLaneFutureDependent(groups[i].identity)) return false;
            futureOwned = 0;
        }
        auto& group = lane.groupPins_[offset + i];
        if (group.Valid() && !group.Release(completedAudioSequence)) return false;
        auto& decision = lane.groupState_[offset + i].decision;
        if (decision.Valid() && !decision.Release()) return false;
    }
    // Immutable descriptors remain until native Lane retirement; only actual
    // move-only own pins have been consumed. Other readers and originals remain.
    row.conditionalProjectionReferencesReturned = true;
    return true;
}

bool OriginalImmediateLaneConditionalProjection::TryRetireNativeConditional(
    DecisionTable& decisions, std::uint64_t completedAudioSequence) noexcept {
    if (!TryCompleteNativeWithdrawal(decisions, completedAudioSequence) ||
        !ReturnNativeReferences(completedAudioSequence)) return false;
    auto& queues = *conditional_.owner_;
    if (!queues.MatchesOriginalConditionalFutureReservation(conditional_)) return false;
    auto& original = queues.originalConditionalRows_[conditional_.row_];
    if (original.state == QueueInstanceTable::OriginalConditionalState::Retired) return true;
    // Reset consumption is a separate selected-future participant, never inferred here.
    if (original.state != QueueInstanceTable::OriginalConditionalState::Committed ||
        original.consumesReset) return false;
    auto* row = queues.Resolve(original.queue);
    if (row == nullptr || row->originalConditionalRow != conditional_.row_ ||
        row->originalConditionalMarker != conditional_.marker_ ||
        row->world != original.world || row->rootInstance != original.rootInstance ||
        row->programSemanticFingerprint != original.programSemanticFingerprint ||
        row->pendingVersion.load(std::memory_order_acquire) != original.version.version ||
        row->pendingNext.version != original.next.version ||
        row->pendingDecision.slot != original.decision.slot ||
        row->pendingDecision.generation != original.decision.generation ||
        !SameOccurrence(row->pendingExpectedCurrent, original.current) ||
        !SameOccurrence(row->pendingSuccessor, original.successor) ||
        row->nextStateOwner != QueueInstanceTable::NextStateOwner::RealConditional ||
        row->pendingActivation.load(std::memory_order_acquire) != ConditionalQueueActivation::Waiting ||
        row->pendingConsumesReset.load(std::memory_order_acquire) ||
        row->conditionalReserved.load(std::memory_order_acquire)) return false;
    bool expected = false;
    if (!row->conditionalReplacementReserved.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    // The exact C1 lease is already LogicFinal/Cancelled and all captured readers
    // are strictly behind the supplied real watermark. No old frontier is invalidated.
    queues.EndOriginalConditionalFuture(*row);
    row->pendingVersion.store(0, std::memory_order_release);
    row->nextStateOwner = QueueInstanceTable::NextStateOwner::None;
    row->pendingCompletionIsLogicalBoundary = false;
    row->pendingCompletionHasLogicalFloor = false;
    row->pendingLogicalCompletionFrame = 0;
    row->pendingRetiredSource = {};
    row->pendingConsumesReset.store(false, std::memory_order_release);
    row->pendingActivation.store(ConditionalQueueActivation::Waiting, std::memory_order_release);
    row->conditionalReplacementReserved.store(false, std::memory_order_release);
    return true;
}

} // namespace harken
