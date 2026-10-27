#ifndef HARKEN_CORE_QUEUE_RUNTIME_H
#define HARKEN_CORE_QUEUE_RUNTIME_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/CurrentSelection.h"
#include "harken/core/DecisionToken.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/KnownCompletion.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/OccurrenceToken.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/TimelineArmTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/StableSlotStorage.h"

#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

namespace harken {
class OriginalImmediateLaneConditionalProjection;
class QueueContinuationLane;
class PrivatePendingQueueFrontierBinding;

class OccurrenceStatePool;
class CompositeCompletionOwnerPool;
class ScheduledTransactionPool;
class PrivateSelectedQueueFutureSpanPool;
class ImmediateActionLane;
class OriginalSourceSubmissionRegistry;
class PreparedQueueProjection;
struct ImmediateLaneBucket;
struct Engine;

inline constexpr std::uint32_t kMaxQueueInstancesHard = 16384;
inline constexpr std::uint32_t kMaxQueueCheckpointRowsHard = 65536;
inline constexpr std::uint32_t kMaxQueueAvoidHistoryHard = 64;

struct QueueInstanceToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

struct ConditionalQueueVersionToken {
    std::uint64_t version = 0;

    bool Valid() const noexcept { return version != 0; }
};

enum class ConditionalQueueActivation : std::uint8_t {
    Waiting = 0,
    Activated = 1,
    InvalidatedIdentity = 2,
    InvalidatedDeadline = 3,
    InvalidatedTransport = 4,
    InvalidatedProgress = 5,
    InvalidatedCapacity = 6,
};

enum class QueueFailureReason : std::uint8_t {
    RenderProgressLost = 0,
    OccurrenceLost = 1,
    CompletionOwnerLost = 2,
    Deadline = 3,
    TransportReset = 4,
    WorldReplaced = 5,
    ProgressCounterExhausted = 6,
    TargetPreparationFailed = 7,
    Capacity = 8,
};

enum class QueueFailurePhase : std::uint8_t {
    Healthy = 0,
    FailingSourceActive = 1,
    FailingAudibleRetiring = 2,
    FailedRetired = 3,
};

enum class QueueFailureDisposition : std::uint8_t {
    NewlyFailed = 0,
    AlreadyFailed = 1,
    Stale = 2,
    DeferredByFrontier = 3,
};

enum class QueueSequenceStepStatus : std::uint8_t {
    Successor = 0,
    NaturalEnd = 1,
    Invalid = 2,
};

enum class QueueConditionalProofStatus : std::uint8_t {
    ReadyToActivate = 0,
    Waiting = 1,
    FailedIdentity = 2,
    FailedProgress = 3,
    MissedDeadline = 4,
    Stale = 5,
    ReadyToActivateRetiredSource = 6,
};

enum class QueueConditionalCompletionMode : std::uint8_t {
    InQuantum = 0,
    RetiredSource = 1,
    LogicalBoundary = 2,
    SourceWait = 3,
};

struct QueueFrontierSourceWaitContext {
    const ImmediateActionLane* lane = nullptr;
    const ImmediateLaneBucket* bucket = nullptr;
    const DecisionTable* decisions = nullptr;
    std::uint32_t slot = UINT32_MAX;
};

struct QueueConditionalProof {
    QueueInstanceToken queue{};
    ConditionalQueueVersionToken version{};
    DecisionToken decision{};
    OccurrenceToken currentOccurrence{};
    OccurrenceToken successorOccurrence{};
    TimelineArmToken retiredSourceArm{};
    std::uint64_t retiredSourceRenderHandle = 0;
    std::uint64_t retiredSourceLogicalCompletionSequence = 0;
    std::uint64_t retiredSourceTombstoneAudioSequence = 0;
    KnownCompletionQuantumProof completion{};
    QueueConditionalCompletionMode completionMode =
        QueueConditionalCompletionMode::InQuantum;
};

struct EffectiveQueueState {
    std::uint64_t version = 0;
    std::uint32_t cursor = 0;
    std::uint32_t currentChildOrdinal = 0;
    std::uint64_t cycleOrdinal = 0;
    std::uint64_t selectionOrdinal = 0;
    std::uint32_t randomInitialRecentCount = 0;
    std::array<std::uint32_t, kMaxQueueAvoidHistoryHard>
        randomInitialRecent{};
};

// Borrowed from the semantic resolver for one serialized preparation scope.
// Plain queue states cannot manufacture a resolved Reset projection.
class ResolvedQueueResetProjection {
public:
    ResolvedQueueResetProjection(const ResolvedQueueResetProjection&) = delete;
    ResolvedQueueResetProjection& operator=(const ResolvedQueueResetProjection&) = delete;
private:
    friend class PreparedQueueProjection;
    friend class PrivateSelectedQueueFutureSpanPool;
    ResolvedQueueResetProjection() = default;
    QueueInstanceToken queue_{};
    WorldGeneration world_ = 0;
    PlayableInstanceHandle root_ = 0;
    std::uint64_t fingerprint_ = 0;
    EffectiveQueueState current_{};
    KnownCompletionPredicateV1 completion_{};
    const EffectiveQueueState* states_ = nullptr;
    std::uint32_t count_ = 0;
};

inline bool QueueSuccessorRestartsCycle(
    const EffectiveQueueState& current,
    const EffectiveQueueState& next) noexcept {
    return next.cycleOrdinal != current.cycleOrdinal && next.cursor == 0;
}

struct RandomQueueSeedContext {
    std::uint64_t worldSemanticFingerprint = 0;
    VnObjectId rootStableId{};
    std::uint64_t callerSeed = 0;
    std::uint64_t acceptedPostOrdinal = 0;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
    std::uint64_t resetEpoch = 0;
};

struct QueueCheckpointKey {
    WorldGeneration world = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t resetEpoch = 0;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
};

struct QueueCheckpointImage {
    EffectiveQueueState effective{};
    RandomQueueSeedContext randomSeed{};
    bool randomSeedValid = false;
    bool replayCurrent = false;
};

enum class QueueResetStatusV1 : std::uint8_t {
    Ok = 0,
    InvalidInput = 1,
    Backpressure = 2,
    NotFound = 3,
    RuntimeRecreateRequired = 4,
};

class QueueCheckpointTable;

class PreparedQueueCheckpointResetV1 {
public:
    PreparedQueueCheckpointResetV1() = default;
    PreparedQueueCheckpointResetV1(
        const PreparedQueueCheckpointResetV1&) = delete;
    PreparedQueueCheckpointResetV1& operator=(
        const PreparedQueueCheckpointResetV1&) = delete;
    PreparedQueueCheckpointResetV1(
        PreparedQueueCheckpointResetV1&& other) noexcept
        : owner_(other.owner_), slot_(other.slot_),
          prepared_(other.prepared_), idempotent_(other.idempotent_) {
        other.owner_ = nullptr;
        other.slot_ = UINT32_MAX;
        other.prepared_ = false;
        other.idempotent_ = false;
    }
    PreparedQueueCheckpointResetV1& operator=(
        PreparedQueueCheckpointResetV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            slot_ = other.slot_;
            prepared_ = other.prepared_;
            idempotent_ = other.idempotent_;
            other.owner_ = nullptr;
            other.slot_ = UINT32_MAX;
            other.prepared_ = false;
            other.idempotent_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    bool Idempotent() const noexcept { return Valid() && idempotent_; }

private:
    friend class QueueCheckpointTable;
    QueueCheckpointTable* owner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    bool prepared_ = false;
    bool idempotent_ = false;
};

class QueueCheckpointTable {
    struct Row;
public:
    explicit QueueCheckpointTable(std::uint32_t capacity);

    bool Valid() const noexcept { return capacity_ != 0; }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    using BackingPreparation = StableSlotStorage<Row>::Preparation;
    using BackingUse = StableSlotStorage<Row>::PreparedUse;
    bool PrepareBacking(std::uint32_t count, BackingPreparation* out) noexcept;
    std::uint32_t ConstructedRowCount() const noexcept { return rows_.ConstructedCount(); }
    std::size_t RowStorageBytes() const noexcept { return rows_.RequestedBytes(); }
    std::size_t RowCommittedBytes() const noexcept { return rows_.CommittedBytes(); }
    std::size_t RowDirectoryBytes() const noexcept { return rows_.DirectoryBytes(); }
    void SetRowCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }
    std::uint32_t LiveCount() const noexcept;
    bool Store(const QueueCheckpointKey& key,
               const QueueCheckpointImage& image) noexcept;
    bool Load(const QueueCheckpointKey& key,
              QueueCheckpointImage* out) const noexcept;
    bool Clear(const QueueCheckpointKey& key) noexcept;
    QueueResetStatusV1 PrepareReset(
        const QueueCheckpointKey& key,
        PreparedQueueCheckpointResetV1* out) noexcept;
    QueueResetStatusV1 CommitPreparedReset(
        PreparedQueueCheckpointResetV1* prepared,
        LogicCommitSequence sequence) noexcept;
    QueueResetStatusV1 CancelPreparedReset(
        PreparedQueueCheckpointResetV1* prepared) noexcept;
    void ClearWorld(WorldGeneration world) noexcept;
    void ClearAll() noexcept;

private:
    struct Row {
        bool live = false;
        bool resetReserved = false;
        QueueCheckpointKey key{};
        QueueCheckpointImage image{};
    };

