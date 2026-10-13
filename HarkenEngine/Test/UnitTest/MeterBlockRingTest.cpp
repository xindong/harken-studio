// MeterBlockRing unit tests: FIFO
// order and drain-all, the full-ring overflow side-record (peaks never
// silently lost, at worst time-compressed; delivered with no dependence
// on future appends; no duplicate delivery) with its counter and
// position stamp, the armed gate, and the power-of-two capacity
// round-up. Also the MeterSystem drain protocol over the ring: the
// per-pairing epoch gate on ordinary record positions, the pairing
// epoch as the ENTRY-drain boundary, the compressed block's delivery to
// every active pairing through the VALIDATED take (defer while earlier
// records remain queued; merge-back on a failed re-check), existence of
// a compressed span judged by the counter (an all-silent span still
// costs one release step), and the drain boundary that leaves records
// appended past it for the next round.
#include "Common/ObjectIdTestSupport.h"
#include "harken/core/MeterBlockRing.h"

#include "harken/param/MeterSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

namespace {

using harken::MeterBlockRing;

// The engine-identical per-block one-pole coefficient (same expression,
// same double-to-float narrowing) at the 256/48000 session shape the
// protocol tests below construct MeterSystem with.
float ProtocolBlockCoeff(std::uint32_t ms) {
    if (ms == 0) {
        return 0.0f;
    }
    const double tauFrames = 48000.0 * static_cast<double>(ms) / 1000.0;
    return static_cast<float>(std::exp(-256.0 / tauFrames));
}

TEST(MeterBlockRing, PushAndDrainPreserveOrderAndPayload) {
    MeterBlockRing ring(4, 2);
    const float a[2] = {0.1f, 0.2f};
    const float b[2] = {0.3f, 0.4f};
    const float c[2] = {0.5f, 0.6f};
    ring.Push(10, a, 2);
    ring.Push(11, b, 2);
    ring.Push(12, c, 2);

    std::uint64_t blockIndex = 0;
    const float* peaks = nullptr;
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(blockIndex, 10u);
    EXPECT_EQ(peaks[0], 0.1f);
    EXPECT_EQ(peaks[1], 0.2f);
    ring.PopFront();
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(blockIndex, 11u);
    EXPECT_EQ(peaks[0], 0.3f);
    ring.PopFront();
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(blockIndex, 12u);
    EXPECT_EQ(peaks[1], 0.6f);
    ring.PopFront();
    EXPECT_FALSE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(ring.OverflowCount(), 0u);
}

TEST(MeterBlockRing, ThreeTapRowsPreserveLanesAndLegacyView) {
    MeterBlockRing ring(2, 4);
    const float pre[2] = {0.1f, 0.2f};
    const float post[2] = {0.3f, 0.4f};
    const float effects[2] = {0.5f, 0.6f};
    ring.PushTaps(7, pre, post, effects, 2);

    std::uint64_t blockIndex = 0;
    const float* preView = nullptr;
    const float* postView = nullptr;
    const float* effectsView = nullptr;
    ASSERT_TRUE(ring.FrontTaps(&blockIndex, &preView, &postView,
                               &effectsView));
    EXPECT_EQ(blockIndex, 7u);
    EXPECT_FLOAT_EQ(preView[0], 0.1f);
    EXPECT_FLOAT_EQ(preView[1], 0.2f);
    EXPECT_FLOAT_EQ(postView[0], 0.3f);
    EXPECT_FLOAT_EQ(postView[1], 0.4f);
    EXPECT_FLOAT_EQ(effectsView[0], 0.5f);
    EXPECT_FLOAT_EQ(effectsView[1], 0.6f);
    for (std::uint32_t bus = 2; bus < 4; ++bus) {
        EXPECT_FLOAT_EQ(preView[bus], 0.0f);
        EXPECT_FLOAT_EQ(postView[bus], 0.0f);
        EXPECT_FLOAT_EQ(effectsView[bus], 0.0f);
    }

    const float* legacyView = nullptr;
    ASSERT_TRUE(ring.Front(&blockIndex, &legacyView));
    EXPECT_FLOAT_EQ(legacyView[0], effectsView[0]);
    EXPECT_FLOAT_EQ(legacyView[1], effectsView[1]);
}

TEST(MeterBlockRing, ThreeTapOverflowMaxHoldsEveryLane) {
    MeterBlockRing ring(1, 2);
    const float silent[2] = {0.0f, 0.0f};
    ring.PushTaps(0, silent, silent, silent, 2);

    const float preA[2] = {0.2f, 0.8f};
    const float postA[2] = {0.4f, 0.3f};
    const float effectsA[2] = {0.6f, 0.1f};
    const float preB[2] = {0.5f, 0.7f};
    const float postB[2] = {0.1f, 0.9f};
    const float effectsB[2] = {0.3f, 0.4f};
    ring.PushTaps(1, preA, postA, effectsA, 2);
    ring.PushTaps(2, preB, postB, effectsB, 2);

    float pre[2] = {};
    float post[2] = {};
    float effects[2] = {};
    ring.TakeOverflowTaps(pre, post, effects);
    EXPECT_FLOAT_EQ(pre[0], 0.5f);
    EXPECT_FLOAT_EQ(pre[1], 0.8f);
    EXPECT_FLOAT_EQ(post[0], 0.4f);
    EXPECT_FLOAT_EQ(post[1], 0.9f);
    EXPECT_FLOAT_EQ(effects[0], 0.6f);
    EXPECT_FLOAT_EQ(effects[1], 0.4f);
    EXPECT_EQ(ring.OverflowCount(), 2u);
}

TEST(MeterSystem, ProbeGenerationsAreGlobalAcrossReusedSlots) {
    harken::MeterSystem meters(2, 256, 48000);
    std::uint32_t first = 0;
    ASSERT_EQ(VN_OK, meters.Install(
                         harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 1,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0, &first));
    EXPECT_EQ(1u, first);
    harken::MeterSystem::Probe* firstRow = meters.Find(harkentest::ObjectId(100));
    ASSERT_NE(nullptr, firstRow);
    meters.BeginWithdrawal(*firstRow, 0);
    EXPECT_EQ(0u, meters.ServiceWithdrawals(false, 0, 0, 0));
    EXPECT_EQ(1u, meters.ServiceWithdrawals(false, 0, 0, 1));

    std::uint32_t second = 0;
    ASSERT_EQ(VN_OK, meters.Install(
                         harkentest::ObjectId(200), harkentest::ObjectId(200), 0, 1,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0, &second));
    EXPECT_EQ(2u, second);
    std::uint32_t third = 0;
    ASSERT_EQ(VN_OK, meters.Install(
                         harkentest::ObjectId(100), harkentest::ObjectId(100), 1, 1,
                         VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0, &third));
    EXPECT_EQ(3u, third);
    EXPECT_NE(first, meters.Find(harkentest::ObjectId(100))->identity.probeGeneration);
}

TEST(MeterBlockRing, OrdinaryRecordsDoNotCrossBusGenerations) {
    MeterBlockRing ring(4, 1);
    harken::MeterSystem meters(1, 256, 48000);
    ASSERT_EQ(VN_OK,
              meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 2, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0));

