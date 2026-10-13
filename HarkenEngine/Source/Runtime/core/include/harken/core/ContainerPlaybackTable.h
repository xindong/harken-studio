#ifndef HARKEN_CORE_CONTAINER_PLAYBACK_TABLE_H
#define HARKEN_CORE_CONTAINER_PLAYBACK_TABLE_H

#include <harken/ObjectId.h>
#include "harken/core/PlayableInstanceTable.h"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>

namespace harken {
namespace detail {
struct ContainerPlaybackTableTestAccess;
struct ContainerPlaybackPreparationTestAccess;
}

enum class ContainerPlaybackPhase : std::uint8_t {
    Free = 0,
    Preparing = 1,
    Active = 2,
    WaitingForChild = 3,
    WaitingForNextFrontier = 4,
    LogicalComplete = 5,
    AudibleRetiring = 6,
    Retired = 7,
    PreparedCommitHeld = 8,
};

struct ContainerPlaybackRow {
    bool live = false;
    PlayableInstanceHandle instance = 0;
    std::uint64_t expectedScheduleEpoch = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint8_t mode = 0;
    ContainerPlaybackPhase phase = ContainerPlaybackPhase::Free;
    std::uint32_t loopLimit = 1;
    std::uint32_t completedSelections = 0;
    PlayableInstanceHandle activeChild = 0;
    std::uint64_t activeChildEpoch = 0;
    PlayableInstanceHandle lastConsumedChild = 0;
    std::uint64_t lastConsumedChildEpoch = 0;
    std::uint64_t lastConsumedOutcomeOrdinal = 0;
};

class ContainerPlaybackTable;

class PreparedContainerPlaybackRow {
public:
    PreparedContainerPlaybackRow() = default;
    PreparedContainerPlaybackRow(const PreparedContainerPlaybackRow&) = delete;
    PreparedContainerPlaybackRow& operator=(
        const PreparedContainerPlaybackRow&) = delete;
    PreparedContainerPlaybackRow(PreparedContainerPlaybackRow&& other) noexcept;
    PreparedContainerPlaybackRow& operator=(
        PreparedContainerPlaybackRow&& other) noexcept;
    ~PreparedContainerPlaybackRow() = default;

    bool Valid() const noexcept;
    PlayableInstanceHandle Instance() const noexcept { return instance_; }
    PlayableInstanceTable* InstanceOwner() const noexcept {
        return instanceOwner_;
    }
    std::uint64_t ExpectedScheduleEpoch() const noexcept {
        return expectedScheduleEpoch_;
    }

private:
    friend class ContainerPlaybackTable;
    friend class PreparedPlayableTreeStep;
    ContainerPlaybackTable* owner_ = nullptr;
    PlayableInstanceTable* instanceOwner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    PlayableInstanceHandle instance_ = 0;
    std::uint64_t expectedScheduleEpoch_ = 0;
    bool preparedOwned_ = false;
    VnObjectId originalContainerId_{};
    std::uint64_t originalFingerprint_ = 0;
    std::uint8_t originalMode_ = 0;
    std::uint32_t originalLoopLimit_ = 1;
    friend struct detail::ContainerPlaybackTableTestAccess;
};

class ContainerPlaybackTable {
public:
    explicit ContainerPlaybackTable(std::uint32_t capacity);
    ContainerPlaybackTable(const ContainerPlaybackTable&) = delete;
    ContainerPlaybackTable& operator=(const ContainerPlaybackTable&) = delete;
    ContainerPlaybackTable(ContainerPlaybackTable&&) = delete;
    ContainerPlaybackTable& operator=(ContainerPlaybackTable&&) = delete;

    class PreparedCommitProof {
    public:
        PreparedCommitProof() = default;
        ~PreparedCommitProof() { Release(); }
        PreparedCommitProof(const PreparedCommitProof&) = delete;
        PreparedCommitProof& operator=(const PreparedCommitProof&) = delete;
        PreparedCommitProof(PreparedCommitProof&& other) noexcept;
        PreparedCommitProof& operator=(PreparedCommitProof&& other) noexcept;
        bool Valid() const noexcept { return row_ != nullptr; }
        bool Associated(const ContainerPlaybackTable* table,
            const PreparedContainerPlaybackRow* original) const noexcept {
            return Valid() && owner_ == table && original_ == original;
        }
        void Release() noexcept;
    private:
        friend class ContainerPlaybackTable;
        ContainerPlaybackTable* owner_ = nullptr;
        ContainerPlaybackRow* row_ = nullptr;
        PreparedContainerPlaybackRow* original_ = nullptr;
    };
    bool PreparedCommitHeld(const PreparedContainerPlaybackRow& original) const noexcept;
    bool ProvePreparedCommit(PreparedContainerPlaybackRow& original, PreparedCommitProof* out) noexcept;
    static void ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept;

    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(rows_.size());
    }
    std::uint32_t LiveCount() const noexcept { return liveCount_; }
    std::uint32_t FreeCount() const noexcept {
        return Capacity() - liveCount_ - preparedCount_;
    }

    bool Prepare(
        PlayableInstanceTable& instanceTable,
        PlayableInstanceHandle instance, std::uint64_t scheduleEpoch,
        VnObjectId containerStableId,
        std::uint64_t programSemanticFingerprint, std::uint8_t mode,
        std::uint32_t loopLimit,
        PreparedContainerPlaybackRow* out) noexcept;
    bool Prepare(
        PlayableInstanceHandle instance, std::uint64_t scheduleEpoch,
        VnObjectId containerStableId,
        std::uint64_t programSemanticFingerprint, std::uint8_t mode,
        std::uint32_t loopLimit,
        PreparedContainerPlaybackRow* out) noexcept;
    bool Commit(PreparedContainerPlaybackRow* prepared) noexcept;
    bool Cancel(PreparedContainerPlaybackRow* prepared) noexcept;

