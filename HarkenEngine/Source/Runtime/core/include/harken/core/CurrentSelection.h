#ifndef HARKEN_CORE_CURRENT_SELECTION_H
#define HARKEN_CORE_CURRENT_SELECTION_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"
#include "harken/core/Random.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kSelectionPathNodeCapacity = 64;

enum class SelectionDisciplineV1 : std::uint32_t {
    Draw = 0,
    Rotation = 1,
    Sequence = 2,
};

enum class SelectionStatusV1 : std::uint32_t {
    Ok = 0,
    InvalidInput = 1,
    InvalidAvoid = 2,
    WeightOverflow = 3,
    OutputCapacity = 4,
    SequenceComplete = 5,
};

struct SelectionPathNodeV1 {
    PlayableKindV1 kind = PlayableKindV1::Sound;
    VnObjectId objectId = {};
    VnObjectId trackId{};
    VnObjectId clipId{};
    std::uint64_t clipGuidHigh = 0;
    std::uint64_t clipGuidLow = 0;
    std::uint32_t childOrdinal = 0;
};

struct SelectionSeedInputV1 {
    std::uint64_t worldSemanticFingerprint = 0;
    VnObjectId programStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    MemoryScopeV1 memoryScope = MemoryScopeV1::Emitter;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
    std::uint64_t resetEpoch = 0;
    VnObjectId rootStableId{};
    std::uint64_t callerSeed = 0;
    std::uint64_t acceptedPostOrdinal = 0;
    std::uint32_t pathCount = 0;
    const SelectionPathNodeV1* path = nullptr;
    std::uint32_t occurrenceOrdinal = 0;
    std::uint32_t loopIteration = 0;
    std::uint64_t queueCycleOrdinal = 0;
    std::uint64_t selectionOrdinal = 0;
    SelectionDisciplineV1 discipline = SelectionDisciplineV1::Draw;
};

struct SelectionChildV1 {
    std::uint32_t stableChildOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t frequencyWeight = 0;
    std::uint32_t orderWeight = 0;
};

struct SequenceSelectionV1 {
    std::uint32_t selectedChildOrdinal = 0;
    std::uint32_t nextCursor = 0;
    bool stopped = false;
};

struct SelectionMemoryConfigV1 {
    std::uint32_t maxEmitters = 128;
    std::uint32_t maxGlobalRows = 256;
    std::uint32_t maxEmitterRowsPerEmitter = 16;
    std::uint32_t maxPendingDeltasTotal = 512;
    std::uint32_t maxPendingGlobalPerRow = 512;
    std::uint32_t maxPendingEmitterPerRow = 16;
    std::uint32_t maxChildren = 64;
    std::uint32_t maxAvoidLastN = 8;
    std::uint32_t maxPathNodes = 64;
};

struct SelectionMemoryKeyV1 {
    WorldGeneration world = 0;
    VnObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    MemoryScopeV1 memoryScope = MemoryScopeV1::Emitter;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
};

struct SelectionPlayRequestV1 {
    SelectionMemoryKeyV1 key{};
    const ContainerProgramViewV3* program = nullptr;
    std::uint64_t worldSemanticFingerprint = 0;
    VnObjectId rootStableId{};
    std::uint64_t callerSeed = 0;
    std::uint64_t acceptedPostOrdinal = 0;
    std::uint32_t pathCount = 0;
    const SelectionPathNodeV1* path = nullptr;
    std::uint32_t occurrenceOrdinal = 0;
    std::uint32_t loopIteration = 0;
    std::uint64_t ticket = 0;
    std::uint32_t eventIndex = 0;
    std::uint64_t executeFrame = 0;
};

// Read-only copy of the four integers ComputePlay sent into
// ComputeSelectionSeedV1. Must not be written back into seedInput.
struct SelectionSeedProbeCopyV1 {
    bool captured = false;
    std::uint64_t derivedCallerSeed = 0;
    std::uint64_t acceptedPostOrdinal = 0;
    std::uint64_t selectionOrdinal = 0;
    std::uint64_t resetEpoch = 0;
};

struct SelectionDeltaRefV1 {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint64_t lifetime = 0;

    explicit operator bool() const noexcept {
        return slot != UINT32_MAX && generation != 0 && lifetime != 0;
    }
};