    const float oldPeak[1] = {0.9f};
    const float newPeak[1] = {0.2f};
    const std::uint32_t oldGeneration[1] = {1};
    const std::uint32_t newGeneration[1] = {2};
    ring.Push(0, oldPeak, 1, oldGeneration);
    ring.Push(1, newPeak, 1, newGeneration);

    EXPECT_EQ(2u, meters.Drain(ring));
    EXPECT_NEAR(20.0f * std::log10(0.2f), meters.ValueDb(harkentest::ObjectId(100)), 1e-5f);
}

TEST(MeterBlockRing, OverflowReplacesPeakWhenBusGenerationChanges) {
    MeterBlockRing ring(1, 1);
    harken::MeterSystem meters(1, 256, 48000);
    ASSERT_EQ(VN_OK,
              meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 2, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0));

    const float silent[1] = {0.0f};
    const float oldPeak[1] = {0.9f};
    const float newPeak[1] = {0.2f};
    const std::uint32_t oldGeneration[1] = {1};
    const std::uint32_t newGeneration[1] = {2};
    ring.Push(0, silent, 1, oldGeneration);
    ring.Push(1, oldPeak, 1, oldGeneration);
    ring.Push(2, newPeak, 1, newGeneration);

    EXPECT_EQ(2u, meters.Drain(ring));
    EXPECT_NEAR(20.0f * std::log10(0.2f), meters.ValueDb(harkentest::ObjectId(100)), 1e-5f);
}

