// Log ring: wait-free producer records, drop-oldest overwrite, exact
// drop accounting at drain time, and the engine-level drain contract
// (records are consumed and counted even when no sink is configured).
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;

harken::LogRecord MakeRecord(std::uint32_t level, std::uint32_t code,
                            std::uint64_t arg0, std::uint64_t arg1) {
    harken::LogRecord record{};
    record.level = level;
    record.code = code;
    record.arg0 = arg0;
    record.arg1 = arg1;
    return record;
}

std::uint64_t MixLogSequence(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

harken::LogRecord MakeSequenceRecord(std::uint64_t sequence) {
    auto record = MakeRecord(
        static_cast<std::uint32_t>(
            MixLogSequence(sequence ^ 0xA0761D6478BD642Full)),
        static_cast<std::uint32_t>(
            MixLogSequence(sequence ^ 0xE7037ED1A0B428DBull)),
        sequence, MixLogSequence(sequence ^ 0x8EBC6AF09C88C6E3ull));
    record.timestampNs = MixLogSequence(sequence ^ 0x63e729ab19d540f1ull);
    record.objectId = VnObjectId{MixLogSequence(sequence ^ 101), MixLogSequence(sequence ^ 103)};
    record.projectId = VnObjectId{MixLogSequence(sequence ^ 107), MixLogSequence(sequence ^ 109)};
    return record;
}

bool IsSequenceRecordConsistent(const harken::LogRecord& record) {
    const std::uint64_t sequence = record.arg0;
    return record.level == static_cast<std::uint32_t>(
                               MixLogSequence(
                                   sequence ^ 0xA0761D6478BD642Full)) &&
           record.code == static_cast<std::uint32_t>(
                              MixLogSequence(
                                  sequence ^ 0xE7037ED1A0B428DBull)) &&
           record.arg1 ==
               MixLogSequence(sequence ^ 0x8EBC6AF09C88C6E3ull) &&
           record.timestampNs == MixLogSequence(sequence ^ 0x63e729ab19d540f1ull) &&
           record.objectId == VnObjectId{MixLogSequence(sequence ^ 101), MixLogSequence(sequence ^ 103)} &&
           record.projectId == VnObjectId{MixLogSequence(sequence ^ 107), MixLogSequence(sequence ^ 109)};
}

TEST(LogRing, ConsistencyCheckerRejectsCorruption) {
    const harken::LogRecord good = MakeSequenceRecord(42);
    EXPECT_TRUE(IsSequenceRecordConsistent(good));

    harken::LogRecord payloadCorruption = good;
    ++payloadCorruption.arg1;
    EXPECT_FALSE(IsSequenceRecordConsistent(payloadCorruption));

    harken::LogRecord generationCorruption = good;
    ++generationCorruption.arg0;
    EXPECT_FALSE(IsSequenceRecordConsistent(generationCorruption));
}

TEST(LogRing, RoundTripPreservesEveryField) {
    harken::LogRing ring;
    const auto before = harken::LogTimestampNs();
    ring.Push(MakeRecord(VN_LOG_INFO, 100, 11, 21));
    const auto after = harken::LogTimestampNs();
    ring.Push(MakeRecord(VN_LOG_WARN, 101, 12, 22));
    ring.Push(MakeRecord(VN_LOG_ERROR, 102, 13, 23));

    harken::LogRecord out{};
    ASSERT_TRUE(ring.TryPop(out));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_LOG_INFO), out.level);
    EXPECT_EQ(100u, out.code);
    EXPECT_EQ(11u, out.arg0);
    EXPECT_EQ(21u, out.arg1);
    EXPECT_GE(out.timestampNs, before);
    EXPECT_LE(out.timestampNs, after);
    ASSERT_TRUE(ring.TryPop(out));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_LOG_WARN), out.level);
    EXPECT_EQ(101u, out.code);
    ASSERT_TRUE(ring.TryPop(out));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_LOG_ERROR), out.level);
    EXPECT_EQ(102u, out.code);
    EXPECT_FALSE(ring.TryPop(out));
    EXPECT_EQ(0u, ring.DroppedCount());
}

