// Biquad filters for the tone stage (LPF + HPF). Coefficients follow the
// standard audio EQ cookbook formulation (Robert Bristow-Johnson, "Cookbook
// formulae for audio EQ biquad filter coefficients"); the section is
// realized in transposed direct form II, which keeps two state variables
// per channel and good float behavior.
//
// Pure math: no allocation, audio-thread safe. Coefficient construction
// uses transcendental functions and normally runs at control rate.
#ifndef HARKEN_DSP_BIQUAD_H
#define HARKEN_DSP_BIQUAD_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace harken {

struct BiquadCoeffs {
    // Normalized transfer function
    //   H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
};

struct BiquadTrigPoint {
    double sine = 0.0;
    double cosine = 1.0;
};

// Immutable control-rate trigonometric table. Construction happens with the
// rest of engine initialization on the logic thread. Render workers only read
// it, so parameter-domain tone glides never call sin/cos/tan on an audio path.
struct BiquadTrigTable {
    static constexpr std::size_t kPointCount = 4097;
    std::array<float, kPointCount> sine{};
    std::array<float, kPointCount> cosine{};

    BiquadTrigTable() {
        for (std::size_t i = 0; i < kPointCount; ++i) {
            const double w = 3.14159265358979323846 *
                             static_cast<double>(i) /
                             static_cast<double>(kPointCount - 1);
            sine[i] = static_cast<float>(std::sin(w));
            cosine[i] = static_cast<float>(std::cos(w));
        }
    }

    BiquadTrigPoint Lookup(float sampleRate, float frequencyHz) const {
        const double normalized =
            std::clamp(static_cast<double>(frequencyHz) /
                           (0.5 * static_cast<double>(sampleRate)),
                       0.0, 1.0);
        const double position =
            normalized * static_cast<double>(kPointCount - 1);
        const std::size_t index = static_cast<std::size_t>(position);
        const std::size_t next =
            std::min(index + 1, static_cast<std::size_t>(kPointCount - 1));
        const double fraction = position - static_cast<double>(index);
        BiquadTrigPoint point;
        point.sine = static_cast<double>(sine[index]) +
                     (static_cast<double>(sine[next]) -
                      static_cast<double>(sine[index])) *
                         fraction;
        point.cosine = static_cast<double>(cosine[index]) +
                       (static_cast<double>(cosine[next]) -
                        static_cast<double>(cosine[index])) *
                           fraction;
        return point;
    }
};

// Estimates the number of frames needed for a biquad's homogeneous response
// to fall to approximately -60 dB. The denominator roots determine the
// slowest pole; an unstable or non-finite section uses the all-ones sentinel
// convention so the render layer can treat it as infinite.
inline std::uint32_t EstimateBiquadTailFrames(const BiquadCoeffs& c) {
    constexpr std::uint32_t infinite =
        (std::numeric_limits<std::uint32_t>::max)();
    const double a1 = static_cast<double>(c.a1);
    const double a2 = static_cast<double>(c.a2);
    if (!std::isfinite(a1) || !std::isfinite(a2)) {
        return infinite;
    }

    const double discriminant = a1 * a1 - 4.0 * a2;
    double radius = 0.0;
    if (discriminant < 0.0) {
        if (!(a2 >= 0.0)) {
            return infinite;
        }
        radius = std::sqrt(a2);
    } else {
        const double root = std::sqrt(discriminant);
        const double r0 = (-a1 + root) * 0.5;
        const double r1 = (-a1 - root) * 0.5;
        radius = std::max(std::fabs(r0), std::fabs(r1));
    }
    if (!std::isfinite(radius) || radius >= 1.0) {
        return infinite;
    }
    if (!(radius > 0.0)) {
        return 0;
    }

    constexpr double kMinus60DbMagnitude = 6.90775527898213705205;
    const double frames =
        std::ceil(kMinus60DbMagnitude / (1.0 - radius));
    if (!std::isfinite(frames) || frames >=
                                     static_cast<double>(infinite - 1u)) {
        return infinite - 1u;
    }
    return frames > 0.0 ? static_cast<std::uint32_t>(frames) : 0u;
}

