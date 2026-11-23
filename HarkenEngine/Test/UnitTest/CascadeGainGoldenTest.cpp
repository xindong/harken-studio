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

TEST_F(CascadeGainGolden, VolumeTransitionPreservesInitializationAndProtectsRenderedObjects) {
    for (unsigned kind = 0; kind != 4; ++kind) {
        for (bool rendered : {false, true}) {
            for (bool assigned : {false, true}) {
                for (unsigned ms : {0u, 2u, 200u, 500u}) {
                    SCOPED_TRACE(::testing::Message() << kind << "/" << rendered
                        << "/" << assigned << "/" << ms);
                    EngineSession session(MakeNullConfig(2, kBlockFrames));
                    ASSERT_EQ(VN_OK, session.InitResult());
                    auto* engine = session.Engine();
                    engine->buses.SetAutomaticSleepEnabledForTest(false);
                    const auto bus = harkentest::ObjectId(2);
                    ASSERT_EQ(VN_OK, Vn_CreateBus(bus, VN_BUS_MASTER));
                    auto samples = harkentest::MakeDC(48000, .25f);
                    auto pcm = MonoDesc(samples, 48000);
                    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
                    auto play = MakePlayDesc(harkentest::ObjectId(1));
                    play.busId = bus;
                    VnVoiceHandle voice = 0;
                    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
                    const auto set = [&](float db, unsigned duration) {
                        return kind == 0 ? Vn_SetVoiceVolume(voice, db, duration) :
                            kind == 1 ? Vn_SetVoiceOutputVolume(voice, db, duration) :
                            kind == 2 ? Vn_SetBusVolume(bus, db, duration) :
                                        Vn_SetBusOutputVolume(bus, db, duration);
                    };
                    if (assigned) ASSERT_EQ(VN_OK, set(-6, 0));
                    if (rendered) engine->nullBackend.Pump(1);
                    ASSERT_EQ(VN_OK, set(-12, ms));
                    const auto duration = rendered ? std::max(ms, 5u) :
                        kind == 2 && !assigned ? 0u : ms;
                    const unsigned frames = duration * 48u;
                    Capture out;
                    harkentest::PumpAndCapture(engine, (frames + kBlockFrames - 1) / kBlockFrames + 1, out);
                    const float initial = assigned ? std::pow(10.f, -6.f / 20.f) : 1.f;
                    const float target = std::pow(10.f, -12.f / 20.f);
                    for (std::size_t n = 0; n < out.left.size(); ++n) {
                        const double ratio = frames == 0 ? 1.0 : std::min(1.0, double(n + 1) / frames);
                        const float expected = .25f * static_cast<float>(initial + (target - initial) * ratio);
                        ASSERT_NEAR(expected, out.left[n], 1e-7f) << n;
                        ASSERT_EQ(out.left[n], out.right[n]) << n;
                    }
                    EXPECT_EQ(0u, harken::memguard::HitCount());
                }
            }
        }
    }
}

TEST_F(CascadeGainGolden, SendTransitionsFinishAtRequestedSampleInBothBlockSizes) {
    for (bool voiceSend : {false, true}) {
        for (unsigned block : {256u, 1024u}) {
            for (unsigned ms : {0u, 100u}) {
                SCOPED_TRACE(::testing::Message() << voiceSend << "/" << block << "/" << ms);
                EngineSession session(MakeNullConfig(2, block));
                ASSERT_EQ(VN_OK, session.InitResult());
                auto* engine = session.Engine();
                const auto bus = harkentest::ObjectId(2);
                ASSERT_EQ(VN_OK, Vn_CreateBus(bus, VN_BUS_MASTER));
                auto samples = harkentest::MakeDC(48000, .25f);
                auto pcm = MonoDesc(samples, 48000);
                ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
                auto play = MakePlayDesc(harkentest::ObjectId(1));
                play.busId = bus;
                VnVoiceHandle voice = 0;
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
                if (voiceSend) {
                    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(voice, -800, 0));
                    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(voice, 0, VN_BUS_MASTER, -800));
                } else {
                    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(bus, -800, 0));
                    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(bus, VN_BUS_MASTER, -800));
                }
                engine->nullBackend.Pump(1);
                ASSERT_EQ(VN_OK, voiceSend ? Vn_SetVoiceSendLevel(voice, 0, 0, ms) :
                    Vn_SetBusSendLevel(bus, VN_BUS_MASTER, 0, ms));
                const unsigned frames = std::max(ms, 5u) * 48u;
                Capture out;
                harkentest::PumpAndCapture(engine, (frames + block - 1) / block + 1, out);
                for (std::size_t n = 0; n < out.left.size(); ++n) {
                    const float expected = .25f * static_cast<float>(std::min(1.0, double(n + 1) / frames));
                    ASSERT_NEAR(expected, out.left[n], 1e-7f) << n;
                    ASSERT_EQ(out.left[n], out.right[n]) << n;
                }
                EXPECT_LT(out.left[frames - 2], .25f);
                EXPECT_FLOAT_EQ(.25f, out.left[frames - 1]);
                EXPECT_FLOAT_EQ(.25f, out.left[frames]);
                EXPECT_EQ(0u, harken::memguard::HitCount());
            }
        }
    }
}

} // namespace
