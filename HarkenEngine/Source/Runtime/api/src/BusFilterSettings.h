#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace harken {

// Only explicitly configured filters retain settings. Preparation precedes
// command publication; commit and Bus-incarnation reset never allocate.
class BusFilterSettings {
public:
    struct Setting { float cutoffHz = 0, q = 0; bool enabled = false, configured = false; };
    Setting Get(std::uint32_t bus, std::uint32_t section) const noexcept {
        const auto found = Lower(bus);
        if (found == rows_.end() || found->bus != bus || section >= 4) return {};
        return {found->cutoff[section], found->q[section],
            (found->enabled & (1u << section)) != 0, (found->configured & (1u << section)) != 0};
    }
    bool Prepare(std::uint32_t bus) noexcept {
        try {
            const auto found = Lower(bus);
            if (found == rows_.end() || found->bus != bus) {
                Row row; row.bus = bus;
                rows_.insert(found, row);
            }
            return true;
        } catch (...) { return false; }
    }
    void Set(std::uint32_t bus, std::uint32_t section, Setting value) noexcept {
        const auto found = Lower(bus);
        assert(found != rows_.end() && found->bus == bus && section < 4);
        found->cutoff[section] = value.cutoffHz;
        found->q[section] = value.q;
        const auto bit = 1u << section;
        found->enabled = (found->enabled & ~bit) | (value.enabled ? bit : 0u);
        found->configured = (found->configured & ~bit) | (value.configured ? bit : 0u);
    }
    void Clear(std::uint32_t bus) noexcept {
        const auto found = Lower(bus);
        if (found != rows_.end() && found->bus == bus) rows_.erase(found);
        if (rows_.empty()) std::vector<Row>().swap(rows_);
    }
    std::size_t StorageBytes() const noexcept { return rows_.capacity() * sizeof(Row); }
private:
    struct Row {
        std::uint32_t bus = 0;
        std::array<float, 4> cutoff{}, q{};
        std::uint8_t enabled = 0, configured = 0;
    };
    std::vector<Row>::iterator Lower(std::uint32_t bus) noexcept {
        return std::lower_bound(rows_.begin(), rows_.end(), bus,
            [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<Row>::const_iterator Lower(std::uint32_t bus) const noexcept {
        return std::lower_bound(rows_.begin(), rows_.end(), bus,
            [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<Row> rows_;
};

} // namespace harken
