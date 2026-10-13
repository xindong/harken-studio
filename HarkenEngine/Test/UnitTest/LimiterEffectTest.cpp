// Independent sample-domain proof for the foundational Limiter. The reference
// below uses double-precision equations and owns no production helpers:
//
//   ceiling = 10^(ceilingDb / 20)
//   a = exp(-1 / (releaseSeconds * sampleRate))
//   released[n] = a * gain[n-1] + (1 - a)
//   required[n] = min(1, ceiling / linkedPeak[n])
//   gain[n] = min(required[n], released[n])
//   y[ch,n] = x[ch,n] * gain[n]
//
// Attack is therefore instantaneous in the current frame, while decreasing
// attenuation follows a one-pole release toward unity.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;

struct ReferenceLimiter {
    double ceiling = 1.0;
    double releaseCoeff = 0.0;
    double gain = 1.0;

    ReferenceLimiter(double ceilingDb, double releaseMs) {
        ceiling = std::pow(10.0, ceilingDb / 20.0);
        releaseCoeff =
            std::exp(-1.0 / (releaseMs * 0.001 * static_cast<double>(kRate)));
    }

    double Step(double linkedPeak) {
        const double released =
            releaseCoeff * gain + (1.0 - releaseCoeff);
        const double required =
            linkedPeak > ceiling ? ceiling / linkedPeak : 1.0;
        gain = std::min(required, released);
        return gain;
    }
};

struct CompressorBlockTelemetryBeforeLinearMinimum {
    float mainInputPeak = 0.0f;
    float detectorPeak = 0.0f;
    float outputPeak = 0.0f;
    float maxGainReductionDb = 0.0f;
    float endGainReductionDb = 0.0f;
    std::uint64_t detectorNonFiniteCount = 0;
    std::uint64_t detectorSaturationCount = 0;
    bool resetThisBlock = false;
    bool resetBridgeActive = false;
};

struct CompressorStateBeforeLinearMinimum {
    std::uint32_t sampleRate = 0;
    harken::CompressorParameters authoredParameters{};
    harken::CompressorPreparedParameters parameters{};
    harken::CompressorPreparedParameters targetParameters{};
    harken::CompressorPreparedParameters parameterIncrements{};
    std::uint32_t parameterTransitionFramesRemaining = 0;
    float currentGain = 1.0f;
    float lastAppliedWetGain = 1.0f;
    float resetBridgeStartGain = 1.0f;
    std::uint32_t resetBridgeTotalFrames = 0;
    std::uint32_t resetBridgeFramesRemaining = 0;
    bool hasRendered = false;
    bool resetPending = false;
    CompressorBlockTelemetryBeforeLinearMinimum telemetry{};
};

double ReferenceCompressorTargetGain(
    const harken::CompressorPreparedParameters& parameters,
    double detectorMagnitude) {
    if (!(detectorMagnitude > 0.0) ||
        parameters.reciprocalRatio >= 1.0f) {
        return 1.0;
    }
    const double inputDb = 20.0 * std::log10(detectorMagnitude);
    const double thresholdDb =
        static_cast<double>(parameters.thresholdDb);
    const double reciprocalRatio =
        static_cast<double>(parameters.reciprocalRatio);
    const double kneeDb = static_cast<double>(parameters.kneeDb);
    const double halfKnee = 0.5 * kneeDb;
    double outputDb = inputDb;
    if (kneeDb > 0.0 &&
        inputDb >= thresholdDb - halfKnee &&
        inputDb <= thresholdDb + halfKnee) {
        const double distance =
            inputDb - thresholdDb + halfKnee;
        outputDb += (reciprocalRatio - 1.0) * distance * distance /
                    (2.0 * kneeDb);
    } else if (inputDb > thresholdDb + halfKnee) {
        outputDb = thresholdDb +
                   (inputDb - thresholdDb) * reciprocalRatio;
    }
    const double reductionDb = outputDb - inputDb;
    return reductionDb < 0.0
               ? std::pow(10.0, reductionDb / 20.0)
               : 1.0;
}

float LegacyFloatCompressorTargetGain(
    const harken::CompressorPreparedParameters& parameters,
    float detectorMagnitude) {
    float targetGain = 1.0f;
    if (detectorMagnitude > 0.0f &&
        parameters.reciprocalRatio < 1.0f) {
        const float inputDb =
            20.0f * std::log10(detectorMagnitude);
        const float halfKnee = 0.5f * parameters.kneeDb;
        float outputDb = inputDb;
        if (parameters.kneeDb > 0.0f &&
            inputDb >= parameters.thresholdDb - halfKnee &&
            inputDb <= parameters.thresholdDb + halfKnee) {
            const float distance =
                inputDb - parameters.thresholdDb + halfKnee;
            outputDb +=
                (parameters.reciprocalRatio - 1.0f) *
                distance * distance / (2.0f * parameters.kneeDb);
        } else if (inputDb >
                   parameters.thresholdDb + halfKnee) {
            outputDb =
                parameters.thresholdDb +
                (inputDb - parameters.thresholdDb) *
                    parameters.reciprocalRatio;
        }
        const float reductionDb = outputDb - inputDb;
        if (reductionDb < 0.0f) {
            targetGain =
                std::pow(10.0f, reductionDb / 20.0f);
        }
    }
    return targetGain;
}

float ReferenceCompressorEnvelopeStep(
    const harken::CompressorPreparedParameters& parameters,
    float currentGain, float targetGain) {
    return targetGain < currentGain
               ? parameters.attackCoeff * currentGain +
                     parameters.attackAdd * targetGain
               : parameters.releaseCoeff * currentGain +
                     parameters.releaseAdd * targetGain;
}

void ExpectNextPublicCompressorBlockMatchesLegacyTarget(
    harken::Engine* engine, harken::CompressorState* state,
    const harken::CompressorPreparedParameters& expectedParameters,
    float detectorMagnitude) {
    ASSERT_NE(nullptr, engine);
    ASSERT_NE(nullptr, state);
    ASSERT_EQ(0u, state->parameterTransitionFramesRemaining);
    ASSERT_EQ(0u, state->resetBridgeFramesRemaining);
    const float expectedTarget =
        LegacyFloatCompressorTargetGain(expectedParameters,
                                        detectorMagnitude);
    const float expectedGain =
        ReferenceCompressorEnvelopeStep(
            expectedParameters, state->currentGain, expectedTarget);
    const float expectedOutput =
        detectorMagnitude * expectedGain *
        expectedParameters.makeupLinear;

    Capture captured;
    harkentest::PumpAndCapture(engine, 1, captured);
    ASSERT_FALSE(captured.left.empty());
    EXPECT_NEAR(expectedOutput, captured.left.front(), 2.0e-5f);
}

struct Kernel {
    harken::LimiterState state;
    harken::InsertEffect effect;

    Kernel(float ceilingDb, float releaseMs) {
        state.sampleRate = kRate;
        effect = harken::BuildEffect(VN_EFFECT_LIMITER, &state);
        effect.setParam(&state, harken::kLimiterParamCeilingDb, ceilingDb);
        effect.setParam(&state, harken::kLimiterParamReleaseMs, releaseMs);
        effect.reset(&state);
    }
};

VnLimiterDesc LimiterDesc(float ceilingDb, float releaseMs) {
    VnLimiterDesc desc{};
    desc.structSize = sizeof(VnLimiterDesc);
    desc.ceilingDb = ceilingDb;
    desc.releaseMs = releaseMs;
    return desc;
}

VnCompressorDesc CompressorDesc(float thresholdDb = -18.0f,
                                float ratio = 1.0f,
                                float attackMs = 10.0f,
                                float releaseMs = 100.0f,
                                float kneeDb = 6.0f,
                                float makeupDb = 0.0f) {
    VnCompressorDesc desc{};
    desc.structSize = sizeof(VnCompressorDesc);
    desc.thresholdDb = thresholdDb;
    desc.ratio = ratio;
    desc.attackMs = attackMs;
    desc.releaseMs = releaseMs;
    desc.kneeDb = kneeDb;
    desc.makeupDb = makeupDb;
    return desc;
}

const VnEffectDesc* AsBase(const void* desc) {
    return static_cast<const VnEffectDesc*>(desc);
}

const VnEffectTypeStats* FindLimiter(const VnSnapshot& snapshot) {
    for (std::uint32_t i = 0; i < snapshot.effectTypeStatCount; ++i) {
        if (snapshot.effectTypeStats[i].effectType == VN_EFFECT_LIMITER) {
            return &snapshot.effectTypeStats[i];
        }
    }
    return nullptr;
}

VnSnapshot Snapshot() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

TEST(LimiterEffect, BelowCeilingIsBitIdenticalFromCleanState) {
    Kernel kernel(-6.0f, 100.0f);
    std::vector<float> left = {
        0.0f, 0.125f, -0.25f, 0.375f, -0.5f, 0.0625f};
    std::vector<float> right = {
        -0.25f, 0.5f, 0.0f, -0.125f, 0.25f, -0.375f};
    const std::vector<float> expectedLeft = left;
    const std::vector<float> expectedRight = right;
    float* channels[2] = {left.data(), right.data()};
    kernel.effect.process(&kernel.state, channels, 2, 0,
                          static_cast<std::uint32_t>(left.size()));

    EXPECT_EQ(0, std::memcmp(left.data(), expectedLeft.data(),
                             left.size() * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(right.data(), expectedRight.data(),
                             right.size() * sizeof(float)));
    EXPECT_FLOAT_EQ(1.0f, kernel.state.currentGain);
}

TEST(LimiterEffect, CurrentFrameCeilingLinkedBalanceAndZeroLatency) {
    Kernel kernel(-6.020599913f, 100.0f); // linear Ceiling = 0.5
    std::vector<float> left = {1.0f, 0.0f, -2.0f, 0.0f};
    std::vector<float> right = {0.25f, 0.0f, -0.5f, 0.0f};
    float* channels[2] = {left.data(), right.data()};
    kernel.effect.process(&kernel.state, channels, 2, 0,
                          static_cast<std::uint32_t>(left.size()));

    const float ceiling = kernel.state.ceilingLinear;
    for (std::size_t i = 0; i < left.size(); ++i) {
        EXPECT_LE(std::fabs(left[i]), ceiling);
        EXPECT_LE(std::fabs(right[i]), ceiling);
    }
    // The impulse is acted on in frame zero, not shifted into a later frame.
    EXPECT_GT(std::fabs(left[0]), 0.0f);
    EXPECT_FLOAT_EQ(0.0f, left[1]);
    // One linked gain is applied to both channels, preserving their 4:1 ratio.
    EXPECT_FLOAT_EQ(left[0], right[0] * 4.0f);
    EXPECT_FLOAT_EQ(left[2], right[2] * 4.0f);
}

TEST(LimiterEffect, LinkedDetectorScansEveryChannelNotOnlyChannelZero) {
    Kernel kernel(-6.020599913f, 100.0f); // linear Ceiling = 0.5
    // Channel 0's peak magnitude never exceeds the Ceiling (0.25 and 0.5, the
    // latter exactly at the Ceiling and therefore not over it), so on its own it
    // would trigger no gain reduction. All of the over-Ceiling energy lives in
    // channel 1. A detector that scans channel 0 alone would leave channel 1
    // above the Ceiling; the linked detector must scan every active channel.
    std::vector<float> left = {0.25f, 0.0f, -0.5f, 0.0f};
    std::vector<float> right = {1.0f, 0.0f, -2.0f, 0.0f};
    float* channels[2] = {left.data(), right.data()};
    kernel.effect.process(&kernel.state, channels, 2, 0,
                          static_cast<std::uint32_t>(left.size()));

    const float ceiling = kernel.state.ceilingLinear;
    for (std::size_t i = 0; i < left.size(); ++i) {
        EXPECT_LE(std::fabs(left[i]), ceiling) << "left sample " << i;
        EXPECT_LE(std::fabs(right[i]), ceiling) << "right sample " << i;
    }
    // Channel 0 is attenuated even though its own magnitude never breached the
    // Ceiling: the only possible cause is the linked peak observed in channel 1.
    EXPECT_LT(std::fabs(left[0]), 0.25f);
    EXPECT_LT(std::fabs(left[2]), 0.5f);
    // One linked gain scales both channels, preserving their 4:1 input ratio.
    EXPECT_FLOAT_EQ(right[0], left[0] * 4.0f);
    EXPECT_FLOAT_EQ(right[2], left[2] * 4.0f);
}

TEST(LimiterEffect, ReleaseMatchesIndependentTimeConstantReference) {
    constexpr float kCeilingDb = -12.0f;
    constexpr float kReleaseMs = 100.0f;
    Kernel kernel(kCeilingDb, kReleaseMs);
    ReferenceLimiter reference(kCeilingDb, kReleaseMs);

    std::vector<float> signal(4801, 0.25f);
    signal[0] = 1.0f;
    std::vector<float> expected(signal.size());
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double gain = reference.Step(std::fabs(signal[i]));
        expected[i] =
            static_cast<float>(static_cast<double>(signal[i]) * gain);
    }

    float* channels[1] = {signal.data()};
    kernel.effect.process(&kernel.state, channels, 1, 0,
                          static_cast<std::uint32_t>(signal.size()));

    for (std::size_t i : {0u, 1u, 16u, 256u, 2400u, 4800u}) {
        EXPECT_NEAR(signal[i], expected[i], 2.0e-5f) << "sample " << i;
    }
    const double expectedAfterOneTau =
        1.0 - (1.0 - reference.ceiling) * std::exp(-1.0);
    EXPECT_NEAR(kernel.state.currentGain, expectedAfterOneTau, 2.0e-4);
}

