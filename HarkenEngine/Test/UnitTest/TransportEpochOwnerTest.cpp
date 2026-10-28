// TransportEpochOwner and transport-rebuild contract tests.
//
// The owner side of the epoch: it mints from 1, raises monotonically, and refuses
// at the wrap boundary with a terminal RuntimeRecreateRequired rather than reusing
// a value. The rebuild cases pin the two ways old-geometry work stops being
// executed after a raise: the existing constant-time stale no-op (a consumer
// reading the pre-reset pool under the new epoch sees nothing), and the reset
// sequence (the pool re-opened under the new epoch holds only new work).

#include "harken/core/DeferredIntentPool.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TimelinePlanner.h"
#include "harken/core/TransportEpochOwner.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

using harken::ConsumedEvent;
using harken::DeferredIntentPool;
using harken::EpochRaiseStatus;
using harken::EventTemplate;
using harken::OccurrenceStatePool;
using harken::PlayableInstanceHandle;
using harken::PlayableInstanceTable;
using harken::PlayableKind;
using harken::PostInput;
using harken::PostStatus;
using harken::PrePublishLatePolicy;
using harken::ProgramView;
using harken::ReserveStatus;
using harken::ScheduledEventInput;
using harken::ScheduledTransactionPool;
using harken::TimelinePlanner;
using harken::TransactionRequest;
using harken::TransportEpoch;
using harken::TransportEpochOwner;

namespace {

ScheduledTransactionPool::Config PoolConfig() {
    ScheduledTransactionPool::Config c;
    c.quantumFrames = 100;
    c.scheduleHorizonFrames = 400;
    c.publishLeadQuanta = 1;
    c.maxScheduledTransactions = 8;
    c.maxTransactionSpanFrames = 400;
    c.maxIngressTransactionsPerQuantum = 8;
    c.maxFrameBatchesPerQuantum = 8;
    c.maxEventsPerQuantum = 8;
    c.maxTransactionEvents = 8;
    return c;
}

// Reserves a one-event transaction owned by `owner`, stamped with `epoch`.
ReserveStatus ReserveOne(ScheduledTransactionPool& pool, TransportEpoch epoch,
                         PlayableInstanceHandle owner, std::uint64_t ownerEpoch,
                         std::uint64_t frame, std::uint64_t payload) {
    ScheduledEventInput e;
    e.executeFrame = frame;
    e.payload = payload;
    TransactionRequest req;
    req.transactionId = payload;
    req.transportEpoch = epoch;
    req.ownerInstanceHandle = owner;
    req.ownerInstanceScheduleEpoch = ownerEpoch;
    req.events = &e;
    req.eventCount = 1;
    return pool.Reserve(req, PrePublishLatePolicy::Refuse).status;
}

std::size_t ConsumeCount(ScheduledTransactionPool& pool, std::uint32_t page,
                         TransportEpoch epoch, const PlayableInstanceTable& inst,
                         const OccurrenceStatePool& occ) {
    std::array<ConsumedEvent, 32> buf{};
    return pool.ConsumePage(page, epoch, inst, occ, buf.data(), buf.size());
}

} // namespace

// The first epoch is 1, and raising walks 2, 3, 4 ... never touching the reserved
// zero.
TEST(TransportEpochOwner, MintsFromOneAndRaisesMonotonically) {
    TransportEpochOwner owner;
    EXPECT_EQ(owner.Current(), 1u);
    for (std::uint64_t expected = 2; expected <= 5; ++expected) {
        const auto r = owner.RaiseEpoch();
        EXPECT_EQ(r.status, EpochRaiseStatus::Raised);
        EXPECT_EQ(r.epoch, expected);
        EXPECT_EQ(owner.Current(), expected);
        EXPECT_NE(owner.Current(), 0u);
    }
}

// A resumed session starts at its persisted epoch; the reserved zero resumes as a
// fresh mint of 1.
TEST(TransportEpochOwner, SeededResumeUsesPersistedEpoch) {
    EXPECT_EQ(TransportEpochOwner(42).Current(), 42u);
    EXPECT_EQ(TransportEpochOwner(0).Current(), 1u); // 0 is not a real geometry
}

