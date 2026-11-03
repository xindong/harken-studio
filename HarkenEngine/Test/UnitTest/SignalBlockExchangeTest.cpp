#include "harken/render/SignalBlockExchange.h"

#include <gtest/gtest.h>
#include <array>
#include <limits>

namespace {

using harken::SignalBlockExchange;
using harken::SignalBlockIdentity;
using harken::SignalConnectionInput;
constexpr harken::SignalBlockFormat kFormat{2, 4, 48000, VN_CHANNEL_LAYOUT_STEREO};
SignalBlockIdentity Block(std::uint64_t epoch) { return {1, 2, 3, epoch, epoch * 4, 4}; }
using Samples = std::array<float, 4>;
bool Capture(SignalBlockExchange& exchange, std::uint32_t source,
             const Samples& left, const Samples& right,
             std::uint32_t offset = 0, std::uint32_t frames = 4) {
    const float* planes[]{left.data(), right.data()};
    return exchange.Capture(source, planes, offset, frames);
}
void ExpectPlane(const float* actual, const Samples& expected) {
    ASSERT_NE(actual, nullptr);
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_FLOAT_EQ(expected[i], actual[i]) << i;
}

TEST(SignalBlockExchange, SignedMixReadsOneCompleteBlockAndPreservesSource) {
    SignalBlockExchange exchange;
    const SignalConnectionInput routes[]{{0, 0, .5f}, {1, 0, 1}, {0, 1, 2}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 2, 2, routes));
    const Samples a{2, -4, 6, -8}, b{-1, 2, -3, 4};
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    ExpectPlane(exchange.Target(0, 0), {});
    ASSERT_TRUE(Capture(exchange, 0, a, a));
    ASSERT_TRUE(Capture(exchange, 1, b, b));
    ExpectPlane(exchange.Target(0, 0), {});
    EXPECT_FALSE(exchange.BeginBlock(Block(1)));
    EXPECT_FALSE(Capture(exchange, 0, a, a));
    ASSERT_TRUE(exchange.CommitBlock());
    EXPECT_EQ(exchange.Target(0, 0), nullptr);
    EXPECT_FALSE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(1)));
    ExpectPlane(exchange.Target(0, 0), {});
    ExpectPlane(exchange.Target(1, 0), {4, -8, 12, -16});
    ExpectPlane(exchange.Target(1, 1), {4, -8, 12, -16});
    EXPECT_EQ(a, (Samples{2, -4, 6, -8}));
    EXPECT_EQ(exchange.Target(2, 0), nullptr);
    EXPECT_EQ(exchange.Target(1, 2), nullptr);
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(2)));
    ExpectPlane(exchange.Target(1, 0), {});
}

TEST(SignalBlockExchange, SendRampAdvancesOnceAcrossChannelsConsumersAndSilence) {
    SignalBlockExchange exchange;
    const SignalConnectionInput routes[]{{0, 0, 0}, {0, 1, .5f}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 1, 2, routes));
    const Samples a{1, 1, 1, 1};
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    ASSERT_TRUE(Capture(exchange, 0, a, a));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.SetLevel(0, 1, 8));
    auto invalid = Block(1);
    invalid.frames = 5;
    EXPECT_FALSE(exchange.BeginBlock(invalid));
    EXPECT_FALSE(exchange.SetLevel(0, -1, 0));
    ASSERT_TRUE(exchange.BeginBlock(Block(1)));
    ExpectPlane(exchange.Target(0, 0), {.125f, .25f, .375f, .5f});
    ExpectPlane(exchange.Target(0, 1), {.125f, .25f, .375f, .5f});
    ExpectPlane(exchange.Target(0, 0), {.125f, .25f, .375f, .5f});
    ExpectPlane(exchange.Target(1, 0), {.5f, .5f, .5f, .5f});
    EXPECT_FALSE(exchange.SetLevel(0, 0, 4));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(2)));
    ExpectPlane(exchange.Target(0, 0), {});
    ASSERT_TRUE(Capture(exchange, 0, a, a));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(3)));
    ExpectPlane(exchange.Target(0, 0), a);
}

TEST(SignalBlockExchange, ActiveOffsetsAndStoppedFinalPrefixAreNotReplayed) {
    SignalBlockExchange exchange;
    const SignalConnectionInput routes[]{{0, 0, 1}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 1, 1, routes));
    const Samples a{1, 2, 3, 4};
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    ASSERT_TRUE(Capture(exchange, 0, a, a, 2, 2));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(1)));
    ExpectPlane(exchange.Target(0, 0), {0, 0, 3, 4});
    ASSERT_TRUE(Capture(exchange, 0, a, a, 0, 1));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(2)));
    ExpectPlane(exchange.Target(0, 0), {1, 0, 0, 0});
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(3)));
    ExpectPlane(exchange.Target(0, 0), {});
}

TEST(SignalBlockExchange, GenerationFrameAndEpochDiscontinuitiesInvalidatePreviousPcm) {
    for (int change = 0; change < 8; ++change) {
        SignalBlockExchange exchange;
        const SignalConnectionInput routes[]{{0, 0, 1}};
        ASSERT_TRUE(exchange.Prepare(kFormat, 1, 1, routes));
        const Samples a{1, 1, 1, 1};
        ASSERT_TRUE(exchange.BeginBlock(Block(0)));
        ASSERT_TRUE(Capture(exchange, 0, a, a));
        ASSERT_TRUE(exchange.CommitBlock());
        auto next = Block(1);
        switch (change) {
        case 0: ++next.world; break;
        case 1: ++next.transport; break;
        case 2: ++next.deviceFormat; break;
        case 3: ++next.epoch; break;
        case 4: --next.epoch; break;
        case 5: ++next.frameBegin; break;
        case 6: --next.frames; break;
        case 7: exchange.Invalidate(); break;
        }
        ASSERT_TRUE(exchange.BeginBlock(next));
        const auto* plane = exchange.Target(0, 0);
        for (std::uint32_t i = 0; i < next.frames; ++i) EXPECT_EQ(plane[i], 0);
    }
}

