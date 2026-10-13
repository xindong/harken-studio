// Distance must not widen an authored point source without a width curve.
// Constant 0 dB volume and neutral air isolate the rendered image.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc3D;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.4f;
constexpr VnAttenuationId kProfile = harkentest::ObjectId(11);

void CaptureAtDistance(float d, float* outL, float* outR,
                       std::uint32_t channels = 1, bool widthCurve = false,
                       float authoredWidth = 0) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> dc(48000 * channels, kAmp);
    if (channels == 2)
        for (std::size_t i = 0; i < 48000; ++i) dc[i * 2 + 1] = .1f;
    const VnPCMDesc pcm = harkentest::InterleavedDesc(dc, channels, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 11.0f);
    // Constant 0 dB volume override: distance must not change level, so
    // any channel difference below is the width default's doing.
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 1;
    desc.volumeCurve.points[0].x = 0.0f;
    desc.volumeCurve.points[0].y = 0.0f;
    if (widthCurve) {
        desc.widthCurve.enabled = 1;
        desc.widthCurve.pointCount = 2;
        desc.widthCurve.points[0] = {1, 1};
        desc.widthCurve.points[1] = {11, 0};
        desc.widthCurve.handles[0] = harkentest::EndpointHandles(
            desc.widthCurve.points[0], desc.widthCurve.points[1]);
    }
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), d, 0.0f, 0.0f); // hard right
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kProfile));
    ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, authoredWidth));
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture captured;
    const auto before = engine->mixer.BlocksRendered();
    harkentest::PumpAndCapture(engine, 2, captured);
    ASSERT_EQ(before + 2, engine->mixer.BlocksRendered());
    ASSERT_EQ(2u * kBlockFrames, captured.left.size());
    ASSERT_EQ(captured.left.size(), captured.right.size());
    *outL = captured.left[kBlockFrames + kBlockFrames / 2];
    *outR = captured.right[kBlockFrames + kBlockFrames / 2];
}

TEST(WidthCollapseGolden, DefaultMonoRemainsAPointAtEveryDistance) {
    for (float distance : {.25f, 1.0f, 6.0f, 11.0f, 20.0f}) {
        SCOPED_TRACE(distance);
        float l = 0.0f, r = 0.0f;
        ASSERT_NO_FATAL_FAILURE(CaptureAtDistance(distance, &l, &r));
        ASSERT_GT(std::fabs(r), harkentest::kSilenceFloor);
        EXPECT_NEAR(0, l, 1e-5f);
        EXPECT_NEAR(kAmp * std::sqrt(2.0f), r, 1e-4f);
    }
}

TEST(WidthCollapseGolden, DefaultStereoRemainsAPointAtEveryDistance) {
    for (float distance : {.25f, 1.0f, 6.0f, 11.0f, 20.0f}) {
        SCOPED_TRACE(distance);
        float l = 0.0f, r = 0.0f;
        ASSERT_NO_FATAL_FAILURE(CaptureAtDistance(distance, &l, &r, 2));
        ASSERT_GT(std::fabs(r), harkentest::kSilenceFloor);
        EXPECT_NEAR(0, l, 1e-5f);
        EXPECT_NEAR((kAmp + .1f) / std::sqrt(2.0f), r, 1e-4f);
    }
}

TEST(WidthCollapseGolden, ExplicitWidthCurveStillControlsTheImage) {
    for (std::uint32_t channels : {1u, 2u}) {
        float l = 0.0f, r = 0.0f;
        ASSERT_NO_FATAL_FAILURE(CaptureAtDistance(1, &l, &r, channels, true));
        EXPECT_NEAR(kAmp, l, 1e-4f);
        EXPECT_NEAR(channels == 1 ? kAmp : .1f, r, 1e-4f);
        ASSERT_NO_FATAL_FAILURE(CaptureAtDistance(11, &l, &r, channels, true));
        EXPECT_NEAR(0, l, 1e-5f);
        EXPECT_NEAR(channels == 1 ? kAmp * std::sqrt(2.0f)
                                 : (kAmp + .1f) / std::sqrt(2.0f), r, 1e-4f);
    }
}

TEST(WidthCollapseGolden, AuthoredWidthRemainsAvailableWithoutAWidthCurve) {
    for (float distance : {1.0f, 11.0f}) {
        float l = 0.0f, r = 0.0f;
        ASSERT_NO_FATAL_FAILURE(CaptureAtDistance(distance, &l, &r, 2, false, 1));
        EXPECT_NEAR(kAmp, l, 1e-4f);
        EXPECT_NEAR(.1f, r, 1e-4f);
    }
}

} // namespace
