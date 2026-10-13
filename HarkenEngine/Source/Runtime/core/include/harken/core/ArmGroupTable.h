#ifndef HARKEN_CORE_ARM_GROUP_TABLE_H
#define HARKEN_CORE_ARM_GROUP_TABLE_H

#include "harken/core/DecisionToken.h"
#include "harken/core/TimelineArmTable.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace harken {

inline constexpr std::uint32_t kMaxArmGroupsHard = 16384;
inline constexpr std::uint16_t kMaxArmsPerGroupHard = 1024;

enum class ArmGroupState : std::uint8_t {
    AwaitingAcks = 0,
    Ready = 1,
    Retiring = 2,
    Quarantined = 3,
};

struct ArmGroupSnapshot {
    ArmGroupState state = ArmGroupState::AwaitingAcks;
    DecisionToken parent{};
    std::uint16_t memberCount = 0;
    std::uint16_t ackedCount = 0;
    std::uint16_t tombstonedCount = 0;
    std::uint64_t maxTombstoneSequence = 0;
};

struct ArmGroupMemberSnapshot {
    TimelineArmToken token{};
    bool acked = false;
    bool tombstoned = false;
};

class ArmGroupTable {
public:
    ArmGroupTable(std::uint32_t capacity,
                  std::uint32_t maxMembersPerGroup);

    std::uint32_t Capacity() const noexcept { return capacity_; }
    bool Valid() const noexcept {
        return capacity_ != 0 && maxMembersPerGroup_ != 0;
    }
    std::uint32_t LiveCount() const noexcept;
    std::uint32_t FreeCount() const noexcept;
    std::uint32_t QuarantinedCount() const noexcept;
    bool Reserve(DecisionToken parent, const TimelineArmToken* members,
                 std::uint16_t memberCount,
                 ArmGroupToken* outToken) noexcept;
    bool Prepare(DecisionToken parent, const TimelineArmToken* members,
                 std::uint16_t memberCount,
                 ArmGroupToken* outToken) noexcept;
    bool CommitPrepared(ArmGroupToken token) noexcept;
    bool CancelPrepared(ArmGroupToken token) noexcept;
    bool Acknowledge(ArmGroupToken token, TimelineArmToken member,
                     std::uint64_t completionSequence) noexcept;
    bool CancelBeforePublication(ArmGroupToken token) noexcept;
    bool BeginRetire(ArmGroupToken token) noexcept;
    bool Tombstone(ArmGroupToken token, TimelineArmToken member,
                   std::uint64_t audioSequence) noexcept;
    bool Reclaim(ArmGroupToken token,
                 std::uint64_t observedAudioSequence) noexcept;
    bool Capture(ArmGroupToken token, ArmGroupSnapshot* out) const noexcept;
    bool CaptureMember(ArmGroupToken token, std::uint16_t index,
                       ArmGroupMemberSnapshot* out) const noexcept;
    // Producer-only: serialize this query with Tombstone and group retirement.
    bool HasPublishedMemberTombstone(ArmGroupToken group, DecisionToken parent,
                                     TimelineArmToken member,
                                     std::uint64_t completedAudioSequence) const noexcept;

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
        DecisionToken parent{};
        std::uint16_t memberCount = 0;
        std::atomic<std::uint16_t> ackedCount{0};
        std::atomic<std::uint16_t> tombstonedCount{0};
        std::atomic<std::uint64_t> maxTombstoneSequence{0};
        std::atomic<ArmGroupState> state{ArmGroupState::AwaitingAcks};
    };

    struct MemberRow {
        TimelineArmToken token{};
        std::atomic<std::uint8_t> acked{0};
        std::atomic<std::uint8_t> tombstoned{0};
    };

    Row* Resolve(ArmGroupToken token) noexcept;
    const Row* Resolve(ArmGroupToken token) const noexcept;
    Row* ResolvePrepared(ArmGroupToken token) noexcept;
    bool ReserveInternal(DecisionToken parent,
                         const TimelineArmToken* members,
                         std::uint16_t memberCount,
                         ArmGroupToken* outToken,
                         bool prepared) noexcept;
    MemberRow* Members(std::uint32_t slot) noexcept;
    const MemberRow* Members(std::uint32_t slot) const noexcept;
    MemberRow* ResolveMember(std::uint32_t slot,
                             TimelineArmToken member) noexcept;

    std::unique_ptr<Row[]> rows_;
    std::unique_ptr<MemberRow[]> members_;
    std::uint32_t capacity_ = 0;
    std::uint16_t maxMembersPerGroup_ = 0;
};

static_assert(std::atomic<ArmGroupState>::is_always_lock_free);

} // namespace harken

#endif // HARKEN_CORE_ARM_GROUP_TABLE_H
