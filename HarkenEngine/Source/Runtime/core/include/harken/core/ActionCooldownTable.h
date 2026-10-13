#ifndef HARKEN_CORE_ACTION_COOLDOWN_TABLE_H
#define HARKEN_CORE_ACTION_COOLDOWN_TABLE_H

#include <harken/ObjectId.h>
#include "harken/core/CurrentWorld.h"

#include <cstdint>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kMaxActionCooldownRowsHard = 4096;

enum class ActionDefinitionOwnerKind : std::uint8_t {
    World = 1,
    Pack = 2,
};

struct ActionDefinitionOwnerStamp {
    ActionDefinitionOwnerKind kind = ActionDefinitionOwnerKind::World;
    std::uint64_t stableId = 0;
    std::uint64_t revision = 0;
    std::uint32_t slotGeneration = 0;
    std::uint64_t partitionRevision = 0;

    bool Valid() const noexcept;
};

bool operator==(const ActionDefinitionOwnerStamp& left,
                const ActionDefinitionOwnerStamp& right) noexcept;

struct ActionCooldownKey {
    ActionDefinitionOwnerStamp owner{};
    VnObjectId actionStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t emitter = 0;
    std::uint32_t authoredEventIndex = 0;
    ActionCooldownScopeV1 scope = ActionCooldownScopeV1::Global;

    bool Valid() const noexcept;
};

bool operator==(const ActionCooldownKey& left,
                const ActionCooldownKey& right) noexcept;

struct ActionCooldownToken {
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t generation = 0;

    bool Valid() const noexcept {
        return slot != UINT32_MAX && generation != 0;
    }
};

enum class ActionCooldownPrepareStatus : std::uint8_t {
    Prepared = 0,
    Suppressed = 1,
    Capacity = 2,
    Invalid = 3,
};

struct ActionCooldownPrepareResult {
    ActionCooldownPrepareStatus status = ActionCooldownPrepareStatus::Invalid;
    ActionCooldownToken token{};
};

class ActionCooldownTable {
public:
    bool Reset(std::uint32_t capacity);

    ActionCooldownPrepareResult Prepare(const ActionCooldownKey& key,
                                        std::uint64_t nowFrame,
                                        std::uint64_t endExclusive) noexcept;
    bool Commit(ActionCooldownToken* token) noexcept;
    bool Abandon(ActionCooldownToken* token) noexcept;

    std::uint32_t SweepExpired(std::uint64_t nowFrame) noexcept;
    std::uint32_t RemoveOwner(
        const ActionDefinitionOwnerStamp& owner) noexcept;
    std::uint32_t RemoveEmitter(std::uint64_t emitter) noexcept;
    bool CanClearAll() const noexcept;
    bool ClearAll() noexcept;

    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(rows_.size());
    }
    std::uint32_t LiveCount() const noexcept { return liveCount_; }
    std::uint32_t PreparedCount() const noexcept { return preparedCount_; }
    std::uint32_t HighWater() const noexcept { return highWater_; }
    std::uint32_t QuarantinedCount() const noexcept {
        return quarantinedCount_;
    }
    std::uint64_t RefusalCount() const noexcept { return refusalCount_; }
    bool SetFreeGenerationForTests(std::uint32_t slot,
                                   std::uint64_t generation) noexcept;

private:
    enum class State : std::uint8_t {
        Free = 0,
        Prepared = 1,
        Live = 2,
        Quarantined = 3,
    };

    struct Row {
        State state = State::Free;
        std::uint64_t generation = 1;
        ActionCooldownKey key{};
        std::uint64_t endExclusive = 0;
    };

    Row* Resolve(ActionCooldownToken token, State expected) noexcept;
    bool AbandonPrepared(Row& row) noexcept;
    bool RecycleLive(Row& row) noexcept;
    void NoteOccupancy() noexcept;

    std::vector<Row> rows_;
    std::uint32_t liveCount_ = 0;
    std::uint32_t preparedCount_ = 0;
    std::uint32_t highWater_ = 0;
    std::uint32_t quarantinedCount_ = 0;
    std::uint64_t refusalCount_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_ACTION_COOLDOWN_TABLE_H
