// The render-worker rendezvous deadline and what abandoning a batch leaves
// behind. What is pinned here:
//   1. A worker that stops making progress inside a claimed job makes the
//      rendezvous overrun its deadline, and the coordinator leaves rather than
//      waiting for it.
//   2. The set of jobs the abandonment names as unfinished is exactly the set
//      whose work never ran, and it is fixed at the moment of classification --
//      the stalled worker finishing later changes no answer.
//   3. A stalled executor strands exactly one job, not the rest of the batch.
//   4. Every wait the coordinator can be held in by a worker is bounded, not
//      just the ordinary completion barrier: the two that are only reachable
//      after a fault are bounded by the same deadline and end the same way.
//
// This target compiles the worker pool with the shipping wake strategy for the
// platform and adds only the test gates, which are friend declarations and a
// free-function namespace -- no member, so the object layout is the shipping
// one. That is what separates it from the forced-fallback target, whose wake
// primitive really does change the class.
//
// Determinism: the stall tests use a QPC budget with wide headroom for ordinary
// publication and notification. The coordinator waits until a worker is parked
// inside its claimed job; that gate then keeps the rendezvous open past the
// budget without relying on real job cost. The coordinator-overrun case uses the
// same shape, then keeps the coordinator inside its own job past the budget.
#include "harken/render/RenderWorkerPool.h"

#include "harken/core/CpuClock.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace wake_test = harken::render_worker_pool_test;

constexpr auto kQuiesceTimeout = 5s;
// The budget leaves enough time for thread notification and the first claim, then
// expires while the test gate keeps the worker inside its claimed job.
constexpr std::uint64_t kStallBudgetMicros = 50000;
constexpr std::uint64_t kCoordinatorBudgetMicros = 50000;
constexpr std::uint64_t kCoordinatorOverrunMicros = 100000;

std::uint64_t BudgetTicks(std::uint64_t micros) {
    const std::uint64_t ticksPerSecond = harken::CpuClockTicksPerSecond();
    if (ticksPerSecond == 0) {
        return 0;
    }
    const std::uint64_t ticks =
        (micros * ticksPerSecond) / 1000000ull;
    return ticks != 0 ? ticks : 1;
}

std::uint64_t StallBudgetTicks() {
    return BudgetTicks(kStallBudgetMicros);
}

std::uint64_t CoordinatorBudgetTicks() {
    return BudgetTicks(kCoordinatorBudgetMicros);
}

