#ifndef HARKEN_CORE_IMMEDIATE_ACTION_LANE_H
#define HARKEN_CORE_IMMEDIATE_ACTION_LANE_H

#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/AuthoredStopFence.h"
#include "harken/core/EngineConfig.h"

#include <atomic>
#include <bitset>
#include <cassert>
#include <utility>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace harken {
class OriginalImmediateLaneConditionalProjection;
class OriginalConditionalReplacementBatch;
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace detail { struct OriginalConditionalWithdrawalNegativeTestAccess; }
#endif

enum class ImmediateLaneBucketState : std::uint8_t {
    Empty, Prepared, Committed, AudioClaimed, Applied, Cancelled, Missed,
    CancelledOnly
};

enum class ImmediateLaneAttemptState : std::uint8_t {
    Free, Reserved, Prepared, Committed, Cancelling, PrivateCancelled,
    PrivateLayoutHeld
};

struct ImmediateLaneHandle {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    bool Valid() const noexcept { return slot != UINT32_MAX && generation != 0; }
};

class ImmediateLaneBucketClaimReceipt {
public:
    ImmediateLaneBucketClaimReceipt() noexcept = default;
    bool Valid() const noexcept {
        return slot_ != UINT32_MAX && generation_ != 0 &&
            bucketOrdinal_ != UINT32_MAX;
    }
    std::uint32_t Slot() const noexcept { return slot_; }
    std::uint32_t Generation() const noexcept { return generation_; }
    std::uint32_t BucketOrdinal() const noexcept { return bucketOrdinal_; }
    std::uint64_t Quantum() const noexcept { return quantum_; }

private:
    friend class ImmediateActionLane;
    ImmediateLaneBucketClaimReceipt(std::uint32_t slot,
                                    std::uint32_t generation,
                                    std::uint32_t bucketOrdinal,
                                    std::uint64_t quantum) noexcept
        : slot_(slot), generation_(generation),
          bucketOrdinal_(bucketOrdinal), quantum_(quantum) {}
    std::uint32_t slot_ = UINT32_MAX;
    std::uint32_t generation_ = 0;
    std::uint32_t bucketOrdinal_ = UINT32_MAX;
    std::uint64_t quantum_ = 0;
};

struct ImmediateLaneObservation {
    bool committed = false;
    bool applied = false;
    bool missed = false;
    bool cancelled = false;
    bool claimed = false;
    bool complete = false;
    bool arenaCapacityMissed = false;
    bool protocolRefused = false;
    bool cancelledOnly = false;
};

