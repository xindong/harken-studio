#include "harken/render/SignalRenderPlan.h"
#include "harken/render/SignalRoutingTypes.h"
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

TEST(BusSend, OverflowRefusalsPreserveFilteredBusAndVoiceSends) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.busSendSlotsPerBus = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto dc = RegisterDc(0.1f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusB, 0, 1000.0f, 0.707f, 1, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusC, 0, 1000.0f, 0.707f, 1, 0));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusB, 0.0f));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
    const auto expectSettled = [&](float expected) {
        Capture out;
        PumpAndCapture(engine, 4, out);
        ASSERT_GE(out.left.size(), kBlockFrames);
        for (std::size_t i = out.left.size() - kBlockFrames;
             i < out.left.size(); ++i) {
            ASSERT_NEAR(expected, out.left[i], 1e-4f);
            ASSERT_NEAR(expected, out.right[i], 1e-4f);
        }
    };
    expectSettled(0.3f);
    for (const float invalid : {800.0f, (std::numeric_limits<float>::max)(),
                               std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
        const auto published = engine->commands.PublishedHead();
        const auto plans = engine->topologyPlanCount;
        EXPECT_EQ(VN_INVALID_ARG, Vn_ConnectBusSend(kBusA, kBusC, invalid));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusSendLevel(kBusA, kBusB, invalid, 0));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSend(handle, 0, kBusC, invalid));
        EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSendLevel(handle, 0, invalid, 0));
        EXPECT_EQ(published, engine->commands.PublishedHead());
        EXPECT_EQ(plans, engine->topologyPlanCount);
        expectSettled(0.3f);
    }
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(kBusA, kBusC, -800.0f));
    ASSERT_EQ(VN_OK, Vn_SetBusSendLevel(kBusA, kBusB, -800.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSendLevel(handle, 0, -800.0f, 0));
    expectSettled(0.1f);
    ASSERT_EQ(VN_OK, Vn_SetBusSendLevel(kBusA, kBusB, 0.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kBusB, 0.0f));
    expectSettled(0.3f);
}

TEST(BusSend, FallingLevelsKeepTheirEndpointAndPolarity) {
    constexpr std::uint32_t kRampFrames = 256;
    for (const bool voiceSend : {false, true}) {
        for (const auto levels : {std::pair{60.0f, -96.0f},
                                  std::pair{720.0f, -1000.0f}}) {
            SCOPED_TRACE(voiceSend);
            SCOPED_TRACE(levels.first);
            VnConfig config = MakeNullConfig(1, kRampFrames);
            config.maxBuses = 3;
            config.busSendSlotsPerBus = 1;
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            const auto dc = RegisterDc(0.5f);
            ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
            ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
            ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -1000.0f, 0));
            auto play = MakePlayDesc(harkentest::ObjectId(1));
            play.busId = kBusA;
            play.loopCount = 0;
            VnVoiceHandle voice = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
            ASSERT_EQ(VN_OK, voiceSend
                ? Vn_SetVoiceSend(voice, 0, kBusB, levels.first)
                : Vn_ConnectBusSend(kBusA, kBusB, levels.first));
            Capture initial;
            PumpAndCapture(session.Engine(), 1, initial);
            ASSERT_EQ(1.0f, initial.left.back());
            ASSERT_EQ(VN_OK, voiceSend
                ? Vn_SetVoiceSendLevel(voice, 0, levels.second, 1)
                : Vn_SetBusSendLevel(kBusA, kBusB, levels.second, 1));
            Capture falling;
            PumpAndCapture(session.Engine(), 2, falling);
            ASSERT_EQ(2u * kRampFrames, falling.left.size());
            const float expected = 0.5f * std::pow(10.0f, levels.second / 20.0f);
            for (std::size_t i = 0; i < kRampFrames; ++i) {
                EXPECT_GE(falling.left[i], expected) << i;
                EXPECT_GE(falling.right[i], expected) << i;
            }
            for (std::size_t i = kRampFrames - 1; i < falling.left.size(); ++i) {
                EXPECT_FLOAT_EQ(expected, falling.left[i]) << i;
                EXPECT_FLOAT_EQ(expected, falling.right[i]) << i;
            }
        }
    }
}

