// Voice Render Cascade protocol behavior: sample-accurate start, the
// framesProduced/SourcePhase result facts, insert-effect tail drain, and
// fade-stop. Most cases drive RenderVoiceBlock directly for per-block
// protocol visibility; the last case exercises the same fade path through
// the public Vn_Stop.
#include "Common/ObjectIdTestSupport.h"
#include "harken/render/Voice.h"

#include "Common/TestUtil.h"

#include <limits>

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

// Drives one voice context block-by-block the way the mixer loop does,
// with a fresh planar master accumulator per block.
struct CascadeHarness {
    explicit CascadeHarness(const std::vector<float>& input,
                            double ratio = 1.0)
        : ring(harken::kResampleFifoFrames, 0.0f) {
        ctx.biquadTrig = &trig;
        ctx.pcm = MakeMonoView(input);
        ctx.phase = SourcePhase::Active;
        // One active chain slot, left empty (an empty slot is a zero-cost
        // identity, exactly the old default passthrough); the tail-drain
        // case installs an effect into slot 0.
        ctx.fxSlotCount = 1;
        ctx.gain.Set(1.0f, 0);
        float* rings[1] = {ring.data()};
        ctx.resampler.Reset(rings, 1, ResampleQuality::Hermite, ratio,
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

    harken::BiquadTrigTable trig;
    VoiceRenderContext ctx;
    std::vector<float> ring;
    BlockArena arena{65536};
    std::vector<float> masterL;
    std::vector<float> masterR;
};

constexpr float kTailOffset = 0.25f;

void AddOffsetProcess(void* state, float* const* channels,
                      std::uint32_t channelCount, std::uint32_t offset,
                      std::uint32_t frames);
std::uint32_t FixedTail300(const void* state);

TEST(CascadeProtocol, SampleAccurateStartRendersAtOffset) {
    const std::vector<float> sine =
        harkentest::MakeSine(400, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    CascadeHarness harness(sine);
    harness.ctx.startOffsetFrames = 37;

    const StageResult result = harness.RenderBlock();
    EXPECT_EQ(kBlock - 37u, result.framesProduced);
    EXPECT_EQ(SourcePhase::Active, result.phase);

    // Every sample before the offset is untouched silence; from the offset
    // on, the source plays from its first sample — no shift, no smear.
    for (std::size_t i = 0; i < 37; ++i) {
        ASSERT_EQ(0.0f, harness.masterL[i]) << "pre-offset sample " << i;
    }
    for (std::size_t i = 37; i < kBlock; ++i) {
        ASSERT_EQ(sine[i - 37], harness.masterL[i]) << "sample " << i;
    }
}

TEST(CascadeProtocol, PendingImmediateDurationStartsAtAdmittedAudioFrame) {
    const std::vector<float> source = harkentest::MakeDC(2000, 0.5f);
    CascadeHarness harness(source);
    harness.ctx.timelineGated = true;
    harness.ctx.pendingAuthoredDurationFrames = 128;
    harness.ctx.timelineEvents.quantumIndex = 3;
    EXPECT_EQ(0u, harness.RenderBlock().framesProduced);
    EXPECT_EQ(UINT64_MAX, harness.ctx.authoredSourceDeadlineFrame);

    harness.ctx.timelineEvents.quantumIndex = 4;
    harness.ctx.timelineEvents.eventCount = 1;
    auto& start = harness.ctx.timelineEvents.events[0];
    start.kind = harken::TimelineEventKind::Start;
    start.offset = 9;
    start.frame = 4u * kBlock + start.offset;
    const StageResult started = harness.RenderBlock();
    EXPECT_EQ(4u * kBlock + 9u + 128u,
              harness.ctx.authoredSourceDeadlineFrame);
    EXPECT_EQ(UINT64_MAX, harness.ctx.pendingAuthoredDurationFrames);
    EXPECT_EQ(128u, started.framesProduced);
    for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
        EXPECT_FLOAT_EQ(frame >= 9u && frame < 137u ? 0.5f : 0.0f,
                        harness.masterL[frame]);
    }
}

TEST(CascadeProtocol, FramesProducedAndPhaseAcrossSourceEnd) {
    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    CascadeHarness harness(sine);

    const StageResult block1 = harness.RenderBlock();
    EXPECT_EQ(kBlock, block1.framesProduced);
    EXPECT_EQ(SourcePhase::Active, block1.phase);

    const StageResult block2 = harness.RenderBlock();
    EXPECT_EQ(300u - kBlock, block2.framesProduced); // the remaining 44
    EXPECT_EQ(SourcePhase::Done, block2.phase);

    HARKENTEST_ASSERT_NON_SILENT(harness.masterL);
    for (std::size_t i = 0; i < 44; ++i) {
        ASSERT_EQ(sine[kBlock + i], harness.masterL[i]) << "sample " << i;
    }
    for (std::size_t i = 44; i < kBlock; ++i) {
        ASSERT_EQ(0.0f, harness.masterL[i]) << "past-end sample " << i;
    }
}

TEST(CascadeProtocol, AuthoredDeadlineCutsResidentSourceAtExactFrame) {
    const std::vector<float> source(400, 0.5f);
    CascadeHarness harness(source);
    harness.ctx.authoredSourceDeadlineFrame = 100;

    const StageResult result = harness.RenderBlock();

    EXPECT_EQ(100u, result.framesProduced);
    EXPECT_EQ(SourcePhase::Done, result.phase);
    EXPECT_EQ(100u, harness.ctx.logicalSourceOutputFramesThisBlock);
    EXPECT_EQ(100u, harness.ctx.stagedSourceTerminalOffset);
    for (std::size_t frame = 0; frame < 100; ++frame) {
        EXPECT_FLOAT_EQ(0.5f, harness.masterL[frame]);
    }
    for (std::size_t frame = 100; frame < kBlock; ++frame) {
        EXPECT_FLOAT_EQ(0.0f, harness.masterL[frame]);
    }
}

TEST(CascadeProtocol, AuthoredDeadlineIsIndependentOfSourceOffset) {
    std::vector<float> source(400);
    for (std::size_t frame = 0; frame < source.size(); ++frame) {
        source[frame] = static_cast<float>(frame) / 400.0f;
    }
    CascadeHarness harness(source);
    harness.ctx.srcCursor = 50;
    harness.ctx.authoredSourceDeadlineFrame = 100;

    const StageResult result = harness.RenderBlock();

    EXPECT_EQ(100u, result.framesProduced);
    EXPECT_EQ(SourcePhase::Done, result.phase);
    EXPECT_FLOAT_EQ(source[50], harness.masterL[0]);
    EXPECT_FLOAT_EQ(source[149], harness.masterL[99]);
    EXPECT_FLOAT_EQ(0.0f, harness.masterL[100]);
}

TEST(CascadeProtocol, NaturalEndWinsEqualOrLongerAuthoredDeadline) {
    const std::vector<float> source(300, 0.5f);
    for (const std::uint64_t deadline : {300u, 500u}) {
        SCOPED_TRACE(deadline);
        CascadeHarness harness(source);
        harness.ctx.authoredSourceDeadlineFrame = deadline;

        const StageResult first = harness.RenderBlock();
        const StageResult second = harness.RenderBlock();

        EXPECT_EQ(kBlock, first.framesProduced);
        EXPECT_EQ(44u, second.framesProduced);
        EXPECT_EQ(SourcePhase::Done, second.phase);
        for (std::size_t frame = 44; frame < kBlock; ++frame) {
            EXPECT_FLOAT_EQ(0.0f, harness.masterL[frame]);
        }
    }
}

TEST(CascadeProtocol, AuthoredDeadlineDrainsFiniteTailAfterSourceBoundary) {
    const std::vector<float> source(400, 0.5f);
    CascadeHarness harness(source);
    harness.ctx.authoredSourceDeadlineFrame = 100;
    harness.ctx.fxSlots[0].process = AddOffsetProcess;
    harness.ctx.fxSlots[0].tailFrames = FixedTail300;

    const StageResult first = harness.RenderBlock();

    EXPECT_EQ(kBlock, first.framesProduced);
    EXPECT_EQ(SourcePhase::Draining, first.phase);
    EXPECT_EQ(100u, harness.ctx.logicalSourceOutputFramesThisBlock);
    EXPECT_EQ(144u, harness.ctx.tailRemaining);
    for (std::size_t frame = 0; frame < 100; ++frame) {
        EXPECT_FLOAT_EQ(0.5f + kTailOffset, harness.masterL[frame]);
    }
    for (std::size_t frame = 100; frame < kBlock; ++frame) {
        EXPECT_FLOAT_EQ(kTailOffset, harness.masterL[frame]);
    }
}

TEST(CascadeProtocol, AuthoredDeadlineBoundsInfiniteLoopAndAllPitchRatios) {
    const std::vector<float> source(64, 0.5f);
    for (const bool infinite : {false, true}) {
        for (const double ratio : {0.5, 1.0, 2.0}) {
            SCOPED_TRACE(infinite);
            SCOPED_TRACE(ratio);
            CascadeHarness harness(source, ratio);
            harness.ctx.loopStartFrame = 0;
            harness.ctx.loopEndFrame = source.size();
            harness.ctx.loopInfinite = infinite;
            harness.ctx.loopPassesRemaining = 3;
            harness.ctx.authoredSourceDeadlineFrame = 100;

            const StageResult result = harness.RenderBlock();
            const std::uint32_t expectedFrames =
                !infinite && ratio == 2.0 ? 96u : 100u;

            EXPECT_EQ(expectedFrames, result.framesProduced);
            EXPECT_EQ(SourcePhase::Done, result.phase);
            EXPECT_EQ(expectedFrames,
                      harness.ctx.logicalSourceOutputFramesThisBlock);
            EXPECT_EQ(expectedFrames,
                      harness.ctx.stagedSourceTerminalOffset);
        }
    }
}

TEST(CascadeProtocol, AuthoredDeadlineExpiresBeforeStreamPrebufferOpens) {
    const std::vector<float> source(400, 0.5f);
    CascadeHarness harness(source);
    std::vector<float> streamStorage(kBlock, 0.0f);
    float* streamPlanes[harken::kMaxChannels] = {};
    streamPlanes[0] = streamStorage.data();
    harken::StreamRing stream;
    stream.Wire(streamPlanes, kBlock);
    stream.ResetForUse(1);
    harness.ctx.streamRing = &stream;
    harness.ctx.streamGateOpen = false;
    harness.ctx.prebufferTargetFrames = kBlock;
    harness.ctx.authoredSourceDeadlineFrame = 100;

    const StageResult result = harness.RenderBlock();

    EXPECT_TRUE(harness.ctx.streamGateOpen);
    EXPECT_EQ(100u, result.framesProduced);
    EXPECT_EQ(SourcePhase::Done, result.phase);
    EXPECT_EQ(0u, harness.ctx.logicalSourceOutputFramesThisBlock);
    EXPECT_FALSE(harness.ctx.starvedThisBlock);
    EXPECT_EQ(100u, harness.ctx.stagedSourceTerminalOffset);
    EXPECT_TRUE(std::all_of(harness.masterL.begin(), harness.masterL.end(),
                            [](float sample) { return sample == 0.0f; }));
}

// Effect with a fixed tail that adds a DC offset to every frame it
// processes: the offset appearing on silence-fed frames proves the drain
// runs through the full cascade rather than around it.
void AddOffsetProcess(void* /*state*/, float* const* channels,
                      std::uint32_t channelCount, std::uint32_t offset,
                      std::uint32_t frames) {
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* p = channels[ch] + offset;
        for (std::uint32_t i = 0; i < frames; ++i) {
            p[i] += kTailOffset;
        }
    }
}

std::uint32_t FixedTail300(const void* /*state*/) {
    return 300;
}

TEST(CascadeProtocol, TailDrainFeedsSilenceThroughCascadeUntilDone) {
    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    CascadeHarness harness(sine);
    harness.ctx.fxSlots[0].process = AddOffsetProcess;
    harness.ctx.fxSlots[0].tailFrames = FixedTail300;
    harness.ctx.fxSlots[0].state = nullptr;

    // Block 1: pure source frames.
    const StageResult block1 = harness.RenderBlock();
    EXPECT_EQ(kBlock, block1.framesProduced);
    EXPECT_EQ(SourcePhase::Active, block1.phase);

    // Block 2: 44 source frames, then 212 silence-fed frames of the
    // 300-frame tail. The block is full and the voice is Draining.
    const StageResult block2 = harness.RenderBlock();
    EXPECT_EQ(kBlock, block2.framesProduced);
    EXPECT_EQ(SourcePhase::Draining, block2.phase);
    for (std::size_t i = 0; i < 44; ++i) {
        ASSERT_NEAR(sine[kBlock + i] + kTailOffset, harness.masterL[i], 1e-6f)
            << "source+offset sample " << i;
    }
    for (std::size_t i = 44; i < kBlock; ++i) {
        ASSERT_NEAR(kTailOffset, harness.masterL[i], 1e-6f)
            << "drain sample " << i;
    }

    // Block 3: the remaining 88 tail frames, then Done; the rest of the
    // block carries nothing.
    const StageResult block3 = harness.RenderBlock();
    EXPECT_EQ(88u, block3.framesProduced);
    EXPECT_EQ(SourcePhase::Done, block3.phase);
    HARKENTEST_ASSERT_NON_SILENT(harness.masterL);
    for (std::size_t i = 0; i < 88; ++i) {
        ASSERT_NEAR(kTailOffset, harness.masterL[i], 1e-6f)
            << "drain sample " << i;
    }
    for (std::size_t i = 88; i < kBlock; ++i) {
        ASSERT_EQ(0.0f, harness.masterL[i]) << "past-tail sample " << i;
    }
}

TEST(CascadeProtocol, LogicalSourceProgressExcludesFeedAheadAndTailFrames) {
    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.8f);
    CascadeHarness harness(sine);
    harness.ctx.fxSlots[0].process = AddOffsetProcess;
    harness.ctx.fxSlots[0].tailFrames = FixedTail300;