// At the wrap boundary the owner refuses and reports the terminal condition,
// leaving the current epoch untouched -- never reset to 1, never advanced into 0.
TEST(TransportEpochOwner, RaiseRefusesAtWrapWithHardFailure) {
    TransportEpochOwner atMax(UINT64_MAX);
    EXPECT_TRUE(atMax.Exhausted());
    const auto r = atMax.RaiseEpoch();
    EXPECT_EQ(r.status, EpochRaiseStatus::RuntimeRecreateRequired);
    EXPECT_EQ(r.epoch, UINT64_MAX);
    EXPECT_EQ(atMax.Current(), UINT64_MAX); // unchanged, not wrapped
    const auto repeated = atMax.RaiseEpoch();
    EXPECT_EQ(repeated.status, EpochRaiseStatus::RuntimeRecreateRequired);
    EXPECT_EQ(repeated.epoch, UINT64_MAX);
    EXPECT_EQ(atMax.Current(), UINT64_MAX);

    TransportEpochOwner nearMax(UINT64_MAX - 1);
    EXPECT_FALSE(nearMax.Exhausted());
    EXPECT_EQ(nearMax.RaiseEpoch().status, EpochRaiseStatus::Raised);
    EXPECT_EQ(nearMax.Current(), UINT64_MAX);
    EXPECT_EQ(nearMax.RaiseEpoch().status,
              EpochRaiseStatus::RuntimeRecreateRequired);
    EXPECT_EQ(nearMax.Current(), UINT64_MAX);
}

// After a raise, a consumer reading the pre-reset pool under the new epoch sees no
// old transaction: the existing epoch compare no-ops the whole batch in constant
// time, so no old-rate frame is executed. No new mechanism is involved.
TEST(TransportEpochOwner, RaiseInvalidatesOldTransactionsOnTheStalePath) {
    PlayableInstanceTable instances(8);
    OccurrenceStatePool occ(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Segment, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    TransportEpochOwner epoch;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, epoch.Current())); // epoch 1
    ASSERT_EQ(ReserveOne(pool, epoch.Current(), owner, ownerEpoch, 50, 0xA),
              ReserveStatus::Committed);
    EXPECT_EQ(ConsumeCount(pool, 0, epoch.Current(), instances, occ), 1u);

    // Raise the epoch; the pre-reset pool now holds a stale-geometry transaction.
    const auto raised = epoch.RaiseEpoch();
    ASSERT_EQ(raised.status, EpochRaiseStatus::Raised);
    EXPECT_EQ(ConsumeCount(pool, 0, epoch.Current(), instances, occ), 0u);
}

// The documented rebuild sequence: raise, reconstruct the deferred pool, reset the
// pool under the new epoch, re-bind the planner. The reset window holds only new
// work; the old transaction is gone.
TEST(TransportEpochOwner, RebuildSequenceReopensUnderNewEpoch) {
    PlayableInstanceTable instances(8);
    OccurrenceStatePool occ(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Segment, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    TransportEpochOwner epoch;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, epoch.Current()));
    ASSERT_EQ(ReserveOne(pool, epoch.Current(), owner, ownerEpoch, 50, 0xA),
              ReserveStatus::Committed);

    // 1. raise. 2. fresh deferred pool. 3. pool.Reset under the new epoch.
    // 4. re-bind the planner.
    ASSERT_EQ(epoch.RaiseEpoch().status, EpochRaiseStatus::Raised);
    DeferredIntentPool deferred(8, 0);
    ASSERT_TRUE(pool.Reset(PoolConfig(), 0, epoch.Current())); // discards old work
    TimelinePlanner planner;
    TimelinePlanner::Config pc;
    ASSERT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));

    // The old transaction is gone; a new one under the new epoch files and reads.
    // Reset publishes the lead frontier (one quantum = frame 100 here), so the new
    // play is placed at or past it to file without a late shift.
    const EventTemplate ev[] = {{0, 1, 0, 0, 0, 0xB}};
    ProgramView p;
    p.ownerInstance = owner;
    p.ownerInstanceScheduleEpoch = ownerEpoch;
    p.transactionId = 0xB;
    p.entryAnchorFrame = 150; // page 1, past the published frontier at 100
    p.events = ev;
    p.eventCount = 1;
    PostInput in;
    in.currentFrame = 0;
    in.transportEpoch = epoch.Current();
    in.latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
    EXPECT_EQ(planner.PostPlay(p, in).status, PostStatus::Reserved);

    std::array<ConsumedEvent, 32> buf{};
    const std::size_t n =
        pool.ConsumePage(1, epoch.Current(), instances, occ, buf.data(),
                         buf.size());
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(buf[0].payload, 0xBu); // the new work, not the discarded 0xA
}