    bool Capture(PlayableInstanceHandle instance,
                 ContainerPlaybackRow* out) const noexcept;
    bool Restore(const ContainerPlaybackRow& snapshot) noexcept;
    bool SetActiveChild(PlayableInstanceHandle instance,
                        PlayableInstanceHandle child,
                        std::uint64_t childEpoch) noexcept;
    bool BeginWaitingForNextFrontier(
        PlayableInstanceHandle instance) noexcept;
    bool MarkLogicalComplete(PlayableInstanceHandle instance) noexcept;
    bool MarkAudibleRetiring(PlayableInstanceHandle instance) noexcept;
    bool MarkRetired(PlayableInstanceHandle instance) noexcept;
    bool Discard(PlayableInstanceHandle instance) noexcept;

    enum class ChildOutcomeStatus : std::uint8_t {
        Accepted = 0,
        Duplicate = 1,
        Mismatch = 2,
        Invalid = 3,
    };
    ChildOutcomeStatus ConsumeChildOutcome(
        PlayableInstanceHandle parent, PlayableInstanceHandle child,
        std::uint64_t childEpoch, std::uint64_t outcomeOrdinal) noexcept;
    ChildOutcomeStatus ConsumeNaturalSelectionOutcome(
        PlayableInstanceHandle parent, PlayableInstanceHandle child,
        std::uint64_t childEpoch, std::uint64_t outcomeOrdinal) noexcept;

private:
    friend class PreparedContainerPlaybackRow;
    friend class PreparedPlayableTreeStep;
    friend struct detail::ContainerPlaybackTableTestAccess;
    bool OwnsPrepared(const PreparedContainerPlaybackRow& prepared) const noexcept;
    friend struct detail::ContainerPlaybackPreparationTestAccess;
    bool PrepareInternal(PlayableInstanceTable* instanceOwner,
        PlayableInstanceHandle instance, std::uint64_t scheduleEpoch,
        VnObjectId containerStableId, std::uint64_t programSemanticFingerprint,
        std::uint8_t mode, std::uint32_t loopLimit, PreparedContainerPlaybackRow* out) noexcept;

    ContainerPlaybackRow* Find(PlayableInstanceHandle instance) noexcept;
    const ContainerPlaybackRow* Find(
        PlayableInstanceHandle instance) const noexcept;
    ContainerPlaybackRow* FindPrepared(
        const PreparedContainerPlaybackRow& prepared) noexcept;

    std::vector<ContainerPlaybackRow> rows_;
    std::uint32_t liveCount_ = 0;
    std::uint32_t preparedCount_ = 0;

    bool RollbackCommitted(PlayableInstanceHandle instance) noexcept;
};

// A tree step owns all private reservations for one start or continuation.
// It is deliberately move-only so a prepared frontier has one clear owner.
class PreparedPlayableTreeStep {
public:
    static constexpr std::size_t kAdoptedReservationCapacity = 512;
    explicit PreparedPlayableTreeStep(std::uint32_t capacity = 0);
    PreparedPlayableTreeStep(const PreparedPlayableTreeStep&) = delete;
    PreparedPlayableTreeStep& operator=(const PreparedPlayableTreeStep&) = delete;
    PreparedPlayableTreeStep(PreparedPlayableTreeStep&& other) noexcept;
    PreparedPlayableTreeStep& operator=(PreparedPlayableTreeStep&& other) noexcept;
    ~PreparedPlayableTreeStep();

