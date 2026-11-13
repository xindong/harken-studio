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

void FillCursorRows(harken::SourceCursorMirror::WriteView view,
                    std::uint64_t generation) {
    auto* rows = view.rows;
    for (std::uint32_t row = 0; row < view.count; ++row) {
        rows[row].publicationSerial = view.serial;
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
        rows[row].progress.sourceReadFrame = generation + row;
        rows[row].progress.sourceStartFrame = generation * 7u + row;
        rows[row].active =
            (MixCursorGeneration(generation ^
                                 (0xE7037ED1A0B428DBull * (row + 1))) &
             1u) != 0;
    }
}

template<class Row>
bool AreCursorRowsConsistent(
    const std::vector<Row>& rows,
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
    EXPECT_EQ(0u, row.progress.sourceReadFrame);
}

TEST(SourceCursorMirror, ConsistencyCheckerRejectsCorruption) {
    constexpr std::uint32_t kRows = 8;
    constexpr std::uint64_t kGeneration = 42;
    std::vector<harken::SourceCursorMirror::Row> good(kRows);
    FillCursorRows({good.data(), kRows, kGeneration}, kGeneration);
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
    harken::SourceCursorMirror mirror(kRows * 2u);
    ASSERT_TRUE(mirror.Prepare(kRows));
    FillCursorRows(mirror.BeginWrite(kRows), 1);
    mirror.EndWrite(1, 64, 3);

    std::atomic<bool> start{false};
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::uint64_t generation = 2;
             generation <= kPublications + 1; ++generation) {
            FillCursorRows(mirror.BeginWrite(kRows), generation);
            mirror.EndWrite(generation, generation * 64u, generation * 3u);
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::vector<harken::SourceCursorMirror::ParkingRow> rows(kRows);
    harken::SourceCursorMirror::Publication batch{};
    std::uint64_t captured = 0;
    std::uint64_t tears = 0;
    const auto capture = [&] {
        if (mirror.CaptureParking(rows.data(), kRows, &batch)) {
            ++captured;
            if (batch.availableCount != kRows ||
                !AreCursorRowsConsistent(rows, batch.blockIndex)) {
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
                single.publicationSerial != publication.serial ||
                single.progress.sourceReadFrame != generation + index ||
                single.progress.sourceStartFrame != generation * 7u + index ||
                single.renderGeneration != static_cast<std::uint32_t>(MixCursorGeneration(
                    generation ^ (0x8EBC6AF09C88C6E3ull * (index + 1))))) ++tears;
        }
    };

    start.store(true, std::memory_order_release);
    const bool grew = mirror.Prepare(kRows * 2u);
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
    ASSERT_TRUE(grew);
    ASSERT_GT(captured, 0u);
    EXPECT_EQ(0u, tears);
    ASSERT_TRUE(mirror.CaptureParking(rows.data(), kRows, &batch));
    EXPECT_EQ(kPublications + 1, batch.blockIndex);
    EXPECT_TRUE(AreCursorRowsConsistent(rows, batch.blockIndex));
}

TEST(SourceCursorMirror, UntouchedRingRowsRetireAtTheSameOutputFrame) {
    harken::SourceCursorMirror mirror(2);
    ASSERT_TRUE(mirror.Prepare(2));
    harken::SourceCursorMirror::Publication publication{};
    harken::SourceCursorMirror::Row row{};
    std::uint64_t previousSerial = 0;
    for (std::uint64_t commandCut = 1; commandCut <= 8; ++commandCut) {
        const auto write = mirror.BeginWrite(2);
        if (commandCut <= 3) {
            FillCursorRows(write, commandCut);
            write.rows[1].active = true;
        }
        mirror.EndWrite(7, 448, commandCut);
        ASSERT_TRUE(mirror.CaptureRow(1, &row, &publication));
        EXPECT_EQ(2u, publication.availableCount);
        EXPECT_EQ(448u, publication.outputFrame);
        EXPECT_EQ(commandCut, publication.consumedCommands);
        EXPECT_GT(publication.serial, previousSerial);
        previousSerial = publication.serial;
        EXPECT_EQ(commandCut <= 3, row.active);
        if (commandCut > 3) EXPECT_FALSE(row.exactTimingValid);
    }
}

TEST(SourceCursorMirror, GrowthFailureAndRetryKeepThePublishedPrefix) {
    harken::SourceCursorMirror mirror(128);
    EXPECT_EQ(0u, mirror.CommittedBytes());
    harken::SourceCursorMirror::Publication publication{};
    ASSERT_TRUE(mirror.CaptureParking(nullptr, 0, &publication));
    EXPECT_EQ(0u, publication.availableCount);
    ASSERT_TRUE(mirror.Prepare(1));
    FillCursorRows(mirror.BeginWrite(1), 1);
    mirror.EndWrite(1, 64, 3);
    const auto committedBefore = mirror.CommittedBytes();
    mirror.SetCommitPageLimitForTests(1, 0);
    EXPECT_FALSE(mirror.Prepare(128));
    EXPECT_GT(mirror.CommittedBytes(), committedBefore);

    harken::SourceCursorMirror::ParkingRow parking[2]{};
    parking[1].sourceFrame = 999;
    ASSERT_TRUE(mirror.CaptureParking(parking, 2, &publication));
    ASSERT_EQ(1u, publication.availableCount);
    EXPECT_EQ(999u, parking[1].sourceFrame);
    harken::SourceCursorMirror::Row row{};
    EXPECT_FALSE(mirror.CaptureRow(127, &row, &publication));

    const auto write = mirror.BeginWrite(1);
    mirror.SetCommitPageLimitForTests(1, SIZE_MAX);
    ASSERT_TRUE(mirror.Prepare(128));
    FillCursorRows(write, 2);
    mirror.EndWrite(2, 128, 6);
    ASSERT_TRUE(mirror.CaptureParking(parking, 1, &publication));
    EXPECT_EQ(1u, publication.availableCount);
    EXPECT_FALSE(mirror.CaptureRow(127, &row, &publication));
    FillCursorRows(mirror.BeginWrite(128), 3);
    mirror.EndWrite(3, 192, 9);
    ASSERT_TRUE(mirror.CaptureRow(127, &row, &publication));
    EXPECT_EQ(128u, publication.availableCount);
    EXPECT_EQ(130u, row.progress.sourceReadFrame);
}

TEST(SourceCursorMirror, AudioCanPublishPreparedRowsWithoutGrowingStorage) {
    harken::SourceCursorMirror mirror(128);
    ASSERT_TRUE(mirror.Prepare(1));
    const auto committed = mirror.CommittedBytes();
    bool grew = false;
    {
        harken::memguard::AudioThreadScope audio;
        grew = mirror.Prepare(128);
        FillCursorRows(mirror.BeginWrite(1), 1);
        mirror.EndWrite(1, 64, 3);
    }
    EXPECT_FALSE(grew);
    EXPECT_EQ(committed, mirror.CommittedBytes());
    harken::SourceCursorMirror::Row row{};
    harken::SourceCursorMirror::Publication publication{};
    ASSERT_TRUE(mirror.CaptureRow(0, &row, &publication));
    EXPECT_EQ(3u, publication.consumedCommands);
}

} // namespace
