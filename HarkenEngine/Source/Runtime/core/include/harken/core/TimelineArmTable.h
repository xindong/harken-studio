#ifndef HARKEN_CORE_TIMELINE_ARM_TABLE_H
#define HARKEN_CORE_TIMELINE_ARM_TABLE_H

#include "harken/core/StableRowStorage.h"

#include "harken/core/DecisionToken.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/FrontierActivation.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace harken {
namespace detail { struct OriginalControlOriginTestAccess; struct OriginalGroupMemberTestAccess; }
class ScheduledTransactionPool;
class DecisionTable;
class ArmGroupTable;
class AuthoredStopOperation;
struct ScheduledEventInput;
struct ConsumedEvent;
class ImmediateActionLane;
class PrivateImmediateLanePlacementProof;
class MixerLoop;
class PrivateSelectedQueueFutureSpanPool;
class PrivatePendingQueueFrontierBinding;
class OriginalQueueFutureGroupRegistration;
struct ImmediateLaneEvent;
struct Plan03PageGroupView;

enum class ArmState : std::uint8_t {
    Free = 0,
    PendingApply = 1,
    Armed = 2,
    Published = 3,
    Started = 4,
    CancelRequested = 5,
    Tombstoned = 6,
    Quarantined = 7,
    AudioClaimed = 8,
    Prepared = 9,
};

enum class ArmPublishStatus : std::uint8_t {
    Published,
    DeferredReservation,
    Invalid,
};

struct TimelineArmToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint32_t renderSlot = UINT32_MAX;
    std::uint32_t renderGeneration = 0;

    bool Valid() const {
        return slot != UINT32_MAX && generation != 0 &&
               renderSlot != UINT32_MAX && renderGeneration != 0;
    }
};

struct TimelineArmObservation {
    ArmState state = ArmState::Free;
    std::uint64_t armedCompletionSequence = 0;
    std::uint64_t startedBlockSequence = 0;
    std::uint64_t logicalCompletionSequence = 0;
    std::uint64_t tombstoneAudioSequence = 0;
    ParentAudioGroupRef parentGroup{};
    std::uint16_t memberIndex = UINT16_MAX;
    bool finalSourcePublished = false;
};

class TimelineArmTable;
class PrivateUnboundPageArmRetirementProof;
class UnpublishedGroupArmCancellationProof;
class OriginalSourceSubmissionRegistry;

class TimelineArmReaderPin {
public:
    TimelineArmReaderPin() = default;
    TimelineArmReaderPin(const TimelineArmReaderPin&) = delete;
    TimelineArmReaderPin& operator=(const TimelineArmReaderPin&) = delete;
    TimelineArmReaderPin(TimelineArmReaderPin&& other) noexcept
        : owner_(other.owner_), token_(other.token_) { other.owner_ = nullptr; }
    TimelineArmReaderPin& operator=(TimelineArmReaderPin&& other) noexcept {
        if (this != &other) {
            assert(owner_ == nullptr);
            owner_ = other.owner_;
            token_ = other.token_;
            other.owner_ = nullptr;
        }
        return *this;
    }
    bool Valid() const noexcept { return owner_ != nullptr; }
    bool Release(std::uint64_t completedAudioSequence) noexcept;
private:
    friend class TimelineArmTable;
    friend class ImmediateActionLane;
    TimelineArmTable* owner_ = nullptr;
    TimelineArmToken token_{};
};

enum class TimelineArmOperationKind : std::uint8_t { Start, Control, TransientControl };
enum class TimelineArmOperationDisposition : std::uint8_t {
    Claimed, Cancelled, Stale, Deferred, Ineligible
};

class TimelineArmTable {
public:
    // Original backing remains immutable and alive through actual reader retirement.
    struct AuthoredControlInput {
        const AuthoredStopOperation* operation = nullptr;
        TimelineTransactionProgramRef program{};
        const ScheduledEventInput* events = nullptr;
        std::uint32_t eventCount = 0;
        std::uint64_t onsetFrame = 0;
        std::uint64_t fadeStartFrame = 0;
        std::uint32_t renderSlot = UINT32_MAX;
        std::uint32_t renderGeneration = 0;
        TimelineSemanticTarget target{};
        std::uint64_t armDeadline = 0;
    };

    enum class AuthoredControlStage : std::uint8_t { None, Pending, Bound };

    struct PendingAuthoredControlInput {
        const AuthoredStopOperation* operation = nullptr;
        TimelineTransactionProgramRef program{};
        const ScheduledEventInput* events = nullptr;
        std::uint32_t eventCapacity = 0;
        std::uint64_t fadeStartFrame = 0;
        TimelineArmToken sourceStart{};
        TimelineSemanticTarget target{};
    };

    struct AuthoredControlEventSnapshot {
        std::uint64_t executeFrame = 0;
        std::uint64_t payload = 0;
        std::uint32_t executionOrdinal = 0;
        std::uint32_t eventIndex = 0;
        std::uint32_t subeventPhase = 0;
        bool authoredFadeOutCandidate = false;
    };

    struct AuthoredControlMetadata {
        const AuthoredStopOperation* operation = nullptr;
        TimelineTransactionProgramRef program{};
        const TimelineEventPayload* payloads = nullptr;
        const ScheduledEventInput* events = nullptr;
        AuthoredStopOperationIdentity identity{};
        std::uint64_t onsetFrame = 0;
        std::uint64_t fadeStartFrame = 0;
        const CurveLUT* curve = nullptr;
        std::uint32_t eventCount = 0;
        AuthoredControlEventSnapshot eventsSnapshot[2]{};
        TimelineArmToken sourceStart{};
        std::uint32_t eventCapacity = 0;
        AuthoredControlStage stage = AuthoredControlStage::None;
        ParentAudioGroupRef selectedSourceGroup{};
    };

    struct PreparedCommitInput {
        TimelineArmToken arm{};
        std::uint64_t startFrame = 0;
        std::uint64_t armDeadline = 0;
        ProgramGeneration programGeneration = 0;
        TimelineSemanticTarget target{};
        ParentAudioGroupRef parentGroup{};
        std::uint16_t memberIndex = UINT16_MAX;
        bool pendingImmediateStart = false;
    };

    struct PendingImmediateRetirementInput {
        TimelineArmToken arm{};
        ParentAudioGroupRef parentGroup{};
        std::uint16_t memberIndex = UINT16_MAX;
        std::uint64_t startFrame = 0;
        const FrontierGroupBinding* binding = nullptr;
        AudioSliceLease* lease = nullptr;
        TimelineArmReaderPin* ownedReader = nullptr;
    };