TEST(LimiterEffect, ResetRetuneBlockSplitAndTailAreDeterministic) {
    Kernel whole(-9.0f, 75.0f);
    Kernel split(-9.0f, 75.0f);
    std::vector<float> input(777);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = (i % 9 == 0) ? 1.25f : ((i % 2) ? 0.2f : -0.3f);
    }
    std::vector<float> a = input;
    std::vector<float> b = input;
    float* ca[1] = {a.data()};
    float* cb[1] = {b.data()};
    whole.effect.process(&whole.state, ca, 1, 0,
                         static_cast<std::uint32_t>(a.size()));
    split.effect.process(&split.state, cb, 1, 0, 113);
    split.effect.process(&split.state, cb, 1, 113, 257);
    split.effect.process(&split.state, cb, 1, 370,
                         static_cast<std::uint32_t>(b.size() - 370));

    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(float)));
    EXPECT_FLOAT_EQ(whole.state.currentGain, split.state.currentGain);
    EXPECT_EQ(0u, whole.effect.tailFrames(&whole.state));

    const float dirtyGain = whole.state.currentGain;
    ASSERT_LT(dirtyGain, 1.0f);
    whole.effect.setParam(&whole.state, harken::kLimiterParamCeilingDb, -3.0f);
    whole.effect.setParam(&whole.state, harken::kLimiterParamReleaseMs, 250.0f);
    EXPECT_FLOAT_EQ(dirtyGain, whole.state.currentGain)
        << "parameter retune must preserve detector state";
    const float retainedCeiling = whole.state.ceilingLinear;
    const float retainedRelease = whole.state.releaseCoeff;
    whole.effect.reset(&whole.state);
    EXPECT_FLOAT_EQ(1.0f, whole.state.currentGain);
    EXPECT_FLOAT_EQ(retainedCeiling, whole.state.ceilingLinear);
    EXPECT_FLOAT_EQ(retainedRelease, whole.state.releaseCoeff);

    float below = 0.25f;
    float* one[1] = {&below};
    whole.effect.process(&whole.state, one, 1, 0, 1);
    EXPECT_FLOAT_EQ(0.25f, below)
        << "a reset or recycled state starts clean";
}

TEST(LimiterEffect, BypassedVoiceSlotIsExactIdentity) {
    auto render = [](bool installLimiter, bool bypass) {
        VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> source = harkentest::MakeDC(2048, 0.9f);
        const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        VnVoiceHandle handle = 0;
        const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        if (bypass) {
            // Set bypass while the slot is empty so it settles dry before the
            // effect is installed; this isolates bypass identity from the
            // deliberate one-block transition used by a live toggle.
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));
        }
        if (installLimiter) {
            const VnLimiterDesc desc = LimiterDesc(-12.0f, 100.0f);
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(
                                 handle, 0, VN_EFFECT_LIMITER, AsBase(&desc)));
        }
        Capture capture;
        harkentest::PumpAndCapture(session.Engine(), 6, capture);
        return capture;
    };

    const Capture dry = render(false, false);
    const Capture bypassed = render(true, true);
    const Capture active = render(true, false);
    ASSERT_EQ(dry.left.size(), bypassed.left.size());
    EXPECT_EQ(0, std::memcmp(dry.left.data(), bypassed.left.data(),
                             dry.left.size() * sizeof(float)));
    EXPECT_LT(harkentest::Peak(active.left), harkentest::Peak(dry.left));
}

TEST(LimiterEffect, BypassToEnableEdgeResetsDetectorHistoryOnLiveVoice) {
    // Toggling a live voice-slot Limiter from bypass back to enable must clear
    // its gain-reduction history, so the first below-ceiling block after the
    // enable edge starts from clean unity gain rather than the attenuation left
    // over from earlier over-ceiling material. This is driven through the
    // engine-level C API so it pins the VoicePool bypass wiring itself: a
    // kernel-only toggle would never traverse ApplySetVoiceEffectBypass and so
    // could not catch removal of the enabling-edge reset. The sibling
    // BypassedVoiceSlotIsExactIdentity deliberately settles bypass on an empty
    // slot and never exercises the live bypass->enable transition.
    constexpr std::uint32_t kOverBlocks = 6; // dirty the gain, settle past fade-in
    constexpr float kOver = 0.9f;   // above the -6 dB ceiling (~0.501): reduces gain
    constexpr float kBelow = 0.25f; // below the ceiling: a clean unity passthrough

    std::vector<float> source(static_cast<std::size_t>(kOverBlocks) * kBlock,
                              kOver);
    source.resize(static_cast<std::size_t>(kOverBlocks + 2) * kBlock, kBelow);

    struct EdgeRun {
        Capture warm;  // kOverBlocks blocks; last block exposes the dirtied gain
        Capture below; // one block: the first below-ceiling block after the edge
    };

    auto run = [&](bool withLimiter) {
        VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
        config.fxSlotsPerVoice = 1;
        config.limiterEffectPoolCapacity = 1;
        EngineSession session(config);
        EXPECT_EQ(VN_OK, session.InitResult());
        const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        VnVoiceHandle handle = 0;
        const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        if (withLimiter) {
            const VnLimiterDesc desc = LimiterDesc(-6.0f, 100.0f);
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(
                                 handle, 0, VN_EFFECT_LIMITER, AsBase(&desc)));
        }
        EdgeRun out;
        harkentest::PumpAndCapture(session.Engine(), kOverBlocks, out.warm);
        if (withLimiter) {
            // Bypass then immediately re-enable with no render in between: both
            // commands drain before the next block, so the settled wet mix never
            // moves and it is the enable edge, not a bypass crossfade, that
            // governs the block below.
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 0));
        }
        harkentest::PumpAndCapture(session.Engine(), 1, out.below);
        return out;
    };

    const EdgeRun dry = run(false);
    const EdgeRun toggled = run(true);

    // Guard: the limiter really was reducing gain at the enable edge, so the
    // reset it must perform is genuinely exercised (otherwise the kill below is
    // vacuous — a never-dirtied gain would pass with or without the reset).
    const float dryEdge = harkentest::Peak(
        dry.warm.left.data() + dry.warm.left.size() - kBlock, kBlock);
    const float toggledEdge = harkentest::Peak(
        toggled.warm.left.data() + toggled.warm.left.size() - kBlock, kBlock);
    ASSERT_LT(toggledEdge, dryEdge)
        << "over-ceiling material did not dirty the limiter gain";

    // Kill: with a clean unity gain the below-ceiling block is an exact
    // passthrough, bit-identical to the same block rendered by a voice carrying
    // no limiter. Removing the enabling-edge reset retains the dirty gain, which
    // attenuates this block and diverges the bytes.
    HARKENTEST_ASSERT_NON_SILENT(dry.below.left);
    ASSERT_EQ(dry.below.left.size(), toggled.below.left.size());
    EXPECT_EQ(0, std::memcmp(dry.below.left.data(), toggled.below.left.data(),
                             dry.below.left.size() * sizeof(float)));
}

TEST(LimiterEffect, VoiceSlotMatchesIndependentSampleReference) {
    constexpr float kCeilingDb = -12.0f;
    constexpr float kReleaseMs = 80.0f;
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.limiterEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::vector<float> source(kBlock, 0.2f);
    source[0] = 1.0f;
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnLimiterDesc desc = LimiterDesc(kCeilingDb, kReleaseMs);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, 0, VN_EFFECT_LIMITER, AsBase(&desc)));

    Capture output;
    harkentest::PumpAndCapture(session.Engine(), 1, output);
    ASSERT_EQ(kBlock, output.left.size());
    ASSERT_EQ(output.left.size(), output.right.size());

    ReferenceLimiter reference(kCeilingDb, kReleaseMs);
    for (std::size_t i = 0; i < source.size(); ++i) {
        const float expected = static_cast<float>(
            static_cast<double>(source[i]) *
            reference.Step(std::fabs(static_cast<double>(source[i]))));
        EXPECT_NEAR(expected, output.left[i], 2.0e-5f) << "sample " << i;
        EXPECT_NEAR(expected, output.right[i], 2.0e-5f) << "sample " << i;
    }
}

TEST(LimiterEffect, BusPreAndPostFaderMatchIndependentReferences) {
    constexpr float kInput = 0.75f;
    constexpr float kCeilingDb = -6.020599913f;
    constexpr float kReleaseMs = 100.0f;
    constexpr float kFaderDb = -6.020599913f;
    const double fader = std::pow(10.0, kFaderDb / 20.0);

    auto render = [&](std::uint32_t faderAfterSlot) {
        VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.limiterEffectPoolCapacity = 1;
        EngineSession session(config);
        EXPECT_EQ(VN_OK, session.InitResult());
        // Keep the bus detector input fixed while testing fader topology.
        const std::vector<float> source =
            harkentest::MakeDC(kBlock * 2, kInput);
        const VnPCMDesc pcm = harkentest::InterleavedDesc(source, 2, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        EXPECT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(2001), VN_BUS_MASTER));
        EXPECT_EQ(VN_OK, Vn_SetBusFaderPoint(harkentest::ObjectId(2001), faderAfterSlot));
        EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(2001), kFaderDb, 0));
        const VnLimiterDesc limiter =
            LimiterDesc(kCeilingDb, kReleaseMs);
        EXPECT_EQ(VN_OK, Vn_SetBusEffect(
                             harkentest::ObjectId(2001), 0, VN_EFFECT_LIMITER, AsBase(&limiter)));
        VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
        play.busId = harkentest::ObjectId(2001);
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        Capture output;
        harkentest::PumpAndCapture(session.Engine(), 1, output);
        return output.left;
    };

    const std::vector<float> pre = render(1);
    const std::vector<float> post = render(0);
    ASSERT_EQ(kBlock, pre.size());
    ASSERT_EQ(pre.size(), post.size());
    ReferenceLimiter preReference(kCeilingDb, kReleaseMs);
    ReferenceLimiter postReference(kCeilingDb, kReleaseMs);
    for (std::size_t i = 0; i < pre.size(); ++i) {
        const double preGain = preReference.Step(kInput);
        const double postInput = static_cast<double>(kInput) * fader;
        const double postGain = postReference.Step(postInput);
        const float expectedPre = static_cast<float>(
            static_cast<double>(kInput) * preGain * fader);
        const float expectedPost =
            static_cast<float>(postInput * postGain);
        EXPECT_NEAR(expectedPre, pre[i], 2.0e-5f) << "pre sample " << i;
        EXPECT_NEAR(expectedPost, post[i], 2.0e-5f) << "post sample " << i;
    }
    EXPECT_GT(std::fabs(pre.back() - post.back()), 0.1f);
}

TEST(LimiterEffect,
     FaderRelocationKeepsOneDirtyStateAndProcessesOnlyTheNewTopology) {
    constexpr VnBusId kBus = harkentest::ObjectId(2002);
    constexpr float kInput = 0.75f;
    constexpr float kCeilingDb = -6.020599913f;
    constexpr float kReleaseMs = 100.0f;
    constexpr float kFaderDb = -6.020599913f;
    const double fader = std::pow(10.0, kFaderDb / 20.0);
    const double ceiling = std::pow(10.0, kCeilingDb / 20.0);
    const double releaseCoeff =
        std::exp(-1.0 / (kReleaseMs * 0.001 * kRate));

    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.limiterEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    // Keep the bus detector input fixed while testing state relocation.
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 8, kInput);
    const VnPCMDesc pcm = harkentest::InterleavedDesc(source, 2, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBus, 1));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBus, kFaderDb, 0));
    const VnLimiterDesc limiter = LimiterDesc(kCeilingDb, kReleaseMs);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBus, 0, VN_EFFECT_LIMITER, AsBase(&limiter)));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBus;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture warm;
    harkentest::PumpAndCapture(session.Engine(), 1, warm);
    const std::uint32_t busIndex = session.Engine()->buses.FindIndex(kBus);
    harken::BusTree::Bus* bus =
        session.Engine()->buses.BusState(busIndex);
    ASSERT_NE(nullptr, bus);
    ASSERT_EQ(VN_EFFECT_LIMITER, bus->fxSlotEffectRef[0].type);
    void* const stateAddress = bus->fxSlots[0].state;
    const harken::EffectInstanceRef instance = bus->fxSlotEffectRef[0];
    auto* state = static_cast<harken::LimiterState*>(stateAddress);
    ASSERT_NE(nullptr, state);
    const float dirtyGain = state->currentGain;
    ASSERT_LT(dirtyGain, 1.0f);
    EXPECT_NEAR(ceiling / kInput, dirtyGain, 2.0e-5);
    ASSERT_EQ(1u, session.Engine()->effectBank.InUseOf(VN_EFFECT_LIMITER));

    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBus, 0));
    Capture moved;
    harkentest::PumpAndCapture(session.Engine(), 1, moved);
    ASSERT_EQ(kBlock, moved.left.size());

    bus = session.Engine()->buses.BusState(busIndex);
    ASSERT_NE(nullptr, bus);
    EXPECT_EQ(stateAddress, bus->fxSlots[0].state);
    EXPECT_EQ(instance.type, bus->fxSlotEffectRef[0].type);
    EXPECT_EQ(instance.poolIndex, bus->fxSlotEffectRef[0].poolIndex);
    EXPECT_EQ(1u, session.Engine()->effectBank.InUseOf(VN_EFFECT_LIMITER));

    double expectedGain = dirtyGain;
    const double postFaderInput = static_cast<double>(kInput) * fader;
    ASSERT_LT(postFaderInput, ceiling);
    for (std::size_t i = 0; i < moved.left.size(); ++i) {
        expectedGain =
            releaseCoeff * expectedGain + (1.0 - releaseCoeff);
        const float expected =
            static_cast<float>(postFaderInput * expectedGain);
        EXPECT_NEAR(expected, moved.left[i], 3.0e-5f)
            << "moved sample " << i;
    }
    EXPECT_NEAR(expectedGain,
                static_cast<harken::LimiterState*>(
                    bus->fxSlots[0].state)->currentGain,
                3.0e-5);
    EXPECT_GT(moved.left.back(), moved.left.front());
    EXPECT_LT(moved.left.front(), static_cast<float>(postFaderInput) - 0.05f)
        << "a reset or wet/dry relocation crossfade leaked into the block";
}

