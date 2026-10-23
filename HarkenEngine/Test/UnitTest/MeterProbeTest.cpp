// Private MeterProbe authority: install/uninstall lifecycle with exact
// generations, internal probe Binding definition and attachment, sidechain
// waveform reaching the bound target audibly, withdrawal to neutral on
// uninstall and bus destroy, slot reuse under fresh generations, and the
// rejection of probes as Variable or decision inputs.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "Common/VariableOutcomeCapture.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/ParamTargetTable.h"
#include "harken/param/VariableService.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <cmath>
#include <array>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;

constexpr VnSoundId kDcSound = harkentest::ObjectId(901);
constexpr VnBusId kSourceBus = harkentest::ObjectId(21);
constexpr VnBusId kTargetBus = harkentest::ObjectId(22);
constexpr VnObjectId kProbe = harkentest::ObjectId(8001);
constexpr VnObjectId kSidechain = harkentest::ObjectId(8002);
constexpr VnObjectId kSecondProbe = harkentest::ObjectId(8003);
constexpr VnObjectId kSecondSidechain = harkentest::ObjectId(8004);

VnConfig ProbeConfig() {
    VnConfig config = MakeNullConfig(4, 256, 48000);
    config.maxBuses = 8;
    config.maxBindings = 8;
    config.maxBindingAttachments = 8;
    return config;
}

TEST(MeterProbe, PreparedBusRemovalRescansNewProbesAndLeavesUnusedSequenceHoles) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kProbe, kSourceBus,
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    const std::vector<harken::BusSlotRef> refs{engine->buses.FindRef(kSourceBus)};
    const auto firstSequence = engine->logicCommits.Current();
    const auto head = engine->commands.PublishedHead();
    harken::MeterProbeSystem::PreparedBusRemovalV1 abandoned;
    ASSERT_EQ(VN_OK, engine->meterProbes.PrepareBusRemovalV1(*engine, refs, abandoned));
    EXPECT_EQ(firstSequence + 1, engine->logicCommits.Current());
    EXPECT_EQ(harken::MeterSystem::State::Installed, engine->meterDetectors.Find(kProbe)->state);
    EXPECT_TRUE(engine->meterProbes.CanCommitBusRemovalV1(*engine, abandoned));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kSecondProbe, kSourceBus,
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    EXPECT_FALSE(engine->meterProbes.CanCommitBusRemovalV1(*engine, abandoned));
    harken::MeterProbeSystem::PreparedBusRemovalV1 replacement;
    ASSERT_EQ(VN_OK, engine->meterProbes.PrepareBusRemovalV1(*engine, refs, replacement));
    EXPECT_EQ(firstSequence + 3, engine->logicCommits.Current());
    ASSERT_TRUE(engine->meterProbes.CanCommitBusRemovalV1(*engine, replacement));
    static_assert(noexcept(engine->meterProbes.CommitBusRemovalV1(*engine, replacement)));
    engine->meterProbes.CommitBusRemovalV1(*engine, replacement);
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_EQ(harken::MeterSystem::State::Withdrawing, engine->meterDetectors.Find(kProbe)->state);
    EXPECT_EQ(harken::MeterSystem::State::Withdrawing, engine->meterDetectors.Find(kSecondProbe)->state);
    EXPECT_EQ(firstSequence + 2, engine->meterDetectors.Find(kProbe)->withdrawalLogicSequence);
    EXPECT_EQ(firstSequence + 3, engine->meterDetectors.Find(kSecondProbe)->withdrawalLogicSequence);
    EXPECT_FALSE(engine->meterProbes.CanCommitBusRemovalV1(*engine, replacement));
}

TEST(MeterProbe, PreparedBusRemovalRefusesStateDriftAndDoesNotRestartWithdrawal) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    std::uint32_t generation = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kProbe, kSourceBus,
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generation));
    const std::vector<harken::BusSlotRef> refs{engine->buses.FindRef(kSourceBus)};
    harken::MeterProbeSystem::PreparedBusRemovalV1 first;
    ASSERT_EQ(VN_OK, engine->meterProbes.PrepareBusRemovalV1(*engine, refs, first));
    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(*engine, kProbe, generation));
    EXPECT_FALSE(engine->meterProbes.CanCommitBusRemovalV1(*engine, first));
    const auto sequence = engine->meterDetectors.Find(kProbe)->withdrawalLogicSequence;
    const auto issued = engine->logicCommits.Current();
    const auto count = engine->meterProbeWithdrawalCount;
    harken::MeterProbeSystem::PreparedBusRemovalV1 second;
    ASSERT_EQ(VN_OK, engine->meterProbes.PrepareBusRemovalV1(*engine, refs, second));
    ASSERT_TRUE(engine->meterProbes.CanCommitBusRemovalV1(*engine, second));
    engine->meterProbes.CommitBusRemovalV1(*engine, second);
    EXPECT_EQ(issued, engine->logicCommits.Current());
    EXPECT_EQ(sequence, engine->meterDetectors.Find(kProbe)->withdrawalLogicSequence);
    EXPECT_EQ(count, engine->meterProbeWithdrawalCount);
}

TEST(MeterProbe, PreparedBusRemovalSequenceExhaustionDoesNotPartiallyRetire) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kProbe, kSourceBus,
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, kSecondProbe, kSourceBus,
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    const std::vector<harken::BusSlotRef> refs{engine->buses.FindRef(kSourceBus)};
    engine->logicCommits.SetCurrentForTests(UINT64_MAX - 1);
    const auto head = engine->commands.PublishedHead();
    const auto revision = engine->acceptedBusTopologyRevision;
    harken::MeterProbeSystem::PreparedBusRemovalV1 prepared;
    EXPECT_EQ(VN_BACKPRESSURE, engine->meterProbes.PrepareBusRemovalV1(*engine, refs, prepared));
    EXPECT_FALSE(engine->meterProbes.CanCommitBusRemovalV1(*engine, prepared));
    EXPECT_TRUE(engine->runtimeRecreateRequired);
    EXPECT_TRUE(engine->logicCommits.Exhausted());
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_EQ(revision, engine->acceptedBusTopologyRevision);
    EXPECT_EQ(0u, engine->meterProbeWithdrawalCount);
    for (const auto id : {kProbe, kSecondProbe}) {
        ASSERT_NE(nullptr, engine->meterDetectors.Find(id));
        EXPECT_EQ(harken::MeterSystem::State::Installed, engine->meterDetectors.Find(id)->state);
        EXPECT_EQ(0u, engine->meterDetectors.Find(id)->withdrawalLogicSequence);
    }
}