    struct Row {
        std::atomic<ArmState> state{ArmState::Free};
        std::atomic<std::uint64_t> identity{0};
        std::atomic_flag reservation = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> tombstoneAudioSequence{0};
        std::atomic<std::uint64_t> armedCompletionSequence{0};
        std::uint64_t sourcePublicationSequence = 0;
        std::atomic<std::uint64_t> startedBlockSequence{0};
        std::atomic<std::uint64_t> logicalCompletionSequence{0};
        TimelineArmReaderPin pendingControlSourceReader{};
        TimelineArmToken originalPlainControl{};
        AuthoredStopOperationIdentity originalPlainControlIdentity{};
        TimelineArmToken appliedPlainStopSource{};
        AuthoredStopOperationIdentity appliedPlainStop{};
        std::atomic<std::uint64_t> appliedPlainStopSequence{0};
        std::uint32_t generation = 1;
        std::uint32_t renderSlot = UINT32_MAX;
        std::uint32_t renderGeneration = 0;
        std::uint64_t startFrame = 0;
        std::uint64_t armDeadline = 0;
        bool pendingImmediateStart = false;
        bool startBound = true;
        ProgramGeneration programGeneration = 0;
        TimelineSemanticTarget target{};
        ParentAudioGroupRef parentGroup{};
        std::uint16_t memberIndex = UINT16_MAX;
        std::atomic<std::uint32_t> frontierReaderPins{0};
        std::atomic<std::uint32_t> variableReadPins{0};
        std::atomic<bool> originalLifetimePinned{false};
        bool originalLifetimeIssued = false;
        const void* originalSourceSubmission = nullptr;
        std::uint64_t originalSourceEpisode = 0;
        std::atomic<std::uint64_t> frontierLastReadSequence{0};
        FrontierDueOperationRef frontierOperation{};
        const FrontierGroupBinding* frontierAdmission = nullptr;
        AudioSliceLease* frontierAdmissionLease = nullptr;
        std::atomic<AudioSliceLease*> frontierActiveLease{nullptr};
        std::atomic<bool> frontierAdmissionFinished{true};
        // Issued only by the owning never-published, cancelled Lane under this row gate.
        // The same-generation record survives cancellation and clears only at Reserve.
        std::atomic<bool> nativePrivateWithdrawn{false};
        bool finalSourcePublicationIssued = false;
        bool finalSourcePublicationConsumed = false;
        AuthoredControlMetadata authoredControl{};
    };

    // Speculation owns only this row and one original operation reader.
    class AuthoredControlBucketProof {
    public:
        AuthoredControlBucketProof() = default;
        AuthoredControlBucketProof(const AuthoredControlBucketProof&) = delete;
        AuthoredControlBucketProof& operator=(const AuthoredControlBucketProof&) = delete;
        AuthoredControlBucketProof(AuthoredControlBucketProof&& other) noexcept;
        AuthoredControlBucketProof& operator=(AuthoredControlBucketProof&& other) noexcept;
        ~AuthoredControlBucketProof();
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept;
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        const AuthoredStopOperation* operation_ = nullptr;
        std::uint64_t audioSequence_ = 0;
        bool containsStop_ = false;
        bool applied_ = false;
    };

    // Original source reader stays stable and backing stays frozen until release.
    class PendingAuthoredControlBindingProof {
    public:
        PendingAuthoredControlBindingProof() = default;
        PendingAuthoredControlBindingProof(const PendingAuthoredControlBindingProof&) = delete;
        PendingAuthoredControlBindingProof& operator=(const PendingAuthoredControlBindingProof&) = delete;
        PendingAuthoredControlBindingProof(PendingAuthoredControlBindingProof&& other) noexcept;
        PendingAuthoredControlBindingProof& operator=(PendingAuthoredControlBindingProof&& other) noexcept;
        ~PendingAuthoredControlBindingProof();
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept;
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        Row* source_ = nullptr;
        const AuthoredStopOperation* operation_ = nullptr;
        AuthoredControlMetadata final_{};
        std::uint64_t deadline_ = 0;
    };

    // Stable storage and the table must outlive this command-local reservation.
    // This proves a live PendingApply row, not durable original Source issuance.
    class SourceCommandRowProof {
    public:
        SourceCommandRowProof() = default;
        SourceCommandRowProof(const SourceCommandRowProof&) = delete;
        SourceCommandRowProof& operator=(const SourceCommandRowProof&) = delete;
        ~SourceCommandRowProof() { Release(); }
        bool Valid() const noexcept { return row_ != nullptr; }
        const TimelineArmTable* Owner() const noexcept { return owner_; }
        TimelineArmToken Token() const noexcept { return token_; }
        ParentAudioGroupRef Parent() const noexcept { return row_->parentGroup; }
        std::uint16_t MemberIndex() const noexcept { return row_->memberIndex; }
        std::uint64_t CompletionSequence() const noexcept { return completion_; }
        bool IsCancellation() const noexcept { return cancellation_; }
        const void* OriginalSourceSubmission() const noexcept {
            return row_->originalSourceSubmission;
        }
        void Release() noexcept;
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        TimelineArmToken token_{};
        std::uint64_t completion_ = 0;
        bool cancellation_ = false;
    };
    enum class SourceCommandRowStatus : std::uint8_t {
        Ready, DeferredReservation, NotPendingSource, Invalid, RequiresOriginalSubmission
    };
    SourceCommandRowStatus ProvePendingSourceCommand(TimelineArmToken token,
        SourceCommandRowProof* out, const void* originalSubmission = nullptr) noexcept;
    // Keep the row held until the actual physical installation is published.
    static void AcknowledgeSourceCommand(SourceCommandRowProof& proof,
        std::uint64_t completionSequence, std::uint64_t publicationSequence = 0) noexcept;
    bool SourcePublicationAccepted(TimelineArmToken token) const noexcept;

    // Issued by the real fixed Source factory; the original owner retains its row.
    class PublicationAfterInstallation {
    public:
        PublicationAfterInstallation() = default;
        PublicationAfterInstallation(const PublicationAfterInstallation&) = delete;
        PublicationAfterInstallation& operator=(const PublicationAfterInstallation&) = delete;
        PublicationAfterInstallation(PublicationAfterInstallation&& other) noexcept {
            *this = std::move(other);
        }
        PublicationAfterInstallation& operator=(PublicationAfterInstallation&& other) noexcept {
            if (this != &other) {
                owner_ = std::exchange(other.owner_, nullptr);
                row_ = std::exchange(other.row_, nullptr);
                token_ = std::exchange(other.token_, {});
                submission_ = std::exchange(other.submission_, nullptr);
                incarnation_ = std::exchange(other.incarnation_, 0);
                episode_ = std::exchange(other.episode_, 0);
                frame_ = std::exchange(other.frame_, 0);
                deadline_ = std::exchange(other.deadline_, 0);
                program_ = std::exchange(other.program_, 0);
                target_ = std::exchange(other.target_, {});
            }
            return *this;
        }
        bool Valid() const noexcept { return owner_ != nullptr; }
    private:
        friend class TimelineArmTable;
        friend class OriginalSourceSubmissionRegistry;
        void Reset() noexcept {
            owner_ = nullptr; row_ = nullptr; token_ = {}; submission_ = nullptr;
            incarnation_ = episode_ = frame_ = deadline_ = program_ = 0; target_ = {};
        }
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        TimelineArmToken token_{};
        const void* submission_ = nullptr;
        std::uint64_t incarnation_ = 0;
        std::uint64_t episode_ = 0;
        std::uint64_t frame_ = 0;
        std::uint64_t deadline_ = 0;
        ProgramGeneration program_ = 0;
        TimelineSemanticTarget target_{};
    };

