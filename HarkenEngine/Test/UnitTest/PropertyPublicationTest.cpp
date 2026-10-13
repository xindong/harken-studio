// Transactional property publication: two-phase publication with one
// HasRoom(N) admission check for
// the whole set, all-or-nothing push under forced queue pressure (zero
// partial application), whole-batch retry next tick with the deferral
// counted, and the admission reservation that bounds a deferred batch's
// delay at one tick under sustained single-command traffic. The section
// 10.4 capacity contract is covered at the same seam: worst-case growth
// beyond the queue capacity is refused with a log record carrying both
// numbers, and the batch buffer itself never grows past its bound.
//
// These tests exercise the publisher seam directly against a fake queue —
// including the restage flow (each tick's fresh gather REBUILDS a
// deferred batch at current values, the reservation tracking the rebuilt
// size) and the instance-layer worst-case accounting BindVoiceTarget
// maintains. The live-engine pressure goldens drive the same discipline
// through Vn_Update on the null backend.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"
#include "harken/param/PropertyPublisher.h"

#include <cstdint>
#include <vector>

namespace {

using harken::PropertyCommit;
using harken::PropertyPublisher;

// A command-queue stand-in the tests squeeze at will: freeSlots is the
// admission the preflight sees; every accepted push consumes one slot and
// records the commit, and every push attempt is counted, accepted or not,
// so a test can prove the preflight refused the SET without touching the
// queue. Admission queries are counted too, so a test can prove the
// preflight is ONE check for the whole set, never a per-commit re-query.
struct FakeQueue {
    std::uint32_t freeSlots = 0;
    std::uint32_t freeSlotsCalls = 0;
    std::uint32_t pushCalls = 0;
    std::vector<PropertyCommit> pushed;

    static std::uint32_t FreeSlots(void* ctx) {
        FakeQueue* queue = static_cast<FakeQueue*>(ctx);
        ++queue->freeSlotsCalls;
        return queue->freeSlots;
    }
    static bool Push(void* ctx, const PropertyCommit& commit) {
        FakeQueue* queue = static_cast<FakeQueue*>(ctx);
        ++queue->pushCalls;
        if (queue->freeSlots == 0) {
            return false;
        }
        --queue->freeSlots;
        queue->pushed.push_back(commit);
        return true;
    }
};

// One bus-volume commit per distinct bus, so a batch models the spec's
// many-target mix switch: same target row, distinct object ids.
PropertyCommit BusVolumeCommit(std::uint64_t busId, float resolvedDb) {
    PropertyCommit commit;
    commit.scopeKind = harken::kTargetScopeBus;
    commit.objectId = busId;
    commit.targetId = 0; // BusVolume
    commit.resolved = resolvedDb;
    commit.rampMs = 100;
    return commit;
}

TEST(PropertyPublication, PublishesWholeBatchWhenTheQueueHasRoom) {
    PropertyPublisher publisher(/*batchCapacity=*/8);
    FakeQueue queue;

    for (std::uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(10 + i, -6.0f - i)));
    }
    EXPECT_EQ(3u, publisher.StagedCount());

    // Room for exactly N: HasRoom(N) admits the set and all N push, off
    // ONE admission check for the whole set.
    queue.freeSlots = 3;
    EXPECT_TRUE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(1u, queue.freeSlotsCalls);
    ASSERT_EQ(3u, queue.pushed.size());
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(10 + i, queue.pushed[i].objectId) << "commit " << i;
        EXPECT_FLOAT_EQ(-6.0f - i, queue.pushed[i].resolved) << "commit " << i;
    }
    EXPECT_EQ(0u, publisher.StagedCount());
    EXPECT_EQ(0u, publisher.PendingReservation());
    EXPECT_EQ(0u, publisher.DeferredCount());
}

TEST(PropertyPublication, NoRoomPushesNothingKeepsBatchCountsDeferral) {
    PropertyPublisher publisher(/*batchCapacity=*/8);
    FakeQueue queue;

    for (std::uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(20 + i, -1.5f * i)));
    }

    // One slot short: the preflight refuses the whole set off ONE
    // admission check. Nothing was pushed and nothing was even attempted —
    // zero partial application.
    queue.freeSlots = 4;
    EXPECT_FALSE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(1u, queue.freeSlotsCalls);
    EXPECT_EQ(0u, queue.pushCalls);
    EXPECT_TRUE(queue.pushed.empty());

    // Every target stays dirty: the batch is still staged in full, and the
    // deferral was counted.
    EXPECT_EQ(5u, publisher.StagedCount());
    EXPECT_EQ(1u, publisher.DeferredCount());

    // Next tick the queue drained: the SAME batch publishes whole, in
    // stage order, off one more admission check, and the deferral count
    // does not grow on success. (Seam behavior WITHOUT a restage: a
    // caller that does not rebuild republishes the retained batch
    // verbatim. The engine's tick loop restages and re-gathers instead —
    // covered by the Restage tests below.)
    queue.freeSlots = 5;
    EXPECT_TRUE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(2u, queue.freeSlotsCalls);
    ASSERT_EQ(5u, queue.pushed.size());
    for (std::uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(20 + i, queue.pushed[i].objectId) << "commit " << i;
        EXPECT_FLOAT_EQ(-1.5f * i, queue.pushed[i].resolved) << "commit " << i;
    }
    EXPECT_EQ(0u, publisher.StagedCount());
    EXPECT_EQ(1u, publisher.DeferredCount());
}