void DefineSidechain(harken::Engine* engine, VnObjectId bindingId,
                     VnObjectId probeId, std::uint32_t generation,
                     std::uint32_t transitionMs = 0) {
    const harken::CurvePoint points[2] = {{-96.0f, 0.0f}, {-6.0f, -18.0f}};
    const harken::CurveHandlePair handles[1] = {
        {-96.0f, 0.0f, -6.0f, -18.0f}};
    ASSERT_EQ(VN_OK, harken::DefineMeterProbeBinding(
                         *engine, bindingId, probeId, generation,
                         VN_TARGET_SCOPE_BUS, /*targetId=*/0, points,
                         handles, 2, transitionMs));
}

void DefineConstantProbeBinding(
    harken::Engine* engine, VnObjectId bindingId,
    VnObjectId probeId, std::uint32_t generation,
    std::uint32_t targetScope, std::uint32_t targetId, float contribution,
    std::uint32_t transitionMs) {
    const harken::CurvePoint points[2] = {
        {-96.0f, contribution}, {0.0f, contribution}};
    const harken::CurveHandlePair handles[1] = {
        {-96.0f, contribution, 0.0f, contribution}};
    ASSERT_EQ(VN_OK, harken::DefineMeterProbeBinding(
                         *engine, bindingId, probeId, generation,
                         targetScope, targetId, points, handles, 2,
                         transitionMs));
}

