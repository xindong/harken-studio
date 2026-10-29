// Full insert-effect chain mechanics with synthetic effects (defined here,
// in test code only — nothing synthetic ships in the runtime): ascending
// slot processing, empty slots skipped at zero cost and bit-transparent,
// the eight-slot capacity, the aggregate tail as the serial sum over occupied
// slots regardless of bypass, plus the config plumbing and the
// Vn_SetVoiceEffectBypass argument contract. Slot ORDER (non-commuting
// effects) and bypass click-freedom are the golden cases; this file proves
// the counting/structural facts.
#include "Common/ObjectIdTestSupport.h"
#include "harken/render/Voice.h"
#include "harken/render/EffectChain.h"

#include "Common/TestUtil.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using harken::BlockArena;
using harken::PCMView;
using harken::RenderVoiceBlock;
using harken::ResampleQuality;
using harken::ResampleTables;
using harken::SourcePhase;
using harken::StageResult;
using harken::VoiceRenderContext;

constexpr std::uint32_t kBlock = 256;

const ResampleTables* Tables() {
    static ResampleTables tables;
    static bool initialized = false;
    if (!initialized) {
        tables.Init();
        initialized = true;
    }
    return &tables;
}

PCMView MakeMonoView(const std::vector<float>& samples) {
    PCMView view;
    view.channels[0] = samples.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = samples.size();
    return view;
}

// A per-slot process probe: an add-a-constant transform that also tallies
// how many times it ran and over how many frames. Shared across slots it
// counts total chain invocations; the constant makes ascending order and
// all-slots-run observable in the output.
struct AddProbe {
    float constant = 0.0f;
    std::uint64_t* calls = nullptr;
    std::uint64_t* frames = nullptr;
};

void AddProbeProcess(void* state, float* const* channels,
                     std::uint32_t channelCount, std::uint32_t offset,
                     std::uint32_t frames) {
    auto* probe = static_cast<AddProbe*>(state);
    if (probe->calls != nullptr) {
        *probe->calls += 1;
    }
    if (probe->frames != nullptr) {
        *probe->frames += frames;
    }
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + offset;
        for (std::uint32_t i = 0; i < frames; ++i) {
            p[i] += probe->constant;
        }
    }
}

// A tail-only effect: identity on the signal, reports a configurable tail so
// the drain length is under test control. state points at the tail count.
void IdentityProcess(void* /*state*/, float* const* /*channels*/,
                     std::uint32_t /*channelCount*/, std::uint32_t /*offset*/,
                     std::uint32_t /*frames*/) {}

std::uint32_t ConfiguredTail(const void* state) {
    return *static_cast<const std::uint32_t*>(state);
}

// A clearly-wet transform (multiply); state points at the factor. Used to
// tell a bypassed (dry) output apart from an active (wet) one.
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

TEST(EffectChain, BypassEndpointPreservesDryRangeAndRunsEffectState) {
    const float infinity = std::numeric_limits<float>::infinity();
    for (const bool ramping : {false, true}) {
        SCOPED_TRACE(ramping);
        float signal[] = {7.0f, 0.25f, -0.5f, 9.0f};
        float dry[2]{};
        float* planes[] = {signal};
        float* scratch[] = {dry};
        std::uint64_t calls = 0, frames = 0;
        AddProbe probe{infinity, &calls, &frames};
        harken::InsertEffect effect{};
        effect.process = AddProbeProcess;
        effect.state = &probe;
        bool transitioning = ramping;
        float mix = ramping ? 0.0f : 1.0f;
        harken::ProcessChainSlot(effect, transitioning, mix, 1.0f,
                                 planes, scratch, 1, 1, 2);
        EXPECT_EQ(1u, calls);
        EXPECT_EQ(2u, frames);
        EXPECT_EQ(7.0f, signal[0]);
        EXPECT_EQ(9.0f, signal[3]);
        if (!ramping) EXPECT_EQ(0.25f, signal[1]);
        EXPECT_EQ(-0.5f, signal[2]);
        EXPECT_EQ(1.0f, mix);
        EXPECT_FALSE(transitioning);
    }
}