TEST(MeterBlockRing, MergeBackCannotOverwriteANewerBusGeneration) {
    MeterBlockRing ring(1, 1);
    const float silent[1] = {0.0f};
    const float oldPeak[1] = {0.9f};
    const float newPeak[1] = {0.2f};
    const std::uint32_t oldGeneration[1] = {1};
    const std::uint32_t newGeneration[1] = {2};
    ring.Push(0, silent, 1, oldGeneration);
    ring.Push(1, oldPeak, 1, oldGeneration);

    float taken[1] = {0.0f};
    std::uint32_t takenGenerations[harken::kMeterTapLaneCount] = {};
    ring.TakeOverflowTaps(nullptr, nullptr, taken, takenGenerations);
    ASSERT_EQ(0.9f, taken[0]);
    ASSERT_EQ(1u, takenGenerations[0]);

    ring.Push(2, newPeak, 1, newGeneration);
    ring.MergeBackOverflowTaps(nullptr, nullptr, taken, takenGenerations);

    ring.TakeOverflowTaps(nullptr, nullptr, taken, takenGenerations);
    EXPECT_EQ(0.2f, taken[0]);
    EXPECT_EQ(2u, takenGenerations[0]);
}

TEST(MeterBlockRing, ShortPeakRowZeroFillsRemainingBuses) {
    MeterBlockRing ring(2, 4);
    const float one[1] = {0.9f};
    ring.Push(0, one, 1);
    std::uint64_t blockIndex = 0;
    const float* peaks = nullptr;
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(peaks[0], 0.9f);
    EXPECT_EQ(peaks[1], 0.0f);
    EXPECT_EQ(peaks[2], 0.0f);
    EXPECT_EQ(peaks[3], 0.0f);
}

TEST(MeterBlockRing, OverflowSideRecordDeliversWithoutFurtherAppend) {
    MeterBlockRing ring(2, 1);
    ASSERT_EQ(ring.Capacity(), 2u);
    const float low[1] = {0.1f};
    const float mid[1] = {0.5f};
    const float high[1] = {0.8f};
    float taken[1] = {0.0f};

    ring.Push(0, low, 1);
    ring.Push(1, low, 1); // full from here (positions 0 and 1, write = 2)
    ring.TakeOverflowLanes(taken);
    EXPECT_EQ(taken[0], 0.0f); // nothing compressed yet

    // Full: overflowing blocks max-hold into the dedicated side-record;
    // published slots are never touched. Both compressed blocks would
    // have taken append position 2 (the write cursor is parked while
    // full), so the diagnostic stamp reads 2.
    ring.Push(2, high, 1);
    ring.Push(3, mid, 1);
    EXPECT_EQ(ring.OverflowCount(), 2u);

    // The ordinary records are exactly what was appended.
    std::uint64_t blockIndex = 0;
    const float* peaks = nullptr;
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(blockIndex, 0u);
    EXPECT_EQ(peaks[0], 0.1f);
    ring.PopFront();
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(blockIndex, 1u);
    EXPECT_EQ(peaks[0], 0.1f);
    ring.PopFront();
    ASSERT_FALSE(ring.Front(&blockIndex, &peaks));

    // The side-record delivers with ZERO further appends, and the stamp
    // reads the merge position the validated take checks against.
    ring.TakeOverflowLanes(taken);
    EXPECT_EQ(taken[0], 0.8f);
    EXPECT_EQ(ring.OverflowPosition(), 2u);

    // Taken means taken: no duplicate delivery on a second take.
    ring.TakeOverflowLanes(taken);
    EXPECT_EQ(taken[0], 0.0f);

    // Nothing is parked producer-side either: a later append carries
    // only its own block's peaks, and the counter holds.
    ring.Push(4, mid, 1);
    ASSERT_TRUE(ring.Front(&blockIndex, &peaks));
    EXPECT_EQ(blockIndex, 4u);
    EXPECT_EQ(peaks[0], 0.5f);
    EXPECT_EQ(ring.OverflowCount(), 2u);
}