TEST(PropertyPublication, DeferredBatchReservesAdmissionAgainstSingles) {
    PropertyPublisher publisher(/*batchCapacity=*/8);
    FakeQueue queue;

    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(30 + i, -3.0f)));
    }
    queue.freeSlots = 2;
    EXPECT_FALSE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(4u, publisher.PendingReservation());

    // From the moment of deferral, single-command producers admit only
    // into capacity beyond the pending batch's size: free == batch size
    // leaves nothing beyond the reservation.
    queue.freeSlots = 4;
    EXPECT_FALSE(publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue));
    queue.freeSlots = 5;
    EXPECT_TRUE(publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue));

    // Publishing the batch releases the reservation: a single command then
    // admits into any free slot.
    queue.freeSlots = 4;
    EXPECT_TRUE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(0u, publisher.PendingReservation());
    queue.freeSlots = 1;
    EXPECT_TRUE(publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue));
}

TEST(PropertyPublication, SustainedSingleTrafficCannotStarveDeferredBatch) {
    PropertyPublisher publisher(/*batchCapacity=*/8);
    FakeQueue queue;

    // Tick 1: heavy single-command traffic fills the queue before the
    // batch flushes; the 4-commit batch defers whole.
    queue.freeSlots = 6;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue));
        --queue.freeSlots; // the admitted single occupies its slot
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(40 + i, -9.0f)));
    }
    EXPECT_FALSE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(1u, publisher.DeferredCount());

    // Tick 2: the consumer drained everything, and the same steady single
    // traffic keeps arriving ahead of the flush. The reservation admits
    // singles only into capacity beyond the pending batch: exactly two of
    // six attempts land, the rest are held off.
    queue.freeSlots = 6;
    std::uint32_t admitted = 0;
    for (int i = 0; i < 6; ++i) {
        if (publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue)) {
            --queue.freeSlots;
            ++admitted;
        }
    }
    EXPECT_EQ(2u, admitted);

    // The reserved capacity is intact, so the deferred batch lands in
    // full on this very tick — the delay bound is one tick, and no second
    // deferral is ever counted.
    EXPECT_TRUE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    ASSERT_EQ(4u, queue.pushed.size());
    for (std::uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(40 + i, queue.pushed[i].objectId) << "commit " << i;
    }
    EXPECT_EQ(1u, publisher.DeferredCount());
    EXPECT_EQ(0u, publisher.StagedCount());
}

TEST(PropertyPublication, StageRefusesBeyondTheFixedBatchCapacity) {
    // The batch buffer is fixed at construction: the section-10.4 contract
    // keeps legal wirings inside it, and an over-bound gather is refused
    // per commit instead of growing storage.
    PropertyPublisher publisher(/*batchCapacity=*/4);
    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(50 + i, -1.0f)));
    }
    EXPECT_FALSE(publisher.Stage(BusVolumeCommit(54, -1.0f)));
    EXPECT_EQ(4u, publisher.StagedCount());
}

TEST(PropertyPublication, CapacityContractRefusesWorstCaseBeyondTheQueue) {
    // Section 10.4: the worst-case one-tick publication must fit the
    // command queue. Growth is admitted incrementally up to the bound,
    // refused with zero state change beyond it, and the refusal's log
    // record carries both numbers (proposed worst case, capacity).
    PropertyPublisher publisher(/*batchCapacity=*/8);
    harken::LogRing log;
    EXPECT_EQ(0u, publisher.WorstCaseCommits());
    EXPECT_EQ(8u, publisher.BatchCapacity());

    EXPECT_TRUE(publisher.TryGrowWorstCase(5, &log));
    EXPECT_TRUE(publisher.TryGrowWorstCase(3, &log)); // exactly at the bound
    EXPECT_EQ(8u, publisher.WorstCaseCommits());

    EXPECT_FALSE(publisher.TryGrowWorstCase(1, &log));
    EXPECT_EQ(8u, publisher.WorstCaseCommits()); // refusal changed nothing

    harken::LogRecord record{};
    ASSERT_TRUE(log.TryPop(record));
    EXPECT_EQ(harken::kLogCodePropertyCapacityRefused, record.code);
    EXPECT_EQ(9u, record.arg0); // proposed worst case
    EXPECT_EQ(8u, record.arg1); // queue capacity
    EXPECT_FALSE(log.TryPop(record)); // admitted growth logged nothing

    // Detaching releases demand (clamped at zero) and re-opens admission.
    publisher.ShrinkWorstCase(3);
    EXPECT_EQ(5u, publisher.WorstCaseCommits());
    EXPECT_TRUE(publisher.TryGrowWorstCase(1, &log));
    publisher.ShrinkWorstCase(100);
    EXPECT_EQ(0u, publisher.WorstCaseCommits());
}

