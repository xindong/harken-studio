// Bus-graph sends and the topology plan: bus->bus and voice->bus send mixing,
// the post-pan voice-send tap, cycle refusal over the combined tree+send graph,
// feeds-first plan order (a send source created before its receiver still
// evaluates first), level changes republishing nothing, structural-change
// atomicity (plan-pool and command-admission shortages), and park/return
// round-tripping a voice's sends.
//
// Signals are analytic DC so the expected mix is closed-form. A DC mono voice
// routed at unity produces exactly its source value from block 0 (proven by the
// baseline bus-routing test), so exact-equality assertions are used where the only
// arithmetic is unity sums; dB-level cases compute the expected value with the
// same pow() the engine uses and compare with a tight tolerance.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlockFrames = 64;
constexpr VnBusId kBusA = harkentest::ObjectId(10);
constexpr VnBusId kBusB = harkentest::ObjectId(11);
constexpr VnBusId kBusC = harkentest::ObjectId(12);
constexpr VnBusId kBusD = harkentest::ObjectId(13);

// Registers a DC mono source under id 1 and returns its owned samples (kept
// alive by the caller because COPY policy copies at registration anyway).
std::vector<float> RegisterDc(float value) {
    std::vector<float> dc = MakeDC(kBlockFrames, value);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    return dc;
}

class BusSendSlotReconcile : public ::testing::Test {
protected:
    void SetUp() override {
        VnConfig config = MakeNullConfig(1, kBlockFrames, 48000);
        config.maxBuses = 4;
        config.busSendSlotsPerBus = 2;
        config.topologyPlanPoolCapacity = 8;
        session = std::make_unique<EngineSession>(config);
        ASSERT_EQ(VN_OK, session->InitResult());
        engine = session->Engine();
        ASSERT_NE(nullptr, engine);
        ASSERT_EQ(VN_OK, engine->buses.CreateBus(kBusA, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, engine->buses.CreateBus(kBusB, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, engine->buses.CreateBus(kBusC, VN_BUS_MASTER));
    }

    std::uint32_t BuildPlan(std::uint32_t slot0, std::uint32_t slot1,
                            bool applyPrepared = false,
                            float preparedLevel = 0.0f) {
        const std::uint32_t plan = engine->topologyPlans.Acquire();
        EXPECT_NE(harken::kInvalidTopologyPlanIndex, plan);
        if (plan == harken::kInvalidTopologyPlanIndex) {
            return plan;
        }
        std::uint32_t* order = engine->topologyPlans.OrderData(plan);
        order[0] = 1; // source before both possible receivers
        order[1] = 2;
        order[2] = 3;
        order[3] = 0; // master last
        engine->topologyPlans.SetOrderCount(plan, 4);
        engine->topologyPlans.SetSlotCount(plan, 4);
        std::uint32_t* targets =
            engine->topologyPlans.SendTargetsRow(plan, 1);
        targets[0] = slot0;
        targets[1] = slot1;
        if (applyPrepared) {
            engine->topologyPlans.PreparedSendLevelsRow(plan, 1)[0] =
                preparedLevel;
            engine->topologyPlans.SendLevelApplyRow(plan, 1)[0] = 1;
        }
        return plan;
    }

    std::vector<float> RenderSourceBlock() {
        constexpr std::uint32_t kBusCount = 4;
        constexpr std::uint32_t kChannels = 2;
        std::vector<float> storage(
            static_cast<std::size_t>(kBusCount) * kChannels * kBlockFrames,
            0.0f);
        std::vector<float*> planes(kBusCount * kChannels);
        for (std::uint32_t bus = 0; bus < kBusCount; ++bus) {
            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                const std::size_t plane = bus * kChannels + channel;
                planes[plane] = storage.data() +
                                plane *
                                    static_cast<std::size_t>(kBlockFrames);
            }
        }
        std::fill_n(planes[2], kBlockFrames, 1.0f);
        std::fill_n(planes[3], kBlockFrames, 1.0f);
        engine->buses.MixDown(planes.data(), kBlockFrames, 0);
        return std::vector<float>(planes[0], planes[0] + kBlockFrames);
    }

    std::unique_ptr<EngineSession> session;
    harken::Engine* engine = nullptr;
};

TEST_F(BusSendSlotReconcile, SameSlotAndTargetPreserveActiveRamp) {
    const std::uint32_t initial = BuildPlan(2, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, initial);
    engine->buses.ApplyTopologyPlan(initial, 1, 2, 0.0f);

    const float halfDb = 20.0f * std::log10(0.5f);
    engine->buses.ApplySetBusSendLevel(1, 2, halfDb, 2);
    const std::vector<float> first = RenderSourceBlock();
    ASSERT_NEAR(1.75f, first.back(), 1e-5f);

    const std::uint32_t republish = BuildPlan(2, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, republish);
    engine->buses.ApplyTopologyPlan(
        republish, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0.0f);
    const std::vector<float> second = RenderSourceBlock();
    EXPECT_NEAR(1.5f, second.back(), 1e-5f);
}

TEST_F(BusSendSlotReconcile, EmptyAuthoritativeSlotClearsEdge) {
    const float halfDb = 20.0f * std::log10(0.5f);
    const std::uint32_t initial = BuildPlan(2, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, initial);
    engine->buses.ApplyTopologyPlan(initial, 1, 2, halfDb);
    ASSERT_NEAR(1.5f, RenderSourceBlock().back(), 1e-5f);

    const std::uint32_t cleared =
        BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, cleared);
    engine->buses.ApplyTopologyPlan(
        cleared, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0.0f);
    EXPECT_FLOAT_EQ(1.0f, RenderSourceBlock().back());
}

