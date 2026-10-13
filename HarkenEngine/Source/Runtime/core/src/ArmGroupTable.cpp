#include "harken/core/ArmGroupTable.h"

#include <limits>

namespace harken {
namespace {

std::uint64_t PackIdentity(std::uint32_t slot,
                           std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

bool SameArm(TimelineArmToken left, TimelineArmToken right) noexcept {
    return left.slot == right.slot && left.generation == right.generation &&
           left.renderSlot == right.renderSlot &&
           left.renderGeneration == right.renderGeneration;
}

bool ValidConfig(std::uint32_t capacity,
                 std::uint32_t maxMembers) noexcept {
    return capacity != 0 && capacity <= kMaxArmGroupsHard &&
           maxMembers != 0 && maxMembers <= kMaxArmsPerGroupHard;
}

} // namespace

ArmGroupTable::ArmGroupTable(std::uint32_t capacity,
                             std::uint32_t maxMembersPerGroup)
    : rows_(ValidConfig(capacity, maxMembersPerGroup)
                ? std::make_unique<Row[]>(capacity)
                : nullptr),
      members_(!ValidConfig(capacity, maxMembersPerGroup)
                   ? nullptr
                   : std::make_unique<MemberRow[]>(
                         static_cast<std::size_t>(capacity) *
                         maxMembersPerGroup)),
      capacity_(ValidConfig(capacity, maxMembersPerGroup) ? capacity : 0),
      maxMembersPerGroup_(ValidConfig(capacity, maxMembersPerGroup)
                              ? static_cast<std::uint16_t>(maxMembersPerGroup)
                              : 0) {}

ArmGroupTable::MemberRow* ArmGroupTable::Members(
    std::uint32_t slot) noexcept {
    return members_.get() +
           static_cast<std::size_t>(slot) * maxMembersPerGroup_;
}

const ArmGroupTable::MemberRow* ArmGroupTable::Members(
    std::uint32_t slot) const noexcept {
    return const_cast<ArmGroupTable*>(this)->Members(slot);
}

ArmGroupTable::Row* ArmGroupTable::Resolve(ArmGroupToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Live ||
        row.identity.load(std::memory_order_acquire) !=
            PackIdentity(token.slot, token.generation)) {
        return nullptr;
    }
    return &row;
}

const ArmGroupTable::Row* ArmGroupTable::Resolve(
    ArmGroupToken token) const noexcept {
    return const_cast<ArmGroupTable*>(this)->Resolve(token);
}

ArmGroupTable::MemberRow* ArmGroupTable::ResolveMember(
    std::uint32_t slot, TimelineArmToken member) noexcept {
    MemberRow* rows = Members(slot);
    for (std::uint16_t index = 0; index < rows_[slot].memberCount; ++index) {
        if (SameArm(rows[index].token, member)) {
            return &rows[index];
        }
    }
    return nullptr;
}

std::uint32_t ArmGroupTable::LiveCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Live) {
            ++count;
        }
    }
    return count;
}

std::uint32_t ArmGroupTable::FreeCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Free) {
            ++count;
        }
    }
    return count;
}

std::uint32_t ArmGroupTable::QuarantinedCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Quarantined) {
            ++count;
        }
    }
    return count;
}

bool ArmGroupTable::Reserve(DecisionToken parent,
                            const TimelineArmToken* members,
                            std::uint16_t memberCount,
                            ArmGroupToken* outToken) noexcept {
    return ReserveInternal(parent, members, memberCount, outToken, false);
}

bool ArmGroupTable::Prepare(DecisionToken parent,
                            const TimelineArmToken* members,
                            std::uint16_t memberCount,
                            ArmGroupToken* outToken) noexcept {
    return ReserveInternal(parent, members, memberCount, outToken, true);
}

