#ifndef HARKEN_CORE_ACTION_DEFERRED_RECORD_TABLE_H
#define HARKEN_CORE_ACTION_DEFERRED_RECORD_TABLE_H

#include "harken/core/ActionFiringTable.h"
#include "harken/core/DeferredIntentPool.h"
#include "harken/core/StableRowStorage.h"

#include <cstdint>
#include <utility>

namespace harken {

using ActionDeferredOwnerKind = ActionFiringOwnerKind;
using ActionDeferredOwner = ActionFiringOwnerStamp;

struct ActionDeferredRecord {
    ActionStepProgramViewV1 step{};
    ActionFiringHandle firing = 0;
    std::uint32_t stepIndex = 0;
    std::uint32_t executionOrdinal = 0;
    std::uint32_t depth = 0;
    VnObjectId ownerActionStableId{};
    VnObjectId rootActionStableId{};
    std::uint64_t rootActionProgramSemanticFingerprint = 0;
    std::uint64_t callerSeed = 0;
    std::uint64_t emitter = 0;
    std::uint64_t ownerCancellationEpoch = 0;
    ActionDeferredOwner owner{};
    PackPartitionLease targetPackLease{};
    std::uint64_t firstRowFrame = 0;
    std::uint64_t lastRowFrame = 0;
    std::uint64_t intendedBoundaryFrame = 0;
    std::uint64_t deadlineFrame = 0;
    PlayableInstanceHandle rootPlayable = 0;
    std::uint64_t rootScheduleEpoch = 0;
    std::uint64_t authoredExecutionFrame = 0;
    std::uint64_t authoredQuantizeBaseFrame = 0;
    bool waitingForImmediateEntry = false;
};

enum class ActionDeferredRecordState : std::uint8_t {
    Free = 0,
    Prepared = 1,
    Held = 2,
    Claimed = 3,
    CancelRequested = 4,
    Quarantined = 5,
};

struct ActionDeferredRecordToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

enum class ActionDeferredRecordPrepareStatus : std::uint8_t {
    Prepared = 0,
    Capacity = 1,
    Invalid = 2,
};

struct ActionDeferredRecordPrepareResult {
    ActionDeferredRecordPrepareStatus status =
        ActionDeferredRecordPrepareStatus::Invalid;
    ActionDeferredRecordToken token{};
};

enum class ActionDeferredCancelOwnership : std::uint8_t {
    None = 0,
    Canceller = 1,
    Claimant = 2,
};

enum class FixedPlayClaimMode : std::uint8_t {
    BeforeDeadline = 0,
    Expired = 1,
};

enum class FixedPlayClaimStatus : std::uint8_t {
    None = 0,
    Claimed = 1,
    ProtocolViolation = 2,
};

struct ActionDeferredPairState {
    ActionDeferredRecordToken recordToken{};
    ActionDeferredRecordState recordState = ActionDeferredRecordState::Free;
    DeferredIntentToken poolToken{};
    DeferredIntentEntryState poolState = DeferredIntentEntryState::Free;
    PackPartitionLease targetPackLease{};
    DeferredIntentRetryReservation recordReservation{};
    DeferredIntentRetryReservation poolReservation{};
};

class ActionDeferredRecordTable {
public:
    // A producer-only loan on the exact prepared record and native pool entry.
    // Both receivers must outlive the proof. Return releases only the loan;
    // the original preparation owners still return their real resources.
    class PreparedPairCommitProof {
    public:
        PreparedPairCommitProof() = default;
        PreparedPairCommitProof(const PreparedPairCommitProof&) = delete;
        PreparedPairCommitProof& operator=(const PreparedPairCommitProof&) = delete;
        PreparedPairCommitProof(PreparedPairCommitProof&& other) noexcept { *this = std::move(other); }
        PreparedPairCommitProof& operator=(PreparedPairCommitProof&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                pool_ = std::exchange(other.pool_, nullptr);
                record_ = std::exchange(other.record_, ActionDeferredRecordToken{});
                poolToken_ = std::exchange(other.poolToken_, DeferredIntentToken{});
                marker_ = std::exchange(other.marker_, 0);
            }
            return *this;
        }
        ~PreparedPairCommitProof() { Release(); }
        bool Valid() const noexcept;
        bool Covers(const ActionDeferredRecordTable& receiver,
                    ActionDeferredRecordToken record, const DeferredIntentPool& pool,
                    DeferredIntentToken entry) const noexcept {
            return owner_ == &receiver && pool_ == &pool &&
                record_.slot == record.slot && record_.generation == record.generation &&
                poolToken_.slot == entry.slot && poolToken_.generation == entry.generation && Valid();
        }
        void Release() noexcept;
    private:
        friend class ActionDeferredRecordTable;
        ActionDeferredRecordTable* owner_ = nullptr;
        DeferredIntentPool* pool_ = nullptr;
        ActionDeferredRecordToken record_{};
        DeferredIntentToken poolToken_{};
        std::uint64_t marker_ = 0;
    };