    StableSlotStorage<Row> rows_;
    std::uint32_t capacity_ = 0;
};

struct QueueInstanceCreateRequest {
    VnObjectId projectId{};
    WorldGeneration world = 0;
    PlayableInstanceHandle rootInstance = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint32_t childCount = 0;
    QueueCycleV1 cycle = QueueCycleV1::Once;
    bool resumePosition = false;
    std::uint64_t resetEpoch = 0;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
    EffectiveQueueState initial{};
    EffectiveQueueState resetInitial{};
    MonitorLifecycleFacts monitorIdentity{};
};

class QueueInstanceTable;

class PreparedQueueInstanceV1 {
public:
    PreparedQueueInstanceV1() = default;
    PreparedQueueInstanceV1(const PreparedQueueInstanceV1&) = delete;
    PreparedQueueInstanceV1& operator=(
        const PreparedQueueInstanceV1&) = delete;
    PreparedQueueInstanceV1(PreparedQueueInstanceV1&& other) noexcept
        : owner_(other.owner_), token_(other.token_),
          prepared_(other.prepared_) {
        other.owner_ = nullptr;
        other.token_ = {};
        other.prepared_ = false;
    }
    PreparedQueueInstanceV1& operator=(
        PreparedQueueInstanceV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            token_ = other.token_;
            prepared_ = other.prepared_;
            other.owner_ = nullptr;
            other.token_ = {};
            other.prepared_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    QueueInstanceToken Token() const noexcept {
        return Valid() ? token_ : QueueInstanceToken{};
    }

private:
    friend class QueueInstanceTable;
    QueueInstanceTable* owner_ = nullptr;
    QueueInstanceToken token_{};
    bool prepared_ = false;
};

struct RetiredQueueSourceProof {
    TimelineArmToken sourceArm{};
    std::uint64_t sourceRenderHandle = 0;
    std::uint64_t logicalCompletionSequence = 0;
    std::uint64_t tombstoneAudioSequence = 0;
};

struct ConditionalQueuePrepareRequest {
    std::uint64_t expectedCurrentVersion = 0;
    DecisionToken rootDecision{};
    OccurrenceToken expectedCurrentOccurrence{};
    OccurrenceToken successorOccurrence{};
    KnownCompletionPredicateV1 completion{};
    CompletionOwnerRef completionOwner{};
    bool completionIsLogicalBoundary = false;
    bool completionHasLogicalFloor = false;
    std::uint64_t logicalCompletionFrame = 0;
    EffectiveQueueState next{};
    RetiredQueueSourceProof retiredSource{};
    const EffectiveQueueState* projectedStates = nullptr;
    std::uint32_t projectedStateCount = 0;
};

class PreparedQueueConditionalV1 {
public:
    PreparedQueueConditionalV1() = default;
    PreparedQueueConditionalV1(
        const PreparedQueueConditionalV1&) = delete;
    PreparedQueueConditionalV1& operator=(
        const PreparedQueueConditionalV1&) = delete;
    PreparedQueueConditionalV1(
        PreparedQueueConditionalV1&& other) noexcept
        : owner_(other.owner_), queue_(other.queue_),
          version_(other.version_), prepared_(other.prepared_),
          originalRow_(std::exchange(other.originalRow_, UINT32_MAX)),
          originalMarker_(std::exchange(other.originalMarker_, 0)) {
        other.owner_ = nullptr;
        other.queue_ = {};
        other.version_ = {};
        other.prepared_ = false;
    }
    PreparedQueueConditionalV1& operator=(
        PreparedQueueConditionalV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            queue_ = other.queue_;
            version_ = other.version_;
            prepared_ = other.prepared_;
            originalRow_ = std::exchange(other.originalRow_, UINT32_MAX);
            originalMarker_ = std::exchange(other.originalMarker_, 0);
            other.owner_ = nullptr;
            other.queue_ = {};
            other.version_ = {};
            other.prepared_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    ConditionalQueueVersionToken Version() const noexcept {
        return Valid() ? version_ : ConditionalQueueVersionToken{};
    }

private:
    friend class QueueInstanceTable;
    QueueInstanceTable* owner_ = nullptr;
    QueueInstanceToken queue_{};
    ConditionalQueueVersionToken version_{};
    bool prepared_ = false;
    std::uint32_t originalRow_ = UINT32_MAX;
    std::uint64_t originalMarker_ = 0;
};

struct QueueInstanceSnapshot {
    WorldGeneration world = 0;
    PlayableInstanceHandle rootInstance = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint32_t childCount = 0;
    QueueCycleV1 cycle = QueueCycleV1::Once;
    bool resumePosition = false;
    std::uint64_t resetEpoch = 0;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
    EffectiveQueueState logicMirror{};
    bool resetPending = false;
    ConditionalQueueActivation pendingActivation =
        ConditionalQueueActivation::Waiting;
    std::uint64_t pendingVersion = 0;
    std::uint64_t initialActivationSequence = 0;
    QueueFailurePhase failurePhase = QueueFailurePhase::Healthy;
    QueueFailureReason failureReason = QueueFailureReason::RenderProgressLost;
    std::uint64_t failureFrame = 0;
};

struct QueueResetTargetV1 {
    WorldGeneration world = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
};

struct QueueExecutionOrderV1 {
    std::uint64_t executeFrame = 0;
    LogicCommitSequence sequence = 0;
    std::uint32_t executionOrdinal = 0;
};

// Logic-owned observation; it never participates in Queue selection or proof.
struct MonitorQueueResetObservation {
    enum class Stage : std::uint8_t { None, Intent, Selected, Applied };
    MonitorControlCause cause{};
    QueueExecutionOrderV1 order{};
    std::uint64_t resetIntent = 0;
    std::uint64_t previousVersion = 0;
    std::uint64_t nextVersion = 0;
    Stage stage = Stage::None;
};

inline bool QueueExecutionPrecedes(QueueExecutionOrderV1 left,
                                  QueueExecutionOrderV1 right) noexcept {
    if (left.executeFrame != right.executeFrame)
        return left.executeFrame < right.executeFrame;
    if (left.sequence != right.sequence) return left.sequence < right.sequence;
    return left.executionOrdinal < right.executionOrdinal;
}

enum class QueueSignalStatusV1 : std::uint8_t {
    Ok,
    InvalidInput,
    Backpressure,
    Stale,
    RuntimeRecreateRequired,
};

enum class QueueSignalOutcomeV1 : std::uint8_t {
    InFlight,
    Applied,
    DefinitivelyNotApplied,
};

struct AcceptedQueueSignalAdvanceV1 {
    QueueInstanceToken queue{};
    std::uint64_t attempt = 0;
    std::uint64_t resetIntent = 0;
    std::uint64_t mirrorSequence = 0;
    QueueExecutionOrderV1 order{};

