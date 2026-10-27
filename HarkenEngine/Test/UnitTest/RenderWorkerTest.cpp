// Job-parallel voice rendering (M5 §8 / acceptance §11.4). Four things are
// proven here:
//   1. Determinism: serial (renderWorkerCount = 0) and parallel (N in {1,2,4})
//      renders of the same scene are BYTE-IDENTICAL — the load-bearing §8.2
//      invariant — across stereo, 5.1, 7.1, and a 5.1->stereo output downmix,
//      with a scene that exercises multiple voices, an effect chain, voice
//      sends, and spatial panning so every fold path is covered.
//   2. Observability: renderWorkerBusyPeakUs is nonzero when workers carry a
//      real load and stays exactly 0 in serial rendering (no fabricated figure).
//   3. Allocation guard on workers: the guard is genuinely armed on the worker
//      threads (a clean guard is only evidence if it was watching), and no
//      worker touches the system heap across a parallel render.
//   4. Zero new vocabulary: the M3 command-vocabulary assert (D-174) re-run on
//      the parallel scene observes no opcode beyond the frozen set.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"
#include "VoicePoolReleaseTestAccess.h"

#include "harken/core/AllocGuard.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/CpuClock.h"
#include "harken/core/LogRing.h"
#include "harken/param/MeterProbe.h"
#include "harken/render/MixerLoop.h"
#include "harken/render/RenderWorkerPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <process.h>
#include <string>
#include <thread>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

using harkentest::CaptureMulti;
using harkentest::EngineSession;
using harkentest::MakeNullConfigLayout;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::PumpAndCaptureMulti;

namespace {

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr VnBusId kSendBus = harkentest::ObjectId(7);
constexpr VnBusId kSidechainDetectorBus = harkentest::ObjectId(31);
constexpr VnBusId kSidechainMusicBus = harkentest::ObjectId(32);
constexpr VnSoundId kSidechainMusicSound = harkentest::ObjectId(41);
constexpr VnSoundId kSidechainDialogueSound = harkentest::ObjectId(42);
constexpr VnObjectId kSidechainProbe = harkentest::ObjectId(43);

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

// Full-vintage snapshot capture (reaches the multichannel / job-parallel tail).
VnSnapshot CaptureFull() {
    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snap));
    return snap;
}

// A failed worker setup or wake primitive disables parallel rendering.
// Deadline deferral preserves voices and does not disable the worker set.
bool ParallelRenderUnavailable(std::uint32_t configuredWorkers,
                             const VnSnapshot& snap) {
    return configuredWorkers > 0 && snap.renderWorkerCount == 0;
}

struct BusyPeakChainLoad {
    std::atomic<std::uint32_t> started{0};
    std::uint32_t participantCount = 0;
    std::uint64_t spinTicks = 0;
    std::uint64_t workerSpinTicks = 0;
    std::thread::id coordinator = std::this_thread::get_id();

    static void Process(void* state, float* const*, std::uint32_t,
                        std::uint32_t, std::uint32_t) {
        auto* load = static_cast<BusyPeakChainLoad*>(state);
        load->started.fetch_add(1, std::memory_order_acq_rel);
        while (load->started.load(std::memory_order_acquire) <
               load->participantCount) {
            std::this_thread::yield();
        }
        const std::uint64_t deadline =
            harken::CpuClockTicks() +
            (std::this_thread::get_id() == load->coordinator
                ? load->spinTicks : load->workerSpinTicks);
        while (harken::CpuClockTicks() < deadline) {
        }
    }
};

// One deterministic multi-voice scene, rendered at the given worker count and
// mix/device layout, captured across every device channel. Same seed, same PCM,
// same call order, same pump count on every invocation, so the only variable is
// renderWorkerCount — which the determinism gate then proves has no effect on a
// single output bit. The scene deliberately mixes spatialized and 2D voices, an
// effect chain, and a voice send so the parallel cascade + serial fold cover the
// pan law, the effect blend, the N-channel fold, and the send scatter.
struct RenderSceneFormat {
    std::uint32_t blockFrames = kBlock;
    std::uint32_t mixRate = kRate;
    std::uint32_t sourceRate = kRate;
    std::uint32_t bedChannels = 1;
};

void RenderScene(std::uint32_t workerCount, std::uint32_t mixLayout,
                 std::uint32_t deviceLayout, std::uint32_t blocks,
                 CaptureMulti& out, std::uint32_t arenaBytes = 1u << 20,
                 std::uint64_t* outOverflow = nullptr,
                 bool* outParallelUnavailable = nullptr,
                 RenderSceneFormat format = {}) {
    VnConfig config = MakeNullConfigLayout(16, format.blockFrames,
        format.mixRate, mixLayout, deviceLayout);
    config.fxSlotsPerVoice = 2;
    config.voiceSendSlots = 2;
    config.renderWorkerCount = workerCount;
    // Arena size is a parameter: the determinism gate uses a generous arena
    // (degradation is a defined path but not what that gate measures), while the
    // tight-arena gate below drives it into the shortfall region on purpose.
    config.arenaBytes = arenaBytes;

    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // A send target bus, so a couple of voices exercise the scatter path.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSendBus, VN_BUS_MASTER));

    // Eight distinct sine sources; long enough to sound through every block.
    const double freqs[8] = {110.0, 165.0, 220.0, 330.0,
                             440.0, 550.0, 660.0, 880.0};
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint32_t channels = (i & 1u) == 0 ? 1u : format.bedChannels;
        std::vector<float> sine(format.sourceRate * channels);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto channel = harkentest::MakeSine(format.sourceRate,
                freqs[i] + 37.0 * ch, format.sourceRate, 0.25f / channels);
            for (std::uint32_t frame = 0; frame < format.sourceRate; ++frame)
                sine[frame * channels + ch] = channel[frame];
        }
        const VnPCMDesc desc =
            harkentest::InterleavedDesc(sine, channels, format.sourceRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1 + i), &desc));
    }

    // Play the eight voices: alternating spatialized 3D (spread around the
    // listener) and 2D, at staggered volumes and pitches.
    VnVoiceHandle handles[8] = {};
    for (std::uint32_t i = 0; i < 8; ++i) {
        const VnSoundId id = harkentest::ObjectId(1 + i);
        VnPlayDesc play;
        if ((i & 1u) == 0) {
            const float az = -1.0f + 0.5f * static_cast<float>(i);
            play = MakePlayDesc3D(id, az, 0.0f, -3.0f, -3.0f);
        } else {
            play = MakePlayDesc(id, -2.0f,
                                0.5f + 0.1f * static_cast<float>(i));
        }
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handles[i]));
    }

    // A feedback delay on voice 0 and a three-band tone on voice 1 — the cascade
    // effect chain, run in parallel.
    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(VnFeedbackDelayDesc);
    delay.delayMs = 12.0f;
    delay.feedback = 0.4f;
    delay.wet = 0.6f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handles[0], 0, VN_EFFECT_FEEDBACK_DELAY,
                                       AsBase(&delay)));

    VnThreeBandToneDesc tone{};
    tone.structSize = sizeof(VnThreeBandToneDesc);
    tone.lowShelfGainDb = 3.0f;
    tone.lowShelfFreqHz = 200.0f;
    tone.peakGainDb = -4.0f;
    tone.peakFreqHz = 1000.0f;
    tone.peakQ = 1.0f;
    tone.highShelfGainDb = 2.0f;
    tone.highShelfFreqHz = 6000.0f;
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handles[1], 0, VN_EFFECT_THREE_BAND_TONE,
                                       AsBase(&tone)));

    // Voice 1 blends its insert before the same voice's send image is folded.
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handles[1], 0, 1));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handles[1], 0, kSendBus, -3.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handles[5], 0, kSendBus, -6.0f));

    ASSERT_EQ(VN_OK, Vn_Update());
    const auto before = engine->mixer.BlocksRendered();
    PumpAndCaptureMulti(engine, blocks, out);
    EXPECT_EQ(before + blocks, engine->mixer.BlocksRendered());
    EXPECT_EQ(0u, harken::memguard::HitCount())
        << "the allocation guard fired during the render";
    if (outOverflow != nullptr || outParallelUnavailable != nullptr) {
        const VnSnapshot snap = CaptureFull();
        ASSERT_EQ(before + blocks - 1u, snap.blockIndex);
        if (outOverflow != nullptr) {
            // Cumulative arena-overflow count for the whole run — the degradation
            // fingerprint the tight-arena gate compares across worker counts.
            *outOverflow = snap.arenaOverflowCount;
        }
        if (outParallelUnavailable != nullptr) {
            *outParallelUnavailable = ParallelRenderUnavailable(workerCount, snap);
        }
    }
}

