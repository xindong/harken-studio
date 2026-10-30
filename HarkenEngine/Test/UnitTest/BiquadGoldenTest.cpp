// Golden cases: biquad LPF/HPF with known cookbook coefficients against
// closed-form expectations.
//
// The expectation path is entirely test-side: coefficients are re-derived
// here in double directly from the published cookbook formulas (Robert
// Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
// coefficients") and the magnitude is the modulus of the rational transfer
// function evaluated on the unit circle with std::complex. No production
// DSP code participates in computing an expected value; production code
// appears only as the unit under test.
//
// Tolerances:
//   - Production coefficients vs the double-precision rederivation, and
//     the HPF Nyquist gain identity (well-conditioned coefficient sums):
//     1e-6.
//   - LPF DC gain identity: 6e-6. The denominator 1 + a1 + a2 cancels
//     ~1.8-magnitude terms down to 2(1-cos w0)/a0 ~= 0.0157, amplifying
//     the half-ulp float storage rounding of a1 and a2 by ~64x; the
//     worst-case bound is derived at the case.
//   - Sine steady-state amplitude vs analytic |H(e^{jw})|: 1e-3. Amplitude
//     is estimated as RMS * sqrt(2) over an integer number of periods,
//     which is exact for a sampled sine (a peak search would quantize by
//     up to one sample and fail at high frequencies).
//   - LPF DC settling: 1e-4 against the exact input level.
//   - HPF DC rejection: the observation points are the non-silent charging
//     transient (first quarter, peak > 0.1) and the decayed tail (last
//     quarter, peak < 1e-3); the comparison is transient vs decay, never
//     silence vs silence.
#include "harken/dsp/Biquad.h"

#include "Common/SignalTestSupport.h"

#include <algorithm>
#include <complex>
#include <limits>

