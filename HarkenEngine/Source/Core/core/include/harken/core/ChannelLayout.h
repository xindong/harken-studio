// Channel-layout metadata, the multichannel pan law, and the default downmix
// coefficient tables — the normative Appendix A data of the multichannel-output
// amendment, in one place so the render path, the config resolver, and the
// tests all read the same tables. Header-only (inline / constexpr): it carries
// no state and defines the pinned constants once.
//
// Canonical channel order per layout matches SMPTE / WAVE_FORMAT_EXTENSIBLE /
// miniaudio, so the runtime and device agree without remapping:
//   MONO   : C            (treated as front-center for name placement)
//   STEREO : FL, FR
//   QUAD   : FL, FR, BL, BR
//   5.1    : FL, FR, FC, LFE, BL, BR
//   7.1    : FL, FR, FC, LFE, BL, BR, SL, SR
#ifndef HARKEN_CORE_CHANNEL_LAYOUT_H
#define HARKEN_CORE_CHANNEL_LAYOUT_H

#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"

#include <cmath>
#include <cstdint>

namespace harken {

// Speaker roles across all supported layouts. The mono channel is treated as
// FC (front center) for name placement, so a mono bus output name-places into a
// consumer's center when present.
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

// Channel count of a concrete layout; 0 for VN_CHANNEL_LAYOUT_DEFAULT or any
// value outside the fixed enum band.
inline std::uint32_t LayoutChannelCount(std::uint32_t layout) {
    switch (layout) {
    case VN_CHANNEL_LAYOUT_MONO:
        return 1;
    case VN_CHANNEL_LAYOUT_STEREO:
        return 2;
    case VN_CHANNEL_LAYOUT_QUAD:
        return 4;
    case VN_CHANNEL_LAYOUT_5_1:
        return 6;
    case VN_CHANNEL_LAYOUT_7_1:
        return 8;
    default:
        return 0;
    }
}

// A concrete layout is one of the five with a defined channel order (i.e. not
// the DEFAULT sentinel and within the enum band).
inline bool IsConcreteLayout(std::uint32_t layout) {
    return LayoutChannelCount(layout) > 0;
}

// The standard layout a source channel count implies (1 = mono, 2 = stereo,
// 4 = quad, 6 = 5.1, 8 = 7.1), or VN_CHANNEL_LAYOUT_DEFAULT for a non-standard
// count (3/5/7) that names no layout. Used to map a multichannel source's
// channels by role into the mix layout.
inline std::uint32_t LayoutFromChannelCount(std::uint32_t count) {
    switch (count) {
    case 1:
        return VN_CHANNEL_LAYOUT_MONO;
    case 2:
        return VN_CHANNEL_LAYOUT_STEREO;
    case 4:
        return VN_CHANNEL_LAYOUT_QUAD;
    case 6:
        return VN_CHANNEL_LAYOUT_5_1;
    case 8:
        return VN_CHANNEL_LAYOUT_7_1;
    default:
        return VN_CHANNEL_LAYOUT_DEFAULT;
    }
}

// Resolves a real device's reported native channel count to a concrete
// VnChannelLayout for the deviceChannelLayout == VN_CHANNEL_LAYOUT_DEFAULT
// "follow the device's native/default layout" contract. A standard count maps
// to its exact layout; a non-standard count (0/3/5/7 or > 8) floors to the
// largest supported layout whose channel count does not exceed the native
// count, so the engine never presents more channels than the device has (a
// count of 0 or 1 floors to mono/stereo defensively). This is a documented
// decision — the real-device path is validated manually per spec §7; the
// mapping itself is unit-tested.
inline std::uint32_t DeviceLayoutFromNativeChannels(std::uint32_t count) {
    if (count >= 8) {
        return VN_CHANNEL_LAYOUT_7_1;
    }
    if (count >= 6) {
        return VN_CHANNEL_LAYOUT_5_1;
    }
    if (count >= 4) {
        return VN_CHANNEL_LAYOUT_QUAD;
    }
    if (count >= 2) {
        return VN_CHANNEL_LAYOUT_STEREO;
    }
    if (count == 1) {
        return VN_CHANNEL_LAYOUT_MONO;
    }
    return VN_CHANNEL_LAYOUT_STEREO; // a device reporting 0 channels: default
}

// dst is narrower-or-equal to src when both are concrete and dst has no more
// channels — the only direction a downmix matrix folds. Also true for equal
// layouts (an identity, no fold), which callers treat as passthrough.
inline bool LayoutNarrowerOrEqual(std::uint32_t dst, std::uint32_t src) {
    return IsConcreteLayout(dst) && IsConcreteLayout(src) &&
           LayoutChannelCount(dst) <= LayoutChannelCount(src);
}

// The ordered speaker roles of a concrete layout, written into `outRoles`
// (capacity kMaxChannels). Returns the channel count (0 for a non-concrete
// layout, leaving outRoles untouched). Index i of outRoles is the role of
// channel i in the canonical order above.
inline std::uint32_t LayoutRoles(std::uint32_t layout,
                                 ChannelRole* outRoles) {
    using R = ChannelRole;
    switch (layout) {
    case VN_CHANNEL_LAYOUT_MONO:
        outRoles[0] = R::FC;
        return 1;
    case VN_CHANNEL_LAYOUT_STEREO:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        return 2;
    case VN_CHANNEL_LAYOUT_QUAD:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        outRoles[2] = R::BL;
        outRoles[3] = R::BR;
        return 4;
    case VN_CHANNEL_LAYOUT_5_1:
        outRoles[0] = R::FL;
        outRoles[1] = R::FR;
        outRoles[2] = R::FC;
        outRoles[3] = R::LFE;
        outRoles[4] = R::BL;
        outRoles[5] = R::BR;
        return 6;
    case VN_CHANNEL_LAYOUT_7_1:
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

// Channel index carrying `role` in `layout`, or -1 when the layout has no such
// channel. Used for name placement (narrower -> wider) and downmix table
// construction.
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

// --- multichannel pan (the widened fold-in for a mono point source) ---

// Ordered front-speaker channel indices of a layout, left to right (the arc a
// point source pans across). Written into `outIdx` (capacity 3); returns the
// count. STEREO/QUAD front is {FL, FR}; 5.1/7.1 front is {FL, FC, FR}; MONO is
// {FC}. The scalar pan is mapped linearly across these speakers (equal-weight
// bracketing), which reduces bit-exactly to the frozen stereo law for the
// two-speaker case.
inline std::uint32_t FrontSpeakers(std::uint32_t layout, int* outIdx) {
    switch (layout) {
    case VN_CHANNEL_LAYOUT_MONO:
        outIdx[0] = 0; // C
        return 1;
    case VN_CHANNEL_LAYOUT_STEREO:
    case VN_CHANNEL_LAYOUT_QUAD:
        outIdx[0] = ChannelOfRole(layout, ChannelRole::FL);
        outIdx[1] = ChannelOfRole(layout, ChannelRole::FR);
        return 2;
    case VN_CHANNEL_LAYOUT_5_1:
    case VN_CHANNEL_LAYOUT_7_1:
        outIdx[0] = ChannelOfRole(layout, ChannelRole::FL);
        outIdx[1] = ChannelOfRole(layout, ChannelRole::FC);
        outIdx[2] = ChannelOfRole(layout, ChannelRole::FR);
        return 3;
    default:
        return 0;
    }
}

// The resolved pair carried from the logic thread to the audio thread for one
// fixed session mix layout. channel1 is kInvalidPanChannel for a mono layout.
inline constexpr std::uint32_t kInvalidPanChannel = ~std::uint32_t{0};

struct ResolvedPanGains {
    float pan = 0.0f;
    float gain0 = 1.0f;
    float gain1 = 0.0f;
    std::uint32_t channel0 = 0;
    std::uint32_t channel1 = kInvalidPanChannel;
};

// Resolves the exact equal-power pair for a scalar pan on the logic thread.
// Stereo reduces through the same front-speaker selection to the established
// quarter-circle expression. Wider layouts use the former render-side segment
// selection and pair formula. The returned authored pan remains available for
// observability while the audio thread consumes only the indices and gains.
inline ResolvedPanGains ResolvePanGains(std::uint32_t layout, float pan) {
    ResolvedPanGains resolved;
    float clamped = pan;
    if (!(clamped >= -1.0f && clamped <= 1.0f)) {
        clamped = (clamped > 1.0f) ? 1.0f
                                   : (clamped < -1.0f) ? -1.0f : 0.0f;
    }
    resolved.pan = clamped;

    int front[3];
    const std::uint32_t n = FrontSpeakers(layout, front);
    if (n == 0) {
        resolved.gain0 = 0.0f;
        return resolved;
    }
    resolved.channel0 = static_cast<std::uint32_t>(front[0]);
    if (n == 1) {
        return resolved;
    }

    const float p = (resolved.pan + 1.0f) * 0.5f;
    const float scaled = p * static_cast<float>(n - 1);
    std::uint32_t seg = static_cast<std::uint32_t>(scaled);
    if (seg > n - 2) {
        seg = n - 2;
    }
    const float q = scaled - static_cast<float>(seg);
    const float theta = q * 0.5f * 3.14159265358979323846f;
    resolved.channel0 = static_cast<std::uint32_t>(front[seg]);
    resolved.channel1 = static_cast<std::uint32_t>(front[seg + 1]);
    resolved.gain0 = std::cos(theta);
    resolved.gain1 = std::sin(theta);
    return resolved;
}

// --- default downmix coefficient tables (Appendix A, pinned) ---

// The -3 dB equal-power coefficient (1/sqrt(2)) and its half (0.5/sqrt(2)),
// the standard BS.775 surround-fold levels. Defined once here so every fold and
// every golden reads the identical constant.
inline constexpr float kDownmixMinus3dB = 0.70710678118654752440f;
inline constexpr float kDownmixHalfMinus3dB = 0.35355339059327376220f;

// Sets coeffs[dstRole][srcRole] = gain when both roles exist in their layouts;
// a role absent from either side contributes nothing (e.g. LFE folded to a
// layout without LFE is simply dropped). Row-major, destination outer, matching
// VnDownmixMatrixDesc packing.
inline void SetDownmixCoeff(float* coeffs, std::uint32_t dstLayout,
                            std::uint32_t srcLayout, ChannelRole dstRole,
                            ChannelRole srcRole, float gain) {
    const int r = ChannelOfRole(dstLayout, dstRole);
    const int c = ChannelOfRole(srcLayout, srcRole);
    if (r >= 0 && c >= 0) {
        coeffs[r * 8 + c] = gain;
    }
}

// Fills `coeffs` (a full 8x8 array, row-major dst outer) with the default fold
// from srcLayout to dstLayout. The 5.1->stereo / 5.1->mono / 7.1->stereo /
// 7.1->5.1 / 7.1->mono folds are the normative Appendix A tables (BS.775 for
// the 5.1 source folds, the documented -3 dB engineering extension for the 7.1
// folds); the remaining narrower pairs (quad and stereo folds) follow the same
// equal-power convention as documented engineering defaults. dst == src is the
// identity (never applied — callers treat equal layouts as passthrough). Assumes
// LayoutNarrowerOrEqual(dstLayout, srcLayout).
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
    if (dstLayout == VN_CHANNEL_LAYOUT_MONO) {
        // Front pair at -3 dB, center at unity, surrounds folded in: 5.1 back
        // pair at 0.5 (BS.775 Annex 4 Table 2), 7.1 side/back at 0.3536 (the
        // 7.1->5.1 fold composed with 5.1->mono). LFE discarded.
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FL, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FR, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::FC, 1.0f);
        const bool sevenOne = (srcLayout == VN_CHANNEL_LAYOUT_7_1);
        const float back = sevenOne ? h3 : 0.5f;
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::BL, back);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::BR, back);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::SL, h3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FC, R::SR, h3);
        return;
    }
    if (dstLayout == VN_CHANNEL_LAYOUT_STEREO) {
        // Lo = FL + .7071 FC + .7071 BL (+ .7071 SL); Ro = FR + .7071 FC +
        // .7071 BR (+ .7071 SR). LFE discarded.
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
    if (dstLayout == VN_CHANNEL_LAYOUT_QUAD) {
        // Front L/R carry the center at -3 dB; back pair passes through, with a
        // 7.1 source folding its sides into the back pair. LFE discarded.
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FL, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FR, 1.0f);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FL, R::FC, m3);
        SetDownmixCoeff(coeffs, dstLayout, srcLayout, R::FR, R::FC, m3);
        if (srcLayout == VN_CHANNEL_LAYOUT_7_1) {
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
    if (dstLayout == VN_CHANNEL_LAYOUT_5_1) {
        // 7.1 -> 5.1: front + LFE pass through at unity; the side pair folds
        // into the back pair at -3 dB equal power.
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

} // namespace harken

#endif // HARKEN_CORE_CHANNEL_LAYOUT_H
