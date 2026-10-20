#ifndef HARKEN_CORE_VARIABLE_SLICE_TABLE_H
#define HARKEN_CORE_VARIABLE_SLICE_TABLE_H

#include "harken/core/CommandQueue.h"
#include "harken/core/VariableReadCut.h"

#include "harken/core/StableRowStorage.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kVariableSliceRowHardLimit = UINT32_MAX;
inline constexpr std::uint32_t kVariableSliceTargetHardLimit = 4096;
inline constexpr std::uint32_t kVariableSliceTargetPoolHardLimit = 65536;
inline constexpr std::uint32_t kVariableSliceSourceRejected = 1u << 30;
inline constexpr std::uint32_t kVariableSliceTargetRejected = 1u << 29;

enum class VariableSliceState : std::uint8_t {
    Free = 0,
    Prepared = 1,
    PageCommitted = 2,
    AudioClaimed = 3,
    SliceApplied = 4,
    Invalidated = 5,
    Quarantined = 6,
};

enum class VariableSliceInvalidationResult : std::uint8_t {
    InvalidReference = 0,
    InvalidatedBeforeClaim = 1,
    AudioClaimedApplyWhole = 2,
    AlreadyApplied = 3,
    AlreadyInvalidated = 4,
    NotPublished = 5,
};

enum class VariableTargetScope : std::uint8_t {
    Voice = 0,
    Bus = 1,
    VoiceEffect = 2,
    BusEffect = 3,
};

// Internal claim-release reason. RetryPreserve keeps the existing
// pending/prepared max merge. RejectCandidate restores the pre-candidate
// legal ramp request and must not run after the slice is published.
enum class ClaimReleaseReason : std::uint8_t {
    RetryPreserve = 0,
    RejectCandidate = 1,
};

struct VariableSliceRef {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

struct VariableSliceSource {
    VariableSliceRef slice{};
    std::uint64_t world = 0;
    std::uint64_t transport = 0;
    std::uint64_t ticket = 0;
    std::uint64_t bankOrdinal = 0;
};

enum class TargetClaimKind : std::uint8_t {
    SingleSlice,
    VariablePublication,
};

struct TargetClaimOwner {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
    TargetClaimKind kind = TargetClaimKind::SingleSlice;

    constexpr TargetClaimOwner() = default;
    constexpr TargetClaimOwner(std::uint32_t slotValue, std::uint32_t generationValue,
                               TargetClaimKind kindValue = TargetClaimKind::SingleSlice)
        : slot(slotValue), generation(generationValue), kind(kindValue) {}
    constexpr TargetClaimOwner(VariableSliceRef slice)
        : slot(slice.slot), generation(slice.generation) {}
    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0 &&
            (kind == TargetClaimKind::SingleSlice || kind == TargetClaimKind::VariablePublication);
    }
    bool Shared() const noexcept { return kind == TargetClaimKind::VariablePublication; }
    bool operator==(TargetClaimOwner other) const noexcept {
        return slot == other.slot && generation == other.generation && kind == other.kind;
    }
    bool operator!=(TargetClaimOwner other) const noexcept { return !(*this == other); }
};

struct VariableTargetLocator {
    VariableTargetScope scope = VariableTargetScope::Voice;
    std::uint64_t object = 0;
    std::uint32_t objectGeneration = 0;
    std::uint32_t targetId = 0;
    std::uint32_t effectSlot = 0;
    std::uint32_t paramIndex = 0;
    std::uint32_t effectType = 0;
};

struct VariableSliceTarget {
    VariableTargetLocator locator{};
    std::uint32_t targetRevision = 0;
    float resolvedValue = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t carriedRampMs = 0;
    VariableNativeEffectIdentity nativeEffect{};
    VariableToneBandValues toneBand{};
    CommandRecord command{};
};

