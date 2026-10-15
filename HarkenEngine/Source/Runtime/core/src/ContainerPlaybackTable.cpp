#include "harken/core/ContainerPlaybackTable.h"
#include "harken/core/CurrentWorld.h"

#include <utility>
#include <cassert>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace harken {
namespace {
std::size_t ParentLinkReservationCapacity(std::uint32_t capacity) {
    const std::size_t internal = capacity == 0 ? 1u : capacity;
    const auto adopted = std::min(internal, PreparedPlayableTreeStep::kAdoptedReservationCapacity);
    if (internal > UINT32_MAX - adopted ||
        internal + adopted > (std::numeric_limits<std::size_t>::max() - 1u) / 3u)
        throw std::length_error("tree parent link capacity overflow");
    return internal + adopted;
}
}

PreparedContainerPlaybackRow::PreparedContainerPlaybackRow(
    PreparedContainerPlaybackRow&& other) noexcept {
    *this = std::move(other);
}

PreparedContainerPlaybackRow& PreparedContainerPlaybackRow::operator=(
    PreparedContainerPlaybackRow&& other) noexcept {
    if (this != &other &&
        (owner_ == nullptr || !owner_->PreparedCommitHeld(*this)) &&
        (other.owner_ == nullptr || !other.owner_->PreparedCommitHeld(other))) {
        owner_ = other.owner_;
        instanceOwner_ = other.instanceOwner_;
        slot_ = other.slot_;
        instance_ = other.instance_;
        expectedScheduleEpoch_ = other.expectedScheduleEpoch_;
        preparedOwned_ = other.preparedOwned_;
        originalContainerId_ = other.originalContainerId_;
        originalFingerprint_ = other.originalFingerprint_;
        originalMode_ = other.originalMode_;
        originalLoopLimit_ = other.originalLoopLimit_;
        other.owner_ = nullptr;
        other.instanceOwner_ = nullptr;
        other.slot_ = UINT32_MAX;
        other.instance_ = 0;
        other.expectedScheduleEpoch_ = 0;
        other.preparedOwned_ = false;
        other.originalContainerId_ = {};
        other.originalFingerprint_ = 0;
        other.originalMode_ = 0;
        other.originalLoopLimit_ = 1;
    }
    return *this;
}

bool PreparedContainerPlaybackRow::Valid() const noexcept {
    return owner_ != nullptr && owner_->OwnsPrepared(*this);
}

ContainerPlaybackTable::PreparedCommitProof::PreparedCommitProof(PreparedCommitProof&& other) noexcept {
    *this = std::move(other);
}

ContainerPlaybackTable::PreparedCommitProof&
ContainerPlaybackTable::PreparedCommitProof::operator=(PreparedCommitProof&& other) noexcept {
    if (this == &other) return *this;
    Release();
    owner_ = other.owner_;
    row_ = other.row_;
    original_ = other.original_;
    other.owner_ = nullptr;
    other.row_ = nullptr;
    other.original_ = nullptr;
    return *this;
}

void ContainerPlaybackTable::PreparedCommitProof::Release() noexcept {
    if (row_ != nullptr) {
        assert(!row_->live && row_->phase == ContainerPlaybackPhase::PreparedCommitHeld);
        row_->phase = ContainerPlaybackPhase::Preparing;
    }
    owner_ = nullptr;
    row_ = nullptr;
    original_ = nullptr;
}

bool ContainerPlaybackTable::OwnsPrepared(const PreparedContainerPlaybackRow& prepared) const noexcept {
    if (prepared.owner_ != this || !prepared.preparedOwned_ || prepared.slot_ >= rows_.size()) return false;
    const auto& row = rows_[prepared.slot_];
    return !row.live && (row.phase == ContainerPlaybackPhase::Preparing ||
        row.phase == ContainerPlaybackPhase::PreparedCommitHeld) && row.instance == prepared.instance_ &&
        row.expectedScheduleEpoch == prepared.expectedScheduleEpoch_;
}

bool ContainerPlaybackTable::PreparedCommitHeld(const PreparedContainerPlaybackRow& original) const noexcept {
    return OwnsPrepared(original) && rows_[original.slot_].phase == ContainerPlaybackPhase::PreparedCommitHeld;
}

bool ContainerPlaybackTable::ProvePreparedCommit(PreparedContainerPlaybackRow& original,
    PreparedCommitProof* out) noexcept {
    if (out == nullptr || out->Valid()) return false;
    auto* row = FindPrepared(original);
    if (row == nullptr || row->containerStableId != original.originalContainerId_ ||
        row->programSemanticFingerprint != original.originalFingerprint_ ||
        row->mode != original.originalMode_ || row->loopLimit != original.originalLoopLimit_ ||
        row->completedSelections != 0 || row->activeChild != 0 || row->activeChildEpoch != 0 ||
        row->lastConsumedChild != 0 || row->lastConsumedChildEpoch != 0 || row->lastConsumedOutcomeOrdinal != 0)
        return false;
    row->phase = ContainerPlaybackPhase::PreparedCommitHeld;
    out->owner_ = this;
    out->row_ = row;
    out->original_ = &original;
    return true;
}

void ContainerPlaybackTable::ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept {
    assert(proof.Valid() && proof.owner_ != nullptr && proof.original_ != nullptr);
    if (!proof.Valid()) return;
    assert(!proof.row_->live && proof.row_->phase == ContainerPlaybackPhase::PreparedCommitHeld);
    proof.row_->live = true;
    proof.row_->phase = ContainerPlaybackPhase::Active;
    --proof.owner_->preparedCount_;
    ++proof.owner_->liveCount_;
    // Retain the consumed identity for the existing tree rollback owner.
    proof.original_->preparedOwned_ = false;
    proof.owner_ = nullptr;
    proof.row_ = nullptr;
    proof.original_ = nullptr;
}

ContainerPlaybackTable::ContainerPlaybackTable(std::uint32_t capacity)
    : rows_(capacity) {}

ContainerPlaybackRow* ContainerPlaybackTable::Find(
    PlayableInstanceHandle instance) noexcept {
    if (instance == 0) {
        return nullptr;
    }
    for (auto& row : rows_) {
        if (row.live && row.instance == instance) {
            return &row;
        }
    }
    return nullptr;
}

const ContainerPlaybackRow* ContainerPlaybackTable::Find(
    PlayableInstanceHandle instance) const noexcept {
    if (instance == 0) {
        return nullptr;
    }
    for (const auto& row : rows_) {
        if (row.live && row.instance == instance) {
            return &row;
        }
    }
    return nullptr;
}

ContainerPlaybackRow* ContainerPlaybackTable::FindPrepared(
    const PreparedContainerPlaybackRow& prepared) noexcept {
    if (!OwnsPrepared(prepared)) {
        return nullptr;
    }
    ContainerPlaybackRow& row = rows_[prepared.slot_];
    return row.phase == ContainerPlaybackPhase::Preparing
               ? &row
               : nullptr;
}

