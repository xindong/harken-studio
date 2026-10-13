// Error-contract coverage for the spatial API surface: listener pose,
// attenuation profile registration, voice binding, emitter orientation,
// and the 2D/3D play-descriptor discrimination.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/Spatializer.h"
#include "harken/param/ParamTargets.h"

#include <cmath>
#include <limits>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::Vec3;

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr std::uint32_t kBlockFrames = 256;

TEST(SpatialApi, EverySpatialCallRefusesWithoutInit) {
    const VnVec3 v = Vec3(0.0f, 0.0f, -1.0f);
    const VnVec3 up = Vec3(0.0f, 1.0f, 0.0f);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_SetListener(v, v, up));
    const VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_BindVoiceAttenuation(1, harkentest::ObjectId(1)));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_SetVoiceOrientation(1, v));
}

TEST(SpatialApi, ListenerRefusesNonFiniteAndDegenerateBases) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnVec3 origin = Vec3(0.0f, 0.0f, 0.0f);
    const VnVec3 fwd = Vec3(0.0f, 0.0f, -1.0f);
    const VnVec3 up = Vec3(0.0f, 1.0f, 0.0f);

    EXPECT_EQ(VN_OK, Vn_SetListener(origin, fwd, up));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetListener(Vec3(kNan, 0.0f, 0.0f), fwd, up));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetListener(origin, Vec3(kInf, 0.0f, 0.0f), up));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetListener(origin, fwd, Vec3(0.0f, kNan, 0.0f)));
    // Zero-length forward/up and a parallel pair leave no right axis.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetListener(origin, Vec3(0.0f, 0.0f, 0.0f), up));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetListener(origin, fwd, Vec3(0.0f, 0.0f, 0.0f)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetListener(origin, up, up));
}

TEST(SpatialApi, RegisterRefusesMalformedDescriptors) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), nullptr));

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    desc.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc = MakeAttenuationDesc(0.0f, 10.0f); // dmin must be > 0
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc = MakeAttenuationDesc(10.0f, 10.0f); // empty span
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc = MakeAttenuationDesc(kNan, 10.0f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc = MakeAttenuationDesc(1.0f, 10.0f, 3u); // unknown rolloff
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc = MakeAttenuationDesc(1.0f, 10.0f);
    desc.airAbsorptionNearHz = 0.0f; // log-domain endpoint must be > 0
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc = MakeAttenuationDesc(1.0f, 10.0f);
    desc.proximityRadius = -1.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
}

TEST(SpatialApi, ProximityFrequencyZeroFallsBackAndNonZeroIsAuthored) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // A recompiled caller that never touches the appended field leaves it
    // zero-initialized: the full-size descriptor must register and inherit the
    // 250 Hz compatibility corner rather than be refused as an authored 0 Hz.
    VnAttenuationDesc unset = MakeAttenuationDesc(1.0f, 10.0f);
    ASSERT_EQ(sizeof(VnAttenuationDesc), unset.structSize);
    ASSERT_FLOAT_EQ(0.0f, unset.proximityFrequency);
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &unset));
    const harken::AttenuationProfile* fallback = engine->spatial.FindProfile(harkentest::ObjectId(1));
    ASSERT_NE(nullptr, fallback);
    EXPECT_FLOAT_EQ(harken::kToneLowShelfDefaultHz,
                    fallback->proximityFrequency);

    // A non-zero corner is authored and stored verbatim.
    VnAttenuationDesc authored = MakeAttenuationDesc(1.0f, 10.0f);
    authored.proximityFrequency = 400.0f;
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(2), &authored));
    const harken::AttenuationProfile* stored = engine->spatial.FindProfile(harkentest::ObjectId(2));
    ASSERT_NE(nullptr, stored);
    EXPECT_FLOAT_EQ(400.0f, stored->proximityFrequency);

    // Only a non-zero corner is validated: a negative or non-finite authored
    // value is malformed and refused.
    VnAttenuationDesc negative = MakeAttenuationDesc(1.0f, 10.0f);
    negative.proximityFrequency = -1.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(3), &negative));

    VnAttenuationDesc nonFinite = MakeAttenuationDesc(1.0f, 10.0f);
    nonFinite.proximityFrequency = kNan;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(4), &nonFinite));
}

