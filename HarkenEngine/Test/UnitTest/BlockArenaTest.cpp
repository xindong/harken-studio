// The two-ended arena's three load-bearing properties, each with a failure it
// is meant to catch:
//
//  1. The published high-water is the most both ends held at the same instant,
//     not the sum of each end's own peak. The two agree whenever the ends peak
//     in the same block, which every existing scene does, so the wrong
//     definition passes the whole suite and only over-reports in production.
//  2. The descending end returns aligned, in-bounds blocks that never overlap
//     the ascending end, including at capacities that are not a multiple of the
//     alignment, and refuses an oversized request instead of wrapping its base
//     into something that looks like a valid address.
//  3. Which allocations succeed is identical to a single-cursor arena for the
//     allocation shape the engine actually issues. This is what keeps a tight
//     arena dropping the same voices, and therefore what keeps the
//     byte-identical goldens byte-identical.
//
// Property 3 holds only while every caller uses one alignment. A modelled sweep
// over mixed alignments found 161 of 8000 configurations where the two schemes
// disagree -- including capacity 602, where the two-ended arena refused an
// allocation the single-cursor arena granted AND granted one it refused. That is
// why BlockArena::Alloc is private: an outside caller cannot express a mixed
// alignment. If that ever changes, this equivalence has to be re-established
// before the goldens can be trusted.
#include "harken/core/BlockArena.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr std::size_t kAlign = alignof(float);

// A single-cursor arena's verdict on the same request sequence: planes first
// (as RenderBlock issues them), then the per-voice scratch.
std::vector<bool> SingleCursorVerdict(std::uint32_t capacity,
                                      std::uint32_t lowCount,
                                      std::uint32_t highCount,
                                      std::size_t floatsEach) {
    const std::size_t bytes = floatsEach * sizeof(float);
    std::vector<bool> granted;
    std::size_t used = 0;
    const std::uint32_t total = lowCount + highCount;
    for (std::uint32_t i = 0; i < total; ++i) {
        const std::size_t aligned = (used + kAlign - 1) & ~(kAlign - 1);
        if (aligned + bytes > capacity) {
            granted.push_back(false);
            continue;
        }
        used = aligned + bytes;
        granted.push_back(true);
    }
    return granted;
}

TEST(BlockArena, HighWaterIsTheCombinedPeakNotTheSumOfEachEndsPeak) {
    // One block where the low end peaks and the high end is idle, then one
    // where the reverse holds. The two peaks never coincide, which is exactly
    // the case that separates the two definitions.
    constexpr std::uint32_t kCapacity = 4096;
    constexpr std::size_t kFloats = 64; // 256 bytes per allocation
    constexpr std::uint32_t kLowHeavy = 8;  // 2048 bytes
    constexpr std::uint32_t kHighHeavy = 6; // 1536 bytes
    harken::BlockArena arena(kCapacity);

    for (std::uint32_t i = 0; i < kLowHeavy; ++i) {
        ASSERT_NE(nullptr, arena.AllocFloats(kFloats)) << "low " << i;
    }
    const std::uint32_t lowOnlyPeak = arena.HighWaterBytes();
    EXPECT_EQ(kLowHeavy * kFloats * sizeof(float), lowOnlyPeak);

    arena.Reset();
    for (std::uint32_t i = 0; i < kHighHeavy; ++i) {
        ASSERT_NE(nullptr, arena.AllocFloatsHigh(kFloats)) << "high " << i;
    }
    const std::uint32_t highOnlyPeak =
        kHighHeavy * kFloats * sizeof(float);

    // The larger single-block figure is the answer. Summing the two ends' peaks
    // would report lowOnlyPeak + highOnlyPeak, which no block ever held.
    EXPECT_EQ(lowOnlyPeak, arena.HighWaterBytes());
    EXPECT_LT(arena.HighWaterBytes(), lowOnlyPeak + highOnlyPeak)
        << "high water is being summed across blocks, not observed within one";
}