TEST_F(BusSendSlotReconcile, ChangedSlotTargetResetsInsteadOfFollowingTarget) {
    const std::uint32_t initial = BuildPlan(2, 3);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, initial);
    engine->buses.ApplyTopologyPlan(initial, 1, 2, 0.0f);
    engine->buses.ApplySetBusSendLevel(
        1, 2, 20.0f * std::log10(0.5f), 0);
    engine->buses.ApplySetBusSendLevel(
        1, 3, 20.0f * std::log10(0.25f), 0);
    ASSERT_NEAR(1.75f, RenderSourceBlock().back(), 1e-5f);

    // Target 3 moves from slot 1 to slot 0. Slot 0 is authoritative, so this
    // is a target replacement and receives a fresh unity edge rather than
    // carrying target 3's old quarter-gain state across slots.
    const std::uint32_t replaced = BuildPlan(3, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, replaced);
    engine->buses.ApplyTopologyPlan(
        replaced, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0.0f);
    EXPECT_FLOAT_EQ(2.0f, RenderSourceBlock().back());
}

TEST_F(BusSendSlotReconcile, PreparedLevelSnapsAndClearsRampBookkeeping) {
    const std::uint32_t initial = BuildPlan(2, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, initial);
    engine->buses.ApplyTopologyPlan(initial, 1, 2, 0.0f);
    engine->buses.ApplySetBusSendLevel(
        1, 2, 20.0f * std::log10(0.25f), 3);
    ASSERT_NEAR(1.75f, RenderSourceBlock().back(), 1e-5f);

    const std::uint32_t prepared =
        BuildPlan(2, harken::kEmptySendTarget, true, 0.4f);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, prepared);
    engine->buses.ApplyTopologyPlan(
        prepared, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0.0f);
    const std::uint32_t republish = BuildPlan(2, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, republish);
    engine->buses.ApplyTopologyPlan(
        republish, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0.0f);

    for (int block = 0; block < 4; ++block) {
        const std::vector<float> output = RenderSourceBlock();
        for (float sample : output) {
            ASSERT_NEAR(1.4f, sample, 1e-5f) << "block " << block;
        }
    }
}

