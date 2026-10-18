// Bus insert-effect chain + fader point (spec §7.3/§7.4/§7.5): bus chains reuse
// the voice chain mechanics, so these tests reuse the voice-chain golden
// technique — synthetic non-commuting effects (add-a-constant, multiply, and a
// read-only input capture; test code only, nothing synthetic ships) injected
// directly into a bus's render state through the internal BusState accessor.
// That yields closed-form expected outputs at tight tolerance for chain order,
// the pre/post-fader split, and post-fader observation, without depending on any
// real effect's internal DSP.
//
// Signals are analytic DC so every expectation is closed form. A DC mono voice
// at the native rate and unity produces exactly its source value from block 0
// (the baseline bus-routing test proves this), so exact-equality assertions are
// used where the only arithmetic is on exactly representable operands; the
// gain-staging cases compute the expected gain with the same pow() the engine
// uses and compare at a tight tolerance.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "PlayableStorageTestSupport.h"

#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

class BusEffect : public testing::Test {
    harkentest::PlayableCapacityScope playableCapacity_{8, 1};
};

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr VnBusId kBusA = harkentest::ObjectId(10);

// Non-commuting synthetic transforms; state points at the operand. Copied from
// the voice-chain golden so the bus path is proven with the identical technique.
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

// Read-only: records the first sample it is handed (its input at this stage)
// into *state, leaving the signal untouched. Used to observe what signal a
// post-fader slot actually sees.
void CaptureInputProcess(void* state, float* const* channels,
                         std::uint32_t channelCount, std::uint32_t offset,
                         std::uint32_t frames) {
    if (channelCount > 0 && frames > 0) {
        *static_cast<float*>(state) = channels[0][offset];
    }
}

VnConfig BusFxConfig(std::uint32_t slots) {
    VnConfig config = MakeNullConfig(0, kBlock, kRate);
    config.maxTriggerDepth = 1;
    config.maxActionFirings = 8;
    config.fxSlotsPerBus = slots;
    return config;
}

void ExpectCoeffsEqual(const harken::BiquadCoeffs& expected,
                       const harken::BiquadCoeffs& actual) {
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b0),
              std::bit_cast<std::uint32_t>(actual.b0));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b1),
              std::bit_cast<std::uint32_t>(actual.b1));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b2),
              std::bit_cast<std::uint32_t>(actual.b2));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.a1),
              std::bit_cast<std::uint32_t>(actual.a1));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.a2),
              std::bit_cast<std::uint32_t>(actual.a2));
}

// Registers a DC mono source under id 1.
std::vector<float> RegisterDc(float value, std::size_t frames = 2000) {
    std::vector<float> dc = MakeDC(frames, value);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    return dc;
}

// A bus chain processes its slots in ascending order: two orderings of a
// non-commuting add/multiply pair on the SAME bus differ exactly as computed
// (the default all-pre-fader split with unity BusVolume leaves the fold at
// unity, so only the chain arithmetic shows).
TEST_F(BusEffect, SlotsProcessInAscendingOrder) {
    float addC = 0.125f;
    float mulM = 2.0f;

    auto renderOrder = [&](bool addFirst) -> float {
        EngineSession session(BusFxConfig(2));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> dc = RegisterDc(0.25f);
        EXPECT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = kBusA;
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        // Inject the synthetic pair straight into the bus's chain (the bus is
        // reachable by dense index the moment it is created; it renders once the
        // create swap applies on the pump below).
        const std::uint32_t idx = session.Engine()->buses.FindIndex(kBusA);
        harken::BusTree::Bus* bus = session.Engine()->buses.BusState(idx);
        EXPECT_NE(nullptr, bus);
        bus->fxSlots[0].process = addFirst ? AddConstantProcess : MultiplyProcess;
        bus->fxSlots[0].state =
            addFirst ? static_cast<void*>(&addC) : static_cast<void*>(&mulM);
        bus->fxSlots[1].process = addFirst ? MultiplyProcess : AddConstantProcess;
        bus->fxSlots[1].state =
            addFirst ? static_cast<void*>(&mulM) : static_cast<void*>(&addC);
        Capture out;
        PumpAndCapture(session.Engine(), 1, out);
        return out.left[0];
    };

    // slot0 add, slot1 mul -> (0.25 + 0.125) * 2 = 0.75
    // slot0 mul, slot1 add ->  0.25 * 2  + 0.125 = 0.625
    const float addThenMul = renderOrder(/*addFirst=*/true);
    const float mulThenAdd = renderOrder(/*addFirst=*/false);
    EXPECT_FLOAT_EQ(0.75f, addThenMul);
    EXPECT_FLOAT_EQ(0.625f, mulThenAdd);
    EXPECT_NE(addThenMul, mulThenAdd)
        << "non-commuting bus effects must expose slot order";
}

// A bus slot's bypass toggle is a click-free one-block wet/dry crossfade, and
// settles to the dry (bypassed) signal — the identical discipline the voice
// chain uses.
TEST_F(BusEffect, PerSlotBypassIsClickFree) {
    EngineSession session(BusFxConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kDc = 0.2f;
    float factor = 4.0f;
    const float wet = kDc * factor; // 0.8
    const float dry = kDc;          // 0.2

    const std::vector<float> dc = RegisterDc(kDc);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const std::uint32_t idx = session.Engine()->buses.FindIndex(kBusA);
    harken::BusTree::Bus* bus = session.Engine()->buses.BusState(idx);
    ASSERT_NE(nullptr, bus);
    bus->fxSlots[0].process = MultiplyProcess;
    bus->fxSlots[0].state = &factor;

    Capture out;
    // Block 1: fully active (wet).
    PumpAndCapture(session.Engine(), 1, out);
    // Arm the bypass; block 2 is the one-block crossfade, block 3+ steady dry.
    ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(kBusA, 0, 1));
    PumpAndCapture(session.Engine(), 3, out);

    HARKENTEST_ASSERT_NON_SILENT(out.left);
    ASSERT_NEAR(wet, out.left[0], 1e-6f);
    ASSERT_NEAR(dry, out.left[out.left.size() - 1], 1e-6f);
    const float kMaxStep =
        std::fabs(wet - dry) / static_cast<float>(kBlock) + 1e-6f;
    for (std::size_t i = 1; i < out.left.size(); ++i) {
        const float step = std::fabs(out.left[i] - out.left[i - 1]);
        ASSERT_LE(step, kMaxStep) << "click at sample " << i;
    }
}

