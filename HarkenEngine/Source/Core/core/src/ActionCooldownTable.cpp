#include "harken/core/ActionCooldownTable.h"

#include <algorithm>
#include <new>

namespace harken {

bool ActionDefinitionOwnerStamp::Valid() const noexcept {
    if (revision == 0) {
        return false;
    }
    if (kind == ActionDefinitionOwnerKind::World) {
        return stableId == 0 && slotGeneration == 0 && partitionRevision == 0;
    }
    return kind == ActionDefinitionOwnerKind::Pack && stableId != 0 &&
           slotGeneration != 0 && partitionRevision != 0;
}

bool operator==(const ActionDefinitionOwnerStamp& left,
                const ActionDefinitionOwnerStamp& right) noexcept {
    return left.kind == right.kind && left.stableId == right.stableId &&
           left.revision == right.revision &&
           left.slotGeneration == right.slotGeneration &&
           left.partitionRevision == right.partitionRevision;
}

bool ActionCooldownKey::Valid() const noexcept {
    if (!owner.Valid() || actionStableId == VnObjectId{} ||
        programSemanticFingerprint == 0) {
        return false;
    }
    if (scope == ActionCooldownScopeV1::Global) {
        return emitter == 0;
    }
    return scope == ActionCooldownScopeV1::PerEmitter && emitter != 0;
}

bool operator==(const ActionCooldownKey& left,
                const ActionCooldownKey& right) noexcept {
    return left.owner == right.owner &&
           left.actionStableId == right.actionStableId &&
           left.programSemanticFingerprint ==
               right.programSemanticFingerprint &&
           left.emitter == right.emitter &&
           left.authoredEventIndex == right.authoredEventIndex &&
           left.scope == right.scope;
}

bool ActionCooldownTable::Reset(std::uint32_t capacity) {
    if (capacity == 0 || capacity > kMaxActionCooldownRowsHard) {
        return false;
    }
    try {
        std::vector<Row> rows(capacity);
        rows_ = std::move(rows);
    } catch (const std::bad_alloc&) {
        return false;
    }
    liveCount_ = 0;
    preparedCount_ = 0;
    highWater_ = 0;
    quarantinedCount_ = 0;
    refusalCount_ = 0;
    return true;
}

void ActionCooldownTable::NoteOccupancy() noexcept {
    highWater_ = std::max(highWater_, liveCount_ + preparedCount_);
}

ActionCooldownTable::Row* ActionCooldownTable::Resolve(
    ActionCooldownToken token, State expected) noexcept {
    if (!token.Valid() || token.slot >= rows_.size()) {
        return nullptr;
    }
    Row& row = rows_[token.slot];
    return row.state == expected && row.generation == token.generation
               ? &row
               : nullptr;
}

bool ActionCooldownTable::AbandonPrepared(Row& row) noexcept {
    if (row.state != State::Prepared) {
        return false;
    }
    --preparedCount_;
    row.state = State::Free;
    row.key = {};
    row.endExclusive = 0;
    return true;
}

bool ActionCooldownTable::RecycleLive(Row& row) noexcept {
    if (row.state != State::Live) {
        return false;
    }
    --liveCount_;
    row.key = {};
    row.endExclusive = 0;
    if (row.generation == UINT64_MAX) {
        row.state = State::Quarantined;
        ++quarantinedCount_;
    } else {
        ++row.generation;
        row.state = State::Free;
    }
    return true;
}

std::uint32_t ActionCooldownTable::SweepExpired(
    std::uint64_t nowFrame) noexcept {
    std::uint32_t removed = 0;
    for (Row& row : rows_) {
        if (row.state == State::Live && nowFrame >= row.endExclusive) {
            removed += RecycleLive(row) ? 1u : 0u;
        }
    }
    return removed;
}

ActionCooldownPrepareResult ActionCooldownTable::Prepare(
    const ActionCooldownKey& key, std::uint64_t nowFrame,
    std::uint64_t endExclusive) noexcept {
    if (!key.Valid() || endExclusive <= nowFrame) {
        return {ActionCooldownPrepareStatus::Invalid, {}};
    }
    (void)SweepExpired(nowFrame);
    for (const Row& row : rows_) {
        if ((row.state == State::Prepared || row.state == State::Live) &&
            row.key == key && nowFrame < row.endExclusive) {
            return {ActionCooldownPrepareStatus::Suppressed, {}};
        }
    }
    for (std::uint32_t slot = 0; slot < rows_.size(); ++slot) {
        Row& row = rows_[slot];
        if (row.state != State::Free) {
            continue;
        }
        row.state = State::Prepared;
        row.key = key;
        row.endExclusive = endExclusive;
        ++preparedCount_;
        return {ActionCooldownPrepareStatus::Prepared,
                {slot, row.generation}};
    }
    ++refusalCount_;
    return {ActionCooldownPrepareStatus::Capacity, {}};
}

bool ActionCooldownTable::Commit(ActionCooldownToken* token) noexcept {
    if (token == nullptr) {
        return false;
    }
    Row* row = Resolve(*token, State::Prepared);
    if (row == nullptr) {
        return false;
    }
    row->state = State::Live;
    --preparedCount_;
    ++liveCount_;
    NoteOccupancy();
    *token = {};
    return true;
}

bool ActionCooldownTable::Abandon(ActionCooldownToken* token) noexcept {
    if (token == nullptr) {
        return false;
    }
    Row* row = Resolve(*token, State::Prepared);
    if (row == nullptr) {
        return false;
    }
    const bool recycled = AbandonPrepared(*row);
    *token = {};
    return recycled;
}

std::uint32_t ActionCooldownTable::RemoveOwner(
    const ActionDefinitionOwnerStamp& owner) noexcept {
    if (!owner.Valid()) {
        return 0;
    }
    std::uint32_t removed = 0;
    for (Row& row : rows_) {
        if (row.key.owner != owner) {
            continue;
        }
        if (row.state == State::Prepared) {
            removed += AbandonPrepared(row) ? 1u : 0u;
        } else if (row.state == State::Live) {
            removed += RecycleLive(row) ? 1u : 0u;
        }
    }
    return removed;
}

std::uint32_t ActionCooldownTable::RemoveEmitter(
    std::uint64_t emitter) noexcept {
    if (emitter == 0) {
        return 0;
    }
    std::uint32_t removed = 0;
    for (Row& row : rows_) {
        if (row.key.scope != ActionCooldownScopeV1::PerEmitter ||
            row.key.emitter != emitter) {
            continue;
        }
        if (row.state == State::Prepared) {
            removed += AbandonPrepared(row) ? 1u : 0u;
        } else if (row.state == State::Live) {
            removed += RecycleLive(row) ? 1u : 0u;
        }
    }
    return removed;
}

bool ActionCooldownTable::CanClearAll() const noexcept {
    return std::none_of(rows_.begin(), rows_.end(), [](const Row& row) {
        return row.state == State::Live && row.generation == UINT64_MAX;
    });
}

bool ActionCooldownTable::ClearAll() noexcept {
    if (!CanClearAll()) {
        return false;
    }
    for (Row& row : rows_) {
        if (row.state == State::Prepared) {
            (void)AbandonPrepared(row);
        } else if (row.state == State::Live) {
            (void)RecycleLive(row);
        }
    }
    return true;
}

bool ActionCooldownTable::SetFreeGenerationForTests(
    std::uint32_t slot, std::uint64_t generation) noexcept {
    if (slot >= rows_.size() || generation == 0 ||
        rows_[slot].state != State::Free) {
        return false;
    }
    rows_[slot].generation = generation;
    return true;
}

} // namespace harken