bool ContainerPlaybackTable::Prepare(
    PlayableInstanceTable& instanceTable,
    PlayableInstanceHandle instance, std::uint64_t scheduleEpoch,
    VnObjectId containerStableId, std::uint64_t programSemanticFingerprint,
    std::uint8_t mode, std::uint32_t loopLimit,
    PreparedContainerPlaybackRow* out) noexcept {
    return PrepareInternal(&instanceTable, instance, scheduleEpoch, containerStableId,
        programSemanticFingerprint, mode, loopLimit, out);
}

bool ContainerPlaybackTable::Prepare(
    PlayableInstanceHandle instance, std::uint64_t scheduleEpoch,
    VnObjectId containerStableId, std::uint64_t programSemanticFingerprint,
    std::uint8_t mode, std::uint32_t loopLimit,
    PreparedContainerPlaybackRow* out) noexcept {
    return PrepareInternal(nullptr, instance, scheduleEpoch, containerStableId,
        programSemanticFingerprint, mode, loopLimit, out);
}

bool ContainerPlaybackTable::PrepareInternal(PlayableInstanceTable* instanceOwner,
    PlayableInstanceHandle instance, std::uint64_t scheduleEpoch,
    VnObjectId containerStableId, std::uint64_t programSemanticFingerprint,
    std::uint8_t mode, std::uint32_t loopLimit, PreparedContainerPlaybackRow* out) noexcept {
    if (out == nullptr || out->Valid() || instance == 0 ||
        scheduleEpoch == 0 || containerStableId == VnObjectId{} ||
        Find(instance) != nullptr) {
        return false;
    }
    for (const auto& row : rows_) {
        if (row.instance == instance && (row.live || row.phase == ContainerPlaybackPhase::Preparing ||
            row.phase == ContainerPlaybackPhase::PreparedCommitHeld)) return false;
    }
    for (std::uint32_t index = 0; index < rows_.size(); ++index) {
        ContainerPlaybackRow& row = rows_[index];
        if (row.live || row.phase == ContainerPlaybackPhase::Preparing ||
            row.phase == ContainerPlaybackPhase::PreparedCommitHeld) {
            continue;
        }
        row = {};
        row.instance = instance;
        row.expectedScheduleEpoch = scheduleEpoch;
        row.containerStableId = containerStableId;
        row.programSemanticFingerprint = programSemanticFingerprint;
        row.mode = mode;
        row.phase = ContainerPlaybackPhase::Preparing;
        row.loopLimit = loopLimit;
        ++preparedCount_;
        out->owner_ = this;
        out->slot_ = index;
        out->instance_ = instance;
        out->expectedScheduleEpoch_ = scheduleEpoch;
        out->instanceOwner_ = instanceOwner;
        out->preparedOwned_ = true;
        out->originalContainerId_ = containerStableId;
        out->originalFingerprint_ = programSemanticFingerprint;
        out->originalMode_ = mode;
        out->originalLoopLimit_ = loopLimit;
        return true;
    }
    return false;
}

bool ContainerPlaybackTable::Commit(
    PreparedContainerPlaybackRow* prepared) noexcept {
    if (prepared == nullptr) {
        return false;
    }
    ContainerPlaybackRow* row = FindPrepared(*prepared);
    if (row == nullptr) {
        return false;
    }
    row->live = true;
    row->phase = ContainerPlaybackPhase::Active;
    --preparedCount_;
    ++liveCount_;
    prepared->preparedOwned_ = false;
    return true;
}

bool ContainerPlaybackTable::Cancel(
    PreparedContainerPlaybackRow* prepared) noexcept {
    if (prepared == nullptr) {
        return false;
    }
    ContainerPlaybackRow* row = FindPrepared(*prepared);
    if (row == nullptr) {
        return false;
    }
    *row = {};
    --preparedCount_;
    *prepared = {};
    return true;
}

bool ContainerPlaybackTable::RollbackCommitted(
    PlayableInstanceHandle instance) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr) {
        return false;
    }
    *row = {};
    --liveCount_;
    return true;
}

bool ContainerPlaybackTable::Capture(PlayableInstanceHandle instance,
                                     ContainerPlaybackRow* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    const ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr) {
        return false;
    }
    *out = *row;
    return true;
}

bool ContainerPlaybackTable::Restore(
    const ContainerPlaybackRow& snapshot) noexcept {
    ContainerPlaybackRow* row = Find(snapshot.instance);
    if (row == nullptr || !snapshot.live) {
        return false;
    }
    *row = snapshot;
    return true;
}

bool ContainerPlaybackTable::SetActiveChild(
    PlayableInstanceHandle instance, PlayableInstanceHandle child,
    std::uint64_t childEpoch) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr || child == 0 || childEpoch == 0 ||
        (row->phase != ContainerPlaybackPhase::Active &&
         row->phase != ContainerPlaybackPhase::WaitingForNextFrontier)) {
        return false;
    }
    row->activeChild = child;
    row->activeChildEpoch = childEpoch;
    row->phase = ContainerPlaybackPhase::WaitingForChild;
    return true;
}

bool ContainerPlaybackTable::BeginWaitingForNextFrontier(
    PlayableInstanceHandle instance) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr || row->phase != ContainerPlaybackPhase::WaitingForChild) {
        return false;
    }
    row->phase = ContainerPlaybackPhase::WaitingForNextFrontier;
    return true;
}

bool ContainerPlaybackTable::MarkLogicalComplete(
    PlayableInstanceHandle instance) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr || row->phase == ContainerPlaybackPhase::LogicalComplete ||
        row->phase == ContainerPlaybackPhase::AudibleRetiring ||
        row->phase == ContainerPlaybackPhase::Retired) {
        return false;
    }
    row->phase = ContainerPlaybackPhase::LogicalComplete;
    return true;
}

bool ContainerPlaybackTable::MarkAudibleRetiring(
    PlayableInstanceHandle instance) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr || row->phase != ContainerPlaybackPhase::LogicalComplete) {
        return false;
    }
    row->phase = ContainerPlaybackPhase::AudibleRetiring;
    return true;
}

bool ContainerPlaybackTable::MarkRetired(
    PlayableInstanceHandle instance) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr || row->phase != ContainerPlaybackPhase::AudibleRetiring) {
        return false;
    }
    *row = {};
    --liveCount_;
    return true;
}

bool ContainerPlaybackTable::Discard(PlayableInstanceHandle instance) noexcept {
    ContainerPlaybackRow* row = Find(instance);
    if (row == nullptr) {
        return false;
    }
    *row = {};
    --liveCount_;
    return true;
}

ContainerPlaybackTable::ChildOutcomeStatus
ContainerPlaybackTable::ConsumeChildOutcome(
    PlayableInstanceHandle parent, PlayableInstanceHandle child,
    std::uint64_t childEpoch, std::uint64_t outcomeOrdinal) noexcept {
    ContainerPlaybackRow* row = Find(parent);
    if (row == nullptr || child == 0 || childEpoch == 0) {
        return ChildOutcomeStatus::Invalid;
    }
    if (row->lastConsumedChild == child &&
        row->lastConsumedChildEpoch == childEpoch &&
        row->lastConsumedOutcomeOrdinal == outcomeOrdinal) {
        return ChildOutcomeStatus::Duplicate;
    }
    if (row->activeChild != child || row->activeChildEpoch != childEpoch) {
        return ChildOutcomeStatus::Mismatch;
    }
    row->lastConsumedChild = child;
    row->lastConsumedChildEpoch = childEpoch;
    row->lastConsumedOutcomeOrdinal = outcomeOrdinal;
    row->activeChild = 0;
    row->activeChildEpoch = 0;
    row->phase = ContainerPlaybackPhase::WaitingForNextFrontier;
    return ChildOutcomeStatus::Accepted;
}