namespace {

using harken::BiquadCoeffs;
using harken::BiquadProcess;
using harken::BiquadState;

constexpr float kRate = 48000.0f;
constexpr double kPi = 3.14159265358979323846;

// Reference coefficients, rederived from the cookbook text independently of
// the production MakeLowpass/MakeHighpass implementations:
//   w0 = 2*pi*f0/Fs, alpha = sin(w0)/(2*Q), all normalized by a0 = 1+alpha
//   LPF: b = [(1-cos w0)/2, 1-cos w0, (1-cos w0)/2]
//   HPF: b = [(1+cos w0)/2, -(1+cos w0), (1+cos w0)/2]
//   shared denominator: a1 = -2 cos w0, a2 = 1-alpha
struct RefCoeffs {
    double b0, b1, b2, a1, a2;
};

RefCoeffs RefLowpass(double rate, double cutoff, double q) {
    const double w0 = 2.0 * kPi * cutoff / rate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    return {(1.0 - cosW0) * 0.5 / a0, (1.0 - cosW0) / a0,
            (1.0 - cosW0) * 0.5 / a0, -2.0 * cosW0 / a0,
            (1.0 - alpha) / a0};
}

RefCoeffs RefHighpass(double rate, double cutoff, double q) {
    const double w0 = 2.0 * kPi * cutoff / rate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    return {(1.0 + cosW0) * 0.5 / a0, -(1.0 + cosW0) / a0,
            (1.0 + cosW0) * 0.5 / a0, -2.0 * cosW0 / a0,
            (1.0 - alpha) / a0};
}

// Shelf references, transcribed afresh from the cookbook in double:
//   A = 10^(dB/40), beta = 2*sqrt(A)*alpha, ap1 = A+1, am1 = A-1
//   low shelf:  b = A*[(ap1-am1 c + beta), 2(am1-ap1 c), (ap1-am1 c - beta)]
//               a0 = ap1 + am1 c + beta, a1 = -2(am1+ap1 c), a2 = ap1+am1 c-beta
//   high shelf: b = A*[(ap1+am1 c + beta), -2(am1+ap1 c), (ap1+am1 c - beta)]
//               a0 = ap1 - am1 c + beta, a1 = 2(am1-ap1 c), a2 = ap1-am1 c-beta
// These reproduce the section's shelf-band gain of 10^(dB/20) analytically.
RefCoeffs RefLowShelf(double rate, double f0, double q, double dB) {
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / rate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0, am1 = A - 1.0;
    const double a0 = ap1 + am1 * cosW0 + beta;
    return {A * (ap1 - am1 * cosW0 + beta) / a0,
            2.0 * A * (am1 - ap1 * cosW0) / a0,
            A * (ap1 - am1 * cosW0 - beta) / a0,
            -2.0 * (am1 + ap1 * cosW0) / a0,
            (ap1 + am1 * cosW0 - beta) / a0};
}

RefCoeffs RefHighShelf(double rate, double f0, double q, double dB) {
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / rate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0, am1 = A - 1.0;
    const double a0 = ap1 - am1 * cosW0 + beta;
    return {A * (ap1 + am1 * cosW0 + beta) / a0,
            -2.0 * A * (am1 + ap1 * cosW0) / a0,
            A * (ap1 + am1 * cosW0 - beta) / a0,
            2.0 * (am1 - ap1 * cosW0) / a0,
            (ap1 - am1 * cosW0 - beta) / a0};
}

// An exact (sine, cosine) point for a frequency: the trig table's
// interpolation removed, so a *FromTrig kernel and the cookbook reference
// differ only by the kernel's own coefficient algebra and float storage.
harken::BiquadTrigPoint ExactTrig(double rate, double freq) {
    const double w0 = 2.0 * kPi * freq / rate;
    harken::BiquadTrigPoint point;
    point.sine = std::sin(w0);
    point.cosine = std::cos(w0);
    return point;
}

RefCoeffs AsRef(const BiquadCoeffs& c) {
    return {c.b0, c.b1, c.b2, c.a1, c.a2};
}

void ExpectCoeffsNear(const RefCoeffs& r, const BiquadCoeffs& c, double tol) {
    EXPECT_NEAR(r.b0, c.b0, tol);
    EXPECT_NEAR(r.b1, c.b1, tol);
    EXPECT_NEAR(r.b2, c.b2, tol);
    EXPECT_NEAR(r.a1, c.a1, tol);
    EXPECT_NEAR(r.a2, c.a2, tol);
}

// |H(e^{jw})| as the modulus of the rational transfer function evaluated at
// z^-1 = e^{-jw}.
double RefMagnitude(const RefCoeffs& c, double w) {
    const std::complex<double> zInv = std::polar(1.0, -w);
    const std::complex<double> num = c.b0 + (c.b1 + c.b2 * zInv) * zInv;
    const std::complex<double> den = 1.0 + (c.a1 + c.a2 * zInv) * zInv;
    return std::abs(num / den);
}

// Sine amplitude over a window spanning an integer number of periods:
// RMS * sqrt(2), exact for uniformly sampled sines away from DC/Nyquist.
double SineAmplitude(const float* data, std::size_t count) {
    double sumSquares = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sumSquares += static_cast<double>(data[i]) * data[i];
    }
    return std::sqrt(2.0 * sumSquares / static_cast<double>(count));
}

TEST(BiquadGolden, LowpassCoefficientDCGainIsUnity) {
    const BiquadCoeffs c = harken::MakeLowpass(kRate, 1000.0f, 0.7071f);

    // Every production coefficient must match the double-precision
    // rederivation of the same cookbook formulas.
    const RefCoeffs r = RefLowpass(kRate, 1000.0, 0.7071);
    EXPECT_NEAR(r.b0, c.b0, 1e-6);
    EXPECT_NEAR(r.b1, c.b1, 1e-6);
    EXPECT_NEAR(r.b2, c.b2, 1e-6);
    EXPECT_NEAR(r.a1, c.a1, 1e-6);
    EXPECT_NEAR(r.a2, c.a2, 1e-6);

    // H(z=1) = (b0+b1+b2) / (1+a1+a2) is exactly 1 in exact arithmetic;
    // both sums reduce to 2(1-cos w0)/a0 ~= 0.0157 at these parameters.
    // The denominator evaluates 1 - 1.815 + 0.831 from float-stored
    // coefficients, so storage rounding of up to half an ulp each of a1
    // (5.96e-8) and a2 (2.98e-8) lands on a ~0.0157 sum: worst-case
    // relative deviation ~= 8.94e-8 / 0.0157 ~= 5.7e-6 even with
    // double-derived coefficients. 6e-6 is that storage-quantization
    // bound; it cannot tighten without storing coefficients in double.
    const double dcGain = (c.b0 + c.b1 + c.b2) / (1.0 + c.a1 + c.a2);
    EXPECT_NEAR(1.0, dcGain, 6e-6);
}