// A compressed span that straddles a pairing entry can contain one peak from
// before the entry and one from after it.  This is the bounded attribution
// degradation documented for an already-overflowing ring: the old peak may
// reach the new pairing's first compressed step, but never becomes an
// ordinary pre-epoch record.
TEST(MeterBlockRing, DeferredOverflowMayCarryOnePrePairingPeak) {
    harken::MeterBlockRing ring(2, 1);
    harken::MeterSystem meters(1, 256, 48000);
    const float silent[1] = {0.0f};
    const float oldPeak[1] = {0.9f};
    const float newPeak[1] = {0.2f};

    ring.Push(0, silent, 1);
    ring.Push(1, silent, 1); // full at write position 2
    ring.Push(2, oldPeak, 1); // pre-entry overflow, stamped at position 2
    const std::uint64_t entry = ring.WriteCursor();
    ASSERT_EQ(entry, 2u);

    // The consumer frees one slot while the pairing entry drain is in
    // flight.  The producer then appends an ordinary record and overflows
    // again at position 3, so the side-record remains deferred at front 2.
    ring.PopFront();
    ring.Push(3, newPeak, 1); // ordinary append at position 2
    ring.Push(4, newPeak, 1); // post-entry overflow, stamped at position 3
    ASSERT_EQ(ring.OverflowCount(), 2u);
    ASSERT_EQ(meters.DrainUpTo(ring, entry), 1u);
    ASSERT_EQ(ring.FrontPosition(), entry);

    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, entry),
              VN_OK);
    EXPECT_EQ(meters.Drain(ring), 2u); // ordinary post-entry + one compressed
    EXPECT_FLOAT_EQ(meters.ValueDb(harkentest::ObjectId(100)), 20.0f * std::log10(oldPeak[0]));
}

// When an overflow merge is stamped at position 2, a consumer can free a
// slot and let an ordinary record publish at that same position before the
// side-record is taken.  Overflow mode explicitly permits this bounded
// ordering compression: the ordinary record is folded first, then the
// compressed side-record.  Use nonzero release and a low ordinary peak so an
// attack/release inversion changes the final envelope and is observable.
TEST(MeterBlockRing, SamePositionOverflowUsesDocumentedCompressedOrder) {
    harken::MeterBlockRing ring(2, 1);
    harken::MeterSystem meters(1, 256, 48000);
    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 20, 100, 0),
              VN_OK);
    const float initial[1] = {0.8f};
    const float silent[1] = {0.0f};
    const float low[1] = {0.2f};

    ring.Push(0, initial, 1);
    ring.Push(1, silent, 1); // full at write position 2
    ring.Push(2, initial, 1); // overflow side-record stamped at position 2

    // Establish a hot envelope while leaving the earlier silent record and
    // the position-2 overflow pending.  Freeing the first slot lets the
    // producer publish a normal position-2 record before the side take.
    ASSERT_EQ(meters.DrainUpTo(ring, 1), 1u);
    ring.Push(3, low, 1); // ordinary record published at position 2

    EXPECT_EQ(meters.Drain(ring), 3u); // silent, low, then compressed peak

    const float release = ProtocolBlockCoeff(100);
    const float attack = ProtocolBlockCoeff(20);
    float expected = 0.0f;
    const auto step = [&](float peak) {
        const float coeff = peak >= expected ? attack : release;
        expected = peak + (expected - peak) * coeff;
    };
    step(initial[0]);
    step(0.0f);
    step(low[0]);
    step(initial[0]);
    EXPECT_FLOAT_EQ(meters.ValueDb(harkentest::ObjectId(100)), 20.0f * std::log10(expected));
}

