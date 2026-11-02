// Bus mix-wide LPF/HPF (S2) and Direct-only output filter (S3): default
// bypass, Send follows S2 but not S3, incoming sends take the destination
// bus S2, and refusals have zero side effects.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/CommandQueue.h"
#include "harken/dsp/Biquad.h"

#include <cmath>
#include <limits>
#include <memory>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlockFrames = 64;
constexpr VnBusId kBusA = harkentest::ObjectId(10);
constexpr VnBusId kBusB = harkentest::ObjectId(11);
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::vector<float> RegisterDc(float value) {
    std::vector<float> dc = MakeDC(48000, value);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    return dc;
}

TEST(BusFilter, DefaultAndDisableMatchUnfiltered) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture before;
    PumpAndCapture(session.Engine(), 2, before);
    ASSERT_NEAR(0.5f, before.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusA, harken::kFilterKindHighpass,
                                     1000.0f, 0.707f, 0, 0));
    Capture disabled;
    PumpAndCapture(session.Engine(), 4, disabled);
    ASSERT_NEAR(0.5f, disabled.left.back(), 1e-4f);
}

TEST(BusFilter, MixFilterChangesDirectAndSend) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture both;
    PumpAndCapture(session.Engine(), 2, both);
    ASSERT_NEAR(1.0f, both.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -120.0f, 0));
    Capture sendOnly;
    PumpAndCapture(session.Engine(), 2, sendOnly);
    ASSERT_NEAR(0.5f, sendOnly.left.back(), 1e-4f);
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, 0.0f, 0));

    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusA, harken::kFilterKindHighpass,
                                     1000.0f, 0.707f, 1, 0));
    Capture filteredBoth;
    PumpAndCapture(session.Engine(), 8, filteredBoth);
    EXPECT_LT(std::fabs(filteredBoth.left.back()), 0.05f);

    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -120.0f, 0));
    Capture filteredSend;
    PumpAndCapture(session.Engine(), 4, filteredSend);
    EXPECT_LT(std::fabs(filteredSend.left.back()), 0.05f);
}

TEST(BusFilter, IncomingSendTakesDestinationMixFilter) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusB, kBusA, 0.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusB;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusB, -120.0f, 0));
    Capture throughA;
    PumpAndCapture(session.Engine(), 2, throughA);
    ASSERT_NEAR(0.5f, throughA.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusA, harken::kFilterKindHighpass,
                                     1000.0f, 0.707f, 1, 0));
    Capture filtered;
    PumpAndCapture(session.Engine(), 8, filtered);
    EXPECT_LT(std::fabs(filtered.left.back()), 0.05f);
}

TEST(BusFilter, RefusalsHaveZeroSideEffects) {
    VnConfig config = MakeNullConfig(2, kBlockFrames, 48000);
    config.maxBuses = 4;
    config.commandQueueDepth = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    EXPECT_EQ(VN_UNKNOWN_ID,
              Vn_SetBusFilter(harkentest::ObjectId(99), harken::kFilterKindLowpass, 800.0f, 0.707f,
                              1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusFilter(kBusA, 2, 800.0f, 0.707f, 1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusFilter(kBusA, harken::kFilterKindLowpass, kNaN, 0.707f,
                              1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusFilter(kBusA, harken::kFilterKindLowpass, 800.0f, kNaN,
                              1, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusFilter(kBusA, harken::kFilterKindLowpass, 800.0f, 0.707f,
                              2, 0));
    Capture stillOpen;
    PumpAndCapture(session.Engine(), 2, stillOpen);
    ASSERT_NEAR(0.5f, stillOpen.left.back(), 1e-4f);

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
    }
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetBusFilter(kBusA, harken::kFilterKindHighpass, 1000.0f,
                              0.707f, 1, 0));
    session.Engine()->nullBackend.Pump(1);
    Capture afterRefuse;
    PumpAndCapture(session.Engine(), 2, afterRefuse);
    ASSERT_NEAR(0.5f, afterRefuse.left.back(), 1e-4f);

    EXPECT_EQ(VN_OK, Vn_SetBusFilter(kBusA, harken::kFilterKindHighpass,
                                     1000.0f, 0.707f, 1, 0));
    Capture accepted;
    PumpAndCapture(session.Engine(), 8, accepted);
    EXPECT_LT(std::fabs(accepted.left.back()), 0.05f);
}

TEST(BusFilter, OutputFilterChangesDirectNotSend) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture both;
    PumpAndCapture(session.Engine(), 2, both);
    ASSERT_NEAR(1.0f, both.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetBusOutputFilter(kBusA, harken::kFilterKindHighpass,
                                           1000.0f, 0.707f, 1, 0));
    Capture after;
    PumpAndCapture(session.Engine(), 8, after);
    EXPECT_NEAR(0.5f, after.left.back(), 5e-2f);

    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -120.0f, 0));
    Capture sendOnly;
    PumpAndCapture(session.Engine(), 4, sendOnly);
    ASSERT_NEAR(0.5f, sendOnly.left.back(), 1e-4f);
}

TEST(BusFilter, OutputFilterMatchesOutputVolumeSendIsolation) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture warm;
    PumpAndCapture(session.Engine(), 2, warm);
    ASSERT_NEAR(1.0f, warm.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -120.0f, 0));
    Capture volumeMutedDirect;
    PumpAndCapture(session.Engine(), 2, volumeMutedDirect);
    ASSERT_NEAR(0.5f, volumeMutedDirect.left.back(), 1e-4f);
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, 0.0f, 0));

    ASSERT_EQ(VN_OK, Vn_SetBusOutputFilter(kBusA, harken::kFilterKindHighpass,
                                           1000.0f, 0.707f, 1, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -120.0f, 0));
    Capture filterMutedDirect;
    PumpAndCapture(session.Engine(), 8, filterMutedDirect);
    ASSERT_NEAR(0.5f, filterMutedDirect.left.back(), 1e-4f);
}

