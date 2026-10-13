#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <algorithm>
#include <array>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint64_t kOwner = 0x425553544F504F4Cull;

VnBusTopologyBus Bus(VnBusId id, VnBusId parent) {
    VnBusTopologyBus row{};
    row.structSize = sizeof(row);
    row.busId = id;
    row.parentId = parent;
    return row;
}

VnBusTopologySend Send(VnBusId from, VnBusId to, float levelDb = 0.0f,
                       std::uint32_t rampMs = 0) {
    VnBusTopologySend row{};
    row.structSize = sizeof(row);
    row.fromBusId = from;
    row.toBusId = to;
    row.levelDb = levelDb;
    row.rampMs = rampMs;
    return row;
}

VnBusTopologyDesc Topology(std::uint64_t baseRevision,
                           std::uint64_t revision,
                           const VnBusTopologyBus* buses,
                           std::uint32_t busCount,
                           const VnBusTopologySend* sends = nullptr,
                           std::uint32_t sendCount = 0) {
    VnBusTopologyDesc desc{};
    desc.structSize = sizeof(desc);
    desc.ownerId = kOwner;
    desc.baseRevision = baseRevision;
    desc.revision = revision;
    desc.busCount = busCount;
    desc.busStructSize = sizeof(VnBusTopologyBus);
    desc.buses = buses;
    desc.sendCount = sendCount;
    desc.sendStructSize = sizeof(VnBusTopologySend);
    desc.sends = sends;
    return desc;
}

VnSnapshot Snapshot() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(snapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

void Pump(harken::Engine* engine) {
    Capture capture;
    PumpAndCapture(engine, 1, capture);
}

TEST(BusTopologySnapshot, OnePlanRevisionAndCanonicalRetry) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 8;
    config.busSendSlotsPerBus = 3;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER), Bus(harkentest::ObjectId(11), harkentest::ObjectId(10))};
    std::array sends{Send(harkentest::ObjectId(11), VN_BUS_MASTER, -6.0f, 20)};
    VnBusTopologyDesc desc =
        Topology(0, 1, buses.data(), buses.size(), sends.data(), sends.size());
    const std::uint64_t plansBefore = session.Engine()->topologyPlanCount;

    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(plansBefore + 1, session.Engine()->topologyPlanCount);
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(10)));
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(11)));

    VnSnapshot accepted = Snapshot();
    EXPECT_EQ(1u, accepted.acceptedBusTopologyRevision);
    EXPECT_EQ(0u, accepted.installedBusTopologyRevision);

    std::reverse(buses.begin(), buses.end());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(plansBefore + 1, session.Engine()->topologyPlanCount);

    Pump(session.Engine());
    const VnSnapshot installed = Snapshot();
    EXPECT_EQ(installed.acceptedBusTopologyRevision,
              installed.installedBusTopologyRevision);
}

TEST(BusTopologySnapshot,
     VariableOwnerBlocksAProspectiveSameGenerationSoloGainFlip) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 4;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER), Bus(harkentest::ObjectId(11), VN_BUS_MASTER)};
    VnBusTopologyDesc desc =
        Topology(0, 1, buses.data(), buses.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    Pump(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(harkentest::ObjectId(10), 1, 0));
    Pump(session.Engine());
    ASSERT_EQ(VN_OK, Vn_Update());

    harken::Engine* engine = session.Engine();
    const std::uint32_t busA = engine->buses.FindIndex(harkentest::ObjectId(10));
    const std::uint32_t busB = engine->buses.FindIndex(harkentest::ObjectId(11));
    ASSERT_NE(harken::kInvalidBusIndex, busA);
    ASSERT_NE(harken::kInvalidBusIndex, busB);
    ASSERT_EQ(0u, engine->buses.ParentOf(busB));
    const harken::VariableSliceRef owner{37u, 4u};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        busB, harken::kBusTargetBusVolume, owner,
        engine->busTargets.TargetRevision(
            busB, harken::kBusTargetBusVolume)));
    buses[1].parentId = harkentest::ObjectId(10);
    desc.baseRevision = 1;
    desc.revision = 2;
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t planCount = engine->topologyPlanCount;

    EXPECT_EQ(VN_BACKPRESSURE, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(0u, engine->buses.ParentOf(busB));
    EXPECT_EQ(1u, Snapshot().acceptedBusTopologyRevision);
    EXPECT_EQ(planCount, engine->topologyPlanCount);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        busB, harken::kBusTargetBusVolume));

    engine->busTargets.ReleaseClaim(
        busB, harken::kBusTargetBusVolume, owner);
    EXPECT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(busA, engine->buses.ParentOf(busB));
    EXPECT_EQ(2u, Snapshot().acceptedBusTopologyRevision);
    EXPECT_EQ(planCount + 1u, engine->topologyPlanCount);
}

