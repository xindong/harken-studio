// Voice parameter setter contracts: stale handles are benign no-ops with
// the counter, a full command queue refuses with zero side effects (the
// combine slot is untouched and the identical retry succeeds), non-finite
// or out-of-domain values are refused up front, repeated API-direct sets
// replace (never accumulate) their slot, and position/pitch commands land
// in the render context at the block boundary.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/CommandQueue.h"
#include "harken/dsp/GainRamp.h"
#include "harken/param/BindingTable.h"
#include "harken/param/ParamTargets.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kSampleRate = 48000;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

VnResult RegisterConstVoiceVolumeBinding(harken::Engine* engine,
                                         harken::BindingId id,
                                         VnVariableId variable,
                                         float db) {
    const harken::CurvePoint pts[] = {{0.0f, db}, {1.0f, db}};
    const harken::CurveHandlePair handles[] = {{0.0f, db, 1.0f, db}};
    return harken::RegisterBinding(*engine, id, variable,
                                  harken::kTargetScopeVoice,
                                  harken::kTargetVoiceVolume, pts, handles,
                                  2, 0);
}

void ExpectPitchSnapAfterBlock(const harken::VoiceRenderContext& context,
                               double targetRatio) {
    EXPECT_DOUBLE_EQ(targetRatio, context.resampler.TargetRatio());
    EXPECT_EQ(0u, context.resampler.RampFramesRemaining());
    EXPECT_DOUBLE_EQ(targetRatio, context.resampler.CurrentRatio());
}

class VoiceSetter : public ::testing::Test {
protected:
    void SetUp() override {
        VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
        session = std::make_unique<EngineSession>(config);
        ASSERT_EQ(VN_OK, session->InitResult());
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harkentest::InstallSmoothDefinitions(
                      session->Engine(), {harkentest::ObjectId(77), harkentest::ObjectId(81), harkentest::ObjectId(82)}, 0.0f, 1.0f).status);

        samples = harkentest::MakeDC(48000, 1.0f);
        desc = MonoDesc(samples, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    }

    void Pump(std::uint32_t blocks) {
        session->Engine()->nullBackend.Pump(blocks);
    }

    std::unique_ptr<EngineSession> session;
    std::vector<float> samples;
    VnPCMDesc desc{};
    VnVoiceHandle handle = 0;
};

TEST_F(VoiceSetter, StaleHandleIsBenignNoOpOnEverySetter) {
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    Pump(1); // the stop applies and the slot is released

    const std::uint64_t staleBefore =
        session->Engine()->voices.StaleHandleCount();
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceVolume(handle, -3.0f, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoicePitch(handle, 1.5f));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetVoicePosition(handle, VnVec3{1.0f, 2.0f, 3.0f}));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceDistanceScale(handle, 2.0f));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetVoiceSpatializationMix(handle, 0.5f));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, 1000.0f,
                                0.707f, 1, 0));
    EXPECT_EQ(staleBefore + 6,
              session->Engine()->voices.StaleHandleCount());
}

TEST_F(VoiceSetter, InvalidValuesAreRefusedBeforeAnyStateChanges) {
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(handle, kNaN, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceVolume(
                  handle, std::numeric_limits<float>::infinity(), 0));

    // Pitch must be finite, positive, and at most 64: the value enters
    // the combine model as cents, where zero and negatives are undefined.
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoicePitch(handle, 0.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoicePitch(handle, -1.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoicePitch(handle, 65.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoicePitch(handle, kNaN));

    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoicePosition(handle, VnVec3{kNaN, 0.0f, 0.0f}));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoicePosition(handle, VnVec3{0.0f, kNaN, 0.0f}));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoicePosition(handle, VnVec3{0.0f, 0.0f, kNaN}));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceDistanceScale(handle, 0.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceDistanceScale(handle, 0.009f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceDistanceScale(handle, 100.001f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceDistanceScale(handle, kNaN));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceDistanceScale(handle, kInf));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceDistanceScale(handle, -kInf));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSpatializationMix(handle, -0.001f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSpatializationMix(handle, 1.001f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSpatializationMix(handle, kNaN));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSpatializationMix(handle, kInf));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSpatializationMix(handle, -kInf));

    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceFilter(handle, 2, 1000.0f, 0.707f, 1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, 1000.0f,
                                0.707f, 2, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, kNaN,
                                0.707f, 1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceFilter(handle, harken::kFilterKindHighpass, 1000.0f,
                                kNaN, 1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, 1000.0f,
                                0.0f, 1, 0));

    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(session->Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session->Engine(), handle), &spatial));
    EXPECT_FLOAT_EQ(1.0f, spatial.distanceScale);
    EXPECT_FLOAT_EQ(1.0f, spatial.spatializationMix);
    EXPECT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 0.01f));
    EXPECT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 100.0f));
    ASSERT_TRUE(session->Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session->Engine(), handle), &spatial));
    EXPECT_FLOAT_EQ(100.0f, spatial.distanceScale);
    EXPECT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 0.0f));
    EXPECT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 1.0f));
    ASSERT_TRUE(session->Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session->Engine(), handle), &spatial));
    EXPECT_FLOAT_EQ(1.0f, spatial.spatializationMix);

    // Nothing above touched the combine state: still the authored base.
    EXPECT_FLOAT_EQ(-6.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(harken::kLowpassOpenHz,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceLowpassCutoff));
    EXPECT_FLOAT_EQ(harken::kHighpassClosedHz,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceHighpassCutoff));
}

