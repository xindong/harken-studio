#include "harken/core/ResamplerAdvance.h"
#include "harken/dsp/Resampler.h"
#include "harken/core/SourcePhase.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <vector>

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

TEST(ResamplerAdvance, FixedNaturalEndMatchesActualRenderExactly) {
    harken::ResampleTables tables;
    tables.Init();
    for (const auto sourceRate : {16000u, 44100u, 48000u}) {
        for (const auto mixRate : {44100u, 48000u}) {
            for (const float pitch : {0.75f, 1.0f, 1.137f, 1.5f}) {
                for (const auto frames : {1u, 2u, 147u, 441u, 48000u}) {
                    for (const auto passes : {1u, 3u}) {
                        for (const auto block : {1u, 64u, 257u}) {
                            SCOPED_TRACE(::testing::Message() << sourceRate << '/' << mixRate
                                << " pitch=" << pitch << " frames=" << frames
                                << " passes=" << passes << " block=" << block);
                            const auto start = frames > 2u ? frames / 3u : 0u;
                            const auto loopStart = frames > 2u ? start + 1u : start;
                            const auto loopEnd = frames > 2u ? frames - 1u : frames;
                            auto state = UnityPass(frames);
                            state.sourceStartFrame = start;
                            state.sourceIntegerFrame = start;
                            state.loopStartFrame = loopStart;
                            state.loopEndFrame = loopEnd;
                            state.loopsRemaining = passes;
                            state.currentRatio = static_cast<double>(pitch) *
                                (static_cast<double>(sourceRate) / mixRate);
                            state.targetRatio = state.currentRatio;
                            std::uint64_t predicted = 0;
                            ASSERT_TRUE(harken::FixedSourceOutputFrames(
                                static_cast<std::uint64_t>(frames - start) +
                                    (passes - 1ull) * (loopEnd - loopStart),
                                state.currentRatio, &predicted));
                            harken::ResamplerBoundaryResult boundary{};
                            ASSERT_TRUE(harken::ResolveResamplerBoundary(state,
                                harken::ResamplerBoundaryKind::NaturalEnd, 0, &boundary));
                            ASSERT_TRUE(boundary.available);
                            EXPECT_EQ(predicted, boundary.outputFrame);
                            std::vector<float> input(frames, 0.25f);
                            harken::PCMView pcm{};
                            pcm.channels[0] = input.data();
                            pcm.channelCount = 1;
                            pcm.stride = 1;
                            pcm.sampleRate = sourceRate;
                            pcm.frames = frames;
                            std::array<float, harken::kResampleFifoFrames> ring{};
                            std::array<float, harken::kResampleFifoFrames> restoredRing{};
                            float* rings[] = {ring.data()};
                            harken::Resampler resampler;
                            resampler.Reset(rings, 1, harken::ResampleQuality::Linear,
                                state.currentRatio, &tables);
                            std::array<float, 257> output{};
                            float* outputs[] = {output.data()};
                            std::uint64_t produced = 0;
                            std::uint64_t cursor = start;
                            auto remainingPasses = passes;
                            bool restored = false;
                            const std::uint64_t limit = (frames + 1ull) * passes * 4ull + 64ull;
                            while (!resampler.Exhausted() && produced < limit) {
                                const auto count = resampler.Produce(outputs, 0, block);
                                produced += count;
                                if (!resampler.Exhausted()) {
                                    auto live = state;
                                    live.sourcePhase = {resampler.phaseHigh, resampler.phaseLow};
                                    live.logicalOutputFramesProduced = produced;
                                    harken::SetResamplerConsumedPosition(&live, start, passes,
                                        resampler.SourceFrame() - harken::kResampleHistoryPad);
                                    ASSERT_TRUE(harken::ResolveResamplerBoundary(live,
                                        harken::ResamplerBoundaryKind::NaturalEnd, produced, &boundary));
                                    ASSERT_TRUE(boundary.available);
                                    EXPECT_EQ(predicted, boundary.outputFrame);
                                }
                                if (!restored && produced != 0 && block == 64u) {
                                    harken::ResamplerParkState parked{};
                                    ASSERT_TRUE(resampler.CaptureParkState(&parked));
                                    --parked.formatVersion;
                                    EXPECT_FALSE(harken::Resampler::ValidateParkState(parked));
                                    ++parked.formatVersion;
                                    float* restoredRings[] = {restoredRing.data()};
                                    ASSERT_TRUE(resampler.RestoreParkState(parked, restoredRings,
                                        1, &tables, nullptr));
                                    restored = true;
                                }
                                if (count == block) continue;
                                const auto feedEnd = remainingPasses > 1u ? loopEnd : frames;
                                const auto chunk = static_cast<std::uint32_t>(
                                    std::min<std::uint64_t>({harken::kResampleChunkFrames,
                                        feedEnd - cursor, resampler.FreeFrames()}));
                                if (chunk != 0) {
                                    resampler.PushPCM(pcm, cursor, chunk);
                                    cursor += chunk;
                                    if (cursor == feedEnd) {
                                        if (--remainingPasses != 0) cursor = loopStart;
                                        else resampler.MarkInputEnd();
                                    }
                                } else if (!resampler.Exhausted()) {
                                    resampler.PushSilence(std::min(harken::kResampleChunkFrames,
                                        resampler.FreeFrames()));
                                }
                            }
                            ASSERT_TRUE(resampler.Exhausted());
                            EXPECT_EQ(predicted, produced);
                        }
                    }
                }
            }
        }
    }
}