    const StageResult block1 = harness.RenderBlock();
    ASSERT_EQ(kBlock, block1.framesProduced);
    EXPECT_EQ(kBlock,
              harness.ctx.logicalSourceOutputFramesThisBlock);

    const StageResult block2 = harness.RenderBlock();
    ASSERT_EQ(kBlock, block2.framesProduced);
    EXPECT_EQ(44u, harness.ctx.logicalSourceOutputFramesThisBlock);
    EXPECT_EQ(300u, harness.ctx.srcCursor);

    const StageResult block3 = harness.RenderBlock();
    ASSERT_EQ(88u, block3.framesProduced);
    EXPECT_EQ(0u, harness.ctx.logicalSourceOutputFramesThisBlock);
}

TEST(CascadeProtocol, ArenaShortfallPublishesZeroLogicalSourceProgress) {
    const std::vector<float> sine =
        harkentest::MakeSine(400, 1000.0, 48000.0, 0.8f);
    CascadeHarness harness(sine);
    harness.ctx.logicalSourceOutputFramesThisBlock = 91;
    BlockArena emptyArena{0};
    std::vector<float> left(kBlock);
    std::vector<float> right(kBlock);
    float* master[harken::kOutputChannels] = {left.data(), right.data()};

    const StageResult result =
        RenderVoiceBlock(harness.ctx, emptyArena, master, kBlock, 1,
                         VN_CHANNEL_LAYOUT_MONO);

    EXPECT_EQ(0u, result.framesProduced);
    EXPECT_EQ(0u, harness.ctx.logicalSourceOutputFramesThisBlock);
    EXPECT_EQ(0u, harness.ctx.srcCursor);
}

