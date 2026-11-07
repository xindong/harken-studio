#include "harken/core/SourceTerminalRecord.h"

#include <gtest/gtest.h>

#include <array>

TEST(SourceTerminalRecords, PreparedReservationsRemainVisibleAcrossMutationAndReuse) {
    constexpr std::uint32_t count = 513;
    harken::PlayableInstanceTable instances(count);
    harken::SourceTerminalRecords records(count);
    std::array<harken::PlayableInstanceHandle, count> leaves{};
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        leaves[i] = instances.Acquire(harken::PlayableKind::Sound, 0);
        ASSERT_NE(0u, leaves[i]);
    }
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(instances.PrepareAcquire(harken::PlayableKind::Sound, 0, &prepared));
    leaves.back() = prepared.Handle();
    for (const auto i : {0u, 1u, 64u, 256u, 512u})
        ASSERT_TRUE(instances.AcquireSourceRetirementHold(leaves[i],
            i == 512u ? prepared.ScheduleEpoch() : instances.Classify(leaves[i]).scheduleEpoch));
    const auto reserve = [&](std::uint32_t i) {
        return records.Reserve(instances, leaves[i], instances.SourceRetirementEpoch(leaves[i]),
            i + 1u, {i, 1}, leaves[i]);
    };
    std::uint32_t cursor = 0;
    EXPECT_FALSE(records.NextOccupied(&cursor).Valid());
    const auto high = reserve(512);
    const auto removed = reserve(64);
    const auto old = reserve(0);
    const auto adjacent = reserve(1);
    ASSERT_TRUE(high.Valid());
    ASSERT_TRUE(removed.Valid());
    ASSERT_TRUE(old.Valid());
    ASSERT_TRUE(adjacent.Valid());
    EXPECT_FALSE(instances.Classify(leaves.back()).live);
    EXPECT_FALSE(reserve(0).Valid());
    EXPECT_EQ(4u, records.OccupiedCount());
    cursor = 0;
    EXPECT_EQ(old.slot, records.NextOccupied(&cursor).slot);
    ASSERT_TRUE(records.Release(old));
    ASSERT_TRUE(records.Release(removed));
    const auto low = reserve(0);
    const auto added = reserve(256);
    ASSERT_TRUE(low.Valid());
    ASSERT_TRUE(added.Valid());
    EXPECT_NE(old.generation, low.generation);
    EXPECT_FALSE(records.Release(old));
    EXPECT_NE(nullptr, records.Resolve(low));
    EXPECT_EQ(adjacent.slot, records.NextOccupied(&cursor).slot);
    EXPECT_EQ(added.slot, records.NextOccupied(&cursor).slot);
    EXPECT_EQ(high.slot, records.NextOccupied(&cursor).slot);
    EXPECT_FALSE(records.NextOccupied(&cursor).Valid());
    cursor = 0;
    const auto first = records.NextOccupied(&cursor);
    EXPECT_EQ(low.slot, first.slot);
    EXPECT_EQ(low.generation, first.generation);
    ASSERT_TRUE(records.Release(low));
    ASSERT_TRUE(records.Release(adjacent));
    ASSERT_TRUE(records.Release(added));
    ASSERT_TRUE(records.Release(high));
    cursor = 0;
    EXPECT_FALSE(records.NextOccupied(&cursor).Valid());
    EXPECT_EQ(0u, records.OccupiedCount());
    records.At(low.slot)->generation = UINT32_MAX;
    const auto last = reserve(0);
    ASSERT_TRUE(last.Valid());
    cursor = 0;
    EXPECT_EQ(UINT32_MAX, records.NextOccupied(&cursor).generation);
    ASSERT_TRUE(records.Release(last));
    EXPECT_FALSE(reserve(0).Valid());
    cursor = 0;
    EXPECT_FALSE(records.NextOccupied(&cursor).Valid());
    for (const auto i : {0u, 1u, 64u, 256u, 512u})
        ASSERT_TRUE(instances.ReleaseSourceRetirementHold(leaves[i]));
    ASSERT_TRUE(instances.CancelPrepared(&prepared));
    harken::SourceTerminalRecords empty(0);
    cursor = 0;
    EXPECT_FALSE(empty.NextOccupied(&cursor).Valid());
    EXPECT_FALSE(empty.NextOccupied(nullptr).Valid());
}
