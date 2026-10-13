#ifndef HARKEN_CORE_VARIABLE_SLICE_TABLE_H
#define HARKEN_CORE_VARIABLE_SLICE_TABLE_H

#include "harken/core/CommandQueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kVariableSliceRowHardLimit = 65536;
inline constexpr std::uint32_t kVariableSliceTargetHardLimit = 4096;
inline constexpr std::uint32_t kVariableSliceTargetPoolHardLimit = 65536;

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
    CommandRecord command{};
};

class VariableSliceTable {
public:
    using AfterAudioClaimHookForTest =
        void (*)(void*, VariableSliceRef) noexcept;

    VariableSliceTable() = default;

    bool Reset(std::uint32_t capacity, std::uint32_t maxTargetsPerSlice,
               std::uint32_t targetPoolCapacity);
    bool Reserve(std::uint64_t world, std::uint64_t transport,
                 std::uint64_t ticket, std::uint64_t effectiveFrame,
                 std::uint64_t bankOrdinal, std::uint32_t exactTargetCount,
                 VariableSliceRef* out) noexcept;
    bool AddTarget(VariableSliceRef ref, const VariableSliceTarget& target) noexcept;
    bool Publish(VariableSliceRef ref) noexcept;
    VariableSliceInvalidationResult Invalidate(
        VariableSliceRef ref, std::uint32_t reason) noexcept;
    bool Apply(VariableSliceRef ref, std::uint64_t blockSequence,
               void* context,
               bool (*validate)(void*, const VariableSliceTarget&) noexcept,
               void (*apply)(void*,
                             const VariableSliceTarget&) noexcept) noexcept;
    VariableSliceState State(VariableSliceRef ref) const noexcept;
    std::uint64_t AppliedBlockSequence(VariableSliceRef ref) const noexcept;
    std::uint32_t TargetCount(VariableSliceRef ref) const noexcept;
    VariableSliceTarget* Target(VariableSliceRef ref,
                                std::uint32_t index) noexcept;
    const VariableSliceTarget* Target(VariableSliceRef ref,
                                      std::uint32_t index) const noexcept;
    bool Reclaim(VariableSliceRef ref,
                 std::uint64_t observedBlockSequence) noexcept;
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
        std::uint64_t world = 0;
        std::uint64_t transport = 0;
        std::uint64_t ticket = 0;
        std::uint64_t effectiveFrame = 0;
        std::uint64_t bankOrdinal = 0;
        std::uint32_t targetSpanOffset = 0;
        std::uint32_t targetSpanCapacity = 0;
        std::uint32_t targetCount = 0;
        std::atomic<std::uint64_t> appliedBlockSequence{0};
        std::atomic<std::uint32_t> pendingInvalidationReasons{0};
    };

    struct FreeRange {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    Row* Resolve(VariableSliceRef ref) noexcept;
    const Row* Resolve(VariableSliceRef ref) const noexcept;
    bool ReleaseSpan(std::uint32_t offset, std::uint32_t count) noexcept;
    bool RecycleRow(Row& row, VariableSliceRef ref) noexcept;

    std::uint32_t capacity_ = 0;
    std::uint32_t maxTargetsPerSlice_ = 0;
    std::uint32_t targetPoolCapacity_ = 0;
    std::unique_ptr<Row[]> rows_;
    std::vector<VariableSliceTarget> targets_;
    std::vector<FreeRange> freeRanges_;
    std::atomic<AfterAudioClaimHookForTest> afterAudioClaimHookForTest_{
        nullptr};
    std::atomic<void*> afterAudioClaimHookContextForTest_{nullptr};
};

} // namespace harken

#endif // HARKEN_CORE_VARIABLE_SLICE_TABLE_H