// Coefficient derivation runs in double and stores float: the gain
// identities of these sections divide near-cancelling coefficient sums
// (e.g. the lowpass DC denominator 1 + a1 + a2 collapses two ~1.8
// magnitude terms to ~0.016), so any rounding accumulated while deriving
// is amplified by that cancellation. Double derivation leaves each stored
// coefficient within half a float ulp of exact; the runtime TDF2 tick
// stays float.

// Cookbook lowpass:
//   w0 = 2*pi*f0/Fs, alpha = sin(w0)/(2*Q)
//   b0 = (1-cos w0)/2, b1 = 1-cos w0, b2 = b0
//   a0 = 1+alpha, a1 = -2 cos w0, a2 = 1-alpha   (normalized by a0)
inline BiquadCoeffs MakeLowpass(float sampleRate, float cutoffHz, float q) {
    const double w0 = 2.0 * 3.14159265358979323846 *
                      static_cast<double>(cutoffHz) /
                      static_cast<double>(sampleRate);
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(q));
    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = static_cast<float>((1.0 - cosW0) * 0.5 / a0);
    c.b1 = static_cast<float>((1.0 - cosW0) / a0);
    c.b2 = c.b0;
    c.a1 = static_cast<float>(-2.0 * cosW0 / a0);
    c.a2 = static_cast<float>((1.0 - alpha) / a0);
    return c;
}

// Cookbook highpass:
//   b0 = (1+cos w0)/2, b1 = -(1+cos w0), b2 = b0, same denominator.
inline BiquadCoeffs MakeHighpass(float sampleRate, float cutoffHz, float q) {
    const double w0 = 2.0 * 3.14159265358979323846 *
                      static_cast<double>(cutoffHz) /
                      static_cast<double>(sampleRate);
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(q));
    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = static_cast<float>((1.0 + cosW0) * 0.5 / a0);
    c.b1 = static_cast<float>(-(1.0 + cosW0) / a0);
    c.b2 = c.b0;
    c.a1 = static_cast<float>(-2.0 * cosW0 / a0);
    c.a2 = static_cast<float>((1.0 - alpha) / a0);
    return c;
}

// Cookbook low shelf (same public source; math integrated from the
// project's seed kernel collection):
//   A = 10^(gainDb/40), beta = 2*sqrt(A)*alpha
//   b0 =    A*((A+1) - (A-1)cos w0 + beta)
//   b1 =  2*A*((A-1) - (A+1)cos w0)
//   b2 =    A*((A+1) - (A-1)cos w0 - beta)
//   a0 =       (A+1) + (A-1)cos w0 + beta   (normalizes the section)
//   a1 =   -2*((A-1) + (A+1)cos w0)
//   a2 =       (A+1) + (A-1)cos w0 - beta
// Boosts (or cuts) below cutoffHz by gainDb, unity above. gainDb = 0
// derives an identity section, but callers bypass instead (exactness by
// omission beats exactness by cancellation).
inline BiquadCoeffs MakeLowShelf(float sampleRate, float cutoffHz, float q,
                                 float gainDb) {
    const double w0 = 2.0 * 3.14159265358979323846 *
                      static_cast<double>(cutoffHz) /
                      static_cast<double>(sampleRate);
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(q));
    const double A =
        std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0;
    const double am1 = A - 1.0;
    const double a0 = ap1 + am1 * cosW0 + beta;
    BiquadCoeffs c;
    c.b0 = static_cast<float>(A * (ap1 - am1 * cosW0 + beta) / a0);
    c.b1 = static_cast<float>(2.0 * A * (am1 - ap1 * cosW0) / a0);
    c.b2 = static_cast<float>(A * (ap1 - am1 * cosW0 - beta) / a0);
    c.a1 = static_cast<float>(-2.0 * (am1 + ap1 * cosW0) / a0);
    c.a2 = static_cast<float>((ap1 + am1 * cosW0 - beta) / a0);
    return c;
}