enum class SelectionDeltaKindV1 : std::uint8_t { Play = 0, Reset = 1 };

enum class SelectionMemoryStatusV1 : std::uint8_t {
    Ok = 0,
    InvalidInput = 1,
    Capacity = 2,
    NotFound = 3,
    NotHead = 4,
    SequenceComplete = 5,
    RuntimeRecreateRequired = 6,
};

struct SelectionAppendResultV1 {
    SelectionMemoryStatusV1 status = SelectionMemoryStatusV1::InvalidInput;
    SelectionDeltaRefV1 delta{};
    bool idempotent = false;
};

struct SelectionDeltaViewV1 {
    bool live = false;
    bool head = false;
    SelectionDeltaKindV1 kind = SelectionDeltaKindV1::Play;
    SelectionMemoryStatusV1 status = SelectionMemoryStatusV1::InvalidInput;
    SelectionMemoryKeyV1 key{};
    std::uint64_t ticket = 0;
    std::uint32_t eventIndex = 0;
    std::uint32_t selectedChildOrdinal = 0;
    std::uint64_t resetEpochAfter = 0;
    std::uint64_t selectionOrdinalAfter = 0;
};

struct SelectionMemorySnapshotV1 {
    bool found = false;
    std::uint64_t resetEpoch = 0;
    std::uint64_t selectionOrdinal = 0;
    std::uint32_t sequenceCursor = 0;
    bool sequenceStopped = false;
    std::uint32_t rotationCursor = 0;
    std::uint32_t recentCount = 0;
    std::uint32_t permutationCount = 0;
    std::uint32_t pendingCount = 0;
};

class SelectionMemoryTableV1;

struct SelectionPendingBatchIdentityV1 {
    WorldGeneration world = 0;
    std::uint64_t worldSemanticFingerprint = 0;
    VnObjectId rootStableId{};
    std::uint64_t callerSeed = 0;
    std::uint64_t acceptedPostOrdinal = 0;
    std::uint64_t ticket = 0;
    std::uint32_t emitterSlot = UINT32_MAX;
    std::uint32_t emitterGeneration = 0;
    std::uint32_t eventBegin = 0;
    std::uint32_t eventCount = 0;
};

class PendingSelectionReplacementV1 {
public:
    PendingSelectionReplacementV1() = default;
    PendingSelectionReplacementV1(const PendingSelectionReplacementV1&) = delete;
    PendingSelectionReplacementV1& operator=(const PendingSelectionReplacementV1&) = delete;
    PendingSelectionReplacementV1& operator=(PendingSelectionReplacementV1&&) = delete;
    PendingSelectionReplacementV1(PendingSelectionReplacementV1&& other) noexcept
        : owner_(other.owner_), slot_(other.slot_), generation_(other.generation_),
          lifetime_(other.lifetime_), session_(other.session_) { other.Clear(); }
    bool Valid() const noexcept { return owner_ != nullptr; }

private:
    friend class SelectionMemoryTableV1;
    void Clear() noexcept {
        owner_ = nullptr;
        slot_ = UINT32_MAX;
        generation_ = 0;
        lifetime_ = session_ = 0;
    }
    SelectionMemoryTableV1* owner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    std::uint32_t generation_ = 0;
    std::uint64_t lifetime_ = 0;
    std::uint64_t session_ = 0;
};

struct SelectionReplacementPlayViewV1 {
    SelectionMemoryStatusV1 status = SelectionMemoryStatusV1::InvalidInput;
    std::uint32_t selectedChildOrdinal = 0;
    SelectionSeedProbeCopyV1 seedProbe{};
};

class PendingSelectionBatchV1 {
public:
    PendingSelectionBatchV1() = default;
    PendingSelectionBatchV1(const PendingSelectionBatchV1&) = delete;
    PendingSelectionBatchV1& operator=(const PendingSelectionBatchV1&) = delete;
    PendingSelectionBatchV1& operator=(PendingSelectionBatchV1&&) = delete;
    PendingSelectionBatchV1(PendingSelectionBatchV1&& other) noexcept
        : owner_(other.owner_), slot_(other.slot_), generation_(other.generation_),
          lifetime_(other.lifetime_) { other.Clear(); }