    bool Valid() const noexcept { return queue.Valid() && attempt != 0; }
};

class PreparedQueueSignalAdvanceV1 {
public:
    PreparedQueueSignalAdvanceV1() = default;
    PreparedQueueSignalAdvanceV1(const PreparedQueueSignalAdvanceV1&) = delete;
    PreparedQueueSignalAdvanceV1& operator=(const PreparedQueueSignalAdvanceV1&) = delete;
    PreparedQueueSignalAdvanceV1(PreparedQueueSignalAdvanceV1&& other) noexcept
        : owner_(other.owner_), identity_(other.identity_),
          current_(other.current_), next_(other.next_) { other.owner_ = nullptr; }
    PreparedQueueSignalAdvanceV1& operator=(PreparedQueueSignalAdvanceV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            identity_ = other.identity_;
            current_ = other.current_;
            next_ = other.next_;
            other.owner_ = nullptr;
        }
        return *this;
    }
    bool Valid() const noexcept { return owner_ != nullptr && identity_.Valid(); }

private:
    friend class QueueInstanceTable;
    QueueInstanceTable* owner_ = nullptr;
    AcceptedQueueSignalAdvanceV1 identity_{};
    EffectiveQueueState current_{};
    EffectiveQueueState next_{};
};

class QueueInstanceTable;

class PreparedQueueResetSetV1 {
public:
    PreparedQueueResetSetV1() = default;
    PreparedQueueResetSetV1(const PreparedQueueResetSetV1&) = delete;
    PreparedQueueResetSetV1& operator=(
        const PreparedQueueResetSetV1&) = delete;
    PreparedQueueResetSetV1(PreparedQueueResetSetV1&& other) noexcept
        : owner_(other.owner_), target_(other.target_),
          matchedCount_(other.matchedCount_),
          reservedCount_(other.reservedCount_), prepared_(other.prepared_),
          reservation_(other.reservation_), order_(other.order_),
          nativeScopeMarker_(std::exchange(other.nativeScopeMarker_, 0)) {
        other.owner_ = nullptr;
        other.matchedCount_ = 0;
        other.reservedCount_ = 0;
        other.prepared_ = false;
    }
    PreparedQueueResetSetV1& operator=(
        PreparedQueueResetSetV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            target_ = other.target_;
            matchedCount_ = other.matchedCount_;
            reservedCount_ = other.reservedCount_;
            prepared_ = other.prepared_;
            reservation_ = other.reservation_;
            order_ = other.order_;
            nativeScopeMarker_ = std::exchange(other.nativeScopeMarker_, 0);
            other.owner_ = nullptr;
            other.matchedCount_ = 0;
            other.reservedCount_ = 0;
            other.prepared_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    bool Idempotent() const noexcept {
        return Valid() && reservedCount_ == 0;
    }
    std::uint32_t MatchedCount() const noexcept {
        return Valid() ? matchedCount_ : 0;
    }

private:
    friend class QueueInstanceTable;
    QueueInstanceTable* owner_ = nullptr;
    QueueResetTargetV1 target_{};
    std::uint32_t matchedCount_ = 0;
    std::uint32_t reservedCount_ = 0;
    bool prepared_ = false;
    std::uint64_t reservation_ = 0;
    QueueExecutionOrderV1 order_{};
    std::uint64_t nativeScopeMarker_ = 0;
};

class PreparedQueueConditionalReplacementV1 {
public:
    PreparedQueueConditionalReplacementV1() = default;
    PreparedQueueConditionalReplacementV1(
        const PreparedQueueConditionalReplacementV1&) = delete;
    PreparedQueueConditionalReplacementV1& operator=(
        const PreparedQueueConditionalReplacementV1&) = delete;
    PreparedQueueConditionalReplacementV1(
        PreparedQueueConditionalReplacementV1&& other) noexcept
        : owner_(other.owner_), queue_(other.queue_),
          expectedVersion_(other.expectedVersion_),
          expectedDecision_(other.expectedDecision_),
          replacement_(other.replacement_), projectionFirst_(other.projectionFirst_),
          projectionCount_(other.projectionCount_), prepared_(other.prepared_),
          stagedOwner_(std::exchange(other.stagedOwner_, nullptr)),
          currentOccurrence_(std::move(other.currentOccurrence_)),
          currentDecision_(std::move(other.currentDecision_)) {
        other.owner_ = nullptr;
        other.queue_ = {};
        other.expectedVersion_ = {};
        other.expectedDecision_ = {};
        other.replacement_ = {};
        other.prepared_ = false;
    }
    PreparedQueueConditionalReplacementV1& operator=(
        PreparedQueueConditionalReplacementV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            queue_ = other.queue_;
            expectedVersion_ = other.expectedVersion_;
            expectedDecision_ = other.expectedDecision_;
            currentOccurrence_ = std::move(other.currentOccurrence_);
            currentDecision_ = std::move(other.currentDecision_);
            replacement_ = other.replacement_;
            projectionFirst_ = other.projectionFirst_;
            projectionCount_ = other.projectionCount_;
            prepared_ = other.prepared_;
            stagedOwner_ = std::exchange(other.stagedOwner_, nullptr);
            other.owner_ = nullptr;
            other.queue_ = {};
            other.expectedVersion_ = {};
            other.expectedDecision_ = {};
            other.replacement_ = {};
            other.prepared_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }

private:
    friend class QueueInstanceTable;
    friend class QueueContinuationLane;
    friend class OriginalConditionalReplacementBatch;
    QueueInstanceTable* owner_ = nullptr;
    QueueInstanceToken queue_{};
    ConditionalQueueVersionToken expectedVersion_{};
    DecisionToken expectedDecision_{};
    ConditionalQueuePrepareRequest replacement_{};
    EffectiveQueueState projectionFirst_{};
    std::uint32_t projectionCount_ = 0;
    bool prepared_ = false;
    const QueueContinuationLane* stagedOwner_ = nullptr;
    OccurrenceStatePool::OriginalSingleCompletionOwnerPin currentOccurrence_{};
    OriginalDecisionLifetimePin currentDecision_{};
};

QueueSequenceStepStatus ResolveSequenceQueueStep(
    const ContainerProgramViewV3& program,
    const EffectiveQueueState& current,
    EffectiveQueueState* outNext) noexcept;

SelectionStatusV1 BuildRandomQueuePermutationV1(
    const ContainerProgramViewV3& program,
    const RandomQueueSeedContext& seed,
    const EffectiveQueueState& state,
    SelectionChildV1* childScratch,
    std::uint32_t* remainingScratch,
    std::uint32_t* permutation,
    std::uint32_t capacity) noexcept;

QueueSequenceStepStatus ResolveRandomQueueStep(
    const ContainerProgramViewV3& program,
    const RandomQueueSeedContext& seed,
    const EffectiveQueueState& current,
    SelectionChildV1* childScratch,
    std::uint32_t* remainingScratch,
    std::uint32_t* permutation,
    std::uint32_t capacity,
    EffectiveQueueState* outNext) noexcept;

class QueueInstanceTable {
    struct Row;
    struct OriginalConditionalRow;
public:
    struct BackingPreparation {
        StableSlotStorage<Row>::Preparation instances;
        StableSlotStorage<OriginalConditionalRow>::Preparation conditionals;
    };
    class BackingUse {
    public:
        explicit BackingUse(const BackingPreparation* backing = nullptr) noexcept
            : instances_(backing == nullptr ? StableSlotStorage<Row>::PreparedUse{}
                  : backing->instances.Use()),
              conditionals_(backing == nullptr ? StableSlotStorage<OriginalConditionalRow>::PreparedUse{}
                  : backing->conditionals.Use()) {}
    private:
        StableSlotStorage<Row>::PreparedUse instances_;
        StableSlotStorage<OriginalConditionalRow>::PreparedUse conditionals_;
    };
    bool PrepareBacking(std::uint32_t instances, std::uint32_t conditionals,
        BackingPreparation* out) noexcept;
    bool AttachRowStoragePreparation(void* context,
        bool (*prepare)(void*, std::uint32_t) noexcept) noexcept;
    std::uint32_t ConstructedRowCount() const noexcept { return rows_.ConstructedCount(); }
    std::uint32_t ConstructedConditionalRowCount() const noexcept {
        return originalConditionalRows_.ConstructedCount();
    }
    std::size_t RowStorageBytes() const noexcept { return rows_.RequestedBytes(); }
    std::size_t ConditionalRowStorageBytes() const noexcept { return originalConditionalRows_.RequestedBytes(); }
    std::size_t RowCommittedBytes() const noexcept { return rows_.CommittedBytes(); }
    std::size_t ConditionalRowCommittedBytes() const noexcept { return originalConditionalRows_.CommittedBytes(); }
    std::size_t RowDirectoryBytes() const noexcept { return rows_.DirectoryBytes(); }
    std::size_t ConditionalRowDirectoryBytes() const noexcept { return originalConditionalRows_.DirectoryBytes(); }
    void SetRowCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }
    void SetConditionalRowCommitPageLimitForTests(std::size_t pages) noexcept {
        originalConditionalRows_.SetCommitPageLimitForTests(pages);
    }
    // Logic-producer storage ownership issued only by the actual row factory.
    // It preserves Initializing/Live storage without freezing queue progress.
    class OriginalLifetimePin {
    public:
        OriginalLifetimePin() noexcept = default;
        OriginalLifetimePin(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin& operator=(const OriginalLifetimePin&) = delete;
        OriginalLifetimePin(OriginalLifetimePin&& other) noexcept { *this = std::move(other); }
        OriginalLifetimePin& operator=(OriginalLifetimePin&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                token_ = std::exchange(other.token_, QueueInstanceToken{});
                marker_ = std::exchange(other.marker_, 0);
            }
            return *this;
        }
        ~OriginalLifetimePin() { Release(); }
        bool Valid() const noexcept;
        QueueInstanceToken Token() const noexcept { return Valid() ? token_ : QueueInstanceToken{}; }
        bool Borrow(OriginalLifetimePin* out) const noexcept;
        void Release() noexcept;
    private:
        friend class QueueInstanceTable;
        QueueInstanceTable* owner_ = nullptr;
        QueueInstanceToken token_{};
        std::uint64_t marker_ = 0;
    };

