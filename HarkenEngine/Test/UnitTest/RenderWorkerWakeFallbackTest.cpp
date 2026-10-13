#include "harken/render/RenderWorkerPool.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||              \
    defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace {

using namespace std::chrono_literals;
namespace wake_test = harken::render_worker_pool_test;

constexpr auto kGateTimeout = 2s;
constexpr auto kProgressWindow = 200ms;
constexpr auto kRescueTimeout = 2s;

bool WaitUntil(const auto& predicate,
               std::chrono::steady_clock::duration timeout = kGateTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void NoOpJob(void*, std::uint32_t) {}

bool StartOffline(harken::RenderWorkerPool& pool,
                  std::uint32_t workerCount) {
    return pool.Start(workerCount, 256) && pool.EnableOffline();
}

struct RealtimeDomainProbe {
    static constexpr std::uint32_t kWorkerCount = 4;

    std::uint32_t failJoinNumber = 0;
    std::array<std::uint32_t, kWorkerCount> joinCount{};
    std::array<std::uint32_t, kWorkerCount> leaveCount{};
    std::array<std::uint32_t, kWorkerCount> failLeaveCount{};
    std::array<std::uint64_t, kWorkerCount> joinGeneration{};
    std::array<std::uint64_t, kWorkerCount> leaveGeneration{};
    std::array<std::thread::id, kWorkerCount> joinThread{};
    std::array<bool, kWorkerCount> membership{};
    std::array<bool, kWorkerCount> leaveRanOnJoinThread{
        true, true, true, true};

    static harken::RealtimeWorkerJoinResult Join(
        void* context, std::uint32_t workerId,
        std::uint64_t generation) noexcept {
        auto* probe = static_cast<RealtimeDomainProbe*>(context);
        if (probe == nullptr || workerId >= kWorkerCount || generation == 0 ||
            probe->membership[workerId]) {
            return harken::RealtimeWorkerJoinResult::FailedClean;
        }
        ++probe->joinCount[workerId];
        probe->joinGeneration[workerId] = generation;
        probe->joinThread[workerId] = std::this_thread::get_id();
        probe->membership[workerId] = true;
        if (probe->failJoinNumber == workerId + 1u) {
            // Model a platform call whose validation failed after it retained
            // per-thread membership. The pool must still run Leave here.
            return harken::RealtimeWorkerJoinResult::FailedNeedsLeave;
        }
        return harken::RealtimeWorkerJoinResult::Joined;
    }

    static bool Leave(void* context, std::uint32_t workerId,
                      std::uint64_t generation) noexcept {
        auto* probe = static_cast<RealtimeDomainProbe*>(context);
        if (probe == nullptr || workerId >= kWorkerCount || generation == 0 ||
            !probe->membership[workerId]) {
            return false;
        }
        ++probe->leaveCount[workerId];
        probe->leaveGeneration[workerId] = generation;
        probe->leaveRanOnJoinThread[workerId] =
            probe->leaveRanOnJoinThread[workerId] &&
            probe->joinThread[workerId] == std::this_thread::get_id();
        if (probe->failLeaveCount[workerId] != 0) {
            --probe->failLeaveCount[workerId];
            return false;
        }
        probe->membership[workerId] = false;
        return true;
    }
};

TEST(RenderWorkerDomain, StartFailureUnwindsPartialThreadSet) {
    harken::RenderWorkerPool failedPool;
    wake_test::FailNextStartBeforeWorker(2);
    EXPECT_FALSE(failedPool.Start(4, 256));
    EXPECT_EQ(0u, failedPool.WorkerCount());
    EXPECT_NO_THROW(failedPool.Stop());
    EXPECT_NO_THROW(failedPool.Stop());

    harken::RenderWorkerPool nextPool;
    ASSERT_TRUE(StartOffline(nextPool, 2));
    EXPECT_EQ(2u, nextPool.WorkerCount());
    nextPool.Stop();
}

TEST(RenderWorkerDomain, ZeroConfiguredCountRemainsLifecycleNoOp) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(0, 256));
    ASSERT_TRUE(pool.EnsureReady());
    ASSERT_TRUE(pool.EnableOffline());
    EXPECT_EQ(0u, pool.WorkerCount());
    pool.Stop();
    ASSERT_TRUE(pool.EnsureReady());
    EXPECT_TRUE(wake_test::HasCleanRestartState(pool));
    EXPECT_EQ(0u, pool.WorkerCount());
}

