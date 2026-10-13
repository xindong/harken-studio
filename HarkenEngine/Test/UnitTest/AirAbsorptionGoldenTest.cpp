// Golden case: air-absorption lowpass interpolates in the LOG-frequency
// domain — the halfway distance must land on the geometric mean of the
// endpoint cutoffs, not their arithmetic mean.
//
// Profile: fcNear 20000 Hz, fcFar 500 Hz over [1, 101]; the voice sits at
// d = 51 (t = 0.5), so
//   fc = exp((ln 20000 + ln 500) / 2) = sqrt(20000 * 500) = 3162.28 Hz.
// A linear-frequency interpolation would give 10250 Hz instead — the
// param assertion pins the geometric mean and explicitly excludes the
// arithmetic one.
//
// Render evidence: a sine at exactly fc renders at |H(fc)| = 1/sqrt(2)
// through the 2nd-order Butterworth lowpass. The same setup with neutral
// air endpoints provides the reference; the ratio isolates the filter
// (rolloff and pan cancel). At the linear-interp cutoff 10250 Hz the same
// tone would pass at |H| = 0.995 — the ratio ceiling kills that too.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/ParamTargets.h"

#include <cmath>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr double kRate = 48000.0;
constexpr double kToneHz = 3162.2777; // sqrt(20000 * 500)
constexpr VnAttenuationId kProfile = harkentest::ObjectId(9);

float BlockRms(const std::vector<float>& s, std::size_t begin,
               std::size_t end) {
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += static_cast<double>(s[i]) * static_cast<double>(s[i]);
    }
    return static_cast<float>(
        std::sqrt(sum / static_cast<double>(end - begin)));
}

// Renders the bound tone at d = 51 and reports the settled RMS plus the
// resolved lowpass cutoff.
void CaptureAbsorbed(float fcNearHz, float fcFarHz, float* outRms,
                     float* outResolvedHz) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, kToneHz, kRate, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc pcm = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 101.0f);
    desc.airAbsorptionNearHz = fcNearHz;
    desc.airAbsorptionFarHz = fcFarHz;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -51.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kProfile));
    ASSERT_EQ(VN_OK, Vn_Update());

    *outResolvedHz = engine->params.ResolvedValue(harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceLowpassCutoff);

    Capture captured;
    harkentest::PumpAndCapture(engine, 6, captured);
    HARKENTEST_ASSERT_NON_SILENT(captured.left);
    // Filter and level transients settle well before the last two blocks.
    *outRms = BlockRms(captured.left, 4 * kBlockFrames, 6 * kBlockFrames);
}

TEST(AirAbsorptionGolden, HalfwayDistanceIsTheGeometricMean) {
    float rmsAbsorbed = 0.0f, resolvedHz = 0.0f;
    CaptureAbsorbed(20000.0f, 500.0f, &rmsAbsorbed, &resolvedHz);

    // Geometric mean, pinned tight; the arithmetic mean (10250 Hz) is
    // excluded outright.
    EXPECT_NEAR(3162.28f, resolvedHz, 5.0f);
    EXPECT_LT(resolvedHz, 5000.0f);

    float rmsOpen = 0.0f, resolvedOpenHz = 0.0f;
    CaptureAbsorbed(1.0e9f, 1.0e9f, &rmsOpen, &resolvedOpenHz);
    ASSERT_GT(rmsOpen, harkentest::kSilenceFloor)
        << "silence-guard: reference capture is silent";

    // The tone sits exactly on the interpolated cutoff: -3.01 dB.
    const float ratio = rmsAbsorbed / rmsOpen;
    EXPECT_NEAR(0.7071f, ratio, 0.05f);
    // A linear-frequency midpoint (10250 Hz) would pass the tone almost
    // untouched (|H| = 0.995): the ceiling refuses that world.
    EXPECT_LT(ratio, 0.9f);
}

} // namespace