// Exercise the atomic overflow lanes against a concurrent producer.  The
// ring stays full so every producer write takes the side-record path.  The
// consumer may coalesce any number of counter increments into one take, but
// the release counter and per-lane max must remain exact.
TEST(MeterBlockRing, ConcurrentOverflowTakePreservesCounterAndMax) {
    constexpr std::uint32_t kPushes = 20000;
    harken::MeterBlockRing ring(1, 2);
    const float fill[2] = {0.0f, 0.0f};
    ring.Push(0, fill, 2); // keep the single-slot ring full

    std::atomic<bool> start{false};
    std::atomic<bool> firstOverflowReady{false};
    std::atomic<bool> firstTakeDone{false};
    std::atomic<bool> producerObservedFirstTakeBeforeResume{false};
    std::atomic<bool> producerDone{false};
    float observed0 = 0.0f;
    float observed1 = 0.0f;
    std::uint32_t highTakeCount = 0;
    bool firstTakeObserved = false;
    const auto recordTakenLanes = [&](const float* taken) {
        observed0 = std::max(observed0, taken[0]);
        observed1 = std::max(observed1, taken[1]);
        if (taken[0] == 1.0f) {
            ++highTakeCount;
        }
    };

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const float first[2] = {0.5f, 0.25f};
        ring.Push(1, first, 2);
        firstOverflowReady.store(true, std::memory_order_release);
        while (!firstTakeDone.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        producerObservedFirstTakeBeforeResume.store(
            true, std::memory_order_release);

        for (std::uint32_t i = 1; i < kPushes; ++i) {
            const bool sentinel = i == kPushes / 2;
            const float peaks[2] = {sentinel ? 1.0f : 0.5f,
                                    sentinel ? 0.75f : 0.25f};
            ring.Push(static_cast<std::uint64_t>(i + 1), peaks, 2);
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        start.store(true, std::memory_order_release);
        while (!firstOverflowReady.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::uint64_t seen = 0;
        float taken[2] = {0.0f, 0.0f};
        for (;;) {
            const std::uint64_t count = ring.OverflowCount();
            if (count != seen) {
                ring.TakeOverflowLanes(taken);
                recordTakenLanes(taken);
                if (seen == 0) {
                    firstTakeObserved =
                        taken[0] == 0.5f && taken[1] == 0.25f;
                    firstTakeDone.store(true, std::memory_order_release);
                }
                seen = count;
                continue;
            }
            if (producerDone.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::yield();
        }
        // Close the final merge/take window if the producer's last release
        // store was observed just after the loop's count sample.
        if (ring.OverflowCount() != seen) {
            ring.TakeOverflowLanes(taken);
            recordTakenLanes(taken);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(firstTakeDone.load(std::memory_order_acquire));
    EXPECT_TRUE(producerObservedFirstTakeBeforeResume.load(
        std::memory_order_acquire));
    EXPECT_TRUE(producerDone.load(std::memory_order_acquire));
    EXPECT_EQ(ring.OverflowCount(), static_cast<std::uint64_t>(kPushes));
    EXPECT_FLOAT_EQ(observed0, 1.0f);
    EXPECT_FLOAT_EQ(observed1, 0.75f);
    EXPECT_TRUE(firstTakeObserved);
    EXPECT_EQ(highTakeCount, 1u); // the sentinel peak is delivered once
}

// The MeterSystem drain protocol over the ring, standalone (no engine):
// a pairing consumes ordinary records only at or past its epoch position.
TEST(MeterBlockRing, DrainSkipsOrdinaryRecordsBeforeThePairingEpoch) {
    harken::MeterBlockRing ring(4, 2);
    harken::MeterSystem meters(2, 256, 48000);
    const float hot[2] = {0.5f, 0.5f};

    ring.Push(0, hot, 2);
    ring.Push(1, hot, 2);
    // Bus 0 pairs before the records existed (epoch 0); bus 1 pairs after
    // them (epoch = write cursor = 2).
    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0),
              VN_OK);
    ASSERT_EQ(meters.Install(harkentest::ObjectId(101), harkentest::ObjectId(101), 1, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, ring.WriteCursor()),
              VN_OK);

    EXPECT_EQ(meters.Drain(ring), 2u);
    EXPECT_GT(meters.ValueDb(harkentest::ObjectId(100)), harken::kMeterFloorDb);
    EXPECT_EQ(meters.ValueDb(harkentest::ObjectId(101)), harken::kMeterFloorDb);

    // A record appended from the epoch on reaches both pairings.
    ring.Push(2, hot, 2);
    EXPECT_EQ(meters.Drain(ring), 1u);
    EXPECT_GT(meters.ValueDb(harkentest::ObjectId(101)), harken::kMeterFloorDb);
}

// The compressed block carries no per-pairing gate at this layer: in the
// engine, the pairing call's entry drain consumes (or defers) the
// side-record at the entry boundary, so an applied block's content
// post-dates every active consumer ("from this call on" is defined at
// call entry).
TEST(MeterBlockRing, CompressedBlockDeliversToAllActivePairings) {
    harken::MeterBlockRing ring(2, 2);
    harken::MeterSystem meters(2, 256, 48000);
    const float silent[2] = {0.0f, 0.0f};
    const float hot[2] = {0.5f, 0.5f};

    ring.Push(0, silent, 2);
    ring.Push(1, silent, 2); // full; write parked at 2
    ring.Push(2, hot, 2);    // compressed
    EXPECT_EQ(ring.OverflowCount(), 1u);

    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0),
              VN_OK);
    ASSERT_EQ(meters.Install(harkentest::ObjectId(101), harkentest::ObjectId(101), 1, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 3),
              VN_OK);

    // 2 ordinary (silent) records — epoch-gated as usual — plus 1
    // compressed block delivered to BOTH pairings.
    EXPECT_EQ(meters.Drain(ring), 3u);
    EXPECT_GT(meters.ValueDb(harkentest::ObjectId(100)), harken::kMeterFloorDb);
    EXPECT_GT(meters.ValueDb(harkentest::ObjectId(101)), harken::kMeterFloorDb);
    EXPECT_EQ(ring.OverflowPosition(), 2u); // the merge position
}

// Existence of a compressed span is the COUNTER's delta, not any lane
// value: an all-silent compressed span still advances ballistics exactly
// one release step (time compression folds many blocks into one, never
// into zero).
TEST(MeterBlockRing, SilentCompressedBlockStillAdvancesBallistics) {
    harken::MeterBlockRing ring(2, 1);
    harken::MeterSystem meters(1, 256, 48000);
    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 50, 0),
              VN_OK); // instant attack, 50 ms release
    const float hot[1] = {0.5f};
    const float silent[1] = {0.0f};
    ring.Push(0, hot, 1);
    ring.Push(1, silent, 1); // full from here
    ring.Push(2, silent, 1); // silent compressed span: lanes stay zero
    EXPECT_EQ(ring.OverflowCount(), 1u);

    // hot -> one ordinary silent release step -> one COMPRESSED silent
    // release step. Skipping the compressed step would leave the
    // envelope one release factor too hot.
    EXPECT_EQ(meters.Drain(ring), 3u);
    const float release = ProtocolBlockCoeff(50);
    float env = 0.5f; // instant attack landed on the peak
    env = 0.0f + (env - 0.0f) * release;
    env = 0.0f + (env - 0.0f) * release;
    EXPECT_FLOAT_EQ(meters.ValueDb(harkentest::ObjectId(100)), 20.0f * std::log10(env));
}

