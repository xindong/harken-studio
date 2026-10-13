#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/BusTopologySync.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using HarkenEditor::Bridge::BusTopologyOperation;
using HarkenEditor::Bridge::BusTopologyState;
using HarkenEditor::Bridge::EngineResult;
using HarkenEditor::Bridge::RuntimeBusDefinition;
using HarkenEditor::Bridge::RuntimeBusSendDefinition;
using HarkenEditor::Bridge::kEngineBackpressure;
using HarkenEditor::Bridge::kEngineOk;
using HarkenEditor::Bridge::kEngineUnknownId;
using HarkenEditor::Bridge::Detail::BusTopologyBackend;
using HarkenEditor::Bridge::Detail::BusTopologySync;
using HarkenEditor::Bridge::Detail::ConfirmedBusPropertyPatch;

struct FakeBackend {
    std::vector<std::string> calls;
    harkenstudio::ObjectId failBus{};
    std::vector<harkenstudio::ObjectId> snapshotBusIds;
    std::vector<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> snapshotSends;
    harkenstudio::ObjectId propertyBackpressureBus{};
    bool propertyBackpressureOnce = false;
    bool propertyBackpressureReturned = false;
    bool snapshotBackpressureOnce = false;
    bool snapshotBackpressureReturned = false;
    EngineResult PropertyResult(harkenstudio::ObjectId busId) {
        if (propertyBackpressureOnce &&
            busId == propertyBackpressureBus &&
            !propertyBackpressureReturned) {
            propertyBackpressureReturned = true;
            return kEngineBackpressure;
        }
        return kEngineOk;
    }

    BusTopologyBackend Make() {
        BusTopologyBackend backend;
        backend.applySnapshot =
            [this](const std::vector<RuntimeBusDefinition>& buses,
                   const std::vector<RuntimeBusSendDefinition>& sends,
                   uint64_t baseRevision, uint64_t revision) {
                calls.push_back(
                    "snapshot:" + std::to_string(baseRevision) + ":" +
                    std::to_string(revision) + ":" +
                    std::to_string(buses.size()) + ":" +
                    std::to_string(sends.size()));
                snapshotBusIds.clear();
                snapshotSends.clear();
                for (const auto& bus : buses)
                    snapshotBusIds.push_back(bus.id);
                for (const auto& send : sends)
                    snapshotSends.emplace_back(send.sourceId, send.targetId);
                if (snapshotBackpressureOnce
                    && !snapshotBackpressureReturned) {
                    snapshotBackpressureReturned = true;
                    return kEngineBackpressure;
                }
                if (std::find(snapshotBusIds.begin(), snapshotBusIds.end(),
                              failBus) != snapshotBusIds.end())
                    return kEngineUnknownId;
                return kEngineOk;
            };
        backend.setVolume = [this](harkenstudio::ObjectId busId, float,
                                   uint32_t fadeMs) {
            calls.push_back("volume:" + harkenstudio::ObjectIdString(busId) + ":" +
                            std::to_string(fadeMs));
            return PropertyResult(busId);
        };
        backend.setOutputVolume = [this](harkenstudio::ObjectId busId, float,
                                         uint32_t fadeMs) {
            calls.push_back("output:" + harkenstudio::ObjectIdString(busId) + ":" +
                            std::to_string(fadeMs));
            return PropertyResult(busId);
        };
        backend.setMute = [this](harkenstudio::ObjectId busId, bool,
                                 uint32_t fadeMs) {
            calls.push_back("mute:" + harkenstudio::ObjectIdString(busId) + ":" +
                            std::to_string(fadeMs));
            return kEngineOk;
        };
        backend.setSolo = [this](harkenstudio::ObjectId busId, bool,
                                 uint32_t fadeMs) {
            calls.push_back("solo:" + harkenstudio::ObjectIdString(busId) + ":" +
                            std::to_string(fadeMs));
            return kEngineOk;
        };
        return backend;
    }
};

RuntimeBusDefinition Bus(uint64_t id, uint64_t parentId,
                         const char* name) {
    RuntimeBusDefinition bus;
    bus.id = harkentest::StudioObjectId(id);
    bus.parentId = harkentest::StudioObjectId(parentId);
    bus.name = QString::fromUtf8(name);
    return bus;
}

ConfirmedBusPropertyPatch Patch(const RuntimeBusDefinition& bus) {
    ConfirmedBusPropertyPatch patch;
    patch.busId = bus.id;
    patch.busName = bus.name;
    patch.volumeDb = bus.volumeDb;
    patch.outputVolumeDb = bus.outputVolumeDb;
    patch.mute = bus.mute;
    patch.solo = bus.solo;
    return patch;
}