TEST(LimiterEffect, ValidationRefusalsHaveNoPoolOrRenderSideEffects) {
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_LIMITER, harken::kLimiterParamCeilingDb,
                         -24.0f, kRate));
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_LIMITER, harken::kLimiterParamCeilingDb,
                         0.0f, kRate));
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_LIMITER, harken::kLimiterParamReleaseMs,
                         1.0f, kRate));
    EXPECT_EQ(VN_OK, harken::ValidateEffectParam(
                         VN_EFFECT_LIMITER, harken::kLimiterParamReleaseMs,
                         1000.0f, kRate));

    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.limiterEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> source = harkentest::MakeDC(4096, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    const VnSnapshot before = Snapshot();
    const VnEffectTypeStats* beforeLimiter = FindLimiter(before);
    ASSERT_NE(nullptr, beforeLimiter);
    EXPECT_EQ(1u, beforeLimiter->capacity);

    VnLimiterDesc invalid = LimiterDesc(-24.0001f, 100.0f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_LIMITER,
                                  AsBase(&invalid)));
    invalid = LimiterDesc(0.0f, 1000.0001f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_LIMITER,
                                  AsBase(&invalid)));
    invalid = LimiterDesc(std::numeric_limits<float>::quiet_NaN(), 100.0f);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_LIMITER,
                                  AsBase(&invalid)));
    invalid = LimiterDesc(-1.0f, 100.0f);
    invalid.structSize -= 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffect(
                                  handle, 0, VN_EFFECT_LIMITER,
                                  AsBase(&invalid)));

    const VnSnapshot after = Snapshot();
    const VnEffectTypeStats* afterLimiter = FindLimiter(after);
    ASSERT_NE(nullptr, afterLimiter);
    EXPECT_EQ(beforeLimiter->activeCount, afterLimiter->activeCount);
    EXPECT_EQ(beforeLimiter->highWater, afterLimiter->highWater);
    EXPECT_EQ(beforeLimiter->refusalCount, afterLimiter->refusalCount);

    const VnLimiterDesc valid = LimiterDesc(-1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, 0, VN_EFFECT_LIMITER, AsBase(&valid)));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0,
                                     harken::kLimiterParamCeilingDb, 0.0001f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0,
                                     harken::kLimiterParamReleaseMs, 0.9999f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0,
                                     harken::kLimiterParamCount, 100.0f));

    Capture output;
    harkentest::PumpAndCapture(session.Engine(), 2, output);
    EXPECT_NEAR(0.5f, output.left.back(), 1.0e-5f);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(CompressorKernel, LinearGainAccumulatorReusesTelemetryPadding) {
    EXPECT_EQ(sizeof(CompressorBlockTelemetryBeforeLinearMinimum),
              sizeof(harken::CompressorBlockTelemetry));
    EXPECT_EQ(alignof(CompressorBlockTelemetryBeforeLinearMinimum),
              alignof(harken::CompressorBlockTelemetry));
    EXPECT_EQ(sizeof(CompressorStateBeforeLinearMinimum),
              sizeof(harken::CompressorState));
    EXPECT_EQ(alignof(CompressorStateBeforeLinearMinimum),
              alignof(harken::CompressorState));
}

TEST(CompressorKernel, PreparedCacheLayoutAndRegistryPoolStrideArePinned) {
    EXPECT_EQ(44u, sizeof(harken::CompressorPreparedParameters));
    EXPECT_EQ(4u, alignof(harken::CompressorPreparedParameters));
    EXPECT_EQ(240u, sizeof(harken::CompressorState));
    EXPECT_EQ(8u, alignof(harken::CompressorState));

    const harken::EffectRegistryRow* row =
        harken::FindEffectRow(VN_EFFECT_COMPRESSOR);
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(sizeof(harken::CompressorState), row->stateSize);
    EXPECT_EQ(alignof(harken::CompressorState), row->stateAlign);

    constexpr std::size_t kPoolCapacity = 64;
    constexpr std::size_t kPoolUnit = sizeof(std::max_align_t);
    const std::size_t poolBytes =
        kPoolCapacity *
        ((sizeof(harken::CompressorState) + kPoolUnit - 1) / kPoolUnit) *
        kPoolUnit;
    EXPECT_EQ(15360u, poolBytes);
}

TEST(CompressorKernel, SteadyTargetGainMatchesLegacyOracleMatrix) {
    const float ratioAfterUnity =
        std::nextafter(1.0f,
                       (std::numeric_limits<float>::infinity)());
    const float subnormal =
        (std::numeric_limits<float>::denorm_min)();
    const float maximum = (std::numeric_limits<float>::max)();
    const float nan = (std::numeric_limits<float>::quiet_NaN)();
    const float infinity =
        (std::numeric_limits<float>::infinity)();
    double maximumTargetError = 0.0;
    double maximumOutputError = 0.0;

    for (float thresholdDb :
         {harken::kCompressorMinThresholdDb,
          harken::kCompressorMaxThresholdDb}) {
        for (float ratio :
             {harken::kCompressorMinRatio, ratioAfterUnity,
              harken::kCompressorMaxRatio}) {
            for (float kneeDb :
                 {harken::kCompressorMinKneeDb,
                  harken::kCompressorMaxKneeDb}) {
                harken::CompressorParameters parameters{};
                parameters.thresholdDb = thresholdDb;
                parameters.ratio = ratio;
                parameters.attackMs = harken::kCompressorMinAttackMs;
                parameters.releaseMs =
                    harken::kCompressorMinReleaseMs;
                parameters.kneeDb = kneeDb;
                parameters.makeupDb = 0.0f;
                harken::CompressorPreparedParameters prepared{};
                ASSERT_TRUE(harken::PrepareCompressorParameters(
                    parameters, kRate, &prepared));

                const float halfKnee = 0.5f * kneeDb;
                const float lowerBoundary = std::pow(
                    10.0f, (thresholdDb - halfKnee) / 20.0f);
                const float upperBoundary = std::pow(
                    10.0f, (thresholdDb + halfKnee) / 20.0f);
                const std::vector<float> detectors = {
                    0.0f,
                    -0.0f,
                    subnormal,
                    1.0f,
                    maximum,
                    nan,
                    infinity,
                    -infinity,
                    std::nextafter(lowerBoundary, 0.0f),
                    lowerBoundary,
                    std::nextafter(lowerBoundary, infinity),
                    std::nextafter(upperBoundary, 0.0f),
                    upperBoundary,
                    std::nextafter(upperBoundary, infinity)};

                for (std::size_t detectorIndex = 0;
                     detectorIndex < detectors.size();
                     ++detectorIndex) {
                    harken::CompressorState state{};
                    harken::CompressorInit(&state, kRate);
                    harken::CompressorApplyPreparedParameters(
                        &state, prepared);

                    float output = 1.0f;
                    float* channels[1] = {&output};
                    const float detector = detectors[detectorIndex];
                    harken::CompressorProcessWithDetector(
                        &state, channels, 1, &detector, 0, 1);

                    const double sanitizedDetector =
                        std::isfinite(detector)
                            ? std::abs(static_cast<double>(detector))
                            : 0.0;
                    const double expectedTarget =
                        ReferenceCompressorTargetGain(
                            prepared, sanitizedDetector);
                    const double actualTarget =
                        (static_cast<double>(state.currentGain) -
                         static_cast<double>(prepared.attackCoeff)) /
                        static_cast<double>(prepared.attackAdd);
                    maximumTargetError = std::max(
                        maximumTargetError,
                        std::abs(expectedTarget - actualTarget));
                    EXPECT_NEAR(expectedTarget, actualTarget, 2.0e-6)
                        << "threshold=" << thresholdDb
                        << " ratio=" << ratio
                        << " knee=" << kneeDb
                        << " detectorIndex=" << detectorIndex;
                    const double expectedOutput =
                        static_cast<double>(prepared.attackCoeff) +
                        static_cast<double>(prepared.attackAdd) *
                            expectedTarget;
                    maximumOutputError = std::max(
                        maximumOutputError,
                        std::abs(expectedOutput -
                                 static_cast<double>(output)));
                    EXPECT_NEAR(expectedOutput, output, 2.0e-5)
                        << "threshold=" << thresholdDb
                        << " ratio=" << ratio
                        << " knee=" << kneeDb
                        << " detectorIndex=" << detectorIndex;
                    EXPECT_GE(state.currentGain, 0.0f);
                    EXPECT_LE(state.currentGain, 1.0f);
                    EXPECT_TRUE(std::isfinite(state.currentGain));
                }
            }
        }
    }
    std::ostringstream targetError;
    targetError.precision(17);
    targetError << maximumTargetError;
    RecordProperty("maximum_target_error", targetError.str());
    std::ostringstream outputError;
    outputError.precision(17);
    outputError << maximumOutputError;
    RecordProperty("maximum_output_error", outputError.str());
}

TEST(CompressorKernel, PreparedCachesFollowApplyTransitionRetargetAndReset) {
    harken::CompressorParameters first{};
    first.thresholdDb = harken::kCompressorMinThresholdDb;
    first.ratio = harken::kCompressorMaxRatio;
    first.attackMs = harken::kCompressorMinAttackMs;
    first.releaseMs = harken::kCompressorMaxReleaseMs;
    first.kneeDb = harken::kCompressorMaxKneeDb;
    harken::CompressorPreparedParameters firstPrepared{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        first, kRate, &firstPrepared));
    EXPECT_TRUE(std::isfinite(firstPrepared.lowerKneeLinear));
    EXPECT_TRUE(std::isfinite(firstPrepared.upperKneeLinear));
    EXPECT_TRUE(std::isfinite(firstPrepared.hardKneeScale));
    EXPECT_FLOAT_EQ(
        std::pow(10.0f,
                 (first.thresholdDb - 0.5f * first.kneeDb) /
                     20.0f),
        firstPrepared.lowerKneeLinear);
    EXPECT_FLOAT_EQ(
        std::pow(10.0f,
                 (first.thresholdDb + 0.5f * first.kneeDb) /
                     20.0f),
        firstPrepared.upperKneeLinear);
    EXPECT_FLOAT_EQ(
        std::pow(10.0f,
                 first.thresholdDb *
                     (1.0f - firstPrepared.reciprocalRatio) /
                     20.0f),
        firstPrepared.hardKneeScale);

    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);
    harken::CompressorApplyPreparedParameters(
        &state, firstPrepared);
    EXPECT_FLOAT_EQ(firstPrepared.lowerKneeLinear,
                    state.parameters.lowerKneeLinear);
    EXPECT_FLOAT_EQ(firstPrepared.upperKneeLinear,
                    state.targetParameters.upperKneeLinear);
    EXPECT_FLOAT_EQ(firstPrepared.hardKneeScale,
                    state.targetParameters.hardKneeScale);
    EXPECT_FLOAT_EQ(0.0f,
                    state.parameterIncrements.lowerKneeLinear);
    EXPECT_FLOAT_EQ(0.0f,
                    state.parameterIncrements.upperKneeLinear);
    EXPECT_FLOAT_EQ(0.0f,
                    state.parameterIncrements.hardKneeScale);

    float rendered = 0.0f;
    float* channels[1] = {&rendered};
    harken::CompressorProcess(&state, channels, 1, 0, 1);

    harken::CompressorParameters second = first;
    second.thresholdDb = harken::kCompressorMaxThresholdDb;
    second.ratio = 2.0f;
    second.kneeDb = harken::kCompressorMinKneeDb;
    harken::CompressorPreparedParameters secondPrepared{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        second, kRate, &secondPrepared));
    constexpr std::uint32_t kTransitionFrames = 8;
    harken::CompressorRequestPreparedParameters(
        &state, secondPrepared, kTransitionFrames);
    EXPECT_FLOAT_EQ(secondPrepared.lowerKneeLinear,
                    state.targetParameters.lowerKneeLinear);
    EXPECT_FLOAT_EQ(secondPrepared.upperKneeLinear,
                    state.targetParameters.upperKneeLinear);
    EXPECT_FLOAT_EQ(secondPrepared.hardKneeScale,
                    state.targetParameters.hardKneeScale);
    EXPECT_FLOAT_EQ(0.0f,
                    state.parameterIncrements.lowerKneeLinear);
    EXPECT_FLOAT_EQ(0.0f,
                    state.parameterIncrements.upperKneeLinear);
    EXPECT_FLOAT_EQ(0.0f,
                    state.parameterIncrements.hardKneeScale);

    const harken::CompressorPreparedParameters beforeZero =
        state.parameters;
    harken::CompressorProcess(&state, channels, 1, 0, 0);
    EXPECT_EQ(kTransitionFrames,
              state.parameterTransitionFramesRemaining);
    EXPECT_FLOAT_EQ(beforeZero.lowerKneeLinear,
                    state.parameters.lowerKneeLinear);
    EXPECT_FLOAT_EQ(beforeZero.upperKneeLinear,
                    state.parameters.upperKneeLinear);
    EXPECT_FLOAT_EQ(beforeZero.hardKneeScale,
                    state.parameters.hardKneeScale);

    harken::CompressorParameters third = second;
    third.thresholdDb = -24.0f;
    third.ratio = 6.0f;
    third.kneeDb = 12.0f;
    harken::CompressorPreparedParameters thirdPrepared{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        third, kRate, &thirdPrepared));
    harken::CompressorRequestPreparedParameters(
        &state, thirdPrepared, kTransitionFrames);
    EXPECT_FLOAT_EQ(thirdPrepared.lowerKneeLinear,
                    state.targetParameters.lowerKneeLinear);
    EXPECT_FLOAT_EQ(thirdPrepared.hardKneeScale,
                    state.targetParameters.hardKneeScale);

    std::vector<float> silence(kTransitionFrames, 0.0f);
    channels[0] = silence.data();
    harken::CompressorProcess(
        &state, channels, 1, 0,
        static_cast<std::uint32_t>(silence.size()));
    EXPECT_EQ(0u, state.parameterTransitionFramesRemaining);
    EXPECT_FLOAT_EQ(thirdPrepared.lowerKneeLinear,
                    state.parameters.lowerKneeLinear);
    EXPECT_FLOAT_EQ(thirdPrepared.upperKneeLinear,
                    state.parameters.upperKneeLinear);
    EXPECT_FLOAT_EQ(thirdPrepared.hardKneeScale,
                    state.parameters.hardKneeScale);

    harken::CompressorRequestPreparedParameters(
        &state, secondPrepared, kTransitionFrames);
    harken::CompressorReset(&state);
    EXPECT_EQ(0u, state.parameterTransitionFramesRemaining);
    EXPECT_FLOAT_EQ(secondPrepared.lowerKneeLinear,
                    state.parameters.lowerKneeLinear);
    EXPECT_FLOAT_EQ(secondPrepared.upperKneeLinear,
                    state.parameters.upperKneeLinear);
    EXPECT_FLOAT_EQ(secondPrepared.hardKneeScale,
                    state.parameters.hardKneeScale);
}

