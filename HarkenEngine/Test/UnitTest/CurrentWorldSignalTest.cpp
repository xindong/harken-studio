#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "Engine.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/CurrentWorldArtifact.h"
#include "harken/core/detail/CurrentWorldValidation.h"

#include <gtest/gtest.h>
#include <array>
#include <limits>

namespace {
using namespace harken;

struct SignalWorld {
    std::array<SignalRelationV1, 2> relations{};
    std::array<SignalReceiverV1, 2> receivers{};
    ContainerWorldInstallViewV1 world{};
    SignalWorld() {
        world.sourceBodyVintage = kCurrentWorldBodyVintageV20;
        for (std::uint32_t i = 0; i < 2; ++i) {
            receivers[i].target.stableId = harkentest::ObjectId(i + 1);
            relations[i].stableId = harkentest::ObjectId(i + 10);
            relations[i].source.stableId = harkentest::ObjectId(2 - i);
            relations[i].target = receivers[i].target;
        }
        world.signalRelations = relations.data();
        world.signalRelationCount = 2;
        world.signalReceivers = receivers.data();
        world.signalReceiverCount = 2;
        Refresh();
    }
    void Refresh() {
        world.identity.capabilityMask = detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
        world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(world);
    }
};

TEST(CurrentWorldSignal, V20OwnsRoundtripTablesAndFingerprintIncludesEveryConfiguration) {
    SignalWorld f;
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
    std::vector<std::uint8_t> bytes, again;
    ASSERT_TRUE(EncodeCurrentWorldBodyV20(f.world, bytes));
    CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 20, CurrentWorldDecodeMode::StrictArtifact));
    const auto* view = image.ProductionView();
    ASSERT_NE(view, nullptr);
    ASSERT_EQ(view->signalRelationCount, 2u);
    ASSERT_EQ(view->signalReceiverCount, 2u);
    ASSERT_TRUE(EncodeCurrentWorldBodyV20(*view, again));
    EXPECT_EQ(again, bytes);
    EXPECT_NE(view->signalRelations, f.relations.data());
    EXPECT_NE(view->signalReceivers, f.receivers.data());
    const auto original = ComputeCurrentWorldFingerprintV7(*view);
    f.relations[0].sendGainDb = -6;
    EXPECT_NE(original, ComputeCurrentWorldFingerprintV7(f.world));
    f.relations[0].sendGainDb = 0;
    f.receivers[0].attackMs = 10;
    EXPECT_NE(original, ComputeCurrentWorldFingerprintV7(f.world));
    EXPECT_EQ(original, ComputeCurrentWorldFingerprintV7(*view));
    ASSERT_TRUE(EncodeCurrentWorldArtifactV1(*view, bytes));
    CurrentWorldArtifactHeaderV1 header;
    ASSERT_EQ(InspectCurrentWorldArtifactHeaderV1(bytes.data(), bytes.size(), bytes.size(), header),
              CurrentWorldLoadStatusV1::Ok);
    EXPECT_EQ(header.bodyVintage, 20u);
    CurrentWorldBodyImage artifact;
    ASSERT_EQ(DecodeCurrentWorldArtifactBodyV1(header,
        bytes.data() + kCurrentWorldArtifactHeaderBytes,
        bytes.size() - kCurrentWorldArtifactHeaderBytes, artifact), CurrentWorldLoadStatusV1::Ok);
    EXPECT_EQ(artifact.ProductionView()->signalRelationCount, 2u);
}