    bool Valid() const noexcept { return owner_ != nullptr; }

private:
    friend class SelectionMemoryTableV1;
    void Clear() noexcept {
        owner_ = nullptr;
        slot_ = UINT32_MAX;
        generation_ = 0;
        lifetime_ = 0;
    }
    SelectionMemoryTableV1* owner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    std::uint32_t generation_ = 0;
    std::uint64_t lifetime_ = 0;
};

struct PendingSelectionBatchViewV1 {
    bool live = false;
    std::uint32_t memberCount = 0;
    std::uint64_t revision = 0;
};

struct PendingSelectionBatchCommitV1 {
    PendingSelectionBatchV1* batch = nullptr;
    std::uint64_t revision = 0;
};

struct PendingSelectionBatchMemberViewV1 {
    SelectionDeltaRefV1 ref{};
    SelectionDeltaViewV1 delta{};
    SelectionSeedProbeCopyV1 seedProbe{};
    std::uint64_t revision = 0;
};

class PreparedSelectionPlayV1 {
public:
    PreparedSelectionPlayV1() = default;
    PreparedSelectionPlayV1(const PreparedSelectionPlayV1&) = delete;
    PreparedSelectionPlayV1& operator=(
        const PreparedSelectionPlayV1&) = delete;
    PreparedSelectionPlayV1(PreparedSelectionPlayV1&& other) noexcept
        : owner_(other.owner_), slot_(other.slot_),
          generation_(other.generation_), lifetime_(other.lifetime_), prepared_(other.prepared_),
          committed_(other.committed_) {
        other.owner_ = nullptr;
        other.slot_ = UINT32_MAX;
        other.generation_ = 0;
        other.lifetime_ = 0;
        other.prepared_ = false;
        other.committed_ = false;
    }
    PreparedSelectionPlayV1& operator=(
        PreparedSelectionPlayV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            slot_ = other.slot_;
            generation_ = other.generation_;
            lifetime_ = other.lifetime_;
            prepared_ = other.prepared_;
            committed_ = other.committed_;
            other.owner_ = nullptr;
            other.slot_ = UINT32_MAX;
            other.generation_ = 0;
            other.lifetime_ = 0;
            other.prepared_ = false;
            other.committed_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    bool Committed() const noexcept { return owner_ != nullptr && committed_; }

private:
    friend class SelectionMemoryTableV1;
    SelectionMemoryTableV1* owner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    std::uint32_t generation_ = 0;
    std::uint64_t lifetime_ = 0;
    bool prepared_ = false;
    bool committed_ = false;
};

class PreparedSelectionResetV1 {
public:
    PreparedSelectionResetV1() = default;
    PreparedSelectionResetV1(const PreparedSelectionResetV1&) = delete;
    PreparedSelectionResetV1& operator=(
        const PreparedSelectionResetV1&) = delete;
    PreparedSelectionResetV1(PreparedSelectionResetV1&& other) noexcept
        : owner_(other.owner_), slot_(other.slot_),
          generation_(other.generation_), lifetime_(other.lifetime_), prepared_(other.prepared_),
          idempotent_(other.idempotent_), pending_(other.pending_) {
        other.owner_ = nullptr;
        other.slot_ = UINT32_MAX;
        other.generation_ = 0;
        other.lifetime_ = 0;
        other.prepared_ = false;
        other.idempotent_ = false;
        other.pending_ = false;
    }
    PreparedSelectionResetV1& operator=(
        PreparedSelectionResetV1&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            slot_ = other.slot_;
            generation_ = other.generation_;
            lifetime_ = other.lifetime_;
            prepared_ = other.prepared_;
            idempotent_ = other.idempotent_;
            pending_ = other.pending_;
            other.owner_ = nullptr;
            other.slot_ = UINT32_MAX;
            other.generation_ = 0;
            other.lifetime_ = 0;
            other.prepared_ = false;
            other.idempotent_ = false;
            other.pending_ = false;
        }
        return *this;
    }

    bool Valid() const noexcept { return owner_ != nullptr && prepared_; }
    bool Idempotent() const noexcept { return Valid() && idempotent_; }

private:
    friend class SelectionMemoryTableV1;
    SelectionMemoryTableV1* owner_ = nullptr;
    std::uint32_t slot_ = UINT32_MAX;
    std::uint32_t generation_ = 0;
    std::uint64_t lifetime_ = 0;
    bool prepared_ = false;
    bool idempotent_ = false;
    bool pending_ = false;
};