TEST(CompressorKernel,
     TransitionFinalFrameAndMidRetargetUseLegacyTargetPath) {
    harken::CompressorParameters target{};
    target.thresholdDb = -37.0f;
    target.ratio = 7.0f;
    target.attackMs = harken::kCompressorMinAttackMs;
    target.releaseMs = 125.0f;
    target.kneeDb = 11.0f;
    harken::CompressorPreparedParameters targetPrepared{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        target, kRate, &targetPrepared));

    const std::vector<float> detectors = {
        std::nextafter(
            std::pow(10.0f,
                     (target.thresholdDb + 0.5f * target.kneeDb) /
                         20.0f),
            (std::numeric_limits<float>::infinity)()),
        1.0f,
        (std::numeric_limits<float>::max)()};
    for (float detector : detectors) {
        harken::CompressorState state{};
        harken::CompressorInit(&state, kRate);
        state.hasRendered = true;
        state.targetParameters = targetPrepared;
        state.parameterTransitionFramesRemaining = 1;
        const float expectedTarget =
            LegacyFloatCompressorTargetGain(targetPrepared, detector);
        const float expectedGain =
            ReferenceCompressorEnvelopeStep(
                targetPrepared, state.currentGain, expectedTarget);

        float output = 1.0f;
        float* channels[1] = {&output};
        harken::CompressorProcessWithDetector(
            &state, channels, 1, &detector, 0, 1);

        EXPECT_EQ(0u, state.parameterTransitionFramesRemaining);
        EXPECT_EQ(0, std::memcmp(&expectedGain, &state.currentGain,
                                 sizeof(expectedGain)));
    }

    harken::CompressorState retargeted{};
    harken::CompressorInit(&retargeted, kRate);
    float silence = 0.0f;
    float* channels[1] = {&silence};
    harken::CompressorProcess(&retargeted, channels, 1, 0, 1);

    harken::CompressorParameters intermediate = target;
    intermediate.thresholdDb = -18.0f;
    intermediate.ratio = 3.0f;
    intermediate.kneeDb = 3.0f;
    harken::CompressorPreparedParameters intermediatePrepared{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        intermediate, kRate, &intermediatePrepared));
    constexpr std::uint32_t kTransitionFrames = 8;
    harken::CompressorRequestPreparedParameters(
        &retargeted, intermediatePrepared, kTransitionFrames);
    std::vector<float> leadIn(3, 0.0f);
    channels[0] = leadIn.data();
    harken::CompressorProcess(&retargeted, channels, 1, 0, 3);

    harken::CompressorRequestPreparedParameters(
        &retargeted, targetPrepared, kTransitionFrames);
    harken::CompressorPreparedParameters expectedParameters =
        retargeted.parameters;
    expectedParameters.thresholdDb +=
        retargeted.parameterIncrements.thresholdDb;
    expectedParameters.reciprocalRatio +=
        retargeted.parameterIncrements.reciprocalRatio;
    expectedParameters.attackCoeff +=
        retargeted.parameterIncrements.attackCoeff;
    expectedParameters.releaseCoeff +=
        retargeted.parameterIncrements.releaseCoeff;
    expectedParameters.kneeDb +=
        retargeted.parameterIncrements.kneeDb;
    expectedParameters.makeupLinear +=
        retargeted.parameterIncrements.makeupLinear;
    expectedParameters.attackAdd =
        1.0f - expectedParameters.attackCoeff;
    expectedParameters.releaseAdd =
        1.0f - expectedParameters.releaseCoeff;
    const float retargetDetector = 1.0f;
    const float expectedTarget =
        LegacyFloatCompressorTargetGain(
            expectedParameters, retargetDetector);
    const float expectedGain =
        ReferenceCompressorEnvelopeStep(
            expectedParameters, retargeted.currentGain,
            expectedTarget);
    float output = 1.0f;
    channels[0] = &output;
    harken::CompressorProcessWithDetector(
        &retargeted, channels, 1, &retargetDetector, 0, 1);
    EXPECT_EQ(0, std::memcmp(&expectedGain,
                             &retargeted.currentGain,
                             sizeof(expectedGain)));
}

TEST(CompressorKernel, NeutralRatioAndMakeupAreBitTransparent) {
    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);

    harken::CompressorParameters parameters{};
    parameters.thresholdDb = -18.0f;
    parameters.ratio = 1.0f;
    parameters.attackMs = 10.0f;
    parameters.releaseMs = 100.0f;
    parameters.kneeDb = 6.0f;
    parameters.makeupDb = 0.0f;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(parameters, kRate, &prepared));
    harken::CompressorApplyPreparedParameters(&state, prepared);

    std::vector<float> left = {
        0.0f, 0.125f, -0.25f, 0.75f, -1.0f, 0.0625f};
    std::vector<float> right = {
        -0.25f, 0.5f, 0.0f, -0.125f, 1.0f, -0.375f};
    const std::vector<float> expectedLeft = left;
    const std::vector<float> expectedRight = right;
    float* channels[2] = {left.data(), right.data()};

    harken::CompressorProcess(&state, channels, 2, 0,
                             static_cast<std::uint32_t>(left.size()));

    EXPECT_EQ(0, std::memcmp(left.data(), expectedLeft.data(),
                             left.size() * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(right.data(), expectedRight.data(),
                             right.size() * sizeof(float)));
    EXPECT_FLOAT_EQ(1.0f, state.currentGain);
}

TEST(CompressorKernel,
     HardKneeAttackReleaseAndLinkingMatchIndependentReference) {
    constexpr double kThresholdDb = -12.0;
    constexpr double kRatio = 4.0;
    constexpr double kAttackMs = 10.0;
    constexpr double kReleaseMs = 100.0;

    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);
    harken::CompressorParameters parameters{};
    parameters.thresholdDb = static_cast<float>(kThresholdDb);
    parameters.ratio = static_cast<float>(kRatio);
    parameters.attackMs = static_cast<float>(kAttackMs);
    parameters.releaseMs = static_cast<float>(kReleaseMs);
    parameters.kneeDb = 0.0f;
    parameters.makeupDb = 0.0f;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(parameters, kRate, &prepared));
    harken::CompressorApplyPreparedParameters(&state, prepared);

    std::vector<float> left(960, 0.01f);
    std::fill(left.begin(), left.begin() + 480, 1.0f);
    std::vector<float> right(left.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        right[i] = 0.5f * left[i];
    }
    const std::vector<float> input = left;

    const double attackCoeff = std::exp(
        -1.0 / (kAttackMs * 0.001 * static_cast<double>(kRate)));
    const double releaseCoeff = std::exp(
        -1.0 / (kReleaseMs * 0.001 * static_cast<double>(kRate)));
    double referenceGain = 1.0;
    double referenceMinimumGain = 1.0;
    double referenceOutputPeak = 0.0;
    std::vector<float> expected(left.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double inputDb = 20.0 * std::log10(input[i]);
        double targetGain = 1.0;
        if (inputDb > kThresholdDb) {
            const double outputDb =
                kThresholdDb + (inputDb - kThresholdDb) / kRatio;
            targetGain = std::pow(10.0, (outputDb - inputDb) / 20.0);
        }
        const double coeff =
            targetGain < referenceGain ? attackCoeff : releaseCoeff;
        referenceGain =
            coeff * referenceGain + (1.0 - coeff) * targetGain;
        referenceMinimumGain =
            std::min(referenceMinimumGain, referenceGain);
        expected[i] =
            static_cast<float>(static_cast<double>(input[i]) * referenceGain);
        referenceOutputPeak =
            std::max(referenceOutputPeak,
                     std::abs(static_cast<double>(expected[i])));
    }

    float* channels[2] = {left.data(), right.data()};
    harken::CompressorProcess(&state, channels, 2, 0,
                             static_cast<std::uint32_t>(left.size()));

    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(left[i], expected[i], 2.0e-5f) << "sample " << i;
        EXPECT_NEAR(right[i], 0.5f * expected[i], 2.0e-5f)
            << "linked sample " << i;
    }
    EXPECT_NEAR(state.currentGain, referenceGain, 2.0e-5);
    EXPECT_FLOAT_EQ(1.0f, state.telemetry.mainInputPeak);
    EXPECT_FLOAT_EQ(1.0f, state.telemetry.detectorPeak);
    EXPECT_NEAR(state.telemetry.outputPeak, referenceOutputPeak, 2.0e-5);
    EXPECT_NEAR(
        state.telemetry.maxGainReductionDb,
        -20.0 * std::log10(referenceMinimumGain), 2.0e-4);
    EXPECT_NEAR(
        state.telemetry.endGainReductionDb,
        -20.0 * std::log10(referenceGain), 2.0e-4);
}

TEST(CompressorKernel, SoftKneeIsBlockSplitInvariant) {
    harken::CompressorParameters parameters{};
    parameters.thresholdDb = -18.0f;
    parameters.ratio = 6.0f;
    parameters.attackMs = 3.0f;
    parameters.releaseMs = 75.0f;
    parameters.kneeDb = 12.0f;
    parameters.makeupDb = 2.0f;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(parameters, kRate, &prepared));

    harken::CompressorState whole{};
    harken::CompressorState split{};
    harken::CompressorInit(&whole, kRate);
    harken::CompressorInit(&split, kRate);
    harken::CompressorApplyPreparedParameters(&whole, prepared);
    harken::CompressorApplyPreparedParameters(&split, prepared);

    std::vector<float> input(777);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = (i % 17 == 0) ? 1.0f
                                 : static_cast<float>((i % 31) + 1) / 64.0f;
    }
    std::vector<float> a = input;
    std::vector<float> b = input;
    float* wholeChannels[1] = {a.data()};
    float* splitChannels[1] = {b.data()};
    harken::CompressorBeginBlock(&whole);
    harken::CompressorBeginBlock(&split);
    harken::CompressorProcess(&whole, wholeChannels, 1, 0,
                             static_cast<std::uint32_t>(a.size()));
    harken::CompressorProcess(&split, splitChannels, 1, 0, 113);
    harken::CompressorProcess(&split, splitChannels, 1, 113, 257);
    harken::CompressorProcess(
        &split, splitChannels, 1, 370,
        static_cast<std::uint32_t>(b.size() - 370));

    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(float)));
    EXPECT_FLOAT_EQ(whole.currentGain, split.currentGain);
    EXPECT_FLOAT_EQ(whole.telemetry.blockMinimumLinearGain,
                    split.telemetry.blockMinimumLinearGain);
    EXPECT_FLOAT_EQ(whole.lastAppliedWetGain, split.lastAppliedWetGain);
    EXPECT_FLOAT_EQ(whole.telemetry.mainInputPeak,
                    split.telemetry.mainInputPeak);
    EXPECT_FLOAT_EQ(whole.telemetry.detectorPeak,
                    split.telemetry.detectorPeak);
    EXPECT_FLOAT_EQ(whole.telemetry.outputPeak,
                    split.telemetry.outputPeak);
    EXPECT_FLOAT_EQ(whole.telemetry.maxGainReductionDb,
                    split.telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(whole.telemetry.endGainReductionDb,
                    split.telemetry.endGainReductionDb);
    EXPECT_EQ(whole.telemetry.detectorNonFiniteCount,
              split.telemetry.detectorNonFiniteCount);
    EXPECT_EQ(whole.telemetry.detectorSaturationCount,
              split.telemetry.detectorSaturationCount);
    EXPECT_EQ(whole.telemetry.resetThisBlock,
              split.telemetry.resetThisBlock);
    EXPECT_EQ(whole.telemetry.resetBridgeActive,
              split.telemetry.resetBridgeActive);

    const harken::CompressorBlockTelemetry beforeZeroFrames =
        split.telemetry;
    const float gainBeforeZeroFrames = split.currentGain;
    const float appliedBeforeZeroFrames = split.lastAppliedWetGain;
    harken::CompressorProcess(&split, splitChannels, 1, 0, 0);
    EXPECT_FLOAT_EQ(gainBeforeZeroFrames, split.currentGain);
    EXPECT_FLOAT_EQ(appliedBeforeZeroFrames, split.lastAppliedWetGain);
    EXPECT_FLOAT_EQ(beforeZeroFrames.mainInputPeak,
                    split.telemetry.mainInputPeak);
    EXPECT_FLOAT_EQ(beforeZeroFrames.detectorPeak,
                    split.telemetry.detectorPeak);
    EXPECT_FLOAT_EQ(beforeZeroFrames.outputPeak,
                    split.telemetry.outputPeak);
    EXPECT_FLOAT_EQ(beforeZeroFrames.maxGainReductionDb,
                    split.telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(beforeZeroFrames.endGainReductionDb,
                    split.telemetry.endGainReductionDb);

    harken::CompressorBeginBlock(&split);
    const float bypassEndGainReductionDb =
        gainBeforeZeroFrames < 1.0f
            ? -20.0f * std::log10(gainBeforeZeroFrames)
            : 0.0f;
    harken::CompressorProcess(&split, splitChannels, 1, 0, 0);
    EXPECT_FLOAT_EQ(0.0f, split.telemetry.mainInputPeak);
    EXPECT_FLOAT_EQ(0.0f, split.telemetry.detectorPeak);
    EXPECT_FLOAT_EQ(0.0f, split.telemetry.outputPeak);
    EXPECT_FLOAT_EQ(0.0f, split.telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(bypassEndGainReductionDb,
                    split.telemetry.endGainReductionDb);

    harken::CompressorReset(&split);
    EXPECT_FLOAT_EQ(1.0f, split.currentGain);
    harken::CompressorBeginBlock(&split);
    harken::CompressorProcess(&split, splitChannels, 1, 0, 0);
    EXPECT_TRUE(split.telemetry.resetThisBlock);
    EXPECT_FALSE(split.telemetry.resetBridgeActive);
    EXPECT_FLOAT_EQ(0.0f, split.telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(0.0f, split.telemetry.endGainReductionDb);
}

TEST(CompressorKernel, PreparedTransitionIsExactAndRetargetable) {
    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);

    float neutralSample = 0.0f;
    float* neutralChannels[1] = {&neutralSample};
    harken::CompressorProcess(&state, neutralChannels, 1, 0, 1);

    harken::CompressorParameters first{};
    first.thresholdDb = -42.0f;
    first.ratio = 4.0f;
    first.attackMs = 20.0f;
    first.releaseMs = 400.0f;
    first.kneeDb = 18.0f;
    first.makeupDb = 12.0f;
    harken::CompressorPreparedParameters firstPrepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(first, kRate, &firstPrepared));

    const std::uint32_t transitionFrames =
        harken::CompressorTransitionFrames(kRate);
    ASSERT_EQ(480u, transitionFrames);
    harken::CompressorRequestPreparedParameters(
        &state, firstPrepared, transitionFrames);

    std::vector<float> silence(240, 0.0f);
    float* channels[1] = {silence.data()};
    harken::CompressorProcess(&state, channels, 1, 0, 240);

    EXPECT_EQ(240u, state.parameterTransitionFramesRemaining);
    EXPECT_NEAR(-30.0f, state.parameters.thresholdDb, 2.5e-4f);
    EXPECT_NEAR(0.5f * (1.0f + firstPrepared.makeupLinear),
                state.parameters.makeupLinear, 2.0e-5f);

    harken::CompressorParameters second = first;
    second.thresholdDb = -60.0f;
    second.makeupDb = -6.0f;
    harken::CompressorPreparedParameters secondPrepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(second, kRate, &secondPrepared));
    const float retargetThreshold = state.parameters.thresholdDb;
    const float retargetMakeup = state.parameters.makeupLinear;
    harken::CompressorRequestPreparedParameters(
        &state, secondPrepared, transitionFrames);
    EXPECT_FLOAT_EQ(retargetThreshold, state.parameters.thresholdDb);
    EXPECT_FLOAT_EQ(retargetMakeup, state.parameters.makeupLinear);

    silence.assign(transitionFrames, 0.0f);
    channels[0] = silence.data();
    harken::CompressorProcess(
        &state, channels, 1, 0,
        static_cast<std::uint32_t>(silence.size()));
    EXPECT_EQ(0u, state.parameterTransitionFramesRemaining);
    EXPECT_FLOAT_EQ(secondPrepared.thresholdDb,
                    state.parameters.thresholdDb);
    EXPECT_FLOAT_EQ(secondPrepared.makeupLinear,
                    state.parameters.makeupLinear);
}

