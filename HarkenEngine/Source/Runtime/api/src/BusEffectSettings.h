#pragma once

#include "harken/render/EffectPool.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <span>
#include <vector>

namespace harken {

// Logic-owned identity survives DSP retirement. A chain shares its scope;
// genuinely mixed scopes use sparse exceptions without weakening ownership.
class BusEffectSettings {
public:
    struct Setting {
        std::uint32_t type = kEffectTypeNone;
        EffectObjectIdentity identity{};
        VnPackHandle owner = 0;
    };
    struct Write { std::uint32_t bus, slot; Setting value; };
    Setting Get(std::uint32_t bus, std::uint32_t slot) const noexcept {
        if (slot >= kMaxFxSlotsPerBus) return {};
        const auto found = Lower(bus);
        if (found == m_rows.end() || found->bus != bus || !(found->mask & (1u << slot))) return {};
        auto scope = found->common;
        const auto key = Key(bus, slot);
        const auto extra = Extra(key);
        if (extra != m_exceptions.end() && extra->key == key) scope = extra->scope;
        return {found->types[slot], {found->ids[slot], scope.project}, scope.owner};
    }
    bool Prepare(std::span<const Write> writes) noexcept {
        try {
            std::vector<Write> ordered;
            ordered.reserve(writes.size());
            for (const auto& write : writes) {
                if (write.value.type == kEffectTypeNone) continue;
                if (write.slot >= kMaxFxSlotsPerBus || !FindEffectRow(write.value.type)) return false;
                ordered.push_back(write);
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.bus < b.bus; });
            std::size_t groups = m_rows.size(), exceptions = m_exceptions.size();
            for (std::size_t begin = 0; begin < ordered.size();) {
                const auto bus = ordered[begin].bus;
                const auto found = Lower(bus);
                const bool existing = found != m_rows.end() && found->bus == bus;
                if (!existing) ++groups;
                const auto common = existing ? found->common : ScopeOf(ordered[begin].value);
                unsigned mask = existing ? found->mask : 0;
                bool mixed = false;
                if (existing) for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot)
                    if (mask & (1u << slot)) mixed |= ScopeOf(Get(bus, slot)) != common;
                auto end = begin;
                for (; end < ordered.size() && ordered[end].bus == bus; ++end) {
                    mask |= 1u << ordered[end].slot;
                    mixed |= ScopeOf(ordered[end].value) != common;
                }
                // Includes the stored common even when its original slot was cleared.
                // Clear/reinsert can choose any incoming scope; this union bound
                // covers every operation prefix without simulating the transaction.
                if (mixed) {
                    const auto extra = std::popcount(mask);
                    if (extra > m_exceptions.max_size() - exceptions) return false;
                    exceptions += extra;
                }
                begin = end;
            }
            m_rows.reserve(groups);
            m_exceptions.reserve(exceptions);
            return true;
        } catch (...) { return false; }
    }
    void Set(std::uint32_t bus, std::uint32_t slot, Setting value) noexcept {
        assert(slot < kMaxFxSlotsPerBus);
        auto found = Lower(bus);
        const auto key = Key(bus, slot);
        auto extra = Extra(key);
        if (value.type == kEffectTypeNone) {
            if (extra != m_exceptions.end() && extra->key == key) m_exceptions.erase(extra);
            if (found == m_rows.end() || found->bus != bus || !(found->mask & (1u << slot))) return;
            found->mask &= ~(1u << slot);
            --m_count;
            if (!found->mask) m_rows.erase(found);
            return;
        }
        assert(FindEffectRow(value.type));
        if (found == m_rows.end() || found->bus != bus) {
            RequireRoom(m_rows);
            Row row{}; row.bus = bus; row.common = ScopeOf(value);
            found = m_rows.insert(found, row);
        }
        const auto scope = ScopeOf(value);
        if (scope == found->common) {
            if (extra != m_exceptions.end() && extra->key == key) m_exceptions.erase(extra);
        } else if (extra != m_exceptions.end() && extra->key == key) extra->scope = scope;
        else { RequireRoom(m_exceptions); m_exceptions.insert(extra, {key, scope}); }
        if (!(found->mask & (1u << slot))) ++m_count;
        found->mask |= 1u << slot;
        found->types[slot] = static_cast<std::uint8_t>(value.type);
        found->ids[slot] = value.identity.effectId;
    }
    void ClearBus(std::uint32_t bus) noexcept {
        const auto found = Lower(bus);
        if (found == m_rows.end() || found->bus != bus) return;
        m_count -= std::popcount(found->mask);
        m_rows.erase(found);
        const auto begin = Extra(Key(bus, 0));
        auto end = begin;
        while (end != m_exceptions.end() && (end->key >> 32) == bus) ++end;
        m_exceptions.erase(begin, end);
    }
    bool HasOwner(std::uint32_t bus, VnPackHandle owner = 0) const noexcept {
        for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot) {
            const auto setting = Get(bus, slot);
            if (setting.owner != 0 && (owner == 0 || setting.owner == owner)) return true;
        }
        return false;
    }
    std::size_t Count() const noexcept { return m_count; }
    std::size_t StorageBytes() const noexcept {
        return m_rows.capacity() * sizeof(Row) + m_exceptions.capacity() * sizeof(Exception);
    }
private:
    struct Scope { VnObjectId project{}; VnPackHandle owner = 0; bool operator==(const Scope&) const = default; };
    struct Row {
        std::uint32_t bus = 0;
        std::uint8_t mask = 0;
        std::array<std::uint8_t, kMaxFxSlotsPerBus> types{};
        std::array<VnObjectId, kMaxFxSlotsPerBus> ids{};
        Scope common;
    };
    struct Exception { std::uint64_t key; Scope scope; };
    static_assert(kMaxFxSlotsPerBus == 8);
    static_assert([] { for (const auto& row : kEffectRegistry) if (row.effectType > UINT8_MAX) return false; return true; }());
    static Scope ScopeOf(const Setting& value) noexcept { return {value.identity.projectId, value.owner}; }
    static std::uint64_t Key(std::uint32_t bus, std::uint32_t slot) noexcept {
        return (static_cast<std::uint64_t>(bus) << 32) | slot;
    }
    template<class T> static void RequireRoom(const std::vector<T>& rows) noexcept {
        assert(rows.size() < rows.capacity());
        if (rows.size() == rows.capacity()) std::terminate();
    }
    std::vector<Row>::iterator Lower(std::uint32_t bus) noexcept {
        return std::lower_bound(m_rows.begin(), m_rows.end(), bus, [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<Row>::const_iterator Lower(std::uint32_t bus) const noexcept {
        return std::lower_bound(m_rows.begin(), m_rows.end(), bus, [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<Exception>::iterator Extra(std::uint64_t key) noexcept {
        return std::lower_bound(m_exceptions.begin(), m_exceptions.end(), key, [](const auto& row, auto value) { return row.key < value; });
    }
    std::vector<Exception>::const_iterator Extra(std::uint64_t key) const noexcept {
        return std::lower_bound(m_exceptions.begin(), m_exceptions.end(), key, [](const auto& row, auto value) { return row.key < value; });
    }
    std::vector<Row> m_rows;
    std::vector<Exception> m_exceptions;
    std::size_t m_count = 0;
};

} // namespace harken
