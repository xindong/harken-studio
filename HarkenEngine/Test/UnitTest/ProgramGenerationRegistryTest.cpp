// Program-generation registry contract tests.
//
// Three properties here are not observable from a value and so have to be
// pinned: that a refused publish changes nothing, that a superseded generation
// stays readable until the audio thread is demonstrably past it, and that the
// fence comparison errs towards retaining rather than freeing.

#include "harken/core/ProgramGenerationRegistry.h"

#include <gtest/gtest.h>

using harken::ProgramGeneration;
using harken::ProgramGenerationRegistry;

TEST(ProgramGenerationRegistry, StartsWithNoCurrentGeneration)
{
    ProgramGenerationRegistry registry(2);
    EXPECT_EQ(registry.Current(), 0u);
    EXPECT_FALSE(registry.IsReadable(0));
    EXPECT_FALSE(registry.IsCurrent(0));
    EXPECT_EQ(registry.RetainedCount(), 0u);
}

TEST(ProgramGenerationRegistry, PublishedGenerationIsCurrentAndCarriesItsPayload)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration first = registry.Publish(0xABCDu, 100);
    ASSERT_NE(first, 0u);
    EXPECT_TRUE(registry.IsCurrent(first));
    EXPECT_TRUE(registry.IsReadable(first));

    std::uint64_t payload = 0;
    ASSERT_TRUE(registry.PayloadOf(first, &payload));
    EXPECT_EQ(payload, 0xABCDu);
}

// Replacement must not edit in place: after the swap, a reference taken to the
// old generation before the swap still resolves. Freeing on publish would pull
// memory out from under a reader that is mid-block.
TEST(ProgramGenerationRegistry, SupersededGenerationStaysReadableAfterTheSwap)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration first = registry.Publish(1, 100);
    const ProgramGeneration second = registry.Publish(2, 100);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_NE(first, second);

    EXPECT_TRUE(registry.IsCurrent(second));
    EXPECT_FALSE(registry.IsCurrent(first));
    // Still readable, which is the whole point of not editing in place.
    EXPECT_TRUE(registry.IsReadable(first));
    EXPECT_EQ(registry.RetainedCount(), 2u);
}

// The fence is strictly greater than the sequence at which the generation was
// superseded. A reader sitting exactly at that sequence may still be inside the
// block that read the old set, so equality must not release it. This case is
// the reason the comparison is not >=.
TEST(ProgramGenerationRegistry, DoesNotReclaimAtTheFenceSequenceItself)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration first = registry.Publish(1, 500);
    ASSERT_NE(registry.Publish(2, 500), 0u);

    EXPECT_EQ(registry.ReclaimSuperseded(500), 0u);
    EXPECT_TRUE(registry.IsReadable(first));

    EXPECT_EQ(registry.ReclaimSuperseded(501), 1u);
    EXPECT_FALSE(registry.IsReadable(first));
}

// The current generation is never reclaimable, however far the audio thread has
// advanced: there is nothing newer for a reader to have moved on to.
TEST(ProgramGenerationRegistry, NeverReclaimsTheCurrentGeneration)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration only = registry.Publish(1, 10);
    ASSERT_NE(only, 0u);

    EXPECT_EQ(registry.ReclaimSuperseded(1000000), 0u);
    EXPECT_TRUE(registry.IsCurrent(only));
    EXPECT_EQ(registry.RetainedCount(), 1u);
}

// A caller either gets a complete new generation or the state it already had.
// The capacity check runs before anything moves, so the previous current
// generation is still current and still readable after a refusal.
TEST(ProgramGenerationRegistry, RefusedPublishLeavesThePreviousStateIntact)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration first = registry.Publish(1, 10);
    const ProgramGeneration second = registry.Publish(2, 20);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(registry.RetainedCount(), 2u);

    EXPECT_EQ(registry.Publish(3, 30), 0u);
    EXPECT_TRUE(registry.IsCurrent(second));
    EXPECT_TRUE(registry.IsReadable(first));
    EXPECT_EQ(registry.RetainedCount(), 2u);

    // Room reappears through the fence, not through the refusal.
    EXPECT_EQ(registry.ReclaimSuperseded(21), 1u);
    EXPECT_NE(registry.Publish(3, 30), 0u);
}

// A reclaimed id must not resolve to the payload it used to carry, or a stale
// reference would keep reading numbers that no longer describe anything.
TEST(ProgramGenerationRegistry, ReclaimedGenerationResolvesToNothing)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration first = registry.Publish(0x11u, 5);
    ASSERT_NE(registry.Publish(0x22u, 5), 0u);
    ASSERT_EQ(registry.ReclaimSuperseded(6), 1u);

    std::uint64_t payload = 0xDEADu;
    EXPECT_FALSE(registry.PayloadOf(first, &payload));
    EXPECT_EQ(payload, 0xDEADu);
}

// Generation ids are monotonic and never reused, so a retained-and-reclaimed
// slot cannot hand a later publish the same identity as an earlier one.
TEST(ProgramGenerationRegistry, MaximumGenerationPublishesOnceThenExhausts)
{
    ProgramGenerationRegistry registry(2);
    ASSERT_TRUE(registry.SetNextGenerationForTests(UINT64_MAX));
    EXPECT_EQ(registry.Publish(1, 7), UINT64_MAX);
    EXPECT_TRUE(registry.IsCurrent(UINT64_MAX));
    EXPECT_TRUE(registry.IsReadable(UINT64_MAX));
    EXPECT_TRUE(registry.GenerationExhausted());
    EXPECT_EQ(registry.Publish(2, 8), 0u);
    EXPECT_TRUE(registry.IsCurrent(UINT64_MAX));
}

TEST(ProgramGenerationRegistry, NeverReissuesAGenerationId)
{
    ProgramGenerationRegistry registry(2);
    const ProgramGeneration first = registry.Publish(1, 1);
    const ProgramGeneration second = registry.Publish(2, 1);
    ASSERT_EQ(registry.ReclaimSuperseded(2), 1u);
    const ProgramGeneration third = registry.Publish(3, 2);

    EXPECT_NE(third, first);
    EXPECT_NE(third, second);
    EXPECT_GT(third, second);
    EXPECT_EQ(registry.HighWater(), 2u);
}