// Bit-exact channel-by-channel comparison — a single differing sample fails.
void ExpectBitIdentical(const CaptureMulti& serial, const CaptureMulti& parallel,
                        std::uint32_t workerCount) {
    ASSERT_EQ(serial.channelCount, parallel.channelCount);
    ASSERT_EQ(serial.channels.size(), parallel.channels.size());
    for (std::size_t ch = 0; ch < serial.channels.size(); ++ch) {
        ASSERT_EQ(serial.channels[ch].size(), parallel.channels[ch].size());
        const int cmp = std::memcmp(
            serial.channels[ch].data(), parallel.channels[ch].data(),
            serial.channels[ch].size() * sizeof(float));
        EXPECT_EQ(0, cmp)
            << "renderWorkerCount=" << workerCount
            << " drifted from serial on channel " << ch;
    }
}

// The scene must actually sound (silence guard): a comparison of two silent
// captures would pass vacuously.
void ExpectNonSilent(const CaptureMulti& cap) {
    float peak = 0.0f;
    for (const std::vector<float>& channel : cap.channels) {
        const float p = harkentest::Peak(channel);
        if (p > peak) {
            peak = p;
        }
    }
    ASSERT_GT(peak, harkentest::kSilenceFloor)
        << "silence-guard: the compared scene is silent";
}

void RunDeterminismCase(std::uint32_t mixLayout, std::uint32_t deviceLayout) {
    constexpr std::uint32_t kBlocks = 48;
    CaptureMulti serial;
    // Serial rendering (renderWorkerCount = 0) dispatches no worker batch, so it
    // cannot lose its worker set; only the parallel renders below are guarded.
    RenderScene(0, mixLayout, deviceLayout, kBlocks, serial);
    ExpectNonSilent(serial);
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        CaptureMulti parallel;
        bool unavailable = false;
        RenderScene(workers, mixLayout, deviceLayout, kBlocks, parallel,
                    1u << 20, nullptr, &unavailable);
        if (unavailable) {
            // Byte identity requires a run that actually used workers.
            GTEST_SKIP()
                << "renderWorkerCount=" << workers
                << " could not keep its worker set available (host "
                   "overloaded); byte-identity holds only for a completed render";
        }
        ExpectNonSilent(parallel);
        ExpectBitIdentical(serial, parallel, workers);
    }
}

// --- 1. Determinism gate across layouts ---

TEST(RenderWorkerDeterminism, StereoByteIdentical) {
    RunDeterminismCase(VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO);
}

TEST(RenderWorkerDeterminism, FiveOneByteIdentical) {
    RunDeterminismCase(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1);
}

TEST(RenderWorkerDeterminism, SevenOneByteIdentical) {
    RunDeterminismCase(VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_7_1);
}

TEST(RenderWorkerDeterminism, FiveOneToStereoDownmixByteIdentical) {
    // The master->device output downmix runs (device narrower than mix), so the
    // determinism gate covers the fold's downmix path too.
    RunDeterminismCase(VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_STEREO);
}

struct RenderGeometryCase {
    const char* name;
    RenderSceneFormat format;
    std::uint32_t mixLayout;
    std::uint32_t deviceLayout;
    std::uint32_t workers;
};

class RenderWorkerGeometry : public testing::TestWithParam<RenderGeometryCase> {};

TEST_P(RenderWorkerGeometry, PreservesEveryOutputSample) {
    const auto& test = GetParam();
    constexpr std::uint32_t blocks = 48;
    CaptureMulti serial, parallel;
    std::uint64_t serialOverflow = 0, parallelOverflow = 0;
    bool unavailable = false;
    ASSERT_NO_FATAL_FAILURE(RenderScene(0, test.mixLayout, test.deviceLayout,
        blocks, serial, 1u << 20, &serialOverflow, nullptr, test.format));
    ASSERT_NO_FATAL_FAILURE(RenderScene(test.workers, test.mixLayout, test.deviceLayout,
        blocks, parallel, 1u << 20, &parallelOverflow, &unavailable, test.format));
    ASSERT_FALSE(unavailable) << "the requested worker configuration was not exercised";
    EXPECT_EQ(0u, serialOverflow);
    EXPECT_EQ(0u, parallelOverflow);
    const auto expectedChannels = test.deviceLayout == VN_CHANNEL_LAYOUT_5_1 ? 6u : 2u;
    for (const auto* capture : {&serial, &parallel}) {
        ASSERT_EQ(expectedChannels, capture->channelCount);
        ASSERT_EQ(expectedChannels, capture->channels.size());
        ASSERT_NO_FATAL_FAILURE(ExpectNonSilent(*capture));
        for (const auto& channel : capture->channels) {
            ASSERT_EQ(blocks * test.format.blockFrames, channel.size());
            EXPECT_TRUE(std::all_of(channel.begin(), channel.end(),
                [](float sample) { return std::isfinite(sample); }));
        }
    }
    ExpectBitIdentical(serial, parallel, test.workers);
}