    // This owns one real original conditional record and its acquired Queue
    // parent. It does not grant Current/Source, mutation, Claim or cutover rights.
    class OriginalConditionalLaneClock;
    class OriginalConditionalFutureReservation {
    public:
        OriginalConditionalFutureReservation() noexcept = default;
        OriginalConditionalFutureReservation(const OriginalConditionalFutureReservation&) = delete;
        OriginalConditionalFutureReservation& operator=(const OriginalConditionalFutureReservation&) = delete;
        OriginalConditionalFutureReservation(OriginalConditionalFutureReservation&& other) noexcept {
            *this = std::move(other);
        }
        OriginalConditionalFutureReservation& operator=(OriginalConditionalFutureReservation&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                row_ = std::exchange(other.row_, UINT32_MAX);
                generation_ = std::exchange(other.generation_, 0);
                marker_ = std::exchange(other.marker_, 0);
                queue_ = std::exchange(other.queue_, QueueInstanceToken{});
                version_ = std::exchange(other.version_, ConditionalQueueVersionToken{});
            }
            return *this;
        }
        ~OriginalConditionalFutureReservation() { Release(); }
        bool Valid() const noexcept;
        QueueInstanceToken Token() const noexcept { return Valid() ? queue_ : QueueInstanceToken{}; }
        ConditionalQueueVersionToken Version() const noexcept {
            return Valid() ? version_ : ConditionalQueueVersionToken{};
        }
        bool Borrow(OriginalConditionalFutureReservation* out) const noexcept;
        void Release() noexcept;
    private:
        friend class QueueInstanceTable;
        friend class OriginalConditionalLaneClock;
        friend class PrivateSelectedQueueFutureSpanPool;
        friend class OriginalImmediateLaneConditionalProjection;
        QueueInstanceTable* owner_ = nullptr;
        std::uint32_t row_ = UINT32_MAX;
        std::uint32_t generation_ = 0;
        std::uint64_t marker_ = 0;
        QueueInstanceToken queue_{};
        ConditionalQueueVersionToken version_{};
    };

    enum class OriginalConditionalLaneClockState : std::uint8_t { Unbound, Bound, Retired };
    // Historical native binding observation, never a Current/Ready/Reset proof.
    struct OriginalConditionalInitialClockBinding {
        std::uint64_t anchorFrame = 0;
        std::uint64_t initialGroupFrame = 0;
        std::uint64_t conditionalGroupFrame = 0;
        std::uint64_t sourceStartFrame = 0;
        std::uint64_t installedAckSequence = 0;
        TimelineArmToken sourceArm{};
    };
    class OriginalConditionalLaneClock {
    public:
        OriginalConditionalLaneClock() noexcept = default;
        OriginalConditionalLaneClock(const OriginalConditionalLaneClock&) = delete;
        OriginalConditionalLaneClock& operator=(const OriginalConditionalLaneClock&) = delete;
        OriginalConditionalLaneClock(OriginalConditionalLaneClock&&) noexcept = default;
        OriginalConditionalLaneClock& operator=(OriginalConditionalLaneClock&&) noexcept = default;
        ~OriginalConditionalLaneClock() = default;
        bool Valid() const noexcept;
        bool Borrow(OriginalConditionalLaneClock* out) const noexcept;
        bool CaptureState(OriginalConditionalLaneClockState* out) const noexcept;
        bool CaptureInitialBinding(OriginalConditionalInitialClockBinding* out) const noexcept;
        void Release() noexcept { original_.Release(); }
    private:
        friend class QueueInstanceTable;
        OriginalConditionalFutureReservation original_{};
    };

    bool PrepareOriginalConditionalLaneClock(
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parentOriginal, ScheduledTransactionPool& pool,
        const OriginalQueueFutureGroupRegistration& originalC0,
        const OriginalQueueFutureGroupRegistration& originalC1,
        OriginalConditionalLaneClock* out) noexcept;
    bool IsOriginalConditionalLaneClockFor(
        const OriginalConditionalLaneClock& clock,
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parentOriginal) const noexcept;

    bool IsOriginalConditionalFutureReservationFor(
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parentOriginal, QueueInstanceToken queue,
        ConditionalQueueVersionToken version, DecisionToken decision,
        OccurrenceToken current) const noexcept;
    bool IsSelectedOriginalConditionalFutureReservationFor(
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parentOriginal, QueueInstanceToken queue,
        ConditionalQueueVersionToken version, DecisionToken decision,
        OccurrenceToken current) const noexcept;
    // This observes the actual current association. It is not a publication,
    // completion or audio-claim proof, and is logic-producer only.
    bool IsCurrentOriginalConditionalFutureReservation(
        const OriginalConditionalFutureReservation& original) const noexcept;

    bool IsOriginalLifetimePinFor(const OriginalLifetimePin& original,
        QueueInstanceToken expected) const noexcept;

    // A genuine matched-row lifetime scope, not a reset publication or a stable
    // projection. Old audio progress remains eligible while this scope exists.
    class PreparedCatalogResetScope {
    public:
        PreparedCatalogResetScope() = default;
        PreparedCatalogResetScope(const PreparedCatalogResetScope&) = delete;
        PreparedCatalogResetScope& operator=(const PreparedCatalogResetScope&) = delete;
        PreparedCatalogResetScope(PreparedCatalogResetScope&& other) noexcept { *this = std::move(other); }
        PreparedCatalogResetScope& operator=(PreparedCatalogResetScope&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = std::exchange(other.owner_, nullptr);
                marker_ = std::exchange(other.marker_, 0);
                matchedCount_ = std::exchange(other.matchedCount_, 0);
                nativeOnly_ = std::exchange(other.nativeOnly_, false);
            }
            return *this;
        }
        ~PreparedCatalogResetScope() { Release(); }
        bool Valid() const noexcept;
        std::uint32_t MatchedCount() const noexcept { return Valid() ? matchedCount_ : 0; }
        void Release() noexcept;
    private:
        friend class QueueInstanceTable;
        friend class PrivateSelectedQueueFutureSpanPool;
        QueueInstanceTable* owner_ = nullptr;
        std::uint64_t marker_ = 0;
        std::uint32_t matchedCount_ = 0;
        bool nativeOnly_ = false;
    };

    QueueResetStatusV1 PrepareCatalogResetScope(const QueueResetTargetV1& target,
        PreparedCatalogResetScope* out) noexcept;
    // Select only the actual retained native futures. The ordinary remainder
    // still prepares under its original Reset rules and must join the same commit.
    QueueResetStatusV1 PrepareNativeConditionalCatalogResetScope(const QueueResetTargetV1& target,
        PreparedCatalogResetScope* out) noexcept;
    QueueResetStatusV1 ExtendCatalogResetScope(PreparedCatalogResetScope& original,
        const QueueResetTargetV1& target) noexcept;
    // Returns current observations from the original pinned rows. This does
    // not issue any future, projection, mutation or acceptance capability.
    bool CaptureCatalogResetScopeQueue(const PreparedCatalogResetScope& original,
        std::uint32_t index, QueueInstanceToken* outToken,
        QueueInstanceSnapshot* outSnapshot) const noexcept;
    class PrivateConditionalTimingProof {
    public:
        bool Valid() const noexcept { return owner_ != nullptr; }
    private:
        friend class QueueInstanceTable;
        QueueInstanceTable* owner_ = nullptr;
        void* row_ = nullptr;
        std::uint32_t slot_ = UINT32_MAX;
        KnownCompletionPredicateV1 completion_{};
        std::uint64_t logicalCompletionFrame_ = 0;
        bool updatesCompletion_ = false;
        void* retainedOriginal_ = nullptr;
        std::uint64_t retainedGeneration_ = 0;
    };

    class FrontierInitialProof {
    public:
        bool Valid() const noexcept { return row_ != nullptr; }
    private:
        friend class QueueInstanceTable;
        void* row_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
        std::uint64_t audioSequence_ = 0;
    };

    bool BindFrontierInitial(QueueInstanceToken token, const FrontierGroupBinding& binding,
        const ScheduledTransactionPool& pool) noexcept;
    bool ProveFrontierInitial(const FrontierGroupBinding& binding, AudioSliceLease& lease,
        std::uint64_t audioSequence, FrontierInitialProof* out) noexcept;
    void ApplyFrontierInitial(const FrontierInitialProof& proof) noexcept;

    class FrontierInitialCompletionProof {
    public:
        FrontierInitialCompletionProof() = default;
        ~FrontierInitialCompletionProof() { Release(); }
        FrontierInitialCompletionProof(const FrontierInitialCompletionProof&) = delete;
        FrontierInitialCompletionProof& operator=(const FrontierInitialCompletionProof&) = delete;
        bool Valid() const noexcept { return pool_ != nullptr && claim_.Valid(); }
        void Release() noexcept;
    private:
        friend class QueueInstanceTable;
        const ScheduledTransactionPool* pool_ = nullptr;
        ImmediateLaneFrontierClaim claim_{};
        void* row_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
    };
    bool ProveFrontierInitialCompletion(const FrontierGroupBinding& binding,
        const Plan03PageGroupView& source, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, std::uint64_t sourceSequence,
        const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool* composites,
        std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
        std::uint64_t completedAudioSequence, FrontierInitialCompletionProof* out,
        const QueueFrontierSourceWaitContext* sourceWait = nullptr,
        QueueConditionalProof* outCompletion = nullptr) noexcept;
    void ApplyFrontierInitialCompletion(const FrontierInitialCompletionProof& proof) noexcept;

    enum class FrontierConditionalSlice : std::uint8_t { Activation, Prelude, TargetLock };
    class FrontierConditionalProof {
    public:
        FrontierConditionalProof() = default;
        ~FrontierConditionalProof();
        FrontierConditionalProof(const FrontierConditionalProof&) = delete;
        FrontierConditionalProof& operator=(const FrontierConditionalProof&) = delete;
        bool Valid() const noexcept { return row_ != nullptr; }
        void Release() noexcept;
    private:
        friend class QueueInstanceTable;
        void* row_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
        FrontierConditionalSlice slice_ = FrontierConditionalSlice::Activation;
    };

