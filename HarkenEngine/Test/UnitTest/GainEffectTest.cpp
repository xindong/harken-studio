// Gain insert effect (VN_EFFECT_GAIN): a memoryless per-channel linear gain,
// distinct from a voice's fixed Voice Gain and a bus's Bus Volume fader. These
// cases drive the effect through the public API where the full path matters
// (unity bit-transparency, descriptor/param validation, pool accounting,
// block-boundary parameter steps, voice lifetime) and through the kernel seam
// directly where a per-sample analytic assertion or an unobservable reset must
// be isolated.
//
// The analytic gain table below spells its expected linear values as literals
// computed from first principles (linear = 10^(dB/20)), NOT by calling the same
// std::pow the kernel uses — so a wrong conversion (dB used directly, a 10^(dB/
// 10) power ratio, a sign flip) is caught rather than mirrored.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/render/BuiltinEffects.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr VnBusId kBusA = harkentest::ObjectId(10);

VnConfig GainConfig(std::uint32_t slots = 1, std::uint32_t gainCap = 0,
                    std::uint32_t delayCap = 0, std::uint32_t toneCap = 0) {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, kRate);
    config.fxSlotsPerVoice = slots;
    config.gainEffectPoolCapacity = gainCap;
    config.delayEffectPoolCapacity = delayCap;
    config.toneEffectPoolCapacity = toneCap;
    return config;
}

