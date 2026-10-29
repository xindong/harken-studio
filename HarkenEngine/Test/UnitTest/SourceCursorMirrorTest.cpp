// Focused coherence tests for the audio-to-logic source cursor mirror.
#include "harken/core/SourceCursorMirror.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

std::uint64_t MixCursorGeneration(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void FillCursorRows(harken::SourceCursorMirror::Row* rows,
                    std::uint32_t rowCount, std::uint64_t generation) {
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        rows[row].sourceFrame = MixCursorGeneration(
            generation ^ (0xA0761D6478BD642Full * (row + 1)));
        rows[row].renderGeneration = static_cast<std::uint32_t>(
            MixCursorGeneration(
                generation ^ (0x8EBC6AF09C88C6E3ull * (row + 1))));
        rows[row].loopPassesRemaining =
            1u + static_cast<std::uint32_t>(
                     MixCursorGeneration(
                         generation ^
                         (0x589965CC75374CC3ull * (row + 1))) %
                     1000u);
        rows[row].exactTimingValid = true;
        rows[row].progress.sourceIntegerFrame = generation + row;
        rows[row].progress.pcmFrameCount = generation * 7u + row;
        rows[row].active =
            (MixCursorGeneration(generation ^
                                 (0xE7037ED1A0B428DBull * (row + 1))) &
             1u) != 0;
    }
}

bool AreCursorRowsConsistent(
    const std::vector<harken::SourceCursorMirror::Row>& rows,
    std::uint64_t generation) {
    for (std::uint32_t row = 0; row < rows.size(); ++row) {
        if (rows[row].sourceFrame !=
                MixCursorGeneration(
                    generation ^ (0xA0761D6478BD642Full * (row + 1))) ||
            rows[row].renderGeneration !=
                static_cast<std::uint32_t>(
                    MixCursorGeneration(
                        generation ^
                        (0x8EBC6AF09C88C6E3ull * (row + 1)))) ||
            rows[row].loopPassesRemaining !=
                1u + static_cast<std::uint32_t>(
                         MixCursorGeneration(
                             generation ^
                             (0x589965CC75374CC3ull * (row + 1))) %
                         1000u) ||
            rows[row].active !=
                ((MixCursorGeneration(
                      generation ^
                      (0xE7037ED1A0B428DBull * (row + 1))) &
                  1u) != 0)) {
            return false;
        }
    }
    return true;
}

TEST(SourceCursorMirror, DefaultInactiveRowHasNoAuthority) {
    const harken::SourceCursorMirror::Row row{};
    EXPECT_EQ(0u, row.sourceFrame);
    EXPECT_EQ(0u, row.renderGeneration);
    EXPECT_EQ(0u, row.loopPassesRemaining);
    EXPECT_FALSE(row.active);
    EXPECT_FALSE(row.exactTimingValid);
    EXPECT_EQ(0u, row.progress.pcmFrameCount);
}

TEST(SourceCursorMirror, ConsistencyCheckerRejectsCorruption) {
    constexpr std::uint32_t kRows = 8;
    constexpr std::uint64_t kGeneration = 42;
    std::vector<harken::SourceCursorMirror::Row> good(kRows);
    FillCursorRows(good.data(), kRows, kGeneration);
    EXPECT_TRUE(AreCursorRowsConsistent(good, kGeneration));

    auto rowCorruption = good;
    ++rowCorruption[3].sourceFrame;
    EXPECT_FALSE(AreCursorRowsConsistent(rowCorruption, kGeneration));
    rowCorruption = good;
    ++rowCorruption[3].renderGeneration;
    EXPECT_FALSE(AreCursorRowsConsistent(rowCorruption, kGeneration));
    rowCorruption = good;
    ++rowCorruption[3].loopPassesRemaining;
    EXPECT_FALSE(AreCursorRowsConsistent(rowCorruption, kGeneration));
    EXPECT_FALSE(AreCursorRowsConsistent(good, kGeneration + 1));
}

TEST(SourceCursorMirror, ConcurrentPublicationNeverTearsBatch) {
    constexpr std::uint32_t kRows = 64;
    constexpr std::uint64_t kPublications = 200000;
    harken::SourceCursorMirror mirror(kRows);
    FillCursorRows(mirror.BeginWrite(), kRows, 1);
    mirror.EndWrite(1, 64, 3);

    std::atomic<bool> start{false};
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::uint64_t generation = 2;
             generation <= kPublications + 1; ++generation) {
            FillCursorRows(mirror.BeginWrite(), kRows, generation);
            mirror.EndWrite(generation, generation * 64u, generation * 3u);
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::vector<harken::SourceCursorMirror::Row> rows(kRows);
    std::uint64_t blockIndex = 0;
    std::uint64_t captured = 0;
    std::uint64_t tears = 0;
    const auto capture = [&] {
        if (mirror.Capture(rows.data(), &blockIndex)) {
            ++captured;
            if (!AreCursorRowsConsistent(rows, blockIndex)) {
                ++tears;
            }
        }
        const auto index = static_cast<std::uint32_t>(captured % kRows);
        harken::SourceCursorMirror::Row single{};
        harken::SourceCursorMirror::Publication publication{};
        if (mirror.CaptureRow(index, &single, &publication)) {
            const auto generation = publication.blockIndex;
            if (publication.outputFrame != generation * 64u ||
                publication.consumedCommands != generation * 3u ||
                !single.exactTimingValid ||
                single.progress.sourceIntegerFrame != generation + index ||
                single.progress.pcmFrameCount != generation * 7u + index ||
                single.renderGeneration != static_cast<std::uint32_t>(MixCursorGeneration(
                    generation ^ (0x8EBC6AF09C88C6E3ull * (index + 1))))) ++tears;
        }
    };

    start.store(true, std::memory_order_release);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timedOut = false;
    while (!producerDone.load(std::memory_order_acquire)) {
        capture();
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
    }
    producer.join();
    capture();

    ASSERT_FALSE(timedOut) << "publication did not converge within the time bound";
    ASSERT_GT(captured, 0u);
    EXPECT_EQ(0u, tears);
    ASSERT_TRUE(mirror.Capture(rows.data(), &blockIndex));
    EXPECT_EQ(kPublications + 1, blockIndex);
    EXPECT_TRUE(AreCursorRowsConsistent(rows, blockIndex));
}

} // namespace