    bool BindFrontierConditional(QueueInstanceToken token, const FrontierGroupBinding& binding,
        const ScheduledTransactionPool& pool) noexcept;
    bool CapturePrivatePendingFrontierBinding(const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parent, const FrontierGroupBinding& binding,
        const ScheduledTransactionPool& pool, const TransitionInstancePool& transitions,
        PrivatePendingQueueFrontierBinding* out,
        const FrontierGroupBinding* counterpart = nullptr,
        const CompositeCompletionOwnerPool* composites = nullptr) const noexcept;
    bool ProveFrontierConditional(const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool* composites,
        std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame, std::uint64_t completedAudioSequence,
        FrontierConditionalProof* out, FrontierConditionalSlice slice = FrontierConditionalSlice::Activation,
        const QueueFrontierSourceWaitContext* sourceWait = nullptr,
        QueueConditionalProof* outCompletion = nullptr,
        const FrontierInitialProof* stagedInitial = nullptr) noexcept;
    void ApplyFrontierConditional(const FrontierConditionalProof& proof) noexcept;
    QueueConditionalProofStatus PreflightFrontierSourceWait(
        const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const QueueFrontierSourceWaitContext& context,
        const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool* composites,
        std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
        std::uint64_t completedAudioSequence, QueueConditionalProof* outProof) const noexcept;

    explicit QueueInstanceTable(std::uint32_t capacity,
        std::uint32_t originalConditionalCapacity = 0);
    QueueInstanceTable(const QueueInstanceTable&) = delete;
    QueueInstanceTable& operator=(const QueueInstanceTable&) = delete;
    QueueInstanceTable(QueueInstanceTable&&) = delete;
    QueueInstanceTable& operator=(QueueInstanceTable&&) = delete;
    ~QueueInstanceTable();

    bool Valid() const noexcept;
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t LiveCount() const noexcept;
    std::uint32_t QuarantinedCount() const noexcept;

