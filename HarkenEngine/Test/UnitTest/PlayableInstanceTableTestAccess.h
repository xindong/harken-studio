#pragma once

#include "harken/core/PlayableInstanceTable.h"
#include <cassert>

namespace harken::detail {

// Test-only access reaches malformed or terminal generation states without
// exposing production mutation seams.
struct PlayableInstanceTableTestAccess {
    static std::uint32_t OriginalLifetimeCount(const PlayableInstanceTable& table,
        const PlayableInstanceTable::OriginalLifetimePin& original) {
        return table.IsOriginalLifetimePinFor(original, original.OriginalHandle())
            ? table.m_entries[static_cast<std::uint32_t>(original.OriginalHandle())].originalLifetimeCount : 0;
    }
    static std::uint64_t OriginalLifetimeMarker(const PlayableInstanceTable& table,
        const PlayableInstanceTable::OriginalLifetimePin& original) {
        return table.IsOriginalLifetimePinFor(original, original.OriginalHandle())
            ? table.m_entries[static_cast<std::uint32_t>(original.OriginalHandle())].originalLifetimeMarker : 0;
    }
    // Negative-only scoped limit; never issues a marker or successful cap.
    static bool RefuseOriginalBorrow(PlayableInstanceTable& table,
        const PlayableInstanceTable::OriginalLifetimePin& original) {
        if (OriginalLifetimeCount(table, original) != 1) return false;
        table.m_entries[static_cast<std::uint32_t>(original.OriginalHandle())].originalLifetimeCount = UINT32_MAX;
        return true;
    }
    static void RestoreOriginalBorrow(PlayableInstanceTable& table,
        const PlayableInstanceTable::OriginalLifetimePin& original) {
        auto& entry = table.m_entries[static_cast<std::uint32_t>(original.OriginalHandle())];
        assert(table.IsOriginalLifetimePinFor(original, original.OriginalHandle()) && entry.originalLifetimeCount == UINT32_MAX);
        entry.originalLifetimeCount = 1;
    }
    static bool RefuseOriginalMarker(PlayableInstanceTable& table) {
        const auto before = table.m_originalLifetimeMarkerExhausted;
        table.m_originalLifetimeMarkerExhausted = true;
        return before;
    }
    static void RestoreOriginalMarker(PlayableInstanceTable& table, bool before) {
        table.m_originalLifetimeMarkerExhausted = before;
    }

    static void SeedNextScheduleEpoch(PlayableInstanceTable& table,
                                      std::uint64_t next) {
        table.m_nextScheduleEpoch = next;
        table.m_scheduleEpochExhausted = false;
    }

    static std::uint64_t PreparedCancellationCount(const PlayableInstanceTable& table) {
        return table.m_preparedCancellationCount;
    }

    static void SeedGeneration(PlayableInstanceTable& table,
                               std::uint32_t slot,
                               std::uint32_t generation) {
        auto& entry = table.m_entries.at(slot);
        entry.generation = generation;
        entry.quarantined = false;
        entry.lifecycle = PlayableLifecycle::Free;
        entry.parent = 0;
        entry.scheduleEpoch = 0;
    }

    static PlayableInstanceHandle Handle(std::uint32_t slot,
                                         std::uint32_t generation) {
        return PlayableInstanceTable::MakeHandle(slot, generation);
    }

    static bool SetParent(PlayableInstanceTable& table,
                          PlayableInstanceHandle handle,
                          PlayableInstanceHandle parent) {
        auto* entry = table.FindMut(handle);
        if (entry == nullptr) return false;
        entry->parent = parent;
        return true;
    }
};

} // namespace harken::detail
