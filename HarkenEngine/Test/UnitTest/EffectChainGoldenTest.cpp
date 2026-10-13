// Golden cases for the insert-effect chain, driven through the public API so
// the full path is exercised (config -> chain length, command -> apply ->
// render). Synthetic non-commuting effects (add-a-constant, multiply — test
// code only, nothing synthetic ships) prove that slots process in ascending
// order, with closed-form expected outputs at tolerance zero. A separate
// case proves the per-slot bypass toggle is click-free: the wet/dry
// crossfade spreads the whole step across one block, so no adjacent-sample
// jump exceeds the documented per-sample bound.
//
// Tolerances: ordering outputs are exact
// arithmetic on exactly representable operands (tolerance 0); the bypass
// step bound is |wet - dry| / blockFrames, the largest step a one-block
// linear crossfade of a DC signal can take, plus a 1e-6 float-noise margin.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

// Non-commuting synthetic transforms; state points at the operand.
void AddConstantProcess(void* state, float* const* channels,
                        std::uint32_t channelCount, std::uint32_t offset,
                        std::uint32_t frames) {
    const float c = *static_cast<const float*>(state);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + offset;
        for (std::uint32_t i = 0; i < frames; ++i) {
            p[i] += c;
        }
    }
}

void MultiplyProcess(void* state, float* const* channels,
                     std::uint32_t channelCount, std::uint32_t offset,
                     std::uint32_t frames) {
    const float m = *static_cast<const float*>(state);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + offset;
        for (std::uint32_t i = 0; i < frames; ++i) {
            p[i] *= m;
        }
    }
}

VnConfig ChainConfig(std::uint32_t slots) {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = slots;
    return config;
}

TEST(EffectChainGolden, SlotsProcessInAscendingOrder) {
    // A DC source of 0.25 through two orderings of add-0.125 and multiply-2:
    //   slot0 add, slot1 mul -> (0.25 + 0.125) * 2   = 0.75
    //   slot0 mul, slot1 add ->  0.25 * 2   + 0.125  = 0.625
    // Ascending processing makes the two differ exactly as computed.
    float addC = 0.125f;
    float mulM = 2.0f;

    auto renderOrder = [&](bool addFirst) -> float {
        EngineSession session(ChainConfig(2));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> dc = harkentest::MakeDC(600, 0.25f);
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harken::VoiceRenderContext* voice =
            session.Engine()->voices.Resolve(harkentest::RenderHandle(
                session.Engine(), handle));
        EXPECT_NE(nullptr, voice);
        voice->fxSlots[0].process = addFirst ? AddConstantProcess
                                             : MultiplyProcess;
        voice->fxSlots[0].state = addFirst ? static_cast<void*>(&addC)
                                           : static_cast<void*>(&mulM);
        voice->fxSlots[1].process = addFirst ? MultiplyProcess
                                             : AddConstantProcess;
        voice->fxSlots[1].state = addFirst ? static_cast<void*>(&mulM)
                                           : static_cast<void*>(&addC);
        Capture out;
        harkentest::PumpAndCapture(session.Engine(), 1, out);
        return out.left[0];
    };

    const float addThenMul = renderOrder(/*addFirst=*/true);
    const float mulThenAdd = renderOrder(/*addFirst=*/false);
    EXPECT_EQ(0.75f, addThenMul);
    EXPECT_EQ(0.625f, mulThenAdd);
    EXPECT_NE(addThenMul, mulThenAdd)
        << "non-commuting effects must expose slot order";
}

TEST(EffectChainGolden, PerSlotBypassTransitionIsClickFree) {
    // A multiply-by-4 effect on a DC 0.2 source: active output is 0.8, dry
    // (bypassed) output is 0.2. Toggling bypass spreads the 0.6 step across
    // one block, so every adjacent-sample step stays within the crossfade's
    // per-sample bound. An abrupt (buggy) toggle would jump 0.6 at the
    // boundary and fail.
    EngineSession session(ChainConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kDc = 0.2f;
    float factor = 4.0f;
    const float wet = kDc * factor; // 0.8
    const float dry = kDc;          // 0.2

    const std::vector<float> dc = harkentest::MakeDC(2000, kDc);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::VoiceRenderContext* voice =
        session.Engine()->voices.Resolve(harkentest::RenderHandle(session.Engine(
            ), handle));
    ASSERT_NE(nullptr, voice);
    voice->fxSlots[0].process = MultiplyProcess;
    voice->fxSlots[0].state = &factor;

    Capture out;
    // Block 1: fully active.
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    // Arm the bypass; block 2 is the one-block crossfade, block 3+ steady dry.
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));
    harkentest::PumpAndCapture(session.Engine(), 3, out);

    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // Endpoints: wet before the toggle, dry after the crossfade block.
    ASSERT_NEAR(wet, out.left[0], 1e-6f);
    ASSERT_NEAR(dry, out.left[out.left.size() - 1], 1e-6f);

    const float kMaxStep = std::fabs(wet - dry) / static_cast<float>(kBlock) +
                           1e-6f;
    for (std::size_t i = 1; i < out.left.size(); ++i) {
        const float step = std::fabs(out.left[i] - out.left[i - 1]);
        ASSERT_LE(step, kMaxStep) << "click at sample " << i;
    }
}

} // namespace
