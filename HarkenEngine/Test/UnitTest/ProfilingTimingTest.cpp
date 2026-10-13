// Per-stage CPU timing (the snapshot's seven-field profiling vintage):
// under a genuine render load, every field reports a nonzero measurement,
// the peaks dominate their lasts, and the audio stage trio sums to no more
// than the whole callback plus the documented tolerance. The audio quintet
// is published through the existing snapshot double buffer, so the
// null-pump-then-capture pattern makes each pumped block's timing visible
// to the next capture; the update pair is the logic thread's own
// self-measurement.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;

namespace {

// The documented measurement tolerance for the stage-sum invariant
// (Harken.h, audioCpuLastUs): drain + voices + buses <= whole + this. The
// stage windows share their boundary clock reads and the tick-to-micros
// conversion truncates, so the inequality holds exactly by construction;
// the tolerance is headroom against clock-boundary noise should the
// conversion ever change.
constexpr std::uint32_t kTimingSumToleranceUs = 2;

constexpr std::uint32_t kBlockFrames = 256;

VnSnapshot Capture() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

// A load scene that exercises every measured stage: many looping voices
// (some 3D with attenuation, some through insert effects), so the voice
// cascade, the bus fold, the command drain, and the per-update spatial and
// parameter work all have real material.
struct LoadScene {
    std::vector<VnVoiceHandle> voices;

    void Build() {
        const std::vector<float> tone =
            harkentest::MakeSine(48000, 220.0, 48000.0, 0.5f);
        VnPCMDesc pcm = MonoDesc(tone, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));

        VnAttenuationDesc attenuation =
            harkentest::MakeAttenuationDesc(1.0f, 100.0f);
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(1), &attenuation));

        for (int i = 0; i < 24; ++i) {
            VnPlayDesc play =
                (i % 2 == 0)
                    ? MakePlayDesc(harkentest::ObjectId(1))
                    : MakePlayDesc3D(harkentest::ObjectId(1), 2.0f + static_cast<float>(i), 0.0f,
                                     -3.0f);
            play.loopCount = 0; // loop until stopped: sustained load
            play.pitchRatio = 1.0f + 0.01f * static_cast<float>(i);
            VnVoiceHandle handle = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
            if (i % 2 == 1) {
                ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, harkentest::ObjectId(1)));
            }
            voices.push_back(handle);
        }
        // Insert effects on a spread of voices: both built-in types, within
        // their default pool capacities (8 delay + 16 tone instances).
        VnFeedbackDelayDesc delay{};
        delay.structSize = sizeof(VnFeedbackDelayDesc);
        delay.delayMs = 25.0f;
        delay.feedback = 0.4f;
        delay.wet = 0.5f;
        delay.dry = 1.0f;
        VnThreeBandToneDesc toneFx{};
        toneFx.structSize = sizeof(VnThreeBandToneDesc);
        toneFx.lowShelfGainDb = 3.0f;
        toneFx.lowShelfFreqHz = 200.0f;
        toneFx.peakGainDb = -2.0f;
        toneFx.peakFreqHz = 1000.0f;
        toneFx.peakQ = 1.0f;
        toneFx.highShelfGainDb = 2.0f;
        toneFx.highShelfFreqHz = 6000.0f;
        for (int i = 0; i < 8; ++i) {
            ASSERT_EQ(VN_OK,
                      Vn_SetVoiceEffect(voices[static_cast<std::size_t>(i)],
                                        0, VN_EFFECT_FEEDBACK_DELAY,
                                        reinterpret_cast<VnEffectDesc*>(
                                            &delay)));
        }
        for (int i = 8; i < 20; ++i) {
            ASSERT_EQ(VN_OK,
                      Vn_SetVoiceEffect(voices[static_cast<std::size_t>(i)],
                                        0, VN_EFFECT_THREE_BAND_TONE,
                                        reinterpret_cast<VnEffectDesc*>(
                                            &toneFx)));
        }
    }

    // One load round: a burst of commands (feeds the drain stage), a
    // rendered block, and a pump with fresh listener/parameter work (feeds
    // the update measurement). Returns the post-round capture.
    VnSnapshot Round(harken::Engine* engine, int iteration) {
        for (std::size_t v = 0; v < voices.size(); ++v) {
            (void)Vn_SetVoiceVolume(voices[v],
                                    -1.0f - 0.1f * static_cast<float>(v), 0);
        }
        (void)Vn_SetListener(
            harkentest::Vec3(0.1f * static_cast<float>(iteration), 0.0f,
                            0.0f),
            harkentest::Vec3(0.0f, 0.0f, -1.0f),
            harkentest::Vec3(0.0f, 1.0f, 0.0f));
        engine->nullBackend.Pump(1);
        EXPECT_EQ(VN_OK, Vn_Update());
        return Capture();
    }
};

} // namespace

