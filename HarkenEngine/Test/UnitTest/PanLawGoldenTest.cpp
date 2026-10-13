// Golden case: the equal-power pan law at the bus fold-in, proven on the
// output buffers with closed-form expectations.
//
// A spatialized mono DC voice with no attenuation profile pans purely by
// azimuth. With the default listener (origin, forward (0,0,-1), up
// (0,1,0)) the right axis is +x, so an emitter direction whose normalized
// x component is p resolves pan = p. The fold-in applies
//   theta = (pan + 1) * pi/4,  gL = sqrt(2)*cos(theta),  gR = sqrt(2)*sin(theta)
// with width 0 (unbound voices collapse to a point source), so the
// captured channels must equal amplitude * gL / gR:
//   full left  (pan -1): L = a*sqrt(2),  R = 0
//   full right (pan +1): L = 0,          R = a*sqrt(2)
//   front      (pan  0): L = R = a
//   half right (pan .5): L = a*sqrt(2)*cos(3pi/8), R = a*sqrt(2)*sin(3pi/8)
// Tolerance 1e-6: the path is DC through a unity cascade (exact) times
// one float multiply per channel.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.25f;
const float kPannedAmp = kAmp * std::sqrt(2.0f);
constexpr double kQuarterPi = 3.14159265358979323846 / 4.0;
constexpr float kTol = 1e-6f;

constexpr std::uint32_t kMotionSampleRate = 96000;
constexpr std::uint32_t kControlTickFrames = 960;
constexpr std::uint32_t kHoldBlocks = kMotionSampleRate / kBlockFrames;
constexpr std::uint32_t kMotionBlocks =
    2 * kMotionSampleRate / kBlockFrames;
constexpr std::uint32_t kMotionControlSteps = 200;
constexpr std::size_t kMotionEventSample =
    kMotionSampleRate + kMotionSampleRate;
constexpr std::size_t kSpectrumWindow = 512;
// A 10 ms transition spreads either midpoint control step below
// to less than 1e-6 per frame. The wider bound leaves float and downstream
// arithmetic headroom while still excluding a block-boundary replacement.
constexpr float kMaxEventDifference = 5.0e-5f;
constexpr double kBroadbandMargin = 64.0;

enum class MotionControl {
    FixedPosition,
    Combined,
    PanOnly,
    AttenuationOnly,
    ListenerAttenuationOnly,
    WidthOnly,
};

struct MotionCapture {
    Capture audio;
    std::uint64_t backpressureCount = 0;
    std::uint64_t publicationDeferredCount = 0;
};

float SmoothStep(float value) {
    return value * value * (3.0f - 2.0f * value);
}

float MaxFirstDifference(const std::vector<float>& samples,
                         std::size_t begin, std::size_t end) {
    float maximum = 0.0f;
    for (std::size_t i = std::max<std::size_t>(begin, 1); i < end; ++i) {
        maximum =
            std::max(maximum, std::fabs(samples[i] - samples[i - 1]));
    }
    return maximum;
}

float MaxSecondDifference(const std::vector<float>& samples,
                          std::size_t begin, std::size_t end) {
    float maximum = 0.0f;
    for (std::size_t i = std::max<std::size_t>(begin, 2); i < end; ++i) {
        maximum = std::max(
            maximum,
            std::fabs(samples[i] - 2.0f * samples[i - 1] +
                      samples[i - 2]));
    }
    return maximum;
}

double WindowBandEnergy(const std::vector<float>& samples,
                        std::size_t center, std::size_t windowSize,
                        double sampleRate, double lowHz, double highHz) {
    constexpr double kPi = 3.14159265358979323846;
    const std::size_t begin = center - windowSize / 2;
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
            const double window =
                0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                     static_cast<double>(windowSize - 1));
            const double angle =
                -2.0 * kPi * static_cast<double>(bin * n) /
                static_cast<double>(windowSize);
            const double value =
                static_cast<double>(samples[begin + n]) * window;
            real += value * std::cos(angle);
            imag += value * std::sin(angle);
        }
        energy += real * real + imag * imag;
    }
    return energy;
}

float StereoFirstDifference(const Capture& capture, std::size_t begin,
                            std::size_t end) {
    return std::max(MaxFirstDifference(capture.left, begin, end),
                    MaxFirstDifference(capture.right, begin, end));
}