TEST(CascadeProtocol, TimelineEnvelopeEndpointsN2N3AndCrossQuantum) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);
    for (const std::uint64_t length : {2ull, 3ull}) {
        CascadeHarness harness(dc);
        harness.ctx.timelineEvents.quantumIndex = 0;
        harness.ctx.timelineEnvelopes[0] = {
            true, false, 0, length};
        const StageResult result = harness.RenderBlock();
        ASSERT_EQ(result.phase, SourcePhase::Active);
        EXPECT_EQ(harness.masterL[0], 0.0f);
        EXPECT_EQ(harness.masterL[length - 1], 1.0f);
    }

    CascadeHarness crossQuantum(dc);
    crossQuantum.ctx.timelineEvents.quantumIndex = 0;
    crossQuantum.ctx.timelineEnvelopes[0] = {
        true, false, 255, 258};
    crossQuantum.RenderBlock();
    EXPECT_EQ(crossQuantum.masterL[255], 0.0f);
    crossQuantum.ctx.timelineEvents.quantumIndex = 1;
    crossQuantum.RenderBlock();
    EXPECT_EQ(crossQuantum.masterL[0], 0.5f);
    EXPECT_EQ(crossQuantum.masterL[1], 1.0f);
}

TEST(CascadeProtocol, FutureTimelineEnvelopeDoesNotAffectEarlierSamples) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);
    for (const std::uint16_t eventOffset : {std::uint16_t{1},
                                           std::uint16_t{kBlock - 1}}) {
        CascadeHarness harness(dc);
        harness.ctx.timelineEvents.quantumIndex = 0;
        harness.ctx.timelineEvents.eventCount = 1;
        harken::VoiceTimelineEvent& event =
            harness.ctx.timelineEvents.events[0];
        event.kind = harken::TimelineEventKind::Envelope;
        event.offset = eventOffset;
        event.frame = eventOffset;
        event.endFrame = eventOffset + 3;
        event.envelopeDirection =
            harken::TimelineEnvelopeDirection::FadeIn;

        harness.RenderBlock();
        for (std::uint32_t frame = 0; frame < eventOffset; ++frame) {
            EXPECT_EQ(harness.masterL[frame], 1.0f)
                << "offset=" << eventOffset << " frame=" << frame;
        }
        EXPECT_EQ(harness.masterL[eventOffset], 0.0f);
    }
}

