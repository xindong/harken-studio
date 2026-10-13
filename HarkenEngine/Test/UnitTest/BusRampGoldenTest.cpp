// Golden cases: the bus volume ramp against its closed form. Bus Volume is
// frame-accurate OutputGain: a change from gain a to gain b over
// N = ceil(rampMs * rate / 1000) mix frames scales ramp sample n (0-based)
// by
//
//     g(n) = a + (b - a) * (n + 1) / N
//
// with the N-th sample forced to b. The first volume set on a bus snaps
// even if rampMs != 0. Later rampMs == 0 snaps at the next block.
// Do not round N up to a whole block (10ms@48k is 480, not 512;
// 20ms@48k is 960, not 1024).
//
// Tolerances:
//   - ramp curve vs closed form: 1e-4 absolute;
//   - end frame exact against TryVolumeGainFromDb;
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
constexpr double kRate = 48000.0;

const float kHalfDb = 20.0f * std::log10(0.5f);       // gain 0.5
const float kQuarterDb = 20.0f * std::log10(0.25f);   // gain 0.25

class BusRampGolden : public ::testing::Test {
protected:
    void SetUp() override {
        session = std::make_unique<EngineSession>(
            MakeNullConfig(2, kBlockFrames, 48000));
        ASSERT_EQ(VN_OK, session->InitResult());
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));

        dc = harkentest::MakeDC(48000, 1.0f);
        HARKENTEST_ASSERT_NON_SILENT(dc);
        desc = MonoDesc(dc, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    }

    void PlayThroughBus() {
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = harkentest::ObjectId(10);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    }

    std::unique_ptr<EngineSession> session;
    std::vector<float> dc;
    VnPCMDesc desc{};
};

TEST_F(BusRampGolden, RampFollowsClosedFormAndStaysStepBounded) {
    // Establish a snapped starting gain of 1.0 (first set always snaps).
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), 0.0f, 0));
    PlayThroughBus();

    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // Unity plateau before the ramp: DC 1.0 times exact 1.0.
    ASSERT_EQ(1.0f, out.left[0]);
    ASSERT_EQ(1.0f, out.left[kBlockFrames - 1]);

    // 10 ms at 48 kHz = 480 frames, not 512 whole-block frames.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), kQuarterDb, 10));
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

    harkentest::PumpAndCapture(session->Engine(), 1, out);
    for (std::size_t n = rampStart + rampLen; n < out.left.size(); ++n) {
        ASSERT_NEAR(0.25f, out.left[n], 1e-5f) << "plateau sample " << n;
    }
}

TEST_F(BusRampGolden, TwentyMsAt48kLandsOnFrame960Not1024) {
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), 0.0f, 0));
    PlayThroughBus();

    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), kQuarterDb, 20));
    harken::Engine* engine = session->Engine();
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(10));
    harken::BusTree::Bus* bus = engine->buses.BusState(index);
    ASSERT_NE(nullptr, bus);

    harkentest::PumpAndCapture(engine, 1, out);
    EXPECT_EQ(704u, bus->gain.remainingFrames);
    harkentest::PumpAndCapture(engine, 2, out);
    EXPECT_EQ(192u, bus->gain.remainingFrames);
    harkentest::PumpAndCapture(engine, 1, out);
    EXPECT_EQ(0u, bus->gain.remainingFrames);

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

TEST_F(BusRampGolden, ZeroRampSnapsAtNextBlockBoundary) {
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), 0.0f, 0));
    PlayThroughBus();

    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    // rampMs 0: the whole next block renders at the new value — the first
    // sample already carries it, with no intra-block ramp.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), kHalfDb, 0));
    harkentest::PumpAndCapture(session->Engine(), 1, out);

    for (std::size_t n = 0; n < kBlockFrames; ++n) {
        ASSERT_NEAR(0.5f, out.left[kBlockFrames + n], 1e-5f)
            << "snapped sample " << n;
    }
    // Constant gain over DC: the block is exactly flat.
    ASSERT_EQ(out.left[kBlockFrames], out.left[2 * kBlockFrames - 1]);
}

TEST_F(BusRampGolden, FirstVolumeSetSnapsEvenWithNonzeroRamp) {
    // No volume was ever set on the bus: the first set snaps regardless of
    // rampMs — there is no fictitious prior value to ramp from.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), kHalfDb, 50));
    PlayThroughBus();

    Capture out;
    harkentest::PumpAndCapture(session->Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t n = 0; n < kBlockFrames; ++n) {
        ASSERT_NEAR(0.5f, out.left[n], 1e-5f) << "sample " << n;
    }
    ASSERT_EQ(out.left[0], out.left[kBlockFrames - 1]);
}

} // namespace
