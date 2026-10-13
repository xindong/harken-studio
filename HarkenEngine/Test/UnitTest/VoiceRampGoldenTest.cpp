// Golden cases: the voice volume ramp against its closed form. Voice Volume
// is frame-accurate OutputGain: a change from gain a to gain b over
// N = ceil(rampMs * rate / 1000) produced samples scales ramp sample n
// (0-based) by
//
//     g(n) = a + (b - a) * (n + 1) / N
//
// with the N-th sample forced to b. rampMs == 0 snaps at the next block
// boundary. Do not round N up to a whole block (10ms@48k is 480, not 512;
// 20ms@48k is 960, not 1024).
//
// Tolerances:
//   - ramp curve vs closed form: 1e-4 absolute;
//   - end frame exact;
//   - post-ramp / snapped plateaus: 1e-5 absolute;
//   - adjacent-sample step bound (zipper surrogate): |b - a| / N + 2e-4.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "harken/dsp/GainRamp.h"

#include <cmath>
#include <memory>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;

const float kHalfDb = 20.0f * std::log10(0.5f);     // gain 0.5
const float kQuarterDb = 20.0f * std::log10(0.25f); // gain 0.25

class VoiceRampGolden : public ::testing::Test {
protected:
    void SetUp() override {
        session = std::make_unique<EngineSession>(
            MakeNullConfig(2, kBlockFrames, 48000));
        ASSERT_EQ(VN_OK, session->InitResult());

        dc = harkentest::MakeDC(48000, 1.0f);
        HARKENTEST_ASSERT_NON_SILENT(dc);
        desc = MonoDesc(dc, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1)); // 0 dB: unity authored gain
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    }

    std::unique_ptr<EngineSession> session;
    std::vector<float> dc;
    VnPCMDesc desc{};
    VnVoiceHandle handle = 0;
};

TEST_F(VoiceRampGolden, RampFollowsClosedFormAndStaysStepBounded) {
    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // Unity plateau before the ramp: play-time gain snapped exactly.
    ASSERT_EQ(1.0f, out.left[0]);
    ASSERT_EQ(1.0f, out.left[kBlockFrames - 1]);

    // 10 ms at 48 kHz = 480 frames, not 512 whole-block frames.
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, kQuarterDb, 10));
    harkentest::PumpAndCapture(session->Engine(), 2, out);

    const std::size_t rampStart = kBlockFrames;
    const std::size_t rampLen = 480;
    for (std::size_t n = 0; n < rampLen; ++n) {
        const float expected =
            1.0f + (0.25f - 1.0f) * static_cast<float>(n + 1) /
                       static_cast<float>(rampLen);
        ASSERT_NEAR(expected, out.left[rampStart + n], 1e-4f)
            << "ramp sample " << n;
    }
    float targetLinear = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(kQuarterDb, &targetLinear));
    ASSERT_EQ(targetLinear, out.left[rampStart + rampLen - 1]);

    const float stepBound = 0.75f / static_cast<float>(rampLen) + 2e-4f;
    for (std::size_t i = rampStart - 1; i + 1 < rampStart + rampLen; ++i) {
        ASSERT_LE(std::fabs(out.left[i + 1] - out.left[i]), stepBound)
            << "step at sample " << i;
    }

    // The remainder of the second block, then a third block, hold target.
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    for (std::size_t n = rampStart + rampLen; n < out.left.size(); ++n) {
        ASSERT_NEAR(0.25f, out.left[n], 1e-5f) << "plateau sample " << n;
    }
}

TEST_F(VoiceRampGolden, TwentyMsAt48kLandsOnFrame960Not1024) {
    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, kQuarterDb, 20));
    harken::VoiceRenderContext* ctx = session->Engine()->voices.Resolve(
        harkentest::RenderHandle(session->Engine(), handle));
    ASSERT_NE(nullptr, ctx);

    harkentest::PumpAndCapture(session->Engine(), 1, out);
    EXPECT_EQ(704u, ctx->gain.remainingFrames);
    harkentest::PumpAndCapture(session->Engine(), 2, out);
    EXPECT_EQ(192u, ctx->gain.remainingFrames);
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    EXPECT_EQ(0u, ctx->gain.remainingFrames);

    const std::size_t rampStart = kBlockFrames;
    const std::size_t rampLen = 960;
    for (std::size_t n = 0; n < rampLen; ++n) {
        const float expected =
            1.0f + (0.25f - 1.0f) * static_cast<float>(n + 1) /
                       static_cast<float>(rampLen);
        ASSERT_NEAR(expected, out.left[rampStart + n], 1e-4f)
            << "ramp sample " << n;
    }
    float targetLinear = 0.0f;
    ASSERT_TRUE(harken::TryVolumeGainFromDb(kQuarterDb, &targetLinear));
    ASSERT_EQ(targetLinear, out.left[rampStart + rampLen - 1]);
    const float wholeBlockEnd =
        1.0f + (targetLinear - 1.0f) * 960.0f / 1024.0f;
    ASSERT_NE(targetLinear, wholeBlockEnd);
    for (std::size_t n = rampStart + rampLen; n < out.left.size(); ++n) {
        ASSERT_NEAR(0.25f, out.left[n], 1e-5f) << "plateau sample " << n;
    }
}

TEST_F(VoiceRampGolden, ZeroRampSnapsAtNextBlockBoundary) {
    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    // rampMs 0: the whole next block renders at the new value — the
    // first sample already carries it, with no intra-block ramp.
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, kHalfDb, 0));
    harkentest::PumpAndCapture(session->Engine(), 1, out);

    for (std::size_t n = 0; n < kBlockFrames; ++n) {
        ASSERT_NEAR(0.5f, out.left[kBlockFrames + n], 1e-5f)
            << "snapped sample " << n;
    }
    // Constant gain over DC: the block is exactly flat.
    ASSERT_EQ(out.left[kBlockFrames], out.left[2 * kBlockFrames - 1]);
}

TEST_F(VoiceRampGolden, RampComposesOnTheAuthoredBaseNotOverwritesIt) {
    // The setter's dB value is an API-direct contribution added to the
    // authored base. A fresh voice at -6 dB base receiving a 0 dB
    // API-direct write must stay at -6 dB — and a +6 dB write must land
    // on unity.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    session->Engine()->nullBackend.Pump(1);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -6.0206f); // gain 0.5
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    ASSERT_NEAR(0.5f, out.left[0], 1e-5f);

    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 0.0f, 0));
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    ASSERT_NEAR(0.5f, out.left[kBlockFrames], 1e-5f)
        << "0 dB API-direct must not overwrite the -6 dB base";

    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 6.0206f, 0));
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    ASSERT_NEAR(1.0f, out.left[2 * kBlockFrames], 1e-4f)
        << "+6 dB API-direct on a -6 dB base must land on unity";
}

} // namespace