TEST(BiquadGolden, HighpassCoefficientNyquistGainIsUnity) {
    const BiquadCoeffs c = harken::MakeHighpass(kRate, 1000.0f, 0.7071f);

    const RefCoeffs r = RefHighpass(kRate, 1000.0, 0.7071);
    EXPECT_NEAR(r.b0, c.b0, 1e-6);
    EXPECT_NEAR(r.b1, c.b1, 1e-6);
    EXPECT_NEAR(r.b2, c.b2, 1e-6);
    EXPECT_NEAR(r.a1, c.a1, 1e-6);
    EXPECT_NEAR(r.a2, c.a2, 1e-6);

    // H(z=-1) = (b0-b1+b2) / (1-a1+a2) must be exactly 1 for the cookbook
    // highpass. Unlike the lowpass DC identity, this ratio is
    // well-conditioned: both sums reduce to 2(1+cos w0)/a0 ~= 3.65, so
    // coefficient storage rounding is not amplified and 1e-6 holds.
    const double nyquistGain = (c.b0 - c.b1 + c.b2) / (1.0 - c.a1 + c.a2);
    EXPECT_NEAR(1.0, nyquistGain, 1e-6);
}

TEST(BiquadGolden, LowpassSineSteadyStateMatchesAnalyticMagnitude) {
    const BiquadCoeffs c = harken::MakeLowpass(kRate, 1000.0f, 0.7071f);

    // 200 Hz sine, 0.1 s: the last fifth is fully steady-state for a
    // second-order section at this cutoff.
    const double freq = 200.0;
    std::vector<float> signal = harkentest::MakeSine(4800, freq, kRate, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(signal);

    BiquadState state;
    BiquadProcess(c, state, signal.data(), 4800);

    // 960 samples = 4 full periods at 200 Hz / 48 kHz. The expectation is
    // the test-side magnitude of the test-side coefficients.
    EXPECT_GT(harkentest::Peak(signal.data() + 3840, 960),
              harkentest::kSilenceFloor);
    const double amplitude = SineAmplitude(signal.data() + 3840, 960);
    const double expected = RefMagnitude(RefLowpass(kRate, 1000.0, 0.7071),
                                         2.0 * kPi * freq / kRate);
    EXPECT_NEAR(expected, amplitude, 1e-3);
}

TEST(BiquadGolden, HighpassSinePassbandMatchesAnalyticMagnitude) {
    const BiquadCoeffs c = harken::MakeHighpass(kRate, 500.0f, 0.7071f);

    const double freq = 8000.0;
    std::vector<float> signal = harkentest::MakeSine(4800, freq, kRate, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(signal);

    BiquadState state;
    BiquadProcess(c, state, signal.data(), 4800);

    // 960 samples = 160 full periods at 8 kHz / 48 kHz.
    EXPECT_GT(harkentest::Peak(signal.data() + 3840, 960),
              harkentest::kSilenceFloor);
    const double amplitude = SineAmplitude(signal.data() + 3840, 960);
    const double expected =
        0.8 * RefMagnitude(RefHighpass(kRate, 500.0, 0.7071),
                           2.0 * kPi * freq / kRate);
    EXPECT_NEAR(expected, amplitude, 1e-3);
}

TEST(BiquadGolden, LowpassPassesDCUnchanged) {
    const BiquadCoeffs c = harken::MakeLowpass(kRate, 1000.0f, 0.7071f);

    std::vector<float> signal = harkentest::MakeDC(4800, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(signal);

    BiquadState state;
    BiquadProcess(c, state, signal.data(), 4800);

    // Settled output equals the DC level (unity DC gain).
    for (std::size_t i = 4000; i < 4800; ++i) {
        ASSERT_NEAR(0.6f, signal[i], 1e-4f) << "sample " << i;
    }
}

TEST(BiquadGolden, HighpassRejectsDCAfterNonSilentTransient) {
    const BiquadCoeffs c = harken::MakeHighpass(kRate, 500.0f, 0.7071f);

    std::vector<float> signal = harkentest::MakeDC(4800, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(signal);

    BiquadState state;
    BiquadProcess(c, state, signal.data(), 4800);

    // Non-silent observation point: the charging transient. The first
    // output sample is b0 * 1.0 which is near unity for a highpass.
    const float transientPeak = harkentest::Peak(signal.data(), 1200);
    EXPECT_GT(transientPeak, 0.1f);

    // The DC component then decays to (numerically) nothing.
    const float tailPeak = harkentest::Peak(signal.data() + 3600, 1200);
    EXPECT_LT(tailPeak, 1e-3f);
}

TEST(BiquadGolden, TailEstimateConservativelyBoundsIndependentPoleDecay) {
    BiquadCoeffs c;
    c.a1 = -1.2f;
    c.a2 = 0.45f;
    const double a1 = static_cast<double>(c.a1);
    const double a2 = static_cast<double>(c.a2);
    const std::complex<double> discriminant(a1 * a1 - 4.0 * a2, 0.0);
    const std::complex<double> root = std::sqrt(discriminant);
    const double radius =
        std::max(std::abs((-a1 + root) * 0.5),
                 std::abs((-a1 - root) * 0.5));
    constexpr double kMinus60DbMagnitude = 6.90775527898213705205;
    const auto expected = static_cast<std::uint32_t>(
        std::ceil(kMinus60DbMagnitude / (1.0 - radius)));
    const auto exact = static_cast<std::uint32_t>(
        std::ceil(kMinus60DbMagnitude / -std::log(radius)));
    const std::uint32_t estimated = harken::EstimateBiquadTailFrames(c);

    EXPECT_GE(estimated, exact);
    EXPECT_EQ(expected, estimated);
}

TEST(BiquadGolden, TailEstimateClassifiesStableBoundaries) {
    BiquadCoeffs immediate;
    immediate.a1 = 0.0f;
    immediate.a2 = 0.0f;
    EXPECT_EQ(0u, harken::EstimateBiquadTailFrames(immediate));

    BiquadCoeffs unitPole;
    unitPole.a1 = -1.0f;
    unitPole.a2 = 0.0f;
    EXPECT_EQ((std::numeric_limits<std::uint32_t>::max)(),
              harken::EstimateBiquadTailFrames(unitPole));

    BiquadCoeffs nonFinite;
    nonFinite.a1 = std::numeric_limits<float>::infinity();
    EXPECT_EQ((std::numeric_limits<std::uint32_t>::max)(),
              harken::EstimateBiquadTailFrames(nonFinite));
}

TEST(BiquadGolden, StableTransitionHasExactEndpointsAndSampleDuration) {
    using Transition = harken::BiquadTransitionState<1>;
    Transition transition;
    harken::BiquadCoeffs active;
    harken::BiquadCoeffs target =
        harken::MakeLowpass(kRate, 1200.0f, 0.70710678f);
    bool activeEnabled = true;
    std::uint32_t activeTail = 0;
    harken::BiquadState state[1];
    float* channels[1];
    std::vector<float> signal(480, 0.0f);
    signal[0] = 1.0f;
    channels[0] = signal.data();

    harken::RequestBiquadTransition(transition, true, active, 120,
                                    true, target, 480);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail,
                                 channels, 1u, 0u, 128u);
    EXPECT_FLOAT_EQ(1.0f, signal.front());
    EXPECT_TRUE(transition.transitioning);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail,
                                 channels, 1u, 128u, 256u);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail,
                                 channels, 1u, 384u, 96u);
    EXPECT_FALSE(transition.transitioning);
    EXPECT_TRUE(harken::BiquadCoeffsEqual(active, target));
    EXPECT_EQ(480u, transition.durationFrames);
}

TEST(BiquadGolden, StableTransitionCoalescesLatestRetarget) {
    using Transition = harken::BiquadTransitionState<1>;
    Transition transition;
    harken::BiquadCoeffs active;
    const harken::BiquadCoeffs firstTarget =
        harken::MakeLowpass(kRate, 800.0f, 0.70710678f);
    const harken::BiquadCoeffs latestTarget =
        harken::MakeLowpass(kRate, 3200.0f, 0.70710678f);
    bool activeEnabled = true;
    std::uint32_t activeTail = 0;
    harken::BiquadState state[1];
    float* channels[1];
    std::vector<float> signal(1000, 0.25f);
    channels[0] = signal.data();

    harken::RequestBiquadTransition(transition, true, active, 40, true,
                                    firstTarget, 480);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail,
                                 channels, 1u, 0u, 100u);
    harken::RequestBiquadTransition(transition, true, active, 80, true,
                                    latestTarget, 480);
    EXPECT_TRUE(transition.pendingValid);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail,
                                 channels, 1u, 100u, 900u);
    EXPECT_FALSE(transition.transitioning);
    EXPECT_TRUE(harken::BiquadCoeffsEqual(active, latestTarget));
    EXPECT_FALSE(transition.pendingValid);
}