// Drives a single-pass voice context with a configurable chain block by
// block, collecting the mono master output until Done.
struct ChainHarness {
    explicit ChainHarness(const std::vector<float>& input,
                          std::uint32_t slotCount)
        : ring(harken::kResampleFifoFrames, 0.0f) {
        ctx.biquadTrig = &trig;
        ctx.pcm = MakeMonoView(input);
        ctx.phase = SourcePhase::Active;
        ctx.loopStartFrame = 0;
        ctx.loopEndFrame = input.size();
        ctx.loopPassesRemaining = 1; // single pass
        ctx.fxSlotCount = slotCount;
        ctx.gain.Set(1.0f, 0);
        float* rings[1] = {ring.data()};
        ctx.resampler = resamplerBacking;
        ctx.resampler.Reset(rings, 1, ResampleQuality::Hermite, 1.0,
                            Tables());
    }

    StageResult RenderBlock() {
        arena.Reset();
        masterL.assign(kBlock, 0.0f);
        masterR.assign(kBlock, 0.0f);
        float* master[harken::kOutputChannels] = {masterL.data(),
                                                 masterR.data()};
        return RenderVoiceBlock(ctx, arena, master, kBlock, 1,
                                VN_CHANNEL_LAYOUT_MONO);
    }

    SourcePhase CollectUntilDone(std::uint32_t blockCap,
                                 std::vector<float>* out) {
        SourcePhase phase = SourcePhase::Active;
        for (std::uint32_t b = 0; b < blockCap; ++b) {
            const StageResult r = RenderBlock();
            for (std::uint32_t i = 0; i < r.framesProduced; ++i) {
                out->push_back(masterL[i]);
            }
            phase = r.phase;
            if (phase == SourcePhase::Done) {
                break;
            }
        }
        return phase;
    }

    harken::BiquadTrigTable trig;
    harken::Resampler resamplerBacking;
    VoiceRenderContext ctx;
    std::vector<float> ring;
    BlockArena arena{262144}; // room for scratch + a dry plane per bypass slot
    std::vector<float> masterL;
    std::vector<float> masterR;
};

TEST(EffectChain, AllEightSlotsRunOverEveryFrame) {
    // Eight occupied slots each add 1 to a silent source: the output is
    // exactly 8, proving all eight ran, and the shared counter records one
    // call per slot per produced block.
    const std::vector<float> dc = harkentest::MakeDC(300, 0.0f);

    std::uint64_t calls = 0;
    std::uint64_t frames = 0;
    ChainHarness harness(dc, /*slotCount=*/8);
    AddProbe probes[8];
    for (std::uint32_t s = 0; s < 8; ++s) {
        probes[s].constant = 1.0f;
        probes[s].calls = &calls;
        probes[s].frames = &frames;
        harness.ctx.fxSlots[s].process = AddProbeProcess;
        harness.ctx.fxSlots[s].state = &probes[s];
    }

    std::vector<float> out;
    harness.CollectUntilDone(4, &out);
    ASSERT_EQ(dc.size(), out.size());
    HARKENTEST_ASSERT_NON_SILENT(out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(8.0f, out[i]) << "sample " << i;
    }
    // 300 frames over 256-frame blocks = two blocks with data; eight slots
    // each: 16 calls, every source frame seen once per slot.
    EXPECT_EQ(16u, calls);
    EXPECT_EQ(dc.size() * 8u, frames);
}

