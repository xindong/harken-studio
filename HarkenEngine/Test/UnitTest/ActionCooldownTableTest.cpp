#include "Common/ObjectIdTestSupport.h"
#include "harken/core/ActionCooldownTable.h"

#include <gtest/gtest.h>

namespace {

harken::ActionCooldownKey GlobalKey(std::uint64_t action,
                                   std::uint32_t event) {
    harken::ActionCooldownKey key{};
    key.owner = {harken::ActionDefinitionOwnerKind::World, 0, 7};
    key.actionStableId = harkentest::ObjectId(action);
    key.programSemanticFingerprint = action + 100;
    key.authoredEventIndex = event;
    return key;
}

TEST(ActionCooldownTable, PreparedRowsSuppressWithinTheSameAdmission) {
    harken::ActionCooldownTable table;
    ASSERT_TRUE(table.Reset(2));

    auto first = table.Prepare(GlobalKey(10, 0), 100, 120);
    ASSERT_EQ(first.status, harken::ActionCooldownPrepareStatus::Prepared);
    EXPECT_EQ(table.PreparedCount(), 1u);

    const auto repeated = table.Prepare(GlobalKey(10, 0), 100, 120);
    EXPECT_EQ(repeated.status,
              harken::ActionCooldownPrepareStatus::Suppressed);
    EXPECT_FALSE(repeated.token.Valid());

    ASSERT_TRUE(table.Commit(&first.token));
    EXPECT_EQ(table.LiveCount(), 1u);
    EXPECT_EQ(table.PreparedCount(), 0u);
}

TEST(ActionCooldownTable, ExpiryIsExclusiveAndSuppressionDoesNotRefresh) {
    harken::ActionCooldownTable table;
    ASSERT_TRUE(table.Reset(1));

    auto first = table.Prepare(GlobalKey(20, 0), 100, 120);
    ASSERT_EQ(first.status, harken::ActionCooldownPrepareStatus::Prepared);
    ASSERT_TRUE(table.Commit(&first.token));

    EXPECT_EQ(table.Prepare(GlobalKey(20, 0), 119, 200).status,
              harken::ActionCooldownPrepareStatus::Suppressed);
    auto boundary = table.Prepare(GlobalKey(20, 0), 120, 140);
    EXPECT_EQ(boundary.status,
              harken::ActionCooldownPrepareStatus::Prepared);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.PreparedCount(), 1u);
}

TEST(ActionCooldownTable, CapacityRefusalAndAbandonLeaveNoLiveMutation) {
    harken::ActionCooldownTable table;
    ASSERT_TRUE(table.Reset(2));

    auto first = table.Prepare(GlobalKey(30, 0), 0, 10);
    auto second = table.Prepare(GlobalKey(30, 1), 0, 10);
    ASSERT_EQ(first.status, harken::ActionCooldownPrepareStatus::Prepared);
    ASSERT_EQ(second.status, harken::ActionCooldownPrepareStatus::Prepared);
    EXPECT_EQ(table.HighWater(), 0u);

    EXPECT_EQ(table.Prepare(GlobalKey(30, 2), 0, 10).status,
              harken::ActionCooldownPrepareStatus::Capacity);
    EXPECT_EQ(table.RefusalCount(), 1u);
    ASSERT_TRUE(table.Abandon(&second.token));
    ASSERT_TRUE(table.Abandon(&first.token));
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.PreparedCount(), 0u);
}

TEST(ActionCooldownTable, OwnerAndEmitterCleanupAreExact) {
    harken::ActionCooldownTable table;
    ASSERT_TRUE(table.Reset(4));

    auto world = table.Prepare(GlobalKey(40, 0), 1, 100);
    harken::ActionCooldownKey pack = GlobalKey(40, 0);
    pack.owner = {harken::ActionDefinitionOwnerKind::Pack, 90, 3, 4, 5};
    auto packRow = table.Prepare(pack, 1, 100);
    harken::ActionCooldownKey emitter = GlobalKey(40, 1);
    emitter.scope = harken::ActionCooldownScopeV1::PerEmitter;
    emitter.emitter = UINT64_C(0x0000000200000001);
    auto emitterRow = table.Prepare(emitter, 1, 100);
    ASSERT_TRUE(table.Commit(&world.token));
    ASSERT_TRUE(table.Commit(&packRow.token));
    ASSERT_TRUE(table.Commit(&emitterRow.token));

    EXPECT_EQ(table.RemoveOwner(pack.owner), 1u);
    EXPECT_EQ(table.RemoveEmitter(UINT64_C(0x0000000100000001)), 0u);
    EXPECT_EQ(table.RemoveEmitter(emitter.emitter), 1u);
    EXPECT_EQ(table.LiveCount(), 1u);
}

TEST(ActionCooldownTable, GenerationExhaustionQuarantinesOnRelease) {
    harken::ActionCooldownTable table;
    ASSERT_TRUE(table.Reset(1));
    ASSERT_TRUE(table.SetFreeGenerationForTests(0, UINT64_MAX));
    auto row = table.Prepare(GlobalKey(50, 0), 0, 1);
    ASSERT_EQ(row.status, harken::ActionCooldownPrepareStatus::Prepared);
    ASSERT_TRUE(table.Commit(&row.token));
    EXPECT_FALSE(table.CanClearAll());
    EXPECT_FALSE(table.ClearAll());
    EXPECT_EQ(table.LiveCount(), 1u);
}

TEST(ActionCooldownTable,
     AbandonPreparedLeavesGenerationAndVisibleFactsUntouched) {
    harken::ActionCooldownTable table;
    ASSERT_TRUE(table.Reset(1));
    ASSERT_TRUE(table.SetFreeGenerationForTests(0, UINT64_MAX));

    auto first = table.Prepare(GlobalKey(60, 0), 0, 10);
    ASSERT_EQ(first.status, harken::ActionCooldownPrepareStatus::Prepared);
    EXPECT_EQ(table.HighWater(), 0u);
    ASSERT_TRUE(table.Abandon(&first.token));

    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.PreparedCount(), 0u);
    EXPECT_EQ(table.QuarantinedCount(), 0u);
    EXPECT_EQ(table.HighWater(), 0u);

    auto second = table.Prepare(GlobalKey(60, 0), 0, 10);
    ASSERT_EQ(second.status, harken::ActionCooldownPrepareStatus::Prepared);
    EXPECT_EQ(second.token.generation, UINT64_MAX);
}

} // namespace
