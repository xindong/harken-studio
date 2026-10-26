#include "Widgets/Common/PollReentryGuard.h"

#include <gtest/gtest.h>

TEST(PollReentryGuard, ReentryIsBlocked)
{
    VNX::PollReentryGuard guard;
    int outerEntered = 0;
    int innerEntered = 0;
    EXPECT_TRUE(guard.run([&] {
        ++outerEntered;
        EXPECT_FALSE(guard.run([&] { ++innerEntered; }));
    }));
    EXPECT_EQ(outerEntered, 1);
    EXPECT_EQ(innerEntered, 0);
}

TEST(PollReentryGuard, LeaveAllowsNextRun)
{
    VNX::PollReentryGuard guard;
    int ran = 0;
    EXPECT_TRUE(guard.run([&] { ++ran; }));
    EXPECT_TRUE(guard.run([&] { ++ran; }));
    EXPECT_EQ(ran, 2);
}