// Cookbook peaking EQ (same public source):
//   A = 10^(gainDb/40), w0, alpha = sin(w0)/(2*Q)
//   b0 = 1 + alpha*A,  b1 = -2 cos w0,  b2 = 1 - alpha*A
//   a0 = 1 + alpha/A,  a1 = -2 cos w0,  a2 = 1 - alpha/A   (normalized by a0)
// Boosts (or cuts) a band around cutoffHz by gainDb, unity far from it; Q sets
// the bandwidth. gainDb = 0 derives an identity section.
inline BiquadCoeffs MakePeaking(float sampleRate, float cutoffHz, float q,
                                float gainDb) {
    const double w0 = 2.0 * 3.14159265358979323846 *
                      static_cast<double>(cutoffHz) /
                      static_cast<double>(sampleRate);
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(q));
    const double A =
        std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double a0 = 1.0 + alpha / A;
    BiquadCoeffs c;
    c.b0 = static_cast<float>((1.0 + alpha * A) / a0);
    c.b1 = static_cast<float>(-2.0 * cosW0 / a0);
    c.b2 = static_cast<float>((1.0 - alpha * A) / a0);
    c.a1 = static_cast<float>(-2.0 * cosW0 / a0);
    c.a2 = static_cast<float>((1.0 - alpha / A) / a0);
    return c;
}

// Cookbook high shelf (same public source):
//   A = 10^(gainDb/40), beta = 2*sqrt(A)*alpha
//   b0 =    A*((A+1) + (A-1)cos w0 + beta)
//   b1 = -2*A*((A-1) + (A+1)cos w0)
//   b2 =    A*((A+1) + (A-1)cos w0 - beta)
//   a0 =       (A+1) - (A-1)cos w0 + beta   (normalizes the section)
//   a1 =    2*((A-1) - (A+1)cos w0)
//   a2 =       (A+1) - (A-1)cos w0 - beta
// Boosts (or cuts) above cutoffHz by gainDb, unity below. gainDb = 0 derives
// an identity section.
inline BiquadCoeffs MakeHighShelf(float sampleRate, float cutoffHz, float q,
                                  float gainDb) {
    const double w0 = 2.0 * 3.14159265358979323846 *
                      static_cast<double>(cutoffHz) /
                      static_cast<double>(sampleRate);
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(q));
    const double A =
        std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0;
    const double am1 = A - 1.0;
    const double a0 = ap1 - am1 * cosW0 + beta;
    BiquadCoeffs c;
    c.b0 = static_cast<float>(A * (ap1 + am1 * cosW0 + beta) / a0);
    c.b1 = static_cast<float>(-2.0 * A * (am1 + ap1 * cosW0) / a0);
    c.b2 = static_cast<float>(A * (ap1 + am1 * cosW0 - beta) / a0);
    c.a1 = static_cast<float>(2.0 * (am1 - ap1 * cosW0) / a0);
    c.a2 = static_cast<float>((ap1 - am1 * cosW0 - beta) / a0);
    return c;
}

inline BiquadCoeffs MakeLowpassFromTrig(const BiquadTrigPoint& trig,
                                        float q) {
    const double alpha = trig.sine / (2.0 * static_cast<double>(q));
    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = static_cast<float>((1.0 - trig.cosine) * 0.5 / a0);
    c.b1 = static_cast<float>((1.0 - trig.cosine) / a0);
    c.b2 = c.b0;
    c.a1 = static_cast<float>(-2.0 * trig.cosine / a0);
    c.a2 = static_cast<float>((1.0 - alpha) / a0);
    return c;
}

