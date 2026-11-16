#ifndef HARKEN_CORE_ACTION_SEEK_TABLE_H
#define HARKEN_CORE_ACTION_SEEK_TABLE_H

#include <harken/Harken.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace harken {

inline constexpr std::uint32_t kMaxActionSeeksHard = 4096;
inline constexpr std::uint32_t kActionSeekSlotBits = 12;
inline constexpr std::uint64_t kMaxActionSeekGeneration =
    UINT64_MAX >> kActionSeekSlotBits;

enum class ActionSeekLifecycle : std::uint32_t {
    Free = 0,
    Prepared = 1,
    Accepted = 2,
    CancelRequested = 3,
    Applying = 4,
    Applied = 5,
    Cancelled = 6,
    Quarantined = 7,
};

class ActionSeekTable {
public:
    bool Reset(std::uint32_t capacity);
    bool Prepare(VnActionFiringHandle firing, std::uint32_t targetCount,
                 std::uint64_t acceptedLogicSequence,
                 VnActionSeekTicket* outTicket) noexcept;
    bool Commit(VnActionSeekTicket ticket) noexcept;
    bool Abandon(VnActionSeekTicket ticket) noexcept;
    bool HasOutstanding(VnActionFiringHandle firing) const noexcept;

    bool RequestCancel(VnActionSeekTicket ticket, VnResult result) noexcept;
    bool TryBeginApply(VnActionSeekTicket ticket) noexcept;
    bool FinishApplied(VnActionSeekTicket ticket,
                       std::uint64_t audioBlockSequence,
                       std::uint64_t minimumSourceFrame,
                       std::uint64_t maximumSourceFrame) noexcept;
    bool FinishCancelled(VnActionSeekTicket ticket, VnResult result) noexcept;
    bool FinishRequestedCancellation(VnActionSeekTicket ticket) noexcept;
    bool CancelAcceptedFromAudio(VnActionSeekTicket ticket,
                                 VnResult result) noexcept;

    VnResult Query(VnActionSeekTicket ticket,
                   VnActionSeekInfoV1* outInfo) const noexcept;
    VnResult Release(VnActionSeekTicket ticket) noexcept;
    void CancelAllAfterAudioJoin(VnResult result) noexcept;

    std::uint32_t Capacity() const noexcept { return capacity_; }

private:
    struct Row {
        std::atomic<std::uint32_t> lifecycle{
            static_cast<std::uint32_t>(ActionSeekLifecycle::Free)};
        std::uint64_t generation = 0;
        VnActionFiringHandle firing = 0;
        std::uint64_t acceptedLogicSequence = 0;
        std::uint64_t appliedAudioBlockSequence = 0;
        std::uint64_t minimumAppliedSourceFrame = 0;
        std::uint64_t maximumAppliedSourceFrame = 0;
        std::uint32_t targetCount = 0;
        std::uint32_t appliedCount = 0;
        std::uint32_t cancelledCount = 0;
        std::atomic<std::int32_t> requestedCancelResult{VN_OK};
        VnResult result = VN_OK;
    };

    static VnActionSeekTicket MakeTicket(std::uint32_t slot,
                                         std::uint64_t generation) noexcept;
    static std::uint32_t SlotOf(VnActionSeekTicket ticket) noexcept;
    static std::uint64_t GenerationOf(VnActionSeekTicket ticket) noexcept;
    Row* Resolve(VnActionSeekTicket ticket) noexcept;
    const Row* Resolve(VnActionSeekTicket ticket) const noexcept;
    bool Recycle(std::uint32_t slot, Row& row) noexcept;

    std::unique_ptr<Row[]> rows_;
    std::uint32_t capacity_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_ACTION_SEEK_TABLE_H
