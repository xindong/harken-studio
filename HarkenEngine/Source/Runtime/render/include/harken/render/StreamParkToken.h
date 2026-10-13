#ifndef HARKEN_RENDER_STREAM_PARK_TOKEN_H
#define HARKEN_RENDER_STREAM_PARK_TOKEN_H

#include "harken/core/StreamParkToken.h"
#include "harken/dsp/Resampler.h"

#include <cstdint>
#include <type_traits>

namespace harken {

struct StreamParkToken {
    static constexpr std::uint32_t kFormatVersion = 3;

    std::uint32_t formatVersion = kFormatVersion;
    std::uint32_t usedBytes = 0;
    std::uint32_t channelCount = 0;
    ResampleProfile profile = ResampleProfile::LegacyCompatibility;
    std::uint32_t ringGeneration = 0;
    std::uint64_t activationFrame = 0;
    std::uint32_t activationLoopsRemaining = 0;
    std::uint64_t audibleFrame = 0;
    std::uint32_t audibleLoopsRemaining = 0;
    std::uint64_t feedFrame = 0;
    std::uint32_t feedLoopsRemaining = 0;
    bool gateOpen = false;
    ResamplerParkState resampler{};
};

static_assert(std::is_trivially_copyable_v<StreamParkToken>,
              "stream park token must be pointer-free and trivially copyable");
static_assert(sizeof(StreamParkToken) <= kStreamParkTokenBytes,
              "stream park token exceeds the 32 KiB fixed slot");

} // namespace harken

#endif // HARKEN_RENDER_STREAM_PARK_TOKEN_H