// The fader point splits the chain around BusVolume: the same effect before vs
// after the gain differs exactly as computed. An affine effect (add a constant)
// does not commute with a scalar gain, so pre-fader (x + c) * g differs from
// post-fader x * g + c.
TEST_F(BusEffect, PreFaderVsPostFaderSplitDiffersAsComputed) {
    constexpr float kV = 0.5f;
    constexpr float kC = 0.25f;
    const float gainDb = -6.0206f; // ~0.5 linear
    const float g = std::pow(10.0f, gainDb / 20.0f);
    float addC = kC;

    auto render = [&](std::uint32_t faderAfterSlot) -> float {
        EngineSession session(BusFxConfig(1));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> dc = RegisterDc(kV);
        EXPECT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        EXPECT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, faderAfterSlot));
        EXPECT_EQ(VN_OK, Vn_SetBusVolume(kBusA, gainDb, 0)); // snap
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = kBusA;
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        const std::uint32_t idx = session.Engine()->buses.FindIndex(kBusA);
        harken::BusTree::Bus* bus = session.Engine()->buses.BusState(idx);
        EXPECT_NE(nullptr, bus);
        bus->fxSlots[0].process = AddConstantProcess;
        bus->fxSlots[0].state = &addC;
        Capture out;
        PumpAndCapture(session.Engine(), 1, out);
        return out.left[0];
    };

    // faderAfterSlot 1: the add is pre-fader -> (v + c) * g.
    // faderAfterSlot 0: the add is post-fader -> v * g + c.
    const float pre = render(/*faderAfterSlot=*/1);
    const float post = render(/*faderAfterSlot=*/0);
    ASSERT_NEAR((kV + kC) * g, pre, 1e-5f);
    ASSERT_NEAR(kV * g + kC, post, 1e-5f);
    ASSERT_GT(std::fabs(pre - post), 0.01f)
        << "the fader split must change the gain staging";
}

TEST_F(BusEffect, InternalMeterTapsFollowBusSignalBoundaries) {
    constexpr float kInput = 0.25f;
    constexpr float kPreAdd = 0.125f;
    constexpr float kPostAdd = 0.0625f;
    const float gainDb = -6.0206f;
    const float gain = std::pow(10.0f, gainDb / 20.0f);
    float preAdd = kPreAdd;
    float postAdd = kPostAdd;

    EngineSession session(BusFxConfig(2));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(kInput);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 1));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, gainDb, 0));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harken::Engine* const engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(kBusA);
    harken::BusTree::Bus* const bus = engine->buses.BusState(busIndex);
    ASSERT_NE(nullptr, bus);
    bus->fxSlots[0].process = AddConstantProcess;
    bus->fxSlots[0].state = &preAdd;
    bus->fxSlots[1].process = AddConstantProcess;
    bus->fxSlots[1].state = &postAdd;

    engine->meterRing.SetArmedCount(1);
    Capture out;
    PumpAndCapture(engine, 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    const float expectedPre = kInput + kPreAdd;
    const float expectedPost = expectedPre * gain;
    const float expectedEffects = expectedPost + kPostAdd;
    EXPECT_NEAR(bus->preFaderPeak, expectedPre, 1e-5f);
    EXPECT_NEAR(bus->postFaderPeak, expectedPost, 1e-5f);
    EXPECT_NEAR(bus->outputPeak, expectedEffects, 1e-5f);

    harken::SnapshotPayload payload{};
    ASSERT_TRUE(engine->mixer.Snapshots().Capture(payload));
    EXPECT_NEAR(payload.busPeaksPreFader[busIndex], expectedPre, 1e-5f);
    EXPECT_NEAR(payload.busPeaksPostFader[busIndex], expectedPost, 1e-5f);
    EXPECT_NEAR(payload.busPeaks[busIndex], expectedEffects, 1e-5f);

    std::uint64_t blockIndex = 0;
    const float* prePeaks = nullptr;
    const float* postPeaks = nullptr;
    const float* effectsPeaks = nullptr;
    ASSERT_TRUE(engine->meterRing.FrontTaps(
        &blockIndex, &prePeaks, &postPeaks, &effectsPeaks));
    EXPECT_EQ(blockIndex, 0u);
    EXPECT_NEAR(prePeaks[busIndex], expectedPre, 1e-5f);
    EXPECT_NEAR(postPeaks[busIndex], expectedPost, 1e-5f);
    EXPECT_NEAR(effectsPeaks[busIndex], expectedEffects, 1e-5f);

    const float* legacyPeaks = nullptr;
    ASSERT_TRUE(engine->meterRing.Front(&blockIndex, &legacyPeaks));
    EXPECT_NEAR(legacyPeaks[busIndex], expectedEffects, 1e-5f);
}

TEST_F(BusEffect, MeterProbesSelectAllThreeSignalTaps) {
    constexpr float kInput = 0.25f;
    constexpr float kPreAdd = 0.125f;
    constexpr float kPostAdd = 0.0625f;
    const float gainDb = -6.0206f;
    const float gain = std::pow(10.0f, gainDb / 20.0f);
    float preAdd = kPreAdd;
    float postAdd = kPostAdd;

    EngineSession session(BusFxConfig(2));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(kInput);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 1));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, gainDb, 0));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harken::Engine* engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(kBusA);
    harken::BusTree::Bus* const bus = engine->buses.BusState(busIndex);
    ASSERT_NE(nullptr, bus);
    bus->fxSlots[0].process = AddConstantProcess;
    bus->fxSlots[0].state = &preAdd;
    bus->fxSlots[1].process = AddConstantProcess;
    bus->fxSlots[1].state = &postAdd;

    // Three probes, one per tap, on the same source bus. Zero ballistics
    // select the instant coefficient, so one drained block lands each
    // envelope exactly on that tap's peak — closed-form dBFS per stage.
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8101), kBusA,
                                              VN_BUS_METER_TAP_PRE_FADER,
                                              0, 0));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8102), kBusA,
                                              VN_BUS_METER_TAP_POST_FADER,
                                              0, 0));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8103), kBusA,
                                              VN_BUS_METER_TAP_POST_EFFECTS,
                                              0, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(engine->meterDetectors.ValueDb(harkentest::ObjectId(8101)),
                    20.0f * std::log10(kInput + kPreAdd));
    EXPECT_FLOAT_EQ(engine->meterDetectors.ValueDb(harkentest::ObjectId(8102)),
                    20.0f * std::log10((kInput + kPreAdd) * gain));
    EXPECT_FLOAT_EQ(engine->meterDetectors.ValueDb(harkentest::ObjectId(8103)),
                    20.0f * std::log10((kInput + kPreAdd) * gain + kPostAdd));
}

