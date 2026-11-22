#pragma once

#include <harken_pack/PackVoiceRecipe.h>

#include <cstdint>

namespace harken::studio::build {
inline constexpr std::uint32_t kMaxChannels = 8;
inline constexpr std::uint32_t kMaxFxSlotsPerBus = 8;
inline constexpr std::uint32_t kMaxVoiceFxSlots = HK_PACK_MAX_VOICE_FX_SLOTS;
inline constexpr std::uint32_t kMaxVoiceSendSlots = HK_PACK_MAX_VOICE_SEND_SLOTS;
}
