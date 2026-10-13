#include "Widgets/Common/PollReentryGuard.h"

#include <gtest/gtest.h>

TEST(PollReentryGuard, FirstEnterRunsAndCounts)
{
    VNX::PollReentryGuard guard;
    int ran = 0;
    EXPECT_TRUE(guard.run([&] { ++ran; }));
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(guard.executedCount(), 1);
    EXPECT_EQ(guard.blockedCount(), 0);
    EXPECT_FALSE(guard.busy());
}

TEST(PollReentryGuard, ReentryIsBlockedAndCounted)
{
    VNX::PollReentryGuard guard;
    int innerEntered = 0;
    EXPECT_TRUE(guard.run([&] {
        EXPECT_TRUE(guard.busy());
        EXPECT_FALSE(guard.run([&] { ++innerEntered; }));
    }));
    EXPECT_EQ(innerEntered, 0);
    EXPECT_EQ(guard.executedCount(), 1);
    EXPECT_EQ(guard.blockedCount(), 1);
    EXPECT_FALSE(guard.busy());
}

TEST(PollReentryGuard, LeaveAllowsNextRun)
{
    VNX::PollReentryGuard guard;
    EXPECT_TRUE(guard.run([] {}));
    EXPECT_TRUE(guard.run([] {}));
    EXPECT_EQ(guard.executedCount(), 2);
    EXPECT_EQ(guard.blockedCount(), 0);
}
