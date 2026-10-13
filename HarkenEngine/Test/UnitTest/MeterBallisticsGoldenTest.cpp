// Meter ballistics analytic goldens over the private MeterProbe level.
// The envelope advances per drained block on the audio-frame timebase:
//
//   coeff(ms) = exp(-blockFrames / (sampleRate * ms / 1000))
//   env       = peak + (env - peak) * (peak > env ? attackCoeff
//                                                 : releaseCoeff)
//
// (the standard first-order RC one-pole exponential averager of envelope
// detection — public textbook math, e.g. U. Zoelzer, "Digital Audio
// Signal Processing"). The tests replicate that float recurrence against
// the probe's observed dBFS level: attack and release trajectories in
// closed form, the -96 dBFS floor, and the pump-ratio equivalence: the
// same rendered block sequence drained at 1, 3, and 7 blocks per tick
// yields the bitwise-identical envelope, because ballistics advance by
// block frame count, never by tick spacing.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "harken/param/MeterProbe.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

constexpr VnSoundId kDcSound = harkentest::ObjectId(911);
constexpr VnObjectId kProbe = harkentest::ObjectId(7101);
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;

// The engine-identical per-block coefficient (same expression, same
// double-to-float narrowing), so the replicated recurrence is bit-exact.
float BlockCoeff(std::uint32_t ms) {
    if (ms == 0) {
        return 0.0f;
    }
    const double tauFrames =
        static_cast<double>(kRate) * static_cast<double>(ms) / 1000.0;
    return static_cast<float>(
        std::exp(-static_cast<double>(kBlock) / tauFrames));
}

float EnvToDb(float env) {
    if (!(env > 0.0f)) {
        return harken::kMeterFloorDb;
    }
    const float db = 20.0f * std::log10(env);
    return db < harken::kMeterFloorDb ? harken::kMeterFloorDb : db;
}

float ReadProbe(harken::Engine* engine) {
    return engine->meterDetectors.ValueDb(kProbe);
}

void RegisterLongDc(float amplitude) {
    static std::vector<float> dc; // outlives the session (COPY policy anyway)
    dc = harkentest::MakeDC(kRate * 8, amplitude);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kDcSound, &pcm), VN_OK);
}

TEST(MeterBallisticsGolden, AttackAndReleaseTrajectoriesMatchClosedForm) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();

    constexpr std::uint32_t kAttackMs = 20;
    constexpr std::uint32_t kReleaseMs = 100;
    ASSERT_EQ(harken::InstallMeterProbe(*engine, kProbe, VN_BUS_MASTER,
                                       VN_BUS_METER_TAP_POST_EFFECTS,
                                       kAttackMs, kReleaseMs),
              VN_OK);
    EXPECT_EQ(ReadProbe(engine), harken::kMeterFloorDb);

    RegisterLongDc(0.5f);
    VnPlayDesc play = harkentest::MakePlayDesc(kDcSound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);

    // The master's block peak is constant under DC; take it from the same
    // tap the ring records (the published snapshot) after the first block.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(Vn_CaptureSnapshot(&snapshot), VN_OK);
    const float peak = snapshot.busPeaks[0];
    ASSERT_GT(peak, harkentest::kSilenceFloor);

    // Attack: one block drained per tick; the probe level must track the
    // replicated float recurrence exactly (same ops, same order).
    const float attackCoeff = BlockCoeff(kAttackMs);
    const float releaseCoeff = BlockCoeff(kReleaseMs);
    float env = 0.0f;
    env = peak + (env - peak) * attackCoeff; // the block already drained
    EXPECT_FLOAT_EQ(ReadProbe(engine), EnvToDb(env));
    for (int i = 0; i < 12; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
        env = peak + (env - peak) * attackCoeff;
        EXPECT_FLOAT_EQ(ReadProbe(engine), EnvToDb(env));
    }

    // Release: cut the voice; peaks drop to 0 and the release coefficient
    // takes over (peak > env selects attack, otherwise release).
    ASSERT_EQ(Vn_Stop(voice, 0), VN_OK);
    engine->nullBackend.Pump(1); // the stop lands at this block boundary
    ASSERT_EQ(Vn_Update(), VN_OK);
    env = 0.0f + (env - 0.0f) * releaseCoeff;
    EXPECT_FLOAT_EQ(ReadProbe(engine), EnvToDb(env));
    for (int i = 0; i < 8; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(Vn_Update(), VN_OK);
        env = env * releaseCoeff;
        EXPECT_FLOAT_EQ(ReadProbe(engine), EnvToDb(env));
    }
}

