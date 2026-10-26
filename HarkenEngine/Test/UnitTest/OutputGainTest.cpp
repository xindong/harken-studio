// OutputGain frame-accurate kernel. Independent of Voice volRamp / Bus
// rampBlocks. The three PLAN_V7 §4 killed bugs stay dead: float-tile
// reanchor 1ULP retreat, DAZ-swallowed subnormal slope reverse, and
// last-sample vs current dual evaluation retarget 1ULP.
#include "harken/core/FloatMode.h"
#include "harken/dsp/GainRamp.h"

#include "Common/SignalTestSupport.h"

#include <array>
#include <bit>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using harken::OutputGain;

constexpr float kCanary = 1234.5f;

float IndependentAt(float start, float target, double step,
                    std::uint64_t total, std::uint64_t remaining,
                    std::uint32_t frame) {
    const std::uint64_t k = static_cast<std::uint64_t>(frame) + 1;
    if (remaining == 0 || k >= remaining) {
        return target;
    }
    const std::uint64_t rest = remaining - k;
    const double low = static_cast<double>(start < target ? start : target);
    const std::uint64_t distance = target >= start ? total - rest : rest;
    return static_cast<float>(low + step * static_cast<double>(distance));
}

double AbsBound(double expected) {
    return 2.0e-6 + 2.0e-6 * std::abs(expected);
}

struct Planes {
    static constexpr std::uint32_t kPad = 4;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::vector<std::vector<float>> storage;
    std::array<float*, 8> pointers{};

    void Reset(std::uint32_t channelCount, std::uint32_t frameCount,
               bool unaligned) {
        channels = channelCount;
        frames = frameCount;
        storage.assign(8, std::vector<float>(kPad + frameCount + kPad, kCanary));
        for (std::uint32_t c = 0; c < 8; ++c) {
            pointers[c] = storage[c].data() + kPad + (unaligned ? 1u : 0u);
            if (c < channelCount) {
                for (std::uint32_t i = 0; i < frameCount; ++i) {
                    pointers[c][i] = static_cast<float>(c + 1);
                }
            }
        }
    }

    float* const* Data() { return pointers.data(); }

    void ExpectCanaries(std::uint32_t usedChannels) const {
        for (std::uint32_t c = 0; c < 8; ++c) {
            EXPECT_EQ(kCanary, storage[c][0]) << "ch " << c;
            EXPECT_EQ(kCanary, storage[c].back()) << "ch " << c;
            if (c >= usedChannels) {
                for (float sample : storage[c]) {
                    EXPECT_EQ(kCanary, sample) << "unused ch " << c;
                }
            }
        }
    }
};

void ExpectMonotoneNoOvershoot(const float* samples, std::uint32_t frames,
                               float start, float target) {
    const float lo = start < target ? start : target;
    const float hi = start > target ? start : target;
    const bool up = target >= start;
    for (std::uint32_t i = 0; i < frames; ++i) {
        EXPECT_GE(samples[i], lo) << i;
        EXPECT_LE(samples[i], hi) << i;
        if (i == 0) {
            continue;
        }
        if (up) {
            EXPECT_GE(samples[i], samples[i - 1]) << i;
        } else {
            EXPECT_LE(samples[i], samples[i - 1]) << i;
        }
    }
}

TEST(OutputGain, SnapZeroFramesLandsOnTarget) {
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.5f, 0);
    EXPECT_EQ(0.5f, gain.current);
    EXPECT_EQ(0.5f, gain.target);
    EXPECT_EQ(0u, gain.remainingFrames);
    EXPECT_EQ(0.5f, gain.At(0));
    EXPECT_EQ(0.5f, gain.At(17));
}

TEST(OutputGain, SameValueDoesNotRestart) {
    OutputGain gain;
    gain.current = 0.4f;
    gain.Set(0.4f, 960);
    EXPECT_EQ(0u, gain.remainingFrames);
    EXPECT_EQ(0.4f, gain.current);
}