class VariableSliceTable {
public:
    enum class ReservationStatus : std::uint8_t {
        Ready, TargetLimit, ProofCapacity, PayloadCapacity, TargetSpan
    };
    using AfterAudioClaimHookForTest =
        void (*)(void*, VariableSliceRef) noexcept;

    VariableSliceTable() = default;

    bool Reset(std::uint32_t capacity, std::uint32_t maxTargetsPerSlice,
               std::uint32_t targetPoolCapacity,
               std::uint32_t payloadCapacity = 0);
    bool Reserve(std::uint64_t world, std::uint64_t transport,
                 std::uint64_t ticket, std::uint64_t effectiveFrame,
                 std::uint64_t bankOrdinal, std::uint32_t exactTargetCount,
                 VariableSliceRef* out) noexcept;
    bool AddTarget(VariableSliceRef ref, const VariableSliceTarget& target) noexcept;
    ReservationStatus ReservationAvailability(std::uint32_t exactTargetCount) const noexcept;
    bool CaptureSource(VariableSliceRef ref, VariableSliceSource* out) const noexcept;
    bool RetainSource(const VariableSliceSource& source) noexcept;
    bool ReleaseSource(const VariableSliceSource& source) noexcept;
    bool SourceApplied(const VariableSliceSource& source) const noexcept;
    bool AddSource(VariableSliceRef ref, const VariableSliceSource& source) noexcept;
    const VariableSliceSource* Source(VariableSliceRef ref, std::uint32_t index) const noexcept;
    const VariableSliceSource* NextSource(VariableSliceRef ref, std::uint32_t* cursor) const noexcept;
    bool ClearPreparedSources(VariableSliceRef ref) noexcept;
    bool SetPreparedEffectiveFrame(VariableSliceRef ref, std::uint64_t frame) noexcept;
    bool SetPreparedCommandFrontier(VariableSliceRef ref, std::uint64_t frontier) noexcept;
    std::uint64_t EffectiveFrame(VariableSliceRef ref) const noexcept;
    std::uint32_t SourceCount(VariableSliceRef ref) const noexcept;
    std::uint32_t SourceObserverCount(VariableSliceRef ref) const noexcept;
    std::uint32_t InvalidationReasons(VariableSliceRef ref) const noexcept;
    std::size_t StorageBytes() const noexcept;
    std::size_t RowStorageBytes() const noexcept { return rows_.RequestedBytes(); }
    std::size_t RowCommittedBytes() const noexcept { return rows_.CommittedBytes(); }
    std::size_t RowReservedBytes() const noexcept { return rows_.ReservedBytes(); }
    std::uint32_t ConstructedRowCount() const noexcept { return rows_.ConstructedCount(); }
    void SetRowCommitPageLimitForTests(std::size_t pages) noexcept {
        rows_.SetCommitPageLimitForTests(pages);
    }
    bool Publish(VariableSliceRef ref) noexcept;
    bool CanPublishPrepared(VariableSliceRef ref, bool allowPreparedSources = false) const noexcept;
    VariableSliceInvalidationResult Invalidate(
        VariableSliceRef ref, std::uint32_t reason) noexcept;
    bool Apply(VariableSliceRef ref, std::uint64_t blockSequence,
               void* context,
               bool (*validate)(void*, const VariableSliceTarget&) noexcept,
               void (*apply)(void*,
                             const VariableSliceTarget&) noexcept,
               std::uint64_t settledCommandFrontier = UINT64_MAX) noexcept;
    VariableSliceState State(VariableSliceRef ref) const noexcept;
    std::uint64_t AppliedBlockSequence(VariableSliceRef ref) const noexcept;
    std::uint32_t TargetCount(VariableSliceRef ref) const noexcept;
    VariableSliceTarget* Target(VariableSliceRef ref,
                                std::uint32_t index) noexcept;
    const VariableSliceTarget* Target(VariableSliceRef ref,
                                      std::uint32_t index) const noexcept;
    bool Reclaim(VariableSliceRef ref,
                 std::uint64_t observedBlockSequence) noexcept;
    bool Acknowledge(VariableSliceRef ref) noexcept;
    bool AbandonPrepared(VariableSliceRef ref) noexcept;
    void ReclaimSettled(std::uint64_t observedBlockSequence) noexcept;
    bool HasUnreclaimedWorldRows(std::uint64_t world) const noexcept;
    bool HasUnreclaimedTransportRows(std::uint64_t transport) const noexcept;
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t MaxTargetsPerSlice() const noexcept {
        return maxTargetsPerSlice_;
    }
    std::uint32_t TargetPoolCapacity() const noexcept {
        return targetPoolCapacity_;
    }
    std::uint32_t FreeTargetCapacity() const noexcept;
    std::uint32_t FreeRowCount() const noexcept;
    std::uint32_t PayloadCapacity() const noexcept { return payloadCapacity_; }
    std::uint32_t FreePayloadCount() const noexcept {
        return static_cast<std::uint32_t>(freePayloads_.size());
    }
    std::size_t FreeRangeCount() const noexcept { return freeRanges_.size(); }
    bool SetFreeRowGenerationForTest(std::uint32_t slot,
                                     std::uint32_t generation) noexcept;
    void SetAfterAudioClaimHookForTest(AfterAudioClaimHookForTest hook,
                                       void* context) noexcept {
        afterAudioClaimHookContextForTest_.store(context,
                                                 std::memory_order_relaxed);
        afterAudioClaimHookForTest_.store(hook, std::memory_order_release);
    }

private:
    struct Row {
        std::atomic<VariableSliceState> state{VariableSliceState::Free};
        std::atomic<std::uint64_t> identity{0};
        VariableSliceSource source{};
        std::uint64_t effectiveFrame = 0;
        std::uint64_t requiredCommandFrontier = 0;
        std::uint32_t payload = UINT32_MAX;
        std::uint32_t sourceObservers = 0;
        bool logicAcknowledged = false;
        std::atomic<std::uint64_t> appliedBlockSequence{0};
        std::atomic<std::uint32_t> pendingInvalidationReasons{0};
    };

