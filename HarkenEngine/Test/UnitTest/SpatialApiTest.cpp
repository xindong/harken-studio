// Error-contract coverage for the spatial API surface: listener pose,
// attenuation profile registration, voice binding, emitter orientation,
// and the 2D/3D play-descriptor discrimination.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/Spatializer.h"
#include "harken/param/ParamTargets.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

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
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_SetEmitterDistanceScale(1, 2.0f));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_SetListenerDistanceReference(0));
}

TEST(SpatialApi, DistanceReferenceValidatesFullHandleAndResetsAtInit) {
    const auto profile = MakeAttenuationDesc(1.0f, 101.0f);
    {
        auto config = MakeNullConfig(1);
        config.maxEmitters = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &profile));
        const auto distanceDb = [&] {
            return engine->spatial.AttenuationVolumeDb(
                engine->emitters, harkentest::ObjectId(9), 100, 0, 0, 1);
        };
        const float fallback = distanceDb();
        VnEmitterHandle reference = 0, state = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reference));
        VnEmitterCreateDescV1 create{sizeof(create), VN_EMITTER_CREATE_STATE_ONLY};
        ASSERT_EQ(VN_OK, Vn_CreateEmitterEx(&create, &state));
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, Vec3(99, 0, 0)));
        ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(reference));
        const auto freeCommands = engine->commands.FreeSlots();
        EXPECT_EQ(VN_OK, Vn_SetListenerDistanceReference(reference));
        EXPECT_EQ(VN_STALE_HANDLE, Vn_SetListenerDistanceReference(UINT64_MAX));
        EXPECT_EQ(VN_STALE_HANDLE, Vn_SetListenerDistanceReference(engine->emitters.DefaultState()));
        EXPECT_EQ(VN_CAPABILITY_UNAVAILABLE, Vn_SetListenerDistanceReference(state));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetEmitterPosition(reference, Vec3(kNan, 0, 0)));
        EXPECT_FLOAT_EQ(0.0f, distanceDb());
        EXPECT_EQ(freeCommands, engine->commands.FreeSlots());
        ASSERT_EQ(VN_OK, Vn_DestroyEmitter(state));
        EXPECT_FLOAT_EQ(0.0f, distanceDb());
        // Bypass the public cleanup to prove even a same-handle probe checks
        // native liveness, and the resolver defensively falls back.
        ASSERT_TRUE(engine->emitters.Destroy(reference));
        EXPECT_EQ(VN_STALE_HANDLE, Vn_SetListenerDistanceReference(reference));
        EXPECT_FLOAT_EQ(fallback, distanceDb());
        VnEmitterHandle replacement = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
        EXPECT_EQ(static_cast<std::uint32_t>(reference), static_cast<std::uint32_t>(replacement));
        EXPECT_NE(reference, replacement);
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(replacement, Vec3(99, 0, 0)));
        EXPECT_FLOAT_EQ(fallback, distanceDb());
        ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(replacement));
        EXPECT_EQ(VN_STALE_HANDLE, Vn_DestroyEmitter(reference));
        EXPECT_FLOAT_EQ(0.0f, distanceDb());
        ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(0));
        EXPECT_FLOAT_EQ(fallback, distanceDb());
        ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(replacement));
    }
    VnEmitterHandle selectedBeforeRestart = 0;
    {
        EngineSession next(MakeNullConfig(1));
        ASSERT_EQ(VN_OK, next.InitResult());
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &profile));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&selectedBeforeRestart));
        ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(selectedBeforeRestart));
    }
    EngineSession next(MakeNullConfig(1));
    ASSERT_EQ(VN_OK, next.InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &profile));
    VnEmitterHandle replacement = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&replacement));
    EXPECT_EQ(selectedBeforeRestart, replacement);
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(replacement, Vec3(99, 0, 0)));
    EXPECT_LT(next.Engine()->spatial.AttenuationVolumeDb(
        next.Engine()->emitters, harkentest::ObjectId(9), 100, 0, 0, 1), -30.0f);
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