inline BiquadCoeffs MakeHighpassFromTrig(const BiquadTrigPoint& trig,
                                         float q) {
    const double alpha = trig.sine / (2.0 * static_cast<double>(q));
    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = static_cast<float>((1.0 + trig.cosine) * 0.5 / a0);
    c.b1 = static_cast<float>(-(1.0 + trig.cosine) / a0);
    c.b2 = c.b0;
    c.a1 = static_cast<float>(-2.0 * trig.cosine / a0);
    c.a2 = static_cast<float>((1.0 - alpha) / a0);
    return c;
}

inline BiquadCoeffs MakeLowShelfFromTrig(const BiquadTrigPoint& trig, float q,
                                         float gainDb) {
    const double alpha = trig.sine / (2.0 * static_cast<double>(q));
    const double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0;
    const double am1 = A - 1.0;
    const double a0 = ap1 + am1 * trig.cosine + beta;
    BiquadCoeffs c;
    c.b0 =
        static_cast<float>(A * (ap1 - am1 * trig.cosine + beta) / a0);
    c.b1 = static_cast<float>(
        2.0 * A * (am1 - ap1 * trig.cosine) / a0);
    c.b2 =
        static_cast<float>(A * (ap1 - am1 * trig.cosine - beta) / a0);
    c.a1 =
        static_cast<float>(-2.0 * (am1 + ap1 * trig.cosine) / a0);
    c.a2 =
        static_cast<float>((ap1 + am1 * trig.cosine - beta) / a0);
    return c;
}

inline BiquadCoeffs MakeHighShelfFromTrig(const BiquadTrigPoint& trig,
                                          float q, float gainDb) {
    const double alpha = trig.sine / (2.0 * static_cast<double>(q));
    const double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0;
    const double am1 = A - 1.0;
    const double a0 = ap1 - am1 * trig.cosine + beta;
    BiquadCoeffs c;
    c.b0 =
        static_cast<float>(A * (ap1 + am1 * trig.cosine + beta) / a0);
    c.b1 = static_cast<float>(
        -2.0 * A * (am1 + ap1 * trig.cosine) / a0);
    c.b2 =
        static_cast<float>(A * (ap1 + am1 * trig.cosine - beta) / a0);
    c.a1 =
        static_cast<float>(2.0 * (am1 - ap1 * trig.cosine) / a0);
    c.a2 =
        static_cast<float>((ap1 - am1 * trig.cosine - beta) / a0);
    return c;
}

struct BiquadParameterGlide {
    float currentLog2Hz = 0.0f;
    float currentLog2Q = 0.0f;
    float currentGainDb = 0.0f;
    float startLog2Hz = 0.0f;
    float startLog2Q = 0.0f;
    float startGainDb = 0.0f;
    float targetLog2Hz = 0.0f;
    float targetLog2Q = 0.0f;
    float targetGainDb = 0.0f;
    std::uint32_t progress = 0;
    std::uint32_t total = 0;
    bool active = false;

    void Snap(float frequencyHz, float q, float gainDb) {
        currentLog2Hz = std::log2(frequencyHz);
        currentLog2Q = std::log2(q);
        currentGainDb = gainDb;
        startLog2Hz = targetLog2Hz = currentLog2Hz;
        startLog2Q = targetLog2Q = currentLog2Q;
        startGainDb = targetGainDb = gainDb;
        progress = 0;
        total = 0;
        active = false;
    }

    void Retarget(float frequencyHz, float q, float gainDb,
                  std::uint32_t frames) {
        startLog2Hz = currentLog2Hz;
        startLog2Q = currentLog2Q;
        startGainDb = currentGainDb;
        targetLog2Hz = std::log2(frequencyHz);
        targetLog2Q = std::log2(q);
        targetGainDb = gainDb;
        progress = 0;
        total = frames;
        active = frames != 0;
        if (!active) {
            currentLog2Hz = targetLog2Hz;
            currentLog2Q = targetLog2Q;
            currentGainDb = targetGainDb;
        }
    }

