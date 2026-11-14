#ifndef HARKEN_DSP_BIQUAD_DORMANCY_H
#define HARKEN_DSP_BIQUAD_DORMANCY_H

#include "harken/dsp/Biquad.h"
#include <bit>

namespace harken {

// Bit tests must precede any float-to-double conversion: DAZ could otherwise
// make a nonzero float subnormal appear to be a reconstructible zero.
template<std::size_t Channels>
inline bool CaptureBiquadZeroState(const BiquadState (&states)[Channels], const float (&lastInput)[Channels],
                      std::uint32_t& signs) noexcept {
    static_assert(Channels <= 8);
    signs = 0;
    for (std::uint32_t ch = 0; ch < Channels; ++ch) {
        const auto& state = states[ch];
        if (state.wide) {
            const auto a = std::bit_cast<std::uint64_t>(state.memory.extended.z1);
            const auto b = std::bit_cast<std::uint64_t>(state.memory.extended.z2);
            if ((a & UINT64_C(0x7fffffffffffffff)) != 0 ||
                (b & UINT64_C(0x7fffffffffffffff)) != 0) return false;
            signs |= static_cast<std::uint32_t>(a >> 63) << (ch * 3);
            signs |= static_cast<std::uint32_t>(b >> 63) << (ch * 3 + 1);
            signs |= 1u << (24 + ch);
        } else {
            const auto a = std::bit_cast<std::uint32_t>(state.memory.normal.z1);
            const auto b = std::bit_cast<std::uint32_t>(state.memory.normal.z2);
            if ((a & 0x7fffffffu) != 0 || (b & 0x7fffffffu) != 0) return false;
            signs |= (a >> 31) << (ch * 3);
            signs |= (b >> 31) << (ch * 3 + 1);
        }
        const auto input = std::bit_cast<std::uint32_t>(lastInput[ch]);
        if ((input & 0x7fffffffu) != 0) return false;
        signs |= (input >> 31) << (ch * 3 + 2);
    }
    return true;
}

template<std::size_t Channels>
inline void RestoreBiquadZeroState(BiquadState (&states)[Channels],
                                   float (&lastInput)[Channels], std::uint32_t signs) noexcept {
    static_assert(Channels <= 8);
    for (std::size_t ch = 0; ch < Channels; ++ch) {
        const auto sign = [&](unsigned field) { return (signs >> (ch * 3 + field)) & 1u; };
        if ((signs & (1u << (24 + ch))) != 0) {
            states[ch].memory.extended = {
                std::bit_cast<double>(static_cast<std::uint64_t>(sign(0)) << 63),
                std::bit_cast<double>(static_cast<std::uint64_t>(sign(1)) << 63)};
            states[ch].wide = true;
        } else {
            states[ch].SetFloat(std::bit_cast<float>(sign(0) << 31), std::bit_cast<float>(sign(1) << 31));
        }
        lastInput[ch] = std::bit_cast<float>(sign(2) << 31);
    }
}

} // namespace harken
#endif
