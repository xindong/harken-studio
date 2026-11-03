#ifndef HARKEN_RENDER_SIGNAL_PATH_BOUNDS_H
#define HARKEN_RENDER_SIGNAL_PATH_BOUNDS_H

#include "harken/dsp/Biquad.h"
#include "harken/dsp/GainRamp.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace harken {

inline double SignalBoundProduct(double a, double b) noexcept {
    return a == 0 || b == 0 ? 0 : a * b;
}
inline double SignalOutputGainBound(const OutputGain& gain, std::uint32_t frames) noexcept {
    double bound = 0;
    for (std::uint32_t n = 0; n < frames; ++n)
        bound = std::max(bound, std::abs(static_cast<double>(gain.At(n))));
    return bound;
}
inline double SignalIncrementBound(float current, float target, float increment,
                                   std::uint32_t remaining, std::uint32_t frames) noexcept {
    double maximum = std::abs(static_cast<double>(current));
    for (std::uint32_t n = 0; n < std::min(remaining, frames); ++n) {
        current += increment;
        if (n + 1 == remaining) current = target;
        maximum = std::max(maximum, std::abs(static_cast<double>(current)));
    }
    return maximum;
}

// Scalar bound on the transposed direct-form state in a stable moving basis.
// This is numeric metadata, never a second audio/filter history. It follows
// the owning filter's real coefficient schedule and reset lifetime.
class SignalBiquadBound {
public:
    void Reset() noexcept { *this = {}; }
    double Advance(const BiquadCoeffs& c, std::uint32_t frames) noexcept {
        const double alpha = -.5 * c.a1;
        const double d = alpha * alpha - c.a2;
        const double root = std::sqrt(std::abs(d));
        const double radius = d >= 0 ? std::abs(alpha) + root : std::sqrt(c.a2);
        if (!std::isfinite(radius) || radius >= 1) return Fail();
        const double delta = std::max(root, (1 - radius) * .5);
        const double q = Norm(alpha, delta, d / delta, alpha);
        const double b0 = c.b0;
        const double b1 = c.b1 - c.a1 * b0;
        const double b2 = c.b2 - c.a2 * b0;
        const double beta = std::hypot(b1, (b2 + alpha * b1) / delta);
        if (!std::isfinite(q) || q >= 1 || !std::isfinite(beta)) return Fail();
        const double steady = beta / (1 - q);
        if (valid_)
            state_ *= Norm(1, 0, (alpha - alpha_) / delta, delta_ / delta);
        // A new section may be seeded at its input's DC equilibrium. Taking
        // the stable unit-input radius covers that seed as well as zero state.
        state_ = std::max(state_, steady);
        const double initial = state_;
        for (std::uint32_t n = 0; n < frames; ++n) state_ = SignalBoundProduct(q, state_) + beta;
        alpha_ = alpha;
        delta_ = delta;
        valid_ = true;
        const double result = std::abs(b0) + std::max(initial, state_);
        // Allow for normalized float coefficient/state arithmetic. Overflow
        // is a conservative local infinity; never manufacture a finite cap.
        return result * (1 + 64 * std::numeric_limits<float>::epsilon());
    }
private:
    static double Norm(double a, double b, double c, double d) noexcept {
        return .5 * (std::hypot(a + d, c - b) + std::hypot(a - d, c + b));
    }
    double Fail() noexcept {
        state_ = std::numeric_limits<double>::infinity();
        return state_;
    }
    double alpha_ = 0, delta_ = 1, state_ = 0;
    bool valid_ = false;
};

} // namespace harken
#endif