    ActionDeferredRecordTable() = default;
    ActionDeferredRecordTable(const ActionDeferredRecordTable&) = delete;
    ActionDeferredRecordTable& operator=(const ActionDeferredRecordTable&) = delete;
    ActionDeferredRecordTable(ActionDeferredRecordTable&&) = delete;
    ActionDeferredRecordTable& operator=(ActionDeferredRecordTable&&) = delete;
    ~ActionDeferredRecordTable();
    bool PreparedPairIssuerExhausted() const noexcept { return nextPreparedPairMarker_ == UINT64_MAX; }
    bool ProvePreparedPair(ActionDeferredRecordToken token, DeferredIntentPool& pool,
                           DeferredIntentToken poolToken, PreparedPairCommitProof* out) noexcept;
    void ConsumePreparedPair(PreparedPairCommitProof&& proof) noexcept;
    bool Reset(std::uint32_t capacity);
    ActionDeferredRecordPrepareResult Prepare(
        const ActionDeferredRecord& record) noexcept;
    bool Abandon(ActionDeferredRecordToken* token) noexcept;
    bool Commit(ActionDeferredRecordToken* token,
                DeferredIntentToken poolToken) noexcept;
    bool CommitPreparedPair(ActionDeferredRecordToken* token,
                            DeferredIntentPool* pool,
                            DeferredIntentToken poolToken) noexcept;
    bool Claim(const DeferredIntentClaim& claim,
               ActionDeferredRecordToken* outToken,
               ActionDeferredRecord* outRecord) noexcept;
    FixedPlayClaimStatus ClaimFixedPlayPair(
        DeferredIntentPool* pool, FixedPlayClaimMode mode,
        std::uint64_t renderedQuantum, std::uint32_t blockFrames,
        std::uint64_t windowEndFrame, std::uint64_t publishCutoffFrame,
        DeferredIntentClaim* outClaim,
        ActionDeferredPairState* outFaultState = nullptr) noexcept;
    bool InspectClaimedPlayPrerequisites(
        DeferredIntentPool* pool, DeferredIntentToken poolToken,
        ActionDeferredRecordToken recordToken, bool* outHeld,
        ActionDeferredPairState* outFaultState = nullptr) const noexcept;
    bool RetryFixedPlayPair(DeferredIntentPool* pool,
                            DeferredIntentToken poolToken,
                            ActionDeferredRecordToken recordToken,
                            std::uint64_t notBeforeQuantum,
                            bool reserveForRetryOwner = false) noexcept;
    bool ClearFixedPlayPairRetryReservation(
        DeferredIntentPool* pool, DeferredIntentToken poolToken,
        ActionDeferredRecordToken recordToken) noexcept;
    bool InspectPair(const DeferredIntentPool& pool,
                     DeferredIntentToken poolToken,
                     ActionDeferredRecordToken recordToken,
                     ActionDeferredPairState* outState) const noexcept;
    bool Capture(ActionDeferredRecordToken token,
                 ActionDeferredRecord* outRecord) const noexcept;
    bool CaptureAt(std::uint32_t slot, ActionDeferredRecordToken* outToken,
                   ActionDeferredRecord* outRecord,
                   ActionDeferredRecordState* outState) const noexcept;
    bool CapturePublicationAt(std::uint32_t slot, ActionDeferredRecordToken* outToken,
                              ActionDeferredRecord* outRecord,
                              ActionDeferredRecordState* outState) const noexcept;
    bool PoolToken(ActionDeferredRecordToken token,
                   DeferredIntentToken* outToken) const noexcept;
    bool RetryClaim(ActionDeferredRecordToken token) noexcept;
    bool RebaseHeldPendingStop(ActionDeferredRecordToken token,
                               DeferredIntentPool* pool,
                               std::uint64_t firstRowFrame,
                               std::uint64_t lastRowFrame) noexcept;
    ActionDeferredCancelOwnership RequestCancel(
        ActionDeferredRecordToken token) noexcept;
    bool FinalizeHeld(ActionDeferredRecordToken token) noexcept;
    bool FinalizeClaim(ActionDeferredRecordToken token) noexcept;

    ActionDeferredRecordState State(
        ActionDeferredRecordToken token) const noexcept;
    std::uint32_t Capacity() const noexcept {
        return rows_.Capacity();
    }
    std::uint32_t PreparedCount() const noexcept { return preparedCount_; }
    std::uint32_t LiveCount() const noexcept { return liveCount_; }
    std::uint32_t HighWater() const noexcept { return highWater_; }
    std::uint32_t QuarantinedCount() const noexcept {
        return quarantinedCount_;
    }
    bool SetFreeGenerationForTests(std::uint32_t slot,
                                   std::uint64_t generation) noexcept;
    void SetCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }
    bool SetTargetFingerprintForTests(ActionDeferredRecordToken token,
                                      std::uint64_t fingerprint) noexcept;

private:
    struct Row {
        ActionDeferredRecordState state = ActionDeferredRecordState::Free;
        std::uint64_t generation = 1;
        ActionDeferredRecord record{};
        DeferredIntentToken poolToken{};
        bool heldCancelOwned = false;
        std::uint64_t preparedPairMarker = 0;
        DeferredIntentRetryReservation retryReservation{};
    };

    bool MatchesPreparedPair(const PreparedPairCommitProof& proof) const noexcept;
    void ReleasePreparedPair(PreparedPairCommitProof& proof) noexcept;
    Row* Resolve(ActionDeferredRecordToken token) noexcept;
    const Row* Resolve(ActionDeferredRecordToken token) const noexcept;
    void CommitValidated(Row& row, ActionDeferredRecordToken* token,
                         DeferredIntentToken poolToken) noexcept;
    bool Finalize(Row& row) noexcept;
    bool InspectPlayPrerequisites(DeferredIntentPool& pool,
        const DeferredIntent& candidate, std::uint32_t candidateSlot,
        bool* outHeld, ActionDeferredPairState* outFaultState) const noexcept;

    std::uint64_t nextPreparedPairMarker_ = 0;
    std::uint32_t preparedPairLoans_ = 0;
    StableRowStorage<Row> rows_;
    std::uint32_t preparedCount_ = 0;
    std::uint32_t liveCount_ = 0;
    std::uint32_t highWater_ = 0;
    std::uint32_t quarantinedCount_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_ACTION_DEFERRED_RECORD_TABLE_H