TEST_F(VoiceSetter, SpatializationMixIsStoredButIgnoredByA2DVoice) {
    Pump(1);
    harkentest::Capture before;
    harkentest::PumpAndCapture(session->Engine(), 1, before);
    ASSERT_FALSE(before.left.empty());
    ASSERT_FALSE(before.right.empty());

    ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 0.0f));
    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(session->Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session->Engine(), handle), &spatial));
    EXPECT_FLOAT_EQ(0.0f, spatial.spatializationMix);

    harkentest::Capture after;
    harkentest::PumpAndCapture(session->Engine(), 2, after);
    ASSERT_FALSE(after.left.empty());
    ASSERT_FALSE(after.right.empty());
    for (const float sample : after.left) {
        EXPECT_FLOAT_EQ(before.left.back(), sample);
    }
    for (const float sample : after.right) {
        EXPECT_FLOAT_EQ(before.right.back(), sample);
    }
}

TEST_F(VoiceSetter, BackpressureRefusalHasZeroSideEffects) {
    // A dedicated tiny queue: depth 4, with the play command already
    // drained so exactly four pushes fill it.
    session.reset();
    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.commandQueueDepth = 4;
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Pump(1); // drain the play command; the voice renders

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
    }
    // Queue full: the setter refuses and the API-direct slot stays
    // untouched — resolved is still the authored base.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoiceVolume(handle, 2.0f, 0));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoiceDistanceScale(handle, 2.0f));
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetVoiceSpatializationMix(handle, 0.0f));
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, 800.0f,
                                0.707f, 1, 0));
    harken::Spatializer::SpatialVoiceState refused;
    ASSERT_TRUE(session->Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session->Engine(), handle), &refused));
    EXPECT_FLOAT_EQ(1.0f, refused.distanceScale);
    EXPECT_FLOAT_EQ(1.0f, refused.spatializationMix);
    EXPECT_FLOAT_EQ(-6.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(harken::kLowpassOpenHz,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceLowpassCutoff));

    // After a drain the identical call succeeds and contributes exactly
    // once: base -6 dB plus one +2 dB API-direct write.
    Pump(1);
    EXPECT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 2.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetVoiceDistanceScale(handle, 2.0f));
    EXPECT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(handle, 0.0f));
    EXPECT_EQ(VN_OK,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, 800.0f,
                                0.707f, 1, 0));
    harken::Spatializer::SpatialVoiceState accepted;
    ASSERT_TRUE(session->Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session->Engine(), handle), &accepted));
    EXPECT_FLOAT_EQ(2.0f, accepted.distanceScale);
    EXPECT_FLOAT_EQ(0.0f, accepted.spatializationMix);
    EXPECT_FLOAT_EQ(-4.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(800.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceLowpassCutoff));
}

TEST_F(VoiceSetter, RepeatedApiDirectSetsReplaceTheirSlot) {
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 2.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 4.0f, 0));
    // The API-direct category holds one value, not a history: -6 + 4.
    EXPECT_FLOAT_EQ(-2.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoiceVolume));
}

TEST_F(VoiceSetter, PositionIsStoredOnTheVoiceAtTheBlockBoundary) {
    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(handle, VnVec3{1.5f, -2.5f, 3.75f}));
    harken::VoiceRenderContext* ctx =
        session->Engine()->voices.Resolve(harkentest::RenderHandle(
            session->Engine(), handle));
    ASSERT_NE(nullptr, ctx);
    // Not yet applied: commands take effect when the block drains them.
    EXPECT_EQ(0.0f, ctx->positionX);
    Pump(1);
    EXPECT_EQ(1.5f, ctx->positionX);
    EXPECT_EQ(-2.5f, ctx->positionY);
    EXPECT_EQ(3.75f, ctx->positionZ);
}

TEST_F(VoiceSetter, BackpressureAlsoRefusesPitchAndPositionSideEffectFree) {
    // Same tiny-queue arrangement as the volume case, extended to the
    // other two setters.
    session.reset();
    VnConfig config = MakeNullConfig(4, kBlockFrames, 48000);
    config.commandQueueDepth = 4;
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Pump(1);

    // Establish a known applied position so the refused update below has
    // an observable "old" value to preserve.
    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(handle, VnVec3{1.0f, 2.0f, 3.0f}));
    Pump(1);
    harken::VoiceRenderContext* ctx =
        session->Engine()->voices.Resolve(harkentest::RenderHandle(
            session->Engine(), handle));
    ASSERT_NE(nullptr, ctx);
    ASSERT_EQ(1.0f, ctx->positionX);

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
    }
    // Full queue: pitch refuses with the combine slot untouched (still
    // the 0-cent base), position refuses with no command in flight.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoicePitch(handle, 2.0f));
    EXPECT_FLOAT_EQ(0.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoicePitch));
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetVoicePosition(handle, VnVec3{9.0f, 9.0f, 9.0f}));

    Pump(1); // drains only the four bus commands
    EXPECT_EQ(1.0, ctx->resampler.CurrentRatio()); // pitch unchanged
    EXPECT_EQ(1.0f, ctx->positionX);      // position still the old value
    EXPECT_EQ(2.0f, ctx->positionY);
    EXPECT_EQ(3.0f, ctx->positionZ);

    // The identical retries succeed and contribute exactly once.
    EXPECT_EQ(VN_OK, Vn_SetVoicePitch(handle, 2.0f));
    EXPECT_NEAR(1200.0f,
                session->Engine()->params.ResolvedValue(harkentest::RenderHandle(
                    session->Engine(), handle), harken::kTargetVoicePitch),
                1e-3f);
    EXPECT_EQ(VN_OK,
              Vn_SetVoicePosition(handle, VnVec3{9.0f, 9.0f, 9.0f}));
    Pump(1);
    ExpectPitchSnapAfterBlock(*ctx, 2.0);
    EXPECT_EQ(9.0f, ctx->positionX);
    EXPECT_EQ(9.0f, ctx->positionY);
    EXPECT_EQ(9.0f, ctx->positionZ);
}