TEST(RenderWorkerDomain,
     BindStaysDisabledAndFailedJoinRollsBackBeforeRetry) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(RealtimeDomainProbe::kWorkerCount, 256));
    EXPECT_EQ(0u, pool.WorkerCount());

    RealtimeDomainProbe probe;
    probe.failJoinNumber = 3;
    harken::RealtimeWorkerDomain firstDomain{};
    firstDomain.ops.join = &RealtimeDomainProbe::Join;
    firstDomain.ops.leave = &RealtimeDomainProbe::Leave;
    firstDomain.context = &probe;
    firstDomain.generation = 41;

    EXPECT_FALSE(pool.BindRealtimeDomain(firstDomain));
    EXPECT_EQ(0u, pool.WorkerCount());
    for (std::uint32_t worker = 0;
         worker < RealtimeDomainProbe::kWorkerCount; ++worker) {
        EXPECT_EQ(1u, probe.joinCount[worker]) << "worker=" << worker;
        EXPECT_EQ(41u, probe.joinGeneration[worker]) << "worker=" << worker;
        EXPECT_FALSE(probe.membership[worker]) << "worker=" << worker;
        const std::uint32_t expectedLeaves = 1u;
        EXPECT_EQ(expectedLeaves, probe.leaveCount[worker])
            << "worker=" << worker;
        if (expectedLeaves != 0) {
            EXPECT_EQ(41u, probe.leaveGeneration[worker])
                << "worker=" << worker;
            EXPECT_TRUE(probe.leaveRanOnJoinThread[worker])
                << "worker=" << worker;
        }
    }

    probe.failJoinNumber = 0;
    harken::RealtimeWorkerDomain retryDomain = firstDomain;
    retryDomain.generation = 42;
    ASSERT_TRUE(pool.BindRealtimeDomain(retryDomain));
    EXPECT_EQ(0u, pool.WorkerCount());
    ASSERT_TRUE(pool.EnableRealtime());
    EXPECT_EQ(RealtimeDomainProbe::kWorkerCount, pool.WorkerCount());

    pool.Disable();
    EXPECT_EQ(0u, pool.WorkerCount());
    ASSERT_TRUE(pool.UnbindRealtimeDomain());
    for (std::uint32_t worker = 0;
         worker < RealtimeDomainProbe::kWorkerCount; ++worker) {
        EXPECT_EQ(2u, probe.joinCount[worker]) << "worker=" << worker;
        EXPECT_EQ(42u, probe.joinGeneration[worker]) << "worker=" << worker;
        EXPECT_EQ(2u, probe.leaveCount[worker])
            << "worker=" << worker;
        EXPECT_EQ(42u, probe.leaveGeneration[worker]) << "worker=" << worker;
        EXPECT_TRUE(probe.leaveRanOnJoinThread[worker])
            << "worker=" << worker;
        EXPECT_FALSE(probe.membership[worker]) << "worker=" << worker;
    }
    pool.Stop();
}