TEST(MeterProbe,
     TwoBusProbesAggregateAddDbAndEachRemovalPreservesTheSurvivor) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

    std::uint32_t firstGeneration = 0;
    std::uint32_t secondGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &firstGeneration));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kSecondProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &secondGeneration));
    DefineConstantProbeBinding(
        engine, kSidechain, kProbe, firstGeneration, VN_TARGET_SCOPE_BUS,
        harken::kBusTargetBusVolume, -6.0f, 17);
    DefineConstantProbeBinding(
        engine, kSecondSidechain, kSecondProbe, secondGeneration,
        VN_TARGET_SCOPE_BUS, harken::kBusTargetBusVolume, -3.0f, 41);

    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    const std::uint32_t targetGeneration =
        engine->buses.GenerationAt(targetIndex);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSecondSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    ASSERT_EQ(2u, engine->meterProbes.AttachmentCount(*engine));

    std::uint64_t head = engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(head + 1u, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.SlotActive(
        targetIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.SlotValue(
                               targetIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.Reduce(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(41u, engine->busTargets.LastPublishedRampMs(
                       targetIndex, harken::kBusTargetBusVolume));

    head = engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, harken::DetachMeterProbeBinding(
                         *engine, kSecondSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    EXPECT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.SlotActive(
        targetIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.SlotValue(
                               targetIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(head + 1u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(
                               targetIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(41u, engine->busTargets.LastPublishedRampMs(
                       targetIndex, harken::kBusTargetBusVolume));

    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSecondSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    head = engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(head + 1u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.SlotValue(
                               targetIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(41u, engine->busTargets.LastPublishedRampMs(
                       targetIndex, harken::kBusTargetBusVolume));
    ASSERT_EQ(2u, engine->meterProbes.AttachmentCount(*engine));

    head = engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, harken::DetachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    EXPECT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.SlotActive(
        targetIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.SlotValue(
                               targetIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-9.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(head + 1u, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(-3.0f, engine->busTargets.SlotValue(
                               targetIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-3.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(17u, engine->busTargets.LastPublishedRampMs(
                       targetIndex, harken::kBusTargetBusVolume));
}

TEST(MeterProbe, DetachingLastBusAttachmentWithdrawsContributionToNeutral) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

    constexpr std::uint32_t kDepartureRampMs = 23;
    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    DefineConstantProbeBinding(
        engine, kSidechain, kProbe, probeGeneration, VN_TARGET_SCOPE_BUS,
        harken::kBusTargetBusVolume, -6.0f, kDepartureRampMs);

    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    const std::uint32_t targetGeneration =
        engine->buses.GenerationAt(targetIndex);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(engine->busTargets.SlotActive(
        targetIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    ASSERT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(
                               targetIndex,
                               harken::kBusTargetBusVolume));

    const std::uint64_t head = engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, harken::DetachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.SlotActive(
        targetIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(
                               targetIndex,
                               harken::kBusTargetBusVolume));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(head + 1u, engine->commands.PublishedHead());
    EXPECT_FALSE(engine->busTargets.SlotActive(
        targetIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.Reduce(
                              targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                              targetIndex, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(kDepartureRampMs, engine->busTargets.LastPublishedRampMs(
                                    targetIndex,
                                    harken::kBusTargetBusVolume));
}

TEST(MeterProbe,
     NaturalVoiceEndsReleaseExactPrivateAttachmentCapacityBeforeSlotReuse) {
    VnConfig config = ProbeConfig();
    config.maxVoices = 1;
    config.maxBindingAttachments = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));

    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    DefineConstantProbeBinding(
        engine, kSidechain, kProbe, probeGeneration, VN_TARGET_SCOPE_VOICE,
        harken::kTargetVoiceVolume, -6.0f, 0);

    constexpr VnSoundId kShortSound = harkentest::ObjectId(902);
    const std::vector<float> shortDc = MakeDC(64, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(shortDc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kShortSound, &pcm));

    VnVoiceHandle previousRender = 0;
    std::uint32_t previousGeneration = 0;
    for (std::uint32_t iteration = 0; iteration < 3; ++iteration) {
        const VnPlayDesc play = MakePlayDesc(kShortSound);
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
        ASSERT_NE(0u, render);
        const std::uint32_t renderGeneration =
            static_cast<std::uint32_t>(render >> 32);
        if (iteration != 0) {
            EXPECT_EQ(static_cast<std::uint32_t>(previousRender),
                      static_cast<std::uint32_t>(render));
            EXPECT_NE(previousRender, render);
            EXPECT_GT(renderGeneration, previousGeneration);
            EXPECT_EQ(VN_STALE_HANDLE, harken::AttachMeterProbeBinding(
                                           *engine, kSidechain,
                                           VN_TARGET_SCOPE_VOICE, {}, voice,
                                           previousGeneration));
            EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
        }

        if (iteration == 0) {
            engine->params.SetRetiringTargetCommitPageLimitForTests(0);
            EXPECT_EQ(VN_BACKPRESSURE, harken::AttachMeterProbeBinding(
                *engine, kSidechain, VN_TARGET_SCOPE_VOICE, {}, voice, renderGeneration));
            EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
            engine->params.SetRetiringTargetCommitPageLimitForTests(SIZE_MAX);
        }
        ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                             *engine, kSidechain, VN_TARGET_SCOPE_VOICE,
                             {}, voice, renderGeneration));
        engine->params.SetRetiringTargetCommitPageLimitForTests(0);
        ASSERT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));
        ASSERT_EQ(1u, engine->bindings.AttachmentCount());
        ASSERT_NE(nullptr, engine->bindings.FindProbeAttachment(
                               kSidechain, VN_TARGET_SCOPE_VOICE, {}, voice,
                               renderGeneration));

        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_FLOAT_EQ(-6.0f, engine->params.LastCommitted(
                                   render, harken::kTargetVoiceVolume));
        EXPECT_FALSE(engine->params.PendingPublication(
            render, harken::kTargetVoiceVolume));
        const std::uint64_t headAfterContribution =
            engine->commands.PublishedHead();

        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
                  engine->ledger.Classify(voice).kind);
        EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
        EXPECT_EQ(0u, engine->bindings.AttachmentCount());
        EXPECT_EQ(nullptr, engine->bindings.FindProbeAttachment(
                               kSidechain, VN_TARGET_SCOPE_VOICE, {}, voice,
                               renderGeneration));
        EXPECT_EQ(1u, engine->meterProbes.BindingCount(*engine));
        EXPECT_FALSE(engine->params.PendingPublication(
            render, harken::kTargetVoiceVolume));
        EXPECT_FLOAT_EQ(0.0f, engine->params.LastCommitted(
                                  render, harken::kTargetVoiceVolume));
        EXPECT_EQ(0u, engine->params.DirtyTargetCount());
        EXPECT_EQ(headAfterContribution, engine->commands.PublishedHead());

        previousRender = render;
        previousGeneration = renderGeneration;
    }
}

TEST(MeterProbe,
     WithdrawalUsesOwnFrontierAndFenceDespitePersistentUnrelatedEffectDemand) {
    VnConfig config = ProbeConfig();
    config.fxSlotsPerBus = 1;
    config.delayEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(delay);
    delay.delayMs = 10.0f;
    delay.feedback = 0.25f;
    delay.wet = 1.0f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kTargetBus, 0, VN_EFFECT_FEEDBACK_DELAY,
                         reinterpret_cast<const VnEffectDesc*>(&delay)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    constexpr std::uint32_t kWithdrawalRampMs = 10;
    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    DefineConstantProbeBinding(
        engine, kSidechain, kProbe, probeGeneration, VN_TARGET_SCOPE_BUS,
        harken::kBusTargetBusVolume, -6.0f, kWithdrawalRampMs);
    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    const std::uint32_t targetGeneration =
        engine->buses.GenerationAt(targetIndex);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0, targetGeneration));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(
                               targetIndex, harken::kBusTargetBusVolume));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    using EffectScope = harken::EffectParamTargetStore::Scope;
    std::uint32_t wetRevision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex),
        targetGeneration, 0, harken::kDelayParamWet,
        VN_EFFECT_FEEDBACK_DELAY, &wetRevision));
    harken::VariableSliceRef unrelatedOwner{};
    ASSERT_TRUE(engine->variableSlices.Reserve(
        engine->variables.CurrentWorldGeneration(),
        engine->variables.CurrentTransportEpoch(), 0x554E52454C415445ull,
        0, 0, 1, &unrelatedOwner));
    harken::VariableSliceTarget unrelatedTarget{};
    unrelatedTarget.locator.scope =
        harken::VariableTargetScope::BusEffect;
    unrelatedTarget.locator.object = targetIndex;
    unrelatedTarget.locator.objectGeneration = targetGeneration;
    unrelatedTarget.locator.effectSlot = 0;
    unrelatedTarget.locator.paramIndex = harken::kDelayParamWet;
    unrelatedTarget.locator.effectType = VN_EFFECT_FEEDBACK_DELAY;
    unrelatedTarget.targetRevision = wetRevision;
    ASSERT_TRUE(engine->variableSlices.AddTarget(
        unrelatedOwner, unrelatedTarget));
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex),
        targetGeneration, 0, harken::kDelayParamWet, unrelatedOwner,
        wetRevision));
    const float wetCommitted = engine->effectParamTargets.LastCommitted(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex), 0,
        harken::kDelayParamWet);
    engine->effectParamTargets.NoteDirectBound(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex), 0,
        harken::kDelayParamWet, 0.25f);
    ASSERT_TRUE(engine->effectParamTargets.PendingPublication(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex), 0,
        harken::kDelayParamWet));
    ASSERT_EQ(1u, engine->effectParamTargets.DemandCount());
    ASSERT_TRUE(engine->effectParamTargets.ForeignClaim(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex),
        targetGeneration, 0, harken::kDelayParamWet));

    harken::MeterSystem::Probe* withdrawal =
        engine->meterDetectors.Find(kProbe);
    ASSERT_NE(nullptr, withdrawal);
    ASSERT_EQ(harken::MeterSystem::State::Installed, withdrawal->state);
    const std::uint64_t headBeforeNeutral =
        engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(
                         *engine, kProbe, probeGeneration));
    ASSERT_EQ(harken::MeterSystem::State::Withdrawing, withdrawal->state);
    ASSERT_EQ(probeGeneration, withdrawal->identity.probeGeneration);
    ASSERT_EQ(1u, engine->meterProbeWithdrawalCount);
    ASSERT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));
    ASSERT_EQ(1u, engine->meterProbes.BindingCount(*engine));
    const std::uint32_t sourceIndex =
        engine->buses.FindIndex(kSourceBus);
    EXPECT_EQ(VN_IN_USE, harken::AttachMeterProbeBinding(
                             *engine, kSidechain,
                             VN_TARGET_SCOPE_BUS, kSourceBus, 0,
                             engine->buses.GenerationAt(sourceIndex)));
    EXPECT_EQ(VN_IN_USE, harken::DetachMeterProbeBinding(
                             *engine, kSidechain,
                             VN_TARGET_SCOPE_BUS, kTargetBus, 0,
                             targetGeneration));
    EXPECT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));

    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VariableSliceRef withdrawalSlice =
        engine->meterProbes.WithdrawalSliceForTests(
            *engine, kProbe, probeGeneration);
    ASSERT_TRUE(withdrawalSlice.Valid());
    EXPECT_EQ(harken::VariableSliceState::PageCommitted,
              engine->variableSlices.State(withdrawalSlice));
    EXPECT_EQ(1u,
              engine->variableSlices.TargetCount(withdrawalSlice));
    EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_EQ(0u, engine->meterProbes.BindingCount(*engine));
    EXPECT_EQ(headBeforeNeutral, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.LastCommitted(
                                   targetIndex,
                                   harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.Reduce(
                                  targetIndex,
                                  harken::kBusTargetBusVolume));
    EXPECT_TRUE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));
    EXPECT_TRUE(withdrawal->withdrawalFrontierCaptured);
    EXPECT_EQ(headBeforeNeutral,
              withdrawal->withdrawalCommandFrontier);
    EXPECT_FALSE(withdrawal->withdrawalSettled);
    EXPECT_EQ(harken::MeterSystem::State::Withdrawing, withdrawal->state);
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        targetIndex, targetGeneration, harken::kBusTargetBusVolume));
    EXPECT_TRUE(engine->effectParamTargets.PendingPublication(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex), 0,
        harken::kDelayParamWet));
    EXPECT_EQ(1u, engine->effectParamTargets.DemandCount());
    EXPECT_FLOAT_EQ(wetCommitted,
                    engine->effectParamTargets.LastCommitted(
                        EffectScope::Bus,
                        static_cast<std::uint64_t>(targetIndex), 0,
                        harken::kDelayParamWet));

    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         kTargetBus, 0, harken::kDelayParamDry, 0.75f));
    EXPECT_EQ(headBeforeNeutral + 1u,
              engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(0.75f,
                    engine->effectParamTargets.LastCommitted(
                        EffectScope::Bus,
                        static_cast<std::uint64_t>(targetIndex), 0,
                        harken::kDelayParamDry));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::MeterSystem::State::Withdrawing, withdrawal->state);
    EXPECT_EQ(headBeforeNeutral,
              withdrawal->withdrawalCommandFrontier);
    EXPECT_FALSE(withdrawal->withdrawalSettled);
    EXPECT_EQ(harken::VariableSliceState::PageCommitted,
              engine->variableSlices.State(withdrawalSlice));

    for (std::uint32_t block = 0;
         block < 16 &&
         engine->variableSlices.State(withdrawalSlice) ==
             harken::VariableSliceState::PageCommitted;
         ++block) {
        engine->nullBackend.Pump(1);
    }
    const std::uint64_t ownAppliedBlock = engine->mixer.BlocksRendered();
    ASSERT_EQ(harken::VariableSliceState::SliceApplied,
              engine->variableSlices.State(withdrawalSlice));
    ASSERT_EQ(ownAppliedBlock,
              engine->variableSlices.AppliedBlockSequence(
                  withdrawalSlice));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(withdrawal->withdrawalSettled);
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                                  targetIndex,
                                  harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        targetIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(kWithdrawalRampMs,
              engine->busTargets.LastPublishedRampMs(
                  targetIndex, harken::kBusTargetBusVolume));
    constexpr std::uint64_t kRampFrames =
        (48000ull * kWithdrawalRampMs + 999ull) / 1000ull;
    constexpr std::uint64_t kRampBlocks =
        (kRampFrames + 256ull - 1ull) / 256ull;
    static_assert(kRampFrames == 480);
    static_assert(kRampBlocks == 2);
    const std::uint64_t retireBlock =
        ownAppliedBlock + kRampBlocks + 1u;
    EXPECT_EQ(retireBlock, withdrawal->withdrawalAudioFenceBlock);
    EXPECT_EQ(harken::MeterSystem::State::Withdrawing, withdrawal->state);
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        targetIndex, targetGeneration, harken::kBusTargetBusVolume));

    for (std::uint64_t block = 0; block < kRampBlocks; ++block) {
        const float dry = block == 0 ? 0.5f : 0.75f;
        const std::uint64_t trafficHead = engine->commands.PublishedHead();
        ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                             kTargetBus, 0, harken::kDelayParamDry, dry));
        EXPECT_EQ(trafficHead + 1u, engine->commands.PublishedHead());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(harken::MeterSystem::State::Withdrawing,
                  withdrawal->state);
        EXPECT_EQ(headBeforeNeutral,
                  withdrawal->withdrawalCommandFrontier);
        EXPECT_TRUE(engine->effectParamTargets.PendingPublication(
            EffectScope::Bus, static_cast<std::uint64_t>(targetIndex), 0,
            harken::kDelayParamWet));
        EXPECT_EQ(1u, engine->effectParamTargets.DemandCount());
        EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
            EffectScope::Bus,
            static_cast<std::uint64_t>(targetIndex),
            targetGeneration, 0, harken::kDelayParamWet));
    }
    EXPECT_EQ(ownAppliedBlock + kRampBlocks,
              engine->mixer.BlocksRendered());

    const std::uint64_t finalTrafficHead =
        engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_SetBusEffectParam(
                         kTargetBus, 0, harken::kDelayParamDry, 0.5f));
    EXPECT_EQ(finalTrafficHead + 1u, engine->commands.PublishedHead());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(retireBlock, engine->mixer.BlocksRendered());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::MeterSystem::State::Retired, withdrawal->state);
    EXPECT_FALSE(engine->meterDetectors.IsProbe(kProbe));
    EXPECT_EQ(0u, engine->meterProbeWithdrawalCount);
    EXPECT_GT(engine->commands.PublishedHead(), headBeforeNeutral);
    EXPECT_TRUE(engine->effectParamTargets.PendingPublication(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex), 0,
        harken::kDelayParamWet));
    EXPECT_EQ(1u, engine->effectParamTargets.DemandCount());
    EXPECT_FLOAT_EQ(wetCommitted,
                    engine->effectParamTargets.LastCommitted(
                        EffectScope::Bus,
                        static_cast<std::uint64_t>(targetIndex), 0,
                        harken::kDelayParamWet));
    EXPECT_TRUE(engine->effectParamTargets.ForeignClaim(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex),
        targetGeneration, 0, harken::kDelayParamWet));

    std::uint32_t rebornGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &rebornGeneration));
    EXPECT_GT(rebornGeneration, probeGeneration);
    engine->effectParamTargets.ReleaseClaim(
        EffectScope::Bus, static_cast<std::uint64_t>(targetIndex),
        targetGeneration, 0, harken::kDelayParamWet, unrelatedOwner);
    EXPECT_TRUE(
        engine->variableSlices.AbandonPrepared(unrelatedOwner));
}