TEST(BusSend, MultiBlockLevelRetargetStartsAtTheCurrentGain) {
    for (const bool voiceSend : {false, true}) {
        SCOPED_TRACE(voiceSend);
        auto config = MakeNullConfig(1, kBlockFrames);
        config.maxBuses = 3;
        config.busSendSlotsPerBus = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto dc = RegisterDc(0.5f);
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusOutputVolume(kBusA, -1000.0f, 0));
        auto play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = kBusA;
        play.loopCount = 0;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, voiceSend ? Vn_SetVoiceSend(voice, 0, kBusB, -12.0f)
                                  : Vn_ConnectBusSend(kBusA, kBusB, -12.0f));
        Capture initial;
        PumpAndCapture(session.Engine(), 1, initial);
        const auto setLevel = [&](float level) {
            return voiceSend ? Vn_SetVoiceSendLevel(voice, 0, level, 4)
                             : Vn_SetBusSendLevel(kBusA, kBusB, level, 4);
        };
        const float start = std::pow(10.0f, -12.0f / 20.0f);
        const float target = std::pow(10.0f, -6.0f / 20.0f);
        ASSERT_EQ(VN_OK, setLevel(-6.0f));
        Capture first;
        PumpAndCapture(session.Engine(), 1, first);
        for (std::size_t i = 0; i < first.left.size(); ++i) {
            const float expected = 0.5f * (start + (target - start) *
                static_cast<float>(i + 1) / (3 * kBlockFrames));
            EXPECT_NEAR(expected, first.left[i], 1.0e-6f);
            EXPECT_EQ(first.left[i], first.right[i]);
        }
        ASSERT_EQ(VN_OK, setLevel(0.0f));
        Capture restarted;
        PumpAndCapture(session.Engine(), 4, restarted);
        const float restart = start + (target - start) / 3;
        for (std::size_t i = 0; i < restarted.left.size(); ++i) {
            const float progress = std::min(1.0f,
                static_cast<float>(i + 1) / (3 * kBlockFrames));
            const float expected = 0.5f * (restart + (1.0f - restart) * progress);
            EXPECT_NEAR(expected, restarted.left[i], 1.0e-6f);
            EXPECT_EQ(restarted.left[i], restarted.right[i]);
        }
    }
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
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSend(handle, 0, kBusB, 800.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSendLevel(handle, 0, 800.0f, 0));
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
    // A neutral bypassed insert preserves the oracle while exercising blending.
    for (const bool withBypass : {false, true}) {
        SCOPED_TRACE(withBypass);
        // Renders a 3D voice hard-panned right (listener at origin facing -Z, source
        // at +X) into bus A, optionally sending a unity copy into sibling bus B; both
        // A and B fold to master at unity. Captures one settled block into `out`.
        auto renderPanned = [withBypass](bool withSend, Capture& out,
                                       float outputDb = 0.0f, float amplitude = 0.25f) {
            VnConfig config = MakeNullConfig(2, kBlockFrames);
            config.maxBuses = 4;
            if (withBypass) config.fxSlotsPerVoice = 1;
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
            if (withBypass) {
                VnGainDesc gain{};
                gain.structSize = sizeof(gain);
                gain.gainDb = 0.0f;
                ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN,
                    static_cast<const VnEffectDesc*>(static_cast<const void*>(&gain))));
                ASSERT_EQ(VN_OK, Vn_SetVoiceEffectBypass(handle, 0, 1));
            }
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

    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSend(handle, 0, kBusB, 800.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetVoiceSendLevel(handle, 0, 800.0f, 0));

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