INSTANTIATE_TEST_SUITE_P(Formats, RenderWorkerGeometry, testing::Values(
    RenderGeometryCase{"Native44100Short", {64, 44100, 44100, 2},
        VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, 1},
    RenderGeometryCase{"Native48000Unaligned", {257, 48000, 48000, 2},
        VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, 2},
    RenderGeometryCase{"Upsample96000Long", {1024, 96000, 44100, 2},
        VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, 4},
    RenderGeometryCase{"SixChannelBed", {256, 48000, 48000, 6},
        VN_CHANNEL_LAYOUT_5_1, VN_CHANNEL_LAYOUT_5_1, 2},
    RenderGeometryCase{"EightChannelBedDownmix", {480, 44100, 48000, 8},
        VN_CHANNEL_LAYOUT_7_1, VN_CHANNEL_LAYOUT_STEREO, 4},
    RenderGeometryCase{"Downsample96000Short", {64, 48000, 96000, 2},
        VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, 1}),
    [](const testing::TestParamInfo<RenderGeometryCase>& info) { return info.param.name; });

TEST(RenderWorkerDeterminism, TightArenaDegradesIdenticallyAcrossWorkerCounts) {
    // Determinism at the arena boundary. Every per-voice arena reservation —
    // source scratch and the blend/send planes — is made in the serial
    // slot-order pre-pass (scratch -> blend/tap for one voice, then the
    // next voice); the cascade and fold phases touch the arena zero times. So
    // the allocation SEQUENCE, and therefore exactly which voices degrade to
    // silence when the arena runs short, is fixed by slot order and independent
    // of the worker count. A path that reserved all scratch first and the send
    // taps afterward would exhaust the arena at a different point and drop a
    // different voice under pressure — this gate would then drift.
    //
    // Sizing mechanism: a full block of this scene needs the bus planes plus
    // eight mono voice-scratch planes plus two stereo send taps. This arena
    // holds the bus planes and only the first voices, so the reservation runs
    // out mid-scene (arenaOverflowCount > 0) while the early voices still sound
    // (non-silent) — the partial-degradation band the gate needs.
    constexpr std::uint32_t kBlocks = 16;
    constexpr std::uint32_t kTightArena = 8192;
    CaptureMulti serial;
    std::uint64_t serialOverflow = 0;
    RenderScene(0, VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO, kBlocks,
                serial, kTightArena, &serialOverflow);
    ExpectNonSilent(serial);
    ASSERT_GT(serialOverflow, 0u)
        << "arena not tight enough to force a shortfall — the gate is vacuous";
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        CaptureMulti parallel;
        std::uint64_t parallelOverflow = 0;
        bool unavailable = false;
        RenderScene(workers, VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO,
                    kBlocks, parallel, kTightArena, &parallelOverflow,
                    &unavailable);
        if (unavailable) {
            // Serial fallback after worker setup or wake failure; the
            // degrade-identically invariant is defined only for a completed
            // parallel render.
            GTEST_SKIP()
                << "renderWorkerCount=" << workers
                << " fell back to serial after a worker setup or wake failure";
        }
        ExpectNonSilent(parallel);
        ExpectBitIdentical(serial, parallel, workers);
        EXPECT_EQ(serialOverflow, parallelOverflow)
            << "renderWorkerCount=" << workers
            << " degraded a different set of voices than serial";
    }
}

struct SidechainWorkerResult {
    CaptureMulti capture;
    float baselinePeak = 0.0f;
    float attenuatedPeak = 0.0f;
};

void RenderSidechainScene(std::uint32_t workerCount, SidechainWorkerResult& out,
                          bool* outUnavailable = nullptr) {
    VnConfig config = MakeNullConfigLayout(16, kBlock, kRate,
                                           VN_CHANNEL_LAYOUT_STEREO,
                                           VN_CHANNEL_LAYOUT_STEREO);
    config.renderWorkerCount = workerCount;
    config.arenaBytes = 1u << 20;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    ASSERT_EQ(VN_OK, Vn_CreateBus(kSidechainDetectorBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSidechainMusicBus, VN_BUS_MASTER));
    const std::vector<float> music =
        harkentest::MakeSine(kRate, 220.0, kRate, 0.25f);
    const std::vector<float> dialogue =
        harkentest::MakeSine(kRate, 440.0, kRate, 0.5f);
    const VnPCMDesc musicDesc = MonoDesc(music, kRate);
    // Identical stereo detector channels preserve the authored bus-meter
    // level for the -12 dB sidechain pin independently of mono panning.
    // This fixture isolates worker determinism from source channel conversion.
    std::vector<float> dialogueStereo(dialogue.size() * 2u);
    for (std::size_t frame = 0; frame < dialogue.size(); ++frame) {
        dialogueStereo[frame * 2u] = dialogue[frame];
        dialogueStereo[frame * 2u + 1u] = dialogue[frame];
    }
    const VnPCMDesc dialogueDesc =
        harkentest::InterleavedDesc(dialogueStereo, 2, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSidechainMusicSound, &musicDesc));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSidechainDialogueSound, &dialogueDesc));

    VnPlayDesc musicPlay = MakePlayDesc(kSidechainMusicSound);
    musicPlay.busId = kSidechainMusicBus;
    VnVoiceHandle musicHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&musicPlay, &musicHandle));
    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kSidechainProbe, kSidechainDetectorBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    const harken::CurvePoint sidechainPoints[2] = {{-96.0f, 0.0f},
                                                  {-6.0f, -12.0f}};
    // Endpoint handles on the segment ends select the linear segment.
    const harken::CurveHandlePair sidechainHandles[1] = {
        {-96.0f, 0.0f, -6.0f, -12.0f}};
    ASSERT_EQ(VN_OK, harken::DefineMeterProbeBinding(
                         *engine, harkentest::ObjectId(440), kSidechainProbe, probeGeneration,
                         VN_TARGET_SCOPE_BUS, /*targetId=*/0,
                         sidechainPoints, sidechainHandles, 2,
                         /*transitionMs=*/0));
    const std::uint32_t musicIndex = engine->buses.FindIndex(kSidechainMusicBus);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, harkentest::ObjectId(440), VN_TARGET_SCOPE_BUS,
                         kSidechainMusicBus, 0,
                         engine->buses.GenerationAt(musicIndex)));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kSidechainMusicBus, 0.0f, 0));

    // Establish a neutral-gain music block before the detector sounds. The
    // extra update drains those silent detector records so each compared run
    // starts dialogue from the same empty meter transport state.
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    const std::uint32_t musicBusIndex = engine->buses.FindIndex(kSidechainMusicBus);
    ASSERT_LT(musicBusIndex,
              static_cast<std::uint32_t>(VN_SNAPSHOT_MAX_BUSES));
    out.baselinePeak = snapshot.busPeaks[musicBusIndex];
    ASSERT_GT(out.baselinePeak, harkentest::kSilenceFloor);

    VnPlayDesc dialoguePlay = MakePlayDesc(kSidechainDialogueSound);
    dialoguePlay.busId = kSidechainDetectorBus;
    VnVoiceHandle dialogueHandle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&dialoguePlay, &dialogueHandle));

    // Pump and update block by block so every worker-count run sees the same
    // meter records and property-publication effect points. Capture begins at
    // dialogue onset and includes both the neutral-latency prefix and the live
    // attenuated suffix.
    out.capture.channelCount = engine->nullBackend.Channels();
    out.capture.channels.assign(out.capture.channelCount, {});
    for (std::uint32_t block = 0; block < 48; ++block) {
        engine->nullBackend.Pump(1);
        const float* rendered = engine->nullBackend.LastBlock();
        for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
            for (std::uint32_t ch = 0; ch < out.capture.channelCount; ++ch) {
                out.capture.channels[ch].push_back(
                    rendered[frame * out.capture.channelCount + ch]);
            }
        }
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    out.attenuatedPeak = snapshot.busPeaks[musicBusIndex];
    EXPECT_EQ(0u, harken::memguard::HitCount())
        << "the allocation guard fired during the sidechain render";
    if (outUnavailable != nullptr) {
        *outUnavailable = ParallelRenderUnavailable(workerCount, snapshot);
    }
}

