// Golden case: near-field low-frequency emphasis inside the proximity
// radius, rendered through the tone stage's low shelf.
//
// Profile: proximityRadius 10, maxBoostDb 12, constant 0 dB volume
// override (isolates the shelf), neutral air. At d = 5 the boost is
//   shelfDb = 12 * (1 - 5/10) = 6 dB,
// and a DC source measures the shelf's DC gain exactly: 10^(6/20). At
// d = 15 (outside the radius) the shelf resolves 0 dB, which disables the
// section — an exact bypass, asserted bit-for-bit against a profile with
// no proximity feature at all.
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
constexpr float kAmp = 0.2f;
constexpr VnAttenuationId kProfile = harkentest::ObjectId(13);

void CaptureShelved(float proximityRadius, float maxBoostDb, float d,
                    Capture* out, float* outResolvedDb) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 101.0f);
    desc.proximityRadius = proximityRadius;
    desc.proximityMaxBoostDb = maxBoostDb;
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 1;
    desc.volumeCurve.points[0].x = 0.0f;
    desc.volumeCurve.points[0].y = 0.0f;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -d);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kProfile));
    ASSERT_EQ(VN_OK, Vn_Update());

    *outResolvedDb = engine->params.ResolvedValue(harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceLowShelfGainDb);
    harkentest::PumpAndCapture(engine, 6, *out);
}

TEST(NearFieldShelfGolden, HalfRadiusBoostsHalfTheMaximum) {
    Capture captured;
    float resolvedDb = 0.0f;
    CaptureShelved(10.0f, 12.0f, 5.0f, &captured, &resolvedDb);
    HARKENTEST_ASSERT_NON_SILENT(captured.left);

    EXPECT_NEAR(6.0f, resolvedDb, 1e-4f);
    // DC gain of the RBJ low shelf is exactly its shelf gain; the biquad
    // has settled by the last block.
    const float expected =
        kAmp * std::pow(10.0f, 6.0f / 20.0f);
    const float settled = captured.left[6 * kBlockFrames - 1];
    EXPECT_NEAR(expected, settled, expected * 2e-3f);
}

TEST(NearFieldShelfGolden, OutsideRadiusIsBitExactBypass) {
    Capture withProximity;
    float resolvedDb = -1.0f;
    CaptureShelved(10.0f, 12.0f, 15.0f, &withProximity, &resolvedDb);
    HARKENTEST_ASSERT_NON_SILENT(withProximity.left);
    EXPECT_FLOAT_EQ(0.0f, resolvedDb);

    Capture withoutProximity;
    float resolvedNoneDb = -1.0f;
    CaptureShelved(0.0f, 0.0f, 15.0f, &withoutProximity, &resolvedNoneDb);
    EXPECT_FLOAT_EQ(0.0f, resolvedNoneDb);

    // 0 dB means the section never enters the path: sample-for-sample
    // identity with a profile that has no proximity feature at all.
    ASSERT_EQ(withProximity.left.size(), withoutProximity.left.size());
    for (std::size_t i = 0; i < withProximity.left.size(); ++i) {
        ASSERT_EQ(withoutProximity.left[i], withProximity.left[i])
            << "left sample " << i;
        ASSERT_EQ(withoutProximity.right[i], withProximity.right[i])
            << "right sample " << i;
    }
}

} // namespace
