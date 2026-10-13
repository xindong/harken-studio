#ifndef HARKEN_CORE_DECISION_TABLE_H
#define HARKEN_CORE_DECISION_TABLE_H

#include "harken/core/CurrentWorld.h"
#include "harken/core/DecisionToken.h"
#include "harken/core/FrontierActivation.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/TimeCoordinates.h"
#include "harken/core/TimelineProgram.h"
#include "harken/core/TransitionInstancePool.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace harken {

inline constexpr std::uint32_t kMaxDecisionRowsHard = 65536;
inline constexpr std::uint32_t kMaxParticipantsPerDecisionHard = 2048;

enum class DecisionCommitState : std::uint8_t {
    Open = 0,
    CommitSealed = 1,
    Committed = 2,
    Cancelled = 3,
    PreparedTicketHeld = 4,
};

enum class AudioGroupState : std::uint8_t {
    Pending = 0,
    AudioClaimed = 1,
    Admitted = 2,
    Invalidated = 3,
    DeadlineInvalidated = 4,
    PostStartInvalidated = 5,
    PostStartDeadlineInvalidated = 6,
    Retired = 7,
};

struct PreparedGroupToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

using TransitionAudibleGateRef = TransitionInstanceToken;

enum class DecisionParticipantKind : std::uint8_t {
    Invalid = 0,
    Occurrence = 1,
    TimelineArm = 2,
    Property = 3,
    Variable = 4,
    Transition = 5,
    QueueInstance = 6,
    Subdecision = 7,
};

struct DecisionParticipantRef {
    DecisionParticipantKind kind = DecisionParticipantKind::Invalid;
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return kind != DecisionParticipantKind::Invalid &&
               slot != UINT32_MAX && generation != 0;
    }
};

struct DecisionReserveRequest {
    LogicCommitTicket ticket{};
    WorldGeneration world = kNoWorldGeneration;
    TransportEpoch transport = kInvalidTransportEpoch;
    std::uint64_t expectedContinuationGeneration = 0;
    std::uint16_t sliceCount = 0;
    TransitionAudibleGateRef transitionGate{};
};

struct DecisionCommitPayload {
    const DecisionParticipantRef* participants = nullptr;
    std::uint32_t participantCount = 0;
    PreparedGroupToken pages{};
    ArmGroupToken armGroup{};
};

struct DecisionCommittedSnapshot {
    DecisionCommitState commit = DecisionCommitState::Open;
    LogicCommitTicket ticket{};
    WorldGeneration world = kNoWorldGeneration;
    TransportEpoch transport = kInvalidTransportEpoch;
    std::uint64_t continuationGeneration = 0;
    AudioGroupState audioState = AudioGroupState::Pending;
    std::uint16_t sliceCount = 0;
    std::uint16_t consumedSlices = 0;
    std::uint32_t participantCount = 0;
    PreparedGroupToken pages{};
    ArmGroupToken armGroup{};
    std::uint32_t futurePinCount = 0;
    std::uint64_t lastAudioSequence = 0;
    TransitionAudibleGateRef transitionGate{};
    bool authoredStopInvalidated = false;
};

struct FrontierActivationSidecarSnapshot {
    FrontierReceiptState state = FrontierReceiptState::Empty;
    Plan03GroupSliceIdentity primary{};
    bool pagePinHeld = false;
    bool observerPinHeld = false;
    FrontierReceiptRead receiptRead = FrontierReceiptRead::Stale;
    FrontierActivationReceipt receipt{};
};

enum class DecisionDeadlineDisposition : std::uint8_t {
    NewlyInvalidated = 0,
    AlreadyInvalidated = 1,
    NotEligible = 2,
    Stale = 3,
    DeferredByFrontier = 4,
};

enum class DecisionInvalidationDisposition : std::uint8_t {
    NewlyInvalidated = 0,
    AlreadyInvalidated = 1,
    NotEligible = 2,
    Stale = 3,
    DeferredByFrontier = 4,
};

class DecisionTable;
class ScheduledTransactionPool;
namespace detail { struct DecisionOriginalLifetimeTestAccess; struct QueueDecisionIssuerLimitTestAccess; }