bool WaitUntil(const auto& predicate,
               std::chrono::steady_clock::duration timeout =
                   kQuiesceTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct StalledJobFault final {};

// One batch's observations. `ran` counts executions per job index, which is what
// makes the classification checkable from the outside: a job the pool reports
// unfinished must be one whose function never ran, and the other way round.
struct StallProbe {
    std::thread::id coordinator;
    std::uint32_t jobCount = 0;
    // Hold the coordinator in its first job until a worker is in the gate. Only
    // set for batches that armed the gate; without it the coordinator would wait
    // for a stall that is never going to arrive.
    bool awaitStall = false;
    bool coordinatorThrows = false;
    std::vector<std::uint32_t> ran;
    std::atomic<std::uint32_t> coordinatorJobs{0};
    std::atomic<std::uint32_t> totalRuns{0};

    static void Job(void* userData, std::uint32_t jobIndex) {
        auto* probe = static_cast<StallProbe*>(userData);
        const bool onCoordinator =
            std::this_thread::get_id() == probe->coordinator;
        const bool firstCoordinatorJob =
            onCoordinator && probe->coordinatorJobs.fetch_add(
                                 1, std::memory_order_acq_rel) == 0;
        if (firstCoordinatorJob && probe->awaitStall) {
            // The gate is entered after a worker has registered its claim, so a
            // nonzero count means one job is provably claimed and unfinished.
            while (wake_test::GateEnteredCount(
                       wake_test::TestGatePoint::BeforeJobFunction) == 0) {
                std::this_thread::yield();
            }
        }
        // The claim cursor hands each index out once, so a plain write is
        // sufficient -- and counting rather than flagging also catches a second
        // execution if that ever stopped being true.
        ++probe->ran[jobIndex];
        probe->totalRuns.fetch_add(1, std::memory_order_acq_rel);
        if (firstCoordinatorJob && probe->coordinatorThrows) {
            throw StalledJobFault{};
        }
    }
};

struct CoordinatorBudgetProbe final {
    std::thread::id coordinator;
    std::atomic<std::uint32_t> coordinatorJobs{0};
    std::atomic<std::uint32_t> workerJobs{0};

    static void Job(void* userData, std::uint32_t) {
        auto* probe = static_cast<CoordinatorBudgetProbe*>(userData);
        if (std::this_thread::get_id() != probe->coordinator) {
            probe->workerJobs.fetch_add(1, std::memory_order_release);
            return;
        }

        const bool firstCoordinatorJob =
            probe->coordinatorJobs.fetch_add(1, std::memory_order_acq_rel) ==
            0;
        if (!firstCoordinatorJob) {
            return;
        }

        // Keep the coordinator inside its job while the worker claims and
        // completes the other job. The initial budget covers publication and
        // the first claim, while the QPC interval below makes coordinator-owned
        // work unambiguously over-budget.
        while (wake_test::GateEnteredCount(
                   wake_test::TestGatePoint::BeforeJobFunction) == 0) {
            std::this_thread::yield();
        }
        wake_test::ReleaseGate(wake_test::TestGatePoint::BeforeJobFunction);
        while (probe->workerJobs.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        const std::uint64_t releaseTicks = harken::CpuClockTicks();
        const std::uint64_t overrunTicks = BudgetTicks(kCoordinatorOverrunMicros);
        while (harken::CpuClockTicks() - releaseTicks < overrunTicks) {
            std::this_thread::yield();
        }
    }
};

// Runs one batch that is guaranteed to strand at least one job. The gate is left
// armed and held on return: the caller inspects the classification while the
// stall is still in place, then releases.
harken::RenderWorkerPool::RunResult RunStalledBatch(
    harken::RenderWorkerPool& pool, StallProbe& probe,
    wake_test::TestGateOrdering ordering) {
    probe.coordinator = std::this_thread::get_id();
    probe.awaitStall = true;
    probe.ran.assign(probe.jobCount, 0);
    wake_test::ArmBeforeJobFunctionGate(1, ordering);
    pool.SetJobDeadlineTicksForTest(StallBudgetTicks());
    return pool.Run(probe.jobCount, &StallProbe::Job, &probe);
}

// Runs one batch with no stall and no budget.
harken::RenderWorkerPool::RunResult RunPlainBatch(
    harken::RenderWorkerPool& pool, StallProbe& probe) {
    probe.coordinator = std::this_thread::get_id();
    probe.awaitStall = false;
    probe.ran.assign(probe.jobCount, 0);
    pool.SetJobDeadlineTicksForTest(0);
    return pool.Run(probe.jobCount, &StallProbe::Job, &probe);
}

// Releases the stall and waits until every job has run exactly once, which is
// the point at which no worker is inside the abandoned batch any more and Stop
// cannot be held up by one.
void ReleaseAndQuiesce(StallProbe& probe) {
    wake_test::ReleaseGate(wake_test::TestGatePoint::BeforeJobFunction);
    ASSERT_TRUE(WaitUntil([&] {
        return probe.totalRuns.load(std::memory_order_acquire) ==
               probe.jobCount;
    })) << "a released worker never finished the job it was holding";
}

TEST(RenderWorkerDeadline, StalledWorkerMakesTheRendezvousOverrunAndAbandon) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(2, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 8;
    const harken::RenderWorkerPool::RunResult result =
        RunStalledBatch(pool, probe, wake_test::TestGateOrdering::Ordered);

    EXPECT_TRUE(result.abandoned)
        << "the coordinator waited out a worker that was not making progress";
    EXPECT_GE(result.unfinishedJobs, 1u);
    EXPECT_LE(result.unfinishedJobs, 2u)
        << "more jobs were stranded than there are executors to hold them";
    EXPECT_EQ(0u, result.busyPeakTicks)
        << "an abandoned batch reported a busy peak it could not have read";

    // The classification is exactly "whose work did not run". Read while the
    // stall is still held, so both sides describe the same instant.
    std::uint32_t unfinished = 0;
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        const bool complete = pool.JobCompletedForLastBatch(j);
        EXPECT_EQ(complete, probe.ran[j] != 0) << "job " << j;
        if (!complete) {
            ++unfinished;
        }
    }
    EXPECT_EQ(result.unfinishedJobs, unfinished)
        << "the reported count and the quarantine set disagree";

    ReleaseAndQuiesce(probe);
    pool.Stop();
}

TEST(RenderWorkerDeadline,
     ClassificationIsFixedAndDoesNotFollowTheStalledWorker) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 8;
    const harken::RenderWorkerPool::RunResult result =
        RunStalledBatch(pool, probe, wake_test::TestGateOrdering::Ordered);
    ASSERT_TRUE(result.abandoned);
    ASSERT_EQ(1u, result.unfinishedJobs);

    std::vector<bool> verdictWhileStalled(probe.jobCount, false);
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        verdictWhileStalled[j] = pool.JobCompletedForLastBatch(j);
    }

    // The stalled worker now runs to completion and marks its job done. A query
    // that re-read the progress word would begin calling that job finished, and
    // the count already handed to the caller -- who has by then put that voice
    // slot beyond reuse -- would stop matching the set it holds.
    ReleaseAndQuiesce(probe);
    std::uint32_t unfinished = 0;
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        const bool complete = pool.JobCompletedForLastBatch(j);
        EXPECT_EQ(verdictWhileStalled[j], complete)
            << "job " << j << " changed its verdict after the fact";
        if (!complete) {
            ++unfinished;
        }
    }
    EXPECT_EQ(result.unfinishedJobs, unfinished);

    pool.Stop();
}