// A post-fader effect on the master bus observes the post-fader (gained) signal:
// with a known BusVolume change, a capture in a post-fader slot reads v*g, while
// the same capture pre-fader reads v.
TEST_F(BusEffect, MasterPostFaderObservesPostFaderSignal) {
    constexpr float kV = 0.5f;
    const float gainDb = -6.0206f; // ~0.5 linear
    const float g = std::pow(10.0f, gainDb / 20.0f);

    auto capturedAt = [&](std::uint32_t faderAfterSlot) -> float {
        EngineSession session(BusFxConfig(1));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> dc = RegisterDc(kV);
        EXPECT_EQ(VN_OK, Vn_SetBusFaderPoint(VN_BUS_MASTER, faderAfterSlot));
        EXPECT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, gainDb, 0)); // snap
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1)); // master bus by default
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        float seen = -1.0f;
        harken::BusTree::Bus* master = session.Engine()->buses.BusState(0);
        EXPECT_NE(nullptr, master);
        master->fxSlots[0].process = CaptureInputProcess;
        master->fxSlots[0].state = &seen;
        Capture out;
        PumpAndCapture(session.Engine(), 1, out);
        return seen;
    };

    const float postFaderSaw = capturedAt(/*faderAfterSlot=*/0);
    const float preFaderSaw = capturedAt(/*faderAfterSlot=*/1);
    // Post-fader observes the gained signal; pre-fader observes the raw input.
    ASSERT_NEAR(kV * g, postFaderSaw, 1e-5f);
    ASSERT_NEAR(kV, preFaderSaw, 1e-5f);
    ASSERT_LT(postFaderSaw, preFaderSaw);
}

// Moving the fader point applies at a block boundary as a plain swap of the
// split index. Click discipline: with gain-commuting staging (a linear effect,
// here a scalar multiply) the swap is exactly output-invariant, so no
// discontinuity appears at the boundary — this pins the block-boundary-swap
// behavior for the click-free (commuting) regime. (A nonlinear effect straddling
// the move under a non-unity gain can step at the boundary; a true output
// crossfade would require processing the stateful chain twice per block — see
// the report's self-flag.)
TEST_F(BusEffect, FaderMoveAppliesAtBlockBoundaryWithoutClick) {
    EngineSession session(BusFxConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kV = 0.5f;
    float factor = 2.0f;
    const float gainDb = -6.0206f;
    const float g = std::pow(10.0f, gainDb / 20.0f);
    // Multiply commutes with the scalar gain, so both splits yield v*factor*g.
    const float expected = kV * factor * g;

    const std::vector<float> dc = RegisterDc(kV);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, gainDb, 0));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const std::uint32_t idx = session.Engine()->buses.FindIndex(kBusA);
    harken::BusTree::Bus* bus = session.Engine()->buses.BusState(idx);
    ASSERT_NE(nullptr, bus);
    bus->fxSlots[0].process = MultiplyProcess;
    bus->fxSlots[0].state = &factor;

    Capture out;
    // Two blocks all-pre-fader (the default split), then move the effect
    // post-fader and render two more.
    PumpAndCapture(session.Engine(), 2, out);
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 0));
    PumpAndCapture(session.Engine(), 2, out);

    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // The commuting move leaves every sample at the same steady value, so the
    // boundary is continuous (no click) and the value is preserved.
    for (std::size_t i = 0; i < out.left.size(); ++i) {
        ASSERT_NEAR(expected, out.left[i], 1e-5f) << "sample " << i;
    }
    for (std::size_t i = 1; i < out.left.size(); ++i) {
        ASSERT_LT(std::fabs(out.left[i] - out.left[i - 1]), 1e-5f)
            << "click at sample " << i;
    }
}

// A slot bypassed while empty stays bypassed when an effect is later installed
// into it (the per-slot bypass persists independent of the install) — the same
// empty-slot bypass discipline the voice slots use, applied to a bus slot.
TEST_F(BusEffect, EmptySlotBypassPersistsAcrossInstall) {
    EngineSession session(BusFxConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kV = 0.5f;
    float factor = 2.0f;
    const std::vector<float> dc = RegisterDc(kV);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Bypass the still-empty slot 0, then pump so the empty-slot bypass settles
    // to fully dry (mix 1) at apply.
    ASSERT_EQ(VN_OK, Vn_SetBusEffectBypass(kBusA, 0, 1));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);

    // Now install a multiply effect into that slot (via the render state, so the
    // expected value is closed form). Because the slot is bypassed, the effect
    // must not be heard: the output stays the dry input.
    const std::uint32_t idx = session.Engine()->buses.FindIndex(kBusA);
    harken::BusTree::Bus* bus = session.Engine()->buses.BusState(idx);
    ASSERT_NE(nullptr, bus);
    bus->fxSlots[0].process = MultiplyProcess;
    bus->fxSlots[0].state = &factor;

    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // Dry (bypass persisted): kV, not kV*factor.
    ASSERT_NEAR(kV, out.left[out.left.size() - 1], 1e-5f);
}

// Default invariance: a bus with no effects and no fader-point call folds
// exactly like the prior gain-only path — a DC voice through a created bus
// reaches the master unchanged. (The whole prior suite passing on the
// fx-capable bus path is the broader evidence; this pins one explicit value.)
TEST_F(BusEffect, NoEffectsNoFaderIsBitIdenticalPassthrough) {
    EngineSession session(BusFxConfig(0)); // 0 -> default fxSlotsPerBus (8)
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < kBlock; ++i) {
        ASSERT_FLOAT_EQ(0.5f, out.left[i]) << "sample " << i;
        ASSERT_FLOAT_EQ(0.5f, out.right[i]) << "sample " << i;
    }
}