TEST(MeterProbe, EffectWithdrawalFreezesTheAuthoredTransition) {
    VnConfig config = ProbeConfig();
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = 0.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kTargetBus, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    constexpr std::uint32_t kTransitionMs = 37;
    const std::uint32_t targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    DefineConstantProbeBinding(
        engine, kSidechain, kProbe, probeGeneration,
        VN_TARGET_SCOPE_BUS_EFFECT_PARAM, targetId, -6.0f,
        kTransitionMs);
    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    const std::uint32_t targetGeneration =
        engine->buses.GenerationAt(targetIndex);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain,
                         VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kTargetBus, 0,
                         targetGeneration));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(
                         *engine, kProbe, probeGeneration));
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VariableSliceRef slice =
        engine->meterProbes.WithdrawalSliceForTests(
            *engine, kProbe, probeGeneration);
    ASSERT_TRUE(slice.Valid());
    ASSERT_EQ(harken::VariableSliceState::PageCommitted,
              engine->variableSlices.State(slice));
    const harken::VariableSliceTable& slices = engine->variableSlices;
    ASSERT_EQ(1u, slices.TargetCount(slice));
    const harken::VariableSliceTarget* target = slices.Target(slice, 0);
    ASSERT_NE(nullptr, target);
    EXPECT_EQ(harken::VariableTargetScope::BusEffect,
              target->locator.scope);
    EXPECT_EQ(kTransitionMs, target->rampMs);
    EXPECT_EQ(harken::kOpcodeSetBusEffectParam, target->command.opcode);
    EXPECT_EQ(1776u,
              target->command.payload.setBusEffectParam.transitionFrames);
}