VnGainDesc GainDesc(float gainDb) {
    VnGainDesc g{};
    g.structSize = sizeof(VnGainDesc);
    g.gainDb = gainDb;
    return g;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

VnVoiceHandle PlayMono(EngineSession& session, VnSoundId id,
                       const std::vector<float>& samples) {
    const VnPCMDesc desc = MonoDesc(samples, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
    const VnPlayDesc play = MakePlayDesc(id);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    return handle;
}

const VnEffectTypeStats* FindRow(const VnSnapshot& snap, VnEffectType type) {
    for (std::uint32_t i = 0; i < snap.effectTypeStatCount; ++i) {
        if (snap.effectTypeStats[i].effectType == type) {
            return &snap.effectTypeStats[i];
        }
    }
    return nullptr;
}

TEST(GainEffect, BypassKeepsFiniteDrySignalWhenWetGainOverflows) {
    for (const bool busEffect : {false, true}) {
        SCOPED_TRACE(busEffect ? "bus" : "voice");
        auto config = GainConfig(1);
        config.fxSlotsPerBus = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto source = harkentest::MakeDC(
            6 * kBlock, std::numeric_limits<float>::max() / 8.0f);
        const auto voice = PlayMono(session, harkentest::ObjectId(1), source);
        const auto gain = GainDesc(24.0f);
        ASSERT_EQ(VN_OK, busEffect
            ? Vn_SetBusEffect(VN_BUS_MASTER, 0, VN_EFFECT_GAIN, AsBase(&gain))
            : Vn_SetVoiceEffect(voice, 0, VN_EFFECT_GAIN, AsBase(&gain)));
        ASSERT_EQ(VN_OK, busEffect
            ? Vn_SetBusEffectBypass(VN_BUS_MASTER, 0, 1)
            : Vn_SetVoiceEffectBypass(voice, 0, 1));
        Capture output;
        harkentest::PumpAndCapture(session.Engine(), 3, output);
        ASSERT_EQ(3u * kBlock, output.left.size());
        // The dry endpoint includes the final sample of the transition.
        // Final-output saturation is expected; a zero result means NaN leaked
        // out of a wet signal whose weight was already zero.
        for (std::size_t frame = kBlock - 1; frame < output.left.size(); ++frame) {
            ASSERT_EQ(1.0f, output.left[frame]) << "frame " << frame;
            ASSERT_EQ(1.0f, output.right[frame]) << "frame " << frame;
        }
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }
}

VnSnapshot TakeSnapshot() {
    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    return snap;
}

// --- unity bit-transparency ---

TEST(GainEffect, ZeroDbIsBitTransparentThroughVoiceSlot) {
    // A Gain at exactly 0 dB must be bit-for-bit identical to a no-effect render
    // (the neutral fast path leaves the buffer untouched — no multiply, no
    // rounding). A companion +6 dB render proves the slot is actually live, so
    // the byte-identity is genuine transparency of a running stage, not a dead
    // slot that never processed.
    //
    // The source is STEREO with distinct per-channel content: the voice runs its
    // insert chain at the source's channel count, so the Gain processes two
    // channels here. The +6 dB assertions below check BOTH output channels rise,
    // so a gain loop that scaled only channel 0 (leaving the right channel at its
    // no-effect level) is caught.
    auto render = [](bool withGain, float gainDb) -> Capture {
        EngineSession session(GainConfig(1));
        EXPECT_EQ(VN_OK, session.InitResult());
        const std::vector<float> left =
            harkentest::MakeSine(2000, 997.0, kRate, 0.5f);
        const std::vector<float> right =
            harkentest::MakeSine(2000, 613.0, kRate, 0.25f);
        std::vector<float> interleaved(left.size() * 2);
        for (std::size_t i = 0; i < left.size(); ++i) {
            interleaved[i * 2 + 0] = left[i];
            interleaved[i * 2 + 1] = right[i];
        }
        const VnPCMDesc pcm = harkentest::InterleavedDesc(interleaved, 2, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        if (withGain) {
            const VnGainDesc g = GainDesc(gainDb);
            EXPECT_EQ(VN_OK,
                      Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));
        }
        Capture out;
        harkentest::PumpAndCapture(session.Engine(), 8, out);
        return out;
    };

    const Capture noEffect = render(false, 0.0f);
    const Capture gain0 = render(true, 0.0f);
    const Capture gain6 = render(true, 6.0f);

    // Both channels must carry energy so the per-channel +6 dB checks are
    // meaningful (a silent channel would make its EXPECT_GT vacuous).
    HARKENTEST_ASSERT_NON_SILENT(noEffect.left);
    HARKENTEST_ASSERT_NON_SILENT(noEffect.right);
    ASSERT_EQ(noEffect.left.size(), gain0.left.size());
    ASSERT_EQ(noEffect.right.size(), gain0.right.size());

    // 0 dB: byte-for-byte identity on BOTH channels (memcmp of the raw float
    // bytes), the strongest form of transparency the contract requires.
    EXPECT_EQ(0, std::memcmp(noEffect.left.data(), gain0.left.data(),
                             noEffect.left.size() * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(noEffect.right.data(), gain0.right.data(),
                             noEffect.right.size() * sizeof(float)));

    // +6 dB is live on EVERY channel: both the left and right output peaks rise.
    // The right-channel assertion is the one that fails if the gain loop scales
    // only channel 0 — and it also confirms the 0 dB identity above is genuine
    // transparency of a running stage, not a slot that silently did nothing.
    EXPECT_GT(harkentest::Peak(gain6.left), harkentest::Peak(noEffect.left));
    EXPECT_GT(harkentest::Peak(gain6.right), harkentest::Peak(noEffect.right));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- analytic gain (per-sample, kernel seam) ---

TEST(GainEffect, AnalyticGainMultipliesEachSampleByLinearFactor) {
    // Independently-computed linear factors (10^(dB/20)) as literals; the kernel
    // must reproduce input * literal within a few float ULPs. The kernel derives
    // the linear gain in float via pow and applies one float multiply, so the
    // result can differ from the double-precision literal by the float rounding
    // of pow (<= 1 ULP of the linear value) plus the multiply rounding — bounded
    // well below 8 * FLT_EPSILON relative, which is far tighter than any real
    // formula error (a dB-direct or 10^(dB/10) mistake is orders of magnitude
    // off).
    struct GainCase {
        float gainDb;
        double linear;
    };
    const GainCase cases[] = {
        {0.0f, 1.0},                    // 10^(0/20)   = 1 exactly (unity)
        {6.0f, 1.9952623149688795},     // 10^(6/20)
        {-6.0f, 0.5011872336272722},    // 10^(-6/20)
        {20.0f, 10.0},                  // 10^(20/20)  = 10 exactly
        {-20.0f, 0.1},                  // 10^(-20/20) = 1/10
        {24.0f, 15.848931924611133},    // 10^(24/20), the +24 dB ceiling
        {-96.0f, 1.5848931924611133e-05}, // 10^(-96/20), the -96 dB floor
        {12.0f, 3.9810717055349722},    // 10^(12/20)
    };

    // Two channels with DISTINCT per-channel data (exactly representable, so
    // input(double) == input(float) and the only rounding under test is the
    // kernel's own). The gain must scale EVERY channel by the same factor,
    // computed from that channel's OWN input — so a loop clamped to channel 0
    // (later channels untouched) or a channel-0-copied-to-1 bug is caught: the
    // two inputs differ at every index.
    const std::vector<float> in0 = {0.5f,  -0.25f, 0.125f, 1.0f,
                                    -1.0f, 0.0f,   0.75f,  -0.625f};
    const std::vector<float> in1 = {-0.375f, 0.875f, -0.5f,  0.25f,
                                    -0.125f, 1.0f,   -0.75f, 0.0625f};
    ASSERT_EQ(in0.size(), in1.size());

    auto expectScaled = [](const std::vector<float>& in,
                           const std::vector<float>& out, double linear,
                           float gainDb, std::uint32_t ch) {
        for (std::size_t i = 0; i < in.size(); ++i) {
            const double expected = static_cast<double>(in[i]) * linear;
            const double tol =
                8.0 * std::fabs(expected) *
                    static_cast<double>(std::numeric_limits<float>::epsilon()) +
                1e-12;
            EXPECT_NEAR(static_cast<double>(out[i]), expected, tol)
                << "gainDb=" << gainDb << " ch=" << ch << " sample=" << i;
        }
    };

    for (const GainCase& c : cases) {
        harken::GainState st;
        harken::InsertEffect fx = harken::BuildEffect(VN_EFFECT_GAIN, &st);
        ASSERT_NE(nullptr, fx.setParam);
        ASSERT_NE(nullptr, fx.process);
        fx.setParam(&st, harken::kGainParamGainDb, c.gainDb);

        std::vector<float> w0 = in0;
        std::vector<float> w1 = in1;
        float* channels[2] = {w0.data(), w1.data()};
        fx.process(&st, channels, 2, 0, static_cast<std::uint32_t>(w0.size()));

        expectScaled(in0, w0, c.linear, c.gainDb, 0);
        expectScaled(in1, w1, c.linear, c.gainDb, 1);
    }
}

// --- descriptor and parameter validation ---

TEST(GainEffect, RejectsOutOfRangeMalformedAndNonFiniteDescriptors) {
    EngineSession session(GainConfig(2));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(400, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), sine);

    // Valid across the range: the endpoints and unity are all accepted.
    for (float db : {-96.0f, -6.0f, 0.0f, 12.0f, 24.0f}) {
        const VnGainDesc g = GainDesc(db);
        EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN,
                                           AsBase(&g)))
            << "gainDb=" << db;
    }

    // Out of range on either side is refused.
    VnGainDesc g = GainDesc(24.0001f);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));
    g = GainDesc(-96.0001f);
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));

    // Non-finite is refused.
    g = GainDesc(std::nanf(""));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));
    g = GainDesc(std::numeric_limits<float>::infinity());
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));

    // Wrong struct size and a null descriptor are refused.
    VnGainDesc badSize = GainDesc(0.0f);
    badSize.structSize = sizeof(VnGainDesc) + 4;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&badSize)));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, nullptr));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(GainEffect, BusPreAndPostFaderGainsComposeInRender) {
    // Render-proven bus placement (not just acceptance): route a deterministic DC
    // source through a bus whose fader point splits slot 0 (pre-fader, before
    // BusVolume) from slot 1 (post-fader). With a pre-fader gain, a non-unity
    // BusVolume fader, and a post-fader gain — all DISTINCT — the steady output
    // equals the input times the product of all three linear factors. The gains
    // are linear and commute with the scalar fader, so the product is
    // order-independent; what this proves is that BOTH a pre-fader and a
    // post-fader gain slot AND the fader are live in the rendered chain, and that
    // a parameter update re-renders. Linear factors are precomputed literals
    // (10^(dB/20)), not the kernel's pow. Follows BusEffectTest's DC-through-bus
    // technique.
    constexpr float kV = 0.5f;
    const float preDb = -6.0f;    // pre-fader gain
    const float postDb = 6.0f;    // post-fader gain
    const float faderDb = -12.0f; // BusVolume fader (non-unity)
    const double gPre = 0.5011872336272722;  // 10^(-6/20)
    const double gPost = 1.9952623149688795; // 10^(6/20)
    const double gFader = 0.251188643150958; // 10^(-12/20)

    EngineSession session(harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> dc = harkentest::MakeDC(4000, kV);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 1)); // slot 0 pre, slot 1 post
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, faderDb, 0)); // snap (no ramp)

    const VnGainDesc pre = GainDesc(preDb);
    const VnGainDesc post = GainDesc(postDb);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusA, 0, VN_EFFECT_GAIN, AsBase(&pre)));
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusA, 1, VN_EFFECT_GAIN, AsBase(&post)));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Settle past the block-boundary activation of the two bus effects; the DC
    // source makes the settled sample a closed-form value.
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 3, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    const double expected = static_cast<double>(kV) * gPre * gFader * gPost;
    EXPECT_NEAR(expected, static_cast<double>(out.left.back()), 1e-4);

    // A parameter update on the pre-fader gain slot re-renders: drop it to unity
    // (0 dB), so the composition loses the pre factor.
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectParam(kBusA, 0, harken::kGainParamGainDb, 0.0f));
    Capture out2;
    harkentest::PumpAndCapture(session.Engine(), 3, out2);
    const double expected2 = static_cast<double>(kV) * 1.0 * gFader * gPost;
    EXPECT_NEAR(expected2, static_cast<double>(out2.left.back()), 1e-4);
    EXPECT_GT(std::fabs(expected2 - expected), 0.02)
        << "the bus-effect param update must change the rendered output";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(GainEffect, ParamStepAtBlockBoundaryAndIndexSpace) {
    // The effect exposes exactly one parameter; index 0 updates the gain at the
    // block boundary; an unknown index is refused at the API (house convention).
    EXPECT_EQ(1u, harken::EffectParamCount(VN_EFFECT_GAIN));

    EngineSession session(GainConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> dc = harkentest::MakeDC(4000, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), dc);

    const VnGainDesc g = GainDesc(0.0f); // unity
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));

    // At unity a DC 0.5 mono voice renders 0.5 per channel (the bus-routing
    // baseline).
    Capture unity;
    harkentest::PumpAndCapture(session.Engine(), 2, unity);
    HARKENTEST_ASSERT_NON_SILENT(unity.left);
    EXPECT_NEAR(0.5f, unity.left.back(), 1e-4f);

    // A +6 dB step applies at the next block boundary: the level rises by the
    // 10^(6/20) linear factor (literal, not the kernel's pow).
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(handle, 0, harken::kGainParamGainDb,
                                            6.0f));
    Capture stepped;
    harkentest::PumpAndCapture(session.Engine(), 2, stepped);
    EXPECT_NEAR(0.5f * 1.9952623149688795f, stepped.left.back(), 1e-3f);

    // Unknown parameter index is refused (matches delay/tone).
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0, harken::kGainParamCount, 0.0f));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetVoiceEffectParam(handle, 0, 99u, 0.0f));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- zero tail ---