// The full pooled effect lifecycle on a bus through the PUBLIC API only (the
// goldens above inject synthetic effects, so this pins the real path):
// descriptor validation + acquire/configure at Vn_SetBusEffect, an audible
// Vn_SetBusEffectParam change, an in-place replace with a different type
// (retiring the old instance — verified via pool occupancy so nothing leaks),
// and Vn_ClearBusEffect resuming passthrough immediately (tail cut, no drain).
TEST_F(BusEffect, PublicPooledEffectLifecycle) {
    EngineSession session(BusFxConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float kV = 0.5f;
    const std::vector<float> dc = RegisterDc(kV);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));

    // Install a feedback delay: no feedback, dry + a one-period wet copy, so a
    // steady DC settles to dry + wet = 2*v (an audible, closed-form doubling).
    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(VnFeedbackDelayDesc);
    delay.delayMs = 1.0f; // 48 frames at 48 kHz, well inside the block
    delay.feedback = 0.0f;
    delay.wet = 1.0f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusA, 0, VN_EFFECT_FEEDBACK_DELAY,
                                     reinterpret_cast<const VnEffectDesc*>(
                                         &delay)));
    EXPECT_EQ(1u, session.Engine()->effectBank.DelayInUse());

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    play.loopCount = 0; // infinite, so the DC keeps flowing across all phases
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Steady state: dry (v) + wet (v, delayed) = 1.0.
    Capture full;
    PumpAndCapture(session.Engine(), 4, full);
    HARKENTEST_ASSERT_NON_SILENT(full.left);
    ASSERT_NEAR(2.0f * kV, full.left[full.left.size() - 1], 1e-4f);

    // Param change (wet 1.0 -> 0.5): steady state becomes v + 0.5*v = 0.75.
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(kBusA, 0, /*wet=*/2u, 0.5f));
    Capture afterParam;
    PumpAndCapture(session.Engine(), 3, afterParam);
    ASSERT_NEAR(kV + 0.5f * kV, afterParam.left[afterParam.left.size() - 1],
                1e-4f);

    // In-place replace with a different type: a -6 dB low-shelf tone, which
    // attenuates DC (below the shelf corner). The old delay instance is retired
    // on the audio side and reclaimed by Vn_Update, so pool occupancy flips to
    // the tone with zero delay instances left (no leak).
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = -6.0f;
    tone.lowShelfFreqHz = 1000.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 6000.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusA, 0, VN_EFFECT_THREE_BAND_TONE,
                                     reinterpret_cast<const VnEffectDesc*>(
                                         &tone)));
    EXPECT_EQ(1u, session.Engine()->effectBank.ToneInUse());
    Capture afterReplace;
    PumpAndCapture(session.Engine(), 1, afterReplace);
    Vn_Update(); // reclaims the retired delay instance
    EXPECT_EQ(0u, session.Engine()->effectBank.DelayInUse());
    PumpAndCapture(session.Engine(), 4, afterReplace);
    // The doubling delay is gone and the low-shelf attenuates DC: clearly below
    // the raw input, and nowhere near the delay's >= 0.75.
    ASSERT_LT(afterReplace.left[afterReplace.left.size() - 1], 0.4f);

    // Clear: passthrough resumes immediately (tail cut, no drain) and the tone
    // instance is retired and reclaimed.
    ASSERT_EQ(VN_OK, Vn_ClearBusEffect(kBusA, 0));
    Capture afterClear;
    PumpAndCapture(session.Engine(), 1, afterClear);
    Vn_Update();
    EXPECT_EQ(0u, session.Engine()->effectBank.ToneInUse());
    PumpAndCapture(session.Engine(), 2, afterClear);
    ASSERT_NEAR(kV, afterClear.left[afterClear.left.size() - 1], 1e-4f);
}

std::uint32_t PrepareNeutralTestPlan(harken::Engine& engine,
                                     std::uint32_t selected,
                                     std::uint8_t mask) {
    auto& plans = engine.topologyPlans;
    plans.DrainRetire();
    const auto plan = plans.Acquire();
    if (plan == harken::kInvalidTopologyPlanIndex) return plan;
    const auto count = engine.buses.BusCount();
    plans.SetSlotCount(plan, count);
    plans.SetOrderCount(plan, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& bus = *engine.buses.BusState(i);
        plans.OrderData(plan)[i] = count - i - 1;
        plans.ParentsData(plan)[i] = bus.parentIndex;
        plans.AliveData(plan)[i] = bus.id != harkentest::ObjectId(0);
        plans.BusIdsData(plan)[i] = bus.id;
        plans.GenerationsData(plan)[i] = bus.generation;
        plans.OutputLayoutData(plan)[i] = bus.outputLayout;
        std::copy_n(bus.downmixCoeffs, 64,
                    plans.DownmixCoeffsData(plan) + i * 64);
    }
    plans.BusVolumeApplyData(plan)[selected] = mask;
    return plan;
}

void ApplyNeutralTestPlan(harken::Engine& engine, std::uint32_t plan) {
    const auto hits = harken::memguard::HitCount();
    {
        harken::memguard::AudioThreadScope scope;
        engine.buses.ApplyTopologyPlan(plan, harken::kInvalidBusIndex,
                                       harken::kInvalidBusIndex, 0.0f);
    }
    EXPECT_EQ(hits, harken::memguard::HitCount());
}

void InstallNeutralTestGain(VnBusId bus, std::uint32_t slot = 0) {
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = -6.0206f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
        bus, slot, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
}