TEST(MeterProbe, CommitSequenceExhaustionRefusesUninstallBeforeMutation) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));

    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0,
                         &probeGeneration));
    harken::MeterSystem::Probe* probe =
        engine->meterDetectors.Find(kProbe);
    ASSERT_NE(nullptr, probe);
    ASSERT_EQ(harken::MeterSystem::State::Installed, probe->state);
    engine->logicCommits.SetCurrentForTests(UINT64_MAX);

    EXPECT_EQ(VN_BACKPRESSURE, harken::UninstallMeterProbe(
                                   *engine, kProbe, probeGeneration));
    EXPECT_TRUE(engine->runtimeRecreateRequired);
    EXPECT_EQ(harken::MeterSystem::State::Installed, probe->state);
    EXPECT_EQ(1u, engine->meterDetectors.ActiveCount());
    EXPECT_EQ(0u, engine->meterProbeWithdrawalCount);
}

TEST(MeterProbe, InstallLifecycleAndGenerationExactness) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));

    std::uint32_t generation = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generation));
    EXPECT_NE(0u, generation);
    // Duplicate stable id refuses; unknown bus refuses; bad tap refuses.
    EXPECT_EQ(VN_IN_USE, harken::InstallMeterProbe(
                             *engine, kProbe, kSourceBus,
                             VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    EXPECT_EQ(VN_UNKNOWN_ID,
              harken::InstallMeterProbe(*engine, harkentest::ObjectId(8009), harkentest::ObjectId(777),
                                       VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    EXPECT_EQ(VN_INVALID_ARG,
              harken::InstallMeterProbe(*engine, harkentest::ObjectId(8009), kSourceBus, 3, 0, 0));
    // Uninstall needs the exact generation; a stale one refuses.
    EXPECT_EQ(VN_STALE_HANDLE,
              harken::UninstallMeterProbe(*engine, kProbe, generation + 1u));
    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(*engine, kProbe, generation));
    EXPECT_EQ(VN_IN_USE,
              harken::UninstallMeterProbe(*engine, kProbe, generation));
    EXPECT_TRUE(engine->meterDetectors.IsProbe(kProbe));
    EXPECT_EQ(VN_IN_USE, harken::InstallMeterProbe(
                             *engine, kProbe, kSourceBus,
                             VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
    // Withdrawal retirement waits for publication, command settlement, and
    // an audio fence before the stable id can reuse its slot.
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine->meterDetectors.IsProbe(kProbe));
    std::uint32_t next = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &next));
    EXPECT_GT(next, generation);
    // A binding defined against the old generation refuses.
    const harken::CurvePoint points[2] = {{-96.0f, 0.0f}, {-6.0f, -12.0f}};
    const harken::CurveHandlePair handles[1] = {
        {-96.0f, 0.0f, -6.0f, -12.0f}};
    EXPECT_EQ(VN_STALE_HANDLE,
              harken::DefineMeterProbeBinding(
                  *engine, kSidechain, kProbe, generation,
                  VN_TARGET_SCOPE_BUS, 0, points, handles, 2, 0));
}

TEST(MeterProbe, ProbeIsNeverAVariableOrDecisionInput) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));

    // The probe id is not a Variable: public Set refuses with the unknown-
    // definition result, and no definitions-only world definition exists.
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValue(kProbe, 1.0f));
    EXPECT_EQ(nullptr, engine->variables.Definition(kProbe));
    // A public Variable Binding on the probe id resolves nothing: the probe
    // level never leaks into Variable evaluation.
    EXPECT_TRUE(engine->meterDetectors.IsProbe(kProbe));
    float unused = 0.0f;
    EXPECT_FALSE(engine->variables.CurrentValue(kProbe, &unused));
}

