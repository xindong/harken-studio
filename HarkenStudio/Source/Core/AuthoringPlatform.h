#ifndef HARKEN_STUDIO_AUTHORING_PLATFORM_H
#define HARKEN_STUDIO_AUTHORING_PLATFORM_H

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace VNX {

enum class AuthoringPlatform : std::uint8_t {
    Universal = 0,
    Windows = 1,
    macOS = 2,
    Linux = 3,
    Android = 4,
    iOS = 5
};

inline constexpr std::array kTargetAuthoringPlatforms{
    AuthoringPlatform::Windows, AuthoringPlatform::macOS,
    AuthoringPlatform::Linux, AuthoringPlatform::Android,
    AuthoringPlatform::iOS};

constexpr bool IsTargetAuthoringPlatform(AuthoringPlatform platform) noexcept
{
    for (const auto target : kTargetAuthoringPlatforms)
        if (platform == target) return true;
    return false;
}

constexpr std::string_view AuthoringPlatformName(AuthoringPlatform platform) noexcept
{
    switch (platform) {
    case AuthoringPlatform::Universal: return "Universal";
    case AuthoringPlatform::Windows: return "Windows";
    case AuthoringPlatform::macOS: return "macOS";
    case AuthoringPlatform::Linux: return "Linux";
    case AuthoringPlatform::Android: return "Android";
    case AuthoringPlatform::iOS: return "iOS";
    }
    return "Unknown";
}

constexpr std::optional<AuthoringPlatform> ParseAuthoringPlatform(
    std::string_view name) noexcept
{
    if (name == "Universal") return AuthoringPlatform::Universal;
    for (const auto platform : kTargetAuthoringPlatforms)
        if (AuthoringPlatformName(platform) == name) return platform;
    return std::nullopt;
}

}

#endif
