#ifndef HARKEN_RENDER_SIGNAL_ROUTING_TYPES_H
#define HARKEN_RENDER_SIGNAL_ROUTING_TYPES_H
#include <harken/Harken.h>
#include <cstdint>
#include <algorithm>
#include <span>
#include <utility>
namespace harken {
// Entries are sorted by stable Bus index; the value is the compact signal node.
// Routing scratch is indexed by entry ordinal, never by the stable index.
class SignalBusMapping {
public:
    explicit SignalBusMapping(std::span<const std::pair<std::uint32_t, std::uint32_t>> entries)
        : entries_(entries), count_(static_cast<std::uint32_t>(entries.size())) {}
    static SignalBusMapping Dense(std::uint32_t count) noexcept {
        SignalBusMapping result(std::span<const std::pair<std::uint32_t, std::uint32_t>>{});
        result.count_ = count;
        return result;
    }
    std::uint32_t Count() const noexcept { return count_; }
    std::uint32_t Bus(std::uint32_t ordinal) const noexcept {
        return entries_.empty() ? ordinal : entries_[ordinal].first;
    }
    std::uint32_t Node(std::uint32_t ordinal) const noexcept {
        return entries_.empty() ? ordinal : entries_[ordinal].second;
    }
    std::uint32_t Ordinal(std::uint32_t bus) const noexcept {
        if (entries_.empty()) return bus < count_ ? bus : UINT32_MAX;
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), bus,
            [](const auto& entry, auto key) { return entry.first < key; });
        return it != entries_.end() && it->first == bus
            ? static_cast<std::uint32_t>(it - entries_.begin()) : UINT32_MAX;
    }
private:
    std::span<const std::pair<std::uint32_t, std::uint32_t>> entries_;
    std::uint32_t count_ = 0;
};

struct SignalBusRoutingInfo {
    VnBusId id{};
    std::uint32_t generation = 0;
    std::uint32_t inputNode = 0;
    double inputBound = 1;
    double outputFilterBound = 1;
    double outputGainBound = 1;
};
}
#endif