TEST_F(BusSendSlotReconcile, LegacyConnectPayloadStillSnapsInitialLevel) {
    const std::uint32_t plan = BuildPlan(2, harken::kEmptySendTarget);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
    engine->buses.ApplyTopologyPlan(
        plan, 1, 2, 20.0f * std::log10(0.5f));

    const std::vector<float> output = RenderSourceBlock();
    for (float sample : output) {
        ASSERT_NEAR(1.5f, sample, 1e-5f);
    }
}

// A bus->bus send at unity adds the source bus's finalized output to the target
// bus on top of the tree path, so the master hears both copies.
TEST(BusSend, BusSendAddsDryPlusSendPath) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(0.5f);

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    // A -> B at unity: B receives A's finalized output (0.5) on top of its own.
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // master = A (0.5, tree) + B (0.5 received via send, tree) = 1.0.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_FLOAT_EQ(1.0f, out.left[i]) << "sample " << i;
        ASSERT_FLOAT_EQ(1.0f, out.right[i]) << "sample " << i;
    }
}

// The send level scales the copy; the dB math is exact against pow().
TEST(BusSend, BusSendLevelScalesTheCopy) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(0.5f);

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    const float levelDb = -6.0206f; // ~0.5 linear
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, levelDb));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    const float sendLinear = std::pow(10.0f, levelDb / 20.0f);
    const float expected = 0.5f + 0.5f * sendLinear;
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(expected, out.left[i], 1e-5f) << "sample " << i;
    }
}

TEST(BusSend, VoiceOutputVolumeRampsOnlyTheMainOutput) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    ASSERT_NEAR(1.0f, warm.left.back(), 1e-5f);

    const float halfDb = 20.0f * std::log10(0.5f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, halfDb, 2));
    Capture ramp;
    PumpAndCapture(session.Engine(), 2, ramp);
    for (std::size_t i = 0; i < ramp.left.size(); ++i) {
        const float progress = std::min(1.0f, static_cast<float>(i + 1) / 96.0f);
        const float expected = 0.5f + 0.5f * (1.0f - 0.5f * progress);
        ASSERT_NEAR(expected, ramp.left[i], 1e-5f);
        ASSERT_NEAR(expected, ramp.right[i], 1e-5f);
    }
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, -120.0f, 0));
    Capture muted;
    PumpAndCapture(session.Engine(), 1, muted);
    ASSERT_NEAR(0.5f, muted.left.back(), 1e-5f);
}

TEST(BusSend, BusOutputVolumeRampsFromUnityAndMasterScalesTheFinalMix) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    ASSERT_NEAR(1.0f, warm.left.back(), 1e-5f);

    const float halfDb = 20.0f * std::log10(0.5f);
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, halfDb, 2));
    Capture ramp;
    PumpAndCapture(session.Engine(), 2, ramp);
    for (std::size_t i = 0; i < ramp.left.size(); ++i) {
        const float progress = std::min(1.0f, static_cast<float>(i + 1) / 96.0f);
        const float expected = 0.5f + 0.5f * (1.0f - 0.5f * progress);
        ASSERT_NEAR(expected, ramp.left[i], 1e-5f);
        ASSERT_NEAR(expected, ramp.right[i], 1e-5f);
    }
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, halfDb, 0));
    Capture master;
    PumpAndCapture(session.Engine(), 1, master);
    ASSERT_NEAR(0.375f, master.left.front(), 1e-5f);
    ASSERT_NEAR(0.375f, master.right.back(), 1e-5f);
}

