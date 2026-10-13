// Variable-rate interpolation kernels for the resample stage.
//
// Three families, in increasing quality and cost:
//   1. Linear 2-tap            (~2.5 ops/sample)
//   2. Hermite 4-point cubic   (~5 ops/sample)
//   3. Kaiser-windowed sinc polyphase FIR, 8-tap and 16-tap, 256 phases,
//      with inter-phase linear interpolation for continuous phase resolution
//      (~10 / ~18 ops/sample)
//
// The math uses standard, publicly documented DSP formulations with no
// proprietary origin (Catmull-Rom / Hermite cubic; Kaiser-windowed sinc
// polyphase). Table ownership and memory placement are this project's design:
// tables are built off the audio thread at engine init and shared read-only.
// References: J. O. Smith, Digital Audio Resampling (CCRMA); Crochiere and
// Rabiner, Multirate Digital Signal Processing.
//
// All functions are pure and allocation-free; table init runs off the audio
// thread (uses std::sin/sqrt), interpolation is audio-thread safe.
#ifndef HARKEN_DSP_INTERP_KERNELS_H
#define HARKEN_DSP_INTERP_KERNELS_H

#include <cmath>
#include <cstdint>

namespace harken {

// ---------------------------------------------------------------------------
// Linear 2-tap.
//   s0 = sample at floor(pos), s1 = sample at floor(pos)+1, frac in [0, 1).
// ---------------------------------------------------------------------------
inline float InterpLinear(float s0, float s1, float frac)
{
    // Preserve the exact endpoint before subtracting. This also keeps signed
    // infinity available for the device-boundary sanitizer instead of turning
    // equal infinities into NaN through inf - inf.
    if (frac == 0.0f)
        return s0;
    return s0 + frac * (s1 - s0);
}

// ---------------------------------------------------------------------------
// Hermite 4-point cubic.
//   sm1..s2 = samples at floor(pos)-1 .. floor(pos)+2, frac in [0, 1).
// Catmull-Rom-family coefficients evaluated in Horner form.
// ---------------------------------------------------------------------------
inline float InterpHermite(float sm1, float s0, float s1, float s2, float frac)
{
    const float c0 = s0;
    const float c1 = 0.5f * (s1 - sm1);
    const float c2 = sm1 - 2.5f * s0 + 2.0f * s1 - 0.5f * s2;
    const float c3 = 0.5f * (s2 - sm1) + 1.5f * (s0 - s1);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

// ---------------------------------------------------------------------------
// Modified Bessel function of the first kind, order 0 (25-term series).
// Sufficient precision for Kaiser window generation.
// ---------------------------------------------------------------------------
inline double BesselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double xHalf = x * 0.5;
    for (int k = 1; k <= 25; ++k)
    {
        term *= xHalf / static_cast<double>(k);
        sum += term * term;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Kaiser-windowed sinc polyphase coefficient table.
//
//   NumTaps  : FIR length per phase (even; center tap = NumTaps/2 - 1, i.e.
//              the "current" sample sits at index NumTaps/2 - 1 of the input
//              window).
//   kPhases  : 256 phase rows; interpolation lerps between adjacent rows so
//              effective phase resolution is continuous (removes the phase
//              quantization noise floor).
//
// Each row is normalized to unity DC gain (taps sum to 1) so pure resampling
// is gain-transparent.
//
// Proven parameter pairs from prior tuning:
//   16 taps, beta = 9.0  -> THD below -85 dB   (highest quality tier)
//    8 taps, beta = 5.0  -> mid quality tier
// ---------------------------------------------------------------------------
template <int NumTaps>
struct KaiserSincTable
{
    static_assert(NumTaps >= 4 && (NumTaps % 2) == 0, "even tap count >= 4");

    static constexpr int kPhases    = 256;
    static constexpr int kTapCenter = NumTaps / 2 - 1;

    float coeff[kPhases][NumTaps] = {};

    // Off-audio-thread. Fills and normalizes all phase rows.
    void Init(double beta)
    {
        constexpr double kPi = 3.14159265358979323846;
        const double halfWidth = (NumTaps - 1) * 0.5;
        const double i0Beta = BesselI0(beta);

        for (int p = 0; p < kPhases; ++p)
        {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0.0;

            for (int t = 0; t < NumTaps; ++t)
            {
                const double x = static_cast<double>(t - kTapCenter) - frac;

                const double sinc = (std::abs(x) < 1e-10)
                    ? 1.0
                    : std::sin(kPi * x) / (kPi * x);

                const double r = x / halfWidth;
                const double window = (std::abs(r) >= 1.0)
                    ? 0.0
                    : BesselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta;

                const double c = sinc * window;
                coeff[p][t] = static_cast<float>(c);
                sum += c;
            }

            if (sum > 1e-15)
            {
                const float invSum = static_cast<float>(1.0 / sum);
                for (int t = 0; t < NumTaps; ++t)
                    coeff[p][t] *= invSum;
            }
        }
    }
};

// Samples the stored Kaiser-sinc prototype at an arbitrary coordinate. The
// table represents x as tap - center - phase; choosing ceil(x + center) as
// the tap therefore leaves a phase in [0, 1), which is resolved with the same
// inter-phase interpolation used by InterpPolyphase.
template <int NumTaps>
inline float SampleKaiserSincPrototype(
    const KaiserSincTable<NumTaps>& table, float x)
{
    constexpr float kHalfWidth = static_cast<float>(NumTaps - 1) * 0.5f;
    if (x <= -kHalfWidth || x >= kHalfWidth)
        return 0.0f;

    const float shifted = x + static_cast<float>(table.kTapCenter);
    int tap = static_cast<int>(shifted);
    if (static_cast<float>(tap) < shifted)
        ++tap;
    const float frac = static_cast<float>(tap) - shifted;

    const float phaseF = frac * static_cast<float>(table.kPhases);
    int p0 = static_cast<int>(phaseF);
    float mu = phaseF - static_cast<float>(p0);

    if (p0 >= table.kPhases - 1) { p0 = table.kPhases - 2; mu = 1.0f; }
    if (p0 < 0)                  { p0 = 0;                 mu = 0.0f; }

    return table.coeff[p0][tap] +
           mu * (table.coeff[p0 + 1][tap] - table.coeff[p0][tap]);
}

// ---------------------------------------------------------------------------
// Polyphase interpolation with inter-phase coefficient lerp.
//   samples : NumTaps contiguous samples; samples[kTapCenter] is the sample
//             at floor(pos).
//   frac    : fractional position in [0, 1).
// Scalar reference form; a SIMD variant is a drop-in dot-product replacement.
// ---------------------------------------------------------------------------
template <int NumTaps>
inline float InterpPolyphase(const KaiserSincTable<NumTaps>& table,
                             const float* samples, float frac)
{
    const float phaseF = frac * static_cast<float>(table.kPhases);
    int p0 = static_cast<int>(phaseF);
    float mu = phaseF - static_cast<float>(p0);

    if (p0 >= table.kPhases - 1) { p0 = table.kPhases - 2; mu = 1.0f; }
    if (p0 < 0)                  { p0 = 0;                 mu = 0.0f; }

    const float* a = table.coeff[p0];
    const float* b = table.coeff[p0 + 1];

    float sum = 0.0f;
    for (int t = 0; t < NumTaps; ++t)
    {
        const float c = a[t] + mu * (b[t] - a[t]);
        sum += c * samples[t];
    }
    return sum;
}

} // namespace harken

#endif // HARKEN_DSP_INTERP_KERNELS_H
