// Golden cases: analytic signals (impulse / sine / DC) through the full
// Voice Render Cascade on the null backend, at unity gain and at a fixed
// gain, plus multi-voice summation into the master bus.
//
// Tolerances:
//   - Unity-gain identity: exact float equality (tolerance 0). At pitch
//     ratio 1.0 the Hermite kernel reads frac == 0 and returns the input
//     sample bit-for-bit; unity gain and master summation add/multiply by
//     exact constants.
//   - Fixed-gain cases: absolute tolerance 1e-5 — the dB -> linear
//     conversion (pow/log10 round trip) is the only inexact step.
//   - Two-voice summation: absolute tolerance 1e-6 (one float add).
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr double kRate = 48000.0;
constexpr std::uint32_t kBlockFrames = 256;

class CascadeGainGolden : public ::testing::Test {
protected:
    void PlayAndCapture(const std::vector<float>& samples, float volumeDb,
                        std::uint32_t blocks, Capture& out) {
        EngineSession session(MakeNullConfig(0, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());

        const VnPCMDesc desc = MonoDesc(samples, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), volumeDb);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

        harkentest::PumpAndCapture(session.Engine(), blocks, out);

        EXPECT_EQ(0u, harken::memguard::HitCount());
    }
};

TEST_F(CascadeGainGolden, ImpulseUnityGainIsIdentity) {
    const std::vector<float> impulse = harkentest::MakeImpulse(64);
    HARKENTEST_ASSERT_NON_SILENT(impulse);

    Capture out;
    ASSERT_NO_FATAL_FAILURE(PlayAndCapture(impulse, 0.0f, 1, out));

    // Mono duplicates into both output channels at unity.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        const float expected = (i < impulse.size()) ? impulse[i] : 0.0f;
        ASSERT_EQ(expected, out.left[i]) << "left sample " << i;
        ASSERT_EQ(expected, out.right[i]) << "right sample " << i;
    }
}

TEST_F(CascadeGainGolden, SineUnityGainIsIdentity) {
    const std::vector<float> sine =
        harkentest::MakeSine(200, 1000.0, kRate, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    Capture out;
    ASSERT_NO_FATAL_FAILURE(PlayAndCapture(sine, 0.0f, 1, out));
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        const float expected = (i < sine.size()) ? sine[i] : 0.0f;
        ASSERT_EQ(expected, out.left[i]) << "left sample " << i;
    }
}

TEST_F(CascadeGainGolden, DCFixedGainScalesExactly) {
    const std::vector<float> dc = harkentest::MakeDC(300, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(dc);

    // 20*log10(0.5) dB requests exactly half amplitude.
    const float halfDb = 20.0f * std::log10(0.5f);
    Capture out;
    ASSERT_NO_FATAL_FAILURE(PlayAndCapture(dc, halfDb, 2, out));
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t i = 0; i < dc.size(); ++i) {
        ASSERT_NEAR(0.35f, out.left[i], 1e-5f) << "left sample " << i;
    }
    // Past the source end the voice is done and the mix is silent.
    for (std::size_t i = dc.size(); i < out.left.size(); ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "tail sample " << i;
    }
}

TEST_F(CascadeGainGolden, SineFixedGainScales) {
    const std::vector<float> sine =
        harkentest::MakeSine(kBlockFrames, 440.0, kRate, 0.9f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    const float halfDb = 20.0f * std::log10(0.5f);
    Capture out;
    ASSERT_NO_FATAL_FAILURE(PlayAndCapture(sine, halfDb, 1, out));
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(sine[i] * 0.5f, out.left[i], 1e-5f) << "sample " << i;
    }
}

TEST_F(CascadeGainGolden, TwoVoicesSumIntoMaster) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(kBlockFrames, 500.0, kRate, 0.4f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7));
    VnVoiceHandle h1 = 0, h2 = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h2));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(sine[i] * 2.0f, out.left[i], 1e-6f) << "sample " << i;
    }
}

} // namespace