TEST(BusTopologySync, AtomicSnapshotRetriesExactlyAndOrdersPropertiesAfterIt) {
    FakeBackend fake;
    fake.snapshotBackpressureOnce = true;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusDefinition parent = Bus(10, 1, "Parent");
    parent.volumeDb = -3.0f;
    parent.mute = true;
    RuntimeBusDefinition child = Bus(20, 10, "Child");
    RuntimeBusSendDefinition send;
    send.sourceId = harkentest::StudioObjectId(20);
    send.targetId = harkentest::StudioObjectId(1);
    send.levelDb = -6.0206f;

    sync.Submit({child, parent}, {send}, 25);
    sync.Pump();
    ASSERT_EQ(BusTopologyState::Syncing, sync.State());
    ASSERT_EQ(1u, fake.calls.size());
    EXPECT_EQ("snapshot:0:1:2:1", fake.calls[0]);

    sync.Pump();
    EXPECT_EQ(BusTopologyState::Ready, sync.State());
    ASSERT_GE(fake.calls.size(), 4u);
    EXPECT_EQ("snapshot:0:1:2:1", fake.calls[1]);
    EXPECT_EQ("volume:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":25", fake.calls[2]);
    EXPECT_EQ("mute:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":25", fake.calls[3]);

    const std::size_t identicalCallCount = fake.calls.size();
    sync.Submit({child, parent}, {send}, 25);
    sync.Pump();
    EXPECT_EQ(identicalCallCount, fake.calls.size());

    child.parentId = harkentest::StudioObjectId(1);
    sync.Submit({child, parent}, {send}, 25);
    sync.Pump();
    EXPECT_EQ(BusTopologyState::Ready, sync.State());
    ASSERT_GT(fake.calls.size(), identicalCallCount);
    EXPECT_EQ("snapshot:1:2:2:1", fake.calls[identicalCallCount]);
}

TEST(BusTopologySync, PreservesIdsAboveUint32AcrossTopologyAndSends) {
    constexpr uint64_t parentId = UINT64_C(0x100000009);
    constexpr uint64_t childId = UINT64_C(0x10000000a);
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusSendDefinition send;
    send.sourceId = harkentest::StudioObjectId(childId);
    send.targetId = harkentest::StudioObjectId(parentId);
    send.levelDb = -12.0f;

    sync.Submit({Bus(childId, parentId, "Child"),
                 Bus(parentId, 1, "Parent")}, {send});
    sync.Pump();

    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0], "snapshot:0:1:2:1");
    EXPECT_EQ(fake.snapshotBusIds,
              (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(parentId), harkentest::StudioObjectId(childId)}));
    EXPECT_EQ(fake.snapshotSends,
              (std::vector<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>>{
                  {harkentest::StudioObjectId(childId), harkentest::StudioObjectId(parentId)}}));
    EXPECT_EQ(sync.ConfirmedBusIds(),
              (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(1), harkentest::StudioObjectId(parentId), harkentest::StudioObjectId(childId)}));
}

TEST(BusTopologySync, DoesNotConfirmATerminallyRefusedBus) {
    FakeBackend fake;
    fake.failBus = harkentest::StudioObjectId(20);
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));

    sync.Submit({Bus(10, 1, "Parent"), Bus(20, 10, "Child")}, {});
    sync.Pump();

    EXPECT_EQ(sync.State(), BusTopologyState::Failed);
    EXPECT_EQ(sync.Failure().operation, BusTopologyOperation::ApplySnapshot);
    EXPECT_EQ(sync.Failure().result, kEngineUnknownId);
    EXPECT_EQ(sync.Failure().busId, harkentest::StudioObjectId(0u));
    EXPECT_EQ(sync.ConfirmedBusIds(),
              (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(1)}));
}

TEST(BusTopologySync, ConnectsSendsOnlyAfterBothBusesExist) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusSendDefinition send;
    send.sourceId = harkentest::StudioObjectId(20);
    send.targetId = harkentest::StudioObjectId(10);
    send.levelDb = -6.0206f;

    sync.Submit({Bus(20, 10, "Child"), Bus(10, 1, "Parent")},
                {send});
    sync.Pump();

    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0], "snapshot:0:1:2:1");
    EXPECT_EQ(fake.snapshotSends,
              (std::vector<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>>{{harkentest::StudioObjectId(20), harkentest::StudioObjectId(10)}}));
}

