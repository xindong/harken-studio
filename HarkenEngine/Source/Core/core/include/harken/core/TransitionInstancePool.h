#ifndef HARKEN_CORE_TRANSITION_INSTANCE_POOL_H
#define HARKEN_CORE_TRANSITION_INSTANCE_POOL_H

#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/FrontierActivation.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace harken {

inline constexpr std::uint32_t kMaxTransitionInstancesHard = 16384;
inline constexpr std::uint32_t kMaxTransitionInstanceGeneration =
    0x7fffffffu;

struct TransitionInstanceToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

struct TransitionInstanceCreateRequest {
    std::uint64_t audibleFrame = 0;
    std::uint64_t lockBlock = 0;
    MonitorLifecycleFacts monitorIdentity{};
};

enum class TransitionAudibleGateState : std::uint8_t {
    Replaceable = 0,
    Invalidated = 1,
    Locked = 2,
    Retired = 3,
};

struct TransitionInstanceSnapshot {
    TransitionAudibleGateState state =
        TransitionAudibleGateState::Replaceable;
    std::uint64_t audibleFrame = 0;
    std::uint64_t lockBlock = 0;
    std::uint64_t retiredAtAudioSequence = 0;
};

class ScheduledTransactionPool;
class ImmediateActionLane;
struct ImmediateLaneHandle;

class TransitionInstancePool {
    struct Row;
public:
    class PrivateTimingProof {
    public:
        PrivateTimingProof() = default;
        ~PrivateTimingProof() { Release(); }
        PrivateTimingProof(const PrivateTimingProof&) = delete;
        PrivateTimingProof& operator=(const PrivateTimingProof&) = delete;
        PrivateTimingProof(PrivateTimingProof&& other) noexcept;
        PrivateTimingProof& operator=(PrivateTimingProof&& other) noexcept;
        bool Valid() const noexcept { return row_ != nullptr; }
        void Release() noexcept;
    private:
        friend class TransitionInstancePool;
        Row* row_ = nullptr;
        std::uint64_t audibleFrame_ = 0;
        std::uint64_t lockBlock_ = 0;
    };

    bool ProvePrivateTimingBeforePublication(TransitionInstanceToken token,
        const ImmediateActionLane& lane, ImmediateLaneHandle handle,
        std::uint64_t root, std::uint64_t oldAudibleFrame,
        std::uint64_t oldLockBlock, std::uint64_t newAudibleFrame,
        std::uint64_t newLockBlock, PrivateTimingProof* out) noexcept;
    void ApplyPrivateTimingBeforePublication(const PrivateTimingProof& proof) noexcept;

    class FrontierProof {
    public:
        FrontierProof() = default;
        ~FrontierProof() { Release(); }
        FrontierProof(const FrontierProof&) = delete;
        FrontierProof& operator=(const FrontierProof&) = delete;
        bool Valid() const noexcept { return row_ != nullptr; }
        void Release() noexcept;
    private:
        friend class TransitionInstancePool;
        Row* row_ = nullptr;
        AudioSliceLease* lease_ = nullptr;
        bool locks_ = false;
    };
    explicit TransitionInstancePool(std::uint32_t capacity);

    bool Valid() const noexcept { return capacity_ != 0; }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t LiveCount() const noexcept;
    std::uint32_t FreeCount() const noexcept;
    std::uint32_t QuarantinedCount() const noexcept;

    bool Acquire(const TransitionInstanceCreateRequest& request,
                 TransitionInstanceToken* outToken) noexcept;
    bool PrepareAcquire(const TransitionInstanceCreateRequest& request,
                        TransitionInstanceToken* outToken) noexcept;
    bool CommitPreparedAcquire(TransitionInstanceToken token) noexcept;
    bool CancelPreparedAcquire(TransitionInstanceToken token) noexcept;
    bool CancelBeforePublication(TransitionInstanceToken token) noexcept;
    bool InvalidateReady(TransitionInstanceToken token) noexcept;
    bool CanLockFromAudioClaim(
        TransitionInstanceToken token) const noexcept;
    void LockFromClaimedAudioSlice(
        TransitionInstanceToken token) noexcept;
    bool BeginRetire(TransitionInstanceToken token,
                     std::uint64_t audioSequence) noexcept;
    bool Reclaim(TransitionInstanceToken token,
                 std::uint64_t observedAudioSequence) noexcept;
    bool Capture(TransitionInstanceToken token,
                 TransitionInstanceSnapshot* out) const noexcept;
    bool BindFrontier(TransitionInstanceToken token, const FrontierGroupBinding& binding,
                      const ScheduledTransactionPool& pool) noexcept;
    bool ProveFrontier(const FrontierGroupBinding& binding, AudioSliceLease& lease,
                       TransitionAudibleGateState expected, bool locks, FrontierProof* out) noexcept;
    void ApplyFrontier(const FrontierProof& proof) noexcept;
    bool ReleaseFrontierBinding(TransitionInstanceToken token) noexcept;
    bool SetGenerationForTests(std::uint32_t slot,
                               std::uint32_t generation) noexcept;

private:
    enum class SlotState : std::uint8_t {
        Free = 0,
        Prepared = 1,
        Live = 2,
        Reclaiming = 3,
        Quarantined = 4,
    };

    struct Row {
        std::atomic<SlotState> slotState{SlotState::Free};
        std::atomic<std::uint64_t> identity{0};
        std::atomic_flag reservation = ATOMIC_FLAG_INIT;
        std::uint32_t generation = 1;
        std::atomic<TransitionAudibleGateState> gateState{
            TransitionAudibleGateState::Replaceable};
        std::uint64_t audibleFrame = 0;
        std::uint64_t lockBlock = 0;
        MonitorLifecycleFacts monitorIdentity{};
        std::atomic<std::uint64_t> retiredAtAudioSequence{0};
        const ScheduledTransactionPool* frontierPool = nullptr;
        FrontierOwnerIdentity frontierOwner{};
        DecisionToken frontierDecision{};
    };

    Row* Resolve(TransitionInstanceToken token) noexcept;
    const Row* Resolve(TransitionInstanceToken token) const noexcept;
    Row* ResolvePrepared(TransitionInstanceToken token) noexcept;
    bool AcquireInternal(const TransitionInstanceCreateRequest& request,
                         TransitionInstanceToken* outToken,
                         bool prepared) noexcept;
    bool RecyclePrepared(Row& row) noexcept;
    bool MayMutateFrontier(Row& row) const noexcept;

    std::unique_ptr<Row[]> rows_;
    std::uint32_t capacity_ = 0;
};

} // namespace harken

static_assert(std::atomic<harken::TransitionAudibleGateState>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

#endif // HARKEN_CORE_TRANSITION_INSTANCE_POOL_H
