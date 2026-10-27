// The scheduled-transaction pool: the single place a timeline producer files
// musical work into a bounded horizon of quantum-sized pages, and the single
// place a consumer reads it back in a fixed, deterministic order.
//
// The shape of the problem. Everything the runtime will eventually execute is an
// event at a frame; events group into frame batches (all the events at one
// execute frame), batches group into a transaction (one authored change -- a weak
// -up, a bridge, a fade -- that must land whole or not at all), and transactions
// are filed into pages, one page per mix quantum, across a stated horizon window.
// Filing is by frame, not by arrival, because arrival order says nothing about
// when something should sound.
//
// Four invariants this structure exists to guarantee, each of which a naive queue
// would break:
//
//  * All-or-nothing. A transaction spanning several pages reserves room in every
//    one of them -- a page slot for each batch, event rows for each event, and a
//    transaction record -- and is written only if every reservation succeeds. A
//    partial commit would leave a weak-up sounding with no downbeat, so a shortfall
//    on any page refuses the whole transaction and writes nothing.
//
//  * Publication freezes. A page below the publish cutoff is frozen: it stops
//    accepting work so a consumer can read it without the ground moving. A new
//    event that arrives too late for its page is not silently slid to a later one
//    -- that would move a sound in time -- it takes the caller's stated late policy
//    instead (reschedule to the earliest safe frame, drop, or refuse).
//
//  * Stale work no-ops in one check. A transaction carries the stamps a cancel
//    invalidates: the session transport epoch, the owning instance's handle and
//    schedule epoch, and -- when it is bound to one clip occurrence -- that
//    occurrence's token and schedule epoch. At consume time a single validation
//    per transaction decides the whole transaction's fate; a mismatch skips every
//    event it filed. Cancellation never hunts records down by position; stale work
//    is skipped on read and its slots reclaimed when the page leaves the window.
//
//  * Deterministic order. Within a page, events are read in one total order --
//    (executeFrame, logicCommitSequence, eventIndex, transactionOrdinal,
//    subeventPhase) -- so the same filed inputs consume to the same sequence on
//    every run and every machine. eventIndex is authoring semantic order and is
//    never reordered by a later key; the remaining keys only break ties beneath it.
//
// ABA safety. A page is identified by (transportEpoch, absoluteQuantumIndex,
// pageGeneration): the epoch changes when the whole timeline is rebuilt, the
// quantum index counts absolute quanta, and the generation is bumped each time a
// page slot is reopened at the far end of the window. A reference minted against
// an old occupant of a ring slot therefore cannot be mistaken for the current one.
//
// Capacity is bounded and declared, never elastic. Control prepares stable backing
// before publication within the fixed ceilings; readers never allocate. A request
// over any ceiling is refused with a specific reason and counted,
// and each ceiling has a high-water mark. A full pool refuses rather than evicting,
// because evicting accepted work would silence it.
//
// Single producer. Reserve is written for exactly one writer -- the timeline
// planner -- so it takes no lock; concurrent producers are out of contract. The
// pool runs on the logic thread today, but the consume and advance paths are
// written to the standard a future real-time consumer needs: no allocation, no
// lock, no unbounded scan.
//
// Not this structure's job. The pool consumes a transport epoch; it never rebuilds
// the transport or resets the epoch on wrap (that is the session owner's decision,
// a full runtime rebuild). Far-future intents beyond the horizon are held by the
// separate deferred-intent pool and materialized into transactions here once the
// window reaches them; the materialization call site is a seam, not logic owned
// here.
#ifndef HARKEN_CORE_SCHEDULED_TRANSACTION_POOL_H
#define HARKEN_CORE_SCHEDULED_TRANSACTION_POOL_H

#include "harken/core/CurrentSliceBacking.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/Plan03GroupIdentity.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/TimeCoordinates.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/AudioSliceLease.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/StableRowStorage.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <utility>

namespace harken {
class DecisionTable;
class OriginalImmediateLaneConditionalProjection;

class ImmediateActionLane;
class ImmediateLaneBucketClaimReceipt;
struct ImmediateLaneHandle;
struct ImmediateLaneBucket;
class PrivateImmediateLanePublicationProof;

// Caller-assigned identifier for one transaction, opaque to the pool.
using TransactionId = std::uint64_t;

enum class FrontierOwnerInvalidationDisposition : std::uint8_t {
    Ready,
    DeferredAfterClaim,
    NotEligible,
};

enum class ImmediateEntryAttemptState : std::uint8_t {
    Retired = 0,
    Preparing = 1,
    Finalizing = 2,
    Committed = 3,
    Missed = 4,
    Aborted = 5,
    PublicationHeld = 6,
};

// Storage must remain stable until every page and reader for its generation retires.
class ImmediateEntryAttemptGate {
public:
    class PublicationProof {
    public:
        PublicationProof() noexcept = default;
        PublicationProof(const PublicationProof&) = delete;
        PublicationProof& operator=(const PublicationProof&) = delete;
        PublicationProof(PublicationProof&& other) noexcept { *this = std::move(other); }
        PublicationProof& operator=(PublicationProof&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                generation_ = std::exchange(other.generation_, 0);
                if (owner_ != nullptr) owner_->publicationProof_ = this;
            }
            return *this;
        }
        ~PublicationProof() { Release(); }
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept {
            if (owner_ != nullptr) {
                owner_->value_.store(Encode(generation_, ImmediateEntryAttemptState::Preparing),
                    std::memory_order_release);
                owner_->publicationProof_ = nullptr;
            }
            owner_ = nullptr;
            generation_ = 0;
        }
    private:
        friend class ImmediateEntryAttemptGate;
        ImmediateEntryAttemptGate* owner_ = nullptr;
        std::uint64_t generation_ = 0;
    };

    ~ImmediateEntryAttemptGate() {
        if (publicationProof_ != nullptr) publicationProof_->Release();
    }

    bool ProvePublication(std::uint64_t generation, PublicationProof* out) noexcept {
        if (out == nullptr || out->Valid() || generation == 0 ||
            generation > (UINT64_MAX >> 3) || publicationProof_ != nullptr ||
            !Transition(generation, ImmediateEntryAttemptState::Preparing,
                ImmediateEntryAttemptState::PublicationHeld)) return false;
        out->owner_ = this;
        out->generation_ = generation;
        publicationProof_ = out;
        return true;
    }

    static void ApplyPublication(PublicationProof&& proof) noexcept {
        assert(proof.Valid());
        if (!proof.Valid()) return;
        auto* owner = proof.owner_;
        owner->value_.store(Encode(proof.generation_, ImmediateEntryAttemptState::Committed),
            std::memory_order_release);
        owner->publicationProof_ = nullptr;
        proof.owner_ = nullptr;
        proof.generation_ = 0;
    }