bool ArmGroupTable::ReserveInternal(DecisionToken parent,
                                    const TimelineArmToken* members,
                                    std::uint16_t memberCount,
                                    ArmGroupToken* outToken,
                                    bool prepared) noexcept {
    if (!parent.Valid() || (memberCount != 0 && members == nullptr) ||
        memberCount > maxMembersPerGroup_ || outToken == nullptr) {
        return false;
    }
    for (std::uint16_t index = 0; index < memberCount; ++index) {
        if (!members[index].Valid()) {
            return false;
        }
        for (std::uint16_t prior = 0; prior < index; ++prior) {
            if (SameArm(members[index], members[prior])) {
                return false;
            }
        }
    }
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Free ||
            row.reservation.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        SlotState expected = SlotState::Free;
        row.parent = parent;
        row.memberCount = memberCount;
        row.ackedCount.store(0, std::memory_order_relaxed);
        row.tombstonedCount.store(0, std::memory_order_relaxed);
        row.maxTombstoneSequence.store(0, std::memory_order_relaxed);
        row.state.store(memberCount == 0 ? ArmGroupState::Ready
                                         : ArmGroupState::AwaitingAcks,
                        std::memory_order_relaxed);
        MemberRow* destination = Members(slot);
        for (std::uint16_t index = 0; index < memberCount; ++index) {
            destination[index].token = members[index];
            destination[index].acked.store(0, std::memory_order_relaxed);
            destination[index].tombstoned.store(0,
                                                std::memory_order_relaxed);
        }
        row.identity.store(PackIdentity(slot, row.generation),
                           std::memory_order_release);
        if (row.slotState.compare_exchange_strong(
                expected,
                prepared ? SlotState::Prepared : SlotState::Live,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            row.reservation.clear(std::memory_order_release);
            *outToken = {slot, row.generation};
            return true;
        }
        row.identity.store(0, std::memory_order_release);
        row.reservation.clear(std::memory_order_release);
    }
    return false;
}

ArmGroupTable::Row* ArmGroupTable::ResolvePrepared(
    ArmGroupToken token) noexcept {
    if (!token.Valid() || token.slot >= capacity_) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Prepared ||
        row.identity.load(std::memory_order_acquire) !=
            PackIdentity(token.slot, token.generation)) {
        return nullptr;
    }
    return &row;
}

