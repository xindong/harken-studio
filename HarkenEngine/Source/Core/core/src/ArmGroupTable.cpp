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
    : capacity_(ValidConfig(capacity, maxMembersPerGroup) ? capacity : 0),
      maxMembersPerGroup_(ValidConfig(capacity, maxMembersPerGroup)
                              ? static_cast<std::uint16_t>(maxMembersPerGroup)
                              : 0) {
    if (!rows_.Reserve(capacity_)) throw std::bad_alloc{};
}

std::size_t ArmGroupTable::MemberStorageBytes() const noexcept {
    std::size_t bytes = preparedMemberBytes_;
    for (const auto& row : rows_) bytes += row.memberCapacity * sizeof(MemberRow);
    return bytes;
}

ArmGroupTable::PreparedMembers::~PreparedMembers() {
    if (owner != nullptr) owner->preparedMemberBytes_ -=
        sizeof(PreparedMembers) + static_cast<std::size_t>(capacity) * sizeof(MemberRow);
}

ArmGroupTable::BackingPreparation::~BackingPreparation() { Release(); }
ArmGroupTable::BackingPreparation::BackingPreparation(BackingPreparation&& other) noexcept {
    *this = std::move(other);
}
ArmGroupTable::BackingPreparation& ArmGroupTable::BackingPreparation::operator=(BackingPreparation&& other) noexcept {
    if (this != &other) {
        Release();
        rows_ = std::move(other.rows_);
        members_ = std::move(other.members_);
    }
    return *this;
}

void ArmGroupTable::BackingPreparation::Release() noexcept {
    for (auto& member : members_) {
        if (member != nullptr && member->inUse) {
            member->held = false;
            (void)member.release();
        }
    }
    members_.clear();
    rows_.Release();
}

ArmGroupTable::PreparedUse::PreparedUse(ArmGroupTable& owner, BackingPreparation* backing) noexcept
    : owner_(owner), previous_(owner.activeBacking_),
      rows_(backing == nullptr ? StableStoragePreparedUse{} : backing->rows_.Use()) {
    owner_.activeBacking_ = backing;
}
ArmGroupTable::PreparedUse::~PreparedUse() { owner_.activeBacking_ = previous_; }

bool ArmGroupTable::PrepareBacking(std::uint32_t groupCount, std::uint32_t totalMembers,
                                  BackingPreparation* out) noexcept {
    if (out == nullptr || out->Valid() || memguard::IsAudioTagged()) return false;
    groupCount = std::min(groupCount, capacity_);
    auto occupied = rows_.ConstructedCount();
    while (occupied != 0 && rows_[occupied - 1u].slotState.load(std::memory_order_acquire) == SlotState::Free)
        --occupied;
    BackingPreparation candidate;
    if (!rows_.PrepareBacking(groupCount, occupied, &candidate.rows_)) return false;
    try {
        const auto nonempty = std::min(groupCount, totalMembers);
        candidate.members_.reserve(nonempty);
        // Sorted group sizes obey size[i] <= totalMembers / (i + 1).
        for (std::uint32_t index = 0; index < nonempty; ++index) {
            const auto count = static_cast<std::uint16_t>(std::min<std::uint32_t>(
                maxMembersPerGroup_, totalMembers / (index + 1u)));
            if (count > memberAllocationLimitForTests_) return false;
            auto member = std::make_unique<PreparedMembers>();
            member->members = std::make_unique<MemberRow[]>(count);
            member->capacity = count;
            member->owner = this;
            preparedMemberBytes_ += sizeof(PreparedMembers) +
                static_cast<std::size_t>(count) * sizeof(MemberRow);
            candidate.members_.push_back(std::move(member));
        }
    } catch (...) { return false; }
    *out = std::move(candidate);
    return true;
}

void ArmGroupTable::ReturnPreparedMembers(Row& row) noexcept {
    auto* member = std::exchange(row.preparedMembers, nullptr);
    if (member == nullptr) return;
    member->inUse = false;
    if (!member->held) delete member;
}

ArmGroupTable::MemberRow* ArmGroupTable::Members(
    std::uint32_t slot) noexcept {
    const auto& row = rows_[slot];
    return row.preparedMembers != nullptr ? row.preparedMembers->members.get() : row.members.get();
}

const ArmGroupTable::MemberRow* ArmGroupTable::Members(
    std::uint32_t slot) const noexcept {
    return const_cast<ArmGroupTable*>(this)->Members(slot);
}

