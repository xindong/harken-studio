// Golden case: stage taps report the block peak at each cascade stage
// boundary, and the bus output tap (the bus meter) reflects the bus's
// applied gain.
//
// Analytic expectations for a 1 kHz sine, amplitude 0.5, engine rate
// source (unity resample ratio):
//   - postResample ~ 0.5: a 256-frame block spans 5.3 cycles, and the
//     densest sample sits within pi/48 rad of a crest, so the sampled
//     peak is >= 0.5 * cos(pi/48) ~ 0.4989. Tolerance 0.02 leaves margin
//     for the interpolation kernel.
//   - passthrough insert effect and bypassed tone filter change nothing:
//     postFx and postFilter equal postResample exactly.
//   - a -6.0206 dB voice gain (linear 0.5) halves postGain relative to
//     postFilter.
//   - a -6.0206 dB bus gain halves the bus meter relative to the voice's
//     postGain contribution; the master meter equals the child bus meter
//     because the master's own gain stays at unity.
//
// When tap measurement is compiled out the same fields must read exactly
// 0 — the layout is build-invariant, only the measurement is gated.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/SnapshotBuffer.h"

#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr float kAmplitude = 0.5f;
// dB value whose linear gain is 0.5 to well below float precision.
constexpr float kHalfGainDb = -6.0205999f;

VnSnapshot CaptureNow() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

TEST(StageTapGolden, UnityCascadeTapsMatchSinePeak) {
    EngineSession session(MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 1000.0, 48000.0, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(4);

    const VnSnapshot snapshot = CaptureNow();
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    ASSERT_GE(row, 0);
    const VnVoiceSnapshot& voice = snapshot.voices[row];
    ASSERT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              voice.state);

#if HARKEN_STAGE_TAPS_ACTIVE
    // Scalar silence guard: a tap below the floor would turn the
    // comparisons below into silent-vs-silent.
    ASSERT_GT(voice.tapPostResample, harkentest::kSilenceFloor);
    EXPECT_NEAR(kAmplitude, voice.tapPostResample, 0.02f);
    // Passthrough effect and bypassed filters are exact identities.
    EXPECT_FLOAT_EQ(voice.tapPostResample, voice.tapPostFx);
    EXPECT_FLOAT_EQ(voice.tapPostFx, voice.tapPostFilter);
    // Unity gain: the gain stage multiplies by exactly 1.
    EXPECT_FLOAT_EQ(voice.tapPostFilter, voice.tapPostGain);
    ASSERT_GT(snapshot.busPeaks[0], harkentest::kSilenceFloor);
    EXPECT_FLOAT_EQ(voice.tapPostGain, snapshot.busPeaks[0]);
#else
    EXPECT_EQ(0.0f, voice.tapPostResample);
    EXPECT_EQ(0.0f, voice.tapPostFx);
    EXPECT_EQ(0.0f, voice.tapPostFilter);
    EXPECT_EQ(0.0f, voice.tapPostGain);
    // The BUS OUTPUT peak left the stage-tap gate in M6 (spec section
    // 11.7): honest in every build, only the per-voice taps read 0.
    EXPECT_GT(snapshot.busPeaks[0], harkentest::kSilenceFloor);
#endif
}

TEST(StageTapGolden, VoiceGainScalesOnlyThePostGainTap) {
    EngineSession session(MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 1000.0, 48000.0, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), kHalfGainDb);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(4);

    const VnSnapshot snapshot = CaptureNow();
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    ASSERT_GE(row, 0);
    const VnVoiceSnapshot& voice = snapshot.voices[row];

#if HARKEN_STAGE_TAPS_ACTIVE
    ASSERT_GT(voice.tapPostFilter, harkentest::kSilenceFloor);
    // Upstream taps are unaffected by the gain stage...
    EXPECT_NEAR(kAmplitude, voice.tapPostResample, 0.02f);
    EXPECT_FLOAT_EQ(voice.tapPostResample, voice.tapPostFilter);
    // ...while the post-gain tap carries exactly the halved signal.
    EXPECT_NEAR(0.5f * voice.tapPostFilter, voice.tapPostGain, 1e-3f);
    ASSERT_GT(voice.tapPostGain, harkentest::kSilenceFloor);
#else
    EXPECT_EQ(0.0f, voice.tapPostGain);