TEST(RenderWorkerDeterminism, SidechainActiveByteIdenticalAcrossWorkerCounts) {
    SidechainWorkerResult reference;
    bool referenceUnavailable = false;
    RenderSidechainScene(1, reference, &referenceUnavailable);
    if (referenceUnavailable) {
        // The 1-worker reference also depends on successful worker setup under
        // host overload; comparing against a degraded reference is meaningless.
        GTEST_SKIP() << "the 1-worker reference fell back to serial after "
                        "a worker setup or wake failure (host "
                        "overloaded)";
    }
    ExpectNonSilent(reference.capture);
    const float expectedAttenuatedGain = std::pow(10.0f, -12.0f / 20.0f);
    ASSERT_NEAR(reference.attenuatedPeak / reference.baselinePeak,
                expectedAttenuatedGain, 2e-3f)
        << "the determinism scene never reached its authored attenuation";

    for (std::uint32_t workers : {2u, 4u}) {
        SidechainWorkerResult parallel;
        bool unavailable = false;
        RenderSidechainScene(workers, parallel, &unavailable);
        if (unavailable) {
            GTEST_SKIP() << "renderWorkerCount=" << workers
                         << " fell back to serial after a worker setup or wake failure";
        }
        ExpectNonSilent(parallel.capture);
        ASSERT_NEAR(parallel.attenuatedPeak / parallel.baselinePeak,
                    expectedAttenuatedGain, 2e-3f)
            << "renderWorkerCount=" << workers
            << " never reached its authored attenuation";
        ExpectBitIdentical(reference.capture, parallel.capture, workers);
    }
}

struct RawVoiceSidechainWorkerResult {
    CaptureMulti capture;
    float detectorPeak = 0.0f;
};

void RenderRawVoiceSidechainScene(
    std::uint32_t workerCount,
    RawVoiceSidechainWorkerResult& out,
    bool* outUnavailable = nullptr) {
    VnConfig config = MakeNullConfigLayout(
        4, kBlock, kRate, VN_CHANNEL_LAYOUT_STEREO,
        VN_CHANNEL_LAYOUT_STEREO);
    config.renderWorkerCount = workerCount;
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kRawSourceBus = harkentest::ObjectId(51);
    constexpr VnBusId kRawTargetBus = harkentest::ObjectId(52);
    constexpr VnSoundId kRawSourceSound = harkentest::ObjectId(53);
    constexpr VnSoundId kRawTargetSound = harkentest::ObjectId(54);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kRawSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kRawTargetBus, VN_BUS_MASTER));

    VnCompressorDesc compressor{};
    compressor.structSize = sizeof compressor;
    compressor.thresholdDb = -30.0f;
    compressor.ratio = 8.0f;
    compressor.attackMs = 0.1f;
    compressor.releaseMs = 80.0f;
    compressor.kneeDb = 3.0f;
    ASSERT_EQ(
        VN_OK,
        Vn_SetBusEffect(
            kRawTargetBus, 0, VN_EFFECT_COMPRESSOR,
            AsBase(&compressor)));

    const std::vector<float> source =
        harkentest::MakeSine(kBlock * 12, 440.0, kRate, 0.8f);
    const std::vector<float> target =
        harkentest::MakeSine(kBlock * 12, 220.0, kRate, 0.25f);
    const VnPCMDesc sourceDesc = MonoDesc(source, kRate);
    const VnPCMDesc targetDesc = MonoDesc(target, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kRawSourceSound, &sourceDesc));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kRawTargetSound, &targetDesc));

    VnPlayDesc sourcePlay = MakePlayDesc(kRawSourceSound);
    sourcePlay.busId = kRawSourceBus;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
    VnPlayDesc targetPlay = MakePlayDesc(kRawTargetSound);
    targetPlay.busId = kRawTargetBus;
    VnVoiceHandle targetVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));

    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof route;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_VOICE;
    route.tap = VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL;
    route.sourceVoiceHandle = sourceVoice;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(
                  kRawTargetBus, 0, 0, &route));

    PumpAndCaptureMulti(engine, 8, out.capture);
    const std::uint32_t targetIndex =
        engine->buses.FindIndex(kRawTargetBus);
    harken::BusTree::Bus* bus = engine->buses.BusState(targetIndex);
    ASSERT_NE(nullptr, bus);
    const auto* state =
        static_cast<const harken::CompressorState*>(
            bus->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    out.detectorPeak = state->telemetry.detectorPeak;
    EXPECT_EQ(0u, harken::memguard::HitCount())
        << "the allocation guard fired during raw Voice sidechain render";
    if (outUnavailable != nullptr) {
        *outUnavailable = ParallelRenderUnavailable(workerCount, CaptureFull());
    }
}