TEST(BusSend, OutputVolumeRefusalsPreserveAcceptedGains) {
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_SetBusOutputVolume(VN_BUS_MASTER, 0.0f, 0));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_SetVoiceOutputVolume(0, 0.0f, 0));
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.commandQueueDepth = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const float halfDb = 20.0f * std::log10(0.5f);
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, halfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, halfDb, 0));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    ASSERT_NEAR(0.125f, warm.left.back(), 1e-5f);

    VnResult result = VN_OK;
    for (int i = 0; i < 64 && result == VN_OK; ++i)
        result = Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0);
    ASSERT_EQ(VN_BACKPRESSURE, result);
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusOutputVolume(harkentest::ObjectId(0), invalid, 0));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(0, invalid, 0));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusOutputVolume(VN_BUS_MASTER, invalid, 0));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(handle, invalid, 0));
    }
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusOutputVolume(kBusD, 0.0f, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceOutputVolume(0, 0.0f, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusOutputVolume(VN_BUS_MASTER, 800.0f, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(handle, 800.0f, 0));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetBusOutputVolume(VN_BUS_MASTER, 0.0f, 0));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoiceOutputVolume(handle, 0.0f, 0));
    Capture refused;
    PumpAndCapture(session.Engine(), 1, refused);
    ASSERT_NEAR(0.125f, refused.left.back(), 1e-5f);
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, 0.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, 0.0f, 0));
    Capture retried;
    PumpAndCapture(session.Engine(), 1, retried);
    ASSERT_NEAR(0.5f, retried.left.front(), 1e-5f);
    EXPECT_EQ(VN_OK, Vn_SetBusOutputVolume(VN_BUS_MASTER, 600.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, 600.0f, 0));
}

TEST(BusSend, PendingOutputVolumeBypassesFullQueueAndSnapsAtOnset) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.commandQueueDepth = 16;
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
    VnResult result = VN_OK;
    for (int i = 0; i < 64 && result == VN_OK; ++i)
        result = Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0);
    ASSERT_EQ(VN_BACKPRESSURE, result);
    const auto published = session.Engine()->commands.PublishedHead();
    const auto resolved = session.Engine()->ledger.Classify(handle);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending, resolved.kind);
    const float pendingOutput =
        session.Engine()->pendingStarts[resolved.ledgerSlot].outputBusVolumeDb;
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceOutputVolume(handle, 800.0f, 0));
    EXPECT_EQ(published, session.Engine()->commands.PublishedHead());
    EXPECT_FLOAT_EQ(
        pendingOutput,
        session.Engine()->pendingStarts[resolved.ledgerSlot].outputBusVolumeDb);
    const float halfDb = 20.0f * std::log10(0.5f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, halfDb, 1000));
    EXPECT_EQ(published, session.Engine()->commands.PublishedHead());
    Capture staged;
    PumpAndCapture(session.Engine(), 1, staged);
    EXPECT_FLOAT_EQ(0.0f, staged.left.back());
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
    Capture started;
    PumpAndCapture(session.Engine(), 1, started);
    for (std::size_t i = 0; i < started.left.size(); ++i) {
        ASSERT_NEAR(0.75f, started.left[i], 1e-5f);
        ASSERT_NEAR(0.75f, started.right[i], 1e-5f);
    }
}

TEST(BusSend, OutputVolumesScaleEachMainEdgeWithoutScalingSends) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 5;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto dc = RegisterDc(0.5f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, kBusC));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    const float halfDb = 20.0f * std::log10(0.5f);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(kBusA, halfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, halfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusC, halfDb, 0));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.volumeDb = halfDb;
    play.busId = kBusA;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, halfDb, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    for (std::size_t i = 0; i < out.left.size(); ++i) {
        ASSERT_NEAR(0.328125f, out.left[i], 1e-5f);
        ASSERT_NEAR(0.328125f, out.right[i], 1e-5f);
    }
}

// Feeds-first plan order: a send source created BEFORE its receiver (lower dense
// index) must still evaluate first, which the descending-index fold gets wrong
// (it would finalize the higher-index receiver before the source scattered into
// it, dropping the send). Correct audio here requires the published plan.
TEST(BusSend, PlanOrderEvaluatesSourceBeforeReceiver) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(0.5f);

    // Source S first (index 1), receiver R after (index 2): descending order
    // would finalize R before S.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER)); // S
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER)); // R
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f)); // S -> R at unity

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA; // voice into the source bus
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    // Correct: S (0.5) folds to master and sends 0.5 into R; R (0.5) folds to
    // master. master = 1.0. Descending order would drop the send and give 0.5.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_FLOAT_EQ(1.0f, out.left[i]) << "sample " << i;
    }
}

