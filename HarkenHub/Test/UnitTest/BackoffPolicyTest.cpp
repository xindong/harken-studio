#include "Download/BackoffPolicy.h"

#include <gtest/gtest.h>

using VNH::BackoffPolicy;

TEST(BackoffPolicy, GrowsExponentiallyAndCaps)
{
    BackoffPolicy policy;
    policy.baseDelayMs = 100;
    policy.maxDelayMs = 1000;
    policy.jitterFactor = 0.0;

    EXPECT_EQ(policy.delayMsForAttempt(0, 0.0), 100);
    EXPECT_EQ(policy.delayMsForAttempt(1, 0.0), 200);
    EXPECT_EQ(policy.delayMsForAttempt(2, 0.0), 400);
    EXPECT_EQ(policy.delayMsForAttempt(3, 0.0), 800);
    EXPECT_EQ(policy.delayMsForAttempt(4, 0.0), 1000);
    EXPECT_EQ(policy.delayMsForAttempt(30, 0.0), 1000);
    EXPECT_EQ(policy.delayMsForAttempt(-2, 0.0), 100);
}

TEST(BackoffPolicy, JitterShrinksDelayWithinConfiguredWindow)
{
    BackoffPolicy policy;
    policy.baseDelayMs = 1000;
    policy.maxDelayMs = 8000;
    policy.jitterFactor = 0.5;

    EXPECT_EQ(policy.delayMsForAttempt(0, 0.0), 1000);
    EXPECT_EQ(policy.delayMsForAttempt(0, 0.5), 750);
    EXPECT_EQ(policy.delayMsForAttempt(0, 1.0), 500);
    EXPECT_EQ(policy.delayMsForAttempt(0, 2.0), 500);
    EXPECT_EQ(policy.delayMsForAttempt(0, -1.0), 1000);
}

TEST(BackoffPolicy, SampleStaysWithinJitterWindow)
{
    BackoffPolicy policy;
    policy.baseDelayMs = 1000;
    policy.maxDelayMs = 8000;
    policy.jitterFactor = 0.5;

    for (int attempt = 0; attempt < 6; ++attempt) {
        const int full = policy.delayMsForAttempt(attempt, 0.0);
        const int floor = policy.delayMsForAttempt(attempt, 1.0);
        for (int i = 0; i < 20; ++i) {
            const int sampled = policy.sampleDelayMs(attempt);
            EXPECT_GE(sampled, floor);
            EXPECT_LE(sampled, full);
        }
    }
}
