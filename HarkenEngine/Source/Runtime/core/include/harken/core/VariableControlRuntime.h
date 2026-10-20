#ifndef HARKEN_CORE_VARIABLE_CONTROL_RUNTIME_H
#define HARKEN_CORE_VARIABLE_CONTROL_RUNTIME_H

#include <harken/ObjectId.h>
#include "harken/core/BankVersionToken.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/DecisionToken.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/WeightSegment.h"
#include "harken/core/FrontierActivation.h"
#include "harken/core/StableRowStorage.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>

namespace harken {

class ScheduledTransactionPool;
class DecisionTable;
struct Engine;

namespace detail {
struct VariableControlInstanceTableTestAccess;
}

struct VariableControlInstanceToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

enum class VariableControlSide : std::uint8_t {
    Discrete = 0,
    Continuous = 1,
};

enum class VariableControlSideState : std::uint8_t {
    Empty = 0,
    Pending = 1,
    Committed = 2,
    Terminal = 3,
    Transitioning = 4,
    Quarantined = 5,
};

enum class VariableControlInstanceStatus : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    Stale = 2,
    Capacity = 3,
};

bool ResolveWeightSegment(const WeightSegmentV1& segment,
                          std::uint64_t frame,
                          float* outWeight) noexcept;

WeightSegmentStatus RetargetWeightSegment(
    const WeightSegmentV1* current,
    std::uint64_t decisionFrame,
    std::uint32_t envelopeFrames,
    float targetWeight,
    DecisionToken decision,
    BankVersionToken bank,
    WeightSegmentV1* outSegment) noexcept;

bool ResolveVariableControlCadenceFrame(
    std::uint64_t anchorFrame,
    std::uint32_t evaluationPeriodFrames,
    std::uint64_t observedFrame,
    std::uint64_t* outFrame) noexcept;

struct VariableControlInstanceCreateRequest {
    WorldGeneration world = 0;
    PlayableInstanceHandle rootInstance = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
};

class VariableControlInstanceTable;

class PreparedVariableControlInstance {
public:
    PreparedVariableControlInstance() = default;
    PreparedVariableControlInstance(
        const PreparedVariableControlInstance&) = delete;
    PreparedVariableControlInstance& operator=(
        const PreparedVariableControlInstance&) = delete;
    PreparedVariableControlInstance(
        PreparedVariableControlInstance&& other) noexcept;
    PreparedVariableControlInstance& operator=(
        PreparedVariableControlInstance&& other) noexcept;

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    VariableControlInstanceToken Token() const noexcept {
        return Valid() ? token_ : VariableControlInstanceToken{};
    }
    const VariableControlInstanceCreateRequest& OriginalRequest() const noexcept {
        return originalRequest_;
    }

private:
    friend class VariableControlInstanceTable;
    VariableControlInstanceTable* owner_ = nullptr;
    VariableControlInstanceToken token_{};
    bool prepared_ = false;
    struct SideReceipt {
        DecisionToken decision{};
        BankVersionToken bank{};
        std::uint64_t generation = 0;
        bool issued = false;
        bool silent = false;
    };
    VariableControlInstanceCreateRequest originalRequest_{};
    std::array<SideReceipt, 2> originalSides_{};
};

struct VariableControlSideSnapshot {
    VariableControlSideState state = VariableControlSideState::Empty;
    DecisionToken decision{};
    BankVersionToken newestObserved{};
    BankVersionToken newestPending{};
    BankVersionToken lastApplied{};
};

struct VariableControlInstanceSnapshot {
    WorldGeneration world = 0;
    PlayableInstanceHandle rootInstance = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    VariableControlSideSnapshot discrete{};
    VariableControlSideSnapshot continuous{};
};

struct VariableControlActivationProof {
    VariableControlInstanceToken instance{};
    VariableControlSide side = VariableControlSide::Discrete;
    DecisionToken decision{};
    BankVersionToken bank{};
    std::uint64_t sideGeneration = 0;
};

