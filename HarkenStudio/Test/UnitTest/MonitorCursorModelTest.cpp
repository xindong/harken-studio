#include "Widgets/TimeSeries/MonitorCursorModel.h"

#include <gtest/gtest.h>

#include <vector>

using VNX::MonitorCursorClock;
using VNX::MonitorCursorModel;
using VNX::MonitorCursorSnapshot;

namespace {

MonitorCursorClock exactClock()
{
    MonitorCursorClock clock;
    clock.deviceEpoch = 7;
    clock.clockAnchorGeneration = 3;
    clock.deviceUncertaintyFrames = 0;
    return clock;
}

}

TEST(MonitorCursorModel, ReentrantUpdateDeliversImmutableSnapshotsInOrder)
{
    MonitorCursorModel model;
    model.setSessionIdentity(7, 3);
    std::vector<std::uint64_t> seenA;
    std::vector<std::uint64_t> seenB;
    std::vector<std::int64_t> framesB;
    model.subscribe([&](const MonitorCursorSnapshot& snap) {
        seenA.push_back(snap.generation);
        if (snap.executeFrame == 1)
            model.advanceLive(2, exactClock());
    });
    model.subscribe([&](const MonitorCursorSnapshot& snap) {
        seenB.push_back(snap.generation);
        if (snap.executeFrame)
            framesB.push_back(*snap.executeFrame);
    });
    ASSERT_TRUE(model.advanceLive(1, exactClock()));
    ASSERT_GE(seenA.size(), 2u);
    ASSERT_GE(seenB.size(), 2u);
    EXPECT_EQ(seenA.back(), seenB.back());
    ASSERT_GE(framesB.size(), 2u);
    EXPECT_EQ(framesB[framesB.size() - 2], 1);
    EXPECT_EQ(framesB.back(), 2);
}