TEST(ResamplerAdvance, FixedBoundarySupportsLongSourcesWithoutScanning) {
    std::uint64_t frames = 0;
    ASSERT_TRUE(harken::FixedSourceOutputFrames(1ull << 60, 0.5, &frames));
    EXPECT_EQ(1ull << 61, frames);
    ASSERT_TRUE(harken::FixedSourceOutputFrames(1, 0x1p-38, &frames));
    EXPECT_EQ(1ull << 38, frames);
    EXPECT_FALSE(harken::FixedSourceOutputFrames(UINT64_MAX, 0.5, &frames));
    ASSERT_TRUE(harken::FixedSourceOutputFrames(UINT64_MAX, 1.0, &frames));
    EXPECT_EQ(UINT64_MAX, frames);
}

TEST(ResamplerAdvance, LocalBoundaryMatchesExactRenderAtOneThirdRate) {
    harken::ResampleTables tables;
    tables.Init();
    std::array<float, harken::kResampleFifoFrames> ring{};
    float* rings[] = {ring.data()};
    harken::Resampler resampler;
    const double ratio = 16000.0 / 48000.0;
    resampler.Reset(rings, 1, harken::ResampleQuality::Linear, ratio, &tables);
    const std::array<float, 1> input{0.25f};
    harken::PCMView pcm{};
    pcm.channels[0] = input.data();
    pcm.channelCount = pcm.stride = 1;
    pcm.frames = 1;
    pcm.sampleRate = 16000;
    resampler.PushPCM(pcm, 0, 1);
    resampler.MarkInputEnd();
    resampler.PushSilence(64);
    std::array<float, 64> output{};
    float* outputs[] = {output.data()};
    const auto produced = resampler.Produce(outputs, 0, 64);
    ASSERT_TRUE(resampler.Exhausted());
    EXPECT_EQ(4u, produced);
    auto state = UnityPass(1);
    state.currentRatio = state.targetRatio = ratio;
    harken::ResamplerBoundaryResult boundary{};
    ASSERT_TRUE(harken::ResolveResamplerBoundary(state,
        harken::ResamplerBoundaryKind::NaturalEnd, 0, &boundary));
    ASSERT_TRUE(boundary.available);
    EXPECT_EQ(produced, boundary.outputFrame);
}

TEST(ResamplerAdvance, ExactPhaseLowWordCarriesAcrossJumpAndStep) {
    harken::SourcePhaseStep step{};
    ASSERT_TRUE(harken::DecodeSourcePhaseStep(0x1.0000000000001p-38, &step));
    EXPECT_EQ(1ull << 26, step.fraction.high);
    EXPECT_EQ(1ull << 38, step.fraction.low);
    harken::ExactSourcePhase carryCase{UINT64_MAX - (1ull << 26), UINT64_MAX};
    EXPECT_EQ(1u, harken::StepSourcePhase(step, &carryCase));
    EXPECT_EQ(0u, carryCase.high);
    EXPECT_EQ((1ull << 38) - 1u, carryCase.low);
    for (const auto initial : {harken::ExactSourcePhase{},
                              harken::ExactSourcePhase{UINT64_MAX, UINT64_MAX}}) {
        for (const auto frames : {0u, 1u, 2u, 257u}) {
            auto phase = initial;
            std::uint64_t consumed = 0;
            for (std::uint32_t index = 0; index < frames; ++index)
                consumed += harken::StepSourcePhase(step, &phase);
            EXPECT_EQ(consumed, harken::JumpSourcePhase(step, initial, frames));
        }
    }
    std::uint64_t high = 0, low = 0, frames = 0;
    harken::MultiplySourcePhaseWords(UINT64_MAX, UINT64_MAX, &high, &low);
    EXPECT_EQ(UINT64_MAX - 1u, high);
    EXPECT_EQ(1u, low);
    ASSERT_TRUE(harken::FixedSourceOutputFrames(1, 0x1.0000000000001p-38, &frames));
    EXPECT_EQ(1ull << 38, frames);
}

} // namespace
