#ifndef HARKEN_CORE_ACTION_FIRING_TABLE_H
#define HARKEN_CORE_ACTION_FIRING_TABLE_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/PackReadinessContract.h"

#include <cstdint>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kMaxActionFiringsHard = 4096;
inline constexpr std::uint32_t kActionFiringSlotBits = 12;
inline constexpr std::uint64_t kMaxActionFiringGeneration =
    UINT64_MAX >> kActionFiringSlotBits;

using ActionFiringHandle = std::uint64_t;

enum class ActionFiringOwnerKind : std::uint8_t {
    DirectWorld = 1,
    Pack = 2,
};

struct ActionFiringOwnerStamp {
    ActionFiringOwnerKind kind = ActionFiringOwnerKind::DirectWorld;
    std::uint64_t worldGeneration = 0;
    PackPartitionLease pack{};
    VnObjectId projectId{};

    static ActionFiringOwnerStamp DirectWorld(
        std::uint64_t generation, VnObjectId projectId = {}) noexcept {
        ActionFiringOwnerStamp owner;
        owner.kind = ActionFiringOwnerKind::DirectWorld;
        owner.worldGeneration = generation;
        owner.projectId = projectId;
        return owner;
    }

    static ActionFiringOwnerStamp Pack(
        const PackPartitionLease& lease, VnObjectId projectId = {}) noexcept {
        ActionFiringOwnerStamp owner;
        owner.kind = ActionFiringOwnerKind::Pack;
        owner.pack = lease;
        owner.projectId = projectId;
        return owner;
    }

    bool Valid() const noexcept;
};

bool operator==(const ActionFiringOwnerStamp& left,
                const ActionFiringOwnerStamp& right) noexcept;

enum class ActionFiringState : std::uint8_t {
    Active = 0,
    Stopping = 1,
    TerminalRetained = 2,
};

enum class ActionFiringOutcome : std::uint8_t {
    Pending = 0,
    Succeeded = 1,
    SucceededWithStepFailure = 2,
    Failed = 3,
    Cancelled = 4,
};

enum class ActionStepState : std::uint8_t {
    Pending = 0,
    Active = 1,
    Succeeded = 2,
    NoOp = 3,
    Suppressed = 4,
    Failed = 5,
    Cancelled = 6,
    AwaitingMaterialization = 7,
};

enum class ActionTerminalReason : std::uint8_t {
    None = 0,
    Natural = 1,
    StopRequested = 2,
    WorldReplaced = 3,
    TransportReset = 4,
    TargetGone = 5,
    Capacity = 6,
    Deadline = 7,
    GenerationExhausted = 8,
    MediaNotReady = 9,
    CapabilityUnavailable = 10,
    RuntimeError = 11,
};

struct ActionFiringStepResult {
    ActionStepState state = ActionStepState::Pending;
    std::int32_t result = 0;
    ActionTerminalReason terminalReason = ActionTerminalReason::None;
    std::uint32_t opcode = 0;
    VnObjectId ownerActionId{};
    VnObjectId targetStableId{};
    std::uint64_t logicCommitSequence = 0;
    ActionFiringHandle childFiring = 0;
    std::uint64_t producedPlayableInstance = 0;
    std::uint32_t authoredEventIndex = 0;
    std::uint32_t executionOrdinal = 0;
    std::uint32_t depth = 0;
    std::uint32_t targetKind = 0;
    std::uint32_t capturedPlayableCount = 0;
    std::uint32_t remainingPlayableCount = 0;
};

struct ActionFiringSnapshot {
    ActionFiringState state = ActionFiringState::Active;
    ActionFiringOutcome outcome = ActionFiringOutcome::Pending;
    ActionTerminalReason terminalReason = ActionTerminalReason::None;
    VnObjectId actionId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t worldGeneration = 0;
    ActionFiringOwnerStamp owner{};
    std::uint64_t cancellationEpoch = 0;
    std::uint32_t ownedDeferredCount = 0;
    std::uint64_t acceptedLogicSequence = 0;
    ActionFiringHandle parent = 0;
    std::uint32_t directStepCount = 0;
    std::uint32_t directStepTerminalCount = 0;
    std::uint32_t childFiringCount = 0;
    std::uint32_t livePlayableCount = 0;
};

class ActionFiringTable;