TEST_F(BusEffect, NeutralPlanClearsSelectedPooledStateAndPreservesForeignBus) {
    for (const VnBusId selectedId : {kBusA, VN_BUS_MASTER}) {
        EngineSession session(BusFxConfig(2));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const auto dc = RegisterDc(0.25f);
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(20), VN_BUS_MASTER));
        InstallNeutralTestGain(selectedId);
        InstallNeutralTestGain(harkentest::ObjectId(20));
        ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(selectedId, 0));
        ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(harkentest::ObjectId(20), 1));
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = kBusA;
        play.loopCount = 0;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        Capture before;
        PumpAndCapture(&engine, 2, before);
        ASSERT_NEAR(0.125f, before.left.back(), 1e-5f);
        const auto selected = engine.buses.FindIndex(selectedId);
        const auto other = engine.buses.FindIndex(harkentest::ObjectId(20));
        auto& bus = *engine.buses.BusState(selected);
        auto& foreign = *engine.buses.BusState(other);
        const auto foreignRef = foreign.fxSlotEffectRef[0];
        const auto foreignProcess = foreign.fxSlots[0].process;
        engine.buses.ApplyVolume(selected, -6.0206f, 0, bus.generation);
        engine.buses.ApplyVolume(selected, -6.0206f, 0, bus.generation, true);
        engine.buses.ApplyVolume(other, -9.0f, 0, foreign.generation);
        const auto foreignGain = foreign.gain.current;
        bus.fxBypassMix[0] = 0.5f;
        bus.fxBypassTarget[0] = 1.0f;
        bus.fxBypassRamping[0] = true;
        bus.fxBypassMix[1] = 1.0f;
        bus.fxBypassTarget[1] = 1.0f;
        const auto plan = PrepareNeutralTestPlan(
            engine, selected, harken::kApplyBusNeutralProperties |
                                  harken::kApplyBusVolume |
                                  harken::kApplyBusOutputVolume);
        ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
        ApplyNeutralTestPlan(engine, plan);
        EXPECT_EQ(2u, bus.faderAfterSlot);
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
            EXPECT_EQ(harken::kEffectTypeNone, bus.fxSlotEffectRef[slot].type);
            EXPECT_EQ(nullptr, bus.fxSlots[slot].process);
            EXPECT_FLOAT_EQ(0.0f, bus.fxBypassMix[slot]);
            EXPECT_FLOAT_EQ(0.0f, bus.fxBypassTarget[slot]);
            EXPECT_FALSE(bus.fxBypassRamping[slot]);
        }
        EXPECT_FLOAT_EQ(1.0f, bus.gain.current);
        EXPECT_FLOAT_EQ(1.0f, bus.outputGain.current);
        EXPECT_EQ(foreignRef.poolIndex, foreign.fxSlotEffectRef[0].poolIndex);
        EXPECT_EQ(foreignRef.generation, foreign.fxSlotEffectRef[0].generation);
        EXPECT_EQ(foreignProcess, foreign.fxSlots[0].process);
        EXPECT_EQ(1u, foreign.faderAfterSlot);
        EXPECT_FLOAT_EQ(foreignGain, foreign.gain.current);
        EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
        Capture after;
        PumpAndCapture(&engine, 1, after);
        HARKENTEST_ASSERT_NON_SILENT(after.left);
        for (float sample : after.left) EXPECT_NEAR(0.25f, sample, 1e-6f);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    }
}

