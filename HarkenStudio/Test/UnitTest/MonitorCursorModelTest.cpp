#include "Widgets/TimeSeries/MonitorCursorModel.h"

#include <gtest/gtest.h>

#include <vector>

using VNX::MonitorCursorClock;
using VNX::MonitorCursorMode;
using VNX::MonitorCursorModel;
using VNX::MonitorCursorReject;
using VNX::MonitorCursorSnapshot;
using VNX::MonitorCursorUncertaintyKind;

namespace {

MonitorCursorClock exactClock()
{
    MonitorCursorClock clock;
    clock.deviceEpoch = 7;
    clock.clockAnchorGeneration = 3;
    clock.deviceUncertaintyFrames = 0;
    return clock;
}

} // namespace

TEST(MonitorCursorModel, StartsUnreadyWithoutInventingZero)
{
    MonitorCursorModel model;
    EXPECT_FALSE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(model.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Unknown);
}

TEST(MonitorCursorModel, NewEpochDropsLockedFrame)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    model.setRetainedWindow(0, 100);
    ASSERT_TRUE(model.advanceLive(12, exactClock()));
    EXPECT_EQ(model.lockTo(8, exactClock()), MonitorCursorReject::None);
    model.setSessionIdentity(8, 3);
    EXPECT_EQ(model.snapshot().sessionEpoch, 8u);
    EXPECT_FALSE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(model.snapshot().mode, MonitorCursorMode::FollowLive);
}

TEST(MonitorCursorModel, FollowLiveAdvancesExecuteFrame)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    ASSERT_TRUE(model.advanceLive(12, exactClock()));
    EXPECT_EQ(model.snapshot().mode, MonitorCursorMode::FollowLive);
    ASSERT_TRUE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(*model.snapshot().executeFrame, 12);
    EXPECT_EQ(model.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Exact);
}

TEST(MonitorCursorModel, LockIgnoresLaterLiveAdvances)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    model.setRetainedWindow(0, 100);
    ASSERT_TRUE(model.advanceLive(10, exactClock()));
    EXPECT_EQ(model.lockTo(8, exactClock()), MonitorCursorReject::None);
    ASSERT_TRUE(model.advanceLive(40, exactClock()));
    ASSERT_TRUE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(*model.snapshot().executeFrame, 8);
    EXPECT_EQ(model.snapshot().mode, MonitorCursorMode::LockHistory);
}

TEST(MonitorCursorModel, FollowLiveJumpsToLatestAfterUnlock)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    model.setRetainedWindow(0, 100);
    ASSERT_TRUE(model.advanceLive(10, exactClock()));
    EXPECT_EQ(model.lockTo(8, exactClock()), MonitorCursorReject::None);
    ASSERT_TRUE(model.advanceLive(40, exactClock()));
    ASSERT_TRUE(model.followLive());
    ASSERT_TRUE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(*model.snapshot().executeFrame, 40);
    EXPECT_EQ(model.snapshot().mode, MonitorCursorMode::FollowLive);
}

TEST(MonitorCursorModel, RejectsLockOutsideHalfOpenWindow)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    model.setRetainedWindow(10, 20);
    ASSERT_TRUE(model.advanceLive(12, exactClock()));
    EXPECT_EQ(model.lockTo(20, exactClock()), MonitorCursorReject::HistoryUnavailable);
    ASSERT_TRUE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(*model.snapshot().executeFrame, 12);
    EXPECT_FALSE(model.snapshot().historyUnavailable);
}

TEST(MonitorCursorModel, ContributorWindowsUnionAndSurviveEmptyPeer)
{
    MonitorCursorModel model;
    model.setContributorWindow(1, std::nullopt, std::nullopt);
    model.setContributorWindow(2, 40, 41);
    EXPECT_EQ(model.lockTo(40), MonitorCursorReject::None);
    model.setContributorWindow(1, 1, 2);
    EXPECT_EQ(model.lockTo(1), MonitorCursorReject::None);
    EXPECT_EQ(model.lockTo(40), MonitorCursorReject::None);
}

TEST(MonitorCursorModel, EmptyWindowRejectsLock)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    EXPECT_EQ(model.lockTo(1, exactClock()), MonitorCursorReject::HistoryUnavailable);
}

TEST(MonitorCursorModel, ShrinkingWindowMarksHistoryUnavailable)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    model.setRetainedWindow(0, 50);
    EXPECT_EQ(model.lockTo(12, exactClock()), MonitorCursorReject::None);
    model.setRetainedWindow(20, 40);
    ASSERT_TRUE(model.snapshot().executeFrame.has_value());
    EXPECT_EQ(*model.snapshot().executeFrame, 12);
    EXPECT_TRUE(model.snapshot().historyUnavailable);
}

TEST(MonitorCursorModel, MissingAnchorIsUnknownEvenWhenUncertaintyIsZero)
{
    MonitorCursorModel model;
    MonitorCursorClock clock;
    clock.deviceUncertaintyFrames = 0;
    ASSERT_TRUE(model.advanceLive(4, clock));
    EXPECT_EQ(model.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Unknown);
}