TEST(SpatialApi, RegisterRefusesMalformedCones) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    desc.coneEnabled = 1;
    desc.coneInnerDeg = 100.0f; // outer < inner
    desc.coneOuterDeg = 90.0f;
    desc.coneOuterLowpassHz = 2000.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.coneInnerDeg = 30.0f;
    desc.coneOuterDeg = 200.0f; // beyond the half-space
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.coneOuterDeg = 90.0f;
    desc.coneOuterLowpassHz = 0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.coneOuterLowpassHz = 2000.0f;
    desc.coneOuterGainDb = kNan;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    // The same block passes once the cone is disabled: disabled cone
    // fields are dormant, not validated.
    desc.coneEnabled = 0;
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
}

TEST(SpatialApi, RegisterRefusesMalformedOverrideCurves) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 2;
    desc.volumeCurve.points[0] = {1.0f, 0.0f};
    desc.volumeCurve.points[1] = {9.0f, -6.0f};
    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);

    desc.volumeCurve.pointCount = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.volumeCurve.pointCount = VN_ATTENUATION_MAX_CURVE_POINTS + 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.volumeCurve.pointCount = 2;
    desc.volumeCurve.points[0] = {5.0f, 0.0f};
    desc.volumeCurve.points[1] = {5.0f, -6.0f};
    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.volumeCurve.points[0] = {1.0f, 0.0f};
    desc.volumeCurve.points[1] = {9.0f, -6.0f};
    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);
    desc.volumeCurve.handles[0].h1x = 0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);
    desc.volumeCurve.handles[0].h1y = -97.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);
    desc.volumeCurve.points[1].y = 0.01f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.volumeCurve.points[1].y = -6.0f;

    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);
    desc.volumeCurve.handles[1].h2y = 1.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.volumeCurve.handles[1] = {};
    desc.volumeCurve.handles[1].h2y = -0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.volumeCurve.handles[1] = {};
    desc.lowpassCurve.enabled = 1;
    desc.lowpassCurve.pointCount = 2;
    desc.lowpassCurve.points[0] = {1.0f, 20000.0f};
    desc.lowpassCurve.points[1] = {9.0f, 1000.0f};
    desc.lowpassCurve.handles[0] = harkentest::EndpointHandles(
        desc.lowpassCurve.points[0], desc.lowpassCurve.points[1]);
    desc.lowpassCurve.handles[0].h2y = 0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.lowpassCurve.handles[0] = harkentest::EndpointHandles(
        desc.lowpassCurve.points[0], desc.lowpassCurve.points[1]);
    desc.lowpassCurve.points[1].y = 0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.lowpassCurve.enabled = 0;
    desc.widthCurve.enabled = 1;
    desc.widthCurve.pointCount = 2;
    desc.widthCurve.points[0] = {1.0f, 1.0f};
    desc.widthCurve.points[1] = {9.0f, 0.0f};
    desc.widthCurve.handles[0] = harkentest::EndpointHandles(
        desc.widthCurve.points[0], desc.widthCurve.points[1]);
    desc.widthCurve.handles[0].h1y = 1.01f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
    desc.widthCurve.handles[0] = harkentest::EndpointHandles(
        desc.widthCurve.points[0], desc.widthCurve.points[1]);
    desc.widthCurve.points[1].y = -0.01f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    desc.widthCurve.enabled = 0;
    desc.volumeCurve.enabled = 0;
    desc.volumeCurve.pointCount = 1;
    desc.volumeCurve.points[0] = {kNan, kInf};
    desc.volumeCurve.handles[0].h1y = -0.0f;
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));

    VnAttenuationDesc single = MakeAttenuationDesc(1.0f, 10.0f);
    single.volumeCurve.enabled = 1;
    single.volumeCurve.pointCount = 1;
    single.volumeCurve.points[0] = {1.0f, -6.0f};
    single.volumeCurve.handles[0].h1y = -0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(2), &single));
    single.volumeCurve.handles[0] = {};
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(2), &single));
}

TEST(SpatialApi, DuplicateIdAndFullTableRefuse) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);

    for (std::uint32_t i = 0; i < harken::kMaxAttenuationProfiles; ++i) {
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(100 + i), &desc))
            << "profile " << i;
    }
    // A live id never silently replaces (PCM registration rule).
    EXPECT_EQ(VN_IN_USE, Vn_RegisterAttenuation(harkentest::ObjectId(100), &desc));
    // The 65th distinct id exhausts the table.
    EXPECT_EQ(VN_NO_ATTENUATION, Vn_RegisterAttenuation(harkentest::ObjectId(9999), &desc));
}

TEST(SpatialApi, BindRefusesUnknownIdsAndStaleHandles) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(4096, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(5), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -1.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(6)));
    EXPECT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(5)));

    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    engine->nullBackend.Pump(1); // the audio side releases the slot
    EXPECT_EQ(VN_STALE_HANDLE, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(5)));
}