TEST(EffectChain, EmptySlotsSkippedTransparentAndUncounted) {
    // An eight-slot chain with occupied slots only at 0, 3, 7 (the rest
    // empty). Each occupied effect is an identity that only counts, so the
    // output must equal the source exactly (empty slots contribute nothing —
    // bit-identical to a no-effect render), and the shared counter must show
    // exactly three calls per produced block, never eight: empty slots are
    // not invoked.
    const std::vector<float> sine =
        harkentest::MakeSine(300, 900.0, 48000.0, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    std::uint64_t calls = 0;
    std::uint64_t frames = 0;
    ChainHarness harness(sine, /*slotCount=*/8);
    AddProbe probes[3]; // installed into slots 0, 3, 7
    const std::uint32_t occupied[3] = {0, 3, 7};
    for (std::uint32_t k = 0; k < 3; ++k) {
        probes[k].constant = 0.0f; // identity: only counts
        probes[k].calls = &calls;
        probes[k].frames = &frames;
        harness.ctx.fxSlots[occupied[k]].process = AddProbeProcess;
        harness.ctx.fxSlots[occupied[k]].state = &probes[k];
    }

    std::vector<float> out;
    harness.CollectUntilDone(4, &out);
    ASSERT_EQ(sine.size(), out.size());
    for (std::size_t i = 0; i < sine.size(); ++i) {
        ASSERT_EQ(sine[i], out[i]) << "sample " << i;
    }
    // Two produced blocks, three occupied slots each = six calls; the five
    // empty slots are never invoked.
    EXPECT_EQ(6u, calls);
    EXPECT_EQ(sine.size() * 3u, frames);
}

TEST(EffectChain, AggregateTailSumsOccupiedRegardlessOfBypass) {
    // Two occupied slots reporting tails 300 and 100. The drain must last
    // their serial sum (400), and bypassing the longer-tail slot must NOT
    // shorten it: a bypassed effect still rings.
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.7f);
    std::uint32_t tailLong = 300;
    std::uint32_t tailShort = 100;

    auto drainFrames = [&](bool bypassLong) -> std::size_t {
        ChainHarness harness(sine, /*slotCount=*/2);
        harness.ctx.fxSlots[0].process = IdentityProcess;
        harness.ctx.fxSlots[0].tailFrames = ConfiguredTail;
        harness.ctx.fxSlots[0].state = &tailLong;
        harness.ctx.fxSlots[1].process = IdentityProcess;
        harness.ctx.fxSlots[1].tailFrames = ConfiguredTail;
        harness.ctx.fxSlots[1].state = &tailShort;
        if (bypassLong) {
            harness.ctx.fxBypassMix[0] = 1.0f; // steady bypassed (dry)
        }
        std::vector<float> out;
        const SourcePhase phase = harness.CollectUntilDone(8, &out);
        EXPECT_EQ(SourcePhase::Done, phase);
        return out.size();
    };

    // Source (100) + serial tail sum (300 + 100) = 500, whether or not the
    // longer slot is bypassed.
    EXPECT_EQ(sine.size() + tailLong + tailShort,
              drainFrames(/*bypassLong=*/false));
    EXPECT_EQ(sine.size() + tailLong + tailShort,
              drainFrames(/*bypassLong=*/true));
}

TEST(EffectChain, AggregateTailSaturatesBelowInfiniteSentinel) {
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.7f);
    std::uint32_t nearCap = harken::kInfiniteTail - 100u;
    std::uint32_t extra = 200u;
    ChainHarness harness(sine, /*slotCount=*/2);
    harness.ctx.fxSlots[0].process = IdentityProcess;
    harness.ctx.fxSlots[0].tailFrames = ConfiguredTail;
    harness.ctx.fxSlots[0].state = &nearCap;
    harness.ctx.fxSlots[1].process = IdentityProcess;
    harness.ctx.fxSlots[1].tailFrames = ConfiguredTail;
    harness.ctx.fxSlots[1].state = &extra;

    const StageResult result = harness.RenderBlock();
    ASSERT_EQ(SourcePhase::Draining, result.phase);
    ASSERT_EQ(harken::kInfiniteTail - 1u - 156u,
              harness.ctx.tailRemaining);
    EXPECT_NE(harken::kInfiniteTail, harness.ctx.tailRemaining);
}