float StereoSecondDifference(const Capture& capture, std::size_t begin,
                             std::size_t end) {
    return std::max(MaxSecondDifference(capture.left, begin, end),
                    MaxSecondDifference(capture.right, begin, end));
}

double StereoBandEnergy(const Capture& capture, std::size_t center) {
    return WindowBandEnergy(capture.left, center, kSpectrumWindow,
                            kMotionSampleRate, 4000.0,
                            0.5 * kMotionSampleRate) +
           WindowBandEnergy(capture.right, center, kSpectrumWindow,
                            kMotionSampleRate, 4000.0,
                            0.5 * kMotionSampleRate);
}

VnVec3 MotionPosition(MotionControl control, std::uint32_t step) {
    if (control == MotionControl::FixedPosition) {
        return harkentest::Vec3(0.0f, 0.0f, -2.0f);
    }
    const float progress = static_cast<float>(step) /
                           static_cast<float>(kMotionControlSteps - 1);
    const float amount = SmoothStep(progress);
    const float x = 8.0f * amount;
    const float z = -2.0f - 3.0f * amount;
    if (control != MotionControl::AttenuationOnly) {
        if (control == MotionControl::WidthOnly) {
            const float distance = std::sqrt(x * x + z * z);
            return harkentest::Vec3(distance, 0.0f, 0.0f);
        }
        return harkentest::Vec3(x, 0.0f, z);
    }
    // Preserve the route's exact distance while holding azimuth at center.
    // Width changes are acoustically neutral when both pan gains are equal.
    const float distance = std::sqrt(x * x + z * z);
    return harkentest::Vec3(0.0f, 0.0f, -distance);
}

VnVec3 ListenerMotionPosition(std::uint32_t step) {
    const VnVec3 emitterPath =
        MotionPosition(MotionControl::AttenuationOnly, step);
    const float distance = -emitterPath.z;
    return harkentest::Vec3(0.0f, 0.0f, distance - 2.0f);
}

void CaptureCenterToRightMotion(MotionControl control,
                                MotionCapture* captured) {
    EngineSession session(
        MakeNullConfig(1, kBlockFrames, kMotionSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc =
        harkentest::MakeDC(3 * kMotionSampleRate, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, kMotionSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK,
              Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, 0.0f),
                             harkentest::Vec3(0.0f, 0.0f, -1.0f),
                             harkentest::Vec3(0.0f, 1.0f, 0.0f)));

    const bool hasAttenuation =
        control == MotionControl::Combined ||
        control == MotionControl::AttenuationOnly ||
        control == MotionControl::ListenerAttenuationOnly ||
        control == MotionControl::WidthOnly;
    if (hasAttenuation) {
        VnAttenuationDesc attenuation{};
        attenuation.structSize = sizeof(VnAttenuationDesc);
        attenuation.rolloff = VN_ROLLOFF_LINEAR;
        attenuation.minDistance = 1.0f;
        attenuation.maxDistance = 14.0f;
        attenuation.airAbsorptionNearHz = 20000.0f;
        attenuation.airAbsorptionFarHz = 20000.0f;
        if (control == MotionControl::WidthOnly) {
            attenuation.widthCurve.enabled = 1;
            attenuation.widthCurve.pointCount = 2;
            attenuation.widthCurve.points[0] = {1.0f, 1.0f};
            attenuation.widthCurve.points[1] = {14.0f, 0.0f};
            attenuation.widthCurve.handles[0] = harkentest::EndpointHandles(
                attenuation.widthCurve.points[0], attenuation.widthCurve.points[1]);
            attenuation.volumeCurve.enabled = 1;
            attenuation.volumeCurve.pointCount = 2;
            attenuation.volumeCurve.points[0] = {1.0f, 0.0f};
            attenuation.volumeCurve.points[1] = {14.0f, 0.0f};
            attenuation.volumeCurve.handles[0] =
                harkentest::EndpointHandles(
                    attenuation.volumeCurve.points[0],
                    attenuation.volumeCurve.points[1]);
        }
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &attenuation));
    }

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -2.0f, -9.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    if (hasAttenuation) {
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(1)));
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    harkentest::PumpAndCapture(engine, kHoldBlocks, captured->audio);

    std::uint32_t nextControlStep = 0;
    for (std::uint32_t block = 0; block < kMotionBlocks; ++block) {
        const std::uint32_t blockStart = block * kBlockFrames;
        // The 10 ms control clock is not block-aligned. Apply each update at
        // the first 256-frame boundary at or after its 960-frame deadline.
        while (nextControlStep < kMotionControlSteps &&
               nextControlStep * kControlTickFrames <= blockStart) {
            if (control == MotionControl::ListenerAttenuationOnly) {
                ASSERT_EQ(
                    VN_OK,
                    Vn_SetListener(ListenerMotionPosition(nextControlStep),
                                   harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                   harkentest::Vec3(0.0f, 1.0f, 0.0f)));
            } else {
                ASSERT_EQ(
                    VN_OK,
                    Vn_SetVoicePosition(
                        handle, MotionPosition(control, nextControlStep)));
            }
            ASSERT_EQ(VN_OK, Vn_Update());
            ++nextControlStep;
        }
        harkentest::PumpAndCapture(engine, 1, captured->audio);
    }
    ASSERT_EQ(kMotionControlSteps, nextControlStep);
    captured->backpressureCount = engine->commands.BackpressureCount();
    captured->publicationDeferredCount =
        engine->propertyPublisher.DeferredCount();
}

