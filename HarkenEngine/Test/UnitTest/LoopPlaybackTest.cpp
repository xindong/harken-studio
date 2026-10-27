// In-memory looping: read-position wrap inside the source stage, loop-count
// accounting, intro/outro region semantics, and the lifecycle interactions
// (single-pass bit-identity, infinite-loop endurance, stop-cut / stop-fade /
// mid-pass stop). The region-content cases drive RenderVoiceBlock directly
// for per-sample visibility on the 1:1 unresampled path (where the seam is
// EXACT, tolerance 0); the lifecycle cases run through
// the public API on the null backend, where the allocation guard also proves
// a wrap allocates nothing on the audio thread.
#include "Common/ObjectIdTestSupport.h"
#include "harken/render/Voice.h"

#include "Common/TestUtil.h"

#include <cstddef>
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

// Drives one looping voice context the way the mixer loop does, on the 1:1
// unresampled path (source rate == engine rate, ratio 1.0), collecting the
// mono master output block by block until the voice reports Done or a block
// cap is hit.
struct LoopHarness {
    explicit LoopHarness(const std::vector<float>& input,
                         std::uint64_t loopStartFrame,
                         std::uint64_t loopEndFrame, std::uint32_t loopCount)
        : ring(harken::kResampleFifoFrames, 0.0f) {
        ctx.biquadTrig = &trig;
        ctx.pcm = MakeMonoView(input);
        ctx.phase = SourcePhase::Active;
        ctx.loopStartFrame = loopStartFrame;
        ctx.loopEndFrame = loopEndFrame;
        ctx.loopInfinite = (loopCount == 0);
        ctx.loopPassesRemaining = loopCount;
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

    // Renders up to blockCap blocks, appending each block's produced frames
    // to `out`, stopping at the first Done. Returns the terminal phase.
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
    BlockArena arena{65536};
    std::vector<float> masterL;
    std::vector<float> masterR;
};

// --- region-content cases (cascade harness, 1:1 exact) ---

TEST(LoopPlayback, SinglePassIsWholeFileBitIdentical) {
    // loopCount 1 must never wrap: the whole file plays exactly once,
    // bit-identical to a non-looping voice. 300 frames spans a partial
    // final block.
    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    LoopHarness harness(sine, /*loopStart=*/0, /*loopEnd=*/sine.size(),
                        /*loopCount=*/1);
    std::vector<float> out;
    const SourcePhase phase = harness.CollectUntilDone(8, &out);
    EXPECT_EQ(SourcePhase::Done, phase);
    ASSERT_EQ(sine.size(), out.size());
    for (std::size_t i = 0; i < sine.size(); ++i) {
        ASSERT_EQ(sine[i], out[i]) << "sample " << i;
    }
}

TEST(LoopPlayback, LoopCountHonoredExactlyNTraversals) {
    // Whole-file loop, 3 passes: exactly three concatenated copies, no
    // more, no fewer, joined seamlessly (frac 0 returns the exact sample,
    // so the seam is bit-exact on this path).
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1200.0, 48000.0, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    constexpr std::uint32_t kPasses = 3;
    LoopHarness harness(sine, 0, sine.size(), kPasses);
    std::vector<float> out;
    const SourcePhase phase = harness.CollectUntilDone(16, &out);
    EXPECT_EQ(SourcePhase::Done, phase);
    ASSERT_EQ(sine.size() * kPasses, out.size())
        << "N passes must produce exactly N traversals of frames";
    for (std::size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(sine[i % sine.size()], out[i]) << "sample " << i;
    }
}

TEST(LoopPlayback, SubRegionIntroPlaysOnceThenRegionRepeatsThenOutro) {
    // Region [100, 200) inside a 300-frame file, 2 passes: the intro
    // [0,100) plays once, the region repeats twice, then the outro
    // [200,300) plays out. Output is [0,200) ++ [100,300) exactly.
    const std::vector<float> sine =
        harkentest::MakeSine(300, 900.0, 48000.0, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    constexpr std::uint64_t kLoopStart = 100;
    constexpr std::uint64_t kLoopEnd = 200;

    LoopHarness harness(sine, kLoopStart, kLoopEnd, /*loopCount=*/2);
    std::vector<float> out;
    const SourcePhase phase = harness.CollectUntilDone(16, &out);
    EXPECT_EQ(SourcePhase::Done, phase);

    std::vector<float> expected;
    for (std::size_t i = 0; i < kLoopEnd; ++i) {
        expected.push_back(sine[i]); // intro + first region traversal
    }
    for (std::size_t i = kLoopStart; i < sine.size(); ++i) {
        expected.push_back(sine[i]); // second region traversal + outro
    }
    ASSERT_EQ(expected.size(), out.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i], out[i]) << "sample " << i;
    }
    // Explicit seam check: the wrap point lands where the region restarts.
    ASSERT_EQ(sine[kLoopStart], out[kLoopEnd]) << "seam sample";
}