// A structural loan issued only by the actual Decision row factory.
// It grants no execution or physical Current authority.
class OriginalDecisionLifetimePin {
public:
    OriginalDecisionLifetimePin() = default;
    ~OriginalDecisionLifetimePin();
    OriginalDecisionLifetimePin(const OriginalDecisionLifetimePin&) = delete;
    OriginalDecisionLifetimePin& operator=(const OriginalDecisionLifetimePin&) = delete;
    OriginalDecisionLifetimePin(OriginalDecisionLifetimePin&& other) noexcept;
    OriginalDecisionLifetimePin& operator=(OriginalDecisionLifetimePin&& other) noexcept;
    bool Valid() const noexcept { return owner_ != nullptr; }
    DecisionToken Token() const noexcept { return Valid() ? token_ : DecisionToken{}; }
    const DecisionTable* Owner() const noexcept { return owner_; }
    bool Borrow(OriginalDecisionLifetimePin* out) const noexcept;
    bool Release() noexcept;
private:
    friend class DecisionTable;
    DecisionTable* owner_ = nullptr;
    DecisionToken token_{};
    std::uint64_t marker_ = 0;
};

class FrontierDecisionApplyProof {
public:
    bool Valid() const noexcept { return row_ != nullptr; }

private:
    friend class DecisionTable;
    void* row_ = nullptr;
    const DecisionTable* table_ = nullptr;
    FrontierActivation* activation_ = nullptr;
    AudioSliceLease* lease_ = nullptr;
    std::uint64_t audioSequence_ = 0;
    std::uint16_t sliceIndex_ = 0;
    bool prelude_ = false;
};

class DecisionFuturePin {
public:
    DecisionFuturePin() = default;
    ~DecisionFuturePin();
    DecisionFuturePin(const DecisionFuturePin&) = delete;
    DecisionFuturePin& operator=(const DecisionFuturePin&) = delete;
    DecisionFuturePin(DecisionFuturePin&& other) noexcept;
    DecisionFuturePin& operator=(DecisionFuturePin&& other) noexcept;
    bool Valid() const noexcept { return owner_ != nullptr; }
    DecisionToken Token() const noexcept { return token_; }
    const DecisionTable* Owner() const noexcept { return owner_; }
    bool Release() noexcept;

private:
    friend class DecisionTable;
    DecisionTable* owner_ = nullptr;
    DecisionToken token_{};
};

class PreparedDecisionReservation {
public:
    PreparedDecisionReservation() = default;
    PreparedDecisionReservation(const PreparedDecisionReservation&) = delete;
    PreparedDecisionReservation& operator=(
        const PreparedDecisionReservation&) = delete;
    PreparedDecisionReservation(PreparedDecisionReservation&& other) noexcept;
    PreparedDecisionReservation& operator=(PreparedDecisionReservation&& other) noexcept;
    const DecisionReserveRequest& OriginalRequest() const noexcept { return originalRequest_; }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    DecisionToken Token() const noexcept {
        return Valid() ? token_ : DecisionToken{};
    }

private:
    friend class DecisionTable;
    DecisionTable* owner_ = nullptr;
    DecisionToken token_{};
    bool prepared_ = false;
    DecisionReserveRequest originalRequest_{};
    std::uint64_t originalLifetimeMarker_ = 0;
    TransitionAudibleGateRef attachedTransitionGate_{};
    std::uint16_t attachedSliceCount_ = 0;
};

class DecisionTable {
    struct Row;
public:
    class PreparedTicketCommitProof {
    public:
        PreparedTicketCommitProof() = default;
        ~PreparedTicketCommitProof() { Release(); }
        PreparedTicketCommitProof(const PreparedTicketCommitProof&) = delete;
        PreparedTicketCommitProof& operator=(const PreparedTicketCommitProof&) = delete;
        PreparedTicketCommitProof(PreparedTicketCommitProof&& other) noexcept;
        PreparedTicketCommitProof& operator=(PreparedTicketCommitProof&& other) noexcept;
        bool Valid() const noexcept { return row_ != nullptr && original_ != nullptr; }
        bool Associated(const DecisionTable* table, const PreparedDecisionReservation* original) const noexcept {
            return Valid() && table_ == table && original_ == original;
        }
        DecisionToken Token() const noexcept { return token_; }
        LogicCommitSequence Sequence() const noexcept { return sequence_; }
        void Release() noexcept;
    private:
        friend class DecisionTable;
        DecisionTable* table_ = nullptr;
        Row* row_ = nullptr;
        PreparedDecisionReservation* original_ = nullptr;
        DecisionToken token_{};
        LogicCommitSequence sequence_ = 0;
    };
    bool ProvePreparedReservationCommit(PreparedDecisionReservation& original,
        const DecisionReserveRequest& originalInput, LogicCommitSequence expectedSequence,
        PreparedTicketCommitProof* out) noexcept;
    static void ConsumePreparedReservationCommit(PreparedTicketCommitProof&& proof) noexcept;
    bool PreparedTicketHeld(DecisionToken token) const noexcept;
    DecisionTable(std::uint32_t capacity,
                  std::uint32_t maxParticipantsPerDecision);
    ~DecisionTable();
    DecisionTable(const DecisionTable&) = delete;
    DecisionTable& operator=(const DecisionTable&) = delete;
    DecisionTable(DecisionTable&&) = delete;
    DecisionTable& operator=(DecisionTable&&) = delete;