TEST(BusTopologySync, TreatsTheAuthoredMasterAsTheExistingRoot) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusDefinition master = Bus(1, 0, "Master Bus");
    master.volumeDb = -3.0f;

    sync.Submit({master, Bus(10, 1, "SFX")}, {});
    sync.Pump();

    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    ASSERT_EQ(fake.calls.size(), 2u);
    EXPECT_EQ(fake.calls[0], "snapshot:0:1:1:0");
    EXPECT_EQ(fake.calls[1], "volume:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(1)) + ":0");
}

TEST(BusTopologySync, PreservesMixerFadeAcrossBackpressureRetry) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    sync.Submit({Bus(10, 1, "SFX")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    fake.calls.clear();

    RuntimeBusDefinition changed = Bus(10, 1, "SFX");
    changed.volumeDb = -6.0f;
    changed.mute = true;
    changed.solo = true;
    fake.propertyBackpressureBus = harkentest::StudioObjectId(10);
    fake.propertyBackpressureOnce = true;

    sync.Submit({changed}, {}, 250);
    sync.Pump();

    ASSERT_EQ(sync.State(), BusTopologyState::Syncing);
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0], "volume:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":250");

    sync.Pump();

    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    ASSERT_EQ(fake.calls.size(), 4u);
    EXPECT_EQ(fake.calls[1], "volume:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":250");
    EXPECT_EQ(fake.calls[2], "mute:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":250");
    EXPECT_EQ(fake.calls[3], "solo:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":250");

    fake.calls.clear();
    sync.Submit({changed}, {});

    EXPECT_EQ(sync.State(), BusTopologyState::Ready);
    EXPECT_TRUE(fake.calls.empty());
}

TEST(BusTopologySync, RepeatedSnapshotIsIdempotentAndRemovalIsAtomic) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    const std::vector<RuntimeBusDefinition> buses{
        Bus(10, 1, "Parent"), Bus(20, 10, "Child")};

    sync.Submit(buses, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    fake.calls.clear();

    sync.Submit(buses, {});
    EXPECT_EQ(sync.State(), BusTopologyState::Ready);
    EXPECT_TRUE(fake.calls.empty());

    sync.Submit({}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0], "snapshot:1:2:0:0");
}

TEST(BusTopologySync, ReparentAndRemovalUseOneSnapshot) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    sync.Submit(
        {Bus(10, 1, "OldParent"), Bus(20, 10, "RetainedChild")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    fake.calls.clear();

    sync.Submit({Bus(20, 1, "RetainedChild")}, {});
    sync.Pump();

    ASSERT_EQ(sync.State(), BusTopologyState::Ready);
    ASSERT_EQ(fake.calls.size(), 1u);
    EXPECT_EQ(fake.calls[0], "snapshot:1:2:1:0");
}

TEST(BusTopologySync, RejectsAParentCycleBeforeCallingTheBackend) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));

    sync.Submit({Bus(10, 20, "A"), Bus(20, 10, "B")}, {});

    EXPECT_EQ(sync.State(), BusTopologyState::Failed);
    EXPECT_EQ(sync.Failure().operation, BusTopologyOperation::ApplySnapshot);
    EXPECT_EQ(sync.Failure().result,
              HarkenEditor::Bridge::kEngineInvalidArg);
    EXPECT_TRUE(fake.calls.empty());
}

TEST(BusTopologySync, RejectsNonMasterZeroParentWithoutReinterpretingIt) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));

    sync.Submit({Bus(10, 0, "Orphan")}, {});

    EXPECT_EQ(sync.State(), BusTopologyState::Failed);
    EXPECT_EQ(sync.Failure().operation, BusTopologyOperation::ApplySnapshot);
    EXPECT_EQ(sync.Failure().result, kEngineUnknownId);
    EXPECT_EQ(sync.Failure().busId, harkentest::StudioObjectId(10u));
    EXPECT_EQ(sync.Failure().relatedBusId, harkentest::StudioObjectId(0u));
    EXPECT_TRUE(fake.calls.empty());
}