class SelectionMemoryTableV1 {
public:
    bool Reset(const SelectionMemoryConfigV1& config) noexcept;
    static std::uint64_t EmitterStorageBytes(const SelectionMemoryConfigV1& config) noexcept;
    SelectionAppendResultV1 AppendPlay(
        const SelectionPlayRequestV1& request) noexcept;
    SelectionAppendResultV1 AppendReset(
        const SelectionMemoryKeyV1& key, std::uint64_t ticket,
        std::uint32_t eventIndex) noexcept;
    SelectionMemoryStatusV1 PreparePlay(
        const SelectionPlayRequestV1& request,
        PreparedSelectionPlayV1* out,
        std::uint32_t* outSelectedChildOrdinal) noexcept;
    SelectionMemoryStatusV1 PreparePlayForPendingBatch(
        const SelectionPlayRequestV1& request,
        PreparedSelectionPlayV1* out,
        std::uint32_t* outSelectedChildOrdinal) noexcept;
    SelectionSeedProbeCopyV1 PreparedPlaySeedProbe(
        const PreparedSelectionPlayV1& prepared) const noexcept;
    SelectionMemoryStatusV1 CommitPreparedPlay(
        PreparedSelectionPlayV1* prepared,
        std::uint64_t ticket) noexcept;
    SelectionMemoryStatusV1 MovePreparedPlayToPending(
        PreparedSelectionPlayV1* prepared,
        std::uint64_t ticket,
        SelectionDeltaRefV1* out) noexcept;
    SelectionMemoryStatusV1 MovePreparedPlaysToPending(
        PreparedSelectionPlayV1* const* prepared, std::uint32_t count,
        std::uint64_t ticket, PendingSelectionBatchV1* out) noexcept;
    SelectionMemoryStatusV1 IssueProducerPendingSelectionBatch(
        const SelectionPendingBatchIdentityV1& identity,
        PreparedSelectionPlayV1* const* prepared, std::uint32_t count,
        PendingSelectionBatchV1* out) noexcept;
    SelectionMemoryStatusV1 BeginPendingBatchReplacement(
        const PendingSelectionBatchV1& batch, std::uint64_t revision,
        PendingSelectionReplacementV1* out) noexcept;
    SelectionMemoryStatusV1 PrepareReplacementPlay(
        PendingSelectionReplacementV1* replacement,
        const SelectionPlayRequestV1& request,
        std::uint32_t* outSelectedChildOrdinal) noexcept;
    SelectionReplacementPlayViewV1 ReadReplacementPlay(
        const PendingSelectionReplacementV1& replacement, std::uint32_t member) const noexcept;
    SelectionMemoryStatusV1 SealReplacement(
        PendingSelectionReplacementV1* replacement, std::uint32_t count,
        std::uint64_t frame) noexcept;
    SelectionMemoryStatusV1 CanInstallReplacement(
        const PendingSelectionReplacementV1& replacement) const noexcept;
    SelectionMemoryStatusV1 InstallReplacement(
        PendingSelectionReplacementV1* replacement,
        PendingSelectionBatchV1* batch) noexcept;
    SelectionMemoryStatusV1 CancelReplacement(PendingSelectionReplacementV1* replacement) noexcept;
    PendingSelectionBatchViewV1 ReadPendingBatch(
        const PendingSelectionBatchV1& batch) const noexcept;
    PendingSelectionBatchMemberViewV1 ReadPendingBatchMember(
        const PendingSelectionBatchV1& batch, std::uint32_t member) const noexcept;
    SelectionMemoryStatusV1 CanCommitPendingBatch(
        const PendingSelectionBatchV1& batch, std::uint64_t revision) const noexcept;
    SelectionMemoryStatusV1 CommitPendingBatch(
        PendingSelectionBatchV1* batch, std::uint64_t revision) noexcept;
    SelectionMemoryStatusV1 CanCommitPendingBatches(
        const PendingSelectionBatchCommitV1* batches, std::uint32_t count) const noexcept;
    SelectionMemoryStatusV1 CommitPendingBatches(
        const PendingSelectionBatchCommitV1* batches, std::uint32_t count) noexcept;
    SelectionMemoryStatusV1 CancelPendingBatch(PendingSelectionBatchV1* batch) noexcept;
    std::uint32_t FreePendingBatchCount() const noexcept;
    // Registry, membership and scratch only; excludes existing state frontier metadata.
    std::size_t PendingBatchStorageBytes() const noexcept;
    SelectionMemoryStatusV1 CancelPreparedPlay(
        PreparedSelectionPlayV1* prepared) noexcept;
    SelectionMemoryStatusV1 CommitPreparedPlayTransactional(
        PreparedSelectionPlayV1* prepared,
        std::uint64_t ticket) noexcept;
    SelectionMemoryStatusV1 FinalizeCommittedPlay(
        PreparedSelectionPlayV1* committed) noexcept;
    SelectionMemoryStatusV1 RollbackCommittedPlay(
        PreparedSelectionPlayV1* committed) noexcept;
    SelectionMemoryStatusV1 PrepareReset(
        const SelectionMemoryKeyV1& key, std::uint64_t expectedTicket,
        std::uint32_t eventIndex,
        PreparedSelectionResetV1* out,
        std::uint64_t executeFrame = 0) noexcept;
    SelectionMemoryStatusV1 CommitPreparedReset(
        PreparedSelectionResetV1* prepared,
        std::uint64_t ticket) noexcept;
    SelectionMemoryStatusV1 PreparePendingReset(
        const SelectionMemoryKeyV1& key, std::uint64_t expectedTicket,
        std::uint32_t eventIndex, PreparedSelectionResetV1* out,
        std::uint64_t executeFrame = 0) noexcept;
    SelectionMemoryStatusV1 AcceptPreparedPendingReset(
        PreparedSelectionResetV1* prepared, std::uint64_t ticket) noexcept;
    SelectionMemoryStatusV1 CanAcceptPreparedPendingReset(
        const PreparedSelectionResetV1& prepared, std::uint64_t ticket,
        bool* outIdempotent = nullptr) const noexcept;
    SelectionMemoryStatusV1 CancelPreparedReset(
        PreparedSelectionResetV1* prepared) noexcept;
    SelectionMemoryStatusV1 CommitHead(SelectionDeltaRefV1 delta) noexcept;
    SelectionMemoryStatusV1 FailHead(SelectionDeltaRefV1 delta) noexcept;
    SelectionMemoryStatusV1 CancelDelta(SelectionDeltaRefV1 delta) noexcept;
    SelectionDeltaViewV1 Delta(SelectionDeltaRefV1 delta) const noexcept;
    SelectionMemorySnapshotV1 Snapshot(
        const SelectionMemoryKeyV1& key) const noexcept;
    std::uint32_t CopyRecent(const SelectionMemoryKeyV1& key,
                             std::uint32_t* output,
                             std::uint32_t capacity) const noexcept;
    std::uint32_t CopyPermutation(const SelectionMemoryKeyV1& key,
                                  std::uint32_t* output,
                                  std::uint32_t capacity) const noexcept;
    void RetireEmitter(std::uint32_t emitterSlot,
                       std::uint32_t emitterGeneration) noexcept;
    void RetireWorld(WorldGeneration world) noexcept;
    void RetireUnusedWorldsBefore(WorldGeneration current,
        bool (*hasReaders)(const void*, WorldGeneration) noexcept,
        const void* context) noexcept;
    std::uint32_t RowCount() const noexcept { return rowCount_; }
    std::uint32_t PendingCount() const noexcept { return pendingCount_; }
    std::uint32_t FreeDeltaCount() const noexcept;
    std::uint32_t QuarantinedDeltaCount() const noexcept {
        return quarantinedDeltaCount_;
    }
    bool SetDeltaGenerationForTests(std::uint32_t slot,
                                    std::uint32_t generation) noexcept;
    void ApplyDue(std::uint64_t now) noexcept;

private:
    struct MemoryState {
        std::uint64_t resetEpoch = 0;
        std::uint64_t selectionOrdinal = 0;
        std::uint32_t sequenceCursor = 0;
        bool sequenceStopped = false;
        std::uint32_t rotationCursor = 0;
        std::vector<std::uint32_t> recent;
        std::vector<std::uint32_t> permutation;
        bool hasScheduledFrontier = false;
        std::uint64_t scheduledFrame = 0;
        std::uint64_t scheduledTicket = 0;
        std::uint32_t scheduledEvent = 0;
        bool hasNativeScheduledFrontier = false;
        std::uint64_t nativeScheduledFrame = 0;
        std::uint64_t nativeScheduledTicket = 0;
        std::uint32_t nativeScheduledEvent = 0;
        bool hasImmediateFrontier = false;
        std::uint64_t immediateTicket = 0;
        std::uint32_t immediateEvent = 0;
        bool hasPrivateScheduledFrontier = false;
        std::uint64_t privateScheduledFrame = 0;
        std::uint64_t privateScheduledTicket = 0;
        std::uint32_t privateScheduledEvent = 0;
        bool hasPrivateImmediateFrontier = false;
        std::uint64_t privateImmediateTicket = 0;
        std::uint32_t privateImmediateEvent = 0;
    };

