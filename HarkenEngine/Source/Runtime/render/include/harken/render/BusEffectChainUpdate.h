#pragma once

#include "harken/render/EffectChainUpdate.h"
#include "harken/core/BusSlotRef.h"

namespace harken {

// A stable row is allocated on the logic thread when a Preview owner first
// needs this physical Bus slot. Accepted fields are logic-only; audio borrows
// update after the topology command publishes it, until Complete.
struct BusEffectChainState {
    BusSlotRef acceptedHost{};
    std::uint64_t owner = 0;
    std::uint64_t revision = 0;
    std::array<PreviewEffectChainEntry, kMaxFxSlotsPerBus> accepted{};
    EffectChainUpdate update;
    std::uint64_t preparedOwner = 0;
};

struct BusEffectChainAdoption {
    BusEffectChainState* state = nullptr;
    std::uint64_t host = 0;
    std::uint64_t owner = 0;
    std::uint64_t revision = 0;
};

// A preparation also records no-op rows: a direct edit after prepare must
// invalidate that project even if it originally contained no chain changes.
struct PreparedBusEffectChain {
    BusEffectChainState* state = nullptr;
    BusSlotRef host{};
    std::uint64_t expectedOwner = 0;
    std::uint64_t expectedRevision = 0;
    bool changes = false;
};

} // namespace harken