TEST(BusSend, FormalSignalInstallReachesMixerAndRemovedInputsReleaseSmoothly) {
    auto config = MakeNullConfig(2, kBlockFrames, 48000);
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_5_1;
    config.maxBuses = 4;
    config.topologyPlanPoolCapacity = 8;
    EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto* engine = session.Engine();
    ASSERT_EQ(Vn_CreateBus(kBusA, VN_BUS_MASTER), VN_OK);
    ASSERT_EQ(Vn_CreateBus(kBusB, VN_BUS_MASTER), VN_OK);
    std::array<harken::SignalReceiverV1, 2> receivers{};
    receivers[0].target.stableId = kBusA;
    receivers[1].target.stableId = kBusB;
    std::array<harken::SignalRelationV1, 2> relations{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        relations[i].stableId = harkentest::ObjectId(10000 + i);
        relations[i].source = receivers[1 - i].target;
        relations[i].target = receivers[i].target;
    }
    harken::ContainerWorldInstallViewV1 world;
    world.sourceBodyVintage = harken::kCurrentWorldBodyVintageV20;
    world.signalRelations = relations.data(); world.signalRelationCount = 2;
    world.signalReceivers = receivers.data(); world.signalReceiverCount = 2;
    world.identity.capabilityMask = harken::kWorldCapabilitySignalRelations;
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV7(world);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, world).status, harken::WorldInstallStatus::Ok);
    auto samples = MakeDC(kBlockFrames * 1400, .1f);
    auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(2), &pcm), VN_OK);
    std::fill(samples.begin() + kBlockFrames * 300, samples.begin() + kBlockFrames * 420, .2f);
    ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1), &pcm), VN_OK);
    for (auto bus : {kBusA, kBusB}) {
        auto play = MakePlayDesc(harkentest::ObjectId(bus == kBusA ? 1 : 2));
        play.busId = bus;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);
    }
    ASSERT_EQ(Vn_Update(), VN_OK);
    Capture settled;
    PumpAndCapture(engine, 150, settled);
    const double slope = 1 / (1 / harken::kDuckingBaselineSlope + 1);
    ASSERT_NEAR(settled.left.back(), .2 / (1 + slope), 1e-5);
    // Observe both receivers separately so opposite oscillations cannot cancel
    // in the master sum. Neither feedback relation is removed or muted.
    for (unsigned i = 0; i < 2; ++i) {
        VnDownmixMatrixDesc matrix{};
        matrix.structSize = sizeof(matrix);
        matrix.srcLayout = VN_CHANNEL_LAYOUT_5_1;
        matrix.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
        matrix.coeffs[i * 8 + i] = 1;
        ASSERT_EQ(Vn_SetBusOutputLayout(i == 0 ? kBusA : kBusB, VN_CHANNEL_LAYOUT_STEREO), VN_OK);
        ASSERT_EQ(Vn_SetBusDownmixMatrix(i == 0 ? kBusA : kBusB, &matrix), VN_OK);
    }
    Capture separated;
    PumpAndCapture(engine, 150, separated);
    const float steady = static_cast<float>(.1 / (1 + slope));
    ASSERT_NEAR(separated.left.back(), steady, 1e-5);
    ASSERT_NEAR(separated.right.back(), steady, 1e-5);
    Capture perturbed;
    PumpAndCapture(engine, 120, perturbed);
    Capture restored;
    PumpAndCapture(engine, 500, restored);
    for (const auto* channel : {&perturbed.left, &perturbed.right, &restored.left, &restored.right})
        for (const auto sample : *channel) ASSERT_TRUE(std::isfinite(sample));
    for (std::size_t i = restored.left.size() - 64; i < restored.left.size(); ++i) {
        EXPECT_NEAR(restored.left[i], steady, 1e-5);
        EXPECT_NEAR(restored.right[i], steady, 1e-5);
    }
    // Restore the ordinary stereo output before checking the Release edge.
    for (const auto bus : {kBusA, kBusB}) {
        VnDownmixMatrixDesc matrix{};
        matrix.structSize = sizeof(matrix);
        matrix.srcLayout = VN_CHANNEL_LAYOUT_5_1;
        matrix.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
        matrix.coeffs[0] = matrix.coeffs[9] = 1;
        ASSERT_EQ(Vn_SetBusDownmixMatrix(bus, &matrix), VN_OK);
    }
    Capture resettled;
    PumpAndCapture(engine, 150, resettled);
    world.signalRelations = nullptr; world.signalRelationCount = 0;
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV7(world);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, world).status, harken::WorldInstallStatus::Ok);
    Capture releasing;
    PumpAndCapture(engine, 1, releasing);
    EXPECT_LT(releasing.left.front(), .199f);
    EXPECT_GT(releasing.left.front(), settled.left.back() - 1e-5f);
    Capture released;
    PumpAndCapture(engine, 100, released);
    EXPECT_NEAR(released.left.back(), .2f, 1e-5);
}

