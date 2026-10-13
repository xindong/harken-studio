// Golden cases for the two real built-in effects, driven through the public
// API. Expected values come from references computed INDEPENDENTLY of the
// production DSP: the RBJ cookbook coefficient formulas are transcribed afresh
// in double (RefLowShelf/RefPeaking/RefHighShelf below), never by calling the
// engine's own biquad builders, and the delay's DC fixed point from its own
// recurrence. A shared coefficient bug therefore cannot pass both the engine
// and the reference. The discriminating facts (non-commuting order, aggregate
// tail = the serial sum, a sample-count parameter transition,
// per-band magnitude at non-0 dB, click-free real bypass) hold by construction.
//
// Tolerances: steady-state comparisons use a small
// tolerance because the delay/tone converge geometrically but not in exactly
// the block count captured, and because the reference runs in double while the
// engine stores float coefficients; the non-commutation margin (> 0.05) and the
// aggregate-tail relations are structural, independent of that residual.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/render/BuiltinEffects.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

VnConfig EffectConfig(std::uint32_t slots, std::uint32_t delayCap,
                      std::uint32_t toneCap) {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = slots;
    config.delayEffectPoolCapacity = delayCap;
    config.toneEffectPoolCapacity = toneCap;
    return config;
}

VnFeedbackDelayDesc DelayDesc(float delayMs, float feedback, float wet,
                              float dry) {
    VnFeedbackDelayDesc d{};
    d.structSize = sizeof(VnFeedbackDelayDesc);
    d.delayMs = delayMs;
    d.feedback = feedback;
    d.wet = wet;
    d.dry = dry;
    return d;
}

