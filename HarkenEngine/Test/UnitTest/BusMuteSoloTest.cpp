// Native bus mute/solo: rendered-output proof that mute silences a bus
// without disturbing its composed volume state, that unmute restores the
// latest composed value exactly, that solo audibility is tree-path scoped
// (send-fed paths do not follow), that mute wins inside a solo path, plus
// the refusal matrix, backpressure atomicity, fade ramp shape, and the
// snapshot audibility masks.
//
// Tolerances follow BusMixGoldenTest: DC through <= 3 gain stages stays
// within 1e-6 of the closed form; 1e-5 leaves headroom while failing on any
// wrong audibility (a mis-muted bus shifts the sum by a whole contribution).
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlockFrames = 256;
constexpr VnBusId kBusA = harkentest::ObjectId(10);
constexpr VnBusId kBusB = harkentest::ObjectId(11);
constexpr VnBusId kBusC = harkentest::ObjectId(12);

// 20*log10(0.5): requests exactly half amplitude.
const float kHalfDb = 20.0f * std::log10(0.5f);

// PumpAndCapture appends; a fresh Capture per pumped block keeps every
// assertion reading the block it names.
Capture PumpBlock(harken::Engine* engine) {
    Capture out;
    PumpAndCapture(engine, 1, out);
    return out;
}

void RegisterDcVoiceOnBus(VnSoundId soundId, float amplitude, VnBusId busId,
                          std::vector<float>& storage) {
    storage = harkentest::MakeDC(kBlockFrames, amplitude);
    const VnPCMDesc desc = MonoDesc(storage, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(soundId, &desc));
    VnPlayDesc play = MakePlayDesc(soundId);
    play.busId = busId;
    play.loopCount = 0; // loop so every captured block carries the DC
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
}

TEST(BusMuteSolo, MuteSilencesAndUnmuteRestoresComposedVolume) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, kHalfDb, 0));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.7f, kBusA, dc);

    Capture out;
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.35f, out.left[kBlockFrames - 1], 1e-5f);

    // Mute: the bus's published gain is the silence floor (exact linear
    // zero), snapped at the block boundary.
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 0));
    out = PumpBlock(session.Engine());
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "muted sample " << i;
    }

    // Unmute restores the composed volume exactly — the authored half gain
    // survived the mute untouched.
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 0, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.35f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusMuteSolo, VolumeChangeWhileMutedStaysSilentAndAppliesOnUnmute) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, kHalfDb, 0));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.7f, kBusA, dc);
    Capture out;
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.35f, out.left[kBlockFrames - 1], 1e-5f);

    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 0));
    // A direct volume change while muted retargets the composed base but the
    // wire value stays at the floor: still silent.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, 0.0f, 0));
    out = PumpBlock(session.Engine());
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "muted sample " << i;
    }

    // Unmute publishes the LATEST composed value (unity), not the pre-mute
    // half gain — mute never owned the volume state.
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 0, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.7f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusMuteSolo, MuteFadeRampsAcrossTheBlockWithoutAHardCut) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, kHalfDb, 0));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.7f, kBusA, dc);
    Capture out;
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.35f, out.left[kBlockFrames - 1], 1e-5f);

    // ~5 ms at 48 kHz rounds up to one 256-frame block: the fade block
    // starts near the pre-mute level and lands on silence at its last
    // sample; the next block is exactly silent.
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 5));
    out = PumpBlock(session.Engine());
    EXPECT_GT(out.left[0], 0.3f) << "fade must not hard-cut at block start";
    EXPECT_LT(out.left[kBlockFrames - 1], 1e-2f);
    out = PumpBlock(session.Engine());
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "post-fade sample " << i;
    }
}

TEST(BusMuteSolo, SoloScopesAudibilityToTreePathAndUnsoloRestores) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    // master <- A <- B, and C a sibling of A under the master.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));

    std::vector<float> dcB;
    std::vector<float> dcC;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.5f, kBusB, dcB);
    RegisterDcVoiceOnBus(harkentest::ObjectId(2), 0.25f, kBusC, dcC);

    Capture out;
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.75f, out.left[kBlockFrames - 1], 1e-5f);

    // Solo the leaf B: its ancestor chain (A, master) stays audible so the
    // signal reaches the output; the off-path C is silenced.
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusB, 1, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.5f, out.left[kBlockFrames - 1], 1e-5f);

    // A second solo widens the audible set: both contributions return.
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusC, 1, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.75f, out.left[kBlockFrames - 1], 1e-5f);

    // Dropping one solo narrows again; clearing the last restores everything.
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusC, 0, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.5f, out.left[kBlockFrames - 1], 1e-5f);
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusB, 0, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.75f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusMuteSolo, MuteWinsInsideASoloPath) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));

    std::vector<float> dcA;
    std::vector<float> dcC;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.5f, kBusA, dcA);
    RegisterDcVoiceOnBus(harkentest::ObjectId(2), 0.25f, kBusC, dcC);

    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 0));
    Capture out;
    out = PumpBlock(session.Engine());
    // A is muted (mute wins over its own solo) and C is off the solo path:
    // the mix is silent.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "sample " << i;
    }

    // Unmuting inside the still-active solo brings only A back.
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 0, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.5f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusMuteSolo, SendFedPathDoesNotFollowSolo) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.3f, kBusA, dc);
    Capture out;
    out = PumpBlock(session.Engine());
    // Dry through A plus the unity send copy through B: double.
    ASSERT_NEAR(0.6f, out.left[kBlockFrames - 1], 1e-5f);

    // Solo A: B still receives the send scatter, but B is off every tree
    // solo path, so its own gain stage silences the copy.
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 0));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.3f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusMuteSolo, RefusalMatrixAndBenignNoOps) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));

    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusMute(kBusA, 2, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusSolo(kBusA, 2, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusMute(harkentest::ObjectId(999), 1, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusSolo(harkentest::ObjectId(999), 1, 0));

    // Setting the held state is a benign no-op.
    EXPECT_EQ(VN_OK, Vn_SetBusMute(kBusA, 0, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 0, 0));

    // The master is accepted by both calls.
    EXPECT_EQ(VN_OK, Vn_SetBusMute(VN_BUS_MASTER, 1, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusMute(VN_BUS_MASTER, 0, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusSolo(VN_BUS_MASTER, 1, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusSolo(VN_BUS_MASTER, 0, 0));
}

TEST(BusMuteSolo, BackpressureRefusesWholeCallWithZeroSideEffects) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.commandQueueDepth = 2; // rounds to 2; easy to fill
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();

    // Fill the command queue (no pump between).
    VnResult r = VN_OK;
    int pushed = 0;
    while ((r = Vn_SetBusVolume(kBusA, -1.0f, 0)) == VN_OK && pushed < 64) {
        ++pushed;
    }
    ASSERT_EQ(VN_BACKPRESSURE, r);

    // A mute that needs a command refuses whole; the flag did not flip.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusMute(kBusA, 1, 0));
    // A solo whose flip set needs commands (B leaves the audible set)
    // refuses whole; no solo state landed.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusSolo(kBusA, 1, 0));
    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    EXPECT_EQ(0u, s.busMuteMask);
    EXPECT_EQ(0x7u, s.busSoloEffectiveMask); // master + A + B all audible

    // A block drains the queue; the same calls now land.
    PumpAndCapture(session.Engine(), 1, warm);
    EXPECT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 0));
    PumpAndCapture(session.Engine(), 1, warm);
    EXPECT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 0));
}