TEST(BusTopologySync,
     RejectedTopologyReplaysPropertiesForStillConfirmedBuses) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.volumeDb = -3.0f;
    sync.Submit({accepted}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);

    RuntimeBusDefinition edited = accepted;
    edited.volumeDb = -9.0f;
    edited.mute = true;
    edited.solo = true;
    fake.failBus = harkentest::StudioObjectId(20);
    sync.Submit({edited, Bus(20, 1, "Rejected")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Failed);

    fake.propertyBackpressureBus = harkentest::StudioObjectId(10);
    fake.propertyBackpressureOnce = true;
    sync.BeginConfirmedPropertyReplay({Patch(edited)});
    sync.PumpConfirmedPropertyReplay();
    EXPECT_TRUE(sync.ConfirmedPropertyReplayPending());
    EXPECT_EQ(sync.State(), BusTopologyState::Failed);
    auto confirmed = sync.ConfirmedBuses();
    auto bus = std::find_if(confirmed.begin(), confirmed.end(),
                            [](const RuntimeBusDefinition& row) {
                                return row.id == harkentest::StudioObjectId(10);
                            });
    ASSERT_NE(bus, confirmed.end());
    EXPECT_FLOAT_EQ(bus->volumeDb, -3.0f);

    sync.PumpConfirmedPropertyReplay();
    EXPECT_FALSE(sync.ConfirmedPropertyReplayPending());
    EXPECT_EQ(sync.State(), BusTopologyState::Failed);
    confirmed = sync.ConfirmedBuses();
    bus = std::find_if(confirmed.begin(), confirmed.end(),
                       [](const RuntimeBusDefinition& row) {
                           return row.id == harkentest::StudioObjectId(10);
                       });
    ASSERT_NE(bus, confirmed.end());
    EXPECT_FLOAT_EQ(bus->volumeDb, -9.0f);
    EXPECT_TRUE(bus->mute);
    EXPECT_TRUE(bus->solo);
}

TEST(BusTopologySync,
     NewCanonicalSubmitSupersedesBackpressuredPropertyReplay) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.volumeDb = -3.0f;
    sync.Submit({accepted}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);

    RuntimeBusDefinition edited = accepted;
    edited.volumeDb = -9.0f;
    edited.mute = true;
    fake.failBus = harkentest::StudioObjectId(20);
    sync.Submit({edited, Bus(20, 1, "Rejected")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Failed);

    fake.propertyBackpressureBus = harkentest::StudioObjectId(10);
    fake.propertyBackpressureOnce = true;
    sync.BeginConfirmedPropertyReplay({Patch(edited)});
    sync.PumpConfirmedPropertyReplay();
    ASSERT_TRUE(sync.ConfirmedPropertyReplayPending());

    edited.solo = true;
    sync.Submit({edited}, {});
    sync.Pump();

    EXPECT_EQ(sync.State(), BusTopologyState::Ready);
    EXPECT_FALSE(sync.ConfirmedPropertyReplayPending());
    const auto confirmed = sync.ConfirmedBuses();
    const auto bus = std::find_if(
        confirmed.begin(), confirmed.end(),
        [](const RuntimeBusDefinition& row) { return row.id == harkentest::StudioObjectId(10); });
    ASSERT_NE(bus, confirmed.end());
    EXPECT_FLOAT_EQ(bus->volumeDb, -9.0f);
    EXPECT_TRUE(bus->mute);
    EXPECT_TRUE(bus->solo);
}