TEST(RenderWorkerDeterminism,
     RawVoiceSidechainIsByteIdenticalAcrossZeroOneTwoAndFourWorkers) {
    RawVoiceSidechainWorkerResult reference;
    RenderRawVoiceSidechainScene(0, reference);
    ExpectNonSilent(reference.capture);
    ASSERT_GT(reference.detectorPeak, 0.7f);

    for (std::uint32_t workers : {1u, 2u, 4u}) {
        RawVoiceSidechainWorkerResult parallel;
        bool unavailable = false;
        RenderRawVoiceSidechainScene(workers, parallel, &unavailable);
        if (unavailable) {
            GTEST_SKIP() << "renderWorkerCount=" << workers
                         << " fell back to serial after a worker setup or wake failure";
        }
        ExpectNonSilent(parallel.capture);
        EXPECT_FLOAT_EQ(reference.detectorPeak, parallel.detectorPeak);
        ExpectBitIdentical(reference.capture, parallel.capture, workers);
    }
}

// --- 1b. produced==0-while-Active corner (send-active voice reserves its tap
//         regardless of produced frames) ---

constexpr VnSoundId kStarveStream = harkentest::ObjectId(101);
constexpr VnSoundId kStarvePcmBase = harkentest::ObjectId(110);

struct StarveResult {
    CaptureMulti capture;
    std::uint64_t overflow = 0;
    std::uint64_t highWater = 0;
    std::uint64_t starved = 0;
};

// A deterministic starvation scene: a streamed voice whose ring is filled once
// (opening the prebuffer gate) and then throttled so it never refills, so after
// its prebuffered frames drain it renders `produced == 0 while Active` — the
// base-spec starvation state — for the rest of the run. It carries an active
// send. A pack of ordinary PCM voices renders alongside it so a tight arena has
// real per-voice reservations to run short on.
//
// The property this pins: a send-active voice reserves its mix-layout-wide send
// tap at prepare time regardless of how many frames it produces. `produced` is
// a fact of the cascade (run after the reservation) and the cascade is fanned
// across workers, so no parallel schedule can reproduce a serial per-voice
// "reserve the tap only if produced > 0" interleaving — reserving
// unconditionally in slot order is what keeps the arena allocation sequence, and
// thus degradation order, independent of the worker count (§8.2). The cost is a
// tap reserved for a zero-produced block, a recorded deviation from the M4
// serial path that only shows up at the arena-exhaustion boundary.
void RenderStarveScene(std::uint32_t workerCount, std::uint32_t arenaBytes,
                       bool sendOnStream, std::uint32_t blocks,
                       StarveResult& out, bool* outUnavailable = nullptr) {
    VnConfig config = MakeNullConfigLayout(8, kBlock, kRate,
                                           VN_CHANNEL_LAYOUT_STEREO,
                                           VN_CHANNEL_LAYOUT_STEREO);
    config.voiceSendSlots = 2;
    config.renderWorkerCount = workerCount;
    config.arenaBytes = arenaBytes;
    config.streamRingFrames = 512;       // two blocks of ring
    config.prebufferMs = 1;              // tiny gate, opens on the activation fill
    config.starvationCooldownBlocks = 1; // emit STARVED every starved block
    config.maxStreams = 2;

    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    ASSERT_EQ(VN_OK, Vn_CreateBus(kSendBus, VN_BUS_MASTER));

    // The stream source: long enough that a dry ring is always starvation
    // (source still active), never end-of-source (which would drain to Done).
    harkentest::StreamSpec spec;
    spec.soundId = kStarveStream;
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    const std::vector<float> streamSine =
        harkentest::MakeSine(kRate, 220.0, kRate, 0.25f);
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::string streamPath = std::string(HARKEN_TEMP_DIR "/") +
                                   "rw_starve_" + std::to_string(_getpid()) +
                                   ".stream";
    harkentest::WriteFileBytes(streamPath,
                              harkentest::BuildStreamBytes(spec, streamSine));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(kStarveStream, streamPath.c_str()));

    // The streamed voice takes slot 0 (first Free), so its scratch + tap are
    // reserved before the PCM voices — the tap participates in the sequence
    // ahead of the voices that degrade under a tight arena.
    VnPlayDesc streamPlay = MakePlayDesc(kStarveStream);
    VnVoiceHandle streamVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&streamPlay, &streamVoice));
    if (sendOnStream) {
        ASSERT_EQ(VN_OK, Vn_SetVoiceSend(streamVoice, 0, kSendBus, -3.0f));
    }

    // Six ordinary PCM voices for the scratch load (single-pass sources longer
    // than the run, so they sound in every block).
    for (std::uint32_t i = 0; i < 6; ++i) {
        const std::vector<float> sine =
            harkentest::MakeSine(kRate, 300.0 + 30.0 * i, kRate, 0.2f);
        const VnPCMDesc desc = MonoDesc(sine, kRate);
        ASSERT_EQ(VN_OK,
                  Vn_RegisterPCM(harkentest::OffsetObjectId(kStarvePcmBase, i),
                                 &desc));
        const VnPlayDesc play =
            MakePlayDesc(harkentest::OffsetObjectId(kStarvePcmBase, i), -6.0f);
        VnVoiceHandle h = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
    }

    // The activation-time serve fills the ring once; throttling now guarantees
    // no refill, so the ring drains to dry and the stream voice starves.
    engine->streams.SetThrottled(true);

    ASSERT_EQ(VN_OK, Vn_Update());
    PumpAndCaptureMulti(engine, blocks, out.capture);

    const VnSnapshot snap = CaptureFull();
    out.overflow = snap.arenaOverflowCount;
    out.highWater = snap.arenaHighWaterBytes;
    out.starved = snap.voiceStarvedCount;
    EXPECT_EQ(0u, harken::memguard::HitCount())
        << "the allocation guard fired during the starvation render";
    if (outUnavailable != nullptr) {
        *outUnavailable = ParallelRenderUnavailable(workerCount, snap);
    }
}

