#ifndef HARKEN_DSP_DOWNMIX_SUM_H
#define HARKEN_DSP_DOWNMIX_SUM_H

#include <cmath>
#include <cstdint>
#include <limits>

namespace harken {

// Keep the ordinary float sum and its rounding. Only retry a non-finite
// result: finite matrix terms can overflow before canceling later in the row.
inline float RecoverFiniteDownmixSum(float sum, const float* coefficients,
                                     float* const* planes,
                                     std::uint32_t channels,
                                     std::uint32_t frame) noexcept {
    if (std::isfinite(sum)) return sum;
    double wide = 0.0;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        const float sample = planes[ch][frame];
        const float gain = coefficients[ch];
        // Do not reinterpret genuinely non-finite input, including at zero
        // gain. The caller retains its existing zero-coefficient semantics.
        if (!std::isfinite(sample) || !std::isfinite(gain)) return sum;
        wide += static_cast<double>(sample) * static_cast<double>(gain);
    }
    if (std::fabs(wide) <= std::numeric_limits<float>::max())
        return static_cast<float>(wide);
    // True overload remains available to the device-boundary sanitizer.
    return sum;
}

} // namespace harken

#endif // HARKEN_DSP_DOWNMIX_SUM_H
