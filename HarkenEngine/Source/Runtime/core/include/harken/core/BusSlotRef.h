#ifndef HARKEN_CORE_BUS_SLOT_REF_H
#define HARKEN_CORE_BUS_SLOT_REF_H

#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kInvalidBusGeneration = 0;

struct BusSlotRef {
    std::uint32_t index = 0;
    std::uint32_t generation = kInvalidBusGeneration;

    constexpr bool Valid() const noexcept {
        return generation != kInvalidBusGeneration;
    }

    constexpr std::uint64_t Packed() const noexcept {
        return static_cast<std::uint64_t>(generation) << 32u | index;
    }

    static constexpr BusSlotRef FromPacked(std::uint64_t packed) noexcept {
        return {static_cast<std::uint32_t>(packed),
                static_cast<std::uint32_t>(packed >> 32u)};
    }
};

inline constexpr bool operator==(BusSlotRef lhs, BusSlotRef rhs) noexcept {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

inline constexpr bool operator!=(BusSlotRef lhs, BusSlotRef rhs) noexcept {
    return !(lhs == rhs);
}

} // namespace harken

#endif // HARKEN_CORE_BUS_SLOT_REF_H