    struct Row {
        bool used = false;
        bool committed = false;
        bool counted = false;
        SelectionMemoryKeyV1 key{};
        ContainerModeV1 mode = ContainerModeV1::Random;
        RandomDisciplineV1 randomDiscipline = RandomDisciplineV1::Draw;
        SequenceEndPolicyV1 sequenceEndPolicy = SequenceEndPolicyV1::Wrap;
        std::uint32_t initialSequenceCursor = 0;
        std::uint32_t avoidLastN = 0;
        MemoryState state{};
        std::uint32_t head = UINT32_MAX;
        std::uint32_t tail = UINT32_MAX;
        std::uint32_t pendingCount = 0;
        std::uint32_t preparedHead = UINT32_MAX;
        std::uint32_t preparedTail = UINT32_MAX;
        std::uint32_t preparedCount = 0;
        std::uint32_t scheduledHead = UINT32_MAX;
        std::uint32_t scheduledTail = UINT32_MAX;
        std::uint32_t scheduledCount = 0;
        std::uint32_t candidateCount = 0;
    };

    struct DeltaRow {
        bool used = false;
        bool prepared = false;
        bool nativePendingPreparation = false;
        bool pendingReset = false;
        bool replacementCandidate = false;
        bool quarantined = false;
        std::uint32_t generation = 1;
        std::uint32_t row = UINT32_MAX;
        std::uint32_t next = UINT32_MAX;
        std::uint32_t previous = UINT32_MAX;
        SelectionDeltaKindV1 kind = SelectionDeltaKindV1::Play;
        SelectionMemoryStatusV1 status =
            SelectionMemoryStatusV1::InvalidInput;
        SelectionPlayRequestV1 request{};
        std::vector<SelectionPathNodeV1> path;
        std::uint64_t ticket = 0;
        std::uint32_t eventIndex = 0;
        std::uint64_t executeFrame = 0;
        std::uint32_t selectedChildOrdinal = 0;
        SelectionSeedProbeCopyV1 seedProbe{};
        MemoryState overlay{};
        MemoryState rollback{};
        bool rollbackCommitted = false;
        bool rollbackCounted = false;
        bool rollbackable = false;
        struct BatchMembership {
            std::uint32_t slot = UINT32_MAX;
            std::uint32_t generation = 0;
            std::uint32_t next = UINT32_MAX;
            std::uint32_t ordinal = 0;
        } batch{};
    };