    struct Payload {
        std::uint32_t targetSpanOffset = 0;
        std::uint32_t targetSpanCapacity = 0;
        std::uint32_t targetCount = 0;
        std::uint32_t sourceCount = 0;
    };

    struct FreeRange {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    Row* Resolve(VariableSliceRef ref) noexcept;
    bool PrepareRow(std::uint32_t slot) noexcept;
    const Row* Resolve(VariableSliceRef ref) const noexcept;
    bool ReleaseSpan(std::uint32_t offset, std::uint32_t count) noexcept;
    bool RecycleRow(Row& row, VariableSliceRef ref) noexcept;
    bool ReleasePayload(Row& row, VariableSliceRef ref) noexcept;
    Payload* ResolvePayload(Row* row) noexcept;
    const Payload* ResolvePayload(const Row* row) const noexcept;
    bool SourceIdentityMatches(const Row& row, const VariableSliceSource& source) const noexcept;

    std::uint32_t capacity_ = 0;
    std::uint32_t maxTargetsPerSlice_ = 0;
    std::uint32_t targetPoolCapacity_ = 0;
    StableRowStorage<Row> rows_;
    std::vector<Payload> payloads_;
    std::vector<std::uint32_t> freeRows_;
    std::vector<std::uint32_t> freePayloads_;
    std::vector<VariableSliceTarget> targets_;
    std::vector<std::uint64_t> sourceBits_;
    std::uint32_t sourceWords_ = 0;
    std::uint32_t payloadCapacity_ = 0;
    std::vector<FreeRange> freeRanges_;
    std::atomic<AfterAudioClaimHookForTest> afterAudioClaimHookForTest_{
        nullptr};
    std::atomic<void*> afterAudioClaimHookContextForTest_{nullptr};
};

} // namespace harken

#endif // HARKEN_CORE_VARIABLE_SLICE_TABLE_H
