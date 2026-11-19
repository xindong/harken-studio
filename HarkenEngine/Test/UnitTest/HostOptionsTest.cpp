// The host demo's --seconds parsing contract, tested through the pure
// parser the tool itself uses: whole-string conversion, finiteness, and
// the (0, 3600] range — no partial parses, no infinities reaching the
// duration-to-frame conversions.
#include "DemoOptions.h"
#include "SchedulingMeasureOptions.h"
#include "Common/PackFoundationFixture.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

TEST(HostOptions, AcceptsFiniteSecondsInRange) {
    double seconds = 0.0;
    EXPECT_TRUE(harkenhost::ParseSeconds("6", &seconds));
    EXPECT_DOUBLE_EQ(6.0, seconds);
    EXPECT_TRUE(harkenhost::ParseSeconds("0.5", &seconds));
    EXPECT_DOUBLE_EQ(0.5, seconds);
    EXPECT_TRUE(harkenhost::ParseSeconds("3600", &seconds));
    EXPECT_DOUBLE_EQ(3600.0, seconds);
}

TEST(HostOptions, RefusesMalformedInput) {
    double seconds = -1.0;
    EXPECT_FALSE(harkenhost::ParseSeconds(nullptr, &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("abc", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("6x", &seconds)); // trailing junk
    EXPECT_FALSE(harkenhost::ParseSeconds("6 7", &seconds));
    // A refused parse leaves the output untouched.
    EXPECT_DOUBLE_EQ(-1.0, seconds);
}

TEST(HostOptions, RefusesNonFiniteAndOutOfRange) {
    double seconds = -1.0;
    EXPECT_FALSE(harkenhost::ParseSeconds("inf", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("nan", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("1e999", &seconds)); // overflows
    EXPECT_FALSE(harkenhost::ParseSeconds("0", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("-3", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("3600.1", &seconds)); // over cap
    EXPECT_DOUBLE_EQ(-1.0, seconds);
}

TEST(HostOptions, ParsesSchedulingWorkerMatrix) {
    std::uint32_t workers = 99;
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("0", &workers));
    EXPECT_EQ(0u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("1", &workers));
    EXPECT_EQ(1u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("2", &workers));
    EXPECT_EQ(2u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("4", &workers));
    EXPECT_EQ(4u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("8", &workers));
    EXPECT_EQ(8u, workers);
    EXPECT_FALSE(harkenhost::ParseSchedulingWorkerCount("3", &workers));
    EXPECT_FALSE(harkenhost::ParseSchedulingWorkerCount("9", &workers));
    EXPECT_FALSE(harkenhost::ParseSchedulingWorkerCount(" 4", &workers));
    EXPECT_EQ(8u, workers);
}

TEST(HostOptions, ParsesSchedulingLoadAndCompetition) {
    harkenhost::SchedulingLoad load = harkenhost::SchedulingLoad::Heavy;
    EXPECT_TRUE(harkenhost::ParseSchedulingLoad("light", &load));
    EXPECT_EQ(harkenhost::SchedulingLoad::Light, load);
    EXPECT_TRUE(harkenhost::ParseSchedulingLoad("medium", &load));
    EXPECT_EQ(harkenhost::SchedulingLoad::Medium, load);
    EXPECT_TRUE(harkenhost::ParseSchedulingLoad("heavy", &load));
    EXPECT_EQ(harkenhost::SchedulingLoad::Heavy, load);
    EXPECT_FALSE(harkenhost::ParseSchedulingLoad("LIGHT", &load));

    std::uint32_t duty = 99;
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuDuty("0", &duty));
    EXPECT_EQ(0u, duty);
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuDuty("50", &duty));
    EXPECT_EQ(50u, duty);
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuDuty("90", &duty));
    EXPECT_EQ(90u, duty);
    EXPECT_FALSE(harkenhost::ParseSchedulingCpuDuty("10", &duty));
    EXPECT_EQ(90u, duty);

    std::uint32_t threads = 0;
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuThreads("256", &threads));
    EXPECT_EQ(256u, threads);
    EXPECT_FALSE(harkenhost::ParseSchedulingCpuThreads("257", &threads));
    EXPECT_EQ(256u, threads);
}

} // namespace