TEST(EffectChain, AggregateTailIncludesVoiceToneSections) {
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.7f);
    ChainHarness harness(sine, /*slotCount=*/0);
    harness.ctx.lpfEnabled = true;
    harness.ctx.hpfEnabled = true;
    harness.ctx.shelfEnabled = true;
    harness.ctx.lpfCoeffs = harken::MakeLowpass(48000.0f, 100.0f, 0.70710678f);
    harness.ctx.hpfCoeffs = harken::MakeHighpass(48000.0f, 100.0f, 0.70710678f);
    harness.ctx.shelfCoeffs =
        harken::MakeLowShelf(48000.0f, 100.0f, 0.70710678f, 6.0f);
    harness.ctx.lpfTailFrames =
        harken::EstimateBiquadTailFrames(harness.ctx.lpfCoeffs);
    harness.ctx.hpfTailFrames =
        harken::EstimateBiquadTailFrames(harness.ctx.hpfCoeffs);
    harness.ctx.shelfTailFrames =
        harken::EstimateBiquadTailFrames(harness.ctx.shelfCoeffs);

    std::uint32_t expected = harken::SaturatingTailAdd(
        harness.ctx.lpfTailFrames, harness.ctx.hpfTailFrames);
    expected = harken::SaturatingTailAdd(expected, harness.ctx.shelfTailFrames);
    ASSERT_GT(expected, 156u);

    const StageResult result = harness.RenderBlock();
    EXPECT_EQ(SourcePhase::Draining, result.phase);
    EXPECT_EQ(expected - 156u, harness.ctx.tailRemaining);
}

TEST(EffectChain, ReplacingTailDuringDrainRecomputesAggregate) {
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.5f);
    std::uint32_t initialLong = 700u;
    std::uint32_t initialShort = 300u;
    std::uint32_t replacement = 100u;

    auto install = [&](ChainHarness& h) {
        h.ctx.fxSlots[0].process = IdentityProcess;
        h.ctx.fxSlots[0].tailFrames = ConfiguredTail;
        h.ctx.fxSlots[0].state = &initialLong;
        h.ctx.fxSlots[1].process = IdentityProcess;
        h.ctx.fxSlots[1].tailFrames = ConfiguredTail;
        h.ctx.fxSlots[1].state = &initialShort;
    };

    ChainHarness baseline(sine, /*slotCount=*/2);
    install(baseline);
    std::vector<float> baselineOut;
    EXPECT_EQ(SourcePhase::Done, baseline.CollectUntilDone(16, &baselineOut));

    ChainHarness replaced(sine, /*slotCount=*/2);
    install(replaced);
    std::vector<float> replacedOut;
    const StageResult first = replaced.RenderBlock();
    ASSERT_EQ(SourcePhase::Draining, first.phase);
    for (std::uint32_t i = 0; i < first.framesProduced; ++i) {
        replacedOut.push_back(replaced.masterL[i]);
    }
    replaced.ctx.fxSlots[0].state = &replacement;
    EXPECT_EQ(SourcePhase::Done, replaced.CollectUntilDone(16, &replacedOut));
    EXPECT_EQ(sine.size() + initialShort + replacement, replacedOut.size());
    EXPECT_EQ(static_cast<std::size_t>(initialLong - replacement),
              baselineOut.size() - replacedOut.size());
}

TEST(EffectChain, ConfigFxSlotsPlumbToVoiceChainLength) {
    // The resolved fxSlotsPerVoice becomes the voice's active chain length.
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, 48000);
    config.fxSlotsPerVoice = 8;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(200, 1000.0, 48000.0, 0.5f);
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::VoiceRenderContext* voice =
        session.Engine()->voices.Resolve(harkentest::RenderHandle(session.Engine(
            ), handle));
    ASSERT_NE(nullptr, voice);
    EXPECT_EQ(8u, voice->fxSlotCount);
}

TEST(EffectChain, ConfigFxSlotsOverCapacityRefused) {
    // The chain is capped at kMaxFxSlotsPerVoice (8); a config asking for
    // more is refused at Init like every other over-ceiling field.
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, 48000);
    config.fxSlotsPerVoice = harken::kMaxFxSlotsPerVoice + 1;
    harkentest::EngineSession session(config);
    EXPECT_EQ(VN_INVALID_CONFIG, session.InitResult());
}