// Both sections are memoryless (b1 = b2 = a1 = a2 = 0), so BiquadTick reduces
// to y = b0 * x with no z1/z2 carryover: source and target outputs are exact
// scalar multiples of a constant input, independent of the production
// coefficient-design helpers and of any coefficient interpolation. This
// isolates the ramp shape itself — every intermediate sample is checked
// against source + (target - source) * n / (T - 1), the uniform per-sample
// weight the transition is contracted to produce.
TEST(BiquadGolden, StableTransitionRampIsUniformAcrossEveryIntermediateSample) {
    using Transition = harken::BiquadTransitionState<1>;
    Transition transition;
    harken::BiquadCoeffs active; // identity: b0 = 1
    harken::BiquadCoeffs target;
    target.b0 = 3.0f;
    bool activeEnabled = true;
    std::uint32_t activeTail = 0;
    harken::BiquadState state[1];
    float* channels[1];
    constexpr std::uint32_t kTotal = 5;
    constexpr float kInput = 2.0f;
    std::vector<float> signal(kTotal, kInput);
    channels[0] = signal.data();

    harken::RequestBiquadTransition(transition, true, active, 0, true, target,
                                   kTotal);
    ASSERT_TRUE(transition.transitioning);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail, channels, 1u, 0u, kTotal);

    constexpr float kSource = kInput;       // active.b0 * kInput
    constexpr float kTarget = 3.0f * kInput; // target.b0 * kInput
    for (std::uint32_t n = 0; n < kTotal; ++n) {
        const float mix =
            static_cast<float>(n) / static_cast<float>(kTotal - 1u);
        const float expected = kSource + (kTarget - kSource) * mix;
        EXPECT_FLOAT_EQ(expected, signal[n]) << "sample " << n;
    }
    EXPECT_FLOAT_EQ(kSource, signal[0])
        << "first transitioning sample must be pure source (t = 0)";
    EXPECT_FLOAT_EQ(kTarget, signal[kTotal - 1])
        << "last transitioning sample must be pure target (t = 1)";
    EXPECT_FALSE(transition.transitioning);
    EXPECT_TRUE(harken::BiquadCoeffsEqual(active, target));
}