TEST(CompressorKernel, ExternalDetectorIsControlOnly) {
    harken::CompressorParameters parameters{};
    parameters.thresholdDb = -24.0f;
    parameters.ratio = 20.0f;
    parameters.attackMs = 0.1f;
    parameters.releaseMs = 100.0f;
    parameters.kneeDb = 0.0f;
    parameters.makeupDb = 0.0f;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(parameters, kRate, &prepared));

    harken::CompressorState self{};
    harken::CompressorState external{};
    harken::CompressorInit(&self, kRate);
    harken::CompressorInit(&external, kRate);
    harken::CompressorApplyPreparedParameters(&self, prepared);
    harken::CompressorApplyPreparedParameters(&external, prepared);

    std::vector<float> selfAudio(kBlock, 0.05f);
    std::vector<float> externalAudio = selfAudio;
    std::vector<float> detector(kBlock, 1.0f);
    float* selfChannels[1] = {selfAudio.data()};
    float* externalChannels[1] = {externalAudio.data()};

    harken::CompressorProcess(&self, selfChannels, 1, 0, kBlock);
    harken::CompressorProcessWithDetector(
        &external, externalChannels, 1, detector.data(), 0, kBlock);

    EXPECT_FLOAT_EQ(0.05f, selfAudio.back());
    EXPECT_LT(externalAudio.back(), 0.025f);
    EXPECT_FLOAT_EQ(1.0f, detector.back());

    std::vector<float> silence(kBlock, 0.0f);
    float* silentChannels[1] = {silence.data()};
    harken::CompressorProcessWithDetector(
        &external, silentChannels, 1, detector.data(), 0, kBlock);
    for (float sample : silence) {
        EXPECT_FLOAT_EQ(0.0f, sample);
    }
}

TEST(CompressorKernel, ResetBridgeIsContinuousAndReportsFreshEnvelope) {
    harken::CompressorParameters parameters{};
    parameters.thresholdDb = -36.0f;
    parameters.ratio = 20.0f;
    parameters.attackMs = 0.1f;
    parameters.releaseMs = 1000.0f;
    parameters.kneeDb = 0.0f;
    parameters.makeupDb = 0.0f;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(parameters, kRate, &prepared));

    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);
    harken::CompressorApplyPreparedParameters(&state, prepared);
    std::vector<float> compressed(512, 1.0f);
    float* compressedChannels[1] = {compressed.data()};
    harken::CompressorProcess(
        &state, compressedChannels, 1, 0,
        static_cast<std::uint32_t>(compressed.size()));
    const float preResetAppliedGain = state.lastAppliedWetGain;
    ASSERT_LT(preResetAppliedGain, 0.2f);

    harken::CompressorRequestReset(
        &state, harken::CompressorTransitionFrames(kRate));
    harken::CompressorBeginBlock(&state);
    EXPECT_FLOAT_EQ(1.0f, state.currentGain);
    EXPECT_EQ(480u, state.resetBridgeFramesRemaining);

    std::vector<float> bridged(480, 1.0f);
    std::vector<float> silentDetector(480, 0.0f);
    float* bridgedChannels[1] = {bridged.data()};
    harken::CompressorProcessWithDetector(
        &state, bridgedChannels, 1, silentDetector.data(), 0,
        static_cast<std::uint32_t>(bridged.size()));

    EXPECT_NEAR(preResetAppliedGain, bridged.front(), 2.0e-6f);
    EXPECT_FLOAT_EQ(1.0f, bridged.back());
    EXPECT_EQ(0u, state.resetBridgeFramesRemaining);
    EXPECT_TRUE(state.telemetry.resetThisBlock);
    EXPECT_TRUE(state.telemetry.resetBridgeActive);
    EXPECT_NEAR(0.0f, state.telemetry.endGainReductionDb, 1.0e-6f);
}

TEST(CompressorKernel, NonFiniteExternalDetectorCannotPoisonState) {
    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);
    std::vector<float> audio = {0.25f, -0.25f, 0.25f, -0.25f};
    std::vector<float> detector = {
        (std::numeric_limits<float>::quiet_NaN)(),
        (std::numeric_limits<float>::infinity)(),
        0.5f,
        0.0f};
    float* channels[1] = {audio.data()};

    harken::CompressorBeginBlock(&state);
    harken::CompressorProcessWithDetector(
        &state, channels, 1, detector.data(), 0,
        static_cast<std::uint32_t>(audio.size()));

    EXPECT_EQ(2u, state.telemetry.detectorNonFiniteCount);
    EXPECT_TRUE(std::isfinite(state.currentGain));
    EXPECT_TRUE(std::isfinite(state.lastAppliedWetGain));
    for (float sample : audio) {
        EXPECT_TRUE(std::isfinite(sample));
    }
}

TEST(CompressorKernel,
     OutputPeakScanMatchesNonFiniteSampleReferenceAfterAppliedGain) {
    constexpr float kMakeupDb = -6.0f;
    harken::CompressorParameters parameters{};
    parameters.ratio = 1.0f;
    parameters.makeupDb = kMakeupDb;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(parameters, kRate, &prepared));

    harken::CompressorState state{};
    harken::CompressorInit(&state, kRate);
    harken::CompressorApplyPreparedParameters(&state, prepared);
    std::vector<float> left = {
        0.25f,
        (std::numeric_limits<float>::quiet_NaN)(),
        (std::numeric_limits<float>::infinity)(),
        -(std::numeric_limits<float>::infinity)(),
        -0.5f};
    std::vector<float> right = {
        -0.125f,
        0.75f,
        -(std::numeric_limits<float>::infinity)(),
        (std::numeric_limits<float>::infinity)(),
        (std::numeric_limits<float>::quiet_NaN)()};
    const std::vector<float> detector = {
        (std::numeric_limits<float>::quiet_NaN)(),
        (std::numeric_limits<float>::infinity)(),
        -0.5f,
        0.25f,
        0.0f};
    std::vector<float> expectedLeft = left;
    std::vector<float> expectedRight = right;
    const float expectedGain = std::pow(10.0f, kMakeupDb / 20.0f);
    float expectedOutputPeak = 0.0f;
    for (std::size_t i = 0; i < expectedLeft.size(); ++i) {
        expectedLeft[i] *= expectedGain;
        expectedRight[i] *= expectedGain;
        for (float output : {expectedLeft[i], expectedRight[i]}) {
            const float magnitude = output >= 0.0f ? output : -output;
            if (std::isfinite(magnitude) &&
                magnitude > expectedOutputPeak) {
                expectedOutputPeak = magnitude;
            }
        }
    }

    float* channels[2] = {left.data(), right.data()};
    harken::CompressorBeginBlock(&state);
    harken::CompressorProcessWithDetector(
        &state, channels, 2, detector.data(), 0,
        static_cast<std::uint32_t>(left.size()));

    const auto expectSameFloat = [](float actual, float expected,
                                    std::size_t sample,
                                    const char* channel) {
        if (std::isnan(expected)) {
            EXPECT_TRUE(std::isnan(actual))
                << channel << " sample " << sample;
        } else if (std::isinf(expected)) {
            EXPECT_TRUE(std::isinf(actual))
                << channel << " sample " << sample;
            EXPECT_EQ(std::signbit(expected), std::signbit(actual))
                << channel << " sample " << sample;
        } else {
            EXPECT_FLOAT_EQ(expected, actual)
                << channel << " sample " << sample;
        }
    };
    for (std::size_t i = 0; i < left.size(); ++i) {
        expectSameFloat(left[i], expectedLeft[i], i, "left");
        expectSameFloat(right[i], expectedRight[i], i, "right");
    }
    EXPECT_FLOAT_EQ(0.75f, state.telemetry.mainInputPeak);
    EXPECT_FLOAT_EQ(0.5f, state.telemetry.detectorPeak);
    EXPECT_FLOAT_EQ(expectedOutputPeak, state.telemetry.outputPeak);
    EXPECT_EQ(2u, state.telemetry.detectorNonFiniteCount);
    EXPECT_FLOAT_EQ(1.0f, state.currentGain);
    EXPECT_FLOAT_EQ(expectedGain, state.lastAppliedWetGain);
    EXPECT_FLOAT_EQ(0.0f, state.telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(0.0f, state.telemetry.endGainReductionDb);
}

TEST(CompressorEffect, DescriptorValidationAndPoolCapacityAreAtomic) {
    VnConfig config = harkentest::MakeNullConfig(2, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> source = harkentest::MakeDC(kBlock * 4, 0.75f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));

    VnCompressorDesc invalid = CompressorDesc();
    invalid.reserved0 = 1;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(first, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&invalid)));
    invalid = CompressorDesc();
    invalid.ratio = 20.0001f;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(first, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&invalid)));
    EXPECT_EQ(0u, session.Engine()->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));

    const VnCompressorDesc valid =
        CompressorDesc(-18.0f, 4.0f, 1.0f, 100.0f, 6.0f, 0.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         first, 0, VN_EFFECT_COMPRESSOR, AsBase(&valid)));
    EXPECT_EQ(1u, session.Engine()->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
    EXPECT_EQ(VN_NO_EFFECT,
              Vn_SetVoiceEffect(second, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&valid)));
    EXPECT_EQ(1u, session.Engine()->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
}

TEST(CompressorEffect,
     VoiceFirstInstallSnapsMakeupWithoutStartingResetBridge) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kInput = 0.125f;
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 2, kInput);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(41), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(41));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnCompressorDesc compressor =
        CompressorDesc(-18.0f, 1.0f, 10.0f, 100.0f, 6.0f, 6.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(voice, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&compressor)));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    const float expected =
        kInput * std::pow(10.0f, compressor.makeupDb / 20.0f);
    ASSERT_EQ(kBlock, rendered.left.size());
    EXPECT_NEAR(expected, rendered.left.front(), 1.0e-6f);

    harken::VoiceRenderContext* render = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), voice));
    ASSERT_NE(nullptr, render);
    auto* state =
        static_cast<harken::CompressorState*>(render->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_EQ(0u, state->resetBridgeTotalFrames);
    EXPECT_EQ(0u, state->resetBridgeFramesRemaining);
    EXPECT_FALSE(state->telemetry.resetThisBlock);
    EXPECT_FALSE(state->telemetry.resetBridgeActive);
}

TEST(CompressorEffect,
     BusFirstInstallSnapsMakeupWithoutStartingResetBridge) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const VnCompressorDesc compressor =
        CompressorDesc(-18.0f, 1.0f, 10.0f, 100.0f, 6.0f, 6.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    constexpr float kInput = 0.125f;
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 2, kInput);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(42), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(42));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    const float expected =
        kInput * std::pow(10.0f, compressor.makeupDb / 20.0f);
    ASSERT_EQ(kBlock, rendered.left.size());
    EXPECT_NEAR(expected, rendered.left.front(), 1.0e-6f);

    harken::BusTree::Bus* master = session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, master);
    auto* state =
        static_cast<harken::CompressorState*>(master->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_EQ(0u, state->resetBridgeTotalFrames);
    EXPECT_EQ(0u, state->resetBridgeFramesRemaining);
    EXPECT_FALSE(state->telemetry.resetThisBlock);
    EXPECT_FALSE(state->telemetry.resetBridgeActive);
}