TEST(OutputGain, TwentyMsAt48kLandsOnTargetAtFrame960) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.95f, 960);
    ASSERT_EQ(960u, gain.remainingFrames);

    Planes planes;
    planes.Reset(1, 960, false);
    gain.Apply(planes.Data(), 1, 0, 960);
    EXPECT_EQ(0.95f, planes.pointers[0][959]);
    EXPECT_EQ(0.95f, gain.current);
    EXPECT_EQ(0u, gain.remainingFrames);
    planes.ExpectCanaries(1);
}

TEST(OutputGain, TwentyMsSplitBlocksStillHitFrame960) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.95f, 960);
    Planes planes;
    const std::uint32_t chunks[] = {256, 256, 256, 192};
    std::uint32_t cursor = 0;
    planes.Reset(1, 960, false);
    for (std::uint32_t n : chunks) {
        gain.Apply(planes.Data(), 1, cursor, n);
        cursor += n;
    }
    EXPECT_EQ(960u, cursor);
    EXPECT_EQ(0.95f, planes.pointers[0][959]);
    EXPECT_EQ(0.95f, gain.current);
    EXPECT_EQ(0u, gain.remainingFrames);
}

TEST(OutputGain, FloatTileReanchorDoesNotRetreatOneUlp) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 1.0f;
    gain.Set(2.0f, 100000ULL);
    const double maximum = 2.0;
    ASSERT_GE(gain.step, 4.0 * static_cast<double>(
                                   std::numeric_limits<float>::epsilon()) *
                             maximum);
    ASSERT_GE(gain.step,
              static_cast<double>(std::numeric_limits<float>::min()));
    ASSERT_GT(gain.remainingFrames, 121u);
    gain.remainingFrames -= 121;

    Planes planes;
    planes.Reset(8, 256, false);
    gain.Apply(planes.Data(), 8, 0, 256);
    for (std::uint32_t i = 1; i < 256; ++i) {
        EXPECT_GE(planes.pointers[7][i], planes.pointers[7][i - 1]) << i;
    }
}

TEST(OutputGain, SubnormalSlopeDoesNotReverseUnderDaz) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 1.0e-34f;
    gain.Set(0.0f, 1000000ULL);

    Planes planes;
    planes.Reset(8, 127, false);
    gain.Apply(planes.Data(), 8, 0, 127);
    for (std::uint32_t i = 1; i < 127; ++i) {
        EXPECT_LE(planes.pointers[7][i], planes.pointers[7][i - 1]) << i;
    }
}

TEST(OutputGain, LastFrameAndCurrentShareOneEvaluation) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 1.0f;
    gain.Set(2.0f, 7);

    Planes planes;
    planes.Reset(8, 4, false);
    for (std::uint32_t c = 0; c < 8; ++c) {
        for (std::uint32_t i = 0; i < 4; ++i) {
            planes.pointers[c][i] = 1.0f;
        }
    }
    gain.Apply(planes.Data(), 8, 0, 4);
    const float last = planes.pointers[7][3];
    EXPECT_EQ(last, gain.current);

    gain.Set(3.0f, 2000000000ULL);
    planes.Reset(8, 1, false);
    for (std::uint32_t c = 0; c < 8; ++c) {
        planes.pointers[c][0] = 1.0f;
    }
    gain.Apply(planes.Data(), 8, 0, 1);
    EXPECT_GE(planes.pointers[7][0], last);
}

TEST(OutputGain, AdvanceUsesTheSameLastFrameAsAt) {
    OutputGain gain;
    gain.current = 0.2f;
    gain.Set(0.8f, 100);
    const float last = gain.At(39);
    gain.Advance(40);
    EXPECT_EQ(last, gain.current);
    EXPECT_EQ(60u, gain.remainingFrames);
    EXPECT_EQ(gain.At(0), IndependentAt(gain.start, gain.target, gain.step,
                                         gain.totalFrames, gain.remainingFrames,
                                         0));
}