    const TimelineProgramView* PrepareFrontierControlProgram(DecisionToken token,
        std::uint64_t programGeneration) noexcept;
    bool CaptureFrontierControlProgram(DecisionToken token,
        TimelineTransactionProgramRef* out) const noexcept;

    bool PrepareFrontierActivation(DecisionToken token,
        const FrontierActivationDescriptor& descriptor,
        ScheduledTransactionPool& pool) noexcept;
    bool CanPublishFrontierActivation(DecisionToken token) const noexcept;
    bool PublishFrontierActivation(DecisionToken token) noexcept;
    bool CancelPreparedFrontierActivation(DecisionToken token) noexcept;
    FrontierActivation* ResolveFrontierActivation(DecisionToken token) noexcept;
    const FrontierActivation* ResolveFrontierActivation(DecisionToken token) const noexcept;
    bool CaptureFrontierActivationSidecar(DecisionToken token,
        const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
        FrontierActivationSidecarSnapshot* out) const noexcept;
    bool ReconcileFrontierActivation(DecisionToken token,
        const FrontierEntryReceiptRef& entry, std::uint64_t completedAudioSequence,
        FrontierActivationReceipt* out) noexcept;
    std::uint64_t FrontierStorageBytes() const noexcept;
    bool ValidateFrontierDecisionApply(DecisionToken token, std::uint64_t audioSequence,
                                      FrontierDecisionApplyProof* out) noexcept;
    void ApplyClaimedFrontierDecision(const FrontierDecisionApplyProof& proof) noexcept;
    bool BindFrontierDecision(DecisionToken token, const FrontierGroupBinding& binding,
        const ScheduledTransactionPool& pool) noexcept;
    bool ReleaseFrontierDecisionBinding(DecisionToken token) noexcept;
    bool HasFrontierDecisionBinding(DecisionToken token) const noexcept;
    // Observes the actual binding receiver and original full group scope.
    // It does not claim audio, mutate timing or issue a replacement grant.
    bool IsFrontierDecisionBindingFor(DecisionToken token,
        const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) const noexcept;
    bool CanRollbackPreparedFrontierDecision(DecisionToken token, DecisionToken coordinator,
        const FrontierGroupBinding* binding = nullptr) const noexcept;
    bool RollbackPreparedFrontierDecision(DecisionToken token, DecisionToken coordinator,
        const FrontierGroupBinding* binding = nullptr) noexcept;
    bool ValidateFrontierMemberDecisionApply(const FrontierGroupBinding& binding,
        AudioSliceLease& lease, std::uint64_t audioSequence, FrontierDecisionApplyProof* out) noexcept;

    std::uint32_t Capacity() const noexcept { return capacity_; }
    bool Valid() const noexcept {
        return capacity_ != 0 && maxParticipantsPerDecision_ != 0;
    }
    std::uint32_t MaxParticipantsPerDecision() const noexcept {
        return maxParticipantsPerDecision_;
    }
    std::uint32_t LiveCount() const noexcept;
    std::uint32_t QuarantinedCount() const noexcept;