TEST(BusMuteSolo,
     VariableOwnerBlocksExactVolumeMuteAndSoloFlipsWithoutMutation) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::Engine* engine = session.Engine();
    const std::uint32_t busA = engine->buses.FindIndex(kBusA);
    const std::uint32_t busB = engine->buses.FindIndex(kBusB);
    ASSERT_NE(harken::kInvalidBusIndex, busA);
    ASSERT_NE(harken::kInvalidBusIndex, busB);
    const harken::VariableSliceRef owner{23u, 5u};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        busA, harken::kBusTargetBusVolume, owner,
        engine->busTargets.TargetRevision(
            busA, harken::kBusTargetBusVolume)));
    const float slotBefore = engine->busTargets.SlotValue(
        busA, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex);
    const std::uint32_t rampBefore =
        engine->busTargets.LastPublishedRampMs(
            busA, harken::kBusTargetBusVolume);
    const std::uint64_t commandHead = engine->commands.PublishedHead();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusVolume(kBusA, -6.0f, 19));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(slotBefore, engine->busTargets.SlotValue(
                                          busA,
                                          harken::kBusTargetBusVolume,
                                          harken::BusTargetStore::
                                              kSlotApiDirectIndex));
    EXPECT_EQ(rampBefore, engine->busTargets.LastPublishedRampMs(
                              busA, harken::kBusTargetBusVolume));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(kBusB, -3.0f, 7));
    EXPECT_EQ(commandHead + 1u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(-3.0f, engine->busTargets.SlotValue(
                               busB, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusMute(kBusA, 1, 11));
    EXPECT_EQ(0u, engine->busMuted[busA]);
    EXPECT_EQ(commandHead + 1u, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        busA, harken::kBusTargetBusVolume));

    engine->busTargets.ReleaseClaim(
        busA, harken::kBusTargetBusVolume, owner);
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 11));
    EXPECT_EQ(1u, engine->busMuted[busA]);
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 0, 0));
    EXPECT_EQ(0u, engine->busMuted[busA]);

    ASSERT_TRUE(engine->busTargets.TryClaim(
        busB, harken::kBusTargetBusVolume, owner,
        engine->busTargets.TargetRevision(
            busB, harken::kBusTargetBusVolume)));
    const std::uint64_t soloHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusSolo(kBusA, 1, 13));
    EXPECT_EQ(0u, engine->busSoloed[busA]);
    EXPECT_EQ(0u, engine->busSoloActiveCount);
    EXPECT_EQ(soloHead, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        busB, harken::kBusTargetBusVolume));
    engine->busTargets.ReleaseClaim(
        busB, harken::kBusTargetBusVolume, owner);
    EXPECT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 13));
    EXPECT_EQ(1u, engine->busSoloed[busA]);
    EXPECT_EQ(1u, engine->busSoloActiveCount);
    EXPECT_EQ(soloHead + 1u, engine->commands.PublishedHead());
}

TEST(BusMuteSolo, SnapshotMasksReportMuteAndEffectiveAudibility) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    // Dense indices: master 0, then creation order A=1, B=2, C=3.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));

    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    EXPECT_EQ(0u, s.busMuteMask);
    EXPECT_EQ(0xFu, s.busSoloEffectiveMask); // all four audible

    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 1, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusB, 1, 0));
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    EXPECT_EQ(0x2u, s.busMuteMask); // A muted
    // Solo path of B = {B, its ancestors A and master}; A drops out again
    // by mute; C is off-path. Audible: master (bit 0) and B (bit 2).
    EXPECT_EQ(0x5u, s.busSoloEffectiveMask);

    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusB, 0, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusMute(kBusA, 0, 0));
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    EXPECT_EQ(0u, s.busMuteMask);
    EXPECT_EQ(0xFu, s.busSoloEffectiveMask);
}

} // namespace