    bool PrepareInstance(PlayableInstanceTable& table, PlayableKind kind,
                         PlayableInstanceHandle parent = 0) noexcept;
    bool PrepareContainer(PlayableInstanceTable& instanceTable,
                          ContainerPlaybackTable& table,
                          PlayableInstanceHandle instance,
                          std::uint64_t scheduleEpoch,
                          VnObjectId containerStableId,
                          std::uint64_t programSemanticFingerprint,
                          std::uint8_t mode, std::uint32_t loopLimit) noexcept;
    bool AdoptPreparedInstanceReservation(PreparedPlayableInstance* prepared) noexcept;
    bool AdoptPreparedContainerReservation(PreparedContainerPlaybackRow* prepared) noexcept;
    void SetExternalInstanceContext(PlayableInstanceTable* table,
                                    PreparedPlayableInstance* prepared) noexcept;
    bool ProveContainerCommits(ContainerPlaybackTable* expectedOwner = nullptr,
        const PreparedContainerPlaybackRow* required = nullptr) noexcept;
    bool ProveInstanceCommits(PlayableInstanceTable* expectedOwner = nullptr) noexcept;
    bool ProveParentLinks(PlayableInstanceTable& expectedOwner) noexcept;
    void ReleaseParentLinkProof() noexcept { parentLinkProof_.Release(); }
    bool ParentLinkProofHeld() const noexcept { return parentLinkProof_.Valid(); }
    const PlayableInstanceTable::PreparedParentLinksProof* OriginalPreparedParentLinks(
        const PlayableInstanceTable& expectedOwner) const noexcept {
        return parentLinkOwner_ == &expectedOwner && ValidateParentLinkScope() ? &parentLinkProof_ : nullptr;
    }
    void ReleaseInstanceCommitProofs() noexcept;
    bool InstanceCommitProofsHeld() const noexcept;
    void ReleaseContainerCommitProofs() noexcept;
    bool ContainerCommitProofsHeld() const noexcept;
    bool Validate() const noexcept;
    bool Commit() noexcept;
    // Commit reservations while keeping instance-to-parent links private.
    // PlayableTreeExecutor uses this phase to promote dependent owners before
    // the authored tree becomes externally traversable.
    bool CommitWithoutPublishingLinks() noexcept;
    bool PublishPreparedLinks() noexcept;
    // Reclaim a tree after it has been committed but before its owner
    // publishes the surrounding playback transaction.
    bool RollbackCommitted() noexcept;
    void Cancel() noexcept;
    bool TryCancel() noexcept;
    bool Committed() const noexcept { return committed_; }
    std::uint32_t InstanceReservationCount() const noexcept {
        return static_cast<std::uint32_t>(instances_.size());
    }
    std::uint32_t ContainerReservationCount() const noexcept {
        return static_cast<std::uint32_t>(containers_.size());
    }
    const PreparedContainerPlaybackRow* PreparedContainerToken(
        std::uint32_t index) const noexcept {
        if (index < containers_.size()) return &containers_[index];
        index -= static_cast<std::uint32_t>(containers_.size());
        return index < adoptedContainerCount_ ? adoptedContainers_[index] : nullptr;
    }
    PlayableInstanceHandle PreparedInstanceHandle(
        std::uint32_t index) const noexcept {
        return index < instances_.size() ? instances_[index].Handle() : 0;
    }
    PreparedPlayableInstance* PreparedInstanceToken(
        std::uint32_t index) noexcept {
        return index < instances_.size() ? &instances_[index] : nullptr;
    }
    // Lookup the unique reservation owned or adopted by this executor.
    const PreparedPlayableInstance* FindPreparedInstanceToken(
        PlayableInstanceHandle handle) const noexcept;
    std::uint64_t PreparedInstanceEpoch(
        std::uint32_t index) const noexcept {
        return index < instances_.size() ? instances_[index].ScheduleEpoch() : 0;
    }

private:
    friend class PlayableTreeExecutor;
    bool ResetForReuse(bool published) noexcept;
    bool HasSourceRetirementHolds() const noexcept;
    bool ValidateReservations() const noexcept;
    bool ValidateActivationCommitProofs() const noexcept;
    bool ValidateParentLinkScope() const noexcept;
    bool ProveParentLinksForScope(PlayableInstanceTable& expectedOwner,
        PreparedPlayableInstance* root, PlayableInstanceTable::PreparedCommitProof* rootActivation) noexcept;
    bool PublishPreparedLinksForScope() noexcept;
    void ResetParentLinkScope() noexcept;
    bool CommitProofsRequired() const noexcept {
        return containerCommitProofsRequired_ || instanceCommitProofsRequired_ || parentLinksRequired_;
    }
    void ReleaseCommitProofs() noexcept {
        ReleaseParentLinkProof();
        ReleaseInstanceCommitProofs();
        ReleaseContainerCommitProofs();
    }
    std::vector<PreparedPlayableInstance> instances_;
    std::vector<PreparedContainerPlaybackRow> containers_;
    std::vector<PlayableInstanceTable::PreparedCommitProof> instanceCommitProofs_;
    std::vector<ContainerPlaybackTable::PreparedCommitProof> containerCommitProofs_;
    std::array<PreparedPlayableInstance*, kAdoptedReservationCapacity>
        adoptedInstances_{};
    std::array<PreparedContainerPlaybackRow*, kAdoptedReservationCapacity>
        adoptedContainers_{};
    std::array<ContainerPlaybackTable::PreparedCommitProof, kAdoptedReservationCapacity>
        adoptedContainerCommitProofs_{};
    std::array<PlayableInstanceTable::PreparedCommitProof, kAdoptedReservationCapacity>
        adoptedInstanceCommitProofs_{};
    std::uint32_t adoptedInstanceCount_ = 0;
    std::uint32_t adoptedContainerCount_ = 0;
    std::uint32_t maxReservations_ = 0;
    std::vector<PlayableInstanceTable::ParentLinkRequest> parentLinkRequests_;
    PlayableInstanceTable::PreparedParentLinksProof parentLinkProof_;
    PlayableInstanceTable* parentLinkOwner_ = nullptr;
    PlayableInstanceTable::ParentLinkRequest parentLinkAnchor_{};
    std::size_t parentLinkRequestCount_ = 0;
    bool parentLinksRequired_ = false;
    bool committed_ = false;
    // A native rollback actually began; ordinary published Cancel stays closed.
    bool rollbackRequested_ = false;
    bool containerCommitProofsRequired_ = false;
    bool instanceCommitProofsRequired_ = false;
    bool deferLinkPublication_ = false;
    PlayableInstanceTable* externalTable_ = nullptr;
    PreparedPlayableInstance* externalInstance_ = nullptr;
};

// Runtime-internal lifecycle facade shared by direct starts, action starts,
// and container completion service.  Entry adapters keep their own lookup and
// bookkeeping, while this facade owns the prepare/validate/commit/cancel seam
// for playable instances and container rows.
class PlayableTreeExecutor {
public:
    using OwnerValidateFn = bool (*)(void*) noexcept;
    using OwnerCommitFn = bool (*)(void*, std::uint64_t) noexcept;
    using OwnerCancelFn = bool (*)(void*) noexcept;
    // Engine.h derives the maximum Action owner fan-out from each fixed
    // topology domain and asserts that it fits this allocation-free store.
    // Action starts can expose the flattened VariableControl frontier in
    // addition to the ordinary selection/occurrence/voice owners. Keep this
    // Runtime-internal store large enough for the complete transaction.
    static constexpr std::size_t kOwnerTokenCapacity = 640;

