// Golden cases: DC through the bus tree with fixed (snapped) bus gains —
// a two-layer tree, a three-layer chain whose gains multiply level by
// level, and the master bus's own gain over the final mix.
//
// Tolerances:
//   - Each bus level contributes one dB -> linear conversion (pow/log10
//     round trip, relative error ~1e-7) and one float multiply per level.
//     For signals <= 1.0 through <= 2 gain stages the accumulated absolute
//     error stays below 1e-6; tolerance 1e-5 leaves an order of magnitude
//     of headroom while still failing on any wrong gain topology (a missed
//     or doubled level shifts the output by a factor of 2 here).
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;

// 20*log10(0.5): requests exactly half amplitude.
const float kHalfDb = 20.0f * std::log10(0.5f);

TEST(BusMixGolden, TwoLayerTreeGainScalesRoutedVoice) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));

    // The first volume ever set on a bus snaps, so the whole first block
    // renders at the fixed gain.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), kHalfDb, 0));

    const std::vector<float> dc = harkentest::MakeDC(kBlockFrames, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(10);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    // DC 0.7 through the bus at half gain into the master at unity.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(0.35f, out.left[i], 1e-5f) << "left sample " << i;
        ASSERT_NEAR(0.35f, out.right[i], 1e-5f) << "right sample " << i;
    }
}

TEST(BusMixGolden, ThreeLayerChainGainsMultiplyPerLevel) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(11), harkentest::ObjectId(10)));

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), kHalfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(11), kHalfDb, 0));

    const std::vector<float> dc = harkentest::MakeDC(kBlockFrames, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(11); // the leaf of master <- 10 <- 11
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    // 0.8 * 0.5 (bus 11) * 0.5 (bus 10) * 1.0 (master) = 0.2: each level
    // of the chain multiplies exactly once.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(0.2f, out.left[i], 1e-5f) << "sample " << i;
    }
}

TEST(BusMixGolden, MasterVolumeScalesFinalMix) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, kHalfDb, 0));

    const std::vector<float> dc = harkentest::MakeDC(kBlockFrames, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(0.3f, out.left[i], 1e-5f) << "sample " << i;
    }
}

} // namespace