// A voice send carries the IDENTICAL post-pan/post-width stereo block, scaled by
// the send level — not a pre-pan (equal-channel) copy and not a dropped edge.
// Proven by comparing two otherwise-identical renders of a hard-panned 3D voice:
// without the send the master hears one dry copy; with a unity send into a
// sibling bus it hears exactly two copies of the same panned block, so every
// sample of both channels doubles. A pre-pan copy would not double the panned
// channels by the same factor as the dry ones; a disabled send would not double
// at all.
TEST(BusSend, VoiceSendCarriesPostPanImage) {
    // Renders a 3D voice hard-panned right (listener at origin facing -Z, source
    // at +X) into bus A, optionally sending a unity copy into sibling bus B; both
    // A and B fold to master at unity. Captures one settled block into `out`.
    auto renderPanned = [](bool withSend, Capture& out, float outputDb = 0.0f,
                           float amplitude = 0.25f) {
        VnConfig config = MakeNullConfig(2, kBlockFrames);
        config.maxBuses = 4;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        // Keep the linear send comparison below the final device ceiling.
        std::vector<float> dc = MakeDC(kBlockFrames, amplitude);
        const VnPCMDesc desc = MonoDesc(dc, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));

        VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 10.0f, 0.0f, 0.0f);
        play.busId = kBusA;
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        if (withSend) {
            ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
        }
        ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, outputDb, 0));
        // Commit the spatializer pan before rendering, so the dry block (and
        // therefore its send copy) is actually panned.
        Vn_Update();
        PumpAndCapture(session.Engine(), 1, out);
    };

    Capture dryOnly;
    renderPanned(false, dryOnly);
    HARKENTEST_ASSERT_NON_SILENT(dryOnly.right);
    Capture withSend;
    renderPanned(true, withSend);
    HARKENTEST_ASSERT_NON_SILENT(withSend.right);
    Capture scaledOutput;
    renderPanned(true, scaledOutput, -6.0206f);
    Capture clippedOutput;
    renderPanned(true, clippedOutput, 0.0f, 0.5f);

    // Guard against a vacuous pass: the voice must be genuinely panned, not
    // centered, or "both channels doubled" would hold trivially.
    ASSERT_GT(dryOnly.right[kBlockFrames / 2],
              dryOnly.left[kBlockFrames / 2] + 0.1f);
    // A unity send adds an identical post-pan copy of each channel, so both
    // channels double exactly, sample for sample: x + x == 2*x is exact in IEEE
    // float. A pre-pan equal-channel copy would add the same value to both
    // channels and fail this on the panned channel; a dropped send would not
    // double at all.
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_NEAR(0.25f * std::sqrt(2.0f), dryOnly.right[i], 1e-6f);
        ASSERT_FLOAT_EQ(2.0f * dryOnly.left[i], withSend.left[i])
            << "left sample " << i;
        ASSERT_FLOAT_EQ(2.0f * dryOnly.right[i], withSend.right[i])
            << "right sample " << i;
        ASSERT_NEAR(1.5f * dryOnly.left[i], scaledOutput.left[i], 1e-5f);
        ASSERT_NEAR(1.5f * dryOnly.right[i], scaledOutput.right[i], 1e-5f);
        // Two 0.5 sources at the hard-right endpoint exceed unity only at
        // final output; the existing device clamp remains authoritative.
        ASSERT_FLOAT_EQ(0.0f, clippedOutput.left[i]);
        ASSERT_FLOAT_EQ(1.0f, clippedOutput.right[i]);
    }
}

// A cycle through the combined tree+send graph is refused at connect time with
// zero side effects (the edge is not added, no plan is published).
TEST(BusSend, ConnectRefusesDirectSendCycle) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    const std::uint64_t before = [&] {
        VnSnapshot s{};
        s.structSize = sizeof(VnSnapshot);
        Vn_CaptureSnapshot(&s);
        return s.topologyPlanCount;
    }();
    // B -> A would close A -> B -> A.
    EXPECT_EQ(VN_SEND_CYCLE, Vn_ConnectBusSend(kBusB, kBusA, 0.0f));
    VnSnapshot after{};
    after.structSize = sizeof(VnSnapshot);
    Vn_CaptureSnapshot(&after);
    EXPECT_EQ(before, after.topologyPlanCount); // refused: nothing published
    EXPECT_EQ(0u, after.noSendCount);
}