TEST(CurrentWorldSignal, TruncatedOrOversizedTablesPreservePreviousDecodedImage) {
    SignalWorld f;
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeCurrentWorldBodyV20(f.world, bytes));
    CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 20, CurrentWorldDecodeMode::StrictArtifact));
    const auto* prior = image.ProductionView();
    const auto suffix = bytes.size() - 8 - 2 * 104 - 2 * 56;
    for (std::size_t n = suffix; n < bytes.size(); ++n) {
        EXPECT_FALSE(image.Decode(bytes.data(), n, 20, CurrentWorldDecodeMode::StrictArtifact));
        EXPECT_EQ(image.ProductionView(), prior);
    }
    for (std::size_t n = 0; n < 8; ++n) bytes[suffix + n] = 0xff;
    EXPECT_FALSE(image.Decode(bytes.data(), bytes.size(), 20, CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(image.ProductionView(), prior);
}

TEST(CurrentWorldSignal, OldBodiesCannotSilentlyDiscardSignalTablesOrEnableMusic) {
    SignalWorld f;
    std::vector<std::uint8_t> bytes;
    for (const auto vintage : {18u, 19u}) {
        f.world.sourceBodyVintage = vintage;
        EXPECT_FALSE(vintage == 18 ? EncodeCurrentWorldShippingBody(f.world, bytes)
                                  : EncodeCurrentWorldBodyV19(f.world, bytes));
    }
    ContainerWorldInstallViewV1 empty;
    empty.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV5(empty);
    ASSERT_TRUE(EncodeCurrentWorldShippingBody(empty, bytes));
    CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 18, CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(image.ProductionView()->signalRelationCount, 0u);
    EXPECT_EQ(image.ProductionView()->signalReceiverCount, 0u);
    EXPECT_EQ(ComputeCurrentWorldFingerprintV5(*image.ProductionView()),
              empty.identity.runtimeSemanticFingerprint);
    empty.sourceBodyVintage = 19;
    empty.identity.capabilityMask = kWorldCapabilityEntryScheduling;
    empty.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV6(empty);
    EXPECT_EQ(ValidateCurrentWorldV1(empty), WorldInstallStatus::CapabilityUnavailable);
    ASSERT_TRUE(EncodeCurrentWorldBodyV19(empty, bytes));
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), 19, CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(image.ProductionView()->signalReceiverCount, 0u);
    empty.sourceBodyVintage = 20;
    empty.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(empty);
    EXPECT_EQ(ValidateCurrentWorldV1(empty), WorldInstallStatus::InvalidFormat);
}

TEST(CurrentWorldSignal, RejectsDuplicateTargetsUnknownConsumersAndInvalidParameters) {
    SignalWorld f;
    const auto rejected = [&] {
        f.Refresh();
        EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::InvalidWorld);
    };
    f.receivers[1].target = f.receivers[0].target;
    rejected();
    f.receivers[1].target = f.relations[1].target;
    f.relations[1].stableId = f.relations[0].stableId;
    rejected();
    f.relations[1].stableId = harkentest::ObjectId(11);
    f.relations[0].consumer = static_cast<SignalConsumerKindV1>(2);
    rejected();
    f.relations[0].consumer = SignalConsumerKindV1::Ducking;
    f.receivers[0].algorithm = 2;
    rejected();
    f.receivers[0].algorithm = 1;
    for (const auto value : {-97.0f, 25.0f, std::numeric_limits<float>::quiet_NaN()}) {
        f.relations[0].sendGainDb = value;
        rejected();
    }
    f.relations[0].sendGainDb = 0;
    f.receivers[0].releaseMs = 5001;
    rejected();
    f.receivers[0].releaseMs = 1;
    f.receivers[0].attackMs = .09f;
    rejected();
    f.receivers[0].attackMs = .1f;
    f.relations[0].sendGainDb = -96;
    f.relations[1].sendGainDb = 24;
    f.Refresh();
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
}

TEST(CurrentWorldSignal, MissingContentAndReceiverRequireFinalClosureValidation) {
    SignalWorld f;
    f.relations[0].source.kind = SignalEndpointKindV1::Sound;
    f.relations[0].source.expectedPropertyFingerprint = 123;
    f.Refresh();
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
    EXPECT_EQ(ValidateCurrentWorldV1(f.world), WorldInstallStatus::InvalidWorld);
    f.relations[0].source.kind = SignalEndpointKindV1::Bus;
    f.relations[0].source.expectedPropertyFingerprint = 0;
    f.world.signalReceiverCount = 1;
    f.Refresh();
    EXPECT_EQ(detail::ValidateCurrentWorldPartitionLocalV1(f.world), WorldInstallStatus::Ok);
    EXPECT_EQ(ValidateCurrentWorldV1(f.world), WorldInstallStatus::InvalidWorld);
}