#endif
}

TEST(StageTapGolden, ToneFilterAttenuationShowsOnlyAtPostFilter) {
    // A 7 kHz sine against a 500 Hz Butterworth lowpass (via the Variable
    // binding route into the per-voice filter): |H| = 1/sqrt(1+(f/fc)^4)
    // ~ 0.005 at 14x the cutoff, so the post-filter tap must collapse
    // while the upstream taps still read the raw sine peak. The 0.05
    // ratio bound leaves a 10x margin over the analytic response. 7 kHz
    // at 48 kHz lands exactly on a crest every 48 samples, so the raw
    // block peak equals the amplitude.
    EngineSession session(MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(11)}, 0.0f, 1.0f).status);

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 7000.0, 48000.0, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 500.0f}, {1.0f, 500.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(harkentest::ObjectId(11), harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceLowpassCutoff, curve));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(11), 0.5f));
    ASSERT_EQ(VN_OK, Vn_Update());

    // Filter and play apply at the first block boundary; by the fourth
    // block the biquad's startup transient is long gone.
    engine->nullBackend.Pump(4);
    const VnSnapshot snapshot = CaptureNow();
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    ASSERT_GE(row, 0);
    const VnVoiceSnapshot& voice = snapshot.voices[row];
    ASSERT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              voice.state);

#if HARKEN_STAGE_TAPS_ACTIVE
    // Upstream of the filter nothing changed: raw sine peak, and the
    // passthrough effect is an exact identity.
    ASSERT_GT(voice.tapPostResample, harkentest::kSilenceFloor);
    EXPECT_NEAR(kAmplitude, voice.tapPostResample, 0.02f);
    EXPECT_FLOAT_EQ(voice.tapPostResample, voice.tapPostFx);
    // The filter collapses the tone; measuring before the filter (or
    // copying the upstream tap) would fail this by two orders of
    // magnitude. The upstream tap is silence-guarded above, so this can
    // never compare silence against silence.
    EXPECT_LT(voice.tapPostFilter, 0.05f * voice.tapPostFx);
    // Unity gain: the post-gain tap carries the filtered signal.
    EXPECT_FLOAT_EQ(voice.tapPostFilter, voice.tapPostGain);
#else
    EXPECT_EQ(0.0f, voice.tapPostResample);
    EXPECT_EQ(0.0f, voice.tapPostFx);
    EXPECT_EQ(0.0f, voice.tapPostFilter);
    EXPECT_EQ(0.0f, voice.tapPostGain);
#endif
}

TEST(StageTapGolden, BusGainScalesTheBusMeter) {
    EngineSession session(MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 1000.0, 48000.0, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    constexpr VnBusId kBus = harkentest::ObjectId(7);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBus, kHalfGainDb, 0));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBus;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(4);

    const VnSnapshot snapshot = CaptureNow();
    ASSERT_EQ(2u, snapshot.busCount);
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    ASSERT_GE(row, 0);
    const VnVoiceSnapshot& voice = snapshot.voices[row];

#if HARKEN_STAGE_TAPS_ACTIVE
    ASSERT_GT(voice.tapPostGain, harkentest::kSilenceFloor);
    // Bus meters measure after the bus's own gain: the created bus (dense
    // index 1) halves the voice's contribution.
    ASSERT_GT(snapshot.busPeaks[1], harkentest::kSilenceFloor);
    EXPECT_NEAR(0.5f * voice.tapPostGain, snapshot.busPeaks[1], 1e-3f);
    EXPECT_NEAR(0.25f, snapshot.busPeaks[1], 0.02f);
    // The master folded exactly this one child and applies unity gain,
    // so the final-mix meter equals the child's meter.
    EXPECT_FLOAT_EQ(snapshot.busPeaks[1], snapshot.busPeaks[0]);
#else
    // Section 11.7: bus output peaks are live in every build — only the
    // per-voice taps are gated, so the bus-gain relation still holds.
    ASSERT_GT(snapshot.busPeaks[1], harkentest::kSilenceFloor);
    EXPECT_NEAR(0.25f, snapshot.busPeaks[1], 0.02f);
    EXPECT_FLOAT_EQ(snapshot.busPeaks[1], snapshot.busPeaks[0]);
#endif
}

} // namespace