    explicit PlayableTreeExecutor(std::uint32_t capacity = 0)
        : step_(capacity) {}

    PlayableTreeExecutor(const PlayableTreeExecutor&) = delete;
    PlayableTreeExecutor& operator=(const PlayableTreeExecutor&) = delete;
    PlayableTreeExecutor(PlayableTreeExecutor&& other) noexcept;
    PlayableTreeExecutor& operator=(PlayableTreeExecutor&& other) noexcept;

    bool PrepareInstance(PlayableInstanceTable& table, PlayableKind kind,
                         PlayableInstanceHandle parent = 0) noexcept {
        return step_.PrepareInstance(table, kind, parent);
    }

    bool PrepareContainer(
        PlayableInstanceTable& instanceTable, ContainerPlaybackTable& table,
        PlayableInstanceHandle instance,
        std::uint64_t scheduleEpoch, VnObjectId containerStableId,
        std::uint64_t programSemanticFingerprint, std::uint8_t mode,
        std::uint32_t loopLimit) noexcept {
        return step_.PrepareContainer(
            instanceTable, table, instance, scheduleEpoch, containerStableId,
            programSemanticFingerprint, mode, loopLimit);
    }

    bool AdoptPreparedInstanceReservation(
        PreparedPlayableInstance* prepared) noexcept {
        return step_.AdoptPreparedInstanceReservation(prepared);
    }

    bool AdoptPreparedContainerReservation(
        PreparedContainerPlaybackRow* prepared) noexcept {
        return step_.AdoptPreparedContainerReservation(prepared);
    }

    bool ProveContainerCommits(ContainerPlaybackTable* expectedOwner = nullptr,
        const PreparedContainerPlaybackRow* required = nullptr) noexcept {
        if (!step_.ProveContainerCommits(expectedOwner, required)) {
            ReleaseCommitProofs();
            return false;
        }
        return true;
    }
    void ReleaseContainerCommitProofs() noexcept { step_.ReleaseContainerCommitProofs(); }
    bool ContainerCommitProofsHeld() const noexcept { return step_.ContainerCommitProofsHeld(); }
    bool ProveInstanceCommits(PlayableInstanceTable* expectedOwner = nullptr,
        const PreparedPlayableInstance* requiredRoot = nullptr) noexcept;
    bool ProveParentLinks(PlayableInstanceTable& expectedOwner,
        const PreparedPlayableInstance* requiredRoot = nullptr) noexcept;
    void ReleaseParentLinkProof() noexcept { step_.ReleaseParentLinkProof(); }
    bool ParentLinkProofHeld() const noexcept { return step_.ParentLinkProofHeld(); }
    const PlayableInstanceTable::PreparedParentLinksProof* OriginalPreparedParentLinks(
        const PlayableInstanceTable& expectedOwner) const noexcept {
        return step_.OriginalPreparedParentLinks(expectedOwner);
    }
    void ReleaseInstanceCommitProofs() noexcept {
        ReleaseParentLinkProof();
        externalInstanceCommitProof_.Release();
        step_.ReleaseInstanceCommitProofs();
    }
    void ReleaseCommitProofs() noexcept {
        ReleaseParentLinkProof();
        ReleaseInstanceCommitProofs();
        ReleaseContainerCommitProofs();
    }
    bool InstanceCommitProofsHeld() const noexcept {
        return externalInstanceCommitProof_.Valid() || step_.InstanceCommitProofsHeld();
    }

    bool Validate() const noexcept {
        if (!step_.Validate() ||
            (externalInstance_ != nullptr && (!externalInstance_->Valid() ||
                (step_.instanceCommitProofsRequired_ &&
                 !externalInstanceCommitProof_.AssociatedWith(*externalInstance_))))) {
            return false;
        }
        for (std::uint32_t index = 0; index < ownerTokenCount_; ++index) {
            const OwnerToken& token = ownerTokens_[index];
            if (token.validate == nullptr ||
                !token.validate(token.context)) {
                return false;
            }
        }
        return true;
    }
    bool Commit() noexcept { return Commit(0); }
    bool EnablePreparedOwnerRollbackRetention() noexcept {
        if (step_.CommitProofsRequired() || !CanRetainPreparedOwners()) return false;
        retainPreparedOwnerRollback_ = true;
        return true;
    }
    bool Commit(std::uint64_t ticket) noexcept {
        if (step_.instanceCommitProofsRequired_ && externalInstance_ != nullptr) {
            ReleaseCommitProofs();
            return false;
        }
        if (retainPreparedOwnerRollback_ && !CanRetainPreparedOwners()) {
            ReleaseCommitProofs();
            return false;
        }
        if (!Validate() || !step_.CommitWithoutPublishingLinks()) {
            Cancel();
            return false;
        }
        bool committed = true;
        for (std::uint32_t index = 0; index < ownerTokenCount_; ++index) {
            OwnerToken& token = ownerTokens_[index];
            if (token.commit == nullptr || !token.commit(token.context, ticket)) {
                committed = false;
                break;
            }
        }
        if (!committed) {
            // Owner commits are required to be non-failing after Validate.
            // Cancel dependent render owners before releasing their held leaves.
            ReleaseCommitProofs();
            const bool cancelled = CancelOwnerTokens();
            if (retainPreparedOwnerRollback_ && !cancelled) return false;
            (void)step_.RollbackCommitted();
            return false;
        }
        if (!step_.PublishPreparedLinks()) {
            ReleaseCommitProofs();
            const bool cancelled = CancelOwnerTokens();
            if (retainPreparedOwnerRollback_ && !cancelled) return false;
            (void)step_.RollbackCommitted();
            return false;
        }
        RetainPreparedOwnersAfterCommit();
        published_ = true;
        return true;
    }
    void Cancel() noexcept {
        ReleaseCommitProofs();
        if (retainPreparedOwnerRollback_ && ownershipTransferred_) return;
        published_ = false;
        ownershipTransferred_ = false;
        const bool ownersCancelled = CancelOwnerTokens();
        const bool committedOwnersCancelled = CancelCommittedOwnerTokens();
        if (!ownersCancelled || !committedOwnersCancelled) return;
        step_.Cancel();
    }
    bool Committed() const noexcept {
        return step_.Committed() || ExternalInstanceIsLive();
    }