    bool Acquire(const QueueInstanceCreateRequest& request,
                 QueueInstanceToken* outToken,
                 OriginalLifetimePin* originalLifetime = nullptr) noexcept;
    bool PrepareAcquire(const QueueInstanceCreateRequest& request,
                        PreparedQueueInstanceV1* out,
                        OriginalLifetimePin* originalLifetime = nullptr) noexcept;
    bool CommitPreparedAcquire(PreparedQueueInstanceV1* prepared,
                               QueueInstanceToken* outToken) noexcept;
    bool CancelPreparedAcquire(PreparedQueueInstanceV1* prepared,
        OriginalLifetimePin* originalBaseline = nullptr) noexcept;
    bool Release(QueueInstanceToken token,
        OriginalLifetimePin* originalBaseline = nullptr) noexcept;
    bool Capture(QueueInstanceToken token,
                 QueueInstanceSnapshot* out) const noexcept;
    bool CaptureMonitorReset(QueueInstanceToken token,
        MonitorQueueResetObservation* out) const noexcept;
    void RecordMonitorResetSelection(QueueInstanceToken token, QueueExecutionOrderV1 order,
        const MonitorControlCause& cause) noexcept;
    bool ProvePendingConditionalIdentity(QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        DecisionToken decision) const noexcept;
    bool ResolveEffective(QueueInstanceToken token,
                          EffectiveQueueState* out) const noexcept;
    bool ResolveNextSelection(QueueInstanceToken token,
                              EffectiveQueueState* outCurrent,
                              EffectiveQueueState* outNext,
                              bool* outReset, MonitorQueueResetObservation* observation = nullptr) const noexcept;
    bool ResolveNextSelection(QueueInstanceToken token,
                              QueueExecutionOrderV1 selectionOrder,
                              EffectiveQueueState* outCurrent,
                              EffectiveQueueState* outNext,
                              std::uint64_t* outResetIntent, MonitorQueueResetObservation* observation = nullptr) const noexcept;
    bool ResolveResetSelection(
        QueueInstanceToken token,
        EffectiveQueueState* outCurrent,
        EffectiveQueueState* outNext) const noexcept;
    QueueResetStatusV1 PrepareReset(
        const QueueResetTargetV1& target,
        PreparedQueueResetSetV1* out,
        std::uint64_t executeFrame = 0,
        std::uint32_t executionOrdinal = 0) noexcept;
    QueueResetStatusV1 PrepareResetRemainder(const QueueResetTargetV1& target,
        const PreparedCatalogResetScope& nativeScope, PreparedQueueResetSetV1* out,
        std::uint64_t executeFrame = 0, std::uint32_t executionOrdinal = 0) noexcept;
    QueueResetStatusV1 CommitPreparedReset(
        PreparedQueueResetSetV1* prepared,
        LogicCommitSequence sequence,
        std::uint64_t executeFrame = UINT64_MAX,
        const MonitorControlCause& cause = {}) noexcept;
    void ApplyDue(std::uint64_t now) noexcept;
    QueueResetStatusV1 CancelPreparedReset(
        PreparedQueueResetSetV1* prepared) noexcept;
    bool PrepareConditional(
        QueueInstanceToken token,
        const ConditionalQueuePrepareRequest& request,
        ConditionalQueueVersionToken* outVersion) noexcept;
    bool PrepareConditional(
        QueueInstanceToken token,
        const ConditionalQueuePrepareRequest& request,
        PreparedQueueConditionalV1* out) noexcept;
    // The extra original is issued only with the actual acquired parent.
    // Failure leaves both outputs unchanged; the ordinary overload is intact.
    bool PrepareConditional(
        QueueInstanceToken token,
        const ConditionalQueuePrepareRequest& request,
        PreparedQueueConditionalV1* out,
        const OriginalLifetimePin& parentOriginal,
        OriginalConditionalFutureReservation* outOriginal) noexcept;
    class ContinuationSourceBinding {
    public:
        ContinuationSourceBinding() noexcept = default;
        ContinuationSourceBinding(const ContinuationSourceBinding&) = delete;
        ContinuationSourceBinding& operator=(const ContinuationSourceBinding&) = delete;
        bool Valid() const noexcept { return validate_ != nullptr && validate_(*this); }
    private:
        friend class QueueInstanceTable;
        friend class OriginalSourceSubmissionRegistry;
        const void* issuer_ = nullptr;
        const void* source_ = nullptr;
        const TimelineArmTable* arms_ = nullptr;
        const OccurrenceStatePool* occurrences_ = nullptr;
        TimelineArmToken arm_{};
        OccurrenceToken occurrence_{};
        bool (*validate_)(const ContinuationSourceBinding&) noexcept = nullptr;
    };
    // A following node retains its actual predecessor and source-arm identity.
    bool PrepareFollowingConditional(
        QueueInstanceToken token, const ConditionalQueuePrepareRequest& request,
        const OriginalConditionalFutureReservation& predecessor,
        const ContinuationSourceBinding& source,
        PreparedQueueConditionalV1* out, const OriginalLifetimePin& parentOriginal,
        OriginalConditionalFutureReservation* outOriginal) noexcept;
    bool ResolveConditionalVersion(QueueInstanceToken token, DecisionToken decision,
        ConditionalQueueVersionToken* out) const noexcept;
    // Attach before callbacks start. Audio acknowledges only after its final Queue read.
    void EnableConditionalReadReclamation() noexcept { conditionalReadReclamationEnabled_ = true; }
    void CompleteAudioConditionalReads() noexcept;
    void CollectRetiredConditionalReads() noexcept;
#if !defined(HARKEN_RT_QUALIFICATION)
    using ConditionalHeadReadHookForTest = void (*)(void*) noexcept;
    void SetConditionalHeadReadHookForTest(ConditionalHeadReadHookForTest hook, void* context) noexcept {
        conditionalHeadReadHookForTest_ = hook;
        conditionalHeadReadContextForTest_ = context;
    }
#endif
    QueueConditionalProofStatus CaptureConditionalSourceDelay(QueueInstanceToken token,
        ConditionalQueueVersionToken version, std::uint64_t* delay) const noexcept;
    bool ConditionalSourceReady(QueueInstanceToken token,
        ConditionalQueueVersionToken version) const noexcept;
    bool CommitPreparedConditional(
        PreparedQueueConditionalV1* prepared,
        ConditionalQueueVersionToken* outVersion) noexcept;
    // The caller keeps the conditional unreachable by audio until Apply.
    bool ProvePrivateConditionalTimingBeforePublication(
        QueueInstanceToken queue,
        ConditionalQueueVersionToken version,
        DecisionToken decision,
        OccurrenceToken currentOccurrence,
        std::uint64_t newLogicalStartFrame,
        std::uint64_t newLogicalCompletionFrame,
        PrivateConditionalTimingProof* out) noexcept;
    bool ProveRetainedOriginalConditionalTimingBeforePublication(
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parent, QueueInstanceToken queue,
        ConditionalQueueVersionToken version, DecisionToken decision,
        OccurrenceToken currentOccurrence, std::uint64_t newLogicalStartFrame,
        std::uint64_t newLogicalCompletionFrame, PrivateConditionalTimingProof* out) noexcept;
    void ApplyPrivateConditionalTimingBeforePublication(
        const PrivateConditionalTimingProof& proof) noexcept;
    bool CancelPreparedConditional(
        PreparedQueueConditionalV1* prepared) noexcept;
    QueueResetStatusV1 PrepareConditionalReplacement(
        QueueInstanceToken token,
        ConditionalQueueVersionToken expectedVersion,
        DecisionToken expectedDecision,
        const ConditionalQueuePrepareRequest& replacement,
        PreparedQueueConditionalReplacementV1* out,
        const QueueContinuationLane* stagedOwner = nullptr) noexcept;
    // Retain the actual immutable Current while replacing its separate future.
    QueueResetStatusV1 PrepareConditionalReplacementRetainingOriginalCurrent(
        QueueInstanceToken token, ConditionalQueueVersionToken expectedVersion,
        DecisionToken expectedDecision, const ConditionalQueuePrepareRequest& replacement,
        OccurrenceStatePool& occurrences,
        const OccurrenceStatePool::OriginalSingleCompletionOwnerPin& current,
        DecisionTable& decisions, const OriginalDecisionLifetimePin& currentDecision,
        PreparedQueueConditionalReplacementV1* out,
        const QueueContinuationLane* stagedOwner = nullptr) noexcept;
    QueueResetStatusV1 CommitPreparedConditionalReplacement(
        PreparedQueueConditionalReplacementV1* prepared,
        LogicCommitSequence sequence) noexcept;
    QueueResetStatusV1 CancelPreparedConditionalReplacement(
        PreparedQueueConditionalReplacementV1* prepared) noexcept;
    bool ActivateConditional(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        DecisionToken expectedDecision,
        OccurrenceToken expectedCurrentOccurrence) noexcept;
    bool SettleActivated(QueueInstanceToken token,
                         ConditionalQueueVersionToken version,
                         std::uint64_t completedAudioSequence = 0) noexcept;
    bool AdvanceAfterSignalCompletion(
        QueueInstanceToken token,
        std::uint64_t expectedCurrentVersion,
        const EffectiveQueueState& next) noexcept;
    QueueSignalStatusV1 PrepareSignalAdvance(
        QueueInstanceToken token, const EffectiveQueueState& current,
        const EffectiveQueueState& next, QueueExecutionOrderV1 order,
        std::uint64_t resetIntent, PreparedQueueSignalAdvanceV1* out) noexcept;
    bool ValidatePreparedSignalAdvance(const PreparedQueueSignalAdvanceV1& prepared) const noexcept;
    bool CommitPreparedSignalAdvance(PreparedQueueSignalAdvanceV1* prepared) noexcept;
    bool CancelPreparedSignalAdvance(PreparedQueueSignalAdvanceV1* prepared) noexcept;
    bool PublishPreparedSignalAdvance(PreparedQueueSignalAdvanceV1* prepared,
                                     AcceptedQueueSignalAdvanceV1* out) noexcept;
    bool SettleSignalAdvance(const AcceptedQueueSignalAdvanceV1& accepted,
                             QueueSignalOutcomeV1 outcome) noexcept;
    bool RollbackUnpublishedConditional(const OriginalConditionalFutureReservation& original,
        const QueueContinuationLane& lane) noexcept;
    bool RetireConditionalForAbort(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        DecisionToken expectedDecision) noexcept;
    bool RollbackInitializingConditional(
        const PreparedQueueInstanceV1& preparedInstance,
        QueueInstanceToken expectedQueue,
        ConditionalQueueVersionToken expectedVersion,
        DecisionToken expectedDecision) noexcept;
    QueueConditionalProofStatus ProveConditionalForQuantum(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        const OccurrenceStatePool& occurrences,
        const CompositeCompletionOwnerPool& composites,
        std::uint64_t quantumStartFrame,
        std::uint64_t quantumEndFrame,
        std::uint64_t expectedCompletedThroughBlockSequence,
        bool currentQuantumScratchAdmitted,
        QueueConditionalProof* outProof) const noexcept;
    QueueConditionalProofStatus ProveConditionalForQuantum(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        const OccurrenceStatePool& occurrences,
        std::uint64_t quantumStartFrame,
        std::uint64_t quantumEndFrame,
        std::uint64_t expectedCompletedThroughBlockSequence,
        bool currentQuantumScratchAdmitted,
        QueueConditionalProof* outProof) const noexcept;
    bool ActivateProvenConditional(
        const QueueConditionalProof& proof) noexcept;
    QueueConditionalProofStatus EvaluateConditionalForQuantum(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        const OccurrenceStatePool& occurrences,
        const CompositeCompletionOwnerPool& composites,
        std::uint64_t quantumStartFrame,
        std::uint64_t quantumEndFrame,
        std::uint64_t expectedCompletedThroughBlockSequence,
        bool currentQuantumScratchAdmitted) noexcept;
    QueueConditionalProofStatus EvaluateConditionalForQuantum(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        const OccurrenceStatePool& occurrences,
        std::uint64_t quantumStartFrame,
        std::uint64_t quantumEndFrame,
        std::uint64_t expectedCompletedThroughBlockSequence,
        bool currentQuantumScratchAdmitted) noexcept;
    QueueFailureDisposition FailContainer(
        QueueInstanceToken token,
        ConditionalQueueVersionToken version,
        ConditionalQueueActivation activation,
        QueueFailureReason reason,
        std::uint64_t failureFrame) noexcept;
    QueueFailureDisposition FailContainerBeforeConditional(
        QueueInstanceToken token, QueueFailureReason reason,
        std::uint64_t failureFrame) noexcept;

private:
    class SourceWaitAuthority {
    public:
        SourceWaitAuthority() = default;
        SourceWaitAuthority(const SourceWaitAuthority&) = delete;
        SourceWaitAuthority& operator=(const SourceWaitAuthority&) = delete;
    private:
        friend class QueueInstanceTable;
        const FrontierGroupBinding* binding_ = nullptr;
        std::uint64_t delayFrames_ = 0;
    };
    bool ProveSourceWaitAuthority(const FrontierGroupBinding& binding, AudioSliceLease& lease,
        const ScheduledTransactionPool& pool, const QueueFrontierSourceWaitContext& context,
        std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
        std::uint64_t completedAudioSequence, SourceWaitAuthority* out,
        const FrontierInitialProof* stagedInitial = nullptr) const noexcept;
    QueueConditionalProofStatus ProveConditionalCompletion(
        QueueInstanceToken token, ConditionalQueueVersionToken version,
        const OccurrenceStatePool& occurrences, const CompositeCompletionOwnerPool& composites,
        std::uint64_t quantumStartFrame, std::uint64_t quantumEndFrame,
        std::uint64_t expectedCompletedThroughBlockSequence, bool currentQuantumScratchAdmitted,
        QueueConditionalProof* outProof, const SourceWaitAuthority* sourceWait) const noexcept;
    QueueResetStatusV1 PrepareConditionalReplacementInternal(
        QueueInstanceToken token, ConditionalQueueVersionToken expectedVersion,
        DecisionToken expectedDecision, const ConditionalQueuePrepareRequest& replacement,
        PreparedQueueConditionalReplacementV1* out, bool retainedCurrent,
        const QueueContinuationLane* stagedOwner = nullptr) noexcept;
    QueueResetStatusV1 CommitConditionalReplacementInternal(
        PreparedQueueConditionalReplacementV1* prepared, LogicCommitSequence sequence,
        bool acceptedStaged) noexcept;
    friend class PrivateSelectedQueueFutureSpanPool;
    friend class OriginalConditionalReplacementBatch;
    friend class OriginalImmediateLaneConditionalProjection;
    friend struct QueueInstanceTableTestAccess;
    friend struct Engine;
    friend class PrivatePendingQueueFrontierBinding;
    bool MatchesPrivatePendingFrontierBinding(const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parent, const FrontierGroupBinding& binding,
        const ScheduledTransactionPool& pool, const TransitionInstancePool& transitions,
        const FrontierGroupBinding* counterpart, const CompositeCompletionOwnerPool* composites) const noexcept;
    // Same native prepared record and parent loan; never a Live Current grant.
    bool CapturePreparedOriginalConditionalFamily(
        const OriginalConditionalFutureReservation& original, const OriginalLifetimePin& parent,
        const PreparedQueueConditionalV1& prepared,
        FrontierQueueInitialIdentity* outInitial, FrontierQueueConditionalIdentity* outConditional,
        WorldGeneration* outWorld, PlayableInstanceHandle* outRoot) const noexcept;
    bool CapturePreparedOriginalConditionalCurrent(
        const OriginalConditionalFutureReservation& original, const OriginalLifetimePin& parent,
        const PreparedQueueConditionalV1& prepared, KnownCompletionPredicateV1* predicate,
        CompletionOwnerRef* owner) const noexcept;
    enum class SlotState : std::uint8_t {
        Free = 0,
        Initializing = 1,
        Live = 2,
        Reclaiming = 3,
        Quarantined = 4,
    };