TEST(MeterProbe, SidechainWaveformReachesBoundBusAndReleases) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));

    std::uint32_t generation = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generation));
    DefineSidechain(engine, kSidechain, kProbe, generation);
    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0,
                         engine->buses.GenerationAt(targetIndex)));

    // Steady target-bus tone, then loud source-bus signal: the probe level
    // rises and the sidechain attenuates the target audibly.
    const std::vector<float> dc = MakeDC(48000 * 2, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kDcSound, &pcm));
    VnPlayDesc targetPlay = MakePlayDesc(kDcSound);
    targetPlay.busId = kTargetBus;
    targetPlay.loopCount = 0;
    VnVoiceHandle targetVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture loud;
    harkentest::PumpAndCapture(engine, 2, loud);
    const float loudPeak = harkentest::Peak(loud.left);
    ASSERT_GT(loudPeak, harkentest::kSilenceFloor);

    VnPlayDesc sourcePlay = MakePlayDesc(kDcSound);
    sourcePlay.busId = kSourceBus;
    sourcePlay.loopCount = 0;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_GT(engine->meterDetectors.ValueDb(kProbe),
              harken::kMeterFloorDb);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(8);
    ASSERT_EQ(VN_OK, Vn_Stop(sourceVoice, 0));
    engine->nullBackend.Pump(1);
    harkentest::Capture ducked;
    harkentest::PumpAndCapture(engine, 1, ducked);
    const float duckedPeak = harkentest::Peak(ducked.left);
    ASSERT_GT(duckedPeak, 0.0f);
    EXPECT_LT(duckedPeak, loudPeak * 0.5f);

    // With the source stopped, release ballistics let the target recover.
    for (int i = 0; i < 12; ++i) {
        engine->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    harkentest::Capture recovered;
    harkentest::PumpAndCapture(engine, 2, recovered);
    EXPECT_GT(harkentest::Peak(recovered.left), duckedPeak);
}

TEST(MeterProbe, UninstallWithdrawsContributionToNeutral) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    std::uint32_t generation = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 400,
                         &generation));
    DefineSidechain(engine, kSidechain, kProbe, generation, 30);
    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    engine->busTargets.SetRetiringTargetCommitPageLimitForTests(0);
    EXPECT_EQ(VN_BACKPRESSURE, harken::AttachMeterProbeBinding(
        *engine, kSidechain, VN_TARGET_SCOPE_BUS, kTargetBus, 0,
        engine->buses.GenerationAt(targetIndex)));
    EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
    engine->busTargets.SetRetiringTargetCommitPageLimitForTests(SIZE_MAX);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0,
                         engine->buses.GenerationAt(targetIndex)));
    engine->busTargets.SetRetiringTargetCommitPageLimitForTests(0);

    const std::vector<float> dc = MakeDC(48000 * 2, 0.5f);
    const VnPCMDesc pcm = harkentest::MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kDcSound, &pcm));
    VnPlayDesc targetPlay = MakePlayDesc(kDcSound);
    targetPlay.busId = kTargetBus;
    targetPlay.loopCount = 0;
    VnVoiceHandle targetVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));
    VnPlayDesc sourcePlay = MakePlayDesc(kDcSound);
    sourcePlay.busId = kSourceBus;
    sourcePlay.loopCount = 0;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));
    engine->nullBackend.Pump(3);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    harkentest::Capture ducked;
    harkentest::PumpAndCapture(engine, 2, ducked);
    const float duckedPeak = harkentest::Peak(ducked.left);
    ASSERT_GT(duckedPeak, harkentest::kSilenceFloor);

    // Uninstall while hot: attachments withdraw to neutral, bindings and the
    // slot retire, and the target returns to full level even though the
    // source keeps sounding (the 400 ms release would otherwise pin it low).
    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(*engine, kProbe, generation));
    EXPECT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_EQ(1u, engine->meterProbes.BindingCount(*engine));
    for (int i = 0; i < 6; ++i) {
        engine->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    harkentest::Capture withdrawn;
    harkentest::PumpAndCapture(engine, 2, withdrawn);
    EXPECT_GT(harkentest::Peak(withdrawn.left), duckedPeak * 1.5f);
    EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_EQ(0u, engine->meterProbes.BindingCount(*engine));
}

TEST(MeterProbe, RepeatedWithdrawalReturnsSharedSliceResources) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_TRUE(engine.variableSlices.Reset(4, 8, 8, 2));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    const auto targetIndex = engine.buses.FindIndex(kTargetBus);
    for (int iteration = 0; iteration < 6; ++iteration) {
        SCOPED_TRACE(iteration);
        std::uint32_t generation = 0;
        ASSERT_EQ(VN_OK, harken::InstallMeterProbe(engine, kProbe, kSourceBus,
            VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generation));
        DefineSidechain(&engine, kSidechain, kProbe, generation);
        ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(engine, kSidechain,
            VN_TARGET_SCOPE_BUS, kTargetBus, 0,
            engine.buses.GenerationAt(targetIndex)));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(engine, kProbe, generation));
        for (int step = 0; step < 12; ++step) {
            engine.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        EXPECT_FALSE(engine.meterDetectors.IsProbe(kProbe));
        EXPECT_EQ(0u, engine.meterProbeWithdrawalCount);
        EXPECT_EQ(4u, engine.variableSlices.FreeRowCount());
        EXPECT_EQ(2u, engine.variableSlices.FreePayloadCount());
        EXPECT_EQ(8u, engine.variableSlices.FreeTargetCapacity());
        harken::BindingDirtyTarget target;
        for (std::uint32_t cursor = 0; engine.bindings.Dependencies().DirtyTargetAt(cursor, &target); ++cursor) {
            harken::BindingDependencyIdentity identity;
            ASSERT_TRUE(engine.bindings.Dependencies().TargetIdentity(target, &identity));
            EXPECT_NE(static_cast<std::uint32_t>(harken::BindingSourceKind::MeterProbe), identity.detail);
        }
    }
}