// The drain boundary: records appended past it survive the round and are
// consumed by the next one — the mechanism that keeps a compressed block
// from being applied after ordinary blocks that are really LATER than it.
TEST(MeterBlockRing, DrainBoundaryLeavesRecordsPushedPastIt) {
    harken::MeterBlockRing ring(4, 1);
    harken::MeterSystem meters(1, 256, 48000);
    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0),
              VN_OK); // instant both ways
    const float hot[1] = {0.5f};
    const float silent[1] = {0.0f};

    ring.Push(0, hot, 1);
    const std::uint64_t boundary = ring.WriteCursor();
    ring.Push(1, silent, 1); // "appended while the drain runs"

    // Only the record before the boundary is consumed this round...
    EXPECT_EQ(meters.DrainUpTo(ring, boundary), 1u);
    EXPECT_GT(meters.ValueDb(harkentest::ObjectId(100)), harken::kMeterFloorDb);

    // ...and the later record is intact for the next round.
    EXPECT_EQ(meters.Drain(ring), 1u);
    EXPECT_EQ(meters.ValueDb(harkentest::ObjectId(100)), harken::kMeterFloorDb);
}

// The pairing epoch IS the entry drain's boundary — one cursor sample at
// call entry serves as both. Replayed single-threaded: records pushed
// after the sample play the role of blocks the audio thread completes
// while the wiring call runs; they must survive the entry round and
// reach the NEW pairing ("from this call on" is call entry, not drain
// exit — an epoch re-sampled after the drain would skip them forever).
TEST(MeterBlockRing, RecordsAppendedDuringTheEntryDrainReachTheNewPairing) {
    harken::MeterBlockRing ring(8, 1);
    harken::MeterSystem meters(1, 256, 48000);
    const float silent[1] = {0.0f};
    const float hot[1] = {0.5f};
    ring.Push(0, silent, 1);
    ring.Push(1, silent, 1);
    ring.Push(2, silent, 1);

    const std::uint64_t entry = ring.WriteCursor(); // the one sample
    ring.Push(3, hot, 1); // "appended while the entry drain runs"
    ring.Push(4, hot, 1);
    EXPECT_EQ(meters.DrainUpTo(ring, entry), 3u); // the entry drain
    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, entry),
              VN_OK);

    // The during-call records deliver to the new pairing next round.
    EXPECT_EQ(meters.Drain(ring), 2u);
    EXPECT_GT(meters.ValueDb(harkentest::ObjectId(100)), harken::kMeterFloorDb);
}