TEST(MonitorCursorModel, GenerationMismatchIsUnknown)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    auto clock = exactClock();
    clock.clockAnchorGeneration = 99;
    ASSERT_TRUE(model.advanceLive(4, clock));
    EXPECT_EQ(model.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Unknown);
}

TEST(MonitorCursorModel, PositiveUncertaintyIsEstimate)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    auto clock = exactClock();
    clock.deviceUncertaintyFrames = 4;
    ASSERT_TRUE(model.advanceLive(4, clock));
    EXPECT_EQ(model.snapshot().uncertaintyKind, MonitorCursorUncertaintyKind::Estimate);
}

TEST(MonitorCursorModel, LocalGenerationIsMonotonicAndIgnoresNoChange)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    std::vector<std::uint64_t> seen;
    model.subscribe([&](const MonitorCursorSnapshot& snap) { seen.push_back(snap.generation); });
    ASSERT_TRUE(model.advanceLive(1, exactClock()));
    const auto first = model.snapshot().generation;
    ASSERT_TRUE(model.advanceLive(2, exactClock()));
    const auto second = model.snapshot().generation;
    ASSERT_TRUE(model.advanceLive(2, exactClock()));
    EXPECT_GT(first, 0u);
    EXPECT_EQ(second, first + 1);
    EXPECT_EQ(model.snapshot().generation, second);
    ASSERT_GE(seen.size(), 3u);
    EXPECT_EQ(seen[seen.size() - 2], first);
    EXPECT_EQ(seen.back(), second);
}

TEST(MonitorCursorModel, NewSubscriberGetsCurrentSnapshot)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    ASSERT_TRUE(model.advanceLive(9, exactClock()));
    std::optional<std::int64_t> first;
    model.subscribe([&](const MonitorCursorSnapshot& snap) { first = snap.executeFrame; });
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 9);
}

TEST(MonitorCursorModel, UnsubscribeStopsNotifications)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    int count = 0;
    const auto token = model.subscribe([&](const MonitorCursorSnapshot&) { ++count; });
    model.unsubscribe(token);
    ASSERT_TRUE(model.advanceLive(2, exactClock()));
    EXPECT_EQ(count, 1);
}

TEST(MonitorCursorModel, InitialNotifySurvivesReentrantSubscribe)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    int outerCount = 0;
    int nestedCount = 0;
    model.subscribe([&](const MonitorCursorSnapshot&) {
        ++outerCount;
        if (nestedCount != 0)
            return;
        model.subscribe([&](const MonitorCursorSnapshot&) { ++nestedCount; });
    });
    EXPECT_EQ(outerCount, 1);
    EXPECT_EQ(nestedCount, 1);
}

TEST(MonitorCursorModel, ReentrantSubscribeSeesCurrentGeneration)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    std::uint64_t nestedGeneration = 0;
    model.subscribe([&](const MonitorCursorSnapshot&) {
        if (nestedGeneration != 0)
            return;
        model.subscribe([&](const MonitorCursorSnapshot& snap) {
            nestedGeneration = snap.generation;
        });
    });
    ASSERT_TRUE(model.advanceLive(3, exactClock()));
    EXPECT_EQ(nestedGeneration, model.snapshot().generation);
}

TEST(MonitorCursorModel, ReentrantUpdateDeliversImmutableSnapshotsInOrder)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    model.setRetainedWindow(0, 100);
    std::vector<std::uint64_t> seenA;
    std::vector<std::uint64_t> seenB;
    std::vector<std::int64_t> framesB;
    std::uint64_t tokenB = 0;
    model.subscribe([&](const MonitorCursorSnapshot& snap) {
        seenA.push_back(snap.generation);
        if (snap.executeFrame == 1)
            model.advanceLive(2, exactClock());
    });
    tokenB = model.subscribe([&](const MonitorCursorSnapshot& snap) {
        seenB.push_back(snap.generation);
        if (snap.executeFrame)
            framesB.push_back(*snap.executeFrame);
    });
    ASSERT_NE(tokenB, 0u);
    ASSERT_TRUE(model.advanceLive(1, exactClock()));
    ASSERT_GE(seenA.size(), 3u);
    ASSERT_GE(seenB.size(), 3u);
    EXPECT_EQ(seenA[seenA.size() - 2] + 1, seenA.back());
    EXPECT_EQ(seenB[seenB.size() - 2] + 1, seenB.back());
    EXPECT_EQ(seenA.back(), seenB.back());
    ASSERT_GE(framesB.size(), 2u);
    EXPECT_EQ(framesB[framesB.size() - 2], 1);
    EXPECT_EQ(framesB.back(), 2);
}

TEST(MonitorCursorModel, UnsubscribeDuringPublishStopsLaterListeners)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    int countB = 0;
    std::uint64_t tokenB = 0;
    model.subscribe([&](const MonitorCursorSnapshot&) {
        if (tokenB != 0)
            model.unsubscribe(tokenB);
    });
    tokenB = model.subscribe([&](const MonitorCursorSnapshot&) { ++countB; });
    ASSERT_TRUE(model.advanceLive(5, exactClock()));
    EXPECT_EQ(countB, 1);
}