TEST(EffectChain, SetVoiceEffectBypassArgumentContract) {
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, 48000);
    config.fxSlotsPerVoice = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(200, 1000.0, 48000.0, 0.5f);
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // bypass01 is a two-value flag (spatialized-flag precedent).
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffectBypass(handle, 0, 2));
    // slot must index the configured chain.
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceEffectBypass(handle, 4, 1));
    // In range: accepted for both flag values.
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 3, 1));
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 0));

    // A stale handle is a benign no-op like every other voice op.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceEffectBypass(handle, 0, 1));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(EffectChain, BypassOnEmptySlotSettlesSoLaterEffectIsDryFromSampleZero) {
    // Bypassing an EMPTY slot must settle the mix immediately (the cascade
    // skips empty slots before a ramp could settle). Otherwise the ramp
    // lingers, and when an effect is later installed into the slot its first
    // block would crossfade wet->dry and leak one audibly wet block despite
    // the earlier bypass. After the fix that first block is fully dry from
    // sample 0.
    VnConfig config = harkentest::MakeNullConfig(0, kBlock, 48000);
    config.fxSlotsPerVoice = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    constexpr float kDc = 0.2f;
    const std::vector<float> dc = harkentest::MakeDC(2000, kDc);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Bypass the still-empty slot 0, then render one block so both the play
    // and the bypass commands apply (the empty-slot bypass settles here).
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));
    harkentest::Capture warmup;
    harkentest::PumpAndCapture(session.Engine(), 1, warmup);

    // Now install a clearly-wet effect (x4) into the settled-bypassed slot.
    float factor = 4.0f;
    harken::VoiceRenderContext* voice =
        session.Engine()->voices.Resolve(harkentest::RenderHandle(session.Engine(
            ), handle));
    ASSERT_NE(nullptr, voice);
    voice->fxSlots[0].process = MultiplyProcess;
    voice->fxSlots[0].state = &factor;

    // The first block after install is fully dry (the 0.2 source), never the
    // 0.8 wet result — no leftover ramp leaks.
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < out.left.size(); ++i) {
        ASSERT_NEAR(kDc, out.left[i], 1e-6f)
            << "sample " << i << " leaked wet after empty-slot bypass";
    }
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// A test-only synthetic effect with an unbounded tail: it overwrites its input
// with a constant (so the chain's output stays non-silent while the voice
// drains) and reports kInfiniteTail, exercising the cascade's infinite-tail
// aggregation without shipping any synthetic effect in the runtime.
void ConstantFillProcess(void* state, float* const* channels,
                         std::uint32_t channelCount, std::uint32_t offset,
                         std::uint32_t frames) {
    const float c = *static_cast<const float*>(state);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + offset;
        for (std::uint32_t i = 0; i < frames; ++i) {
            p[i] = c;
        }
    }
}

std::uint32_t InfiniteTailReport(const void* /*state*/) {
    return harken::kInfiniteTail;
}

TEST(EffectChain, InfiniteTailNeverAutoCompletes) {
    // A slot reporting an infinite tail keeps the voice Draining forever after
    // the source ends: it must never auto-complete, and the chain must keep
    // producing (the constant-fill stays audible every block).
    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.5f);
    float fill = 0.5f;
    ChainHarness harness(sine, /*slotCount=*/1);
    harness.ctx.fxSlots[0].process = ConstantFillProcess;
    harness.ctx.fxSlots[0].tailFrames = InfiniteTailReport;
    harness.ctx.fxSlots[0].state = &fill;

    for (std::uint32_t b = 0; b < 64; ++b) {
        const harken::StageResult r = harness.RenderBlock();
        ASSERT_NE(harken::SourcePhase::Done, r.phase)
            << "infinite-tail voice auto-completed at block " << b;
        ASSERT_EQ(kBlock, r.framesProduced) << "block " << b;
        ASSERT_NEAR(0.5f, harness.masterL[0], 1e-6f) << "block " << b;
    }
}

TEST(EffectChain, InfiniteTailDominatesFiniteSlots) {
    // Aggregate tail with one infinite slot and one finite slot is infinite:
    // the sentinel dominates any finite count, so the voice still never
    // auto-completes.
    const std::vector<float> sine =
        harkentest::MakeSine(200, 1000.0, 48000.0, 0.5f);
    float fill = 0.25f;
    std::uint32_t finiteTail = 128;
    ChainHarness harness(sine, /*slotCount=*/2);
    harness.ctx.fxSlots[0].process = IdentityProcess;
    harness.ctx.fxSlots[0].tailFrames = ConfiguredTail;
    harness.ctx.fxSlots[0].state = &finiteTail;
    harness.ctx.fxSlots[1].process = ConstantFillProcess;
    harness.ctx.fxSlots[1].tailFrames = InfiniteTailReport;
    harness.ctx.fxSlots[1].state = &fill;

    for (std::uint32_t b = 0; b < 32; ++b) {
        const harken::StageResult r = harness.RenderBlock();
        ASSERT_NE(harken::SourcePhase::Done, r.phase) << "block " << b;
    }
}

