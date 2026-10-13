#ifndef HARKEN_STUDIO_BUILD_CHANNEL_LAYOUT_H
#define HARKEN_STUDIO_BUILD_CHANNEL_LAYOUT_H

#include <harken_studio/AudioValues.h>

#include "harken_studio_build/BuildLimits.h"

#include <cmath>
#include <cstdint>

namespace harkenstudiobuild {
enum class ChannelRole : std::uint8_t {
    FL = 0,
    FR = 1,
    FC = 2,
    LFE = 3,
    BL = 4,
    BR = 5,
    SL = 6,
    SR = 7
};
inline std::uint32_t LayoutChannelCount(std::uint32_t layout) {
    switch (layout) {
    case harkenstudio::kChannelLayoutMono:
        return 1;
    case harkenstudio::kChannelLayoutStereo:
        return 2;
    case harkenstudio::kChannelLayoutQuad:
        return 4;
    case harkenstudio::kChannelLayout5_1:
        return 6;
    case harkenstudio::kChannelLayout7_1:
        return 8;
    default:
        return 0;
    }
}
inline bool IsConcreteLayout(std::uint32_t layout) {
    return LayoutChannelCount(layout) > 0;
}
inline std::uint32_t LayoutFromChannelCount(std::uint32_t count) {
    switch (count) {
    case 1:
        return harkenstudio::kChannelLayoutMono;
    case 2:
        return harkenstudio::kChannelLayoutStereo;
    case 4:
        return harkenstudio::kChannelLayoutQuad;
    case 6:
        return harkenstudio::kChannelLayout5_1;
    case 8:
        return harkenstudio::kChannelLayout7_1;
    default:
        return harkenstudio::kChannelLayoutDefault;
    }
}
inline bool LayoutNarrowerOrEqual(std::uint32_t dst, std::uint32_t src) {
    return IsConcreteLayout(dst) && IsConcreteLayout(src) &&
           LayoutChannelCount(dst) <= LayoutChannelCount(src);
}
inline std::uint32_t LayoutRoles(std::uint32_t layout,
                                 ChannelRole* outRoles) {
    using R = ChannelRole;
    switch (layout) {
    case harkenstudio::kChannelLayoutMono:
        outRoles[0] = R::FC;
        return 1;
    case harkenstudio::kChannelLayoutStereo:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        return 2;
    case harkenstudio::kChannelLayoutQuad:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        outRoles[2] = R::BL;
        outRoles[3] = R::BR;
        return 4;
    case harkenstudio::kChannelLayout5_1:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        outRoles[2] = R::FC;
        outRoles[3] = R::LFE;
        outRoles[4] = R::BL;
        outRoles[5] = R::BR;
        return 6;
    case harkenstudio::kChannelLayout7_1:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        outRoles[2] = R::FC;
        outRoles[3] = R::LFE;
        outRoles[4] = R::BL;
        outRoles[5] = R::BR;
        outRoles[6] = R::SL;
        outRoles[7] = R::SR;
        return 8;
    default:
        return 0;
    }
}
inline int ChannelOfRole(std::uint32_t layout, ChannelRole role) {
    ChannelRole roles[kMaxChannels];
    const std::uint32_t n = LayoutRoles(layout, roles);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (roles[i] == role) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
inline constexpr float kDownmixMinus3dB = 0.70710678118654752440f;
inline constexpr float kDownmixHalfMinus3dB = 0.35355339059327376220f;
inline void SetDownmixCoeff(float* coeffs, std::uint32_t dstLayout,
                            std::uint32_t srcLayout, ChannelRole dstRole,
                            ChannelRole srcRole, float gain) {
    const int r = ChannelOfRole(dstLayout, dstRole);
    const int c = ChannelOfRole(srcLayout, srcRole);
    if (r >= 0 && c >= 0) {
        coeffs[r * 8 + c] = gain;
    }
}
inline void DefaultDownmixMatrix(std::uint32_t srcLayout,
                                 std::uint32_t dstLayout, float* coeffs) {
    using R = ChannelRole;
    for (int i = 0; i < 64; ++i) {
        coeffs[i] = 0.0f;
    }
    const std::uint32_t srcCount = LayoutChannelCount(srcLayout);
    const std::uint32_t dstCount = LayoutChannelCount(dstLayout);
    if (srcCount == 0 || dstCount == 0) {
        return;
    }
    if (dstLayout == srcLayout) {
        for (std::uint32_t i = 0; i < dstCount; ++i) {
            coeffs[i * 8 + i] = 1.0f;
        }
        return;
    }
    const float m3 = kDownmixMinus3dB;
    const float h3 = kDownmixHalfMinus3dB;
    if (dstLayout == harkenstudio::kChannelLayoutMono) {
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FL, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FR, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FC, 1.0f);
        const bool sevenOne = (srcLayout == harkenstudio::kChannelLayout7_1);
        const float back = sevenOne ? h3 : 0.5f;
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::BL, back);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::BR, back);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::SL, h3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::SR, h3);
        return;
    }
    if (dstLayout == harkenstudio::kChannelLayoutStereo) {
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FL, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FR, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FC, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FC, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::BL, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::BR, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::SL, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::SR, m3);
        return;
    }
    if (dstLayout == harkenstudio::kChannelLayoutQuad) {
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FL, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FR, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FC, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FC, m3);
        if (srcLayout == harkenstudio::kChannelLayout7_1) {
            SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BL, R::BL, m3);
            SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BR, R::BR, m3);
            SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BL, R::SL, m3);
            SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BR, R::SR, m3);
        } else {
            SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BL, R::BL, 1.0f);
            SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BR, R::BR, 1.0f);
        }
        return;
    }
    if (dstLayout == harkenstudio::kChannelLayout5_1) {
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FL, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FR, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FC, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::LFE, R::LFE, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BL, R::BL, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BR, R::BR, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BL, R::SL, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::BR, R::SR, m3);
        return;
    }
}

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_CHANNEL_LAYOUT_H