// A one-frame transition has no room for a ramp: the contract snaps straight
// to the target on that single sample rather than dividing by zero or
// staying at the source.
TEST(BiquadGolden, SingleFrameTransitionIsImmediateTarget) {
    using Transition = harken::BiquadTransitionState<1>;
    Transition transition;
    harken::BiquadCoeffs active; // identity: b0 = 1
    harken::BiquadCoeffs target;
    target.b0 = 3.0f;
    bool activeEnabled = true;
    std::uint32_t activeTail = 0;
    harken::BiquadState state[1];
    float* channels[1];
    constexpr float kInput = 2.0f;
    std::vector<float> signal(1, kInput);
    channels[0] = signal.data();

    harken::RequestBiquadTransition(transition, true, active, 0, true, target,
                                   1u);
    ASSERT_TRUE(transition.transitioning);
    harken::BiquadProcessSmoothed(transition, activeEnabled, active, state,
                                 activeTail, channels, 1u, 0u, 1u);

    EXPECT_FLOAT_EQ(3.0f * kInput, signal[0])
        << "the only sample of a single-frame transition must be the target";
    EXPECT_FALSE(transition.transitioning);
    EXPECT_TRUE(harken::BiquadCoeffsEqual(active, target));
}

TEST(BiquadGolden, ParameterGlideIsLogBoundedAndInterruptedFromCurrentPoint) {
    harken::BiquadParameterGlide glide;
    glide.Snap(20.0f, 0.1f, -48.0f);
    glide.Retarget(23952.0f, 100.0f, 48.0f, 1024);
    glide.Advance(333);
    const float interruptedFrequency = glide.FrequencyHz();
    const float interruptedQ = glide.Q();
    const float interruptedGain = glide.currentGainDb;
    EXPECT_GT(interruptedFrequency, 20.0f);
    EXPECT_LT(interruptedFrequency, 23952.0f);
    EXPECT_GT(interruptedQ, 0.1f);
    EXPECT_LT(interruptedQ, 100.0f);
    EXPECT_GT(interruptedGain, -48.0f);
    EXPECT_LT(interruptedGain, 48.0f);

    glide.Retarget(80.0f, 0.2f, -12.0f, 257);
    EXPECT_FLOAT_EQ(interruptedFrequency, glide.FrequencyHz());
    EXPECT_FLOAT_EQ(interruptedQ, glide.Q());
    EXPECT_FLOAT_EQ(interruptedGain, glide.currentGainDb);
    glide.Advance(257);
    EXPECT_FALSE(glide.active);
    EXPECT_FLOAT_EQ(80.0f, glide.FrequencyHz());
    EXPECT_FLOAT_EQ(0.2f, glide.Q());
    EXPECT_FLOAT_EQ(-12.0f, glide.currentGainDb);
}