TEST(SpatialApi, OrientationValidatesValueThenHandle) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(4096, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -1.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceOrientation(handle, Vec3(kNan, 0.0f, 0.0f)));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceOrientation(handle, Vec3(0.0f, 0.0f, 0.0f)));
    EXPECT_EQ(VN_OK,
              Vn_SetVoiceOrientation(handle, Vec3(0.0f, 0.0f, 1.0f)));

    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    engine->nullBackend.Pump(1);
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetVoiceOrientation(handle, Vec3(0.0f, 0.0f, 1.0f)));
    // The discriminating case for the ordering contract: a bad value on
    // a stale handle must still report the value error — the vector is
    // inspected before the handle.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceOrientation(handle, Vec3(kNan, 0.0f, 0.0f)));
}

TEST(SpatialApi, SpatializedPlayAcceptsMonoStereoAndDemandsCleanFields) {
    EngineSession session(MakeNullConfig(4, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> mono = harkentest::MakeDC(4096, 0.5f);
    const VnPCMDesc monoPcm = MonoDesc(mono, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &monoPcm));

    // Interleaved stereo source is valid in both spatial modes.
    const std::vector<float> stereo(8192, 0.25f);
    VnPCMDesc stereoPcm{};
    stereoPcm.structSize = sizeof(VnPCMDesc);
    stereoPcm.sampleRate = 48000;
    stereoPcm.channels = 2;
    stereoPcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    stereoPcm.policy = VN_PCM_POLICY_COPY;
    stereoPcm.frames = stereo.size() / 2;
    stereoPcm.samplesInterleaved = stereo.data();
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &stereoPcm));

    VnVoiceHandle handle = 0;
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(2), 0.0f, 0.0f, -1.0f);
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    play.spatialized = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -1.0f);
    play.spatialized = 2; // not a known mode
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc3D(harkentest::ObjectId(1), kNan, 0.0f, -1.0f); // 3D position is input
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -1.0f);
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
}