TEST(CascadeProtocol,
     VariableWeightTimelineEventStartsAtItsExactSampleOffset) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);
    CascadeHarness harness(dc);
    harness.ctx.timelineEvents.quantumIndex = 0;
    harness.ctx.timelineEvents.eventCount = 1;
    harken::VoiceTimelineEvent& event =
        harness.ctx.timelineEvents.events[0];
    event.kind = harken::TimelineEventKind::VariableWeight;
    event.offset = 4;
    event.variableWeight.startFrame = 4;
    event.variableWeight.endFrame = 8;
    event.variableWeight.startWeight = 0.0f;
    event.variableWeight.endWeight = 1.0f;
    event.variableWeight.segmentGeneration = 1;
    event.variableWeight.decision = {3, 5};
    event.variableWeight.bank = {7, 11};

    harness.RenderBlock();
    for (std::uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(1.0f, harness.masterL[frame]);
    }
    EXPECT_FLOAT_EQ(0.0f, harness.masterL[4]);
    EXPECT_NEAR(1.0f / 3.0f, harness.masterL[5], 1e-6f);
    EXPECT_NEAR(2.0f / 3.0f, harness.masterL[6], 1e-6f);
    EXPECT_FLOAT_EQ(1.0f, harness.masterL[7]);
    EXPECT_FLOAT_EQ(1.0f, harness.masterL[kBlock - 1]);
}

