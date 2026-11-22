#pragma once
#include <cstdint>
namespace harken::studio::build {
inline constexpr std::uint32_t kMaxBindingTransitionMs = 60000;
inline constexpr bool IsValidBindingTransitionMs(std::uint32_t ms) noexcept {
    return ms <= kMaxBindingTransitionMs;
}
} // namespace harken::studio::build