    // Adopt an entry-owned token so its commit/cancel remains part of this
    // executor's lifecycle transaction without transferring scratch storage.
    bool AdoptPreparedInstance(
        PlayableInstanceTable& table,
        PreparedPlayableInstance* prepared) noexcept {
        if (step_.CommitProofsRequired() || instanceTable_ != nullptr || prepared == nullptr ||
            !table.OwnsPrepared(*prepared) ||
            InstanceReservationCount() != 0 ||
            ContainerReservationCount() != 0) {
            return false;
        }
        instanceTable_ = &table;
        externalInstance_ = prepared;
        step_.SetExternalInstanceContext(&table, prepared);
        return true;
    }

    // Register an additional move-only owner reservation with this tree
    // transaction. The callbacks are Runtime-internal and must not publish
    // externally visible state before the executor commits the complete tree.
    bool AdoptPreparedOwnerToken(void* context, OwnerValidateFn validate,
                                 OwnerCommitFn commit,
                                 OwnerCancelFn cancel) noexcept {
        if (step_.CommitProofsRequired() || context == nullptr || validate == nullptr || commit == nullptr ||
            cancel == nullptr || ownerTokenCount_ >= kOwnerTokenCapacity ||
            step_.Committed() ||
            (retainPreparedOwnerRollback_ &&
             (!CanRetainPreparedOwners() ||
              ownerTokenCount_ >= kOwnerTokenCapacity - committedOwnerTokenCount_))) {
            return false;
        }
        ownerTokens_[ownerTokenCount_++] = {context, validate, commit, cancel};
        return true;
    }

    // Keep post-commit entry resources in the shared rollback scope.
    bool AdoptCommittedOwnerToken(void* context,
                                  OwnerCancelFn cancel) noexcept {
        if (context == nullptr || cancel == nullptr ||
            committedOwnerTokenCount_ >= kOwnerTokenCapacity ||
            !step_.Committed() ||
            (retainPreparedOwnerRollback_ && ownershipTransferred_)) {
            return false;
        }
        committedOwnerTokens_[committedOwnerTokenCount_++] =
            {context, nullptr, nullptr, cancel};
        return true;
    }

    std::uint32_t OwnerTokenCount() const noexcept {
        return ownerTokenCount_;
    }

    bool SetPreparedRootMetadata(
        PlayableInstanceTable& table,
        const PlayableRootMetadata& metadata) noexcept {
        return !step_.CommitProofsRequired() && instanceTable_ == &table && externalInstance_ != nullptr &&
               table.SetPreparedRootMetadata(*externalInstance_, metadata);
    }

    bool Commit(PlayableInstanceTable& table) noexcept {
        return Commit(table, 0);
    }

    bool Commit(PlayableInstanceTable& table, std::uint64_t ticket) noexcept {
        if (step_.instanceCommitProofsRequired_ && externalInstance_ != nullptr && instanceTable_ != &table) {
            ReleaseCommitProofs();
            return false;
        }
        if (retainPreparedOwnerRollback_ && !CanRetainPreparedOwners()) {
            ReleaseCommitProofs();
            return false;
        }
        if (instanceTable_ != &table || externalInstance_ == nullptr) {
            return Commit(ticket);
        }
        if (!Validate()) {
            ReleaseCommitProofs();
            return false;
        }
        if (step_.instanceCommitProofsRequired_) {
            PlayableInstanceTable::ConsumePreparedCommit(std::move(externalInstanceCommitProof_));
        } else if (!table.CommitPrepared(externalInstance_, false)) {
            ReleaseCommitProofs();
            return false;
        }
        const bool committed = step_.CommitWithoutPublishingLinks();
        if (!committed) {
            ReleaseCommitProofs();
            if (retainPreparedOwnerRollback_ && !CancelOwnerTokens()) return false;
            const bool rootResolved = !step_.Committed() && ResolveExternalInstance(table);
            if (rootResolved) {
                ClearExternalInstanceContext();
            }
            return false;
        }
        for (std::uint32_t index = 0; index < ownerTokenCount_; ++index) {
            OwnerToken& token = ownerTokens_[index];
            if (token.commit == nullptr || !token.commit(token.context, ticket)) {
                ReleaseCommitProofs();
                const bool cancelled = CancelOwnerTokens();
                if (retainPreparedOwnerRollback_ && !cancelled) return false;
                const bool rolledBack = step_.RollbackCommitted();
                if (rolledBack) {
                    instanceTable_ = nullptr;
                    externalInstance_ = nullptr;
                    step_.SetExternalInstanceContext(nullptr, nullptr);
                }
                return false;
            }
        }
        if (!step_.PublishPreparedLinks()) {
            ReleaseCommitProofs();
            const bool cancelled = CancelOwnerTokens();
            if (retainPreparedOwnerRollback_ && !cancelled) return false;
            const bool rolledBack = step_.RollbackCommitted();
            if (rolledBack) {
                instanceTable_ = nullptr;
                externalInstance_ = nullptr;
                step_.SetExternalInstanceContext(nullptr, nullptr);
            }
            return false;
        }
        RetainPreparedOwnersAfterCommit();
        published_ = true;
        return true;
    }

