// Golden cases for the widened N-channel fold-in (the multichannel amendment,
// spec M5 §6.2): a mono point source pans across the layout's front speakers by
// the pairwise constant-power law, the LFE is never fed by panning, and a
// multichannel 2D source maps channel-for-channel by role (proving the
// Appendix A channel order).
//
// The pan law: with the default listener (origin, forward (0,0,-1), up
// (0,1,0)) the right axis is +x, so an emitter direction whose normalized x
// component is p resolves pan = p (as in the stereo pan_law golden). The
// widened stage maps p across the ordered front speakers ({FL, FC, FR} for
// 5.1/7.1) and splits power in the bracketing pair by gLeft = sqrt(2)*cos(q*pi/2),
// gRight = sqrt(2)*sin(q*pi/2) with q the local segment position. For a DC source of
// amplitude a through a unity cascade the captured channel equals a * gain.
// Tolerance 1e-5: one dB->linear-free multiply per channel on an exact DC path.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>

namespace {

using harkentest::CaptureMulti;
using harkentest::EngineSession;
using harkentest::InterleavedDesc;
using harkentest::MakeNullConfigLayout;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kAmp = 0.25f;
const float kPannedAmp = kAmp * std::sqrt(2.0f);
constexpr float kTol = 1e-5f;

// 5.1 / 7.1 canonical channel indices (Appendix A).
enum { FL = 0, FR = 1, FC = 2, LFE = 3, BL = 4, BR = 5, SL = 6, SR = 7 };

// Renders one spatialized mono DC voice at `pos` in `mixLayout` (device ==
// mix, so no output downmix) and returns each channel's steady block-2 value.
std::vector<float> CapturePannedN(std::uint32_t mixLayout, float x, float y,
                                  float z, float spatializationMix = 1.0f) {
    EngineSession session(
        MakeNullConfigLayout(2, kBlockFrames, 48000, mixLayout, mixLayout));
    EXPECT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), x, y, z);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    if (spatializationMix != 1.0f) {
        EXPECT_EQ(VN_OK,
                  Vn_SetVoiceSpatializationMix(handle, spatializationMix));
    }
    EXPECT_EQ(VN_OK, Vn_Update()); // the tick resolves and commits the pan

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    std::vector<float> out(cap.channelCount, 0.0f);
    for (std::uint32_t ch = 0; ch < cap.channelCount; ++ch) {
        out[ch] = cap.channels[ch][kBlockFrames + kBlockFrames / 2];
    }
    return out;
}