    // The original Source owns this right through actual command installation.
    class GroupedPublicationAfterInstallation {
    public:
        GroupedPublicationAfterInstallation() = default;
        GroupedPublicationAfterInstallation(const GroupedPublicationAfterInstallation&) = delete;
        GroupedPublicationAfterInstallation& operator=(const GroupedPublicationAfterInstallation&) = delete;
        bool Valid() const noexcept { return owner_ != nullptr; }
    private:
        friend class TimelineArmTable;
        friend class OriginalSourceSubmissionRegistry;
        void Reset() noexcept {
            owner_ = nullptr; row_ = nullptr; token_ = {}; submission_ = nullptr;
            incarnation_ = episode_ = frame_ = deadline_ = program_ = 0; target_ = {};
            groups_ = nullptr; group_ = {}; parent_ = {}; member_ = UINT16_MAX;
            binding_ = nullptr; lease_ = nullptr;
        }
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        TimelineArmToken token_{};
        const void* submission_ = nullptr;
        std::uint64_t incarnation_ = 0;
        std::uint64_t episode_ = 0;
        std::uint64_t frame_ = 0;
        std::uint64_t deadline_ = 0;
        ProgramGeneration program_ = 0;
        TimelineSemanticTarget target_{};
        ArmGroupTable* groups_ = nullptr;
        ArmGroupToken group_{};
        DecisionToken parent_{};
        std::uint16_t member_ = UINT16_MAX;
        const FrontierGroupBinding* binding_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
    };