TEST(MeterProbe, MultipleWithdrawalChunksShareVariablePayloadsAndRespectTheStrictFence) {
    const auto config = ProbeConfig();
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto proofCapacity = engine.variableSlices.Capacity();
    const auto payloadCapacity = engine.variableSlices.PayloadCapacity();
    const auto targetCapacity = engine.variableSlices.TargetPoolCapacity();
    EXPECT_EQ(engine.variables.RevisionStorageCapacity() + 2 * config.maxBuses, proofCapacity);
    EXPECT_EQ(engine.variables.PendingCapacity() + 2 * config.maxBuses, payloadCapacity);
    ASSERT_EQ(proofCapacity, engine.variableSlices.FreeRowCount());
    ASSERT_EQ(payloadCapacity, engine.variableSlices.FreePayloadCount());
    ASSERT_TRUE(engine.variableSlices.Reset(proofCapacity, 1, targetCapacity, payloadCapacity));
    engine.meterProbes = harken::MeterProbeSystem(config.maxBindingAttachments, 1);
    const auto variable = harkentest::ObjectId(9001);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallSmoothDefinitions(&engine, {variable}, 0, 1).status);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::array<VnBusId, 4> targets{kTargetBus, harkentest::ObjectId(23),
        harkentest::ObjectId(24), harkentest::ObjectId(25)};
    for (const auto target : targets) {
        ASSERT_EQ(VN_OK, Vn_CreateBus(target, VN_BUS_MASTER));
        engine.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    const std::array<VnObjectId, 2> probes{kProbe, kSecondProbe};
    std::array<std::uint32_t, 2> generations{};
    for (std::size_t p = 0; p < probes.size(); ++p) {
        ASSERT_EQ(VN_OK, harken::InstallMeterProbe(engine, probes[p], kSourceBus,
            VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generations[p]));
        const auto binding = harkentest::ObjectId(8100 + p);
        DefineConstantProbeBinding(&engine, binding, probes[p], generations[p],
            VN_TARGET_SCOPE_BUS, harken::kBusTargetBusVolume, -12.0f, 0);
        for (std::size_t t = p * 2; t < p * 2 + 2; ++t) {
            const auto index = engine.buses.FindIndex(targets[t]);
            ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(engine, binding,
                VN_TARGET_SCOPE_BUS, targets[t], 0, engine.buses.GenerationAt(index)));
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    for (const auto target : targets)
        EXPECT_FLOAT_EQ(-12.0f, engine.busTargets.LastCommitted(
            engine.buses.FindIndex(target), harken::kBusTargetBusVolume));
    for (std::size_t p = 0; p < probes.size(); ++p)
        ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(engine, probes[p], generations[p]));
    ASSERT_EQ(VN_OK, Vn_Update());
    std::array<harken::VariableSliceRef, 2> first{};
    for (std::size_t p = 0; p < probes.size(); ++p) {
        first[p] = engine.meterProbes.WithdrawalSliceForTests(engine, probes[p], generations[p]);
        ASSERT_TRUE(first[p].Valid());
        ASSERT_EQ(1u, engine.variableSlices.TargetCount(first[p]));
    }
    ASSERT_EQ(proofCapacity - 2, engine.variableSlices.FreeRowCount());
    ASSERT_EQ(payloadCapacity - 2, engine.variableSlices.FreePayloadCount());
    const auto firstPublicationEnd = engine.timelinePool.PublishCutoffFrame();
    auto observed = harken::ObserveVariableCallAt(engine.variables.CurrentWorldGeneration(),
        engine.variables.CurrentTransportEpoch(), config.blockFrames, 0, 0);
    observed.requestedEffectiveFrame = firstPublicationEnd + 8ull * config.blockFrames;
    const auto accepted = engine.variables.AcceptGlobal(variable, 0.75f, observed,
        engine.params, engine.bindings, engine.busTargets);
    ASSERT_EQ(VN_OK, accepted.result);
    harkentest::VariableOutcomeCapture outcomes(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto variableSlice = engine.variables.SliceForTicketForTests(accepted.ticket.sequence);
    ASSERT_TRUE(variableSlice.Valid());
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine.variableSlices.State(variableSlice));
    ASSERT_EQ(0u, engine.variableSlices.TargetCount(variableSlice));
    ASSERT_EQ(proofCapacity - 3, engine.variableSlices.FreeRowCount());
    ASSERT_EQ(payloadCapacity - 3, engine.variableSlices.FreePayloadCount());
    engine.mixer.SetCurrentSliceScratchLimitForTest(0);
    for (unsigned step = 0; step < 32 &&
        engine.mixer.BlocksRendered() * config.blockFrames < firstPublicationEnd; ++step)
        engine.nullBackend.Pump(1);
    ASSERT_GE(engine.mixer.BlocksRendered() * config.blockFrames, firstPublicationEnd);
    engine.mixer.RestoreCurrentSliceScratchLimitForTest();
    engine.nullBackend.Pump(1);
    const auto appliedBlock = engine.mixer.BlocksRendered();
    for (const auto ref : first) {
        ASSERT_EQ(harken::VariableSliceState::SliceApplied, engine.variableSlices.State(ref));
        ASSERT_EQ(appliedBlock, engine.variableSlices.AppliedBlockSequence(ref));
    }
    ASSERT_EQ(harken::VariableSliceState::PageCommitted, engine.variableSlices.State(variableSlice));
    ASSERT_EQ(VN_OK, Vn_Update());
    for (std::size_t p = 0; p < probes.size(); ++p)
        ASSERT_FALSE(engine.meterProbes.WithdrawalSliceForTests(engine, probes[p], generations[p]).Valid());
    ASSERT_EQ(payloadCapacity - 3, engine.variableSlices.FreePayloadCount());
    ASSERT_EQ(VN_OK, Vn_Update());
    std::array<harken::VariableSliceRef, 2> second{};
    for (std::size_t p = 0; p < probes.size(); ++p) {
        second[p] = engine.meterProbes.WithdrawalSliceForTests(engine, probes[p], generations[p]);
        ASSERT_TRUE(second[p].Valid());
        ASSERT_EQ(1u, engine.variableSlices.TargetCount(second[p]));
        for (const auto ref : first) EXPECT_NE(ref.slot, second[p].slot);
        EXPECT_NE(variableSlice.slot, second[p].slot);
        EXPECT_EQ(harken::VariableSliceState::SliceApplied, engine.variableSlices.State(first[p]));
        EXPECT_EQ(1u, engine.variableSlices.TargetCount(first[p]));
        EXPECT_EQ(0u, engine.variableSlices.SourceObserverCount(first[p]));
    }
    EXPECT_NE(second[0].slot, second[1].slot);
    EXPECT_EQ(appliedBlock, engine.mixer.BlocksRendered());
    EXPECT_EQ(proofCapacity - 5, engine.variableSlices.FreeRowCount());
    EXPECT_EQ(payloadCapacity - 5, engine.variableSlices.FreePayloadCount());
    EXPECT_EQ(targetCapacity - 4, engine.variableSlices.FreeTargetCapacity());
    EXPECT_EQ(1u, engine.variables.PendingCount());
    EXPECT_EQ(0u, outcomes.Count());
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    for (const auto ref : first) EXPECT_EQ(harken::VariableSliceState::Free, engine.variableSlices.State(ref));
    for (unsigned step = 0; step < 64 &&
        (engine.variableSlices.FreeRowCount() != proofCapacity || engine.meterProbeWithdrawalCount != 0); ++step) {
        engine.nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, engine.meterProbeWithdrawalCount);
    EXPECT_EQ(0u, engine.meterProbes.AttachmentCount(engine));
    EXPECT_EQ(0u, engine.meterProbes.BindingCount(engine));
    for (const auto probe : probes) EXPECT_FALSE(engine.meterDetectors.IsProbe(probe));
    for (const auto target : targets) {
        const auto index = engine.buses.FindIndex(target);
        EXPECT_FLOAT_EQ(0.0f, engine.busTargets.LastCommitted(index, harken::kBusTargetBusVolume));
        EXPECT_FLOAT_EQ(1.0f, engine.buses.BusState(index)->gain.target);
        EXPECT_FALSE(engine.busTargets.ForeignClaim(index, engine.buses.GenerationAt(index),
            harken::kBusTargetBusVolume));
    }
    EXPECT_EQ(0u, engine.variables.PendingCount());
    EXPECT_EQ(1u, outcomes.Count());
    harken::VariableOutcomeRecord outcome{};
    ASSERT_TRUE(outcomes.Pop(&outcome));
    EXPECT_EQ(accepted.ticket.sequence, outcome.acceptedTicket.sequence);
    EXPECT_EQ(harken::VariableOutcomeKind::Applied, outcome.outcome);
    float value = 0;
    ASSERT_TRUE(engine.variables.CurrentValue(variable, &value));
    EXPECT_FLOAT_EQ(0.75f, value);
    EXPECT_EQ(proofCapacity, engine.variableSlices.FreeRowCount());
    EXPECT_EQ(payloadCapacity, engine.variableSlices.FreePayloadCount());
    EXPECT_EQ(targetCapacity, engine.variableSlices.FreeTargetCapacity());
    EXPECT_FALSE(outcomes.Overflowed());
}