    enum class SignalReservation : std::uint8_t { None, Prepared, ReadyToPublish };
    enum class NextStateOwner : std::uint8_t { None, RealConditional, AcceptedSignal };

    // Published facts remain at stable addresses until the owning future fence retires.
    struct ConditionalState {
        std::atomic<ConditionalState*> following{nullptr};
        const TimelineArmTable* sourceArms = nullptr;
        const OccurrenceStatePool* sourceOccurrences = nullptr;
        const std::atomic<bool>* predecessorReceipt = nullptr;
        DecisionToken predecessorDecision{};
        TimelineArmToken sourceArm{};
        std::uint32_t originalConditionalRow = UINT32_MAX;
        std::uint64_t originalConditionalMarker = 0;
        std::uint64_t pendingResetIntent = 0;
        std::atomic<bool> pendingConsumesReset{false};
        DecisionToken pendingDecision{};
        OccurrenceToken pendingExpectedCurrent{};
        OccurrenceToken pendingSuccessor{};
        KnownCompletionPredicateV1 pendingCompletion{};
        CompletionOwnerRef pendingCompletionOwner{};
        bool pendingCompletionIsLogicalBoundary = false;
        bool pendingCompletionHasLogicalFloor = false;
        std::uint64_t pendingLogicalCompletionFrame = 0;
        RetiredQueueSourceProof pendingRetiredSource{};
        EffectiveQueueState pendingNext{};
        std::uint32_t pendingStepCount = 0;
        std::atomic<ConditionalQueueActivation> pendingActivation{
            ConditionalQueueActivation::Waiting};
        std::atomic<std::uint64_t> pendingVersion{0};
    };

    struct Row {
        ConditionalState initialConditional{};
        std::atomic<ConditionalState*> conditionalHead{&initialConditional};

        ConditionalState& Pending() noexcept {
            return *conditionalHead.load(std::memory_order_acquire);
        }
        const ConditionalState& Pending() const noexcept {
            return *conditionalHead.load(std::memory_order_acquire);
        }
        std::atomic<SlotState> slotState{SlotState::Free};
        std::uint32_t generation = 1;
        std::uint64_t catalogResetScopeMarker = 0;
        std::uint32_t catalogResetScopeGeneration = 0;
        std::uint64_t originalLifetimeMarker = 0;
        std::uint32_t originalLifetimePinCount = 0;

        VnObjectId projectId{};
        WorldGeneration world = 0;
        PlayableInstanceHandle rootInstance = 0;
        VnObjectId containerStableId{};
        std::uint64_t programSemanticFingerprint = 0;
        const ScheduledTransactionPool* frontierPool = nullptr;
        const FrontierGroupBinding* frontierInitial = nullptr;
        FrontierOwnerIdentity frontierOwner{};
        std::atomic<std::uint64_t> initialActivationSequence{0};
        std::uint32_t childCount = 0;
        QueueCycleV1 cycle = QueueCycleV1::Once;
        bool resumePosition = false;
        std::uint64_t resetEpoch = 0;
        std::uint32_t emitterSlot = UINT32_MAX;
        std::uint32_t emitterGeneration = 0;
        EffectiveQueueState resetInitial{};
        std::uint64_t resetNextVersion = 0;
        std::atomic<bool> resetReserved{false};
        std::atomic<bool> resetPending{false};
        std::uint64_t resetExecuteFrame = 0;
        std::uint64_t resetReservation = 0;
        std::uint64_t resetIntentCounter = 0;
        std::uint64_t resetIntentGeneration = 0;
        QueueExecutionOrderV1 resetOrder{};
        MonitorQueueResetObservation monitorReset{};
        MonitorLifecycleFacts monitorIdentity{};

        SignalReservation signalReservation = SignalReservation::None;
        NextStateOwner nextStateOwner = NextStateOwner::None;
        std::uint64_t signalAttemptCounter = 0;
        AcceptedQueueSignalAdvanceV1 signalIdentity{};
        std::atomic<bool> conditionalReserved{false};
        std::atomic<bool> conditionalReplacementReserved{false};
        const QueueContinuationLane* stagedReplacementOwner = nullptr;

        std::atomic<std::uint64_t> mirrorSequence{0};
        std::atomic<std::uint64_t> mirrorVersion{0};
        std::atomic<std::uint32_t> mirrorCursor{0};
        std::atomic<std::uint32_t> mirrorCurrentChildOrdinal{0};
        std::atomic<std::uint64_t> mirrorCycleOrdinal{0};
        std::atomic<std::uint64_t> mirrorSelectionOrdinal{0};
        std::atomic<std::uint32_t> mirrorRandomInitialRecentCount{0};
        std::array<std::atomic<std::uint32_t>,
                   kMaxQueueAvoidHistoryHard>
            mirrorRandomInitialRecent{};

        std::atomic<QueueFailurePhase> failurePhase{
            QueueFailurePhase::Healthy};
        QueueFailureReason failureReason =
            QueueFailureReason::RenderProgressLost;
        std::uint64_t failureFrame = 0;
    };

    enum class OriginalConditionalState : std::uint8_t {
        Free, Prepared, Committed, Retired, Quarantined
    };
    enum class OriginalConditionalClockState : std::uint8_t { Absent, Unbound, Bound, Retired };
    struct OriginalConditionalRow {
        OriginalConditionalState state = OriginalConditionalState::Free;
        std::uint32_t generation = 1;
        std::uint64_t marker = 0;
        std::uint32_t loans = 0;
        std::uint64_t readRetirementEpoch = 0;
        OriginalConditionalRow* nextReadRetirement = nullptr;
        OriginalLifetimePin parent;
        OriginalConditionalFutureReservation predecessor;
        ConditionalState followingBody{};
        ConditionalState* body = nullptr;
        std::atomic<bool> activated{false};
        // A private selected-future issuer; Old progress never consults this.
        const PrivateSelectedQueueFutureSpanPool* selectedFutureOwner = nullptr;
        std::uint64_t selectedFutureMarker = 0;
        // Written only by the accepted native replacement participant. The old
        // factory record still owns its original C0 dependency and first clock.
        bool selectedFutureConsumed = false;
        DecisionToken selectedDecision{};
        OccurrenceToken selectedSuccessor{};
        ConditionalQueueVersionToken selectedVersion{};
        // Stable original storage; no absolute clock exists before real binding.
        OriginalConditionalClockState clockState = OriginalConditionalClockState::Absent;
        ScheduledTransactionPool* clockPool = nullptr;
        std::array<OriginalQueueFutureGroupLifetimePin, 2> clockGroups{};
        std::array<std::uint64_t, 2> clockRelativeFrames{};
        OriginalConditionalInitialClockBinding clockBinding{};
        QueueInstanceToken queue{};
        ConditionalQueueVersionToken version{};
        WorldGeneration world = 0;
        PlayableInstanceHandle rootInstance = 0;
        std::uint64_t programSemanticFingerprint = 0;
        DecisionToken decision{};
        OccurrenceToken current{};
        OccurrenceToken successor{};
        KnownCompletionPredicateV1 completion{};
        CompletionOwnerRef completionOwner{};
        bool logicalBoundary = false;
        bool logicalFloor = false;
        std::uint64_t logicalCompletionFrame = 0;
        RetiredQueueSourceProof retiredSource{};
        EffectiveQueueState currentState{};
        // Projection bodies are validated before reservation; the live row owns
        // the selected body. Historical native readers retain only its versions.
        ConditionalQueueVersionToken next{};
        ConditionalQueueVersionToken first{};
        std::uint32_t stepCount = 0;
        std::uint64_t resetIntent = 0;
        bool consumesReset = false;
    };
    bool PrepareConditionalInternal(QueueInstanceToken token,
        const ConditionalQueuePrepareRequest& request, PreparedQueueConditionalV1* out,
        const OriginalLifetimePin* parentOriginal,
        OriginalConditionalFutureReservation* outOriginal,
        const OriginalConditionalFutureReservation* predecessor = nullptr,
        const ContinuationSourceBinding* source = nullptr) noexcept;
    static bool ValidateProjection(const ConditionalQueuePrepareRequest& request,
        const EffectiveQueueState& current, std::uint32_t childCount,
        EffectiveQueueState* first, std::uint32_t* count) noexcept;
    ConditionalState* FindConditional(Row& row, ConditionalQueueVersionToken version) noexcept;
    const ConditionalState* FindConditional(const Row& row, ConditionalQueueVersionToken version) const noexcept;
    const ConditionalState* ReadConditionalHead(const Row& row) const noexcept;
    void RetireDetachedConditional(Row& row, ConditionalState& state) noexcept;
    void EndConditionalChain(Row& row) noexcept;
    bool FollowingSourceAdmitted(const ConditionalState& state) const noexcept;
    // Only the actual Engine issuer can enter this producer-private finalizer.
    // General PrepareConditional and the no-frontier-reader timing API retain
    // their existing contracts. This creates no publication or cutover grant.
    bool BindPrivateInitialOriginalConditionalClock(
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parentOriginal, const OriginalConditionalLaneClock& clock,
        ScheduledTransactionPool& pool, const PrivateOriginalQueueGroupClockProof& privateClock,
        const TimelineArmTable& arms, const DecisionTable& decisions, TimelineArmToken sourceArm,
        PlayableInstanceHandle sourceRoot, std::uint64_t sourceRootEpoch,
        OccurrenceToken sourceCurrent, const CompletionOwnerRef& sourceOwner,
        std::uint64_t installedAckSequence) noexcept;
    bool IsSelectedOriginalConditionalCurrent(
        const OriginalConditionalFutureReservation& original) const noexcept;
    bool IsAcceptedSelectedConditionalHead(const Row& row,
        const OriginalConditionalRow& original) const noexcept;
    bool BindRetainedOriginalFrontierConditional(
        const OriginalConditionalFutureReservation& original,
        const OriginalLifetimePin& parent, const FrontierGroupBinding& binding,
        const ScheduledTransactionPool& pool) noexcept;
    bool MatchesOriginalConditionalLaneClock(const OriginalConditionalLaneClock& clock) const noexcept;
    bool BorrowOriginalConditionalLaneClock(const OriginalConditionalLaneClock& clock,
        OriginalConditionalLaneClock* out) noexcept;
    bool MatchesOriginalConditionalFutureReservation(
        const OriginalConditionalFutureReservation& original) const noexcept;
    bool BorrowOriginalConditionalFutureReservation(
        const OriginalConditionalFutureReservation& original,
        OriginalConditionalFutureReservation* out) noexcept;
    void ReleaseOriginalConditionalFutureReservation(
        OriginalConditionalFutureReservation& original) noexcept;
    bool MatchesPreparedOriginalConditional(const PreparedQueueConditionalV1& prepared,
        const Row& row) const noexcept;
    // Only the owning Engine registers these serialized-producer callbacks.
    // The resolver selects existing storage; the table authenticates its exact
    // native record before calling associationEnding. No observer can issue it.
    using OriginalConditionalBaselineResolver = OriginalConditionalFutureReservation* (*)(
        void*, QueueInstanceTable&, QueueInstanceToken) noexcept;
    using OriginalConditionalAssociationEnding = void (*)(
        void*, QueueInstanceTable&, OriginalConditionalFutureReservation&) noexcept;
    bool RegisterOriginalConditionalOwner(void* context,
        OriginalConditionalBaselineResolver resolver,
        OriginalConditionalAssociationEnding associationEnding) noexcept;
    bool OriginalConditionalOwnerEmpty(const OriginalConditionalFutureReservation& baseline,
        const OriginalConditionalLaneClock& clock) const noexcept;
    bool IsAssociatedOriginalConditional(const OriginalConditionalFutureReservation& cap) const noexcept;
    bool IsOriginalConditionalClockEmpty(const OriginalConditionalLaneClock& clock) const noexcept;
    bool IsEndedOriginalConditional(const OriginalConditionalFutureReservation& cap) const noexcept;
    void EndOriginalConditionalFuture(Row& row) noexcept;
    void EndOriginalConditionalFuture(Row& row, ConditionalState& state) noexcept;
    void EndOriginalConditionalsForQuiescentOwnerTeardown() noexcept;
    void EndOriginalConditionalAssociationsAfterQuiescence() noexcept;
    void ReclaimOriginalConditionalIfUnowned(OriginalConditionalRow& original) noexcept;