TEST(BusFilter, MasterOutputFilterAppliesAfterSendScatter) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture before;
    PumpAndCapture(session.Engine(), 2, before);
    ASSERT_NEAR(0.5f, before.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK,
              Vn_SetBusOutputFilter(VN_BUS_MASTER, harken::kFilterKindHighpass,
                                    1000.0f, 0.707f, 1, 0));
    Capture filtered;
    PumpAndCapture(session.Engine(), 8, filtered);
    EXPECT_LT(std::fabs(filtered.left.back()), 0.05f);
}

// A live bus (already pumped DC) must not snap the first HPF install even
// at rampMs=0. Hard-cut HPF kills DC inside one 64-frame block; the Voice
// post-firstBlock dry→wet span keeps the first block mostly dry, then
// settles after the smoothing window.
void ExpectLiveFirstHpfCutCrossfades(
    VnResult (*setFilter)(VnBusId, std::uint32_t, float, float, std::uint32_t,
                          std::uint32_t)) {
    constexpr std::uint32_t kRate = 48000;
    VnConfig config = MakeNullConfig(2, kBlockFrames, kRate);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), 0.0f);
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture live;
    PumpAndCapture(session.Engine(), 4, live);
    ASSERT_NEAR(0.5f, live.left.back(), 1e-4f);

    ASSERT_EQ(VN_OK, setFilter(kBusA, harken::kFilterKindHighpass, 1000.0f,
                              0.707f, 1, 0));
    Capture first;
    PumpAndCapture(session.Engine(), 1, first);
    ASSERT_EQ(kBlockFrames, first.left.size());
    EXPECT_GT(std::fabs(first.left.back()), 0.2f)
        << "first live HPF block must not already be at HPF DC null";

    const std::uint32_t smoothingFrames =
        harken::BiquadSmoothingFrames(kRate);
    ASSERT_GT(smoothingFrames, kBlockFrames);
    const std::uint32_t settleBlocks =
        (smoothingFrames + kBlockFrames - 1u) / kBlockFrames;
    Capture settled;
    PumpAndCapture(session.Engine(), settleBlocks, settled);
    EXPECT_LT(std::fabs(settled.left.back()), 0.05f);
}

TEST(BusFilter, LiveMixFirstEnableDoesNotSnapHpf) {
    ASSERT_NO_FATAL_FAILURE(ExpectLiveFirstHpfCutCrossfades(&Vn_SetBusFilter));
}

TEST(BusFilter, LiveOutputFirstEnableDoesNotSnapHpf) {
    ASSERT_NO_FATAL_FAILURE(
        ExpectLiveFirstHpfCutCrossfades(&Vn_SetBusOutputFilter));
}

TEST(BusFilter, LargeFiniteGainDoesNotPoisonLaterAudio) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> samples(8192, 1.0f);
    for (std::size_t i = kBlockFrames; i < samples.size(); ++i) {
        samples[i] = 0.25f * std::sin(static_cast<float>(i) *
                                    6.283185307179586f * 1000.0f / 48000.0f);
    }
    const auto desc = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetBusFilter(VN_BUS_MASTER, harken::kFilterKindHighpass,
                                    1000.0f, 0.707f, 1, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 768.0f, 0));
    Capture loud;
    PumpAndCapture(session.Engine(), 1, loud);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0));
    Capture recovered;
    PumpAndCapture(session.Engine(), 64, recovered);
    double energy = 0.0;
    for (std::size_t i = recovered.left.size() - 768; i < recovered.left.size(); ++i) {
        ASSERT_TRUE(std::isfinite(recovered.left[i]));
        energy += static_cast<double>(recovered.left[i]) * recovered.left[i];
    }
    // A 0.25-amplitude sine at the Butterworth corner has RMS 0.125.
    EXPECT_NEAR(0.125, std::sqrt(energy / 768.0), 0.002);
}

TEST(BusFilter, FiniteResampledExtremesDoNotPoisonLaterAudio) {
    EngineSession session(MakeNullConfig(2, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> samples(8192);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = i < 32 ? (i % 2 ? -2.0e38f : 2.0e38f)
            : 0.25f * std::sin(static_cast<float>(i) *
                              6.283185307179586f * 2000.0f / 48000.0f);
    }
    const auto desc = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto play = MakePlayDesc(harkentest::ObjectId(1), -24.0f);
    play.pitchRatio = 0.5f;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetBusFilter(VN_BUS_MASTER, harken::kFilterKindHighpass,
                                   1000.0f, 0.707f, 1, 0));
    Capture recovered;
    PumpAndCapture(session.Engine(), 64, recovered);
    double energy = 0.0;
    for (std::size_t i = recovered.left.size() - 768; i < recovered.left.size(); ++i) {
        ASSERT_TRUE(std::isfinite(recovered.left[i]));
        energy += static_cast<double>(recovered.left[i]) * recovered.left[i];
    }
    // Half-speed linear interpolation and -24 dB gain preserve the later tone.
    EXPECT_NEAR(0.125 * std::pow(10.0, -24.0 / 20.0), std::sqrt(energy / 768.0), 0.0002);
}

} // namespace