TEST(LogRing, RetainsProducerTimestampAcrossDelayedDrain) {
    harken::LogRing ring;
    auto record = MakeRecord(VN_LOG_WARN, 17, 42, 100);
    record.timestampNs = 123456789;
    record.objectId = VnObjectId{123, 456};
    record.projectId = VnObjectId{789, 123};
    ASSERT_TRUE(ring.Push(record));
    EXPECT_EQ(1u, ring.PublishedCount());
    EXPECT_EQ(0u, ring.ConsumedCount());
    harken::LogRecord out{};
    ASSERT_TRUE(ring.TryPop(out));
    EXPECT_EQ(record.timestampNs, out.timestampNs);
    EXPECT_EQ(record.objectId, out.objectId);
    EXPECT_EQ(record.projectId, out.projectId);
    EXPECT_EQ(1u, ring.ConsumedCount());
}

TEST(LogRing, OverflowDropsOldestWithExactCount) {
    harken::LogRing ring;
    constexpr std::uint32_t kExtra = 40;
    constexpr std::uint32_t kTotal = harken::LogRing::kCapacity + kExtra;
    for (std::uint32_t i = 0; i < kTotal; ++i) {
        ring.Push(MakeRecord(VN_LOG_INFO, i, i * 2ull, i * 3ull));
    }

    // The survivors are exactly the newest kCapacity - 1 records, in
    // order: the overwritten span plus the indeterminate boundary record
    // (which a concurrent producer could be reclaiming mid-copy) are
    // counted once, at the first drained pop.
    harken::LogRecord out{};
    std::uint32_t expected = kExtra + 1;
    while (ring.TryPop(out)) {
        ASSERT_EQ(expected, out.code);
        ASSERT_EQ(expected * 2ull, out.arg0);
        ASSERT_EQ(expected * 3ull, out.arg1);
        ++expected;
    }
    EXPECT_EQ(kTotal, expected);
    EXPECT_EQ(static_cast<std::uint64_t>(kExtra) + 1, ring.DroppedCount());
}

TEST(LogRing, DrainWithoutSinkStillConsumesAndCounts) {
    // No log callback configured: Vn_Update must still drain the ring
    // (drop accounting stays exact) without formatting anything. An
    // arena too small for one accumulator plane emits exactly one
    // overflow record per block, so the expected drop count is exact.
    VnConfig config = MakeNullConfig(2, 256, 48000);
    config.arenaBytes = 64;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr std::uint32_t kExtra = 44;
    const std::uint32_t blocks = harken::LogRing::kCapacity + kExtra;
    engine->nullBackend.Pump(blocks);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    // kExtra overwritten records plus the conservatively dropped
    // boundary record (see the ring's survivor-window comment).
    EXPECT_EQ(static_cast<std::uint64_t>(kExtra) + 1,
              snapshot.logDropCount);
    // A second drain finds the ring empty: everything was consumed.
    ASSERT_EQ(VN_OK, Vn_Update());
    VnSnapshot again{};
    again.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&again));
    EXPECT_EQ(snapshot.logDropCount, again.logDropCount);
}

TEST(LogRing, ConcurrentProducerNeverTearsRecords) {
    constexpr std::uint64_t kPrefill = harken::LogRing::kCapacity * 4ull;
    constexpr std::uint64_t kPushes = 200000;
    harken::LogRing ring;

    std::atomic<bool> producerReady{false};
    std::atomic<bool> consumerStarted{false};
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (std::uint64_t sequence = 1; sequence <= kPrefill; ++sequence) {
            ring.Push(MakeSequenceRecord(sequence));
        }
        producerReady.store(true, std::memory_order_release);
        while (!consumerStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::uint64_t sequence = kPrefill + 1; sequence <= kPushes;
            ++sequence) {
            ring.Push(MakeSequenceRecord(sequence));
            std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::uint64_t delivered = 0;
    std::uint64_t tears = 0;
    harken::LogRecord out{};
    const auto count = [&] {
        ++delivered;
        if (!IsSequenceRecordConsistent(out)) {
            ++tears;
        }
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timedOut = false;
    while (!producerReady.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
        std::this_thread::yield();
    }
    consumerStarted.store(true, std::memory_order_release);
    while (!timedOut) {
        while (ring.TryPop(out)) {
            count();
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
        }
        if (producerDone.load(std::memory_order_acquire)) {
            while (ring.TryPop(out)) {
                count();
            }
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
        std::this_thread::yield();
    }
    producer.join();

    if (!timedOut) {
        while (ring.TryPop(out)) {
            count();
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
        }
    }

    ASSERT_FALSE(timedOut) << "drain did not converge within the time bound";
    EXPECT_EQ(0u, tears) << "a record was overwritten mid-copy";
    EXPECT_GT(ring.DroppedCount(), 0u);
    EXPECT_EQ(kPushes, delivered + ring.DroppedCount());
}

} // namespace