class VariableControlInstanceTable {
    struct SideRow;
    struct Entry;
public:
    class OriginalInitialSideReceipt {
    public:
        OriginalInitialSideReceipt() noexcept = default;
        ~OriginalInitialSideReceipt();
        OriginalInitialSideReceipt(const OriginalInitialSideReceipt&) = delete;
        OriginalInitialSideReceipt& operator=(const OriginalInitialSideReceipt&) = delete;
        OriginalInitialSideReceipt(OriginalInitialSideReceipt&& other) noexcept;
        OriginalInitialSideReceipt& operator=(OriginalInitialSideReceipt&& other) noexcept;
        bool Valid() const noexcept { return owner_ != nullptr; }
        void Release() noexcept;
    private:
        friend class VariableControlInstanceTable;
        VariableControlInstanceTable* owner_ = nullptr;
        Entry* entry_ = nullptr;
        VariableControlInstanceToken token_{};
        VariableControlInstanceCreateRequest create_{};
        VariableControlSide side_ = VariableControlSide::Discrete;
        VariableControlSideSnapshot snapshot_{};
        std::uint64_t generation_ = 0;
        bool replacing_ = false;
    };
    class InitialSideReplacement {
    public:
        InitialSideReplacement() noexcept = default;
        ~InitialSideReplacement();
        InitialSideReplacement(const InitialSideReplacement&) = delete;
        InitialSideReplacement& operator=(const InitialSideReplacement&) = delete;
        bool Valid() const noexcept { return original_ != nullptr; }
    private:
        friend class VariableControlInstanceTable;
        OriginalInitialSideReceipt* original_ = nullptr;
        DecisionToken replacement_{};
        std::uint64_t generation_ = 0;
    };

    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        ~PreparedCommitProof() { Release(); }
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept;
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        bool Valid() const noexcept { return entry_ != nullptr; }
        bool Associated(const VariableControlInstanceTable* table,
            const PreparedVariableControlInstance* original) const noexcept {
            return Valid() && owner_ == table && original_ == original;
        }
        VariableControlInstanceToken Token() const noexcept { return token_; }
        void Release() noexcept;
    private:
        friend class VariableControlInstanceTable;
        VariableControlInstanceTable* owner_ = nullptr;
        Entry* entry_ = nullptr;
        PreparedVariableControlInstance* original_ = nullptr;
        VariableControlInstanceToken token_{};
    };
    bool PreparedCommitHeld(VariableControlInstanceToken token) const noexcept;
    bool ProvePreparedCommit(PreparedVariableControlInstance& original,
        PreparedCommitProof* out) noexcept;
    static void ConsumePreparedAcquire(PreparedCommitProof&& proof) noexcept;
    class FrontierSideProof {
    public:
        FrontierSideProof() = default;
        ~FrontierSideProof();
        FrontierSideProof(const FrontierSideProof&) = delete;
        FrontierSideProof& operator=(const FrontierSideProof&) = delete;
        bool Valid() const noexcept { return side_ != nullptr; }
        void Release() noexcept;
    private:
        friend class VariableControlInstanceTable;
        SideRow* side_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
        std::uint64_t bank_ = 0;
        VariableControlSideState priorState_ = VariableControlSideState::Pending;
        bool applied_ = false;
    };

    bool BindFrontierSide(VariableControlInstanceToken instance,
        const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) noexcept;
    bool ProveFrontierSide(const FrontierGroupBinding& binding,
        AudioSliceLease& lease, FrontierSideProof* out) noexcept;
    void ApplyFrontierSide(FrontierSideProof& proof) noexcept;
    explicit VariableControlInstanceTable(std::uint32_t capacity);
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t ConstructedCount() const noexcept { return entries_.ConstructedCount(); }
    std::size_t StorageBytes() const noexcept { return entries_.RequestedBytes(); }

    std::uint32_t LiveCount() const noexcept {
        return liveCount_.load(std::memory_order_acquire);
    }
    std::uint32_t HighWater() const noexcept {
        return highWater_.load(std::memory_order_acquire);
    }

    VariableControlInstanceStatus Acquire(
        const VariableControlInstanceCreateRequest& request,
        VariableControlInstanceToken* out) noexcept;
    VariableControlInstanceStatus PrepareAcquire(
        const VariableControlInstanceCreateRequest& request,
        PreparedVariableControlInstance* out) noexcept;
    bool CommitPreparedAcquire(
        PreparedVariableControlInstance* prepared,
        VariableControlInstanceToken* out) noexcept;
    bool CancelPreparedAcquire(
        PreparedVariableControlInstance* prepared) noexcept;

    VariableControlInstanceStatus PrepareSide(
        VariableControlInstanceToken instance,
        VariableControlSide side,
        DecisionToken decision,
        BankVersionToken bank) noexcept;
    VariableControlInstanceStatus PrepareSide(
        PreparedVariableControlInstance& original,
        VariableControlSide side, DecisionToken decision,
        BankVersionToken bank) noexcept;

    VariableControlInstanceStatus CommitSilentSide(
        VariableControlInstanceToken instance,
        VariableControlSide side,
        BankVersionToken bank) noexcept;
    VariableControlInstanceStatus CommitSilentSide(
        PreparedVariableControlInstance& original, VariableControlSide side,
        BankVersionToken bank) noexcept;

    VariableControlInstanceStatus AdvanceCommittedSideBank(
        VariableControlInstanceToken instance,
        VariableControlSide side,
        BankVersionToken expectedApplied,
        BankVersionToken newerBank) noexcept;

    VariableControlInstanceStatus PrepareNewerSide(
        VariableControlInstanceToken instance,
        VariableControlSide side,
        DecisionToken decision,
        BankVersionToken bank,
        DecisionToken* outSupersededDecision) noexcept;

    bool CancelPreparedSideAndRestoreCommitted(
        VariableControlInstanceToken instance,
        VariableControlSide side,
        DecisionToken preparedDecision,
        DecisionToken priorDecision) noexcept;

    bool ActivateSide(VariableControlInstanceToken instance,
                      VariableControlSide side,
                      DecisionToken decision,
                      BankVersionToken bank) noexcept;

    bool CanActivateDecision(VariableControlInstanceToken instance,
                             DecisionToken decision) const noexcept;

    bool ProveActivation(VariableControlInstanceToken instance,
                         DecisionToken decision,
                         VariableControlActivationProof* out) const noexcept;

    bool ActivateProven(
        const VariableControlActivationProof& proof) noexcept;

    void ApplyClaimedActivation(
        const VariableControlActivationProof& proof) noexcept;

    bool ActivateDecision(VariableControlInstanceToken instance,
                          DecisionToken decision) noexcept;

    bool CancelSide(VariableControlInstanceToken instance,
                    VariableControlSide side,
                    DecisionToken decision) noexcept;

    bool Release(VariableControlInstanceToken instance) noexcept;

    bool Capture(VariableControlInstanceToken instance,
                 VariableControlInstanceSnapshot* out) const noexcept;