    void Advance(std::uint32_t frames) {
        if (!active) {
            return;
        }
        progress += std::min(frames, total - progress);
        const float amount = total == 0
                                 ? 1.0f
                                 : static_cast<float>(progress) /
                                       static_cast<float>(total);
        currentLog2Hz =
            startLog2Hz + (targetLog2Hz - startLog2Hz) * amount;
        currentLog2Q =
            startLog2Q + (targetLog2Q - startLog2Q) * amount;
        currentGainDb =
            startGainDb + (targetGainDb - startGainDb) * amount;
        active = progress < total;
    }

    float FrequencyHz() const { return std::exp2(currentLog2Hz); }
    float Q() const { return std::exp2(currentLog2Q); }
};

// Per-channel filter memory (transposed direct form II).
struct BiquadState {
    float z1 = 0.0f, z2 = 0.0f;

    void Reset() { z1 = z2 = 0.0f; }
};

inline float BiquadTick(const BiquadCoeffs& c, BiquadState& s, float x);

inline bool BiquadCoeffsEqual(const BiquadCoeffs& lhs,
                              const BiquadCoeffs& rhs) {
    return lhs.b0 == rhs.b0 && lhs.b1 == rhs.b1 && lhs.b2 == rhs.b2 &&
           lhs.a1 == rhs.a1 && lhs.a2 == rhs.a2;
}

// The Schur conditions are a compact sufficient and necessary test for a
// real second-order denominator. Keeping the strict inequalities here makes
// every coefficient set crossing into the render path explicitly stable.
inline bool IsBiquadStable(const BiquadCoeffs& c) {
    if (!std::isfinite(c.b0) || !std::isfinite(c.b1) ||
        !std::isfinite(c.b2) || !std::isfinite(c.a1) ||
        !std::isfinite(c.a2)) {
        return false;
    }
    return std::fabs(c.a2) < 1.0f &&
           std::fabs(c.a1) < (1.0f + c.a2);
}

inline void SeedBiquadStateForDc(BiquadState& state,
                                 const BiquadCoeffs& coeffs, float input) {
    const float denominator = 1.0f + coeffs.a1 + coeffs.a2;
    if (!std::isfinite(denominator) || std::fabs(denominator) < 1.0e-8f ||
        !std::isfinite(input)) {
        state.Reset();
        return;
    }
    const float output =
        (coeffs.b0 + coeffs.b1 + coeffs.b2) / denominator * input;
    if (!std::isfinite(output)) {
        state.Reset();
        return;
    }
    const float z2 = coeffs.b2 * input - coeffs.a2 * output;
    const float z1 = coeffs.b1 * input - coeffs.a1 * output + z2;
    if (!std::isfinite(z1) || !std::isfinite(z2)) {
        state.Reset();
        return;
    }
    state.z1 = z1;
    state.z2 = z2;
}

inline std::uint32_t BiquadSmoothingFrames(std::uint32_t sampleRate) {
    const std::uint64_t frames =
        (static_cast<std::uint64_t>(sampleRate) * 10u + 999u) / 1000u;
    constexpr std::uint64_t cap =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
    return static_cast<std::uint32_t>(frames > cap ? cap : frames);
}

inline std::uint32_t SaturatingBiquadTailAdd(std::uint32_t aggregate,
                                             std::uint32_t contribution) {
    constexpr std::uint32_t infinite =
        (std::numeric_limits<std::uint32_t>::max)();
    if (aggregate == infinite || contribution == infinite) {
        return infinite;
    }
    constexpr std::uint32_t finiteCap = infinite - 1u;
    if (contribution > finiteCap - aggregate) {
        return finiteCap;
    }
    return aggregate + contribution;
}

template <std::size_t Channels>
struct BiquadTransitionState {
    bool transitioning = false;
    BiquadCoeffs targetCoeffs;
    std::uint32_t targetTailFrames = 0;
    bool targetEnabled = false;
    BiquadState targetState[Channels];
    std::uint32_t progress = 0;
    std::uint32_t total = 0;
    std::uint32_t durationFrames = 1;