    Row* Resolve(QueueInstanceToken token) noexcept;
    const Row* Resolve(QueueInstanceToken token) const noexcept;
    static bool CaptureRowSnapshot(const Row& row, QueueInstanceSnapshot* out) noexcept;
    static bool LoadMirror(const Row& row,
                           EffectiveQueueState* out) noexcept;
    static bool StoreMirror(Row& row,
                            const EffectiveQueueState& state) noexcept;
    static bool MayMutateFrontier(const Row& row) noexcept;
    static bool HasFrontierReaders(const Row& row) noexcept;
    static bool SameResetTarget(const Row& row,
                                const QueueResetTargetV1& target) noexcept;
    bool MatchesCatalogResetScope(const PreparedCatalogResetScope& original) const noexcept;
    QueueResetStatusV1 PrepareCatalogResetScopeInternal(const QueueResetTargetV1& target,
        bool nativeOnly, PreparedCatalogResetScope* out) noexcept;
    bool HasCurrentNativeConditional(const Row& row) const noexcept;
    QueueResetStatusV1 PrepareResetInternal(const QueueResetTargetV1& target,
        const PreparedCatalogResetScope* nativeScope, PreparedQueueResetSetV1* out,
        std::uint64_t executeFrame, std::uint32_t executionOrdinal) noexcept;
    void ReleaseCatalogResetScope(PreparedCatalogResetScope& original) noexcept;
    bool MatchesOriginalLifetimePin(const OriginalLifetimePin& original) const noexcept;
    bool BorrowOriginalLifetimePin(const OriginalLifetimePin& original,
        OriginalLifetimePin* out) noexcept;
    void ReleaseOriginalLifetimePin(OriginalLifetimePin& original) noexcept;
    void ConsumeOriginalLifetimePinAfterReclaim(
        OriginalLifetimePin* original, Row& row) noexcept;
    bool AcquireInternal(const QueueInstanceCreateRequest& request,
                         QueueInstanceToken* outToken,
                         bool prepared, OriginalLifetimePin* originalLifetime) noexcept;
    void ReleaseResetReservations(
        const QueueResetTargetV1& target,
        std::uint32_t slotEndExclusive, std::uint64_t reservation) noexcept;
    static bool ProjectReset(const Row& row, const EffectiveQueueState& current,
                             EffectiveQueueState* out) noexcept;
    static void ConsumeResetIntent(Row& row, std::uint64_t intent) noexcept;
    static MonitorControlCause MarkMonitorResetApplied(Row& row,
        std::uint64_t intent, std::uint64_t previousVersion, std::uint64_t nextVersion) noexcept;
    static bool SignalOwnsNext(const Row& row) noexcept {
        return row.signalReservation != SignalReservation::None ||
               row.nextStateOwner == NextStateOwner::AcceptedSignal;
    }

    bool PrepareInstanceSlot(std::uint32_t slot) noexcept;
    bool PrepareConditionalSlot(std::uint32_t slot) noexcept;
    StableSlotStorage<Row> rows_;
    // Noncopyable loans live in fixed separate storage, never in resettable Row.
    StableSlotStorage<OriginalConditionalRow> originalConditionalRows_;
    void* rowStorageContext_ = nullptr;
    bool (*prepareRowStorage_)(void*, std::uint32_t) noexcept = nullptr;
    std::uint32_t originalConditionalCapacity_ = 0;
    std::uint64_t nextOriginalConditionalFactoryMarker_ = 0;
    bool conditionalReadReclamationEnabled_ = false;
    std::atomic<std::uint64_t> conditionalReadRetirementRequest_{0};
    std::atomic<std::uint64_t> conditionalReadRetirementAck_{0};
    OriginalConditionalRow* retiredConditionalReads_ = nullptr;
#if !defined(HARKEN_RT_QUALIFICATION)
    ConditionalHeadReadHookForTest conditionalHeadReadHookForTest_ = nullptr;
    void* conditionalHeadReadContextForTest_ = nullptr;
#endif
    void* originalConditionalOwnerContext_ = nullptr;
    OriginalConditionalBaselineResolver originalConditionalBaselineResolver_ = nullptr;
    OriginalConditionalAssociationEnding originalConditionalAssociationEnding_ = nullptr;
    std::uint32_t capacity_ = 0;
    std::uint64_t resetReservationCounter_ = 0;
    std::uint64_t nextCatalogResetScopeMarker_ = 0;
    std::uint64_t liveCatalogResetScopeMarker_ = 0;
    std::uint64_t nextOriginalLifetimeMarker_ = 0;
    std::uint64_t logicNow_ = 0;
};

// Synchronous borrowed evidence; every use rechecks the original Queue and parent loan.
class PrivatePendingQueueFrontierBinding {
public:
    bool Valid() const noexcept { return owner_ != nullptr; }
    bool Matches(const FrontierGroupBinding& binding, const ScheduledTransactionPool& pool) const noexcept;
private:
    friend class QueueInstanceTable;
    const QueueInstanceTable* owner_ = nullptr;
    const QueueInstanceTable::OriginalConditionalFutureReservation* original_ = nullptr;
    const QueueInstanceTable::OriginalLifetimePin* parent_ = nullptr;
    const FrontierGroupBinding* binding_ = nullptr;
    const FrontierGroupBinding* counterpart_ = nullptr;
    const ScheduledTransactionPool* pool_ = nullptr;
    const TransitionInstancePool* transitions_ = nullptr;
    const CompositeCompletionOwnerPool* composites_ = nullptr;
};

static_assert(std::atomic<ConditionalQueueActivation>::is_always_lock_free);
static_assert(std::atomic<QueueFailurePhase>::is_always_lock_free);

} // namespace harken

#endif // HARKEN_CORE_QUEUE_RUNTIME_H
