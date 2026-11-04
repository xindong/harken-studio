#ifndef HARKEN_DSP_DOWNMIX_SUM_H
#define HARKEN_DSP_DOWNMIX_SUM_H

#include "harken/core/EngineConfig.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace harken {

// Keep the ordinary float sum and its rounding. Only retry a non-finite
// result: finite matrix terms can overflow before canceling later in the row.
inline float RecoverFiniteDownmixSum(float sum, const float* coefficients,
                                     float* const* planes,
                                     std::uint32_t channels,
                                     std::uint32_t frame) noexcept {
    if (std::isfinite(sum)) return sum;
    if (channels > kMaxChannels) return sum;
    double partials[kMaxChannels];
    std::uint32_t count = 0;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        const float sample = planes[ch][frame];
        const float gain = coefficients[ch];
        // Do not reinterpret genuinely non-finite input, including at zero
        // gain. The caller retains its existing zero-coefficient semantics.
        if (!std::isfinite(sample) || !std::isfinite(gain)) return sum;
        // A float product fits exactly in double. Retain the rounding residue
        // of every addition so a small middle term survives later cancellation.
        // The expansion has at most one partial per source channel.
        double high = static_cast<double>(sample) * static_cast<double>(gain);
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            double low = partials[i];
            if (std::fabs(high) < std::fabs(low)) std::swap(high, low);
            const double next = high + low;
            const double residue = low - (next - high);
            if (residue != 0.0) partials[kept++] = residue;
            high = next;
        }
        partials[kept++] = high;
        count = kept;
    }
    double wide = 0.0;
    for (std::uint32_t i = 0; i < count; ++i) wide += partials[i];
    if (std::fabs(wide) <= std::numeric_limits<float>::max())
        return static_cast<float>(wide);
    // True overload remains available to the device-boundary sanitizer.
    return sum;
}

} // namespace harken

#endif // HARKEN_DSP_DOWNMIX_SUM_H