class PreparedActionFiring {
public:
    bool Valid() const noexcept;
    ActionFiringHandle Handle() const noexcept { return handle_; }
    // Only a still-prepared child of its live, uncancelled original parent
    // carries an inherited accepted order. Roots cannot mint such an order.
    std::uint64_t InheritedSequence() const noexcept;

private:
    friend class ActionFiringTable;
    ActionFiringTable* owner_ = nullptr;
    ActionFiringHandle handle_ = 0;
    bool prepared_ = false;
};

class ActionFiringTable {
public:
    bool Reset(std::uint32_t capacity);
    bool PrepareRoot(VnObjectId actionId,
                     const ActionFiringOwnerStamp& owner,
                     std::uint32_t directStepCount,
                     PreparedActionFiring* out) noexcept;
    bool PrepareRoot(VnObjectId actionId, std::uint64_t worldGeneration,
                     std::uint32_t directStepCount,
                     PreparedActionFiring* out) noexcept;
    bool PrepareChild(const PreparedActionFiring& parent,
                      VnObjectId actionId,
                      const ActionFiringOwnerStamp& owner,
                      std::uint32_t directStepCount,
                      PreparedActionFiring* out) noexcept;
    bool PrepareChild(const PreparedActionFiring& parent,
                      VnObjectId actionId,
                      std::uint64_t worldGeneration,
                      std::uint32_t directStepCount,
                      PreparedActionFiring* out) noexcept;
    bool PrepareChild(ActionFiringHandle parent,
                      VnObjectId actionId,
                      const ActionFiringOwnerStamp& owner,
                      std::uint32_t directStepCount,
                      PreparedActionFiring* out) noexcept;
    bool CommitPreparedChild(PreparedActionFiring* prepared) noexcept;
    bool SetPreparedProgramFingerprint(const PreparedActionFiring& prepared,
                                        std::uint64_t fingerprint) noexcept;
    bool SetPreparedStep(const PreparedActionFiring& prepared,
                         std::uint32_t index,
                         const ActionFiringStepResult& result) noexcept;
    bool UpdateStep(ActionFiringHandle handle, std::uint32_t index,
                    const ActionFiringStepResult& result) noexcept;
    bool TransitionStep(ActionFiringHandle handle, std::uint32_t index,
                        ActionStepState expected,
                        const ActionFiringStepResult& result) noexcept;
    bool SetPreparedDeferredStopCount(
        const PreparedActionFiring& prepared, std::uint32_t index,
        std::uint32_t capturedCount) noexcept;
    bool SettleDeferredStopLeaf(ActionFiringHandle handle,
                                std::uint32_t index, bool stopped,
                                std::int32_t result,
                                ActionTerminalReason reason) noexcept;
    bool SetLivePlayableCount(ActionFiringHandle handle,
                              std::uint32_t count) noexcept;
    bool AddLivePlayableCount(ActionFiringHandle handle,
                              std::uint32_t count) noexcept;
    bool AcquireDeferred(ActionFiringHandle handle) noexcept;
    bool ReleaseDeferred(ActionFiringHandle handle) noexcept;
    std::uint32_t OwnedDeferredCount(
        ActionFiringHandle handle) const noexcept;
    std::uint64_t CancellationEpoch(
        ActionFiringHandle handle) const noexcept;
    bool BumpCancellationEpoch(ActionFiringHandle handle,
                                std::uint64_t* outEpoch = nullptr) noexcept;
    bool CompletePlayable(ActionFiringHandle handle,
                          std::uint32_t executionOrdinal,
                          std::int32_t result,
                          ActionTerminalReason reason) noexcept;
    bool CompleteAuthoredStop(ActionFiringHandle handle,
                              std::uint32_t executionOrdinal) noexcept;
    bool Commit(PreparedActionFiring* prepared,
                std::uint64_t acceptedLogicSequence) noexcept;
    bool CommitTree(PreparedActionFiring* root,
                    std::uint64_t acceptedLogicSequence) noexcept;
    bool Abandon(PreparedActionFiring* prepared) noexcept;
    bool MarkStopping(ActionFiringHandle handle,
                      ActionTerminalReason reason) noexcept;
    bool MarkTreeStopping(ActionFiringHandle member,
                          ActionTerminalReason reason) noexcept;
    bool MarkTerminal(
        ActionFiringHandle handle,
        ActionFiringOutcome outcome = ActionFiringOutcome::Succeeded,
        ActionTerminalReason reason = ActionTerminalReason::Natural) noexcept;
    bool ReleaseRoot(ActionFiringHandle handle) noexcept;
    bool Capture(ActionFiringHandle handle,
                 ActionFiringSnapshot* out) const noexcept;
    ActionFiringHandle ActiveRootAt(std::uint32_t slot) const noexcept;
    bool CaptureTerminalTreeOwners(
        ActionFiringHandle root, ActionFiringOwnerStamp* outOwners,
        std::uint32_t ownerCapacity,
        std::uint32_t* outOwnerCount) const noexcept;
    bool BelongsToTree(ActionFiringHandle root,
                       ActionFiringHandle member) const noexcept;
    bool TreeLivePlayableCount(ActionFiringHandle root,
                               std::uint32_t* out) const noexcept;
    bool TreeOwnedDeferredCount(ActionFiringHandle root,
                                std::uint32_t* out) const noexcept;
    bool CaptureStep(ActionFiringHandle handle, std::uint32_t index,
                     ActionFiringStepResult* out) const noexcept;

    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(rows_.size());
    }
    std::uint32_t LiveCount() const noexcept { return liveCount_; }
    std::uint32_t HighWater() const noexcept { return highWater_; }
    std::uint32_t FreeCount() const noexcept;
    std::uint32_t QuarantinedCount() const noexcept {
        return quarantinedCount_;
    }
    std::uint64_t RefusalCount() const noexcept { return refusalCount_; }
    bool SetFreeGenerationForTests(std::uint32_t slot,
                                   std::uint64_t generation) noexcept;
    bool IsPrepared(ActionFiringHandle handle) const noexcept;