TEST(SpatialApi, AttenuationVintagesReadOnlyTheirCompleteFields) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    for (const auto size : {608u, 612u, 796u}) {
        auto desc = MakeAttenuationDesc(1, 20);
        desc.structSize = size;
        desc.proximityFrequency = 400;
        desc.highpassCurve.enabled = 1;
        desc.highpassCurve.pointCount = 1;
        desc.highpassCurve.points[0] = {0, 1000};
        auto bytes = std::make_unique<std::uint32_t[]>(size / sizeof(std::uint32_t));
        std::memcpy(bytes.get(), &desc, size);
        const auto* prefix = reinterpret_cast<const VnAttenuationDesc*>(bytes.get());
        const auto id = harkentest::ObjectId(size);
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(id, prefix)) << size;
        ASSERT_EQ(VN_OK, Vn_ReplaceAttenuation(id, prefix)) << size;
        const auto* profile = session.Engine()->spatial.FindProfile(id);
        ASSERT_NE(nullptr, profile);
        EXPECT_FLOAT_EQ(size == 608 ? 250 : 400, profile->proximityFrequency);
        EXPECT_EQ(size == 796, profile->highpassOverride);
    }
    for (const auto size : {607u, 609u, 611u, 613u, 795u, 797u}) {
        auto desc = MakeAttenuationDesc(1, 20);
        desc.structSize = size;
        EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterAttenuation(harkentest::ObjectId(1), &desc));
        EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceAttenuation(harkentest::ObjectId(612), &desc));
    }
}

TEST(SpatialApi, HighpassOverrideValidatesNonnegativeFiniteHz) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto valid = MakeAttenuationDesc(1, 20);
    valid.highpassCurve.enabled = 1;
    valid.highpassCurve.pointCount = 2;
    valid.highpassCurve.points[0] = {1, 0};
    valid.highpassCurve.points[1] = {20, 1000};
    valid.highpassCurve.handles[0] = harkentest::EndpointHandles(
        valid.highpassCurve.points[0], valid.highpassCurve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &valid));
    for (std::uint32_t fault = 0; fault < 7; ++fault) {
        auto desc = valid;
        switch (fault) {
        case 0: desc.highpassCurve.enabled = 2; break;
        case 1: desc.highpassCurve.pointCount = 0; break;
        case 2: desc.highpassCurve.pointCount = 9; break;
        case 3: desc.highpassCurve.points[0].y = -1; break;
        case 4: desc.highpassCurve.points[1].y = kInf; break;
        case 5: desc.highpassCurve.handles[0].h1y = -1; break;
        case 6: desc.highpassCurve.handles[6].h1y = -0.0f; break;
        }
        EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceAttenuation(harkentest::ObjectId(1), &desc)) << fault;
        const auto* profile = session.Engine()->spatial.FindProfile(harkentest::ObjectId(1));
        ASSERT_NE(nullptr, profile);
        EXPECT_TRUE(profile->highpassOverride);
    }
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