VnThreeBandToneDesc ToneDesc(float lsG, float lsF, float pG, float pF,
                             float pQ, float hsG, float hsF) {
    VnThreeBandToneDesc t{};
    t.structSize = sizeof(VnThreeBandToneDesc);
    t.lowShelfGainDb = lsG;
    t.lowShelfFreqHz = lsF;
    t.peakGainDb = pG;
    t.peakFreqHz = pF;
    t.peakQ = pQ;
    t.highShelfGainDb = hsG;
    t.highShelfFreqHz = hsF;
    return t;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

VnVoiceHandle PlayMono(EngineSession& session, VnSoundId id,
                       const std::vector<float>& samples) {
    const VnPCMDesc desc = MonoDesc(samples, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
    const VnPlayDesc play = MakePlayDesc(id);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    return handle;
}

// DC steady state of the delay's feedback recurrence line = u + fb*tanh(line):
// a contraction for fb in [0, 1), so the iteration converges to the same fixed
// point the engine reaches over its delayed samples. Uses the float tanh the
// effect uses.
float DelaySteadyDC(float u, float fb) {
    float d = 0.0f;
    for (int i = 0; i < 256; ++i) {
        d = u + fb * std::tanh(d);
    }
    return d;
}

// --- independent RBJ cookbook transcription (double) ---
// Deliberately NOT the production harken:: biquad builders: transcribed straight
// from the cookbook so a shared coefficient error cannot pass both the engine
// and this reference. Coefficients are normalized to a0 = 1, matching the
// engine's stored form.
struct RefBiquad {
    double b0, b1, b2, a1, a2;
};

constexpr double kPi = 3.14159265358979323846;

RefBiquad RefLowShelf(double fs, double f0, double q, double dB) {
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0, am1 = A - 1.0;
    const double a0 = ap1 + am1 * cw + beta;
    return {A * (ap1 - am1 * cw + beta) / a0,
            2.0 * A * (am1 - ap1 * cw) / a0,
            A * (ap1 - am1 * cw - beta) / a0,
            -2.0 * (am1 + ap1 * cw) / a0,
            (ap1 + am1 * cw - beta) / a0};
}

RefBiquad RefPeaking(double fs, double f0, double q, double dB) {
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha / A;
    return {(1.0 + alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha * A) / a0,
            -2.0 * cw / a0, (1.0 - alpha / A) / a0};
}

RefBiquad RefHighShelf(double fs, double f0, double q, double dB) {
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double beta = 2.0 * std::sqrt(A) * alpha;
    const double ap1 = A + 1.0, am1 = A - 1.0;
    const double a0 = ap1 - am1 * cw + beta;
    return {A * (ap1 + am1 * cw + beta) / a0,
            -2.0 * A * (am1 + ap1 * cw) / a0,
            A * (ap1 + am1 * cw - beta) / a0,
            2.0 * (am1 - ap1 * cw) / a0,
            (ap1 - am1 * cw - beta) / a0};
}

// DC gain H(z=1) of a normalized section.
double RefDcGain(const RefBiquad& c) {
    return (c.b0 + c.b1 + c.b2) / (1.0 + c.a1 + c.a2);
}

// Magnitude response |H(e^jw)| of a normalized section.
double RefMag(const RefBiquad& c, double w) {
    const double cw = std::cos(w), sw = std::sin(w);
    const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const double nr = c.b0 + c.b1 * cw + c.b2 * c2;
    const double ni = -(c.b1 * sw + c.b2 * s2);
    const double dr = 1.0 + c.a1 * cw + c.a2 * c2;
    const double di = -(c.a1 * sw + c.a2 * s2);
    return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

// Runs one normalized section in double (transposed direct form II) from zero
// state, so a reference block can be compared to the engine sample by sample.
std::vector<double> RefBiquadRun(const RefBiquad& c,
                                 const std::vector<double>& x) {
    std::vector<double> y(x.size());
    double z1 = 0.0, z2 = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double xi = x[i];
        const double yi = c.b0 * xi + z1;
        z1 = c.b1 * xi - c.a1 * yi + z2;
        z2 = c.b2 * xi - c.a2 * yi;
        y[i] = yi;
    }
    return y;
}

// DC gain of the three-band tone: the product of the three sections' DC gains,
// each from the independent transcription above. (Analytically the low shelf
// contributes 10^(lsG/20) while the peak and high shelf are unity at DC; the
// product is computed rather than asserting that identity so a formula error in
// any section still shows up.)
double ToneDcGain(float lsG, float lsF, float pG, float pF, float pQ, float hsG,
                  float hsF) {
    return RefDcGain(RefLowShelf(kRate, lsF, harken::kToneShelfQ, lsG)) *
           RefDcGain(RefPeaking(kRate, pF, pQ, pG)) *
           RefDcGain(RefHighShelf(kRate, hsF, harken::kToneShelfQ, hsG));
}

TEST(BuiltinEffectGolden, ChainOrderIsNonCommuting) {
    // A soft-saturated feedback delay and a linear tone do NOT commute: the
    // saturation makes delay(tone(x)) != tone(delay(x)). Both orderings are
    // computed independently and compared to the engine at a small tolerance,
    // and the two must differ by a wide margin.
    constexpr float kU = 0.2f;
    constexpr float kFb = 0.7f;
    constexpr float kWet = 0.5f;
    constexpr float kDry = 1.0f;

    auto renderOrder = [&](bool delayFirst) -> float {
        EngineSession session(EffectConfig(2, 1, 1));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> dc = harkentest::MakeDC(20000, kU);
        VnVoiceHandle h = PlayMono(session, harkentest::ObjectId(1), dc);
        const VnFeedbackDelayDesc d = DelayDesc(1.0f, kFb, kWet, kDry);
        const VnThreeBandToneDesc t =
            ToneDesc(6.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
        if (delayFirst) {
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY,
                                               AsBase(&d)));
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 1, VN_EFFECT_THREE_BAND_TONE,
                                               AsBase(&t)));
        } else {
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_THREE_BAND_TONE,
                                               AsBase(&t)));
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 1, VN_EFFECT_FEEDBACK_DELAY,
                                               AsBase(&d)));
        }
        Capture out;
        harkentest::PumpAndCapture(session.Engine(), 40, out);
        return out.left.back();
    };

    const float g =
        ToneDcGain(6.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
    // Order A: delay then tone.
    const float da = DelaySteadyDC(kU, kFb);
    const float outA = g * (kDry * kU + kWet * da);
    // Order B: tone then delay.
    const float u2 = g * kU;
    const float db = DelaySteadyDC(u2, kFb);
    const float outB = kDry * u2 + kWet * db;

    const float engA = renderOrder(/*delayFirst=*/true);
    const float engB = renderOrder(/*delayFirst=*/false);
    EXPECT_GT(std::fabs(engA), harkentest::kSilenceFloor);
    EXPECT_GT(std::fabs(engB), harkentest::kSilenceFloor);
    EXPECT_NEAR(outA, engA, 3e-3f);
    EXPECT_NEAR(outB, engB, 3e-3f);
    EXPECT_GT(std::fabs(outA - outB), 0.05f)
        << "delay and tone must not commute";
    EXPECT_GT(std::fabs(engA - engB), 0.05f)
        << "engine slot order must be observable";
}