TEST(CascadeProtocol, FutureTimelineStopDoesNotAffectEarlierSamples) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);
    for (const std::uint16_t eventOffset : {std::uint16_t{1},
                                           std::uint16_t{kBlock - 1}}) {
        CascadeHarness harness(dc);
        harness.ctx.engineSampleRate = 48000;
        harness.ctx.timelineEvents.quantumIndex = 0;
        harness.ctx.timelineEvents.eventCount = 1;
        harken::VoiceTimelineEvent& event =
            harness.ctx.timelineEvents.events[0];
        event.kind = harken::TimelineEventKind::Stop;
        event.offset = eventOffset;

        harness.RenderBlock();
        for (std::uint32_t frame = 0; frame < eventOffset; ++frame) {
            EXPECT_EQ(harness.masterL[frame], 1.0f)
                << "offset=" << eventOffset << " frame=" << frame;
        }
        EXPECT_LT(harness.masterL[eventOffset], 1.0f);
    }
}

TEST(CascadeProtocol, TimelineFadeOutEndpointsM2M3) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);
    for (const std::uint64_t length : {2ull, 3ull}) {
        CascadeHarness harness(dc);
        harness.ctx.timelineEvents.quantumIndex = 0;
        harness.ctx.timelineEnvelopes[1] = {
            true, true, 0, length};
        harness.RenderBlock();
        EXPECT_EQ(harness.masterL[0], 1.0f);
        EXPECT_EQ(harness.masterL[length - 1], 0.0f);
    }
}

