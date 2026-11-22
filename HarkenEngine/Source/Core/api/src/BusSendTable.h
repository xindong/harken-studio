#pragma once

#include "harken/Harken.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace harken {

// Logical definitions and unpublished level actions. Audio never accesses this
// table; publication copies membership and adopts its own mutable ramp state.
struct BusSendRecord {
    std::uint32_t source = UINT32_MAX;
    std::uint32_t target = UINT32_MAX;
    std::uint32_t slot = UINT32_MAX;
    float levelDb = 0.0f;
    VnPackHandle ownerCookie = 0;
    std::uint64_t topologyOwnerId = 0;
    std::uint64_t generation = 0;
    float preparedGain = 1.0f;
    std::uint32_t rampMs = 0;
    bool applyLevel = false;
    bool operator==(const BusSendRecord&) const = default;
};

// Only actual edges occupy storage. Source/slot ordering preserves existing
// traversal order without allocating holes up to the largest slot ordinal.
// Structural callers edit a private candidate and commit with Swap. Ordinary
// level writes may update an existing record in the accepted table directly.
class BusSendTable {
public:
    std::span<const BusSendRecord> Records() const noexcept { return m_records; }
    std::span<BusSendRecord> Records() noexcept { return m_records; }
    std::uint32_t Count() const noexcept { return static_cast<std::uint32_t>(m_records.size()); }
    bool Empty() const noexcept { return m_records.empty(); }
    std::uint32_t IndexOf(const BusSendRecord& edge) const noexcept {
        return static_cast<std::uint32_t>(&edge - m_records.data());
    }

    std::span<const BusSendRecord> From(std::uint32_t source) const noexcept {
        const auto first = std::lower_bound(m_records.begin(), m_records.end(), source,
            [](const auto& edge, auto value) { return edge.source < value; });
        const auto last = std::upper_bound(first, m_records.end(), source,
            [](auto value, const auto& edge) { return value < edge.source; });
        return {first, last};
    }
    std::span<BusSendRecord> From(std::uint32_t source) noexcept {
        const auto first = std::lower_bound(m_records.begin(), m_records.end(), source,
            [](const auto& edge, auto value) { return edge.source < value; });
        const auto last = std::upper_bound(first, m_records.end(), source,
            [](auto value, const auto& edge) { return value < edge.source; });
        return {first, last};
    }
    const BusSendRecord* Find(std::uint32_t source, std::uint32_t target) const noexcept {
        for (const auto& edge : From(source)) if (edge.target == target) return &edge;
        return nullptr;
    }
    BusSendRecord* Find(std::uint32_t source, std::uint32_t target) noexcept {
        for (auto& edge : From(source)) if (edge.target == target) return &edge;
        return nullptr;
    }

    void Reserve(std::size_t count) { m_records.reserve(count); }
    void Add(BusSendRecord edge) {
        if (m_records.size() >= UINT32_MAX) throw std::length_error("Bus Send index exhausted");
        if (edge.slot == UINT32_MAX) {
            edge.slot = 0;
            for (const auto& existing : From(edge.source)) {
                if (existing.slot != edge.slot) break;
                ++edge.slot;
            }
        }
        const auto position = std::lower_bound(m_records.begin(), m_records.end(), edge, Before);
        m_records.insert(position, edge);
    }
    // Bulk preparation avoids moving the accepted graph once per authored edge.
    void Assign(std::vector<BusSendRecord> records) {
        if (records.size() > UINT32_MAX) throw std::length_error("Bus Send index exhausted");
        std::sort(records.begin(), records.end(), Before);
        m_records = std::move(records);
    }
    template<class Predicate> void EraseIf(Predicate&& remove) noexcept {
        std::erase_if(m_records, std::forward<Predicate>(remove));
    }
    void ClearActions() noexcept {
        for (auto& edge : m_records) {
            edge.applyLevel = false;
            edge.rampMs = 0;
        }
    }
    void Swap(BusSendTable& other) noexcept { m_records.swap(other.m_records); }
    bool operator==(const BusSendTable&) const = default;

private:
    static bool Before(const BusSendRecord& a, const BusSendRecord& b) noexcept {
        return a.source < b.source || (a.source == b.source && a.slot < b.slot);
    }
    std::vector<BusSendRecord> m_records;
};

} // namespace harken
