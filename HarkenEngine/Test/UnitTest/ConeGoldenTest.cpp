// Golden case: directional cone attenuation — inner zone, dB-domain
// midpoint, outer zone, and the cone lowpass's log-domain midpoint.
//
// Profile: inverse-square rolloff with the voice at d = dmin (rolloff
// contributes exactly 0 dB, isolating the cone), neutral air, cone inner
// 30 deg / outer 90 deg / outer gain -24 dB / outer lowpass 2000 Hz.
// The voice sits straight ahead at distance 1; Vn_SetVoiceOrientation
// steers the emitter's forward axis so the emitter-to-listener angle
// theta takes 0, 60 and 120 degrees:
//   theta 0   (<= inner):        0 dB, no cone lowpass
//   theta 60  (transition mid): -24 * (60-30)/(90-30) = -12 dB —
//             the dB-domain interpolation (amplitude-domain interpolation
//             would read about -5.6 dB and fails the tolerance);
//             lowpass = sqrt(20000 * 2000) = 6324.6 Hz, the log-domain
//             midpoint between the audible-band-open reference and the
//             outer cutoff
//   theta 120 (>= outer):       -24 dB, lowpass 2000 Hz
// Render evidence uses amplitude ratios against the on-axis capture, so
// the pan split cancels.
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
using harkentest::Vec3;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.5f;
constexpr VnAttenuationId kProfile = harkentest::ObjectId(17);

// Captures the steady left-channel value with the emitter facing
// `forward`; also reports the resolved volume dB and lowpass Hz.
void CaptureFacing(VnVec3 forward, float* outLeft, float* outVolumeDb,
                   float* outLowpassHz) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc pcm = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    VnAttenuationDesc desc =
        MakeAttenuationDesc(1.0f, 100.0f, VN_ROLLOFF_INVERSE_SQUARE);
    desc.coneEnabled = 1;
    desc.coneInnerDeg = 30.0f;
    desc.coneOuterDeg = 90.0f;
    desc.coneOuterGainDb = -24.0f;
    desc.coneOuterLowpassHz = 2000.0f;
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(kProfile, &desc));

    // Straight ahead at exactly dmin: the rolloff gain is 1 (0 dB).
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -1.0f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, kProfile));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOrientation(handle, forward));
    ASSERT_EQ(VN_OK, Vn_Update());

    *outVolumeDb =
        engine->params.ResolvedValue(harkentest::RenderHandle(engine, handle),
            harken::kTargetVoiceVolume);
    *outLowpassHz = engine->params.ResolvedValue(harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceLowpassCutoff);

    Capture captured;
    harkentest::PumpAndCapture(engine, 4, captured);
    *outLeft = captured.left[4 * kBlockFrames - 1];
}

TEST(ConeGolden, InnerMidpointOuterFollowTheDbDomainLaw) {
    // The listener sits at the origin; the voice at (0,0,-1) sees it
    // along (0,0,1). Forward vectors at 0/60/120 degrees off that axis:
    float onAxis = 0.0f, onAxisDb = 0.0f, onAxisHz = 0.0f;
    CaptureFacing(Vec3(0.0f, 0.0f, 1.0f), &onAxis, &onAxisDb, &onAxisHz);
    ASSERT_GT(std::fabs(onAxis), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(0.0f, onAxisDb, 1e-3f);
    // Inside the inner angle the cone does not restrict the spectrum:
    // the lowpass target holds the profile's (neutral) air value.
    EXPECT_FLOAT_EQ(1.0e9f, onAxisHz);

    float mid = 0.0f, midDb = 0.0f, midHz = 0.0f;
    CaptureFacing(Vec3(0.86602540f, 0.0f, 0.5f), &mid, &midDb, &midHz);
    // theta = 60: halfway through [30, 90] in the dB domain is -12 dB.
    // Amplitude-domain interpolation would resolve about -5.6 dB and
    // trip this tolerance.
    EXPECT_NEAR(-12.0f, midDb, 1e-2f);
    // Cone lowpass midpoint: geometric mean of 20 kHz and 2 kHz.
    EXPECT_NEAR(6324.6f, midHz, 10.0f);
    const float midRatio = mid / onAxis;
    EXPECT_NEAR(std::pow(10.0f, -12.0f / 20.0f), midRatio, 0.01f);

    float outer = 0.0f, outerDb = 0.0f, outerHz = 0.0f;
    CaptureFacing(Vec3(0.86602540f, 0.0f, -0.5f), &outer, &outerDb,
                  &outerHz);
    // theta = 120 >= outer: the outer values hold.
    EXPECT_NEAR(-24.0f, outerDb, 1e-2f);
    EXPECT_FLOAT_EQ(2000.0f, outerHz);
    const float outerRatio = outer / onAxis;
    EXPECT_NEAR(std::pow(10.0f, -24.0f / 20.0f), outerRatio, 0.005f);
}

} // namespace