TEST(CascadeProtocol, OneFrameTimelineEnvelopeUsesTerminalGain) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);

    CascadeHarness fadeIn(dc);
    fadeIn.ctx.timelineEvents.quantumIndex = 0;
    fadeIn.ctx.timelineEnvelopes[0] = {true, false, 0, 1};
    fadeIn.RenderBlock();
    EXPECT_EQ(1.0f, fadeIn.masterL[0]);
    EXPECT_FALSE(fadeIn.ctx.timelineEnvelopes[0].active);

    CascadeHarness fadeOut(dc);
    fadeOut.ctx.timelineEvents.quantumIndex = 0;
    fadeOut.ctx.timelineEnvelopes[1] = {true, true, 0, 1};
    fadeOut.RenderBlock();
    EXPECT_EQ(0.0f, fadeOut.masterL[0]);
    EXPECT_TRUE(fadeOut.ctx.timelineFadeOutCoveredSample);
    EXPECT_FALSE(fadeOut.ctx.timelineEnvelopes[1].active);
}

TEST(CascadeProtocol, FadeStopRampsToZeroTruncatesAndFinishes) {
    const std::vector<float> dc = harkentest::MakeDC(2000, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(dc);

    CascadeHarness harness(dc);

    const StageResult block1 = harness.RenderBlock();
    EXPECT_EQ(kBlock, block1.framesProduced);
    EXPECT_EQ(SourcePhase::Active, block1.phase);

    // Arm a 100-frame fade the way Vn_Stop does.
    harness.ctx.fading = true;
    harness.ctx.fadeTotalFrames = 100;
    harness.ctx.fadeDoneFrames = 0;

    const StageResult block2 = harness.RenderBlock();
    EXPECT_EQ(100u, block2.framesProduced);
    EXPECT_EQ(SourcePhase::Done, block2.phase);
    HARKENTEST_ASSERT_NON_SILENT(harness.masterL);

    // Linear ramp to zero: sample i carries (total - (i+1)) / total.
    for (std::size_t i = 0; i < 100; ++i) {
        const float expected =
            static_cast<float>(100 - (i + 1)) / 100.0f;
        ASSERT_NEAR(expected, harness.masterL[i], 1e-6f)
            << "fade sample " << i;
    }
    // Truncated: nothing renders past the ramp's zero.
    for (std::size_t i = 100; i < kBlock; ++i) {
        ASSERT_EQ(0.0f, harness.masterL[i]) << "post-fade sample " << i;
    }
}

TEST(CascadeProtocol, VnStopFadeRampsThenReleasesSlot) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(48000, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);

    // 2 ms at 48 kHz = 96 fade frames.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 2));
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);

    for (std::size_t i = 0; i < 96; ++i) {
        const float expected = static_cast<float>(96 - (i + 1)) / 96.0f;
        ASSERT_NEAR(expected, out.left[kBlock + i], 1e-6f)
            << "fade sample " << i;
    }
    for (std::size_t i = 96; i < kBlock; ++i) {
        ASSERT_EQ(0.0f, out.left[kBlock + i]) << "post-fade sample " << i;
    }

    // The voice ended: its slot is released and the old handle is stale.
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(handle, 0));

#ifdef _DEBUG
    EXPECT_TRUE(harken::memguard::IsArmed());
#endif
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- high-ratio voice lifecycle (pitch decimation) ---
//
// A combined ratio far above unity makes the resampler read head jump past
// its write head, which the FreeFrames underflow turns into a permanent
// stall: the stage produces nothing further. Because the fade-stop ramp only
// advances on blocks that produce frames, a stalled voice can never be faded
// out — it becomes a zombie the host cannot stop, and a short source can
// never reach its natural end. These cases drive the whole engine at a ratio
// (60:1) that overshoots the history pad but stays within the pitch contract
// ceiling, and pin the post-fix lifecycle. Every pump loop is bounded, so a
// stall shows up as a failed release inside a finite block budget.

struct NotificationLog {
    std::vector<VnNotification> notifications;
    bool Saw(std::uint32_t type) const {
        for (const VnNotification& n : notifications) {
            if (n.type == type) {
                return true;
            }
        }
        return false;
    }
};

void RecordNotification(const VnNotification* notification, void* userData) {
    static_cast<NotificationLog*>(userData)->notifications.push_back(
        *notification);
}

VnConfig ConfigWithLog(NotificationLog* log) {
    VnConfig config = harkentest::MakeNullConfig(4, kBlock, 48000);
    config.onNotification = &RecordNotification;
    config.notificationUserData = log;
    return config;
}

// 60:1 sits within the pitch contract's (0, 64] range, far above the stall
// threshold, and is not a divisor of the feed chunk, so the read head
// overshoots the history pad — the ratio that triggers the stall.
constexpr float kDecimationPitch = 60.0f;