ContainerPlaybackTable::ChildOutcomeStatus
ContainerPlaybackTable::ConsumeNaturalSelectionOutcome(
    PlayableInstanceHandle parent, PlayableInstanceHandle child,
    std::uint64_t childEpoch, std::uint64_t outcomeOrdinal) noexcept {
    ContainerPlaybackRow* row = Find(parent);
    if (row == nullptr || child == 0 || childEpoch == 0 || outcomeOrdinal == 0 ||
        (row->mode != static_cast<std::uint8_t>(ContainerModeV1::Random) &&
         row->mode != static_cast<std::uint8_t>(ContainerModeV1::Sequence))) {
        return ChildOutcomeStatus::Invalid;
    }
    if (row->lastConsumedChild == child &&
        row->lastConsumedChildEpoch == childEpoch &&
        row->lastConsumedOutcomeOrdinal == outcomeOrdinal) {
        return ChildOutcomeStatus::Duplicate;
    }
    if (row->phase != ContainerPlaybackPhase::WaitingForChild ||
        row->activeChild != child || row->activeChildEpoch != childEpoch) {
        return ChildOutcomeStatus::Mismatch;
    }
    if (row->loopLimit != 0 && row->completedSelections >= row->loopLimit) {
        return ChildOutcomeStatus::Invalid;
    }
    row->lastConsumedChild = child;
    row->lastConsumedChildEpoch = childEpoch;
    row->lastConsumedOutcomeOrdinal = outcomeOrdinal;
    row->activeChild = 0;
    row->activeChildEpoch = 0;
    if (row->completedSelections != UINT32_MAX) ++row->completedSelections;
    row->phase = ContainerPlaybackPhase::WaitingForNextFrontier;
    return ChildOutcomeStatus::Accepted;
}

PreparedPlayableTreeStep::PreparedPlayableTreeStep(std::uint32_t capacity)
    : instanceCommitProofs_(capacity == 0 ? 1u : capacity),
      containerCommitProofs_(capacity == 0 ? 1u : capacity),
      maxReservations_(capacity == 0 ? 1u : capacity),
      parentLinkRequests_(ParentLinkReservationCapacity(capacity)),
      parentLinkProof_(ParentLinkReservationCapacity(capacity)) {
    instances_.reserve(maxReservations_);
    containers_.reserve(maxReservations_);
}

PreparedPlayableTreeStep::PreparedPlayableTreeStep(
    PreparedPlayableTreeStep&& other) noexcept {
    *this = std::move(other);
}

PreparedPlayableTreeStep& PreparedPlayableTreeStep::operator=(
    PreparedPlayableTreeStep&& other) noexcept {
    if (this != &other && !CommitProofsRequired() && !other.CommitProofsRequired()) {
        if (!TryCancel()) return *this;
        instances_ = std::move(other.instances_);
        containers_ = std::move(other.containers_);
        instanceCommitProofs_ = std::move(other.instanceCommitProofs_);
        containerCommitProofs_ = std::move(other.containerCommitProofs_);
        adoptedInstanceCommitProofs_ = std::move(other.adoptedInstanceCommitProofs_);
        parentLinkRequests_ = std::move(other.parentLinkRequests_);
        parentLinkProof_ = std::move(other.parentLinkProof_);
        parentLinkOwner_ = other.parentLinkOwner_;
        parentLinkAnchor_ = other.parentLinkAnchor_;
        parentLinkRequestCount_ = other.parentLinkRequestCount_;
        adoptedContainerCommitProofs_ = std::move(other.adoptedContainerCommitProofs_);
        adoptedInstances_ = other.adoptedInstances_;
        adoptedContainers_ = other.adoptedContainers_;
        adoptedInstanceCount_ = other.adoptedInstanceCount_;
        adoptedContainerCount_ = other.adoptedContainerCount_;
        maxReservations_ = other.maxReservations_;
        committed_ = other.committed_;
        rollbackRequested_ = other.rollbackRequested_;
        deferLinkPublication_ = other.deferLinkPublication_;
        externalTable_ = other.externalTable_;
        externalInstance_ = other.externalInstance_;
        other.maxReservations_ = 0;
        other.adoptedInstances_.fill(nullptr);
        other.adoptedContainers_.fill(nullptr);
        other.adoptedInstanceCount_ = 0;
        other.adoptedContainerCount_ = 0;
        other.committed_ = false;
        other.rollbackRequested_ = false;
        other.deferLinkPublication_ = false;
        other.externalTable_ = nullptr;
        other.externalInstance_ = nullptr;
        other.ResetParentLinkScope();
    }
    return *this;
}

PreparedPlayableTreeStep::~PreparedPlayableTreeStep() { Cancel(); }

bool PreparedPlayableTreeStep::ResetForReuse(bool published) noexcept {
    if (ContainerCommitProofsHeld() || InstanceCommitProofsHeld() || ParentLinkProofHeld()) return false;
    if (!published && (committed_ || !instances_.empty() || !containers_.empty() ||
        adoptedInstanceCount_ != 0 || adoptedContainerCount_ != 0)) return false;
    instances_.clear();
    containers_.clear();
    adoptedInstances_.fill(nullptr);
    adoptedContainers_.fill(nullptr);
    adoptedInstanceCount_ = 0;
    adoptedContainerCount_ = 0;
    committed_ = false;
    rollbackRequested_ = false;
    containerCommitProofsRequired_ = false;
    instanceCommitProofsRequired_ = false;
    ResetParentLinkScope();
    deferLinkPublication_ = false;
    externalTable_ = nullptr;
    externalInstance_ = nullptr;
    return true;
}

bool PreparedPlayableTreeStep::PrepareInstance(
    PlayableInstanceTable& table, PlayableKind kind,
    PlayableInstanceHandle parent) noexcept {
    if (committed_ || CommitProofsRequired() || instances_.size() >= maxReservations_) {
        return false;
    }
    PreparedPlayableInstance prepared;
    if (!table.PrepareAcquire(kind, parent, &prepared)) {
        return false;
    }
    instances_.push_back(prepared);
    return true;
}

bool PreparedPlayableTreeStep::PrepareContainer(
    PlayableInstanceTable& instanceTable, ContainerPlaybackTable& table,
    PlayableInstanceHandle instance,
    std::uint64_t scheduleEpoch, VnObjectId containerStableId,
    std::uint64_t programSemanticFingerprint, std::uint8_t mode,
    std::uint32_t loopLimit) noexcept {
    if (committed_ || CommitProofsRequired() || containers_.size() >= maxReservations_) {
        return false;
    }
    PreparedContainerPlaybackRow prepared;
    if (!table.Prepare(instanceTable, instance, scheduleEpoch, containerStableId,
                       programSemanticFingerprint, mode, loopLimit,
                       &prepared)) {
        return false;
    }
    prepared.instanceOwner_ = &instanceTable;
    containers_.push_back(std::move(prepared));
    return true;
}

