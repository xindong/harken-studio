// NotificationQueue tests pin the full-queue contract for the audio -> logic
// SPSC notification ring. Under overflow the ring drops the oldest records
// (notifications are advisory and may be lost), but it must never hand the
// consumer a record the producer can still be overwriting. The survivor window
// is capacity - 1, the boundary record at lag == capacity is conservatively
// dropped and counted, and post-copy write-cursor revalidation prevents a
// producer lapping the ring mid-copy from surfacing a torn record. These
// assertions failed under the pre-fix implementation, which used a strict '>'
// resync and omitted post-copy revalidation.
#include "harken/core/NotificationQueue.h"

#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

// splitmix64 finalizer: a bijective avalanche mix used to derive record fields
// from a sequence number.
std::uint64_t Mix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A notification whose every word is a function of one sequence number, so any
// independent field can prove the record self-consistent. voiceHandle carries
// the sequence verbatim; the other three words derive from it through distinct
// mixes. The queue treats VnNotification as opaque POD, so the derived type
// value need not be a real VN_NOTIFY_* code — only the cross-field agreement
// matters. A delivered record whose fields disagree with the sequence its
// voiceHandle names was stitched from two different pushes, i.e. torn.
VnNotification MakeNotification(std::uint64_t seq) {
    VnNotification n{};
    n.type =
        static_cast<std::uint32_t>(Mix64(seq ^ 0x2545F4914F6CDD1Dull));
    n.reserved0 =
        static_cast<std::uint32_t>(Mix64(seq ^ 0xD1B54A32D192ED03ull));
    n.voiceHandle = seq;
    n.blockTimestamp = Mix64(seq);
    return n;
}

bool IsConsistent(const VnNotification& n) {
    const std::uint64_t seq = n.voiceHandle;
    return n.type ==
               static_cast<std::uint32_t>(Mix64(seq ^ 0x2545F4914F6CDD1Dull)) &&
           n.reserved0 ==
               static_cast<std::uint32_t>(Mix64(seq ^ 0xD1B54A32D192ED03ull)) &&
           n.blockTimestamp == Mix64(seq);
}

TEST(NotificationQueue, DepthOneClampsCapacityToTwo) {
    harken::NotificationQueue queue(1);

    EXPECT_EQ(2u, queue.Capacity());
}

TEST(NotificationQueue, DepthOneOverflowDrainsLatestAndRemainsUsable) {
    harken::NotificationQueue queue(1);
    ASSERT_EQ(2u, queue.Capacity());
    queue.Push(MakeNotification(1));
    queue.Push(MakeNotification(2));

    VnNotification out{};
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(2u, out.voiceHandle);
    EXPECT_EQ(1u, queue.DroppedCount());
    EXPECT_FALSE(queue.TryPop(out));

    queue.Push(MakeNotification(3));
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(3u, out.voiceHandle);
    EXPECT_FALSE(queue.TryPop(out));
    EXPECT_EQ(1u, queue.DroppedCount());
}

// Case D: the tearing detector must not be a vacuous always-true. A pristine
// record passes; corrupting any single word — as a mid-copy overwrite from a
// different sequence would — is caught.
TEST(NotificationQueue, ConsistencyCheckDetectsTornRecords) {
    const VnNotification good = MakeNotification(0x0123456789ABCDEFull);
    EXPECT_TRUE(IsConsistent(good));

    VnNotification torn = good;
    torn.type ^= 1u;
    EXPECT_FALSE(IsConsistent(torn));

    torn = good;
    torn.reserved0 ^= 1u;
    EXPECT_FALSE(IsConsistent(torn));

    torn = good;
    torn.blockTimestamp ^= 1ull;
    EXPECT_FALSE(IsConsistent(torn));

    // Swapping the voiceHandle (the sequence the other words are checked
    // against) while the payload words survive is a tear too.
    torn = good;
    torn.voiceHandle = good.voiceHandle + 1;
    EXPECT_FALSE(IsConsistent(torn));
}

