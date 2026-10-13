// End-to-end attenuation override goldens. Authored volume coordinates are
// dB, while Runtime converts every point and handle y to amplitude before
// Bezier evaluation and converts the result back to dB.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/ParamTargets.h"

#include <cmath>
#include <iterator>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.5f;
constexpr VnAttenuationId kProfile = harkentest::ObjectId(19);

struct BezierCase {
    VnCurveHandlePair handle;
    float expectedDb[3];
};

// These values come from a 200-iteration double bisection oracle over the
// amplitude-domain cubic. Runtime uses its fixed 32-iteration inverse.
constexpr BezierCase kBezierCases[] = {
    {{0.0f, 0.0f, 100.0f, -96.0f}, {-2.4987f, -6.0205f, -12.0408f}},
    {{25.0f, 0.0f, 75.0f, -96.0f}, {-1.7487f, -6.0205f, -14.7810f}},
    {{100.0f, 0.0f, 0.0f, -96.0f}, {-0.2621f, -6.0204f, -30.5332f}},
};

void CaptureProfile(const VnCurveHandlePair& handle, float distance,
                    float* outLeft, float* outResolvedDb) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 2;
    desc.volumeCurve.points[0] = {0.0f, 0.0f};
    desc.volumeCurve.points[1] = {100.0f, -96.0f};
    desc.volumeCurve.handles[0] = handle;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -distance);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, kProfile));
    ASSERT_EQ(VN_OK, Vn_Update());

    *outResolvedDb = engine->params.ResolvedValue(
        harkentest::RenderHandle(engine, voice), harken::kTargetVoiceVolume);

    Capture captured;
    harkentest::PumpAndCapture(engine, 2, captured);
    *outLeft = captured.left[kBlockFrames + kBlockFrames / 2];
}

TEST(AttenuationOverrideGolden, BezierVolumeEvaluatesInAmplitudeDomain) {
    constexpr float kDistances[] = {25.0f, 50.0f, 75.0f};
    for (std::size_t group = 0; group < std::size(kBezierCases); ++group) {
        for (std::size_t sample = 0; sample < std::size(kDistances); ++sample) {
            float left = 0.0f;
            float resolvedDb = 0.0f;
            CaptureProfile(kBezierCases[group].handle, kDistances[sample],
                           &left, &resolvedDb);
            ASSERT_GT(std::fabs(left), harkentest::kSilenceFloor)
                << "group " << group << " sample " << sample;
            EXPECT_NEAR(kBezierCases[group].expectedDb[sample], resolvedDb,
                        1.0e-4f)
                << "group " << group << " sample " << sample;
            const float expectedGain =
                std::pow(10.0f,
                         kBezierCases[group].expectedDb[sample] / 20.0f);
            EXPECT_NEAR(kAmp * expectedGain, left, 2.0e-5f)
                << "group " << group << " sample " << sample;
        }
    }
}

TEST(AttenuationOverrideGolden, DisabledOverrideHandsBackTheDefault) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    desc.volumeCurve.enabled = 0;
    desc.volumeCurve.pointCount = 2;
    desc.volumeCurve.points[0] = {0.0f, 12.0f};
    desc.volumeCurve.points[1] = {100.0f, -120.0f};
    desc.volumeCurve.handles[6].h1y = -0.0f;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -51.0f);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, kProfile));
    ASSERT_EQ(VN_OK, Vn_Update());
    const float resolvedDb = engine->params.ResolvedValue(
        harkentest::RenderHandle(engine, voice), harken::kTargetVoiceVolume);
    EXPECT_NEAR(20.0f * std::log10(0.5f), resolvedDb, 1.0e-4f);
}

} // namespace