TEST_F(VoiceSetter,
       VariableSliceClaimRefusesEveryDirectTargetBeforeMutation) {
    Pump(1);
    harken::Engine* engine = session->Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    const harken::VariableSliceRef owner{19u, 7u};
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const auto expectExactRefusal =
        [&](std::uint32_t targetId, const auto& rejected,
            const auto& unrelated, const auto& retry) {
        const float before = engine->params.ResolvedValue(render, targetId);
        ASSERT_TRUE(engine->params.TryClaim(
            render, targetId, owner,
            engine->params.TargetRevision(render, targetId)));
        const std::uint64_t commandHead = engine->commands.PublishedHead();
        EXPECT_EQ(VN_BACKPRESSURE, rejected());
        EXPECT_EQ(commandHead, engine->commands.PublishedHead());
        EXPECT_FLOAT_EQ(before,
                        engine->params.ResolvedValue(render, targetId));
        EXPECT_TRUE(engine->params.ForeignClaim(render, targetId));
        EXPECT_EQ(VN_OK, unrelated());
        EXPECT_EQ(commandHead + 1u, engine->commands.PublishedHead());
        EXPECT_FLOAT_EQ(before,
                        engine->params.ResolvedValue(render, targetId));
        engine->params.ReleaseClaim(render, targetId, owner);
        EXPECT_FALSE(engine->params.ForeignClaim(render, targetId));
        EXPECT_EQ(VN_OK, retry());
        EXPECT_EQ(commandHead + 2u, engine->commands.PublishedHead());
        Pump(1);
    };

    expectExactRefusal(
        harken::kTargetVoiceVolume,
        [&]() { return Vn_SetVoiceVolume(handle, 2.0f, 0); },
        [&]() { return Vn_SetVoicePitch(handle, 1.25f); },
        [&]() { return Vn_SetVoiceVolume(handle, 2.0f, 0); });
    expectExactRefusal(
        harken::kTargetVoicePitch,
        [&]() { return Vn_SetVoicePitch(handle, 2.0f); },
        [&]() { return Vn_SetVoicePan(handle, 0.1f); },
        [&]() { return Vn_SetVoicePitch(handle, 2.0f); });
    expectExactRefusal(
        harken::kTargetVoicePan,
        [&]() { return Vn_SetVoicePan(handle, 0.5f); },
        [&]() { return Vn_SetVoiceWidth(handle, 0.2f); },
        [&]() { return Vn_SetVoicePan(handle, 0.5f); });
    expectExactRefusal(
        harken::kTargetVoiceWidth,
        [&]() { return Vn_SetVoiceWidth(handle, 0.75f); },
        [&]() { return Vn_SetVoiceVolume(handle, 1.0f, 0); },
        [&]() { return Vn_SetVoiceWidth(handle, 0.75f); });
    EXPECT_EQ(backpressure + 4u, engine->commands.BackpressureCount());
    EXPECT_FLOAT_EQ(-5.0f, engine->params.ResolvedValue(
                               render, harken::kTargetVoiceVolume));
    EXPECT_NEAR(1200.0f, engine->params.ResolvedValue(
                             render, harken::kTargetVoicePitch),
                1e-3f);
    EXPECT_FLOAT_EQ(0.5f, engine->params.ResolvedValue(
                              render, harken::kTargetVoicePan));
    EXPECT_FLOAT_EQ(0.75f, engine->params.ResolvedValue(
                               render, harken::kTargetVoiceWidth));

    const harken::VoiceRenderContext* ctx = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    EXPECT_FLOAT_EQ(std::pow(10.0f, -5.0f / 20.0f), ctx->gain.target);
    EXPECT_DOUBLE_EQ(2.0, ctx->resampler.TargetRatio());
    EXPECT_FLOAT_EQ(0.5f, ctx->pan);
    EXPECT_FLOAT_EQ(0.75f, ctx->width);
}

