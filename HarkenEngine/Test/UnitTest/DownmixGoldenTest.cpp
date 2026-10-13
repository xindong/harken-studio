// Golden cases for the master->device output downmix (spec M5 §6.3 boundary 1,
// Appendix A default tables): with the mix layout wider than the device layout,
// the master's default matrix folds the final mix to the device once per block.
// A multichannel DC source with a distinct level per channel maps identity into
// the mix (channel c carries v[c]); the captured device channels must equal the
// closed-form fold of Appendix A.
//
// The -3 dB coefficient is 1/sqrt(2); the 7.1 side/back fold to mono composes to
// 0.5/sqrt(2). Tolerance 1e-5: a small fixed number of multiply-adds on an exact
// DC path.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using harkentest::CaptureMulti;
using harkentest::EngineSession;
using harkentest::InterleavedDesc;
using harkentest::MakeNullConfigLayout;

constexpr std::uint32_t kBlockFrames = 256;
constexpr float kTol = 1e-5f;

const float kInv2 = 1.0f / std::sqrt(2.0f);       // -3 dB
const float kHalfInv2 = 0.5f / std::sqrt(2.0f);   // 7.1 side/back -> mono

// Captures a multichannel source at the device boundary, optionally using a
// custom master downmix matrix. The returned sample is from the steady block.
std::vector<float> CaptureAtBoundary(
    std::uint32_t mixLayout, std::uint32_t deviceLayout,
    const std::vector<float>& data, std::uint32_t srcChannels,
    const VnDownmixMatrixDesc* matrix = nullptr,
    CaptureMulti* captured = nullptr,
    const VnPlayDesc* playDesc = nullptr,
    const float* postRegistrationPcmValue = nullptr) {
    EngineSession session(
        MakeNullConfigLayout(2, kBlockFrames, 48000, mixLayout, deviceLayout));
    EXPECT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> registeredData = data;
    if (postRegistrationPcmValue != nullptr) {
        std::fill(registeredData.begin(), registeredData.end(), 0.0f);
    }
    const VnPCMDesc desc = InterleavedDesc(
        registeredData, srcChannels, 48000,
        postRegistrationPcmValue != nullptr ? VN_PCM_POLICY_REFERENCE
                                            : VN_PCM_POLICY_COPY);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    if (postRegistrationPcmValue != nullptr) {
        std::fill(registeredData.begin(), registeredData.end(),
                  *postRegistrationPcmValue);
    }
    if (matrix != nullptr) {
        EXPECT_EQ(VN_OK, Vn_SetBusDownmixMatrix(VN_BUS_MASTER, matrix));
    }

    VnPlayDesc play = playDesc != nullptr ? *playDesc
                                         : harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    if (play.spatialized != 0) {
        EXPECT_EQ(VN_OK, Vn_Update());
    }

    CaptureMulti cap;
    harkentest::PumpAndCaptureMulti(engine, 2, cap);
    std::vector<float> out(cap.channelCount, 0.0f);
    for (std::uint32_t ch = 0; ch < cap.channelCount; ++ch) {
        out[ch] = cap.channels[ch][kBlockFrames + kBlockFrames / 2];
    }
    if (captured != nullptr) {
        *captured = cap;
    }
    return out;
}

// Plays a `srcChannels`-channel DC source (level 0.05*(c+1)) 2D into the
// master at `mixLayout`, folds to `deviceLayout`, and returns the device
// channels' steady block-2 values.
std::vector<float> CaptureFold(std::uint32_t mixLayout,
                               std::uint32_t deviceLayout,
                               std::uint32_t srcChannels) {
    const std::uint32_t frames = 512;
    std::vector<float> data(static_cast<std::size_t>(frames) * srcChannels);
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t c = 0; c < srcChannels; ++c) {
            data[i * srcChannels + c] = 0.05f * static_cast<float>(c + 1);
        }
    }
    return CaptureAtBoundary(mixLayout, deviceLayout, data, srcChannels);
}

// v[c] is the per-channel source level the helper wrote.
float V(std::uint32_t c) { return 0.05f * static_cast<float>(c + 1); }