    struct BatchRow {
        bool used = false;
        bool quarantined = false;
        std::uint32_t generation = 1;
        std::uint32_t head = UINT32_MAX;
        std::uint32_t count = 0;
        std::uint64_t revision = 1;
        bool producer = false;
        SelectionPendingBatchIdentityV1 identity{};
        bool replacementActive = false;
        bool replacementSealed = false;
        std::uint64_t replacementSession = 0;
        std::uint64_t replacementEpoch = 0;
        std::uint64_t replacementRevision = 0;
        std::uint64_t replacementFrame = 0;
        std::uint32_t replacementHead = UINT32_MAX;
        std::uint32_t replacementTail = UINT32_MAX;
        std::uint32_t replacementCount = 0;
    };

    const BatchRow* ResolveBatch(const PendingSelectionBatchV1& batch) const noexcept;
    bool ValidateBatchMembers(std::uint32_t slot) const noexcept;
    void RecycleBatch(std::uint32_t slot) noexcept;
    void CancelBatchSlot(std::uint32_t slot) noexcept;
    const BatchRow* ResolveReplacement(const PendingSelectionReplacementV1& replacement) const noexcept;
    SelectionMemoryStatusV1 CheckReplacementCurrent(const PendingSelectionReplacementV1& replacement) const noexcept;
    bool ValidateReplacementMembers(std::uint32_t slot) const noexcept;
    void CancelReplacementSlot(std::uint32_t slot) noexcept;
    void AdvanceMutationEpoch() noexcept;
    bool ValidBatchIdentity(const SelectionPendingBatchIdentityV1& identity) const noexcept;
    bool MatchesBatchIdentity(const SelectionPendingBatchIdentityV1& identity,
        const SelectionPlayRequestV1& request) const noexcept;
    bool ReplacementRowCompatible(std::uint32_t rowIndex) const noexcept;
    SelectionMemoryStatusV1 CheckReplacementPlayOrder(std::uint32_t batchSlot,
        std::uint32_t rowIndex, const SelectionPlayRequestV1& request) const noexcept;
    SelectionMemoryStatusV1 CheckReplacementBatchOrder(std::uint32_t batchSlot) const noexcept;
    SelectionMemoryStatusV1 ComputeReplacementPlay(std::uint32_t batchSlot,
        std::uint32_t rowIndex, const SelectionPlayRequestV1& request,
        MemoryState& after, std::uint32_t* selected,
        SelectionSeedProbeCopyV1* seed) noexcept;
    SelectionMemoryStatusV1 PreparePlayImpl(
        const SelectionPlayRequestV1& request,
        PreparedSelectionPlayV1* out,
        std::uint32_t* outSelectedChildOrdinal,
        bool nativePendingPreparation) noexcept;