TEST(BlockArena, DescendingEndStaysAlignedInBoundsAndClearOfTheAscendingEnd) {
    // Capacities that are not multiples of the alignment: the descending base
    // is rounded down, so this is where a sloppy rounding shows up.
    for (const std::uint32_t capacity : {4096u, 4095u, 4094u, 4093u}) {
        harken::BlockArena arena(capacity);
        float* low = arena.AllocFloats(16);
        ASSERT_NE(nullptr, low) << "capacity " << capacity;
        float* high = arena.AllocFloatsHigh(16);
        ASSERT_NE(nullptr, high) << "capacity " << capacity;

        EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(high) % kAlign)
            << "capacity " << capacity;
        // The descending block sits above the ascending one and both are inside
        // the declared capacity.
        EXPECT_GT(reinterpret_cast<std::uintptr_t>(high),
                  reinterpret_cast<std::uintptr_t>(low) + 16 * sizeof(float) -
                      1)
            << "capacity " << capacity;
        EXPECT_LE(reinterpret_cast<std::uintptr_t>(high) + 16 * sizeof(float),
                  reinterpret_cast<std::uintptr_t>(low) + capacity)
            << "capacity " << capacity;
        EXPECT_EQ(0u, arena.OverflowCount()) << "capacity " << capacity;
    }

    // An oversized descending request must refuse. Computing the base before
    // the size test would wrap and hand back an address far outside the buffer.
    harken::BlockArena tight(256);
    EXPECT_EQ(nullptr, tight.AllocFloatsHigh(1024));
    EXPECT_EQ(1u, tight.OverflowCount());
    EXPECT_EQ(0u, tight.UsedBytes()) << "a refused request must not consume";
    // And the arena still works afterwards.
    EXPECT_NE(nullptr, tight.AllocFloatsHigh(8));
}

TEST(BlockArena, ZeroCountAllocationsDoNotChangeArenaState) {
    harken::BlockArena arena(4096);
    // A capacity that is not a multiple of the alignment, probed while both ends
    // are still empty. This is the only moment the descending end's limit is
    // itself unaligned, so a zero count routed through the general path rounds
    // the base down and bills the rounding difference as used bytes. The first
    // real high-end allocation absorbs that same misalignment and leaves the
    // limit aligned for good, which is why an empty arena has to be probed
    // before anything else happens to it.
    arena.SetCapacityForTests(4095);
    ASSERT_EQ(0u, arena.UsedBytes());
    ASSERT_EQ(0u, arena.HighWaterBytes());
    ASSERT_EQ(0u, arena.OverflowCount());

    EXPECT_NE(nullptr, arena.AllocFloats(0));
    EXPECT_NE(nullptr, arena.AllocFloatsHigh(0));
    EXPECT_EQ(0u, arena.UsedBytes())
        << "a zero count consumed bytes from an empty arena";
    EXPECT_EQ(0u, arena.HighWaterBytes())
        << "a zero count raised the high water of an empty arena";
    EXPECT_EQ(0u, arena.OverflowCount());

    // The same contract once both ends already hold real blocks.
    ASSERT_NE(nullptr, arena.AllocFloats(16));
    ASSERT_NE(nullptr, arena.AllocFloatsHigh(16));

    const std::uint32_t usedBefore = arena.UsedBytes();
    const std::uint32_t highWaterBefore = arena.HighWaterBytes();
    const std::uint64_t overflowsBefore = arena.OverflowCount();

    EXPECT_NE(nullptr, arena.AllocFloats(0));
    EXPECT_NE(nullptr, arena.AllocFloatsHigh(0));
    EXPECT_EQ(usedBefore, arena.UsedBytes());
    EXPECT_EQ(highWaterBefore, arena.HighWaterBytes());
    EXPECT_EQ(overflowsBefore, arena.OverflowCount());
}

TEST(BlockArena, QuarantineRetiresTheWholeDescendingSpanAcrossResets) {
    // The quarantine's job is to make every descending byte the abandoned block
    // handed out unreachable, not just the subset its owner can still enumerate:
    // the stranded writers hold pointers interleaved through that whole span.
    constexpr std::uint32_t kCapacity = 4096;
    constexpr std::size_t kFloats = 16; // 64 bytes
    harken::BlockArena arena(kCapacity);
    EXPECT_EQ(0u, arena.QuarantinedHighBytes());

    float* const firstBlock = arena.AllocFloatsHigh(kFloats);
    ASSERT_NE(nullptr, firstBlock);
    float* const lowEdgeBlock = arena.AllocFloatsHigh(kFloats);
    ASSERT_NE(nullptr, lowEdgeBlock);
    // The descending end grows downwards, so the second block sits below the
    // first and is the low edge of the span handed out this block.
    ASSERT_LT(reinterpret_cast<std::uintptr_t>(lowEdgeBlock),
              reinterpret_cast<std::uintptr_t>(firstBlock));
    const std::uint32_t spanBytes = arena.UsedBytes();
    ASSERT_EQ(2u * kFloats * sizeof(float), spanBytes);

    arena.QuarantineHighEnd();
    EXPECT_EQ(spanBytes, arena.QuarantinedHighBytes());

    // A reset does not give the span back, and the next descending allocation
    // lands strictly below every retired byte -- including below the block the
    // owner never named.
    arena.Reset();
    EXPECT_EQ(spanBytes, arena.UsedBytes())
        << "a reset handed back quarantined bytes";
    float* const afterQuarantine = arena.AllocFloatsHigh(kFloats);
    ASSERT_NE(nullptr, afterQuarantine);
    EXPECT_LE(reinterpret_cast<std::uintptr_t>(afterQuarantine) +
                  kFloats * sizeof(float),
              reinterpret_cast<std::uintptr_t>(lowEdgeBlock))
        << "an allocation after the quarantine overlapped the retired span";

    // The retired bytes are gone from the capacity the ascending end can reach
    // too: the two ends share one budget, so the shortfall is honest rather than
    // hidden until they meet.
    harken::BlockArena reference(kCapacity);
    std::uint32_t referenceGrants = 0;
    while (reference.AllocFloats(kFloats) != nullptr) {
        ++referenceGrants;
    }
    arena.Reset();
    std::uint32_t quarantinedGrants = 0;
    while (arena.AllocFloats(kFloats) != nullptr) {
        ++quarantinedGrants;
    }
    EXPECT_EQ(referenceGrants - spanBytes / (kFloats * sizeof(float)),
              quarantinedGrants);
    EXPECT_GT(arena.OverflowCount(), 0u);
}

