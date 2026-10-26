#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/BusTopologySync.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using HarkenEditor::Bridge::BusTopologyState;
using HarkenEditor::Bridge::RuntimeBusDefinition;
using HarkenEditor::Bridge::RuntimeBusSendDefinition;
using HarkenEditor::Bridge::kEngineOk;
using HarkenEditor::Bridge::Detail::BusTopologyBackend;
using HarkenEditor::Bridge::Detail::BusTopologySync;

struct FakeBackend {
    std::vector<std::string> calls;
    std::vector<harkenstudio::ObjectId> snapshotBusIds;
    std::vector<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> snapshotSends;

    BusTopologyBackend Make() {
        BusTopologyBackend backend;
        backend.applySnapshot =
            [this](const std::vector<RuntimeBusDefinition>& buses,
                   const std::vector<RuntimeBusSendDefinition>& sends,
                   uint64_t, uint64_t) {
                calls.push_back("snapshot");
                snapshotBusIds.clear();
                snapshotSends.clear();
                for (const auto& bus : buses)
                    snapshotBusIds.push_back(bus.id);
                for (const auto& send : sends)
                    snapshotSends.emplace_back(send.sourceId, send.targetId);
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
    const std::vector<harkenstudio::ObjectId> expectedIds{
        harkentest::StudioObjectId(parentId), harkentest::StudioObjectId(childId)};
    EXPECT_TRUE(std::is_permutation(fake.snapshotBusIds.begin(), fake.snapshotBusIds.end(),
                                    expectedIds.begin(), expectedIds.end()));
    EXPECT_EQ(fake.snapshotSends,
              (std::vector<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>>{
                  {harkentest::StudioObjectId(childId), harkentest::StudioObjectId(parentId)}}));
}

TEST(BusTopologySync, RejectsAParentCycleBeforeCallingTheBackend) {
    FakeBackend fake;
    BusTopologySync sync(fake.Make(), harkentest::StudioObjectId(1));

    sync.Submit({Bus(10, 20, "A"), Bus(20, 10, "B")}, {});

    EXPECT_EQ(sync.State(), BusTopologyState::Failed);
    EXPECT_TRUE(fake.calls.empty());
}

}