TEST(CascadeProtocol, HighRatioFadeStopReleasesStalledVoice) {
    NotificationLog log;
    harkentest::EngineSession session(ConfigWithLog(&log));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Long enough that the fade ramp (2400 frames) completes before the
    // source itself ends once the stage produces normally again.
    const std::vector<float> dc = harkentest::MakeDC(300000, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1), 0.0f, kDecimationPitch);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // A couple of blocks put the voice into the high-ratio starve state.
    session.Engine()->nullBackend.Pump(2);

    // 50 ms at 48 kHz = 2400 fade frames.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 50));

    constexpr int kMaxBlocks = 64;
    bool released = false;
    for (int block = 0; block < kMaxBlocks; ++block) {
        session.Engine()->nullBackend.Pump(1);
        if (session.Engine()->voices.Resolve(harkentest::RenderHandle(
                session.Engine(), handle)) == nullptr) {
            released = true;
            break;
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_TRUE(released)
        << "faded high-ratio voice never released within " << kMaxBlocks
        << " blocks (activeVoiceCount="
        << session.Engine()->voices.ActiveCount() << ")";
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_TRUE(log.Saw(
        static_cast<std::uint32_t>(VN_NOTIFY_VOICE_STOPPED_BY_FADE)))
        << "fade completion did not emit stopped-by-fade";
}

TEST(CascadeProtocol, StarvedFadeNearUint32LimitCompletesWithoutWrap) {
    const std::vector<float> dc = harkentest::MakeDC(kBlock, 1.0f);
    CascadeHarness harness(dc);

    std::vector<float> streamStorage(kBlock, 0.0f);
    float* streamPlanes[harken::kMaxChannels] = {};
    streamPlanes[0] = streamStorage.data();
    harken::StreamRing stream;
    stream.Wire(streamPlanes, kBlock);
    stream.ResetForUse(1);

    harness.ctx.streamRing = &stream;
    harness.ctx.streamGateOpen = true;
    harness.ctx.fading = true;
    harness.ctx.fadeTotalFrames =
        std::numeric_limits<std::uint32_t>::max();
    harness.ctx.fadeDoneFrames = harness.ctx.fadeTotalFrames - 5;

    const StageResult result = harness.RenderBlock();

    EXPECT_EQ(0u, result.framesProduced);
    EXPECT_EQ(SourcePhase::Done, result.phase);
    EXPECT_EQ(harness.ctx.fadeTotalFrames, harness.ctx.fadeDoneFrames);
}

TEST(CascadeProtocol, HighRatioImmediateStopReleasesViaEscapeValve) {
    NotificationLog log;
    harkentest::EngineSession session(ConfigWithLog(&log));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(300000, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1), 0.0f, kDecimationPitch);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    session.Engine()->nullBackend.Pump(2);

    // An immediate cut releases the slot directly, bypassing the
    // produce-gated fade path — the escape valve holds even while the stage
    // is stalled.
    ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
                           session.Engine(), handle)));
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_TRUE(log.Saw(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED)))
        << "immediate stop did not emit voice-ended";
}

TEST(CascadeProtocol, HighRatioShortSourceEndsNaturally) {
    NotificationLog log;
    harkentest::EngineSession session(ConfigWithLog(&log));
    ASSERT_EQ(VN_OK, session.InitResult());

    // 2048 frames at 60:1 decimate to ~34 output frames — the whole voice
    // fits inside the first block once the stage no longer stalls.
    const std::vector<float> dc = harkentest::MakeDC(2048, 1.0f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1), 0.0f, kDecimationPitch);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    constexpr int kMaxBlocks = 64;
    bool ended = false;
    for (int block = 0; block < kMaxBlocks; ++block) {
        session.Engine()->nullBackend.Pump(1);
        if (session.Engine()->voices.Resolve(harkentest::RenderHandle(
                session.Engine(), handle)) == nullptr) {
            ended = true;
            break;
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_TRUE(ended)
        << "short high-ratio voice never ended within " << kMaxBlocks
        << " blocks (activeVoiceCount="
        << session.Engine()->voices.ActiveCount() << ")";
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_TRUE(log.Saw(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED)))
        << "natural end did not emit voice-ended";
}

} // namespace
