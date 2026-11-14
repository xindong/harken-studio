#pragma once

#include "harken/render/EffectPool.h"
#include <algorithm>
#include <cassert>
#include <vector>

namespace harken {

// Logic-owned installed settings. Empty slots need only their independent
// bypass bit; physical instances belong to working rows, not definitions.
class BusEffectSettings {
public:
    struct Setting {
        std::uint32_t type = kEffectTypeNone;
        EffectObjectIdentity identity{};
        VnPackHandle owner = 0;
    };
    const Setting& Get(std::uint32_t bus, std::uint32_t slot) const noexcept {
        const auto key = Key(bus, slot);
        const auto found = Lower(key);
        static const Setting empty;
        return found != m_rows.end() && found->key == key ? found->value : empty;
    }
    bool Prepare(std::size_t additions) noexcept {
        if (additions > m_rows.max_size() - m_rows.size()) return false;
        try { m_rows.reserve(m_rows.size() + additions); return true; }
        catch (...) { return false; }
    }
    void Set(std::uint32_t bus, std::uint32_t slot, Setting value) noexcept {
        const auto key = Key(bus, slot);
        const auto found = Lower(key);
        if (found != m_rows.end() && found->key == key) {
            if (value.type == kEffectTypeNone) m_rows.erase(found);
            else found->value = value;
        } else if (value.type != kEffectTypeNone) {
            assert(m_rows.size() < m_rows.capacity());
            if (m_rows.size() == m_rows.capacity()) std::terminate();
            m_rows.insert(found, {key, value});
        }
    }
    void ClearBus(std::uint32_t bus) noexcept {
        const auto begin = Lower(Key(bus, 0));
        auto end = begin;
        while (end != m_rows.end() && (end->key >> 32) == bus) ++end;
        m_rows.erase(begin, end);
    }
    bool HasOwner(std::uint32_t bus, VnPackHandle owner = 0) const noexcept {
        for (auto it = Lower(Key(bus, 0)); it != m_rows.end() && (it->key >> 32) == bus; ++it)
            if (it->value.owner != 0 && (owner == 0 || it->value.owner == owner)) return true;
        return false;
    }
    std::size_t Count() const noexcept { return m_rows.size(); }
    std::size_t StorageBytes() const noexcept { return m_rows.capacity() * sizeof(Row); }
private:
    struct Row { std::uint64_t key; Setting value; };
    static std::uint64_t Key(std::uint32_t bus, std::uint32_t slot) noexcept {
        return (static_cast<std::uint64_t>(bus) << 32) | slot;
    }
    std::vector<Row>::iterator Lower(std::uint64_t key) noexcept {
        return std::lower_bound(m_rows.begin(), m_rows.end(), key,
            [](const auto& row, auto value) { return row.key < value; });
    }
    std::vector<Row>::const_iterator Lower(std::uint64_t key) const noexcept {
        return std::lower_bound(m_rows.begin(), m_rows.end(), key,
            [](const auto& row, auto value) { return row.key < value; });
    }
    std::vector<Row> m_rows;
};

} // namespace harken
