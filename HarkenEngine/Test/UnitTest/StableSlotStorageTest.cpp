#include "harken/core/StableSlotStorage.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

TEST(StableSlotStorage, SparseIterationPreservesOrderAcrossDirectoryBoundaries) {
    harken::StableSlotStorage<unsigned> rows;
    ASSERT_TRUE(rows.Reserve(8195));
    const std::array<unsigned, 7> slots{0, 63, 64, 4095, 4096, 8192, 8194};
    for (auto it = slots.rbegin(); it != slots.rend(); ++it)
        ASSERT_TRUE(rows.Emplace(*it, *it));
    auto* first = &rows[0];
    ASSERT_TRUE(rows.Emplace(0, 99u));
    EXPECT_EQ(slots.size(), rows.ConstructedCount());
    EXPECT_EQ(first, &rows[0]);
    EXPECT_EQ(0u, rows[0]);
    std::vector<unsigned> actual(rows.begin(), rows.end());
    EXPECT_EQ(std::vector<unsigned>(slots.begin(), slots.end()), actual);
    const auto& immutable = rows;
    actual.assign(immutable.begin(), immutable.end());
    EXPECT_EQ(std::vector<unsigned>(slots.begin(), slots.end()), actual);

    auto cursor = rows.begin();
    ASSERT_TRUE(rows.Emplace(2048, 2048u));
    EXPECT_EQ(0u, *cursor++);
    EXPECT_EQ(63u, *cursor++);
    EXPECT_EQ(64u, *cursor++);
    EXPECT_EQ(2048u, *cursor);
    EXPECT_EQ(first, &rows[0]);
}

TEST(StableSlotStorage, SwapCarriesSparseDirectoryAndPreparedUseRejectsConstruction) {
    harken::StableSlotStorage<unsigned> left, right, empty;
    ASSERT_TRUE(left.Reserve(8195));
    ASSERT_TRUE(right.Reserve(65));
    ASSERT_TRUE(left.Emplace(8194, 7u));
    ASSERT_TRUE(right.Emplace(64, 9u));
    auto* high = &left[8194];
    left.Swap(right);
    EXPECT_EQ(9u, *left.begin());
    EXPECT_EQ(7u, *right.begin());
    EXPECT_EQ(high, &right[8194]);
    EXPECT_EQ(1, std::distance(right.begin(), right.end()));
    {
        harken::StableSlotStorage<unsigned>::PreparedUse use(&right);
        EXPECT_FALSE(right.Emplace(4096, 3u));
        EXPECT_TRUE(right.Emplace(8194, 3u));
        EXPECT_EQ(1u, right.ConstructedCount());
    }
    ASSERT_TRUE(right.Emplace(4096, 3u));
    EXPECT_EQ(3u, *right.begin());
    right.Swap(empty);
    EXPECT_EQ(right.begin(), right.end());
    EXPECT_EQ(2, std::distance(empty.begin(), empty.end()));
    EXPECT_EQ(high, &empty[8194]);
}

} // namespace
