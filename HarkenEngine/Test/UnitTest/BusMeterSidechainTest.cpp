// MeterProbe-reactive sidechain attenuation end-to-end: the detector bus's
// smoothed probe level drives the music bus's BusVolume through a private
// binding, entirely on the null backend and observed in rendered bus peaks.
// Also covers the live two-slot bus combine, the transition ramp riding the
// published bus command, and honest probe observation in every build.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/MeterProbe.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

constexpr VnSoundId kMusicSound = harkentest::ObjectId(921);
constexpr VnSoundId kVoiceSound = harkentest::ObjectId(922);
constexpr VnObjectId kSidechainProbe = harkentest::ObjectId(7201);
constexpr VnBusId kVoBus = harkentest::ObjectId(31);
constexpr VnBusId kMusicBus = harkentest::ObjectId(32);
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;

float BusPeak(harken::Engine* engine, VnBusId busId) {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(Vn_CaptureSnapshot(&snapshot), VN_OK);
    const std::uint32_t index = engine->buses.FindIndex(busId);
    EXPECT_LT(index, static_cast<std::uint32_t>(VN_SNAPSHOT_MAX_BUSES));
    return snapshot.busPeaks[index];
}

struct SidechainScene {
    harken::Engine* engine = nullptr;
    VnVoiceHandle music = 0;
    float musicBaseline = 0.0f; // steady music-bus peak before attenuation

    void Build(std::uint32_t transitionMs) {
        ASSERT_EQ(Vn_CreateBus(kVoBus, VN_BUS_MASTER), VN_OK);
        ASSERT_EQ(Vn_CreateBus(kMusicBus, VN_BUS_MASTER), VN_OK);

        static std::vector<float> musicDc;
        static std::vector<float> voiceDc;
        musicDc = harkentest::MakeDC(kRate * 8, 0.5f);
        voiceDc = harkentest::MakeDC(kRate * 8, 1.0f);
        const VnPCMDesc musicPcm = harkentest::MonoDesc(musicDc, kRate);
        ASSERT_EQ(Vn_RegisterPCM(kMusicSound, &musicPcm), VN_OK);
        const VnPCMDesc voicePcm = harkentest::MonoDesc(voiceDc, kRate);
        ASSERT_EQ(Vn_RegisterPCM(kVoiceSound, &voicePcm), VN_OK);

        VnPlayDesc musicPlay = harkentest::MakePlayDesc(kMusicSound);
        musicPlay.busId = kMusicBus;
        ASSERT_EQ(harkentest::PlayRaw(&musicPlay, &music), VN_OK);

        // Instant detector ballistics isolate the binding and publication
        // path; the ballistics goldens own smoothing behavior.
        std::uint32_t probeGeneration = 0;
        ASSERT_EQ(harken::InstallMeterProbe(
                      *engine, kSidechainProbe, kVoBus,
                      VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                      &probeGeneration),
                  VN_OK);
        const harken::CurvePoint points[2] = {{-96.0f, 0.0f},
                                             {-6.0f, -12.0f}};
        const harken::CurveHandlePair handles[1] = {
            {-96.0f, 0.0f, -6.0f, -12.0f}};
        ASSERT_EQ(harken::DefineMeterProbeBinding(
                      *engine, harkentest::ObjectId(71), kSidechainProbe, probeGeneration,
                      VN_TARGET_SCOPE_BUS, 0, points, handles, 2,
                      transitionMs),
                  VN_OK);
        const std::uint32_t musicIndex = engine->buses.FindIndex(kMusicBus);
        ASSERT_EQ(harken::AttachMeterProbeBinding(
                      *engine, harkentest::ObjectId(71), VN_TARGET_SCOPE_BUS, kMusicBus, 0,
                      engine->buses.GenerationAt(musicIndex)),
                  VN_OK);

        // Prime the music bus gain at unity: a bus's FIRST volume command
        // snaps regardless of rampMs (no fictitious prior value to ramp
        // from), so the transition-ramp case below needs a started gain.
        ASSERT_EQ(Vn_SetBusVolume(kMusicBus, 0.0f, 0), VN_OK);

        // Settle: music plays, detector silent, sidechain contributes nothing.
        engine->nullBackend.Pump(2);
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(2);
        ASSERT_EQ(Vn_Update(), VN_OK);
        musicBaseline = BusPeak(engine, kMusicBus);
        ASSERT_GT(musicBaseline, harkentest::kSilenceFloor);
    }