TEST_F(VoiceSetter, VariableDrivenPitchClampsToTheWireSpan) {
    // A binding can resolve pitch far beyond the setter's input ceiling:
    // +9600 cents is ratio 256. The wire boundary must clamp what reaches
    // the resampler to 64 while the combine domain keeps the true value.
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, 9600.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(session->Engine()->params.BindVoiceTarget(harkentest::ObjectId(77),
        harkentest::RenderHandle(session->Engine(), handle),
        harken::kTargetVoicePitch, curve));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(77), 1.0f));
    harken::VoiceRenderContext* ctx =
        session->Engine()->voices.Resolve(harkentest::RenderHandle(
            session->Engine(), handle));
    ASSERT_NE(nullptr, ctx);
    for (std::uint32_t step = 0;
         step < 32 && ctx->resampler.TargetRatio() != 64.0; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, session->Engine()->variables.PendingCount());
    EXPECT_DOUBLE_EQ(64.0, ctx->resampler.TargetRatio());
    EXPECT_EQ(0u, ctx->resampler.RampFramesRemaining());
    for (std::uint32_t step = 0;
         step < 32 && ctx->resampler.CurrentRatio() != 64.0; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        Pump(1);
    }
    EXPECT_DOUBLE_EQ(64.0, ctx->resampler.CurrentRatio());
    // The combine model is not clamped — only the wire value is.
    EXPECT_FLOAT_EQ(9600.0f,
                    session->Engine()->params.ResolvedValue(
                        harkentest::RenderHandle(session->Engine(), handle),
                        harken::kTargetVoicePitch));
}

TEST_F(VoiceSetter, NonFiniteResolvedCommitIsDroppedNotRetried) {
    // Two constant curves at float max are individually finite (legal
    // control points), but their category accumulation overflows the
    // volume reduction to +infinity — the wire gate must consume the
    // poisoned commit without retrying it forever.
    const float big = std::numeric_limits<float>::max();
    harken::Curve constBig;
    const harken::CurvePoint one[] = {{0.0f, big}};
    ASSERT_TRUE(constBig.SetPoints(one, nullptr, 1));
    ASSERT_TRUE(session->Engine()->params.BindVoiceTarget(harkentest::ObjectId(81),
        harkentest::RenderHandle(session->Engine(), handle),
        harken::kTargetVoiceVolume, constBig));
    ASSERT_TRUE(session->Engine()->params.BindVoiceTarget(harkentest::ObjectId(82),
        harkentest::RenderHandle(session->Engine(), handle),
        harken::kTargetVoiceVolume, constBig));

    harken::VoiceRenderContext* ctx =
        session->Engine()->voices.Resolve(harkentest::RenderHandle(
            session->Engine(), handle));
    ASSERT_NE(nullptr, ctx);
    Pump(1);
    const float baseGain = ctx->gain.current; // authored -6 dB, applied

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(81), 0.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(82), 0.0f));
    ASSERT_EQ(VN_OK, Vn_Update()); // resolved is +inf: dropped at the wire
    const std::uint64_t commitsAfterDrop =
        session->Engine()->params.CommitCount();
    Pump(1);
    EXPECT_EQ(baseGain, ctx->gain.current)
        << "a poisoned volume commit must never reach the gain stage";

    // Dropped means consumed: no dirty target lingers, so further pumps
    // do not re-present the poisoned value.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(commitsAfterDrop,
              session->Engine()->params.CommitCount());
    Pump(1);
    EXPECT_EQ(baseGain, ctx->gain.current);
}

TEST_F(VoiceSetter, FiniteOverflowingBindingVolumeIsDroppedNotRetried) {
    // A finite 800 dB curve is a legal control point, but the resolved
    // volume cannot convert to a finite linear gain. The wire gate must
    // drop it the same way as +inf: no command, no retry, old gain stays.
    harken::Curve const800;
    const harken::CurvePoint one[] = {{0.0f, 800.0f}};
    ASSERT_TRUE(const800.SetPoints(one, nullptr, 1));
    harken::Engine* engine = session->Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(81), render, harken::kTargetVoiceVolume, const800));

    harken::VoiceRenderContext* ctx = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    Pump(1);
    const float baseGain = ctx->gain.current;
    const std::uint64_t dropsBefore = engine->paramDropCount;
    const std::uint64_t invalidBefore =
        engine->voices.InvalidVolumeApplyCount();

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(81), 0.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    const float resolved =
        engine->params.ResolvedValue(render, harken::kTargetVoiceVolume);
    EXPECT_TRUE(std::isfinite(resolved));
    EXPECT_FALSE(harken::TryVolumeGainFromDb(resolved));
    EXPECT_EQ(dropsBefore + 1u, engine->paramDropCount);
    const std::uint64_t commitsAfterDrop = engine->params.CommitCount();
    Pump(1);
    EXPECT_EQ(baseGain, ctx->gain.current)
        << "a finite overflowing volume must drop before the gain stage";
    EXPECT_EQ(invalidBefore, engine->voices.InvalidVolumeApplyCount())
        << "the drop must not Stage and rely on the audio apply defense";

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(commitsAfterDrop, engine->params.CommitCount());
    EXPECT_EQ(dropsBefore + 1u, engine->paramDropCount);
    Pump(1);
    EXPECT_EQ(baseGain, ctx->gain.current);
    EXPECT_EQ(invalidBefore, engine->voices.InvalidVolumeApplyCount());
}