TEST_F(BusSendSlotReconcile, SignalFeedbackRunsThroughMixerAndPostOutputVolume) {
    session.reset();
    auto config = MakeNullConfig(2, kBlockFrames, 48000);
    config.maxBuses = 4;
    config.busSendSlotsPerBus = 2;
    config.topologyPlanPoolCapacity = 8;
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    engine = session->Engine();
    ASSERT_EQ(VN_OK, engine->buses.CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, engine->buses.CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, engine->buses.CreateBus(kBusC, VN_BUS_MASTER));
    const auto plan = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    const std::array nodes{
        harken::SignalRenderNodeInput{harken::SignalEndpointKindV1::Bus, true,
            harken::kDuckingDefaultAttackMs, harken::kDuckingDefaultReleaseMs, 1},
        harken::SignalRenderNodeInput{harken::SignalEndpointKindV1::Bus, true,
            harken::kDuckingDefaultAttackMs, harken::kDuckingDefaultReleaseMs, 2}};
    const std::array relations{harken::SignalConnectionInput{0, 1, 1},
        harken::SignalConnectionInput{1, 0, 1}};
    const std::array paths{harken::SignalFeedbackEdge{0, 1, 1, true},
        harken::SignalFeedbackEdge{1, 0, 1, true}};
    auto signals = harken::SignalRenderPlan::Prepare({2, kBlockFrames, 48000,
        VN_CHANNEL_LAYOUT_STEREO}, nodes, relations, paths);
    ASSERT_NE(signals, nullptr);
    engine->topologyPlans.SetSignals(plan, std::move(signals));
    engine->buses.ApplyTopologyPlan(plan, harken::kInvalidBusIndex,
        harken::kInvalidBusIndex, 0);
    const auto samples = MakeDC(kBlockFrames * 300, .1f);
    const auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    for (auto bus : {kBusA, kBusB}) {
        auto play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = bus;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture first;
    PumpAndCapture(engine, 1, first);
    ASSERT_FALSE(first.left.empty());
    EXPECT_NEAR(.2f, first.left.back(), 1e-6);
    Capture settled;
    PumpAndCapture(engine, 150, settled);
    const double slope = 1 / (1 / harken::kDuckingBaselineSlope + 1);
    EXPECT_NEAR(.2 / (1 + slope), settled.left.back(), 1e-5);
    engine->buses.ApplyVolume(1, -96, 0, 0, true);
    Capture muted;
    PumpAndCapture(engine, 100, muted);
    // The muted source no longer sustains the other receiver's attenuation.
    EXPECT_NEAR(.1, muted.left.back(), 1e-4);
    const auto replacement = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    engine->topologyPlans.SetSignals(replacement, engine->topologyPlans.Signals(plan));
    auto* identity = engine->buses.InstalledSignals();
    engine->buses.ApplyTopologyPlan(replacement, harken::kInvalidBusIndex,
        harken::kInvalidBusIndex, 0);
    engine->topologyPlans.DrainRetire();
    EXPECT_EQ(identity, engine->buses.InstalledSignals());
    Capture after;
    PumpAndCapture(engine, 1, after);
    EXPECT_NEAR(muted.left.back(), after.left.back(), 1e-5);
}

TEST_F(BusSendSlotReconcile, ActualOrdinaryRoutingClosesSingleSignalRelation) {
    const auto plan = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    std::array<harken::SignalRenderNodeInput, 4> nodes{};
    for (std::uint32_t i = 0; i < nodes.size(); ++i) nodes[i].busIndex = i;
    nodes[1].receiver = true;
    const std::array relations{harken::SignalConnectionInput{0, 1, 1}};
    const std::array paths{harken::SignalFeedbackEdge{0, 1, 1, true}};
    auto signals = harken::SignalRenderPlan::Prepare({2, kBlockFrames, 48000,
        VN_CHANNEL_LAYOUT_STEREO}, nodes, relations, paths);
    ASSERT_NE(signals, nullptr);
    signals->EnableActualRouting();
    ASSERT_TRUE(engine->mixer.PrepareSignalBacking(*signals));
    engine->topologyPlans.SetSignals(plan, std::move(signals));
    engine->buses.ApplyTopologyPlan(plan, harken::kInvalidBusIndex,
        harken::kInvalidBusIndex, 0);
    const auto samples = MakeDC(kBlockFrames * 500, .1f);
    const auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture settled;
    PumpAndCapture(engine, 160, settled);
    const double slope = 1 / (1 / harken::kDuckingBaselineSlope + 1);
    ASSERT_FALSE(settled.left.empty());
    EXPECT_NEAR(.1 / (1 + slope), settled.left.back(), 1e-5);

    // Change the ordinary route while the previous source block is live.
    engine->buses.ApplyVolume(2, 6.0205999f, 0);
    const auto replacement = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    engine->topologyPlans.ParentsData(replacement)[1] = 2;
    engine->topologyPlans.SetSignals(replacement, engine->topologyPlans.Signals(plan));
    engine->buses.ApplyTopologyPlan(replacement, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    engine->topologyPlans.DrainRetire();
    Capture rerouted;
    PumpAndCapture(engine, 160, rerouted);
    const double changedSlope = 1 / (1 / harken::kDuckingBaselineSlope + 2);
    EXPECT_NEAR(.2 / (1 + 2 * changedSlope), rerouted.left.back(), 1e-5);
}

TEST_F(BusSendSlotReconcile, LateSignalInstallKeepsFiniteWarmFilterAndDelayBounds) {
    const auto first = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    engine->buses.ApplyTopologyPlan(first, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(delay);
    delay.delayMs = 1;
    delay.feedback = .8f;
    delay.wet = .5f;
    delay.dry = 1;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusB, 0, VN_EFFECT_FEEDBACK_DELAY,
        reinterpret_cast<const VnEffectDesc*>(&delay)));
    Capture empty;
    PumpAndCapture(engine, 1, empty);
    ASSERT_NE(engine->buses.BusState(2)->fxSlots[0].state, nullptr);
    const auto initial = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    engine->topologyPlans.ParentsData(initial)[1] = 2;
    engine->buses.ApplyTopologyPlan(initial, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    const auto samples = MakeDC(kBlockFrames * 600, .1f);
    const auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = kBusA;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusB, harken::kFilterKindLowpass, 1000, .707f, 1, 0));
    Capture warm;
    PumpAndCapture(engine, 50, warm);
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(kBusB, 0, harken::kDelayParamFeedback, .2f));
    Capture baseline;
    PumpAndCapture(engine, 50, baseline);
    ASSERT_EQ(VN_OK, Vn_SetBusFilter(kBusB, harken::kFilterKindLowpass, 4000, .707f, 1, 100));
    Capture partial;
    PumpAndCapture(engine, 3, partial);
    ASSERT_TRUE(engine->buses.BusState(2)->busLpf.glide.active);

    const auto plan = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    engine->topologyPlans.ParentsData(plan)[1] = 2;
    std::array<harken::SignalRenderNodeInput, 4> nodes{};
    for (std::uint32_t i = 0; i < nodes.size(); ++i) nodes[i].busIndex = i;
    nodes[1].receiver = true;
    const std::array relations{harken::SignalConnectionInput{0, 1, 1}};
    const std::array paths{harken::SignalFeedbackEdge{0, 1, 1, true}};
    auto signals = harken::SignalRenderPlan::Prepare({2, kBlockFrames, 48000,
        VN_CHANNEL_LAYOUT_STEREO}, nodes, relations, paths);
    ASSERT_NE(signals, nullptr);
    signals->EnableActualRouting();
    ASSERT_TRUE(engine->mixer.PrepareSignalBacking(*signals));
    engine->topologyPlans.SetSignals(plan, std::move(signals));
    engine->buses.ApplyTopologyPlan(plan, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    Capture ducked;
    PumpAndCapture(engine, 250, ducked);
    for (float sample : ducked.left) ASSERT_TRUE(std::isfinite(sample));
    EXPECT_GT(ducked.left.back(), .01f);
    EXPECT_LT(ducked.left.back(), baseline.left.back() * .999f);
    const auto* state = static_cast<const harken::DelayEffectState*>(engine->buses.BusState(2)->fxSlots[0].state);
    ASSERT_NE(state, nullptr);
    EXPECT_FLOAT_EQ(state->feedback, .2f);
    EXPECT_FLOAT_EQ(state->maxFeedbackSinceReset, .8f);
    EXPECT_TRUE(std::isfinite(engine->buses.BusState(2)->busLpf.signalHighWater));
    // Finishing between 16-frame control ticks must not generate an unbounded
    // number of one-frame schedule entries for the rest of this block.
    const auto shortTarget = harken::MakeLowpass(48000, 500, .707f);
    engine->buses.ApplySetBusFilter(2, harken::kFilterKindLowpass, true, 500, .707f,
        shortTarget, harken::EstimateBiquadTailFrames(shortTarget), 15, false);
    Capture shortGlide;
    PumpAndCapture(engine, 1, shortGlide);
    ASSERT_FALSE(shortGlide.left.empty());
    for (float sample : shortGlide.left) EXPECT_TRUE(std::isfinite(sample));
}