TEST(BiquadGolden, ParameterGlideIsInvariantToControlBlockSplits) {
    harken::BiquadParameterGlide whole;
    harken::BiquadParameterGlide split;
    whole.Snap(40.0f, 0.1f, -24.0f);
    split = whole;
    whole.Retarget(20000.0f, 100.0f, 36.0f, 1000);
    split.Retarget(20000.0f, 100.0f, 36.0f, 1000);
    whole.Advance(1000);
    for (std::uint32_t frames : {7u, 13u, 101u, 211u, 263u, 405u}) {
        split.Advance(frames);
    }
    EXPECT_FLOAT_EQ(whole.FrequencyHz(), split.FrequencyHz());
    EXPECT_FLOAT_EQ(whole.Q(), split.Q());
    EXPECT_FLOAT_EQ(whole.currentGainDb, split.currentGainDb);
}

TEST(BiquadGolden, ParameterGlideCompletesAtMaximumFrameDuration) {
    harken::BiquadParameterGlide glide;
    glide.Snap(100.0f, 0.5f, -12.0f);
    glide.Retarget(6400.0f, 2.0f, 6.0f, UINT32_MAX);
    glide.Advance(UINT32_MAX - 15u);
    ASSERT_TRUE(glide.active);
    glide.Advance(16);
    EXPECT_FALSE(glide.active);
    EXPECT_EQ(UINT32_MAX, glide.progress);
    EXPECT_NEAR(6400.0f, glide.FrequencyHz(), 0.01f);
    EXPECT_FLOAT_EQ(2.0f, glide.Q());
    EXPECT_FLOAT_EQ(6.0f, glide.currentGainDb);
    glide.Advance(256);
    EXPECT_EQ(UINT32_MAX, glide.progress);
}

// The glide must interpolate in stable parameter domains — log-frequency,
// log-Q, and decibel gain — never in the raw Hz / linear-Q / linear-amplitude
// domains. Steady-state endpoint checks cannot detect the domain: a
// linear-domain glide agrees with a log/dB-domain glide at both endpoints and
// diverges only in between. These cases pin the trajectory shape at interior
// points. Each expected value is rederived test-side from the geometric
// (frequency, Q) or arithmetic-decibel (gain) interpolation formula,
// independent of the production log2/exp2 path.
TEST(BiquadGolden, ParameterGlideFrequencyFollowsGeometricTrajectory) {
    constexpr double kStartHz = 100.0;
    constexpr double kTargetHz = 10000.0;
    constexpr std::uint32_t kTotal = 4800;
    harken::BiquadParameterGlide glide;
    glide.Snap(static_cast<float>(kStartHz), 1.0f, 0.0f);
    glide.Retarget(static_cast<float>(kTargetHz), 1.0f, 0.0f, kTotal);

    // Geometric interior points: t = 0.5 is the geometric mean 1 kHz, while a
    // linear-Hz ramp would read ~5.05 kHz. The quarter points are 100^1.25
    // ~= 316 Hz and 100^1.75 ~= 3162 Hz.
    constexpr std::uint32_t kStep = kTotal / 4;
    for (double amount : {0.25, 0.5, 0.75}) {
        glide.Advance(kStep);
        const double expected =
            kStartHz * std::pow(kTargetHz / kStartHz, amount);
        EXPECT_NEAR(expected, glide.FrequencyHz(), expected * 1e-3)
            << "amount " << amount;
    }
}

