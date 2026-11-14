#pragma once

#include "harken/render/EffectChainUpdate.h"
#include "harken/core/BusSlotRef.h"
#include <memory>

namespace harken {

// A stable row is allocated on the logic thread when a Preview owner first
// needs this physical Bus slot. Accepted fields are logic-only; audio borrows
// update after the topology command publishes it, until Complete. The stable
// phase remains readable after logic releases the completed payload.
struct BusEffectChainState {
    BusSlotRef acceptedHost{};
    std::uint64_t owner = 0;
    std::uint64_t revision = 0;
    std::array<PreviewEffectChainEntry, kMaxFxSlotsPerBus> accepted{};
    std::atomic<EffectChainUpdate::Phase> phase{EffectChainUpdate::Phase::Free};
    std::unique_ptr<EffectChainUpdate> update;
    BusEffectChainState* retiringNext = nullptr; // Audio owner only, cleared before Complete.
    std::uint64_t preparedOwner = 0;
};

struct BusEffectChainAdoption {
    BusEffectChainState* state = nullptr;
    std::uint64_t host = 0;
    std::uint64_t owner = 0;
    std::uint64_t revision = 0;
    bool releaseOwner = false;
};

// Logic preparation may clone a topology after its old adoption completed.
// Only this still-unpublished edit owns a readable preparation payload.
inline const EffectChainUpdate* PreparedBusEffectUpdate(const BusEffectChainAdoption& adoption) noexcept {
    const auto* state = adoption.state;
    if (state == nullptr || state->phase.load(std::memory_order_acquire) != EffectChainUpdate::Phase::Preparing)
        return nullptr;
    const auto* update = state->update.get();
    return update != nullptr && update->host == adoption.host &&
        update->targetRevision == adoption.revision && state->preparedOwner == adoption.owner ? update : nullptr;
}

// A preparation also records no-op rows: a direct edit after prepare must
// invalidate that project even if it originally contained no chain changes.
struct PreparedBusEffectChain {
    BusEffectChainState* state = nullptr;
    BusSlotRef host{};
    std::uint64_t expectedOwner = 0;
    std::uint64_t expectedRevision = 0;
    bool changes = false;
    bool releaseOwner = false;
};

} // namespace harken