TEST(CompressorEffect,
     VoiceCapacityOneRecycleStartsNeutralAndTelemetryClean) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kInput = 0.5f;
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 8, kInput);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(43), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(43));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnCompressorDesc strong =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 1000.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(voice, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&strong)));

    Capture dirtied;
    harkentest::PumpAndCapture(session.Engine(), 2, dirtied);
    harken::VoiceRenderContext* render = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), voice));
    ASSERT_NE(nullptr, render);
    auto* dirtyState =
        static_cast<harken::CompressorState*>(render->fxSlots[0].state);
    ASSERT_NE(nullptr, dirtyState);
    ASSERT_LT(dirtyState->currentGain, 0.2f);
    const harken::EffectInstanceRef dirtyRef = render->fxSlotEffectRef[0];

    ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(voice, 0));
    Capture cleared;
    harkentest::PumpAndCapture(session.Engine(), 1, cleared);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u,
              session.Engine()->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));

    const VnCompressorDesc neutral = CompressorDesc();
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(voice, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&neutral)));
    Capture reused;
    harkentest::PumpAndCapture(session.Engine(), 1, reused);
    ASSERT_EQ(kBlock, reused.left.size());
    const std::vector<float> expected(kBlock, kInput);
    EXPECT_EQ(0, std::memcmp(expected.data(), reused.left.data(),
                             expected.size() * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(expected.data(), reused.right.data(),
                             expected.size() * sizeof(float)));

    render = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), voice));
    ASSERT_NE(nullptr, render);
    auto* reusedState =
        static_cast<harken::CompressorState*>(render->fxSlots[0].state);
    ASSERT_EQ(dirtyState, reusedState);
    EXPECT_EQ(dirtyRef.poolIndex, render->fxSlotEffectRef[0].poolIndex);
    EXPECT_NE(dirtyRef.generation, render->fxSlotEffectRef[0].generation);
    EXPECT_FLOAT_EQ(1.0f, reusedState->currentGain);
    EXPECT_EQ(0u, reusedState->resetBridgeFramesRemaining);
    EXPECT_FALSE(reusedState->telemetry.resetThisBlock);
    EXPECT_FALSE(reusedState->telemetry.resetBridgeActive);
    EXPECT_FLOAT_EQ(0.0f, reusedState->telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(0.0f, reusedState->telemetry.endGainReductionDb);

    VnCompressorRuntimeInfo runtime{};
    runtime.structSize = sizeof runtime;
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureCompressorRuntimeInfo(&runtime, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_EQ(VN_EFFECT_HOST_SCOPE_VOICE, runtime.hostScope);
    EXPECT_EQ(voice, runtime.instanceHandle);
    EXPECT_EQ(0u, runtime.stateFlags & VN_COMPRESSOR_RUNTIME_STALE);
    EXPECT_EQ(0u,
              runtime.stateFlags & VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK);
    EXPECT_EQ(0u, runtime.stateFlags & VN_COMPRESSOR_RUNTIME_RESET_BRIDGE);
    EXPECT_FLOAT_EQ(0.0f, runtime.maxGainReductionDb);
    EXPECT_FLOAT_EQ(0.0f, runtime.endGainReductionDb);
}

TEST(CompressorEffect,
     BusCapacityOneRecycleStartsNeutralAndTelemetryClean) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kInput = 0.5f;
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 8, kInput);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(44), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(44));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnCompressorDesc strong =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 1000.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&strong)));

    Capture dirtied;
    harkentest::PumpAndCapture(session.Engine(), 2, dirtied);
    harken::BusTree::Bus* master = session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, master);
    auto* dirtyState =
        static_cast<harken::CompressorState*>(master->fxSlots[0].state);
    ASSERT_NE(nullptr, dirtyState);
    ASSERT_LT(dirtyState->currentGain, 0.2f);
    const harken::EffectInstanceRef dirtyRef = master->fxSlotEffectRef[0];

    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(VN_BUS_MASTER, 0));
    Capture cleared;
    harkentest::PumpAndCapture(session.Engine(), 1, cleared);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u,
              session.Engine()->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));

    const VnCompressorDesc neutral = CompressorDesc();
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&neutral)));
    Capture reused;
    harkentest::PumpAndCapture(session.Engine(), 1, reused);
    ASSERT_EQ(kBlock, reused.left.size());
    const std::vector<float> expected(kBlock, kInput);
    EXPECT_EQ(0, std::memcmp(expected.data(), reused.left.data(),
                             expected.size() * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(expected.data(), reused.right.data(),
                             expected.size() * sizeof(float)));

    master = session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, master);
    auto* reusedState =
        static_cast<harken::CompressorState*>(master->fxSlots[0].state);
    ASSERT_EQ(dirtyState, reusedState);
    EXPECT_EQ(dirtyRef.poolIndex, master->fxSlotEffectRef[0].poolIndex);
    EXPECT_NE(dirtyRef.generation, master->fxSlotEffectRef[0].generation);
    EXPECT_FLOAT_EQ(1.0f, reusedState->currentGain);
    EXPECT_EQ(0u, reusedState->resetBridgeFramesRemaining);
    EXPECT_FALSE(reusedState->telemetry.resetThisBlock);
    EXPECT_FALSE(reusedState->telemetry.resetBridgeActive);
    EXPECT_FLOAT_EQ(0.0f, reusedState->telemetry.maxGainReductionDb);
    EXPECT_FLOAT_EQ(0.0f, reusedState->telemetry.endGainReductionDb);

    VnCompressorRuntimeInfo runtime{};
    runtime.structSize = sizeof runtime;
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureCompressorRuntimeInfo(&runtime, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_EQ(VN_EFFECT_HOST_SCOPE_BUS, runtime.hostScope);
    EXPECT_EQ(VN_BUS_MASTER, runtime.objectId);
    EXPECT_EQ(0u, runtime.stateFlags & VN_COMPRESSOR_RUNTIME_STALE);
    EXPECT_EQ(0u,
              runtime.stateFlags & VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK);
    EXPECT_EQ(0u, runtime.stateFlags & VN_COMPRESSOR_RUNTIME_RESET_BRIDGE);
    EXPECT_FLOAT_EQ(0.0f, runtime.maxGainReductionDb);
    EXPECT_FLOAT_EQ(0.0f, runtime.endGainReductionDb);
}

TEST(CompressorEffect, ConfigVintageDefaultsAndNewTailValidation) {
    {
        VnConfig phase1 = harkentest::MakeNullConfig(1, kBlock, kRate);
        phase1.structSize = static_cast<std::uint32_t>(
            offsetof(VnConfig, compressorEffectPoolCapacity));
        phase1.compressorEffectPoolCapacity = 9999;
        phase1.dynamicRouteCapacity = 9999;
        phase1.dynamicRoutePlanPoolCapacity = 1;
        EngineSession session(phase1);
        ASSERT_EQ(VN_OK, session.InitResult());
        EXPECT_EQ(16u, session.Engine()->config.compressorEffectPoolCapacity);
        EXPECT_EQ(64u, session.Engine()->config.dynamicRouteCapacity);
        EXPECT_EQ(4u,
                  session.Engine()->config.dynamicRoutePlanPoolCapacity);
        EXPECT_EQ(16u, session.Engine()->config.dynamicBusTapCapacity);
        EXPECT_EQ(16u, session.Engine()->config.dynamicVoiceTapCapacity);
    }
    {
        VnConfig invalid = harkentest::MakeNullConfig(1, kBlock, kRate);
        invalid.dynamicRoutePlanPoolCapacity = 1;
        EngineSession session(invalid);
        EXPECT_EQ(VN_INVALID_CONFIG, session.InitResult());
    }
    {
        VnConfig invalid = harkentest::MakeNullConfig(1, kBlock, kRate);
        invalid.reserved9 = 1;
        EngineSession session(invalid);
        EXPECT_EQ(VN_INVALID_ARG, session.InitResult());
    }
    {
        VnConfig invalid = harkentest::MakeNullConfig(1, kBlock, kRate);
        invalid.dynamicRouteCapacity = 16384;
        invalid.dynamicRoutePlanPoolCapacity = 4096;
        invalid.dynamicBusTapCapacity = 4096;
        invalid.dynamicVoiceTapCapacity = 4096;
        EngineSession session(invalid);
        EXPECT_EQ(VN_INVALID_CONFIG, session.InitResult());
    }
}

TEST(CompressorEffect, LiveCommandsCarryOneCompleteRetargetableVector) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> source = harkentest::MakeDC(kBlock * 8, 0.75f);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnCompressorDesc neutral = CompressorDesc();
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, 0, VN_EFFECT_COMPRESSOR, AsBase(&neutral)));

    Capture firstBlock;
    harkentest::PumpAndCapture(session.Engine(), 1, firstBlock);
    harken::VoiceRenderContext* render = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, render);
    ASSERT_EQ(VN_EFFECT_COMPRESSOR, render->fxSlotEffectRef[0].type);
    auto* state =
        static_cast<harken::CompressorState*>(render->fxSlots[0].state);
    ASSERT_NE(nullptr, state);

    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0, harken::kCompressorParamThresholdDb,
                         -24.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0, harken::kCompressorParamRatio, 8.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0, harken::kCompressorParamAttackMs, 20.0f));

    Capture transitioned;
    harkentest::PumpAndCapture(session.Engine(), 1, transitioned);

    harken::CompressorParameters authored{};
    authored.thresholdDb = -24.0f;
    authored.ratio = 8.0f;
    authored.attackMs = 20.0f;
    authored.releaseMs = 100.0f;
    authored.kneeDb = 6.0f;
    authored.makeupDb = 0.0f;
    harken::CompressorPreparedParameters expected{};
    ASSERT_TRUE(
        harken::PrepareCompressorParameters(authored, kRate, &expected));
    EXPECT_FLOAT_EQ(expected.thresholdDb, state->targetParameters.thresholdDb);
    EXPECT_FLOAT_EQ(expected.reciprocalRatio,
                    state->targetParameters.reciprocalRatio);
    EXPECT_FLOAT_EQ(expected.attackCoeff,
                    state->targetParameters.attackCoeff);
    EXPECT_FLOAT_EQ(expected.releaseCoeff,
                    state->targetParameters.releaseCoeff);
    EXPECT_FLOAT_EQ(expected.kneeDb, state->targetParameters.kneeDb);
    EXPECT_FLOAT_EQ(expected.makeupLinear,
                    state->targetParameters.makeupLinear);
    EXPECT_EQ(harken::CompressorTransitionFrames(kRate) - kBlock,
              state->parameterTransitionFramesRemaining);
    EXPECT_LT(harkentest::Peak(transitioned.left),
              harkentest::Peak(firstBlock.left));
}

TEST(CompressorEffect,
     VoiceLiveUpdateSteadyTargetAndResetMatchLegacyOracle) {
    constexpr float kSourceLevel = 0.75f;
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 20, kSourceLevel);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(241), &pcm));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(241));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnCompressorDesc neutral = CompressorDesc();
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, 0, VN_EFFECT_COMPRESSOR,
                         AsBase(&neutral)));
    Capture discarded;
    harkentest::PumpAndCapture(session.Engine(), 1, discarded);

    harken::VoiceRenderContext* render =
        session.Engine()->voices.Resolve(
            harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, render);
    auto* state =
        static_cast<harken::CompressorState*>(
            render->fxSlots[0].state);
    ASSERT_NE(nullptr, state);

    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0,
                         harken::kCompressorParamThresholdDb, -24.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0, harken::kCompressorParamRatio, 4.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0,
                         harken::kCompressorParamAttackMs, 0.1f));
    discarded = Capture{};
    harkentest::PumpAndCapture(session.Engine(), 1, discarded);
    ASSERT_GT(state->parameterTransitionFramesRemaining, 0u);

    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0, harken::kCompressorParamRatio, 8.0f));
    discarded = Capture{};
    harkentest::PumpAndCapture(session.Engine(), 2, discarded);

    harken::CompressorParameters authored{};
    authored.thresholdDb = -24.0f;
    authored.ratio = 8.0f;
    authored.attackMs = 0.1f;
    authored.releaseMs = 100.0f;
    authored.kneeDb = 6.0f;
    authored.makeupDb = 0.0f;
    harken::CompressorPreparedParameters expected{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        authored, kRate, &expected));
    ExpectNextPublicCompressorBlockMatchesLegacyTarget(
        session.Engine(), state, expected, kSourceLevel);

    ASSERT_EQ(VN_OK, Vn_ResetVoiceEffect(
                         handle, 0, VN_EFFECT_COMPRESSOR));
    discarded = Capture{};
    harkentest::PumpAndCapture(session.Engine(), 2, discarded);
    ExpectNextPublicCompressorBlockMatchesLegacyTarget(
        session.Engine(), state, expected, kSourceLevel);
}

TEST(CompressorEffect,
     BusLiveUpdateSteadyTargetAndResetMatchLegacyOracle) {
    constexpr float kSourceLevel = 0.75f;
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 20, kSourceLevel);
    const VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(242), &pcm));
    VnVoiceHandle handle = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(242));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnCompressorDesc neutral = CompressorDesc();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                         AsBase(&neutral)));
    Capture discarded;
    harkentest::PumpAndCapture(session.Engine(), 1, discarded);

    harken::BusTree::Bus* master =
        session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, master);
    auto* state =
        static_cast<harken::CompressorState*>(
            master->fxSlots[0].state);
    ASSERT_NE(nullptr, state);

    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         VN_BUS_MASTER, 0,
                         harken::kCompressorParamThresholdDb, -24.0f));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         VN_BUS_MASTER, 0,
                         harken::kCompressorParamRatio, 8.0f));
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         VN_BUS_MASTER, 0,
                         harken::kCompressorParamAttackMs, 0.1f));
    discarded = Capture{};
    harkentest::PumpAndCapture(session.Engine(), 2, discarded);

    harken::CompressorParameters authored{};
    authored.thresholdDb = -24.0f;
    authored.ratio = 8.0f;
    authored.attackMs = 0.1f;
    authored.releaseMs = 100.0f;
    authored.kneeDb = 6.0f;
    authored.makeupDb = 0.0f;
    harken::CompressorPreparedParameters expected{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(
        authored, kRate, &expected));
    ExpectNextPublicCompressorBlockMatchesLegacyTarget(
        session.Engine(), state, expected, kSourceLevel);

    ASSERT_EQ(VN_OK, Vn_ResetBusEffect(
                         VN_BUS_MASTER, 0,
                         VN_EFFECT_COMPRESSOR));
    discarded = Capture{};
    harkentest::PumpAndCapture(session.Engine(), 2, discarded);
    ExpectNextPublicCompressorBlockMatchesLegacyTarget(
        session.Engine(), state, expected, kSourceLevel);
}

VnEffectDynamicRouteDesc RawBusRoute(VnBusId sourceBus,
                                     std::uint32_t tap =
                                         VN_DYNAMIC_TAP_BUS_POST_EFFECTS,
                                     float levelDb = 0.0f) {
    VnEffectDynamicRouteDesc desc{};
    desc.structSize = sizeof desc;
    desc.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    desc.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    desc.tap = tap;
    desc.sourceId = sourceBus;
    desc.levelDb = levelDb;
    desc.expectedEffectType = VN_EFFECT_COMPRESSOR;
    return desc;
}

VnEffectDynamicRouteDesc RawVoiceRoute(VnVoiceHandle sourceVoice,
                                       float levelDb = 0.0f) {
    VnEffectDynamicRouteDesc desc{};
    desc.structSize = sizeof desc;
    desc.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    desc.sourceKind = VN_DYNAMIC_SOURCE_VOICE;
    desc.tap = VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL;
    desc.sourceVoiceHandle = sourceVoice;
    desc.levelDb = levelDb;
    desc.expectedEffectType = VN_EFFECT_COMPRESSOR;
    return desc;
}