private:
    friend class PreparedActionFiring;
    std::uint64_t InheritedSequence(const PreparedActionFiring& child) const noexcept;
    enum class SlotState : std::uint8_t {
        Free = 0,
        Prepared = 1,
        Active = 2,
        Stopping = 3,
        TerminalRetained = 4,
        Quarantined = 5,
    };

    struct Row {
        SlotState state = SlotState::Free;
        std::uint64_t generation = 0;
        VnObjectId actionId{};
        std::uint64_t programSemanticFingerprint = 0;
        std::uint64_t worldGeneration = 0;
        ActionFiringOwnerStamp owner{};
        std::uint64_t cancellationEpoch = 0;
        std::uint64_t acceptedLogicSequence = 0;
        ActionFiringOutcome outcome = ActionFiringOutcome::Pending;
        ActionTerminalReason terminalReason = ActionTerminalReason::None;
        std::int32_t terminalResult = 0;
        ActionFiringHandle parent = 0;
        ActionFiringHandle root = 0;
        ActionFiringHandle firstChild = 0;
        ActionFiringHandle nextSibling = 0;
        std::uint32_t directStepCount = 0;
        std::uint32_t directStepTerminalCount = 0;
        std::uint32_t childFiringCount = 0;
        std::uint32_t livePlayableCount = 0;
        std::uint32_t ownedDeferredCount = 0;
    };

    static ActionFiringHandle MakeHandle(std::uint32_t slot,
                                         std::uint64_t generation) noexcept;
    static std::uint32_t SlotOf(ActionFiringHandle handle) noexcept;
    static std::uint64_t GenerationOf(ActionFiringHandle handle) noexcept;
    Row* Resolve(ActionFiringHandle handle) noexcept;
    const Row* Resolve(ActionFiringHandle handle) const noexcept;
    Row* ResolvePrepared(const PreparedActionFiring& prepared) noexcept;
    bool Recycle(std::uint32_t slot, Row& row) noexcept;
    bool UnlinkChild(Row& parent, ActionFiringHandle child) noexcept;
    bool ActivatePreparedSubtree(ActionFiringHandle handle,
                                 std::uint64_t acceptedLogicSequence,
                                 std::uint32_t* committed) noexcept;
    bool AbandonPreparedSubtree(ActionFiringHandle handle) noexcept;
    bool MarkStoppingSubtree(ActionFiringHandle handle,
                             ActionTerminalReason reason) noexcept;
    bool ReleaseTerminalSubtree(ActionFiringHandle handle) noexcept;
    bool PropagateTerminal(Row& row) noexcept;
    bool TryFinalizeSubtree(ActionFiringHandle handle) noexcept;
    bool TryFinalize(Row& row) noexcept;

    std::vector<Row> rows_;
    std::vector<ActionFiringStepResult> results_;
    std::uint32_t liveCount_ = 0;
    std::uint32_t highWater_ = 0;
    std::uint32_t quarantinedCount_ = 0;
    std::uint64_t refusalCount_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_ACTION_FIRING_TABLE_H
