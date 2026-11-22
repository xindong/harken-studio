#pragma once

#include "harken/core/LogicAllocCheckpoint.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace harken {

struct VoiceSendConfig {
    bool active = false;
    std::uint32_t targetBusIndex = 0;
    float levelDb = 0;
    std::uint32_t targetBusGeneration = 0;
    std::uint32_t slot = 0;
    std::uint64_t incarnation = 0;
    bool operator==(const VoiceSendConfig&) const noexcept = default;
};

// Immutable logic snapshots make identity capture and commit allocation-free.
// Sparse uint32 slot labels never determine allocation size. Mutation builds
// a private replacement before the caller's admission/commit boundary.
class VoiceSendConfigs {
public:
    using Records = std::vector<VoiceSendConfig>;
    std::span<const VoiceSendConfig> View() const noexcept {
        return m_rows ? std::span<const VoiceSendConfig>(*m_rows) : std::span<const VoiceSendConfig>{};
    }
    auto begin() const noexcept { return View().begin(); }
    auto end() const noexcept { return View().end(); }
    std::size_t size() const noexcept { return View().size(); }
    bool empty() const noexcept { return View().empty(); }
    const VoiceSendConfig* Find(std::uint32_t slot) const noexcept {
        const auto rows = View();
        const auto found = std::lower_bound(rows.begin(), rows.end(), slot,
            [](const auto& row, auto label) { return row.slot < label; });
        return found != rows.end() && found->slot == slot ? &*found : nullptr;
    }
    static VoiceSendConfigs From(Records rows) {
        VoiceSendConfigs result;
        if (!rows.empty()) {
            detail::LogicAllocCheckpoint(detail::LogicAllocSite::VoiceSendStorage);
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
            result.m_rows = std::make_shared<const Records>(std::move(rows));
        }
        return result;
    }
    VoiceSendConfigs WithSet(VoiceSendConfig row) const {
        detail::LogicAllocCheckpoint(detail::LogicAllocSite::VoiceSendStorage);
        Records rows(begin(), end());
        const auto found = std::lower_bound(rows.begin(), rows.end(), row.slot,
            [](const auto& prior, auto label) { return prior.slot < label; });
        if (found != rows.end() && found->slot == row.slot) *found = row;
        else rows.insert(found, row);
        return From(std::move(rows));
    }
    template<class Predicate> VoiceSendConfigs Without(Predicate predicate) const {
        if (std::none_of(begin(), end(), predicate)) return *this;
        Records rows;
        rows.reserve(size());
        for (const auto& row : *this) if (!predicate(row)) rows.push_back(row);
        return From(std::move(rows));
    }
    VoiceSendConfigs WithoutSlot(std::uint32_t slot) const {
        return Without([slot](const auto& row) { return row.slot == slot; });
    }
    bool operator==(const VoiceSendConfigs& other) const noexcept {
        return m_rows == other.m_rows || (size() == other.size() &&
            std::equal(begin(), end(), other.begin()));
    }
private:
    std::shared_ptr<const Records> m_rows;
};

struct VoiceSendSet;
// Logic-owned backing reserved before a pending/park return can stop a victim.
struct PreparedVoiceSends {
    VoiceSendSet* set = nullptr;
    VoiceSendConfigs snapshot;
    std::uint64_t render = 0;
};

} // namespace harken