    bool Cancel(PlayableInstanceTable& table) noexcept {
        ReleaseCommitProofs();
        if (retainPreparedOwnerRollback_ && ownershipTransferred_) return false;
        if (instanceTable_ != nullptr && instanceTable_ != &table) {
            return false;
        }
        published_ = false;
        ownershipTransferred_ = false;
        const bool ownersCancelled = CancelOwnerTokens();
        const bool committedOwnersCancelled = CancelCommittedOwnerTokens();
        if (!ownersCancelled || !committedOwnersCancelled) return false;
        bool cancelled = true;
        if (instanceTable_ == &table && externalInstance_ != nullptr) {
            if (step_.Committed()) {
                cancelled = step_.rollbackRequested_ ? step_.RollbackCommitted()
                    : table.CancelPrepared(externalInstance_);
            } else {
                // Release private child reservations before reclaiming their
                // external root. A live root can be the retained result of an
                // interrupted external-root commit.
                cancelled = step_.TryCancel();
                if (cancelled) cancelled = ResolveExternalInstance(table);
            }
            if (cancelled) {
                ClearExternalInstanceContext();
            }
        }
        const bool privateResolved = step_.TryCancel();
        return cancelled && privateResolved && ownersCancelled && committedOwnersCancelled;
    }

    bool RollbackCommitted(PlayableInstanceTable& table) noexcept {
        ReleaseCommitProofs();
        if (retainPreparedOwnerRollback_ && ownershipTransferred_) return false;
        if (instanceTable_ != nullptr && instanceTable_ != &table) {
            return false;
        }
        published_ = false;
        ownershipTransferred_ = false;
        const bool ownersResolved = CancelOwnerTokens();
        const bool committedOwnersResolved = CancelCommittedOwnerTokens();
        if (!ownersResolved || !committedOwnersResolved) return false;
        bool treeResolved = false;
        if (step_.Committed()) {
            treeResolved = step_.RollbackCommitted();
        } else {
            // The external root is committed before the private tree. Cancel
            // any still-prepared children first, then reclaim that root.
            treeResolved = step_.TryCancel();
            if (treeResolved) treeResolved = ResolveExternalInstance(table);
        }
        // A committed tree can be blocked by an adopted owner that must
        // release a dependent reservation first. Retry the tree rollback once
        // after owner cancellation, keeping the operation bounded.
        if (!treeResolved && ownersResolved && committedOwnersResolved) {
            treeResolved = step_.RollbackCommitted();
        }
        if (!treeResolved && !step_.Committed() &&
            step_.InstanceReservationCount() == 0 && step_.ContainerReservationCount() == 0 &&
            step_.adoptedInstanceCount_ == 0 && step_.adoptedContainerCount_ == 0 &&
            !ExternalInstanceIsLive() && (externalInstance_ == nullptr || externalInstance_->Handle() == 0) &&
            ownerTokenCount_ == 0 &&
            committedOwnerTokenCount_ == 0) {
            // The rollback may have completed while resolving an owner
            // dependency; a subsequent check must report that settled state
            // as success so callers can retire their scratch row.
            treeResolved = true;
        }
        if (treeResolved) {
            ClearExternalInstanceContext();
        }
        return treeResolved && ownersResolved && committedOwnersResolved;
    }

    bool ResetForReuse() noexcept {
        if (InstanceCommitProofsHeld() || ParentLinkProofHeld() || ownerTokenCount_ != 0 ||
            (published_ && !ownershipTransferred_) ||
            (externalInstance_ != nullptr && (externalInstance_->Valid() ||
                (!published_ && externalInstance_->Handle() != 0))) ||
            (!published_ && (committedOwnerTokenCount_ != 0 || ExternalInstanceIsLive())) ||
            !step_.ResetForReuse(published_)) return false;
        ownerTokens_.fill({});
        committedOwnerTokens_.fill({});
        committedOwnerTokenCount_ = 0;
        ClearExternalInstanceContext();
        published_ = false;
        ownershipTransferred_ = false;
        retainPreparedOwnerRollback_ = false;
        return true;
    }

    bool ReleasePublishedOwnership() noexcept {
        if (!published_ || ownerTokenCount_ != 0) return false;
        if (retainPreparedOwnerRollback_) {
            if (ownershipTransferred_) return true;
            for (std::uint32_t index = 0; index < committedOwnerTokenCount_; ++index) {
                committedOwnerTokens_[index] = {};
            }
            committedOwnerTokenCount_ = 0;
        }
        ownershipTransferred_ = true;
        return true;
    }

    std::uint32_t InstanceReservationCount() const noexcept {
        return step_.InstanceReservationCount();
    }
    std::uint32_t ContainerReservationCount() const noexcept {
        return step_.ContainerReservationCount();
    }
    const PreparedContainerPlaybackRow* PreparedContainerToken(
        std::uint32_t index) const noexcept {
        return step_.PreparedContainerToken(index);
    }
    PlayableInstanceHandle PreparedInstanceHandle(
        std::uint32_t index) const noexcept {
        return step_.PreparedInstanceHandle(index);
    }
    PreparedPlayableInstance* PreparedInstanceToken(
        std::uint32_t index) noexcept {
        return step_.PreparedInstanceToken(index);
    }
    const PreparedPlayableInstance* FindPreparedInstanceToken(
        PlayableInstanceHandle handle) const noexcept {
        return step_.FindPreparedInstanceToken(handle);
    }
    std::uint64_t PreparedInstanceEpoch(
        std::uint32_t index) const noexcept {
        return step_.PreparedInstanceEpoch(index);
    }