TEST(EffectChain, ClearingLongestTailDuringDrainEndsEarly) {
    // A voice already in tail drain must re-derive its aggregate tail from the
    // slots still occupied. Two occupied slots report a long tail (6000) and a
    // short tail (1500). Clearing the LONG-tail slot on the first draining
    // block must make the voice finish per the surviving SHORT tail: relative
    // to a run that never clears, the total output is shorter by exactly the
    // removed LONG contribution (6000). A frozen-at-drain-start aggregate
    // would ignore the clear and the two totals would be identical.
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.5f);
    std::uint32_t tailLong = 6000;
    std::uint32_t tailShort = 1500;

    auto installTwoTails = [&](ChainHarness& h) {
        h.ctx.fxSlots[0].process = IdentityProcess;
        h.ctx.fxSlots[0].tailFrames = ConfiguredTail;
        h.ctx.fxSlots[0].state = &tailLong;
        h.ctx.fxSlots[1].process = IdentityProcess;
        h.ctx.fxSlots[1].tailFrames = ConfiguredTail;
        h.ctx.fxSlots[1].state = &tailShort;
    };

    // Baseline: never clear — the voice drains the full long tail.
    std::size_t totalNoClear = 0;
    {
        ChainHarness harness(sine, /*slotCount=*/2);
        installTwoTails(harness);
        std::vector<float> out;
        const SourcePhase phase = harness.CollectUntilDone(64, &out);
        EXPECT_EQ(SourcePhase::Done, phase);
        totalNoClear = out.size();
    }

    // Clear the long-tail slot on the first draining block.
    std::size_t totalCleared = 0;
    {
        ChainHarness harness(sine, /*slotCount=*/2);
        installTwoTails(harness);
        std::vector<float> out;
        bool cleared = false;
        SourcePhase phase = SourcePhase::Active;
        for (std::uint32_t b = 0; b < 64; ++b) {
            const StageResult r = harness.RenderBlock();
            for (std::uint32_t i = 0; i < r.framesProduced; ++i) {
                out.push_back(harness.masterL[i]);
            }
            phase = r.phase;
            if (phase == SourcePhase::Draining && !cleared) {
                harness.ctx.fxSlots[0] = harken::InsertEffect{};
                cleared = true;
            }
            if (phase == SourcePhase::Done) {
                break;
            }
        }
        EXPECT_EQ(SourcePhase::Done, phase);
        EXPECT_TRUE(cleared);
        totalCleared = out.size();
    }

    ASSERT_GT(totalNoClear, tailLong); // it really drained the long tail
    EXPECT_LT(totalCleared, totalNoClear);
    EXPECT_EQ(static_cast<std::size_t>(tailLong),
              totalNoClear - totalCleared)
        << "cleared voice must finish per the surviving shorter tail";
}

TEST(EffectChain, ClearingInfiniteTailDuringDrainLetsVoiceFinish) {
    // A synthetic infinite-tail slot keeps the voice Draining forever. Clearing
    // it mid-drain must drop the aggregate to the surviving finite slot; here
    // that finite tail (1500) has already fully elapsed over the infinite-drain
    // blocks, so the voice must reach Done promptly instead of hanging.
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.5f);
    float fill = 0.5f;
    std::uint32_t finiteTail = 1500;
    ChainHarness harness(sine, /*slotCount=*/2);
    harness.ctx.fxSlots[0].process = IdentityProcess;
    harness.ctx.fxSlots[0].tailFrames = ConfiguredTail;
    harness.ctx.fxSlots[0].state = &finiteTail;
    harness.ctx.fxSlots[1].process = ConstantFillProcess;
    harness.ctx.fxSlots[1].tailFrames = InfiniteTailReport;
    harness.ctx.fxSlots[1].state = &fill;

    // While the infinite slot is present the voice never completes.
    std::uint32_t drainBlocks = 0;
    for (std::uint32_t b = 0; b < 16; ++b) {
        const StageResult r = harness.RenderBlock();
        ASSERT_NE(SourcePhase::Done, r.phase) << "block " << b;
        if (r.phase == SourcePhase::Draining) {
            ++drainBlocks;
        }
    }
    ASSERT_GT(drainBlocks, 8u); // genuinely stuck draining before the clear

    harness.ctx.fxSlots[1] = harken::InsertEffect{}; // clear the infinite slot
    SourcePhase phase = SourcePhase::Draining;
    std::uint32_t after = 0;
    for (std::uint32_t b = 0; b < 32; ++b) {
        phase = harness.RenderBlock().phase;
        ++after;
        if (phase == SourcePhase::Done) {
            break;
        }
    }
    EXPECT_EQ(SourcePhase::Done, phase);
    // The finite survivor was long since elapsed, so it ends almost at once.
    EXPECT_LE(after, 3u);
}