TEST_F(VoiceSetter, OverflowingVolumeTargetsAreRefusedWithZeroSideEffects) {
    float gain = 1.0f;
    EXPECT_TRUE(harken::TryVolumeGainFromDb(600.0f, &gain));
    EXPECT_TRUE(std::isfinite(gain));
    EXPECT_GT(gain, 0.0f);
    EXPECT_FALSE(harken::TryVolumeGainFromDb(800.0f, &gain));
    float underflow = 1.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(-800.0f, &underflow));
    EXPECT_TRUE(std::isfinite(underflow));
    EXPECT_GE(underflow, 0.0f);
    EXPECT_LT(underflow, 1.0e-30f);

    harken::Engine* engine = session->Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    const float before = engine->params.ResolvedValue(
        render, harken::kTargetVoiceVolume);
    const float lastCommitted =
        engine->params.LastCommitted(render, harken::kTargetVoiceVolume);
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(handle, 800.0f, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(handle, 800.0f, 0));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(before, engine->params.ResolvedValue(
                                render, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(lastCommitted, engine->params.LastCommitted(
                                       render, harken::kTargetVoiceVolume));

    EXPECT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 600.0f, 0));
    EXPECT_FLOAT_EQ(before + 600.0f, engine->params.ResolvedValue(
                                         render, harken::kTargetVoiceVolume));
    Pump(1);
    harken::VoiceRenderContext* ctx = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    float accepted = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(before + 600.0f, &accepted));
    EXPECT_FLOAT_EQ(accepted, ctx->gain.target);

    EXPECT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -800.0f, 0));
    Pump(1);
    EXPECT_TRUE(std::isfinite(ctx->gain.target));
    EXPECT_GE(ctx->gain.target, 0.0f);
    EXPECT_LT(ctx->gain.target, 1.0e-30f);

    const float applied = ctx->gain.target;
    const std::uint64_t invalidBefore =
        engine->voices.InvalidVolumeApplyCount();
    engine->voices.ApplySetVolume(render, 800.0f, 0);
    EXPECT_EQ(applied, ctx->gain.target);
    EXPECT_EQ(invalidBefore + 1u, engine->voices.InvalidVolumeApplyCount());
}

TEST_F(VoiceSetter, CombinedFiniteVolumeContributionsThatOverflowAreRefused) {
    session.reset();
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 400.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::Engine* engine = session->Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    EXPECT_FLOAT_EQ(400.0f, engine->params.ResolvedValue(
                                render, harken::kTargetVoiceVolume));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(handle, 400.0f, 0));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(400.0f, engine->params.ResolvedValue(
                                render, harken::kTargetVoiceVolume));
}

TEST_F(VoiceSetter, PendingAndParkedOverflowingVolumeDoesNotWriteRecords) {
    session.reset();
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc staged = MakePlayDesc(harkentest::ObjectId(1), 400.0f);
    staged.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    VnVoiceHandle pending = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&staged, &pending));
    const harken::VoiceLedger::Resolved pendingResolved =
        session->Engine()->ledger.Classify(pending);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending, pendingResolved.kind);
    harken::PendingStartRecord& pendingRec =
        session->Engine()->pendingStarts[pendingResolved.ledgerSlot];
    const float pendingOutput = pendingRec.outputBusVolumeDb;
    EXPECT_FALSE(pendingRec.hasVolume);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(pending, 800.0f, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(pending, 400.0f, 0));
    EXPECT_FALSE(pendingRec.hasVolume);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(pending, 800.0f, 0));
    EXPECT_FLOAT_EQ(pendingOutput, pendingRec.outputBusVolumeDb);
    EXPECT_EQ(VN_OK, Vn_SetVoiceVolume(pending, 0.0f, 0));
    EXPECT_TRUE(pendingRec.hasVolume);
    EXPECT_FLOAT_EQ(0.0f, pendingRec.volumeDb);

    session.reset();
    VnConfig parkConfig = MakeNullConfig(1, kBlockFrames, kSampleRate);
    parkConfig.maxParkedVoices = 1;
    parkConfig.audibilityParkEnabled = 1;
    parkConfig.parkThresholdDb = -40.0f;
    session = std::make_unique<EngineSession>(parkConfig);
    ASSERT_EQ(VN_OK, session->InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    quiet.loopCount = 0;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &parked));
    const harken::VoiceLedger::Resolved parkedResolved =
        session->Engine()->ledger.Classify(parked);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parkedResolved.kind);
    harken::ParkRecord& parkRec =
        session->Engine()->parkRecords[parkedResolved.ledgerSlot];
    const float parkedOutput = parkRec.facts.outputBusVolumeDb;
    const std::size_t apiDirectCount = parkRec.apiDirect.size();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceVolume(parked, 800.0f, 0));
    EXPECT_EQ(apiDirectCount, parkRec.apiDirect.size());
    for (const harken::ParamSystem::ApiDirectState& a : parkRec.apiDirect) {
        EXPECT_NE(800.0f, a.value);
    }
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(parked, 800.0f, 0));
    EXPECT_FLOAT_EQ(parkedOutput, parkRec.facts.outputBusVolumeDb);
}

