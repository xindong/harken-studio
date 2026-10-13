// Live bus reparent: rendered-output proof that a reparented tree mixes
// identically to the same shape built from scratch, that the moved bus's
// volume state and live sends travel with it, plus the refusal matrix
// (master, self, unknown, combined-graph cycles), the benign no-op, solo
// audibility following the new tree paths, and backpressure atomicity.
//
// Tolerances follow BusMixGoldenTest: DC through <= 3 gain stages stays
// within 1e-6 of the closed form; 1e-5 leaves headroom while failing on any
// wrong topology (a missed or doubled gain level shifts the sum by a factor
// of 2 here).
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

std::uint64_t PlanPublishCount() {
    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&s));
    return s.topologyPlanCount;
}

TEST(BusReparent, ReparentedTreeRendersLikeTheFromScratchShape) {
    // Reparented run: build master <- A (half) and master <- B (half), route
    // a voice to B, then move B under A so its gain chain gains A's level.
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, kHalfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusB, kHalfDb, 0));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.8f, kBusB, dc);
    Capture out = PumpBlock(session.Engine());
    // Sibling shape: 0.8 * 0.5 (B) into the master at unity.
    ASSERT_NEAR(0.4f, out.left[kBlockFrames - 1], 1e-5f);

    ASSERT_EQ(VN_OK, Vn_ReparentBus(kBusB, kBusA));
    out = PumpBlock(session.Engine());
    // Chained shape: 0.8 * 0.5 (B) * 0.5 (A) — exactly what
    // BusMixGolden.ThreeLayerChainGainsMultiplyPerLevel renders for a tree
    // built as master <- A <- B from scratch. B's volume state traveled.
    ASSERT_NEAR(0.2f, out.left[kBlockFrames - 1], 1e-5f);

    // And back: the sibling shape returns exactly.
    ASSERT_EQ(VN_OK, Vn_ReparentBus(kBusB, VN_BUS_MASTER));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.4f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusReparent, LiveSendKeepsScatteringAcrossAReparent) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusC, 0.0f));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.3f, kBusA, dc);
    Capture out = PumpBlock(session.Engine());
    // Dry through A plus the unity send copy through C: double.
    ASSERT_NEAR(0.6f, out.left[kBlockFrames - 1], 1e-5f);

    // Moving A under B must keep the A->C send edge and its level: the dry
    // path re-routes (A -> B -> master at unity gains, same sum) and the
    // send copy still scatters into C.
    ASSERT_EQ(VN_OK, Vn_ReparentBus(kBusA, kBusB));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.6f, out.left[kBlockFrames - 1], 1e-5f);
}

TEST(BusReparent, RefusalMatrixAndBenignNoOp) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA));

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReparentBus(harkentest::ObjectId(999), kBusA));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReparentBus(kBusA, harkentest::ObjectId(999)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReparentBus(VN_BUS_MASTER, kBusA));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReparentBus(kBusA, kBusA));
    // Tree cycle: A under its own descendant B.
    EXPECT_EQ(VN_SEND_CYCLE, Vn_ReparentBus(kBusA, kBusB));

    // The benign no-op returns VN_OK and republishes nothing.
    const std::uint64_t published = PlanPublishCount();
    EXPECT_EQ(VN_OK, Vn_ReparentBus(kBusB, kBusA));
    EXPECT_EQ(published, PlanPublishCount());

    // Every refusal above left the tree untouched: a real reparent still
    // publishes exactly one plan.
    EXPECT_EQ(VN_OK, Vn_ReparentBus(kBusB, VN_BUS_MASTER));
    EXPECT_EQ(published + 1, PlanPublishCount());
}

TEST(BusReparent, SendLoopClosedByReparentRefuses) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    // Send B -> A: B feeds A, A feeds the master — acyclic as siblings.
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusB, kBusA, 0.0f));

    // Reparenting A under B adds the tree edge A -> B while the send already
    // carries B -> A: a cycle through the COMBINED graph that no pure tree
    // check would catch (B is not a descendant of A). Refused with the tree
    // untouched.
    const std::uint64_t published = PlanPublishCount();
    EXPECT_EQ(VN_SEND_CYCLE, Vn_ReparentBus(kBusA, kBusB));
    EXPECT_EQ(published, PlanPublishCount());

    // Dropping the send edge clears the loop: the same reparent now lands.
    // (Pump + update first: the un-pumped structural runs above hold every
    // plan slot until swaps retire them — the documented retry.)
    ASSERT_EQ(VN_OK, Vn_DisconnectBusSend(kBusB, kBusA));
    Capture warm = PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_ReparentBus(kBusA, kBusB));
}

TEST(BusReparent, SoloAudibleSetFollowsTheNewTreePath) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));

    std::vector<float> dc;
    RegisterDcVoiceOnBus(harkentest::ObjectId(1), 0.5f, kBusB, dc);

    // Solo A: B is off the solo path and silent.
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 0));
    Capture out = PumpBlock(session.Engine());
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "sample " << i;
    }

    // Moving B under the soloed A puts it inside the solo subtree: audible
    // again in the same call that republished the tree.
    ASSERT_EQ(VN_OK, Vn_ReparentBus(kBusB, kBusA));
    out = PumpBlock(session.Engine());
    ASSERT_NEAR(0.5f, out.left[kBlockFrames - 1], 1e-5f);

    // And moving it back out silences it again.
    ASSERT_EQ(VN_OK, Vn_ReparentBus(kBusB, VN_BUS_MASTER));
    out = PumpBlock(session.Engine());
    ASSERT_EQ(0.0f, out.left[kBlockFrames - 1]);
}

TEST(BusReparent,
     VariableOwnerBlocksAProspectiveSurvivingSoloGainFlip) {
    EngineSession session(MakeNullConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(kBusA, 1, 0));
    PumpBlock(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::Engine* engine = session.Engine();
    const std::uint32_t busA = engine->buses.FindIndex(kBusA);
    const std::uint32_t busB = engine->buses.FindIndex(kBusB);
    ASSERT_NE(harken::kInvalidBusIndex, busA);
    ASSERT_NE(harken::kInvalidBusIndex, busB);
    ASSERT_EQ(0u, engine->buses.ParentOf(busB));
    const harken::VariableSliceRef owner{29u, 3u};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        busB, harken::kBusTargetBusVolume, owner,
        engine->busTargets.TargetRevision(
            busB, harken::kBusTargetBusVolume)));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t planCount = PlanPublishCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReparentBus(kBusB, kBusA));
    EXPECT_EQ(0u, engine->buses.ParentOf(busB));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(planCount, PlanPublishCount());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        busB, harken::kBusTargetBusVolume));

    engine->busTargets.ReleaseClaim(
        busB, harken::kBusTargetBusVolume, owner);
    EXPECT_EQ(VN_OK, Vn_ReparentBus(kBusB, kBusA));
    EXPECT_EQ(busA, engine->buses.ParentOf(busB));
    EXPECT_EQ(planCount + 1u, PlanPublishCount());
}

TEST(BusReparent, BackpressureRefusesWholeCallWithZeroSideEffects) {
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

    // The reparent refuses whole; the logic-side parent did not move, so a
    // later drain + reparent publishes from the ORIGINAL tree.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReparentBus(kBusB, kBusA));
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    // Still a sibling of A: reparenting onto the master is the benign no-op
    // (parent unchanged), proving the refused call mutated nothing.
    const std::uint64_t published = PlanPublishCount();
    EXPECT_EQ(VN_OK, Vn_ReparentBus(kBusB, VN_BUS_MASTER));
    EXPECT_EQ(published, PlanPublishCount());
}

} // namespace