private:
    friend struct Engine;
    friend struct detail::VariableControlInstanceTableTestAccess;

    // Engine must retain its genuine private Entry placement while replacing.
    bool IssueOriginalInitialSideReceipt(const PreparedVariableControlInstance& original,
        VariableControlSide side, OriginalInitialSideReceipt* out) noexcept;
    bool MatchesOriginalInitialSideReceipt(const OriginalInitialSideReceipt& original,
        VariableControlInstanceToken instance, VariableControlSide side,
        DecisionToken decision, BankVersionToken bank) const noexcept;
    VariableControlInstanceStatus BeginInitialSideReplacement(OriginalInitialSideReceipt& original,
        DecisionToken replacement, InitialSideReplacement* out) noexcept;
    bool FinishInitialSideReplacement(InitialSideReplacement& replacement, bool commit) noexcept;
    bool MatchesInitialSideEntry(const OriginalInitialSideReceipt& original) const noexcept;
    bool UnbindPrivateInitialFrontierSide(const OriginalInitialSideReceipt& original,
        const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool,
        const DecisionTable& decisions) noexcept;
    bool MatchesPrivateInitialFrontierSide(const OriginalInitialSideReceipt& original,
        const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool,
        const DecisionTable& decisions) const noexcept;

    enum class SlotState : std::uint8_t {
        Free = 0,
        Initializing = 1,
        Live = 2,
        Reclaiming = 3,
        Quarantined = 4,
        PreparedCommitHeld = 5,
    };

    struct SideRow {
        std::atomic<VariableControlSideState> state{
            VariableControlSideState::Empty};
        std::atomic<std::uint64_t> decision{0};
        std::atomic<std::uint64_t> newestObserved{0};
        std::atomic<std::uint64_t> newestPending{0};
        std::atomic<std::uint64_t> lastApplied{0};
        std::atomic<std::uint64_t> generation{1};
        std::atomic<const InitialSideReplacement*> initialReplacement{nullptr};
        const ScheduledTransactionPool* frontierPool = nullptr;
        FrontierOwnerIdentity frontierOwner{};
    };

    struct Entry {
        std::atomic<SlotState> state{SlotState::Free};
        std::atomic<std::uint32_t> generation{1};
        std::atomic<WorldGeneration> world{0};
        std::atomic<PlayableInstanceHandle> rootInstance{0};
        std::atomic<std::uint64_t> containerIdHigh{0};
        std::atomic<std::uint64_t> containerIdLow{0};
        std::atomic<std::uint64_t> programSemanticFingerprint{0};
        SideRow discrete{};
        SideRow continuous{};
    };

    Entry* Resolve(VariableControlInstanceToken token) noexcept;
    const Entry* Resolve(VariableControlInstanceToken token) const noexcept;
    Entry* ResolvePreparing(VariableControlInstanceToken token) noexcept;
    VariableControlInstanceStatus AcquireInternal(
        const VariableControlInstanceCreateRequest& request,
        VariableControlInstanceToken* out, bool prepared) noexcept;
    void NoteLiveAcquire() noexcept;
    VariableControlInstanceStatus PrepareSideInternal(
        VariableControlInstanceToken instance, VariableControlSide side,
        DecisionToken decision, BankVersionToken bank,
        PreparedVariableControlInstance* original) noexcept;
    VariableControlInstanceStatus CommitSilentSideInternal(
        VariableControlInstanceToken instance, VariableControlSide side,
        BankVersionToken bank, PreparedVariableControlInstance* original) noexcept;
    static void ResetEntry(Entry& entry) noexcept;
    static bool MayMutateSide(SideRow& side) noexcept;

    StableRowStorage<Entry> entries_;
    std::uint32_t capacity_ = 0;
    std::atomic<std::uint32_t> liveCount_{0};
    std::atomic<std::uint32_t> highWater_{0};
};

static_assert(std::atomic<VariableControlSideState>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace harken

#endif // HARKEN_CORE_VARIABLE_CONTROL_RUNTIME_H