TEST(SpatialApi, DefinitionGrowthAndDuplicateIdRefusal) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnAttenuationDesc desc = MakeAttenuationDesc(1.0f, 10.0f);

    for (std::uint32_t i = 0; i < 65u; ++i) {
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(100 + i), &desc))
            << "profile " << i;
    }
    // A live id never silently replaces (PCM registration rule).
    EXPECT_EQ(VN_IN_USE, Vn_RegisterAttenuation(harkentest::ObjectId(100), &desc));
    EXPECT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9999), &desc));
    EXPECT_NEAR(-6.0206f, session.Engine()->spatial.AttenuationVolumeDb(
        session.Engine()->emitters, harkentest::ObjectId(164), 0, 0, -5.5f, 1), 0.0001f);
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
    overrideDesc.highpassCurve.enabled = 1;
    overrideDesc.highpassCurve.pointCount = 2;
    overrideDesc.highpassCurve.points[0] = {1, 0};
    overrideDesc.highpassCurve.points[1] = {20, 1000};
    overrideDesc.highpassCurve.handles[0] = harkentest::EndpointHandles(
        overrideDesc.highpassCurve.points[0], overrideDesc.highpassCurve.points[1]);
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
                                  harken::kTargetVoiceHighpassCutoff,
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
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(8.0f, 0.0f, -6.0f)));
    ASSERT_EQ(VN_OK, Vn_SetEmitterOrientation(emitter, Vec3(1.0f, 0.0f, 0.0f)));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 8.0f, 0.0f, -6.0f);
    play.emitterHandle = emitter;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(12)));
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    const float pan = engine->params.ResolvedValue(render, harken::kTargetVoicePan);
    const float volume =
        engine->params.ResolvedValue(render, harken::kTargetVoiceVolume);
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 4.0f));
    ASSERT_EQ(VN_OK, Vn_SetEmitterDistanceScale(emitter, 200.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(pan, engine->params.ResolvedValue(render,
                                                      harken::kTargetVoicePan));
    EXPECT_FLOAT_EQ(volume,
                    engine->params.ResolvedValue(render,
                                                 harken::kTargetVoiceVolume));
}

TEST(SpatialApi, EmitterScaleCombinesWithVoiceScaleAcrossWideFiniteGeometry) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto samples = harkentest::MakeDC(48000, 0.25f);
    const auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    auto profile = MakeAttenuationDesc(1.0f, 20.0f);
    profile.airAbsorptionNearHz = 18000.0f;
    profile.airAbsorptionFarHz = 2000.0f;
    profile.proximityRadius = 8.0f;
    profile.proximityMaxBoostDb = 6.0f;
    profile.widthCurve.enabled = 1;
    profile.widthCurve.pointCount = 2;
    profile.widthCurve.points[0] = {1.0f, 0.9f};
    profile.widthCurve.points[1] = {20.0f, 0.1f};
    profile.widthCurve.handles[0] = harkentest::EndpointHandles(
        profile.widthCurve.points[0], profile.widthCurve.points[1]);
    profile.highpassCurve.enabled = 1;
    profile.highpassCurve.pointCount = 2;
    profile.highpassCurve.points[0] = {1, 0};
    profile.highpassCurve.points[1] = {20, 1000};
    profile.highpassCurve.handles[0] = harkentest::EndpointHandles(
        profile.highpassCurve.points[0], profile.highpassCurve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &profile));
    VnEmitterHandle emitter = 0, distanceReference = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&distanceReference));
    auto play = MakePlayDesc3D(harkentest::ObjectId(1), 4.0f, 0.0f, 0.0f);
    VnVoiceHandle reference = 0, scaled = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &reference));
    play.emitterHandle = emitter;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &scaled));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(reference, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(scaled, harkentest::ObjectId(9)));
    const auto referenceRender = harkentest::RenderHandle(engine, reference);
    const auto scaledRender = harkentest::RenderHandle(engine, scaled);
    const auto targets = {harken::kTargetVoiceVolume, harken::kTargetVoiceLowpassCutoff,
                              harken::kTargetVoiceWidth, harken::kTargetVoiceLowShelfGainDb};
    const float max = std::numeric_limits<float>::max();
    const float tiny = std::numeric_limits<float>::denorm_min();
    struct Case { float sourceX, listenerX, emitterScale, voiceScale, effectiveDistance; };
    const Case cases[] = {
        {16.0f, 0.0f, 2.0f, 2.0f, 4.0f},
        {max, -max, max, 0.5f, 4.0f},
        {max, -max, max, 100.0f, 0.02f},
        {tiny * 4.0f, 0.0f, tiny, 1.0f, 4.0f},
        {8.0f, 0.0f, tiny, 1.0f, max},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.emitterScale);
        ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(0));
        ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0)));
        ASSERT_EQ(VN_OK, Vn_SetVoicePosition(reference, Vec3(test.effectiveDistance, 0, 0)));
        ASSERT_EQ(VN_OK, Vn_Update());
        std::vector<float> expected;
        for (const auto target : targets)
            expected.push_back(engine->params.ResolvedValue(referenceRender, target));
        for (const bool independentReference : {false, true}) {
            SCOPED_TRACE(independentReference);
            ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(emitter, Vec3(test.sourceX, 0, 0)));
            ASSERT_EQ(VN_OK, Vn_SetEmitterDistanceScale(emitter, test.emitterScale));
            ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(scaled, test.voiceScale));
            ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(distanceReference, Vec3(test.listenerX, 0, 0)));
            ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(independentReference ? distanceReference : 0));
            ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(independentReference ? 0 : test.listenerX, 0, 0),
                                            Vec3(0, 0, -1), Vec3(0, 1, 0)));
            ASSERT_EQ(VN_OK, Vn_Update());
            std::size_t index = 0;
            for (const auto target : targets) {
                const auto actual = engine->params.ResolvedValue(scaledRender, target);
                EXPECT_TRUE(std::isfinite(actual));
                EXPECT_NEAR(expected[index++], actual, 1e-4f) << target;
            }
            EXPECT_FLOAT_EQ(1.0f, engine->params.ResolvedValue(scaledRender, harken::kTargetVoicePan));
            EXPECT_NEAR(expected.front(), engine->spatial.AttenuationVolumeDb(
                engine->emitters, harkentest::ObjectId(9), test.sourceX, 0, 0, test.voiceScale, 0,
                test.emitterScale), 1e-4f);
        }
    }
}