TEST(OutputGain, ZeroGainKeepsMultiplyAndDoesNotMemset) {
    OutputGain gain;
    gain.Set(0.0f, 0);
    Planes planes;
    planes.Reset(2, 8, false);
    planes.pointers[0][3] = std::numeric_limits<float>::infinity();
    planes.pointers[1][3] = -std::numeric_limits<float>::infinity();
    gain.Apply(planes.Data(), 2, 0, 8);
    EXPECT_TRUE(std::isnan(planes.pointers[0][3]));
    EXPECT_TRUE(std::isnan(planes.pointers[1][3]));
    EXPECT_EQ(0.0f, planes.pointers[0][0]);
}

TEST(OutputGain, UnityConstantLeavesBufferUntouched) {
    OutputGain gain;
    gain.Set(1.0f, 0);
    Planes planes;
    planes.Reset(2, 5, false);
    const float before0 = planes.pointers[0][2];
    const float before1 = planes.pointers[1][2];
    gain.Apply(planes.Data(), 2, 0, 5);
    EXPECT_EQ(before0, planes.pointers[0][2]);
    EXPECT_EQ(before1, planes.pointers[1][2]);
}

class OutputGainApply : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(OutputGainApply, SharedTrajectoryAndCanaries) {
    harken::floatmode::ScopedFlushDenormals denormals;
    const std::uint32_t channels = GetParam();
    const std::uint32_t lengths[] = {0, 1, 3, 127, 128, 129, 256};
    for (bool unaligned : {false, true}) {
        for (std::uint32_t n : lengths) {
            OutputGain gain;
            gain.current = 0.25f;
            gain.Set(0.75f, 960);
            Planes planes;
            planes.Reset(channels, n == 0 ? 4 : n, unaligned);
            const std::uint64_t remainingBefore = gain.remainingFrames;
            const float start = gain.start;
            const float target = gain.target;
            const double step = gain.step;
            const std::uint64_t total = gain.totalFrames;
            gain.Apply(planes.Data(), channels, 0, n);
            if (n == 0) {
                EXPECT_EQ(remainingBefore, gain.remainingFrames);
                planes.ExpectCanaries(channels);
                continue;
            }
            std::vector<float> trajectory(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                trajectory[i] = planes.pointers[0][i] / 1.0f;
                const float expected = IndependentAt(
                    start, target, step, total, remainingBefore, i);
                const double err = std::abs(static_cast<double>(trajectory[i]) -
                                            static_cast<double>(expected));
                const bool end = (static_cast<std::uint64_t>(i) + 1 >=
                                  remainingBefore);
                if (end) {
                    EXPECT_EQ(expected, trajectory[i]) << "n=" << n << " i=" << i;
                } else {
                    EXPECT_LE(err, AbsBound(expected)) << "n=" << n << " i=" << i;
                }
                for (std::uint32_t c = 1; c < channels; ++c) {
                    EXPECT_FLOAT_EQ(planes.pointers[c][i],
                                    trajectory[i] * static_cast<float>(c + 1))
                        << "ch " << c;
                }
            }
            ExpectMonotoneNoOvershoot(trajectory.data(), n, start, target);
            EXPECT_EQ(gain.current, IndependentAt(start, target, step, total,
                                                  remainingBefore, n - 1));
            EXPECT_EQ(n >= remainingBefore ? 0 : remainingBefore - n,
                      gain.remainingFrames);
            planes.ExpectCanaries(channels);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Channels, OutputGainApply,
                         ::testing::Values(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u));

TEST(OutputGain, FallingRampIsMonotoneAndDoesNotOvershoot) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.9f;
    gain.Set(0.1f, 200);
    Planes planes;
    planes.Reset(3, 200, true);
    gain.Apply(planes.Data(), 3, 0, 200);
    std::vector<float> trajectory(200);
    for (std::uint32_t i = 0; i < 200; ++i) {
        trajectory[i] = planes.pointers[0][i];
    }
    ExpectMonotoneNoOvershoot(trajectory.data(), 200, 0.9f, 0.1f);
    EXPECT_EQ(0.1f, trajectory[199]);
    EXPECT_EQ(0.1f, gain.current);
}

TEST(OutputGain, GainRampOneBlockDurationUnchanged) {
    harken::GainRamp ramp;
    ramp.SnapTo(0.25f);
    ramp.SetTarget(0.75f);
    std::vector<float> left(8, 1.0f);
    float* channels[] = {left.data()};
    ramp.ApplyBlock(channels, 1, 0, 8);
    EXPECT_EQ(0.75f, left[7]);
    EXPECT_EQ(0.75f, ramp.current);
}

TEST(OutputGain, FillMatchesAtAndDoesNotAdvance) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.75f, 960);
    const float current = gain.current;
    const std::uint64_t remaining = gain.remainingFrames;
    std::vector<float> dest(129, kCanary);
    gain.Fill(dest.data() + 1, 127, 0);
    EXPECT_EQ(current, gain.current);
    EXPECT_EQ(remaining, gain.remainingFrames);
    EXPECT_EQ(kCanary, dest.front());
    EXPECT_EQ(kCanary, dest.back());
    for (std::uint32_t i = 0; i < 127; ++i) {
        const float expected = gain.At(i);
        const double err = std::abs(static_cast<double>(dest[i + 1]) -
                                    static_cast<double>(expected));
        EXPECT_LE(err, AbsBound(expected)) << i;
    }
}