    static bool SameKey(const SelectionMemoryKeyV1& a,
                        const SelectionMemoryKeyV1& b) noexcept;
    bool ValidKey(const SelectionMemoryKeyV1& key) const noexcept;
    std::uint32_t FindRow(const SelectionMemoryKeyV1& key) const noexcept;
    std::uint32_t FindFreeRow(const SelectionMemoryKeyV1& key) const noexcept;
    std::uint32_t FindFreeDelta() const noexcept;
    bool PendingOrderValid(const Row& row, std::uint64_t ticket,
                           std::uint32_t eventIndex) const noexcept;
    bool CopyState(const MemoryState& source, MemoryState& target) noexcept;
    void InitializeState(const Row& row, MemoryState& state) noexcept;
    SelectionMemoryStatusV1 ComputePlay(
        const Row& row, const SelectionPlayRequestV1& request,
        const MemoryState& before, MemoryState& after,
        std::uint32_t* selectedChildOrdinal,
        SelectionSeedProbeCopyV1* seedProbe) noexcept;
    SelectionMemoryStatusV1 ComputeReset(const Row& row,
                                         const MemoryState& before,
                                         MemoryState& after) noexcept;
    const MemoryState& PreparedPredecessor(const Row& row,
        std::uint64_t executeFrame, std::uint64_t ticket,
        std::uint32_t eventIndex, std::uint32_t stopBefore = UINT32_MAX) const noexcept;
    SelectionMemoryStatusV1 PendingPredecessor(const Row& row,
        const MemoryState& before, const SelectionPlayRequestV1& request,
        const MemoryState** out, bool includePrivate = false) noexcept;
    static void MarkScheduledFrontier(MemoryState& state, const DeltaRow& reset) noexcept;
    static void ClearResetFrontiers(MemoryState& state) noexcept;
    static bool ResetConsumed(const MemoryState& state, const DeltaRow& reset) noexcept;
    static bool ResetAppliesBefore(const DeltaRow& reset, std::uint64_t frame,
        std::uint64_t ticket, std::uint32_t eventIndex) noexcept;
    std::uint32_t PrivateResetCount(const SelectionMemoryKeyV1& key) const noexcept;
    bool PrivateResetConflicts(const SelectionMemoryKeyV1& key,
        std::uint64_t ticket = 0) const noexcept;
    bool PendingResetHasHistory(const Row& row, const DeltaRow& reset) const noexcept;
    bool PendingResetBlocked(const Row& row, const DeltaRow& reset) const noexcept;
    void RefreshResetJournal(std::uint32_t rowIndex) noexcept;
    void AppendRecent(MemoryState& state, std::uint32_t child,
                      std::uint32_t avoidLastN) noexcept;
    void LinkDelta(Row& row, std::uint32_t rowIndex,
                   std::uint32_t deltaIndex) noexcept;
    void LinkScheduled(Row& row, std::uint32_t rowIndex,
                       std::uint32_t deltaIndex) noexcept;
    void UnlinkScheduledHead(Row& row, std::uint32_t deltaIndex) noexcept;
    void UnlinkScheduled(Row& row, std::uint32_t deltaIndex) noexcept;
    bool DropScheduledThrough(Row& row, std::uint64_t executeFrame,
                              std::uint64_t ticket,
                              std::uint32_t eventIndex) noexcept;
    void UnlinkHead(Row& row, std::uint32_t deltaIndex) noexcept;
    void UnlinkDelta(Row& row, std::uint32_t deltaIndex) noexcept;
    void RecycleDelta(std::uint32_t deltaIndex) noexcept;
    void ReleaseRow(std::uint32_t rowIndex) noexcept;
    void Recompute(std::uint32_t rowIndex) noexcept;
    void RecomputePrepared(std::uint32_t rowIndex) noexcept;
    const DeltaRow* ResolveDelta(SelectionDeltaRefV1 ref) const noexcept;
    DeltaRow* ResolveDelta(SelectionDeltaRefV1 ref) noexcept;