TEST_F(VoiceSetter, InitialOverflowingVolumeRefusesStart) {
    session.reset();
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    harken::Engine* engine = session->Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(81)}, 0.0f, 1.0f)
                  .status);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const std::uint64_t invalidBefore =
        engine->voices.InvalidVolumeApplyCount();
    const std::uint32_t activeBefore = engine->voices.ActiveCount();

    VnVoiceHandle refused = 0;
    VnPlayDesc play800 = MakePlayDesc(harkentest::ObjectId(1), 800.0f);
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play800, &refused));
    EXPECT_EQ(0u, refused);
    EXPECT_EQ(activeBefore, engine->voices.ActiveCount());

    ASSERT_EQ(VN_OK, RegisterConstVoiceVolumeBinding(engine, harkentest::ObjectId(10), harkentest::ObjectId(81), 800.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10), harken::kTargetScopeVoice,
                                          harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(81), 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    VnPlayDesc playBound = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    refused = 0;
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&playBound, &refused));
    EXPECT_EQ(0u, refused);
    EXPECT_EQ(activeBefore, engine->voices.ActiveCount());

    ASSERT_EQ(VN_OK, harken::DetachBinding(*engine, harkentest::ObjectId(10), harken::kTargetScopeVoice,
                                          harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, harken::UnregisterBinding(*engine, harkentest::ObjectId(10)));
    ASSERT_EQ(VN_OK, RegisterConstVoiceVolumeBinding(engine, harkentest::ObjectId(11), harkentest::ObjectId(81), 400.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(11), harken::kTargetScopeVoice,
                                          harkentest::ObjectId(1)));
    VnPlayDesc playCombo = MakePlayDesc(harkentest::ObjectId(1), 400.0f);
    refused = 0;
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&playCombo, &refused));
    EXPECT_EQ(0u, refused);
    EXPECT_EQ(activeBefore, engine->voices.ActiveCount());
    Pump(1);
    EXPECT_EQ(invalidBefore, engine->voices.InvalidVolumeApplyCount());
    EXPECT_EQ(activeBefore, engine->voices.ActiveCount());
}

TEST_F(VoiceSetter, LegalInitialVolumeStillStarts) {
    session.reset();
    VnConfig config = MakeNullConfig(4, kBlockFrames, kSampleRate);
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle legal = 0;
    VnPlayDesc play600 = MakePlayDesc(harkentest::ObjectId(1), 600.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play600, &legal));
    ASSERT_NE(0u, legal);
    harken::Engine* engine = session->Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, legal);
    Pump(1);
    harken::VoiceRenderContext* ctx = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    EXPECT_TRUE(std::isfinite(ctx->gain.current));
    EXPECT_TRUE(std::isfinite(ctx->gain.target));
    EXPECT_GT(ctx->gain.current, 0.0f);
    ASSERT_EQ(VN_OK, Vn_Stop(legal, 0));
    Pump(1);

    VnVoiceHandle quiet = 0;
    VnPlayDesc playQuiet = MakePlayDesc(harkentest::ObjectId(1), -6.0f);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playQuiet, &quiet));
    Pump(1);
    harken::VoiceRenderContext* quietCtx = engine->voices.Resolve(
        harkentest::RenderHandle(engine, quiet));
    ASSERT_NE(nullptr, quietCtx);
    float expected = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(-6.0f, &expected));
    EXPECT_FLOAT_EQ(expected, quietCtx->gain.current);
}

TEST_F(VoiceSetter, OverflowingStartDoesNotAffectLiveVoice) {
    harken::Engine* engine = session->Engine();
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    harken::VoiceRenderContext* ctx = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, ctx);
    Pump(1);
    const float liveGain = ctx->gain.current;
    const std::uint64_t invalidBefore =
        engine->voices.InvalidVolumeApplyCount();
    const std::uint32_t activeBefore = engine->voices.ActiveCount();
    EXPECT_GE(activeBefore, 1u);

    VnVoiceHandle refused = 0;
    VnPlayDesc play800 = MakePlayDesc(harkentest::ObjectId(1), 800.0f);
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play800, &refused));
    EXPECT_EQ(0u, refused);
    Pump(1);
    EXPECT_EQ(activeBefore, engine->voices.ActiveCount());
    EXPECT_EQ(render, harkentest::RenderHandle(engine, handle));
    EXPECT_EQ(liveGain, ctx->gain.current);
    EXPECT_EQ(invalidBefore, engine->voices.InvalidVolumeApplyCount());
}

TEST_F(VoiceSetter, ParkReturnOverflowingVolumeDoesNotBecomeAudible) {
    session.reset();
    VnConfig parkConfig = MakeNullConfig(1, kBlockFrames, kSampleRate);
    parkConfig.maxParkedVoices = 1;
    parkConfig.audibilityParkEnabled = 1;
    parkConfig.parkThresholdDb = -40.0f;
    session = std::make_unique<EngineSession>(parkConfig);
    ASSERT_EQ(VN_OK, session->InitResult());
    harken::Engine* engine = session->Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(81)}, 0.0f, 1.0f)
                  .status);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, RegisterConstVoiceVolumeBinding(engine, harkentest::ObjectId(10), harkentest::ObjectId(81), -80.0f));
    ASSERT_EQ(VN_OK, harken::AttachBinding(*engine, harkentest::ObjectId(10), harken::kTargetScopeVoice,
                                          harkentest::ObjectId(1)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(81), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    VnPlayDesc quiet = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    quiet.loopCount = 0;
    VnVoiceHandle parked = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&quiet, &parked));
    const harken::VoiceLedger::Resolved parkedResolved =
        engine->ledger.Classify(parked);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parkedResolved.kind);
    const std::uint64_t invalidBefore =
        engine->voices.InvalidVolumeApplyCount();
    const std::uint32_t activeBefore = engine->voices.ActiveCount();

    const harken::CurvePoint pts[] = {{0.0f, 800.0f}, {1.0f, 800.0f}};
    const harken::CurveHandlePair handles[] = {{0.0f, 800.0f, 1.0f, 800.0f}};
    ASSERT_EQ(VN_OK, harken::ReplaceBinding(*engine, harkentest::ObjectId(10), harkentest::ObjectId(81),
                                           harken::kTargetScopeVoice,
                                           harken::kTargetVoiceVolume, pts,
                                           handles, 2, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    Pump(1);

    const harken::VoiceLedger::Resolved after =
        engine->ledger.Classify(parked);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked, after.kind);
    EXPECT_EQ(activeBefore, engine->voices.ActiveCount());
    EXPECT_EQ(invalidBefore, engine->voices.InvalidVolumeApplyCount());
    EXPECT_EQ(nullptr, engine->voices.Resolve(
                           harkentest::RenderHandle(engine, parked)));
}