// Case A, deterministic off-by-one, variant 1: five records into a four-slot
// ring with no interleaved drain. The fixed contract keeps the newest
// capacity - 1 == 3 records (sequences 3, 4, 5) in order and counts the two
// older ones as dropped — including the record the producer's next Push would
// reclaim. The pre-fix strict-'>' resync kept four (starting at sequence 2)
// and counted one.
TEST(NotificationQueue, OverflowSurvivorWindowIsCapacityMinusOne) {
    harken::NotificationQueue queue(4);
    ASSERT_EQ(4u, queue.Capacity());
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        queue.Push(MakeNotification(seq));
    }

    std::vector<std::uint64_t> delivered;
    VnNotification out{};
    while (queue.TryPop(out)) {
        delivered.push_back(out.voiceHandle);
    }

    EXPECT_EQ((std::vector<std::uint64_t>{3, 4, 5}), delivered);
    EXPECT_EQ(2u, queue.DroppedCount());
}

// Case A, variant 2: exactly capacity records with no drain, so lag ==
// capacity. The fixed contract treats the oldest as the indeterminate boundary
// and drops it (survivors = sequences 2, 3, 4; dropped = 1). The pre-fix
// strict-'>' test let lag == capacity through untouched, delivering all four
// with dropped == 0 — the off-by-one handed out a slot that the next Push
// would overwrite.
TEST(NotificationQueue, LagEqualToCapacityDropsBoundaryRecord) {
    harken::NotificationQueue queue(4);
    ASSERT_EQ(4u, queue.Capacity());
    for (std::uint64_t seq = 1; seq <= 4; ++seq) {
        queue.Push(MakeNotification(seq));
    }

    std::vector<std::uint64_t> delivered;
    VnNotification out{};
    while (queue.TryPop(out)) {
        delivered.push_back(out.voiceHandle);
    }

    EXPECT_EQ((std::vector<std::uint64_t>{2, 3, 4}), delivered);
    EXPECT_EQ(1u, queue.DroppedCount());
}

// Case B, conservation guardrail: whatever the survivor-window
// boundary, every pushed record is either delivered or counted as dropped,
// never double-counted or lost. The read cursor advances by one per delivery
// and jumps by the drop delta on resync, so delivered + dropped equals the
// pushed count once the ring is fully drained. This held both before and after
// the fix; it guards a future rewrite against miscounting.
TEST(NotificationQueue, DeliveredPlusDroppedEqualsPushedGuardrail) {
    harken::NotificationQueue queue(8);
    constexpr std::uint64_t kPushed = 500;
    for (std::uint64_t seq = 1; seq <= kPushed; ++seq) {
        queue.Push(MakeNotification(seq));
    }

    std::uint64_t delivered = 0;
    VnNotification out{};
    while (queue.TryPop(out)) {
        ++delivered;
    }

    EXPECT_EQ(kPushed, delivered + queue.DroppedCount());
}

// Case C, the concurrent guard. A producer laps a small ring while a
// slower consumer validates every delivered record. Under the fixed contract
// (survivor window capacity - 1 plus a post-copy write-cursor revalidation) no
// torn record can surface. The pre-fix implementation delivered the record at
// lag == capacity without revalidating the copy, so a producer writing the slot
// mid-copy could hand the consumer a record stitched from two sequence numbers.
// A time bound also guards against a consumer that fails to converge.
TEST(NotificationQueue, ConcurrentProducerNeverTearsRecords) {
    constexpr std::uint64_t kPushes = 200000;
    harken::NotificationQueue queue(8);

    std::atomic<bool> start{false};
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::uint64_t seq = 1; seq <= kPushes; ++seq) {
            queue.Push(MakeNotification(seq));
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::uint64_t delivered = 0;
    std::uint64_t tears = 0;
    VnNotification out{};
    const auto count = [&] {
        ++delivered;
        if (!IsConsistent(out)) {
            ++tears;
        }
    };

    start.store(true, std::memory_order_release);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timedOut = false;
    for (;;) {
        while (queue.TryPop(out)) {
            count();
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
        }
        if (timedOut) {
            break;
        }
        if (producerDone.load(std::memory_order_acquire)) {
            // The producerDone acquire synchronizes with every prior Push, so
            // this final drain observes the last write and empties the ring.
            while (queue.TryPop(out)) {
                count();
                if (std::chrono::steady_clock::now() >= deadline) {
                    timedOut = true;
                    break;
                }
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

    ASSERT_FALSE(timedOut) << "drain did not converge within the time bound";
    EXPECT_EQ(0u, tears) << "a record was overwritten mid-copy";
    EXPECT_EQ(kPushes, delivered + queue.DroppedCount());
}

} // namespace