TEST(RenderWorkerDomain,
     FailedJoinRollbackLeaveFailureForceStopsBeforeReturning) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(RealtimeDomainProbe::kWorkerCount, 256));

    RealtimeDomainProbe probe;
    probe.failJoinNumber = 2;
    probe.failLeaveCount[1] = 1;
    harken::RealtimeWorkerDomain domain{};
    domain.ops.join = &RealtimeDomainProbe::Join;
    domain.ops.leave = &RealtimeDomainProbe::Leave;
    domain.context = &probe;
    domain.generation = 71;

    EXPECT_FALSE(pool.BindRealtimeDomain(domain));
    EXPECT_EQ(0u, pool.WorkerCount());
    EXPECT_FALSE(pool.EnableOffline());
    for (std::uint32_t worker = 0;
         worker < RealtimeDomainProbe::kWorkerCount; ++worker) {
        EXPECT_FALSE(probe.membership[worker]) << "worker=" << worker;
        EXPECT_TRUE(probe.leaveRanOnJoinThread[worker])
            << "worker=" << worker;
        EXPECT_EQ(worker == 1 ? 2u : 1u, probe.leaveCount[worker])
            << "worker=" << worker;
    }

    ASSERT_TRUE(pool.EnsureReady());
    EXPECT_TRUE(wake_test::HasCleanRestartState(pool));
    probe.failJoinNumber = 0;
    harken::RealtimeWorkerDomain retryDomain = domain;
    retryDomain.generation = 72;
    ASSERT_TRUE(pool.BindRealtimeDomain(retryDomain));
    ASSERT_TRUE(pool.EnableRealtime());
    EXPECT_EQ(RealtimeDomainProbe::kWorkerCount, pool.WorkerCount());
    pool.Disable();
    ASSERT_TRUE(pool.UnbindRealtimeDomain());
    EXPECT_NO_THROW(pool.Stop());
}

TEST(RenderWorkerDomain,
     RestartCreationFailureUnwindsAndLaterRetryUsesFrozenCount) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(pool.Start(RealtimeDomainProbe::kWorkerCount, 256));
    pool.Stop();

    wake_test::FailNextStartBeforeWorker(2);
    EXPECT_FALSE(pool.EnsureReady());
    EXPECT_EQ(0u, pool.WorkerCount());
    EXPECT_FALSE(pool.EnableOffline());

    ASSERT_TRUE(pool.EnsureReady());
    EXPECT_TRUE(wake_test::HasCleanRestartState(pool));
    ASSERT_TRUE(pool.EnableOffline());
    EXPECT_EQ(RealtimeDomainProbe::kWorkerCount, pool.WorkerCount());

    std::array<std::atomic<std::uint32_t>, 8> counts{};
    pool.Run(
        static_cast<std::uint32_t>(counts.size()),
        [](void* data, std::uint32_t job) {
            auto* jobCounts = static_cast<
                std::array<std::atomic<std::uint32_t>, 8>*>(data);
            (*jobCounts)[job].fetch_add(1, std::memory_order_relaxed);
        },
        &counts);
    for (const auto& count : counts) {
        EXPECT_EQ(1u, count.load(std::memory_order_relaxed));
    }
    pool.Stop();
}

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||              \
    defined(__x86_64__)

constexpr unsigned int kDenormalModeMask =
    _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;

class MxcsrRestore final {
public:
    explicit MxcsrRestore(unsigned int value) noexcept : m_value(value) {}
    ~MxcsrRestore() { _mm_setcsr(m_value); }

    MxcsrRestore(const MxcsrRestore&) = delete;
    MxcsrRestore& operator=(const MxcsrRestore&) = delete;

private:
    unsigned int m_value;
};

struct WorkerFloatModeProbe {
    std::thread::id coordinator;
    std::atomic<std::uint32_t> started{0};
    std::atomic<bool> workerObserved{false};
    std::atomic<bool> workersEnabled{true};
    std::uint32_t jobCount = 0;

    static void Job(void* userData, std::uint32_t /*jobIndex*/) {
        auto* probe = static_cast<WorkerFloatModeProbe*>(userData);
        if (std::this_thread::get_id() != probe->coordinator) {
            probe->workerObserved.store(true, std::memory_order_relaxed);
            if ((_mm_getcsr() & kDenormalModeMask) != kDenormalModeMask) {
                probe->workersEnabled.store(false, std::memory_order_relaxed);
            }
        }
        probe->started.fetch_add(1, std::memory_order_acq_rel);
        while (probe->started.load(std::memory_order_acquire) <
               probe->jobCount) {
            std::this_thread::yield();
        }
    }
};

