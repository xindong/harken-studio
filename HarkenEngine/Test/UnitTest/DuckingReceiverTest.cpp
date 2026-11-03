#include "harken/render/DuckingReceiver.h"
#include "harken/render/SignalBlockExchange.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

harken::DuckingPreparedParameters Parameters(std::uint32_t rate = 48000) {
    harken::DuckingPreparedParameters result;
    EXPECT_TRUE(harken::PrepareDuckingParameters(rate,
        harken::kDuckingDefaultAttackMs, harken::kDuckingDefaultReleaseMs, result));
    return result;
}

TEST(DuckingReceiver, OneWayFollowsRmsBaselineAndOneGainAcrossAllChannels) {
    harken::DuckingReceiver receiver;
    const auto params = Parameters();
    receiver.SetParameters(params);
    std::array<float, 256> mainLeft, mainRight, sideLeft, sideRight;
    std::array<double, 256> gains;
    mainLeft.fill(1); mainRight.fill(-.25f);
    sideLeft.fill(.05f); sideRight.fill(.1f);
    const float* main[]{mainLeft.data(), mainRight.data()};
    const float* side[]{sideLeft.data(), sideRight.data()};
    double expectedGain = 1;
    for (std::uint32_t block = 0; block < 200; ++block) {
        ASSERT_TRUE(receiver.Process(main, side, 2, 256, nullptr, nullptr, 0, gains.data()));
        for (std::uint32_t n = 0; n < 256; ++n) {
            const double rms = static_cast<double>(sideRight[0]) *
                std::sqrt(1 - std::pow(params.energyRise, block * 256.0 + n + 1));
            const double desired = 1 - harken::kDuckingBaselineSlope * rms;
            const double alpha = desired < expectedGain ? params.attack : params.release;
            expectedGain = desired + alpha * (expectedGain - desired);
            EXPECT_NEAR(gains[n], expectedGain, 2e-14);
        }
    }
    EXPECT_NEAR(gains.back(), 1 - harken::kDuckingBaselineSlope * sideRight[0], 2e-14);
    float* output[]{mainLeft.data(), mainRight.data()};
    harken::DuckingReceiver::Apply(output, 2, 256, gains.data());
    for (std::uint32_t n = 0; n < 256; ++n)
        EXPECT_FLOAT_EQ(mainRight[n], mainLeft[n] * -.25f);
}

TEST(DuckingReceiver, MissingSourceReleasesAndResetClearsDetectionHistory) {
    harken::DuckingReceiver receiver;
    receiver.SetParameters(Parameters());
    std::array<float, 256> samples;
    samples.fill(1);
    const float* planes[]{samples.data()};
    std::array<double, 256> gains;
    for (int block = 0; block < 20; ++block)
        ASSERT_TRUE(receiver.Process(planes, planes, 1, 256, nullptr, nullptr, 0, gains.data()));
    EXPECT_LT(gains.back(), 1e-10);
    for (int block = 0; block < 20; ++block)
        ASSERT_TRUE(receiver.Process(planes, nullptr, 1, 256, nullptr, nullptr, 0, gains.data()));
    EXPECT_NEAR(gains.back(), 1, 1e-12);
    ASSERT_TRUE(receiver.Process(planes, planes, 1, 256, nullptr, nullptr, 0, gains.data()));
    receiver.Reset();
    ASSERT_TRUE(receiver.Process(planes, nullptr, 1, 256, nullptr, nullptr, 0, gains.data()));
    for (double gain : gains) EXPECT_EQ(gain, 1);
    ASSERT_TRUE(receiver.Process(planes, planes, 1, 256, nullptr, nullptr, 0, gains.data()));
    receiver.SetParameters(Parameters(44100));
    ASSERT_TRUE(receiver.Process(planes, nullptr, 1, 256, nullptr, nullptr, 0, gains.data()));
    EXPECT_EQ(gains.front(), 1);
}

TEST(DuckingReceiver, ParametersRetainTimeConstantsAndRejectInvalidUpdates) {
    const auto p48 = Parameters();
    const auto p96 = Parameters(96000);
    EXPECT_NEAR(p48.attack, .995, 2e-9);
    EXPECT_NEAR(p48.release, .98, 2e-8);
    EXPECT_NEAR(p96.mainFall * p96.mainFall, p48.mainFall, 1e-15);
    EXPECT_NEAR(p96.attack * p96.attack, p48.attack, 1e-15);
    auto output = p48;
    EXPECT_FALSE(harken::PrepareDuckingParameters(0, 1, 1, output));
    EXPECT_FALSE(harken::PrepareDuckingParameters(48000, .09f, 1, output));
    EXPECT_FALSE(harken::PrepareDuckingParameters(48000, 501, 1, output));
    EXPECT_FALSE(harken::PrepareDuckingParameters(48000, 1, 5001, output));
    EXPECT_FALSE(harken::PrepareDuckingParameters(48000,
        std::numeric_limits<float>::quiet_NaN(), 1, output));
    EXPECT_EQ(output.attack, p48.attack);
    EXPECT_EQ(output.sampleRate, p48.sampleRate);
}

