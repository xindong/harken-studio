#include <harken/Harken.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(VN_EFFECT_GAIN == 2,
              "VN_EFFECT_GAIN is ABI-frozen at 2 (append-only after "
              "VN_EFFECT_THREE_BAND_TONE = 1)");
static_assert(sizeof(VnGainDesc) == 32);
static_assert(offsetof(VnGainDesc, structSize) == 0 &&
                  offsetof(VnGainDesc, reserved0) == 4 &&
                  offsetof(VnGainDesc, effectId) == 8 &&
                  offsetof(VnGainDesc, gainDb) == 24,
              "VnGainDesc field layout pinned: header then gainDb");
static_assert(std::is_same_v<decltype(VnGainDesc::structSize), std::uint32_t>,
              "VnGainDesc::structSize must be uint32_t");
static_assert(std::is_same_v<decltype(VnGainDesc::reserved0), std::uint32_t>,
              "VnGainDesc::reserved0 must be uint32_t");
static_assert(std::is_same_v<decltype(VnGainDesc::gainDb), float>,
              "VnGainDesc::gainDb must be float (a uint32_t gainDb is a "
              "different contract)");

static_assert(VN_EFFECT_LIMITER == 3,
              "VN_EFFECT_LIMITER is ABI-frozen at 3 (append-only after "
              "VN_EFFECT_GAIN = 2)");
static_assert(sizeof(VnLimiterDesc) == 32);
static_assert(offsetof(VnLimiterDesc, structSize) == 0 &&
                  offsetof(VnLimiterDesc, reserved0) == 4 &&
                  offsetof(VnLimiterDesc, effectId) == 8 &&
                  offsetof(VnLimiterDesc, ceilingDb) == 24 &&
                  offsetof(VnLimiterDesc, releaseMs) == 28,
              "VnLimiterDesc field layout pinned: header, ceilingDb, releaseMs");
static_assert(std::is_same_v<decltype(VnLimiterDesc::structSize), std::uint32_t>,
              "VnLimiterDesc::structSize must be uint32_t");
static_assert(std::is_same_v<decltype(VnLimiterDesc::reserved0), std::uint32_t>,
              "VnLimiterDesc::reserved0 must be uint32_t");
static_assert(std::is_same_v<decltype(VnLimiterDesc::ceilingDb), float>,
              "VnLimiterDesc::ceilingDb must be float");
static_assert(std::is_same_v<decltype(VnLimiterDesc::releaseMs), float>,
              "VnLimiterDesc::releaseMs must be float");