TEST(LoopPlayback, WholeFileSeamJoinsEndToStartExactly) {
    // The single defining property of the wrap: the sample immediately
    // after the last frame of a pass is the first frame of the next pass,
    // exactly (no smear, no dropped or duplicated sample).
    const std::vector<float> ramp = [] {
        std::vector<float> v(64);
        for (std::size_t i = 0; i < v.size(); ++i) {
            // A strictly varying signal so a one-sample slip would show.
            v[i] = -0.5f + static_cast<float>(i) * (1.0f / 64.0f);
        }
        return v;
    }();
    HARKENTEST_ASSERT_NON_SILENT(ramp);

    LoopHarness harness(ramp, 0, ramp.size(), /*loopCount=*/2);
    std::vector<float> out;
    harness.CollectUntilDone(8, &out);
    ASSERT_EQ(ramp.size() * 2, out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(ramp[i % ramp.size()], out[i]) << "sample " << i;
    }
    ASSERT_EQ(ramp.back(), out[ramp.size() - 1]) << "pre-seam";
    ASSERT_EQ(ramp.front(), out[ramp.size()]) << "post-seam";
}

TEST(LoopPlayback, InfiniteLoopNeverEndsWithStablePeaks) {
    // loopCount 0 loops forever: over a long soak the voice never reports
    // Done and every block's peak stays at the source peak (no decay, no
    // growth, no drift).
    const std::vector<float> sine =
        harkentest::MakeSine(100, 1000.0, 48000.0, 0.75f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const float sourcePeak = harkentest::Peak(sine);

    LoopHarness harness(sine, 0, sine.size(), /*loopCount=*/0);
    constexpr std::uint32_t kSoakBlocks = 400; // ~2.1 s at 48 kHz
    for (std::uint32_t b = 0; b < kSoakBlocks; ++b) {
        const StageResult r = harness.RenderBlock();
        ASSERT_EQ(kBlock, r.framesProduced) << "block " << b;
        ASSERT_EQ(SourcePhase::Active, r.phase)
            << "infinite loop must never finish, block " << b;
        const float peak = harkentest::Peak(harness.masterL);
        ASSERT_NEAR(sourcePeak, peak, 1e-4f) << "block " << b << " peak drift";
    }
}

// --- lifecycle cases (public API, null backend) ---

TEST(LoopPlayback, OldVintageDescriptorPlaysSinglePass) {
    // A caller built against the pre-loopCount struct vintage: structSize is
    // the field's offset, and raw voice start must treat the sound as a single pass
    // (not read past the caller's struct, not loop). The voice ends inside
    // the source-length window.
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.8f);
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play{};
    play.structSize = offsetof(VnPlayDesc, loopCount); // old vintage
    play.soundId = harkentest::ObjectId(1);
    play.busId = VN_BUS_MASTER;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Two blocks cover the 300-frame source; a looping voice would still be
    // alive far beyond that.
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 3, out);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)))
        << "old-vintage play must be a single pass, not a loop";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(LoopPlayback, InfiniteLoopBrokenByImmediateStop) {
    // The only thing that ends an infinite loop is a stop. An immediate cut
    // releases the slot at the next boundary.
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(120, 1000.0, 48000.0, 0.8f);
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0; // infinite
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 20, out);
    // Still looping after 20 blocks (far past the 120-frame source).
    ASSERT_NE(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));

    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)))
        << "an immediate stop must break the infinite loop";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(LoopPlayback, InfiniteLoopBrokenByFadeStop) {
    // A fade during an infinite loop ends the voice when the ramp reaches
    // zero — loops never resurrect a stopped voice.
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(120, 1.0f);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0; // infinite
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 10, out);
    ASSERT_NE(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));

    // 2 ms = 96 frames of fade, then the voice ends.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 2));
    harkentest::PumpAndCapture(session.Engine(), 2, out);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)))
        << "a fade must break the infinite loop";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(LoopPlayback, FiniteLoopStopMidPassEndsImmediately) {
    // A finite loop stopped mid-pass ends at once; it does not wait out the
    // remaining passes.
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    // A long source so 5 passes would run for many blocks; the stop lands
    // during the first pass.
    const std::vector<float> sine =
        harkentest::MakeSine(4000, 500.0, 48000.0, 0.8f);
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 5;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 2, out); // still first pass
    ASSERT_NE(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));

    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)))
        << "a mid-pass stop ends the voice without finishing the passes";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(LoopPlayback, InfiniteLoopSoakThroughApiIsGuardClean) {
    // Endurance across many wraps through the real render path: the
    // allocation guard is armed inside RenderBlock, so a clean run proves a
    // loop wrap allocates nothing on the audio thread. The voice stays alive
    // and the master peak stays stable across the soak.
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(97, 1000.0, 48000.0, 0.7f); // odd, non-aligned
    const VnPCMDesc desc = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    for (std::uint32_t b = 0; b < 300; ++b) {
        harkentest::Capture out;
        harkentest::PumpAndCapture(session.Engine(), 1, out);
        ASSERT_GT(harkentest::Peak(out.left), harkentest::kSilenceFloor)
            << "block " << b << " went silent";
    }
    EXPECT_NE(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)))
        << "infinite loop must survive the soak";
    EXPECT_EQ(0u, harken::memguard::HitCount())
        << "a loop wrap must allocate nothing on the audio thread";
}

} // namespace