const PreparedPlayableInstance* PreparedPlayableTreeStep::FindPreparedInstanceToken(
    PlayableInstanceHandle handle) const noexcept {
    if (handle == 0) return nullptr;
    const PreparedPlayableInstance* found = nullptr;
    for (const auto& instance : instances_) {
        if (instance.Handle() != handle) continue;
        if (found != nullptr) return nullptr;
        found = &instance;
    }
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        const auto* instance = adoptedInstances_[index];
        if (instance == nullptr || instance->Handle() != handle) continue;
        if (found != nullptr) return nullptr;
        found = instance;
    }
    return found;
}

bool PreparedPlayableTreeStep::AdoptPreparedInstanceReservation(
    PreparedPlayableInstance* prepared) noexcept {
    if (committed_ || CommitProofsRequired() || prepared == nullptr || !prepared->Valid() ||
        prepared == externalInstance_ ||
        (externalInstance_ != nullptr &&
         externalInstance_->owner_ == prepared->owner_ &&
         externalInstance_->Handle() == prepared->Handle()) ||
        instances_.size() + adoptedInstanceCount_ >= maxReservations_ ||
        adoptedInstanceCount_ >= adoptedInstances_.size()) {
        return false;
    }
    for (const auto* existing : adoptedInstances_) {
        if (existing == prepared ||
            (existing != nullptr && existing->Handle() == prepared->Handle())) {
            return false;
        }
    }
    for (const auto& existing : instances_) {
        if (&existing == prepared || existing.Handle() == prepared->Handle()) {
            return false;
        }
    }
    adoptedInstances_[adoptedInstanceCount_++] = prepared;
    return true;
}

bool PreparedPlayableTreeStep::AdoptPreparedContainerReservation(
    PreparedContainerPlaybackRow* prepared) noexcept {
    if (committed_ || CommitProofsRequired() || prepared == nullptr || !prepared->Valid() ||
        containers_.size() + adoptedContainerCount_ >= maxReservations_ ||
        adoptedContainerCount_ >= adoptedContainers_.size()) {
        return false;
    }
    for (const auto* existing : adoptedContainers_) {
        if (existing == prepared ||
            (existing != nullptr && existing->Instance() == prepared->Instance())) {
            return false;
        }
    }
    for (const auto& existing : containers_) {
        if (&existing == prepared || existing.Instance() == prepared->Instance()) {
            return false;
        }
    }
    adoptedContainers_[adoptedContainerCount_++] = prepared;
    return true;
}

void PreparedPlayableTreeStep::SetExternalInstanceContext(
    PlayableInstanceTable* table, PreparedPlayableInstance* prepared) noexcept {
    if (CommitProofsRequired()) return;
    externalTable_ = table;
    externalInstance_ = prepared;
}

bool PreparedPlayableTreeStep::Validate() const noexcept {
    return ValidateActivationCommitProofs() && (!parentLinksRequired_ || ValidateParentLinkScope());
}

bool PreparedPlayableTreeStep::ValidateActivationCommitProofs() const noexcept {
    if (!ValidateReservations()) return false;
    if (instanceCommitProofsRequired_) {
        for (std::size_t i = 0; i < instances_.size(); ++i)
            if (!instanceCommitProofs_[i].AssociatedWith(instances_[i])) return false;
        for (std::uint32_t i = 0; i < adoptedInstanceCount_; ++i)
            if (adoptedInstances_[i] == nullptr ||
                !adoptedInstanceCommitProofs_[i].AssociatedWith(*adoptedInstances_[i])) return false;
    }
    if (containerCommitProofsRequired_) {
        for (std::size_t i = 0; i < containers_.size(); ++i) {
            if (!containerCommitProofs_[i].Associated(containers_[i].owner_, &containers_[i])) return false;
        }
        for (std::uint32_t i = 0; i < adoptedContainerCount_; ++i) {
            const auto* cap = adoptedContainers_[i];
            if (cap == nullptr || !adoptedContainerCommitProofs_[i].Associated(cap->owner_, cap)) return false;
        }
    }
    return true;
}

bool PreparedPlayableTreeStep::ContainerCommitProofsHeld() const noexcept {
    for (std::size_t i = 0; i < containers_.size(); ++i)
        if (containerCommitProofs_[i].Valid()) return true;
    for (std::uint32_t i = 0; i < adoptedContainerCount_; ++i)
        if (adoptedContainerCommitProofs_[i].Valid()) return true;
    return false;
}

bool PreparedPlayableTreeStep::InstanceCommitProofsHeld() const noexcept {
    for (std::size_t i = 0; i < instances_.size(); ++i)
        if (instanceCommitProofs_[i].Valid()) return true;
    for (std::uint32_t i = 0; i < adoptedInstanceCount_; ++i)
        if (adoptedInstanceCommitProofs_[i].Valid()) return true;
    return false;
}

void PreparedPlayableTreeStep::ReleaseInstanceCommitProofs() noexcept {
    ReleaseParentLinkProof();
    for (std::size_t i = 0; i < instances_.size(); ++i) instanceCommitProofs_[i].Release();
    for (std::uint32_t i = 0; i < adoptedInstanceCount_; ++i) adoptedInstanceCommitProofs_[i].Release();
}