void ExpectNoSpatialClick(const MotionCapture& captured) {
    ASSERT_EQ(kMotionEventSample + kMotionSampleRate,
              captured.audio.left.size());
    ASSERT_EQ(captured.audio.left.size(), captured.audio.right.size());
    HARKENTEST_ASSERT_NON_SILENT(captured.audio.left);
    HARKENTEST_ASSERT_NON_SILENT(captured.audio.right);
    bool allFinite = true;
    float peak = 0.0f;
    for (float sample : captured.audio.left) {
        allFinite = allFinite && std::isfinite(sample);
        peak = std::max(peak, std::fabs(sample));
    }
    for (float sample : captured.audio.right) {
        allFinite = allFinite && std::isfinite(sample);
        peak = std::max(peak, std::fabs(sample));
    }
    ASSERT_TRUE(allFinite);
    EXPECT_LE(peak, 1.0f);
    EXPECT_EQ(0u, captured.backpressureCount);
    EXPECT_EQ(0u, captured.publicationDeferredCount);

    const float eventD1 = StereoFirstDifference(
        captured.audio, kMotionEventSample, kMotionEventSample + 1);
    const float eventD2 = StereoSecondDifference(
        captured.audio, kMotionEventSample, kMotionEventSample + 2);
    EXPECT_LE(eventD1, kMaxEventDifference)
        << "spatial update introduced a sample discontinuity";
    EXPECT_LE(eventD2, kMaxEventDifference)
        << "spatial update introduced a slope discontinuity";

    const double eventHighBand =
        StereoBandEnergy(captured.audio, kMotionEventSample);
    const double steadyHighBand =
        StereoBandEnergy(captured.audio, kMotionEventSample - 384);
    ASSERT_GT(steadyHighBand, 0.0);
    EXPECT_LE(eventHighBand, steadyHighBand * kBroadbandMargin)
        << "spatial update introduced broadband transient energy";
}

// Renders one spatialized DC voice at `position` and returns the second
// captured block's steady per-channel values.
void CapturePanned(float posX, float posY, float posZ, float* outL,
                   float* outR, float spatializationMix = 1.0f,
                   float width = 0.0f) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), posX, posY, posZ);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    if (spatializationMix != 1.0f) {
        ASSERT_EQ(VN_OK,
                  Vn_SetVoiceSpatializationMix(handle, spatializationMix));
    }
    if (width != 0.0f) {
        engine->params.SetAttenuationSlot(
            harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceWidth, width);
    }
    // The tick computes pan from the mirrors and commits it; the play and
    // pan commands both apply at the next block boundary.
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture captured;
    harkentest::PumpAndCapture(engine, 2, captured);
    *outL = captured.left[kBlockFrames + kBlockFrames / 2];
    *outR = captured.right[kBlockFrames + kBlockFrames / 2];
}

TEST(PanLawGolden, FullLeftIsLeftOnly) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(-10.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_GT(std::fabs(l), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // theta = 0: gL = sqrt(2), gR = 0.
    EXPECT_NEAR(kPannedAmp, l, kTol);
    EXPECT_NEAR(0.0f, r, kTol);
}

TEST(PanLawGolden, FullRightIsRightOnly) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(10.0f, 0.0f, 0.0f, &l, &r);
    ASSERT_GT(std::fabs(r), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // theta = pi/2: gL = 0, gR = sqrt(2).
    EXPECT_NEAR(0.0f, l, kTol);
    EXPECT_NEAR(kPannedAmp, r, kTol);
}

TEST(PanLawGolden, SpatializationMixZeroUsesEqualPowerTwoDimensionalMonoFold) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(10.0f, 0.0f, 0.0f, &l, &r, 0.0f);
    ASSERT_GT(std::fabs(l), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), l, kTol);
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), r, kTol);
}