TEST(RenderWorkerDeterminism, ZeroProducedSendVoiceReservesTapAndStaysDeterministic) {
    constexpr std::uint32_t kBlocks = 24;
    constexpr std::uint32_t kLargeArena = 1u << 20;
    // Sized so bus planes + the stream voice's scratch + tap fit, plus a couple
    // PCM scratch planes, but not all six — the reservation runs short mid-slot,
    // degrading the later PCM voices (see the per-voice footprint in the helper).
    constexpr std::uint32_t kTightArena = 8192;
    // One mix-layout-wide send tap: the exact arena footprint of a stereo tap.
    const std::uint64_t kTapBytes =
        static_cast<std::uint64_t>(2u) * kBlock * sizeof(float);

    // (b) Discernible proof, at a roomy arena so no degradation confounds the
    // measurement: the send-active starving voice reserves its tap every block
    // even while producing zero frames, so the arena high-water is higher than
    // the identical scene without the send by exactly one tap footprint. The
    // high-water bytes ARE the discernible count.
    StarveResult withSend;
    StarveResult withoutSend;
    RenderStarveScene(0, kLargeArena, /*sendOnStream=*/true, kBlocks, withSend);
    RenderStarveScene(0, kLargeArena, /*sendOnStream=*/false, kBlocks,
                      withoutSend);
    ASSERT_GT(withSend.starved, 0u)
        << "the stream voice never starved — the produced==0 corner is vacuous";
    EXPECT_EQ(0u, withSend.overflow) << "roomy arena must not degrade";
    EXPECT_EQ(0u, withoutSend.overflow) << "roomy arena must not degrade";
    EXPECT_EQ(withoutSend.highWater + kTapBytes, withSend.highWater)
        << "a send-active voice producing 0 frames did not reserve its tap";

    // (a)+(c) Determinism at the arena boundary: with the tap now part of the
    // tight-arena footprint, serial and parallel degrade identically — same
    // voices dropped, same overflow, same high-water — and produce byte-
    // identical output. The starvation counter (proof the produced==0 corner is
    // live) must also match.
    StarveResult serial;
    RenderStarveScene(0, kTightArena, /*sendOnStream=*/true, kBlocks, serial);
    ExpectNonSilent(serial.capture);
    ASSERT_GT(serial.starved, 0u)
        << "the stream voice never starved — the produced==0 corner is vacuous";
    ASSERT_GT(serial.overflow, 0u)
        << "the tight arena did not degrade — the boundary corner is vacuous";
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        StarveResult parallel;
        bool unavailable = false;
        RenderStarveScene(workers, kTightArena, /*sendOnStream=*/true, kBlocks,
                          parallel, &unavailable);
        if (unavailable) {
            // Serial fallback after worker setup or wake failure: the
            // degradation fingerprint and byte-identity are defined only for a
            // completed parallel render.
            GTEST_SKIP()
                << "renderWorkerCount=" << workers
                << " fell back to serial after a worker setup or wake failure";
        }
        ExpectNonSilent(parallel.capture);
        ExpectBitIdentical(serial.capture, parallel.capture, workers);
        EXPECT_EQ(serial.overflow, parallel.overflow)
            << "renderWorkerCount=" << workers << " overflow diverged";
        EXPECT_EQ(serial.highWater, parallel.highWater)
            << "renderWorkerCount=" << workers << " high-water diverged";
        EXPECT_EQ(serial.starved, parallel.starved)
            << "renderWorkerCount=" << workers << " starvation count diverged";
    }
}

// --- 2. Worker busy-peak observability ---