    bool Reserve(const DecisionReserveRequest& request,
                 DecisionToken* outToken,
                 OriginalDecisionLifetimePin* outOriginal = nullptr) noexcept;
    bool PrepareReservation(
        const DecisionReserveRequest& request,
        PreparedDecisionReservation* out,
        OriginalDecisionLifetimePin* outOriginal = nullptr) noexcept;
    bool CommitPreparedReservation(
        PreparedDecisionReservation* prepared,
        LogicCommitSequence sequence,
        DecisionToken* outToken) noexcept;
    using OriginalLifetimeBaselineResolver = OriginalDecisionLifetimePin* (*)(
        void*, DecisionTable&, DecisionToken) noexcept;
    // The resolver may return only an already issued owner baseline.
    bool RegisterOriginalLifetimeBaselineResolver(void* context,
        OriginalLifetimeBaselineResolver resolver) noexcept;
    bool CancelPreparedReservation(
        PreparedDecisionReservation* prepared,
        OriginalDecisionLifetimePin* originalBaseline = nullptr) noexcept;
    bool IsOpen(DecisionToken token) const noexcept;
    bool OwnsOpenPreparedReservation(const PreparedDecisionReservation& original) const noexcept {
        return original.Valid() && original.owner_ == this &&
            MatchesOpenReservation(original.token_, original.originalRequest_);
    }
    bool MatchesOpenReservation(DecisionToken token,
                                const DecisionReserveRequest& request) const noexcept;
    bool MatchesCancelledReservation(DecisionToken token,
                                    const DecisionReserveRequest& request) const noexcept;
    bool IsCancelled(DecisionToken token) const noexcept;
    bool Contains(DecisionToken token) const noexcept { return Resolve(token) != nullptr; }
    // The native held producer may attach runtime gating before any original loan.
    // The immutable factory input and its lifetime marker remain unchanged.
    bool AttachPreparedTransitionGateBeforeCommit(
        PreparedDecisionReservation& original,
        TransitionAudibleGateRef transitionGate,
        std::uint16_t sliceCount) noexcept;
    bool AttachTransitionGateBeforeCommit(
        DecisionToken token,
        TransitionAudibleGateRef transitionGate,
        std::uint16_t sliceCount) noexcept;
    bool Commit(DecisionToken token,
                const DecisionCommitPayload& payload) noexcept;
    bool Cancel(DecisionToken token) noexcept;
    bool ReclaimCancelled(DecisionToken token,
        OriginalDecisionLifetimePin* originalBaseline = nullptr) noexcept;
    bool TryClaimAudio(DecisionToken token) noexcept;
    void ApplyClaimedAudioSlice(DecisionToken token) noexcept;
    bool TryInvalidatePending(DecisionToken token) noexcept;
    bool MarkAudioAdmitted(DecisionToken token) noexcept;
    bool RecordAppliedSlice(DecisionToken token, PreparedGroupToken pages,
                            std::uint16_t sliceIndex,
                            std::uint64_t audioSequence) noexcept;
    bool Retire(DecisionToken token,
                std::uint64_t audioSequence) noexcept;
    bool ReclaimRetired(DecisionToken token,
        OriginalDecisionLifetimePin* originalBaseline = nullptr) noexcept;
    // Predicates authenticate the immutable original family, not execution state.
    bool IsOriginalDecisionLifetimePinFor(const OriginalDecisionLifetimePin& original,
        DecisionToken token) const noexcept;
    bool MatchesOriginalPreparedDecision(const OriginalDecisionLifetimePin& original,
        const PreparedDecisionReservation& preparation) const noexcept;
    bool MatchesOriginalDecisionFactory(const OriginalDecisionLifetimePin& original,
        const DecisionReserveRequest& request) const noexcept;
    // Acquire and release pins on the logic thread; audio only uses pinned rows.
    bool AcquireFuturePin(DecisionToken token, DecisionFuturePin* out) noexcept;
    // Producer-only retention transfer; never restores the Decision's execution state.
    bool CloneFuturePin(const DecisionFuturePin& source, DecisionFuturePin* out) noexcept;
    DecisionDeadlineDisposition InvalidateForDeadline(
        DecisionToken token) noexcept;
    DecisionInvalidationDisposition Invalidate(
        DecisionToken token) noexcept;
    DecisionInvalidationDisposition InvalidateForAuthoredStop(
        DecisionToken token) noexcept;

    bool CaptureCommitted(DecisionToken token,
                          DecisionCommittedSnapshot* out) const noexcept;
    bool CopyCommittedParticipants(DecisionToken token,
                                   DecisionParticipantRef* out,
                                   std::uint32_t capacity,
                                   std::uint32_t* outCount) const noexcept;
    bool CaptureCommittedParticipant(DecisionToken token, std::uint32_t index,
                                     DecisionParticipantRef* out) const noexcept;
    bool HasExclusiveParticipant(DecisionToken token, DecisionParticipantRef participant) const noexcept;