TEST(GainEffect, ReportsZeroTailRegardlessOfGain) {
    // Memoryless: no tail at any gain value.
    harken::GainState st;
    harken::InsertEffect fx = harken::BuildEffect(VN_EFFECT_GAIN, &st);
    ASSERT_NE(nullptr, fx.setParam);
    ASSERT_NE(nullptr, fx.tailFrames);
    for (float db : {-96.0f, 0.0f, 6.0f, 24.0f}) {
        fx.setParam(&st, harken::kGainParamGainDb, db);
        EXPECT_EQ(0u, fx.tailFrames(&st)) << "gainDb=" << db;
    }
}

TEST(GainEffect, VoiceWithOnlyGainEndsWhenSourceEnds) {
    // A voice carrying only a Gain (zero tail) must complete right after its
    // source ends — no tail drain extends its life.
    EngineSession session(GainConfig(1));
    ASSERT_EQ(VN_OK, session.InitResult());
    // A short source: 256 frames == exactly one block.
    std::vector<float> src = harkentest::MakeSine(256, 1000.0, kRate, 0.5f);
    VnVoiceHandle handle = PlayMono(session, harkentest::ObjectId(1), src);
    const VnGainDesc g = GainDesc(6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN, AsBase(&g)));

    std::uint32_t blocks = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
        session.Engine()->nullBackend.Pump(1);
        ++blocks;
        if (TakeSnapshot().activeVoiceCount == 0) {
            break;
        }
    }
    // The one-block source, plus the block that observes completion, ends the
    // voice within a couple of blocks — nowhere near a tail drain.
    EXPECT_LT(blocks, 4u);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- reset ---