TEST(PropertyPublication, RestageRebuildsThePendingBatchAndReservation) {
    // The dirty marks are the retry mechanism: each tick the caller
    // restages and re-gathers, so a deferred batch is REBUILT at current
    // values — possibly a different set. The reservation survives the
    // rebuild window and tracks the rebuilt batch's size (the rebuilt
    // batch IS the pending batch: the next whole-set admission needs
    // exactly its size, so pinning the stale size would over- or
    // under-reserve).
    PropertyPublisher publisher(/*batchCapacity=*/8);
    FakeQueue queue;

    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(60 + i, -3.0f)));
    }
    queue.freeSlots = 2;
    EXPECT_FALSE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(4u, publisher.PendingReservation());
    EXPECT_EQ(1u, publisher.DeferredCount());

    // Next tick: restage, then a fresh gather stages a DIFFERENT set (one
    // target was consumed elsewhere, values moved on). The reservation now
    // tracks the rebuilt size.
    publisher.Restage();
    EXPECT_EQ(0u, publisher.StagedCount());
    for (std::uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(70 + i, -7.0f - i)));
    }
    EXPECT_EQ(3u, publisher.PendingReservation());
    queue.freeSlots = 3;
    EXPECT_FALSE(publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue));
    queue.freeSlots = 4;
    EXPECT_TRUE(publisher.AdmitSingle(&FakeQueue::FreeSlots, &queue));

    // The rebuilt batch publishes whole: exactly the new set, in its stage
    // order — the discarded image never reaches the queue — and the
    // deferred flag clears on success.
    queue.freeSlots = 3;
    EXPECT_TRUE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    ASSERT_EQ(3u, queue.pushed.size());
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(70 + i, queue.pushed[i].objectId) << "commit " << i;
        EXPECT_FLOAT_EQ(-7.0f - i, queue.pushed[i].resolved) << "commit " << i;
    }
    EXPECT_EQ(0u, publisher.PendingReservation());
    EXPECT_EQ(0u, publisher.StagedCount());
    EXPECT_EQ(1u, publisher.DeferredCount());
}

TEST(PropertyPublication, RestageToEmptyPublishesTriviallyAndReleases) {
    // A deferred batch whose every target was consumed by other means
    // (direct setters cleaned them, voices ended) rebuilds to EMPTY: the
    // publish is trivial — no admission query — and the reservation lifts.
    PropertyPublisher publisher(/*batchCapacity=*/8);
    FakeQueue queue;

    for (std::uint32_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(publisher.Stage(BusVolumeCommit(80 + i, -2.0f)));
    }
    queue.freeSlots = 1;
    EXPECT_FALSE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(2u, publisher.PendingReservation());
    EXPECT_EQ(1u, queue.freeSlotsCalls);

    publisher.Restage();
    EXPECT_TRUE(
        publisher.Publish(&FakeQueue::FreeSlots, &FakeQueue::Push, &queue));
    EXPECT_EQ(1u, queue.freeSlotsCalls); // trivial publish queried nothing
    EXPECT_TRUE(queue.pushed.empty());
    EXPECT_EQ(0u, publisher.PendingReservation());
    EXPECT_EQ(1u, publisher.DeferredCount());
}

// --- section 10.4 wired into the instance layer ---

// A voice handle as the pool would mint it: generation in the high bits,
// slot in the low (the param system resolves by slot and verifies the
// full handle).
constexpr VnVoiceHandle kBoundVoice = (1ULL << 32) | 0u;
constexpr VnVoiceHandle kBoundVoiceReborn = (2ULL << 32) | 0u;
constexpr VnVoiceHandle kNeverStarted = (1ULL << 32) | 1u;

harken::Curve UnitCurve() {
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 0.0f}, {1.0f, -12.0f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    EXPECT_TRUE(curve.SetPoints(pts, handles, 2));
    return curve;
}