TEST(CurrentWorldSignal, DirectInstallIsAtomicIdempotentAndPreservesLiveTopology) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.maxBuses = 4;
    config.topologyPlanPoolCapacity = 8;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    SignalWorld fixture;
    for (const auto& receiver : fixture.receivers)
        ASSERT_EQ(Vn_CreateBus(receiver.target.stableId, VN_BUS_MASTER), VN_OK);
    const auto source = fixture.receivers[0].target.stableId;
    ASSERT_EQ(Vn_SetBusOutputLayout(source, VN_CHANNEL_LAYOUT_MONO), VN_OK);
    engine.nullBackend.Pump(1);
    const auto before = engine.commands.FreeSlots();
    const auto installed = InstallContainerWorldV1(engine, fixture.world);
    ASSERT_EQ(installed.status, WorldInstallStatus::Ok);
    EXPECT_EQ(engine.commands.FreeSlots(), before - 1);
    ASSERT_NE(engine.acceptedSignalPlan, nullptr);
    auto* first = engine.acceptedSignalPlan.get();
    const auto generation = engine.variables.CurrentWorldGeneration();
    const auto playbackFingerprint = engine.variables.CurrentPlayableWorld().identity.runtimeSemanticFingerprint;
    CommandRecord filler{};
    filler.opcode = kOpcodeSetBusVolume;
    filler.payload.setBusVolume.busIndex = 0;
    while (engine.commands.TryPush(filler)) {}
    const auto retry = InstallContainerWorldV1(engine, fixture.world);
    EXPECT_EQ(retry.status, WorldInstallStatus::Ok);
    EXPECT_TRUE(retry.idempotent);
    fixture.relations[0].sendGainDb = -6;
    fixture.Refresh();
    EXPECT_EQ(InstallContainerWorldV1(engine, fixture.world).status, WorldInstallStatus::Backpressure);
    EXPECT_EQ(engine.variables.CurrentWorldGeneration(), generation);
    EXPECT_EQ(engine.acceptedSignalPlan.get(), first);
    engine.nullBackend.Pump(8);
    EXPECT_EQ(engine.buses.InstalledSignals(), first);
    const auto bus = engine.buses.FindIndex(source);
    EXPECT_EQ(engine.buses.BusOutputLayout(bus), static_cast<std::uint32_t>(VN_CHANNEL_LAYOUT_MONO));
    VariableServiceV1::PreparedCurrentWorldImageV1 stale;
    ASSERT_EQ(engine.variables.PrepareCurrentWorldImageV1(fixture.world, stale), WorldInstallStatus::Ok);
    EXPECT_TRUE(stale.PreservesContent());
    EXPECT_FALSE(stale.Idempotent());
    const auto changed = InstallContainerWorldV1(engine, fixture.world);
    ASSERT_EQ(changed.status, WorldInstallStatus::Ok);
    EXPECT_FALSE(changed.idempotent);
    EXPECT_EQ(changed.resultGeneration, generation);
    EXPECT_EQ(engine.variables.CurrentPlayableWorld().identity.runtimeSemanticFingerprint, playbackFingerprint);
    auto content = fixture.world;
    content.identity.capabilityMask &= ~kWorldCapabilitySignalRelations;
    EXPECT_EQ(playbackFingerprint, ComputeCurrentWorldFingerprintV5(content));
    EXPECT_FALSE(engine.variables.CanCommitPreparedCurrentWorldImageV1(stale));
    auto* second = engine.acceptedSignalPlan.get();
    ASSERT_NE(first, second);
    EXPECT_EQ(second->WorldIdentity(), first->WorldIdentity());
    ASSERT_EQ(Vn_CreateBus(harkentest::ObjectId(99), VN_BUS_MASTER), VN_OK);
    engine.nullBackend.Pump(2);
    EXPECT_EQ(engine.buses.InstalledSignals(), second);
    const auto current = engine.variables.CurrentWorldGeneration();
    fixture.relations[0].source.stableId = harkentest::ObjectId(999);
    fixture.Refresh();
    EXPECT_EQ(InstallContainerWorldV1(engine, fixture.world).status, WorldInstallStatus::InvalidWorld);
    EXPECT_EQ(engine.variables.CurrentWorldGeneration(), current);
    EXPECT_EQ(engine.acceptedSignalPlan.get(), second);
    fixture.relations[0].source.stableId = fixture.receivers[1].target.stableId;
    fixture.world.signalRelations = nullptr;
    fixture.world.signalRelationCount = 0;
    fixture.world.signalReceivers = nullptr;
    fixture.world.signalReceiverCount = 0;
    fixture.Refresh();
    ASSERT_EQ(InstallContainerWorldV1(engine, fixture.world).status, WorldInstallStatus::Ok);
    EXPECT_EQ(engine.variables.CurrentWorldGeneration(), generation);
    engine.nullBackend.Pump(2);
    EXPECT_EQ(engine.buses.InstalledSignals(), nullptr);
    fixture.world.signalRelations = fixture.relations.data();
    fixture.world.signalRelationCount = 2;
    fixture.world.signalReceivers = fixture.receivers.data();
    fixture.world.signalReceiverCount = 2;
    fixture.Refresh();
    ASSERT_EQ(InstallContainerWorldV1(engine, fixture.world).status, WorldInstallStatus::Ok);
    EXPECT_EQ(engine.variables.CurrentWorldGeneration(), generation);
    auto changedContent = fixture.world;
    changedContent.identity.projectId = harkentest::ObjectId(700);
    changedContent.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV7(changedContent);
    EXPECT_FALSE(engine.variables.SignalOnlyWorld(changedContent));
    ASSERT_EQ(Vn_Term(), VN_OK);
    session.Release();
    harkentest::EngineSession restarted(config);
    ASSERT_EQ(restarted.InitResult(), VN_OK);
    for (const auto& receiver : fixture.receivers)
        ASSERT_EQ(Vn_CreateBus(receiver.target.stableId, VN_BUS_MASTER), VN_OK);
    restarted.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(InstallContainerWorldV1(*restarted.Engine(), fixture.world).status, WorldInstallStatus::Ok);
    EXPECT_EQ(restarted.Engine()->variables.CurrentPlayableWorld().identity.runtimeSemanticFingerprint,
              playbackFingerprint);
}