TEST_F(BusSendSlotReconcile, NewBusCannotAliasContentNodeInOlderSignalPlan) {
    const auto topology = BuildPlan(harken::kEmptySendTarget, harken::kEmptySendTarget);
    engine->buses.ApplyTopologyPlan(topology, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    std::array<harken::SignalRenderNodeInput, 4> nodes{};
    nodes[0].busIndex = 0;
    nodes[1].busIndex = 1;
    nodes[1].receiver = true;
    nodes[2].kind = harken::SignalEndpointKindV1::Sound;
    nodes[3].kind = harken::SignalEndpointKindV1::Container;
    const std::array relations{harken::SignalConnectionInput{0, 1, 1}};
    const std::array paths{harken::SignalFeedbackEdge{0, 1, 1, true}};
    auto signals = harken::SignalRenderPlan::Prepare({2, kBlockFrames, 48000,
        VN_CHANNEL_LAYOUT_STEREO}, nodes, relations, paths);
    ASSERT_NE(signals, nullptr);
    signals->EnableActualRouting();
    ASSERT_TRUE(signals->BeginBlock({1, 1, 1, 1, 0, kBlockFrames}));
    harken::BlockArena arena(128 * 1024);
    EXPECT_FALSE(signals->ReserveRouting(engine->buses, arena, 0, 0));
    EXPECT_EQ(arena.UsedBytes(), 0u);
    EXPECT_FALSE(signals->BuildRouting({}, engine->buses));
}

TEST_F(BusSendSlotReconcile, ExternalDetectorInjectionCannotInfluenceAnEarlierTap) {
    session.reset();
    auto config = MakeNullConfig(1, kBlockFrames, 48000);
    config.maxBuses = 6;
    config.fxSlotsPerBus = 2;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicBusTapCapacity = 4;
    config.topologyPlanPoolCapacity = 12;
    session = std::make_unique<EngineSession>(config);
    ASSERT_EQ(VN_OK, session->InitResult());
    engine = session->Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusA, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusB, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusD, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBusC, kBusD));
    VnCompressorDesc compressor{};
    compressor.structSize = sizeof(compressor);
    compressor.thresholdDb = -24;
    compressor.ratio = 4;
    compressor.attackMs = 5;
    compressor.releaseMs = 80;
    compressor.kneeDb = 3;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusA, 1, VN_EFFECT_COMPRESSOR,
        reinterpret_cast<const VnEffectDesc*>(&compressor)));
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(kBusB, 0, VN_EFFECT_COMPRESSOR,
        reinterpret_cast<const VnEffectDesc*>(&compressor)));
    ASSERT_EQ(VN_OK, Vn_SetBusFaderPoint(kBusA, 1));
    Capture empty;
    PumpAndCapture(engine, 1, empty);
    ASSERT_EQ(VN_OK, Vn_Update());
    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof(route);
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    route.sourceId = kBusD;
    route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
    ASSERT_EQ(VN_OK, Vn_SetBusEffectDynamicRoute(kBusA, 1, 0, &route));
    route.sourceId = kBusA;
    route.tap = VN_DYNAMIC_TAP_BUS_PRE_FADER;
    ASSERT_EQ(VN_OK, Vn_SetBusEffectDynamicRoute(kBusB, 0, 0, &route));
    PumpAndCapture(engine, 1, empty);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto check = [&](bool feedback) {
        constexpr std::uint32_t endpointNodes = 6;
        std::vector<harken::SignalBusRoutingInfo> buses(engine->buses.AudioBusCount());
        std::vector<harken::SignalFeedbackEdge> edges(engine->buses.SignalRoutingEdgeCapacity() + 1);
        std::vector<harken::SignalBiquadSegment> schedules(buses.size() * 4 * (kBlockFrames / 16 + 2));
        const auto count = engine->buses.SignalRoutingSnapshot(buses, edges, kBlockFrames, schedules, endpointNodes);
        ASSERT_NE(count, UINT32_MAX);
        // B -> C is one-way while A's detector injection is after its outgoing tap.
        edges[count] = {engine->buses.FindIndex(kBusB), engine->buses.FindIndex(kBusC), 1, true};
        edges.resize(count + 1);
        const auto nodeCount = endpointNodes + static_cast<std::uint32_t>(engine->buses.SignalRoutingExtraNodes());
        std::vector<std::byte> scratch(harken::SignalFeedbackPlan::ScratchBytes(nodeCount, edges.size()));
        harken::SignalFeedbackView graph;
        ASSERT_TRUE(harken::SignalFeedbackPlan::Compile(nodeCount, edges, scratch, graph));
        const auto& node = graph.nodes[engine->buses.FindIndex(kBusC)];
        EXPECT_EQ(node.feedbackInput, feedback);
        EXPECT_DOUBLE_EQ(node.slope, feedback ? 0 : harken::kDuckingBaselineSlope);
    };
    check(false);
    route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
    ASSERT_EQ(VN_OK, Vn_SetBusEffectDynamicRoute(kBusB, 0, 0, &route));
    PumpAndCapture(engine, 1, empty);
    check(true);
}