#endif

template <typename Future>
bool RescueRunIfNeeded(Future& run, harken::RenderWorkerPool& pool) {
    const bool completed = run.wait_for(kProgressWindow) ==
                           std::future_status::ready;
    if (!completed) {
        wake_test::NotifyWithoutEpochChange(pool);
    }
    EXPECT_EQ(std::future_status::ready, run.wait_for(kRescueTimeout));
    if (run.wait_for(0s) == std::future_status::ready) {
        run.get();
    }
    return completed;
}

template <typename Future>
bool RescueStopIfNeeded(Future& stop, harken::RenderWorkerPool& pool) {
    const bool completed = stop.wait_for(kProgressWindow) ==
                           std::future_status::ready;
    if (!completed) {
        wake_test::NotifyWithoutEpochChange(pool);
    }
    EXPECT_EQ(std::future_status::ready, stop.wait_for(kRescueTimeout));
    if (stop.wait_for(0s) == std::future_status::ready) {
        stop.get();
    }
    return completed;
}

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||              \
    defined(__x86_64__)

TEST(RenderWorkerWakeFallback, WorkerJobsRunWithFtzAndDazEnabled) {
    const unsigned int originalMxcsr = _mm_getcsr();
    MxcsrRestore restore(originalMxcsr);
    const unsigned int callerMxcsr = originalMxcsr & ~kDenormalModeMask;
    _mm_setcsr(callerMxcsr);

    harken::RenderWorkerPool pool;
    ASSERT_TRUE(StartOffline(pool, 4));
    ASSERT_EQ(4u, pool.WorkerCount());

    WorkerFloatModeProbe probe;
    probe.coordinator = std::this_thread::get_id();
    probe.jobCount = 4;
    pool.Run(probe.jobCount, &WorkerFloatModeProbe::Job, &probe);

    EXPECT_TRUE(probe.workerObserved.load(std::memory_order_relaxed));
    EXPECT_TRUE(probe.workersEnabled.load(std::memory_order_relaxed));
    EXPECT_EQ(callerMxcsr, _mm_getcsr());
    pool.Stop();
}

#endif

TEST(RenderWorkerWakeFallback, NotifyBeforeWaitIsRemembered) {
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        harken::RenderWorkerPool pool;
        wake_test::ArmBeforeFirstEpochLoadGate(workers);
        ASSERT_TRUE(StartOffline(pool, workers));
        ASSERT_TRUE(WaitUntil([&] {
            return wake_test::GateEnteredCount(
                       wake_test::TestGatePoint::BeforeFirstEpochLoad) ==
                   workers;
        }));

        const auto notifications = wake_test::NotificationCount();
        auto run = std::async(std::launch::async, [&] {
            pool.Run(0, &NoOpJob, nullptr);
        });
        ASSERT_TRUE(WaitUntil([&] {
            return wake_test::NotificationCount() > notifications;
        }));
        wake_test::ReleaseGate(
            wake_test::TestGatePoint::BeforeFirstEpochLoad);

        EXPECT_EQ(std::future_status::ready, run.wait_for(kProgressWindow));
        if (run.wait_for(0s) != std::future_status::ready) {
            wake_test::NotifyWithoutEpochChange(pool);
        }
        ASSERT_EQ(std::future_status::ready, run.wait_for(kRescueTimeout));
        run.get();
        pool.Stop();
    }
}