TEST(CurrentWorldSignal, PairedPublicationCarriesSignalsInItsSingleTopologyCommand) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.maxBuses = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    SignalWorld fixture;
    for (const auto& receiver : fixture.receivers)
        ASSERT_EQ(Vn_CreateBus(receiver.target.stableId, VN_BUS_MASTER), VN_OK);
    engine.nullBackend.Pump(1);
    ProjectGenerationInstallViewV1 view;
    view.token = {11, 2, 3, 4};
    view.ownerId = 77; view.ownerRevision = 1;
    view.compiledWorld = fixture.world;
    PreparedProjectGenerationV1 prepared;
    ASSERT_EQ(PrepareProjectGenerationV1(engine, view, prepared).status, ProjectGenerationPrepareStatusV1::Ok);
    EXPECT_EQ(engine.acceptedSignalPlan, nullptr);
    const auto free = engine.commands.FreeSlots();
    // A prepared world must recheck backing at promotion, not rely on the
    // capacity observed when it was prepared.
    engine.mixer.ArenaForTests().SetCapacityForTests(1);
    EXPECT_EQ(CommitPreparedProjectGenerationV1(engine, prepared).status, ProjectGenerationCommitStatusV1::Backpressure);
    EXPECT_EQ(engine.commands.FreeSlots(), free);
    EXPECT_EQ(engine.acceptedSignalPlan, nullptr);
    engine.mixer.ArenaForTests().RestoreCapacityForTests();
    ASSERT_EQ(CommitPreparedProjectGenerationV1(engine, prepared).status, ProjectGenerationCommitStatusV1::Ok);
    ASSERT_NE(engine.acceptedSignalPlan, nullptr);
    EXPECT_EQ(engine.commands.FreeSlots(), free - 1);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(engine.buses.InstalledSignals(), engine.acceptedSignalPlan.get());
}

TEST(CurrentWorldSignal, PendingPlanBackingIsReleasedOnlyAfterRetirementDrain) {
    auto config = harkentest::MakeNullConfig(64, 64, 48000);
    config.maxBuses = 4;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    std::array<SignalRenderNodeInput, 64> nodes{};
    auto owner = SignalRenderPlan::Prepare({2, 64, 48000, VN_CHANNEL_LAYOUT_STEREO}, nodes, {}, {});
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(engine.mixer.PrepareSignalBacking(*owner));
    const auto old = engine.topologyPlans.Acquire();
    ASSERT_NE(old, kInvalidTopologyPlanIndex);
    engine.topologyPlans.SetSignals(old, std::move(owner));
    const auto newer = engine.topologyPlans.Acquire();
    ASSERT_NE(newer, kInvalidTopologyPlanIndex);
    // The latest empty plan does not make an older queued plan disappear.
    engine.mixer.ArenaForTests().SetCapacityForTests(4096);
    EXPECT_FALSE(engine.mixer.PrepareVoiceBacking(64));
    engine.topologyPlans.Retire(old);
    EXPECT_FALSE(engine.mixer.PrepareVoiceBacking(64));
    engine.topologyPlans.DrainRetire();
    EXPECT_TRUE(engine.mixer.PrepareVoiceBacking(64));
    engine.mixer.ArenaForTests().RestoreCapacityForTests();
    engine.topologyPlans.Recycle(newer);
}
} // namespace