TEST(PanLawGolden, SpatializationMixOneKeepsPointSourceEndpoint) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(10.0f, 0.0f, 0.0f, &l, &r, 1.0f);
    ASSERT_GT(std::fabs(r), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(0.0f, l, kTol);
    EXPECT_NEAR(kPannedAmp, r, kTol);
}

TEST(PanLawGolden, SpatializationMixHalfBlendsWithoutRenormalization) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(10.0f, 0.0f, 0.0f, &l, &r, 0.5f);
    ASSERT_GT(std::fabs(l), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // Full-right 3D gains are (0,sqrt(2)); centered 2D is (1,1).
    EXPECT_NEAR(0.5f * kPannedAmp / std::sqrt(2.0f), l, kTol);
    EXPECT_NEAR(0.5f * kPannedAmp * (1.0f + 1.0f / std::sqrt(2.0f)), r, kTol);
}

TEST(PanLawGolden, SpatializationMixAppliesAfterWidthWithoutRenormalization) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(10.0f, 0.0f, 0.0f, &l, &r, 0.5f, 1.0f);
    ASSERT_GT(std::fabs(l), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    const float widened = kPannedAmp / std::sqrt(2.0f);
    EXPECT_NEAR(widened, l, kTol);
    EXPECT_NEAR(widened, r, kTol);
}

TEST(PanLawGolden, FrontIsEqualPowerCenter) {
    float l = 0.0f, r = 0.0f;
    CapturePanned(0.0f, 0.0f, -10.0f, &l, &r);
    ASSERT_GT(std::fabs(l), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // theta = pi/4: both channels equal the original amplitude,
    // also the centered 2D mono image under the shared power reference.
    const float expected =
        kPannedAmp * static_cast<float>(std::cos(kQuarterPi));
    EXPECT_NEAR(expected, l, kTol);
    EXPECT_NEAR(expected, r, kTol);
}

TEST(PanLawGolden, HalfRightMatchesQuarterCircle) {
    // Emitter direction (0.5, 0, -sqrt(0.75)) is unit-x-component 0.5,
    // so pan = 0.5 and theta = 3pi/8.
    float l = 0.0f, r = 0.0f;
    CapturePanned(5.0f, 0.0f, -8.66025404f, &l, &r);
    ASSERT_GT(std::fabs(r), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    const float expectedL =
        kPannedAmp * static_cast<float>(std::cos(1.5 * kQuarterPi));
    const float expectedR =
        kPannedAmp * static_cast<float>(std::sin(1.5 * kQuarterPi));
    EXPECT_NEAR(expectedL, l, 1e-5f);
    EXPECT_NEAR(expectedR, r, 1e-5f);
    // The pair stays on the unity-center circle with squared gain 2.
    EXPECT_NEAR(kPannedAmp * kPannedAmp, l * l + r * r, 1e-5f);
}

TEST(PanLawGolden, InitialResolvedPanSnapsBeforeFirstFrame) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 10.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture captured;
    harkentest::PumpAndCapture(engine, 1, captured);
    ASSERT_EQ(kBlockFrames, captured.left.size());
    EXPECT_NEAR(0.0f, captured.left.front(), kTol);
    EXPECT_NEAR(kPannedAmp, captured.right.front(), kTol);
}

TEST(PanLawGolden, RuntimePanChangeReplacesResolvedGains) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), -10.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture left;
    harkentest::PumpAndCapture(engine, 1, left);
    EXPECT_NEAR(kPannedAmp, left.left.back(), kTol);
    EXPECT_NEAR(0.0f, left.right.back(), kTol);

    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(handle, harkentest::Vec3(10.0f, 0.0f,
                                                                0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    // The live retarget crossfades the equal-power pair over a fixed
    // control-rate window rather than snapping at the next block boundary
    // (see SpatialMotionAtControlCadenceHasNoBroadbandTransient); pumping
    // past the window's frame count guarantees the pair has landed on the
    // fully-resolved target by the last captured block.
    Capture right;
    harkentest::PumpAndCapture(engine, 3, right);
    EXPECT_NEAR(0.0f, right.left.back(), kTol);
    EXPECT_NEAR(kPannedAmp, right.right.back(), kTol);
}

TEST(PanLawGolden, SpatializationMixRetargetUsesLiveTransition) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 10.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture initial;
    harkentest::PumpAndCapture(engine, 1, initial);
    EXPECT_NEAR(0.0f, initial.left.back(), kTol);
    EXPECT_NEAR(kPannedAmp, initial.right.back(), kTol);

    ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 0.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture transition;
    harkentest::PumpAndCapture(engine, 1, transition);
    EXPECT_LT(transition.left.front(), transition.left.back());
    EXPECT_NEAR(kPannedAmp, transition.right.front(), kTol);
    EXPECT_LT(transition.right.back(), transition.right.front());

    Capture settled;
    harkentest::PumpAndCapture(engine, 3, settled);
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), settled.left.back(), kTol);
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), settled.right.back(), kTol);
}