bool ArmGroupTable::CommitPrepared(ArmGroupToken token) noexcept {
    Row* row = ResolvePrepared(token);
    if (row == nullptr) {
        return false;
    }
    SlotState expected = SlotState::Prepared;
    return row->slotState.compare_exchange_strong(
        expected, SlotState::Live, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool ArmGroupTable::CancelPrepared(ArmGroupToken token) noexcept {
    Row* row = ResolvePrepared(token);
    if (row == nullptr) {
        return false;
    }
    SlotState expected = SlotState::Prepared;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    row->identity.store(0, std::memory_order_release);
    row->parent = {};
    row->memberCount = 0;
    row->ackedCount.store(0, std::memory_order_relaxed);
    row->tombstonedCount.store(0, std::memory_order_relaxed);
    row->maxTombstoneSequence.store(0, std::memory_order_relaxed);
    row->state.store(ArmGroupState::AwaitingAcks,
                     std::memory_order_relaxed);
    if (row->generation == UINT32_MAX) {
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
    } else {
        ++row->generation;
        row->slotState.store(SlotState::Free, std::memory_order_release);
    }
    return true;
}

bool ArmGroupTable::Acknowledge(ArmGroupToken token,
                                TimelineArmToken member,
                                std::uint64_t) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            ArmGroupState::AwaitingAcks) {
        return false;
    }
    MemberRow* memberRow = ResolveMember(token.slot, member);
    std::uint8_t expected = 0;
    if (memberRow == nullptr || !memberRow->acked.compare_exchange_strong(
                                    expected, 1, std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
        return false;
    }
    const std::uint16_t count = static_cast<std::uint16_t>(
        row->ackedCount.fetch_add(1, std::memory_order_acq_rel) + 1);
    if (count == row->memberCount) {
        ArmGroupState awaiting = ArmGroupState::AwaitingAcks;
        (void)row->state.compare_exchange_strong(
            awaiting, ArmGroupState::Ready, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    return true;
}

bool ArmGroupTable::CancelBeforePublication(
    ArmGroupToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->ackedCount.load(std::memory_order_acquire) != 0 ||
        row->tombstonedCount.load(std::memory_order_acquire) != 0) {
        return false;
    }
    ArmGroupState state = row->state.load(std::memory_order_acquire);
    if (state != ArmGroupState::AwaitingAcks &&
        state != ArmGroupState::Ready) {
        return false;
    }
    if (!row->state.compare_exchange_strong(
            state, ArmGroupState::Retiring, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    SlotState expected = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        row->state.store(state, std::memory_order_release);
        return false;
    }
    row->identity.store(0, std::memory_order_release);
    if (row->generation == std::numeric_limits<std::uint32_t>::max()) {
        row->state.store(ArmGroupState::Quarantined,
                         std::memory_order_release);
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        return false;
    }
    ++row->generation;
    row->parent = {};
    row->memberCount = 0;
    row->ackedCount.store(0, std::memory_order_relaxed);
    row->tombstonedCount.store(0, std::memory_order_relaxed);
    row->maxTombstoneSequence.store(0, std::memory_order_relaxed);
    row->state.store(ArmGroupState::AwaitingAcks,
                     std::memory_order_relaxed);
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool ArmGroupTable::BeginRetire(ArmGroupToken token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr) {
        return false;
    }
    ArmGroupState state = row->state.load(std::memory_order_acquire);
    while (state == ArmGroupState::AwaitingAcks ||
           state == ArmGroupState::Ready) {
        if (row->state.compare_exchange_weak(
                state, ArmGroupState::Retiring, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool ArmGroupTable::Tombstone(ArmGroupToken token,
                              TimelineArmToken member,
                              std::uint64_t audioSequence) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || audioSequence == 0 ||
        row->state.load(std::memory_order_acquire) !=
            ArmGroupState::Retiring) {
        return false;
    }
    MemberRow* memberRow = ResolveMember(token.slot, member);
    std::uint8_t expected = 0;
    if (memberRow == nullptr ||
        !memberRow->tombstoned.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    std::uint64_t current =
        row->maxTombstoneSequence.load(std::memory_order_acquire);
    while (current < audioSequence &&
           !row->maxTombstoneSequence.compare_exchange_weak(
               current, audioSequence, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
    row->tombstonedCount.fetch_add(1, std::memory_order_release);
    return true;
}

bool ArmGroupTable::HasPublishedMemberTombstone(
    ArmGroupToken token, DecisionToken parent, TimelineArmToken member,
    std::uint64_t completedAudioSequence) const noexcept {
    ArmGroupSnapshot group{};
    if (!parent.Valid() || !member.Valid() || !Capture(token, &group) ||
        group.parent.slot != parent.slot || group.parent.generation != parent.generation ||
        group.state != ArmGroupState::Retiring || group.maxTombstoneSequence == 0 ||
        completedAudioSequence < group.maxTombstoneSequence) return false;
    for (std::uint16_t index = 0; index < group.memberCount; ++index) {
        ArmGroupMemberSnapshot candidate{};
        if (CaptureMember(token, index, &candidate) && candidate.tombstoned &&
            candidate.token.slot == member.slot && candidate.token.generation == member.generation &&
            candidate.token.renderSlot == member.renderSlot &&
            candidate.token.renderGeneration == member.renderGeneration) return true;
    }
    return false;
}

bool ArmGroupTable::Reclaim(ArmGroupToken token,
                            std::uint64_t observedAudioSequence) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->state.load(std::memory_order_acquire) !=
            ArmGroupState::Retiring ||
        row->tombstonedCount.load(std::memory_order_acquire) !=
            row->memberCount ||
        observedAudioSequence <=
            row->maxTombstoneSequence.load(std::memory_order_acquire)) {
        return false;
    }
    SlotState expected = SlotState::Live;
    if (!row->slotState.compare_exchange_strong(
            expected, SlotState::Reclaiming, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    row->identity.store(0, std::memory_order_release);
    if (row->generation == std::numeric_limits<std::uint32_t>::max()) {
        row->state.store(ArmGroupState::Quarantined,
                         std::memory_order_release);
        row->slotState.store(SlotState::Quarantined,
                             std::memory_order_release);
        return false;
    }
    ++row->generation;
    row->slotState.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool ArmGroupTable::CaptureMember(ArmGroupToken token, std::uint16_t index,
                                  ArmGroupMemberSnapshot* out) const noexcept {
    const Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || index >= row->memberCount) return false;
    const MemberRow& member = Members(token.slot)[index];
    ArmGroupMemberSnapshot snapshot{};
    snapshot.token = member.token;
    snapshot.acked = member.acked.load(std::memory_order_acquire) != 0;
    snapshot.tombstoned = member.tombstoned.load(std::memory_order_acquire) != 0;
    *out = snapshot;
    return true;
}

bool ArmGroupTable::Capture(ArmGroupToken token,
                            ArmGroupSnapshot* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    const Row* row = Resolve(token);
    if (row == nullptr) {
        return false;
    }
    ArmGroupSnapshot snapshot{};
    snapshot.state = row->state.load(std::memory_order_acquire);
    snapshot.parent = row->parent;
    snapshot.memberCount = row->memberCount;
    snapshot.ackedCount = row->ackedCount.load(std::memory_order_acquire);
    snapshot.tombstonedCount =
        row->tombstonedCount.load(std::memory_order_acquire);
    snapshot.maxTombstoneSequence =
        row->maxTombstoneSequence.load(std::memory_order_acquire);
    *out = snapshot;
    return true;
}

} // namespace harken