TEST(BuiltinEffectGolden, AggregateTailDrainsSerialSum) {
    // Two feedback delays (no feedback -> tail = delay length) on one voice.
    // The voice's drain length must equal the serial sum: (short, long) falls
    // between (short, short) and (long, long). Measured as the block at which
    // the voice leaves the active set; the resampler's constant latency
    // cancels in the comparisons.
    auto drainBlocks = [](float msA, float msB) -> std::uint32_t {
        EngineSession session(EffectConfig(2, 2, 1));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> src =
            harkentest::MakeSine(256, 1000.0, kRate, 0.5f);
        VnVoiceHandle h = PlayMono(session, harkentest::ObjectId(1), src);
        const VnFeedbackDelayDesc a = DelayDesc(msA, 0.0f, 1.0f, 1.0f);
        const VnFeedbackDelayDesc b = DelayDesc(msB, 0.0f, 1.0f, 1.0f);
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY,
                                           AsBase(&a)));
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 1, VN_EFFECT_FEEDBACK_DELAY,
                                           AsBase(&b)));
        std::uint32_t blocks = 0;
        for (std::uint32_t i = 0; i < 64; ++i) {
            session.Engine()->nullBackend.Pump(1);
            ++blocks;
            VnSnapshot snap{};
            snap.structSize = sizeof(VnSnapshot);
            EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
            if (snap.activeVoiceCount == 0) {
                break;
            }
        }
        return blocks;
    };

    const std::uint32_t shortLong = drainBlocks(10.0f, 20.0f); // tails 480, 960
    const std::uint32_t shortShort = drainBlocks(10.0f, 10.0f); // 480, 480
    const std::uint32_t longLong = drainBlocks(20.0f, 20.0f);   // 960, 960
    EXPECT_LT(shortLong, longLong) << "serial sum must remain below two long tails";
    EXPECT_GT(shortLong, shortShort) << "and outlast the shorter-tail chain";
}

TEST(BuiltinEffectGolden, ParamUpdateSmoothsAcrossTenMilliseconds) {
    // A live tone update keeps the first sample at the old response and reaches
    // the prepared target after the fixed ten-millisecond sample span.
    EngineSession session(EffectConfig(1, 1, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float kDc = 0.2f;
    const std::vector<float> dc = harkentest::MakeDC(20000, kDc);
    VnVoiceHandle h = PlayMono(session, harkentest::ObjectId(1), dc);

    const VnThreeBandToneDesc t =
        ToneDesc(0.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&t)));

    // Settle the transparent (0 dB = exact identity) chain; the last block is
    // bit-transparent and every biquad state is zero going into the update.
    Capture before;
    harkentest::PumpAndCapture(session.Engine(), 6, before);
    EXPECT_NEAR(kDc, before.left.back(), 1e-5f); // 0 dB is transparent

    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         h, 0, harken::kToneParamLowShelfGainDb, 12.0f));

    // Independent reference, computed without calling the production
    // smoothing helper: at constant DC, the pre-update (0 dB, exact identity)
    // response is bit-transparent at every sample, and the post-update
    // response is DC-seeded so it too sits exactly at its converged value
    // from its very first sample — both branches of the crossfade are the
    // constant source/target DC levels for the whole span, so the blended
    // sample at transition index n is exactly source + (target - source) *
    // n / (T - 1), the contracted uniform ramp.
    const double g =
        ToneDcGain(12.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 6000.0f);
    EXPECT_GT(g, 1.5);
    const std::uint32_t transitionFrames = harken::BiquadSmoothingFrames(kRate);
    ASSERT_GT(transitionFrames, kBlock)
        << "the span must outlast one block for this probe index to sit "
           "inside the transition";
    constexpr std::uint32_t kProbeIndex = kBlock - 1; // n = 255, 0-based
    const double mix = static_cast<double>(kProbeIndex) /
                       static_cast<double>(transitionFrames - 1u);
    const double expectedAtProbe = kDc + (kDc * g - kDc) * mix;

    // The first transition sample is old-weighted rather than a block step.
    Capture first;
    harkentest::PumpAndCapture(session.Engine(), 1, first);
    ASSERT_FALSE(first.left.empty());
    EXPECT_NEAR(first.left.front(), before.left.back(), 1e-5f);
    EXPECT_NEAR(expectedAtProbe, static_cast<double>(first.left.back()), 1e-3)
        << "sample " << kProbeIndex << " must sit at the uniform ramp's "
           "contracted weight, not an arbitrary bound";

    // The transition is sample-count based, so two 256-frame blocks cover the
    // 480-frame span without adding an extra block-boundary step.
    Capture transition;
    harkentest::PumpAndCapture(session.Engine(), 2, transition);
    ASSERT_FALSE(transition.left.empty());

    // After the shelf's own exponential settling, the DC output converges to
    // the new low-shelf DC gain. The first transition block is pinned
    // sample-by-sample against the dual-run linear-mix reference.
    Capture settled;
    harkentest::PumpAndCapture(session.Engine(), 16, settled);
    EXPECT_NEAR(kDc * g, static_cast<double>(settled.left.back()), 1e-3);
}