    static ContainerPlaybackTable::ChildOutcomeStatus ValidateChildOutcome(
        PlayableInstanceTable& instances, ContainerPlaybackTable& table,
        PlayableInstanceHandle parent,
        PlayableInstanceHandle child, std::uint64_t childEpoch) noexcept {
        const auto parentView = instances.Classify(parent);
        const auto childView = instances.Classify(child);
        if (!parentView.live || !childView.live ||
            parentView.kind != PlayableKind::Container ||
            childView.parent != parent) {
            return ContainerPlaybackTable::ChildOutcomeStatus::Invalid;
        }
        ContainerPlaybackRow playback{};
        if (!table.Capture(parent, &playback)) {
            return ContainerPlaybackTable::ChildOutcomeStatus::Invalid;
        }
        if (playback.expectedScheduleEpoch != parentView.scheduleEpoch) {
            return ContainerPlaybackTable::ChildOutcomeStatus::Mismatch;
        }
        bool linked = false;
        auto cursor = parentView.firstChild;
        for (std::uint32_t count = 0;
             cursor != 0 && count <= instances.Capacity(); ++count) {
            if (cursor == child) {
                linked = true;
                break;
            }
            cursor = instances.NextSibling(cursor);
        }
        if (!linked) {
            return ContainerPlaybackTable::ChildOutcomeStatus::Invalid;
        }
        if (childEpoch == 0 || childView.scheduleEpoch != childEpoch ||
            childView.lifecycle < PlayableLifecycle::LogicalComplete ||
            childView.terminalOutcome.kind == PlayableTerminalKind::None) {
            return ContainerPlaybackTable::ChildOutcomeStatus::Mismatch;
        }
        return ContainerPlaybackTable::ChildOutcomeStatus::Accepted;
    }

    static ContainerPlaybackTable::ChildOutcomeStatus ServiceChildOutcome(
        PlayableInstanceTable& instances, ContainerPlaybackTable& table,
        PlayableInstanceHandle parent, PlayableInstanceHandle child,
        std::uint64_t childEpoch, std::uint64_t outcomeOrdinal) noexcept {
        const auto status = ValidateChildOutcome(
            instances, table, parent, child, childEpoch);
        return status == ContainerPlaybackTable::ChildOutcomeStatus::Accepted
            ? table.ConsumeChildOutcome(parent, child, childEpoch, outcomeOrdinal)
            : status;
    }

    static ContainerPlaybackTable::ChildOutcomeStatus ServiceNaturalSelectionOutcome(
        PlayableInstanceTable& instances, ContainerPlaybackTable& table,
        PlayableInstanceHandle parent, PlayableInstanceHandle child,
        std::uint64_t childEpoch, std::uint64_t outcomeOrdinal) noexcept {
        const auto status = ValidateChildOutcome(
            instances, table, parent, child, childEpoch);
        if (status != ContainerPlaybackTable::ChildOutcomeStatus::Accepted) {
            return status;
        }
        const auto outcome = instances.Classify(child).terminalOutcome;
        if (outcome.kind != PlayableTerminalKind::NaturalCompletion ||
            outcomeOrdinal == 0 ||
            outcome.naturalCompletionOrdinal != outcomeOrdinal) {
            return ContainerPlaybackTable::ChildOutcomeStatus::Mismatch;
        }
        return table.ConsumeNaturalSelectionOutcome(
            parent, child, childEpoch, outcomeOrdinal);
    }

    // Link one committed container child through the shared lifecycle service.
    // Entry adapters must not publish active-child state without validating
    // the parent/child relationship and the child's current schedule epoch.
    static bool ServiceActiveChild(
        PlayableInstanceTable& instances, ContainerPlaybackTable& table,
        PlayableInstanceHandle parent,
        PlayableInstanceHandle child) noexcept {
        const auto parentView = instances.Classify(parent);
        const auto childView = instances.Classify(child);
        if (!parentView.live || !childView.live ||
            parentView.kind != PlayableKind::Container ||
            childView.parent != parent || childView.scheduleEpoch == 0) {
            return false;
        }
        return table.SetActiveChild(parent, child, childView.scheduleEpoch);
    }

    // Retire a committed playable subtree from leaves to root.  The entry
    // adapters retain ownership of surrounding leases; this seam only
    // validates and advances the shared instance/container lifecycle.
    static bool RetirePlayableTreeBottomUp(
        PlayableInstanceTable& instances, ContainerPlaybackTable& containers,
        PlayableInstanceHandle handle,
        PlayableTerminalKind terminalKind) noexcept {
        if (handle == 0) return true;
        bool retired = true;
        for (PlayableInstanceHandle child = instances.FirstChild(handle);
             child != 0;) {
            const PlayableInstanceHandle next = instances.NextSibling(child);
            retired = RetirePlayableTreeBottomUp(
                          instances, containers, child, terminalKind) &&
                      retired;
            child = next;
        }
        const PlayableInstanceView view = instances.Classify(handle);
        if (!view.live) return retired;
        if (!retired || instances.FirstChild(handle) != 0 ||
            instances.HasSourceRetirementHold(handle)) return false;
        ContainerPlaybackRow containerRow{};
        if (containers.Capture(handle, &containerRow) && containerRow.live) {
            retired = containers.Discard(handle) && retired;
        }
        if (view.terminalOutcome.kind == PlayableTerminalKind::None)
            retired = instances.SetTerminalOutcome(handle, {terminalKind, 0, 0}) && retired;
        if (view.lifecycle < PlayableLifecycle::LogicalComplete)
            retired = instances.AdvanceLifecycle(handle, PlayableLifecycle::LogicalComplete) && retired;
        if (view.lifecycle < PlayableLifecycle::AudibleRetiring)
            retired = instances.AdvanceLifecycle(handle, PlayableLifecycle::AudibleRetiring) && retired;
        retired = instances.AdvanceLifecycle(handle, PlayableLifecycle::Retired) &&
                  retired;
        return retired && !instances.Classify(handle).live;
    }

private:
    struct OwnerToken {
        void* context = nullptr;
        OwnerValidateFn validate = nullptr;
        OwnerCommitFn commit = nullptr;
        OwnerCancelFn cancel = nullptr;
    };