TEST(OutputGain, MixAddMatchesAtTimesScale) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.75f, 960);
    const float current = gain.current;
    const std::uint64_t remaining = gain.remainingFrames;
    std::vector<float> src(64, 2.0f);
    std::vector<float> dst(64, 1.0f);
    gain.MixAdd(src.data(), dst.data(), 64, 0, 0.5f);
    EXPECT_EQ(current, gain.current);
    EXPECT_EQ(remaining, gain.remainingFrames);
    for (std::uint32_t i = 0; i < 64; ++i) {
        const float expected = 1.0f + 2.0f * 0.5f * gain.At(i);
        const double err = std::abs(static_cast<double>(dst[i]) -
                                    static_cast<double>(expected));
        EXPECT_LE(err, AbsBound(expected)) << i;
    }
}

TEST(OutputGain, FillTwentyMsAt48kFrame960Exact) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.95f, 960);
    std::vector<float> dest(960, 0.0f);
    gain.Fill(dest.data(), 960, 0);
    EXPECT_EQ(0.95f, dest[959]);
    EXPECT_EQ(0.25f, gain.current);
    EXPECT_EQ(960u, gain.remainingFrames);
}

TEST(OutputGain, MixAddZeroKeepsMultiply) {
    OutputGain gain;
    gain.Set(0.0f, 0);
    const float inf = std::numeric_limits<float>::infinity();
    float src = inf;
    float dst = 1.0f;
    gain.MixAdd(&src, &dst, 1, 0);
    EXPECT_TRUE(std::isnan(dst));
}