    bool SetGenerationForTests(std::uint32_t slot,
                               std::uint32_t generation) noexcept;
    bool SetFuturePinCountForTests(DecisionToken token, std::uint32_t count) noexcept;

private:
    friend class PrivateSelectedQueueFutureSpanPool;
    friend class PreparedDecisionReservation;
    friend class DecisionFuturePin;
    friend class OriginalDecisionLifetimePin;
    friend struct detail::DecisionOriginalLifetimeTestAccess;
    friend struct detail::QueueDecisionIssuerLimitTestAccess;
    OriginalDecisionLifetimePin* ResolveOriginalLifetimeBaseline(DecisionToken token,
        OriginalDecisionLifetimePin* explicitBaseline) noexcept;
    void* originalLifetimeBaselineContext_ = nullptr;
    OriginalLifetimeBaselineResolver originalLifetimeBaselineResolver_ = nullptr;
    bool CanIssueOriginalDecisionLifetime(const OriginalDecisionLifetimePin* out) const noexcept;
    void IssueOriginalDecisionLifetime(Row& row, DecisionToken token,
        const DecisionReserveRequest& request, OriginalDecisionLifetimePin& out) noexcept;
    bool MatchesOriginalDecisionLifetime(const OriginalDecisionLifetimePin& original) const noexcept;
    bool BorrowOriginalDecisionLifetime(const OriginalDecisionLifetimePin& original,
        OriginalDecisionLifetimePin* out) noexcept;
    bool ReleaseOriginalDecisionLifetime(OriginalDecisionLifetimePin& original) noexcept;
    bool OriginalDecisionRetirementReady(const Row& row, DecisionToken token,
        const OriginalDecisionLifetimePin* baseline) const noexcept;
    void ConsumeOriginalDecisionBaseline(Row& row, DecisionToken token,
        OriginalDecisionLifetimePin* baseline) noexcept;
    bool MayMutateFrontierDecision(DecisionToken token, AudioSliceInvalidateReason reason) const noexcept;
    bool ReleaseFuturePin(DecisionToken token) noexcept;
    enum class SlotState : std::uint8_t {
        Free = 0,
        Live = 1,
        Reclaiming = 2,
        Quarantined = 3,
    };

    struct Row {
        std::atomic<SlotState> slotState{SlotState::Free};
        std::atomic<DecisionCommitState> commit{DecisionCommitState::Cancelled};
        std::atomic<std::uint64_t> identity{0};
        std::atomic_flag reservation = ATOMIC_FLAG_INIT;
        std::uint32_t generation = 1;
        LogicCommitTicket ticket{};
        WorldGeneration world = kNoWorldGeneration;
        TransportEpoch transport = kInvalidTransportEpoch;
        std::atomic<std::uint64_t> continuationGeneration{0};
        std::atomic<AudioGroupState> audioState{AudioGroupState::Pending};
        std::atomic<bool> authoredStopInvalidated{false};
        std::uint16_t sliceCount = 0;
        std::atomic<std::uint16_t> consumedSlices{0};
        std::atomic<std::uint64_t> lastAudioSequence{0};
        TransitionAudibleGateRef transitionGate{};
        std::uint32_t participantCount = 0;
        PreparedGroupToken pages{};
        ArmGroupToken armGroup{};
        std::atomic<std::uint32_t> futurePinCount{0};
        // Logic-thread structural lifetime, independent of commit's future-pin gate.
        std::uint64_t originalLifetimeMarker = 0;
        std::uint32_t originalLifetimePinCount = 0;
        DecisionReserveRequest originalLifetimeRequest{};
    };

    Row* Resolve(DecisionToken token) noexcept;
    const Row* Resolve(DecisionToken token) const noexcept;
    DecisionParticipantRef* Participants(std::uint32_t slot) noexcept;
    const DecisionParticipantRef* Participants(
        std::uint32_t slot) const noexcept;

    std::unique_ptr<Row[]> rows_;
    std::unique_ptr<DecisionParticipantRef[]> participants_;
    struct FrontierSidecar;
    void ResetFrontierStorage(std::uint32_t slot) noexcept;
    std::unique_ptr<FrontierSidecar[]> frontiers_;
    std::uint64_t nextOriginalLifetimeMarker_ = 1;
    bool originalLifetimeMarkerExhausted_ = false;
    std::uint32_t capacity_ = 0;
    std::uint32_t maxParticipantsPerDecision_ = 0;
};

static_assert(std::atomic<DecisionCommitState>::is_always_lock_free);
static_assert(std::atomic<AudioGroupState>::is_always_lock_free);

} // namespace harken

#endif // HARKEN_CORE_DECISION_TABLE_H