TEST(BiquadGolden, ParameterGlideQFollowsGeometricTrajectory) {
    constexpr double kStartQ = 0.5;
    constexpr double kTargetQ = 50.0;
    constexpr std::uint32_t kTotal = 4800;
    harken::BiquadParameterGlide glide;
    glide.Snap(1000.0f, static_cast<float>(kStartQ), 0.0f);
    glide.Retarget(1000.0f, static_cast<float>(kTargetQ), 0.0f, kTotal);

    // Geometric mean at t = 0.5 is sqrt(0.5 * 50) = 5; a linear-Q ramp would
    // read 25.25.
    constexpr std::uint32_t kStep = kTotal / 4;
    for (double amount : {0.25, 0.5, 0.75}) {
        glide.Advance(kStep);
        const double expected = kStartQ * std::pow(kTargetQ / kStartQ, amount);
        EXPECT_NEAR(expected, glide.Q(), expected * 1e-3) << "amount " << amount;
    }
}

TEST(BiquadGolden, ParameterGlideGainFollowsDecibelTrajectory) {
    constexpr double kStartDb = -24.0;
    constexpr double kTargetDb = 24.0;
    constexpr std::uint32_t kTotal = 4800;
    harken::BiquadParameterGlide glide;
    glide.Snap(1000.0f, 1.0f, static_cast<float>(kStartDb));
    glide.Retarget(1000.0f, 1.0f, static_cast<float>(kTargetDb), kTotal);

    // Linear in decibels puts the midpoint at 0 dB. Interpolating in linear
    // amplitude instead would pull it to 20*log10((10^-1.2 + 10^1.2)/2)
    // ~= +18 dB. The quarter points are -12 dB and +12 dB.
    constexpr std::uint32_t kStep = kTotal / 4;
    for (double amount : {0.25, 0.5, 0.75}) {
        glide.Advance(kStep);
        const double expected = kStartDb + (kTargetDb - kStartDb) * amount;
        EXPECT_NEAR(expected, glide.currentGainDb, 1e-3) << "amount " << amount;
    }
}

TEST(BiquadGolden, HighQWideGlideProducesOnlyFiniteStableSections) {
    harken::BiquadTrigTable table;
    harken::BiquadParameterGlide glide;
    glide.Snap(20.0f, 0.1f, -48.0f);
    glide.Retarget(23952.0f, 100.0f, 48.0f, 4096);
    harken::BiquadState states[4];
    for (std::uint32_t progressed = 0; progressed < 4096;
         progressed += 16) {
        glide.Advance(16);
        const harken::BiquadTrigPoint trig =
            table.Lookup(48000.0f, glide.FrequencyHz());
        const harken::BiquadCoeffs sections[] = {
            harken::MakeLowpassFromTrig(trig, glide.Q()),
            harken::MakeHighpassFromTrig(trig, glide.Q()),
            harken::MakeLowShelfFromTrig(
                trig, glide.Q(), glide.currentGainDb),
            harken::MakeHighShelfFromTrig(
                trig, glide.Q(), glide.currentGainDb),
        };
        for (std::size_t sectionIndex = 0; sectionIndex < 4;
             ++sectionIndex) {
            const harken::BiquadCoeffs& section = sections[sectionIndex];
            EXPECT_TRUE(harken::IsBiquadStable(section));
            const double discriminant =
                static_cast<double>(section.a1) * section.a1 -
                4.0 * static_cast<double>(section.a2);
            double poleRadius = 0.0;
            if (discriminant < 0.0) {
                poleRadius = std::sqrt(static_cast<double>(section.a2));
            } else {
                const double root = std::sqrt(discriminant);
                poleRadius = std::max(
                    std::fabs((-section.a1 + root) * 0.5),
                    std::fabs((-section.a1 - root) * 0.5));
            }
            EXPECT_LT(poleRadius, 1.0);
            float output = 0.0f;
            for (std::uint32_t i = 0; i < 16; ++i) {
                output = harken::BiquadTick(
                    section, states[sectionIndex],
                    progressed == 0 && i == 0 ? 0.1f : 0.0f);
                EXPECT_TRUE(std::isfinite(output));
                EXPECT_LT(std::fabs(output), 1.0e6f);
            }
        }
    }
}