TEST(BusSend, FormalNestedSignalReturnKeepsOrdinarySendWhenOutputIsZero) {
    std::array<Capture, 2> output;
    for (unsigned perturb = 0; perturb < 2; ++perturb) {
        auto config = MakeNullConfig(2, kBlockFrames, 48000);
        config.maxBuses = 4;
        config.topologyPlanPoolCapacity = 16;
        config.mixChannelLayout = VN_CHANNEL_LAYOUT_5_1;
        EngineSession session(config);
        ASSERT_EQ(session.InitResult(), VN_OK);
        auto* engine = session.Engine();
        for (const auto bus : {kBusA, kBusB, kBusC}) ASSERT_EQ(Vn_CreateBus(bus, VN_BUS_MASTER), VN_OK);
        ASSERT_EQ(Vn_ConnectBusSend(kBusA, kBusB, 0), VN_OK);
        ASSERT_EQ(Vn_SetBusOutputVolume(kBusA, -1000, 0), VN_OK);
        for (unsigned i = 0; i < 2; ++i) {
            const auto bus = i == 0 ? kBusB : kBusC;
            ASSERT_EQ(Vn_SetBusOutputLayout(bus, VN_CHANNEL_LAYOUT_STEREO), VN_OK);
            VnDownmixMatrixDesc matrix{};
            matrix.structSize = sizeof(matrix);
            matrix.srcLayout = VN_CHANNEL_LAYOUT_5_1; matrix.dstLayout = VN_CHANNEL_LAYOUT_STEREO;
            matrix.coeffs[i * 8 + i] = 1;
            ASSERT_EQ(Vn_SetBusDownmixMatrix(bus, &matrix), VN_OK);
        }
        std::array<harken::SignalReceiverV1, 3> receivers{};
        receivers[0].target.stableId = kBusA;
        receivers[1].target.stableId = kBusB;
        receivers[2].target.stableId = kBusC;
        std::array<harken::SignalRelationV1, 3> relations{};
        relations[0].source = relations[1].source = receivers[2].target;
        relations[0].target = receivers[0].target;
        relations[1].target = receivers[1].target;
        relations[2].source = receivers[1].target;
        relations[2].target = receivers[2].target;
        for (unsigned i = 0; i < 3; ++i) relations[i].stableId = harkentest::ObjectId(1400 + i);
        harken::ContainerWorldInstallViewV1 world;
        world.sourceBodyVintage = harken::kCurrentWorldBodyVintageV20;
        world.signalRelations = relations.data(); world.signalRelationCount = 3;
        world.signalReceivers = receivers.data(); world.signalReceiverCount = 3;
        world.identity.capabilityMask = harken::kWorldCapabilitySignalRelations;
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV7(world);
        ASSERT_EQ(harken::InstallContainerWorldV1(*engine, world).status, harken::WorldInstallStatus::Ok);
        for (unsigned i = 0; i < 2; ++i) {
            auto samples = MakeDC(64 * 2000, .1f);
            if (perturb && i == 0) std::fill(samples.begin() + 400 * 64, samples.begin() + 460 * 64, .2f);
            const auto desc = MonoDesc(samples, 48000);
            const auto sound = harkentest::ObjectId(1500 + i);
            ASSERT_EQ(Vn_RegisterPCM(sound, &desc), VN_OK);
            auto play = MakePlayDesc(sound); play.busId = i == 0 ? kBusA : kBusC;
            VnVoiceHandle voice = 0;
            ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);
        }
        ASSERT_EQ(Vn_Update(), VN_OK);
        PumpAndCapture(engine, 1800, output[perturb]);
        EXPECT_NEAR(output[perturb].left.back(), .0140523467663895, 2e-5);
        EXPECT_NEAR(output[perturb].right.back(), .0935444242871800, 2e-5);
    }
    for (std::size_t i = output[0].left.size() - 12000; i < output[0].left.size(); ++i) {
        ASSERT_NEAR(output[0].left[i], output[1].left[i], 1e-5);
        ASSERT_NEAR(output[0].right[i], output[1].right[i], 1e-5);
    }
}

