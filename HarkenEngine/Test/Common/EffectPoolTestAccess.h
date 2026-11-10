#pragma once
#include "harken/render/EffectPool.h"

namespace harken::detail {
struct EffectPoolTestAccess {
    static bool SeedFreeGeneration(EffectPoolBank& bank, const EffectInstanceRef& ref,
                                   std::uint64_t generation) {
        auto* slot = bank.SlotFor(ref.type);
        auto* row = slot == nullptr ? nullptr : slot->rows.TryAt(ref.poolIndex);
        if (row == nullptr || row->state.load(std::memory_order_acquire) !=
            EffectPoolBank::InstanceState::Free) return false;
        row->generation.store(generation, std::memory_order_relaxed);
        return true;
    }
    static std::uint32_t ReadyCount(const EffectPoolBank& bank, std::uint32_t type) {
        const auto* slot = bank.SlotFor(type);
        return slot == nullptr ? 0 : slot->readyCount;
    }
    static EffectRetireQueue& RetireLane(EffectPoolBank& bank, const EffectInstanceRef& ref) {
        return *bank.SlotFor(ref.type)->rows[ref.poolIndex].retireLane;
    }
};
} // namespace harken::detail