TEST(MeterProbe, SourceBusDestroyRetiresProbeAndWithdraws) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    std::uint32_t generation = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generation));
    DefineSidechain(engine, kSidechain, kProbe, generation);
    const std::uint32_t targetIndex = engine->buses.FindIndex(kTargetBus);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS,
                         kTargetBus, 0,
                         engine->buses.GenerationAt(targetIndex)));
    ASSERT_EQ(1u, engine->meterDetectors.ActiveCount());

    // Destroying the metered source bus retires the probe, withdraws its
    // contributions, and frees the identity for a fresh generation.
    ASSERT_EQ(VN_OK, Vn_DestroyBus(kSourceBus));
    EXPECT_EQ(0u, engine->meterDetectors.ActiveCount());
    EXPECT_EQ(1u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_TRUE(engine->meterDetectors.IsProbe(kProbe));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    for (int i = 0; i < 3; ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_FALSE(engine->meterDetectors.IsProbe(kProbe));
    EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
    std::uint32_t reborn = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &reborn));
    EXPECT_GT(reborn, generation);
}

TEST(MeterProbe, MixedWithdrawalChunkDropsIllegalVolumeWithoutCandidateRollback) {
    EngineSession session(ProbeConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSourceBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTargetBus, VN_BUS_MASTER));
    constexpr VnBusId kIllegalBus = harkentest::ObjectId(23);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kIllegalBus, VN_BUS_MASTER));
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f)
                  .status);

    std::uint32_t generation = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(
                         *engine, kProbe, kSourceBus,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &generation));
    DefineConstantProbeBinding(engine, kSidechain, kProbe, generation,
                               VN_TARGET_SCOPE_BUS, harken::kBusTargetBusVolume,
                               -12.0f, 17);
    DefineConstantProbeBinding(engine, kSecondSidechain, kProbe, generation,
                               VN_TARGET_SCOPE_BUS, harken::kBusTargetBusVolume,
                               -800.0f, 41);
    const std::uint32_t legalIndex = engine->buses.FindIndex(kTargetBus);
    const std::uint32_t illegalIndex = engine->buses.FindIndex(kIllegalBus);
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSidechain, VN_TARGET_SCOPE_BUS, kTargetBus, 0,
                         engine->buses.GenerationAt(legalIndex)));
    ASSERT_EQ(VN_OK, harken::AttachMeterProbeBinding(
                         *engine, kSecondSidechain, VN_TARGET_SCOPE_BUS,
                         kIllegalBus, 0, engine->buses.GenerationAt(illegalIndex)));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(2);
    EXPECT_FLOAT_EQ(-12.0f, engine->busTargets.LastCommitted(
                                legalIndex, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(-800.0f, engine->busTargets.LastCommitted(
                                 illegalIndex, harken::kBusTargetBusVolume));

    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = harkentest::ObjectId(7);
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = harken::kBusTargetBusVolume;
    desc.curve.structSize = sizeof(desc.curve);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 800.0f};
    desc.curve.handles[0] =
        harkentest::EndpointHandles(desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(3), &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(attach);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = kIllegalBus;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(3), &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                              illegalIndex, harken::kBusTargetBusVolume));

    const std::uint64_t dropsBefore = engine->paramDropCount;
    const float illegalGain =
        engine->buses.BusState(illegalIndex)->gain.target;
    ASSERT_EQ(VN_OK, harken::UninstallMeterProbe(*engine, kProbe, generation));
    for (int i = 0; i < 16 && (engine->meterProbes.AttachmentCount(*engine) != 0 ||
                               engine->meterProbeWithdrawalCount != 0);
         ++i) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(0u, engine->meterProbes.AttachmentCount(*engine));
    EXPECT_FALSE(engine->busTargets.ForeignClaim(
        legalIndex, harken::kBusTargetBusVolume));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.Reduce(
                              legalIndex, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(illegalGain,
                    engine->buses.BusState(illegalIndex)->gain.target);
    EXPECT_TRUE(std::isfinite(
        engine->buses.BusState(illegalIndex)->gain.target));
    EXPECT_NE(800.0f, engine->busTargets.LastCommitted(
                          illegalIndex, harken::kBusTargetBusVolume));
    EXPECT_GT(engine->paramDropCount, dropsBefore);
}

} // namespace
