// Validation hardening for the spatial surface: vector lengths are
// computed in the double domain (finite float components near FLT_MAX
// must normalize correctly instead of overflowing to a zero axis or a
// spurious refusal), and every descriptor enable flag is a strict 0/1
// contract.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/ParamTargets.h"
#include "harken/param/Spatializer.h"

#include <cmath>
#include <type_traits>

namespace {

using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::Vec3;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kHuge = 3.0e38f; // finite, but its float square is inf

static_assert(noexcept(
    std::declval<harken::Spatializer&>().CommitPreparedProfile(
        static_cast<harken::Spatializer::PreparedProfileToken*>(nullptr))));

TEST(SpatialValidation,
     PreparedProfileFailureIsAtomicAndCommitUsesTheExactSlot) {
    harken::EngineConfig config{};
    harken::Spatializer spatial(config);
    VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 10.0f, VN_ROLLOFF_LINEAR);
    harken::Spatializer::PreparedProfileToken token;

    VnAttenuationDesc invalid = desc;
    invalid.maxDistance = invalid.minDistance;
    EXPECT_EQ(VN_INVALID_ARG,
              spatial.PrepareRegisterProfile(harkentest::ObjectId(17), &invalid, 0, &token));
    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(0u, spatial.ProfileCount());

    spatial.SetProfileCommitPageLimitForTests(0);
    EXPECT_EQ(VN_BACKPRESSURE,
              spatial.PrepareRegisterProfile(harkentest::ObjectId(17), &desc, 0, &token));
    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(0u, spatial.ProfileCount());
    spatial.SetProfileCommitPageLimitForTests(SIZE_MAX);
    EXPECT_EQ(VN_OK,
              spatial.PrepareRegisterProfile(harkentest::ObjectId(17), &desc, 0, &token));
    ASSERT_TRUE(token.IsPrepared());
    spatial.SetProfileCommitPageLimitForTests(0);
    EXPECT_EQ(0u, token.SlotIndex());
    spatial.CommitPreparedProfile(&token);

    EXPECT_FALSE(token.IsPrepared());
    ASSERT_EQ(1u, spatial.ProfileCount());
    const harken::AttenuationProfile* profile = spatial.FindProfile(harkentest::ObjectId(17));
    ASSERT_NE(nullptr, profile);
    EXPECT_FLOAT_EQ(1.0f, profile->minDistance);
    EXPECT_FLOAT_EQ(10.0f, profile->maxDistance);

    desc.maxDistance = 25.0f;
    EXPECT_EQ(VN_OK,
              spatial.PrepareReplaceProfile(harkentest::ObjectId(17), &desc, &token));
    ASSERT_TRUE(token.IsPrepared());
    EXPECT_EQ(0u, token.SlotIndex());
    spatial.CommitPreparedProfile(&token);
    EXPECT_FLOAT_EQ(25.0f, spatial.FindProfile(harkentest::ObjectId(17))->maxDistance);
}

TEST(SpatialValidation, HugeFiniteOrientationNormalizesInsteadOfZeroAxis) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    // Cone: 0 dB inside 30 deg, -24 dB at/after 90 deg. The voice sits
    // at (0,0,-1) so "toward the listener" is (0,0,1).
    VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 100.0f, VN_ROLLOFF_INVERSE_SQUARE);
    desc.coneEnabled = 1;
    desc.coneInnerDeg = 30.0f;
    desc.coneOuterDeg = 90.0f;
    desc.coneOuterGainDb = -24.0f;
    desc.coneOuterLowpassHz = 2000.0f;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(7), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -1.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(7)));

    // A huge finite vector pointing straight at the listener. With
    // float-domain length math its square overflows to infinity and the
    // stored axis collapses to zero (reading as theta = 90, -24 dB);
    // the double-domain length normalizes it to (0,0,1): on-axis, 0 dB.
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceOrientation(handle, Vec3(0.0f, 0.0f, kHuge)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(0.0f,
                engine->params.ResolvedValue(harkentest::RenderHandle(engine,
                    handle),
                                             harken::kTargetVoiceVolume),
                1e-3f);
}

TEST(SpatialValidation, HugeFiniteListenerBasisIsAcceptedAndCorrect) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    // Scaled versions of the default pose: forward (0,0,-1), up (0,1,0)
    // at huge finite magnitude. The float-domain cross product overflows
    // to infinity and used to refuse this representable pose; the
    // double-domain basis accepts it and derives right = +x.
    ASSERT_EQ(VN_OK,
              Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                             Vec3(0.0f, 0.0f, -kHuge),
                             Vec3(0.0f, kHuge, 0.0f)));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 10.0f, 0.0f, 0.0f); // hard right
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(1.0f, engine->params.ResolvedValue(harkentest::RenderHandle(
        engine, handle), harken::kTargetVoicePan));
}

TEST(SpatialValidation, ConeEnableFlagAboveOneIsRefused) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    desc.coneEnabled = 2;
    desc.coneInnerDeg = 30.0f;
    desc.coneOuterDeg = 90.0f;
    desc.coneOuterGainDb = -12.0f;
    desc.coneOuterLowpassHz = 2000.0f; // fields valid; only the flag is not
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.coneEnabled = 1;
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
}

TEST(SpatialValidation, CurveEnableFlagsAboveOneAreRefused) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    // A perfectly valid single-point curve; only the flag is out of
    // contract, per curve slot.
    desc.volumeCurve.pointCount = 1;
    desc.volumeCurve.points[0] = {0.0f, 0.0f};
    desc.volumeCurve.enabled = 2;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.volumeCurve.enabled = 0;

    desc.lowpassCurve.pointCount = 1;
    desc.lowpassCurve.points[0] = {0.0f, 1000.0f};
    desc.lowpassCurve.enabled = 3;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.lowpassCurve.enabled = 0;

    desc.widthCurve.pointCount = 1;
    desc.widthCurve.points[0] = {0.0f, 0.5f};
    desc.widthCurve.enabled = 2;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.widthCurve.enabled = 1;
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
}

} // namespace
