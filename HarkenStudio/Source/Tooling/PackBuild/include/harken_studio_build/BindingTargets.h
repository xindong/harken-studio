#pragma once

#include <harken_studio/AudioValues.h>
#include <cstdint>

namespace harkenstudiobuild {
inline constexpr std::uint32_t kTargetScopeVoice = harkenstudio::kTargetScopeVoice;
inline constexpr std::uint32_t kTargetScopeBus = harkenstudio::kTargetScopeBus;
inline constexpr std::uint32_t kTargetVoiceVolume = 0;
inline constexpr std::uint32_t kTargetVoicePitch = 1;
inline constexpr std::uint32_t kBusTargetBusVolume = 0;
inline constexpr std::uint32_t kBusTargetBusOutputVolume = 1;

struct TargetMeta { bool supportsTransition; };

inline const TargetMeta* FindTargetMeta(std::uint32_t scope, std::uint32_t target)
{
    constexpr static TargetMeta ramp{true};
    constexpr static TargetMeta direct{false};
    if (scope == harkenstudio::kTargetScopeBus && target < 2) return &ramp;
    if (scope == harkenstudio::kTargetScopeVoice && target < 14)
        return target == 4 || target == 5 ? &direct : &ramp;
    return nullptr;
}
}