TEST(PropertyPublication, InstanceGrowthRefusesAtTheWorstCaseBound) {
    // BindVoiceTarget maintains the section-10.4 bound incrementally:
    // unique (voice, target) pairs on live voices count, duplicates and
    // dead-voice bindings do not, and growth past the queue capacity
    // refuses with zero side effects plus the two-number log record.
    harken::ParamSystem params(harken::EngineConfig{});
    PropertyPublisher publisher(/*batchCapacity=*/2);
    harken::LogRing log;
    params.AttachPublicationCapacity(&publisher, &log);
    params.OnVoiceStarted(kBoundVoice, 0.0f, 1.0);

    const harken::Curve curve = UnitCurve();
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoice,
                                       harken::kTargetVoiceVolume, curve));
    // Same target again: a second contributor, but still ONE unique
    // target — one command per target per tick.
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(2), kBoundVoice,
                                       harken::kTargetVoiceVolume, curve));
    EXPECT_EQ(1u, publisher.WorstCaseCommits());
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoice,
                                       harken::kTargetVoicePitch, curve));
    EXPECT_EQ(2u, publisher.WorstCaseCommits());

    // A third unique pair would exceed the queue: refused, nothing
    // appended, both numbers logged.
    EXPECT_FALSE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoice,
                                        harken::kTargetVoicePan, curve));
    EXPECT_EQ(2u, publisher.WorstCaseCommits());
    std::vector<harken::ParamSystem::ApiDirectState> apiDirect;
    std::vector<harken::ParamSystem::BindingState> bindings;
    params.CaptureVoiceState(kBoundVoice, &apiDirect, &bindings);
    EXPECT_EQ(3u, bindings.size()); // the refused binding left no entry
    harken::LogRecord record{};
    ASSERT_TRUE(log.TryPop(record));
    EXPECT_EQ(harken::kLogCodePropertyCapacityRefused, record.code);
    EXPECT_EQ(3u, record.arg0);
    EXPECT_EQ(2u, record.arg1);
    EXPECT_FALSE(log.TryPop(record));

    // A binding on a voice that never started is inert forever (handles
    // are generational): no demand, accepted even at the bound.
    EXPECT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kNeverStarted,
                                       harken::kTargetVoicePan, curve));
    EXPECT_EQ(2u, publisher.WorstCaseCommits());
}

TEST(PropertyPublication, BindOnLiveVoiceMarksFutureDemand) {
    // A new binding on a live voice is known future demand: its first
    // evaluation runs at the next gather whatever the Variable holds, so
    // the demand census grows AT THE BIND — an active deferral's
    // reservation covers it before any later admission. Evaluation is
    // the only consumer of the mark.
    harken::ParamSystem params(harken::EngineConfig{});
    params.OnVoiceStarted(kBoundVoice, 0.0f, 1.0);
    EXPECT_EQ(0u, params.DirtyTargetCount());

    const harken::Curve curve = UnitCurve();
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoice,
                                       harken::kTargetVoiceVolume, curve));
    EXPECT_EQ(1u, params.DirtyTargetCount());
    // A second binding on the same target claims no extra seat (one
    // command per target per tick), and a binding on a voice that never
    // started is inert — no demand.
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(2), kBoundVoice,
                                       harken::kTargetVoiceVolume, curve));
    EXPECT_EQ(1u, params.DirtyTargetCount());
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kNeverStarted,
                                       harken::kTargetVoicePan, curve));
    EXPECT_EQ(1u, params.DirtyTargetCount());

    // Evaluation consumes the mark. With no Variable value at either
    // scope the bindings contribute nothing, so nothing publishes and
    // the census returns to zero.
    std::uint32_t handed = 0;
    params.FlushCommits([&handed](const harken::ParamCommit&) {
        ++handed;
        return harken::FlushOutcome::Committed;
    });
    EXPECT_EQ(0u, handed);
    EXPECT_EQ(0u, params.DirtyTargetCount());
}

TEST(PropertyPublication, SlotReuseReleasesWorstCaseDemand) {
    // Voice end frees binding state lazily at slot reuse: the dead
    // occupant's unique targets leave the worst case, reopening capacity
    // for the new voice's bindings.
    harken::ParamSystem params(harken::EngineConfig{});
    PropertyPublisher publisher(/*batchCapacity=*/2);
    harken::LogRing log;
    params.AttachPublicationCapacity(&publisher, &log);
    params.OnVoiceStarted(kBoundVoice, 0.0f, 1.0);

    const harken::Curve curve = UnitCurve();
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoice,
                                       harken::kTargetVoiceVolume, curve));
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoice,
                                       harken::kTargetVoicePitch, curve));
    EXPECT_EQ(2u, publisher.WorstCaseCommits());

    // The slot restarts under a new generation: the old handle's two
    // unique targets shrink away and fresh growth is admitted again.
    params.OnVoiceStarted(kBoundVoiceReborn, 0.0f, 1.0);
    EXPECT_EQ(0u, publisher.WorstCaseCommits());
    EXPECT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(1), kBoundVoiceReborn,
                                       harken::kTargetVoiceVolume, curve));
    EXPECT_EQ(1u, publisher.WorstCaseCommits());
}

} // namespace