TEST(BusTopologySnapshot, ThirtyBusGraphAndSendLevelRevisionUseOneCommandEach) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 31;
    config.busSendSlotsPerBus = 1;
    config.commandQueueDepth = 2;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array<VnBusTopologyBus, 30> buses{};
    for (std::size_t i = 0; i < buses.size(); ++i) {
        buses[i] = Bus(harkentest::ObjectId(10 + i), VN_BUS_MASTER);
    }

    std::array<VnBusTopologySend, 15> sends{};
    for (std::size_t i = 0; i < sends.size(); ++i) {
        const VnBusId from = harkentest::ObjectId(10 + i * 2);
        sends[i] = Send(from, harkentest::ObjectId(11 + i * 2), -3.0f, 10);
    }

    VnBusTopologyDesc desc =
        Topology(0, 1, buses.data(), buses.size(), sends.data(), sends.size());
    const std::uint64_t plansBefore = session.Engine()->topologyPlanCount;
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(plansBefore + 1, session.Engine()->topologyPlanCount);

    Pump(session.Engine());
    const VnSnapshot installed = Snapshot();
    EXPECT_EQ(31u, installed.activeBusCount);
    EXPECT_EQ(1u, installed.installedBusTopologyRevision);

    for (VnBusTopologySend& send : sends) {
        send.levelDb = -12.0f;
        send.rampMs = 25;
    }
    desc.baseRevision = 1;
    desc.revision = 2;
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(plansBefore + 2, session.Engine()->topologyPlanCount);

    Pump(session.Engine());
    EXPECT_EQ(2u, Snapshot().installedBusTopologyRevision);
}

TEST(BusTopologySnapshot, FullCapacityReplacementReusesGenerationalSlots) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 3;
    config.busSendSlotsPerBus = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array first{Bus(harkentest::ObjectId(10), VN_BUS_MASTER), Bus(harkentest::ObjectId(11), harkentest::ObjectId(10))};
    VnBusTopologyDesc firstDesc =
        Topology(0, 1, first.data(), first.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&firstDesc));
    Pump(session.Engine());
    const VnSnapshot before = Snapshot();

    std::array second{Bus(harkentest::ObjectId(20), VN_BUS_MASTER), Bus(harkentest::ObjectId(21), harkentest::ObjectId(20))};
    VnBusTopologyDesc secondDesc =
        Topology(1, 2, second.data(), second.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&secondDesc));
    EXPECT_EQ(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(10)));
    EXPECT_EQ(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(11)));
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(20)));
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(21)));

    Pump(session.Engine());
    const VnSnapshot after = Snapshot();
    EXPECT_EQ(3u, after.busCount);
    EXPECT_EQ(3u, after.activeBusCount);
    EXPECT_EQ(0u, after.freeBusSlotCount);
    EXPECT_EQ(before.busSlotReuseCount + 2, after.busSlotReuseCount);
    EXPECT_GT(after.busGenerations[1], before.busGenerations[1]);
    EXPECT_GT(after.busGenerations[2], before.busGenerations[2]);
}

TEST(BusTopologySnapshot, PureRemovalWaitsForAudioAcknowledgementBeforeSlotReuse) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER)};
    auto install = Topology(0, 1, buses.data(), buses.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&install));
    Pump(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto index = engine->buses.FindIndex(harkentest::ObjectId(10));
    const auto generation = engine->buses.GenerationAt(index);

    auto remove = Topology(1, 2, nullptr, 0);
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&remove));
    const auto revision = engine->acceptedBusTopologyRevision;
    EXPECT_FALSE(engine->buses.AliveAt(index));
    EXPECT_EQ(revision, engine->buses.RetirementRevisionAt(index));
    EXPECT_EQ(generation, engine->busRetirements[index].generation);
    EXPECT_EQ(kOwner, engine->busRetirements[index].ownerId);
    EXPECT_EQ(kOwner, engine->busTopologyOwnerIds[index]);
    EXPECT_EQ(VN_NO_BUS, Vn_CreateBus(harkentest::ObjectId(20), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(revision, engine->buses.RetirementRevisionAt(index));

    Pump(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(index));
    EXPECT_EQ(0u, engine->busRetirements[index].revision);
    EXPECT_EQ(0u, engine->busTopologyOwnerIds[index]);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(20), VN_BUS_MASTER));
    EXPECT_EQ(index, engine->buses.FindIndex(harkentest::ObjectId(20)));
    EXPECT_GT(engine->buses.GenerationAt(index), generation);
}