TEST(RenderWorkerWakeFallback, PredicateWindowRetainsWakeToken) {
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        harken::RenderWorkerPool pool;
        wake_test::ArmBeforeWaitGate(workers);
        ASSERT_TRUE(StartOffline(pool, workers));
        ASSERT_TRUE(WaitUntil([&] {
            return wake_test::GateEnteredCount(
                       wake_test::TestGatePoint::BeforeWait) == workers;
        }));

        const auto notifications = wake_test::NotificationCount();
        auto run = std::async(std::launch::async, [&] {
            pool.Run(0, &NoOpJob, nullptr);
        });
        ASSERT_TRUE(WaitUntil([&] {
            return wake_test::NotificationCount() > notifications;
        }));
        wake_test::ReleaseGate(wake_test::TestGatePoint::BeforeWait);

        const bool progressedWithoutRescue = RescueRunIfNeeded(run, pool);
        pool.Stop();
        EXPECT_TRUE(progressedWithoutRescue)
            << "no progress in the bounded predicate-to-sleep window";
    }
}

TEST(RenderWorkerWakeFallback, StopWindowRetainsWakeToken) {
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        harken::RenderWorkerPool pool;
        wake_test::ArmBeforeWaitGate(workers);
        ASSERT_TRUE(StartOffline(pool, workers));
        ASSERT_TRUE(WaitUntil([&] {
            return wake_test::GateEnteredCount(
                       wake_test::TestGatePoint::BeforeWait) == workers;
        }));

        const auto notifications = wake_test::NotificationCount();
        auto stop = std::async(std::launch::async, [&] { pool.Stop(); });
        ASSERT_TRUE(WaitUntil([&] {
            return wake_test::NotificationCount() > notifications;
        }));
        wake_test::ReleaseGate(wake_test::TestGatePoint::BeforeWait);

        const bool progressedWithoutRescue = RescueStopIfNeeded(stop, pool);
        EXPECT_TRUE(progressedWithoutRescue)
            << "Stop made no progress in the bounded predicate-to-sleep window";
    }
}

TEST(RenderWorkerWakeFallback, StopCompletesWhenWakeSignalIsWithheld) {
    harken::RenderWorkerPool pool;
    wake_test::ArmBeforeWaitGate(1);
    ASSERT_TRUE(StartOffline(pool, 1));
    const bool workerReachedWait = WaitUntil([&] {
        return wake_test::GateEnteredCount(
                   wake_test::TestGatePoint::BeforeWait) == 1;
    });
    if (!workerReachedWait) {
        wake_test::ReleaseGate(wake_test::TestGatePoint::BeforeWait);
        pool.Stop();
    }
    ASSERT_TRUE(workerReachedWait);

    const auto notifications = wake_test::NotificationCount();
    wake_test::WithholdNextWakeNotificationForWorker(0);
    auto stop = std::async(std::launch::async, [&] { pool.Stop(); });
    const bool stopNotificationObserved = WaitUntil([&] {
        return wake_test::NotificationCount() > notifications;
    });
    wake_test::ReleaseGate(wake_test::TestGatePoint::BeforeWait);

    ASSERT_TRUE(stopNotificationObserved);
    EXPECT_EQ(std::future_status::ready, stop.wait_for(kProgressWindow));
    if (stop.wait_for(0s) == std::future_status::ready) {
        stop.get();
        EXPECT_EQ(0u, pool.WorkerCount());
    }
}

TEST(RenderWorkerWakeFallback, RunThenStopCyclesCompleteAcrossWorkerCounts) {
    constexpr std::uint32_t kCycles = 200;

    for (std::uint32_t workers : {1u, 2u, 4u}) {
        for (std::uint32_t cycle = 0; cycle < kCycles; ++cycle) {
            SCOPED_TRACE(::testing::Message()
                         << "workers=" << workers << " cycle=" << cycle);

            harken::RenderWorkerPool pool;
            ASSERT_TRUE(StartOffline(pool, workers));
            for (std::uint32_t jobCount : {0u, 1u, 7u}) {
                std::vector<std::atomic<std::uint32_t>> counts(jobCount);
                for (auto& count : counts) {
                    count.store(0, std::memory_order_relaxed);
                }
                pool.Run(
                    jobCount,
                    [](void* data, std::uint32_t job) {
                        auto* counts = static_cast<
                            std::vector<std::atomic<std::uint32_t>>*>(data);
                        (*counts)[job].fetch_add(1,
                                                std::memory_order_relaxed);
                    },
                    &counts);
                for (std::uint32_t job = 0; job < jobCount; ++job) {
                    EXPECT_EQ(
                        1u, counts[job].load(std::memory_order_relaxed))
                        << "jobCount=" << jobCount << " job=" << job;
                }
            }
            pool.Stop();
        }
    }
}

