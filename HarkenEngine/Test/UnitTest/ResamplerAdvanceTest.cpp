#include "harken/core/ResamplerAdvance.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

harken::ResamplerAdvanceState UnityPass(std::uint64_t frames,
                                       std::uint64_t start = 0) {
    harken::ResamplerAdvanceState state{};
    state.pcmFrameCount = frames;
    state.loopEndFrame = frames;
    state.absoluteStartFrame = start;
    state.loopsRemaining = 1;
    return state;
}

TEST(ResamplerAdvance, NaturalEndAtUnityRateUsesRemainingSource) {
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        UnityPass(4096, 100), harken::ResamplerBoundaryKind::NaturalEnd, 100,
        &result));
    EXPECT_TRUE(result.available);
    EXPECT_EQ(4196u, result.outputFrame);
}

TEST(ResamplerAdvance, NaturalEndAtDoubleRateHalvesOutputFrames) {
    auto state = UnityPass(4096, 0);
    state.currentRatio = 2.0;
    state.targetRatio = 2.0;
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::NaturalEnd, 0, &result));
    EXPECT_TRUE(result.available);
    EXPECT_EQ(2048u, result.outputFrame);
}

TEST(ResamplerAdvance, NaturalEndAccountsForInFlightRatioRamp) {
    auto state = UnityPass(20, 0);
    state.currentRatio = 1.0;
    state.targetRatio = 2.0;
    state.rampStep = 0.25;
    state.rampRemaining = 4;
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::NaturalEnd, 0, &result));
    EXPECT_TRUE(result.available);
    EXPECT_EQ(11u, result.outputFrame);
}

TEST(ResamplerAdvance, CurrentLoopEndSkipsPassesAfterDelayedFrame) {
    harken::ResamplerAdvanceState state{};
    state.pcmFrameCount = 100;
    state.loopStartFrame = 0;
    state.loopEndFrame = 100;
    state.loopInfinite = true;
    state.loopsRemaining = 0;
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::CurrentLoopEnd, 250, &result));
    EXPECT_TRUE(result.available);
    EXPECT_EQ(300u, result.outputFrame);
}

TEST(ResamplerAdvance, CurrentLoopEndBeforeFirstBoundaryStaysOnFirstPass) {
    harken::ResamplerAdvanceState state{};
    state.pcmFrameCount = 100;
    state.loopEndFrame = 100;
    state.loopInfinite = true;
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::CurrentLoopEnd, 40, &result));
    EXPECT_TRUE(result.available);
    EXPECT_EQ(100u, result.outputFrame);
}

TEST(ResamplerAdvance, ExitMarkerDropsWhenMissing) {
    auto state = UnityPass(4096);
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::ExitMarker, 0, &result));
    EXPECT_FALSE(result.available);
}

TEST(ResamplerAdvance, ExitMarkerUsesNextPassWhenBehindCursor) {
    harken::ResamplerAdvanceState state{};
    state.pcmFrameCount = 100;
    state.loopStartFrame = 0;
    state.loopEndFrame = 100;
    state.loopInfinite = true;
    state.hasExitMarker = true;
    state.exitMarkerSourceFrame = 25;
    state.sourceIntegerFrame = 40;
    harken::ResamplerBoundaryResult result{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::ExitMarker, 0, &result));
    EXPECT_TRUE(result.available);
    EXPECT_EQ(85u, result.outputFrame);
}

TEST(ResamplerAdvance, FiniteLastPassCurrentLoopEndEqualsNaturalEnd) {
    harken::ResamplerAdvanceState state{};
    state.pcmFrameCount = 120;
    state.loopStartFrame = 20;
    state.loopEndFrame = 80;
    state.loopsRemaining = 1;
    state.sourceIntegerFrame = 20;
    harken::ResamplerBoundaryResult loop{};
    harken::ResamplerBoundaryResult natural{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::CurrentLoopEnd, 0, &loop));
    ASSERT_TRUE(harken::ResolveResamplerBoundary(
        state, harken::ResamplerBoundaryKind::NaturalEnd, 0, &natural));
    EXPECT_TRUE(loop.available);
    EXPECT_TRUE(natural.available);
    EXPECT_EQ(natural.outputFrame, loop.outputFrame);
    EXPECT_EQ(100u, loop.outputFrame);
}

TEST(ResamplerAdvance, NextBoundariesMatchResolver) {
    auto state = UnityPass(4096, 50);
    std::uint64_t loopEnd = 0;
    std::uint64_t exitMarker = 0;
    ASSERT_TRUE(harken::ComputeNextResamplerBoundaries(state, &loopEnd, &exitMarker));
    EXPECT_EQ(4146u, loopEnd);
    EXPECT_EQ(UINT64_MAX, exitMarker);
}

} // namespace