    // The table and its private owner must outlive this exclusive mutation right.
    // Abandoning the proof leaves the original Prepared cancellation right intact.
    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              row_(std::exchange(other.row_, nullptr)) {}
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                row_ = std::exchange(other.row_, nullptr);
            }
            return *this;
        }
        ~PreparedCommitProof() { Release(); }
        bool Valid() const noexcept { return row_ != nullptr; }
        TimelineArmToken OriginalPreparedToken() const noexcept;
        const TimelineArmTable* Owner() const noexcept { return owner_; }
        void Release() noexcept {
            if (row_ != nullptr) {
                auto* held = std::exchange(row_, nullptr);
                owner_ = nullptr;
                held->reservation.clear(std::memory_order_release);
            }
        }
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
    };

    // Retains only this genuinely prepared row's lifetime and full identity.
    // It grants no Source purpose, Frontier read, cancellation or queue right.
    // The table must outlive this owner and every release in progress.
    class OriginalArmRowOwner {
    public:
        OriginalArmRowOwner() = default;
        OriginalArmRowOwner(const OriginalArmRowOwner&) = delete;
        OriginalArmRowOwner& operator=(const OriginalArmRowOwner&) = delete;
        OriginalArmRowOwner(OriginalArmRowOwner&& other) noexcept;
        OriginalArmRowOwner& operator=(OriginalArmRowOwner&& other) noexcept;
        ~OriginalArmRowOwner() { Release(); }
        bool Valid() const noexcept { return owner_ != nullptr; }
        TimelineArmToken Token() const noexcept { return token_; }
        const TimelineArmTable* Owner() const noexcept { return owner_; }
        void Release() noexcept;
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        TimelineArmToken token_{};
    };
    // Lifetime only. Return after private rollback, an audio fence, or joined teardown.
    class VariableReadLifetime {
    public:
        VariableReadLifetime() = default;
        VariableReadLifetime(const VariableReadLifetime&) = delete;
        VariableReadLifetime& operator=(const VariableReadLifetime&) = delete;
        VariableReadLifetime(VariableReadLifetime&& other) noexcept;
        VariableReadLifetime& operator=(VariableReadLifetime&& other) noexcept;
        ~VariableReadLifetime() { Release(); }
        bool Valid() const noexcept { return row_ != nullptr; }
        TimelineArmToken Token() const noexcept { return token_; }
        const TimelineArmTable* Owner() const noexcept { return owner_; }
        void Release() noexcept;
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row* row_ = nullptr;
        TimelineArmToken token_{};
    };
    // Issued only at the actual unpublished Source disposition, before its
    // original Arm owner returns. This observation alone never grants Arm Free.
    class OriginalUnpublishedSourceDisposition {
    public:
        OriginalUnpublishedSourceDisposition() = default;
        OriginalUnpublishedSourceDisposition(const OriginalUnpublishedSourceDisposition&) = delete;
        OriginalUnpublishedSourceDisposition& operator=(const OriginalUnpublishedSourceDisposition&) = delete;
        OriginalUnpublishedSourceDisposition(OriginalUnpublishedSourceDisposition&&) = delete;
        OriginalUnpublishedSourceDisposition& operator=(OriginalUnpublishedSourceDisposition&&) = delete;
        bool Valid() const noexcept { return owner_ != nullptr; }
        TimelineArmToken Token() const noexcept { return token_; }
    private:
        friend class TimelineArmTable;
        friend class OriginalSourceSubmissionRegistry;
        friend class ScheduledTransactionPool;
        void Reset() noexcept {
            owner_ = nullptr; row_ = nullptr; submission_ = nullptr; token_ = {};
            tableIncarnation_ = sourceEpisode_ = originalMarker_ = sourceIdentity_ = 0;
            commandQueue_ = originalRange_ = nullptr;
            rangeOrigin_ = rangeHead_ = 0; rangeCount_ = 0; unboundOriginal_ = false;
        }
        const TimelineArmTable* owner_ = nullptr;
        const Row* row_ = nullptr;
        const void* submission_ = nullptr;
        TimelineArmToken token_{};
        std::uint64_t tableIncarnation_ = 0;
        std::uint64_t sourceEpisode_ = 0;
        std::uint64_t originalMarker_ = 0;
        std::uint64_t sourceIdentity_ = 0;
        const void* commandQueue_ = nullptr;
        const void* originalRange_ = nullptr;
        std::uint64_t rangeOrigin_ = 0;
        std::uint64_t rangeHead_ = 0;
        std::uint32_t rangeCount_ = 0;
        bool unboundOriginal_ = false;
    };

    enum class UnparentedUnpublishedSourceRetirement : std::uint8_t {
        Consumed, Busy, NotEligible
    };
    // Retires only the original never-read Arm, never its program or Service storage.
    UnparentedUnpublishedSourceRetirement ConsumeOriginalUnparentedUnpublishedSource(
        const OriginalUnpublishedSourceDisposition& original) noexcept;

    // Logic thread only; a genuine typed Pending/Bound Control is refused.
    bool IssueOriginalArmRowOwner(const PreparedCommitProof& preparation,
        OriginalArmRowOwner* out) noexcept;
    // A distinct typed lifetime holder cannot be used as a Source capability.
    class OriginalControlHeaderRowOwner {
    public:
        OriginalControlHeaderRowOwner() = default;
        OriginalControlHeaderRowOwner(const OriginalControlHeaderRowOwner&) = delete;
        OriginalControlHeaderRowOwner& operator=(const OriginalControlHeaderRowOwner&) = delete;
        OriginalControlHeaderRowOwner(OriginalControlHeaderRowOwner&&) noexcept = default;
        OriginalControlHeaderRowOwner& operator=(OriginalControlHeaderRowOwner&&) noexcept = default;
        bool Valid() const noexcept { return lifetime_.Valid(); }
        TimelineArmToken Token() const noexcept { return lifetime_.Token(); }
        const TimelineArmTable* Owner() const noexcept { return lifetime_.Owner(); }
        // Immutable observations captured by the genuine Pending factory.
        // These values grant no Current, Source, operation or Catalog right.
        struct OriginalPreparedScope {
            std::uint64_t incarnation = 0;
            TimelineArmToken sourceStart{};
            const AuthoredStopOperation* operation = nullptr;
            AuthoredStopOperationIdentity operationIdentity{};
            TimelineTransactionProgramRef program{};
            TimelineSemanticTarget target{};
        };
        const OriginalPreparedScope& OriginalScope() const noexcept { return scope_; }
        void Release() noexcept {
            auto lifetime = std::move(lifetime_);
            scope_ = {};
            lifetime.Release();
        }
    private:
        friend class TimelineArmTable;
        friend struct detail::OriginalControlOriginTestAccess;
        OriginalArmRowOwner lifetime_;
        OriginalPreparedScope scope_{};
    };
    bool IssueOriginalControlHeaderRowOwner(const PreparedCommitProof& preparation,
        OriginalControlHeaderRowOwner* out) noexcept;
    bool BindOriginalSourceSubmission(const PreparedCommitProof& preparation,
        const OriginalArmRowOwner& original, const void* submission) noexcept;
    SourceCommandRowStatus ProveOriginalSourceCancellation(
        const OriginalArmRowOwner& original, const void* submission,
        SourceCommandRowProof* out) noexcept;
    static void TombstoneOriginalSourceCommand(SourceCommandRowProof& proof,
        std::uint64_t audioSequence) noexcept;

    // Stable caller storage retains this proof until Finish or an untouched Abort.
    class UnpublishedPendingRetirementProof {
    public:
        UnpublishedPendingRetirementProof() = default;
        UnpublishedPendingRetirementProof(const UnpublishedPendingRetirementProof&) = delete;
        UnpublishedPendingRetirementProof& operator=(const UnpublishedPendingRetirementProof&) = delete;
        UnpublishedPendingRetirementProof(UnpublishedPendingRetirementProof&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), inputs_(other.inputs_),
              rows_(other.rows_), count_(other.count_), nextLease_(other.nextLease_) {}
        bool Valid() const noexcept { return owner_ != nullptr; }
        std::size_t Count() const noexcept { return Valid() ? count_ : 0; }
    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        const PendingImmediateRetirementInput* inputs_ = nullptr;
        Row** rows_ = nullptr;
        std::size_t count_ = 0;
        std::size_t nextLease_ = 0;
    };

    class PlainAudioProof {
    public:
        PlainAudioProof() = default;
        ~PlainAudioProof() { Release(); }
        PlainAudioProof(const PlainAudioProof&) = delete;
        PlainAudioProof& operator=(const PlainAudioProof&) = delete;
        bool Valid() const noexcept { return row_ != nullptr; }
        void Release() noexcept {
            if (row_ != nullptr) row_->reservation.clear(std::memory_order_release);
            row_ = nullptr;
        }
    private:
        friend class TimelineArmTable;
        Row* row_ = nullptr;
        std::uint64_t sequence_ = 0;
        bool starts_ = false;
        bool transient_ = false;
    };
    TimelineArmOperationDisposition ProvePlainAudioApply(TimelineArmToken token,
        bool starts, bool transient, std::uint64_t audioSequence, PlainAudioProof* out) noexcept;
    void ApplyPlainAudio(PlainAudioProof& proof) noexcept;
    bool MatchesVariableInitializerStart(const PlainAudioProof& proof, TimelineArmToken token,
        std::uint64_t onsetFrame) const noexcept;

    class AudioOperationClaim {
    public:
        AudioOperationClaim() = default;
        ~AudioOperationClaim() { if (!applied_) CancelFrontierOperation(*this); }
        AudioOperationClaim(const AudioOperationClaim&) = delete;
        AudioOperationClaim& operator=(const AudioOperationClaim&) = delete;
        AudioOperationClaim(AudioOperationClaim&& other) noexcept
            : row_(other.row_), kind_(other.kind_), sequence_(other.sequence_), applied_(other.applied_),
              priorState_(other.priorState_), operation_(other.operation_) {
            other.row_ = nullptr;
        }
        bool Valid() const noexcept { return row_ != nullptr; }
    private:
        friend class TimelineArmTable;
        Row* row_ = nullptr;
        TimelineArmOperationKind kind_ = TimelineArmOperationKind::Start;
        std::uint64_t sequence_ = 0;
        bool applied_ = false;
        ArmState priorState_ = ArmState::Published;
        FrontierDueOperationRef operation_{};
    };

    class BoundAudioProof {
    public:
        BoundAudioProof() = default;
        ~BoundAudioProof() { Release(); }
        BoundAudioProof(const BoundAudioProof&) = delete;
        BoundAudioProof& operator=(const BoundAudioProof&) = delete;
        bool Valid() const noexcept { return row_ != nullptr; }
        bool AdmissionOnly() const noexcept { return admissionOnly_; }
        void Release() noexcept {
            if (row_ != nullptr) {
                const auto state = lease_->Capture().state;
                if (row_->frontierAdmission == binding_ &&
                    (state == AudioSliceState::SliceApplied || state == AudioSliceState::SliceInvalidated))
                    row_->frontierAdmissionFinished.store(true, std::memory_order_release);
                row_->frontierActiveLease.store(nullptr, std::memory_order_release);
                row_->reservation.clear(std::memory_order_release);
            }
            row_ = nullptr;
        }
    private:
        friend class TimelineArmTable;
        Row* row_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
        const FrontierGroupBinding* binding_ = nullptr;
        std::uint64_t sequence_ = 0;
        bool starts_ = false;
        bool admissionOnly_ = false;
    };

    class LaneAudioProof {
    public:
        LaneAudioProof() = default;
        ~LaneAudioProof() { Release(); }
        LaneAudioProof(const LaneAudioProof&) = delete;
        LaneAudioProof& operator=(const LaneAudioProof&) = delete;
        bool Valid() const noexcept { return row_ != nullptr; }
        void Release() noexcept {
            if (row_ != nullptr)
                row_->reservation.clear(std::memory_order_release);
            row_ = nullptr;
            token_ = {};
            audioSequence_ = 0;
        }
    private:
        friend class TimelineArmTable;
        Row* row_ = nullptr;
        TimelineArmToken token_{};
        std::uint64_t audioSequence_ = 0;
    };

    bool MatchesVariableInitializerStart(const BoundAudioProof& proof, TimelineArmToken token,
        std::uint64_t onsetFrame) const noexcept;
    bool MatchesVariableInitializerStart(const LaneAudioProof& proof, TimelineArmToken token,
        std::uint64_t onsetFrame) const noexcept;
    bool MatchesVariableInitializerStart(const AudioOperationClaim& proof, TimelineArmToken token,
        std::uint64_t onsetFrame) const noexcept;
    bool ProveInitialVariableDeferredStart(TimelineArmToken token, const FrontierGroupBinding& binding,
        AudioSliceLease& lease, const ScheduledTransactionPool& pool,
        std::uint64_t audioSequence, BoundAudioProof* out) noexcept;

    struct PrivatePendingImmediateArmInput {
        TimelineArmToken arm{};
        std::uint64_t startFrame = 0;
        std::uint64_t armDeadline = 0;
        TimelineArmReaderPin* pin = nullptr;
        const FrontierGroupBinding* admission = nullptr;
        AudioSliceLease* lease = nullptr;
    };

    class PrivatePendingImmediateArmProof {
    public:
        PrivatePendingImmediateArmProof() = default;
        PrivatePendingImmediateArmProof(
            const PrivatePendingImmediateArmProof&) = delete;
        PrivatePendingImmediateArmProof& operator=(
            const PrivatePendingImmediateArmProof&) = delete;
        ~PrivatePendingImmediateArmProof() { Release(); }
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept {
            if (owner_ == nullptr) return;
            for (std::size_t index = 0; index < count_; ++index) {
                rows_[index]->reservation.clear(std::memory_order_release);
                rows_[index] = nullptr;
            }
            owner_ = nullptr;
            inputs_ = nullptr;
            rows_ = nullptr;
            count_ = 0;
        }

    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        const PrivatePendingImmediateArmInput* inputs_ = nullptr;
        Row** rows_ = nullptr;
        std::size_t count_ = 0;
    };

    struct BoundArmPublicationInput {
        PreparedCommitInput expected{};
        const TimelineArmReaderPin* reader = nullptr;
        const FrontierGroupBinding* admission = nullptr;
        AudioSliceLease* lease = nullptr;
        const AuthoredControlInput* control = nullptr;
        TimelineArmToken sourceStart{};
    };

    // The table and caller-owned row scratch must outlive this publication reservation.
    class BoundArmPublicationProof {
    public:
        BoundArmPublicationProof() = default;
        BoundArmPublicationProof(const BoundArmPublicationProof&) = delete;
        BoundArmPublicationProof& operator=(const BoundArmPublicationProof&) = delete;
        ~BoundArmPublicationProof() { Release(); }
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept;

    private:
        friend class TimelineArmTable;
        TimelineArmTable* owner_ = nullptr;
        Row** rows_ = nullptr;
        std::size_t count_ = 0;
    };

    explicit TimelineArmTable(std::uint32_t capacity);
    ~TimelineArmTable();

    std::uint32_t Capacity() const { return capacity_; }
    std::uint32_t ConstructedRowCount() const noexcept { return rows_.ConstructedCount(); }
    std::size_t RowStorageBytes() const noexcept { return rows_.RequestedBytes(); }
    std::size_t RowCommittedBytes() const noexcept { return rows_.CommittedBytes(); }
    std::size_t RowReservedBytes() const noexcept { return rows_.ReservedBytes(); }
    std::size_t ScopeStorageBytes() const noexcept { return groupMemberScopes_.RequestedBytes(); }
    std::size_t ScopeCommittedBytes() const noexcept { return groupMemberScopes_.CommittedBytes(); }
    std::size_t ScopeReservedBytes() const noexcept { return groupMemberScopes_.ReservedBytes(); }
    bool AttachRowStoragePreparation(void* context,
        bool (*prepare)(void*, std::uint32_t) noexcept) noexcept;
    bool PrepareBacking(std::uint32_t count, StableStoragePreparation* out) noexcept;
    bool SetFreeGenerationForTests(std::uint32_t slot, std::uint32_t generation) noexcept;
    void SetRowCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }
    void SetScopeCommitPageLimitForTests(std::size_t pages) noexcept {
        groupMemberScopes_.SetCommitPageLimitForTests(pages);
    }
    std::uint32_t LiveCount() const noexcept;
    std::uint32_t FreeCount() const noexcept;
    bool PrepareAuthoredControl(const AuthoredControlInput& input,
                                TimelineArmToken* outToken) noexcept;
    bool PreparePendingAuthoredControl(const PendingAuthoredControlInput& input,
                                      TimelineArmToken* outToken) noexcept;
    bool ProvePendingAuthoredControlCommit(TimelineArmToken token,
        const PendingAuthoredControlInput& expected, PreparedCommitProof* out) noexcept;
    bool ProvePendingAuthoredControlBinding(TimelineArmToken token,
        const TimelineArmReaderPin& sourceReader, const AuthoredControlInput& final,
        PendingAuthoredControlBindingProof* out) noexcept;
    static void ConsumePendingAuthoredControlBinding(
        PendingAuthoredControlBindingProof&& proof) noexcept;
    bool ProveAuthoredControlBucket(TimelineArmToken token,
        const TimelineArmReaderPin& reader,
        const TimelineTransactionProgramRef& program,
        std::uint64_t quantumBegin, std::uint32_t blockFrames,
        std::uint64_t audioSequence, const ScheduledEventInput* dueEvents,
        std::uint32_t dueCount, AuthoredControlBucketProof* out) noexcept;
    // True Lane claim precedes Apply; immutable Voice event publication precedes Release.
    static void ApplyAuthoredControlBucket(AuthoredControlBucketProof& proof) noexcept;
    bool Reserve(std::uint32_t renderSlot, std::uint32_t renderGeneration,
                 std::uint64_t startFrame, std::uint64_t armDeadline,
                 ProgramGeneration programGeneration,
                 const TimelineSemanticTarget& target,
                 TimelineArmToken* outToken) noexcept;
    bool Prepare(std::uint32_t renderSlot, std::uint32_t renderGeneration,
                 std::uint64_t startFrame, std::uint64_t armDeadline,
                 ProgramGeneration programGeneration,
                 const TimelineSemanticTarget& target,
                 TimelineArmToken* outToken) noexcept;
    bool ReserveGrouped(std::uint32_t renderSlot,
                        std::uint32_t renderGeneration,
                        std::uint64_t startFrame,
                        std::uint64_t armDeadline,
                        ProgramGeneration programGeneration,
                        const TimelineSemanticTarget& target,
                        ParentAudioGroupRef parentGroup,
                        std::uint16_t memberIndex,
                        TimelineArmToken* outToken) noexcept;
    bool PrepareGrouped(std::uint32_t renderSlot,
                        std::uint32_t renderGeneration,
                        std::uint64_t startFrame,
                        std::uint64_t armDeadline,
                        ProgramGeneration programGeneration,
                        const TimelineSemanticTarget& target,
                        ParentAudioGroupRef parentGroup,
                        std::uint16_t memberIndex,
                        TimelineArmToken* outToken) noexcept;
    bool ReservePendingImmediate(
        std::uint32_t renderSlot, std::uint32_t renderGeneration,
        ProgramGeneration programGeneration,
        const TimelineSemanticTarget& target,
        TimelineArmToken* outToken) noexcept;
    bool PreparePendingImmediate(
        std::uint32_t renderSlot, std::uint32_t renderGeneration,
        ProgramGeneration programGeneration,
        const TimelineSemanticTarget& target,
        TimelineArmToken* outToken) noexcept;
    bool ReservePendingImmediateGrouped(
        std::uint32_t renderSlot, std::uint32_t renderGeneration,
        ProgramGeneration programGeneration,
        const TimelineSemanticTarget& target,
        ParentAudioGroupRef parentGroup, std::uint16_t memberIndex,
        TimelineArmToken* outToken) noexcept;
    bool PreparePendingImmediateGrouped(
        std::uint32_t renderSlot, std::uint32_t renderGeneration,
        ProgramGeneration programGeneration,
        const TimelineSemanticTarget& target,
        ParentAudioGroupRef parentGroup, std::uint16_t memberIndex,
        TimelineArmToken* outToken) noexcept;
    bool BindPendingImmediateStart(TimelineArmToken token,
                                   std::uint64_t startFrame,
                                   std::uint64_t armDeadline) noexcept;
    bool ProvePrivatePendingImmediateArmBatch(
        const PrivatePendingImmediateArmInput* inputs,
        std::size_t inputCount, Row** reservedRows,
        std::size_t reservedCapacity,
        PrivatePendingImmediateArmProof* out) noexcept;
    static void ApplyPrivatePendingImmediateArmBatch(
        PrivatePendingImmediateArmProof& proof) noexcept;
    bool ProveBoundArmPublicationBatch(const BoundArmPublicationInput* inputs,
        std::size_t inputCount, Row** reservedRows, std::size_t reservedCapacity,
        BoundArmPublicationProof* out) noexcept;
    static void ApplyBoundArmPublicationBatch(BoundArmPublicationProof& proof) noexcept;
    bool CommitPrepared(TimelineArmToken token) noexcept;
    bool ProvePreparedCommit(const PreparedCommitInput& expected,
                             PreparedCommitProof* out) noexcept;
    static void ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept;
    bool PinPreparedVariableRead(const PreparedCommitProof& proof,
        VariableReadLifetime* out) noexcept;
    bool CancelPrepared(TimelineArmToken token) noexcept;
    // The caller has already cancelled the exact unpublished voice batch and
    // witnessed every slice invalidating directly from Ready.
    bool RetirePendingImmediateBatchExact(
        const PendingImmediateRetirementInput* inputs, std::size_t inputCount,
        Row** reservedRows, std::size_t reservedCapacity) noexcept;
    bool BeginUnpublishedPendingRetirement(
        const PendingImmediateRetirementInput* inputs, std::size_t inputCount,
        Row** reservedRows, std::size_t reservedCapacity,
        UnpublishedPendingRetirementProof* out) noexcept;
    bool AbortUnpublishedPendingRetirement(
        UnpublishedPendingRetirementProof* proof) noexcept;
    bool FinishUnpublishedPendingRetirement(
        UnpublishedPendingRetirementProof* proof) noexcept;
    enum class ControlCommandAckStatus : std::uint8_t {
        Applied, Cancelled, AlreadySettled, Stale, DeferredReservation, Invalid
    };
    // Only the accepted immutable TimelineControlArm command invokes this path.
    ControlCommandAckStatus AcknowledgeControlCommand(TimelineArmToken token,
        std::uint64_t originalCompletionSequence,
        std::uint64_t currentAudioSequence) noexcept;
    bool AcknowledgeApplied(std::uint64_t packedIdentity,
                            std::uint64_t completionSequence) noexcept;
    bool ResolveIdentity(std::uint64_t packedIdentity,
                         TimelineArmToken* outToken) const noexcept;
    void SetResolveIdentityHookForTests(void (*hook)(void*), void* context) {
        resolveIdentityHookContext_.store(context, std::memory_order_release);
        resolveIdentityHook_.store(hook, std::memory_order_release);
    }
    bool AcknowledgeApplied(TimelineArmToken token,
                            std::uint64_t completionSequence = 0) noexcept;
    void SetTokenAcknowledgeHookForTests(void (*hook)(void*), void* context) noexcept {
        tokenAcknowledgeHookContext_.store(context, std::memory_order_relaxed);
        tokenAcknowledgeHook_.store(hook, std::memory_order_release);
    }
    bool Publish(TimelineArmToken token) noexcept;
    ArmPublishStatus PublishExact(TimelineArmToken token) noexcept;
    bool ClaimAudioSlice(TimelineArmToken token) noexcept;
    bool ProveLaneStart(TimelineArmToken token,
                        std::uint64_t executeFrame,
                        std::uint64_t audioSequence,
                        LaneAudioProof* out,
                        ProgramGeneration programGeneration = 0,
                        const TimelineSemanticTarget* target = nullptr) noexcept;
    bool ProveLaneCancelledStart(const TimelineArmReaderPin& reader,
        const ConsumedEvent& original, ProgramGeneration armGeneration,
        std::uint64_t audioSequence, LaneAudioProof* out) noexcept;
    static bool LaneStartCancelled(const LaneAudioProof& proof) noexcept;
    bool ProveLaneFutureStart(TimelineArmToken token,
                              const FrontierGroupBinding& binding,
                              std::uint16_t memberIndex,
                              std::uint64_t executeFrame,
                              std::uint64_t audioSequence,
                              LaneAudioProof* out,
                              ProgramGeneration programGeneration,
                              const TimelineSemanticTarget& target,
                              bool entryInSameBucket = false) noexcept;
    bool MatchesHeldLaneFutureStart(const LaneAudioProof& proof,
        const FrontierGroupBinding& binding, std::uint16_t memberIndex,
        TimelineArmToken token, std::uint64_t executeFrame,
        std::uint64_t audioSequence, ProgramGeneration programGeneration,
        const TimelineSemanticTarget& target, bool entryInSameBucket) const noexcept;
    bool MatchesHeldLaneStart(const LaneAudioProof& proof, TimelineArmToken token,
        std::uint64_t executeFrame, std::uint64_t audioSequence,
        ProgramGeneration programGeneration, const TimelineSemanticTarget& target) const noexcept;
    static void ApplyLaneStart(LaneAudioProof& proof) noexcept;
    void ApplyClaimedAudioSlice(TimelineArmToken token) noexcept;
    bool RequestCancelAppliedAudioSlice(
        TimelineArmToken token) noexcept;
    void CompleteClaimedAudioSlice(
        TimelineArmToken token,
        std::uint64_t audioSequence) noexcept;
    bool Start(TimelineArmToken token) noexcept;
    bool Start(TimelineArmToken token,
               std::uint64_t blockSequence) noexcept;
    bool PinFrontierReader(TimelineArmToken token, TimelineArmReaderPin* out) noexcept;
    bool BindFrontierAdmission(TimelineArmToken token, const FrontierGroupBinding& binding,
                              AudioSliceLease& lease) noexcept;
    bool UnbindPrivatePendingFrontierAdmission(TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions) noexcept;
    bool BindPrivatePendingFrontierStart(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) noexcept;
    bool PrivatePendingFrontierStartReady(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) const noexcept;
    bool BindPrivatePendingQueueFrontierStart(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) noexcept;
    bool PrivatePendingQueueFrontierStartReady(const TimelineArmReaderPin& ownedReader,
        TimelineArmToken expectedArm, const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) const noexcept;
    bool BindPrivatePendingQueueFrontierControl(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) noexcept;
    bool PrivatePendingQueueFrontierControlReady(const TimelineArmReaderPin& ownedReader,
        TimelineArmToken expectedArm, const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal) const noexcept;
    bool UnbindPrivatePendingQueueFrontierAdmission(TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue) noexcept;
    std::uint64_t Incarnation() const noexcept { return incarnation_; }
    bool WitnessUnpublishedFrontierRetirement(
        TimelineArmToken token, const FrontierGroupBinding& binding,
        const AudioSliceLease& lease) const noexcept;
    bool FrontierAdmissionReleasedForRetirement(
        TimelineArmToken token, const FrontierGroupBinding& binding,
        const AudioSliceLease& lease) const noexcept;
    bool PrivateFrontierAdmissionReady(TimelineArmToken token,
        const FrontierGroupBinding& binding, AudioSliceLease& lease) noexcept;
    bool ProveBoundFrontierArm(TimelineArmToken token, const FrontierEntryReceiptRef& entry,
        std::uint64_t audioSequence, bool starts, BoundAudioProof* out,
        const FrontierGroupBinding* expectedBinding = nullptr) noexcept;
    bool ProveGroupedFrontierOperation(TimelineArmToken token, const FrontierGroupBinding& binding,
        AudioSliceLease& lease, const ScheduledTransactionPool& pool,
        std::uint64_t audioSequence, bool starts, BoundAudioProof* out) noexcept;
    bool ProveDeferredFrontierStart(TimelineArmToken token, const FrontierGroupBinding& binding,
        AudioSliceLease& lease, const ScheduledTransactionPool& pool,
        std::uint64_t audioSequence, BoundAudioProof* out) noexcept;
    bool ProveLaneGroupedFutureStart(TimelineArmToken token, const FrontierGroupBinding& binding,
        AudioSliceLease& lease, const ScheduledTransactionPool& pool,
        std::uint64_t audioSequence, std::uint16_t memberIndex,
        ProgramGeneration programGeneration, const TimelineSemanticTarget& target,
        BoundAudioProof* out) noexcept;
    static void ApplyBoundFrontierArm(BoundAudioProof& proof, bool completesControl = false) noexcept;
    TimelineArmOperationDisposition BeginFrontierOperation(TimelineArmToken token,
        const FrontierDueOperationRef& operation, TimelineArmOperationKind kind,
        std::uint64_t audioSequence, AudioOperationClaim* out,
        const FrontierEventBinding* binding = nullptr,
        const ScheduledTransactionPool* pool = nullptr) noexcept;
    static void ApplyFrontierOperation(AudioOperationClaim& claim) noexcept;
    static void CancelFrontierOperation(AudioOperationClaim& claim) noexcept;
    static void FinishFrontierOperation(AudioOperationClaim& claim, bool completesControl = false) noexcept;
    bool AcknowledgeLogicalComplete(
        TimelineArmToken token,
        std::uint64_t completionSequence) noexcept;
    bool RequestCancel(TimelineArmToken token, bool* newlyCancelled = nullptr) noexcept;
    bool RequestCancelNeverPublished(TimelineArmToken token) noexcept;
    void SetNeverPublishedCancelHookForTests(
        void (*hook)(void*), void* context) noexcept {
        neverPublishedCancelHookContext_.store(context, std::memory_order_relaxed);
        neverPublishedCancelHook_.store(hook, std::memory_order_release);
    }
    bool RequestCancelStartedVoice(TimelineArmToken token) noexcept;
    bool Tombstone(TimelineArmToken token,
                   std::uint64_t audioSequence) noexcept;
    bool Reclaim(TimelineArmToken token,
                 std::uint64_t observedAudioSequence) noexcept;
    std::uint32_t ReclaimTombstoned(
        std::uint64_t observedAudioSequence) noexcept;
    bool HasLiveGeneration(ProgramGeneration generation) const noexcept;
    bool State(TimelineArmToken token, ArmState* outState) const noexcept;
    bool AckSnapshot(TimelineArmToken token, ArmState* outState,
                     std::uint64_t* outCompletionSequence) const noexcept;
    bool Observe(TimelineArmToken token,
                 TimelineArmObservation* out) const noexcept;
    // Producer-only query for a cancelled member with no audio publication.
    bool HasUnpublishedTombstoneExact(TimelineArmToken token,
        ParentAudioGroupRef parent, std::uint16_t memberIndex) noexcept;
    // Producer-only mapping query; an assigned retirement sequence is not an audio read.
    bool HasUnobservedTombstoneExact(TimelineArmToken token,
        ParentAudioGroupRef parent, std::uint16_t memberIndex) noexcept;

    bool Snapshot(TimelineArmToken token, ArmState* outState,
                  TimelineSemanticTarget* outTarget) noexcept;
    const Row* Resolve(TimelineArmToken token) const noexcept;
    Row* Resolve(TimelineArmToken token) noexcept;
    const Row* RowAt(std::uint32_t slot) const noexcept {
        return slot < rows_.ConstructedCount() ? &rows_[slot] : nullptr;
    }
    Row* RowAt(std::uint32_t slot) noexcept {
        return slot < rows_.ConstructedCount() ? &rows_[slot] : nullptr;
    }