TEST(RenderWorkerWakeFallback, EmptySingleAndMultiJobBatchesAreExact) {
    for (std::uint32_t workers : {1u, 2u, 4u}) {
        harken::RenderWorkerPool pool;
        ASSERT_TRUE(StartOffline(pool, workers));
        for (std::uint32_t jobCount : {0u, 1u, 3u, 64u}) {
            std::vector<std::atomic<std::uint32_t>> counts(jobCount);
            for (auto& count : counts) {
                count.store(0, std::memory_order_relaxed);
            }
            pool.Run(
                jobCount,
                [](void* data, std::uint32_t job) {
                    auto* counts =
                        static_cast<std::vector<std::atomic<std::uint32_t>>*>(
                            data);
                    (*counts)[job].fetch_add(1, std::memory_order_relaxed);
                },
                &counts);
            for (std::uint32_t job = 0; job < jobCount; ++job) {
                EXPECT_EQ(1u, counts[job].load(std::memory_order_relaxed))
                    << "workers=" << workers << " job=" << job;
            }
        }
        pool.Stop();
    }
}

TEST(RenderWorkerWakeFallback,
     WakeFaultFallsBackToSerialUntilLifecycleRecovery) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(StartOffline(pool, 4));

    std::vector<std::atomic<std::uint32_t>> counts(64);
    for (auto& count : counts) {
        count.store(0, std::memory_order_relaxed);
    }
    wake_test::FailNextWakeNotification();
    pool.Run(
        static_cast<std::uint32_t>(counts.size()),
        [](void* data, std::uint32_t job) {
            auto* counts =
                static_cast<std::vector<std::atomic<std::uint32_t>>*>(data);
            (*counts)[job].fetch_add(1, std::memory_order_relaxed);
        },
        &counts);
    for (std::uint32_t job = 0; job < counts.size(); ++job) {
        EXPECT_EQ(1u, counts[job].load(std::memory_order_relaxed));
    }
    ASSERT_TRUE(wake_test::IsSerialFallback(pool));

    struct ThreadProbe {
        std::thread::id coordinator;
        std::atomic<bool> workerObserved{false};
    } probe{std::this_thread::get_id()};
    pool.Run(
        64,
        [](void* data, std::uint32_t) {
            auto* probe = static_cast<ThreadProbe*>(data);
            if (std::this_thread::get_id() != probe->coordinator) {
                probe->workerObserved.store(true, std::memory_order_relaxed);
            }
        },
        &probe);
    EXPECT_FALSE(probe.workerObserved.load(std::memory_order_relaxed));
    pool.Stop();
}

