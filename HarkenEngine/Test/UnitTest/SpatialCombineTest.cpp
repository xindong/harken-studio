// Combine-model coverage for the spatial contributions: the attenuation
// slot composes with the API-direct slot under each target's declared
// domain, the new bounded targets clamp, and an unbound 3D voice pans
// without attenuating.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/param/ParamSystem.h"
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
// Slot 0, generation 1 — the shape the pool mints for its first voice.
constexpr VnVoiceHandle kVoice0 = (1ull << 32) | 0;

TEST(SpatialCombine, AttenuationSlotAddsWithApiDirectInDb) {
    harken::EngineConfig config;
    harken::ParamSystem params(config);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceVolume, -6.0f);
    const float resolved =
        params.SetApiDirect(kVoice0, harken::kTargetVoiceVolume, -3.0f);
    // AddDb: base 0 + API direct (-3) + attenuation (-6).
    EXPECT_FLOAT_EQ(-9.0f, resolved);
    EXPECT_FLOAT_EQ(-9.0f, params.ResolvedValue(
                               kVoice0, harken::kTargetVoiceVolume));

    // Clearing the attenuation slot hands the target back to the
    // API-direct contribution alone.
    params.ClearAttenuationSlot(kVoice0, harken::kTargetVoiceVolume);
    EXPECT_FLOAT_EQ(-3.0f, params.ResolvedValue(
                               kVoice0, harken::kTargetVoiceVolume));
}

TEST(SpatialCombine, SpatialVolumeRampJoinsSameTickMaxAggregation) {
    harken::EngineConfig config;
    harken::ParamSystem params(config);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    EXPECT_FLOAT_EQ(
        -3.0f,
        params.SetApiDirect(kVoice0, harken::kTargetVoiceVolume, -3.0f, 0));
    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceVolume, -6.0f,
                              10);

    harken::ParamCommit observed{};
    std::uint32_t commitCount = 0;
    params.FlushCommits([&](const harken::ParamCommit& commit) {
        observed = commit;
        ++commitCount;
        return harken::FlushOutcome::Committed;
    });

    ASSERT_EQ(1u, commitCount);
    EXPECT_EQ(kVoice0, observed.handle);
    EXPECT_EQ(harken::kTargetVoiceVolume, observed.targetId);
    EXPECT_FLOAT_EQ(-9.0f, observed.resolved);
    EXPECT_EQ(10u, observed.rampMs);
}

TEST(SpatialCombine, PanAddsAndClampsToUnitSpan) {
    harken::EngineConfig config;
    harken::ParamSystem params(config);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    params.SetAttenuationSlot(kVoice0, harken::kTargetVoicePan, 0.8f);
    float resolved =
        params.SetApiDirect(kVoice0, harken::kTargetVoicePan, 0.5f);
    // Add domain, then the bounded-target clamp: 0.8 + 0.5 -> 1.0.
    EXPECT_FLOAT_EQ(1.0f, resolved);

    resolved = params.SetApiDirect(kVoice0, harken::kTargetVoicePan, -0.5f);
    EXPECT_NEAR(0.3f, resolved, 1e-6f);
}

TEST(SpatialCombine, WidthTakesTheWidestContribution) {
    harken::EngineConfig config;
    harken::ParamSystem params(config);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceWidth, 0.3f);
    float resolved =
        params.SetApiDirect(kVoice0, harken::kTargetVoiceWidth, 0.6f);
    EXPECT_FLOAT_EQ(0.6f, resolved); // max(base 0, 0.6, 0.3)

    resolved = params.SetApiDirect(kVoice0, harken::kTargetVoiceWidth, 0.1f);
    EXPECT_FLOAT_EQ(0.3f, resolved); // the attenuation slot now wins

    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceWidth, 2.5f);
    resolved = params.ResolvedValue(kVoice0, harken::kTargetVoiceWidth);
    EXPECT_FLOAT_EQ(1.0f, resolved); // clamped to the unit span
}

TEST(SpatialCombine, UnboundSpatializedVoicePansButNeverAttenuates) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    // Hard right and very far away: azimuth is geometry (applies), but
    // distance attenuation is policy (must not apply implicitly).
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 1000.0f, 0.0f, 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_FLOAT_EQ(1.0f, engine->params.ResolvedValue(harkentest::RenderHandle(
        engine, handle), harken::kTargetVoicePan));
    EXPECT_FLOAT_EQ(0.0f, engine->params.ResolvedValue(harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceVolume));

    Capture captured;
    harkentest::PumpAndCapture(engine, 2, captured);
    const float l = captured.left[kBlockFrames + kBlockFrames / 2];
    const float r = captured.right[kBlockFrames + kBlockFrames / 2];
    ASSERT_GT(std::fabs(r), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(0.0f, l, 1e-6f);   // pan applied
    EXPECT_NEAR(0.5f * std::sqrt(2.0f), r, 1e-5f); // hard-right gain, no distance loss
}

TEST(SpatialCombine, InitialAttenuationSnapsBeforeFirstFrame) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(3), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -51.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(3)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 2.0f));
    ASSERT_EQ(VN_OK, Vn_Update());

    Capture captured;
    harkentest::PumpAndCapture(engine, 1, captured);
    ASSERT_EQ(kBlockFrames, captured.left.size());
    // World distance 51 scaled by 2 gives 25.5; the authored linear profile
    // [1, 101] therefore contributes gain 1 - (25.5 - 1) / (101 - 1) = 0.755.
    const float effectiveDistance = 51.0f / 2.0f;
    const float linearGain =
        1.0f - (effectiveDistance - desc.minDistance) /
                    (desc.maxDistance - desc.minDistance);
    const float expected = 0.5f * linearGain;
    EXPECT_NEAR(expected, captured.left.front(), 1e-4f);
    EXPECT_NEAR(expected, captured.right.front(), 1e-4f);
}

TEST(SpatialCombine, LiveDistanceScaleUsesExistingSpatialVolumeRamp) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(3), &desc));
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -51.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(3)));
    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 2.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(harkentest::RenderHandle(engine, handle));
    ASSERT_NE(nullptr, ctx);
    EXPECT_EQ(0u, ctx->gain.remainingFrames);

    ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 1.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_EQ(224u, ctx->gain.remainingFrames);
}

TEST(SpatialCombine, BoundAttenuationComposesWithTheVolumeSetter) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(3), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -51.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(3)));
    ASSERT_EQ(VN_OK, Vn_Update()); // attenuation slot: 20*log10(0.5)

    // The setter writes the API-direct slot; the resolved value carries
    // both contributions in the dB domain.
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -6.0f, 0));
    const float expectedDb = -6.0f + 20.0f * std::log10(0.5f);
    EXPECT_NEAR(expectedDb,
                engine->params.ResolvedValue(harkentest::RenderHandle(engine,
                    handle),
                                             harken::kTargetVoiceVolume),
                1e-4f);

    Capture captured;
    harkentest::PumpAndCapture(engine, 2, captured);
    const float l = captured.left[kBlockFrames + kBlockFrames / 2];
    ASSERT_GT(std::fabs(l), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    const float expected =
        0.5f * std::pow(10.0f, expectedDb / 20.0f);
    EXPECT_NEAR(expected, l, 1e-4f);
}

} // namespace
