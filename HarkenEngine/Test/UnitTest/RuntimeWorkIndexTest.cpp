#include "harken/core/RuntimeWorkIndex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <type_traits>
#include <vector>

namespace {

struct TestFamily {};
using Identity = harken::RuntimeIndexIdentity<TestFamily>;
using FrameIndex = harken::RuntimeDueIndex<TestFamily, harken::RuntimeFrameClock>;
using SequenceIndex = harken::RuntimeDueIndex<TestFamily, harken::RuntimeCompletedSequenceClock>;

TEST(RuntimeWorkIndex, SparseDuePreparationKeepsFailurePagesAndClockQuotasIndependent) {
    FrameIndex frames;
    SequenceIndex sequences;
    static_assert(!std::is_convertible_v<FrameIndex::Time, SequenceIndex::Time>);
    ASSERT_TRUE(frames.Reserve(65537));
    ASSERT_TRUE(sequences.Reserve(65537));
    EXPECT_EQ(0u, frames.RequestedBytes());
    EXPECT_EQ(0u, frames.CommittedBytes());
    EXPECT_GT(frames.DirectoryBytes(), 0u);
    const Identity high{0x1234567800000001ull, 65536, 1};
    frames.SetCommitPageLimitForTests(1);
    EXPECT_FALSE(frames.PrepareOwner(high));
    EXPECT_EQ(0u, frames.PreparedCount());
    EXPECT_FALSE(frames.Prepared(high));
    EXPECT_GT(frames.CommittedBytes(), 0u);
    frames.SetCommitPageLimitForTests(SIZE_MAX);
    ASSERT_TRUE(frames.PrepareOwner(high));
    EXPECT_EQ(2u * harken::detail::StablePageReservation::SystemPageBytes(), frames.CommittedBytes());
    ASSERT_TRUE(sequences.PrepareOwner(high));
    ASSERT_TRUE(frames.Insert(high, {100}));
    ASSERT_TRUE(sequences.Insert(high, {4}));
    const Identity low{0xabcdef0100000001ull, 0, 1};
    ASSERT_TRUE(frames.PrepareOwner(low));
    ASSERT_TRUE(frames.Insert(low, {3}));
    ASSERT_TRUE(frames.Erase(low));
    ASSERT_TRUE(frames.ReleaseOwner(low));
    Identity out{};
    FrameIndex::Time when{};
    ASSERT_TRUE(frames.Peek(&out, &when));
    EXPECT_EQ(high, out);
    EXPECT_FALSE(frames.PopDue({99}, &out));
    EXPECT_FALSE(sequences.PopDue({4}, &out, true));
    ASSERT_TRUE(sequences.PopDue({5}, &out, true));
    EXPECT_TRUE(frames.Contains(high));
    EXPECT_EQ(1u, sequences.PreparedCount());
    const auto bytes = frames.CommittedBytes();
    {
        harken::memguard::AudioThreadScope audio;
        EXPECT_FALSE(frames.PrepareOwner(low));
        EXPECT_TRUE(frames.Retime(high, {90}));
        EXPECT_TRUE(frames.PopDue({90}, &out));
    }
    EXPECT_EQ(bytes, frames.CommittedBytes());
    ASSERT_TRUE(frames.ReleaseOwner(high));
    auto fresh = high;
    fresh.generation += std::uint64_t{1} << 32;
    ASSERT_TRUE(frames.PrepareOwner(fresh));
    ASSERT_TRUE(frames.Insert(fresh, {9}));
    EXPECT_FALSE(frames.Erase(high));
    EXPECT_FALSE(frames.Retime(high, {0}));
    EXPECT_FALSE(frames.ReleaseOwner(high));
    ASSERT_TRUE(frames.PopDue({9}, &out));
    EXPECT_EQ(fresh, out);
}

TEST(RuntimeWorkIndex, DueMutationMatchesOrderedAuthorityAndActivityRejectsOldGenerations) {
    constexpr std::uint32_t count = 521;
    FrameIndex due;
    harken::RuntimeActivityIndex<TestFamily> activity;
    ASSERT_TRUE(due.Reserve(count));
    ASSERT_TRUE(activity.Reserve(count));
    std::vector<Identity> owners(count);
    std::vector<std::uint64_t> dates(count, UINT64_MAX);
    const auto exact = [&](Identity id) noexcept { return owners[id.slot] == id; };
    const auto resolve = [&](std::uint32_t slot) noexcept { return owners[slot]; };
    for (std::uint32_t i = 0; i < count; ++i) {
        owners[i] = {0x1234567800000001ull, i, 7};
        ASSERT_TRUE(due.PrepareOwner(owners[i]));
        ASSERT_TRUE(activity.Prepare(owners[i]));
    }
    std::mt19937 random(20719);
    for (std::uint32_t step = 0; step < 2000; ++step) {
        const auto slot = random() % count;
        const auto date = std::uint64_t{random() % 16u};
        if (step % 3u == 0 && dates[slot] != UINT64_MAX) {
            ASSERT_TRUE(due.Erase(owners[slot]));
            dates[slot] = UINT64_MAX;
        } else {
            ASSERT_TRUE(dates[slot] == UINT64_MAX ? due.Insert(owners[slot], {date}) :
                due.Retime(owners[slot], {date}));
            dates[slot] = date;
        }
        const auto best = std::min_element(dates.begin(), dates.end());
        Identity first{};
        FrameIndex::Time time{};
        if (*best != UINT64_MAX) {
            ASSERT_TRUE(due.Peek(&first, &time));
            EXPECT_EQ(static_cast<std::uint32_t>(best - dates.begin()), first.slot);
            EXPECT_EQ(*best, time.value);
        } else EXPECT_FALSE(due.Peek(&first, &time));
    }
    for (auto slot : {520u, 256u, 63u, 0u}) {
        ASSERT_TRUE(activity.MarkDirty(owners[slot], exact));
        ASSERT_TRUE(activity.SetMembership(owners[slot], harken::RuntimeActivitySet::Active, true, exact));
    }
    auto old = owners[256];
    ASSERT_TRUE(activity.Remove(old, exact));
    owners[256].generation += std::uint64_t{1} << 32;
    ASSERT_TRUE(activity.MarkDirty(owners[256], exact));
    EXPECT_FALSE(activity.MarkDirty(old, exact));
    EXPECT_FALSE(activity.Remove(old, exact));
    std::uint32_t cursor = 0;
    for (auto slot : {0u, 63u, 256u, 520u}) {
        const auto id = activity.Next(harken::RuntimeActivitySet::Dirty, &cursor, resolve);
        EXPECT_EQ(owners[slot], id);
    }
    EXPECT_FALSE(activity.Next(harken::RuntimeActivitySet::Dirty, &cursor, resolve).Valid());
    cursor = 0;
    for (auto slot : {0u, 63u, 520u})
        EXPECT_EQ(owners[slot], activity.Next(harken::RuntimeActivitySet::Active, &cursor, resolve));
    EXPECT_FALSE(activity.Next(harken::RuntimeActivitySet::Active, &cursor, resolve).Valid());
}

} // namespace