// The validated take's defer gate: while a queued record is EARLIER than
// the newest merge, the compressed block must not apply this round —
// its content would jump ahead of peaks that really precede it. The
// deferred block loses nothing: the untouched lanes and pending counter
// delta deliver it once the earlier records are consumed.
TEST(MeterBlockRing, CompressedTakeDefersWhileEarlierRecordsRemainQueued) {
    harken::MeterBlockRing ring(2, 1);
    harken::MeterSystem meters(1, 256, 48000);
    ASSERT_EQ(meters.Install(harkentest::ObjectId(100), harkentest::ObjectId(100), 0, 0, VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, 0),
              VN_OK); // instant both ways
    const float silent[1] = {0.0f};
    const float hot[1] = {0.8f};
    ring.Push(0, silent, 1);
    ring.Push(1, silent, 1); // full; write parked at 2
    ring.Push(2, hot, 1);    // merged: stamp 2, counter 1
    EXPECT_EQ(ring.OverflowCount(), 1u);

    // A round bounded before the stamp leaves a queued earlier record:
    // one ordinary silent step only, no compressed step, envelope at
    // silence — the hot compressed peak did NOT jump the queue.
    EXPECT_EQ(meters.DrainUpTo(ring, 1), 1u);
    EXPECT_EQ(meters.ValueDb(harkentest::ObjectId(100)), harken::kMeterFloorDb);

    // The next full round consumes the remaining earlier record, then
    // takes and applies the deferred block — proof the deferred round
    // moved neither the lanes nor the watermark.
    EXPECT_EQ(meters.Drain(ring), 2u);
    EXPECT_FLOAT_EQ(meters.ValueDb(harkentest::ObjectId(100)), 20.0f * std::log10(0.8f));
}

// The take-failure merge-back is a concurrency defense (a merge landing
// inside the take window): with a single-threaded producer the re-check
// cannot fail inside drain, so this pins the path's max-fold semantics
// directly — merged-back values fold with newer merges per lane, and a
// later take delivers the fold (nothing lost, nothing duplicated).
TEST(MeterBlockRing, MergeBackRestoresTakenLanesByMaxFold) {
    MeterBlockRing ring(2, 2);
    const float fill[2] = {0.0f, 0.0f};
    ring.Push(0, fill, 2);
    ring.Push(1, fill, 2); // full from here
    const float first[2] = {0.6f, 0.2f};
    ring.Push(2, first, 2); // merged
    float taken[2] = {0.0f, 0.0f};
    ring.TakeOverflowLanes(taken);
    EXPECT_EQ(taken[0], 0.6f);
    EXPECT_EQ(taken[1], 0.2f);

    // A newer merge lands while the taken values are in hand...
    const float second[2] = {0.4f, 0.5f};
    ring.Push(3, second, 2);
    // ...and the merge-back max-folds instead of overwriting.
    ring.MergeBackOverflowLanes(taken);
    ring.TakeOverflowLanes(taken);
    EXPECT_EQ(taken[0], 0.6f); // kept: the merged-back peak is higher
    EXPECT_EQ(taken[1], 0.5f); // kept: the newer merge is higher
}

TEST(MeterBlockRing, ArmedGateFollowsPairingCount) {
    MeterBlockRing ring(4, 1);
    EXPECT_FALSE(ring.Armed());
    ring.SetArmedCount(1);
    EXPECT_TRUE(ring.Armed());
    ring.SetArmedCount(0);
    EXPECT_FALSE(ring.Armed());
}

TEST(MeterBlockRing, CapacityRoundsUpToPowerOfTwo) {
    MeterBlockRing ring(3, 1);
    EXPECT_EQ(ring.Capacity(), 4u);
    MeterBlockRing exact(8, 1);
    EXPECT_EQ(exact.Capacity(), 8u);
}

} // namespace