private:
    friend class OriginalSourceSubmissionRegistry;
    friend class MixerLoop;
    const Row* CapturePrivatePendingFrontierStart(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal, std::uint64_t* startFrame) const noexcept;
    const Row* CapturePrivatePendingQueueFrontierStart(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal, std::uint64_t* startFrame) const noexcept;
    const Row* CapturePrivatePendingQueueFrontierEvent(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        const OriginalQueueFutureGroupRegistration& original, const OriginalQueueFutureGroupRegistration& outer,
        const ImmediateActionLane& lane, const PrivateImmediateLanePlacementProof& placement,
        std::uint32_t eventOrdinal, bool control, std::uint64_t* frame) const noexcept;
    const Row* CapturePrivatePendingFrontierStartRow(const TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease, const Plan03PageGroupView& native,
        const ImmediateLaneEvent& placed, std::uint64_t anchor, std::uint64_t* startFrame) const noexcept;
    bool UnbindPrivatePendingFrontierAdmissionRow(TimelineArmReaderPin& ownedReader,
        const FrontierGroupBinding& binding, AudioSliceLease& lease, const Plan03PageGroupView& native) noexcept;
    bool FinalSourcePreparationEligible(const PreparedCommitProof& preparation) const noexcept;
    bool IssuePublicationAfterInstallation(const PreparedCommitProof& preparation,
        const OriginalArmRowOwner& original, const void* submission,
        PublicationAfterInstallation* out) noexcept;
    static bool MatchesPublicationAfterInstallation(const SourceCommandRowProof& proof,
        const PublicationAfterInstallation& publication) noexcept;
    static bool ConsumePublicationAfterInstallation(SourceCommandRowProof& proof,
        PublicationAfterInstallation& publication) noexcept;
    bool IssueGroupedPublicationAfterInstallation(const OriginalArmRowOwner& original,
        const void* submission, ArmGroupTable& groups, ArmGroupToken group,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        GroupedPublicationAfterInstallation* out) noexcept;
    bool IssueQueueGroupedPublicationAfterInstallation(const OriginalArmRowOwner& original,
        const void* submission, ArmGroupTable& groups, ArmGroupToken group,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const DecisionTable& decisions,
        const PrivatePendingQueueFrontierBinding& queue,
        GroupedPublicationAfterInstallation* out) noexcept;
    bool IssueAuthenticatedGroupedPublicationAfterInstallation(const OriginalArmRowOwner& original,
        const void* submission, ArmGroupTable& groups, ArmGroupToken group,
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        GroupedPublicationAfterInstallation* out) noexcept;
    static bool MatchesGroupedPublicationAfterInstallation(const SourceCommandRowProof& proof,
        const GroupedPublicationAfterInstallation& publication) noexcept;
    enum class GroupedInstallationPublicationResult : std::uint8_t { Published, CancelRequired, Refused };
    static GroupedInstallationPublicationResult ConsumeGroupedPublicationAfterInstallation(
        SourceCommandRowProof& proof, GroupedPublicationAfterInstallation& publication) noexcept;
    friend class PrivateSelectedQueueFutureSpanPool;
    bool PreparePendingAuthoredControlInternal(const PendingAuthoredControlInput& input,
        ParentAudioGroupRef selectedSourceGroup, TimelineArmToken* outToken) noexcept;
    bool ProvePendingAuthoredControlBindingInternal(TimelineArmToken token,
        const TimelineArmReaderPin& sourceReader, const AuthoredControlInput& final,
        ParentAudioGroupRef selectedSourceGroup, PendingAuthoredControlBindingProof* out) noexcept;
    const Row* PinnedAuthoredControlSource(const Row& control) const noexcept;
    bool BindOriginalSourceEpisode(const PreparedCommitProof& preparation,
        const OriginalArmRowOwner& original, const void* submission, std::uint64_t sourceEpisode) noexcept;
    SourceCommandRowStatus RequestOriginalPendingSourceRetirement(
        const OriginalArmRowOwner& original, const void* submission,
        std::uint64_t sourceEpisode) noexcept;
    bool OriginalPendingSourceRetirementSettled(const OriginalArmRowOwner& original,
        const void* submission, std::uint64_t sourceEpisode) const noexcept;
    bool IssueOriginalUnpublishedSourceDisposition(const OriginalArmRowOwner& original,
        const void* submission, std::uint64_t sourceEpisode, std::uint64_t originalMarker,
        std::uint64_t sourceIdentity, OriginalUnpublishedSourceDisposition* out) const noexcept;
    friend class TimelineArmReaderPin;
    friend class ImmediateActionLane;
    friend class ScheduledTransactionPool;
    friend class PrivateUnboundPageArmRetirementProof;
    friend class UnpublishedGroupArmCancellationProof;
    UnpublishedGroupArmCancellationProof* unpublishedGroupArmCancellations_ = nullptr;
    PrivateUnboundPageArmRetirementProof* privateUnboundPageArmRetirements_ = nullptr;
    // The caller must have committed an exact old-arm witness before using this proof.
    bool FrontierArmReclaimedAfterRetirementWitness(
        TimelineArmToken token, std::uint64_t witnessedIncarnation) const noexcept;
    bool ReleaseFrontierReader(TimelineArmToken token,
        std::uint64_t completedAudioSequence) noexcept;
    const Row* PinnedPlainRow(const TimelineArmReaderPin& pin) const noexcept;
    bool PinnedPlainSourceStartMatches(const TimelineArmReaderPin& pin,
        const ConsumedEvent& event, ProgramGeneration armGeneration) const noexcept;
    const AuthoredControlMetadata* PinnedPlainControlMatches(
        const TimelineArmReaderPin& pin, const ConsumedEvent& event,
        const AuthoredStopOperation* operation) const noexcept;
    void ReleaseWithdrawnPlainLaneReader(TimelineArmReaderPin& pin) noexcept;
    bool ReleasePendingControlSourceReader(Row& control, TimelineArmToken token,
        std::uint64_t completedAudioSequence) noexcept;
    bool ReserveInternal(std::uint32_t renderSlot,
                         std::uint32_t renderGeneration,
                         std::uint64_t startFrame,
                         std::uint64_t armDeadline,
                         ProgramGeneration programGeneration,
                         const TimelineSemanticTarget& target,
                         ParentAudioGroupRef parentGroup,
                         std::uint16_t memberIndex,
                         bool grouped, bool prepared, bool pendingImmediateStart,
                         TimelineArmToken* outToken,
                         const AuthoredControlMetadata* control = nullptr) noexcept;
    static void ReleaseAuthoredControlProof(AuthoredControlBucketProof& proof) noexcept;

    // Producer-only issuance bookkeeping; audio never reads or resets these records.
    class GroupMemberReceiver {
        friend class TimelineArmTable;
        friend class ScheduledTransactionPool;
        friend struct detail::OriginalGroupMemberTestAccess;
        const TimelineArmTable* owner_ = nullptr;
        const ScheduledTransactionPool* pool_ = nullptr;
        GroupMemberReceiver* previous_ = nullptr;
        GroupMemberReceiver* next_ = nullptr;
        std::uint64_t activeCount_ = 0;
        std::uint64_t episode_ = 0;
        std::uint64_t lastDetachedEpisode_ = 0;
    };
    // The pool owns one stable origin per group until its last original is returned.
    struct OriginalGroupIdentity {
        Plan03GroupSliceIdentity group{};
        GroupMemberReceiver* receiver = nullptr;
        std::uint64_t armTableIncarnation = 0;
        std::uint64_t receiverEpisode = 0;
        std::uint32_t liveMembers = 0;
    };
    class OriginalGroupMember {
        friend class TimelineArmTable;
        friend class ScheduledTransactionPool;
        friend struct detail::OriginalGroupMemberTestAccess;
        TimelineArmToken arm_{};
        OriginalGroupIdentity* groupIdentity_ = nullptr;
        GroupMemberReceiver* Receiver() const noexcept {
            return groupIdentity_ == nullptr ? nullptr : groupIdentity_->receiver;
        }
        std::uint64_t ArmTableIncarnation() const noexcept {
            return groupIdentity_ == nullptr ? 0 : groupIdentity_->armTableIncarnation;
        }
        std::uint64_t ReceiverEpisode() const noexcept {
            return groupIdentity_ == nullptr ? 0 : groupIdentity_->receiverEpisode;
        }
        const Plan03GroupSliceIdentity& Group() const noexcept {
            assert(groupIdentity_ != nullptr);
            return groupIdentity_->group;
        }
        std::uint64_t serial_ = 0;
        std::uint16_t memberIndex_ = UINT16_MAX;
    };
    struct GroupMemberScope {
        TimelineArmToken arm{};
        TimelineArmToken sourceFactoryArm{};
        bool unparentedPreparedSourceFactory = false;
        std::uint64_t activeCount = 0;
        bool jointHeld = false;
    };
    bool IssueOriginalGroupMember(GroupMemberReceiver& receiver,
        const ScheduledTransactionPool& pool, OriginalGroupIdentity& identity,
        TimelineArmToken arm, OriginalGroupMember* out) const noexcept;
    void ReturnOriginalGroupMember(OriginalGroupMember& original) const noexcept;
    void DetachGroupMemberReceiver(GroupMemberReceiver& receiver) const noexcept;
    bool CompleteOriginalGroupMemberCoverage(TimelineArmToken arm,
        const OriginalGroupMember* const* originals, std::size_t count) const noexcept;
    friend struct detail::OriginalGroupMemberTestAccess;
    mutable StableRowStorage<GroupMemberScope> groupMemberScopes_;
    mutable GroupMemberReceiver* groupMemberReceivers_ = nullptr;
    mutable std::uint64_t nextGroupMemberSerial_ = 0;

    mutable StableRowStorage<Row> rows_;
    bool PrepareRowDependencies(std::uint32_t count) noexcept;
    bool ConstructRowsThrough(std::uint32_t count) noexcept;
    void* rowStorageContext_ = nullptr;
    bool (*prepareRowStorage_)(void*, std::uint32_t) noexcept = nullptr;
    std::uint64_t acceptedSourceTail_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint64_t incarnation_ = 0;
    std::atomic<std::uint32_t> liveSourceCommandProofs_{0};
    std::atomic<std::uint64_t> liveOriginalArmRowOwners_{0};
    mutable std::atomic<void (*)(void*)> resolveIdentityHook_{nullptr};
    mutable std::atomic<void*> resolveIdentityHookContext_{nullptr};
    std::atomic<void (*)(void*)> tokenAcknowledgeHook_{nullptr};
    std::atomic<void*> tokenAcknowledgeHookContext_{nullptr};
    std::atomic<void (*)(void*)> neverPublishedCancelHook_{nullptr};
    std::atomic<void*> neverPublishedCancelHookContext_{nullptr};
};

static_assert(std::atomic<ArmState>::is_always_lock_free,
              "timeline arm state must be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "timeline arm identity must be lock-free");

} // namespace harken

#endif // HARKEN_CORE_TIMELINE_ARM_TABLE_H