namespace {

struct ThrowingJobState {
    std::thread::id coordinator;
    std::atomic<bool> coordinatorEntered{false};
    std::atomic<bool> workerEntered{false};
    std::atomic<bool> workerThrew{false};
    std::array<std::atomic<std::uint32_t>, 64> counts;
};

struct ThrowingJob final {};

void ThrowOnceOnWorker(void* userData, std::uint32_t job) {
    auto* state = static_cast<ThrowingJobState*>(userData);
    state->counts[job].fetch_add(1, std::memory_order_relaxed);
    if (std::this_thread::get_id() == state->coordinator) {
        if (!state->coordinatorEntered.exchange(true,
                                                std::memory_order_acq_rel)) {
            while (!state->workerEntered.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return;
    }
    state->workerEntered.store(true, std::memory_order_release);
    if (!state->workerThrew.exchange(true, std::memory_order_acq_rel)) {
        throw ThrowingJob{};
    }
}

} // namespace

TEST(RenderWorkerWakeFallback, ThrowingWorkerJobIsContainedAndDegrades) {
    harken::RenderWorkerPool pool;
    ASSERT_TRUE(StartOffline(pool, 4));

    ThrowingJobState state;
    state.coordinator = std::this_thread::get_id();
    for (auto& count : state.counts) {
        count.store(0, std::memory_order_relaxed);
    }

    EXPECT_NO_THROW(pool.Run(static_cast<std::uint32_t>(state.counts.size()),
                             &ThrowOnceOnWorker, &state));
    ASSERT_TRUE(state.workerThrew.load(std::memory_order_acquire));
    EXPECT_EQ(1u, wake_test::JobFaultCount(pool));
    for (std::uint32_t job = 0;
         job < static_cast<std::uint32_t>(state.counts.size()); ++job) {
        EXPECT_EQ(1u, state.counts[job].load(std::memory_order_relaxed))
            << "job=" << job;
    }

    struct SerialProbe {
        std::thread::id coordinator;
        std::atomic<bool> workerObserved{false};
        std::atomic<std::uint32_t> completed{0};
    } probe{std::this_thread::get_id()};
    EXPECT_NO_THROW(pool.Run(
        16,
        [](void* userData, std::uint32_t) {
            auto* p = static_cast<SerialProbe*>(userData);
            if (std::this_thread::get_id() != p->coordinator) {
                p->workerObserved.store(true, std::memory_order_relaxed);
            }
            p->completed.fetch_add(1, std::memory_order_relaxed);
        },
        &probe));
    EXPECT_EQ(16u, probe.completed.load(std::memory_order_relaxed));
    EXPECT_FALSE(probe.workerObserved.load(std::memory_order_relaxed));
    ASSERT_TRUE(wake_test::IsSerialFallback(pool));
    EXPECT_NO_THROW(pool.Stop());

    ASSERT_TRUE(pool.EnsureReady());
    EXPECT_TRUE(wake_test::HasCleanRestartState(pool));
    EXPECT_EQ(1u, wake_test::JobFaultCount(pool));
    ASSERT_TRUE(pool.EnableOffline());
    EXPECT_EQ(4u, pool.WorkerCount());
    EXPECT_NO_THROW(pool.Run(8, &NoOpJob, nullptr));
    pool.Stop();
}

TEST(RenderWorkerWakeFallback, HundredThousandHandshakesPerWorkerCount) {
    constexpr std::uint32_t kHandshakes = 100000;
    constexpr auto kStressWindow = 30s;

    for (std::uint32_t workers : {1u, 2u, 4u}) {
        harken::RenderWorkerPool pool;
        ASSERT_TRUE(StartOffline(pool, workers));
        std::atomic<std::uint32_t> completed{0};
        auto stress = std::async(std::launch::async, [&] {
            for (std::uint32_t i = 0; i < kHandshakes; ++i) {
                pool.Run(
                    1,
                    [](void* data, std::uint32_t) {
                        static_cast<std::atomic<std::uint32_t>*>(data)->fetch_add(
                            1, std::memory_order_relaxed);
                    },
                    &completed);
            }
        });

        const bool completedInWindow =
            stress.wait_for(kStressWindow) == std::future_status::ready;
        while (stress.wait_for(0s) != std::future_status::ready) {
            wake_test::NotifyWithoutEpochChange(pool);
            std::this_thread::yield();
        }
        stress.get();
        pool.Stop();

        EXPECT_TRUE(completedInWindow)
            << "handshake stress made no progress within its bounded window";
        EXPECT_EQ(kHandshakes, completed.load(std::memory_order_relaxed));
    }
}

} // namespace