TEST(DownmixGolden, FiveOneToStereo) {
    const std::vector<float> o =
        CaptureFold(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_STEREO, 6);
    ASSERT_EQ(2u, o.size());
    // Lo = FL + .7071 FC + .7071 BL ; Ro = FR + .7071 FC + .7071 BR ; LFE gone.
    const float lo = V(0) + kInv2 * V(2) + kInv2 * V(4);
    const float ro = V(1) + kInv2 * V(2) + kInv2 * V(5);
    ASSERT_GT(std::fabs(o[0]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(lo, o[0], kTol);
    EXPECT_NEAR(ro, o[1], kTol);
}

TEST(DownmixGolden, FiveOneToMono) {
    const std::vector<float> o =
        CaptureFold(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_MONO, 6);
    ASSERT_EQ(1u, o.size());
    // M = .7071 FL + .7071 FR + 1.0 FC + 0.5 BL + 0.5 BR (BS.775 3/2 -> 1/0).
    const float m =
        kInv2 * V(0) + kInv2 * V(1) + 1.0f * V(2) + 0.5f * V(4) + 0.5f * V(5);
    ASSERT_GT(std::fabs(o[0]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(m, o[0], kTol);
}

TEST(DownmixGolden, SevenOneToStereo) {
    const std::vector<float> o =
        CaptureFold(VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_STEREO, 8);
    ASSERT_EQ(2u, o.size());
    // All four surround channels fold at -3 dB (BS.775 + the side extension).
    const float lo = V(0) + kInv2 * (V(2) + V(4) + V(6));
    const float ro = V(1) + kInv2 * (V(2) + V(5) + V(7));
    ASSERT_GT(std::fabs(o[0]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(lo, o[0], kTol);
    EXPECT_NEAR(ro, o[1], kTol);
}

TEST(DownmixGolden, SevenOneToFiveOne) {
    const std::vector<float> o =
        CaptureFold(VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_5_1, 8);
    ASSERT_EQ(6u, o.size());
    // Front + LFE pass through; the side pair folds into the back pair at -3 dB.
    ASSERT_GT(std::fabs(o[0]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(V(0), o[0], kTol); // FL
    EXPECT_NEAR(V(1), o[1], kTol); // FR
    EXPECT_NEAR(V(2), o[2], kTol); // FC
    EXPECT_NEAR(V(3), o[3], kTol); // LFE
    EXPECT_NEAR(kInv2 * (V(4) + V(6)), o[4], kTol); // BL = .7071(BL+SL)
    EXPECT_NEAR(kInv2 * (V(5) + V(7)), o[5], kTol); // BR = .7071(BR+SR)
}

TEST(DownmixGolden, SevenOneToMono) {
    const std::vector<float> o =
        CaptureFold(VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_MONO, 8);
    ASSERT_EQ(1u, o.size());
    // Engineering extension: 7.1->5.1 composed with 5.1->mono (0.5 * .7071).
    const float m = kInv2 * V(0) + kInv2 * V(1) + 1.0f * V(2) +
                    kHalfInv2 * (V(4) + V(5) + V(6) + V(7));
    ASSERT_GT(std::fabs(o[0]), harkentest::kSilenceFloor)
        << "silence-guard: compared signal is silent";
    EXPECT_NEAR(m, o[0], kTol);
}

TEST(DownmixGolden, DeviceEqualToMixDoesNotFold) {
    // Device layout equal to the mix: identity pass-through (no downmix), the
    // regression-preserving case for the multichannel path.
    const std::vector<float> o =
        CaptureFold(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1, 6);
    ASSERT_EQ(6u, o.size());
    for (std::uint32_t c = 0; c < 6; ++c) {
        EXPECT_NEAR(V(c), o[c], kTol) << "channel " << c;
    }
}

TEST(DownmixGolden, DeviceBoundaryClampsEveryMixerBranch) {
    constexpr float kOverRangeDc = 2.0f;
    ASSERT_GT(kOverRangeDc, 1.0f);
    const float expectedClamped = std::clamp(kOverRangeDc, -1.0f, 1.0f);

    const std::uint32_t frames = 512;
    const std::uint32_t srcChannels = 6;
    const std::vector<float> overRangeData(
        static_cast<std::size_t>(frames) * srcChannels, kOverRangeDc);
    const auto expectClamped = [&](const std::vector<float>& samples,
                                   std::size_t expectedSize, float expected) {
        ASSERT_EQ(expectedSize, samples.size());
        HARKENTEST_ASSERT_NON_SILENT(samples);
        for (float sample : samples) {
            EXPECT_NEAR(expected, sample, kTol);
        }
    };

    const std::vector<float> equal = CaptureAtBoundary(
        VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1, overRangeData,
        srcChannels);
    expectClamped(equal, 6u, expectedClamped);

    const std::vector<float> negativeData(
        static_cast<std::size_t>(frames) * srcChannels, -kOverRangeDc);
    const std::vector<float> negativeEqual = CaptureAtBoundary(
        VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1, negativeData,
        srcChannels);
    const float expectedNegativeClamped =
        std::clamp(-kOverRangeDc, -1.0f, 1.0f);
    expectClamped(negativeEqual, 6u, expectedNegativeClamped);

    VnDownmixMatrixDesc matrix{};
    matrix.structSize = sizeof(VnDownmixMatrixDesc);
    matrix.srcLayout = VN_CHANNEL_LAYOUT_5_1;
    matrix.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
    matrix.coeffs[0 * 8 + 0] = 1.0f;
    matrix.coeffs[1 * 8 + 1] = 1.0f;
    const std::vector<float> narrower = CaptureAtBoundary(
        VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_STEREO, overRangeData,
        srcChannels, &matrix);
    expectClamped(narrower, 2u, expectedClamped);

    const std::vector<float> wider = CaptureAtBoundary(
        VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_7_1, overRangeData,
        srcChannels);
    ASSERT_EQ(8u, wider.size());
    HARKENTEST_ASSERT_NON_SILENT(wider);
    for (std::uint32_t ch = 0; ch < 6; ++ch) {
        EXPECT_NEAR(expectedClamped, wider[ch], kTol);
    }
    EXPECT_FLOAT_EQ(0.0f, wider[6]);
    EXPECT_FLOAT_EQ(0.0f, wider[7]);
}

TEST(DownmixGolden, DeviceBoundarySanitizesNonFiniteEveryMixerBranch) {
    constexpr std::uint32_t frames = 512;
    const float inf = std::numeric_limits<float>::infinity();
    struct BoundaryCase {
        float boundaryInput;
        float pcmValue;
        float expected;
    };
    // Illegal start volume is refused before play. Inf/-inf PCM with a
    // legal 0 dB play still reaches the device-boundary sanitizer.
    const std::array<BoundaryCase, 6> cases = {{
        {std::numeric_limits<float>::quiet_NaN(),
         std::numeric_limits<float>::quiet_NaN(), 0.0f},
        {inf, inf, 1.0f},
        {-inf, -inf, -1.0f},
        {2.0f, 2.0f, 1.0f},
        {-2.0f, -2.0f, -1.0f},
        // Hard-left mono uses sqrt(2) relative to centered source unity.
        {0.25f, 0.25f, 0.25f * std::sqrt(2.0f)},
    }};

    const auto expectFiniteAndBounded = [](const CaptureMulti& capture,
                                           const char* branch) {
        for (std::uint32_t ch = 0; ch < capture.channelCount; ++ch) {
            for (std::size_t frame = 0; frame < capture.channels[ch].size();
                 ++frame) {
                const float sample = capture.channels[ch][frame];
                if (!std::isfinite(sample) || sample < -1.0f || sample > 1.0f) {
                    ADD_FAILURE() << branch << " channel " << ch << " frame "
                                  << frame << " escaped the device boundary: "
                                  << sample;
                    return;
                }
            }
        }
    };

    VnDownmixMatrixDesc matrix{};
    matrix.structSize = sizeof(VnDownmixMatrixDesc);
    matrix.srcLayout = VN_CHANNEL_LAYOUT_5_1;
    matrix.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
    matrix.coeffs[0 * 8 + 0] = 1.0f;
    matrix.coeffs[1 * 8 + 1] = 1.0f;

    const auto runBranch = [&](const char* branch,
                               std::uint32_t deviceLayout,
                               const VnDownmixMatrixDesc* downmix,
                               std::size_t expectedChannels,
                               std::size_t silentTailStart) {
        std::vector<float> observed;
        observed.reserve(cases.size());
        std::vector<float> data(frames);
        for (const BoundaryCase& boundaryCase : cases) {
            std::fill(data.begin(), data.end(), boundaryCase.pcmValue);
            VnPlayDesc play = harkentest::MakePlayDesc3D(
                harkentest::ObjectId(1), -10.0f, 0.0f, 0.0f);
            CaptureMulti capture;
            const float* postRegistrationPcmValue =
                std::isfinite(boundaryCase.pcmValue)
                    ? nullptr
                    : &boundaryCase.pcmValue;
            const std::vector<float> output = CaptureAtBoundary(
                VN_CHANNEL_LAYOUT_5_1, deviceLayout, data, 1, downmix, &capture,
                &play, postRegistrationPcmValue);
            ASSERT_EQ(expectedChannels, output.size());
            observed.push_back(output[0]);
            EXPECT_NEAR(boundaryCase.expected, output[0], kTol)
                << branch << " input " << boundaryCase.boundaryInput;
            for (std::size_t ch = silentTailStart; ch < expectedChannels; ++ch) {
                EXPECT_FLOAT_EQ(0.0f, output[ch]);
            }
            expectFiniteAndBounded(capture, branch);
        }
        HARKENTEST_ASSERT_NON_SILENT(observed);
    };

    runBranch("equal-width", VN_CHANNEL_LAYOUT_5_1, nullptr, 6u, 6u);
    runBranch("narrower", VN_CHANNEL_LAYOUT_STEREO, &matrix, 2u, 2u);
    runBranch("wider", VN_CHANNEL_LAYOUT_7_1, nullptr, 8u, 6u);
}

} // namespace