TEST(DuckingReceiver, FeedbackReferenceAgesWhileReturnPathIsAbsent) {
    harken::DuckingReceiver switched, fresh;
    switched.SetParameters(Parameters());
    fresh.SetParameters(Parameters());
    std::array<float, 256> samples;
    samples.fill(1);
    const float* planes[]{samples.data()};
    std::array<double, 256> references, switchedGains, freshGains;
    references.fill(1e10);
    ASSERT_TRUE(switched.Process(planes, nullptr, 1, 256,
        references.data(), references.data(), 1, switchedGains.data()));
    for (int block = 0; block < 40; ++block)
        ASSERT_TRUE(switched.Process(planes, nullptr, 1, 256,
            nullptr, nullptr, 0, switchedGains.data()));
    references.fill(1);
    ASSERT_TRUE(switched.Process(planes, planes, 1, 256,
        references.data(), references.data(), 1, switchedGains.data()));
    ASSERT_TRUE(fresh.Process(planes, planes, 1, 256,
        references.data(), references.data(), 1, freshGains.data()));
    EXPECT_EQ(switchedGains, freshGains);
}

TEST(DuckingReceiver, ExtremeFiniteSamplesRetainDeepGainAndIntermediateHeadroom) {
    float left[]{std::numeric_limits<float>::max(), 4};
    float* output[]{left};
    const double gain[]{1e-40, .5};
    harken::DuckingReceiver::Apply(output, 1, 2, gain);
    EXPECT_FLOAT_EQ(left[0], .034028235f);
    EXPECT_EQ(left[1], 2);
    harken::DuckingReceiver receiver;
    receiver.SetParameters(Parameters());
    float main[]{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    float side[]{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    const float* m[]{main};
    const float* s[]{side};
    double gains[2];
    ASSERT_TRUE(receiver.Process(m, s, 1, 2, nullptr, nullptr, 0, gains));
    for (const double value : gains) {
        EXPECT_TRUE(std::isfinite(value));
        EXPECT_GT(value, 0);
        EXPECT_LE(value, 1);
    }
}

// Exercise the actual transport and receiver together. This is still below
// the production mixer/member installation layer, which needs its own tests.
TEST(DuckingReceiver, MutualFeedbackSettlesWithSignedBiasAndMemberCancellation) {
    for (const auto rate : {44100u, 48000u, 96000u})
    for (const auto frames : {64u, 256u, 512u})
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(::testing::Message() << rate << '/' << frames << '/' << scenario);
        const float mainValue = scenario == 2 ? .001f : 1;
        const float tapScale = scenario == 2 ? .5005f : 1;
        const float bias = scenario == 1 ? -.7f : 0;
        const double reference = scenario == 2 ? 1.999 : 1;
        const double slope = 1 / (1 / harken::kDuckingBaselineSlope + 1);
        harken::SignalBlockExchange exchange;
        const harken::SignalConnectionInput routes[]{{0, 1, 1}, {1, 0, 1}, {2, 0, 1}, {2, 1, 1}};
        ASSERT_TRUE(exchange.Prepare({1, frames, rate, VN_CHANNEL_LAYOUT_MONO}, 3, 2, routes));
        harken::DuckingReceiver receivers[2];
        for (auto& receiver : receivers) receiver.SetParameters(Parameters(rate));
        std::vector<float> main(frames, mainValue), source(frames), external(frames, bias);
        std::vector<double> bound(frames, reference), gains(frames);
        double final[2]{};
        for (std::uint32_t block = 0; block < rate * 2 / frames; ++block) {
            ASSERT_TRUE(exchange.BeginBlock({1, 1, 1, block,
                static_cast<std::uint64_t>(block) * frames, frames}));
            for (std::uint32_t target = 0; target < 2; ++target) {
                const float* mainPlanes[]{main.data()};
                const float* sidePlanes[]{exchange.Target(target, 0)};
                ASSERT_TRUE(receivers[target].Process(mainPlanes, sidePlanes, 1, frames,
                    bound.data(), bound.data(), slope, gains.data()));
                for (std::uint32_t n = 0; n < frames; ++n)
                    source[n] = static_cast<float>(tapScale * gains[n]);
                if (block == 0 && target == 0) source[0] *= .999999f;
                const float* sourcePlanes[]{source.data()};
                ASSERT_TRUE(exchange.Capture(target, sourcePlanes, 0, frames));
                final[target] = gains.back();
            }
            const float* externalPlanes[]{external.data()};
            ASSERT_TRUE(exchange.Capture(2, externalPlanes, 0, frames));
            ASSERT_TRUE(exchange.CommitBlock());
        }
        const double expected = (1 - slope * bias / reference) /
            (1 + slope * tapScale / reference);
        EXPECT_NEAR(final[0], expected, 3e-7);
        EXPECT_NEAR(final[1], expected, 3e-7);
        EXPECT_NEAR(final[0], final[1], 3e-7);
    }
}

} // namespace