    VnVoiceHandle StartDialogue() {
        VnPlayDesc voicePlay = harkentest::MakePlayDesc(kVoiceSound);
        voicePlay.busId = kVoBus;
        VnVoiceHandle handle = 0;
        EXPECT_EQ(harkentest::PlayRaw(&voicePlay, &handle), VN_OK);
        return handle;
    }
};

TEST(BusMeterSidechain, DetectionLatencyStaysWithinControlRateBound) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(session.InitResult(), VN_OK);
    SidechainScene scene;
    scene.engine = session.Engine();
    scene.Build(/*transitionMs=*/0);
    harken::Engine* engine = scene.engine;

    // The dialogue command is queued before this block, so this block's
    // start is the first possible over-threshold signal time. The test pumps
    // exactly one block per logic tick. Under section 11.6 the latest legal
    // attenuation onset is: one detector block + up to one one-block tick interval
    // + one command-to-next-block-boundary interval = 3 blocks. At 48 kHz / 256
    // frames that is 16 ms; the bound is derived from this cadence rather than
    // copying the spec's separate 60 Hz example.
    scene.StartDialogue();
    const std::uint64_t signalBlockStart = engine->mixer.BlocksRendered();
    constexpr std::uint64_t kDetectorBlocks = 1;
    constexpr std::uint64_t kTickCadenceBlocks = 1;
    constexpr std::uint64_t kCommandBoundaryBlocks = 1;
    constexpr std::uint64_t kMaxLatencyBlocks =
        kDetectorBlocks + kTickCadenceBlocks + kCommandBoundaryBlocks;

    std::uint64_t firstAttenuationBlockStart = 0;
    bool attenuationObserved = false;
    for (std::uint64_t i = 0; i <= kMaxLatencyBlocks; ++i) {
        const std::uint64_t blockStart = engine->mixer.BlocksRendered();
        engine->nullBackend.Pump(1);
        const float musicPeak = BusPeak(engine, kMusicBus);
        if (musicPeak < scene.musicBaseline * 0.99f) {
            firstAttenuationBlockStart = blockStart;
            attenuationObserved = true;
            break;
        }
        ASSERT_EQ(Vn_Update(), VN_OK);
    }

    ASSERT_TRUE(attenuationObserved)
        << "music gain never left neutral inside the latency window";
    const std::uint64_t latencyBlocks =
        firstAttenuationBlockStart - signalBlockStart;
    const double latencyMs = static_cast<double>(latencyBlocks) *
                             static_cast<double>(kBlock) * 1000.0 /
                             static_cast<double>(kRate);
    const double maxLatencyMs = static_cast<double>(kMaxLatencyBlocks) *
                                static_cast<double>(kBlock) * 1000.0 /
                                static_cast<double>(kRate);
    EXPECT_LE(latencyBlocks, kMaxLatencyBlocks);
    EXPECT_LE(latencyMs, maxLatencyMs);
}