bool PreparedPlayableTreeStep::ProveInstanceCommits(PlayableInstanceTable* expectedOwner) noexcept {
    if (committed_ || InstanceCommitProofsHeld() || ParentLinkProofHeld()) {
        ReleaseCommitProofs();
        return false;
    }
    instanceCommitProofsRequired_ = true;
    const auto fail = [&]() noexcept { ReleaseCommitProofs(); return false; };
    if (!ValidateReservations()) return fail();
    const auto count = instances_.size() + adoptedInstanceCount_;
    const auto capAt = [&](std::size_t i) -> PreparedPlayableInstance* {
        return i < instances_.size() ? &instances_[i] : adoptedInstances_[i - instances_.size()];
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto* cap = capAt(i);
        if (cap == nullptr || cap->owner_ == nullptr ||
            (expectedOwner != nullptr && cap->owner_ != expectedOwner) ||
            (externalInstance_ != nullptr && (cap == externalInstance_ ||
                (cap->owner_ == externalTable_ && cap->Handle() == externalInstance_->Handle()))))
            return fail();
        for (std::size_t j = 0; j < i; ++j) {
            const auto* previous = capAt(j);
            if (cap == previous || (cap->owner_ == previous->owner_ && cap->Handle() == previous->Handle()))
                return fail();
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto* cap = capAt(i);
        auto& proof = i < instances_.size() ? instanceCommitProofs_[i]
            : adoptedInstanceCommitProofs_[i - instances_.size()];
        if (!cap->owner_->ProvePreparedCommit(*cap, &proof)) return fail();
    }
    return true;
}

void PreparedPlayableTreeStep::ResetParentLinkScope() noexcept {
    assert(!ParentLinkProofHeld());
    parentLinksRequired_ = false;
    parentLinkOwner_ = nullptr;
    parentLinkAnchor_ = {};
    parentLinkRequestCount_ = 0;
}

bool PreparedPlayableTreeStep::ProveParentLinks(PlayableInstanceTable& expectedOwner) noexcept {
    return ProveParentLinksForScope(expectedOwner, nullptr, nullptr);
}

bool PreparedPlayableTreeStep::ProveParentLinksForScope(PlayableInstanceTable& expectedOwner,
    PreparedPlayableInstance* root, PlayableInstanceTable::PreparedCommitProof* rootActivation) noexcept {
    parentLinksRequired_ = true;
    const auto fail = [&]() noexcept { ReleaseCommitProofs(); return false; };
    if (committed_ || ParentLinkProofHeld() || !instanceCommitProofsRequired_ ||
        !ValidateActivationCommitProofs() || (root == nullptr) != (rootActivation == nullptr) ||
        root != externalInstance_ || (root != nullptr &&
            (externalTable_ != &expectedOwner || !rootActivation->AssociatedWith(*root)))) return fail();
    const auto count = instances_.size() + adoptedInstanceCount_;
    if (count > parentLinkRequests_.size() || (count == 0 && root == nullptr)) return fail();
    for (std::size_t index = 0; index < count; ++index) {
        auto* original = index < instances_.size() ? &instances_[index]
            : adoptedInstances_[index - instances_.size()];
        auto* activation = index < instances_.size() ? &instanceCommitProofs_[index]
            : &adoptedInstanceCommitProofs_[index - instances_.size()];
        if (original == nullptr || original->owner_ != &expectedOwner) return fail();
        parentLinkRequests_[index] = {original, activation};
    }
    parentLinkOwner_ = &expectedOwner;
    parentLinkRequestCount_ = count;
    parentLinkAnchor_ = {root, rootActivation};
    const auto requests = std::span<const PlayableInstanceTable::ParentLinkRequest>(
        parentLinkRequests_.data(), parentLinkRequestCount_);
    const auto anchors = std::span<const PlayableInstanceTable::ParentLinkRequest>(
        &parentLinkAnchor_, root == nullptr ? 0u : 1u);
    if (!expectedOwner.ProvePreparedParentLinks(requests, anchors, &parentLinkProof_)) return fail();
    return true;
}

bool PreparedPlayableTreeStep::ValidateParentLinkScope() const noexcept {
    if (!parentLinksRequired_ || parentLinkOwner_ == nullptr ||
        parentLinkRequestCount_ != instances_.size() + adoptedInstanceCount_ ||
        parentLinkRequestCount_ > parentLinkRequests_.size() ||
        parentLinkAnchor_.original != externalInstance_ ||
        (externalInstance_ != nullptr && externalTable_ != parentLinkOwner_)) return false;
    for (std::size_t index = 0; index < parentLinkRequestCount_; ++index) {
        const auto* original = index < instances_.size() ? &instances_[index]
            : adoptedInstances_[index - instances_.size()];
        const auto* activation = index < instances_.size() ? &instanceCommitProofs_[index]
            : &adoptedInstanceCommitProofs_[index - instances_.size()];
        const auto& request = parentLinkRequests_[index];
        if (request.original != original || request.activation != activation) return false;
    }
    return parentLinkProof_.MatchesOriginalScope(parentLinkOwner_,
        std::span<const PlayableInstanceTable::ParentLinkRequest>(parentLinkRequests_.data(), parentLinkRequestCount_),
        std::span<const PlayableInstanceTable::ParentLinkRequest>(&parentLinkAnchor_,
            externalInstance_ == nullptr ? 0u : 1u));
}

bool PreparedPlayableTreeStep::PublishPreparedLinksForScope() noexcept {
    if (parentLinksRequired_) {
        if (!ValidateParentLinkScope()) return false;
        PlayableInstanceTable::ConsumePreparedParentLinks(std::move(parentLinkProof_));
        return true;
    }
    for (auto& instance : instances_)
        if (!instance.owner_->PublishPreparedLink(instance.handle_)) return false;
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        auto* instance = adoptedInstances_[index];
        if (instance == nullptr || instance->owner_ == nullptr ||
            !instance->owner_->PublishPreparedLink(instance->handle_)) return false;
    }
    return true;
}

void PreparedPlayableTreeStep::ReleaseContainerCommitProofs() noexcept {
    for (std::size_t i = 0; i < containers_.size(); ++i) containerCommitProofs_[i].Release();
    for (std::uint32_t i = 0; i < adoptedContainerCount_; ++i) adoptedContainerCommitProofs_[i].Release();
    // Required routing survives release until the original scope actually retires.
}

bool PreparedPlayableTreeStep::ProveContainerCommits(ContainerPlaybackTable* expectedOwner,
    const PreparedContainerPlaybackRow* required) noexcept {
    const auto fail = [&]() noexcept { ReleaseCommitProofs(); return false; };
    if (ContainerCommitProofsHeld() || !ValidateReservations() ||
        ((expectedOwner == nullptr) != (required == nullptr))) return fail();
    const auto count = containers_.size() + adoptedContainerCount_;
    const auto capAt = [&](std::size_t i) -> PreparedContainerPlaybackRow* {
        return i < containers_.size() ? &containers_[i] : adoptedContainers_[i - containers_.size()];
    };
    bool foundRequired = required == nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* cap = capAt(i);
        if (cap == nullptr || cap->owner_ == nullptr ||
            (expectedOwner != nullptr && cap->owner_ != expectedOwner)) return fail();
        foundRequired = foundRequired || cap == required;
        for (std::size_t j = 0; j < i; ++j) {
            const auto* previous = capAt(j);
            if (cap == previous || (cap->owner_ == previous->owner_ && cap->slot_ == previous->slot_))
                return fail();
        }
    }
    if (!foundRequired) return fail();
    containerCommitProofsRequired_ = true;
    for (std::size_t i = 0; i < count; ++i) {
        auto* cap = capAt(i);
        auto& proof = i < containers_.size() ? containerCommitProofs_[i]
            : adoptedContainerCommitProofs_[i - containers_.size()];
        if (!cap->owner_->ProvePreparedCommit(*cap, &proof)) {
            return fail();
        }
    }
    return true;
}