TEST(SignalBlockExchange, AbortInvalidatesOpenBorrowAndDoesNotRewindSendTime) {
    SignalBlockExchange exchange;
    const SignalConnectionInput routes[]{{0, 0, 0}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 1, 1, routes));
    ASSERT_TRUE(exchange.SetLevel(0, 1, 8));
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    exchange.AbortBlock();
    EXPECT_EQ(exchange.Target(0, 0), nullptr);
    const Samples a{1, 1, 1, 1};
    ASSERT_TRUE(exchange.BeginBlock(Block(1)));
    ExpectPlane(exchange.Target(0, 0), {});
    ASSERT_TRUE(Capture(exchange, 0, a, a));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(2)));
    ExpectPlane(exchange.Target(0, 0), a);
    exchange.Invalidate();
    EXPECT_EQ(exchange.Target(0, 0), nullptr);
    EXPECT_FALSE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(3)));
    ExpectPlane(exchange.Target(0, 0), {});
}

TEST(SignalBlockExchange, InvalidPreparationAndCapturePreserveAcceptedState) {
    SignalBlockExchange exchange;
    const SignalConnectionInput route[]{{0, 0, 1}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 1, 1, route));
    const Samples a{1, 2, 3, 4};
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    auto invalid = a;
    invalid[3] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(Capture(exchange, 0, a, invalid));
    EXPECT_FALSE(Capture(exchange, 0, a, a, 3, 2));
    EXPECT_FALSE(Capture(exchange, 1, a, a));
    ASSERT_TRUE(Capture(exchange, 0, a, a));
    EXPECT_FALSE(exchange.Prepare(kFormat, 0, 0, {}));
    ASSERT_TRUE(exchange.CommitBlock());
    auto format = kFormat;
    format.layout = VN_CHANNEL_LAYOUT_MONO;
    EXPECT_FALSE(exchange.Prepare(format, 1, 1, route));
    const SignalConnectionInput badRoute[]{{0, 1, 1}};
    EXPECT_FALSE(exchange.Prepare(kFormat, 1, 1, badRoute));
    format = kFormat;
    format.capacityFrames = UINT32_MAX;
    EXPECT_FALSE(exchange.Prepare(format, UINT32_MAX, UINT32_MAX, {}));
    EXPECT_FALSE(exchange.SetLevel(0, std::numeric_limits<float>::infinity(), 0));
    ASSERT_TRUE(exchange.BeginBlock(Block(1)));
    ExpectPlane(exchange.Target(0, 0), a);
}

TEST(SignalBlockExchange, FiniteExtremeProductsCancelBeforeFloatConversion) {
    SignalBlockExchange exchange;
    const float maximum = std::numeric_limits<float>::max();
    const SignalConnectionInput routes[]{{0, 0, maximum}, {1, 0, maximum},
        {0, 1, maximum}, {1, 2, maximum}, {2, 3, 1}, {2, 3, 1}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 3, 4, routes));
    const Samples positive{maximum, maximum, maximum, maximum};
    const Samples negative{-maximum, -maximum, -maximum, -maximum};
    const Samples two{2, 2, 2, 2};
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    ASSERT_TRUE(Capture(exchange, 0, positive, positive));
    ASSERT_TRUE(Capture(exchange, 1, negative, negative));
    ASSERT_TRUE(Capture(exchange, 2, two, two));
    ASSERT_TRUE(exchange.CommitBlock());
    ASSERT_TRUE(exchange.BeginBlock(Block(1)));
    ExpectPlane(exchange.Target(0, 0), {});
    ExpectPlane(exchange.Target(1, 0), positive);
    ExpectPlane(exchange.Target(2, 0), negative);
    ExpectPlane(exchange.Target(3, 0), {4, 4, 4, 4});
}

TEST(SignalBlockExchange, EpochAndFrameArithmeticCannotWrapIntoContinuity) {
    SignalBlockExchange exchange;
    const SignalConnectionInput routes[]{{0, 0, 1}};
    ASSERT_TRUE(exchange.Prepare(kFormat, 1, 1, routes));
    auto identity = Block(0);
    identity.epoch = UINT64_MAX;
    identity.frameBegin = UINT64_MAX - 3;
    EXPECT_FALSE(exchange.BeginBlock(identity));
    identity.frameBegin = 0;
    ASSERT_TRUE(exchange.BeginBlock(identity));
    const Samples a{1, 1, 1, 1};
    ASSERT_TRUE(Capture(exchange, 0, a, a));
    ASSERT_TRUE(exchange.CommitBlock());
    identity.epoch = 0;
    identity.frameBegin = 4;
    ASSERT_TRUE(exchange.BeginBlock(identity));
    ExpectPlane(exchange.Target(0, 0), {});
    exchange.Invalidate();
    ASSERT_TRUE(exchange.Prepare(kFormat, 0, 0, {}));
    ASSERT_TRUE(exchange.BeginBlock(Block(0)));
    EXPECT_EQ(exchange.Target(0, 0), nullptr);
    ASSERT_TRUE(exchange.CommitBlock());
}

} // namespace