TEST(BusTopologySnapshot, RetirementCensusFixtureRequiresExactPendingReferenceToDrain) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER)};
    auto install = Topology(0, 1, buses.data(), buses.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&install));
    Pump(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto index = engine->buses.FindIndex(harkentest::ObjectId(10));
    const auto generation = engine->buses.GenerationAt(index);
    auto remove = Topology(1, 2, nullptr, 0);
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&remove));
    Pump(engine);

    ASSERT_FALSE(engine->pendingStarts.empty());
    auto& pending = engine->pendingStarts[0];
    ASSERT_FALSE(pending.active);
    pending.active = true;
    pending.busIndex = index;
    pending.busGeneration = generation;
    EXPECT_EQ(0u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(index));
    pending.busGeneration = generation + 1;
    EXPECT_EQ(1u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(index));
    pending.active = false;
    EXPECT_EQ(0u, harken::DrainTopologyRetirementsV1(*engine));
}

TEST(BusTopologySnapshot, PublishedTimelineIdentityMigratesOffRemovedSlot) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER)};
    auto install = Topology(0, 1, buses.data(), buses.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&install));
    Pump(engine);
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto index = engine->buses.FindIndex(harkentest::ObjectId(10));
    const auto generation = engine->buses.GenerationAt(index);
    ASSERT_FALSE(engine->pendingStarts.empty());
    ASSERT_FALSE(engine->parkRecords.empty());
    ASSERT_FALSE(engine->timelineVoiceInstallIdentities.empty());
    auto& pending = engine->pendingStarts[0];
    pending.active = true;
    pending.busIndex = index;
    pending.busGeneration = generation;
    pending.voiceSends[0] = {true, index, -2.0f, generation};
    auto& parked = engine->parkRecords[0];
    parked.active = true;
    parked.facts.busIndex = index;
    parked.facts.busGeneration = generation;
    parked.voiceSends[0] = {true, index, -8.0f, generation};
    auto& identity = engine->timelineVoiceInstallIdentities[0];
    identity.renderHandle = 1;
    identity.busIndex = index;
    identity.busGeneration = generation;
    identity.voiceSends[0] = {true, index, -4.0f, generation};
    auto remove = Topology(1, 2, nullptr, 0);
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&remove));
    EXPECT_EQ(0u, identity.busIndex);
    EXPECT_EQ(engine->buses.GenerationAt(0), identity.busGeneration);
    EXPECT_FALSE(identity.voiceSends[0].active);
    EXPECT_EQ(0u, pending.busIndex);
    EXPECT_FALSE(pending.voiceSends[0].active);
    EXPECT_EQ(0u, parked.facts.busIndex);
    EXPECT_FALSE(parked.voiceSends[0].active);
    EXPECT_NE(0u, engine->buses.RetirementRevisionAt(index));
    EXPECT_EQ(0u, harken::DrainTopologyRetirementsV1(*engine));
    pending.active = false;
    parked.active = false;
    Pump(engine);
    EXPECT_EQ(1u, harken::DrainTopologyRetirementsV1(*engine));
    EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(index));
    identity = {};
}