TEST(RenderWorkerDeadline, AStalledExecutorStrandsExactlyOneJob) {
    // One worker, eight jobs. The worker is stopped inside the first job it
    // claims; the coordinator takes the rest. If a stranded worker could keep
    // claiming from the batch the coordinator left, the stranded set would grow
    // with the batch instead of staying at one job per executor.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 8;
    const harken::RenderWorkerPool::RunResult result =
        RunStalledBatch(pool, probe, wake_test::TestGateOrdering::Ordered);
    ASSERT_TRUE(result.abandoned);
    EXPECT_EQ(1u, result.unfinishedJobs)
        << "one stalled executor stranded more than one job";

    ReleaseAndQuiesce(probe);
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        EXPECT_EQ(1u, probe.ran[j]) << "job " << j << " did not run once";
    }
    pool.Stop();
}

TEST(RenderWorkerDeadline, UnorderedGateReachesTheSameVerdict) {
    // The same stall, with a gate that adds no release/acquire edge of its own,
    // so the result does not rest on synchronization the gate itself supplied.
    //
    // Platform limit, stated because it bounds what this proves: x86 is total-
    // store-ordered, so a plain store there already behaves as a release and a
    // missing release in the code under test very likely still passes. Removing
    // the gate's own edge removes the gate's bias; it does not make this a
    // memory-order proof. The discriminating platforms are the weakly ordered
    // ones, and every measurement behind this file is from x86.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 8;
    const harken::RenderWorkerPool::RunResult result =
        RunStalledBatch(pool, probe, wake_test::TestGateOrdering::Unordered);
    EXPECT_TRUE(result.abandoned);
    EXPECT_EQ(1u, result.unfinishedJobs);
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        EXPECT_EQ(pool.JobCompletedForLastBatch(j), probe.ran[j] != 0)
            << "job " << j;
    }

    ReleaseAndQuiesce(probe);
    pool.Stop();
}

TEST(RenderWorkerDeadline, WakeFaultDrainCompletesInsideTheWholeBudget) {
    // The wake-fault path stops waiting for the completion barrier and waits for
    // the job counter instead. A budget that survives publication leaves the
    // coordinator enough time to drain every job itself, so the batch completes
    // without quarantine while the worker set still enters serial fallback.
    //
    // A wake-fault drain held open by a worker remains unconstructible with these
    // gates: the fault is established during publication and workers normally
    // observe it before entering the batch. Its bound is therefore code-level;
    // this case pins only the reachable all-jobs-drained side of that path.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 8;
    probe.coordinator = std::this_thread::get_id();
    probe.awaitStall = false;
    probe.ran.assign(probe.jobCount, 0);
    pool.SetJobDeadlineTicksForTest(StallBudgetTicks());
    wake_test::FailNextWakeNotification();
    const harken::RenderWorkerPool::RunResult result =
        pool.Run(probe.jobCount, &StallProbe::Job, &probe);

    EXPECT_FALSE(result.abandoned)
        << "a drain with nothing outstanding abandoned the batch anyway";
    EXPECT_EQ(0u, result.unfinishedJobs);
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        EXPECT_EQ(1u, probe.ran[j]) << "job " << j;
    }
    EXPECT_TRUE(wake_test::IsSerialFallback(pool));
    pool.Stop();
}

TEST(RenderWorkerDeadline, JobFaultDrainWaitIsBoundedToo) {
    // Same for the drain the coordinator switches to when it observes a job
    // fault from inside the completion barrier. The coordinator's own first job
    // throws, so the fault is established on this thread rather than depending
    // on which worker got there first, and a worker is held in the batch at the
    // same time, so the counter being drained cannot reach the job count.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 8;
    probe.coordinatorThrows = true;
    const harken::RenderWorkerPool::RunResult result =
        RunStalledBatch(pool, probe, wake_test::TestGateOrdering::Ordered);

    EXPECT_TRUE(result.abandoned)
        << "the drain wait after a job fault was not bounded";
    EXPECT_EQ(1u, result.unfinishedJobs);
    EXPECT_EQ(1u, wake_test::JobFaultCount(pool));

    ReleaseAndQuiesce(probe);
    pool.Stop();
}