TEST(CompressorDynamicRoute,
     DescriptorValidationCaptureAndBusSameBlockDetectorAreAtomic) {
    VnConfig config = harkentest::MakeNullConfig(2, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kSourceBus = harkentest::ObjectId(101);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(102);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    VnEffectDynamicRouteDesc route = RawBusRoute(kSourceBus);
    VnEffectDynamicRouteDesc invalid = route;
    invalid.reserved1 = 1;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &invalid));
    invalid = route;
    invalid.levelDb = std::numeric_limits<float>::infinity();
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &invalid));
    VnEffectDynamicRouteDesc selfRoute = RawBusRoute(kTargetBus);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_ORDER,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &selfRoute));

    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
    EXPECT_EQ(1u, required);
    VnEffectDynamicRouteInfo pending{};
    pending.structSize = sizeof pending;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&pending, 1, &required));
    EXPECT_NE(0u, pending.stateFlags & VN_DYNAMIC_ROUTE_PENDING);

    const std::vector<float> target =
        harkentest::MakeDC(kBlock * 4, 0.25f);
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 4, 1.0f);
    VnPCMDesc targetPcm = harkentest::MonoDesc(target, kRate);
    VnPCMDesc sourcePcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(201), &targetPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(202), &sourcePcm));
    VnPlayDesc targetPlay = harkentest::MakePlayDesc(harkentest::ObjectId(201));
    targetPlay.busId = kTargetBus;
    VnPlayDesc sourcePlay = harkentest::MakePlayDesc(harkentest::ObjectId(202));
    sourcePlay.busId = kSourceBus;
    VnVoiceHandle targetVoice = 0;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    const std::uint32_t targetIndex =
        session.Engine()->buses.FindIndex(kTargetBus);
    harken::BusTree::Bus* bus =
        session.Engine()->buses.BusState(targetIndex);
    ASSERT_NE(nullptr, bus);
    auto* state =
        static_cast<harken::CompressorState*>(bus->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_NEAR(0.25f, state->telemetry.mainInputPeak, 1.0e-6f);
    EXPECT_NEAR(1.0f, state->telemetry.detectorPeak, 1.0e-6f);

    VnEffectDynamicRouteInfo active{};
    active.structSize = sizeof active;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&active, 1, &required));
    EXPECT_NE(0u, active.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_INACTIVE_NONE, active.inactiveReason);

    ASSERT_EQ(VN_OK, Vn_ClearBusEffectDynamicRoute(
                         kTargetBus, 0, 0, VN_EFFECT_COMPRESSOR));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    EXPECT_NEAR(0.25f, state->telemetry.detectorPeak, 1.0e-6f);
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
    EXPECT_EQ(0u, required);
}

TEST(CompressorDynamicRoute,
     VoiceRouteIsFirstBlockSilentThenReadsStrictlyPreviousBlock) {
    VnConfig config = harkentest::MakeNullConfig(2, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kSourceBus = harkentest::ObjectId(111);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(112);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    const std::vector<float> target =
        harkentest::MakeDC(kBlock * 5, 0.25f);
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 5, 1.0f);
    VnPCMDesc targetPcm = harkentest::MonoDesc(target, kRate);
    VnPCMDesc sourcePcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(211), &targetPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(212), &sourcePcm));
    VnPlayDesc targetPlay = harkentest::MakePlayDesc(harkentest::ObjectId(211));
    targetPlay.busId = kTargetBus;
    VnPlayDesc sourcePlay = harkentest::MakePlayDesc(harkentest::ObjectId(212));
    sourcePlay.busId = kSourceBus;
    VnVoiceHandle targetVoice = 0;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
    VnEffectDynamicRouteDesc route = RawVoiceRoute(sourceVoice);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    const std::uint32_t targetIndex =
        session.Engine()->buses.FindIndex(kTargetBus);
    harken::BusTree::Bus* bus =
        session.Engine()->buses.BusState(targetIndex);
    ASSERT_NE(nullptr, bus);
    auto* state =
        static_cast<harken::CompressorState*>(bus->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_FLOAT_EQ(0.0f, state->telemetry.detectorPeak)
        << "an authored external route must not fall back to self";

    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo first{};
    first.structSize = sizeof first;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&first, 1, &required));
    EXPECT_NE(0u, first.stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);

    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    EXPECT_NEAR(1.0f, state->telemetry.detectorPeak, 1.0e-6f);
    VnEffectDynamicRouteInfo second{};
    second.structSize = sizeof second;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&second, 1, &required));
    EXPECT_NE(0u, second.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
}

TEST(CompressorDynamicRoute,
     ImmediateVoiceStopInvalidatesTheReadBankInTheAppliedBlock) {
    for (std::uint32_t workers : {0u, 1u, 2u, 4u}) {
    SCOPED_TRACE(::testing::Message()
                 << "renderWorkerCount=" << workers);
    VnConfig config = harkentest::MakeNullConfig(2, kBlock, kRate);
    config.renderWorkerCount = workers;
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kSourceBus = harkentest::ObjectId(115);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(116);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    const std::vector<float> target =
        harkentest::MakeDC(kBlock * 6, 0.25f);
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 6, 1.0f);
    VnPCMDesc targetPcm = harkentest::MonoDesc(target, kRate);
    VnPCMDesc sourcePcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(215), &targetPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(216), &sourcePcm));
    VnPlayDesc targetPlay = harkentest::MakePlayDesc(harkentest::ObjectId(215));
    targetPlay.busId = kTargetBus;
    VnPlayDesc sourcePlay = harkentest::MakePlayDesc(harkentest::ObjectId(216));
    sourcePlay.busId = kSourceBus;
    VnVoiceHandle targetVoice = 0;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
    const VnEffectDynamicRouteDesc route = RawVoiceRoute(sourceVoice);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 2, rendered);
    const std::uint32_t targetIndex =
        session.Engine()->buses.FindIndex(kTargetBus);
    harken::BusTree::Bus* bus =
        session.Engine()->buses.BusState(targetIndex);
    ASSERT_NE(nullptr, bus);
    auto* state =
        static_cast<harken::CompressorState*>(bus->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_NEAR(1.0f, state->telemetry.detectorPeak, 1.0e-6f);

    ASSERT_EQ(VN_OK, Vn_Stop(sourceVoice, 0));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    EXPECT_FLOAT_EQ(0.0f, state->telemetry.detectorPeak);

    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo stopped{};
    stopped.structSize = sizeof stopped;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&stopped, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_NE(0u, stopped.stateFlags & VN_DYNAMIC_ROUTE_STALE);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_INACTIVE_SOURCE_STALE,
              stopped.inactiveReason);
    }
}

TEST(CompressorDynamicRoute,
     WholeMixerArenaFailurePublishesLiveVoiceSilenceWithoutRearming) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kSourceBus = harkentest::ObjectId(117);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(118);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 8, 1.0f);
    VnPCMDesc sourcePcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(217), &sourcePcm));
    VnPlayDesc sourcePlay = harkentest::MakePlayDesc(harkentest::ObjectId(217));
    sourcePlay.busId = kSourceBus;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
    const VnEffectDynamicRouteDesc route = RawVoiceRoute(sourceVoice);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 2, rendered);
    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo status{};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);

    const VnVoiceHandle render =
        session.Engine()->ledger.ToRender(sourceVoice);
    const harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    const std::uint64_t cursorBeforeFailure = context->srcCursor;

    // Three stereo Bus planes require 6144 bytes. Lowering the effective
    // capacity by one byte makes plane allocation fail before RenderAll.
    session.Engine()->mixer.ArenaForTests().SetCapacityForTests(
        3u * 2u * kBlock * sizeof(float) - 1u);
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(cursorBeforeFailure, context->srcCursor);

    session.Engine()->mixer.ArenaForTests().RestoreCapacityForTests();
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_SOURCE_SILENT);
    EXPECT_EQ(0u, status.stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);

    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    EXPECT_EQ(0u, status.stateFlags & VN_DYNAMIC_ROUTE_SOURCE_SILENT);
    EXPECT_EQ(0u, status.stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);
}

TEST(CompressorDynamicRoute,
     NaturalEndAndRenderSlotReuseLeaveTheOldPublicRouteStale) {
    for (std::uint32_t workers : {0u, 1u, 2u, 4u}) {
    SCOPED_TRACE(::testing::Message()
                 << "renderWorkerCount=" << workers);
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.renderWorkerCount = workers;
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kTargetBus = harkentest::ObjectId(120);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    const std::vector<float> first =
        harkentest::MakeDC(kBlock * 2, 1.0f);
    const std::vector<float> replacement =
        harkentest::MakeDC(kBlock * 3, 0.5f);
    VnPCMDesc firstPcm = harkentest::MonoDesc(first, kRate);
    VnPCMDesc replacementPcm =
        harkentest::MonoDesc(replacement, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(220), &firstPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(221), &replacementPcm));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(220));
    VnVoiceHandle ended = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &ended));
    const VnEffectDynamicRouteDesc route = RawVoiceRoute(ended);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 3, rendered);
    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo status{};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_STALE);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_INACTIVE_SOURCE_STALE,
              status.inactiveReason);

    play = harkentest::MakePlayDesc(harkentest::ObjectId(221));
    VnVoiceHandle reused = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &reused));
    EXPECT_NE(ended, reused);
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    status = {};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_STALE);
    EXPECT_EQ(ended, status.sourceVoiceHandle);
    }
}

TEST(CompressorDynamicRoute, DrainingVoiceTailRemainsALiveSource) {
    for (std::uint32_t workers : {0u, 1u, 2u, 4u}) {
    SCOPED_TRACE(::testing::Message()
                 << "renderWorkerCount=" << workers);
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.renderWorkerCount = workers;
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.delayEffectPoolCapacity = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kTargetBus = harkentest::ObjectId(121);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    const std::vector<float> source =
        harkentest::MakeDC(kBlock, 1.0f);
    VnPCMDesc sourcePcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(222), &sourcePcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(222));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof delay;
    delay.delayMs = 200.0f;
    delay.feedback = 0.0f;
    delay.wet = 1.0f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(
                  voice, 0, VN_EFFECT_FEEDBACK_DELAY,
                  reinterpret_cast<const VnEffectDesc*>(&delay)));
    const VnEffectDynamicRouteDesc route = RawVoiceRoute(voice);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 2, rendered);
    const VnVoiceHandle render =
        session.Engine()->ledger.ToRender(voice);
    const harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_EQ(harken::SourcePhase::Draining, context->phase);

    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo status{};
    status.structSize = sizeof status;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&status, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_NE(0u, status.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    }
}

TEST(CompressorDynamicRoute,
     AddingASharedTapRouteDoesNotRearmTheEstablishedRoute) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicVoiceTapCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kFirstTarget = harkentest::ObjectId(122);
    constexpr VnBusId kSecondTarget = harkentest::ObjectId(123);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kFirstTarget, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSecondTarget, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kFirstTarget, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kSecondTarget, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 8, 1.0f);
    VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(223), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(223));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnEffectDynamicRouteDesc route = RawVoiceRoute(voice);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(
                  kFirstTarget, 0, 0, &route));

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 2, rendered);
    std::uint32_t required = 0;
    VnEffectDynamicRouteInfo first{};
    first.structSize = sizeof first;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&first, 1, &required));
    ASSERT_EQ(1u, required);
    ASSERT_NE(0u, first.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);

    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(
                  kSecondTarget, 0, 0, &route));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    VnEffectDynamicRouteInfo rows[2]{};
    rows[0].structSize = sizeof rows[0];
    rows[1].structSize = sizeof rows[1];
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(rows, 2, &required));
    ASSERT_EQ(2u, required);
    ASSERT_EQ(kFirstTarget, rows[0].targetBusId);
    ASSERT_EQ(kSecondTarget, rows[1].targetBusId);
    EXPECT_NE(0u, rows[0].stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    EXPECT_EQ(0u, rows[0].stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);
    EXPECT_NE(0u, rows[1].stateFlags & VN_DYNAMIC_ROUTE_FIRST_BLOCK);
}

TEST(CompressorDynamicRoute,
     SilentSourcesRemainActiveAndDegradedTargetsReportStale) {
    const auto run = [](bool degradeTarget) {
        VnConfig config = harkentest::MakeNullConfig(2, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        if (degradeTarget) {
            config.arenaBytes = 64;
        }
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());

        constexpr VnBusId kSourceBus = harkentest::ObjectId(113);
        constexpr VnBusId kTargetBus = harkentest::ObjectId(114);
        ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
        const VnCompressorDesc compressor =
            CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 0.0f, 0.0f);
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                                  AsBase(&compressor)));
        const VnEffectDynamicRouteDesc route = RawBusRoute(kSourceBus);
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));

        const std::vector<float> silence =
            harkentest::MakeDC(kBlock * 2, 0.0f);
        const std::vector<float> target =
            harkentest::MakeDC(kBlock * 2, 0.25f);
        const VnPCMDesc silencePcm = harkentest::MonoDesc(silence, kRate);
        const VnPCMDesc targetPcm = harkentest::MonoDesc(target, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(213), &silencePcm));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(214), &targetPcm));
        VnPlayDesc sourcePlay = harkentest::MakePlayDesc(harkentest::ObjectId(213));
        sourcePlay.busId = kSourceBus;
        VnPlayDesc targetPlay = harkentest::MakePlayDesc(harkentest::ObjectId(214));
        targetPlay.busId = kTargetBus;
        VnVoiceHandle sourceVoice = 0;
        VnVoiceHandle targetVoice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));

        Capture rendered;
        harkentest::PumpAndCapture(session.Engine(), 1, rendered);
        std::uint32_t required = 0;
        VnEffectDynamicRouteInfo info{};
        info.structSize = sizeof info;
        ASSERT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(&info, 1, &required));
        ASSERT_EQ(1u, required);
        if (degradeTarget) {
            EXPECT_EQ(0u, info.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
            EXPECT_NE(0u, info.stateFlags & VN_DYNAMIC_ROUTE_STALE);
            EXPECT_EQ(VN_DYNAMIC_ROUTE_INACTIVE_TARGET_STALE,
                      info.inactiveReason);
        } else {
            EXPECT_NE(0u, info.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
            EXPECT_NE(0u,
                      info.stateFlags & VN_DYNAMIC_ROUTE_SOURCE_SILENT);
            EXPECT_EQ(VN_DYNAMIC_ROUTE_INACTIVE_NONE,
                      info.inactiveReason);
        }
    };

    run(false);
    run(true);
}