TEST(SpatialApi, DistanceReferenceControlsDistanceTargetsButListenerKeepsPanAndCone) {
    EngineSession session(MakeNullConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto samples = harkentest::MakeDC(48000, 0.25f);
    const auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    auto profile = MakeAttenuationDesc(1, 20);
    profile.airAbsorptionNearHz = 18000;
    profile.airAbsorptionFarHz = 2000;
    profile.proximityRadius = 8;
    profile.proximityMaxBoostDb = 6;
    profile.widthCurve.enabled = 1;
    profile.widthCurve.pointCount = 2;
    profile.widthCurve.points[0] = {1, 0.9f};
    profile.widthCurve.points[1] = {20, 0.1f};
    profile.widthCurve.handles[0] = harkentest::EndpointHandles(
        profile.widthCurve.points[0], profile.widthCurve.points[1]);
    profile.highpassCurve.enabled = 1;
    profile.highpassCurve.pointCount = 2;
    profile.highpassCurve.points[0] = {1, 0};
    profile.highpassCurve.points[1] = {20, 1000};
    profile.highpassCurve.handles[0] = harkentest::EndpointHandles(
        profile.highpassCurve.points[0], profile.highpassCurve.points[1]);
    profile.coneEnabled = 1;
    profile.coneInnerDeg = 30;
    profile.coneOuterDeg = 60;
    profile.coneOuterGainDb = -12;
    profile.coneOuterLowpassHz = 20000;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &profile));
    auto play = MakePlayDesc3D(harkentest::ObjectId(1), 0, 0, -1);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOrientation(voice, Vec3(0, 0, 1)));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto render = harkentest::RenderHandle(engine, voice);
    const auto targets = {harken::kTargetVoiceVolume, harken::kTargetVoiceLowpassCutoff,
                          harken::kTargetVoiceWidth, harken::kTargetVoiceLowShelfGainDb,
                          harken::kTargetVoiceHighpassCutoff};
    std::vector<float> nearTargets;
    for (const auto target : targets) nearTargets.push_back(engine->params.ResolvedValue(render, target));
    ASSERT_EQ(VN_OK, Vn_SetVoicePosition(voice, Vec3(0, 0, -10)));
    VnEmitterHandle reference = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reference));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, Vec3(0, 0, -9)));
    ASSERT_EQ(VN_OK, Vn_SetEmitterDistanceScale(reference, 200));
    ASSERT_EQ(VN_OK, Vn_SetEmitterOrientation(reference, Vec3(0, 1, 0)));
    ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(reference));
    ASSERT_EQ(VN_OK, Vn_Update());
    std::size_t index = 0;
    for (const auto target : targets)
        EXPECT_NEAR(nearTargets[index++], engine->params.ResolvedValue(render, target), 1e-4f) << target;
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(render, harken::kTargetVoicePan));
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0, 0, -20), Vec3(0, 0, -1), Vec3(0, 1, 0)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(nearTargets.front() - 12, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume), 1e-4f);
    EXPECT_NEAR(nearTargets.front(), engine->spatial.AttenuationVolumeDb(
        engine->emitters, harkentest::ObjectId(9), 0, 0, -10, 1), 1e-4f);
    index = 1;
    for (const auto target : {harken::kTargetVoiceLowpassCutoff, harken::kTargetVoiceWidth,
                              harken::kTargetVoiceLowShelfGainDb, harken::kTargetVoiceHighpassCutoff})
        EXPECT_NEAR(nearTargets[index++], engine->params.ResolvedValue(render, target), 1e-4f) << target;
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(10, 0, -10), Vec3(0, 0, -1), Vec3(0, 1, 0)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-1.0f, engine->params.ResolvedValue(render, harken::kTargetVoicePan));
    ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, Vec3(0, 0, 0)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_LT(engine->params.ResolvedValue(render, harken::kTargetVoiceVolume), nearTargets.front() - 12);
    EXPECT_LT(engine->params.ResolvedValue(render, harken::kTargetVoiceLowpassCutoff), nearTargets[1]);
    EXPECT_LT(engine->params.ResolvedValue(render, harken::kTargetVoiceWidth), nearTargets[2]);
    EXPECT_LT(engine->params.ResolvedValue(render, harken::kTargetVoiceLowShelfGainDb), nearTargets[3]);
    EXPECT_GT(engine->params.ResolvedValue(render, harken::kTargetVoiceHighpassCutoff), nearTargets[4]);
    EXPECT_FLOAT_EQ(-1.0f, engine->params.ResolvedValue(render, harken::kTargetVoicePan));
}

} // namespace