// A cycle formed through a tree edge plus a send edge is likewise refused: B is
// a tree child of A (B feeds A), so a send A -> B closes the loop.
TEST(BusSend, ConnectRefusesTreePlusSendCycle) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, kBusA)); // B's tree parent is A
    EXPECT_EQ(VN_SEND_CYCLE, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
}

// Per-bus send-slot capacity: connecting more than busSendSlotsPerBus distinct
// targets from one bus is refused VN_NO_SEND and counted.
TEST(BusSend, ConnectRefusesWhenSendSlotsExhausted) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 6;
    config.busSendSlotsPerBus = 2;
    config.topologyPlanPoolCapacity = 8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusD, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusC, 0.0f));
    // A's two send slots are full; a third distinct target is refused.
    EXPECT_EQ(VN_NO_SEND, Vn_ConnectBusSend(kBusA, kBusD, 0.0f));
    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    Vn_CaptureSnapshot(&s);
    EXPECT_EQ(1u, s.noSendCount);
}

// A send-level change is not a structural change: it republishes no plan, so the
// publish counter stays static while the level ramps.
TEST(BusSend, SetBusSendLevelRepublishesNothing) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));

    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    Vn_CaptureSnapshot(&s);
    const std::uint64_t published = s.topologyPlanCount;
    ASSERT_GT(published, 0u);

    // Several level changes (snap and ramp) publish nothing.
    EXPECT_EQ(VN_OK, Vn_SetBusSendLevel(kBusA, kBusB, -3.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusSendLevel(kBusA, kBusB, -6.0f, 50));
    EXPECT_EQ(VN_OK, Vn_SetBusSendLevel(kBusA, kBusB, 0.0f, 20));
    Capture out;
    PumpAndCapture(session.Engine(), 4, out);
    Vn_Update();
    Vn_CaptureSnapshot(&s);
    EXPECT_EQ(published, s.topologyPlanCount);
    // A level on a non-edge is refused.
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusSendLevel(kBusB, kBusA, 0.0f, 0));
}

// Create and disconnect each publish exactly one plan (the single-command
// structural-change path), and a disconnect of a missing edge publishes nothing.
TEST(BusSend, CreateAndDisconnectEachPublishOnePlan) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    auto planCount = [&] {
        VnSnapshot s{};
        s.structSize = sizeof(VnSnapshot);
        Vn_CaptureSnapshot(&s);
        return s.topologyPlanCount;
    };

    const std::uint64_t base = planCount();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    EXPECT_EQ(base + 1, planCount());
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    EXPECT_EQ(base + 2, planCount());
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    EXPECT_EQ(base + 3, planCount());
    ASSERT_EQ(VN_OK, Vn_DisconnectBusSend(kBusA, kBusB));
    EXPECT_EQ(base + 4, planCount());
    // Disconnecting a missing edge is a benign no-op that publishes nothing.
    EXPECT_EQ(VN_OK, Vn_DisconnectBusSend(kBusA, kBusB));
    EXPECT_EQ(base + 4, planCount());
}

// Structural-change atomicity: a full plan pool refuses a connect with zero side
// effects, and the same call succeeds after a pump + Vn_Update drains the plan
// retires. The pool default is 4, but only capacity-1 plans are free once one
// plan is live, so a run of connects without a pump exhausts it.
TEST(BusSend, FullPlanPoolRefusesConnectThenSucceedsAfterDrain) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 8;
    config.busSendSlotsPerBus = 8;
    config.topologyPlanPoolCapacity = 2; // 1 live + 1 free at rest
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    // Create the buses first, pumping so their plans retire and the pool
    // returns to one-live/one-free.
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    Capture warm;
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();

    // One free plan slot remains. The first connect takes it; the second finds
    // the pool full and is refused with zero side effects.
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ConnectBusSend(kBusA, kBusC, 0.0f));
    VnSnapshot s{};
    s.structSize = sizeof(VnSnapshot);
    Vn_CaptureSnapshot(&s);
    EXPECT_EQ(1u, s.topologyRefusedCount);

    // Draining plan retires (pump installs the connect plan and retires the
    // superseded one; Vn_Update returns it to the pool) clears the shortage.
    PumpAndCapture(session.Engine(), 1, warm);
    Vn_Update();
    EXPECT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusC, 0.0f));
}