TEST(CompressorDynamicRoute,
     TargetClearAndTypeReplacementAtomicallyClearRoutes) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 2;
    config.limiterEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kSourceBus = harkentest::ObjectId(121);
    constexpr VnBusId kTargetBus = harkentest::ObjectId(122);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
    const VnLimiterDesc limiter = LimiterDesc(-1.0f, 100.0f);
    const VnEffectDynamicRouteDesc route = RawBusRoute(kSourceBus);
    std::uint32_t required = 0;

    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));
    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);

    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
    EXPECT_EQ(1u, required) << "same-type replacement preserves route identity";
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);

    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(kTargetBus, 0));
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
    EXPECT_EQ(0u, required)
        << "the logic view clears in the same accepted effect transaction";
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);

    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_LIMITER,
                              AsBase(&limiter)));
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
    EXPECT_EQ(0u, required)
        << "a non-Compressor replacement clears target routes atomically";
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    const std::uint32_t targetIndex =
        session.Engine()->buses.FindIndex(kTargetBus);
    const harken::BusTree::Bus* bus =
        session.Engine()->buses.BusState(targetIndex);
    ASSERT_NE(nullptr, bus);
    EXPECT_EQ(VN_EFFECT_LIMITER, bus->fxSlotEffectRef[0].type);
}

TEST(CompressorDynamicRoute,
     BusDestroyAtomicallyScrubsSourceAndTargetRoutes) {
    {
        VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        constexpr VnBusId kSourceBus = harkentest::ObjectId(131);
        constexpr VnBusId kTargetBus = harkentest::ObjectId(132);
        ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
        const VnCompressorDesc compressor =
            CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                                  AsBase(&compressor)));
        const VnEffectDynamicRouteDesc route = RawBusRoute(kSourceBus);
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));
        ASSERT_EQ(VN_OK, Vn_DestroyBus(kSourceBus));
        std::uint32_t required = 0;
        ASSERT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
        EXPECT_EQ(0u, required);
        Capture rendered;
        harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    }
    {
        VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        constexpr VnBusId kSourceBus = harkentest::ObjectId(133);
        constexpr VnBusId kTargetBus = harkentest::ObjectId(134);
        ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
        const VnCompressorDesc compressor =
            CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffect(kTargetBus, 0, VN_EFFECT_COMPRESSOR,
                                  AsBase(&compressor)));
        const VnEffectDynamicRouteDesc route = RawBusRoute(kSourceBus);
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(kTargetBus, 0, 0, &route));
        ASSERT_EQ(VN_OK, Vn_DestroyBus(kTargetBus));
        std::uint32_t required = 0;
        ASSERT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
        EXPECT_EQ(0u, required);
        Capture rendered;
        harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    }
}

TEST(CompressorDynamicRoute,
     CombinedSendAndDetectorGraphRejectsTheOperationThatClosesTheCycle) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr VnBusId kA = harkentest::ObjectId(141);
    constexpr VnBusId kB = harkentest::ObjectId(142);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kB, VN_BUS_MASTER));
    const VnCompressorDesc compressor =
        CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(kB, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kB, kA, 0.0f));
    const VnEffectDynamicRouteDesc route = RawBusRoute(kA);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_CYCLE,
              Vn_SetBusEffectDynamicRoute(kB, 0, 0, &route));

    ASSERT_EQ(VN_OK, Vn_DisconnectBusSend(kB, kA));
    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kB, 0, 0, &route));
    EXPECT_EQ(VN_SEND_CYCLE, Vn_ConnectBusSend(kB, kA, 0.0f));
}

TEST(CompressorDynamicRoute,
     RouteTapAndPlanPoolsRefuseWithoutMutatingAuthoredRows) {
    {
        VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        config.dynamicRouteCapacity = 1;
        config.dynamicBusTapCapacity = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(151), VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(152), VN_BUS_MASTER));
        const VnCompressorDesc compressor =
            CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                             harkentest::ObjectId(152), 0, VN_EFFECT_COMPRESSOR,
                             AsBase(&compressor)));
        const VnEffectDynamicRouteDesc route = RawBusRoute(harkentest::ObjectId(151));
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(152), 0, 0, &route));
        Capture rendered;
        harkentest::PumpAndCapture(session.Engine(), 1, rendered);
        EXPECT_EQ(VN_NO_DYNAMIC_ROUTE,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(152), 0, 1, &route));
        std::uint32_t required = 0;
        ASSERT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
        EXPECT_EQ(1u, required);
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof snapshot;
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_EQ(1u, snapshot.dynamicRouteCurrent);
        EXPECT_EQ(1u, snapshot.dynamicRouteCapacity);
        EXPECT_EQ(1u, snapshot.dynamicRouteHighWater);
        EXPECT_EQ(1u, snapshot.dynamicBusTapCurrent);
        EXPECT_EQ(2u, snapshot.dynamicBusTapCapacity);
        EXPECT_EQ(1u, snapshot.dynamicBusTapHighWater);
        EXPECT_EQ(0u, snapshot.dynamicVoiceTapCurrent);
        EXPECT_EQ(config.dynamicVoiceTapCapacity == 0
                      ? 16u
                      : config.dynamicVoiceTapCapacity,
                  snapshot.dynamicVoiceTapCapacity);
        EXPECT_EQ(1u, snapshot.dynamicRouteRefusalCount);
        EXPECT_GE(snapshot.dynamicPlanPublicationCount, 1u);
        EXPECT_EQ(0u, snapshot.dynamicPlanRetireOverflowCount);
    }
    {
        VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        config.dynamicRouteCapacity = 2;
        config.dynamicBusTapCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(153), VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(154), VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(155), VN_BUS_MASTER));
        const VnCompressorDesc compressor =
            CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                             harkentest::ObjectId(155), 0, VN_EFFECT_COMPRESSOR,
                             AsBase(&compressor)));
        const VnEffectDynamicRouteDesc first = RawBusRoute(harkentest::ObjectId(153));
        const VnEffectDynamicRouteDesc second = RawBusRoute(harkentest::ObjectId(154));
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(155), 0, 0, &first));
        Capture rendered;
        harkentest::PumpAndCapture(session.Engine(), 1, rendered);
        EXPECT_EQ(VN_NO_DYNAMIC_ROUTE,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(155), 0, 1, &second));
        std::uint32_t required = 0;
        ASSERT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
        EXPECT_EQ(1u, required);
    }
    {
        VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        config.dynamicRouteCapacity = 2;
        config.dynamicBusTapCapacity = 2;
        config.dynamicRoutePlanPoolCapacity = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(156), VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(157), VN_BUS_MASTER));
        const VnCompressorDesc compressor =
            CompressorDesc(-24.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f);
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                             harkentest::ObjectId(157), 0, VN_EFFECT_COMPRESSOR,
                             AsBase(&compressor)));
        const VnEffectDynamicRouteDesc route = RawBusRoute(harkentest::ObjectId(156));
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(157), 0, 0, &route));
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(157), 0, 1, &route))
            << "capacity two must stage a replacement while one plan is queued";
        EXPECT_EQ(VN_BACKPRESSURE,
                  Vn_ClearBusEffectDynamicRoute(
                      harkentest::ObjectId(157), 0, 0, VN_EFFECT_COMPRESSOR));
        std::uint32_t required = 0;
        ASSERT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(nullptr, 0, &required));
        EXPECT_EQ(2u, required);
    }
}

TEST(CompressorEffect, PublicBusResetUsesExistingOpcodeAndRetainsIdentity) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const VnCompressorDesc compressor =
        CompressorDesc(-40.0f, 20.0f, 0.1f, 100.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 4, 1.0f);
    VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(221), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(221));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);

    harken::BusTree::Bus* master = session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, master);
    auto* state =
        static_cast<harken::CompressorState*>(master->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    ASSERT_LT(state->currentGain, 1.0f);
    ASSERT_EQ(VN_OK,
              Vn_ResetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    EXPECT_GT(state->resetBridgeFramesRemaining, 0u);
    EXPECT_EQ(VN_EFFECT_COMPRESSOR, master->fxSlotEffectRef[0].type);
}

TEST(CompressorEffect,
     RuntimeTelemetryPublishesVoiceThenBusWithStablePublicIdentity) {
    VnConfig config = harkentest::MakeNullConfig(1, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const VnCompressorDesc compressor =
        CompressorDesc(-18.0f, 1.0f, 10.0f, 100.0f, 6.0f, 0.0f);
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)));
    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 4, 0.5f);
    VnPCMDesc pcm = harkentest::MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(231), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(231));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK,
              Vn_SetVoiceEffect(voice, 0, VN_EFFECT_COMPRESSOR,
                                AsBase(&compressor)));

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureCompressorRuntimeInfo(nullptr, 0, &required));
    ASSERT_EQ(2u, required);
    std::array<VnCompressorRuntimeInfo, 2> before{};
    before[0].structSize = sizeof before[0];
    ASSERT_EQ(VN_OK, Vn_CaptureCompressorRuntimeInfo(
                         before.data(), 2, &required));
    EXPECT_NE(0u, before[0].stateFlags & VN_COMPRESSOR_RUNTIME_STALE);
    EXPECT_NE(0u, before[1].stateFlags & VN_COMPRESSOR_RUNTIME_STALE);

    Capture rendered;
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    std::array<VnCompressorRuntimeInfo, 2> rows{};
    rows[0].structSize = sizeof rows[0];
    ASSERT_EQ(VN_OK,
              Vn_CaptureCompressorRuntimeInfo(rows.data(), 2, &required));
    ASSERT_EQ(2u, required);
    EXPECT_EQ(VN_EFFECT_HOST_SCOPE_VOICE, rows[0].hostScope);
    EXPECT_EQ(voice, rows[0].instanceHandle);
    EXPECT_EQ(VN_EFFECT_HOST_SCOPE_BUS, rows[1].hostScope);
    EXPECT_EQ(VN_BUS_MASTER, rows[1].objectId);
    EXPECT_EQ(0u, rows[0].stateFlags & VN_COMPRESSOR_RUNTIME_STALE);
    EXPECT_EQ(0u, rows[1].stateFlags & VN_COMPRESSOR_RUNTIME_STALE);
    EXPECT_NEAR(-6.0206f, rows[0].mainInputPeakDb, 1.0e-3f);
    EXPECT_NEAR(-6.0206f, rows[1].mainInputPeakDb, 1.0e-3f);
    EXPECT_EQ(rows[0].blockEpoch, rows[1].blockEpoch);

    ASSERT_EQ(VN_OK,
              Vn_ResetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_COMPRESSOR));
    harkentest::PumpAndCapture(session.Engine(), 1, rendered);
    rows = {};
    rows[0].structSize = sizeof rows[0];
    ASSERT_EQ(VN_OK,
              Vn_CaptureCompressorRuntimeInfo(rows.data(), 2, &required));
    EXPECT_NE(0u, rows[1].stateFlags &
                      VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK);
    EXPECT_NE(0u,
              rows[1].stateFlags & VN_COMPRESSOR_RUNTIME_RESET_BRIDGE);
}

TEST(EffectDynamicTransition,
     GainDelayAndLimiterUseRetargetablePreparedTenMillisecondSeams) {
    {
        harken::GainState state{};
        const harken::InsertEffect effect =
            harken::BuildEffect(VN_EFFECT_GAIN, &state);
        std::vector<float> rendered(1, 1.0f);
        float* channels[1] = {rendered.data()};
        effect.process(&state, channels, 1, 0, 1);
        harken::GainApplyPreparedParam(&state, 2.0f, 4);
        std::vector<float> first(2, 1.0f);
        channels[0] = first.data();
        effect.process(&state, channels, 1, 0, 2);
        EXPECT_FLOAT_EQ(1.25f, first[0]);
        EXPECT_FLOAT_EQ(1.5f, first[1]);
        harken::GainApplyPreparedParam(&state, 1.0f, 2);
        std::vector<float> retargeted(2, 1.0f);
        channels[0] = retargeted.data();
        effect.process(&state, channels, 1, 0, 2);
        EXPECT_FLOAT_EQ(1.25f, retargeted[0]);
        EXPECT_FLOAT_EQ(1.0f, retargeted[1]);
        EXPECT_EQ(0u, state.transitionFramesRemaining);
    }
    {
    float line[harken::kMaxChannels * 8] = {};
        harken::DelayEffectState state{};
        state.lineBase = line;
        state.lineFrames = 8;
        state.sampleRate = kRate;
        const harken::InsertEffect effect =
            harken::BuildEffect(VN_EFFECT_FEEDBACK_DELAY, &state);
        effect.reset(&state);
        std::vector<float> rendered(1, 1.0f);
        float* channels[1] = {rendered.data()};
        effect.process(&state, channels, 1, 0, 1);
        harken::DelayApplyPreparedParam(
            &state, harken::kDelayParamWet, 1.0f, 4);
        harken::DelayApplyPreparedParam(
            &state, harken::kDelayParamDry, 0.0f, 4);
        std::vector<float> transition(4, 1.0f);
        channels[0] = transition.data();
        effect.process(&state, channels, 1, 0, 2);
        EXPECT_FLOAT_EQ(0.5f, state.wet);
        EXPECT_FLOAT_EQ(0.5f, state.dry);
        EXPECT_EQ(2u, state.transitionFramesRemaining);
        effect.process(&state, channels, 1, 2, 2);
        EXPECT_FLOAT_EQ(1.0f, state.wet);
        EXPECT_FLOAT_EQ(0.0f, state.dry);
        EXPECT_EQ(0u, state.transitionFramesRemaining);
    }
    {
        harken::LimiterState state{};
        state.sampleRate = kRate;
        const harken::InsertEffect effect =
            harken::BuildEffect(VN_EFFECT_LIMITER, &state);
        harken::LimiterApplyPreparedParam(
            &state, harken::kLimiterParamCeilingDb, 1.0f);
        harken::LimiterApplyPreparedParam(
            &state, harken::kLimiterParamReleaseMs, 0.0f);
        std::vector<float> rendered(1, 0.0f);
        float* channels[1] = {rendered.data()};
        effect.process(&state, channels, 1, 0, 1);
        harken::LimiterRequestPreparedParam(
            &state, harken::kLimiterParamCeilingDb, 0.5f, 4);
        harken::LimiterRequestPreparedParam(
            &state, harken::kLimiterParamReleaseMs, 0.8f, 4);
        std::vector<float> transition(4, 0.0f);
        channels[0] = transition.data();
        effect.process(&state, channels, 1, 0, 2);
        EXPECT_FLOAT_EQ(0.75f, state.ceilingLinear);
        EXPECT_FLOAT_EQ(0.4f, state.releaseCoeff);
        EXPECT_EQ(2u, state.transitionFramesRemaining);
        effect.process(&state, channels, 1, 2, 2);
        EXPECT_FLOAT_EQ(0.5f, state.ceilingLinear);
        EXPECT_FLOAT_EQ(0.8f, state.releaseCoeff);
        EXPECT_EQ(0u, state.transitionFramesRemaining);
    }
}

} // namespace