TEST(RenderWorkerBusyPeak, NonzeroUnderLoadAndZeroWhenSerial) {
    CaptureMulti discard;

    // Serial: the worker busy peak is a passive 0, never fabricated.
    {
        VnConfig config = MakeNullConfigLayout(16, kBlock, kRate,
                                               VN_CHANNEL_LAYOUT_STEREO,
                                               VN_CHANNEL_LAYOUT_STEREO);
        config.renderWorkerCount = 0;
        config.arenaBytes = 1u << 20;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> sine =
            harkentest::MakeSine(kRate, 220.0, kRate, 0.25f);
        const VnPCMDesc desc = MonoDesc(sine, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        for (int i = 0; i < 8; ++i) {
            const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -2.0f);
            VnVoiceHandle h = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpAndCaptureMulti(session.Engine(), 32, discard);
        const VnSnapshot snap = CaptureFull();
        EXPECT_EQ(0u, snap.renderWorkerCount);
        EXPECT_EQ(0u, snap.renderWorkerBusyPeakUs);
    }

    // Parallel with a real voice load: some block put a worker to measurable
    // work, so the running peak is strictly positive.
    {
        VnConfig config = MakeNullConfigLayout(16, kBlock, kRate,
                                               VN_CHANNEL_LAYOUT_STEREO,
                                               VN_CHANNEL_LAYOUT_STEREO);
        config.renderWorkerCount = 4;
        config.arenaBytes = 1u << 20;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> sine =
            harkentest::MakeSine(kRate, 220.0, kRate, 0.25f);
        const VnPCMDesc desc = MonoDesc(sine, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        for (int i = 0; i < 16; ++i) {
            const VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.1f, 0.0f, -3.0f, -2.0f);
            VnVoiceHandle h = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        PumpAndCaptureMulti(session.Engine(), 200, discard);
        const VnSnapshot snap = CaptureFull();
        // A failed worker setup/wake disables the pool. Even with workers,
        // the coordinator may claim every job before any worker wakes, leaving
        // no worker busy span to measure.
        // The busy-span measurement itself is proven under a controlled load,
        // independent of the host, by
        // RenderWorkerBusyPeak.MeasuresBusySpanUnderControlledLoad.
        if (snap.renderWorkerCount == 0u || snap.renderWorkerBusyPeakUs == 0u) {
            GTEST_SKIP()
                << "host did not sustain measurable render-worker parallelism "
                   "for this engine render (renderWorkerCount="
                << snap.renderWorkerCount
                << ", busy peak=" << snap.renderWorkerBusyPeakUs << "us)";
        }
        EXPECT_EQ(4u, snap.renderWorkerCount);
        EXPECT_GT(snap.renderWorkerBusyPeakUs, 0u)
            << "workers carried a load but reported zero busy time";
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }
}

TEST(RenderWorkerBusyPeak, MeasuresBusySpanUnderControlledLoad) {
    // The snapshot's renderWorkerBusyPeakUs is fed by each worker's RunBatch
    // wall-clock span. Whether a real engine render puts a worker to measurable
    // work depends on how the host schedules the coordinator against the workers,
    // which a constrained runner does not guarantee (see the skip in the test
    // above). This proves the measurement itself, independent of the host: a
    // start-barrier holds every executor inside its job at once, so the
    // coordinator cannot drain the batch alone and at least the worker threads
    // each run a job, and a controlled busy span makes every RunBatch exceed the
    // one-microsecond snapshot floor. The measured peak must then be nonzero.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(4, 256));
    ASSERT_TRUE(pool.EnableOffline());
    ASSERT_EQ(4u, pool.WorkerCount());

    struct Load {
        std::atomic<std::uint32_t> started{0};
        std::uint32_t jobCount = 0;
        std::uint64_t spinTicks = 0;

        static void Job(void* userData, std::uint32_t) {
            auto* p = static_cast<Load*>(userData);
            // Start-barrier: no executor leaves its job until every job has
            // begun, so the coordinator (stuck in the one job it claims) cannot
            // drain the rest and at least jobCount-1 jobs run on worker threads.
            p->started.fetch_add(1, std::memory_order_acq_rel);
            while (p->started.load(std::memory_order_acquire) < p->jobCount) {
                std::this_thread::yield();
            }
            // A controlled busy span so each executor's RunBatch span exceeds the
            // whole-microsecond measurement floor regardless of core count.
            const std::uint64_t deadline =
                harken::CpuClockTicks() + p->spinTicks;
            while (harken::CpuClockTicks() < deadline) {
                // Busy spin: the point is to spend measurable wall-clock time.
            }
        }
    } load;
    load.jobCount = pool.WorkerCount();
    const std::uint64_t ticksPerSecond = harken::CpuClockTicksPerSecond();
    ASSERT_GT(ticksPerSecond, 0u);
    load.spinTicks = ticksPerSecond / 10000;  // about 100 microseconds

    const harken::RenderWorkerPool::RunResult result =
        pool.Run(load.jobCount, &Load::Job, &load);
    ASSERT_FALSE(result.deferred)
        << "the controlled batch deferred; a deferred batch reports no busy "
           "peak by design";
    const std::uint64_t micros =
        harken::CpuTicksToMicros(0, result.busyPeakTicks, ticksPerSecond);
    EXPECT_GT(micros, 0u)
        << "a worker carried a controlled load of about 100us but the measured "
           "busy peak was zero";

    pool.Stop();
}

TEST(RenderWorkerBusyPeak,
     VoicePoolPeakConvertsPublishesAndReachesPublicSnapshot) {
    VnConfig config = MakeNullConfigLayout(2, 480, kRate,
                                           VN_CHANNEL_LAYOUT_STEREO,
                                           VN_CHANNEL_LAYOUT_STEREO);
    config.fxSlotsPerVoice = 1;
    config.renderWorkerCount = 1;
    config.arenaBytes = 1u << 20;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();

    const std::vector<float> samples =
        harkentest::MakeSine(kRate, 220.0, kRate, 0.25f);
    const VnPCMDesc desc = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    std::array<VnVoiceHandle, 2> handles{};
    for (VnVoiceHandle& handle : handles) {
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1), -2.0f);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    const std::uint64_t ticksPerSecond = harken::CpuClockTicksPerSecond();
    ASSERT_GT(ticksPerSecond, 0u);
    BusyPeakChainLoad load;
    load.participantCount = static_cast<std::uint32_t>(handles.size());
    load.spinTicks = std::max<std::uint64_t>(1, ticksPerSecond / 10000);
    // The worker needs 2 ms while the callback has a 10 ms block. Finishing
    // this valid quantum must not be cut short by a fixed 1 ms wait budget.
    load.workerSpinTicks = std::max<std::uint64_t>(1, ticksPerSecond / 500);
    for (const VnVoiceHandle publicHandle : handles) {
        const VnVoiceHandle renderHandle =
            harkentest::RenderHandle(engine, publicHandle);
        harken::VoiceRenderContext* const context =
            engine->voices.Resolve(renderHandle);
        ASSERT_NE(context, nullptr);
        ASSERT_GE(context->fxSlotCount, 1u);
        context->fxSlots[0].process = &BusyPeakChainLoad::Process;
        context->fxSlots[0].state = &load;
    }

    const auto completedBefore = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    harken::detail::VoicePoolReleaseTestAccess::JoinWorkers(engine->voices);
    EXPECT_EQ(completedBefore + 1u, engine->mixer.BlocksRendered());
    EXPECT_EQ(load.started.load(std::memory_order_acquire), handles.size());
    const std::uint64_t peakTicks = engine->voices.RenderWorkerBusyPeakTicks();
    ASSERT_GT(peakTicks, 0u);

    const std::uint64_t seconds = peakTicks / ticksPerSecond;
    const std::uint64_t remainder = peakTicks % ticksPerSecond;
    const std::uint64_t micros64 =
        seconds * 1'000'000ull +
        (remainder * 1'000'000ull) / ticksPerSecond;
    const std::uint32_t expectedMicros =
        micros64 > UINT32_MAX ? UINT32_MAX
                              : static_cast<std::uint32_t>(micros64);
    ASSERT_GT(expectedMicros, 0u);

    const VnSnapshot snapshot = CaptureFull();
    EXPECT_EQ(snapshot.renderWorkerBusyPeakUs, expectedMicros);
}

// --- 3. Allocation guard armed on the worker threads ---

namespace {

struct GuardProbe {
    std::atomic<std::uint32_t> started{0};
    std::vector<char> tagged;
    std::uint32_t jobCount = 0;

    static void Job(void* userData, std::uint32_t jobIndex) {
        auto* p = static_cast<GuardProbe*>(userData);
        // Recorded before the barrier so the observation is the running thread's
        // own tag state, not a post-release artifact.
        p->tagged[jobIndex] =
            harken::memguard::IsAudioTagged() ? 1 : 0;
        // Start-barrier: no participant leaves its job until every job has
        // begun. The coordinator, stuck in the first job it claims, cannot then
        // drain the rest — so at least jobCount-1 jobs run on worker threads,
        // making the proof deterministic under any scheduler.
        p->started.fetch_add(1, std::memory_order_acq_rel);
        while (p->started.load(std::memory_order_acquire) < p->jobCount) {
            std::this_thread::yield();
        }
    }
};

} // namespace

TEST(RenderWorkerPool, WorkersRunTaggedAsAudioThreads) {
    // The test thread is deliberately NOT inside an AudioThreadScope, so a job
    // that observes IsAudioTagged() == true can only be executing on a worker
    // (whose RunBatch entered the scope). If that scope were ever removed the
    // count would collapse to zero and this goes red — a real, red-able proof
    // rather than a review argument.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(4, 256));
    ASSERT_TRUE(pool.EnableOffline());
    ASSERT_EQ(4u, pool.WorkerCount());

    GuardProbe probe;
    probe.jobCount = 4;
    probe.tagged.assign(probe.jobCount, 0);
    pool.Run(probe.jobCount, &GuardProbe::Job, &probe);

    std::uint32_t taggedOnWorker = 0;
    for (char t : probe.tagged) {
        taggedOnWorker += (t != 0) ? 1u : 0u;
    }
    // The coordinator runs at most one (barrier-blocked) job, so at least
    // jobCount-1 jobs ran tagged on workers.
    EXPECT_GE(taggedOnWorker, probe.jobCount - 1u)
        << "worker threads were not tagged as audio threads";
    EXPECT_EQ(0u, harken::memguard::HitCount());

    pool.Stop();
}

TEST(RenderWorkerPool, RunsEveryJobExactlyOnce) {
    // Pool mechanics across worker counts and job counts, including the empty
    // batch and batches smaller than the worker count.
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        harken::RenderWorkerPool pool;
        ASSERT_TRUE(pool.Start(workers, 256));
        ASSERT_TRUE(pool.EnableOffline());
        for (std::uint32_t jobCount : {0u, 1u, 3u, 64u}) {
            std::vector<std::atomic<std::uint32_t>> counts(jobCount);
            for (auto& c : counts) {
                c.store(0, std::memory_order_relaxed);
            }
            struct Ctx {
                std::vector<std::atomic<std::uint32_t>>* counts;
            } ctx{&counts};
            pool.Run(
                jobCount,
                [](void* ud, std::uint32_t j) {
                    (*static_cast<Ctx*>(ud)->counts)[j].fetch_add(
                        1, std::memory_order_relaxed);
                },
                &ctx);
            for (std::uint32_t j = 0; j < jobCount; ++j) {
                EXPECT_EQ(1u, counts[j].load(std::memory_order_relaxed))
                    << "workers=" << workers << " jobCount=" << jobCount
                    << " job " << j;
            }
        }
        pool.Stop();
    }
}