bool PreparedPlayableTreeStep::ValidateReservations() const noexcept {
    if (committed_) {
        return false;
    }
    for (const auto& instance : instances_) {
        if (!instance.Valid()) {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        const auto* instance = adoptedInstances_[index];
        if (instance == nullptr || !instance->Valid() ||
            instance == externalInstance_) {
            return false;
        }
    }
    const auto ownerForHandle = [&](PlayableInstanceHandle handle) noexcept
        -> PlayableInstanceTable* {
        PlayableInstanceTable* owner = nullptr;
        bool ambiguous = false;
        const auto consider = [&](PlayableInstanceTable* candidate) noexcept {
            if (candidate == nullptr) {
                return;
            }
            if (owner == nullptr) {
                owner = candidate;
            } else if (owner != candidate) {
                ambiguous = true;
            }
        };
        for (const auto& instance : instances_) {
            if (instance.Handle() == handle) {
                consider(instance.owner_);
            }
        }
        for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
            const auto* instance = adoptedInstances_[index];
            if (instance != nullptr && instance->Handle() == handle) {
                consider(instance->owner_);
            }
        }
        if (externalInstance_ != nullptr &&
            externalInstance_->Handle() == handle) {
            consider(externalTable_);
        }
        return ambiguous ? nullptr : owner;
    };
    const auto matchesInstance = [&](PlayableInstanceHandle handle,
                                     std::uint64_t epoch,
                                     PlayableInstanceTable* owner) noexcept {
        if (owner == nullptr) {
            return false;
        }
        for (const auto& instance : instances_) {
            if (instance.owner_ == owner && instance.Handle() == handle) {
                return epoch == 0 || instance.ScheduleEpoch() == epoch;
            }
        }
        for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
            const auto* instance = adoptedInstances_[index];
            if (instance != nullptr && instance->owner_ == owner &&
                instance->Handle() == handle) {
                return epoch == 0 || instance->ScheduleEpoch() == epoch;
            }
        }
        if (externalTable_ == owner && externalInstance_ != nullptr &&
            externalInstance_->Handle() == handle) {
            return epoch == 0 || externalInstance_->ScheduleEpoch() == epoch;
        }
        const auto view = owner->Classify(handle);
        return view.live && (epoch == 0 || view.scheduleEpoch == epoch);
    };
    for (const auto& container : containers_) {
        if (!container.Valid() || container.instanceOwner_ == nullptr) {
            return false;
        }
        if (ownerForHandle(container.Instance()) != container.instanceOwner_) {
            return false;
        }
        if (!matchesInstance(
                container.Instance(), container.ExpectedScheduleEpoch(),
                container.instanceOwner_)) {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < adoptedContainerCount_; ++index) {
        const auto* container = adoptedContainers_[index];
        if (container == nullptr || !container->Valid() ||
            container->instanceOwner_ == nullptr ||
            ownerForHandle(container->Instance()) != container->instanceOwner_ ||
            !matchesInstance(
                container->Instance(), container->ExpectedScheduleEpoch(),
                container->instanceOwner_)) {
            return false;
        }
    }
    for (const auto& instance : instances_) {
        const auto parent = instance.Parent();
        if (parent == 0) {
            continue;
        }
        bool parentIsPrepared = false;
        for (const auto& candidate : instances_) {
            if (candidate.owner_ == instance.owner_ &&
                candidate.Handle() == parent) {
                parentIsPrepared = true;
                break;
            }
        }
        if (parentIsPrepared) {
            for (const auto& candidate : instances_) {
                if (candidate.owner_ == instance.owner_ &&
                    candidate.Handle() == parent && !candidate.Valid()) {
                    return false;
                }
            }
        } else if (externalTable_ == instance.owner_ &&
                   externalInstance_ != nullptr &&
                   externalInstance_->Handle() == parent &&
                   (externalInstance_->Valid() ||
                    (externalTable_ != nullptr &&
                     externalTable_->Classify(parent).live))) {
        } else if (!matchesInstance(parent, 0, instance.owner_)) {
            // Parent reservations are allowed to be outside this step only
            // when they are already live in the same instance table.
            return false;
        }
    }
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        const auto* instance = adoptedInstances_[index];
        const auto parent = instance->Parent();
        if (parent == 0) continue;
        bool parentKnown = false;
        for (const auto& candidate : instances_) {
            parentKnown = (candidate.owner_ == instance->owner_ &&
                           candidate.Handle() == parent) || parentKnown;
        }
        for (std::uint32_t candidate = 0;
             candidate < adoptedInstanceCount_; ++candidate) {
            const auto* parentCandidate = adoptedInstances_[candidate];
            parentKnown = (parentCandidate != nullptr &&
                           parentCandidate->owner_ == instance->owner_ &&
                           parentCandidate->Handle() == parent) || parentKnown;
        }
        if (externalTable_ == instance->owner_ &&
            externalInstance_ != nullptr &&
            externalInstance_->Handle() == parent) {
            parentKnown = true;
        }
        if (!parentKnown &&
            (instance->owner_ == nullptr ||
             !instance->owner_->Classify(parent).live)) {
            return false;
        }
    }
    return true;
}

bool PreparedPlayableTreeStep::Commit() noexcept {
    const bool publishLinks = !deferLinkPublication_;
    deferLinkPublication_ = false;
    if (!Validate()) {
        Cancel();
        return false;
    }
    std::uint32_t committedInstances = 0;
    const auto rollback = [&]() noexcept {
        // Every native token survives a failed cancellation, including a
        // partial commit rolled back into its real Preparing reservation.
        committed_ = true;
        (void)RollbackCommitted();
    };
    for (auto& instance : instances_) {
        if (instanceCommitProofsRequired_) {
            PlayableInstanceTable::ConsumePreparedCommit(std::move(instanceCommitProofs_[committedInstances]));
        } else if (!instance.owner_->CommitPrepared(&instance, false)) {
            rollback();
            return false;
        }
        ++committedInstances;
    }
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        auto* instance = adoptedInstances_[index];
        if (instanceCommitProofsRequired_) {
            PlayableInstanceTable::ConsumePreparedCommit(std::move(adoptedInstanceCommitProofs_[index]));
        } else if (instance == nullptr || instance->owner_ == nullptr ||
            !instance->owner_->CommitPrepared(instance, false)) {
            rollback();
            return false;
        }
    }
    for (std::size_t index = 0; index < containers_.size(); ++index) {
        auto& container = containers_[index];
        if (containerCommitProofsRequired_) {
            ContainerPlaybackTable::ConsumePreparedCommit(std::move(containerCommitProofs_[index]));
        } else if (!container.owner_->Commit(&container)) {
            rollback();
            return false;
        }
    }
    for (std::uint32_t index = 0; index < adoptedContainerCount_; ++index) {
        auto* container = adoptedContainers_[index];
        if (containerCommitProofsRequired_) {
            ContainerPlaybackTable::ConsumePreparedCommit(std::move(adoptedContainerCommitProofs_[index]));
        } else if (container == nullptr || container->owner_ == nullptr ||
            !container->owner_->Commit(container)) {
            rollback();
            return false;
        }
    }
    if (publishLinks && !PublishPreparedLinksForScope()) {
        rollback();
        return false;
    }
    committed_ = true;
    return true;
}

bool PreparedPlayableTreeStep::CommitWithoutPublishingLinks() noexcept {
    deferLinkPublication_ = true;
    return Commit();
}

bool PreparedPlayableTreeStep::PublishPreparedLinks() noexcept {
    if (!committed_) {
        return false;
    }
    return PublishPreparedLinksForScope();
}

bool PreparedPlayableTreeStep::HasSourceRetirementHolds() const noexcept {
    const auto sourceHeld = [](const PreparedPlayableInstance* instance) {
        return instance != nullptr && instance->owner_ != nullptr &&
            instance->owner_->HasSourceRetirementHold(instance->handle_);
    };
    for (const auto& instance : instances_) {
        if (sourceHeld(&instance)) return true;
    }
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        if (sourceHeld(adoptedInstances_[index])) return true;
    }
    return sourceHeld(externalInstance_);
}