TEST_F(VoiceSetter, RejectedBindingParkReturnRetainsRecipeAndDiscardsTemporaryResources) {
    for (const bool definitionBacked : {false, true}) {
        SCOPED_TRACE(definitionBacked);
        session.reset();
        VnConfig config = MakeNullConfig(1, kBlockFrames, kSampleRate);
        config.maxParkedVoices = 1;
        config.audibilityParkEnabled = 1;
        config.parkThresholdDb = -40.0f;
        session = std::make_unique<EngineSession>(config);
        ASSERT_EQ(VN_OK, session->InitResult());
        harken::Engine *engine = session->Engine();
        ASSERT_EQ(
            harken::WorldInstallStatus::Ok,
            harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(81)}, 0.0f, 1.0f)
                .status);
        samples.assign(kSampleRate, 0.05f);
        desc = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        const harken::CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, 800.0f}};
        const harken::CurveHandlePair handles[] = {{0.0f, 0.0f, 1.0f, 800.0f}};
        if (definitionBacked) {
            ASSERT_EQ(VN_OK,
                      harken::RegisterBinding(*engine, harkentest::ObjectId(10),
                                              harkentest::ObjectId(81), harken::kTargetScopeVoice,
                                              harken::kTargetVoiceVolume, points, handles, 2, 0));
            ASSERT_EQ(VN_OK,
                      harken::AttachBinding(*engine, harkentest::ObjectId(10),
                                            harken::kTargetScopeVoice, harkentest::ObjectId(1)));
        }
        const auto baseDemand = engine->propertyPublisher.WorstCaseCommits();
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        play.loopCount = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
        harken::Curve curve;
        ASSERT_TRUE(curve.SetPoints(points, handles, 2));
        if (!definitionBacked) {
            ASSERT_TRUE(engine->params.BindVoiceTarget(harkentest::ObjectId(81), render,
                                                       harken::kTargetVoiceVolume, curve));
        }
        ASSERT_EQ(baseDemand + 1, engine->propertyPublisher.WorstCaseCommits());
        ASSERT_EQ(VN_OK, Vn_Update());
        Pump(2);
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -80.0f, 0));
        Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto parked = engine->ledger.Classify(handle);
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
        Pump(2);
        ASSERT_EQ(nullptr, engine->voices.Resolve(render));
        const auto &record = engine->parkRecords[parked.ledgerSlot];
        ASSERT_EQ(definitionBacked ? 1u : 0u, record.instanceReservedTargets);
        ASSERT_EQ(definitionBacked ? 0u : 1u, record.bindings.size());
        ASSERT_EQ(definitionBacked ? 1u : 0u, record.bindingInstances.size());
        ASSERT_TRUE(record.heldPcmRef);
        auto *const useCount = record.facts.view.useCount;
        ASSERT_NE(nullptr, useCount);
        const auto references = useCount->load(std::memory_order_acquire);
        ASSERT_EQ(1u, references);
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 0.0f, 0));
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(81), 1.0f));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
        const auto commandHead = engine->commands.PublishedHead();
        const auto invalidApplies = engine->voices.InvalidVolumeApplyCount();
        for (int attempt = 0; attempt < 3; ++attempt) {
            ASSERT_EQ(VN_OK, Vn_Update());
            Pump(1);
            EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
                      engine->ledger.Classify(handle).kind);
            EXPECT_EQ(baseDemand + (definitionBacked ? 1u : 0u),
                      engine->propertyPublisher.WorstCaseCommits());
            EXPECT_EQ(0u, engine->voices.ActiveCount());
            EXPECT_EQ(commandHead, engine->commands.PublishedHead());
            EXPECT_EQ(invalidApplies, engine->voices.InvalidVolumeApplyCount());
            EXPECT_EQ(0u, engine->params.VoiceBindingCount(render));
            EXPECT_TRUE(record.heldPcmRef);
            EXPECT_EQ(references, useCount->load(std::memory_order_acquire));
            EXPECT_EQ(definitionBacked ? 1u : 0u, record.instanceReservedTargets);
            if (definitionBacked) {
                ASSERT_EQ(1u, record.bindingInstances.size());
                EXPECT_EQ(harkentest::ObjectId(10), record.bindingInstances[0].definitionId);
            } else {
                ASSERT_EQ(1u, record.bindings.size());
                EXPECT_EQ(harkentest::ObjectId(81), record.bindings[0].variableId);
                EXPECT_EQ(harken::kTargetVoiceVolume, record.bindings[0].targetId);
                EXPECT_FLOAT_EQ(0.0f, record.bindings[0].curve.Eval(0.0f));
                EXPECT_FLOAT_EQ(800.0f, record.bindings[0].curve.Eval(1.0f));
            }
            for (std::uint32_t sample = 0; sample < 2 * kBlockFrames; ++sample)
                EXPECT_FLOAT_EQ(0.0f, engine->nullBackend.LastBlock()[sample]);
        }
        ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(81), 0.0f));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
        ASSERT_EQ(VN_OK, Vn_Update());
        Pump(1);
        EXPECT_EQ(harken::VoiceLedger::HandleKind::Live, engine->ledger.Classify(handle).kind);
        const auto returnedRender = harkentest::RenderHandle(engine, handle);
        ASSERT_NE(nullptr, engine->voices.Resolve(returnedRender));
        EXPECT_EQ(1u, engine->voices.ActiveCount());
        EXPECT_EQ(1u, engine->params.VoiceBindingCount(returnedRender));
        EXPECT_EQ(baseDemand + 1, engine->propertyPublisher.WorstCaseCommits());
        for (std::uint32_t sample = kBlockFrames; sample < 2 * kBlockFrames; ++sample)
            EXPECT_NEAR(0.05f, engine->nullBackend.LastBlock()[sample], 1.0e-6f);
    }
}