ArmGroupTable::Row* ArmGroupTable::Resolve(ArmGroupToken token) noexcept {
    if (!token.Valid() || token.slot >= rows_.ConstructedCount()) {
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
    for (std::uint32_t slot = 0, constructed = rows_.ConstructedCount(); slot < constructed; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Live) {
            ++count;
        }
    }
    return count;
}

std::uint32_t ArmGroupTable::FreeCount() const noexcept {
    const auto constructed = rows_.ConstructedCount();
    std::uint32_t count = capacity_ - constructed;
    for (std::uint32_t slot = 0; slot < constructed; ++slot) {
        if (rows_[slot].slotState.load(std::memory_order_acquire) ==
            SlotState::Free) {
            ++count;
        }
    }
    return count;
}

std::uint32_t ArmGroupTable::QuarantinedCount() const noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0, constructed = rows_.ConstructedCount(); slot < constructed; ++slot) {
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
    if (memguard::IsAudioTagged() || !parent.Valid() || (memberCount != 0 && members == nullptr) ||
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
        if (!rows_.ConstructThrough(slot + 1u)) return false;
        Row& row = rows_[slot];
        if (row.slotState.load(std::memory_order_acquire) != SlotState::Free ||
            row.reservation.test_and_set(std::memory_order_acquire)) {
            continue;
        }
        // Reuse only after the original group fence; published member addresses stay fixed.
        if (memberCount > row.memberCapacity) {
            if (activeBacking_ != nullptr) {
                PreparedMembers* best = nullptr;
                for (const auto& candidate : activeBacking_->members_) {
                    if (candidate == nullptr || candidate->inUse || candidate->capacity < memberCount) continue;
                    if (best == nullptr || candidate->capacity < best->capacity) best = candidate.get();
                }
                if (best == nullptr) {
                    row.reservation.clear(std::memory_order_release);
                    return false;
                }
                best->inUse = true;
                row.preparedMembers = best;
            } else {
                if (memberCount > memberAllocationLimitForTests_) {
                    row.reservation.clear(std::memory_order_release);
                    return false;
                }
                try {
                    auto candidate = std::make_unique<MemberRow[]>(memberCount);
                    row.members.swap(candidate);
                    row.memberCapacity = memberCount;
                } catch (...) {
                    row.reservation.clear(std::memory_order_release);
                    return false;
                }
            }
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
        ReturnPreparedMembers(row);
        row.reservation.clear(std::memory_order_release);
    }
    return false;
}

ArmGroupTable::Row* ArmGroupTable::ResolvePrepared(
    ArmGroupToken token) noexcept {
    return const_cast<Row*>(static_cast<const ArmGroupTable*>(this)->ResolvePrepared(token));
}

const ArmGroupTable::Row* ArmGroupTable::ResolvePrepared(
    ArmGroupToken token) const noexcept {
    if (!token.Valid() || token.slot >= rows_.ConstructedCount()) {
        return nullptr;
    }
    const Row& row = rows_[token.slot];
    if (row.slotState.load(std::memory_order_acquire) != SlotState::Prepared ||
        row.identity.load(std::memory_order_acquire) !=
            PackIdentity(token.slot, token.generation)) {
        return nullptr;
    }
    return &row;
}

bool ArmGroupTable::MatchesPreparedMembers(ArmGroupToken token, DecisionToken parent,
    const TimelineArmToken* members, std::uint16_t memberCount) const noexcept {
    const Row* row = ResolvePrepared(token);
    if (row == nullptr || !parent.Valid() || (memberCount != 0 && members == nullptr) ||
        row->parent.slot != parent.slot || row->parent.generation != parent.generation ||
        row->memberCount != memberCount ||
        row->state.load(std::memory_order_acquire) !=
            (memberCount == 0 ? ArmGroupState::Ready : ArmGroupState::AwaitingAcks) ||
        row->ackedCount.load(std::memory_order_acquire) != 0 ||
        row->tombstonedCount.load(std::memory_order_acquire) != 0 ||
        row->maxTombstoneSequence.load(std::memory_order_acquire) != 0) return false;
    const MemberRow* original = Members(token.slot);
    for (std::uint16_t index = 0; index < memberCount; ++index)
        if (!SameArm(original[index].token, members[index]) ||
            original[index].acked.load(std::memory_order_acquire) != 0 ||
            original[index].tombstoned.load(std::memory_order_acquire) != 0) return false;
    return true;
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
    if (memguard::IsAudioTagged()) return false;
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
    ReturnPreparedMembers(*row);
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
    if (memguard::IsAudioTagged()) return false;
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
    ReturnPreparedMembers(*row);
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
    if (memguard::IsAudioTagged()) return false;
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
    ReturnPreparedMembers(*row);
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
