// Fixed-cadence block staging: the device path renders ONLY whole
// engine blocks into staging and copies out whatever the callback asked
// for, carrying leftovers across callbacks. These tests pin the cadence
// contract with a recording harness — no device, no engine.
#include "RenderSlicer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <tuple>
#include <vector>

namespace {

struct CopyRecord {
    std::uint32_t dstFrames;
    std::uint32_t srcFrames;
    std::uint32_t frames;

    bool operator==(const CopyRecord& other) const {
        return dstFrames == other.dstFrames &&
               srcFrames == other.srcFrames && frames == other.frames;
    }
};

// Records every render and copy the pump issues. Renders take no frame
// argument by construction — the pump can only ever request whole
// blocks, which is the cadence fact under test.
struct Harness {
    harken::BlockStagingCursor cursor;
    std::uint32_t renderCount = 0;

    std::vector<CopyRecord> Pump(std::uint32_t frameCount,
                                 std::uint32_t blockFrames) {
        std::vector<CopyRecord> copies;
        harken::PumpStagedBlocks(
            cursor, frameCount, blockFrames, [&] { ++renderCount; },
            [&](std::uint32_t dst, std::uint32_t src,
                std::uint32_t frames) {
                copies.push_back({dst, src, frames});
            });
        return copies;
    }
};

TEST(RenderStaging, ExactMultipleRendersWholeBlocks) {
    Harness harness;
    const std::vector<CopyRecord> expected = {{0, 0, 256}, {256, 0, 256}};
    EXPECT_EQ(expected, harness.Pump(512, 256));
    EXPECT_EQ(2u, harness.renderCount);
    EXPECT_EQ(0u, harness.cursor.availFrames);
}

TEST(RenderStaging, RemainderStaysStagedForTheNextCallback) {
    Harness harness;
    const std::vector<CopyRecord> expected = {
        {0, 0, 256}, {256, 0, 256}, {512, 0, 88}};
    EXPECT_EQ(expected, harness.Pump(600, 256));
    // Three whole renders served 600 frames; 168 staged frames remain.
    EXPECT_EQ(3u, harness.renderCount);
    EXPECT_EQ(88u, harness.cursor.readFrames);
    EXPECT_EQ(168u, harness.cursor.availFrames);
}

TEST(RenderStaging, LeftoverCarriesAcrossCallbacks) {
    Harness harness;
    const std::vector<CopyRecord> first = {{0, 0, 100}};
    EXPECT_EQ(first, harness.Pump(100, 256));
    EXPECT_EQ(1u, harness.renderCount);
    EXPECT_EQ(156u, harness.cursor.availFrames);

    // The second callback drains the 156 staged frames before rendering
    // exactly one more whole block for the remaining 244.
    const std::vector<CopyRecord> second = {{0, 100, 156}, {156, 0, 244}};
    EXPECT_EQ(second, harness.Pump(400, 256));
    EXPECT_EQ(2u, harness.renderCount);
    EXPECT_EQ(12u, harness.cursor.availFrames);
}

TEST(RenderStaging, SingleFrameCallbackStillRendersAWholeBlock) {
    Harness harness;
    const std::vector<CopyRecord> expected = {{0, 0, 1}};
    EXPECT_EQ(expected, harness.Pump(1, 256));
    EXPECT_EQ(1u, harness.renderCount);
    EXPECT_EQ(255u, harness.cursor.availFrames);
}

TEST(RenderStaging, ZeroFramesRendersNothing) {
    Harness harness;
    EXPECT_TRUE(harness.Pump(0, 256).empty());
    EXPECT_EQ(0u, harness.renderCount);
}

TEST(RenderStaging, ZeroBlockSizeServesNothingInsteadOfSpinning) {
    Harness harness;
    EXPECT_TRUE(harness.Pump(600, 0).empty());
    EXPECT_EQ(0u, harness.renderCount);
}

TEST(RenderStaging, EveryOutputFrameCoveredOnceAndAccountingBalances) {
    // A run of irregular callback sizes: within each callback the copies
    // must tile the output contiguously; staged reads must be contiguous
    // and inside the block; and at the end every rendered frame is
    // either delivered or still staged — nothing lost, nothing doubled.
    const std::uint32_t blockFrames = 256;
    const std::uint32_t callbackSizes[] = {480, 1,   256, 999, 13,
                                           700, 512, 0,   255, 257};
    Harness harness;
    std::uint64_t delivered = 0;
    std::uint32_t expectedRead = 0; // staged read position across calls
    for (const std::uint32_t frameCount : callbackSizes) {
        const std::vector<CopyRecord> copies =
            harness.Pump(frameCount, blockFrames);
        std::uint32_t dst = 0;
        for (const CopyRecord& copy : copies) {
            ASSERT_EQ(dst, copy.dstFrames);
            ASSERT_GT(copy.frames, 0u);
            if (copy.srcFrames == 0) {
                expectedRead = 0; // a fresh block was rendered
            }
            ASSERT_EQ(expectedRead, copy.srcFrames);
            ASSERT_LE(copy.srcFrames + copy.frames, blockFrames);
            expectedRead += copy.frames;
            dst += copy.frames;
        }
        ASSERT_EQ(frameCount, dst);
        delivered += frameCount;
    }
    EXPECT_EQ(static_cast<std::uint64_t>(harness.renderCount) * blockFrames,
              delivered + harness.cursor.availFrames);
}

} // namespace