TEST(BlockArena, QuarantineIsMonotoneAndNeverGivesBytesBack) {
    constexpr std::uint32_t kCapacity = 4096;
    harken::BlockArena arena(kCapacity);
    ASSERT_NE(nullptr, arena.AllocFloatsHigh(32));
    arena.QuarantineHighEnd();
    const std::uint32_t firstFloor = arena.QuarantinedHighBytes();
    ASSERT_GT(firstFloor, 0u);

    // A later block that uses less of the descending end than the retired span
    // must not shrink the floor: those bytes were retired because a writer may
    // still be in them, and nothing here proves otherwise.
    arena.Reset();
    arena.QuarantineHighEnd();
    EXPECT_EQ(firstFloor, arena.QuarantinedHighBytes());
    arena.Reset();
    ASSERT_NE(nullptr, arena.AllocFloatsHigh(8));
    arena.QuarantineHighEnd();
    EXPECT_GT(arena.QuarantinedHighBytes(), firstFloor);
}

TEST(BlockArena, ZeroCountContractSurvivesAQuarantinedFloor) {
    // The floor must not disturb the no-op contract: with the descending end
    // rewound to a retired edge rather than to zero, a zero count still has to
    // return a non-null aligned pointer and change nothing.
    harken::BlockArena arena(4096);
    arena.SetCapacityForTests(4095);
    ASSERT_NE(nullptr, arena.AllocFloatsHigh(16));
    arena.QuarantineHighEnd();
    arena.Reset();

    const std::uint32_t usedBefore = arena.UsedBytes();
    const std::uint32_t highWaterBefore = arena.HighWaterBytes();
    const std::uint64_t overflowsBefore = arena.OverflowCount();
    ASSERT_GT(usedBefore, 0u) << "the floor was not in force";

    float* const low = arena.AllocFloats(0);
    float* const high = arena.AllocFloatsHigh(0);
    EXPECT_NE(nullptr, low);
    EXPECT_NE(nullptr, high);
    EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(low) % kAlign);
    EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(high) % kAlign);
    EXPECT_EQ(usedBefore, arena.UsedBytes());
    EXPECT_EQ(highWaterBefore, arena.HighWaterBytes());
    EXPECT_EQ(overflowsBefore, arena.OverflowCount());
}

TEST(BlockArena, TwoEndedGrantsExactlyWhatOneCursorWouldForTheEngineShape) {
    // Every engine allocation is AllocFloats-shaped: one alignment, a whole
    // number of floats. Across capacities that straddle the exhaustion point,
    // the two-ended arena must grant and refuse the same allocations a single
    // cursor would -- that identity is what a tight arena's determinism rests on.
    constexpr std::size_t kFloats = 8; // 32 bytes
    constexpr std::uint32_t kLow = 3;
    constexpr std::uint32_t kHigh = 5;
    for (std::uint32_t capacity = 0; capacity <= 512; ++capacity) {
        const std::vector<bool> expected =
            SingleCursorVerdict(capacity, kLow, kHigh, kFloats);
        harken::BlockArena arena(capacity);
        std::vector<bool> actual;
        for (std::uint32_t i = 0; i < kLow; ++i) {
            actual.push_back(arena.AllocFloats(kFloats) != nullptr);
        }
        for (std::uint32_t i = 0; i < kHigh; ++i) {
            actual.push_back(arena.AllocFloatsHigh(kFloats) != nullptr);
        }
        ASSERT_EQ(expected, actual) << "capacity " << capacity;
    }
}

} // namespace