TEST(BusSend, FormalFeedbackRecoversThroughActiveEffectsAndParameterTransitions) {
    const auto run = [&](unsigned effect, bool perturb, bool enabled, Capture& output) {
        auto config = MakeNullConfig(1, kBlockFrames, 48000);
        config.maxBuses = 4;
        config.fxSlotsPerBus = 1;
        config.delayEffectPoolCapacity = config.toneEffectPoolCapacity = 1;
        config.gainEffectPoolCapacity = config.limiterEffectPoolCapacity = config.compressorEffectPoolCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(session.InitResult(), VN_OK);
        auto* engine = session.Engine();
        ASSERT_EQ(Vn_CreateBus(kBusB, VN_BUS_MASTER), VN_OK);
        ASSERT_EQ(Vn_CreateBus(kBusA, kBusB), VN_OK);
        if (enabled) {
            VnFeedbackDelayDesc delay{};
            delay.structSize = sizeof(delay); delay.delayMs = 1; delay.feedback = .8f; delay.wet = .5f; delay.dry = 1;
            VnGainDesc gain{};
            gain.structSize = sizeof(gain); gain.gainDb = 3;
            VnThreeBandToneDesc tone{};
            tone.structSize = sizeof(tone); tone.lowShelfGainDb = 6; tone.lowShelfFreqHz = 200;
            tone.peakFreqHz = 1000; tone.peakQ = .707f; tone.highShelfFreqHz = 6000;
            VnLimiterDesc limiter{};
            limiter.structSize = sizeof(limiter); limiter.ceilingDb = -18; limiter.releaseMs = 80;
            VnCompressorDesc compressor{};
            compressor.structSize = sizeof(compressor); compressor.thresholdDb = -24; compressor.ratio = 4;
            compressor.attackMs = 5; compressor.releaseMs = 80; compressor.kneeDb = 3; compressor.makeupDb = 3;
            const VnEffectDesc* descriptors[] = {reinterpret_cast<const VnEffectDesc*>(&delay),
                reinterpret_cast<const VnEffectDesc*>(&gain), reinterpret_cast<const VnEffectDesc*>(&tone),
                reinterpret_cast<const VnEffectDesc*>(&limiter), reinterpret_cast<const VnEffectDesc*>(&compressor)};
            const unsigned types[] = {VN_EFFECT_FEEDBACK_DELAY, VN_EFFECT_GAIN, VN_EFFECT_THREE_BAND_TONE,
                VN_EFFECT_LIMITER, VN_EFFECT_COMPRESSOR};
            ASSERT_EQ(Vn_SetBusEffect(kBusB, 0, types[effect], descriptors[effect]), VN_OK);
            if (effect == 0) {
                ASSERT_EQ(Vn_SetBusFilter(kBusB, harken::kFilterKindLowpass, 1000, .707f, 1, 0), VN_OK);
                ASSERT_EQ(Vn_SetBusOutputFilter(kBusB, harken::kFilterKindLowpass, 2000, .707f, 1, 0), VN_OK);
            }
        }
        auto samples = MakeDC(64 * 3000, .3f);
        if (effect == 2) for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = .1f * static_cast<float>(std::sin(2 * 3.14159265358979323846 * 50 * i / 48000));
        if (perturb) for (unsigned i = 400 * 64; i < 460 * 64; ++i) samples[i] *= 1.5f;
        const auto pcm = MonoDesc(samples, 48000);
        ASSERT_EQ(Vn_RegisterPCM(harkentest::ObjectId(1600), &pcm), VN_OK);
        auto play = MakePlayDesc(harkentest::ObjectId(1600)); play.busId = kBusA;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(64);
        harken::SignalReceiverV1 receiver;
        receiver.target.stableId = kBusA;
        harken::SignalRelationV1 relation;
        relation.stableId = harkentest::ObjectId(1601);
        relation.source.stableId = VN_BUS_MASTER; relation.target = receiver.target;
        harken::ContainerWorldInstallViewV1 world;
        world.sourceBodyVintage = harken::kCurrentWorldBodyVintageV20;
        world.signalRelations = &relation; world.signalRelationCount = 1;
        world.signalReceivers = &receiver; world.signalReceiverCount = 1;
        world.identity.capabilityMask = harken::kWorldCapabilitySignalRelations;
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV7(world);
        ASSERT_EQ(harken::InstallContainerWorldV1(*engine, world).status, harken::WorldInstallStatus::Ok);
        for (unsigned block = 0; block < 1900; ++block) {
            if (enabled && block == 80) {
                const unsigned parameters[] = {harken::kDelayParamFeedback, harken::kGainParamGainDb,
                    harken::kToneParamLowShelfGainDb, harken::kLimiterParamCeilingDb, harken::kCompressorParamMakeupDb};
                const float values[] = {.2f, 6.0205999f, 3, -12, 6};
                ASSERT_EQ(Vn_SetBusEffectParam(kBusB, 0, parameters[effect], values[effect]), VN_OK);
                if (effect == 0) ASSERT_EQ(Vn_SetBusFilter(kBusB, harken::kFilterKindLowpass, 4000, .707f, 1, 10), VN_OK);
            }
            if (enabled && effect == 4 && block == 120)
                ASSERT_EQ(Vn_ResetBusEffect(kBusB, 0, VN_EFFECT_COMPRESSOR), VN_OK);
            if (enabled && effect == 3 && (block == 120 || block == 140))
                ASSERT_EQ(Vn_SetBusEffectBypass(kBusB, 0, block == 120 ? 1u : 0u), VN_OK);
            PumpAndCapture(engine, 1, output);
        }
        for (float value : output.left) ASSERT_TRUE(std::isfinite(value));
        ASSERT_EQ(engine->mixer.Arena().OverflowCount(), 0u);
        if (effect == 0) {
            ASSERT_EQ(Vn_Stop(voice, 0), VN_OK);
            Capture tail;
            PumpAndCapture(engine, 1000, tail);
            for (std::size_t i = tail.left.size() - 12000; i < tail.left.size(); ++i)
                ASSERT_NEAR(tail.left[i], 0, 1e-5);
        }
    };
    for (unsigned effect = 0; effect < 5; ++effect) {
        SCOPED_TRACE(effect);
        Capture reference, perturbed, noEffect;
        ASSERT_NO_FATAL_FAILURE(run(effect, false, true, reference));
        ASSERT_NO_FATAL_FAILURE(run(effect, true, true, perturbed));
        ASSERT_NO_FATAL_FAILURE(run(effect, false, false, noEffect));
        ASSERT_EQ(reference.left.size(), perturbed.left.size());
        ASSERT_EQ(reference.left.size(), noEffect.left.size());
        double squared = 0, activeDifference = 0;
        for (std::size_t i = reference.left.size() - 12000; i < reference.left.size(); ++i) {
            const auto difference = static_cast<double>(reference.left[i]) - perturbed.left[i];
            ASSERT_LE(std::abs(difference), 1e-5);
            squared += difference * difference;
            activeDifference = std::max(activeDifference, std::abs(static_cast<double>(reference.left[i]) - noEffect.left[i]));
        }
        EXPECT_LE(std::sqrt(squared / 12000), 2e-6);
        EXPECT_GT(activeDifference, 1e-4);
    }
}