    bool pendingValid = false;
    BiquadCoeffs pendingCoeffs;
    std::uint32_t pendingTailFrames = 0;
    bool pendingEnabled = false;

    float lastInput[Channels] = {};

    void Reset() {
        transitioning = false;
        targetCoeffs = BiquadCoeffs{};
        targetTailFrames = 0;
        targetEnabled = false;
        progress = 0;
        total = 0;
        durationFrames = 1;
        pendingValid = false;
        pendingCoeffs = BiquadCoeffs{};
        pendingTailFrames = 0;
        pendingEnabled = false;
        for (std::size_t ch = 0; ch < Channels; ++ch) {
            targetState[ch].Reset();
            lastInput[ch] = 0.0f;
        }
    }
};

template <std::size_t Channels>
inline void StartBiquadTransition(BiquadTransitionState<Channels>& transition,
                                  bool targetEnabled,
                                  const BiquadCoeffs& targetCoeffs,
                                  std::uint32_t targetTailFrames,
                                  std::uint32_t totalFrames) {
    transition.targetEnabled = targetEnabled;
    transition.targetCoeffs = targetCoeffs;
    transition.targetTailFrames = targetTailFrames;
    transition.total = totalFrames > 0 ? totalFrames : 1u;
    transition.durationFrames = transition.total;
    transition.progress = 0;
    transition.transitioning = true;
    for (std::size_t ch = 0; ch < Channels; ++ch) {
        transition.targetState[ch].Reset();
        if (targetEnabled) {
            SeedBiquadStateForDc(transition.targetState[ch], targetCoeffs,
                                 transition.lastInput[ch]);
        }
    }
}

template <std::size_t Channels>
inline void RequestBiquadTransition(
    BiquadTransitionState<Channels>& transition, bool activeEnabled,
    const BiquadCoeffs& activeCoeffs, std::uint32_t targetTailFrames,
    bool targetEnabled, const BiquadCoeffs& targetCoeffs,
    std::uint32_t totalFrames) {
    if (!transition.transitioning && activeEnabled == targetEnabled &&
        (!targetEnabled || BiquadCoeffsEqual(activeCoeffs, targetCoeffs))) {
        transition.pendingValid = false;
        return;
    }
    if (transition.transitioning) {
        if (transition.targetEnabled == targetEnabled &&
            (!targetEnabled ||
             BiquadCoeffsEqual(transition.targetCoeffs, targetCoeffs))) {
            transition.pendingValid = false;
            return;
        }
        transition.pendingEnabled = targetEnabled;
        transition.pendingCoeffs = targetCoeffs;
        transition.pendingTailFrames = targetTailFrames;
        transition.pendingValid = true;
        return;
    }
    StartBiquadTransition(transition, targetEnabled, targetCoeffs,
                          targetTailFrames, totalFrames);
}

template <std::size_t Channels>
inline void SnapBiquadTransition(BiquadTransitionState<Channels>& transition,
                                 bool enabled) {
    transition.transitioning = false;
    transition.pendingValid = false;
    transition.targetEnabled = false;
    transition.progress = 0;
    transition.total = 0;
    transition.durationFrames = 1;
    if (!enabled) {
        transition.targetTailFrames = 0;
    }
}

template <std::size_t Channels>
inline std::uint32_t BiquadTransitionTailFrames(
    const BiquadTransitionState<Channels>& transition, bool activeEnabled,
    std::uint32_t activeTailFrames) {
    if (!transition.transitioning) {
        return activeEnabled ? activeTailFrames : 0u;
    }
    const std::uint32_t remaining =
        transition.total > transition.progress
            ? transition.total - transition.progress
            : 0u;
    const std::uint32_t branchTail =
        std::max(activeTailFrames, transition.targetTailFrames);
    std::uint32_t result = SaturatingBiquadTailAdd(remaining, branchTail);
    if (transition.pendingValid) {
        result = SaturatingBiquadTailAdd(
            result, SaturatingBiquadTailAdd(transition.total,
                                             transition.pendingTailFrames));
    }
    return result;
}