TEST_F(BusEffect, NeutralPlanIdentityAndGainMaskDifferential) {
    for (int mode = 0; mode < 5; ++mode) {
        SCOPED_TRACE(mode);
        EngineSession session(BusFxConfig(1));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        InstallNeutralTestGain(kBusA);
        Capture warm;
        PumpAndCapture(&engine, 1, warm);
        const auto index = engine.buses.FindIndex(kBusA);
        auto& bus = *engine.buses.BusState(index);
        engine.buses.ApplyVolume(index, -6.0206f, 0, bus.generation);
        engine.buses.ApplyVolume(index, -9.0f, 0, bus.generation, true);
        bus.faderAfterSlot = 0;
        const auto gain = bus.gain.current;
        const auto output = bus.outputGain.current;
        const auto ref = bus.fxSlotEffectRef[0];
        const auto mask = mode == 0 ? harken::kApplyBusVolume :
            mode == 3 ? harken::kApplyBusNeutralProperties :
            mode == 4 ? 0 : harken::kApplyBusNeutralProperties |
                               harken::kApplyBusVolume |
                               harken::kApplyBusOutputVolume;
        const auto plan = PrepareNeutralTestPlan(engine, index, mask);
        ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
        if (mode < 2) engine.topologyPlans.GenerationsData(plan)[index] = 0;
        if (mode == 2) {
            engine.topologyPlans.SetSlotCount(plan, 0);
            engine.topologyPlans.GenerationsData(plan)[index] = bus.generation + 1;
        }
        ApplyNeutralTestPlan(engine, plan);
        EXPECT_FLOAT_EQ(mode == 0 ? 1.0f : gain, bus.gain.current);
        EXPECT_FLOAT_EQ(output, bus.outputGain.current);
        EXPECT_EQ(mode == 3 ? 1u : 0u, bus.faderAfterSlot);
        if (mode == 3) {
            EXPECT_EQ(harken::kEffectTypeNone, bus.fxSlotEffectRef[0].type);
        } else {
            EXPECT_EQ(ref.type, bus.fxSlotEffectRef[0].type);
            EXPECT_EQ(ref.poolIndex, bus.fxSlotEffectRef[0].poolIndex);
            EXPECT_EQ(ref.generation, bus.fxSlotEffectRef[0].generation);
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(mode == 3 ? 0u : 1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    }
}

TEST_F(BusEffect, NeutralPlanDeadRowMatchesOrdinaryTombstone) {
    for (const bool neutral : {false, true}) {
        EngineSession session(BusFxConfig(1));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        InstallNeutralTestGain(kBusA);
        Capture warm;
        PumpAndCapture(&engine, 1, warm);
        const auto index = engine.buses.FindIndex(kBusA);
        auto& bus = *engine.buses.BusState(index);
        engine.buses.ApplyVolume(index, -6.0206f, 0, bus.generation);
        const auto gain = bus.gain.current;
        const auto plan = PrepareNeutralTestPlan(
            engine, index, neutral ? harken::kApplyBusNeutralProperties |
                                      harken::kApplyBusVolume : 0);
        ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
        engine.topologyPlans.AliveData(plan)[index] = 0;
        engine.topologyPlans.OrderData(plan)[0] = 0;
        engine.topologyPlans.SetOrderCount(plan, 1);
        ApplyNeutralTestPlan(engine, plan);
        EXPECT_EQ(harkentest::ObjectId(0u), bus.id);
        EXPECT_EQ(harken::kEffectTypeNone, bus.fxSlotEffectRef[0].type);
        EXPECT_FLOAT_EQ(gain, bus.gain.current);
        EXPECT_FLOAT_EQ(0.0f, bus.outputPeak);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    }
}

TEST_F(BusEffect, NeutralPlanGenerationTransitionAndRepeatedSwapsRetireOnce) {
    VnConfig config = BusFxConfig(1);
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    InstallNeutralTestGain(kBusA);
    Capture warm;
    PumpAndCapture(&engine, 1, warm);
    const auto index = engine.buses.FindIndex(kBusA);
    const auto generation = engine.buses.BusState(index)->generation + 1;
    for (int swap = 0; swap < 3; ++swap) {
        const auto plan = PrepareNeutralTestPlan(
            engine, index, harken::kApplyBusNeutralProperties);
        ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
        engine.topologyPlans.GenerationsData(plan)[index] = generation;
        ApplyNeutralTestPlan(engine, plan);
        EXPECT_EQ(generation, engine.buses.BusState(index)->generation);
        EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    }
    harken::EffectInstanceRef retired{};
    ASSERT_TRUE(engine.effectBank.RetireQueue().TryPop(retired));
    EXPECT_EQ(VN_EFFECT_GAIN, retired.type);
    engine.effectBank.Recycle(retired);
    EXPECT_FALSE(engine.effectBank.RetireQueue().TryPop(retired));
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    const auto first = engine.effectBank.Acquire(VN_EFFECT_GAIN);
    const auto second = engine.effectBank.Acquire(VN_EFFECT_GAIN);
    EXPECT_NE(harken::kEffectTypeNone, first.type);
    EXPECT_NE(harken::kEffectTypeNone, second.type);
    EXPECT_NE(first.poolIndex, second.poolIndex);
    EXPECT_EQ(harken::kEffectTypeNone,
              engine.effectBank.Acquire(VN_EFFECT_GAIN).type);
    engine.effectBank.Recycle(first);
    engine.effectBank.Recycle(second);
}

TEST_F(BusEffect, NeutralPlanSharesRetirementCapacityWithUndrainedVoiceEffects) {
    VnConfig config = BusFxConfig(2);
    config.gainEffectPoolCapacity = 8;
    config.delayEffectPoolCapacity = 1;
    config.toneEffectPoolCapacity = 1;
    config.limiterEffectPoolCapacity = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto dc = RegisterDc(0.03125f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    InstallNeutralTestGain(kBusA, 0);
    InstallNeutralTestGain(kBusA, 1);
    std::array<VnVoiceHandle, 6> voices{};
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    for (auto& voice : voices) {
        auto play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = kBusA;
        play.loopCount = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
            voice, 0, VN_EFFECT_GAIN,
            reinterpret_cast<const VnEffectDesc*>(&gain)));
    }
    Capture warm;
    PumpAndCapture(&engine, 2, warm);
    ASSERT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    for (auto voice : voices) ASSERT_EQ(VN_OK, Vn_ClearVoiceEffect(voice, 0));
    PumpAndCapture(&engine, 1, warm);
    EXPECT_EQ(8u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    std::array<harken::EffectInstanceRef, 4> otherTypes{};
    const std::array<std::uint32_t, 4> types{
        VN_EFFECT_FEEDBACK_DELAY, VN_EFFECT_THREE_BAND_TONE,
        VN_EFFECT_LIMITER, VN_EFFECT_COMPRESSOR};
    for (std::size_t i = 0; i < types.size(); ++i) {
        otherTypes[i] = engine.effectBank.Acquire(types[i]);
        ASSERT_EQ(types[i], otherTypes[i].type);
    }
    std::array<bool, 4> pushed{};
    {
        harken::memguard::AudioThreadScope scope;
        for (std::size_t i = 0; i < otherTypes.size(); ++i)
            pushed[i] = engine.effectBank.RetireQueue().TryPush(otherTypes[i]);
    }
    for (bool result : pushed) ASSERT_TRUE(result);
    const auto index = engine.buses.FindIndex(kBusA);
    for (int swap = 0; swap < 3; ++swap) {
        const auto plan = PrepareNeutralTestPlan(
            engine, index, harken::kApplyBusNeutralProperties);
        ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
        ApplyNeutralTestPlan(engine, plan);
    }
    Capture after;
    PumpAndCapture(&engine, 1, after);
    for (float sample : after.left) EXPECT_NEAR(0.1875f, sample, 1e-6f);
    std::array<bool, 8> seen{};
    std::uint32_t retiredCount = 0;
    harken::EffectInstanceRef ref{};
    while (engine.effectBank.RetireQueue().TryPop(ref)) {
        ++retiredCount;
        if (ref.type == VN_EFFECT_GAIN) {
            ASSERT_LT(ref.poolIndex, seen.size());
            EXPECT_FALSE(seen[ref.poolIndex]);
            seen[ref.poolIndex] = true;
        }
        engine.effectBank.Recycle(ref);
    }
    EXPECT_EQ(12u, retiredCount);
    for (bool retired : seen) EXPECT_TRUE(retired);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    std::array<harken::EffectInstanceRef, 8> reacquired{};
    for (auto& acquired : reacquired) {
        acquired = engine.effectBank.Acquire(VN_EFFECT_GAIN);
        EXPECT_EQ(VN_EFFECT_GAIN, acquired.type);
    }
    EXPECT_EQ(harken::kEffectTypeNone,
              engine.effectBank.Acquire(VN_EFFECT_GAIN).type);
    for (const auto& acquired : reacquired) engine.effectBank.Recycle(acquired);
}

TEST_F(BusEffect, VariableOwnerBlocksBusReplaceAndClearWithoutMutation) {
    EngineSession session(BusFxConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(delay);
    delay.delayMs = 10.0f;
    delay.feedback = 0.5f;
    delay.wet = 1.0f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kBusA, 0, VN_EFFECT_FEEDBACK_DELAY,
                         reinterpret_cast<const VnEffectDesc*>(&delay)));

    harken::Engine* engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(kBusA);
    const std::uint32_t busGeneration =
        engine->buses.GenerationAt(busIndex);
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamWet, VN_EFFECT_FEEDBACK_DELAY, &revision));
    std::uint32_t dryRevision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamDry, VN_EFFECT_FEEDBACK_DELAY, &dryRevision));
    const harken::VariableSliceRef owner{0, 1};
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamWet, owner, revision));

    const std::uint64_t publishedBefore = engine->commands.PublishedHead();
    const std::uint32_t recordsBefore =
        engine->effectParamTargets.LiveRecordCount();
    const harken::EffectInstanceRef instanceBefore =
        engine->busEffectInstanceRefs[busIndex][0];
    const VnPackHandle ownerBefore = engine->busEffectOwnerCookies[busIndex][0];
    const std::uint32_t demandBefore =
        engine->effectParamTargets.DemandCount();
    const std::uint32_t reservedBefore =
        engine->effectParamTargets.ReservedRecordCountForTests();
    const std::uint32_t retiringBefore =
        engine->effectParamTargets.RetiringRecordCountForTests();
    const float wetCommittedBefore =
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus,
            static_cast<std::uint64_t>(busIndex), 0,
            harken::kDelayParamWet);
    const float wetResolvedBefore = engine->effectParamTargets.Reduce(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), 0,
        harken::kDelayParamWet);
    const float dryCommittedBefore =
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus,
            static_cast<std::uint64_t>(busIndex), 0,
            harken::kDelayParamDry);
    const std::uint32_t delayInUseBefore =
        engine->effectBank.InUseOf(VN_EFFECT_FEEDBACK_DELAY);
    const std::uint32_t delayHighWaterBefore =
        engine->effectBank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY);
    const std::uint32_t delayRefusalsBefore =
        engine->effectBank.RefusalCountOf(VN_EFFECT_FEEDBACK_DELAY);
    const std::uint32_t toneInUseBefore =
        engine->effectBank.InUseOf(VN_EFFECT_THREE_BAND_TONE);
    const std::uint32_t toneHighWaterBefore =
        engine->effectBank.HighWaterOf(VN_EFFECT_THREE_BAND_TONE);
    const std::uint32_t toneRefusalsBefore =
        engine->effectBank.RefusalCountOf(VN_EFFECT_THREE_BAND_TONE);
    const std::uint64_t topologyCountBefore = engine->topologyPlanCount;
    const std::uint32_t topologyInUseBefore = engine->topologyPlans.InUse();
    const std::uint32_t topologyHighWaterBefore =
        engine->topologyPlans.HighWater();
    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(tone);
    tone.lowShelfGainDb = 6.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = 0.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 0.0f;
    tone.highShelfFreqHz = 6000.0f;
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetBusEffect(
                  kBusA, 0, VN_EFFECT_THREE_BAND_TONE,
                  reinterpret_cast<const VnEffectDesc*>(&tone)));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ClearBusEffect(kBusA, 0));
    EXPECT_EQ(publishedBefore, engine->commands.PublishedHead());
    EXPECT_EQ(recordsBefore, engine->effectParamTargets.LiveRecordCount());
    EXPECT_EQ(demandBefore, engine->effectParamTargets.DemandCount());
    EXPECT_EQ(reservedBefore,
              engine->effectParamTargets.ReservedRecordCountForTests());
    EXPECT_EQ(retiringBefore,
              engine->effectParamTargets.RetiringRecordCountForTests());
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY,
              engine->busEffectSlotTypes[busIndex][0]);
    const harken::EffectInstanceRef instanceAfter =
        engine->busEffectInstanceRefs[busIndex][0];
    EXPECT_EQ(instanceBefore.type, instanceAfter.type);
    EXPECT_EQ(instanceBefore.poolIndex, instanceAfter.poolIndex);
    EXPECT_EQ(instanceBefore.generation, instanceAfter.generation);
    EXPECT_EQ(ownerBefore, engine->busEffectOwnerCookies[busIndex][0]);
    std::uint32_t observedRevision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamWet, VN_EFFECT_FEEDBACK_DELAY,
        &observedRevision));
    EXPECT_EQ(revision, observedRevision);
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamDry, VN_EFFECT_FEEDBACK_DELAY,
        &observedRevision));
    EXPECT_EQ(dryRevision, observedRevision);
    EXPECT_FLOAT_EQ(wetCommittedBefore,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Bus,
                        static_cast<std::uint64_t>(busIndex), 0,
                        harken::kDelayParamWet));
    EXPECT_FLOAT_EQ(dryCommittedBefore,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Bus,
                        static_cast<std::uint64_t>(busIndex), 0,
                        harken::kDelayParamDry));
    EXPECT_EQ(delayInUseBefore,
              engine->effectBank.InUseOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(delayHighWaterBefore,
              engine->effectBank.HighWaterOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(delayRefusalsBefore,
              engine->effectBank.RefusalCountOf(VN_EFFECT_FEEDBACK_DELAY));
    EXPECT_EQ(toneInUseBefore,
              engine->effectBank.InUseOf(VN_EFFECT_THREE_BAND_TONE));
    EXPECT_EQ(toneHighWaterBefore,
              engine->effectBank.HighWaterOf(VN_EFFECT_THREE_BAND_TONE));
    EXPECT_EQ(toneRefusalsBefore,
              engine->effectBank.RefusalCountOf(VN_EFFECT_THREE_BAND_TONE));
    EXPECT_EQ(topologyCountBefore, engine->topologyPlanCount);
    EXPECT_EQ(topologyInUseBefore, engine->topologyPlans.InUse());
    EXPECT_EQ(topologyHighWaterBefore, engine->topologyPlans.HighWater());

    const std::uint64_t exactParamHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetBusEffectParam(
                  kBusA, 0, harken::kDelayParamWet, 0.5f));
    EXPECT_EQ(exactParamHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        wetResolvedBefore,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Bus,
            static_cast<std::uint64_t>(busIndex), 0,
            harken::kDelayParamWet));
    EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamWet));
    EXPECT_EQ(VN_OK,
              Vn_SetBusEffectParam(
                  kBusA, 0, harken::kDelayParamDry, 0.75f));
    EXPECT_EQ(exactParamHead + 1u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        0.75f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus,
            static_cast<std::uint64_t>(busIndex), 0,
            harken::kDelayParamDry));

    engine->effectParamTargets.ReleaseClaim(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        harken::kDelayParamWet, owner);
    EXPECT_EQ(VN_OK,
              Vn_SetBusEffectParam(
                  kBusA, 0, harken::kDelayParamWet, 0.625f));
    EXPECT_EQ(exactParamHead + 2u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        0.625f,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Bus,
            static_cast<std::uint64_t>(busIndex), 0,
            harken::kDelayParamWet));
    const float preparedParams[] = {delay.delayMs, delay.feedback, delay.wet,
                                    delay.dry};
    harken::EffectParamTargetStore::PreparedSeedToken prepared;
    ASSERT_TRUE(engine->effectParamTargets.PrepareInstalledEffectSeed(
        harken::EffectParamTargetStore::Scope::Bus,
        static_cast<std::uint64_t>(busIndex), busGeneration, 0,
        VN_EFFECT_FEEDBACK_DELAY, preparedParams, harken::kDelayParamCount,
        &prepared));
    const std::uint64_t preparedHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE,
              Vn_SetBusEffectParam(kBusA, 0, harken::kDelayParamWet, 0.5f));
    EXPECT_EQ(preparedHead, engine->commands.PublishedHead());
    engine->effectParamTargets.CancelPreparedInstalledEffectSeed(&prepared);
    EXPECT_EQ(VN_OK, Vn_ClearBusEffect(kBusA, 0));
}