TEST(BusTopologySnapshot, RemovedSourceMetersRetireForPureAndReusedSlots) {
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        VnConfig config = MakeNullConfig(2, kBlockFrames);
        config.maxBuses = 2;
        config.topologyPlanPoolCapacity = 4;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER)};
        auto install = Topology(0, 1, buses.data(), buses.size());
        ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&install));
        Pump(engine);
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto old = engine->buses.FindRef(harkentest::ObjectId(10));
        ASSERT_TRUE(old.Valid());
        ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine, harkentest::ObjectId(8001), harkentest::ObjectId(10),
            VN_BUS_METER_TAP_POST_EFFECTS, 0, 0));
        auto* probe = engine->meterDetectors.Find(harkentest::ObjectId(8001));
        ASSERT_NE(nullptr, probe);
        buses[0].busId = harkentest::ObjectId(20);
        auto remove = Topology(1, 2, reuse ? buses.data() : nullptr, reuse ? 1 : 0);
        ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&remove));
        EXPECT_EQ(harken::MeterSystem::State::Withdrawing, probe->state);
        EXPECT_NE(0u, probe->withdrawalLogicSequence);
        EXPECT_EQ(old.generation, probe->identity.sourceBusGeneration);
        if (reuse) {
            EXPECT_EQ(old.index, engine->buses.FindIndex(harkentest::ObjectId(20)));
            EXPECT_GT(engine->buses.GenerationAt(old.index), old.generation);
            EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(old.index));
        } else {
            EXPECT_NE(0u, engine->buses.RetirementRevisionAt(old.index));
            EXPECT_EQ(VN_NO_BUS, Vn_CreateBus(harkentest::ObjectId(20), VN_BUS_MASTER));
        }
        for (std::uint32_t i = 0; i < 4; ++i) {
            Pump(engine);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        EXPECT_EQ(harken::MeterSystem::State::Retired, probe->state);
        EXPECT_EQ(0u, engine->buses.RetirementRevisionAt(old.index));
        if (!reuse) EXPECT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(20), VN_BUS_MASTER));
    }
}

TEST(BusTopologySnapshot, RefusalsDoNotAdvanceOwnerOrGraphRevision) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 6;
    config.busSendSlotsPerBus = 2;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(50), VN_BUS_MASTER));
    Pump(session.Engine());

    const VnSnapshot before = Snapshot();
    const std::uint64_t plansBefore = session.Engine()->topologyPlanCount;
    std::array conflict{Bus(harkentest::ObjectId(50), VN_BUS_MASTER)};
    VnBusTopologyDesc conflictDesc =
        Topology(0, 1, conflict.data(), conflict.size());
    EXPECT_EQ(VN_IN_USE, Vn_ApplyBusTopology(&conflictDesc));
    EXPECT_EQ(plansBefore, session.Engine()->topologyPlanCount);
    EXPECT_EQ(before.acceptedBusTopologyRevision,
              Snapshot().acceptedBusTopologyRevision);

    std::array cycleBuses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER), Bus(harkentest::ObjectId(11), harkentest::ObjectId(10))};
    std::array cycleSends{Send(harkentest::ObjectId(10), harkentest::ObjectId(11))};
    VnBusTopologyDesc cycleDesc =
        Topology(0, 1, cycleBuses.data(), cycleBuses.size(),
                 cycleSends.data(), cycleSends.size());
    EXPECT_EQ(VN_SEND_CYCLE, Vn_ApplyBusTopology(&cycleDesc));
    EXPECT_EQ(plansBefore, session.Engine()->topologyPlanCount);

    cycleDesc.sendCount = 0;
    cycleDesc.sends = nullptr;
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&cycleDesc));
    EXPECT_EQ(plansBefore + 1, session.Engine()->topologyPlanCount);
}

TEST(BusTopologySnapshot, RevisionAndDescriptorValidationAreDeterministic) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 5;
    config.topologyPlanPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array buses{Bus(harkentest::ObjectId(10), VN_BUS_MASTER)};
    VnBusTopologyDesc desc = Topology(0, 1, buses.data(), buses.size());
    ASSERT_EQ(VN_OK, Vn_ApplyBusTopology(&desc));
    const std::uint64_t plans = session.Engine()->topologyPlanCount;

    buses[0].parentId = harkentest::ObjectId(999);
    EXPECT_EQ(VN_REVISION_CONFLICT, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(plans, session.Engine()->topologyPlanCount);

    desc.baseRevision = 0;
    desc.revision = 2;
    EXPECT_EQ(VN_REVISION_CONFLICT, Vn_ApplyBusTopology(&desc));

    desc.baseRevision = 1;
    buses[0].parentId = VN_BUS_MASTER;
    buses[0].structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ApplyBusTopology(&desc));
    EXPECT_EQ(plans, session.Engine()->topologyPlanCount);
}

} // namespace