// The render path never calls MakeLowpass/MakeHighpass/MakeLowShelf/
// MakeHighShelf for a live voice section: its per-control-block tone glides run
// the *FromTrig kernels (VoiceCascade ProcessFixedToneSection). Those kernels
// carry their own coefficient algebra, so a class error such as a flipped b1
// sign would be invisible to the stability sweep above (it keeps the poles
// inside the unit circle) and to steady-state endpoint checks. Each kernel is
// pinned against the independently transcribed cookbook coefficients — fed an
// exact (sin, cos) point so only the kernel algebra and float storage differ —
// and against the analytic magnitude the section is contracted to produce.
TEST(BiquadGolden, LowpassFromTrigMatchesIndependentCookbook) {
    const double cutoff = 1200.0, q = 0.70710678;
    const BiquadCoeffs c =
        harken::MakeLowpassFromTrig(ExactTrig(kRate, cutoff),
                                   static_cast<float>(q));
    ExpectCoeffsNear(RefLowpass(kRate, cutoff, q), c, 1e-6);
    // Unity DC gain; deep stopband well below the analytic passband edge.
    EXPECT_NEAR(1.0, RefMagnitude(AsRef(c), 2.0 * kPi * 10.0 / kRate), 1e-3);
    EXPECT_LT(RefMagnitude(AsRef(c), 2.0 * kPi * 12000.0 / kRate), 0.1);
}

TEST(BiquadGolden, HighpassFromTrigMatchesIndependentCookbook) {
    const double cutoff = 500.0, q = 0.70710678;
    const BiquadCoeffs c =
        harken::MakeHighpassFromTrig(ExactTrig(kRate, cutoff),
                                    static_cast<float>(q));
    ExpectCoeffsNear(RefHighpass(kRate, cutoff, q), c, 1e-6);
    // Unity gain at Nyquist; deep stopband well below the passband edge.
    EXPECT_NEAR(1.0, RefMagnitude(AsRef(c), kPi), 1e-3);
    EXPECT_LT(RefMagnitude(AsRef(c), 2.0 * kPi * 20.0 / kRate), 0.1);
}

TEST(BiquadGolden, LowShelfFromTrigMatchesIndependentCookbook) {
    const double cutoff = 2000.0, q = 0.70710678, dB = 6.0;
    const BiquadCoeffs c = harken::MakeLowShelfFromTrig(
        ExactTrig(kRate, cutoff), static_cast<float>(q),
        static_cast<float>(dB));
    ExpectCoeffsNear(RefLowShelf(kRate, cutoff, q, dB), c, 1e-6);
    // Shelf-band gain is 10^(dB/20) well below cutoff, unity well above.
    const double shelfGain = std::pow(10.0, dB / 20.0);
    EXPECT_NEAR(shelfGain, RefMagnitude(AsRef(c), 2.0 * kPi * 50.0 / kRate),
                shelfGain * 5e-3);
    EXPECT_NEAR(1.0, RefMagnitude(AsRef(c), 2.0 * kPi * 20000.0 / kRate),
                5e-3);
}

TEST(BiquadGolden, HighShelfFromTrigMatchesIndependentCookbook) {
    const double cutoff = 2000.0, q = 0.70710678, dB = 6.0;
    const BiquadCoeffs c = harken::MakeHighShelfFromTrig(
        ExactTrig(kRate, cutoff), static_cast<float>(q),
        static_cast<float>(dB));
    ExpectCoeffsNear(RefHighShelf(kRate, cutoff, q, dB), c, 1e-6);
    // Shelf-band gain is 10^(dB/20) well above cutoff, unity well below.
    const double shelfGain = std::pow(10.0, dB / 20.0);
    EXPECT_NEAR(shelfGain,
                RefMagnitude(AsRef(c), 2.0 * kPi * 20000.0 / kRate),
                shelfGain * 5e-3);
    EXPECT_NEAR(1.0, RefMagnitude(AsRef(c), 2.0 * kPi * 50.0 / kRate), 5e-3);
}

} // namespace