TEST(BusTopologySync,
     ConsecutiveRejectedSubmissionsPreserveBackpressuredPropertyReplay) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.volumeDb = -3.0f;
    sync.Submit({accepted}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);

    RuntimeBusDefinition edited = accepted;
    edited.volumeDb = -9.0f;
    fake.failBus = harkentest::StudioObjectId(20);
    sync.Submit({edited, Bus(20, 1, "Rejected")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Failed);

    fake.propertyBackpressureBus = harkentest::StudioObjectId(10);
    fake.propertyBackpressureOnce = true;
    sync.BeginConfirmedPropertyReplay({Patch(edited)});
    sync.PumpConfirmedPropertyReplay();
    ASSERT_TRUE(sync.ConfirmedPropertyReplayPending());

    fake.failBus = harkentest::StudioObjectId(30);
    sync.Submit({edited, Bus(30, 1, "RejectedAgain")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Failed);
    ASSERT_TRUE(sync.ConfirmedPropertyReplayPending());

    sync.PumpConfirmedPropertyReplay();
    EXPECT_FALSE(sync.ConfirmedPropertyReplayPending());
    const auto confirmed = sync.ConfirmedBuses();
    const auto bus = std::find_if(
        confirmed.begin(), confirmed.end(),
        [](const RuntimeBusDefinition& row) { return row.id == harkentest::StudioObjectId(10); });
    ASSERT_NE(bus, confirmed.end());
    EXPECT_FLOAT_EQ(bus->volumeDb, -9.0f);
}

TEST(BusTopologySync,
     LaterRejectedPatchDoesNotOverwriteBackpressuredFields) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.volumeDb = -3.0f;
    sync.Submit({accepted}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Ready);

    RuntimeBusDefinition edited = accepted;
    edited.volumeDb = -9.0f;
    fake.failBus = harkentest::StudioObjectId(20);
    sync.Submit({edited, Bus(20, 1, "Rejected")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Failed);

    fake.propertyBackpressureBus = harkentest::StudioObjectId(10);
    fake.propertyBackpressureOnce = true;
    ConfirmedBusPropertyPatch volumePatch;
    volumePatch.busId = harkentest::StudioObjectId(10);
    volumePatch.volumeDb = -9.0f;
    sync.BeginConfirmedPropertyReplay({volumePatch});
    sync.PumpConfirmedPropertyReplay();
    ASSERT_TRUE(sync.ConfirmedPropertyReplayPending());

    fake.failBus = harkentest::StudioObjectId(30);
    sync.Submit({edited, Bus(30, 1, "RejectedAgain")}, {});
    sync.Pump();
    ASSERT_EQ(sync.State(), BusTopologyState::Failed);

    ConfirmedBusPropertyPatch mutePatch;
    mutePatch.busId = harkentest::StudioObjectId(10);
    mutePatch.mute = true;
    sync.BeginConfirmedPropertyReplay({mutePatch});
    sync.PumpConfirmedPropertyReplay();

    EXPECT_FALSE(sync.ConfirmedPropertyReplayPending());
    const auto confirmed = sync.ConfirmedBuses();
    const auto bus = std::find_if(
        confirmed.begin(), confirmed.end(),
        [](const RuntimeBusDefinition& row) { return row.id == harkentest::StudioObjectId(10); });
    ASSERT_NE(bus, confirmed.end());
    EXPECT_FLOAT_EQ(bus->volumeDb, -9.0f);
    EXPECT_TRUE(bus->mute);
}

TEST(BusTopologySync, RejectedSnapshotPreservesTheLastAcceptedSessionCache) {
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    auto session = HarkenEditor::Bridge::EngineSession::create(config);
    ASSERT_NE(session, nullptr);

    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.parentId = HarkenEditor::Bridge::kEngineMasterBusId;
    accepted.volumeDb = -3.0f;
    session->confirmActiveBusTopologyWithoutApply({accepted}, {});
    ASSERT_EQ(session->busTopologyState(), BusTopologyState::Ready);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(10)), -3.0f);
    EXPECT_EQ(session->runtimeBusIds(),
              (std::vector<harkenstudio::ObjectId>{HarkenEditor::Bridge::kEngineMasterBusId, harkentest::StudioObjectId(10)}));

    RuntimeBusDefinition rejected = Bus(20, 0, "Rejected");
    rejected.volumeDb = -12.0f;
    session->confirmActiveBusTopologyWithoutApply({rejected}, {});

    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Failed);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(10)), -3.0f);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(20)), 0.0f);
    EXPECT_EQ(session->runtimeBusIds(),
              (std::vector<harkenstudio::ObjectId>{HarkenEditor::Bridge::kEngineMasterBusId, harkentest::StudioObjectId(10)}));

    session->setBusVolumeDbById(harkentest::StudioObjectId(20), -18.0f);
    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Failed);

    session->setBusVolumeDbById(harkentest::StudioObjectId(10), -6.0f);
    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Ready);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(10)), -6.0f);
    EXPECT_EQ(session->runtimeBusIds(),
              (std::vector<harkenstudio::ObjectId>{HarkenEditor::Bridge::kEngineMasterBusId, harkentest::StudioObjectId(10)}));
}

TEST(BusTopologySync,
     RuntimeRefusedSnapshotPreservesTheLastAcceptedSessionCache) {
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    auto session = HarkenEditor::Bridge::EngineSession::create(config);
    ASSERT_NE(session, nullptr);

    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.parentId = HarkenEditor::Bridge::kEngineMasterBusId;
    accepted.volumeDb = -3.0f;
    session->confirmActiveBusTopologyWithoutApply({accepted}, {});
    ASSERT_EQ(session->busTopologyState(), BusTopologyState::Ready);

    std::vector<RuntimeBusDefinition> overCapacity;
    for (uint64_t id = 100; id < 132; ++id)
        overCapacity.push_back(Bus(id, 0, "Rejected"));
    session->confirmActiveBusTopologyWithoutApply(overCapacity, {});

    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Failed);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(10)), -3.0f);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(100)), 0.0f);
    EXPECT_EQ(session->runtimeBusIds(),
              (std::vector<harkenstudio::ObjectId>{HarkenEditor::Bridge::kEngineMasterBusId, harkentest::StudioObjectId(10)}));

    session->setBusVolumeDbById(harkentest::StudioObjectId(100), -18.0f);
    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Failed);

    session->setBusVolumeDbById(harkentest::StudioObjectId(10), -9.0f);
    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Ready);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(10)), -9.0f);
    EXPECT_EQ(session->runtimeBusIds(),
              (std::vector<harkenstudio::ObjectId>{HarkenEditor::Bridge::kEngineMasterBusId, harkentest::StudioObjectId(10)}));
}