    SelectionMemoryConfigV1 config_{};
    std::vector<Row> rows_;
    std::vector<DeltaRow> deltas_;
    std::vector<BatchRow> batches_;
    MemoryState pendingScratch_[2];
    MemoryState replacementScratch_[2];
    std::vector<std::uint8_t> replacementAffectedRows_;
    struct BatchDependencyEdge { std::uint32_t before, after; };
    mutable std::vector<BatchDependencyEdge> replacementBatchEdges_;
    mutable std::vector<std::uint8_t> replacementBatchReachable_;
    mutable std::vector<std::uint32_t> pendingCommitHeads_;
    std::vector<SelectionChildV1> childScratch_;
    std::vector<std::uint32_t> remainingScratch_;
    std::uint32_t rowCount_ = 0;
    std::uint32_t pendingCount_ = 0;
    std::uint32_t quarantinedDeltaCount_ = 0;
    std::uint64_t tableLifetime_ = 0;
    std::uint64_t mutationEpoch_ = 1;
};

std::size_t SelectionSeedByteCountV1(
    const SelectionSeedInputV1& input) noexcept;
bool EncodeSelectionSeedV1(const SelectionSeedInputV1& input,
                           std::uint8_t* output,
                           std::size_t outputCapacity,
                           std::size_t* outputSize) noexcept;
bool ComputeSelectionSeedV1(const SelectionSeedInputV1& input,
                            std::uint64_t* output) noexcept;

SelectionStatusV1 SelectRandomDrawV1(
    const SelectionChildV1* children, std::uint32_t childCount,
    const std::uint32_t* recentChildOrdinals, std::uint32_t recentCount,
    SplitMix64* stream, std::uint32_t* selectedChildOrdinal) noexcept;

SelectionStatusV1 BuildRandomRotationV1(
    const SelectionChildV1* children, std::uint32_t childCount,
    const std::uint32_t* initialRecentChildOrdinals,
    std::uint32_t initialRecentCount, SplitMix64* stream,
    std::uint32_t* remainingIndexScratch, std::uint32_t scratchCapacity,
    std::uint32_t* outputChildOrdinals,
    std::uint32_t outputCapacity) noexcept;

SelectionStatusV1 SelectSequenceV1(
    const SelectionChildV1* children, std::uint32_t childCount,
    std::uint32_t cursor, bool alreadyStopped,
    SequenceEndPolicyV1 endPolicy, SequenceSelectionV1* output) noexcept;

}

#endif
