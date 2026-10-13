// Allocation-guard scope re-entrancy: the audio tag is a per-thread
// depth count, so a nested scope's exit must not untag the thread while
// an outer scope is still live — the device callback wraps the mixer's
// per-block scope and relies on exactly this.
#include "harken/core/AllocGuard.h"

#include <gtest/gtest.h>
#include <cstdlib>

namespace {

TEST(MemguardScope, AllocationObserverDetectsRealLogicAllocAndRealloc) {
    using namespace harken::memguard;
    const bool alreadyArmed = IsArmed();
    Install();
    if (!IsArmed()) GTEST_SKIP() << "Debug CRT allocation observer unavailable";
    AllocationObservation observed{};
    void* allocation = nullptr;
    void* resized = nullptr;
    {
        AllocationObservationScope scope(observed);
        allocation = std::malloc(37);
        resized = std::realloc(allocation, 91);
    }
    std::free(resized != nullptr ? resized : allocation);
    if (!alreadyArmed) Uninstall();
    ASSERT_TRUE(observed.available);
    ASSERT_NE(nullptr, allocation);
    ASSERT_NE(nullptr, resized);
    EXPECT_EQ(1u, observed.logic.allocations);
    EXPECT_EQ(1u, observed.logic.reallocations);
    EXPECT_EQ(37u, observed.logic.allocationBytes);
    EXPECT_EQ(91u, observed.logic.reallocationBytes);
    EXPECT_EQ(0u, observed.audio.allocations + observed.audio.reallocations);
    EXPECT_EQ(0u, observed.audio.allocationBytes + observed.audio.reallocationBytes);
}

TEST(MemguardScope, UnarmedAllocationObserverIsUnavailable) {
    using namespace harken::memguard;
    const bool alreadyArmed = IsArmed();
    Uninstall();
    AllocationObservation observed{};
    {
        AllocationObservationScope scope(observed);
    }
    if (alreadyArmed) Install();
    EXPECT_FALSE(observed.available);
}

TEST(MemguardScope, NestedScopeExitKeepsTheOuterTagLive) {
    using harken::memguard::AudioThreadScope;
    using harken::memguard::IsAudioTagged;

    EXPECT_FALSE(IsAudioTagged());
    {
        AudioThreadScope outer;
        EXPECT_TRUE(IsAudioTagged());
        {
            AudioThreadScope inner;
            EXPECT_TRUE(IsAudioTagged());
        }
        // The regression this pins: a flag-based tag went false here.
        EXPECT_TRUE(IsAudioTagged());
    }
    EXPECT_FALSE(IsAudioTagged());
}

TEST(MemguardScope, DeeperNestingUnwindsInOrder) {
    using harken::memguard::AudioThreadScope;
    using harken::memguard::IsAudioTagged;

    AudioThreadScope a;
    {
        AudioThreadScope b;
        {
            AudioThreadScope c;
            EXPECT_TRUE(IsAudioTagged());
        }
        EXPECT_TRUE(IsAudioTagged());
    }
    EXPECT_TRUE(IsAudioTagged());
}

} // namespace