TEST(PanLawGolden, SimultaneousPanWidthAndMixRetargetsStayContinuous) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), -10.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture before;
    harkentest::PumpAndCapture(engine, 1, before);

    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    engine->params.SetAttenuationSlot(render, harken::kTargetVoiceWidth, 1.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 0.0f));
    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(handle,
                                  harkentest::Vec3(10.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture transition;
    harkentest::PumpAndCapture(engine, 2, transition);
    EXPECT_NEAR(before.left.back(), transition.left.front(), kTol);
    EXPECT_NEAR(before.right.back(), transition.right.front(), kTol);
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), transition.left.back(), kTol);
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), transition.right.back(), kTol);
}

TEST(PanLawGolden, AuthoredZeroRampVolumeStillSnapsAtNextBlock) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -2.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture before;
    harkentest::PumpAndCapture(engine, 1, before);

    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -12.0f, 0));
    Capture after;
    harkentest::PumpAndCapture(engine, 1, after);
    ASSERT_EQ(kBlockFrames, after.left.size());
    const float expected =
        kPannedAmp * static_cast<float>(std::pow(10.0, -12.0 / 20.0)) *
        0.70710678f;
    EXPECT_NEAR(expected, after.left.front(), kTol);
    EXPECT_NEAR(expected, after.right.front(), kTol);
}

TEST(PanLawGolden, SpatialMotionAtControlCadenceHasNoBroadbandTransient) {
    MotionCapture fixed;
    ASSERT_NO_FATAL_FAILURE(
        CaptureCenterToRightMotion(MotionControl::FixedPosition, &fixed));
    {
        SCOPED_TRACE("same-position control");
        ExpectNoSpatialClick(fixed);
    }

    MotionCapture combined;
    ASSERT_NO_FATAL_FAILURE(
        CaptureCenterToRightMotion(MotionControl::Combined, &combined));
    {
        SCOPED_TRACE("center-to-right motion with linear attenuation");
        ExpectNoSpatialClick(combined);
    }

    MotionCapture panOnly;
    ASSERT_NO_FATAL_FAILURE(
        CaptureCenterToRightMotion(MotionControl::PanOnly, &panOnly));
    {
        SCOPED_TRACE("center-to-right pan-only motion");
        ExpectNoSpatialClick(panOnly);
    }

    MotionCapture attenuationOnly;
    ASSERT_NO_FATAL_FAILURE(CaptureCenterToRightMotion(
        MotionControl::AttenuationOnly, &attenuationOnly));
    {
        SCOPED_TRACE("symmetric attenuation-only motion");
        ExpectNoSpatialClick(attenuationOnly);
    }

    MotionCapture listenerAttenuationOnly;
    ASSERT_NO_FATAL_FAILURE(CaptureCenterToRightMotion(
        MotionControl::ListenerAttenuationOnly, &listenerAttenuationOnly));
    {
        SCOPED_TRACE("listener-only attenuation motion");
        ExpectNoSpatialClick(listenerAttenuationOnly);
    }

    MotionCapture widthOnly;
    ASSERT_NO_FATAL_FAILURE(
        CaptureCenterToRightMotion(MotionControl::WidthOnly, &widthOnly));
    {
        SCOPED_TRACE("off-center width-only motion");
        ExpectNoSpatialClick(widthOnly);
    }
}

} // namespace