TEST(RenderWorkerDeadline, AbandonmentLeavesTheGenerationUnrunnable) {
    // After the coordinator walks away, the slot the new epoch selects must not
    // name runnable work: a worker that wakes reloads the epoch and switches on
    // that slot's command with no further check, so a command left there is a
    // command it will execute. This pins the invariant that clearing the command
    // before bumping the epoch exists to maintain.
    //
    // Honest limit: it does not currently discriminate the two orderings. Every
    // reachable predecessor of that slot has already cleared its own command -- a
    // completed batch and a completed control round both do, and every path that
    // returns without clearing also closes parallel dispatch for good -- so the
    // slot is already unrunnable and the bump-first ordering would pass this too.
    // It becomes discriminating as soon as some path can leave a live command in
    // a slot a later generation selects.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 4;
    const harken::RenderWorkerPool::RunResult result =
        RunStalledBatch(pool, probe, wake_test::TestGateOrdering::Ordered);
    ASSERT_TRUE(result.abandoned);
    ReleaseAndQuiesce(probe);

    // Surplus wakes are part of the protocol -- the epoch is the authority -- so
    // the released worker must absorb them without running anything. Nothing
    // beyond this batch's own jobs may execute.
    for (int i = 0; i < 4; ++i) {
        wake_test::NotifyWithoutEpochChange(pool);
        std::this_thread::yield();
    }
    EXPECT_EQ(probe.jobCount, probe.totalRuns.load(std::memory_order_acquire))
        << "a woken worker ran work from a generation nobody published";

    // And later blocks are serial: an abandoned set never dispatches again.
    EXPECT_TRUE(wake_test::IsSerialFallback(pool));
    EXPECT_EQ(0u, pool.WorkerCount());
    StallProbe serialProbe;
    serialProbe.jobCount = 4;
    const harken::RenderWorkerPool::RunResult serialResult =
        RunPlainBatch(pool, serialProbe);
    EXPECT_FALSE(serialResult.abandoned);
    for (std::uint32_t j = 0; j < serialProbe.jobCount; ++j) {
        EXPECT_EQ(1u, serialProbe.ran[j]) << "job " << j;
    }
    pool.Stop();
}

TEST(RenderWorkerDeadline, CompletedBatchesReportNoDeadlineDamage) {
    // The budget only ever fires on a batch that did not finish in time. A pool
    // running normally reports nothing abandoned, and a query about its jobs
    // answers "finished" instead of reading whatever an earlier abandonment might
    // have left in the classification storage.
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(2, 64));
    ASSERT_TRUE(pool.EnableOffline());

    StallProbe probe;
    probe.jobCount = 16;
    const harken::RenderWorkerPool::RunResult result =
        RunPlainBatch(pool, probe);
    EXPECT_FALSE(result.abandoned);
    EXPECT_EQ(0u, result.unfinishedJobs);
    EXPECT_EQ(0u, result.overrunTicks);
    for (std::uint32_t j = 0; j < probe.jobCount; ++j) {
        EXPECT_EQ(1u, probe.ran[j]) << "job " << j;
        EXPECT_TRUE(pool.JobCompletedForLastBatch(j)) << "job " << j;
    }
    pool.Stop();
}

TEST(RenderWorkerDeadline,
     CoordinatorJobOverBudgetAbandonsAfterWorkersFinish) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(1, 8));
    ASSERT_TRUE(pool.EnableOffline());

    CoordinatorBudgetProbe probe;
    probe.coordinator = std::this_thread::get_id();
    wake_test::ArmBeforeJobFunctionGate(1,
                                        wake_test::TestGateOrdering::Ordered);
    pool.SetJobDeadlineTicksForTest(CoordinatorBudgetTicks());
    const harken::RenderWorkerPool::RunResult result =
        pool.Run(2, &CoordinatorBudgetProbe::Job, &probe);

    EXPECT_EQ(1u, probe.coordinatorJobs.load(std::memory_order_acquire));
    EXPECT_EQ(1u, probe.workerJobs.load(std::memory_order_acquire));
    EXPECT_TRUE(result.abandoned)
        << "coordinator work that exceeded the budget was reported complete";
    EXPECT_GT(result.overrunTicks, 0u);
    EXPECT_EQ(0u, result.unfinishedJobs)
        << "completed coordinator and worker jobs were quarantined";
    EXPECT_TRUE(pool.JobCompletedForLastBatch(0));
    EXPECT_TRUE(pool.JobCompletedForLastBatch(1));
    pool.Stop();
}

} // namespace