TEST_F(VoiceSetter, PitchCommandRetunesTheResamplerInPlace) {
    harken::VoiceRenderContext* ctx =
        session->Engine()->voices.Resolve(harkentest::RenderHandle(
            session->Engine(), handle));
    ASSERT_NE(nullptr, ctx);
    Pump(1);
    ASSERT_EQ(1.0, ctx->resampler.CurrentRatio()); // 48k source at 48k engine

    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(handle, 2.0f));
    Pump(1);
    ExpectPitchSnapAfterBlock(*ctx, 2.0);
    Pump(1);
    EXPECT_DOUBLE_EQ(2.0, ctx->resampler.CurrentRatio());
    EXPECT_EQ(0u, ctx->resampler.RampFramesRemaining());
}

TEST(VoiceFilter, EnableChangesDirectAndVoiceSend) {
    constexpr std::uint32_t kBlock = 256;
    VnConfig config = MakeNullConfig(2, kBlock, 48000);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(11), VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, harkentest::ObjectId(11), 0.0f));

    harkentest::Capture both;
    harkentest::PumpAndCapture(session.Engine(), 2, both);
    ASSERT_NEAR(1.0f, both.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, -120.0f, 0));
    harkentest::Capture sendOnly;
    harkentest::PumpAndCapture(session.Engine(), 2, sendOnly);
    ASSERT_NEAR(0.5f, sendOnly.left.back(), 1e-4f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, 0.0f, 0));

    ASSERT_EQ(VN_OK,
              Vn_SetVoiceFilter(handle, harken::kFilterKindHighpass, 1000.0f,
                                0.707f, 1, 0));
    harkentest::Capture filteredBoth;
    harkentest::PumpAndCapture(session.Engine(), 8, filteredBoth);
    EXPECT_LT(std::fabs(filteredBoth.left.back()), 0.05f);

    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, -120.0f, 0));
    harkentest::Capture filteredSend;
    harkentest::PumpAndCapture(session.Engine(), 4, filteredSend);
    EXPECT_LT(std::fabs(filteredSend.left.back()), 0.05f);
}

TEST(VoiceFilter, DoesNotAffectOtherVoices) {
    constexpr std::uint32_t kBlock = 256;
    VnConfig config = MakeNullConfig(4, kBlock, 48000);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.loopCount = 0;
    VnVoiceHandle filtered = 0;
    VnVoiceHandle other = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &filtered));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &other));
    harkentest::Capture before;
    harkentest::PumpAndCapture(session.Engine(), 2, before);
    ASSERT_NEAR(1.0f, before.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK,
              Vn_SetVoiceFilter(filtered, harken::kFilterKindHighpass,
                                1000.0f, 0.707f, 1, 0));
    harkentest::Capture after;
    harkentest::PumpAndCapture(session.Engine(), 8, after);
    ASSERT_NEAR(0.5f, after.left.back(), 5e-2f);

    ASSERT_EQ(VN_OK, Vn_Stop(filtered, 0));
    harkentest::Capture otherOnly;
    harkentest::PumpAndCapture(session.Engine(), 2, otherOnly);
    ASSERT_NEAR(0.5f, otherOnly.left.back(), 1e-4f);
}

TEST(VoiceFilter, ApiDirectComposesWithVariableCutoff) {
    constexpr std::uint32_t kBlock = 256;
    VnConfig config = MakeNullConfig(2, kBlock, 48000);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(91)}, 0.0f, 1.0f)
                  .status);
    const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::Curve curve;
    const harken::CurvePoint points[] = {{0.0f, 20000.0f}};
    ASSERT_TRUE(curve.SetPoints(points, nullptr, 1));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, handle);
    ASSERT_TRUE(engine->params.BindVoiceTarget(
        harkentest::ObjectId(91), render, harken::kTargetVoiceLowpassCutoff, curve));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(91), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceFilter(handle, harken::kFilterKindLowpass, 800.0f,
                                0.707f, 1, 0));
    EXPECT_FLOAT_EQ(800.0f,
                    engine->params.ResolvedValue(
                        render, harken::kTargetVoiceLowpassCutoff));
    const float resolvedQ = engine->params.ResolvedValue(
        render, harken::kTargetVoiceLowpassQ);
    EXPECT_TRUE(std::isfinite(resolvedQ));
    EXPECT_GE(resolvedQ, 0.1f);
    EXPECT_LE(resolvedQ, 10.0f);
}

} // namespace
