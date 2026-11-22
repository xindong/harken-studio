#pragma once

#include <cassert>
#include <cstdint>
#include <span>

namespace harken {

struct BusRenderSlot {
    std::uint32_t plane = UINT32_MAX;
    std::uint32_t generation = 0;
    std::uint32_t parent = 0;
};

// Stable Bus identities address compact, preallocated channel-plane rows.
// The render owner keeps this immutable directory alive through worker joins.
class BusRenderView {
public:
    // Dense views also serve isolated cascade tests without a BusTree owner.
    BusRenderView(float* const* planes) noexcept : planes_(planes) {}
    BusRenderView(float* const* planes, std::span<const BusRenderSlot> slots,
                  std::uint64_t* fallbackCount = nullptr) noexcept
        : planes_(planes), slots_(slots), fallbackCount_(fallbackCount) {
        assert(slots.empty() || slots[0].plane != UINT32_MAX);
    }

    float* const* ForBus(std::uint32_t index, std::uint32_t channels,
                        std::uint32_t generation = 0) const noexcept {
        if (slots_.empty()) return planes_ + static_cast<std::size_t>(index) * channels;
        const bool matches = index < slots_.size() &&
            (generation == 0 || slots_[index].generation == generation);
        if (matches && slots_[index].plane != UINT32_MAX)
            return planes_ + static_cast<std::size_t>(slots_[index].plane) * channels;
        if (fallbackCount_ != nullptr) ++*fallbackCount_;
        // Accepted starts must never need this isolation path. A stale identity
        // cannot borrow the replacement Bus's ancestry or audio storage.
        if (!matches) index = 0;
        for (std::size_t remaining = slots_.size(); remaining != 0; --remaining) {
            if (slots_[index].plane != UINT32_MAX)
                return planes_ + static_cast<std::size_t>(slots_[index].plane) * channels;
            const auto parent = slots_[index].parent;
            if (parent >= slots_.size() || parent == index) break;
            index = parent;
        }
        return planes_ + static_cast<std::size_t>(slots_[0].plane) * channels;
    }

private:
    float* const* planes_ = nullptr;
    std::span<const BusRenderSlot> slots_{};
    std::uint64_t* fallbackCount_ = nullptr;
};

} // namespace harken