TEST(GainEffect, ResetRestoresUnity) {
    // A dirtied gain returns to 0 dB (exact unity) on reset — the state the pool
    // guarantees on acquire.
    harken::GainState st;
    harken::InsertEffect fx = harken::BuildEffect(VN_EFFECT_GAIN, &st);
    ASSERT_NE(nullptr, fx.setParam);
    ASSERT_NE(nullptr, fx.reset);

    fx.setParam(&st, harken::kGainParamGainDb, 12.0f);
    EXPECT_NE(1.0f, st.gainLinear);

    // Kernel-level unknown index is a no-op (defensive; the API refuses it).
    const float before = st.gainLinear;
    fx.setParam(&st, harken::kGainParamCount, -40.0f);
    EXPECT_EQ(before, st.gainLinear);

    fx.reset(&st);
    EXPECT_EQ(1.0f, st.gainLinear); // exact 0 dB unity
}

// --- pool accounting ---

TEST(GainEffect, PoolCapacityFromOwnFieldExhaustionAndIsolation) {
    // The Gain pool sizes from its own EngineConfig field; exhaustion refuses
    // with VN_NO_EFFECT and counts against the gain row; the delay and tone rows
    // are untouched (per-type isolation).
    EngineSession session(GainConfig(/*slots=*/1, /*gainCap=*/1, /*delayCap=*/2,
                                     /*toneCap=*/2));
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<float> sine = harkentest::MakeSine(4000, 1000.0, kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle a = 0, b = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));

    const VnGainDesc g = GainDesc(0.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(a, 0, VN_EFFECT_GAIN, AsBase(&g)));

    // The gain row reads its own capacity source.
    VnSnapshot s0 = TakeSnapshot();
    const VnEffectTypeStats* gain0 = FindRow(s0, VN_EFFECT_GAIN);
    ASSERT_NE(nullptr, gain0);
    EXPECT_EQ(1u, gain0->capacity);
    EXPECT_EQ(0u, gain0->refusalCount);

    // The one-instance gain pool is now full: the second Set refuses.
    EXPECT_EQ(VN_NO_EFFECT,
              Vn_SetVoiceEffect(b, 0, VN_EFFECT_GAIN, AsBase(&g)));

    VnSnapshot s1 = TakeSnapshot();
    EXPECT_EQ(1u, FindRow(s1, VN_EFFECT_GAIN)->refusalCount);
    // Isolation: the delay and tone pools kept their own capacities and were not
    // charged a refusal by a gain exhaustion.
    const VnEffectTypeStats* delay1 = FindRow(s1, VN_EFFECT_FEEDBACK_DELAY);
    const VnEffectTypeStats* tone1 = FindRow(s1, VN_EFFECT_THREE_BAND_TONE);
    ASSERT_NE(nullptr, delay1);
    ASSERT_NE(nullptr, tone1);
    EXPECT_EQ(2u, delay1->capacity);
    EXPECT_EQ(2u, tone1->capacity);
    EXPECT_EQ(0u, delay1->refusalCount);
    EXPECT_EQ(0u, tone1->refusalCount);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(GainEffect, ZeroCapacityResolvesToDefaultSixteen) {
    // A zero gainEffectPoolCapacity resolves to the documented default (16),
    // mirroring the tone field exactly.
    EngineSession session(GainConfig(/*slots=*/1, /*gainCap=*/0));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnSnapshot snap = TakeSnapshot();
    const VnEffectTypeStats* gain = FindRow(snap, VN_EFFECT_GAIN);
    ASSERT_NE(nullptr, gain);
    EXPECT_EQ(16u, gain->capacity);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

} // namespace