    bool CanRetainPreparedOwners() const noexcept {
        return !published_ && !ownershipTransferred_ && !step_.Committed() &&
               !ExternalInstanceIsLive() &&
               committedOwnerTokenCount_ <= kOwnerTokenCapacity &&
               ownerTokenCount_ <= kOwnerTokenCapacity - committedOwnerTokenCount_;
    }

    void RetainPreparedOwnersAfterCommit() noexcept {
        if (retainPreparedOwnerRollback_) {
            for (std::uint32_t index = 0; index < ownerTokenCount_; ++index) {
                const auto& token = ownerTokens_[index];
                committedOwnerTokens_[committedOwnerTokenCount_++] =
                    {token.context, nullptr, nullptr, token.cancel};
                ownerTokens_[index] = {};
            }
        }
        ownerTokenCount_ = 0;
    }

    bool CancelOwnerTokens() noexcept {
        bool allCancelled = true;
        const std::uint32_t originalCount = ownerTokenCount_;
        std::uint32_t retainedBegin = originalCount;
        for (std::uint32_t index = originalCount; index-- > 0;) {
            const OwnerToken token = ownerTokens_[index];
            if (token.cancel == nullptr || !token.cancel(token.context)) {
                allCancelled = false;
                ownerTokens_[--retainedBegin] = token;
                continue;
            }
        }
        ownerTokenCount_ = originalCount - retainedBegin;
        for (std::uint32_t index = 0; index < ownerTokenCount_; ++index) {
            ownerTokens_[index] = ownerTokens_[retainedBegin + index];
        }
        for (std::uint32_t index = ownerTokenCount_;
             index < originalCount; ++index) {
            ownerTokens_[index] = {};
        }
        return allCancelled;
    }

    bool CancelCommittedOwnerTokens() noexcept {
        bool allCancelled = true;
        const std::uint32_t originalCount = committedOwnerTokenCount_;
        std::uint32_t retainedBegin = originalCount;
        for (std::uint32_t index = originalCount; index-- > 0;) {
            const OwnerToken token = committedOwnerTokens_[index];
            if (token.cancel == nullptr || !token.cancel(token.context)) {
                allCancelled = false;
                committedOwnerTokens_[--retainedBegin] = token;
            }
        }
        committedOwnerTokenCount_ = originalCount - retainedBegin;
        for (std::uint32_t index = 0; index < committedOwnerTokenCount_; ++index) {
            committedOwnerTokens_[index] =
                committedOwnerTokens_[retainedBegin + index];
        }
        for (std::uint32_t index = committedOwnerTokenCount_;
             index < originalCount; ++index) {
            committedOwnerTokens_[index] = {};
        }
        return allCancelled;
    }

    bool ExternalInstanceIsLive() const noexcept {
        return instanceTable_ != nullptr && externalInstance_ != nullptr &&
               externalInstance_->Handle() != 0 &&
               instanceTable_->Classify(externalInstance_->Handle()).live;
    }

    bool ResolveExternalInstance(PlayableInstanceTable& table) noexcept {
        if (instanceTable_ == nullptr && externalInstance_ == nullptr) {
            return true;
        }
        if (instanceTable_ != &table || externalInstance_ == nullptr) {
            return false;
        }
        if (externalInstance_->Handle() == 0) return true;
        if (table.CancelPrepared(externalInstance_)) return true;
        const PlayableInstanceHandle handle = externalInstance_->Handle();
        // A stale observation is not a successful native cancellation receipt.
        return table.Classify(handle).live && table.RollbackCommitted(handle) &&
               table.CancelPrepared(externalInstance_);
    }

    void ClearExternalInstanceContext() noexcept {
        instanceTable_ = nullptr;
        externalInstance_ = nullptr;
        step_.SetExternalInstanceContext(nullptr, nullptr);
    }

    PreparedPlayableTreeStep step_;
    PlayableInstanceTable::PreparedCommitProof externalInstanceCommitProof_{};
    std::array<OwnerToken, kOwnerTokenCapacity> ownerTokens_{};
    std::uint32_t ownerTokenCount_ = 0;
    std::array<OwnerToken, kOwnerTokenCapacity> committedOwnerTokens_{};
    std::uint32_t committedOwnerTokenCount_ = 0;
    PlayableInstanceTable* instanceTable_ = nullptr;
    PreparedPlayableInstance* externalInstance_ = nullptr;
    bool published_ = false;
    bool ownershipTransferred_ = false;
    bool retainPreparedOwnerRollback_ = false;
};

} // namespace harken

#endif // HARKEN_CORE_CONTAINER_PLAYBACK_TABLE_H