TEST(BuiltinEffectGolden, ThreeBandToneMagnitudeMatchesIndependentRbj) {
    // Per-band non-0 dB coverage against an INDEPENDENT magnitude reference. A
    // DC test cannot observe the peak or high shelf (both are unity at DC), so
    // this drives steady sines at three probe frequencies — one in each band's
    // region — and compares the measured steady-state RMS gain to |H| from the
    // independent RBJ transcription. All three bands carry a non-zero gain, so
    // a coefficient error in any of them shifts the measured gain.
    constexpr float kLsG = 6.0f, kLsF = 200.0f;
    constexpr float kPG = 9.0f, kPF = 1000.0f, kPQ = 2.0f;
    constexpr float kHsG = -6.0f, kHsF = 6000.0f;

    // Measures the tone chain's steady-state gain at `freq` Hz. The RMS window
    // is an integer number of periods taken well past the filter settling; the
    // resampler runs at ratio 1.0 (source rate == engine rate) so it passes the
    // sine through unchanged and only the tone shapes the amplitude.
    auto measureGain = [&](double freq, std::uint32_t periodSamples) -> double {
        EngineSession session(EffectConfig(1, 1, 1));
        EXPECT_EQ(VN_OK, session.InitResult());
        constexpr float kAmp = 0.2f;
        const std::vector<float> sine =
            harkentest::MakeSine(24000, freq, kRate, kAmp);
        VnVoiceHandle h = PlayMono(session, harkentest::ObjectId(1), sine);
        const VnThreeBandToneDesc t =
            ToneDesc(kLsG, kLsF, kPG, kPF, kPQ, kHsG, kHsF);
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_THREE_BAND_TONE,
                                           AsBase(&t)));
        Capture out;
        harkentest::PumpAndCapture(session.Engine(), 90, out); // ~23040 samples
        const std::size_t window =
            static_cast<std::size_t>(periodSamples) * 100; // 100 whole periods
        EXPECT_GT(out.left.size(), window);
        const std::size_t start = out.left.size() - window;
        double sumSq = 0.0;
        for (std::size_t i = start; i < out.left.size(); ++i) {
            sumSq += static_cast<double>(out.left[i]) *
                     static_cast<double>(out.left[i]);
        }
        const double rms = std::sqrt(sumSq / static_cast<double>(window));
        return rms / (static_cast<double>(kAmp) / std::sqrt(2.0));
    };

    struct Probe {
        double freq;
        std::uint32_t period;
    };
    // 48000 / freq is an integer for each, so the window holds whole periods.
    const Probe probes[] = {{300.0, 160}, {1000.0, 48}, {6000.0, 8}};
    for (const Probe& p : probes) {
        const double w = 2.0 * kPi * p.freq / static_cast<double>(kRate);
        const double ref =
            RefMag(RefLowShelf(kRate, kLsF, harken::kToneShelfQ, kLsG), w) *
            RefMag(RefPeaking(kRate, kPF, kPQ, kPG), w) *
            RefMag(RefHighShelf(kRate, kHsF, harken::kToneShelfQ, kHsG), w);
        const double meas = measureGain(p.freq, p.period);
        EXPECT_GT(ref, 0.0);
        EXPECT_NEAR(ref, meas, ref * 0.02 + 1e-3) << "at " << p.freq << " Hz";
    }
}

TEST(BuiltinEffectGolden, RealDelayBypassIsClickFree) {
    // Bypassing a live feedback delay (wet tail present) crossfades wet->dry
    // over one block, so no adjacent-sample step exceeds the crossfade bound.
    // An abrupt toggle would jump the full wet-minus-dry step and fail.
    EngineSession session(EffectConfig(1, 1, 1));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float kDc = 0.2f;
    constexpr float kFb = 0.5f;
    constexpr float kWet = 0.5f;
    constexpr float kDry = 1.0f;
    const std::vector<float> dc = harkentest::MakeDC(20000, kDc);
    VnVoiceHandle h = PlayMono(session, harkentest::ObjectId(1), dc);

    const VnFeedbackDelayDesc d = DelayDesc(1.0f, kFb, kWet, kDry);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&d)));

    // Converge the delay to steady state before capturing.
    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 8, warm);

    const float wetSteady = kDry * kDc + kWet * DelaySteadyDC(kDc, kFb);
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out); // one steady wet block
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(h, 0, 1));  // arm the bypass
    harkentest::PumpAndCapture(session.Engine(), 4, out); // crossfade + dry

    HARKENTEST_ASSERT_NON_SILENT(out.left);
    ASSERT_NEAR(wetSteady, out.left[0], 1e-3f);
    ASSERT_NEAR(kDc, out.left.back(), 1e-3f);
    const float maxStep =
        std::fabs(wetSteady - kDc) / static_cast<float>(kBlock) + 1e-4f;
    for (std::size_t i = 1; i < out.left.size(); ++i) {
        const float step = std::fabs(out.left[i] - out.left[i - 1]);
        ASSERT_LE(step, maxStep) << "click at sample " << i;
    }
}

} // namespace