TEST(SpatialApi, PositioningA2DVoiceHasNoAcousticEffect) {
    // Two sessions, identical except the second moves its 2D voice hard
    // right; the captures must match sample for sample.
    Capture still;
    {
        EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> sine =
            harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
        const VnPCMDesc pcm = MonoDesc(sine, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_Update());
        harkentest::PumpAndCapture(session.Engine(), 3, still);
    }
    Capture moved;
    {
        EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> sine =
            harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
        const VnPCMDesc pcm = MonoDesc(sine, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        VnVec3 pos = Vec3(10.0f, 0.0f, 0.0f);
        ASSERT_EQ(VN_OK, Vn_SetVoicePosition(handle, pos));
        ASSERT_EQ(VN_OK, Vn_Update());
        harkentest::PumpAndCapture(session.Engine(), 3, moved);
    }
    HARKENTEST_ASSERT_NON_SILENT(still.left);
    ASSERT_EQ(still.left.size(), moved.left.size());
    for (std::size_t i = 0; i < still.left.size(); ++i) {
        ASSERT_EQ(still.left[i], moved.left[i]) << "left sample " << i;
        ASSERT_EQ(still.right[i], moved.right[i]) << "right sample " << i;
    }
}

TEST(SpatialApi, DistanceScaleMatchesEffectiveDistanceAcrossDomainTargets) {
    EngineSession session(MakeNullConfig(8, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc builtIn = MakeAttenuationDesc(1.0f, 20.0f);
    builtIn.airAbsorptionNearHz = 2000.0f;
    builtIn.airAbsorptionFarHz = 20000.0f;
    builtIn.proximityRadius = 8.0f;
    builtIn.proximityMaxBoostDb = 6.0f;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(10), &builtIn));

    VnAttenuationDesc overrideDesc = builtIn;
    overrideDesc.proximityRadius = 0.0f;
    overrideDesc.proximityMaxBoostDb = 0.0f;
    overrideDesc.volumeCurve.enabled = 1;
    overrideDesc.volumeCurve.pointCount = 2;
    overrideDesc.volumeCurve.points[0] = {1.0f, -1.0f};
    overrideDesc.volumeCurve.points[1] = {20.0f, -20.0f};
    overrideDesc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        overrideDesc.volumeCurve.points[0],
        overrideDesc.volumeCurve.points[1]);
    overrideDesc.lowpassCurve.enabled = 1;
    overrideDesc.lowpassCurve.pointCount = 2;
    overrideDesc.lowpassCurve.points[0] = {1.0f, 18000.0f};
    overrideDesc.lowpassCurve.points[1] = {20.0f, 2000.0f};
    overrideDesc.lowpassCurve.handles[0] = harkentest::EndpointHandles(
        overrideDesc.lowpassCurve.points[0],
        overrideDesc.lowpassCurve.points[1]);
    overrideDesc.widthCurve.enabled = 1;
    overrideDesc.widthCurve.pointCount = 2;
    overrideDesc.widthCurve.points[0] = {1.0f, 0.9f};
    overrideDesc.widthCurve.points[1] = {20.0f, 0.1f};
    overrideDesc.widthCurve.handles[0] = harkentest::EndpointHandles(
        overrideDesc.widthCurve.points[0],
        overrideDesc.widthCurve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(11), &overrideDesc));

    VnVoiceHandle builtScaled = 0;
    VnVoiceHandle builtUnit = 0;
    VnVoiceHandle overrideScaled = 0;
    VnVoiceHandle overrideUnit = 0;
    VnPlayDesc builtScaledPlay = MakePlayDesc3D(harkentest::ObjectId(1), 8.0f, 0.0f, 0.0f);
    VnPlayDesc builtUnitPlay = MakePlayDesc3D(harkentest::ObjectId(1), 4.0f, 0.0f, 0.0f);
    VnPlayDesc overrideScaledPlay = MakePlayDesc3D(harkentest::ObjectId(1), 8.0f, 0.0f, 0.0f);
    VnPlayDesc overrideUnitPlay = MakePlayDesc3D(harkentest::ObjectId(1), 4.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&builtScaledPlay, &builtScaled));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&builtUnitPlay, &builtUnit));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&overrideScaledPlay, &overrideScaled));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&overrideUnitPlay, &overrideUnit));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(builtScaled, harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(builtUnit, harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(overrideScaled, harkentest::ObjectId(11)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(overrideUnit, harkentest::ObjectId(11)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(builtScaled, 2.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(overrideScaled, 2.0f));
    ASSERT_EQ(VN_OK, Vn_Update());

    const VnVoiceHandle builtScaledRender =
        harkentest::RenderHandle(engine, builtScaled);
    const VnVoiceHandle builtUnitRender =
        harkentest::RenderHandle(engine, builtUnit);
    const VnVoiceHandle overrideScaledRender =
        harkentest::RenderHandle(engine, overrideScaled);
    const VnVoiceHandle overrideUnitRender =
        harkentest::RenderHandle(engine, overrideUnit);
    struct DistanceScaleCase {
        VnVoiceHandle unitRender;
        VnVoiceHandle scaledRender;
        const char* label;
    };
    const DistanceScaleCase cases[] = {
        {builtUnitRender, builtScaledRender, "built-in"},
        {overrideUnitRender, overrideScaledRender, "override"},
    };
    for (const DistanceScaleCase& testCase : cases) {
        for (const auto target : {harken::kTargetVoiceVolume,
                                  harken::kTargetVoiceLowpassCutoff,
                                  harken::kTargetVoiceWidth,
                                  harken::kTargetVoiceLowShelfGainDb}) {
            EXPECT_NEAR(
                engine->params.ResolvedValue(testCase.unitRender, target),
                engine->params.ResolvedValue(testCase.scaledRender, target),
                1e-5f)
                << testCase.label << " target " << target;
        }
    }
}

TEST(SpatialApi, DistanceScaleLeavesPanAndConeAnglePhysical) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 20.0f);
    desc.volumeCurve.enabled = 1;
    desc.volumeCurve.pointCount = 2;
    desc.volumeCurve.points[0] = {1.0f, 0.0f};
    desc.volumeCurve.points[1] = {20.0f, 0.0f};
    desc.volumeCurve.handles[0] = harkentest::EndpointHandles(
        desc.volumeCurve.points[0], desc.volumeCurve.points[1]);
    desc.coneEnabled = 1;
    desc.coneInnerDeg = 20.0f;
    desc.coneOuterDeg = 60.0f;
    desc.coneOuterGainDb = -12.0f;
    desc.coneOuterLowpassHz = 20000.0f;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(12), &desc));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 8.0f, 0.0f, -6.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(12)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOrientation(handle, Vec3(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    const float pan = engine->params.ResolvedValue(render, harken::kTargetVoicePan);
    const float volume =
        engine->params.ResolvedValue(render, harken::kTargetVoiceVolume);
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 4.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(pan, engine->params.ResolvedValue(render,
                                                      harken::kTargetVoicePan));
    EXPECT_FLOAT_EQ(volume,
                    engine->params.ResolvedValue(render,
                                                 harken::kTargetVoiceVolume));
}

} // namespace