bool PreparedPlayableTreeStep::RollbackCommitted() noexcept {
    ReleaseCommitProofs();
    if (!committed_) return false;
    rollbackRequested_ = true;
    // Retain the complete private tree while a render owner still holds a leaf.
    if (HasSourceRetirementHolds()) return false;
    bool ok = true;
    const auto rollbackContainer = [&](PreparedContainerPlaybackRow* container) {
        if (container == nullptr) {
            return false;
        }
        if (container->owner_ == nullptr && container->instance_ == 0) {
            return true;
        }
        if (container->Valid()) return container->owner_->Cancel(container);
        if (container->owner_ == nullptr || container->instance_ == 0 ||
            !container->owner_->RollbackCommitted(container->instance_)) {
            return false;
        }
        // RollbackCommitted clears the table row rather than leaving a
        // prepared token, so clear the token explicitly after success.
        *container = {};
        return true;
    };
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it) {
        ok = rollbackContainer(&*it) && ok;
    }
    for (std::uint32_t index = adoptedContainerCount_; index-- > 0;) {
        ok = rollbackContainer(adoptedContainers_[index]) && ok;
    }

    const auto rollbackInstance = [](PreparedPlayableInstance* instance) {
        if (instance == nullptr) {
            return false;
        }
        if (instance->owner_ == nullptr && instance->handle_ == 0) {
            return true;
        }
        if (instance->owner_ == nullptr || instance->handle_ == 0) {
            return false;
        }
        // A prior pass may have moved the row back to the prepared state but
        // failed to cancel its token.  Retry cancellation before attempting
        // another active-row rollback; otherwise an already rolled-back row
        // would be mistaken for a new rollback failure.
        if (instance->owner_->CancelPrepared(instance)) {
            return true;
        }
        if (!instance->owner_->RollbackCommitted(instance->handle_)) {
            return false;
        }
        return instance->owner_->CancelPrepared(instance);
    };

    // Reservations can be split between private and adopted storage while
    // their parent-child topology remains global.  Retry reverse traversal
    // until a complete pass makes no progress, allowing an adopted child to
    // unblock an internal parent (and vice versa).
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto it = instances_.rbegin(); it != instances_.rend(); ++it) {
            if (it->owner_ == nullptr && it->handle_ == 0) {
                continue;
            }
            if (rollbackInstance(&*it)) {
                progress = true;
            }
        }
        for (std::uint32_t index = adoptedInstanceCount_; index-- > 0;) {
            auto* instance = adoptedInstances_[index];
            if (instance == nullptr ||
                (instance->owner_ == nullptr && instance->handle_ == 0)) {
                continue;
            }
            if (rollbackInstance(instance)) {
                progress = true;
            }
        }
        if (externalTable_ != nullptr && externalInstance_ != nullptr &&
            (externalInstance_->owner_ != nullptr ||
             externalInstance_->handle_ != 0)) {
            bool rolledBack = externalTable_->CancelPrepared(externalInstance_);
            if (!rolledBack && externalTable_->RollbackCommitted(
                                   externalInstance_->handle_)) {
                rolledBack = externalTable_->CancelPrepared(externalInstance_);
            }
            if (rolledBack) {
                progress = true;
            }
        }
    }

    const auto unresolvedInstance = [](const PreparedPlayableInstance* instance) {
        return instance != nullptr &&
               !(instance->owner_ == nullptr && instance->handle_ == 0);
    };
    bool instancesResolved = true;
    for (const auto& instance : instances_) {
        instancesResolved = !unresolvedInstance(&instance) && instancesResolved;
    }
    for (std::uint32_t index = 0; index < adoptedInstanceCount_; ++index) {
        instancesResolved =
            !unresolvedInstance(adoptedInstances_[index]) && instancesResolved;
    }
    if (externalInstance_ != nullptr) {
        instancesResolved =
            !unresolvedInstance(externalInstance_) && instancesResolved;
    }
    bool containersResolved = true;
    for (const auto& container : containers_) {
        containersResolved =
            (container.owner_ == nullptr && container.instance_ == 0) &&
            containersResolved;
    }
    for (std::uint32_t index = 0; index < adoptedContainerCount_; ++index) {
        const auto* container = adoptedContainers_[index];
        containersResolved =
            (container != nullptr && container->owner_ == nullptr &&
             container->instance_ == 0) &&
            containersResolved;
    }
    // A failed pass can be a dependency-order rejection: a parent is
    // retried after its child is reclaimed.  Only unresolved ownership at the
    // end is a failed rollback result; successful retries must retain the
    // existing all-or-nothing return contract.
    ok = instancesResolved && containersResolved;
    if (ok && instancesResolved && containersResolved) {
        instances_.clear();
        containers_.clear();
        adoptedInstances_.fill(nullptr);
        adoptedContainers_.fill(nullptr);
        adoptedInstanceCount_ = 0;
        adoptedContainerCount_ = 0;
        committed_ = false;
        rollbackRequested_ = false;
        containerCommitProofsRequired_ = false;
        instanceCommitProofsRequired_ = false;
        ResetParentLinkScope();
        externalTable_ = nullptr;
        externalInstance_ = nullptr;
    }
    return ok;
}

void PreparedPlayableTreeStep::Cancel() noexcept { (void)TryCancel(); }

bool PreparedPlayableTreeStep::TryCancel() noexcept {
    ReleaseCommitProofs();
    if (committed_) return rollbackRequested_ && RollbackCommitted();
    if (HasSourceRetirementHolds()) return false;
    const auto cancelContainer = [](PreparedContainerPlaybackRow* token) noexcept {
        if (token == nullptr) return false;
        if (token->owner_ == nullptr && token->instance_ == 0) return true;
        return token->Valid() && token->owner_->Cancel(token);
    };
    const auto cancelInstance = [](PreparedPlayableInstance* token) noexcept {
        if (token == nullptr) return false;
        if (token->owner_ == nullptr && token->handle_ == 0) return true;
        return token->owner_ != nullptr && token->owner_->CancelPrepared(token);
    };
    bool resolved = true;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        resolved = cancelContainer(&*it) && resolved;
    for (std::uint32_t index = adoptedContainerCount_; index-- > 0;)
        resolved = cancelContainer(adoptedContainers_[index]) && resolved;
    for (auto it = instances_.rbegin(); it != instances_.rend(); ++it)
        resolved = cancelInstance(&*it) && resolved;
    for (std::uint32_t index = adoptedInstanceCount_; index-- > 0;)
        resolved = cancelInstance(adoptedInstances_[index]) && resolved;
    if (!resolved) return false;
    instances_.clear();
    containers_.clear();
    adoptedInstances_.fill(nullptr);
    adoptedContainers_.fill(nullptr);
    adoptedInstanceCount_ = 0;
    adoptedContainerCount_ = 0;
    containerCommitProofsRequired_ = false;
    instanceCommitProofsRequired_ = false;
    ResetParentLinkScope();
    return true;
}

