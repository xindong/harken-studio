#ifndef HARKEN_CORE_SOURCE_PHASE_H
#define HARKEN_CORE_SOURCE_PHASE_H

#include <cstdint>
#include <cstring>

namespace harken {

// Exact binary source phase. The Q128 fraction represents every binary64
// playback ratio in the supported source/mix-rate and pitch range without
// rounding the requested rate. Interpolation uses a double mirror only.
struct ExactSourcePhase {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

struct SourcePhaseStep {
    std::uint64_t whole = 0;
    ExactSourcePhase fraction{};
};

inline bool DecodeSourcePhaseStep(double ratio, SourcePhaseStep* out) noexcept {
    if (out == nullptr || !(ratio > 0.0)) return false;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &ratio, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 52) & 0x7ffu) - 1023;
    if (exponent < -76 || exponent >= 64) return false;
    const std::uint64_t mantissa = (bits & ((1ull << 52) - 1u)) | (1ull << 52);
    SourcePhaseStep step{};
    if (exponent >= 52) {
        step.whole = mantissa << (exponent - 52);
    } else {
        const int fractionBits = 52 - exponent;
        const std::uint64_t fraction = fractionBits < 64
            ? mantissa & ((1ull << fractionBits) - 1u) : mantissa;
        if (exponent >= 0) step.whole = mantissa >> fractionBits;
        const int shift = 128 - fractionBits;
        if (shift >= 64) step.fraction.high = fraction << (shift - 64);
        else {
            step.fraction.low = fraction << shift;
            if (shift != 0) step.fraction.high = fraction >> (64 - shift);
        }
    }
    *out = step;
    return true;
}

inline double SourcePhaseFraction(ExactSourcePhase phase) noexcept {
    const double fraction = static_cast<double>(phase.high) * 0x1p-64 +
                            static_cast<double>(phase.low) * 0x1p-128;
    return fraction < 1.0 ? fraction : 0x1.fffffffffffffp-1;
}

inline std::uint64_t StepSourcePhase(SourcePhaseStep step,
                                     ExactSourcePhase* phase) noexcept {
    const auto low = phase->low + step.fraction.low;
    const auto carry = low < phase->low ? 1ull : 0ull;
    const auto high = phase->high + step.fraction.high;
    const auto whole = step.whole + (high < phase->high ? 1ull : 0ull) +
                       (high + carry < high ? 1ull : 0ull);
    phase->low = low;
    phase->high = high + carry;
    return whole;
}

inline void MultiplySourcePhaseWords(std::uint64_t a, std::uint64_t b,
                                     std::uint64_t* high,
                                     std::uint64_t* low) noexcept {
    const auto a0 = static_cast<std::uint32_t>(a);
    const auto b0 = static_cast<std::uint32_t>(b);
    const auto a1 = a >> 32;
    const auto b1 = b >> 32;
    const std::uint64_t p00 = static_cast<std::uint64_t>(a0) * b0;
    const auto p01 = a0 * b1;
    const auto p10 = a1 * b0;
    const auto middle = (p00 >> 32) + static_cast<std::uint32_t>(p01) +
                        static_cast<std::uint32_t>(p10);
    *low = (middle << 32) | static_cast<std::uint32_t>(p00);
    *high = a1 * b1 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

// Saturation is sufficient for monotone boundary search and avoids compiler-
// specific 128-bit integer types. Work is independent of media duration.
inline std::uint64_t JumpSourcePhase(SourcePhaseStep step, ExactSourcePhase phase,
                                     std::uint64_t frames) noexcept {
    std::uint64_t lowHigh = 0, lowLow = 0, highHigh = 0, highLow = 0;
    MultiplySourcePhaseWords(step.fraction.low, frames, &lowHigh, &lowLow);
    MultiplySourcePhaseWords(step.fraction.high, frames, &highHigh, &highLow);
    const auto middle = lowHigh + highLow;
    std::uint64_t carry = middle < lowHigh ? 1u : 0u;
    const auto withPhase = middle + phase.high;
    carry += withPhase < middle ? 1u : 0u;
    const auto lowCarry = lowLow + phase.low < lowLow ? 1ull : 0ull;
    carry += withPhase + lowCarry < withPhase ? 1u : 0u;
    if (highHigh > UINT64_MAX - carry) return UINT64_MAX;
    const auto fractionWhole = highHigh + carry;
    if (step.whole != 0 && frames > (UINT64_MAX - fractionWhole) / step.whole)
        return UINT64_MAX;
    return step.whole * frames + fractionWhole;
}

inline bool FixedSourceOutputFrames(std::uint64_t sourceFrames, double ratio,
                                    std::uint64_t* out,
                                    ExactSourcePhase initial = {}) noexcept {
    SourcePhaseStep step{};
    if (out == nullptr || !DecodeSourcePhaseStep(ratio, &step)) return false;
    if (JumpSourcePhase(step, initial, UINT64_MAX) < sourceFrames) return false;
    std::uint64_t lower = 0, upper = UINT64_MAX;
    while (lower < upper) {
        const auto middle = lower + (upper - lower) / 2u;
        if (JumpSourcePhase(step, initial, middle) >= sourceFrames) upper = middle;
        else lower = middle + 1u;
    }
    *out = lower;
    return true;
}

} // namespace harken

#endif // HARKEN_CORE_SOURCE_PHASE_H