TEST(EffectChain, ClearingInfiniteTailFallsBackToSurvivingFiniteTail) {
    // Same fallback, but the infinite slot is cleared EARLY while the finite
    // survivor still has tail left: the voice must then drain the survivor's
    // REMAINING tail (measured from the elapsed count accumulated during the
    // infinite phase) and complete — finite and bounded by that tail, neither
    // immediate nor endless.
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.5f);
    float fill = 0.5f;
    std::uint32_t finiteTail = 3000;
    ChainHarness harness(sine, /*slotCount=*/2);
    harness.ctx.fxSlots[0].process = IdentityProcess;
    harness.ctx.fxSlots[0].tailFrames = ConfiguredTail;
    harness.ctx.fxSlots[0].state = &finiteTail;
    harness.ctx.fxSlots[1].process = ConstantFillProcess;
    harness.ctx.fxSlots[1].tailFrames = InfiniteTailReport;
    harness.ctx.fxSlots[1].state = &fill;

    // Take a few infinite-drain blocks (survivor tail not yet elapsed).
    std::uint32_t draining = 0;
    while (draining < 3) {
        const StageResult r = harness.RenderBlock();
        ASSERT_NE(SourcePhase::Done, r.phase);
        if (r.phase == SourcePhase::Draining) {
            ++draining;
        }
    }

    harness.ctx.fxSlots[1] = harken::InsertEffect{}; // clear the infinite slot
    SourcePhase phase = SourcePhase::Draining;
    std::uint32_t after = 0;
    for (std::uint32_t b = 0; b < 40; ++b) {
        phase = harness.RenderBlock().phase;
        ++after;
        if (phase == SourcePhase::Done) {
            break;
        }
    }
    EXPECT_EQ(SourcePhase::Done, phase);
    EXPECT_GT(after, 1u); // drained a real remainder, not an immediate finish
    EXPECT_LT(after, 14u); // bounded by the finite survivor (~3000 frames)
}

TEST(EffectChain, FadeStopEndsInfiniteTailVoice) {
    // An infinite tail is not immortal: a fade-stop (the audio-side Stop
    // mechanism) still drives the voice to Done, because the fade completing
    // truncates the block and sets Done after the tail logic runs. A short
    // source (under one block) means the very first block already drains.
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.5f);
    float fill = 0.5f;
    ChainHarness harness(sine, /*slotCount=*/1);
    harness.ctx.fxSlots[0].process = ConstantFillProcess;
    harness.ctx.fxSlots[0].tailFrames = InfiniteTailReport;
    harness.ctx.fxSlots[0].state = &fill;

    // Every block already drains the infinite tail (source ended in block 0).
    for (int b = 0; b < 3; ++b) {
        const harken::StageResult r = harness.RenderBlock();
        ASSERT_EQ(harken::SourcePhase::Draining, r.phase) << "block " << b;
    }
    // Arm a short fade; the next block completes it and ends the voice.
    harness.ctx.fading = true;
    harness.ctx.fadeTotalFrames = 100;
    harness.ctx.fadeDoneFrames = 0;
    const harken::StageResult r = harness.RenderBlock();
    EXPECT_EQ(harken::SourcePhase::Done, r.phase);
}

} // namespace