    bool Prepare(std::uint64_t generation) noexcept {
        if (generation == 0 || generation > (UINT64_MAX >> 3)) return false;
        std::uint64_t prior = value_.load(std::memory_order_acquire);
        if (State(prior) != ImmediateEntryAttemptState::Retired ||
            generation <= Generation(prior)) return false;
        return value_.compare_exchange_strong(
            prior, Encode(generation, ImmediateEntryAttemptState::Preparing),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool Finalize(std::uint64_t generation) noexcept {
        return Transition(generation, ImmediateEntryAttemptState::Preparing,
                          ImmediateEntryAttemptState::Finalizing);
    }

    bool Commit(std::uint64_t generation) noexcept {
        return Transition(generation, ImmediateEntryAttemptState::Finalizing,
                          ImmediateEntryAttemptState::Committed);
    }

    bool Abort(std::uint64_t generation) noexcept {
        return Transition(generation, ImmediateEntryAttemptState::Preparing,
                          ImmediateEntryAttemptState::Aborted) ||
               Transition(generation, ImmediateEntryAttemptState::Finalizing,
                          ImmediateEntryAttemptState::Aborted);
    }

    bool Retire(std::uint64_t generation) noexcept {
        return Transition(generation, ImmediateEntryAttemptState::Committed,
                          ImmediateEntryAttemptState::Retired) ||
               Transition(generation, ImmediateEntryAttemptState::Missed,
                          ImmediateEntryAttemptState::Retired) ||
               Transition(generation, ImmediateEntryAttemptState::Aborted,
                          ImmediateEntryAttemptState::Retired);
    }

    ImmediateEntryAttemptState Observe(std::uint64_t generation) const noexcept {
        const std::uint64_t value = value_.load(std::memory_order_acquire);
        return Generation(value) == generation
            ? State(value) : ImmediateEntryAttemptState::Retired;
    }

    // Only the audio reader may turn a visible but unfinished attempt into Missed.
    bool AdmitAudio(std::uint64_t generation) noexcept {
        std::uint64_t value = value_.load(std::memory_order_acquire);
        if (Generation(value) != generation) return false;
        if (State(value) == ImmediateEntryAttemptState::Committed) return true;
        if (State(value) == ImmediateEntryAttemptState::Finalizing) {
            if (value_.compare_exchange_strong(
                value, Encode(generation, ImmediateEntryAttemptState::Missed),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false;
            }
            return value == Encode(generation,
                                   ImmediateEntryAttemptState::Committed);
        }
        return false;
    }

private:
    static constexpr std::uint64_t Encode(
        std::uint64_t generation, ImmediateEntryAttemptState state) noexcept {
        return (generation << 3) | static_cast<std::uint64_t>(state);
    }
    static constexpr std::uint64_t Generation(std::uint64_t value) noexcept {
        return value >> 3;
    }
    static constexpr ImmediateEntryAttemptState State(std::uint64_t value) noexcept {
        return static_cast<ImmediateEntryAttemptState>(value & 7u);
    }
    bool Transition(std::uint64_t generation,
                    ImmediateEntryAttemptState from,
                    ImmediateEntryAttemptState to) noexcept {
        std::uint64_t expected = Encode(generation, from);
        return value_.compare_exchange_strong(
            expected, Encode(generation, to),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
    std::atomic<std::uint64_t> value_{0};
    PublicationProof* publicationProof_ = nullptr;
};

struct ImmediateEntryAttemptRef {
    ImmediateEntryAttemptGate* gate = nullptr;
    std::uint64_t generation = 0;
    bool Valid() const noexcept { return gate != nullptr && generation != 0; }
};

// The optional binding of a transaction to a single clip occurrence. When
// present, the occurrence token and its schedule epoch are validated on consume
// in addition to the owning instance, so cancelling one occurrence stops its
// transactions without touching its siblings.
struct TransactionOccurrenceRef {
    bool present = false;
    OccurrenceToken token{};
    std::uint64_t occurrenceScheduleEpoch = 0;
    std::uint64_t arrangingInstanceScheduleEpoch = 0;
};

// One event to file. The six ordering fields are the page's total sort order;
// Action inserts executionOrdinal after the sequence. Non-Action rows keep
// ordinal 0. The payload is the opaque descriptor of the runtime primitive to
// perform (the pool schedules, it does not interpret). executeFrame is on the
// absolute execute/mix-frame axis of the pool window. Before filing an event,
// the caller adds ClockKernel's signed entry-relative SampleFrame to the
// instance's absolute entry anchor and verifies the result is nonnegative. An
// unresolved negative weak-up frame must never be cast directly to this unsigned
// coordinate.
struct ScheduledEventInput {
    std::uint64_t executeFrame = 0;
    LogicCommitSequence logicCommitSequence = 0;
    std::uint32_t executionOrdinal = 0;
    std::uint32_t eventIndex = 0;        // authoring semantic order; never reordered
    std::uint32_t transactionOrdinal = 0;
    std::uint32_t subeventPhase = 0;
    std::uint64_t payload = 0;
    bool authoredFadeOutCandidate = false;
    TimelineArmToken armToken{};
    FrontierEventBinding frontier{};
};

// A whole transaction to reserve. firstFrame/lastFrame are derived from the
// events (min/max executeFrame) and reported back in the result. The events array
// is borrowed for the duration of the Reserve call only.
struct TransactionRequest {
    TransactionId transactionId = 0;
    TransportEpoch transportEpoch = kInvalidTransportEpoch;
    PlayableInstanceHandle ownerInstanceHandle = 0;
    std::uint64_t ownerInstanceScheduleEpoch = 0;
    TransactionOccurrenceRef occurrence{};
    TimelineTransactionProgramRef program{};
    TimelineArmToken armToken{};
    const ScheduledEventInput* events = nullptr;
    std::size_t eventCount = 0;
    ImmediateEntryAttemptRef immediateEntry{};
};

struct CommittedTransactionRef {
    TransportEpoch epoch = kInvalidTransportEpoch;
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t commitSerial = 0;
    std::uint64_t absoluteQuantum = UINT64_MAX;
    std::uint64_t pageGeneration = 0;

    bool Valid() const noexcept {
        return epoch != kInvalidTransportEpoch && slot != UINT32_MAX &&
            commitSerial != 0 && absoluteQuantum != UINT64_MAX &&
            pageGeneration != 0;
    }
};

struct OrdinaryStartTransactionIdentity {
    TransactionId transactionId = 0;
    PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerScheduleEpoch = 0;
    TimelineTransactionProgramRef program{};
    TimelineArmToken arm{};
    std::uint64_t startFrame = 0;
    std::uint32_t serviceSlot = UINT32_MAX;
    std::uint64_t serviceGeneration = 0;
};

enum class OrdinaryStartCancelResult : std::uint8_t {
    Removed,
    NotEligible,
    Stale,
    ProtocolViolation,
};

enum class CancelledFrontierStartResult : std::uint8_t {
    Removed,
    NotEligible,
    Stale,
    ProtocolViolation,
};

struct UnpublishedFrontierEntryEventExpected {
    TimelineTransactionProgramRef program{};
    PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerScheduleEpoch = 0;
    PlayableInstanceHandle frontierOwnerInstance = 0;
    std::uint64_t frontierOwnerEpoch = 0;
    Plan03GroupSliceIdentity group{};
    TimelineArmToken arm{};
    std::uint32_t serviceSlot = UINT32_MAX;
    std::uint64_t serviceGeneration = 0;
};

struct UnpublishedFrontierEntryExpected {
    FrontierActivationDescriptor descriptor{};
    Plan03GroupSliceIdentity member{};
    std::uint64_t executeFrame = 0;
    std::array<UnpublishedFrontierEntryEventExpected, 3> events{};
};

// What a committed transaction reserved, kept for observability and reclaim.
struct ReservationSummary {
    std::uint32_t frameBatchCount = 0; // distinct (page, executeFrame) batches
    std::uint32_t eventCount = 0;
    std::uint32_t pageSpan = 0;        // number of pages the transaction touches
};

// The read-only header of one page. Together the epoch, absolute quantum index
// and generation identify the page uniquely across the ring's lifetime.
struct HorizonPageHeader {
    TransportEpoch transportEpoch = kInvalidTransportEpoch;
    std::uint64_t absoluteQuantumIndex = 0;
    std::uint64_t pageGeneration = 0;
    std::uint64_t publishCutoffFrame = 0; // cutoff in force when frozen; 0 while open
    std::uint32_t committedTransactionCount = 0;
    std::uint32_t plan03GroupCount = 0;
    bool frozen = false;
};

struct Plan03GroupSliceInput {
    std::uint64_t executeFrame = 0;
    DecisionToken decision{};
    std::uint16_t sliceIndex = 0;
    const TimelineArmToken* members = nullptr;
    std::uint16_t memberCount = 0;
    std::uint16_t sourceControlCount = 0;
    ImmediateEntryAttemptRef immediateEntry{};
};

enum class Plan03GroupReserveStatus : std::uint8_t {
    Committed = 0,
    Invalid = 1,
    OutsideWindow = 2,
    PageFrozen = 3,
    PageGroupCapacity = 4,
    MemberCapacity = 5,
    ArmIdentityMismatch = 6,
    DuplicateMember = 7,
    DuplicateSlice = 8,
};

struct Plan03PageGroupView {
    Plan03GroupSliceIdentity identity{};
    std::uint16_t memberCount = 0;
    std::uint16_t sourceControlCount = 0;
    const TimelineArmToken* members = nullptr;
    AudioSliceLease* lease = nullptr;
    const FrontierGroupBinding* frontier = nullptr;
    ImmediateEntryAttemptRef immediateEntry{};
};

// Logic-thread observation of one exact Horizon group and its actual Page row.
struct Plan03ExactGroupSnapshot {
    Plan03GroupSliceIdentity identity{};
    HorizonPageHeader page{};
    std::uint64_t pageStamp = UINT64_MAX;
    std::uint64_t emptyPageStamp = UINT64_MAX;
    std::uint64_t globalPublishCutoffFrame = 0;
    std::uint32_t livePageGroupCount = 0;
    std::uint32_t pagePublicationHolds = 0;
    std::uint32_t readerPins = 0;
    std::uint64_t lastAudioSequence = 0;
    std::uint32_t frontierDependents = 0;
    std::uint16_t memberCount = 0;
    bool frontierPinsOwned = false;
    bool publicationHeld = false;
    std::uint8_t unpublishedRetirementStage = 0;
    FrontierGroupBinding frontier{};
    AudioSliceLeaseSnapshot effectiveLease{};
};

enum class Plan03UnpublishedRetirementResult : std::uint8_t {
    Ready, Pending, NotEligible, Stale, Fault,
};

struct Plan03UnpublishedRetirementToken {
    Plan03GroupSliceIdentity member{};
    Plan03GroupSliceIdentity primary{};
    FrontierEntryReceiptRef entry{};

    bool Valid() const noexcept {
        return member.Exact() && primary.Exact() && entry.Valid();
    }
    bool operator==(const Plan03UnpublishedRetirementToken& other) const noexcept {
        return member == other.member && primary == other.primary && entry == other.entry;
    }
};

struct Plan03ImmediateLaneGroupDescriptor {
    Plan03GroupSliceIdentity identity{};
    std::uint64_t dueQuantum = UINT64_MAX;
    bool futureDependent = false;
    std::uint32_t laneSlot = UINT32_MAX;
    std::uint32_t laneGeneration = 0;
    std::uint32_t bucketOrdinal = UINT32_MAX;
    std::uint32_t groupIndex = UINT32_MAX;
};

struct ImmediateLaneFrontierClaim {
    std::uint32_t ownerSlot = UINT32_MAX;
    std::uint64_t ownerGeneration = 0;
    std::uint32_t decisionSlot = UINT32_MAX;
    std::uint32_t decisionGeneration = 0;
    bool Valid() const noexcept {
        return ownerSlot != UINT32_MAX && ownerGeneration != 0 &&
            decisionSlot != UINT32_MAX && decisionGeneration != 0;
    }
};

class ScheduledTransactionPool;
struct Engine;

class OriginalQueueFutureGroupRegistration;
class PrivateSelectedQueueFutureSpanPool;
class PrivateImmediateLanePlacementProof;
class PrivateOriginalQueueGroupClockProof;

class OriginalQueueFutureGroupLifetimePin {
public:
    OriginalQueueFutureGroupLifetimePin() noexcept = default;
    OriginalQueueFutureGroupLifetimePin(const OriginalQueueFutureGroupLifetimePin&) = delete;
    OriginalQueueFutureGroupLifetimePin& operator=(const OriginalQueueFutureGroupLifetimePin&) = delete;
    OriginalQueueFutureGroupLifetimePin(OriginalQueueFutureGroupLifetimePin&& other) noexcept {
        Take(other);
    }
    OriginalQueueFutureGroupLifetimePin& operator=(OriginalQueueFutureGroupLifetimePin&& other) noexcept {
        if (this != &other) { Release(); Take(other); }
        return *this;
    }
    ~OriginalQueueFutureGroupLifetimePin() { Release(); }

    bool Valid() const noexcept;
    Plan03GroupSliceIdentity Identity() const noexcept {
        return Valid() ? identity_ : Plan03GroupSliceIdentity{};
    }
    bool Borrow(OriginalQueueFutureGroupLifetimePin* out) const noexcept;
    void Release() noexcept;

private:
    friend class ScheduledTransactionPool;
    friend class OriginalQueueFutureGroupRegistration;
    void Take(OriginalQueueFutureGroupLifetimePin& other) noexcept {
        owner_ = std::exchange(other.owner_, nullptr);
        identity_ = std::exchange(other.identity_, Plan03GroupSliceIdentity{});
        marker_ = std::exchange(other.marker_, 0);
    }
    ScheduledTransactionPool* owner_ = nullptr;
    Plan03GroupSliceIdentity identity_{};
    std::uint64_t marker_ = 0;
};

struct OriginalQueueFutureGroupScope {
    std::uint64_t cohortRoot = 0;
    std::uint64_t provisionalAnchorFrame = 0;
    std::uint64_t selectedFutureDomain = 0;
    Plan03GroupSliceInput input{};
};

class OriginalQueueFutureGroupRegistration {
public:
    OriginalQueueFutureGroupRegistration() noexcept = default;
    OriginalQueueFutureGroupRegistration(const OriginalQueueFutureGroupRegistration&) = delete;
    OriginalQueueFutureGroupRegistration& operator=(const OriginalQueueFutureGroupRegistration&) = delete;
    OriginalQueueFutureGroupRegistration(OriginalQueueFutureGroupRegistration&&) noexcept = default;
    OriginalQueueFutureGroupRegistration& operator=(OriginalQueueFutureGroupRegistration&&) noexcept = default;
    ~OriginalQueueFutureGroupRegistration() = default;

    bool Valid() const noexcept { return lifetime_.Valid(); }
    Plan03GroupSliceIdentity Identity() const noexcept { return lifetime_.Identity(); }
    bool BorrowLifetime(OriginalQueueFutureGroupLifetimePin* out) const noexcept {
        return lifetime_.Borrow(out);
    }
    void Release() noexcept { lifetime_.Release(); }
    bool MatchesOriginalScope(std::uint64_t cohortRoot,
        std::uint64_t provisionalAnchorFrame, const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable) const noexcept;
    // Producer-only borrowed observation. It adds no loan or publication rights.
    bool CaptureOriginalScope(const ScheduledTransactionPool& pool,
        const TimelineArmTable& armTable, OriginalQueueFutureGroupScope* out) const noexcept;

private:
    friend class ScheduledTransactionPool;
    friend class PrivateSelectedQueueFutureSpanPool;
    friend class PrivateSelectedQueueFutureDomain;
    friend class OriginalImmediateLaneConditionalProjection;
    OriginalQueueFutureGroupLifetimePin lifetime_{};
    std::uint64_t cohortRoot_ = 0;
    std::uint64_t selectedFutureDomain_ = 0;
    std::uint64_t provisionalAnchorFrame_ = 0;
    std::uint64_t executeFrame_ = 0;
    std::uint16_t memberCount_ = 0;
    std::uint16_t sourceControlCount_ = 0;
    ImmediateEntryAttemptRef immediateEntry_{};
    const TimelineArmTable* armTable_ = nullptr;
    std::uint64_t armTableIncarnation_ = 0;
};

class PrivateImmediateLaneGroupTransferProof;
class PreparedPrivateImmediateLaneGroup;
class OriginalImmediateLaneAttemptLifetimePin;
struct ImmediateLaneDemand;
struct ImmediateLaneHandle;

class PrivateSelectedQueueFutureCalendar;
class PreparedQueueReplacementOrder;

class PrivateSelectedQueueFutureDomain {
public:
    PrivateSelectedQueueFutureDomain() = default;
    PrivateSelectedQueueFutureDomain(const PrivateSelectedQueueFutureDomain&) = delete;
    PrivateSelectedQueueFutureDomain& operator=(const PrivateSelectedQueueFutureDomain&) = delete;
    PrivateSelectedQueueFutureDomain(PrivateSelectedQueueFutureDomain&& other) noexcept;
    PrivateSelectedQueueFutureDomain& operator=(PrivateSelectedQueueFutureDomain&& other) noexcept;
    ~PrivateSelectedQueueFutureDomain();
    bool Valid() const noexcept { return owner_ != nullptr; }
    std::uint64_t RootObservation() const noexcept { return marker_; }
    bool OwnsOriginal(const OriginalQueueFutureGroupRegistration& original) const noexcept;
    bool IsOldC0Original(const OriginalQueueFutureGroupRegistration& original) const noexcept;
    bool ReserveCalendar(ImmediateLaneDemand demand, ImmediateLaneHandle* out,
        OriginalImmediateLaneAttemptLifetimePin* original) noexcept;
    bool ReserveGroup(std::uint64_t anchor, const Plan03GroupSliceInput& input,
        const TimelineArmTable& arms, Plan03GroupSliceIdentity* out,
        PreparedPrivateImmediateLaneGroup* original) noexcept;
    bool BindGroup(const Plan03GroupSliceIdentity& group,
        std::uint32_t slot, std::uint32_t generation) noexcept;
    bool Rebase(std::uint64_t anchor) noexcept;
    bool PublishGroups() noexcept;
    bool CancelGroups(std::uint64_t completed,
        OriginalQueueFutureGroupRegistration* baselines, std::size_t capacity) noexcept;
    bool Return() noexcept;
private:
    friend struct Engine;
    void ReturnOwnStorageAfterQuiescence() noexcept;
    friend class ScheduledTransactionPool;
    friend class PrivateSelectedQueueFutureSpanPool;
    friend class PrivateSelectedQueueFutureCalendar;
    ScheduledTransactionPool* owner_ = nullptr;
    ImmediateActionLane* lane_ = nullptr;
    std::uint32_t laneSlot_ = UINT32_MAX, laneGeneration_ = 0;
    std::uint64_t marker_ = 0;
    const PrivateSelectedQueueFutureCalendar* calendar_ = nullptr;
    OriginalQueueFutureGroupLifetimePin oldC0_{};
};

// Only the original reservation factory can issue this transfer right.
class PreparedPrivateImmediateLaneGroup {
public:
    PreparedPrivateImmediateLaneGroup() = default;
    ~PreparedPrivateImmediateLaneGroup();
    PreparedPrivateImmediateLaneGroup(const PreparedPrivateImmediateLaneGroup&) = delete;
    PreparedPrivateImmediateLaneGroup& operator=(const PreparedPrivateImmediateLaneGroup&) = delete;
    PreparedPrivateImmediateLaneGroup(PreparedPrivateImmediateLaneGroup&& other) noexcept;
    PreparedPrivateImmediateLaneGroup& operator=(PreparedPrivateImmediateLaneGroup&& other) noexcept;
    bool Valid() const noexcept;
    bool Held() const noexcept;
    const Plan03GroupSliceIdentity& Identity() const noexcept { return identity_; }

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    Plan03GroupSliceIdentity identity_{};
    std::uint64_t cohortRoot_ = 0;
    std::uint64_t selectedFutureDomain_ = 0;
    std::uint64_t provisionalAnchorFrame_ = 0;
    std::uint64_t executeFrame_ = 0;
    std::uint16_t memberCount_ = 0;
    std::uint16_t sourceControlCount_ = 0;
    ImmediateEntryAttemptRef immediateEntry_{};
    const TimelineArmTable* armTable_ = nullptr;
    std::uint64_t armTableIncarnation_ = 0;
    std::uint64_t originalMarker_ = 0;
};

class PrivateImmediateLaneGroupTransferProof {
public:
    PrivateImmediateLaneGroupTransferProof() = default;
    ~PrivateImmediateLaneGroupTransferProof() { Release(); }
    PrivateImmediateLaneGroupTransferProof(const PrivateImmediateLaneGroupTransferProof&) = delete;
    PrivateImmediateLaneGroupTransferProof& operator=(const PrivateImmediateLaneGroupTransferProof&) = delete;
    PrivateImmediateLaneGroupTransferProof(PrivateImmediateLaneGroupTransferProof&& other) noexcept;
    PrivateImmediateLaneGroupTransferProof& operator=(PrivateImmediateLaneGroupTransferProof&& other) noexcept;
    bool Valid() const noexcept;
    Plan03GroupSliceIdentity Identity() const noexcept {
        return Valid() ? identity_ : Plan03GroupSliceIdentity{};
    }
    bool MatchesOriginalScope(const PreparedPrivateImmediateLaneGroup& original,
        std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
        const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept;
    void Release() noexcept;

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    void* row_ = nullptr;
    PreparedPrivateImmediateLaneGroup* original_ = nullptr;
    Plan03GroupSliceIdentity identity_{};
};

// Retains every actual private group and lease until the original Lane proof
// is released. Apply the Pool proof before publishing that Lane.
class PrivateImmediateLaneGroupsPublicationProof {
public:
    PrivateImmediateLaneGroupsPublicationProof() noexcept = default;
    ~PrivateImmediateLaneGroupsPublicationProof() { Release(); }
    PrivateImmediateLaneGroupsPublicationProof(const PrivateImmediateLaneGroupsPublicationProof&) = delete;
    PrivateImmediateLaneGroupsPublicationProof& operator=(const PrivateImmediateLaneGroupsPublicationProof&) = delete;
    PrivateImmediateLaneGroupsPublicationProof(PrivateImmediateLaneGroupsPublicationProof&& other) noexcept;
    PrivateImmediateLaneGroupsPublicationProof& operator=(PrivateImmediateLaneGroupsPublicationProof&& other) noexcept;
    bool Valid() const noexcept { return owner_ != nullptr; }
    void Release() noexcept;
private:
    friend class ScheduledTransactionPool;
    friend class PrivateImmediateLanePublicationProof;
    ScheduledTransactionPool* owner_ = nullptr;
    PrivateImmediateLanePublicationProof* lane_ = nullptr;
    std::uint64_t root_ = 0;
    PrivateImmediateLaneGroupsPublicationProof* next_ = nullptr;
};

// Release is explicit because a destructor cannot manufacture a completed block.
class Plan03GroupReaderPin {
public:
    Plan03GroupReaderPin() = default;
    Plan03GroupReaderPin(const Plan03GroupReaderPin&) = delete;
    Plan03GroupReaderPin& operator=(const Plan03GroupReaderPin&) = delete;
    Plan03GroupReaderPin(Plan03GroupReaderPin&& other) noexcept
        : owner_(other.owner_), identity_(other.identity_) {
        other.owner_ = nullptr;
        other.identity_ = {};
    }
    Plan03GroupReaderPin& operator=(Plan03GroupReaderPin&&) = delete;
    bool Valid() const noexcept { return owner_ != nullptr; }
    const Plan03GroupSliceIdentity& Identity() const noexcept { return identity_; }
    bool Release(std::uint64_t completedAudioSequence) noexcept;

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    Plan03GroupSliceIdentity identity_{};
};

struct PreparedPageGroupTransferScope {
    std::uint64_t cohortRoot = 0;
    LogicCommitSequence sequence = 0;
    std::uint64_t world = 0;
    std::uint64_t attemptGeneration = 0;
    bool Valid() const noexcept {
        return cohortRoot != 0 && sequence != 0 && world != 0 && attemptGeneration != 0;
    }
    bool operator==(const PreparedPageGroupTransferScope& other) const noexcept {
        return cohortRoot == other.cohortRoot && sequence == other.sequence &&
            world == other.world && attemptGeneration == other.attemptGeneration;
    }
};

class PreparedPageGroupTransferProof;
class PreparedPageFrontierBindingsProof;

class PreparedPlan03GroupSlice {
public:
    PreparedPlan03GroupSlice() = default;
    ~PreparedPlan03GroupSlice();
    PreparedPlan03GroupSlice(const PreparedPlan03GroupSlice&) = delete;
    PreparedPlan03GroupSlice& operator=(const PreparedPlan03GroupSlice&) = delete;
    PreparedPlan03GroupSlice(PreparedPlan03GroupSlice&& other) noexcept;
    PreparedPlan03GroupSlice& operator=(PreparedPlan03GroupSlice&& other) noexcept;
    bool Valid() const noexcept;
    bool Held() const noexcept;
    bool CaptureProspectiveIdentity(Plan03GroupSliceIdentity* out) const noexcept;

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    std::size_t rowIndex_ = SIZE_MAX;
    std::uint32_t generation_ = 0;
    bool prepared_ = false;
    bool transferSelected_ = false;
};

class PreparedPageGroupTransferProof {
public:
    PreparedPageGroupTransferProof() = default;
    ~PreparedPageGroupTransferProof() { Release(); }
    PreparedPageGroupTransferProof(const PreparedPageGroupTransferProof&) = delete;
    PreparedPageGroupTransferProof& operator=(const PreparedPageGroupTransferProof&) = delete;
    PreparedPageGroupTransferProof(PreparedPageGroupTransferProof&& other) noexcept;
    PreparedPageGroupTransferProof& operator=(PreparedPageGroupTransferProof&& other) noexcept;
    bool Valid() const noexcept;
    bool MatchesOriginalScope(const PreparedPlan03GroupSlice& original,
        const PreparedPageGroupTransferScope& scope, const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable) const noexcept;
    void Release() noexcept;

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    void* row_ = nullptr;
    PreparedPlan03GroupSlice* original_ = nullptr;
};

class PrivateUnboundPageArmRetirementProof;
class UnpublishedGroupArmCancellationProof;

class CommittedPlan03GroupSlice {
public:
    CommittedPlan03GroupSlice() = default;
    ~CommittedPlan03GroupSlice();
    CommittedPlan03GroupSlice(const CommittedPlan03GroupSlice&) = delete;
    CommittedPlan03GroupSlice& operator=(
        const CommittedPlan03GroupSlice&) = delete;
    CommittedPlan03GroupSlice(CommittedPlan03GroupSlice&& other) noexcept {
        *this = std::move(other);
    }
    CommittedPlan03GroupSlice& operator=(
        CommittedPlan03GroupSlice&& other) noexcept {
        if (this != &other && !Held() && !other.Held()) {
            owner_ = other.owner_;
            rowIndex_ = other.rowIndex_;
            generation_ = other.generation_;
            privateFactory_ = other.privateFactory_;
            privateExecuteFrame_ = other.privateExecuteFrame_;
            privateArmTableIncarnation_ = other.privateArmTableIncarnation_;
            other.owner_ = nullptr;
            other.rowIndex_ = SIZE_MAX;
            other.generation_ = 0;
            other.privateFactory_ = false;
            other.privateExecuteFrame_ = 0;
            other.privateArmTableIncarnation_ = 0;
        }
        return *this;
    }

    bool Valid() const noexcept {
        return owner_ != nullptr && rowIndex_ != SIZE_MAX && generation_ != 0;
    }
    bool Held() const noexcept {
        return privateRetirement_ != nullptr || frontierBindings_ != nullptr ||
            jointCancellation_ != nullptr || cancellationPending_;
    }

private:
    friend class ScheduledTransactionPool;
    friend class PrivateUnboundPageArmRetirementProof;
    ScheduledTransactionPool* owner_ = nullptr;
    std::size_t rowIndex_ = SIZE_MAX;
    std::uint32_t generation_ = 0;
    bool privateFactory_ = false;
    std::uint64_t privateExecuteFrame_ = 0;
    std::uint64_t privateArmTableIncarnation_ = 0;
    PrivateUnboundPageArmRetirementProof* privateRetirement_ = nullptr;
    PreparedPageFrontierBindingsProof* frontierBindings_ = nullptr;
    UnpublishedGroupArmCancellationProof* jointCancellation_ = nullptr;
    bool cancellationPending_ = false;
};


enum class UnpublishedGroupArmCancellationStatus : std::uint8_t {
    Held, Consumed, Busy, NotEligible, StorageTooSmall
};

// Caller storage must outlive its proof and remain at the same address.
class UnpublishedGroupCancellationHold {
public:
    UnpublishedGroupCancellationHold() = default;
    UnpublishedGroupCancellationHold(const UnpublishedGroupCancellationHold&) = delete;
    UnpublishedGroupCancellationHold& operator=(const UnpublishedGroupCancellationHold&) = delete;
private:
    friend class ScheduledTransactionPool;
    CommittedPlan03GroupSlice* original_ = nullptr;
    Plan03GroupSliceIdentity identity_{};
    AudioSliceLease* lease_ = nullptr;
    UnpublishedGroupArmCancellationProof* heldBy_ = nullptr;
};

class UnpublishedArmCancellationHold {
public:
    UnpublishedArmCancellationHold() = default;
    UnpublishedArmCancellationHold(const UnpublishedArmCancellationHold&) = delete;
    UnpublishedArmCancellationHold& operator=(const UnpublishedArmCancellationHold&) = delete;
private:
    friend class ScheduledTransactionPool;
    friend class UnpublishedGroupArmCancellationProof;
    TimelineArmToken token_{};
    TimelineArmTable::Row* row_ = nullptr;
    const void* submission_ = nullptr;
    std::uint64_t sourceEpisode_ = 0;
    UnpublishedGroupArmCancellationProof* heldBy_ = nullptr;
};

class UnpublishedGroupArmCancellationProof {
public:
    UnpublishedGroupArmCancellationProof() = default;
    UnpublishedGroupArmCancellationProof(const UnpublishedGroupArmCancellationProof&) = delete;
    UnpublishedGroupArmCancellationProof& operator=(const UnpublishedGroupArmCancellationProof&) = delete;
    ~UnpublishedGroupArmCancellationProof() { Release(); }
    bool Valid() const noexcept { return owner_ != nullptr; }
    bool ConsumedOriginalArm(TimelineArmToken token) const noexcept;
    bool HoldsOriginalArm(const TimelineArmTable& table, TimelineArmToken token) const noexcept;
    std::size_t OriginalArmCount() const noexcept { return Valid() ? arms_.size() : 0; }
    TimelineArmToken OriginalArm(std::size_t index) const noexcept {
        return Valid() && index < arms_.size() ? arms_[index].token_ : TimelineArmToken{};
    }
    void Release() noexcept;
private:
    friend class ScheduledTransactionPool;
    friend class TimelineArmTable;
    ScheduledTransactionPool* owner_ = nullptr;
    TimelineArmTable* armTable_ = nullptr;
    std::uint64_t armTableIncarnation_ = 0;
    std::span<UnpublishedGroupCancellationHold> groups_{};
    std::span<UnpublishedArmCancellationHold> arms_{};
    bool consumed_ = false;
    UnpublishedGroupArmCancellationProof* poolNext_ = nullptr;
    UnpublishedGroupArmCancellationProof* armTableNext_ = nullptr;
};

// Stable continuation of the original three private Page group grants.
// It reserves only Pool bindings and their own reader/dependent references.
class PreparedPageFrontierBindingsProof {
public:
    PreparedPageFrontierBindingsProof() = default;
    PreparedPageFrontierBindingsProof(const PreparedPageFrontierBindingsProof&) = delete;
    PreparedPageFrontierBindingsProof& operator=(const PreparedPageFrontierBindingsProof&) = delete;
    PreparedPageFrontierBindingsProof(PreparedPageFrontierBindingsProof&&) noexcept {}
    PreparedPageFrontierBindingsProof& operator=(PreparedPageFrontierBindingsProof&&) noexcept { return *this; }
    ~PreparedPageFrontierBindingsProof() { Release(); }
    bool Valid() const noexcept;
    void Release() noexcept;
private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    std::array<PreparedPageGroupTransferProof*, 3> groupProofs_{};
    std::array<CommittedPlan03GroupSlice*, 3> outputs_{};
    std::array<Plan03GroupSliceIdentity, 3> identities_{};
    std::array<FrontierGroupBinding, 3> bindings_{};
    std::array<bool, 3> minted_{};
};

class PrivateUnboundPageArmRetirementProof {
public:
    PrivateUnboundPageArmRetirementProof() = default;
    PrivateUnboundPageArmRetirementProof(const PrivateUnboundPageArmRetirementProof&) = delete;
    PrivateUnboundPageArmRetirementProof& operator=(const PrivateUnboundPageArmRetirementProof&) = delete;
    PrivateUnboundPageArmRetirementProof(PrivateUnboundPageArmRetirementProof&&) noexcept {}
    PrivateUnboundPageArmRetirementProof& operator=(PrivateUnboundPageArmRetirementProof&&) noexcept { return *this; }
    ~PrivateUnboundPageArmRetirementProof() { Release(); }
    bool Valid() const noexcept;
    void Release() noexcept;
private:
    friend class ScheduledTransactionPool;
    friend class TimelineArmTable;
    ScheduledTransactionPool* owner_ = nullptr;
    TimelineArmTable* armTable_ = nullptr;
    std::uint64_t armTableIncarnation_ = 0;
    std::array<CommittedPlan03GroupSlice*, 2> originals_{};
    std::array<Plan03GroupSliceIdentity, 2> identities_{};
    std::array<TimelineArmToken, 2> arms_{};
    std::array<TimelineArmTable::Row*, 2> rows_{};
    std::array<AudioSliceLease*, 2> leases_{};
    std::size_t heldLeases_ = 0;
    std::size_t heldArms_ = 0;
    PrivateUnboundPageArmRetirementProof* poolNext_ = nullptr;
    PrivateUnboundPageArmRetirementProof* armTableNext_ = nullptr;
};

struct ConsumedPreparedPageGroup {
    CommittedPlan03GroupSlice owner{};
    Plan03GroupSliceIdentity identity{};
};

class PreparedPlan03GroupSliceReplacement {
public:
    PreparedPlan03GroupSliceReplacement() = default;
    PreparedPlan03GroupSliceReplacement(
        const PreparedPlan03GroupSliceReplacement&) = delete;
    PreparedPlan03GroupSliceReplacement& operator=(
        const PreparedPlan03GroupSliceReplacement&) = delete;
    PreparedPlan03GroupSliceReplacement(
        PreparedPlan03GroupSliceReplacement&& other) noexcept
        : owner_(other.owner_), rowIndex_(other.rowIndex_),
          expected_(other.expected_), replacement_(other.replacement_),
          prepared_(other.prepared_) {
        other.owner_ = nullptr;
        other.rowIndex_ = 0;
        other.expected_ = {};
        other.replacement_ = {};
        other.prepared_ = false;
    }
    PreparedPlan03GroupSliceReplacement& operator=(
        PreparedPlan03GroupSliceReplacement&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            rowIndex_ = other.rowIndex_;
            expected_ = other.expected_;
            replacement_ = other.replacement_;
            prepared_ = other.prepared_;
            other.owner_ = nullptr;
            other.rowIndex_ = 0;
            other.expected_ = {};
            other.replacement_ = {};
            other.prepared_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    std::size_t rowIndex_ = 0;
    Plan03GroupSliceIdentity expected_{};
    Plan03GroupSliceInput replacement_{};
    bool prepared_ = false;
};

class PreparedPageBatchTransferProof;

class PreparedTransactionBatch {
public:
    PreparedTransactionBatch() = default;
    ~PreparedTransactionBatch();
    PreparedTransactionBatch(const PreparedTransactionBatch&) = delete;
    PreparedTransactionBatch& operator=(const PreparedTransactionBatch&) = delete;
    PreparedTransactionBatch(PreparedTransactionBatch&& other) noexcept;
    PreparedTransactionBatch& operator=(PreparedTransactionBatch&& other) noexcept;
    bool Valid() const noexcept;
    bool Held() const noexcept;
private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    std::uint32_t claimSlot_ = UINT32_MAX;
    std::uint32_t generation_ = 0;
    bool prepared_ = false;
    bool transferSelected_ = false;
};

// How to handle a transaction whose earliest frame is before the publish
// frontier. The pool implements the three outcomes it can decide on its own;
// NextEquivalentBoundary is resolved upstream by the planner (it needs the
// tempo/meter map and the clock), so submitting it here returns
// NeedsBoundaryResolution rather than the pool inventing a boundary.
enum class PrePublishLatePolicy : std::uint32_t {
    NextEquivalentBoundary = 0,
    ExecuteAtEarliestSafeFrame = 1,
    Drop = 2,
    Refuse = 3
};

enum class ReserveStatus : std::uint32_t {
    Committed = 0,   // written; firstFrame/lastFrame report where
    DroppedLate = 1, // late + Drop policy: counted, nothing written
    Refused = 2      // nothing written; see refusal
};

// Why a reserve was refused. One code per ceiling and per structural rejection,
// so a caller can attribute a refusal without guessing and so each capacity key
// has its own counted rejection.
enum class ReserveRefusal : std::uint32_t {
    None = 0,
    InvalidTransportEpoch = 1,   // epoch is 0 or does not match the pool's epoch
    EmptyTransaction = 2,
    OutsideWindow = 3,           // an event falls beyond the far edge of the window
    Late = 4,                    // before the publish frontier + Refuse policy
    NeedsBoundaryResolution = 5, // NextEquivalentBoundary: planner must resolve first
    TransactionEventsExceeded = 6,
    SpanExceeded = 7,
    PoolFull = 8,                // maxScheduledTransactions
    IngressRateExceeded = 9,     // maxIngressTransactionsPerQuantum
    PageBatchCapacity = 10,      // maxFrameBatchesPerQuantum on some page
    PageEventCapacity = 11,      // maxEventsPerQuantum on some page
    FrontierIdentity = 12,
    CommitSerialExhausted = 13,
};

inline bool IsTransientReserveRefusal(ReserveRefusal refusal) noexcept {
    return refusal == ReserveRefusal::PoolFull ||
           refusal == ReserveRefusal::IngressRateExceeded ||
           refusal == ReserveRefusal::PageBatchCapacity ||
           refusal == ReserveRefusal::PageEventCapacity;
}

struct ReserveResult {
    ReserveStatus status = ReserveStatus::Refused;
    ReserveRefusal refusal = ReserveRefusal::None;
    TransactionId transactionId = 0;
    std::uint64_t firstFrame = 0; // final span after any earliest-safe shift
    std::uint64_t lastFrame = 0;
    CommittedTransactionRef committed{};
    bool rescheduled = false;     // true when ExecuteAtEarliestSafeFrame moved it
};

// One event handed back by ConsumePage, carrying its ordering keys (so a consumer
// can verify order) and the owning transaction id.
struct ConsumedEvent {
    std::uint64_t executeFrame = 0;
    LogicCommitSequence logicCommitSequence = 0;
    std::uint32_t executionOrdinal = 0;
    std::uint32_t eventIndex = 0;
    std::uint32_t transactionOrdinal = 0;
    std::uint32_t subeventPhase = 0;
    std::uint64_t rowSequence = 0;
    std::uint64_t payload = 0;
    bool authoredFadeOutCandidate = false;
    TransactionId transactionId = 0;
    TimelineTransactionProgramRef program{};
    TimelineArmToken armToken{};
    std::uint32_t txnSlot = 0;
    bool frozenTxnValid = false;
    PlayableInstanceHandle ownerInstanceHandle = 0;
    std::uint64_t ownerInstanceScheduleEpoch = 0;
    FrontierEventBinding frontier{};
    ImmediateEntryAttemptRef immediateEntry{};
};

class PreparedPageBatchTransferProof {
public:
    PreparedPageBatchTransferProof() = default;
    ~PreparedPageBatchTransferProof() { Release(); }
    PreparedPageBatchTransferProof(const PreparedPageBatchTransferProof&) = delete;
    PreparedPageBatchTransferProof& operator=(const PreparedPageBatchTransferProof&) = delete;
    PreparedPageBatchTransferProof(PreparedPageBatchTransferProof&& other) noexcept;
    PreparedPageBatchTransferProof& operator=(PreparedPageBatchTransferProof&& other) noexcept;
    bool Valid() const noexcept;
    bool MatchesOriginalScope(const PreparedTransactionBatch& original,
        const PreparedPageGroupTransferScope& scope,
        const std::array<ConsumedEvent, 5>& inputs,
        const std::array<FrontierEventBinding, 5>& bindings) const noexcept;
    void Release() noexcept;
private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    void* claim_ = nullptr;
    PreparedTransactionBatch* original_ = nullptr;
    std::array<ConsumedEvent, 5> inputs_{};
    std::array<FrontierEventBinding, 5> bindings_{};
};

enum class CurrentSliceKind : std::uint8_t {
    VariableBankCut = 1,
    MeterProbeWithdrawal = 2,
    NativeStartCalendar = 3,
};

struct CurrentSliceRef {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
};

struct ScheduledCurrentSliceInput {
    std::uint64_t executeFrame = 0;
    LogicCommitSequence logicCommitSequence = 0;
    CurrentSliceKind kind = CurrentSliceKind::VariableBankCut;
    CurrentSliceRef slice{};
    std::uint32_t executionOrdinal = 0;
};

struct ConsumedCurrentSlice {
    std::uint64_t executeFrame = 0;
    LogicCommitSequence logicCommitSequence = 0;
    CurrentSliceKind kind = CurrentSliceKind::VariableBankCut;
    CurrentSliceRef slice{};
    std::uint32_t executionOrdinal = 0;
    std::uint64_t reservationSequence = 0;
};

// The pool must outlive every held token, including RAII rollback.
class HeldCurrentSliceToken {
public:
    HeldCurrentSliceToken() = default;
    ~HeldCurrentSliceToken();
    HeldCurrentSliceToken(const HeldCurrentSliceToken&) = delete;
    HeldCurrentSliceToken& operator=(const HeldCurrentSliceToken&) = delete;
    HeldCurrentSliceToken(HeldCurrentSliceToken&& other) noexcept;
    HeldCurrentSliceToken& operator=(HeldCurrentSliceToken&& other) noexcept;

    bool IsHeld() const noexcept { return owner_ != nullptr; }
    std::uint64_t ExecuteFrame() const noexcept { return executeFrame_; }
    CurrentSliceKind Kind() const noexcept { return kind_; }
    CurrentSliceRef Slice() const noexcept { return slice_; }
    std::uint64_t ReservationSequence() const noexcept { return rowSequence_; }

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
    std::uint64_t incarnation_ = 0;
    TransportEpoch epoch_ = kInvalidTransportEpoch;
    std::uint64_t absoluteQuantum_ = 0;
    std::uint64_t pageGeneration_ = 0;
    std::uint64_t rowSequence_ = 0;
    std::uint64_t executeFrame_ = 0;
    CurrentSliceKind kind_ = CurrentSliceKind::VariableBankCut;
    CurrentSliceRef slice_{};
};

inline constexpr std::uint64_t kUnpublishedQuantumStamp = UINT64_MAX;

struct UnplacedBatchCreditRequest {
    std::uint32_t transactions = 0;
    std::uint32_t preparedClaims = 0;
    std::uint32_t events = 0;
    std::uint32_t frameBatches = 0;
    std::uint32_t groupHeaders = 0;
};

struct UnplacedBatchCredit {
    std::size_t slot = SIZE_MAX;
    std::uint64_t generation = 0;
    TransportEpoch epoch = kInvalidTransportEpoch;
    bool Valid() const noexcept {
        return slot != SIZE_MAX && generation != 0 &&
            epoch != kInvalidTransportEpoch;
    }
};
struct UnplacedBatchCohortMember {
    UnplacedBatchCredit* credit = nullptr;
    const TransactionRequest* requests = nullptr;
    std::size_t requestCount = 0;
    const Plan03GroupSliceInput* groups = nullptr;
    std::size_t groupCount = 0;
};
inline constexpr std::uint32_t kMaxPlan03GroupHeadersPerPageHard = 1024;
inline constexpr std::uint32_t kMaxPlan03ArmsPerGroupHard = 1024;
namespace detail {
struct ScheduledTransactionPoolTestAccess;
#if !defined(HARKEN_RT_QUALIFICATION)
struct ScheduledTransactionPoolCancelTestAccess;
#endif
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
struct ScheduledTransactionPoolOriginalTestAccess;
#endif
}

class ScheduledTransactionPool {
    struct LaneGroupMembers;
public:
    class ImmediateLaneBackingPreparation {
    public:
        ImmediateLaneBackingPreparation() = default;
        ~ImmediateLaneBackingPreparation();
        ImmediateLaneBackingPreparation(const ImmediateLaneBackingPreparation&) = delete;
        ImmediateLaneBackingPreparation& operator=(const ImmediateLaneBackingPreparation&) = delete;
        ImmediateLaneBackingPreparation(ImmediateLaneBackingPreparation&& other) noexcept;
        ImmediateLaneBackingPreparation& operator=(ImmediateLaneBackingPreparation&& other) noexcept;
        bool Valid() const noexcept { return rows_.Valid(); }
        std::size_t DirectoryBytes() const noexcept {
            return members_.capacity() * sizeof(std::unique_ptr<LaneGroupMembers>);
        }
    private:
        friend class ScheduledTransactionPool;
        void Release() noexcept;
        ScheduledTransactionPool* owner_ = nullptr;
        StableStoragePreparation rows_;
        std::vector<std::unique_ptr<LaneGroupMembers>> members_;
    };
    class ImmediateLaneBackingUse {
    public:
        ImmediateLaneBackingUse(ScheduledTransactionPool& owner,
            ImmediateLaneBackingPreparation* backing) noexcept;
        ~ImmediateLaneBackingUse();
        ImmediateLaneBackingUse(const ImmediateLaneBackingUse&) = delete;
        ImmediateLaneBackingUse& operator=(const ImmediateLaneBackingUse&) = delete;
    private:
        ScheduledTransactionPool& owner_;
        ImmediateLaneBackingPreparation* previous_;
        StableStoragePreparedUse rows_;
    };
    bool PrepareImmediateLaneBacking(std::uint32_t groupCount, std::uint64_t totalMembers,
        ImmediateLaneBackingPreparation* out) noexcept;
    // Attach before preparing rows; the producer callback must not re-enter this pool.
    bool AttachImmediateLaneRowStoragePreparation(void* context,
        bool (*prepare)(void*, std::uint32_t) noexcept) noexcept;
    void DetachImmediateLaneRowStoragePreparation() noexcept {
        immediateLaneRowStorageContext_ = nullptr;
        prepareImmediateLaneRowStorage_ = nullptr;
    }
    std::size_t ImmediateLaneRowStorageBytes() const noexcept { return immediateLaneGroups_.RequestedBytes(); }
    std::size_t ImmediateLaneRowCommittedBytes() const noexcept { return immediateLaneGroups_.CommittedBytes(); }
    std::size_t ImmediateLaneRowReservedBytes() const noexcept { return immediateLaneGroups_.ReservedBytes(); }
    std::size_t ImmediateLaneMemberStorageBytes() const noexcept;
    std::size_t PageGroupMemberStorageBytes() const noexcept;
    void SetImmediateLaneRowCommitPageLimitForTests(std::size_t pages) noexcept {
        immediateLaneGroups_.SetCommitPageLimitForTests(pages);
    }
    void SetImmediateLaneMemberAllocationLimitForTests(std::uint16_t count) noexcept {
        immediateMemberAllocationLimitForTests_ = count;
    }

    class PrivateMaterializationTransaction {
    public:
        PrivateMaterializationTransaction() = default;
        PrivateMaterializationTransaction(
            const PrivateMaterializationTransaction&) = delete;
        PrivateMaterializationTransaction& operator=(
            const PrivateMaterializationTransaction&) = delete;
        PrivateMaterializationTransaction(
            PrivateMaterializationTransaction&&) = delete;
        PrivateMaterializationTransaction& operator=(
            PrivateMaterializationTransaction&&) = delete;
        ~PrivateMaterializationTransaction();

        bool Valid() const noexcept { return owner_ != nullptr; }
        PreparedTransactionBatch* Transactions() noexcept {
            return &transactions_;
        }
        PreparedPlan03GroupSlice* Group(std::size_t index) noexcept {
            return index < groupCount_ ? &groups_[index] : nullptr;
        }
        const CommittedPlan03GroupSlice* CommittedGroup(
            std::size_t index) const noexcept {
            return index < groupCount_ ? &committedGroups_[index] : nullptr;
        }
        std::size_t GroupCount() const noexcept { return groupCount_; }
        bool Commit(LogicCommitSequence sequence) noexcept;
        bool ReleasePublication() noexcept;

    private:
        friend class ScheduledTransactionPool;
        ScheduledTransactionPool* owner_ = nullptr;
        UnplacedBatchCredit* credit_ = nullptr;
        UnplacedBatchCredit** cohortCredits_ = nullptr;
        std::uint32_t cohortCreditCount_ = 0;
        PreparedTransactionBatch transactions_{};
        PreparedPlan03GroupSlice* groups_ = nullptr;
        CommittedPlan03GroupSlice* committedGroups_ = nullptr;
        std::uint32_t groupCount_ = 0;
        bool committed_ = false;
    };

    // The full set of ceilings, using the capacity-ledger key names verbatim.
    // quantumFrames is the mix quantum; pageCount is derived as
    // scheduleHorizonFrames / quantumFrames.
    struct Config {
        std::uint32_t quantumFrames = 0;
        std::uint64_t scheduleHorizonFrames = 0;
        std::uint32_t publishLeadQuanta = 0;
        std::uint32_t maxScheduledTransactions = 0;
        std::uint64_t maxTransactionSpanFrames = 0;
        std::uint32_t maxIngressTransactionsPerQuantum = 0;
        std::uint32_t maxFrameBatchesPerQuantum = 0;
        std::uint32_t maxEventsPerQuantum = 0;
        std::uint32_t maxTransactionEvents = 0;
        std::uint32_t maxCurrentSlicesPerQuantum = 0;
        std::uint32_t maxNativeStartCalendarsPerQuantum = 0;
        std::uint32_t maxPlan03GroupHeadersPerPage = 0;
        std::uint32_t maxArmsPerGroup = 0;
        std::uint32_t maxImmediateLaneGroupRows = 0;
        std::uint32_t maxImmediateLaneOwnerGates = 0;
        std::uint32_t maxImmediateLaneDecisionGates = 0;
    };

    ScheduledTransactionPool() = default;
    ~ScheduledTransactionPool();
    ScheduledTransactionPool(const ScheduledTransactionPool&) = delete;
    ScheduledTransactionPool& operator=(const ScheduledTransactionPool&) = delete;
    ScheduledTransactionPool(ScheduledTransactionPool&&) = delete;
    ScheduledTransactionPool& operator=(ScheduledTransactionPool&&) = delete;

    struct PendingEventStorageDemand {
        std::uint64_t nativeEvents = 0;
        std::uint64_t retiredEvents = 0;
        std::uint64_t nativeGroups = 0;
        std::uint64_t retiredGroups = 0;
    };
    // Registrations are stationary and attach/detach only at consumer quiescence.
    class PendingEventStorageRegistration {
    public:
        using Prepare = bool (*)(void*, PendingEventStorageDemand) noexcept;
        PendingEventStorageRegistration() = default;
        ~PendingEventStorageRegistration() { Detach(); }
        PendingEventStorageRegistration(const PendingEventStorageRegistration&) = delete;
        PendingEventStorageRegistration& operator=(const PendingEventStorageRegistration&) = delete;
        void Detach() noexcept;
    private:
        friend class ScheduledTransactionPool;
        const ScheduledTransactionPool* owner_ = nullptr;
        PendingEventStorageRegistration* previous_ = nullptr;
        PendingEventStorageRegistration* next_ = nullptr;
        void* context_ = nullptr;
        Prepare prepare_ = nullptr;
    };
    // Control-only. Prepare this existing demand before attaching a consumer.
    PendingEventStorageDemand CapturePendingEventStorageDemand() const noexcept;
    void AttachPendingEventStorage(PendingEventStorageRegistration& registration,
        void* context, PendingEventStorageRegistration::Prepare prepare) const noexcept;
    class CurrentSliceStorageRegistration {
    public:
        using Prepare = bool (*)(void*, std::size_t) noexcept;
        CurrentSliceStorageRegistration() = default;
        ~CurrentSliceStorageRegistration() { Detach(); }
        CurrentSliceStorageRegistration(const CurrentSliceStorageRegistration&) = delete;
        CurrentSliceStorageRegistration& operator=(const CurrentSliceStorageRegistration&) = delete;
        void Detach() noexcept;
    private:
        friend class ScheduledTransactionPool;
        const ScheduledTransactionPool* owner_ = nullptr;
        CurrentSliceStorageRegistration* previous_ = nullptr;
        CurrentSliceStorageRegistration* next_ = nullptr;
        void* context_ = nullptr;
        Prepare prepare_ = nullptr;
    };
    std::size_t PreparedCurrentSliceRowCount() const noexcept {
        return currentSliceRows_.ConstructedCount();
    }
    // Control-only. Prepare the existing high-water mark before attaching.
    void AttachCurrentSliceStorage(CurrentSliceStorageRegistration& registration,
        void* context, CurrentSliceStorageRegistration::Prepare prepare) const noexcept;
    std::uint64_t RetiredPageEventCount() const noexcept {
        return retiredPageEvents_.load(std::memory_order_acquire);
    }
    std::uint64_t RetiredPageGroupCount() const noexcept {
        return retiredPageGroups_.load(std::memory_order_acquire);
    }

    // Sizes all storage from the config and opens the window at startFrame with
    // the given transport epoch. Returns false (leaving the pool invalid) if the
    // config is degenerate (zero quantum, horizon not a whole number of quanta,
    // any zero ceiling) or the epoch is the reserved 0. Non-reentrant: Reset
    // discards all state.
    bool Reset(const Config& config, std::uint64_t startFrame,
               TransportEpoch epoch);
    // Rebuilds the window under a new epoch with the config already in
    // force — the transport-reset seam's pool step (TransportEpochOwner.h
    // order). Discards every filed page and transaction like Reset.
    bool CanResetForEpoch(std::uint64_t startFrame,
                          TransportEpoch epoch) const noexcept;
    bool ResetForEpoch(std::uint64_t startFrame,
                       TransportEpoch epoch) noexcept;

    bool Valid() const { return terminalState_.load(std::memory_order_acquire) == 0; }
    TransportEpoch Epoch() const { return epoch_; }
    std::uint32_t PageCount() const { return pageCount_; }
    std::uint32_t QuantumFrames() const { return quantumFrames_; }
    std::uint32_t PublishLeadQuanta() const { return config_.publishLeadQuanta; }
    // The per-transaction event ceiling, so a producer can size its own filing
    // scratch to match what Reserve will accept.
    std::uint32_t MaxTransactionEvents() const {
        return config_.maxTransactionEvents;
    }
    std::uint32_t MaxEventsPerQuantum() const {
        return config_.maxEventsPerQuantum;
    }
    std::uint32_t MaxScheduledTransactions() const {
        return config_.maxScheduledTransactions;
    }
    std::uint32_t MaxCurrentSlicesPerQuantum() const {
        return currentSliceStorageCapacity_;
    }
    std::uint32_t MaxVariableCurrentSlicesPerQuantum() const {
        return config_.maxCurrentSlicesPerQuantum;
    }
    std::uint64_t CurrentSliceStorageBytes() const noexcept;
    std::size_t NativeEventRowStorageBytes() const noexcept { return nativeEventRows_.RequestedBytes(); }
    std::size_t NativeEventBackingCount() const noexcept { return nativeEventRows_.ConstructedCount(); }
    std::size_t NativeEventRowReservedBytes() const noexcept { return nativeEventRows_.ReservedBytes(); }
    std::size_t NativeEventRowCommittedBytes() const noexcept { return nativeEventRows_.CommittedBytes(); }
    std::size_t NativeGroupRowStorageBytes() const noexcept { return nativeGroupRows_.RequestedBytes(); }
    std::size_t NativeGroupRowReservedBytes() const noexcept { return nativeGroupRows_.ReservedBytes(); }
    std::size_t NativeGroupRowCommittedBytes() const noexcept { return nativeGroupRows_.CommittedBytes(); }
    std::size_t PrivateMaterializationStorageBytes() const noexcept {
        return unplacedCredits_.RequestedBytes() + privatePreparedGroups_.RequestedBytes() +
            privateCommittedGroups_.RequestedBytes() + privateCohortCredits_.RequestedBytes() +
            privateCohortRequests_.RequestedBytes() + privateCohortGroups_.RequestedBytes();
    }
    std::size_t PrivateMaterializationCommittedBytes() const noexcept {
        return unplacedCredits_.CommittedBytes() + privatePreparedGroups_.CommittedBytes() +
            privateCommittedGroups_.CommittedBytes() + privateCohortCredits_.CommittedBytes() +
            privateCohortRequests_.CommittedBytes() + privateCohortGroups_.CommittedBytes();
    }
    std::size_t PrivateMaterializationReservedBytes() const noexcept {
        return unplacedCredits_.ReservedBytes() + privatePreparedGroups_.ReservedBytes() +
            privateCommittedGroups_.ReservedBytes() + privateCohortCredits_.ReservedBytes() +
            privateCohortRequests_.ReservedBytes() + privateCohortGroups_.ReservedBytes();
    }
    std::size_t GroupDirectoryStorageBytes() const noexcept {
        return pageGroupSlots_.capacity() * sizeof(std::size_t) +
            pagePlan03GroupRowStates_.capacity() * sizeof(Plan03GroupRowState) +
            pagePlan03GroupClaimGenerations_.capacity() * sizeof(std::uint32_t);
    }
    std::size_t PageEventDirectoryStorageBytes() const noexcept {
        return pageEventSlots_.RequestedBytes() + pageEventOrder_.RequestedBytes() +
            freeEventSlots_.RequestedBytes() + pageBatchRows_.RequestedBytes();
    }
    std::size_t PageEventDirectoryCommittedBytes() const noexcept {
        return pageEventSlots_.CommittedBytes() + pageEventOrder_.CommittedBytes() +
            freeEventSlots_.CommittedBytes() + pageBatchRows_.CommittedBytes();
    }
    std::size_t PageEventDirectoryReservedBytes() const noexcept {
        return pageEventSlots_.ReservedBytes() + pageEventOrder_.ReservedBytes() +
            freeEventSlots_.ReservedBytes() + pageBatchRows_.ReservedBytes();
    }
    std::uint64_t CurrentSliceCommittedBytes() const noexcept;
    std::uint64_t CurrentSliceReservedBytes() const noexcept;
    bool PrepareCurrentSliceBacking(CurrentSliceBacking* out) noexcept;
    std::uint32_t MaxPlan03GroupHeadersPerPage() const {
        return config_.maxPlan03GroupHeadersPerPage;
    }
    std::uint32_t MaxArmsPerGroup() const {
        return config_.maxArmsPerGroup;
    }
    std::uint64_t MaxTransactionSpanFrames() const {
        return config_.maxTransactionSpanFrames;
    }

    bool ReserveUnplacedBatchCredit(const UnplacedBatchCreditRequest& request,
                                    UnplacedBatchCredit* out) noexcept;
    bool CancelUnplacedBatchCredit(UnplacedBatchCredit* credit) noexcept;
    bool PreflightUnplacedBatchMaterialization(
        const UnplacedBatchCredit& credit,
        const TransactionRequest* requests, std::size_t requestCount,
        const Plan03GroupSliceInput* groups, std::size_t groupCount,
        const TimelineArmTable& armTable,
        std::uint64_t publicationFrontier,
        ReserveRefusal* transactionRefusal,
        Plan03GroupReserveStatus* groupStatus) noexcept;
    bool TryMaterializeUnplacedBatch(
        UnplacedBatchCredit* credit,
        const TransactionRequest* requests, std::size_t requestCount,
        const Plan03GroupSliceInput* groups, std::size_t groupCount,
        const TimelineArmTable& armTable,
        std::uint64_t publicationFrontier,
        PrivateMaterializationTransaction* out,
        ReserveRefusal* transactionRefusal,
        Plan03GroupReserveStatus* groupStatus) noexcept;
    bool TryMaterializeUnplacedCohort(
        const UnplacedBatchCohortMember* members,
        std::size_t memberCount, const TimelineArmTable& armTable,
        std::uint64_t publicationFrontier,
        PrivateMaterializationTransaction* out,
        ReserveRefusal* transactionRefusal,
        Plan03GroupReserveStatus* groupStatus) noexcept;
    void SetPrivateMaterializationFaultStepForTest(
        std::uint32_t step) noexcept {
        privateMaterializationFaultStepForTest_ = step;
    }
    std::uint32_t PrivateMaterializationFaultStepForTest() const noexcept {
        return privateMaterializationFaultStepForTest_;
    }
    struct PrivateMaterializationSnapshotForTest {
        std::uint64_t preparedEvents = 0;
        std::uint64_t preparedGroups = 0;
        std::uint64_t publicationHolds = 0;
        bool active = false;
    };
    PrivateMaterializationSnapshotForTest
    CapturePrivateMaterializationSnapshotForTest() const noexcept {
        PrivateMaterializationSnapshotForTest snapshot{};
        for (const auto count : pagePreparedEventCount_)
            snapshot.preparedEvents += count;
        for (const auto count : pagePreparedPlan03GroupCount_)
            snapshot.preparedGroups += count;
        for (const auto count : pagePublicationHolds_)
            snapshot.publicationHolds += count;
        snapshot.active = privateMaterializationActive_;
        return snapshot;
    }

    // First frame the window covers and the first frame past it.
    std::uint64_t WindowStartFrame() const { return baseFrame_; }
    // Absolute quantum containing WindowStartFrame.
    std::uint64_t WindowStartQuantum() const { return baseQuantumIndex_; }
    std::uint64_t WindowEndFrame() const {
        return baseFrame_ +
               static_cast<std::uint64_t>(pageCount_) * quantumFrames_;
    }

    // The page a frame belongs to. False when the frame is outside the window --
    // the same answer Reserve gives, so asking first never differs from trying.
    bool PageForFrame(std::uint64_t frame, std::uint32_t* outPage) const;

    // The read-only header of a page, or nullptr for an index outside the ring.
    const HorizonPageHeader* Page(std::uint32_t page) const;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    std::size_t CapturePageEventsForTest(
        std::uint32_t page, ConsumedEvent* out,
        std::size_t capacity) const noexcept;
    bool CaptureFrameBatchReferencesForTest(
        std::uint64_t frame, std::uint32_t* live,
        std::uint32_t* prepared) const noexcept;
#endif
    bool IsFrozen(std::uint32_t page) const;
    std::uint64_t PublishCutoffFrame() const { return publishCutoffFrame_; }

    // Freezes every page whose whole frame range is below cutoffFrame, recording
    // the cutoff in their headers. Idempotent and monotonic: a lower cutoff than
    // one already in force is ignored, because unfreezing a published page would
    // let a reader's ground move.
    void Publish(std::uint64_t cutoffFrame,
                 const PlayableInstanceTable* instances = nullptr,
                 const OccurrenceStatePool* occurrences = nullptr,
                 TimelineArmTable* armTable = nullptr);

    // The single producer's write path. Groups the request's events into pages,
    // checks every ceiling for every touched page, and writes only if all pass.
    // A late request takes latePolicy. Returns what happened and, on a commit,
    // the final frame span. A producer preparing an unpublished prefix can supply
    // its candidate frontier for the policy decision without freezing any page.
    ReserveResult Reserve(const TransactionRequest& request,
                          PrePublishLatePolicy latePolicy);
    ReserveResult Reserve(const TransactionRequest& request,
                          PrePublishLatePolicy latePolicy,
                          std::uint64_t publicationFrontier);
    OrdinaryStartCancelResult CancelUnpublishedOrdinaryStartExact(
        CommittedTransactionRef ref,
        const OrdinaryStartTransactionIdentity& expected) noexcept;
    bool CaptureUnpublishedFrontierStartRowSequence(
        const TransactionRequest& expected,
        const FrontierEntryReceiptRef& entry,
        const Plan03GroupSliceIdentity& member,
        std::uint64_t* outRowSequence) const noexcept;
    CancelledFrontierStartResult CancelUnpublishedFrontierStartExact(
        const TimelineTransactionProgramRef* relatedPrograms,
        std::size_t relatedProgramCount,
        const TransactionRequest& expected,
        const FrontierEntryReceiptRef& entry,
        const Plan03GroupSliceIdentity& member,
        std::uint64_t expectedRowSequence,
        const PlayableInstanceTable& instances) noexcept;
    CancelledFrontierStartResult CancelUnpublishedFrontierEntryExact(
        const UnpublishedFrontierEntryExpected& expected,
        const PlayableInstanceTable& instances) noexcept;
    bool HasFrontierEntryEvent(const FrontierEntryReceiptRef& entry) const noexcept;
    ReserveRefusal PreflightSingleEvent(
        std::uint64_t executeFrame,
        std::uint64_t publicationFrontier) const noexcept;
    ReserveRefusal PreflightSingleEventTransactions(
        std::uint64_t executeFrame,
        std::uint32_t additionalTransactionCount,
        std::uint64_t publicationFrontier) const noexcept;
    ReserveRefusal PreflightSingleEventTransactionBatch(
        const std::uint64_t* executeFrames,
        std::uint32_t transactionCount,
        std::uint64_t publicationFrontier) const noexcept;
    ReserveRefusal PreflightTransaction(
        const TransactionRequest& request,
        std::uint64_t publicationFrontier) const noexcept;
    ReserveRefusal PreflightTransactionPair(
        const TransactionRequest& first,
        const TransactionRequest& second,
        std::uint64_t publicationFrontier) const noexcept;
    ReserveRefusal PreflightTransactionBatch(
        const TransactionRequest* requests,
        std::size_t requestCount,
        std::uint64_t publicationFrontier) const noexcept;
    ReserveRefusal PrepareTransactionBatch(
        const TransactionRequest* requests,
        std::size_t requestCount,
        std::uint64_t publicationFrontier,
        PreparedTransactionBatch* out,
        const PreparedPageGroupTransferScope* transferScope = nullptr) noexcept;
    bool ProvePreparedPageBatchTransfer(PreparedTransactionBatch& original,
        const PreparedPageGroupTransferScope& scope,
        const std::array<ConsumedEvent, 5>& inputs,
        const std::array<FrontierEventBinding, 5>& bindings,
        PreparedPageBatchTransferProof* out) noexcept;
    static PreparedTransactionBatch ConsumePreparedPageBatchTransfer(
        PreparedPageBatchTransferProof&& proof) noexcept;
    bool FreezePreparedTransactionBatch(
        const PreparedTransactionBatch& prepared,
        LogicCommitSequence sequence) noexcept;
    // Reads the actual reserved row key before metadata promotion or exposure.
    bool CapturePreparedTransactionEvent(
        const PreparedTransactionBatch& prepared,
        std::uint32_t requestIndex, std::uint32_t eventOrdinal,
        ConsumedEvent* out) const noexcept;
    // Qualifies the same uncommitted, frozen one-event requests and actual row keys.
    bool MatchesFrozenPreparedTransactionEvents(
        const PreparedTransactionBatch& prepared, LogicCommitSequence sequence,
        TransportEpoch transport, const ConsumedEvent* originals, std::size_t count,
        const FrontierEventBinding* bindings = nullptr) const noexcept;
    bool BindPreparedTransactionEventFrontier(
        const PreparedTransactionBatch& prepared,
        std::uint32_t requestIndex, std::uint32_t eventOrdinal,
        const FrontierEventBinding& binding) noexcept;
    bool CommitPreparedTransactionBatch(
        PreparedTransactionBatch* prepared,
        LogicCommitSequence sequence,
        bool retainPublicationHold = false) noexcept;
    bool CanReleasePreparedTransactionPublication(
        const PreparedTransactionBatch& prepared) const noexcept;
    bool ReleasePreparedTransactionPublication(
        PreparedTransactionBatch* prepared) noexcept;
    bool CancelPreparedTransactionBatch(
        PreparedTransactionBatch* prepared) noexcept;
    ReserveResult ReserveCurrentSlice(
        const ScheduledCurrentSliceInput& input,
        PrePublishLatePolicy latePolicy,
        std::uint64_t publicationFrontier);
    ReserveResult ReserveHeldCurrentSlice(
        const ScheduledCurrentSliceInput& input,
        PrePublishLatePolicy latePolicy,
        std::uint64_t publicationFrontier,
        HeldCurrentSliceToken* out, CurrentSliceBacking* backing = nullptr);
    bool CanReleaseHeldCurrentSlice(const HeldCurrentSliceToken& token) const noexcept;
    ReserveResult PreflightHeldCurrentSlice(const ScheduledCurrentSliceInput& input,
        PrePublishLatePolicy latePolicy, std::uint64_t publicationFrontier) const noexcept;
    bool ReleaseHeldCurrentSlice(HeldCurrentSliceToken* token) noexcept;
    bool CancelHeldCurrentSlice(HeldCurrentSliceToken* token,
        CurrentSliceBacking* restoreBacking = nullptr) noexcept;
    Plan03GroupReserveStatus PreflightPlan03GroupSlices(
        std::uint64_t executeFrame,
        std::uint32_t additionalGroupCount) const noexcept;
    Plan03GroupReserveStatus ReservePlan03GroupSlice(
        const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable) noexcept;
    Plan03GroupReserveStatus PreparePlan03GroupSlice(
        const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable,
        PreparedPlan03GroupSlice* out,
        const PreparedPageGroupTransferScope* transferScope = nullptr) noexcept;
    bool ProvePreparedPageGroupTransfer(PreparedPlan03GroupSlice& original,
        const PreparedPageGroupTransferScope& scope, const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable, PreparedPageGroupTransferProof* out) noexcept;
    static ConsumedPreparedPageGroup ConsumePreparedPageGroupTransfer(
        PreparedPageGroupTransferProof&& proof) noexcept;
    bool ProvePreparedPageFrontierBindings(
        const std::array<PreparedPageGroupTransferProof*, 3>& groups,
        const PreparedPageBatchTransferProof& batch,
        const std::array<FrontierGroupBinding, 3>& bindings,
        const std::array<CommittedPlan03GroupSlice*, 3>& outputs,
        PreparedPageFrontierBindingsProof* out) noexcept;
    static void ConsumePreparedPageGroupIntoFrontierBindings(
        PreparedPageGroupTransferProof&& group,
        PreparedPageFrontierBindingsProof& bindings,
        CommittedPlan03GroupSlice& output) noexcept;
    static void ConsumePreparedPageFrontierBindings(
        PreparedPageFrontierBindingsProof&& proof) noexcept;
    bool CommitPreparedPlan03GroupSlice(
        PreparedPlan03GroupSlice* prepared) noexcept;
    bool CommitPreparedPlan03GroupSlice(
        PreparedPlan03GroupSlice* prepared,
        CommittedPlan03GroupSlice* committed,
        bool retainPublicationHold = false) noexcept;
    bool CancelPreparedPlan03GroupSlice(
        PreparedPlan03GroupSlice* prepared) noexcept;
    bool MatchesCommittedPlan03GroupSlice(const CommittedPlan03GroupSlice& committed,
        const Plan03GroupSliceIdentity& identity) const noexcept;
    bool RollbackCommittedPlan03GroupSlice(
        CommittedPlan03GroupSlice* committed) noexcept;
    // Begin is an explicit cancellation request. Its native page protection
    // remains owned by the original committed cap across Busy and Release.
    UnpublishedGroupArmCancellationStatus BeginUnpublishedGroupArmCancellation(
        std::span<CommittedPlan03GroupSlice* const> originals, TimelineArmTable& arms,
        std::span<UnpublishedGroupCancellationHold> groupStorage,
        std::span<UnpublishedArmCancellationHold> armStorage,
        UnpublishedGroupArmCancellationProof* out) noexcept;
    UnpublishedGroupArmCancellationStatus FinishUnpublishedGroupArmCancellation(
        UnpublishedGroupArmCancellationProof& proof,
        std::span<const TimelineArmTable::OriginalUnpublishedSourceDisposition* const> sources) noexcept;
    bool ProvePrivateUnboundPageArmRetirement(
        CommittedPlan03GroupSlice& first, CommittedPlan03GroupSlice& second,
        TimelineArmTable& arms, const std::array<TimelineArmToken, 2>& members,
        std::uint64_t executeFrame, PrivateUnboundPageArmRetirementProof* out) noexcept;
    static void ConsumePrivateUnboundPageArmRetirement(
        PrivateUnboundPageArmRetirementProof&& proof) noexcept;
    bool CanReleaseCommittedPlan03GroupSliceOwnership(
        const CommittedPlan03GroupSlice& committed) const noexcept;
    bool ReleaseCommittedPlan03GroupSliceOwnership(
        CommittedPlan03GroupSlice* committed) noexcept;
    Plan03GroupReserveStatus PreparePlan03GroupSliceReplacement(
        const Plan03GroupSliceIdentity& expected,
        const Plan03GroupSliceInput& replacement,
        const TimelineArmTable& armTable,
        PreparedPlan03GroupSliceReplacement* out) noexcept;
    bool CommitPreparedPlan03GroupSliceReplacement(
        PreparedPlan03GroupSliceReplacement* prepared) noexcept;
    bool CancelPreparedPlan03GroupSliceReplacement(
        PreparedPlan03GroupSliceReplacement* prepared) noexcept;

    // Appends a page's live events to out in the total sort order, skipping every
    // event whose transaction fails the one-per-transaction stale check against
    // currentEpoch, the instance table and the occurrence pool. Writes at most
    // outCapacity events and returns how many were written; a caller sizing out
    // at maxEventsPerQuantum can never be truncated. Does not allocate.
    std::size_t ConsumePage(std::uint32_t page, TransportEpoch currentEpoch,
                            const PlayableInstanceTable& instances,
                            const OccurrenceStatePool& occurrences,
                            ConsumedEvent* out, std::size_t outCapacity);
    std::size_t ConsumePageRt(std::uint64_t absoluteQuantum,
                              ConsumedEvent* out,
                              std::size_t outCapacity) const noexcept;
    // Copies the exact-quantum slice set all-or-none. A value above
    // outCapacity is the required count and means nothing was copied.
    std::size_t ConsumeCurrentSlicesRt(
        std::uint64_t absoluteQuantum, ConsumedCurrentSlice* out,
        std::size_t outCapacity) const noexcept;
    std::size_t CapturePlan03PageGroupsRt(
        std::uint64_t absoluteQuantum, Plan03PageGroupView* out,
        std::size_t outCapacity) const noexcept;
    std::size_t CaptureLivePlan03Groups(
        Plan03PageGroupView* out, std::size_t outCapacity) const noexcept;
    bool CaptureExactPlan03Group(
        const Plan03GroupSliceIdentity& identity,
        Plan03ExactGroupSnapshot* out) const noexcept;
    Plan03UnpublishedRetirementResult BeginUnpublishedFrontierRetirement(
        const Plan03GroupSliceIdentity& member,
        const Plan03GroupSliceIdentity& primary,
        const FrontierEntryReceiptRef& entry,
        const FrontierActivationReceipt& receipt,
        Plan03UnpublishedRetirementToken* out) noexcept;
    Plan03UnpublishedRetirementResult FinishUnpublishedFrontierRetirement(
        const Plan03UnpublishedRetirementToken& token) noexcept;
    bool PreparePrivateSelectedQueueFutureDomain(
        const OriginalQueueFutureGroupRegistration& oldC0,
        const OriginalQueueFutureGroupRegistration& oldC1,
        const LogicCommitCoordinator::PreparedIssue& sequence,
        ImmediateActionLane& distinctNewLane,
        PrivateSelectedQueueFutureDomain* out) noexcept;
    // The original attempt capability authenticates use of the same physical
    // scheduler. ReserveCalendar still allocates an independent native attempt.
    bool PreparePrivateSelectedQueueFutureDomain(
        const OriginalQueueFutureGroupRegistration& oldC0,
        const OriginalQueueFutureGroupRegistration& oldC1,
        const OriginalImmediateLaneAttemptLifetimePin& originalAttempt,
        const PreparedQueueReplacementOrder& order,
        ImmediateActionLane& newLane,
        PrivateSelectedQueueFutureDomain* out) noexcept;

    // Lane groups are owned by the initial Action cohort and never occupy a
    // horizon Page. Their exact identity remains stable when its anchor moves.
    bool ReserveImmediateLaneGroup(std::uint64_t cohortRoot,
        std::uint64_t provisionalAnchorFrame, const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable, Plan03GroupSliceIdentity* out,
        PreparedPrivateImmediateLaneGroup* original = nullptr) noexcept;
    bool ProvePrivateImmediateLaneGroupsPublication(
        PrivateImmediateLanePublicationProof& lane,
        PrivateImmediateLaneGroupsPublicationProof* out) noexcept;
    bool IsPrivateImmediateLaneGroupsPublicationProofFor(
        const PrivateImmediateLaneGroupsPublicationProof& proof,
        const Plan03GroupSliceIdentity& identity, const FrontierEntryReceiptRef& entry) const noexcept;
    static void ApplyPrivateImmediateLaneGroupsPublication(
        PrivateImmediateLaneGroupsPublicationProof&& proof) noexcept;
    bool OwnsPrivateImmediateLaneGroupOriginal(
        const PreparedPrivateImmediateLaneGroup& original) const noexcept;
    bool PrivateImmediateLaneGroupTransferHeld(
        const PreparedPrivateImmediateLaneGroup& original) const noexcept;
    bool PrivateImmediateLaneGroupTransferHeld(
        const Plan03GroupSliceIdentity& identity) const noexcept;
    bool MatchesHeldPrivateImmediateLaneGroupOriginal(
        const PreparedPrivateImmediateLaneGroup& original,
        const PrivateImmediateLaneGroupTransferProof& proof,
        std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
        const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable) const noexcept;
    bool ProvePrivateImmediateLaneGroupTransfer(
        PreparedPrivateImmediateLaneGroup& original, std::uint64_t cohortRoot,
        std::uint64_t provisionalAnchorFrame, const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable,
        PrivateImmediateLaneGroupTransferProof* out) noexcept;
    static Plan03GroupSliceIdentity ConsumePrivateImmediateLaneGroupTransfer(
        PrivateImmediateLaneGroupTransferProof&& proof) noexcept;
    bool ConsumePrivateImmediateLaneGroupTransfer(
        PrivateImmediateLaneGroupTransferProof&& proof,
        OriginalQueueFutureGroupRegistration* out) noexcept;
    bool IsOriginalQueueFutureGroupRegistrationFor(
        const OriginalQueueFutureGroupRegistration& original,
        const Plan03GroupSliceIdentity& expected) const noexcept;
    bool IsOriginalQueueFutureGroupRegistrationFor(
        const OriginalQueueFutureGroupRegistration& original,
        const Plan03GroupSliceIdentity& expected,
        std::uint64_t originalCohortRoot) const noexcept;
    bool IsOriginalQueueFutureGroupFamily(
        const OriginalQueueFutureGroupRegistration& first,
        const OriginalQueueFutureGroupRegistration& second) const noexcept;
    bool CaptureOriginalQueueGroupRelativeFrames(
        const OriginalQueueFutureGroupRegistration& first,
        const OriginalQueueFutureGroupRegistration& second,
        std::array<std::uint64_t, 2>* out) const noexcept;
    bool ProvePrivateOriginalQueueGroupClock(
        const OriginalQueueFutureGroupRegistration& first,
        const OriginalQueueFutureGroupRegistration& second,
        PrivateImmediateLanePlacementProof* actualPlacement,
        PrivateOriginalQueueGroupClockProof* out) noexcept;
    // Observes actual private geometry and true initial Source binding. The
    // caller must separately authenticate Queue and physical Source receivers.
    bool CapturePrivateOriginalQueueClockInitialSource(
        const PrivateOriginalQueueGroupClockProof& proof,
        const OriginalQueueFutureGroupLifetimePin& c0,
        const OriginalQueueFutureGroupLifetimePin& c1,
        const TimelineArmTable& arms, const DecisionTable& decisions,
        TimelineArmToken source, std::array<std::uint64_t, 4>* outFrames,
        const FrontierGroupBinding** outInitial,
        const FrontierGroupBinding** outConditional) const noexcept;
    // Scoped native geometry only. Current/Source family authority remains
    // with the original Engine issuer; no C1 or historical anchor is accepted.
    bool CapturePrivateOriginalQueueC0Source(
        const PrivateOriginalQueueGroupClockProof& proof,
        const OriginalQueueFutureGroupLifetimePin& primary,
        const OriginalQueueFutureGroupLifetimePin& initial,
        const TimelineArmTable& arms, const DecisionTable& decisions,
        TimelineArmToken source, std::array<std::uint64_t, 3>* outFrames,
        const FrontierGroupBinding** outPrimary,
        const FrontierGroupBinding** outInitial) const noexcept;
    bool IsPrivateOriginalQueueGroupClockProofFor(
        const PrivateOriginalQueueGroupClockProof& proof,
        const OriginalQueueFutureGroupRegistration& first,
        const OriginalQueueFutureGroupRegistration& second) const noexcept;
    // Observes only a relation issued by the same private Engine factories.
    bool IsInitialSoundQueueCompanionC0(
        const Plan03GroupSliceIdentity& primary, const Plan03GroupSliceIdentity& member,
        const FrontierOwnerIdentity& sound, const FrontierOwnerIdentity& queue) const noexcept;
    bool IsInitialSoundQueueCompanionC1(
        const Plan03GroupSliceIdentity& primary, const Plan03GroupSliceIdentity& member,
        const Plan03GroupSliceIdentity& prerequisiteMember,
        const FrontierOwnerIdentity& sound, const FrontierOwnerIdentity& queue) const noexcept;
    bool CancelPrivateImmediateLaneGroup(
        PreparedPrivateImmediateLaneGroup* original,
        Plan03GroupSliceIdentity* rawHolder) noexcept;
    bool BindImmediateLaneGroupOwner(
        const Plan03GroupSliceIdentity& identity,
        const ImmediateActionLane& lane, std::uint64_t cohortRoot,
        std::uint32_t laneSlot, std::uint32_t laneGeneration) noexcept;
    bool RebaseImmediateLaneGroups(std::uint64_t cohortRoot,
        std::uint64_t anchorFrame) noexcept;
    bool PublishImmediateLaneGroups(std::uint64_t cohortRoot) noexcept;
    bool CancelPrivateImmediateLaneGroup(std::uint64_t cohortRoot,
        const Plan03GroupSliceIdentity& identity,
        OriginalQueueFutureGroupRegistration* originalBaseline = nullptr) noexcept;
    using ImmediateLaneGroupDisposalObserver = void (*)(void*, Plan03GroupSliceIdentity,
        std::uint64_t, std::uint64_t) noexcept;
    // Producer-only. The observer must not re-enter or mutate this pool.
    void SetImmediateLaneGroupDisposalObserver(void* context,
        ImmediateLaneGroupDisposalObserver observer) noexcept {
        immediateLaneGroupDisposalContext_ = context;
        immediateLaneGroupDisposalObserver_ = observer;
    }
    bool CancelImmediateLaneGroups(std::uint64_t cohortRoot,
        std::uint64_t completedAudioSequence,
        OriginalQueueFutureGroupRegistration* originalBaselines = nullptr,
        std::size_t originalBaselineCapacity = 0) noexcept;
    std::size_t CaptureImmediateLaneCohortGroups(
        std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
        std::size_t outCapacity) const noexcept;
    // Logic-only observation; includes private and published rows without transferring ownership.
    std::size_t CaptureLiveImmediateLaneCohortGroups(
        std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
        std::size_t outCapacity) const noexcept;
    std::size_t CaptureImmediateLaneGroupsRt(std::uint64_t absoluteQuantum,
        Plan03PageGroupView* out, std::size_t outCapacity) const noexcept;
    bool PrivatePlan03GroupBindable(const Plan03GroupSliceIdentity& identity) const noexcept;
    bool PrivateImmediateLaneGroupRetirementReady(const Plan03GroupSliceIdentity& identity,
        std::uint64_t completedAudioSequence) const noexcept;
    bool PrivateImmediateLaneFrontierRollbackReady(
        const Plan03GroupSliceIdentity& identity) const noexcept;
    bool MatchesPrivateImmediateLaneGroupPlacement(const Plan03GroupSliceIdentity& identity,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement) const noexcept;
    bool ClaimImmediateLaneFrontier(const Plan03GroupSliceIdentity& identity,
        ImmediateLaneFrontierClaim* out) const noexcept;
    bool ValidateImmediateLaneGroupClaim(
        const Plan03GroupSliceIdentity& identity,
        const AudioSliceLease* lease,
        const ImmediateLaneBucketClaimReceipt& receipt) const noexcept;
    bool ProveImmediateLaneGroupBeforeClaim(
        const Plan03GroupSliceIdentity& identity,
        const AudioSliceLease* lease, const ImmediateActionLane& lane,
        ImmediateLaneHandle handle,
        const ImmediateLaneBucket* bucket) const noexcept;
    bool BindImmediateLaneGroupBuckets(
        const Plan03ImmediateLaneGroupDescriptor* groups,
        std::uint32_t count, const ImmediateActionLane& lane,
        std::uint32_t laneSlot,
        std::uint32_t laneGeneration) noexcept;
    bool ClaimPrivateImmediateLaneFrontier(
        const Plan03GroupSliceIdentity& identity,
        ImmediateLaneFrontierClaim* out) const noexcept;
    bool ReleaseImmediateLaneFrontier(ImmediateLaneFrontierClaim* claim) const noexcept;
    // Logic thread: transfer audio-side O(1) gate invalidations to their leases.
    void SettleInvalidatedImmediateLaneFrontiers(
        bool (*held)(void*, std::uint64_t, std::uint32_t,
                     std::uint32_t) noexcept = nullptr,
        void* context = nullptr) noexcept;
    void DropPreparedOccupancy() noexcept;
    bool ResolvePlan03GroupSliceRt(
        const Plan03GroupSliceIdentity& identity,
        Plan03PageGroupView* out) const noexcept;
    bool ResolvePlan03GroupSliceProducer(
        const Plan03GroupSliceIdentity& identity,
        Plan03PageGroupView* out) const noexcept;
    bool AcquirePlan03GroupReader(const Plan03GroupSliceIdentity& identity,
                                 Plan03GroupReaderPin* out) noexcept;
    bool ClonePlan03GroupReader(const Plan03GroupReaderPin& source,
                               Plan03GroupReaderPin* out) noexcept;
    bool IsPlan03GroupReaderFor(const Plan03GroupReaderPin& pin,
        const Plan03GroupSliceIdentity& identity) const noexcept {
        return pin.owner_ == this && pin.identity_ == identity;
    }
    bool NotePlan03GroupReadRt(const Plan03GroupSliceIdentity& identity,
                              std::uint64_t audioSequence) const noexcept;
    bool IsInvalidatedPlan03GroupAudioReadComplete(
        const Plan03GroupSliceIdentity& identity, std::uint32_t expectedReasonBit,
        std::uint64_t completedAudioSequence) const noexcept;
    std::uint64_t Plan03GroupReaderStorageBytes() const noexcept;
    bool BindPlan03GroupFrontier(const Plan03GroupSliceIdentity& identity,
                                const FrontierGroupBinding& binding) noexcept;
    bool DetachPlan03GroupFrontier(const Plan03GroupSliceIdentity& identity,
                                  std::uint64_t completedAudioSequence) noexcept;
    bool AcquireImmediateLaneFutureDependent(
        const Plan03GroupSliceIdentity& identity) noexcept;
    bool ReleaseImmediateLaneFutureDependent(
        const Plan03GroupSliceIdentity& identity) noexcept;
    bool InvalidateFrontierOwnerOperations(const FrontierOwnerIdentity& owner,
        DecisionToken decision = {}, AudioSliceInvalidateReason reason = AudioSliceInvalidateReason::Cancel) const noexcept;
    FrontierOwnerInvalidationDisposition TryInvalidateFrontierOwnerOperations(
        const FrontierOwnerIdentity& owner, DecisionToken decision,
        AudioSliceInvalidateReason reason) const noexcept;
    bool HasFrontierOwnerOperations(const FrontierOwnerIdentity& owner, DecisionToken decision = {}) const noexcept;
    bool FrontierMemberBindingDetached(const Plan03GroupSliceIdentity& identity,
        const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence) const noexcept;
    bool FrontierCoordinatorDetached(const Plan03GroupSliceIdentity& identity,
        const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
        const Plan03GroupReaderPin* activationReader = nullptr) const noexcept;
    std::uint64_t PageStamp(std::uint64_t absoluteQuantum) const noexcept;
    // Producer-only: confirms every attempt-owned entry record was frozen valid.
    bool VerifyImmediateEntryPage(
        std::uint64_t absoluteQuantum, ImmediateEntryAttemptRef entry,
        std::uint32_t expectedEvents,
        std::uint32_t expectedGroups) const noexcept;
    std::uint64_t PublishedEmptyPrefixEnd(std::uint64_t frontier,
                                        std::uint64_t end) const noexcept;
    bool TryAdvancePublishedEmptyPrefixRt(std::uint64_t frontier,
                                         std::uint64_t end) noexcept;
    std::uint64_t PublishedEmptyStampStorageBytes() const noexcept {
        return static_cast<std::uint64_t>(pageCount_) * sizeof(std::atomic<std::uint64_t>);
    }
    std::uint64_t SessionBaseQuantumIndex() const {
        return sessionBaseQuantumIndex_;
    }
    std::atomic<std::uint64_t>& ConsumerFrontier() {
        consumerFrontierEnabled_ = true;
        return *consumedFrontierQuantum_;
    }
    const std::atomic<std::uint64_t>& ConsumerFrontier() const {
        return *consumedFrontierQuantum_;
    }

    // Slides the window forward one quantum. The page that falls out is emptied
    // and reopened at the far end with a bumped generation and the next absolute
    // quantum index. Returns how many event rows fell out with it, which is how a
    // caller notices it published too late rather than losing work silently. If
    // the new far edge would overflow, no geometry changes, the pool becomes
    // invalid, and the session recreates the Runtime under its
    // RuntimeRecreateRequired policy instead of folding the frame axis.
    std::uint32_t Advance();

    // Live occupancy and the peak each reached, for the capacity contract. Every
    // ceiling has a high-water mark here; a per-ceiling refusal counter records
    // how often it turned work away.
    std::uint32_t LiveTransactionCount() const { return liveTransactionCount_; }
    std::uint32_t TransactionHighWater() const { return transactionHighWater_; }
    std::uint32_t FrameBatchHighWater() const { return frameBatchHighWater_; }
    std::uint32_t EventHighWater() const { return eventHighWater_; }
    std::uint32_t TransactionEventsHighWater() const {
        return transactionEventsHighWater_;
    }
    std::uint64_t TransactionSpanHighWater() const {
        return transactionSpanHighWater_;
    }
    std::uint32_t IngressThisQuantum() const { return ingressThisQuantum_; }
    std::uint32_t IngressHighWater() const { return ingressHighWater_; }
    std::uint64_t DroppedLateCount() const { return droppedLateCount_; }
    std::uint64_t AdvanceBlockedByConsumer() const {
        return advanceBlockedByConsumer_;
    }
    std::uint64_t RefusalCount(ReserveRefusal reason) const;
    bool HasLiveProgramGeneration(ProgramGeneration generation) const;
    bool HasProgramReference(TimelineTransactionProgramRef program) const;

private:
    friend struct Engine;
    bool PrepareEventStorage(std::uint64_t additionalEvents,
        bool consumeAcceptedBacking = false) noexcept;
    bool PreparePrivateMaterializationBacking(const UnplacedBatchCreditRequest& request) noexcept;
    bool PrepareGroupStorage(std::size_t additionalGroups,
        bool consumeAcceptedBacking = false) noexcept;
    Plan03GroupReserveStatus PreparePlan03GroupSliceImpl(
        const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
        PreparedPlan03GroupSlice* out, const PreparedPageGroupTransferScope* transferScope,
        bool consumeAcceptedBacking) noexcept;
    ReserveRefusal PrepareTransactionBatchImpl(
        const TransactionRequest* requests, std::size_t requestCount,
        std::uint64_t publicationFrontier, PreparedTransactionBatch* out,
        const PreparedPageGroupTransferScope* transferScope,
        bool consumeAcceptedBacking) noexcept;
    void RecordRetiredPageEvents(std::uint64_t count) noexcept;
    void RecordRetiredPageGroups(std::uint64_t count) noexcept;
    bool IsOrdinaryOriginalQueueFutureGroupRegistrationFor(
        const OriginalQueueFutureGroupRegistration& original,
        const Plan03GroupSliceIdentity& expected, std::uint64_t root) const noexcept {
        return original.selectedFutureDomain_ == 0 &&
            IsOriginalQueueFutureGroupRegistrationFor(original, expected, root);
    }
    bool DeclareInitialSoundQueueCompanion(
        const std::array<const OriginalQueueFutureGroupRegistration*, 4>& originals,
        const ImmediateActionLane& lane, std::uint32_t laneSlot, std::uint32_t laneGeneration,
        std::uint64_t root, std::uint64_t attempt, std::uint64_t world,
        const FrontierOwnerIdentity& sound, const FrontierOwnerIdentity& queue,
        const FrontierQueueInitialIdentity& initial,
        const FrontierQueueConditionalIdentity& conditional) noexcept;
    friend class PreparedTransactionBatch;
    friend class PreparedPageBatchTransferProof;
    bool OwnsPreparedPageBatchOriginal(const PreparedTransactionBatch& original) const noexcept;
    bool PreparedPageBatchHeld(const PreparedTransactionBatch& original) const noexcept;
    bool MatchesPreparedPageBatchInput(const PreparedTransactionBatch& original,
        const PreparedPageGroupTransferScope& scope,
        const std::array<ConsumedEvent, 5>& inputs,
        const std::array<FrontierEventBinding, 5>& bindings) const noexcept;
    void SpendPreparedPageBatchOriginal(std::size_t slot) noexcept;
    void DetachPreparedPageBatchTransfer(std::size_t slot) noexcept;
    void DropPreparedPageBatchOriginal(PreparedTransactionBatch* original) noexcept;
    friend class PreparedPlan03GroupSlice;
    friend class PreparedPageGroupTransferProof;
    friend class PrivateUnboundPageArmRetirementProof;
    friend class UnpublishedGroupArmCancellationProof;
    friend class CommittedPlan03GroupSlice;
    void ReleaseUnpublishedGroupArmCancellation(UnpublishedGroupArmCancellationProof& proof) noexcept;
    void ConsumeHeldUnpublishedGroupArmCancellation(UnpublishedGroupArmCancellationProof& proof) noexcept;
    bool CompleteJointGroupMemberCoverage(const UnpublishedGroupArmCancellationProof& proof,
        TimelineArmToken arm) const noexcept;
    void DropUnpublishedCancellationOriginal(CommittedPlan03GroupSlice& original) noexcept;
    UnpublishedGroupArmCancellationProof* unpublishedGroupArmCancellations_ = nullptr;
    friend class PreparedPageFrontierBindingsProof;
    bool PreparedPageFrontierBindingsHeld(const PreparedPageFrontierBindingsProof& proof) const noexcept;
    void ReleasePreparedPageFrontierBindings(PreparedPageFrontierBindingsProof& proof,
        bool ownerDying = false) noexcept;
    std::size_t preparedPageFrontierBindingHolds_ = 0;
    void ReleasePrivateUnboundPageArmRetirement(PrivateUnboundPageArmRetirementProof& proof) noexcept;
    PrivateUnboundPageArmRetirementProof* privateUnboundPageArmRetirements_ = nullptr;
    bool OwnsPreparedPageGroupOriginal(const PreparedPlan03GroupSlice& original) const noexcept;
    bool PreparedPageGroupHeld(const PreparedPlan03GroupSlice& original) const noexcept;
    bool MatchesPreparedPageGroupInput(const PreparedPlan03GroupSlice& original,
        const PreparedPageGroupTransferScope& scope, const Plan03GroupSliceInput& input,
        const TimelineArmTable& armTable) const noexcept;
    void DetachPreparedPageGroupTransfer(std::size_t rowIndex) noexcept;
    void ReleasePreparedPageGroupTransferLease(PreparedPageGroupTransferProof* proof) noexcept;
    void DropPreparedPageGroupOriginal(PreparedPlan03GroupSlice* original) noexcept;
    friend class PrivateOriginalQueueGroupClockProof;
    bool MatchesPrivateOriginalQueueGroupClockProof(
        const PrivateOriginalQueueGroupClockProof& proof) const noexcept;
    void ReturnPrivateOriginalQueueGroupClockProof(
        PrivateOriginalQueueGroupClockProof& proof,
        PrivateImmediateLanePlacementProof* returnPlacement = nullptr) noexcept;
    void RetargetPrivateOriginalQueueGroupClockProof(
        PrivateOriginalQueueGroupClockProof& from,
        PrivateOriginalQueueGroupClockProof& to) noexcept;
    friend class OriginalQueueFutureGroupLifetimePin;
    friend class OriginalImmediateLaneConditionalProjection;
    friend class PrivateSelectedQueueFutureSpanPool;
    friend class OriginalQueueFutureGroupRegistration;
    bool MatchesOriginalQueueFutureGroupLifetime(
        const OriginalQueueFutureGroupLifetimePin& original) const noexcept;
    // Hidden native readers retain these exact factory loans. This does not
    // publish their Lane or grant Catalog selection, admission or a lease claim.
    bool ResolveRetainedHiddenConditionalGroup(
        const OriginalQueueFutureGroupLifetimePin& primary,
        const OriginalQueueFutureGroupLifetimePin& member,
        Plan03PageGroupView* out) const noexcept;
    friend class PrivateSelectedQueueFutureDomain;
    bool ReturnPrivateSelectedQueueFutureDomain(PrivateSelectedQueueFutureDomain& domain) noexcept;
    friend class QueueContinuationLane;
    std::uint64_t nextSelectedFutureDomain_ = 0;
    std::uint32_t selectedFutureDomains_ = 0;
    bool ReserveImmediateLaneGroupInDomain(
    std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
    const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
    Plan03GroupSliceIdentity* out, PreparedPrivateImmediateLaneGroup* original, std::uint64_t domain) noexcept;
    bool CancelImmediateLaneGroupsInDomain(
    std::uint64_t cohortRoot, std::uint64_t completedAudioSequence,
    OriginalQueueFutureGroupRegistration* originalBaselines,
    std::size_t originalBaselineCapacity, std::uint64_t domain) noexcept;
    bool CancelPrivateImmediateLaneGroupInDomain(
    std::uint64_t cohortRoot, const Plan03GroupSliceIdentity& identity,
    OriginalQueueFutureGroupRegistration* originalBaseline, std::uint64_t domain) noexcept;
    bool BindImmediateLaneGroupOwnerInDomain(
    const Plan03GroupSliceIdentity& identity,
    const ImmediateActionLane& lane, std::uint64_t cohortRoot,
    std::uint32_t laneSlot, std::uint32_t laneGeneration, std::uint64_t domain) noexcept;
    bool RebaseImmediateLaneGroupsInDomain(
    std::uint64_t cohortRoot, std::uint64_t anchorFrame, std::uint64_t domain) noexcept;
    bool PublishImmediateLaneGroupsInDomain(
    std::uint64_t cohortRoot, std::uint64_t domain) noexcept;
    bool OriginalQueueFutureGroupRetirementReady(
        const Plan03GroupSliceIdentity& identity,
        const OriginalQueueFutureGroupRegistration* originalBaseline) const noexcept;
    bool BorrowOriginalQueueFutureGroupLifetime(
        const OriginalQueueFutureGroupLifetimePin& original,
        OriginalQueueFutureGroupLifetimePin* out) noexcept;
    void ReleaseOriginalQueueFutureGroupLifetime(
        OriginalQueueFutureGroupLifetimePin& original) noexcept;
    bool MatchesOriginalQueueFutureGroupScope(
        const OriginalQueueFutureGroupRegistration& original,
        std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
        const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept;
    bool MatchesOriginalQueueFutureGroupScopeImpl(
        const OriginalQueueFutureGroupRegistration& original,
        std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
        const Plan03GroupSliceInput& input, const TimelineArmTable& armTable,
        bool retiredSounds) const noexcept;
    bool CaptureOriginalQueueFutureGroupScope(
        const OriginalQueueFutureGroupRegistration& original,
        const TimelineArmTable& armTable, OriginalQueueFutureGroupScope* out) const noexcept;
    friend class PreparedPrivateImmediateLaneGroup;
    friend class PrivateImmediateLaneGroupTransferProof;
    friend class PrivateImmediateLaneGroupsPublicationProof;
    void ReleasePrivateImmediateLaneGroupsPublication(
        PrivateImmediateLaneGroupsPublicationProof& proof, bool publish = false) noexcept;
    void MovePrivateImmediateLaneGroupsPublication(
        PrivateImmediateLaneGroupsPublicationProof& from,
        PrivateImmediateLaneGroupsPublicationProof& to) noexcept;
    bool InitialLaneCohortPublicationHeld(std::uint64_t root) const noexcept;
    PrivateImmediateLaneGroupsPublicationProof* laneGroupPublications_ = nullptr;
    void DropPrivateImmediateLaneGroupOriginal(
        PreparedPrivateImmediateLaneGroup* original) noexcept;
    void ReleasePrivateImmediateLaneGroupTransferLease(
        PrivateImmediateLaneGroupTransferProof* proof) noexcept;
    bool MatchesPrivateImmediateLaneGroupInput(
        const PreparedPrivateImmediateLaneGroup& original,
        std::uint64_t cohortRoot, std::uint64_t provisionalAnchorFrame,
        const Plan03GroupSliceInput& input, const TimelineArmTable& armTable) const noexcept;
    friend struct detail::ScheduledTransactionPoolTestAccess;
#if !defined(HARKEN_RT_QUALIFICATION)
    friend struct detail::ScheduledTransactionPoolCancelTestAccess;
#endif
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    friend struct detail::ScheduledTransactionPoolOriginalTestAccess;
#endif
    bool ClaimImmediateLaneFrontierBound(
        const Plan03GroupSliceIdentity& identity,
        ImmediateLaneFrontierClaim* out, bool producerPrivate) const noexcept;
    bool RollbackPrivateMaterialization(
        PrivateMaterializationTransaction* transaction) noexcept;
    bool CanRollbackPrivateMaterialization(
        const PrivateMaterializationTransaction& transaction) const noexcept;
    bool CanCommitPrivateMaterialization(
        const PrivateMaterializationTransaction& transaction,
        LogicCommitSequence sequence) const noexcept;
    bool PreflightMaterializationGroups(
        const Plan03GroupSliceInput* groups, std::size_t groupCount,
        const TimelineArmTable& armTable,
        std::uint64_t publicationFrontier,
        Plan03GroupReserveStatus* groupStatus) const noexcept;
    ReserveRefusal CheckUnplacedPageBudget(std::uint64_t additionalEvents,
                                           std::uint64_t additionalBatches) const noexcept;
    bool HasUnplacedGroupBudget(std::uint32_t additionalGroups) const noexcept;
    friend class Plan03GroupReaderPin;
    bool ReleasePlan03GroupReader(Plan03GroupReaderPin* pin,
                                 std::uint64_t completedAudioSequence) noexcept;
    bool HasPlan03GroupReaders() const noexcept;
    void FinishPreparedTransactionClaim(PreparedTransactionBatch* prepared) noexcept;
    bool MatchesPlan03GroupRow(const Plan03GroupSliceIdentity& identity,
                              std::size_t rowIndex) const noexcept;
    bool IsUnpublishedRetirementParticipant(
        const Plan03GroupSliceIdentity& identity) const noexcept;
    struct Plan03GroupReaderState {
        std::atomic<std::uint32_t> pins{0};
        mutable std::atomic<std::uint64_t> lastAudioSequence{0};
    };
    enum class LaneGroupState : std::uint8_t { Free, Private, Published, Retired };
    struct LaneFrontierGate {
        std::atomic<std::uint64_t> generation{0};
        std::atomic<std::uint32_t> ownerSlot{UINT32_MAX};
        std::atomic<std::uint64_t> ownerGeneration{0};
        // High bit invalidates, next six bits retain reasons, low bits count
        // audio claims. One word gives cancellation and claim one linearization point.
        std::atomic<std::uint32_t> state{0};
        std::uint32_t references = 0; // Logic thread only.
    };
    struct InitialSoundQueueCompanionAssociation {
        std::uint64_t root = 0;
        std::uint64_t attempt = 0;
        std::uint64_t world = 0;
        const ImmediateActionLane* lane = nullptr;
        std::uint32_t laneSlot = UINT32_MAX;
        std::uint32_t laneGeneration = 0;
        FrontierOwnerIdentity sound{};
        FrontierOwnerIdentity queue{};
        FrontierQueueInitialIdentity initial{};
        FrontierQueueConditionalIdentity conditional{};
        std::array<Plan03GroupSliceIdentity, 4> groups{};
        std::array<std::uint64_t, 4> markers{};
    };
    bool MatchesInitialSoundQueueCompanionRows(
        const InitialSoundQueueCompanionAssociation& original, bool audio) const noexcept;
    bool MatchesInitialSoundQueueCompanionBinding(
        const Plan03GroupSliceIdentity& identity, const FrontierGroupBinding& binding,
        bool audio = false) const noexcept;
    bool MatchesInitialSoundQueueCompanionPublished(
        const InitialSoundQueueCompanionAssociation& original) const noexcept;
    struct LaneGroupMembers {
        ScheduledTransactionPool* owner = nullptr;
        std::unique_ptr<TimelineArmToken[]> tokens;
        std::unique_ptr<TimelineArmTable::OriginalGroupMember[]> originals;
        std::unique_ptr<std::uint16_t[]> indices;
        std::uint16_t capacity = 0;
        bool held = false;
        bool inUse = false;
        bool pageBacking = false;
        ~LaneGroupMembers();
        std::size_t RequestedBytes() const noexcept {
            return sizeof(LaneGroupMembers) + static_cast<std::size_t>(capacity) *
                (sizeof(TimelineArmToken) + sizeof(TimelineArmTable::OriginalGroupMember) +
                    (indices == nullptr ? 0u : sizeof(std::uint16_t)));
        }
    };
    struct PageGroupMemberSlot {
        std::unique_ptr<LaneGroupMembers> owned;
        std::unique_ptr<LaneGroupMembers> replacement;
        LaneGroupMembers* borrowed = nullptr;
        ~PageGroupMemberSlot();
        PageGroupMemberSlot() = default;
        PageGroupMemberSlot(PageGroupMemberSlot&& other) noexcept;
        PageGroupMemberSlot(const PageGroupMemberSlot&) = delete;
        LaneGroupMembers* Storage() const noexcept { return borrowed == nullptr ? owned.get() : borrowed; }
        void ReturnBorrowed() noexcept;
    };
    struct PageGroupMemberBacking {
        std::vector<std::unique_ptr<LaneGroupMembers>> members;
        ~PageGroupMemberBacking();
    };
    TimelineArmToken* PageGroupTokens(std::size_t row) const noexcept;
    TimelineArmTable::OriginalGroupMember* PageGroupOriginals(std::size_t row) const noexcept;
    bool PreparePageGroupMembers(std::size_t row, std::uint16_t count,
        bool consumeAcceptedBacking = false) noexcept;
    void ClearPageGroupMembers(std::size_t row, std::uint16_t count) noexcept;
    std::unique_ptr<LaneGroupMembers> CreatePageGroupMembers(std::uint16_t count) noexcept;
    std::unique_ptr<PageGroupMemberBacking> PreparePageGroupMemberBacking(std::uint32_t count) noexcept;
    struct LaneGroupRow {
        ~LaneGroupRow() noexcept {
            if (preparedMembers != nullptr) {
                preparedMembers->inUse = false;
                if (!preparedMembers->held) delete preparedMembers;
            }
        }
        std::unique_ptr<LaneGroupMembers> members;
        LaneGroupMembers* preparedMembers = nullptr;
        TimelineArmTable::OriginalGroupIdentity originalIdentity{};
        LaneGroupMembers* MemberStorage() const noexcept {
            return preparedMembers != nullptr ? preparedMembers : members.get();
        }
        TimelineArmToken* MemberTokens() const noexcept {
            const auto* storage = MemberStorage();
            return storage == nullptr ? nullptr : storage->tokens.get();
        }
        TimelineArmTable::OriginalGroupMember* MemberOriginals() const noexcept {
            const auto* storage = MemberStorage();
            return storage == nullptr ? nullptr : storage->originals.get();
        }
        std::uint16_t* MemberIndices() const noexcept {
            const auto* storage = MemberStorage();
            return storage == nullptr ? nullptr : storage->indices.get();
        }
        InitialSoundQueueCompanionAssociation initialSoundQueueCompanion{};
        // Logic-thread-only association and exclusion for the original transfer.
        PreparedPrivateImmediateLaneGroup* transferOriginal = nullptr;
        PrivateImmediateLaneGroupTransferProof* transferProof = nullptr;
        PrivateOriginalQueueGroupClockProof* clockProof = nullptr;
        PrivateImmediateLaneGroupsPublicationProof* publicationProof = nullptr;
        std::uint64_t originalFactoryMarker = 0;
        std::uint32_t originalRegistrationPinCount = 0;
        std::atomic<LaneGroupState> state{LaneGroupState::Free};
        std::uint64_t cohortRoot = 0;
        std::uint64_t selectedFutureDomain = 0;
        const ImmediateActionLane* laneOwner = nullptr;
        std::uint32_t laneSlot = UINT32_MAX;
        std::uint32_t laneGeneration = 0;
        std::uint32_t bucketOrdinal = UINT32_MAX;
        std::uint32_t groupIndex = UINT32_MAX;
        std::uint64_t relativeFrame = 0;
        std::uint64_t rebasedFrame = 0;
        bool frameRebased = false;
        std::uint64_t dueQuantum = 0;
        std::uint32_t generation = 0;
        DecisionToken decision{};
        std::uint16_t sliceIndex = 0;
        std::uint16_t memberCount = 0;
        std::uint16_t sourceControlCount = 0;
        FrontierGroupBinding frontier{};
        bool frontierPinsOwned = false;
        std::uint32_t frontierDependents = 0;
        AudioSliceLease lease{};
        Plan03GroupReaderState readers{};
    };
    static bool CanReuseImmediateLaneRow(const LaneGroupRow& row) noexcept;
    bool PrepareImmediateLaneMembers(LaneGroupRow& row, std::uint16_t count) noexcept;
    void ReturnPreparedImmediateLaneMembers(LaneGroupRow& row) noexcept;
    std::unique_ptr<LaneGroupMembers> CreateImmediateLaneMembers(std::uint16_t count) noexcept;
    friend class ImmediateActionLane;
    // Producer-only enumeration of the actual selected-domain Lane binding.
    // It does not issue a lifetime, admission, selector or audio read permission.
    std::size_t CapturePrivateSelectedLaneGroupsForPinning(
        const ImmediateActionLane& lane, ImmediateLaneHandle handle,
        std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
        std::size_t outCapacity) const noexcept;
    bool ResolveImmediateLaneGroup(const Plan03GroupSliceIdentity& identity,
        Plan03PageGroupView* out, bool audio) const noexcept;
    std::size_t CaptureImmediateLaneCohortGroupsImpl(
        std::uint64_t cohortRoot, Plan03ImmediateLaneGroupDescriptor* out,
        std::size_t outCapacity, bool includePublished) const noexcept;
    AudioSliceLease* EffectiveImmediateLaneGroupLease(std::size_t row) const noexcept;
    bool ReleaseImmediateLaneGroupReader(Plan03GroupReaderPin* pin,
        std::uint64_t completedAudioSequence) noexcept;
    // A filed transaction. remainingBatches falls as its pages leave the window;
    // the record is reclaimed when it reaches zero. The consume cache turns the
    // stale check into one validation per transaction per ConsumePage call.
    struct TransactionRecord {
        bool used = false;
        bool prepared = false;
        std::uint32_t preparedClaimSlot = UINT32_MAX;
        std::uint32_t preparedClaimGeneration = 0;
        std::uint32_t preparedRequestIndex = 0;
        TransactionId id = 0;
        std::uint64_t commitSerial = 0;
        TransportEpoch epoch = kInvalidTransportEpoch;
        PlayableInstanceHandle ownerInstance = 0;
        std::uint64_t ownerScheduleEpoch = 0;
        TransactionOccurrenceRef occurrence{};
        TimelineTransactionProgramRef program{};
        TimelineArmToken armToken{};
        ImmediateEntryAttemptRef immediateEntry{};
        std::uint64_t firstFrame = 0;
        std::uint64_t lastFrame = 0;
        // Events still filed in live pages. Falls as pages leave the window; the
        // record is reclaimed when it reaches zero, so no live event row ever
        // points at a freed slot.
        std::uint32_t remainingEvents = 0;
        ReservationSummary summary{};
        std::uint64_t cacheTick = 0;
        bool cacheValid = false;
    };

    // One filed event, living in its page. txnSlot links it back for the stale
    // check and reclaim; rowSequence is the final tiebreak that makes the sort a
    // strict total order even if two events share all six semantic keys.
    struct EventRow {
        std::uint64_t executeFrame = 0;
        LogicCommitSequence logicCommitSequence = 0;
        std::uint32_t executionOrdinal = 0;
        std::uint32_t eventIndex = 0;
        std::uint32_t transactionOrdinal = 0;
        std::uint32_t subeventPhase = 0;
        std::uint64_t payload = 0;
        bool authoredFadeOutCandidate = false;
        TimelineArmToken armToken{};
        TransactionId transactionId = 0;
        std::uint32_t txnSlot = 0;
        std::uint64_t rowSequence = 0;
        bool frozenTxnValid = false;
        std::uint32_t preparedEventOrdinal = 0;
        FrontierEventBinding frontier{};
    };

    struct FrameBatchRow {
        std::uint64_t frame = 0;
        std::uint32_t liveReferences = 0;
        std::uint32_t preparedReferences = 0;
    };

    struct PreparedTransactionClaim {
        PreparedTransactionBatch* transferOriginal = nullptr;
        PreparedPageBatchTransferProof* transferProof = nullptr;
        PreparedPageGroupTransferScope transferScope{};
        std::array<std::uint32_t, 5> transferTransactionSlots{};
        std::array<std::uint64_t, 5> transferRowSequences{};
        std::array<TimelineArmToken, 5> transferRequestArms{};
        std::uint32_t transferRing = UINT32_MAX;
        std::uint64_t transferAbsoluteQuantum = 0;
        std::uint64_t transferPageGeneration = 0;
        TransportEpoch transferTransport = kInvalidTransportEpoch;
        std::uint64_t transferFrame = 0;
        bool used = false;
        bool committed = false;
        std::uint32_t generation = 1;
        std::uint32_t transactionCount = 0;
        LogicCommitSequence frozenSequence = 0;
        std::uint64_t preparedIngressQuantum = 0;
        std::uint64_t committedIngressQuantum = 0;
    };

    bool CanChargePreparedTransactionIngress(const PreparedTransactionClaim& claim) const noexcept;

    struct CurrentSliceRow {
        std::uint64_t executeFrame = 0;
        LogicCommitSequence logicCommitSequence = 0;
        CurrentSliceKind kind = CurrentSliceKind::VariableBankCut;
        CurrentSliceRef slice{};
        std::uint64_t rowSequence = 0;
        std::uint32_t executionOrdinal = 0;
        bool publicationHeld = false;
        std::size_t next = SIZE_MAX;
        std::size_t previous = SIZE_MAX;
    };

    struct CurrentSlicePage {
        std::size_t first = SIZE_MAX;
        std::size_t last = SIZE_MAX;
        std::uint32_t nativeCount = 0;
    };

    friend class CurrentSliceBacking;
    void ReleaseCurrentSliceBacking(CurrentSliceBacking& backing) noexcept;
    bool PrepareCurrentSliceRows() noexcept;
    void ReleaseCurrentSliceRow(std::size_t slot) noexcept;
    void RecycleCurrentSlicePage(std::uint32_t ring) noexcept;
    void SortCurrentSlices(std::uint32_t ring) noexcept;
    ReserveResult ReserveCurrentSliceImpl(
        const ScheduledCurrentSliceInput& input, PrePublishLatePolicy latePolicy,
        std::uint64_t publicationFrontier, HeldCurrentSliceToken* held,
        CurrentSliceBacking* backing = nullptr);
    ReserveResult PlanCurrentSlicePlacement(const ScheduledCurrentSliceInput& input,
        PrePublishLatePolicy latePolicy, std::uint64_t publicationFrontier,
        bool publicationHeld) const noexcept;
    bool LocateHeldCurrentSlice(const HeldCurrentSliceToken& token,
        std::uint32_t* ring, std::size_t* index) const noexcept;
    bool HasHeldCurrentSlices() const noexcept;

    struct Plan03GroupSliceRow {
        UnpublishedGroupArmCancellationProof* jointCancellation = nullptr;
        CommittedPlan03GroupSlice* cancellationOriginal = nullptr;
        PrivateUnboundPageArmRetirementProof* privateUnboundArmRetirement = nullptr;
        PreparedPageFrontierBindingsProof* frontierBindings = nullptr;
        // Factory-issued association and immutable scope for selected original groups.
        PreparedPlan03GroupSlice* transferOriginal = nullptr;
        PreparedPageGroupTransferProof* transferProof = nullptr;
        Plan03GroupSliceIdentity transferPreparedIdentity{};
        Plan03GroupSliceIdentity transferProspectiveIdentity{};
        PreparedPageGroupTransferScope transferScope{};
        std::uint64_t transferExecuteFrame = 0;
        const TimelineArmTable* transferArmTable = nullptr;
        std::uint64_t transferArmTableIncarnation = 0;
        TimelineArmToken transferMember{};
        std::uint16_t transferMemberIndex = 0;
        DecisionToken decision{};
        std::uint16_t sliceIndex = 0;
        std::uint16_t memberCount = 0;
        std::uint16_t sourceControlCount = 0;
        ImmediateEntryAttemptRef immediateEntry{};
        FrontierGroupBinding frontier{};
        std::uint32_t frontierDependents = 0;
        bool frontierPinsOwned = false;
        bool publicationHeld = false;
        std::uint8_t unpublishedRetirementStage = 0;
        Plan03GroupSliceIdentity unpublishedRetirementMember{};
        FrontierEntryReceiptRef unpublishedRetirementEntry{};
        std::uint64_t unpublishedRetirementArmTableIncarnation = 0;
    };

    enum class Plan03GroupRowState : std::uint8_t {
        Free = 0,
        Prepared = 1,
        Live = 2,
        Retired = 3,
    };

    struct NativeGroupRow {
        Plan03GroupSliceRow value{};
        mutable AudioSliceLease lease{};
        Plan03GroupReaderState readers{};
        TimelineArmTable::OriginalGroupIdentity identity{};
        PageGroupMemberSlot members{};
        std::size_t nextFree = SIZE_MAX;
    };

    bool HasPageGroupStorage(std::size_t index) const noexcept {
        return index < pageGroupSlots_.size() && pageGroupSlots_[index] != SIZE_MAX;
    }
    NativeGroupRow& PageGroupData(std::size_t index) noexcept {
        assert(HasPageGroupStorage(index));
        return nativeGroupRows_[pageGroupSlots_[index]];
    }
    const NativeGroupRow& PageGroupData(std::size_t index) const noexcept {
        assert(HasPageGroupStorage(index));
        return nativeGroupRows_[pageGroupSlots_[index]];
    }
    void AcquirePageGroup(std::size_t index) noexcept;
    void ReleasePageGroup(std::size_t index) noexcept;

    EventRow& PageEvent(std::size_t index) noexcept {
        return nativeEventRows_[pageEventSlots_[index]];
    }
    const EventRow& PageEvent(std::size_t index) const noexcept {
        return nativeEventRows_[pageEventSlots_[index]];
    }
    void AcquirePageEvent(std::size_t index) noexcept;
    void ReleasePageEvent(std::size_t index) noexcept;
    std::uint32_t RingIndex(std::uint32_t page) const;
    AudioSliceLease* EffectivePlan03GroupLease(std::size_t rowIndex) const noexcept;
    // Sorts one page's live event indices into the total order, in place.
    void SortPage(std::uint32_t ringIndex);
    // Validates one transaction record against the current stamps, memoized for
    // the duration of a single ConsumePage call via cacheTick (which is why the
    // record is taken mutably even though the pool's own state is not changed).
    bool TransactionValid(TransactionRecord& record, TransportEpoch currentEpoch,
                          const PlayableInstanceTable& instances,
                          const OccurrenceStatePool& occurrences) const;

    Config config_{};
    mutable PendingEventStorageRegistration* pendingEventStorage_ = nullptr;
    mutable CurrentSliceStorageRegistration* currentSliceStorage_ = nullptr;
    std::atomic<std::uint64_t> retiredPageEvents_{0};
    std::atomic<std::uint64_t> retiredPageGroups_{0};
    void* immediateLaneGroupDisposalContext_ = nullptr;
    ImmediateLaneGroupDisposalObserver immediateLaneGroupDisposalObserver_ = nullptr;
    std::uint32_t pageCount_ = 0;
    std::uint32_t quantumFrames_ = 0;
    TransportEpoch epoch_ = kInvalidTransportEpoch;
    std::uint64_t baseFrame_ = 0;
    std::uint64_t baseQuantumIndex_ = 0;
    std::uint64_t sessionBaseQuantumIndex_ = 0;
    std::uint32_t ringStart_ = 0;
    std::uint64_t publishCutoffFrame_ = 0;
    std::uint64_t nextRowSequence_ = 1;
    std::uint64_t nextCurrentSliceSequence_ = 1;
    std::uint32_t currentSliceStorageCapacity_ = 0;
    std::uint64_t currentSliceIncarnation_ = 0;
    std::uint64_t nextCommitSerial_ = 1;
    std::uint64_t consumeTick_ = 0;

    // Per-page storage, indexed by ring index. Events and batch frames are packed
    // into stable per-page spans.
    std::vector<HorizonPageHeader> pageHeaders_;
    std::vector<std::uint32_t> pageEventCount_;
    std::vector<std::uint32_t> pagePreparedEventCount_;
    std::vector<std::uint64_t> pagePublicationHolds_;
    std::vector<std::uint8_t> pageSorted_; // 0/1: event order is current
    std::unique_ptr<std::atomic<std::uint64_t>[]> pageQuantumStamp_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> pageEmptyQuantumStamp_;
    StableRowStorage<EventRow, std::size_t> nativeEventRows_;
    StableRowStorage<std::size_t, std::size_t> pageEventSlots_;
    StableRowStorage<std::size_t, std::size_t> pageEventOrder_;
    StableRowStorage<std::size_t, std::size_t> freeEventSlots_;
    std::size_t freeEventSlotCount_ = 0;
    std::size_t issuedEventSlots_ = 0;
    std::vector<std::uint32_t> pageCurrentSliceCount_;
    std::unique_ptr<CurrentSlicePage[]> pageCurrentSliceStorage_;
    StableRowStorage<CurrentSliceRow, std::size_t> currentSliceRows_;
    StableRowStorage<std::size_t> currentSliceOrder_;
    std::size_t currentSliceBackingHolds_ = 0;
    std::size_t occupiedCurrentSliceRows_ = 0;
    std::size_t issuedCurrentSliceRows_ = 0;
    std::size_t freeCurrentSliceRow_ = SIZE_MAX;
    std::vector<std::uint32_t> pagePlan03GroupCount_;
    std::vector<std::uint32_t> pagePreparedPlan03GroupCount_;
    bool IssueGroupMemberOriginals(const Plan03GroupSliceIdentity& group,
        const TimelineArmTable& table, const TimelineArmToken* members,
        std::uint16_t count, TimelineArmTable::OriginalGroupMember* originals,
        TimelineArmTable::OriginalGroupIdentity& identity) noexcept;
    void ReturnGroupMemberOriginals(TimelineArmTable::OriginalGroupMember* originals,
        std::size_t count) noexcept;
    void ReturnAllGroupMemberOriginals() noexcept;
    friend struct detail::OriginalGroupMemberTestAccess;
    TimelineArmTable::GroupMemberReceiver groupMemberReceiver_{};
    std::size_t pageMemberBytes_ = 0;
    StableRowStorage<NativeGroupRow, std::size_t> nativeGroupRows_;
    std::vector<std::size_t> pageGroupSlots_;
    std::size_t issuedGroupSlots_ = 0;
    std::size_t occupiedGroupSlots_ = 0;
    std::size_t freeGroupSlot_ = SIZE_MAX;
    std::unique_ptr<TimelineArmTable::OriginalGroupMember[]> replacementGroupMemberOriginals_;
    TimelineArmTable::OriginalGroupIdentity replacementGroupIdentity_{};
    std::vector<Plan03GroupRowState> pagePlan03GroupRowStates_;
    std::vector<std::uint32_t> pagePlan03GroupClaimGenerations_;
    std::uint64_t nextOriginalLaneGroupFactoryMarker_ = 0;
    std::size_t immediateMemberBytes_ = 0;
    std::uint32_t immediateBackingHolds_ = 0;
    std::uint16_t immediateMemberAllocationLimitForTests_ = UINT16_MAX;
    ImmediateLaneBackingPreparation* activeImmediateLaneBacking_ = nullptr;
    void* immediateLaneRowStorageContext_ = nullptr;
    bool (*prepareImmediateLaneRowStorage_)(void*, std::uint32_t) noexcept = nullptr;
    StableRowStorage<LaneGroupRow> immediateLaneGroups_;
    std::unique_ptr<LaneFrontierGate[]> immediateLaneOwnerGates_;
    std::unique_ptr<LaneFrontierGate[]> immediateLaneDecisionGates_;
    const TimelineArmTable* plan03ArmTable_ = nullptr;
    bool plan03ArmTableEstablishedByPrepared_ = false;
    std::vector<std::uint32_t> pageBatchCount_;
    std::vector<std::uint32_t> pageBatchOccupiedCount_;
    StableRowStorage<FrameBatchRow, std::size_t> pageBatchRows_;

    std::vector<TransactionRecord> transactions_;
    std::vector<std::uint32_t> freeTransactions_; // stack of free slot indices
    std::vector<PreparedTransactionClaim> preparedTransactionClaims_;
    std::vector<std::uint32_t> freePreparedTransactionClaims_;

    struct UnplacedCreditRow {
        UnplacedBatchCreditRequest request{};
        std::unique_ptr<PageGroupMemberBacking> memberBacking;
        std::uint64_t generation = 0;
        TransportEpoch epoch = kInvalidTransportEpoch;
        std::uint64_t chargedQuantum = UINT64_MAX;
        bool active = false;
        bool materializing = false;
    };
    StableRowStorage<UnplacedCreditRow, std::size_t> unplacedCredits_;
    StableRowStorage<PreparedPlan03GroupSlice, std::size_t> privatePreparedGroups_;
    StableRowStorage<CommittedPlan03GroupSlice, std::size_t> privateCommittedGroups_;
    StableRowStorage<UnplacedBatchCredit*, std::size_t> privateCohortCredits_;
    StableRowStorage<TransactionRequest, std::size_t> privateCohortRequests_;
    StableRowStorage<Plan03GroupSliceInput, std::size_t> privateCohortGroups_;
    bool privateMaterializationActive_ = false;
    std::uint64_t nextUnplacedCreditGeneration_ = 1;
    std::uint64_t unplacedTransactions_ = 0;
    std::uint64_t unplacedPreparedClaims_ = 0;
    std::uint64_t unplacedEvents_ = 0;
    std::uint64_t unplacedFrameBatches_ = 0;
    std::uint64_t unplacedGroupHeaders_ = 0;
    std::uint32_t unplacedIngressThisQuantum_ = 0;
    std::uint32_t privateMaterializationFaultStepForTest_ = 0;

    // Reserve-time scratch (sized at Reset to maxTransactionEvents), so the write
    // path allocates nothing.
    mutable std::vector<std::uint32_t> scratchEventPage_;
    mutable std::vector<std::uint64_t> scratchEventFrame_;

    std::uint32_t liveTransactionCount_ = 0;
    std::uint32_t transactionHighWater_ = 0;
    std::uint32_t frameBatchHighWater_ = 0;
    std::uint32_t eventHighWater_ = 0;
    std::uint32_t transactionEventsHighWater_ = 0;
    std::uint64_t transactionSpanHighWater_ = 0;
    std::uint32_t ingressThisQuantum_ = 0;
    std::uint32_t preparedIngressThisQuantum_ = 0;
    std::uint32_t preparedTransactionClaimCount_ = 0;
    std::uint32_t ingressHighWater_ = 0;
    std::uint64_t droppedLateCount_ = 0;
    std::uint64_t advanceBlockedByConsumer_ = 0;
    std::vector<std::uint64_t> refusalCounts_;
    std::unique_ptr<std::atomic<std::uint64_t>> consumedFrontierQuantum_;
    bool consumerFrontierEnabled_ = false;

    std::atomic<std::uint8_t> terminalState_{1};
};

} // namespace harken

#endif // HARKEN_CORE_SCHEDULED_TRANSACTION_POOL_H