TEST(ProfilingTiming, SeptetNonZeroUnderLoadWithPeakDominance) {
    VnConfig config = MakeNullConfig(32, kBlockFrames);
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    LoadScene scene;
    ASSERT_NO_FATAL_FAILURE(scene.Build());

    // Rendering 24 looping voices with effects costs well over a
    // microsecond per block on any machine this suite runs on, and the
    // update pass walks the same voices' spatial and parameter state; the
    // bounded retry only absorbs a first block or two of warm-up. If every
    // round of a sustained load still measures zero somewhere, that IS the
    // defect this test exists to catch.
    VnSnapshot snapshot{};
    bool allNonZero = false;
    harken::Engine* engine = session.Engine();
    for (int i = 0; i < 200 && !allNonZero; ++i) {
        snapshot = scene.Round(engine, i);
        allNonZero = snapshot.audioCpuLastUs > 0 &&
                     snapshot.audioCpuPeakUs > 0 &&
                     snapshot.audioDrainLastUs > 0 &&
                     snapshot.audioVoicesLastUs > 0 &&
                     snapshot.audioBusesLastUs > 0 &&
                     snapshot.updateCpuLastUs > 0 &&
                     snapshot.updateCpuPeakUs > 0;
    }
    EXPECT_GT(snapshot.audioCpuLastUs, 0u);
    EXPECT_GT(snapshot.audioCpuPeakUs, 0u);
    EXPECT_GT(snapshot.audioDrainLastUs, 0u);
    EXPECT_GT(snapshot.audioVoicesLastUs, 0u);
    EXPECT_GT(snapshot.audioBusesLastUs, 0u);
    EXPECT_GT(snapshot.updateCpuLastUs, 0u);
    EXPECT_GT(snapshot.updateCpuPeakUs, 0u);

    // Peaks dominate the measurement they peak over — by definition for
    // the block that set them, and monotonically ever after.
    EXPECT_GE(snapshot.audioCpuPeakUs, snapshot.audioCpuLastUs);
    EXPECT_GE(snapshot.updateCpuPeakUs, snapshot.updateCpuLastUs);

    // The render is genuinely sounding, not a silent no-op being timed.
    EXPECT_GT(snapshot.busPeaks[0], harkentest::kSilenceFloor);
}

TEST(ProfilingTiming, StageSumWithinWholePlusToleranceEveryBlock) {
    VnConfig config = MakeNullConfig(32, kBlockFrames);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    LoadScene scene;
    ASSERT_NO_FATAL_FAILURE(scene.Build());

    harken::Engine* engine = session.Engine();
    std::uint32_t lastPeak = 0;
    for (int i = 0; i < 60; ++i) {
        const VnSnapshot snapshot = scene.Round(engine, i);
        const std::uint64_t stageSum =
            static_cast<std::uint64_t>(snapshot.audioDrainLastUs) +
            snapshot.audioVoicesLastUs + snapshot.audioBusesLastUs;
        EXPECT_LE(stageSum,
                  static_cast<std::uint64_t>(snapshot.audioCpuLastUs) +
                      kTimingSumToleranceUs)
            << "block " << i << ": stage sum exceeded the whole-callback "
            << "measurement past the documented tolerance";
        // The peak never regresses.
        EXPECT_GE(snapshot.audioCpuPeakUs, lastPeak);
        lastPeak = snapshot.audioCpuPeakUs;
    }
}

TEST(ProfilingTiming, TimingFieldsReadZeroBeforeAnyBlockOrUpdate) {
    // A fresh engine has rendered nothing and pumped nothing: every timing
    // field is an honest zero, not a stale or invented measurement.
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const VnSnapshot snapshot = Capture();
    EXPECT_EQ(0u, snapshot.audioCpuLastUs);
    EXPECT_EQ(0u, snapshot.audioCpuPeakUs);
    EXPECT_EQ(0u, snapshot.audioDrainLastUs);
    EXPECT_EQ(0u, snapshot.audioVoicesLastUs);
    EXPECT_EQ(0u, snapshot.audioBusesLastUs);
    EXPECT_EQ(0u, snapshot.updateCpuLastUs);
    EXPECT_EQ(0u, snapshot.updateCpuPeakUs);
}
