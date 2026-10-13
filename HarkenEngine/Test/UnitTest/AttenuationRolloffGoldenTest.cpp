// Golden case: the three built-in distance rolloff models at analytically
// chosen distances, proven on the output buffers.
//
// A spatialized mono DC voice straight ahead pans center (both channels
// original amplitude); binding a profile multiplies in the rolloff gain
// g via the volume target's dB conversion (20*log10 g, floored at -96):
//   linear:         dmin 1, dmax 101, d 51  -> g = 1 - 50/100     = 0.5
//   inverse-square: dmin 1,           d 2   -> g = (1/2)^2        = 0.25
//   logarithmic:    dmin 1, dmax 100, d 10  -> g = 1 - ln10/ln100 = 0.5
// Expected channel value: a * g. Tolerance 1e-4 absolute covers
// the dB round trip (20*log10 then 10^(dB/20)) in float.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/AttenuationDefaults.h"
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
constexpr float kAmp = 0.5f;
constexpr VnAttenuationId kProfile = harkentest::ObjectId(7);

// Plays one bound spatialized DC voice at distance `d` straight ahead and
// returns the steady left-channel value plus the resolved volume dB.
void CaptureRolloff(const VnAttenuationDesc& desc, float d, float* outLeft,
                    float* outResolvedDb) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -d);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kProfile));
    ASSERT_EQ(VN_OK, Vn_Update());

    *outResolvedDb =
        engine->params.ResolvedValue(harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume);

    Capture captured;
    harkentest::PumpAndCapture(engine, 2, captured);
    *outLeft = captured.left[kBlockFrames + kBlockFrames / 2];
}

TEST(AttenuationRolloffGolden, LinearHalfway) {
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    float left = 0.0f, resolvedDb = 0.0f;
    CaptureRolloff(desc, 51.0f, &left, &resolvedDb);
    ASSERT_GT(std::fabs(left), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(20.0f * std::log10(0.5f), resolvedDb, 1e-4f);
    EXPECT_NEAR(kAmp * 0.5f, left, 1e-4f);
}

TEST(AttenuationRolloffGolden, InverseSquareAtTwiceMin) {
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_INVERSE_SQUARE);
    float left = 0.0f, resolvedDb = 0.0f;
    CaptureRolloff(desc, 2.0f, &left, &resolvedDb);
    ASSERT_GT(std::fabs(left), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(20.0f * std::log10(0.25f), resolvedDb, 1e-4f);
    EXPECT_NEAR(kAmp * 0.25f, left, 1e-4f);
}

TEST(AttenuationRolloffGolden, LogarithmicAtLogMidpoint) {
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 100.0f, VN_ROLLOFF_LOGARITHMIC);
    float left = 0.0f, resolvedDb = 0.0f;
    CaptureRolloff(desc, 10.0f, &left, &resolvedDb);
    ASSERT_GT(std::fabs(left), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // d = 10 is the logarithmic midpoint of [1, 100]: g = 0.5 exactly.
    EXPECT_NEAR(20.0f * std::log10(0.5f), resolvedDb, 1e-3f);
    EXPECT_NEAR(kAmp * 0.5f, left, 1e-4f);
}

TEST(AttenuationRolloffGolden, LinearBeyondMaxLandsOnFloor) {
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    float left = 0.0f, resolvedDb = 0.0f;
    CaptureRolloff(desc, 500.0f, &left, &resolvedDb);
    // g = 0 converts to exactly the documented -96 dB floor, never
    // -infinity. The literal is intentional: asserting the engine
    // constant against itself would be self-referential and blind to a
    // drifted floor.
    EXPECT_FLOAT_EQ(-96.0f, resolvedDb);
    // -96 dB of a 0.5 source is ~7.9e-6 — inaudibly below the silence
    // floor by construction, so the render assertion is the param fact
    // above, not a silent-vs-silent buffer comparison.
    EXPECT_LT(std::fabs(left), 1e-4f);
}

} // namespace