TEST(BusTopologySync,
     RuntimeRefusedSnapshotDoesNotPartiallyApplyCandidateProperties) {
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    auto session = HarkenEditor::Bridge::EngineSession::create(config);
    ASSERT_NE(session, nullptr);

    RuntimeBusDefinition accepted = Bus(10, 1, "Accepted");
    accepted.parentId = HarkenEditor::Bridge::kEngineMasterBusId;
    accepted.volumeDb = -3.0f;
    session->confirmActiveBusTopologyWithoutApply({accepted}, {});
    ASSERT_EQ(session->busTopologyState(), BusTopologyState::Ready);

    RuntimeBusDefinition edited = accepted;
    edited.volumeDb = -9.0f;
    std::vector<RuntimeBusDefinition> overCapacity{edited};
    for (uint64_t id = 100; id < 132; ++id)
        overCapacity.push_back(Bus(id, 0, "Rejected"));
    session->confirmActiveBusTopologyWithoutApply(overCapacity, {});

    EXPECT_EQ(session->busTopologyState(), BusTopologyState::Failed);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(10)), -3.0f);
    EXPECT_FLOAT_EQ(session->busVolumeDbById(harkentest::StudioObjectId(100)), 0.0f);
    EXPECT_EQ(session->runtimeBusIds(),
              (std::vector<harkenstudio::ObjectId>{HarkenEditor::Bridge::kEngineMasterBusId, harkentest::StudioObjectId(10)}));
}

TEST(BusTopologySync, ConfirmWithoutApplyDoesNotCallSetOutputVolume) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    auto bus = Bus(10, 1, "Output");
    bus.outputVolumeDb = -6.0f;
    sync.ConfirmWithoutApply({bus}, {});
    EXPECT_EQ(BusTopologyState::Ready, sync.State());
    EXPECT_TRUE(fake.calls.empty());
    const auto confirmed = sync.ConfirmedBuses();
    ASSERT_EQ(2u, confirmed.size());
    EXPECT_FLOAT_EQ(-6.0f, confirmed[1].outputVolumeDb);
}

TEST(BusTopologySync, OutputVolumeRetriesAndReplaysIndependently) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));
    auto bus = Bus(10, 1, "Output");
    bus.volumeDb = -3.0f;
    sync.Submit({bus}, {});
    sync.Pump();
    ASSERT_EQ(BusTopologyState::Ready, sync.State());
    fake.calls.clear();
    fake.propertyBackpressureBus = harkentest::StudioObjectId(10);
    fake.propertyBackpressureOnce = true;
    bus.outputVolumeDb = -6.0f;
    sync.Submit({bus}, {}, 20);
    sync.Pump();
    EXPECT_EQ(BusTopologyState::Syncing, sync.State());
    sync.Pump();
    ASSERT_EQ(BusTopologyState::Ready, sync.State());
    EXPECT_EQ((std::vector<std::string>{"output:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":20", "output:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":20"}), fake.calls);
    const auto confirmed = sync.ConfirmedBuses();
    ASSERT_EQ(2u, confirmed.size());
    EXPECT_FLOAT_EQ(-3.0f, confirmed[1].volumeDb);
    EXPECT_FLOAT_EQ(-6.0f, confirmed[1].outputVolumeDb);
    fake.calls.clear();
    bus.outputVolumeDb = 0.0f;
    sync.BeginConfirmedPropertyReplay({Patch(bus)}, 10);
    sync.PumpConfirmedPropertyReplay();
    EXPECT_EQ((std::vector<std::string>{"output:" + harkenstudio::ObjectIdString(harkentest::StudioObjectId(10)) + ":10"}), fake.calls);
    EXPECT_FLOAT_EQ(0.0f, sync.ConfirmedBuses()[1].outputVolumeDb);
}

} // namespace