TEST(OutputGain, FillAndMixAddMatchAtWhenRemainingExceeds32Bits) {
    harken::floatmode::ScopedFlushDenormals denormals;
    OutputGain gain;
    gain.current = 0.25f;
    gain.Set(0.75f, (1ull << 40) + 50ull);
    ASSERT_EQ((1ull << 40) + 50ull, gain.remainingFrames);
    const float current = gain.current;
    const std::uint64_t remaining = gain.remainingFrames;

    std::vector<float> filled(8, kCanary);
    gain.Fill(filled.data(), 8, 0);
    std::vector<float> src(8, 2.0f);
    std::vector<float> mixed(8, 1.0f);
    gain.MixAdd(src.data(), mixed.data(), 8, 0);
    EXPECT_EQ(current, gain.current);
    EXPECT_EQ(remaining, gain.remainingFrames);
    for (std::uint32_t i = 0; i < 8; ++i) {
        EXPECT_EQ(gain.At(i), filled[i]) << i;
        EXPECT_EQ(1.0f + 2.0f * gain.At(i), mixed[i]) << i;
    }

    // uint32(remaining-1) of this remaining is 49. 64 frames would have
    // been truncated to 49 and the tail filled with target.
    std::vector<float> wide(64, kCanary);
    gain.Fill(wide.data(), 64, 0);
    EXPECT_EQ(remaining, gain.remainingFrames);
    for (std::uint32_t i = 0; i < 64; ++i) {
        EXPECT_EQ(gain.At(i), wide[i]) << i;
        EXPECT_NE(gain.target, wide[i]) << i;
    }
}

TEST(OutputGain, MonoStereoMixPreservesSeparateRoundingAndShortUnalignedSpans) {
    std::array<float, 264> source{}, left{}, right{}, expectedLeft{}, expectedRight{};
    std::uint32_t seed = 17;
    for (auto& value : source) {
        seed = 1664525u * seed + 1013904223u;
        value = static_cast<float>(static_cast<std::int32_t>(seed >> 8) - 0x800000) / 0x800000;
    }
    source[2] = std::bit_cast<float>(std::uint32_t{1});
    source[3] = -0.0f;
    const auto verify = [&] {
        for (const auto kernel : {&harken::output_gain_detail::MixMonoToStereoScaled,
                                 harken::output_gain_detail::SelectMonoStereoMixKernel()}) {
            for (const auto offset : {0u, 1u, 3u}) {
                for (const auto frames : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 16u, 31u, 63u, 64u, 65u, 66u, 67u, 68u, 69u, 70u, 71u, 255u, 256u, 257u}) {
                    for (const auto voiceGain : {0.0f, 0.001f, 0.67f, 1.0f, -0.5f}) {
                        for (const auto scale : {0.0f, -0.0f, -1.0f, 0.7071067f, 1.0f}) {
                            for (const auto rightScale : {scale, -scale * 0.3f}) {
                                left.fill(kCanary);
                                right.fill(kCanary);
                                for (std::uint32_t i = offset; i < offset + frames; ++i) {
                                    left[i] = source[(i + 17u) % source.size()] * 0.13f;
                                    right[i] = -source[(i + 11u) % source.size()] * 0.07f;
                                }
                                expectedLeft = left;
                                expectedRight = right;
                                for (std::uint32_t i = offset; i < offset + frames; ++i) {
                                    volatile float value = voiceGain == 1.0f ? source[i] : source[i] * voiceGain;
                                    volatile float leftContribution = value * scale;
                                    volatile float rightContribution = value * rightScale;
                                    expectedLeft[i] += leftContribution;
                                    expectedRight[i] += rightContribution;
                                }
                                kernel(source.data() + offset,
                                    left.data() + offset, right.data() + offset, frames, voiceGain, scale, rightScale);
                                EXPECT_EQ(0, std::memcmp(left.data(), expectedLeft.data(), sizeof(left)))
                                    << offset << " " << frames << " " << voiceGain << " " << scale;
                                EXPECT_EQ(0, std::memcmp(right.data(), expectedRight.data(), sizeof(right)))
                                    << offset << " " << frames << " " << voiceGain << " " << scale;
                            }
                        }
                    }
                }
            }
        }
    };
#if defined(HARKEN_OUTPUT_GAIN_SSE2)
    harken::floatmode::ScopedFlushDenormals restoreMode;
    const unsigned mode = _mm_getcsr();
    for (const unsigned flush : {0u, 0x8040u}) {
        _mm_setcsr((mode & ~0x8040u) | flush);
        verify();
    }
#else
    verify();
#endif
}

} // namespace