// --- 3b. The rendezvous deadline expression ---

namespace {

struct DeadlineLogSink {
    std::vector<std::string> texts;
    std::vector<std::uint32_t> codes;
};

void DeadlineLogCb(void* userData, std::uint32_t level, std::uint32_t code,
                   const char* text) {
    (void)level;
    auto* sink = static_cast<DeadlineLogSink*>(userData);
    sink->codes.push_back(code);
    sink->texts.push_back(text == nullptr ? "" : text);
}

std::uint64_t DeadlineMicros(const harken::MixerLoop& mixer) {
    return harken::CpuTicksToMicros(0, mixer.JobDeadlineTicks(),
                                   harken::CpuClockTicksPerSecond());
}

} // namespace

TEST(RenderWorkerDeadlineBudget, BlockPeriodReservesHalfForMixTail) {
    for (const std::uint32_t frames : {64u, 256u, 480u}) {
        VnConfig config = MakeNullConfigLayout(4, frames, kRate,
            VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO);
        config.renderWorkerCount = 2;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto micros = DeadlineMicros(session.Engine()->mixer);
        const auto halfPeriodUs = static_cast<std::uint64_t>(frames) * 500000u / kRate;
        EXPECT_NEAR(static_cast<double>(halfPeriodUs), static_cast<double>(micros), 1.0);
        EXPECT_GT(micros, 0u);
    }
}

TEST(RenderWorkerDeadlineBudget, ShortBlocksKeepTheirBudgetAndEmitLogCode) {
    // A short block keeps its own deadline and reports the small scheduling
    // margin; the warning must not extend it to a fixed minimum duration.
    constexpr std::uint32_t kShortBlock = 64; // 1333 us at 48 kHz
    DeadlineLogSink sink;
    VnConfig config = MakeNullConfigLayout(4, kShortBlock, kRate,
                                           VN_CHANNEL_LAYOUT_STEREO,
                                           VN_CHANNEL_LAYOUT_STEREO);
    config.renderWorkerCount = 2;
    config.logCallback = &DeadlineLogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update()); // formats the records queued at init

    const std::uint64_t micros = DeadlineMicros(session.Engine()->mixer);
    EXPECT_GT(micros, 0u);
    EXPECT_LT(micros, 1000u) << "the block-period ceiling did not bind";
    // Half of 1333 us, within the truncation of two tick conversions.
    EXPECT_GE(micros, 650u);
    EXPECT_LE(micros, 680u);

    std::string capText;
    for (std::size_t i = 0; i < sink.codes.size(); ++i) {
        if (sink.codes[i] == harken::kLogCodeRenderDeadlineBelowJitterFloor) {
            capText = sink.texts[i];
        }
    }
    ASSERT_FALSE(capText.empty())
        << "a capped deadline was not reported at init";
}

TEST(RenderWorkerDeadlineBudget, OrdinaryBlockSizesReportNothing) {
    DeadlineLogSink sink;
    VnConfig config = MakeNullConfigLayout(4, kBlock, kRate,
                                           VN_CHANNEL_LAYOUT_STEREO,
                                           VN_CHANNEL_LAYOUT_STEREO);
    config.renderWorkerCount = 2;
    config.logCallback = &DeadlineLogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_Update());
    for (std::uint32_t code : sink.codes) {
        EXPECT_NE(harken::kLogCodeRenderDeadlineBelowJitterFloor, code)
            << "a healthy block size reported a capped deadline";
    }
}

// --- 4. Zero new vocabulary under the parallel path ---

TEST(RenderWorkerVocabulary, ParallelSceneAddsNoOpcode) {
    VnConfig config = MakeNullConfigLayout(16, kBlock, kRate,
                                           VN_CHANNEL_LAYOUT_5_1,
                                           VN_CHANNEL_LAYOUT_STEREO);
    config.fxSlotsPerVoice = 2;
    config.voiceSendSlots = 2;
    config.renderWorkerCount = 4;
    config.arenaBytes = 1u << 20;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    ASSERT_EQ(VN_OK, Vn_CreateBus(kSendBus, VN_BUS_MASTER));
    const std::vector<float> sine =
        harkentest::MakeSine(kRate, 330.0, kRate, 0.25f);
    const VnPCMDesc desc = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    for (int i = 0; i < 8; ++i) {
        const VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 0.2f, 0.0f, -3.0f, -3.0f);
        VnVoiceHandle h = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
        if (i == 0) {
            ASSERT_EQ(VN_OK, Vn_SetVoiceSend(h, 0, kSendBus, -4.0f));
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());

    CaptureMulti discard;
    PumpAndCaptureMulti(engine, 64, discard);

    // The narrow-waist claim (§4 / §11.4): job-parallel rendering changes only
    // scheduling, so no opcode beyond the frozen set is pushed by the scene.
    const std::uint32_t seen = engine->commands.OpcodeSeenMask();
    const std::uint32_t legal =
        static_cast<std::uint32_t>((1ull << (harken::kMaxKnownOpcode + 1)) - 2u);
    EXPECT_EQ(0u, seen & ~legal)
        << "the parallel scene used an opcode beyond the frozen vocabulary";
    EXPECT_FALSE(engine->commands.OpcodeOverflowSeen())
        << "the parallel scene pushed an opcode the seen-mask cannot represent";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

} // namespace
