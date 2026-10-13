#include <gtest/gtest.h>

// Placeholder ids reserved by MonitorModelOnlyTests.txt. They must exist as
// TEST() symbols so the appendix gate can refuse to count them as
// RuntimeVerified / ContractVerified. They must stay skipped until M1 Hub
// implements the live behavior.

TEST(MonitorModelOnly, PollAcrossTermRetirement) {
    GTEST_SKIP() << "deferred:M1-1 Poll across Term is Hub lifetime, not M0";
}

TEST(MonitorModelOnly, CapturePlanLiveSlotRun) {
    GTEST_SKIP() << "deferred:M1-4 live CapturePlan slots are not M0 fixtures";
}