// Command-admission shortage: a full command queue refuses a connect, and the
// same call succeeds after an audio block drains the queue.
TEST(BusSend, FullCommandQueueRefusesConnectThenSucceeds) {
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

    // Fill the command queue with bus-volume commands (no pump between).
    VnResult r = VN_OK;
    int pushed = 0;
    while ((r = Vn_SetBusVolume(kBusA, -1.0f, 0)) == VN_OK && pushed < 64) {
        ++pushed;
    }
    ASSERT_EQ(VN_BACKPRESSURE, r);
    // A connect cannot be admitted either, and left the graph untouched.
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));

    // A block drains the queue; the same connect now succeeds.
    PumpAndCapture(session.Engine(), 1, warm);
    EXPECT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
}

// A voice's sends survive a park/return round-trip: an attenuated-to-inaudible
// voice parks (releasing its render slot), and when it returns its send is
// re-applied, so the send target hears it again.
TEST(BusSend, ParkReturnRoundTripsVoiceSends) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(0.5f);

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));

    // Play on master with an infinite loop (park-eligible) and a unity send to B.
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0; // infinite: park-eligible while inaudible
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));

    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, -6.0206f, 0));
    Capture warm;
    PumpAndCapture(session.Engine(), 2, warm);
    Vn_Update();

    // Drive the voice inaudible: it parks, releasing its render slot.
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, -80.0f, 0));
    PumpAndCapture(session.Engine(), 2, warm);
    Vn_Update();
    VnSnapshot parked{};
    parked.structSize = sizeof(VnSnapshot);
    Vn_CaptureSnapshot(&parked);
    ASSERT_EQ(1u, parked.parkedVoiceCount);

    // Raise it audible again: it returns and its send is re-applied. Pump with
    // interleaved updates so the return issues and the resumed voice settles
    // past its resampler priming.
    ASSERT_EQ(VN_OK, Vn_SetVoiceOutputVolume(handle, -12.0412f, 1000));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(handle, 0.0f, 0));
    Capture settle;
    for (int i = 0; i < 6; ++i) {
        PumpAndCapture(session.Engine(), 1, settle);
        Vn_Update();
    }
    VnSnapshot returned{};
    returned.structSize = sizeof(VnSnapshot);
    Vn_CaptureSnapshot(&returned);
    EXPECT_EQ(0u, returned.parkedVoiceCount);
    EXPECT_EQ(1u, returned.voiceReturnedCount);

    // The send target carries the voice again: the settled mix is the dry copy
    // on master plus the send copy through B, well above the dry-only 0.5.
    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    ASSERT_NEAR(0.625f, out.left[kBlockFrames - 1], 1e-3f);
}

// Clearing a voice send stops its contribution: after the clear the target no
// longer receives the voice.
TEST(BusSend, ClearVoiceSendStopsContribution) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> dc = RegisterDc(0.5f);

    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));

    Capture withSend;
    PumpAndCapture(session.Engine(), 1, withSend);
    ASSERT_NEAR(1.0f, withSend.left[kBlockFrames - 1], 1e-4f); // dry + send

    ASSERT_EQ(VN_OK, Vn_ClearVoiceSend(handle, 0));
    Capture cleared;
    PumpAndCapture(session.Engine(), 1, cleared);
    // Only the dry copy on master remains.
    ASSERT_NEAR(0.5f, cleared.left[kBlockFrames - 1], 1e-4f);
}

} // namespace