// A fader-point move republishes no topology plan (the §9.7 "level/fader changes
// republish nothing" bullet's fader half; the level half is pinned in the
// bus-send tests). The publish counter stays static across the move and the
// following pump.
TEST_F(BusEffect, FaderMoveRepublishesNothing) {
    EngineSession session(BusFxConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();

    auto planCount = [&]() -> std::uint64_t {
        VnSnapshot s{};
        s.structSize = sizeof(VnSnapshot);
        Vn_CaptureSnapshot(&s);
        return s.topologyPlanCount;
    };

    const std::uint64_t before = planCount();
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 1));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 0));
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    EXPECT_EQ(before, planCount()) << "a fader-point move must republish nothing";
}

TEST_F(BusEffect, PreparedToneUpdatePreservesStateAndOtherSections) {
    VnConfig config = BusFxConfig(1);
    config.toneEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    VnThreeBandToneDesc desc{};
    desc.structSize = sizeof(VnThreeBandToneDesc);
    desc.lowShelfGainDb = 4.0f;
    desc.lowShelfFreqHz = 200.0f;
    desc.peakGainDb = -3.0f;
    desc.peakFreqHz = 900.0f;
    desc.peakQ = 0.9f;
    desc.highShelfGainDb = 2.0f;
    desc.highShelfFreqHz = 6500.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         VN_BUS_MASTER, 0, VN_EFFECT_THREE_BAND_TONE,
                         reinterpret_cast<const VnEffectDesc*>(&desc)));
    session.Engine()->nullBackend.Pump(1);

    harken::BusTree::Bus* bus = session.Engine()->buses.BusState(0);
    ASSERT_NE(nullptr, bus);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE, bus->fxSlotEffectRef[0].type);
    auto* tone =
        static_cast<harken::ToneEffectState*>(bus->fxSlots[0].state);
    ASSERT_NE(nullptr, tone);
    tone->lowShelfState[0] = {1.0f, -1.0f};
    tone->peakState[0] = {2.0f, -2.0f};
    tone->highShelfState[0] = {3.0f, -3.0f};
    const harken::BiquadCoeffs lowBefore = tone->lowShelf;
    const harken::BiquadCoeffs peakBefore = tone->peak;
    const harken::BiquadCoeffs highBefore = tone->highShelf;
    const std::uint32_t lowTailBefore = tone->lowShelfTailFrames;
    const std::uint32_t peakTailBefore = tone->peakTailFrames;
    const std::uint32_t highTailBefore = tone->highShelfTailFrames;

    constexpr float newHighGain = 8.0f;
    const harken::BiquadCoeffs prepared{0.5f, -0.25f, 0.125f, -0.0625f,
                                       0.03125f};
    constexpr std::uint32_t preparedTail = 6789;

    session.Engine()->buses.ApplySetBusEffectParam(
        0, 0, harken::kToneParamHighShelfGainDb, newHighGain,
        VN_EFFECT_FEEDBACK_DELAY, prepared, preparedTail);
    EXPECT_FLOAT_EQ(desc.highShelfGainDb, tone->highShelfGainDb);
    ExpectCoeffsEqual(highBefore, tone->highShelf);
    EXPECT_EQ(highTailBefore, tone->highShelfTailFrames);

    session.Engine()->buses.ApplySetBusEffectParam(
        0, 0, harken::kToneParamHighShelfGainDb, newHighGain,
        VN_EFFECT_THREE_BAND_TONE, prepared, preparedTail);
    EXPECT_FLOAT_EQ(newHighGain, tone->highShelfGainDb);
    EXPECT_TRUE(tone->highShelfTransition.transitioning);
    ExpectCoeffsEqual(highBefore, tone->highShelf);
    EXPECT_EQ(highTailBefore, tone->highShelfTailFrames);
    ExpectCoeffsEqual(lowBefore, tone->lowShelf);
    ExpectCoeffsEqual(peakBefore, tone->peak);
    EXPECT_EQ(lowTailBefore, tone->lowShelfTailFrames);
    EXPECT_EQ(peakTailBefore, tone->peakTailFrames);
    EXPECT_FLOAT_EQ(1.0f, tone->lowShelfState[0].z1);
    EXPECT_FLOAT_EQ(-1.0f, tone->lowShelfState[0].z2);
    EXPECT_FLOAT_EQ(2.0f, tone->peakState[0].z1);
    EXPECT_FLOAT_EQ(-2.0f, tone->peakState[0].z2);
    EXPECT_FLOAT_EQ(3.0f, tone->highShelfState[0].z1);
    EXPECT_FLOAT_EQ(-3.0f, tone->highShelfState[0].z2);
    session.Engine()->nullBackend.Pump(3);
    EXPECT_FALSE(tone->highShelfTransition.transitioning);
    ExpectCoeffsEqual(prepared, tone->highShelf);
    EXPECT_EQ(preparedTail, tone->highShelfTailFrames);
}

} // namespace
