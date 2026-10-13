// Golden case: runtime pitch ratio update continuity. A voice playing a
// sine gets its ratio raised mid-stream; the resampler must keep its read
// position, ring contents, and kernel history — no reset, no FIFO flush —
// so the rendered stream stays continuous through the transition while
// the output frequency changes.
//
// Evidence is two-sided:
//   - Continuity: every adjacent-sample step across the whole capture
//     (including the update boundary) stays below the maximum slope of
//     the post-update sine. A reset or flush would produce a jump of up
//     to the full amplitude, orders of magnitude above the bound.
//   - Effect: zero-crossing counts before and after the update match the
//     source frequency times the ratio (the pitch change is real, so the
//     continuity assertion is not vacuously passing on an unchanged
//     stream).
//
// Bound: a sine of amplitude A at frequency f sampled at Fs moves at most
// A * 2*pi*f / Fs per sample — 0.5 * 2*pi*660 / 48000 = 0.0432 here;
// 0.06 adds margin for interpolation overshoot. Zero-crossing windows of
// 512 samples give 2*f*512/48000 crossings, +/-1 for phase.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr double kRate = 48000.0;
constexpr double kSourceHz = 440.0;
constexpr float kAmplitude = 0.5f;

std::uint32_t CountZeroCrossings(const std::vector<float>& s,
                                 std::size_t begin, std::size_t end) {
    std::uint32_t crossings = 0;
    for (std::size_t i = begin + 1; i < end; ++i) {
        const bool was = s[i - 1] >= 0.0f;
        const bool now = s[i] >= 0.0f;
        if (was != now) {
            ++crossings;
        }
    }
    return crossings;
}

float MaxFirstDifference(const std::vector<float>& samples, std::size_t begin,
                         std::size_t end) {
    float maximum = 0.0f;
    for (std::size_t i = std::max<std::size_t>(begin, 1); i < end; ++i) {
        maximum = std::max(maximum,
                           std::fabs(samples[i] - samples[i - 1]));
    }
    return maximum;
}

float MaxSecondDifference(const std::vector<float>& samples,
                          std::size_t begin, std::size_t end) {
    float maximum = 0.0f;
    for (std::size_t i = std::max<std::size_t>(begin, 2); i < end; ++i) {
        maximum = std::max(
            maximum,
            std::fabs(samples[i] - 2.0f * samples[i - 1] + samples[i - 2]));
    }
    return maximum;
}

double WindowBandEnergy(const std::vector<float>& samples, std::size_t center,
                        std::size_t windowSize, double sampleRate,
                        double lowHz, double highHz) {
    constexpr double kPi = 3.14159265358979323846;
    const std::size_t begin = center - windowSize / 2;
    std::vector<double> window(windowSize);
    for (std::size_t n = 0; n < windowSize; ++n) {
        window[n] =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                 static_cast<double>(windowSize - 1));
    }

    double energy = 0.0;
    for (std::size_t bin = 0; bin <= windowSize / 2; ++bin) {
        const double hz = static_cast<double>(bin) * sampleRate /
                          static_cast<double>(windowSize);
        if (hz < lowHz || hz >= highHz) {
            continue;
        }
        double real = 0.0;
        double imag = 0.0;
        for (std::size_t n = 0; n < windowSize; ++n) {
            const double angle =
                -2.0 * kPi * static_cast<double>(bin * n) /
                static_cast<double>(windowSize);
            const double value =
                static_cast<double>(samples[begin + n]) * window[n];
            real += value * std::cos(angle);
            imag += value * std::sin(angle);
        }
        energy += real * real + imag * imag;
    }
    return energy;
}

void CapturePitchedBindingRamp(const std::vector<float>& sine, float oldRatio,
                               float newRatio, std::uint32_t rampMs,
                               std::uint32_t preBlocks, std::uint32_t postBlocks,
                               Capture* out) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::Engine* engine = session.Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    engine->voices.ApplySetPitch(render, oldRatio, 0);
    harkentest::PumpAndCapture(engine, preBlocks, *out);
    engine->voices.ApplySetPitch(render, newRatio, rampMs);
    harkentest::PumpAndCapture(engine, postBlocks, *out);
}