TEST(BusMeterSidechain, DialogueAttenuatesMusicAndReleases) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(session.InitResult(), VN_OK);
    SidechainScene scene;
    scene.engine = session.Engine();
    scene.Build(/*transitionMs=*/0);
    harken::Engine* engine = scene.engine;

    // Dialogue lands: one block to meter it, one update to publish the
    // attenuation, one block for the command to apply at its boundary.
    const VnVoiceHandle dialogue = scene.StartDialogue();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    // While the detector is hot, both the bus peak and the private probe
    // observation report the live signal.
    EXPECT_GT(BusPeak(engine, kVoBus), harkentest::kSilenceFloor);
    EXPECT_GT(engine->meterDetectors.ValueDb(kSidechainProbe),
              harken::kMeterFloorDb);

    // The attenuation: music drops by the curve's -12 dB (a pure gain scale on
    // the same signal, so the peak ratio is the linear gain).
    const float attenuated = BusPeak(engine, kMusicBus);
    const float expectedGain = std::pow(10.0f, -12.0f / 20.0f);
    EXPECT_NEAR(attenuated / scene.musicBaseline, expectedGain, 2e-3f);

    // Two-slot combine: an API-direct -6 dB sums in dB with the -12 dB
    // binding contribution (order-free, one reduced command).
    ASSERT_EQ(Vn_SetBusVolume(kMusicBus, -6.0f, 0), VN_OK);
    engine->nullBackend.Pump(1);
    const float combined = BusPeak(engine, kMusicBus);
    const float expectedCombined = std::pow(10.0f, -18.0f / 20.0f);
    EXPECT_NEAR(combined / scene.musicBaseline, expectedCombined, 2e-3f);
    ASSERT_EQ(Vn_SetBusVolume(kMusicBus, 0.0f, 0), VN_OK);

    // Release: dialogue stops, detector falls to the floor (release 0),
    // the contribution returns to neutral, music recovers.
    ASSERT_EQ(Vn_Stop(dialogue, 0), VN_OK);
    engine->nullBackend.Pump(1); // silent detector block
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1); // the un-attenuate command applies
    const float released = BusPeak(engine, kMusicBus);
    EXPECT_NEAR(released / scene.musicBaseline, 1.0f, 2e-3f);
}

TEST(BusMeterSidechain, TransitionMsRidesThePublishedBusRamp) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(session.InitResult(), VN_OK);
    SidechainScene scene;
    scene.engine = session.Engine();
    // 200 ms transition: ~38 blocks at 48 kHz / 256, so one block into
    // the ramp the gain has moved only a fraction of the way.
    scene.Build(/*transitionMs=*/200);
    harken::Engine* engine = scene.engine;

    scene.StartDialogue();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK); // publishes the attenuation with rampMs 200
    // Six blocks into a ~38-block ramp: a DC block's peak sits at the
    // ramping gain's start-of-block value, so the mid-ramp reading is
    // taken a few blocks in, where it is unambiguously between the
    // baseline and the target.
    engine->nullBackend.Pump(6);
    const float early = BusPeak(engine, kMusicBus);
    const float target = scene.musicBaseline * std::pow(10.0f, -12.0f / 20.0f);
    EXPECT_LT(early, scene.musicBaseline * 0.97f);
    EXPECT_GT(early, target * 1.05f);

    // After the full ramp span (plus slack), the attenuation has landed.
    for (int i = 0; i < 45; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
    }
    const float landed = BusPeak(engine, kMusicBus);
    EXPECT_NEAR(landed / scene.musicBaseline,
                std::pow(10.0f, -12.0f / 20.0f), 2e-3f);
}

TEST(BusMeterSidechain, DetachRampsContributionBackToNeutral) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(session.InitResult(), VN_OK);
    SidechainScene scene;
    scene.engine = session.Engine();
    scene.Build(/*transitionMs=*/0);
    harken::Engine* engine = scene.engine;

    scene.StartDialogue();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    const float attenuated = BusPeak(engine, kMusicBus);
    ASSERT_LT(attenuated, scene.musicBaseline * 0.5f);

    // Detach while attenuated: the contribution leaves at the next update
    // (transitionMs 0 snaps) and music returns to baseline even though
    // the detector stays hot.
    const std::uint32_t musicIndex = engine->buses.FindIndex(kMusicBus);
    ASSERT_EQ(harken::DetachMeterProbeBinding(
                  *engine, harkentest::ObjectId(71), VN_TARGET_SCOPE_BUS, kMusicBus, 0,
                  engine->buses.GenerationAt(musicIndex)),
              VN_OK);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    engine->nullBackend.Pump(1);
    const float released = BusPeak(engine, kMusicBus);
    EXPECT_NEAR(released / scene.musicBaseline, 1.0f, 2e-3f);
}

} // namespace