bool PlayableTreeExecutor::ProveParentLinks(PlayableInstanceTable& expectedOwner,
    const PreparedPlayableInstance* requiredRoot) noexcept {
    step_.parentLinksRequired_ = true;
    const auto fail = [&]() noexcept { ReleaseCommitProofs(); return false; };
    if (step_.Committed() || ParentLinkProofHeld() ||
        (externalInstance_ == nullptr && requiredRoot != nullptr) ||
        (externalInstance_ != nullptr && (requiredRoot != externalInstance_ ||
            instanceTable_ != &expectedOwner || !externalInstanceCommitProof_.AssociatedWith(*externalInstance_))))
        return fail();
    if (!step_.ProveParentLinksForScope(expectedOwner, externalInstance_,
            externalInstance_ == nullptr ? nullptr : &externalInstanceCommitProof_)) return fail();
    return true;
}

bool PlayableTreeExecutor::ProveInstanceCommits(PlayableInstanceTable* expectedOwner,
    const PreparedPlayableInstance* requiredRoot) noexcept {
    if (step_.Committed() || InstanceCommitProofsHeld() || ParentLinkProofHeld()) {
        ReleaseCommitProofs();
        return false;
    }
    step_.instanceCommitProofsRequired_ = true;
    const auto fail = [&]() noexcept { ReleaseCommitProofs(); return false; };
    if ((expectedOwner == nullptr) != (requiredRoot == nullptr)) return fail();
    if (externalInstance_ != nullptr) {
        if (expectedOwner == nullptr || requiredRoot != externalInstance_ || instanceTable_ != expectedOwner ||
            !expectedOwner->OwnsPrepared(*externalInstance_) || !step_.ValidateReservations() ||
            !expectedOwner->ProvePreparedCommit(*externalInstance_, &externalInstanceCommitProof_)) return fail();
    } else if (requiredRoot != nullptr) {
        return fail();
    }
    if (!step_.ProveInstanceCommits(expectedOwner)) return fail();
    return true;
}

bool PlayableTreeExecutor::IssueOriginalPreparedInstanceLifetimes(
    PlayableInstanceTable& expectedOwner,
    std::span<const PreparedPlayableInstance* const> originals,
    std::span<PlayableInstanceTable::OriginalLifetimePin> stableBaselines) noexcept {
    const auto stepCount = step_.instances_.size() + step_.adoptedInstanceCount_;
    const auto count = stepCount + (externalInstance_ == nullptr ? 0u : 1u);
    if (step_.Committed() || !step_.instanceCommitProofsRequired_ || !Validate() ||
        count == 0 || originals.size() != count || stableBaselines.size() != expectedOwner.Capacity() ||
        (externalInstance_ != nullptr && (instanceTable_ != &expectedOwner ||
            !externalInstanceCommitProof_.AssociatedWith(*externalInstance_)))) return false;
    const auto originalAt = [&](std::size_t index) -> const PreparedPlayableInstance* {
        if (index == stepCount) return externalInstance_;
        return index < step_.instances_.size() ? &step_.instances_[index]
            : step_.adoptedInstances_[index - step_.instances_.size()];
    };
    const auto proofAt = [&](std::size_t index) -> const PlayableInstanceTable::PreparedCommitProof& {
        if (index == stepCount) return externalInstanceCommitProof_;
        return index < step_.instances_.size() ? step_.instanceCommitProofs_[index]
            : step_.adoptedInstanceCommitProofs_[index - step_.instances_.size()];
    };
    // Complete pointer identity is checked before issuing any structural retention.
    for (std::size_t index = 0; index < count; ++index) {
        const auto* original = originalAt(index);
        if (original == nullptr || !expectedOwner.OwnsPrepared(*original) ||
            !proofAt(index).AssociatedWith(*original) ||
            std::count(originals.begin(), originals.end(), original) != 1 ||
            static_cast<std::uint32_t>(original->Handle()) >= stableBaselines.size() ||
            stableBaselines[static_cast<std::uint32_t>(original->Handle())].Valid()) return false;
    }
    std::size_t issued = 0;
    for (; issued < count; ++issued) {
        const auto slot = static_cast<std::uint32_t>(originalAt(issued)->Handle());
        if (!expectedOwner.IssueOriginalPreparedLifetime(proofAt(issued), &stableBaselines[slot])) break;
    }
    if (issued == count) return true;
    while (issued != 0)
        stableBaselines[static_cast<std::uint32_t>(originalAt(--issued)->Handle())].Release();
    return false;
}

PlayableTreeExecutor::PlayableTreeExecutor(PlayableTreeExecutor&& other) noexcept
    : step_(std::move(other.step_)) {
    if (other.step_.CommitProofsRequired()) return;
    externalInstanceCommitProof_ = std::move(other.externalInstanceCommitProof_);
    ownerTokens_ = other.ownerTokens_;
    ownerTokenCount_ = other.ownerTokenCount_;
    committedOwnerTokens_ = other.committedOwnerTokens_;
    committedOwnerTokenCount_ = other.committedOwnerTokenCount_;
    instanceTable_ = other.instanceTable_;
    externalInstance_ = other.externalInstance_;
    published_ = other.published_;
    ownershipTransferred_ = other.ownershipTransferred_;
    retainPreparedOwnerRollback_ = other.retainPreparedOwnerRollback_;
    other.ownerTokens_.fill({});
    other.ownerTokenCount_ = 0;
    other.committedOwnerTokens_.fill({});
    other.committedOwnerTokenCount_ = 0;
    other.instanceTable_ = nullptr;
    other.externalInstance_ = nullptr;
    other.published_ = false;
    other.ownershipTransferred_ = false;
    other.retainPreparedOwnerRollback_ = false;
}

PlayableTreeExecutor& PlayableTreeExecutor::operator=(PlayableTreeExecutor&& other) noexcept {
    if (this == &other || step_.CommitProofsRequired() || other.step_.CommitProofsRequired())
        return *this;
    // Move-overwrite cannot discard a real unresolved private owner.
    if (instanceTable_ != nullptr) {
        if (!Cancel(*instanceTable_)) return *this;
    } else {
        Cancel();
        if (ownerTokenCount_ != 0 || committedOwnerTokenCount_ != 0 || !step_.TryCancel()) return *this;
    }
    step_ = std::move(other.step_);
    externalInstanceCommitProof_ = std::move(other.externalInstanceCommitProof_);
    ownerTokens_ = other.ownerTokens_;
    ownerTokenCount_ = other.ownerTokenCount_;
    committedOwnerTokens_ = other.committedOwnerTokens_;
    committedOwnerTokenCount_ = other.committedOwnerTokenCount_;
    instanceTable_ = other.instanceTable_;
    externalInstance_ = other.externalInstance_;
    published_ = other.published_;
    ownershipTransferred_ = other.ownershipTransferred_;
    retainPreparedOwnerRollback_ = other.retainPreparedOwnerRollback_;
    other.ownerTokens_.fill({});
    other.ownerTokenCount_ = 0;
    other.committedOwnerTokens_.fill({});
    other.committedOwnerTokenCount_ = 0;
    other.instanceTable_ = nullptr;
    other.externalInstance_ = nullptr;
    other.published_ = false;
    other.ownershipTransferred_ = false;
    other.retainPreparedOwnerRollback_ = false;
    return *this;
}

} // namespace harken