template <std::size_t Channels>
inline std::uint32_t BiquadSectionTailFrames(
    const BiquadTransitionState<Channels>& transition,
    const BiquadParameterGlide& glide, bool activeEnabled,
    std::uint32_t activeTailFrames) {
    const std::uint32_t transitionTail = BiquadTransitionTailFrames(
        transition, activeEnabled, activeTailFrames);
    if (!glide.active) {
        return transitionTail;
    }
    const std::uint32_t remaining =
        glide.total > glide.progress ? glide.total - glide.progress : 0u;
    const std::uint32_t branchTail =
        std::max(activeTailFrames, transition.targetTailFrames);
    return std::max(
        transitionTail,
        SaturatingBiquadTailAdd(remaining, branchTail));
}

template <std::size_t Channels>
inline void BiquadProcessSmoothed(
    BiquadTransitionState<Channels>& transition, bool& activeEnabled,
    BiquadCoeffs& activeCoeffs, BiquadState (&activeState)[Channels],
    std::uint32_t& activeTailFrames, float* const* channels,
    std::uint32_t channelCount, std::uint32_t offset,
    std::uint32_t frames) {
    const std::uint32_t count =
        std::min(channelCount, static_cast<std::uint32_t>(Channels));
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t ch = 0; ch < count; ++ch) {
            const float input = channels[ch][offset + i];
            transition.lastInput[ch] = input;
            const float source = activeEnabled
                                     ? BiquadTick(activeCoeffs,
                                                  activeState[ch], input)
                                     : input;
            if (!transition.transitioning) {
                channels[ch][offset + i] = source;
                continue;
            }
            const float target = transition.targetEnabled
                                     ? BiquadTick(transition.targetCoeffs,
                                                  transition.targetState[ch],
                                                  input)
                                     : input;
            // Uniform ramp over the transition's total frames: progress 0
            // (the first transitioning sample) lands exactly on the source,
            // progress total-1 (the last) lands exactly on the target. A
            // single-frame transition has no room for a ramp, so it snaps
            // straight to the target.
            const float mix =
                transition.total <= 1u
                    ? 1.0f
                    : static_cast<float>(transition.progress) /
                          static_cast<float>(transition.total - 1u);
            channels[ch][offset + i] = source + (target - source) * mix;
        }
        if (transition.transitioning) {
            ++transition.progress;
            if (transition.progress >= transition.total) {
                activeEnabled = transition.targetEnabled;
                activeState[0] = transition.targetState[0];
                for (std::uint32_t ch = 1; ch < count; ++ch) {
                    activeState[ch] = transition.targetState[ch];
                }
                if (transition.targetEnabled) {
                    activeCoeffs = transition.targetCoeffs;
                    activeTailFrames = transition.targetTailFrames;
                }
                transition.transitioning = false;
                transition.progress = 0;
                transition.total = 0;
                if (transition.pendingValid) {
                    const bool pendingEnabled = transition.pendingEnabled;
                    const BiquadCoeffs pendingCoeffs =
                        transition.pendingCoeffs;
                    const std::uint32_t pendingTail =
                        transition.pendingTailFrames;
                    transition.pendingValid = false;
                    StartBiquadTransition(transition, pendingEnabled,
                                          pendingCoeffs, pendingTail,
                                          transition.durationFrames);
                }
            }
        }
    }
}

inline float BiquadTick(const BiquadCoeffs& c, BiquadState& s, float x) {
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

inline void BiquadProcess(const BiquadCoeffs& c, BiquadState& s, float* buffer,
                          std::uint32_t frames) {
    for (std::uint32_t i = 0; i < frames; ++i) {
        buffer[i] = BiquadTick(c, s, buffer[i]);
    }
}

} // namespace harken

#endif // HARKEN_DSP_BIQUAD_H