enum class ImmediateLaneEntryOutcome : std::uint8_t {
    Dormant, Admitting, Withdrawing, Applied, Withdrawn, NotApplied
};
static_assert(std::atomic<ImmediateLaneEntryOutcome>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

struct ImmediateLaneEntryOutcomeSnapshot {
    ImmediateLaneEntryOutcome outcome = ImmediateLaneEntryOutcome::Dormant;
    std::uint64_t audioSequence = 0;
    std::uint32_t bucketOrdinal = UINT32_MAX;
    bool operator==(const ImmediateLaneEntryOutcomeSnapshot&) const = default;
};

struct ImmediateLaneValidationRefusalSnapshot {
    std::uint64_t root = 0;
    std::uint32_t bucketOrdinal = UINT32_MAX;
    std::uint32_t firstGroup = 0;
    std::uint32_t groupCount = 0;
    std::uint64_t audioSequence = 0;
    std::uint64_t dueQuantum = 0;
    std::uint64_t sourceWaitSequence = 0;
};

enum class ImmediateLaneAdmissionFailure : std::uint8_t {
    None = 0,
    ArenaCapacity = 1,
    FrontierValidation = 2,
};
static_assert(std::atomic<ImmediateLaneAdmissionFailure>::is_always_lock_free);

struct ImmediateLaneDemand {
    std::uint32_t events = 0;
    std::uint32_t groups = 0;
    std::uint32_t members = 0;
    std::uint32_t buckets = 0;
};

enum class ImmediateLaneTimeDomain : std::uint8_t {
    RelativeToAnchor = 0, FixedAbsoluteFrame = 1
};

struct ImmediateLaneEvent {
    std::uint64_t relativeFrame = 0;
    ProgramGeneration armProgramGeneration = 0;
    bool futureGroupedStart = false;
    ConsumedEvent event{};
    ImmediateLaneTimeDomain timeDomain = ImmediateLaneTimeDomain::RelativeToAnchor;
    std::uint64_t fixedFrame = 0;
};

// Scheduling originals only; these copies do not own arms or payload backing.
struct PlainAuthoredControlLayoutInput {
    ImmediateLaneEvent start{};
    ImmediateLaneEvent envelope{};
    ImmediateLaneEvent stop{};
    std::uint64_t stopFrame = 0;
    std::uint64_t fadeStartFrame = 0;
};

using ImmediateLaneGroupRef = Plan03ImmediateLaneGroupDescriptor;

struct ImmediateLaneDeclaredGroup {
    Plan03GroupSliceIdentity identity{};
    std::uint64_t relativeFrame = 0;
    ImmediateLaneTimeDomain timeDomain = ImmediateLaneTimeDomain::RelativeToAnchor;
    std::uint64_t fixedFrame = 0;
};

enum class ImmediateLaneMemberKind : std::uint8_t {
    SingleEvent = 0,
    CurrentEventSpan,
    FrozenFutureTarget,
    HistoricalSourceCompletion,
};

struct ImmediateLaneMemberManifest {
    Plan03GroupSliceIdentity group{};
    Plan03GroupSliceIdentity relatedGroup{};
    TimelineArmToken arm{};
    std::uint32_t eventOrdinal = UINT32_MAX;
    std::uint16_t eventSpanCount = 1;
    std::uint16_t memberIndex = UINT16_MAX;
    ImmediateLaneMemberKind kind = ImmediateLaneMemberKind::SingleEvent;
    bool future = false;
    std::uint64_t relativeFrame = 0;
    ProgramGeneration programGeneration = 0;
    TimelineSemanticTarget target{};
    PlayableInstanceHandle ownerInstance = 0;
    std::uint64_t ownerEpoch = 0;
    ImmediateLaneTimeDomain timeDomain = ImmediateLaneTimeDomain::RelativeToAnchor;
    std::uint64_t fixedFrame = 0;
    std::uint32_t secondEventOrdinal = UINT32_MAX;
};

// Borrowed original factory inputs, valid only for private declaration checks.
// This association does not transfer any resource or issue an audio receipt.
struct ImmediateLanePrivateVariableControlEntryScope {
    FrontierEntryReceiptRef entry{};
    DecisionReserveRequest coordinatorRequest{};
    std::uint64_t provisionalFrame = 0;
    std::uint32_t firstMember = UINT32_MAX;
    std::uint32_t memberCount = 0;
    const ImmediateLaneMemberManifest* members = nullptr;
    std::array<Plan03GroupSliceInput, 3> groups{};
};

// Accepted factories remain owned by their original producers. Declaration
// borrows their inputs and records which outer Play owns each native group.
struct ImmediateLaneAcceptedPrivateGroup {
    const OriginalQueueFutureGroupRegistration* original = nullptr;
    OriginalQueueFutureGroupScope scope{};
    ImmediateLaneDeclaredGroup schedule{};
};

struct ImmediateLaneAcceptedPrivateEntryScope {
    FrontierEntryReceiptRef entry{};
    DecisionReserveRequest coordinatorRequest{};
    std::uint64_t provisionalFrame = 0;
    std::uint32_t firstMember = UINT32_MAX;
    std::uint32_t memberCount = 0;
    const ImmediateLaneMemberManifest* members = nullptr;
    std::array<Plan03GroupSliceIdentity, 3> outerGroups{};
    const ImmediateLaneAcceptedPrivateGroup* groups = nullptr;
    std::uint32_t groupCount = 0;
};

// Indices into the original immutable lane arrays, not resource ownership.
struct ImmediateLanePrivateVariableControlEntryMapping {
    std::array<std::uint32_t, 3> groups{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::array<std::uint32_t, 3> events{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::uint32_t firstMember = UINT32_MAX;
    std::uint32_t memberCount = 0;
    std::uint32_t coordinatorBucket = UINT32_MAX;
    std::uint32_t ownedGroupCount = 0;
    std::uint32_t ownedEventCount = 0;
    bool operator==(const ImmediateLanePrivateVariableControlEntryMapping&) const = default;
};

struct ImmediateLanePrivateEntryBucketShape {
    std::array<std::uint32_t, kMaxKnownScheduleLeaves + 3u> ordinals{};
    std::uint32_t count = 0;
    std::uint32_t coordinator = UINT32_MAX;
    bool operator==(const ImmediateLanePrivateEntryBucketShape&) const = default;
};

// Omitted cells were excluded by a real whole terminal without audio capture.
// Their Dormant outcome and zero sequence are not an audio acknowledgement.
struct ImmediateLanePrivateEntryTerminalProjection {
    ImmediateLanePrivateEntryBucketShape shape{};
    std::array<ImmediateLaneEntryOutcomeSnapshot, kMaxKnownScheduleLeaves + 3u> outcomes{};
    std::uint32_t registeredEntryCount = 0;
    std::bitset<kMaxKnownScheduleLeaves + 3u> omitted{};
    bool operator==(const ImmediateLanePrivateEntryTerminalProjection&) const = default;
};

struct ImmediateLanePrivateEntryResultCell {
    std::uint32_t bucketOrdinal = UINT32_MAX;
    ImmediateLaneEntryOutcomeSnapshot outcome{};
    bool omitted = false;
};

// Caller-owned stable storage, never a pointer into the reusable lane arena.
struct ImmediateLanePrivateEntryResultView {
    ImmediateLanePrivateEntryResultCell* cells = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t count = 0;
    std::uint32_t coordinator = UINT32_MAX;
    std::uint32_t registeredEntryCount = 0;
};

struct ImmediateLaneBucket {
    std::atomic<ImmediateLaneBucketState> state{ImmediateLaneBucketState::Empty};
    std::atomic<ImmediateLaneAdmissionFailure> admissionFailure{
        ImmediateLaneAdmissionFailure::None};
    std::uint32_t attemptGeneration = 0;
    std::uint64_t quantum = 0;
    std::uint32_t firstEvent = 0;
    std::uint32_t eventCount = 0;
    std::uint32_t firstGroup = 0;
    std::uint32_t groupCount = 0;
    std::uint32_t memberCount = 0;
    std::atomic<std::uint64_t> sourceWaitSequence{0};
    std::atomic<std::uint64_t> completedAudioSequence{0};
};

class ImmediateActionLane;
class PrivatePlainLayoutCommitProof;

class PrivatePlainLayoutEdit {
public:
    PrivatePlainLayoutEdit() noexcept = default;
    ~PrivatePlainLayoutEdit();
    PrivatePlainLayoutEdit(const PrivatePlainLayoutEdit&) = delete;
    PrivatePlainLayoutEdit& operator=(const PrivatePlainLayoutEdit&) = delete;
    PrivatePlainLayoutEdit(PrivatePlainLayoutEdit&& other) noexcept;
    PrivatePlainLayoutEdit& operator=(PrivatePlainLayoutEdit&& other) noexcept;
    bool Valid() const noexcept { return owner_ != nullptr; }
    void ClearCandidate() noexcept;
    bool StageCandidate(const ImmediateLaneEvent* events, std::uint32_t count,
        std::uint64_t anchorFrame, std::uint64_t firstSafeFrame) noexcept;
    bool StageAuthoredControlCandidate(std::uint64_t anchorFrame,
        std::uint64_t firstSafeFrame) noexcept;
    bool SealCandidate(PrivatePlainLayoutCommitProof* out) noexcept;
    void Abort() noexcept;

private:
    friend class ImmediateActionLane;
    friend class PrivatePlainLayoutCommitProof;
    bool StageFixedCandidate(const ImmediateLaneEvent* events, std::uint32_t count,
        std::uint64_t anchorFrame, std::uint64_t firstSafeFrame) noexcept;
    struct BucketSnapshot {
        ImmediateLaneBucketState state = ImmediateLaneBucketState::Empty;
        ImmediateLaneAdmissionFailure failure = ImmediateLaneAdmissionFailure::None;
        std::uint32_t generation = 0;
        std::uint64_t quantum = 0;
        std::uint32_t firstEvent = 0, eventCount = 0;
        std::uint32_t firstGroup = 0, groupCount = 0, memberCount = 0;
    };
    ImmediateActionLane* owner_ = nullptr;
    ImmediateLaneHandle handle_{};
    ImmediateLaneAttemptState originalState_ = ImmediateLaneAttemptState::Free;
    std::uint32_t originalEventCount_ = 0, originalBucketCount_ = 0;
    std::uint32_t originalNextBucket_ = 0;
    std::uint64_t originalAnchor_ = 0;
    std::array<ImmediateLaneEvent, 3> originalEvents_{};
    std::array<BucketSnapshot, 3> originalBuckets_{};
    PlainAuthoredControlLayoutInput authoredLayout_{};
    bool hasAuthoredLayout_ = false;
    bool originalWasPristineReserved_ = false;
    bool ready_ = false;
};

class PrivatePlainLayoutCommitProof {
public:
    PrivatePlainLayoutCommitProof() noexcept = default;
    PrivatePlainLayoutCommitProof(const PrivatePlainLayoutCommitProof&) = delete;
    PrivatePlainLayoutCommitProof& operator=(const PrivatePlainLayoutCommitProof&) = delete;
    PrivatePlainLayoutCommitProof(PrivatePlainLayoutCommitProof&&) noexcept = default;
    PrivatePlainLayoutCommitProof& operator=(PrivatePlainLayoutCommitProof&&) noexcept = default;
    bool Valid() const noexcept { return edit_.Valid() && edit_.ready_; }
    void Consume() noexcept;
private:
    friend class ImmediateActionLane;
    friend class PrivatePlainLayoutEdit;
    PrivatePlainLayoutEdit edit_{};
};

class ImmediateActionLane;
struct ImmediateLaneAttempt;
// Keeps the existing placed Lane private until all root proofs can apply.
class PrivateImmediateLanePublicationProof {
public:
    PrivateImmediateLanePublicationProof() noexcept = default;
    PrivateImmediateLanePublicationProof(const PrivateImmediateLanePublicationProof&) = delete;
    PrivateImmediateLanePublicationProof& operator=(const PrivateImmediateLanePublicationProof&) = delete;
    PrivateImmediateLanePublicationProof(PrivateImmediateLanePublicationProof&& other) noexcept;
    PrivateImmediateLanePublicationProof& operator=(PrivateImmediateLanePublicationProof&& other) noexcept;
    ~PrivateImmediateLanePublicationProof();
    bool Valid() const noexcept { return owner_ != nullptr; }
    void Release() noexcept;
private:
    friend class ImmediateActionLane;
    friend class ScheduledTransactionPool;
    ImmediateActionLane* owner_ = nullptr;
    ImmediateLaneAttempt* attempt_ = nullptr;
    ImmediateLaneHandle handle_{};
    PrivateImmediateLaneGroupsPublicationProof* groupsPublication_ = nullptr;
};

// Issued only by actual successful TryPlace. This brief private anchor hold
// is not a Source/Current/Queue/group/publication/budget/audio capability.
class PrivateImmediateLanePlacementProof {
public:
    PrivateImmediateLanePlacementProof() noexcept = default;
    PrivateImmediateLanePlacementProof(const PrivateImmediateLanePlacementProof&) = delete;
    PrivateImmediateLanePlacementProof& operator=(const PrivateImmediateLanePlacementProof&) = delete;
    PrivateImmediateLanePlacementProof(PrivateImmediateLanePlacementProof&& other) noexcept;
    PrivateImmediateLanePlacementProof& operator=(PrivateImmediateLanePlacementProof&& other) noexcept;
    ~PrivateImmediateLanePlacementProof();
    bool Valid() const noexcept;
    bool CaptureAnchorFrame(std::uint64_t* out) const noexcept;
    void Release() noexcept;
private:
    friend class ImmediateActionLane;
    void Take(PrivateImmediateLanePlacementProof& other) noexcept;
    ImmediateActionLane* owner_ = nullptr;
    ImmediateLaneAttempt* attempt_ = nullptr;
    ImmediateLaneHandle handle_{};
    std::uint64_t root_ = 0;
    std::uint64_t anchor_ = 0;
};

struct ImmediateLaneAttempt {
    mutable std::atomic_flag readerGate = ATOMIC_FLAG_INIT;
    std::atomic<bool> captureActive{false};
    std::atomic<ImmediateLaneAttemptState> state{ImmediateLaneAttemptState::Free};
    std::uint64_t root = 0;
    std::uint32_t generation = 0;
    std::uint32_t eventCredit = 0;
    std::uint32_t groupCredit = 0;
    std::uint32_t memberCredit = 0;
    std::uint32_t bucketCredit = 0;
    std::uint32_t eventStorageCount = 0;
    std::uint32_t memberStorageCount = 0;
    std::uint32_t eventCount = 0;
    std::uint32_t groupCount = 0;
    std::uint32_t pinGroupManifestCount = 0;
    ScheduledTransactionPool* pinGroupPool = nullptr;
    std::uint32_t declaredGroupCount = 0;
    std::uint32_t memberCount = 0;
    std::uint32_t bucketCount = 0;
    std::uint32_t privateEntryCount = 0;
    std::uint32_t sourceWaitEntryCount = 0;
    std::uint64_t anchorFrame = 0;
    bool pinsInstalled = false;
    bool groupPinsInstalled = false;
    std::atomic<std::uint32_t> nextBucket{0};
    std::uint32_t capturedBucketOrdinal = UINT32_MAX;
    std::uint64_t capturedAudioSequence = 0;
    PrivatePlainLayoutEdit* privateLayoutEdit = nullptr;
    PrivateImmediateLanePlacementProof* privatePlacementProof = nullptr;
    PrivateImmediateLanePublicationProof* privatePublicationProof = nullptr;
    std::uint64_t originalLifetimeMarker = 0;
    std::uint32_t originalLifetimePins = 0;
    bool conditionalProjectionIssued = false;
    bool conditionalProjectionBound = false;
    bool conditionalProjectionCancelIssued = false;
    bool conditionalProjectionCancelled = false;
    bool conditionalProjectionReferencesReturned = false;
    std::array<Plan03GroupSliceIdentity, 2> conditionalProjectionGroups{};
    std::atomic<std::uint64_t> conditionalProjectionAdmission{0};
    std::atomic<std::uint64_t> conditionalProjectionLastUse{0};
    bool capturedConditionalProjection = false;
    OriginalConditionalReplacementBatch* conditionalReplacement = nullptr;
    std::uint64_t conditionalReplacementGeneration = 0;
    bool capturedConditionalReplacementRead = false;
    DecisionTable* conditionalProjectionDecisions = nullptr;
    FrontierEntryReceiptRef conditionalProjectionEntry{};
};

class OriginalImmediateLaneAttemptLifetimePin {
public:
    OriginalImmediateLaneAttemptLifetimePin() = default;
    OriginalImmediateLaneAttemptLifetimePin(const OriginalImmediateLaneAttemptLifetimePin&) = delete;
    OriginalImmediateLaneAttemptLifetimePin& operator=(const OriginalImmediateLaneAttemptLifetimePin&) = delete;
    OriginalImmediateLaneAttemptLifetimePin(OriginalImmediateLaneAttemptLifetimePin&& other) noexcept;
    OriginalImmediateLaneAttemptLifetimePin& operator=(OriginalImmediateLaneAttemptLifetimePin&& other) noexcept;
    ~OriginalImmediateLaneAttemptLifetimePin();
    bool Valid() const noexcept;
    bool Borrow(OriginalImmediateLaneAttemptLifetimePin* out) const noexcept;
    void Return() noexcept;
    ImmediateLaneHandle HandleObservation() const noexcept { return handle_; }
private:
    friend class ImmediateActionLane;
    friend class PrivateSelectedQueueFutureSpanPool;
    friend class OriginalImmediateLaneConditionalProjection;
    friend class OriginalConditionalReplacementBatch;
    ImmediateActionLane* lane_ = nullptr;
    ImmediateLaneHandle handle_{};
    std::uint64_t root_ = 0, marker_ = 0;
};

// Stable storage for initial Action cohort events. The logic producer prepares
// immutable descriptors, then publishes a complete bank with a release store.
// The audio reader scans one bucket head per attempt and never allocates.
class ImmediateActionLane {
public:
    ImmediateActionLane() noexcept = default;
    ~ImmediateActionLane();
    ImmediateActionLane(const ImmediateActionLane&) = delete;
    ImmediateActionLane& operator=(const ImmediateActionLane&) = delete;
    ImmediateActionLane(ImmediateActionLane&&) = delete;
    ImmediateActionLane& operator=(ImmediateActionLane&&) = delete;
    struct Config {
        std::uint32_t attempts = 0;
        std::uint32_t eventsPerAttempt = 0;
        std::uint32_t maxEventsPerQuantum = 0;
        std::uint32_t quantumFrames = 0;
        bool requireArmPins = false;
        std::uint32_t maxGroupsPerQuantum = 0;
    };

    bool Reset(Config config);
    bool PreparePrivateEntryBacking(std::uint32_t count,
        StableStoragePreparation* out) noexcept;
    bool PrepareAttemptBacking(StableStoragePreparation* out) noexcept;
    bool IsOriginalAttemptLifetimePinFor(
        const OriginalImmediateLaneAttemptLifetimePin& original,
        ImmediateLaneHandle handle, std::uint64_t root) const noexcept;
    // Observes storage-only retention after real unpublished cancellation.
    // It grants no cancellation, Current, Source or publication capability.
    bool ObserveOriginalUnpublishedStorageOnlyRetention(
        const OriginalImmediateLaneAttemptLifetimePin& baseline,
        ImmediateLaneHandle handle, std::uint64_t root,
        std::uint64_t completedAudioSequence) const noexcept;
    bool AcquirePrivatePlainLayoutEdit(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        PrivatePlainLayoutEdit* out) noexcept;
    bool AcquirePrivateReservedPlainLayoutEdit(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        PrivatePlainLayoutEdit* out) noexcept;
    bool AcquirePrivatePlainAuthoredControlLayoutEdit(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        const PlainAuthoredControlLayoutInput& input,
        PrivatePlainLayoutEdit* out) noexcept;
    bool Valid() const noexcept { return valid_; }
    std::uint32_t AttemptCapacity() const noexcept { return config_.attempts; }
    std::uint32_t EventsPerAttempt() const noexcept { return config_.eventsPerAttempt; }
    std::size_t PrivateEntryStorageBytes() const noexcept {
        return privateEntries_.RequestedBytes() +
            static_cast<std::size_t>(config_.attempts) * config_.eventsPerAttempt *
            (2u * sizeof(PrivateEntryBucket) + 4u * sizeof(std::uint32_t));
    }
    std::size_t PrivateEntryCommittedBytes() const noexcept { return privateEntries_.CommittedBytes(); }
    std::size_t PrivateEntryReservedBytes() const noexcept { return privateEntries_.ReservedBytes(); }
    std::size_t EventStorageBytes() const noexcept { return eventStorageBytes_; }
    std::size_t MemberStorageBytes() const noexcept { return memberStorageBytes_; }
    std::size_t PayloadCommittedBytes() const noexcept {
        return eventStorage_.CommittedBytes() + memberStorage_.CommittedBytes();
    }
    std::size_t PayloadReservedBytes() const noexcept {
        return eventStorage_.ReservedBytes() + memberStorage_.ReservedBytes();
    }
    std::size_t PayloadDirectoryBytes() const noexcept {
        return eventStorage_.DirectoryBytes() + memberStorage_.DirectoryBytes();
    }
#if !defined(HARKEN_RT_QUALIFICATION)
    void SetPrivateEntryCommitPageLimitForTests(std::size_t pages) noexcept {
        privateEntries_.SetCommitPageLimitForTests(pages);
    }
    void SetPayloadCommitPageLimitsForTests(std::size_t eventPages,
        std::size_t memberPages) noexcept {
        eventStorage_.SetCommitPageLimitForTests(eventPages);
        memberStorage_.SetCommitPageLimitForTests(memberPages);
    }
#endif
    std::uint32_t MaxEventsPerQuantum() const noexcept { return config_.maxEventsPerQuantum; }
    bool OwnsPrivate(ImmediateLaneHandle handle,
                     std::uint64_t root) const noexcept;
    bool MatchesReservedMembers(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        const ImmediateLaneMemberManifest* originalMembers,
        std::uint32_t memberCount) const noexcept;
    bool MatchesReservedMemberSpan(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        std::uint32_t firstMember,
        const ImmediateLaneMemberManifest* originalMembers,
        std::uint32_t memberCount) const noexcept;
    bool DeclarePrivateVariableControlEntry(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        const ImmediateLanePrivateVariableControlEntryScope& scope,
        const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
        const DecisionTable& decisions,
        const std::array<PreparedPrivateImmediateLaneGroup, 3>& originals,
        const std::array<PrivateImmediateLaneGroupTransferProof, 3>& proofs,
        std::uint32_t* outOrdinal) noexcept;
    bool MatchesPrivateVariableControlEntry(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        std::uint32_t ordinal,
        const ImmediateLanePrivateVariableControlEntryScope& scope,
        const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
        const DecisionTable& decisions,
        const std::array<PreparedPrivateImmediateLaneGroup, 3>& originals,
        const std::array<PrivateImmediateLaneGroupTransferProof, 3>& proofs) const noexcept;
    bool DeclareAcceptedPrivateVariableControlEntry(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        const ImmediateLaneAcceptedPrivateEntryScope& scope,
        const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
        const DecisionTable& decisions, std::uint32_t* outOrdinal) noexcept;
    bool OwnsNeverCommitted(ImmediateLaneHandle handle,
                             std::uint64_t root) const noexcept;
    bool HasDue(std::uint64_t quantum) const noexcept;
    bool Observe(ImmediateLaneHandle handle, ImmediateLaneObservation* out) const noexcept;
    bool CapturePublishedValidationRefusal(
        ImmediateLaneHandle handle, std::uint64_t completedAudioSequence,
        ImmediateLaneValidationRefusalSnapshot* out) const noexcept;
    bool CapturePublishedValidationRefusalGroup(
        ImmediateLaneHandle handle, std::uint64_t completedAudioSequence,
        std::uint32_t bucketOrdinal, std::uint32_t groupOffset,
        ImmediateLaneGroupRef* out) const noexcept;
    bool HasProgramReference(ProgramGeneration generation) const noexcept;
    bool HasProgramReference(TimelineTransactionProgramRef program) const noexcept;
    // Logic requests exact withdrawal; false retains any issued request for retry.
    bool WithdrawPlainNoneFence(ImmediateLaneHandle handle, std::uint64_t root,
        const TimelineTransactionProgramRef& program,
        const AuthoredStopFenceDescriptor& descriptor,
        std::uint64_t completedAudioSequence) noexcept;
    bool WithdrawPlainSourceStartReference(ImmediateLaneHandle handle, std::uint64_t root,
        const TimelineTransactionProgramRef& program, TimelineArmToken source,
        std::uint64_t completedAudioSequence) noexcept;
    bool WithdrawPlainAuthoredControlReferences(ImmediateLaneHandle handle, std::uint64_t root,
        const TimelineTransactionProgramRef& program, TimelineArmToken control,
        const AuthoredStopOperation& operation, std::uint64_t completedAudioSequence) noexcept;
#if !defined(HARKEN_RT_QUALIFICATION)
    void SetReserveGateHookForTest(void (*hook)(void*, std::uint32_t),
                                   void* context) noexcept {
        reserveGateHook_ = hook;
        reserveGateContext_ = context;
    }
    void SetFinishHeadHookForTest(void (*hook)(void*),
                                  void* context) noexcept {
        finishHeadHook_ = hook;
        finishHeadContext_ = context;
    }
    void SetCaptureGateHookForTest(void (*hook)(void*),
                                   void* context) noexcept {
        captureGateHook_ = hook;
        captureGateContext_ = context;
    }
    void SetEventAdmissionHookForTest(void (*hook)(void*, std::uint32_t),
                                      void* context) noexcept {
        eventAdmissionHook_ = hook;
        eventAdmissionContext_ = context;
    }
    // Observes only a successfully published native capture before finalization.
    void SetCapturedAdmissionHookForTest(void (*hook)(void*, std::uint32_t),
                                         void* context) noexcept {
        capturedAdmissionHook_ = hook;
        capturedAdmissionContext_ = context;
    }
    void SetValidationRefusalHookForTest(void (*hook)(void*),
                                       void* context) noexcept {
        validationRefusalHook_ = hook;
        validationRefusalContext_ = context;
    }
#endif

    // Logic thread. A reservation is part of Post's all-or-nothing resource
    // transaction; its descriptor credit remains occupied until retirement.
    bool Reserve(std::uint64_t root, std::uint32_t eventCredit,
                 ImmediateLaneHandle* out) noexcept;
    bool Reserve(std::uint64_t root, ImmediateLaneDemand demand,
                 ImmediateLaneHandle* out) noexcept;
    bool Reserve(std::uint64_t root, ImmediateLaneDemand demand,
        ImmediateLaneHandle* out, OriginalImmediateLaneAttemptLifetimePin* original) noexcept;
    bool DeclareMember(ImmediateLaneHandle handle,
                       const ImmediateLaneMemberManifest& member,
                       std::uint32_t* outOrdinal = nullptr) noexcept;
    bool DeclareGroup(ImmediateLaneHandle handle,
                      const ImmediateLaneDeclaredGroup& group) noexcept;
    bool Prepare(ImmediateLaneHandle handle,
                 const ImmediateLaneEvent* events,
                 std::uint32_t eventCount) noexcept;
    bool PinArms(ImmediateLaneHandle handle,
                  TimelineArmTable& arms) noexcept;
    bool PinGroups(ImmediateLaneHandle handle,
                   ScheduledTransactionPool& pool,
                   DecisionTable& decisions,
                   std::uint32_t maxGroupsPerQuantum) noexcept;
    std::uint32_t PrivatePinGroupCount(ImmediateLaneHandle handle) const noexcept;
    bool CapturePrivatePinGroup(ImmediateLaneHandle handle,
                                std::uint32_t index,
                                Plan03GroupSliceIdentity* out) const noexcept;
    // Logic producer observation after successful TryPlace and before Publish.
    // The complete installed event/group layout determines the horizon.
    bool CapturePrivatePlacedLastQuantum(ImmediateLaneHandle handle,
        std::uint64_t* out) const noexcept;
    bool CapturePrivatePendingVariableStartGeometry(const PrivatePlainLayoutCommitProof& proof,
        const TimelineArmTable& arms, TimelineArmToken arm, std::uint64_t authoredOffset,
        std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept;
    bool CapturePrivatePendingVariableStartGeometry(const PrivateImmediateLanePlacementProof& proof,
        const TimelineArmTable& arms, TimelineArmToken arm, std::uint64_t authoredOffset,
        std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept;
    bool CapturePrivatePreparedVariableStartGeometry(const PrivateImmediateLanePlacementProof& placement,
        const TimelineArmTable::PreparedCommitProof& arm, std::uint64_t authoredOffset,
        std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept;
    bool CapturePrivateUnboundVariableStartGeometry(const PrivateImmediateLanePlacementProof& placement,
        const TimelineArmTable& arms, TimelineArmToken arm, std::uint64_t authoredOffset,
        std::uint64_t* logicalFrame, std::uint64_t* onsetFrame) const noexcept;
    // Producer-only observation from the genuine held placement and installed
    // native readers. It does not issue Current/ACK or audio admission rights.
    bool CapturePrivatePlacedInitialSource(
        const PrivateImmediateLanePlacementProof& placement,
        const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
        const DecisionTable& decisions, const FrontierGroupBinding& initial,
        TimelineArmToken source, std::uint64_t* outFrame) const noexcept;
    bool CapturePrivateVariableControlEntryMapping(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& entry, const ScheduledTransactionPool& pool,
        const TimelineArmTable& arms, const DecisionTable& decisions,
        ImmediateLanePrivateVariableControlEntryMapping* out) const noexcept;
    bool RequestPrivateEntryStop(ImmediateLaneHandle handle, std::uint64_t root,
        std::uint32_t ordinal, const FrontierEntryReceiptRef& entry) noexcept;
    bool MatchesPrivateEntryAssociation(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& entry) const noexcept;
    bool CapturePrivateEntryBucketShape(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& entry,
        ImmediateLanePrivateEntryBucketShape* out) const noexcept;
    bool CapturePrivateEntryTerminalProjection(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
        ImmediateLanePrivateEntryTerminalProjection* out) const noexcept;
    bool CapturePrivateEntryBucketShape(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& identity,
        ImmediateLanePrivateEntryResultView* out) const noexcept;
    bool CapturePrivateEntryTerminalProjection(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& identity, std::uint64_t completedAudioSequence,
        ImmediateLanePrivateEntryResultView* out) const noexcept;
    bool CapturePrivateEntryOutcome(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& entry, std::uint32_t bucketOrdinal,
        std::uint64_t completedAudioSequence,
        ImmediateLaneEntryOutcomeSnapshot* out) const noexcept;
    bool CanCancelPrivateEntrySources(ImmediateLaneHandle handle,
        std::uint64_t root, std::uint32_t ordinal,
        const FrontierEntryReceiptRef& entry) const noexcept;
    bool ReleasePrivatePinGroupOwnership(ImmediateLaneHandle handle,
                                         ScheduledTransactionPool& pool) noexcept;
    // Return only this never-published, cancelled Lane's actual installed pins.
    // The full handle and group manifest remain owned until true Retire.
    bool PrivateInstalledInitialStartsRollbackShape(ImmediateLaneHandle handle,
        std::uint64_t root, TimelineArmTable& arms,
        ScheduledTransactionPool& pool) noexcept;
    bool ReleaseCancelledPrivateInitialPins(ImmediateLaneHandle handle,
        std::uint64_t root, TimelineArmTable& arms,
        ScheduledTransactionPool& pool, DecisionTable& decisions,
        std::uint64_t completedAudioSequence) noexcept;
    // The actual held Lane pin and private group establish the publication scope.
    bool PrivateInstalledInitialArmRollbackReady(ImmediateLaneHandle handle,
        std::uint64_t root, TimelineArmTable& arms,
        ScheduledTransactionPool& pool, TimelineArmToken arm,
        const FrontierGroupBinding& binding, AudioSliceLease& lease) noexcept;

    bool TryPlace(ImmediateLaneHandle handle, std::uint64_t earliestFrame,
                  std::uint32_t additionalQuanta,
                  std::uint64_t* outAnchorFrame) noexcept;
    // The original overload retains its existing call and behavior.
    bool TryPlace(ImmediateLaneHandle handle, std::uint64_t earliestFrame,
                  std::uint32_t additionalQuanta, std::uint64_t* outAnchorFrame,
                  PrivateImmediateLanePlacementProof* outPlacement) noexcept;
    bool IsPrivatePlacementProofFor(const PrivateImmediateLanePlacementProof& proof,
        ImmediateLaneHandle handle, std::uint64_t root) const noexcept;
    bool CapturePrivatePlacedEvent(const PrivateImmediateLanePlacementProof& proof,
        std::uint32_t ordinal, ImmediateLaneEvent* out) const noexcept;
    bool TryPlaceExactPrivate(ImmediateLaneHandle handle, std::uint64_t anchor,
        PrivateImmediateLanePlacementProof* outPlacement = nullptr) noexcept;
    bool Publish(ImmediateLaneHandle handle) noexcept;
    // Holds the actual reader gate until publication or explicit abandonment.
    bool ProvePrivatePublication(ImmediateLaneHandle handle, std::uint64_t root,
        PrivateImmediateLanePublicationProof* out) noexcept;
    static void ApplyPrivatePublication(PrivateImmediateLanePublicationProof&& proof) noexcept;
    bool Cancel(ImmediateLaneHandle handle) noexcept;
    bool Retire(ImmediateLaneHandle handle,
                std::uint64_t completedAudioSequence = 0) noexcept;
    bool Retire(ImmediateLaneHandle handle, std::uint64_t completedAudioSequence,
        OriginalImmediateLaneAttemptLifetimePin* baseline) noexcept;

    // Audio thread. CaptureDue pins a bucket until a failed Claim or Finish.
    // Claim races cancellation through the bucket state; Finish releases the
    // scan pin after publishing a single receipt. No operation waits or spins.
    const ImmediateLaneBucket* CaptureDue(std::uint32_t slot,
                                          std::uint64_t quantum) noexcept;
    bool SuspendCaptureForSourceWait(std::uint32_t slot,
        const ImmediateLaneBucket* bucket,
        const FrontierEntryReceiptRef* waitingEntries = nullptr,
        std::uint32_t waitingEntryCount = 0) noexcept;
    bool CapturedSourceWaitQuantum(std::uint32_t slot,
        const ImmediateLaneBucket* bucket, std::uint64_t* outQuantum) const noexcept;
    // Called before the first native C1 staging or claim, while its actual
    // capture is held. Current C0 and unrelated groups do not join Reset.
    bool BeforeConditionalProjectionNativeWrite(ImmediateLaneHandle handle,
        const Plan03GroupSliceIdentity& group) const noexcept;
    // Borrows the actual installed reader only while the original capture is held.
    const TimelineArmReaderPin* CapturedArmReader(ImmediateLaneHandle handle,
        const ImmediateLaneBucket* bucket, std::uint32_t eventIndex) const noexcept;
    const AuthoredStopFenceDescriptor* CapturedNoneFence(ImmediateLaneHandle handle,
        const ImmediateLaneBucket* bucket, std::uint32_t eventIndex) const noexcept;
    bool Claim(ImmediateLaneHandle handle,
               const ImmediateLaneBucket* bucket) noexcept;
    bool Claim(ImmediateLaneHandle handle,
               const ImmediateLaneBucket* bucket,
               ImmediateLaneBucketClaimReceipt* receipt) noexcept;
    // Refused publication keeps the capture until its actual proof prefix is returned.
    bool ClaimRetainingCaptureOnRefusal(ImmediateLaneHandle handle,
        const ImmediateLaneBucket* bucket) noexcept;
    bool ValidateClaimReceipt(const ImmediateLaneBucketClaimReceipt& receipt,
                              const ImmediateLaneBucket* bucket) const noexcept;
    bool ValidateGroupClaimReceipt(
        const ImmediateLaneBucketClaimReceipt& receipt,
        const Plan03GroupSliceIdentity& group,
        std::uint32_t bucketOrdinal,
        std::uint32_t groupIndex) const noexcept;
    bool CapturePublishedGroupDescriptor(
        ImmediateLaneHandle handle, std::uint32_t groupIndex,
        ImmediateLaneGroupRef* out) const noexcept;
    bool CapturePublishedGroupCount(ImmediateLaneHandle handle,
                                    std::uint32_t* out) const noexcept;
    bool ProvePrivateGroupPlacement(
        ImmediateLaneHandle handle,
        const Plan03GroupSliceIdentity& group,
        std::uint32_t bucketOrdinal,
        std::uint32_t groupIndex) const noexcept;
    bool ReleaseCapture(std::uint32_t slot,
                         const ImmediateLaneBucket* bucket) noexcept;
    bool FinishDiscardedPlainWithdrawals(std::uint32_t slot,
        const ImmediateLaneBucket* bucket) noexcept;
    bool RejectCapturedBucket(ImmediateLaneHandle handle,
                              const ImmediateLaneBucket* bucket) noexcept;
    bool RecordAdmissionFailure(std::uint32_t slot,
                                const ImmediateLaneBucket* bucket,
                                ImmediateLaneAdmissionFailure failure) noexcept;
    bool Finish(std::uint32_t slot, const ImmediateLaneBucket* bucket,
                bool applied) noexcept;
    bool ClaimCancelledOnly(ImmediateLaneHandle handle,
                            const ImmediateLaneBucket* bucket) noexcept;
    bool FinishCancelledOnly(std::uint32_t slot,
                             const ImmediateLaneBucket* bucket) noexcept;
    bool IsEventAdmitted(std::uint32_t slot, const ImmediateLaneBucket* bucket,
                          std::uint32_t globalIndex) const noexcept;
    bool IsGroupAdmitted(std::uint32_t slot, const ImmediateLaneBucket* bucket,
                          std::uint32_t globalIndex) const noexcept;
    bool CapturedEntryProjectionValid(std::uint32_t slot,
                                      const ImmediateLaneBucket* bucket) const noexcept;
    bool HasRegisteredEntries(std::uint32_t slot,
                              std::uint32_t generation) const noexcept;
    bool CanAdmitFutureEvent(std::uint32_t slot,
        const ImmediateLaneBucket* bucket, std::uint32_t globalIndex) const noexcept;
    const ImmediateLaneEvent* Events(std::uint32_t slot,
                                      const ImmediateLaneBucket* bucket) const noexcept;
    const ImmediateLaneGroupRef* Groups(std::uint32_t slot,
                                        const ImmediateLaneBucket* bucket) const noexcept;
    bool FutureMember(std::uint32_t slot,
                      std::uint32_t attemptGeneration,
                      const Plan03GroupSliceIdentity& group,
                      std::uint16_t memberIndex,
                      TimelineArmToken arm,
                      const ImmediateLaneEvent** event,
                      std::uint16_t* eventSpanCount = nullptr) const noexcept;
    bool CaptureFrozenFutureTarget(ImmediateLaneHandle handle,
                                  const Plan03GroupSliceIdentity& source,
                                  const Plan03GroupSliceIdentity& target,
                                  ImmediateLaneMemberManifest* out,
                                  std::uint16_t memberIndex = UINT16_MAX) const noexcept;
    bool EntryApplied(std::uint32_t slot,
                      std::uint32_t attemptGeneration) const noexcept;
    bool EntryApplied(std::uint32_t slot, std::uint32_t attemptGeneration,
                       const FrontierEntryReceiptRef& entry) const noexcept;
    bool CapturedEntrySourceWait(std::uint32_t slot, const ImmediateLaneBucket* bucket,
        const FrontierEntryReceiptRef& entry) const noexcept;

private:
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    friend struct detail::OriginalConditionalWithdrawalNegativeTestAccess;
#endif
    friend class ImmediateLaneTimelineFinalizer;
    friend class ScheduledTransactionPool;
    // This is reachable only through the Pool's actual two-C0-row short proof.
    bool CapturePrivatePlacedC0Source(
        const PrivateImmediateLanePlacementProof& placement,
        const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
        const DecisionTable& decisions, const FrontierGroupBinding& primary,
        const FrontierGroupBinding& initial, TimelineArmToken source,
        std::uint64_t* outFrame) const noexcept;
    // Called only after the real finalizer has returned every held payload proof.
    bool FinishDiscardedConditionalWithdrawal(std::uint32_t slot,
        const ImmediateLaneBucket* bucket) noexcept;
    friend class OriginalImmediateLaneConditionalProjection;
    friend class OriginalConditionalReplacementBatch;
    friend class PrivatePlainLayoutEdit;
    friend class PrivatePlainLayoutCommitProof;
    friend class PrivateImmediateLanePlacementProof;
    friend class PrivateImmediateLanePublicationProof;
    bool ValidatePrivatePublication(ImmediateLaneHandle handle,
        const ImmediateLaneAttempt& attempt) const noexcept;
    void ApplyPrivatePublicationImpl(ImmediateLaneHandle handle,
        ImmediateLaneAttempt& attempt) noexcept;
    bool MatchesPrivatePlacementProof(const PrivateImmediateLanePlacementProof& proof) const noexcept;
    bool CapturePrivatePendingVariableStartGeometryImpl(ImmediateLaneHandle handle,
        const TimelineArmTable& arms, TimelineArmToken arm, std::uint64_t authoredOffset,
        std::uint64_t* logicalFrame, std::uint64_t* onsetFrame, bool unbound = false) const noexcept;
    void ReturnPrivatePlacementProof(PrivateImmediateLanePlacementProof& proof) noexcept;
    void InstallPlacement(ImmediateLaneHandle handle, std::uint64_t anchor) noexcept;
    void FinishPrivateLayout(PrivatePlainLayoutEdit& edit, bool commit) noexcept;
    bool HasProgramReferenceImpl(ProgramGeneration generation,
        const TimelineProgramView* program) const noexcept;
    bool AcquirePrivatePlainLayoutEditImpl(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        PrivatePlainLayoutEdit* out, bool pristineReserved) noexcept;
    struct PrivateEntryBucket {
        std::uint32_t ordinal = UINT32_MAX;
        std::atomic<ImmediateLaneEntryOutcome> outcome{
            ImmediateLaneEntryOutcome::Dormant};
        std::atomic<std::uint64_t> audioSequence{0};
    };
    struct SourceWaitEntry {
        FrontierEntryReceiptRef entry{};
        std::uint64_t sequence = 0;
    };
    struct PrivateEntryAssociation {
        FrontierEntryReceiptRef entry{};
        DecisionReserveRequest coordinatorRequest{};
        const ScheduledTransactionPool* pool = nullptr;
        const TimelineArmTable* armTable = nullptr;
        std::uint64_t armTableIncarnation = 0;
        const DecisionTable* decisions = nullptr;
        std::uint64_t provisionalFrame = 0;
        std::uint32_t firstMember = UINT32_MAX;
        std::uint32_t memberCount = 0;
        std::array<Plan03GroupSliceIdentity, 3> groups{};
        std::uint32_t firstOwnedGroup = UINT32_MAX;
        std::uint32_t ownedGroupCount = 0;
        ImmediateLanePrivateVariableControlEntryMapping mapping{};
        bool mappingValid = false;
        std::atomic<std::uint8_t> admission{0};
        PrivateEntryBucket* buckets = nullptr;
        std::uint32_t bucketCapacity = 0;
        std::uint32_t bucketCount = 0;
        bool inUse = false;
    };
    PrivateEntryAssociation& PrivateEntryAt(std::size_t logical) noexcept {
        return privateEntries_[privateEntryDirectory_[logical]];
    }
    const PrivateEntryAssociation& PrivateEntryAt(std::size_t logical) const noexcept {
        return privateEntries_[privateEntryDirectory_[logical]];
    }
    template<class Owner>
    struct PrivateEntryView {
        Owner* owner;
        std::size_t base;
        decltype(auto) operator[](std::size_t index) const noexcept {
            return owner->PrivateEntryAt(base + index);
        }
    };
    auto PrivateEntries(std::uint32_t slot) noexcept {
        return PrivateEntryView<ImmediateActionLane>{this,
            static_cast<std::size_t>(slot) * config_.eventsPerAttempt};
    }
    auto PrivateEntries(std::uint32_t slot) const noexcept {
        return PrivateEntryView<const ImmediateActionLane>{this,
            static_cast<std::size_t>(slot) * config_.eventsPerAttempt};
    }
    PrivateEntryAssociation* AcquirePrivateEntry(std::size_t logical) noexcept;
    void ReleasePrivateEntries(std::uint32_t slot) noexcept;
    bool FreezePrivateEntryAdmission(ImmediateLaneHandle handle) noexcept;
    void BeginPrivateEntryAdmission(std::uint32_t slot,
                                    std::uint32_t bucketOrdinal) noexcept;
    void EndPrivateEntryAdmission(std::uint32_t slot,
        const ImmediateLaneBucket* bucket, bool applied) noexcept;
    const PrivateEntryBucket* PrivateEntryBucketFor(
        const PrivateEntryAssociation& entry, std::uint32_t ordinal) const noexcept;
    bool AllCapturedEntriesWithdrawing(std::uint32_t slot,
        const ImmediateLaneBucket* bucket) const noexcept;
    bool FinishBucket(std::uint32_t slot, const ImmediateLaneBucket* bucket,
                      ImmediateLaneBucketState terminal) noexcept;
    void PreserveSourceWaitSuccessors(std::uint32_t slot) noexcept;
    bool ClaimImpl(ImmediateLaneHandle handle, const ImmediateLaneBucket* bucket,
        ImmediateLaneBucketClaimReceipt* receipt, bool releaseOnRefusal) noexcept;
    bool ConditionalProjectionGroup(std::uint32_t slot, std::uint32_t index) const noexcept;
    bool ConditionalProjectionEvent(std::uint32_t slot, std::uint32_t index) const noexcept;
    bool EntireConditionalProjectionBucket(std::uint32_t slot, const ImmediateLaneBucket& bucket) const noexcept;
    void BeginConditionalProjection(std::uint32_t slot, const ImmediateLaneBucket& bucket,
        std::uint64_t actualAudioSequence) noexcept;
    void EndConditionalProjection(std::uint32_t slot) noexcept;
    void BeginEventAdmission(std::uint32_t slot, const ImmediateLaneBucket& bucket) noexcept;
    void EndEventAdmission(std::uint32_t slot, const ImmediateLaneBucket& bucket) noexcept;
    void AssignPlainControlWithdrawalScopes(ImmediateLaneHandle handle) noexcept;
    bool PlainReferenceAttempt(ImmediateLaneHandle handle, std::uint64_t root) const noexcept;
    bool BuildPrivateVariableControlEntryMapping(ImmediateLaneHandle handle,
        const ImmediateLaneAttempt& attempt, const PrivateEntryAssociation& entry,
        ImmediateLanePrivateVariableControlEntryMapping* out) const noexcept;
    bool BuildAcceptedPrivateEntryMapping(ImmediateLaneHandle handle,
        const ImmediateLaneAttempt& attempt, const PrivateEntryAssociation& entry,
        ImmediateLanePrivateVariableControlEntryMapping* out) const noexcept;
    void InvalidatePrivateEntryMappings(ImmediateLaneHandle handle) noexcept;
    bool ValidatePrivateVariableControlEntryScope(ImmediateLaneHandle handle,
        std::uint64_t root, ImmediateLaneDemand originalDemand,
        const ImmediateLanePrivateVariableControlEntryScope& scope,
        const ScheduledTransactionPool& pool, const TimelineArmTable& arms,
        const DecisionTable& decisions,
        const std::array<PreparedPrivateImmediateLaneGroup, 3>& originals,
        const std::array<PrivateImmediateLaneGroupTransferProof, 3>& proofs) const noexcept;
    ImmediateLaneAttempt* Resolve(ImmediateLaneHandle handle) noexcept;
    const ImmediateLaneAttempt* Resolve(ImmediateLaneHandle handle) const noexcept;
    ImmediateLaneEvent* EventBase(std::uint32_t slot) noexcept;
    const ImmediateLaneEvent* EventBase(std::uint32_t slot) const noexcept;
    ImmediateLaneMemberManifest* MemberBase(std::uint32_t slot) noexcept;
    const ImmediateLaneMemberManifest* MemberBase(std::uint32_t slot) const noexcept;
    bool PrepareAttemptStorage(std::uint32_t slot, std::uint32_t events,
        std::uint32_t members) noexcept;
    bool EnsureAttemptBacking(std::uint64_t count,
        std::uint32_t excluded = UINT32_MAX) noexcept;
    ImmediateLaneGroupRef* GroupBase(std::uint32_t slot) noexcept;
    const ImmediateLaneGroupRef* GroupBase(std::uint32_t slot) const noexcept;
    ImmediateLaneBucket* BucketBase(std::uint32_t slot) noexcept;
    const ImmediateLaneBucket* BucketBase(std::uint32_t slot) const noexcept;
    bool CandidateFits(std::uint32_t slot, std::uint64_t anchor,
        std::uint64_t firstSafeFrame) const noexcept;

    friend class OriginalImmediateLaneAttemptLifetimePin;
    bool MatchesOriginalAttempt(const OriginalImmediateLaneAttemptLifetimePin& original) const noexcept;
    bool BorrowOriginalAttempt(const OriginalImmediateLaneAttemptLifetimePin& original,
        OriginalImmediateLaneAttemptLifetimePin* out) noexcept;
    void ReturnOriginalAttempt(OriginalImmediateLaneAttemptLifetimePin& original) noexcept;
    std::uint64_t nextOriginalLifetimeMarker_ = 0;
    Config config_{};
    std::unique_ptr<ImmediateLaneAttempt[]> attempts_;
    detail::StablePageReservation eventStorage_;
    detail::StablePageReservation memberStorage_;
    StableStorageGrowthState payloadGrowth_{};
    std::size_t eventStorageBytes_ = 0;
    std::size_t memberStorageBytes_ = 0;
    ImmediateLaneEvent* events_ = nullptr;
    std::unique_ptr<ImmediateLaneGroupRef[]> groups_;
    std::unique_ptr<ImmediateLaneDeclaredGroup[]> declaredGroups_;
    std::unique_ptr<std::uint32_t[]> eventPlacementOrder_;
    std::unique_ptr<std::uint32_t[]> groupPlacementOrder_;
    ImmediateLaneMemberManifest* members_ = nullptr;
    StableRowStorage<PrivateEntryAssociation> privateEntries_;
    std::unique_ptr<std::uint32_t[]> privateEntryDirectory_;
    std::unique_ptr<std::uint32_t[]> privateOwnedGroupIndices_;
    std::unique_ptr<PrivateEntryBucket[]> privateEntryBuckets_;
    std::unique_ptr<SourceWaitEntry[]> sourceWaitEntries_;
    std::unique_ptr<std::uint32_t[]> eventPrivateEntries_;
    std::unique_ptr<std::uint32_t[]> groupPrivateEntries_;
    std::unique_ptr<std::uint32_t[]> memberOrder_;
    std::unique_ptr<std::uint32_t[]> groupMemberBases_;
    std::unique_ptr<std::uint16_t[]> groupMemberCounts_;
    std::unique_ptr<ImmediateLaneBucket[]> buckets_;
    std::unique_ptr<TimelineArmReaderPin[]> armPins_;
    std::unique_ptr<AuthoredStopFenceLaneReference[]> fenceReferences_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> eventWithdrawal_;
    std::unique_ptr<std::uint32_t[]> eventWithdrawalLeader_;
    std::unique_ptr<std::uint64_t[]> scopeLastCaptureSequence_;
    std::unique_ptr<std::uint8_t[]> capturedEventAdmission_;
    std::unique_ptr<std::uint8_t[]> conditionalEventScopes_;
    std::unique_ptr<Plan03GroupReaderPin[]> groupPins_;
    std::unique_ptr<DecisionFuturePin[]> decisionPins_;
    std::unique_ptr<std::uint8_t[]> futureDependentOwned_;
    ScheduledTransactionPool* groupPool_ = nullptr;
#if !defined(HARKEN_RT_QUALIFICATION)
    void (*reserveGateHook_)(void*, std::uint32_t) = nullptr;
    void* reserveGateContext_ = nullptr;
    void (*finishHeadHook_)(void*) = nullptr;
    void* finishHeadContext_ = nullptr;
    void (*captureGateHook_)(void*) = nullptr;
    void* captureGateContext_ = nullptr;
    void (*eventAdmissionHook_)(void*, std::uint32_t) = nullptr;
    void* eventAdmissionContext_ = nullptr;
    void (*capturedAdmissionHook_)(void*, std::uint32_t) = nullptr;
    void* capturedAdmissionContext_ = nullptr;
    void (*validationRefusalHook_)(void*) = nullptr;
    void* validationRefusalContext_ = nullptr;
#endif
    bool valid_ = false;
};

} // namespace harken

#endif // HARKEN_CORE_IMMEDIATE_ACTION_LANE_H