TEST(MultichannelPanGolden, FiveOneFullLeftIsFrontLeftOnly) {
    const std::vector<float> g = CapturePannedN(VN_CHANNEL_LAYOUT_5_1, -10.0f,
                                                0.0f, 0.0f);
    ASSERT_EQ(6u, g.size());
    ASSERT_GT(std::fabs(g[FL]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(kPannedAmp, g[FL], kTol); // pan -1 -> q=0 -> FL = sqrt(2)
    EXPECT_NEAR(0.0f, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[FR], kTol);
    EXPECT_NEAR(0.0f, g[LFE], kTol);
    EXPECT_NEAR(0.0f, g[BL], kTol);
    EXPECT_NEAR(0.0f, g[BR], kTol);
}

TEST(MultichannelPanGolden, FiveOneCenterIsFrontCenterOnly) {
    const std::vector<float> g =
        CapturePannedN(VN_CHANNEL_LAYOUT_5_1, 0.0f, 0.0f, -10.0f);
    ASSERT_EQ(6u, g.size());
    ASSERT_GT(std::fabs(g[FC]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // pan 0 -> the center bracket boundary -> all energy in the discrete
    // center speaker (this engine's documented center policy).
    EXPECT_NEAR(kPannedAmp, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[FL], kTol);
    EXPECT_NEAR(0.0f, g[FR], kTol);
    EXPECT_NEAR(0.0f, g[LFE], kTol);
}

TEST(MultichannelPanGolden, FiveOneFullRightIsFrontRightOnly) {
    const std::vector<float> g =
        CapturePannedN(VN_CHANNEL_LAYOUT_5_1, 10.0f, 0.0f, 0.0f);
    ASSERT_EQ(6u, g.size());
    ASSERT_GT(std::fabs(g[FR]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(kPannedAmp, g[FR], kTol); // pan +1 -> q=1 in the FC-FR segment -> FR
    EXPECT_NEAR(0.0f, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[FL], kTol);
}

TEST(MultichannelPanGolden, FiveOneMixZeroUsesFrontTwoDimensionalImage) {
    const std::vector<float> g =
        CapturePannedN(VN_CHANNEL_LAYOUT_5_1, 10.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_EQ(6u, g.size());
    ASSERT_GT(std::fabs(g[FL]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), g[FL], kTol);
    EXPECT_NEAR(kPannedAmp / std::sqrt(2.0f), g[FR], kTol);
    EXPECT_NEAR(0.0f, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[LFE], kTol);
    EXPECT_NEAR(0.0f, g[BL], kTol);
    EXPECT_NEAR(0.0f, g[BR], kTol);
}

TEST(MultichannelPanGolden, FiveOneMixHalfBlendsFrontImagePerChannel) {
    const std::vector<float> g =
        CapturePannedN(VN_CHANNEL_LAYOUT_5_1, -10.0f, 0.0f, 0.0f, 0.5f);
    ASSERT_EQ(6u, g.size());
    ASSERT_GT(std::fabs(g[FR]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    // Full-left 3D is (FL=sqrt(2), FR=0); 2D mono is (FL=1, FR=1).
    EXPECT_NEAR(0.5f * kPannedAmp * (1.0f + 1.0f / std::sqrt(2.0f)), g[FL], kTol);
    EXPECT_NEAR(0.5f * kPannedAmp / std::sqrt(2.0f), g[FR], kTol);
    EXPECT_NEAR(0.0f, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[LFE], kTol);
    EXPECT_NEAR(0.0f, g[BL], kTol);
    EXPECT_NEAR(0.0f, g[BR], kTol);
}

TEST(MultichannelPanGolden, MonoMixLayoutHasEquivalentEndpoints) {
    const std::vector<float> left =
        CapturePannedN(VN_CHANNEL_LAYOUT_MONO, -10.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<float> right =
        CapturePannedN(VN_CHANNEL_LAYOUT_MONO, 10.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(1u, left.size());
    ASSERT_EQ(1u, right.size());
    EXPECT_NEAR(kAmp, left[0], kTol);
    EXPECT_NEAR(kAmp, right[0], kTol);
}

TEST(MultichannelPanGolden, FiveOneHalfLeftBracketsFrontLeftAndCenter) {
    // Emitter direction (-0.5, 0, -sqrt(0.75)): unit x = -0.5, so pan = -0.5,
    // p = 0.25, segment FL-FC at local q = 0.5 -> both gains cos(pi/4).
    const std::vector<float> g =
        CapturePannedN(VN_CHANNEL_LAYOUT_5_1, -5.0f, 0.0f, -8.66025404f);
    ASSERT_EQ(6u, g.size());
    const float expected =
        kPannedAmp * static_cast<float>(std::cos(3.14159265358979323846 / 4.0));
    ASSERT_GT(std::fabs(g[FL]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(expected, g[FL], kTol);
    EXPECT_NEAR(expected, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[FR], kTol);
    EXPECT_NEAR(0.0f, g[BL], kTol);
    // Constant power across the active pair.
    EXPECT_NEAR(kPannedAmp * kPannedAmp, g[FL] * g[FL] + g[FC] * g[FC], 1e-5f);
}

TEST(MultichannelPanGolden, LiveSegmentCrossingKeepsOldSpeakerContinuous) {
    EngineSession session(MakeNullConfigLayout(2, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> dc = harkentest::MakeDC(48000, kAmp);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play =
        MakePlayDesc3D(harkentest::ObjectId(1), -5.0f, 0.0f, -8.66025404f);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());

    CaptureMulti steady;
    harkentest::PumpAndCaptureMulti(engine, 1, steady);
    ASSERT_EQ(6u, steady.channelCount);

    ASSERT_EQ(VN_OK,
              Vn_SetVoicePosition(
                  handle, harkentest::Vec3(5.0f, 0.0f, -8.66025404f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    CaptureMulti moving;
    harkentest::PumpAndCaptureMulti(engine, 2, moving);
    ASSERT_EQ(6u, moving.channelCount);

    const float expected =
        kPannedAmp * static_cast<float>(std::cos(3.14159265358979323846 / 4.0));
    EXPECT_NEAR(expected, steady.channels[FL].back(), kTol);
    EXPECT_NEAR(expected, steady.channels[FC].back(), kTol);
    EXPECT_NEAR(0.0f, steady.channels[FR].back(), kTol);
    EXPECT_NEAR(steady.channels[FL].back(), moving.channels[FL].front(), kTol);
    EXPECT_NEAR(steady.channels[FC].back(), moving.channels[FC].front(), kTol);
    EXPECT_NEAR(steady.channels[FR].back(), moving.channels[FR].front(), kTol);
    EXPECT_NEAR(0.0f, moving.channels[FL].back(), kTol);
    EXPECT_NEAR(expected, moving.channels[FC].back(), kTol);
    EXPECT_NEAR(expected, moving.channels[FR].back(), kTol);
}

TEST(MultichannelPanGolden, SevenOneFullLeftIsFrontLeftOnly) {
    const std::vector<float> g =
        CapturePannedN(VN_CHANNEL_LAYOUT_7_1, -10.0f, 0.0f, 0.0f);
    ASSERT_EQ(8u, g.size());
    ASSERT_GT(std::fabs(g[FL]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(kPannedAmp, g[FL], kTol);
    EXPECT_NEAR(0.0f, g[FC], kTol);
    EXPECT_NEAR(0.0f, g[FR], kTol);
    // The side and back channels never receive panned point-source content.
    EXPECT_NEAR(0.0f, g[SL], kTol);
    EXPECT_NEAR(0.0f, g[SR], kTol);
    EXPECT_NEAR(0.0f, g[BL], kTol);
    EXPECT_NEAR(0.0f, g[BR], kTol);
}

TEST(MultichannelPanGolden, LfeNeverReceivesPannedContent) {
    // Sweep several pans; the LFE stays silent at every one (spec §6.2: the
    // LFE is never fed by panning).
    const float xs[] = {-10.0f, -5.0f, 0.0f, 5.0f, 10.0f};
    for (float x : xs) {
        const std::vector<float> g =
            CapturePannedN(VN_CHANNEL_LAYOUT_5_1, x, 0.0f, -1.0f);
        ASSERT_EQ(6u, g.size());
        EXPECT_NEAR(0.0f, g[LFE], kTol) << "LFE non-zero at x=" << x;
    }
}

TEST(MultichannelPanGolden, MultichannelSourceMapsChannelForChannel) {
    // A 5.1 (6-channel) 2D source into a 5.1 mix maps by role — identity here —
    // so channel c of the output carries source channel c: a known-signal-per-
    // channel proof of the Appendix A channel order.
    EngineSession session(MakeNullConfigLayout(2, kBlockFrames, 48000,
                                               VN_CHANNEL_LAYOUT_5_1,
                                               VN_CHANNEL_LAYOUT_5_1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Interleaved 6-channel DC, a distinct level per channel.
    const std::uint32_t frames = 512;
    std::vector<float> data(static_cast<std::size_t>(frames) * 6);
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t c = 0; c < 6; ++c) {
            data[i * 6 + c] = 0.1f * static_cast<float>(c + 1);
        }
    }
    const VnPCMDesc desc = InterleavedDesc(data, 6, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &desc));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(2)); // 2D (a multichannel source)
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    ASSERT_EQ(6u, cap.channelCount);
    for (std::uint32_t c = 0; c < 6; ++c) {
        const float v = cap.channels[c][kBlockFrames + kBlockFrames / 2];
        EXPECT_NEAR(0.1f * static_cast<float>(c + 1), v, kTol)
            << "channel " << c;
    }
}

} // namespace
