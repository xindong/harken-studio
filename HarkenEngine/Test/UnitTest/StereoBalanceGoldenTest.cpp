// Golden case: the constant-gain 2D stereo balance law at the bus fold-in for a
// non-spatialized voice, proven on the output buffers.
//
// A non-spatialized stereo source folds into the stereo pair under
//   gainL = min(1, 1 - pan),  gainR = min(1, 1 + pan),
// so a centered voice keeps unity on both channels (byte identical to the plain
// channel mapping) and a full pan mutes the opposite output while leaving its
// own untouched. This is distinct from the equal-power pan law used by the
// mono equal-power fold. Width is not
// consumed by this law.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::InterleavedDesc;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr float kAmp = 0.25f;
constexpr VnSoundId kSound = harkentest::ObjectId(1);

bool BytesEqual(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(),
                       a.size() * sizeof(float)) == 0;
}

// A voice that never receives a pan command and one that receives an explicit
// centered pan (p == 0) must both fold through the plain channel mapping, byte
// for byte — the balance branch is only taken when the pair leaves unity.
TEST(StereoBalanceGolden, ZeroPanCommandIsByteIdenticalToNoPanCommand) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);

    Capture noPan;
    {
        EngineSession session(MakeNullConfig(2, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
        VnPlayDesc play = MakePlayDesc(kSound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpAndCapture(session.Engine(), 3, noPan);
    }

    Capture zeroPan;
    {
        EngineSession session(MakeNullConfig(2, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
        VnPlayDesc play = MakePlayDesc(kSound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_Update());
        // Render one block so the command lands on a live voice, then apply the
        // centered pan: the balance stays at unity and the remaining blocks fold
        // through the identical plain mapping.
        PumpAndCapture(session.Engine(), 1, zeroPan);
        ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, 0.0f));
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpAndCapture(session.Engine(), 2, zeroPan);
    }

    // A never-panned mono voice duplicates the source into both channels.
    HARKENTEST_ASSERT_NON_SILENT(noPan.left);
    EXPECT_TRUE(BytesEqual(noPan.left, noPan.right))
        << "centered mono fold is not symmetric";
    // The explicit p=0 command must not perturb a single bit of the mix.
    EXPECT_TRUE(BytesEqual(noPan.left, zeroPan.left))
        << "left channel diverged from the no-command path";
    EXPECT_TRUE(BytesEqual(noPan.right, zeroPan.right))
        << "right channel diverged from the no-command path";
}

// A hard-right pan on a stereo source mutes the left output exactly and passes
// the right source channel through bit-for-bit (gainR == min(1, 2) == 1).
TEST(StereoBalanceGolden, FullRightPanMutesLeftAndPassesRightBitExact) {
    constexpr float kLeftSample = 0.3f;
    constexpr float kRightSample = 0.7f;
    std::vector<float> stereo(2 * static_cast<std::size_t>(kRate));
    for (std::size_t frame = 0; frame < kRate; ++frame) {
        stereo[2 * frame] = kLeftSample;
        stereo[2 * frame + 1] = kRightSample;
    }

    EngineSession session(MakeNullConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnPCMDesc desc = InterleavedDesc(stereo, 2, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));

    VnPlayDesc play = MakePlayDesc(kSound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    // Start centered so the pan is a live retarget, then settle past the 10 ms
    // (480-frame) smoothing window before reading the fully-resolved balance.
    Capture warmup;
    PumpAndCapture(session.Engine(), 1, warmup);
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, 1.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture settled;
    PumpAndCapture(session.Engine(), 3, settled);

    const std::size_t total = settled.left.size();
    ASSERT_EQ(settled.right.size(), total);
    ASSERT_GE(total, static_cast<std::size_t>(kBlock));
    ASSERT_GT(std::fabs(kRightSample), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    for (std::size_t i = total - kBlock; i < total; ++i) {
        EXPECT_EQ(0.0f, settled.left[i]);
        EXPECT_EQ(kRightSample, settled.right[i]);
    }
}

TEST(StereoBalanceGolden, NegativeAndIntermediatePanMatchExactBalanceLaw) {
    struct PanCase {
        float pan;
        float expectedLeft;
        float expectedRight;
    };
    const PanCase cases[] = {
        {-1.0f, kAmp, 0.0f},
        {0.5f, 0.125f, kAmp},
    };
    const std::vector<float> dc = MakeDC(2 * kRate, kAmp);

    for (const PanCase& testCase : cases) {
        EngineSession session(MakeNullConfig(2, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        const VnPCMDesc desc = InterleavedDesc(dc, 2, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
        VnPlayDesc play = MakePlayDesc(kSound);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        ASSERT_EQ(VN_OK, Vn_Update());
        Capture warmup;
        PumpAndCapture(session.Engine(), 1, warmup);
        ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, testCase.pan));
        ASSERT_EQ(VN_OK, Vn_Update());
        Capture settled;
        PumpAndCapture(session.Engine(), 3, settled);

        ASSERT_EQ(settled.left.size(), settled.right.size());
        ASSERT_GE(settled.left.size(), static_cast<std::size_t>(kBlock));
        const std::size_t first = settled.left.size() - kBlock;
        for (std::size_t i = first; i < settled.left.size(); ++i) {
            EXPECT_EQ(settled.left[i], testCase.expectedLeft)
                << "pan " << testCase.pan << " frame " << i;
            EXPECT_EQ(settled.right[i], testCase.expectedRight)
                << "pan " << testCase.pan << " frame " << i;
        }
    }
}

// A live pan change eases the balance across the smoothing window rather than
// snapping: the first ramped frame still sits at the pre-change level, the
// block ends lower, and no single sample step jumps.
TEST(StereoBalanceGolden, LivePanChangeRampsWithoutInstantJump) {
    const std::vector<float> dc = MakeDC(2 * kRate, kAmp);
    EngineSession session(MakeNullConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnPCMDesc desc = InterleavedDesc(dc, 2, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));

    VnPlayDesc play = MakePlayDesc(kSound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture before;
    PumpAndCapture(session.Engine(), 1, before);
    ASSERT_FALSE(before.left.empty());
    EXPECT_NEAR(kAmp, before.left.back(), 1e-6f);

    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, 1.0f));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture ramp;
    PumpAndCapture(session.Engine(), 1, ramp);
    ASSERT_EQ(static_cast<std::size_t>(kBlock), ramp.left.size());

    // Eases in from the current left level and ends lower within one block.
    EXPECT_NEAR(kAmp, ramp.left.front(), 1e-6f);
    EXPECT_LT(ramp.left.back(), ramp.left.front());
    float maxStep = 0.0f;
    for (std::size_t i = 1; i < ramp.left.size(); ++i) {
        maxStep =
            std::max(maxStep, std::fabs(ramp.left[i] - ramp.left[i - 1]));
    }
    // A 10 ms ramp of a unit change spreads each step far below this bound.
    EXPECT_LT(maxStep, 0.01f);
    // min(1, 1 + pan) holds at unity for pan >= 0, so the right output is
    // unaffected by a rightward pan.
    for (float sample : ramp.right) {
        EXPECT_NEAR(kAmp, sample, 1e-6f);
    }
}

}  // namespace