TEST(PitchContinuityGolden, RatioUpdateIsClickFreeAndChangesFrequency) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(48000, kSourceHz, kRate, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 2, out);
    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(handle, 1.5f));
    harkentest::PumpAndCapture(session.Engine(), 4, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    // Continuity: the bound holds across all six blocks, most
    // importantly across the update boundary at sample 512.
    const float stepBound =
        0.06f; // max slope of the 660 Hz result plus margin
    for (std::size_t i = 1; i < out.left.size(); ++i) {
        ASSERT_LE(std::fabs(out.left[i] - out.left[i - 1]), stepBound)
            << "discontinuity at sample " << i;
    }

    // Effect: 440 Hz before the update, 660 Hz after. 512-sample windows
    // expect 9.4 and 14.1 crossings respectively; +/-1 for phase.
    const std::uint32_t before = CountZeroCrossings(out.left, 0, 512);
    const std::uint32_t after = CountZeroCrossings(out.left, 1024, 1536);
    EXPECT_GE(before, 8u);
    EXPECT_LE(before, 11u);
    EXPECT_GE(after, 13u);
    EXPECT_LE(after, 16u);
    // The two windows must actually differ — the ratio change happened.
    EXPECT_GT(after, before);
}

TEST(PitchContinuityGolden, LargeRatioUpdateHasNoBroadbandTransient) {
    constexpr std::size_t kEventSample = 8 * kBlockFrames;
    constexpr std::size_t kSteadyWindow = 4 * kBlockFrames;
    constexpr std::size_t kSpectrumWindow = 512;
    constexpr std::uint32_t kBindingRampMs = 50;
    constexpr std::uint32_t kPostBlocks = 16;
    constexpr std::size_t kAfterBegin = kEventSample + 12u * kBlockFrames;
    constexpr float kSlopeMargin = 1.01f;
    constexpr float kCurvatureMargin = 2.0f;
    constexpr double kBroadbandMargin = 100.0;
    const std::size_t preWindowBegin = kEventSample - kSteadyWindow;
    const std::vector<float> sine =
        harkentest::MakeSine(48000, 400.0, kRate, kAmplitude);

    Capture switched;
    CapturePitchedBindingRamp(sine, 2.0f, 0.75f, kBindingRampMs, 8,
                              kPostBlocks, &switched);
    Capture control;
    CapturePitchedBindingRamp(sine, 2.0f, 2.0f, kBindingRampMs, 8,
                              kPostBlocks, &control);
    HARKENTEST_ASSERT_NON_SILENT(switched.left);
    HARKENTEST_ASSERT_NON_SILENT(control.left);
    ASSERT_EQ((8u + kPostBlocks) * kBlockFrames, switched.left.size());
    ASSERT_EQ(switched.left.size(), control.left.size());
    for (float sample : switched.left) {
        ASSERT_TRUE(std::isfinite(sample));
    }

    const float preD1Max = MaxFirstDifference(
        switched.left, preWindowBegin, kEventSample);
    const float eventJump = MaxFirstDifference(
        switched.left, kEventSample, kEventSample + 1);
    EXPECT_LE(eventJump, preD1Max * kSlopeMargin);

    const float preD2Max = MaxSecondDifference(
        switched.left, preWindowBegin, kEventSample);
    const float eventD2 = MaxSecondDifference(
        switched.left, kEventSample + 1, kEventSample + 2);
    EXPECT_LE(eventD2, preD2Max * kCurvatureMargin)
        << "pitch update introduced a slope discontinuity";

    const double switchedHighBand =
        WindowBandEnergy(switched.left, kEventSample, kSpectrumWindow, kRate,
                         4000.0, 20000.0);
    const double controlHighBand =
        WindowBandEnergy(control.left, kEventSample, kSpectrumWindow, kRate,
                         4000.0, 20000.0);
    ASSERT_GT(controlHighBand, 0.0);
    EXPECT_LE(switchedHighBand, controlHighBand * kBroadbandMargin)
        << "pitch update introduced broadband transient energy";

    const float controlPreD2Max = MaxSecondDifference(
        control.left, preWindowBegin, kEventSample);
    const float controlEventD2 = MaxSecondDifference(
        control.left, kEventSample + 1, kEventSample + 2);
    EXPECT_LE(controlEventD2, controlPreD2Max * kCurvatureMargin);

    const std::uint32_t before = CountZeroCrossings(
        switched.left, preWindowBegin, kEventSample);
    const std::uint32_t after = CountZeroCrossings(
        switched.left, kAfterBegin, kAfterBegin + kSteadyWindow);
    EXPECT_GE(before, 32u);
    EXPECT_LE(before, 36u);
    EXPECT_GE(after, 11u);
    EXPECT_LE(after, 15u);
}

} // namespace
