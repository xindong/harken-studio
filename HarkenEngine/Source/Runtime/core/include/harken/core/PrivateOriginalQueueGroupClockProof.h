#ifndef HARKEN_CORE_PRIVATE_ORIGINAL_QUEUE_GROUP_CLOCK_PROOF_H
#define HARKEN_CORE_PRIVATE_ORIGINAL_QUEUE_GROUP_CLOCK_PROOF_H

#include "harken/core/ImmediateActionLane.h"

namespace harken {

// A short private exclusion, not a Queue/Current/ACK or publication authority.
class PrivateOriginalQueueGroupClockProof {
public:
    PrivateOriginalQueueGroupClockProof() noexcept = default;
    PrivateOriginalQueueGroupClockProof(const PrivateOriginalQueueGroupClockProof&) = delete;
    PrivateOriginalQueueGroupClockProof& operator=(const PrivateOriginalQueueGroupClockProof&) = delete;
    PrivateOriginalQueueGroupClockProof(PrivateOriginalQueueGroupClockProof&& other) noexcept;
    PrivateOriginalQueueGroupClockProof& operator=(PrivateOriginalQueueGroupClockProof&& other) noexcept;
    ~PrivateOriginalQueueGroupClockProof() { Release(); }
    bool Valid() const noexcept;
    bool CaptureAnchorFrame(std::uint64_t* out) const noexcept;
    bool ReturnPlacement(PrivateImmediateLanePlacementProof* out) noexcept;
    void Release() noexcept;

private:
    friend class ScheduledTransactionPool;
    void Take(PrivateOriginalQueueGroupClockProof& other) noexcept;
    ScheduledTransactionPool* owner_ = nullptr;
    PrivateImmediateLanePlacementProof placement_{};
    std::array<OriginalQueueFutureGroupLifetimePin, 2> originals_{};
    std::array<Plan03GroupSliceIdentity, 4> rows_{};
    std::array<AudioSliceLease*, 2> leases_{};
    std::uint32_t rowCount_ = 0;
    std::uint32_t leaseCount_ = 0;
    std::uint64_t root_ = 0;
    std::uint64_t anchor_ = 0;
};

} // namespace harken

#endif
