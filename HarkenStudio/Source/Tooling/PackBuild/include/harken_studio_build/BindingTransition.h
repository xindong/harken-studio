#pragma once
#include <cstdint>
namespace harkenstudiobuild {
inline constexpr std::uint32_t kMaxBindingTransitionMs = 60000;
inline constexpr bool IsValidBindingTransitionMs(std::uint32_t ms) noexcept {
    return ms <= kMaxBindingTransitionMs;
}
} // namespace harkenstudiobuild