TEST(MeterBallisticsGolden, SilenceFloorsAtMinus96NeverBelow) {
    harkentest::EngineSession session(
        harkentest::MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();

    // Instant attack, near-instant release: the envelope collapses fast.
    ASSERT_EQ(harken::InstallMeterProbe(*engine, kProbe, VN_BUS_MASTER,
                                       VN_BUS_METER_TAP_POST_EFFECTS, 0, 2),
              VN_OK);
    RegisterLongDc(0.5f);
    VnPlayDesc play = harkentest::MakePlayDesc(kDcSound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);
    engine->nullBackend.Pump(2);
    ASSERT_EQ(Vn_Update(), VN_OK);
    ASSERT_GT(ReadProbe(engine), harken::kMeterFloorDb);

    ASSERT_EQ(Vn_Stop(voice, 0), VN_OK);
    // releaseMs = 2 decays ~23 dB per block: a dozen silent blocks land
    // far below any floor — the observed level must clamp at exactly
    // -96, never -inf and never below.
    engine->nullBackend.Pump(12);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(ReadProbe(engine), harken::kMeterFloorDb);
}

// Pump-ratio equivalence: identical block sequences drained 1, 3, and 7
// blocks per tick produce the identical envelope at every common
// checkpoint (21 and 42 blocks: common multiples of all three cadences).
// Bitwise equality, not tolerance: the audio-frame timebase makes the
// drain cadence algebraically invisible.
TEST(MeterBallisticsGolden, PumpRatioEquivalenceOneThreeSeven) {
    constexpr std::uint32_t kCheckpointA = 21;
    constexpr std::uint32_t kCheckpointB = 42;
    float valuesA[3] = {};
    float valuesB[3] = {};
    const std::uint32_t cadences[3] = {1, 3, 7};

    for (int run = 0; run < 3; ++run) {
        harkentest::EngineSession session(
            harkentest::MakeNullConfig(0, kBlock, kRate));
        ASSERT_EQ(session.InitResult(), VN_OK);
        harken::Engine* engine = session.Engine();
        // A slow attack keeps the envelope moving across all 42 blocks,
        // so a cadence-dependent drain would diverge measurably.
        ASSERT_EQ(harken::InstallMeterProbe(*engine, kProbe, VN_BUS_MASTER,
                                           VN_BUS_METER_TAP_POST_EFFECTS,
                                           200, 400),
                  VN_OK);
        RegisterLongDc(0.5f);
        VnPlayDesc play = harkentest::MakePlayDesc(kDcSound);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);

        const std::uint32_t cadence = cadences[run];
        std::uint32_t rendered = 0;
        while (rendered < kCheckpointB) {
            engine->nullBackend.Pump(cadence);
            rendered += cadence;
            ASSERT_EQ(Vn_Update(), VN_OK);
            if (rendered == kCheckpointA) {
                valuesA[run] = ReadProbe(engine);
            }
        }
        valuesB[run] = ReadProbe(engine);
    }

    EXPECT_EQ(valuesA[0], valuesA[1]);
    EXPECT_EQ(valuesA[0], valuesA[2]);
    EXPECT_EQ(valuesB[0], valuesB[1]);
    EXPECT_EQ(valuesB[0], valuesB[2]);
    // And the envelope really was still moving between the checkpoints —
    // equal values from a settled envelope would prove nothing.
    EXPECT_NE(valuesA[0], valuesB[0]);
}

} // namespace
